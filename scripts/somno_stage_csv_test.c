/*
 * SomnoTrace - Host unit tests for the SomnoStage Zeo CSV exporter
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3, or (at your option) any later version.
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

/* somno_stage_csv.c is pure libc — no shims needed.  What is pinned here:
 *
 *   - the Zeo row format OSCAR's "Import ZEO Data" loader parses (header
 *     columns, quoted Detailed/Sleep Graph fields, space-separated codes);
 *   - SomnoTrace -> Zeo stage mapping (0 Wake->1, 1 Light->3, 2 Deep->4,
 *     3 REM->2);
 *   - the strict 30 s output grid: Gen1 rings produce 32 s epochs
 *     (8 x 4 s samples), and OSCAR steps DSG slots at a hardcoded 30 s —
 *     writing raw 32 s epochs would compress an 8 h night to 7 h 30 m.
 *     The midpoint resample is checked to land every transition within
 *     half a slot of its true timestamp;
 *   - summary fields (Time-in-*, Time to Z, Awakenings) over the remapped
 *     grid;
 *   - INSUFFICIENT/PARTIAL/corrupt sidecars must never produce a CSV;
 *   - somno_stage_export_csv_from_sst() reads a real SST1 file back.
 */

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "somno_ml.h"
#include "somno_stage_csv.h"

static int g_failures;
#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg));          \
            g_failures++;                                                     \
        }                                                                     \
    } while (0)

/* ── Fixture helpers ──────────────────────────────────────────────── */

static somno_sst_header_t mk_hdr(uint32_t n_epochs, uint32_t epoch_sec,
                                 uint8_t flags)
{
    somno_sst_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic      = SOMNO_SST_MAGIC;
    h.version    = SOMNO_SST_VERSION;
    h.flags      = flags;
    h.source_type = SOMNO_SST_SOURCE_RING;
    h.n_epochs   = n_epochs;
    h.start_ms   = 1760000000000LL;   /* fixed instant; TZ-independent checks */
    h.epoch_sec  = epoch_sec;
    return h;
}

/* stages[] filled from a (stage,count) run-length spec. */
static uint8_t *mk_stages(uint32_t n, const int (*runs)[2], int n_runs)
{
    uint8_t *s = malloc(n);
    if (!s) return NULL;
    uint32_t at = 0;
    for (int i = 0; i < n_runs; i++)
        for (int j = 0; j < runs[i][1] && at < n; j++)
            s[at++] = (uint8_t)runs[i][0];
    while (at < n) s[at++] = 0;
    return s;
}

static char *read_all(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* The row's graph columns are the only quoted fields: field `which` sits
 * between a `,"` (first field) or `","` separators (later fields) and the
 * next `"`.  Returns malloc'd field text or NULL. */
static char *quoted_field(char *row, int which)
{
    char *p = row;
    if (which == 0) {
        p = strstr(p, ",\"");          /* ," opens the first quoted field */
        if (!p) return NULL;
        p += 2;
    } else {
        for (int i = 0; i < which; i++) {
            p = strstr(p, "\",\"");    /* "," closes one, opens the next */
            if (!p) return NULL;
            p += 3;                    /* past quote, comma, quote       */
        }
    }
    char *end = strchr(p, '"');
    if (!end) return NULL;
    size_t n = (size_t)(end - p);
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, p, n);
    out[n] = '\0';
    return out;
}

/* Parse a space-separated code list into out[]; returns the count. */
static int parse_codes(const char *field, int *out, int max)
{
    int n = 0;
    const char *p = field;
    while (*p && n < max) {
        char *e;
        long v = strtol(p, &e, 10);
        if (e == p) break;
        out[n++] = (int)v;
        p = e;
    }
    return n;
}

static int int_field(const char *row, int idx)
{
    /* Unquoted numeric fields, comma-separated, before the quoted graphs.
     * strtok_r destroys its input, so work on a private copy every call. */
    char *copy = strdup(row);
    if (!copy) return -1;
    char *save = NULL;
    int i = 0, val = -1;
    for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        if (i == idx) { val = atoi(tok); break; }
        if (++i > 11) break;
    }
    free(copy);
    return val;
}

/* ── Tests ────────────────────────────────────────────────────────── */

static void test_30s_identity(void)
{
    /* 50 x 30 s epochs: 10 wake, 20 light, 5 wake, 10 deep, 5 REM. */
    const int runs[][2] = { {0,10}, {1,20}, {0,5}, {2,10}, {3,5} };
    uint8_t *st = mk_stages(50, runs, 5);
    somno_sst_header_t h = mk_hdr(50, 30, 0);

    const char *path = "/tmp/sst_csv_t30/out.csv";
    unlink(path);
    CHECK(somno_stage_write_zeo_csv_file(path, &h, st) == 0,
          "30 s export failed");
    char *txt = read_all(path);
    CHECK(txt != NULL, "no CSV written");

    if (txt) {
        static const char k_hdr[] =
            "Start of Night,End of Night,Rise Time,ZQ,Time to Z,"
            "Time in Wake,Time in REM,Time in Light,Time in Deep,"
            "Awakenings,Morning Feel,Detailed Sleep Graph,Sleep Graph\n";
        CHECK(strncmp(txt, k_hdr, sizeof(k_hdr) - 1) == 0,
              "header columns differ from the Zeo layout");
        char *row = strchr(txt, '\n');
        CHECK(row != NULL, "no data row");
        if (row) {
            row = strdup(row + 1);
            /* field order: 0 start,1 end,2 rise,3 ZQ,4 TimeToZ,5 Wake,
             * 6 REM,7 Light,8 Deep,9 Awakenings */
            /* ZQ from the minutes below: (18/60 + 3/120 + 5*1.5/60 - 8/120
             * - 1/15) * 8.5 = 2.69 -> 3.  Each of its terms moves it. */
            CHECK(int_field(row, 3) == 3,   "ZQ != 3");
            CHECK(int_field(row, 4) == 5,   "Time to Z != 5 min");
            CHECK(int_field(row, 5) == 8,   "Time in Wake != 8 min");
            CHECK(int_field(row, 6) == 3,   "Time in REM != 3 min");
            CHECK(int_field(row, 7) == 10,  "Time in Light != 10 min");
            CHECK(int_field(row, 8) == 5,   "Time in Deep != 5 min");
            CHECK(int_field(row, 9) == 1,   "Awakenings != 1");

            /* strtok_r destroyed row — reparse for the quoted fields. */
            char *row2 = strchr(txt, '\n') + 1;
            char *dsg = quoted_field(row2, 0);
            CHECK(dsg != NULL, "no DSG field");
            if (dsg) {
                int codes[64];
                int n = parse_codes(dsg, codes, 64);
                CHECK(n == 50, "DSG length != 50 for a 30 s source");
                /* mapping: 0->1, 1->3, 0->1, 2->4, 3->2 */
                int exp[50];
                int at = 0;
                for (int i = 0; i < 10; i++) exp[at++] = 1;
                for (int i = 0; i < 20; i++) exp[at++] = 3;
                for (int i = 0; i < 5;  i++) exp[at++] = 1;
                for (int i = 0; i < 10; i++) exp[at++] = 4;
                for (int i = 0; i < 5;  i++) exp[at++] = 2;
                CHECK(n == 50 && memcmp(codes, exp, sizeof(exp)) == 0,
                      "DSG codes are not the identity-mapped Zeo sequence");
                free(dsg);
            }
            char *sg = quoted_field(strchr(txt, '\n') + 1, 1);
            CHECK(sg != NULL, "no Sleep Graph field");
            if (sg) {
                int codes[8];
                int n = parse_codes(sg, codes, 8);
                CHECK(n == 5, "Sleep Graph != ceil(50/10) entries");
                free(sg);
            }
            free(row);
        }
        free(txt);
    }
    free(st);
}

static void test_32s_gen1_resample(void)
{
    /* Gen1 ring: 900 epochs x 32 s = 8.0 h.  A raw dump would feed OSCAR
     * 900 x 30 s = 7.5 h — the exporter must emit 960 thirty-second slots. */
    const int runs[][2] = { {1,450}, {2,450} };   /* Light, then Deep */
    uint8_t *st = mk_stages(900, runs, 2);
    somno_sst_header_t h = mk_hdr(900, 32, 0);

    const char *path = "/tmp/sst_csv_t32/out.csv";
    unlink(path);
    CHECK(somno_stage_write_zeo_csv_file(path, &h, st) == 0,
          "32 s export failed");
    char *txt = read_all(path);
    CHECK(txt != NULL, "no CSV written for the 32 s case");

    if (txt) {
        char *row = strchr(txt, '\n') + 1;
        char *dsg = quoted_field(row, 0);
        CHECK(dsg != NULL, "no DSG field");
        if (dsg) {
            int *codes = malloc(1000 * sizeof(int));
            int n = parse_codes(dsg, codes, 1000);
            CHECK(n == 960, "32 s night did not expand to a 960-slot 30 s grid");
            if (n == 960) {
                /* True transition is at epoch 450 = t 14400 s.  Slot k owns
                 * time (k*30+15)/32: k=479 -> epoch 449 (Light), k=480 ->
                 * epoch 450 (Deep).  The transition lands within half a
                 * slot of its true timestamp — never drifting 30 min over
                 * the night like a raw dump would. */
                CHECK(codes[0] == 3 && codes[479] == 3,
                      "early slots not mapped to Light");
                CHECK(codes[480] == 4 && codes[959] == 4,
                      "late slots not mapped to Deep");
                int n_light = 0, n_deep = 0;
                for (int i = 0; i < 960; i++) {
                    if (codes[i] == 3) n_light++;
                    else if (codes[i] == 4) n_deep++;
                }
                CHECK(n_light == 480 && n_deep == 480,
                      "32 s resample skewed the stage totals");
            }
            free(codes);
            free(dsg);
        }
        /* durations must sum to the true 8.0 h, not 7.5 h */
        char *rowc = strdup(strchr(txt, '\n') + 1);
        int light_min = int_field(rowc, 7);
        int deep_min  = int_field(rowc, 8);
        CHECK(light_min + deep_min == 480,
              "stage minutes do not sum to the real 8 h night");
        free(rowc);
        free(txt);
    }
    free(st);
}

static void test_rejections(void)
{
    uint8_t st[8] = {0,1,2,3,0,1,2,3};
    somno_sst_header_t h;

    h = mk_hdr(8, 30, SOMNO_SST_FLAG_INSUFFICIENT);
    CHECK(somno_stage_write_zeo_csv_file("/tmp/sst_csv_bad/a.csv", &h, st) != 0,
          "INSUFFICIENT sidecar produced a CSV");
    h = mk_hdr(8, 30, SOMNO_SST_FLAG_PARTIAL);
    CHECK(somno_stage_write_zeo_csv_file("/tmp/sst_csv_bad/b.csv", &h, st) != 0,
          "PARTIAL sidecar produced a CSV");
    h = mk_hdr(0, 30, 0);
    CHECK(somno_stage_write_zeo_csv_file("/tmp/sst_csv_bad/c.csv", &h, st) != 0,
          "zero-epoch sidecar produced a CSV");
    h = mk_hdr(8, 30, 0); h.magic = 0xDEADBEEF;
    CHECK(somno_stage_write_zeo_csv_file("/tmp/sst_csv_bad/d.csv", &h, st) != 0,
          "bad magic produced a CSV");
    h = mk_hdr(8, 0, 0);
    CHECK(somno_stage_write_zeo_csv_file("/tmp/sst_csv_bad/e.csv", &h, st) != 0,
          "zero epoch_sec produced a CSV");
    h = mk_hdr(8, 30, 0);
    CHECK(somno_stage_write_zeo_csv_file("/tmp/sst_csv_bad/f.csv", &h, NULL) != 0,
          "NULL stages produced a CSV");
}

static void test_export_from_sst(void)
{
    const char *sst = "/tmp/sst_csv_sst/in.sst";
    const char *csv = "/tmp/sst_csv_sst/out/out.csv";
    unlink(sst); unlink(csv);

    /* A real SST1 file: 64-byte header + stages[n] + conf[n]. */
    const int runs[][2] = { {0,4}, {1,12}, {3,4} };
    uint32_t n = 20;
    uint8_t *st = mk_stages(n, runs, 3);
    uint8_t *conf = malloc(n);
    memset(conf, 200, n);
    somno_sst_header_t h = mk_hdr(n, 30, 0);

    mkdir("/tmp/sst_csv_sst", 0775);
    FILE *f = fopen(sst, "wb");
    CHECK(f != NULL, "cannot write fixture sst");
    if (f) {
        fwrite(&h, 1, sizeof(h), f);
        fwrite(st, 1, n, f);
        fwrite(conf, 1, n, f);
        fclose(f);
    }

    CHECK(somno_stage_export_csv_from_sst(sst, csv) == 0,
          "sst->csv export failed");
    char *txt = read_all(csv);
    CHECK(txt != NULL, "sst->csv produced no file");
    if (txt) {
        char *dsg = quoted_field(strchr(txt, '\n') + 1, 0);
        int codes[32];
        int cnt = dsg ? parse_codes(dsg, codes, 32) : 0;
        CHECK(cnt == 20, "round-trip DSG length wrong");
        CHECK(cnt == 20 && codes[0] == 1 && codes[4] == 3 && codes[16] == 2,
              "round-trip stage codes wrong");
        free(dsg);
        free(txt);
    }

    /* Skips, not errors: header-only stub (INSUFFICIENT), truncation,
     * foreign magic, missing file. */
    somno_sst_header_t stub = mk_hdr(0, 30, SOMNO_SST_FLAG_INSUFFICIENT);
    f = fopen(sst, "wb");
    fwrite(&stub, 1, sizeof(stub), f);
    fclose(f);
    CHECK(somno_stage_export_csv_from_sst(sst, csv) == 1,
          "INSUFFICIENT stub must be skipped, not exported");

    f = fopen(sst, "wb");
    h.n_epochs = 40;
    fwrite(&h, 1, sizeof(h), f);
    fwrite(st, 1, 10, f);              /* truncated body */
    fclose(f);
    CHECK(somno_stage_export_csv_from_sst(sst, csv) == 1,
          "truncated sst must be skipped");
    CHECK(somno_stage_export_csv_from_sst("/tmp/sst_csv_sst/nope.sst", csv) == 1,
          "missing sst must be skipped");

    free(st);
    free(conf);
}

/* A night that is asleep from its first epoch, and asleep in DEEP: sleep
 * onset, the awakening count and the "was asleep" state must all treat Deep
 * as sleep, and onset at slot 0 must still count the awakening after it. */
static void test_onset_in_deep(void)
{
    /* 4 deep, 2 wake, 4 deep (30 s) */
    const int runs[][2] = { {2,4}, {0,2}, {2,4} };
    uint8_t *st = mk_stages(10, runs, 3);
    somno_sst_header_t h = mk_hdr(10, 30, 0);
    const char *path = "/tmp/sst_csv_deep/out.csv";
    unlink(path);
    CHECK(somno_stage_write_zeo_csv_file(path, &h, st) == 0, "deep-onset export failed");
    char *txt = read_all(path);
    char *row = txt ? strchr(txt, '\n') : NULL;
    CHECK(row != NULL, "deep-onset CSV has no data row");
    if (row) {
        row = strdup(row + 1);
        CHECK(int_field(row, 4) == 0, "Time to Z != 0 for a night asleep at slot 0");
        CHECK(int_field(row, 8) == 4, "Time in Deep != 4 min");
        CHECK(int_field(row, 9) == 1, "deep -> wake -> deep must count one awakening");
        free(row);
    }
    free(txt);
    free(st);
}

/* The 5-minute Sleep Graph: a last group shorter than 10 slots still gets a
 * code, and a Deep majority (code 4, the highest) can win a group. */
static void test_sleep_graph_tail_and_deep(void)
{
    /* 10 deep, then 5 REM: groups [0,10) and a 5-slot tail [10,15) */
    const int runs[][2] = { {2,10}, {3,5} };
    uint8_t *st = mk_stages(15, runs, 2);
    somno_sst_header_t h = mk_hdr(15, 30, 0);
    const char *path = "/tmp/sst_csv_sg/out.csv";
    unlink(path);
    CHECK(somno_stage_write_zeo_csv_file(path, &h, st) == 0, "sleep-graph export failed");
    char *txt = read_all(path);
    char *row = txt ? strchr(txt, '\n') : NULL;
    char *sg = row ? quoted_field(row + 1, 1) : NULL;
    int codes[4];
    int n = sg ? parse_codes(sg, codes, 4) : 0;
    CHECK(n == 2, "Sleep Graph != 2 groups for 15 slots");
    CHECK(n == 2 && codes[0] == 4 && codes[1] == 2,
          "Sleep Graph != \"4 2\" (deep group, then the 5-slot REM tail)");
    free(sg);
    free(txt);
    free(st);
}

/* The limits are inclusive: SST_MAX_EPOCHS epochs and a 3600 s epoch are both
 * valid sidecars, and start_ms 0 is not. ZQ is clamped to the Zeo 0..150 range. */
static void test_limits(void)
{
    const uint32_t max_epochs = 100000u;   /* SST_MAX_EPOCHS */
    uint8_t *st = malloc(max_epochs);
    memset(st, 1, max_epochs);             /* all light */
    somno_sst_header_t h = mk_hdr(max_epochs, 30, 0);
    const char *path = "/tmp/sst_csv_lim/max.csv";
    CHECK(somno_stage_write_zeo_csv_file(path, &h, st) == 0,
          "SST_MAX_EPOCHS epochs rejected (the limit is inclusive)");
    char *txt = read_all(path);
    char *row = txt ? strchr(txt, '\n') : NULL;
    CHECK(row && int_field(row + 1, 3) == 150, "ZQ not clamped to 150");
    free(txt);

    h = mk_hdr(1, 3600, 0);
    CHECK(somno_stage_write_zeo_csv_file("/tmp/sst_csv_lim/hour.csv", &h, st) == 0,
          "a 3600 s epoch rejected (the limit is inclusive)");

    memset(st, 0, 10);                     /* all wake */
    h = mk_hdr(10, 30, 0);
    CHECK(somno_stage_write_zeo_csv_file("/tmp/sst_csv_lim/wake.csv", &h, st) == 0,
          "all-wake export failed");
    txt = read_all("/tmp/sst_csv_lim/wake.csv");
    row = txt ? strchr(txt, '\n') : NULL;
    CHECK(row && int_field(row + 1, 3) == 0, "ZQ not clamped to 0 for an all-wake night");
    free(txt);

    h = mk_hdr(10, 30, 0); h.start_ms = 0;
    CHECK(somno_stage_write_zeo_csv_file("/tmp/sst_csv_lim/zero.csv", &h, st) != 0,
          "start_ms 0 produced a CSV");

    /* Every public entry point refuses a NULL it cannot use. */
    h = mk_hdr(10, 30, 0);
    CHECK(somno_stage_write_zeo_csv_stream(NULL, &h, st) == -1, "stream(NULL out) != -1");
    CHECK(somno_stage_write_zeo_csv_file(NULL, &h, st) == -1, "file(NULL path) != -1");
    CHECK(somno_stage_export_csv_from_sst(NULL, "/tmp/sst_csv_lim/x.csv") == -1,
          "export(NULL sst) != -1");
    CHECK(somno_stage_export_csv_from_sst("/tmp/sst_csv_lim/x.sst", NULL) == -1,
          "export(NULL csv) != -1");

    /* A '/' at byte 512 is one past mk_parents' buffer: it must be skipped,
     * not copied (ASan reports the overflow if it is). */
    char longp[560];
    memset(longp, 'a', sizeof(longp));
    memcpy(longp, "/tmp/", 5);
    longp[512] = '/';
    memcpy(longp + 513, "x.csv", 6);
    (void)somno_stage_write_zeo_csv_file(longp, &h, st);
    free(st);
}

/* sst -> csv: a sidecar of exactly SST_MAX_EPOCHS exports, and a flagged
 * sidecar with a full body is still skipped. */
static void test_export_limits(void)
{
    const char *sst = "/tmp/sst_csv_sst2/in.sst";
    const char *csv = "/tmp/sst_csv_sst2/out.csv";
    mkdir("/tmp/sst_csv_sst2", 0775);
    const uint32_t n = 100000u;            /* SST_MAX_EPOCHS */
    uint8_t *body = malloc(2u * n);
    memset(body, 1, 2u * n);
    somno_sst_header_t h = mk_hdr(n, 30, 0);
    FILE *f = fopen(sst, "wb");
    CHECK(f != NULL, "cannot write fixture sst");
    if (f) { fwrite(&h, 1, sizeof(h), f); fwrite(body, 1, 2u * n, f); fclose(f); }
    CHECK(somno_stage_export_csv_from_sst(sst, csv) == 0,
          "an SST_MAX_EPOCHS sidecar was not exported (the limit is inclusive)");

    h = mk_hdr(20, 30, SOMNO_SST_FLAG_PARTIAL);
    f = fopen(sst, "wb");
    if (f) { fwrite(&h, 1, sizeof(h), f); fwrite(body, 1, 40, f); fclose(f); }
    unlink(csv);
    CHECK(somno_stage_export_csv_from_sst(sst, csv) == 1,
          "a PARTIAL sidecar with a full body was exported");
    free(body);
}

int main(void)
{
    /* The CSV embeds local-time timestamps; pin TZ so the run is identical
     * everywhere. */
    setenv("TZ", "UTC", 1);
    tzset();

    test_30s_identity();
    test_32s_gen1_resample();
    test_rejections();
    test_export_from_sst();
    test_onset_in_deep();
    test_sleep_graph_tail_and_deep();
    test_limits();
    test_export_limits();

    if (g_failures) {
        printf("somno_stage_csv_test: %d FAILED\n", g_failures);
        return 1;
    }
    printf("somno_stage_csv_test: all tests passed\n");
    return 0;
}
