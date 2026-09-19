"""Independent whole-invocation deadline guard for q1024 E2E runs."""

from __future__ import annotations

import argparse
import os
import re
import sys
import time
from pathlib import Path

from postgres_runtime import (
    _cleanup_owned_postgresql_backends,
    _postgresql_recovery_service_boundary_provider,
)
from process_state import (
    BUILD_GUARD_WORKER_ARM_TIMEOUT_SEC,
    BUILD_TIMEOUT_TERM_GRACE_SEC,
    _ProcessIdentity,
    _capture_process_identity,
    _claim_q1024_postgresql_cleanup,
    _finish_q1024_postgresql_cleanup,
    _is_pid_alive,
    _load_state,
    _process_is_zombie,
    _recover_guarded_worker_descendants,
    _same_process,
    _same_process_context,
    _terminate_validated_process_tree,
    _update_q1024_watchdog_state,
)

_Q1024_MAX_WHOLE_SECONDS = 43_200.0
_Q1024_CLEANUP_RESERVE_SECONDS = 300.0
_WATCHDOG_POLL_SECONDS = 1.0


def _identity(value: object) -> _ProcessIdentity | None:
    if not isinstance(value, dict):
        return None
    try:
        return _ProcessIdentity(**value)
    except (TypeError, ValueError):
        return None


def _owned_entry(mode: str, run_token: str) -> dict[str, object]:
    entry = _load_state().get(mode)
    if (not isinstance(entry, dict)
            or entry.get("q1024_run_token") != run_token):
        raise RuntimeError("q1024 watchdog ownership disappeared")
    return dict(entry)


def _validated_deadlines(
        entry: dict[str, object]) -> tuple[float, float, float]:
    try:
        started_at = float(entry["started_at"])
        work_deadline = float(entry["q1024_work_deadline"])
        client_kill_deadline = float(entry["q1024_client_kill_deadline"])
        whole_deadline = float(entry["q1024_whole_deadline"])
    except (KeyError, TypeError, ValueError) as exc:
        raise RuntimeError("q1024 watchdog deadlines are malformed") from exc
    if (whole_deadline - started_at > _Q1024_MAX_WHOLE_SECONDS
            or abs(
                whole_deadline - client_kill_deadline
                - _Q1024_CLEANUP_RESERVE_SECONDS) > 0.001
            or abs(
                client_kill_deadline - work_deadline
                - BUILD_TIMEOUT_TERM_GRACE_SEC) > 0.001
            or not started_at < work_deadline < client_kill_deadline
                < whole_deadline):
        raise RuntimeError("q1024 watchdog deadline ordering is invalid")
    return work_deadline, client_kill_deadline, whole_deadline


def _await_own_arm(mode: str, run_token: str) -> dict[str, object]:
    arm_deadline = time.monotonic() + BUILD_GUARD_WORKER_ARM_TIMEOUT_SEC
    while time.monotonic() < arm_deadline:
        entry = _owned_entry(mode, run_token)
        expected = _identity(entry.get("q1024_watchdog_process_identity"))
        if (entry.get("q1024_watchdog_status") == "armed"
                and expected is not None
                and expected.pid == os.getpid()
                and _same_process_context(
                    _capture_process_identity(os.getpid()), expected)):
            return entry
        time.sleep(0.02)
    raise RuntimeError("q1024 watchdog did not receive its exact arm")


def _publish_failure(
        mode: str, run_token: str, reason: str,
        *, terminal_status: str | None = None) -> None:
    fields: dict[str, object] = {
        "q1024_watchdog_status": "failed",
        "q1024_watchdog_evidence": {
            "cleanup_verified": False,
            "reason": reason,
            "completed_at": time.time(),
        },
        "cleanup_unverified": True,
    }
    if terminal_status is not None:
        fields.update({
            "status": terminal_status,
            "step": terminal_status,
            "completed_at": time.time(),
            "exit_code": None,
            "notes": reason,
        })
    _update_q1024_watchdog_state(mode, run_token, **fields)


def _recover_dead_worker_processes(
        entry: dict[str, object],
        *, hard_kill_deadline: float,
) -> tuple[dict[str, object], bool]:
    """Clean only an exact active root or Linux descendants with its token."""
    service_boundary_provider = (
        _postgresql_recovery_service_boundary_provider(entry))
    marker = entry.get("active_guard_marker")
    guard_marker = marker if isinstance(marker, str) else ""
    active = _identity(entry.get("active_process_identity"))
    if active is not None:
        observed = _capture_process_identity(
            active.pid, deadline=hard_kill_deadline)
        if _same_process(observed, active):
            evidence = _terminate_validated_process_tree(
                active,
                hard_kill_deadline=hard_kill_deadline,
                guard_marker=guard_marker,
                service_boundary_provider=service_boundary_provider,
            )
            return evidence, evidence.get("cleanup_verified") is True
        if observed is not None:
            return ({
                "cleanup_verified": False,
                "error": "active client PID was reused; no signal sent",
                "expected": active.__dict__,
                "observed": observed.__dict__,
            }, False)
    marker = entry.get("active_guard_marker")
    if (not sys.platform.startswith("linux")
            or not isinstance(marker, str)
            or re.fullmatch(r"[0-9a-f]{32}", marker) is None):
        return ({
            "cleanup_verified": False,
            "error": (
                "dead worker left no live exact client root and this host "
                "cannot prove a token-only descendant sweep"),
        }, False)
    evidence, unverified = _recover_guarded_worker_descendants(
        entry,
        strict_marker_only=True,
        hard_kill_deadline=hard_kill_deadline,
        service_boundary_provider=service_boundary_provider,
    )
    return evidence, not unverified


def _cleanup_postgresql_owner(
        mode: str,
        run_token: str,
        entry: dict[str, object],
        whole_deadline: float,
) -> bool:
    if entry.get("postgres_backend_owner_started") is not True:
        return entry.get("postgres_cleanup_unverified") is not True
    claim = _claim_q1024_postgresql_cleanup(
        mode, run_token, actor="watchdog")
    if claim is None:
        return False
    claim_token, claimed_entry = claim
    identity = claimed_entry.get("postgres_backend_identity")
    if (not isinstance(identity, dict)
            or identity.get("application_name")
                != claimed_entry.get("postgres_application_name")
            or not isinstance(identity.get("database"), str)
            or not isinstance(identity.get("user"), str)):
        evidence: dict[str, object] = {
            "cleanup_verified": False,
            "error": "armed PostgreSQL owner scope is missing or contradictory",
        }
    else:
        try:
            evidence = _cleanup_owned_postgresql_backends(
                Path(str(claimed_entry["build_dir"])),
                str(claimed_entry["preset"]),
                application_name=str(identity["application_name"]),
                database_name=str(identity["database"]),
                database_user=str(identity["user"]),
                log_handle=sys.stderr,
                deadline=whole_deadline,
                allow_sigkill_backend=True,
            )
        except (OSError, RuntimeError) as exc:
            evidence = {"cleanup_verified": False, "error": str(exc)}
    return _finish_q1024_postgresql_cleanup(
        mode, run_token, claim_token, evidence)


def run_watchdog(mode: str, run_token: str) -> int:
    if mode != "benchmark-e2e" or re.fullmatch(
            r"[0-9a-f]{32}", run_token) is None:
        return 2
    try:
        entry = _await_own_arm(mode, run_token)
        work_deadline, client_kill_deadline, whole_deadline = (
            _validated_deadlines(entry))
        expected_worker = _identity(entry.get("worker_process_identity"))
        if expected_worker is None:
            raise RuntimeError("q1024 worker has no exact process identity")
        _update_q1024_watchdog_state(
            mode,
            run_token,
            q1024_watchdog_status="monitoring",
            q1024_watchdog_evidence={
                "cleanup_verified": False,
                "phase": "monitoring",
                "armed_at": time.time(),
                "worker_process_identity": expected_worker.__dict__,
            },
        )

        while True:
            now = time.time()
            entry = _owned_entry(mode, run_token)
            pid_still_alive = _is_pid_alive(expected_worker.pid)
            worker_terminal = entry.get("status") in {
                "completed", "failed", "aborted", "killed_timeout"}

            should_intervene = (
                now >= work_deadline
                or (worker_terminal and not pid_still_alive)
                or not pid_still_alive)
            observed_worker: _ProcessIdentity | None = None
            worker_zombie = False
            worker_replaced = False
            if worker_terminal and pid_still_alive:
                observed_worker = _capture_process_identity(
                    expected_worker.pid, deadline=whole_deadline)
                worker_replaced = (
                    observed_worker is not None
                    and not _same_process(observed_worker, expected_worker))
                worker_zombie = (
                    _same_process(observed_worker, expected_worker)
                    and _process_is_zombie(
                        expected_worker, deadline=whole_deadline))
                should_intervene = (
                    should_intervene or worker_zombie or worker_replaced)
            if (entry.get("status") == "completed"
                    and (not pid_still_alive or worker_zombie or worker_replaced)
                    and entry.get("postgres_backend_owner_started") is not True
                    and entry.get("postgres_cleanup_unverified") is not True
                    and entry.get("cleanup_unverified") is not True
                    and float(entry.get("completed_at") or whole_deadline + 1)
                        <= whole_deadline):
                _update_q1024_watchdog_state(
                    mode,
                    run_token,
                    q1024_watchdog_status="completed",
                    q1024_watchdog_evidence={
                        "cleanup_verified": True,
                        "phase": "worker_completed",
                        "worker_exit_observation": (
                            "exact_zombie" if worker_zombie else
                            "pid_reused" if worker_replaced else "absent"),
                        "completed_at": time.time(),
                    },
                )
                return 0
            if not should_intervene:
                time.sleep(min(
                    _WATCHDOG_POLL_SECONDS,
                    max(0.0, work_deadline - now)))
                continue

            process_evidence: dict[str, object]
            process_verified = False
            if time.time() >= whole_deadline:
                raise TimeoutError(
                    "q1024 whole deadline reached before cleanup began")
            if observed_worker is None and pid_still_alive:
                observed_worker = _capture_process_identity(
                    expected_worker.pid, deadline=client_kill_deadline)
            worker_alive = (
                _same_process(observed_worker, expected_worker)
                and not worker_zombie)
            if worker_alive:
                marker = entry.get("active_guard_marker")
                process_evidence = _terminate_validated_process_tree(
                    expected_worker,
                    hard_kill_deadline=client_kill_deadline,
                    guard_marker=(
                        marker if isinstance(marker, str) else ""),
                    service_boundary_provider=(
                        _postgresql_recovery_service_boundary_provider(entry)),
                )
                process_verified = (
                    process_evidence.get("cleanup_verified") is True)
            elif worker_zombie:
                if (time.time() + 2 * BUILD_TIMEOUT_TERM_GRACE_SEC
                        >= whole_deadline):
                    process_evidence = {
                        "cleanup_verified": False,
                        "error": (
                            "insufficient whole-deadline reserve for exact "
                            "zombie-worker descendant sweep"),
                    }
                else:
                    process_evidence, process_verified = (
                        _recover_dead_worker_processes(
                            entry,
                            hard_kill_deadline=client_kill_deadline,
                        ))
            elif pid_still_alive and observed_worker is None:
                process_evidence = {
                    "cleanup_verified": False,
                    "error": (
                        "q1024 worker is live but its exact identity could "
                        "not be revalidated; no signal sent"),
                    "expected": expected_worker.__dict__,
                }
            elif observed_worker is not None:
                process_evidence = {
                    "cleanup_verified": False,
                    "error": "q1024 worker PID was reused; no signal sent",
                    "expected": expected_worker.__dict__,
                    "observed": observed_worker.__dict__,
                }
            else:
                if (time.time() + 2 * BUILD_TIMEOUT_TERM_GRACE_SEC
                        >= whole_deadline):
                    process_evidence = {
                        "cleanup_verified": False,
                        "error": (
                            "insufficient whole-deadline reserve for exact "
                            "worker-death descendant sweep"),
                    }
                else:
                    process_evidence, process_verified = (
                        _recover_dead_worker_processes(
                            entry,
                            hard_kill_deadline=client_kill_deadline,
                        ))

            _update_q1024_watchdog_state(
                mode,
                run_token,
                termination_evidence=process_evidence,
                process_cleanup_unverified=not process_verified,
                cleanup_unverified=(
                    not process_verified
                    or entry.get("postgres_cleanup_unverified") is True),
            )
            entry = _owned_entry(mode, run_token)
            postgres_verified = (
                _cleanup_postgresql_owner(
                    mode, run_token, entry, whole_deadline)
                if process_verified else False)
            finished_at = time.time()
            within_deadline = finished_at <= whole_deadline
            cleanup_verified = (
                process_verified and postgres_verified and within_deadline)
            terminal_status = (
                "killed_timeout" if now >= work_deadline else "aborted")
            _update_q1024_watchdog_state(
                mode,
                run_token,
                status=terminal_status,
                step=terminal_status,
                completed_at=finished_at,
                exit_code=None,
                cleanup_unverified=not cleanup_verified,
                q1024_watchdog_status=(
                    "completed" if cleanup_verified else "failed"),
                q1024_watchdog_evidence={
                    "cleanup_verified": cleanup_verified,
                    "process_cleanup_verified": process_verified,
                    "postgres_cleanup_verified": postgres_verified,
                    "within_whole_deadline": within_deadline,
                    "completed_at": finished_at,
                },
                notes=(
                    "q1024 whole-invocation watchdog enforced the deadline"
                    if cleanup_verified else
                    "q1024 watchdog cleanup could not be verified within "
                    "the whole-invocation deadline"),
            )
            return 0 if cleanup_verified else 1
    except Exception as exc:
        _publish_failure(mode, run_token, str(exc), terminal_status="aborted")
        return 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", required=True)
    parser.add_argument("--run-token", required=True)
    arguments = parser.parse_args()
    return run_watchdog(arguments.mode, arguments.run_token)


if __name__ == "__main__":
    raise SystemExit(main())
