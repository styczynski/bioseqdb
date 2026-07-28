#!/usr/bin/env python3
"""Provision, run, verify, and stop BioSeq's native AWS package builders.

This is the OpenTofu-only entry point for DOC0390.  The normal ``run`` command
owns the complete lifecycle: it selects architecture-specific cached AMIs,
applies the isolated OpenTofu stack, starts only the exact instances recorded
in that stack, waits for bootstrap, builds on both native architectures,
validates the remote manifests and S3 objects, and stops both instances in a
``finally`` block.  It never terminates instances and hard-rejects the two
protected development instance IDs from the specification.

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
    "Lifecycle": "start-build-stop",
}

CACHE_TAGS = {
    "Project": "bioseq",
    "Component": "package-builder-cache",
    "ManagedBy": "bioseq-package-builder",
    "BaseOS": "ubuntu-24.04",
}

BOOTSTRAP_SCHEMA = "1"
DEFAULT_REGION = "us-east-1"
DEFAULT_S3_PREFIX = "bioseq/packages"
REMOTE_ROOT = "/opt/bioseq-package-builder/workspace/builds"
INSTANCE_ID_RE = re.compile(r"^i-[0-9a-f]{8,17}$")
AMI_ID_RE = re.compile(r"^ami-[0-9a-f]{8,17}$")
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
            # SIGINT is OpenTofu's state-flushing interrupt. Escalate only to
            # TERM (never KILL), then let exact state recovery decide whether
            # it is safe to stop instances.
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


def tofu_apply(
    *,
    region: str,
    ssh_cidr: str,
    cached_amis: dict[str, str],
    artifact_bucket_name: str,
) -> dict[str, Any]:
    guard_local_state_against_protected_instances()
    tofu_init()
    variables = {
        "region": region,
        "ssh_ingress_cidr": ssh_cidr,
        "arm64_ami_id": cached_amis.get("arm64", ""),
        "x64_ami_id": cached_amis.get("x64", ""),
        "artifact_bucket_name": artifact_bucket_name,
        "ami_schema_version": BOOTSTRAP_SCHEMA,
        "root_volume_size_gb": "500",
        "compiler_rt_priority": "1",
    }
    argv = [tofu_executable(), "apply", "-input=false", "-auto-approve"]
    argv.extend(f"-var={key}={value}" for key, value in variables.items())
    run_command(argv, cwd=STACK_DIR)
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


def latest_cached_ami(region: str, architecture: Architecture) -> dict[str, Any] | None:
    filters = [
        f"Name=tag:{key},Values={value}" for key, value in CACHE_TAGS.items()
    ]
    filters.extend(
        [
            f"Name=tag:BuildArchitecture,Values={architecture.name}",
            f"Name=tag:Architecture,Values={architecture.aws_architecture}",
            f"Name=tag:BootstrapSchema,Values={BOOTSTRAP_SCHEMA}",
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
    candidates = [image for image in images if AMI_ID_RE.match(str(image.get("ImageId", "")))]
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
    for builder in builders.values():
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
    return validate_builders(region, builders, matrix)


def stop_builders(
    region: str, builders: dict[str, Builder], matrix: Matrix, *, wait: bool = True
) -> None:
    errors: list[str] = []
    for name, builder in builders.items():
        try:
            record = describe_builder(region, builder, matrix.architectures[name])
            state = instance_state(record)
            if state in {"stopped", "stopping"}:
                info(f"{name} builder {builder.instance_id} is {state}; no stop needed")
                continue
            if state == "pending":
                # StopInstances rejects an EBS-backed instance that has not
                # reached running yet. Finish the bounded launch waiter, then
                # revalidate ownership/lifecycle invariants before stopping.
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
                record = describe_builder(
                    region, builder, matrix.architectures[name]
                )
                state = instance_state(record)
            if state != "running":
                raise PackageBuilderError(
                    f"refusing to stop {builder.instance_id} from unexpected state {state!r}"
                )
            aws_no_output(
                region, "ec2", "stop-instances", "--instance-ids", builder.instance_id
            )
        except Exception as error:  # continue so the other architecture is still stopped
            errors.append(f"{name}: {error}")
    if wait:
        for name, builder in builders.items():
            if any(error.startswith(f"{name}:") for error in errors):
                continue
            try:
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
            except Exception as error:
                errors.append(f"{name}: {error}")
    if errors:
        raise PackageBuilderError("failed to stop all managed builders: " + "; ".join(errors))


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
    # cloud-init reports a nonzero status when bootstrap failed, which keeps a
    # partially initialized machine from ever entering the package matrix.
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


def create_cached_ami(
    region: str,
    builder: Builder,
    architecture: Architecture,
    matrix: Matrix,
    key_path: Path,
) -> tuple[dict[str, Any], Builder]:
    # Snapshot only a stopped, fully validated pre-source machine. Explicit
    # --no-reboot prevents CreateImage from attempting its default reboot after
    # we have already established the filesystem-consistent stopped state. The
    # exact instance is restarted and revalidated before source is staged.
    ssh_run(
        builder,
        key_path,
        "sudo cloud-init clean --logs --machine-id",
    )
    stop_builders(region, {architecture.name: builder}, matrix)
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d-%H%M%S")
    name = f"bioseq-package-builder-{architecture.name}-schema{BOOTSTRAP_SCHEMA}-{stamp}"
    payload = aws_json(
        region,
        "ec2",
        "create-image",
        "--instance-id",
        builder.instance_id,
        "--name",
        name,
        "--description",
        f"BioSeq native package builder {architecture.name}, bootstrap schema {BOOTSTRAP_SCHEMA}",
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
    records = start_builders(
        region, {architecture.name: builder}, matrix
    )
    refreshed = refreshed_builders(records, {architecture.name: builder})[
        architecture.name
    ]
    wait_for_bootstrap(refreshed, key_path)
    image = wait_for_cached_ami(region, image_id)
    info(f"created reusable {architecture.name} package-builder AMI {image_id}")
    return image, refreshed


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


def run_remote_build(
    *,
    builder: Builder,
    key_path: Path,
    remote_source: str,
    revision: str,
    selection: Selection,
    region: str,
    bucket: str,
    s3_prefix: str,
    secret_arn: str,
    run_id: str,
) -> dict[str, Any]:
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    log_path = LOG_DIR / f"{run_id}-{builder.architecture}.log"
    manifest_path = f"{REMOTE_ROOT}/{run_id}-{builder.architecture}-manifest.json"
    build_root = f"{REMOTE_ROOT}/{run_id}-{builder.architecture}"
    argv = [
        "env",
        "BIOSEQ_OPTTIMIZATION_LEVEL=0",
        "python3",
        f"{remote_source}/main/infra/packaging/build_packages.py",
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
    ]
    remote_command = (
        "set -o pipefail; "
        f"install -d -m 0755 {shlex.quote(build_root)}; "
        f"{command_text(argv)} 2>&1 | tee {shlex.quote(build_root + '/build.log')}"
    )
    info(f"{builder.architecture} build log: {log_path}")
    with log_path.open("w", encoding="utf-8") as log:
        completed = ssh_run(
            builder,
            key_path,
            f"bash -lc {shlex.quote(remote_command)}",
            stdout=log,
            check=False,
        )
    manifest_result = ssh_run(
        builder,
        key_path,
        f"cat {shlex.quote(manifest_path)}",
        capture=True,
        check=False,
    )
    if not manifest_result.stdout.strip():
        raise PackageBuilderError(
            f"{builder.architecture} produced no manifest; ssh exit "
            f"{completed.returncode}; see {log_path}"
        )
    try:
        manifest = json.loads(manifest_result.stdout)
    except json.JSONDecodeError as error:
        raise PackageBuilderError(
            f"{builder.architecture} manifest is invalid JSON: {error}"
        ) from error
    if completed.returncode != 0:
        raise PackageBuilderError(
            f"{builder.architecture} package build failed with exit "
            f"{completed.returncode}; see {log_path}; manifest={manifest.get('status')}"
        )
    return manifest


def expected_artifact_suffix(package_format: str) -> str:
    return {
        "deb": ".deb",
        "rpm": ".rpm",
        "pacman": ".pkg.tar.zst",
    }[package_format]


def validate_manifest(
    manifest: dict[str, Any],
    *,
    architecture: str,
    selection: Selection,
    matrix: Matrix,
    bucket: str,
    region: str,
    s3_prefix: str,
) -> list[dict[str, Any]]:
    if manifest.get("schema_version") != 1 or manifest.get("status") != "success":
        raise PackageBuilderError(
            f"{architecture} manifest did not report schema 1 success"
        )
    if manifest.get("architecture") != architecture:
        raise PackageBuilderError(f"{architecture} manifest architecture mismatch")
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
        upload = cell.get("upload", {})
        expected_prefix = f"{s3_prefix}/{architecture}/postgresql{key[0]}/{key[1]}/"
        if (
            upload.get("published") is not True
            or upload.get("bucket") != bucket
            or not str(upload.get("key", "")).startswith(expected_prefix)
            or not str(upload.get("key", "")).endswith(filename)
            or upload.get("size") != artifact.get("size")
        ):
            raise PackageBuilderError(
                f"{architecture} cell {key} upload contract mismatch: {upload}"
            )
        uploads.append(upload)
    if seen != expected_cells:
        raise PackageBuilderError(f"{architecture} manifest omitted cells")
    return uploads


def verify_s3_uploads(region: str, bucket: str, uploads: Iterable[dict[str, Any]]) -> None:
    for upload in uploads:
        payload = aws_json(
            region,
            "s3api",
            "head-object",
            "--bucket",
            bucket,
            "--key",
            str(upload["key"]),
        )
        if payload.get("ContentLength") != upload.get("size"):
            raise PackageBuilderError(
                f"S3 object size mismatch for s3://{bucket}/{upload['key']}"
            )
        encryption = payload.get("ServerSideEncryption")
        if encryption not in {"AES256", "aws:kms"}:
            raise PackageBuilderError(
                f"S3 object is not server-side encrypted: s3://{bucket}/{upload['key']}"
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
    aws_preflight(args.region)
    ssh_cidr = normalize_ssh_cidr(args.ssh_cidr)
    info(f"SSH ingress is fail-closed to {ssh_cidr}")
    cached: dict[str, str] = {}
    cache_hits: dict[str, bool] = {}
    for name, architecture in matrix.architectures.items():
        image = None if args.from_scratch else latest_cached_ami(args.region, architecture)
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
    )
    builders = output_builders(outputs, matrix)
    records = start_builders(args.region, builders, matrix)
    builders = refreshed_builders(records, builders)
    key_path = ssh_key_path(outputs)
    for builder in builders.values():
        wait_for_bootstrap(builder, key_path)
    if not args.no_snapshot:
        for name, builder in builders.items():
            if not cache_hits[name]:
                _, builders[name] = create_cached_ami(
                    args.region,
                    builder,
                    matrix.architectures[name],
                    matrix,
                    key_path,
                )
    return outputs, builders, records, cache_hits


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
            HERE / "packaging" / "build_packages.py",
        ]
    )
    key_path = ssh_key_path(outputs)
    selected_builders = {
        name: builders[name] for name in selection.architectures
    }
    remote_sources: dict[str, str] = {}
    for name, builder in selected_builders.items():
        remote_sources[name] = sync_committed_source(builder, key_path, revision)

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
    signing_host_name = "x64" if "x64" in selected_builders else selection.architectures[0]
    initialize_signing_key(
        selected_builders[signing_host_name],
        key_path,
        remote_sources[signing_host_name],
        args.region,
        secret_arn,
    )

    run_id = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ") + f"-{revision[:12]}"
    build_results: dict[str, dict[str, Any]] = {}
    errors: list[str] = []
    with concurrent.futures.ThreadPoolExecutor(
        max_workers=len(selected_builders), thread_name_prefix="bioseq-package"
    ) as executor:
        futures = {
            executor.submit(
                run_remote_build,
                builder=builder,
                key_path=key_path,
                remote_source=remote_sources[name],
                revision=revision,
                selection=selection,
                region=args.region,
                bucket=bucket,
                s3_prefix=args.s3_prefix,
                secret_arn=secret_arn,
                run_id=run_id,
            ): name
            for name, builder in selected_builders.items()
        }
        for future in concurrent.futures.as_completed(futures):
            name = futures[future]
            try:
                build_results[name] = future.result()
                info(f"{name} native package matrix completed")
            except Exception as error:
                errors.append(f"{name}: {error}")
    if errors:
        raise PackageBuilderError("native package builds failed: " + "; ".join(errors))

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
            )
        )
    verify_s3_uploads(args.region, bucket, uploads)
    result = {
        "schema_version": 1,
        "status": "success",
        "run_id": run_id,
        "revision": revision,
        "completed_at": utc_now(),
        "selection": {
            "architectures": list(selection.architectures),
            "distributions": list(selection.distributions),
            "postgresql_majors": list(selection.postgresql_majors),
        },
        "bucket": bucket,
        "s3_prefix": args.s3_prefix,
        "artifact_count": len(uploads),
        "manifests": build_results,
    }
    write_last_run(result)
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


def cmd_stop(args: argparse.Namespace) -> None:
    matrix = load_matrix()
    builders = recover_managed_builders(matrix)
    if not builders:
        info("package-build stack is not applied; no builders to stop")
        return
    stop_builders(args.region, builders, matrix)
    info("all exact managed package builders are stopped")


def cmd_up(args: argparse.Namespace) -> None:
    matrix = load_matrix()
    try:
        outputs, builders, _, _ = provision_and_start(args, matrix)
    except BaseException as failure:
        recovery_error: BaseException | None = None
        try:
            partial_builders = recover_managed_builders(matrix)
            if partial_builders:
                stop_builders(args.region, partial_builders, matrix)
                info("stopped exact state-backed builders after incomplete up")
        except BaseException as error:
            recovery_error = error
        if recovery_error is not None:
            raise PackageBuilderError(
                f"up failed ({failure}); partial-instance cleanup also failed "
                f"({recovery_error})"
            ) from failure
        raise
    payload = {
        "region": args.region,
        "bucket": outputs.get("package_bucket_name"),
        "builders": {name: builder.__dict__ for name, builder in builders.items()},
    }
    print(json.dumps(payload, indent=2, sort_keys=True))
    info("builders remain running for explicit debugging; use `package_builder.py stop`")


def cmd_run(args: argparse.Namespace) -> None:
    matrix = load_matrix()
    selection = select_matrix(args, matrix)
    outputs: dict[str, Any] = {}
    builders: dict[str, Builder] = {}
    failure: BaseException | None = None
    result: dict[str, Any] | None = None
    try:
        outputs, builders, _, _ = provision_and_start(args, matrix)
        result = execute_build(args, matrix, selection, outputs, builders)
    except BaseException as error:
        failure = error
    finally:
        if not builders:
            try:
                builders = recover_managed_builders(matrix)
            except BaseException as recovery_error:
                if failure is None:
                    failure = recovery_error
                else:
                    failure = PackageBuilderError(
                        f"build failed ({failure}); exact OpenTofu state recovery also "
                        f"failed ({recovery_error})"
                    )
        if builders:
            try:
                stop_builders(args.region, builders, matrix)
                info("all exact managed package builders are stopped")
            except BaseException as stop_error:
                if failure is None:
                    failure = stop_error
                else:
                    failure = PackageBuilderError(
                        f"build failed ({failure}); stopping builders also failed ({stop_error})"
                    )
    if failure is not None:
        raise failure
    assert result is not None
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
        "--from-scratch",
        action="store_true",
        help="ignore matching package-builder AMI caches for this apply",
    )
    parser.add_argument(
        "--no-snapshot",
        action="store_true",
        help="do not create missing architecture-specific AMI caches after bootstrap",
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
        help="comma-separated OmniPackage distro IDs (default: complete DOC0390 set)",
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
        "run", help="provision/start, build and upload the matrix, then always stop"
    )
    add_common_options(run_parser)
    add_matrix_options(run_parser)
    run_parser.add_argument("--s3-prefix", default=DEFAULT_S3_PREFIX)
    run_parser.set_defaults(handler=cmd_run)

    up_parser = subparsers.add_parser(
        "up", help="provision/start both builders for explicit debugging"
    )
    add_common_options(up_parser)
    up_parser.set_defaults(handler=cmd_up)

    stop_parser = subparsers.add_parser(
        "stop", help="stop only the exact validated builders in OpenTofu output"
    )
    stop_parser.add_argument("--region", default=DEFAULT_REGION)
    stop_parser.set_defaults(handler=cmd_stop)

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
