/*
 * SomnoTrace - SomnoStage internal declarations (component-private)
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

#ifndef SOMNO_ML_INTERNAL_H
#define SOMNO_ML_INTERNAL_H

#include <stdint.h>
#include "somno_ml_model.h"

/* Access the parsed model (NULL before/without somno_ml_init). */
const somno_model_t *somno_ml_get_model(void);

/* Reconstruct the 32-byte artifact key from the two injected halves.
 * Caller must scrub the output buffer after use. Defined in
 * somno_ml_keys.c (real-model builds only). */
void somno_ml_reconstruct_key(uint8_t out[32]);

/* Start the scoring worker (called once by somno_ml_init on success). */
void somno_ml_worker_start(void);

#endif /* SOMNO_ML_INTERNAL_H */
