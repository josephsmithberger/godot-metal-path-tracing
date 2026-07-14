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

FIXTURE = "e1_geometry"
FIXTURE_REVISION = "e1-geometry-v1"
LABELS = ("cold", "reload")
KINDS = (
    "initial_beauty",
    "initial_instance_id",
    "initial_primitive_id",
    "mutated_beauty",
    "mutated_instance_id",
    "mutated_primitive_id",
)


class VerificationError(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Verify the C14 geometry editor-scene capture contract.")
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


def rgb_pixels(data: bytes) -> list[tuple[int, int, int]]:
    return [tuple(data[offset : offset + 3]) for offset in range(0, len(data), 4)]


def validate_manifest(artifact_dir: Path, label: str) -> None:
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
        "fixture_object_count": 6,
        "multimesh_instance_count": 3,
        "editor_process": True,
    }
    mismatches = [
        f"{key}={manifest.get(key)!r}, expected {expected!r}"
        for key, expected in required.items()
        if manifest.get(key) != expected
    ]
    if mismatches:
        raise VerificationError(f"manifest contract mismatch in {path}: " + "; ".join(mismatches))

    expected_matrix = {
        "indexed_float3",
        "nonindexed_float3",
        "compressed_unorm16x4",
        "blend_shape_deformed_refit",
        "multimesh_repeated",
        "negative_scale",
    }
    if set(manifest.get("geometry_matrix", [])) != expected_matrix:
        raise VerificationError(f"geometry capability matrix mismatch in {path}")
    captures = manifest.get("captures")
    if not isinstance(captures, dict):
        raise VerificationError(f"missing capture hash map in {path}")
    for kind in KINDS:
        image_path = artifact_dir / f"{FIXTURE}_{label}_{kind}.png"
        if captures.get(kind) != sha256(image_path):
            raise VerificationError(f"capture digest mismatch for {image_path}")


def image_semantics(kind: str, data: bytes) -> dict[str, int]:
    pixels = rgb_pixels(data)
    unique = len(set(pixels))
    channel_range = max(max(pixel) for pixel in pixels) - min(min(pixel) for pixel in pixels)
    if "beauty" in kind:
        if unique < 64 or channel_range < 80:
            raise VerificationError(f"{kind} lacks scene variation: unique={unique}, range={channel_range}")
        return {"unique_rgb": unique, "channel_range": channel_range}

    quantized = Counter(tuple(channel // 32 for channel in pixel) for pixel in pixels)
    dark_pixels = sum(count for color, count in quantized.items() if max(color) == 0)
    colored_regions = sum(1 for color, count in quantized.items() if max(color) >= 2 and count >= 6)
    minimum_regions = 5 if "instance" in kind else 3
    if dark_pixels < 32 or colored_regions < minimum_regions:
        raise VerificationError(
            f"{kind} lacks miss and stable ID regions: dark={dark_pixels}, regions={colored_regions}"
        )
    return {"dark_pixels": dark_pixels, "colored_regions": colored_regions}


def verify(artifact_dir: Path) -> int:
    for label in LABELS:
        validate_manifest(artifact_dir, label)

    decoded: dict[tuple[str, str], bytes] = {}
    semantics: dict[str, dict[str, int]] = {}
    for label in LABELS:
        for kind in KINDS:
            image_path = artifact_dir / f"{FIXTURE}_{label}_{kind}.png"
            width, height, data = read_rgba8_png(image_path)
            if (width, height) != (64, 64):
                raise VerificationError(f"expected 64x64 image, got {width}x{height}: {image_path}")
            decoded[label, kind] = data
            if label == "cold":
                semantics[kind] = image_semantics(kind, data)

    comparisons: dict[str, dict[str, Any]] = {}
    for kind in KINDS:
        reference = decoded["cold", kind]
        actual = decoded["reload", kind]
        max_difference = max((abs(a - b) for a, b in zip(actual, reference)), default=0)
        differing_pixels = sum(
            1
            for offset in range(0, len(actual), 4)
            if actual[offset : offset + 4] != reference[offset : offset + 4]
        )
        if max_difference != 0:
            raise VerificationError(
                f"reload {kind} differs from cold capture: max_difference={max_difference}, pixels={differing_pixels}"
            )
        diff_path = artifact_dir / f"{FIXTURE}_reload_vs_cold_{kind}_diff.png"
        write_rgba8_png(diff_path, 64, 64, bytes([0, 0, 0, 255]) * (64 * 64))
        comparisons[kind] = {
            "comparison_mode": "exact_pixels",
            "max_channel_difference": max_difference,
            "differing_pixels": differing_pixels,
            "diff": diff_path.name,
        }

    initial = decoded["cold", "initial_beauty"]
    mutated = decoded["cold", "mutated_beauty"]
    mutation_pixels = sum(
        1
        for offset in range(0, len(initial), 4)
        if initial[offset : offset + 4] != mutated[offset : offset + 4]
    )
    if mutation_pixels < 16:
        raise VerificationError(f"geometry mutation did not materially change the beauty image: {mutation_pixels} pixels")

    metrics = {
        "schema_version": 1,
        "status": "passed",
        "fixture": FIXTURE,
        "fixture_revision": FIXTURE_REVISION,
        "comparison": comparisons,
        "semantics": semantics,
        "mutation_differing_pixels": mutation_pixels,
    }
    metrics_path = artifact_dir / f"{FIXTURE}_metrics.json"
    metrics_path.write_text(json.dumps(metrics, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(
        "METAL_RT_C14_FIXTURE_VERIFY=passed "
        f"instance_regions={semantics['initial_instance_id']['colored_regions']} "
        f"primitive_regions={semantics['initial_primitive_id']['colored_regions']} "
        f"mutation_pixels={mutation_pixels}"
    )
    return 0


def main() -> int:
    args = parse_args()
    try:
        return verify(args.artifact_dir.resolve())
    except (OSError, ImageDiffError, VerificationError) as error:
        print(f"METAL_RT_C14_FIXTURE_VERIFY=failed error={error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
