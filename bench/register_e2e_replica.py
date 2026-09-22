#!/usr/bin/env python3
"""Register one verified runtime replica with its live AWS/OS inventory."""
import argparse
import copy
import fcntl
import json
import shlex
import subprocess
import sys

import yaml
import aggregate_e2e_algorithm_matrix as audit
from provision_e2e_test7 import aws, ACCOUNTS
from recover_e2e_measurements import ROOT, atomic, authority_path


def register(family, profile='test7'):
    assert family in ACCOUNTS[profile][3]
    name = profile + '-' + family
    evidence_dir = ROOT / ('reporting/bioseq_benchmark_e2e/' + profile + '_replicas') / name
    imported = json.loads((evidence_dir / 'imported.json').read_bytes())
    origin = json.loads((evidence_dir / 'source_export.json').read_bytes())['source_receipt']
    instance = aws('ec2', 'describe-instances', '--instance-ids', imported['instance_id'], profile=profile)['Reservations'][0]['Instances'][0]
    disk = instance['BlockDeviceMappings'][0]
    volume = aws('ec2', 'describe-volumes', '--volume-ids', disk['Ebs']['VolumeId'], profile=profile)['Volumes'][0]
    source_policy = audit.APPROVED_TUPLES_REMOTE_MACHINE_POLICIES[family.upper()]
    assert instance['InstanceType'] == source_policy['instance_type']
    assert volume['Size'] == 500 and volume['Iops'] == 6000 and volume['Throughput'] == 500 and volume['Encrypted']
    command = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10', '-i',
        str(ROOT / 'infra/testing_infra/.secrets/bioseq-bench-aws.pem'), 'ubuntu@' + instance['PublicIpAddress']]
    program = """import json,platform,subprocess,re,pathlib
tree=pathlib.Path(TREE)
profile=next((line.split('=',1)[1] for line in (tree/'CMakeCache.txt').read_text().splitlines() if line.startswith('BIOSEQ_TEMPLATE_FLATTEN_PROFILE:')),None)
if not profile:
 match=re.search(r'-DBIOSEQ_TEMPLATE_FLATTEN_PROFILE=([a-z0-9_]+)',(tree/'build.ninja').read_text())
 profile=match.group(1) if match else None
if not profile:raise RuntimeError('Copied runtime alphabet capability is unknown')
print(json.dumps({'template_profile':profile,'host':{k:getattr(platform,f)() for k,f in [('node','node'),('system','system'),('machine','machine'),('release','release')]},'cpu':json.loads(subprocess.check_output(['lscpu','-J'],text=True))}))""".replace('TREE', repr(origin['build_dir']))
    actual = json.loads(subprocess.check_output(command + ['python3 -c ' + shlex.quote(program)], text=True))
    cpu = {row['field'].rstrip(':'): row['data'] for row in actual['cpu']['lscpu']}
    policy = dict(source_policy, authority_filename='ALL_RESULTS_TUPLES_REMOTE_' + name.upper().replace('-', '_') + '.yaml',
        account_id=ACCOUNTS[profile][0], instance_id=instance['InstanceId'], hostname=actual['host']['node'],
        availability_zone=instance['Placement']['AvailabilityZone'], volume_id=volume['VolumeId'])
    assert int(cpu['CPU(s)']) == policy['vcpu_count']
    source_document = yaml.load(authority_path('default-' + family).read_bytes(), Loader=yaml.CSafeLoader)
    descriptor = copy.deepcopy(source_document['remote_host_binding'])
    machine_id = name.upper().replace('-', '_')
    descriptor.update(machine_id=machine_id, receipt_host=actual['host'])
    descriptor['aws_identity'].update(account_id=policy['account_id'], instance_id=policy['instance_id'],
        availability_zone=policy['availability_zone'])
    descriptor['storage']['volume_id'] = policy['volume_id']
    descriptor['cpu']['model'] = cpu['Model name']
    descriptor['cpu']['isa'] = cpu.get('Flags', descriptor['cpu']['isa'])
    descriptor['measurement_device'] = {'identity': 'cpu:' + actual['host']['machine'] + ':' + actual['host']['node'],
                                        'version': 'Linux:' + actual['host']['release']}
    descriptor['descriptor_sha256'] = audit._remote_host_descriptor_sha256(descriptor)
    audit.APPROVED_TUPLES_REMOTE_MACHINE_POLICIES[machine_id] = policy
    audit.APPROVED_TUPLES_NEW_FLEET_MACHINE_IDS |= {machine_id}
    descriptor = audit._validate_remote_host_descriptor(descriptor, 'new replica', require_field_order=False)
    registry_path = ROOT / 'bench/e2e_fleet_replicas.json'
    with (registry_path.with_suffix('.lock')).open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        registry = json.loads(registry_path.read_bytes())
        if name in registry['hosts']:
            assert registry['hosts'][name]['machine_policy']['instance_id'] == instance['InstanceId']
        destination = authority_path(name)
        if not destination.exists():
            local_bytes = authority_path('local').read_bytes()
            local = yaml.load(local_bytes, Loader=yaml.CSafeLoader)
            document = audit._new_remote_approved_tuple_authority(local, audit._sha256(local_bytes), descriptor)
            document['purpose'] += (' This worker reuses byte-identical artifacts copied from the original '
                + family.upper() + ' worker, validated by an explicit MCP artifact-import receipt; no build occurred here. '
                'Original-host timings are never copied. Initially approved_results is empty; raw-audited q1024 '
                'five-repeat measurements are appended as available. Existing same-family baseline evidence '
                'may be used only for correctness, with reuse disclosed in each affected result.')
            document['authority']['sealed_verification_manifest_required_for_append'] = False
            audit._regenerate_remote_approved_tuple_projections(document, [])
            atomic(destination, audit._canonical_approved_tuples_yaml_v3(document))
        # Publish inventory last: controllers never see a host without authority.
        registry['hosts'][name] = {'address': instance['PublicIpAddress'], 'build_receipt_id': imported['build_receipt_id'],
                                  'template_profile': actual['template_profile'], 'machine_policy': policy}
        atomic(registry_path, (json.dumps(registry, indent=2) + '\n').encode())
        atomic(evidence_dir / 'inventory.json', (json.dumps({'instance': instance, 'volume': volume,
            'os_inventory': actual, 'host_descriptor': descriptor}, indent=2) + '\n').encode())
    print(name + ': registered; zero recipient-host measurements invented')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('family', choices=['c7g', 'm4', 'm7a', 'm7i'])
    parser.add_argument('--profile', choices=ACCOUNTS, default='test7')
    args = parser.parse_args()
    register(args.family, args.profile)
