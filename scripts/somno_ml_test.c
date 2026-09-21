/*
 * SomnoTrace - host parity test for the SomnoStage C runtime
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

/* Host parity harness: feeds a dev fixture (SSTF) through the C feature
 * engine + packed-model inference + decoder, and compares against the Python
 * reference (SSTR) and feature dump (SSTX). Exit 0 = pass.
 *
 * The artifact dir comes from argv[1], or SOMNO_ML_ARTIFACTS when run through
 * scripts/run_host_tests.sh (which invokes test binaries with no arguments).
 * Artifacts live in the training repo — never committed here.
 *
 * Build:
 *   cc -std=c11 -Wall -Wextra -Werror -Icomponents/somno_ml/include \
 *      -Icomponents/somno_ml \
 *      components/somno_ml/somno_ml_features.c \
 *      components/somno_ml/somno_ml_model.c \
 *      scripts/somno_ml_test.c -lm -o /tmp/somno-ml-test
 *   /tmp/somno-ml-test <dev-artifact-dir>
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "somno_ml_features.h"
#include "somno_ml_model.h"

static void *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open %s failed\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { exit(2); }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static int run_case(const char *dir, const char *tag, const somno_model_t *m)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/dev_fixture_%s.bin", dir, tag);
    size_t flen; uint8_t *fb = read_file(path, &flen);
    snprintf(path, sizeof(path), "%s/dev_reference_%s.bin", dir, tag);
    size_t rlen; uint8_t *rb = read_file(path, &rlen);
    snprintf(path, sizeof(path), "%s/dev_features_%s.bin", dir, tag);
    size_t xlen; uint8_t *xb = read_file(path, &xlen);

    if (memcmp(fb, "SSTF", 4) || memcmp(rb, "SSTR", 4) || memcmp(xb, "SSTX", 4)) {
        fprintf(stderr, "%s: bad magic\n", tag); return 1;
    }
    uint16_t flags; float dt; uint32_t n_samp, start_min; uint8_t clock_ok;
    memcpy(&flags, fb + 6, 2);
    memcpy(&dt, fb + 8, 4);
    memcpy(&n_samp, fb + 12, 4);
    memcpy(&start_min, fb + 16, 4);
    clock_ok = fb[20];
    (void)start_min; (void)clock_ok;
    int has_motion = flags & 1;
    const float *spo2 = (const float *)(fb + 24);
    const float *pr = spo2 + n_samp;
    const float *mot = pr + n_samp;
    const uint8_t *sv = (const uint8_t *)(mot + n_samp);
    const uint8_t *pv = sv + n_samp;
    const uint8_t *mv = pv + n_samp;

    uint32_t n_ref;
    memcpy(&n_ref, rb + 8, 4);
    const double *ref_probs = (const double *)(rb + 12);
    const double *ref_post = ref_probs + (size_t)n_ref * 4;
    const uint8_t *ref_stages = (const uint8_t *)(ref_post + (size_t)n_ref * 4);

    uint32_t xrows, xcols;
    memcpy(&xrows, xb + 4, 4);
    memcpy(&xcols, xb + 8, 4);
    const double *ref_feats = (const double *)(xb + 12);
    if (xcols != SOMNO_ML_N_FEATURES) { fprintf(stderr, "%s: feature count mismatch\n", tag); return 1; }

    somno_feat_t *fe = somno_feat_create(dt, has_motion);
    if (!fe) { fprintf(stderr, "feat create failed\n"); return 1; }

    double *fused = malloc((size_t)n_ref * 4 * sizeof(double));
    double max_fdiff = 0; int n_feat_cmp = 0;
    int e = 0;
    for (uint32_t i = 0; i < n_samp; i++) {
        float feats[SOMNO_ML_N_FEATURES];
        if (!somno_feat_push(fe, spo2[i], pr[i], mot[i], sv[i], pv[i], mv[i], feats))
            continue;
        if (e >= (int)n_ref) break;
        /* feature-level comparison */
        for (int c = 0; c < SOMNO_ML_N_FEATURES; c++) {
            double rv = ref_feats[(size_t)e * xcols + c];
            double cv = feats[c];
            if (isnan(rv) && isnan(cv)) continue;
            double d = fabs(rv - (double)cv);
            if (d > max_fdiff) max_fdiff = d;
            n_feat_cmp++;
        }
        double out[4];
        somno_ml_predict(m, feats, out);
        memcpy(&fused[(size_t)e * 4], out, 4 * sizeof(double));
        e++;
    }
    if (e != (int)n_ref) {
        fprintf(stderr, "%s: epoch count %d != reference %u\n", tag, e, n_ref);
        return 1;
    }

    uint8_t *stages = malloc(n_ref);
    double *post = malloc((size_t)n_ref * 4 * sizeof(double));
    somno_ml_decode_night(m, fused, (int)n_ref, stages, post);

    int agree = 0, stage_agree = 0;
    double max_pdiff = 0, max_postdiff = 0;
    for (uint32_t i = 0; i < n_ref; i++) {
        double disp[4];
        memcpy(disp, &fused[i * 4], sizeof(disp));
        somno_ml_normalize4(disp);
        int ra = 0, ca = 0;
        for (int c = 0; c < 4; c++) {
            double d = fabs(disp[c] - ref_probs[i * 4 + c]);
            if (d > max_pdiff) max_pdiff = d;
            d = fabs(post[i * 4 + c] - ref_post[i * 4 + c]);
            if (d > max_postdiff) max_postdiff = d;
            if (ref_probs[i * 4 + c] > ref_probs[i * 4 + ra]) ra = c;
            if (disp[c] > disp[ca]) ca = c;
        }
        if (ra == ca) agree++;
        if (stages[i] == ref_stages[i]) stage_agree++;
    }
    double agree_f = (double)agree / n_ref;
    double stage_f = (double)stage_agree / n_ref;
    printf("case %s: epochs=%u feat|maxdiff|=%.3e prob|maxdiff|=%.4f "
           "post|maxdiff|=%.4f argmax=%.4f stages=%.4f\n",
           tag, n_ref, max_fdiff, max_pdiff, max_postdiff, agree_f, stage_f);

    int fail = 0;
    if (agree_f < 0.99) { printf("  FAIL argmax agreement < 0.99\n"); fail = 1; }
    if (stage_f < 0.98) { printf("  FAIL stage agreement < 0.98\n"); fail = 1; }
    if (max_pdiff > 0.05) { printf("  FAIL prob diff > 0.05\n"); fail = 1; }
    if (max_fdiff > 1e-3) { printf("  WARN feature diff %.3e\n", max_fdiff); }

    free(fused); free(stages); free(post);
    free(fb); free(rb); free(xb);
    somno_feat_destroy(fe);
    return fail;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : getenv("SOMNO_ML_ARTIFACTS");
    if (!dir || !*dir) {
        fprintf(stderr, "usage: %s <dev-artifact-dir>  (or set SOMNO_ML_ARTIFACTS)\n",
                argv[0]);
        return 2;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/model.sstp", dir);
    size_t mlen; void *mb = read_file(path, &mlen);
    somno_model_t *m = somno_ml_model_load(mb, mlen);
    if (!m) { fprintf(stderr, "model parse failed\n"); return 1; }
    printf("model: semver=%s features=%d\n",
           somno_ml_model_semver_of(m), somno_ml_model_n_features(m));

    int fail = 0;
    fail |= run_case(dir, "a", m);
    fail |= run_case(dir, "b", m);
    fail |= run_case(dir, "c", m);
    somno_ml_model_destroy(m);
    free(mb);
    printf(fail ? "PARITY: FAIL\n" : "PARITY: PASS\n");
    return fail;
}
