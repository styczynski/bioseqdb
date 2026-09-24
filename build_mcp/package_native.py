"""MCP-owned, native Linux package collection without signing or publication.

Reuse the retained distro backends, ELF validator, and fresh-container SQL
smoke. The working tree (including uncommitted fixes) is the source of truth;
Git revision is descriptive, never an admission gate. Docker never receives
AWS credentials and this path never invokes the publisher.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time


def run_native_packages(*, repository_root, log_handle, deadline, progress):
    repository = Path(repository_root).resolve()
    selection = os.environ.get("BIOSEQ_PACKAGE_DISTROS", "").strip()
    if not selection:
        raise RuntimeError("Linux package-linux requires BIOSEQ_PACKAGE_DISTROS from infra/package_matrix.json")
    state = repository / "build/package-linux"
    state.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ)
    environment["BIOSEQ_TEMPLATE_FLATTEN_PROFILE"] = "all_alphabets_uint64"
    environment["POETRY_VIRTUALENVS_IN_PROJECT"] = "true"

    def run(argv, cwd=None):
        log_handle.write("+ " + __import__("shlex").join(map(str, argv)) + "\n")
        log_handle.flush()
        with subprocess.Popen(list(map(str, argv)), cwd=cwd, env=environment,
                              stdout=log_handle, stderr=subprocess.STDOUT,
                              start_new_session=True) as child:
            try:
                code = child.wait(timeout=max(1, deadline - time.time()))
            except subprocess.TimeoutExpired:
                os.killpg(child.pid, signal.SIGTERM)
                try:
                    child.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait()
                raise TimeoutError("native package worker reached its deadline")
        if code:
            raise RuntimeError(f"native package command exited {code}; see MCP log")

    progress("preparing locked native packaging Python environment")
    bootstrap = state / "bootstrap-venv"
    if not (bootstrap / "bin/poetry").exists():
        run([sys.executable, "-m", "venv", bootstrap])
        run([bootstrap / "bin/pip", "install", "poetry==1.8.5"])
    packaging = repository / "infra/packaging"
    run([bootstrap / "bin/poetry", "install", "--only", "main", "--no-interaction"], cwd=packaging)
    progress("checking retained packaging contracts before compilation")
    run([bootstrap / "bin/poetry", "run", "python", "-m", "unittest", "discover",
         "-s", str(packaging / "tests")], cwd=packaging)
    for script in ('prepare_build_environment.sh', 'build_and_stage.sh', 'smoke_installed_package.sh'):
        run(['bash', '-n', packaging / 'scripts' / script])
    progress("validating existing native packages and installed SQL; no compilation"
             if environment.get("BIOSEQ_PACKAGE_VALIDATE_EXISTING_ONLY") == "1"
             else "building native distro packages; validating ELF and installed SQL")
    run([bootstrap / "bin/poetry", "run", "python", Path(__file__).resolve(),
         "--collect", repository, selection], cwd=packaging)
    receipt = repository / "bin_artifacts/packages/manifest.json"
    result = json.loads(receipt.read_text())
    return {"receipt_path": str(receipt), "published": False,
            "architecture": result["architecture"], "packages": result["packages"]}


def collect(repository: Path, selection: str) -> int:
    sys.path.insert(0, str(repository / "infra/packaging"))
    from bioseq_packaging.acceptance import AcceptanceService
    from bioseq_packaging.artifacts import sha256_file
    from bioseq_packaging.commands import CommandRunner
    from bioseq_packaging.config import build_cells, native_architecture, parse_distros
    from bioseq_packaging.containers import ContainerRunner
    from bioseq_packaging.model import CellContext, ContainerOutput, PackageFormat, WorkerRequest
    from bioseq_packaging.packagers import deb, rpm, pacman
    from bioseq_packaging.templates import TemplateRenderer
    from bioseq_packaging.workspace import WorkspaceManager
    from bioseq_packaging import postgres_sdk

    matrix = json.loads((repository / "infra/package_matrix.json").read_text())
    requested_majors = os.environ.get("BIOSEQ_PACKAGE_POSTGRES_MAJORS", "").strip()
    majors = tuple(int(value) for value in requested_majors.split(",")) if requested_majors else tuple(matrix["postgresql_majors"])
    if not majors or len(set(majors)) != len(majors) or not set(majors).issubset(matrix["postgresql_majors"]):
        raise RuntimeError("PostgreSQL selection must be a nonempty unique subset of the package matrix")
    architecture = native_architecture()
    distros = parse_distros([selection])
    output = repository / "bin_artifacts/packages"
    output.mkdir(parents=True, exist_ok=True)
    revision = os.environ.get("BIOSEQ_PACKAGE_SOURCE_REVISION", "") or subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=repository,
        capture_output=True, text=True).stdout.strip()
    # Old backends require these publication fields, but no publisher is
    # constructed. Their local-only sentinel is never used as an AWS target.
    request = WorkerRequest(
        architecture=architecture, bucket="local-output-not-published",
        region="local", postgres_majors=majors,
        distro_ids=distros, source_dir=repository,
        build_root=repository / "build/package-linux/native",
        manifest_path=output / "manifest.json", s3_prefix="local-only",
        aws_bin="unused-no-publication", container_output=ContainerOutput.STDOUT,
        fail_fast=False, signing_secret_arn=None, source_revision=revision,
        poetry_lock_sha256=sha256_file(repository / "infra/packaging/poetry.lock"))
    workspace = WorkspaceManager(request)
    runner = CommandRunner()
    containers = ContainerRunner(runner)
    templates = TemplateRenderer()
    acceptance = AcceptanceService(runner, environment=os.environ)
    builders = {PackageFormat.DEB: deb, PackageFormat.RPM: rpm, PackageFormat.PACMAN: pacman}
    manifest = {"schema_revision": 1, "published": False, "signed": False,
                "description": "Native packages validated in fresh matching distro containers; not uploaded or signed.",
                "source": "rsynced local working tree, including uncommitted changes",
                "git_revision_informational_only": revision,
                "template_profile": "all_alphabets_uint64",
                "optimization": "-O3 -DNDEBUG -fno-omit-frame-pointer",
                "architecture": architecture.value, "packages": [], "failures": []}
    validate_existing_only = os.environ.get("BIOSEQ_PACKAGE_VALIDATE_EXISTING_ONLY") == "1"
    for spec in build_cells(request):
        print(f"PACKAGE_CELL_START pg{spec.postgres_major}/{spec.distro.distro_id}/{architecture}", flush=True)
        try:
            if validate_existing_only:
                # Explicit acceptance-only retry after a validator/provisioning
                # fix. Never delete the completed package or re-enter CMake.
                cell = CellContext.from_spec(spec, workspace.cell_paths(spec))
                suffix = {PackageFormat.DEB: ".deb", PackageFormat.RPM: ".rpm",
                          PackageFormat.PACMAN: ".pkg.tar.zst"}[cell.distro.package_format]
                candidates = [path for path in cell.paths.package_root.rglob("*" + suffix)
                              if path.name.startswith((cell.package_name + "_" + cell.package_version,
                                                       cell.package_name + "-" + cell.package_version))
                              and not path.name.endswith(".src.rpm")]
                if len(candidates) != 1:
                    raise RuntimeError(f"acceptance-only retry requires exactly one existing package; found {len(candidates)}")
                artifact = candidates[0]
                print(f"PACKAGE_CELL_REVALIDATE {artifact}", flush=True)
            else:
                cell, artifact = build_cell(spec, workspace, request, builders, containers, templates)
            record, validation = acceptance.validate_built(cell, artifact)
            smoke = acceptance.smoke(cell, artifact)
            channel = matrix.get("postgresql_release_channels", {}).get(str(cell.postgres_major), "unknown")
            destination = output
            if channel != "stable":
                destination /= "experimental"
            destination = destination / str(cell.distro.distro_id) / architecture.value
            destination.mkdir(parents=True, exist_ok=True)
            target = destination / artifact.name
            shutil.copy2(artifact, target)
            if sha256_file(target) != record.sha256:
                raise RuntimeError("collected package checksum differs from validated artifact")
            sdk_details = {}
            if postgres_sdk.required(cell):
                runtime, sdk = postgres_sdk.runtime_package(cell)
                companions = destination / 'sdk' / f'pg{cell.postgres_major}'
                companions.mkdir(parents=True, exist_ok=True)
                runtime_copy = companions / runtime.name
                shutil.copy2(runtime, runtime_copy)
                if sha256_file(runtime_copy) != sdk['sha256']:
                    raise RuntimeError('SDK companion changed during collection')
                metadata_file = companions / 'sdk.json'
                metadata_file.write_text(json.dumps(sdk, indent=2) + '\n')
                setup = companions / 'SDK_SETUP.txt'
                installer = ('sudo pacman -U' if cell.distro.package_format is PackageFormat.PACMAN
                             else 'sudo zypper --no-gpg-checks install')
                requirement = cell.distro.runtime_dependencies(cell.postgres_major)[0]
                setup.write_text(f'''Experimental PostgreSQL SDK for BioseqDB
Required runtime: {requirement}
Source URL: {sdk['source_url']}
Source SHA-256: {sdk['source_sha256']}
Runtime package SHA-256: {sdk['sha256']}
Prefix: {sdk['prefix']}

Download the runtime package and Bioseq package into the same directory, then:
{installer} {runtime_copy.name} {target.name}

These unsigned alpha packages are native to {cell.distro.distro_id}/{architecture.value}.
This is a prerelease/development server, not qualified for production data.
It does not replace the distro server, initialize a database, or start a service.
Use the server binaries in {sdk['prefix']}/bin to create a separate development cluster.
Then configure shared_preload_libraries = 'bioseq', restart that cluster and run
CREATE EXTENSION bioseq_cache; CREATE EXTENSION bioseq;
The query cache remains disabled by default. Other prerelease ABIs are not interchangeable.
''')
                sdk_details = dict(postgres_version=sdk['version'], postgres_runtime_requirement=requirement,
                    postgres_sdk_support_files=[dict(file=str(p.relative_to(output)),
                        bytes=p.stat().st_size, sha256=sha256_file(p))
                        for p in (runtime_copy, metadata_file, setup)])
            manifest["packages"].append({"distro": str(cell.distro.distro_id),
                "postgres_major": cell.postgres_major, "file": str(target.relative_to(output)),
                "postgres_release_channel": channel,
                "git_revision_informational_only": revision,
                "bytes": record.size, "sha256": record.sha256,
                "validation": validation, "installed_sql_smoke": smoke, **sdk_details})
            print(f"PACKAGE_CELL_COMPLETE pg{cell.postgres_major}/{cell.distro.distro_id}/{architecture}", flush=True)
        except Exception as error:
            manifest["failures"].append({"postgres_major": spec.postgres_major, "distro": str(spec.distro.distro_id), "error": str(error)})
            print(f"PACKAGE_CELL_FAILED {spec.distro.distro_id}/{architecture}: {error}", flush=True)
        temporary = request.manifest_path.with_suffix(".tmp")
        temporary.write_text(json.dumps(manifest, indent=2) + "\n")
        temporary.replace(request.manifest_path)
    return int(bool(manifest["failures"]))


def build_cell(spec, workspace, request, builders, containers, templates):
    """Build one cell, retaining the existing bounded scratch/cache policy."""
    # Container package managers create root-owned scratch files.
    # Reclaim only this exact, idle cell on an explicit retry; caches
    # and already collected packages are outside this subtree.
    scratch = workspace.cell_paths(spec).root
    if scratch.exists():
        if scratch.is_symlink() or not scratch.resolve().is_relative_to(request.build_root.resolve()):
            raise RuntimeError("unsafe native package scratch directory")
        subprocess.run(["sudo", "-n", "chown", "-Rh",
            f"{os.getuid()}:{os.getgid()}", "--", str(scratch)], check=True)
        subprocess.run(["chmod", "-R", "u+rwX", str(scratch)], check=True)
    cell = workspace.prepare_cell(spec)
    artifact = builders[cell.distro.package_format].build_package(
        cell, containers=containers, templates=templates, workspace=workspace)
    return cell, artifact


if __name__ == "__main__":
    if len(sys.argv) != 4 or sys.argv[1] != "--collect":
        raise SystemExit("Use bioseq-build start_build(mode=package-linux) on the native worker")
    raise SystemExit(collect(Path(sys.argv[2]).resolve(), sys.argv[3]))
