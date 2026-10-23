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
 *   phase C  unit-id validation at its edges: a state file whose id is one
 *            byte too long for any scanned CSV is refused at load, the longest
 *            legal id is accepted; a CSV id with a space (0x20) is tracked, one
 *            with a control byte (0x1f) is not
 *   phase D  a unit's day is fixed once known: a CSV that moves to another day
 *            folder is evicted and re-offered, it does not inherit the mark
 *   phase E  what reaches disk: only backends with an outcome, only registered
 *            backends, and a failed rename leaves no .tmp and keeps the unit
 *            dirty until a later flush succeeds
 *   phase F  reboot with PSRAM exhausted: unit files still load through the
 *            plain-malloc fallback
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

#include "cJSON.h"
#include "upload_stage.h"

/* ── Backend registry stub (upload_index.c is not linked) ──────────── */

static const char *const k_backends[] = { "smb", "sleephq", "ftp", "ha" };
#define N_BACKENDS ((int)(sizeof(k_backends) / sizeof(k_backends[0])))
#define SLOT_SMB 0
#define SLOT_SHQ 1

/* How many backends are registered this boot.  The real registry
 * (upload_index.c) can hold fewer than UPLOAD_MAX_BACKENDS, and its
 * upload_index_backend_name() answers "?" — not NULL — for a slot past the
 * registered count; the stub mirrors both so phase E can register three. */
static int g_registered = N_BACKENDS;

int upload_index_backend_slot(const char *backend_id)
{
    if (!backend_id) return -1;
    for (int i = 0; i < g_registered; i++)
        if (strcmp(backend_id, k_backends[i]) == 0) return i;
    return -1;
}

const char *upload_index_backend_name(int slot)
{
    return (slot >= 0 && slot < g_registered) ? k_backends[slot] : "?";
}

int upload_index_backend_count(void) { return g_registered; }

/* esp_heap_caps.h shim hook: non-zero makes heap_caps_malloc() fail. */
int host_heap_caps_malloc_fail;

/* ── Harness ────────────────────────────────────────────────────────── */

static int g_failures;
#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg));          \
            g_failures++;                                                     \
        }                                                                     \
    } while (0)

#define STAGE_DIR  SD_MOUNT_POINT "/.somnotrace/stages"
#define STATE_DIR  UPLOAD_STATE_DIR "/stage"

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

/* Fresh card + state tree for a phase that must not see earlier phases. */
static void reset_tree(void)
{
    rmtree(SD_MOUNT_POINT);
    rmtree(UPLOAD_STATE_DIR);
    mkdirs(STAGE_DIR);
    mkdirs(UPLOAD_STATE_DIR);
}

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

/* The "be" object of a unit's state file as the component wrote it. */
static cJSON *read_unit_file(const char *id)
{
    char p[896], buf[4096];
    snprintf(p, sizeof(p), "%s/%s.json", STATE_DIR, id);
    FILE *f = fopen(p, "r");
    if (!f) return NULL;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return cJSON_Parse(buf);
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
    int r = 0;
    static const char *const k_phases[] = { "b", "c", "d", "e" };
    for (size_t i = 0; i < sizeof(k_phases) / sizeof(k_phases[0]); i++) {
        int pr = run_phase(self, k_phases[i]);
        if (pr) printf("  FAIL phase %s exited %d\n", k_phases[i], pr);
        r += pr;
    }
    return g_failures + r;
}

/* ── Phase B: reboot — reload, fingerprint reset, eviction, window ── */

static int phase_b(void)
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
    return g_failures;
}

/* ── Phase C: unit-id validation at both edges ─────────────────────── */

static int phase_c(void)
{
    reset_tree();
    mkdirs(STATE_DIR);

    /* A scanned CSV id is at most UPLOAD_STAGE_ID_LEN - 1 bytes (it has to
     * fit the NUL-terminated id field), so a state file naming a longer id
     * can only be corrupt and must be refused.  The longest legal id loads. */
    char too_long[UPLOAD_STAGE_ID_LEN + 1], longest[UPLOAD_STAGE_ID_LEN];
    memset(too_long, 'a', UPLOAD_STAGE_ID_LEN); too_long[UPLOAD_STAGE_ID_LEN] = '\0';
    memset(longest, 'b', UPLOAD_STAGE_ID_LEN - 1); longest[UPLOAD_STAGE_ID_LEN - 1] = '\0';
    char p[896], json[512];
    snprintf(p, sizeof(p), "%s/too_long.json", STATE_DIR);
    snprintf(json, sizeof(json), "{\"v\":1,\"id\":\"%s\",\"day\":\"20261101\",\"fp\":\"1\"}", too_long);
    write_text(p, json);
    snprintf(p, sizeof(p), "%s/%s.json", STATE_DIR, longest);
    snprintf(json, sizeof(json), "{\"v\":1,\"id\":\"%s\",\"day\":\"20261102\",\"fp\":\"1\"}", longest);
    write_text(p, json);

    CHECK(upload_stage_init() == ESP_OK, "upload_stage_init failed");
    CHECK(upload_stage_cached_pending(SLOT_SMB) == 1,
          "load must refuse an id of UPLOAD_STAGE_ID_LEN bytes and keep one of LEN-1");
    int done = -1, total = -1;
    upload_stage_backend_progress(SLOT_SMB, WINDOW, &done, &total);
    CHECK(total == 1, "an over-long state id was loaded as a day of its own");

    /* Ids are path components: a space is a legal byte, anything below
     * 0x20 is not. */
    mk_stage_csv("20261103", "night 1", 1);
    mk_stage_csv("20261103", "night\x1f" "2", 2);
    upload_stage_ref_t *refs = calloc(UPLOAD_STAGE_MAX_UNITS, sizeof(*refs));
    if (!refs) return 1;
    int kept = upload_stage_reconcile(refs, UPLOAD_STAGE_MAX_UNITS, WINDOW);
    CHECK(find_ref(refs, kept, "night 1") != NULL, "a CSV id containing a space was not tracked");
    CHECK(find_ref(refs, kept, "night\x1f" "2") == NULL, "a CSV id with a control byte was tracked");
    CHECK(kept == 1, "reconcile kept something other than the one valid CSV");
    free(refs);
    return g_failures;
}

/* ── Phase D: a CSV that changes day folder is a new upload ──────────── */

static int phase_d(void)
{
    reset_tree();
    mk_stage_csv("20261104", "moved", 7);
    CHECK(upload_stage_init() == ESP_OK, "upload_stage_init failed");

    upload_stage_ref_t *refs = calloc(UPLOAD_STAGE_MAX_UNITS, sizeof(*refs));
    if (!refs) return 1;
    int kept = upload_stage_reconcile(refs, UPLOAD_STAGE_MAX_UNITS, WINDOW);
    CHECK(kept == 1, "the one CSV was not scanned");
    if (kept == 1) upload_stage_mark(&refs[0], SLOT_SMB, UG_OK, NULL);
    CHECK(kept == 1 && upload_stage_status(&refs[0], SLOT_SMB) == UG_OK, "mark not recorded");

    /* Same bytes, same name, another day folder.  The state recorded for
     * 20261104 describes a CSV that is gone; the unit's day is only ever
     * filled in when it was missing, never rewritten. */
    char from[896], to[896], dir[768];
    unit_csv(from, sizeof(from), "20261104", "moved");
    snprintf(dir, sizeof(dir), "%s/20261105", STAGE_DIR);
    mkdirs(dir);
    unit_csv(to, sizeof(to), "20261105", "moved");
    CHECK(rename(from, to) == 0, "could not move the CSV");

    kept = upload_stage_reconcile(refs, UPLOAD_STAGE_MAX_UNITS, WINDOW);
    const upload_stage_ref_t *r = find_ref(refs, kept, "moved");
    CHECK(r && !strcmp(r->day, "20261105"), "moved CSV not scanned under its new day");
    CHECK(r && upload_stage_status(r, SLOT_SMB) != UG_OK,
          "a CSV moved to another day inherited the old day's upload mark");
    CHECK(upload_stage_pending(refs, kept, SLOT_SMB) == 1, "moved CSV not re-offered");
    free(refs);
    return g_failures;
}

/* ── Phase E: what the unit file contains, and a failed rename ───────── */

static int phase_e(const char *self)
{
    reset_tree();
    g_registered = 3;          /* smb, sleephq, ftp — "ha" is not registered */
    mk_stage_csv("20261106", "u1", 1);
    mk_stage_csv("20261106", "u2", 2);
    CHECK(upload_stage_init() == ESP_OK, "upload_stage_init failed");

    upload_stage_ref_t *refs = calloc(UPLOAD_STAGE_MAX_UNITS, sizeof(*refs));
    if (!refs) return 1;
    int kept = upload_stage_reconcile(refs, UPLOAD_STAGE_MAX_UNITS, WINDOW);
    CHECK(kept == 2, "both CSVs not scanned");
    const upload_stage_ref_t *u1 = find_ref(refs, kept, "u1");
    const upload_stage_ref_t *u2 = find_ref(refs, kept, "u2");
    if (!u1 || !u2) { free(refs); return g_failures + 1; }

    /* Only a backend with an outcome is written; slot 3 is inside
     * UPLOAD_MAX_BACKENDS but past the registered count, so it has no name
     * to be written under and must not reach disk as "?". */
    upload_stage_mark(u1, SLOT_SMB, UG_OK, NULL);
    upload_stage_mark(u1, 3, UG_FAILED, NULL);
    cJSON *root = read_unit_file("u1");
    cJSON *be = root ? cJSON_GetObjectItem(root, "be") : NULL;
    CHECK(cJSON_IsObject(be), "u1 unit file has no backend object");
    CHECK(be && cJSON_GetObjectItem(be, "smb") != NULL, "the marked backend was not written");
    CHECK(be && cJSON_GetObjectItem(be, "sleephq") == NULL && cJSON_GetObjectItem(be, "ftp") == NULL,
          "untouched backends were written as pending");
    CHECK(be && cJSON_GetObjectItem(be, "?") == NULL,
          "an unregistered slot was written under the registry's placeholder name");
    CHECK(be && cJSON_GetArraySize(be) == 1, "unit file holds backends other than the marked one");
    cJSON_Delete(root);

    /* A directory where u2's state file belongs: unlink() cannot clear it
     * and rename() onto it fails.  The temp file must be cleaned up, and the
     * unit stays dirty so the next flush writes it once the path is free. */
    char sp[896], tmp[896];
    snprintf(sp, sizeof(sp), "%s/u2.json", STATE_DIR);
    snprintf(tmp, sizeof(tmp), "%s/u2.json.tmp", STATE_DIR);
    mkdirs(sp);
    upload_stage_mark(u2, SLOT_SMB, UG_OK, NULL);
    CHECK(!exists(tmp), "a failed rename left the .tmp file behind");
    CHECK(upload_stage_status(u2, SLOT_SMB) == UG_OK, "mark lost from RAM after a failed write");
    rmdir(sp);
    CHECK(!exists(sp), "test setup: blocker directory still present");
    upload_stage_reconcile(refs, UPLOAD_STAGE_MAX_UNITS, WINDOW);
    root = read_unit_file("u2");
    CHECK(root != NULL, "a unit whose save failed was not re-flushed by the next reconcile");
    cJSON_Delete(root);

    free(refs);
    int r = run_phase(self, "f");
    if (r) printf("  FAIL phase f exited %d\n", r);
    return g_failures + r;
}

/* ── Phase F: reboot with heap_caps_malloc() failing ─────────────────── */

static int phase_f(void)
{
    g_registered = 3;
    host_heap_caps_malloc_fail = 1;   /* PSRAM exhausted: plain-malloc fallback */
    CHECK(upload_stage_init() == ESP_OK, "upload_stage_init failed");
    host_heap_caps_malloc_fail = 0;

    upload_stage_ref_t *refs = calloc(UPLOAD_STAGE_MAX_UNITS, sizeof(*refs));
    if (!refs) return 1;
    int kept = upload_stage_reconcile(refs, UPLOAD_STAGE_MAX_UNITS, WINDOW);
    CHECK(kept == 2, "reboot reconcile lost units");
    CHECK(upload_stage_pending(refs, kept, SLOT_SMB) == 0,
          "marks did not load when heap_caps_malloc() failed");
    free(refs);
    return g_failures;
}

int main(int argc, char **argv)
{
    const char *phase = argc > 1 ? argv[1] : "a";
    int r;
    if      (!strcmp(phase, "a")) r = phase_a(argv[0]);
    else if (!strcmp(phase, "b")) r = phase_b();
    else if (!strcmp(phase, "c")) r = phase_c();
    else if (!strcmp(phase, "d")) r = phase_d();
    else if (!strcmp(phase, "e")) r = phase_e(argv[0]);
    else if (!strcmp(phase, "f")) r = phase_f();
    else r = 1;

    if (r) { printf("upload_stage_test: phase %s FAILED (%d)\n", phase, r); return 1; }
    if (!strcmp(phase, "a")) printf("upload_stage_test: all phases passed\n");
    return 0;
}
