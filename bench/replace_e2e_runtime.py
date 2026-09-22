#!/usr/bin/env python3
"""Replace an idle, maintenance-held worker's frozen payload from its own family.

Copy only the MCP-exported build-tree payload; keep host configuration, source,
databases and results untouched. Validate through MCP before queue admission.
The caller releases maintenance with the returned import receipt explicitly.
"""
import argparse
import json
from pathlib import Path
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
    assert state['status'] in fleet.TERMINAL_STATUSES and not state.get('cleanup_unverified'), state
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
