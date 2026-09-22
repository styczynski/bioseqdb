#!/usr/bin/env python3
"""Copy original-family frozen E2E payload to a new test7 host, without builds.

Uses bioseq-build MCP to validate/export and import artifacts. No source-host
database, private key, credentials, result row or process state is cloned.
"""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import sys
import time

import manage_e2e_fleet as fleet
from provision_e2e_test7 import aws, state_path
from recover_e2e_measurements import ROOT, atomic


def rpc(command, tool, arguments):
    messages = [
        {'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {
            'protocolVersion': '2024-11-05', 'capabilities': {},
            'clientInfo': {'name': 'e2e-runtime-replica', 'version': '1'}}},
        {'jsonrpc': '2.0', 'method': 'notifications/initialized', 'params': {}},
        {'jsonrpc': '2.0', 'id': 2, 'method': 'tools/call', 'params': {'name': tool, 'arguments': arguments}}]
    result = subprocess.run(command, input='\n'.join(map(json.dumps, messages)) + '\n',
                            capture_output=True, text=True, timeout=180)
    for line in result.stdout.splitlines():
        try:
            response = json.loads(line)
        except ValueError:
            continue
        if response.get('id') == 2:
            payload = response.get('result', {})
            if 'error' in response or payload.get('isError'):
                raise RuntimeError(str(response))
            return json.loads(payload['content'][0]['text'])
    raise RuntimeError(result.stderr[-1500:] + result.stdout[-1500:])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('family', choices=['c7g', 'm4', 'm7a', 'm7i'])
    parser.add_argument('--profile', choices=['test7', 'test2'], default='test7')
    args = parser.parse_args()
    host = args.profile + '-' + args.family
    record = json.loads(state_path(args.profile).read_bytes())['hosts'][args.family]
    instance = aws('ec2', 'describe-instances', '--instance-ids', record['instance_id'], profile=args.profile)['Reservations'][0]['Instances'][0]
    assert instance['InstanceType'] == record['instance_type'] and instance['State']['Name'] == 'running'
    address = instance['PublicIpAddress']
    key = ROOT / 'infra/testing_infra/.secrets/bioseq-bench-aws.pem'
    dest = ['ssh', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=accept-new',
            '-o', 'ConnectTimeout=15', '-i', str(key), 'ubuntu@' + address]
    subprocess.run(dest + ['test -f /var/lib/bioseq-e2e-replica-bootstrap.ready'], check=True)
    output = ROOT / ('reporting/bioseq_benchmark_e2e/' + args.profile + '_replicas') / host
    output.mkdir(parents=True, exist_ok=True)
    source = 'default-' + args.family
    retirement_path = ROOT / 'bench/e2e_fleet_retirement.json'
    retirement = json.loads(retirement_path.read_bytes())['hosts'] if retirement_path.exists() else {}
    if retirement.get(source, {}).get('draining'):
        # These are already verified copies of the exact original-family payload.
        source = {'c7g': 'test7-c7g', 'm4': 'test7-m4',
                  'm7a': 'test2-m7a', 'm7i': 'test2-m7i'}[args.family]
        if source == host:
            raise RuntimeError('Cannot clone a retiring source onto its only surviving replica')
    control = tuple(str(p.relative_to(ROOT)) for p in (ROOT / 'build_mcp').rglob('*.py'))
    fleet.sync_files(source, control)
    state = fleet.monitor._remote_state(*fleet.HOSTS[source])
    receipt_id = state['build_receipt_id']
    print(host, 'validating source frozen artifacts', receipt_id, flush=True)
    exported = rpc(fleet.ssh(source) + ['cd /home/ubuntu/bioseqdb && ' + fleet.ENVIRONMENT +
                                       'python3 build_mcp/server.py'],
                   'export_e2e_artifacts', {'build_receipt_id': receipt_id})
    atomic(output / 'source_export.json', (json.dumps(exported, indent=2) + '\n').encode())
    paths = exported['paths']
    allowed = ('/home/ubuntu/bioseqdb/build/', '/opt/bioseq-package-toolchain/',
               '/etc/postgresql/17/main/', '/usr/local/sbin/bioseq-zram-swap',
               '/etc/systemd/system/bioseq-zram-swap.service')
    if any(not p.startswith(allowed) or '..' in Path(p).parts or '\n' in p for p in paths):
        raise RuntimeError('unexpected export path')
    print(host, 'streaming', len(paths), 'validated runtime/configuration/dependency files', flush=True)
    # Low-priority compression; no temporary multi-GB archive on either machine.
    pack = fleet.ssh(source) + ['sudo -n nice -n 19 ionice -c 3 tar -C / --null --files-from=- -I "zstd -1 -T1" -cf -']
    unpack = dest + ['sudo -n tar -C / -I zstd -xf -']
    with (output / 'transfer.log').open('ab') as log:
        sender = subprocess.Popen(pack, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log)
        receiver = subprocess.Popen(unpack, stdin=sender.stdout, stdout=log, stderr=log)
        sender.stdout.close()
        sender.stdin.write(('\0'.join(p.lstrip('/') for p in paths) + '\0').encode())
        sender.stdin.close()
        rc = receiver.wait(timeout=3600)
        src_rc = sender.wait(timeout=60)
        if rc or src_rc:
            raise RuntimeError(f'artifact transfer failed: sender={src_rc} recipient={rc}')
    # Current local scripts/configs are authoritative. No Git push or commit.
    files = list(control)
    for directory in ('bench', 'bioseq', 'cmake', 'common', 'bench_framework'):
        files.extend(str(p.relative_to(ROOT)) for p in (ROOT / directory).rglob('*')
                     if p.is_file() and p.suffix in ('.py', '.json', '.cmake', '.sql', '.control', '.txt', '.hpp', '.h'))
    files += [str(p.relative_to(ROOT)) for p in ROOT.glob('CMake*.txt')]
    files += [str(p.relative_to(ROOT)) for p in ROOT.glob('CMake*.json')]
    files += ['infra/package_build_infra/scripts/finish_e2e_replica.sh']
    files += ['infra/package_build_infra/scripts/register_frozen_runtime_libraries.sh']
    files += ['bench_framework/index.cppm']
    transport = shlex.join(dest[:-1])
    subprocess.run(['rsync', '-azR', '--files-from=-', '-e', transport,
                    str(ROOT) + '/', 'ubuntu@' + address + ':/home/ubuntu/bioseqdb/'],
                   input='\n'.join(sorted(set(files))) + '\n', text=True, check=True, timeout=900)
    subprocess.run(dest + ['sudo -n bash /home/ubuntu/bioseqdb/infra/package_build_infra/scripts/finish_e2e_replica.sh'],
                   check=True, timeout=120)
    subprocess.run(dest + ['cd /home/ubuntu/bioseqdb && python3 bench/aggregate_e2e_algorithm_matrix.py --help >/dev/null'],
                   check=True, timeout=30)
    print(host, 'validating copied artifacts through MCP; no configure/build', flush=True)
    imported = rpc(dest + ['cd /home/ubuntu/bioseqdb && ' + fleet.ENVIRONMENT + 'python3 build_mcp/server.py'],
                   'import_e2e_artifacts', {'source_receipt': exported['source_receipt']})
    imported.update(fleet_host=host, address=address, instance_id=instance['InstanceId'],
                    instance_type=instance['InstanceType'], copied_at=time.time())
    atomic(output / 'imported.json', (json.dumps(imported, indent=2) + '\n').encode())
    print(json.dumps(imported), flush=True)


if __name__ == '__main__':
    main()
