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

FIXTURE = "e2_materials"
FIXTURE_REVISION = "e2-materials-v1"
LABELS = ("cold", "reload")
KINDS = ("initial_beauty", "initial_material_id", "mutated_beauty", "mutated_material_id")


class VerificationError(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Verify the C15 Metal material-dispatch capture contract.")
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
        "fixture_object_count": 7,
        "editor_process": True,
        "failed_shader_policy": "stage-global custom helper excluded with actionable diagnostic",
    }
    mismatches = [
        f"{key}={manifest.get(key)!r}, expected {expected!r}"
        for key, expected in required.items()
        if manifest.get(key) != expected
    ]
    if mismatches:
        raise VerificationError(f"manifest contract mismatch in {path}: " + "; ".join(mismatches))

    expected_matrix = {"opaque", "alpha_scissor", "double_sided", "textured", "custom_uniform_texture"}
    if set(manifest.get("material_matrix", [])) != expected_matrix:
        raise VerificationError(f"material capability matrix mismatch in {path}")
    captures = manifest.get("captures")
    if not isinstance(captures, dict):
        raise VerificationError(f"missing capture hash map in {path}")
    for kind in KINDS:
        image_path = artifact_dir / f"{FIXTURE}_{label}_{kind}.png"
        if captures.get(kind) != sha256(image_path):
            raise VerificationError(f"capture digest mismatch for {image_path}")


def dominant_quantized_region(data: bytes, x0: int, y0: int, x1: int, y1: int) -> tuple[int, int, int]:
    colors: Counter[tuple[int, int, int]] = Counter()
    for y in range(y0, y1):
        for x in range(x0, x1):
            offset = (y * 64 + x) * 4
            colors.update([(data[offset] // 16, data[offset + 1] // 16, data[offset + 2] // 16)])
    return colors.most_common(1)[0][0]


def region_mean_rgb(data: bytes, x0: int, y0: int, x1: int, y1: int) -> tuple[float, float, float]:
    totals = [0, 0, 0]
    count = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            offset = (y * 64 + x) * 4
            for channel in range(3):
                totals[channel] += data[offset + channel]
            count += 1
    return tuple(total / count for total in totals)


def validate_semantics(decoded: dict[tuple[str, str], bytes]) -> dict[str, Any]:
    initial_beauty = decoded["cold", "initial_beauty"]
    mutated_beauty = decoded["cold", "mutated_beauty"]
    initial_pixels = rgb_pixels(initial_beauty)
    unique = len(set(initial_pixels))
    channel_range = max(max(pixel) for pixel in initial_pixels) - min(min(pixel) for pixel in initial_pixels)
    if unique < 64 or channel_range < 80:
        raise VerificationError(f"beauty image lacks scene variation: unique={unique}, range={channel_range}")

    id_pixels = rgb_pixels(decoded["cold", "initial_material_id"])
    quantized = Counter(tuple(channel // 32 for channel in pixel) for pixel in id_pixels)
    colored_regions = sum(1 for color, count in quantized.items() if max(color) >= 2 and count >= 6)
    if colored_regions < 6:
        raise VerificationError(f"material-ID output lacks the floor plus five supported materials: regions={colored_regions}")

    mutation_pixels = sum(
        1
        for offset in range(0, len(initial_beauty), 4)
        if initial_beauty[offset : offset + 4] != mutated_beauty[offset : offset + 4]
    )
    if mutation_pixels < 64:
        raise VerificationError(f"shader/uniform/alpha mutation changed only {mutation_pixels} pixels")

    # The custom box is the right-most supported object in the fixed E2 composition.
    initial_custom = region_mean_rgb(initial_beauty, 40, 27, 47, 36)
    mutated_custom = region_mean_rgb(mutated_beauty, 40, 27, 47, 36)
    custom_delta = sum(abs(after - before) for before, after in zip(initial_custom, mutated_custom))
    if custom_delta < 24.0:
        raise VerificationError(
            f"custom shader source/uniform reload is not visually material: delta={custom_delta:.3f}"
        )

    initial_custom_id = dominant_quantized_region(decoded["cold", "initial_material_id"], 40, 27, 47, 36)
    mutated_custom_id = dominant_quantized_region(decoded["cold", "mutated_material_id"], 40, 27, 47, 36)
    if initial_custom_id != mutated_custom_id:
        raise VerificationError(
            f"custom material identity changed across source reload: {initial_custom_id} -> {mutated_custom_id}"
        )

    # The cutout card has a 55%-alpha field: it passes the initial .45 threshold
    # and is rejected after the scripted .62 edit. Material ID makes this a
    # lighting-independent assertion about candidate confirmation.
    initial_material_id = decoded["cold", "initial_material_id"]
    mutated_material_id = decoded["cold", "mutated_material_id"]
    alpha_silhouette_pixels = sum(
        1
        for y in range(24, 38)
        for x in range(18, 27)
        if initial_material_id[(y * 64 + x) * 4 : (y * 64 + x + 1) * 4]
        != mutated_material_id[(y * 64 + x) * 4 : (y * 64 + x + 1) * 4]
    )
    if alpha_silhouette_pixels < 12:
        raise VerificationError(
            f"alpha-threshold reload changed only {alpha_silhouette_pixels} cutout pixels"
        )

    # The intentionally unsupported helper material would occupy this far-right
    # box. Its Material-ID output must instead remain the floor behind it.
    rejected_helper_id = dominant_quantized_region(initial_material_id, 48, 27, 56, 37)
    floor_id = dominant_quantized_region(initial_material_id, 48, 38, 56, 46)
    if rejected_helper_id != floor_id:
        raise VerificationError(
            f"rejected helper surface leaked into Material ID: {rejected_helper_id} != floor {floor_id}"
        )

    return {
        "beauty_unique_rgb": unique,
        "beauty_channel_range": channel_range,
        "material_id_regions": colored_regions,
        "mutation_differing_pixels": mutation_pixels,
        "custom_reload_rgb_delta": round(custom_delta, 3),
        "custom_material_id_bin": initial_custom_id,
        "alpha_silhouette_pixels": alpha_silhouette_pixels,
        "rejected_helper_excluded": True,
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
        if max_difference > 1 or differing_pixels > 64:
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
            "comparison_mode": "near_exact_pixels",
            "max_allowed_channel_difference": 1,
            "max_allowed_differing_pixels": 64,
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
        "METAL_RT_C15_FIXTURE_VERIFY=passed "
        f"material_regions={semantics['material_id_regions']} "
        f"alpha_pixels={semantics['alpha_silhouette_pixels']} "
        f"mutation_pixels={semantics['mutation_differing_pixels']} "
        f"custom_delta={semantics['custom_reload_rgb_delta']}"
    )
    return 0


def main() -> int:
    args = parse_args()
    try:
        return verify(args.artifact_dir.resolve())
    except (OSError, ImageDiffError, VerificationError) as error:
        print(f"METAL_RT_C15_FIXTURE_VERIFY=failed error={error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
