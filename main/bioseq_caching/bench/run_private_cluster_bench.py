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


def _digest_output(stdout: str, elapsed_ns: int) -> QueryOutput:
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
        rows=len(rows),
        ordered_sha256=ordered.hexdigest(),
        multiset_sha256=multiset.hexdigest(),
        stdout_bytes=len(stdout.encode("utf-8")),
    )


def _execute_query(cluster: Any, *, mode: str, sql: str) -> QueryOutput:
    stdout, elapsed = _run_sql(
        cluster,
        f"SET bioseq.query_cache_optimisation_mode = '{mode}';\n{sql}",
    )
    return _digest_output(stdout, elapsed)


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


def _measure_explain(cluster: Any, *, mode: str, sql: str) -> dict[str, float]:
    stdout, _ = _run_sql(
        cluster,
        f"SET bioseq.query_cache_optimisation_mode = '{mode}'; "
        f"EXPLAIN (ANALYZE, FORMAT JSON, COSTS OFF, TIMING OFF) {sql}",
    )
    document = json.loads(stdout)
    root = document[0]
    return {
        "planning_ms": float(root.get("Planning Time", 0.0)),
        "execution_ms": float(root.get("Execution Time", 0.0)),
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
    disabled_explain = _measure_explain(cluster, mode="disabled", sql=sql)

    _reset_case(cluster)
    producer = _execute_query(cluster, mode="same", sql=sql)
    hit_samples = [
        _execute_query(cluster, mode="same", sql=sql)
        for _ in range(sample_count)
    ]
    same_explain = _measure_explain(cluster, mode="same", sql=sql)
    case_stats = _stats(cluster)
    correctness = _same_result(reference, producer, ordered) and all(
        _same_result(reference, sample, ordered) for sample in hit_samples
    )
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
        "disabled": {
            **_sample_summary([reference.elapsed_ns]),
            **disabled_explain,
        },
        "producer": {
            **_sample_summary([producer.elapsed_ns]),
            "baseline_delta_percent": producer_delta,
        },
        "completed_hit": {
            **_sample_summary([sample.elapsed_ns for sample in hit_samples]),
            "speedup_vs_disabled": speedup,
            **same_explain,
        },
        "stats": case_stats,
    }


def _wait_for_waiters(cluster: Any, count: int, timeout_seconds: float = 30.0) -> None:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        stdout, _ = _run_sql(
            cluster,
            "SELECT COALESCE(max(waiter_count), 0) "
            "FROM bioseq_cache_test.entry_states()",
        )
        if int(stdout.strip()) >= count:
            return
        time.sleep(0.02)
    raise BenchmarkError(f"waiter count did not reach {count}")


def _measure_fanout(cluster: Any, clients: int, sql: str) -> dict[str, Any]:
    _reset_case(cluster)
    reference = _execute_query(cluster, mode="disabled", sql=sql)
    _reset_case(cluster)
    _run_sql(cluster, "SELECT bioseq_cache_test.arm_pause('after_building_publish')")

    def execute() -> QueryOutput:
        return _execute_query(cluster, mode="same", sql=sql)

    with concurrent.futures.ThreadPoolExecutor(max_workers=clients) as executor:
        producer = executor.submit(execute)
        _run_sql(
            cluster,
            "SELECT bioseq_cache_test.wait_until_paused('after_building_publish')",
        )
        waiters = [executor.submit(execute) for _ in range(clients - 1)]
        _wait_for_waiters(cluster, clients - 1)
        released_at = time.perf_counter_ns()
        _run_sql(cluster, "SELECT bioseq_cache_test.release_pause('after_building_publish')")
        outputs = [producer.result(), *(waiter.result() for waiter in waiters)]
        completed_at = time.perf_counter_ns()

    fanout_stats = _stats(cluster)
    correct = all(_same_result(reference, output, False) for output in outputs)
    return {
        "key": f"inflight_fanout_{clients}",
        "clients": clients,
        "correctness": {
            "passed": correct,
            "row_count": reference.rows,
            "multiset_sha256": reference.multiset_sha256,
        },
        **_sample_summary([output.elapsed_ns for output in outputs]),
        "release_to_all_complete_ns": completed_at - released_at,
        "stats": fanout_stats,
        "exactly_one_child_execution": fanout_stats["produced"] == 1,
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
        _execute_query(cluster, mode="same", sql=query)
        started = time.perf_counter_ns()
        _run_sql(cluster, operation)
        invalidation_ns = time.perf_counter_ns() - started
        output = _execute_query(cluster, mode="same", sql=query)
        results.append(
            {
                "key": f"{key}_invalidation",
                "operation_ns": invalidation_ns,
                "post_write_rows": output.rows,
                "stats": _stats(cluster),
            }
        )
    return results


def _measure_ttl_and_eviction(cluster: Any, sql: str) -> dict[str, Any]:
    _reset_case(cluster, ttl_ms=1)
    _execute_query(cluster, mode="same", sql=sql)
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
    fanout_passed = all(bool(item["exactly_one_child_execution"]) for item in fanout)
    thresholds.append(
        _threshold(
            "one_child_execution_under_fanout",
            actual={item["clients"]: item["stats"]["produced"] for item in fanout},
            limit="exactly 1 for 2/8/32 clients",
            passed=fanout_passed,
            detail="Deterministic BUILDING pause guarantees all other clients register as waiters.",
        )
    )
    correctness_passed = all(bool(case["correctness"]["passed"]) for case in cases)
    correctness_passed = correctness_passed and all(
        bool(item["correctness"]["passed"]) for item in fanout
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
    thresholds.append(
        _threshold(
            "ttl_cleanup_bounded",
            actual=ttl["entries_remaining"],
            limit=0,
            passed=int(ttl["entries_remaining"]) == 0,
            detail="Actual janitor sweep after TTL expiry.",
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
        ttl = _measure_ttl_and_eviction(cluster, fanout_sql)
        disk = shutil.disk_usage(cluster.run_dir)
        thresholds = _evaluate_thresholds(core, cases, fanout, ttl, disk.free)
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
            "ttl_eviction": ttl,
            "storage": {
                "filesystem_total_bytes": disk.total,
                "filesystem_used_bytes": disk.used,
                "filesystem_free_bytes": disk.free,
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
