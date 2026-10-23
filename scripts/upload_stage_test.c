/*
 * SomnoTrace - Host unit tests for the SomnoStage CSV upload-state store
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

/* Stage CSVs are the third independent upload stream (after CPAP days and
 * oximetry recordings): discovered under SD_STAGES_DIR/<day>/<unit>.csv,
 * persisted one JSON file per unit under upload_state/stage/, keyed by
 * content fingerprint so a rescore re-uploads automatically.  This suite
 * pins:
 *
 *   phase A  scan + reconcile find the exports, every unit is pending, a
 *            mark is durable in RAM *and* on disk, and upload_stage_clear()
 *            (#317 parity) wipes BOTH so a reconcile re-offers everything
 *   phase B  fresh process = reboot: marks load from unit files; a changed
 *            CSV fingerprint resets only that unit; deleting the CSV evicts
 *            its state; the max_days window bounds pending/progress counts
 *
 * Phases are separate exec'd processes because s_states is static — a reboot
 * has to lose it for the reload path to be exercised.
 *
 * Compile-time roots: -DSD_MOUNT_POINT and -DUPLOAD_STATE_DIR point the
 * component at the scratch tree below.  upload_stage.c is compiled in
 * directly with the backend registry stubbed here (same pattern as
 * upload_ox_test.c). */

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "upload_stage.h"

/* ── Backend registry stub (upload_index.c is not linked) ──────────── */

static const char *const k_backends[] = {
    "smb", "sleephq", "aerivue", "breathingroom", "future_sst", "future_csv"
};
#define N_BACKENDS ((int)(sizeof(k_backends) / sizeof(k_backends[0])))
#define SLOT_SMB     0
#define SLOT_SHQ     1
#define SLOT_AER     2
#define SLOT_BR      3
#define SLOT_FUT_SST 4
#define SLOT_FUT_CSV 5

int upload_index_backend_slot(const char *backend_id)
{
    if (!backend_id) return -1;
    for (int i = 0; i < N_BACKENDS; i++)
        if (strcmp(backend_id, k_backends[i]) == 0) return i;
    return -1;
}

const char *upload_index_backend_name(int slot)
{
    return (slot >= 0 && slot < N_BACKENDS) ? k_backends[slot] : NULL;
}

int upload_index_backend_count(void) { return N_BACKENDS; }

/* ── Harness ────────────────────────────────────────────────────────── */

static int g_failures;
#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg));          \
            g_failures++;                                                     \
        }                                                                     \
    } while (0)

#define STAGE_DIR   SD_MOUNT_POINT "/.somnotrace/stages"
#define STREAMS_DIR SD_MOUNT_POINT "/.somnotrace/sessions/streams"
#define OX_REC_DIR  SD_MOUNT_POINT "/.somnotrace/oximetry/recordings"
#define STATE_DIR   UPLOAD_STATE_DIR "/stage"

/* 12 units, 2 per day across days 20261001..20261006 — day folders are
 * scanned newest-first, so unit ordering inside a day does not matter. */
#define N_UNITS 12
#define N_DAYS  6
#define WINDOW  45

static void mkdirs(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0775); *p = '/'; }
    }
    mkdir(tmp, 0775);
}

static void rmtree(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    char sub[1024];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
        struct stat st;
        if (lstat(sub, &st) == 0 && S_ISDIR(st.st_mode)) rmtree(sub);
        else unlink(sub);
    }
    closedir(d);
    rmdir(path);
}

static void unit_id(int i, char out[UPLOAD_STAGE_ID_LEN])
{
    /* Odd-numbered units pretend to be AS11 exports; the id shape does not
     * matter to the tracker, only that it is unique per file. */
    snprintf(out, UPLOAD_STAGE_ID_LEN, i % 3 == 0 ? "as11_%06d" : "%014d",
             20261001 * 100 + i);
}

static void unit_day(int i, char out[12])
{
    snprintf(out, 12, "%08d", 20261001 + i % N_DAYS);
}

static void unit_csv(char *out, size_t n, const char *day, const char *id)
{
    snprintf(out, n, "%s/%s/%s.csv", STAGE_DIR, day, id);
}

/* A Zeo-shaped line; the byte content is what the fingerprint sees. */
static void mk_stage_csv(const char *day, const char *id, int fill)
{
    char dir[768], p[896];
    snprintf(dir, sizeof(dir), "%s/%s", STAGE_DIR, day);
    mkdirs(dir);
    unit_csv(p, sizeof(p), day, id);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "Start of Night,Sleep Graph\n\"row\",%d\n", fill);
    fclose(f);
}

static int exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static int state_files(void)
{
    DIR *d = opendir(STATE_DIR);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (strstr(e->d_name, ".json")) n++;
    closedir(d);
    return n;
}

static const upload_stage_ref_t *find_ref(const upload_stage_ref_t *refs,
                                          int n, const char *id)
{
    for (int i = 0; i < n; i++)
        if (strcmp(refs[i].unit_id, id) == 0) return &refs[i];
    return NULL;
}

static int run_phase(const char *self, const char *phase)
{
    char *resolved = realpath(self, NULL);
    if (!resolved) { perror("realpath"); return 1; }
    pid_t pid = fork();
    if (pid == 0) {
        execl(resolved, resolved, phase, (char *)NULL);
        _exit(127);
    }
    free(resolved);
    int st = 0;
    if (pid < 0 || waitpid(pid, &st, 0) < 0) return 1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

/* ── Phase A: seed + mark + the #317 clear path ────────────────────── */

static int phase_a(const char *self)
{
    char base[768];
    snprintf(base, sizeof(base), "%s", SD_MOUNT_POINT);
    rmtree(base);
    snprintf(base, sizeof(base), "%s", UPLOAD_STATE_DIR);
    rmtree(base);

    for (int i = 0; i < N_UNITS; i++) {
        char id[UPLOAD_STAGE_ID_LEN], day[12];
        unit_id(i, id); unit_day(i, day);
        mk_stage_csv(day, id, i);
    }
    mkdirs(UPLOAD_STATE_DIR);   /* parents, as sd_storage_init makes on-card */

    CHECK(upload_stage_init() == ESP_OK, "upload_stage_init failed");

    upload_stage_ref_t *refs = calloc(UPLOAD_STAGE_MAX_UNITS, sizeof(*refs));
    if (!refs) return 1;
    int kept = upload_stage_reconcile(refs, UPLOAD_STAGE_MAX_UNITS, WINDOW);
    CHECK(kept == N_UNITS, "reconcile kept fewer than the 12 stage CSVs");
    CHECK(upload_stage_pending(refs, kept, SLOT_SMB) == N_UNITS,
          "fresh stage CSVs not all pending");

    int done = -1, total = -1;
    upload_stage_backend_progress(SLOT_SMB, WINDOW, &done, &total);
    CHECK(done == 0 && total == N_DAYS,
          "progress does not report 0/6 days for a fresh tree");

    for (int i = 0; i < kept; i++)
        upload_stage_mark(&refs[i], SLOT_SMB, UG_OK, NULL);

    CHECK(upload_stage_pending(refs, kept, SLOT_SMB) == 0,
          "marked units still read pending in RAM");
    CHECK(upload_stage_cached_pending(SLOT_SMB) == 0,
          "cached pending non-zero after marking all");
    CHECK(state_files() == N_UNITS, "unit files not persisted");
    upload_stage_backend_progress(SLOT_SMB, WINDOW, &done, &total);
    CHECK(done == N_DAYS && total == N_DAYS,
          "progress does not report 6/6 days after marking all");

    /* #317 parity: Reset must clear disk AND RAM — and the next reconcile
     * must re-offer every CSV still on the card. */
    CHECK(upload_stage_clear() == ESP_OK, "upload_stage_clear failed");
    CHECK(state_files() == 0, "clear left unit files on disk");
    CHECK(upload_stage_cached_pending(SLOT_SMB) == 0,
          "in-memory list not cleared");
    kept = upload_stage_reconcile(refs, UPLOAD_STAGE_MAX_UNITS, WINDOW);
    CHECK(kept == N_UNITS, "post-clear reconcile lost CSVs");
    CHECK(upload_stage_pending(refs, kept, SLOT_SMB) == N_UNITS,
          "cleared units did not come back pending");
    CHECK(upload_stage_cached_pending(SLOT_SMB) == N_UNITS,
          "cleared units not re-offered in cached pending");

    /* Restore the all-OK state for the reboot phase. */
    for (int i = 0; i < kept; i++)
        upload_stage_mark(&refs[i], SLOT_SMB, UG_OK, NULL);
    CHECK(upload_stage_cached_pending(SLOT_SMB) == 0,
          "re-marked units still pending");

    free(refs);
    int r = run_phase(self, "b");
    return g_failures + r;
}

/* ── Phase B: reboot — reload, fingerprint reset, eviction, window ── */

static int phase_b(const char *self)
{
    CHECK(upload_stage_init() == ESP_OK, "upload_stage_init failed");

    upload_stage_ref_t *refs = calloc(UPLOAD_STAGE_MAX_UNITS, sizeof(*refs));
    if (!refs) return 1;
    int kept = upload_stage_reconcile(refs, UPLOAD_STAGE_MAX_UNITS, WINDOW);
    CHECK(kept == N_UNITS, "reboot reconcile lost units");
    CHECK(upload_stage_pending(refs, kept, SLOT_SMB) == 0,
          "uploaded marks did not survive a reboot");

    /* A rescored CSV (changed content -> changed fingerprint) resets exactly
     * that unit, for every backend.  Put it on the OLDEST day so the window
     * check below also proves the unit drops out of view. */
    char id[UPLOAD_STAGE_ID_LEN], day[12], p[896];
    unit_id(2, id);            /* i=2 lands on day 20261003 */
    unit_day(2, day);
    unit_csv(p, sizeof(p), day, id);
    FILE *f = fopen(p, "w");
    if (f) { fputs("\"rescored\",999\n", f); fclose(f); }

    kept = upload_stage_reconcile(refs, UPLOAD_STAGE_MAX_UNITS, WINDOW);
    const upload_stage_ref_t *r2 = find_ref(refs, kept, id);
    CHECK(r2 && upload_stage_status(r2, SLOT_SMB) != UG_OK,
          "changed CSV did not reset its unit to pending");
    CHECK(upload_stage_cached_pending(SLOT_SMB) == 1,
          "exactly one unit should be pending after a single rescore");

    /* Deleting the CSV must evict its state file. */
    char id5[UPLOAD_STAGE_ID_LEN], day5[12];
    unit_id(5, id5); unit_day(5, day5);
    unit_csv(p, sizeof(p), day5, id5);
    unlink(p);
    kept = upload_stage_reconcile(refs, UPLOAD_STAGE_MAX_UNITS, WINDOW);
    CHECK(kept == N_UNITS - 1, "deleted CSV still scanned");
    {
        char sp[896];
        snprintf(sp, sizeof(sp), "%s/%s.json", STATE_DIR, id5);
        CHECK(!exists(sp), "state file not evicted for a deleted CSV");
    }

    /* Windowing: only the newest day folder counts toward pending/progress.
     * max_days=1 keeps day 20261006 — unit 5 was deleted above, so only
     * unit 11 remains.  The pending rescored unit sits on 20261003 —
     * outside the window — and must not inflate the pending badge. */
    kept = upload_stage_reconcile(refs, UPLOAD_STAGE_MAX_UNITS, 1);
    CHECK(kept == 1, "max_days=1 did not keep exactly the one visible unit");
    CHECK(upload_stage_cached_pending(SLOT_SMB) == 0,
          "out-of-window pending unit leaked into the cached count");
    int done = -1, total = -1;
    upload_stage_backend_progress(SLOT_SMB, 1, &done, &total);
    CHECK(total == 1, "windowed progress total is not the one visible unit");
    CHECK(done == 1, "windowed progress done wrong");

    /* status() itself is fingerprint-gated: a ref whose fingerprint no
     * longer matches the recorded state must not claim OK even while the
     * unit's stored mark says uploaded. */
    char id1[UPLOAD_STAGE_ID_LEN];
    unit_id(11, id1);
    const upload_stage_ref_t *r1 = find_ref(refs, kept, id1);
    CHECK(r1 && upload_stage_status(r1, SLOT_SMB) == UG_OK,
          "untouched unit lost its uploaded mark in the windowed reconcile");
    if (r1) {
        upload_stage_ref_t stale = *r1;
        stale.fingerprint ^= 0x5a5a;
        CHECK(upload_stage_status(&stale, SLOT_SMB) != UG_OK,
              "stale fingerprint still reads uploaded");
    }

    free(refs);
    if (g_failures) return g_failures;
    return run_phase(self, "c");
}

/* ── Phase C & D: Multi-backend CSV + native SST1 tracking ─────────── */

static void wr_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void wr_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static void wr_i64_le(uint8_t *p, int64_t v)
{
    uint64_t u = (uint64_t)v;
    wr_u32_le(p, (uint32_t)(u & 0xffffffffu));
    wr_u32_le(p + 4, (uint32_t)(u >> 32));
}

static void write_sst_fixture(const char *path, uint8_t flags,
                              uint32_t n_epochs, uint32_t epoch_sec,
                              uint8_t stage_val, uint8_t conf_val,
                              int truncate_bytes)
{
    uint8_t hdr[64];
    memset(hdr, 0, sizeof(hdr));
    wr_u32_le(hdr + 0, 0x31545353u);   /* "SST1" */
    wr_u16_le(hdr + 4, 1);             /* version 1 */
    hdr[6] = 2;                        /* decode_mode */
    hdr[7] = flags;
    strcpy((char *)(hdr + 8), "v4.2.3-test");
    hdr[32] = 0;
    wr_u32_le(hdr + 36, n_epochs);
    wr_i64_le(hdr + 40, 1792680265000LL);
    wr_u32_le(hdr + 48, epoch_sec);

    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(hdr, 1, sizeof(hdr), f);
    for (uint32_t i = 0; i < n_epochs; i++) {
        uint8_t s = stage_val;
        fwrite(&s, 1, 1, f);
    }
    uint32_t conf_count = n_epochs;
    if (truncate_bytes > 0 && (uint32_t)truncate_bytes <= conf_count)
        conf_count -= (uint32_t)truncate_bytes;
    for (uint32_t i = 0; i < conf_count; i++) {
        uint8_t c = conf_val;
        fwrite(&c, 1, 1, f);
    }
    fclose(f);
}

static void mk_as11_sst(const char *day, const char *prefix, uint8_t flags,
                        uint32_t n_epochs, uint32_t epoch_sec,
                        uint8_t stage_val, uint8_t conf_val, int truncate_bytes)
{
    char dir[768], path[896];
    snprintf(dir, sizeof(dir), "%s/%s", STREAMS_DIR, day);
    mkdirs(dir);
    snprintf(path, sizeof(path), "%s/%s_stages.sst", dir, prefix);
    write_sst_fixture(path, flags, n_epochs, epoch_sec, stage_val, conf_val,
                      truncate_bytes);
}

static void mk_ring_sst(const char *day, const char *rec_id, int active_gen,
                        uint8_t flags, uint32_t n_epochs, uint32_t epoch_sec,
                        uint8_t stage_val, uint8_t conf_val)
{
    char rdir[768], gdir[840], rj[840], sst[896];
    snprintf(rdir, sizeof(rdir), "%s/%s/%s", OX_REC_DIR, day, rec_id);
    snprintf(gdir, sizeof(gdir), "%s/generations/%d/data", rdir, active_gen);
    mkdirs(gdir);
    snprintf(rj, sizeof(rj), "%s/recording.json", rdir);
    FILE *f = fopen(rj, "w");
    if (f) {
        fprintf(f, "{\"recording_id\":\"%s\",\"active_generation\":%d,\"state\":\"ready\"}",
                rec_id, active_gen);
        fclose(f);
    }
    snprintf(sst, sizeof(sst), "%s/stages.sst", gdir);
    write_sst_fixture(sst, flags, n_epochs, epoch_sec, stage_val, conf_val, 0);
}

static void bind_test_formats(void)
{
    upload_stage_set_backend_fmt(SLOT_SMB,     UPLOAD_STAGE_FMT_CSV);
    upload_stage_set_backend_fmt(SLOT_AER,     UPLOAD_STAGE_FMT_SST);
    upload_stage_set_backend_fmt(SLOT_BR,      UPLOAD_STAGE_FMT_SST);
    upload_stage_set_backend_fmt(SLOT_FUT_SST, UPLOAD_STAGE_FMT_SST);
    upload_stage_set_backend_fmt(SLOT_FUT_CSV, UPLOAD_STAGE_FMT_CSV);
}

static int phase_c(const char *self)
{
    char base[768];
    snprintf(base, sizeof(base), "%s", SD_MOUNT_POINT);
    rmtree(base);
    snprintf(base, sizeof(base), "%s", UPLOAD_STATE_DIR);
    rmtree(base);
    mkdirs(UPLOAD_STATE_DIR);

    bind_test_formats();
    CHECK(upload_stage_get_backend_fmt(SLOT_SMB) == UPLOAD_STAGE_FMT_CSV,
          "SLOT_SMB format should be CSV");
    CHECK(upload_stage_get_backend_fmt(SLOT_AER) == UPLOAD_STAGE_FMT_SST,
          "SLOT_AER format should be SST");
    CHECK(upload_stage_get_backend_fmt(SLOT_BR) == UPLOAD_STAGE_FMT_SST,
          "SLOT_BR format should be SST");
    CHECK(upload_stage_get_backend_fmt(SLOT_FUT_SST) == UPLOAD_STAGE_FMT_SST,
          "SLOT_FUT_SST (slot 4) format should be SST");

    /* Unit 1 (day 20261020): ring recording with BOTH CSV and valid 32s Gen1 SST */
    mk_stage_csv("20261020", "20261020223000", 10);
    mk_ring_sst("20261020", "20261020223000", 2, 0x02 /* MOTION_USED */, 40, 32, 1, 200);

    /* Unit 2 (day 20261021): AS11 session with BOTH CSV and valid 30s SST */
    mk_stage_csv("20261021", "as11_20261021_230000", 20);
    mk_as11_sst("20261021", "20261021_230000", 0, 60, 30, 2, 180, 0);

    /* Unit 3 (day 20261021): SST-only ring recording (no CSV in stages/) */
    mk_ring_sst("20261021", "20261021235500", 1, 0x02, 50, 30, 3, 210);

    /* Invalid SSTs that MUST be ignored (INSUFFICIENT, PARTIAL, truncated, all-0xFF) */
    mk_as11_sst("20261021", "20261021_010000", 0x04 /* INSUFFICIENT */, 50, 30, 0xFF, 0, 0);
    mk_as11_sst("20261021", "20261021_020000", 0x01 /* PARTIAL */, 50, 30, 1, 150, 0);
    mk_as11_sst("20261021", "20261021_030000", 0, 50, 30, 1, 150, 10 /* truncated */);
    mk_as11_sst("20261021", "20261021_040000", 0, 50, 30, 0xFF /* no stage */, 150, 0);

    CHECK(upload_stage_init() == ESP_OK, "phase_c init failed");

    upload_stage_ref_t *refs = calloc(UPLOAD_STAGE_MAX_UNITS, sizeof(*refs));
    if (!refs) return 1;
    int kept = upload_stage_reconcile(refs, UPLOAD_STAGE_MAX_UNITS, WINDOW);
    CHECK(kept == 3, "expected exactly 3 valid stage units (2 CSV+SST, 1 SST-only)");

    /* CSV backends see 2 pending units (Unit 1 and Unit 2); SST backends see all 3. */
    CHECK(upload_stage_pending(refs, kept, SLOT_SMB) == 2,
          "SMB (CSV) should see 2 pending units");
    CHECK(upload_stage_cached_pending(SLOT_SMB) == 2,
          "SMB cached pending should be 2");
    CHECK(upload_stage_pending(refs, kept, SLOT_AER) == 3,
          "Aerivue (SST) should see 3 pending units");
    CHECK(upload_stage_cached_pending(SLOT_AER) == 3,
          "Aerivue cached pending should be 3");
    CHECK(upload_stage_pending(refs, kept, SLOT_BR) == 3,
          "BreathingRoom (SST) should see 3 pending units");
    CHECK(upload_stage_pending(refs, kept, SLOT_FUT_SST) == 3,
          "5th backend (SST) should see 3 pending units");

    /* Check SST filename helper */
    const upload_stage_ref_t *ru1 = find_ref(refs, kept, "20261020223000");
    char fname[80];
    upload_stage_sst_filename(ru1, fname, sizeof(fname));
    CHECK(strcmp(fname, "20261020223000_stages.sst") == 0,
          "unexpected SST filename for ring unit");

    /* Mark SMB (CSV) and Aerivue (SST) OK on all units; leave BreathingRoom
     * and Future-SST pending, then mark them OK too to test independent resets. */
    for (int i = 0; i < kept; i++) {
        if (upload_stage_ref_has_fmt(&refs[i], UPLOAD_STAGE_FMT_CSV)) {
            upload_stage_mark(&refs[i], SLOT_SMB, UG_OK, NULL);
            upload_stage_mark(&refs[i], SLOT_FUT_CSV, UG_OK, NULL);
        }
        if (upload_stage_ref_has_fmt(&refs[i], UPLOAD_STAGE_FMT_SST)) {
            upload_stage_mark(&refs[i], SLOT_AER, UG_OK, "imp-aer");
        }
    }
    CHECK(upload_stage_cached_pending(SLOT_SMB) == 0, "SMB should be 0 pending");
    CHECK(upload_stage_cached_pending(SLOT_AER) == 0, "Aerivue should be 0 pending");
    CHECK(upload_stage_cached_pending(SLOT_BR) == 3,
          "BreathingRoom must remain 3 pending when only Aerivue uploaded");

    for (int i = 0; i < kept; i++) {
        if (upload_stage_ref_has_fmt(&refs[i], UPLOAD_STAGE_FMT_SST)) {
            upload_stage_mark(&refs[i], SLOT_BR, UG_OK, "imp-br");
            upload_stage_mark(&refs[i], SLOT_FUT_SST, UG_OK, "imp-fut");
        }
    }
    CHECK(upload_stage_cached_pending(SLOT_BR) == 0, "BreathingRoom should be 0 pending");
    CHECK(upload_stage_cached_pending(SLOT_FUT_SST) == 0, "Future SST should be 0 pending");

    free(refs);
    int r = run_phase(self, "d");
    return g_failures + r;
}

static int phase_d(void)
{
    bind_test_formats();
    CHECK(upload_stage_init() == ESP_OK, "phase_d init failed");

    upload_stage_ref_t *refs = calloc(UPLOAD_STAGE_MAX_UNITS, sizeof(*refs));
    if (!refs) return 1;
    int kept = upload_stage_reconcile(refs, UPLOAD_STAGE_MAX_UNITS, WINDOW);
    CHECK(kept == 3, "phase_d reboot reconcile should keep 3 units");
    CHECK(upload_stage_cached_pending(SLOT_SMB) == 0, "SMB marks lost across reboot");
    CHECK(upload_stage_cached_pending(SLOT_AER) == 0, "Aerivue marks lost across reboot");
    CHECK(upload_stage_cached_pending(SLOT_BR) == 0, "BreathingRoom marks lost across reboot");
    CHECK(upload_stage_cached_pending(SLOT_FUT_SST) == 0, "Future SST marks lost across reboot");
    CHECK(upload_stage_cached_pending(SLOT_FUT_CSV) == 0, "Future CSV marks lost across reboot");

    /* Modify ONLY the SST of Unit 1 (e.g. confidence changed from 200 to 220).
     * SST backends (Aerivue, BreathingRoom, Future-SST) MUST reset to pending
     * for Unit 1, while CSV backends (SMB, Future-CSV) MUST stay UG_OK! */
    mk_ring_sst("20261020", "20261020223000", 2, 0x02, 40, 32, 1, 220);
    kept = upload_stage_reconcile(refs, UPLOAD_STAGE_MAX_UNITS, WINDOW);
    CHECK(upload_stage_cached_pending(SLOT_SMB) == 0,
          "SST change falsely invalidated SMB (CSV)");
    CHECK(upload_stage_cached_pending(SLOT_FUT_CSV) == 0,
          "SST change falsely invalidated Future-CSV");
    CHECK(upload_stage_cached_pending(SLOT_AER) == 1,
          "SST change did not reset Aerivue for Unit 1");
    CHECK(upload_stage_cached_pending(SLOT_BR) == 1,
          "SST change did not reset BreathingRoom for Unit 1");
    CHECK(upload_stage_cached_pending(SLOT_FUT_SST) == 1,
          "SST change did not reset Future-SST for Unit 1");

    /* Re-mark SST backends OK on Unit 1, then modify ONLY the CSV of Unit 2.
     * CSV backends MUST reset to pending for Unit 2, while SST backends stay OK! */
    const upload_stage_ref_t *ru1 = find_ref(refs, kept, "20261020223000");
    CHECK(ru1 != NULL, "Unit 1 missing");
    if (ru1) {
        upload_stage_mark(ru1, SLOT_AER, UG_OK, NULL);
        upload_stage_mark(ru1, SLOT_BR, UG_OK, NULL);
        upload_stage_mark(ru1, SLOT_FUT_SST, UG_OK, NULL);
    }
    mk_stage_csv("20261021", "as11_20261021_230000", 999);
    kept = upload_stage_reconcile(refs, UPLOAD_STAGE_MAX_UNITS, WINDOW);
    CHECK(upload_stage_cached_pending(SLOT_SMB) == 1,
          "CSV change did not reset SMB for Unit 2");
    CHECK(upload_stage_cached_pending(SLOT_FUT_CSV) == 1,
          "CSV change did not reset Future-CSV for Unit 2");
    CHECK(upload_stage_cached_pending(SLOT_AER) == 0,
          "CSV change falsely invalidated Aerivue (SST)");
    CHECK(upload_stage_cached_pending(SLOT_BR) == 0,
          "CSV change falsely invalidated BreathingRoom (SST)");
    CHECK(upload_stage_cached_pending(SLOT_FUT_SST) == 0,
          "CSV change falsely invalidated Future-SST (SST)");

    free(refs);
    return g_failures;
}

int main(int argc, char **argv)
{
    const char *phase = argc > 1 ? argv[1] : "a";
    int r;
    if      (!strcmp(phase, "a")) r = phase_a(argv[0]);
    else if (!strcmp(phase, "b")) r = phase_b(argv[0]);
    else if (!strcmp(phase, "c")) r = phase_c(argv[0]);
    else if (!strcmp(phase, "d")) r = phase_d();
    else r = 1;

    if (r) { printf("upload_stage_test: phase %s FAILED (%d)\n", phase, r); return 1; }
    if (!strcmp(phase, "d")) printf("upload_stage_test: all phases passed\n");
    return 0;
}
