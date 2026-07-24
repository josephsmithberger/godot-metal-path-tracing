#!/usr/bin/env python3
"""Non-instrumented gate for the Apple Metal ray-query miscompile.

Apple's Metal compiler mis-codegens the path-tracing kernel when a second
intersection_query traversal is inlined into it, forcing exactly one color
channel of scattered output pixels to 0.0 (see mac-rt-planning/apple-bug-report/).
The defect is register-allocation sensitive: MTL_SHADER_VALIDATION=1 masks it
entirely, so none of the other RT stages can see it -- they all run with shader
validation on. This stage therefore runs with validation explicitly *off* and
reads the raw RGBA16Float path-tracer output back on the CPU
(GODOT_DBG_DUMP_RT=1), counting pixels where exactly one channel is 0.0.

The engine no longer forces an inlining boundary by default, because the current
generated kernel does not trigger the miscompile on any measured configuration
and the boundary costs ~33% of the frame in mixed-alpha scenes. That is a
property of the current shader's register allocation, not a compiler fix, so
this gate exists to fail the build if a future shader edit re-triggers it. If it
does fail, GODOT_MTL_RT_NOINLINE=shadow restores the boundary immediately.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

# A live miscompile produces hundreds of thousands of bad pixels out of ~2.1M
# (the original repro measured 436,000-485,000). Legitimately black content
# never approaches this, so the threshold is set far above scene noise and far
# below the defect signal.
MAX_ZERO_CHANNEL_PIXELS = 1000

RAW_DUMP_PATTERN = re.compile(r"RT_RAW_DUMP frame=(\d+) zero_channel=(\d+) nonfinite=(\d+) total=(\d+)")

# Sweep the axes that move register allocation in the trace kernel: sample and
# bounce count, the alpha-candidate workload, and the traversal lane.
CONFIGURATIONS: tuple[dict[str, str], ...] = (
    {"GODOT_PERF_SPP": "1", "GODOT_PERF_BOUNCES": "2"},
    {"GODOT_PERF_SPP": "1", "GODOT_PERF_BOUNCES": "2", "GODOT_MTL_RT_INTERSECTOR": "0"},
    {"GODOT_PERF_SPP": "4", "GODOT_PERF_BOUNCES": "3"},
    {"GODOT_PERF_SPP": "4", "GODOT_PERF_BOUNCES": "3", "GODOT_MTL_RT_INTERSECTOR": "0"},
    {"GODOT_PERF_SPP": "1", "GODOT_PERF_BOUNCES": "2", "GODOT_PERF_MIXED_ALPHA": "1"},
    {"GODOT_PERF_SPP": "1", "GODOT_PERF_BOUNCES": "2", "GODOT_PERF_MIXED_ALPHA": "1", "GODOT_MTL_RT_INTERSECTOR": "0"},
    {"GODOT_PERF_SPP": "4", "GODOT_PERF_BOUNCES": "3", "GODOT_PERF_MIXED_ALPHA": "1"},
    {"GODOT_PERF_SPP": "4", "GODOT_PERF_BOUNCES": "3", "GODOT_PERF_MIXED_ALPHA": "1", "GODOT_MTL_RT_INTERSECTOR": "0"},
)

# Shader validation is what masks the defect, so it must not leak in from the
# runner's environment for these dispatches.
VALIDATION_VARIABLES = (
    "MTL_DEBUG_LAYER",
    "MTL_DEBUG_LAYER_ERROR_MODE",
    "MTL_SHADER_VALIDATION",
    "MTL_SHADER_VALIDATION_REPORT_TO_STDERR",
)


class VerificationError(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("binary", type=Path)
    parser.add_argument("project", type=Path, help="Path to the benchmark project (orbiting path-traced scene).")
    parser.add_argument("--measure-frames", type=int, default=150)
    return parser.parse_args()


def describe(configuration: dict[str, str]) -> str:
    return " ".join(f"{key}={value}" for key, value in sorted(configuration.items())) or "defaults"


def run_configuration(binary: Path, project: Path, configuration: dict[str, str], measure_frames: int) -> int:
    environment = {key: value for key, value in os.environ.items() if key not in VALIDATION_VARIABLES}
    environment.update(configuration)
    environment["GODOT_DBG_DUMP_RT"] = "1"
    environment["GODOT_PERF_MEASURE_FRAMES"] = str(measure_frames)

    completed = subprocess.run(
        [
            str(binary),
            "--path",
            str(project),
            "--rendering-driver",
            "metal",
            "--rendering-method",
            "forward_plus",
        ],
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        raise VerificationError(f"{describe(configuration)}: exited {completed.returncode}\n{completed.stderr[-2000:]}")

    samples = RAW_DUMP_PATTERN.findall(completed.stdout)
    if not samples:
        raise VerificationError(
            f"{describe(configuration)}: no RT_RAW_DUMP samples; the readback hook or the RT lane is inactive"
        )

    worst_zero_channel = 0
    for _frame, zero_channel, nonfinite, _total in samples:
        if int(nonfinite) > 0:
            raise VerificationError(f"{describe(configuration)}: {nonfinite} non-finite pixels in the raw output")
        worst_zero_channel = max(worst_zero_channel, int(zero_channel))

    print(f"  {describe(configuration)}: samples={len(samples)} worst_zero_channel={worst_zero_channel}")
    return worst_zero_channel


def main() -> int:
    args = parse_args()
    if not args.binary.is_file():
        print(f"METAL_RT_RAW_CORRUPTION=failed missing binary {args.binary}")
        return 1

    failures: list[str] = []
    for configuration in CONFIGURATIONS:
        try:
            worst = run_configuration(args.binary, args.project, configuration, args.measure_frames)
        except VerificationError as error:
            failures.append(str(error))
            continue
        if worst > MAX_ZERO_CHANNEL_PIXELS:
            failures.append(
                f"{describe(configuration)}: {worst} single-zero-channel pixels exceeds the "
                f"{MAX_ZERO_CHANNEL_PIXELS} threshold -- the Apple ray-query miscompile is live again. "
                "Set GODOT_MTL_RT_NOINLINE=shadow to restore the inlining boundary while the shader is fixed."
            )

    if failures:
        for failure in failures:
            print(f"  FAIL {failure}")
        print("METAL_RT_RAW_CORRUPTION=failed")
        return 1

    print("METAL_RT_RAW_CORRUPTION=passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
