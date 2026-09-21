/*
 * SomnoTrace - Causal feature engine for the SomnoStage model
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

/* Portable causal replica of model/inference/features.py (contract
 * ring_tabular_v4_1, 95 features). Pure C11 - no ESP-IDF dependencies, so the
 * same code compiles for the host parity test. */

#ifndef SOMNO_ML_FEATURES_H
#define SOMNO_ML_FEATURES_H

#include <stdint.h>

#define SOMNO_ML_N_FEATURES 95

typedef struct somno_feat somno_feat_t;

/* dt = seconds between samples (1.0 for Gen2/AS11, 4.0 for Gen1 0.25 Hz).
 * has_motion: nonzero when a motion channel exists (ring) vs absent (AS11). */
somno_feat_t *somno_feat_create(double dt, int has_motion);
void somno_feat_destroy(somno_feat_t *f);

/* Feed one sample. Invalid/missing values are passed as-is (NaN allowed) with
 * the corresponding valid flag clear. Returns 1 when a full epoch closed and
 * `feats` was filled (NaN for undefined features), else 0. */
int somno_feat_push(somno_feat_t *f, double spo2, double pr, double motion,
                    int spo2_valid, int pr_valid, int motion_valid,
                    float feats[SOMNO_ML_N_FEATURES]);

/* Epochs emitted so far. */
int somno_feat_epochs(const somno_feat_t *f);

/* Nonzero when the most recently emitted epoch row was valid (met the
 * per-channel min_required sample counts). */
int somno_feat_last_valid(const somno_feat_t *f);

#endif /* SOMNO_ML_FEATURES_H */
