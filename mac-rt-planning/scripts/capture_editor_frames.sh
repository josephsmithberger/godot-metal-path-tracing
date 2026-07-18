#!/bin/zsh
# Capture full-resolution editor 3D viewport frames from the e3 debug scene.
#
# The shipping fixture captures downscale to 64x64 (Lanczos), which averages
# away 8x4-pixel SIMD-group tearing; this script saves the viewport readback
# at native resolution while the camera orbits (frames moving_*.png) and then
# holds still (static_*.png). See
# tests/metal_rt/editor/scripts/debug_presented_capture.gd for the schedule.
#
# Usage: capture_editor_frames.sh [output-dir] [extra godot env pairs...]
#   capture_editor_frames.sh
#   capture_editor_frames.sh /tmp/run-barriers GODOT_MTL_FORCE_BARRIERS=1

set -euo pipefail
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT_DIR="${1:-$REPO_ROOT/mac-rt-planning/artifacts/dbg-editor-frames-$(date +%Y%m%dT%H%M%S)}"
shift 2>/dev/null || true

rm -f "$REPO_ROOT/tests/metal_rt/editor/.godot/editor/editor_layout.cfg"
env GODOT_DBG_CAPTURE_DIR="$OUT_DIR" "$@" \
	"$REPO_ROOT/bin/godot.macos.editor.arm64" \
	--editor \
	--path "$REPO_ROOT/tests/metal_rt/editor" \
	res://fixtures/e3_debug.tscn \
	--quit-after 600
echo "Frames in: $OUT_DIR"
