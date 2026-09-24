#!/usr/bin/env python3
"""Install one package in a fresh native container and exercise PostgreSQL."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
from typing import Sequence


DISTRO_IMAGES = {
    "opensuse_16.0": "opensuse/leap:16.0",
    "fedora_44": "fedora:44",
    "debian_13": "debian:trixie",
    "ubuntu_26.04": "ubuntu:26.04",
}
PACKAGE_SUFFIXES = (".deb", ".rpm", ".pkg.tar.zst")


class SmokeError(RuntimeError):
    pass


def native_architecture() -> str:
    machine = platform.machine().lower()
    if machine in {"x86_64", "amd64"}:
        return "x64"
    if machine in {"aarch64", "arm64"}:
        return "arm64"
    raise SmokeError(f"unsupported native machine architecture {machine!r}")


def container_image(distro: str, arch_image: str) -> str:
    if distro == "arch":
        if not arch_image.strip():
            raise SmokeError("the native Arch container image is empty")
        return arch_image
    try:
        return DISTRO_IMAGES[distro]
    except KeyError as error:
        raise SmokeError(f"unsupported smoke-test distro {distro!r}") from error


def docker_command(
    *,
    docker: str,
    source_dir: Path,
    artifact: Path,
    architecture: str,
    postgres_major: int,
    distro: str,
    arch_image: str,
    postgres_runtime_artifact: Path | None = None,
) -> tuple[list[str], str]:
    image = container_image(distro, arch_image)
    script = source_dir / "infra" / "packaging" / "scripts" / "smoke_installed_package.sh"
    if not script.is_file():
        raise SmokeError(f"container smoke script is missing: {script}")
    if not artifact.is_file() or artifact.stat().st_size <= 0:
        raise SmokeError(f"package artifact is missing or empty: {artifact}")
    if not artifact.name.endswith(PACKAGE_SUFFIXES):
        raise SmokeError(f"unsupported package artifact: {artifact.name}")

    package_target = f"/artifact/{artifact.name}"
    command = [
        docker,
        "run",
        "--rm",
        "--init",
        # PostgreSQL parallel query DSM can exceed Docker's 64 MiB default.
        # This is a tmpfs ceiling, not a preallocation or a planner override.
        "--shm-size=1g",
        "--mount",
        f"type=bind,src={artifact.resolve()},dst={package_target},readonly",
        "--mount",
        f"type=bind,src={script.resolve()},dst=/smoke/smoke_installed_package.sh,readonly",
        "--env",
        f"BIOSEQ_SMOKE_DISTRO={distro}",
        "--env",
        f"BIOSEQ_POSTGRES_MAJOR={postgres_major}",
        "--env",
        f"BIOSEQ_ARCHITECTURE={architecture}",
        "--env",
        f"BIOSEQ_PACKAGE_PATH={package_target}",
    ]
    if postgres_runtime_artifact is not None:
        runtime = postgres_runtime_artifact.resolve()
        if (not runtime.is_file() or runtime.stat().st_size <= 0
                or not runtime.name.startswith(f'bioseq-postgresql{postgres_major}-runtime-')
                or not runtime.name.endswith(('.rpm', '.pkg.tar.zst'))):
            raise SmokeError('Invalid native PostgreSQL SDK companion package')
        runtime_target = '/runtime/' + runtime.name
        command.extend(['--mount', f'type=bind,src={runtime},dst={runtime_target},readonly',
                        '--env', f'BIOSEQ_POSTGRES_RUNTIME_PACKAGE={runtime_target}'])
    command.extend([image, '/bin/bash', '/smoke/smoke_installed_package.sh'])
    if "--platform" in command or any(value.startswith("--platform=") for value in command):
        raise SmokeError("package smoke must not request architecture emulation")
    return command, image


def run_smoke(command: Sequence[str]) -> str:
    try:
        completed = subprocess.run(
            list(command),
            check=True,
            capture_output=True,
            text=True,
        )
    except FileNotFoundError as error:
        raise SmokeError(f"container runtime is missing: {command[0]}") from error
    except subprocess.CalledProcessError as error:
        detail = (error.stderr or error.stdout or "").strip()
        if len(detail) > 4000:
            detail = detail[-4000:]
        raise SmokeError(f"fresh-container PostgreSQL smoke failed: {detail}") from error
    return completed.stdout


def parse_result(
    output: str,
    *,
    distro: str,
    architecture: str,
    postgres_major: int,
    image: str,
) -> dict[str, object]:
    try:
        result = json.loads(output)
    except json.JSONDecodeError as error:
        raise SmokeError(f"container smoke emitted invalid JSON: {output[-1000:]}") from error
    true_fields = (
        "passed",
        "package_installed",
        "preload_active",
        "bioseq_extension_created",
        "bioseq_cache_extension_created",
        "nonempty_roundtrip",
    )
    if not isinstance(result, dict) or any(result.get(key) is not True for key in true_fields):
        raise SmokeError(f"container smoke did not pass every assertion: {result}")
    if (
        result.get("distro") != distro
        or result.get("architecture") != architecture
        or result.get("postgres_major") != postgres_major
        or result.get("roundtrip") != "ACGT"
    ):
        raise SmokeError(f"container smoke attested the wrong matrix cell: {result}")
    result["container_image"] = image
    result["fresh_container"] = True
    return result


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--artifact", type=Path, required=True)
    result.add_argument("--architecture", choices=("arm64", "x64"), required=True)
    result.add_argument("--postgres-major", type=int, required=True)
    result.add_argument(
        "--distro",
        choices=(*DISTRO_IMAGES.keys(), "arch"),
        required=True,
    )
    result.add_argument("--arch-image", required=True)
    result.add_argument("--docker-bin", default=os.environ.get("BIOSEQ_DOCKER_BIN", "docker"))
    result.add_argument("--postgres-runtime-artifact", type=Path)
    result.add_argument(
        "--source-dir",
        type=Path,
        default=Path(__file__).resolve().parents[3],
    )
    return result


def main(argv: Sequence[str] | None = None) -> int:
    arguments = parser().parse_args(argv)
    try:
        if arguments.postgres_major < 17:
            raise SmokeError("PostgreSQL majors below 17 are unsupported")
        if native_architecture() != arguments.architecture:
            raise SmokeError(
                f"requested {arguments.architecture} smoke on native {native_architecture()} worker"
            )
        command, image = docker_command(
            docker=arguments.docker_bin,
            source_dir=arguments.source_dir.resolve(),
            artifact=arguments.artifact.resolve(),
            architecture=arguments.architecture,
            postgres_major=arguments.postgres_major,
            distro=arguments.distro,
            arch_image=arguments.arch_image,
            postgres_runtime_artifact=arguments.postgres_runtime_artifact,
        )
        result = parse_result(
            run_smoke(command),
            distro=arguments.distro,
            architecture=arguments.architecture,
            postgres_major=arguments.postgres_major,
            image=image,
        )
    except SmokeError as error:
        print(str(error), file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
