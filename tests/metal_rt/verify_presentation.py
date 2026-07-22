#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any

from image_diff import ImageDiffError, read_rgba8_png, write_rgba8_png


class VerificationError(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Verify the PRESENTATION Metal presentation and temporal-reset captures.")
    parser.add_argument("artifact_dir", type=Path)
    return parser.parse_args()


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise VerificationError(f"cannot read JSON {path}: {error}") from error
    if not isinstance(value, dict):
        raise VerificationError(f"expected a JSON object: {path}")
    return value


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_image(path: Path) -> dict[str, int]:
    width, height, data = read_rgba8_png(path)
    if (width, height) != (64, 64):
        raise VerificationError(f"expected 64x64 capture, got {width}x{height}: {path}")
    pixels = [tuple(data[offset : offset + 3]) for offset in range(0, len(data), 4)]
    unique = len(set(pixels))
    channel_range = max(max(pixel) for pixel in pixels) - min(min(pixel) for pixel in pixels)
    if unique < 32 or channel_range < 64:
        raise VerificationError(f"capture lacks scene variation: unique={unique}, range={channel_range}: {path}")
    return {"unique_rgb": unique, "channel_range": channel_range}


def compare_presentation(reference_path: Path, actual_path: Path, diff_path: Path) -> dict[str, Any]:
    reference_width, reference_height, reference = read_rgba8_png(reference_path)
    actual_width, actual_height, actual = read_rgba8_png(actual_path)
    if (actual_width, actual_height) != (reference_width, reference_height):
        raise VerificationError(
            f"presentation dimensions differ: reference={reference_width}x{reference_height}, "
            f"actual={actual_width}x{actual_height}"
        )

    rgb_differences: list[int] = []
    visual_diff = bytearray(len(actual))
    for offset in range(0, len(actual), 4):
        for channel in range(3):
            difference = abs(actual[offset + channel] - reference[offset + channel])
            rgb_differences.append(difference)
            visual_diff[offset + channel] = min(255, difference * 4)
        visual_diff[offset + 3] = 255
    write_rgba8_png(diff_path, actual_width, actual_height, bytes(visual_diff))
    differing_pixels = sum(
        actual[offset : offset + 3] != reference[offset : offset + 3]
        for offset in range(0, len(actual), 4)
    )
    return {
        "reference": reference_path.name,
        "actual": actual_path.name,
        "diff": diff_path.name,
        "max_rgb_difference": max(rgb_differences, default=0),
        "mean_absolute_rgb_difference": sum(rgb_differences) / len(rgb_differences) if rgb_differences else 0.0,
        "differing_pixels": differing_pixels,
    }


def verify(artifact_dir: Path) -> int:
    manifest = load_json(artifact_dir / "presentation_manifest.json")
    required = {
        "fixture": "e0_hg0",
        "fixture_revision": "e0-hg0-v1",
        "renderer": "forward_plus",
        "rendering_driver": "metal",
        "resolution": [64, 64],
        "ser": "disabled",
    }
    mismatches = [
        f"{key}={manifest.get(key)!r}, expected {expected!r}"
        for key, expected in required.items()
        if manifest.get(key) != expected
    ]
    if mismatches:
        raise VerificationError("manifest contract mismatch: " + "; ".join(mismatches))

    modes = manifest.get("presentation_modes")
    if not isinstance(modes, list) or not {"native", "fsr1", "fsr2"}.issubset(modes):
        raise VerificationError(f"presentation matrix is incomplete: {modes!r}")

    denoiser = manifest.get("denoiser")
    native_denoising = manifest.get("native_denoising")
    if denoiser == "metalfx":
        if native_denoising != "metalfx" or "metalfx_denoised" not in modes:
            raise VerificationError(
                "MetalFX denoising was advertised without its presentation capture: "
                f"native_denoising={native_denoising!r}, modes={modes!r}"
            )
    elif denoiser == "none":
        if native_denoising != "unavailable":
            raise VerificationError(f"unexpected native denoising state: {native_denoising!r}")
    else:
        raise VerificationError(f"invalid denoiser contract: {denoiser!r}")

    temporal_mode = manifest.get("temporal_test_mode")
    if temporal_mode not in {"fsr2", "metalfx_temporal", "metalfx_denoised"}:
        raise VerificationError(f"invalid temporal test mode: {temporal_mode!r}")

    expected_events = {
        "camera_cut",
        "resize",
        "editor_pause",
        "editor_resume",
        "viewport_switch",
        "viewport_return",
    }
    events = manifest.get("events")
    if not isinstance(events, list) or not expected_events.issubset(events):
        raise VerificationError(f"temporal event trace is incomplete: {events!r}")

    captures = manifest.get("captures")
    if not isinstance(captures, dict):
        raise VerificationError("manifest is missing capture hashes")
    expected_captures = set(modes) | {
        "before_camera_cut",
        "after_camera_cut",
        "after_resize",
        "after_pause_resume",
        "secondary_viewport",
        "after_viewport_return",
    }
    if not expected_captures.issubset(captures):
        raise VerificationError(f"capture matrix is incomplete: missing={sorted(expected_captures - captures.keys())}")

    metrics: dict[str, dict[str, int]] = {}
    for name in sorted(expected_captures):
        image_path = artifact_dir / f"presentation_{name}.png"
        if captures[name] != sha256(image_path):
            raise VerificationError(f"capture digest mismatch: {image_path}")
        metrics[name] = validate_image(image_path)

    comparisons: dict[str, dict[str, Any]] = {}
    native_path = artifact_dir / "presentation_native.png"
    for mode in modes:
        if mode == "native":
            continue
        actual_path = artifact_dir / f"presentation_{mode}.png"
        diff_path = artifact_dir / f"presentation_native_vs_{mode}_diff.png"
        comparisons[mode] = compare_presentation(native_path, actual_path, diff_path)

    sanitized_path = artifact_dir / str(manifest.get("sanitized_environment", ""))
    serialized = sanitized_path.read_text(encoding="utf-8")
    if "pathtracing_denoiser = 1" in serialized:
        raise VerificationError("sanitized Environment retained DLSS Ray Reconstruction")

    result = {
        "schema_version": 1,
        "status": "passed",
        "presentation_modes": modes,
        "temporal_test_mode": temporal_mode,
        "events": events,
        "captures": metrics,
        "native_comparisons": comparisons,
    }
    (artifact_dir / "presentation_metrics.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(f"METAL_RT_PRESENTATION_VERIFY=passed modes={','.join(modes)} captures={len(metrics)}")
    return 0


def main() -> int:
    args = parse_args()
    try:
        return verify(args.artifact_dir.resolve())
    except (OSError, ImageDiffError, VerificationError) as error:
        print(f"METAL_RT_PRESENTATION_VERIFY=failed error={error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
