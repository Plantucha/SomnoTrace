/*
 * SomnoTrace - Host unit tests for the oximetry upload-state store
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

/* Regression test for the 64-slot state-table overflow: with 65 recordings on
 * the card, the 65th never got a slot, so its successful SleepHQ upload mark
 * was silently dropped and every scheduler pass re-uploaded it.  The store is
 * now a grow-on-demand list persisted as one file per recording — the same
 * model as the CPAP day index — and this suite pins the semantics:
 *
 *   phase A  seed 70 recordings (> old cap), mark all uploaded, prove the
 *            mark is durable in RAM *and* on disk
 *   phase B  fresh process = reboot: state must load from unit files, a
 *            changed fingerprint must reset only that unit, and deleting a
 *            recording must evict its state file
 *   phase C  fresh process again: the legacy oximetry.json must migrate into
 *            per-recording files exactly once
 *
 * Phases are separate exec'd processes because s_states is static — a reboot
 * has to lose it for the reload path to be exercised.
 *
 * Compile-time roots: -DSD_MOUNT_POINT and -DUPLOAD_STATE_DIR point the
 * component at the scratch tree below.  upload_ox.c is compiled in directly
 * (the suite's usual pattern) with the backend registry stubbed here. */

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "upload_ox.h"

/* ── Backend registry stub (upload_index.c is not linked) ──────────── */

static const char *const k_backends[] = { "smb", "sleephq", "ftp", "ha" };
#define N_BACKENDS ((int)(sizeof(k_backends) / sizeof(k_backends[0])))
#define SLOT_SHQ 1

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

#define REC_DIR   SD_MOUNT_POINT "/.somnotrace/oximetry/recordings"
#define OX_DIR    UPLOAD_STATE_DIR "/ox"
#define LEGACY    UPLOAD_STATE_DIR "/oximetry.json"
#define LEGACY_OK UPLOAD_STATE_DIR "/oximetry.json.migrated"

#define N_RECS 70   /* deliberately past the old fixed table's 64 */
#define WINDOW 45

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

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}

/* Minimal canonical recording: recording.json (state=ready, gen 1),
 * source/source.bin filled with `fill`, generations/1/manifest.json. */
static void mk_recording(const char *day, const char *id, unsigned char fill)
{
    char dir[768], p[896], buf[256];
    snprintf(dir, sizeof(dir), "%s/%s/%s", REC_DIR, day, id);
    snprintf(p, sizeof(p), "%s/source", dir);       mkdirs(p);
    snprintf(p, sizeof(p), "%s/generations/1", dir); mkdirs(p);

    snprintf(p, sizeof(p), "%s/recording.json", dir);
    snprintf(buf, sizeof(buf),
             "{\"recording_id\":\"%s\",\"active_generation\":1,"
             "\"state\":\"ready\",\"uploadable\":true}\n", id);
    write_text(p, buf);

    snprintf(p, sizeof(p), "%s/source/source.bin", dir);
    FILE *f = fopen(p, "wb");
    if (f) {
        unsigned char blob[64];
        memset(blob, fill, sizeof(blob));
        fwrite(blob, 1, sizeof(blob), f);
        fclose(f);
    }

    snprintf(p, sizeof(p), "%s/generations/1/manifest.json", dir);
    snprintf(buf, sizeof(buf),
             "{\"source\":[{\"original_name\":\"%s.bin\"}]}\n", id);
    write_text(p, buf);
}

/* 70 recordings, 4 per day across days 20260801..20260818 (4/day, N_RECS/4
 * day folders — all inside the 45-day window). */
static void rec_id(int i, char out_id[16], char out_day[12])
{
    snprintf(out_day, 12, "%08d", 20260801 + i / 4);
    snprintf(out_id, 16, "%s%06d", out_day, (i % 4) * 10000);
}

static int exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void unit_path(char *out, size_t n, const char *id)
{
    snprintf(out, n, "%s/%s.json", OX_DIR, id);
}

static const upload_ox_ref_t *find_ref(const upload_ox_ref_t *refs, int n,
                                       const char *id)
{
    for (int i = 0; i < n; i++)
        if (strcmp(refs[i].recording_id, id) == 0) return &refs[i];
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

/* ── Phase A: seed + mark ──────────────────────────────────────────── */

static int phase_a(const char *self)
{
    char base[768];
    snprintf(base, sizeof(base), "%s", SD_MOUNT_POINT);
    rmtree(base);                       /* whole SD mount root            */
    snprintf(base, sizeof(base), "%s", UPLOAD_STATE_DIR);
    rmtree(base);                       /* whole upload_state tree        */

    for (int i = 0; i < N_RECS; i++) {
        char id[16], day[12];
        rec_id(i, id, day);
        mk_recording(day, id, (unsigned char)(0x40 + i));
    }
    /* init() does plain mkdir() for OX_DIR, relying on sd_storage_init
     * having made the parents on the real card — create them here. */
    mkdirs(UPLOAD_STATE_DIR);

    CHECK(upload_ox_init() == ESP_OK, "upload_ox_init failed");

    upload_ox_ref_t *refs = calloc(UPLOAD_OX_MAX_UNITS, sizeof(*refs));
    if (!refs) return 1;
    int kept = upload_ox_reconcile(refs, UPLOAD_OX_MAX_UNITS, WINDOW);
    CHECK(kept == N_RECS, "reconcile kept fewer than the 70 recordings");
    CHECK(upload_ox_pending(refs, kept, SLOT_SHQ) == N_RECS,
          "fresh recordings not all pending");

    for (int i = 0; i < kept; i++)
        upload_ox_mark(&refs[i], SLOT_SHQ, UG_OK, "imp-1");

    CHECK(upload_ox_pending(refs, kept, SLOT_SHQ) == 0,
          "marked recordings still read pending in RAM");
    CHECK(upload_ox_cached_pending(SLOT_SHQ) == 0,
          "cached pending non-zero after marking all");

    /* The bug this test exists for: unit #65+ must have a state file too. */
    char id[16], day[12], p[896];
    rec_id(N_RECS - 1, id, day);
    unit_path(p, sizeof(p), id);
    CHECK(exists(p), "state file missing for the 70th recording — the "
                     "old fixed table would have dropped its mark");

    /* Backends must be written by NAME so registration order can't renumber
     * existing state (the CPAP day-file convention). */
    FILE *f = fopen(p, "r");
    char body[1024] = {0};
    if (f) { size_t rd = fread(body, 1, sizeof(body) - 1, f); (void)rd; fclose(f); }
    CHECK(strstr(body, "\"sleephq\":{\"s\":\"ok\"") != NULL,
          "unit file does not record the backend by name");

    free(refs);
    int r = run_phase(self, "b");
    return g_failures + r;
}

/* ── Phase B: reboot — reload, fingerprint reset, eviction ─────────── */

static int phase_b(const char *self)
{
    CHECK(upload_ox_init() == ESP_OK, "upload_ox_init failed");

    upload_ox_ref_t *refs = calloc(UPLOAD_OX_MAX_UNITS, sizeof(*refs));
    if (!refs) return 1;
    int kept = upload_ox_reconcile(refs, UPLOAD_OX_MAX_UNITS, WINDOW);
    CHECK(kept == N_RECS, "reboot reconcile lost recordings");
    CHECK(upload_ox_pending(refs, kept, SLOT_SHQ) == 0,
          "uploaded marks did not survive a reboot");
    CHECK(upload_ox_cached_pending(SLOT_SHQ) == 0,
          "cached pending non-zero after reboot");

    /* A changed fingerprint must reset exactly that unit for all backends. */
    char id3[16], day[12], p[896];
    rec_id(3, id3, day);
    snprintf(p, sizeof(p), "%s/%s/%s/source/source.bin", REC_DIR, day, id3);
    write_text(p, "changed content — re-converted recording");
    kept = upload_ox_reconcile(refs, UPLOAD_OX_MAX_UNITS, WINDOW);
    const upload_ox_ref_t *r3 = find_ref(refs, kept, id3);
    CHECK(r3 && upload_ox_status(r3, SLOT_SHQ) != UG_OK,
          "changed content did not reset the recording to pending");
    CHECK(upload_ox_cached_pending(SLOT_SHQ) == 1,
          "exactly one unit should be pending after a single fingerprint change");

    /* Deleting the recording must evict its state file. */
    char id5[16];
    rec_id(5, id5, day);
    snprintf(p, sizeof(p), "%s/%s/%s", REC_DIR, day, id5);
    rmtree(p);
    unit_path(p, sizeof(p), id5);
    kept = upload_ox_reconcile(refs, UPLOAD_OX_MAX_UNITS, WINDOW);
    CHECK(kept == N_RECS - 1, "deleted recording still scanned");
    CHECK(!exists(p), "state file not evicted for a deleted recording");
    {
        upload_ox_ref_t dead = {0};
        strcpy(dead.recording_id, id5);
        dead.generation = 1;
        CHECK(upload_ox_status(&dead, SLOT_SHQ) != UG_OK,
              "deleted recording still reads uploaded");
    }

    /* Plant a legacy single-file state for phase C.  One unit belongs to rec
     * #7 — still on the card, fingerprint copied from the state file just
     * written — so migration must preserve its OK mark.  The other belongs to
     * a recording long deleted: with no day to check, only the init-time
     * orphan pass can evict it, which is the point. */
    char id7[16], ubuf[1024] = {0}, fpbuf[24] = "0";
    rec_id(7, id7, day);
    unit_path(p, sizeof(p), id7);
    FILE *lf = fopen(p, "r");
    CHECK(lf != NULL, "unit file missing for fp harvest");
    if (lf) { size_t rd = fread(ubuf, 1, sizeof(ubuf) - 1, lf); (void)rd; fclose(lf); }
    const char *fpkey = strstr(ubuf, "\"fp\":\"");
    CHECK(fpkey != NULL, "no fingerprint in written unit file");
    if (fpkey) snprintf(fpbuf, sizeof(fpbuf), "%.16s", fpkey + 6);
    unlink(p);    /* drop the new-format file so migration is what restores it */

    mkdirs(UPLOAD_STATE_DIR);
    char legacy[1024];
    snprintf(legacy, sizeof(legacy),
        "{\"units\":["
        "{\"recording_id\":\"%s\",\"generation\":1,\"fingerprint\":\"%s\","
         "\"backends\":[{\"slot\":1,\"status\":\"ok\",\"attempts\":0,"
         "\"last_try_s\":1700000000}]},"
        "{\"recording_id\":\"19990101000000\",\"generation\":1,"
         "\"fingerprint\":\"000000000000beef\",\"backends\":"
         "[{\"slot\":1,\"status\":\"ok\",\"last_try_s\":1700000000}]}]}\n",
        id7, fpbuf);
    write_text(LEGACY, legacy);

    free(refs);
    int r = run_phase(self, "c");
    return g_failures + r;
}

/* ── Phase C: reboot — legacy migration ────────────────────────────── */

static int phase_c(void)
{
    CHECK(upload_ox_init() == ESP_OK, "upload_ox_init failed");

    CHECK(!exists(LEGACY), "legacy oximetry.json left in place");
    CHECK(exists(LEGACY_OK), "migrated legacy file not renamed aside");

    /* The unit whose recording survived: migrated, day resolved, mark kept. */
    char id7[16], day[12], p[896];
    rec_id(7, id7, day);
    unit_path(p, sizeof(p), id7);
    CHECK(exists(p), "migrated unit file not written");
    upload_ox_ref_t legacy = {0};
    strcpy(legacy.recording_id, id7);
    legacy.generation = 1;
    CHECK(upload_ox_status(&legacy, SLOT_SHQ) == UG_OK,
          "migrated unit lost its uploaded mark");
    CHECK(upload_ox_status(&legacy, 0) != UG_OK,
          "migrated unit claims a backend it never had");

    /* The unit whose recording is gone: evicted by the orphan pass rather
     * than left pending forever. */
    upload_ox_ref_t gone = {0};
    strcpy(gone.recording_id, "19990101000000");
    gone.generation = 1;
    CHECK(upload_ox_status(&gone, SLOT_SHQ) != UG_OK,
          "orphaned legacy unit was not evicted");
    CHECK(!exists(OX_DIR "/19990101000000.json"),
          "orphaned legacy unit left a state file");

    /* No phantom states: only the fp-changed unit is pending now. */
    upload_ox_ref_t *refs = calloc(UPLOAD_OX_MAX_UNITS, sizeof(*refs));
    if (!refs) return 1;
    int kept = upload_ox_reconcile(refs, UPLOAD_OX_MAX_UNITS, WINDOW);
    CHECK(kept == N_RECS - 1, "post-migration reconcile count wrong");
    CHECK(upload_ox_pending(refs, kept, SLOT_SHQ) == 1,
          "pending set drifted — expected only the changed unit");
    CHECK(upload_ox_status(&legacy, SLOT_SHQ) == UG_OK,
          "migrated mark reset by reconcile despite unchanged content");
    free(refs);
    return g_failures;
}

int main(int argc, char **argv)
{
    const char *phase = argc > 1 ? argv[1] : "a";
    int r;
    if      (!strcmp(phase, "a")) r = phase_a(argv[0]);
    else if (!strcmp(phase, "b")) r = phase_b(argv[0]);
    else if (!strcmp(phase, "c")) r = phase_c();
    else r = 1;

    if (r) { printf("upload_ox_test: phase %s FAILED (%d)\n", phase, r); return 1; }
    if (!strcmp(phase, "c")) printf("upload_ox_test: all phases passed\n");
    return 0;
}
