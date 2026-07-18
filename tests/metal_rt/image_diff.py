#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
import sys
import zlib
from pathlib import Path
from typing import Any

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
PNG_COLOR_TYPE_RGBA = 6
PNG_CHANNELS = 4


class ImageDiffError(Exception):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare an RGBA8 Metal RT image with its reviewed PNG reference.",
    )
    parser.add_argument("actual", type=Path, help="Rendered RGBA8 PNG to compare.")
    parser.add_argument("reference", type=Path, help="Reviewed RGBA8 PNG reference.")
    parser.add_argument("--manifest", type=Path, required=True, help="Reference manifest and comparison policy.")
    parser.add_argument("--diff", type=Path, required=True, help="Output path for the amplified visual diff PNG.")
    parser.add_argument(
        "--metrics",
        type=Path,
        required=True,
        help="Output path for machine-readable comparison metrics.",
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--exact-pixels",
        action="store_true",
        help="Require decoded RGBA8 pixels to match exactly, ignoring PNG encoding differences.",
    )
    mode.add_argument(
        "--exact-bytes",
        action="store_true",
        help="Require the complete encoded PNG files to be byte-for-byte identical.",
    )
    return parser.parse_args()


def load_manifest(path: Path, reference: Path) -> dict[str, Any]:
    try:
        manifest: dict[str, Any] = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ImageDiffError(f"unable to read reference manifest {path}: {error}") from error

    if manifest.get("schema_version") != 1:
        raise ImageDiffError("reference manifest schema_version must be 1")
    if not manifest.get("reference_id"):
        raise ImageDiffError("reference manifest has no reference_id")

    reference_record = manifest.get("reference", {})
    if reference_record.get("file") != reference.name:
        raise ImageDiffError(
            f"manifest reference file {reference_record.get('file')!r} does not match {reference.name!r}"
        )
    expected_sha256 = reference_record.get("sha256")
    if not isinstance(expected_sha256, str) or len(expected_sha256) != 64:
        raise ImageDiffError("reference manifest has no valid sha256")
    actual_sha256 = hashlib.sha256(reference.read_bytes()).hexdigest()
    if actual_sha256 != expected_sha256:
        raise ImageDiffError(f"reference SHA-256 mismatch: expected {expected_sha256}, got {actual_sha256}")

    comparison = manifest.get("comparison", {})
    threshold = comparison.get("max_channel_difference")
    diff_scale = comparison.get("diff_scale")
    if not isinstance(threshold, int) or not 0 <= threshold <= 255:
        raise ImageDiffError("comparison.max_channel_difference must be an integer from 0 to 255")
    if not isinstance(diff_scale, int) or not 1 <= diff_scale <= 255:
        raise ImageDiffError("comparison.diff_scale must be an integer from 1 to 255")
    return manifest


def read_rgba8_png(path: Path) -> tuple[int, int, bytes]:
    try:
        encoded = path.read_bytes()
    except OSError as error:
        raise ImageDiffError(f"unable to read PNG {path}: {error}") from error
    if not encoded.startswith(PNG_SIGNATURE):
        raise ImageDiffError(f"not a PNG file: {path}")

    offset = len(PNG_SIGNATURE)
    ihdr: bytes | None = None
    compressed = bytearray()
    saw_iend = False
    while offset < len(encoded):
        if offset + 12 > len(encoded):
            raise ImageDiffError(f"truncated PNG chunk in {path}")
        length = struct.unpack_from(">I", encoded, offset)[0]
        chunk_type = encoded[offset + 4 : offset + 8]
        data_start = offset + 8
        data_end = data_start + length
        crc_end = data_end + 4
        if crc_end > len(encoded):
            raise ImageDiffError(f"truncated PNG chunk payload in {path}")
        chunk_data = encoded[data_start:data_end]
        expected_crc = struct.unpack_from(">I", encoded, data_end)[0]
        actual_crc = zlib.crc32(chunk_type)
        actual_crc = zlib.crc32(chunk_data, actual_crc) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            raise ImageDiffError(f"invalid {chunk_type.decode('ascii', 'replace')} CRC in {path}")

        if chunk_type == b"IHDR":
            if ihdr is not None or length != 13:
                raise ImageDiffError(f"invalid IHDR in {path}")
            ihdr = chunk_data
        elif chunk_type == b"IDAT":
            compressed.extend(chunk_data)
        elif chunk_type == b"IEND":
            saw_iend = True
            break
        offset = crc_end

    if ihdr is None or not compressed or not saw_iend:
        raise ImageDiffError(f"PNG is missing IHDR, IDAT, or IEND: {path}")

    width, height, bit_depth, color_type, compression, filtering, interlace = struct.unpack(">IIBBBBB", ihdr)
    if width == 0 or height == 0:
        raise ImageDiffError(f"PNG dimensions must be nonzero: {path}")
    if (bit_depth, color_type, compression, filtering, interlace) != (8, PNG_COLOR_TYPE_RGBA, 0, 0, 0):
        raise ImageDiffError(
            f"PNG must be non-interlaced RGBA8 (got bit depth {bit_depth}, color type {color_type}): {path}"
        )

    try:
        filtered = zlib.decompress(bytes(compressed))
    except zlib.error as error:
        raise ImageDiffError(f"invalid compressed PNG data in {path}: {error}") from error
    stride = width * PNG_CHANNELS
    expected_size = height * (stride + 1)
    if len(filtered) != expected_size:
        raise ImageDiffError(f"unexpected decoded PNG size in {path}: expected {expected_size}, got {len(filtered)}")

    pixels = bytearray(height * stride)
    previous = bytearray(stride)
    source_offset = 0
    for y in range(height):
        filter_type = filtered[source_offset]
        source_offset += 1
        row = bytearray(filtered[source_offset : source_offset + stride])
        source_offset += stride
        for x in range(stride):
            left = row[x - PNG_CHANNELS] if x >= PNG_CHANNELS else 0
            above = previous[x]
            upper_left = previous[x - PNG_CHANNELS] if x >= PNG_CHANNELS else 0
            if filter_type == 1:
                row[x] = (row[x] + left) & 0xFF
            elif filter_type == 2:
                row[x] = (row[x] + above) & 0xFF
            elif filter_type == 3:
                row[x] = (row[x] + ((left + above) // 2)) & 0xFF
            elif filter_type == 4:
                estimate = left + above - upper_left
                distance_left = abs(estimate - left)
                distance_above = abs(estimate - above)
                distance_upper_left = abs(estimate - upper_left)
                if distance_left <= distance_above and distance_left <= distance_upper_left:
                    predictor = left
                elif distance_above <= distance_upper_left:
                    predictor = above
                else:
                    predictor = upper_left
                row[x] = (row[x] + predictor) & 0xFF
            elif filter_type != 0:
                raise ImageDiffError(f"unsupported PNG filter {filter_type} in {path}")
        pixels[y * stride : (y + 1) * stride] = row
        previous = row
    return width, height, bytes(pixels)


def png_chunk(chunk_type: bytes, data: bytes) -> bytes:
    checksum = zlib.crc32(chunk_type)
    checksum = zlib.crc32(data, checksum) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + chunk_type + data + struct.pack(">I", checksum)


def write_rgba8_png(path: Path, width: int, height: int, pixels: bytes) -> None:
    stride = width * PNG_CHANNELS
    if len(pixels) != height * stride:
        raise ImageDiffError("diff pixel buffer has the wrong size")
    scanlines = bytearray()
    for y in range(height):
        scanlines.append(0)
        scanlines.extend(pixels[y * stride : (y + 1) * stride])
    ihdr = struct.pack(">IIBBBBB", width, height, 8, PNG_COLOR_TYPE_RGBA, 0, 0, 0)
    encoded = PNG_SIGNATURE + png_chunk(b"IHDR", ihdr)
    encoded += png_chunk(b"IDAT", zlib.compress(bytes(scanlines), level=9))
    encoded += png_chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(encoded)


def write_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def comparison_mode(args: argparse.Namespace) -> str:
    if getattr(args, "exact_bytes", False):
        return "exact_bytes"
    if getattr(args, "exact_pixels", False):
        return "exact_pixels"
    return "threshold"


def compare(args: argparse.Namespace) -> int:
    manifest = load_manifest(args.manifest, args.reference)
    actual_encoded = args.actual.read_bytes()
    reference_encoded = args.reference.read_bytes()
    encoded_bytes_equal = actual_encoded == reference_encoded
    actual_width, actual_height, actual = read_rgba8_png(args.actual)
    reference_width, reference_height, reference = read_rgba8_png(args.reference)
    if (actual_width, actual_height) != (reference_width, reference_height):
        raise ImageDiffError(
            "image dimensions differ: "
            f"actual={actual_width}x{actual_height}, reference={reference_width}x{reference_height}"
        )

    comparison = manifest["comparison"]
    mode = comparison_mode(args)
    threshold = 0 if mode != "threshold" else comparison["max_channel_difference"]
    diff_scale = comparison["diff_scale"]
    channel_diffs = [abs(actual_value - reference_value) for actual_value, reference_value in zip(actual, reference)]
    max_diff = max(channel_diffs, default=0)
    mean_diff = sum(channel_diffs) / len(channel_diffs) if channel_diffs else 0.0
    rms_diff = math.sqrt(sum(value * value for value in channel_diffs) / len(channel_diffs)) if channel_diffs else 0.0
    differing_channels = sum(value != 0 for value in channel_diffs)
    differing_pixels = sum(
        any(channel_diffs[offset + channel] != 0 for channel in range(PNG_CHANNELS))
        for offset in range(0, len(channel_diffs), PNG_CHANNELS)
    )

    visual_diff = bytearray(len(actual))
    for offset in range(0, len(channel_diffs), PNG_CHANNELS):
        visual_diff[offset] = min(255, channel_diffs[offset] * diff_scale)
        visual_diff[offset + 1] = min(255, channel_diffs[offset + 1] * diff_scale)
        visual_diff[offset + 2] = min(255, channel_diffs[offset + 2] * diff_scale)
        visual_diff[offset + 3] = 255
    write_rgba8_png(args.diff, actual_width, actual_height, bytes(visual_diff))

    pixels_equal = max_diff == 0
    passed = max_diff <= threshold
    if mode == "exact_bytes":
        passed = pixels_equal and encoded_bytes_equal
    metrics = {
        "schema_version": 1,
        "status": "passed" if passed else "failed",
        "reference_id": manifest["reference_id"],
        "actual_file": args.actual.name,
        "actual_sha256": hashlib.sha256(actual_encoded).hexdigest(),
        "reference_file": args.reference.name,
        "reference_sha256": hashlib.sha256(reference_encoded).hexdigest(),
        "diff_file": args.diff.name,
        "width": actual_width,
        "height": actual_height,
        "color_space": manifest["reference"]["color_space"],
        "comparison_mode": mode,
        "decoded_pixels_equal": pixels_equal,
        "encoded_bytes_equal": encoded_bytes_equal,
        "max_channel_difference": max_diff,
        "max_channel_difference_normalized": max_diff / 255.0,
        "mean_absolute_channel_difference": mean_diff,
        "rms_channel_difference": rms_diff,
        "differing_channels": differing_channels,
        "total_channels": len(channel_diffs),
        "differing_pixels": differing_pixels,
        "total_pixels": actual_width * actual_height,
        "threshold": {
            "metric": "encoded_file_identity" if mode == "exact_bytes" else comparison["metric"],
            "max_channel_difference": threshold,
        },
    }
    write_json(args.metrics, metrics)
    print(
        "MetalRT IMAGE_COMPARISON image diff: "
        f"reference={manifest['reference_id']} status={metrics['status']} "
        f"mode={mode} max_diff={max_diff}/{threshold} mean_diff={mean_diff:.6f} "
        f"encoded_bytes_equal={str(encoded_bytes_equal).lower()} "
        f"differing_pixels={differing_pixels}/{actual_width * actual_height}"
    )
    return 0 if passed else 1


def main() -> int:
    args = parse_args()
    try:
        return compare(args)
    except (ImageDiffError, OSError) as error:
        write_json(
            args.metrics,
            {
                "schema_version": 1,
                "status": "error",
                "error": str(error),
                "comparison_mode": comparison_mode(args),
                "actual_file": args.actual.name,
                "reference_file": args.reference.name,
                "diff_file": args.diff.name,
            },
        )
        print(f"MetalRT IMAGE_COMPARISON image diff error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
