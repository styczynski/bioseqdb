#!/usr/bin/env python3
"""Evacuate explicitly retired release hosts; never delete EC2 resources.

Copy all retained reports, package archives (including unqualified outputs), SDK
companions, and MCP logs/state to local checksum-verified rescue archives. New
admissions are blocked independently by package_retirements.json. At the cutoff,
stop only Bioseq containers and identified MCP package worker process groups,
then repeat collection. A host-local shutdown timer provides a final safety net.
Unqualified archives remain quarantine evidence, never automatically published.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import hashlib
import inspect
import json
from pathlib import Path
import shlex
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from infra import build_release_fleet as fleet

DESTINATION = fleet.CONTROL / "retirements"


def create_archive(host, phase):
    """Runs on the remote host; no source changes or build invocation."""
    import hashlib
    import json
    import os
    from pathlib import Path
    import subprocess
    import tarfile
    import time
    os.nice(19)
    root = Path('/home/ubuntu/bioseqdb')
    paths = set()
    for folder in (root / 'reporting', root / 'bin_artifacts/packages',
                   root / 'build/package-linux/smoke-diagnostics'):
        if folder.exists():
            paths.update(p for p in folder.rglob('*') if p.is_file() and not p.is_symlink())
    native = root / 'build/package-linux/native/cells'
    if native.exists():
        paths.update(p for p in native.rglob('*') if p.is_file() and not p.is_symlink()
                     and p.name.endswith(('.deb', '.rpm', '.pkg.tar.zst')))
    paths.update(p for p in Path('/tmp').glob('bioseq_build_mcp*')
                 if p.is_file() and not p.is_symlink() and p.suffix in ('.log', '.json'))
    paths.update(p for p in root.glob('ALL_RESULTS*.yaml') if p.is_file() and not p.is_symlink())
    # Source is authoritative locally; preserve evidence of the frozen remote revision.
    metadata = {'host': host, 'phase': phase, 'time_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                'source_delivery': 'frozen local commit copied without .git; revisions are in package manifests'}
    if (root / '.git').exists():
        metadata['source_head'] = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
        metadata['source_status'] = subprocess.check_output(['git', 'status', '--short'], cwd=root, text=True)
    archive = Path('/tmp') / f'bioseq-retirement-{host}-{phase}.tar.gz'
    with tarfile.open(archive, 'w:gz', compresslevel=1) as output:
        for path in sorted(paths):
            output.add(path, arcname=str(path).lstrip('/'), recursive=False)
    digest = hashlib.file_digest(archive.open('rb'), 'sha256').hexdigest()
    return dict(metadata, archive=str(archive), sha256=digest, bytes=archive.stat().st_size,
                file_count=len(paths))


def stop_package_workers():
    """Remote cutoff: identify exact owned containers and process groups first."""
    import json
    import os
    from pathlib import Path
    import signal
    import subprocess
    import time
    state = json.loads(Path('/tmp/bioseq_build_mcp_state.json').read_text())
    containers = subprocess.check_output(['docker', 'ps', '--format', '{{.ID}} {{.Names}}'], text=True)
    stopped = []
    for line in containers.splitlines():
        cid, name = line.split(maxsplit=1)
        if name.startswith('bioseq-'):
            subprocess.run(['docker', 'stop', '--time', '10', cid], check=True, timeout=20,
                           stdout=subprocess.DEVNULL)
            stopped.append(name)
    # Normally the MCP owner records failure naturally when its container stops.
    time.sleep(3)
    killed = []
    for mode in ('package-linux', 'website', 'benchmark-e2e'):
        pid = state.get(mode, {}).get('pid')
        if not isinstance(pid, int) or pid <= 1:
            continue
        result = subprocess.run(['ps', '-p', str(pid), '-o', 'args='], capture_output=True, text=True)
        if 'server.py' not in result.stdout or f'--run-build {mode} ' not in result.stdout + ' ':
            continue
        pgid = os.getpgid(pid)
        if pgid != pid:
            raise RuntimeError(f'Refusing unowned process group for {mode}/{pid}')
        os.killpg(pgid, signal.SIGTERM)
        killed.append(pid)
    return {'stopped_containers': stopped, 'terminated_workers': killed}


def archive_host(host, phase):
    directory = DESTINATION / host
    directory.mkdir(parents=True, exist_ok=True)
    code = inspect.getsource(create_archive) + f'\nprint(__import__("json").dumps(create_archive({host!r}, {phase!r})))\n'
    metadata = json.loads(fleet.remote(host, 'python3 -c ' + shlex.quote(code), timeout=900))
    path = directory / Path(metadata['archive']).name
    ssh = fleet.fleet.ssh(host)
    transfer = subprocess.run(['rsync', '-a', '--partial', '-e', shlex.join(ssh[:-1]),
                              ssh[-1] + ':' + metadata['archive'], str(path)],
                             capture_output=True, text=True, timeout=600)
    if transfer.returncode:
        raise RuntimeError(transfer.stderr[-1000:])
    checksum = hashlib.sha256()
    with path.open('rb') as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b''):
            checksum.update(block)
    digest = checksum.hexdigest()
    if digest != metadata['sha256'] or path.stat().st_size != metadata['bytes']:
        raise RuntimeError(f'{host}: rescue archive checksum mismatch')
    metadata['local_file'] = str(path.relative_to(fleet.ROOT))
    fleet.write_json_atomic(directory / f'{phase}.json', metadata)
    fleet.event('retirement_archive_verified', **metadata)
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', required=True)
    parser.add_argument('--cutoff-utc', required=True)
    parser.add_argument('--shutdown-utc', required=True)
    args = parser.parse_args()
    hosts = [h for h in fleet.PLAN if fleet.retirement_policy(h).get('profile') == args.profile]
    if not hosts:
        raise RuntimeError('No explicitly configured retirement hosts')
    cutoff = datetime.fromisoformat(args.cutoff_utc).timestamp()
    shutdown = datetime.fromisoformat(args.shutdown_utc)
    if shutdown.tzinfo != timezone.utc or shutdown.timestamp() <= cutoff:
        raise RuntimeError('Expected explicit UTC cutoff before shutdown')
    identity = json.loads(subprocess.check_output(
        ['aws', '--profile', args.profile, 'sts', 'get-caller-identity', '--output', 'json'], text=True))
    if any(fleet.retirement_policy(h)['account_id'] != identity['Account'] for h in hosts):
        raise RuntimeError('AWS account identity mismatch')
    # This persists on each EC2 host if the laptop sleeps or this collector dies.
    for host in hosts:
        command = ('sudo -n systemd-run --unit=bioseq-account-retirement '
                   '--on-calendar=' + shlex.quote(shutdown.strftime('%Y-%m-%d %H:%M:%S UTC'))
                   + ' /usr/sbin/shutdown -h now')
        result = fleet.remote(host, 'systemctl is-active --quiet bioseq-account-retirement.timer || '
                              + command + ' 2>&1', timeout=30)
        fleet.event('retirement_shutdown_armed', host=host, utc=args.shutdown_utc, response=result.strip())
    def backup(item):
        host, phase = item
        try:
            return archive_host(host, phase)
        except Exception as error:
            fleet.event('retirement_collection_failed', host=host, phase=phase, error=str(error))
            return None
    with ThreadPoolExecutor(max_workers=4) as pool:
        initial = list(pool.map(backup, [(h, 'initial') for h in hosts]))
    if not all(initial):
        raise RuntimeError('Initial rescue failed; inspect errors before shutdown')
    fleet.event('retirement_initial_collection_complete', hosts=hosts)
    from e2e_tuple_progress import append_progress_log
    append_progress_log(f"All {len(hosts)} {args.profile} hosts now have SHA-256-verified initial rescue archives locally. "
                        f"Final drain scheduled for {args.cutoff_utc}; host-local shutdown timers for {args.shutdown_utc}. "
                        "No unqualified output was promoted to release acceptance.", title="Account retirement initial evacuation verified")
    while time.time() < cutoff:
        time.sleep(min(60, cutoff - time.time()))
    def finish(host):
        code = inspect.getsource(stop_package_workers) + '\nprint(__import__("json").dumps(stop_package_workers()))\n'
        try:
            stopped = json.loads(fleet.remote(host, 'python3 -c ' + shlex.quote(code), timeout=90))
            fleet.event('retirement_jobs_stopped', host=host, **stopped)
            return backup((host, 'final'))
        except Exception as error:
            fleet.event('retirement_drain_failed', host=host, error=str(error))
            return None
    with ThreadPoolExecutor(max_workers=4) as pool:
        final = list(pool.map(finish, hosts))
    fleet.event('retirement_final_collection_complete', hosts=hosts, success=all(final))
    if not all(final):
        raise RuntimeError('Final evacuation incomplete; initial archives remain safe')
    append_progress_log(f"Final rescue archives for all {len(hosts)} {args.profile} hosts verified locally after stopping Bioseq jobs. "
                        "Packages, SDK companions, retained raw benchmark reports, MCP state and full logs are preserved under "
                        "reporting/package_release_fleet/retirements. Host-local shutdown timers remain armed. "
                        "Compiler caches are reproducible and excluded; incomplete archives are not accepted packages.",
                        title="Account retirement final evacuation verified")


if __name__ == '__main__':
    main()
