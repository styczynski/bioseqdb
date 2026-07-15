#!/usr/bin/env python3
"""Run core and SQL bioseq_cache benchmarks in a disposable PostgreSQL node."""

from __future__ import annotations

import argparse
import concurrent.futures
import contextlib
import ctypes
import ctypes.util
import dataclasses
import datetime as dt
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import select
import shutil
import subprocess
import sys
import threading
import time
from typing import Any, Mapping, Sequence


SCRIPT_DIR = Path(__file__).resolve().parent
CACHE_ROOT = SCRIPT_DIR.parent
REPOSITORY_ROOT = CACHE_ROOT.parents[1]
PRIVATE_RUNNER_PATH = CACHE_ROOT / "test" / "run_private_cluster.py"

GENERIC_SQL_CASE_KEYS = frozenset(
    {
        "narrow_fixed_rows",
        "wide_nullable_rows",
        "large_toasted_values",
        "empty_result",
        "highly_selective_result",
        "ordered_result",
        "limit_exact_result",
    }
)
ORDERED_SQL_CASE_KEYS = frozenset({"ordered_result", "limit_exact_result"})
CORE_BENCHMARK_NAMES = frozenset(
    {
        "column_encode",
        "column_full_prevalidation",
        "column_fixed_decode",
        "admission_reserve_release",
        "memory_copy_reference",
    }
)
SECTION45_THRESHOLD_NAMES = frozenset(
    {
        "disabled_planning_overhead_percent",
        "disabled_execution_overhead_percent",
        "disabled_no_cache_plan_and_correct_result",
        "compact_result_producer_overhead_percent",
        "completed_hit_speedup",
        "one_child_execution_under_fanout",
        "fanout_request_locking_behavior",
        "fanout_wait_duration_measured",
        "row_and_checksum_correctness",
        "manifest_exact_hit_behavior",
        "insert_update_delete_invalidation",
        "eviction_under_pressure",
        "conservative_unrelated_commit_rejection",
        "ttl_cleanup_bounded",
        "internal_disk_ceiling_and_free_space_floor",
        "internal_storage_tier_metrics",
        "cold_and_warm_file_hit_with_checksum_prepass",
        "fixed_decode_memory_copy_order_bandwidth",
        "generic_reusable_current_hit",
        "generic_overlapping_predicate_reuse",
        "provider_partition_fragment_reuse",
        "waiter_cancel_producer_latency",
        "backend_postmaster_rss_and_physical_io_measured",
        "repeated_eviction_resource_reclamation",
    }
)


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


class _LibpqApi:
    """Minimal libpq binding for protocol-level single-row measurements."""

    CONNECTION_OK = 0
    PGRES_COMMAND_OK = 1
    PGRES_TUPLES_OK = 2
    PGRES_SINGLE_TUPLE = 9

    def __init__(self) -> None:
        library_name = ctypes.util.find_library("pq")
        if library_name is None:
            raise BenchmarkError(
                "libpq is required for trustworthy first-row measurements"
            )
        library = ctypes.CDLL(library_name)
        library.PQconnectdbParams.argtypes = [
            ctypes.POINTER(ctypes.c_char_p),
            ctypes.POINTER(ctypes.c_char_p),
            ctypes.c_int,
        ]
        library.PQconnectdbParams.restype = ctypes.c_void_p
        library.PQstatus.argtypes = [ctypes.c_void_p]
        library.PQstatus.restype = ctypes.c_int
        library.PQerrorMessage.argtypes = [ctypes.c_void_p]
        library.PQerrorMessage.restype = ctypes.c_char_p
        library.PQfinish.argtypes = [ctypes.c_void_p]
        library.PQfinish.restype = None
        library.PQexec.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        library.PQexec.restype = ctypes.c_void_p
        library.PQsendQuery.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        library.PQsendQuery.restype = ctypes.c_int
        library.PQsetSingleRowMode.argtypes = [ctypes.c_void_p]
        library.PQsetSingleRowMode.restype = ctypes.c_int
        library.PQgetResult.argtypes = [ctypes.c_void_p]
        library.PQgetResult.restype = ctypes.c_void_p
        library.PQresultStatus.argtypes = [ctypes.c_void_p]
        library.PQresultStatus.restype = ctypes.c_int
        library.PQresultErrorMessage.argtypes = [ctypes.c_void_p]
        library.PQresultErrorMessage.restype = ctypes.c_char_p
        library.PQntuples.argtypes = [ctypes.c_void_p]
        library.PQntuples.restype = ctypes.c_int
        library.PQnfields.argtypes = [ctypes.c_void_p]
        library.PQnfields.restype = ctypes.c_int
        library.PQgetisnull.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_int,
        ]
        library.PQgetisnull.restype = ctypes.c_int
        library.PQgetvalue.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_int,
        ]
        library.PQgetvalue.restype = ctypes.c_void_p
        library.PQgetlength.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_int,
        ]
        library.PQgetlength.restype = ctypes.c_int
        library.PQclear.argtypes = [ctypes.c_void_p]
        library.PQclear.restype = None
        library.PQlibVersion.argtypes = []
        library.PQlibVersion.restype = ctypes.c_int
        self.library = library
        self.library_name = library_name


_LIBPQ_API: _LibpqApi | None = None
_LIBPQ_API_LOCK = threading.Lock()


def _libpq_api() -> _LibpqApi:
    global _LIBPQ_API
    with _LIBPQ_API_LOCK:
        if _LIBPQ_API is None:
            _LIBPQ_API = _LibpqApi()
        return _LIBPQ_API


class _LibpqConnection:
    """One private-node libpq connection with checked result ownership."""

    def __init__(self, cluster: Any) -> None:
        self._api = _libpq_api()
        self._connection: int | None = None
        parameters = (
            ("host", str(cluster.socket_dir)),
            ("port", str(cluster.port)),
            ("user", str(cluster.database_user)),
            ("dbname", str(cluster.database)),
            ("client_encoding", "UTF8"),
            ("application_name", "bioseq_cache_benchmark"),
            ("connect_timeout", "10"),
        )
        keys = (ctypes.c_char_p * (len(parameters) + 1))()
        values = (ctypes.c_char_p * (len(parameters) + 1))()
        for index, (name, value) in enumerate(parameters):
            keys[index] = name.encode("utf-8")
            values[index] = value.encode("utf-8")
        connection = self._api.library.PQconnectdbParams(keys, values, 0)
        if not connection:
            raise BenchmarkError("libpq could not allocate a connection")
        self._connection = int(connection)
        if self._api.library.PQstatus(connection) != self._api.CONNECTION_OK:
            message = self.error_message()
            self.close()
            raise BenchmarkError(
                f"libpq private-node connection failed: {message}"
            )

    @property
    def connection(self) -> int:
        if self._connection is None:
            raise BenchmarkError("libpq connection is closed")
        return self._connection

    def error_message(self) -> str:
        if self._connection is None:
            return "connection is closed"
        message = self._api.library.PQerrorMessage(self._connection)
        return (
            message.decode("utf-8", errors="replace").strip()
            if message else "unknown libpq error"
        )

    def close(self) -> None:
        if self._connection is not None:
            self._api.library.PQfinish(self._connection)
            self._connection = None

    def __enter__(self) -> "_LibpqConnection":
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        self.close()

    def _result_error(self, result: int) -> str:
        message = self._api.library.PQresultErrorMessage(result)
        return (
            message.decode("utf-8", errors="replace").strip()
            if message else self.error_message()
        )

    def _append_result_rows(self, result: int, output: bytearray) -> int:
        rows = self._api.library.PQntuples(result)
        fields = self._api.library.PQnfields(result)
        for row in range(rows):
            for field in range(fields):
                if field != 0:
                    output.extend(b"\t")
                if self._api.library.PQgetisnull(result, row, field):
                    output.extend(b"\\N")
                    continue
                length = self._api.library.PQgetlength(result, row, field)
                pointer = self._api.library.PQgetvalue(result, row, field)
                if length < 0 or (length > 0 and not pointer):
                    raise BenchmarkError(
                        "libpq returned an invalid text field"
                    )
                if length > 0:
                    output.extend(ctypes.string_at(pointer, length))
            output.extend(b"\n")
        return rows

    def execute(self, sql: str) -> str:
        """Execute setup/admin SQL and return its final text result."""
        result = self._api.library.PQexec(
            self.connection, sql.encode("utf-8")
        )
        if not result:
            raise BenchmarkError(
                f"libpq command failed without a result: {self.error_message()}"
            )
        try:
            status = self._api.library.PQresultStatus(result)
            if status not in (
                self._api.PGRES_COMMAND_OK,
                self._api.PGRES_TUPLES_OK,
            ):
                raise BenchmarkError(
                    f"libpq command failed: {self._result_error(result)}\n"
                    f"SQL:\n{sql}"
                )
            output = bytearray()
            self._append_result_rows(result, output)
            return output.decode("utf-8")
        finally:
            self._api.library.PQclear(result)

    def execute_single_row(
        self, sql: str
    ) -> tuple[str, int | None, int]:
        """Execute one SELECT via PQsetSingleRowMode without row buffering."""
        if self._api.library.PQsendQuery(
            self.connection, sql.encode("utf-8")
        ) != 1:
            raise BenchmarkError(
                f"libpq send failed: {self.error_message()}\nSQL:\n{sql}"
            )
        if self._api.library.PQsetSingleRowMode(self.connection) != 1:
            raise BenchmarkError(
                "libpq single-row mode is unavailable for the benchmark query"
            )
        output = bytearray()
        first_row_at_ns: int | None = None
        terminal_result_seen = False
        while True:
            result = self._api.library.PQgetResult(self.connection)
            if not result:
                break
            try:
                status = self._api.library.PQresultStatus(result)
                if status == self._api.PGRES_SINGLE_TUPLE:
                    rows = self._append_result_rows(result, output)
                    if rows != 1:
                        raise BenchmarkError(
                            "libpq single-row result did not contain one row"
                        )
                    if first_row_at_ns is None:
                        first_row_at_ns = time.perf_counter_ns()
                elif status == self._api.PGRES_TUPLES_OK:
                    if self._append_result_rows(result, output) != 0:
                        raise BenchmarkError(
                            "libpq terminal result unexpectedly contained rows"
                        )
                    terminal_result_seen = True
                else:
                    raise BenchmarkError(
                        f"libpq query failed: {self._result_error(result)}\n"
                        f"SQL:\n{sql}"
                    )
            finally:
                self._api.library.PQclear(result)
        completed_at_ns = time.perf_counter_ns()
        if not terminal_result_seen:
            raise BenchmarkError(
                "libpq single-row query exposed no terminal tuple result"
            )
        return output.decode("utf-8"), first_row_at_ns, completed_at_ns


@dataclasses.dataclass(frozen=True)
class QueryOutput:
    elapsed_ns: int
    first_row_ns: int | None
    completed_at_ns: int
    rows: int
    ordered_sha256: str
    multiset_sha256: str
    stdout_bytes: int


@dataclasses.dataclass(frozen=True)
class _ProcessSample:
    rss_bytes: int
    read_characters: int | None
    write_characters: int | None
    read_bytes: int | None
    write_bytes: int | None


def _postmaster_pid(cluster: Any) -> int:
    path = cluster.data_dir / "postmaster.pid"
    try:
        pid = int(path.read_text(encoding="utf-8").splitlines()[0])
    except (OSError, ValueError, IndexError) as error:
        raise BenchmarkError(f"cannot read private postmaster PID from {path}") from error
    if pid <= 0:
        raise BenchmarkError(f"invalid private postmaster PID: {pid}")
    return pid


def _process_children(pid: int) -> set[int]:
    pending = [pid]
    descendants: set[int] = set()
    while pending:
        parent = pending.pop()
        path = Path(f"/proc/{parent}/task/{parent}/children")
        try:
            children = [int(value) for value in path.read_text().split()]
        except (OSError, ValueError):
            continue
        for child in children:
            if child > 0 and child not in descendants:
                descendants.add(child)
                pending.append(child)
    return descendants


def _read_process_sample(pid: int) -> _ProcessSample | None:
    status_path = Path(f"/proc/{pid}/status")
    try:
        status = status_path.read_text(encoding="utf-8")
    except OSError:
        return None
    rss_bytes: int | None = None
    for line in status.splitlines():
        if line.startswith("VmRSS:"):
            fields = line.split()
            if len(fields) >= 2:
                with contextlib.suppress(ValueError):
                    rss_bytes = int(fields[1]) * 1024
            break
    if rss_bytes is None:
        return None

    read_characters: int | None = None
    write_characters: int | None = None
    read_bytes: int | None = None
    write_bytes: int | None = None
    try:
        io_document = Path(f"/proc/{pid}/io").read_text(encoding="utf-8")
    except OSError:
        io_document = ""
    for line in io_document.splitlines():
        name, separator, value = line.partition(":")
        if separator == "":
            continue
        if name == "rchar":
            with contextlib.suppress(ValueError):
                read_characters = int(value.strip())
        elif name == "wchar":
            with contextlib.suppress(ValueError):
                write_characters = int(value.strip())
        elif name == "read_bytes":
            with contextlib.suppress(ValueError):
                read_bytes = int(value.strip())
        elif name == "write_bytes":
            with contextlib.suppress(ValueError):
                write_bytes = int(value.strip())
    return _ProcessSample(
        rss_bytes=rss_bytes,
        read_characters=read_characters,
        write_characters=write_characters,
        read_bytes=read_bytes,
        write_bytes=write_bytes,
    )


def _process_resource_snapshot(cluster: Any) -> dict[str, Any]:
    if not Path("/proc/self/status").is_file():
        return _unavailable_metric("Linux /proc process accounting is unavailable.")
    postmaster = _postmaster_pid(cluster)
    parent = _read_process_sample(postmaster)
    if parent is None:
        return _unavailable_metric("The private postmaster disappeared during sampling.")
    backends = [
        sample
        for pid in _process_children(postmaster)
        if (sample := _read_process_sample(pid)) is not None
    ]
    return {
        "status": "measured",
        "postmaster_rss_bytes": parent.rss_bytes,
        "maximum_backend_rss_bytes": max(
            (sample.rss_bytes for sample in backends), default=0
        ),
        "process_tree_rss_bytes": parent.rss_bytes
        + sum(sample.rss_bytes for sample in backends),
        "observed_backend_processes": len(backends),
    }


class _ProcessResourceSampler:
    """Sample private PostgreSQL RSS and kernel-accounted I/O by process."""

    def __init__(self, cluster: Any, interval_seconds: float = 0.005) -> None:
        self._cluster = cluster
        self._interval_seconds = interval_seconds
        self._postmaster = 0
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._sample_count = 0
        self._postmaster_peak_rss = 0
        self._backend_peak_rss = 0
        self._tree_peak_rss = 0
        self._baseline_io: dict[int, tuple[int, int, int, int]] = {}
        self._maximum_io: dict[int, tuple[int, int, int, int]] = {}
        self._io_available = False
        self._available = Path("/proc/self/status").is_file()

    def _sample(self, *, initial: bool = False) -> None:
        if not self._available:
            return
        pids = {self._postmaster, *_process_children(self._postmaster)}
        samples: dict[int, _ProcessSample] = {}
        for pid in pids:
            sample = _read_process_sample(pid)
            if sample is not None:
                samples[pid] = sample
        parent = samples.get(self._postmaster)
        if parent is None:
            return
        backend_rss = [
            sample.rss_bytes
            for pid, sample in samples.items()
            if pid != self._postmaster
        ]
        self._sample_count += 1
        self._postmaster_peak_rss = max(
            self._postmaster_peak_rss, parent.rss_bytes
        )
        self._backend_peak_rss = max(
            self._backend_peak_rss, max(backend_rss, default=0)
        )
        self._tree_peak_rss = max(
            self._tree_peak_rss,
            sum(sample.rss_bytes for sample in samples.values()),
        )
        for pid, sample in samples.items():
            if (
                sample.read_characters is None
                or sample.write_characters is None
                or sample.read_bytes is None
                or sample.write_bytes is None
            ):
                continue
            self._io_available = True
            if pid not in self._baseline_io:
                self._baseline_io[pid] = (
                    (
                        sample.read_characters,
                        sample.write_characters,
                        sample.read_bytes,
                        sample.write_bytes,
                    )
                    if initial
                    else (0, 0, 0, 0)
                )
            prior = self._maximum_io.get(pid, (0, 0, 0, 0))
            self._maximum_io[pid] = (
                max(prior[0], sample.read_characters),
                max(prior[1], sample.write_characters),
                max(prior[2], sample.read_bytes),
                max(prior[3], sample.write_bytes),
            )

    def _run(self) -> None:
        while not self._stop.wait(self._interval_seconds):
            self._sample()

    def __enter__(self) -> "_ProcessResourceSampler":
        if self._available:
            self._postmaster = _postmaster_pid(self._cluster)
            self._sample(initial=True)
            self._thread = threading.Thread(
                target=self._run,
                name="bioseq-cache-resource-sampler",
                daemon=True,
            )
            self._thread.start()
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2.0)
        self._sample()

    def report(self) -> dict[str, Any]:
        if not self._available or self._sample_count == 0:
            return _unavailable_metric(
                "Linux /proc RSS and per-process I/O accounting is unavailable."
            )
        io: dict[str, Any]
        if self._io_available:
            read_characters = 0
            write_characters = 0
            read_bytes = 0
            write_bytes = 0
            for pid, maximum in self._maximum_io.items():
                baseline = self._baseline_io.get(pid, (0, 0, 0, 0))
                read_characters += max(0, maximum[0] - baseline[0])
                write_characters += max(0, maximum[1] - baseline[1])
                read_bytes += max(0, maximum[2] - baseline[2])
                write_bytes += max(0, maximum[3] - baseline[3])
            io = {
                "status": "measured",
                "logical_read_bytes": read_characters,
                "logical_write_bytes": write_characters,
                "physical_read_bytes": read_bytes,
                "physical_write_bytes": write_bytes,
                "detail": (
                    "Linux /proc/<pid>/io rchar/wchar page-cache-inclusive "
                    "traffic and read_bytes/write_bytes physical traffic, "
                    "summed over the private postmaster process tree."
                ),
            }
        else:
            io = _unavailable_metric(
                "Linux per-process physical-I/O counters are unavailable."
            )
        return {
            "status": "measured",
            "samples": self._sample_count,
            "interval_ns": int(self._interval_seconds * 1_000_000_000),
            "peak_postmaster_rss_bytes": self._postmaster_peak_rss,
            "peak_backend_rss_bytes": self._backend_peak_rss,
            "peak_process_tree_rss_bytes": self._tree_peak_rss,
            "page_cache_and_physical_io": io,
        }


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
        key_value = case["key"]
        category = case["category"]
        ordered = case["ordered"]
        sql = case["sql"]
        if (
            not isinstance(key_value, str)
            or re.fullmatch(r"[a-z][a-z0-9_]*", key_value) is None
            or not isinstance(category, str)
            or re.fullmatch(r"[a-z][a-z0-9_]*", category) is None
            or type(ordered) is not bool
            or not isinstance(sql, str)
            or not sql.strip()
        ):
            raise BenchmarkError(f"malformed benchmark case: {case!r}")
        key = key_value
        if key in seen:
            raise BenchmarkError(f"duplicate benchmark key: {key}")
        seen.add(key)
        if ";" in sql:
            raise BenchmarkError(f"benchmark SQL must be one statement: {key}")
        if ordered != (key in ORDERED_SQL_CASE_KEYS):
            raise BenchmarkError(
                f"benchmark case has the wrong ordered-checksum contract: {key}"
            )
    missing = sorted(GENERIC_SQL_CASE_KEYS - seen)
    if missing:
        raise BenchmarkError(
            f"mandatory generic SQL benchmark cases are absent: {missing}"
        )
    try:
        fixture_rows = int(document.get("fixture_rows", 0))
        samples_per_case = int(document.get("samples_per_case", 0))
    except (TypeError, ValueError) as error:
        raise BenchmarkError("benchmark fixture/sample sizes are not integers") from error
    if (
        not isinstance(document.get("fixture_rows"), int)
        or isinstance(document.get("fixture_rows"), bool)
        or not isinstance(document.get("samples_per_case"), int)
        or isinstance(document.get("samples_per_case"), bool)
        or fixture_rows < 2_000
        or samples_per_case < 3
    ):
        raise BenchmarkError(
            "generic SQL benchmark requires at least 2,000 fixture rows and "
            "three samples per case"
        )
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
    return _validate_core_benchmark_fragment(fragment)


def _run_harness_unit_tests() -> dict[str, Any]:
    """Run the benchmark-only contract suite before allocating a cluster."""
    test_path = SCRIPT_DIR / "test_run_private_cluster_bench.py"
    completed = subprocess.run(
        [sys.executable, str(test_path), "-v"],
        env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        raise BenchmarkError(
            "benchmark harness unit tests failed before the timed run:\n"
            + completed.stdout
        )
    match = re.search(r"Ran (\d+) tests?", completed.stdout)
    return {
        "passed": True,
        "test_count": int(match.group(1)) if match is not None else None,
        "command": [sys.executable, str(test_path), "-v"],
        "output": completed.stdout.splitlines(),
    }


def _validate_core_benchmark_fragment(fragment: Any) -> dict[str, Any]:
    """Reject standalone-core reports without complete metric/correctness proof."""
    if not isinstance(fragment, dict):
        raise BenchmarkError("core benchmark fragment is not a JSON object")
    rows = fragment.get("rows")
    sample_count = fragment.get("samples")
    if (
        fragment.get("schema_version") != 1
        or not isinstance(rows, int)
        or isinstance(rows, bool)
        or rows <= 0
        or not isinstance(sample_count, int)
        or isinstance(sample_count, bool)
        or sample_count < 3
    ):
        raise BenchmarkError(
            "core benchmark fragment has invalid schema, row, or sample metadata"
        )
    correctness = fragment.get("correctness")
    if not isinstance(correctness, dict) or correctness.get("passed") is not True:
        raise BenchmarkError("core benchmark fragment has no passing correctness proof")
    required_correctness = {
        "row_count",
        "expected_ordered_checksum",
        "actual_ordered_checksum",
        "column_blob_digest",
        "expected_iteration_guard",
        "decode_guard",
        "memory_copy_guard",
        "admission_final_reserved_bytes",
    }
    if not required_correctness.issubset(correctness):
        raise BenchmarkError(
            "core benchmark correctness proof is missing row/checksum evidence"
        )
    checksum_names = (
        "expected_ordered_checksum",
        "actual_ordered_checksum",
        "column_blob_digest",
    )
    if (
        not isinstance(correctness["row_count"], int)
        or isinstance(correctness["row_count"], bool)
        or correctness["row_count"] != rows
        or not all(
            isinstance(correctness[name], str) and correctness[name]
            for name in checksum_names
        )
        or correctness["expected_ordered_checksum"]
        != correctness["actual_ordered_checksum"]
        or not all(
            isinstance(correctness[name], int)
            and not isinstance(correctness[name], bool)
            and 0 <= correctness[name] <= (1 << 64) - 1
            for name in (
                "expected_iteration_guard",
                "decode_guard",
                "memory_copy_guard",
            )
        )
        or correctness["decode_guard"]
            != correctness["expected_iteration_guard"]
        or correctness["memory_copy_guard"]
            != correctness["expected_iteration_guard"]
        or correctness["admission_final_reserved_bytes"] != 0
    ):
        raise BenchmarkError(
            "core benchmark correctness row count or ordered checksum is invalid"
        )
    metrics = fragment.get("metrics")
    required_metrics = {
        "encoded_logical_bytes",
        "encoded_stored_bytes",
        "row_group_count",
        "full_block_prevalidation_bytes",
        "full_block_prevalidation_p50_ns",
        "full_block_prevalidation_p95_ns",
        "full_block_prevalidation_p99_ns",
        "fixed_decode_bytes_per_second",
        "memory_copy_bytes_per_second",
    }
    if not isinstance(metrics, dict) or not required_metrics.issubset(metrics):
        raise BenchmarkError(
            "core benchmark fragment is missing mandatory storage metrics"
        )
    if any(
        not isinstance(metrics[name], (int, float))
        or isinstance(metrics[name], bool)
        or metrics[name] < 0
        for name in required_metrics
    ):
        raise BenchmarkError(
            "core benchmark fragment has nonnumeric or negative storage metrics"
        )
    if (
        metrics["encoded_logical_bytes"] <= 0
        or metrics["encoded_stored_bytes"] <= 0
        or metrics["row_group_count"] <= 0
        or metrics["full_block_prevalidation_bytes"]
        != metrics["encoded_stored_bytes"]
        or metrics["fixed_decode_bytes_per_second"] <= 0
        or metrics["memory_copy_bytes_per_second"] <= 0
    ):
        raise BenchmarkError(
            "core benchmark fragment has inconsistent storage/decode metrics"
        )
    benchmarks = fragment.get("benchmarks")
    if not isinstance(benchmarks, list) or not benchmarks:
        raise BenchmarkError("core benchmark fragment has no raw benchmark samples")
    benchmark_names = [
        benchmark.get("name")
        for benchmark in benchmarks
        if isinstance(benchmark, dict)
    ]
    if (
        not CORE_BENCHMARK_NAMES.issubset(benchmark_names)
        or len(benchmark_names) != len(set(benchmark_names))
    ):
        raise BenchmarkError(
            "core benchmark fragment has missing or duplicate mandatory series"
        )
    for benchmark in benchmarks:
        raw_samples = benchmark.get("raw_samples_ns") \
            if isinstance(benchmark, dict) else None
        if (
            not isinstance(benchmark, dict)
            or not isinstance(raw_samples, list)
            or len(raw_samples) != sample_count
            or not all(
                isinstance(sample, int)
                and not isinstance(sample, bool)
                and sample >= 0
                for sample in raw_samples
            )
            or not all(
                name in benchmark for name in ("p50_ns", "p95_ns", "p99_ns")
            )
            or not all(
                isinstance(benchmark.get(name), int)
                and not isinstance(benchmark.get(name), bool)
                and benchmark[name] >= 0
                for name in ("p50_ns", "p95_ns", "p99_ns")
            )
            or not isinstance(benchmark.get("operations"), int)
            or isinstance(benchmark.get("operations"), bool)
            or benchmark["operations"] <= 0
        ):
            raise BenchmarkError(
                f"core benchmark entry lacks raw/p50/p95/p99 proof: {benchmark!r}"
            )
        expected_percentiles = {
            "p50_ns": _nearest_rank_percentile(raw_samples, 50, 100),
            "p95_ns": _nearest_rank_percentile(raw_samples, 95, 100),
            "p99_ns": _nearest_rank_percentile(raw_samples, 99, 100),
        }
        if any(
            benchmark[name] != value
            for name, value in expected_percentiles.items()
        ):
            raise BenchmarkError(
                "core benchmark percentile does not match raw samples: "
                f"{benchmark!r}"
            )
    return fragment


def _nearest_rank_percentile(
    samples: Sequence[int], numerator: int, denominator: int
) -> int:
    """Return the integer nearest-rank percentile used by the C++ CLI."""
    if (
        not samples
        or any(
            not isinstance(sample, int)
            or isinstance(sample, bool)
            or sample < 0
            for sample in samples
        )
        or not isinstance(numerator, int)
        or isinstance(numerator, bool)
        or not isinstance(denominator, int)
        or isinstance(denominator, bool)
        or numerator <= 0
        or denominator <= 0
        or numerator > denominator
    ):
        raise BenchmarkError("nearest-rank percentile has invalid inputs")
    ordered = sorted(samples)
    rank = (numerator * len(ordered) + denominator - 1) // denominator
    return ordered[max(1, rank) - 1]


def _validate_sample_summary_contract(
    value: Any,
    label: str,
    *,
    raw_name: str,
    require_nonempty: bool,
) -> None:
    """Validate raw observations and their exact nearest-rank summaries."""
    percentile_names = (
        ("p50_ns", "p95_ns", "p99_ns")
        if raw_name == "raw_samples_ns"
        else ("p50", "p95", "p99")
    )
    if not isinstance(value, Mapping):
        raise BenchmarkError(
            f"Section 45 report lacks a sample summary for {label}"
        )
    raw_samples = value.get(raw_name)
    if (
        not isinstance(raw_samples, list)
        or not all(
            isinstance(sample, int)
            and not isinstance(sample, bool)
            and sample >= 0
            for sample in raw_samples
        )
        or (require_nonempty and not raw_samples)
    ):
        raise BenchmarkError(
            f"Section 45 report has invalid raw samples for {label}"
        )
    if not raw_samples:
        if value.get("status") != "not_applicable" or any(
            name in value for name in percentile_names
        ):
            raise BenchmarkError(
                f"Section 45 empty sample summary is not applicable: {label}"
            )
        return
    expected = {
        percentile_names[0]: _nearest_rank_percentile(
            raw_samples, 50, 100
        ),
        percentile_names[1]: _nearest_rank_percentile(
            raw_samples, 95, 100
        ),
        percentile_names[2]: _nearest_rank_percentile(
            raw_samples, 99, 100
        ),
    }
    if any(
        not isinstance(value.get(name), int)
        or isinstance(value.get(name), bool)
        or value[name] != expected[name]
        for name in percentile_names
    ):
        raise BenchmarkError(
            "Section 45 percentile does not match raw samples for "
            f"{label}"
        )


def _validate_nested_sample_summary_contracts(
    value: Any, label: str
) -> None:
    """Validate every latency and throughput sample series in a report tree."""
    if isinstance(value, Mapping):
        if "raw_samples_ns" in value:
            _validate_sample_summary_contract(
                value,
                label,
                raw_name="raw_samples_ns",
                require_nonempty=False,
            )
        if "raw_samples" in value:
            _validate_sample_summary_contract(
                value,
                label,
                raw_name="raw_samples",
                require_nonempty=False,
            )
        for name, child in value.items():
            _validate_nested_sample_summary_contracts(
                child, f"{label}.{name}"
            )
    elif isinstance(value, (list, tuple)):
        for index, child in enumerate(value):
            _validate_nested_sample_summary_contracts(
                child, f"{label}[{index}]"
            )


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


class _ProviderSession:
    """Measure backend-local provider counters beside each query execution."""

    def __init__(self, cluster: Any) -> None:
        self._cluster = cluster
        self._connection: _LibpqConnection | None = None

    @property
    def connection(self) -> _LibpqConnection:
        if self._connection is None:
            raise BenchmarkError("provider benchmark session is closed")
        return self._connection

    def _command(self, sql: str) -> list[str]:
        self.connection.execute(
            "SET bioseq.query_cache_optimisation_mode = 'disabled'"
        )
        return self.connection.execute(sql).splitlines(keepends=True)

    def __enter__(self) -> "_ProviderSession":
        connection = _LibpqConnection(self._cluster)
        self._connection = connection
        try:
            self._command("SELECT bioseq_cache_test.provider_init()")
            connection.execute("SET bioseq.query_cache_ttl = 3600000")
        except BaseException:
            connection.close()
            self._connection = None
            raise
        return self

    def use_relation(self, relation: str) -> None:
        if not re.fullmatch(r"[a-z_][a-z0-9_]*", relation):
            raise BenchmarkError(
                f"unsafe provider benchmark relation name: {relation!r}"
            )
        self._command(
            "SELECT bioseq_cache_test.provider_use_relation("
            f"'{relation}'::regclass)"
        )

    def reset_provider(self) -> None:
        self._command("SELECT bioseq_cache_test.provider_reset()")

    def provider_stats(self) -> dict[str, Any]:
        lines = [
            line.strip()
            for line in self._command(
                "SELECT bioseq_cache_test.provider_stats()::text"
            )
            if line.strip()
        ]
        if len(lines) != 1:
            raise BenchmarkError(
                f"unexpected generic provider stats output: {lines!r}"
            )
        document = json.loads(lines[0])
        if not isinstance(document, dict):
            raise BenchmarkError(
                f"generic provider stats are not an object: {document!r}"
            )
        return document

    def execute(self, *, mode: str, sql: str) -> QueryOutput:
        if mode not in {"disabled", "same", "reuse"}:
            raise BenchmarkError(f"unsupported provider benchmark mode: {mode}")
        started = time.perf_counter_ns()
        self.connection.execute(
            "SET bioseq.query_cache_optimisation_mode = "
            f"'{mode}'"
        )
        stdout, first_row_at_ns, completed_at_ns = (
            self.connection.execute_single_row(sql.strip().rstrip(";"))
        )
        first_row_ns = (
            first_row_at_ns - started
            if first_row_at_ns is not None
            else None
        )
        return _digest_output(
            stdout,
            completed_at_ns - started,
            first_row_ns,
            completed_at_ns,
        )

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        connection = self._connection
        if connection is None:
            return
        try:
            with contextlib.suppress(Exception):
                connection.execute(
                    "SET bioseq.query_cache_optimisation_mode = 'disabled'"
                )
                connection.execute(
                    "SELECT bioseq_cache_test.provider_shutdown()"
                )
        finally:
            connection.close()
            self._connection = None


def _run_query_streaming(
    cluster: Any,
    sql: str,
    *,
    setup_sql: Sequence[str] = (),
) -> tuple[str, int, int | None, int]:
    """Capture the first tuple through libpq's single-row protocol mode."""
    started = time.perf_counter_ns()
    with _LibpqConnection(cluster) as connection:
        for command in setup_sql:
            connection.execute(command)
        stdout, first_row_at_ns, completed_at_ns = (
            connection.execute_single_row(sql.strip().rstrip(";"))
        )
    return (
        stdout,
        completed_at_ns - started,
        (
            first_row_at_ns - started
            if first_row_at_ns is not None
            else None
        ),
        completed_at_ns,
    )


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


def _is_nonnegative_integer(value: Any, *, minimum: int = 0) -> bool:
    """Reject bool aliases while validating observed integer quantities."""
    return (
        isinstance(minimum, int)
        and not isinstance(minimum, bool)
        and minimum >= 0
        and isinstance(value, int)
        and not isinstance(value, bool)
        and value >= minimum
    )


def _streaming_protocol_probe_passed(
    output: QueryOutput,
    row_values: Sequence[str],
    *,
    minimum_post_first_row_ns: int,
) -> bool:
    """Require two correct rows with a material delay after the first."""
    timing_values = (
        output.elapsed_ns,
        output.first_row_ns,
        minimum_post_first_row_ns,
    )
    return (
        list(row_values) == ["1", "2"]
        and output.rows == 2
        and all(
            _is_nonnegative_integer(value)
            for value in timing_values
        )
        and output.elapsed_ns > output.first_row_ns
        and (
            output.elapsed_ns - output.first_row_ns
            >= minimum_post_first_row_ns
        )
    )


def _verify_streaming_first_row_protocol(cluster: Any) -> dict[str, Any]:
    """Prove cursor/unbuffered output precedes a deliberately delayed row."""
    delayed_seconds = 0.25
    minimum_post_first_row_ns = 150_000_000
    sql = (
        "SELECT value, pg_sleep(CASE WHEN value = 2 THEN "
        f"{delayed_seconds} ELSE 0.0 END) "
        "FROM generate_series(1, 2) AS streamed(value)"
    )
    stdout, elapsed_ns, first_row_ns, completed_at_ns = (
        _run_query_streaming(cluster, sql)
    )
    output = _digest_output(
        stdout, elapsed_ns, first_row_ns, completed_at_ns
    )
    normalized = stdout.replace("\r\n", "\n").rstrip("\n")
    row_values = [
        row.split("\t", 1)[0]
        for row in ([] if normalized == "" else normalized.split("\n"))
    ]
    passed = _streaming_protocol_probe_passed(
        output,
        row_values,
        minimum_post_first_row_ns=minimum_post_first_row_ns,
    )
    report = {
        "status": "measured" if passed else "failed",
        "passed": passed,
        "protocol": "libpq PQsetSingleRowMode",
        "libpq_library": _libpq_api().library_name,
        "libpq_version": _libpq_api().library.PQlibVersion(),
        "sql": sql,
        "delayed_later_row_ns": int(delayed_seconds * 1_000_000_000),
        "minimum_post_first_row_ns": minimum_post_first_row_ns,
        "first_row_ns": first_row_ns,
        "total_elapsed_ns": elapsed_ns,
        "post_first_row_ns": (
            elapsed_ns - first_row_ns
            if first_row_ns is not None else None
        ),
        "row_values": row_values,
    }
    if not passed:
        raise BenchmarkError(
            "unbuffered one-row cursor did not expose the first row before "
            f"the delayed second row: {report!r}"
        )
    return report


def _execute_query(
    cluster: Any,
    *,
    mode: str | None,
    sql: str,
    ttl_ms: int | None = None,
) -> QueryOutput:
    setup_sql: list[str] = []
    if mode is not None:
        setup_sql.append(
            f"SET bioseq.query_cache_optimisation_mode = '{mode}'"
        )
    if ttl_ms is not None:
        if mode is None:
            raise BenchmarkError("cache TTL requires a cache-enabled query mode")
        setup_sql.append(f"SET bioseq.query_cache_ttl = {ttl_ms}")
    stdout, elapsed, first_row_ns, completed_at_ns = _run_query_streaming(
        cluster,
        sql,
        setup_sql=setup_sql,
    )
    return _digest_output(stdout, elapsed, first_row_ns, completed_at_ns)


def _sample_summary(samples_ns: Sequence[int]) -> dict[str, Any]:
    return {
        "raw_samples_ns": list(samples_ns),
        "p50_ns": _nearest_rank_percentile(samples_ns, 50, 100),
        "p95_ns": _nearest_rank_percentile(samples_ns, 95, 100),
        "p99_ns": _nearest_rank_percentile(samples_ns, 99, 100),
    }


def _throughput_summary(outputs: Sequence[QueryOutput]) -> dict[str, Any]:
    def rate(quantity: int, elapsed_ns: int) -> int:
        return int(quantity * 1_000_000_000 / max(1, elapsed_ns))

    def summary(samples: Sequence[int], unit: str) -> dict[str, Any]:
        return {
            "raw_samples": list(samples),
            "p50": _nearest_rank_percentile(samples, 50, 100),
            "p95": _nearest_rank_percentile(samples, 95, 100),
            "p99": _nearest_rank_percentile(samples, 99, 100),
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


def _fixed_quantity_throughput(
    quantity: int,
    outputs: Sequence[QueryOutput],
    unit: str,
) -> dict[str, Any]:
    if quantity < 0 or not outputs:
        raise BenchmarkError("throughput requires a nonnegative quantity and samples")
    samples = [
        int(quantity * 1_000_000_000 / max(1, output.elapsed_ns))
        for output in outputs
    ]
    return {
        "raw_samples": samples,
        "p50": _nearest_rank_percentile(samples, 50, 100),
        "p95": _nearest_rank_percentile(samples, 95, 100),
        "p99": _nearest_rank_percentile(samples, 99, 100),
        "unit": unit,
        "quantity_per_execution": quantity,
    }


def _unavailable_metric(detail: str) -> dict[str, Any]:
    return {"status": "unavailable", "value": None, "detail": detail}


def _is_measured_integer_metric(
    metric: Any,
    *,
    minimum: int = 0,
    unit: str | None = None,
    source_field: str | None = None,
) -> bool:
    """Return whether a metric contains an observed integer quantity."""
    return (
        _is_nonnegative_integer(minimum)
        and isinstance(metric, Mapping)
        and metric.get("status") == "measured"
        and _is_nonnegative_integer(metric.get("value"), minimum=minimum)
        and (unit is None or metric.get("unit") == unit)
        and (
            source_field is None
            or metric.get("source_field") == source_field
        )
    )


def _cold_physical_io_measured(resources: Any) -> bool:
    """Require a cold file hit to cause an observed physical server read."""
    io = (
        resources.get("page_cache_and_physical_io")
        if isinstance(resources, Mapping) else None
    )
    return (
        isinstance(resources, Mapping)
        and resources.get("status") == "measured"
        and isinstance(io, Mapping)
        and io.get("status") == "measured"
        and isinstance(io.get("physical_read_bytes"), int)
        and not isinstance(io.get("physical_read_bytes"), bool)
        and int(io["physical_read_bytes"]) > 0
    )


def _latency_delta_within_tolerance(
    baseline_ns: int, candidate_ns: int, tolerance_ns: int
) -> bool:
    """Apply a two-sided material-latency bound."""
    if min(baseline_ns, candidate_ns, tolerance_ns) < 0:
        return False
    return abs(candidate_ns - baseline_ns) <= tolerance_ns


def _ttl_cleanup_contract_passed(report: Any) -> bool:
    """Require a usable COMPLETE entry before timed expiry and reclamation."""
    if not isinstance(report, Mapping):
        return False
    pre_expiry = report.get("pre_expiry")
    reclamation = report.get("reclamation")
    correctness = report.get("correctness")
    if (
        not isinstance(pre_expiry, Mapping)
        or not isinstance(reclamation, Mapping)
        or not isinstance(correctness, Mapping)
    ):
        return False
    stats = pre_expiry.get("stats")
    tier = reclamation.get("tier")
    store = reclamation.get("store")
    ttl_ms = report.get("ttl_ms")
    expiry_wait_ns = report.get("expiry_wait_ns")
    return (
        correctness.get("passed") is True
        and pre_expiry.get("passed") is True
        and _is_nonnegative_integer(
            pre_expiry.get("complete_entries"), minimum=1
        )
        and isinstance(stats, Mapping)
        and _is_nonnegative_integer(stats.get("produced"), minimum=1)
        and stats.get("produced") == 1
        and _is_nonnegative_integer(
            stats.get("completed_hits"), minimum=1
        )
        and stats.get("completed_hits") == 1
        and _is_nonnegative_integer(stats.get("fallbacks"))
        and stats.get("fallbacks") == 0
        and _is_nonnegative_integer(ttl_ms, minimum=1)
        and _is_nonnegative_integer(expiry_wait_ns)
        and expiry_wait_ns >= ttl_ms * 1_000_000
        and _is_nonnegative_integer(reclamation.get("complete_entries"))
        and reclamation.get("complete_entries") == 0
        and isinstance(tier, Mapping)
        and all(
            _is_nonnegative_integer(tier.get(name))
            and tier.get(name) == 0
            for name in (
                "dsa_blocks",
                "file_blocks",
                "reserved_hot_bytes",
                "reserved_file_bytes",
            )
        )
        and isinstance(store, Mapping)
        and _is_nonnegative_integer(store.get("file_count"))
        and store.get("file_count") == 0
        and _is_nonnegative_integer(store.get("bytes"))
        and store.get("bytes") == 0
    )


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
            "Client-observed time to the first libpq single-row tuple; it includes "
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
        if any(
            not isinstance(value, int)
            or isinstance(value, bool)
            or value < 0
            for value in values
        ):
            return _unavailable_metric(
                f"Admin field {selected!r} is not wholly nonnegative integer data."
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
        "prevalidation_bytes": aggregate(
            ("prevalidation_bytes", "full_block_prevalidation_bytes"),
            "No per-entry full-block prevalidation byte count is exposed by "
            "the generic admin API.",
        ),
        "row_group_count": aggregate(
            ("row_group_count", "row_groups"),
            "No per-entry row-group count is exposed by the generic admin API.",
        ),
        "child_executions": aggregate(
            ("child_executions", "child_execution_count"),
            "No executor child-Begin/execution count is exposed by the generic admin API.",
        ),
    }


def _exact_child_execution_snapshot(cluster: Any) -> dict[str, Any]:
    stdout, _ = _run_sql(
        cluster,
        "SELECT exact_child_executions "
        "FROM bioseq_cache_test.runtime_counters()",
    )
    value = stdout.strip()
    try:
        measured = int(value)
    except ValueError as error:
        raise BenchmarkError(
            f"unexpected exact-child execution counter: {stdout!r}"
        ) from error
    if measured < 0:
        raise BenchmarkError(
            f"negative exact-child execution counter: {measured}"
        )
    return {
        "status": "measured",
        "value": measured,
        "source": "bioseq_cache_test.runtime_counters.exact_child_executions",
    }


def _test_storage_tier_snapshot(cluster: Any) -> dict[str, Any]:
    stdout, _ = _run_sql(
        cluster,
        "SELECT force_file_tier, dsa_blocks, file_blocks, "
        "reserved_hot_bytes, reserved_file_bytes "
        "FROM bioseq_cache_test.storage_tier_stats()",
    )
    values = stdout.strip().split("\t")
    if len(values) != 5 or values[0] not in {"t", "f"}:
        raise BenchmarkError(f"unexpected storage tier test output: {stdout!r}")
    try:
        numeric = [int(value) for value in values[1:]]
    except ValueError as error:
        raise BenchmarkError(
            f"nonnumeric storage tier test output: {stdout!r}"
        ) from error
    if any(value < 0 for value in numeric):
        raise BenchmarkError(f"negative storage tier test output: {stdout!r}")
    return {
        "force_file_tier": values[0] == "t",
        "dsa_blocks": numeric[0],
        "file_blocks": numeric[1],
        "reserved_hot_bytes": numeric[2],
        "reserved_file_bytes": numeric[3],
    }


def _runtime_storage_snapshot(cluster: Any) -> dict[str, Any]:
    """Combine production diagnostics with exact test-ledger byte evidence."""
    runtime = _entry_runtime_snapshot(cluster)
    tier = _test_storage_tier_snapshot(cluster)
    runtime["test_tier_evidence"] = tier
    runtime["dsa_hot_bytes"] = {
        "status": "measured",
        "value": tier["reserved_hot_bytes"],
        "unit": "bytes",
        "source_field": "bioseq_cache_test.storage_tier_stats.reserved_hot_bytes",
        "detail": (
            "The cache-only private benchmark exposes the physical storage "
            "ledger so Section 45 never substitutes block counts for bytes."
        ),
    }
    runtime["file_bytes"] = {
        "status": "measured",
        "value": tier["reserved_file_bytes"],
        "unit": "bytes",
        "source_field": "bioseq_cache_test.storage_tier_stats.reserved_file_bytes",
        "detail": (
            "Reserved file-tier bytes are read from the test-only physical "
            "storage ledger in the disposable benchmark node."
        ),
    }
    return runtime


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


PROVIDER_COUNTER_NAMES = (
    "analyze_query",
    "describe_path",
    "domain_digest",
    "generation_digest",
    "runtime_digest",
    "map_source",
    "decorate_path",
    "build_recipe",
    "extract_occurrence",
    "late_fetch_batch",
    "late_fetch_references",
    "late_fetch_values",
    "late_fetch_bytes",
    "planner_bridge_calls",
    "executor_rows",
    "gated_expensive_rows",
)


def _counter_delta(
    before: Mapping[str, Any],
    after: Mapping[str, Any],
    names: Sequence[str],
) -> dict[str, int]:
    result: dict[str, int] = {}
    for name in names:
        before_value = before.get(name)
        after_value = after.get(name)
        if not isinstance(before_value, int) or not isinstance(after_value, int):
            raise BenchmarkError(
                f"counter {name!r} is absent or non-integral: "
                f"{before_value!r} -> {after_value!r}"
            )
        result[name] = after_value - before_value
    return result


def _storage_snapshot(cluster: Any) -> dict[str, Any]:
    runtime = _runtime_storage_snapshot(cluster)
    return {
        "runtime_admin": runtime,
        "test_tier_evidence": runtime["test_tier_evidence"],
        "dsa_bytes": runtime["dsa_hot_bytes"],
        "file_bytes": runtime["file_bytes"],
    }


def _cache_store_usage(cluster: Any) -> dict[str, int]:
    directory = cluster.data_dir / "bioseq_cache" / "store"
    sizes: list[int] = []
    if directory.is_dir():
        for path in directory.iterdir():
            with contextlib.suppress(OSError):
                if path.is_file():
                    sizes.append(path.stat().st_size)
    return {
        "file_count": len(sizes),
        "bytes": sum(sizes),
    }


def _filesystem_policy_snapshot(cluster: Any) -> dict[str, Any]:
    """Measure the v1 file-admission ceiling and free-space floor."""
    usage = shutil.disk_usage(cluster.run_dir)
    policy_ceiling = min(128 << 30, usage.total // 10)
    free_floor = max(10 << 30, usage.total // 10)
    return {
        "status": "measured",
        "filesystem_total_bytes": usage.total,
        "filesystem_used_bytes": usage.used,
        "filesystem_free_bytes": usage.free,
        "policy_file_ceiling_bytes": policy_ceiling,
        "policy_free_space_floor_bytes": free_floor,
        "free_space_floor_preserved": usage.free >= free_floor,
        "policy_source": (
            "DOC0371 FileAdmissionPolicy v1: min(128 GiB, 10% capacity) "
            "ceiling and max(10 GiB, 10% capacity) free-space floor."
        ),
    }


def _drop_cache_store_page_cache(cluster: Any) -> dict[str, Any]:
    if not hasattr(os, "posix_fadvise") or not hasattr(
        os, "POSIX_FADV_DONTNEED"
    ):
        return _unavailable_metric(
            "POSIX_FADV_DONTNEED is unavailable on this benchmark host."
        )
    directory = cluster.data_dir / "bioseq_cache" / "store"
    files = [] if not directory.is_dir() else sorted(
        path for path in directory.iterdir()
        if path.is_file() and path.name.endswith(".cache")
    )
    if not files:
        return _unavailable_metric(
            "The forced file-tier producer created no cache block files."
        )
    advised_bytes = 0
    try:
        for path in files:
            descriptor = os.open(path, os.O_RDWR | os.O_CLOEXEC)
            try:
                os.fsync(descriptor)
                os.posix_fadvise(
                    descriptor, 0, 0, os.POSIX_FADV_DONTNEED
                )
                advised_bytes += path.stat().st_size
            finally:
                os.close(descriptor)
    except OSError as error:
        return _unavailable_metric(
            f"Cannot evict forced file-tier pages with posix_fadvise: {error}"
        )
    return {
        "status": "measured",
        "file_count": len(files),
        "advised_bytes": advised_bytes,
        "advice": "POSIX_FADV_DONTNEED",
        "detail": (
            "Every complete cache block was fsynced and advised DONTNEED "
            "before the first timed completed hit."
        ),
    }


def _provider_phase_report(
    cluster: Any,
    *,
    reference: QueryOutput,
    outputs: Sequence[QueryOutput],
    ordered: bool,
    cache_before: Mapping[str, Any],
    cache_after: Mapping[str, Any],
    provider_before: Mapping[str, Any],
    provider_after: Mapping[str, Any],
    input_rows: int,
) -> dict[str, Any]:
    if not outputs:
        raise BenchmarkError("provider benchmark phase has no output samples")
    if input_rows <= 0:
        raise BenchmarkError("provider benchmark phase has no positive input size")
    digest_name = "ordered_sha256" if ordered else "multiset_sha256"
    digest_value = (
        reference.ordered_sha256 if ordered else reference.multiset_sha256
    )
    cache_delta = _counter_delta(
        cache_before, cache_after, tuple(cache_before.keys())
    )
    provider_delta = _counter_delta(
        provider_before, provider_after, PROVIDER_COUNTER_NAMES
    )
    executor_rows = provider_delta["executor_rows"]
    gated_rows = provider_delta["gated_expensive_rows"]
    late_fetch_references = provider_delta["late_fetch_references"]
    late_fetch_values = provider_delta["late_fetch_values"]
    late_fetch_byte_count = provider_delta["late_fetch_bytes"]
    if late_fetch_byte_count < 0:
        raise BenchmarkError("generic provider reported negative late-fetch bytes")
    late_fetch_bytes = {
        "status": "measured",
        "value": late_fetch_byte_count,
        "detail": (
            "Sum of datumGetSize for every successfully emitted non-null "
            "late-fetch value; null values and occurrence/status envelopes "
            "contribute zero bytes."
        ),
    }
    return {
        "correctness": {
            "passed": all(
                _same_result(reference, output, ordered)
                for output in outputs
            ),
            "row_count": reference.rows,
            "ordered_sha256": reference.ordered_sha256,
            "multiset_sha256": reference.multiset_sha256,
            "comparison_digest": digest_name,
            "comparison_sha256": digest_value,
            "stdout_bytes": reference.stdout_bytes,
        },
        "wall": _sample_summary([output.elapsed_ns for output in outputs]),
        "time_to_first_row": _first_row_summary(outputs),
        "throughput": _throughput_summary(outputs),
        "cache_stats": {
            "before": dict(cache_before),
            "after": dict(cache_after),
            "delta": cache_delta,
        },
        "provider_stats": {
            "before": dict(provider_before),
            "after": dict(provider_after),
            "delta": provider_delta,
        },
        "predicate_runtime": {
            "late_fetch_rows": {
                "status": "measured",
                "value": late_fetch_references,
            },
            "late_fetch_values": {
                "status": "measured",
                "value": late_fetch_values,
            },
            "late_fetch_bytes": late_fetch_bytes,
            "selection_ratio": {
                "status": "measured",
                "value": reference.rows / input_rows,
                "selected_rows": reference.rows,
                "input_rows": input_rows,
                "detail": (
                    "Correctness-checked output occurrences divided by the "
                    "fixture occurrences available to the provider relation."
                ),
            },
            "provider_gate_pass_ratio": (
                {
                    "status": "measured",
                    "value": gated_rows / executor_rows,
                    "gated_expensive_rows": gated_rows,
                    "executor_rows": executor_rows,
                    "detail": (
                        "The test provider increments executor_rows only after "
                        "its gate passes; this is a gate-accounting check, not "
                        "the benchmark selection ratio."
                    ),
                }
                if executor_rows > 0
                else {
                    "status": "not_applicable",
                    "value": None,
                    "detail": "This phase executed no provider rows.",
                }
            ),
        },
        "storage": {
            **_storage_snapshot(cluster),
            "filesystem_policy": _filesystem_policy_snapshot(cluster),
        },
    }


def _reset_case(cluster: Any, *, ttl_ms: int = 3_600_000) -> None:
    _run_sql(
        cluster,
        "SELECT bioseq_cache_test.set_force_file_tier(false); "
        "SET bioseq.query_cache_optimisation_mode = 'disabled'; "
        "SELECT bioseq_cache.invalidate_all(); "
        "SELECT bioseq_cache.reset_stats(); "
        f"SET bioseq.query_cache_ttl = {ttl_ms};",
    )


def _measure_explain(
    cluster: Any, *, mode: str | None, sql: str
) -> dict[str, Any]:
    settings = (
        ""
        if mode is None
        else f"SET bioseq.query_cache_optimisation_mode = '{mode}'; "
    )
    stdout, _ = _run_sql(
        cluster,
        settings
        + f"EXPLAIN (ANALYZE, FORMAT JSON, COSTS OFF, TIMING OFF) {sql}",
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
    mode: str | None,
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
        "cache_plan_count": len(properties),
    }


_MICROPLANNING_SQL = (
    "SELECT sum(a_value + b_value + c_value + d_value + e_value + f_value) "
    "FROM generate_series(1, 4) AS a(a_value) "
    "JOIN generate_series(1, 4) AS b(b_value) ON b_value = a_value "
    "JOIN generate_series(1, 4) AS c(c_value) ON c_value = b_value "
    "JOIN generate_series(1, 4) AS d(d_value) ON d_value = c_value "
    "JOIN generate_series(1, 4) AS e(e_value) ON e_value = d_value "
    "JOIN generate_series(1, 4) AS f(f_value) ON f_value = e_value"
)


def _long_execution_sql(rows: int) -> str:
    return (
        "SELECT sum((value * 17) % 1000003) "
        "FROM generate_series(1::bigint, "
        f"{rows}::bigint) AS source_rows(value)"
    )


def _measure_disabled_hook_overhead(
    cluster: Any,
    inputs: Any,
    *,
    configured_sample_count: int,
) -> dict[str, Any]:
    baseline_inputs = dataclasses.replace(
        inputs,
        kind="bench-no-hook",
        load_cache=False,
        bioseq_dso=None,
        bioseq_bootstrap=None,
    )
    planning_sample_count = max(31, configured_sample_count * 5)
    execution_sample_count = max(3, min(5, configured_sample_count))
    with PRIVATE.UniquePrivateCluster(baseline_inputs) as baseline_cluster:
        settings_stdout, _ = _run_sql(
            baseline_cluster,
            "SELECT count(*) FROM pg_settings "
            "WHERE name = 'bioseq.query_cache_optimisation_mode'",
        )
        extension_stdout, _ = _run_sql(
            baseline_cluster,
            "SELECT count(*) FROM pg_extension WHERE extname = 'bioseq_cache'",
        )
        cache_setting_stdout, _ = _run_sql(
            cluster,
            "SELECT count(*) FROM pg_settings "
            "WHERE name = 'bioseq.query_cache_optimisation_mode'",
        )
        if settings_stdout.strip() != "0" or extension_stdout.strip() != "0":
            raise BenchmarkError(
                "no-hook baseline unexpectedly loaded or bootstrapped bioseq_cache"
            )
        if cache_setting_stdout.strip() != "1":
            raise BenchmarkError(
                "cache-enabled benchmark node exposes no cache mode GUC"
            )

        for _ in range(5):
            _measure_explain(
                baseline_cluster, mode=None, sql=_MICROPLANNING_SQL
            )
            _measure_explain(
                cluster, mode="disabled", sql=_MICROPLANNING_SQL
            )
        baseline_planning_samples: list[int] = []
        disabled_planning_samples: list[int] = []
        planning_cache_plans = 0
        for sample in range(planning_sample_count):
            if sample % 2 == 0:
                baseline_sample = _measure_explain(
                    baseline_cluster, mode=None, sql=_MICROPLANNING_SQL
                )
                disabled_sample = _measure_explain(
                    cluster, mode="disabled", sql=_MICROPLANNING_SQL
                )
            else:
                disabled_sample = _measure_explain(
                    cluster, mode="disabled", sql=_MICROPLANNING_SQL
                )
                baseline_sample = _measure_explain(
                    baseline_cluster, mode=None, sql=_MICROPLANNING_SQL
                )
            baseline_planning_samples.append(
                int(float(baseline_sample["planning_ms"]) * 1_000_000)
            )
            disabled_planning_samples.append(
                int(float(disabled_sample["planning_ms"]) * 1_000_000)
            )
            planning_cache_plans += int(
                disabled_sample["cache_properties"] is not None
            )

        target_execution_ms = 1_100.0
        execution_rows = 8_000_000
        calibration: list[dict[str, Any]] = []
        for _ in range(8):
            sql = _long_execution_sql(execution_rows)
            measured = _measure_explain(
                baseline_cluster, mode=None, sql=sql
            )
            calibration.append(
                {
                    "rows": execution_rows,
                    "execution_ms": measured["execution_ms"],
                }
            )
            if measured["execution_ms"] >= target_execution_ms:
                break
            scale = min(
                4.0,
                max(
                    1.25,
                    target_execution_ms
                    / max(1.0, float(measured["execution_ms"]))
                    * 1.10,
                ),
            )
            execution_rows = int(execution_rows * scale) + 1
            if execution_rows > 512_000_000:
                break
        if calibration[-1]["execution_ms"] < 1_000.0:
            raise BenchmarkError(
                "cannot calibrate a one-second no-hook execution baseline: "
                f"{calibration!r}"
            )

        execution_sql = _long_execution_sql(execution_rows)
        baseline_execution_samples: list[int] = []
        disabled_execution_samples: list[int] = []
        execution_cache_plans = 0
        for sample in range(execution_sample_count):
            if sample % 2 == 0:
                baseline_sample = _measure_explain(
                    baseline_cluster, mode=None, sql=execution_sql
                )
                disabled_sample = _measure_explain(
                    cluster, mode="disabled", sql=execution_sql
                )
            else:
                disabled_sample = _measure_explain(
                    cluster, mode="disabled", sql=execution_sql
                )
                baseline_sample = _measure_explain(
                    baseline_cluster, mode=None, sql=execution_sql
                )
            baseline_execution_samples.append(
                int(float(baseline_sample["execution_ms"]) * 1_000_000)
            )
            disabled_execution_samples.append(
                int(float(disabled_sample["execution_ms"]) * 1_000_000)
            )
            execution_cache_plans += int(
                disabled_sample["cache_properties"] is not None
            )

        reference = _execute_query(
            baseline_cluster, mode=None, sql=execution_sql
        )
        candidate = _execute_query(
            cluster, mode="disabled", sql=execution_sql
        )
        baseline_planning = _sample_summary(baseline_planning_samples)
        disabled_planning = _sample_summary(disabled_planning_samples)
        baseline_execution = _sample_summary(baseline_execution_samples)
        disabled_execution = _sample_summary(disabled_execution_samples)
        baseline_planning_ns = int(baseline_planning["p50_ns"])
        disabled_planning_ns = int(disabled_planning["p50_ns"])
        baseline_execution_ns = int(baseline_execution["p50_ns"])
        disabled_execution_ns = int(disabled_execution["p50_ns"])
        if baseline_planning_ns <= 0 or baseline_execution_ns <= 0:
            raise BenchmarkError("no-hook baseline produced a zero-duration median")
        return {
            "baseline": {
                "kind": "private_node_without_cache_preload_or_bootstrap",
                "run_directory": str(baseline_cluster.run_dir),
                "cache_setting_count": 0,
                "cache_extension_count": 0,
                "planning_query": _MICROPLANNING_SQL,
                "planning": baseline_planning,
                "planning_sample_count": planning_sample_count,
                "execution_query": execution_sql,
                "execution_rows": execution_rows,
                "execution": baseline_execution,
                "execution_sample_count": execution_sample_count,
                "calibration": calibration,
                "correctness": {
                    "row_count": reference.rows,
                    "ordered_sha256": reference.ordered_sha256,
                    "multiset_sha256": reference.multiset_sha256,
                },
            },
            "disabled_hook": {
                "cache_setting_count": 1,
                "planning": disabled_planning,
                "execution": disabled_execution,
                "planning_cache_plan_count": planning_cache_plans,
                "execution_cache_plan_count": execution_cache_plans,
                "correctness": {
                    "passed": _same_result(reference, candidate, True),
                    "row_count": candidate.rows,
                    "ordered_sha256": candidate.ordered_sha256,
                    "multiset_sha256": candidate.multiset_sha256,
                },
            },
            "disabled_planning_overhead_percent": (
                (disabled_planning_ns - baseline_planning_ns)
                / baseline_planning_ns
                * 100.0
            ),
            "disabled_execution_overhead_percent": (
                (disabled_execution_ns - baseline_execution_ns)
                / baseline_execution_ns
                * 100.0
            ),
            "source_execution_exceeds_one_second": (
                baseline_execution_ns >= 1_000_000_000
            ),
            "detail": (
                "Paired samples alternate between simultaneous disposable "
                "PostgreSQL nodes. The baseline has neither cache preload nor "
                "bootstrap; server EXPLAIN timings exclude connection startup "
                "and cache-mode SET time."
            ),
        }


def _measure_case(
    cluster: Any,
    case: Mapping[str, Any],
    *,
    sample_count: int,
) -> dict[str, Any]:
    sql = str(case["sql"])
    ordered = bool(case["ordered"])
    if sample_count <= 0:
        raise BenchmarkError("SQL benchmark sample count must be positive")
    _reset_case(cluster)
    disabled_explain = _measure_explain_samples(
        cluster,
        mode="disabled",
        sql=sql,
        sample_count=sample_count,
    )

    force_file_tier = str(case["category"]) == "file_tier_candidate"
    if force_file_tier and sample_count < 2:
        raise BenchmarkError(
            "file-tier cold/warm comparison requires at least two samples"
        )
    producer_samples: list[QueryOutput] = []
    reference_samples: list[QueryOutput] = []
    producer_stats: list[dict[str, int]] = []
    cold_resources: dict[str, Any] = {
        "status": "not_applicable",
        "detail": "The case does not use the forced file tier.",
    }
    warm_resources = dict(cold_resources)
    try:
        for sample in range(sample_count):
            _reset_case(cluster)
            if force_file_tier:
                _run_sql(
                    cluster,
                    "SELECT bioseq_cache_test.set_force_file_tier(true)",
                )
            if sample % 2 == 0:
                reference_samples.append(
                    _execute_query(cluster, mode="disabled", sql=sql)
                )
                producer_samples.append(
                    _execute_query(cluster, mode="same", sql=sql)
                )
            else:
                producer_samples.append(
                    _execute_query(cluster, mode="same", sql=sql)
                )
                reference_samples.append(
                    _execute_query(cluster, mode="disabled", sql=sql)
                )
            producer_stats.append(_stats(cluster))
        page_cache_drop = (
            _drop_cache_store_page_cache(cluster)
            if force_file_tier
            else {
                "status": "not_applicable",
                "detail": "The case does not use the forced file tier.",
            }
        )
        hit_samples: list[QueryOutput]
        if force_file_tier:
            with _ProcessResourceSampler(cluster) as cold_sampler:
                first_hit = _execute_query(cluster, mode="same", sql=sql)
            cold_resources = cold_sampler.report()
            with _ProcessResourceSampler(cluster) as warm_sampler:
                warm_hits = [
                    _execute_query(cluster, mode="same", sql=sql)
                    for _ in range(sample_count - 1)
                ]
            warm_resources = warm_sampler.report()
            hit_samples = [first_hit, *warm_hits]
        else:
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
        runtime = _runtime_storage_snapshot(cluster)
        runtime["filesystem_policy"] = _filesystem_policy_snapshot(cluster)
    finally:
        if force_file_tier:
            _run_sql(
                cluster, "SELECT bioseq_cache_test.set_force_file_tier(false)"
            )
    runtime["storage_explain"] = same_explain["cache_storage"]
    reference = reference_samples[0]
    correctness = all(
        _same_result(reference, sample, ordered)
        for sample in [*reference_samples, *producer_samples, *hit_samples]
    )
    hit_action = "[Hit] exact final result"
    producer_behavior = all(
        stats["produced"] == 1
        and stats["completed_hits"] == 0
        and stats["fallbacks"] == 0
        for stats in producer_stats
    )
    cache_behavior = {
        "passed": (
            producer_behavior
            and case_stats["produced"] == 1
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
        "producer_trials_passed": producer_behavior,
        "producer_trial_stats": producer_stats,
    }
    reference_ns = _nearest_rank_percentile(
        [sample.elapsed_ns for sample in reference_samples], 50, 100
    )
    producer_ns = _nearest_rank_percentile(
        [sample.elapsed_ns for sample in producer_samples], 50, 100
    )
    producer_delta = (
        ((producer_ns - reference_ns) / reference_ns) * 100.0
        if reference_ns > 0
        else 0.0
    )
    hit_p50 = _nearest_rank_percentile(
        [sample.elapsed_ns for sample in hit_samples], 50, 100
    )
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
    file_hit_temperature: dict[str, Any]
    if force_file_tier:
        if len(hit_samples) < 2:
            raise BenchmarkError(
                "file-tier cold/warm comparison requires at least two hit samples"
            )
        first_hit = hit_samples[0]
        warm_hits = hit_samples[1:]
        warm_p50 = _nearest_rank_percentile(
            [sample.elapsed_ns for sample in warm_hits], 50, 100
        )
        first_checksum = (
            first_hit.ordered_sha256
            if ordered else first_hit.multiset_sha256
        )
        warm_checksums = [
            sample.ordered_sha256 if ordered else sample.multiset_sha256
            for sample in warm_hits
        ]
        checksum_evidence = {
            "status": "measured",
            "algorithm": "sha256",
            "comparison_digest": digest_name,
            "reference": digest_value,
            "first_cold_file_hit": first_checksum,
            "warm_file_hits": warm_checksums,
            "passed": (
                first_checksum == digest_value
                and all(value == digest_value for value in warm_checksums)
            ),
        }
        prevalidation_evidence = {
            "status": (
                "measured"
                if _is_measured_integer_metric(
                    runtime["prevalidation_bytes"], minimum=1
                )
                and _is_measured_integer_metric(
                    runtime["prevalidation_duration_ns"], minimum=1
                )
                and _is_measured_integer_metric(
                    runtime["row_group_count"], minimum=1
                )
                else "unavailable"
            ),
            "bytes": runtime["prevalidation_bytes"],
            "duration_ns": runtime["prevalidation_duration_ns"],
            "row_group_count": runtime["row_group_count"],
            "validated_result_checksum": checksum_evidence,
        }
        prevalidation_evidence["passed"] = (
            prevalidation_evidence["status"] == "measured"
            and checksum_evidence["passed"]
        )
        cold_physical_io_passed = _cold_physical_io_measured(
            cold_resources
        )
        file_hit_temperature = {
            "status": "measured"
            if (
                page_cache_drop["status"] == "measured"
                and prevalidation_evidence["status"] == "measured"
                and cold_physical_io_passed
            )
            else "unavailable",
            "page_cache_drop": page_cache_drop,
            "full_block_prevalidation": prevalidation_evidence,
            "first_cold_file_hit": {
                **_sample_summary([first_hit.elapsed_ns]),
                "time_to_first_row": _first_row_summary([first_hit]),
                "process_resources": cold_resources,
                "physical_io_passed": cold_physical_io_passed,
                "correctness_passed": _same_result(
                    reference, first_hit, ordered
                ),
            },
            "warm_file_hits": {
                **_sample_summary(
                    [sample.elapsed_ns for sample in warm_hits]
                ),
                "time_to_first_row": _first_row_summary(warm_hits),
                "process_resources": warm_resources,
                "correctness_passed": all(
                    _same_result(reference, sample, ordered)
                    for sample in warm_hits
                ),
            },
            "cold_to_warm_p50_ratio": (
                first_hit.elapsed_ns / warm_p50 if warm_p50 > 0 else None
            ),
            "passed": (
                page_cache_drop["status"] == "measured"
                and page_cache_drop["file_count"] > 0
                and page_cache_drop["advised_bytes"] > 0
                and prevalidation_evidence["passed"]
                and cold_physical_io_passed
                and _same_result(reference, first_hit, ordered)
                and all(
                    _same_result(reference, sample, ordered)
                    for sample in warm_hits
                )
            ),
            "detail": (
                "The first completed file hit follows fsync plus "
                "POSIX_FADV_DONTNEED. Acceptance requires runtime-measured "
                "full-block prevalidation bytes and duration, a positive "
                "row-group count, a measured nonzero physical server read, "
                "and matching result checksums for the cold and warm completed "
                "hits."
            ),
        }
    else:
        file_hit_temperature = {
            "status": "not_applicable",
            "passed": None,
            "detail": "The case does not use the forced file tier.",
        }
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
            **_sample_summary(
                [sample.elapsed_ns for sample in reference_samples]
            ),
            "time_to_first_row": _first_row_summary(reference_samples),
            "throughput": _throughput_summary(reference_samples),
            **disabled_explain,
        },
        "producer": {
            **_sample_summary(
                [sample.elapsed_ns for sample in producer_samples]
            ),
            "time_to_first_row": _first_row_summary(producer_samples),
            "throughput": _throughput_summary(producer_samples),
            "encoded_throughput": {
                "logical_bytes_per_second": _fixed_quantity_throughput(
                    case_stats["logical_bytes"],
                    producer_samples,
                    "logical bytes/s",
                ),
                "stored_bytes_per_second": _fixed_quantity_throughput(
                    case_stats["stored_bytes"],
                    producer_samples,
                    "stored bytes/s",
                ),
            },
            "baseline_delta_percent": producer_delta,
            "paired_sample_count": sample_count,
        },
        "completed_hit": {
            **_sample_summary([sample.elapsed_ns for sample in hit_samples]),
            "time_to_first_row": hit_first_row,
            "throughput": _throughput_summary(hit_samples),
            "cache_read_throughput": {
                "logical_bytes_per_second": _fixed_quantity_throughput(
                    case_stats["logical_bytes"],
                    hit_samples,
                    "logical bytes/s",
                ),
                "stored_bytes_per_second": _fixed_quantity_throughput(
                    case_stats["stored_bytes"],
                    hit_samples,
                    "stored bytes/s",
                ),
            },
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
        "file_hit_temperature": file_hit_temperature,
        "runtime_storage": runtime,
        "encoded_bytes": {
            "logical": case_stats["logical_bytes"],
            "stored": case_stats["stored_bytes"],
        },
        "stats": case_stats,
    }


def _long_exact_sql(right_rows: int) -> str:
    return (
        "SELECT sum((left_rows.id * 17 + right_rows.id) % 1000003) "
        "FROM cache_bench_narrow AS left_rows "
        "CROSS JOIN cache_bench_narrow AS right_rows "
        f"WHERE right_rows.id <= {right_rows}"
    )


def _measure_long_source_exact_hit(
    cluster: Any,
    *,
    configured_sample_count: int,
) -> dict[str, Any]:
    sample_count = max(3, min(5, configured_sample_count))
    target_execution_ms = 1_100.0
    right_rows = 128
    calibration: list[dict[str, Any]] = []
    for _ in range(8):
        sql = _long_exact_sql(right_rows)
        measured = _measure_explain(cluster, mode="disabled", sql=sql)
        calibration.append(
            {
                "right_rows": right_rows,
                "execution_ms": measured["execution_ms"],
            }
        )
        if measured["execution_ms"] >= target_execution_ms:
            break
        scale = min(
            4.0,
            max(
                1.25,
                target_execution_ms
                / max(1.0, float(measured["execution_ms"]))
                * 1.10,
            ),
        )
        right_rows = min(20_000, int(right_rows * scale) + 1)
    if calibration[-1]["execution_ms"] < 1_000.0:
        raise BenchmarkError(
            "cannot calibrate a one-second heap exact-cache source: "
            f"{calibration!r}"
        )
    sql = _long_exact_sql(right_rows)
    _reset_case(cluster)
    references = [
        _execute_query(cluster, mode="disabled", sql=sql)
        for _ in range(sample_count)
    ]
    reference = references[0]
    producer = _execute_query(cluster, mode="same", sql=sql)
    hits = [
        _execute_query(cluster, mode="same", sql=sql)
        for _ in range(sample_count)
    ]
    explain = _measure_explain_samples(
        cluster,
        mode="same",
        sql=sql,
        sample_count=sample_count,
    )
    stats = _stats(cluster)
    hit_p50_ns = _nearest_rank_percentile(
        [output.elapsed_ns for output in hits], 50, 100
    )
    correct = (
        all(_same_result(reference, output, True) for output in references)
        and _same_result(reference, producer, True)
        and all(_same_result(reference, hit, True) for hit in hits)
    )
    hit_action = "[Hit] exact final result"
    behavior = (
        stats["produced"] == 1
        and stats["completed_hits"] >= sample_count * 2
        and stats["fallbacks"] == 0
        and len(explain["actual_cache_actions"]) == sample_count
        and all(
            action == hit_action for action in explain["actual_cache_actions"]
        )
    )
    return {
        "key": "one_second_compact_exact_result",
        "sql": sql,
        "calibration": calibration,
        "right_rows": right_rows,
        "correctness": {
            "passed": correct,
            "row_count": reference.rows,
            "ordered_sha256": reference.ordered_sha256,
            "multiset_sha256": reference.multiset_sha256,
        },
        "disabled": {
            **_sample_summary([output.elapsed_ns for output in references]),
            "time_to_first_row": _first_row_summary(references),
            "throughput": _throughput_summary(references),
        },
        "producer": {
            **_sample_summary([producer.elapsed_ns]),
            "time_to_first_row": _first_row_summary([producer]),
            "throughput": _throughput_summary([producer]),
        },
        "completed_hit": {
            **_sample_summary([output.elapsed_ns for output in hits]),
            "time_to_first_row": _first_row_summary(hits),
            "throughput": _throughput_summary(hits),
            "speedup_vs_disabled": (
                _nearest_rank_percentile(
                    [output.elapsed_ns for output in references], 50, 100
                ) / hit_p50_ns
                if hit_p50_ns > 0
                else None
            ),
            **explain,
        },
        "source_execution_exceeds_one_second": (
            _nearest_rank_percentile(
                [output.elapsed_ns for output in references], 50, 100
            ) >= 1_000_000_000
        ),
        "cache_behavior": {
            "passed": behavior,
            "stats": stats,
            "explain_actions": explain["actual_cache_actions"],
        },
        "runtime_storage": _runtime_storage_snapshot(cluster),
    }


def _measure_generic_provider_reuse(
    cluster: Any,
    *,
    fixture_rows: int,
    sample_count: int,
) -> dict[str, Any]:
    if fixture_rows <= 0 or sample_count <= 0:
        raise BenchmarkError("generic provider benchmark sizes must be positive")
    partition_input_rows = max(512, fixture_rows // 4)
    projection = (
        "SELECT row_id, base_id, occurrence_no, nullable_value, "
        "payload, capability_mask "
    )
    current_sql = (
        projection
        + "FROM cache_bench_provider_rows "
        "WHERE nullable_value <= 10 ORDER BY row_id"
    )
    overlap_sql = (
        projection
        + "FROM cache_bench_provider_rows "
        "WHERE nullable_value >= 0 ORDER BY row_id"
    )
    partition_sql = (
        projection
        + "FROM cache_bench_provider_part_a "
        "WHERE nullable_value <= 10 ORDER BY row_id"
    )

    with _ProviderSession(cluster) as session:
        _reset_case(cluster)
        session.use_relation("cache_bench_provider_rows")
        session.reset_provider()
        disabled_provider_before = session.provider_stats()
        disabled_cache_before = _stats(cluster)
        disabled_outputs = [
            session.execute(mode="disabled", sql=current_sql)
            for _ in range(sample_count)
        ]
        disabled_provider_after = session.provider_stats()
        disabled_cache_after = _stats(cluster)
        reference = disabled_outputs[0]
        disabled = _provider_phase_report(
            cluster,
            reference=reference,
            outputs=disabled_outputs,
            ordered=True,
            cache_before=disabled_cache_before,
            cache_after=disabled_cache_after,
            provider_before=disabled_provider_before,
            provider_after=disabled_provider_after,
            input_rows=fixture_rows,
        )

        session.reset_provider()
        producer_provider_before = session.provider_stats()
        producer_cache_before = _stats(cluster)
        producer_output = session.execute(mode="reuse", sql=current_sql)
        producer_provider_after = session.provider_stats()
        producer_cache_after = _stats(cluster)
        producer = _provider_phase_report(
            cluster,
            reference=reference,
            outputs=[producer_output],
            ordered=True,
            cache_before=producer_cache_before,
            cache_after=producer_cache_after,
            provider_before=producer_provider_before,
            provider_after=producer_provider_after,
            input_rows=fixture_rows,
        )

        session.reset_provider()
        hit_provider_before = session.provider_stats()
        hit_cache_before = _stats(cluster)
        hit_outputs = [
            session.execute(mode="reuse", sql=current_sql)
            for _ in range(sample_count)
        ]
        hit_provider_after = session.provider_stats()
        hit_cache_after = _stats(cluster)
        identical_hit = _provider_phase_report(
            cluster,
            reference=reference,
            outputs=hit_outputs,
            ordered=True,
            cache_before=hit_cache_before,
            cache_after=hit_cache_after,
            provider_before=hit_provider_before,
            provider_after=hit_provider_after,
            input_rows=fixture_rows,
        )

        disabled_cache_delta = disabled["cache_stats"]["delta"]
        producer_cache_delta = producer["cache_stats"]["delta"]
        producer_provider_delta = producer["provider_stats"]["delta"]
        hit_cache_delta = identical_hit["cache_stats"]["delta"]
        hit_provider_delta = identical_hit["provider_stats"]["delta"]
        current_behavior = {
            "passed": (
                disabled["correctness"]["passed"]
                and producer["correctness"]["passed"]
                and identical_hit["correctness"]["passed"]
                and all(value == 0 for value in disabled_cache_delta.values())
                and producer_cache_delta["produced"] == 1
                and producer_cache_delta["reuse_hits"] == 0
                and producer_cache_delta["fallbacks"] == 0
                and producer_provider_delta["extract_occurrence"] > 0
                and producer_provider_delta["executor_rows"] > 0
                and producer_provider_delta["gated_expensive_rows"] > 0
                and hit_cache_delta["produced"] == 0
                and hit_cache_delta["reuse_hits"] == sample_count
                and hit_cache_delta["fallbacks"] == 0
                and hit_provider_delta["extract_occurrence"] == 0
                and hit_provider_delta["executor_rows"] == 0
                and hit_provider_delta["gated_expensive_rows"] == 0
            ),
            "expected_identical_hits": sample_count,
            "producer_cache_delta": producer_cache_delta,
            "producer_provider_delta": producer_provider_delta,
            "hit_cache_delta": hit_cache_delta,
            "hit_provider_delta": hit_provider_delta,
        }

        _reset_case(cluster)
        session.use_relation("cache_bench_provider_rows")
        session.reset_provider()
        overlap_source_provider_before = session.provider_stats()
        overlap_source_cache_before = _stats(cluster)
        overlap_source_output = session.execute(
            mode="reuse", sql=current_sql
        )
        overlap_source_provider_after = session.provider_stats()
        overlap_source_cache_after = _stats(cluster)
        overlap_source = _provider_phase_report(
            cluster,
            reference=reference,
            outputs=[overlap_source_output],
            ordered=True,
            cache_before=overlap_source_cache_before,
            cache_after=overlap_source_cache_after,
            provider_before=overlap_source_provider_before,
            provider_after=overlap_source_provider_after,
            input_rows=fixture_rows,
        )

        session.reset_provider()
        overlap_disabled_provider_before = session.provider_stats()
        overlap_disabled_cache_before = _stats(cluster)
        overlap_reference_output = session.execute(
            mode="disabled", sql=overlap_sql
        )
        overlap_disabled_provider_after = session.provider_stats()
        overlap_disabled_cache_after = _stats(cluster)
        overlap_disabled = _provider_phase_report(
            cluster,
            reference=overlap_reference_output,
            outputs=[overlap_reference_output],
            ordered=True,
            cache_before=overlap_disabled_cache_before,
            cache_after=overlap_disabled_cache_after,
            provider_before=overlap_disabled_provider_before,
            provider_after=overlap_disabled_provider_after,
            input_rows=fixture_rows,
        )

        session.reset_provider()
        overlap_reuse_provider_before = session.provider_stats()
        overlap_reuse_cache_before = _stats(cluster)
        overlap_reuse_output = session.execute(mode="reuse", sql=overlap_sql)
        overlap_reuse_provider_after = session.provider_stats()
        overlap_reuse_cache_after = _stats(cluster)
        overlap_reuse = _provider_phase_report(
            cluster,
            reference=overlap_reference_output,
            outputs=[overlap_reuse_output],
            ordered=True,
            cache_before=overlap_reuse_cache_before,
            cache_after=overlap_reuse_cache_after,
            provider_before=overlap_reuse_provider_before,
            provider_after=overlap_reuse_provider_after,
            input_rows=fixture_rows,
        )
        overlap_cache_delta = overlap_reuse["cache_stats"]["delta"]
        overlap_provider_delta = overlap_reuse["provider_stats"]["delta"]
        overlap_disabled_provider_delta = overlap_disabled[
            "provider_stats"
        ]["delta"]
        overlap_supported = (
            overlap_cache_delta["reuse_hits"] > 0
            and overlap_provider_delta["map_source"] > 0
        )
        overlap_work_reduced = (
            overlap_provider_delta["executor_rows"]
            < overlap_disabled_provider_delta["executor_rows"]
            and overlap_provider_delta["gated_expensive_rows"]
            < overlap_disabled_provider_delta["gated_expensive_rows"]
        )
        overlapping = {
            "status": "measured" if overlap_supported else "not_applicable",
            "detail": (
                "An older predicate source was mapped and read while the "
                "provider executed only the residual."
                if overlap_supported
                else "The planner/runtime emitted no mapped older-source hit."
            ),
            "source": overlap_source,
            "disabled_reference": overlap_disabled,
            "reuse": overlap_reuse,
            "behavior": {
                "supported": overlap_supported,
                "passed": (
                    overlap_reuse["correctness"]["passed"]
                    and overlap_work_reduced
                    and overlap_cache_delta["fallbacks"] == 0
                )
                if overlap_supported
                else None,
                "provider_work_reduced": overlap_work_reduced,
                "cache_delta": overlap_cache_delta,
                "provider_delta": overlap_provider_delta,
                "disabled_provider_delta": overlap_disabled_provider_delta,
            },
        }

        _reset_case(cluster)
        session.use_relation("cache_bench_provider_part_a")
        session.reset_provider()
        partition_disabled_provider_before = session.provider_stats()
        partition_disabled_cache_before = _stats(cluster)
        partition_reference_output = session.execute(
            mode="disabled", sql=partition_sql
        )
        partition_disabled_provider_after = session.provider_stats()
        partition_disabled_cache_after = _stats(cluster)
        partition_disabled = _provider_phase_report(
            cluster,
            reference=partition_reference_output,
            outputs=[partition_reference_output],
            ordered=True,
            cache_before=partition_disabled_cache_before,
            cache_after=partition_disabled_cache_after,
            provider_before=partition_disabled_provider_before,
            provider_after=partition_disabled_provider_after,
            input_rows=partition_input_rows,
        )

        session.reset_provider()
        partition_producer_provider_before = session.provider_stats()
        partition_producer_cache_before = _stats(cluster)
        partition_producer_output = session.execute(
            mode="reuse", sql=partition_sql
        )
        partition_producer_provider_after = session.provider_stats()
        partition_producer_cache_after = _stats(cluster)
        partition_producer = _provider_phase_report(
            cluster,
            reference=partition_reference_output,
            outputs=[partition_producer_output],
            ordered=True,
            cache_before=partition_producer_cache_before,
            cache_after=partition_producer_cache_after,
            provider_before=partition_producer_provider_before,
            provider_after=partition_producer_provider_after,
            input_rows=partition_input_rows,
        )

        cache_before_neighbor_write = _stats(cluster)
        write_started = time.perf_counter_ns()
        _run_sql(
            cluster,
            "UPDATE cache_bench_provider_part_b "
            "SET payload = payload || '-updated' WHERE row_id = "
            "(SELECT min(row_id) FROM cache_bench_provider_part_b)",
        )
        neighbor_write_ns = time.perf_counter_ns() - write_started
        cache_after_neighbor_write = _stats(cluster)

        session.reset_provider()
        partition_hit_provider_before = session.provider_stats()
        partition_hit_cache_before = _stats(cluster)
        partition_hit_outputs = [
            session.execute(mode="reuse", sql=partition_sql)
            for _ in range(sample_count)
        ]
        partition_hit_provider_after = session.provider_stats()
        partition_hit_cache_after = _stats(cluster)
        partition_hit = _provider_phase_report(
            cluster,
            reference=partition_reference_output,
            outputs=partition_hit_outputs,
            ordered=True,
            cache_before=partition_hit_cache_before,
            cache_after=partition_hit_cache_after,
            provider_before=partition_hit_provider_before,
            provider_after=partition_hit_provider_after,
            input_rows=partition_input_rows,
        )
        partition_cache_delta = partition_hit["cache_stats"]["delta"]
        partition_provider_delta = partition_hit["provider_stats"]["delta"]
        partition_behavior = {
            "passed": (
                partition_disabled["correctness"]["passed"]
                and partition_producer["correctness"]["passed"]
                and partition_hit["correctness"]["passed"]
                and partition_producer["cache_stats"]["delta"]["produced"]
                == 1
                and cache_after_neighbor_write["produced"]
                == cache_before_neighbor_write["produced"]
                and partition_cache_delta["produced"] == 0
                and partition_cache_delta["reuse_hits"] == sample_count
                and partition_cache_delta["fallbacks"] == 0
                and partition_provider_delta["extract_occurrence"] == 0
                and partition_provider_delta["executor_rows"] == 0
                and partition_provider_delta["gated_expensive_rows"] == 0
            ),
            "expected_post_write_hits": sample_count,
            "neighbor_write_ns": neighbor_write_ns,
            "cache_before_neighbor_write": cache_before_neighbor_write,
            "cache_after_neighbor_write": cache_after_neighbor_write,
            "cache_neighbor_write_delta": _counter_delta(
                cache_before_neighbor_write,
                cache_after_neighbor_write,
                tuple(cache_before_neighbor_write.keys()),
            ),
            "post_write_cache_delta": partition_cache_delta,
            "post_write_provider_delta": partition_provider_delta,
        }
        partition = {
            "disabled_reference": partition_disabled,
            "producer": partition_producer,
            "post_neighbor_write_hit": partition_hit,
            "behavior": partition_behavior,
        }

    overlap_correct = (
        overlapping["behavior"]["passed"] is not False
    )
    return {
        "fixture": {
            "provider_rows": fixture_rows,
            "partition_rows_per_leaf": partition_input_rows,
            "measurement_scope": (
                "One persistent backend keeps provider counters local; wall "
                "and first-row samples start when each SQL command is sent and "
                "exclude initial connection startup."
            ),
            "null_rule": "row_id % 17 = 0",
            "current_predicate": "nullable_value <= 10",
            "overlap_predicate": "nullable_value >= 0",
            "current_sql": current_sql,
            "overlap_sql": overlap_sql,
            "partition_sql": partition_sql,
        },
        "disabled_reference": disabled,
        "reusable_producer": producer,
        "identical_current_hit": identical_hit,
        "current_behavior": current_behavior,
        "overlapping_predicate_reuse": overlapping,
        "partition_fragment_reuse": partition,
        "correctness": {
            "passed": (
                current_behavior["passed"]
                and partition_behavior["passed"]
                and overlap_correct
            ),
            "overlap_applicable": overlapping["status"] == "measured",
        },
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


def _wait_for_waiter_cleanup(
    cluster: Any, timeout_seconds: float = 30.0
) -> None:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        stdout, _ = _run_sql(
            cluster,
            "SELECT COALESCE(max(waiter_count), 0) "
            "FROM bioseq_cache_test.entry_states()",
        )
        if int(stdout.strip()) == 0:
            return
        time.sleep(0.02)
    raise BenchmarkError("cancelled waiter did not release its registry claim")


def _cancel_waiter_backend(cluster: Any, process: subprocess.Popen[str],
                           application_name: str) -> None:
    deadline = time.monotonic() + 30.0
    backend_pid = 0
    while time.monotonic() < deadline:
        stdout, _ = _run_sql(
            cluster,
            "SELECT COALESCE(max(pid), 0) FROM pg_stat_activity "
            f"WHERE application_name = '{application_name}'",
        )
        backend_pid = int(stdout.strip())
        if backend_pid != 0:
            break
        if process.poll() is not None:
            raise BenchmarkError("waiter psql exited before cancellation")
        time.sleep(0.02)
    if backend_pid == 0:
        raise BenchmarkError("waiter backend did not appear in pg_stat_activity")
    stdout, _ = _run_sql(
        cluster, f"SELECT pg_cancel_backend({backend_pid})"
    )
    if stdout.strip() != "t":
        raise BenchmarkError(
            f"pg_cancel_backend({backend_pid}) returned {stdout!r}"
        )
    try:
        _, error = process.communicate(timeout=30.0)
    except subprocess.TimeoutExpired as timeout:
        process.kill()
        process.communicate()
        raise BenchmarkError("cancelled waiter psql did not exit") from timeout
    if process.returncode == 0 or "canceling statement" not in error:
        raise BenchmarkError(
            "waiter cancellation did not surface query cancellation: "
            f"returncode={process.returncode}, stderr={error!r}"
        )
    _wait_for_waiter_cleanup(cluster)


def _measure_waiter_cancel_latency(
    cluster: Any, sql: str, sample_count: int
) -> dict[str, Any]:
    trials = max(3, min(sample_count, 5))
    _reset_case(cluster)
    reference = _execute_query(cluster, mode="disabled", sql=sql)

    def trial(
        cancel_waiter: bool, sequence: int
    ) -> tuple[QueryOutput, int, dict[str, int], dict[str, Any]]:
        _reset_case(cluster)
        waiter: subprocess.Popen[str] | None = None
        try:
            with concurrent.futures.ThreadPoolExecutor(max_workers=1) \
                    as executor, _PauseController(
                        cluster, "after_building_publish"
                    ) as pause:
                producer = executor.submit(
                    _execute_query, cluster, mode="same", sql=sql
                )
                _run_sql(
                    cluster,
                    "SELECT bioseq_cache_test.wait_until_paused("
                    "'after_building_publish')",
                )
                if cancel_waiter:
                    application_name = (
                        f"bioseq_cache_cancel_{os.getpid()}_{sequence}"
                    )
                    environment = cluster.environment()
                    environment["PGAPPNAME"] = application_name
                    waiter = subprocess.Popen(
                        _psql_command(
                            cluster,
                            "SET bioseq.query_cache_optimisation_mode = 'same'; "
                            + sql,
                        ),
                        env=environment,
                        stdin=subprocess.DEVNULL,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        text=True,
                    )
                    _wait_for_waiters(cluster, 1)
                    _cancel_waiter_backend(
                        cluster, waiter, application_name
                    )
                released_at = time.perf_counter_ns()
                pause.release()
                output = producer.result()
                return (
                    output,
                    output.completed_at_ns - released_at,
                    _stats(cluster),
                    _exact_child_execution_snapshot(cluster),
                )
        finally:
            if waiter is not None and waiter.poll() is None:
                waiter.kill()
                waiter.communicate()

    baseline: list[
        tuple[QueryOutput, int, dict[str, int], dict[str, Any]]
    ] = [
        trial(False, index) for index in range(trials)
    ]
    cancelled: list[
        tuple[QueryOutput, int, dict[str, int], dict[str, Any]]
    ] = [
        trial(True, index) for index in range(trials)
    ]
    baseline_latency = [sample[1] for sample in baseline]
    cancelled_latency = [sample[1] for sample in cancelled]
    baseline_p95 = _nearest_rank_percentile(
        baseline_latency, 95, 100
    )
    cancelled_p95 = _nearest_rank_percentile(
        cancelled_latency, 95, 100
    )
    tolerance_ns = max(int(baseline_p95 * 0.25), 5_000_000)
    correctness = all(
        _same_result(reference, sample[0], False)
        for sample in [*baseline, *cancelled]
    )

    def behavior_matches(
        sample: tuple[QueryOutput, int, dict[str, int], dict[str, Any]],
        *,
        cancelled_waiter: bool,
    ) -> bool:
        stats = sample[2]
        child_executions = sample[3]
        return (
            stats["produced"] == 1
            and stats["completed_hits"] == 0
            and stats["inflight_waits"] == int(cancelled_waiter)
            and stats["fallbacks"] == 0
            and stats["invalidations"] == 0
            and child_executions["status"] == "measured"
            and child_executions["value"] == 1
        )

    behavior_passed = all(
        behavior_matches(sample, cancelled_waiter=False)
        for sample in baseline
    ) and all(
        behavior_matches(sample, cancelled_waiter=True)
        for sample in cancelled
    )
    return {
        "trials": trials,
        "baseline_release_to_complete": _sample_summary(baseline_latency),
        "cancelled_release_to_complete": _sample_summary(cancelled_latency),
        "p95_delta_ns": cancelled_p95 - baseline_p95,
        "absolute_p95_delta_ns": abs(cancelled_p95 - baseline_p95),
        "tolerance_ns": tolerance_ns,
        "correctness": {
            "passed": correctness,
            "oracle": "independent disabled execution",
            "row_count": reference.rows,
            "ordered_sha256": reference.ordered_sha256,
            "multiset_sha256": reference.multiset_sha256,
        },
        "cache_behavior": {
            "passed": behavior_passed,
            "baseline_stats": [sample[2] for sample in baseline],
            "cancelled_stats": [sample[2] for sample in cancelled],
            "baseline_child_executions": [sample[3] for sample in baseline],
            "cancelled_child_executions": [sample[3] for sample in cancelled],
        },
        "passed": (
            correctness
            and behavior_passed
            and _latency_delta_within_tolerance(
                baseline_p95, cancelled_p95, tolerance_ns
            )
        ),
        "detail": (
            "Producer release-to-completion p95 after a registered waiter is "
            "cancelled before release; the two-sided absolute tolerance is "
            "25% or 5 ms, whichever is larger."
        ),
    }


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
        controlled_wait_seconds = 0.05
        time.sleep(controlled_wait_seconds)
        released_at = time.perf_counter_ns()
        pause.release()
        producer_output = producer.result()
        waiter_outputs = [waiter.result() for waiter in waiters]
        outputs = [producer_output, *waiter_outputs]
        completed_at = time.perf_counter_ns()

    fanout_stats = _stats(cluster)
    runtime = _runtime_storage_snapshot(cluster)
    correct = all(_same_result(reference, output, False) for output in outputs)
    child_executions = _exact_child_execution_snapshot(cluster)
    cache_behavior = {
        "passed": (
            fanout_stats["produced"] == 1
            and fanout_stats["completed_hits"] == 0
            and fanout_stats["inflight_waits"] == clients - 1
            and fanout_stats["fallbacks"] == 0
            and fanout_stats["invalidations"] == 0
            and child_executions["status"] == "measured"
            and child_executions["value"] == 1
        ),
        "expected_producers": 1,
        "expected_inflight_waits": clients - 1,
        "stats": fanout_stats,
        "child_executions": child_executions,
    }
    observed_wait_lower_bound_ns = released_at - waiters_observed_at
    query_start_to_release = [
        max(
            0,
            released_at
            - (output.completed_at_ns - output.elapsed_ns),
        )
        for output in waiter_outputs
    ]
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
        "waiter_latency": _sample_summary(
            [output.elapsed_ns for output in waiter_outputs]
        ),
        "waiter_throughput": _throughput_summary(waiter_outputs),
        "wait_duration": {
            "status": "bounded_measurement",
            "controlled_hold_target_ns": int(
                controlled_wait_seconds * 1_000_000_000
            ),
            "condition_wait_lower_bound_ns": observed_wait_lower_bound_ns,
            "query_start_to_release_upper_bounds": _sample_summary(
                query_start_to_release
            ),
            "release_to_waiter_completion": _sample_summary(
                [
                    max(0, output.completed_at_ns - released_at)
                    for output in waiter_outputs
                ]
            ),
            "server_condition_variable_duration": _unavailable_metric(
                "The runtime exposes waiter counts but no exact per-waiter CV "
                "timestamps; the report records measured lower/upper bounds."
            ),
            "detail": (
                "All waiters are observed in the shared registry, then held "
                "for 50 ms before release. The observed-to-release interval is "
                "a lower bound; query-start-to-release is an upper bound that "
                "includes planning and lookup."
            ),
        },
        "release_to_all_complete_ns": completed_at - released_at,
        "runtime_storage": runtime,
        "stats": fanout_stats,
        "child_execution_count": child_executions,
        "cache_behavior": cache_behavior,
    }


def _measure_invalidation(
    cluster: Any, sample_count: int
) -> list[dict[str, Any]]:
    results: list[dict[str, Any]] = []
    trials = max(3, min(5, sample_count))

    def operation_sql(key: str, trial: int) -> str:
        if key == "insert":
            row_id = 1_000_000_000 + trial
            return (
                "INSERT INTO cache_bench_mutable VALUES "
                f"({row_id}, 1, {row_id * 10})"
            )
        if key == "update":
            return (
                "UPDATE cache_bench_mutable SET payload = payload + 1 "
                "WHERE id = 1"
            )
        return f"DELETE FROM cache_bench_mutable WHERE id = {trial + 2}"

    query = "SELECT id, group_id, payload FROM cache_bench_mutable ORDER BY id"
    for key in ("insert", "update", "delete"):
        trial_reports: list[dict[str, Any]] = []
        producers: list[QueryOutput] = []
        post_write_outputs: list[QueryOutput] = []
        for trial in range(trials):
            _reset_case(cluster)
            pre_write_reference = _execute_query(
                cluster, mode="disabled", sql=query
            )
            producer = _execute_query(cluster, mode="same", sql=query)
            before = _stats(cluster)
            operation = operation_sql(key, trial)
            started = time.perf_counter_ns()
            _run_sql(cluster, operation)
            invalidation_ns = time.perf_counter_ns() - started
            reference = _execute_query(cluster, mode="disabled", sql=query)
            output = _execute_query(cluster, mode="same", sql=query)
            after = _stats(cluster)
            producers.append(producer)
            post_write_outputs.append(output)
            trial_reports.append(
                {
                    "trial": trial + 1,
                    "operation": operation,
                    "operation_ns": invalidation_ns,
                    "producer_correctness_passed": _same_result(
                        pre_write_reference, producer, True
                    ),
                    "post_write_correctness_passed": _same_result(
                        reference, output, True
                    ),
                    "pre_write_row_count": pre_write_reference.rows,
                    "pre_write_ordered_sha256": (
                        pre_write_reference.ordered_sha256
                    ),
                    "row_count": reference.rows,
                    "ordered_sha256": reference.ordered_sha256,
                    "multiset_sha256": reference.multiset_sha256,
                    "invalidation_observed": (
                        after["produced"] == before["produced"] + 1
                        and after["completed_hits"]
                            == before["completed_hits"]
                        and after["fallbacks"] == before["fallbacks"]
                        and after["invalidations"] > before["invalidations"]
                    ),
                    "stats_before_write": before,
                    "stats_after_write": after,
                }
            )
        operation_samples = [
            int(trial["operation_ns"]) for trial in trial_reports
        ]
        results.append(
            {
                "key": f"{key}_invalidation",
                "trials": trial_reports,
                "operation_latency": _sample_summary(operation_samples),
                "operation_ns": _nearest_rank_percentile(
                    operation_samples, 50, 100
                ),
                "correctness": {
                    "passed": all(
                        bool(trial["producer_correctness_passed"])
                        and bool(trial["post_write_correctness_passed"])
                        for trial in trial_reports
                    ),
                    "oracle": (
                        "independent disabled executions before and after each write"
                    ),
                    "pre_write_row_counts": [
                        int(trial["pre_write_row_count"])
                        for trial in trial_reports
                    ],
                    "pre_write_ordered_sha256": [
                        str(trial["pre_write_ordered_sha256"])
                        for trial in trial_reports
                    ],
                    "row_counts": [
                        int(trial["row_count"]) for trial in trial_reports
                    ],
                    "ordered_sha256": [
                        str(trial["ordered_sha256"])
                        for trial in trial_reports
                    ],
                    "multiset_sha256": [
                        str(trial["multiset_sha256"])
                        for trial in trial_reports
                    ],
                },
                "producer_time_to_first_row": _first_row_summary(producers),
                "post_write_time_to_first_row": _first_row_summary(
                    post_write_outputs
                ),
                "invalidation_observed": all(
                    bool(trial["invalidation_observed"])
                    for trial in trial_reports
                ),
                "runtime_storage": _runtime_storage_snapshot(cluster),
            }
        )
    return results


def _measure_unrelated_commit(cluster: Any, sql: str) -> dict[str, Any]:
    def insert_unrelated() -> int:
        started = time.perf_counter_ns()
        _run_sql(
            cluster,
            "INSERT INTO cache_bench_unrelated "
            "SELECT COALESCE(max(id), 0) + 1 FROM cache_bench_unrelated",
        )
        return time.perf_counter_ns() - started

    # First prove that a generic completed entry is not reused across an
    # unrelated committed write in the database-wide conservative domain.
    _reset_case(cluster)
    reference = _execute_query(cluster, mode="disabled", sql=sql)
    producer = _execute_query(cluster, mode="same", sql=sql)
    before = _stats(cluster)
    commit_ns = insert_unrelated()
    after_commit = _execute_query(cluster, mode="same", sql=sql)
    after = _stats(cluster)
    completed_rejection = (
        after["produced"] == before["produced"] + 1
        and after["completed_hits"] == before["completed_hits"]
        and after["fallbacks"] == before["fallbacks"]
        and after["invalidations"] > before["invalidations"]
    )

    # Then pin the producer immediately before terminal publication. The
    # unrelated commit must reject that publication; a later request produces
    # once, and only its successor may be a completed hit.
    _reset_case(cluster)
    paused_reference = _execute_query(cluster, mode="disabled", sql=sql)
    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor, \
            _PauseController(cluster, "before_terminal_publish") as pause:
        paused_producer = executor.submit(
            _execute_query, cluster, mode="same", sql=sql
        )
        _run_sql(
            cluster,
            "SELECT bioseq_cache_test.wait_until_paused("
            "'before_terminal_publish')",
        )
        paused_commit_ns = insert_unrelated()
        pause.release()
        paused_output = paused_producer.result()
    rejected_stats = _stats(cluster)
    complete_stdout, _ = _run_sql(
        cluster,
        "SELECT count(*) FROM bioseq_cache_test.entry_states() "
        "WHERE state = 'COMPLETE'",
    )
    rejected_complete_entries = int(complete_stdout.strip())
    fresh_output = _execute_query(cluster, mode="same", sql=sql)
    hit_output = _execute_query(cluster, mode="same", sql=sql)
    final_stats = _stats(cluster)
    paused_correctness = all(
        _same_result(paused_reference, output, False)
        for output in (paused_output, fresh_output, hit_output)
    )
    paused_rejection = (
        rejected_complete_entries == 0
        and rejected_stats["produced"] == 0
        and rejected_stats["completed_hits"] == 0
        and rejected_stats["fallbacks"] == 1
        and final_stats["produced"] == 1
        and final_stats["completed_hits"] == 1
        and final_stats["fallbacks"] == 1
    )
    return {
        "key": "conservative_unrelated_commit",
        "correctness": {
            "passed": (
                _same_result(reference, producer, False)
                and _same_result(reference, after_commit, False)
                and paused_correctness
            ),
            "row_count": reference.rows,
            "multiset_sha256": reference.multiset_sha256,
        },
        "completed_entry_rejection": {
            "unrelated_commit_ns": commit_ns,
            "producer_time_to_first_row": _first_row_summary([producer]),
            "post_commit_time_to_first_row": _first_row_summary(
                [after_commit]
            ),
            "passed": completed_rejection,
            "stats_before_commit": before,
            "stats_after_commit": after,
        },
        "paused_publication_rejection": {
            "unrelated_commit_ns": paused_commit_ns,
            "producer_time_to_first_row": _first_row_summary(
                [paused_output]
            ),
            "fresh_producer_time_to_first_row": _first_row_summary(
                [fresh_output]
            ),
            "completed_hit_time_to_first_row": _first_row_summary(
                [hit_output]
            ),
            "complete_entries_after_rejected_publish": (
                rejected_complete_entries
            ),
            "stats_after_rejected_publish": rejected_stats,
            "final_stats": final_stats,
            "passed": paused_rejection and paused_correctness,
        },
        "conservative_rejection_observed": (
            completed_rejection and paused_rejection
        ),
        "runtime_storage": _runtime_storage_snapshot(cluster),
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
    correct_request_count = 0
    request_oracle = hashlib.sha256()

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
        request_correct = _same_result(reference, candidate, True)
        all_correct = all_correct and request_correct
        correct_request_count += int(request_correct)
        request_oracle.update(
            (
                f"{index + 1}\0{reference.rows}\0"
                f"{reference.ordered_sha256}\0{candidate.rows}\0"
                f"{candidate.ordered_sha256}\n"
            ).encode("ascii")
        )
        request_latencies.append(candidate.elapsed_ns)
        request_first_rows.append(candidate)
        request_count = index + 1
        if request_count % batch_size == 0:
            snapshot = _stats(cluster)
            if request_count >= minimum_requests and snapshot["evictions"] > 0:
                break

    fill_stats = _stats(cluster)
    fill_runtime = _runtime_storage_snapshot(cluster)

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
            "generated_request_count": request_count,
            "correct_generated_request_count": correct_request_count,
            "request_oracle_sha256": request_oracle.hexdigest(),
            "first_ordered_sha256": first_reference.ordered_sha256,
            "last_ordered_sha256": last_reference.ordered_sha256,
        },
        "fill_latency": _sample_summary(request_latencies),
        "fill_time_to_first_row": _first_row_summary(request_first_rows),
        "eviction_observed": fill_stats["evictions"] > 0,
        "trigger_query": {
            **_sample_summary([trigger_output.elapsed_ns]),
            "eviction_time": {
                **_sample_summary([trigger_output.elapsed_ns]),
                "detail": (
                    "Client wall time for the request that advances the "
                    "eviction counter; no isolated server eviction timer is "
                    "currently exposed."
                ),
            },
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
    ttl_ms = 1_000
    _reset_case(cluster, ttl_ms=ttl_ms)
    reference = _execute_query(cluster, mode="disabled", sql=sql)
    producer = _execute_query(
        cluster, mode="same", sql=sql, ttl_ms=ttl_ms
    )
    complete_stdout, _ = _run_sql(
        cluster,
        "SELECT count(*) FROM bioseq_cache_test.entry_states() "
        "WHERE state = 'COMPLETE'",
    )
    complete_entries = int(complete_stdout.strip())
    pre_expiry_hit = _execute_query(
        cluster, mode="same", sql=sql, ttl_ms=ttl_ms
    )
    pre_expiry_stats = _stats(cluster)
    pre_expiry_passed = (
        complete_entries >= 1
        and _same_result(reference, producer, False)
        and _same_result(reference, pre_expiry_hit, False)
        and pre_expiry_stats["produced"] == 1
        and pre_expiry_stats["completed_hits"] == 1
        and pre_expiry_stats["fallbacks"] == 0
    )

    expiry_wait_started = time.perf_counter_ns()
    time.sleep(ttl_ms / 1000.0 + 0.10)
    expiry_wait_ns = time.perf_counter_ns() - expiry_wait_started
    before_cleanup = _stats(cluster)
    cleanup_started = time.perf_counter_ns()
    reclamation = _wait_for_storage_reclamation(cluster)
    cleanup_ns = time.perf_counter_ns() - cleanup_started
    stats_after = _stats(cluster)
    report = {
        "key": "ttl_cleanup",
        "ttl_ms": ttl_ms,
        "correctness": {
            "passed": (
                _same_result(reference, producer, False)
                and _same_result(reference, pre_expiry_hit, False)
            ),
            "row_count": reference.rows,
            "multiset_sha256": reference.multiset_sha256,
        },
        "producer_time_to_first_row": _first_row_summary([producer]),
        "pre_expiry_hit_time_to_first_row": _first_row_summary(
            [pre_expiry_hit]
        ),
        "pre_expiry": {
            "passed": pre_expiry_passed,
            "complete_entries": complete_entries,
            "stats": pre_expiry_stats,
        },
        "expiry_wait_ns": expiry_wait_ns,
        "cleanup_ns": cleanup_ns,
        "eviction_ns": cleanup_ns,
        "entries_remaining": reclamation["complete_entries"],
        "reclamation": reclamation,
        "stats_before": before_cleanup,
        "stats_after": stats_after,
        "runtime_storage_after": _runtime_storage_snapshot(cluster),
    }
    report["passed"] = _ttl_cleanup_contract_passed(report)
    return report


def _wait_for_storage_reclamation(
    cluster: Any, timeout_seconds: float = 20.0
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_seconds
    latest: dict[str, Any] = {}
    while time.monotonic() < deadline:
        _run_sql(cluster, "SELECT bioseq_cache_test.request_janitor_sweep()")
        stdout, _ = _run_sql(
            cluster,
            "SELECT count(*) FROM bioseq_cache_test.entry_states() "
            "WHERE state = 'COMPLETE'",
        )
        tier = _test_storage_tier_snapshot(cluster)
        store = _cache_store_usage(cluster)
        latest = {
            "complete_entries": int(stdout.strip()),
            "tier": tier,
            "store": store,
        }
        if (
            latest["complete_entries"] == 0
            and tier["dsa_blocks"] == 0
            and tier["file_blocks"] == 0
            and tier["reserved_hot_bytes"] == 0
            and tier["reserved_file_bytes"] == 0
            and store["file_count"] == 0
            and store["bytes"] == 0
        ):
            return latest
        time.sleep(0.05)
    raise BenchmarkError(
        f"cache storage was not reclaimed within {timeout_seconds}s: {latest!r}"
    )


def _measure_repeated_eviction_cycles(cluster: Any) -> dict[str, Any]:
    cycle_count = 3
    entries_per_cycle = 8
    ttl_ms = 500
    cycles: list[dict[str, Any]] = []
    _reset_case(cluster, ttl_ms=ttl_ms)
    _wait_for_storage_reclamation(cluster)

    for cycle in range(cycle_count):
        outputs: list[QueryOutput] = []
        references: list[QueryOutput] = []
        with _ProcessResourceSampler(cluster) as resources:
            _run_sql(
                cluster, "SELECT bioseq_cache_test.set_force_file_tier(true)"
            )
            try:
                for entry in range(entries_per_cycle):
                    first_id = cycle * 512 + entry * 32 + 1
                    sql = (
                        "SELECT id, group_id, payload "
                        "FROM cache_bench_narrow WHERE id BETWEEN "
                        f"{first_id} AND {first_id + 31} ORDER BY id"
                    )
                    reference = _execute_query(
                        cluster, mode="disabled", sql=sql
                    )
                    output = _execute_query(
                        cluster,
                        mode="same",
                        sql=sql,
                        ttl_ms=ttl_ms,
                    )
                    references.append(reference)
                    outputs.append(output)
                peak_tier = _test_storage_tier_snapshot(cluster)
                peak_store = _cache_store_usage(cluster)
                peak_filesystem = _filesystem_policy_snapshot(cluster)
            finally:
                _run_sql(
                    cluster,
                    "SELECT bioseq_cache_test.set_force_file_tier(false)",
                )

            time.sleep(ttl_ms / 1000.0 + 0.10)
            cleanup_started = time.perf_counter_ns()
            reclaimed = _wait_for_storage_reclamation(cluster)
            cleanup_ns = time.perf_counter_ns() - cleanup_started
            after_cleanup = _process_resource_snapshot(cluster)
        resource_report = resources.report()
        correctness = all(
            _same_result(reference, output, True)
            for reference, output in zip(references, outputs)
        )
        cycles.append(
            {
                "cycle": cycle + 1,
                "entries": entries_per_cycle,
                "correctness": {
                    "passed": correctness,
                    "row_counts": [output.rows for output in outputs],
                    "ordered_sha256": [
                        output.ordered_sha256 for output in outputs
                    ],
                },
                "peak_tier": peak_tier,
                "peak_store": peak_store,
                "peak_filesystem_policy": peak_filesystem,
                "cleanup_ns": cleanup_ns,
                "post_cleanup": reclaimed,
                "post_cleanup_resources": after_cleanup,
                "resource_interval": resource_report,
            }
        )

    first_resources = cycles[0]["resource_interval"]
    resources_measured = all(
        cycle["resource_interval"]["status"] == "measured"
        for cycle in cycles
    )
    rss_limits: dict[str, int] = {}
    rss_bounded = False
    if resources_measured:
        rss_limits = {
            "peak_postmaster_rss_bytes": int(
                first_resources["peak_postmaster_rss_bytes"]
                + max(
                    16 << 20,
                    first_resources["peak_postmaster_rss_bytes"] // 4,
                )
            ),
            "peak_backend_rss_bytes": int(
                first_resources["peak_backend_rss_bytes"]
                + max(
                    32 << 20,
                    first_resources["peak_backend_rss_bytes"] // 4,
                )
            ),
            "peak_process_tree_rss_bytes": int(
                first_resources["peak_process_tree_rss_bytes"]
                + max(
                    64 << 20,
                    first_resources["peak_process_tree_rss_bytes"] // 4,
                )
            ),
        }
        rss_bounded = all(
            all(
                int(cycle["resource_interval"][name]) <= limit
                for cycle in cycles[1:]
            )
            for name, limit in rss_limits.items()
        )

    storage_reclaimed = all(
        cycle["peak_tier"]["file_blocks"] > 0
        and cycle["peak_tier"]["reserved_file_bytes"] > 0
        and cycle["peak_store"]["file_count"] > 0
        and cycle["post_cleanup"]["tier"]["dsa_blocks"] == 0
        and cycle["post_cleanup"]["tier"]["file_blocks"] == 0
        and cycle["post_cleanup"]["tier"]["reserved_hot_bytes"] == 0
        and cycle["post_cleanup"]["tier"]["reserved_file_bytes"] == 0
        and cycle["post_cleanup"]["store"]["file_count"] == 0
        and cycle["post_cleanup"]["store"]["bytes"] == 0
        for cycle in cycles
    )
    disk_policy_preserved = all(
        cycle["peak_filesystem_policy"]["free_space_floor_preserved"]
        and cycle["peak_store"]["bytes"]
            <= cycle["peak_filesystem_policy"]["policy_file_ceiling_bytes"]
        for cycle in cycles
    )
    correctness = all(
        bool(cycle["correctness"]["passed"]) for cycle in cycles
    )
    return {
        "cycles": cycles,
        "rss_growth_limits": rss_limits,
        "rss_bounded": rss_bounded,
        "storage_reclaimed": storage_reclaimed,
        "disk_policy_preserved": disk_policy_preserved,
        "correctness": {"passed": correctness},
        "passed": (
            correctness
            and storage_reclaimed
            and disk_policy_preserved
            and resources_measured
            and rss_bounded
        ),
        "detail": (
            "Three equal forced-file-tier generations are expired and swept. "
            "Every cycle must return file/DSA reservations and physical files "
            "to zero, preserve the internal file ceiling/free-space floor, "
            "and keep later-cycle process peaks bounded relative to the fully "
            "warmed first cycle."
        ),
    }


def _setup_fixture(cluster: Any, rows: int) -> None:
    provider_partition_rows = max(512, rows // 4)
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

    CREATE TABLE cache_bench_provider_rows(
      row_id bigint PRIMARY KEY,
      base_id bigint NOT NULL,
      occurrence_no integer NOT NULL,
      nullable_value integer,
      payload text NOT NULL,
      capability_mask bigint NOT NULL,
      UNIQUE(base_id, occurrence_no));
    INSERT INTO cache_bench_provider_rows
    SELECT i,
           (i + 1) / 2,
           ((i - 1) % 2)::integer,
           CASE WHEN i % 17 = 0 THEN NULL ELSE (i % 101) - 50 END,
           repeat(md5(i::text), 2),
           3
    FROM generate_series(1, {rows}) AS g(i);

    CREATE TABLE cache_bench_provider_partitioned(
      row_id bigint NOT NULL,
      part integer NOT NULL,
      base_id bigint NOT NULL,
      occurrence_no integer NOT NULL,
      nullable_value integer,
      payload text NOT NULL,
      capability_mask bigint NOT NULL
    ) PARTITION BY LIST(part);
    CREATE TABLE cache_bench_provider_part_a
      PARTITION OF cache_bench_provider_partitioned FOR VALUES IN (1);
    CREATE TABLE cache_bench_provider_part_b
      PARTITION OF cache_bench_provider_partitioned FOR VALUES IN (2);
    INSERT INTO cache_bench_provider_partitioned
    SELECT i,
           1,
           i,
           0,
           CASE WHEN i % 17 = 0 THEN NULL ELSE (i % 101) - 50 END,
           repeat(md5('a-' || i::text), 2),
           3
    FROM generate_series(1, {provider_partition_rows}) AS g(i);
    INSERT INTO cache_bench_provider_partitioned
    SELECT {provider_partition_rows} + i,
           2,
           {provider_partition_rows} + i,
           0,
           CASE WHEN i % 17 = 0 THEN NULL ELSE (i % 101) - 50 END,
           repeat(md5('b-' || i::text), 2),
           3
    FROM generate_series(1, {provider_partition_rows}) AS g(i);

    ANALYZE cache_bench_narrow;
    ANALYZE cache_bench_wide;
    ANALYZE cache_bench_toast;
    ANALYZE cache_bench_provider_rows;
    ANALYZE cache_bench_provider_part_a;
    ANALYZE cache_bench_provider_part_b;
    """
    _run_sql(cluster, setup_sql)


def _git_output(arguments: Sequence[str]) -> str:
    completed = subprocess.run(
        ["git", "-C", str(REPOSITORY_ROOT), *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        raise BenchmarkError(
            f"git {' '.join(arguments)} failed: {completed.stdout}"
        )
    return completed.stdout


def _git_metadata() -> dict[str, Any]:
    commit = _git_output(("rev-parse", "HEAD")).strip()
    status = _git_output(("status", "--porcelain=v1", "--untracked-files=normal"))
    tracked_diff = _git_output(("diff", "--binary", "HEAD", "--"))
    return {
        "commit": commit,
        "worktree_dirty": status != "",
        "porcelain_status": status.splitlines(),
        "tracked_diff_sha256": hashlib.sha256(
            tracked_diff.encode("utf-8")
        ).hexdigest(),
        "detail": (
            "The commit identifies HEAD; status and the tracked-diff digest "
            "identify an intentionally uncommitted benchmark candidate."
        ),
    }


def _cmake_assignment(document: str, name: str) -> str | None:
    match = re.search(
        rf'^set\({re.escape(name)}\s+"?([^"\)]+)"?\)$',
        document,
        flags=re.MULTILINE,
    )
    return match.group(1) if match is not None else None


def _build_tree_metadata(cache_dso: Path) -> dict[str, Any]:
    build_directory = next(
        (
            parent
            for parent in (cache_dso.parent, *cache_dso.parents)
            if (parent / "CMakeCache.txt").is_file()
        ),
        None,
    )
    if build_directory is None:
        raise BenchmarkError(
            f"cannot locate the CMake build tree containing {cache_dso}"
        )
    compiler_documents = list(
        (build_directory / "CMakeFiles").glob(
            "*/CMakeCXXCompiler.cmake"
        )
    )
    if not compiler_documents:
        raise BenchmarkError(
            f"CMake C++ compiler metadata is absent from {build_directory}"
        )
    compiler_metadata_path = max(
        compiler_documents, key=lambda path: path.stat().st_mtime_ns
    )
    compiler_document = compiler_metadata_path.read_text(encoding="utf-8")
    compiler_path = _cmake_assignment(
        compiler_document, "CMAKE_CXX_COMPILER"
    )
    compiler_id = _cmake_assignment(
        compiler_document, "CMAKE_CXX_COMPILER_ID"
    )
    compiler_version = _cmake_assignment(
        compiler_document, "CMAKE_CXX_COMPILER_VERSION"
    )
    if not compiler_path or not compiler_id or not compiler_version:
        raise BenchmarkError(
            "CMake C++ compiler metadata is incomplete: "
            f"{compiler_metadata_path}"
        )
    return {
        "build_directory": str(build_directory),
        "effective_preset": build_directory.name,
        "compiler": {
            "id": compiler_id,
            "version": compiler_version,
            "path": compiler_path,
            "display": f"{compiler_id} {compiler_version} ({compiler_path})",
        },
        "cmake_compiler_metadata": str(compiler_metadata_path),
        "requested_preset_environment": (
            os.environ.get("CMAKE_PRESET_NAME")
            or os.environ.get("BIOSEQ_BUILD_PRESET")
            or None
        ),
    }


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
    disabled_overhead: Mapping[str, Any],
    long_source_exact: Mapping[str, Any],
    cases: Sequence[Mapping[str, Any]],
    generic_provider: Mapping[str, Any],
    fanout: Sequence[Mapping[str, Any]],
    waiter_cancel: Mapping[str, Any],
    invalidation: Sequence[Mapping[str, Any]],
    eviction_pressure: Mapping[str, Any],
    unrelated_commit: Mapping[str, Any],
    ttl: Mapping[str, Any],
    repeated_eviction: Mapping[str, Any],
    resource_usage: Mapping[str, Any],
    disk_snapshot: Mapping[str, Any],
) -> list[dict[str, Any]]:
    thresholds: list[dict[str, Any]] = []
    planning_overhead = float(
        disabled_overhead["disabled_planning_overhead_percent"]
    )
    thresholds.append(
        _threshold(
            "disabled_planning_overhead_percent",
            actual=planning_overhead,
            limit="<5",
            passed=planning_overhead < 5.0,
            detail=(
                "Measured no-hook versus disabled-hook private-node "
                "microplanning medians; this threshold is mandatory."
            ),
        )
    )
    execution_overhead = float(
        disabled_overhead["disabled_execution_overhead_percent"]
    )
    long_source = bool(
        disabled_overhead["source_execution_exceeds_one_second"]
    )
    thresholds.append(
        _threshold(
            "disabled_execution_overhead_percent",
            actual=execution_overhead,
            limit="<1 for source execution >=1s",
            passed=long_source and execution_overhead < 1.0,
            detail=(
                "Measured no-hook versus disabled-hook execution medians after "
                "calibrating the source above one second; this threshold is mandatory."
            ),
        )
    )
    disabled_evidence = disabled_overhead["disabled_hook"]
    thresholds.append(
        _threshold(
            "disabled_no_cache_plan_and_correct_result",
            actual={
                "planning_cache_plan_count": disabled_evidence[
                    "planning_cache_plan_count"
                ],
                "execution_cache_plan_count": disabled_evidence[
                    "execution_cache_plan_count"
                ],
                "correctness": disabled_evidence["correctness"],
            },
            limit="zero cache plans and checksum equality",
            passed=(
                int(disabled_evidence["planning_cache_plan_count"]) == 0
                and int(disabled_evidence["execution_cache_plan_count"]) == 0
                and bool(disabled_evidence["correctness"]["passed"])
            ),
            detail=(
                "The disabled comparison is rejected if either structured "
                "EXPLAIN contains a cache node or the long-query checksum differs."
            ),
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
            detail=(
                "Paired p50 client wall time across independently reset cold "
                "same-mode producers versus disabled references."
            ),
        )
    )
    source_ns = int(long_source_exact["disabled"]["p50_ns"])
    hit_speedup = float(
        long_source_exact["completed_hit"]["speedup_vs_disabled"] or 0.0
    )
    thresholds.append(
        _threshold(
            "completed_hit_speedup",
            actual=hit_speedup,
            limit=">=5 with a measured source execution >=1s",
            passed=(
                source_ns >= 1_000_000_000
                and bool(long_source_exact["source_execution_exceeds_one_second"])
                and hit_speedup >= 5.0
                and bool(long_source_exact["correctness"]["passed"])
                and bool(long_source_exact["cache_behavior"]["passed"])
            ),
            detail=(
                "The calibrated one-second compact result makes this threshold "
                "mandatory rather than conditionally not applicable."
            ),
        )
    )
    child_counts_measured = all(
        item["child_execution_count"]["status"] == "measured"
        for item in fanout
    )
    fanout_passed = (
        all(item["child_execution_count"]["value"] == 1 for item in fanout)
        if child_counts_measured
        else False
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
                "execution measurements. Sign-off requires a measured executor "
                "counter for every fanout size."
            ),
        )
    )
    thresholds.append(
        _threshold(
            "fanout_request_locking_behavior",
            actual={
                item["clients"]: item["cache_behavior"] for item in fanout
            },
            limit=(
                "one producer, clients-1 in-flight waits, no completed-hit "
                "or fallback substitution, and correct rows for 2/8/32 clients"
            ),
            passed=all(
                bool(item["cache_behavior"]["passed"])
                and bool(item["correctness"]["passed"])
                for item in fanout
            ),
            detail=(
                "The child counter is paired with request-family counters so "
                "an accidental passthrough cannot satisfy the fanout gate."
            ),
        )
    )
    wait_duration_passed = all(
        item["wait_duration"]["status"] == "bounded_measurement"
        and item["wait_duration"]["condition_wait_lower_bound_ns"]
            >= int(item["wait_duration"]["controlled_hold_target_ns"] * 0.9)
        for item in fanout
    )
    thresholds.append(
        _threshold(
            "fanout_wait_duration_measured",
            actual={
                item["clients"]: item["wait_duration"] for item in fanout
            },
            limit="measured wait bounds include at least 90% of a 50 ms hold",
            passed=wait_duration_passed,
            detail=(
                "Registry waiter observation precedes the controlled hold, so "
                "observed-to-release is a conservative condition-wait lower bound."
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
    correctness_passed = correctness_passed and bool(
        generic_provider["correctness"]["passed"]
    )
    correctness_passed = correctness_passed and bool(
        repeated_eviction["correctness"]["passed"]
    )
    correctness_passed = correctness_passed and bool(
        long_source_exact["correctness"]["passed"]
    )
    correctness_passed = correctness_passed and bool(
        waiter_cancel["correctness"]["passed"]
    )
    correctness_passed = correctness_passed and bool(
        ttl["correctness"]["passed"]
    )
    correctness_passed = correctness_passed and bool(
        disabled_overhead["disabled_hook"]["correctness"]["passed"]
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
            actual={
                "completed_entry": unrelated_commit[
                    "completed_entry_rejection"
                ],
                "paused_publication": unrelated_commit[
                    "paused_publication_rejection"
                ],
            },
            limit=(
                "post-commit execution misses and a producer spanning the "
                "unrelated commit cannot publish"
            ),
            passed=bool(unrelated_commit["conservative_rejection_observed"]),
            detail=(
                "The database-wide generic transition domain is checked both "
                "before lookup and atomically at terminal publication."
            ),
        )
    )
    thresholds.append(
        _threshold(
            "ttl_cleanup_bounded",
            actual={
                "ttl_ms": ttl.get("ttl_ms"),
                "pre_expiry": ttl.get("pre_expiry"),
                "expiry_wait_ns": ttl.get("expiry_wait_ns"),
                "cleanup_ns": ttl.get("cleanup_ns"),
                "reclamation": ttl.get("reclamation"),
            },
            limit=(
                "one usable COMPLETE hit before expiry, a full TTL wait, "
                "then zero registry entries, storage reservations, blocks, "
                "and files after a bounded sweep"
            ),
            passed=_ttl_cleanup_contract_passed(ttl),
            detail=(
                "The benchmark proves publication and a completed hit before "
                "waiting beyond the configured TTL, then requests cleanup and "
                "waits for both registry and physical storage reclamation."
            ),
        )
    )
    case_disk_snapshots = {
        case["key"]: case["runtime_storage"]["filesystem_policy"]
        for case in cases
    }
    cycle_disk_snapshots = {
        f"cycle_{cycle['cycle']}": cycle["peak_filesystem_policy"]
        for cycle in repeated_eviction["cycles"]
    }
    all_disk_snapshots = {
        "report_end": dict(disk_snapshot),
        **case_disk_snapshots,
        **cycle_disk_snapshots,
    }
    internal_disk_policy_passed = all(
        bool(snapshot["free_space_floor_preserved"])
        for snapshot in all_disk_snapshots.values()
    ) and all(
        case["runtime_storage"]["test_tier_evidence"][
            "reserved_file_bytes"
        ]
        <= case["runtime_storage"]["filesystem_policy"][
            "policy_file_ceiling_bytes"
        ]
        for case in cases
    ) and bool(repeated_eviction["disk_policy_preserved"])
    thresholds.append(
        _threshold(
            "internal_disk_ceiling_and_free_space_floor",
            actual=all_disk_snapshots,
            limit=(
                "every measured point preserves max(10 GiB, 10% capacity) "
                "free and forced-file cycles stay below min(128 GiB, 10%)"
            ),
            passed=internal_disk_policy_passed,
            detail=(
                "The v1 internal policy is checked during every SQL case, at "
                "each forced-file peak, and after the complete run. The separate "
                "structured-storage gate checks configured reservation ceilings."
            ),
        )
    )
    compact_storage = compact["completed_hit"]["cache_storage"]
    toast = next(case for case in cases if case["key"] == "large_toasted_values")
    toast_storage = toast["completed_hit"]["cache_storage"]
    compact_tier_evidence = compact["runtime_storage"]["test_tier_evidence"]
    toast_tier_evidence = toast["runtime_storage"]["test_tier_evidence"]
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
        and not compact_tier_evidence["force_file_tier"]
        and compact_tier_evidence["dsa_blocks"] > 0
        and toast_tier_evidence["force_file_tier"]
        and toast_tier_evidence["file_blocks"] > 0
    )
    thresholds.append(
        _threshold(
            "internal_storage_tier_metrics",
            actual={
                "compact": compact_storage,
                "large_toasted": toast_storage,
                "compact_test_evidence": compact_tier_evidence,
                "large_toasted_test_evidence": toast_tier_evidence,
            },
            limit=(
                "all reservations within ceilings; compact observes DSA and "
                "large TOAST observes file tier"
            ),
            passed=tiers_bounded and tier_cases_observed,
            detail=(
                "Ceilings come from structured EXPLAIN after mandatory hit "
                "prevalidation; test-only ledger counts prove both physical "
                "backing branches without adding a production GUC."
            ),
        )
    )
    file_hit_temperature = toast["file_hit_temperature"]
    file_prevalidation = file_hit_temperature.get(
        "full_block_prevalidation", {}
    )
    file_checksum = (
        file_prevalidation.get("validated_result_checksum", {})
        if isinstance(file_prevalidation, Mapping) else {}
    )
    first_cold_file_hit = file_hit_temperature.get(
        "first_cold_file_hit", {}
    )
    cold_physical_io_passed = (
        isinstance(first_cold_file_hit, Mapping)
        and _cold_physical_io_measured(
            first_cold_file_hit.get("process_resources")
        )
    )
    file_prevalidation_passed = (
        isinstance(file_prevalidation, Mapping)
        and file_prevalidation.get("status") == "measured"
        and file_prevalidation.get("passed") is True
        and _is_measured_integer_metric(
            file_prevalidation.get("bytes"), minimum=1
        )
        and _is_measured_integer_metric(
            file_prevalidation.get("duration_ns"), minimum=1
        )
        and _is_measured_integer_metric(
            file_prevalidation.get("row_group_count"), minimum=1
        )
        and isinstance(file_checksum, Mapping)
        and file_checksum.get("status") == "measured"
        and file_checksum.get("passed") is True
    )
    thresholds.append(
        _threshold(
            "cold_and_warm_file_hit_with_checksum_prepass",
            actual=file_hit_temperature,
            limit=(
                "fsync+DONTNEED cold hit and later warm hits are correct; "
                "all timings include full checksum validation"
            ),
            passed=(
                file_hit_temperature.get("status") == "measured"
                and file_hit_temperature.get("passed") is True
                and file_prevalidation_passed
                and cold_physical_io_passed
            ),
            detail=str(file_hit_temperature["detail"]),
        )
    )
    decode_bandwidth = _core_metric(core, "fixed_decode_bytes_per_second")
    memcpy_bandwidth = _core_metric(core, "memory_copy_bytes_per_second")
    decode_passed = (
        decode_bandwidth is not None
        and memcpy_bandwidth is not None
        and decode_bandwidth >= memcpy_bandwidth
    )
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
            "generic_reusable_current_hit",
            actual=generic_provider["current_behavior"],
            limit=(
                "one reusable publication; identical current requests read it "
                "without provider executor work"
            ),
            passed=bool(generic_provider["current_behavior"]["passed"]),
            detail=(
                "Cache and backend-local provider counter deltas are measured "
                "beside ordered row/checksum validation."
            ),
        )
    )
    overlap = generic_provider["overlapping_predicate_reuse"]
    overlap_passed = bool(
        overlap["status"] == "measured"
        and overlap["behavior"]["passed"]
    )
    thresholds.append(
        _threshold(
            "generic_overlapping_predicate_reuse",
            actual=overlap["behavior"],
            limit=(
                "mapped older-source reuse must preserve the ordered bag and "
                "reduce provider residual work"
            ),
            passed=overlap_passed,
            detail=overlap["detail"],
        )
    )
    thresholds.append(
        _threshold(
            "provider_partition_fragment_reuse",
            actual=generic_provider["partition_fragment_reuse"]["behavior"],
            limit="unaffected provider partition remains reusable",
            passed=bool(
                generic_provider["partition_fragment_reuse"]["behavior"][
                    "passed"
                ]
            ),
            detail=(
                "Measured with the deterministic cache-only generic provider: "
                "a committed write to leaf B must leave leaf A's reusable "
                "generation readable without provider executor work."
            ),
        )
    )
    thresholds.append(
        _threshold(
            "waiter_cancel_producer_latency",
            actual={
                "baseline": waiter_cancel["baseline_release_to_complete"],
                "cancelled": waiter_cancel["cancelled_release_to_complete"],
                "p95_delta_ns": waiter_cancel["p95_delta_ns"],
                "absolute_p95_delta_ns": waiter_cancel[
                    "absolute_p95_delta_ns"
                ],
            },
            limit={
                "absolute_p95_delta_ns": (
                    f"<={waiter_cancel['tolerance_ns']}"
                )
            },
            passed=bool(waiter_cancel["passed"]),
            detail=str(waiter_cancel["detail"]),
        )
    )
    resources_measured = (
        resource_usage.get("status") == "measured"
        and int(resource_usage.get("peak_postmaster_rss_bytes", 0)) > 0
        and int(resource_usage.get("peak_backend_rss_bytes", 0)) > 0
        and resource_usage.get("page_cache_and_physical_io", {}).get("status")
        == "measured"
    )
    thresholds.append(
        _threshold(
            "backend_postmaster_rss_and_physical_io_measured",
            actual=resource_usage,
            limit="measured nonzero RSS peaks and Linux physical-I/O counters",
            passed=resources_measured,
            detail=(
                "A high-frequency /proc sampler covers the private PostgreSQL "
                "process tree for the complete generic SQL benchmark."
            ),
        )
    )
    thresholds.append(
        _threshold(
            "repeated_eviction_resource_reclamation",
            actual={
                "rss_growth_limits": repeated_eviction["rss_growth_limits"],
                "rss_bounded": repeated_eviction["rss_bounded"],
                "storage_reclaimed": repeated_eviction["storage_reclaimed"],
                "cycles": repeated_eviction["cycles"],
            },
            limit=(
                "three correct forced-file cycles return blocks, reservations, "
                "and files to zero without later-cycle RSS growth"
            ),
            passed=bool(repeated_eviction["passed"]),
            detail=str(repeated_eviction["detail"]),
        )
    )
    return thresholds


def _validate_section45_inventory(
    *,
    core: Mapping[str, Any],
    streaming_protocol: Mapping[str, Any],
    cases: Sequence[Mapping[str, Any]],
    generic_provider: Mapping[str, Any],
    fanout: Sequence[Mapping[str, Any]],
    invalidation: Sequence[Mapping[str, Any]],
    thresholds: Sequence[Mapping[str, Any]],
    resource_usage: Mapping[str, Any],
) -> dict[str, Any]:
    """Reject a report that silently omits a mandatory generic §45 field."""

    if (
        streaming_protocol.get("status") != "measured"
        or streaming_protocol.get("passed") is not True
        or any(
            not isinstance(streaming_protocol.get(name), int)
            or isinstance(streaming_protocol.get(name), bool)
            or streaming_protocol[name] < 0
            for name in (
                "first_row_ns",
                "total_elapsed_ns",
                "post_first_row_ns",
                "minimum_post_first_row_ns",
            )
        )
        or streaming_protocol["total_elapsed_ns"]
            <= streaming_protocol["first_row_ns"]
        or streaming_protocol["post_first_row_ns"]
            != streaming_protocol["total_elapsed_ns"]
                - streaming_protocol["first_row_ns"]
        or streaming_protocol["post_first_row_ns"]
            < streaming_protocol["minimum_post_first_row_ns"]
    ):
        raise BenchmarkError(
            "Section 45 first-row metric lacks a delayed-later-row "
            "streaming protocol proof"
        )

    def require_sample_summary(value: Any, label: str) -> None:
        _validate_sample_summary_contract(
            value,
            label,
            raw_name="raw_samples_ns",
            require_nonempty=True,
        )

    _validate_nested_sample_summary_contracts(cases, "cases")
    _validate_nested_sample_summary_contracts(
        generic_provider, "generic_provider"
    )
    _validate_nested_sample_summary_contracts(fanout, "fanout")
    _validate_nested_sample_summary_contracts(invalidation, "invalidation")
    _validate_nested_sample_summary_contracts(thresholds, "thresholds")

    case_by_key = {
        str(case.get("key")): case
        for case in cases
        if isinstance(case, Mapping)
    }
    if len(case_by_key) != len(cases):
        raise BenchmarkError("Section 45 SQL result cases have duplicate keys")
    missing_cases = sorted(GENERIC_SQL_CASE_KEYS - case_by_key.keys())
    if missing_cases:
        raise BenchmarkError(
            f"Section 45 report omits generic result cases: {missing_cases}"
        )
    for key in sorted(GENERIC_SQL_CASE_KEYS):
        case = case_by_key[key]
        correctness = case.get("correctness")
        digest_name = (
            "ordered_sha256" if bool(case.get("ordered"))
            else "multiset_sha256"
        )
        if (
            not isinstance(correctness, Mapping)
            or type(correctness.get("passed")) is not bool
            or not isinstance(correctness.get("row_count"), int)
            or correctness["row_count"] < 0
            or not isinstance(correctness.get(digest_name), str)
            or not correctness[digest_name]
        ):
            raise BenchmarkError(
                f"Section 45 case lacks its row/checksum oracle: {key}"
            )
        for phase in ("disabled", "producer", "completed_hit"):
            phase_value = case.get(phase)
            require_sample_summary(phase_value, f"{key}.{phase}")
            if (
                not isinstance(phase_value.get("time_to_first_row"), Mapping)
                or not isinstance(phase_value.get("throughput"), Mapping)
            ):
                raise BenchmarkError(
                    f"Section 45 case lacks first-row/throughput metrics: {key}.{phase}"
                )
        disabled = case["disabled"]
        completed_hit = case["completed_hit"]
        require_sample_summary(disabled.get("planning"), f"{key}.planning")
        require_sample_summary(
            completed_hit.get("planning"), f"{key}.hit_planning"
        )
        runtime = case.get("runtime_storage")
        dsa_bytes = (
            runtime.get("dsa_hot_bytes")
            if isinstance(runtime, Mapping) else None
        )
        file_bytes = (
            runtime.get("file_bytes")
            if isinstance(runtime, Mapping) else None
        )
        row_group_count = (
            runtime.get("row_group_count")
            if isinstance(runtime, Mapping) else None
        )
        prevalidation_bytes = (
            runtime.get("prevalidation_bytes")
            if isinstance(runtime, Mapping) else None
        )
        prevalidation_duration = (
            runtime.get("prevalidation_duration_ns")
            if isinstance(runtime, Mapping) else None
        )
        if (
            not isinstance(runtime, Mapping)
            or not _is_measured_integer_metric(
                dsa_bytes,
                minimum=0,
                unit="bytes",
                source_field=(
                    "bioseq_cache_test.storage_tier_stats."
                    "reserved_hot_bytes"
                ),
            )
            or not _is_measured_integer_metric(
                file_bytes,
                minimum=0,
                unit="bytes",
                source_field=(
                    "bioseq_cache_test.storage_tier_stats."
                    "reserved_file_bytes"
                ),
            )
            or not _is_measured_integer_metric(
                row_group_count,
                minimum=0 if key == "empty_result" else 1,
            )
            or not _is_measured_integer_metric(
                prevalidation_bytes, minimum=1
            )
            or not _is_measured_integer_metric(
                prevalidation_duration, minimum=1
            )
        ):
            raise BenchmarkError(
                "Section 45 case lacks measured DSA/file/row-group/"
                f"prevalidation bytes or duration: {key}"
            )

    file_case = case_by_key["large_toasted_values"]
    file_temperature = file_case.get("file_hit_temperature")
    file_prevalidation = (
        file_temperature.get("full_block_prevalidation")
        if isinstance(file_temperature, Mapping) else None
    )
    checksum = (
        file_prevalidation.get("validated_result_checksum")
        if isinstance(file_prevalidation, Mapping) else None
    )
    first_cold_file_hit = (
        file_temperature.get("first_cold_file_hit")
        if isinstance(file_temperature, Mapping) else None
    )
    if (
        not isinstance(file_temperature, Mapping)
        or file_temperature.get("status") != "measured"
        or not isinstance(file_prevalidation, Mapping)
        or file_prevalidation.get("status") != "measured"
        or file_prevalidation.get("passed") is not True
        or not _is_measured_integer_metric(
            file_prevalidation.get("bytes"), minimum=1
        )
        or not _is_measured_integer_metric(
            file_prevalidation.get("duration_ns"), minimum=1
        )
        or not _is_measured_integer_metric(
            file_prevalidation.get("row_group_count"), minimum=1
        )
        or not isinstance(checksum, Mapping)
        or checksum.get("status") != "measured"
        or checksum.get("passed") is not True
        or not isinstance(checksum.get("reference"), str)
        or not checksum["reference"]
        or not isinstance(first_cold_file_hit, Mapping)
        or not _cold_physical_io_measured(
            first_cold_file_hit.get("process_resources")
        )
    ):
        raise BenchmarkError(
            "Section 45 file hit lacks measured full-block bytes, duration, "
            "row groups, checksum evidence, or nonzero cold physical I/O"
        )

    fanout_by_clients = {
        int(item.get("clients", 0)): item
        for item in fanout
        if isinstance(item, Mapping)
    }
    if set(fanout_by_clients) != {2, 8, 32}:
        raise BenchmarkError(
            "Section 45 report requires fanout measurements for 2, 8, and 32 clients"
        )
    for clients, item in fanout_by_clients.items():
        if (
            not isinstance(item.get("correctness"), Mapping)
            or not isinstance(item.get("waiter_latency"), Mapping)
            or not isinstance(item.get("wait_duration"), Mapping)
            or not isinstance(item.get("child_execution_count"), Mapping)
            or not isinstance(item.get("cache_behavior"), Mapping)
        ):
            raise BenchmarkError(
                f"Section 45 fanout evidence is incomplete for {clients} clients"
            )
        require_sample_summary(
            item["waiter_latency"], f"fanout_{clients}.waiter_latency"
        )

    invalidation_by_key = {
        str(item.get("key")): item
        for item in invalidation
        if isinstance(item, Mapping)
    }
    required_invalidation = {
        "insert_invalidation",
        "update_invalidation",
        "delete_invalidation",
    }
    if set(invalidation_by_key) != required_invalidation:
        raise BenchmarkError(
            "Section 45 report requires INSERT/UPDATE/DELETE invalidation cases"
        )
    for key, item in invalidation_by_key.items():
        require_sample_summary(item.get("operation_latency"), key)
        if not isinstance(item.get("correctness"), Mapping):
            raise BenchmarkError(f"Section 45 invalidation oracle is absent: {key}")

    overlap = generic_provider.get("overlapping_predicate_reuse")
    overlap_reuse = overlap.get("reuse") if isinstance(overlap, Mapping) else None
    predicate = (
        overlap_reuse.get("predicate_runtime")
        if isinstance(overlap_reuse, Mapping) else None
    )
    if (
        not isinstance(predicate, Mapping)
        or "late_fetch_rows" not in predicate
        or "late_fetch_bytes" not in predicate
        or "selection_ratio" not in predicate
        or not isinstance(predicate.get("late_fetch_bytes"), Mapping)
        or predicate["late_fetch_bytes"].get("status") != "measured"
    ):
        raise BenchmarkError(
            "Section 45 reusable report omits late-fetch or selection metrics"
        )

    threshold_names = [
        str(item.get("name"))
        for item in thresholds
        if isinstance(item, Mapping)
    ]
    if (
        len(threshold_names) != len(thresholds)
        or len(threshold_names) != len(set(threshold_names))
        or not SECTION45_THRESHOLD_NAMES.issubset(threshold_names)
    ):
        raise BenchmarkError(
            "Section 45 report has missing, duplicate, or malformed threshold decisions"
        )
    for item in thresholds:
        name = str(item.get("name"))
        status = item.get("status")
        passed = item.get("passed")
        if (
            status not in {"pass", "fail", "not_applicable"}
            or "actual" not in item
            or "limit" not in item
            or "detail" not in item
        ):
            raise BenchmarkError(
                f"Section 45 threshold decision is incomplete: {item!r}"
            )
        if name in SECTION45_THRESHOLD_NAMES and status not in {
            "pass",
            "fail",
        }:
            raise BenchmarkError(
                "Section 45 mandatory threshold cannot be not_applicable: "
                f"{name}"
            )
        expected_passed = {
            "pass": True,
            "fail": False,
            "not_applicable": None,
        }[status]
        if passed is not expected_passed:
            raise BenchmarkError(
                "Section 45 threshold status/passed evidence is "
                f"contradictory: {item!r}"
            )

    core_metrics = core.get("metrics")
    if not isinstance(core_metrics, Mapping):
        raise BenchmarkError("Section 45 core storage metrics are absent")
    resource_status = resource_usage.get("status")
    if resource_status not in {"measured", "unavailable"} or (
        resource_status == "measured"
        and not isinstance(
            resource_usage.get("page_cache_and_physical_io"), Mapping
        )
    ):
        raise BenchmarkError("Section 45 RSS/I/O measurement envelope is absent")
    return {
        "status": "complete",
        "generic_case_keys": sorted(GENERIC_SQL_CASE_KEYS),
        "fanout_clients": sorted(fanout_by_clients),
        "invalidation_cases": sorted(required_invalidation),
        "threshold_names": sorted(SECTION45_THRESHOLD_NAMES),
        "metric_groups": [
            "planning_p50_p95_p99",
            "time_to_first_row_and_total_latency",
            "producer_and_cache_read_throughput",
            "waiter_latency_and_wait_bounds",
            "logical_stored_row_group_dsa_file_prevalidation",
            "cold_warm_file_hits",
            "backend_postmaster_rss_and_page_cache_physical_io",
            "child_and_cache_action_counters",
            "late_fetch_and_selection_ratio",
            "invalidation_and_eviction_latency",
            "row_count_ordered_or_multiset_sha256",
        ],
    }


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
    harness_tests = _run_harness_unit_tests()
    cache_dso = _required_path("BIOSEQ_CACHE_DSO")
    cache_bootstrap = _required_path("BIOSEQ_CACHE_BOOTSTRAP_SQL")
    core_executable = _required_path("BIOSEQ_CACHE_CORE_BENCH")
    build_metadata = _build_tree_metadata(cache_dso)
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
    fixture_rows = int(cases_document.get("fixture_rows", 20_000))
    sample_count = int(cases_document.get("samples_per_case", 7))
    cluster = PRIVATE.UniquePrivateCluster(inputs)
    with cluster:
        streaming_protocol = _verify_streaming_first_row_protocol(cluster)
        core_fragment_path = cluster.run_dir / "core_fragment.json"
        core = _run_core_benchmark(
            core_executable,
            core_fragment_path,
            cluster.run_dir / "core_benchmark.log",
        )
        disabled_overhead = _measure_disabled_hook_overhead(
            cluster,
            inputs,
            configured_sample_count=sample_count,
        )
        with _ProcessResourceSampler(cluster) as resource_sampler:
            _setup_fixture(cluster, fixture_rows)
            long_source_exact = _measure_long_source_exact_hit(
                cluster,
                configured_sample_count=sample_count,
            )
            generic_provider = _measure_generic_provider_reuse(
                cluster,
                fixture_rows=fixture_rows,
                sample_count=sample_count,
            )
            cases = [
                _measure_case(cluster, case, sample_count=sample_count)
                for case in cases_document["cases"]
            ]
            fanout_sql = str(cases_document["cases"][0]["sql"])
            fanout = [
                _measure_fanout(cluster, clients, fanout_sql)
                for clients in (2, 8, 32)
            ]
            waiter_cancel = _measure_waiter_cancel_latency(
                cluster, fanout_sql, sample_count
            )
            invalidation = _measure_invalidation(cluster, sample_count)
            eviction_pressure = _measure_eviction_pressure(cluster)
            unrelated_commit = _measure_unrelated_commit(cluster, fanout_sql)
            ttl = _measure_ttl_and_eviction(cluster, fanout_sql)
            repeated_eviction = _measure_repeated_eviction_cycles(cluster)
        resource_usage = resource_sampler.report()
        disk = shutil.disk_usage(cluster.run_dir)
        disk_snapshot = _filesystem_policy_snapshot(cluster)
        thresholds = _evaluate_thresholds(
            core,
            disabled_overhead,
            long_source_exact,
            cases,
            generic_provider,
            fanout,
            waiter_cancel,
            invalidation,
            eviction_pressure,
            unrelated_commit,
            ttl,
            repeated_eviction,
            resource_usage,
            disk_snapshot,
        )
        section45_contract = _validate_section45_inventory(
            core=core,
            streaming_protocol=streaming_protocol,
            cases=cases,
            generic_provider=generic_provider,
            fanout=fanout,
            invalidation=invalidation,
            thresholds=thresholds,
            resource_usage=resource_usage,
        )
        violations = [item for item in thresholds if item["status"] == "fail"]
        git_metadata = _git_metadata()
        report = {
            "schema_version": 1,
            "benchmark": "bioseq_cache",
            "timestamp_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
            "git_commit": git_metadata["commit"],
            "source_state": git_metadata,
            "preset": build_metadata["effective_preset"],
            "compiler": build_metadata["compiler"]["display"],
            "build_metadata": build_metadata,
            "private_cluster": True,
            "build_tree_dso": str(cache_dso),
            "core": core,
            "first_row_streaming_protocol": streaming_protocol,
            "harness_unit_tests": harness_tests,
            "disabled_overhead": disabled_overhead,
            "long_source_exact": long_source_exact,
            "generic_provider_reuse": generic_provider,
            "cases": cases,
            "fanout": fanout,
            "waiter_cancel": waiter_cancel,
            "invalidation": invalidation,
            "eviction_pressure": eviction_pressure,
            "unrelated_commit": unrelated_commit,
            "ttl_eviction": ttl,
            "repeated_eviction": repeated_eviction,
            "storage": {
                "filesystem_total_bytes": disk.total,
                "filesystem_used_bytes": disk.used,
                "filesystem_free_bytes": disk.free,
                "internal_policy": disk_snapshot,
                "runtime_admin": cases[0]["runtime_storage"],
            },
            "process_resources": resource_usage,
            "baseline_deltas": {
                "disabled_planning_overhead_percent": disabled_overhead[
                    "disabled_planning_overhead_percent"
                ],
                "disabled_execution_overhead_percent": disabled_overhead[
                    "disabled_execution_overhead_percent"
                ],
                "compact_producer_overhead_percent": next(
                    case["producer"]["baseline_delta_percent"]
                    for case in cases
                    if case["key"] == "narrow_fixed_rows"
                ),
                "one_second_completed_hit_speedup": long_source_exact[
                    "completed_hit"
                ]["speedup_vs_disabled"],
                "file_cold_to_warm_p50_ratio": next(
                    case["file_hit_temperature"].get(
                        "cold_to_warm_p50_ratio"
                    )
                    for case in cases
                    if case["key"] == "large_toasted_values"
                ),
                "waiter_cancel_producer_p95_delta_ns": waiter_cancel[
                    "p95_delta_ns"
                ],
            },
            "section45_contract": section45_contract,
            "thresholds": thresholds,
            "violations": violations,
            "passed": not violations,
        }
        _validate_nested_sample_summary_contracts(report, "report")
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
