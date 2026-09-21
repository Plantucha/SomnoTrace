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

/* Portable SSTP payload parser + LightGBM ensemble evaluator + temporal
 * decoder. Pure C11 — no ESP-IDF dependencies (host parity test compiles the
 * same objects). */

#ifndef SOMNO_ML_MODEL_H
#define SOMNO_ML_MODEL_H

#include <stddef.h>
#include <stdint.h>

typedef struct somno_model somno_model_t;

/* Parse an SSTP payload (decrypted). The buffer must outlive the model —
 * trees are evaluated in place, zero-copy. Returns NULL on format errors. */
somno_model_t *somno_ml_model_load(const void *sstp, size_t len);
void somno_ml_model_destroy(somno_model_t *m);

const char *somno_ml_model_semver_of(const somno_model_t *m);
uint32_t somno_ml_model_feature_fnv(const somno_model_t *m);
int somno_ml_model_n_features(const somno_model_t *m);

/* Full per-epoch pipeline: 3 boosters -> temperature -> soft hierarchical
 * fusion -> convex fusion -> prior correction. out4 receives the fused,
 * prior-corrected probabilities (unnormalized — the same values the Python
 * reference feeds decode_probabilities). Normalize for display/confidence. */
void somno_ml_predict(const somno_model_t *m, const float *feats, double out4[4]);

/* Normalize a prob row in place (display form, p_display in the reference). */
static inline void somno_ml_normalize4(double p[4])
{
    double s = p[0] + p[1] + p[2] + p[3];
    if (s < 1e-12) s = 1e-12;
    for (int i = 0; i < 4; i++) p[i] /= s;
}

/* Post-session decode over n fused-but-unnormalized probability rows
 * (n*4 doubles, the same values predict() emits before normalization —
 * pass display probs; decode re-normalizes internally where needed).
 * stages receives argmax labels; posterior (may be NULL) receives n*4
 * forward-backward gamma values. */
int somno_ml_decode_night(const somno_model_t *m, const double *probs, int n,
                          uint8_t *stages, double *posterior);

/* Decode mode id stored in .sst (forward_backward = 2). */
#define SOMNO_ML_DECODE_FORWARD_BACKWARD 2

#endif /* SOMNO_ML_MODEL_H */
