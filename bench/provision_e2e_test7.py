#!/usr/bin/env python3
"""Idempotently launch the four user-requested test7 replicas within EC2 quota.

Copies original instance/volume specs, not mutable databases or credentials.
Runtime artifacts are transferred separately and benchmark admission uses MCP.
"""
import json
import argparse
from functools import partial
from pathlib import Path
import subprocess
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
STATE = ROOT / 'reporting/bioseq_benchmark_e2e/test7_provisioning.json'
SOURCES = {'c7g': 'i-0cab5f0ab9a1cb002', 'm4': 'i-09b8f0b9dc8c60564',
           'm7a': 'i-0c724c456e6a87366', 'm7i': 'i-0e647495fa0691f94'}
ACCOUNTS = {
    'test7': ('872590157365', 'vpc-0c049873c8cd2be05', 'subnet-0711b3bee309e2ba4', tuple(SOURCES)),
    'test2': ('090072209895', 'vpc-0b8c38283f256e963', 'subnet-0f066472995586f5d', ('m7a', 'm7i')),
}


def state_path(profile):
    return ROOT / f'reporting/bioseq_benchmark_e2e/{profile}_provisioning.json'


def aws(service, operation, *args, profile='test7'):
    return json.loads(subprocess.check_output(
        ['aws', '--profile', profile, '--region', 'us-east-1', service, operation,
         *args, '--output', 'json'], text=True) or '{}')


def main(profile='test7'):
    account, vpc, subnet, families = ACCOUNTS[profile]
    api = partial(aws, profile=profile)
    path = state_path(profile)
    assert api('sts', 'get-caller-identity')['Account'] == account
    state = json.loads(path.read_bytes()) if path.exists() else {'account': account, 'hosts': {}}
    group_name = 'bioseq-e2e-' + profile
    key_name = 'bioseq-e2e-replicas' if profile == 'test7' else 'bioseq-e2e-test2-replicas'
    groups = api('ec2', 'describe-security-groups', '--filters', 'Name=group-name,Values=' + group_name)['SecurityGroups']
    if groups:
        group = groups[0]['GroupId']
    else:
        group = api('ec2', 'create-security-group', '--group-name', group_name,
                    '--description', 'User-requested q1024 fleet replicas', '--vpc-id', vpc)['GroupId']
        ip = urllib.request.urlopen('https://checkip.amazonaws.com', timeout=20).read().decode().strip()
        import ipaddress
        ipaddress.IPv4Address(ip)
        api('ec2', 'authorize-security-group-ingress', '--group-id', group,
            '--protocol', 'tcp', '--port', '22', '--cidr', ip + '/32')
    keys = api('ec2', 'describe-key-pairs')['KeyPairs']
    if not any(k['KeyName'] == key_name for k in keys):
        public = subprocess.check_output(['ssh-keygen', '-y', '-f',
            str(ROOT / 'infra/testing_infra/.secrets/bioseq-bench-aws.pem')], text=True).strip()
        api('ec2', 'import-key-pair', '--key-name', key_name,
            '--public-key-material', public, '--cli-binary-format', 'raw-in-base64-out')
    quota = api('service-quotas', 'get-service-quota', '--service-code', 'ec2',
                '--quota-code', 'L-1216C47A')['Quota']['Value']
    # Existing instances (including external benchmarks) are observed only to
    # calculate spare quota. Never start/stop/tag/sync or modify any of them.
    existing = [i for r in api('ec2', 'describe-instances')['Reservations'] for i in r['Instances']
                if i['State']['Name'] in ('pending', 'running')]
    used = sum(i.get('CpuOptions', {}).get('CoreCount', 0) * i.get('CpuOptions', {}).get('ThreadsPerCore', 1) for i in existing)
    for family in families:
        source = SOURCES[family]
        if family in state['hosts']:
            continue
        if used + 4 > quota:
            print(f'{family}: pending quota ({used}/{quota:g} vCPUs occupied)', flush=True)
            continue
        blueprint_path = ROOT / 'bench/e2e_original_machine_blueprints.json'
        blueprint = json.loads(blueprint_path.read_bytes())['families'][family] if blueprint_path.exists() else None
        original = blueprint['instance'] if blueprint else aws('ec2', 'describe-instances', '--instance-ids', source, profile='default')['Reservations'][0]['Instances'][0]
        mapping = original['BlockDeviceMappings'][0]
        volume = blueprint['volume'] if blueprint else aws('ec2', 'describe-volumes', '--volume-ids', mapping['Ebs']['VolumeId'], profile='default')['Volumes'][0]
        blocks = [{'DeviceName': mapping['DeviceName'], 'Ebs': {
            'VolumeSize': volume['Size'], 'VolumeType': volume['VolumeType'], 'Iops': volume['Iops'],
            'Throughput': volume['Throughput'], 'Encrypted': True, 'DeleteOnTermination': False}}]
        tags = [{'ResourceType': 'instance', 'Tags': [{'Key': 'Name', 'Value': 'bioseq-e2e-' + profile + '-' + family},
                  {'Key': 'BlueprintInstance', 'Value': source}, {'Key': 'Purpose', 'Value': 'bioseq-q1024'}]}]
        launched = api('ec2', 'run-instances', '--image-id', original['ImageId'],
            '--instance-type', original['InstanceType'], '--key-name', key_name,
            '--subnet-id', subnet, '--security-group-ids', group,
            '--associate-public-ip-address', '--block-device-mappings', json.dumps(blocks),
            '--metadata-options', 'HttpTokens=required,HttpEndpoint=enabled,HttpPutResponseHopLimit=2,InstanceMetadataTags=enabled',
            '--tag-specifications', json.dumps(tags), '--client-token', 'bioseq-' + profile + '-20260921-' + family,
            '--user-data', 'file://' + str(ROOT / 'infra/package_build_infra/scripts/clone_e2e_runtime_bootstrap.sh'))['Instances'][0]
        state['hosts'][family] = {'instance_id': launched['InstanceId'], 'source_instance': source,
            'instance_type': original['InstanceType'], 'image_id': original['ImageId'], 'block_devices': blocks}
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_suffix('.tmp')
        temporary.write_text(json.dumps(state, indent=2) + '\n'); temporary.replace(path)
        used += 4
        print(f"{family}: launched {launched['InstanceId']} ({original['InstanceType']})", flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', choices=ACCOUNTS, default='test7')
    main(parser.parse_args().profile)
