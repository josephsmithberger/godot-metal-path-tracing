#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST = REPO_ROOT / "mac-rt-planning" / "next-chunks.json"
EXPECTED_CHUNKS = [f"C{number}" for number in range(13, 19)]
MARKDOWN_LINK = re.compile(r"\[[^\]]*\]\(([^)]+)\)")
CHUNK_ID = re.compile(r"^C(\d+)$")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Validate the C13-C18 Metal RT planning package.")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST, help="Planning manifest to validate.")
    parser.add_argument("--json-output", type=Path, help="Optional machine-readable check result.")
    return parser.parse_args()


def resolve_from_repo(path: Path) -> Path:
    return path if path.is_absolute() else REPO_ROOT / path


def read_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError("manifest root must be an object")
    return value


def chunk_number(chunk_id: str) -> int | None:
    match = CHUNK_ID.fullmatch(chunk_id)
    return int(match.group(1)) if match else None


def main() -> int:
    args = parse_args()
    manifest_path = resolve_from_repo(args.manifest)
    checks: list[dict[str, str]] = []

    def record(name: str, passed: bool, detail: str) -> None:
        checks.append({"name": name, "status": "passed" if passed else "failed", "detail": detail})

    try:
        manifest = read_json(manifest_path)
    except (OSError, json.JSONDecodeError, ValueError) as error:
        print(f"MetalRT next-chunk plan check error: {error}", file=sys.stderr)
        return 2

    record("schema", manifest.get("schema_version") == 1, "schema_version must be 1")
    record("plan_id", manifest.get("plan_id") == "metal-rt-editor-parity-c13-c18", "stable plan ID")

    vocabulary = manifest.get("status_vocabulary")
    vocabulary_valid = isinstance(vocabulary, list) and {
        "available",
        "scaffolded",
        "planned",
        "blocked",
    }.issubset(vocabulary)
    record("status_vocabulary", vocabulary_valid, "required status terms are declared")

    profiles = manifest.get("test_profiles")
    profiles_valid = isinstance(profiles, list) and all(isinstance(profile, str) for profile in profiles)
    record("test_profiles", profiles_valid, "test profile catalog is a string list")
    profile_set = set(profiles) if profiles_valid else set()

    documents = manifest.get("documents")
    documents_valid = isinstance(documents, list) and all(isinstance(document, str) for document in documents)
    record("documents", documents_valid, "document catalog is a string list")
    markdown_documents: list[Path] = []
    if documents_valid:
        for document in documents:
            document_path = resolve_from_repo(Path(document))
            exists = document_path.is_file()
            record(f"document:{document}", exists, "tracked planning document exists")
            if exists and document_path.suffix == ".md":
                markdown_documents.append(document_path)

    runner = manifest.get("canonical_test_runner")
    runner_valid = isinstance(runner, str) and resolve_from_repo(Path(runner)).is_file()
    record("canonical_test_runner", runner_valid, "canonical runtime runner exists")

    chunks = manifest.get("chunks")
    chunks_valid = isinstance(chunks, list) and all(isinstance(chunk, dict) for chunk in chunks)
    record("chunks", chunks_valid, "chunk catalog is an object list")
    chunk_ids = [chunk.get("id") for chunk in chunks] if chunks_valid else []
    record("chunk_ids", chunk_ids == EXPECTED_CHUNKS, f"expected ordered IDs {', '.join(EXPECTED_CHUNKS)}")

    if chunks_valid:
        for chunk in chunks:
            chunk_id = chunk.get("id")
            label = chunk_id if isinstance(chunk_id, str) else "invalid"
            number = chunk_number(label)
            status = chunk.get("status")
            record(f"{label}:title", isinstance(chunk.get("title"), str) and bool(chunk["title"]), "title is set")
            record(f"{label}:status", status in vocabulary if vocabulary_valid else False, f"status={status!r}")

            plan = chunk.get("plan")
            plan_valid = isinstance(plan, str) and resolve_from_repo(Path(plan)).is_file()
            record(f"{label}:plan", plan_valid, f"plan={plan!r}")
            if plan_valid:
                plan_path = resolve_from_repo(Path(plan))
                markdown_documents.append(plan_path)
                plan_text = plan_path.read_text(encoding="utf-8")
                heading_valid = plan_text.startswith(f"# {label}:")
                status_label = status.capitalize() if isinstance(status, str) else ""
                status_valid = f"**Status:** {status_label}" in plan_text
                record(f"{label}:heading", heading_valid, "plan begins with the stable chunk ID")
                record(f"{label}:status_sync", status_valid, "manifest and Markdown status agree")

            dependencies = chunk.get("depends_on")
            dependencies_valid = isinstance(dependencies, list) and all(
                isinstance(dependency, str) and chunk_number(dependency) is not None for dependency in dependencies
            )
            if dependencies_valid and number is not None:
                dependencies_valid = all(chunk_number(dependency) < number for dependency in dependencies)
            record(f"{label}:dependencies", dependencies_valid, "dependencies are valid earlier chunks")

            required_profiles = chunk.get("required_test_profiles")
            required_profiles_valid = isinstance(required_profiles, list) and bool(required_profiles) and all(
                isinstance(profile, str) and profile in profile_set for profile in required_profiles
            )
            record(f"{label}:test_profiles", required_profiles_valid, "required profiles exist in the catalog")

            stages = chunk.get("future_runner_stages")
            stages_valid = isinstance(stages, list) and bool(stages) and all(isinstance(stage, str) for stage in stages)
            record(f"{label}:runner_stages", stages_valid, "future runner stages are declared")

            markers = chunk.get("acceptance_markers")
            markers_valid = isinstance(markers, list) and bool(markers) and all(
                isinstance(marker, str) and marker.startswith("METAL_RT_") and "=" in marker for marker in markers
            )
            record(f"{label}:markers", markers_valid, "machine-readable acceptance markers are declared")

    checked_documents: set[Path] = set()
    for markdown_path in markdown_documents:
        if markdown_path in checked_documents:
            continue
        checked_documents.add(markdown_path)
        try:
            text = markdown_path.read_text(encoding="utf-8")
        except OSError as error:
            record(f"links:{markdown_path.name}", False, str(error))
            continue
        for raw_target in MARKDOWN_LINK.findall(text):
            target = raw_target.strip().split("#", 1)[0]
            if not target or "://" in target or target.startswith("mailto:"):
                continue
            target_path = (markdown_path.parent / target).resolve()
            relative_document = markdown_path.relative_to(REPO_ROOT)
            record(
                f"link:{relative_document}:{raw_target}",
                target_path.exists(),
                "local Markdown target exists",
            )

    failed = [check for check in checks if check["status"] == "failed"]
    result = {
        "schema_version": 1,
        "status": "failed" if failed else "passed",
        "manifest": str(manifest_path.relative_to(REPO_ROOT)),
        "chunk_statuses": {
            chunk.get("id", "invalid"): chunk.get("status", "missing") for chunk in chunks
        }
        if chunks_valid
        else {},
        "checks": checks,
        "failed_checks": len(failed),
    }

    if args.json_output:
        output_path = resolve_from_repo(args.json_output)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    print(
        "MetalRT next-chunk plan check: "
        f"status={result['status']} checks={len(checks)} chunks={len(chunk_ids)} failed={len(failed)}"
    )
    for failure in failed:
        print(f"  FAIL {failure['name']}: {failure['detail']}", file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
