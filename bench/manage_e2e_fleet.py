#!/usr/bin/env python3
"""Persistent, single-owner q1024 fleet controller with bounded recovery.

Run with --once --dry-run to inspect assignments, or --daemon to leave it
running without an agent polling. Logs, reservations and snapshots live in
reporting/bioseq_benchmark_e2e/fleet_controller/. All benchmark launches use
bioseq-build MCP run_only with verified frozen artifacts. The existing MCP
runtime restarts PostgreSQL before each invocation, including no-index jobs.
Reports are fetched compressed on terminal transitions and audited separately.
Failed tuples are not retried automatically. Known infrastructure failures may
receive one incremental repair per source-fix digest; never a blind build loop.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import fcntl
import hashlib
import json
import os
import re
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys
import time
import tarfile
import threading

import yaml
import monitor_e2e_fleet as monitor
from recover_e2e_measurements import COLLECTED, MANIFEST, ROOT, atomic, authority_path

OUTPUT = ROOT / "reporting/bioseq_benchmark_e2e/fleet_controller"
HOSTS = {h[0]: h for h in monitor.HOSTS}
ENVIRONMENT = (
    "export PATH=/opt/bioseq-package-toolchain/cmake/bin:/opt/bioseq-package-toolchain/llvm-22/bin:/usr/local/bin:/usr/bin:/bin "
    "VCPKG_ROOT=/opt/bioseq-package-toolchain/vcpkg "
    "LLVM_CONFIG_BINARY=/opt/bioseq-package-toolchain/llvm-22/bin/llvm-config "
    "BIOSEQ_LLVM_ROOT=/opt/bioseq-package-toolchain/llvm-22 ZIC=/usr/sbin/zic "
    "BIOSEQ_E2E_REUSE_FROZEN_BINARY=1; unset BIOSEQ_BUILD_PRESET; "
)


def event(kind: str, **fields) -> None:
    row = {"timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "event": kind, **fields}
    with (OUTPUT / "events.jsonl").open("a") as output:
        output.write(json.dumps(row) + "\n")
    print(json.dumps(row), flush=True)


def ssh(name: str) -> list[str]:
    _, ip, key, instance = HOSTS[name]
    if instance:
        error = monitor._refresh_default_key(instance)
        if error:
            raise RuntimeError(error)
    key = str(ROOT / key) if not key.startswith("/") else key
    return ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", "-o", "StrictHostKeyChecking=no",
            "-i", key, "ubuntu@" + ip]


def family(name: str) -> str:
    return name.rsplit("-", 1)[-1]


def reclaim_idle_local_indexes() -> None:
    """Remove only unreferenced, unopened benchmark image files on an idle host.

    PostgreSQL owns the catalogs. Never unlink a referenced image, manipulate
    PostgreSQL relation files, or delete files while a SQL workload is active.
    User explicitly authorized dropping benchmark tables/indexes for space.
    """
    if shutil.disk_usage(ROOT).free >= 35 * 1024**3 or monitor._local_state().get("status") == "running":
        return
    psql = "/opt/homebrew/opt/postgresql@17/bin/psql"
    def query(sql: str) -> list[str]:
        result = subprocess.run([psql, "-U", "postgres", "-d", "bioseq", "-At", "-v", "ON_ERROR_STOP=1", "-c", sql],
                                capture_output=True, text=True, timeout=20)
        if result.returncode:
            raise RuntimeError("disk cleanup catalog check failed: " + result.stderr[-500:])
        return result.stdout.splitlines()
    if query("SELECT count(*) FROM pg_stat_activity WHERE datname=current_database() AND pid<>pg_backend_pid() AND state<>'idle';") != ["0"]:
        return
    data = query("SHOW data_directory;")
    db = query("SELECT oid FROM pg_database WHERE datname=current_database();")
    if len(data) != 1 or len(db) != 1 or not db[0].isdigit():
        return
    owned_root = Path("/opt/homebrew/var/postgresql@17")
    if Path(data[0]).resolve() != owned_root.resolve():
        return
    image_root = owned_root / "base" / db[0] / "bioseq_minimizer_fragment_images"
    references = set(query("SELECT oid::text FROM pg_class UNION SELECT user_relid::text FROM bioseq_internal.minimizer_generation_storage UNION SELECT main_relfilenode::text FROM bioseq_internal.minimizer_generation_storage;"))
    removed = []
    reclaimed = 0
    for path in image_root.glob("*/*.minimizer_fragment_index"):
        match = re.fullmatch(r"(\d+)\.(\d+)\.\d+\.\d+\.\d+\.\d+\.minimizer_fragment_index", path.name)
        if not match or path.is_symlink() or match[1] in references or match[2] in references:
            continue
        if time.time() - path.stat().st_mtime < 3600:
            continue
        opened = subprocess.run(["lsof", str(path)], capture_output=True, timeout=15)
        if opened.returncode != 1 or opened.stdout or opened.stderr:
            continue
        reclaimed += path.stat().st_size
        path.unlink()
        removed.append(str(path))
    if removed:
        event("orphan_index_cleanup", removed=removed, bytes=reclaimed, recovery="regenerate benchmark indexes")


def priority(child: dict) -> tuple:
    i = child["execution_cell"]["identity"]
    gpu = "vulkan" in json.dumps(i)
    preferred = (i["requested_minimizer_algorithm"] == "bounded_nthash_simd"
                 and i["requested_chaining_algorithm"] == "simd_minimap2"
                 and i["requested_base_alignment_algorithm"] == "block_aligner")
    return (gpu, not preferred, i["engine"] != "minimap2", i["target_size_gbp"],
            i["alphabet"] != "dna4", i["index_mode"] != "just_index",
            i["bioseq_index_name"] != "minimizers_per_fragment", child["ordinal"])


def collect(name: str, token: str) -> bool:
    """Stream a compressed, bounded report bundle; never rsync the whole report tree."""
    dest = COLLECTED / name
    dest.mkdir(parents=True, exist_ok=True)
    inventory_path = dest / "collection_inventory.json"
    known = json.loads(inventory_path.read_bytes()) if inventory_path.exists() else {}
    # Include receipt/state backups when present, but their absence is not fatal.
    base = ROOT if name == "local" else Path("/home/ubuntu/bioseqdb")
    program = (
        "import pathlib,tarfile,sys,json,io; known=json.load(sys.stdin); "
        f"p=pathlib.Path({str(base / 'reporting/bioseq_benchmark_e2e')!r}); "
        "all_reports=sorted(p.glob('report_*.json'),key=lambda x:int(x.stem.split('_')[1])); "
        "fs=all_reports[-24:]; baselines={};\n"
        "for f in all_reports:\n"
        " try:\n"
        "  with f.with_suffix('.raw.jsonl').open() as raw:\n"
        "   next(raw); r=json.loads(next(raw))\n"
        "  if r.get('benchmark_shape',{}).get('expected_query_rows')==1024: fs.append(f)\n"
        "  if r.get('algorithm','').startswith('minimap2') and r.get('benchmark_shape',{}).get('expected_query_rows')==1024:\n"
        "   baselines[(r['test_case'],r['algorithm'])]=f\n"
        " except (OSError,ValueError,StopIteration): pass\n"
        "fs=list(dict.fromkeys([*baselines.values(),*fs])); selected=[]; inventory=dict(known);\n"
        "for f in fs:\n"
        " raw=f.with_suffix('.raw.jsonl')\n"
        " if not raw.exists(): continue\n"
        " signature=[f.stat().st_size,f.stat().st_mtime_ns,raw.stat().st_size,raw.stat().st_mtime_ns]\n"
        " if known.get(f.name)!=signature: selected.append(f); inventory[f.name]=signature\n"
        "t=tarfile.open(fileobj=sys.stdout.buffer,mode='w|gz'); "
        "[(t.add(f,arcname=f.name),t.add(f.with_suffix('.raw.jsonl'),arcname=f.with_suffix('.raw.jsonl').name) "
        "if f.with_suffix('.raw.jsonl').exists() else None) for f in selected]; "
        "body=json.dumps(inventory).encode(); info=tarfile.TarInfo('_collection_inventory.json'); "
        "info.size=len(body); t.addfile(info,io.BytesIO(body)); "
        "[t.add(f,arcname='control/'+f.name) for f in map(pathlib.Path,"
        "['/tmp/bioseq_build_mcp_receipts.json','/tmp/bioseq_build_mcp_state.json']) if f.exists()]; t.close()"
    )
    command = [sys.executable, "-c", program] if name == "local" else ssh(name) + ["python3 -c " + shlex.quote(program)]
    archive = dest / ("completion_" + token + ".tar.gz")
    temporary = archive.with_suffix(".partial")
    with temporary.open("wb") as output:
        result = subprocess.run(command, input=json.dumps(known).encode(), stdout=output, stderr=subprocess.PIPE, timeout=240)
    if result.returncode:
        event("collect_error", host=name, detail=result.stderr.decode(errors="replace")[-800:])
        return False
    os.replace(temporary, archive)
    with tarfile.open(archive) as tar:
        inventory = tar.extractfile("_collection_inventory.json").read()
    atomic(inventory_path, inventory)
    event("collected", host=name, bytes=archive.stat().st_size)
    return True


def sync_files(name: str, paths: tuple[str, ...]) -> None:
    if name == "local":
        return
    remote = ssh(name)
    for relative in paths:
        result = subprocess.run(
            ["rsync", "-az", "-e", shlex.join(remote[:-1]), str(ROOT / relative),
             remote[-1] + ":/home/ubuntu/bioseqdb/" + relative],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=45)
        if result.returncode:
            raise RuntimeError(result.stderr.decode(errors="replace")[-800:])


def mcp_start(name: str, args: dict, label: str) -> str:
    messages = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "protocolVersion": "2024-11-05", "capabilities": {}, "clientInfo": {"name": "q1024-fleet-controller", "version": "1"}}},
        {"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}},
        {"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {"name": "start_build", "arguments": args}},
    ]
    if name == "local":
        command = [sys.executable, "build_mcp/server.py"]
    else:
        remote = ssh(name)
        command = remote + ["cd /home/ubuntu/bioseqdb && " + ENVIRONMENT +
                            "export BIOSEQ_BUILD_PARALLEL_JOBS=3; " +
                            ("export BIOSEQ_BUILD_HARD_TIMEOUT_MIN=720; "
                             if args.get("build_only") and family(name) == "m4" else "") +
                            "python3 build_mcp/server.py"]
    log = OUTPUT / f"{label}_{name}_{int(time.time())}.log"
    with log.open("wb") as output:
        result = subprocess.run(command, input=("\n".join(json.dumps(m) for m in messages) + "\n").encode(),
                                stdout=output, stderr=subprocess.STDOUT, cwd=ROOT, timeout=150,
                                env=os.environ | {"BIOSEQ_E2E_REUSE_FROZEN_BINARY": "1", "BIOSEQ_BUILD_PARALLEL_JOBS": "2"})
    response = None
    for line in log.read_text(errors="replace").splitlines():
        try:
            value = json.loads(line)
        except ValueError:
            continue
        if value.get("id") == 2:
            response = value
    payload = (response or {}).get("result", {})
    text = " ".join(item.get("text", "") for item in payload.get("content", []))
    if result.returncode or not response or "error" in response or payload.get("isError") or " is scheduled (" not in text:
        raise RuntimeError("MCP did not admit launch: " + (text or str(response))[-1200:])
    return str(log)


def dispatch(name: str, receipt: str, child: dict) -> None:
    config = child["path"]
    sync_files(name, ("build_mcp/build_worker.py", "build_mcp/q1024_watchdog.py",
                      "bench/aggregate_e2e_algorithm_matrix.py", config))
    log = mcp_start(name, {"mode": "benchmark-e2e", "run_only": True, "debug": False,
        "reuse_build_receipt": receipt, "bench_e2e_config": config,
        "filter": {"benchmarks": ["bench_e2e"]}}, f"dispatch_{child['ordinal']:04d}")
    observed = monitor._local_state() if name == "local" else monitor._remote_state(*HOSTS[name])
    if observed.get("bench_e2e_config") != config:
        raise RuntimeError("MCP accepted launch, but active config does not match; inspect " + log)
    event("dispatch_verified", host=name, ordinal=child["ordinal"], status=observed.get("status"),
          phase=observed.get("phase"), log=log)


def repair_build(name: str, s: dict, state: dict, dry_run: bool) -> bool:
    """Allow one incremental retry only after a specifically identified fix."""
    reason = None
    paths = ("build_mcp/build_worker.py", "build_mcp/q1024_watchdog.py", "bench/aggregate_e2e_algorithm_matrix.py")
    if "source graph changed during build" in (s.get("notes") or ""):
        reason = "operational_source_fingerprint"
    elif (family(name) == "m4" and s.get("build_only") is True
          and s.get("notes") == "process identity snapshot deadline was reached"):
        # These slow hosts exhausted the old four-hour build budget while
        # compiling. Preserve every completed object and resume once with the
        # supported build-only timeout override, not a clean rebuild. Admission
        # still revalidates that the previous owned processes have exited.
        reason = "m4_incremental_after_build_deadline"
        paths += ("CMakeLists.txt", "bench/tool_minimap2/e2e_phase_receipt.cmake")
    elif s.get("status") == "failed" and str(s.get("phase")).startswith("build:") and name != "local":
        command = "tail -80 " + shlex.quote(s.get("log_file") or "/dev/null")
        result = subprocess.run(ssh(name) + [command], capture_output=True, text=True, timeout=30)
        if "undefined symbol: glslang::" in result.stdout:
            reason = "minimap2_helper_static_link_closure"
            paths += ("bench/tool_minimap2/e2e_phase_receipt.cmake",)
        elif "libc++.modules.json was not found" in result.stdout:
            reason = "llvm_triple_directory_module_inventory"
            paths += ("CMakeLists.txt", "bench/tool_minimap2/e2e_phase_receipt.cmake")
    if reason is None:
        event("build_blocked", host=name, detail=s.get("notes"))
        return False
    digest = hashlib.sha256((reason + "".join(
        hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in paths)).encode()).hexdigest()
    repairs = state.setdefault("build_repairs", {})
    # Receipt IDs change on each retry; the repair key deliberately does not.
    previous = repairs.get(name, {})
    history = set(previous.get("history", []))
    if previous.get("reason"):
        history.add(previous["reason"])
    if reason in history:
        event("build_repair_exhausted", host=name, detail=s.get("notes"))
        return False
    event("build_repair", host=name, reason=reason, fix_sha256=digest, dry_run=dry_run)
    if not dry_run:
        repairs[name] = {"reason": reason, "fix_sha256": digest, "at": time.time(),
                         "history": sorted(history | {reason})}
        atomic(OUTPUT / "state.json", (json.dumps(state, indent=2) + "\n").encode())
        sync_files(name, paths)
        log = mcp_start(name, {"mode": "benchmark-e2e", "build_only": True, "debug": False, "fresh": False}, "repair")
        event("build_repair_admitted", host=name, log=log)
    return True


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--once", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--daemon", action="store_true")
    parser.add_argument("--interval-seconds", type=int, default=120)
    args = parser.parse_args()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    if args.interval_seconds < 60:
        parser.error("minimum polling interval is 60 seconds")
    if args.daemon:
        with (OUTPUT / "controller.stdout").open("ab") as output:
            command = [sys.executable, str(Path(__file__).resolve()), "--interval-seconds", str(args.interval_seconds)]
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=output, stderr=output,
                                       cwd=ROOT, start_new_session=True)
        print(json.dumps({"controller_pid": process.pid, "output": str(OUTPUT)}))
        return
    lock = (OUTPUT / "controller.lock").open("a")
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    atomic(OUTPUT / "controller.pid", (str(os.getpid()) + "\n").encode())
    state_path = OUTPUT / "state.json"
    state = json.loads(state_path.read_bytes()) if state_path.exists() else {"attempted": {}, "collected": {}, "last_dispatch": {}}
    wake = threading.Event()
    signal.signal(signal.SIGUSR1, lambda *_: wake.set())
    state.setdefault("last_hourly_log", time.time())
    children = sorted(json.loads(MANIFEST.read_bytes())["children"], key=priority)
    by_path = {c["path"]: c for c in children}
    recovery = None
    while True:
        snapshot = {"local": monitor._local_state()}
        def poll(h):
            try:
                return h[0], monitor._remote_state(*h)
            except Exception as exc:
                return h[0], {"status": "unreachable", "error": str(exc)}
        with ThreadPoolExecutor(max_workers=4) as pool:
            snapshot.update(pool.map(poll, monitor.HOSTS))
        atomic(OUTPUT / "snapshot.json", (json.dumps({"timestamp": time.time(), "hosts": snapshot}, indent=2) + "\n").encode())
        occupied = {}
        for name, s in snapshot.items():
            reserved = occupied.setdefault(family(name), set())
            path = authority_path(name)
            if path.exists():
                d = yaml.load(path.read_bytes(), Loader=yaml.CSafeLoader)
                reserved.update(r["stable_key"] for r in d["approved_results"]
                                if r.get("timing_scope") == "query_only_framework_iteration_v1")
            config = s.get("bench_e2e_config")
            if config in by_path:
                # Reserve observed attempts, including terminal failures, to
                # avoid spending the night rerunning the same failing tuple.
                key = by_path[config]["stable_key"]
                state["attempted"].setdefault(name, [])
                if key not in state["attempted"][name]:
                    state["attempted"][name].append(key)
            reserved.update(state["attempted"].get(name, []))
            summary_path = COLLECTED / name / "numeric_recovery_summary.json"
            if summary_path.exists():
                reserved.update(r["stable_key"] for r in json.loads(summary_path.read_bytes())["dispositions"])
        for name, s in snapshot.items():
            try:
                if s.get("status") not in ("completed", "aborted", "failed"):
                    continue
                if name == "local" and not args.dry_run:
                    reclaim_idle_local_indexes()
                # A failed build has no reusable receipt. Repair classified
                # infrastructure failures once; never submit its pending ID.
                if s.get("status") == "failed" and not s.get("bench_e2e_config"):
                    repair_build(name, s, state, args.dry_run)
                    continue
                receipt = s.get("build_receipt_id")
                token = "v3_" + (s.get("run_receipt_id") or Path(s.get("log_file") or "unknown").stem)
                if state["collected"].get(name) != token and not args.dry_run:
                    if shutil.disk_usage(ROOT).free < 4 * 1024**3:
                        # Reports remain durable on the worker. A temporary
                        # local collection backlog must not idle twelve AWS hosts.
                        event("disk_guard", host=name, action="collection_deferred_remote_dispatch_allowed")
                    elif collect(name, token):
                        state["collected"][name] = token
                previous = state["last_dispatch"].get(name, {})
                if not receipt:
                    event("dispatch_blocked", host=name, detail="No successful reusable build receipt")
                    continue
                if previous.get("previous_token") == token and previous.get("accepted") is True:
                    continue
                if name == "local" and shutil.disk_usage(ROOT).free < 12 * 1024**3:
                    event("disk_guard", host=name, action="dispatch_paused")
                    continue
                reserved = occupied[family(name)]
                choices = [c for c in children if c["stable_key"] not in reserved
                           and "ksw2_batched" not in c["stable_key"]
                           and (name == "local" or "vulkan" not in c["stable_key"])]
                if s.get("template_profile") == "dna4_uint64":
                    # Existing x64 binaries explicitly reject DNA5 typmods.
                    # Continue useful DNA4 coverage with these binaries; DNA5
                    # Bioseq cells remain unmeasured until an all-alphabet build.
                    # External minimap2 has its own independent alphabet support.
                    choices = [c for c in choices if c["execution_cell"]["identity"]["engine"] == "minimap2"
                               or c["execution_cell"]["identity"]["alphabet"] == "dna4"]
                if family(name) == "c7g":
                    # 8GiB hosts currently reject 4G input admission; spend
                    # available time collecting 2G cells, not repeating that failure.
                    choices = [c for c in choices if c["execution_cell"]["identity"]["target_size_gbp"] == 2]
                if not choices:
                    continue
                selected = choices[0]
                reserved.add(selected["stable_key"])
                event("assignment", host=name, ordinal=selected["ordinal"], dry_run=args.dry_run)
                if args.dry_run:
                    continue
                state["last_dispatch"][name] = {"previous_token": token, "ordinal": selected["ordinal"],
                                               "at": time.time(), "accepted": False}
                atomic(state_path, (json.dumps(state, indent=2) + "\n").encode())
                dispatch(name, receipt, selected)
                state["last_dispatch"][name]["accepted"] = True
                state["attempted"].setdefault(name, []).append(selected["stable_key"])
                atomic(state_path, (json.dumps(state, indent=2) + "\n").encode())
            except Exception as exc:
                event("host_error", host=name, detail=str(exc))
        if not args.dry_run:
            atomic(state_path, (json.dumps(state, indent=2) + "\n").encode())
            if recovery is None or recovery.poll() is not None:
                with (OUTPUT / "recovery.log").open("ab") as output:
                    recovery = subprocess.Popen(["nice", "-n", "19", sys.executable,
                        "bench/recover_e2e_measurements.py", "--publish"], cwd=ROOT,
                        stdin=subprocess.DEVNULL, stdout=output, stderr=output)
            if time.time() - state.setdefault("last_hourly_log", time.time()) >= 3600:
                # DOC0432's path was resolved through bioseq-build doc_read.
                # Append operational facts only; this is not result authority.
                counts = {}
                for value in snapshot.values():
                    status = str(value.get("status"))
                    counts[status] = counts.get(status, 0) + 1
                log = ROOT / "DOCS/CURRENT/BENCHMARK_FRAMEWORK/0432_LOCAL_E2E_ALGORITHM_MATRIX_BENCHMARK_LOG.md"
                with log.open("a") as output:
                    output.write("\n### " + time.strftime("%Y-%m-%d %H:%M %Z") +
                                 " — automatic q1024 fleet checkpoint\n\n" +
                                 "- MCP host statuses: " + json.dumps(counts, sort_keys=True) + ".\n" +
                                 "- Controller reuses binaries; only classified infrastructure faults receive "
                                 "one bounded incremental repair. Completed reports are collected compressed, "
                                 "and raw audit/publication runs independently. Statuses describe workers, "
                                 "not confirmed new results. Per-host phases and blockers are in the "
                                 "persistent fleet_controller snapshot/events.\n")
                state["last_hourly_log"] = time.time()
                atomic(state_path, (json.dumps(state, indent=2) + "\n").encode())
        event("cycle_complete", hosts={k: {"status": v.get("status"), "phase": v.get("phase")} for k, v in snapshot.items()})
        if args.once:
            break
        wake.wait(args.interval_seconds)
        wake.clear()


if __name__ == "__main__":
    main()
