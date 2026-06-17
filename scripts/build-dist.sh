#!/usr/bin/env bash
#
# One-shot build: compiles the firmware and produces a SINGLE merged image
# (flashable at offset 0x0) in the git-ignored dist/ folder.
#
# The output is named after the project and the current version derived from
# git tags:
#   - exactly on a tag:           somnotrace-v1.2.3-merged.bin
#   - N commits after a tag:      somnotrace-v1.2.3+N-merged.bin
#   - uncommitted local changes:  ...+dirty
#   - no tags yet:                somnotrace-0.0.0+<count>.g<sha>-merged.bin
#
# Usage:
#   ./scripts/build-dist.sh            # incremental build (fast)
#   ./scripts/build-dist.sh --clean    # full clean build first
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DIST_DIR="${PROJECT_DIR}/dist"
PROJECT_NAME="somnotrace"

DO_CLEAN=0
for arg in "$@"; do
    case "$arg" in
        --clean) DO_CLEAN=1 ;;
        *) echo "Unknown option: $arg" >&2; exit 2 ;;
    esac
done

# --- Derive a version string from git ---------------------------------------
version_from_git() {
    cd "${PROJECT_DIR}"
    local describe dirty d rest commits tag count sha
    if describe="$(git describe --tags --long --dirty 2>/dev/null)"; then
        dirty=""
        case "$describe" in
            *-dirty) dirty="+dirty"; describe="${describe%-dirty}" ;;
        esac
        d="${describe%-g*}"        # strip -g<hash>  ->  <tag>-<commits>
        commits="${d##*-}"          # commits since tag
        tag="${d%-*}"               # tag name
        if [ "$commits" = "0" ]; then
            printf '%s%s' "$tag" "$dirty"
        else
            printf '%s+%s%s' "$tag" "$commits" "$dirty"
        fi
    else
        # No tags yet (or not a git repo): fall back to commit count + sha.
        count="$(git rev-list --count HEAD 2>/dev/null || echo 0)"
        sha="$(git rev-parse --short HEAD 2>/dev/null || echo nogit)"
        dirty=""
        # Only meaningful once there is a HEAD commit to diff against.
        if git rev-parse --verify -q HEAD >/dev/null 2>&1 \
           && ! git diff --quiet --ignore-submodules HEAD 2>/dev/null; then
            dirty="+dirty"
        fi
        printf '0.0.0+%s.g%s%s' "$count" "$sha" "$dirty"
    fi
}

VERSION="$(version_from_git)"
OUTPUT_NAME="${PROJECT_NAME}-${VERSION}-merged.bin"
OTA_NAME="${PROJECT_NAME}-${VERSION}-ota.bin"

# --- Build ------------------------------------------------------------------
if [ "${DO_CLEAN}" -eq 1 ]; then
    echo "==> Clean build requested: running fullclean..."
    "${SCRIPT_DIR}/idf.sh" fullclean || true
fi

echo "==> Building firmware (version: ${VERSION})..."
"${SCRIPT_DIR}/idf.sh" build

echo "==> Merging into a single image (flashable at 0x0)..."
"${SCRIPT_DIR}/idf.sh" merge-bin -o merged.bin

echo "==> Publishing images to dist/..."
mkdir -p "${DIST_DIR}"
cp "${PROJECT_DIR}/build/merged.bin" "${DIST_DIR}/${OUTPUT_NAME}"
cp "${PROJECT_DIR}/build/${PROJECT_NAME}.bin" "${DIST_DIR}/${OTA_NAME}"

# Convenience pointers to the most recent builds.
ln -sf "${OUTPUT_NAME}" "${DIST_DIR}/${PROJECT_NAME}-latest-merged.bin"
ln -sf "${OTA_NAME}" "${DIST_DIR}/${PROJECT_NAME}-latest-ota.bin"

# --- Clean up transient artifacts -------------------------------------------
# The merged image is a derived convenience artifact; the rest of build/ is
# kept to allow fast incremental rebuilds.
rm -f "${PROJECT_DIR}/build/merged.bin"

echo ""
echo "Done. Flash this file at offset 0x0:"
echo "  ${DIST_DIR}/${OUTPUT_NAME}"
ls -lh "${DIST_DIR}/${OUTPUT_NAME}"
echo "OTA application image:"
echo "  ${DIST_DIR}/${OTA_NAME}"
ls -lh "${DIST_DIR}/${OTA_NAME}"
