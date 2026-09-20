"""PostgreSQL runtime lifecycle support."""

from __future__ import annotations

from typing import List, Dict, Set, Optional, Any, Mapping

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

from build_presets import (
    CMAKE_PRESETS_FILE,
    REPOSITORY_ROOT,
    _cmake_cache_value,
    _cmake_preset_cache_contract,
    _is_aws_preset,
)
from integrity import sha256_file
from process_state import (
    BUILD_GUARD_ENV_KEY,
    _GuardedServiceBoundary,
    _GuardedServiceBoundaryProvider,
    _ProcessIdentity,
    _capture_process_identity,
    _load_state,
    _log,
    _process_descendant_closure,
    _persisted_guarded_service_boundary_provider,
    _process_snapshot,
    _publish_q1024_postgresql_service_boundary,
    _same_process,
    _same_process_context,
    _stream_subprocess,
    _trusted_root_owned_executable,
)

_Q1024_SERVICE_BOUNDARY_ARM_TIMEOUT_SECONDS = 5.0

_CMAKE_HELPER_DIR = REPOSITORY_ROOT / "cmake"
if str(_CMAKE_HELPER_DIR) not in sys.path:
    sys.path.insert(0, str(_CMAKE_HELPER_DIR))



from sanitizer_runtime import (
    SANITIZER_RUNTIME_ENV, SanitizerRuntimeError,
    postgres_sanitizer_environment, sanitizer_descendant_environment,
)



_POSTGRESQL_DISCOVERY_CACHE_UNSET_ARGS = (
    "-UPostgreSQL_*",
    "-U_PostgreSQL_*",
    "-UFIND_PACKAGE_MESSAGE_DETAILS_PostgreSQL",
)

_LIBPQ_CONNECTION_ENVIRONMENT_KEYS = (
    "PGAPPNAME",
    "PGCHANNELBINDING",
    "PGCLIENTENCODING",
    "PGCONNECT_TIMEOUT",
    "PGDATABASE",
    "PGGSSENCMODE",
    "PGGSSLIB",
    "PGHOST",
    "PGHOSTADDR",
    "PGKRBSRVNAME",
    "PGLOADBALANCEHOSTS",
    "PGOPTIONS",
    "PGPASSFILE",
    "PGPASSWORD",
    "PGPORT",
    "PGREQUIREAUTH",
    "PGREQUIREPEER",
    "PGREQUIRESSL",
    "PGSERVICE",
    "PGSERVICEFILE",
    "PGSSLCERT",
    "PGSSLCOMPRESSION",
    "PGSSLCRL",
    "PGSSLCRLDIR",
    "PGSSLKEY",
    "PGSSLMAXPROTOCOLVERSION",
    "PGSSLMINPROTOCOLVERSION",
    "PGSSLMODE",
    "PGSSLNEGOTIATION",
    "PGSSLROOTCERT",
    "PGSSLSNI",
    "PGSYSCONFDIR",
    "PGTARGETSESSIONATTRS",
    "PGUSER",
)

_REQUIRED_MAX_PREPARED_TRANSACTIONS = 16


def _remaining_subprocess_timeout(
        deadline: float | None, maximum_seconds: float) -> float:
    if deadline is None:
        return maximum_seconds
    remaining = deadline - time.time()
    if remaining <= 0.0:
        raise TimeoutError("PostgreSQL runtime deadline was reached")
    return min(maximum_seconds, remaining)


@dataclass(frozen=True)
class _PostgreSQLFileIdentity:
    """Descriptor-sealed identity for one runtime-selected file."""

    path: str
    device: int
    inode: int
    size: int
    sha256: str


@dataclass(frozen=True)
class _PostgreSQLMappedFile:
    """Identity fields exposed by one live postmaster file mapping."""

    path: str
    device: int
    inode: int


def _seal_postgresql_file_identity(
        path: str, description: str) -> _PostgreSQLFileIdentity:
    """Hash one stable descriptor and reject non-regular runtime inputs."""
    try:
        resolved = Path(path).resolve(strict=True)
        flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
        flags |= getattr(os, "O_NOFOLLOW", 0)
        descriptor = os.open(resolved, flags)
    except OSError as exc:
        raise RuntimeError(
            f"could not open {description} {path!r}: {exc}") from exc
    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode):
            raise RuntimeError(
                f"{description} is not a regular file: {resolved}")
        digest = sha256_file(descriptor)
    finally:
        os.close(descriptor)
    return _PostgreSQLFileIdentity(
        path=str(resolved),
        device=metadata.st_dev,
        inode=metadata.st_ino,
        size=metadata.st_size,
        sha256=digest,
    )


def _same_postgresql_file_identity(
        first: _PostgreSQLFileIdentity,
        second: _PostgreSQLFileIdentity) -> bool:
    """Permit alternate hardlink paths only when the file object is exact."""
    return (
        first.device,
        first.inode,
        first.size,
        first.sha256,
    ) == (
        second.device,
        second.inode,
        second.size,
        second.sha256,
    )


def _postgresql_postmaster_pid(
        data_dir: str, active_preset: str,
        deadline: float | None = None) -> int:
    """Read the bounded postmaster PID file without consulting SQL."""
    pid_path = Path(data_dir) / "postmaster.pid"
    try:
        with pid_path.open("r", encoding="ascii") as handle:
            first_line = handle.readline(64)
    except (OSError, UnicodeError) as exc:
        if not _is_aws_preset(active_preset):
            raise RuntimeError(
                f"could not read PostgreSQL postmaster PID from {pid_path}: "
                f"{exc}") from exc
        # AWS clusters normally keep their data directory postgres-only.
        # Reuse the same trusted UID boundary used for their process maps.
        body = _postgresql_aws_read_process_file(pid_path, deadline)
        first, newline, _ = body.partition("\n")
        first_line = (first + newline)[:64]
    if not first_line.endswith("\n") or not first_line[:-1].isdigit():
        raise RuntimeError(
            "PostgreSQL postmaster PID file has an invalid first line: "
            f"{pid_path}")
    pid = int(first_line, 10)
    if pid <= 1:
        raise RuntimeError(f"PostgreSQL postmaster PID is invalid: {pid}")
    return pid


def _postgresql_aws_read_process_file(
        path: Path, deadline: float | None = None) -> str:
    """Read postgres-owned runtime metadata through the existing AWS boundary."""
    selected = shutil.which("cat")
    if not selected:
        raise RuntimeError(
            "cannot inspect PostgreSQL runtime: cat was not found")
    trusted_executable = _trusted_root_owned_executable(selected)
    trusted_sudo = _trusted_root_owned_executable("/usr/bin/sudo")
    try:
        result = subprocess.run(
            [str(trusted_sudo), "-n", "-u", "postgres", "--",
             str(trusted_executable), str(path)],
            capture_output=True,
            text=True,
            timeout=_remaining_subprocess_timeout(deadline, 10.0),
            env=_postgresql_admin_environment(),
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError(
            f"could not inspect {path} through the AWS postgres-uid "
            f"fallback: {exc}") from exc
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        raise RuntimeError(
            f"could not inspect {path} through the AWS postgres-uid "
            f"fallback: {detail or f'exit {result.returncode}'}")
    return result.stdout


def _linux_postgresql_mapped_files(
        pid: int, active_preset: str,
        deadline: float | None = None) -> tuple[_PostgreSQLMappedFile, ...]:
    maps_path = Path("/proc") / str(pid) / "maps"
    try:
        maps_text = maps_path.read_text(encoding="utf-8", errors="replace")
    except OSError as direct_error:
        if not _is_aws_preset(active_preset):
            raise RuntimeError(
                f"could not inspect PostgreSQL postmaster {pid} maps: "
                f"{direct_error}") from direct_error
        maps_text = _postgresql_aws_read_process_file(maps_path, deadline)

    mappings: set[_PostgreSQLMappedFile] = set()
    for line in maps_text.splitlines():
        fields = line.split(maxsplit=5)
        if len(fields) != 6 or ":" not in fields[3]:
            continue
        major_text, minor_text = fields[3].split(":", 1)
        try:
            device = os.makedev(
                int(major_text, 16), int(minor_text, 16))
            inode = int(fields[4], 10)
        except ValueError:
            continue
        if inode > 0:
            mappings.add(_PostgreSQLMappedFile(
                path=fields[5], device=device, inode=inode))
    return tuple(sorted(
        mappings, key=lambda value: (value.device, value.inode, value.path)))


def _lsof_postgresql_mapped_files(
        pid: int,
        deadline: float | None = None) -> tuple[_PostgreSQLMappedFile, ...]:
    lsof = (
        "/usr/sbin/lsof"
        if Path("/usr/sbin/lsof").is_file()
        else shutil.which("lsof")
    )
    if not lsof:
        raise RuntimeError(
            "cannot inspect PostgreSQL postmaster mappings: lsof not found")
    try:
        result = subprocess.run(
            [lsof, "-nP", "-a", "-p", str(pid), "-F0fDin"],
            capture_output=True,
            timeout=_remaining_subprocess_timeout(deadline, 15.0),
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError(
            f"could not inspect PostgreSQL postmaster {pid} mappings: "
            f"{exc}") from exc
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(
            f"could not inspect PostgreSQL postmaster {pid} mappings: "
            f"{detail or f'exit {result.returncode}'}")

    mappings: set[_PostgreSQLMappedFile] = set()
    for record in result.stdout.splitlines():
        fields: dict[str, str] = {}
        for raw_field in record.split(b"\0"):
            if not raw_field:
                continue
            field = raw_field.decode("utf-8", errors="replace")
            fields[field[0]] = field[1:]
        if fields.get("f") not in {"txt", "mem"}:
            continue  # An ordinary open descriptor does not prove a mapping.
        try:
            device = int(fields["D"], 0)
            inode = int(fields["i"], 10)
            path = fields["n"]
        except (KeyError, ValueError):
            continue
        mappings.add(_PostgreSQLMappedFile(
            path=path, device=device, inode=inode))
    return tuple(sorted(
        mappings, key=lambda value: (value.device, value.inode, value.path)))


def _postgresql_mapped_files(
        pid: int, active_preset: str,
        deadline: float | None = None) -> tuple[_PostgreSQLMappedFile, ...]:
    if sys.platform.startswith("linux"):
        return _linux_postgresql_mapped_files(
            pid, active_preset, deadline)
    return _lsof_postgresql_mapped_files(pid, deadline)


def _verify_postgresql_runtime_identity(
        data_dir: str,
        active_preset: str,
        expected_postgres: _PostgreSQLFileIdentity,
        expected_bioseq_dso: _PostgreSQLFileIdentity,
        log_handle,
        deadline: float | None = None) -> _ProcessIdentity:
    """Prove executable and mapped DSO identities before post-restart SQL."""
    current_postgres = _seal_postgresql_file_identity(
        expected_postgres.path, "selected PostgreSQL server executable")
    current_dso = _seal_postgresql_file_identity(
        expected_bioseq_dso.path, "installed bioseq DSO")
    if not _same_postgresql_file_identity(
            expected_postgres, current_postgres):
        raise RuntimeError(
            "selected PostgreSQL server executable changed during restart; "
            "refusing SQL")
    if not _same_postgresql_file_identity(expected_bioseq_dso, current_dso):
        raise RuntimeError(
            "installed bioseq DSO changed during restart; refusing SQL")

    pid = _postgresql_postmaster_pid(data_dir, active_preset, deadline)
    initial_snapshot = _process_snapshot(deadline)
    postmaster_identity = initial_snapshot.get(pid)
    if postmaster_identity is None:
        raise RuntimeError(
            "PostgreSQL postmaster disappeared before runtime identity "
            "verification")
    mappings = _postgresql_mapped_files(pid, active_preset, deadline)
    mapped_postgres = next((
        mapping for mapping in mappings
        if (mapping.device, mapping.inode) == (
            expected_postgres.device, expected_postgres.inode)
    ), None)
    if mapped_postgres is None:
        raise RuntimeError(
            "PostgreSQL postmaster does not map the sealed selected server "
            "executable; refusing SQL: "
            f"expected={expected_postgres}")

    mapped_dso = next((
        mapping for mapping in mappings
        if (
            mapping.device,
            mapping.inode,
        ) == (
            expected_bioseq_dso.device,
            expected_bioseq_dso.inode,
        )
    ), None)
    if mapped_dso is None:
        raise RuntimeError(
            "PostgreSQL postmaster does not map the sealed installed bioseq "
            "DSO; refusing SQL: "
            f"expected={expected_bioseq_dso}")
    stable_pid = _postgresql_postmaster_pid(
        data_dir, active_preset, deadline)
    final_snapshot = _process_snapshot(deadline)
    if (
        stable_pid != pid
        or not _same_process_context(
            final_snapshot.get(stable_pid), postmaster_identity)
    ):
        raise RuntimeError(
            "PostgreSQL postmaster changed during runtime identity "
            f"verification: before={pid} after={stable_pid}")

    log_handle.write(
        "# verified PostgreSQL runtime identity "
        f"pid={pid} postgres_path={mapped_postgres.path} "
        f"postgres_device={expected_postgres.device} "
        f"postgres_inode={expected_postgres.inode} "
        f"postgres_size={expected_postgres.size} "
        f"postgres_sha256={expected_postgres.sha256} "
        f"bioseq_path={mapped_dso.path} "
        f"bioseq_device={expected_bioseq_dso.device} "
        f"bioseq_inode={expected_bioseq_dso.inode} "
        f"bioseq_size={expected_bioseq_dso.size} "
        f"bioseq_sha256={expected_bioseq_dso.sha256}\n")
    log_handle.flush()
    return postmaster_identity


def _postgresql_data_directory_identity(data_dir: str) -> dict[str, object]:
    path = Path(data_dir)
    resolved = path.resolve(strict=True)
    metadata = resolved.stat()
    if resolved != path or not stat.S_ISDIR(metadata.st_mode):
        raise RuntimeError(
            "PostgreSQL data directory is not one canonical directory")
    return {
        "path": str(resolved),
        "device": metadata.st_dev,
        "inode": metadata.st_ino,
    }


def _postgresql_service_boundary_evidence(
        *,
        mode: str,
        run_token: str,
        application_name: str,
        client_identity: _ProcessIdentity,
        guard_marker: str,
        data_dir: str,
        postgres_identity: _PostgreSQLFileIdentity,
        bioseq_identity: _PostgreSQLFileIdentity,
        postmaster_identity: _ProcessIdentity,
) -> dict[str, object]:
    return {
        "schema_version": 1,
        "mode": mode,
        "run_token": run_token,
        "application_name": application_name,
        "client_process_identity": client_identity.__dict__,
        "guard_marker": guard_marker,
        "data_directory": _postgresql_data_directory_identity(data_dir),
        "postgres_executable_identity": postgres_identity.__dict__,
        "bioseq_dso_identity": bioseq_identity.__dict__,
        "postmaster_process_identity": postmaster_identity.__dict__,
        "runtime_identity_verified": True,
        "verified_at": time.time(),
    }


def _plausible_postgresql_service_roots(
        snapshot: Mapping[int, _ProcessIdentity],
        candidate_pids: Set[int],
) -> tuple[_ProcessIdentity, ...]:
    """Classify possible postmaster trees; classification never proves one."""
    service_pattern = re.compile(
        r"^(?:\S*/)?(?:postgres|postmaster)(?=$|[ :])",
        re.IGNORECASE,
    )
    candidates = {
        pid: process
        for pid, process in snapshot.items()
        if pid in candidate_pids and service_pattern.search(process.command)
    }
    roots = []
    for pid, process in candidates.items():
        ancestor = snapshot.get(process.ppid)
        has_candidate_ancestor = False
        visited: set[int] = set()
        while ancestor is not None and ancestor.pid not in visited:
            visited.add(ancestor.pid)
            if ancestor.pid in candidates:
                has_candidate_ancestor = True
                break
            ancestor = snapshot.get(ancestor.ppid)
        if not has_candidate_ancestor:
            roots.append(process)
    return tuple(sorted(roots, key=lambda value: value.pid))


def _merged_postgresql_boundary(
        observation: _GuardedServiceBoundary,
        candidates: tuple[_ProcessIdentity, ...],
) -> _GuardedServiceBoundary:
    quarantined = [*observation.quarantined_roots]
    if observation.quarantined_root is not None:
        quarantined.append(observation.quarantined_root)
    quarantined.extend(candidates)
    unique: dict[tuple[int, str], _ProcessIdentity] = {}
    for candidate in quarantined:
        if not _same_process(candidate, observation.verified_root):
            unique[(candidate.pid, candidate.started)] = candidate
    return _GuardedServiceBoundary(
        verified_root=observation.verified_root,
        quarantined_root=None,
        quarantined_roots=tuple(
            unique[key] for key in sorted(unique)),
        evidence=dict(observation.evidence),
    )


@dataclass
class _PostgreSQLGuardServiceBoundaryProvider:
    mode: str
    run_token: str
    application_name: str
    active_build_dir: Path
    active_preset: str
    log_handle: object
    _data_dir: str | None = None
    _postgres_identity: _PostgreSQLFileIdentity | None = None
    _bioseq_identity: _PostgreSQLFileIdentity | None = None
    _last_service_candidate: _ProcessIdentity | None = None

    def _cached_quarantine(
            self,
            snapshot: Mapping[int, _ProcessIdentity],
            candidates: tuple[_ProcessIdentity, ...],
            evidence: Mapping[str, object],
            extra: tuple[_ProcessIdentity, ...] = (),
    ) -> _GuardedServiceBoundary:
        cached = (
            snapshot.get(self._last_service_candidate.pid)
            if self._last_service_candidate is not None else None)
        cached = (
            self._last_service_candidate
            if _same_process(cached, self._last_service_candidate) else None)
        boundary = _GuardedServiceBoundary(
            verified_root=None,
            quarantined_root=None,
            quarantined_roots=tuple(
                [*extra] + ([cached] if cached is not None else [])),
            evidence=dict(evidence),
        )
        return _merged_postgresql_boundary(boundary, candidates)

    def _runtime_selection(
            self, deadline: float | None,
    ) -> tuple[str, _PostgreSQLFileIdentity, _PostgreSQLFileIdentity]:
        if (
            self._data_dir is not None
            and self._postgres_identity is not None
            and self._bioseq_identity is not None
        ):
            return (
                self._data_dir,
                self._postgres_identity,
                self._bioseq_identity,
            )
        pg_config = _pg_config_from_cache(self.active_build_dir)
        pg_bindir = _postgresql_config_path(
            pg_config, "--bindir", deadline)
        pg_pkglibdir = _postgresql_config_path(
            pg_config, "--pkglibdir", deadline)
        postgres = str(Path(pg_bindir) / "postgres")
        bioseq_dso = _postgresql_bioseq_dso_path(pg_pkglibdir)
        data_dir = _query_postgresql_data_directory(
            str(Path(pg_bindir) / "psql"), self.active_preset, deadline)
        _postgresql_data_directory_identity(data_dir)
        self._data_dir = data_dir
        self._postgres_identity = _seal_postgresql_file_identity(
            postgres, "selected PostgreSQL server executable")
        self._bioseq_identity = _seal_postgresql_file_identity(
            bioseq_dso, "installed bioseq DSO")
        return data_dir, self._postgres_identity, self._bioseq_identity

    def __call__(
            self,
            snapshot: Mapping[int, _ProcessIdentity],
            client_identity: _ProcessIdentity,
            guard_marker: str,
            deadline: float | None,
            candidate_pids: Set[int],
    ) -> _GuardedServiceBoundary:
        candidates = _plausible_postgresql_service_roots(
            snapshot, candidate_pids)
        entry = _load_state().get(self.mode)
        if not isinstance(entry, dict):
            return self._cached_quarantine(snapshot, candidates, {
                "runtime_identity_verified": False,
                "error": (
                    "q1024 state disappeared during service-boundary "
                    "validation"),
            })
        persisted = _persisted_guarded_service_boundary_provider(entry)
        if persisted is not None:
            observation = persisted(
                snapshot, client_identity, guard_marker, deadline,
                candidate_pids)
            if (
                observation.verified_root is not None
                or observation.quarantined_root is not None
                or observation.quarantined_roots
            ):
                self._last_service_candidate = (
                    observation.verified_root
                    or observation.quarantined_root
                    or observation.quarantined_roots[0])
                return _merged_postgresql_boundary(observation, candidates)

        try:
            data_dir, postgres_identity, bioseq_identity = (
                self._runtime_selection(deadline))
            postmaster_pid = _postgresql_postmaster_pid(
                data_dir, self.active_preset, deadline)
        except (OSError, RuntimeError, ValueError,
                subprocess.SubprocessError) as exc:
            return self._cached_quarantine(snapshot, candidates, {
                    "runtime_identity_verified": False,
                    "error": f"{type(exc).__name__}: {exc}",
                })
        candidate = snapshot.get(postmaster_pid)
        if candidate is None:
            return self._cached_quarantine(snapshot, candidates, {
                    "runtime_identity_verified": False,
                    "error": (
                        "postmaster PID file named a process absent from the "
                        "complete guard snapshot"),
                    "postmaster_pid": postmaster_pid,
                })
        try:
            verified = _verify_postgresql_runtime_identity(
                data_dir,
                self.active_preset,
                postgres_identity,
                bioseq_identity,
                self.log_handle,
                deadline,
            )
            if not _same_process_context(candidate, verified):
                raise RuntimeError(
                    "guard snapshot and PostgreSQL verifier disagree")
            boundary = _postgresql_service_boundary_evidence(
                mode=self.mode,
                run_token=self.run_token,
                application_name=self.application_name,
                client_identity=client_identity,
                guard_marker=guard_marker,
                data_dir=data_dir,
                postgres_identity=postgres_identity,
                bioseq_identity=bioseq_identity,
                postmaster_identity=verified,
            )
            if not _publish_q1024_postgresql_service_boundary(
                self.mode,
                self.run_token,
                client_identity,
                guard_marker,
                boundary,
            ):
                raise RuntimeError(
                    "q1024 service-boundary state CAS was rejected")
            self._last_service_candidate = verified
            return _merged_postgresql_boundary(_GuardedServiceBoundary(
                verified_root=verified,
                quarantined_root=None,
                evidence=boundary,
            ), candidates)
        except (OSError, RuntimeError, ValueError,
                subprocess.SubprocessError) as exc:
            self._last_service_candidate = candidate
            return self._cached_quarantine(
                snapshot, candidates, {
                    "runtime_identity_verified": False,
                    "postmaster_process_identity": candidate.__dict__,
                    "error": f"{type(exc).__name__}: {exc}",
                }, extra=(candidate,))


def _postgresql_guard_service_boundary_provider(
        *,
        mode: str,
        run_token: str,
        application_name: str,
        active_build_dir: Path,
        active_preset: str,
        log_handle,
) -> _PostgreSQLGuardServiceBoundaryProvider:
    if (
        mode != "benchmark-e2e"
        or re.fullmatch(r"[0-9a-f]{32}", run_token) is None
        or application_name != f"bioseq-q1024-{run_token}"
    ):
        raise RuntimeError(
            "PostgreSQL guard service boundary is reserved for exact q1024")
    return _PostgreSQLGuardServiceBoundaryProvider(
        mode=mode,
        run_token=run_token,
        application_name=application_name,
        active_build_dir=active_build_dir,
        active_preset=active_preset,
        log_handle=log_handle,
    )


def _postgresql_recovery_service_boundary_provider(
        entry: Mapping[str, object],
) -> _GuardedServiceBoundaryProvider | None:
    """Quarantine plausible q1024 services even before proof is persisted."""
    persisted = _persisted_guarded_service_boundary_provider(entry)
    if (
        entry.get("mode") != "benchmark-e2e"
        or (
            entry.get("postgres_backend_owner_started") is not True
            and persisted is None
        )
    ):
        return persisted

    def provider(
            snapshot: Mapping[int, _ProcessIdentity],
            client_identity: _ProcessIdentity,
            guard_marker: str,
            deadline: float | None,
            candidate_pids: Set[int],
    ) -> _GuardedServiceBoundary:
        candidates = _plausible_postgresql_service_roots(
            snapshot, candidate_pids)
        if persisted is not None:
            observation = persisted(
                snapshot, client_identity, guard_marker, deadline,
                candidate_pids)
        else:
            observation = _GuardedServiceBoundary(
                verified_root=None,
                quarantined_root=None,
                evidence={
                    "source": "q1024_preboundary_postgresql_classifier",
                    "runtime_identity_verified": False,
                },
            )
        return _merged_postgresql_boundary(observation, candidates)

    return provider


def _legacy_q1024_global_postgresql_zero_evidence(
        *,
        entry: Mapping[str, object],
        active_build_dir: Path,
        active_preset: str,
        deadline: float,
) -> dict[str, object]:
    """Prove a legacy terminal run has no PostgreSQL process on this host."""
    evidence: dict[str, object] = {
        "schema_version": 1,
        "method": "complete_global_process_snapshot_zero_postgresql",
        "cleanup_verified": False,
        "deadline": deadline,
    }
    if deadline <= time.time():
        evidence["error"] = "legacy revalidation deadline expired"
        return evidence
    try:
        pg_config = _pg_config_from_cache(active_build_dir)
        pg_bindir = Path(_postgresql_config_path(
            pg_config, "--bindir", deadline)).resolve(strict=True)
        selected_postgres = (pg_bindir / "postgres").resolve(strict=True)
        selected_identity = _seal_postgresql_file_identity(
            str(selected_postgres),
            "legacy cleanup selected PostgreSQL executable",
        )
        cached_postgres = _cmake_cache_value(
            active_build_dir, "PostgreSQL_EXECUTABLE")
        if isinstance(cached_postgres, str) and cached_postgres:
            cached_identity = _seal_postgresql_file_identity(
                cached_postgres,
                "legacy cleanup configured PostgreSQL executable",
            )
        else:
            # FindPostgreSQL caches the server executable only on Apple (for
            # bundle_loader). Other platforms bind pg_config's selection to
            # the runtime identity already validated by the recovery claim.
            boundary = entry.get("postgres_service_boundary")
            recorded = (boundary.get("postgres_executable_identity")
                        if isinstance(boundary, dict) else None)
            if (not isinstance(boundary, dict)
                    or boundary.get("runtime_identity_verified") is not True
                    or not isinstance(recorded, dict)):
                raise RuntimeError(
                    "legacy build has no configured or verified PostgreSQL executable")
            try:
                cached_identity = _PostgreSQLFileIdentity(**recorded)
            except TypeError as exc:
                raise RuntimeError(
                    "invalid recorded PostgreSQL executable identity") from exc
        if not _same_postgresql_file_identity(
                selected_identity, cached_identity):
            raise RuntimeError(
                "pg_config bindir and configured PostgreSQL executable differ")
        snapshot = _process_snapshot(deadline, require_complete=True)
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        evidence["error"] = f"{type(exc).__name__}: {exc}"
        return evidence

    dead_identities: dict[str, dict[str, object]] = {}
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
            expected = None
        if expected is None:
            evidence["error"] = (
                f"legacy state has no exact {field_name}")
            return evidence
        observed = snapshot.get(expected.pid)
        dead_identities[field_name] = {
            "expected": expected.__dict__,
            "observed": (
                observed.__dict__ if observed is not None else None),
        }
        if _same_process(observed, expected):
            evidence["error"] = f"{field_name} remains live"
            evidence["dead_identity_checks"] = dead_identities
            return evidence

    selected_path = str(selected_postgres)
    candidate_pattern = re.compile(
        r"(?i)(?:^|[ /])(?:postgres(?:ql)?|postmaster|pg_ctl|psql)"
        r"(?=$|[: /])|bioseq_benchmark_e2e|q1024_watchdog\.py"
    )
    candidates = []
    for process in snapshot.values():
        reasons = []
        if selected_path in process.command:
            reasons.append("selected_postgres_path")
        if candidate_pattern.search(process.command):
            reasons.append("postgresql_process_name")
        if reasons:
            candidates.append({
                "process_identity": process.__dict__,
                "reasons": reasons,
            })
    evidence.update({
        "active_preset": active_preset,
        "active_build_dir": str(active_build_dir),
        "selected_postgres_identity": selected_identity.__dict__,
        "configured_postgres_identity": cached_identity.__dict__,
        "dead_identity_checks": dead_identities,
        "global_snapshot_process_count": len(snapshot),
        "postgresql_candidates": candidates,
        "snapshot_complete": True,
        "completed_at": time.time(),
    })
    if candidates:
        evidence["error"] = (
            "complete global snapshot contains PostgreSQL candidates")
        return evidence
    evidence["cleanup_verified"] = True
    return evidence

def _postgresql_discovery_cache_reset_args(
        build_dir: Path,
        preset: str,
        *,
        presets_file: Path = CMAKE_PRESETS_FILE,
        environment: Mapping[str, str] | None = None) -> tuple[str, ...]:
    """Invalidate derived PostgreSQL discovery when its toolchain is stale."""
    contract = _cmake_preset_cache_contract(
        preset,
        ("PostgreSQL_CONFIG",),
        presets_file=presets_file,
        environment=environment,
    )
    expected_config = contract.get("PostgreSQL_CONFIG")
    if expected_config is None:
        return ()
    cached_config = _cmake_cache_value(
        build_dir,
        "PostgreSQL_CONFIG",
    )
    cached_server = _cmake_cache_value(
        build_dir,
        "PostgreSQL_EXECUTABLE",
    )
    if cached_config != expected_config or cached_server is None:
        return _POSTGRESQL_DISCOVERY_CACHE_UNSET_ARGS

    expected_bin = Path(expected_config).parent
    cached_bin = Path(cached_server).parent
    try:
        same_bin = (
            cached_bin == expected_bin
            or cached_bin.resolve(strict=False)
                == expected_bin.resolve(strict=False)
        )
    except (OSError, RuntimeError):
        same_bin = False
    return () if same_bin else _POSTGRESQL_DISCOVERY_CACHE_UNSET_ARGS

def _cmake_install_cmd(active_build_dir: Path, active_preset: str) -> list[str]:
    cmake = shutil.which("cmake") or "cmake"
    cmd = [cmake, "--install", str(active_build_dir), "--component", "bioseq"]
    if _is_aws_preset(active_preset):
        return ["sudo", "-n", *cmd]
    return cmd

def _prepare_postgresql_test_database(
        active_build_dir: Path,
        active_preset: str,
        log_handle,
        deadline: float,
        *,
        env: Mapping[str, str] | None = None,
        hard_kill_deadline: float | None = None,
        state_mode: str | None = None,
        phase: str | None = None) -> None:
    """Make PostgreSQL match the integration-test connection defaults."""
    is_aws = (
        sys.platform.startswith("linux")
        and _is_aws_preset(active_preset)
    )
    log_handle.write(
        "\n# Preparing PostgreSQL test auth/database "
        "(postgres/postgres, database bioseq) ...\n")
    log_handle.flush()

    pg_config = _pg_config_from_cache(active_build_dir)
    try:
        bindir_result = subprocess.run(
            [pg_config, "--bindir"],
            capture_output=True,
            text=True,
            timeout=_remaining_subprocess_timeout(deadline, 10.0),
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError(
            f"pg_config --bindir failed while preparing PostgreSQL "
            f"test database: {exc}") from exc
    if bindir_result.returncode != 0:
        raise RuntimeError(
            "pg_config --bindir failed while preparing PostgreSQL "
            f"test database: {bindir_result.stderr.strip()}")

    pg_bindir = Path(bindir_result.stdout.strip())
    psql = pg_bindir / "psql"
    createdb = pg_bindir / "createdb"
    for executable in (psql, createdb):
        if not executable.is_file():
            raise RuntimeError(
                f"PostgreSQL test database tool not found: {executable}")

    # _query_postgresql_data_directory() has already proven that the
    # configured cluster admits this builder (or the AWS postgres uid) over
    # its Unix socket.  Use that administrator path to establish the test
    # role/password; trying the password before setting it would make clean
    # clusters circular.  Ambient libpq selectors must not redirect setup.
    test_env = dict(os.environ if env is None else env)
    for key in _LIBPQ_CONNECTION_ENVIRONMENT_KEYS:
        test_env.pop(key, None)

    role_argv = [
        str(psql),
        "-v", "ON_ERROR_STOP=1",
        "--dbname", "postgres",
        "--command", "ALTER ROLE postgres WITH LOGIN PASSWORD 'postgres';",
    ]
    if is_aws:
        role_argv = ["sudo", "-n", "-u", "postgres", *role_argv]

    rc = _stream_subprocess(
        role_argv,
        log_handle,
        deadline,
        env=test_env,
        hard_kill_deadline=hard_kill_deadline,
        state_mode=state_mode,
        phase=phase,
    )
    if rc != 0:
        raise RuntimeError(
            f"PostgreSQL test role setup exited with code {rc}")

    query = shlex.join([
        str(psql),
        "-Atq", "--dbname", "postgres",
        "--command",
        "SELECT 1 FROM pg_database WHERE datname = 'bioseq';",
    ])
    create = shlex.join([
        str(createdb),
        "--owner", "postgres", "bioseq",
    ])
    create_db_script = (
        "set -euo pipefail\n"
        f"if ! {query} | grep -qx 1; then\n"
        f"  {create}\n"
        "fi\n"
    )
    database_argv = ["/bin/bash", "-lc", create_db_script]
    if is_aws:
        database_argv = [
            "sudo", "-n", "-u", "postgres", *database_argv]
    rc = _stream_subprocess(
        database_argv,
        log_handle,
        deadline,
        env=test_env,
        hard_kill_deadline=hard_kill_deadline,
        state_mode=state_mode,
        phase=phase,
    )

    if rc != 0:
        raise RuntimeError(
            f"PostgreSQL test database setup exited with code {rc}")

def _run_postgresql_restart_cmd(
        restart_cmd: list[str], log_handle,
        *,
        env: dict[str, str] | None = None,
        data_dir: str,
        active_preset: str,
        expected_postgres: str,
        expected_bioseq_dso: str,
        deadline: float | None = None) -> None:
    postgres_identity = _seal_postgresql_file_identity(
        expected_postgres, "selected PostgreSQL server executable")
    bioseq_identity = _seal_postgresql_file_identity(
        expected_bioseq_dso, "installed bioseq DSO")
    pretty = " ".join(restart_cmd)
    log_handle.write(f"\n+ {pretty}\n")
    log_handle.flush()
    sys.stderr.write(f"\n+ {pretty}\n")
    sys.stderr.flush()
    try:
        result = subprocess.run(
            restart_cmd,
            env=env,
            stdout=log_handle,
            stderr=subprocess.STDOUT,
            timeout=_remaining_subprocess_timeout(deadline, 90.0),
        )
        log_handle.flush()
        if result.returncode != 0:
            msg = (
                f"PostgreSQL restart exited {result.returncode}; refusing to "
                "run with a potentially stale bioseq DSO")
            _log(f"ERROR: {msg}")
            raise RuntimeError(msg)
        try:
            _verify_postgresql_runtime_identity(
                data_dir,
                active_preset,
                postgres_identity,
                bioseq_identity,
                log_handle,
                deadline,
            )
        except RuntimeError as exc:
            _log(f"ERROR: {exc}")
            raise
        _log("PostgreSQL restarted with verified executable and bioseq DSO")
    except subprocess.TimeoutExpired as exc:
        msg = (
            "PostgreSQL restart timed out after 90s; refusing to run with a "
            "potentially stale bioseq DSO")
        _log(f"ERROR: {msg}")
        raise RuntimeError(msg) from exc

def _postgresql_admin_psql_argv(
        psql: str, active_preset: str, *arguments: str) -> list[str]:
    """Address the configured cluster through its proven local admin path."""
    argv = [psql, *arguments]
    if sys.platform.startswith("linux") and _is_aws_preset(active_preset):
        return ["sudo", "-n", "-u", "postgres", *argv]
    return argv

def _postgresql_admin_environment() -> dict[str, str]:
    """Prevent ambient libpq selectors from redirecting cluster mutation."""
    environment = dict(os.environ)
    for key in _LIBPQ_CONNECTION_ENVIRONMENT_KEYS:
        environment.pop(key, None)
    return environment


def _cleanup_owned_postgresql_backends(
        active_build_dir: Path,
        active_preset: str,
        *,
        application_name: str,
        database_name: str,
        database_user: str,
        log_handle,
        deadline: float,
        allow_sigkill_backend: bool = False) -> dict[str, object]:
    """Cancel, terminate, and verify only one reserved E2E backend owner.

    PostgreSQL backends are postmaster children rather than benchmark-client
    descendants, so process-group cleanup cannot reach them.  The opaque
    application name plus database and role form the complete cleanup scope.
    Every client invocation is independently bounded by the caller's whole-run
    deadline; failure to prove a zero count is returned fail-closed.
    """
    evidence: dict[str, object] = {
        "application_name": application_name,
        "database": database_name,
        "user": database_user,
        "cleanup_verified": False,
        "phases": [],
    }
    if re.fullmatch(r"bioseq-q1024-[0-9a-f]{32}", application_name) is None:
        evidence["error"] = "PostgreSQL backend owner is malformed"
        return evidence
    for field_name, value in (
            ("database", database_name), ("user", database_user)):
        if (not isinstance(value, str) or not value
                or len(value.encode("utf-8")) > 63
                or any(ord(character) < 0x20 or ord(character) == 0x7f
                       for character in value)):
            evidence["error"] = f"PostgreSQL backend {field_name} is malformed"
            return evidence

    def remaining_timeout(maximum: float) -> float:
        remaining = deadline - time.time()
        if remaining <= 0.0:
            raise TimeoutError(
                "whole-run deadline reached during PostgreSQL backend cleanup")
        return min(maximum, remaining)

    try:
        pg_config = _pg_config_from_cache(active_build_dir)
        bindir_result = subprocess.run(
            [pg_config, "--bindir"],
            capture_output=True,
            text=True,
            timeout=remaining_timeout(10.0),
        )
        if bindir_result.returncode != 0:
            raise RuntimeError(
                "pg_config --bindir failed during backend cleanup: "
                + (bindir_result.stderr or bindir_result.stdout).strip())
        pg_bindir = Path(bindir_result.stdout.strip()).resolve(strict=True)
        psql = (pg_bindir / "psql").resolve(strict=True)
        if (psql.parent != pg_bindir or not psql.is_file()
                or not os.access(psql, os.X_OK)):
            raise RuntimeError(
                "configured PostgreSQL cleanup psql is not a regular bindir file")
        if sys.platform.startswith("linux") and _is_aws_preset(active_preset):
            psql = _trusted_root_owned_executable(str(psql))
    except (OSError, RuntimeError, subprocess.SubprocessError,
            TimeoutError) as exc:
        evidence["error"] = str(exc)
        return evidence

    selector = (
        "backend_type = 'client backend' "
        "AND pid <> pg_backend_pid() "
        "AND application_name = :'owner_application' "
        "AND datname = :'owner_database' "
        "AND usename = :'owner_user'"
    )
    statements = (
        (
            "cancel",
            "SELECT COALESCE(bool_and(pg_cancel_backend(pid)), TRUE) "
            f"FROM pg_stat_activity WHERE {selector};",
        ),
        (
            "terminate",
            "SELECT COALESCE(bool_and(pg_terminate_backend(pid, 2000)), TRUE) "
            f"FROM pg_stat_activity WHERE {selector};",
        ),
        (
            "verify",
            f"SELECT count(*) FROM pg_stat_activity WHERE {selector};",
        ),
    )
    environment = _postgresql_admin_environment()

    def run_owner_statement(
            phase: str, statement: str,
            *, maximum_seconds: float = 10.0) -> dict[str, object]:
        phase_evidence: dict[str, object] = {"phase": phase}
        try:
            argv = _postgresql_admin_psql_argv(
                str(psql), active_preset,
                "-X", "-Atq", "-v", "ON_ERROR_STOP=1",
                "--dbname", "postgres",
                "--set", f"owner_application={application_name}",
                "--set", f"owner_database={database_name}",
                "--set", f"owner_user={database_user}",
                "--file", "-",
            )
            result = subprocess.run(
                argv,
                input=statement,
                capture_output=True,
                text=True,
                timeout=remaining_timeout(maximum_seconds),
                env=environment,
            )
            phase_evidence["returncode"] = result.returncode
            phase_evidence["stdout"] = result.stdout.strip()[:2048]
            phase_evidence["stderr"] = result.stderr.strip()[:2048]
        except (OSError, subprocess.SubprocessError, TimeoutError) as exc:
            phase_evidence["error"] = str(exc)
        return phase_evidence

    snapshot_statement = (
        "SELECT pid::text || E'\\t' || backend_start::text || E'\\t' || "
        "pg_postmaster_start_time()::text FROM pg_stat_activity WHERE "
        f"{selector} ORDER BY pid;"
    )
    sealed_backends: dict[tuple[int, str], _ProcessIdentity] = {}
    sealed_postmaster: _ProcessIdentity | None = None
    sealed_postmaster_start: str | None = None
    data_dir: str | None = None
    if allow_sigkill_backend:
        snapshot = run_owner_statement("snapshot", snapshot_statement)
        evidence["snapshot"] = snapshot
        try:
            if snapshot.get("returncode") != 0:
                raise RuntimeError("could not snapshot exact owned backends")
            rows = [
                line.split("\t")
                for line in str(snapshot.get("stdout", "")).splitlines()
                if line
            ]
            if any(len(row) != 3 or not row[0].isdigit() for row in rows):
                raise RuntimeError("owned backend snapshot is malformed")
            postmaster_starts = {row[2] for row in rows}
            if len(postmaster_starts) > 1:
                raise RuntimeError(
                    "owned backends report multiple postmaster starts")
            data_dir = _query_postgresql_data_directory(
                str(psql), active_preset, deadline)
            postmaster_pid = _postgresql_postmaster_pid(
                data_dir, active_preset, deadline)
            sealed_postmaster = _capture_process_identity(
                postmaster_pid, deadline=deadline)
            if sealed_postmaster is None:
                raise RuntimeError("could not capture exact postmaster identity")
            if rows:
                sealed_postmaster_start = next(iter(postmaster_starts))
            for pid_text, backend_start, _ in rows:
                backend = _capture_process_identity(
                    int(pid_text), deadline=deadline)
                if backend is None or backend.ppid != postmaster_pid:
                    raise RuntimeError(
                        "owned backend is not a captured direct postmaster child")
                key = (backend.pid, backend_start)
                if key in sealed_backends:
                    raise RuntimeError("owned backend snapshot contains duplicates")
                sealed_backends[key] = backend
            evidence["postmaster_provenance"] = {
                "data_directory": data_dir,
                "sql_postmaster_start": sealed_postmaster_start,
                "process_identity": sealed_postmaster.__dict__,
            }
            evidence["sealed_backends"] = [
                {
                    "pid": pid,
                    "backend_start": backend_start,
                    "process_identity": identity.__dict__,
                }
                for (pid, backend_start), identity in sorted(
                    sealed_backends.items())
            ]
        except (OSError, RuntimeError, subprocess.SubprocessError,
                TimeoutError) as exc:
            evidence["last_resort_disabled_reason"] = str(exc)
            sealed_backends.clear()
            sealed_postmaster = None

    phase_results: list[dict[str, object]] = []
    for phase, statement in statements:
        phase_results.append(run_owner_statement(phase, statement))
    evidence["phases"] = phase_results
    verify = phase_results[-1]
    evidence["matching_backends"] = (
        int(verify["stdout"])
        if (verify.get("returncode") == 0
            and isinstance(verify.get("stdout"), str)
            and re.fullmatch(r"[0-9]+", verify["stdout"]) is not None)
        else None
    )
    evidence["cleanup_verified"] = evidence["matching_backends"] == 0

    if (not evidence["cleanup_verified"]
            and allow_sigkill_backend
            and sealed_backends
            and sealed_postmaster is not None
            and data_dir is not None):
        last_resort: dict[str, object] = {
            "attempted": True,
            "signals": [],
            "recovery_consequence": (
                "SIGKILL of a postmaster child can make this already-owned "
                "local benchmark cluster terminate sibling sessions and "
                "perform crash recovery"),
        }
        evidence["last_resort"] = last_resort
        signal_deadline = deadline - 15.0
        try:
            remaining = run_owner_statement(
                "last_resort_recheck", snapshot_statement)
            last_resort["recheck"] = remaining
            if remaining.get("returncode") != 0:
                raise RuntimeError("exact backend SQL recheck failed")
            remaining_rows = [
                line.split("\t")
                for line in str(remaining.get("stdout", "")).splitlines()
                if line
            ]
            if any(len(row) != 3 or not row[0].isdigit()
                   for row in remaining_rows):
                raise RuntimeError("exact backend SQL recheck is malformed")
            remaining_keys = {
                (int(row[0]), row[1]): row[2] for row in remaining_rows}
            if not set(remaining_keys).issubset(sealed_backends):
                raise RuntimeError(
                    "backend identity appeared after the sealed snapshot")
            if any(value != sealed_postmaster_start
                   for value in remaining_keys.values()):
                raise RuntimeError("SQL postmaster provenance changed")

            for key in sorted(remaining_keys):
                if time.time() >= signal_deadline:
                    raise TimeoutError(
                        "cleanup reserve is too short for backend SIGKILL")
                stable_postmaster_pid = _postgresql_postmaster_pid(
                    data_dir, active_preset, deadline)
                observed_postmaster = _capture_process_identity(
                    stable_postmaster_pid, deadline=deadline)
                observed_backend = _capture_process_identity(
                    key[0], deadline=deadline)
                expected_backend = sealed_backends[key]
                if (stable_postmaster_pid != sealed_postmaster.pid
                        or not _same_process(
                            observed_postmaster, sealed_postmaster)
                        or not _same_process(
                            observed_backend, expected_backend)
                        or observed_backend is None
                        or observed_backend.ppid != sealed_postmaster.pid):
                    raise RuntimeError(
                        "exact backend/postmaster OS identity changed before signal")

                signal_evidence: dict[str, object] = {
                    "pid": key[0],
                    "backend_start": key[1],
                    "process_identity": expected_backend.__dict__,
                }
                if (sys.platform.startswith("linux")
                        and _is_aws_preset(active_preset)):
                    trusted_sudo = _trusted_root_owned_executable(
                        "/usr/bin/sudo")
                    trusted_kill = _trusted_root_owned_executable("/bin/kill")
                    killed = subprocess.run(
                        [str(trusted_sudo), "-n", "-u", "postgres", "--",
                         str(trusted_kill), "-s", "KILL", str(key[0])],
                        capture_output=True,
                        text=True,
                        timeout=remaining_timeout(2.0),
                        env=environment,
                    )
                    signal_evidence["returncode"] = killed.returncode
                    signal_evidence["stderr"] = killed.stderr.strip()[:2048]
                    if killed.returncode != 0:
                        raise RuntimeError(
                            "trusted exact backend SIGKILL failed")
                else:
                    os.kill(key[0], signal.SIGKILL)
                    signal_evidence["returncode"] = 0
                last_resort["signals"].append(signal_evidence)

            while time.time() < deadline:
                post_kill = run_owner_statement(
                    "last_resort_verify", statements[-1][1],
                    maximum_seconds=5.0)
                last_resort["verify"] = post_kill
                output = post_kill.get("stdout")
                if (post_kill.get("returncode") == 0
                        and isinstance(output, str)
                        and output == "0"):
                    stable_postmaster_pid = _postgresql_postmaster_pid(
                        data_dir, active_preset, deadline)
                    observed_postmaster = _capture_process_identity(
                        stable_postmaster_pid, deadline=deadline)
                    if (stable_postmaster_pid != sealed_postmaster.pid
                            or not _same_process(
                                observed_postmaster, sealed_postmaster)):
                        raise RuntimeError(
                            "postmaster identity changed during crash recovery")
                    evidence["matching_backends"] = 0
                    evidence["cleanup_verified"] = True
                    last_resort["recovery_ready"] = True
                    break
                time.sleep(min(0.25, max(0.0, deadline - time.time())))
        except (OSError, RuntimeError, subprocess.SubprocessError,
                TimeoutError) as exc:
            last_resort["error"] = str(exc)

    evidence["completed_at"] = time.time()
    log_handle.write(
        "# q1024 PostgreSQL backend cleanup="
        + json.dumps(evidence, ensure_ascii=True, sort_keys=True)
        + "\n")
    log_handle.flush()
    return evidence

def _shared_preload_contains_bioseq(setting: str) -> bool:
    for entry in setting.split(","):
        normalized = entry.strip()
        if len(normalized) >= 2 and normalized[0] == normalized[-1] == '"':
            normalized = normalized[1:-1]
        if normalized in {"bioseq", "$libdir/bioseq"}:
            return True
    return False

def _ensure_postgresql_shared_preload(
        psql: str, active_preset: str, log_handle,
        deadline: float,
        hard_kill_deadline: float | None = None,
        state_mode: str | None = None,
        phase: str | None = None) -> None:
    """Persistently add bioseq to shared_preload_libraries before restart."""
    environment = _postgresql_admin_environment()
    show_argv = _postgresql_admin_psql_argv(
        psql, active_preset,
        "-X", "-Atq", "--dbname", "postgres",
        "--command", "SHOW shared_preload_libraries;",
    )
    try:
        shown = subprocess.run(
            show_argv,
            capture_output=True,
            text=True,
            timeout=_remaining_subprocess_timeout(deadline, 10.0),
            env=environment,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError(
            "could not inspect PostgreSQL shared_preload_libraries: "
            f"{exc}") from exc
    if shown.returncode != 0:
        detail = (shown.stderr or shown.stdout).strip()
        raise RuntimeError(
            "could not inspect PostgreSQL shared_preload_libraries: "
            f"{detail or f'exit {shown.returncode}'}")

    current = shown.stdout.strip()
    if _shared_preload_contains_bioseq(current):
        return
    desired = "bioseq" if not current else f"{current},bioseq"
    if "\x00" in desired or "\n" in desired or "\r" in desired:
        raise RuntimeError(
            "shared_preload_libraries contains an invalid line break")
    sql_literal = desired.replace("'", "''")
    alter_argv = _postgresql_admin_psql_argv(
        psql, active_preset,
        "-X", "-v", "ON_ERROR_STOP=1", "--dbname", "postgres",
        "--command",
        "ALTER SYSTEM SET shared_preload_libraries = "
        f"'{sql_literal}';",
    )
    log_handle.write(
        "\n# Enabling bioseq in PostgreSQL shared_preload_libraries ...\n")
    log_handle.flush()
    rc = _stream_subprocess(
        alter_argv,
        log_handle,
        deadline,
        env=environment,
        hard_kill_deadline=hard_kill_deadline,
        state_mode=state_mode,
        phase=phase,
    )
    if rc != 0:
        raise RuntimeError(
            "PostgreSQL shared_preload_libraries setup exited with code "
            f"{rc}")

def _verify_postgresql_shared_preload(
        psql: str, active_preset: str,
        deadline: float | None = None) -> None:
    """Fail closed when a higher-priority setting defeated ALTER SYSTEM."""
    environment = _postgresql_admin_environment()
    show_argv = _postgresql_admin_psql_argv(
        psql, active_preset,
        "-X", "-Atq", "--dbname", "postgres",
        "--command", "SHOW shared_preload_libraries;",
    )
    try:
        shown = subprocess.run(
            show_argv,
            capture_output=True,
            text=True,
            timeout=_remaining_subprocess_timeout(deadline, 10.0),
            env=environment,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError(
            "could not verify PostgreSQL shared_preload_libraries after "
            f"restart: {exc}") from exc
    if shown.returncode != 0:
        detail = (shown.stderr or shown.stdout).strip()
        raise RuntimeError(
            "could not verify PostgreSQL shared_preload_libraries after "
            f"restart: {detail or f'exit {shown.returncode}'}")
    if not _shared_preload_contains_bioseq(shown.stdout.strip()):
        raise RuntimeError(
            "PostgreSQL restarted without bioseq in "
            "shared_preload_libraries")

def _max_prepared_transactions_value(setting: str) -> int:
    try:
        value = int(setting.strip(), 10)
    except ValueError as exc:
        raise RuntimeError(
            "PostgreSQL returned an invalid max_prepared_transactions "
            f"value: {setting!r}") from exc
    if value < 0:
        raise RuntimeError(
            "PostgreSQL returned a negative max_prepared_transactions "
            f"value: {value}")
    return value

def _ensure_postgresql_max_prepared_transactions(
        psql: str, active_preset: str, log_handle,
        deadline: float,
        hard_kill_deadline: float | None = None,
        state_mode: str | None = None,
        phase: str | None = None) -> None:
    """Persist the project minimum before restarting the shared cluster."""
    environment = _postgresql_admin_environment()
    show_argv = _postgresql_admin_psql_argv(
        psql, active_preset,
        "-X", "-Atq", "--dbname", "postgres",
        "--command", "SHOW max_prepared_transactions;",
    )
    try:
        shown = subprocess.run(
            show_argv,
            capture_output=True,
            text=True,
            timeout=_remaining_subprocess_timeout(deadline, 10.0),
            env=environment,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError(
            "could not inspect PostgreSQL max_prepared_transactions: "
            f"{exc}") from exc
    if shown.returncode != 0:
        detail = (shown.stderr or shown.stdout).strip()
        raise RuntimeError(
            "could not inspect PostgreSQL max_prepared_transactions: "
            f"{detail or f'exit {shown.returncode}'}")

    current = _max_prepared_transactions_value(shown.stdout)
    if current >= _REQUIRED_MAX_PREPARED_TRANSACTIONS:
        return

    alter_argv = _postgresql_admin_psql_argv(
        psql, active_preset,
        "-X", "-v", "ON_ERROR_STOP=1", "--dbname", "postgres",
        "--command",
        "ALTER SYSTEM SET max_prepared_transactions = "
        f"'{_REQUIRED_MAX_PREPARED_TRANSACTIONS}';",
    )
    log_handle.write(
        "\n# Enabling PostgreSQL prepared transactions for bioseq tests ...\n")
    log_handle.flush()
    rc = _stream_subprocess(
        alter_argv,
        log_handle,
        deadline,
        env=environment,
        hard_kill_deadline=hard_kill_deadline,
        state_mode=state_mode,
        phase=phase,
    )
    if rc != 0:
        raise RuntimeError(
            "PostgreSQL max_prepared_transactions setup exited with code "
            f"{rc}")

def _verify_postgresql_max_prepared_transactions(
        psql: str, active_preset: str,
        deadline: float | None = None) -> None:
    """Fail closed if restart did not activate the project minimum."""
    environment = _postgresql_admin_environment()
    show_argv = _postgresql_admin_psql_argv(
        psql, active_preset,
        "-X", "-Atq", "--dbname", "postgres",
        "--command", "SHOW max_prepared_transactions;",
    )
    try:
        shown = subprocess.run(
            show_argv,
            capture_output=True,
            text=True,
            timeout=_remaining_subprocess_timeout(deadline, 10.0),
            env=environment,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError(
            "could not verify PostgreSQL max_prepared_transactions after "
            f"restart: {exc}") from exc
    if shown.returncode != 0:
        detail = (shown.stderr or shown.stdout).strip()
        raise RuntimeError(
            "could not verify PostgreSQL max_prepared_transactions after "
            f"restart: {detail or f'exit {shown.returncode}'}")
    actual = _max_prepared_transactions_value(shown.stdout)
    if actual < _REQUIRED_MAX_PREPARED_TRANSACTIONS:
        raise RuntimeError(
            "PostgreSQL restarted with max_prepared_transactions="
            f"{actual}; bioseq tests require at least "
            f"{_REQUIRED_MAX_PREPARED_TRANSACTIONS}")

def _is_homebrew_arm64_pg_ctl(pg_ctl: str) -> bool:
    """Whether pg_ctl is from the native /opt/homebrew arm64 installation."""
    try:
        Path(pg_ctl).resolve(strict=False).relative_to(Path("/opt/homebrew"))
        return True
    except (OSError, RuntimeError, ValueError):
        return False

def _darwin_postgresql_restart_argv(
        pg_ctl: str, postgres: str, data_dir: str) -> list[str]:
    """Keep Postgres.app native via arch, but never scrub Homebrew dyld env."""
    command = [pg_ctl, "-D", data_dir, "restart", "-p", postgres]
    if _is_homebrew_arm64_pg_ctl(pg_ctl):
        return command
    return ["arch", "-arm64", *command]


def _postgresql_restart_argv(
        pg_ctl: str,
        postgres: str,
        data_dir: str,
        active_preset: str,
        *,
        fast_wait: bool = False) -> list[str]:
    """Construct a platform restart while pinning every pg_ctl route."""
    trailing = ["-m", "fast", "-w"] if fast_wait else []
    if sys.platform == "darwin":
        return [
            *_darwin_postgresql_restart_argv(
                pg_ctl, postgres, data_dir),
            *trailing,
        ]
    if _is_aws_preset(active_preset):
        systemctl = shutil.which("systemctl")
        if systemctl:
            # systemd has no pg_ctl -p equivalent. Runtime attestation below
            # proves its service launched the selected executable before SQL.
            return ["sudo", "-n", systemctl, "restart", "postgresql"]
        return [
            "sudo", "-n", "-u", "postgres",
            pg_ctl, "-D", data_dir, "restart", "-p", postgres,
            *trailing,
        ]
    return [
        pg_ctl, "-D", data_dir, "restart", "-p", postgres, *trailing]

def _postgresql_restart_environment(
        base_env: dict[str, str] | None = None,
        *, allowed_paths=None) -> dict[str, str]:
    """Strip ambient dyld injection and map one validated runtime at PG only."""
    try:
        validation = {} if allowed_paths is None else {
            "allowed_paths": allowed_paths}
        clean = sanitizer_descendant_environment(
            os.environ if base_env is None else base_env, **validation)
        if sys.platform == "darwin":
            return postgres_sanitizer_environment(clean, **validation)
        return clean
    except SanitizerRuntimeError as exc:
        raise RuntimeError(str(exc)) from exc

def _query_postgresql_data_directory(
        psql: str, active_preset: str,
        deadline: float | None = None) -> str:
    """Query directly, with a narrowly validated AWS postgres-uid fallback."""
    argv = [psql, "-t", "-c", "SHOW data_directory;", "postgres"]
    environment = _postgresql_admin_environment()
    direct_detail = ""
    try:
        direct = subprocess.run(
            argv, capture_output=True, text=True,
            timeout=_remaining_subprocess_timeout(deadline, 10.0),
            env=environment)
        if direct.returncode == 0:
            return direct.stdout.strip()
        direct_detail = (direct.stderr or direct.stdout).strip()
    except (OSError, subprocess.TimeoutExpired) as exc:
        direct_detail = str(exc)

    if not _is_aws_preset(active_preset):
        raise RuntimeError(
            "direct psql SHOW data_directory failed: "
            f"{direct_detail or 'unknown error'}")

    trusted_psql = _trusted_root_owned_executable(psql)
    trusted_sudo = _trusted_root_owned_executable("/usr/bin/sudo")
    try:
        fallback = subprocess.run(
            [str(trusted_sudo), "-n", "-u", "postgres", "--",
             str(trusted_psql), "-t", "-c", "SHOW data_directory;",
             "postgres"],
            capture_output=True,
            text=True,
            timeout=_remaining_subprocess_timeout(deadline, 10.0),
            env=environment,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError(
            "direct psql failed and trusted AWS postgres-uid fallback could "
            f"not launch (direct: {direct_detail or 'unknown error'}; "
            f"fallback: {exc})") from exc
    if fallback.returncode != 0:
        fallback_detail = (fallback.stderr or fallback.stdout).strip()
        raise RuntimeError(
            "direct psql and trusted AWS postgres-uid fallback failed "
            f"(direct: {direct_detail or 'unknown error'}; "
            f"fallback: {fallback_detail or f'exit {fallback.returncode}'})")
    return fallback.stdout.strip()


def _postgresql_config_path(
        pg_config: str, option: str,
        deadline: float | None = None) -> str:
    """Resolve one absolute runtime path from the selected pg_config."""
    try:
        result = subprocess.run(
            [pg_config, option],
            capture_output=True,
            text=True,
            timeout=_remaining_subprocess_timeout(deadline, 10.0),
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError(f"pg_config {option} failed: {exc}") from exc
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        raise RuntimeError(
            f"pg_config {option} exited {result.returncode}: "
            f"{detail or 'no diagnostic'}")
    value = result.stdout.strip()
    if not value or not Path(value).is_absolute():
        raise RuntimeError(
            f"pg_config {option} returned a non-absolute path: {value!r}")
    return value


def _postgresql_bioseq_dso_path(pg_pkglibdir: str) -> str:
    if sys.platform == "darwin":
        suffix = ".dylib"
    elif sys.platform.startswith("win"):
        suffix = ".dll"
    else:
        suffix = ".so"
    return str(Path(pg_pkglibdir) / f"bioseq{suffix}")

def _restart_postgresql(
        active_build_dir: Path,
        active_preset: str,
        log_handle,
        deadline: float,
        *,
        hard_kill_deadline: float | None = None,
        state_mode: str | None = None,
        phase: str | None = None) -> None:
    """Restart the PostgreSQL postmaster after installing a new extension dylib.

    On macOS, fork()d backend processes inherit the postmaster's dyld image
    list.  When the postmaster has an older bioseq.dylib already mapped,
    dlopen("bioseq.dylib") in every fork'd child returns the old cached handle
    — dlsym() then fails to find functions added in the new build.  A
    postmaster restart evicts the stale mapping; the first CREATE EXTENSION
    after restart loads the freshly installed dylib.
    """
    cmake_cache = active_build_dir / "CMakeCache.txt"
    pg_config: str | None = None
    if cmake_cache.exists():
        for line in cmake_cache.read_text(errors="replace").splitlines():
            if line.startswith("PostgreSQL_CONFIG:FILEPATH="):
                pg_config = line.split("=", 1)[1].strip()
                break
    if not pg_config:
        msg = "PostgreSQL_CONFIG not in CMakeCache.txt; cannot restart PG after installing extension"
        _log(f"ERROR: {msg}")
        raise RuntimeError(msg)

    try:
        pg_bindir = _postgresql_config_path(
            pg_config, "--bindir", deadline)
        pg_pkglibdir = _postgresql_config_path(
            pg_config, "--pkglibdir", deadline)
    except RuntimeError as exc:
        msg = (
            f"{exc}; cannot restart PG after installing extension")
        _log(f"ERROR: {msg}")
        raise RuntimeError(msg) from exc

    pg_ctl = os.path.join(pg_bindir, "pg_ctl")
    postgres = os.path.join(pg_bindir, "postgres")
    psql   = os.path.join(pg_bindir, "psql")
    bioseq_dso = _postgresql_bioseq_dso_path(pg_pkglibdir)
    if not os.path.isfile(pg_ctl):
        msg = f"pg_ctl not found at {pg_ctl}; cannot restart PG after installing extension"
        _log(f"ERROR: {msg}")
        raise RuntimeError(msg)
    if not os.path.isfile(psql):
        msg = f"psql not found at {psql}; cannot locate PG data_directory for restart"
        _log(f"ERROR: {msg}")
        raise RuntimeError(msg)
    if not os.path.isfile(postgres):
        msg = (
            f"postgres not found at {postgres}; cannot pin the selected "
            "server executable for restart")
        _log(f"ERROR: {msg}")
        raise RuntimeError(msg)
    if not os.path.isfile(bioseq_dso):
        msg = (
            f"installed bioseq DSO not found at {bioseq_dso}; refusing "
            "PostgreSQL restart")
        _log(f"ERROR: {msg}")
        raise RuntimeError(msg)

    try:
        data_dir = _query_postgresql_data_directory(
            psql, active_preset, deadline)
    except TimeoutError:
        raise
    except Exception as exc:
        msg = f"could not query data_directory ({exc}); cannot restart PG after installing extension"
        _log(f"ERROR: {msg}")
        raise RuntimeError(msg) from exc

    if not data_dir or not os.path.isdir(data_dir):
        msg = f"data_directory '{data_dir}' is not a directory; cannot restart PG after installing extension"
        _log(f"ERROR: {msg}")
        raise RuntimeError(msg)

    _ensure_postgresql_shared_preload(
        psql,
        active_preset,
        log_handle,
        deadline,
        hard_kill_deadline=hard_kill_deadline,
        state_mode=state_mode,
        phase=phase,
    )
    _ensure_postgresql_max_prepared_transactions(
        psql,
        active_preset,
        log_handle,
        deadline,
        hard_kill_deadline=hard_kill_deadline,
        state_mode=state_mode,
        phase=phase,
    )

    restart_env = _postgresql_restart_environment()
    sanitizer_runtime = restart_env.get(SANITIZER_RUNTIME_ENV)

    # Postgres.app's universal pg_ctl still needs the native-architecture
    # wrapper.  Homebrew pg_ctl is already thin arm64 and must run directly:
    # /usr/bin/arch is a platform binary, so macOS strips DYLD insertion before
    # the replacement postmaster can inherit the sanitizer runtime.
    if (
        sys.platform == "darwin"
        and sanitizer_runtime is not None
        and not _is_homebrew_arm64_pg_ctl(pg_ctl)
    ):
        raise RuntimeError(
            f"{SANITIZER_RUNTIME_ENV} requires native Homebrew pg_ctl; "
            f"refusing platform-wrapped restart through {pg_ctl}")
    restart_cmd = _postgresql_restart_argv(
        pg_ctl, postgres, data_dir, active_preset)

    _log(f"Restarting PostgreSQL (data_dir={data_dir}) to evict stale dylib cache ...")
    # DON'T use _stream_subprocess here: that function blocks until the pipe
    # reaches EOF.  pg_ctl restart forks the new postmaster, which inherits
    # pg_ctl's stdout pipe and keeps it open indefinitely, causing a hang.
    # Writing stdout directly to log_handle (a file) avoids the pipe: Python
    # then calls proc.wait() which returns as soon as pg_ctl itself exits
    # (after the postmaster signals readiness), regardless of what the
    # postmaster does with the file handle afterwards.
    _run_postgresql_restart_cmd(
        restart_cmd,
        log_handle,
        env=restart_env,
        data_dir=data_dir,
        active_preset=active_preset,
        expected_postgres=postgres,
        expected_bioseq_dso=bioseq_dso,
        deadline=deadline,
    )
    _verify_postgresql_shared_preload(psql, active_preset, deadline)
    _verify_postgresql_max_prepared_transactions(
        psql, active_preset, deadline)

def _pg_config_from_cache(active_build_dir: Path) -> str:
    cmake_cache = active_build_dir / "CMakeCache.txt"
    if cmake_cache.exists():
        for line in cmake_cache.read_text(errors="replace").splitlines():
            if line.startswith("PostgreSQL_CONFIG:FILEPATH="):
                return line.split("=", 1)[1].strip()
    raise RuntimeError("PostgreSQL_CONFIG not in CMakeCache.txt")


def _postgresql_runtime_identity_verifier_argv(
        data_dir: str,
        active_preset: str,
        expected_postgres: _PostgreSQLFileIdentity,
        expected_bioseq_dso: _PostgreSQLFileIdentity) -> list[str]:
    """Build the internal pre-SQL verifier used after every E2E restart."""
    return [
        str(Path(sys.executable).resolve(strict=True)),
        str(Path(__file__).resolve(strict=True)),
        "verify-postgresql-runtime-identity",
        "--data-dir", data_dir,
        "--active-preset", active_preset,
        "--postgres-path", expected_postgres.path,
        "--postgres-device", str(expected_postgres.device),
        "--postgres-inode", str(expected_postgres.inode),
        "--postgres-size", str(expected_postgres.size),
        "--postgres-sha256", expected_postgres.sha256,
        "--bioseq-dso-path", expected_bioseq_dso.path,
        "--bioseq-dso-device", str(expected_bioseq_dso.device),
        "--bioseq-dso-inode", str(expected_bioseq_dso.inode),
        "--bioseq-dso-size", str(expected_bioseq_dso.size),
        "--bioseq-dso-sha256", expected_bioseq_dso.sha256,
    ]

def _postgresql_e2e_restart_shell_command(
        active_build_dir: Path,
        active_preset: str,
        *,
        boundary_mode: str | None = None,
        boundary_run_token: str | None = None,
        boundary_application_name: str | None = None,
) -> str:
    """Build the shell command passed to bioseq_benchmark_e2e.

    The benchmark executes BIOSEQ_E2E_PG_RESTART_CMD with std::system() before
    every just_index timed sample. Redirect restart and attestation output to
    a file: the benchmark itself is run under the MCP worker's stdout/stderr
    pipe, and a restarted postmaster can otherwise inherit that pipe and keep
    it open.
    """
    pg_config = _pg_config_from_cache(active_build_dir)
    pg_bindir = _postgresql_config_path(pg_config, "--bindir")
    pg_pkglibdir = _postgresql_config_path(pg_config, "--pkglibdir")
    pg_ctl = os.path.join(pg_bindir, "pg_ctl")
    postgres = os.path.join(pg_bindir, "postgres")
    psql = os.path.join(pg_bindir, "psql")
    bioseq_dso = _postgresql_bioseq_dso_path(pg_pkglibdir)
    if not os.path.isfile(pg_ctl):
        raise RuntimeError(f"pg_ctl not found at {pg_ctl}")
    if not os.path.isfile(psql):
        raise RuntimeError(f"psql not found at {psql}")
    if not os.path.isfile(postgres):
        raise RuntimeError(
            f"postgres not found at {postgres}; cannot pin E2E restart")
    if not os.path.isfile(bioseq_dso):
        raise RuntimeError(
            f"installed bioseq DSO not found at {bioseq_dso}; refusing "
            "E2E restart")

    data_dir = _query_postgresql_data_directory(psql, active_preset)
    if not data_dir or not os.path.isdir(data_dir):
        raise RuntimeError(f"data_directory '{data_dir}' is not a directory")

    restart_argv = _postgresql_restart_argv(
        pg_ctl,
        postgres,
        data_dir,
        active_preset,
        fast_wait=True,
    )
    postgres_identity = _seal_postgresql_file_identity(
        postgres, "selected PostgreSQL server executable")
    bioseq_identity = _seal_postgresql_file_identity(
        bioseq_dso, "installed bioseq DSO")
    verifier_argv = _postgresql_runtime_identity_verifier_argv(
        data_dir,
        active_preset,
        postgres_identity,
        bioseq_identity,
    )
    boundary_values = (
        boundary_mode,
        boundary_run_token,
        boundary_application_name,
    )
    if any(value is not None for value in boundary_values):
        if (
            boundary_mode != "benchmark-e2e"
            or not isinstance(boundary_run_token, str)
            or re.fullmatch(r"[0-9a-f]{32}", boundary_run_token) is None
            or boundary_application_name
                != f"bioseq-q1024-{boundary_run_token}"
        ):
            raise RuntimeError(
                "E2E restart service boundary is malformed")
        verifier_argv.extend([
            "--boundary-mode", boundary_mode,
            "--boundary-run-token", boundary_run_token,
            "--boundary-application-name", boundary_application_name,
        ])

    return (
        "( " + shlex.join(restart_argv)
        + " && " + shlex.join(verifier_argv)
        + " ) > /tmp/bioseq_e2e_pg_restart.log 2>&1"
    )


def _await_q1024_client_boundary_arm(
        *,
        mode: str,
        run_token: str,
        application_name: str,
        guard_marker: str,
        timeout_seconds: float = _Q1024_SERVICE_BOUNDARY_ARM_TIMEOUT_SECONDS,
        clock=time.monotonic,
        wall_clock=time.time,
        sleeper=time.sleep,
) -> _ProcessIdentity:
    """Wait boundedly for this verifier's exact client ancestry to be armed."""
    if (
        mode != "benchmark-e2e"
        or re.fullmatch(r"[0-9a-f]{32}", run_token) is None
        or application_name != f"bioseq-q1024-{run_token}"
        or re.fullmatch(r"[0-9a-f]{32}", guard_marker) is None
        or timeout_seconds <= 0.0
    ):
        raise RuntimeError("q1024 client boundary arm request is malformed")
    deadline = clock() + min(
        timeout_seconds, _Q1024_SERVICE_BOUNDARY_ARM_TIMEOUT_SECONDS)
    while True:
        entry = _load_state().get(mode)
        if (
            not isinstance(entry, dict)
            or entry.get("mode") != mode
            or entry.get("status") != "running"
            or entry.get("q1024_run_token") != run_token
            or entry.get("postgres_application_name") != application_name
            or entry.get("postgres_backend_owner_started") is not True
        ):
            raise RuntimeError(
                "q1024 client boundary state/token ownership is stale")
        marker_value = entry.get("active_guard_marker")
        active_value = entry.get("active_process_identity")
        client_identity: _ProcessIdentity | None = None
        if marker_value == guard_marker and active_value is not None:
            try:
                client_identity = (
                    _ProcessIdentity(**active_value)
                    if isinstance(active_value, dict) else None)
            except (TypeError, ValueError) as exc:
                raise RuntimeError(
                    "q1024 client boundary identity is malformed") from exc
            if client_identity is None:
                raise RuntimeError(
                    "q1024 client boundary identity is malformed")
        now = clock()
        if marker_value == guard_marker and client_identity is not None:
            remaining = deadline - now
            if remaining <= 0.0:
                raise RuntimeError(
                    "q1024 client boundary arm deadline was reached")
            snapshot = _process_snapshot(
                wall_clock() + remaining, require_complete=True)
            observed_client = snapshot.get(client_identity.pid)
            verifier = snapshot.get(os.getpid())
            descendants = _process_descendant_closure(
                snapshot, client_identity, require_context=True)
            if (
                _same_process_context(observed_client, client_identity)
                and verifier is not None
                and verifier.pid != client_identity.pid
                and verifier.pid in descendants
            ):
                return client_identity
        now = clock()
        if now >= deadline:
            raise RuntimeError(
                "q1024 client boundary arm deadline was reached")
        sleeper(min(0.02, max(0.0, deadline - now)))


def _postgresql_runtime_identity_cli(arguments: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Verify the live PostgreSQL executable and mapped bioseq DSO "
            "before E2E SQL"))
    parser.add_argument(
        "command", choices=("verify-postgresql-runtime-identity",))
    parser.add_argument("--data-dir", required=True)
    parser.add_argument("--active-preset", required=True)
    parser.add_argument("--postgres-path", required=True)
    parser.add_argument("--postgres-device", required=True, type=int)
    parser.add_argument("--postgres-inode", required=True, type=int)
    parser.add_argument("--postgres-size", required=True, type=int)
    parser.add_argument("--postgres-sha256", required=True)
    parser.add_argument("--bioseq-dso-path", required=True)
    parser.add_argument("--bioseq-dso-device", required=True, type=int)
    parser.add_argument("--bioseq-dso-inode", required=True, type=int)
    parser.add_argument("--bioseq-dso-size", required=True, type=int)
    parser.add_argument("--bioseq-dso-sha256", required=True)
    parser.add_argument("--boundary-mode")
    parser.add_argument("--boundary-run-token")
    parser.add_argument("--boundary-application-name")
    parsed = parser.parse_args(arguments)

    for label, path, device, inode, size, digest in (
        (
            "postgres",
            parsed.postgres_path,
            parsed.postgres_device,
            parsed.postgres_inode,
            parsed.postgres_size,
            parsed.postgres_sha256,
        ),
        (
            "bioseq DSO",
            parsed.bioseq_dso_path,
            parsed.bioseq_dso_device,
            parsed.bioseq_dso_inode,
            parsed.bioseq_dso_size,
            parsed.bioseq_dso_sha256,
        ),
    ):
        if not Path(path).is_absolute():
            parser.error(f"{label} path must be absolute")
        if device < 0 or inode <= 0 or size < 0:
            parser.error(f"{label} identity metadata is invalid")
        if re.fullmatch(r"[0-9a-f]{64}", digest) is None:
            parser.error(f"{label} digest must be lowercase SHA-256")

    expected_postgres = _PostgreSQLFileIdentity(
        path=parsed.postgres_path,
        device=parsed.postgres_device,
        inode=parsed.postgres_inode,
        size=parsed.postgres_size,
        sha256=parsed.postgres_sha256,
    )
    expected_bioseq_dso = _PostgreSQLFileIdentity(
        path=parsed.bioseq_dso_path,
        device=parsed.bioseq_dso_device,
        inode=parsed.bioseq_dso_inode,
        size=parsed.bioseq_dso_size,
        sha256=parsed.bioseq_dso_sha256,
    )
    boundary_values = (
        parsed.boundary_mode,
        parsed.boundary_run_token,
        parsed.boundary_application_name,
    )
    boundary_requested = any(value is not None for value in boundary_values)
    client_identity: _ProcessIdentity | None = None
    guard_marker: str | None = None
    if boundary_requested:
        if (
            parsed.boundary_mode != "benchmark-e2e"
            or parsed.boundary_run_token is None
            or re.fullmatch(
                r"[0-9a-f]{32}", parsed.boundary_run_token) is None
            or parsed.boundary_application_name
                != f"bioseq-q1024-{parsed.boundary_run_token}"
        ):
            parser.error("service-boundary arguments are malformed")
        guard_marker = os.environ.get(BUILD_GUARD_ENV_KEY)
        if not isinstance(guard_marker, str):
            raise RuntimeError(
                "q1024 verifier did not inherit its guard marker")
        client_identity = _await_q1024_client_boundary_arm(
            mode=parsed.boundary_mode,
            run_token=parsed.boundary_run_token,
            application_name=parsed.boundary_application_name,
            guard_marker=guard_marker,
        )

    postmaster_identity = _verify_postgresql_runtime_identity(
        parsed.data_dir,
        parsed.active_preset,
        expected_postgres,
        expected_bioseq_dso,
        sys.stdout,
    )
    if boundary_requested:
        if client_identity is None or guard_marker is None:
            raise RuntimeError("q1024 client boundary arm was lost")
        boundary = _postgresql_service_boundary_evidence(
            mode=parsed.boundary_mode,
            run_token=parsed.boundary_run_token,
            application_name=parsed.boundary_application_name,
            client_identity=client_identity,
            guard_marker=guard_marker,
            data_dir=parsed.data_dir,
            postgres_identity=expected_postgres,
            bioseq_identity=expected_bioseq_dso,
            postmaster_identity=postmaster_identity,
        )
        if not _publish_q1024_postgresql_service_boundary(
            parsed.boundary_mode,
            parsed.boundary_run_token,
            client_identity,
            guard_marker,
            boundary,
        ):
            raise RuntimeError(
                "q1024 service-boundary publication was rejected")
    return 0


if __name__ == "__main__":
    raise SystemExit(_postgresql_runtime_identity_cli(sys.argv[1:]))
