#!/usr/bin/env python3

from __future__ import annotations

import argparse
import contextlib
import hashlib
import io
import json
import tempfile
import unittest
from pathlib import Path

import image_diff


class ImageDiffTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)
        self.reference = self.root / "reference.png"
        self.actual = self.root / "actual.png"
        self.manifest = self.root / "reference.json"
        self.diff = self.root / "diff.png"
        self.metrics = self.root / "metrics.json"
        self.reference_pixels = bytes((10, 20, 30, 255, 40, 50, 60, 255))
        image_diff.write_rgba8_png(self.reference, 2, 1, self.reference_pixels)
        self.manifest.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "reference_id": "image-diff-self-test",
                    "reference": {
                        "file": self.reference.name,
                        "sha256": hashlib.sha256(self.reference.read_bytes()).hexdigest(),
                        "color_space": "linear-rgba8",
                    },
                    "comparison": {
                        "metric": "max_channel_difference",
                        "max_channel_difference": 5,
                        "diff_scale": 32,
                    },
                }
            ),
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def arguments(self, *, exact_pixels: bool = False, exact_bytes: bool = False) -> argparse.Namespace:
        return argparse.Namespace(
            actual=self.actual,
            reference=self.reference,
            manifest=self.manifest,
            diff=self.diff,
            metrics=self.metrics,
            exact_pixels=exact_pixels,
            exact_bytes=exact_bytes,
        )

    def metrics_record(self) -> dict[str, object]:
        return json.loads(self.metrics.read_text(encoding="utf-8"))

    def compare(self, arguments: argparse.Namespace) -> int:
        with contextlib.redirect_stdout(io.StringIO()):
            return image_diff.compare(arguments)

    def test_threshold_mode_allows_small_pixel_difference(self) -> None:
        actual_pixels = bytes((11, 20, 30, 255, 40, 50, 60, 255))
        image_diff.write_rgba8_png(self.actual, 2, 1, actual_pixels)

        self.assertEqual(self.compare(self.arguments()), 0)
        self.assertEqual(self.metrics_record()["comparison_mode"], "threshold")

    def test_exact_pixels_rejects_small_pixel_difference(self) -> None:
        actual_pixels = bytes((11, 20, 30, 255, 40, 50, 60, 255))
        image_diff.write_rgba8_png(self.actual, 2, 1, actual_pixels)

        self.assertEqual(self.compare(self.arguments(exact_pixels=True)), 1)
        self.assertFalse(self.metrics_record()["decoded_pixels_equal"])

    def test_exact_pixels_ignores_png_encoding_difference(self) -> None:
        encoded = self.reference.read_bytes()
        text_chunk = image_diff.png_chunk(b"tEXt", b"generator\x00image-diff-self-test")
        self.actual.write_bytes(encoded[:-12] + text_chunk + encoded[-12:])

        self.assertEqual(self.compare(self.arguments(exact_pixels=True)), 0)
        self.assertFalse(self.metrics_record()["encoded_bytes_equal"])

    def test_exact_bytes_rejects_png_encoding_difference(self) -> None:
        encoded = self.reference.read_bytes()
        text_chunk = image_diff.png_chunk(b"tEXt", b"generator\x00image-diff-self-test")
        self.actual.write_bytes(encoded[:-12] + text_chunk + encoded[-12:])

        self.assertEqual(self.compare(self.arguments(exact_bytes=True)), 1)
        self.assertTrue(self.metrics_record()["decoded_pixels_equal"])
        self.assertFalse(self.metrics_record()["encoded_bytes_equal"])

    def test_exact_bytes_accepts_identical_files(self) -> None:
        self.actual.write_bytes(self.reference.read_bytes())

        self.assertEqual(self.compare(self.arguments(exact_bytes=True)), 0)
        self.assertTrue(self.metrics_record()["encoded_bytes_equal"])


if __name__ == "__main__":
    unittest.main()
