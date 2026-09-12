"""Detached process state and lifecycle guards."""

from __future__ import annotations

from typing import List, Dict, Set, Optional, Any, Callable, Mapping

import argparse
import base64
import contextlib
import ctypes
import errno
import fcntl
import hashlib
import json
import math
import os
import platform as host_platform
import re
import select
import secrets
import shlex
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
import xml.etree.ElementTree as ElementTree
from dataclasses import dataclass, field
from pathlib import Path

from publication import (
    durable_replace_bytes,
)

_CMAKE_HELPER_DIR = Path(__file__).resolve().parent.parent / "cmake"
if str(_CMAKE_HELPER_DIR) not in sys.path:
    sys.path.insert(0, str(_CMAKE_HELPER_DIR))

from sanitizer_runtime import (  # noqa: E402
    SanitizerRuntimeError,
    sanitizer_descendant_environment,
)




STATE_FILE = Path("/tmp/bioseq_build_mcp_state.json")

LOCK_FILE = Path("/tmp/bioseq_build_mcp.lock")

BUILD_RECEIPT_FILE = Path("/tmp/bioseq_build_mcp_receipts.json")

LOG_DIR = Path("/tmp")

def _trusted_root_owned_path_error(
        path: Path, *, executable: bool = False) -> str | None:
    """Return why ``path`` is unsafe for a privileged hand-off, if any."""
    try:
        metadata = path.stat()
    except OSError as exc:
        return f"cannot stat {path}: {exc}"
    if not stat.S_ISREG(metadata.st_mode):
        return f"{path} is not a regular file"
    if metadata.st_uid != 0:
        return f"{path} is not owned by root"
    if metadata.st_mode & 0o022:
        return f"{path} is group- or world-writable"
    if executable and not os.access(path, os.X_OK):
        return f"{path} is not executable"
    return None

def _require_trusted_root_owned_path(
        path: Path, *, executable: bool = False) -> None:
    problem = _trusted_root_owned_path_error(path, executable=executable)
    if problem is not None:
        raise RuntimeError(problem)

BUILD_TIMEOUT_TERM_GRACE_SEC = 10

BUILD_GUARD_TRACKER_POLL_SEC = 2.0

BUILD_GUARD_WORKER_ARM_TIMEOUT_SEC = 15.0

BUILD_GUARD_ENV_KEY = "BIOSEQ_BUILD_GUARD_TOKEN"

def _log(msg: str) -> None:
    """Server-side log line. Always stderr — stdout is JSON-RPC only."""
    print(f"[bioseq-build-mcp] {msg}", file=sys.stderr, flush=True)

_STATE_THREAD_LOCK = threading.RLock()

_STATE_LOCK_CONTEXT = threading.local()

@contextlib.contextmanager
def _global_lock():
    """Reentrant thread/process lock around state-file mutations.

    MCP servers are spawned per Claude Code session, so two sessions can
    race on start_build for the same mode. The lock makes the read-check-
    spawn-write critical section atomic across processes. A process-local
    RLock and thread-local depth make nested state helpers safe: opening a
    second independent fd and calling flock again can otherwise deadlock the
    calling process against its own first lock.
    """
    with _STATE_THREAD_LOCK:
        depth = getattr(_STATE_LOCK_CONTEXT, "depth", 0)
        if depth:
            _STATE_LOCK_CONTEXT.depth = depth + 1
            try:
                yield
            finally:
                _STATE_LOCK_CONTEXT.depth = depth
            return

        LOCK_FILE.parent.mkdir(parents=True, exist_ok=True)
        flags = os.O_RDWR | os.O_CREAT
        flags |= getattr(os, "O_CLOEXEC", 0)
        fd = os.open(str(LOCK_FILE), flags, 0o600)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX)
            _STATE_LOCK_CONTEXT.depth = 1
            yield
        finally:
            _STATE_LOCK_CONTEXT.depth = 0
            try:
                fcntl.flock(fd, fcntl.LOCK_UN)
            finally:
                os.close(fd)

def _load_state() -> dict:
    if not STATE_FILE.exists():
        return {}
    try:
        with STATE_FILE.open("r") as f:
            data = json.load(f)
        if not isinstance(data, dict):
            return {}
        return data
    except (OSError, json.JSONDecodeError):
        return {}

def _save_state_locked(state: dict) -> None:
    """Durably replace the state file while `_global_lock` is held."""
    if getattr(_STATE_LOCK_CONTEXT, "depth", 0) <= 0:
        raise RuntimeError(
            "_save_state_locked requires the global state lock")
    durable_replace_bytes(
        STATE_FILE,
        (json.dumps(state, indent=2, sort_keys=True) + "\n").encode("utf-8"),
    )

def _save_state(state: dict) -> None:
    """Durably replace the complete state under the reentrant global lock."""
    with _global_lock():
        _save_state_locked(state)

def _update_entry(mode: str, **fields) -> None:
    """Atomically merge fields without losing another worker's state."""
    with _global_lock():
        state = _load_state()
        entry = dict(state.get(mode) or {})
        entry.update(fields)
        state[mode] = entry
        _save_state_locked(state)

def _load_build_receipts() -> dict[str, dict[str, object]]:
    """Load persistent successful build receipts.

    Receipt ids are intentionally opaque.  Malformed top-level or individual
    entries are ignored rather than becoming authority for a run-only or
    inspection request.
    """
    if not BUILD_RECEIPT_FILE.exists():
        return {}
    try:
        with BUILD_RECEIPT_FILE.open("r", encoding="utf-8") as handle:
            value = json.load(handle)
    except (OSError, json.JSONDecodeError):
        return {}
    if not isinstance(value, dict):
        return {}
    return {
        key: entry
        for key, entry in value.items()
        if isinstance(key, str) and isinstance(entry, dict)
    }

def _save_build_receipts_locked(
        receipts: Mapping[str, Mapping[str, object]]) -> None:
    """Durably replace the receipt ledger while the global lock is held."""
    if getattr(_STATE_LOCK_CONTEXT, "depth", 0) <= 0:
        raise RuntimeError(
            "_save_build_receipts_locked requires the global state lock")
    durable_replace_bytes(
        BUILD_RECEIPT_FILE,
        (json.dumps(receipts, indent=2, sort_keys=True) + "\n").encode("utf-8"),
    )

def _store_build_receipt(receipt: Mapping[str, object]) -> None:
    receipt_id = receipt.get("build_receipt_id")
    if not isinstance(receipt_id, str) or not receipt_id:
        raise RuntimeError("build receipt is missing its opaque id")
    with _global_lock():
        receipts = _load_build_receipts()
        if receipt_id in receipts:
            raise RuntimeError(
                f"build receipt id collision for {receipt_id!r}")
        receipts[receipt_id] = dict(receipt)
        _save_build_receipts_locked(receipts)


def _get_build_receipt(receipt_id: str) -> dict[str, object] | None:
    if not isinstance(receipt_id, str) or not receipt_id:
        return None
    with _global_lock():
        receipt = _load_build_receipts().get(receipt_id)
        return dict(receipt) if isinstance(receipt, dict) else None

def _is_pid_alive(pid) -> bool:
    if not isinstance(pid, int) or pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        # Process exists but we don't own it — extremely unlikely in this
        # workflow, but treat as alive to be safe.
        return True
    except OSError as e:
        if e.errno == errno.ESRCH:
            return False
        return True
    return True

def _refresh(
        state: dict,
        mode: str,
        *,
        service_boundary_provider: (
            _GuardedServiceBoundaryProvider | None) = None,
) -> dict | None:
    """Reconcile the recorded status of one mode with reality.

    A "running" entry whose pid is dead means the worker was killed without
    getting a chance to write its terminal status (OOM, SIGKILL from outside,
    machine reboot, etc.) is promoted to "aborted". Guarded fresh workers first
    sweep their unique build directory; an unverified orphan cleanup keeps the
    next build fail-closed.
    """
    entry = state.get(mode)
    if not entry:
        return None
    watchdog_value = entry.get("q1024_watchdog_process_identity")
    if (entry.get("q1024_watchdog_status") in {"armed", "monitoring"}
            and isinstance(watchdog_value, dict)):
        try:
            watchdog_identity = _ProcessIdentity(**watchdog_value)
        except (TypeError, ValueError):
            watchdog_identity = None
        observed_watchdog = (
            _capture_process_identity(watchdog_identity.pid)
            if watchdog_identity is not None else None)
        if not _same_process(observed_watchdog, watchdog_identity):
            entry = dict(entry)
            entry.update({
                "q1024_watchdog_status": "unexpected_death",
                "q1024_watchdog_evidence": {
                    "cleanup_verified": False,
                    "reason": (
                        "independent q1024 watchdog identity disappeared or "
                        "was replaced"),
                    "observed_at": time.time(),
                },
                "cleanup_unverified": True,
                "process_cleanup_unverified": True,
            })
            state[mode] = entry
            _save_state(state)
    worker_identity_value = entry.get("worker_process_identity")
    worker_identity: _ProcessIdentity | None = None
    if isinstance(worker_identity_value, dict):
        try:
            worker_identity = _ProcessIdentity(**worker_identity_value)
        except (TypeError, ValueError):
            worker_identity = None
    worker_identity_mismatch = False
    worker_pid_alive = _is_pid_alive(entry.get("pid"))
    if entry.get("status") == "running" and worker_identity is not None:
        observed_worker = _capture_process_identity(worker_identity.pid)
        # A transient process-table failure must not misclassify a live worker
        # and kill its healthy build. A successful mismatching observation is
        # PID reuse; an actually dead PID enters orphan recovery below.
        worker_identity_mismatch = (
            observed_worker is not None
            and not _same_process(observed_worker, worker_identity)
        )
    if entry.get("status") == "running" and (
        worker_identity_mismatch or not worker_pid_alive
    ):
        entry = dict(entry)
        postgres_cleanup_unverified = (
            entry.get("postgres_cleanup_unverified") is True)
        entry["status"] = "aborted"
        entry["step"] = "aborted"
        if entry.get("build_timeout_sec") is not None:
            recovery_evidence, cleanup_unverified = (
                _recover_guarded_worker_descendants(
                    entry,
                    service_boundary_provider=(
                        service_boundary_provider
                        or _persisted_guarded_service_boundary_provider(entry)),
                )
            )
            entry["termination_evidence"] = recovery_evidence
            entry["cleanup_unverified"] = (
                cleanup_unverified or postgres_cleanup_unverified)
        if postgres_cleanup_unverified:
            cleanup_evidence = entry.get("postgres_cleanup_evidence")
            cleanup_evidence = (
                dict(cleanup_evidence)
                if isinstance(cleanup_evidence, dict) else {})
            cleanup_evidence["cleanup_verified"] = False
            cleanup_evidence["unexpected_worker_exit"] = True
            cleanup_evidence.setdefault(
                "reason",
                "worker exited before exact PostgreSQL owner cleanup was verified",
            )
            entry["postgres_cleanup_evidence"] = cleanup_evidence
            entry["cleanup_unverified"] = True
        if worker_identity_mismatch:
            entry["notes"] = (
                "recorded worker PID no longer has the reserved start identity"
            )
        if not entry.get("completed_at"):
            entry["completed_at"] = time.time()
        state[mode] = entry
        _save_state(state)
    return entry

@dataclass(frozen=True)
class _ProcessIdentity:
    pid: int
    ppid: int
    pgid: int
    session: int
    started: str
    command: str


@dataclass(frozen=True)
class _GuardedServiceBoundary:
    """One exact long-lived service root separated from a guarded client."""

    verified_root: _ProcessIdentity | None
    quarantined_root: _ProcessIdentity | None
    evidence: Mapping[str, object]
    quarantined_roots: tuple[_ProcessIdentity, ...] = ()


_GuardedServiceBoundaryProvider = Callable[
    [
        Mapping[int, _ProcessIdentity],
        _ProcessIdentity,
        str,
        float | None,
        Set[int],
    ],
    _GuardedServiceBoundary,
]

def _process_snapshot(
        deadline: float | None = None,
        *,
        require_complete: bool = False,
) -> dict[int, _ProcessIdentity]:
    """Capture one bounded process table used for identity-safe termination."""
    timeout = 10.0
    if deadline is not None:
        remaining = deadline - time.time()
        if remaining <= 0.0:
            raise RuntimeError("process identity snapshot deadline was reached")
        timeout = min(timeout, remaining)
    try:
        completed = subprocess.run(
            [
                "ps", "-axo",
                "pid=,ppid=,pgid=,sess=,lstart=,command=",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError("process identity snapshot timed out") from exc
    if completed.returncode != 0:
        raise RuntimeError(
            "could not capture process identities: "
            + completed.stderr.strip())
    snapshot: dict[int, _ProcessIdentity] = {}
    errors: list[str] = []
    for line_number, line in enumerate(completed.stdout.splitlines(), 1):
        fields = line.strip().split(None, 9)
        if len(fields) != 10:
            if require_complete and line.strip():
                errors.append(f"line {line_number}: malformed ps record")
            continue
        try:
            pid, ppid, parsed_pgid, parsed_session = map(int, fields[:4])
        except ValueError:
            if require_complete:
                errors.append(f"line {line_number}: malformed process ids")
            continue
        try:
            # BSD/macOS `ps -o sess` reports zero for ordinary processes.
            # Kernel queries provide the actual session/group identity needed
            # to validate start_new_session=True.
            pgid = os.getpgid(pid)
            session = os.getsid(pid)
        except ProcessLookupError:
            # ``ps`` can report a process which exits before this bounded
            # kernel identity query (including the ``ps`` process itself).
            # ESRCH proves that exact listed PID is no longer live; it is not
            # a parse or permission gap in the remaining live snapshot.
            continue
        except (PermissionError, OSError) as exc:
            if require_complete:
                errors.append(
                    f"pid {pid}: kernel process identity unavailable: "
                    f"{type(exc).__name__}: {exc}")
                continue
            if parsed_pgid <= 0 or parsed_session <= 0:
                continue
            pgid = parsed_pgid
            session = parsed_session
        snapshot[pid] = _ProcessIdentity(
            pid=pid,
            ppid=ppid,
            pgid=pgid,
            session=session,
            started=" ".join(fields[4:9]),
            command=fields[9],
        )
    if errors:
        raise RuntimeError(
            "process identity snapshot was incomplete: "
            + "; ".join(errors[:16]))
    return snapshot

def _capture_process_identity(
        pid: int, *, deadline: float | None = None) -> _ProcessIdentity | None:
    try:
        return _process_snapshot(deadline).get(pid)
    except RuntimeError:
        return None

def _same_process(
        observed: _ProcessIdentity | None,
        expected: _ProcessIdentity | None) -> bool:
    # PPID, process group, session and argv can legitimately change while a
    # process survives TERM. PID plus the kernel-reported start identity is the
    # immutable pair used to reject PID reuse.
    return (
        observed is not None
        and expected is not None
        and observed.pid == expected.pid
        and observed.started == expected.started
    )


def _process_is_zombie(
        expected: _ProcessIdentity,
        *, deadline: float | None = None) -> bool:
    """Return true only for the same exact process observed in zombie state."""
    observed = _capture_process_identity(expected.pid, deadline=deadline)
    if not _same_process(observed, expected):
        return False
    timeout = 10.0
    if deadline is not None:
        remaining = deadline - time.time()
        if remaining <= 0.0:
            return False
        timeout = min(timeout, remaining)
    try:
        result = subprocess.run(
            ["ps", "-o", "stat=", "-p", str(expected.pid)],
            capture_output=True,
            text=True,
            check=False,
            timeout=timeout,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    return (
        result.returncode == 0
        and result.stdout.strip().upper().startswith("Z")
    )

def _same_process_context(
        observed: _ProcessIdentity | None,
        expected: _ProcessIdentity) -> bool:
    """Match one exact process and its immutable build-worker session."""
    return (
        _same_process(observed, expected)
        and observed is not None
        and observed.session == expected.session
    )

def _valid_detached_worker_identity(
        identity: _ProcessIdentity | None,
        *,
        child_pid: int,
        parent_pid: int) -> bool:
    """Validate the exact topology promised by ``start_new_session=True``."""
    return (
        identity is not None
        and identity.pid == child_pid
        and identity.ppid == parent_pid
        and identity.pgid == child_pid
        and identity.session == child_pid
        and bool(identity.started)
    )


def _await_q1024_watchdog_arm(
        mode: str, run_token: str) -> dict[str, object]:
    """Require the independently detached q1024 watchdog before setup."""
    deadline = time.monotonic() + BUILD_GUARD_WORKER_ARM_TIMEOUT_SEC
    while time.monotonic() < deadline:
        entry = _load_state().get(mode)
        if not isinstance(entry, dict):
            raise RuntimeError("q1024 state disappeared before watchdog arm")
        if entry.get("status") != "running":
            raise RuntimeError("q1024 worker was cancelled before watchdog arm")
        value = entry.get("q1024_watchdog_process_identity")
        if (entry.get("q1024_run_token") == run_token
                and entry.get("q1024_watchdog_status")
                    in {"armed", "monitoring"}
                and isinstance(value, dict)):
            try:
                expected = _ProcessIdentity(**value)
            except (TypeError, ValueError) as exc:
                raise RuntimeError(
                    f"q1024 watchdog identity is malformed: {exc}") from exc
            observed = _capture_process_identity(expected.pid)
            if not _same_process_context(observed, expected):
                raise RuntimeError(
                    "q1024 watchdog pid/start/session does not match its arm")
            return dict(entry)
        time.sleep(0.02)
    raise RuntimeError("q1024 watchdog identity arm timed out")


def _claim_q1024_postgresql_cleanup(
        mode: str, run_token: str, *, actor: str,
) -> tuple[str, dict[str, object]] | None:
    """Atomically claim owner cleanup without holding the lock while doing it.

    A dead exact claimant may be replaced.  A live claimant, malformed state,
    or a different run token is fail-closed and produces no claim.
    """
    if actor not in {"worker", "watchdog"}:
        raise RuntimeError("invalid q1024 PostgreSQL cleanup actor")
    claimant_identity = _capture_process_identity(os.getpid())
    if claimant_identity is None:
        raise RuntimeError("cannot capture q1024 cleanup claimant identity")
    with _global_lock():
        state = _load_state()
        entry = state.get(mode)
        if (not isinstance(entry, dict)
                or entry.get("q1024_run_token") != run_token
                or entry.get("postgres_backend_owner_started") is not True):
            return None
        actor_identity_value = entry.get(
            "worker_process_identity"
            if actor == "worker" else "q1024_watchdog_process_identity")
        try:
            actor_identity = (
                _ProcessIdentity(**actor_identity_value)
                if isinstance(actor_identity_value, dict) else None)
        except (TypeError, ValueError):
            actor_identity = None
        if (actor_identity is None
                or not _same_process_context(
                    claimant_identity, actor_identity)):
            return None
        previous = entry.get("postgres_cleanup_claim")
        if isinstance(previous, dict):
            previous_value = previous.get("process_identity")
            previous_identity: _ProcessIdentity | None = None
            if isinstance(previous_value, dict):
                try:
                    previous_identity = _ProcessIdentity(**previous_value)
                except (TypeError, ValueError):
                    previous_identity = None
            previous_live = (
                previous_identity is not None
                and _same_process(
                    _capture_process_identity(previous_identity.pid),
                    previous_identity)
                and not _process_is_zombie(previous_identity)
            )
            if previous_live:
                if (_same_process(previous_identity, claimant_identity)
                        and previous.get("actor") == actor
                        and isinstance(previous.get("claim_token"), str)):
                    return str(previous["claim_token"]), dict(entry)
                return None
        claim_token = secrets.token_hex(16)
        entry = dict(entry)
        entry["postgres_cleanup_claim"] = {
            "actor": actor,
            "claim_token": claim_token,
            "claimed_at": time.time(),
            "process_identity": claimant_identity.__dict__,
        }
        state[mode] = entry
        _save_state_locked(state)
        return claim_token, dict(entry)


def _claim_q1024_postgresql_revalidation(
        mode: str,
        run_token: str,
        *,
        deadline: float,
) -> tuple[str, dict[str, object]] | None:
    """Claim terminal legacy cleanup without impersonating worker/watchdog."""
    if (
        mode != "benchmark-e2e"
        or re.fullmatch(r"[0-9a-f]{32}", run_token) is None
    ):
        return None
    snapshot = _process_snapshot(deadline, require_complete=True)
    claimant_identity = snapshot.get(os.getpid())
    if claimant_identity is None:
        raise RuntimeError(
            "cannot capture server cleanup revalidator identity")
    with _global_lock():
        state = _load_state()
        entry = state.get(mode)
        if (
            not isinstance(entry, dict)
            or entry.get("status") not in {
                "completed", "failed", "aborted", "killed_timeout"}
            or entry.get("q1024_run_token") != run_token
            or entry.get("postgres_application_name")
                != f"bioseq-q1024-{run_token}"
            or entry.get("postgres_backend_owner_started") is not True
            or entry.get("postgres_cleanup_unverified") is not True
            or entry.get("postgres_service_boundary") is not None
        ):
            return None
        for field_name in (
                "worker_process_identity",
                "active_process_identity",
                "q1024_watchdog_process_identity"):
            value = entry.get(field_name)
            try:
                expected = (
                    _ProcessIdentity(**value)
                    if isinstance(value, dict) else None)
            except (TypeError, ValueError):
                return None
            if expected is None:
                return None
            if _same_process(snapshot.get(expected.pid), expected):
                return None
        previous = entry.get("postgres_cleanup_claim")
        if isinstance(previous, dict):
            previous_value = previous.get("process_identity")
            try:
                previous_identity = (
                    _ProcessIdentity(**previous_value)
                    if isinstance(previous_value, dict) else None)
            except (TypeError, ValueError):
                previous_identity = None
            if previous_identity is not None and _same_process(
                    snapshot.get(previous_identity.pid), previous_identity):
                if (
                    _same_process(previous_identity, claimant_identity)
                    and previous.get("actor") == "server_revalidator"
                    and isinstance(previous.get("claim_token"), str)
                ):
                    return str(previous["claim_token"]), dict(entry)
                return None
        claim_token = secrets.token_hex(16)
        entry = dict(entry)
        entry["postgres_cleanup_claim"] = {
            "actor": "server_revalidator",
            "claim_token": claim_token,
            "claimed_at": time.time(),
            "process_identity": claimant_identity.__dict__,
        }
        state[mode] = entry
        _save_state_locked(state)
        return claim_token, dict(entry)


def _finish_q1024_postgresql_cleanup(
        mode: str,
        run_token: str,
        claim_token: str,
        evidence: Mapping[str, object],
) -> bool:
    """Publish cleanup evidence iff this exact run still owns the claim."""
    with _global_lock():
        state = _load_state()
        entry = state.get(mode)
        claim = (
            entry.get("postgres_cleanup_claim")
            if isinstance(entry, dict) else None)
        if (not isinstance(entry, dict)
                or entry.get("q1024_run_token") != run_token
                or not isinstance(claim, dict)
                or claim.get("claim_token") != claim_token):
            return False
        verified = evidence.get("cleanup_verified") is True
        termination_evidence = entry.get("termination_evidence")
        process_unverified = bool(
            entry.get("cleanup_unverified_before_postgres_owner")
            or entry.get("process_cleanup_unverified"))
        if termination_evidence is not None:
            process_unverified = (
                process_unverified
                or not isinstance(termination_evidence, dict)
                or termination_evidence.get("cleanup_verified") is not True
                or any(
                    not isinstance(termination_evidence.get(field, []), list)
                    or bool(termination_evidence.get(field, []))
                    for field in (
                        "survivor_pids",
                        "marker_survivor_pids",
                        "survivor_identities",
                    )
                )
            )
        entry = dict(entry)
        entry.update({
            "postgres_backend_owner_started": not verified,
            "postgres_cleanup_evidence": dict(evidence),
            "postgres_cleanup_unverified": not verified,
            "cleanup_unverified": process_unverified or not verified,
            "postgres_cleanup_claim": {
                **claim,
                "finished_at": time.time(),
                "cleanup_verified": verified,
            },
        })
        state[mode] = entry
        _save_state_locked(state)
        return verified


def _finish_q1024_postgresql_revalidation(
        mode: str,
        run_token: str,
        claim_token: str,
        evidence: Mapping[str, object],
) -> bool:
    """Publish separate terminal revalidation evidence under the exact claim."""
    with _global_lock():
        state = _load_state()
        entry = state.get(mode)
        claim = (
            entry.get("postgres_cleanup_claim")
            if isinstance(entry, dict) else None)
        if (
            not isinstance(entry, dict)
            or entry.get("status") == "running"
            or entry.get("q1024_run_token") != run_token
            or not isinstance(claim, dict)
            or claim.get("actor") != "server_revalidator"
            or claim.get("claim_token") != claim_token
        ):
            return False
        verified = evidence.get("cleanup_verified") is True
        entry = dict(entry)
        entry["postgres_cleanup_revalidation_evidence"] = dict(evidence)
        entry["postgres_cleanup_claim"] = {
            **claim,
            "finished_at": time.time(),
            "cleanup_verified": verified,
        }
        if verified:
            entry["postgres_backend_owner_started"] = False
            entry["postgres_cleanup_unverified"] = False
            # Process-tree uncertainty remains until the existing exact
            # build-root ownership sweep proves it empty.
            entry["cleanup_unverified"] = True
        state[mode] = entry
        _save_state_locked(state)
        return verified


def _update_q1024_watchdog_state(
        mode: str, run_token: str, **fields: object) -> bool:
    """Merge watchdog evidence only while the same reserved run is current."""
    with _global_lock():
        state = _load_state()
        entry = state.get(mode)
        if (not isinstance(entry, dict)
                or entry.get("q1024_run_token") != run_token):
            return False
        entry = dict(entry)
        entry.update(fields)
        state[mode] = entry
        _save_state_locked(state)
        return True


def _publish_q1024_postgresql_service_boundary(
        mode: str,
        run_token: str,
        client_identity: _ProcessIdentity,
        guard_marker: str,
        boundary: Mapping[str, object],
) -> bool:
    """CAS-publish a runtime proof for the exact active q1024 client."""
    if (
        mode != "benchmark-e2e"
        or re.fullmatch(r"[0-9a-f]{32}", run_token) is None
        or re.fullmatch(r"[0-9a-f]{32}", guard_marker) is None
        or boundary.get("schema_version") != 1
        or boundary.get("mode") != mode
        or boundary.get("run_token") != run_token
        or boundary.get("application_name")
            != f"bioseq-q1024-{run_token}"
        or boundary.get("client_process_identity")
            != client_identity.__dict__
        or boundary.get("guard_marker") != guard_marker
        or boundary.get("runtime_identity_verified") is not True
    ):
        return False
    with _global_lock():
        state = _load_state()
        entry = state.get(mode)
        active_value = (
            entry.get("active_process_identity")
            if isinstance(entry, dict) else None)
        try:
            active_identity = (
                _ProcessIdentity(**active_value)
                if isinstance(active_value, dict) else None)
        except (TypeError, ValueError):
            active_identity = None
        if (
            not isinstance(entry, dict)
            or entry.get("status") != "running"
            or entry.get("q1024_run_token") != run_token
            or entry.get("postgres_application_name")
                != f"bioseq-q1024-{run_token}"
            or entry.get("active_guard_marker") != guard_marker
            or not _same_process(active_identity, client_identity)
        ):
            return False
        entry = dict(entry)
        entry["postgres_service_boundary"] = dict(boundary)
        state[mode] = entry
        _save_state_locked(state)
        return True

class _GuardedTerminationError(RuntimeError):
    """A guarded subprocess could not prove that all descendants exited."""

    def __init__(self, message: str, evidence: Mapping[str, object]):
        super().__init__(message)
        self.evidence = dict(evidence)

_DARWIN_LIBPROC = None

def _process_current_working_directory(pid: int) -> Path | None:
    """Return a same-user process cwd without shelling out per PID."""
    if sys.platform.startswith("linux"):
        try:
            return Path(os.readlink(f"/proc/{pid}/cwd"))
        except (OSError, ValueError):
            return None
    if sys.platform == "darwin":
        # proc_vnodepathinfo is two 1,176-byte vnode_info_path records;
        # vip_path begins at byte 152 (MacOSX SDK sys/proc_info.h).
        global _DARWIN_LIBPROC
        try:
            if _DARWIN_LIBPROC is None:
                _DARWIN_LIBPROC = ctypes.CDLL(
                    "/usr/lib/libproc.dylib", use_errno=True)
                _DARWIN_LIBPROC.proc_pidinfo.argtypes = [
                    ctypes.c_int,
                    ctypes.c_int,
                    ctypes.c_uint64,
                    ctypes.c_void_p,
                    ctypes.c_int,
                ]
                _DARWIN_LIBPROC.proc_pidinfo.restype = ctypes.c_int
            buffer_size = 2352
            buffer = ctypes.create_string_buffer(buffer_size)
            returned = _DARWIN_LIBPROC.proc_pidinfo(
                pid, 9, 0, buffer, buffer_size)
            if returned < 1176:
                return None
            raw_path = buffer.raw[152:1176].split(b"\0", 1)[0]
            if not raw_path:
                return None
            return Path(os.fsdecode(raw_path))
        except (AttributeError, OSError, ValueError):
            return None
    return None

def _process_inherits_guard_marker(pid: int, marker: str) -> bool:
    """Read the inherited guard token where the host exposes proc environ."""
    if not sys.platform.startswith("linux"):
        return False
    try:
        body = Path(f"/proc/{pid}/environ").read_bytes()
    except OSError:
        return False
    needle = f"{BUILD_GUARD_ENV_KEY}={marker}".encode("ascii")
    return needle in body.split(b"\0")

def _path_is_within(path: Path, root: Path) -> bool:
    try:
        path.absolute().relative_to(root.absolute())
        return True
    except (OSError, ValueError):
        return False

def _guarded_process_snapshot(
        marker: str,
        ownership_root: Path | None = None,
        *, strict_marker_only: bool = False,
        include_commands: bool = False,
) -> tuple[dict[int, _ProcessIdentity], set[int]]:
    """Capture identities plus processes inheriting one private guard marker.

    Linux exposes the inherited marker through ``/proc``. macOS deliberately
    omits process environments from ``ps`` and KERN_PROCARGS2 on current
    releases, so fresh guarded runs also use their unique MCP build directory:
    a detached Ninja/compiler/linker remains discoverable by argv or cwd even
    after reparenting and ``setsid``. Returned command text stays redacted.
    """
    completed = subprocess.run(
        [
            "ps", "-axo",
            "pid=,ppid=,pgid=,sess=,lstart=,command=",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        text=True,
        timeout=5,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            "could not capture guarded process identities: "
            + completed.stderr.strip())
    marker_text = f"{BUILD_GUARD_ENV_KEY}={marker}"
    snapshot: dict[int, _ProcessIdentity] = {}
    marked: set[int] = set()
    for line in completed.stdout.splitlines():
        fields = line.strip().split(None, 9)
        if len(fields) != 10:
            continue
        try:
            pid, ppid, parsed_pgid, parsed_session = map(int, fields[:4])
        except ValueError:
            continue
        try:
            pgid = os.getpgid(pid)
            session = os.getsid(pid)
        except (ProcessLookupError, PermissionError, OSError):
            if parsed_pgid <= 0 or parsed_session <= 0:
                continue
            pgid = parsed_pgid
            session = parsed_session
        snapshot[pid] = _ProcessIdentity(
            pid=pid,
            ppid=ppid,
            pgid=pgid,
            session=session,
            started=" ".join(fields[4:9]),
            command=(
                fields[9]
                if include_commands else "<redacted guarded process>"),
        )
        owned_by_command = (
            ownership_root is not None
            and str(ownership_root) in fields[9]
        )
        cwd = _process_current_working_directory(pid)
        owned_by_cwd = (
            ownership_root is not None
            and cwd is not None
            and _path_is_within(cwd, ownership_root)
        )
        if (
            marker_text in fields[9]
            or _process_inherits_guard_marker(pid, marker)
            or (not strict_marker_only and owned_by_command)
            or (not strict_marker_only and owned_by_cwd)
        ):
            marked.add(pid)
    return snapshot, marked


def _process_descendant_closure(
        snapshot: Mapping[int, _ProcessIdentity],
        root: _ProcessIdentity | None,
        *,
        require_context: bool = False,
) -> set[int]:
    """Return only identities connected to one exact root by current PPIDs."""
    observed_root = snapshot.get(root.pid) if root is not None else None
    root_matches = (
        _same_process_context(observed_root, root)
        if root is not None and require_context
        else _same_process(observed_root, root)
    )
    if not root_matches:
        return set()
    descendants = {root.pid}
    changed = True
    while changed:
        changed = False
        for process in snapshot.values():
            if process.pid not in descendants and process.ppid in descendants:
                descendants.add(process.pid)
                changed = True
    return descendants


def _guarded_boundary_file_identity_valid(value: object) -> bool:
    """Cheaply rebind a descriptor-sealed file proof without rehashing it."""
    if not isinstance(value, dict) or set(value) != {
        "path", "device", "inode", "size", "sha256",
    }:
        return False
    path = value.get("path")
    if (
        not isinstance(path, str)
        or not Path(path).is_absolute()
        or type(value.get("device")) is not int
        or type(value.get("inode")) is not int
        or type(value.get("size")) is not int
        or not isinstance(value.get("sha256"), str)
        or re.fullmatch(r"[0-9a-f]{64}", str(value["sha256"])) is None
    ):
        return False
    try:
        resolved = Path(path).resolve(strict=True)
        metadata = resolved.stat()
    except (OSError, RuntimeError, ValueError):
        return False
    return (
        resolved == Path(path)
        and stat.S_ISREG(metadata.st_mode)
        and metadata.st_dev == value["device"]
        and metadata.st_ino == value["inode"]
        and metadata.st_size == value["size"]
    )


def _guarded_service_partition(
        snapshot: Mapping[int, _ProcessIdentity],
        boundary: _GuardedServiceBoundary | None,
) -> tuple[set[int], set[int]]:
    """Partition a proven service and an unproven candidate by exact ancestry."""
    if boundary is None:
        return set(), set()
    preserved = _process_descendant_closure(
        snapshot, boundary.verified_root, require_context=True)
    quarantine_roots = [*boundary.quarantined_roots]
    if boundary.quarantined_root is not None:
        quarantine_roots.append(boundary.quarantined_root)
    quarantined: set[int] = set()
    for root in quarantine_roots:
        quarantined.update(_process_descendant_closure(snapshot, root))
    quarantined.difference_update(preserved)
    return preserved, quarantined


def _provider_exception_quarantine_roots(
        snapshot: Mapping[int, _ProcessIdentity],
        candidate_pids: Set[int],
        control_root: _ProcessIdentity,
) -> tuple[_ProcessIdentity, ...]:
    """Quarantine uncertainty while retaining one exact signalable root."""
    return tuple(
        snapshot[pid]
        for pid in sorted(candidate_pids)
        if (
            pid in snapshot
            and not _same_process_context(snapshot[pid], control_root)
        )
    )


def _persisted_guarded_service_boundary_provider(
        entry: Mapping[str, object],
) -> _GuardedServiceBoundaryProvider | None:
    """Revalidate one token-bound persisted service root without PostgreSQL I/O."""
    value = entry.get("postgres_service_boundary")
    if not isinstance(value, dict):
        return None
    root_value = value.get("postmaster_process_identity")
    try:
        persisted_root = (
            _ProcessIdentity(**root_value)
            if isinstance(root_value, dict) else None)
    except (TypeError, ValueError):
        persisted_root = None
    expected_client = entry.get("active_process_identity")
    expected_worker = entry.get("worker_process_identity")
    try:
        persisted_client = (
            _ProcessIdentity(**expected_client)
            if isinstance(expected_client, dict) else None)
        persisted_worker = (
            _ProcessIdentity(**expected_worker)
            if isinstance(expected_worker, dict) else None)
    except (TypeError, ValueError):
        persisted_client = None
        persisted_worker = None
    static_bindings_valid: bool | None = None

    def provider(
            snapshot: Mapping[int, _ProcessIdentity],
            client_root: _ProcessIdentity,
            guard_marker: str,
            deadline: float | None,
            candidate_pids: Set[int],
    ) -> _GuardedServiceBoundary:
        del deadline, candidate_pids
        observed_root = (
            snapshot.get(persisted_root.pid)
            if persisted_root is not None else None)
        evidence = {
            "source": "persisted_q1024_postgresql_service_boundary",
            "boundary": dict(value),
        }
        expected_token = entry.get("q1024_run_token")
        expected_application = entry.get("postgres_application_name")
        nonlocal static_bindings_valid
        if static_bindings_valid is None:
            data_directory = value.get("data_directory")
            data_directory_valid = False
            if isinstance(data_directory, dict) and set(data_directory) == {
                    "path", "device", "inode"}:
                path = data_directory.get("path")
                try:
                    metadata = Path(str(path)).stat()
                    data_directory_valid = (
                        isinstance(path, str)
                        and Path(path).is_absolute()
                        and Path(path).resolve(strict=True) == Path(path)
                        and stat.S_ISDIR(metadata.st_mode)
                        and metadata.st_dev == data_directory.get("device")
                        and metadata.st_ino == data_directory.get("inode")
                    )
                except (OSError, RuntimeError, ValueError):
                    data_directory_valid = False
            static_bindings_valid = (
                set(value) == {
                    "schema_version", "mode", "run_token",
                    "application_name", "client_process_identity",
                    "guard_marker", "data_directory",
                    "postgres_executable_identity", "bioseq_dso_identity",
                    "postmaster_process_identity",
                    "runtime_identity_verified", "verified_at",
                }
                and value.get("schema_version") == 1
                and value.get("mode") == "benchmark-e2e"
                and entry.get("mode") == "benchmark-e2e"
                and isinstance(expected_token, str)
                and re.fullmatch(r"[0-9a-f]{32}", expected_token) is not None
                and value.get("run_token") == expected_token
                and expected_application == f"bioseq-q1024-{expected_token}"
                and value.get("application_name") == expected_application
                and value.get("guard_marker") == entry.get(
                    "active_guard_marker")
                and isinstance(value.get("guard_marker"), str)
                and re.fullmatch(
                    r"[0-9a-f]{32}", str(value["guard_marker"])) is not None
                and value.get("client_process_identity") == expected_client
                and persisted_client is not None
                and persisted_worker is not None
                and persisted_root is not None
                and value.get("runtime_identity_verified") is True
                and type(value.get("verified_at")) in {int, float}
                and data_directory_valid
                and _guarded_boundary_file_identity_valid(
                    value.get("postgres_executable_identity"))
                and _guarded_boundary_file_identity_valid(
                    value.get("bioseq_dso_identity"))
            )
        caller_bound = (
            persisted_client is not None
            and _same_process_context(client_root, persisted_client)
        ) or (
            persisted_worker is not None
            and _same_process_context(client_root, persisted_worker)
        ) or (
            persisted_client is not None
            and persisted_client.pid
                in _process_descendant_closure(
                    snapshot, client_root, require_context=True)
        )
        bindings_valid = (
            static_bindings_valid
            and value.get("guard_marker") == guard_marker
            and caller_bound
        )
        evidence["bindings_valid"] = bindings_valid
        if bindings_valid and persisted_root is not None and (
            _same_process_context(observed_root, persisted_root)
        ):
            return _GuardedServiceBoundary(
                verified_root=persisted_root,
                quarantined_root=None,
                evidence=evidence,
            )
        if persisted_root is not None and _same_process(
                observed_root, persisted_root):
            evidence["error"] = (
                "live persisted PostgreSQL root failed boundary bindings")
            return _GuardedServiceBoundary(
                verified_root=None,
                quarantined_root=persisted_root,
                evidence=evidence,
            )
        return _GuardedServiceBoundary(
            verified_root=None,
            quarantined_root=None,
            evidence=evidence,
        )

    return provider

@dataclass
class _GuardedProcessTracker:
    """Retain exact descendants even after reparenting or session changes."""

    root: _ProcessIdentity
    marker: str
    ownership_root: Path | None = None
    strict_marker_only: bool = False
    service_boundary_provider: _GuardedServiceBoundaryProvider | None = None
    service_boundary_deadline: float | None = None
    members: dict[tuple[int, str], _ProcessIdentity] = field(
        default_factory=dict)
    last_snapshot: dict[int, _ProcessIdentity] = field(default_factory=dict)
    last_live: dict[tuple[int, str], _ProcessIdentity] = field(
        default_factory=dict)
    last_preserved_service: dict[tuple[int, str], _ProcessIdentity] = field(
        default_factory=dict)
    last_quarantined_service: dict[tuple[int, str], _ProcessIdentity] = field(
        default_factory=dict)
    last_service_boundary_evidence: dict[str, object] = field(
        default_factory=dict)
    service_boundary_failed: bool = False
    snapshot_errors: list[str] = field(default_factory=list)

    def __post_init__(self) -> None:
        key = (self.root.pid, self.root.started)
        self.members[key] = self.root
        # Even if the first global snapshot fails, the Popen-captured root is
        # exact and its new process group remains a safe TERM/KILL fallback.
        self.last_snapshot[self.root.pid] = self.root
        self.last_live[key] = self.root

    def refresh(self) -> dict[tuple[int, str], _ProcessIdentity]:
        snapshot_kwargs = {
            "strict_marker_only": self.strict_marker_only,
        }
        if self.service_boundary_provider is not None:
            snapshot_kwargs["include_commands"] = True
        raw_snapshot, marked_pids = _guarded_process_snapshot(
            self.marker, self.ownership_root, **snapshot_kwargs)
        candidate_pids = set(marked_pids)
        candidate_pids.update(_process_descendant_closure(
            raw_snapshot, self.root, require_context=True))
        for identity in self.members.values():
            if _same_process(raw_snapshot.get(identity.pid), identity):
                candidate_pids.add(identity.pid)
        boundary: _GuardedServiceBoundary | None = None
        if self.service_boundary_provider is not None:
            try:
                boundary = self.service_boundary_provider(
                    raw_snapshot,
                    self.root,
                    self.marker,
                    self.service_boundary_deadline,
                    candidate_pids,
                )
            except Exception as exc:
                self.record_snapshot_error(exc)
                self.service_boundary_failed = True
                self.last_service_boundary_evidence = {
                    "error": f"{type(exc).__name__}: {exc}",
                    "provider_exception_quarantine": True,
                }
                boundary = _GuardedServiceBoundary(
                    verified_root=None,
                    quarantined_root=None,
                    quarantined_roots=(
                        _provider_exception_quarantine_roots(
                            raw_snapshot, candidate_pids, self.root)),
                    evidence=self.last_service_boundary_evidence,
                )
            else:
                self.last_service_boundary_evidence = dict(boundary.evidence)
        snapshot = {
            pid: _ProcessIdentity(
                pid=identity.pid,
                ppid=identity.ppid,
                pgid=identity.pgid,
                session=identity.session,
                started=identity.started,
                command="<redacted guarded process>",
            )
            for pid, identity in raw_snapshot.items()
        }
        preserved_pids, quarantined_pids = _guarded_service_partition(
            snapshot, boundary)
        selected: set[int] = set(marked_pids)
        for identity in self.members.values():
            if _same_process(snapshot.get(identity.pid), identity):
                selected.add(identity.pid)
        if _same_process(snapshot.get(self.root.pid), self.root):
            selected.add(self.root.pid)

        changed = True
        while changed:
            changed = False
            selected_sessions = {
                snapshot[pid].session
                for pid in selected
                if pid in snapshot and snapshot[pid].session > 0
            }
            for process in snapshot.values():
                if process.pid in selected:
                    continue
                if (
                    process.ppid in selected
                    or (
                        process.session > 0
                        and process.session in selected_sessions
                    )
                ):
                    selected.add(process.pid)
                    changed = True

        selected.difference_update(preserved_pids | quarantined_pids)

        for pid in selected:
            process = snapshot.get(pid)
            if process is None:
                continue
            key = (process.pid, process.started)
            self.members[key] = process

        live = {
            key: observed
            for key, identity in self.members.items()
            if (
                (observed := snapshot.get(identity.pid)) is not None
                and _same_process(observed, identity)
                and observed.pid not in preserved_pids
                and observed.pid not in quarantined_pids
            )
        }
        self.last_snapshot = snapshot
        self.last_live = live
        self.last_preserved_service = {
            (snapshot[pid].pid, snapshot[pid].started): snapshot[pid]
            for pid in preserved_pids
            if pid in snapshot
        }
        self.last_quarantined_service = {
            (snapshot[pid].pid, snapshot[pid].started): snapshot[pid]
            for pid in quarantined_pids
            if pid in snapshot
        }
        return dict(live)

    def record_snapshot_error(self, exc: BaseException) -> None:
        message = f"{type(exc).__name__}: {exc}"
        if not self.snapshot_errors or self.snapshot_errors[-1] != message:
            self.snapshot_errors.append(message)
        if len(self.snapshot_errors) > 16:
            del self.snapshot_errors[:-16]

def _guarded_worker_validation(
        state_mode: str | None) -> tuple[dict[str, object], str | None]:
    if state_mode is None:
        return {}, "guarded termination has no state mode"
    entry = _load_state().get(state_mode)
    value = entry.get("worker_process_identity") if isinstance(entry, dict) else None
    if not isinstance(value, dict):
        return {}, "guarded worker identity is absent from state"
    try:
        expected = _ProcessIdentity(**value)
    except (TypeError, ValueError) as exc:
        return {}, f"guarded worker identity is malformed: {exc}"
    observed = _capture_process_identity(expected.pid)
    evidence = {
        "expected": expected.__dict__,
        "observed": observed.__dict__ if observed is not None else None,
    }
    if not _same_process_context(observed, expected):
        return evidence, "guarded worker pid/start/session did not revalidate"
    return evidence, None

def _signal_guarded_processes(
        live: Mapping[tuple[int, str], _ProcessIdentity],
        sig: signal.Signals,
        *,
        snapshot: Mapping[int, _ProcessIdentity]) -> dict[str, object]:
    groups = sorted({process.pgid for process in live.values() if process.pgid > 0})
    live_by_pid = {process.pid: process for process in live.values()}
    signaled_groups: list[int] = []
    skipped_groups: list[int] = []
    signaled_pids: list[int] = []
    errors: list[str] = []
    for pgid in groups:
        observed_group = [
            process for process in snapshot.values()
            if process.pgid == pgid
        ]
        group_is_owned = bool(observed_group) and all(
            _same_process(process, live_by_pid.get(process.pid))
            for process in observed_group
        )
        if not group_is_owned:
            skipped_groups.append(pgid)
            continue
        try:
            os.killpg(pgid, sig)
            signaled_groups.append(pgid)
        except ProcessLookupError:
            continue
        except OSError as exc:
            errors.append(f"pgid {pgid}: {exc}")
    # Exact-PID signals are a backstop for a process that moved groups after
    # the snapshot and avoid killpg when the group contains an unowned member.
    for process in live.values():
        try:
            os.kill(process.pid, sig)
            signaled_pids.append(process.pid)
        except ProcessLookupError:
            continue
        except OSError as exc:
            errors.append(f"pid {process.pid}: {exc}")
    return {
        "signal": int(sig),
        "groups": signaled_groups,
        "skipped_unowned_groups": skipped_groups,
        "direct_pids": sorted(signaled_pids),
        "candidate_pids": sorted(process.pid for process in live.values()),
        "errors": errors,
    }


def _guarded_service_evidence(
        tracker: _GuardedProcessTracker) -> dict[str, object]:
    return {
        "service_boundary_failed": tracker.service_boundary_failed,
        "service_boundary_evidence": dict(
            tracker.last_service_boundary_evidence),
        "preserved_service_identities": [
            process.__dict__
            for process in sorted(
                tracker.last_preserved_service.values(),
                key=lambda identity: identity.pid,
            )
        ],
        "quarantined_service_identities": [
            process.__dict__
            for process in sorted(
                tracker.last_quarantined_service.values(),
                key=lambda identity: identity.pid,
            )
        ],
    }

def _terminate_guarded_process_tree(
        tracker: _GuardedProcessTracker,
        *,
        hard_kill_deadline: float,
        state_mode: str | None,
        clock=time.monotonic,
        sleeper=time.sleep,
        validate_worker: bool = True,
) -> dict[str, object]:
    """TERM, rescan, and KILL every exact guarded identity by the deadline."""
    evidence: dict[str, object] = {
        "root": tracker.root.__dict__,
        "term_started_monotonic": clock(),
        "hard_kill_deadline_monotonic": hard_kill_deadline,
    }
    if validate_worker:
        worker_evidence, worker_error = _guarded_worker_validation(state_mode)
        evidence["worker_validation"] = worker_evidence
        if worker_error is not None:
            evidence["worker_validation_error"] = worker_error
    else:
        worker_evidence, worker_error = {}, None
        evidence["worker_validation"] = {
            "skipped": "recovering descendants after worker death",
        }

    try:
        live = tracker.refresh()
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        tracker.record_snapshot_error(exc)
        live = dict(tracker.last_live)
    evidence["term"] = _signal_guarded_processes(
        live, signal.SIGTERM, snapshot=tracker.last_snapshot)

    grace_deadline = min(
        hard_kill_deadline,
        clock() + BUILD_TIMEOUT_TERM_GRACE_SEC,
    )
    latest_successful_refresh = False
    while clock() < grace_deadline:
        try:
            live = tracker.refresh()
            latest_successful_refresh = True
            if (
                not live
                and not tracker.last_quarantined_service
                and not tracker.service_boundary_failed
            ):
                if validate_worker and worker_error is not None:
                    worker_evidence, worker_error = (
                        _guarded_worker_validation(state_mode))
                    evidence["worker_validation"] = worker_evidence
                    if worker_error is None:
                        evidence.pop("worker_validation_error", None)
                if worker_error is None:
                    evidence.update({
                        "kill": [],
                        "survivor_pids": [],
                        "survivor_identities": [],
                        "snapshot_errors": list(tracker.snapshot_errors),
                        "completed_monotonic": clock(),
                        "within_hard_deadline": clock() <= hard_kill_deadline,
                        "cleanup_verified": True,
                        **_guarded_service_evidence(tracker),
                    })
                    return evidence
        except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
            tracker.record_snapshot_error(exc)
            latest_successful_refresh = False
            live = dict(tracker.last_live)
        sleeper(min(0.1, max(0.0, grace_deadline - clock())))

    kill_records: list[dict[str, object]] = []
    while clock() < hard_kill_deadline:
        try:
            live = tracker.refresh()
            latest_successful_refresh = True
        except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
            tracker.record_snapshot_error(exc)
            latest_successful_refresh = False
            live = dict(tracker.last_live)
        if (
            not live
            and not tracker.last_quarantined_service
            and not tracker.service_boundary_failed
            and latest_successful_refresh
        ):
            if validate_worker and worker_error is not None:
                worker_evidence, worker_error = _guarded_worker_validation(
                    state_mode)
                evidence["worker_validation"] = worker_evidence
                if worker_error is None:
                    evidence.pop("worker_validation_error", None)
            if worker_error is None:
                evidence.update({
                    "kill": kill_records,
                    "survivor_pids": [],
                    "survivor_identities": [],
                    "snapshot_errors": list(tracker.snapshot_errors),
                    "completed_monotonic": clock(),
                    "within_hard_deadline": True,
                    "cleanup_verified": True,
                    **_guarded_service_evidence(tracker),
                })
                return evidence
        if live:
            kill_records.append(
                _signal_guarded_processes(
                    live,
                    signal.SIGKILL,
                    snapshot=tracker.last_snapshot,
                ))
        remaining = hard_kill_deadline - clock()
        if remaining <= 0.25:
            break
        sleeper(min(0.1, remaining))

    quarantined = list(tracker.last_quarantined_service.values())
    survivor_identities = [
        *tracker.last_live.values(),
        *quarantined,
    ]
    evidence.update({
        "kill": kill_records,
        "survivor_pids": sorted(
            {process.pid for process in survivor_identities}),
        "survivor_identities": [
            process.__dict__
            for process in sorted(
                survivor_identities, key=lambda value: value.pid)
        ],
        "snapshot_errors": list(tracker.snapshot_errors),
        "completed_monotonic": clock(),
        "within_hard_deadline": clock() <= hard_kill_deadline,
        "cleanup_verified": False,
        **_guarded_service_evidence(tracker),
    })
    if worker_error is not None:
        evidence["worker_validation_error"] = worker_error
    raise _GuardedTerminationError(
        "guarded subprocess cleanup was not proven before the hard deadline",
        evidence,
    )

def _recover_guarded_worker_descendants(
        entry: Mapping[str, object],
        *, strict_marker_only: bool = False,
        hard_kill_deadline: float | None = None,
        service_boundary_provider: (
            _GuardedServiceBoundaryProvider | None) = None,
) -> tuple[dict[str, object], bool]:
    """Sweep a dead guarded worker's unique fresh build directory."""
    build_dir_value = entry.get("build_dir")
    if not isinstance(build_dir_value, str) or not build_dir_value:
        return ({"recovery_error": "guarded entry has no build directory"}, True)
    ownership_root = Path(build_dir_value).resolve(strict=False)
    recorded_marker = entry.get("active_guard_marker")
    recovery_marker = (
        str(recorded_marker)
        if (isinstance(recorded_marker, str)
            and re.fullmatch(r"[0-9a-f]{32}", recorded_marker) is not None)
        else "worker-death-ownership-sweep"
    )
    try:
        snapshot, owned_pids = _guarded_process_snapshot(
            recovery_marker,
            ownership_root,
            strict_marker_only=strict_marker_only,
        )
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        return ({
            "recovery_error": f"{type(exc).__name__}: {exc}",
            "ownership_root": str(ownership_root),
            "cleanup_verified": False,
        }, True)
    owned = [
        snapshot[pid] for pid in sorted(owned_pids)
        if pid in snapshot
    ]
    if not owned:
        return ({
            "ownership_root": str(ownership_root),
            "survivor_pids": [],
            "survivor_identities": [],
            "cleanup_verified": True,
            "recovery": "worker died; ownership sweep found no descendants",
        }, False)

    active_value = entry.get("active_process_identity")
    active_identity: _ProcessIdentity | None = None
    if isinstance(active_value, dict):
        try:
            candidate = _ProcessIdentity(**active_value)
            if (
                service_boundary_provider is not None
                or any(_same_process(process, candidate) for process in owned)
            ):
                active_identity = candidate
        except (TypeError, ValueError):
            pass
    if service_boundary_provider is not None and active_identity is None:
        return ({
            "recovery_error": (
                "persisted service boundary has no exact recorded client "
                "identity; no process was signalled"),
            "ownership_root": str(ownership_root),
            "survivor_pids": sorted(process.pid for process in owned),
            "survivor_identities": [
                process.__dict__ for process in owned],
            "cleanup_verified": False,
        }, True)
    root = active_identity or owned[0]
    tracker = _GuardedProcessTracker(
        root,
        recovery_marker,
        ownership_root,
        strict_marker_only=strict_marker_only,
        service_boundary_provider=service_boundary_provider,
        service_boundary_deadline=hard_kill_deadline,
    )
    tracker.last_snapshot = dict(snapshot)
    tracker.last_live = {}
    for process in owned:
        key = (process.pid, process.started)
        tracker.members[key] = process
        tracker.last_live[key] = process
    try:
        remaining = (
            2 * BUILD_TIMEOUT_TERM_GRACE_SEC
            if hard_kill_deadline is None
            else max(0.0, hard_kill_deadline - time.time())
        )
        if remaining <= 0.0:
            raise _GuardedTerminationError(
                "worker-death cleanup deadline was reached",
                {
                    "cleanup_verified": False,
                    "survivor_pids": sorted(
                        process.pid for process in owned),
                },
            )
        evidence = _terminate_guarded_process_tree(
            tracker,
            hard_kill_deadline=(
                time.monotonic()
                + min(2 * BUILD_TIMEOUT_TERM_GRACE_SEC, remaining)),
            state_mode=None,
            validate_worker=False,
        )
        evidence["recovery"] = "worker died; descendants terminated"
        evidence["ownership_root"] = str(ownership_root)
        evidence["cleanup_verified"] = True
        return evidence, False
    except _GuardedTerminationError as exc:
        evidence = dict(exc.evidence)
        evidence["recovery"] = "worker died; cleanup remained unverified"
        evidence["ownership_root"] = str(ownership_root)
        return evidence, True

def _await_guarded_worker_arm(mode: str) -> dict[str, object]:
    """Wait until the parent records this detached worker's exact identity."""
    deadline = time.monotonic() + BUILD_GUARD_WORKER_ARM_TIMEOUT_SEC
    while time.monotonic() < deadline:
        entry = _load_state().get(mode)
        if not isinstance(entry, dict):
            raise RuntimeError("guarded worker state disappeared before arming")
        if entry.get("status") != "running":
            raise RuntimeError("guarded worker was cancelled before arming")
        value = entry.get("worker_process_identity")
        if (
            entry.get("step") == "armed"
            and entry.get("pid") == os.getpid()
            and isinstance(value, dict)
        ):
            try:
                expected = _ProcessIdentity(**value)
            except (TypeError, ValueError) as exc:
                raise RuntimeError(
                    f"guarded worker identity is malformed: {exc}") from exc
            observed = _capture_process_identity(os.getpid())
            if not _same_process_context(observed, expected):
                raise RuntimeError(
                    "guarded worker pid/start/session does not match its arm")
            return dict(entry)
        time.sleep(0.02)
    raise RuntimeError("guarded worker identity arm timed out")

def _validated_process_tree(
        root: _ProcessIdentity,
        *, deadline: float | None = None,
        guard_marker: str = "",
        service_boundary_provider: (
            _GuardedServiceBoundaryProvider | None) = None,
) -> tuple[dict[int, list[_ProcessIdentity]], dict[str, object]]:
    snapshot = _process_snapshot(deadline)
    root_matches = (
        _same_process_context(snapshot.get(root.pid), root)
        if service_boundary_provider is not None
        else _same_process(snapshot.get(root.pid), root)
    )
    if not root_matches:
        raise RuntimeError(
            "subprocess identity changed before timeout termination")
    descendants = {root.pid}
    changed = True
    while changed:
        changed = False
        for process in snapshot.values():
            if (
                process.pid not in descendants
                and process.ppid in descendants
            ):
                descendants.add(process.pid)
                changed = True
    boundary: _GuardedServiceBoundary | None = None
    service_boundary_failed = False
    if service_boundary_provider is not None:
        try:
            boundary = service_boundary_provider(
                snapshot, root, guard_marker, deadline, set(descendants))
        except Exception as exc:
            service_boundary_failed = True
            boundary = _GuardedServiceBoundary(
                verified_root=None,
                quarantined_root=None,
                quarantined_roots=_provider_exception_quarantine_roots(
                    snapshot, set(descendants), root),
                evidence={
                    "error": f"{type(exc).__name__}: {exc}",
                    "provider_exception_quarantine": True,
                },
            )
    preserved, quarantined = _guarded_service_partition(snapshot, boundary)
    descendants.difference_update(preserved | quarantined)
    groups: dict[int, list[_ProcessIdentity]] = {}
    for pid in descendants:
        process = snapshot[pid]
        groups.setdefault(process.pgid, []).append(process)
    evidence = {
        "root": root.__dict__,
        "descendant_pids": sorted(descendants),
        "process_groups": sorted(groups),
        "service_boundary_evidence": (
            dict(boundary.evidence) if boundary is not None else {}),
        "service_boundary_failed": service_boundary_failed,
        "preserved_service_identities": [
            snapshot[pid].__dict__ for pid in sorted(preserved)
            if pid in snapshot
        ],
        "quarantined_service_identities": [
            snapshot[pid].__dict__ for pid in sorted(quarantined)
            if pid in snapshot
        ],
    }
    return groups, evidence

def _terminate_validated_process_tree(
        root: _ProcessIdentity,
        *,
        hard_kill_deadline: float,
        guard_marker: str = "",
        service_boundary_provider: (
            _GuardedServiceBoundaryProvider | None) = None,
) -> dict[str, object]:
    """TERM every validated descendant group, then KILL exact survivors."""
    groups, evidence = _validated_process_tree(
        root,
        deadline=hard_kill_deadline,
        guard_marker=guard_marker,
        service_boundary_provider=service_boundary_provider,
    )
    quarantined = list(evidence["quarantined_service_identities"])
    if service_boundary_provider is None:
        term_groups: list[int] = []
        for pgid in sorted(groups, reverse=True):
            try:
                os.killpg(pgid, signal.SIGTERM)
                term_groups.append(pgid)
            except ProcessLookupError:
                continue
        evidence["term_groups"] = term_groups
    else:
        # Re-snapshot before signaling.  _signal_guarded_processes declines a
        # whole-group signal whenever the group also contains a preserved or
        # quarantined service identity, then uses exact descendants instead.
        term_snapshot = _process_snapshot(hard_kill_deadline)
        term_live = {
            (process.pid, process.started): observed
            for processes in groups.values()
            for process in processes
            if (
                (observed := term_snapshot.get(process.pid)) is not None
                and _same_process(observed, process)
            )
        }
        term_record = _signal_guarded_processes(
            term_live, signal.SIGTERM, snapshot=term_snapshot)
        evidence["term_groups"] = term_record["groups"]
        evidence["term_direct_pids"] = term_record["direct_pids"]
        evidence["term_skipped_unowned_groups"] = term_record[
            "skipped_unowned_groups"]
        evidence["term_errors"] = term_record["errors"]

    grace_deadline = min(
        max(time.time(), hard_kill_deadline - 1.0),
        time.time() + BUILD_TIMEOUT_TERM_GRACE_SEC,
    )
    while time.time() < grace_deadline:
        try:
            snapshot = _process_snapshot(hard_kill_deadline)
        except RuntimeError:
            break
        if (
            not evidence["service_boundary_failed"]
            and not quarantined
            and not any(
                _same_process(snapshot.get(process.pid), process)
                for processes in groups.values()
                for process in processes
            )
        ):
            evidence["kill_groups"] = []
            evidence["survivor_pids"] = []
            evidence["survivor_identities"] = []
            evidence["cleanup_verified"] = True
            evidence["completed_at"] = time.time()
            return evidence
        time.sleep(min(0.1, max(0.0, grace_deadline - time.time())))

    snapshot_error: str | None = None
    try:
        snapshot = _process_snapshot(hard_kill_deadline)
    except RuntimeError as exc:
        snapshot = {}
        snapshot_error = str(exc)
    kill_groups: list[int] = []
    survivor_pids: list[int] = []
    for pgid, processes in sorted(groups.items(), reverse=True):
        exact_survivors = [
            process
            for process in processes
            if (
                _same_process(snapshot.get(process.pid), process)
                and snapshot[process.pid].pgid == pgid
            )
        ]
        if not exact_survivors:
            continue
        survivor_pids.extend(process.pid for process in exact_survivors)
        if service_boundary_provider is None:
            try:
                os.killpg(pgid, signal.SIGKILL)
                kill_groups.append(pgid)
            except ProcessLookupError:
                continue
        else:
            kill_record = _signal_guarded_processes(
                {
                    (process.pid, process.started): process
                    for process in exact_survivors
                },
                signal.SIGKILL,
                snapshot=snapshot,
            )
            kill_groups.extend(kill_record["groups"])
            evidence.setdefault("kill_direct_pids", []).extend(
                kill_record["direct_pids"])
            evidence.setdefault("kill_skipped_unowned_groups", []).extend(
                kill_record["skipped_unowned_groups"])
            evidence.setdefault("kill_errors", []).extend(kill_record["errors"])
    evidence["kill_groups"] = kill_groups
    final_snapshot = snapshot
    while survivor_pids and time.time() < hard_kill_deadline:
        try:
            final_snapshot = _process_snapshot(hard_kill_deadline)
            survivor_pids = [
                process.pid
                for processes in groups.values()
                for process in processes
                if _same_process(final_snapshot.get(process.pid), process)
            ]
            if not survivor_pids:
                break
        except RuntimeError as exc:
            snapshot_error = str(exc)
            break
        time.sleep(min(0.05, max(0.0, hard_kill_deadline - time.time())))
    quarantined_pids = [
        value.get("pid") for value in quarantined
        if isinstance(value, dict) and isinstance(value.get("pid"), int)
    ]
    evidence["survivor_pids"] = sorted(
        set(survivor_pids + quarantined_pids))
    evidence["survivor_identities"] = [
        process.__dict__
        for processes in groups.values()
        for process in processes
        if process.pid in survivor_pids
    ] + quarantined
    if snapshot_error is not None:
        evidence["verification_error"] = snapshot_error
    evidence["cleanup_verified"] = (
        snapshot_error is None
        and not evidence["service_boundary_failed"]
        and not survivor_pids
        and not quarantined
    )
    evidence["completed_at"] = time.time()
    evidence["within_hard_deadline"] = time.time() <= hard_kill_deadline
    return evidence

def _latest_ninja_edge(log_handle) -> str | None:
    """Return the last bounded Ninja progress line from the active log."""
    try:
        log_handle.flush()
        path = Path(str(log_handle.name))
        with path.open("rb") as handle:
            size = handle.seek(0, os.SEEK_END)
            handle.seek(max(0, size - 256 * 1024))
            tail = handle.read().decode("utf-8", errors="replace")
    except (AttributeError, OSError):
        return None
    progress = re.compile(r"^\[[0-9]+/[0-9]+\].*$")
    return next(
        (line for line in reversed(tail.splitlines()) if progress.match(line)),
        None,
    )

def _kill_pgroup(
        pid: int,
        *,
        hard_kill_deadline: float | None = None,
        clock=None) -> None:
    """SIGTERM then SIGKILL the entire process group rooted at pid.

    Cmake spawns ninja which spawns clang++; killing only the cmake pid
    leaves the compile jobs running. start_new_session=True puts each
    Popen in its own session/group so killpg reaches the whole tree.
    """
    try:
        pgid = os.getpgid(pid)
    except (ProcessLookupError, OSError):
        return
    active_clock = clock or time.time
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(pgid, sig)
        except (ProcessLookupError, OSError):
            return
        if sig == signal.SIGKILL:
            return
        # Give children a moment to exit before escalating.
        grace_deadline = active_clock() + 2.0
        if hard_kill_deadline is not None:
            grace_deadline = min(grace_deadline, hard_kill_deadline)
        for _ in range(20):
            remaining = grace_deadline - active_clock()
            if remaining <= 0.0:
                break
            time.sleep(min(0.1, remaining))
            try:
                os.killpg(pgid, 0)
            except (ProcessLookupError, OSError):
                return

def _terminate_unarmed_popen_group(
        proc: subprocess.Popen,
        *,
        grace_sec: float = 1.0,
        guard_marker: str | None = None,
        ownership_root: Path | None = None) -> dict[str, object]:
    """Stop the exact new process group owned by a just-created Popen.

    This is the only safe fallback before a process-table identity has been
    captured: ``start_new_session=True`` makes the child PID its process-group
    id, while the live Popen handle proves which just-created child we own.
    """
    evidence: dict[str, object] = {
        "root_pid": proc.pid,
        "process_group": proc.pid,
        "term_sent": False,
        "kill_sent": False,
    }
    try:
        os.killpg(proc.pid, signal.SIGTERM)
        evidence["term_sent"] = True
    except ProcessLookupError:
        pass
    except OSError as exc:
        evidence["term_error"] = str(exc)
    deadline = time.monotonic() + max(0.0, grace_sec)
    while time.monotonic() < deadline:
        try:
            os.killpg(proc.pid, 0)
        except (ProcessLookupError, OSError):
            break
        time.sleep(min(0.05, deadline - time.monotonic()))
    try:
        os.killpg(proc.pid, signal.SIGKILL)
        evidence["kill_sent"] = True
    except ProcessLookupError:
        pass
    except OSError as exc:
        evidence["kill_error"] = str(exc)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired as exc:
        evidence["wait_error"] = str(exc)
    evidence["root_returncode"] = proc.poll()
    if guard_marker is not None:
        try:
            snapshot, marked_pids = _guarded_process_snapshot(
                guard_marker, ownership_root)
            exact_marked = [
                snapshot[pid] for pid in sorted(marked_pids)
                if pid in snapshot
            ]
            for process in exact_marked:
                try:
                    os.kill(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    continue
            remaining_snapshot, remaining_marked = _guarded_process_snapshot(
                guard_marker, ownership_root)
            evidence["marker_survivor_pids"] = sorted(remaining_marked)
            evidence["survivor_identities"] = [
                remaining_snapshot[pid].__dict__
                for pid in sorted(remaining_marked)
                if pid in remaining_snapshot
            ]
            evidence["cleanup_verified"] = (
                proc.poll() is not None and not remaining_marked)
        except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
            evidence["marker_scan_error"] = str(exc)
            evidence["cleanup_verified"] = False
    else:
        evidence["cleanup_verified"] = proc.poll() is not None
    return evidence

def _stream_subprocess(
        cmd: list[str],
        log_handle,
        deadline: float,
        cwd: Path | None = None,
        env: dict | None = None,
        *,
        verified_executable: Path | None = None,
        hard_kill_deadline: float | None = None,
        state_mode: str | None = None,
        phase: str | None = None,
        require_verified_termination: bool = False,
        service_boundary_provider: (
            _GuardedServiceBoundaryProvider | None) = None,
        initial_service_boundary_provider: (
            _GuardedServiceBoundaryProvider | None) = None,
        deadline_clock=None,
) -> int:
    """Run cmd, tee combined stdout/stderr to log_handle and stderr, enforce
    deadline. Returns the process return code on clean exit. Raises
    TimeoutError if the deadline is hit.

    The subprocess runs in its own session so the whole tree can be killed
    with killpg(); cmake/ninja/clang++ do not all live under cmake's pid.
    """
    clock = deadline_clock or time.time
    pretty = " ".join(cmd)
    header = f"\n+ {pretty}\n"
    log_handle.write(header)
    log_handle.flush()
    sys.stderr.write(header)
    sys.stderr.flush()

    if clock() >= deadline:
        raise TimeoutError(
            f"deadline reached before subprocess launch: {pretty}")
    if require_verified_termination and (
        hard_kill_deadline is None or hard_kill_deadline <= deadline
    ):
        raise RuntimeError(
            "guarded subprocess requires a hard deadline after TERM")
    if (
        require_verified_termination
        and service_boundary_provider is not None
        and initial_service_boundary_provider is None
    ):
        raise RuntimeError(
            "guarded service client requires a pre-arm classification provider")

    guard_marker: str | None = None
    guard_ownership_root: Path | None = None
    popen_env = env
    if require_verified_termination:
        guard_marker = secrets.token_hex(16)
        guarded_entry = (
            _load_state().get(state_mode)
            if state_mode is not None else None
        )
        guarded_build_dir = (
            guarded_entry.get("build_dir")
            if isinstance(guarded_entry, dict) else None
        )
        if not isinstance(guarded_build_dir, str) or not guarded_build_dir:
            raise RuntimeError(
                "guarded subprocess has no isolated build-directory owner")
        guard_ownership_root = Path(guarded_build_dir).resolve(strict=False)
        # Fail before launching CMake/Ninja when this host cannot provide the
        # process/cwd snapshot needed to find detached groups.
        _guarded_process_snapshot(guard_marker, guard_ownership_root)
        popen_env = dict(os.environ if env is None else env)
        popen_env[BUILD_GUARD_ENV_KEY] = guard_marker
        if clock() >= deadline:
            raise TimeoutError(
                f"deadline reached during subprocess guard preflight: {pretty}")

    proc = subprocess.Popen(
        cmd,
        executable=(
            str(verified_executable)
            if verified_executable is not None else None
        ),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        cwd=str(cwd) if cwd else None,
        env=popen_env,
        bufsize=0,
        start_new_session=True,
    )
    process_identity = _capture_process_identity(proc.pid)
    if require_verified_termination and process_identity is None:
        # The Popen handle still identifies the just-created root exactly. Stop
        # it before allowing configure/Ninja to create an unverifiable tree.
        unarmed_termination = _terminate_unarmed_popen_group(
            proc,
            guard_marker=guard_marker,
            ownership_root=guard_ownership_root,
        )
        if state_mode is not None:
            cleanup_unverified = not bool(
                unarmed_termination.get("cleanup_verified"))
            _update_entry(
                state_mode,
                termination_evidence=unarmed_termination,
                cleanup_unverified=cleanup_unverified,
            )
        if proc.stdout is not None:
            proc.stdout.close()
        raise RuntimeError(
            "guarded subprocess identity capture failed before execution")
    tracker = (
        _GuardedProcessTracker(
            process_identity,
            guard_marker,
            guard_ownership_root,
            service_boundary_provider=initial_service_boundary_provider,
            service_boundary_deadline=hard_kill_deadline,
        )
        if (
            require_verified_termination
            and process_identity is not None
            and guard_marker is not None
        )
        else None
    )
    if tracker is not None:
        try:
            tracker.refresh()
        except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
            tracker.record_snapshot_error(exc)
            try:
                try:
                    evidence = _terminate_guarded_process_tree(
                        tracker,
                        hard_kill_deadline=min(
                            hard_kill_deadline,
                            clock() + 2 * BUILD_TIMEOUT_TERM_GRACE_SEC,
                        ),
                        state_mode=state_mode,
                        clock=clock,
                    )
                except _GuardedTerminationError as cleanup_error:
                    if state_mode is not None:
                        _update_entry(
                            state_mode,
                            termination_evidence=cleanup_error.evidence,
                            cleanup_unverified=True,
                        )
                    raise cleanup_error from exc
                if state_mode is not None:
                    _update_entry(
                        state_mode,
                        termination_evidence=evidence,
                        cleanup_unverified=False,
                    )
            finally:
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
                if proc.stdout is not None:
                    proc.stdout.close()
            raise RuntimeError(
                "guarded subprocess initial descendant capture failed") from exc
    if state_mode is not None:
        _update_entry(
            state_mode,
            active_command=pretty,
            active_phase=phase,
            active_pid=proc.pid,
            active_process_identity=(
                process_identity.__dict__
                if process_identity is not None else None
            ),
            active_guard_marker=guard_marker,
        )
    if tracker is not None:
        tracker.service_boundary_provider = service_boundary_provider

    fd = proc.stdout.fileno()
    cleanup_attempted = False
    cleanup_verified = False
    stdout_eof = False
    next_tracker_refresh = clock() + BUILD_GUARD_TRACKER_POLL_SEC
    try:
        while True:
            now = clock()
            remaining = deadline - now
            if remaining <= 0:
                active_edge = _latest_ninja_edge(log_handle)
                if state_mode is not None:
                    _update_entry(
                        state_mode,
                        active_edge=active_edge,
                        timeout_term_started_at=time.time(),
                        timeout_term_started_monotonic=now,
                    )
                termination_evidence: dict[str, object]
                if tracker is not None:
                    cleanup_attempted = True
                    try:
                        termination_evidence = _terminate_guarded_process_tree(
                            tracker,
                            hard_kill_deadline=hard_kill_deadline,
                            state_mode=state_mode,
                            clock=clock,
                        )
                        cleanup_verified = True
                    except _GuardedTerminationError as exc:
                        termination_evidence = exc.evidence
                        if state_mode is not None:
                            _update_entry(
                                state_mode,
                                termination_evidence=termination_evidence,
                                cleanup_unverified=True,
                            )
                        raise
                elif process_identity is not None:
                    termination_evidence = _terminate_validated_process_tree(
                        process_identity,
                        hard_kill_deadline=(
                            hard_kill_deadline
                            if hard_kill_deadline is not None
                            else time.time()
                            + BUILD_TIMEOUT_TERM_GRACE_SEC
                        ),
                    )
                else:
                    _kill_pgroup(
                        proc.pid,
                        hard_kill_deadline=hard_kill_deadline,
                        clock=clock,
                    )
                    termination_evidence = {
                        "fallback_process_group": proc.pid,
                        "reason": "process identity capture unavailable",
                    }
                if state_mode is not None:
                    termination_update: dict[str, object] = {
                        "termination_evidence": termination_evidence,
                    }
                    if termination_evidence.get("cleanup_verified") is not True:
                        # Never overwrite a pre-existing uncertainty with a
                        # weaker process-group termination observation.
                        termination_update["cleanup_unverified"] = True
                        termination_update["process_cleanup_unverified"] = True
                    _update_entry(state_mode, **termination_update)
                raise TimeoutError(f"hard timeout reached during: {pretty}")

            select_timeout = min(remaining, 1.0)
            if tracker is not None:
                select_timeout = min(
                    select_timeout,
                    max(0.0, next_tracker_refresh - now),
                )
            if stdout_eof:
                time.sleep(select_timeout)
                ready = []
            else:
                ready, _, _ = select.select(
                    [fd], [], [], select_timeout)
            if ready:
                try:
                    chunk = os.read(fd, 65536)
                except OSError:
                    chunk = b""
                if not chunk:
                    stdout_eof = True
                else:
                    text = chunk.decode("utf-8", errors="replace")
                    log_handle.write(text)
                    log_handle.flush()
                    sys.stderr.write(text)
                    sys.stderr.flush()
            if tracker is not None and clock() >= next_tracker_refresh:
                try:
                    tracker.refresh()
                except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
                    tracker.record_snapshot_error(exc)
                next_tracker_refresh = clock() + BUILD_GUARD_TRACKER_POLL_SEC
            if proc.poll() is not None:
                # Drain only bytes already ready. A detached child may retain
                # the pipe; a blocking drain would otherwise defeat the guard.
                try:
                    while select.select([fd], [], [], 0)[0]:
                        chunk = os.read(fd, 65536)
                        if not chunk:
                            break
                        text = chunk.decode("utf-8", errors="replace")
                        log_handle.write(text)
                        log_handle.flush()
                        sys.stderr.write(text)
                        sys.stderr.flush()
                except OSError:
                    pass
                break
        return_code = proc.wait()
        if tracker is not None:
            try:
                live = tracker.refresh()
            except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
                tracker.record_snapshot_error(exc)
                live = dict(tracker.last_live)
            if (
                live
                or tracker.last_quarantined_service
                or tracker.service_boundary_failed
            ):
                cleanup_attempted = True
                cleanup_deadline = min(
                    hard_kill_deadline,
                    clock() + 2 * BUILD_TIMEOUT_TERM_GRACE_SEC,
                )
                try:
                    evidence = _terminate_guarded_process_tree(
                        tracker,
                        hard_kill_deadline=cleanup_deadline,
                        state_mode=state_mode,
                        clock=clock,
                    )
                    cleanup_verified = True
                    if state_mode is not None:
                        _update_entry(
                            state_mode, termination_evidence=evidence)
                except _GuardedTerminationError as exc:
                    if state_mode is not None:
                        _update_entry(
                            state_mode,
                            termination_evidence=exc.evidence,
                            cleanup_unverified=True,
                        )
                    raise
                raise RuntimeError(
                    "guarded subprocess exited while descendants remained")
            if service_boundary_provider is not None and state_mode is not None:
                evidence = {
                    "root": tracker.root.__dict__,
                    "cleanup_verified": True,
                    "survivor_pids": [],
                    "survivor_identities": [],
                    **_guarded_service_evidence(tracker),
                }
                _update_entry(state_mode, termination_evidence=evidence)
            cleanup_verified = True
        return return_code
    finally:
        if tracker is not None and not cleanup_verified and not cleanup_attempted:
            cleanup_attempted = True
            cleanup_deadline = min(
                hard_kill_deadline,
                clock() + 2 * BUILD_TIMEOUT_TERM_GRACE_SEC,
            )
            try:
                evidence = _terminate_guarded_process_tree(
                    tracker,
                    hard_kill_deadline=cleanup_deadline,
                    state_mode=state_mode,
                    clock=clock,
                )
                cleanup_verified = True
                if state_mode is not None:
                    _update_entry(state_mode, termination_evidence=evidence)
            except _GuardedTerminationError as exc:
                if state_mode is not None:
                    _update_entry(
                        state_mode,
                        termination_evidence=exc.evidence,
                        cleanup_unverified=True,
                    )
                raise
        elif tracker is None and proc.poll() is None:
            if process_identity is not None:
                try:
                    final_evidence = _terminate_validated_process_tree(
                        process_identity,
                        hard_kill_deadline=(
                            hard_kill_deadline
                            if hard_kill_deadline is not None
                            else time.time()
                                + BUILD_TIMEOUT_TERM_GRACE_SEC
                        ),
                    )
                    if state_mode is not None:
                        final_update: dict[str, object] = {
                            "termination_evidence": final_evidence,
                        }
                        if final_evidence.get("cleanup_verified") is not True:
                            final_update["cleanup_unverified"] = True
                            final_update["process_cleanup_unverified"] = True
                        _update_entry(state_mode, **final_update)
                except RuntimeError as exc:
                    # Identity changed: fail closed rather than signaling an
                    # unverified process. The caller's timeout/failure record
                    # preserves the diagnostic.
                    if state_mode is not None:
                        _update_entry(
                            state_mode,
                            termination_evidence={
                                "cleanup_verified": False,
                                "error": str(exc),
                            },
                            cleanup_unverified=True,
                            process_cleanup_unverified=True,
                        )
            else:
                _kill_pgroup(
                    proc.pid,
                    hard_kill_deadline=hard_kill_deadline,
                    clock=clock,
                )
                if state_mode is not None:
                    _update_entry(
                        state_mode,
                        termination_evidence={
                            "cleanup_verified": False,
                            "fallback_process_group": proc.pid,
                            "reason": "process identity capture unavailable",
                        },
                        cleanup_unverified=True,
                        process_cleanup_unverified=True,
                    )
        if proc.poll() is None:
            try:
                wait_timeout = 5.0
                if hard_kill_deadline is not None:
                    wait_timeout = min(
                        wait_timeout,
                        max(0.0, hard_kill_deadline - clock()),
                    )
                if wait_timeout > 0.0:
                    proc.wait(timeout=wait_timeout)
            except subprocess.TimeoutExpired:
                if tracker is not None:
                    raise _GuardedTerminationError(
                        "guarded subprocess root did not exit after cleanup",
                        {
                            "root": tracker.root.__dict__,
                            "cleanup_verified": False,
                        },
                    )
        if proc.stdout is not None:
            proc.stdout.close()

def _build_worker_descendant_environment(
        base_env: dict[str, str] | None = None,
        *, allowed_paths=None) -> dict[str, str]:
    """Validate the neutral selector and strip raw dyld state for workers."""
    try:
        validation = {} if allowed_paths is None else {
            "allowed_paths": allowed_paths}
        return sanitizer_descendant_environment(
            os.environ if base_env is None else base_env, **validation)
    except SanitizerRuntimeError as exc:
        raise RuntimeError(str(exc)) from exc

def _trusted_root_owned_executable(path: str) -> Path:
    """Resolve and validate an executable before crossing to another OS uid."""
    try:
        resolved = Path(path).resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        raise RuntimeError(f"cannot resolve trusted executable {path}: {exc}") from exc
    _require_trusted_root_owned_path(resolved, executable=True)
    for directory in resolved.parents:
        try:
            metadata = directory.stat()
        except OSError as exc:
            raise RuntimeError(
                f"cannot stat executable parent {directory}: {exc}") from exc
        if metadata.st_uid != 0 or metadata.st_mode & 0o022:
            raise RuntimeError(
                f"executable parent {directory} is not root-owned and "
                "non-writable")
    return resolved

_ACTIVITY_VERIFIED_IDLE_STATES = {"completed", "failed"}

def _activity_cleanup_is_unknown(entry: Mapping[str, object]) -> bool:
    if (
        "cleanup_unverified" in entry
        and entry.get("cleanup_unverified") is not False
    ):
        return True
    evidence = entry.get("termination_evidence")
    if evidence is None:
        return False
    if not isinstance(evidence, dict) or evidence.get("cleanup_verified") is not True:
        return True
    for field in (
        "survivor_pids", "marker_survivor_pids", "survivor_identities",
    ):
        survivors = evidence.get(field, [])
        if not isinstance(survivors, list) or survivors:
            return True
    return False

def _transient_activity(
        entries: list[object], *, registry_present: bool = False) -> dict:
    """Summarize registered detached work without leaking paths or commands."""
    queued = 0
    running = 0
    unknown = False
    recognized = False
    for raw_entry in entries:
        if not isinstance(raw_entry, dict):
            unknown = True
            continue
        recognized = True
        status = raw_entry.get("status")
        if _activity_cleanup_is_unknown(raw_entry):
            unknown = True
        elif status == "queued":
            queued += 1
        elif status == "running":
            if _is_pid_alive(raw_entry.get("pid")):
                running += 1
            else:
                unknown = True
        elif status in _ACTIVITY_VERIFIED_IDLE_STATES:
            continue
        elif status == "killed_timeout":
            if raw_entry.get("termination_evidence") is None:
                unknown = True
        else:
            unknown = True
    if unknown:
        state = "unknown"
    elif running:
        state = "running"
    elif queued:
        state = "queued"
    elif recognized or registry_present:
        state = "idle"
    else:
        state = "absent"
    return {"queued": queued, "running": running, "state": state}

def _read_activity_registry(path: Path) -> tuple[dict | None, bool]:
    """Read an existing manager registry; malformed state remains unknown."""
    if not path.exists():
        return {}, False
    try:
        with path.open("r", encoding="utf-8") as handle:
            value = json.load(handle)
    except (OSError, UnicodeError, json.JSONDecodeError):
        return None, True
    return (value, True) if isinstance(value, dict) else (None, True)



REMOTE_STATE_FILE = Path("/tmp/bioseq_remote_mcp_state.json")

def _tail_path(path_text: str | None, lines: int, cap: int = 14000) -> str:
    if not path_text:
        return "(no log path)"
    path = Path(path_text)
    try:
        data = path.read_text(errors="replace")
    except OSError as e:
        return f"(cannot read {path}: {e})"
    tail = "\n".join(data.splitlines()[-max(1, lines):])
    if len(tail) > cap:
        tail = "…(truncated)…\n" + tail[-cap:]
    return tail or "(empty log)"
