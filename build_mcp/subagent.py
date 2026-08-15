"""Persistent, multi-account subagent orchestration for the build MCP.

The public surface is one command-discriminated MCP tool.  All MCP server
processes (including servers inherited by recursively-created agents) share a
repository-scoped durable registry and message bus protected by one reentrant
cross-process lock.  Agent processes live in tmux, so an MCP restart does not
discard their terminals or vendor conversation context.
"""

from __future__ import annotations

from collections.abc import Mapping
import contextlib
from datetime import datetime
import fcntl
import hashlib
import json
import os
import re
import secrets
import select
import shlex
import signal
import subprocess
import threading
import time
from pathlib import Path
from typing import Any

from build_presets import REPOSITORY_ROOT
from process_state import _ProcessIdentity, _capture_process_identity, _process_snapshot
from publication import durable_replace_bytes


SUBAGENT_SCHEMA_VERSION = 1
SUBAGENT_BUDGET = 1300
QUOTA_REFRESH_SECONDS = 5 * 60
QUOTA_PROBE_LEASE_SECONDS = 30
QUOTA_CREATE_FLOOR_PERCENT = 3.0
QUOTA_HISTORY_LIMIT = 512
BUS_RETENTION_MESSAGES = 5000
LOG_RESPONSE_MAX_CHARS = 60_000
TMUX_HISTORY_LINES = 100_000
OPERATION_LEASE_SECONDS = 120

_REPOSITORY_KEY = hashlib.sha256(
    str(REPOSITORY_ROOT.resolve()).encode("utf-8")
).hexdigest()[:16]
SUBAGENT_STATE_DIR = (
    Path.home() / ".cache" / "bioseq-build" / "subagents" / _REPOSITORY_KEY
)
SUBAGENT_STATE_FILE = SUBAGENT_STATE_DIR / "state.json"
SUBAGENT_BUS_FILE = SUBAGENT_STATE_DIR / "bus.json"
SUBAGENT_LOCK_FILE = SUBAGENT_STATE_DIR / "registry.lock"
SUBAGENT_LOG_DIR = SUBAGENT_STATE_DIR / "logs"
SUBAGENT_TMUX_SOCKET = SUBAGENT_STATE_DIR / "tmux.sock"

AGENT_ID_ENV = "BIOSEQ_SUBAGENT_ID"
AGENT_CAPABILITY_ENV = "BIOSEQ_SUBAGENT_CAPABILITY"

PUBLIC_STATES = frozenset({"ROOT", "WORKING", "INTERRUPTED", "KILLED"})
LIVE_STATES = frozenset({"WORKING", "INTERRUPTED"})
LABELS = frozenset({"general-purpose", "review", "audit", "implementation"})
COMMANDS = frozenset({
    "status", "create", "interrupt", "logs", "kill", "repurpose",
    "bus-read", "bus-write",
})
PENDING_OPERATION_STATES = frozenset({
    "STARTING", "INTERRUPTING", "KILLING", "REPURPOSING", "RESUMING",
})
OPERATION_PHASES = (
    "RESERVED",
    "SESSION_OWNED",
    "VENDOR_STARTING",
    "VENDOR_STARTED",
    "INSTRUCTION_SENDING",
    "INSTRUCTION_SENT",
    "ASSIGNMENT_ACKNOWLEDGED",
)
OPERATION_PHASE_RANK = {
    phase: index for index, phase in enumerate(OPERATION_PHASES)
}

TIER_ORDER = (
    "contractor-intern", "intern", "mid", "senior", "specialist",
)
TIER_RANK = {name: index for index, name in enumerate(TIER_ORDER)}
TIER_CONFIG: dict[str, dict[str, Any]] = {
    "contractor-intern": {
        "provider": "cursor",
        "model": "composer-2.5",
        "model_display": "Composer 2.5",
        "reasoning": None,
        "cost": 10,
    },
    "intern": {
        "provider": "codex",
        "model": "gpt-5.6-terra",
        "model_display": "gpt-5.6-terra",
        "reasoning": "medium",
        "cost": 22,
    },
    "mid": {
        "provider": "codex",
        "model": "gpt-5.6-terra",
        "model_display": "gpt-5.6-terra",
        "reasoning": "high",
        "cost": 50,
    },
    "senior": {
        "provider": "codex",
        "model": "gpt-5.6-sol",
        "model_display": "gpt-5.6-sol",
        "reasoning": "high",
        "cost": 70,
    },
    "specialist": {
        "provider": "codex",
        "model": "gpt-5.6-sol",
        "model_display": "gpt-5.6-sol",
        "reasoning": "max",
        "cost": 100,
    },
}

_ANSI_RE = re.compile(
    r"(?:\x1B\][^\x07]*(?:\x07|\x1B\\)|\x1B\[[0-?]*[ -/]*[@-~]|\x1B[@-_])"
)
_SESSION_RE = re.compile(
    r"\b(?:Session(?:\s+ID)?|Chat(?:\s+ID)?)\s*:\s*"
    r"([0-9a-fA-F]{8}-[0-9a-fA-F-]{27,})\b"
)
_LOCK_THREAD = threading.RLock()
_LOCK_CONTEXT = threading.local()


class SubagentError(RuntimeError):
    """Expected public error with a stable machine-readable code."""

    def __init__(self, code: str, message: str, *, details: Any = None):
        super().__init__(message)
        self.code = code
        self.details = details


def _ensure_private_directories() -> None:
    for directory in (SUBAGENT_STATE_DIR, SUBAGENT_LOG_DIR):
        directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        with contextlib.suppress(OSError):
            directory.chmod(0o700)


@contextlib.contextmanager
def _registry_lock():
    """Reentrant thread/process lock for registry and bus transactions."""
    with _LOCK_THREAD:
        depth = getattr(_LOCK_CONTEXT, "depth", 0)
        if depth:
            _LOCK_CONTEXT.depth = depth + 1
            try:
                yield
            finally:
                _LOCK_CONTEXT.depth = depth
            return

        _ensure_private_directories()
        flags = os.O_RDWR | os.O_CREAT
        flags |= getattr(os, "O_CLOEXEC", 0)
        flags |= getattr(os, "O_NOFOLLOW", 0)
        descriptor = os.open(SUBAGENT_LOCK_FILE, flags, 0o600)
        try:
            os.fchmod(descriptor, 0o600)
            fcntl.flock(descriptor, fcntl.LOCK_EX)
            _LOCK_CONTEXT.depth = 1
            yield
        finally:
            _LOCK_CONTEXT.depth = 0
            try:
                fcntl.flock(descriptor, fcntl.LOCK_UN)
            finally:
                os.close(descriptor)


def _new_state() -> dict[str, Any]:
    return {
        "schema_version": SUBAGENT_SCHEMA_VERSION,
        "revision": 0,
        "next_agent_id": 1,
        "root_identity": None,
        "agents": {},
        "accounts": {},
    }


def _new_bus() -> dict[str, Any]:
    return {
        "schema_version": SUBAGENT_SCHEMA_VERSION,
        "next_message_id": 1,
        "messages": [],
    }


def _read_json_locked(path: Path, factory) -> dict[str, Any]:
    if not path.exists():
        return factory()
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise SubagentError(
            "STATE_CORRUPT",
            f"persistent subagent data at {path.name} is unreadable; refusing to reset it",
            details=str(exc),
        ) from exc
    if not isinstance(value, dict) or value.get("schema_version") != SUBAGENT_SCHEMA_VERSION:
        raise SubagentError(
            "STATE_VERSION_UNSUPPORTED",
            f"persistent subagent data at {path.name} has an unsupported shape/version",
        )
    return value


def _load_state_locked() -> dict[str, Any]:
    state = _read_json_locked(SUBAGENT_STATE_FILE, _new_state)
    if (
        not isinstance(state.get("agents"), dict)
        or not isinstance(state.get("accounts"), dict)
        or not isinstance(state.get("next_agent_id"), int)
        or isinstance(state.get("next_agent_id"), bool)
        or state["next_agent_id"] < 1
    ):
        raise SubagentError("STATE_CORRUPT", "subagent registry invariants are invalid")
    numeric_ids: list[int] = []
    for raw_id, agent in state["agents"].items():
        if (
            not isinstance(raw_id, str)
            or not raw_id.isdigit()
            or raw_id != str(int(raw_id))
            or not isinstance(agent, dict)
            or isinstance(agent.get("id"), bool)
            or agent.get("id") != int(raw_id)
            or agent.get("status") not in PUBLIC_STATES
        ):
            raise SubagentError("STATE_CORRUPT", "subagent registry contains an invalid agent record")
        numeric_id = int(raw_id)
        if (
            (numeric_id == 0 and agent.get("status") != "ROOT")
            or (numeric_id != 0 and agent.get("status") == "ROOT")
            or (
                numeric_id != 0
                and (
                    agent.get("tier") not in TIER_CONFIG
                    or agent.get("highest_spawn_tier") not in TIER_CONFIG
                )
            )
        ):
            raise SubagentError("STATE_CORRUPT", "subagent registry lifecycle/tier data is invalid")
        if numeric_id != 0:
            config = TIER_CONFIG[str(agent["tier"])]
            if (
                agent.get("provider") != config["provider"]
                or agent.get("model") != config["model"]
                or agent.get("reasoning") != config["reasoning"]
                or isinstance(agent.get("cost"), bool)
                or agent.get("cost") != config["cost"]
            ):
                raise SubagentError(
                    "STATE_CORRUPT",
                    "subagent registry contains noncanonical provider/model/cost data",
                )
        numeric_ids.append(numeric_id)
    if numeric_ids and state["next_agent_id"] <= max(numeric_ids):
        raise SubagentError("STATE_CORRUPT", "next_agent_id would reuse a persisted numeric ID")
    root_present = "0" in state["agents"]
    if root_present != isinstance(state.get("root_identity"), dict):
        raise SubagentError("STATE_CORRUPT", "ROOT record and ROOT identity are inconsistent")
    if any(not isinstance(account, dict) for account in state["accounts"].values()):
        raise SubagentError("STATE_CORRUPT", "subagent registry contains an invalid account record")
    return state


def _save_state_locked(state: dict[str, Any]) -> None:
    state["revision"] = int(state.get("revision", 0)) + 1
    durable_replace_bytes(
        SUBAGENT_STATE_FILE,
        (json.dumps(state, indent=2, sort_keys=True) + "\n").encode("utf-8"),
        mode=0o600,
    )


def _load_bus_locked() -> dict[str, Any]:
    bus = _read_json_locked(SUBAGENT_BUS_FILE, _new_bus)
    if (
        not isinstance(bus.get("messages"), list)
        or not isinstance(bus.get("next_message_id"), int)
        or isinstance(bus.get("next_message_id"), bool)
        or bus["next_message_id"] < 1
    ):
        raise SubagentError("STATE_CORRUPT", "subagent bus invariants are invalid")
    message_ids = [
        message.get("id") if isinstance(message, dict) else None
        for message in bus["messages"]
    ]
    if (
        any(not isinstance(message_id, int) or isinstance(message_id, bool) for message_id in message_ids)
        or message_ids != sorted(set(message_ids))
        or (message_ids and bus["next_message_id"] <= message_ids[-1])
    ):
        raise SubagentError("STATE_CORRUPT", "subagent bus message IDs are invalid")
    return bus


def _save_bus_locked(bus: dict[str, Any]) -> None:
    durable_replace_bytes(
        SUBAGENT_BUS_FILE,
        (json.dumps(bus, indent=2, sort_keys=True) + "\n").encode("utf-8"),
        mode=0o600,
    )


def _response(
    command: str,
    *,
    ok: bool,
    summary: str,
    data: Any = None,
    warnings: list[str] | None = None,
    error_code: str | None = None,
) -> str:
    payload: dict[str, Any] = {
        "schema_version": SUBAGENT_SCHEMA_VERSION,
        "ok": ok,
        "command": command,
        "summary": summary,
        "warnings": warnings or [],
        "data": data,
    }
    if error_code is not None:
        payload["error_code"] = error_code
    return json.dumps(payload, indent=2, sort_keys=True)


def _error_response(command: str, exc: SubagentError) -> str:
    details = None if exc.details is None else {"details": exc.details}
    return _response(
        command,
        ok=False,
        summary=str(exc),
        data=details,
        error_code=exc.code,
    )


def _require_exact_keys(
    arguments: Mapping[str, object],
    *,
    allowed: set[str],
    required: set[str],
) -> None:
    unknown = sorted(set(arguments) - allowed)
    missing = sorted(required - set(arguments))
    if unknown:
        raise SubagentError(
            "INVALID_ARGUMENT",
            "unknown argument(s) for this command: " + ", ".join(unknown),
        )
    if missing:
        raise SubagentError(
            "INVALID_ARGUMENT",
            "missing required argument(s): " + ", ".join(missing),
        )


def _require_string(
    arguments: Mapping[str, object],
    name: str,
    *,
    minimum: int = 0,
    maximum: int | None = None,
) -> str:
    value = arguments.get(name)
    if not isinstance(value, str):
        raise SubagentError("INVALID_ARGUMENT", f"{name} must be a string")
    if maximum is not None and len(value) > maximum:
        raise SubagentError(
            "INVALID_ARGUMENT", f"{name} must contain at most {maximum} characters",
        )
    value = value.strip()
    if len(value) < minimum:
        raise SubagentError(
            "INVALID_ARGUMENT", f"{name} must contain at least {minimum} characters",
        )
    if any(
        (ord(character) < 0x20 and character not in "\n\r\t")
        or 0x7F <= ord(character) <= 0x9F
        for character in value
    ):
        raise SubagentError(
            "INVALID_ARGUMENT", f"{name} must not contain unsafe control characters",
        )
    return value


def _require_integer(
    arguments: Mapping[str, object],
    name: str,
    *,
    minimum: int,
    maximum: int | None = None,
) -> int:
    value = arguments.get(name)
    if not isinstance(value, int) or isinstance(value, bool):
        raise SubagentError("INVALID_ARGUMENT", f"{name} must be an integer")
    if value < minimum or (maximum is not None and value > maximum):
        suffix = f"..{maximum}" if maximum is not None else " or greater"
        raise SubagentError(
            "INVALID_ARGUMENT", f"{name} must be {minimum}{suffix}",
        )
    return value


def _validate_tier(value: object, name: str = "reasoning_effort") -> str:
    if not isinstance(value, str) or value not in TIER_CONFIG:
        raise SubagentError(
            "INVALID_ARGUMENT",
            f"{name} must be one of: " + ", ".join(TIER_ORDER),
        )
    return value


def _validate_label(value: object, *, default: str | None = None) -> str | None:
    if value is None and default is not None:
        return default
    if not isinstance(value, str) or value not in LABELS:
        raise SubagentError(
            "INVALID_ARGUMENT", "label must be one of: " + ", ".join(sorted(LABELS)),
        )
    return value


def _capability_digest(capability: str) -> str:
    return hashlib.sha256(capability.encode("utf-8")).hexdigest()


def _identity_dict(identity: _ProcessIdentity | None) -> dict[str, Any] | None:
    if identity is None:
        return None
    return {
        "pid": identity.pid,
        "ppid": identity.ppid,
        "pgid": identity.pgid,
        "session": identity.session,
        "started": identity.started,
        "command_digest": hashlib.sha256(
            identity.command.encode("utf-8", errors="replace")
        ).hexdigest(),
    }


def _identity_is_live(value: object) -> bool:
    if not isinstance(value, dict):
        return False
    pid = value.get("pid")
    started = value.get("started")
    if not isinstance(pid, int) or not isinstance(started, str):
        return False
    observed = _capture_process_identity(pid)
    return observed is not None and observed.started == started


def _process_anchor() -> _ProcessIdentity | None:
    """Return the nearest Codex/Cursor ancestor, falling back to this MCP."""
    try:
        snapshot = _process_snapshot()
    except RuntimeError:
        return _capture_process_identity(os.getpid())
    current = snapshot.get(os.getpid())
    fallback = current
    visited: set[int] = set()
    while current is not None and current.pid not in visited:
        visited.add(current.pid)
        command = current.command.lower()
        if (
            "codex" in command
            and "build_mcp/server.py" not in command
        ) or re.search(r"(?:^|/)agent(?:\s|$)", command):
            return current
        current = snapshot.get(current.ppid)
    return fallback


def _same_identity_dict(left: object, right: object) -> bool:
    return (
        isinstance(left, dict)
        and isinstance(right, dict)
        and left.get("pid") == right.get("pid")
        and left.get("started") == right.get("started")
    )


def _current_ancestry_contains(identity: object) -> bool:
    """Check exact PID/start-time ancestry for an inherited managed runtime."""
    if not isinstance(identity, dict):
        return False
    try:
        snapshot = _process_snapshot()
    except RuntimeError:
        return False
    current = snapshot.get(os.getpid())
    visited: set[int] = set()
    while current is not None and current.pid not in visited:
        visited.add(current.pid)
        if (
            current.pid == identity.get("pid")
            and current.started == identity.get("started")
        ):
            return True
        current = snapshot.get(current.ppid)
    return False


def _unmarked_caller_is_managed(state: Mapping[str, Any]) -> bool:
    for agent in state["agents"].values():
        if not isinstance(agent, dict) or agent.get("id") == 0:
            continue
        if _current_ancestry_contains(agent.get("runtime_identity")):
            return True
    return False


def _started_epoch(value: object) -> float:
    if not isinstance(value, dict) or not isinstance(value.get("started"), str):
        return float("inf")
    try:
        return time.mktime(time.strptime(value["started"], "%a %b %d %H:%M:%S %Y"))
    except ValueError:
        return float("inf")


def _resolve_caller_locked(state: dict[str, Any]) -> int:
    raw_id = os.environ.get(AGENT_ID_ENV)
    raw_capability = os.environ.get(AGENT_CAPABILITY_ENV)
    agents = state["agents"]
    if (raw_id is None) != (raw_capability is None):
        raise SubagentError(
            "CALLER_UNAUTHORIZED",
            "managed caller identity requires both inherited ID and capability markers",
        )
    if raw_id is not None:
        try:
            caller_id = int(raw_id)
        except ValueError as exc:
            raise SubagentError("CALLER_UNAUTHORIZED", "invalid inherited subagent ID") from exc
        if caller_id <= 0:
            raise SubagentError("CALLER_UNAUTHORIZED", "managed caller ID must be positive")
        agent = agents.get(str(caller_id))
        if not isinstance(agent, dict) or not isinstance(raw_capability, str):
            raise SubagentError("CALLER_UNAUTHORIZED", "unregistered managed subagent caller")
        supplied_digest = _capability_digest(raw_capability)
        accepted_digests = [
            value for value in (
                agent.get("capability_sha256"),
                agent.get("pending_capability_sha256"),
            )
            if isinstance(value, str) and value
        ]
        if not any(
            secrets.compare_digest(candidate, supplied_digest)
            for candidate in accepted_digests
        ):
            raise SubagentError("CALLER_UNAUTHORIZED", "subagent capability does not match its ID")
        if agent.get("status") not in LIVE_STATES:
            raise SubagentError(
                "CALLER_UNAUTHORIZED", "a non-live subagent capability cannot authorize commands",
            )
        return caller_id

    now = time.time()
    current_identity = _identity_dict(_process_anchor())
    if current_identity is None:
        raise SubagentError("ROOT_ELECTION_FAILED", "cannot identify the root caller process")
    if _unmarked_caller_is_managed(state):
        raise SubagentError(
            "CALLER_UNAUTHORIZED",
            "a managed subagent cannot become ROOT by deleting its inherited identity markers",
        )
    stored = state.get("root_identity")
    root = agents.get("0")
    if not isinstance(stored, dict):
        if any(
            isinstance(value, dict) and value.get("id") != 0
            for value in agents.values()
        ):
            raise SubagentError(
                "STATE_CORRUPT",
                "managed agents exist without a persisted ROOT identity; refusing unsafe re-election",
            )
        state["root_identity"] = current_identity
        agents["0"] = {
            "id": 0,
            "parent_id": None,
            "account": "root",
            "provider": "root",
            "tier": None,
            "model": None,
            "reasoning": None,
            "cost": 0,
            "label": "general-purpose",
            "description": "Root orchestrator elected from the oldest validated unmarked MCP caller.",
            "highest_spawn_tier": "specialist",
            "status": "ROOT",
            "operation_state": "READY",
            "created_at": now,
            "state_changed_at": now,
            "last_interrupted_at": None,
            "latest_log_at": None,
            "latest_message_at": None,
            "interruption_count": 0,
            "tokens": 0,
            "tokens_known": True,
        }
        _save_state_locked(state)
        return 0
    if _same_identity_dict(stored, current_identity):
        return 0
    if _identity_is_live(stored):
        if _started_epoch(current_identity) < _started_epoch(stored):
            state["root_identity"] = current_identity
            if isinstance(root, dict):
                root["state_changed_at"] = now
            _save_state_locked(state)
            return 0
        raise SubagentError(
            "ROOT_ALREADY_ELECTED",
            "a newer unmarked MCP caller cannot replace the oldest live ROOT process",
        )

    # Fail closed if this unmarked caller appears to be one of our managed
    # runtimes.  An ordinary root MCP restart may otherwise take over safely.
    for raw_agent in agents.values():
        if not isinstance(raw_agent, dict) or raw_agent.get("id") == 0:
            continue
        if _same_identity_dict(raw_agent.get("runtime_identity"), current_identity):
            raise SubagentError(
                "CALLER_UNAUTHORIZED",
                "a managed subagent cannot become ROOT by deleting its identity environment",
            )
    state["root_identity"] = current_identity
    if isinstance(root, dict):
        root["state_changed_at"] = now
    _save_state_locked(state)
    return 0


def _caller_ceiling(state: Mapping[str, Any], caller_id: int) -> str:
    if caller_id == 0:
        return "specialist"
    agent = state["agents"].get(str(caller_id))
    if not isinstance(agent, dict):
        raise SubagentError("CALLER_UNAUTHORIZED", "caller disappeared from the registry")
    return _validate_tier(agent.get("highest_spawn_tier"), "stored highest_spawn_tier")


def _require_authority(state: Mapping[str, Any], caller_id: int, tier: str) -> None:
    ceiling = _caller_ceiling(state, caller_id)
    if TIER_RANK[tier] > TIER_RANK[ceiling]:
        raise SubagentError(
            "UNAUTHORIZED_TIER",
            f"caller {caller_id} may control at most {ceiling}, not {tier}",
        )


def _require_control_authority(
    state: Mapping[str, Any], caller_id: int, tier: str,
) -> None:
    if caller_id == 0:
        return
    ceiling = _caller_ceiling(state, caller_id)
    if TIER_RANK[tier] > TIER_RANK[ceiling]:
        raise SubagentError(
            "UNAUTHORIZED_TIER",
            f"caller {caller_id} may control at most {ceiling}, not {tier}",
        )


def _effective_ceiling(
    value: object,
    *,
    own_tier: str,
    caller_ceiling: str,
) -> str:
    ceiling = own_tier if value is None else _validate_tier(value, "highest_spawn_tier")
    if TIER_RANK[ceiling] > TIER_RANK[caller_ceiling]:
        raise SubagentError(
            "UNAUTHORIZED_TIER",
            f"cannot delegate ceiling {ceiling}; caller ceiling is {caller_ceiling}",
        )
    return ceiling


def _run(
    arguments: list[str],
    *,
    timeout: float = 15.0,
    input_text: str | None = None,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        arguments,
        input=input_text,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
        timeout=timeout,
    )


def _discover_codex_profiles() -> tuple[list[str], list[str]]:
    """Return allocatable profiles and discovery warnings.

    ``default`` is implicit in codex-profile and commonly aliases a named
    profile.  Named profiles are the configured workforce pool; default is a
    fallback only when there are no named profiles.
    """
    warnings: list[str] = []
    try:
        completed = _run(["codex-profile", "status", "--json"], timeout=15)
    except (OSError, subprocess.SubprocessError) as exc:
        raise SubagentError(
            "ACCOUNT_DISCOVERY_FAILED",
            "codex-profile account discovery could not be executed",
            details=str(exc),
        ) from exc
    profiles: list[str] = []
    if completed.returncode == 0:
        try:
            payload = json.loads(completed.stdout)
            for item in payload.get("profiles", []):
                if (
                    isinstance(item, dict)
                    and isinstance(item.get("name"), str)
                    and item.get("state") == "ok"
                ):
                    profiles.append(item["name"])
        except (AttributeError, json.JSONDecodeError):
            profiles = []
    if not profiles:
        try:
            fallback = _run(["codex-profile", "list"], timeout=10)
        except (OSError, subprocess.SubprocessError) as exc:
            raise SubagentError(
                "ACCOUNT_DISCOVERY_FAILED",
                "codex-profile account-list fallback could not be executed",
                details=str(exc),
            ) from exc
        if fallback.returncode == 0:
            profiles = [line.strip() for line in fallback.stdout.splitlines() if line.strip()]
    if not profiles:
        raise SubagentError(
            "ACCOUNT_DISCOVERY_FAILED",
            "codex-profile did not report any usable accounts",
            details=completed.stderr.strip(),
        )
    named = sorted(set(profiles) - {"default"})
    if named:
        if "default" in profiles:
            warnings.append(
                "The implicit default profile is excluded from allocation to avoid double-counting a named credential."
            )
        return named, warnings
    return ["default"], warnings


def _probe_codex_quota(profile: str, timeout: float = 12.0) -> dict[str, Any]:
    """Read the structured rate-limit snapshot backing Codex ``/status``."""
    process = subprocess.Popen(
        ["codex-profile", "cli", profile, "app-server", "--listen", "stdio://"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=False,
        bufsize=0,
        start_new_session=True,
    )
    stderr_bytes = bytearray()
    request_id = 2
    messages = (
        {"id": 1, "method": "initialize", "params": {
            "clientInfo": {"name": "bioseq-build-quota", "version": "1.0"},
        }},
        {"method": "initialized", "params": None},
        {"id": request_id, "method": "account/rateLimits/read", "params": None},
    )
    try:
        assert process.stdin is not None
        assert process.stdout is not None
        assert process.stderr is not None
        payload = b"".join(
            (json.dumps(message, separators=(",", ":")) + "\n").encode("utf-8")
            for message in messages
        )
        process.stdin.write(payload)
        process.stdin.flush()
        deadline = time.monotonic() + timeout
        response: dict[str, Any] | None = None
        stdout_bytes = bytearray()
        descriptors = {
            process.stdout.fileno(): "stdout",
            process.stderr.fileno(): "stderr",
        }
        while time.monotonic() < deadline:
            if not descriptors:
                break
            ready, _, _ = select.select(
                list(descriptors), [], [], min(0.5, deadline - time.monotonic())
            )
            if not ready:
                if process.poll() is not None:
                    break
                continue
            for descriptor in ready:
                try:
                    chunk = os.read(descriptor, 65_536)
                except BlockingIOError:
                    continue
                if not chunk:
                    descriptors.pop(descriptor, None)
                    continue
                if descriptors.get(descriptor) == "stderr":
                    stderr_bytes.extend(chunk)
                    if len(stderr_bytes) > 64_000:
                        del stderr_bytes[:-64_000]
                    continue
                stdout_bytes.extend(chunk)
                while b"\n" in stdout_bytes:
                    raw_line, _, remainder = stdout_bytes.partition(b"\n")
                    stdout_bytes = bytearray(remainder)
                    try:
                        candidate = json.loads(raw_line.decode("utf-8"))
                    except (UnicodeDecodeError, json.JSONDecodeError):
                        continue
                    if isinstance(candidate, dict) and candidate.get("id") == request_id:
                        response = candidate
                        break
                if response is not None:
                    break
            if response is not None:
                break
        if response is None:
            diagnostic = stderr_bytes.decode("utf-8", errors="replace").strip()
            raise SubagentError(
                "QUOTA_PROBE_FAILED", f"quota probe timed out for {profile}",
                details=diagnostic or None,
            )
        if "error" in response:
            raise SubagentError(
                "QUOTA_PROBE_FAILED", f"quota probe failed for {profile}",
                details=response["error"],
            )
        result = response.get("result")
        if not isinstance(result, dict):
            raise SubagentError("QUOTA_PROBE_FAILED", f"quota response for {profile} is malformed")
        snapshots = result.get("rateLimitsByLimitId")
        candidates: list[tuple[str, dict[str, Any]]] = []
        if isinstance(snapshots, dict):
            # Current App Server releases expose the weekly product bucket as
            # ``codex.primary``.  Older releases used ``codex``.  Do not fall
            # through to arbitrary 10,080-minute buckets (for example an
            # unrelated experimental model limit) and mislabel them Codex.
            for limit_id in ("codex.primary", "codex"):
                snapshot = snapshots.get(limit_id)
                if isinstance(snapshot, dict):
                    candidates.append((limit_id, snapshot))
        if isinstance(result.get("rateLimits"), dict):
            candidates.append(("legacy-rateLimits", result["rateLimits"]))
        weekly: dict[str, Any] | None = None
        plan_type: str | None = None
        selected_limit_id: str | None = None
        for limit_id, snapshot in candidates:
            if isinstance(snapshot.get("planType"), str):
                plan_type = snapshot["planType"]
            for name in ("primary", "secondary"):
                window = snapshot.get(name)
                if (
                    isinstance(window, dict)
                    and window.get("windowDurationMins") == 10080
                    and isinstance(window.get("usedPercent"), (int, float))
                ):
                    weekly = window
                    selected_limit_id = limit_id
                    break
            if weekly is not None:
                break
        if weekly is None:
            raise SubagentError(
                "QUOTA_PROBE_FAILED", f"quota response for {profile} has no weekly Codex window",
            )
        used = min(100.0, max(0.0, float(weekly["usedPercent"])))
        return {
            "remaining_percent": 100.0 - used,
            "used_percent": used,
            "resets_at": weekly.get("resetsAt"),
            "window_minutes": 10080,
            "plan_type": plan_type,
            "limit_id": selected_limit_id,
            "source": "codex app-server account/rateLimits/read (/status data)",
        }
    finally:
        with contextlib.suppress(BrokenPipeError, OSError):
            if process.stdin is not None:
                process.stdin.close()
        if process.poll() is None:
            with contextlib.suppress(ProcessLookupError, OSError):
                os.killpg(process.pid, signal.SIGTERM)
        with contextlib.suppress(subprocess.TimeoutExpired):
            process.wait(timeout=2)
        if process.poll() is None:
            with contextlib.suppress(ProcessLookupError, OSError):
                os.killpg(process.pid, signal.SIGKILL)
            with contextlib.suppress(subprocess.TimeoutExpired):
                process.wait(timeout=2)


def _refresh_quotas() -> list[str]:
    profiles, warnings = _discover_codex_profiles()
    now = time.time()
    claimed: list[tuple[str, str]] = []
    with _registry_lock():
        state = _load_state_locked()
        accounts = state["accounts"]
        for account in accounts.values():
            if isinstance(account, dict) and account.get("provider") == "codex":
                account["available"] = False
        for profile in profiles:
            account = accounts.setdefault(profile, {
                "provider": "codex",
                "profile": profile,
                "quota_history": [],
            })
            account["available"] = True
            sampled_at = account.get("quota_sampled_at")
            attempted_at = account.get("quota_attempted_at", sampled_at)
            lease_at = account.get("quota_probe_started_at")
            due = (
                not isinstance(attempted_at, (int, float))
                or now - float(attempted_at) >= QUOTA_REFRESH_SECONDS
            )
            lease_free = (
                not isinstance(lease_at, (int, float))
                or now - float(lease_at) >= QUOTA_PROBE_LEASE_SECONDS
            )
            if due and lease_free:
                lease_token = secrets.token_hex(12)
                account["quota_probe_started_at"] = now
                account["quota_probe_token"] = lease_token
                account["quota_attempted_at"] = now
                claimed.append((profile, lease_token))
        accounts.setdefault("cursor", {
            "provider": "cursor",
            "profile": None,
            "remaining_percent": None,
            "quota_sampled_at": None,
            "quota_error": "Cursor CLI exposes no weekly quota endpoint.",
            "quota_history": [],
        })
        _save_state_locked(state)

    for profile, lease_token in claimed:
        try:
            sample = _probe_codex_quota(profile)
            error = None
        except (OSError, subprocess.SubprocessError, SubagentError) as exc:
            sample = None
            error = str(exc)
        sampled_at = time.time()
        with _registry_lock():
            state = _load_state_locked()
            account = state["accounts"].setdefault(profile, {
                "provider": "codex", "profile": profile, "quota_history": [],
            })
            if not secrets.compare_digest(
                str(account.get("quota_probe_token", "")), lease_token,
            ):
                continue
            account["quota_probe_started_at"] = None
            account["quota_probe_token"] = None
            if sample is None:
                account["quota_error"] = error
                cache_note = (
                    "no cached percentage is available"
                    if account.get("remaining_percent") is None
                    else "the cached percentage is shown but cannot authorize create"
                )
                warnings.append(
                    f"Quota for {profile} is unavailable ({cache_note}): {error}"
                )
            else:
                previous_reset = account.get("resets_at")
                previous_remaining = account.get("remaining_percent")
                history = account.setdefault("quota_history", [])
                if (
                    previous_reset != sample.get("resets_at")
                    or (
                        isinstance(previous_remaining, (int, float))
                        and sample["remaining_percent"] > float(previous_remaining) + 0.5
                    )
                ):
                    history.clear()
                history.append({
                    "sampled_at": sampled_at,
                    "remaining_percent": sample["remaining_percent"],
                    "resets_at": sample.get("resets_at"),
                })
                del history[:-QUOTA_HISTORY_LIMIT]
                account.update(sample)
                account["quota_sampled_at"] = sampled_at
                account["quota_error"] = None
            _save_state_locked(state)
    return warnings


def _tmux(arguments: list[str], *, timeout: float = 10.0, input_text: str | None = None):
    _ensure_private_directories()
    return _run(
        ["tmux", "-S", str(SUBAGENT_TMUX_SOCKET), *arguments],
        timeout=timeout,
        input_text=input_text,
    )


def _tmux_session_observation(session_name: str) -> bool | None:
    try:
        # tmux otherwise accepts a unique prefix, so an absent agent ``-2``
        # could accidentally resolve the live session for agent ``-20``.
        completed = _tmux(["has-session", "-t", f"={session_name}"], timeout=5)
    except (OSError, subprocess.SubprocessError):
        return None
    if completed.returncode == 0:
        return True
    diagnostic = (completed.stderr + completed.stdout).lower()
    if (
        "no server running" in diagnostic
        or "can't find session" in diagnostic
        or "no current client" in diagnostic
        or (
            "error connecting to" in diagnostic
            and "no such file or directory" in diagnostic
        )
    ):
        return False
    return None


def _tmux_has_session(session_name: str) -> bool:
    return _tmux_session_observation(session_name) is True


def _tmux_pane_info(target: str) -> dict[str, Any] | None:
    try:
        completed = _tmux([
            "display-message", "-p", "-t", target,
            "#{pane_id}\t#{pane_pid}\t#{pane_dead}\t#{pane_dead_status}\t#{@bioseq_repository}\t#{@bioseq_agent_id}\t#{pane_current_command}",
        ], timeout=5)
    except (OSError, subprocess.SubprocessError):
        return None
    if completed.returncode != 0:
        return None
    fields = completed.stdout.rstrip("\n").split("\t", 6)
    if len(fields) != 7:
        return None
    try:
        pane_pid = int(fields[1])
    except ValueError:
        pane_pid = None
    return {
        "pane_target": fields[0],
        "pane_pid": pane_pid,
        "pane_dead": fields[2] == "1",
        "pane_dead_status": (
            int(fields[3]) if fields[3].lstrip("-").isdigit() else None
        ),
        "repository_marker": fields[4],
        "agent_marker": fields[5],
        "pane_command": fields[6],
    }


def _verify_tmux_ownership(agent: Mapping[str, Any], pane: Mapping[str, Any]) -> None:
    if (
        pane.get("repository_marker") != _REPOSITORY_KEY
        or pane.get("agent_marker") != str(agent.get("id"))
    ):
        raise SubagentError(
            "RUNTIME_OWNERSHIP_MISMATCH",
            "refusing to mutate a tmux pane that is not marked for this repository and agent",
        )


def _capture_pane(target: str, *, history: bool = True) -> str:
    arguments = ["capture-pane", "-p", "-J"]
    if history:
        arguments.extend(["-S", "-"])
    arguments.extend(["-t", target])
    completed = _tmux(arguments, timeout=10)
    if completed.returncode != 0:
        return ""
    return _sanitize_log(completed.stdout)


def _sanitize_log(text: str) -> str:
    text = _ANSI_RE.sub("", text)
    return "".join(
        character
        for character in text
        if character in "\n\r\t" or ord(character) >= 0x20
    )


def _send_literal(target: str, text: str) -> None:
    buffer_name = f"bioseq-agent-{secrets.token_hex(6)}"
    loaded = _tmux(
        ["load-buffer", "-b", buffer_name, "-"],
        timeout=10,
        input_text=text,
    )
    if loaded.returncode != 0:
        raise SubagentError(
            "RUNTIME_ERROR", "tmux could not load the agent instruction buffer",
            details=loaded.stderr.strip(),
        )
    pasted = _tmux([
        "paste-buffer", "-d", "-b", buffer_name, "-t", target,
    ], timeout=10)
    if pasted.returncode != 0:
        with contextlib.suppress(Exception):
            _tmux(["delete-buffer", "-b", buffer_name], timeout=5)
        raise SubagentError(
            "RUNTIME_ERROR", "tmux could not paste instructions into the agent",
            details=pasted.stderr.strip(),
        )
    time.sleep(0.12)
    entered = _tmux(["send-keys", "-t", target, "Enter"], timeout=5)
    if entered.returncode != 0:
        raise SubagentError(
            "RUNTIME_ERROR", "tmux could not submit instructions to the agent",
            details=entered.stderr.strip(),
        )
    # Codex's multiline paste placeholder can consume the first Enter merely
    # to materialize the pasted text.  A second Enter submits it; when the
    # first already submitted, the active-turn UI safely ignores the second.
    time.sleep(0.18)
    entered_again = _tmux(["send-keys", "-t", target, "Enter"], timeout=5)
    if entered_again.returncode != 0:
        raise SubagentError(
            "RUNTIME_ERROR", "tmux could not confirm instruction submission",
            details=entered_again.stderr.strip(),
        )


def _wait_for_text(target: str, pattern: re.Pattern[str], timeout: float) -> str:
    deadline = time.monotonic() + timeout
    latest = ""
    while time.monotonic() < deadline:
        latest = _capture_pane(target, history=False)
        if pattern.search(latest):
            return latest
        time.sleep(0.2)
    return latest


def _wait_for_codex_ready(target: str, timeout: float = 60.0) -> str:
    """Wait until model and configured MCP startup have reached an idle composer."""
    deadline = time.monotonic() + timeout
    earliest_ready = time.monotonic() + 3.0
    stable_ready_observations = 0
    latest = ""
    while time.monotonic() < deadline:
        latest = _capture_pane(target, history=False)
        startup_busy = "Starting MCP servers" in latest or "esc to interrupt" in latest
        model_loading = re.search(r"model:\s+loading", latest, re.IGNORECASE) is not None
        composer_visible = "›" in latest or re.search(r"^>\s", latest, re.MULTILINE) is not None
        if (
            time.monotonic() >= earliest_ready
            and composer_visible
            and not startup_busy
            and not model_loading
        ):
            stable_ready_observations += 1
            if stable_ready_observations >= 2:
                return latest
        else:
            stable_ready_observations = 0
        time.sleep(0.25)
    raise SubagentError(
        "RUNTIME_ERROR",
        "Codex did not reach an idle composer after model/MCP startup; refusing to inject work into an indeterminate TUI state",
    )


def _observe_codex_assignment(
    agent: Mapping[str, Any],
    target: str,
    *,
    not_before: float,
    timeout: float = 10.0,
) -> str:
    """Return WORKING/INTERRUPTED only after Codex acknowledges the new turn."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        pane = _capture_pane(target, history=False)
        if "esc to interrupt" in pane:
            return "WORKING"
        telemetry = _read_codex_telemetry(agent)
        state_at = _iso_epoch(telemetry.get("turn_state_at"))
        if state_at is not None and state_at >= not_before:
            if telemetry.get("turn_active") is True:
                return "WORKING"
            if telemetry.get("turn_active") is False:
                return "INTERRUPTED"
        time.sleep(0.2)
    raise SubagentError(
        "RUNTIME_ERROR",
        "Codex did not acknowledge the submitted assignment with a turn event or active-turn UI",
    )


def _runtime_arguments(agent: Mapping[str, Any], *, resume: bool) -> list[str]:
    provider = agent["provider"]
    if provider == "codex":
        arguments = [
            "codex-profile", "cli", str(agent["account"]),
        ]
        if resume:
            session_id = agent.get("vendor_session_id")
            if not isinstance(session_id, str) or not session_id:
                raise SubagentError(
                    "RESUME_CONTEXT_MISSING",
                    "the killed Codex agent has no saved session ID and cannot be resumed honestly",
                )
            arguments.append("resume")
        arguments.extend([
            "--yolo", "--no-alt-screen",
            "-C", str(REPOSITORY_ROOT),
            "-m", str(agent["model"]),
            "-c", f'model_reasoning_effort="{agent["reasoning"]}"',
        ])
        if resume:
            arguments.append(str(agent["vendor_session_id"]))
        return arguments
    if provider == "cursor":
        arguments = [
            "agent", "--yolo", "--sandbox", "disabled", "-p",
            "--output-format", "stream-json",
            "--workspace", str(REPOSITORY_ROOT),
            # Cursor documents the display name as "Composer 2.5" while
            # ``agent --list-models`` exposes this canonical CLI identifier.
            "--model", str(agent["model"]),
        ]
        if resume:
            session_id = agent.get("vendor_session_id")
            if not isinstance(session_id, str) or not session_id:
                raise SubagentError(
                    "RESUME_CONTEXT_MISSING",
                    "the killed Cursor agent has no saved chat ID and cannot be resumed honestly",
                )
            arguments.extend(["--resume", session_id])
        return arguments
    raise SubagentError("RUNTIME_ERROR", f"unsupported agent provider: {provider}")


def _prepare_tmux_session(agent: Mapping[str, Any], capability: str) -> tuple[str, str]:
    del capability  # The secret is applied only to the respawned vendor process.
    session_name = str(agent["tmux_session"])
    session_observation = _tmux_session_observation(session_name)
    if session_observation is None:
        raise SubagentError(
            "RUNTIME_UNKNOWN",
            "tmux session availability could not be determined safely",
        )
    created_session = session_observation is False
    created_session_id: str | None = None
    if created_session:
        # The user's tmux configuration may set non-zero base-index or
        # pane-base-index values.  Select the exact fresh session's active
        # pane instead of assuming window 0 / pane 0.
        target_hint = f"={session_name}:"
        # tmux receives this complete command list in one server request.  It
        # cannot expose a newly-created unmarked session if the MCP client dies
        # between separate subprocess calls.
        created = _tmux([
            "new-session", "-d", "-P", "-F", "#{session_id}",
            "-s", session_name,
            "-x", "180", "-y", "60", "-c", str(REPOSITORY_ROOT),
            ";", "set-option", "-p", "-t", target_hint,
            "@bioseq_repository", _REPOSITORY_KEY,
            ";", "set-option", "-p", "-t", target_hint,
            "@bioseq_agent_id", str(agent["id"]),
        ], timeout=15)
        reported_session_id = created.stdout.strip()
        if re.fullmatch(r"\$[0-9]+", reported_session_id):
            created_session_id = reported_session_id
        if created.returncode != 0:
            # The command list is sequential: new-session may have succeeded
            # before a marker write failed.  Roll back only when tmux emitted
            # the immutable session ID created by this exact invocation; an
            # empty result may instead be a duplicate-name race.
            if created_session_id is not None:
                with contextlib.suppress(Exception):
                    _tmux(["kill-session", "-t", created_session_id], timeout=5)
            raise SubagentError(
                "RUNTIME_ERROR", "tmux could not create the persistent agent session",
                details=created.stderr.strip(),
            )
    try:
        pane = _tmux_pane_info(session_name)
        if pane is None:
            raise SubagentError("RUNTIME_ERROR", "tmux agent session has no observable pane")
        target = str(pane["pane_target"])
        if created_session:
            pane = _tmux_pane_info(target) or pane
        _verify_tmux_ownership(agent, pane)
        for option, value in (
            ("remain-on-exit", "on"),
            ("history-limit", str(TMUX_HISTORY_LINES)),
        ):
            configured = _tmux([
                "set-option", "-p", "-t", target, option, value,
            ], timeout=5)
            if configured.returncode != 0:
                raise SubagentError(
                    "RUNTIME_ERROR", f"tmux could not set {option} for the agent pane",
                    details=configured.stderr.strip(),
                )
        log_path = Path(str(agent["log_file"]))
        log_path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        log_path.touch(mode=0o600, exist_ok=True)
        with contextlib.suppress(OSError):
            log_path.chmod(0o600)
        return session_name, target
    except Exception:
        if created_session_id is not None:
            with contextlib.suppress(Exception):
                _tmux(["kill-session", "-t", created_session_id], timeout=5)
        raise


def _attach_runtime_log(
    agent: Mapping[str, Any],
    target: str,
    operation_context: tuple[int, int, str] | None,
) -> Path:
    """Attach one generation-specific pipe while preserving the aggregate log."""
    aggregate_path = Path(str(agent["log_file"]))
    runtime_path = aggregate_path
    if operation_context is not None:
        agent_id, generation, nonce = operation_context
        runtime_path = SUBAGENT_LOG_DIR / (
            f"agent-{agent_id}-generation-{generation}-{nonce[:12]}.jsonl"
        )
    for path in {aggregate_path, runtime_path}:
        path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        path.touch(mode=0o600, exist_ok=True)
        with contextlib.suppress(OSError):
            path.chmod(0o600)
    # Closing the previous pipe first guarantees that buffered prior output is
    # flushed only to its old generation file.  The new pipe duplicates output
    # to the all-generation log and this invocation's unambiguous stream.
    closed = _tmux(["pipe-pane", "-t", target], timeout=5)
    if closed.returncode != 0:
        raise SubagentError(
            "RUNTIME_ERROR", "tmux could not close the previous agent log pipe",
            details=closed.stderr.strip(),
        )
    destinations = [shlex.quote(str(aggregate_path))]
    if runtime_path != aggregate_path:
        destinations.append(shlex.quote(str(runtime_path)))
    pipe_command = f"exec /usr/bin/tee -a -- {' '.join(destinations)}"
    piped = _tmux(["pipe-pane", "-O", "-t", target, pipe_command], timeout=5)
    if piped.returncode != 0:
        raise SubagentError(
            "RUNTIME_ERROR", "tmux could not attach the persistent agent log",
            details=piped.stderr.strip(),
        )
    return runtime_path


def _launch_runtime(
    agent: Mapping[str, Any],
    capability: str,
    prompt: str,
    *,
    resume: bool,
    operation_context: tuple[int, int, str] | None = None,
) -> dict[str, Any]:
    session_name, target = _prepare_tmux_session(agent, capability)
    runtime_log_path = _attach_runtime_log(agent, target, operation_context)
    _record_operation_progress(
        operation_context,
        "SESSION_OWNED",
        tmux_pane=target,
        operation_runtime_log_file=str(runtime_log_path),
    )
    arguments = _runtime_arguments(agent, resume=resume)
    cursor_log_start = 0
    cursor_agent = dict(agent)
    cursor_agent["log_file"] = str(runtime_log_path)
    expected_session_id: str | None = None
    instruction_sent_at: float | None = None
    if agent["provider"] == "cursor":
        if operation_context is None:
            with contextlib.suppress(OSError):
                cursor_log_start = runtime_log_path.stat().st_size
        if resume:
            expected_value = agent.get("vendor_session_id")
            expected_session_id = (
                str(expected_value) if isinstance(expected_value, str) else None
            )
        # Cursor print mode receives the assignment in the respawn argv, so
        # journal the log boundary and expected resume chat before respawn.
        instruction_sent_at = time.time()
        _record_operation_progress(
            operation_context,
            "INSTRUCTION_SENDING",
            operation_cursor_log_start=cursor_log_start,
            operation_expected_session_id=expected_session_id,
            operation_instruction_sent_at=instruction_sent_at,
            last_instruction_at=instruction_sent_at,
        )
    else:
        _record_operation_progress(operation_context, "VENDOR_STARTING")
    environment = [
        "-e", f"{AGENT_ID_ENV}={agent['id']}",
        "-e", f"{AGENT_CAPABILITY_ENV}={capability}",
    ]
    # tmux 3.5 accepts the shell command as separate argv after ``--``.  User
    # instructions never enter a shell command: Codex receives them through a
    # literal paste buffer; Cursor receives one direct argv element.
    cursor_prompt = ["--", prompt] if agent["provider"] == "cursor" else []
    respawned = _tmux([
        "respawn-pane", "-k", "-t", target,
        "-c", str(REPOSITORY_ROOT), *environment, "--",
        *arguments, *cursor_prompt,
    ], timeout=15)
    if respawned.returncode != 0:
        raise SubagentError(
            "RUNTIME_ERROR", "tmux could not launch the agent process",
            details=respawned.stderr.strip(),
        )
    pane = _tmux_pane_info(target)
    if pane is None or (pane.get("pane_dead") and agent["provider"] == "codex"):
        raise SubagentError("RUNTIME_ERROR", "agent process exited during launch")

    identity = (
        _identity_dict(_capture_process_identity(pane["pane_pid"]))
        if isinstance(pane.get("pane_pid"), int)
        else None
    )
    runtime_progress = {
        "tmux_pane": pane.get("pane_target", target),
        "pane_pid": pane.get("pane_pid"),
        "runtime_identity": identity,
    }
    _record_operation_progress(
        operation_context,
        "INSTRUCTION_SENT" if agent["provider"] == "cursor" else "VENDOR_STARTED",
        **runtime_progress,
    )

    vendor_session_id = agent.get("vendor_session_id")
    cursor_snapshot: dict[str, Any] = {}
    if agent["provider"] == "codex":
        _wait_for_codex_ready(target, timeout=60)
        if not resume:
            _tmux(["send-keys", "-t", target, "-l", "/status"], timeout=5)
            time.sleep(0.15)
            _tmux(["send-keys", "-t", target, "Enter"], timeout=5)
            time.sleep(0.15)
            # The first Enter can accept the slash-command completion; the
            # second executes it on current Codex releases.
            _tmux(["send-keys", "-t", target, "Enter"], timeout=5)
            status_text = _wait_for_text(target, _SESSION_RE, timeout=12)
            match = _SESSION_RE.search(status_text)
            if match is None:
                raise SubagentError(
                    "RUNTIME_ERROR",
                    "Codex started but its persistent session ID could not be captured",
                )
            vendor_session_id = match.group(1)
            _tmux(["send-keys", "-t", target, "Escape"], timeout=5)
            _wait_for_codex_ready(target, timeout=15)
        instruction_sent_at = time.time()
        _record_operation_progress(
            operation_context,
            "INSTRUCTION_SENDING",
            vendor_session_id=vendor_session_id,
            operation_instruction_sent_at=instruction_sent_at,
            last_instruction_at=instruction_sent_at,
        )
        _send_literal(target, prompt)
        _record_operation_progress(operation_context, "INSTRUCTION_SENT")
        telemetry_agent = dict(agent)
        telemetry_agent["vendor_session_id"] = vendor_session_id
        acknowledged_status = _observe_codex_assignment(
            telemetry_agent,
            target,
            not_before=instruction_sent_at,
        )
        _record_operation_progress(
            operation_context,
            "ASSIGNMENT_ACKNOWLEDGED",
            vendor_session_id=vendor_session_id,
            operation_assignment_acknowledged_at=time.time(),
        )
    else:
        cursor_snapshot = _wait_for_cursor_init(
            cursor_agent,
            target,
            start_offset=cursor_log_start,
        )
        observed_session_id = str(cursor_snapshot["session_id"])
        if resume and observed_session_id != expected_session_id:
            cleanup_agent = dict(agent)
            cleanup_agent["tmux_pane"] = target
            cleanup_error: str | None = None
            try:
                _kill_runtime(cleanup_agent)
            except SubagentError as exc:
                cleanup_error = str(exc)
            raise SubagentError(
                "RESUME_CONTEXT_MISMATCH",
                "Cursor opened a different chat instead of resuming the exact saved context; the replacement was stopped when ownership could be confirmed.",
                details={
                    "expected_session_id": expected_session_id,
                    "observed_session_id": observed_session_id,
                    "cleanup_error": cleanup_error,
                },
            )
        vendor_session_id = observed_session_id
        _record_operation_progress(
            operation_context,
            "ASSIGNMENT_ACKNOWLEDGED",
            vendor_session_id=vendor_session_id,
            operation_assignment_acknowledged_at=time.time(),
        )
        if (
            cursor_snapshot.get("terminal_result") is True
            and cursor_snapshot.get("result_error") is True
        ):
            cleanup_agent = dict(agent)
            cleanup_agent["tmux_pane"] = target
            with contextlib.suppress(SubagentError):
                _kill_runtime(cleanup_agent)
            raise SubagentError(
                "CURSOR_RESULT_ERROR",
                "Cursor reported an error for the submitted contractor-intern assignment.",
                details=cursor_snapshot.get("result_diagnostic"),
            )
        acknowledged_status = "WORKING"

    pane = _tmux_pane_info(target) or pane
    if agent["provider"] == "cursor":
        cursor_snapshot = _cursor_log_snapshot(
            cursor_agent, start_offset=cursor_log_start,
        )
        if pane.get("pane_dead") and cursor_snapshot.get("terminal_result") is not True:
            # tmux can observe child exit a few milliseconds before the pipe's
            # tee process flushes its final JSONL event.  Settle the unique
            # generation log before classifying the exit as malformed.
            settle_deadline = time.monotonic() + 1.0
            while time.monotonic() < settle_deadline:
                time.sleep(0.05)
                cursor_snapshot = _cursor_log_snapshot(
                    cursor_agent, start_offset=cursor_log_start,
                )
                if cursor_snapshot.get("terminal_result") is True:
                    break
        final_session_id = cursor_snapshot.get("session_id")
        if (
            resume
            and cursor_snapshot.get("initialized") is True
            and final_session_id != expected_session_id
        ):
            cleanup_agent = dict(agent)
            cleanup_agent["tmux_pane"] = target
            with contextlib.suppress(SubagentError):
                _kill_runtime(cleanup_agent)
            raise SubagentError(
                "RESUME_CONTEXT_MISMATCH",
                "Cursor's final stream segment did not retain the exact requested chat ID.",
                details={
                    "expected_session_id": expected_session_id,
                    "observed_session_id": final_session_id,
                },
            )
        if (
            cursor_snapshot.get("terminal_result") is True
            and cursor_snapshot.get("result_error") is True
        ):
            cleanup_agent = dict(agent)
            cleanup_agent["tmux_pane"] = target
            with contextlib.suppress(SubagentError):
                _kill_runtime(cleanup_agent)
            raise SubagentError(
                "CURSOR_RESULT_ERROR",
                "Cursor reported an error for the submitted contractor-intern assignment.",
                details=cursor_snapshot.get("result_diagnostic"),
            )
        if pane.get("pane_dead") and cursor_snapshot.get("terminal_result") is not True:
            raise SubagentError(
                "RUNTIME_ERROR", "Cursor exited without a terminal stream-json result",
            )
        if cursor_snapshot.get("terminal_result") is True:
            runtime_view = dict(agent)
            runtime_view.update({
                "tmux_pane": target,
                "runtime_identity": identity,
            })
            if not pane.get("pane_dead") and _wait_for_owned_runtime_exit(
                runtime_view, timeout=1.0,
            ):
                pane = _tmux_pane_info(target) or {**pane, "pane_dead": True}
            acknowledged_status = (
                "KILLED" if pane.get("pane_dead") else "INTERRUPTED"
            )
    identity = (
        _identity_dict(_capture_process_identity(pane["pane_pid"]))
        if isinstance(pane.get("pane_pid"), int) and not pane.get("pane_dead")
        else identity
    )
    result = {
        "tmux_session": session_name,
        "tmux_pane": target,
        "pane_pid": pane.get("pane_pid"),
        "runtime_identity": identity,
        "vendor_session_id": vendor_session_id,
        "runtime_log_file": str(runtime_log_path),
        "_instruction_sent_at": instruction_sent_at,
        "_acknowledged_status": acknowledged_status,
    }
    if agent["provider"] == "cursor":
        if cursor_snapshot.get("latest_message_at") is not None:
            result["latest_message_at"] = cursor_snapshot["latest_message_at"]
        usage_tokens = cursor_snapshot.get("usage_tokens")
        if (
            cursor_snapshot.get("scan_complete") is True
            and isinstance(usage_tokens, int)
            and not isinstance(usage_tokens, bool)
        ):
            previous = agent.get("tokens")
            result["tokens"] = (
                int(previous) + usage_tokens
                if agent.get("tokens_known") is True
                and isinstance(previous, int)
                and not isinstance(previous, bool)
                else usage_tokens
            )
            result["tokens_known"] = True
    return result


def _interrupt_runtime(agent: Mapping[str, Any]) -> None:
    target = agent.get("tmux_pane") or agent.get("tmux_session")
    if not isinstance(target, str) or not target:
        raise SubagentError("RUNTIME_MISSING", "agent has no persisted tmux target")
    pane = _tmux_pane_info(target)
    if pane is None:
        raise SubagentError("RUNTIME_MISSING", "agent tmux pane is not observable")
    _verify_tmux_ownership(agent, pane)
    sent = _tmux(["send-keys", "-t", target, "Escape"], timeout=5)
    if sent.returncode != 0:
        raise SubagentError(
            "RUNTIME_MISSING", "could not send Escape to the agent process",
            details=sent.stderr.strip(),
        )


def _wait_for_owned_runtime_exit(
    agent: Mapping[str, Any],
    *,
    timeout: float = 0.75,
) -> bool:
    """Return true only when the exact owned runtime is observed stopped."""
    target = agent.get("tmux_pane") or agent.get("tmux_session")
    if not isinstance(target, str) or not target:
        return False
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        pane = _tmux_pane_info(target)
        if pane is not None:
            _verify_tmux_ownership(agent, pane)
            if pane.get("pane_dead"):
                return True
        else:
            session_name = agent.get("tmux_session")
            observation = (
                _tmux_session_observation(str(session_name))
                if isinstance(session_name, str) else None
            )
            if observation is False and not _identity_is_live(agent.get("runtime_identity")):
                return True
        time.sleep(0.05)
    return False


def _owned_runtime_observation(
    agent: Mapping[str, Any],
) -> tuple[bool | None, dict[str, Any] | None]:
    """Observe one owned runtime without collapsing unavailable into absent."""
    session_name = agent.get("tmux_session")
    target = agent.get("tmux_pane") or session_name
    if not isinstance(session_name, str) or not isinstance(target, str):
        return False, None
    present = _tmux_session_observation(session_name)
    if present is False:
        if _identity_is_live(agent.get("runtime_identity")):
            return None, None
        return False, None
    if present is None:
        return None, None
    pane = _tmux_pane_info(target)
    if pane is None:
        return None, None
    _verify_tmux_ownership(agent, pane)
    return not bool(pane.get("pane_dead")), pane


def _kill_runtime(agent: Mapping[str, Any]) -> None:
    target = agent.get("tmux_pane") or agent.get("tmux_session")
    if not isinstance(target, str) or not target:
        return
    session_observation = _tmux_session_observation(
        str(agent.get("tmux_session", "")),
    )
    if session_observation is None:
        raise SubagentError(
            "RUNTIME_UNKNOWN",
            "tmux session availability could not be verified; refusing to release budget",
        )
    if session_observation is False:
        if _identity_is_live(agent.get("runtime_identity")):
            raise SubagentError(
                "RUNTIME_UNKNOWN",
                "tmux reports no session but the persisted process identity is still live; refusing to release budget",
            )
        return
    pane = _tmux_pane_info(target)
    if pane is None:
        raise SubagentError("RUNTIME_MISSING", "agent tmux pane is not observable")
    _verify_tmux_ownership(agent, pane)
    # respawn-pane -k kills exactly this pane's foreground process tree.  The
    # short-lived true command exits immediately and remain-on-exit preserves
    # the dead pane, scrollback and pipe log for later repurpose/logs.
    killed = _tmux([
        "respawn-pane", "-k", "-t", target, "--", "/usr/bin/true",
    ], timeout=10)
    if killed.returncode != 0:
        raise SubagentError(
            "RUNTIME_ERROR", "tmux could not kill the agent pane",
            details=killed.stderr.strip(),
        )
    deadline = time.monotonic() + 3.0
    while time.monotonic() < deadline:
        observed = _tmux_pane_info(target)
        if observed is not None and observed.get("pane_dead"):
            return
        time.sleep(0.05)
    raise SubagentError(
        "RUNTIME_ERROR", "tmux accepted the kill but the owned pane did not reach a dead state",
    )


def _start_idle_runtime(agent: Mapping[str, Any]) -> dict[str, Any]:
    """Keep a Cursor agent's owned pane alive while its vendor child is interrupted."""
    target = agent.get("tmux_pane") or agent.get("tmux_session")
    if not isinstance(target, str) or not target:
        raise SubagentError("RUNTIME_MISSING", "agent has no persisted tmux target")
    pane = _tmux_pane_info(target)
    if pane is None:
        raise SubagentError("RUNTIME_MISSING", "agent tmux pane is not observable")
    _verify_tmux_ownership(agent, pane)
    respawned = _tmux([
        "respawn-pane", "-k", "-t", target, "-c", str(REPOSITORY_ROOT),
        "--", "/usr/bin/tail", "-f", "/dev/null",
    ], timeout=10)
    if respawned.returncode != 0:
        raise SubagentError(
            "RUNTIME_ERROR", "tmux could not start the interrupted Cursor idle supervisor",
            details=respawned.stderr.strip(),
        )
    observed = _tmux_pane_info(target)
    if observed is None or observed.get("pane_dead"):
        raise SubagentError("RUNTIME_ERROR", "Cursor idle supervisor exited during launch")
    _verify_tmux_ownership(agent, observed)
    identity = (
        _identity_dict(_capture_process_identity(observed["pane_pid"]))
        if isinstance(observed.get("pane_pid"), int) else None
    )
    return {
        "tmux_pane": observed.get("pane_target"),
        "pane_pid": observed.get("pane_pid"),
        "runtime_identity": identity,
    }


def _tail_file(path: Path, lines: int) -> str:
    try:
        with path.open("rb") as handle:
            handle.seek(0, os.SEEK_END)
            position = handle.tell()
            blocks: list[bytes] = []
            newline_count = 0
            while position > 0 and newline_count <= lines:
                block_size = min(8192, position)
                position -= block_size
                handle.seek(position)
                block = handle.read(block_size)
                blocks.append(block)
                newline_count += block.count(b"\n")
                if sum(map(len, blocks)) >= 2 * LOG_RESPONSE_MAX_CHARS:
                    break
        text = b"".join(reversed(blocks)).decode("utf-8", errors="replace")
    except OSError:
        return ""
    text = "\n".join(text.splitlines()[-lines:])
    text = _sanitize_log(text)
    if len(text) > LOG_RESPONSE_MAX_CHARS:
        text = "…(truncated)…\n" + text[-LOG_RESPONSE_MAX_CHARS:]
    return text


def _cursor_log_snapshot(
    agent: Mapping[str, Any],
    *,
    start_offset: int = 0,
    maximum_bytes: int = 8 * 1024 * 1024,
) -> dict[str, Any]:
    """Read Cursor stream-json, keeping result state for the latest init turn.

    Token usage is cumulative across every complete event in the selected byte
    range.  Session/result/message state is reset at each ``system/init`` so an
    old turn cannot prove that a later resume was accepted or completed.
    """
    log_path = Path(str(agent.get("log_file", "")))
    initialized = False
    session_id: str | None = None
    terminal_result = False
    result_error: bool | None = None
    result_diagnostic: str | None = None
    usage_tokens: int | None = None
    assistant_seen = False
    latest_message_at: str | float | None = None
    bytes_seen = 0
    scan_complete = True
    try:
        with log_path.open("rb") as handle:
            handle.seek(max(0, start_offset))
            while bytes_seen <= maximum_bytes:
                raw_line = handle.readline()
                if not raw_line:
                    break
                bytes_seen += len(raw_line)
                if not raw_line.endswith(b"\n"):
                    scan_complete = False
                    break
                if bytes_seen > maximum_bytes:
                    scan_complete = False
                    break
                try:
                    event = json.loads(raw_line.decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError):
                    continue
                if not isinstance(event, dict):
                    continue
                event_type = event.get("type")
                subtype = event.get("subtype")
                if event_type == "system" and subtype == "init":
                    candidate = event.get("session_id")
                    if not isinstance(candidate, str) or not candidate:
                        candidate = event.get("sessionId")
                    if isinstance(candidate, str) and candidate:
                        initialized = True
                        session_id = candidate
                        terminal_result = False
                        result_error = None
                        result_diagnostic = None
                        assistant_seen = False
                        latest_message_at = None
                message = event.get("message")
                is_assistant = event_type == "assistant" or (
                    event_type == "message"
                    and (
                        event.get("role") == "assistant"
                        or isinstance(message, dict)
                        and message.get("role") == "assistant"
                    )
                )
                if is_assistant:
                    assistant_seen = True
                    timestamp: object = event.get("timestamp")
                    if timestamp is None and isinstance(message, dict):
                        timestamp = message.get("timestamp", message.get("created_at"))
                    if (
                        isinstance(timestamp, str) and timestamp.strip()
                    ) or (
                        isinstance(timestamp, (int, float))
                        and not isinstance(timestamp, bool)
                    ):
                        latest_message_at = timestamp
                if event_type == "result":
                    terminal_result = True
                    if isinstance(event.get("is_error"), bool):
                        result_error = bool(event["is_error"])
                    elif isinstance(subtype, str):
                        result_error = subtype.lower() not in {"success", "completed"}
                    diagnostic_value = event.get(
                        "error", event.get("result", event.get("message", subtype))
                    )
                    if diagnostic_value is not None:
                        if isinstance(diagnostic_value, str):
                            diagnostic_text = diagnostic_value
                        else:
                            try:
                                diagnostic_text = json.dumps(
                                    diagnostic_value, ensure_ascii=False, sort_keys=True,
                                )
                            except (TypeError, ValueError):
                                diagnostic_text = repr(diagnostic_value)
                        result_diagnostic = _sanitize_log(diagnostic_text).strip()[:2_000]
                    usage = event.get("usage")
                    if isinstance(usage, dict):
                        event_usage_tokens: int | None = None
                        explicit_total = usage.get("total_tokens", usage.get("totalTokens"))
                        if (
                            isinstance(explicit_total, int)
                            and not isinstance(explicit_total, bool)
                            and explicit_total >= 0
                        ):
                            event_usage_tokens = explicit_total
                        else:
                            input_tokens = usage.get("input_tokens", usage.get("inputTokens"))
                            output_tokens = usage.get("output_tokens", usage.get("outputTokens"))
                            valid = [
                                value for value in (input_tokens, output_tokens)
                                if isinstance(value, int)
                                and not isinstance(value, bool)
                                and value >= 0
                            ]
                            if valid:
                                event_usage_tokens = sum(valid)
                        if event_usage_tokens is not None:
                            usage_tokens = (usage_tokens or 0) + event_usage_tokens
    except OSError:
        pass
    if assistant_seen and latest_message_at is None and scan_complete:
        # Cursor events do not consistently carry timestamps.  The durable
        # stream log mtime is a documented approximation for the newest
        # assistant event in the latest initialized segment.
        with contextlib.suppress(OSError):
            latest_message_at = log_path.stat().st_mtime
    return {
        "initialized": initialized,
        "session_id": session_id,
        "terminal_result": terminal_result,
        "result_error": result_error,
        "result_diagnostic": result_diagnostic,
        "usage_tokens": usage_tokens,
        "latest_message_at": latest_message_at,
        "scan_complete": scan_complete and bytes_seen <= maximum_bytes,
    }


def _wait_for_cursor_init(
    agent: Mapping[str, Any],
    target: str,
    *,
    start_offset: int,
    timeout: float = 12.0,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    dead_observed_at: float | None = None
    latest: dict[str, Any] = {}
    while time.monotonic() < deadline:
        latest = _cursor_log_snapshot(agent, start_offset=start_offset)
        if latest.get("initialized") is True and isinstance(latest.get("session_id"), str):
            return latest
        pane = _tmux_pane_info(target)
        if pane is not None and pane.get("pane_dead"):
            if dead_observed_at is None:
                dead_observed_at = time.monotonic()
            elif time.monotonic() - dead_observed_at >= 1.0:
                break
        time.sleep(0.05)
    raise SubagentError(
        "RESUME_CONTEXT_MISSING",
        "Cursor did not emit its system/init session ID; refusing to register an unresumable contractor-intern runtime",
        details={"terminal_result_seen": latest.get("terminal_result") is True},
    )


def _parse_vendor_session_from_log(agent: Mapping[str, Any]) -> str | None:
    log_path = Path(str(agent.get("log_file", "")))
    if agent.get("provider") == "codex":
        text = _tail_file(log_path, 2000)
        matches = _SESSION_RE.findall(text)
        return matches[-1] if matches else None
    snapshot = _cursor_log_snapshot(agent, maximum_bytes=4 * 1024 * 1024)
    session_id = snapshot.get("session_id")
    return session_id if snapshot.get("initialized") is True and isinstance(session_id, str) else None


def _profile_home(profile: str) -> Path | None:
    try:
        completed = _run(["codex-profile", "path", profile], timeout=10)
    except (OSError, subprocess.SubprocessError):
        return None
    if completed.returncode != 0:
        return None
    value = completed.stdout.strip()
    return Path(value) if value else None


def _read_codex_telemetry(agent: Mapping[str, Any]) -> dict[str, Any]:
    session_id = agent.get("vendor_session_id")
    account = agent.get("account")
    if not isinstance(session_id, str) or not isinstance(account, str):
        return {}
    persisted_path = agent.get("telemetry_path")
    path = Path(persisted_path) if isinstance(persisted_path, str) else None
    if path is None or not path.is_file():
        home = _profile_home(account)
        if home is None:
            return {}
        session_root = home / "sessions"
        try:
            candidates = list(session_root.rglob(f"*{session_id}.jsonl"))
        except OSError:
            return {}
        if not candidates:
            return {}
        try:
            path = max(candidates, key=lambda candidate: candidate.stat().st_mtime)
        except OSError:
            return {}
    try:
        file_stat = path.stat()
    except OSError:
        return {}
    file_identity = f"{file_stat.st_dev}:{file_stat.st_ino}"
    continuing = (
        agent.get("telemetry_path") == str(path)
        and agent.get("telemetry_file_identity") == file_identity
        and isinstance(agent.get("telemetry_offset"), int)
        and not isinstance(agent.get("telemetry_offset"), bool)
        and 0 <= int(agent["telemetry_offset"]) <= file_stat.st_size
    )
    offset = int(agent.get("telemetry_offset", 0)) if continuing else 0
    total_tokens: int | None = (
        int(agent["tokens"])
        if continuing
        and agent.get("tokens_known") is True
        and isinstance(agent.get("tokens"), int)
        and not isinstance(agent.get("tokens"), bool)
        else None
    )
    latest_message_at = agent.get("latest_message_at") if continuing else None
    turn_active = agent.get("turn_active") if continuing else None
    turn_state_at = agent.get("turn_state_at") if continuing else None
    turn_started_at = agent.get("turn_started_at") if continuing else None
    committed_offset = offset
    try:
        with path.open("rb") as handle:
            handle.seek(offset)
            while True:
                line_start = handle.tell()
                raw_line = handle.readline()
                if not raw_line:
                    break
                # Do not consume an event while the CLI is still appending it.
                if not raw_line.endswith(b"\n"):
                    handle.seek(line_start)
                    break
                committed_offset = handle.tell()
                try:
                    event = json.loads(raw_line.decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError):
                    continue
                payload = event.get("payload")
                if not isinstance(payload, dict):
                    continue
                if event.get("type") == "event_msg" and payload.get("type") == "token_count":
                    info = payload.get("info")
                    usage = info.get("total_token_usage") if isinstance(info, dict) else None
                    value = usage.get("total_tokens") if isinstance(usage, dict) else None
                    if isinstance(value, int) and not isinstance(value, bool) and value >= 0:
                        total_tokens = value
                if event.get("type") == "event_msg" and payload.get("type") in {
                    "task_started", "turn_started",
                }:
                    turn_active = True
                    if isinstance(event.get("timestamp"), str):
                        turn_state_at = event["timestamp"]
                        turn_started_at = event["timestamp"]
                if event.get("type") == "event_msg" and payload.get("type") in {
                    "task_complete", "turn_complete", "turn_aborted", "task_cancelled",
                }:
                    turn_active = False
                    if isinstance(event.get("timestamp"), str):
                        turn_state_at = event["timestamp"]
                if (
                    event.get("type") == "response_item"
                    and (
                        payload.get("type") == "agent_message"
                        or (
                            payload.get("type") == "message"
                            and payload.get("role") == "assistant"
                        )
                    )
                    and isinstance(event.get("timestamp"), str)
                ):
                    latest_message_at = event["timestamp"]
                if (
                    event.get("type") == "event_msg"
                    and payload.get("type") in {"agent_message", "assistant_message"}
                    and isinstance(event.get("timestamp"), str)
                ):
                    latest_message_at = event["timestamp"]
    except OSError:
        return {}
    return {
        "tokens": total_tokens,
        "tokens_known": total_tokens is not None,
        "latest_message_at": latest_message_at,
        "turn_active": turn_active,
        "turn_state_at": turn_state_at,
        "turn_started_at": turn_started_at,
        "telemetry_path": str(path),
        "telemetry_file_identity": file_identity,
        "telemetry_offset": committed_offset,
    }


def _prompt_prefix(
    agent_id: int,
    tier: str,
    highest_spawn_tier: str,
    bus_count: int,
) -> str:
    allowed = [name for name in reversed(TIER_ORDER) if TIER_RANK[name] <= TIER_RANK[highest_spawn_tier]]
    base = (
        f"You are bioseq subagent {agent_id}, tier {tier}. The shared bus has {bus_count} messages; "
        "use bioseq-build subagent bus-read to read it. "
        f"You may create/control these tiers: {', '.join(allowed)}. "
    )
    if tier in {"specialist", "senior"}:
        policy = (
            "Delegate justified lower-tier slices only; do the majority yourself and never wait passively for delegates."
        )
    else:
        policy = (
            "Spawn help only in critical situations; excessive delegation is penalized, so do the work yourself."
        )
    return base + policy + "\n\n"


def _live_budget(state: Mapping[str, Any]) -> int:
    return sum(
        int(agent.get("cost", 0))
        for agent in state["agents"].values()
        if isinstance(agent, dict) and agent.get("status") in LIVE_STATES
    )


def _select_account(state: Mapping[str, Any], provider: str) -> str:
    if provider == "cursor":
        return "cursor"
    accounts = state["accounts"]
    candidates: list[tuple[int, float, str]] = []
    for name, account in accounts.items():
        if (
            not isinstance(account, dict)
            or account.get("provider") != "codex"
            or account.get("available") is not True
        ):
            continue
        remaining = account.get("remaining_percent")
        sampled_at = account.get("quota_sampled_at")
        if (
            not isinstance(remaining, (int, float))
            or not isinstance(sampled_at, (int, float))
            or time.time() - float(sampled_at) >= QUOTA_REFRESH_SECONDS
            or account.get("quota_error") is not None
            or float(remaining) < QUOTA_CREATE_FLOOR_PERCENT
        ):
            continue
        live_count = sum(
            1 for agent in state["agents"].values()
            if isinstance(agent, dict)
            and agent.get("account") == name
            and agent.get("status") in LIVE_STATES
        )
        candidates.append((live_count, -float(remaining), name))
    if not candidates:
        facts = {
            name: account.get("remaining_percent")
            for name, account in accounts.items()
            if isinstance(account, dict) and account.get("provider") == "codex"
        }
        raise SubagentError(
            "NO_ACCOUNT_QUOTA",
            "No Codex account has a fresh weekly quota of at least 3%; existing agents may continue, but create is blocked for safety.",
            details=facts,
        )
    return min(candidates)[2]


def _iso_epoch(value: object) -> float | None:
    if not isinstance(value, str):
        return None
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()
    except ValueError:
        return None


def _age(now: float, value: object) -> float | None:
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return max(0.0, now - float(value))
    parsed = _iso_epoch(value)
    return None if parsed is None else max(0.0, now - parsed)


def _pane_matches_vendor(agent: Mapping[str, Any], command: object) -> bool:
    if not isinstance(command, str) or not command:
        return False
    basename = Path(command).name
    if agent.get("provider") == "cursor":
        return basename == "agent"
    return basename in {"codex", "codex-profile"}


def _commit_pending_metadata(agent: dict[str, Any]) -> None:
    if isinstance(agent.get("pending_description"), str):
        agent["description"] = agent["pending_description"]
    if agent.get("pending_label") in LABELS:
        agent["label"] = agent["pending_label"]
    if agent.get("pending_highest_spawn_tier") in TIER_CONFIG:
        agent["highest_spawn_tier"] = agent["pending_highest_spawn_tier"]


def _clear_pending_metadata(agent: dict[str, Any]) -> None:
    agent["pending_description"] = None
    agent["pending_label"] = None
    agent["pending_highest_spawn_tier"] = None


def _reconcile_agents() -> None:
    with _registry_lock():
        state = _load_state_locked()
        snapshots = [
            dict(agent)
            for agent in state["agents"].values()
            if isinstance(agent, dict) and agent.get("id") != 0
        ]
    observations: dict[int, dict[str, Any]] = {}
    for agent in snapshots:
        agent_id = int(agent["id"])
        observation: dict[str, Any] = {"generation": agent.get("generation")}
        session_name = agent.get("tmux_session")
        target = agent.get("tmux_pane") or session_name
        session_present = (
            _tmux_session_observation(str(session_name))
            if isinstance(target, str) and isinstance(session_name, str)
            else False
        )
        if session_present is False and _identity_is_live(agent.get("runtime_identity")):
            session_present = None
        pane = _tmux_pane_info(str(target)) if session_present is True else None
        marker_mismatch = (
            pane is not None
            and (
                pane.get("repository_marker") != _REPOSITORY_KEY
                or pane.get("agent_marker") != str(agent_id)
            )
        )
        observation["ownership_mismatch"] = marker_mismatch
        if (
            session_present is None
            or (session_present is True and pane is None)
            or marker_mismatch
        ):
            observation["runtime_present"] = None
        else:
            observation["runtime_present"] = (
                pane is not None and not pane.get("pane_dead", True)
            )
        if pane is not None and not marker_mismatch:
            observed_identity = (
                _identity_dict(_capture_process_identity(pane["pane_pid"]))
                if isinstance(pane.get("pane_pid"), int) else None
            )
            observation.update({
                "tmux_pane": pane.get("pane_target"),
                "pane_pid": pane.get("pane_pid"),
                "pane_dead_status": pane.get("pane_dead_status"),
                "pane_command": pane.get("pane_command"),
                "runtime_identity": observed_identity,
            })
            if isinstance(observed_identity, dict):
                operation = agent.get("operation_state")
                previous_identity = (
                    agent.get("operation_previous_runtime_identity")
                    if operation in PENDING_OPERATION_STATES
                    and "operation_previous_runtime_identity" in agent
                    else agent.get("runtime_identity")
                )
                observation["runtime_replaced"] = (
                    not _same_identity_dict(previous_identity, observed_identity)
                    if isinstance(previous_identity, dict)
                    else True
                    if operation in PENDING_OPERATION_STATES
                    and (
                        agent.get("operation_previous_status") == "KILLED"
                        and _pane_matches_vendor(agent, pane.get("pane_command"))
                        or agent.get("operation_phase") in OPERATION_PHASE_RANK
                        and OPERATION_PHASE_RANK[str(agent["operation_phase"])]
                        >= OPERATION_PHASE_RANK["VENDOR_STARTED"]
                    )
                    else None
                )
        session_id = agent.get("vendor_session_id") or _parse_vendor_session_from_log(agent)
        if isinstance(session_id, str) and session_id:
            observation["vendor_session_id"] = session_id
            agent["vendor_session_id"] = session_id
        log_path = Path(str(agent.get("log_file", "")))
        with contextlib.suppress(OSError):
            observation["latest_log_at"] = log_path.stat().st_mtime
        if agent.get("provider") == "codex":
            observation.update(_read_codex_telemetry(agent))
        elif agent.get("provider") == "cursor":
            aggregate_snapshot = _cursor_log_snapshot(
                agent, maximum_bytes=64 * 1024 * 1024,
            )
            runtime_log_file = agent.get("runtime_log_file")
            cursor_runtime_agent = dict(agent)
            if isinstance(runtime_log_file, str) and runtime_log_file:
                cursor_runtime_agent["log_file"] = runtime_log_file
                cursor_snapshot = _cursor_log_snapshot(
                    cursor_runtime_agent, maximum_bytes=64 * 1024 * 1024,
                )
            else:
                cursor_snapshot = aggregate_snapshot
            for source_key, destination_key in (
                ("latest_message_at", "latest_message_at"),
                ("terminal_result", "cursor_terminal_result"),
                ("result_error", "cursor_result_error"),
                ("result_diagnostic", "cursor_result_diagnostic"),
            ):
                if cursor_snapshot.get(source_key) is not None:
                    observation[destination_key] = cursor_snapshot[source_key]
            usage_tokens = aggregate_snapshot.get("usage_tokens")
            if (
                aggregate_snapshot.get("scan_complete") is True
                and isinstance(usage_tokens, int)
                and not isinstance(usage_tokens, bool)
            ):
                observation["tokens"] = usage_tokens
                observation["tokens_known"] = True
            operation_log_start = agent.get("operation_cursor_log_start")
            operation_log_file = agent.get("operation_runtime_log_file")
            if (
                agent.get("operation_state") in PENDING_OPERATION_STATES
                and isinstance(operation_log_start, int)
                and not isinstance(operation_log_start, bool)
                and operation_log_start >= 0
            ):
                operation_agent = dict(agent)
                if isinstance(operation_log_file, str) and operation_log_file:
                    operation_agent["log_file"] = operation_log_file
                    observation["runtime_log_file"] = operation_log_file
                operation_snapshot = _cursor_log_snapshot(
                    operation_agent,
                    start_offset=operation_log_start,
                    maximum_bytes=64 * 1024 * 1024,
                )
                operation_session_id = operation_snapshot.get("session_id")
                expected_session_id = agent.get("operation_expected_session_id")
                exact_session = (
                    operation_snapshot.get("initialized") is True
                    and isinstance(operation_session_id, str)
                    and (
                        expected_session_id is None
                        or operation_session_id == expected_session_id
                    )
                )
                observation.update({
                    "cursor_assignment_confirmed": exact_session,
                    "cursor_operation_session_id": operation_session_id,
                    "cursor_context_mismatch": (
                        operation_snapshot.get("initialized") is True
                        and isinstance(expected_session_id, str)
                        and operation_session_id != expected_session_id
                    ),
                    "cursor_terminal_result": operation_snapshot.get("terminal_result"),
                    "cursor_result_error": operation_snapshot.get("result_error"),
                    "cursor_result_diagnostic": operation_snapshot.get("result_diagnostic"),
                })
                if operation_snapshot.get("latest_message_at") is not None:
                    observation["latest_message_at"] = operation_snapshot[
                        "latest_message_at"
                    ]
        observations[agent_id] = observation

    if not observations:
        return
    now = time.time()
    with _registry_lock():
        state = _load_state_locked()
        changed = False
        for agent_id, observation in observations.items():
            agent = state["agents"].get(str(agent_id))
            if not isinstance(agent, dict):
                continue
            if agent.get("generation") != observation.get("generation"):
                continue
            for key in (
                "tmux_pane", "pane_pid", "runtime_identity", "vendor_session_id",
                "runtime_log_file",
                "latest_log_at", "latest_message_at", "tokens", "tokens_known",
                "turn_active", "turn_state_at", "turn_started_at",
                "pane_dead_status", "pane_command",
                "telemetry_path", "telemetry_file_identity", "telemetry_offset",
            ):
                if key in observation and observation[key] is not None:
                    if agent.get(key) != observation[key]:
                        agent[key] = observation[key]
                        changed = True
            operation = agent.get("operation_state")
            operation_started_at = agent.get("operation_started_at")
            operation_age = _age(now, operation_started_at)
            stale_operation = (
                operation in PENDING_OPERATION_STATES
                and operation_age is not None
                and operation_age >= OPERATION_LEASE_SECONDS
            )
            if stale_operation:
                runtime_present = observation.get("runtime_present")
                phase = agent.get("operation_phase")
                previous_status = agent.get("operation_previous_status")
                runtime_replaced = observation.get("runtime_replaced")
                instruction_at = agent.get("operation_instruction_sent_at")
                turn_started_at = _iso_epoch(observation.get("turn_started_at"))
                turn_state_at = _iso_epoch(observation.get("turn_state_at"))
                codex_assignment_confirmed = (
                    agent.get("provider") == "codex"
                    and isinstance(instruction_at, (int, float))
                    and turn_started_at is not None
                    and turn_started_at >= float(instruction_at)
                )
                cursor_assignment_confirmed = (
                    agent.get("provider") == "cursor"
                    and observation.get("cursor_assignment_confirmed") is True
                    and observation.get("cursor_context_mismatch") is not True
                )
                assignment_confirmed = (
                    phase == "ASSIGNMENT_ACKNOWLEDGED"
                    or codex_assignment_confirmed
                    or cursor_assignment_confirmed
                )
                assignment_completed = (
                    assignment_confirmed
                    and (
                        agent.get("provider") == "cursor"
                        and observation.get("cursor_terminal_result") is True
                        or agent.get("provider") == "codex"
                        and observation.get("turn_active") is False
                        and turn_state_at is not None
                        and isinstance(instruction_at, (int, float))
                        and turn_state_at >= float(instruction_at)
                    )
                )
                cursor_error = (
                    agent.get("provider") == "cursor"
                    and observation.get("cursor_result_error") is True
                )
                cursor_idle = (
                    agent.get("provider") == "cursor"
                    and Path(str(observation.get("pane_command", ""))).name == "tail"
                )

                if runtime_present is None:
                    if (
                        operation == "STARTING"
                        and observation.get("ownership_mismatch") is True
                        and phase in {None, "RESERVED"}
                    ):
                        # No ownership marker means no vendor capability was
                        # established.  Never touch the colliding tmux session.
                        agent["status"] = "KILLED"
                        agent["operation_state"] = "RECOVERED_BEFORE_SESSION_OWNERSHIP"
                        agent["capability_sha256"] = None
                        agent["pending_capability_sha256"] = None
                        _clear_pending_metadata(agent)
                        _clear_operation_transaction(agent)
                        agent["generation"] = int(agent.get("generation", 0)) + 1
                        agent["state_changed_at"] = now
                        agent["last_error"] = (
                            "Launch ownership was never established; an unowned tmux session was left untouched."
                        )
                    else:
                        # UNKNOWN is not absence.  Keep the public state live,
                        # retain both capabilities, and preserve the CAS lease.
                        if agent.get("status") not in LIVE_STATES:
                            agent["status"] = "WORKING"
                        agent["last_error"] = (
                            f"Runtime evidence is indeterminate while recovering stale {operation}; budget and authorization evidence were retained."
                        )
                        agent["operation_cleanup_required"] = True
                        agent["operation_started_at"] = now
                    changed = True
                    continue

                if runtime_present is True:
                    # Fundamental invariant: a positively observed owned live
                    # process is never demoted to KILLED and never releases its
                    # budget merely because identity/telemetry is incomplete.
                    if operation == "KILLING":
                        previous_cleanup_required = (
                            agent.get("operation_previous_cleanup_required") is True
                        )
                        agent["status"] = (
                            str(previous_status)
                            if previous_status in LIVE_STATES else "WORKING"
                        )
                        agent["operation_state"] = "RECOVERED_KILL_NOT_APPLIED"
                        _clear_operation_transaction(agent)
                        agent["operation_cleanup_required"] = previous_cleanup_required
                    elif cursor_idle:
                        agent["status"] = "INTERRUPTED"
                        agent["operation_state"] = "RECOVERED_INTERRUPTED_RUNTIME"
                        agent["capability_sha256"] = None
                        agent["pending_capability_sha256"] = None
                        _clear_pending_metadata(agent)
                        _clear_operation_transaction(agent)
                    elif not _pane_matches_vendor(
                        agent, observation.get("pane_command"),
                    ):
                        # A crash before respawn leaves only the owned shell.
                        # Check this before INSTRUCTION_SENDING because Cursor
                        # journals that phase before its argv-bearing respawn.
                        agent["status"] = "INTERRUPTED"
                        agent["operation_state"] = "RECOVERED_SESSION_ONLY"
                        agent["capability_sha256"] = None
                        agent["pending_capability_sha256"] = None
                        _clear_pending_metadata(agent)
                        _clear_operation_transaction(agent)
                    elif observation.get("cursor_context_mismatch") is True:
                        if agent.get("status") not in LIVE_STATES:
                            agent["status"] = "WORKING"
                        agent["last_error"] = (
                            "A live Cursor replacement reported a different chat ID; transaction evidence and budget were retained for explicit cleanup."
                        )
                        agent["operation_cleanup_required"] = True
                        agent["operation_started_at"] = now
                        changed = True
                        continue
                    elif assignment_confirmed and not cursor_error:
                        agent["status"] = (
                            "INTERRUPTED" if assignment_completed else "WORKING"
                        )
                        agent["operation_state"] = (
                            "RECOVERED_TURN_COMPLETED"
                            if assignment_completed else "RECOVERED_RUNNING"
                        )
                        if isinstance(agent.get("pending_capability_sha256"), str):
                            agent["capability_sha256"] = agent[
                                "pending_capability_sha256"
                            ]
                        agent["pending_capability_sha256"] = None
                        if operation in {"REPURPOSING", "RESUMING"}:
                            _commit_pending_metadata(agent)
                        _clear_pending_metadata(agent)
                        _clear_operation_transaction(agent)
                    elif cursor_error:
                        # A terminal error was observed, but the process is
                        # still live.  Retain budget and capability evidence so
                        # ROOT can kill it deterministically.
                        if agent.get("status") not in LIVE_STATES:
                            agent["status"] = "WORKING"
                        agent["operation_state"] = "CURSOR_RESULT_ERROR_LIVE"
                        agent["last_error"] = (
                            observation.get("cursor_result_diagnostic")
                            or "Cursor reported a terminal assignment error."
                        )
                        agent["operation_cleanup_required"] = True
                        agent["operation_started_at"] = now
                        changed = True
                        continue
                    elif phase in {"INSTRUCTION_SENDING", "INSTRUCTION_SENT"}:
                        if agent.get("status") not in LIVE_STATES:
                            agent["status"] = "WORKING"
                        agent["last_error"] = (
                            "Instruction delivery may have occurred, but no assignment acknowledgement is observable; the transaction was retained without replay."
                        )
                        agent["operation_cleanup_required"] = True
                        agent["operation_started_at"] = now
                        changed = True
                        continue
                    elif _pane_matches_vendor(agent, observation.get("pane_command")):
                        if (
                            isinstance(agent.get("pending_capability_sha256"), str)
                            and runtime_replaced is True
                        ):
                            agent["capability_sha256"] = agent[
                                "pending_capability_sha256"
                            ]
                            agent["pending_capability_sha256"] = None
                        elif (
                            isinstance(agent.get("pending_capability_sha256"), str)
                            and runtime_replaced is not True
                        ):
                            agent["status"] = "WORKING"
                            agent["last_error"] = (
                                "A live vendor process is present but its capability generation is indeterminate; transaction evidence was retained."
                            )
                            agent["operation_cleanup_required"] = True
                            agent["operation_started_at"] = now
                            changed = True
                            continue
                        agent["status"] = "INTERRUPTED"
                        agent["operation_state"] = "RECOVERED_VENDOR_IDLE"
                        _clear_pending_metadata(agent)
                        _clear_operation_transaction(agent)
                    agent["generation"] = int(agent.get("generation", 0)) + 1
                    agent["state_changed_at"] = now
                    if agent.get("last_error") is None:
                        agent["last_error"] = (
                            f"Recovered a live runtime after stale {operation} transaction metadata."
                        )
                    changed = True
                    continue
            if (
                agent.get("status") in LIVE_STATES
                and observation.get("runtime_present") is False
            ):
                if (
                    operation in PENDING_OPERATION_STATES
                    and operation_age is not None
                    and operation_age < OPERATION_LEASE_SECONDS
                ):
                    continue
                agent["status"] = "KILLED"
                normal_exit = observation.get("pane_dead_status") == 0
                cursor_confirmed = (
                    agent.get("provider") == "cursor"
                    and observation.get("cursor_assignment_confirmed") is True
                    and observation.get("cursor_context_mismatch") is not True
                )
                codex_instruction_at = agent.get("operation_instruction_sent_at")
                codex_started_at = _iso_epoch(observation.get("turn_started_at"))
                codex_confirmed = (
                    agent.get("provider") == "codex"
                    and operation in PENDING_OPERATION_STATES
                    and (
                        agent.get("operation_phase") == "ASSIGNMENT_ACKNOWLEDGED"
                        and isinstance(
                            agent.get("operation_assignment_acknowledged_at"),
                            (int, float),
                        )
                        or isinstance(codex_instruction_at, (int, float))
                        and codex_started_at is not None
                        and codex_started_at >= float(codex_instruction_at)
                    )
                )
                cursor_error = (
                    agent.get("provider") == "cursor"
                    and observation.get("cursor_result_error") is True
                )
                if (
                    (cursor_confirmed and not cursor_error or codex_confirmed)
                    and operation in {
                    "REPURPOSING", "RESUMING",
                    }
                ):
                    _commit_pending_metadata(agent)
                if operation == "KILLING":
                    terminal_operation = "RECOVERED_REQUESTED_KILL"
                elif observation.get("cursor_context_mismatch") is True:
                    terminal_operation = "RESUME_CONTEXT_MISMATCH"
                elif cursor_error:
                    terminal_operation = "CURSOR_RESULT_ERROR"
                elif cursor_confirmed and observation.get("cursor_terminal_result") is True:
                    terminal_operation = "TURN_COMPLETED"
                elif codex_confirmed and observation.get("turn_active") is False:
                    terminal_operation = "RECOVERED_TURN_COMPLETED"
                elif operation in {"INTERRUPTING", "REPURPOSING", "RESUMING"}:
                    terminal_operation = "RECOVERED_OPERATION_STOPPED"
                elif operation == "STARTING":
                    terminal_operation = "RECOVERED_LAUNCH_EXIT"
                else:
                    terminal_operation = (
                        "TURN_COMPLETED" if normal_exit else "UNEXPECTED_EXIT"
                    )
                agent["operation_state"] = terminal_operation
                agent["state_changed_at"] = now
                agent["capability_sha256"] = None
                agent["pending_capability_sha256"] = None
                _clear_pending_metadata(agent)
                _clear_operation_transaction(agent)
                agent["last_error"] = (
                    observation.get("cursor_result_diagnostic")
                    if cursor_error else
                    None if normal_exit and (
                        operation not in PENDING_OPERATION_STATES or cursor_confirmed
                    ) else
                    f"Recovered an absent runtime after stale {operation} transaction metadata."
                    if operation in PENDING_OPERATION_STATES else
                    "The persisted tmux agent process is absent or exited; context may still be resumable."
                )
                changed = True
            elif (
                agent.get("provider") == "cursor"
                and observation.get("cursor_terminal_result") is True
                and agent.get("status") == "WORKING"
            ):
                agent["status"] = "INTERRUPTED"
                if observation.get("cursor_result_error") is True:
                    agent["operation_state"] = "CURSOR_RESULT_ERROR_LIVE"
                    agent["operation_cleanup_required"] = True
                    agent["last_error"] = (
                        observation.get("cursor_result_diagnostic")
                        or "Cursor reported a terminal assignment error."
                    )
                else:
                    agent["operation_state"] = "TURN_COMPLETED"
                    agent["last_error"] = None
                agent["state_changed_at"] = now
                changed = True
            elif (
                agent.get("provider") == "codex"
                and observation.get("turn_active") in {True, False}
            ):
                state_at = _iso_epoch(observation.get("turn_state_at"))
                instructed_at = agent.get("last_instruction_at")
                if (
                    state_at is not None
                    and isinstance(instructed_at, (int, float))
                    and state_at >= float(instructed_at)
                ):
                    if (
                        observation.get("turn_active") is False
                        and agent.get("status") == "WORKING"
                    ):
                        agent["status"] = "INTERRUPTED"
                        agent["operation_state"] = "TURN_COMPLETED"
                        agent["state_changed_at"] = now
                        agent["last_error"] = None
                        changed = True
                    elif (
                        observation.get("turn_active") is True
                        and agent.get("status") == "INTERRUPTED"
                        and (
                            not isinstance(agent.get("last_interrupted_at"), (int, float))
                            or state_at >= float(agent["last_interrupted_at"])
                        )
                    ):
                        agent["status"] = "WORKING"
                        agent["operation_state"] = "RUNNING"
                        agent["state_changed_at"] = now
                        agent["last_error"] = None
                        changed = True
        if changed:
            _save_state_locked(state)


def _quota_trend(account: Mapping[str, Any]) -> dict[str, Any]:
    history = account.get("quota_history")
    if not isinstance(history, list) or len(history) < 6:
        return {
            "sample_count": len(history) if isinstance(history, list) else 0,
            "average_used_percent_per_hour": None,
            "estimated_exhaustion_at": None,
            "reason": "at least six valid five-minute samples are required",
        }
    valid = [
        sample for sample in history
        if isinstance(sample, dict)
        and isinstance(sample.get("sampled_at"), (int, float))
        and isinstance(sample.get("remaining_percent"), (int, float))
    ]
    if len(valid) < 6:
        return {
            "sample_count": len(valid),
            "average_used_percent_per_hour": None,
            "estimated_exhaustion_at": None,
            "reason": "fewer than six valid samples remain",
        }
    elapsed = float(valid[-1]["sampled_at"]) - float(valid[0]["sampled_at"])
    consumed = sum(
        max(0.0, float(previous["remaining_percent"]) - float(current["remaining_percent"]))
        for previous, current in zip(valid, valid[1:])
    )
    if elapsed <= 0 or consumed <= 0:
        return {
            "sample_count": len(valid),
            "average_used_percent_per_hour": 0.0,
            "estimated_exhaustion_at": None,
            "reason": "no positive usage slope is observable",
        }
    rate = consumed / (elapsed / 3600.0)
    remaining = float(valid[-1]["remaining_percent"])
    eta = float(valid[-1]["sampled_at"]) + (remaining / rate) * 3600.0
    return {
        "sample_count": len(valid),
        "average_used_percent_per_hour": rate,
        "estimated_exhaustion_at": eta,
        "reason": None,
    }


def _descendants(
    root_id: int,
    children: Mapping[int, list[int]],
) -> list[int]:
    result: list[int] = []
    stack = list(reversed(children.get(root_id, [])))
    seen = {root_id}
    while stack:
        child = stack.pop()
        if child in seen:
            raise SubagentError("STATE_CORRUPT", "agent parent graph contains a cycle")
        seen.add(child)
        result.append(child)
        stack.extend(reversed(children.get(child, [])))
    return result


def _token_metric(agent_ids: list[int], by_id: Mapping[int, Mapping[str, Any]]):
    total = 0
    complete = True
    unknown_agent_ids: list[int] = []
    for agent_id in agent_ids:
        agent = by_id[agent_id]
        value = agent.get("tokens")
        if agent.get("tokens_known") is True and isinstance(value, int):
            total += value
        else:
            complete = False
            unknown_agent_ids.append(agent_id)
    return {
        "known_total": total,
        "complete": complete,
        "unknown_agent_ids": unknown_agent_ids,
    }


def _status_payload(state: Mapping[str, Any], caller_id: int) -> tuple[dict[str, Any], list[str]]:
    now = time.time()
    by_id: dict[int, Mapping[str, Any]] = {
        int(agent_id): agent
        for agent_id, agent in state["agents"].items()
        if isinstance(agent, dict)
    }
    children: dict[int, list[int]] = {agent_id: [] for agent_id in by_id}
    for agent_id, agent in by_id.items():
        if agent_id == 0:
            continue
        parent_id = agent.get("parent_id")
        if not isinstance(parent_id, int) or parent_id not in by_id or parent_id >= agent_id:
            raise SubagentError(
                "STATE_CORRUPT", f"agent {agent_id} has an invalid persisted parent",
            )
        children.setdefault(parent_id, []).append(agent_id)
    for values in children.values():
        values.sort()

    warnings: list[str] = []
    projected_agents: list[dict[str, Any]] = []
    for agent_id in sorted(by_id):
        agent = by_id[agent_id]
        stored_ceiling = agent.get("highest_spawn_tier")
        if agent_id != 0 and stored_ceiling not in TIER_CONFIG:
            raise SubagentError(
                "STATE_CORRUPT", f"agent {agent_id} has an invalid authority ceiling",
            )
        direct = children.get(agent_id, [])
        recursive = _descendants(agent_id, children)
        interruptions = int(agent.get("interruption_count", 0))
        if interruptions > 10:
            warnings.append(
                f"Agent {agent_id} has been interrupted {interruptions} times. Each resume can resend its growing context; reduce interruptions to avoid escalating token cost."
            )
        projected_agents.append({
            "id": agent_id,
            "parent_id": agent.get("parent_id"),
            "account": agent.get("account"),
            "provider": agent.get("provider"),
            "reasoning_effort": agent.get("tier"),
            "model": agent.get("model"),
            "vendor_reasoning": agent.get("reasoning"),
            "cost": agent.get("cost"),
            "label": agent.get("label"),
            "description": agent.get("description"),
            "highest_spawn_tier": agent.get("highest_spawn_tier"),
            "allowed_tiers": (
                [
                    tier for tier in reversed(TIER_ORDER)
                    if TIER_RANK[tier] <= TIER_RANK[str(stored_ceiling)]
                ]
                if agent_id != 0 else list(reversed(TIER_ORDER))
            ),
            "status": agent.get("status"),
            "operation_state": agent.get("operation_state"),
            "cleanup_required": agent.get("operation_cleanup_required") is True,
            "last_error": agent.get("last_error"),
            "direct_subagents_ever": len(direct),
            "recursive_subagents_ever": len(recursive),
            "direct_subagents_active": sum(
                1 for child in direct if by_id[child].get("status") in LIVE_STATES
            ),
            "recursive_subagents_active": sum(
                1 for child in recursive if by_id[child].get("status") in LIVE_STATES
            ),
            "tokens_self": _token_metric([agent_id], by_id),
            "tokens_self_plus_direct_children": _token_metric([agent_id, *direct], by_id),
            "tokens_recursive_subtree": _token_metric([agent_id, *recursive], by_id),
            "interruption_count": interruptions,
            "created_at": agent.get("created_at"),
            "state_changed_at": agent.get("state_changed_at"),
            "last_interrupted_at": agent.get("last_interrupted_at"),
            "latest_log_at": agent.get("latest_log_at"),
            "latest_message_at": agent.get("latest_message_at"),
            "time_since_state_change_seconds": _age(now, agent.get("state_changed_at")),
            "time_since_last_interruption_seconds": _age(now, agent.get("last_interrupted_at")),
            "time_since_latest_log_seconds": _age(now, agent.get("latest_log_at")),
            "time_since_latest_message_seconds": _age(now, agent.get("latest_message_at")),
        })

    projected_accounts: list[dict[str, Any]] = []
    exhaustion_times: list[float] = []
    all_codex_trends_known = True
    for name in sorted(state["accounts"]):
        account = state["accounts"][name]
        if not isinstance(account, dict):
            continue
        assigned = [
            agent for agent in by_id.values()
            if agent.get("account") == name and agent.get("id") != 0
        ]
        remaining = account.get("remaining_percent")
        trend = _quota_trend(account) if account.get("provider") == "codex" else {
            "sample_count": 0,
            "average_used_percent_per_hour": None,
            "estimated_exhaustion_at": None,
            "reason": "provider exposes no weekly quota endpoint",
        }
        if account.get("provider") == "codex" and account.get("available") is True:
            sampled_at = account.get("quota_sampled_at")
            healthy_sample = (
                account.get("quota_error") is None
                and isinstance(sampled_at, (int, float))
                and now - float(sampled_at) < QUOTA_REFRESH_SECONDS
            )
            eta = trend.get("estimated_exhaustion_at")
            if healthy_sample and isinstance(eta, (int, float)):
                exhaustion_times.append(float(eta))
            else:
                all_codex_trends_known = False
        if isinstance(remaining, (int, float)) and float(remaining) < QUOTA_CREATE_FLOOR_PERCENT:
            warnings.append(
                f"Account {name} has {float(remaining):.1f}% weekly quota left. Do not create there and avoid interrupting its existing agents; at 0% an in-flight turn may continue but a resume can fail."
            )
        projected_accounts.append({
            "name": name,
            "provider": account.get("provider"),
            "available": account.get("available", account.get("provider") == "cursor"),
            "weekly_remaining_percent": remaining,
            "weekly_used_percent": account.get("used_percent"),
            "quota_source": account.get("source"),
            "quota_limit_id": account.get("limit_id"),
            "quota_window_minutes": account.get("window_minutes"),
            "plan_type": account.get("plan_type"),
            "quota_sampled_at": account.get("quota_sampled_at"),
            "quota_attempted_at": account.get("quota_attempted_at"),
            "quota_age_seconds": _age(now, account.get("quota_sampled_at")),
            "quota_stale": (
                not isinstance(account.get("quota_sampled_at"), (int, float))
                or now - float(account["quota_sampled_at"]) >= QUOTA_REFRESH_SECONDS * 2
            ),
            "quota_error": account.get("quota_error"),
            "resets_at": account.get("resets_at"),
            "trend": trend,
            "agent_ids": sorted(int(agent["id"]) for agent in assigned),
            "live_agent_ids": sorted(
                int(agent["id"]) for agent in assigned
                if agent.get("status") in LIVE_STATES
            ),
            "live_agent_count": sum(1 for agent in assigned if agent.get("status") in LIVE_STATES),
            "live_budget_cost": sum(
                int(agent.get("cost", 0)) for agent in assigned
                if agent.get("status") in LIVE_STATES
            ),
        })
    all_exhausted_at = (
        max(exhaustion_times)
        if exhaustion_times and all_codex_trends_known else None
    )
    return ({
        "caller_id": caller_id,
        "budget": {
            "used": _live_budget(state),
            "limit": SUBAGENT_BUDGET,
            "remaining": SUBAGENT_BUDGET - _live_budget(state),
            "note": "The 1300 limit is fixed safety policy and must not be altered to force allocation.",
        },
        "accounts": projected_accounts,
        "agents": projected_agents,
        "quota_estimate": {
            "all_codex_accounts_exhausted_at": all_exhausted_at,
            "method": "maximum per-account linear ETA; unavailable until every currently available Codex account has a fresh successful sample plus at least six valid samples and positive usage",
        },
    }, warnings)


def _target_id(arguments: Mapping[str, object]) -> int:
    if "agent_id" not in arguments:
        return 0
    return _require_integer(arguments, "agent_id", minimum=0)


def _agent_locked(state: Mapping[str, Any], agent_id: int) -> dict[str, Any]:
    agent = state["agents"].get(str(agent_id))
    if not isinstance(agent, dict):
        raise SubagentError("AGENT_NOT_FOUND", f"subagent {agent_id} does not exist")
    return agent


def _mutable_agent_locked(
    state: Mapping[str, Any], caller_id: int, agent_id: int,
    *,
    allow_cleanup: bool = False,
) -> dict[str, Any]:
    if agent_id == 0:
        raise SubagentError(
            "ROOT_NOT_CONTROLLABLE",
            "agent_id defaults to 0, but ROOT cannot be interrupted, killed, or repurposed",
        )
    agent = _agent_locked(state, agent_id)
    tier = _validate_tier(agent.get("tier"), "stored agent tier")
    delegated_ceiling = _validate_tier(
        agent.get("highest_spawn_tier"), "stored highest_spawn_tier",
    )
    if caller_id != agent_id:
        _require_control_authority(state, caller_id, tier)
        _require_control_authority(state, caller_id, delegated_ceiling)
    operation = agent.get("operation_state")
    cleanup_required = agent.get("operation_cleanup_required") is True
    if cleanup_required and not allow_cleanup:
        raise SubagentError(
            "AGENT_CLEANUP_REQUIRED",
            f"subagent {agent_id} has indeterminate lifecycle evidence; an authorized kill must clean up the owned runtime before interrupt or repurpose",
        )
    if operation in PENDING_OPERATION_STATES and not (
        allow_cleanup and cleanup_required
    ):
        raise SubagentError(
            "AGENT_BUSY",
            f"subagent {agent_id} already has lifecycle operation {operation} in progress",
        )
    return agent


def _operation_token(
    agent: dict[str, Any],
    operation_state: str,
    *,
    intended_status: str,
) -> tuple[int, str]:
    generation = int(agent.get("generation", 0)) + 1
    nonce = secrets.token_hex(16)
    agent["operation_previous_status"] = agent.get("status")
    # This identity is the immutable comparison baseline for the whole
    # transaction.  ``runtime_identity`` is refreshed by reconciliation and
    # therefore must never be used as the baseline for a later recovery pass.
    previous_identity = agent.get("runtime_identity")
    agent["operation_previous_runtime_identity"] = (
        dict(previous_identity) if isinstance(previous_identity, dict) else None
    )
    agent["operation_previous_cleanup_required"] = (
        agent.get("operation_cleanup_required") is True
    )
    agent["operation_intended_status"] = intended_status
    agent["generation"] = generation
    agent["operation_nonce"] = nonce
    agent["operation_state"] = operation_state
    agent["operation_phase"] = "RESERVED"
    agent["operation_instruction_sent_at"] = None
    agent["operation_cursor_log_start"] = None
    agent["operation_runtime_log_file"] = None
    agent["operation_expected_session_id"] = None
    agent["operation_assignment_acknowledged_at"] = None
    agent["operation_cleanup_required"] = False
    agent["operation_started_at"] = time.time()
    return generation, nonce


def _operation_agent_locked(
    state: Mapping[str, Any], agent_id: int, generation: int, nonce: str,
) -> dict[str, Any]:
    agent = _agent_locked(state, agent_id)
    if agent.get("generation") != generation or not secrets.compare_digest(
        str(agent.get("operation_nonce", "")), nonce,
    ):
        raise SubagentError(
            "OPERATION_SUPERSEDED",
            f"subagent {agent_id} changed while its external lifecycle operation was running",
        )
    return agent


def _record_operation_progress(
    operation_context: tuple[int, int, str] | None,
    phase: str,
    **updates: Any,
) -> None:
    """Durably journal a launch phase without changing its CAS generation."""
    if operation_context is None:
        return
    if phase not in OPERATION_PHASE_RANK:
        raise SubagentError("RUNTIME_ERROR", f"invalid lifecycle phase {phase}")
    agent_id, generation, nonce = operation_context
    with _registry_lock():
        state = _load_state_locked()
        agent = _operation_agent_locked(state, agent_id, generation, nonce)
        current_phase = agent.get("operation_phase")
        if (
            current_phase in OPERATION_PHASE_RANK
            and OPERATION_PHASE_RANK[phase] < OPERATION_PHASE_RANK[str(current_phase)]
        ):
            raise SubagentError(
                "RUNTIME_ERROR",
                f"lifecycle phase would move backwards from {current_phase} to {phase}",
            )
        agent["operation_phase"] = phase
        for key, value in updates.items():
            agent[key] = value
        _save_state_locked(state)


def _clear_operation_transaction(agent: dict[str, Any]) -> None:
    """Clear only crash-recovery transaction evidence after a proven outcome."""
    for key in (
        "operation_nonce",
        "operation_started_at",
        "operation_previous_status",
        "operation_previous_runtime_identity",
        "operation_previous_cleanup_required",
        "operation_intended_status",
        "operation_phase",
        "operation_instruction_sent_at",
        "operation_cursor_log_start",
        "operation_runtime_log_file",
        "operation_expected_session_id",
        "operation_assignment_acknowledged_at",
    ):
        agent[key] = None
    agent["operation_cleanup_required"] = False


def _bus_count_locked() -> int:
    return len(_load_bus_locked()["messages"])


def _account_resume_warning(state: Mapping[str, Any], agent: Mapping[str, Any]) -> list[str]:
    if agent.get("provider") != "codex":
        return [
            "Cursor exposes no weekly quota percentage; contractor-intern continuity is best-effort."
        ]
    account = state["accounts"].get(str(agent.get("account")))
    remaining = account.get("remaining_percent") if isinstance(account, dict) else None
    sampled_at = account.get("quota_sampled_at") if isinstance(account, dict) else None
    stale_or_unavailable = (
        not isinstance(account, dict)
        or account.get("available") is not True
        or account.get("quota_error") is not None
        or not isinstance(sampled_at, (int, float))
        or time.time() - float(sampled_at) >= QUOTA_REFRESH_SECONDS
    )
    if stale_or_unavailable:
        cached = (
            f"; the stale cached value is {float(remaining):.1f}%"
            if isinstance(remaining, (int, float)) else ""
        )
        return [
            f"Fresh weekly quota for account {agent.get('account')} is unavailable{cached}. Avoid interruption when possible: the current turn may continue while a subsequent request can fail."
        ]
    if float(remaining) < QUOTA_CREATE_FLOOR_PERCENT:
        return [
            f"Account {agent.get('account')} has only {float(remaining):.1f}% weekly quota left. Avoid further interruptions: an in-flight turn can continue, but another resume may fail."
        ]
    return []


def _handle_status(arguments: Mapping[str, object]) -> str:
    _require_exact_keys(arguments, allowed={"command"}, required={"command"})
    warnings: list[str] = []
    try:
        warnings.extend(_refresh_quotas())
    except SubagentError as exc:
        warnings.append(f"Quota refresh is unavailable; showing persisted data: {exc}")
    _reconcile_agents()
    with _registry_lock():
        state = _load_state_locked()
        caller_id = _resolve_caller_locked(state)
        payload, projected_warnings = _status_payload(state, caller_id)
        payload["bus"] = {
            "message_count": _bus_count_locked(),
            "default_read_count": 25,
        }
    warnings.extend(projected_warnings)
    return _response(
        "status",
        ok=True,
        summary=(
            f"Status includes {len(payload['agents'])} registered agents and "
            f"{len(payload['accounts'])} available account records."
        ),
        data=payload,
        warnings=warnings,
    )


def _handle_create(arguments: Mapping[str, object]) -> str:
    _require_exact_keys(
        arguments,
        allowed={
            "command", "reasoning_effort", "instructions", "description",
            "highest_spawn_tier", "label",
        },
        required={"command", "reasoning_effort", "instructions", "description"},
    )
    tier = _validate_tier(arguments.get("reasoning_effort"))
    instructions = _require_string(arguments, "instructions", minimum=1, maximum=100_000)
    description = _require_string(arguments, "description", minimum=200, maximum=8_000)
    label = (
        _validate_label(arguments.get("label"))
        if "label" in arguments else "general-purpose"
    )
    provider = str(TIER_CONFIG[tier]["provider"])
    warnings: list[str] = []
    try:
        warnings.extend(_refresh_quotas())
    except SubagentError as exc:
        if provider == "codex":
            raise
        warnings.append(f"Codex account discovery failed but Cursor allocation can proceed: {exc}")
    _reconcile_agents()

    now = time.time()
    capability = secrets.token_urlsafe(32)
    nonce = secrets.token_hex(16)
    with _registry_lock():
        state = _load_state_locked()
        caller_id = _resolve_caller_locked(state)
        _require_authority(state, caller_id, tier)
        caller_ceiling = _caller_ceiling(state, caller_id)
        ceiling_value = arguments.get("highest_spawn_tier")
        ceiling = _effective_ceiling(
            ceiling_value, own_tier=tier, caller_ceiling=caller_ceiling,
        )
        cost = int(TIER_CONFIG[tier]["cost"])
        budget_used = _live_budget(state)
        if budget_used + cost > SUBAGENT_BUDGET:
            raise SubagentError(
                "BUDGET_EXCEEDED",
                f"Creating a {tier} costs {cost}, but only {SUBAGENT_BUDGET - budget_used} of the fixed {SUBAGENT_BUDGET} live-agent budget remains. Kill an existing agent or choose a cheaper tier; the fixed safety budget must not be altered.",
                details={
                    "budget_used": budget_used,
                    "budget_limit": SUBAGENT_BUDGET,
                    "requested_cost": cost,
                },
            )
        account = _select_account(state, provider)
        agent_id = int(state["next_agent_id"])
        state["next_agent_id"] = agent_id + 1
        bus_count = _bus_count_locked()
        record: dict[str, Any] = {
            "id": agent_id,
            "parent_id": caller_id,
            "account": account,
            "provider": provider,
            "tier": tier,
            "model": TIER_CONFIG[tier]["model"],
            "reasoning": TIER_CONFIG[tier]["reasoning"],
            "cost": cost,
            "label": label,
            "description": description,
            "highest_spawn_tier": ceiling,
            "status": "WORKING",
            "operation_state": "STARTING",
            "operation_started_at": now,
            "operation_nonce": nonce,
            "operation_previous_status": None,
            "operation_previous_runtime_identity": None,
            "operation_previous_cleanup_required": False,
            "operation_intended_status": "WORKING",
            "operation_phase": "RESERVED",
            "operation_instruction_sent_at": None,
            "operation_cursor_log_start": None,
            "operation_runtime_log_file": None,
            "operation_expected_session_id": None,
            "operation_assignment_acknowledged_at": None,
            "operation_cleanup_required": False,
            "generation": 1,
            "created_at": now,
            "state_changed_at": now,
            "last_instruction_at": None,
            "last_interrupted_at": None,
            "latest_log_at": None,
            "latest_message_at": None,
            "interruption_count": 0,
            "tokens": None,
            "tokens_known": False,
            "capability_sha256": _capability_digest(capability),
            "pending_capability_sha256": None,
            "pending_description": None,
            "pending_label": None,
            "pending_highest_spawn_tier": None,
            "tmux_session": f"bioseq-agent-{_REPOSITORY_KEY}-{agent_id}",
            "tmux_pane": None,
            "pane_pid": None,
            "runtime_identity": None,
            "vendor_session_id": None,
            "log_file": str(SUBAGENT_LOG_DIR / f"agent-{agent_id}.log"),
            "runtime_log_file": None,
            "last_error": None,
        }
        state["agents"][str(agent_id)] = record
        _save_state_locked(state)
    prompt = _prompt_prefix(agent_id, tier, ceiling, bus_count) + instructions

    try:
        runtime = _launch_runtime(
            record,
            capability,
            prompt,
            resume=False,
            operation_context=(agent_id, 1, nonce),
        )
    except Exception as raw_exc:
        exc = raw_exc if isinstance(raw_exc, SubagentError) else SubagentError(
            "RUNTIME_ERROR", "agent launch failed", details=str(raw_exc),
        )
        cleanup_confirmed = False
        try:
            _kill_runtime(record)
            cleanup_confirmed = True
        except Exception:
            cleanup_confirmed = False
        with _registry_lock():
            state = _load_state_locked()
            current = state["agents"].get(str(agent_id))
            if (
                isinstance(current, dict)
                and current.get("generation") == 1
                and secrets.compare_digest(str(current.get("operation_nonce", "")), nonce)
            ):
                current["status"] = "KILLED" if cleanup_confirmed else "WORKING"
                current["operation_state"] = (
                    "LAUNCH_FAILED" if cleanup_confirmed else "ORPHANED_RUNTIME_UNKNOWN"
                )
                current["state_changed_at"] = time.time()
                current["last_error"] = str(exc)
                current["capability_sha256"] = None
                current["pending_capability_sha256"] = None
                _clear_operation_transaction(current)
                _save_state_locked(state)
        raise exc

    with _registry_lock():
        state = _load_state_locked()
        current = _operation_agent_locked(state, agent_id, 1, nonce)
        runtime = dict(runtime)
        acknowledged_status = str(runtime.pop("_acknowledged_status", "WORKING"))
        if acknowledged_status not in {"WORKING", "INTERRUPTED", "KILLED"}:
            raise SubagentError(
                "RUNTIME_ERROR", "agent backend returned an invalid acknowledged lifecycle state",
            )
        instruction_sent_at = runtime.pop("_instruction_sent_at", now)
        current.update(runtime)
        current["status"] = acknowledged_status
        current["operation_state"] = (
            "RUNNING" if acknowledged_status == "WORKING" else "TURN_COMPLETED"
        )
        if acknowledged_status == "KILLED":
            current["capability_sha256"] = None
        current["pending_capability_sha256"] = None
        current["last_instruction_at"] = instruction_sent_at
        _clear_operation_transaction(current)
        current["last_error"] = None
        _save_state_locked(state)
        budget_used = _live_budget(state)
    if provider == "cursor":
        warnings.append(
            "Composer 2.5 is a fast, weak one-shot backend with no quota endpoint; use it only for simplistic tasks, and its process may naturally become KILLED when the turn completes."
        )
    return _response(
        "create",
        ok=True,
        summary=f"Created subagent {agent_id} as {tier} on {account}.",
        data={
            "agent_id": agent_id,
            "parent_id": caller_id,
            "account": account,
            "provider": provider,
            "reasoning_effort": tier,
            "model": TIER_CONFIG[tier]["model"],
            "vendor_reasoning": TIER_CONFIG[tier]["reasoning"],
            "cost": cost,
            "label": label,
            "highest_spawn_tier": ceiling,
            "status": acknowledged_status,
            "budget_used": budget_used,
            "budget_limit": SUBAGENT_BUDGET,
        },
        warnings=warnings,
    )


def _handle_interrupt(arguments: Mapping[str, object]) -> str:
    _require_exact_keys(
        arguments,
        allowed={"command", "agent_id", "instructions"},
        required={"command"},
    )
    agent_id = _target_id(arguments)
    instructions = (
        _require_string(arguments, "instructions", minimum=1, maximum=100_000)
        if "instructions" in arguments else None
    )
    quota_warnings: list[str] = []
    try:
        quota_warnings.extend(_refresh_quotas())
    except SubagentError as exc:
        quota_warnings.append(
            f"Quota refresh failed before interrupt; cached data may be stale: {exc}"
        )
    _reconcile_agents()
    with _registry_lock():
        state = _load_state_locked()
        caller_id = _resolve_caller_locked(state)
        agent = _mutable_agent_locked(state, caller_id, agent_id)
        if agent.get("status") == "KILLED":
            raise SubagentError(
                "AGENT_NOT_LIVE",
                f"subagent {agent_id} is KILLED; use repurpose to resume its saved context",
            )
        provider = str(agent.get("provider"))
        old_status = str(agent["status"])
        old_operation = str(agent.get("operation_state", "RUNNING"))
        if old_status == "INTERRUPTED" and instructions is None:
            warnings = list(dict.fromkeys([
                *quota_warnings,
                *_account_resume_warning(state, agent),
            ]))
            return _response(
                "interrupt", ok=True,
                summary=f"Subagent {agent_id} is already INTERRUPTED.",
                data={"agent_id": agent_id, "status": "INTERRUPTED", "interruption_sent": False},
                warnings=warnings,
            )
        if (
            provider == "cursor"
            and (
                not isinstance(agent.get("vendor_session_id"), str)
                or not str(agent.get("vendor_session_id", "")).strip()
            )
        ):
            raise SubagentError(
                "RESUME_CONTEXT_MISSING",
                "contractor-intern has not exposed a resumable Cursor chat ID yet; its current work was left running rather than destructively interrupting context",
            )
        capability = secrets.token_urlsafe(32) if provider == "cursor" and instructions is not None else None
        agent["pending_capability_sha256"] = (
            _capability_digest(capability) if capability is not None else None
        )
        generation, nonce = _operation_token(
            agent,
            "INTERRUPTING",
            intended_status="WORKING" if instructions is not None else "INTERRUPTED",
        )
        bus_count = _bus_count_locked()
        snapshot = dict(agent)
        _save_state_locked(state)

    did_interrupt = False
    interrupt_confirmed = old_status == "INTERRUPTED"
    acknowledged_status: str | None = None
    instruction_sent_at: float | None = None
    runtime: dict[str, Any] = {}
    cursor_idle = False
    cursor_forced_stop = False
    try:
        target = snapshot.get("tmux_pane") or snapshot.get("tmux_session")
        if not isinstance(target, str):
            raise SubagentError("RUNTIME_MISSING", "agent has no tmux target")
        prompt = (
            _prompt_prefix(
                agent_id,
                str(snapshot["tier"]),
                str(snapshot["highest_spawn_tier"]),
                bus_count,
            ) + instructions
            if instructions is not None else None
        )
        if provider == "cursor":
            # Cursor print mode is one-shot rather than an interactive TUI.  We
            # still send the requested Escape, then stop the owned child.  A
            # bare interrupt replaces it with an owned idle supervisor; an
            # instruction-bearing interrupt resumes the exact saved chat.
            if old_status == "WORKING":
                _interrupt_runtime(snapshot)
                did_interrupt = True
                interrupt_confirmed = _wait_for_owned_runtime_exit(snapshot)
                cursor_forced_stop = not interrupt_confirmed
            if prompt is None:
                runtime = _start_idle_runtime(snapshot)
                _record_operation_progress(
                    (agent_id, generation, nonce),
                    "VENDOR_STARTED",
                    **{
                        key: value for key, value in runtime.items()
                        if key in {"tmux_pane", "pane_pid", "runtime_identity"}
                    },
                )
                cursor_idle = True
                acknowledged_status = "INTERRUPTED"
            else:
                assert capability is not None
                runtime = _launch_runtime(
                    snapshot,
                    capability,
                    prompt,
                    resume=True,
                    operation_context=(agent_id, generation, nonce),
                )
                acknowledged_status = str(
                    runtime.pop("_acknowledged_status", "WORKING")
                )
                instruction_sent_at = runtime.pop(
                    "_instruction_sent_at", time.time(),
                )
        else:
            _record_operation_progress(
                (agent_id, generation, nonce),
                "VENDOR_STARTED",
                tmux_pane=snapshot.get("tmux_pane"),
                pane_pid=snapshot.get("pane_pid"),
                runtime_identity=snapshot.get("runtime_identity"),
            )
            if old_status == "WORKING":
                _interrupt_runtime(snapshot)
                did_interrupt = True
                _wait_for_codex_ready(target, timeout=20)
                interrupt_confirmed = True
            if prompt is not None:
                if old_status == "INTERRUPTED":
                    _wait_for_codex_ready(target, timeout=10)
                instruction_sent_at = time.time()
                _record_operation_progress(
                    (agent_id, generation, nonce),
                    "INSTRUCTION_SENDING",
                    operation_instruction_sent_at=instruction_sent_at,
                    last_instruction_at=instruction_sent_at,
                )
                _send_literal(target, prompt)
                _record_operation_progress(
                    (agent_id, generation, nonce), "INSTRUCTION_SENT",
                )
                acknowledged_status = _observe_codex_assignment(
                    snapshot,
                    target,
                    not_before=instruction_sent_at,
                )
                _record_operation_progress(
                    (agent_id, generation, nonce),
                    "ASSIGNMENT_ACKNOWLEDGED",
                    operation_assignment_acknowledged_at=time.time(),
                )
    except Exception as raw_exc:
        exc = raw_exc if isinstance(raw_exc, SubagentError) else SubagentError(
            "RUNTIME_ERROR", "agent interruption failed", details=str(raw_exc),
        )
        cursor_present: bool | None = None
        cursor_pane: dict[str, Any] | None = None
        if provider == "cursor":
            with contextlib.suppress(SubagentError):
                cursor_present, cursor_pane = _owned_runtime_observation(snapshot)
        with _registry_lock():
            state = _load_state_locked()
            current = _operation_agent_locked(state, agent_id, generation, nonce)
            if did_interrupt:
                current["interruption_count"] = int(current.get("interruption_count", 0)) + 1
                current["last_interrupted_at"] = time.time()
            defer_cursor_recovery = False
            cursor_command = cursor_pane.get("pane_command") if cursor_pane else None
            if provider == "cursor" and cursor_present is True and cursor_command in {
                "tail", "/usr/bin/tail",
            }:
                current["status"] = "INTERRUPTED"
                current["operation_state"] = "RECOVERED_INTERRUPTED_RUNTIME"
                current["state_changed_at"] = time.time()
                current["capability_sha256"] = None
                current["pending_capability_sha256"] = None
            elif provider == "cursor" and cursor_present is False:
                current["status"] = "KILLED"
                current["operation_state"] = "INTERRUPT_FAILED_STOPPED"
                current["state_changed_at"] = time.time()
                current["capability_sha256"] = None
                current["pending_capability_sha256"] = None
            elif provider == "cursor":
                # A live/indeterminate pane may be either the original child or
                # the atomically respawned replacement.  Keep the transaction
                # lease and both capability digests until reconciliation can
                # identify it; budget remains reserved throughout.
                current["status"] = "WORKING"
                current["operation_state"] = "INTERRUPTING"
                current["last_error"] = str(exc)
                current["operation_cleanup_required"] = True
                defer_cursor_recovery = True
            elif interrupt_confirmed:
                current["status"] = "INTERRUPTED"
                current["operation_state"] = "USER_INTERRUPTED"
                current["state_changed_at"] = time.time()
            else:
                current["status"] = old_status
                current["operation_state"] = (
                    "INTERRUPT_UNCONFIRMED" if did_interrupt else old_operation
                )
            if not defer_cursor_recovery:
                _clear_operation_transaction(current)
                current["pending_capability_sha256"] = None
                current["last_error"] = str(exc)
            _save_state_locked(state)
        raise exc

    with _registry_lock():
        state = _load_state_locked()
        current = _operation_agent_locked(state, agent_id, generation, nonce)
        current.update(runtime)
        if did_interrupt:
            current["interruption_count"] = int(current.get("interruption_count", 0)) + 1
            current["last_interrupted_at"] = time.time()
        resulting_status = (
            acknowledged_status if instructions is not None else "INTERRUPTED"
        )
        if resulting_status not in {"WORKING", "INTERRUPTED", "KILLED"}:
            raise SubagentError(
                "RUNTIME_ERROR", "agent backend returned an invalid acknowledged lifecycle state",
            )
        current["status"] = resulting_status
        current["operation_state"] = (
            "RUNNING" if resulting_status == "WORKING"
            else "TURN_COMPLETED" if resulting_status == "KILLED" or instructions is not None
            else "FORCED_STOP_IDLE" if provider == "cursor" and cursor_forced_stop
            else "USER_INTERRUPTED"
        )
        _clear_operation_transaction(current)
        current["state_changed_at"] = time.time()
        if instruction_sent_at is not None:
            current["last_instruction_at"] = instruction_sent_at
        if provider == "cursor":
            if cursor_idle or resulting_status == "KILLED":
                current["capability_sha256"] = None
            elif instructions is not None and isinstance(
                current.get("pending_capability_sha256"), str,
            ):
                current["capability_sha256"] = current["pending_capability_sha256"]
            current["pending_capability_sha256"] = None
        current["last_error"] = None
        warnings = list(dict.fromkeys([
            *quota_warnings,
            *_account_resume_warning(state, current),
        ]))
        if provider == "cursor" and not isinstance(current.get("vendor_session_id"), str):
            warnings.append(
                "Cursor did not expose a resumable chat ID before interruption; this idle record cannot accept new work through resume, so use create if repurpose is rejected."
            )
        if provider == "cursor" and cursor_forced_stop:
            warnings.append(
                "Cursor headless mode did not acknowledge Escape by exiting; the controller atomically replaced the owned process and reports FORCED_STOP_IDLE instead of claiming an interactive ESC cancellation."
            )
        interruption_count = int(current.get("interruption_count", 0))
        _save_state_locked(state)
    if interruption_count > 10:
        warnings.append(
            "This agent has more than 10 interruptions; repeated resumes resend growing context and raise token cost. Let it work without further interruption when possible."
        )
    status = acknowledged_status if instructions is not None else "INTERRUPTED"
    return _response(
        "interrupt", ok=True,
        summary=(
            f"Subagent {agent_id} is {status}; "
            + (
                "new instructions were submitted in its retained vendor context."
                if instructions is not None
                else (
                    "Escape was sent; the headless child was forcibly replaced and the owned runtime is idle."
                    if provider == "cursor" and cursor_forced_stop
                    else "Escape was acknowledged by process exit and the owned runtime is idle."
                    if provider == "cursor"
                    else "Escape was sent and the owned runtime is idle."
                )
            )
        ),
        data={
            "agent_id": agent_id,
            "status": status,
            "interruption_sent": did_interrupt,
            "instructions_submitted": instructions is not None,
            "forced_stop": provider == "cursor" and cursor_forced_stop,
            "interruption_count": interruption_count,
        },
        warnings=warnings,
    )


def _handle_logs(arguments: Mapping[str, object]) -> str:
    _require_exact_keys(
        arguments,
        allowed={"command", "agent_id", "lines"},
        required={"command"},
    )
    agent_id = _target_id(arguments)
    lines = (
        _require_integer(arguments, "lines", minimum=1, maximum=2_000)
        if "lines" in arguments else 200
    )
    _reconcile_agents()
    with _registry_lock():
        state = _load_state_locked()
        caller_id = _resolve_caller_locked(state)
        agent = dict(_agent_locked(state, agent_id))
    if agent_id == 0:
        return _response(
            "logs", ok=True,
            summary="ROOT is registry metadata and has no managed Codex CLI log.",
            data={"agent_id": 0, "available": False, "lines_requested": lines, "log": ""},
        )
    log_path = Path(str(agent.get("log_file", "")))
    log_text = _tail_file(log_path, lines)
    if not log_text and agent.get("status") in LIVE_STATES:
        target = agent.get("tmux_pane") or agent.get("tmux_session")
        if isinstance(target, str):
            log_text = "\n".join(_capture_pane(target).splitlines()[-lines:])
            if len(log_text) > LOG_RESPONSE_MAX_CHARS:
                log_text = "…(truncated)…\n" + log_text[-LOG_RESPONSE_MAX_CHARS:]
    return _response(
        "logs", ok=True,
        summary=f"Returned the persisted tail for subagent {agent_id}.",
        data={
            "agent_id": agent_id,
            "status": agent.get("status"),
            "available": bool(log_text) or log_path.exists(),
            "lines_requested": lines,
            "max_characters": LOG_RESPONSE_MAX_CHARS,
            "log": log_text,
        },
    )


def _handle_kill(arguments: Mapping[str, object]) -> str:
    _require_exact_keys(
        arguments,
        allowed={"command", "agent_id"},
        required={"command"},
    )
    agent_id = _target_id(arguments)
    _reconcile_agents()
    with _registry_lock():
        state = _load_state_locked()
        caller_id = _resolve_caller_locked(state)
        agent = _mutable_agent_locked(
            state, caller_id, agent_id, allow_cleanup=True,
        )
        if agent.get("status") == "KILLED":
            return _response(
                "kill", ok=True,
                summary=f"Subagent {agent_id} was already KILLED; its registry and context remain available.",
                data={"agent_id": agent_id, "status": "KILLED", "already_killed": True},
            )
        old_operation = str(agent.get("operation_state", "RUNNING"))
        old_cleanup_required = agent.get("operation_cleanup_required") is True
        generation, nonce = _operation_token(
            agent, "KILLING", intended_status="KILLED",
        )
        snapshot = dict(agent)
        _save_state_locked(state)
    try:
        _kill_runtime(snapshot)
    except Exception as raw_exc:
        exc = raw_exc if isinstance(raw_exc, SubagentError) else SubagentError(
            "RUNTIME_ERROR", "agent kill failed", details=str(raw_exc),
        )
        with _registry_lock():
            state = _load_state_locked()
            current = _operation_agent_locked(state, agent_id, generation, nonce)
            current["operation_state"] = (
                "CLEANUP_KILL_FAILED"
                if old_cleanup_required or old_operation in PENDING_OPERATION_STATES
                else old_operation
            )
            _clear_operation_transaction(current)
            current["operation_cleanup_required"] = (
                old_cleanup_required or old_operation in PENDING_OPERATION_STATES
            )
            current["last_error"] = str(exc)
            _save_state_locked(state)
        raise exc
    with _registry_lock():
        state = _load_state_locked()
        current = _operation_agent_locked(state, agent_id, generation, nonce)
        current["status"] = "KILLED"
        current["operation_state"] = "REQUESTED_KILL"
        _clear_operation_transaction(current)
        current["state_changed_at"] = time.time()
        current["capability_sha256"] = None
        current["pending_capability_sha256"] = None
        current["pending_description"] = None
        current["pending_label"] = None
        current["pending_highest_spawn_tier"] = None
        current["last_error"] = None
        _save_state_locked(state)
        budget_used = _live_budget(state)
    return _response(
        "kill", ok=True,
        summary=f"Killed subagent {agent_id}; its metadata, logs, and vendor context were preserved.",
        data={
            "agent_id": agent_id,
            "status": "KILLED",
            "already_killed": False,
            "budget_used": budget_used,
            "budget_limit": SUBAGENT_BUDGET,
        },
    )


def _handle_repurpose(arguments: Mapping[str, object]) -> str:
    _require_exact_keys(
        arguments,
        allowed={
            "command", "agent_id", "instructions", "description",
            "highest_spawn_tier", "label",
        },
        required={"command", "instructions", "description"},
    )
    agent_id = _target_id(arguments)
    instructions = _require_string(arguments, "instructions", minimum=1, maximum=100_000)
    description = _require_string(arguments, "description", minimum=200, maximum=8_000)
    requested_label = (
        _validate_label(arguments.get("label")) if "label" in arguments else None
    )
    try:
        quota_warnings = _refresh_quotas()
    except SubagentError as exc:
        quota_warnings = [f"Quota refresh failed before repurpose: {exc}"]
    _reconcile_agents()

    capability: str | None = None
    with _registry_lock():
        state = _load_state_locked()
        caller_id = _resolve_caller_locked(state)
        agent = _mutable_agent_locked(state, caller_id, agent_id)
        tier = _validate_tier(agent.get("tier"), "stored agent tier")
        caller_ceiling = _caller_ceiling(state, caller_id)
        if "highest_spawn_tier" in arguments:
            new_ceiling = _effective_ceiling(
                arguments.get("highest_spawn_tier"),
                own_tier=tier,
                caller_ceiling=caller_ceiling,
            )
        else:
            new_ceiling = _validate_tier(
                agent.get("highest_spawn_tier"), "stored highest_spawn_tier",
            )
        new_label = str(agent.get("label")) if requested_label is None else requested_label
        old_status = str(agent.get("status"))
        old_operation = str(agent.get("operation_state", "RUNNING"))
        bus_count = _bus_count_locked()
        prompt = _prompt_prefix(agent_id, tier, new_ceiling, bus_count) + instructions
        resume = old_status == "KILLED" or agent.get("provider") == "cursor"
        if agent.get("provider") == "codex":
            account = state["accounts"].get(str(agent.get("account")))
            remaining = account.get("remaining_percent") if isinstance(account, dict) else None
            if isinstance(remaining, (int, float)) and float(remaining) <= 0.0:
                raise SubagentError(
                    "ACCOUNT_QUOTA_EXHAUSTED",
                    f"Subagent {agent_id}'s original account has 0% weekly quota. Repurpose must submit on that account to preserve context, so it is blocked; use create to load-balance a new agent onto an account with quota.",
                )
        if resume:
            if not isinstance(agent.get("vendor_session_id"), str):
                raise SubagentError(
                    "RESUME_CONTEXT_MISSING",
                    f"subagent {agent_id} has no saved vendor session ID; create a new agent instead because context cannot be resumed honestly",
                )
            cost = int(agent.get("cost", 0))
            budget_used = _live_budget(state)
            if old_status == "KILLED" and budget_used + cost > SUBAGENT_BUDGET:
                raise SubagentError(
                    "BUDGET_EXCEEDED",
                    f"Repurposing subagent {agent_id} would reactivate cost {cost}, but only {SUBAGENT_BUDGET - budget_used} of the fixed {SUBAGENT_BUDGET} budget remains. Kill another agent or use create with a cheaper tier; do not alter the fixed budget.",
                )
            capability = secrets.token_urlsafe(32)
            agent["pending_capability_sha256"] = _capability_digest(capability)
            if old_status == "KILLED":
                agent["status"] = "WORKING"
        else:
            agent["pending_capability_sha256"] = None
        agent["pending_description"] = description
        agent["pending_label"] = new_label
        agent["pending_highest_spawn_tier"] = new_ceiling
        generation, nonce = _operation_token(
            agent,
            "RESUMING" if resume else "REPURPOSING",
            intended_status="WORKING",
        )
        agent["operation_previous_status"] = old_status
        snapshot = dict(agent)
        _save_state_locked(state)

    did_interrupt = False
    interrupt_confirmed = old_status == "INTERRUPTED"
    acknowledged_status = "WORKING"
    instruction_sent_at: float | None = None
    repurpose_forced_stop = False
    try:
        if resume:
            if old_status in LIVE_STATES:
                if old_status == "WORKING" and snapshot.get("provider") == "cursor":
                    _interrupt_runtime(snapshot)
                    did_interrupt = True
                    interrupt_confirmed = _wait_for_owned_runtime_exit(snapshot)
                    repurpose_forced_stop = not interrupt_confirmed
            assert capability is not None
            runtime = _launch_runtime(
                snapshot,
                capability,
                prompt,
                resume=True,
                operation_context=(agent_id, generation, nonce),
            )
        else:
            runtime = {}
            target = snapshot.get("tmux_pane") or snapshot.get("tmux_session")
            if not isinstance(target, str):
                raise SubagentError("RUNTIME_MISSING", "agent has no tmux target")
            _record_operation_progress(
                (agent_id, generation, nonce),
                "VENDOR_STARTED",
                tmux_pane=snapshot.get("tmux_pane"),
                pane_pid=snapshot.get("pane_pid"),
                runtime_identity=snapshot.get("runtime_identity"),
            )
            if old_status == "WORKING":
                _interrupt_runtime(snapshot)
                did_interrupt = True
                _wait_for_codex_ready(target, timeout=20)
                interrupt_confirmed = True
            else:
                _wait_for_codex_ready(target, timeout=10)
            instruction_sent_at = time.time()
            _record_operation_progress(
                (agent_id, generation, nonce),
                "INSTRUCTION_SENDING",
                operation_instruction_sent_at=instruction_sent_at,
                last_instruction_at=instruction_sent_at,
            )
            _send_literal(target, prompt)
            _record_operation_progress(
                (agent_id, generation, nonce), "INSTRUCTION_SENT",
            )
            acknowledged_status = _observe_codex_assignment(
                snapshot,
                target,
                not_before=instruction_sent_at,
            )
            _record_operation_progress(
                (agent_id, generation, nonce),
                "ASSIGNMENT_ACKNOWLEDGED",
                operation_assignment_acknowledged_at=time.time(),
            )
    except Exception as raw_exc:
        exc = raw_exc if isinstance(raw_exc, SubagentError) else SubagentError(
            "RUNTIME_ERROR", "agent repurpose failed", details=str(raw_exc),
        )
        cursor_resume = resume and snapshot.get("provider") == "cursor"
        cursor_present: bool | None = None
        cursor_pane: dict[str, Any] | None = None
        cleanup_confirmed = not resume
        if cursor_resume:
            with contextlib.suppress(SubagentError):
                cursor_present, cursor_pane = _owned_runtime_observation(snapshot)
        elif resume:
            try:
                _kill_runtime(snapshot)
                cleanup_confirmed = True
            except Exception:
                cleanup_confirmed = False
        with _registry_lock():
            state = _load_state_locked()
            current = _operation_agent_locked(state, agent_id, generation, nonce)
            defer_cursor_recovery = False
            cursor_command = cursor_pane.get("pane_command") if cursor_pane else None
            if cursor_resume and cursor_present is True and cursor_command in {
                "tail", "/usr/bin/tail",
            }:
                current["status"] = "INTERRUPTED"
                current["operation_state"] = "RESUME_FAILED_IDLE"
                current["capability_sha256"] = None
                current["pending_capability_sha256"] = None
            elif cursor_resume and cursor_present is False:
                current["status"] = "KILLED"
                current["operation_state"] = "RESUME_FAILED"
                current["capability_sha256"] = None
                current["pending_capability_sha256"] = None
            elif cursor_resume:
                current["status"] = "WORKING"
                current["operation_state"] = "RESUMING"
                current["last_error"] = str(exc)
                current["operation_cleanup_required"] = True
                defer_cursor_recovery = True
            elif resume:
                current["status"] = "KILLED" if cleanup_confirmed else "WORKING"
                current["operation_state"] = (
                    "RESUME_FAILED" if cleanup_confirmed else "ORPHANED_RUNTIME_UNKNOWN"
                )
                current["capability_sha256"] = None
                current["pending_capability_sha256"] = None
            elif interrupt_confirmed:
                current["status"] = "INTERRUPTED"
                current["operation_state"] = "USER_INTERRUPTED"
            else:
                current["status"] = old_status
                current["operation_state"] = (
                    "INTERRUPT_UNCONFIRMED" if did_interrupt else old_operation
                )
            if did_interrupt:
                current["interruption_count"] = int(current.get("interruption_count", 0)) + 1
                current["last_interrupted_at"] = time.time()
            if not defer_cursor_recovery:
                _clear_operation_transaction(current)
                current["pending_capability_sha256"] = None
                current["pending_description"] = None
                current["pending_label"] = None
                current["pending_highest_spawn_tier"] = None
                current["state_changed_at"] = time.time()
                current["last_error"] = str(exc)
            _save_state_locked(state)
        raise exc

    with _registry_lock():
        state = _load_state_locked()
        current = _operation_agent_locked(state, agent_id, generation, nonce)
        runtime = dict(runtime)
        acknowledged_status = str(
            runtime.pop("_acknowledged_status", acknowledged_status)
        )
        if acknowledged_status not in {"WORKING", "INTERRUPTED", "KILLED"}:
            raise SubagentError(
                "RUNTIME_ERROR", "agent backend returned an invalid acknowledged lifecycle state",
            )
        instruction_sent_at = runtime.pop(
            "_instruction_sent_at", instruction_sent_at or time.time(),
        )
        current.update(runtime)
        current["description"] = description
        current["label"] = new_label
        current["highest_spawn_tier"] = new_ceiling
        current["status"] = acknowledged_status
        current["operation_state"] = (
            "RUNNING" if acknowledged_status == "WORKING" else "TURN_COMPLETED"
        )
        if resume:
            if acknowledged_status == "KILLED":
                current["capability_sha256"] = None
            elif isinstance(current.get("pending_capability_sha256"), str):
                current["capability_sha256"] = current["pending_capability_sha256"]
        current["pending_capability_sha256"] = None
        current["pending_description"] = None
        current["pending_label"] = None
        current["pending_highest_spawn_tier"] = None
        _clear_operation_transaction(current)
        current["state_changed_at"] = time.time()
        current["last_instruction_at"] = instruction_sent_at
        current["last_error"] = None
        if did_interrupt:
            current["interruption_count"] = int(current.get("interruption_count", 0)) + 1
            current["last_interrupted_at"] = time.time()
        warnings = [*quota_warnings, *_account_resume_warning(state, current)]
        if repurpose_forced_stop:
            warnings.append(
                "Cursor headless mode did not acknowledge Escape by exiting; repurpose atomically replaced the owned process and retained the exact saved chat."
            )
        interruption_count = int(current.get("interruption_count", 0))
        _save_state_locked(state)
    if interruption_count > 10:
        warnings.append(
            "This agent has more than 10 interruptions; repeated resumes resend growing context and raise token cost."
        )
    return _response(
        "repurpose", ok=True,
        summary=f"Repurposed subagent {agent_id} and submitted its new work in the existing vendor context.",
        data={
            "agent_id": agent_id,
            "status": acknowledged_status,
            "description": description,
            "label": new_label,
            "highest_spawn_tier": new_ceiling,
            "resumed_process": resume,
            "interruption_count": interruption_count,
        },
        warnings=warnings,
    )


def _handle_bus_read(arguments: Mapping[str, object]) -> str:
    _require_exact_keys(
        arguments,
        allowed={"command", "message_count"},
        required={"command"},
    )
    message_count = (
        _require_integer(arguments, "message_count", minimum=1, maximum=200)
        if "message_count" in arguments else 25
    )
    with _registry_lock():
        state = _load_state_locked()
        caller_id = _resolve_caller_locked(state)
        bus = _load_bus_locked()
        messages = list(bus["messages"][-message_count:])
        total = len(bus["messages"])
    return _response(
        "bus-read", ok=True,
        summary=f"Returned {len(messages)} of {total} persisted bus messages.",
        data={
            "caller_id": caller_id,
            "requested_count": message_count,
            "total_message_count": total,
            "messages": messages,
        },
    )


def _handle_bus_write(arguments: Mapping[str, object]) -> str:
    _require_exact_keys(
        arguments,
        allowed={"command", "message"},
        required={"command", "message"},
    )
    message = _require_string(arguments, "message", minimum=1, maximum=20_000)
    with _registry_lock():
        state = _load_state_locked()
        caller_id = _resolve_caller_locked(state)
        author = _agent_locked(state, caller_id)
        bus = _load_bus_locked()
        message_id = int(bus["next_message_id"])
        entry = {
            "id": message_id,
            "author_agent_id": caller_id,
            "author_tier": author.get("tier"),
            "author_label": author.get("label"),
            "created_at": time.time(),
            "message": message,
        }
        bus["next_message_id"] = message_id + 1
        bus["messages"].append(entry)
        if len(bus["messages"]) > BUS_RETENTION_MESSAGES:
            del bus["messages"][:-BUS_RETENTION_MESSAGES]
        _save_bus_locked(bus)
        retained = len(bus["messages"])
    return _response(
        "bus-write", ok=True,
        summary=f"Published shared bus message {message_id} from agent {caller_id}.",
        data={"message": entry, "retained_message_count": retained},
    )


_COMMAND_HANDLERS = {
    "status": _handle_status,
    "create": _handle_create,
    "interrupt": _handle_interrupt,
    "logs": _handle_logs,
    "kill": _handle_kill,
    "repurpose": _handle_repurpose,
    "bus-read": _handle_bus_read,
    "bus-write": _handle_bus_write,
}


def tool_subagent(arguments: Mapping[str, object]) -> str:
    """Validate and execute one public subagent command.

    The protocol layer deliberately does not rely on client-side JSON Schema
    enforcement.  Every branch is validated here so malformed direct MCP
    requests receive the same stable JSON error envelope.
    """
    command = "unknown"
    try:
        if not isinstance(arguments, Mapping):
            raise SubagentError("INVALID_ARGUMENT", "subagent arguments must be an object")
        raw_command = arguments.get("command")
        if not isinstance(raw_command, str) or raw_command not in COMMANDS:
            raise SubagentError(
                "INVALID_COMMAND",
                "command must be one of: " + ", ".join(sorted(COMMANDS)),
            )
        command = raw_command
        return _COMMAND_HANDLERS[command](arguments)
    except SubagentError as exc:
        return _error_response(command, exc)
    except Exception as exc:  # Defensive MCP boundary: never crash the server.
        return _error_response(
            command,
            SubagentError(
                "INTERNAL_ERROR",
                "the subagent controller encountered an unexpected internal error",
                details=f"{type(exc).__name__}: {exc}",
            ),
        )


def subagent_activity_snapshot() -> dict[str, Any]:
    """Return the closed path-free aggregate used by the generic status tool."""
    if not SUBAGENT_STATE_FILE.exists():
        return {"queued": 0, "running": 0, "state": "absent"}
    try:
        with _registry_lock():
            state = _load_state_locked()
            agents = [
                agent for agent in state["agents"].values()
                if isinstance(agent, dict) and agent.get("id") != 0
            ]
    except (OSError, SubagentError):
        return {"queued": 0, "running": 0, "state": "unknown"}
    if any(agent.get("status") not in PUBLIC_STATES for agent in agents):
        return {"queued": 0, "running": 0, "state": "unknown"}
    running = sum(1 for agent in agents if agent.get("status") in LIVE_STATES)
    state_name = "running" if running else "idle"
    return {"queued": 0, "running": running, "state": state_name}
