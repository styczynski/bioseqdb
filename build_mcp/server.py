#!/usr/bin/env python3
"""Composition entrypoint for the bioseq build MCP."""

from __future__ import annotations

import argparse
import json
import os
import sys
import threading

import bootstrap
from build_presets import VALID_MODES
from build_worker import (
    _run_build_inline,
    _run_build_worker,
    _run_papers_build_worker,
    tool_inspect,
)
from process_state import _log
from protocol import _serve, parse_bool
from remote import _run_remote_worker

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--run-build",
        choices=VALID_MODES,
        default=None,
        help="(internal) run as a detached build worker for the given mode",
    )
    parser.add_argument(
        "--run-build-sync",
        choices=VALID_MODES,
        default=None,
        help=(
            "(internal) reserve the state slot and run the build worker INLINE "
            "(synchronous, not detached) for the given mode. Used by the "
            "remote-run tool over SSH so the build verdict is the SSH exit "
            "status and output streams back live. Honours --build-only/--debug "
            "and the core/test/cache BIOSEQ_*_BUILD_PRESET variables."
        ),
    )
    parser.add_argument(
        "--run-inspect",
        default=None,
        metavar="JSON",
        help=(
            "(internal) run the inspect tool with the given JSON argument object "
            "and print the JSON result to stdout. Used by remote-run over SSH to "
            "inspect a binary on the box. Honours the core/test/cache "
            "BIOSEQ_*_BUILD_PRESET variables."
        ),
    )
    parser.add_argument(
        "--run-papers-build",
        action="store_true",
        default=False,
        help=(
            "(internal) run the detached CMake worker for the "
            "main/papers bioseqdb_thesis target"
        ),
    )
    parser.add_argument(
        "--run-remote",
        default=None,
        metavar="OP_ID",
        help=(
            "(internal) run as a detached remote-operation worker for the given "
            "op_id; reads the command/payload from the remote state file and drives "
            "provisioner.py or a remote build/inspect over SSH"
        ),
    )
    parser.add_argument(
        "--build-only",
        choices=['true', 'false'],
        default='false',
        help="(internal) if 'true' then build will only compile target and not run any actual executables like benchmarks or tests",
    )
    parser.add_argument(
        "--debug",
        choices=['true', 'false'],
        default='false',
        help=(
            "(internal) if 'true' then use the selected mode's debug preset "
            "(test family for unit/integration, production family for "
            "benchmarks), adding -g + -save-temps=obj. Required for the "
            "inspect tool. Default 'false' uses that mode's lean preset."
        ),
    )
    parser.add_argument(
        "--fresh",
        choices=['true', 'false'],
        default='false',
        help="(internal) configure an isolated MCP-owned build tree",
    )
    parser.add_argument(
        "--run-only",
        choices=['true', 'false'],
        default='false',
        help="(internal) skip every configure/build phase and reuse a receipt",
    )
    parser.add_argument(
        "--build-timeout-sec",
        default="",
        help="(internal) guarded configure/compile/link hard cutoff in seconds",
    )
    parser.add_argument(
        "--reuse-build-receipt",
        default="",
        help="(internal) opaque successful build receipt id",
    )
    parser.add_argument(
        "--bench-flags",
        default="[]",
        metavar="JSON",
        help=(
            "(internal) JSON array of extra argv flags appended to the bench "
            "binary (e.g. [\"--filter\",\"tags=gpu\",\"--debug\"]). Bench modes only."
        ),
    )
    parser.add_argument(
        "--test-flags",
        default="[]",
        metavar="JSON",
        help=(
            "(internal) JSON array of already-validated Catch2 argv tokens "
            "appended to every executable in a Catch correctness mode (for "
            "example [\"--allow-running-no-tests\",\"[index],[postgres]\"]). "
            "Accepted only for the closed Catch correctness mode set."
        ),
    )
    parser.add_argument(
        "--benchmark-e2e-config",
        default="",
        help=(
            "(internal) canonical repository-contained E2E config path. "
            "Forwarded only to the local benchmark-e2e executable environment."
        ),
    )
    args = parser.parse_args()
    if args.run_papers_build:
        return _run_papers_build_worker()
    if args.run_remote is not None:
        return _run_remote_worker(args.run_remote)
    if args.run_build is not None:
        build_only: bool = parse_bool("build_only", args.build_only)
        debug: bool = parse_bool("debug", args.debug)
        fresh: bool = parse_bool("fresh", args.fresh)
        run_only: bool = parse_bool("run_only", args.run_only)
        try:
            build_timeout_sec = (
                int(args.build_timeout_sec)
                if args.build_timeout_sec else None
            )
        except ValueError:
            _log("--build-timeout-sec must be an integer")
            return 2
        try:
            bench_flags = json.loads(args.bench_flags)
            if not isinstance(bench_flags, list) or not all(isinstance(x, str) for x in bench_flags):
                raise ValueError("must be a JSON array of strings")
            test_flags = json.loads(args.test_flags)
            if not isinstance(test_flags, list) or not all(isinstance(x, str) for x in test_flags):
                raise ValueError("--test-flags must be a JSON array of strings")
        except (ValueError, json.JSONDecodeError) as e:
            _log(f"runtime flags invalid: {e}")
            return 2
        return _run_build_worker(args.run_build, build_only=build_only,
                                 debug=debug, bench_flags=bench_flags,
                                 test_flags=test_flags,
                                 bench_e2e_config=(
                                     args.benchmark_e2e_config or None),
                                 fresh=fresh,
                                 run_only=run_only,
                                 build_timeout_sec=build_timeout_sec,
                                 reuse_build_receipt=(
                                     args.reuse_build_receipt or None))
    if args.run_build_sync is not None:
        build_only: bool = parse_bool("build_only", args.build_only)
        debug: bool = parse_bool("debug", args.debug)
        try:
            bench_flags = json.loads(args.bench_flags)
            if not isinstance(bench_flags, list) or not all(isinstance(x, str) for x in bench_flags):
                raise ValueError("must be a JSON array of strings")
            test_flags = json.loads(args.test_flags)
            if not isinstance(test_flags, list) or not all(isinstance(x, str) for x in test_flags):
                raise ValueError("--test-flags must be a JSON array of strings")
        except (ValueError, json.JSONDecodeError) as e:
            _log(f"runtime flags invalid: {e}")
            return 2
        return _run_build_inline(args.run_build_sync, build_only=build_only,
                                 debug=debug, bench_flags=bench_flags,
                                 test_flags=test_flags,
                                 bench_e2e_config=(
                                     args.benchmark_e2e_config or None))
    if args.run_inspect is not None:
        try:
            inspect_args = json.loads(args.run_inspect)
            if not isinstance(inspect_args, dict):
                raise ValueError("inspect JSON argument must be a JSON object")
        except (ValueError, json.JSONDecodeError) as e:
            print(json.dumps({"error": f"invalid --run-inspect JSON: {e}"}))
            return 2
        print(tool_inspect(inspect_args))
        return 0

    # Non-blocking autostart warm: provision the doc env and bring qmd (collections + HTTP
    # server + background index) online without blocking the MCP handshake or the stdlib build
    # tools. This also makes "keep QMD collections up to date periodically" run even in sessions
    # that never call a doc tool. Disable with BIOSEQ_DOCS_AUTOSTART=0.
    if os.environ.get("BIOSEQ_DOCS_AUTOSTART", "1") == "1":
        from docs.qmd_manager import get_manager

        qmd_manager = get_manager()
        qmd_manager.begin_external_update()

        def _warm() -> None:
            try:
                bootstrap.ensure_doc_env()
                qmd_manager.ensure_ready()
            except Exception as e:
                print(f"[bioseq-build-mcp] autostart warm skipped: {e}",
                      file=sys.stderr, flush=True)
            finally:
                qmd_manager.end_external_update()

        try:
            threading.Thread(target=_warm, daemon=True).start()
        except Exception:
            qmd_manager.end_external_update()
            raise

    try:
        _serve()
    finally:
        try:
            from docs.qmd_manager import get_manager

            get_manager().unregister_idle_process()
        except Exception as exc:
            print(
                f"[bioseq-build-mcp] QMD activity unregister skipped: {exc}",
                file=sys.stderr,
                flush=True,
            )
    return 0


if __name__ == "__main__":
    sys.exit(main())
