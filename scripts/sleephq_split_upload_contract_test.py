#!/usr/bin/env python3
# SomnoTrace - SleepHQ interrupted-night upload contract test
# Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
#
# This file is part of SomnoTrace.
#
# SomnoTrace is free software: you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version.
#
# SomnoTrace is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
# A PARTICULAR PURPOSE. See the GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with
# this program. If not, see <https://www.gnu.org/licenses/>.
#
# ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
# attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
# (https://github.com/ilyakruchinin)." See the NOTICE file for details.

"""Contract test for the split-night upload fix (capture-hold gate).

The incident this guards against: a BLE supervision timeout split one night
into two session groups.  The first group uploaded in an early import; the
later import carried only the second group plus the root bundle, and
SleepHQ's day view lost the first fragment.

The fix is NOT a whole-day resend (that was the rejected atomic_day design).
Uploads stay incremental — only pending groups go out.  What changed is the
capture-hold gate in net_provision.c (upload_capture_hold, wired via
upload_sched_set_therapy_fn): while a therapy session is recording — or lost
BLE recently enough that it may still resume — scheduler passes defer, so
the fragments of one therapy leave together in a single import.  A parked
(permanently failing) group also revives once per UPLOAD_PARK_REVIVE_S so a
transient outage cannot strand a day.

Default mode is deterministic and touches no network: it models the
scheduler's group selection and the gate exactly as the C implements them
(run_backend's day loop + upload_capture_hold), drives both plausible
server-side reconcile semantics, and asserts the invariants above.  Static
source checks tie the model to the real code so a regression fails here.

--upload mode is opt-in and replays a real exported day directory
(--edf-dir, one containing two session groups and the root bundle) against
live SleepHQ in INCREMENTAL order — fragment 1 alone, then fragment 2 — the
sequence the gate exists to prevent mid-therapy and the residual paths
(>grace dropouts, reboots) can still produce.  It then asks the
machine-dates sessions API what the day contains: that is the open question
(Appendix N6) that decides whether a targeted sibling re-send is still
needed.  It creates account-side records; credentials come only from
SLEEPHQ_CLIENT_ID / SLEEPHQ_CLIENT_SECRET and the target machine from
SLEEPHQ_MACHINE_ID.  Nothing is printed or written to disk.
"""

import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

BASE = "https://sleephq.com"
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PARK_HEADER = os.path.join(REPO, "components", "uploader", "upload_park.h")


def _park_define(name):
    """Read a parking constant from upload_park.h, so a retune in C cannot
    leave this model on the old value.  The decision itself is tested
    against the real header by scripts/upload_park_test.c."""
    m = re.search(r"^#define\s+%s\s+(\d+)u?\b" % name,
                  open(PARK_HEADER).read(), re.M)
    if not m:
        sys.exit(f"FAIL: {name} not found in {PARK_HEADER}")
    return int(m.group(1))


UPLOAD_MAX_GROUP_ATTEMPTS = _park_define("UPLOAD_MAX_GROUP_ATTEMPTS")
UPLOAD_PARK_REVIVE_S = _park_define("UPLOAD_PARK_REVIVE_S")
EPOCH_SYNCED_S = _park_define("UPLOAD_CLOCK_SYNCED_S")
UPLOAD_THERAPY_GRACE_MS = 60 * 60 * 1000   # mirrors net_provision.c

# Standard ResMed group suffixes, matching kind_from_name() in upload_scan.c.
GROUP_SUFFIXES = ("BRP", "PLD", "SA2", "EVE", "CSL")


# ── Scheduler model ────────────────────────────────────────────────────
# Mirrors the group-selection loop in run_backend() (upload_sched.c) plus the
# capture-hold gate in run_pass() (s_therapy_fn).  If the C policy changes,
# the static source check below is meant to fail loudly enough that this
# model gets re-synced.

def parked(group, now_s, clock_ok=True):
    """upload_park_active(): parked until the parking itself goes stale.

    While the wall clock is unsynced the group stays parked.  A 1970-era
    last_try (failure before NTP) reads as ancient and revives at once —
    intentional, matching the C comment.  The age is uint32_t in C, so a
    last_try in the future (clock stepped back) wraps and revives."""
    if not (group["status"] == "failed"
            and group["attempts"] >= UPLOAD_MAX_GROUP_ATTEMPTS):
        return False
    if not clock_ok:
        return True
    return ((now_s - group["last_try"]) & 0xFFFFFFFF) < UPLOAD_PARK_REVIVE_S


def day_pending(groups, now_s, clock_ok=True):
    """run_backend only visits a day while a non-parked group is pending."""
    return any(g["status"] != "ok" and not parked(g, now_s, clock_ok)
               for g in groups)


def run_day(groups, now_s, clock_ok=True, fail_prefixes=()):
    """One scheduler pass over a day — incremental sends only.

    Returns the list of group prefixes put_group() was called for, or None
    when the day would not be visited at all.  Mutates group state exactly
    like the C loop: success -> 'ok', failure -> 'failed'; attempts is a
    saturating failure/send counter and last_try refreshes on every send."""
    if not day_pending(groups, now_s, clock_ok):
        return None
    sent = []
    for g in groups:
        if parked(g, now_s, clock_ok):
            continue
        if g["status"] == "ok":
            continue
        sent.append(g["prefix"])
        g["attempts"] = min(g["attempts"] + 1, 255)   # saturating, C-side
        g["last_try"] = now_s
        g["status"] = "failed" if g["prefix"] in fail_prefixes else "ok"
    return sent


class CaptureHold:
    """upload_capture_hold() in net_provision.c.

    recording   — sd_storage_recording_active()
    flag        — bsp_display_is_therapy_active() (can outlive a dropout)
    rec_end_ms  — sd_storage's timestamp of the last recording_end()
    """

    def __init__(self, grace_ms=UPLOAD_THERAPY_GRACE_MS):
        self.recording = False
        self.flag = False
        self.rec_end_ms = None
        self.grace_ms = grace_ms

    def recording_begin(self):
        self.recording = True

    def recording_end(self, now_ms):
        self.recording = False
        self.rec_end_ms = now_ms

    def therapy_stop(self, now_ms):
        self.recording_end(now_ms)
        self.flag = False

    def hold(self, now_ms):
        if self.recording:
            return True
        if not self.flag or self.rec_end_ms is None:
            return False
        return (now_ms - self.rec_end_ms) < self.grace_ms


def run_pass(groups, now_s, gate, now_ms, clock_ok=True, fail_prefixes=()):
    """run_pass(): the gate defers the whole pass; pending groups wait."""
    if gate.hold(now_ms):
        return None
    return run_day(groups, now_s, clock_ok, fail_prefixes)


def reconcile_file_change(group):
    """upload_scan_reconcile_day() resets state when a group's files change."""
    group["status"] = "pending"
    group["attempts"] = 0


# ── Server semantics models ────────────────────────────────────────────
# Which of these SleepHQ implements was never provable from the incident
# account.  The gate makes the incident sequence unreachable mid-therapy
# under EITHER model — that is the property asserted here.

class SleepHQDayView:
    """What SleepHQ shows for one calendar day after each processed import."""

    def __init__(self, semantics):
        assert semantics in ("replace", "append")
        self.semantics = semantics
        self.sessions = {}

    def process_import(self, sent_prefixes):
        if sent_prefixes is None:
            return
        if self.semantics == "replace":
            # The latest processed import defines the day.
            self.sessions = {p: True for p in sent_prefixes}
        else:
            self.sessions.update({p: True for p in sent_prefixes})


def make_group(prefix):
    return {"prefix": prefix, "status": "pending", "attempts": 0,
            "last_try": 0}


FRAG1, FRAG2 = "20990101_232245", "20990102_025729"


# ── Contract assertions ────────────────────────────────────────────────

def scenario_incremental_only():
    """An already-uploaded group is never re-sent on a later pass."""
    g1, g2 = make_group(FRAG1), make_group(FRAG2)
    groups = [g1]
    t = EPOCH_SYNCED_S
    sent1 = run_day(groups, t)
    groups.append(g2)
    sent2 = run_day(groups, t + 60)
    return sent1 == [FRAG1] and sent2 == [FRAG2]


def scenario_held_split(semantics):
    """The incident shape: split mid-therapy -> one import, both fragments."""
    day = SleepHQDayView(semantics)
    gate = CaptureHold()
    t_ms, t_s = 0, EPOCH_SYNCED_S

    gate.flag = True                    # TherapyStart
    gate.recording_begin()
    groups = [make_group(FRAG1)]

    # BLE drops >10 s: frag1 finalized "split", frag2 starts recording in the
    # same handler — the export completes while recording is live.
    gate.recording_end(t_ms)            # finalize("split")
    gate.recording_begin()              # frag2 starts, ~ms later
    day.process_import(run_pass(groups, t_s, gate, t_ms + 1))

    groups.append(make_group(FRAG2))
    t_ms += 4 * 3600 * 1000
    gate.therapy_stop(t_ms)             # real TherapyStop
    day.process_import(run_pass(groups, t_s + 4 * 3600, gate, t_ms + 1))
    return set(day.sessions)


def scenario_long_dropout_in_grace(semantics):
    """Dropout past the stale timeout but inside the grace window."""
    day = SleepHQDayView(semantics)
    gate = CaptureHold()
    t_ms, t_s = 0, EPOCH_SYNCED_S

    gate.flag = True
    gate.recording_begin()
    groups = [make_group(FRAG1)]

    # 10 min of silence -> "timed_out"; recording flag ends, display flag
    # stays (no stop event ever arrived).
    gate.recording_end(t_ms)
    held = gate.hold(t_ms + 30 * 60 * 1000)     # +30 min: still inside grace
    day.process_import(run_pass(groups, t_s + 30 * 60, gate,
                                t_ms + 30 * 60 * 1000))

    # BLE returns at +30 min: flow auto-start opens frag2.
    gate.recording_begin()
    groups.append(make_group(FRAG2))
    gate.therapy_stop(t_ms + 2 * 3600 * 1000)
    day.process_import(run_pass(groups, t_s + 2 * 3600, gate,
                                t_ms + 2 * 3600 * 1000 + 1))
    return held, set(day.sessions)


def scenario_beyond_grace_releases():
    """AS11 gone for good: the gate must release, not stall forever."""
    gate = CaptureHold()
    t_ms = 0
    gate.flag = True
    gate.recording_begin()
    gate.recording_end(t_ms)                    # timed_out finalize
    return not gate.hold(t_ms + UPLOAD_THERAPY_GRACE_MS + 1)


def scenario_parked_revive():
    """A parked group retries once per window — and not while unsynced."""
    groups = [make_group(FRAG1)]
    t = EPOCH_SYNCED_S
    for _ in range(UPLOAD_MAX_GROUP_ATTEMPTS):
        run_day(groups, t, fail_prefixes={FRAG1})
        t += 60
    if not parked(groups[0], t):
        return "group not parked after max attempts"
    if run_day(groups, t + 3600) is not None:
        return "parked group sent inside the revive window"
    sent = run_day(groups, t + UPLOAD_PARK_REVIVE_S + 1)
    if sent != [FRAG1]:
        return "parked group did not revive after the window"
    # Unsynced clock: never revives on its own.
    g2 = make_group(FRAG2)
    g2["status"], g2["attempts"], g2["last_try"] = "failed", 9, 0
    if not parked(g2, now_s=1000, clock_ok=False):
        return "unsynced clock revived a parked group"
    # And a file change still revives immediately.
    reconcile_file_change(groups[0])
    groups[0]["status"], groups[0]["attempts"] = "failed", UPLOAD_MAX_GROUP_ATTEMPTS
    reconcile_file_change(groups[0])
    if run_day(groups, t + 2 * UPLOAD_PARK_REVIVE_S) != [FRAG1]:
        return "file-change reset did not revive the group"
    # Clock stepped backward: last_try is in the future.  C's unsigned age
    # wraps and revives (one early send) rather than trusting it.
    g3 = make_group(FRAG2)
    g3["status"], g3["attempts"], g3["last_try"] = \
        "failed", UPLOAD_MAX_GROUP_ATTEMPTS, t + 3600
    if parked(g3, t):
        return "clock stepped back kept the group parked"
    return None


def check_source_flags():
    """Tie the model to the real code."""
    failures = []

    def read(rel):
        return open(os.path.join(REPO, rel)).read()

    sched = read("components/uploader/upload_sched.c")
    hdr = read("components/uploader/uploader.h")
    shq = read("components/uploader/uploader_sleephq.c")
    smb = read("components/uploader/uploader_smb.c")
    prov = read("main/net_provision.c")
    sdh = read("main/sd_storage.h")
    park = read("components/uploader/upload_park.h")

    # The whole-day resend design is gone — there is no consumer left.
    for name, text in (("uploader.h", hdr), ("upload_sched.c", sched),
                       ("uploader_sleephq.c", shq), ("uploader_smb.c", smb)):
        if "atomic_day" in text:
            failures.append(f"{name} still references atomic_day")

    for needle, where, what in (
            ("upload_park_active(", sched, "scheduler uses the shared park rule"),
            ("upload_park_active(", park, "staleness-aware park check"),
            ("UPLOAD_PARK_REVIVE_S", park, "parked-revive window"),
            ("UPLOAD_MAX_GROUP_ATTEMPTS", park, "attempt cap"),
            ("upload_capture_hold", prov, "capture-hold hook"),
            ("upload_sched_set_therapy_fn(upload_capture_hold)", prov,
             "gate wiring"),
            ("sd_storage_ms_since_recording_end", sdh,
             "recording-end helper declaration"),
            ("sd_storage_ms_since_recording_end", prov,
             "recording-end helper use")):
        if needle not in where:
            failures.append(f"missing {what}: '{needle}'")
    return failures


def run_mock_contract():
    failures = check_source_flags()

    # Incremental sends only — the efficiency invariant.
    if not scenario_incremental_only():
        failures.append("an already-uploaded group was re-sent")

    # The held-split scenario must produce ONE complete-day import under
    # either server model — the incident outcome at zero extra bytes.
    for sem in ("replace", "append"):
        visible = scenario_held_split(sem)
        missing = {FRAG1, FRAG2} - visible
        if missing:
            failures.append(f"held split under {sem} lost {sorted(missing)}")

    # Long dropout inside grace: gate held mid-gap, both fragments together.
    for sem in ("replace", "append"):
        held, visible = scenario_long_dropout_in_grace(sem)
        if not held:
            failures.append(f"grace window did not hold under {sem}")
        if {FRAG1, FRAG2} - visible:
            failures.append(f"long dropout under {sem} lost fragments")

    # The gate must release once the grace window has passed.
    if not scenario_beyond_grace_releases():
        failures.append("gate held past the grace window — AS11-gone stall")

    f = scenario_parked_revive()
    if f:
        failures.append(f)

    return failures


# ── Optional live replay (the N6 experiment) ───────────────────────────
# Requires --edf-dir with a real exported day folder: two session groups
# (YYYYMMDD_HHMMSS_*.edf), STR.edf and Identification.*.  Replays the
# INCREMENTAL sequence — frag1 + bundle, then frag2 + bundle — and asks
# SleepHQ what the day contains.  If fragment 1 is still visible, the gate
# alone suffices; if not, the day needs the sibling re-send from App. P3.
# Note: STR.edf in the folder already covers both sessions, so import 1 is
# "frag1 + full-day STR" — close to, but not identical to, the incident's
# frag1-only STR.

def request(method, path, token=None, body=None, content_type=None):
    headers = {"Accept": "application/vnd.api+json",
               "User-Agent": "SomnoTrace-contract-test/1.0"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    if body is not None:
        headers["Content-Length"] = str(len(body))
        if content_type:
            headers["Content-Type"] = content_type
    req = urllib.request.Request(BASE + path, data=body, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=30) as response:
            return response.status, json.loads(response.read())
    except urllib.error.HTTPError as error:
        payload = error.read().decode("utf-8", "replace")
        try:
            payload = json.loads(payload)
        except json.JSONDecodeError:
            payload = {"error": payload[:400]}
        return error.code, payload


def multipart(fields, filename, payload):
    boundary = "----SomnoTraceContractBoundary"
    chunks = []
    for name, value in fields.items():
        chunks.extend((f"--{boundary}\r\n",
                       f'Content-Disposition: form-data; name="{name}"\r\n\r\n',
                       str(value), "\r\n"))
    chunks.extend((f"--{boundary}\r\n",
                   f'Content-Disposition: form-data; name="file"; filename="{filename}"\r\n',
                   "Content-Type: application/octet-stream\r\n\r\n"))
    body = "".join(chunks).encode() + payload + f"\r\n--{boundary}--\r\n".encode()
    return body, f"multipart/form-data; boundary={boundary}"


def shq_upload_file(token, import_id, name, path, payload):
    import hashlib
    digest = hashlib.md5(name.encode() + payload).hexdigest()
    body, ctype = multipart({"import_id": import_id, "name": name,
                             "path": path, "content_hash": digest},
                            name, payload)
    return request("POST", f"/api/v1/imports/{import_id}/files",
                   token=token, body=body, content_type=ctype)


def run_live(edf_dir):
    import hashlib
    client_id = os.environ.get("SLEEPHQ_CLIENT_ID")
    client_secret = os.environ.get("SLEEPHQ_CLIENT_SECRET")
    machine_id = os.environ.get("SLEEPHQ_MACHINE_ID")
    if not client_id or not client_secret or not machine_id:
        print("SLEEPHQ_CLIENT_ID, SLEEPHQ_CLIENT_SECRET and SLEEPHQ_MACHINE_ID "
              "are required", file=sys.stderr)
        return 2

    groups = {}
    bundle = []
    for name in sorted(os.listdir(edf_dir)):
        m = re.match(r"^(\d{8}_\d{6})_(BRP|PLD|SA2|EVE|CSL)\.edf$", name)
        if m:
            groups.setdefault(m.group(1), []).append(name)
        elif name in ("STR.edf", "Identification.json", "Identification.crc"):
            bundle.append(name)
    if len(groups) < 2 or "STR.edf" not in bundle:
        print("--edf-dir must contain >=2 session groups and STR.edf",
              file=sys.stderr)
        return 2
    prefixes = sorted(groups)
    day = prefixes[0][:8]

    form = urllib.parse.urlencode(
        {"grant_type": "password", "client_id": client_id,
         "client_secret": client_secret, "scope": "read write"}).encode()
    status, auth = request("POST", "/oauth/token", body=form,
                           content_type="application/x-www-form-urlencoded")
    if status < 200 or status >= 300 or "access_token" not in auth:
        print(f"authentication failed: HTTP {status}", file=sys.stderr)
        return 1
    token = auth["access_token"]

    status, me = request("GET", "/api/v1/me", token=token)
    team = (me.get("data", {}).get("attributes", {}) or {}).get("current_team_id")
    if status != 200 or team is None:
        print(f"team discovery failed: HTTP {status}", file=sys.stderr)
        return 1

    def send_import(prefixes_to_send):
        status, created = request("POST", f"/api/v1/teams/{team}/imports",
                                  token=token)
        import_id = (created.get("data", {}) or {}).get("id")
        if status != 201 or import_id is None:
            print(f"import creation failed: HTTP {status}", file=sys.stderr)
            return None
        names = [n for p in prefixes_to_send for n in groups[p]] + bundle
        for name in names:
            with open(os.path.join(edf_dir, name), "rb") as fh:
                payload = fh.read()
            sub = "/SETTINGS" if name.startswith("SETTINGS") else f"/DATALOG/{day}"
            status, _ = shq_upload_file(token, import_id, name, sub, payload)
            if status != 201:
                print(f"file {name} failed: HTTP {status}", file=sys.stderr)
                return None
        status, _ = request("POST", f"/api/v1/imports/{import_id}/process_files",
                            token=token)
        if status < 200 or status >= 300:
            print(f"process_files failed: HTTP {status}", file=sys.stderr)
            return None
        for _ in range(30):
            status, result = request("GET", f"/api/v1/imports/{import_id}",
                                     token=token)
            state = ((result.get("data", {}) or {}).get("attributes", {}) or {}
                     ).get("status")
            if state in ("complete", "completed"):
                return import_id
            if state in ("failed", "error"):
                print(f"import {import_id} failed processing", file=sys.stderr)
                return None
            time.sleep(2)
        print(f"import {import_id} timed out", file=sys.stderr)
        return None

    # The incremental sequence: frag1 alone, then frag2 alone — what the
    # residual paths can still produce.  Assert both stay visible.
    if send_import(prefixes[:1]) is None:
        return 1
    second = send_import(prefixes[1:])
    if second is None:
        return 1

    status, sessions = request(
        "GET", f"/api/v1/machine_dates/{machine_id}/{day}/sessions",
        token=token)
    if status != 200:
        print(f"machine_dates query failed: HTTP {status}", file=sys.stderr)
        return 1
    seen = set()
    for item in sessions.get("data", []) if isinstance(sessions, dict) else []:
        attrs = item.get("attributes", {}) or {}
        seen.add(str(attrs.get("start_time", attrs.get("file_name", item.get("id")))))
    print(json.dumps({"day": day, "expected_prefixes": prefixes,
                      "sessions_seen": len(sessions.get("data", [])),
                      "import2": second}))
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--upload", action="store_true",
                        help="replay --edf-dir incrementally against live "
                             "SleepHQ (the N6 experiment)")
    parser.add_argument("--edf-dir",
                        help="exported day folder for --upload (two session "
                             "groups + STR.edf)")
    args = parser.parse_args()

    if args.upload:
        if not args.edf_dir:
            print("--upload requires --edf-dir", file=sys.stderr)
            return 2
        return run_live(args.edf_dir)

    failures = run_mock_contract()
    if failures:
        for f in failures:
            print(f"FAIL: {f}")
        return 1
    print(">>> SPLIT-UPLOAD CONTRACT TESTS PASSED <<<")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
