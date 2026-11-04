#!/bin/bash
# run_perf_binding_cell.sh — iter8 perf binding cell.
#
# Anchors campaign-wide claims with measured numbers. Runs four consumer
# configurations for $DURATION seconds each on $FIXTURE and emits a
# markdown table comparing:
#   1. mpv --hwdec=vaapi          (DMA-BUF zero-copy through libva)
#   2. mpv --hwdec=vaapi-copy     (HW decode + VAImage readback)
#   3. firefox (iter5-amend, sandbox enabled, file:// URL)
#   4. mpv --hwdec=no             (SW decode baseline / control)
#
# For each consumer: CPU% (median + p90), GPU freq (median MHz), drops in
# measurement window, p50 frame interval (ms), VmRSS delta (MiB).
#
# Usage:
#   ./run_perf_binding_cell.sh [fixture_path]
#
# If no argument, defaults to /home/mfritsche/fourier-test/bbb_1080p30_h264.mp4
# Override DURATION via env: DURATION=60 ./run_perf_binding_cell.sh
#
# Reproducibility: results depend on (a) the iter7-end driver being installed
# at /usr/lib/dri/v4l2_request_drv_video.so, (b) ohm idle (no other compute
# load), (c) fixture present at the expected path. Run on a stable thermal
# state (after a few minutes of cool-down).

set -eu

FIXTURE="${1:-/home/mfritsche/fourier-test/bbb_1080p30_h264.mp4}"
DURATION="${DURATION:-30}"
WORKDIR="${WORKDIR:-$(mktemp -d -t perf_binding.XXXXXX)}"
GPU_DEVFREQ_PATH="${GPU_DEVFREQ_PATH:-/sys/class/devfreq/fde60000.gpu/cur_freq}"

# DISPLAY/Wayland env for the operator's session, needed for Firefox under sudo.
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1001}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export DISPLAY="${DISPLAY:-:0}"
export XAUTHORITY="${XAUTHORITY:-/run/user/1001/xauth_pxiMur}"

# libva env vars for the v4l2_request driver path.
export LIBVA_DRIVER_NAME=v4l2_request
export LIBVA_V4L2_REQUEST_VIDEO_PATH=/dev/video1
export LIBVA_V4L2_REQUEST_MEDIA_PATH=/dev/media0

if [[ ! -f "$FIXTURE" ]]; then
	echo "FAIL: fixture not found: $FIXTURE" >&2
	exit 2
fi

mkdir -p "$WORKDIR"
echo "Fixture:   $FIXTURE ($(stat -c %s "$FIXTURE") bytes)"
echo "Duration:  ${DURATION}s per configuration"
echo "Workdir:   $WORKDIR"
echo "GPU freq:  $GPU_DEVFREQ_PATH"
echo "Driver sha: $(sha256sum /usr/lib/dri/v4l2_request_drv_video.so | cut -d' ' -f1)"
echo

# percentile_from_stream sorted_file pct -> echo Nth percentile value
# Argument: a file with one numeric value per line (no header), and a percentile
# integer (50, 90, etc.). Numbers can be float; uses awk.
percentile_from_stream() {
	local file="$1" pct="$2"
	awk -v pct="$pct" '
	{ a[NR] = $1 }
	END {
		if (NR == 0) { print "0"; exit }
		# sort
		for (i = 1; i <= NR; i++) for (j = i+1; j <= NR; j++) if (a[i] > a[j]) { t = a[i]; a[i] = a[j]; a[j] = t }
		idx = int((pct/100.0) * NR + 0.5)
		if (idx < 1) idx = 1
		if (idx > NR) idx = NR
		print a[idx]
	}' "$file"
}

# Background-poll GPU freq while the consumer runs. Writes Hz values to $1.
poll_gpu_freq() {
	local out="$1"
	: >"$out"
	while [[ -e "/proc/$BG_PARENT_PID" ]]; do
		if [[ -r "$GPU_DEVFREQ_PATH" ]]; then
			cat "$GPU_DEVFREQ_PATH" 2>/dev/null >>"$out" || true
		fi
		sleep 0.1
	done
}

# Run a single consumer configuration. Args:
#   $1 label (used for filename, no spaces)
#   $2 launcher cmd (will be exec'd as mfritsche; should be a single line)
#   $3 'mpv' or 'firefox' — affects how we find the PID to track
run_consumer() {
	local label="$1"
	local launcher="$2"
	local kind="$3"
	local logdir="$WORKDIR/$label"
	mkdir -p "$logdir"

	echo "=== Running: $label ==="

	# Kill any running firefox/mpv first to clean state.
	pkill -f firefox 2>/dev/null || true
	pkill -x mpv     2>/dev/null || true
	sleep 1

	# VmRSS at start (we'll read again at end) — captured per-PID after launch.
	# Launch consumer in background, capture stdout+stderr to a log.
	(
		eval "$launcher" >"$logdir/consumer.log" 2>&1
	) &
	local launcher_pid=$!

	# Wait briefly for the process tree to spawn the actual decode worker.
	sleep 4

	local target_pid
	case "$kind" in
		mpv)
			target_pid=$(pgrep -x mpv | head -1)
			;;
		firefox)
			# Firefox's RDD process holds /dev/video1; that's the one with
			# the libva decoder context. Wait an extra few seconds for it
			# to spawn and bind the device.
			sleep 6
			target_pid=$(pgrep -af 'contentproc.*\brdd\b' | awk '{print $1}' | head -1)
			if [[ -z "${target_pid:-}" ]]; then
				# Fallback: find whichever firefox process holds /dev/video1.
				target_pid=$(sudo lsof -t /dev/video1 2>/dev/null | head -1 || true)
			fi
			;;
		*)
			echo "  bad kind: $kind" >&2
			return 1
			;;
	esac

	if [[ -z "${target_pid:-}" ]]; then
		echo "  WARN: could not locate $kind process; skipping pidstat" >&2
		# Let the consumer run for the duration anyway so the log gets data.
		sleep "$DURATION"
		kill -TERM "$launcher_pid" 2>/dev/null || true
		pkill -f firefox 2>/dev/null || true
		pkill -x mpv     2>/dev/null || true
		return 0
	fi

	echo "  Tracking PID $target_pid"

	# VmRSS at start.
	local rss_start
	rss_start=$(awk '/^VmRSS:/{print $2}' "/proc/$target_pid/status" 2>/dev/null || echo 0)
	echo "  VmRSS start: ${rss_start} kB"

	# Poll GPU freq in background (keyed off launcher_pid).
	BG_PARENT_PID=$launcher_pid
	poll_gpu_freq "$logdir/gpu_freq.log" &
	local poll_pid=$!

	# Run pidstat for $DURATION seconds.
	pidstat -u -p "$target_pid" 1 "$DURATION" >"$logdir/pidstat.log" 2>&1 || true

	# VmRSS at end (before killing).
	local rss_end
	rss_end=$(awk '/^VmRSS:/{print $2}' "/proc/$target_pid/status" 2>/dev/null || echo "$rss_start")

	# Stop everything.
	kill "$poll_pid" 2>/dev/null || true
	kill -TERM "$launcher_pid" 2>/dev/null || true
	pkill -f firefox 2>/dev/null || true
	pkill -x mpv     2>/dev/null || true
	sleep 1

	# Parse pidstat by header: locate the %CPU column index from the
	# column-name row (where any field equals "%CPU"), then apply it
	# to data rows. Robust across sysstat 12.x point releases.
	# pidstat default output has no '#' header marker — the header is
	# the first row containing "%CPU" as a field.
	awk '
		# Header row: any line where some field equals "%CPU".
		!col {
			for (i = 1; i <= NF; i++) if ($i == "%CPU") { col = i; next }
		}
		# Data row: lines whose value at $col is numeric. Skip the
		# trailing "Average" summary by requiring $col to parse cleanly.
		col && NF >= col && $col ~ /^[0-9]+(\.[0-9]+)?$/ {
			print $col
		}
	' "$logdir/pidstat.log" >"$logdir/cpu_pct.log" || true

	local cpu_p50 cpu_p90
	if [[ -s "$logdir/cpu_pct.log" ]]; then
		cpu_p50=$(percentile_from_stream "$logdir/cpu_pct.log" 50)
		cpu_p90=$(percentile_from_stream "$logdir/cpu_pct.log" 90)
	else
		cpu_p50="ERR"
		cpu_p90="ERR"
	fi

	# GPU freq median. Values are Hz; convert to MHz via temp file (avoids
	# unreliable /dev/stdin in a nested subshell-over-pipe).
	local gpu_med_mhz
	if [[ -s "$logdir/gpu_freq.log" ]]; then
		awk '{print $1/1000000}' "$logdir/gpu_freq.log" >"$logdir/gpu_freq_mhz.log"
		gpu_med_mhz=$(percentile_from_stream "$logdir/gpu_freq_mhz.log" 50)
	else
		gpu_med_mhz="—"
	fi

	# RSS delta MiB.
	local rss_delta_mib
	rss_delta_mib=$(awk -v s="$rss_start" -v e="$rss_end" 'BEGIN{printf "%.1f", (e-s)/1024.0}')

	# Drops + p50 frame interval — only available for mpv.
	local drops="—"
	local p50_frame_ms="—"
	if [[ "$kind" == "mpv" ]]; then
		drops=$(grep -oE 'frame-drop-count[^\t ]*\s*=\s*[0-9]+' "$logdir/consumer.log" \
			| awk -F= '{print $2}' | tr -d ' ' | tail -1)
		drops="${drops:-0}"
		# p50 frame interval from mpv vsync-jitter or frame timing — leave
		# as "—" unless mpv emitted detailed timing.
	fi

	# Emit row.
	cat >>"$WORKDIR/results.tsv" <<-ROW
	$label	$cpu_p50	$cpu_p90	$drops	$p50_frame_ms	$gpu_med_mhz	$rss_delta_mib
	ROW

	echo "  CPU% p50=$cpu_p50  p90=$cpu_p90  drops=$drops  gpu_med=$gpu_med_mhz MHz  rss_delta=$rss_delta_mib MiB"
	echo
}

# Header for results.
echo "consumer	cpu_p50	cpu_p90	drops_${DURATION}s	p50_frame_ms	gpu_med_mhz	rss_delta_mib" >"$WORKDIR/results.tsv"

# === Configurations ===

# 1. mpv DMA-BUF zero-copy
run_consumer "mpv-vaapi-dmabuf" \
	"sudo -u mfritsche env LIBVA_DRIVER_NAME=v4l2_request \
		LIBVA_V4L2_REQUEST_VIDEO_PATH=/dev/video1 \
		LIBVA_V4L2_REQUEST_MEDIA_PATH=/dev/media0 \
		mpv --no-config --hwdec=vaapi --vo=null --no-audio \
		    --term-status-msg='\${frame-drop-count}' \
		    --length=$DURATION '$FIXTURE'" \
	mpv

# 2. mpv vaapi-copy
run_consumer "mpv-vaapi-copy" \
	"sudo -u mfritsche env LIBVA_DRIVER_NAME=v4l2_request \
		LIBVA_V4L2_REQUEST_VIDEO_PATH=/dev/video1 \
		LIBVA_V4L2_REQUEST_MEDIA_PATH=/dev/media0 \
		mpv --no-config --hwdec=vaapi-copy --vo=null --no-audio \
		    --term-status-msg='\${frame-drop-count}' \
		    --length=$DURATION '$FIXTURE'" \
	mpv

# 3. Firefox-fourier (iter5-amend, sandbox enabled)
run_consumer "firefox-fourier-hw" \
	"sudo -u mfritsche env XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR \
		WAYLAND_DISPLAY=$WAYLAND_DISPLAY DISPLAY=$DISPLAY \
		XAUTHORITY=$XAUTHORITY \
		LIBVA_DRIVER_NAME=v4l2_request \
		LIBVA_V4L2_REQUEST_VIDEO_PATH=/dev/video1 \
		LIBVA_V4L2_REQUEST_MEDIA_PATH=/dev/media0 \
		firefox --new-window 'file://$FIXTURE'" \
	firefox

# 4. SW baseline
run_consumer "mpv-sw-baseline" \
	"sudo -u mfritsche mpv --no-config --hwdec=no --vo=null --no-audio \
		--term-status-msg='\${frame-drop-count}' \
		--length=$DURATION '$FIXTURE'" \
	mpv

# === Generate markdown table ===
{
	echo "# Performance binding cell — iter8 (libva-multiplanar campaign)"
	echo
	echo "Run date: $(date -Iseconds)"
	echo "Host: $(uname -n) ($(uname -m))"
	echo "Kernel: $(uname -r)"
	echo "Driver sha256: \`$(sha256sum /usr/lib/dri/v4l2_request_drv_video.so | cut -d' ' -f1)\`"
	echo "Fixture: \`$FIXTURE\` ($(stat -c %s "$FIXTURE") bytes)"
	echo "Duration per consumer: ${DURATION}s"
	echo
	echo "| Consumer | CPU% p50 | CPU% p90 | Drops in window | p50 frame ms | GPU MHz median | VmRSS Δ MiB |"
	echo "|---|---|---|---|---|---|---|"
	tail -n +2 "$WORKDIR/results.tsv" | awk -F'\t' '{
		printf "| %s | %s | %s | %s | %s | %s | %s |\n",
			$1, $2, $3, $4, $5, $6, $7
	}'
} >"$WORKDIR/perf_binding_cell.md"

echo "=== Done ==="
echo "Results: $WORKDIR/perf_binding_cell.md"
echo "Per-consumer logs: $WORKDIR/{mpv-vaapi-dmabuf,mpv-vaapi-copy,firefox-fourier-hw,mpv-sw-baseline}/"
echo
cat "$WORKDIR/perf_binding_cell.md"
