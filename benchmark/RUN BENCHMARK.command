#!/bin/bash

set -uo pipefail

BENCHMARK_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$BENCHMARK_DIR/.." && pwd)"
PROJECT="$BENCHMARK_DIR"
RESULTS_ROOT="${GODOT_METAL_RT_RESULTS_ROOT:-$ROOT/benchmark-results}"
BIN="${GODOT_METAL_RT_BIN:-}"
if [[ -z "$BIN" ]]; then
	if [[ -x "$ROOT/Godot Metal RT.app/Contents/MacOS/Godot" ]]; then
		BIN="$ROOT/Godot Metal RT.app/Contents/MacOS/Godot"
	elif [[ -x "$ROOT/bin/godot.macos.editor.arm64" ]]; then
		BIN="$ROOT/bin/godot.macos.editor.arm64"
	else
		BIN="$ROOT/Godot Metal RT.app/Contents/MacOS/Godot"
	fi
fi
COMPUTER_NAME="$(scutil --get ComputerName 2>/dev/null || hostname)"
COMPUTER_SLUG="$(printf '%s' "$COMPUTER_NAME" | tr -cs '[:alnum:]._-' '_' | sed 's/_$//')"
TIMESTAMP="$(date '+%Y-%m-%dT%H:%M:%S%z')"
RUN_ID="$(date '+%Y%m%d-%H%M%S')-$COMPUTER_SLUG"
RESULT_DIR="$RESULTS_ROOT/$RUN_ID"
LOCAL_CSV="$RESULT_DIR/results.csv"
MASTER_CSV="$RESULTS_ROOT/ALL-RUNS.csv"

mkdir -p "$RESULT_DIR"

if [[ ! -x "$BIN" ]]; then
	printf 'ERROR: Godot executable is missing or not executable:\n%s\n' "$BIN"
	printf 'Press Return to close.\n'
	read -r
	exit 1
fi

MODEL_IDENTIFIER="$(sysctl -n hw.model 2>/dev/null || printf 'unknown')"
MEMORY_BYTES="$(sysctl -n hw.memsize 2>/dev/null || printf 'unknown')"
ARCHITECTURE="$(uname -m)"
MACOS_VERSION="$(sw_vers -productVersion 2>/dev/null || printf 'unknown')"
ENGINE_VERSION="$("$BIN" --version 2>&1 | head -1)"
BINARY_COMMIT_SHORT="${ENGINE_VERSION##*.}"
BUILD_INFO_COMMIT="$(sed -n 's/^Commit: //p' "$ROOT/BUILD-INFO.txt" 2>/dev/null | head -1)"
if [[ -n "$BUILD_INFO_COMMIT" && "$BUILD_INFO_COMMIT" == "$BINARY_COMMIT_SHORT"* ]]; then
	ENGINE_COMMIT="$BUILD_INFO_COMMIT"
else
	ENGINE_COMMIT="$BINARY_COMMIT_SHORT"
	if [[ -n "$BUILD_INFO_COMMIT" ]]; then
		printf 'WARNING: BUILD-INFO.txt commit (%s) does not match the running binary (%s).\n' "$BUILD_INFO_COMMIT" "$BINARY_COMMIT_SHORT"
		printf '         Recording the binary hash so results track the actual build.\n'
	fi
fi

if [[ "$ARCHITECTURE" == 'arm64' ]]; then
	RENDERING_ARGS=(--rendering-driver metal --rendering-method forward_plus)
	SPP_VALUES=(1 4 16)
	BOUNCE_VALUES=(1 2 4)
	REPEATS=2
else
	RENDERING_ARGS=(--rendering-driver opengl3 --rendering-method gl_compatibility)
	SPP_VALUES=(0)
	BOUNCE_VALUES=(0)
	REPEATS=2
fi
MEASURE_FRAMES="${GODOT_PERF_MEASURE_FRAMES:-400}"
if [[ "${GODOT_BENCHMARK_QUICK:-0}" == '1' ]]; then
	SPP_VALUES=(1)
	BOUNCE_VALUES=(1)
	REPEATS=1
	MEASURE_FRAMES="${GODOT_PERF_MEASURE_FRAMES:-60}"
fi

system_profiler SPHardwareDataType SPDisplaysDataType -detailLevel mini > "$RESULT_DIR/system-profiler.txt" 2>&1
CHIP="$(sed -n 's/^[[:space:]]*Chip: //p' "$RESULT_DIR/system-profiler.txt" | head -1)"
if [[ -z "$CHIP" ]]; then CHIP="$(sysctl -n machdep.cpu.brand_string 2>/dev/null || printf 'unknown')"; fi
{
	printf 'Benchmark timestamp: %s\nComputer name: %s\nModel identifier: %s\nChip/CPU: %s\nMemory bytes: %s\nArchitecture: %s\nmacOS: %s\nGodot: %s\nCommit: %s\n\nFull hardware/display report:\n' "$TIMESTAMP" "$COMPUTER_NAME" "$MODEL_IDENTIFIER" "$CHIP" "$MEMORY_BYTES" "$ARCHITECTURE" "$MACOS_VERSION" "$ENGINE_VERSION" "$ENGINE_COMMIT"
	cat "$RESULT_DIR/system-profiler.txt"
} > "$RESULT_DIR/system-info.txt"

HEADER='timestamp,computer_name,model_identifier,chip,macos,architecture,engine_commit,engine_version,spp,max_bounces,repeat,exit_code,rt_gate,video_adapter,avg_fps,frame_ms_avg,frame_ms_p50,frame_ms_p95,frame_ms_p99,frame_ms_max,cpu_ms_avg,cpu_ms_p95,pathtracer_gpu_ms_avg,pathtracer_gpu_ms_min,pathtracer_gpu_ms_max,workload'
printf '%s\n' "$HEADER" > "$LOCAL_CSV"
if [[ ! -s "$MASTER_CSV" ]] || [[ "$(head -1 "$MASTER_CSV")" != "$HEADER" ]]; then
	printf '%s\n' "$HEADER" > "$MASTER_CSV"
fi

metric() { sed -n "s/^$1=//p" "$2" | tail -1; }
csv_quote() { local value; value="$(printf '%s' "$1" | tr '\r\n' '  ')"; value="${value//\"/\"\"}"; printf '"%s"' "$value"; }
gpu_stat() {
	grep -E '^[[:space:]]+-Pathtracer:' "$2" 2>/dev/null | sed -E 's/.*Pathtracer: ([0-9.]+)ms.*/\1/' | awk -v mode="$1" '
		NR == 1 { min = $1; max = $1 } { sum += $1; count += 1; if ($1 < min) min = $1; if ($1 > max) max = $1 }
		END { if (!count) print "n/a"; else if (mode == "avg") printf "%.3f", sum / count; else if (mode == "min") printf "%.3f", min; else printf "%.3f", max }'
}

printf '\nGodot Metal RT benchmark matrix\n'
printf 'Machine: %s (%s, %s)\n' "$COMPUTER_NAME" "$MODEL_IDENTIFIER" "$CHIP"
if [[ "$ARCHITECTURE" == 'arm64' ]]; then
	if [[ "${GODOT_BENCHMARK_QUICK:-0}" == '1' ]]; then
		printf 'Quick matrix: 1920x1080, 1 SPP × 1 bounce, one measured run.\n'
	else
		printf 'Matrix: 1920x1080, SPP {1,4,16} × max bounces {1,2,4}, %d repeats each (18 measured runs).\n' "$REPEATS"
	fi
else
	printf 'Intel/OpenGL raster compatibility fallback: %d repeat runs (not RT-comparable).\n' "$REPEATS"
fi
printf 'Each timed run measures %s orbiting-camera frames after shader warmup.\n\n' "$MEASURE_FRAMES"

ANY_FAILURE=0
RUN_NUMBER=0
TOTAL_RUNS=$(( ${#SPP_VALUES[@]} * ${#BOUNCE_VALUES[@]} * REPEATS ))
for SPP in "${SPP_VALUES[@]}"; do
	for BOUNCES in "${BOUNCE_VALUES[@]}"; do
		for REPEAT in $(seq 1 "$REPEATS"); do
			RUN_NUMBER=$((RUN_NUMBER + 1))
			if [[ "$ARCHITECTURE" == 'arm64' ]]; then WORKLOAD="1920x1080-${SPP}spp-${BOUNCES}bounces-orbit-metal-rt"; else WORKLOAD='1920x1080-orbit-intel-raster-fallback'; fi
			LOG="$RESULT_DIR/spp-${SPP}-bounces-${BOUNCES}-run-${REPEAT}.log"
			printf 'Run %d of %d: %s (repeat %d/%d)...\n' "$RUN_NUMBER" "$TOTAL_RUNS" "$WORKLOAD" "$REPEAT" "$REPEATS"
			GODOT_GPU_PROFILE=1 GODOT_PERF_MEASURE_FRAMES="$MEASURE_FRAMES" GODOT_PERF_SPP="$SPP" GODOT_PERF_BOUNCES="$BOUNCES" GODOT_PERF_DENOISER=0 "$BIN" --path "$PROJECT" --disable-vsync "${RENDERING_ARGS[@]}" 2>&1 | tee "$LOG"
			EXIT_CODE=${PIPESTATUS[0]}
			if [[ "$EXIT_CODE" -ne 0 ]]; then ANY_FAILURE=1; fi
			RT_GATE="$(grep -Eo 'C11_GATE=[^[:space:]]+' "$LOG" | tail -1 | cut -d= -f2-)"
			if [[ -z "$RT_GATE" ]]; then
				if [[ "$ARCHITECTURE" != 'arm64' ]]; then RT_GATE='unsupported_intel_raster_fallback'; elif grep -q 'Metal ray tracing: enabled' "$LOG"; then RT_GATE='enabled'; elif grep -q 'Metal ray tracing: disabled' "$LOG"; then RT_GATE='disabled'; else RT_GATE='not_reported'; fi
			fi
			AVG_FPS="$(metric PERF_ORBIT_AVG_FPS "$LOG")"; FRAME_AVG="$(metric PERF_ORBIT_FRAME_TIME_MS_AVG "$LOG")"; FRAME_P50="$(metric PERF_ORBIT_FRAME_TIME_MS_P50 "$LOG")"; FRAME_P95="$(metric PERF_ORBIT_FRAME_TIME_MS_P95 "$LOG")"; FRAME_P99="$(metric PERF_ORBIT_FRAME_TIME_MS_P99 "$LOG")"; FRAME_MAX="$(metric PERF_ORBIT_FRAME_TIME_MS_MAX "$LOG")"; CPU_AVG="$(metric PERF_ORBIT_CPU_TIME_MS_AVG "$LOG")"; CPU_P95="$(metric PERF_ORBIT_CPU_TIME_MS_P95 "$LOG")"; ADAPTER="$(metric PERF_ORBIT_VIDEO_ADAPTER "$LOG")"; GPU_AVG="$(gpu_stat avg "$LOG")"; GPU_MIN="$(gpu_stat min "$LOG")"; GPU_MAX="$(gpu_stat max "$LOG")"
			ROW="$(csv_quote "$TIMESTAMP"),$(csv_quote "$COMPUTER_NAME"),$(csv_quote "$MODEL_IDENTIFIER"),$(csv_quote "$CHIP"),$(csv_quote "$MACOS_VERSION"),$(csv_quote "$ARCHITECTURE"),$(csv_quote "$ENGINE_COMMIT"),$(csv_quote "$ENGINE_VERSION"),$SPP,$BOUNCES,$REPEAT,$EXIT_CODE,$(csv_quote "$RT_GATE"),$(csv_quote "$ADAPTER"),$AVG_FPS,$FRAME_AVG,$FRAME_P50,$FRAME_P95,$FRAME_P99,$FRAME_MAX,$CPU_AVG,$CPU_P95,$GPU_AVG,$GPU_MIN,$GPU_MAX,$(csv_quote "$WORKLOAD")"
			printf '%s\n' "$ROW" >> "$LOCAL_CSV"; printf '%s\n' "$ROW" >> "$MASTER_CSV"
			printf 'Result: exit=%d, RT=%s, average FPS=%s, p95 frame=%sms, Pathtracer GPU avg=%sms\n\n' "$EXIT_CODE" "$RT_GATE" "${AVG_FPS:-n/a}" "${FRAME_P95:-n/a}" "$GPU_AVG"
		done
	done
done

{
	printf 'Godot Metal RT Mac benchmark matrix summary\n===========================================\nTimestamp: %s\nMachine: %s\nModel: %s\nChip/CPU: %s\nmacOS: %s (%s)\nGodot: %s\nCommit: %s\nMatrix: SPP {1,4,16} × max bounces {1,2,4}, %d repeats\n\n' "$TIMESTAMP" "$COMPUTER_NAME" "$MODEL_IDENTIFIER" "$CHIP" "$MACOS_VERSION" "$ARCHITECTURE" "$ENGINE_VERSION" "$ENGINE_COMMIT" "$REPEATS"
	cat "$LOCAL_CSV"
	printf '\nRaw logs and the full hardware report are in this folder.\n'
} > "$RESULT_DIR/SUMMARY.txt"

printf 'Finished. Results: %s\n' "$RESULT_DIR"
open "$RESULT_DIR"
if [[ "$ANY_FAILURE" -eq 0 ]]; then /usr/bin/osascript -e 'display notification "Benchmark matrix finished; results and CSV are ready." with title "Godot Metal RT Benchmark"' >/dev/null 2>&1 || true; else /usr/bin/osascript -e 'display dialog "One or more benchmark runs failed. The results folder contains the raw logs." with title "Godot Metal RT Benchmark" buttons {"OK"} default button "OK"' >/dev/null 2>&1 || true; fi
printf 'You can close this window.\n'
