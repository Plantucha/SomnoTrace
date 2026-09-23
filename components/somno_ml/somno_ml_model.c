/*
 * SomnoTrace - SomnoStage packed-model parser, inference and decode
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * SomnoTrace is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */

/* C11 port of the SomnoStage v4.1 inference path:
 *   score_night.py = extract features -> 3 boosters -> apply_temperature ->
 *   _binary_probs/_multiclass_probs -> soft_hierarchical_probs ->
 *   fuse_probabilities -> prior_correction -> decode_probabilities
 *   (REM-latency prior + forward-backward, argmax labels).
 * Tree evaluation follows the LightGBM text model: numerical "<=" splits,
 * decision_type bitfield (bit1 default_left, bits2-3 missing_type:
 * 0=none,1=zero,2=nan), child <0 encodes leaf (-v-1). boost_from_average
 * bias is already folded into stored leaf values. */

#include "somno_ml_model.h"
#include "somno_ml_features.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define SOMNO_MALLOC(sz) heap_caps_malloc((sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define SOMNO_CALLOC(n, sz) heap_caps_calloc((n), (sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define SOMNO_FREE   heap_caps_free
#else
#define SOMNO_MALLOC(sz) malloc(sz)
#define SOMNO_CALLOC(n, sz) calloc((n), (sz))
#define SOMNO_FREE   free
#endif

#define SSTP_VERSION 1
#define N_CLASSES    4

/* ---- packed SSTP wire structs (little-endian; ESP32-S3 and x86 hosts) ---- */

typedef struct {
    uint8_t  feat;
    uint8_t  dtype;
    int8_t   left;
    int8_t   right;
    float    th;
} sst_node_t;   /* 8 bytes, identical to wire layout */

typedef struct {
    const sst_node_t *nodes;   /* num_leaves-1 entries */
    const float      *leaves;  /* num_leaves entries */
} sst_tree_t;

typedef struct {
    uint8_t    num_class;
    uint8_t    ntpi;
    uint16_t   n_trees;
    sst_tree_t *trees;
} sst_booster_t;

typedef struct {
    double   temp[3];          /* direct, wake_sleep, sleep_stage */
    double   alpha_direct;
    int      prior_enabled;
    double   prior_w[4];
    double   transition[16];
    double   initial_prior[4];
    int      rem_enabled;
    uint16_t rem_min_epochs;
    double   rem_strength;
    uint16_t min_deep_epochs;
} sst_params_t;

struct somno_model {
    sst_booster_t booster[3];
    sst_params_t  par;
    char          semver[33];
    uint32_t      feature_fnv;
    int           n_features;
    sst_tree_t   *tree_pool;   /* single allocation for all trees */
};

/* ---- little-endian readers ---- */

static uint16_t rd_u16(const uint8_t **p) { uint16_t v; memcpy(&v, *p, 2); *p += 2; return v; }
static uint32_t rd_u32(const uint8_t **p) { uint32_t v; memcpy(&v, *p, 4); *p += 4; return v; }
static double   rd_f64(const uint8_t **p) { double v; memcpy(&v, *p, 8); *p += 8; return v; }

somno_model_t *somno_ml_model_load(const void *blob, size_t len)
{
    const uint8_t *p = blob;
    const uint8_t *end = p + len;
    if (len < 16 || memcmp(p, "SSTP", 4) != 0)
        return NULL;
    p += 4;
    uint16_t ver = rd_u16(&p);
    uint16_t n_feat = rd_u16(&p);
    uint32_t fnv = rd_u32(&p);
    uint8_t n_boosters = *p;
    p += 8; /* n_boosters + 7 reserved */
    if (ver != SSTP_VERSION || n_boosters != 3)
        return NULL;

    somno_model_t *m = SOMNO_CALLOC(1, sizeof(*m));
    if (!m) return NULL;
    m->n_features = n_feat;
    m->feature_fnv = fnv;

    uint32_t total_trees = 0;
    /* first pass over booster headers to size the tree pool */
    const uint8_t *q = p;
    for (int b = 0; b < 3; b++) {
        if (q + 12 > end) goto fail;
        uint16_t nt;
        memcpy(&nt, q + 2, 2);
        total_trees += nt;
        /* skip header then each tree body */
        q += 12;
        for (int t = 0; t < nt; t++) {
            uint16_t nl;
            memcpy(&nl, q, 2);
            q += 2 + (size_t)(nl - 1) * sizeof(sst_node_t) + (size_t)nl * 4;
        }
    }
    if (q > end) goto fail;
    m->tree_pool = SOMNO_CALLOC(total_trees, sizeof(sst_tree_t));
    if (!m->tree_pool) goto fail;

    sst_tree_t *tp = m->tree_pool;
    for (int b = 0; b < 3; b++) {
        sst_booster_t *bo = &m->booster[b];
        bo->num_class = *p++;
        bo->ntpi = *p++;
        bo->n_trees = rd_u16(&p);
        p += 8; /* n_internal, n_leaves totals */
        bo->trees = tp;
        tp += bo->n_trees;
        for (int t = 0; t < bo->n_trees; t++) {
            uint16_t nl = rd_u16(&p);
            if (nl < 1 || nl > 127 || p + (size_t)(nl - 1) * sizeof(sst_node_t) + (size_t)nl * 4 > end)
                goto fail;
            bo->trees[t].nodes = (const sst_node_t *)p;
            p += (size_t)(nl - 1) * sizeof(sst_node_t);
            bo->trees[t].leaves = (const float *)p;
            p += (size_t)nl * 4;
        }
        if (bo->ntpi != bo->num_class && !(bo->num_class == 1 && bo->ntpi == 1))
            goto fail;
        if (bo->num_class == 1) {
            /* binary: single score stream */
        } else if (bo->num_class < 2 || bo->num_class > 4) {
            goto fail;
        }
    }

    if (end - p < 8 * 3 + 8 + 8 + 32 + 128 + 32 + 12 + 64)
        goto fail;
    for (int i = 0; i < 3; i++) m->par.temp[i] = rd_f64(&p);
    m->par.alpha_direct = rd_f64(&p);
    m->par.prior_enabled = *p ? 1 : 0;
    p += 8;
    for (int i = 0; i < 4; i++) m->par.prior_w[i] = rd_f64(&p);
    for (int i = 0; i < 16; i++) m->par.transition[i] = rd_f64(&p);
    for (int i = 0; i < 4; i++) m->par.initial_prior[i] = rd_f64(&p);
    m->par.rem_enabled = *p ? 1 : 0;
    p += 2;
    m->par.rem_min_epochs = rd_u16(&p);
    m->par.rem_strength = rd_f64(&p);
    m->par.min_deep_epochs = rd_u16(&p);
    p += 2;
    memcpy(m->semver, p, 32);
    m->semver[32] = '\0';
    return m;
fail:
    somno_ml_model_destroy(m);
    return NULL;
}

void somno_ml_model_destroy(somno_model_t *m)
{
    if (!m) return;
    SOMNO_FREE(m->tree_pool);
    SOMNO_FREE(m);
}

const char *somno_ml_model_semver_of(const somno_model_t *m) { return m ? m->semver : "-"; }
uint32_t somno_ml_model_feature_fnv(const somno_model_t *m) { return m ? m->feature_fnv : 0; }
int somno_ml_model_n_features(const somno_model_t *m) { return m ? m->n_features : 0; }

/* ---- tree evaluation ---- */

static inline double tree_eval(const sst_tree_t *t, const float *f)
{
    int i = 0;
    for (;;) {
        const sst_node_t *n = &t->nodes[i];
        double fv = f[n->feat];
        int go_left;
        if (isnan(fv)) {
            int missing_type = (n->dtype >> 2) & 3;
            if (missing_type == 2) {          /* NaN: route via default_left */
                go_left = (n->dtype & 0x02) != 0;
            } else {                          /* none/zero: NaN acts as 0.0 */
                go_left = (0.0 <= (double)n->th);
            }
        } else {
            go_left = (fv <= (double)n->th);
        }
        int c = go_left ? n->left : n->right;
        if (c < 0)
            return (double)t->leaves[-c - 1];
        i = c;
    }
}

static void booster_raw(const sst_booster_t *b, const float *f, double *raw)
{
    for (int c = 0; c < N_CLASSES; c++) raw[c] = 0.0;
    for (int t = 0; t < b->n_trees; t++)
        raw[t % b->ntpi] += tree_eval(&b->trees[t], f);
}

static void softmax4(const double *z, double *out, int n)
{
    double mx = z[0];
    for (int i = 1; i < n; i++) if (z[i] > mx) mx = z[i];
    double s = 0;
    for (int i = 0; i < n; i++) { out[i] = exp(z[i] - mx); s += out[i]; }
    for (int i = 0; i < n; i++) out[i] /= s;
}

/* apply_temperature: softmax(log(clip(p))/T) */
static void apply_temp(double *p, int n, double temp)
{
    double z[8];
    for (int i = 0; i < n; i++) {
        double c = p[i] < 1e-12 ? 1e-12 : (p[i] > 1.0 ? 1.0 : p[i]);
        z[i] = log(c) / temp;
    }
    softmax4(z, p, n);
}

void somno_ml_predict(const somno_model_t *m, const float *feats, double out4[4])
{
    double raw[N_CLASSES], p_direct[N_CLASSES], p_hier[N_CLASSES];

    /* direct 4-class: raw scores -> softmax (LightGBM predict) -> temperature */
    booster_raw(&m->booster[0], feats, raw);
    softmax4(raw, p_direct, 4);
    apply_temp(p_direct, 4, m->par.temp[0]);

    /* wake/sleep binary: sigmoid -> [1-p, p] -> temperature */
    booster_raw(&m->booster[1], feats, raw);
    double p_sleep = 1.0 / (1.0 + exp(-raw[0]));
    double pa[2] = {1.0 - p_sleep, p_sleep};
    apply_temp(pa, 2, m->par.temp[1]);

    /* sleep-stage 3-class */
    booster_raw(&m->booster[2], feats, raw);
    double pb[4];
    softmax4(raw, pb, 3);
    apply_temp(pb, 3, m->par.temp[2]);

    /* soft hierarchical: [pa0, pa1*pb0, pa1*pb1, pa1*pb2] */
    p_hier[0] = pa[0];
    p_hier[1] = pa[1] * pb[0];
    p_hier[2] = pa[1] * pb[1];
    p_hier[3] = pa[1] * pb[2];

    /* convex fusion + normalize */
    double a = m->par.alpha_direct, s = 0;
    for (int i = 0; i < 4; i++) {
        out4[i] = a * p_direct[i] + (1.0 - a) * p_hier[i];
        s += out4[i];
    }
    for (int i = 0; i < 4; i++) out4[i] /= s;

    /* prior correction: multiply WITHOUT renormalizing — the reference feeds
     * the unnormalized product to decode_probabilities (REM-onset threshold
     * applies to it); display probs are the caller's renormalized copy. */
    if (m->par.prior_enabled) {
        for (int i = 0; i < 4; i++) out4[i] *= m->par.prior_w[i];
    }
}

/* ---- decode: REM-latency prior + forward-backward (single night) ---- */

static void norm_rows(double *A)
{
    for (int r = 0; r < 4; r++) {
        double s = 0;
        for (int c = 0; c < 4; c++) s += A[r * 4 + c];
        if (s < 1e-12) s = 1e-12;
        for (int c = 0; c < 4; c++) A[r * 4 + c] /= s;
    }
}

int somno_ml_decode_night(const somno_model_t *m, const double *probs, int n,
                          uint8_t *stages, double *posterior)
{
    if (n <= 0) return -1;
    double *p = SOMNO_MALLOC((size_t)n * 4 * sizeof(double));
    double *alpha = SOMNO_MALLOC((size_t)n * 4 * sizeof(double));
    double *beta = SOMNO_MALLOC((size_t)n * 4 * sizeof(double));
    if (!p || !alpha || !beta) {
        SOMNO_FREE(p); SOMNO_FREE(alpha); SOMNO_FREE(beta); return -1;
    }
    memcpy(p, probs, (size_t)n * 4 * sizeof(double));

    /* REM-latency prior */
    if (m->par.rem_enabled) {
        int onset = -1;
        for (int e = 0; e < n; e++) {
            double sp = p[e * 4 + 1] + p[e * 4 + 2] + p[e * 4 + 3];
            if (sp >= 0.5) { onset = e; break; }
        }
        if (onset < 0) {
            for (int e = 0; e < n; e++)
                if (p[e * 4 + 3] > 1e-8) p[e * 4 + 3] = 1e-8;
        } else {
            for (int e = 0; e < n; e++) {
                if (e < onset) {
                    p[e * 4 + 3] = 1e-8;
                } else if (e < onset + (int)m->par.rem_min_epochs) {
                    double ramp = (e - onset + 1) / (double)m->par.rem_min_epochs;
                    double f = pow(ramp, m->par.rem_strength);
                    if (f < 1e-8) f = 1e-8;
                    p[e * 4 + 3] *= f;
                }
            }
        }
    }

    /* clip to [1e-12, 1] like forward_backward_posteriors */
    for (int i = 0; i < n * 4; i++) {
        if (p[i] < 1e-12) p[i] = 1e-12;
        if (p[i] > 1.0) p[i] = 1.0;
    }

    double A[16];
    memcpy(A, m->par.transition, sizeof(A));
    norm_rows(A);
    const double *pi = m->par.initial_prior;

    /* scaled forward pass */
    double acc = 0;
    for (int c = 0; c < 4; c++) { alpha[c] = pi[c] * p[c]; acc += alpha[c]; }
    if (acc < 1e-12) acc = 1e-12;
    for (int c = 0; c < 4; c++) alpha[c] /= acc;
    for (int t = 1; t < n; t++) {
        acc = 0;
        for (int c = 0; c < 4; c++) {
            double s = 0;
            for (int r = 0; r < 4; r++) s += alpha[(t - 1) * 4 + r] * A[r * 4 + c];
            alpha[t * 4 + c] = s * p[t * 4 + c];
            acc += alpha[t * 4 + c];
        }
        if (acc < 1e-12) acc = 1e-12;
        for (int c = 0; c < 4; c++) alpha[t * 4 + c] /= acc;
    }
    /* backward pass */
    for (int c = 0; c < 4; c++) beta[(n - 1) * 4 + c] = 1.0;
    for (int t = n - 2; t >= 0; t--) {
        acc = 0;
        for (int r = 0; r < 4; r++) {
            double s = 0;
            for (int c = 0; c < 4; c++)
                s += A[r * 4 + c] * p[(t + 1) * 4 + c] * beta[(t + 1) * 4 + c];
            beta[t * 4 + r] = s;
            acc += s;
        }
        if (acc < 1e-12) acc = 1e-12;
        for (int r = 0; r < 4; r++) beta[t * 4 + r] /= acc;
    }

    for (int t = 0; t < n; t++) {
        acc = 0;
        double g[4];
        for (int c = 0; c < 4; c++) {
            g[c] = alpha[t * 4 + c] * beta[t * 4 + c];
            acc += g[c];
        }
        if (acc < 1e-12) acc = 1e-12;
        int best = 0;
        for (int c = 0; c < 4; c++) {
            g[c] /= acc;
            if (g[c] > g[best]) best = c;
            if (posterior) posterior[t * 4 + c] = g[c];
        }
        stages[t] = (uint8_t)best;
    }

    /* optional isolated-run smoothing (min_deep_epochs > 1) */
    if (m->par.min_deep_epochs > 1) {
        int min_run = m->par.min_deep_epochs;
        int i = 0;
        while (i < n) {
            if (stages[i] == 2) {
                int st = i;
                while (i < n && stages[i] == 2) i++;
                if (i - st < min_run)
                    for (int j = st; j < i; j++) stages[j] = 1;
            } else {
                i++;
            }
        }
    }

    SOMNO_FREE(p); SOMNO_FREE(alpha); SOMNO_FREE(beta);
    return 0;
}
