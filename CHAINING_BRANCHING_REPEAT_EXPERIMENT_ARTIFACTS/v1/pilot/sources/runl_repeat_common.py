#!/usr/bin/env python3
"""Shared, construction-neutral helpers for the repeat Run_L experiment."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import math
import os
from pathlib import Path
import re
import tempfile
from typing import Any, Iterable


ROOT = Path(__file__).resolve().parent
SCHEMA_VERSION = 1
DEFAULT_LANE_WIDTHS = (2, 4, 8, 16, 32, 64)
SAFE_IDENTIFIER = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$")
STREAM_CHUNK_SIZE = 1024 * 1024


@dataclass(frozen=True)
class CaseSpec:
    """One immutable mapping case from the generated manifest."""

    order: int
    case_id: str
    replicate_id: str
    seed: int
    group: str
    parameters: dict[str, Any]
    reference: dict[str, Any]
    query: dict[str, Any]
    construction_artifact: dict[str, Any]
    expectation: dict[str, Any]


def utc_timestamp() -> str:
    """Return a millisecond-resolution UTC timestamp."""

    from datetime import datetime, timezone

    return (
        datetime.now(timezone.utc)
        .isoformat(timespec="milliseconds")
        .replace("+00:00", "Z")
    )


def sha256_file(path: Path) -> str:
    """Return a streaming SHA-256 digest for ``path``."""

    digest = hashlib.sha256()
    with path.open("rb") as source:
        while block := source.read(STREAM_CHUNK_SIZE):
            digest.update(block)
    return digest.hexdigest()


def sha256_bytes(value: bytes) -> str:
    """Return a SHA-256 digest for a byte string."""

    return hashlib.sha256(value).hexdigest()


def canonical_json_bytes(value: object) -> bytes:
    """Serialize a JSON-compatible value deterministically."""

    return json.dumps(
        value,
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def canonical_json_sha256(value: object) -> str:
    """Hash a value using the manifest's canonical JSON representation."""

    return sha256_bytes(canonical_json_bytes(value))


def atomic_write_bytes(path: Path, chunks: Iterable[bytes]) -> str:
    """Write byte chunks atomically and return the completed file digest."""

    path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path: Path | None = None
    digest = hashlib.sha256()
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb",
            dir=path.parent,
            prefix=f".{path.name}.",
            suffix=".tmp",
            delete=False,
        ) as output:
            temporary_path = Path(output.name)
            for chunk in chunks:
                digest.update(chunk)
                output.write(chunk)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary_path, path)
        return digest.hexdigest()
    except BaseException:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)
        raise


def atomic_write_json(path: Path, value: object) -> str:
    """Write a stable, human-readable JSON document atomically."""

    encoded = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")
    return atomic_write_bytes(path, (encoded,))


def resolve_manifest_path(manifest_path: Path, declared_path: str) -> Path:
    """Resolve an artifact below the manifest directory without path escape."""

    candidate = Path(declared_path)
    if candidate.is_absolute():
        raise ValueError(f"manifest artifact path must be relative: {declared_path!r}")
    root = manifest_path.parent.resolve()
    resolved = (root / candidate).resolve()
    try:
        resolved.relative_to(root)
    except ValueError as error:
        raise ValueError(
            f"manifest artifact escapes its directory: {declared_path!r}"
        ) from error
    return resolved


def _require_int(value: Any, label: str, *, minimum: int | None = None) -> int:
    """Validate a non-boolean integer with an optional inclusive lower bound."""

    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{label} must be an integer")
    if minimum is not None and value < minimum:
        raise ValueError(f"{label} must be at least {minimum}")
    return value


def _validate_fasta_artifact(
    manifest_path: Path, case_id: str, label: str, artifact: Any
) -> dict[str, Any]:
    """Validate one immutable FASTA declaration and its current file digest."""

    if not isinstance(artifact, dict):
        raise ValueError(f"case {case_id}.{label} must be an object")
    required = ("path", "file_sha256", "sequence_sha256", "name", "length")
    missing = [field for field in required if field not in artifact]
    if missing:
        raise ValueError(f"case {case_id}.{label} lacks fields: {missing}")
    if not isinstance(artifact["path"], str):
        raise ValueError(f"case {case_id}.{label}.path must be a string")
    resolved = resolve_manifest_path(manifest_path, artifact["path"])
    if not resolved.is_file():
        raise ValueError(f"case {case_id}.{label} file is missing: {resolved}")
    observed = sha256_file(resolved)
    if observed != artifact["file_sha256"]:
        raise ValueError(
            f"case {case_id}.{label} file digest mismatch: "
            f"expected {artifact['file_sha256']}, observed {observed}"
        )
    _require_int(artifact["length"], f"case {case_id}.{label}.length", minimum=1)
    if not isinstance(artifact["name"], str) or not artifact["name"]:
        raise ValueError(f"case {case_id}.{label}.name must be nonempty")
    observed_name: str | None = None
    sequence_digest = hashlib.sha256()
    sequence_length = 0
    alphabet: set[int] = set()
    with resolved.open("rb") as source:
        for line_number, raw_line in enumerate(source, start=1):
            line = raw_line.rstrip(b"\r\n")
            if line.startswith(b">"):
                if observed_name is not None:
                    raise ValueError(
                        f"case {case_id}.{label} contains multiple FASTA records"
                    )
                token = line[1:].split(maxsplit=1)[0]
                if not token:
                    raise ValueError(f"case {case_id}.{label} has an empty FASTA name")
                observed_name = token.decode("ascii")
                continue
            if observed_name is None:
                raise ValueError(
                    f"case {case_id}.{label}:{line_number} sequence precedes header"
                )
            compact = b"".join(line.split()).upper()
            if any(base not in b"ACGTN" for base in compact):
                raise ValueError(
                    f"case {case_id}.{label}:{line_number} has a non-ACGTN base"
                )
            sequence_digest.update(compact)
            sequence_length += len(compact)
            alphabet.update(compact)
    if observed_name != artifact["name"]:
        raise ValueError(
            f"case {case_id}.{label} name mismatch: "
            f"declared {artifact['name']!r}, observed {observed_name!r}"
        )
    if sequence_length != artifact["length"]:
        raise ValueError(
            f"case {case_id}.{label} length mismatch: "
            f"declared {artifact['length']}, observed {sequence_length}"
        )
    if sequence_digest.hexdigest() != artifact["sequence_sha256"]:
        raise ValueError(f"case {case_id}.{label} sequence digest mismatch")
    declared_alphabet = artifact.get("alphabet")
    observed_alphabet = "ACGTN" if ord("N") in alphabet else "ACGT"
    if declared_alphabet is not None and declared_alphabet != observed_alphabet:
        raise ValueError(
            f"case {case_id}.{label} alphabet mismatch: "
            f"declared {declared_alphabet}, observed {observed_alphabet}"
        )
    return {**artifact, "resolved_path": str(resolved)}


def _validate_json_artifact(
    manifest_path: Path, case_id: str, artifact: Any
) -> dict[str, Any]:
    """Validate one construction-description JSON artifact and its digest."""

    if not isinstance(artifact, dict):
        raise ValueError(f"case {case_id}.construction_artifact must be an object")
    if not isinstance(artifact.get("path"), str) or not isinstance(
        artifact.get("file_sha256"), str
    ):
        raise ValueError(
            f"case {case_id}.construction_artifact requires path and file_sha256"
        )
    resolved = resolve_manifest_path(manifest_path, artifact["path"])
    if not resolved.is_file():
        raise ValueError(f"case {case_id} construction artifact is missing: {resolved}")
    observed = sha256_file(resolved)
    if observed != artifact["file_sha256"]:
        raise ValueError(
            f"case {case_id} construction artifact digest mismatch: "
            f"expected {artifact['file_sha256']}, observed {observed}"
        )
    with resolved.open("r", encoding="utf-8") as source:
        parsed = json.load(source)
    if not isinstance(parsed, dict):
        raise ValueError(f"case {case_id} construction artifact must contain an object")
    declared_content_digest = artifact.get("content_sha256")
    if (
        declared_content_digest is not None
        and declared_content_digest != canonical_json_sha256(parsed)
    ):
        raise ValueError(f"case {case_id} construction content digest mismatch")
    return {**artifact, "resolved_path": str(resolved)}


def load_manifest(path: Path) -> tuple[dict[str, Any], list[CaseSpec]]:
    """Load and fully validate a completed repeat-experiment manifest."""

    manifest_path = path.resolve()
    with manifest_path.open("r", encoding="utf-8") as source:
        manifest = json.load(source)
    if not isinstance(manifest, dict):
        raise ValueError("manifest root must be an object")
    if manifest.get("schema_version") != SCHEMA_VERSION:
        raise ValueError(f"manifest.schema_version must be {SCHEMA_VERSION}")
    if manifest.get("status") != "complete":
        raise ValueError("manifest.status must be 'complete'")
    if manifest.get("experiment_id") != "runl_repeat_disruption":
        raise ValueError("manifest.experiment_id must be 'runl_repeat_disruption'")
    seed_plan = manifest.get("seed_plan")
    if not isinstance(seed_plan, dict):
        raise ValueError("manifest.seed_plan must be an object")
    observed_seed_plan_digest = canonical_json_sha256(seed_plan)
    if manifest.get("seed_plan_sha256") != observed_seed_plan_digest:
        raise ValueError("manifest.seed_plan_sha256 does not match seed_plan")

    lane_widths = manifest.get("lane_widths")
    if lane_widths != list(DEFAULT_LANE_WIDTHS):
        raise ValueError(
            f"manifest.lane_widths must be {list(DEFAULT_LANE_WIDTHS)}"
        )

    mapping = manifest.get("mapping")
    if not isinstance(mapping, dict):
        raise ValueError("manifest.mapping must be an object")
    arguments = mapping.get("args")
    if not isinstance(arguments, list) or any(
        not isinstance(argument, str) for argument in arguments
    ):
        raise ValueError("manifest.mapping.args must be an array of strings")
    if int(mapping.get("threads", 1)) != 1:
        raise ValueError("manifest.mapping.threads must be 1")
    timeout_seconds = float(mapping.get("timeout_seconds", 0))
    if not math.isfinite(timeout_seconds) or timeout_seconds <= 0:
        raise ValueError("manifest.mapping.timeout_seconds must be positive")
    if mapping.get("emit_seed_coordinates") is not True:
        raise ValueError("manifest.mapping.emit_seed_coordinates must be true")

    raw_cases = manifest.get("cases")
    if not isinstance(raw_cases, list) or not raw_cases:
        raise ValueError("manifest.cases must be a nonempty array")
    cases: list[CaseSpec] = []
    seen_case_ids: set[str] = set()
    for order, raw_case in enumerate(raw_cases):
        if not isinstance(raw_case, dict):
            raise ValueError(f"manifest.cases[{order}] must be an object")
        case_id = str(raw_case.get("case_id", ""))
        replicate_id = str(raw_case.get("replicate_id", ""))
        group = str(raw_case.get("group", ""))
        if not SAFE_IDENTIFIER.fullmatch(case_id):
            raise ValueError(f"invalid case_id: {case_id!r}")
        if case_id in seen_case_ids:
            raise ValueError(f"duplicate case_id: {case_id}")
        seen_case_ids.add(case_id)
        if not SAFE_IDENTIFIER.fullmatch(replicate_id):
            raise ValueError(f"case {case_id} has invalid replicate_id")
        if not SAFE_IDENTIFIER.fullmatch(group):
            raise ValueError(f"case {case_id} has invalid group")
        seed = _require_int(raw_case.get("seed"), f"case {case_id}.seed", minimum=0)
        parameters = raw_case.get("parameters")
        expectation = raw_case.get("expectation")
        if not isinstance(parameters, dict):
            raise ValueError(f"case {case_id}.parameters must be an object")
        if not isinstance(expectation, dict):
            raise ValueError(f"case {case_id}.expectation must be an object")
        reference = _validate_fasta_artifact(
            manifest_path, case_id, "reference", raw_case.get("reference")
        )
        query = _validate_fasta_artifact(
            manifest_path, case_id, "query", raw_case.get("query")
        )
        construction_artifact = _validate_json_artifact(
            manifest_path, case_id, raw_case.get("construction_artifact")
        )
        cases.append(
            CaseSpec(
                order=order,
                case_id=case_id,
                replicate_id=replicate_id,
                seed=seed,
                group=group,
                parameters=parameters,
                reference=reference,
                query=query,
                construction_artifact=construction_artifact,
                expectation=expectation,
            )
        )
    return manifest, cases
