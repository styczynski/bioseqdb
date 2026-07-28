#!/usr/bin/env python3
"""Initialize the persistent OmniPackage signing key without exposing it.

The OpenTofu stack creates only an empty Secrets Manager secret.  This helper is
run serially on one package builder before either architecture starts.  It
reuses an existing AWSCURRENT value or creates a non-expiring RSA signing key
and writes it directly from a mode-0600 temporary file through the AWS CLI.
Neither the private key nor the secret value is emitted to stdout/stderr.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def fail(message: str) -> "None":
    raise RuntimeError(message)


def run(argv: list[str], *, check: bool = True) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(argv, text=True, capture_output=True, check=False)
    if check and completed.returncode != 0:
        detail = (completed.stderr or completed.stdout).strip()
        fail(f"command {argv[0]!r} failed with exit {completed.returncode}: {detail}")
    return completed


def current_secret(aws: str, region: str, secret_arn: str) -> str | None:
    result = run(
        [
            aws,
            "--region",
            region,
            "--no-cli-pager",
            "secretsmanager",
            "get-secret-value",
            "--secret-id",
            secret_arn,
            "--version-stage",
            "AWSCURRENT",
            "--output",
            "json",
        ],
        check=False,
    )
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        if "ResourceNotFoundException" not in detail:
            fail(
                "refusing to initialize after an unexpected GetSecretValue "
                f"failure: {detail}"
            )
        # Prove the durable secret container exists. An empty secret has no
        # AWSCURRENT version and is the sole state in which initialization is
        # allowed.
        run(
            [
                aws,
                "--region",
                region,
                "--no-cli-pager",
                "secretsmanager",
                "describe-secret",
                "--secret-id",
                secret_arn,
                "--output",
                "json",
            ]
        )
        return None
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        fail(f"Secrets Manager returned malformed JSON: {error}")
    value = payload.get("SecretString")
    if not isinstance(value, str) or "BEGIN PGP PRIVATE KEY BLOCK" not in value:
        fail("AWSCURRENT signing secret is not an armored OpenPGP private key")
    return value


def initialize(region: str, secret_arn: str, identity: str) -> bool:
    aws = shutil.which("aws") or fail("aws executable is not installed")
    gpg = shutil.which("gpg") or fail("gpg executable is not installed")
    if current_secret(aws, region, secret_arn) is not None:
        return False

    previous_umask = os.umask(0o077)
    try:
        with tempfile.TemporaryDirectory(prefix="bioseq-package-signing-") as tmp:
            root = Path(tmp)
            home = root / "gnupg"
            home.mkdir(mode=0o700)
            run(
                [
                    gpg,
                    "--homedir",
                    str(home),
                    "--batch",
                    "--pinentry-mode",
                    "loopback",
                    "--passphrase",
                    "",
                    "--quick-generate-key",
                    identity,
                    "rsa3072",
                    "sign",
                    "0",
                ]
            )
            exported = run(
                [
                    gpg,
                    "--homedir",
                    str(home),
                    "--batch",
                    "--pinentry-mode",
                    "loopback",
                    "--passphrase",
                    "",
                    "--armor",
                    "--export-secret-keys",
                    identity,
                ]
            ).stdout
            if "BEGIN PGP PRIVATE KEY BLOCK" not in exported:
                fail("gpg did not export an armored private key")
            key_file = root / "private-key.asc"
            key_file.write_text(exported, encoding="ascii")
            key_file.chmod(0o600)
            run(
                [
                    aws,
                    "--region",
                    region,
                    "--no-cli-pager",
                    "secretsmanager",
                    "put-secret-value",
                    "--secret-id",
                    secret_arn,
                    "--secret-string",
                    f"file://{key_file}",
                    "--output",
                    "json",
                ]
            )
    finally:
        os.umask(previous_umask)

    if current_secret(aws, region, secret_arn) is None:
        fail("signing key initialization completed without an AWSCURRENT value")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--region", required=True)
    parser.add_argument("--secret-arn", required=True)
    parser.add_argument(
        "--identity",
        default="BioSeq Package Repository <packages@bioseq.invalid>",
    )
    args = parser.parse_args()
    try:
        created = initialize(args.region, args.secret_arn, args.identity)
    except Exception as error:
        print(f"signing-key initialization failed: {error}", file=sys.stderr)
        return 1
    print("signing key initialized" if created else "signing key already initialized")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
