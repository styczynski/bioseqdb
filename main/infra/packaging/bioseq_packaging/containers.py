"""Native Docker planning with a fixed, non-extensible mount policy."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from pathlib import PurePosixPath
import re
import sys

from .commands import Command, CommandRunner
from .model import (
    BindMount,
    CellContext,
    ContainerCapability,
    ContainerOutput,
    ContainerPlan,
    ContainerPurpose,
    PackageBuildError,
)


SOURCE_TARGET = PurePosixPath("/source")
WORK_TARGET = PurePosixPath("/work")
TOOLCHAIN_TARGET = PurePosixPath("/opt/bioseq-package-toolchain")
CMAKE_TARGET = PurePosixPath("/opt/bioseq-package-cmake-cache")
_ENVIRONMENT_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
_CONTAINER_NAME = re.compile(r"[^a-zA-Z0-9_.-]+")
_SIGNING_ENVIRONMENT = "GPG_PRIVATE_KEY"


def _fixed_mounts(context: CellContext) -> tuple[BindMount, ...]:
    return (
        BindMount(context.request.source_dir.resolve(), SOURCE_TARGET, True),
        BindMount(context.paths.root.resolve(), WORK_TARGET, False),
        BindMount(context.paths.toolchain_cache.resolve(), TOOLCHAIN_TARGET, False),
        BindMount(context.paths.cmake_cache.resolve(), CMAKE_TARGET, False),
    )


def native_container_plan(
    context: CellContext,
    purpose: ContainerPurpose | str,
    argv: Sequence[str],
    environment_names: Sequence[str] = (),
) -> ContainerPlan:
    normalized_purpose = ContainerPurpose(purpose)
    names = tuple(dict.fromkeys(environment_names))
    invalid_names = [name for name in names if _ENVIRONMENT_NAME.fullmatch(name) is None]
    if invalid_names:
        raise PackageBuildError(f"invalid container environment names: {invalid_names}")
    if _SIGNING_ENVIRONMENT in names and normalized_purpose is not ContainerPurpose.REPOSITORY:
        raise PackageBuildError("the signing key is restricted to repository containers")
    capabilities = (
        context.distro.capabilities
        if normalized_purpose is ContainerPurpose.BUILD
        else frozenset()
    )
    if capabilities - {ContainerCapability.SYS_NICE}:
        raise PackageBuildError("container requested an unsupported capability")
    raw_name = (
        f"bioseq-{context.request.architecture.value}-pg{context.postgres_major}-"
        f"{context.distro.distro_id.value}-{normalized_purpose.value}"
    )
    name = _CONTAINER_NAME.sub("-", raw_name).strip("-.")
    return ContainerPlan(
        image=context.distro.image_for(context.request.architecture),
        argv=tuple(argv),
        mounts=_fixed_mounts(context),
        environment_names=names,
        capabilities=frozenset(capabilities),
        output=context.request.container_output,
        name=name,
        purpose=normalized_purpose,
    )


def docker_command(plan: ContainerPlan, *, docker_bin: str = "docker") -> Command:
    if any(value == "--platform" or value.startswith("--platform=") for value in plan.argv):
        raise PackageBuildError("cross-architecture Docker --platform is forbidden")
    argv: list[str] = [docker_bin, "run", "--rm", "--init"]
    if plan.name:
        argv.extend(("--name", plan.name))
    for mount in plan.mounts:
        value = f"type=bind,src={mount.source},dst={mount.target}"
        if mount.read_only:
            value += ",readonly"
        argv.extend(("--mount", value))
    for capability in sorted(plan.capabilities, key=str):
        if capability is not ContainerCapability.SYS_NICE:
            raise PackageBuildError(f"unsupported Docker capability {capability}")
        argv.extend(("--cap-add", capability.value))
    for name in plan.environment_names:
        argv.extend(("--env", name))
    argv.append(plan.image)
    argv.extend(plan.argv)
    if any(value == "--platform" or value.startswith("--platform=") for value in argv):
        raise PackageBuildError("cross-architecture Docker --platform is forbidden")
    sensitive = frozenset(
        name for name in plan.environment_names if name == _SIGNING_ENVIRONMENT
    )
    return Command(
        argv=tuple(argv),
        sensitive_environment=sensitive,
        suppress_output_on_failure=bool(sensitive),
    )


class ContainerRunner:
    def __init__(
        self,
        command_runner: CommandRunner | None = None,
        *,
        docker_bin: str = "docker",
    ) -> None:
        self.command_runner = command_runner or CommandRunner()
        self.docker_bin = docker_bin

    def run(self, plan: ContainerPlan, *, environment: Mapping[str, str]) -> None:
        missing = [name for name in plan.environment_names if name not in environment]
        if missing:
            raise PackageBuildError(
                f"container environment is missing declared values: {missing}"
            )
        command = docker_command(plan, docker_bin=self.docker_bin)
        if plan.output is ContainerOutput.NULL:
            self.command_runner.run_captured(command, environment=environment)
            return
        if plan.output is ContainerOutput.STDERR:
            output = self.command_runner.run_captured(command, environment=environment)
            if output:
                print(output, end="", file=sys.stderr, flush=True)
            return
        self.command_runner.run_streamed(command, environment=environment)

    def capture(self, plan: ContainerPlan, *, environment: Mapping[str, str]) -> str:
        missing = [name for name in plan.environment_names if name not in environment]
        if missing:
            raise PackageBuildError(
                f"container environment is missing declared values: {missing}"
            )
        return self.command_runner.run_captured(
            docker_command(plan, docker_bin=self.docker_bin),
            environment=environment,
        )


_DEFAULT_RUNNER = ContainerRunner()


def run(plan: ContainerPlan, *, environment: Mapping[str, str]) -> None:
    _DEFAULT_RUNNER.run(plan, environment=environment)
