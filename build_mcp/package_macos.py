"""MCP-owned native macOS packages; no benchmark dispatch or publication.

Retain an incremental build per PostgreSQL major. Ship a Homebrew-compatible
payload, private non-system dylibs, and explicit unsigned alpha documentation.
Acceptance uses bytes extracted from the final .pkg, never the build-tree DSO.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile
import time

from package_linux import _run, _write_json_atomic

MODE = 'package-macos'
REPORTING_SUBDIR = 'bioseq_package_macos'
VERSION = '0.1.0-alpha.1'


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def dependencies(path, run):
    return [line.strip().split(' (compatibility version', 1)[0]
            for line in run(['/usr/bin/otool', '-L', path], capture=True).splitlines()[1:]]


def bundle_dylibs(extension_files, runtime, pg_bindir, run, extra_libraries=()):
    """Copy and relink the complete non-system Mach-O dependency closure."""
    runtime.mkdir(parents=True, exist_ok=True)
    pending = [(path, path) for path in extension_files]
    by_name = {}
    for original, name in extra_libraries:
        original = Path(original).resolve()
        staged = runtime / name
        shutil.copy2(original, staged)
        staged.chmod(0o755)
        by_name[name] = sha256(original)
        pending.append((original, staged))
    inspected = set()
    receipts = []
    while pending:
        original, staged = pending.pop()
        if staged in inspected:
            continue
        inspected.add(staged)
        load_commands = run(['/usr/bin/otool', '-l', original], capture=True)
        rpaths = re.findall(r'cmd LC_RPATH\n.*?\n\s*path (.*?) \(offset',
                            load_commands, re.DOTALL)
        def expand(path):
            return Path(path.replace('@loader_path', str(original.parent))
                        .replace('@executable_path', str(pg_bindir)))
        for dep in dependencies(original, run):
            if dep.startswith(('/usr/lib/', '/System/Library/')):
                continue
            if dep.startswith('@rpath/'):
                candidates = [expand(p) / dep.removeprefix('@rpath/') for p in rpaths]
                candidates += [Path('/opt/homebrew/opt/llvm@22/lib/c++') / Path(dep).name,
                               Path('/opt/homebrew/lib') / Path(dep).name]
            else:
                candidates = [expand(dep)]
            resolved = next((p.resolve() for p in candidates if p.is_file()), None)
            if resolved is None:
                raise RuntimeError(f'Unresolved Mach-O dependency {dep} in {original}')
            if resolved == original.resolve():  # LC_ID_DYLIB appears in otool -L too.
                continue
            name = Path(dep).name
            digest = sha256(resolved)
            if name in by_name and by_name[name] != digest:
                raise RuntimeError(f'Different runtime libraries share basename {name}')
            destination = runtime / name
            if name not in by_name:
                by_name[name] = digest
                shutil.copy2(resolved, destination)
                destination.chmod(0o755)
                pending.append((resolved, destination))
            relative = ('bioseq_runtime/' if staged.parent != runtime else '') + name
            run(['/usr/bin/install_name_tool', '-change', dep,
                 '@loader_path/' + relative, staged])
        if staged.parent == runtime:
            run(['/usr/bin/install_name_tool', '-id', '@loader_path/' + staged.name, staged])
        # Build-machine RPATHs are unnecessary after the closure is made relative.
        for rpath in dict.fromkeys(rpaths):
            run(['/usr/bin/install_name_tool', '-delete_rpath', rpath, staged])
        run(['/usr/bin/codesign', '--force', '--sign', '-', staged])
        run(['/usr/bin/codesign', '--verify', '--strict', staged])
        architecture = run(['/usr/bin/lipo', '-archs', staged], capture=True).strip()
        if 'arm64' not in architecture.split():
            raise RuntimeError(f'Runtime lacks native arm64: {staged}')
        final_deps = dependencies(staged, run)
        if any(not d.startswith(('/usr/lib/', '/System/Library/', '@loader_path/'))
               for d in final_deps):
            raise RuntimeError(f'Nonportable dependency remains in {staged}: {final_deps}')
        receipts.append(dict(file=staged.name, architecture=architecture,
                             dependencies=final_deps, sha256=sha256(staged)))
    return receipts


def private_postgres(pg_prefix, libdir, sharedir, run_root, run):
    """Keep Homebrew's Cellar/bin vs global lib/share relative layout intact."""
    private = run_root / 'postgres-runtime'
    prefix = private / pg_prefix.relative_to('/opt/homebrew')
    prefix.parent.mkdir(parents=True, exist_ok=True)
    run(['/usr/bin/rsync', '-aL', str(pg_prefix) + '/', str(prefix) + '/'])
    pg_config = prefix / 'bin/pg_config'
    private_lib = Path(run([pg_config, '--pkglibdir'], capture=True).strip())
    private_share = Path(run([pg_config, '--sharedir'], capture=True).strip())
    if not all(p.is_relative_to(private) for p in (private_lib, private_share)):
        raise RuntimeError('PostgreSQL runtime cannot relocate into private acceptance prefix')
    for original, destination in ((libdir, private_lib), (sharedir, private_share)):
        destination.mkdir(parents=True, exist_ok=True)
        run(['/usr/bin/rsync', '-aL', '--exclude=bioseq*',
             str(original) + '/', str(destination) + '/'])
    return prefix / 'bin', private_lib, private_share


def smoke_payload(repository, extracted, payload, pg_prefix, libdir, sharedir, run_root, run):
    """Install extracted payload into a private copy of the matching PG prefix.

    Apple Installer's system/root authorization is not used or claimed here.
    The archive layout and both real extension load paths are exercised instead.
    """
    pg, private_lib, private_share = private_postgres(
        pg_prefix, libdir, sharedir, run_root, run)
    pg_config = pg / 'pg_config'
    for source_dir, destination in ((libdir, private_lib), (sharedir, private_share)):
        source = extracted / source_dir.relative_to('/')
        if source.is_dir():
            run(['/usr/bin/rsync', '-a', str(source) + '/', str(destination) + '/'])
    data, sock = run_root / 'pgdata', run_root / 'socket'
    sock.mkdir(mode=0o700)
    run([pg / 'initdb', '-D', data, '--no-locale', '--encoding=UTF8', '--auth=trust'])
    conf = data / 'postgresql.conf'
    with conf.open('a') as handle:
        handle.write("\nlisten_addresses = ''\nport = 55437\n"
                     f"unix_socket_directories = '{sock}'\n"
                     "shared_preload_libraries = 'bioseq'\n")
    # Reuse the exact retained Linux SQL/default/cache checks. Only the host
    # setup and runuser wrapper differ; no new numerical tests are introduced.
    original = (repository / 'infra/packaging/scripts/smoke_installed_package.sh').read_text()
    checks = original[original.index("run_pg \"${psql_command[@]}\" -c 'CREATE EXTENSION bioseq_cache;'"):]
    script = run_root / 'smoke.sh'
    script.write_text('''#!/bin/bash
set -euo pipefail
exec 3>&1
die() { echo "macOS package smoke: $*" >&2; exit 1; }
run_pg() { "$@"; }
pg_bindir="$1"
data_dir="$2"
socket_dir="$3"
log_file="$4"
postgres_major="$5"
requested_arch=arm64
distro=macos
pg_ctl="${pg_bindir}/pg_ctl"
psql_command=("${pg_bindir}/psql" -X -v ON_ERROR_STOP=1 -h "${socket_dir}" -p 55437 -d postgres)
started=false
cleanup() { if [[ "$started" == true ]]; then "$pg_ctl" -D "$data_dir" -m immediate -w stop >/dev/null 2>&1 || true; fi; }
trap cleanup EXIT
"$pg_ctl" -D "$data_dir" -l "$log_file" -w start
started=true
''' + checks)
    major = re.search(r'PostgreSQL (\d+)', run([pg_config, '--version'], capture=True)).group(1)
    output = run(['/bin/bash', script, pg, data, sock, run_root / 'postgres.log', major], capture=True)
    proof = next((json.loads(line) for line in reversed(output.splitlines())
                  if line.startswith('{"passed":true,')), None)
    if proof is None:
        raise RuntimeError('Private installed-payload SQL checks produced no success result')
    proof.update(installation_method='pkgutil-extracted payload in private PostgreSQL prefix',
                 apple_installer_system_install_tested=False)
    return proof


def build_one(repository, major, state, output, run, progress):
    pg_opt = Path(f'/opt/homebrew/opt/postgresql@{major}')
    pg_config = pg_opt / 'bin/pg_config'
    sdk_formula_source = None
    formula = {19: 'petere/postgresql/postgresql@19',
               20: 'bioseqdb/package-sdks/postgresql@20'}.get(major, f'postgresql@{major}')
    if major == 20:
        # A local-only tap with an immutable upstream archive pin. Installing a
        # PG19 formula's moving HEAD would collide with the accepted beta SDK.
        sdk_formula_source = repository / 'infra/packaging/homebrew/postgresql@20.rb'
        run(['/usr/bin/ruby', '-c', sdk_formula_source])
        tap = Path('/opt/homebrew/Library/Taps/bioseqdb/homebrew-package-sdks')
        if not tap.is_dir():
            run(['/opt/homebrew/bin/brew', 'tap-new', 'bioseqdb/package-sdks'])
        formulas = tap / 'Formula'
        formulas.mkdir(parents=True, exist_ok=True)
        shutil.copy2(sdk_formula_source, formulas / sdk_formula_source.name)
    if not pg_config.is_file():
        progress(f'installing exact Homebrew PostgreSQL {major} SDK/runtime')
        # The versioned upstream-maintainer tap supplies PG19 beta alongside
        # stable core formulae. Never install its HEAD as a different major.
        run(['/opt/homebrew/bin/brew', 'install', formula])
    version = run([pg_config, '--version'], capture=True).strip()
    if re.search(rf'^PostgreSQL {major}(?:\D|$)', version) is None:
        raise RuntimeError(f'Wrong PostgreSQL SDK: {version}')
    pg_prefix = pg_opt.resolve()
    # Core formulae use global versioned lib/share directories. A third-party
    # keg may keep these inside its Cellar prefix: retain that real prefix, not
    # an opt symlink which Installer could replace with a payload directory.
    def stable_path(flag):
        return Path(run([pg_config, flag], capture=True).strip())
    libdir, sharedir = stable_path('--pkglibdir'), stable_path('--sharedir')
    if not all(p.is_relative_to('/opt/homebrew') for p in (libdir, sharedir)):
        raise RuntimeError('Unexpected Homebrew extension directories')
    matching_keg = any(p.is_relative_to(pg_prefix) for p in (libdir, sharedir))
    required_config = pg_prefix / 'bin/pg_config' if matching_keg else pg_config
    runtime_requirement = (f'{version}; exact keg {pg_prefix}' if matching_keg
                           else f'Homebrew PostgreSQL {major}')
    build = state / f'pg{major}'
    progress(f'configuring optimized native arm64 PostgreSQL {major} production modules')
    flags = '-O3 -DNDEBUG -fno-omit-frame-pointer'
    run(['cmake', '--preset', 'bench-opt', '-S', repository, '-B', build,
         '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_FLAGS=', '-DCMAKE_CXX_FLAGS=',
         '-DCMAKE_C_FLAGS_RELEASE=' + flags, '-DCMAKE_CXX_FLAGS_RELEASE=' + flags,
         '-DCMAKE_MODULE_LINKER_FLAGS=-Wl,-headerpad_max_install_names',
         '-DBIOSEQ_PACKAGE_BUILD=ON', '-DBIOSEQ_BENCHMARK_E2E_INTERVALS=OFF',
         '-DBIOSEQ_TEMPLATE_FLATTEN_PROFILE=all_alphabets_uint64',
         '-DBIOSEQ_BUILD_TESTS=OFF', '-DBIOSEQ_TEST_INDEXES=OFF',
         '-DBIOSEQ_BUILD_CORE=ON', '-DBIOSEQ_BUILD_CACHE=ON',
         '-DBIOSEQ_BUILD_PAPERS=OFF', '-DBIOSEQ_BUILD_WEBSITE=OFF',
         '-DBIOSEQ_BUILD_EXTERNAL_API_CLIENTS=OFF', '-DBIOSEQ_CACHE_BUILD_TESTS=OFF',
         '-DBIOSEQ_CACHE_TESTING=OFF', '-DPostgreSQL_CONFIG=' + str(pg_config)])
    progress(f'compiling native arm64 PostgreSQL {major} .pkg payload')
    run(['cmake', '--build', build, '--target', 'postgresql_bioseq', 'postgresql_bioseq_cache',
         '--parallel', os.environ.get('BIOSEQ_MACOS_BUILD_JOBS', '4')])
    run_root = Path(tempfile.mkdtemp(prefix=f'pg{major}-', dir=state))
    payload = run_root / 'payload'
    payload.mkdir()
    # Stage exact generated SQL/control files using the existing CMake component.
    run(['cmake', '--install', build, '--component', 'bioseq'], destdir=payload)
    targets = dict(line.split('\t') for line in
                   (build / 'bioseq-package-target-artifacts.tsv').read_text().splitlines())
    staged_lib = payload / libdir.relative_to('/')
    staged_share = payload / sharedir.relative_to('/') / 'extension'
    staged_lib.mkdir(parents=True, exist_ok=True)
    staged_share.mkdir(parents=True, exist_ok=True)
    for name in ('bioseq', 'bioseq_cache'):
        # Normalize any versioned Cellar DESTDIR paths to stable opt/lib paths.
        for suffix in ('.control', '--1.0.sql'):
            sources = list(payload.rglob(name + suffix))
            if len(sources) != 1:
                raise RuntimeError(f'Expected exactly one staged {name + suffix}')
            target = staged_share / (name + suffix)
            if sources[0] != target:
                shutil.move(str(sources[0]), target)
        target = staged_lib / (name + '.so')
        for old in payload.rglob(name + '.so'):
            if old != target:
                old.unlink()
        shutil.copy2(targets['postgresql_' + name], target)
    cache = (build / 'CMakeCache.txt').read_text()
    loader = re.search(r'^Vulkan_LIBRARY:FILEPATH=(.+)$', cache, re.MULTILINE)
    if loader is None:
        raise RuntimeError('Configured Vulkan loader missing from package build cache')
    archive_helpers = list(payload.rglob('bioseq_gpu_archive_builder'))
    if len(archive_helpers) != 1 or archive_helpers[0].is_symlink():
        raise RuntimeError('Native Mac package must contain its installed Metal archive helper')
    archive_helper = staged_lib / 'bioseq_gpu_archive_builder'
    if archive_helpers[0] != archive_helper:
        shutil.move(str(archive_helpers[0]), archive_helper)
    archive_helper.chmod(0o755)
    mach_o = bundle_dylibs([staged_lib / 'bioseq.so', staged_lib / 'bioseq_cache.so', archive_helper],
                          staged_lib / 'bioseq_runtime', pg_opt / 'bin', run,
                          [(loader.group(1), 'libvulkan.1.dylib')])
    scripts = run_root / 'scripts'
    scripts.mkdir()
    preinstall = scripts / 'preinstall'
    preinstall.write_text(f'''#!/bin/sh
set -eu
pg_config="{required_config}"
if [ ! -x "$pg_config" ] || ! "$pg_config" --version | /usr/bin/grep -Eq '^PostgreSQL {major}([^0-9]|$)'; then
  echo 'Required runtime: {runtime_requirement}. Install {formula} before this package.' >&2
  exit 1
fi
if [ "$(/usr/bin/uname -m)" != arm64 ]; then
  echo 'This package requires Apple Silicon (arm64).' >&2
  exit 1
fi
''')
    preinstall.chmod(0o755)
    readme = staged_share / f'bioseq-pg{major}-README.txt'
    readme.write_text(f'''BioseqDB {VERSION} — PostgreSQL {major}, native macOS arm64
Unsigned and unnotarized alpha. Required runtime: {runtime_requirement}.
SDK installation: brew install {formula}
Required pg_config: {required_config}
Prerelease PostgreSQL SDKs may change ABI; this package does not prove compatibility with another beta.
Built with Clang, -O3 -DNDEBUG, all_alphabets_uint64; E2E instrumentation OFF.
Quit/restart PostgreSQL when installing or upgrading loaded extension files.
Add bioseq to shared_preload_libraries, restart PostgreSQL, then run:
CREATE EXTENSION bioseq_cache; CREATE EXTENSION bioseq;
bioseq_cache loads automatically and its query cache is disabled by default.
Default alignment is bounded ntHash SIMD + SIMD-DP + block aligner.
The installer does not edit postgresql.conf, start services, or delete databases.
Non-system Mach-O dependencies are bundled beside the extensions.
Vulkan GPU use additionally requires a working MoltenVK ICD (brew install molten-vk).
First-time Metal shader compilation requires Apple's xcrun/Metal toolchain.
This package is not qualified for Intel Macs or non-Homebrew PostgreSQL distributions.
''')
    if sdk_formula_source is not None:
        with readme.open('a') as handle:
            handle.write('The pinned PG20 formula and SDK_SETUP.txt accompany this installer. '
                         'Set up that local tap before running brew install; this is not a public GitHub tap.\n')
    component = run_root / 'component.pkg'
    run(['/usr/bin/pkgbuild', '--root', payload, '--install-location', '/',
         '--identifier', f'com.bioseqdb.postgresql{major}.arm64', '--version', '0.1.0',
         '--scripts', scripts, '--ownership', 'recommended', component])
    directory = output / 'macos' / 'arm64' / f'pg{major}'
    directory.mkdir(parents=True, exist_ok=True)
    final = directory / f'bioseqdb-{VERSION}-pg{major}-macos-arm64.pkg'
    candidate = run_root / final.name
    run(['/usr/bin/productbuild', '--package', component, candidate])
    expanded = run_root / 'expanded'
    run(['/usr/sbin/pkgutil', '--expand-full', candidate, expanded])
    extracted = list(expanded.rglob('Payload'))
    extracted = [p for p in extracted if p.is_dir()]
    if len(extracted) != 1:
        raise RuntimeError('Expected one extracted installer component payload')
    for path in payload.rglob('*'):
        if path.is_file():
            copy = extracted[0] / path.relative_to(payload)
            if not copy.is_file() or sha256(copy) != sha256(path):
                raise RuntimeError(f'Installer payload changed: {path.relative_to(payload)}')
    progress(f'validating final macOS PostgreSQL {major} package in private SQL runtime')
    smoke = smoke_payload(repository, extracted[0], payload, pg_prefix,
                          libdir, sharedir, run_root, run)
    support_files = []
    if sdk_formula_source is not None:
        sdk_copy = directory / sdk_formula_source.name
        shutil.copy2(sdk_formula_source, sdk_copy)
        setup = directory / 'SDK_SETUP.txt'
        setup.write_text(f'''Experimental PostgreSQL {major} SDK for BioseqDB {VERSION}
Not a stable PostgreSQL release. Required runtime: {runtime_requirement}
Use only for development; another snapshot is not asserted ABI-compatible.

Download postgresql@20.rb from the same release directory as this file.
Check its SHA-256: {sha256(sdk_copy)}
Inspect the formula, then run in the directory containing that downloaded file:

brew tap-new bioseqdb/package-sdks
cp postgresql@20.rb "$(brew --repository bioseqdb/package-sdks)/Formula/postgresql@20.rb"
brew install bioseqdb/package-sdks/postgresql@20

If that local tap already exists, omit tap-new; retain any other formulae in it.
The formula downloads an immutable upstream commit and verifies its archive SHA-256.
It is keg-only: no service, database cluster, forced linking, or PG19 replacement.
After the SDK is installed, install the accompanying BioseqDB .pkg.
''')
        support_files = [dict(file=str(path.relative_to(output)), bytes=path.stat().st_size,
                              sha256=sha256(path)) for path in (sdk_copy, setup)]
    os.replace(candidate, final)
    return dict(file=str(final.relative_to(output)), bytes=final.stat().st_size,
        sha256=sha256(final), distro='macos', postgres_major=major,
        postgres_version=version, postgres_runtime_requirement=runtime_requirement,
        postgres_sdk_formula=formula,
        postgres_sdk_support_files=support_files,
        validation=dict(passed=True, format='pkg', package_version=VERSION,
                        architecture='arm64', mach_o=mach_o, notarized=False,
                        code_signature='ad-hoc Mach-O only; installer unsigned'),
        installed_sql_smoke=smoke)


def run_package_macos(*, repository_root, log_handle, deadline, progress):
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        raise RuntimeError('Native macOS Apple Silicon host is required')
    repository = Path(repository_root)
    state = repository / 'build' / MODE
    state.mkdir(parents=True, exist_ok=True)
    output = repository / 'bin_artifacts/packages'
    output.mkdir(parents=True, exist_ok=True)
    manifest_path = output / 'local-macos-manifest.json'
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else dict(
        architecture='arm64', template_profile='all_alphabets_uint64',
        optimization='-O3 -DNDEBUG -fno-omit-frame-pointer', signed=False,
        published=False, packages=[], unavailable=[])
    environment = dict(os.environ, BIOSEQ_TEMPLATE_FLATTEN_PROFILE='all_alphabets_uint64',
                       CMAKE_NAMED_ENV_TYPE='H', HOMEBREW_NO_AUTO_UPDATE='1')
    environment.setdefault('VCPKG_ROOT', str(Path.home() / 'vcpkg'))
    environment['PATH'] = '/opt/homebrew/bin:/opt/homebrew/opt/llvm@22/bin:' + environment.get('PATH', '')
    for key in ('CC', 'CXX', 'CFLAGS', 'CXXFLAGS', 'LDFLAGS', 'MAKEFLAGS'):
        environment.pop(key, None)
    def run(argv, capture=False, destdir=None):
        env = environment if destdir is None else dict(environment, DESTDIR=str(destdir))
        return _run(list(map(str, argv)), log=log_handle, deadline=deadline,
                    cwd=repository, environment=env, capture=capture)
    majors = [int(v) for v in os.environ.get('BIOSEQ_PACKAGE_POSTGRES_MAJORS', '17,18,19,20').split(',')]
    if not majors or not set(majors).issubset({17, 18, 19, 20}):
        raise RuntimeError('macOS PostgreSQL selection must be within the release matrix')
    for major in sorted(set(majors)):
        if any(p['postgres_major'] == major and (output / p['file']).is_file()
               and sha256(output / p['file']) == p['sha256'] for p in manifest['packages']):
            progress(f'reusing accepted macOS PostgreSQL {major} package')
            continue
        if shutil.disk_usage(state).free < 20 * 1024**3:
            raise RuntimeError('macOS package build requires at least 20 GiB free; retained measurements are untouched')
        try:
            package = build_one(repository, major, state, output, run, progress)
        except Exception as error:
            manifest['unavailable'] = [r for r in manifest['unavailable'] if r['postgres_major'] != major]
            manifest['unavailable'].append(dict(postgres_major=major, reason=str(error)))
            _write_json_atomic(manifest_path, manifest)
            # Compile or acceptance failures need diagnosis, not silent skipping.
            raise
        manifest['packages'] = [p for p in manifest['packages'] if p['postgres_major'] != major] + [package]
        manifest['unavailable'] = [r for r in manifest['unavailable'] if r['postgres_major'] != major]
        _write_json_atomic(manifest_path, manifest)
    return dict(receipt_path=str(manifest_path), published=False, packages=manifest['packages'])
