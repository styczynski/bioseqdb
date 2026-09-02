#!/usr/bin/env python3
"""Run a dense repeat-disruption sweep with bounded aggregate publication.

The underlying experiment runner remains the single implementation of case
execution, validation, atomic raw commits, immutable result shards, resume
handling, and command-line parsing.  This wrapper changes two process-wide
hooks before entering that runner:

* result provenance names this wrapper, the core runner, and the common helper
  with their exact SHA-256 digests;
* the human-readable YAML aggregate is rebuilt after each 25 newly available
  selected shards, when the selected set becomes complete, and once when the
  runner exits.  Per-case raw commits and shards retain their original atomic
  publication cadence.
"""

from __future__ import annotations

from pathlib import Path
import sys
from typing import Any, Callable

import yaml

import runl_repeat_run_experiment as core


AGGREGATE_SHARD_INTERVAL = 25
WRAPPER_PATH = Path(__file__).resolve()
CORE_RUNNER_PATH = Path(core.__file__).resolve()
COMMON_HELPER_PATH = (core.ROOT / "runl_repeat_common.py").resolve()


def dense_analysis_source_digests() -> dict[str, str]:
    """Bind each result to all Python sources that control its analysis."""

    return {
        WRAPPER_PATH.name: core.sha256_file(WRAPPER_PATH),
        CORE_RUNNER_PATH.name: core.sha256_file(CORE_RUNNER_PATH),
        COMMON_HELPER_PATH.name: core.sha256_file(COMMON_HELPER_PATH),
    }


class DenseAggregatePublisher:
    """Throttle aggregate rewrites without changing per-case persistence."""

    def __init__(
        self,
        publish: Callable[..., None],
        interval: int = AGGREGATE_SHARD_INTERVAL,
    ) -> None:
        if interval < 1:
            raise ValueError("aggregate publication interval must be positive")
        self._publish = publish
        self._interval = interval
        self._latest_arguments: tuple[Any, ...] | None = None
        self._last_published_selected_count: int | None = None
        self._last_written_selected_count: int | None = None

    @staticmethod
    def _available_selected_count(
        selected_case_ids: set[str], shard_dir: Path
    ) -> int:
        return sum(
            (shard_dir / f"{case_id}.json").is_file()
            for case_id in selected_case_ids
        )

    @staticmethod
    def _aggregate_selected_count(
        path: Path,
        manifest_digest: str,
        binary_digest: str,
        selected_case_ids: set[str],
    ) -> int | None:
        """Read a compatible prior checkpoint, or reject it as a cadence base."""

        if not path.is_file():
            return None
        try:
            with path.open("r", encoding="utf-8") as source:
                document = yaml.safe_load(source)
            if not isinstance(document, dict):
                return None
            manifest = document.get("manifest")
            minimap2 = document.get("minimap2")
            if not isinstance(manifest, dict) or not isinstance(minimap2, dict):
                return None
            if manifest.get("sha256") != manifest_digest:
                return None
            if minimap2.get("sha256") != binary_digest:
                return None
            runs = document.get("runs")
            if not isinstance(runs, list):
                return None
            published_ids = {
                run.get("case_id")
                for run in runs
                if isinstance(run, dict) and isinstance(run.get("case_id"), str)
            }
            return len(published_ids.intersection(selected_case_ids))
        except (OSError, UnicodeError, yaml.YAMLError):
            return None

    def __call__(
        self,
        path: Path,
        manifest_path: Path,
        manifest_digest: str,
        binary: Path,
        binary_digest: str,
        resource_policy: dict[str, Any],
        cases: list[core.CaseSpec],
        selected_case_ids: set[str],
        shard_dir: Path,
    ) -> None:
        arguments = (
            path,
            manifest_path,
            manifest_digest,
            binary,
            binary_digest,
            resource_policy,
            cases,
            selected_case_ids,
            shard_dir,
        )
        self._latest_arguments = arguments
        available = self._available_selected_count(selected_case_ids, shard_dir)

        if self._last_published_selected_count is None:
            checkpoint_count = self._aggregate_selected_count(
                path,
                manifest_digest,
                binary_digest,
                selected_case_ids,
            )
            self._last_published_selected_count = min(
                available,
                checkpoint_count if checkpoint_count is not None else 0,
            )

        newly_available = available - self._last_published_selected_count
        selected_complete = available == len(selected_case_ids)
        if newly_available >= self._interval or (
            selected_complete and newly_available > 0
        ):
            self._publish(*arguments)
            self._last_published_selected_count = available
            self._last_written_selected_count = available

    def force_final(self) -> None:
        """Write the last aggregate checkpoint, including a partial exit."""

        if self._latest_arguments is None:
            return
        path = self._latest_arguments[0]
        selected_case_ids = self._latest_arguments[7]
        shard_dir = self._latest_arguments[8]
        available = self._available_selected_count(selected_case_ids, shard_dir)
        already_current = (
            self._last_written_selected_count == available and path.is_file()
        )
        if already_current:
            return
        self._publish(*self._latest_arguments)
        self._last_published_selected_count = available
        self._last_written_selected_count = available


def main() -> int:
    """Install dense-sweep hooks and delegate the complete CLI to the core."""

    original_analysis_source_digests = core.analysis_source_digests
    original_publish_results = core.publish_results
    aggregate_publisher = DenseAggregatePublisher(original_publish_results)
    core.analysis_source_digests = dense_analysis_source_digests
    core.publish_results = aggregate_publisher
    try:
        return core.main()
    finally:
        try:
            aggregate_publisher.force_final()
        finally:
            core.analysis_source_digests = original_analysis_source_digests
            core.publish_results = original_publish_results


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(
            f"{WRAPPER_PATH.name}: {type(error).__name__}: {error}",
            file=sys.stderr,
        )
        raise SystemExit(1)
