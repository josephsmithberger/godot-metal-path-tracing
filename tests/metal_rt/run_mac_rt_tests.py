#!/usr/bin/env python3

from __future__ import annotations

import argparse
import datetime
import json
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

SCRIPT_PATH = Path(__file__).resolve()
REPO_ROOT = SCRIPT_PATH.parents[2]
DEFAULT_ARTIFACT_ROOT = REPO_ROOT / "mac-rt-planning" / "artifacts"
VALID_STAGES = (
    "preflight",
    "caps",
    "build",
    "smoke",
    "unit",
    "gpu",
    "image",
    "editor-scene",
    "geometry-scene",
    "material-scene",
    "intersector-parity",
    "procedural-scene",
    "raw-corruption",
    "fallback",
)
CAPS_PROBE_SOURCE = REPO_ROOT / "docs" / "rt_metal_port" / "capability_probe.mm"
RUNTIME_GATE_PROJECT = REPO_ROOT / "tests" / "metal_rt"
EDITOR_SCENE_PROJECT = RUNTIME_GATE_PROJECT / "editor"
EDITOR_SCENE_FIXTURE = "res://fixtures/e0_hg0.tscn"
EDITOR_SCENE_VERIFY_SCRIPT = RUNTIME_GATE_PROJECT / "verify_editor_scene.py"
PRESENTATION_VERIFY_SCRIPT = RUNTIME_GATE_PROJECT / "verify_presentation.py"
GEOMETRY_SCENE_FIXTURE = "res://fixtures/e1_geometry.tscn"
GEOMETRY_SCENE_VERIFY_SCRIPT = RUNTIME_GATE_PROJECT / "verify_geometry_scene.py"
MATERIAL_SCENE_FIXTURE = "res://fixtures/e2_materials.tscn"
MATERIAL_SCENE_VERIFY_SCRIPT = RUNTIME_GATE_PROJECT / "verify_material_scene.py"
INTERSECTOR_PARITY_VERIFY_SCRIPT = RUNTIME_GATE_PROJECT / "verify_intersector_parity.py"
PROCEDURAL_SCENE_FIXTURE = "res://fixtures/e3_procedural.tscn"
PROCEDURAL_SCENE_VERIFY_SCRIPT = RUNTIME_GATE_PROJECT / "verify_procedural_scene.py"
RAW_CORRUPTION_VERIFY_SCRIPT = RUNTIME_GATE_PROJECT / "verify_raw_corruption.py"
BENCHMARK_PROJECT = REPO_ROOT / "benchmark"
IMAGE_DIFF_SCRIPT = RUNTIME_GATE_PROJECT / "image_diff.py"
IMAGE_DIFF_TEST_SCRIPT = RUNTIME_GATE_PROJECT / "test_image_diff.py"
IMAGE_REFERENCE = RUNTIME_GATE_PROJECT / "references" / "pathtracer_launch_v1.png"
IMAGE_REFERENCE_MANIFEST = IMAGE_REFERENCE.with_suffix(".json")
CAPS_SKIP_EXIT_CODE = 3  # Probe exit code for a machine-readable skip (see capability_probe.mm).

# Metal shader validation instruments the shader itself, so it reports memory
# the GPU touches but does not own. The RT compute lane reaches geometry and
# material data through raw device addresses, and the API debug layer cannot
# see those accesses: a non-resident or out-of-bounds read there returns
# garbage instead of failing, which still renders an image and still prints
# every acceptance marker. Every stage that dispatches real RT work therefore
# runs with shader validation on.
METAL_VALIDATION_ENVIRONMENT = {
    "MTL_DEBUG_LAYER": "1",
    "MTL_DEBUG_LAYER_ERROR_MODE": "nslog",
    "MTL_SHADER_VALIDATION": "1",
    "MTL_SHADER_VALIDATION_REPORT_TO_STDERR": "1",
}

# Shader validation prints these when a shader dereferences memory it does not
# own. They are reported per offending dispatch rather than raising the exit
# code, so the stage only fails if the log is inspected.
METAL_VALIDATION_FORBIDDEN_PATTERNS = (
    "Invalid device load",
    "Invalid device store",
    "Attempted to free invalid ID",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build and test the macOS Metal ray tracing development baseline.",
    )
    parser.add_argument(
        "--stage",
        action="append",
        choices=(*VALID_STAGES, "all"),
        help="Stage to run; repeat for multiple stages (default: all).",
    )
    parser.add_argument("--arch", choices=("arm64", "x86_64"), default="arm64")
    parser.add_argument(
        "--binary",
        type=Path,
        help="Editor binary to test (default: bin/godot.macos.editor.<arch>).",
    )
    parser.add_argument(
        "--artifact-dir",
        type=Path,
        help="Directory for this run (default: a timestamped directory under mac-rt-planning/artifacts).",
    )
    parser.add_argument("--jobs", type=positive_int, help="Parallel SCons job count.")
    parser.add_argument(
        "--scons-flag",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help="Append a SCons option after the standardized defaults; repeat as needed.",
    )
    parser.add_argument("--dry-run", action="store_true", help="Record and print commands without executing them.")
    parser.add_argument("--keep-going", action="store_true", help="Run later stages after a command fails.")
    parser.add_argument(
        "--fail-on-skip",
        action="store_true",
        help="Treat a capability or test skip as failure (intended for configured GPU runners).",
    )
    return parser.parse_args()


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed < 1:
        raise argparse.ArgumentTypeError("must be at least 1")
    return parsed


def resolve_stages(requested: list[str] | None) -> list[str]:
    if not requested or "all" in requested:
        return list(VALID_STAGES)
    return list(dict.fromkeys(requested))


def repo_path(value: Path) -> Path:
    return value if value.is_absolute() else REPO_ROOT / value


def git_output(*args: str) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=REPO_ROOT,
        check=False,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip() if result.returncode == 0 else "unavailable"


def shell_join(command: list[str]) -> str:
    return shlex.join(command)


def command_record(
    name: str,
    command: list[str],
    skip_exit_codes: tuple[int, ...] = (),
    requires_passed: str | None = None,
    preset_skip_reason: str | None = None,
    detect_log_skip: bool = False,
    environment: dict[str, str] | None = None,
    required_log_patterns: tuple[str, ...] = (),
    forbidden_log_patterns: tuple[str, ...] = (),
    required_log_counts: dict[str, int] | None = None,
) -> dict[str, Any]:
    environment = environment or {}
    displayed_command = ["env", *(f"{key}={value}" for key, value in sorted(environment.items())), *command]
    return {
        "name": name,
        "command": command,
        "command_string": shell_join(displayed_command if environment else command),
        "environment": environment,
        "exit_code": None,
        "duration_seconds": 0.0,
        "log": None,
        "status": "pending",
        "skip_exit_codes": list(skip_exit_codes),
        "requires_passed": requires_passed,
        "preset_skip_reason": preset_skip_reason,
        "detect_log_skip": detect_log_skip,
        "required_log_patterns": list(required_log_patterns),
        "forbidden_log_patterns": list(forbidden_log_patterns),
        "required_log_counts": required_log_counts or {},
    }


def make_commands(
    args: argparse.Namespace,
    stages: list[str],
    binary: Path,
    artifact_dir: Path,
) -> list[dict[str, Any]]:
    commands: list[dict[str, Any]] = []

    if "preflight" in stages:
        commands.extend([
            command_record("preflight-macos", ["uname", "-s"]),
            command_record("preflight-architecture", ["uname", "-m"]),
            command_record("preflight-sdk", ["xcrun", "--show-sdk-version"]),
            command_record("preflight-metal-compiler", ["xcrun", "--find", "metal"]),
            command_record("preflight-scons", ["scons", "--version"]),
            command_record("preflight-image-diff-tests", [sys.executable, str(IMAGE_DIFF_TEST_SCRIPT)]),
        ])

    if "caps" in stages or (
        any(
            stage in stages
            for stage in (
                "gpu",
                "image",
                "editor-scene",
                "geometry-scene",
                "material-scene",
                "intersector-parity",
                "procedural-scene",
                "fallback",
            )
        )
        and args.arch == "arm64"
    ):
        probe_binary = artifact_dir / "capability_probe"
        commands.extend([
            command_record(
                "caps-compile",
                [
                    "xcrun",
                    "clang++",
                    "-std=c++17",
                    "-fobjc-arc",
                    "-mmacosx-version-min=11.0",
                    "-framework",
                    "Foundation",
                    "-framework",
                    "Metal",
                    str(CAPS_PROBE_SOURCE),
                    "-o",
                    str(probe_binary),
                ],
            ),
            command_record(
                "caps-probe",
                [str(probe_binary), "--output", str(artifact_dir / "capability_record.json")],
                skip_exit_codes=(CAPS_SKIP_EXIT_CODE,),
            ),
        ])

    if "build" in stages:
        metal_enabled = "yes" if args.arch == "arm64" else "no"
        build = [
            "scons",
            "platform=macos",
            "target=editor",
            f"arch={args.arch}",
            "dev_mode=yes",
            f"metal={metal_enabled}",
            "vulkan=no",
            "angle=no",
            "accesskit=no",
            *args.scons_flag,
        ]
        if args.jobs:
            build.extend(["-j", str(args.jobs)])
        commands.append(command_record("build-editor", build))

    if "smoke" in stages:
        commands.extend([
            command_record("smoke-version", [str(binary), "--version"]),
            command_record("smoke-help", [str(binary), "--help"]),
        ])

    if "unit" in stages:
        commands.append(command_record("unit-tests", [str(binary), "--test", "--force-colors"]))

    if "gpu" in stages:
        commands.append(
            command_record(
                "gpu-acceleration-structures",
                [
                    str(binary),
                    "--test",
                    "--test-case=*[MetalRT][GPU]*",
                    "--no-skip",
                    "--force-colors",
                ],
                requires_passed="caps-probe" if args.arch == "arm64" else None,
                preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                detect_log_skip=True,
                environment=METAL_VALIDATION_ENVIRONMENT,
                forbidden_log_patterns=METAL_VALIDATION_FORBIDDEN_PATTERNS,
            )
        )

    if "image" in stages:
        image_render_name = "gpu-acceleration-structures" if "gpu" in stages else "image-render"
        if "gpu" not in stages:
            commands.append(
                command_record(
                    image_render_name,
                    [
                        str(binary),
                        "--test",
                        "--test-case=*[MetalRT][GPU] PATH_TRACER*",
                        "--no-skip",
                        "--force-colors",
                    ],
                    requires_passed="caps-probe" if args.arch == "arm64" else None,
                    preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                    detect_log_skip=True,
                    environment=METAL_VALIDATION_ENVIRONMENT,
                    forbidden_log_patterns=METAL_VALIDATION_FORBIDDEN_PATTERNS,
                )
            )
        commands.append(
            command_record(
                "image-compare",
                [
                    sys.executable,
                    str(IMAGE_DIFF_SCRIPT),
                    str(artifact_dir / "c10_pathtracer_launch_gpu.png"),
                    str(IMAGE_REFERENCE),
                    "--manifest",
                    str(IMAGE_REFERENCE_MANIFEST),
                    "--diff",
                    str(artifact_dir / "c10_pathtracer_launch_diff.png"),
                    "--metrics",
                    str(artifact_dir / "c10_pathtracer_launch_metrics.json"),
                ],
                requires_passed=image_render_name,
                preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                required_log_patterns=("MetalRT IMAGE_COMPARISON image diff:", "status=passed"),
            )
        )

    if "editor-scene" in stages:
        editor_command = [
            str(binary),
            "--editor",
            "--path",
            str(EDITOR_SCENE_PROJECT),
            EDITOR_SCENE_FIXTURE,
            "--quit-after",
            "600",
        ]
        editor_environment = {
            **METAL_VALIDATION_ENVIRONMENT,
            "GODOT_MRT_EDITOR_CAPTURE": "1",
            "GODOT_MRT_FIXTURE": "e0_hg0",
        }
        presentation_editor_command = [
            str(binary),
            "--editor",
            "--verbose",
            "--path",
            str(EDITOR_SCENE_PROJECT),
            EDITOR_SCENE_FIXTURE,
            "--quit-after",
            "1200",
        ]
        scene_markers = (
            "METAL_RT_EDITOR_ROUTE=compute_ray_query",
            "METAL_RT_DENOISER=none",
            "METAL_RT_EDITOR_HG0=passed",
            "METAL_RT_FIXTURE_REVISION=e0-hg0-v1",
        )
        commands.extend([
            command_record(
                "editor-scene-cold",
                editor_command,
                requires_passed="caps-probe" if args.arch == "arm64" else None,
                preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                environment={**editor_environment, "GODOT_MRT_CAPTURE_LABEL": "cold"},
                required_log_patterns=scene_markers,
                forbidden_log_patterns=METAL_VALIDATION_FORBIDDEN_PATTERNS,
            ),
            command_record(
                "editor-scene-reload",
                editor_command,
                requires_passed="editor-scene-cold" if args.arch == "arm64" else None,
                preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                environment={**editor_environment, "GODOT_MRT_CAPTURE_LABEL": "reload"},
                required_log_patterns=scene_markers,
                forbidden_log_patterns=METAL_VALIDATION_FORBIDDEN_PATTERNS,
            ),
            command_record(
                "editor-scene-verify",
                [sys.executable, str(EDITOR_SCENE_VERIFY_SCRIPT), str(artifact_dir)],
                requires_passed="editor-scene-reload" if args.arch == "arm64" else None,
                preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                required_log_patterns=("METAL_RT_EDITOR_SCENE_FIXTURE_VERIFY=passed",),
            ),
            command_record(
                "editor-scene-forced-fallback",
                editor_command,
                requires_passed="editor-scene-verify" if args.arch == "arm64" else None,
                preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                environment={
                    **editor_environment,
                    "GODOT_MRT_CAPTURE_LABEL": "fallback",
                    "GODOT_MTL_DISABLE_RAYTRACING": "1",
                },
                required_log_patterns=(
                    "using non-RT rendering fallback",
                    "capability_gate=disabled:forced_disabled",
                    "METAL_RT_FIXTURE_REVISION=e0-hg0-v1",
                ),
                forbidden_log_patterns=("METAL_RT_EDITOR_HG0=passed",),
            ),
            command_record(
                "editor-scene-presentation-presentation",
                presentation_editor_command,
                requires_passed="editor-scene-forced-fallback" if args.arch == "arm64" else None,
                preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                environment={
                    **editor_environment,
                    "GODOT_MRT_CAPTURE_LABEL": "presentation",
                    "GODOT_MRT_PRESENTATION": "1",
                },
                required_log_patterns=(
                    "METAL_RT_EDITOR_ROUTE=compute_ray_query",
                    "METAL_RT_DENOISER_DEFAULT=none",
                    "METAL_RT_SER=disabled",
                    "METAL_RT_PRESENTATION=native,fsr1,fsr2",
                    "METAL_RT_TEMPORAL_SEQUENCE=passed",
                    "METAL_RT_PRESENTATION_MAC_UX=passed",
                    "MetalRT PRESENTATION temporal presentation history reset: context",
                    "camera_cut",
                    "The saved path-tracing denoiser is unavailable",
                ),
                forbidden_log_patterns=METAL_VALIDATION_FORBIDDEN_PATTERNS,
                required_log_counts={"The saved path-tracing denoiser is unavailable": 1},
            ),
            command_record(
                "editor-scene-presentation-verify",
                [sys.executable, str(PRESENTATION_VERIFY_SCRIPT), str(artifact_dir)],
                requires_passed="editor-scene-presentation-presentation" if args.arch == "arm64" else None,
                preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                required_log_patterns=("METAL_RT_PRESENTATION_VERIFY=passed",),
            ),
        ])

    if "geometry-scene" in stages:
        geometry_command = [
            str(binary),
            "--editor",
            "--path",
            str(EDITOR_SCENE_PROJECT),
            GEOMETRY_SCENE_FIXTURE,
            "--quit-after",
            "900",
        ]
        geometry_environment = {
            **METAL_VALIDATION_ENVIRONMENT,
            "GODOT_MRT_EDITOR_CAPTURE": "1",
            "GODOT_MRT_FIXTURE": "e1_geometry",
        }
        geometry_markers = (
            "METAL_RT_EDITOR_ROUTE=compute_ray_query",
            "METAL_RT_EDITOR_HG0=passed",
            "METAL_RT_SCENE_GEOMETRY=passed",
            "METAL_RT_MUTATION_SEQUENCE=passed",
            "METAL_RT_FIXTURE_REVISION=e1-geometry-v1",
        )
        commands.extend([
            command_record(
                "geometry-scene-cold",
                geometry_command,
                requires_passed="caps-probe" if args.arch == "arm64" else None,
                preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                environment={**geometry_environment, "GODOT_MRT_CAPTURE_LABEL": "cold"},
                required_log_patterns=geometry_markers,
                forbidden_log_patterns=METAL_VALIDATION_FORBIDDEN_PATTERNS,
            ),
            command_record(
                "geometry-scene-reload",
                geometry_command,
                requires_passed="geometry-scene-cold" if args.arch == "arm64" else None,
                preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                environment={**geometry_environment, "GODOT_MRT_CAPTURE_LABEL": "reload"},
                required_log_patterns=geometry_markers,
                forbidden_log_patterns=METAL_VALIDATION_FORBIDDEN_PATTERNS,
            ),
            command_record(
                "geometry-scene-verify",
                [sys.executable, str(GEOMETRY_SCENE_VERIFY_SCRIPT), str(artifact_dir)],
                requires_passed="geometry-scene-reload" if args.arch == "arm64" else None,
                preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                required_log_patterns=("METAL_RT_SCENE_GEOMETRY_FIXTURE_VERIFY=passed",),
            ),
            command_record(
                "geometry-scene-forced-fallback",
                geometry_command,
                requires_passed="geometry-scene-verify" if args.arch == "arm64" else None,
                preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                environment={
                    **geometry_environment,
                    "GODOT_MRT_CAPTURE_LABEL": "fallback",
                    "GODOT_MTL_DISABLE_RAYTRACING": "1",
                },
                required_log_patterns=(
                    "using non-RT rendering fallback",
                    "capability_gate=disabled:forced_disabled",
                    "METAL_RT_FIXTURE_REVISION=e1-geometry-v1",
                ),
                forbidden_log_patterns=("METAL_RT_EDITOR_HG0=passed", "METAL_RT_SCENE_GEOMETRY=passed"),
            ),
        ])

    if "material-scene" in stages:
        material_command = [
            str(binary),
            "--editor",
            "--path",
            str(EDITOR_SCENE_PROJECT),
            MATERIAL_SCENE_FIXTURE,
            "--quit-after",
            "900",
        ]
        material_environment = {
            **METAL_VALIDATION_ENVIRONMENT,
            "GODOT_MRT_EDITOR_CAPTURE": "1",
            "GODOT_MRT_FIXTURE": "e2_materials",
        }
        material_markers = (
            "METAL_RT_EDITOR_ROUTE=compute_ray_query",
            "METAL_RT_MATERIAL_DISPATCH=passed",
            "METAL_RT_ALPHA_TEST=passed",
            "METAL_RT_CUSTOM_SHADER_RELOAD=passed",
            "METAL_RT_FIXTURE_REVISION=e2-materials-v1",
        )
        commands.extend([
            command_record(
                "material-scene-cold",
                material_command,
                requires_passed="caps-probe" if args.arch == "arm64" else None,
                preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                environment={**material_environment, "GODOT_MRT_CAPTURE_LABEL": "cold"},
                required_log_patterns=material_markers,
                forbidden_log_patterns=METAL_VALIDATION_FORBIDDEN_PATTERNS,
            ),
            command_record(
                "material-scene-reload",
                material_command,
                requires_passed="material-scene-cold" if args.arch == "arm64" else None,
                preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                environment={**material_environment, "GODOT_MRT_CAPTURE_LABEL": "reload"},
                required_log_patterns=material_markers,
                forbidden_log_patterns=METAL_VALIDATION_FORBIDDEN_PATTERNS,
            ),
            command_record(
                "material-scene-verify",
                [sys.executable, str(MATERIAL_SCENE_VERIFY_SCRIPT), str(artifact_dir)],
                requires_passed="material-scene-reload" if args.arch == "arm64" else None,
                preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                required_log_patterns=("METAL_RT_MATERIAL_FIXTURE_VERIFY=passed",),
            ),
            command_record(
                "material-scene-forced-fallback",
                material_command,
                requires_passed="material-scene-verify" if args.arch == "arm64" else None,
                preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                environment={
                    **material_environment,
                    "GODOT_MRT_CAPTURE_LABEL": "fallback",
                    "GODOT_MTL_DISABLE_RAYTRACING": "1",
                },
                required_log_patterns=(
                    "using non-RT rendering fallback",
                    "capability_gate=disabled:forced_disabled",
                    "METAL_RT_FIXTURE_REVISION=e2-materials-v1",
                ),
                forbidden_log_patterns=("METAL_RT_EDITOR_HG0=passed", "METAL_RT_MATERIAL_DISPATCH=passed"),
            ),
        ])

    if "intersector-parity" in stages:
        parity_environment = {
            **METAL_VALIDATION_ENVIRONMENT,
            "GODOT_MRT_EDITOR_CAPTURE": "1",
            # The parity contract isolates the traversal-lane variable
            # (intersector versus forced query). BLAS compaction swaps in a
            # differently laid out BVH at a GPU-timing-dependent frame, which
            # legitimately flips exact-tie hits on shared edges, so it is
            # pinned off here; compaction rendering coverage stays with the
            # editor/geometry/material scene stages.
            "GODOT_RT_BLAS_COMPACTION": "0",
        }
        parity_geometry_command = [
            str(binary),
            "--editor",
            "--verbose",
            "--path",
            str(EDITOR_SCENE_PROJECT),
            GEOMETRY_SCENE_FIXTURE,
            "--quit-after",
            "900",
        ]
        parity_material_command = [
            str(binary),
            "--editor",
            "--verbose",
            "--path",
            str(EDITOR_SCENE_PROJECT),
            MATERIAL_SCENE_FIXTURE,
            "--quit-after",
            "900",
        ]
        geometry_markers = (
            "METAL_RT_EDITOR_ROUTE=compute_ray_query",
            "METAL_RT_SCENE_GEOMETRY=passed",
            "METAL_RT_MUTATION_SEQUENCE=passed",
            "METAL_RT_FIXTURE_REVISION=e1-geometry-v1",
        )
        material_markers = (
            "METAL_RT_EDITOR_ROUTE=compute_ray_query",
            "METAL_RT_MATERIAL_DISPATCH=passed",
            "METAL_RT_ALPHA_TEST=passed",
            "METAL_RT_CUSTOM_SHADER_RELOAD=passed",
            "METAL_RT_FIXTURE_REVISION=e2-materials-v1",
        )
        commands.extend([
            command_record(
                "intersector-parity-geometry-default",
                parity_geometry_command,
                requires_passed="caps-probe" if args.arch == "arm64" else None,
                preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                environment={
                    **parity_environment,
                    "GODOT_MRT_FIXTURE": "e1_geometry",
                    "GODOT_MRT_CAPTURE_LABEL": "cold",
                    "GODOT_MTL_RT_INTERSECTOR": "1",
                },
                required_log_patterns=geometry_markers,
                forbidden_log_patterns=METAL_VALIDATION_FORBIDDEN_PATTERNS,
            ),
            command_record(
                "intersector-parity-geometry-query",
                parity_geometry_command,
                requires_passed="intersector-parity-geometry-default" if args.arch == "arm64" else None,
                preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                environment={
                    **parity_environment,
                    "GODOT_MRT_FIXTURE": "e1_geometry",
                    "GODOT_MRT_CAPTURE_LABEL": "query",
                    "GODOT_MTL_RT_INTERSECTOR": "0",
                },
                required_log_patterns=(
                    *geometry_markers,
                    "intersector fast path disabled by GODOT_MTL_RT_INTERSECTOR=0",
                ),
                forbidden_log_patterns=METAL_VALIDATION_FORBIDDEN_PATTERNS,
            ),
            command_record(
                "intersector-parity-geometry-verify",
                [sys.executable, str(INTERSECTOR_PARITY_VERIFY_SCRIPT), str(artifact_dir), "e1_geometry"],
                requires_passed="intersector-parity-geometry-query" if args.arch == "arm64" else None,
                preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                required_log_patterns=("METAL_RT_INTERSECTOR_INTERSECTOR_PARITY=passed fixture=e1_geometry",),
            ),
            command_record(
                "intersector-parity-material-default",
                parity_material_command,
                requires_passed="intersector-parity-geometry-verify" if args.arch == "arm64" else None,
                preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                environment={
                    **parity_environment,
                    "GODOT_MRT_FIXTURE": "e2_materials",
                    "GODOT_MRT_CAPTURE_LABEL": "cold",
                    "GODOT_MTL_RT_INTERSECTOR": "1",
                },
                required_log_patterns=material_markers,
                forbidden_log_patterns=METAL_VALIDATION_FORBIDDEN_PATTERNS,
            ),
            command_record(
                "intersector-parity-material-query",
                parity_material_command,
                requires_passed="intersector-parity-material-default" if args.arch == "arm64" else None,
                preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                environment={
                    **parity_environment,
                    "GODOT_MRT_FIXTURE": "e2_materials",
                    "GODOT_MRT_CAPTURE_LABEL": "query",
                    "GODOT_MTL_RT_INTERSECTOR": "0",
                },
                required_log_patterns=(
                    *material_markers,
                    "intersector fast path disabled by GODOT_MTL_RT_INTERSECTOR=0",
                ),
                forbidden_log_patterns=METAL_VALIDATION_FORBIDDEN_PATTERNS,
            ),
            command_record(
                "intersector-parity-material-verify",
                [sys.executable, str(INTERSECTOR_PARITY_VERIFY_SCRIPT), str(artifact_dir), "e2_materials"],
                requires_passed="intersector-parity-material-query" if args.arch == "arm64" else None,
                preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                required_log_patterns=("METAL_RT_INTERSECTOR_INTERSECTOR_PARITY=passed fixture=e2_materials",),
            ),
        ])

    if "procedural-scene" in stages:
        procedural_command = [
            str(binary),
            "--editor",
            "--path",
            str(EDITOR_SCENE_PROJECT),
            PROCEDURAL_SCENE_FIXTURE,
            "--quit-after",
            "1200",
        ]
        procedural_environment = {
            **METAL_VALIDATION_ENVIRONMENT,
            "GODOT_MRT_EDITOR_CAPTURE": "1",
            "GODOT_MRT_FIXTURE": "e3_procedural",
        }
        procedural_markers = (
            "METAL_RT_EDITOR_ROUTE=compute_ray_query",
            "METAL_RT_PROCEDURAL=passed",
            "METAL_RT_CUSTOM_INTERSECTION=passed",
            "METAL_RT_MIXED_GEOMETRY=passed",
            "METAL_RT_PROCEDURAL_UPDATE=builds:3,refits:1,removed:1",
            "METAL_RT_FIXTURE_REVISION=e3-procedural-v1",
            "Path tracing omitted invalid procedural AABB geometry:",
        )
        commands.extend([
            command_record(
                "procedural-scene-cold",
                procedural_command,
                requires_passed="caps-probe" if args.arch == "arm64" else None,
                preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                environment={**procedural_environment, "GODOT_MRT_CAPTURE_LABEL": "cold"},
                required_log_patterns=procedural_markers,
                forbidden_log_patterns=METAL_VALIDATION_FORBIDDEN_PATTERNS,
                required_log_counts={"Path tracing omitted invalid procedural AABB geometry:": 1},
            ),
            command_record(
                "procedural-scene-reload",
                procedural_command,
                requires_passed="procedural-scene-cold" if args.arch == "arm64" else None,
                preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                environment={**procedural_environment, "GODOT_MRT_CAPTURE_LABEL": "reload"},
                required_log_patterns=procedural_markers,
                forbidden_log_patterns=METAL_VALIDATION_FORBIDDEN_PATTERNS,
                required_log_counts={"Path tracing omitted invalid procedural AABB geometry:": 1},
            ),
            command_record(
                "procedural-scene-verify",
                [sys.executable, str(PROCEDURAL_SCENE_VERIFY_SCRIPT), str(artifact_dir)],
                requires_passed="procedural-scene-reload" if args.arch == "arm64" else None,
                preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                required_log_patterns=("METAL_RT_PROCEDURAL_FIXTURE_VERIFY=passed",),
            ),
            command_record(
                "procedural-scene-forced-fallback",
                procedural_command,
                requires_passed="procedural-scene-verify" if args.arch == "arm64" else None,
                preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                environment={
                    **procedural_environment,
                    "GODOT_MRT_CAPTURE_LABEL": "fallback",
                    "GODOT_MTL_DISABLE_RAYTRACING": "1",
                },
                required_log_patterns=(
                    "using non-RT rendering fallback",
                    "capability_gate=disabled:forced_disabled",
                    "METAL_RT_FIXTURE_REVISION=e3-procedural-v1",
                ),
                forbidden_log_patterns=(
                    "METAL_RT_PROCEDURAL=passed",
                    "METAL_RT_CUSTOM_INTERSECTION=passed",
                    "METAL_RT_MIXED_GEOMETRY=passed",
                ),
            ),
        ])

    if "raw-corruption" in stages:
        # Deliberately not wrapped in METAL_VALIDATION_ENVIRONMENT: shader
        # validation perturbs register allocation enough to hide the Apple
        # ray-query miscompile this stage exists to detect, which is why none of
        # the validated stages above can serve as evidence against it.
        commands.append(
            command_record(
                "raw-corruption",
                [
                    sys.executable,
                    str(RAW_CORRUPTION_VERIFY_SCRIPT),
                    str(binary),
                    str(BENCHMARK_PROJECT),
                ],
                preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                required_log_patterns=("METAL_RT_RAW_CORRUPTION=passed",),
            )
        )

    if "fallback" in stages:
        gate_command = [
            str(binary),
            "--path",
            str(RUNTIME_GATE_PROJECT),
            "--quit-after",
            "1",
            "--rendering-driver",
            "metal",
        ]
        commands.extend([
            command_record(
                "runtime-gate-supported",
                gate_command,
                requires_passed="caps-probe" if args.arch == "arm64" else None,
                preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                required_log_patterns=("capability_gate=enabled",),
            ),
            command_record(
                "runtime-gate-forced-fallback",
                gate_command,
                requires_passed="runtime-gate-supported" if args.arch == "arm64" else None,
                preset_skip_reason="unsupported_arch" if args.arch != "arm64" else None,
                environment={"GODOT_MTL_DISABLE_RAYTRACING": "1"},
                required_log_patterns=("using non-RT rendering fallback", "capability_gate=disabled:forced_disabled"),
            ),
        ])

    return commands


def write_summary(path: Path, summary: dict[str, Any]) -> None:
    temporary = path.with_suffix(".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as file:
        json.dump(summary, file, indent=2, sort_keys=True)
        file.write("\n")
    temporary.replace(path)


def run_command(command: dict[str, Any], index: int, artifact_dir: Path, dry_run: bool) -> int:
    print(f"[{index:02d}] {command['name']}: {command['command_string']}", flush=True)
    if dry_run:
        command["status"] = "dry-run"
        return 0

    executable = command["command"][0]
    if shutil.which(executable) is None and not Path(executable).is_file():
        message = f"ERROR: executable not found: {executable}\n"
        print(message, end="", file=sys.stderr)
        log_path = artifact_dir / f"{index:02d}-{command['name']}.log"
        log_path.write_text(message, encoding="utf-8")
        command["log"] = log_path.name
        command["exit_code"] = 127
        command["status"] = "failed"
        return 127

    log_path = artifact_dir / f"{index:02d}-{command['name']}.log"
    command["log"] = log_path.name
    started = time.monotonic()
    with log_path.open("w", encoding="utf-8", newline="\n") as log:
        # Tests that produce image evidence (for example the PATH_TRACER controlled
        # scene) write it into the stage artifact directory via this variable.
        command_env = {
            **os.environ,
            "GODOT_MRT_ARTIFACT_DIR": str(artifact_dir),
            **command["environment"],
        }
        process = subprocess.Popen(
            command["command"],
            cwd=REPO_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            env=command_env,
        )
        assert process.stdout is not None
        for line in process.stdout:
            print(line, end="", flush=True)
            log.write(line)
        exit_code = process.wait()

    command["duration_seconds"] = round(time.monotonic() - started, 3)
    command["exit_code"] = exit_code
    if exit_code == 0:
        log_text = log_path.read_text(encoding="utf-8", errors="replace")
        missing_patterns = [pattern for pattern in command["required_log_patterns"] if pattern not in log_text]
        forbidden_patterns = [pattern for pattern in command["forbidden_log_patterns"] if pattern in log_text]
        count_mismatches = [
            f"{pattern!r} occurred {log_text.count(pattern)} time(s), expected {expected}"
            for pattern, expected in command["required_log_counts"].items()
            if log_text.count(pattern) != expected
        ]
        if missing_patterns or forbidden_patterns or count_mismatches:
            errors = []
            if missing_patterns:
                errors.append(f"required log pattern(s) not found: {', '.join(missing_patterns)}")
            if forbidden_patterns:
                errors.append(f"forbidden log pattern(s) found: {', '.join(forbidden_patterns)}")
            if count_mismatches:
                errors.append("log count mismatch: " + "; ".join(count_mismatches))
            message = f"ERROR: {'; '.join(errors)}\n"
            print(message, end="", file=sys.stderr)
            with log_path.open("a", encoding="utf-8", newline="\n") as log:
                log.write(message)
            command["exit_code"] = 1
            command["status"] = "failed"
            return 1
        command["status"] = "passed"
        # A test may exit zero after declining to exercise the GPU (for example
        # when the caps probe passed but the device lacks a required feature).
        # The skip contract requires such runs to be recorded as skips.
        if command.get("detect_log_skip"):
            skip_match = re.search(r"SKIP_REASON=([a-z_]+)", log_text)
            if skip_match:
                command["status"] = "skipped"
                command["skip_reason"] = skip_match.group(1)
    elif exit_code in command["skip_exit_codes"]:
        command["status"] = "skipped"
    else:
        command["status"] = "failed"
    return exit_code


def skip_command(command: dict[str, Any], index: int, artifact_dir: Path, reason: str) -> None:
    message = f"SKIP_REASON={reason}\n"
    print(f"[{index:02d}] {command['name']}: {message}", end="", flush=True)
    log_path = artifact_dir / f"{index:02d}-{command['name']}.log"
    log_path.write_text(message, encoding="utf-8")
    command["log"] = log_path.name
    command["status"] = "skipped"
    command["skip_reason"] = reason


def main() -> int:
    args = parse_args()
    stages = resolve_stages(args.stage)
    timestamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    stage_slug = "all" if stages == list(VALID_STAGES) else "+".join(stages)
    artifact_dir = (
        repo_path(args.artifact_dir)
        if args.artifact_dir
        else DEFAULT_ARTIFACT_ROOT / f"{timestamp}-{args.arch}-{stage_slug}"
    )
    binary = repo_path(args.binary) if args.binary else REPO_ROOT / "bin" / f"godot.macos.editor.{args.arch}"
    artifact_dir.mkdir(parents=True, exist_ok=True)

    commands = make_commands(args, stages, binary, artifact_dir)
    summary: dict[str, Any] = {
        "schema_version": 1,
        "started_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "finished_at_utc": None,
        "status": "running",
        "dry_run": args.dry_run,
        "fail_on_skip": args.fail_on_skip,
        "repository": str(REPO_ROOT),
        "git_commit": git_output("rev-parse", "HEAD"),
        "git_branch": git_output("branch", "--show-current"),
        "git_dirty": bool(git_output("status", "--porcelain")),
        "host": {
            "system": platform.system(),
            "release": platform.release(),
            "machine": platform.machine(),
            "python": platform.python_version(),
        },
        "requested_arch": args.arch,
        "binary": str(binary),
        "stages": stages,
        "commands": commands,
        "artifacts": [],
    }
    summary_path = artifact_dir / "summary.json"
    write_summary(summary_path, summary)

    failed = False
    commands_by_name = {command["name"]: command for command in commands}
    for index, command in enumerate(commands, start=1):
        if not args.dry_run and command["preset_skip_reason"]:
            skip_command(command, index, artifact_dir, command["preset_skip_reason"])
            write_summary(summary_path, summary)
            if args.fail_on_skip:
                failed = True
                if not args.keep_going:
                    break
            continue

        required_name = command["requires_passed"]
        if not args.dry_run and required_name:
            required = commands_by_name[required_name]
            if required["status"] != "passed":
                reason = "missing_metal_rt_feature" if required["status"] == "skipped" else "runner_not_configured"
                skip_command(command, index, artifact_dir, reason)
                write_summary(summary_path, summary)
                if args.fail_on_skip:
                    failed = True
                    if not args.keep_going:
                        break
                continue

        exit_code = run_command(command, index, artifact_dir, args.dry_run)
        write_summary(summary_path, summary)
        if exit_code != 0 and command["status"] != "skipped":
            failed = True
            if not args.keep_going:
                break
        elif command["status"] == "skipped" and args.fail_on_skip:
            failed = True
            if not args.keep_going:
                break

    for command in commands:
        if command["status"] == "pending":
            command["status"] = "not-run"

    summary["finished_at_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    summary["status"] = "failed" if failed else ("dry-run" if args.dry_run else "passed")
    summary["artifacts"] = sorted(
        path.name
        for path in artifact_dir.iterdir()
        if path.is_file() and path.name not in {"capability_probe", "summary.tmp"}
    )
    write_summary(summary_path, summary)
    print(f"Summary: {summary_path}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
