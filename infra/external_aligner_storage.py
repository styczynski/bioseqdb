#!/usr/bin/env python3
"""Match baseline EBS throughput on an idle, account-bound external benchmark host."""
import json
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]


def aws(*args):return json.loads(subprocess.check_output(['aws',*args]))


def ready(machine):
    assert aws('sts','get-caller-identity')['Account']=='090072209895'
    directory=ROOT/'infra/.external_aligner_state'/machine
    outputs={k:v['value'] for k,v in json.loads((directory/'terraform.tfstate').read_text())['outputs'].items()}
    instance=aws('ec2','describe-instances','--region','us-east-1','--instance-ids',outputs['instance_id'])['Reservations'][0]['Instances'][0]
    assert {t['Key']:t['Value'] for t in instance['Tags']}['Name']=='bioseq-e2e-external-'+machine
    outputs['public_ip']=instance['PublicIpAddress']
    volume_id=instance['BlockDeviceMappings'][0]['Ebs']['VolumeId']
    volumes=aws('ec2','describe-volumes','--region','us-east-1','--volume-ids',volume_id)['Volumes']
    volume=volumes[0]
    modifications=aws('ec2','describe-volumes-modifications','--region','us-east-1','--filters','Name=volume-id,Values='+volume_id)['VolumesModifications']
    if modifications and modifications[0]['ModificationState'] not in ['completed','failed']:return False
    if volume['Iops']!=6000 or volume['Throughput']!=500:
        aws('ec2','modify-volume','--region','us-east-1','--volume-id',volume_id,'--iops','6000','--throughput','500')
        return False
    binding=json.loads((directory/'host_binding.json').read_text())
    binding['storage']={k:volume[k] for k in ['VolumeId','Size','VolumeType','Iops','Throughput','Encrypted']}
    (directory/'host_binding.json').write_text(json.dumps(binding,indent=2))
    ssh='ssh -i '+outputs['ssh_key_path']+' -o StrictHostKeyChecking=yes'
    subprocess.run(['rsync','-a','-e',ssh,str(directory/'host_binding.json'),
        'ubuntu@'+outputs['public_ip']+':/home/ubuntu/bioseqdb/bench/e2e_external/host_binding.json'],check=True)
    files=['bench/e2e_external/campaign.py','bench/e2e_external/publish.py','infra/external_aligner_memory.sh']
    files += ['bench/tool_'+tool+'/adapter.py' for tool in ['blastn','mmseqs2','winnowmap','lexicmap']]
    files += ['bench/tool_mmseqs2/single_index_adapter.py','bench/e2e_external/test_audit.py']
    files += ['bench/tool_mmseqs2/nucleotide_baseline_dna5.out']
    files += ['bench/tool_lexicmap/test_reference_encoding.py','build_mcp/external_aligners.py']
    subprocess.run(['rsync','-a','--relative','-e',ssh,*files,
        'ubuntu@'+outputs['public_ip']+':/home/ubuntu/bioseqdb/'],cwd=ROOT,check=True)
    subprocess.run(['ssh','-i',outputs['ssh_key_path'],'-o','StrictHostKeyChecking=yes',
        'ubuntu@'+outputs['public_ip'],
        'sudo -n bash /home/ubuntu/bioseqdb/infra/external_aligner_memory.sh'],check=True)
    return True


if __name__=='__main__':print(json.dumps({'ready':ready(sys.argv[1])}))
