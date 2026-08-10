"""Argv-only command execution with centralized secret redaction."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import os
import re
import shlex
import subprocess
import sys
from typing import Mapping, Sequence

from .model import PackageBuildError


_ENVIRONMENT_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")


class Redactor:
    """Redact registered secret representations from errors and buffered logs."""

    def __init__(self, values: Sequence[str | bytes] = ()) -> None:
        self._values: set[str] = set()
        self.register(*values)

    def register(self, *values: str | bytes) -> None:
        for value in values:
            if isinstance(value, bytes):
                normalized = value.decode("utf-8", errors="ignore")
            else:
                normalized = value
            if normalized:
                self._values.add(normalized)

    def redact(self, value: object) -> str:
        rendered = str(value)
        for secret in sorted(self._values, key=len, reverse=True):
            rendered = rendered.replace(secret, "<redacted>")
        return rendered


@dataclass(frozen=True, slots=True)
class Command:
    argv: tuple[str, ...]
    cwd: Path | None = None
    sensitive_environment: frozenset[str] = frozenset()
    suppress_output_on_failure: bool = False

    def __post_init__(self) -> None:
        if not self.argv or any(not isinstance(value, str) or not value for value in self.argv):
            raise PackageBuildError("command argv must contain non-empty strings")
        invalid = [
            name for name in self.sensitive_environment if _ENVIRONMENT_NAME.fullmatch(name) is None
        ]
        if invalid:
            raise PackageBuildError(f"invalid sensitive environment names: {sorted(invalid)}")
        assignments = [
            argument
            for argument in self.argv
            for name in self.sensitive_environment
            if argument.startswith(f"{name}=")
        ]
        if assignments:
            raise PackageBuildError(
                "sensitive environment values must never be embedded in command argv"
            )


class CommandError(PackageBuildError):
    """A redacted child-process failure."""


def safe_command(command: Command) -> str:
    rendered = shlex.join(command.argv)
    if command.sensitive_environment:
        names = ", ".join(
            f"{name}=<redacted>" for name in sorted(command.sensitive_environment)
        )
        rendered = f"{rendered} [sensitive environment: {names}]"
    return rendered


class CommandRunner:
    def __init__(self, redactor: Redactor | None = None) -> None:
        self.redactor = redactor or Redactor()

    def _environment(self, environment: Mapping[str, str]) -> dict[str, str]:
        result = {str(key): str(value) for key, value in environment.items()}
        if "PATH" not in result:
            result["PATH"] = os.defpath
        return result

    def _register_sensitive_environment(
        self, command: Command, environment: Mapping[str, str]
    ) -> None:
        for name in command.sensitive_environment:
            value = environment.get(name)
            if value:
                self.redactor.register(str(value))

    def _failure(self, command: Command, error: subprocess.CalledProcessError) -> CommandError:
        if command.suppress_output_on_failure:
            detail = "child output suppressed"
        else:
            raw = error.stderr or error.stdout or ""
            detail = self.redactor.redact(raw.strip())
            if len(detail) > 2000:
                detail = detail[-2000:]
            if not detail:
                detail = "no child output"
        rendered = self.redactor.redact(safe_command(command))
        return CommandError(f"command exited {error.returncode}: {rendered}: {detail}")

    def run_streamed(self, command: Command, *, environment: Mapping[str, str]) -> None:
        self._register_sensitive_environment(command, environment)
        rendered = self.redactor.redact(safe_command(command))
        print(f"+ {rendered}", flush=True)
        try:
            if command.sensitive_environment:
                completed = subprocess.run(
                    list(command.argv),
                    check=True,
                    cwd=command.cwd,
                    env=self._environment(environment),
                    capture_output=True,
                    text=True,
                )
                if completed.stdout:
                    print(self.redactor.redact(completed.stdout), end="", flush=True)
                if completed.stderr:
                    print(
                        self.redactor.redact(completed.stderr),
                        end="",
                        file=sys.stderr,
                        flush=True,
                    )
            else:
                subprocess.run(
                    list(command.argv),
                    check=True,
                    cwd=command.cwd,
                    env=self._environment(environment),
                )
        except FileNotFoundError as error:
            raise CommandError(
                f"required executable is missing: {self.redactor.redact(command.argv[0])}"
            ) from error
        except subprocess.CalledProcessError as error:
            raise self._failure(command, error) from error

    def run_captured(self, command: Command, *, environment: Mapping[str, str]) -> str:
        self._register_sensitive_environment(command, environment)
        rendered = self.redactor.redact(safe_command(command))
        print(f"+ {rendered}", flush=True)
        try:
            completed = subprocess.run(
                list(command.argv),
                check=True,
                cwd=command.cwd,
                env=self._environment(environment),
                capture_output=True,
                text=True,
            )
        except FileNotFoundError as error:
            raise CommandError(
                f"required executable is missing: {self.redactor.redact(command.argv[0])}"
            ) from error
        except subprocess.CalledProcessError as error:
            raise self._failure(command, error) from error
        return self.redactor.redact(completed.stdout)


_DEFAULT_RUNNER = CommandRunner()


def run_streamed(command: Command, *, environment: Mapping[str, str]) -> None:
    _DEFAULT_RUNNER.run_streamed(command, environment=environment)


def run_captured(command: Command, *, environment: Mapping[str, str]) -> str:
    return _DEFAULT_RUNNER.run_captured(command, environment=environment)
