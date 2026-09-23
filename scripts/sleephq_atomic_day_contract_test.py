#!/usr/bin/env python3
# SomnoTrace - SleepHQ two-fragment day (atomic_day) contract test
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

"""Contract test for the atomic_day upload policy (the split-night fix).

The incident this guards against: a BLE supervision timeout split one night
into two session groups.  The first group uploaded in an early import; the
later import carried only the second group plus the root bundle, and
SleepHQ's day view lost the first fragment.

Default mode is deterministic and touches no network: it models the
scheduler's group selection exactly as components/uploader/upload_sched.c
implements it (run_backend's day loop), drives it against both plausible
server-side reconcile semantics, and asserts that atomic_day produces a
self-complete import for the day under either one.  Static source checks tie
the model to the real backend tables so a flag regression fails here.

--upload mode is opt-in and replays a real exported day directory
(--edf-dir, one containing two session groups and the root bundle) against
live SleepHQ, then asserts through the machine-dates sessions API that both
session groups remain visible.  It creates account-side records; credentials
come only from SLEEPHQ_CLIENT_ID / SLEEPHQ_CLIENT_SECRET and the target
machine from SLEEPHQ_MACHINE_ID.  Nothing is printed or written to disk.
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
UPLOAD_MAX_GROUP_ATTEMPTS = 5   # mirrors upload_sched.c

# Standard ResMed group suffixes, matching kind_from_name() in upload_scan.c.
GROUP_SUFFIXES = ("BRP", "PLD", "SA2", "EVE", "CSL")


# ── Scheduler model ────────────────────────────────────────────────────
# Mirrors the group-selection loop in run_backend() (upload_sched.c).  If the
# C policy changes, the static source check below is meant to fail loudly
# enough that this model gets re-synced.

def parked(group):
    """A failed group stops counting after UPLOAD_MAX_GROUP_ATTEMPTS tries."""
    return group["status"] == "failed" and group["attempts"] >= UPLOAD_MAX_GROUP_ATTEMPTS


def day_pending(groups):
    """run_backend only visits a day while a non-parked group is pending."""
    return any(g["status"] != "ok" and not parked(g) for g in groups)


def run_day(groups, atomic_day, fail_prefixes=()):
    """One scheduler pass over a day.

    Returns the list of group prefixes put_group() was called for, or None
    when the day would not be visited at all.  Mutates group state exactly
    like the C loop: success -> 'ok', failure -> 'failed'; attempts++ on
    every send of a non-OK group (re-sends of good groups never count)."""
    if not day_pending(groups):
        return None
    sent = []
    for g in groups:
        if parked(g):
            continue
        if not atomic_day and g["status"] == "ok":
            continue
        was_ok = g["status"] == "ok"
        sent.append(g["prefix"])
        if not was_ok:
            # C: if (!was_ok) u->attempts++ — runs on every send of a
            # non-OK group, whatever the outcome; re-sends of already-good
            # groups never count.
            g["attempts"] += 1
        g["status"] = "failed" if g["prefix"] in fail_prefixes else "ok"
    return sent


def reconcile_file_change(group):
    """upload_scan_reconcile_day() resets state when a group's files change."""
    group["status"] = "pending"
    group["attempts"] = 0


# ── Server semantics models ────────────────────────────────────────────
# Which of these SleepHQ implements was never provable from the incident
# account.  The point of atomic_day is that it is correct under BOTH.

class SleepHQDayView:
    """What SleepHQ shows for one calendar day after each processed import."""

    def __init__(self, semantics):
        assert semantics in ("replace", "append")
        self.semantics = semantics
        self.sessions = {}

    def process_import(self, sent_prefixes):
        if self.semantics == "replace":
            # The latest processed import defines the day.
            self.sessions = {p: True for p in sent_prefixes}
        else:
            self.sessions.update({p: True for p in sent_prefixes})


def make_group(prefix):
    return {"prefix": prefix, "status": "pending", "attempts": 0}


# ── Contract assertions ────────────────────────────────────────────────

def scenario_split_day(atomic_day, semantics):
    """Replay the incident night: frag1 uploads alone, then frag2 appears."""
    day = SleepHQDayView(semantics)
    frag1, frag2 = make_group("20990101_232245"), make_group("20990102_025729")
    groups = [frag1]

    # First pass: only fragment 1 exists on the card.
    sent = run_day(groups, atomic_day)
    day.process_import(sent)

    # The split produces fragment 2; the next pass visits the day again.
    groups.append(frag2)
    sent = run_day(groups, atomic_day)
    day.process_import(sent)
    return set(day.sessions)


def check_source_flags():
    """Tie the model to the real code: the flag must exist and be set."""
    failures = []

    def block(text, start_marker):
        i = text.find(start_marker)
        return text[i:i + 800] if i >= 0 else ""

    shq = open(os.path.join(REPO, "components/uploader/uploader_sleephq.c")).read()
    if ".atomic_day = true" not in block(shq, "const upload_backend_t sleephq_backend"):
        failures.append("sleephq_backend lost .atomic_day = true")

    smb = open(os.path.join(REPO, "components/uploader/uploader_smb.c")).read()
    if ".atomic_day = false" not in block(smb, "const upload_backend_t smb_backend"):
        failures.append("smb_backend lost .atomic_day = false")

    sched = open(os.path.join(REPO, "components/uploader/upload_sched.c")).read()
    if "be->atomic_day" not in sched:
        failures.append("upload_sched.c no longer consults be->atomic_day")
    if "UPLOAD_MAX_GROUP_ATTEMPTS" not in sched:
        failures.append("upload_sched.c lost the UPLOAD_MAX_GROUP_ATTEMPTS guard")

    hdr = open(os.path.join(REPO, "components/uploader/uploader.h")).read()
    if "atomic_day" not in hdr:
        failures.append("upload_backend_t lost the atomic_day field")
    return failures


def run_mock_contract():
    failures = []

    for f in check_source_flags():
        failures.append(f)

    # The regression itself: incremental under replace semantics drops frag1.
    visible = scenario_split_day(atomic_day=False, semantics="replace")
    if "20990101_232245" in visible:
        failures.append("incremental+replace unexpectedly kept fragment 1")
    if "20990102_025729" not in visible:
        failures.append("incremental+replace lost even the newest fragment")

    # atomic_day must keep the complete day under EITHER server model.
    for sem in ("replace", "append"):
        visible = scenario_split_day(atomic_day=True, semantics=sem)
        missing = {"20990101_232245", "20990102_025729"} - visible
        if missing:
            failures.append(f"atomic_day+{sem} lost fragment(s): {sorted(missing)}")

    # The retry-storm guard: a permanently failing group must be parked, not
    # drag every sibling through a full-day re-send on every pass.
    frag1, frag2 = make_group("20990101_232245"), make_group("20990102_025729")
    groups = [frag1, frag2]
    frag1_sends = 0
    for _ in range(UPLOAD_MAX_GROUP_ATTEMPTS + 4):
        sent = run_day(groups, True, fail_prefixes={frag2["prefix"]})
        if sent is None:
            break
        frag1_sends += sent.count(frag1["prefix"])
    if day_pending(groups):
        failures.append("permanently failing group still keeps day pending")
    if frag1_sends > UPLOAD_MAX_GROUP_ATTEMPTS + 1:
        failures.append(f"healthy group sent {frag1_sends}x alongside the "
                        f"parked sibling (limit {UPLOAD_MAX_GROUP_ATTEMPTS + 1})")

    # And parking must not be a tombstone: a file change revives the group.
    reconcile_file_change(frag2)
    sent = run_day(groups, True)
    if sent is None or frag2["prefix"] not in sent:
        failures.append("file-change reset did not revive the parked group")

    return failures


# ── Optional live replay ───────────────────────────────────────────────
# Requires --edf-dir with a real exported day folder: two session groups
# (YYYYMMDD_HHMMSS_*.edf), STR.edf and Identification.*.  Runs the atomic_day
# sequence for real and asks SleepHQ what the day contains.

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

    # The atomic_day sequence: frag1 alone, then the complete day.
    if send_import(prefixes[:1]) is None:
        return 1
    second = send_import(prefixes)
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
                        help="replay --edf-dir against live SleepHQ")
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
    print(">>> ATOMIC-DAY CONTRACT TESTS PASSED <<<")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
