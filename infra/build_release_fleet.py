#!/usr/bin/env python3
"""Reuse the existing fleet for MCP-owned packages and a static website.

No provisioning, automatic Git writes, S3 publication, or benchmark-binary replacement.
Run --launch once, then --daemon to collect artifacts on terminal transitions.
--retry HOST explicitly synchronizes fixes and retries only that failed job.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "bench"))
import manage_e2e_fleet as fleet

REMOTE = "/home/ubuntu/bioseqdb"
OUTPUT = ROOT / "bin_artifacts"
CONTROL = ROOT / "reporting/package_release_fleet"
STATE = CONTROL / "state.json"
PLAN = {
    "test3-c7g": ["debian_13", "ubuntu_26.04"],
    "test4-c7g": ["opensuse_16.0", "fedora_44"],
    "test2-c7g": ["arch"],
    "test3-m4": ["debian_13"],
    "test4-m4": ["ubuntu_26.04"],
    "test2-m4": ["opensuse_16.0"],
    "test3-m7a": ["fedora_44"],
    "test4-m7a": ["arch"],
    "test3-m7i": ["website"],
    "test4-m7i": ["debian_13"],
    "test2-m7a": ["ubuntu_26.04"],
    "test2-m7i": ["opensuse_16.0"],
}
SOURCE_ROOTS = (
    "CMakeLists.txt", "CMakePresets.json", "vcpkg.json", "vcpkg-configuration.json",
    "bench", "bench_framework", "bioseq", "bioseq_caching", "build_mcp", "cmake",
    "codegen", "common", "data", "infra", "overlays", "vendor", "website", "DOCS",
)


def rpc(command, tool, arguments):
    messages = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "protocolVersion": "2024-11-05", "capabilities": {},
            "clientInfo": {"name": "release-fleet", "version": "1"}}},
        {"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}},
        {"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {
            "name": tool, "arguments": arguments}}]
    result = subprocess.run(command, input="\n".join(map(json.dumps, messages)) + "\n",
                            capture_output=True, text=True, timeout=180)
    for line in result.stdout.splitlines():
        try:
            response = json.loads(line)
        except ValueError:
            continue
        if response.get("id") == 2:
            payload = response.get("result", {})
            if "error" in response or payload.get("isError"):
                raise RuntimeError(str(response))
            return payload["content"][0]["text"]
    raise RuntimeError(result.stderr[-1500:] + result.stdout[-1500:])


def event(kind, **fields):
    row = {"timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
           "event": kind, **fields}
    CONTROL.mkdir(parents=True, exist_ok=True)
    with (CONTROL / "events.jsonl").open("a") as output:
        output.write(json.dumps(row) + "\n")
    print(json.dumps(row), flush=True)


def remote(host, command, timeout=120):
    result = subprocess.run(fleet.ssh(host) + [command], text=True,
                            capture_output=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"{host}: {result.stderr[-1000:]}")
    return result.stdout


def state_on(host, mode):
    code = """import hashlib, json
from pathlib import Path
mode = %r
state = json.loads(Path('/tmp/bioseq_build_mcp_state.json').read_text()).get(mode, {})
manifest = Path('/home/ubuntu/bioseqdb/bin_artifacts/packages/manifest.json')
if mode == 'package-linux' and manifest.exists():
    raw = manifest.read_bytes()
    if json.loads(raw).get('packages'):
        state['package_manifest_digest'] = hashlib.sha256(raw).hexdigest()
print(json.dumps(state))
""" % mode
    document = json.loads(remote(host, "python3 -c " + shlex.quote(code)))
    return {key: document.get(key) for key in (
        "status", "step", "pid", "active_pid", "log_file", "notes", "completed_at",
        "report_files", "build_dir", "bench_e2e_config", "cleanup_unverified",
        "package_manifest_digest")}


def sync_source(host, roots=SOURCE_ROOTS):
    ssh = fleet.ssh(host)
    endpoint = ssh[-1]
    args = ["rsync", "-a", "--delete", "--relative", "-e", shlex.join(ssh[:-1])]
    for exclusion in ("build", "reporting", ".git", ".secrets", ".terraform", "*.tfstate*",
                      ".venv", "__pycache__", "*.pyc", ".DS_Store", "bin_artifacts"):
        args += ["--exclude=" + exclusion]
    args += [str(ROOT) + "/./" + path for path in roots]
    args += [endpoint + ":" + REMOTE + "/"]
    result = subprocess.run(args, capture_output=True, text=True, timeout=1200)
    if result.returncode:
        raise RuntimeError(result.stderr[-1000:])


def write_json_atomic(path, document):
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    try:
        temporary.write_text(json.dumps(document, indent=2) + "\n")
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def write_release_manifest():
    """Inventory accepted local bytes against the entire native package matrix."""
    destination = OUTPUT / "packages"
    destination.mkdir(parents=True, exist_ok=True)
    matrix = json.loads((ROOT / "infra/package_matrix.json").read_text())
    expected = {(distro["id"], architecture, major): distro["package_format"]
                for distro in matrix["distributions"]
                for architecture in matrix["architectures"]
                for major in matrix["postgresql_majors"]}
    packages = {}
    for source in sorted(destination.glob("*-manifest.json")):
        manifest = json.loads(source.read_text())
        for package in manifest.get("packages", []):
            key = (package["distro"], manifest["architecture"], package["postgres_major"])
            if key not in expected or key in packages:
                raise RuntimeError(f"unexpected or duplicate package cell: {key}")
            if (package["validation"].get("passed") is not True
                    or package["installed_sql_smoke"].get("passed") is not True
                    or package["validation"].get("format") != expected[key]):
                raise RuntimeError(f"package cell lacks matching native acceptance: {key}")
            target = (destination / package["file"]).resolve()
            if not target.is_relative_to(destination.resolve()):
                raise RuntimeError(f"package path escapes delivery directory: {target}")
            digest = hashlib.sha256()
            with target.open("rb") as data:
                for block in iter(lambda: data.read(1024 * 1024), b""):
                    digest.update(block)
            if (target.stat().st_size != package["bytes"]
                    or digest.hexdigest() != package["sha256"]):
                raise RuntimeError(f"local package bytes do not match acceptance: {target}")
            packages[key] = dict(package, architecture=manifest["architecture"],
                build_host=source.name.removesuffix("-manifest.json"),
                template_profile=manifest["template_profile"],
                optimization=manifest["optimization"],
                git_revision_informational_only=package.get("git_revision_informational_only", manifest.get("git_revision_informational_only")),
                postgres_release_channel=matrix.get("postgresql_release_channels", {}).get(str(key[2]), "unknown"),
                published=manifest["published"], signed=manifest["signed"])
    missing = [{"distro": distro, "architecture": architecture, "postgres_major": major}
               for distro, architecture, major in sorted(expected.keys() - packages.keys())]
    functional_pending = [dict(distro=key[0], architecture=key[1], postgres_major=key[2])
                          for key, package in sorted(packages.items())
                          if package["installed_sql_smoke"].get("default_insert_search") is not True]
    write_json_atomic(destination / "manifest.json", {
        "schema_revision": 1,
        "description": "Consolidated native package delivery inventory. Only ELF/payload-validated "
                       "and fresh-PostgreSQL-tested archives are listed; local bytes are SHA-256 "
                       "verified. Package paths are relative to this directory. Website acceptance "
                       "is separate; this inventory does not claim website completion.",
        "expected_package_count": len(expected),
        "validated_package_count": len(packages),
        "package_matrix_complete": not missing and not functional_pending,
        "functional_validation_pending": functional_pending,
        "release_ready_package_count": len(packages) - len(functional_pending),
        "missing_cells": missing,
        "packages": [packages[key] for key in sorted(packages)],
    })


def launch(host, *, validate_existing_only=False, cell=None, website_only_update=False):
    prior = state_on(host, "benchmark-e2e")
    if prior.get("status") == "running" or prior.get("cleanup_unverified"):
        raise RuntimeError("remote benchmark has not stopped cleanly; no package launch")
    mode = "package-linux" if cell else ("website" if PLAN[host] == ["website"] else "package-linux")
    if validate_existing_only and mode != "package-linux":
        raise RuntimeError("acceptance-only retry is only for native packages")
    existing = state_on(host, mode)
    if existing.get("status") == "running":
        if cell:
            raise RuntimeError("host already has a running job; reserved cell was not started")
        return existing
    if website_only_update and mode != "website":
        raise RuntimeError("website-only source refresh requires a website worker")
    # A render/indexing repair does not need unrelated PG19/20 package edits.
    # Keep the already-built PG17 extension tree and sync only the explicit
    # website source scope from local; the ordinary launch still syncs all roots.
    sync_source(host, ("codegen/website", "website") if website_only_update else SOURCE_ROOTS)
    # Native package validation needs these host-side archive readers only;
    # compilation and installed SQL run inside matching distro containers.
    host_packages = ["rpm", "cpio", "zstd", "binutils"]
    if mode == "website":
        # Playwright installs its pinned Chromium, but not Ubuntu's runtime
        # libraries. Provision these before the long website compilation.
        host_packages += [
            "libnss3", "libatk1.0-0t64", "libatk-bridge2.0-0t64", "libgbm1",
            "libasound2t64", "libxkbcommon0", "libxcomposite1", "libxdamage1",
            "libxrandr2", "libcups2t64", "libatspi2.0-0t64", "libxfixes3",
            "libpango-1.0-0", "libcairo2",
        ]
    remote(host, "sudo -n env DEBIAN_FRONTEND=noninteractive NEEDRESTART_MODE=l "
           "apt-get install -y -qq " + shlex.join(host_packages) +
           " > /tmp/bioseq-package-validation-setup.log 2>&1", 600)
    environment = fleet.ENVIRONMENT + "unset BIOSEQ_E2E_REUSE_FROZEN_BINARY; "
    environment += "export BIOSEQ_BUILD_HARD_TIMEOUT_MIN=720 "
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    environment += "BIOSEQ_PACKAGE_SOURCE_REVISION=" + shlex.quote(revision) + " "
    environment += "BIOSEQ_PACKAGE_DISTROS=" + shlex.quote(cell["distro"] if cell else ",".join(PLAN[host])) + "; "
    environment += "export BIOSEQ_PACKAGE_POSTGRES_MAJORS=" + str(cell["postgres_major"] if cell else 17) + "; "
    environment += "export BIOSEQ_PACKAGE_VALIDATE_EXISTING_ONLY=" + (
        "1" if validate_existing_only else "0") + "; "
    answer = rpc(fleet.ssh(host) + ["cd " + REMOTE + " && " + environment + "python3 build_mcp/server.py"],
                 "start_build", {"mode": mode})
    event("launch_response", host=host, mode=mode, response=str(answer))
    observed = state_on(host, mode)
    if observed.get("status") != "running":
        raise RuntimeError("MCP did not admit job: " + str(answer))
    return observed


def verify_website_delivery(destination):
    """Verify transferred bytes against the website's existing output manifest."""
    manifest = json.loads((destination / "site-manifest.json").read_text())
    outputs = manifest.get("outputs")
    if (manifest.get("schema") != "bioseq.website.site_manifest"
            or manifest.get("hash_algorithm") != "sha256"
            or not isinstance(outputs, dict) or "/index.html" not in outputs):
        raise RuntimeError("website delivery lacks the generated output manifest")
    actual = {}
    for path in destination.rglob("*"):
        if path.is_symlink():
            raise RuntimeError(f"website delivery contains a symlink: {path}")
        if path.is_dir():
            continue
        if not path.is_file():
            raise RuntimeError(f"website delivery contains a special file: {path}")
        route = "/" + path.relative_to(destination).as_posix()
        if route != "/site-manifest.json":
            actual[route] = hashlib.sha256(path.read_bytes()).hexdigest()
    if actual != outputs:
        missing = sorted(outputs.keys() - actual.keys())
        extra = sorted(actual.keys() - outputs.keys())
        changed = sorted(key for key in actual.keys() & outputs.keys()
                         if actual[key] != outputs[key])
        raise RuntimeError(f"website transfer differs from its manifest: "
                           f"missing={missing[:5]}, extra={extra[:5]}, changed={changed[:5]}")


def collect(host, observed, *, mode=None):
    ssh = fleet.ssh(host)
    endpoint = ssh[-1]
    mode = mode or ("website" if PLAN[host] == ["website"] else "package-linux")
    destination = OUTPUT / ("website" if mode == "website" else "packages")
    destination.mkdir(parents=True, exist_ok=True)
    if mode == "website":
        if observed.get("status") != "completed":
            return
        source = str(Path(observed["build_dir"]) / "website/site") + "/"
        remote(host, "test -f " + shlex.quote(source + "index.html"))
    else:
        source = REMOTE + "/bin_artifacts/packages/"
        if remote(host, "test -f " + source + "manifest.json && echo yes || true").strip() != "yes":
            return
    # Only the package root inventory is consolidated locally. A website (and
    # any nested package payload) must retain all of its own manifest assets.
    exclusions = ["--exclude=/manifest.json"] if mode == "package-linux" else []
    result = subprocess.run(["rsync", "-a", "-e", shlex.join(ssh[:-1]),
        *exclusions, endpoint + ":" + source, str(destination) + "/"],
        capture_output=True, text=True, timeout=1200)
    if result.returncode:
        raise RuntimeError(result.stderr[-1000:])
    if mode == "website":
        verify_website_delivery(destination)
    if mode == "package-linux":
        manifest = json.loads(remote(host, "cat " + source + "manifest.json"))
        manifest_path = destination / (host + "-manifest.json")
        if manifest_path.exists():
            # A failed acceptance-only retry must not erase earlier accepted
            # artifacts. Recheck their local bytes below before retaining them.
            incoming = {entry["file"] for entry in manifest["packages"]}
            previous = json.loads(manifest_path.read_text())
            manifest["packages"].extend(dict(entry, git_revision_informational_only=
                entry.get("git_revision_informational_only", previous.get("git_revision_informational_only")))
                for entry in previous.get("packages", []) if entry["file"] not in incoming)
        for package in manifest["packages"]:
            target = destination / package["file"]
            digest = hashlib.sha256()
            with target.open("rb") as data:
                for block in iter(lambda: data.read(1024 * 1024), b""):
                    digest.update(block)
            if digest.hexdigest() != package["sha256"]:
                raise RuntimeError(f"local artifact checksum mismatch: {target}")
        write_json_atomic(manifest_path, manifest)
        write_release_manifest()
    event("artifacts_collected" if mode == "website" or manifest["packages"] else "manifest_collected",
          host=host, destination=str(destination))


def cell_key(cell):
    return f"pg{cell['postgres_major']}/{cell['distro']}/{cell['architecture']}"


def dispatch_available(state):
    """Reserve distinct native cells before concurrent launch; do not loop failed builds."""
    matrix = json.loads((ROOT / "infra/package_matrix.json").read_text())
    inventory = json.loads((OUTPUT / "packages/manifest.json").read_text())
    occupied = {cell_key(p) for p in inventory["packages"]
                if p["installed_sql_smoke"].get("default_insert_search") is True}
    occupied.update(state.get("attempted_cells", []))
    for host, job in state["jobs"].items():
        if job.get("cell"):
            occupied.add(cell_key(job["cell"]))
        elif job.get("observed", {}).get("status") == "running" and PLAN[host] != ["website"]:
            for distro in job.get("targets", PLAN[host]):
                occupied.add(cell_key(dict(postgres_major=17, distro=distro,
                    architecture="arm64" if host.endswith("c7g") else "x64")))
    pending = []
    for host in PLAN:
        job = state["jobs"].get(host, {})
        observed = job.get("observed", {})
        if job.get("cell") and not job.get("launched") and not job.get("error"):
            # A controller interruption during admission leaves a reservation.
            # Reconcile it explicitly rather than overwrite a possibly live job.
            continue
        if observed.get("status") == "running" or observed.get("cleanup_unverified"):
            continue
        if job.get("refresh_after_current_pid") or job.get("retry_after_failure_pid"):
            continue
        if PLAN[host] == ["website"] and not (job.get("collected") and observed.get("status") == "completed"):
            continue
        architecture = "arm64" if host.endswith("c7g") else "x64"
        distros = sorted((d["id"] for d in matrix["distributions"]),
                        key=lambda d: (d not in PLAN[host], d))
        candidates = [dict(postgres_major=major, distro=distro, architecture=architecture)
                      for major in sorted(matrix["postgresql_majors"]) for distro in distros]
        cell = next((c for c in candidates if cell_key(c) not in occupied), None)
        if cell is None:
            continue
        key = cell_key(cell)
        occupied.add(key)
        state.setdefault("attempted_cells", []).append(key)
        state.setdefault("history", []).append(dict(host=host, previous_job=job))
        state["jobs"][host] = dict(launched=False, cell=cell, mode="package-linux", targets=[cell["distro"]])
        pending.append((host, cell))
        event("package_cell_reserved", host=host, cell=cell)
    write_json_atomic(STATE, state)
    def start(item):
        host, cell = item
        try:
            return host, dict(launched=True, observed=launch(host, cell=cell))
        except Exception as error:
            event("launch_failed", host=host, cell=cell, error=str(error))
            return host, dict(launched=False, error=str(error))
    with ThreadPoolExecutor(max_workers=12) as pool:
        for host, update in pool.map(start, pending):
            state["jobs"][host].update(update)
            write_json_atomic(STATE, state)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--launch", action="store_true")
    parser.add_argument("--daemon", action="store_true")
    parser.add_argument("--retry", choices=list(PLAN))
    parser.add_argument("--revalidate", choices=list(PLAN),
                        help="Validate/install-smoke existing package archives without compiling")
    parser.add_argument("--retry-after-failure", choices=list(PLAN),
                        help="Queue one repaired retry after this exact running MCP job drains")
    parser.add_argument("--refresh-after-current", choices=list(PLAN),
                        help="Queue one source refresh after this exact MCP job drains, without interrupting it")
    parser.add_argument("--retry-failed", action="store_true")
    args = parser.parse_args()
    CONTROL.mkdir(parents=True, exist_ok=True)
    lock = (CONTROL / "controller.lock").open("a")
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    if args.daemon:
        (CONTROL / "controller.pid").write_text(str(os.getpid()) + "\n")
    state = json.loads(STATE.read_text()) if STATE.exists() else {"jobs": {}}
    write_release_manifest()
    if args.retry_after_failure or args.refresh_after_current:
        host = args.retry_after_failure or args.refresh_after_current
        mode = "website" if PLAN[host] == ["website"] else "package-linux"
        observed = state_on(host, mode)
        if observed.get("status") != "running" or not observed.get("pid"):
            raise RuntimeError("deferred retry requires an identified running MCP worker")
        job = state["jobs"][host]
        pending_key = "refresh_after_current_pid" if args.refresh_after_current else "retry_after_failure_pid"
        job[pending_key] = observed["pid"]
        STATE.write_text(json.dumps(state, indent=2) + "\n")
        event("repaired_retry_queued", host=host, pid=observed["pid"])
    if args.launch or args.retry or args.retry_failed or args.revalidate:
        explicit_host = args.revalidate or args.retry
        selected = [explicit_host] if explicit_host else [
            h for h in PLAN if not state["jobs"].get(h, {}).get("launched")]
        if args.retry_failed:
            selected = [h for h in PLAN if state_on(h,
                "website" if PLAN[h] == ["website"] else "package-linux").get("status")
                not in ("running", "completed")]
        def start(host):
            try:
                cell = state["jobs"].get(host, {}).get("cell")
                return host, {"launched": True,
                              "observed": launch(host, validate_existing_only=bool(args.revalidate), cell=cell),
                              "cell": cell, "mode": "package-linux" if cell else ("website" if PLAN[host] == ["website"] else "package-linux"),
                              "targets": [cell["distro"]] if cell else PLAN[host]}
            except Exception as error:
                event("launch_failed", host=host, error=str(error))
                return host, {"launched": False, "error": str(error), "targets": PLAN[host]}
        with ThreadPoolExecutor(max_workers=9) as pool:
            for host, value in pool.map(start, selected):
                state["jobs"][host] = value
                STATE.write_text(json.dumps(state, indent=2) + "\n")
    while True:
        # Inspect and collect the priority website before package transfers.
        for host, job in sorted(state["jobs"].items(), key=lambda item:
                item[1].get("mode") != "website" and PLAN[item[0]] != ["website"]):
            if not job.get("launched"):
                continue
            try:
                mode = job.get("mode") or ("website" if PLAN[host] == ["website"] else "package-linux")
                observed = state_on(host, mode)
                previous = job.get("observed", {})
                job["observed"] = observed
                website_refresh = (mode == "website"
                    and job.get("refresh_after_current_pid") == observed.get("pid"))
                if (observed.get("status") != "running" and not job.get("collected")
                        and not website_refresh):
                    collect(host, observed, mode=mode)
                    job["collected"] = True
                    event("job_terminal", host=host, observed=observed)
                elif previous.get("step") != observed.get("step"):
                    event("job_progress", host=host, step=observed.get("step"))
                # The ARM hosts have two native distro cells each. Their
                # successful package entries appear in an atomic manifest
                # only after ELF and installed-SQL acceptance. Collect while
                # the next one compiles, without changing worker lifecycle.
                digest = observed.get("package_manifest_digest")
                if (observed.get("status") == "running" and digest
                        and digest != job.get("collected_package_manifest")):
                    collect(host, observed, mode=mode)
                    job["collected_package_manifest"] = digest
                    event("partial_packages_collected", host=host)
                refresh = bool(job.get("refresh_after_current_pid"))
                pending_key = "refresh_after_current_pid" if refresh else "retry_after_failure_pid"
                pending_pid = job.get(pending_key)
                if pending_pid and observed.get("status") != "running":
                    # One explicitly requested repair, never an automatic
                    # retry loop. Preserve completed sibling work before
                    # retrying, and do not cancel the still-running graph.
                    job.pop(pending_key)
                    STATE.write_text(json.dumps(state, indent=2) + "\n")
                    if (observed.get("status") in (("failed", "completed") if refresh else ("failed",))
                            and observed.get("pid") == pending_pid
                            and not observed.get("cleanup_unverified")):
                        event("repaired_retry_started", host=host, previous_pid=pending_pid)
                        job["observed"] = launch(host, cell=job.get("cell"),
                            website_only_update=bool(job.pop("website_only_update", False)))
                        job["collected"] = False
                    else:
                        event("repaired_retry_not_needed_or_unsafe", host=host,
                              observed=observed)
            except Exception as error:
                event("monitor_error", host=host, error=str(error))
        state["local_benchmark"] = fleet.monitor._local_state()
        if args.daemon:
            dispatch_available(state)
        if args.daemon and time.time() - state.get("last_hourly_log", 0) >= 3600:
            from e2e_tuple_progress import append_progress_log
            inventory = json.loads((OUTPUT / "packages/manifest.json").read_text())
            completed_packages = inventory["validated_package_count"]
            ready_packages = inventory["release_ready_package_count"]
            active_jobs = [h for h, job in state["jobs"].items()
                if job.get("observed", {}).get("status") == "running"]
            failed_jobs = [h for h, job in state["jobs"].items()
                if job.get("observed", {}).get("status") == "failed"]
            append_progress_log(
                f"Release fleet: {completed_packages}/{inventory['expected_package_count']} installation-validated packages collected; "
                f"{ready_packages}/{inventory['expected_package_count']} also passed current functional acceptance. "
                f"Active workers: {', '.join(active_jobs) or 'none'}. "
                f"Failed workers requiring investigation: {', '.join(failed_jobs) or 'none'}. "
                f"Local benchmark: {state['local_benchmark'].get('status')}; "
                "AWS tuple admission remains disabled. See reporting/package_release_fleet/state.json.",
                title="Native release fleet hourly checkpoint")
            state["last_hourly_log"] = time.time()
        STATE.write_text(json.dumps(state, indent=2) + "\n")
        if not args.daemon:
            for host, job in state["jobs"].items():
                observed = job.get("observed", {})
                print(host, observed.get("status", "not_launched"),
                      observed.get("step", job.get("error", "")))
            print("local", state["local_benchmark"].get("status"),
                  state["local_benchmark"].get("bench_e2e_config"))
            return
        time.sleep(60)


if __name__ == "__main__":
    main()
