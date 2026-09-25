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
import inspect
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import threading
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
_SNAPSHOT_LOCK = threading.Lock()


def committed_source_snapshot():
    """Freeze a complete local commit before copying a release batch.

    This is not a dirty-tree, receipt, or binary-reuse gate. A new commit never
    invalidates accepted binaries. It prevents the automatic dispatcher from
    copying half of a multi-file edit (e.g. an import before its module exists).
    Commit completed source batches locally before releasing their retries.
    """
    revision = subprocess.check_output(
        ['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    if re.fullmatch(r'[0-9a-f]{40}', revision) is None:
        raise RuntimeError('Cannot resolve local release source commit')
    destination = CONTROL / 'source-snapshots' / revision
    with _SNAPSHOT_LOCK:
        if destination.is_dir():
            return destination, revision
        destination.parent.mkdir(parents=True, exist_ok=True)
        staging = Path(tempfile.mkdtemp(prefix='.assembling-', dir=destination.parent))
        tracked = set(subprocess.check_output(
            ['git', 'ls-tree', '--name-only', revision], cwd=ROOT, text=True).splitlines())
        roots = [path for path in SOURCE_ROOTS if path in tracked]
        archive = subprocess.Popen(['git', 'archive', '--format=tar', revision, '--', *roots],
                                   cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            unpack = subprocess.run(['tar', '-xf', '-', '-C', str(staging)],
                                    stdin=archive.stdout, capture_output=True, timeout=120)
            archive.stdout.close()
            archive.stdout = None
            error = archive.communicate(timeout=30)[1]
            if unpack.returncode or archive.returncode:
                raise RuntimeError('Cannot materialize release source snapshot: ' +
                                   (unpack.stderr + error).decode(errors='replace')[-1000:])
        finally:
            if archive.poll() is None:
                archive.terminate()
                archive.wait(timeout=10)
        os.replace(staging, destination)
        event('local_release_source_frozen', revision=revision)
    return destination, revision


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


def worker_diagnostics(state, mode):
    """Read process identity and compiler errors; never change worker lifecycle.

    Keep this helper self-contained: the status command sends its source over
    SSH, so live workers do not need a source sync to expose diagnostics.
    """
    result = {key: state.get(key) for key in (
        "status", "step", "pid", "log_file", "notes", "cleanup_unverified")}
    result["pid_live"] = False
    result["compiler_errors"] = []
    pid = state.get("pid")
    if isinstance(pid, int) and not isinstance(pid, bool) and pid > 0:
        try:
            process = subprocess.run(
                ["ps", "-p", str(pid), "-o", "args="], capture_output=True,
                text=True, timeout=5)
            result["pid_live"] = (process.returncode == 0
                and "server.py" in process.stdout
                and f"--run-build {mode} " in process.stdout + " ")
        except (OSError, subprocess.TimeoutExpired) as error:
            result["process_observation_error"] = str(error)
    if state.get("log_file"):
        ansi = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
        diagnostic = re.compile(
            r":\d+:\d+: (?:fatal )?error:|^FAILED:|^ninja: build|"
            r"^(?:clang(?:\+\+|-[0-9]+)?|ld(?:\.lld)?): (?:fatal )?error:")
        try:
            with Path(state["log_file"]).open(errors="replace") as lines:
                for number, line in enumerate(lines, 1):
                    line = ansi.sub("", line).strip()
                    if diagnostic.search(line):
                        result["compiler_errors"].append(
                            {"line": number, "text": line[:450]})
                        if len(result["compiler_errors"]) == 3:
                            break
        except OSError as error:
            result["log_observation_error"] = str(error)
    return result


def pending_package_failures(state, inventory):
    """Keep failed cells visible after their host moves to another assignment."""
    latest = {}
    for entry in state.get("history", []):
        job = entry.get("previous_job", {})
        if job.get("cell"):
            latest[cell_key(job["cell"])] = (entry.get("host"), job)
    for host, job in state.get("jobs", {}).items():
        if job.get("cell"):
            latest[cell_key(job["cell"])] = (host, job)
    running = {cell_key(job["cell"]) for job in state.get("jobs", {}).values()
               if job.get("cell") and job.get("observed", {}).get("status") == "running"}
    qualified = {cell_key(package) for package in inventory.get("packages", [])
                 if package.get("installed_sql_smoke", {}).get("column_defaults_and_vacuum") is True}
    queued = {cell_key(cell) for cells in state.get("package_rebuilds", {}).values()
              for cell in cells}
    queued.update(cell_key(cell) for cell in state.get("validation_rechecks", {}).values())
    pending = []
    for key, (host, job) in sorted(latest.items()):
        observed = job.get("observed", {})
        status = observed.get("status")
        if key in qualified or key in running:
            continue
        if status not in ("failed", "aborted") and not job.get("error"):
            continue
        pending.append(dict(cell=job["cell"], host=host, status=status or "launch_failed",
                            pid=observed.get("pid"), log_file=observed.get("log_file"),
                            error=str(job.get("error", ""))[-400:], retry_queued=key in queued))
    return pending


def status_snapshot():
    """Observe the fleet without the controller lock, collection or admission."""
    state = json.loads(STATE.read_text()) if STATE.exists() else {"jobs": {}}
    manifest = OUTPUT / "packages/manifest.json"
    inventory = json.loads(manifest.read_text()) if manifest.exists() else {}
    # Sending the same helper used locally also permits offline mocked tests.
    source = ("import json, re, subprocess\nfrom pathlib import Path\n"
              + inspect.getsource(worker_diagnostics))
    def observe(host):
        job = state["jobs"].get(host, {})
        mode = job_mode(host, job)
        command = source + (
            "\nstate=json.loads(Path('/tmp/bioseq_build_mcp_state.json').read_text())"
            f".get({mode!r}, {{}})\nprint(json.dumps(worker_diagnostics(state, {mode!r})))\n")
        try:
            result = json.loads(remote(host, "python3 -c " + shlex.quote(command), timeout=40))
        except Exception as error:
            result = {"observation_error": str(error)}
        return host, {"cell": job.get("cell"), "mode": mode, **result}
    with ThreadPoolExecutor(max_workers=len(PLAN)) as pool:
        workers = dict(pool.map(observe, PLAN))
    try:
        local_state = json.loads(Path(fleet.monitor.STATE_PATH).read_text())
        local = worker_diagnostics(local_state.get("benchmark-e2e", {}), "benchmark-e2e")
        local_package = worker_diagnostics(local_state.get("package-macos", {}), "package-macos")
    except (OSError, ValueError) as error:
        local = {"observation_error": str(error)}
        local_package = dict(local)
    return {
        "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "accepted_packages": inventory.get("validated_package_count", 0),
        "release_ready_packages": inventory.get("release_ready_package_count", 0),
        "expected_packages": inventory.get("expected_package_count"),
        "pending_package_failures": pending_package_failures(state, inventory),
        "website_manifest_present": (OUTPUT / "website/site-manifest.json").is_file(),
        "local_benchmark": local,
        "local_package": local_package,
        "workers": workers,
    }


def sync_source(host, roots=SOURCE_ROOTS):
    source, revision = committed_source_snapshot()
    ssh = fleet.ssh(host)
    endpoint = ssh[-1]
    # Local edits can predate completion of the previous remote build. Keeping
    # their old timestamps would let Ninja reuse outputs from the old contents.
    # Compare contents to leave identical inputs alone; changed inputs acquire
    # their transfer-time mtime and therefore invalidate the right outputs.
    args = ["rsync", "-a", "--checksum", "--no-times", "--omit-dir-times",
            "--delete", "--relative", "-e", shlex.join(ssh[:-1])]
    for exclusion in ("build", "reporting", ".git", ".secrets", ".terraform", "*.tfstate*",
                      ".venv", "__pycache__", "*.pyc", ".DS_Store", "bin_artifacts"):
        args += ["--exclude=" + exclusion]
    args += [str(source) + "/./" + path for path in roots if (source / path).exists()]
    args += [endpoint + ":" + REMOTE + "/"]
    result = subprocess.run(args, capture_output=True, text=True, timeout=1200)
    if result.returncode:
        raise RuntimeError(result.stderr[-1000:])
    return revision


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
    # The local Apple Silicon worker contributes native installers to the same
    # delivery inventory, but never consumes a Linux fleet scheduling cell.
    expected.update({("macos", "arm64", major): "pkg"
                     for major in matrix["postgresql_majors"]})
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
                          if any(package["installed_sql_smoke"].get(field) is not True
                                 for field in ("default_insert_search", "default_persisted_search",
                                               "column_defaults_and_vacuum"))]
    write_json_atomic(destination / "manifest.json", {
        "schema_revision": 1,
        "description": "Consolidated native package delivery inventory. Only ELF or Mach-O/payload-validated "
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
    revision = sync_source(host, ("codegen/website", "website") if website_only_update else SOURCE_ROOTS)
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


def package_release_ready(package):
    return (package.get("validation", {}).get("passed") is True
            and all(package.get("installed_sql_smoke", {}).get(field) is True
                    for field in ("passed", "default_insert_search", "default_persisted_search",
                                  "column_defaults_and_vacuum")))


def collect_package_revision(host, manifest, source, destination, ssh):
    """Import only advertised bytes, then move cell ownership without losing revisions.

    Remote output directories retain earlier cells. Never rsync that whole tree:
    another worker may since have repaired a same-named package. Content-addressed
    local paths also keep the publisher's previous manifest snapshot valid.
    """
    manifest_path = destination / (host + "-manifest.json")
    manifests = {path: json.loads(path.read_text())
                 for path in destination.glob("*-manifest.json")}
    previous = manifests.get(manifest_path, {})
    def identity(package, owner):
        return package["postgres_major"], package["distro"], owner["architecture"]

    incoming = {identity(p, manifest): p for p in manifest["packages"]}
    if len(incoming) != len(manifest["packages"]):
        raise RuntimeError("Duplicate incoming package cell")
    displaced = {}
    for path, owner in manifests.items():
        if path == manifest_path:
            continue
        for package in owner.get("packages", []):
            replacement = incoming.get(identity(package, owner))
            if replacement is None:
                continue
            if package_release_ready(package) or not package_release_ready(replacement):
                raise RuntimeError("Cross-host replacement requires an unqualified old cell "
                                   "and a qualified new cell")
            displaced.setdefault(path, []).append(package)

    files = {}
    for package in manifest["packages"]:
        for entry in [package, *package.get("postgres_sdk_support_files", [])]:
            relative = Path(entry["file"])
            if (relative.is_absolute() or not relative.parts or ".." in relative.parts
                    or "\0" in entry["file"] or relative.as_posix() != entry["file"]
                    or re.fullmatch(r"[0-9a-f]{64}", entry["sha256"]) is None
                    or type(entry["bytes"]) is not int or entry["bytes"] < 0):
                raise RuntimeError("Unsafe advertised package file")
            prior = files.setdefault(entry["file"], entry)
            if (prior["sha256"], prior["bytes"]) != (entry["sha256"], entry["bytes"]):
                raise RuntimeError("Conflicting advertised package bytes")

    def verify(path, entry, root):
        if (path.is_symlink() or not path.resolve().is_relative_to(root.resolve())
                or not path.is_file() or path.stat().st_size != entry["bytes"]
                or hashlib.sha256(path.read_bytes()).hexdigest() != entry["sha256"]):
            raise RuntimeError(f"Collected artifact checksum/size/path mismatch: {path}")

    relocated = {}
    with tempfile.TemporaryDirectory(prefix=".package-import-", dir=destination) as temporary:
        staging = Path(temporary)
        if files:
            result = subprocess.run(["rsync", "-a", "--from0", "--files-from=-",
                "-e", shlex.join(ssh[:-1]), ssh[-1] + ":" + source, str(staging) + "/"],
                input="\0".join(sorted(files)) + "\0", capture_output=True, text=True, timeout=1200)
            if result.returncode:
                raise RuntimeError(result.stderr[-1000:])
        # Verify the entire transfer before accepting any entry or ownership change.
        for name, entry in files.items():
            verify(staging / name, entry, staging)
        for name, entry in files.items():
            relative = Path("collected_revisions") / entry["sha256"] / name
            target = destination / relative
            if not target.resolve().is_relative_to(destination.resolve()):
                raise RuntimeError("Revision path escapes the collection")
            if target.exists():
                verify(target, entry, destination)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                os.replace(staging / name, target)
            relocated[name] = relative.as_posix()
    for package in manifest["packages"]:
        for entry in [package, *package.get("postgres_sdk_support_files", [])]:
            entry["file"] = relocated[entry["file"]]

    manifest["retired_packages"] = list(previous.get("retired_packages", []))
    for package in previous.get("packages", []):
        package = dict(package, git_revision_informational_only=package.get(
            "git_revision_informational_only", previous.get("git_revision_informational_only")))
        if identity(package, previous) not in incoming:
            manifest["packages"].append(package)
        elif package["sha256"] != incoming[identity(package, previous)]["sha256"]:
            manifest["retired_packages"].append(package)
    for package in manifest["packages"]:
        for entry in [package, *package.get("postgres_sdk_support_files", [])]:
            verify(destination / entry["file"], entry, destination)
    for path, packages in displaced.items():
        owner = manifests[path]
        # Preserve both the old metadata and its untouched content-addressed (or
        # legacy) file. A crash here can be retried; it cannot erase old bytes.
        for package in packages:
            for entry in [package, *package.get("postgres_sdk_support_files", [])]:
                verify(destination / entry["file"], entry, destination)
            owner.setdefault("retired_packages", []).append(dict(package,
                superseded_by_host=host, git_revision_informational_only=package.get(
                    "git_revision_informational_only", owner.get("git_revision_informational_only"))))
        owner["packages"] = [p for p in owner["packages"] if identity(p, owner) not in incoming]
        write_json_atomic(path, owner)
    write_json_atomic(manifest_path, manifest)
    write_release_manifest()


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
        manifest = json.loads(remote(host, "cat " + source + "manifest.json"))
        collect_package_revision(host, manifest, source, destination, ssh)
    if mode == "website":
        result = subprocess.run(["rsync", "-a", "-e", shlex.join(ssh[:-1]),
            endpoint + ":" + source, str(destination) + "/"],
            capture_output=True, text=True, timeout=1200)
        if result.returncode:
            raise RuntimeError(result.stderr[-1000:])
        verify_website_delivery(destination)
    event("artifacts_collected" if mode == "website" or manifest["packages"] else "manifest_collected",
          host=host, destination=str(destination))


def cell_key(cell):
    return f"pg{cell['postgres_major']}/{cell['distro']}/{cell['architecture']}"


def job_mode(host, job):
    """Use the current assignment after the website host joins package work."""
    return job.get("mode") or (
        "package-linux" if job.get("cell") or PLAN[host] != ["website"] else "website")


def reclaim_retired_benchmark_intermediates(repository):
    """Recover only old x64 benchmark objects on an idle host; retain binaries.

    Self-contained for the controller's SSH invocation. Package caches, CMake
    configuration, source, reports, and directories are never removed.
    """
    repository = Path(repository)
    if not repository.is_absolute() or repository.resolve() != repository:
        raise ValueError("Cleanup requires an explicit nonsymlink repository path")
    if not (repository / "CMakeLists.txt").is_file():
        raise ValueError("Cleanup repository marker is missing")
    processes = subprocess.check_output(["ps", "-eo", "pid=,args="], text=True)
    for line in processes.splitlines():
        pid, _, command = line.strip().partition(" ")
        if pid.isdigit() and int(pid) not in (os.getpid(), os.getppid()):
            if re.search(r"(?:^|\s)(?:\S*/)?server\.py\s+--run-build(?:\s|$)", command):
                return {"skipped": "build worker is still live"}
    if subprocess.check_output(["docker", "ps", "-q"], text=True).strip():
        return {"skipped": "container is still live"}
    names = ("aws-x64-linux-e2e", "native-x64-clang22-linux-e2e",
             "test-native-x64-clang22-linux", "test-aws-x64-linux")
    trees = [repository / "build" / name for name in names]
    # Protect independently launched old binaries/compilers before any deletion.
    if any(str(tree) in line and line.strip().split(" ", 1)[0]
           not in (str(os.getpid()), str(os.getppid()))
           for tree in trees for line in processes.splitlines()):
        return {"skipped": "retired build is still referenced"}
    before = shutil.disk_usage(repository).free
    removed, removed_bytes = 0, 0
    touched = []
    for tree in trees:
        objects = tree / "bioseq/CMakeFiles"
        if not objects.is_dir() or objects.resolve() != objects:
            continue
        if not (tree / "CMakeCache.txt").is_file():
            continue
        touched.append(str(objects))
        for directory, _, files in os.walk(objects, followlinks=False):
            for filename in files:
                path = Path(directory) / filename
                if path.suffix not in (".o", ".pcm") or path.is_symlink() or not path.is_file():
                    continue
                size = path.stat().st_size
                path.unlink()
                removed += 1
                removed_bytes += size
        if shutil.disk_usage(repository).free >= 100 * 1024**3:
            break
    available = shutil.disk_usage(repository).free
    return dict(removed_files=removed, removed_file_bytes=removed_bytes,
                reclaimed_bytes=available - before, available_bytes=available, roots=touched)


def dispatch_available(state):
    """Reserve distinct native cells before concurrent launch; do not loop failed builds."""
    matrix = json.loads((ROOT / "infra/package_matrix.json").read_text())
    inventory = json.loads((OUTPUT / "packages/manifest.json").read_text())
    # Collected cells are not fresh work. Explicit pending qualification may
    # migrate to an otherwise idle compatible host, once per cell.
    collected = {cell_key(p) for p in inventory["packages"]}
    occupied = set(collected)
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
        if (job_mode(host, job) == "website"
                and not (job.get("collected") and observed.get("status") == "completed")):
            continue
        architecture = "arm64" if host.endswith("c7g") else "x64"
        distros = sorted((d["id"] for d in matrix["distributions"]),
                        key=lambda d: (d not in PLAN[host], d))
        candidates = [dict(postgres_major=major, distro=distro, architecture=architecture)
                      for major in sorted(matrix["postgresql_majors"]) for distro in distros]
        # Explicit, host-bound revalidation preserves an already built package
        # after a smoke-environment repair. It never interrupts the live cell.
        rechecks = state.get("validation_rechecks", {})
        validation_only = host in rechecks
        repair = None
        if state.get("qualify_shared_tableam"):
            attempted = state.setdefault("tableam_qualification_attempted_cells", [])
            repair = next((dict(postgres_major=p["postgres_major"], distro=p["distro"],
                                architecture=p["architecture"])
                           for p in sorted(inventory["packages"],
                                           key=lambda p: (p["postgres_major"], p["distro"]))
                           if p["build_host"] == host
                           and not package_release_ready(p)
                           and cell_key(p) not in attempted), None)
            # An older archive waiting for an environment-only recheck now
            # needs the proven shared TableAM fixes too, on this same host.
            if validation_only:
                repair = rechecks[host]
                validation_only = False
        # Explicitly diagnosed older failures retain their original host's
        # cache. Consume each queued rebuild once; never retry blindly in a loop.
        rebuilds = state.get("package_rebuilds", {}).get(host, [])
        rebuild = rebuilds[0] if rebuilds and not validation_only and repair is None else None
        cell = (rechecks[host] if validation_only else repair or rebuild) or next(
            (c for c in candidates if cell_key(c) not in occupied), None)
        borrowed_from = None
        if cell is None:
            # Prefer the original cache, but don't strand a diagnosed missing
            # package behind a busy owner while a compatible host is idle.
            active = {cell_key(j["cell"]) for j in state["jobs"].values()
                      if j.get("cell") and (j.get("observed", {}).get("status") == "running"
                          or j.get("observed", {}).get("cleanup_unverified")
                          or (not j.get("launched") and not j.get("error")))}
            available_retries = [("rebuild", owner, candidate)
                                 for owner, queue in state.get("package_rebuilds", {}).items()
                                 for candidate in queue
                                 if owner != host and candidate["architecture"] == architecture
                                 and cell_key(candidate) not in collected | active]
            if state.get("qualify_shared_tableam"):
                available_retries.extend(("qualification", p["build_host"],
                    dict(postgres_major=p["postgres_major"], distro=p["distro"],
                         architecture=p["architecture"])) for p in inventory["packages"]
                    if p["build_host"] != host and p["architecture"] == architecture
                    and not package_release_ready(p)
                    and cell_key(p) not in active
                    and cell_key(p) not in state["tableam_qualification_attempted_cells"])
            if available_retries:
                kind, borrowed_from, cell = min(available_retries, key=lambda item: (
                    item[2]["postgres_major"], item[2]["distro"], item[1]))
                if kind == "qualification":
                    repair = cell
                else:
                    rebuilds = state["package_rebuilds"][borrowed_from]
                    rebuild = cell
        if cell is None:
            continue
        # Match the native workspace preflight before consuming a matrix cell.
        # A full disk is a host condition, not a compilation failure for every
        # remaining distro/major. Recheck automatically after space is reclaimed.
        try:
            available = int(remote(host, "python3 -c " + shlex.quote(
                "import shutil; from pathlib import Path; "
                f"p=Path({(REMOTE + '/build/package-linux')!r}); "
                "p=next(x for x in (p,*p.parents) if x.exists()); "
                "print(shutil.disk_usage(p).free)"), timeout=25))
        except Exception as error:
            event("package_disk_observation_failed", host=host, error=str(error)[:400])
            continue
        disk_attention = state.setdefault("package_disk_attention", {})
        cleaned = state.setdefault("retired_intermediate_cleanup", {})
        if available < 80 * 1024**3 and architecture == "x64" and host not in cleaned:
            try:
                source = ("import json, os, re, shutil, subprocess\nfrom pathlib import Path\n"
                          + inspect.getsource(reclaim_retired_benchmark_intermediates)
                          + f"\nprint(json.dumps(reclaim_retired_benchmark_intermediates({REMOTE!r})))\n")
                cleanup = json.loads(remote(host, "python3 -c " + shlex.quote(source), timeout=55))
                if not cleanup.get("skipped"):
                    cleaned[host] = cleanup
                    available = cleanup["available_bytes"]
                    event("retired_benchmark_intermediates_reclaimed", host=host, **cleanup)
                else:
                    event("retired_intermediate_cleanup_deferred", host=host, **cleanup)
            except Exception as error:
                event("retired_intermediate_cleanup_failed", host=host, error=str(error)[:400])
        if available < 80 * 1024**3:
            if host not in disk_attention:
                event("package_admission_waiting_for_disk", host=host,
                      available_bytes=available, required_bytes=80 * 1024**3)
            disk_attention[host] = dict(available_bytes=available, required_bytes=80 * 1024**3)
            continue
        if disk_attention.pop(host, None) is not None:
            event("package_admission_disk_recovered", host=host, available_bytes=available)
        key = cell_key(cell)
        occupied.add(key)
        state.setdefault("attempted_cells", []).append(key)
        state.setdefault("history", []).append(dict(host=host, previous_job=job))
        state["jobs"][host] = dict(launched=False, cell=cell, mode="package-linux",
                                   targets=[cell["distro"]], validation_only=validation_only)
        if validation_only or repair:
            if repair:
                state["tableam_qualification_attempted_cells"].append(cell_key(cell))
                event("tableam_qualification_reserved", host=host, cell=cell,
                      previous_package_host=borrowed_from or host)
            rechecks.pop(host, None)
        if rebuild:
            rebuilds.remove(rebuild)
            event("diagnosed_package_rebuild_reserved", host=host, cell=cell,
                  previous_queue_host=borrowed_from or host)
        pending.append((host, cell, validation_only))
        event("package_cell_reserved", host=host, cell=cell, validation_only=validation_only)
    write_json_atomic(STATE, state)
    def start(item):
        host, cell, validation_only = item
        try:
            options = {"validate_existing_only": True} if validation_only else {}
            return host, dict(launched=True, observed=launch(host, cell=cell, **options))
        except Exception as error:
            event("launch_failed", host=host, cell=cell, error=str(error))
            return host, dict(launched=False, error=str(error))
    with ThreadPoolExecutor(max_workers=12) as pool:
        for host, update in pool.map(start, pending):
            state["jobs"][host].update(update)
            write_json_atomic(STATE, state)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--status", action="store_true",
                        help="Read-only JSON snapshot, including live PIDs and compiler diagnostics")
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
    if args.status:
        if any((args.launch, args.daemon, args.retry, args.revalidate,
                args.retry_after_failure, args.refresh_after_current, args.retry_failed)):
            parser.error("--status cannot be combined with lifecycle actions")
        print(json.dumps(status_snapshot(), indent=2))
        return
    CONTROL.mkdir(parents=True, exist_ok=True)
    lock = (CONTROL / "controller.lock").open("a")
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    if args.daemon:
        (CONTROL / "controller.pid").write_text(str(os.getpid()) + "\n")
    state = json.loads(STATE.read_text()) if STATE.exists() else {"jobs": {}}
    write_release_manifest()
    if args.retry_after_failure or args.refresh_after_current:
        host = args.retry_after_failure or args.refresh_after_current
        mode = job_mode(host, state["jobs"].get(host, {}))
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
                job_mode(h, state["jobs"].get(h, {}))).get("status")
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
                job_mode(*item) != "website"):
            if not job.get("launched"):
                continue
            try:
                mode = job_mode(host, job)
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
        state["local_package"] = json.loads(Path(fleet.monitor.STATE_PATH).read_text()).get("package-macos", {})
        local_refresh = state.get("local_refresh_after_current_pid")
        if local_refresh and state["local_package"].get("status") != "running":
            state.pop("local_refresh_after_current_pid")
            write_json_atomic(STATE, state)
            if (state["local_package"].get("pid") == local_refresh
                    and not state["local_package"].get("cleanup_unverified")):
                # Explicit one-shot refresh of packaging scripts after an
                # in-flight Python worker drains. Ninja retains compiled work.
                result = rpc([sys.executable, str(ROOT / "build_mcp/server.py")],
                             "start_build", {"mode": "package-macos"})
                event("local_macos_package_refresh", previous_pid=local_refresh, result=result)
        local_manifest = OUTPUT / "packages/local-macos-manifest.json"
        if local_manifest.exists():
            digest = hashlib.sha256(local_manifest.read_bytes()).hexdigest()
            if state.get("local_package_manifest_digest") != digest:
                write_release_manifest()
                state["local_package_manifest_digest"] = digest
                event("local_macos_packages_collected")
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
            failed_cells = [cell_key(row["cell"]) +
                            (" (retry queued)" if row["retry_queued"] else " (needs review)")
                            for row in pending_package_failures(state, inventory)]
            append_progress_log(
                f"Release fleet: {completed_packages}/{inventory['expected_package_count']} installation-validated packages collected; "
                f"{ready_packages}/{inventory['expected_package_count']} also passed current functional acceptance. "
                f"Active workers: {', '.join(active_jobs) or 'none'}. "
                f"Failed workers requiring investigation: {', '.join(failed_jobs) or 'none'}. "
                f"Unresolved package cells, including prior assignments: {', '.join(failed_cells) or 'none'}. "
                f"Local benchmark intentionally retired by user; local macOS package: {state['local_package'].get('status', 'not started')}, "
                f"{state['local_package'].get('step', '')}; "
                "AWS tuple admission remains disabled. See reporting/package_release_fleet/state.json.",
                title="Native release fleet hourly checkpoint")
            state["last_hourly_log"] = time.time()
        STATE.write_text(json.dumps(state, indent=2) + "\n")
        if not args.daemon:
            for host, job in state["jobs"].items():
                observed = job.get("observed", {})
                print(host, observed.get("status", "not_launched"),
                      observed.get("step", job.get("error", "")))
            print("local-package", state["local_package"].get("status"),
                  state["local_package"].get("step"))
            print("local-benchmark-history", state["local_benchmark"].get("status"),
                  state["local_benchmark"].get("bench_e2e_config"))
            return
        time.sleep(60)


if __name__ == "__main__":
    main()
