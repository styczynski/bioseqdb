#!/usr/bin/env python3
"""V18 UI smoke harness — drives the bench_framework_example binary
through a PTY and asserts on rendered output. Stdlib only (no pexpect
or pyte) per BENCH_V18_IMPL_DESIGN.md §10.4.1.

The harness uses ftxui's render-complete sentinel (`\\x1b]666;render-N\\x07`,
emitted only under `--ui-test-mode`) for deterministic frame-by-frame
synchronisation; falls back to a small `time.sleep` when sentinels
aren't being produced (e.g. early frames during startup).

Run as:
    python3 main/bench_framework/tests/v18_ui_smoke.py

Exit code 0 = all stories pass; 1 = any failure.
"""
import os
import pty
import fcntl
import termios
import struct
import select
import time
import re
import signal
import subprocess
import sys

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
BINARY = os.path.join(REPO_ROOT, "main", "build", "bench-opt", "bioseq",
                      "bioseq_bench_framework_example")
REPORT_DIR = os.path.join(REPO_ROOT, "main", "reporting", "bench_framework_example")

ANSI_RE = re.compile(rb"\x1b\[[0-9;?]*[ -/]*[@-~]")
OSC_RE = re.compile(rb"\x1b\][^\x07]*\x07")
RENDER_SENTINEL = b"\x1b]666;render-N\x07"


def strip_ansi(b):
    """Remove all CSI/OSC sequences from `b`. Returns plain text bytes."""
    return ANSI_RE.sub(b"", OSC_RE.sub(b"", b))


class UI:
    def __init__(self, dim_x=240, dim_y=50, ascii_mode=False):
        env = os.environ.copy()
        env["TERM"] = "xterm-256color"
        env["COLORTERM"] = "truecolor"
        env.pop("LANG", None)
        if ascii_mode:
            env["LANG"] = "C"
        self.master_fd, self.slave_fd = pty.openpty()
        fcntl.ioctl(self.slave_fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", dim_y, dim_x, 0, 0))
        args = [BINARY, "--ui", "--ui-test-mode"]
        if ascii_mode:
            args.append("--ui-ascii")
        args += ["--report", REPORT_DIR]
        self.proc = subprocess.Popen(
            args, stdin=self.slave_fd, stdout=self.slave_fd, stderr=self.slave_fd,
            close_fds=True, env=env, start_new_session=True,
        )
        fl = fcntl.fcntl(self.master_fd, fcntl.F_GETFL)
        fcntl.fcntl(self.master_fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)
        self.buf = b""
        self.renders_seen = 0
        # Initial settle — wait for at least 2 sentinels so we capture
        # the content of one full frame between them (the sentinel is
        # written BEFORE ftxui flushes the next frame's bytes per
        # §10.4.1, so n sentinels bracket n−1 fully-flushed frames).
        self.wait_for_render(4.0, expected_renders=2)
        if not self.buf:
            time.sleep(1.0)
            self.drain()

    def drain(self):
        while True:
            r, _, _ = select.select([self.master_fd], [], [], 0.05)
            if not r:
                break
            try:
                chunk = os.read(self.master_fd, 65536)
            except (BlockingIOError, OSError):
                break
            if not chunk:
                break
            self.buf += chunk

    def wait_for_render(self, timeout=2.0, expected_renders=1):
        """Block until the binary has emitted `expected_renders` more
        OSC sentinels since the last call, or `timeout` elapses. The
        sentinel is the `--ui-test-mode` "render-complete" marker."""
        target = self.renders_seen + expected_renders
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            r, _, _ = select.select([self.master_fd], [], [], 0.05)
            if r:
                try:
                    chunk = os.read(self.master_fd, 65536)
                except (BlockingIOError, OSError):
                    chunk = b""
                if chunk:
                    self.buf += chunk
                    seen = self.buf.count(RENDER_SENTINEL)
                    if seen >= target:
                        self.renders_seen = seen
                        return True
        return False

    def send(self, data, settle=0.4):
        """Write `data` and wait for at least one new sentinel (or `settle`s)."""
        if isinstance(data, str):
            data = data.encode("utf-8")
        os.write(self.master_fd, data)
        self.wait_for_render(settle, expected_renders=1)
        self.drain()

    def resize(self, x, y):
        fcntl.ioctl(self.slave_fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", y, x, 0, 0))
        os.killpg(self.proc.pid, signal.SIGWINCH)
        self.wait_for_render(0.6, expected_renders=1)
        self.drain()

    def screen_text(self):
        return strip_ansi(self.buf).decode("utf-8", errors="replace")

    def raw(self):
        return self.buf

    def quit(self):
        try:
            os.write(self.master_fd, b"q")
        except OSError:
            pass
        try:
            self.proc.wait(timeout=2)
        except Exception:
            try:
                self.proc.terminate()
                self.proc.wait(timeout=1)
            except Exception:
                pass
        for fd in (self.master_fd, self.slave_fd):
            try:
                os.close(fd)
            except OSError:
                pass


_PASSED = 0
_TOTAL = 0


def t(label, cond, why=""):
    global _PASSED, _TOTAL
    _TOTAL += 1
    if cond:
        print(f"PASS: {label}")
        _PASSED += 1
    else:
        print(f"FAIL: {label}\n   why: {why[:300]}")


def goto_bench_sort_per_region(ui):
    """Navigate to bench_sort/TINY/region_total_ns — the only triple in
    the example fixture that exercises real per-region samples."""
    ui.send("j")          # report 1
    ui.send("o")          # expand-all
    ui.send("]")          # benchmark forward
    ui.send("]")          # again → bench_sort
    # Metrics in alphabetical order:
    #   output_size, region_max_ns, region_total_ns, region_visits, wall_clock_ns
    # Default = wall_clock_ns; cycle 3 times via `m` → region_total_ns.
    for _ in range(3):
        ui.send("m")


def main():
    if not os.path.exists(BINARY):
        print(f"ERROR: binary not found at {BINARY}", file=sys.stderr)
        return 2

    print("--- Narrow ---")
    ui = UI(180, 40)
    goto_bench_sort_per_region(ui)
    txt = ui.screen_text()
    t("Narrow: Per region collapsible visible", "Per region" in txt)
    t("Narrow: footer V18 keys present",
      ">/< aggr" in txt and "p chart" in txt and "~ pin" in txt)
    ui.send(">")
    txt = ui.screen_text()
    t("Narrow: > emits aggregate notification",
      "aggregate: p99" in txt or "aggr=p99" in txt)
    ui.send("?")
    txt = ui.screen_text()
    t("Narrow: help modal lists V18 keys",
      "cycle per-region aggr" in txt
      and "cycle chart mode" in txt
      and "pin per-region focus" in txt)
    ui.send("?")
    ui.quit()

    print("--- Wide + Stacked bar chart ---")
    ui = UI(280, 50)
    goto_bench_sort_per_region(ui)
    txt = ui.screen_text()
    t("Wide: Per region panel header visible", "Per region" in txt)
    t("Wide: Stacked bars chart title visible (default Mode A)",
      "Stacked bars" in txt)
    t("Wide: region names appear coloured",
      "sort_copy_input" in txt and "sort_run" in txt)
    ui.send("p")
    txt = ui.screen_text()
    t("Wide: Step lines chart title visible after p",
      "Step lines" in txt or "chart: step lines" in txt)
    ui.quit()

    print("--- Pin behaviour ---")
    ui = UI(280, 50)
    goto_bench_sort_per_region(ui)
    ui.send("~")
    txt = ui.screen_text()
    t("Pin: ~ emits 'pinned per-region focus'",
      "pinned per-region focus" in txt)
    t("Pin: panel header shows '(pinned)' suffix", "(pinned)" in txt)
    ui.send("~")
    txt = ui.screen_text()
    t("Unpin: ~ emits 'unpinned per-region focus'",
      "unpinned per-region focus" in txt)
    ui.quit()

    print("--- Aggregate cycle ---")
    ui = UI(260, 50)
    goto_bench_sort_per_region(ui)
    ok = True
    expected = ["p99", "mean", "max", "min", "sum", "p01", "p20", "p80", "p50"]
    for label in expected:
        ui.send(">")
        txt = ui.screen_text()
        if not (f"aggregate: {label}" in txt or f"aggr={label}" in txt):
            ok = False
            break
    t("All 9 aggregates cycle through", ok)
    ui.quit()

    print("--- Layout flip on resize ---")
    ui = UI(300, 50)
    goto_bench_sort_per_region(ui)
    ui.buf = b""
    ui.send(" ")
    buf_wide = ui.screen_text()
    t("Wide layout: Per region panel docked", "Per region" in buf_wide)
    ui.resize(180, 40)
    ui.buf = b""
    ui.send(" ")
    buf_narrow = ui.screen_text()
    t("Narrow after resize: collapsible visible", "Per region" in buf_narrow)
    ui.quit()

    print("--- ASCII fallback ---")
    ui = UI(200, 40, ascii_mode=True)
    goto_bench_sort_per_region(ui)
    txt = ui.screen_text()
    t("ASCII: '[#] Per region' (note emoji 📝→[#])",
      "[#] Per region" in txt)
    ui.quit()

    print("--- Test-mode countdown ---")
    ui = UI(200, 40)
    txt = ui.screen_text()
    t("Test mode countdown 'paused'", "paused" in txt)
    ui.quit()

    print("--- Palette colours distinct ---")
    ui = UI(280, 50)
    goto_bench_sort_per_region(ui)
    raw = ui.raw()
    pat_sci = re.compile(rb"(\x1b\[[0-9;]*m)+\s*sort_copy_input", re.IGNORECASE)
    pat_sr = re.compile(rb"(\x1b\[[0-9;]*m)+\s*sort_run\b", re.IGNORECASE)
    m1 = pat_sci.search(raw)
    m2 = pat_sr.search(raw)
    t("sort_copy_input has SGR escape", m1 is not None)
    t("sort_run has SGR escape", m2 is not None)
    if m1 and m2:
        t("Distinct palette colours for distinct regions",
          m1.group(0) != m2.group(0))
    ui.quit()

    print("--- DeltaCurrent placeholder ---")
    ui = UI(280, 50)
    goto_bench_sort_per_region(ui)
    ui.send("%")
    txt = ui.screen_text()
    t("Stacked + DeltaCurrent renders placeholder text",
      "undefined for stacked" in txt or "Press '\\'" in txt)
    ui.quit()

    print("--- Stacked + Step chart cycle wraps ---")
    ui = UI(280, 50)
    goto_bench_sort_per_region(ui)
    ui.send("p")          # Step alg #1
    ui.send("p")          # Step alg #2
    ui.send("p")          # Step alg #3 OR back to Stacked, depending on count
    txt = ui.screen_text()
    t("Step or Stacked notification produced through cycle",
      "chart: step lines" in txt or "chart: stacked bars" in txt)
    ui.quit()

    print("--- Render sentinel emitted ---")
    ui = UI(200, 40)
    cnt = ui.buf.count(RENDER_SENTINEL)
    t("OSC render sentinel observed at least once during startup",
      cnt >= 1, f"saw {cnt}")
    ui.quit()

    print(f"\n=== {_PASSED}/{_TOTAL} passed ===")
    return 0 if _PASSED == _TOTAL else 1


if __name__ == "__main__":
    sys.exit(main())
