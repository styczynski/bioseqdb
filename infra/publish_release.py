#!/usr/bin/env python3
"""Publish collected artifacts, never build them. Preview by default; --apply writes.

SourceForge uploads use existing SSH credentials, not the Release API token.
SOURCE_FORCE_OAUTH_TOKEN checks project permissions; SOURCE_FORGE_RELEASE_TOKEN
sets file labels. Secrets never enter command arguments, URLs, output or state.
"""
from __future__ import annotations

import argparse
import base64
import fcntl
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
CONTROL = ROOT / 'reporting/release_publish'
RELEASE = '0.1.0-alpha.1'
PROJECT = 'bioseqdb'
SF_USER = 'styczynski'
WEB_REPO = 'styczynski/bioseq-db-web'
SF_HOST = 'frs.sourceforge.net'
# Published by SourceForge over HTTPS, checked 2026-09-24:
# https://sourceforge.net/p/forge/documentation/SSH%20Key%20Fingerprints/
SF_ED25519 = '209BDmH3jsRyO9UeGPPgLWPSegKmYCBIya0nR/AWWCY'
SUMMARY = 'Genomic sequence storage, indexing and alignment inside PostgreSQL'
PROFILE_DESCRIPTION = (
    'BioseqDB is a PostgreSQL extension for storing, indexing and searching biological '
    'sequences with SQL. Combine genomic matches with relational data using compact '
    'sequence types and columnar storage.\n\n'
    'Choose bounded ntHash or other minimizers, SIMD-DP or RMQ chaining, and Block '
    'Aligner or KSW2 alignment. Minimizer indexes cover fragments or whole sequences. '
    'Packaged defaults enable vectorization and column caching; the automatically '
    'loaded query cache is disabled until explicitly enabled.\n\n'
    'Experimental, unsigned alpha packages are provided for ARM64 and x86-64 Linux. '
    'Choose the matching distribution and PostgreSQL major. Published packages pass '
    'payload checks and fresh installed-SQL checks. Prerelease/development PostgreSQL '
    'packages are labelled separately.\n\n'
    'Documentation: https://bioseqdb.com/\n'
    'Source: https://github.com/styczynski/bioseqdb'
)
DESCRIPTION = (
    'BioseqDB is a PostgreSQL extension for storing, indexing and searching '
    'biological sequences with SQL. It brings compact sequence storage and '
    'minimizer-based sequence alignment into the database, so researchers and '
    'developers can combine genomic matches with relational data.\n\n'
    'The configurable search pipeline includes bounded ntHash and other '
    'minimizer algorithms, SIMD dynamic-programming and RMQ chaining, and '
    'Block Aligner or KSW2 base alignment. Minimizer indexes support both '
    'per-fragment and per-sequence layouts. CPU vectorization and column '
    'caching are enabled in the packaged defaults; the companion query cache '
    'is loaded automatically but remains disabled until explicitly enabled.\n\n'
    'This is an experimental alpha release for evaluation, not a production '
    'stability guarantee. Native Linux packages and macOS arm64 installers are organized by distribution, '
    'CPU architecture and PostgreSQL major version. Each published package '
    'passed payload and ELF/Mach-O checks plus SQL checks in a fresh matching '
    'PostgreSQL runtime. macOS acceptance uses an extracted installer payload in '
    'a private Homebrew PostgreSQL prefix; Apple Installer system installation '
    'is not claimed as tested. Packages are unsigned; macOS installers are not notarized.\n\n'
    'Documentation and SQL examples: https://bioseqdb.com/\n'
    'Source code: https://github.com/styczynski/bioseqdb\n'
    'Select the exact distribution, architecture and PostgreSQL major; '
    'packages are not interchangeable across server majors. PostgreSQL '
    'prerelease/development packages, when available, are labelled separately.'
)


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def command(args, *, cwd=ROOT, input=None, timeout=180):
    result = subprocess.run(list(map(str, args)), cwd=cwd, input=input,
                            capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        # No credential-bearing argv or response bodies in exceptions.
        raise RuntimeError(f'{args[0]} failed ({result.returncode}): {result.stderr[-700:]}')
    return result.stdout.strip()


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)


def event(kind, **details):
    row = dict(timestamp_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
               event=kind, **details)
    CONTROL.mkdir(parents=True, exist_ok=True)
    with (CONTROL / 'events.jsonl').open('a') as stream:
        stream.write(json.dumps(row) + '\n')
    print(json.dumps(row), flush=True)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    """Never forward a SourceForge credential to a redirect destination."""
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def sf_api(path, *, method='GET', fields=None, oauth=False):
    if not path.startswith('/') or path.startswith('//'):
        raise ValueError('SourceForge API requires an origin-relative path')
    headers = {'Accept': 'application/json', 'User-Agent': 'BioseqDB-release/1'}
    data = None
    if oauth:
        token = os.environ.get('SOURCE_FORCE_OAUTH_TOKEN')
        if not token:
            raise RuntimeError('SOURCE_FORCE_OAUTH_TOKEN is not exported')
        headers['Authorization'] = 'Bearer ' + token
    if fields is not None:
        token = os.environ.get('SOURCE_FORGE_RELEASE_TOKEN')
        if not token:
            raise RuntimeError('SOURCE_FORGE_RELEASE_TOKEN is not exported')
        data = urllib.parse.urlencode(dict(fields, api_key=token)).encode()
        headers['Content-Type'] = 'application/x-www-form-urlencoded'
    request = urllib.request.Request('https://sourceforge.net' + path,
                                     data=data, headers=headers, method=method)
    try:
        with urllib.request.build_opener(NoRedirect).open(request, timeout=45) as response:
            result = json.load(response)
    except urllib.error.HTTPError as error:
        raise RuntimeError(f'SourceForge API HTTP {error.code} for {path}') from None
    if result.get('error'):
        raise RuntimeError(f'SourceForge API rejected {path}')
    return result


def accepted_packages():
    """Snapshot the atomic collection manifest and verify only approved bytes."""
    root = ROOT / 'bin_artifacts/packages'
    manifest = json.loads((root / 'manifest.json').read_text())
    accepted = []
    seen = set()
    for package in manifest['packages']:
        if (package.get('validation', {}).get('passed') is not True
                or package.get('installed_sql_smoke', {}).get('passed') is not True
                or package['installed_sql_smoke'].get('default_insert_search') is not True
                or package['installed_sql_smoke'].get('default_persisted_search') is not True):
            continue
        relative = PurePosixPath(package['file'])
        if relative.is_absolute() or '..' in relative.parts or not relative.parts:
            raise RuntimeError('Unsafe package path in collection manifest')
        path = root.joinpath(*relative.parts)
        if path.is_symlink() or not path.resolve().is_relative_to(root.resolve()):
            raise RuntimeError('Package escapes collection directory')
        digest = sha256(path)
        if digest != package['sha256'] or path.stat().st_size != package['bytes']:
            raise RuntimeError(f'Collected package checksum/size mismatch: {relative}')
        for field in ('distro', 'architecture'):
            if not re.fullmatch(r'[a-z0-9_.-]+', package[field]):
                raise RuntimeError(f'Unsafe package {field}')
        major = package['postgres_major']
        if type(major) is not int or major < 17:
            raise RuntimeError('Invalid PostgreSQL major')
        key = (major, package['distro'], package['architecture'])
        if key in seen:
            raise RuntimeError(f'Duplicate package cell: {key}')
        seen.add(key)
        # Immutable revision folders preserve prior files when a cell is repaired.
        remote = f"pg{major}/{package['distro']}/{package['architecture']}/{digest[:16]}/{path.name}"
        sdk_files = []
        for companion in package.get('postgres_sdk_support_files', []):
            companion_relative = PurePosixPath(companion['file'])
            if (companion_relative.is_absolute() or '..' in companion_relative.parts
                    or not companion_relative.parts or companion_relative.name == path.name):
                raise RuntimeError('Unsafe SDK support file path')
            companion_path = root.joinpath(*companion_relative.parts)
            if (companion_path.is_symlink()
                    or not companion_path.resolve().is_relative_to(root.resolve())
                    or companion_path.stat().st_size != companion['bytes']
                    or sha256(companion_path) != companion['sha256']):
                raise RuntimeError('SDK support file differs from accepted package metadata')
            sdk_files.append(dict(file=str(PurePosixPath(remote).parent / companion_relative.name),
                                  source=companion_path, bytes=companion['bytes'],
                                  sha256=companion['sha256']))
        accepted.append(dict(file=remote, source=path, sha256=digest,
            bytes=path.stat().st_size, postgres_major=major,
            distro=package['distro'], architecture=package['architecture'],
            package_version=package['validation']['package_version'],
            postgres_release_channel=package['postgres_release_channel'],
            signed=bool(package.get('signed')),
            postgres_sdk_support_files=sdk_files,
            validation=('Mach-O and extracted-payload private PostgreSQL SQL passed; unsigned/unnotarized installer'
                        if package['validation'].get('format') == 'pkg'
                        else 'ELF and fresh installed SQL passed'),
            **{field: package[field] for field in
               ('postgres_version', 'postgres_runtime_requirement', 'postgres_sdk_formula')
               if field in package}))
    if not accepted:
        raise RuntimeError('No accepted packages to publish')
    return accepted


def website_inventory(root, *, repository=False):
    manifest = json.loads((root / 'site-manifest.json').read_text())
    expected = manifest.get('outputs', {})
    actual = {}
    for path in root.rglob('*'):
        relative = path.relative_to(root).as_posix()
        if repository and relative.split('/')[0] in ('.git', '.github'):
            continue
        if path.is_symlink():
            raise RuntimeError('Website contains a symlink')
        if path.is_dir():
            continue
        if not path.is_file():
            raise RuntimeError('Website contains a special file')
        if relative.split('/')[0] in ('.git', '.github'):
            raise RuntimeError('Website artifact must not replace repository metadata/workflows')
        if relative != 'site-manifest.json':
            actual['/' + relative] = sha256(path)
    if (manifest.get('schema') != 'bioseq.website.site_manifest'
            or manifest.get('hash_algorithm') != 'sha256'
            or '/index.html' not in actual or actual != expected):
        raise RuntimeError('Website does not match its generated manifest')
    return actual


def replace_website(checkout, source):
    """Only mutate the verified disposable clone; preserve .git and .github."""
    if Path(command(['git', 'rev-parse', '--show-toplevel'], cwd=checkout)).resolve() != checkout.resolve():
        raise RuntimeError('Website target is not the expected Git root')
    if checkout.resolve() == ROOT or not checkout.resolve().is_relative_to(CONTROL.resolve()):
        raise RuntimeError('Website replacement is restricted to disposable publication clones')
    website_inventory(source)
    for path in checkout.iterdir():
        if path.name in ('.git', '.github'):
            continue
        if path.is_dir() and not path.is_symlink():
            shutil.rmtree(path)
        else:
            path.unlink()
    for path in source.iterdir():
        destination = checkout / path.name
        if path.is_dir():
            shutil.copytree(path, destination)
        else:
            shutil.copy2(path, destination)
    website_inventory(checkout, repository=True)


def publish_website(apply):
    source = ROOT / 'bin_artifacts/website'
    payload = website_inventory(source)
    metadata = json.loads(command(['gh', 'api', f'repos/{WEB_REPO}']))
    branch = metadata['default_branch']
    if metadata['full_name'] != WEB_REPO or not metadata.get('permissions', {}).get('push'):
        raise RuntimeError('Unexpected website repository or no push access')
    parent = Path(tempfile.mkdtemp(prefix='website-', dir=CONTROL))
    checkout = parent / 'checkout'
    command(['gh', 'repo', 'clone', WEB_REPO, checkout, '--', '--depth', '1', '--branch', branch])
    previous = command(['git', 'rev-parse', 'HEAD'], cwd=checkout)
    workflows = command(['git', 'rev-parse', 'HEAD:.github'], cwd=checkout)
    replace_website(checkout, source)
    command(['git', 'add', '-A', '--', '.'], cwd=checkout)
    tree = command(['git', 'write-tree'], cwd=checkout)
    if command(['git', 'rev-parse', f'{tree}:.github'], cwd=checkout) != workflows:
        raise RuntimeError('Website publication would change .github')
    changed = command(['git', 'diff', '--cached', '--name-status'], cwd=checkout)
    event('website_preview', checkout=str(checkout), previous_commit=previous,
          payload_files=len(payload) + 1, changed_files=len(changed.splitlines()),
          preserved_github_tree=workflows)
    if not apply or not changed:
        return
    for name in ('user.name', 'user.email'):
        command(['git', 'config', name, command(['git', 'config', name])], cwd=checkout)
    command(['git', 'commit', '-m', f'Publish BioseqDB {RELEASE} website'], cwd=checkout)
    commit = command(['git', 'rev-parse', 'HEAD'], cwd=checkout)
    command(['git', 'push', 'origin', f'HEAD:refs/heads/{branch}'], cwd=checkout)
    remote_commit = json.loads(command(['gh', 'api', f'repos/{WEB_REPO}/git/ref/heads/{branch}']))['object']['sha']
    if remote_commit != commit:
        raise RuntimeError('Website remote HEAD does not match the publication commit')
    save(CONTROL / 'website.json', dict(repository=WEB_REPO, branch=branch,
         previous_commit=previous, commit=commit, preserved_github_tree=workflows,
         site_manifest_sha256=sha256(source / 'site-manifest.json')))
    event('website_pushed', commit=commit, previous_commit=previous,
          repository=WEB_REPO, removed_old_files_recoverable_in_git=True)


def ssh_options():
    """Pin SourceForge's published ED25519 key; do not modify ~/.ssh."""
    scanned = command(['ssh-keyscan', '-T', '15', '-t', 'ed25519', SF_HOST], timeout=30)
    keys = []
    for line in scanned.splitlines():
        parts = line.split()
        if len(parts) != 3 or parts[1] != 'ssh-ed25519':
            continue
        fingerprint = base64.b64encode(hashlib.sha256(base64.b64decode(parts[2])).digest()).decode().rstrip('=')
        if fingerprint != SF_ED25519:
            raise RuntimeError('SourceForge SSH host fingerprint differs from its published key')
        keys.append(line)
    if not keys:
        raise RuntimeError('No verified SourceForge SSH host key')
    known = CONTROL / 'sourceforge_known_hosts'
    known.write_text('\n'.join(keys) + '\n')
    options = ['-o', 'BatchMode=yes', '-o', 'ConnectTimeout=20', '-o', 'StrictHostKeyChecking=yes',
               '-o', 'HostKeyAlgorithms=ssh-ed25519', '-o', f'UserKnownHostsFile={known}']
    configured = os.environ.get('SOURCEFORGE_SSH_KEY')
    identity = Path(configured).expanduser() if configured else Path.home() / '.ssh/bioseqdb_sourceforge_ed25519'
    if configured or identity.exists():
        if not identity.is_file():
            raise RuntimeError('Configured SourceForge SSH identity does not exist')
        options.extend(['-o', 'IdentitiesOnly=yes', '-i', str(identity)])
    return options


def sourceforge_access():
    project = sf_api(f'/rest/p/{PROJECT}', oauth=True)
    permission = sf_api(f'/rest/p/{PROJECT}/has_access?user={SF_USER}&perm=admin', oauth=True)
    if project.get('shortname') != PROJECT or permission.get('result') is not True:
        raise RuntimeError('SourceForge project identity/admin permission could not be verified')
    options = ssh_options()
    command(['sftp', *options, '-b', '-', f'{SF_USER}@{SF_HOST}'],
            input=f'ls /home/frs/project/{PROJECT}\n', timeout=45)
    return options


def publish_packages(apply):
    packages = accepted_packages()
    event('packages_preview', release=RELEASE, accepted=len(packages),
          package_versions=sorted({p['package_version'] for p in packages}),
          sourceforge_project=PROJECT)
    if not apply:
        return
    if not os.environ.get('SOURCE_FORGE_RELEASE_TOKEN'):
        raise RuntimeError('SOURCE_FORGE_RELEASE_TOKEN is not exported')
    options = sourceforge_access()
    stage = Path(tempfile.mkdtemp(prefix='packages-', dir=CONTROL))
    records = []
    for package in packages:
        path = stage / package['file']
        path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(package['source'], path)
        if sha256(path) != package['sha256']:
            raise RuntimeError('Package changed while taking publication snapshot')
        record = {k: v for k, v in package.items() if k not in ('source', 'postgres_sdk_support_files')}
        companions = []
        for companion in package['postgres_sdk_support_files']:
            copied = stage / companion['file']
            copied.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(companion['source'], copied)
            if sha256(copied) != companion['sha256']:
                raise RuntimeError('SDK support file changed during publication snapshot')
            companions.append({k: v for k, v in companion.items() if k != 'source'})
        if companions:
            record['postgres_sdk_support_files'] = companions
        records.append(record)
    save(stage / 'manifest.json', dict(release=RELEASE, packages=records,
         note='Release label differs from the retained embedded package version. '
              'Unsigned alpha packages; more accepted cells may be added to this same release.'))
    readme = f'# BioseqDB {RELEASE}\n\n{DESCRIPTION}\n\n'
    readme += ('## Release identity\n\nThe public release is 0.1.0-alpha.1. Linux archives retain '
               'their embedded package version 4.2.3; they have not been relabelled or rebuilt. '
               'macOS installers use package version 0.1.0-alpha.1. '
               'This folder grows as additional distro/PostgreSQL packages pass acceptance. '
               'Checksums and the current exact cell list are in manifest.json and SHA256SUMS.\n\n'
               '## Available packages\n\n')
    for p in records:
        readme += f"- PostgreSQL {p['postgres_major']} ({p['postgres_release_channel']}), {p['distro']}, {p['architecture']}: [{Path(p['file']).name}]({p['file']})\n"
        if p.get('postgres_runtime_requirement'):
            readme += f"  Runtime requirement: {p['postgres_runtime_requirement']}. SDK formula: `{p['postgres_sdk_formula']}`.\n"
        for companion in p.get('postgres_sdk_support_files', []):
            readme += f"  SDK support: [{PurePosixPath(companion['file']).name}]({companion['file']})\n"
    (stage / 'README.md').write_text(readme)
    shutil.copy2(ROOT / 'website/static/favicon.svg', stage / 'bioseqdb-logo.svg')
    (stage / 'SHA256SUMS').write_text(''.join(
        f"{sha256(p)}  {p.relative_to(stage).as_posix()}\n"
        for p in sorted(stage.rglob('*')) if p.is_file() and p.name != 'SHA256SUMS'))
    # rsync never deletes remote files. Immutable digest paths avoid replacing binaries.
    import shlex
    shell = shlex.join(['ssh', *options])
    destination = f'{SF_USER}@{SF_HOST}:/home/frs/project/{PROJECT}/{RELEASE}/'
    # FRS acknowledged --delay-updates but exposed only empty directories in
    # read-back. Ordinary per-file rsync renames were verified to publish bytes.
    command(['rsync', '-rlt', '--checksum', '-e', shell,
             str(stage) + '/', destination], timeout=1800)
    state_path = CONTROL / 'sourceforge.json'
    old = json.loads(state_path.read_text()) if state_path.exists() else {}
    verified = old.get('verified_files', {})
    uploads = records + [companion for p in records for companion in p.get('postgres_sdk_support_files', [])]
    for p in uploads:
        relative = RELEASE + '/' + p['file']
        quoted = urllib.parse.quote(relative, safe='/')
        if verified.get(relative) != p['sha256']:
            # Read back uploaded bytes through SFTP, independent of local source.
            downloaded = stage / 'verify-upload'
            command(['sftp', *options, '-b', '-', f'{SF_USER}@{SF_HOST}'], input=
                    f'get /home/frs/project/{PROJECT}/{relative} {downloaded}\n', timeout=300)
            if sha256(downloaded) != p['sha256']:
                raise RuntimeError(f'SourceForge read-back checksum mismatch: {relative}')
            downloaded.unlink()
            verified[relative] = p['sha256']
            save(state_path, dict(release=RELEASE, verified_files=verified))
        if 'postgres_major' not in p:
            event('sdk_support_upload_verified', file=relative, sha256=p['sha256'])
            continue
        label = f"BioseqDB {RELEASE} - PG{p['postgres_major']} {p['distro']} {p['architecture']}"
        # The canonical file resource ends in '/'; FRS redirects the bare
        # filename, and authenticated redirects are deliberately refused.
        response = sf_api(f'/projects/{PROJECT}/files/{quoted}/', method='PUT',
                          fields={'download_label': label})
        metadata = response.get('result', {})
        if (metadata.get('name') != Path(p['file']).name
                or metadata.get('x_sf', {}).get('download_label') != label):
            raise RuntimeError(f'SourceForge did not confirm the file label: {relative}')
        event('package_upload_verified', file=relative, sha256=p['sha256'])
    event('packages_published', release=RELEASE, accepted=len(records),
          url=f'https://sourceforge.net/projects/{PROJECT}/files/{RELEASE}/')


def prepare_profile():
    """Save ready-to-use profile text; no undocumented metadata writes."""
    path = CONTROL / 'sourceforge-profile.json'
    save(path, dict(name='BioseqDB', summary=SUMMARY, short_description=PROFILE_DESCRIPTION,
         external_homepage='https://bioseqdb.com/',
         labels=['bioinformatics', 'genomics', 'postgresql', 'dna', 'sequence-alignment',
                 'minimizers', 'simd', 'database-extension'],
         features=['SQL sequence storage and similarity search',
                   'Compact sequence types and columnar storage',
                   'Minimizer indexes per fragment or per sequence',
                   'Configurable vectorized seeding, chaining and alignment',
                   'Native Linux packages for ARM64 and x86-64',
                   'SQL tutorials and API reference at bioseqdb.com'],
         icon_file=str(ROOT / 'website/static/favicon.svg'),
         admin_page=f'https://sourceforge.net/p/{PROJECT}/admin/overview',
         note='Prepared metadata only. Public REST documentation does not expose '
              'an existing-project description/icon update endpoint.'))
    event('project_profile_prepared', file=str(path))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target', choices=['website', 'packages', 'all', 'check-sourceforge', 'project-profile'])
    parser.add_argument('--apply', action='store_true', help='Commit/push or upload; default is preview')
    parser.add_argument('--watch', action='store_true', help='Recheck collected packages every five minutes')
    args = parser.parse_args()
    if args.watch and (args.target != 'packages' or not args.apply):
        parser.error('--watch requires packages --apply')
    CONTROL.mkdir(parents=True, exist_ok=True)
    with (CONTROL / 'publisher.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if args.target == 'project-profile':
            prepare_profile()
            return
        if args.target == 'check-sourceforge':
            sourceforge_access()
            event('sourceforge_access_verified')
            return
        if args.target in ('website', 'all'):
            publish_website(args.apply)
        if args.target in ('packages', 'all'):
            previous = None
            while True:
                digest = sha256(ROOT / 'bin_artifacts/packages/manifest.json')
                if digest != previous:
                    publish_packages(args.apply)
                    previous = digest
                if not args.watch:
                    break
                time.sleep(300)


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        # Never print HTTP response bodies or environment values.
        message = str(error)
        for name in ('SOURCE_FORGE_RELEASE_TOKEN', 'SOURCE_FORCE_OAUTH_TOKEN'):
            if os.environ.get(name):
                message = message.replace(os.environ[name], '[REDACTED]')
        print(f'Publication stopped: {message}', file=sys.stderr)
        sys.exit(1)
