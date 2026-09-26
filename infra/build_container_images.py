#!/usr/bin/env python3
"""Build and SQL-qualify native runtime images, then publish paired platforms.

The daemon owns a separate queue and only uses idle x64 package hosts plus the
local ARM Docker engine. Packages remain authoritative in bin_artifacts; no
C++ compiler is invoked. Every image worker is admitted through bioseq-build.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shlex
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "infra"))
import build_release_fleet as fleet

CONTROL = ROOT / "reporting/container_images"
ARTIFACTS = ROOT / "bin_artifacts/container_images"
REPOSITORY = "styczynski/bioseqdb"
RELEASE = "0.1.0-alpha.1"
DISTROS = ("ubuntu_26.04", "debian_13", "arch")
WORKERS = ("local", "test3-m4", "test4-m4", "test3-m7a", "test4-m7a", "test3-m7i", "test4-m7i")


def event(name, **fields):
    CONTROL.mkdir(parents=True, exist_ok=True)
    entry = dict(timestamp_utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), event=name, **fields)
    with (CONTROL / "events.jsonl").open("a") as output:
        output.write(json.dumps(entry) + "\n")
    print(json.dumps(entry), flush=True)


def digest(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def command(argv, *, capture=False, input=None, timeout=7200):
    """Bound subprocess lifetime; retain logs without printing credentials."""
    result = subprocess.run(list(map(str, argv)), input=input, text=True,
                            stdout=subprocess.PIPE if capture else None,
                            stderr=subprocess.PIPE if capture else None, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}): {shlex.join(list(map(str,argv[:5])))} "
                           + ((result.stderr or "")[-2000:] if capture else "see worker log"))
    return result.stdout or ""


def local_docker():
    return ["docker", "--context", "orbstack"]


def image_tag(package, architecture=True):
    distro = package["distro"].replace("_", "")
    tag = f"{RELEASE}-pg{package['postgres_major']}-{distro}"
    return f"{REPOSITORY}:{tag}" + ("-" + ("amd64" if package["architecture"] == "x64" else "arm64") if architecture else "")


def image_key(package):
    return fleet.cell_key(package) + "/" + package["sha256"][:16]


def render_context(request, context):
    """Render one architecture using the matching, checksum-verified package."""
    from jinja2 import Environment, FileSystemLoader, StrictUndefined
    package = request["package"]
    major, distro, architecture = package["postgres_major"], package["distro"], package["architecture"]
    if distro not in DISTROS or major not in (17,18,19,20) or architecture not in ("x64","arm64"):
        raise ValueError("Unsupported container matrix cell")
    template_root = ROOT / "infra/container_images"
    environment = Environment(loader=FileSystemLoader(template_root), undefined=StrictUndefined,
                              keep_trailing_newline=True, autoescape=False)
    context.mkdir(parents=True, exist_ok=True)
    packages = context / "packages"
    packages.mkdir(exist_ok=True)
    for item in request["files"]:
        source = Path(item["path"])
        if source.stat().st_size != item["bytes"] or digest(source) != item["sha256"]:
            raise ValueError("Package file does not match accepted SHA-256")
        shutil.copy2(source, packages / source.name)
    binary = next(b["path"] for b in package["validation"]["binaries"] if b["path"].endswith("/bioseq.so"))
    installed = re.search(r"/(?:usr|opt)/.*", binary)
    if not installed:
        raise ValueError("Missing installed extension library path")
    base = ("ubuntu:26.04" if distro == "ubuntu_26.04" else "debian:trixie-slim" if distro == "debian_13"
            else "archlinux:base" if architecture == "x64" else "menci/archlinuxarm:base")
    catalog = (ROOT / "bioseq/backend/pg/table/custom_scan_parts/oid_expression_support/oid_catalog.cppm").read_text()
    functions = list(dict.fromkeys(re.findall(r'resolve\("bioseq\.(\w+)\(anyelement\)"\)', catalog)))
    if not {"cigar", "score", "target_start", "mapq"}.issubset(functions):
        raise ValueError("Alignment output function inventory is incomplete")
    values = dict(base_image=base, entrypoint_image="postgres:17-trixie", release=RELEASE,
        distro=distro, major=major, channel=package.get("postgres_release_channel", "stable"),
        package_sha256=package["sha256"], os_id="ubuntu" if distro.startswith("ubuntu") else "debian",
        os_version="26.04" if distro.startswith("ubuntu") else "13", has_sdk=len(request["files"]) > 1,
        suite=("resolute" if distro.startswith("ubuntu") else "trixie") + ("-pgdg-snapshot" if major >= 20 else "-pgdg"),
        components="main" + (f" {major}" if major >= 19 else ""),
        extension_libdir=str(Path(installed.group()).parent), functions=functions)
    for name, target in (("Dockerfile.j2", "Dockerfile"), ("install.sh.j2", "install.sh"), ("smoke.sql.j2", "smoke.sql")):
        (context / target).write_text(environment.get_template(name).render(**values))
    shutil.copy2(template_root / "init.sql", context / "init.sql")
    return values


def smoke_image(docker, tag, context):
    """Use passwordless postgres over the container socket, never host TCP."""
    name = "bioseq-image-smoke-" + secrets.token_hex(6)
    # Trust is scoped to this ephemeral, network-isolated test container only.
    command(docker + ["run", "-d", "--network", "none", "--name", name,
                      "-e", "POSTGRES_HOST_AUTH_METHOD=trust", tag], capture=True)
    transcript = context / "smoke.log"
    try:
        for attempt in range(90):
            ready = subprocess.run(docker + ["exec", "-u", "postgres", name,
                "psql", "-X", "-U", "postgres", "-d", "postgres", "-Atqc",
                "SELECT count(*)=2 FROM pg_extension WHERE extname IN ('bioseq','bioseq_cache')"],
                capture_output=True, text=True, timeout=15)
            if ready.returncode == 0 and ready.stdout.strip() == "t":
                break
            time.sleep(1)
        else:
            raise RuntimeError("Container did not initialize both extensions")
        sql = (context / "smoke.sql").read_text()
        result = subprocess.run(docker + ["exec", "-i", "-u", "postgres", name,
            "psql", "-X", "-U", "postgres", "-d", "postgres", "-v", "ON_ERROR_STOP=1"],
            input=sql, text=True, capture_output=True, timeout=300)
        transcript.write_text(result.stdout + result.stderr)
        if result.returncode or "BIOSEQ_CONTAINER_SMOKE_OK" not in result.stdout:
            raise RuntimeError("Container SQL smoke failed: " + (result.stdout + result.stderr)[-2500:])
        if "Execution Time:" not in result.stdout or "self_search_valid" not in result.stdout:
            raise RuntimeError("EXPLAIN ANALYZE or self-search evidence missing")
        # Restart the same data volume and re-read the installed extensions.
        command(docker + ["restart", "-t", "30", name], capture=True)
        for attempt in range(60):
            result = subprocess.run(docker + ["exec", "-u", "postgres", name, "psql", "-X",
                "-U", "postgres", "-d", "postgres", "-Atqc",
                "SELECT count(*) FROM pg_extension WHERE extname IN ('bioseq','bioseq_cache')"],
                capture_output=True, text=True, timeout=15)
            if result.returncode == 0 and result.stdout.strip() == "2":
                return dict(passed=True, self_search=True, explain_analyze=True, restart=True,
                            user="postgres", password_used=False, connection="container Unix socket",
                            transcript_sha256=digest(transcript))
            time.sleep(1)
        raise RuntimeError("Container restart smoke failed")
    finally:
        logs = subprocess.run(docker + ["logs", name], capture_output=True, text=True, timeout=30)
        (context / "container.log").write_text(logs.stdout + logs.stderr)
        command(docker + ["rm", "-f", "-v", name], capture=True)


def worker(request_path):
    if os.environ.get("BIOSEQ_CONTAINER_MCP_WORKER") != "1":
        raise RuntimeError("Build and smoke must be invoked through bioseq-build container-images mode")
    request = json.loads(request_path.read_text())
    context = request_path.parent / (request_path.stem + ".context")
    values = render_context(request, context)
    docker = request["docker"]
    platform = "linux/amd64" if request["package"]["architecture"] == "x64" else "linux/arm64"
    actual = command(docker + ["info", "--format", "{{.OSType}}/{{.Architecture}}"], capture=True).strip()
    if actual not in ({"linux/x86_64", "linux/amd64"} if platform.endswith("amd64") else {"linux/aarch64", "linux/arm64"}):
        raise RuntimeError(f"Native Docker engine required: requested {platform}, got {actual}")
    command(["bash", "-n", str(context / "install.sh")])
    tag = image_tag(request["package"])
    command(docker + ["build", "--platform", platform, "--tag", tag, str(context)])
    inspection = json.loads(command(docker + ["image", "inspect", tag], capture=True))[0]
    if inspection["Architecture"] != platform.split("/")[1]:
        raise RuntimeError("Built image architecture mismatch")
    smoke = smoke_image(docker, tag, context)
    archive = request_path.with_suffix(".image.tar")
    command(docker + ["image", "save", "-o", str(archive), tag])
    result = dict(package=request["package"], tag=tag, platform=platform, image_id=inspection["Id"],
        base_image=values["base_image"], smoke=smoke, files=[
            dict(path=str(p), bytes=p.stat().st_size, sha256=digest(p))
            for p in (archive, context / "smoke.log", context / "smoke.sql", context / "container.log", context / "Dockerfile")])
    fleet.write_json_atomic(request_path.with_suffix(".result.json"), result)


def observation(host):
    if host == "local":
        return json.loads(Path(fleet.fleet.monitor.STATE_PATH).read_text()).get("container-images", {})
    return fleet.state_on(host, "container-images")


def eligible_packages():
    manifest = json.loads((ROOT / "bin_artifacts/packages/manifest.json").read_text())
    state = json.loads(fleet.STATE.read_text())
    repair_keys = {fleet.cell_key(c) for name in ("package_rebuilds", "deferred_package_rebuilds")
                   for queue in state.get(name, {}).values() for c in queue}
    for job in state["jobs"].values():
        if job.get("cell") and job.get("observed", {}).get("status") == "running":
            repair_keys.add(fleet.cell_key(job["cell"]))
    return sorted((p for p in manifest["packages"] if p["distro"] in DISTROS
                   and fleet.package_release_ready(p) and fleet.cell_key(p) not in repair_keys),
                  key=lambda p: (DISTROS.index(p["distro"]), p["postgres_major"], p["architecture"]))


def launch(host, package):
    identifier = image_key(package).replace("/", "-")
    requests = CONTROL / "requests"
    requests.mkdir(parents=True, exist_ok=True)
    path = requests / (identifier + ".json")
    files = [package] + [e for e in package.get("postgres_sdk_support_files", []) if ".pkg.tar." in e["file"]]
    entries = []
    for item in files:
        artifact = ROOT / "bin_artifacts/packages" / item["file"]
        if artifact.stat().st_size != item["bytes"] or digest(artifact) != item["sha256"]:
            raise RuntimeError("Authoritative package checksum mismatch")
        entries.append(dict(path=str(artifact), bytes=item["bytes"], sha256=item["sha256"]))
    request = dict(package=package, files=entries, docker=local_docker() if host == "local" else ["docker"])
    if host == "local":
        fleet.write_json_atomic(path, request)
        env = dict(os.environ, BIOSEQ_CONTAINER_REQUEST=str(path))
        answer = fleet.rpc(["env", "BIOSEQ_CONTAINER_REQUEST=" + str(path), sys.executable,
                            str(ROOT / "build_mcp/server.py")], "start_build", {"mode": "container-images"})
    else:
        package_state = fleet.state_on(host, "package-linux")
        if package_state.get("status") == "running":
            raise RuntimeError("Package compiler owns this host; image launch deferred")
        revision = fleet.sync_source(host)
        remote_directory = fleet.REMOTE + "/reporting/container_images/requests"
        fleet.remote(host, "mkdir -p " + remote_directory + "/inputs")
        ssh = fleet.fleet.ssh(host)
        for entry in entries:
            target = remote_directory + "/inputs/" + entry["sha256"] + "-" + Path(entry["path"]).name
            command(["rsync", "-a", "-e", shlex.join(ssh[:-1]), entry["path"], ssh[-1] + ":" + target])
            entry["path"] = target
        fleet.write_json_atomic(path, request)
        remote_path = remote_directory + "/" + path.name
        command(["rsync", "-a", "-e", shlex.join(ssh[:-1]), str(path), ssh[-1] + ":" + remote_path])
        answer = fleet.rpc(ssh + ["cd " + fleet.REMOTE + " && env BIOSEQ_CONTAINER_REQUEST="
            + shlex.quote(remote_path) + " BIOSEQ_PACKAGE_SOURCE_REVISION=" + revision
            + " python3 build_mcp/server.py"], "start_build", {"mode": "container-images"})
    event("image_worker_started", host=host, cell=fleet.cell_key(package), response=answer)
    observed = observation(host)
    if observed.get("status") != "running":
        raise RuntimeError("MCP did not admit image worker")
    return dict(host=host, package=package, request=str(path), observed=observed, collected=False)


def collect_image(job):
    """Durably collect a SQL-tested image before returning its worker to the queue."""
    host = job["host"]
    request = Path(job["request"])
    result_path = request.with_suffix(".result.json")
    destination = ARTIFACTS / request.stem
    destination.mkdir(parents=True, exist_ok=True)
    if host != "local":
        ssh = fleet.fleet.ssh(host)
        remote_result = fleet.REMOTE + "/reporting/container_images/requests/" + result_path.name
        command(["rsync", "-a", "-e", shlex.join(ssh[:-1]), ssh[-1] + ":" + remote_result, str(result_path)])
    result = json.loads(result_path.read_text())
    if image_key(result["package"]) != image_key(job["package"]) or not result.get("smoke",{}).get("passed"):
        raise RuntimeError("Worker result does not match the reserved package or passed smoke")
    for item in result["files"]:
        target = destination / Path(item["path"]).name
        retained = target.is_file() and target.stat().st_size == item["bytes"] and digest(target) == item["sha256"]
        if not retained:
            if host == "local":
                shutil.copy2(item["path"], target)
            else:
                command(["rsync", "-a", "-e", shlex.join(ssh[:-1]), ssh[-1] + ":" + item["path"], str(target)])
        if target.stat().st_size != item["bytes"] or digest(target) != item["sha256"]:
            raise RuntimeError("Image artifact transfer checksum mismatch")
        item["path"] = str(target)
    result["published"] = False
    fleet.write_json_atomic(destination / "result.json", result)
    return result


def publish_collected(result):
    """Publish local artifacts independently of build-worker admission."""
    archive = next(Path(i["path"]) for i in result["files"] if i["path"].endswith(".image.tar"))
    destination = archive.parent
    retained_image = subprocess.run(local_docker() + ["image", "inspect", result["tag"], "--format", "{{.Id}}"],
                                    capture_output=True,text=True,timeout=30)
    if retained_image.returncode or retained_image.stdout.strip() != result["image_id"]:
        command(local_docker() + ["image", "load", "-i", str(archive)])
    command(local_docker() + ["push", result["tag"]])
    descriptor = json.loads(command(local_docker() + ["buildx", "imagetools", "inspect",
        "--format", "{{json .Manifest}}", result["tag"]], capture=True))
    result["registry_digest"] = descriptor["digest"]
    registry_text = command(local_docker() + ["buildx", "imagetools", "inspect", "--raw", result["tag"]], capture=True)
    registry = json.loads(registry_text)
    if "manifests" in registry:
        expected_arch = result["platform"].split("/")[1]
        matches = [m for m in registry["manifests"] if m.get("platform",{}).get("os") == "linux"
                   and m["platform"].get("architecture") == expected_arch]
        if len(matches) != 1:
            raise RuntimeError("Pushed image lacks its one expected native platform")
        result["platform_digest"] = matches[0]["digest"]
        registry_text = command(local_docker() + ["buildx", "imagetools", "inspect", "--raw",
            REPOSITORY + "@" + result["platform_digest"]], capture=True)
        registry = json.loads(registry_text)
    else:
        result["platform_digest"] = descriptor["digest"]
    # Classic Docker reports the config digest as .Id; the containerd store
    # reports the OCI index/manifest digest. Both identify immutable content.
    if result["image_id"] not in {result["registry_digest"], result["platform_digest"],
                                   registry.get("config",{}).get("digest")}:
        raise RuntimeError("Registry content differs from the SQL-tested image identity")
    result["published"] = True
    fleet.write_json_atomic(destination / "result.json", result)
    event("image_published", tag=result["tag"], image_id=result["image_id"])
    return result


def publish_pairs(state):
    groups = {}
    for result in state.get("results", {}).values():
        if result.get("published"):
            groups.setdefault(image_tag(result["package"], False), {})[result["platform"]] = result
    for tag, variants in groups.items():
        if set(variants) != {"linux/amd64", "linux/arm64"}:
            continue
        identities = {p: r["image_id"] for p,r in variants.items()}
        if state.setdefault("multi_platform", {}).get(tag) == identities:
            continue
        command(local_docker() + ["buildx", "imagetools", "create", "--tag", tag,
                                 *[REPOSITORY + "@" + r["platform_digest"] for r in variants.values()]])
        raw = json.loads(command(local_docker() + ["buildx", "imagetools", "inspect", "--raw", tag], capture=True))
        platforms = {m.get("platform",{}).get("os", "") + "/" + m.get("platform",{}).get("architecture", "") for m in raw["manifests"]}
        if platforms != set(variants):
            raise RuntimeError("Published multi-platform manifest lacks an expected architecture")
        state["multi_platform"][tag] = identities
        event("multi_platform_tag_published", tag=tag, platforms=sorted(platforms))


def daemon():
    CONTROL.mkdir(parents=True, exist_ok=True)
    lock = (CONTROL / "controller.lock").open("a")
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    (CONTROL / "controller.pid").write_text(str(os.getpid()) + "\n")
    path = CONTROL / "state.json"
    state = json.loads(path.read_text()) if path.exists() else dict(jobs={}, results={}, failed={})
    while True:
        for host, job in list(state["jobs"].items()):
            try:
                observed = observation(host)
                job["observed"] = observed
                if observed.get("status") == "completed" and not job.get("collected"):
                    result = collect_image(job)
                    state["results"][image_key(job["package"])] = result
                    job["collected"] = True
                    fleet.write_json_atomic(path,state)
                elif observed.get("status") in ("failed", "killed_timeout", "aborted"):
                    state["failed"][image_key(job["package"])] = dict(host=host, observed=observed)
                    fleet.write_json_atomic(path,state)
            except Exception as error:
                event("image_monitor_error", host=host, error=str(error)[:1800])
        occupied = set(state["results"]) | set(state["failed"])
        occupied.update(image_key(j["package"]) for j in state["jobs"].values())
        pending = []
        for host in WORKERS:
            if host != "local" and fleet.retirement_policy(host):
                continue
            job = state["jobs"].get(host)
            if job and not job.get("collected") and job["observed"].get("status") not in ("failed", "killed_timeout", "aborted"):
                continue
            architecture = "arm64" if host == "local" else "x64"
            package = next((p for p in eligible_packages() if p["architecture"] == architecture and image_key(p) not in occupied), None)
            if package:
                occupied.add(image_key(package));pending.append((host,package))
        def start(item):
            host, package = item
            try:return host, launch(host, package), None
            except Exception as error:return host, None, str(error)
        with ThreadPoolExecutor(max_workers=7) as pool:
            for host, job, error in pool.map(start,pending):
                if error:event("image_launch_error",host=host,error=error[:1800])
                else:
                    state["jobs"][host]=job
                    fleet.write_json_atomic(path,state)
        # Refill builders before uploads; a slow registry must not hold all
        # native workers idle. Bound each cycle to two publication attempts.
        unpublished = [r for r in state["results"].values() if not r.get("published")]
        for result in unpublished[:2]:
            try:
                publish_collected(result)
                fleet.write_json_atomic(path,state)
            except Exception as error:
                event("image_publish_error",tag=result["tag"],error=str(error)[:1800])
        try:publish_pairs(state)
        except Exception as error:event("image_manifest_error",error=str(error)[:1800])
        if time.time() - state.get("last_hourly_log",0) >= 3600:
            from e2e_tuple_progress import append_progress_log
            published={fleet.cell_key(r["package"]) for r in state["results"].values() if r.get("published")}
            active=[h for h,j in state["jobs"].items() if j.get("observed",{}).get("status")=="running"]
            append_progress_log(f"Container images: {len(published)}/24 native platform images published, "
                f"{len(state.get('multi_platform',{}))}/12 paired-platform tags. "
                f"Active image workers: {', '.join(active) or 'none'}. "
                f"Failed image requests: {len(state['failed'])}. All accepted images passed passwordless "
                "postgres Unix-socket psql self-search, all alignment output helpers, EXPLAIN ANALYZE and restart. "
                "Native package compilation continues independently; no benchmark restarted. See DOC0443.",
                title="Runtime container image hourly checkpoint")
            state["last_hourly_log"]=time.time()
        fleet.write_json_atomic(path,state)
        time.sleep(60)


def status_summary():
    """Print concise persisted counts; do not mistake stored state for live PIDs."""
    path=CONTROL / "state.json"
    state=json.loads(path.read_text()) if path.exists() else dict(jobs={},results={},failed={})
    packages=json.loads((ROOT / "bin_artifacts/packages/manifest.json").read_text())
    return dict(native_packages_collected=packages["validated_package_count"],native_packages_expected=44,
        container_images_collected=len({fleet.cell_key(r["package"]) for r in state["results"].values()}),
        container_images_published=len({fleet.cell_key(r["package"]) for r in state["results"].values() if r.get("published")}),
        container_images_expected=24,multi_platform_tags_published=len(state.get("multi_platform",{})),
        multi_platform_tags_expected=12,image_failures=state["failed"],
        workers_last_observed={h:dict(cell=fleet.cell_key(j["package"]),status=j["observed"].get("status"),
                                    collected=j.get("collected")) for h,j in state["jobs"].items()})


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("worker", "daemon", "status"))
    parser.add_argument("--request", type=Path)
    args = parser.parse_args()
    if args.action == "worker":worker(args.request)
    elif args.action == "daemon":daemon()
    else:print(json.dumps(status_summary(),indent=2))
