#!/usr/bin/env python3
"""Replace an idle, maintenance-held worker's frozen payload from its own family.

Copy only the MCP-exported build-tree payload; keep host configuration, source,
databases and results untouched. Validate through MCP before queue admission.
The caller releases maintenance with the returned import receipt explicitly.
"""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import time

import manage_e2e_fleet as fleet
from replicate_e2e_runtime import rpc


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', choices=sorted(fleet.HOSTS))
    parser.add_argument('destination', choices=sorted(fleet.HOSTS))
    args = parser.parse_args()
    active = {host[0] for host in fleet.monitor.registered_hosts()}
    assert args.source in active and args.destination in active
    assert args.source != args.destination
    assert fleet.family(args.source) == fleet.family(args.destination)
    maintenance = json.loads((fleet.ROOT / 'bench/e2e_fleet_maintenance.json').read_bytes())
    assert maintenance['hosts'][args.destination]['paused'] is True
    state = fleet.monitor._remote_state(*fleet.HOSTS[args.destination])
    if state['status'] == 'missing':
        # Fresh replicas have an MCP import receipt but no run state yet.
        # Require their exact locally registered import, not a generic missing
        # state exception for an unrecognized or partially provisioned host.
        profile = args.destination.rsplit('-', 1)[0]
        record = json.loads((fleet.ROOT / 'bench/e2e_fleet_replicas.json').read_bytes())['hosts'][args.destination]
        imported_path = fleet.ROOT / ('reporting/bioseq_benchmark_e2e/' + profile + '_replicas') / args.destination / 'imported.json'
        previous_import = json.loads(imported_path.read_bytes())
        assert previous_import['build_receipt_id'] == record['build_receipt_id']
        assert previous_import['instance_id'] == record['machine_policy']['instance_id']
    else:
        assert state['status'] in fleet.TERMINAL_STATUSES, state
    assert not state.get('cleanup_unverified'), state
    source = fleet.monitor._remote_state(*fleet.HOSTS[args.source])
    command = 'cd /home/ubuntu/bioseqdb && ' + fleet.ENVIRONMENT + 'python3 build_mcp/server.py'
    exported = rpc(fleet.ssh(args.source) + [command], 'export_e2e_artifacts',
                   {'build_receipt_id': source['build_receipt_id']})
    origin = exported['source_receipt']
    while origin.get('kind') == 'bioseq-e2e-artifact-import-receipt':
        origin = origin['artifact_import']['source_receipt']
    assert origin['kind'] == 'bioseq-build-receipt'
    root = Path(origin['build_dir'])
    root.relative_to('/home/ubuntu/bioseqdb/build')
    paths = [p for p in exported['paths'] if Path(p).is_relative_to(root)]
    assert paths and all('..' not in Path(p).parts and '\n' not in p for p in paths)
    # An existing warm tree can retain an old codegen sub-build's install plan.
    # Preserve it outside the receipted tree; do not delete compiled objects or
    # relax the exact import-artifact comparison. Fresh replicas have none.
    # A worker migrated from the older AWS preset may not have this native
    # preset directory yet. The validated tar payload creates it below; there
    # are no old install plans to retire in that case.
    retire_plans = """import json,sys,time,pathlib,shutil
d=json.load(sys.stdin);root=pathlib.Path(d['root']).resolve()
root.relative_to('/home/ubuntu/bioseqdb/build')
if root.exists() and not root.is_dir():raise RuntimeError('Build destination is not a directory')
expected=set(d['expected']);retired=[]
backup=pathlib.Path('/home/ubuntu/bioseq-runtime-retired-plans-'+str(time.time_ns()))
for path in (root/'codegen').rglob('cmake_install.cmake'):
 relative=path.relative_to(root)
 if str(relative) in expected:continue
 if path.is_symlink() or not path.is_file():raise RuntimeError('Unexpected nonregular install plan')
 path.resolve(strict=True).relative_to(root)
 target=backup/relative;target.parent.mkdir(parents=True,exist_ok=True)
 shutil.move(str(path),str(target));retired.append({'path':str(path),'recoverable_backup':str(target)})
print(json.dumps(retired))
"""
    retired = subprocess.run(fleet.ssh(args.destination) + ['sudo -n python3 -c ' + shlex.quote(retire_plans)],
        input=json.dumps({'root': str(root), 'expected': [item['relative_path'] for item in origin['authorized_artifacts']]}),
        capture_output=True, text=True, timeout=30)
    if retired.returncode:
        raise RuntimeError('Install-plan retirement failed: ' + retired.stderr[-1800:])
    if json.loads(retired.stdout):
        print('Retired unused install plans:', retired.stdout.strip(), flush=True)
    evidence = fleet.OUTPUT / ('runtime_repair_' + args.destination + '_' + str(int(time.time())))
    evidence.mkdir()
    (evidence / 'export.json').write_text(json.dumps(exported, indent=2) + '\n')
    # Retain the replaced payload for forensic inspection. No source, database,
    # successful measurements, or compiled object cache is removed.
    backup = '/home/ubuntu/bioseq-runtime-backup-' + str(int(time.time())) + '.tar.zst'
    subprocess.run(fleet.ssh(args.destination) + [
        'sudo -n tar -C / --ignore-failed-read --null --files-from=- -I "zstd -1 -T1" -cf ' + backup],
        input=('\0'.join(p.lstrip('/') for p in paths) + '\0').encode(), check=True, timeout=180)
    with (evidence / 'transfer.log').open('ab') as log:
        sender = subprocess.Popen(fleet.ssh(args.source) + [
            'nice -n 19 ionice -c 3 tar -C / --null --files-from=- -I "zstd -1 -T1" -cf -'],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log)
        receiver = subprocess.Popen(fleet.ssh(args.destination) + ['sudo -n tar -C / -I zstd -xf -'],
                                    stdin=sender.stdout, stdout=log, stderr=log)
        sender.stdout.close()
        sender.stdin.write(('\0'.join(p.lstrip('/') for p in paths) + '\0').encode())
        sender.stdin.close()
        received = receiver.wait(timeout=300)
        sent = sender.wait(timeout=60)
        assert not received and not sent, (sent, received)
    imported = rpc(fleet.ssh(args.destination) + [command], 'import_e2e_artifacts',
                   {'source_receipt': origin})
    imported.update(source=args.source, destination=args.destination, backup=backup)
    (evidence / 'import.json').write_text(json.dumps(imported, indent=2) + '\n')
    print(json.dumps(imported), flush=True)


if __name__ == '__main__':
    main()
