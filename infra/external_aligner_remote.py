#!/usr/bin/env python3
"""Call the real per-host bioseq-build MCP, with an account/instance preflight."""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('machine', choices=['m7i','m7a','m4','c7g'])
    parser.add_argument('action', choices=['status','build','run','publish','cancel','recover'])
    parser.add_argument('--tool', default='blastn')
    parser.add_argument('--config')
    args = parser.parse_args()
    identity = json.loads(subprocess.check_output(['aws','sts','get-caller-identity']))
    assert identity['Account'] == '090072209895', identity['Account']
    state = Path(__file__).resolve().parent / '.external_aligner_state' / args.machine
    outputs = {k:v['value'] for k,v in json.loads((state/'terraform.tfstate').read_text())['outputs'].items()}
    live = json.loads(subprocess.check_output(['aws','ec2','describe-instances','--region','us-east-1','--instance-ids',outputs['instance_id']]))['Reservations'][0]['Instances'][0]
    assert live['State']['Name']=='running'
    assert {t['Key']:t['Value'] for t in live['Tags']}['Name']=='bioseq-e2e-external-'+args.machine
    ssh = ['ssh','-i',outputs['ssh_key_path'],'-o','StrictHostKeyChecking=yes',
        'ubuntu@'+live['PublicIpAddress'],
        'cd /home/ubuntu/bioseqdb && BIOSEQ_DOCS_AUTOSTART=0 python3 build_mcp/server.py']
    arguments = {'action':args.action,'tool':args.tool}
    if args.config:
        arguments['config']='/home/ubuntu/bioseqdb/bench/e2e_external/configs/'+args.config
    messages = [
        {'jsonrpc':'2.0','id':1,'method':'initialize','params':{'protocolVersion':'2024-11-05','capabilities':{},'clientInfo':{'name':'external-benchmark-controller','version':'1'}}},
        {'jsonrpc':'2.0','method':'notifications/initialized'},
        {'jsonrpc':'2.0','id':2,'method':'tools/call','params':{'name':'external_aligners','arguments':arguments}},
    ]
    result = subprocess.run(ssh,input=''.join(json.dumps(m)+'\n' for m in messages),text=True,capture_output=True,timeout=45)
    print(result.stdout)
    if result.returncode:
        raise SystemExit(result.stderr)


if __name__=='__main__':
    main()
