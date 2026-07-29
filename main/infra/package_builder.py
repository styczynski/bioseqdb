#!/usr/bin/env python3
"""Provision, run, verify, and image BioSeq's native AWS package builders.

This is the OpenTofu-only entry point for DOC0390.  The normal ``run`` command
owns the complete lifecycle: it selects architecture-specific cached AMIs,
applies the isolated OpenTofu stack without permitting instance replacement,
starts only the exact instances recorded in that stack, prepares lock-hashed
Poetry environments, builds on both native architectures, validates the remote
manifests and exact S3 object versions, and creates post-build AMIs only for the
complete canonical matrix.  It never stops or terminates an instance and
hard-rejects the two protected development instance IDs from the specification.

Only committed source is transferred to builders.  Package signing material is
kept in AWS Secrets Manager and is never copied through this process or placed
in OpenTofu state.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import contextlib
import datetime as dt
import fcntl
import hashlib
import ipaddress
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Sequence


HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
STACK_DIR = HERE / "package_build_infra"
PACKAGING_DIR = HERE / "packaging"
POETRY_LOCK_FILE = PACKAGING_DIR / "poetry.lock"
MATRIX_FILE = HERE / "package_matrix.json"
RUNTIME_DIR = STACK_DIR / ".runtime"
LOG_DIR = STACK_DIR / ".logs"
LAST_RUN_FILE = RUNTIME_DIR / "last-run.json"
LOCK_FILE = RUNTIME_DIR / "package-builder.lock"

PROTECTED_INSTANCE_IDS = frozenset(
    {
        "i-05b18218430e9fecb",
        "i-05d1fa02e86288637",
    }
)

EXPECTED_INSTANCE_TAGS = {
    "Project": "bioseq",
    "Component": "package-build-machine",
    "ManagedBy": "opentofu",
    "ProtectedFromDelete": "true",
    "Lifecycle": "persistent-build-cache",
}

CACHE_TAGS = {
    "Project": "bioseq",
    "Component": "package-builder-cache",
    "ManagedBy": "bioseq-package-builder",
    "BaseOS": "ubuntu-24.04",
    "CacheStage": "post-build",
    "WorkerGeneration": "python-poetry-v1",
}

BOOTSTRAP_SCHEMA = "2"
WORKER_GENERATION = "python-poetry-v1"
EXPECTED_PACKAGE_VERSION = "4.2.3"
EXPECTED_SIGNING_FINGERPRINT = "452F34C5F1EF6D471EC041ADDD94780B0728F093"
DEFAULT_REGION = "us-east-1"
DEFAULT_S3_PREFIX = "bioseq/packages"
REMOTE_WORKSPACE = "/opt/bioseq-package-builder/workspace"
REMOTE_ROOT = f"{REMOTE_WORKSPACE}/builds"
REMOTE_POETRY_VENV_ROOT = f"{REMOTE_WORKSPACE}/caches/poetry-venvs"
INSTANCE_ID_RE = re.compile(r"^i-[0-9a-f]{8,17}$")
AMI_ID_RE = re.compile(r"^ami-[0-9a-f]{8,17}$")
S3_PREFIX_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]*$")
ACTIVE_CHILDREN: set[subprocess.Popen[str]] = set()
ACTIVE_CHILDREN_LOCK = threading.RLock()


class PackageBuilderError(RuntimeError):
    """An actionable, fail-closed package-builder error."""


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")


def info(message: str) -> None:
    print(f"[package-builder] {message}", flush=True)


def command_text(argv: Sequence[str]) -> str:
    return shlex.join(str(arg) for arg in argv)


def run_command(
    argv: Sequence[str],
    *,
    cwd: Path | None = None,
    capture: bool = False,
    check: bool = True,
    env: dict[str, str] | None = None,
    stdin: Any = None,
    stdout: Any = None,
) -> subprocess.CompletedProcess[str]:
    info(f"$ {command_text(argv)}")
    if capture and stdout is not None:
        raise PackageBuilderError("run_command cannot combine capture=True with stdout")
    process = subprocess.Popen(
        [str(arg) for arg in argv],
        cwd=str(cwd) if cwd else None,
        text=True,
        env=env,
        stdin=stdin,
        stdout=subprocess.PIPE if capture else stdout,
        stderr=subprocess.PIPE if capture else None,
    )
    with ACTIVE_CHILDREN_LOCK:
        ACTIVE_CHILDREN.add(process)
    try:
        captured_stdout, captured_stderr = process.communicate()
    finally:
        with ACTIVE_CHILDREN_LOCK:
            ACTIVE_CHILDREN.discard(process)
    completed = subprocess.CompletedProcess(
        [str(arg) for arg in argv],
        process.returncode,
        captured_stdout,
        captured_stderr,
    )
    if check and completed.returncode != 0:
        detail = ""
        if capture:
            detail = (completed.stderr or completed.stdout or "").strip()
        suffix = f": {detail}" if detail else ""
        raise PackageBuilderError(
            f"command failed with exit {completed.returncode}: "
            f"{command_text(argv)}{suffix}"
        )
    return completed


def interrupt_active_children() -> None:
    """Forward an interrupt so OpenTofu/SSH can flush state and exit first."""

    with ACTIVE_CHILDREN_LOCK:
        children = list(ACTIVE_CHILDREN)
    for child in children:
        if child.poll() is None:
            with contextlib.suppress(ProcessLookupError):
                child.send_signal(signal.SIGINT)
    deadline = time.monotonic() + 30
    for child in children:
        if child.poll() is not None:
            continue
        remaining = max(0.0, deadline - time.monotonic())
        try:
            child.wait(timeout=remaining)
        except subprocess.TimeoutExpired:
            # SIGINT is OpenTofu's state-flushing interrupt. Escalate only the
            # local child process to TERM (never KILL); EC2 lifecycle is not
            # changed by signal cleanup.
            with contextlib.suppress(ProcessLookupError):
                child.terminate()


def require_executable(name: str) -> str:
    value = shutil.which(name)
    if value is None:
        raise PackageBuilderError(f"required executable `{name}` is not on PATH")
    return value


def tofu_executable() -> str:
    """Return OpenTofu's executable, deliberately with no alternate fallback."""

    configured = os.environ.get("BIOSEQ_TOFU_BIN")
    if configured:
        path = Path(configured).expanduser().resolve()
        if path.name != "tofu" or not path.is_file() or not os.access(path, os.X_OK):
            raise PackageBuilderError(
                "BIOSEQ_TOFU_BIN must name an executable whose basename is `tofu`"
            )
        return str(path)
    return require_executable("tofu")


def aws_json(region: str, service: str, operation: str, *arguments: str) -> Any:
    argv = [
        require_executable("aws"),
        "--region",
        region,
        "--no-cli-pager",
        service,
        operation,
        *arguments,
        "--output",
        "json",
    ]
    result = run_command(argv, capture=True)
    try:
        return json.loads(result.stdout or "{}")
    except json.JSONDecodeError as error:
        raise PackageBuilderError(
            f"AWS CLI returned invalid JSON for {service} {operation}: {error}"
        ) from error


def aws_no_output(region: str, service: str, operation: str, *arguments: str) -> None:
    argv = [
        require_executable("aws"),
        "--region",
        region,
        "--no-cli-pager",
        service,
        operation,
        *arguments,
        "--output",
        "json",
    ]
    run_command(argv)


def detect_public_ipv4() -> str:
    for url in ("https://checkip.amazonaws.com", "https://api.ipify.org"):
        try:
            with urllib.request.urlopen(url, timeout=10) as response:
                value = response.read().decode("ascii").strip()
            address = ipaddress.ip_address(value)
            if address.version == 4 and not address.is_unspecified:
                return str(address)
        except (OSError, ValueError):
            continue
    raise PackageBuilderError(
        "could not detect a public IPv4 address; pass --ssh-cidr as an explicit /32"
    )


def normalize_ssh_cidr(value: str | None) -> str:
    if value is None:
        value = f"{detect_public_ipv4()}/32"
    try:
        network = ipaddress.ip_network(value, strict=True)
    except ValueError as error:
        raise PackageBuilderError(f"invalid --ssh-cidr {value!r}: {error}") from error
    if network.version != 4 or network.prefixlen != 32:
        raise PackageBuilderError("--ssh-cidr must be one explicit IPv4 /32")
    return str(network)


@dataclass(frozen=True)
class Architecture:
    name: str
    aws_architecture: str
    artifact_architecture: str
    instance_type: str


@dataclass(frozen=True)
class Distribution:
    distro_id: str
    package_format: str


@dataclass(frozen=True)
class Matrix:
    architectures: dict[str, Architecture]
    distributions: dict[str, Distribution]
    postgresql_majors: tuple[int, ...]


@dataclass(frozen=True)
class Selection:
    architectures: tuple[str, ...]
    distributions: tuple[str, ...]
    postgresql_majors: tuple[int, ...]


@dataclass(frozen=True)
class Builder:
    architecture: str
    instance_id: str
    instance_type: str
    aws_architecture: str
    public_ip: str
    private_ip: str
    source_ami_id: str


@dataclass(frozen=True)
class PoetryEnvironment:
    lock_sha256: str
    venv_path: str
    python_version: str
    poetry_version: str

    def manifest_value(self) -> dict[str, str]:
        return {
            "python": self.python_version,
            "poetry": self.poetry_version,
        }


@dataclass(frozen=True)
class RemoteBuildHandle:
    builder: Builder
    unit_name: str
    remote_log_path: str
    manifest_path: str
    build_root: str
    local_log_path: Path


def load_matrix(path: Path = MATRIX_FILE) -> Matrix:
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise PackageBuilderError(f"cannot load package matrix {path}: {error}") from error
    if raw.get("schema_version") != 1:
        raise PackageBuilderError("package matrix schema_version must be 1")

    architecture_rows = raw.get("architectures")
    if not isinstance(architecture_rows, dict) or set(architecture_rows) != {
        "arm64",
        "x64",
    }:
        raise PackageBuilderError("package matrix must define exactly arm64 and x64")
    architectures: dict[str, Architecture] = {}
    expected = {
        "arm64": ("arm64", "aarch64", "c7g.xlarge"),
        "x64": ("x86_64", "x86_64", "m4.xlarge"),
    }
    for name, row in architecture_rows.items():
        if not isinstance(row, dict):
            raise PackageBuilderError(f"matrix architecture {name!r} must be an object")
        values = (
            row.get("aws_architecture"),
            row.get("artifact_architecture"),
            row.get("instance_type"),
        )
        if values != expected[name]:
            raise PackageBuilderError(
                f"matrix architecture {name!r} must be {expected[name]}, got {values}"
            )
        architectures[name] = Architecture(name, *values)

    distribution_rows = raw.get("distributions")
    if not isinstance(distribution_rows, list) or not distribution_rows:
        raise PackageBuilderError("package matrix distributions must be a non-empty list")
    distributions: dict[str, Distribution] = {}
    allowed_formats = {"deb", "rpm", "pacman"}
    for row in distribution_rows:
        if not isinstance(row, dict):
            raise PackageBuilderError("every matrix distribution must be an object")
        distro_id = row.get("id")
        package_format = row.get("package_format")
        if not isinstance(distro_id, str) or not distro_id:
            raise PackageBuilderError("every matrix distribution needs a non-empty id")
        if distro_id in distributions:
            raise PackageBuilderError(f"duplicate matrix distribution {distro_id!r}")
        if package_format not in allowed_formats:
            raise PackageBuilderError(
                f"distribution {distro_id!r} has invalid package format {package_format!r}"
            )
        distributions[distro_id] = Distribution(distro_id, package_format)

    required_distros = {
        "opensuse_16.0": "rpm",
        "fedora_44": "rpm",
        "debian_13": "deb",
        "ubuntu_26.04": "deb",
        "arch": "pacman",
    }
    actual_distros = {key: value.package_format for key, value in distributions.items()}
    if actual_distros != required_distros:
        raise PackageBuilderError(
            f"matrix distributions must be exactly {required_distros}, got {actual_distros}"
        )

    pg_rows = raw.get("postgresql_majors")
    if (
        not isinstance(pg_rows, list)
        or not pg_rows
        or any(not isinstance(item, int) or isinstance(item, bool) or item < 17 for item in pg_rows)
        or len(pg_rows) != len(set(pg_rows))
    ):
        raise PackageBuilderError(
            "postgresql_majors must be unique integer majors greater than or equal to 17"
        )
    return Matrix(architectures, distributions, tuple(pg_rows))


def csv_values(values: Sequence[str] | None) -> list[str]:
    result: list[str] = []
    for value in values or ():
        result.extend(part.strip() for part in value.split(",") if part.strip())
    return result


def select_matrix(args: argparse.Namespace, matrix: Matrix) -> Selection:
    architectures = csv_values(getattr(args, "architectures", None)) or list(
        matrix.architectures
    )
    distributions = csv_values(getattr(args, "distros", None)) or list(
        matrix.distributions
    )
    pg_values = csv_values(getattr(args, "postgres", None))
    postgresql_majors: list[int] = []
    try:
        postgresql_majors = (
            [int(value) for value in pg_values]
            if pg_values
            else list(matrix.postgresql_majors)
        )
    except ValueError as error:
        raise PackageBuilderError("--postgres accepts integer major versions") from error

    unknown_arches = sorted(set(architectures) - set(matrix.architectures))
    unknown_distros = sorted(set(distributions) - set(matrix.distributions))
    if unknown_arches:
        raise PackageBuilderError(f"unknown architectures requested: {unknown_arches}")
    if unknown_distros:
        raise PackageBuilderError(f"unknown distributions requested: {unknown_distros}")
    if len(architectures) != len(set(architectures)):
        raise PackageBuilderError("architectures must not be repeated")
    if len(distributions) != len(set(distributions)):
        raise PackageBuilderError("distributions must not be repeated")
    if (
        not postgresql_majors
        or any(major < 17 for major in postgresql_majors)
        or len(postgresql_majors) != len(set(postgresql_majors))
    ):
        raise PackageBuilderError("PostgreSQL majors must be unique and at least 17")
    return Selection(
        tuple(architectures), tuple(distributions), tuple(postgresql_majors)
    )


def tofu_init() -> None:
    run_command(
        [tofu_executable(), "init", "-input=false"], cwd=STACK_DIR
    )


def tofu_outputs() -> dict[str, Any]:
    result = run_command(
        [tofu_executable(), "output", "-json"],
        cwd=STACK_DIR,
        capture=True,
        check=False,
    )
    if result.returncode != 0 or not result.stdout.strip():
        return {}
    try:
        raw = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise PackageBuilderError(f"OpenTofu output is not valid JSON: {error}") from error
    if not isinstance(raw, dict):
        raise PackageBuilderError("OpenTofu output must be a JSON object")
    return {
        key: value.get("value") if isinstance(value, dict) else None
        for key, value in raw.items()
    }


def _walk_state_modules(module: dict[str, Any]) -> Iterable[dict[str, Any]]:
    resources = module.get("resources", [])
    if isinstance(resources, list):
        for resource in resources:
            if isinstance(resource, dict):
                yield resource
    children = module.get("child_modules", [])
    if isinstance(children, list):
        for child in children:
            if isinstance(child, dict):
                yield from _walk_state_modules(child)


def tofu_state_builders(matrix: Matrix) -> dict[str, Builder]:
    """Recover only exact package-builder resources after a partial apply.

    Aggregate outputs can be unknown when one side of the ``for_each`` failed.
    OpenTofu's JSON state still records each instance that did succeed. No
    sensitive state content is printed or persisted by this recovery path.
    """

    result = run_command(
        [tofu_executable(), "show", "-json"],
        cwd=STACK_DIR,
        capture=True,
        check=False,
    )
    if result.returncode != 0:
        local_state_exists = any(
            path.is_file() and path.stat().st_size > 0
            for path in STACK_DIR.glob("*.tfstate*")
        )
        if not local_state_exists:
            return {}
        detail = (result.stderr or result.stdout or "").strip()
        raise PackageBuilderError(
            "cannot inspect non-empty OpenTofu state while recovering builders"
            + (f": {detail}" if detail else "")
        )
    if not result.stdout.strip():
        return {}
    try:
        state = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise PackageBuilderError(f"OpenTofu state JSON is malformed: {error}") from error
    values = state.get("values", {}) if isinstance(state, dict) else {}
    root_module = values.get("root_module", {}) if isinstance(values, dict) else {}
    if not isinstance(root_module, dict):
        return {}
    expected_addresses = {
        f'aws_instance.builder["{name}"]': name for name in matrix.architectures
    }
    builders: dict[str, Builder] = {}
    for resource in _walk_state_modules(root_module):
        address = resource.get("address")
        if address not in expected_addresses:
            continue
        name = expected_addresses[address]
        if name in builders:
            raise PackageBuilderError(
                f"OpenTofu state contains duplicate builder resource for {name}"
            )
        resource_values = resource.get("values", {})
        if not isinstance(resource_values, dict):
            raise PackageBuilderError(
                f"OpenTofu state values for {name} are malformed"
            )
        instance_id = str(resource_values.get("id", ""))
        if not INSTANCE_ID_RE.match(instance_id):
            # A planned/unknown resource is not an actionable instance.
            continue
        if instance_id in PROTECTED_INSTANCE_IDS:
            raise PackageBuilderError(
                f"refusing protected development instance {instance_id} in OpenTofu state"
            )
        architecture = matrix.architectures[name]
        builders[name] = Builder(
            architecture=name,
            instance_id=instance_id,
            instance_type=architecture.instance_type,
            aws_architecture=architecture.aws_architecture,
            public_ip=str(resource_values.get("public_ip") or ""),
            private_ip=str(resource_values.get("private_ip") or ""),
            source_ami_id=str(resource_values.get("ami") or ""),
        )
    return builders


def recover_managed_builders(matrix: Matrix) -> dict[str, Builder]:
    outputs = tofu_outputs()
    if outputs:
        try:
            return output_builders(outputs, matrix)
        except PackageBuilderError as output_error:
            info(f"aggregate builder output unavailable; recovering exact state: {output_error}")
    return tofu_state_builders(matrix)


def guard_local_state_against_protected_instances() -> None:
    """Reject a poisoned/imported local state before OpenTofu can mutate it."""

    for state_path in STACK_DIR.glob("*.tfstate*"):
        if not state_path.is_file():
            continue
        try:
            state_text = state_path.read_text(encoding="utf-8", errors="replace")
        except OSError as error:
            raise PackageBuilderError(
                f"cannot inspect existing OpenTofu state {state_path.name}: {error}"
            ) from error
        protected = sorted(
            instance_id
            for instance_id in PROTECTED_INSTANCE_IDS
            if instance_id in state_text
        )
        if protected:
            raise PackageBuilderError(
                "refusing OpenTofu apply because local state references protected "
                f"development instance(s): {protected}"
            )


def validate_tofu_plan(plan: dict[str, Any]) -> None:
    """Reject any plan that could replace/delete a builder or touch protected IDs."""

    if not isinstance(plan, dict):
        raise PackageBuilderError("OpenTofu plan JSON must be an object")
    plan_text = json.dumps(plan, separators=(",", ":"), sort_keys=True)
    protected = sorted(
        instance_id for instance_id in PROTECTED_INSTANCE_IDS if instance_id in plan_text
    )
    if protected:
        raise PackageBuilderError(
            "refusing OpenTofu plan because it references protected development "
            f"instance(s): {protected}"
        )
    resource_changes = plan.get("resource_changes", [])
    if not isinstance(resource_changes, list):
        raise PackageBuilderError("OpenTofu plan has malformed resource_changes")
    forbidden: list[str] = []
    for resource in resource_changes:
        if not isinstance(resource, dict):
            raise PackageBuilderError("OpenTofu plan contains a malformed resource change")
        address = str(resource.get("address", ""))
        resource_type = str(resource.get("type", ""))
        if resource_type != "aws_instance" and not address.startswith("aws_instance."):
            continue
        change = resource.get("change", {})
        actions = change.get("actions", []) if isinstance(change, dict) else []
        if not isinstance(actions, list) or any(not isinstance(action, str) for action in actions):
            raise PackageBuilderError(
                f"OpenTofu plan has malformed actions for {address or resource_type}"
            )
        if "delete" in actions:
            forbidden.append(f"{address or resource_type}: {actions}")
    if forbidden:
        raise PackageBuilderError(
            "refusing OpenTofu instance deletion/replacement plan: " + "; ".join(forbidden)
        )


def tofu_apply(
    *,
    region: str,
    ssh_cidr: str,
    cached_amis: dict[str, str],
    artifact_bucket_name: str,
    artifact_prefix: str,
) -> dict[str, Any]:
    guard_local_state_against_protected_instances()
    tofu_init()
    variables = {
        "region": region,
        "ssh_ingress_cidr": ssh_cidr,
        "arm64_ami_id": cached_amis.get("arm64", ""),
        "x64_ami_id": cached_amis.get("x64", ""),
        "artifact_bucket_name": artifact_bucket_name,
        "artifact_prefix": artifact_prefix,
        "ami_schema_version": BOOTSTRAP_SCHEMA,
        "root_volume_size_gb": "500",
        "compiler_rt_priority": "1",
    }
    variable_args = [f"-var={key}={value}" for key, value in variables.items()]
    RUNTIME_DIR.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="tofu-package-plan-", dir=RUNTIME_DIR
    ) as plan_directory:
        plan_path = Path(plan_directory) / "package-build.tfplan"
        run_command(
            [
                tofu_executable(),
                "plan",
                "-input=false",
                f"-out={plan_path}",
                *variable_args,
            ],
            cwd=STACK_DIR,
        )
        rendered = run_command(
            [tofu_executable(), "show", "-json", str(plan_path)],
            cwd=STACK_DIR,
            capture=True,
        )
        try:
            plan = json.loads(rendered.stdout)
        except json.JSONDecodeError as error:
            raise PackageBuilderError(
                f"OpenTofu plan output is not valid JSON: {error}"
            ) from error
        validate_tofu_plan(plan)
        run_command(
            [tofu_executable(), "apply", "-input=false", str(plan_path)],
            cwd=STACK_DIR,
        )
    outputs = tofu_outputs()
    if not outputs:
        raise PackageBuilderError("OpenTofu apply completed without readable outputs")
    return outputs


def aws_preflight(region: str) -> dict[str, Any]:
    identity = aws_json(region, "sts", "get-caller-identity")
    account = identity.get("Account")
    arn = identity.get("Arn")
    if not isinstance(account, str) or not re.fullmatch(r"[0-9]{12}", account):
        raise PackageBuilderError("AWS caller identity did not contain a 12-digit account")
    if not isinstance(arn, str) or not arn:
        raise PackageBuilderError("AWS caller identity did not contain an ARN")
    # This is a permission probe as well as a region sanity check.
    aws_json(region, "ec2", "describe-availability-zones", "--filters", "Name=state,Values=available")
    info(f"AWS preflight passed for account {account} in {region} ({arn})")
    return identity


def normalize_s3_prefix(value: str) -> str:
    if (
        not 1 <= len(value) <= 128
        or S3_PREFIX_RE.fullmatch(value) is None
        or value.endswith("/")
        or "//" in value
    ):
        raise PackageBuilderError(
            "S3 prefix must be a 1-128 character relative key prefix without "
            "a trailing or doubled slash"
        )
    return value


def latest_cached_ami(
    region: str,
    architecture: Architecture,
    lock_sha256: str,
    matrix_fingerprint: str,
) -> dict[str, Any] | None:
    if re.fullmatch(r"[0-9a-f]{64}", lock_sha256) is None or re.fullmatch(
        r"[0-9a-f]{64}", matrix_fingerprint
    ) is None:
        raise PackageBuilderError("cached AMI lookup requires complete SHA-256 provenance")
    filters = [
        f"Name=tag:{key},Values={value}" for key, value in CACHE_TAGS.items()
    ]
    filters.extend(
        [
            f"Name=tag:BuildArchitecture,Values={architecture.name}",
            f"Name=tag:Architecture,Values={architecture.aws_architecture}",
            f"Name=tag:BootstrapSchema,Values={BOOTSTRAP_SCHEMA}",
            f"Name=tag:PoetryLockSha256,Values={lock_sha256}",
            f"Name=tag:CompleteMatrixFingerprint,Values={matrix_fingerprint}",
            f"Name=architecture,Values={architecture.aws_architecture}",
            "Name=root-device-type,Values=ebs",
            "Name=virtualization-type,Values=hvm",
            "Name=state,Values=available",
        ]
    )
    payload = aws_json(
        region,
        "ec2",
        "describe-images",
        "--owners",
        "self",
        "--filters",
        *filters,
    )
    images = payload.get("Images", [])
    if not isinstance(images, list):
        raise PackageBuilderError("EC2 describe-images returned malformed Images")
    candidates: list[dict[str, Any]] = []
    for image in images:
        if not isinstance(image, dict) or AMI_ID_RE.match(str(image.get("ImageId", ""))) is None:
            continue
        tags = {tag.get("Key"): tag.get("Value") for tag in image.get("Tags", [])}
        required_tags = {
            **CACHE_TAGS,
            "BuildArchitecture": architecture.name,
            "Architecture": architecture.aws_architecture,
            "BootstrapSchema": BOOTSTRAP_SCHEMA,
            "PoetryLockSha256": lock_sha256,
            "CompleteMatrixFingerprint": matrix_fingerprint,
        }
        if (
            image.get("Architecture") != architecture.aws_architecture
            or image.get("RootDeviceType") != "ebs"
            or image.get("VirtualizationType") != "hvm"
            or any(tags.get(key) != value for key, value in required_tags.items())
        ):
            continue
        candidates.append(image)
    if not candidates:
        return None
    return max(candidates, key=lambda item: str(item.get("CreationDate", "")))


def output_builders(outputs: dict[str, Any], matrix: Matrix) -> dict[str, Builder]:
    raw = outputs.get("builders")
    if not isinstance(raw, dict) or set(raw) != set(matrix.architectures):
        raise PackageBuilderError(
            "OpenTofu output `builders` must contain exactly arm64 and x64"
        )
    builders: dict[str, Builder] = {}
    for architecture, values in raw.items():
        if not isinstance(values, dict):
            raise PackageBuilderError(f"builder output {architecture!r} must be an object")
        instance_id = str(values.get("instance_id", ""))
        if not INSTANCE_ID_RE.match(instance_id):
            raise PackageBuilderError(f"builder {architecture} has invalid instance ID")
        if instance_id in PROTECTED_INSTANCE_IDS:
            raise PackageBuilderError(
                f"refusing protected development instance {instance_id}"
            )
        expected = matrix.architectures[architecture]
        builders[architecture] = Builder(
            architecture=architecture,
            instance_id=instance_id,
            instance_type=str(values.get("instance_type", "")),
            aws_architecture=str(values.get("architecture", "")),
            public_ip=str(values.get("public_ip") or ""),
            private_ip=str(values.get("private_ip") or ""),
            source_ami_id=str(values.get("source_ami_id") or ""),
        )
        if builders[architecture].instance_type != expected.instance_type:
            raise PackageBuilderError(
                f"builder {architecture} output has instance type "
                f"{builders[architecture].instance_type}, expected {expected.instance_type}"
            )
        if builders[architecture].aws_architecture != expected.aws_architecture:
            raise PackageBuilderError(
                f"builder {architecture} output has AWS architecture "
                f"{builders[architecture].aws_architecture}, expected "
                f"{expected.aws_architecture}"
            )
    return builders


def describe_builder(region: str, builder: Builder, expected: Architecture) -> dict[str, Any]:
    if builder.instance_id in PROTECTED_INSTANCE_IDS:
        raise PackageBuilderError(
            f"refusing protected development instance {builder.instance_id}"
        )
    payload = aws_json(
        region,
        "ec2",
        "describe-instances",
        "--instance-ids",
        builder.instance_id,
    )
    instances = [
        instance
        for reservation in payload.get("Reservations", [])
        for instance in reservation.get("Instances", [])
    ]
    if len(instances) != 1:
        raise PackageBuilderError(
            f"expected exactly one EC2 record for {builder.instance_id}, got {len(instances)}"
        )
    instance = instances[0]
    tags = {tag.get("Key"): tag.get("Value") for tag in instance.get("Tags", [])}
    required_tags = {
        **EXPECTED_INSTANCE_TAGS,
        "BuildArchitecture": expected.name,
        "Architecture": expected.aws_architecture,
        "AmiSchemaVersion": BOOTSTRAP_SCHEMA,
    }
    wrong_tags = {
        key: (tags.get(key), value)
        for key, value in required_tags.items()
        if tags.get(key) != value
    }
    if wrong_tags:
        raise PackageBuilderError(
            f"instance {builder.instance_id} failed ownership-tag validation: {wrong_tags}"
        )
    if instance.get("InstanceType") != expected.instance_type:
        raise PackageBuilderError(
            f"instance {builder.instance_id} type is {instance.get('InstanceType')}, "
            f"expected {expected.instance_type}"
        )
    if instance.get("Architecture") != expected.aws_architecture:
        raise PackageBuilderError(
            f"instance {builder.instance_id} architecture is {instance.get('Architecture')}, "
            f"expected {expected.aws_architecture}"
        )
    root_name = instance.get("RootDeviceName")
    root_mappings = [
        mapping
        for mapping in instance.get("BlockDeviceMappings", [])
        if mapping.get("DeviceName") == root_name
    ]
    if len(root_mappings) != 1 or not root_mappings[0].get("Ebs", {}).get("VolumeId"):
        raise PackageBuilderError(f"instance {builder.instance_id} has no unique EBS root")
    volume_id = root_mappings[0]["Ebs"]["VolumeId"]
    volumes = aws_json(
        region, "ec2", "describe-volumes", "--volume-ids", volume_id
    ).get("Volumes", [])
    if len(volumes) != 1:
        raise PackageBuilderError(f"could not validate root volume {volume_id}")
    volume = volumes[0]
    if volume.get("Size", 0) < 500 or volume.get("Encrypted") is not True:
        raise PackageBuilderError(
            f"builder root {volume_id} must be encrypted and at least 500 GiB"
        )
    termination = aws_json(
        region,
        "ec2",
        "describe-instance-attribute",
        "--instance-id",
        builder.instance_id,
        "--attribute",
        "disableApiTermination",
    )
    if termination.get("DisableApiTermination", {}).get("Value") is not True:
        raise PackageBuilderError(
            f"instance {builder.instance_id} does not have API termination protection"
        )
    stop_protection = aws_json(
        region,
        "ec2",
        "describe-instance-attribute",
        "--instance-id",
        builder.instance_id,
        "--attribute",
        "disableApiStop",
    )
    if stop_protection.get("DisableApiStop", {}).get("Value") is not True:
        raise PackageBuilderError(
            f"instance {builder.instance_id} does not have API stop protection"
        )
    shutdown = aws_json(
        region,
        "ec2",
        "describe-instance-attribute",
        "--instance-id",
        builder.instance_id,
        "--attribute",
        "instanceInitiatedShutdownBehavior",
    )
    if shutdown.get("InstanceInitiatedShutdownBehavior", {}).get("Value") != "stop":
        raise PackageBuilderError(
            f"instance {builder.instance_id} shutdown behavior is not stop"
        )
    return instance


def validate_builders(
    region: str, builders: dict[str, Builder], matrix: Matrix
) -> dict[str, dict[str, Any]]:
    return {
        name: describe_builder(region, builder, matrix.architectures[name])
        for name, builder in builders.items()
    }


def instance_state(instance: dict[str, Any]) -> str:
    return str(instance.get("State", {}).get("Name", "unknown"))


def start_builders(
    region: str, builders: dict[str, Builder], matrix: Matrix
) -> dict[str, dict[str, Any]]:
    records = validate_builders(region, builders, matrix)
    for name, builder in builders.items():
        state = instance_state(records[name])
        if state == "running":
            info(f"{name} builder {builder.instance_id} is already running; no-op")
            continue
        if state == "stopping":
            run_command(
                [
                    require_executable("aws"),
                    "--region",
                    region,
                    "ec2",
                    "wait",
                    "instance-stopped",
                    "--instance-ids",
                    builder.instance_id,
                ]
            )
            state = "stopped"
        if state == "stopped":
            aws_no_output(
                region, "ec2", "start-instances", "--instance-ids", builder.instance_id
            )
        elif state != "pending":
            raise PackageBuilderError(
                f"cannot start {name} builder {builder.instance_id} from state {state!r}"
            )

    def wait_until_healthy(builder: Builder) -> None:
        run_command(
            [
                require_executable("aws"),
                "--region",
                region,
                "ec2",
                "wait",
                "instance-running",
                "--instance-ids",
                builder.instance_id,
            ]
        )
        run_command(
            [
                require_executable("aws"),
                "--region",
                region,
                "ec2",
                "wait",
                "instance-status-ok",
                "--instance-ids",
                builder.instance_id,
            ]
        )

    with concurrent.futures.ThreadPoolExecutor(
        max_workers=len(builders), thread_name_prefix="bioseq-builder-start"
    ) as executor:
        futures = {
            executor.submit(wait_until_healthy, builder): name
            for name, builder in builders.items()
        }
        for future in concurrent.futures.as_completed(futures):
            future.result()
    return validate_builders(region, builders, matrix)


def refreshed_builders(
    records: dict[str, dict[str, Any]], builders: dict[str, Builder]
) -> dict[str, Builder]:
    refreshed: dict[str, Builder] = {}
    for name, builder in builders.items():
        instance = records[name]
        refreshed[name] = Builder(
            architecture=name,
            instance_id=builder.instance_id,
            instance_type=builder.instance_type,
            aws_architecture=builder.aws_architecture,
            public_ip=str(instance.get("PublicIpAddress") or ""),
            private_ip=str(instance.get("PrivateIpAddress") or ""),
            source_ami_id=builder.source_ami_id,
        )
        if not refreshed[name].public_ip:
            raise PackageBuilderError(
                f"running builder {builder.instance_id} has no public IPv4 address"
            )
    return refreshed


def ssh_key_path(outputs: dict[str, Any]) -> Path:
    value = outputs.get("ssh_private_key_path")
    if not isinstance(value, str) or not value:
        raise PackageBuilderError("OpenTofu output `ssh_private_key_path` is missing")
    path = Path(value)
    if not path.is_absolute():
        path = (STACK_DIR / path).resolve()
    if not path.is_file():
        raise PackageBuilderError(f"SSH private key does not exist: {path}")
    path.chmod(0o600)
    return path


def ssh_argv(builder: Builder, key_path: Path) -> list[str]:
    if not builder.public_ip:
        raise PackageBuilderError(f"builder {builder.instance_id} has no public IP")
    return [
        require_executable("ssh"),
        "-i",
        str(key_path),
        "-o",
        "BatchMode=yes",
        "-o",
        "ConnectTimeout=15",
        "-o",
        "ServerAliveInterval=30",
        "-o",
        "ServerAliveCountMax=10",
        "-o",
        "StrictHostKeyChecking=accept-new",
        f"ubuntu@{builder.public_ip}",
    ]


def ssh_run(
    builder: Builder,
    key_path: Path,
    remote_command: str,
    *,
    capture: bool = False,
    check: bool = True,
    stdout: Any = None,
) -> subprocess.CompletedProcess[str]:
    return run_command(
        ssh_argv(builder, key_path) + [remote_command],
        capture=capture,
        check=check,
        stdout=stdout,
    )


def wait_for_ssh(builder: Builder, key_path: Path, timeout_seconds: int = 1200) -> None:
    deadline = time.monotonic() + timeout_seconds
    last_error = ""
    while time.monotonic() < deadline:
        result = ssh_run(
            builder,
            key_path,
            "true",
            capture=True,
            check=False,
        )
        if result.returncode == 0:
            info(f"SSH is ready on {builder.architecture} ({builder.public_ip})")
            return
        last_error = (result.stderr or result.stdout or "").strip()
        time.sleep(10)
    raise PackageBuilderError(
        f"SSH did not become ready on {builder.instance_id}: {last_error}"
    )


def wait_for_bootstrap(builder: Builder, key_path: Path) -> None:
    wait_for_ssh(builder, key_path)
    # A repaired, idempotently resumed bootstrap can be READY even though
    # cloud-init retains the original first-boot error forever. Skip that stale
    # status only when the stronger on-host READY marker is already present;
    # otherwise cloud-init remains the strict first-boot gate.
    ready_probe = ssh_run(
        builder,
        key_path,
        "test \"$(cat /opt/bioseq-package-builder/PROVISION_STATUS 2>/dev/null)\" = READY",
        capture=True,
        check=False,
    )
    if ready_probe.returncode != 0:
        ssh_run(builder, key_path, "sudo cloud-init status --wait --long")
    expected = shlex.quote(builder.architecture)
    verification = (
        "set -eu; "
        "test \"$(cat /opt/bioseq-package-builder/PROVISION_STATUS)\" = READY; "
        "test -s /opt/bioseq-package-builder/READY; "
        f"grep -Fqx 'architecture={expected}' /opt/bioseq-package-builder/READY; "
        f"grep -Fqx 'ami_schema_version={BOOTSTRAP_SCHEMA}' /opt/bioseq-package-builder/READY; "
        "sudo /usr/local/sbin/bioseq-package-builder-verify; "
        "test -x /usr/local/bin/bioseq-compiler-rt-launcher; "
        "docker info >/dev/null"
    )
    ssh_run(builder, key_path, verification)
    info(f"bootstrap invariants passed on {builder.architecture}")


def reconcile_remote_bootstrap(
    builder: Builder, key_path: Path, remote_source: str
) -> None:
    """Reconcile schema-2 bootstrap in place without rebooting or stopping."""

    wait_for_ssh(builder, key_path)
    ssh_run(
        builder,
        key_path,
        "for attempt in $(seq 1 720); do "
        "if ! pgrep -f '[b]ioseq-package-builder-bootstrap' >/dev/null; then exit 0; fi; "
        "sleep 5; done; exit 1",
    )
    ready = ssh_run(
        builder,
        key_path,
        "set -eu; "
        "test \"$(cat /opt/bioseq-package-builder/PROVISION_STATUS 2>/dev/null)\" = READY; "
        f"grep -Fqx 'ami_schema_version={BOOTSTRAP_SCHEMA}' "
        "/opt/bioseq-package-builder/READY; "
        "sudo /usr/local/sbin/bioseq-package-builder-verify",
        capture=True,
        check=False,
    )
    if ready.returncode == 0:
        info(f"schema-{BOOTSTRAP_SCHEMA} bootstrap already reconciled on {builder.architecture}")
        return
    source_bootstrap = (
        f"{remote_source}/main/infra/package_build_infra/scripts/bootstrap.sh"
    )
    command = (
        "set -Eeuo pipefail; "
        f"test -f {shlex.quote(source_bootstrap)}; "
        f"sudo install -o root -g root -m 0755 {shlex.quote(source_bootstrap)} "
        "/usr/local/sbin/bioseq-package-builder-bootstrap; "
        "sudo sed -i -E "
        f"\"s/^BIOSEQ_AMI_SCHEMA_VERSION=.*/BIOSEQ_AMI_SCHEMA_VERSION='{BOOTSTRAP_SCHEMA}'/\" "
        "/etc/bioseq-package-builder.env; "
        f"grep -Fqx \"BIOSEQ_AMI_SCHEMA_VERSION='{BOOTSTRAP_SCHEMA}'\" "
        "/etc/bioseq-package-builder.env; "
        "sudo /usr/local/sbin/bioseq-package-builder-bootstrap"
    )
    ssh_run(builder, key_path, f"bash -lc {shlex.quote(command)}")
    wait_for_bootstrap(builder, key_path)


def create_cached_ami(
    region: str,
    builder: Builder,
    architecture: Architecture,
    key_path: Path,
    revision: str,
    lock_sha256: str,
    matrix_fingerprint: str,
    unit_name: str,
) -> dict[str, Any]:
    """Create a no-reboot image from a quiescent, still-running builder."""

    require_successful_remote_unit(builder, key_path, unit_name)
    record = describe_builder(region, builder, architecture)
    if instance_state(record) != "running":
        raise PackageBuilderError(
            f"refusing to image {builder.instance_id} from non-running state "
            f"{instance_state(record)!r}"
        )
    ssh_run(builder, key_path, "sync")
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d-%H%M%S")
    name = (
        f"bioseq-package-builder-{architecture.name}-schema{BOOTSTRAP_SCHEMA}-"
        f"{matrix_fingerprint[:12]}-{stamp}"
    )
    payload = aws_json(
        region,
        "ec2",
        "create-image",
        "--instance-id",
        builder.instance_id,
        "--name",
        name,
        "--description",
        f"BioSeq complete post-build package cache {architecture.name}, revision {revision}",
        "--no-reboot",
    )
    image_id = str(payload.get("ImageId", ""))
    if not AMI_ID_RE.match(image_id):
        raise PackageBuilderError("EC2 create-image returned an invalid AMI id")
    tags = {
        **CACHE_TAGS,
        "BuildArchitecture": architecture.name,
        "Architecture": architecture.aws_architecture,
        "BootstrapSchema": BOOTSTRAP_SCHEMA,
        "SourceRevision": revision,
        "SourceInstanceId": builder.instance_id,
        "PoetryLockSha256": lock_sha256,
        "CompleteMatrixFingerprint": matrix_fingerprint,
        "WorkerGeneration": WORKER_GENERATION,
        "Name": name,
    }
    aws_no_output(
        region,
        "ec2",
        "create-tags",
        "--resources",
        image_id,
        "--tags",
        *(f"Key={key},Value={value}" for key, value in tags.items()),
    )
    image = wait_for_cached_ami(region, image_id)
    image_tags = {
        tag.get("Key"): tag.get("Value")
        for tag in image.get("Tags", [])
        if isinstance(tag, dict)
    }
    required_image_tags = {
        **CACHE_TAGS,
        "BuildArchitecture": architecture.name,
        "Architecture": architecture.aws_architecture,
        "BootstrapSchema": BOOTSTRAP_SCHEMA,
        "SourceRevision": revision,
        "SourceInstanceId": builder.instance_id,
        "PoetryLockSha256": lock_sha256,
        "CompleteMatrixFingerprint": matrix_fingerprint,
        "WorkerGeneration": WORKER_GENERATION,
        "Name": name,
    }
    if (
        image.get("Architecture") != architecture.aws_architecture
        or image.get("RootDeviceType") != "ebs"
        or image.get("VirtualizationType") != "hvm"
        or any(image_tags.get(key) != value for key, value in required_image_tags.items())
    ):
        raise PackageBuilderError(
            f"created AMI {image_id} failed architecture/provenance validation"
        )
    info(f"created reusable post-build {architecture.name} package-builder AMI {image_id}")
    return image


def wait_for_cached_ami(
    region: str, image_id: str, timeout_seconds: int = 7200
) -> dict[str, Any]:
    """Poll a large AMI beyond the AWS waiter's fixed ten-minute ceiling."""

    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        result = aws_json(region, "ec2", "describe-images", "--image-ids", image_id)
        images = result.get("Images", [])
        if len(images) != 1:
            raise PackageBuilderError(f"EC2 did not return cached AMI {image_id}")
        image = images[0]
        state = image.get("State")
        if state == "available":
            return image
        if state == "failed":
            reason = image.get("StateReason", {}).get("Message", "unknown reason")
            raise PackageBuilderError(f"cached AMI {image_id} failed: {reason}")
        if state != "pending":
            raise PackageBuilderError(
                f"cached AMI {image_id} entered unexpected state {state!r}"
            )
        time.sleep(15)
    raise PackageBuilderError(
        f"cached AMI {image_id} did not become available within "
        f"{timeout_seconds // 60} minutes"
    )


def commit_id() -> str:
    result = run_command(
        ["git", "rev-parse", "--verify", "HEAD"],
        cwd=REPO_ROOT,
        capture=True,
    )
    value = result.stdout.strip()
    if not re.fullmatch(r"[0-9a-f]{40}", value):
        raise PackageBuilderError("could not resolve a full committed Git HEAD")
    return value


def committed_poetry_lock_sha256(revision: str) -> str:
    if re.fullmatch(r"[0-9a-f]{40}", revision) is None:
        raise PackageBuilderError("Poetry lock provenance requires a full Git revision")
    relative = POETRY_LOCK_FILE.relative_to(REPO_ROOT).as_posix()
    result = run_command(
        ["git", "show", f"{revision}:{relative}"],
        cwd=REPO_ROOT,
        capture=True,
    )
    if not result.stdout:
        raise PackageBuilderError("committed Poetry lockfile is empty")
    return hashlib.sha256(result.stdout.encode("utf-8")).hexdigest()


def require_committed_paths(paths: Iterable[Path]) -> None:
    for path in paths:
        try:
            relative = path.resolve().relative_to(REPO_ROOT)
        except ValueError as error:
            raise PackageBuilderError(f"path is outside repository: {path}") from error
        result = run_command(
            ["git", "cat-file", "-e", f"HEAD:{relative.as_posix()}"],
            cwd=REPO_ROOT,
            capture=True,
            check=False,
        )
        if result.returncode != 0:
            raise PackageBuilderError(
                f"required build input is not committed at HEAD: {relative}"
            )


def sync_committed_source(builder: Builder, key_path: Path, revision: str) -> str:
    remote_source = f"/opt/bioseq-package-builder/workspace/source-{revision}"
    ssh_run(
        builder,
        key_path,
        f"install -d -m 0755 {shlex.quote(remote_source)}",
    )
    archive_argv = ["git", "archive", "--format=tar", revision]
    extract_argv = ssh_argv(builder, key_path) + [
        f"tar -xf - -C {shlex.quote(remote_source)}"
    ]
    info(
        f"streaming committed source {revision} to {builder.architecture} "
        f"at {remote_source}"
    )
    archive = subprocess.Popen(
        archive_argv,
        cwd=str(REPO_ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert archive.stdout is not None
    extract = subprocess.Popen(extract_argv, stdin=archive.stdout)
    archive.stdout.close()
    extract_return = extract.wait()
    _, archive_error = archive.communicate()
    if archive.returncode != 0 or extract_return != 0:
        raise PackageBuilderError(
            f"source transfer to {builder.architecture} failed: "
            f"git={archive.returncode}, ssh={extract_return}, "
            f"detail={archive_error.decode(errors='replace').strip()}"
        )
    marker = hashlib.sha256(revision.encode("ascii")).hexdigest()
    ssh_run(
        builder,
        key_path,
        "set -eu; "
        f"test -f {shlex.quote(remote_source + '/main/CMakeLists.txt')}; "
        f"printf '%s\\n' {shlex.quote(revision)} > {shlex.quote(remote_source + '/.bioseq-source-revision')}; "
        f"printf '%s\\n' {shlex.quote(marker)} > {shlex.quote(remote_source + '/.bioseq-source-marker')}",
    )
    return remote_source


def prepare_remote_poetry_environment(
    builder: Builder,
    key_path: Path,
    remote_source: str,
    revision: str,
    lock_sha256: str,
) -> PoetryEnvironment:
    """Verify committed provenance and atomically prepare a lock-keyed venv."""

    if re.fullmatch(r"[0-9a-f]{40}", revision) is None:
        raise PackageBuilderError("remote Poetry setup requires a full Git revision")
    if re.fullmatch(r"[0-9a-f]{64}", lock_sha256) is None:
        raise PackageBuilderError("remote Poetry setup requires a SHA-256 lock hash")
    packaging_dir = f"{remote_source}/main/infra/packaging"
    venv_path = f"{REMOTE_POETRY_VENV_ROOT}/{lock_sha256}"
    sentinel_name = ".bioseq-poetry-environment.json"
    source_q = shlex.quote(remote_source)
    packaging_q = shlex.quote(packaging_dir)
    revision_q = shlex.quote(revision)
    lock_q = shlex.quote(lock_sha256)
    cache_q = shlex.quote(REMOTE_POETRY_VENV_ROOT)
    venv_q = shlex.quote(venv_path)
    sentinel_q = shlex.quote(f"{venv_path}/{sentinel_name}")
    setup = f"""set -Eeuo pipefail
test \"$(cat {source_q}/.bioseq-source-revision)\" = {revision_q}
test -f {packaging_q}/pyproject.toml
test -f {packaging_q}/poetry.lock
actual_lock=$(sha256sum {packaging_q}/poetry.lock | awk '{{print $1}}')
test \"$actual_lock\" = {lock_q}
install -d -m 0755 {cache_q}
exec 9>{cache_q}/.{lock_sha256}.lock
flock 9
valid=0
if [[ -x {venv_q}/bin/python && -s {sentinel_q} ]]; then
  if {venv_q}/bin/python -c 'import json,sys; value=json.load(open(sys.argv[1], encoding="utf-8")); raise SystemExit(0 if value.get("lock_sha256") == sys.argv[2] and value.get("python") and value.get("poetry") else 1)' {sentinel_q} {lock_q} && \
     {venv_q}/bin/python -c 'import liquid'; then
    valid=1
  fi
fi
if [[ $valid -eq 0 ]]; then
  if [[ -e {venv_q} ]]; then
    mv -- {venv_q} {venv_q}.invalid.$(date -u +%Y%m%dT%H%M%SZ).$$
  fi
  temporary={venv_q}.tmp.$$
  trap 'rm -rf -- "$temporary"' EXIT
  rm -rf -- "$temporary"
  python3 -m venv "$temporary"
  VIRTUAL_ENV="$temporary" \
    PATH="$temporary/bin:$PATH" \
    POETRY_VIRTUALENVS_CREATE=false \
    poetry install --directory {packaging_q} --only main --no-root --sync --no-interaction --no-ansi
  "$temporary/bin/python" -c 'import liquid'
  python_version=$("$temporary/bin/python" -c 'import platform; print(platform.python_version())')
  poetry_version=$(poetry --version --no-ansi | sed -n 's/^Poetry (version \\(.*\\))$/\\1/p')
  test -n "$python_version"
  test -n "$poetry_version"
  "$temporary/bin/python" -c 'import json,os,sys; path=sys.argv[1]; value={{"lock_sha256":sys.argv[2],"python":sys.argv[3],"poetry":sys.argv[4]}}; handle=open(path,"w",encoding="utf-8"); json.dump(value,handle,sort_keys=True); handle.write("\\n"); handle.flush(); os.fsync(handle.fileno()); handle.close(); descriptor=os.open(os.path.dirname(path),os.O_RDONLY); os.fsync(descriptor); os.close(descriptor)' "$temporary/{sentinel_name}" {lock_q} "$python_version" "$poetry_version"
  mv -- "$temporary" {venv_q}
  trap - EXIT
  python3 -c 'import os,sys; descriptor=os.open(sys.argv[1],os.O_RDONLY); os.fsync(descriptor); os.close(descriptor)' {cache_q}
fi
flock -u 9
"""
    ssh_run(builder, key_path, f"bash -lc {shlex.quote(setup)}")
    sentinel_result = ssh_run(
        builder,
        key_path,
        f"cat {sentinel_q}",
        capture=True,
    )
    try:
        sentinel = json.loads(sentinel_result.stdout)
    except json.JSONDecodeError as error:
        raise PackageBuilderError(
            f"{builder.architecture} Poetry environment sentinel is invalid JSON"
        ) from error
    if not isinstance(sentinel, dict) or sentinel.get("lock_sha256") != lock_sha256:
        raise PackageBuilderError(
            f"{builder.architecture} Poetry environment lock provenance mismatch"
        )
    python_version = str(sentinel.get("python", ""))
    poetry_version = str(sentinel.get("poetry", ""))
    if re.fullmatch(r"3\.12\.\d+", python_version) is None:
        raise PackageBuilderError(
            f"{builder.architecture} Poetry environment has unsupported Python "
            f"{python_version!r}"
        )
    if re.fullmatch(r"1\.8\.\d+", poetry_version) is None:
        raise PackageBuilderError(
            f"{builder.architecture} Poetry environment has unsupported Poetry "
            f"{poetry_version!r}"
        )
    return PoetryEnvironment(lock_sha256, venv_path, python_version, poetry_version)


def initialize_signing_key(
    builder: Builder,
    key_path: Path,
    remote_source: str,
    region: str,
    secret_arn: str,
) -> None:
    script = f"{remote_source}/main/infra/initialize_package_signing_key.py"
    command = command_text(
        [
            "python3",
            script,
            "--region",
            region,
            "--secret-arn",
            secret_arn,
        ]
    )
    ssh_run(builder, key_path, command)


def remote_unit_status(
    builder: Builder, key_path: Path, unit_name: str
) -> dict[str, str]:
    if re.fullmatch(r"[A-Za-z0-9_.@-]+\.service", unit_name) is None:
        raise PackageBuilderError(f"invalid remote systemd unit name {unit_name!r}")
    result = ssh_run(
        builder,
        key_path,
        command_text(
            [
                "sudo",
                "systemctl",
                "show",
                unit_name,
                "--no-pager",
                "--property=LoadState",
                "--property=ActiveState",
                "--property=SubState",
                "--property=Result",
                "--property=ExecMainStatus",
            ]
        ),
        capture=True,
    )
    status: dict[str, str] = {}
    for line in result.stdout.splitlines():
        key, separator, value = line.partition("=")
        if separator:
            status[key] = value
    required = {"LoadState", "ActiveState", "SubState", "Result", "ExecMainStatus"}
    if set(status) != required:
        raise PackageBuilderError(
            f"{builder.architecture} systemd unit {unit_name} returned malformed "
            f"status fields {sorted(status)}"
        )
    return status


def require_successful_remote_unit(
    builder: Builder, key_path: Path, unit_name: str
) -> dict[str, str]:
    status = remote_unit_status(builder, key_path, unit_name)
    if (
        status.get("LoadState") != "loaded"
        or status.get("ActiveState") != "inactive"
        or status.get("Result") != "success"
        or status.get("ExecMainStatus") != "0"
    ):
        raise PackageBuilderError(
            f"{builder.architecture} systemd unit {unit_name} is not a completed "
            f"success: {status}"
        )
    return status


def launch_remote_build(
    *,
    builder: Builder,
    key_path: Path,
    remote_source: str,
    revision: str,
    poetry_environment: PoetryEnvironment,
    selection: Selection,
    region: str,
    bucket: str,
    s3_prefix: str,
    secret_arn: str,
    run_id: str,
) -> RemoteBuildHandle:
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    log_path = LOG_DIR / f"{run_id}-{builder.architecture}.log"
    manifest_path = f"{REMOTE_ROOT}/{run_id}-{builder.architecture}-manifest.json"
    build_root = f"{REMOTE_ROOT}/{run_id}-{builder.architecture}"
    packaging_dir = f"{remote_source}/main/infra/packaging"
    safe_run_id = re.sub(r"[^A-Za-z0-9_.-]", "-", run_id)
    unit_name = f"bioseq-package-{safe_run_id}-{builder.architecture}.service"
    remote_log_path = f"{build_root}/build.log"
    worker_argv = [
        f"{poetry_environment.venv_path}/bin/python",
        f"{packaging_dir}/build_packages.py",
        "--architecture",
        builder.architecture,
        "--bucket",
        bucket,
        "--region",
        region,
        "--source-dir",
        f"{remote_source}/main",
        "--build-root",
        build_root,
        "--manifest",
        manifest_path,
        "--s3-prefix",
        s3_prefix,
        "--signing-secret-arn",
        secret_arn,
        "--postgres",
        ",".join(str(value) for value in selection.postgresql_majors),
        "--distros",
        ",".join(selection.distributions),
        "--source-revision",
        revision,
        "--poetry-lock-sha256",
        poetry_environment.lock_sha256,
        "--fail-fast",
    ]
    systemd_argv = [
        "sudo",
        "systemd-run",
        "--unit",
        unit_name,
        "--property=Type=exec",
        "--property=User=ubuntu",
        "--property=Group=ubuntu",
        f"--property=WorkingDirectory={packaging_dir}",
        f"--property=StandardOutput=append:{remote_log_path}",
        f"--property=StandardError=append:{remote_log_path}",
        "--",
        "/usr/bin/env",
        "AWS_DEFAULT_REGION=" + region,
        "BIOSEQ_OPTTIMIZATION_LEVEL=0",
        "BIOSEQ_PACKAGE_PARALLELISM=4",
        f"BIOSEQ_POETRY_VERSION={poetry_environment.poetry_version}",
        "BIOSEQ_COMPILER_RT_LAUNCHER=/usr/local/bin/bioseq-compiler-rt-launcher",
        "CMAKE_C_COMPILER_LAUNCHER=/usr/local/bin/bioseq-compiler-rt-launcher",
        "CMAKE_CXX_COMPILER_LAUNCHER=/usr/local/bin/bioseq-compiler-rt-launcher",
        "HOME=/home/ubuntu",
        "VCPKG_MAX_CONCURRENCY=4",
        f"PYTHONPATH={packaging_dir}",
        f"VIRTUAL_ENV={poetry_environment.venv_path}",
        (
            f"PATH={poetry_environment.venv_path}/bin:/usr/local/sbin:"
            "/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
        ),
        *worker_argv,
    ]
    source_check = (
        "set -Eeuo pipefail; "
        f"test \"$(cat {shlex.quote(remote_source + '/.bioseq-source-revision')})\" "
        f"= {shlex.quote(revision)}; "
        f"test \"$(sha256sum {shlex.quote(packaging_dir + '/poetry.lock')} | "
        f"awk '{{print $1}}')\" = {shlex.quote(poetry_environment.lock_sha256)}; "
        f"install -d -m 0755 {shlex.quote(build_root)}; "
        f"{command_text(systemd_argv)}"
    )
    info(f"{builder.architecture} build log: {log_path}")
    ssh_run(builder, key_path, f"bash -lc {shlex.quote(source_check)}")
    return RemoteBuildHandle(
        builder=builder,
        unit_name=unit_name,
        remote_log_path=remote_log_path,
        manifest_path=manifest_path,
        build_root=build_root,
        local_log_path=log_path,
    )


def wait_remote_build(
    handle: RemoteBuildHandle,
    key_path: Path,
    *,
    timeout_seconds: int = 24 * 60 * 60,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_seconds
    terminal_status: dict[str, str] | None = None
    while time.monotonic() < deadline:
        status = remote_unit_status(handle.builder, key_path, handle.unit_name)
        if status["ActiveState"] in {"active", "activating", "reloading", "deactivating"}:
            time.sleep(15)
            continue
        terminal_status = status
        break
    if terminal_status is None:
        raise PackageBuilderError(
            f"{handle.builder.architecture} systemd unit {handle.unit_name} did not "
            f"finish within {timeout_seconds // 3600} hours"
        )

    with handle.local_log_path.open("w", encoding="utf-8") as log:
        ssh_run(
            handle.builder,
            key_path,
            f"cat {shlex.quote(handle.remote_log_path)}",
            stdout=log,
            check=False,
        )
    manifest_result = ssh_run(
        handle.builder,
        key_path,
        f"cat {shlex.quote(handle.manifest_path)}",
        capture=True,
        check=False,
    )
    if not manifest_result.stdout.strip():
        raise PackageBuilderError(
            f"{handle.builder.architecture} produced no manifest; unit status "
            f"{terminal_status}; see {handle.local_log_path}"
        )
    try:
        manifest = json.loads(manifest_result.stdout)
    except json.JSONDecodeError as error:
        raise PackageBuilderError(
            f"{handle.builder.architecture} manifest is invalid JSON: {error}"
        ) from error
    if (
        terminal_status.get("LoadState") != "loaded"
        or terminal_status.get("ActiveState") != "inactive"
        or terminal_status.get("Result") != "success"
        or terminal_status.get("ExecMainStatus") != "0"
    ):
        raise PackageBuilderError(
            f"{handle.builder.architecture} package build failed with unit status "
            f"{terminal_status}; see {handle.local_log_path}; "
            f"manifest={manifest.get('status')}"
        )
    return manifest


def expected_artifact_suffix(package_format: str) -> str:
    return {
        "deb": ".deb",
        "rpm": ".rpm",
        "pacman": ".pkg.tar.zst",
    }[package_format]


def require_smoke_attestation(
    smoke: object,
    *,
    architecture: str,
    postgres_major: int,
    distro: str,
    label: str,
) -> None:
    required_true = (
        "passed",
        "package_installed",
        "preload_active",
        "bioseq_extension_created",
        "bioseq_cache_extension_created",
        "nonempty_roundtrip",
        "fresh_container",
    )
    if not isinstance(smoke, dict) or any(smoke.get(name) is not True for name in required_true):
        raise PackageBuilderError(f"{label} did not pass every assertion: {smoke}")
    if (
        smoke.get("architecture") != architecture
        or smoke.get("postgres_major") != postgres_major
        or smoke.get("distro") != distro
        or smoke.get("roundtrip") != "ACGT"
        or not isinstance(smoke.get("container_image"), str)
        or not smoke["container_image"].strip()
    ):
        raise PackageBuilderError(f"{label} attested the wrong matrix cell: {smoke}")


def validate_manifest(
    manifest: dict[str, Any],
    *,
    architecture: str,
    selection: Selection,
    matrix: Matrix,
    bucket: str,
    region: str,
    s3_prefix: str,
    source_revision: str,
    poetry_lock_sha256: str,
    python_environment: PoetryEnvironment,
) -> list[dict[str, Any]]:
    if manifest.get("schema_version") != 1 or manifest.get("status") != "success":
        raise PackageBuilderError(
            f"{architecture} manifest did not report schema 1 success"
        )
    if manifest.get("package_version") != EXPECTED_PACKAGE_VERSION:
        raise PackageBuilderError(f"{architecture} manifest package version mismatch")
    if manifest.get("architecture") != architecture:
        raise PackageBuilderError(f"{architecture} manifest architecture mismatch")
    if manifest.get("source_revision") != source_revision:
        raise PackageBuilderError(f"{architecture} manifest source revision mismatch")
    if manifest.get("poetry_lock_sha256") != poetry_lock_sha256:
        raise PackageBuilderError(f"{architecture} manifest Poetry lock mismatch")
    if manifest.get("python_environment") != python_environment.manifest_value():
        raise PackageBuilderError(f"{architecture} manifest Python environment mismatch")
    if manifest.get("errors") != []:
        raise PackageBuilderError(f"{architecture} successful manifest contains errors")
    requested = manifest.get("requested", {})
    requested_pg = requested.get("postgres", requested.get("postgresql_majors", []))
    if sorted(requested_pg) != sorted(selection.postgresql_majors):
        raise PackageBuilderError(f"{architecture} manifest PostgreSQL selection mismatch")
    if sorted(requested.get("distros", [])) != sorted(selection.distributions):
        raise PackageBuilderError(f"{architecture} manifest distro selection mismatch")
    s3 = manifest.get("s3", {})
    if (
        s3.get("bucket") != bucket
        or s3.get("region") != region
        or s3.get("base_prefix") != s3_prefix
    ):
        raise PackageBuilderError(f"{architecture} manifest S3 contract mismatch")
    signing = manifest.get("signing", {})
    signing_fingerprint = str(signing.get("fingerprint", ""))
    if signing_fingerprint != EXPECTED_SIGNING_FINGERPRINT:
        raise PackageBuilderError(
            f"{architecture} manifest signing-key fingerprint is not the pinned key"
        )

    cells = manifest.get("cells")
    expected_cells = {
        (major, distro)
        for major in selection.postgresql_majors
        for distro in selection.distributions
    }
    if not isinstance(cells, list) or len(cells) != len(expected_cells):
        raise PackageBuilderError(
            f"{architecture} manifest has {len(cells) if isinstance(cells, list) else 'invalid'} "
            f"cells; expected {len(expected_cells)}"
        )
    seen: set[tuple[int, str]] = set()
    uploads: list[dict[str, Any]] = []
    for cell in cells:
        key = (cell.get("postgres_major"), cell.get("distro"))
        if key not in expected_cells or key in seen:
            raise PackageBuilderError(
                f"{architecture} manifest has unexpected or duplicate cell {key}"
            )
        seen.add(key)
        if cell.get("status") != "uploaded" or cell.get("error") not in (None, ""):
            raise PackageBuilderError(f"{architecture} cell {key} was not uploaded")
        if cell.get("package_name") != f"bioseq-postgresql{key[0]}":
            raise PackageBuilderError(f"{architecture} cell {key} package name mismatch")
        require_smoke_attestation(
            cell.get("smoke"),
            architecture=architecture,
            postgres_major=key[0],
            distro=key[1],
            label=f"{architecture} cell {key} built-package smoke",
        )
        validation = cell.get("validation", {})
        required_validation = {
            "passed": True,
            "six_artifacts": True,
            "rpath_absent": True,
            "compiler_runtime_safe": True,
            "optimization_level": 0,
        }
        if any(validation.get(name) != value for name, value in required_validation.items()):
            raise PackageBuilderError(
                f"{architecture} cell {key} failed validation: {validation}"
            )
        expected_elf = matrix.architectures[architecture].artifact_architecture
        elf_aliases = {
            "arm64": {"aarch64", "arm64"},
            "x64": {"x86_64", "x86-64", "advanced micro devices x86-64"},
        }
        binaries = validation.get("binaries")
        if not isinstance(binaries, list) or len(binaries) != 2:
            raise PackageBuilderError(
                f"{architecture} cell {key} must attest exactly two extension DSOs"
            )
        binary_names = {
            Path(str(binary.get("path", ""))).name
            for binary in binaries
            if isinstance(binary, dict)
        }
        if binary_names != {"bioseq.so", "bioseq_cache.so"}:
            raise PackageBuilderError(
                f"{architecture} cell {key} binary attestations are {sorted(binary_names)}"
            )
        for binary in binaries:
            if not isinstance(binary, dict):
                raise PackageBuilderError(
                    f"{architecture} cell {key} has malformed binary validation"
                )
            actual_elf = str(binary.get("elf_architecture", "")).lower()
            if (
                actual_elf not in elf_aliases[architecture]
                and expected_elf not in actual_elf
            ):
                raise PackageBuilderError(
                    f"{architecture} cell {key} ELF architecture mismatch: {actual_elf!r}"
                )
            if (
                binary.get("rpath_absent") is not True
                or binary.get("compiler_runtime_safe") is not True
            ):
                raise PackageBuilderError(
                    f"{architecture} cell {key} binary safety attestation failed"
                )
        artifact = cell.get("artifact", {})
        filename = str(artifact.get("filename", ""))
        suffix = expected_artifact_suffix(matrix.distributions[key[1]].package_format)
        if not filename.endswith(suffix):
            raise PackageBuilderError(
                f"{architecture} cell {key} artifact {filename!r} does not end in {suffix}"
            )
        if not isinstance(artifact.get("size"), int) or artifact["size"] <= 0:
            raise PackageBuilderError(f"{architecture} cell {key} artifact is empty")
        if not re.fullmatch(r"[0-9a-f]{64}", str(artifact.get("sha256", ""))):
            raise PackageBuilderError(f"{architecture} cell {key} SHA-256 is invalid")
        if (
            not isinstance(artifact.get("build_size"), int)
            or artifact["build_size"] <= 0
            or not re.fullmatch(r"[0-9a-f]{64}", str(artifact.get("build_sha256", "")))
        ):
            raise PackageBuilderError(f"{architecture} cell {key} build provenance is invalid")
        package_format = matrix.distributions[key[1]].package_format
        # RPM signing mutates the package header during publish. DEB and
        # pacman publishing must preserve the package bytes exactly.
        if package_format != "rpm" and (
            artifact.get("build_size") != artifact.get("size")
            or artifact.get("build_sha256") != artifact.get("sha256")
        ):
            raise PackageBuilderError(
                f"{architecture} cell {key} downloaded artifact differs from built package"
            )
        upload = cell.get("upload", {})
        expected_directory = f"{s3_prefix}/{architecture}/postgresql{key[0]}/{key[1]}"
        if package_format == "deb":
            expected_directory += "/stable"
        expected_key = f"{expected_directory}/{filename}"
        if (
            upload.get("published") is not True
            or upload.get("bucket") != bucket
            or upload.get("key") != expected_key
            or upload.get("size") != artifact.get("size")
            or upload.get("sha256") != artifact.get("sha256")
            or not isinstance(upload.get("etag"), str)
            or not upload["etag"]
            or not isinstance(upload.get("version_id"), str)
            or not upload["version_id"]
            or upload.get("server_side_encryption") != "AES256"
        ):
            raise PackageBuilderError(
                f"{architecture} cell {key} upload contract mismatch: {upload}"
            )
        downloaded_validation = upload.get("download_validation", {})
        if any(
            downloaded_validation.get(name) != value
            for name, value in required_validation.items()
        ):
            raise PackageBuilderError(
                f"{architecture} cell {key} downloaded package failed validation: "
                f"{downloaded_validation}"
            )
        require_smoke_attestation(
            upload.get("download_smoke"),
            architecture=architecture,
            postgres_major=key[0],
            distro=key[1],
            label=f"{architecture} cell {key} S3-download smoke",
        )
        signature = upload.get("signature_verification", {})
        expected_signature_kind = {
            "rpm": "embedded-rpm-openpgp",
            "deb": "signed-deb-release-sha256",
            "pacman": "detached-pacman-openpgp",
        }[package_format]
        if (
            not isinstance(signature, dict)
            or signature.get("passed") is not True
            or signature.get("public_key_imported") is not True
            or signature.get("package_signature_verified") is not True
            or signature.get("repository_signature_verified") is not True
            or signature.get("format") != package_format
            or signature.get("package_signature") != expected_signature_kind
            or signature.get("signing_key_fingerprint") != signing_fingerprint
        ):
            raise PackageBuilderError(
                f"{architecture} cell {key} signature verification failed: {signature}"
            )
        signature_base = f"{s3_prefix}/{architecture}/postgresql{key[0]}/{key[1]}"
        expected_signature_keys = {
            "rpm": {
                f"{signature_base}/repodata/repomd.xml.key",
                f"{signature_base}/repodata/repomd.xml",
                f"{signature_base}/repodata/repomd.xml.asc",
            },
            "deb": {
                f"{signature_base}/stable/Release.key",
                f"{signature_base}/stable/Release",
                f"{signature_base}/stable/Release.gpg",
                f"{signature_base}/stable/InRelease",
            },
            "pacman": {
                f"{signature_base}/public.key",
                f"{expected_key}.sig",
                f"{signature_base}/{cell['package_name']}.db.tar.gz",
                f"{signature_base}/{cell['package_name']}.db.tar.gz.sig",
            },
        }[package_format]
        signature_objects = signature.get("objects")
        if not isinstance(signature_objects, list) or len(signature_objects) != len(
            expected_signature_keys
        ):
            raise PackageBuilderError(
                f"{architecture} cell {key} signature-object count is invalid"
            )
        actual_signature_keys: set[str] = set()
        for signature_object in signature_objects:
            if (
                not isinstance(signature_object, dict)
                or not isinstance(signature_object.get("key"), str)
                or not isinstance(signature_object.get("size"), int)
                or signature_object["size"] <= 0
                or re.fullmatch(
                    r"[0-9a-f]{64}", str(signature_object.get("sha256", ""))
                )
                is None
                or not isinstance(signature_object.get("etag"), str)
                or not signature_object["etag"]
                or not isinstance(signature_object.get("version_id"), str)
                or not signature_object["version_id"]
                or signature_object.get("server_side_encryption") != "AES256"
            ):
                raise PackageBuilderError(
                    f"{architecture} cell {key} has malformed signature object: "
                    f"{signature_object}"
                )
            actual_signature_keys.add(signature_object["key"])
        if actual_signature_keys != expected_signature_keys:
            raise PackageBuilderError(
                f"{architecture} cell {key} signature-object keys mismatch: "
                f"{sorted(actual_signature_keys)}"
            )
        uploads.append(upload)
    if seen != expected_cells:
        raise PackageBuilderError(f"{architecture} manifest omitted cells")
    return uploads


def verify_s3_uploads(region: str, bucket: str, uploads: Iterable[dict[str, Any]]) -> None:
    for upload in uploads:
        signature = upload.get("signature_verification", {})
        signature_objects = signature.get("objects", []) if isinstance(signature, dict) else []
        for attestation in [upload, *signature_objects]:
            version_id = attestation.get("version_id")
            if not isinstance(version_id, str) or not version_id:
                raise PackageBuilderError(
                    f"S3 attestation has no captured VersionId for {attestation.get('key')}"
                )
            if attestation.get("server_side_encryption") != "AES256":
                raise PackageBuilderError(
                    f"S3 attestation does not require AES256 for {attestation.get('key')}"
                )
            payload = aws_json(
                region,
                "s3api",
                "head-object",
                "--bucket",
                bucket,
                "--key",
                str(attestation["key"]),
                "--version-id",
                version_id,
            )
            if payload.get("ContentLength") != attestation.get("size"):
                raise PackageBuilderError(
                    f"S3 object size mismatch for s3://{bucket}/{attestation['key']}"
                )
            expected_etag = str(attestation.get("etag", ""))
            actual_etag = str(payload.get("ETag", "")).strip('"')
            if expected_etag and actual_etag != expected_etag:
                raise PackageBuilderError(
                    f"S3 object ETag mismatch for s3://{bucket}/{attestation['key']}"
                )
            if payload.get("VersionId") != version_id:
                raise PackageBuilderError(
                    f"S3 object version mismatch for s3://{bucket}/{attestation['key']}"
                )
            encryption = payload.get("ServerSideEncryption")
            if encryption != "AES256":
                raise PackageBuilderError(
                    "S3 object does not use exact AES256 encryption: "
                    f"s3://{bucket}/{attestation['key']}"
                )


def write_last_run(payload: dict[str, Any]) -> None:
    RUNTIME_DIR.mkdir(parents=True, exist_ok=True)
    temporary = LAST_RUN_FILE.with_suffix(".tmp")
    temporary.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(LAST_RUN_FILE)


@contextlib.contextmanager
def exclusive_lock() -> Iterable[None]:
    RUNTIME_DIR.mkdir(parents=True, exist_ok=True)
    with LOCK_FILE.open("w", encoding="utf-8") as handle:
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise PackageBuilderError("another package-builder operation is active") from error
        handle.write(f"pid={os.getpid()} started={utc_now()}\n")
        handle.flush()
        yield


def provision_and_start(args: argparse.Namespace, matrix: Matrix) -> tuple[
    dict[str, Any], dict[str, Builder], dict[str, dict[str, Any]], dict[str, bool]
]:
    artifact_prefix = normalize_s3_prefix(args.s3_prefix)
    aws_preflight(args.region)
    ssh_cidr = normalize_ssh_cidr(args.ssh_cidr)
    info(f"SSH ingress is fail-closed to {ssh_cidr}")
    revision = commit_id()
    lock_sha256 = committed_poetry_lock_sha256(revision)
    matrix_fingerprint = complete_matrix_fingerprint(matrix, lock_sha256)
    cached: dict[str, str] = {}
    cache_hits: dict[str, bool] = {}
    for name, architecture in matrix.architectures.items():
        image = (
            None
            if args.from_scratch
            else latest_cached_ami(
                args.region,
                architecture,
                lock_sha256,
                matrix_fingerprint,
            )
        )
        cache_hits[name] = image is not None
        if image is not None:
            cached[name] = str(image["ImageId"])
            info(f"selected cached {name} AMI {cached[name]}")
        else:
            info(f"no schema-{BOOTSTRAP_SCHEMA} cached {name} AMI; using Canonical base")
    outputs = tofu_apply(
        region=args.region,
        ssh_cidr=ssh_cidr,
        cached_amis=cached,
        artifact_bucket_name=args.bucket_name or "",
        artifact_prefix=artifact_prefix,
    )
    builders = output_builders(outputs, matrix)
    records = start_builders(args.region, builders, matrix)
    builders = refreshed_builders(records, builders)
    return outputs, builders, records, cache_hits


def is_complete_matrix_selection(selection: Selection, matrix: Matrix) -> bool:
    return (
        len(selection.architectures) == len(matrix.architectures)
        and set(selection.architectures) == set(matrix.architectures)
        and len(selection.distributions) == len(matrix.distributions)
        and set(selection.distributions) == set(matrix.distributions)
        and len(selection.postgresql_majors) == len(matrix.postgresql_majors)
        and set(selection.postgresql_majors) == set(matrix.postgresql_majors)
    )


def complete_matrix_fingerprint(matrix: Matrix, lock_sha256: str) -> str:
    if re.fullmatch(r"[0-9a-f]{64}", lock_sha256) is None:
        raise PackageBuilderError("complete-matrix fingerprint requires a lock SHA-256")
    payload = {
        "bootstrap_schema": BOOTSTRAP_SCHEMA,
        "worker_generation": WORKER_GENERATION,
        "poetry_lock_sha256": lock_sha256,
        "architectures": {
            name: {
                "aws_architecture": value.aws_architecture,
                "artifact_architecture": value.artifact_architecture,
                "instance_type": value.instance_type,
            }
            for name, value in sorted(matrix.architectures.items())
        },
        "distributions": {
            name: value.package_format
            for name, value in sorted(matrix.distributions.items())
        },
        "postgresql_majors": sorted(matrix.postgresql_majors),
    }
    encoded = json.dumps(payload, separators=(",", ":"), sort_keys=True).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def _result_poetry_environment(
    build_result: dict[str, Any], architecture: str, lock_sha256: str
) -> PoetryEnvironment:
    environments = build_result.get("python_environments")
    value = environments.get(architecture) if isinstance(environments, dict) else None
    if not isinstance(value, dict):
        raise PackageBuilderError(
            f"aggregate result has no Python environment for {architecture}"
        )
    environment = PoetryEnvironment(
        lock_sha256=lock_sha256,
        venv_path=str(value.get("venv_path", "")),
        python_version=str(value.get("python", "")),
        poetry_version=str(value.get("poetry", "")),
    )
    expected_path = f"{REMOTE_POETRY_VENV_ROOT}/{lock_sha256}"
    if (
        environment.venv_path != expected_path
        or re.fullmatch(r"3\.12\.\d+", environment.python_version) is None
        or re.fullmatch(r"1\.8\.\d+", environment.poetry_version) is None
    ):
        raise PackageBuilderError(
            f"aggregate result has invalid Python environment for {architecture}"
        )
    return environment


def create_post_build_amis(
    *,
    args: argparse.Namespace,
    matrix: Matrix,
    selection: Selection,
    outputs: dict[str, Any],
    builders: dict[str, Builder],
    build_result: dict[str, Any],
    lock_sha256: str,
) -> dict[str, dict[str, Any]]:
    """Image both live builders only after the canonical matrix passes again."""

    if not is_complete_matrix_selection(selection, matrix):
        raise PackageBuilderError(
            "post-build AMIs require the complete canonical architecture/distro/"
            "PostgreSQL matrix; partial selections cannot image"
        )
    if build_result.get("status") != "success":
        raise PackageBuilderError("post-build AMIs require a successful aggregate result")
    revision = str(build_result.get("revision", ""))
    if re.fullmatch(r"[0-9a-f]{40}", revision) is None:
        raise PackageBuilderError("post-build AMIs require committed source provenance")
    if build_result.get("poetry_lock_sha256") != lock_sha256:
        raise PackageBuilderError("post-build AMI Poetry lock provenance mismatch")
    manifests = build_result.get("manifests")
    worker_units = build_result.get("worker_units")
    expected_architectures = set(matrix.architectures)
    if not isinstance(manifests, dict) or set(manifests) != expected_architectures:
        raise PackageBuilderError("post-build AMIs require both exact architecture manifests")
    if not isinstance(worker_units, dict) or set(worker_units) != expected_architectures:
        raise PackageBuilderError("post-build AMIs require both exact worker units")
    bucket = outputs.get("package_bucket_name")
    if not isinstance(bucket, str) or not bucket:
        raise PackageBuilderError("OpenTofu output `package_bucket_name` is missing")
    expected_artifact_count = (
        len(matrix.architectures)
        * len(matrix.distributions)
        * len(matrix.postgresql_majors)
    )
    if build_result.get("artifact_count") != expected_artifact_count:
        raise PackageBuilderError(
            f"post-build AMIs require {expected_artifact_count} exact package uploads"
        )
    uploads: list[dict[str, Any]] = []
    for name in matrix.architectures:
        environment = _result_poetry_environment(build_result, name, lock_sha256)
        manifest = manifests[name]
        if not isinstance(manifest, dict):
            raise PackageBuilderError(f"{name} aggregate manifest is malformed")
        uploads.extend(
            validate_manifest(
                manifest,
                architecture=name,
                selection=selection,
                matrix=matrix,
                bucket=bucket,
                region=args.region,
                s3_prefix=args.s3_prefix,
                source_revision=revision,
                poetry_lock_sha256=lock_sha256,
                python_environment=environment,
            )
        )
    if len(uploads) != expected_artifact_count:
        raise PackageBuilderError("post-build AMI upload set is not the canonical matrix")
    verify_s3_uploads(args.region, bucket, uploads)

    matrix_fingerprint = complete_matrix_fingerprint(matrix, lock_sha256)
    key_path = ssh_key_path(outputs)
    images: dict[str, dict[str, Any]] = {}
    errors: list[str] = []
    with concurrent.futures.ThreadPoolExecutor(
        max_workers=len(matrix.architectures), thread_name_prefix="bioseq-cache"
    ) as executor:
        futures = {
            executor.submit(
                create_cached_ami,
                args.region,
                builders[name],
                matrix.architectures[name],
                key_path,
                revision,
                lock_sha256,
                matrix_fingerprint,
                str(worker_units[name]),
            ): name
            for name in matrix.architectures
        }
        for future in concurrent.futures.as_completed(futures):
            name = futures[future]
            try:
                images[name] = future.result()
            except Exception as error:
                errors.append(f"{name}: {error}")
    if errors:
        raise PackageBuilderError(
            "post-build package cache creation failed: " + "; ".join(errors)
        )
    build_result["complete_matrix_fingerprint"] = matrix_fingerprint
    return images


def execute_build(
    args: argparse.Namespace,
    matrix: Matrix,
    selection: Selection,
    outputs: dict[str, Any],
    builders: dict[str, Builder],
) -> dict[str, Any]:
    revision = commit_id()
    require_committed_paths(
        [
            HERE / "package_builder.py",
            HERE / "initialize_package_signing_key.py",
            HERE / "package_matrix.json",
            STACK_DIR / "main.tf",
            STACK_DIR / "variables.tf",
            STACK_DIR / "outputs.tf",
            STACK_DIR / "scripts" / "bootstrap.sh",
            STACK_DIR / "scripts" / "user_data.sh.tftpl",
            PACKAGING_DIR / "pyproject.toml",
            PACKAGING_DIR / "poetry.lock",
            PACKAGING_DIR / "build_packages.py",
            PACKAGING_DIR / "bioseq_packaging",
            PACKAGING_DIR / "rpm",
            PACKAGING_DIR / "deb",
            PACKAGING_DIR / "pacman",
            PACKAGING_DIR / "scripts",
        ]
    )
    lock_sha256 = committed_poetry_lock_sha256(revision)
    key_path = ssh_key_path(outputs)
    selected_builders = {
        name: builders[name] for name in selection.architectures
    }
    bucket = outputs.get("package_bucket_name")
    secret_arn = outputs.get("signing_key_secret_arn")
    output_region = outputs.get("region")
    if not isinstance(bucket, str) or not bucket:
        raise PackageBuilderError("OpenTofu output `package_bucket_name` is missing")
    if not isinstance(secret_arn, str) or not secret_arn.startswith("arn:aws:secretsmanager:"):
        raise PackageBuilderError("OpenTofu output `signing_key_secret_arn` is missing")
    if output_region != args.region:
        raise PackageBuilderError(
            f"OpenTofu region output {output_region!r} does not match {args.region!r}"
        )

    remote_sources: dict[str, str] = {}
    poetry_environments: dict[str, PoetryEnvironment] = {}

    def prepare_builder(name: str, builder: Builder) -> tuple[str, PoetryEnvironment]:
        wait_for_ssh(builder, key_path)
        remote_source = sync_committed_source(builder, key_path, revision)
        reconcile_remote_bootstrap(builder, key_path, remote_source)
        environment = prepare_remote_poetry_environment(
            builder,
            key_path,
            remote_source,
            revision,
            lock_sha256,
        )
        return remote_source, environment

    preparation_errors: list[str] = []
    with concurrent.futures.ThreadPoolExecutor(
        max_workers=len(selected_builders), thread_name_prefix="bioseq-prepare"
    ) as executor:
        preparation_futures = {
            executor.submit(prepare_builder, name, builder): name
            for name, builder in selected_builders.items()
        }
        for future in concurrent.futures.as_completed(preparation_futures):
            name = preparation_futures[future]
            try:
                remote_source, environment = future.result()
                remote_sources[name] = remote_source
                poetry_environments[name] = environment
                info(f"{name} committed source and Poetry environment are ready")
            except Exception as error:
                preparation_errors.append(f"{name}: {error}")
    if preparation_errors:
        raise PackageBuilderError(
            "native builder preparation failed: " + "; ".join(preparation_errors)
        )

    signing_host_name = "x64" if "x64" in selected_builders else selection.architectures[0]
    initialize_signing_key(
        selected_builders[signing_host_name],
        key_path,
        remote_sources[signing_host_name],
        args.region,
        secret_arn,
    )

    run_id = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ") + f"-{revision[:12]}"
    handles: dict[str, RemoteBuildHandle] = {}
    build_results: dict[str, dict[str, Any]] = {}
    errors: list[str] = []
    with concurrent.futures.ThreadPoolExecutor(
        max_workers=len(selected_builders), thread_name_prefix="bioseq-launch"
    ) as executor:
        launch_futures = {
            executor.submit(
                launch_remote_build,
                builder=builder,
                key_path=key_path,
                remote_source=remote_sources[name],
                revision=revision,
                poetry_environment=poetry_environments[name],
                selection=selection,
                region=args.region,
                bucket=bucket,
                s3_prefix=args.s3_prefix,
                secret_arn=secret_arn,
                run_id=run_id,
            ): name
            for name, builder in selected_builders.items()
        }
        for future in concurrent.futures.as_completed(launch_futures):
            name = launch_futures[future]
            try:
                handles[name] = future.result()
                info(f"{name} native systemd worker launched")
            except Exception as error:
                errors.append(f"{name} launch: {error}")

    # Every successfully launched unit is allowed to finish even if its sibling
    # failed to launch. Both units are started before either one is awaited.
    with concurrent.futures.ThreadPoolExecutor(
        max_workers=max(1, len(handles)), thread_name_prefix="bioseq-package"
    ) as executor:
        wait_futures = {
            executor.submit(wait_remote_build, handle, key_path): name
            for name, handle in handles.items()
        }
        for future in concurrent.futures.as_completed(wait_futures):
            name = wait_futures[future]
            try:
                build_results[name] = future.result()
                info(f"{name} native package matrix completed")
            except Exception as error:
                errors.append(f"{name} build: {error}")
    if errors:
        raise PackageBuilderError("native package builds failed: " + "; ".join(errors))
    if set(build_results) != set(selected_builders):
        raise PackageBuilderError("native package builds omitted an architecture")

    uploads: list[dict[str, Any]] = []
    for name, manifest in build_results.items():
        uploads.extend(
            validate_manifest(
                manifest,
                architecture=name,
                selection=selection,
                matrix=matrix,
                bucket=bucket,
                region=args.region,
                s3_prefix=args.s3_prefix,
                source_revision=revision,
                poetry_lock_sha256=lock_sha256,
                python_environment=poetry_environments[name],
            )
        )
    verify_s3_uploads(args.region, bucket, uploads)
    result = {
        "schema_version": 1,
        "status": "success",
        "run_id": run_id,
        "revision": revision,
        "source_revision": revision,
        "poetry_lock_sha256": lock_sha256,
        "worker_generation": WORKER_GENERATION,
        "completed_at": utc_now(),
        "selection": {
            "architectures": list(selection.architectures),
            "distributions": list(selection.distributions),
            "postgresql_majors": list(selection.postgresql_majors),
        },
        "bucket": bucket,
        "s3_prefix": args.s3_prefix,
        "artifact_count": len(uploads),
        "python_environments": {
            name: {
                **environment.manifest_value(),
                "venv_path": environment.venv_path,
            }
            for name, environment in poetry_environments.items()
        },
        "worker_units": {
            name: handle.unit_name for name, handle in handles.items()
        },
        "manifests": build_results,
    }
    return result


def cmd_validate(args: argparse.Namespace) -> None:
    matrix = load_matrix()
    selection = select_matrix(args, matrix)
    tofu_executable()
    require_executable("aws")
    require_executable("ssh")
    require_executable("git")
    aws_preflight(args.region)
    run_command([tofu_executable(), "fmt", "-check", "-recursive"], cwd=STACK_DIR)
    tofu_init()
    run_command([tofu_executable(), "validate"], cwd=STACK_DIR)
    print(json.dumps({"status": "valid", "selection": selection.__dict__}, indent=2))


def cmd_output(args: argparse.Namespace) -> None:
    del args
    print(json.dumps(tofu_outputs(), indent=2, sort_keys=True))


def cmd_status(args: argparse.Namespace) -> None:
    matrix = load_matrix()
    outputs = tofu_outputs()
    if not outputs:
        print(json.dumps({"stack": "not-applied"}, indent=2))
        return
    builders = output_builders(outputs, matrix)
    records = validate_builders(args.region, builders, matrix)
    payload = {
        "region": args.region,
        "bucket": outputs.get("package_bucket_name"),
        "builders": {
            name: {
                "instance_id": builder.instance_id,
                "instance_type": builder.instance_type,
                "architecture": builder.aws_architecture,
                "state": instance_state(records[name]),
                "public_ip": records[name].get("PublicIpAddress"),
            }
            for name, builder in builders.items()
        },
        "last_run": json.loads(LAST_RUN_FILE.read_text()) if LAST_RUN_FILE.exists() else None,
    }
    print(json.dumps(payload, indent=2, sort_keys=True))


def cmd_up(args: argparse.Namespace) -> None:
    matrix = load_matrix()
    outputs, builders, _, _ = provision_and_start(args, matrix)
    payload = {
        "region": args.region,
        "bucket": outputs.get("package_bucket_name"),
        "builders": {name: builder.__dict__ for name, builder in builders.items()},
    }
    print(json.dumps(payload, indent=2, sort_keys=True))
    info("builders remain running under the persistent-build-cache lifecycle")


def cmd_run(args: argparse.Namespace) -> None:
    matrix = load_matrix()
    selection = select_matrix(args, matrix)
    if not args.no_snapshot and not is_complete_matrix_selection(selection, matrix):
        raise PackageBuilderError(
            "partial matrix runs must pass --no-snapshot; only the canonical matrix may image"
        )
    outputs, builders, _, _ = provision_and_start(args, matrix)
    result = execute_build(args, matrix, selection, outputs, builders)
    if not args.no_snapshot:
        images = create_post_build_amis(
            args=args,
            matrix=matrix,
            selection=selection,
            outputs=outputs,
            builders=builders,
            build_result=result,
            lock_sha256=str(result["poetry_lock_sha256"]),
        )
        result["cache_images"] = {
            name: {
                "image_id": str(image["ImageId"]),
                "source_instance_id": builders[name].instance_id,
            }
            for name, image in images.items()
        }
    result["completed_at"] = utc_now()
    result["builders_left_running"] = {
        name: builder.instance_id for name, builder in builders.items()
    }
    write_last_run(result)
    info("package builders remain running; no stop or shutdown action was issued")
    print(json.dumps(result, indent=2, sort_keys=True))


def add_common_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--region", default=DEFAULT_REGION)
    parser.add_argument(
        "--ssh-cidr",
        help="explicit operator IPv4 /32 (default: detect it; never fails open)",
    )
    parser.add_argument(
        "--bucket-name",
        help="optional globally unique artifact bucket name (OpenTofu derives one by default)",
    )
    parser.add_argument(
        "--s3-prefix",
        default=DEFAULT_S3_PREFIX,
        help="artifact key prefix; the builder IAM policy is scoped to this exact prefix",
    )
    parser.add_argument(
        "--from-scratch",
        action="store_true",
        help="ignore matching package-builder AMI caches for this apply",
    )
    parser.add_argument(
        "--no-snapshot",
        action="store_true",
        help="skip post-build AMIs (required for any partial matrix run)",
    )


def add_matrix_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--architectures",
        action="append",
        help="comma-separated native architecture keys (default: arm64,x64)",
    )
    parser.add_argument(
        "--distros",
        action="append",
        help="comma-separated native package distro IDs (default: complete DOC0390 set)",
    )
    parser.add_argument(
        "--postgres",
        action="append",
        help="comma-separated PostgreSQL majors (default: matrix selection, currently 17)",
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    run_parser = subparsers.add_parser(
        "run", help="provision/start, build, upload and optionally image; leave running"
    )
    add_common_options(run_parser)
    add_matrix_options(run_parser)
    run_parser.set_defaults(handler=cmd_run)

    up_parser = subparsers.add_parser(
        "up", help="provision/start both builders for explicit debugging"
    )
    add_common_options(up_parser)
    up_parser.set_defaults(handler=cmd_up)

    status_parser = subparsers.add_parser(
        "status", help="show validated builder state and the last successful run"
    )
    status_parser.add_argument("--region", default=DEFAULT_REGION)
    status_parser.set_defaults(handler=cmd_status)

    output_parser = subparsers.add_parser("output", help="print raw OpenTofu outputs")
    output_parser.set_defaults(handler=cmd_output)

    validate_parser = subparsers.add_parser(
        "validate", help="validate matrix, AWS identity, and the OpenTofu stack"
    )
    validate_parser.add_argument("--region", default=DEFAULT_REGION)
    add_matrix_options(validate_parser)
    validate_parser.set_defaults(handler=cmd_validate)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    previous_sigterm = signal.getsignal(signal.SIGTERM)
    previous_sigint = signal.getsignal(signal.SIGINT)

    def interrupt_for_cleanup(signum: int, _frame: Any) -> None:
        interrupt_active_children()
        raise KeyboardInterrupt(f"received signal {signum}")

    signal.signal(signal.SIGTERM, interrupt_for_cleanup)
    signal.signal(signal.SIGINT, interrupt_for_cleanup)
    try:
        with exclusive_lock():
            args.handler(args)
    except KeyboardInterrupt:
        print("package-builder interrupted", file=sys.stderr)
        return 130
    except PackageBuilderError as error:
        print(f"package-builder failed: {error}", file=sys.stderr)
        return 1
    finally:
        signal.signal(signal.SIGTERM, previous_sigterm)
        signal.signal(signal.SIGINT, previous_sigint)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
