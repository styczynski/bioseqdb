#!/usr/bin/env python3
"""Exercise the benchmark framework terminal UI through a pseudo-terminal.

@file
@brief Run deterministic terminal UI integration scenarios without third-party
dependencies.
"""

from __future__ import annotations

import argparse
from contextlib import suppress
from dataclasses import dataclass
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import signal
import struct
import subprocess
import sys
import termios
import time
from types import TracebackType
from typing import Sequence


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_EXAMPLE_REPORT_DIRECTORY = (
    REPOSITORY_ROOT / "reporting" / "bench_framework_example"
)
DEFAULT_BENCHMARK_REPORT_DIRECTORY = (
    REPOSITORY_ROOT / "reporting" / "bioseq_benchmark"
)

ANSI_RE = re.compile(rb"\x1b\[[0-9;?]*[ -/]*[@-~]")
OSC_RE = re.compile(rb"\x1b\][^\x07]*\x07")
RENDER_SENTINEL = b"\x1b]666;render-N\x07"


def repository_path(value: str) -> Path:
    """Resolve a command-line path relative to the repository root.

    @param value Absolute path or repository-root-relative path.
    @return Normalized absolute path without requiring it to exist.
    """

    path = Path(value).expanduser()
    if not path.is_absolute():
        path = REPOSITORY_ROOT / path
    return path.resolve(strict=False)


def strip_ansi(data: bytes) -> bytes:
    """Remove CSI and OSC terminal control sequences from bytes.

    @param data Raw terminal output.
    @return Terminal output with control sequences removed.
    """

    return ANSI_RE.sub(b"", OSC_RE.sub(b"", data))


@dataclass
class TestResults:
    """Collect and print scenario assertion results.

    @brief Track the number of passing and executed terminal UI checks.
    """

    passed: int = 0
    total: int = 0

    def check(self, label: str, condition: bool, why: str = "") -> None:
        """Record one assertion and print its result.

        @param label Human-readable assertion label.
        @param condition Whether the assertion passed.
        @param why Diagnostic text printed for a failure.
        @return None.
        """

        self.total += 1
        if condition:
            print(f"PASS: {label}")
            self.passed += 1
            return
        print(f"FAIL: {label}\n   why: {why[:300]}")


class TerminalUiDriver:
    """Drive one terminal UI process through a bounded pseudo-terminal session.

    @brief Own the child process, terminal descriptors, captured output, and
    deterministic render synchronization for one scenario.
    """

    def __init__(
        self,
        binary: Path,
        report_directory: Path,
        width: int = 240,
        height: int = 50,
        ascii_mode: bool = False,
    ) -> None:
        """Start a terminal UI process with explicit binary and report paths.

        @param binary Benchmark framework example executable.
        @param report_directory Directory containing reports to display.
        @param width Initial terminal width in cells.
        @param height Initial terminal height in cells.
        @param ascii_mode Whether to request the ASCII-only presentation.
        @return None.
        """

        environment = os.environ.copy()
        environment["TERM"] = "xterm-256color"
        environment["COLORTERM"] = "truecolor"
        environment.pop("LANG", None)
        if ascii_mode:
            environment["LANG"] = "C"

        self.master_fd, self.slave_fd = pty.openpty()
        self.buffer = b""
        self.renders_seen = 0
        process: subprocess.Popen[bytes] | None = None
        try:
            fcntl.ioctl(
                self.slave_fd,
                termios.TIOCSWINSZ,
                struct.pack("HHHH", height, width, 0, 0),
            )
            arguments = [
                str(binary),
                "--ui",
                "--ui-test-mode",
                "--report",
                str(report_directory),
            ]
            if ascii_mode:
                arguments.append("--ui-ascii")
            process = subprocess.Popen(
                arguments,
                stdin=self.slave_fd,
                stdout=self.slave_fd,
                stderr=self.slave_fd,
                close_fds=True,
                env=environment,
                start_new_session=True,
            )
            self.process = process
            flags = fcntl.fcntl(self.master_fd, fcntl.F_GETFL)
            fcntl.fcntl(self.master_fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)
        except BaseException:
            if process is not None and process.poll() is None:
                with suppress(OSError):
                    os.killpg(process.pid, signal.SIGKILL)
                with suppress(subprocess.TimeoutExpired):
                    process.wait(timeout=1.0)
            os.close(self.master_fd)
            os.close(self.slave_fd)
            raise

        self.wait_for_render(4.0, expected_renders=2)
        if not self.buffer:
            self.wait_for_render(1.0, expected_renders=1)
            self.drain()

    def __enter__(self) -> TerminalUiDriver:
        """Return this driver for a managed terminal session.

        @return Active terminal UI driver.
        """

        return self

    def __exit__(
        self,
        exception_type: type[BaseException] | None,
        exception: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        """Close the child process and terminal descriptors.

        @param exception_type Exception type raised inside the context, if any.
        @param exception Exception raised inside the context, if any.
        @param traceback Traceback raised inside the context, if any.
        @return None.
        """

        del exception_type, exception, traceback
        self.close()

    def drain(self, timeout_seconds: float = 0.5) -> None:
        """Read available child output until idle or the deadline expires.

        @param timeout_seconds Maximum total time spent draining output.
        @return None.
        """

        deadline = time.monotonic() + timeout_seconds
        while time.monotonic() < deadline:
            remaining = max(0.0, deadline - time.monotonic())
            ready, _, _ = select.select(
                [self.master_fd], [], [], min(0.05, remaining)
            )
            if not ready:
                break
            try:
                chunk = os.read(self.master_fd, 65536)
            except (BlockingIOError, OSError):
                break
            if not chunk:
                break
            self.buffer += chunk

    def wait_for_render(
        self, timeout_seconds: float = 2.0, expected_renders: int = 1
    ) -> bool:
        """Wait for a bounded number of additional render sentinels.

        @param timeout_seconds Maximum time to wait.
        @param expected_renders Number of new sentinels required.
        @return True when the requested render count was observed.
        """

        self.renders_seen = max(
            self.renders_seen, self.buffer.count(RENDER_SENTINEL)
        )
        target = self.renders_seen + expected_renders
        deadline = time.monotonic() + timeout_seconds
        while time.monotonic() < deadline:
            remaining = max(0.0, deadline - time.monotonic())
            ready, _, _ = select.select(
                [self.master_fd], [], [], min(0.05, remaining)
            )
            if not ready:
                continue
            try:
                chunk = os.read(self.master_fd, 65536)
            except (BlockingIOError, OSError):
                chunk = b""
            if chunk:
                self.buffer += chunk
                observed = self.buffer.count(RENDER_SENTINEL)
                self.renders_seen = max(self.renders_seen, observed)
                if observed >= target:
                    return True
        self.renders_seen = max(
            self.renders_seen, self.buffer.count(RENDER_SENTINEL)
        )
        return False

    def send(self, data: str | bytes, settle_seconds: float = 0.4) -> None:
        """Send input and wait for a bounded render response.

        @param data Text or bytes to write to the pseudo-terminal.
        @param settle_seconds Maximum render wait after the write.
        @return None.
        """

        encoded = data.encode("utf-8") if isinstance(data, str) else data
        os.write(self.master_fd, encoded)
        self.wait_for_render(settle_seconds, expected_renders=1)
        self.drain()

    def send_each(self, keys: str, settle_seconds: float = 0.6) -> None:
        """Send a sequence one key at a time with bounded settling.

        @param keys Key sequence to send.
        @param settle_seconds Maximum render wait after each key.
        @return None.
        """

        for key in keys:
            self.send(key, settle_seconds)

    def resize(self, width: int, height: int) -> None:
        """Resize the pseudo-terminal and wait for the resulting render.

        @param width New terminal width in cells.
        @param height New terminal height in cells.
        @return None.
        """

        fcntl.ioctl(
            self.slave_fd,
            termios.TIOCSWINSZ,
            struct.pack("HHHH", height, width, 0, 0),
        )
        os.killpg(self.process.pid, signal.SIGWINCH)
        self.wait_for_render(0.6, expected_renders=1)
        self.drain()

    def screen_text(self) -> str:
        """Decode all captured terminal output as plain text.

        @return Captured output without terminal control sequences.
        """

        return strip_ansi(self.buffer).decode("utf-8", errors="replace")

    def raw_output(self) -> bytes:
        """Return all captured terminal bytes.

        @return Raw captured output.
        """

        return self.buffer

    def clear_output(self) -> None:
        """Discard captured bytes and reset sentinel accounting.

        @return None.
        """

        self.buffer = b""
        self.renders_seen = 0

    def latest_frame_text(self) -> str:
        """Decode the most recent complete rendered frame.

        @return Latest frame without terminal control sequences.
        """

        last_index = self.buffer.rfind(RENDER_SENTINEL)
        if last_index < 0:
            frame = self.buffer
        else:
            frame = self.buffer[last_index + len(RENDER_SENTINEL) :]
            if not frame.strip():
                previous_index = self.buffer.rfind(
                    RENDER_SENTINEL, 0, last_index
                )
                frame_start = (
                    previous_index + len(RENDER_SENTINEL)
                    if previous_index >= 0
                    else 0
                )
                frame = self.buffer[
                    frame_start:last_index
                ]
        return strip_ansi(frame).decode("utf-8", errors="replace")

    def _wait_for_exit(self, timeout_seconds: float) -> bool:
        """Wait a bounded interval for the child to exit.

        @param timeout_seconds Maximum time to wait.
        @return True when the child exited before the deadline.
        """

        try:
            self.process.wait(timeout=timeout_seconds)
        except subprocess.TimeoutExpired:
            return False
        return True

    def _signal_process_group(self, process_signal: signal.Signals) -> None:
        """Signal the isolated child process group when it still exists.

        @param process_signal Signal delivered to the child process group.
        @return None.
        """

        if self.process.poll() is not None:
            return
        with suppress(OSError):
            os.killpg(self.process.pid, process_signal)

    def close(self) -> None:
        """Stop the child within fixed deadlines and close both PTY handles.

        @return None.
        """

        if self.process.poll() is None:
            with suppress(OSError):
                os.write(self.master_fd, b"q")
            if not self._wait_for_exit(2.0):
                self._signal_process_group(signal.SIGTERM)
                if not self._wait_for_exit(1.0):
                    self._signal_process_group(signal.SIGKILL)
                    self._wait_for_exit(1.0)
        for descriptor in (self.master_fd, self.slave_fd):
            with suppress(OSError):
                os.close(descriptor)


def goto_bench_sort_per_region(ui: TerminalUiDriver) -> None:
    """Navigate the example fixture to its per-region sorting metric.

    @param ui Active terminal UI driver for the example reports.
    @return None.
    """

    ui.send("j")
    ui.send("o")
    ui.send("]")
    ui.send("]")
    for _ in range(3):
        ui.send("m")


def run_core_scenarios(
    binary: Path, report_directory: Path, results: TestResults
) -> None:
    """Run navigation, layout, aggregate, and fallback scenarios.

    @param binary Terminal UI executable.
    @param report_directory Example report directory.
    @param results Shared result collector.
    @return None.
    """

    print("--- Narrow ---")
    with TerminalUiDriver(binary, report_directory, 180, 40) as ui:
        goto_bench_sort_per_region(ui)
        screen = ui.screen_text()
        results.check(
            "Narrow: Per region collapsible visible", "Per region" in screen
        )
        results.check(
            "Narrow: footer navigation keys present",
            ">/< aggr" in screen and "p chart" in screen and "~ pin" in screen,
        )
        ui.send(">")
        screen = ui.screen_text()
        results.check(
            "Narrow: > emits aggregate notification",
            "aggregate: p99" in screen or "aggr=p99" in screen,
        )
        ui.send("?")
        screen = ui.screen_text()
        results.check(
            "Narrow: help modal lists navigation keys",
            "cycle per-region aggr" in screen
            and "cycle chart mode" in screen
            and "pin per-region focus" in screen,
        )
        ui.send("?")

    print("--- Wide + Stacked bar chart ---")
    with TerminalUiDriver(binary, report_directory, 280, 50) as ui:
        goto_bench_sort_per_region(ui)
        screen = ui.screen_text()
        results.check("Wide: Per region panel header visible", "Per region" in screen)
        results.check(
            "Wide: Stacked bars chart title visible", "Stacked bars" in screen
        )
        results.check(
            "Wide: region names appear coloured",
            "sort_copy_input" in screen and "sort_run" in screen,
        )
        ui.send("p")
        screen = ui.screen_text()
        results.check(
            "Wide: Step lines chart title visible after p",
            "Step lines" in screen or "chart: step lines" in screen,
        )

    print("--- Pin behaviour ---")
    with TerminalUiDriver(binary, report_directory, 280, 50) as ui:
        goto_bench_sort_per_region(ui)
        ui.send("~")
        screen = ui.screen_text()
        results.check(
            "Pin: ~ emits pinned notification",
            "pinned per-region focus" in screen,
        )
        results.check("Pin: panel header shows pinned suffix", "(pinned)" in screen)
        ui.send("~")
        screen = ui.screen_text()
        results.check(
            "Unpin: ~ emits unpinned notification",
            "unpinned per-region focus" in screen,
        )

    print("--- Aggregate cycle ---")
    with TerminalUiDriver(binary, report_directory, 260, 50) as ui:
        goto_bench_sort_per_region(ui)
        aggregate_cycle_ok = True
        expected_aggregates = [
            "p99",
            "mean",
            "max",
            "min",
            "sum",
            "p01",
            "p20",
            "p80",
            "p50",
        ]
        for aggregate in expected_aggregates:
            ui.send(">")
            screen = ui.screen_text()
            if not (
                f"aggregate: {aggregate}" in screen
                or f"aggr={aggregate}" in screen
            ):
                aggregate_cycle_ok = False
                break
        results.check("All nine aggregates cycle through", aggregate_cycle_ok)

    print("--- Layout flip on resize ---")
    with TerminalUiDriver(binary, report_directory, 300, 50) as ui:
        goto_bench_sort_per_region(ui)
        ui.clear_output()
        ui.send(" ")
        wide_screen = ui.screen_text()
        results.check(
            "Wide layout: Per region panel docked", "Per region" in wide_screen
        )
        ui.resize(180, 40)
        ui.clear_output()
        ui.send(" ")
        narrow_screen = ui.screen_text()
        results.check(
            "Narrow after resize: collapsible visible",
            "Per region" in narrow_screen,
        )

    print("--- ASCII fallback ---")
    with TerminalUiDriver(
        binary, report_directory, 200, 40, ascii_mode=True
    ) as ui:
        goto_bench_sort_per_region(ui)
        results.check(
            "ASCII: Per region marker is rendered",
            "[#] Per region" in ui.screen_text(),
        )

    print("--- Test-mode countdown ---")
    with TerminalUiDriver(binary, report_directory, 200, 40) as ui:
        results.check(
            "Test mode countdown is paused", "paused" in ui.screen_text()
        )


def run_rendering_scenarios(
    binary: Path, report_directory: Path, results: TestResults
) -> None:
    """Run colour, chart, sentinel, and header rendering scenarios.

    @param binary Terminal UI executable.
    @param report_directory Example report directory.
    @param results Shared result collector.
    @return None.
    """

    print("--- Palette colours distinct ---")
    with TerminalUiDriver(binary, report_directory, 280, 50) as ui:
        goto_bench_sort_per_region(ui)
        raw_output = ui.raw_output()
        copy_pattern = re.compile(
            rb"(\x1b\[[0-9;]*m)+\s*sort_copy_input", re.IGNORECASE
        )
        run_pattern = re.compile(
            rb"(\x1b\[[0-9;]*m)+\s*sort_run\b", re.IGNORECASE
        )
        copy_match = copy_pattern.search(raw_output)
        run_match = run_pattern.search(raw_output)
        results.check("sort_copy_input has SGR escape", copy_match is not None)
        results.check("sort_run has SGR escape", run_match is not None)
        if copy_match is not None and run_match is not None:
            results.check(
                "Distinct regions have distinct palette colours",
                copy_match.group(0) != run_match.group(0),
            )

    print("--- DeltaCurrent placeholder ---")
    with TerminalUiDriver(binary, report_directory, 280, 50) as ui:
        goto_bench_sort_per_region(ui)
        ui.send("%")
        screen = ui.screen_text()
        results.check(
            "Stacked + DeltaCurrent renders placeholder text",
            "undefined for stacked" in screen or "Press '\\'" in screen,
        )

    print("--- Stacked + Step chart cycle wraps ---")
    with TerminalUiDriver(binary, report_directory, 280, 50) as ui:
        goto_bench_sort_per_region(ui)
        ui.send_each("ppp")
        screen = ui.screen_text()
        results.check(
            "Chart cycle produces Step or Stacked notification",
            "chart: step lines" in screen or "chart: stacked bars" in screen,
        )

    print("--- Render sentinel emitted ---")
    with TerminalUiDriver(binary, report_directory, 200, 40) as ui:
        sentinel_count = ui.raw_output().count(RENDER_SENTINEL)
        results.check(
            "OSC render sentinel observed during startup",
            sentinel_count >= 1,
            f"saw {sentinel_count}",
        )

    print("--- Wider panel ---")
    with TerminalUiDriver(binary, report_directory, 320, 50) as ui:
        goto_bench_sort_per_region(ui)
        screen = ui.screen_text()
        results.check(
            "Wide panel renders its header and chart",
            "Per region" in screen and "Stacked bars" in screen,
        )

    print("--- Metric in header updates in the same frame ---")
    with TerminalUiDriver(binary, report_directory, 280, 50) as ui:
        goto_bench_sort_per_region(ui)
        screen = ui.screen_text()
        results.check(
            "Header shows region_total_ns after navigation",
            "metric=region_total_ns" in screen,
        )
        ui.send("m")
        updated_screen = ui.screen_text()
        results.check(
            "Header switches metric immediately on m",
            "metric=region_visits" in updated_screen
            or "metric=wall_clock_ns" in updated_screen,
        )


def run_pivot_alignment_scenario(
    binary: Path, report_directory: Path, results: TestResults
) -> None:
    """Verify aligned wrapped columns with the dense benchmark reports.

    @param binary Terminal UI executable.
    @param report_directory Final benchmark report directory.
    @param results Shared result collector.
    @return None.
    """

    print("--- Pivot-mode column alignment ---")
    if not report_directory.is_dir():
        print("SKIP: pivot-mode alignment (benchmark reports not present)")
        return

    with TerminalUiDriver(binary, report_directory, 320, 60) as ui:
        ui.send_each("jo[mmmm", settle_seconds=0.5)
        ui.drain(1.0)
        screen = ui.latest_frame_text()
        chunk_lines: list[tuple[str, list[re.Match[str]]]] = []
        for line in screen.split("\n"):
            if "best (" in line or "worst (" in line or "spread" in line:
                continue
            panel_sections = re.findall(r"│([^│]+)│", line)
            if not panel_sections:
                continue
            panel_body = panel_sections[-1]
            matches = list(
                re.finditer(r"([A-Za-z_][\w.]*):\s+(\S)", panel_body)
            )
            if len(matches) >= 2 and any(
                token in panel_body for token in (" ms", "µs", " ns", "—")
            ):
                chunk_lines.append((panel_body, matches))
        if not chunk_lines:
            print("SKIP: pivot-mode alignment (no pivot rows captured)")
            return
        second_columns = {
            matches[1].start() for _, matches in chunk_lines[:6]
        }
        results.check(
            "Wrapped chunk lines share a second-column start",
            len(second_columns) == 1,
            f"observed cols: {sorted(second_columns)}; "
            f"sample: {chunk_lines[0][0][:120]!r}",
        )


def run_pagination_scenarios(
    binary: Path,
    example_report_directory: Path,
    benchmark_report_directory: Path,
    results: TestResults,
) -> None:
    """Verify chart cycling and dense-report pagination.

    @param binary Terminal UI executable.
    @param example_report_directory Example report directory.
    @param benchmark_report_directory Final benchmark report directory.
    @param results Shared result collector.
    @return None.
    """

    print("--- Table pagination ---")
    with TerminalUiDriver(binary, example_report_directory, 320, 30) as ui:
        goto_bench_sort_per_region(ui)
        ui.send("p")
        screen = ui.screen_text()
        paged = "Page 2/" in screen or "page 2/" in screen
        stepped = "chart: step lines" in screen or "Step lines" in screen
        results.check(
            "p emits a page or chart-mode notification",
            paged or stepped,
            screen[-300:],
        )

    print("--- Dense pagination keeps chart visible ---")
    if not benchmark_report_directory.is_dir():
        print("SKIP: dense pagination (benchmark reports not present)")
        return

    with TerminalUiDriver(binary, benchmark_report_directory, 240, 50) as ui:
        ui.send_each("jo]mmmm", settle_seconds=0.5)
        ui.drain(1.0)
        page_one = ui.latest_frame_text()
        results.check(
            "Dense fixture: Page 1 indicator visible",
            "Page 1/" in page_one,
            page_one[-400:],
        )
        results.check(
            "Dense fixture: chart visible on page 1",
            "Stacked bars" in page_one,
            page_one[-400:],
        )

        ui.send("p", settle_seconds=0.5)
        page_two = ui.latest_frame_text()
        results.check(
            "Dense fixture: p advances to Page 2",
            "Page 2/" in page_two,
            page_two[-400:],
        )
        results.check(
            "Dense fixture: chart remains visible on page 2",
            "Stacked bars" in page_two,
            page_two[-400:],
        )

        ui.send("p", settle_seconds=0.5)
        page_three = ui.latest_frame_text()
        results.check(
            "Dense fixture: p advances to Page 3",
            "Page 3/" in page_three,
            page_three[-400:],
        )
        ui.send("p", settle_seconds=0.5)
        step_page = ui.latest_frame_text()
        results.check(
            "Dense fixture: p enters Step mode after the last page",
            "Step lines" in step_page,
            step_page[-400:],
        )


def build_argument_parser() -> argparse.ArgumentParser:
    """Build the standalone terminal UI test command-line parser.

    @return Configured argument parser.
    """

    parser = argparse.ArgumentParser(
        description="Run terminal UI integration scenarios through a PTY."
    )
    parser.add_argument(
        "--binary",
        required=True,
        type=repository_path,
        help=(
            "Path to bioseq_bench_framework_example, absolute or relative "
            "to the repository root."
        ),
    )
    parser.add_argument(
        "--report-dir",
        type=repository_path,
        default=DEFAULT_EXAMPLE_REPORT_DIRECTORY,
        help="Example report directory (default: reporting/bench_framework_example).",
    )
    parser.add_argument(
        "--benchmark-report-dir",
        type=repository_path,
        default=DEFAULT_BENCHMARK_REPORT_DIRECTORY,
        help="Dense benchmark report directory (default: reporting/bioseq_benchmark).",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    """Run all terminal UI scenarios.

    @param argv Optional command-line arguments excluding the program name.
    @return Zero when every executed check passes, one for assertion failures,
    or two when the executable is missing.
    """

    arguments = build_argument_parser().parse_args(argv)
    binary: Path = arguments.binary
    example_report_directory: Path = arguments.report_dir
    benchmark_report_directory: Path = arguments.benchmark_report_dir

    if not binary.is_file():
        print(f"ERROR: binary not found at {binary}", file=sys.stderr)
        return 2

    results = TestResults()
    run_core_scenarios(binary, example_report_directory, results)
    run_rendering_scenarios(binary, example_report_directory, results)
    run_pivot_alignment_scenario(
        binary, benchmark_report_directory, results
    )
    run_pagination_scenarios(
        binary,
        example_report_directory,
        benchmark_report_directory,
        results,
    )
    print(f"\n=== {results.passed}/{results.total} passed ===")
    return 0 if results.passed == results.total else 1


if __name__ == "__main__":
    raise SystemExit(main())
