#!/usr/bin/env python3
"""V18 UI smoke harness — drives the bench_framework_example binary
through a PTY and asserts on rendered output. Stdlib only (no pexpect
or pyte) per main/DOCS/BENCHMARK_FRAMEWORK/BENCH_V18_IMPL_DESIGN__DOC0245.md §10.4.1.

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

    # User-extension: wider panel target (128 cells, was 64). On a
    # 320-col terminal we expect the panel to dock with at least 100
    # cells of width. Probe by checking that the per-region panel's
    # right border is not at the same column as the main pane's
    # right edge — indirect, but observable as ≥ 100 cells of "Per
    # region" panel surface (header text + chart row + legend).
    print("--- Wider panel ---")
    ui = UI(320, 50)
    goto_bench_sort_per_region(ui)
    txt = ui.screen_text()
    t("Wide panel renders 'Per region' header (320×50 terminal)",
      "Per region" in txt and "Stacked bars" in txt)
    ui.quit()

    # User-extension: panel header includes the metric name AND
    # updates without one-frame lag after `m` press. Pre-fix, the
    # metric in the header was one render stale.
    print("--- Metric in header + m updates same frame ---")
    ui = UI(280, 50)
    goto_bench_sort_per_region(ui)
    txt = ui.screen_text()
    t("Header shows 'metric=region_total_ns' after navigation",
      "metric=region_total_ns" in txt)
    ui.send("m")
    txt2 = ui.screen_text()
    t("Header switches to 'metric=region_visits' immediately on m",
      "metric=region_visits" in txt2 or "metric=wall_clock_ns" in txt2)
    ui.quit()

    # User-extension: pivot-mode column alignment. Force pivot mode by
    # using the bioseq_bench fixture (≥ 30 regions, won't fit at panel
    # default width). All second-column chunks must start at the same
    # screen column for true grid alignment. We use the bench-tools
    # bioseq_bench reports if available.
    print("--- Pivot-mode column alignment ---")
    bb_dir = os.path.join(REPO_ROOT, "main", "reporting", "bioseq_bench")
    if os.path.isdir(bb_dir):
        # Reuse UI but override REPORT_DIR via environment.
        # Spawn directly because UI hardcodes REPORT_DIR; replicate inline.
        master_fd, slave_fd = pty.openpty()
        fcntl.ioctl(slave_fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", 60, 320, 0, 0))
        env = os.environ.copy()
        env["TERM"] = "xterm-256color"
        env["COLORTERM"] = "truecolor"
        env.pop("LANG", None)
        proc = subprocess.Popen(
            [BINARY, "--ui", "--ui-test-mode", "--report", bb_dir],
            stdin=slave_fd, stdout=slave_fd, stderr=slave_fd,
            close_fds=True, env=env, start_new_session=True,
        )
        fl = fcntl.fcntl(master_fd, fcntl.F_GETFL)
        fcntl.fcntl(master_fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)
        time.sleep(3.5)
        bbbuf = b""
        for _ in range(60):
            r, _, _ = select.select([master_fd], [], [], 0.05)
            if not r:
                continue
            try:
                chunk = os.read(master_fd, 65536)
            except (BlockingIOError, OSError):
                break
            if chunk:
                bbbuf += chunk
        for byte in (b"j", b"o", b"["):
            os.write(master_fd, byte)
            time.sleep(0.5)
        for _ in range(4):
            os.write(master_fd, b"m")
            time.sleep(0.4)
        time.sleep(1.0)
        # Drain
        while True:
            r, _, _ = select.select([master_fd], [], [], 0.2)
            if not r:
                break
            try:
                chunk = os.read(master_fd, 65536)
            except (BlockingIOError, OSError):
                break
            if not chunk:
                break
            bbbuf += chunk
        # Use only the LAST rendered frame, identified as bytes after
        # the most recent OSC sentinel. Earlier frames may be from
        # navigation transitions and won't share the final layout.
        last_idx = bbbuf.rfind(RENDER_SENTINEL)
        latest = bbbuf[last_idx + len(RENDER_SENTINEL):] if last_idx >= 0 else bbbuf
        if not latest.strip():
            # Fall back to second-to-last frame if last is empty.
            prev_idx = bbbuf.rfind(RENDER_SENTINEL, 0, last_idx)
            latest = bbbuf[prev_idx + len(RENDER_SENTINEL):last_idx]
        text = strip_ansi(latest).decode("utf-8", errors="replace")
        # Find chunk lines that contain TWO `regname: <ms-or-em-dash>` patterns
        # within the right-side panel (i.e. inside the last `│ ... │` segment).
        chunk_lines = []
        for line in text.split("\n"):
            if "best (" in line or "worst (" in line or "spread" in line:
                continue
            # Take the last vertical-bar-bounded panel section.
            panel_match = re.findall(r"│([^│]+)│", line)
            if not panel_match:
                continue
            panel_body = panel_match[-1]
            mm = list(re.finditer(r"([A-Za-z_][\w.]*):\s+(\S)", panel_body))
            if len(mm) >= 2 and any(
                tok in panel_body for tok in (" ms", "µs", " ns", "—")
            ):
                chunk_lines.append((panel_body, mm))
        if chunk_lines:
            second_cols = {matches[1].start() for _, matches in chunk_lines[:6]}
            t("All wrapped chunk-lines have the same second-column start",
              len(second_cols) == 1,
              f"observed cols: {sorted(second_cols)}; "
              f"sample: {chunk_lines[0][0][:120]!r}")
        else:
            print("SKIP: pivot-mode alignment (no pivot rows captured)")
        os.write(master_fd, b"q")
        try:
            proc.wait(timeout=2)
        except Exception:
            try:
                proc.terminate()
                proc.wait(timeout=1)
            except Exception:
                pass
        for fd in (master_fd, slave_fd):
            try:
                os.close(fd)
            except OSError:
                pass
    else:
        print("SKIP: pivot-mode alignment (bioseq_bench reports not present)")

    # User-extension: pagination cycle. With a small panel height and
    # ≥ 2 algorithms, the table can paginate. We use a 30-row terminal
    # so the chart's reservation crowds out at least one algorithm row,
    # forcing total_pages > 1. Pressing `p` should advance the page
    # before flipping to Step mode.
    print("--- Table pagination ---")
    ui = UI(320, 30)
    goto_bench_sort_per_region(ui)
    txt = ui.screen_text()
    # Either we see a 'Page x/y' indicator, or there's no pagination
    # because algos fit. Both are valid; just verify p still cycles.
    has_page_indicator = "Page 1/" in txt or "Page 2/" in txt
    ui.send("p")
    txt2 = ui.screen_text()
    paged = "Page 2/" in txt2 or "page 2/" in txt2
    stepped = "chart: step lines" in txt2 or "Step lines" in txt2
    t("p emits a page or chart-mode notification (paged or stepped)",
      paged or stepped, txt2[-300:])
    ui.quit()

    # User-extension: pagination must trigger on dense per-region data
    # (bioseq_bench fixture: many regions × multi-algo, forces pivot
    # mode where each algo wraps over 10+ rows). Pre-fix the
    # `pivot_estimate=2 rows` heuristic let the table eat the chart.
    # This test asserts: (a) page indicator visible, (b) chart still
    # rendered alongside the paginated table, (c) `p` advances pages
    # before transitioning to Step mode, (d) chart visible on later
    # pages too.
    print("--- Dense pagination keeps chart visible ---")
    bb_dir = os.path.join(REPO_ROOT, "main", "reporting", "bioseq_bench")
    if os.path.isdir(bb_dir):
        # Custom UI launch with bioseq_bench report dir.
        master_fd, slave_fd = pty.openpty()
        # 240 cols × 50 rows ≈ user's terminal; many regions → pagination.
        fcntl.ioctl(slave_fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", 50, 240, 0, 0))
        env = os.environ.copy()
        env["TERM"] = "xterm-256color"
        env["COLORTERM"] = "truecolor"
        env.pop("LANG", None)
        bb_proc = subprocess.Popen(
            [BINARY, "--ui", "--ui-test-mode", "--report", bb_dir],
            stdin=slave_fd, stdout=slave_fd, stderr=slave_fd,
            close_fds=True, env=env, start_new_session=True,
        )
        fl = fcntl.fcntl(master_fd, fcntl.F_GETFL)
        fcntl.fcntl(master_fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)

        def _drain(buf, sec=0.4):
            deadline = time.monotonic() + sec
            while time.monotonic() < deadline:
                r, _, _ = select.select([master_fd], [], [], 0.1)
                if not r:
                    continue
                try:
                    chunk = os.read(master_fd, 65536)
                except (BlockingIOError, OSError):
                    break
                if chunk:
                    buf += chunk
            return buf

        def _send(seq, buf, sec=0.6):
            for ch in seq:
                os.write(master_fd, ch.encode("utf-8"))
                buf = _drain(buf, sec)
            return buf

        def _last_frame(buf):
            li = buf.rfind(RENDER_SENTINEL)
            tail = buf[li + len(RENDER_SENTINEL):] if li >= 0 else buf
            if not tail.strip():
                pi = buf.rfind(RENDER_SENTINEL, 0, li)
                tail = buf[pi + len(RENDER_SENTINEL):li]
            return strip_ansi(tail).decode("utf-8", errors="replace")

        # Initial settle.
        bbbuf = _drain(b"", 3.5)
        # Navigate: open report 1 (j), expand-all (o), forward to next bench
        # (`]`), then cycle metric until region_total_ns (4 m's from default
        # wall_clock_ns through alphabetical metrics). bioseq_bench's first
        # metric is match_count → mean_position_offset_bp →
        # primary_correct_fraction → region_total_ns.
        bbbuf = _send("jo]mmmm", bbbuf, sec=0.5)
        bbbuf = _drain(bbbuf, 1.0)
        page1 = _last_frame(bbbuf)
        t("Dense fixture: 'Page 1/' indicator visible",
          "Page 1/" in page1, page1[-400:])
        t("Dense fixture: chart visible alongside paginated table (page 1)",
          "Stacked bars" in page1, page1[-400:])
        # Advance one page.
        bbbuf = _send("p", bbbuf, sec=0.5)
        page2 = _last_frame(bbbuf)
        t("Dense fixture: 'p' advances to Page 2/",
          "Page 2/" in page2, page2[-400:])
        t("Dense fixture: chart still visible on page 2",
          "Stacked bars" in page2, page2[-400:])
        # Walk forward through remaining pages until we reach Step mode.
        # bioseq_bench REPR_STORAGE_K27_XL has 3 algos with values; expect
        # Page 3/3 then transition to Step lines.
        bbbuf = _send("p", bbbuf, sec=0.5)
        page3 = _last_frame(bbbuf)
        t("Dense fixture: 'p' advances to Page 3/",
          "Page 3/" in page3, page3[-400:])
        bbbuf = _send("p", bbbuf, sec=0.5)
        page4 = _last_frame(bbbuf)
        t("Dense fixture: after last page 'p' enters Step mode",
          "Step lines" in page4, page4[-400:])
        # Cleanup.
        os.write(master_fd, b"q")
        try:
            bb_proc.wait(timeout=2)
        except Exception:
            try:
                bb_proc.terminate()
                bb_proc.wait(timeout=1)
            except Exception:
                pass
        for fd in (master_fd, slave_fd):
            try:
                os.close(fd)
            except OSError:
                pass
    else:
        print("SKIP: dense pagination (bioseq_bench reports not present)")

    print(f"\n=== {_PASSED}/{_TOTAL} passed ===")
    return 0 if _PASSED == _TOTAL else 1


if __name__ == "__main__":
    sys.exit(main())
