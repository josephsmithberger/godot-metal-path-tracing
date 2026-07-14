#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from collections import Counter
from pathlib import Path
from typing import Any

from image_diff import ImageDiffError, read_rgba8_png, write_rgba8_png

FIXTURE = "e0_hg0"
FIXTURE_REVISION = "e0-hg0-v1"
LABELS = ("cold", "reload")
KINDS = ("beauty", "instance_id")


class VerificationError(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Verify the C13 editor-scene capture contract.")
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


def pixels(data: bytes) -> list[tuple[int, int, int]]:
    return [tuple(data[offset : offset + 3]) for offset in range(0, len(data), 4)]


def validate_manifest(artifact_dir: Path, label: str) -> dict[str, Any]:
    path = artifact_dir / f"{FIXTURE}_{label}_manifest.json"
    manifest = load_json(path)
    required = {
        "fixture": FIXTURE,
        "fixture_revision": FIXTURE_REVISION,
        "capture_label": label,
        "renderer": "forward_plus",
        "rendering_driver": "metal",
        "resolution": [64, 64],
        "format": "rgba8_png",
        "samples_per_pixel": 4,
        "max_bounces": 2,
        "denoiser": "none",
        "upscaler": "none",
        "editor_process": True,
    }
    mismatches = [f"{key}={manifest.get(key)!r}, expected {expected!r}" for key, expected in required.items() if manifest.get(key) != expected]
    if mismatches:
        raise VerificationError(f"manifest contract mismatch in {path}: " + "; ".join(mismatches))

    for kind in KINDS:
        image_path = artifact_dir / f"{FIXTURE}_{label}_{kind}.png"
        digest_key = f"{kind}_sha256"
        digest = sha256(image_path)
        if manifest.get(digest_key) != digest:
            raise VerificationError(f"{digest_key} mismatch for {image_path}")
    return manifest


def validate_semantics(beauty: bytes, instance_id: bytes) -> dict[str, int]:
    beauty_pixels = pixels(beauty)
    id_pixels = pixels(instance_id)
    beauty_unique = len(set(beauty_pixels))
    beauty_range = max(max(pixel) for pixel in beauty_pixels) - min(min(pixel) for pixel in beauty_pixels)
    if beauty_unique < 64 or beauty_range < 96:
        raise VerificationError(
            f"beauty image lacks expected scene variation: unique={beauty_unique}, range={beauty_range}"
        )

    quantized = Counter(tuple(channel // 32 for channel in pixel) for pixel in id_pixels)
    dark_pixels = sum(count for color, count in quantized.items() if max(color) == 0)
    object_bins = [color for color, count in quantized.items() if max(color) >= 2 and count >= 16]
    if dark_pixels < 64 or len(object_bins) < 3:
        raise VerificationError(
            f"instance-ID mask lacks miss plus three stable object regions: dark={dark_pixels}, object_bins={len(object_bins)}"
        )
    return {
        "beauty_unique_rgb": beauty_unique,
        "beauty_channel_range": beauty_range,
        "instance_id_dark_pixels": dark_pixels,
        "instance_id_object_bins": len(object_bins),
    }


def verify(artifact_dir: Path) -> int:
    for label in LABELS:
        validate_manifest(artifact_dir, label)

    decoded: dict[tuple[str, str], bytes] = {}
    dimensions: dict[tuple[str, str], tuple[int, int]] = {}
    for label in LABELS:
        for kind in KINDS:
            image_path = artifact_dir / f"{FIXTURE}_{label}_{kind}.png"
            width, height, data = read_rgba8_png(image_path)
            if (width, height) != (64, 64):
                raise VerificationError(f"expected 64x64 image, got {width}x{height}: {image_path}")
            decoded[label, kind] = data
            dimensions[label, kind] = (width, height)

    semantic_metrics = validate_semantics(decoded["cold", "beauty"], decoded["cold", "instance_id"])
    comparisons: dict[str, dict[str, Any]] = {}
    for kind in KINDS:
        reference = decoded["cold", kind]
        actual = decoded["reload", kind]
        channel_differences = [abs(a - b) for a, b in zip(actual, reference)]
        max_difference = max(channel_differences, default=0)
        differing_pixels = sum(
            1
            for offset in range(0, len(actual), 4)
            if actual[offset : offset + 4] != reference[offset : offset + 4]
        )
        if max_difference != 0:
            raise VerificationError(
                f"reload {kind} pixels differ from cold capture: max_difference={max_difference}, differing_pixels={differing_pixels}"
            )
        diff_path = artifact_dir / f"{FIXTURE}_reload_vs_cold_{kind}_diff.png"
        write_rgba8_png(diff_path, *dimensions["cold", kind], bytes([0, 0, 0, 255]) * (64 * 64))
        comparisons[kind] = {
            "comparison_mode": "exact_pixels",
            "reference": f"{FIXTURE}_cold_{kind}.png",
            "actual": f"{FIXTURE}_reload_{kind}.png",
            "diff": diff_path.name,
            "max_channel_difference": max_difference,
            "differing_pixels": differing_pixels,
        }

    metrics = {
        "schema_version": 1,
        "status": "passed",
        "fixture": FIXTURE,
        "fixture_revision": FIXTURE_REVISION,
        "comparison": comparisons,
        "semantics": semantic_metrics,
    }
    metrics_path = artifact_dir / f"{FIXTURE}_metrics.json"
    metrics_path.write_text(json.dumps(metrics, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(
        "METAL_RT_C13_FIXTURE_VERIFY=passed "
        f"beauty_unique={semantic_metrics['beauty_unique_rgb']} "
        f"instance_regions={semantic_metrics['instance_id_object_bins']}"
    )
    return 0


def main() -> int:
    args = parse_args()
    try:
        return verify(args.artifact_dir.resolve())
    except (OSError, ImageDiffError, VerificationError) as error:
        print(f"METAL_RT_C13_FIXTURE_VERIFY=failed error={error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
