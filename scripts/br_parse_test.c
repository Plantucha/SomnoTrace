/*
 * SomnoTrace - BreathingRoom response parser regression tests
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * SomnoTrace is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "br_parse.h"

static int n_pass, n_fail;

static void check(bool cond, const char *what)
{
    if (cond) n_pass++;
    else { n_fail++; printf("  FAIL %s\n", what); }
}

int main(void)
{
    /* ── /api/imports/upload responses ─────────────────────────────── */

    /* Live capture: settings-only upload */
    {
        br_import_result_t r;
        check(br_parse_import(
            "{\"created\":0,\"skipped\":0,\"deferred\":0,\"sessions\":[]}", &r),
            "import: parse settings-only response");
        check(r.created == 0 && r.skipped == 0 && r.deferred == 0 &&
              r.n_sessions == 0, "import: zeros");
    }

    /* Sessions accepted — two pending sessions returned */
    {
        br_import_result_t r;
        check(br_parse_import(
            "{\"created\":2,\"skipped\":1,\"deferred\":0,"
            "\"sessions\":[{\"id\":\"a\"},{\"id\":\"b\"}]}", &r),
            "import: parse sessions");
        check(r.created == 2 && r.skipped == 1 && r.deferred == 0 &&
              r.n_sessions == 2, "import: counts + session count");
    }

    /* Deferred sessions must be retried — surfaced via the count */
    {
        br_import_result_t r;
        check(br_parse_import(
            "{\"created\":0,\"skipped\":0,\"deferred\":3,\"sessions\":[]}", &r),
            "import: parse deferred");
        check(r.deferred == 3, "import: deferred counted");
    }

    /* Error bodies are NOT valid import responses */
    {
        br_import_result_t r;
        check(!br_parse_import("{\"error\":\"No files provided\"}", &r),
              "import: error body rejected");
        check(!br_parse_import("{\"error\":\"Unsupported file type\"}", &r),
              "import: unsupported-type body rejected");
        check(!br_parse_import("not json", &r), "import: garbage rejected");
        check(!br_parse_import("[]", &r), "import: array rejected");
        check(!br_parse_import(NULL, &r), "import: NULL rejected");
    }

    /* ── /api/oximetry/upload responses ────────────────────────────── */

    /* Live capture shape: two imported files */
    {
        br_ox_result_t r;
        check(br_parse_oximetry(
            "{\"results\":["
            "{\"filename\":\"20260101230000.bin\",\"status\":\"imported\","
            "\"id\":\"00000000-0000-4000-8000-000000000001\",\"nightDate\":\"2026-01-01\","
            "\"device\":\"Synthetic Ring\",\"startedAt\":\"2026-01-01T23:00:00\","
            "\"durationSec\":120,\"intervalSec\":1},"
            "{\"filename\":\"20260102010000.bin\",\"status\":\"replaced\","
            "\"id\":\"00000000-0000-4000-8000-000000000002\",\"nightDate\":\"2026-01-01\","
            "\"device\":\"Synthetic Ring\",\"startedAt\":\"2026-01-02T01:00:00\","
            "\"durationSec\":300,\"intervalSec\":1}]}", &r),
            "ox: parse two imports");
        check(r.n_results == 2, "ox: result count");
        check(r.n_imported == 2 && r.n_rejected == 0, "ox: imported+replaced count");
        check(strcmp(r.first_id, "00000000-0000-4000-8000-000000000001") == 0,
              "ox: first id captured");
        check(r.first_reason[0] == '\0', "ox: no reason on success");
    }

    /* Mixed success + rejection: per-file, never request-fatal */
    {
        br_ox_result_t r;
        check(br_parse_oximetry(
            "{\"results\":["
            "{\"filename\":\"a.bin\",\"status\":\"imported\",\"id\":\"id-1\"},"
            "{\"filename\":\"b.csv\",\"status\":\"rejected\","
            "\"reason\":\"filename timestamp does not match first sample\"}]}", &r),
            "ox: parse mixed");
        check(r.n_imported == 1 && r.n_rejected == 1, "ox: mixed counts");
        check(strcmp(r.first_id, "id-1") == 0, "ox: id from the good file");
        check(strstr(r.first_reason, "timestamp does not match") != NULL,
              "ox: rejection reason captured");
    }

    /* A bare rejection carries the reason for the failure log */
    {
        br_ox_result_t r;
        check(br_parse_oximetry(
            "{\"results\":[{\"filename\":\"big.bin\",\"status\":\"rejected\","
            "\"reason\":\"file too large (max 8 MB)\"}]}", &r),
            "ox: parse rejection");
        check(r.n_imported == 0 && r.n_rejected == 1, "ox: all rejected");
        check(r.first_id[0] == '\0', "ox: rejected has no id");
        check(strstr(r.first_reason, "too large") != NULL,
              "ox: oversize reason");
    }

    /* Rejection without a reason field falls back to the status word */
    {
        br_ox_result_t r;
        check(br_parse_oximetry(
            "{\"results\":[{\"filename\":\"x.bin\",\"status\":\"rejected\"}]}", &r),
            "ox: bare rejection");
        check(r.n_rejected == 1 && strcmp(r.first_reason, "rejected") == 0,
              "ox: reason falls back to status");
    }

    /* Request-level error bodies are NOT valid ox responses */
    {
        br_ox_result_t r;
        check(!br_parse_oximetry("{\"error\":\"No files provided\"}", &r),
              "ox: error body rejected");
        check(!br_parse_oximetry(
            "{\"error\":\"subscription_required\",\"feature\":\"tracker\"}", &r),
              "ox: 402 body rejected");
        check(!br_parse_oximetry("garbage", &r), "ox: garbage rejected");
        check(!br_parse_oximetry(NULL, &r), "ox: NULL rejected");
    }

    /* Empty results array — parses, counts zero (probe edge) */
    {
        br_ox_result_t r;
        check(br_parse_oximetry("{\"results\":[]}", &r), "ox: empty results");
        check(r.n_results == 0 && r.n_imported == 0 && r.n_rejected == 0,
              "ox: empty counts");
    }

    {
        const char *bad[] = {
            "{\"created\":\"invalid\",\"skipped\":0,\"deferred\":0,\"sessions\":[]}",
            "{\"created\":0,\"skipped\":0,\"deferred\":\"invalid\",\"sessions\":[]}",
            "{\"created\":0,\"skipped\":0,\"deferred\":-1,\"sessions\":[]}",
            "{\"created\":0.5,\"skipped\":0,\"deferred\":0,\"sessions\":[]}",
            "{\"created\":2147483648,\"skipped\":0,\"deferred\":0,\"sessions\":[]}",
            "{\"created\":0,\"skipped\":0,\"sessions\":[]}",
            "{\"created\":0,\"skipped\":0,\"deferred\":0,\"sessions\":null}"
        };
        br_import_result_t r;
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
            check(!br_parse_import(bad[i], &r), "import: malformed contract rejected");
    }
    {
        const char *bad[] = {
            "{\"results\":[null]}",
            "{\"results\":[42]}",
            "{\"results\":[{\"filename\":\"a.bin\",\"status\":\"imported\"}]}",
            "{\"results\":[{\"filename\":\"a.bin\",\"status\":\"replaced\",\"id\":\"\"}]}",
            "{\"results\":[{\"status\":\"imported\",\"id\":\"synthetic-id\"}]}",
            "{\"results\":[{\"filename\":\"a.bin\",\"status\":\"unknown\"}]}",
            "{\"results\":[{\"filename\":\"a.bin\",\"status\":\"imported\",\"id\":\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\"}]}"
        };
        br_ox_result_t r;
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
            check(!br_parse_oximetry(bad[i], &r), "ox: malformed contract rejected");
    }

    {
        const char *names[] = {"a.bin", "b.bin"};
        const char *good = "{\"results\":[{\"filename\":\"b.bin\",\"status\":\"imported\",\"id\":\"synthetic-b\"},"
                           "{\"filename\":\"a.bin\",\"status\":\"replaced\",\"id\":\"synthetic-a\"}]}";
        br_ox_result_t r;
        check(br_parse_oximetry_files(good, &r, names, 2), "ox: unordered results matched to filenames");
        check(!br_parse_oximetry_files(good, &r, names, 1), "ox: extra result rejected");
        check(!br_parse_oximetry_files("{\"results\":[]}", &r, names, 2), "ox: missing results rejected");
        const char *duplicate = "{\"results\":[{\"filename\":\"a.bin\",\"status\":\"imported\",\"id\":\"synthetic-a\"},"
                                "{\"filename\":\"a.bin\",\"status\":\"replaced\",\"id\":\"synthetic-a\"}]}";
        check(!br_parse_oximetry_files(duplicate, &r, names, 2), "ox: duplicate filename rejected");
        const char *wrong[] = {"wrong.bin", "b.bin"};
        check(!br_parse_oximetry_files(good, &r, wrong, 2), "ox: unrelated filename rejected");
    }

    /* ── /api/sleep/upload (SomnoStage) responses ───────────────────── */

    /* Live shape: an AS11 unit imported and a ring unit replaced */
    {
        const char *names[] = {"as11_20261023_014432_stages.sst",
                               "20261023014432_stages.sst"};
        br_ox_result_t r;
        check(br_parse_stage_files(
            "{\"results\":["
            "{\"filename\":\"as11_20261023_014432_stages.sst\",\"status\":\"imported\","
            "\"id\":\"5d1b4f3e-8a47-4f0e-9c1d-3a6f1b2e7c90\",\"nightDate\":\"2026-10-22\","
            "\"startedAt\":\"2026-10-23T01:44:32-04:00\",\"epochSec\":30,\"epochs\":842,"
            "\"sourceType\":\"airsense11\",\"offsetSource\":\"unit\"},"
            "{\"filename\":\"20261023014432_stages.sst\",\"status\":\"replaced\","
            "\"id\":\"00000000-0000-4000-8000-000000000002\"}]}",
            &r, names, 2), "stage: parse imported+replaced");
        check(r.n_results == 2 && r.n_imported == 2 && r.n_rejected == 0,
              "stage: imported+replaced counts");
        check(strcmp(r.first_id, "5d1b4f3e-8a47-4f0e-9c1d-3a6f1b2e7c90") == 0,
              "stage: first id captured");
    }

    /* A rejected file carries its reason; the request itself succeeded */
    {
        const char *names[] = {"as11_20261023_014432_stages.sst"};
        br_ox_result_t r;
        check(br_parse_stage_files(
            "{\"results\":[{\"filename\":\"as11_20261023_014432_stages.sst\","
            "\"status\":\"rejected\",\"reason\":\"partial scoring run\"}]}",
            &r, names, 1), "stage: parse rejection");
        check(r.n_rejected == 1 && r.n_imported == 0, "stage: rejection counted");
        check(strcmp(r.first_reason, "partial scoring run") == 0,
              "stage: rejection reason captured");
    }

    /* The transient-vs-permanent split keys off this exact reason string */
    {
        const char *names[] = {"20261023014432_stages.sst"};
        br_ox_result_t r;
        check(br_parse_stage_files(
            "{\"results\":[{\"filename\":\"20261023014432_stages.sst\","
            "\"status\":\"rejected\",\"reason\":\"storage error\"}]}",
            &r, names, 1), "stage: parse storage error");
        check(strstr(r.first_reason, "storage error") != NULL,
              "stage: storage error reason captured");
    }

    /* Filename mismatch and error bodies are not valid stage responses */
    {
        const char *names[] = {"20261023014432_stages.sst"};
        br_ox_result_t r;
        check(!br_parse_stage_files(
            "{\"results\":[{\"filename\":\"other_stages.sst\","
            "\"status\":\"imported\",\"id\":\"x\"}]}", &r, names, 1),
            "stage: unrelated filename rejected");
        check(!br_parse_stage_files("{\"error\":\"Invalid timeZone\"}", &r, names, 1),
              "stage: error body rejected");
    }

    printf("br_parse_test: %d passed, %d failed\n", n_pass, n_fail);
    return n_fail ? 1 : 0;
}
