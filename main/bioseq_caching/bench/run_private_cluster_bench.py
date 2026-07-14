#!/usr/bin/env python3
"""Run core and SQL bioseq_cache benchmarks in a disposable PostgreSQL node."""

from __future__ import annotations

import argparse
import concurrent.futures
import contextlib
import dataclasses
import datetime as dt
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import select
import shutil
import subprocess
import sys
import tempfile
import time
from typing import Any, Mapping, Sequence


SCRIPT_DIR = Path(__file__).resolve().parent
CACHE_ROOT = SCRIPT_DIR.parent
REPOSITORY_ROOT = CACHE_ROOT.parents[1]
PRIVATE_RUNNER_PATH = CACHE_ROOT / "test" / "run_private_cluster.py"


def _load_private_runner() -> Any:
    spec = importlib.util.spec_from_file_location(
        "bioseq_cache_private_cluster", PRIVATE_RUNNER_PATH
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load private-cluster runner: {PRIVATE_RUNNER_PATH}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


PRIVATE = _load_private_runner()


class BenchmarkError(RuntimeError):
    """The benchmark cannot produce a trustworthy report."""


@dataclasses.dataclass(frozen=True)
class QueryOutput:
    elapsed_ns: int
    first_row_ns: int | None
    completed_at_ns: int
    rows: int
    ordered_sha256: str
    multiset_sha256: str
    stdout_bytes: int


def _required_path(environment_name: str, *, directory: bool = False) -> Path:
    value = os.environ.get(environment_name)
    if not value:
        raise BenchmarkError(f"{environment_name} is required")
    path = Path(value).expanduser().resolve()
    valid = path.is_dir() if directory else path.is_file()
    if not valid:
        kind = "directory" if directory else "file"
        raise BenchmarkError(f"{environment_name} is not a {kind}: {path}")
    return path


def _postgresql_bindir() -> Path:
    configured = os.environ.get("POSTGRESQL_BINDIR")
    if configured:
        path = Path(configured).expanduser().resolve()
        if path.is_dir():
            return path
        raise BenchmarkError(f"POSTGRESQL_BINDIR is not a directory: {path}")
    pg_config = shutil.which("pg_config")
    if pg_config is None:
        raise BenchmarkError("pg_config is unavailable and POSTGRESQL_BINDIR is unset")
    completed = subprocess.run(
        [pg_config, "--bindir"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        raise BenchmarkError(f"pg_config --bindir failed: {completed.stdout}")
    path = Path(completed.stdout.strip()).resolve()
    if not path.is_dir():
        raise BenchmarkError(f"pg_config returned a missing bindir: {path}")
    return path


def _read_cases(path: Path) -> dict[str, Any]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if document.get("schema_version") != 1:
        raise BenchmarkError("unsupported SQL benchmark case schema")
    cases = document.get("cases")
    if not isinstance(cases, list) or not cases:
        raise BenchmarkError("SQL benchmark case list is empty")
    required = {"key", "category", "ordered", "sql"}
    seen: set[str] = set()
    for case in cases:
        if not isinstance(case, dict) or not required.issubset(case):
            raise BenchmarkError(f"malformed benchmark case: {case!r}")
        key = str(case["key"])
        if key in seen:
            raise BenchmarkError(f"duplicate benchmark key: {key}")
        seen.add(key)
        if ";" in str(case["sql"]):
            raise BenchmarkError(f"benchmark SQL must be one statement: {key}")
    return document


def _run_core_benchmark(executable: Path, output: Path, log: Path) -> dict[str, Any]:
    completed = subprocess.run(
        [str(executable), "--output", str(output)],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    log.write_text(completed.stdout, encoding="utf-8")
    if completed.returncode != 0:
        raise BenchmarkError(
            f"core benchmark exited {completed.returncode}; see {log}"
        )
    if not output.is_file():
        raise BenchmarkError("core benchmark did not write its JSON fragment")
    fragment = json.loads(output.read_text(encoding="utf-8"))
    if not isinstance(fragment, dict):
        raise BenchmarkError("core benchmark fragment is not a JSON object")
    return fragment


def _psql_command(cluster: Any, sql: str) -> list[str]:
    return [
        str(cluster.inputs.bindir / "psql"),
        "-XqAt",
        "-v",
        "ON_ERROR_STOP=1",
        "--pset",
        "null=\\N",
        "--field-separator=\t",
        "--host",
        str(cluster.socket_dir),
        "--port",
        str(cluster.port),
        "--username",
        cluster.database_user,
        "--dbname",
        cluster.database,
        "--command",
        sql,
    ]


def _run_sql(cluster: Any, sql: str) -> tuple[str, int]:
    started = time.perf_counter_ns()
    completed = subprocess.run(
        _psql_command(cluster, sql),
        env=cluster.environment(),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    elapsed = time.perf_counter_ns() - started
    if completed.returncode != 0:
        raise BenchmarkError(
            f"SQL benchmark command failed ({completed.returncode}):\n"
            f"{completed.stderr}\nSQL:\n{sql}"
        )
    return completed.stdout, elapsed


class _PauseController:
    """Keep the session-level advisory pause lock until fanout is ready."""

    def __init__(self, cluster: Any, point: str) -> None:
        self._cluster = cluster
        self._point = point
        self._process: subprocess.Popen[str] | None = None
        self._released = False

    def _error(self, message: str) -> BenchmarkError:
        error = ""
        if (
            self._process is not None
            and self._process.poll() is not None
            and self._process.stderr is not None
        ):
            with contextlib.suppress(Exception):
                error = self._process.stderr.read()
        suffix = f": {error}" if error else ""
        return BenchmarkError(message + suffix)

    def _command(self, sql: str, marker: str) -> None:
        process = self._process
        if process is None or process.stdin is None or process.stdout is None:
            raise self._error("pause controller has no psql pipes")
        process.stdin.write(sql + "\n")
        process.stdin.flush()
        deadline = time.monotonic() + 30.0
        while time.monotonic() < deadline:
            ready, _, _ = select.select(
                [process.stdout], [], [], min(0.1, deadline - time.monotonic())
            )
            if not ready:
                if process.poll() is not None:
                    raise self._error("pause controller psql exited early")
                continue
            line = process.stdout.readline()
            if line == "":
                raise self._error("pause controller psql closed stdout")
            if line.rstrip("\r\n") == marker:
                return
        raise self._error(f"pause controller timed out waiting for {marker}")

    def __enter__(self) -> "_PauseController":
        command = _psql_command(self._cluster, "")[:-2]
        self._process = subprocess.Popen(
            command,
            env=self._cluster.environment(),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self._command(
            "SELECT 'BIOSEQ_CACHE_PAUSE_ARMED' FROM "
            f"(SELECT bioseq_cache_test.arm_pause('{self._point}')) AS command;",
            "BIOSEQ_CACHE_PAUSE_ARMED",
        )
        return self

    def release(self) -> None:
        if self._released:
            return
        self._command(
            "SELECT 'BIOSEQ_CACHE_PAUSE_RELEASED' FROM "
            f"(SELECT bioseq_cache_test.release_pause('{self._point}')) AS command;",
            "BIOSEQ_CACHE_PAUSE_RELEASED",
        )
        self._released = True

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        process = self._process
        if process is None:
            return
        if not self._released and process.poll() is None:
            with contextlib.suppress(Exception):
                self.release()
        if process.stdin is not None and process.poll() is None:
            with contextlib.suppress(Exception):
                process.stdin.write("\\q\n")
                process.stdin.flush()
        try:
            process.wait(timeout=10.0)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        if process.returncode not in (0, None) and exc_type is None:
            raise self._error(
                f"pause controller psql exited {process.returncode}"
            )


def _run_query_streaming(cluster: Any, sql: str) -> tuple[str, int, int | None, int]:
    """Capture client-observed first output without buffering the full result first."""
    started = time.perf_counter_ns()
    with tempfile.TemporaryFile() as stderr:
        process = subprocess.Popen(
            _psql_command(cluster, sql),
            env=cluster.environment(),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=stderr,
        )
        if process.stdout is None:
            process.kill()
            raise BenchmarkError("psql streaming stdout pipe was not created")
        first_byte = process.stdout.read(1)
        first_row_ns = (
            time.perf_counter_ns() - started if first_byte != b"" else None
        )
        stdout_bytes = first_byte + process.stdout.read()
        returncode = process.wait()
        completed_at_ns = time.perf_counter_ns()
        elapsed_ns = completed_at_ns - started
        stderr.seek(0)
        error_bytes = stderr.read()
    stdout = stdout_bytes.decode("utf-8")
    error = error_bytes.decode("utf-8", errors="replace")
    if returncode != 0:
        raise BenchmarkError(
            f"SQL benchmark command failed ({returncode}):\n"
            f"{error}\nSQL:\n{sql}"
        )
    return stdout, elapsed_ns, first_row_ns, completed_at_ns


def _digest_output(
    stdout: str,
    elapsed_ns: int,
    first_row_ns: int | None,
    completed_at_ns: int,
) -> QueryOutput:
    normalized = stdout.replace("\r\n", "\n")
    if normalized.endswith("\n"):
        normalized = normalized[:-1]
    rows = [] if normalized == "" else normalized.split("\n")
    ordered = hashlib.sha256()
    for row in rows:
        ordered.update(row.encode("utf-8"))
        ordered.update(b"\n")
    multiset = hashlib.sha256()
    for row in sorted(rows):
        multiset.update(row.encode("utf-8"))
        multiset.update(b"\n")
    return QueryOutput(
        elapsed_ns=elapsed_ns,
        first_row_ns=first_row_ns,
        completed_at_ns=completed_at_ns,
        rows=len(rows),
        ordered_sha256=ordered.hexdigest(),
        multiset_sha256=multiset.hexdigest(),
        stdout_bytes=len(stdout.encode("utf-8")),
    )


def _execute_query(
    cluster: Any,
    *,
    mode: str,
    sql: str,
    ttl_ms: int | None = None,
) -> QueryOutput:
    settings = f"SET bioseq.query_cache_optimisation_mode = '{mode}';\n"
    if ttl_ms is not None:
        settings += f"SET bioseq.query_cache_ttl = {ttl_ms};\n"
    stdout, elapsed, first_row_ns, completed_at_ns = _run_query_streaming(
        cluster,
        settings + sql,
    )
    return _digest_output(stdout, elapsed, first_row_ns, completed_at_ns)


def _percentile(samples: Sequence[int], percentile: float) -> float:
    if not samples:
        raise BenchmarkError("cannot calculate percentile of empty samples")
    ordered = sorted(samples)
    if len(ordered) == 1:
        return float(ordered[0])
    position = (len(ordered) - 1) * percentile
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return float(ordered[lower])
    fraction = position - lower
    return ordered[lower] + (ordered[upper] - ordered[lower]) * fraction


def _sample_summary(samples_ns: Sequence[int]) -> dict[str, Any]:
    return {
        "raw_samples_ns": list(samples_ns),
        "p50_ns": int(_percentile(samples_ns, 0.50)),
        "p95_ns": int(_percentile(samples_ns, 0.95)),
        "p99_ns": int(_percentile(samples_ns, 0.99)),
    }


def _throughput_summary(outputs: Sequence[QueryOutput]) -> dict[str, Any]:
    def rate(quantity: int, elapsed_ns: int) -> int:
        return int(quantity * 1_000_000_000 / max(1, elapsed_ns))

    def summary(samples: Sequence[int], unit: str) -> dict[str, Any]:
        return {
            "raw_samples": list(samples),
            "p50": int(_percentile(samples, 0.50)),
            "p95": int(_percentile(samples, 0.95)),
            "p99": int(_percentile(samples, 0.99)),
            "unit": unit,
        }

    row_rates = [rate(output.rows, output.elapsed_ns) for output in outputs]
    byte_rates = [
        rate(output.stdout_bytes, output.elapsed_ns) for output in outputs
    ]
    return {
        "rows_per_second": summary(row_rates, "rows/s"),
        "output_bytes_per_second": summary(byte_rates, "bytes/s"),
    }


def _unavailable_metric(detail: str) -> dict[str, Any]:
    return {"status": "unavailable", "value": None, "detail": detail}


def _first_row_summary(outputs: Sequence[QueryOutput]) -> dict[str, Any]:
    samples = [
        output.first_row_ns
        for output in outputs
        if output.first_row_ns is not None
    ]
    if not samples:
        return {
            "status": "not_applicable",
            "raw_samples_ns": [],
            "detail": "The query returned no rows, so it has no first-row timestamp.",
        }
    return {
        "status": "measured",
        **_sample_summary(samples),
        "detail": (
            "Client-observed time to the first psql result byte; it includes "
            "planning, execution, cache prevalidation, decoding, and transport."
        ),
    }


def _entry_runtime_snapshot(cluster: Any) -> dict[str, Any]:
    stdout, _ = _run_sql(
        cluster,
        "SELECT COALESCE(jsonb_agg(to_jsonb(entry)), '[]'::jsonb) "
        "FROM bioseq_cache.entries() AS entry",
    )
    entries = json.loads(stdout.strip() or "[]")
    if not isinstance(entries, list):
        raise BenchmarkError(f"unexpected entries JSON: {stdout!r}")

    def aggregate(names: Sequence[str], detail: str) -> dict[str, Any]:
        selected = next(
            (name for name in names if any(name in entry for entry in entries)),
            None,
        )
        if selected is None:
            return _unavailable_metric(detail)
        values = [entry.get(selected) for entry in entries]
        if any(not isinstance(value, (int, float)) for value in values):
            return _unavailable_metric(
                f"Admin field {selected!r} is present but is not wholly numeric."
            )
        return {
            "status": "measured",
            "value": sum(values),
            "source_field": selected,
        }

    return {
        "entry_count": len(entries),
        "logical_bytes": aggregate(
            ("logical_bytes",), "bioseq_cache.entries() exposes no logical bytes"
        ),
        "stored_bytes": aggregate(
            ("stored_bytes",), "bioseq_cache.entries() exposes no stored bytes"
        ),
        "dsa_hot_bytes": aggregate(
            ("dsa_bytes", "hot_bytes"),
            "No DSA/hot-tier byte counter is exposed by the generic admin API.",
        ),
        "file_bytes": aggregate(
            ("file_bytes",),
            "No file-tier byte counter is exposed by the generic admin API.",
        ),
        "prevalidation_duration_ns": aggregate(
            ("prevalidation_ns", "prevalidation_duration_ns"),
            "No isolated prevalidation duration is exposed by the generic admin API.",
        ),
        "child_executions": aggregate(
            ("child_executions", "child_execution_count"),
            "No executor child-Begin/execution count is exposed by the generic admin API.",
        ),
    }


def _same_result(reference: QueryOutput, candidate: QueryOutput, ordered: bool) -> bool:
    expected_digest = (
        reference.ordered_sha256 if ordered else reference.multiset_sha256
    )
    actual_digest = candidate.ordered_sha256 if ordered else candidate.multiset_sha256
    return reference.rows == candidate.rows and expected_digest == actual_digest


def _stats(cluster: Any) -> dict[str, int]:
    stdout, _ = _run_sql(
        cluster,
        "SELECT produced, completed_hits, inflight_waits, reuse_hits, fallbacks, "
        "invalidations, evictions, logical_bytes, stored_bytes "
        "FROM bioseq_cache.stats()",
    )
    values = stdout.strip().split("\t")
    names = [
        "produced",
        "completed_hits",
        "inflight_waits",
        "reuse_hits",
        "fallbacks",
        "invalidations",
        "evictions",
        "logical_bytes",
        "stored_bytes",
    ]
    if len(values) != len(names):
        raise BenchmarkError(f"unexpected stats output: {stdout!r}")
    return {name: int(value) for name, value in zip(names, values)}


def _reset_case(cluster: Any, *, ttl_ms: int = 3_600_000) -> None:
    _run_sql(
        cluster,
        "SET bioseq.query_cache_optimisation_mode = 'disabled'; "
        "SELECT bioseq_cache.invalidate_all(); "
        "SELECT bioseq_cache.reset_stats(); "
        f"SET bioseq.query_cache_ttl = {ttl_ms};",
    )


def _measure_explain(cluster: Any, *, mode: str, sql: str) -> dict[str, Any]:
    stdout, _ = _run_sql(
        cluster,
        f"SET bioseq.query_cache_optimisation_mode = '{mode}'; "
        f"EXPLAIN (ANALYZE, FORMAT JSON, COSTS OFF, TIMING OFF) {sql}",
    )
    document = json.loads(stdout)
    root = document[0]

    def cache_properties(plan: Mapping[str, Any]) -> dict[str, Any] | None:
        if "Cache Storage" in plan:
            return {
                "storage": plan.get("Cache Storage"),
                "action": plan.get("Actual Cache Action"),
                "readiness": plan.get("Cache Readiness"),
                "janitor": plan.get("Cache Janitor"),
            }
        for child in plan.get("Plans", []):
            if isinstance(child, dict):
                found = cache_properties(child)
                if found is not None:
                    return found
        return None

    properties = cache_properties(root.get("Plan", {}))
    return {
        "planning_ms": float(root.get("Planning Time", 0.0)),
        "execution_ms": float(root.get("Execution Time", 0.0)),
        "cache_properties": properties,
    }


def _parse_cache_storage(value: Any) -> dict[str, Any]:
    if not isinstance(value, str):
        return _unavailable_metric(
            "Structured EXPLAIN exposed no Cache Storage property."
        )
    match = re.fullmatch(
        r"(ready|unavailable); backend=([^;]+); hot=(\d+)/(\d+); "
        r"file=(\d+)/(\d+); manifests=(\d+); waiters=(\d+)",
        value,
    )
    if match is None:
        return _unavailable_metric(
            f"Unrecognized Cache Storage EXPLAIN value: {value!r}"
        )
    return {
        "status": "measured",
        "bootstrap": match.group(1),
        "backend": match.group(2),
        "reserved_hot_bytes": int(match.group(3)),
        "hot_ceiling_bytes": int(match.group(4)),
        "reserved_file_bytes": int(match.group(5)),
        "file_ceiling_bytes": int(match.group(6)),
        "manifest_count": int(match.group(7)),
        "waiter_references": int(match.group(8)),
    }


def _measure_explain_samples(
    cluster: Any,
    *,
    mode: str,
    sql: str,
    sample_count: int,
) -> dict[str, Any]:
    samples = [
        _measure_explain(cluster, mode=mode, sql=sql)
        for _ in range(sample_count)
    ]
    planning_ns = [int(sample["planning_ms"] * 1_000_000) for sample in samples]
    execution_ns = [int(sample["execution_ms"] * 1_000_000) for sample in samples]
    planning = _sample_summary(planning_ns)
    execution = _sample_summary(execution_ns)
    properties = [
        sample["cache_properties"]
        for sample in samples
        if sample["cache_properties"] is not None
    ]
    parsed_storage = [
        _parse_cache_storage(property["storage"])
        for property in properties
    ]
    measured_storage = [
        storage
        for storage in parsed_storage
        if storage["status"] == "measured"
    ]
    storage: dict[str, Any]
    if measured_storage:
        storage = {
            **measured_storage[-1],
            "peak_reserved_hot_bytes": max(
                item["reserved_hot_bytes"] for item in measured_storage
            ),
            "peak_reserved_file_bytes": max(
                item["reserved_file_bytes"] for item in measured_storage
            ),
        }
    elif parsed_storage:
        storage = parsed_storage[-1]
    else:
        storage = _unavailable_metric(
            "No cache CustomScan was present in structured EXPLAIN."
        )
    return {
        "planning": planning,
        "execution": execution,
        "planning_ms": planning["p50_ns"] / 1_000_000.0,
        "execution_ms": execution["p50_ns"] / 1_000_000.0,
        "actual_cache_actions": [
            property["action"] for property in properties
        ],
        "cache_readiness": [
            property["readiness"] for property in properties
        ],
        "cache_janitor": [property["janitor"] for property in properties],
        "cache_storage": storage,
    }


def _measure_case(
    cluster: Any,
    case: Mapping[str, Any],
    *,
    sample_count: int,
) -> dict[str, Any]:
    sql = str(case["sql"])
    ordered = bool(case["ordered"])
    _reset_case(cluster)
    reference = _execute_query(cluster, mode="disabled", sql=sql)
    disabled_explain = _measure_explain_samples(
        cluster,
        mode="disabled",
        sql=sql,
        sample_count=sample_count,
    )

    _reset_case(cluster)
    producer = _execute_query(cluster, mode="same", sql=sql)
    hit_samples = [
        _execute_query(cluster, mode="same", sql=sql)
        for _ in range(sample_count)
    ]
    same_explain = _measure_explain_samples(
        cluster,
        mode="same",
        sql=sql,
        sample_count=sample_count,
    )
    case_stats = _stats(cluster)
    runtime = _entry_runtime_snapshot(cluster)
    runtime["storage_explain"] = same_explain["cache_storage"]
    correctness = _same_result(reference, producer, ordered) and all(
        _same_result(reference, sample, ordered) for sample in hit_samples
    )
    hit_action = "[Hit] exact final result"
    cache_behavior = {
        "passed": (
            case_stats["produced"] == 1
            and case_stats["completed_hits"] >= sample_count * 2
            and len(same_explain["actual_cache_actions"]) == sample_count
            and all(
                action == hit_action
                for action in same_explain["actual_cache_actions"]
            )
        ),
        "publications": case_stats["produced"],
        "completed_hits": case_stats["completed_hits"],
        "minimum_expected_completed_hits": sample_count * 2,
        "explain_actions": same_explain["actual_cache_actions"],
    }
    reference_ns = reference.elapsed_ns
    producer_delta = (
        ((producer.elapsed_ns - reference_ns) / reference_ns) * 100.0
        if reference_ns > 0
        else 0.0
    )
    hit_p50 = _percentile([sample.elapsed_ns for sample in hit_samples], 0.50)
    speedup = reference_ns / hit_p50 if hit_p50 > 0 else None
    digest_name = "ordered_sha256" if ordered else "multiset_sha256"
    digest_value = (
        reference.ordered_sha256 if ordered else reference.multiset_sha256
    )
    planning_overhead = (
        ((same_explain["planning_ms"] - disabled_explain["planning_ms"])
         / disabled_explain["planning_ms"])
        * 100.0
        if disabled_explain["planning_ms"] > 0.0
        else None
    )
    hit_first_row = _first_row_summary(hit_samples)
    return {
        "key": str(case["key"]),
        "category": str(case["category"]),
        "ordered": ordered,
        "correctness": {
            "passed": correctness,
            "row_count": reference.rows,
            digest_name: digest_value,
            "stdout_bytes": reference.stdout_bytes,
        },
        "cache_behavior": cache_behavior,
        "disabled": {
            **_sample_summary([reference.elapsed_ns]),
            "time_to_first_row": _first_row_summary([reference]),
            "throughput": _throughput_summary([reference]),
            **disabled_explain,
        },
        "producer": {
            **_sample_summary([producer.elapsed_ns]),
            "time_to_first_row": _first_row_summary([producer]),
            "throughput": _throughput_summary([producer]),
            "baseline_delta_percent": producer_delta,
        },
        "completed_hit": {
            **_sample_summary([sample.elapsed_ns for sample in hit_samples]),
            "time_to_first_row": hit_first_row,
            "throughput": _throughput_summary(hit_samples),
            "speedup_vs_disabled": speedup,
            **same_explain,
        },
        "same_mode_planning_overhead_percent": planning_overhead,
        "prevalidation": {
            "isolated_duration": runtime["prevalidation_duration_ns"],
            "client_first_row_inclusive": hit_first_row,
            "detail": (
                "Completed-hit first-row time includes mandatory full-store "
                "prevalidation, but is not reported as an isolated duration."
            ),
        },
        "runtime_storage": runtime,
        "stats": case_stats,
    }


def _wait_for_waiters(
    cluster: Any, count: int, timeout_seconds: float = 30.0
) -> int:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        stdout, _ = _run_sql(
            cluster,
            "SELECT COALESCE(max(waiter_count), 0) "
            "FROM bioseq_cache_test.entry_states()",
        )
        if int(stdout.strip()) >= count:
            return time.perf_counter_ns()
        time.sleep(0.02)
    raise BenchmarkError(f"waiter count did not reach {count}")


def _measure_fanout(cluster: Any, clients: int, sql: str) -> dict[str, Any]:
    _reset_case(cluster)
    reference = _execute_query(cluster, mode="disabled", sql=sql)
    _reset_case(cluster)

    def execute() -> QueryOutput:
        return _execute_query(cluster, mode="same", sql=sql)

    with concurrent.futures.ThreadPoolExecutor(max_workers=clients) as executor, \
            _PauseController(cluster, "after_building_publish") as pause:
        producer = executor.submit(execute)
        _run_sql(
            cluster,
            "SELECT bioseq_cache_test.wait_until_paused('after_building_publish')",
        )
        waiters = [executor.submit(execute) for _ in range(clients - 1)]
        waiters_observed_at = _wait_for_waiters(cluster, clients - 1)
        released_at = time.perf_counter_ns()
        pause.release()
        producer_output = producer.result()
        waiter_outputs = [waiter.result() for waiter in waiters]
        outputs = [producer_output, *waiter_outputs]
        completed_at = time.perf_counter_ns()

    fanout_stats = _stats(cluster)
    runtime = _entry_runtime_snapshot(cluster)
    correct = all(_same_result(reference, output, False) for output in outputs)
    measured_children = runtime["child_executions"]
    child_executions = measured_children
    if measured_children["status"] != "measured":
        child_executions = {
            "status": "inferred",
            "value": fanout_stats["produced"],
            "detail": (
                "The admin API exposes successful publications, not child Begin "
                "or source execution counts; this value cannot prove that a "
                "waiter never initialized its child."
            ),
        }
    return {
        "key": f"inflight_fanout_{clients}",
        "clients": clients,
        "correctness": {
            "passed": correct,
            "row_count": reference.rows,
            "multiset_sha256": reference.multiset_sha256,
        },
        **_sample_summary([output.elapsed_ns for output in outputs]),
        "time_to_first_row": _first_row_summary(outputs),
        "throughput": _throughput_summary(outputs),
        "producer_time_to_first_row": _first_row_summary([producer_output]),
        "producer_throughput": _throughput_summary([producer_output]),
        "waiter_time_to_first_row": _first_row_summary(waiter_outputs),
        "waiter_throughput": _throughput_summary(waiter_outputs),
        "wait_duration": {
            "status": "partially_measured",
            "waiters_observed_to_release_ns": released_at - waiters_observed_at,
            "release_to_waiter_completion": _sample_summary(
                [
                    max(0, output.completed_at_ns - released_at)
                    for output in waiter_outputs
                ]
            ),
            "server_condition_variable_duration": _unavailable_metric(
                "The runtime exposes waiter counts but no per-waiter CV timestamps."
            ),
        },
        "release_to_all_complete_ns": completed_at - released_at,
        "runtime_storage": runtime,
        "stats": fanout_stats,
        "child_execution_count": child_executions,
    }


def _measure_invalidation(cluster: Any) -> list[dict[str, Any]]:
    results: list[dict[str, Any]] = []
    operations = [
        ("insert", "INSERT INTO cache_bench_mutable VALUES (20001, 1, 200010)"),
        ("update", "UPDATE cache_bench_mutable SET payload = payload + 1 WHERE id = 1"),
        ("delete", "DELETE FROM cache_bench_mutable WHERE id = 2"),
    ]
    query = "SELECT id, group_id, payload FROM cache_bench_mutable ORDER BY id"
    for key, operation in operations:
        _reset_case(cluster)
        producer = _execute_query(cluster, mode="same", sql=query)
        before = _stats(cluster)
        started = time.perf_counter_ns()
        _run_sql(cluster, operation)
        invalidation_ns = time.perf_counter_ns() - started
        reference = _execute_query(cluster, mode="disabled", sql=query)
        output = _execute_query(cluster, mode="same", sql=query)
        after = _stats(cluster)
        results.append(
            {
                "key": f"{key}_invalidation",
                "operation_ns": invalidation_ns,
                "correctness": {
                    "passed": _same_result(reference, output, True),
                    "row_count": reference.rows,
                    "ordered_sha256": reference.ordered_sha256,
                    "multiset_sha256": reference.multiset_sha256,
                },
                "producer_time_to_first_row": _first_row_summary([producer]),
                "post_write_time_to_first_row": _first_row_summary([output]),
                "invalidation_observed": after["produced"] > before["produced"],
                "stats_before_write": before,
                "stats_after_write": after,
                "runtime_storage": _entry_runtime_snapshot(cluster),
            }
        )
    return results


def _measure_unrelated_commit(cluster: Any, sql: str) -> dict[str, Any]:
    _reset_case(cluster)
    reference = _execute_query(cluster, mode="disabled", sql=sql)
    producer = _execute_query(cluster, mode="same", sql=sql)
    before = _stats(cluster)
    started = time.perf_counter_ns()
    _run_sql(cluster, "INSERT INTO cache_bench_unrelated VALUES (1)")
    commit_ns = time.perf_counter_ns() - started
    after_commit = _execute_query(cluster, mode="same", sql=sql)
    after = _stats(cluster)
    return {
        "key": "conservative_unrelated_commit",
        "correctness": {
            "passed": (
                _same_result(reference, producer, False)
                and _same_result(reference, after_commit, False)
            ),
            "row_count": reference.rows,
            "multiset_sha256": reference.multiset_sha256,
        },
        "unrelated_commit_ns": commit_ns,
        "producer_time_to_first_row": _first_row_summary([producer]),
        "post_commit_time_to_first_row": _first_row_summary([after_commit]),
        "conservative_rejection_observed": (
            after["produced"] > before["produced"]
            and after["completed_hits"] == before["completed_hits"]
        ),
        "stats_before_commit": before,
        "stats_after_commit": after,
        "runtime_storage": _entry_runtime_snapshot(cluster),
    }


def _measure_eviction_pressure(cluster: Any) -> dict[str, Any]:
    _reset_case(cluster)
    maximum_requests = 1_152
    minimum_requests = 96
    batch_size = 32
    request_latencies: list[int] = []
    request_first_rows: list[QueryOutput] = []
    all_correct = True
    first_sql = ""
    last_sql = ""
    request_count = 0

    for index in range(maximum_requests):
        sql = (
            "SELECT id, payload FROM cache_bench_narrow "
            f"WHERE id >= {index + 1} ORDER BY id LIMIT 4"
        )
        if index == 0:
            first_sql = sql
        last_sql = sql
        reference = _execute_query(cluster, mode="disabled", sql=sql)
        candidate = _execute_query(cluster, mode="same", sql=sql)
        all_correct = all_correct and _same_result(reference, candidate, True)
        request_latencies.append(candidate.elapsed_ns)
        request_first_rows.append(candidate)
        request_count = index + 1
        if request_count % batch_size == 0:
            snapshot = _stats(cluster)
            if request_count >= minimum_requests and snapshot["evictions"] > 0:
                break

    fill_stats = _stats(cluster)
    fill_runtime = _entry_runtime_snapshot(cluster)

    trigger_index = request_count + 1
    trigger_sql = (
        "SELECT id, payload FROM cache_bench_narrow "
        f"WHERE id >= {trigger_index} ORDER BY id LIMIT 4"
    )
    trigger_reference = _execute_query(cluster, mode="disabled", sql=trigger_sql)
    before_trigger = _stats(cluster)
    trigger_output = _execute_query(cluster, mode="same", sql=trigger_sql)
    after_trigger = _stats(cluster)
    all_correct = all_correct and _same_result(
        trigger_reference, trigger_output, True
    )

    first_reference = _execute_query(cluster, mode="disabled", sql=first_sql)
    before_first_revisit = _stats(cluster)
    first_revisit = _execute_query(cluster, mode="same", sql=first_sql)
    after_first_revisit = _stats(cluster)
    all_correct = all_correct and _same_result(
        first_reference, first_revisit, True
    )

    last_reference = _execute_query(cluster, mode="disabled", sql=last_sql)
    before_last_revisit = _stats(cluster)
    last_revisit = _execute_query(cluster, mode="same", sql=last_sql)
    after_last_revisit = _stats(cluster)
    all_correct = all_correct and _same_result(last_reference, last_revisit, True)

    return {
        "key": "eviction_pressure",
        "requests_until_observation": request_count,
        "maximum_requests": maximum_requests,
        "correctness": {
            "passed": all_correct,
            "row_count_per_request": 4,
            "first_ordered_sha256": first_reference.ordered_sha256,
            "last_ordered_sha256": last_reference.ordered_sha256,
        },
        "fill_latency": _sample_summary(request_latencies),
        "fill_time_to_first_row": _first_row_summary(request_first_rows),
        "eviction_observed": fill_stats["evictions"] > 0,
        "trigger_query": {
            **_sample_summary([trigger_output.elapsed_ns]),
            "time_to_first_row": _first_row_summary([trigger_output]),
            "eviction_counter_advanced": (
                after_trigger["evictions"] > before_trigger["evictions"]
            ),
        },
        "oldest_revisit": {
            "was_miss": (
                after_first_revisit["produced"]
                > before_first_revisit["produced"]
            ),
            "time_to_first_row": _first_row_summary([first_revisit]),
        },
        "newest_revisit": {
            "was_hit": (
                after_last_revisit["completed_hits"]
                > before_last_revisit["completed_hits"]
            ),
            "time_to_first_row": _first_row_summary([last_revisit]),
        },
        "runtime_storage_at_pressure": fill_runtime,
        "stats_after_fill": fill_stats,
        "stats_after_revisits": after_last_revisit,
    }


def _measure_ttl_and_eviction(cluster: Any, sql: str) -> dict[str, Any]:
    _reset_case(cluster, ttl_ms=1)
    _execute_query(cluster, mode="same", sql=sql, ttl_ms=1)
    time.sleep(0.01)
    before = _stats(cluster)
    started = time.perf_counter_ns()
    _run_sql(cluster, "SELECT bioseq_cache_test.request_janitor_sweep()")
    deadline = time.monotonic() + 10.0
    remaining = -1
    while time.monotonic() < deadline:
        stdout, _ = _run_sql(cluster, "SELECT count(*) FROM bioseq_cache.entries()")
        remaining = int(stdout.strip())
        if remaining == 0:
            break
        time.sleep(0.02)
    return {
        "key": "ttl_cleanup",
        "eviction_ns": time.perf_counter_ns() - started,
        "entries_remaining": remaining,
        "stats_before": before,
        "stats_after": _stats(cluster),
        "runtime_storage_after": _entry_runtime_snapshot(cluster),
    }


def _setup_fixture(cluster: Any, rows: int) -> None:
    setup_sql = f"""
    CREATE TABLE cache_bench_narrow(
      id bigint PRIMARY KEY, group_id integer NOT NULL, payload bigint NOT NULL);
    INSERT INTO cache_bench_narrow
    SELECT i, i % 101, i * 10 FROM generate_series(1, {rows}) AS g(i);

    CREATE TABLE cache_bench_mutable AS TABLE cache_bench_narrow;
    CREATE UNIQUE INDEX ON cache_bench_mutable(id);

    CREATE TABLE cache_bench_wide(
      id bigint PRIMARY KEY, optional_value integer, payload text NOT NULL);
    INSERT INTO cache_bench_wide
    SELECT i, CASE WHEN i % 3 = 0 THEN NULL ELSE i % 1000 END,
           repeat(md5(i::text), 16)
    FROM generate_series(1, {max(2000, rows // 10)}) AS g(i);

    CREATE TABLE cache_bench_toast(id bigint PRIMARY KEY, payload text NOT NULL);
    INSERT INTO cache_bench_toast
    SELECT i, repeat(md5(i::text), 2048)
    FROM generate_series(1, 64) AS g(i);

    CREATE TABLE cache_bench_unrelated(id bigint PRIMARY KEY);
    ANALYZE cache_bench_narrow;
    ANALYZE cache_bench_wide;
    ANALYZE cache_bench_toast;
    """
    _run_sql(cluster, setup_sql)


def _git_commit() -> str:
    completed = subprocess.run(
        ["git", "-C", str(REPOSITORY_ROOT), "rev-parse", "HEAD"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    return completed.stdout.strip() if completed.returncode == 0 else "unknown"


def _core_metric(core: Mapping[str, Any], name: str) -> float | None:
    metrics = core.get("metrics")
    if isinstance(metrics, dict):
        value = metrics.get(name)
        if isinstance(value, (int, float)):
            return float(value)
    value = core.get(name)
    return float(value) if isinstance(value, (int, float)) else None


def _threshold(
    name: str,
    *,
    actual: Any,
    limit: Any,
    passed: bool | None,
    detail: str,
) -> dict[str, Any]:
    status = "not_applicable" if passed is None else ("pass" if passed else "fail")
    return {
        "name": name,
        "actual": actual,
        "limit": limit,
        "status": status,
        "passed": passed,
        "detail": detail,
    }


def _evaluate_thresholds(
    core: Mapping[str, Any],
    cases: Sequence[Mapping[str, Any]],
    fanout: Sequence[Mapping[str, Any]],
    invalidation: Sequence[Mapping[str, Any]],
    eviction_pressure: Mapping[str, Any],
    unrelated_commit: Mapping[str, Any],
    ttl: Mapping[str, Any],
    disk_free_bytes: int,
) -> list[dict[str, Any]]:
    thresholds: list[dict[str, Any]] = []
    planning_overhead = _core_metric(core, "disabled_planning_overhead_percent")
    thresholds.append(
        _threshold(
            "disabled_planning_overhead_percent",
            actual=planning_overhead,
            limit="<5",
            passed=None if planning_overhead is None else planning_overhead < 5.0,
            detail="Core fragment owns no-hook versus disabled-hook microplanning comparison.",
        )
    )
    execution_overhead = _core_metric(core, "disabled_execution_overhead_percent")
    thresholds.append(
        _threshold(
            "disabled_execution_overhead_percent",
            actual=execution_overhead,
            limit="<1 for source execution >=1s",
            passed=None if execution_overhead is None else execution_overhead < 1.0,
            detail="Core/SQL baseline comparison; only applicable to one-second scans.",
        )
    )
    compact = next(case for case in cases if case["key"] == "narrow_fixed_rows")
    producer_delta = float(compact["producer"]["baseline_delta_percent"])
    thresholds.append(
        _threshold(
            "compact_result_producer_overhead_percent",
            actual=producer_delta,
            limit="<10",
            passed=producer_delta < 10.0,
            detail="Same-mode first producer versus disabled reference wall time.",
        )
    )
    source_ns = int(compact["disabled"]["p50_ns"])
    hit_speedup = float(compact["completed_hit"]["speedup_vs_disabled"] or 0.0)
    thresholds.append(
        _threshold(
            "completed_hit_speedup",
            actual=hit_speedup,
            limit=">=5 when source exceeds 1s",
            passed=None if source_ns < 1_000_000_000 else hit_speedup >= 5.0,
            detail="Threshold is conditional on source execution duration.",
        )
    )
    child_counts_measured = all(
        item["child_execution_count"]["status"] == "measured"
        for item in fanout
    )
    fanout_passed = (
        all(item["child_execution_count"]["value"] == 1 for item in fanout)
        if child_counts_measured
        else None
    )
    thresholds.append(
        _threshold(
            "one_child_execution_under_fanout",
            actual={
                item["clients"]: item["child_execution_count"] for item in fanout
            },
            limit="exactly 1 for 2/8/32 clients",
            passed=fanout_passed,
            detail=(
                "Successful publication counts are not treated as direct child "
                "execution measurements. The threshold becomes applicable only "
                "when the runtime exposes an executor counter."
            ),
        )
    )
    correctness_passed = all(bool(case["correctness"]["passed"]) for case in cases)
    correctness_passed = correctness_passed and all(
        bool(item["correctness"]["passed"]) for item in fanout
    )
    correctness_passed = correctness_passed and all(
        bool(item["correctness"]["passed"]) for item in invalidation
    )
    correctness_passed = correctness_passed and bool(
        eviction_pressure["correctness"]["passed"]
    )
    correctness_passed = correctness_passed and bool(
        unrelated_commit["correctness"]["passed"]
    )
    thresholds.append(
        _threshold(
            "row_and_checksum_correctness",
            actual=correctness_passed,
            limit="no mismatch",
            passed=correctness_passed,
            detail="Ordered or multiset SHA-256 and row count versus disabled execution.",
        )
    )
    exact_behavior = {
        case["key"]: case["cache_behavior"] for case in cases
    }
    thresholds.append(
        _threshold(
            "manifest_exact_hit_behavior",
            actual=exact_behavior,
            limit="one publication and completed manifest hits for every SQL case",
            passed=all(
                bool(case["cache_behavior"]["passed"]) for case in cases
            ),
            detail=(
                "Direct executions and structured EXPLAIN must both observe "
                "completed exact hits rather than silent child passthrough."
            ),
        )
    )
    thresholds.append(
        _threshold(
            "insert_update_delete_invalidation",
            actual={
                item["key"]: item["invalidation_observed"]
                for item in invalidation
            },
            limit="every write forces a miss/new production",
            passed=all(bool(item["invalidation_observed"]) for item in invalidation),
            detail="Each post-write result is also checksum-checked against disabled mode.",
        )
    )
    thresholds.append(
        _threshold(
            "eviction_under_pressure",
            actual={
                "requests": eviction_pressure["requests_until_observation"],
                "eviction_observed": eviction_pressure["eviction_observed"],
                "trigger_advanced": eviction_pressure["trigger_query"][
                    "eviction_counter_advanced"
                ],
            },
            limit="bounded entries and a later request advances evictions",
            passed=(
                bool(eviction_pressure["eviction_observed"])
                and bool(
                    eviction_pressure["trigger_query"][
                        "eviction_counter_advanced"
                    ]
                )
            ),
            detail=(
                "Unique tiny exact results are inserted in bounded batches until "
                "pressure is observed."
            ),
        )
    )
    thresholds.append(
        _threshold(
            "conservative_unrelated_commit_rejection",
            actual=unrelated_commit["conservative_rejection_observed"],
            limit="post-commit execution is a miss, not a completed hit",
            passed=bool(unrelated_commit["conservative_rejection_observed"]),
            detail=(
                "An unadapted generic dependency must conservatively reject a "
                "completed entry after an unrelated committed write."
            ),
        )
    )
    thresholds.append(
        _threshold(
            "ttl_cleanup_bounded",
            actual=ttl["entries_remaining"],
            limit=0,
            passed=int(ttl["entries_remaining"]) == 0,
            detail=(
                "The test API requests cleanup and the benchmark waits for bounded "
                "entry disappearance; worker-side duration is not inferred."
            ),
        )
    )
    minimum_free = int(os.environ.get("BIOSEQ_CACHE_BENCH_MIN_FREE_BYTES", 1 << 30))
    thresholds.append(
        _threshold(
            "disk_free_space_floor",
            actual=disk_free_bytes,
            limit=f">={minimum_free}",
            passed=disk_free_bytes >= minimum_free,
            detail="Free space at report creation; cache internal policy remains authoritative.",
        )
    )
    compact_storage = compact["completed_hit"]["cache_storage"]
    toast = next(case for case in cases if case["key"] == "large_toasted_values")
    toast_storage = toast["completed_hit"]["cache_storage"]
    tier_samples = [
        case["completed_hit"]["cache_storage"] for case in cases
    ]
    tiers_measured = all(
        sample["status"] == "measured" for sample in tier_samples
    )
    tiers_bounded = tiers_measured and all(
        sample["reserved_hot_bytes"] <= sample["hot_ceiling_bytes"]
        and sample["reserved_file_bytes"] <= sample["file_ceiling_bytes"]
        for sample in tier_samples
    )
    tier_cases_observed = (
        tiers_measured
        and compact_storage["peak_reserved_hot_bytes"] > 0
        and toast_storage["peak_reserved_file_bytes"] > 0
    )
    thresholds.append(
        _threshold(
            "internal_storage_tier_metrics",
            actual={
                "compact": compact_storage,
                "large_toasted": toast_storage,
            },
            limit=(
                "all reservations within ceilings; compact observes DSA and "
                "large TOAST observes file tier"
            ),
            passed=tiers_bounded and tier_cases_observed,
            detail=(
                "Values come from structured EXPLAIN's production storage "
                "runtime snapshot after mandatory hit prevalidation."
            ),
        )
    )
    decode_bandwidth = _core_metric(core, "fixed_decode_bytes_per_second")
    memcpy_bandwidth = _core_metric(core, "memory_copy_bytes_per_second")
    decode_passed = None
    if decode_bandwidth is not None and memcpy_bandwidth is not None:
        decode_passed = decode_bandwidth >= memcpy_bandwidth
    thresholds.append(
        _threshold(
            "fixed_decode_memory_copy_order_bandwidth",
            actual={"decode": decode_bandwidth, "memory_copy": memcpy_bandwidth},
            limit="decode >= memory-copy reference",
            passed=decode_passed,
            detail="Measured by the standalone cache core benchmark.",
        )
    )
    thresholds.append(
        _threshold(
            "provider_partition_fragment_reuse",
            actual=None,
            limit="unaffected provider partition remains reusable",
            passed=None,
            detail="Provider-only threshold belongs to cache-bioseq-bench.",
        )
    )
    thresholds.append(
        _threshold(
            "waiter_cancel_producer_latency",
            actual=None,
            limit="no material producer latency change",
            passed=None,
            detail="Correctness is covered by isolation/TAP; provider-scale latency belongs to the provider report.",
        )
    )
    return thresholds


def _write_report(report_dir: Path, report: Mapping[str, Any]) -> Path:
    report_dir.mkdir(parents=True, exist_ok=True)
    pattern = re.compile(r"report_build_(\d+)\.json$")
    indices = [
        int(match.group(1))
        for path in report_dir.iterdir()
        if (match := pattern.match(path.name)) is not None
    ]
    index = max(indices, default=0) + 1
    destination = report_dir / f"report_build_{index}.json"
    temporary = report_dir / f".{destination.name}.{os.getpid()}.tmp"
    with temporary.open("w", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2, sort_keys=True)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, destination)
    return destination


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--provider", action="store_true")
    parser.add_argument("--cases", default=str(SCRIPT_DIR / "sql_cases.json"))
    return parser.parse_args(argv)


def run(argv: Sequence[str]) -> int:
    args = _parse_args(argv)
    if args.provider:
        raise BenchmarkError(
            "generic runner contains no Bioseq/mmsearch cases; provider benchmarks "
            "must be supplied by the provider integration target"
        )
    cache_dso = _required_path("BIOSEQ_CACHE_DSO")
    cache_bootstrap = _required_path("BIOSEQ_CACHE_BOOTSTRAP_SQL")
    core_executable = _required_path("BIOSEQ_CACHE_CORE_BENCH")
    report_value = os.environ.get("BIOSEQ_CACHE_REPORT_DIR")
    if not report_value:
        raise BenchmarkError("BIOSEQ_CACHE_REPORT_DIR is required")
    report_dir = Path(report_value).expanduser().resolve()
    report_dir.mkdir(parents=True, exist_ok=True)
    bindir = _postgresql_bindir()
    cases_document = _read_cases(Path(args.cases).expanduser().resolve())

    run_output_root = report_dir / "runs"
    run_output_root.mkdir(parents=True, exist_ok=True)
    inputs = PRIVATE.ClusterInputs(
        bindir=bindir,
        cache_dso=cache_dso,
        cache_bootstrap=cache_bootstrap,
        config_template=CACHE_ROOT / "test" / "postgresql.conf.in",
        output_root=run_output_root,
        kind="bench",
    )
    cluster = PRIVATE.UniquePrivateCluster(inputs)
    with cluster:
        core_fragment_path = cluster.run_dir / "core_fragment.json"
        core = _run_core_benchmark(
            core_executable,
            core_fragment_path,
            cluster.run_dir / "core_benchmark.log",
        )
        fixture_rows = int(cases_document.get("fixture_rows", 20_000))
        sample_count = int(cases_document.get("samples_per_case", 7))
        _setup_fixture(cluster, fixture_rows)
        cases = [
            _measure_case(cluster, case, sample_count=sample_count)
            for case in cases_document["cases"]
        ]
        fanout_sql = str(cases_document["cases"][0]["sql"])
        fanout = [
            _measure_fanout(cluster, clients, fanout_sql)
            for clients in (2, 8, 32)
        ]
        invalidation = _measure_invalidation(cluster)
        eviction_pressure = _measure_eviction_pressure(cluster)
        unrelated_commit = _measure_unrelated_commit(cluster, fanout_sql)
        ttl = _measure_ttl_and_eviction(cluster, fanout_sql)
        disk = shutil.disk_usage(cluster.run_dir)
        thresholds = _evaluate_thresholds(
            core,
            cases,
            fanout,
            invalidation,
            eviction_pressure,
            unrelated_commit,
            ttl,
            disk.free,
        )
        violations = [item for item in thresholds if item["status"] == "fail"]
        report = {
            "schema_version": 1,
            "benchmark": "bioseq_cache",
            "timestamp_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
            "git_commit": _git_commit(),
            "preset": os.environ.get("CMAKE_PRESET_NAME")
            or os.environ.get("BIOSEQ_BUILD_PRESET")
            or "unknown",
            "compiler": core.get("compiler") or os.environ.get("CXX") or "unknown",
            "private_cluster": True,
            "build_tree_dso": str(cache_dso),
            "core": core,
            "cases": cases,
            "fanout": fanout,
            "invalidation": invalidation,
            "eviction_pressure": eviction_pressure,
            "unrelated_commit": unrelated_commit,
            "ttl_eviction": ttl,
            "storage": {
                "filesystem_total_bytes": disk.total,
                "filesystem_used_bytes": disk.used,
                "filesystem_free_bytes": disk.free,
                "runtime_admin": cases[0]["runtime_storage"],
            },
            "thresholds": thresholds,
            "violations": violations,
            "passed": not violations,
        }
        destination = _write_report(report_dir, report)
        (cluster.run_dir / "report_path.txt").write_text(
            str(destination) + "\n", encoding="utf-8"
        )
    print(destination)
    return 0 if not violations else 2


def main() -> None:
    try:
        raise SystemExit(run(sys.argv[1:]))
    except (BenchmarkError, OSError, json.JSONDecodeError) as error:
        print(f"bioseq_cache benchmark failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error


if __name__ == "__main__":
    main()
