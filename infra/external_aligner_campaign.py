#!/usr/bin/env python3
"""Resume a durable per-host task plan using only that host's bioseq-build MCP."""
import argparse
from datetime import datetime,timezone
import json
import fcntl
from pathlib import Path
import subprocess
import time
from external_aligner_storage import ready as storage_ready

ROOT=Path(__file__).resolve().parents[1]


def call(machine,task):
    argv=['python3',str(ROOT/'infra/external_aligner_remote.py'),machine,task['action']]
    if task.get('tool'): argv+=['--tool',task['tool']]
    if task.get('config'): argv+=['--config',task['config']]
    reply=subprocess.run(argv,cwd=ROOT,capture_output=True,text=True,timeout=60)
    if reply.returncode: raise RuntimeError(reply.stderr)
    for line in reply.stdout.splitlines():
        value=json.loads(line)
        if value.get('id')==2:
            if 'error' in value or value['result'].get('isError'): raise RuntimeError(line)
            return json.loads(value['result']['content'][0]['text'])
    raise RuntimeError('No MCP tool result')


def matching(state,task):
    return state.get('action')==task['action'] and state.get('tool')==task.get('tool','blastn') and (
        not task.get('config') or Path(state.get('config') or '').name==task['config'])


def checkpoint(path,value):
    temporary=path.with_suffix('.tmp');temporary.write_text(json.dumps(value,indent=2));temporary.replace(path)


def hourly_log(machine,directory,progress,state):
    now=time.time()
    if now-progress.get('last_hourly_log',0)<3600:return
    log=ROOT/'DOCS/CURRENT/BENCHMARK_FRAMEWORK/0437_EXTERNAL_ALIGNER_E2E_AWS_BENCHMARK_EXECUTION_LOG.md'
    with log.open('a') as stream:
        fcntl.flock(stream,fcntl.LOCK_EX)
        stream.write('\n## '+datetime.now(timezone.utc).isoformat()+' — '+machine.upper()+' hourly checkpoint\n\n')
        stream.write('Completed controller tasks: '+str(progress['completed'])+'. Current worker: '+
            str(state.get('status'))+' '+str(state.get('action'))+' '+str(state.get('tool'))+'. ')
        if state.get('config'):stream.write('Config: `'+Path(state['config']).name+'`. ')
        stream.write('Only external tool work is scheduled. No timing is accepted until all audits and five q1024 repeats pass.\n\n')
        if state.get('tail'):stream.write('```text\n'+state['tail'][-1500:]+'\n```\n')
        stream.flush()
    progress['last_hourly_log']=now


def fetch(machine,state_dir):
    outputs={k:v['value'] for k,v in json.loads((state_dir/'terraform.tfstate').read_text())['outputs'].items()}
    assert json.loads(subprocess.check_output(['aws','sts','get-caller-identity']))['Account']=='090072209895'
    live=json.loads(subprocess.check_output(['aws','ec2','describe-instances','--region','us-east-1','--instance-ids',outputs['instance_id']]))['Reservations'][0]['Instances'][0]
    assert {t['Key']:t['Value'] for t in live['Tags']}['Name']=='bioseq-e2e-external-'+machine
    outputs['public_ip']=live['PublicIpAddress']
    name='ALL_RESULTS_TUPLES_E2E_EXTERNAL_REMOTE_'+machine.upper()+'.yaml'
    subprocess.run(['rsync','-a','-e','ssh -i '+outputs['ssh_key_path']+' -o StrictHostKeyChecking=yes',
        'ubuntu@'+outputs['public_ip']+':/home/ubuntu/bioseqdb/'+name,str(ROOT/name)],check=True)
    archive=state_dir/'evidence';archive.mkdir(exist_ok=True)
    subprocess.run(['rsync','-a','--prune-empty-dirs','--include=*/','--include=evidence.tar.gz','--exclude=*',
        '-e','ssh -i '+outputs['ssh_key_path']+' -o StrictHostKeyChecking=yes',
        'ubuntu@'+outputs['public_ip']+':/home/ubuntu/bioseqdb/bench/e2e_external/results/',str(archive)+'/'],check=True)
    fixtures=archive/'fixtures';fixtures.mkdir(exist_ok=True)
    subprocess.run(['rsync','-a','--prune-empty-dirs','--include=*/','--include=evidence.tar.gz','--exclude=*',
        '-e','ssh -i '+outputs['ssh_key_path']+' -o StrictHostKeyChecking=yes',
        'ubuntu@'+outputs['public_ip']+':/home/ubuntu/bioseqdb/bench/e2e_external/fixtures/',str(fixtures)+'/'],check=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('machine',choices=['m7i','m7a','m4','c7g'])
    args=parser.parse_args(); directory=ROOT/'infra/.external_aligner_state'/args.machine
    lock=(directory/'campaign_controller.lock').open('a')
    fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    plan=json.loads((directory/'campaign_plan.json').read_text()); status_path=directory/'campaign_progress.json'
    progress=json.loads(status_path.read_text()) if status_path.exists() else {'completed':0,'history':[]}
    while progress['completed']<len(plan):
        try:
            task=plan[progress['completed']]
            state=call(args.machine,{'action':'status'})
            hourly_log(args.machine,directory,progress,state)
            if state.get('status')=='running':
                progress.pop('error',None)
                progress['status']='waiting';progress['worker']=state;checkpoint(status_path,progress)
                time.sleep(300);continue
            if state.get('status')=='failed' and state.get('action')=='build':
                progress['status']='needs_attention';progress['worker']=state
                progress['error']=state.get('error','Build failed')+'\n'+state.get('tail','')
                checkpoint(status_path,progress);time.sleep(30);continue
            if not matching(state,task):
                if task['action']=='run' and not storage_ready(args.machine):
                    progress['status']='preparing_storage';checkpoint(status_path,progress)
                    time.sleep(30);continue
                state=call(args.machine,task)
                progress['status']='running';progress['worker']=state;checkpoint(status_path,progress)
                time.sleep(30);continue
            if state['status']!='completed':
                progress['status']='needs_attention';progress['worker']=state
                progress['error']=state.get('error','Worker failed')+'\n'+state.get('tail','')
                checkpoint(status_path,progress)
                # Keep hourly reporting alive, but never retry a failed measurement
                # without an explicit repair/new worker invocation.
                time.sleep(30);continue
            if task['action']=='publish':
                fetch(args.machine,directory)
                storage_ready(args.machine)
            progress['history'].append({'task':task,'worker':state,'recorded_at':datetime.now(timezone.utc).isoformat()})
            progress.pop('error',None)
            progress['completed']+=1;progress['status']='advancing';checkpoint(status_path,progress)
            print(json.dumps({'machine':args.machine,'completed':progress['completed'],'total':len(plan),'task':task}),flush=True)
        except Exception as error:
            # A lost SSH/MCP response may have started the worker. Re-read its
            # state before taking any next action; matching() safely adopts it.
            progress['status']='needs_attention';progress['error']=str(error)
            state={'status':'controller_error','action':'preflight/status/sync','tool':'external', 'tail':str(error)}
            hourly_log(args.machine,directory,progress,state)
            checkpoint(status_path,progress)
            print(json.dumps({'machine':args.machine,'controller_retry':str(error)}),flush=True)
            time.sleep(30)
    progress['status']='completed';checkpoint(status_path,progress)


if __name__=='__main__':main()
