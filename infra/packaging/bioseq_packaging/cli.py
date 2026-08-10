"""Command-line entry point for the native BioSeq package worker."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys
from typing import Sequence

from .pipeline import PackagePipelineError, run_pipeline


DEFAULT_BUILD_ROOT = Path("/var/lib/bioseq-package-build")
DEFAULT_S3_PREFIX = "bioseq/packages"
DEFAULT_SOURCE_DIR = Path(__file__).resolve().parents[3]


def argument_parser() -> argparse.ArgumentParser:
    """Return the compatibility-preserving worker argument parser."""

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
        help="BioSeq distro ID; repeat or pass a comma-separated list",
    )
    parser.add_argument("--signing-secret-arn")
    parser.add_argument(
        "--source-dir",
        type=Path,
        default=DEFAULT_SOURCE_DIR,
    )
    parser.add_argument("--build-root", type=Path, default=DEFAULT_BUILD_ROOT)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--s3-prefix", default=DEFAULT_S3_PREFIX)
    parser.add_argument("--aws-bin", default="aws")
    parser.add_argument(
        "--container-output",
        choices=("null", "stdout", "stderr"),
        default="stdout",
    )
    parser.add_argument("--fail-fast", action="store_true")
    parser.add_argument(
        "--source-revision",
        required=True,
        help="full committed Git revision transferred to this worker",
    )
    parser.add_argument(
        "--poetry-lock-sha256",
        required=True,
        help="lower-case SHA-256 of the committed packaging poetry.lock",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    """Run the requested native matrix and return a process exit status."""

    arguments = argument_parser().parse_args(argv)
    manifest_path = arguments.manifest or arguments.build_root / "manifest.json"
    try:
        manifest = run_pipeline(arguments, manifest_path=manifest_path)
    except PackagePipelineError as error:
        print(f"package matrix failed; see {manifest_path}: {error}", file=sys.stderr)
        return 1

    if manifest.get("status") != "success":
        print(f"package matrix failed; see {manifest_path}", file=sys.stderr)
        return 1
    print(f"package matrix uploaded successfully; manifest: {manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
