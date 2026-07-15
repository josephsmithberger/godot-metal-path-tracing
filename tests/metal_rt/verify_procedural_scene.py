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

FIXTURE = "e3_procedural"
FIXTURE_REVISION = "e3-procedural-v1"
LABELS = ("cold", "reload")
KINDS = (
    "initial_beauty",
    "initial_instance_id",
    "initial_primitive_id",
    "initial_hit_kind",
    "mutated_beauty",
    "mutated_instance_id",
    "mutated_primitive_id",
    "mutated_hit_kind",
)


class VerificationError(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Verify the C16 Metal procedural-scene capture contract.")
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
        "initial_fixture_object_count": 6,
        "mutated_fixture_object_count": 5,
        "procedural_instances_removed": 1,
        "procedural_refits_expected": 1,
        "editor_process": True,
        "invalid_bounds_policy": "omit invalid procedural geometry; retain valid mixed-scene triangles; warn once",
    }
    mismatches = [
        f"{key}={manifest.get(key)!r}, expected {expected!r}"
        for key, expected in required.items()
        if manifest.get(key) != expected
    ]
    if mismatches:
        raise VerificationError(f"manifest contract mismatch in {path}: " + "; ".join(mismatches))

    expected_support = {
        "triangle",
        "procedural_fallback_bounds",
        "procedural_explicit_bounds",
        "multi_aabb",
        "custom_intersection",
        "bounds_refit",
        "instance_removal",
    }
    if set(manifest.get("support_matrix", [])) != expected_support:
        raise VerificationError(f"procedural support matrix mismatch in {path}")
    captures = manifest.get("captures")
    if not isinstance(captures, dict):
        raise VerificationError(f"missing capture hash map in {path}")
    for kind in KINDS:
        image_path = artifact_dir / f"{FIXTURE}_{label}_{kind}.png"
        if captures.get(kind) != sha256(image_path):
            raise VerificationError(f"capture digest mismatch for {image_path}")


def quantized_regions(data: bytes, divisor: int = 32, minimum_pixels: int = 6) -> int:
    colors = Counter(tuple(channel // divisor for channel in pixel) for pixel in rgb_pixels(data))
    return sum(1 for color, count in colors.items() if max(color) >= 2 and count >= minimum_pixels)


def validate_semantics(decoded: dict[tuple[str, str], bytes]) -> dict[str, Any]:
    initial_beauty = decoded["cold", "initial_beauty"]
    mutated_beauty = decoded["cold", "mutated_beauty"]
    initial_pixels = rgb_pixels(initial_beauty)
    unique = len(set(initial_pixels))
    channel_range = max(max(pixel) for pixel in initial_pixels) - min(min(pixel) for pixel in initial_pixels)
    if unique < 64 or channel_range < 80:
        raise VerificationError(f"beauty image lacks scene variation: unique={unique}, range={channel_range}")

    invalid_magenta_pixels = sum(
        1
        for red, green, blue in initial_pixels
        if red >= 120 and blue >= 120 and green + 40 < min(red, blue)
    )
    if invalid_magenta_pixels > 4:
        raise VerificationError(
            f"invalid procedural material leaked into the mixed-scene beauty image: pixels={invalid_magenta_pixels}"
        )

    initial_instance_regions = quantized_regions(decoded["cold", "initial_instance_id"])
    mutated_instance_regions = quantized_regions(decoded["cold", "mutated_instance_id"])
    if initial_instance_regions < 4:
        raise VerificationError(
            f"instance-ID output lacks mixed triangle/procedural instances: regions={initial_instance_regions}"
        )
    if mutated_instance_regions >= initial_instance_regions:
        raise VerificationError(
            "instance removal did not reduce visible instance-ID regions: "
            f"{initial_instance_regions} -> {mutated_instance_regions}"
        )

    primitive_regions = quantized_regions(decoded["cold", "initial_primitive_id"], minimum_pixels=3)
    if primitive_regions < 3:
        raise VerificationError(f"primitive-ID output lacks propagated primitive identities: regions={primitive_regions}")

    kind_pixels = rgb_pixels(decoded["cold", "initial_hit_kind"])
    procedural_pixels = sum(1 for red, green, blue in kind_pixels if red > blue + 24 and red > green + 8)
    triangle_pixels = sum(1 for red, green, blue in kind_pixels if blue > red + 24 and blue > green + 8)
    if procedural_pixels < 16 or triangle_pixels < 16:
        raise VerificationError(
            "hit-kind output does not contain both generated and triangle hits: "
            f"procedural={procedural_pixels}, triangle={triangle_pixels}"
        )

    mutation_pixels = sum(
        1
        for offset in range(0, len(initial_beauty), 4)
        if initial_beauty[offset : offset + 4] != mutated_beauty[offset : offset + 4]
    )
    if mutation_pixels < 48:
        raise VerificationError(f"bounds update/removal changed only {mutation_pixels} pixels")

    mutated_kind_pixels = rgb_pixels(decoded["cold", "mutated_hit_kind"])
    mutated_procedural_pixels = sum(
        1 for red, green, blue in mutated_kind_pixels if red > blue + 24 and red > green + 8
    )
    if mutated_procedural_pixels >= procedural_pixels:
        raise VerificationError(
            "procedural removal did not reduce generated-hit coverage: "
            f"{procedural_pixels} -> {mutated_procedural_pixels}"
        )

    return {
        "beauty_unique_rgb": unique,
        "beauty_channel_range": channel_range,
        "invalid_magenta_pixels": invalid_magenta_pixels,
        "initial_instance_id_regions": initial_instance_regions,
        "mutated_instance_id_regions": mutated_instance_regions,
        "primitive_id_regions": primitive_regions,
        "initial_procedural_hit_pixels": procedural_pixels,
        "mutated_procedural_hit_pixels": mutated_procedural_pixels,
        "triangle_hit_pixels": triangle_pixels,
        "mutation_differing_pixels": mutation_pixels,
        "invalid_procedural_excluded": invalid_magenta_pixels <= 4,
    }


def verify(artifact_dir: Path) -> int:
    for label in LABELS:
        validate_manifest(artifact_dir, label)

    decoded: dict[tuple[str, str], bytes] = {}
    for label in LABELS:
        for kind in KINDS:
            image_path = artifact_dir / f"{FIXTURE}_{label}_{kind}.png"
            width, height, data = read_rgba8_png(image_path)
            if (width, height) != (64, 64):
                raise VerificationError(f"expected 64x64 image, got {width}x{height}: {image_path}")
            decoded[label, kind] = data

    semantics = validate_semantics(decoded)
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
        is_beauty = kind.endswith("beauty")
        allowed_channel_difference = 2 if is_beauty else 0
        allowed_differing_pixels = 700 if is_beauty else 0
        if max_difference > allowed_channel_difference or differing_pixels > allowed_differing_pixels:
            raise VerificationError(
                f"reload {kind} differs from cold capture: max_difference={max_difference}, pixels={differing_pixels}"
            )
        diff_path = artifact_dir / f"{FIXTURE}_reload_vs_cold_{kind}_diff.png"
        diff = bytearray(64 * 64 * 4)
        for offset in range(0, len(actual), 4):
            changed = actual[offset : offset + 4] != reference[offset : offset + 4]
            diff[offset : offset + 4] = bytes([255, 255, 255, 255] if changed else [0, 0, 0, 255])
        write_rgba8_png(diff_path, 64, 64, bytes(diff))
        comparisons[kind] = {
            "comparison_mode": "threshold_pixels" if is_beauty else "exact_decoded_pixels",
            "max_allowed_channel_difference": allowed_channel_difference,
            "max_allowed_differing_pixels": allowed_differing_pixels,
            "max_channel_difference": max_difference,
            "differing_pixels": differing_pixels,
            "diff": diff_path.name,
        }

    metrics = {
        "schema_version": 1,
        "status": "passed",
        "fixture": FIXTURE,
        "fixture_revision": FIXTURE_REVISION,
        "comparison": comparisons,
        "semantics": semantics,
    }
    metrics_path = artifact_dir / f"{FIXTURE}_metrics.json"
    metrics_path.write_text(json.dumps(metrics, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(
        "METAL_RT_C16_FIXTURE_VERIFY=passed "
        f"procedural_pixels={semantics['initial_procedural_hit_pixels']} "
        f"triangle_pixels={semantics['triangle_hit_pixels']} "
        f"mutation_pixels={semantics['mutation_differing_pixels']}"
    )
    return 0


def main() -> int:
    args = parse_args()
    try:
        return verify(args.artifact_dir.resolve())
    except (OSError, ImageDiffError, VerificationError) as error:
        print(f"METAL_RT_C16_FIXTURE_VERIFY=failed error={error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
