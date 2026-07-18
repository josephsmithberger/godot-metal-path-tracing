#!/usr/bin/env python3

"""Compatibility entry point for the tracked Metal RT test runner."""

from __future__ import annotations

import os
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
TRACKED_RUNNER = REPO_ROOT / "tests" / "metal_rt" / "run_mac_rt_tests.py"


if __name__ == "__main__":
    os.execv(sys.executable, [sys.executable, str(TRACKED_RUNNER), *sys.argv[1:]])
