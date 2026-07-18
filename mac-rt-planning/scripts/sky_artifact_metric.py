#!/usr/bin/env python3
"""Count SIMD-group tearing artifacts in the sky band of an editor capture.

The e3 debug captures show a smooth sky gradient in the top quarter of the
frame; tearing shows up as clusters of pixels far darker than their row.
For each row in the analyzed band the median RGB is computed, and pixels
deviating from it by more than the threshold on any channel are counted.

Usage: sky_artifact_metric.py <capture.png> [more.png ...]
Prints: <path> outliers=<n> band_pixels=<n> ratio=<pct>
"""

import statistics
import struct
import sys
import zlib
from pathlib import Path

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"

BAND_TOP_FRACTION = 0.03  # Skip the very top (editor gizmos never reach it).
BAND_BOTTOM_FRACTION = 0.22  # Stay above the horizon in the debug camera path.
CHANNEL_THRESHOLD = 18


def read_png_rgb(path: Path) -> tuple[int, int, int, bytes]:
    """Return (width, height, channels, pixels) for an 8-bit RGB or RGBA PNG."""
    encoded = path.read_bytes()
    if not encoded.startswith(PNG_SIGNATURE):
        raise ValueError(f"not a PNG file: {path}")
    offset = len(PNG_SIGNATURE)
    ihdr = None
    compressed = bytearray()
    while offset + 12 <= len(encoded):
        length = struct.unpack_from(">I", encoded, offset)[0]
        chunk_type = encoded[offset + 4 : offset + 8]
        chunk_data = encoded[offset + 8 : offset + 8 + length]
        if chunk_type == b"IHDR":
            ihdr = chunk_data
        elif chunk_type == b"IDAT":
            compressed.extend(chunk_data)
        elif chunk_type == b"IEND":
            break
        offset += 12 + length
    if ihdr is None or not compressed:
        raise ValueError(f"PNG missing IHDR/IDAT: {path}")
    width, height, bit_depth, color_type, _, _, interlace = struct.unpack(">IIBBBBB", ihdr)
    if bit_depth != 8 or interlace != 0 or color_type not in (2, 6):
        raise ValueError(f"need non-interlaced RGB8/RGBA8 (depth {bit_depth}, color {color_type}): {path}")
    channels = 3 if color_type == 2 else 4
    filtered = zlib.decompress(bytes(compressed))
    stride = width * channels
    pixels = bytearray(height * stride)
    previous = bytearray(stride)
    src = 0
    for y in range(height):
        filter_type = filtered[src]
        src += 1
        row = bytearray(filtered[src : src + stride])
        src += stride
        if filter_type == 1:
            for x in range(channels, stride):
                row[x] = (row[x] + row[x - channels]) & 0xFF
        elif filter_type == 2:
            for x in range(stride):
                row[x] = (row[x] + previous[x]) & 0xFF
        elif filter_type == 3:
            for x in range(stride):
                left = row[x - channels] if x >= channels else 0
                row[x] = (row[x] + ((left + previous[x]) // 2)) & 0xFF
        elif filter_type == 4:
            for x in range(stride):
                left = row[x - channels] if x >= channels else 0
                above = previous[x]
                upper_left = previous[x - channels] if x >= channels else 0
                estimate = left + above - upper_left
                dl, da, dul = abs(estimate - left), abs(estimate - above), abs(estimate - upper_left)
                if dl <= da and dl <= dul:
                    predictor = left
                elif da <= dul:
                    predictor = above
                else:
                    predictor = upper_left
                row[x] = (row[x] + predictor) & 0xFF
        elif filter_type != 0:
            raise ValueError(f"unsupported PNG filter {filter_type} in {path}")
        pixels[y * stride : (y + 1) * stride] = row
        previous = row
    return width, height, channels, bytes(pixels)


def analyze(path: Path) -> tuple[int, int]:
    width, height, channels, data = read_png_rgb(path)
    top = int(height * BAND_TOP_FRACTION)
    bottom = int(height * BAND_BOTTOM_FRACTION)
    outliers = 0
    total = 0
    stride = width * channels
    for y in range(top, bottom):
        row_off = y * stride
        med = [
            statistics.median(data[row_off + c : row_off + stride : channels])
            for c in range(3)
        ]
        for x in range(width):
            off = row_off + x * channels
            total += 1
            for c in range(3):
                if abs(data[off + c] - med[c]) > CHANNEL_THRESHOLD:
                    outliers += 1
                    break
    return outliers, total


def main() -> int:
    for arg in sys.argv[1:]:
        path = Path(arg)
        outliers, total = analyze(path)
        ratio = 100.0 * outliers / total if total else 0.0
        print(f"{path} outliers={outliers} band_pixels={total} ratio={ratio:.3f}%")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
