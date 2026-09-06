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

CPPCHECK_COMMON=(--std=c11 --language=c --inline-suppr
                 --suppress=missingInclude --suppress=missingIncludeSystem
                 --suppress=unmatchedSuppression
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
shellcheck --severity=warning -f gcc "${sh_files[@]}" || rc=1

printf '\n%s\n' "$([ $rc -eq 0 ] && echo 'lint: blocking tier clean' || echo 'lint: BLOCKING TIER FAILED')"
exit $rc
