#!/usr/bin/env python3
"""Low-frequency, single-owner completion of the four authorized test7 hosts.

Waits for AWS quota approval; never substitutes instance families or builds.
Stops once all four original-family replicas have been imported and registered.
"""
import fcntl
import json
from pathlib import Path
import subprocess
import sys
import time

from provision_e2e_test7 import main as provision, aws, STATE, SOURCES
from recover_e2e_measurements import ROOT
from register_e2e_replica import register


def main():
    output = ROOT / 'reporting/bioseq_benchmark_e2e/test7_replicas'
    output.mkdir(parents=True, exist_ok=True)
    lock = (output / 'completion.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    while True:
        try:
            registry = json.loads((ROOT / 'bench/e2e_fleet_replicas.json').read_bytes())['hosts']
            if all('test7-' + family in registry for family in SOURCES):
                print('All four test7 families registered; completion watcher exits.', flush=True)
                return
            print(time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'checking quota/replica readiness', flush=True)
            provision()
            state = json.loads(STATE.read_bytes())
            for family, row in state['hosts'].items():
                if 'test7-' + family in registry:
                    continue
                evidence = output / ('test7-' + family) / 'imported.json'
                if not evidence.exists():
                    instance = aws('ec2', 'describe-instances', '--instance-ids', row['instance_id'])['Reservations'][0]['Instances'][0]
                    if instance['State']['Name'] != 'running' or not instance.get('PublicIpAddress'):
                        continue
                    ready = subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10',
                        '-o', 'StrictHostKeyChecking=accept-new', '-i',
                        str(ROOT / 'infra/testing_infra/.secrets/bioseq-bench-aws.pem'),
                        'ubuntu@' + instance['PublicIpAddress'], 'test -f /var/lib/bioseq-e2e-replica-bootstrap.ready'], timeout=20)
                    if ready.returncode:
                        continue
                    subprocess.run([sys.executable, 'bench/replicate_e2e_runtime.py', family], cwd=ROOT,
                                   check=True, timeout=5400)
                register(family)
        except Exception as exc:
            print('Completion check failed:', str(exc), flush=True)
        time.sleep(300)


if __name__ == '__main__':
    main()
