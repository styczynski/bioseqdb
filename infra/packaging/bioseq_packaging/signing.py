"""Fail-closed package-signing key handling and agent-free verification.

Secret key material deliberately does not participate in the package model.  It
is fetched once per worker, retained in a redacted/non-serializable object, and
borrowed only for the short-lived repository container environment.  Public-key
verification uses an isolated keyring and :program:`gpgv`; it never imports a
key into a login keyring or relies on ``gpg-agent``.
"""

from __future__ import annotations

import base64
from collections.abc import Callable, Iterator, Mapping, Sequence
from contextlib import contextmanager
from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import shutil
import tempfile
from types import TracebackType
from typing import Final, NoReturn

from .commands import Command, CommandRunner
from .model import PackageBuildError


PINNED_SIGNING_FINGERPRINT: Final = (
    "452F34C5F1EF6D471EC041ADDD94780B0728F093"
)
SIGNING_ENVIRONMENT_NAME: Final = "GPG_PRIVATE_KEY"

_PRIVATE_KEY_BEGIN: Final = "-----BEGIN PGP PRIVATE KEY BLOCK-----"
_PRIVATE_KEY_END: Final = "-----END PGP PRIVATE KEY BLOCK-----"
_FINGERPRINT_RE: Final = re.compile(r"[0-9A-Fa-f]{40}(?:[0-9A-Fa-f]{24})?")


class SigningError(PackageBuildError):
    """Raised when signing material or signature evidence is invalid."""


RedactionRegistrar = Callable[[str], None]


def _normalize_fingerprint(value: str, *, label: str = "fingerprint") -> str:
    normalized = value.strip().upper()
    if not _FINGERPRINT_RE.fullmatch(normalized):
        raise SigningError(f"{label} is not a full OpenPGP fingerprint")
    return normalized


def require_pinned_fingerprint(value: str) -> str:
    """Return a normalized fingerprint only when it is the checked-in key."""

    normalized = _normalize_fingerprint(value)
    if normalized != PINNED_SIGNING_FINGERPRINT:
        raise SigningError(
            "package-signing fingerprint does not match the pinned BioSeq key"
        )
    return normalized


def _register_secret(registrar: RedactionRegistrar | None, value: str) -> None:
    # Very short values make poor redaction tokens and cannot be valid armored or
    # base64-encoded private keys.  Do not let a broken registrar turn a secret
    # into an exception message.
    if registrar is None or len(value) < 16:
        return
    try:
        registrar(value)
    except Exception:
        # A third-party registrar is allowed to fail with an exception whose
        # message repeats the value it was handed.  Suppress that context so a
        # traceback cannot undo the boundary this helper exists to enforce.
        raise SigningError("failed to register package-signing key redaction") from None


class SecretBytes:
    """Normalized private-key bytes with no printable/serializable form.

    The internal value is canonical base64-encoded ASCII armor because that is
    the established repository-container contract.  Callers cannot obtain it
    through ``repr``, ``str``, ``bytes``, JSON, copying, or pickling.  The sole
    intentional exposure is the private repository-environment scope below.
    """

    __slots__ = ("_normalized", "_destroyed")

    def __init__(self, normalized_base64: bytes) -> None:
        if not normalized_base64:
            raise SigningError("GPG private key is empty")
        self._normalized = bytearray(normalized_base64)
        self._destroyed = False

    def __repr__(self) -> str:
        return "SecretBytes(<redacted>)"

    def __str__(self) -> str:
        return "<redacted>"

    def __reduce_ex__(self, _protocol: int) -> NoReturn:
        raise TypeError("SecretBytes cannot be serialized")

    def __getstate__(self) -> NoReturn:
        raise TypeError("SecretBytes cannot be serialized")

    def __copy__(self) -> NoReturn:
        raise TypeError("SecretBytes cannot be copied")

    def __deepcopy__(self, _memo: object) -> NoReturn:
        raise TypeError("SecretBytes cannot be copied")

    def __enter__(self) -> SecretBytes:
        self._require_live()
        return self

    def __exit__(
        self,
        _exc_type: type[BaseException] | None,
        _exc: BaseException | None,
        _traceback: TracebackType | None,
    ) -> None:
        self.destroy()

    def __del__(self) -> None:
        # Best effort only; deterministic owners call ``destroy``/``close``.
        try:
            self.destroy()
        except Exception:
            pass

    @property
    def destroyed(self) -> bool:
        return self._destroyed

    def _require_live(self) -> None:
        if self._destroyed:
            raise SigningError("package-signing key material has been destroyed")

    def _environment_value(self) -> str:
        self._require_live()
        return self._normalized.decode("ascii")

    def _armored_bytes(self) -> bytes:
        self._require_live()
        try:
            return base64.b64decode(bytes(self._normalized), validate=True)
        except ValueError as error:  # Defensive: construction already validates.
            raise SigningError("normalized package-signing key is corrupt") from error

    def _redact(self, message: str) -> str:
        if self._destroyed:
            return message
        values = (self._environment_value(), self._armored_bytes().decode("utf-8"))
        for value in sorted(values, key=len, reverse=True):
            if value:
                message = message.replace(value, "<redacted>")
                message = message.replace(value.strip(), "<redacted>")
        return message

    def register_redactions(self, registrar: RedactionRegistrar) -> None:
        """Register normalized base64 and armor with the command redactor."""

        _register_secret(registrar, self._environment_value())
        armor = self._armored_bytes().decode("utf-8")
        _register_secret(registrar, armor)
        _register_secret(registrar, armor.strip())

    def destroy(self) -> None:
        if self._destroyed:
            return
        for index in range(len(self._normalized)):
            self._normalized[index] = 0
        self._normalized.clear()
        self._destroyed = True

    close = destroy


def _validated_armor(value: str) -> str:
    normalized = value.strip()
    if not (
        normalized.startswith(_PRIVATE_KEY_BEGIN)
        and normalized.endswith(_PRIVATE_KEY_END)
    ):
        raise SigningError("decoded GPG_PRIVATE_KEY is not an armored private key")
    # Canonical transport has exactly one final newline.
    return normalized.rstrip() + "\n"


def normalize_signing_key(
    secret: str | bytes,
    *,
    register_redaction: RedactionRegistrar | None = None,
) -> SecretBytes:
    """Normalize Secrets Manager JSON, ASCII armor, or base64 armor.

    Parsing failures are deliberately generic: JSON/base64 decoder details can
    echo fragments of their input.  Every representation available during
    normalization is registered before it falls out of scope.
    """

    try:
        raw = secret.decode("utf-8") if isinstance(secret, bytes) else secret
    except UnicodeDecodeError:
        # UnicodeDecodeError renders a slice of the rejected bytes.  Never
        # retain it as an exception cause for secret material.
        raise SigningError("package-signing secret is not UTF-8") from None
    if not isinstance(raw, str):
        raise SigningError("package-signing secret must be text or bytes")

    value = raw.strip()
    _register_secret(register_redaction, raw)
    _register_secret(register_redaction, value)
    if value.startswith("{"):
        try:
            decoded_json = json.loads(value)
        except json.JSONDecodeError:
            raise SigningError("signing SecretString is invalid JSON") from None
        if not isinstance(decoded_json, dict):
            raise SigningError("signing SecretString JSON must be an object")
        candidate = decoded_json.get(SIGNING_ENVIRONMENT_NAME)
        if candidate is None:
            candidate = decoded_json.get("gpg_private_key_base64")
        if not isinstance(candidate, str):
            raise SigningError(
                "signing SecretString JSON lacks a string GPG private key"
            )
        value = candidate.strip()
        _register_secret(register_redaction, candidate)
        _register_secret(register_redaction, value)

    if not value:
        raise SigningError("GPG private key is empty")

    if value.startswith(_PRIVATE_KEY_BEGIN):
        armor = _validated_armor(value)
        normalized_base64 = base64.b64encode(armor.encode("utf-8"))
    else:
        compact = "".join(value.split())
        _register_secret(register_redaction, compact)
        try:
            decoded = base64.b64decode(compact, validate=True).decode("utf-8")
        except (ValueError, UnicodeDecodeError):
            raise SigningError(
                "GPG_PRIVATE_KEY must be ASCII armor or its base64 encoding"
            ) from None
        armor = _validated_armor(decoded)
        # Canonicalize base64 as well as whitespace.  This makes redaction and
        # repository-container transport deterministic.
        normalized_base64 = base64.b64encode(armor.encode("utf-8"))

    normalized_text = normalized_base64.decode("ascii")
    _register_secret(register_redaction, armor)
    _register_secret(register_redaction, armor.strip())
    _register_secret(register_redaction, normalized_text)
    result = SecretBytes(normalized_base64)
    if register_redaction is not None:
        result.register_redactions(register_redaction)
    return result


def running_on_aws() -> bool:
    """Detect EC2 without a network/metadata request."""

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


def _sensitive_process(
    argv: Sequence[str],
    *,
    environment: Mapping[str, str],
    operation: str,
    cwd: Path | None = None,
    runner: CommandRunner | None = None,
) -> bytes:
    """Run a sensitive command without ever attaching its output to errors."""

    try:
        command_runner = runner or CommandRunner()
        output = command_runner.run_captured(
            Command(
                argv=tuple(argv),
                cwd=cwd,
                suppress_output_on_failure=True,
            ),
            environment=environment,
        )
    except PackageBuildError as error:
        raise SigningError(operation) from error
    return output.encode("utf-8")


class SigningKeyProvider:
    """Fetch and normalize the worker's private package-signing key."""

    __slots__ = (
        "aws_bin",
        "environment",
        "_is_aws",
        "_local_secret",
        "_register_redaction",
        "runner",
    )

    def __init__(
        self,
        *,
        aws_bin: str = "aws",
        environment: Mapping[str, str] | None = None,
        is_aws: bool | None = None,
        register_redaction: RedactionRegistrar | None = None,
        runner: CommandRunner | None = None,
    ) -> None:
        self.aws_bin = aws_bin
        source_environment = dict(os.environ if environment is None else environment)
        self._local_secret = source_environment.pop(SIGNING_ENVIRONMENT_NAME, None)
        self.environment = source_environment
        self._is_aws = is_aws
        self.runner = runner or CommandRunner()
        self._register_redaction = (
            register_redaction or self.runner.redactor.register
        )

    def __repr__(self) -> str:
        return "SigningKeyProvider(private_key=<redacted>)"

    def __reduce_ex__(self, _protocol: int) -> NoReturn:
        raise TypeError("SigningKeyProvider cannot be serialized")

    def __getstate__(self) -> NoReturn:
        raise TypeError("SigningKeyProvider cannot be serialized")

    def __copy__(self) -> NoReturn:
        raise TypeError("SigningKeyProvider cannot be copied")

    def __deepcopy__(self, _memo: object) -> NoReturn:
        raise TypeError("SigningKeyProvider cannot be copied")

    def fetch(
        self,
        *,
        secret_arn: str | None,
        region: str,
        aws_bin: str | None = None,
        runner: CommandRunner | None = None,
    ) -> SecretBytes:
        on_aws = running_on_aws() if self._is_aws is None else self._is_aws
        if on_aws and not secret_arn:
            self._local_secret = None
            raise SigningError(
                "--signing-secret-arn is required on AWS package builders"
            )

        if secret_arn:
            executable = aws_bin or self.aws_bin
            environment = dict(self.environment)
            environment.pop(SIGNING_ENVIRONMENT_NAME, None)
            environment["AWS_PAGER"] = ""
            output = _sensitive_process(
                (
                    executable,
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
                    "--no-cli-pager",
                ),
                environment=environment,
                operation="failed to fetch signing key from Secrets Manager",
                runner=runner or self.runner,
            )
            try:
                return normalize_signing_key(
                    output, register_redaction=self._register_redaction
                )
            finally:
                self._local_secret = None

        local_secret = self._local_secret or ""
        try:
            return normalize_signing_key(
                local_secret,
                register_redaction=self._register_redaction,
            )
        finally:
            self._local_secret = None


@dataclass(frozen=True, slots=True)
class SigningWorkspace:
    """Paths inside one isolated, ephemeral GnuPG workspace."""

    home: Path
    private_key_path: Path | None


def _write_private_key(path: Path, key: SecretBytes) -> None:
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(path, flags, 0o600)
    try:
        with os.fdopen(descriptor, "wb", closefd=True) as handle:
            descriptor = -1
            handle.write(key._armored_bytes())
            handle.flush()
            os.fsync(handle.fileno())
    finally:
        if descriptor >= 0:
            os.close(descriptor)
    os.chmod(path, 0o600)


def _raise_workspace_outcome(
    *,
    key: SecretBytes | None,
    work_error: BaseException | None,
    traceback: TracebackType | None,
    cleanup_error: BaseException | None,
) -> None:
    redact = key._redact if key is not None else (lambda value: value)
    if work_error is not None and cleanup_error is not None:
        message = redact(str(work_error)) or type(work_error).__name__
        cleanup = redact(str(cleanup_error)) or type(cleanup_error).__name__
        raise SigningError(
            f"signing work failed ({message}) and isolated cleanup failed ({cleanup})"
        ) from None
    if work_error is not None:
        redacted = redact(str(work_error))
        if redacted != str(work_error):
            raise SigningError(redacted or "signing work failed") from None
        raise work_error.with_traceback(traceback)
    if cleanup_error is not None:
        cleanup = redact(str(cleanup_error)) or type(cleanup_error).__name__
        raise SigningError(f"isolated signing cleanup failed ({cleanup})") from None


@contextmanager
def isolated_signing_workspace(
    key: SecretBytes | None = None,
) -> Iterator[SigningWorkspace]:
    """Create a mode-0700 home and optional mode-0600 private-key file.

    Cleanup is attempted for success, ordinary failure, and interruption.  If
    work and cleanup both fail, neither outcome is discarded and both are
    redacted before being reported.
    """

    home = Path(tempfile.mkdtemp(prefix="bioseq-signing-"))
    private_key_path = home / "key.priv" if key is not None else None
    work_error: BaseException | None = None
    work_traceback: TracebackType | None = None
    cleanup_error: BaseException | None = None
    try:
        os.chmod(home, 0o700)
        if private_key_path is not None:
            _write_private_key(private_key_path, key)
        try:
            yield SigningWorkspace(home=home, private_key_path=private_key_path)
        except BaseException as error:
            work_error = error
            work_traceback = error.__traceback__
    except BaseException as error:
        work_error = error
        work_traceback = error.__traceback__
    finally:
        try:
            shutil.rmtree(home)
        except BaseException as error:
            cleanup_error = error

    _raise_workspace_outcome(
        key=key,
        work_error=work_error,
        traceback=work_traceback,
        cleanup_error=cleanup_error,
    )


def _isolated_environment(
    base: Mapping[str, str] | None, home: Path
) -> dict[str, str]:
    environment = dict(os.environ if base is None else base)
    environment.pop(SIGNING_ENVIRONMENT_NAME, None)
    environment.pop("GPG_AGENT_INFO", None)
    environment["GNUPGHOME"] = str(home)
    environment["LC_ALL"] = "C"
    return environment


def _gpg_executable(name: str, environment: Mapping[str, str]) -> str:
    executable = shutil.which(name, path=environment.get("PATH"))
    if executable is None:
        raise SigningError(f"{name} is required for package-signing verification")
    return executable


def _primary_fingerprints(colon_output: str) -> tuple[str, ...]:
    result: list[str] = []
    awaiting_primary = False
    for line in colon_output.splitlines():
        fields = line.split(":")
        record = fields[0] if fields else ""
        if record in {"pub", "sec"}:
            awaiting_primary = True
            continue
        if record in {"sub", "ssb"}:
            awaiting_primary = False
            continue
        if record == "fpr" and awaiting_primary and len(fields) > 9:
            result.append(
                _normalize_fingerprint(fields[9], label="GPG key fingerprint")
            )
            awaiting_primary = False
    if not result:
        raise SigningError("GPG key metadata did not contain a primary fingerprint")
    return tuple(result)


def parse_gpg_fingerprint(output: str) -> str:
    """Parse exactly one primary fingerprint from GnuPG colon output."""

    fingerprints = _primary_fingerprints(output)
    if len(fingerprints) != 1:
        raise SigningError("GPG key material must contain exactly one primary key")
    return fingerprints[0]


def fingerprint(
    key: SecretBytes,
    *,
    environment: Mapping[str, str] | None = None,
    gpg_bin: str | None = None,
    runner: CommandRunner | None = None,
) -> str:
    """Derive and enforce the pinned fingerprint without importing the key."""

    with isolated_signing_workspace(key) as workspace:
        assert workspace.private_key_path is not None
        command_environment = _isolated_environment(environment, workspace.home)
        gpg = gpg_bin or _gpg_executable("gpg", command_environment)
        output = _sensitive_process(
            (
                gpg,
                "--batch",
                "--no-tty",
                "--no-options",
                "--no-autostart",
                "--homedir",
                str(workspace.home),
                "--with-colons",
                "--show-keys",
                str(workspace.private_key_path),
            ),
            environment=command_environment,
            operation="failed to identify package-signing key fingerprint",
            runner=runner,
        )
        try:
            actual = parse_gpg_fingerprint(output.decode("utf-8"))
        except UnicodeDecodeError as error:
            raise SigningError("GPG key metadata was not UTF-8") from error
        return require_pinned_fingerprint(actual)


class SigningMaterial:
    """Pinned fingerprint plus privately retained, nonserializable key bytes."""

    __slots__ = ("_fingerprint", "_key", "_closed")

    def __init__(self, *, key: SecretBytes, fingerprint: str) -> None:
        self._fingerprint = require_pinned_fingerprint(fingerprint)
        self._key = key
        self._closed = False

    def __repr__(self) -> str:
        return (
            "SigningMaterial(fingerprint="
            f"{self.fingerprint!r}, private_key=<redacted>)"
        )

    @property
    def fingerprint(self) -> str:
        return self._fingerprint

    def __reduce_ex__(self, _protocol: int) -> NoReturn:
        raise TypeError("SigningMaterial cannot be serialized")

    def __getstate__(self) -> NoReturn:
        raise TypeError("SigningMaterial cannot be serialized")

    def __copy__(self) -> NoReturn:
        raise TypeError("SigningMaterial cannot be copied")

    def __deepcopy__(self, _memo: object) -> NoReturn:
        raise TypeError("SigningMaterial cannot be copied")

    def __enter__(self) -> SigningMaterial:
        self._borrow_key()
        return self

    def __exit__(
        self,
        _exc_type: type[BaseException] | None,
        _exc: BaseException | None,
        _traceback: TracebackType | None,
    ) -> None:
        self.close()

    @property
    def closed(self) -> bool:
        return self._closed

    def _borrow_key(self) -> SecretBytes:
        if self._closed:
            raise SigningError("package-signing material has been closed")
        self._key._require_live()
        return self._key

    def close(self) -> None:
        if self._closed:
            return
        self._key.destroy()
        self._closed = True


class SigningProvider:
    """High-level provider that fetches once and validates the pinned key."""

    __slots__ = ("key_provider", "environment", "gpg_bin", "runner")

    def __init__(
        self,
        *,
        key_provider: SigningKeyProvider | None = None,
        aws_bin: str = "aws",
        environment: Mapping[str, str] | None = None,
        is_aws: bool | None = None,
        register_redaction: RedactionRegistrar | None = None,
        gpg_bin: str | None = None,
        runner: CommandRunner | None = None,
    ) -> None:
        source_environment = dict(os.environ if environment is None else environment)
        self.environment = dict(source_environment)
        self.environment.pop(SIGNING_ENVIRONMENT_NAME, None)
        self.runner = runner or CommandRunner()
        registrar = register_redaction or self.runner.redactor.register
        self.key_provider = key_provider or SigningKeyProvider(
            aws_bin=aws_bin,
            environment=source_environment,
            is_aws=is_aws,
            register_redaction=registrar,
            runner=self.runner,
        )
        self.gpg_bin = gpg_bin

    def __repr__(self) -> str:
        return "SigningProvider(private_key=<redacted>)"

    def __reduce_ex__(self, _protocol: int) -> NoReturn:
        raise TypeError("SigningProvider cannot be serialized")

    def __getstate__(self) -> NoReturn:
        raise TypeError("SigningProvider cannot be serialized")

    def __copy__(self) -> NoReturn:
        raise TypeError("SigningProvider cannot be copied")

    def __deepcopy__(self, _memo: object) -> NoReturn:
        raise TypeError("SigningProvider cannot be copied")

    def fetch(
        self,
        *,
        secret_arn: str | None,
        region: str,
        aws_bin: str | None = None,
        runner: CommandRunner | None = None,
    ) -> SigningMaterial:
        command_runner = runner or self.runner
        key = self.key_provider.fetch(
            secret_arn=secret_arn,
            region=region,
            aws_bin=aws_bin,
            runner=command_runner,
        )
        try:
            actual = fingerprint(
                key,
                environment=self.environment,
                gpg_bin=self.gpg_bin,
                runner=command_runner,
            )
            return SigningMaterial(key=key, fingerprint=actual)
        except BaseException:
            key.destroy()
            raise


@contextmanager
def repository_environment(
    material: SigningMaterial | SecretBytes,
) -> Iterator[dict[str, str]]:
    """Expose the key only as the repository container's named environment.

    The yielded mapping contains exactly one entry.  It is cleared on every
    exit; callers must pass the name to Docker as ``--env GPG_PRIVATE_KEY`` and
    the mapping itself only as the Docker client process environment.
    """

    key = material._borrow_key() if isinstance(material, SigningMaterial) else material
    environment = {SIGNING_ENVIRONMENT_NAME: key._environment_value()}
    work_error: BaseException | None = None
    traceback: TracebackType | None = None
    try:
        yield environment
    except BaseException as error:
        work_error = error
        traceback = error.__traceback__
    finally:
        environment[SIGNING_ENVIRONMENT_NAME] = "<cleared>"
        environment.clear()
    if work_error is not None:
        message = key._redact(str(work_error))
        if message != str(work_error):
            # The original exception may itself contain the key.  Suppressing
            # explicit and implicit chaining keeps formatted tracebacks safe.
            raise SigningError(message or "repository signing failed") from None
        raise work_error.with_traceback(traceback)


def _public_key_fingerprint(
    public_key: Path,
    *,
    home: Path,
    environment: Mapping[str, str],
    gpg: str,
    runner: CommandRunner | None,
) -> str:
    output = _sensitive_process(
        (
            gpg,
            "--batch",
            "--no-tty",
            "--no-options",
            "--no-autostart",
            "--homedir",
            str(home),
            "--with-colons",
            "--show-keys",
            str(public_key),
        ),
        environment=environment,
        operation="failed to inspect published repository public key",
        runner=runner,
    )
    try:
        return parse_gpg_fingerprint(output.decode("utf-8"))
    except UnicodeDecodeError as error:
        raise SigningError("published GPG key metadata was not UTF-8") from error


def _validsig_primary_fingerprints(status_output: str) -> tuple[str, ...]:
    result: list[str] = []
    for line in status_output.splitlines():
        prefix = "[GNUPG:] VALIDSIG "
        if not line.startswith(prefix):
            continue
        fields = line[len(prefix) :].split()
        if not fields:
            continue
        signing_fingerprint = _normalize_fingerprint(
            fields[0], label="VALIDSIG signing fingerprint"
        )
        primary = signing_fingerprint
        # GnuPG appends the primary-key fingerprint after the nine fixed
        # VALIDSIG fields when a signing subkey was used.
        if len(fields) >= 10 and _FINGERPRINT_RE.fullmatch(fields[9]):
            primary = _normalize_fingerprint(
                fields[9], label="VALIDSIG primary fingerprint"
            )
        result.append(primary)
    if not result:
        raise SigningError("gpgv verification emitted no VALIDSIG status")
    return tuple(result)


def parse_validsig(status_output: str) -> str:
    """Require every valid signature status to bind the pinned primary key."""

    fingerprints = _validsig_primary_fingerprints(status_output)
    if any(value != PINNED_SIGNING_FINGERPRINT for value in fingerprints):
        raise SigningError("signature VALIDSIG does not match the pinned BioSeq key")
    return PINNED_SIGNING_FINGERPRINT


def verify_with_gpgv(
    *,
    public_key: Path,
    signatures: Sequence[tuple[Path, Path | None]],
    environment: Mapping[str, str] | None = None,
    gpg_bin: str | None = None,
    gpgv_bin: str | None = None,
    expected_fingerprint: str = PINNED_SIGNING_FINGERPRINT,
    runner: CommandRunner | None = None,
) -> str:
    """Verify detached or clear-signed payloads in an isolated keyring.

    ``expected_fingerprint`` is accepted to make call sites explicit, but it
    must still equal the checked-in literal.  Successful exit status alone is
    insufficient: every invocation must emit a pinned ``VALIDSIG`` record.
    """

    require_pinned_fingerprint(expected_fingerprint)
    key = public_key.resolve()
    if not key.is_file() or key.stat().st_size <= 0:
        raise SigningError(f"published repository public key is missing: {key}")
    if not signatures:
        raise SigningError("at least one repository signature is required")

    resolved: list[tuple[Path, Path | None]] = []
    for signature, payload in signatures:
        signature_path = signature.resolve()
        payload_path = payload.resolve() if payload is not None else None
        if not signature_path.is_file() or signature_path.stat().st_size <= 0:
            raise SigningError(f"signature is missing or empty: {signature_path}")
        if payload_path is not None and (
            not payload_path.is_file() or payload_path.stat().st_size <= 0
        ):
            raise SigningError(f"signed payload is missing or empty: {payload_path}")
        resolved.append((signature_path, payload_path))

    with isolated_signing_workspace() as workspace:
        command_environment = _isolated_environment(environment, workspace.home)
        gpg = gpg_bin or _gpg_executable("gpg", command_environment)
        gpgv = gpgv_bin or _gpg_executable("gpgv", command_environment)
        actual = _public_key_fingerprint(
            key,
            home=workspace.home,
            environment=command_environment,
            gpg=gpg,
            runner=runner,
        )
        require_pinned_fingerprint(actual)

        keyring = workspace.home / "trustedkeys.gpg"
        _sensitive_process(
            (
                gpg,
                "--batch",
                "--no-tty",
                "--no-options",
                "--no-autostart",
                "--homedir",
                str(workspace.home),
                "--yes",
                "--dearmor",
                "--output",
                str(keyring),
                str(key),
            ),
            environment=command_environment,
            operation="failed to create isolated repository verification keyring",
            runner=runner,
        )
        os.chmod(keyring, 0o600)

        for signature, payload in resolved:
            argv = [
                gpgv,
                "--homedir",
                str(workspace.home),
                "--keyring",
                str(keyring),
                "--status-fd",
                "1",
                str(signature),
            ]
            if payload is not None:
                argv.append(str(payload))
            status = _sensitive_process(
                argv,
                environment=command_environment,
                operation="published repository signature verification failed",
                runner=runner,
            )
            try:
                parse_validsig(status.decode("utf-8"))
            except UnicodeDecodeError as error:
                raise SigningError("gpgv status output was not UTF-8") from error

    return PINNED_SIGNING_FINGERPRINT


__all__ = (
    "PINNED_SIGNING_FINGERPRINT",
    "SIGNING_ENVIRONMENT_NAME",
    "SecretBytes",
    "SigningError",
    "SigningKeyProvider",
    "SigningMaterial",
    "SigningProvider",
    "SigningWorkspace",
    "fingerprint",
    "isolated_signing_workspace",
    "normalize_signing_key",
    "parse_gpg_fingerprint",
    "parse_validsig",
    "repository_environment",
    "require_pinned_fingerprint",
    "running_on_aws",
    "verify_with_gpgv",
)
