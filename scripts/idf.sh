#!/usr/bin/env bash
#
# Thin wrapper that runs idf.py (or an arbitrary command) inside the official
# Espressif ESP-IDF Docker image. This pins the toolchain to a known version
# without installing the multi-GB SDK on the host. Docker is the only host
# dependency required to build.
#
# Usage:
#   ./scripts/idf.sh build
#   ./scripts/idf.sh set-target esp32s3
#   ./scripts/idf.sh menuconfig
#   ./scripts/idf.sh -p /dev/ttyACM0 flash monitor
#   ./scripts/idf.sh                 # interactive shell in the container
#   ./scripts/idf.sh exec <cmd> ...  # run an arbitrary command in the container
#
set -euo pipefail

# Pinned ESP-IDF release. Override: IDF_TAG=v5.5 ./scripts/idf.sh ...
IDF_TAG="${IDF_TAG:-v5.5.1}"
IMAGE="espressif/idf:${IDF_TAG}"

# Project root (parent of this script's directory).
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Forward a serial device for flash/monitor if present. Override with IDF_PORT.
PORT="${IDF_PORT:-/dev/ttyACM0}"
DEVICE_ARGS=()
if [ -e "$PORT" ]; then
    DEVICE_ARGS+=(--device "$PORT")
fi

# Interactive TTY only when attached to one.
TTY_ARGS=()
if [ -t 0 ]; then
    TTY_ARGS+=(-it)
fi

# With no args, drop into an interactive shell. "exec" runs an arbitrary
# command; otherwise the args are passed to idf.py.
if [ "$#" -eq 0 ]; then
    set -- /bin/bash
elif [ "$1" = "exec" ]; then
    shift
else
    set -- idf.py "$@"
fi

exec docker run --rm \
    "${TTY_ARGS[@]}" \
    "${DEVICE_ARGS[@]}" \
    -v "${PROJECT_DIR}:/project" \
    -w /project \
    "$IMAGE" \
    "$@"
