"""Bounded sanitizer-runtime environment handling for PostgreSQL loaders.

Sanitized Bioseq extension modules are loaded by an otherwise unsanitized
PostgreSQL executable.  On macOS ASAN and TSAN therefore need their runtime
inserted before the postmaster starts.  Keep the caller-facing ABI neutral and
translate it to dyld's preload variable only at the two PostgreSQL launch
boundaries that require it.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Mapping, Sequence


SANITIZER_RUNTIME_ENV = "BIOSEQ_SANITIZER_RUNTIME"
DYLD_INSERT_ENV = "DYLD_INSERT_LIBRARIES"

# These stable Homebrew LLVM 22 paths are the only runtimes supported by the
# local arm64 sanitizer presets.  Validation resolves the stable symlink and
# compares canonical files, so the corresponding Cellar target is accepted
# without broadening the allowlist to arbitrary dylibs.
LLVM_SANITIZER_RUNTIME_PATHS = (
    "/opt/homebrew/opt/llvm@22/lib/clang/22/lib/darwin/"
    "libclang_rt.asan_osx_dynamic.dylib",
    "/opt/homebrew/opt/llvm@22/lib/clang/22/lib/darwin/"
    "libclang_rt.ubsan_osx_dynamic.dylib",
    "/opt/homebrew/opt/llvm@22/lib/clang/22/lib/darwin/"
    "libclang_rt.tsan_osx_dynamic.dylib",
)


class SanitizerRuntimeError(ValueError):
    """The requested sanitizer runtime is outside the bounded allowlist."""


def validate_sanitizer_runtime(
    value: str,
    *,
    allowed_paths: Sequence[str | Path] = LLVM_SANITIZER_RUNTIME_PATHS,
) -> str:
    """Return one canonical, existing, allowlisted LLVM runtime path."""
    if not isinstance(value, str) or not value:
        raise SanitizerRuntimeError(
            f"{SANITIZER_RUNTIME_ENV} must name one sanitizer runtime")
    if os.pathsep in value:
        raise SanitizerRuntimeError(
            f"{SANITIZER_RUNTIME_ENV} accepts exactly one runtime path")

    requested = Path(value)
    if not requested.is_absolute():
        raise SanitizerRuntimeError(
            f"{SANITIZER_RUNTIME_ENV} must be an absolute path")
    try:
        resolved_requested = requested.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        raise SanitizerRuntimeError(
            f"{SANITIZER_RUNTIME_ENV} does not resolve to an existing file: "
            f"{value}") from exc
    if not resolved_requested.is_file():
        raise SanitizerRuntimeError(
            f"{SANITIZER_RUNTIME_ENV} is not a file: {resolved_requested}")

    resolved_allowed: set[Path] = set()
    for allowed in allowed_paths:
        try:
            allowed_path = Path(allowed).resolve(strict=True)
        except (OSError, RuntimeError):
            continue
        if allowed_path.is_file():
            resolved_allowed.add(allowed_path)
    if resolved_requested not in resolved_allowed:
        choices = ", ".join(str(path) for path in allowed_paths)
        raise SanitizerRuntimeError(
            f"{SANITIZER_RUNTIME_ENV} is not an allowed LLVM sanitizer "
            f"runtime: {resolved_requested}; allowed paths: {choices}")
    return str(resolved_requested)


def sanitizer_descendant_environment(
    base_env: Mapping[str, str] | None = None,
    *,
    allowed_paths: Sequence[str | Path] = LLVM_SANITIZER_RUNTIME_PATHS,
) -> dict[str, str]:
    """Copy an environment without forwarding raw dyld preload state."""
    env = dict(os.environ if base_env is None else base_env)
    env.pop(DYLD_INSERT_ENV, None)
    requested = env.get(SANITIZER_RUNTIME_ENV)
    if requested is not None:
        env[SANITIZER_RUNTIME_ENV] = validate_sanitizer_runtime(
            requested, allowed_paths=allowed_paths)
    return env


def postgres_sanitizer_environment(
    base_env: Mapping[str, str] | None = None,
    *,
    allowed_paths: Sequence[str | Path] = LLVM_SANITIZER_RUNTIME_PATHS,
) -> dict[str, str]:
    """Map the neutral runtime selector at a PostgreSQL launch boundary."""
    env = sanitizer_descendant_environment(
        base_env, allowed_paths=allowed_paths)
    runtime = env.get(SANITIZER_RUNTIME_ENV)
    if runtime is not None:
        env[DYLD_INSERT_ENV] = runtime
    return env
