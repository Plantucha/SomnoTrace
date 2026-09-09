#!/usr/bin/env bash
# SomnoTrace - static analysis, the same checks CI runs
#
# ── Usage ───────────────────────────────────────────────────────────────────
#   ./scripts/lint.sh              # use the tools installed on this machine
#   ./scripts/lint.sh --docker     # use the pinned image, matching CI exactly
#
# THE POINT OF --docker. A linter's finding set depends on its version (see
# scripts/lint-versions.env for the measurements), so "it passes for me" is not
# a claim you can make from a different cppcheck than CI's. With --docker the
# only dependency is Docker, and the answer is the one CI will give.
#
# Exit status is the BLOCKING tier's. The style tier is printed as a count and
# never affects it — the same split the workflow uses, for the same reason: a
# gate that is permanently red is a gate everyone learns to ignore.
set -uo pipefail          # NOT -e: every check must run even after one fails,
                          # or fixing the first hides the second

cd "$(dirname "$0")/.." || exit 2
# shellcheck source=scripts/lint-versions.env
. scripts/lint-versions.env

if [ "${1:-}" = "--docker" ]; then
    exec docker run --rm -v "$PWD:/src" -w /src "$LINT_IMAGE" bash -c '
        apt-get update -qq >/dev/null 2>&1
        DEBIAN_FRONTEND=noninteractive apt-get install -y -qq cppcheck shellcheck >/dev/null 2>&1
        ./scripts/lint.sh'
fi

for t in cppcheck shellcheck; do
    command -v "$t" >/dev/null || { echo "$t not installed — try: ./scripts/lint.sh --docker"; exit 2; }
done

# Report drift rather than refusing to run: a newer analyser is usually fine, and the
# reason to say so is that an unexplained new finding is otherwise indistinguishable from
# a regression the contributor introduced.
have_cpp=$(cppcheck --version | awk '{print $2}')
have_sh=$(shellcheck --version | awk '/^version:/{print $2}')
[ "$have_cpp" = "$CPPCHECK_VERSION" ] || \
    echo "note: cppcheck $have_cpp, CI pins $CPPCHECK_VERSION — findings may differ (scripts/lint-versions.env)"
[ "$have_sh" = "$SHELLCHECK_VERSION" ] || \
    echo "note: shellcheck $have_sh, CI pins $SHELLCHECK_VERSION — findings may differ (scripts/lint-versions.env)"

# The pointer-subtraction pair is suppressed for the WHOLE tree, not per site.
# ESP-IDF's EMBED_FILES gives each blob a `_binary_<name>_start[]` / `_binary_<name>_end[]`
# pair of linker symbols, and `end - start` is the documented way to get its length.
# cppcheck sees two unrelated extern arrays and calls it undefined behaviour. It is a
# false positive every time, and a NEW one appears for every blob anyone embeds — so a
# per-site inline suppression means the gate breaks on a change that is entirely correct,
# and the person who hits it has to know this to get past it.
#
# WHAT THIS GIVES UP, stated rather than buried: a genuine subtraction of pointers into
# two different objects would no longer be caught. Every instance in this tree today is
# the `_binary_*` pattern, so the check has found nothing else; if that stops being true
# the answer is a targeted assertion, not re-enabling a check with a 100% false-positive
# rate. `comparePointers` is the same finding under cppcheck 2.13's name for it.
CPPCHECK_COMMON=(--std=c11 --language=c --inline-suppr
                 --suppress=missingInclude --suppress=missingIncludeSystem
                 --suppress=unmatchedSuppression
                 --suppress=subtractPointers --suppress=comparePointers
                 -i third_party -i build -i managed_components
                 --template='{severity}: {file}:{line}: {message} [{id}]')

rc=0

printf '\n▸ cppcheck — blocking tier\n'
cppcheck --enable=warning,performance,portability "${CPPCHECK_COMMON[@]}" \
         --error-exitcode=1 main components || rc=1

printf '\n▸ cppcheck — style tier (advisory)\n'
cppcheck --enable=style --suppress=unusedFunction "${CPPCHECK_COMMON[@]}" \
         main components 2>&1 >/dev/null | grep '^style:' > /tmp/lint-style.$$ || true
printf '  %s style finding(s)\n' "$(wc -l < /tmp/lint-style.$$)"
awk -F'[][]' '{print $2}' /tmp/lint-style.$$ | sort | uniq -c | sort -rn | sed 's/^/    /'
rm -f /tmp/lint-style.$$

printf '\n▸ shellcheck — our own scripts\n'
# OUR scripts only: third_party ships more that we do not maintain.
mapfile -t sh_files < <(git ls-files '*.sh' | grep -v '^third_party/')
printf '  %d script(s)\n' "${#sh_files[@]}"
# SC2034 ("appears unused") is EXCLUDED from the blocking tier and reported below with the
# other advisories instead. It is not a correctness finding, and it is structurally noisy on
# any script that destructures a record:
#
#     IFS=$'\t' read -r BOARD BUILD_DIR SDKCONFIG DEFAULTS BUILD_REQUEST CLEAN_REQUEST <<< "$P"
#
# You cannot read positional fields without naming the ones you do not use, so a script that
# parses records trips this once per unused field, every time, for correct code. Measured on
# a 92,000-line branch proposed for this repo: 8 blocking shellcheck findings, 7 of them this
# one shape and none of them a bug.
#
# Everything else at --severity=warning stays blocking, and deserves to. The eighth finding in
# that run was SC2164 — a `cd` with no `|| exit` — which is exactly the kind of thing this gate
# is for: on failure the script keeps running in the wrong directory and reports on a tree it
# never meant to read. That one is fixed in this branch rather than suppressed.
shellcheck --severity=warning -e SC2034 -f gcc "${sh_files[@]}" || rc=1

# Advisory shell tier, mirroring the cppcheck split above: counted, never blocking.
printf '\n▸ shellcheck — style tier (advisory)\n'
sh_style=$(shellcheck --severity=style -f gcc "${sh_files[@]}" 2>/dev/null | grep -cE 'warning|note|error' || true)
printf '  %s style/info finding(s)\n' "${sh_style:-0}"
shellcheck --severity=style -f gcc "${sh_files[@]}" 2>/dev/null \
  | grep -oE 'SC[0-9]+' | sort | uniq -c | sort -rn | head -10 | sed 's/^/       /' || true

printf '\n%s\n' "$([ $rc -eq 0 ] && echo 'lint: blocking tier clean' || echo 'lint: BLOCKING TIER FAILED')"
exit $rc
