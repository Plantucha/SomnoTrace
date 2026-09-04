#!/usr/bin/env python3
# SomnoTrace — scripts/mutate_host.py
# Copyright 2026 Michal Planicka · SPDX-License-Identifier: Apache-2.0
"""
MUTATION ANALYSIS FOR THE HOST TEST SUITE — does a passing test suite actually notice a change?

A green suite proves the tests ran. It does not prove they would have failed. This changes one thing
in the shipped source, rebuilds the host tests, and reports whether they noticed.

── THE DISTINCTION THIS TOOL EXISTS TO MAKE ────────────────────────────────────────────────────────
A surviving mutant is not one finding, it is one of two, and they have OPPOSITE fixes:

    UNREACHED    no test executes that line       →  wire the file in / write a test that reaches it
    UNASSERTED   tests execute it and do not care →  strengthen an assertion

Reporting both as "SURVIVED" fuses them into a queue nobody can act on. Issue #202 was entirely the
first kind: mutating `spool_to_edf` in edf_summary.c changed nothing because the host suite never
linked that translation unit — the test carried a private copy. A survivor count alone would have
read as "the tests are weak"; the actual fix was a build-and-include change.

Reach is measured with gcov, and ⚠️ THE SKIP LIST FAILS CLOSED. Coverage missing, gcov absent, file
not in the report, unparseable line — every one of those resolves to REACHED, i.e. run the mutant.
Over-running mutants costs a few seconds. Under-running them costs the programme its meaning: a skip
list that fails OPEN quietly stops testing code and reports the silence as progress.

── TWO CONTROLS, BOTH MANDATORY ────────────────────────────────────────────────────────────────────
BASELINE   the unmutated tree must build and pass first. If it does not, nothing below is reported —
           a mutant "killed" by an already-red suite is not a kill.
CANARY     one mutant no working suite could miss: the body of a function the tests exercise is
           emptied outright (extreme mutation — Descartes; Petrović & Ivanković, ICSE-SEIP '18).
           If the canary SURVIVES, the harness is not running what it thinks it is and the entire
           run is VOID rather than reported as "0 survivors". A zero means nothing without it.

── WHAT A SURVIVOR IS NOT ──────────────────────────────────────────────────────────────────────────
Not automatically a gap. `if (x < 0) x = 0;` mutated to `<=` still assigns 0 when x IS 0, and no
input separates them. Such a mutant is EQUIVALENT and unkillable by anyone. Record them in
`scripts/mutate_equivalent.txt` with the reason, one `file:line:op` per line, so the next run does
not re-report them and the reason survives the person who worked it out.

Usage
    python3 scripts/mutate_host.py --selftest             # prove the harness detects and discriminates
    python3 scripts/mutate_host.py main/as11_time.c
    python3 scripts/mutate_host.py main/edf_data_dict.h --limit 40
    python3 scripts/mutate_host.py --all                  # every source a host test links

Exit codes: 0 clean · 1 survivors · 2 VOID (baseline or canary failed) · 3 setup error
"""
from __future__ import annotations

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
MAIN = os.path.join(ROOT, "main")
INC = [os.path.join(ROOT, "main"), os.path.join(HERE, "test_include"), ROOT]

# Each host test, and the shipped translation units it links. Keeping this explicit (rather than
# globbing main/) is deliberate: the set of files a test links IS the thing #202 was about, so it
# should be visible and reviewable rather than inferred.
TESTS = {
    "as11_time_test":       ["main/as11_time.c"],
    "as11_events_test":     ["main/as11_time.c", "@cjson"],
    "edf_properties_test":  ["main/as11_time.c", "@cjson"],
    "vld3_decoder_test":    ["main/oximetry_vld3.c"],
}

EQUIV_FILE = os.path.join(HERE, "mutate_equivalent.txt")


def find_cjson() -> str | None:
    """cJSON.c, wherever this checkout keeps it. Not bundled, so it is searched rather than assumed.

    ⚠️ BOUNDED ON PURPOSE. An earlier version fell back to a recursive walk of $HOME. On a box
    with a large data volume it never returned: the tool hung before printing a line and its own
    timeout killed it. A search for a build input is not worth an unbounded filesystem walk — look
    where this project puts it, then ask. Override with $SNT_CJSON or --cjson."""
    env = os.environ.get("SNT_CJSON")
    if env and os.path.exists(env):
        return env
    for pat in ("managed_components/*/cJSON.c", "managed_components/*/*/cJSON.c",
                "components/*/cJSON.c", "build/**/cJSON.c"):
        hit = glob.glob(os.path.join(ROOT, pat), recursive=True)
        if hit:
            return hit[0]
    for direct in ("/usr/include/cjson/cJSON.c", "/usr/local/src/cJSON/cJSON.c",
                   "/usr/share/cjson/cJSON.c"):
        if os.path.exists(direct):
            return direct
    return None


CJSON = find_cjson()


def sources_for(test: str) -> list[str] | None:
    out = []
    for s in TESTS[test]:
        if s == "@cjson":
            if not CJSON:
                return None
            out.append(CJSON)
        else:
            out.append(os.path.join(ROOT, s))
    return out


def build(test: str, outdir: str, coverage: bool = False) -> str | None:
    src = sources_for(test)
    if src is None:
        return None
    exe = os.path.join(outdir, test)
    cmd = ["gcc", "-O0", "-o", exe, os.path.join(HERE, test + ".c"), *src]
    if coverage:
        cmd += ["--coverage"]
    cmd += [f"-I{d}" for d in INC]
    if CJSON:
        cmd.append("-I" + os.path.dirname(CJSON))
    cmd.append("-lm")
    r = subprocess.run(cmd, capture_output=True, cwd=outdir)
    return exe if r.returncode == 0 else None


def run(exe: str, env_tz: str = "UTC") -> bool:
    """True when the test passes. TZ is pinned: a suite whose result depends on the host clock
    cannot distinguish a killed mutant from a different machine."""
    env = dict(os.environ, TZ=env_tz)
    try:
        return subprocess.run([exe], capture_output=True, env=env, timeout=300).returncode == 0
    except subprocess.TimeoutExpired:
        return False          # a hung mutant is killed, not survived


def suite_passes(outdir: str) -> tuple[bool, str]:
    for t in TESTS:
        exe = build(t, outdir)
        if exe is None:
            if sources_for(t) is None:
                continue      # cJSON absent — that test is skipped, not failed
            return False, f"{t}: build failed"
        if not run(exe):
            return False, f"{t}: failed"
    return True, "all pass"


# ── reach ───────────────────────────────────────────────────────────────────────────────────────
def executed_lines(outdir: str) -> dict[str, set[int]] | None:
    """{abs_path: {line numbers executed by at least one host test}}, or None if reach is unknown.

    None is the FAIL-CLOSED signal: every caller must treat it as "everything is reached"."""
    if not shutil.which("gcov"):
        return None
    cov: dict[str, set[int]] = {}
    ok_any = False
    for t in TESTS:
        if sources_for(t) is None:
            continue
        d = tempfile.mkdtemp(prefix=f"cov-{t}-", dir=outdir)
        exe = build(t, d, coverage=True)
        if exe is None or not run(exe):
            continue
        gcda = glob.glob(os.path.join(d, "*.gcda"))
        if not gcda:
            continue
        subprocess.run(["gcov", "-p", *gcda], capture_output=True, cwd=d)
        for g in glob.glob(os.path.join(d, "*.gcov")):
            src = None
            try:
                with open(g, encoding="utf-8", errors="replace") as f:
                    for line in f:
                        m = re.match(r"\s*-:\s*0:Source:(.*)", line)
                        if m:
                            src = os.path.abspath(m.group(1).strip())
                            continue
                        m = re.match(r"\s*([^:]+):\s*(\d+):", line)
                        if not m or src is None:
                            continue
                        cnt, num = m.group(1).strip(), int(m.group(2))
                        # Only three markers mean "not executed". Everything else — a count, a
                        # count with gcov's '*' partial-branch suffix ("9*"), or a form this parser
                        # has never seen — is REACHED. The first version accepted only [#\-0-9=] and
                        # so silently dropped every "N*" line: the ternaries and short-circuits,
                        # i.e. the lines mutation exists to test, all reported as UNREACHED.
                        if num and cnt not in ("#####", "=====", "-"):
                            cov.setdefault(src, set()).add(num)
                ok_any = True
            except OSError:
                return None
    return cov if ok_any else None


# ── mutants ─────────────────────────────────────────────────────────────────────────────────────
OPS: list[tuple[str, str, str]] = [
    ("<=", "<", "relational"), ("<", "<=", "relational"),
    (">=", ">", "relational"), (">", ">=", "relational"),
    ("==", "!=", "equality"), ("!=", "==", "equality"),
    ("&&", "||", "logical"), ("||", "&&", "logical"),
    (" + ", " - ", "arithmetic"), (" - ", " + ", "arithmetic"),
    (" * ", " / ", "arithmetic"),
]

SKIP_LINE = re.compile(r'^\s*(//|/\*|\*|#include|#pragma)')


def load_equivalents() -> set[str]:
    out = set()
    if os.path.exists(EQUIV_FILE):
        for line in open(EQUIV_FILE, encoding="utf-8"):
            line = line.split("#", 1)[0].strip()
            if line:
                out.add(line)
    return out


def gen_mutants(path: str, limit: int) -> list[tuple[int, str, str, str]]:
    """(lineno, op_name, original_line, mutated_line) — textual, one change each."""
    out = []
    with open(path, encoding="utf-8") as f:
        lines = f.readlines()
    for i, line in enumerate(lines, 1):
        if SKIP_LINE.match(line) or '"' in line:
            continue          # string literals: a changed message is not a behaviour change
        for a, b, name in OPS:
            if a in line:
                out.append((i, f"{name}:{a.strip()}→{b.strip()}", line, line.replace(a, b, 1)))
                break
        if len(out) >= limit:
            break
    return out


# A top-level function definition: a line starting at column 0 that is not a control keyword and
# carries an opening paren. Deliberately loose — the previous version required a single-line
# signature returning one of six named types, and across main/*.c it matched NOTHING in 29 files
# while reporting "no canary candidate found" and carrying on. Most functions here return esp_err_t
# or void, and many split their parameters across lines.
_DEF = re.compile(r'^[A-Za-z_][A-Za-z0-9_ \t\*]*\b(\w+)\s*\(')
_NOT_A_DEF = re.compile(r'^\s*(if|for|while|switch|return|else|do|typedef|struct|enum|union)\b')


def canary_for(path: str, lines: list[str]) -> tuple[int, str, str] | None:
    """Empty the body of a function with a substantial body — extreme mutation (Descartes). No suite
    that executes the function can miss it.

    Returns the LARGEST body found rather than the first: a three-line accessor is a weak canary
    (its removal can be invisible if callers ignore the result), and a weak canary that survives
    reads exactly like a broken harness."""
    best = None
    for i, line in enumerate(lines):
        if _NOT_A_DEF.match(line) or not _DEF.match(line) or line.rstrip().endswith(";"):
            continue
        m = _DEF.match(line)
        # walk forward to the opening brace of the body, tolerating a multi-line signature
        j, guard = i, 0
        while j < len(lines) and "{" not in lines[j]:
            if ";" in lines[j] or guard > 6:
                break
            j += 1
            guard += 1
        if j >= len(lines) or "{" not in lines[j]:
            continue
        depth, k = 0, j
        while k < len(lines):
            depth += lines[k].count("{") - lines[k].count("}")
            if depth == 0:
                break
            k += 1
        body_start, body_end = j + 1, k
        n = body_end - body_start
        if n >= 4 and (best is None or n > best[3]):
            best = (body_start, m.group(1), "".join(lines[body_start:body_end]), n)
    return (best[0], best[1], best[2]) if best else None


def apply(path: str, old_text: str, new_text: str) -> None:
    s = open(path, encoding="utf-8").read()
    open(path, "w", encoding="utf-8", newline="\n").write(s.replace(old_text, new_text, 1))


def mutate_file(path: str, outdir: str, reach: dict | None, limit: int, equivs: set[str]) -> dict:
    rel = os.path.relpath(path, ROOT)
    backup = path + ".mutbak"
    shutil.copy(path, backup)
    lines = open(path, encoding="utf-8").readlines()
    res = {"file": rel, "killed": 0, "stillborn": 0, "unasserted": [], "unreached": [],
           "equivalent": 0, "canary": None, "generated": 0}
    try:
        # ── canary first: if it survives, nothing else this run means anything
        c = canary_for(path, lines)
        if c is None:
            # ⚠️ FAIL CLOSED. No canary means no way to show the harness can detect anything in this
            # file, and an uncontrolled run reports "0 survivors" for a file it may never have
            # compiled. The first version carried on regardless — it found no candidate in any of
            # 29 files and would have reported every one of them as clean.
            res["canary"] = {"fn": None, "line": 0, "survived": None}
            return res
        ln, fname, body = c
        apply(path, body, "")
        ok, _ = suite_passes(outdir)
        shutil.copy(backup, path)
        res["canary"] = {"fn": fname, "line": ln, "survived": ok}
        if ok:
            return res

        mutants = gen_mutants(path, limit)
        res["generated"] = len(mutants)
        for ln, op, orig, mut in mutants:
            key = f"{rel}:{ln}:{op}"
            if key in equivs:
                res["equivalent"] += 1
                continue
            # gcov marks a #define non-executable, but mutating one acts at every use site: never
            # let the reach filter skip it.
            is_macro = orig.lstrip().startswith("#define")
            if reach is not None and not is_macro and ln not in reach.get(os.path.abspath(path), set()):
                res["unreached"].append((ln, op, orig.strip()[:70]))
                continue
            apply(path, orig, mut)
            ok, why = suite_passes(outdir)
            shutil.copy(backup, path)
            if ok:
                res["unasserted"].append((ln, op, orig.strip()[:70]))
            elif why.endswith("build failed"):
                # A mutant that does not compile proves nothing about the tests. Counting it as a
                # kill inflates the score with kills the compiler made, not the suite.
                res["stillborn"] += 1
            else:
                res["killed"] += 1
    finally:
        shutil.copy(backup, path)
        os.unlink(backup)
    return res


# ── selftest ────────────────────────────────────────────────────────────────────────────────────
def selftest(outdir: str) -> int:
    """A zero means nothing without controls, so prove both directions before trusting a run.

    PLANTED-KILLABLE   a real behaviour change the suite MUST notice.
    PLANTED-EQUIVALENT a change no input can distinguish; it MUST survive, and a harness that
                       'kills' it is reporting noise as signal."""
    print("── selftest")
    ok, why = suite_passes(outdir)
    print(f"   baseline                     {'PASS' if ok else 'FAIL — ' + why}")
    if not ok:
        return 2

    target = os.path.join(MAIN, "as11_time.c")
    backup = target + ".selfbak"
    shutil.copy(target, backup)
    rc = 0
    try:
        # must be caught: the noon rule inverted
        apply(target, "if (tod < 43200) days -= 1;", "if (tod < 43200) days -= 0;")
        caught = not suite_passes(outdir)[0]
        shutil.copy(backup, target)
        print(f"   planted KILLABLE mutant      {'caught (correct)' if caught else 'MISSED — harness is blind'}")
        rc |= 0 if caught else 2

        # must survive: floor_div's sign fix is unreachable for the positive inputs the suite uses…
        # …so a harness that reports this as a gap is flooding. It is recorded, not celebrated.
        apply(target, "int64_t q = a / b;", "int64_t q = (a) / (b);")
        survived = suite_passes(outdir)[0]
        shutil.copy(backup, target)
        print(f"   planted EQUIVALENT mutant    {'survived (correct)' if survived else 'KILLED — the harness is unstable'}")
        rc |= 0 if survived else 2
    finally:
        shutil.copy(backup, target)
        os.unlink(backup)
    print("   VERDICT:", "harness discriminates" if rc == 0 else "HARNESS UNSOUND — do not trust a run")
    return rc


def main() -> int:
    # Line-buffered: a run that hangs or is killed must still show how far it got. The first version
    # buffered, was killed by a timeout, and printed nothing at all — not even the baseline result
    # that would have said where it stopped.
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except AttributeError:      # pragma: no cover
        pass
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="*")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--limit", type=int, default=25, help="max mutants per file")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--cjson", help="path to cJSON.c when it is not beside the project")
    a = ap.parse_args()

    global CJSON
    if a.cjson:
        CJSON = a.cjson

    if not shutil.which("gcc"):
        print("gcc not found", file=sys.stderr)
        return 3
    outdir = tempfile.mkdtemp(prefix="snt-mutate-")
    print(f"cJSON: {CJSON or '<not found — tests needing it are SKIPPED, not failed>'}")

    if a.selftest:
        return selftest(outdir)

    ok, why = suite_passes(outdir)
    print(f"BASELINE  {'PASS' if ok else 'FAIL — ' + why}")
    if not ok:
        print("VOID — the baseline does not pass, so no verdict below would mean anything.")
        return 2

    reach = executed_lines(outdir)
    print("REACH     " + ("gcov line coverage available" if reach is not None
                          else "UNAVAILABLE — failing closed, every mutant will be run"))

    targets = a.files or ([os.path.join(ROOT, s) for t in TESTS for s in TESTS[t] if s != "@cjson"]
                          if a.all else [])
    targets = sorted({os.path.abspath(t if os.path.isabs(t) else os.path.join(ROOT, t))
                      for t in targets})
    if not targets:
        ap.error("name a file, or pass --all")

    equivs = load_equivalents()
    total_surv = 0
    for t in targets:
        if not os.path.exists(t):
            print(f"\n{t}: not found"); return 3
        r = mutate_file(t, outdir, reach, a.limit, equivs)
        print(f"\n══ {r['file']}")
        if r["canary"]:
            c = r["canary"]
            if c["survived"] is None:
                print("   🔴 NO CANARY — no function in this file has a body large enough to empty.")
                print("   VOID: without a canary there is no evidence this harness can detect")
                print("         anything here, and '0 survivors' would be indistinguishable from")
                print("         a file that was never compiled.")
                return 2
            if c["survived"]:
                print(f"   🔴 CANARY SURVIVED — {c['fn']}() body emptied and every test still passed.")
                print("   VOID: the harness is not running what it thinks it is. No other result here counts.")
                return 2
            print(f"   canary: {c['fn']}() body emptied → killed")
        if r["generated"] == 0:
            print("   ⚠️  NO MUTANTS GENERATED — nothing below was tested. The canary is the only evidence.")
        print(f"   killed {r['killed']}   stillborn {r['stillborn']}   unasserted {len(r['unasserted'])}   "
              f"unreached {len(r['unreached'])}   known-equivalent {r['equivalent']}")
        for ln, op, src in r["unasserted"]:
            print(f"     🔴 UNASSERTED  {r['file']}:{ln}  {op}   {src}")
        for ln, op, src in r["unreached"][:10]:
            print(f"     ·  UNREACHED   {r['file']}:{ln}  {op}   {src}")
        if len(r["unreached"]) > 10:
            print(f"     ·  … and {len(r['unreached']) - 10} more unreached")
        total_surv += len(r["unasserted"])

    print(f"\n{total_surv} unasserted survivor(s)")
    print("UNREACHED lines need a test that reaches them; UNASSERTED need a stronger assertion.")
    return 1 if total_surv else 0


if __name__ == "__main__":
    sys.exit(main())

