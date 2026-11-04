#!/bin/bash
# run_cap_pool_probe.sh — orchestrate the cap_pool probe-pattern regression test.
#
# Runs the cap_pool_probe_pattern test program with the v4l2_request driver
# and grep-checks driver stderr for race indicators. Exits 0 on PASS, 1 on FAIL.
#
# Usage: ./run_cap_pool_probe.sh [path_to_test_binary]
# If no argument, looks for ./cap_pool_probe_pattern in the same directory.

set -eu

BIN="${1:-$(dirname "$0")/cap_pool_probe_pattern}"

if [[ ! -x "$BIN" ]]; then
	echo "FAIL: test binary not found or not executable: $BIN" >&2
	echo "Build it first:" >&2
	echo "  gcc -O2 -Wall -Wextra -o $BIN $(dirname "$0")/cap_pool_probe_pattern.c \\" >&2
	echo "      \$(pkg-config --cflags --libs libva libva-drm)" >&2
	exit 2
fi

LOG=$(mktemp -t cap_pool_probe.XXXXXX.log)
trap 'rm -f "$LOG"' EXIT

env LIBVA_DRIVER_NAME=v4l2_request \
    LIBVA_V4L2_REQUEST_VIDEO_PATH=/dev/video1 \
    LIBVA_V4L2_REQUEST_MEDIA_PATH=/dev/media0 \
    "$BIN" >"$LOG" 2>&1
rc=$?

echo "--- test program output ---"
cat "$LOG"
echo "--- end output ---"

if [[ "$rc" -ne 0 ]]; then
	echo "FAIL: test binary exited with rc=$rc" >&2
	exit 1
fi

# Race indicators on driver-prefixed lines only (avoids matching the
# test program's own informational output). Driver log lines start with
# "v4l2-request:".
race_lines=$(grep -E '^v4l2-request:' "$LOG" \
	| grep -iE 'REQBUFS|EBUSY|Unable to request buffers|Unable to set format' \
	|| true)
if [[ -n "$race_lines" ]]; then
	echo "FAIL: driver stderr contains race indicators:" >&2
	echo "$race_lines" >&2
	exit 1
fi

echo "PASS: cap_pool probe-pattern test clean (no race indicators)."
exit 0
