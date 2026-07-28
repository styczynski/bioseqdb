#!/usr/bin/env python3
"""Build, validate, publish, and attest native Bioseq package matrix cells."""

from __future__ import annotations

import argparse
import base64
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from typing import Iterable, Sequence


PACKAGE_VERSION = "4.2.3"
MANIFEST_SCHEMA_VERSION = 1
SUPPORTED_DISTROS = (
    "opensuse_16.0",
    "fedora_44",
    "debian_13",
    "ubuntu_26.04",
    "arch",
)
PACKAGE_SUFFIXES = (".deb", ".rpm", ".pkg.tar.zst")
DEFAULT_BUILD_ROOT = Path("/var/lib/bioseq-package-build")
DEFAULT_S3_PREFIX = "bioseq/packages"
DEFAULT_ARCH_ARM64_IMAGE = "menci/archlinuxarm:base"


class PackageBuildError(RuntimeError):
    pass


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def csv_values(values: Iterable[str], *, label: str) -> list[str]:
    result: list[str] = []
    for raw in values:
        for value in raw.split(","):
            normalized = value.strip()
            if normalized and normalized not in result:
                result.append(normalized)
    if not result:
        raise PackageBuildError(f"at least one {label} value is required")
    return result


def parse_postgres(values: Iterable[str]) -> list[int]:
    parsed: list[int] = []
    for value in csv_values(values, label="PostgreSQL"):
        if not re.fullmatch(r"\d+", value):
            raise PackageBuildError(f"invalid PostgreSQL major {value!r}")
        major = int(value)
        if major < 17:
            raise PackageBuildError("PostgreSQL majors below 17 are unsupported")
        if major not in parsed:
            parsed.append(major)
    return parsed


def parse_distros(values: Iterable[str]) -> list[str]:
    parsed = csv_values(values, label="distro")
    unknown = [value for value in parsed if value not in SUPPORTED_DISTROS]
    if unknown:
        raise PackageBuildError(
            f"unsupported distro(s): {', '.join(unknown)}; supported: {', '.join(SUPPORTED_DISTROS)}"
        )
    return parsed


def native_architecture() -> str:
    machine = platform.machine().lower()
    if machine in {"x86_64", "amd64"}:
        return "x64"
    if machine in {"aarch64", "arm64"}:
        return "arm64"
    raise PackageBuildError(f"unsupported native machine architecture {machine!r}")


def running_on_aws() -> bool:
    probes = (
        Path("/sys/devices/virtual/dmi/id/sys_vendor"),
        Path("/sys/devices/virtual/dmi/id/product_name"),
        Path("/sys/firmware/devicetree/base/model"),
    )
    for probe in probes:
        try:
            if "amazon" in probe.read_text(errors="ignore").lower():
                return True
        except OSError:
            continue
    try:
        return Path("/sys/hypervisor/uuid").read_text().lower().startswith("ec2")
    except OSError:
        return False


def normalize_signing_key(secret: str) -> str:
    value = secret.strip()
    if value.startswith("{"):
        try:
            decoded_json = json.loads(value)
        except json.JSONDecodeError as error:
            raise PackageBuildError("signing SecretString is invalid JSON") from error
        if not isinstance(decoded_json, dict):
            raise PackageBuildError("signing SecretString JSON must be an object")
        value = str(
            decoded_json.get("GPG_PRIVATE_KEY")
            or decoded_json.get("gpg_private_key_base64")
            or ""
        ).strip()
    if not value:
        raise PackageBuildError("GPG private key is empty")
    if value.startswith("-----BEGIN PGP PRIVATE KEY BLOCK-----"):
        # Canonical ASCII armor ends with one newline. Preserve that contract
        # even though SecretString transport commonly trims surrounding space.
        return base64.b64encode((value.rstrip() + "\n").encode()).decode()
    compact = "".join(value.split())
    try:
        decoded = base64.b64decode(compact, validate=True).decode()
    except (ValueError, UnicodeDecodeError) as error:
        raise PackageBuildError(
            "GPG_PRIVATE_KEY must be ASCII armor or its base64 encoding"
        ) from error
    if "-----BEGIN PGP PRIVATE KEY BLOCK-----" not in decoded:
        raise PackageBuildError("decoded GPG_PRIVATE_KEY is not an armored private key")
    return compact


def fetch_signing_key(
    *,
    secret_arn: str | None,
    aws_bin: str,
    region: str,
    environment: dict[str, str],
    is_aws: bool | None = None,
) -> str:
    on_aws = running_on_aws() if is_aws is None else is_aws
    if on_aws and not secret_arn:
        raise PackageBuildError(
            "--signing-secret-arn is required on AWS package builders"
        )
    if secret_arn:
        try:
            completed = subprocess.run(
                [
                    aws_bin,
                    "secretsmanager",
                    "get-secret-value",
                    "--secret-id",
                    secret_arn,
                    "--region",
                    region,
                    "--query",
                    "SecretString",
                    "--output",
                    "text",
                ],
                check=True,
                env=environment,
                capture_output=True,
                text=True,
            )
        except FileNotFoundError as error:
            raise PackageBuildError(f"AWS CLI is missing: {aws_bin}") from error
        except subprocess.CalledProcessError as error:
            # Never include stdout/stderr: AWS errors can echo sensitive secret
            # metadata depending on account-side policy and wrappers.
            raise PackageBuildError("failed to fetch signing key from Secrets Manager") from error
        return normalize_signing_key(completed.stdout)
    return normalize_signing_key(environment.get("GPG_PRIVATE_KEY", ""))


def _parse_gpg_fingerprint(output: str) -> str:
    for line in output.splitlines():
        fields = line.split(":")
        if len(fields) > 9 and fields[0] == "fpr" and re.fullmatch(r"[0-9A-Fa-f]{40,64}", fields[9]):
            return fields[9].upper()
    raise PackageBuildError("GPG key metadata did not contain a valid fingerprint")


def signing_key_fingerprint(
    signing_key_base64: str, environment: dict[str, str]
) -> str:
    gpg = shutil.which("gpg", path=environment.get("PATH"))
    if not gpg:
        raise PackageBuildError("gpg is required to identify the package-signing key")
    try:
        armor = base64.b64decode(signing_key_base64, validate=True).decode()
        with tempfile.TemporaryDirectory(prefix="bioseq-signing-fingerprint-") as home:
            os.chmod(home, 0o700)
            completed = subprocess.run(
                [
                    gpg,
                    "--batch",
                    "--no-tty",
                    "--homedir",
                    home,
                    "--with-colons",
                    "--import-options",
                    "show-only",
                    "--dry-run",
                    "--import",
                ],
                input=armor,
                check=True,
                env=environment,
                capture_output=True,
                text=True,
            )
    except (ValueError, UnicodeDecodeError) as error:
        raise PackageBuildError("normalized signing key cannot be decoded") from error
    except (FileNotFoundError, subprocess.CalledProcessError) as error:
        # Do not attach GPG output: a wrapper must not be allowed to echo key
        # material into the package manifest or process logs.
        raise PackageBuildError("failed to identify package-signing key fingerprint") from error
    return _parse_gpg_fingerprint(completed.stdout)


def validate_bucket(bucket: str) -> str:
    if not re.fullmatch(r"[a-z0-9][a-z0-9.-]{1,61}[a-z0-9]", bucket):
        raise PackageBuildError(f"invalid S3 bucket name {bucket!r}")
    return bucket


def validate_prefix(prefix: str) -> str:
    value = prefix.strip("/")
    if (
        not value
        or "//" in value
        or any(part in {"", ".", ".."} for part in value.split("/"))
        or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._/-]*", value)
    ):
        raise PackageBuildError(f"invalid S3 prefix {prefix!r}")
    return value


def runtime_dependencies(distro: str, postgres_major: int) -> tuple[str, str]:
    if distro in {"debian_13", "ubuntu_26.04"}:
        return f"postgresql-{postgres_major}", "libvulkan1"
    if distro == "fedora_44":
        return f"postgresql{postgres_major}-server", "vulkan-loader"
    if distro == "opensuse_16.0":
        return f"postgresql{postgres_major}-server", "libvulkan1"
    if distro == "arch":
        postgres_package = "postgresql-old-upgrade" if postgres_major == 17 else "postgresql"
        return postgres_package, "vulkan-icd-loader"
    raise PackageBuildError(f"no runtime dependency strategy for {distro}")


def artifact_format(path: Path) -> str:
    if path.name.endswith(".deb"):
        return "deb"
    if path.name.endswith(".rpm"):
        return "rpm"
    if path.name.endswith(".pkg.tar.zst"):
        return "pacman"
    raise PackageBuildError(f"unsupported artifact {path}")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_manifest(path: Path, manifest: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp-{os.getpid()}")
    data = json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    try:
        with temporary.open("w", encoding="utf-8") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.chmod(temporary, 0o644)
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def safe_command(command: Sequence[str]) -> str:
    # Matrix commands never contain the signing key. Quote only for readable
    # logs; subprocess still receives an argv vector without a shell.
    return " ".join(repr(value) if re.search(r"\s", value) else value for value in command)


def run_streamed(
    command: Sequence[str],
    *,
    environment: dict[str, str],
    cwd: Path | None = None,
) -> None:
    print(f"+ {safe_command(command)}", flush=True)
    try:
        subprocess.run(list(command), check=True, env=environment, cwd=cwd)
    except FileNotFoundError as error:
        raise PackageBuildError(f"required executable is missing: {command[0]}") from error
    except subprocess.CalledProcessError as error:
        raise PackageBuildError(
            f"command exited {error.returncode}: {safe_command(command)}"
        ) from error


def run_captured(
    command: Sequence[str], *, environment: dict[str, str] | None = None
) -> str:
    print(f"+ {safe_command(command)}", flush=True)
    try:
        completed = subprocess.run(
            list(command),
            check=True,
            env=environment,
            capture_output=True,
            text=True,
        )
    except FileNotFoundError as error:
        raise PackageBuildError(f"required executable is missing: {command[0]}") from error
    except subprocess.CalledProcessError as error:
        detail = (error.stderr or error.stdout or "").strip()
        if len(detail) > 2000:
            detail = detail[-2000:]
        raise PackageBuildError(
            f"command exited {error.returncode}: {safe_command(command)}: {detail}"
        ) from error
    return completed.stdout


def resolve_omnipackage(
    requested: str | None,
    source_dir: Path,
    *,
    environment: dict[str, str] | None = None,
) -> Path:
    if requested:
        candidate = Path(requested).expanduser().resolve()
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
        found = shutil.which(requested)
        if found:
            return Path(found).resolve()
        raise PackageBuildError(f"OmniPackage executable is missing: {requested}")
    vendored_root = source_dir.parent / "omnipackage-rs-master"
    manifest = vendored_root / "Cargo.toml"
    lockfile = vendored_root / "Cargo.lock"
    if not manifest.is_file() or not lockfile.is_file():
        raise PackageBuildError(
            f"vendored OmniPackage source or lockfile is missing under {vendored_root}"
        )

    build_environment = dict(environment or os.environ)
    # The build never needs the package-signing key. Excluding it also keeps a
    # failing cargo subprocess from inheriting or accidentally reporting it.
    build_environment.pop("GPG_PRIVATE_KEY", None)
    raw_home = build_environment.get("HOME", "")
    home = Path(raw_home).expanduser() if raw_home else Path("/var/lib/bioseq-package-builder")
    if not home.is_absolute():
        raise PackageBuildError("HOME must be absolute when building vendored OmniPackage")
    home.mkdir(parents=True, exist_ok=True)
    cargo_home = Path(build_environment.get("CARGO_HOME", str(home / ".cargo")))
    rustup_home = Path(build_environment.get("RUSTUP_HOME", str(home / ".rustup")))
    cache_root = source_dir.parent
    if cache_root.name.startswith("source-"):
        cache_root = cache_root.parent
    target_dir = Path(
        build_environment.get(
            "BIOSEQ_OMNIPACKAGE_TARGET_DIR",
            str(cache_root / "caches" / "omnipackage-target"),
        )
    )
    if (
        not cargo_home.is_absolute()
        or not rustup_home.is_absolute()
        or not target_dir.is_absolute()
    ):
        raise PackageBuildError(
            "CARGO_HOME, RUSTUP_HOME, and the OmniPackage target directory must be absolute"
        )
    vendored = target_dir / "release" / "omnipackage"
    build_environment.update(
        {
            "HOME": str(home),
            "CARGO_HOME": str(cargo_home),
            "RUSTUP_HOME": str(rustup_home),
            "CARGO_TARGET_DIR": str(target_dir),
            "PATH": f"{cargo_home / 'bin'}:{build_environment.get('PATH', '')}",
        }
    )
    cargo = shutil.which("cargo", path=build_environment["PATH"])
    if not cargo:
        raise PackageBuildError("cargo is required to build vendored OmniPackage")
    run_streamed(
        [cargo, "build", "--release", "--locked"],
        environment=build_environment,
        cwd=vendored_root,
    )
    if not vendored.is_file() or not os.access(vendored, os.X_OK):
        raise PackageBuildError(
            f"vendored OmniPackage build did not create executable {vendored}"
        )
    return vendored.resolve()


def cell_environment(
    base: dict[str, str],
    *,
    architecture: str,
    bucket: str,
    region: str,
    prefix: str,
    postgres_major: int,
    signing_key: str,
    toolchain_cache_dir: Path,
    cmake_cache_dir: Path,
) -> dict[str, str]:
    if not toolchain_cache_dir.is_absolute() or not cmake_cache_dir.is_absolute():
        raise PackageBuildError("toolchain and CMake cache directories must be absolute")
    environment = dict(base)
    environment.update(
        {
            "BIOSEQ_ARCHITECTURE": architecture,
            "BIOSEQ_POSTGRES_MAJOR": str(postgres_major),
            "BIOSEQ_OPTTIMIZATION_LEVEL": "0",
            "BIOSEQ_PACKAGE_NAME": f"bioseq-postgresql{postgres_major}",
            "BIOSEQ_S3_BUCKET": bucket,
            "BIOSEQ_S3_REGION": region,
            "BIOSEQ_S3_PREFIX": prefix,
            "BIOSEQ_TOOLCHAIN_CACHE_DIR": str(toolchain_cache_dir),
            "BIOSEQ_CMAKE_CACHE_DIR": str(cmake_cache_dir),
            "BIOSEQ_DEB_POSTGRES_RUNTIME": f"postgresql-{postgres_major}",
            "BIOSEQ_FEDORA_POSTGRES_RUNTIME": f"postgresql{postgres_major}-server",
            "BIOSEQ_OPENSUSE_POSTGRES_RUNTIME": f"postgresql{postgres_major}-server",
            "BIOSEQ_ARCH_POSTGRES_RUNTIME": (
                "postgresql-old-upgrade" if postgres_major == 17 else "postgresql"
            ),
            "BIOSEQ_ARCH_CONTAINER_IMAGE": (
                "archlinux:latest"
                if architecture == "x64"
                else base.get(
                    "BIOSEQ_ARCH_ARM64_CONTAINER_IMAGE", DEFAULT_ARCH_ARM64_IMAGE
                )
            ),
            "GPG_PRIVATE_KEY": signing_key,
        }
    )
    if environment["BIOSEQ_OPTTIMIZATION_LEVEL"] != "0":
        raise PackageBuildError("internal error: package optimization level is not zero")
    if not environment["BIOSEQ_ARCH_CONTAINER_IMAGE"].strip():
        raise PackageBuildError("resolved Arch container image is empty")
    return environment


def omnipackage_command(
    executable: Path,
    operation: str,
    source_dir: Path,
    build_dir: Path,
    distro: str,
    container_output: str,
) -> list[str]:
    return [
        str(executable),
        operation,
        str(source_dir),
        "--config-path",
        "infra/packaging/config.yml",
        "--env-file",
        "/dev/null",
        "--distros",
        distro,
        "--build-dir",
        str(build_dir),
        "--fail-fast",
        "--container-output",
        container_output,
    ]


def find_cell_artifact(build_dir: Path, package_name: str) -> Path:
    candidates = sorted(
        path
        for path in build_dir.rglob("*")
        if path.is_file()
        and package_name in path.name
        and path.name.endswith(PACKAGE_SUFFIXES)
    )
    if len(candidates) != 1:
        raise PackageBuildError(
            f"expected exactly one {package_name} artifact under {build_dir}; found {candidates}"
        )
    if candidates[0].stat().st_size <= 0:
        raise PackageBuildError(f"artifact is empty: {candidates[0]}")
    return candidates[0]


def validation_command(
    source_dir: Path,
    artifact: Path,
    architecture: str,
    postgres_major: int,
    distro: str,
) -> list[str]:
    postgres_runtime, vulkan_runtime = runtime_dependencies(distro, postgres_major)
    return [
        sys.executable,
        str(source_dir / "infra" / "packaging" / "scripts" / "validate_package.py"),
        "--artifact",
        str(artifact),
        "--architecture",
        architecture,
        "--postgres-major",
        str(postgres_major),
        "--postgres-runtime-package",
        postgres_runtime,
        "--vulkan-runtime-package",
        vulkan_runtime,
    ]


def parse_validation(output: str) -> dict[str, object]:
    try:
        parsed = json.loads(output)
    except json.JSONDecodeError as error:
        raise PackageBuildError(f"package validator returned invalid JSON: {output[-1000:]}") from error
    if not isinstance(parsed, dict) or parsed.get("passed") is not True:
        raise PackageBuildError(f"package validation failed: {parsed}")
    binaries = parsed.get("binaries")
    if not isinstance(binaries, list) or len(binaries) != 2 or not all(
        isinstance(binary, dict) for binary in binaries
    ):
        raise PackageBuildError("package validator must attest exactly two ELF DSOs")
    binary_names = {Path(str(binary.get("path", ""))).name for binary in binaries}
    if binary_names != {"bioseq.so", "bioseq_cache.so"}:
        raise PackageBuildError(
            f"package validator attested unexpected DSOs: {sorted(binary_names)}"
        )
    machines: set[str] = set()
    for binary in binaries:
        machine = str(binary.get("elf_architecture", ""))
        if "AArch64" in machine:
            machines.add("aarch64")
        elif "X86-64" in machine or "Advanced Micro Devices X86-64" in machine:
            machines.add("x86_64")
        else:
            raise PackageBuildError(f"unknown ELF architecture attestation {machine!r}")
    if len(machines) != 1:
        raise PackageBuildError("package DSOs do not have one consistent ELF architecture")
    parsed["elf_architecture"] = next(iter(machines))
    parsed["rpath_absent"] = all(
        binary.get("rpath_absent") is True for binary in binaries
    )
    parsed["compiler_runtime_safe"] = all(
        binary.get("compiler_runtime_safe") is True for binary in binaries
    )
    if not parsed["rpath_absent"] or not parsed["compiler_runtime_safe"]:
        raise PackageBuildError("package validator did not prove portable ELF linkage")
    if parsed.get("optimization_level") != 0:
        raise PackageBuildError("package validator did not attest optimization level zero")
    return parsed


def smoke_command(
    source_dir: Path,
    artifact: Path,
    architecture: str,
    postgres_major: int,
    distro: str,
    arch_image: str,
) -> list[str]:
    return [
        sys.executable,
        str(source_dir / "infra" / "packaging" / "scripts" / "smoke_test_package.py"),
        "--artifact",
        str(artifact),
        "--architecture",
        architecture,
        "--postgres-major",
        str(postgres_major),
        "--distro",
        distro,
        "--arch-image",
        arch_image,
    ]


def parse_smoke(output: str) -> dict[str, object]:
    try:
        parsed = json.loads(output)
    except json.JSONDecodeError as error:
        raise PackageBuildError(
            f"installed-package smoke returned invalid JSON: {output[-1000:]}"
        ) from error
    required_true = (
        "passed",
        "package_installed",
        "preload_active",
        "bioseq_extension_created",
        "bioseq_cache_extension_created",
        "nonempty_roundtrip",
    )
    if not isinstance(parsed, dict) or any(parsed.get(key) is not True for key in required_true):
        raise PackageBuildError(f"installed-package PostgreSQL smoke failed: {parsed}")
    if parsed.get("roundtrip") != "ACGT":
        raise PackageBuildError("installed-package smoke returned the wrong Bioseq value")
    return parsed


def s3_artifact_key(cell_prefix: str, distro: str, fmt: str, filename: str) -> str:
    if fmt not in {"deb", "rpm", "pacman"}:
        raise PackageBuildError(f"unsupported S3 package format {fmt}")
    directory = f"{cell_prefix}/{distro}"
    if fmt == "deb":
        directory += "/stable"
    return f"{directory}/{filename}"


def s3_head(
    aws_bin: str, bucket: str, key: str, region: str, environment: dict[str, str]
) -> dict[str, object]:
    output = run_captured(
        [
            aws_bin,
            "s3api",
            "head-object",
            "--bucket",
            bucket,
            "--key",
            key,
            "--region",
            region,
            "--output",
            "json",
        ],
        environment=environment,
    )
    try:
        result = json.loads(output)
    except json.JSONDecodeError as error:
        raise PackageBuildError("S3 head-object returned invalid JSON") from error
    if int(result.get("ContentLength", 0)) <= 0:
        raise PackageBuildError(f"uploaded S3 object is empty: s3://{bucket}/{key}")
    return result


def download_s3_object(
    *,
    aws_bin: str,
    bucket: str,
    key: str,
    region: str,
    destination: Path,
    environment: dict[str, str],
) -> dict[str, object]:
    head = s3_head(aws_bin, bucket, key, region, environment)
    destination.parent.mkdir(parents=True, exist_ok=True)
    run_captured(
        [
            aws_bin,
            "s3api",
            "get-object",
            "--bucket",
            bucket,
            "--key",
            key,
            "--region",
            region,
            str(destination),
        ],
        environment=environment,
    )
    size = destination.stat().st_size
    if size != int(head["ContentLength"]):
        raise PackageBuildError(f"downloaded size differs from S3 metadata for {key}")
    return {
        "key": key,
        "size": size,
        "sha256": sha256_file(destination),
        "etag": str(head.get("ETag", "")).strip('"'),
    }


def _gpg_import_and_verify(
    key: Path,
    signatures: Sequence[tuple[Path, Path | None]],
    work_dir: Path,
    environment: dict[str, str],
) -> str:
    gnupg_home = work_dir / "gnupg"
    gnupg_home.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(gnupg_home, 0o700)
    gpg = shutil.which("gpg", path=environment.get("PATH"))
    if not gpg:
        raise PackageBuildError("gpg is required to verify published repository signatures")
    run_captured(
        [gpg, "--batch", "--no-tty", "--homedir", str(gnupg_home), "--import", str(key)],
        environment=environment,
    )
    fingerprint = _parse_gpg_fingerprint(
        run_captured(
            [gpg, "--batch", "--no-tty", "--with-colons", "--show-keys", str(key)],
            environment=environment,
        )
    )
    for signature, payload in signatures:
        command = [
            gpg,
            "--batch",
            "--no-tty",
            "--homedir",
            str(gnupg_home),
            "--verify",
            str(signature),
        ]
        if payload is not None:
            command.append(str(payload))
        run_captured(
            command,
            environment=environment,
        )
    return fingerprint


def _verify_deb_release_artifact(
    release: Path, artifact: Path, expected_sha256: str, expected_size: int
) -> None:
    entries: dict[str, tuple[str, int]] = {}
    in_sha256 = False
    for line in release.read_text(encoding="utf-8").splitlines():
        if line == "SHA256:":
            in_sha256 = True
            continue
        if in_sha256 and not line.startswith(" "):
            break
        if in_sha256:
            fields = line.split()
            if len(fields) == 3 and fields[1].isdigit():
                entries[fields[2]] = (fields[0], int(fields[1]))
    attested = entries.get(artifact.name)
    if attested != (expected_sha256, expected_size):
        raise PackageBuildError(
            "signed DEB Release does not attest the downloaded package SHA-256 and size"
        )


def verify_published_signatures(
    *,
    aws_bin: str,
    bucket: str,
    region: str,
    base_key: str,
    artifact_key: str,
    artifact: Path,
    fmt: str,
    package_name: str,
    expected_signing_fingerprint: str,
    work_dir: Path,
    environment: dict[str, str],
) -> dict[str, object]:
    work_dir.mkdir(parents=True, exist_ok=True)
    objects: list[dict[str, object]] = []

    def fetch(relative_key: str, local_name: str) -> Path:
        destination = work_dir / local_name
        objects.append(
            download_s3_object(
                aws_bin=aws_bin,
                bucket=bucket,
                key=f"{base_key}/{relative_key}",
                region=region,
                destination=destination,
                environment=environment,
            )
        )
        return destination

    if fmt == "rpm":
        public_key = fetch("repodata/repomd.xml.key", "repomd.xml.key")
        repomd = fetch("repodata/repomd.xml", "repomd.xml")
        repomd_signature = fetch("repodata/repomd.xml.asc", "repomd.xml.asc")
        actual_fingerprint = _gpg_import_and_verify(
            public_key,
            [(repomd_signature, repomd)],
            work_dir,
            environment,
        )
        rpmkeys = shutil.which("rpmkeys", path=environment.get("PATH"))
        rpm = shutil.which("rpm", path=environment.get("PATH"))
        if not rpmkeys or not rpm:
            raise PackageBuildError("rpm and rpmkeys are required to verify the published RPM signature")
        rpm_database = work_dir / "rpmdb"
        rpm_database.mkdir()
        run_captured(
            [rpm, "--dbpath", str(rpm_database), "--initdb"],
            environment=environment,
        )
        run_captured(
            [rpmkeys, "--dbpath", str(rpm_database), "--import", str(public_key)],
            environment=environment,
        )
        check_output = run_captured(
            [rpmkeys, "--dbpath", str(rpm_database), "--checksig", str(artifact)],
            environment=environment,
        )
        if "NOT OK" in check_output or not re.search(r"\bOK\b", check_output):
            raise PackageBuildError(f"published RPM signature check failed: {check_output.strip()}")
        package_signature = "embedded-rpm-openpgp"
    elif fmt == "deb":
        public_key = fetch("stable/Release.key", "Release.key")
        release = fetch("stable/Release", "Release")
        release_signature = fetch("stable/Release.gpg", "Release.gpg")
        inrelease = fetch("stable/InRelease", "InRelease")
        actual_fingerprint = _gpg_import_and_verify(
            public_key,
            [(release_signature, release), (inrelease, None)],
            work_dir,
            environment,
        )
        _verify_deb_release_artifact(
            release,
            artifact,
            sha256_file(artifact),
            artifact.stat().st_size,
        )
        package_signature = "signed-deb-release-sha256"
    elif fmt == "pacman":
        public_key = fetch("public.key", "public.key")
        package_signature_path = download_s3_object(
            aws_bin=aws_bin,
            bucket=bucket,
            key=f"{artifact_key}.sig",
            region=region,
            destination=work_dir / f"{artifact.name}.sig",
            environment=environment,
        )
        objects.append(package_signature_path)
        database_name = f"{package_name}.db.tar.gz"
        database = fetch(database_name, database_name)
        database_signature = fetch(f"{database_name}.sig", f"{database_name}.sig")
        actual_fingerprint = _gpg_import_and_verify(
            public_key,
            [
                (work_dir / f"{artifact.name}.sig", artifact),
                (database_signature, database),
            ],
            work_dir,
            environment,
        )
        package_signature = "detached-pacman-openpgp"
    else:
        raise PackageBuildError(f"unsupported signature verification format {fmt}")

    if actual_fingerprint != expected_signing_fingerprint:
        raise PackageBuildError(
            "published repository key fingerprint differs from the Secrets Manager signing key"
        )

    return {
        "passed": True,
        "format": fmt,
        "public_key_imported": True,
        "signing_key_fingerprint": actual_fingerprint,
        "package_signature_verified": True,
        "package_signature": package_signature,
        "repository_signature_verified": True,
        "objects": objects,
    }


def argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--architecture", choices=("arm64", "x64"), required=True)
    parser.add_argument("--bucket", required=True)
    parser.add_argument("--region", required=True)
    parser.add_argument(
        "--postgres",
        action="append",
        required=True,
        help="PostgreSQL major; repeat or pass a comma-separated list",
    )
    parser.add_argument(
        "--distros",
        action="append",
        required=True,
        help="OmniPackage distro ID; repeat or pass a comma-separated list",
    )
    parser.add_argument("--signing-secret-arn")
    parser.add_argument(
        "--source-dir", type=Path, default=Path(__file__).resolve().parents[2]
    )
    parser.add_argument("--build-root", type=Path, default=DEFAULT_BUILD_ROOT)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--s3-prefix", default=DEFAULT_S3_PREFIX)
    parser.add_argument("--omnipackage-bin")
    parser.add_argument("--aws-bin", default="aws")
    parser.add_argument(
        "--container-output", choices=("null", "stdout", "stderr"), default="stdout"
    )
    parser.add_argument("--fail-fast", action="store_true")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = argument_parser().parse_args(argv)
    manifest_path = args.manifest or args.build_root / "manifest.json"
    started_at = utc_now()
    manifest: dict[str, object] = {
        "schema_version": MANIFEST_SCHEMA_VERSION,
        "package_version": PACKAGE_VERSION,
        "architecture": args.architecture,
        "status": "failed",
        "started_at": started_at,
        "completed_at": None,
        "requested": {"postgres": [], "distros": []},
        "s3": {
            "bucket": args.bucket,
            "region": args.region,
            "base_prefix": args.s3_prefix,
        },
        "signing": {"fingerprint": None},
        "cells": [],
        "errors": [],
    }

    try:
        postgres_versions = parse_postgres(args.postgres)
        distros = parse_distros(args.distros)
        bucket = validate_bucket(args.bucket)
        prefix = validate_prefix(args.s3_prefix)
        source_dir = args.source_dir.resolve()
        if source_dir.name != "main" or not (source_dir / "CMakeLists.txt").is_file():
            raise PackageBuildError(
                f"--source-dir must name the bioseq main source directory: {source_dir}"
            )
        if native_architecture() != args.architecture:
            raise PackageBuildError(
                f"requested {args.architecture} on native {native_architecture()} worker"
            )
        omnipackage = resolve_omnipackage(
            args.omnipackage_bin,
            source_dir,
            environment=dict(os.environ),
        )
        signing_key = fetch_signing_key(
            secret_arn=args.signing_secret_arn,
            aws_bin=args.aws_bin,
            region=args.region,
            environment=dict(os.environ),
        )
        signing_fingerprint = signing_key_fingerprint(signing_key, dict(os.environ))
        args.build_root.mkdir(parents=True, exist_ok=True)

        manifest["requested"] = {
            "postgres": postgres_versions,
            "distros": distros,
        }
        manifest["s3"] = {
            "bucket": bucket,
            "region": args.region,
            "base_prefix": prefix,
        }
        manifest["signing"] = {"fingerprint": signing_fingerprint}
        write_manifest(manifest_path, manifest)

        for postgres_major in postgres_versions:
            for distro in distros:
                package_name = f"bioseq-postgresql{postgres_major}"
                cell_prefix = f"{prefix}/{args.architecture}/postgresql{postgres_major}"
                cell_build_dir = (
                    args.build_root
                    / "cells"
                    / f"postgresql{postgres_major}"
                    / distro
                )
                cell: dict[str, object] = {
                    "postgres_major": postgres_major,
                    "distro": distro,
                    "package_name": package_name,
                    "status": "building",
                    "build_dir": str(cell_build_dir),
                    "artifact": None,
                    "validation": None,
                    "smoke": None,
                    "upload": None,
                    "error": None,
                }
                cells = manifest["cells"]
                assert isinstance(cells, list)
                cells.append(cell)
                write_manifest(manifest_path, manifest)

                try:
                    if cell_build_dir.exists():
                        shutil.rmtree(cell_build_dir)
                    cell_build_dir.mkdir(parents=True)
                    revision_root = source_dir.parent
                    workspace_root = (
                        revision_root.parent
                        if revision_root.name.startswith("source-")
                        else revision_root
                    )
                    toolchain_cache_dir = (
                        workspace_root
                        / "caches"
                        / "package-toolchains"
                        / distro
                        / f"postgresql{postgres_major}"
                    )
                    cmake_cache_dir = (
                        workspace_root
                        / "caches"
                        / "package-cmake"
                        / distro
                        / f"postgresql{postgres_major}"
                    )
                    toolchain_cache_dir.mkdir(parents=True, exist_ok=True)
                    cmake_cache_dir.mkdir(parents=True, exist_ok=True)
                    environment = cell_environment(
                        dict(os.environ),
                        architecture=args.architecture,
                        bucket=bucket,
                        region=args.region,
                        prefix=cell_prefix,
                        postgres_major=postgres_major,
                        signing_key=signing_key,
                        toolchain_cache_dir=toolchain_cache_dir,
                        cmake_cache_dir=cmake_cache_dir,
                    )
                    build_command = omnipackage_command(
                        omnipackage,
                        "build",
                        source_dir,
                        cell_build_dir,
                        distro,
                        args.container_output,
                    )
                    run_streamed(build_command, environment=environment)
                    artifact = find_cell_artifact(cell_build_dir, package_name)
                    artifact_data = {
                        "path": str(artifact),
                        "filename": artifact.name,
                        "size": artifact.stat().st_size,
                        "sha256": sha256_file(artifact),
                        "format": artifact_format(artifact),
                        "build_path": str(artifact),
                        "build_size": artifact.stat().st_size,
                        "build_sha256": sha256_file(artifact),
                    }
                    cell["artifact"] = artifact_data
                    validation = parse_validation(
                        run_captured(
                            validation_command(
                                source_dir,
                                artifact,
                                args.architecture,
                                postgres_major,
                                distro,
                            )
                        )
                    )
                    cell["validation"] = validation
                    cell["status"] = "validated"
                    write_manifest(manifest_path, manifest)

                    smoke = parse_smoke(
                        run_captured(
                            smoke_command(
                                source_dir,
                                artifact,
                                args.architecture,
                                postgres_major,
                                distro,
                                environment["BIOSEQ_ARCH_CONTAINER_IMAGE"],
                            ),
                            environment=dict(os.environ),
                        )
                    )
                    cell["smoke"] = smoke
                    cell["status"] = "smoked"
                    write_manifest(manifest_path, manifest)

                    publish_command = omnipackage_command(
                        omnipackage,
                        "publish",
                        source_dir,
                        cell_build_dir,
                        distro,
                        args.container_output,
                    ) + ["--repository", "s3"]
                    run_streamed(publish_command, environment=environment)
                    key = s3_artifact_key(
                        cell_prefix,
                        distro,
                        str(artifact_data["format"]),
                        artifact.name,
                    )
                    head = s3_head(args.aws_bin, bucket, key, args.region, dict(os.environ))

                    downloaded = cell_build_dir / "verified-upload" / artifact.name
                    downloaded.parent.mkdir(parents=True, exist_ok=True)
                    run_captured(
                        [
                            args.aws_bin,
                            "s3api",
                            "get-object",
                            "--bucket",
                            bucket,
                            "--key",
                            key,
                            "--region",
                            args.region,
                            str(downloaded),
                        ],
                        environment=dict(os.environ),
                    )
                    uploaded_validation = parse_validation(
                        run_captured(
                            validation_command(
                                source_dir,
                                downloaded,
                                args.architecture,
                                postgres_major,
                                distro,
                            )
                        )
                    )
                    downloaded_smoke = parse_smoke(
                        run_captured(
                            smoke_command(
                                source_dir,
                                downloaded,
                                args.architecture,
                                postgres_major,
                                distro,
                                environment["BIOSEQ_ARCH_CONTAINER_IMAGE"],
                            ),
                            environment=dict(os.environ),
                        )
                    )
                    downloaded_size = downloaded.stat().st_size
                    downloaded_sha256 = sha256_file(downloaded)
                    if downloaded_size != int(head["ContentLength"]):
                        raise PackageBuildError(
                            "downloaded package size does not match S3 head-object"
                        )
                    if str(artifact_data["format"]) != "rpm" and (
                        downloaded_size != int(artifact_data["build_size"])
                        or downloaded_sha256 != str(artifact_data["build_sha256"])
                    ):
                        raise PackageBuildError(
                            "published DEB/pacman package bytes differ from the validated build artifact"
                        )
                    signature_verification = verify_published_signatures(
                        aws_bin=args.aws_bin,
                        bucket=bucket,
                        region=args.region,
                        base_key=f"{cell_prefix}/{distro}",
                        artifact_key=key,
                        artifact=downloaded,
                        fmt=str(artifact_data["format"]),
                        package_name=package_name,
                        expected_signing_fingerprint=signing_fingerprint,
                        work_dir=cell_build_dir / "verified-signatures",
                        environment=dict(os.environ),
                    )
                    artifact_data.update(
                        {
                            "path": str(downloaded),
                            "size": downloaded_size,
                            "sha256": downloaded_sha256,
                        }
                    )
                    cell["upload"] = {
                        "published": True,
                        "bucket": bucket,
                        "key": key,
                        "etag": str(head.get("ETag", "")).strip('"'),
                        "size": int(head["ContentLength"]),
                        "sha256": downloaded_sha256,
                        "download_validation": uploaded_validation,
                        "download_smoke": downloaded_smoke,
                        "signature_verification": signature_verification,
                    }
                    cell["status"] = "uploaded"
                except Exception as error:  # cell failure must still reach manifest
                    cell["status"] = "failed"
                    cell["error"] = str(error)
                    errors = manifest["errors"]
                    assert isinstance(errors, list)
                    errors.append(
                        {
                            "postgres_major": postgres_major,
                            "distro": distro,
                            "error": str(error),
                        }
                    )
                    write_manifest(manifest_path, manifest)
                    if args.fail_fast:
                        raise
                write_manifest(manifest_path, manifest)

        cells = manifest["cells"]
        assert isinstance(cells, list)
        expected_cells = len(postgres_versions) * len(distros)
        all_uploaded = len(cells) == expected_cells and all(
            isinstance(cell, dict)
            and cell.get("status") == "uploaded"
            and isinstance(cell.get("smoke"), dict)
            and cell["smoke"].get("passed") is True
            and isinstance(cell.get("upload"), dict)
            and cell["upload"].get("published") is True
            and isinstance(cell["upload"].get("download_smoke"), dict)
            and cell["upload"]["download_smoke"].get("passed") is True
            and isinstance(cell["upload"].get("signature_verification"), dict)
            and cell["upload"]["signature_verification"].get("passed") is True
            and cell["upload"]["signature_verification"].get("package_signature_verified") is True
            and cell["upload"]["signature_verification"].get("repository_signature_verified") is True
            and isinstance(cell.get("artifact"), dict)
            and cell["artifact"].get("sha256") == cell["upload"].get("sha256")
            for cell in cells
        )
        manifest["status"] = "success" if all_uploaded else "failed"
        if not all_uploaded:
            raise PackageBuildError(
                f"only {sum(isinstance(cell, dict) and cell.get('status') == 'uploaded' for cell in cells)} "
                f"of {expected_cells} requested package cells reached verified upload"
            )
    except Exception as error:
        errors = manifest["errors"]
        assert isinstance(errors, list)
        if not any(
            isinstance(item, dict) and item.get("error") == str(error) for item in errors
        ):
            errors.append({"error": str(error)})
        manifest["status"] = "failed"
    finally:
        manifest["completed_at"] = utc_now()
        try:
            write_manifest(manifest_path, manifest)
        except OSError as error:
            print(f"failed to write package manifest {manifest_path}: {error}", file=sys.stderr)
            return 1

    if manifest["status"] != "success":
        print(f"package matrix failed; see {manifest_path}", file=sys.stderr)
        return 1
    print(f"package matrix uploaded successfully; manifest: {manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
