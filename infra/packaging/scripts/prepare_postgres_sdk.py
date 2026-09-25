#!/usr/bin/env python3
"""MCP-container-owned native prerelease SDK packages where distro ones are missing.

Install a separate /opt/pgsql-MAJOR prefix. Never create a database, start a
service, replace the distro's server, or substitute a different major.
The resulting runtime package is also delivered beside the Bioseq package.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import tempfile
import urllib.request


def digest(path):
    """Hash a runtime package or upstream archive without buffering it in RAM."""
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def run(argv, *, cwd=None, environment=None):
    """Run one logged, checked SDK build command inside the owned container."""
    print('+ ' + shlex.join(map(str, argv)), flush=True)
    subprocess.run(list(map(str, argv)), cwd=cwd, env=environment, check=True)


def main():
    """Build/reuse and install the pinned SDK for the selected native cell."""
    if platform.system() != 'Linux' or os.geteuid() != 0:
        raise RuntimeError('Run only inside the MCP-owned native package container')
    major = int(os.environ['BIOSEQ_POSTGRES_MAJOR'])
    distro = os.environ['BIOSEQ_SDK_DISTRO']
    if distro not in ('arch', 'opensuse', 'fedora') or major not in (19, 20):
        raise RuntimeError('This SDK provider supports the selected missing prerelease majors only')
    pin_path = Path('/source/infra/packaging/postgres_sdks.json')
    pin = json.loads(pin_path.read_text())[str(major)]
    machine = platform.machine()
    if machine not in ('x86_64', 'aarch64'):
        raise RuntimeError('Unsupported native SDK architecture')
    cache = Path('/opt/bioseq-package-toolchain/postgres-sdk') / str(major)
    output = cache / 'output'
    output.mkdir(parents=True, exist_ok=True)
    metadata_path = output / 'sdk.json'
    package_name = f'bioseq-postgresql{major}-runtime'
    prefix = Path(f'/opt/pgsql-{major}')
    metadata = json.loads(metadata_path.read_text()) if metadata_path.is_file() else {}
    cached_name = metadata.get('file', 'missing')
    if Path(cached_name).name != cached_name:
        raise RuntimeError('Cached SDK artifact escaped its package directory')
    artifact = output / cached_name
    reusable = (metadata.get('source_sha256') == pin['sha256']
                and metadata.get('architecture') == machine
                and metadata.get('distro') == distro
                and artifact.is_file() and digest(artifact) == metadata.get('sha256'))
    if not reusable:
        archive = cache / 'source.archive'
        if not archive.is_file() or digest(archive) != pin['sha256']:
            partial = cache / 'source.partial'
            with urllib.request.urlopen(pin['url'], timeout=120) as response, partial.open('wb') as dest:
                shutil.copyfileobj(response, dest)
            if digest(partial) != pin['sha256']:
                raise RuntimeError('Pinned PostgreSQL source archive checksum mismatch')
            partial.replace(archive)
        # This newly created workspace alone is removed on exit; shared caches
        # and any previously built runtime package are outside its boundary.
        with tempfile.TemporaryDirectory(prefix=f'bioseq-sdk-pg{major}-', dir='/tmp') as raw:
            scratch = Path(raw)
            source = scratch / 'source'
            payload = scratch / 'payload'
            source.mkdir()
            payload.mkdir()
            run(['tar', '-xf', archive, '--strip-components=1', '-C', source])
            env = dict(os.environ)
            for key in ('CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS', 'MAKEFLAGS', 'MFLAGS', 'MAKELEVEL'):
                env.pop(key, None)
            env.update(CC='/opt/bioseq-package-toolchain/llvm-22/bin/clang',
                       CXX='/opt/bioseq-package-toolchain/llvm-22/bin/clang++',
                       CFLAGS='-O3 -DNDEBUG')
            # PostgreSQL's configure probes zlib.h/-lz directly, unlike its
            # pkg-config-aware LZ4/ZSTD probes. Honor the selected zlib ABI
            # provider's non-default paths (e.g. openSUSE zlib-ng-compat).
            env['CPPFLAGS'] = subprocess.check_output(
                ['pkg-config', '--cflags', 'zlib'], text=True, env=env).strip()
            env['LDFLAGS'] = subprocess.check_output(
                ['pkg-config', '--libs-only-L', 'zlib'], text=True, env=env).strip()
            run([source / 'configure', f'--prefix={prefix}', '--with-icu', '--with-openssl',
                 '--with-lz4', '--with-zstd'], cwd=source, environment=env)
            run(['make', '-j', os.environ.get('BIOSEQ_PACKAGE_PARALLELISM', '4')],
                cwd=source, environment=env)
            run(['make', 'install', f'DESTDIR={payload}'], cwd=source, environment=env)
            docs = payload / prefix.relative_to('/') / 'share/doc/postgresql'
            docs.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source / 'COPYRIGHT', docs / 'COPYRIGHT')
            (docs / 'BIOSEQ_SDK.json').write_text(json.dumps(dict(pin, major=major), indent=2) + '\n')
            if distro in ('opensuse', 'fedora'):
                top = scratch / 'rpm'
                for name in ('BUILD', 'BUILDROOT', 'RPMS', 'SOURCES', 'SPECS', 'SRPMS'):
                    (top / name).mkdir(parents=True, exist_ok=True)
                spec = top / 'SPECS/runtime.spec'
                spec.write_text(f'''Name: {package_name}
Version: {pin['version']}
Release: 1
Summary: Pinned experimental PostgreSQL {major} runtime and SDK for BioseqDB
License: PostgreSQL
URL: https://www.postgresql.org/
AutoProv: no
%global __requires_exclude ^(libpq|libecpg|libecpg_compat|libpgtypes)\\.so.*
%global debug_package %{{nil}}
%description
Isolated native PostgreSQL prerelease in {prefix}; not the distribution server.
No database initialization or service activation is performed.
%prep
%build
%install
mkdir -p %{{buildroot}}
cp -a {shlex.quote(str(payload))}/. %{{buildroot}}/
%files
{prefix}
''')
                run(['rpmbuild', '--define', f'_topdir {top}', '--define', '_build_id_links none',
                     '-bb', spec])
                built = list((top / 'RPMS').rglob('*.rpm'))
            else:
                recipe = scratch / 'pacman'
                recipe.mkdir()
                (recipe / 'PKGBUILD').write_text(f'''pkgname={package_name}
pkgver={pin['version']}
pkgrel=1
pkgdesc='Pinned experimental PostgreSQL {major} runtime and SDK for BioseqDB'
arch=('{machine}')
url='https://www.postgresql.org/'
license=('PostgreSQL')
depends=('glibc' 'icu' 'readline' 'openssl' 'lz4' 'zstd' 'zlib')
options=('!debug')
package() {{
  cp -a {shlex.quote(str(payload))}/. "$pkgdir"/
}}
''')
                user = 'bioseq-sdk-builder'
                if subprocess.run(['id', '-u', user], stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL).returncode:
                    run(['useradd', '--system', '--create-home', user])
                run(['chown', '-R', user, scratch])
                run(['runuser', '-u', user, '--', 'makepkg', '--force', '--nodeps', '--noconfirm'],
                    cwd=recipe)
                built = list(recipe.glob('*.pkg.tar.zst'))
            if len(built) != 1:
                raise RuntimeError(f'Expected one native SDK package, found {built}')
            artifact = output / built[0].name
            shutil.copy2(built[0], artifact)
            metadata = dict(file=artifact.name, sha256=digest(artifact), bytes=artifact.stat().st_size,
                            source_sha256=pin['sha256'], source_url=pin['url'],
                            version=pin['version'], postgres_major=major, architecture=machine,
                            distro=distro, runtime_package=package_name, prefix=str(prefix))
            temporary = output / 'sdk.json.tmp'
            temporary.write_text(json.dumps(metadata, indent=2) + '\n')
            temporary.replace(metadata_path)
    if distro == 'opensuse':
        run(['zypper', '--no-gpg-checks', '--non-interactive', 'install', '-y', str(artifact)])
    elif distro == 'fedora':
        run(['dnf', 'install', '-y', '--nogpgcheck', str(artifact)])
    else:
        run(['pacman', '-U', '--noconfirm', artifact])
    version = subprocess.check_output([str(prefix / 'bin/pg_config'), '--version'], text=True)
    if not version.startswith(f'PostgreSQL {major}'):
        raise RuntimeError('Installed SDK reports the wrong PostgreSQL major')
    print(f'POSTGRES_SDK_READY {artifact} sha256={metadata["sha256"]}', flush=True)


if __name__ == '__main__':
    main()
