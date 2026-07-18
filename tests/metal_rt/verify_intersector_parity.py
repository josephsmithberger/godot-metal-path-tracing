#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any

from image_diff import ImageDiffError, read_rgba8_png, write_rgba8_png

FIXTURES: dict[str, dict[str, Any]] = {
    "e0_hg0": {
        "revision": "e0-hg0-v1",
        "kinds": {
            "beauty": (0, 0),
            "instance_id": (0, 0),
        },
    },
    "e1_geometry": {
        "revision": "e1-geometry-v1",
        "kinds": {
            "initial_beauty": (4, 4096),
            "initial_instance_id": (1, 128),
            "initial_primitive_id": (1, 128),
            "mutated_beauty": (4, 4096),
            "mutated_instance_id": (1, 128),
            "mutated_primitive_id": (1, 128),
        },
    },
    "e2_materials": {
        "revision": "e2-materials-v1",
        "kinds": {
            "initial_beauty": (4, 4096),
            "mutated_beauty": (4, 4096),
        },
    },
}


class VerificationError(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Compare default and forced-query Metal RT captures.")
    parser.add_argument("artifact_dir", type=Path)
    parser.add_argument("fixture", choices=tuple(FIXTURES))
    return parser.parse_args()


def load_manifest(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise VerificationError(f"cannot read manifest {path}: {error}") from error
    if not isinstance(value, dict):
        raise VerificationError(f"expected a JSON object: {path}")
    return value


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(artifact_dir: Path, fixture: str) -> int:
    contract = FIXTURES[fixture]
    manifests: dict[str, dict[str, Any]] = {}
    for label in ("cold", "query"):
        path = artifact_dir / f"{fixture}_{label}_manifest.json"
        manifest = load_manifest(path)
        required = {
            "fixture": fixture,
            "fixture_revision": contract["revision"],
            "capture_label": label,
            "rendering_driver": "metal",
            "resolution": [64, 64],
            "format": "rgba8_png",
            "denoiser": "none",
        }
        mismatches = [
            f"{key}={manifest.get(key)!r}, expected {expected!r}"
            for key, expected in required.items()
            if manifest.get(key) != expected
        ]
        if mismatches:
            raise VerificationError(f"manifest contract mismatch in {path}: " + "; ".join(mismatches))
        manifests[label] = manifest

    comparisons: dict[str, dict[str, Any]] = {}
    for kind, (max_channel_difference, max_differing_pixels) in contract["kinds"].items():
        images: dict[str, bytes] = {}
        for label in ("cold", "query"):
            path = artifact_dir / f"{fixture}_{label}_{kind}.png"
            width, height, data = read_rgba8_png(path)
            if (width, height) != (64, 64):
                raise VerificationError(f"expected 64x64 image, got {width}x{height}: {path}")
            if manifests[label].get("captures", {}).get(kind) != sha256(path):
                raise VerificationError(f"capture digest mismatch: {path}")
            images[label] = data

        reference = images["cold"]
        actual = images["query"]
        max_difference = max((abs(a - b) for a, b in zip(actual, reference)), default=0)
        differing_pixels = sum(
            1
            for offset in range(0, len(actual), 4)
            if actual[offset : offset + 4] != reference[offset : offset + 4]
        )
        if max_difference > max_channel_difference or differing_pixels > max_differing_pixels:
            raise VerificationError(
                f"forced-query {kind} differs from default: max_difference={max_difference}, "
                f"pixels={differing_pixels}"
            )

        diff = bytearray(64 * 64 * 4)
        for offset in range(0, len(actual), 4):
            changed = actual[offset : offset + 4] != reference[offset : offset + 4]
            diff[offset : offset + 4] = bytes([255, 255, 255, 255] if changed else [0, 0, 0, 255])
        diff_path = artifact_dir / f"{fixture}_query_vs_default_{kind}_diff.png"
        write_rgba8_png(diff_path, 64, 64, bytes(diff))
        comparisons[kind] = {
            "comparison_mode": "exact_pixels" if max_channel_difference == 0 else "near_exact_pixels",
            "allowed_max_channel_difference": max_channel_difference,
            "allowed_differing_pixels": max_differing_pixels,
            "max_channel_difference": max_difference,
            "differing_pixels": differing_pixels,
            "diff": diff_path.name,
        }

    metrics = {
        "schema_version": 1,
        "status": "passed",
        "fixture": fixture,
        "fixture_revision": contract["revision"],
        "default_lane": "GODOT_MTL_RT_INTERSECTOR=1",
        "comparison_lane": "GODOT_MTL_RT_INTERSECTOR=0",
        "comparison": comparisons,
    }
    metrics_path = artifact_dir / f"{fixture}_intersector_intersector_parity_metrics.json"
    metrics_path.write_text(json.dumps(metrics, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"METAL_RT_INTERSECTOR_INTERSECTOR_PARITY=passed fixture={fixture} captures={len(comparisons)}")
    return 0


def main() -> int:
    args = parse_args()
    try:
        return verify(args.artifact_dir.resolve(), args.fixture)
    except (OSError, ImageDiffError, VerificationError) as error:
        print(f"METAL_RT_INTERSECTOR_INTERSECTOR_PARITY=failed fixture={args.fixture} error={error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
