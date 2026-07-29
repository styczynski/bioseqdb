#!/usr/bin/env python3
"""Compatibility entry point for the native BioSeq packaging worker."""

from __future__ import annotations

from bioseq_packaging.cli import argument_parser, main


__all__ = ("argument_parser", "main")


if __name__ == "__main__":
    raise SystemExit(main())
