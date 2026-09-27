#!/usr/bin/env python3
"""Drain the four explicitly authorized default-account workers; preserve evidence.

Never deletes AWS resources. Stops only these hosts' benchmark processes and
PostgreSQL, archives all reports/logs with content hashes, publishes through the
existing auditor, then removes the hosts from future polling and dispatch.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
import fcntl
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys
import tarfile
import time

import manage_e2e_fleet as fleet
from recover_e2e_measurements import ROOT, COLLECTED, MANIFEST, atomic, recover, authority_path
from e2e_tuple_progress import append_progress_log

HOSTS = tuple(h for h in fleet.monitor.BASE_HOSTS if h[0].startswith('default-'))
# Permit an explicit verification/retry even after normal scheduling omits them.
fleet.HOSTS.update({h[0]: h for h in HOSTS})
OUTPUT = ROOT / 'reporting/bioseq_benchmark_e2e/default_account_retirement_20260922'

# Include every report, not just the subset accepted by the current validator.
# Changed files are copied again; unchanged files retain their previous archive.
PACK = r'''
import pathlib,sys,json,tarfile,io,hashlib
known=json.load(sys.stdin)
root=pathlib.Path('/home/ubuntu/bioseqdb')
paths=list((root/'reporting/bioseq_benchmark_e2e').rglob('*'))
paths+=list(pathlib.Path('/tmp').glob('bioseq_build_mcp*'))
paths+=list(root.glob('ALL_RESULTS_TUPLES*.yaml'))
frozen=root.parent/'bioseq-frozen'
if frozen.exists():
 for pattern in ('report_*.json','report_*.raw.jsonl','*receipts*.json'):
  paths+=list(frozen.rglob(pattern))
inventory={}; changed=0
t=tarfile.open(fileobj=sys.stdout.buffer,mode='w|gz',compresslevel=1)
for p in sorted(set(paths)):
 if not p.is_file() or p.is_symlink(): continue
 before=p.stat(); name=str(p).lstrip('/')
 signature=[before.st_size,before.st_mtime_ns]
 if known.get(name,{}).get('signature')==signature:
  inventory[name]=known[name]; continue
 data=p.read_bytes(); after=p.stat()
 if (after.st_size,after.st_mtime_ns)!=(before.st_size,before.st_mtime_ns):
  raise RuntimeError('File changed during archive: '+str(p))
 item=tarfile.TarInfo(name); item.size=len(data); item.mtime=before.st_mtime
 t.addfile(item,io.BytesIO(data)); changed+=1
 inventory[name]={'signature':signature,'sha256':hashlib.sha256(data).hexdigest()}
data=json.dumps(inventory,sort_keys=True).encode()
item=tarfile.TarInfo('_retirement_inventory.json'); item.size=len(data)
t.addfile(item,io.BytesIO(data)); t.close()
'''

# /proc executable checks avoid signaling a reused PID. No broad pkill/killall.
STOP = r'''
import pathlib,json,os,signal,subprocess,time
p=pathlib.Path('/tmp/bioseq_build_mcp_state.json')
state=json.loads(p.read_bytes()).get('benchmark-e2e',{}) if p.exists() else {}
owned=[]
for item in pathlib.Path('/proc').iterdir():
 if not item.name.isdigit(): continue
 try: exe=os.readlink(item/'exe')
 except OSError: continue
 if ((exe.startswith('/home/ubuntu/bioseqdb/build/') and pathlib.Path(exe).name=='bioseq_benchmark_e2e')
     or pathlib.Path(exe).name=='minimap2'):
  owned.append((int(item.name),exe)); os.kill(int(item.name),signal.SIGTERM)
try:
 subprocess.run(['sudo','-n','systemctl','stop','postgresql@17-main'],check=True,timeout=30)
except subprocess.TimeoutExpired:
 # The account/database is being discarded. A long extension call may not
 # service fast-shutdown interrupts; stop this exact dedicated cluster.
 subprocess.run(['sudo','-n','pg_ctlcluster','--skip-systemctl-redirect',
                 '--mode','immediate','17','main','stop'],check=True,timeout=45)
# Immediate PostgreSQL shutdown can leave a stopped unit marked failed.
# Clear only the bookkeeping after systemd proves there is no main process.
main_pid=subprocess.check_output(['systemctl','show','postgresql@17-main',
                                 '--property=MainPID','--value'],text=True).strip()
if main_pid=='0':
 subprocess.run(['sudo','-n','systemctl','reset-failed','postgresql@17-main'],check=True)
for pid,exe in owned:
 try:
  if os.readlink('/proc/'+str(pid)+'/exe')==exe: os.kill(pid,signal.SIGKILL)
 except ProcessLookupError: pass
 except FileNotFoundError: pass
print(json.dumps({'signaled':owned,'previous_status':state.get('status'),
                  'interrupted_config':state.get('bench_e2e_config') if owned else None}))
'''

VERIFY = r'''
import pathlib,os,json,subprocess
live=[]
for item in pathlib.Path('/proc').iterdir():
 if not item.name.isdigit(): continue
 try: exe=os.readlink(item/'exe')
 except OSError: continue
 if ((exe.startswith('/home/ubuntu/bioseqdb/build/') and pathlib.Path(exe).name=='bioseq_benchmark_e2e')
     or pathlib.Path(exe).name=='minimap2'): live.append(int(item.name))
pg=subprocess.run(['systemctl','is-active','postgresql@17-main'],capture_output=True,text=True)
print(json.dumps({'benchmark_pids':live,'postgres_service':pg.stdout.strip()}))
if live or pg.stdout.strip()!='inactive': raise SystemExit(2)
'''


def remote(host, code, timeout=150):
    result = subprocess.run(fleet.ssh(host) + ['python3 -c ' + shlex.quote(code)],
                            capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(host + ': ' + result.stderr[-2000:] + result.stdout[-2000:])
    return json.loads(result.stdout)


def archive(host, phase):
    folder = OUTPUT / host
    folder.mkdir(parents=True, exist_ok=True)
    manifest = folder / 'inventory.json'
    known = json.loads(manifest.read_bytes()) if manifest.exists() else {}
    # Immutable names: a restarted retirement must not overwrite the archive
    # containing unchanged files omitted from its next incremental backup.
    target = COLLECTED / host / ('retirement_' + phase + '_' + str(time.time_ns()) + '.tar.gz')
    target.parent.mkdir(parents=True, exist_ok=True)
    temp = target.with_suffix('.partial')
    with temp.open('wb') as output:
        result = subprocess.run(fleet.ssh(host) + ['nice -n 19 python3 -c ' + shlex.quote(PACK)],
            input=json.dumps(known).encode(), stdout=output, stderr=subprocess.PIPE, timeout=900)
    if result.returncode:
        raise RuntimeError(host + ': ' + result.stderr.decode(errors='replace')[-2000:])
    with tarfile.open(temp) as source:
        inventory = json.load(source.extractfile('_retirement_inventory.json'))
        for member in source:
            if member.name not in inventory:
                continue
            digest = hashlib.file_digest(source.extractfile(member), 'sha256').hexdigest() if hasattr(hashlib, 'file_digest') else hashlib.sha256(source.extractfile(member).read()).hexdigest()
            if digest != inventory[member.name]['sha256']:
                raise RuntimeError('Archive checksum mismatch: ' + member.name)
    temp.replace(target)
    atomic(manifest, (json.dumps(inventory, indent=2) + '\n').encode())
    proof = {'archive': str(target.relative_to(ROOT)), 'sha256': hashlib.sha256(target.read_bytes()).hexdigest(),
             'bytes': target.stat().st_size, 'preserved_files': len(inventory), 'verified_at': datetime.now().astimezone().isoformat()}
    atomic(folder / (phase + '.json'), (json.dumps(proof, indent=2) + '\n').encode())
    print(json.dumps({'host': host, **proof}), flush=True)
    return proof


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stop-at', default='2026-09-22T13:00:00+02:00')
    args = parser.parse_args()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    with (OUTPUT / 'retirement.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        policy = ROOT / 'bench/e2e_fleet_retirement.json'
        document = json.loads(policy.read_bytes())
        assert document['account_id'] == '970376923108'
        assert all(document['hosts'][h[0]]['draining'] for h in HOSTS)
        children = {c['stable_key']: c for c in json.loads(MANIFEST.read_bytes())['children']}
        # Preserve the original running tuple across a retry after partial stop.
        for h in HOSTS:
            original = COLLECTED / h[0] / 'retirement_initial.tar.gz'
            if original.exists():
                with tarfile.open(original) as archive_file:
                    entry = json.load(archive_file.extractfile('tmp/bioseq_build_mcp_state.json')).get('benchmark-e2e', {})
                if entry.get('status') == 'running':
                    document['hosts'][h[0]]['interrupted_stable_key'] = next(
                        (c['stable_key'] for c in children.values() if c['path'] == entry.get('bench_e2e_config')), None)
        with ThreadPoolExecutor(max_workers=4) as pool:
            futures = [pool.submit(archive, h[0], 'initial') for h in HOSTS]
            for future in futures:
                try: future.result()
                except Exception as exc: print('Initial archive will be retried after stop:', str(exc), flush=True)
        stop_at = datetime.fromisoformat(args.stop_at).timestamp()
        while time.time() < stop_at:
            time.sleep(min(60, stop_at - time.time()))
        with ThreadPoolExecutor(max_workers=4) as pool:
            stopped = dict(zip([h[0] for h in HOSTS], pool.map(lambda h: remote(h[0], STOP), HOSTS)))
        # The MCP worker can finish its status/log writes after its child exits.
        time.sleep(20)
        proofs = {}
        for h in HOSTS:
            host = h[0]
            proofs[host] = {'stop': stopped[host], 'idle': remote(host, VERIFY)}
            proofs[host]['archive'] = archive(host, 'final')
            if not fleet.collect(host, 'retirement_final'):
                raise RuntimeError('Final ordinary report collection failed: ' + host)
        with (COLLECTED / '.recovery.lock').open('a') as audit_lock:
            fcntl.flock(audit_lock, fcntl.LOCK_EX)
            for h in HOSTS:
                host = h[0]
                result = recover(COLLECTED / host, children, True)
                proofs[host]['audit'] = {k: v for k, v in result.items() if k != 'dispositions'}
                proofs[host]['authority'] = str(authority_path(host).relative_to(ROOT))
                proofs[host]['authority_sha256'] = hashlib.sha256(authority_path(host).read_bytes()).hexdigest()
                proofs[host]['idle_final'] = remote(host, VERIFY)
                document['hosts'][host]['retired'] = True
                interrupted = stopped[host].get('interrupted_config')
                if interrupted:
                    document['hosts'][host]['interrupted_stable_key'] = next(
                        (c['stable_key'] for c in children.values() if c['path'] == interrupted), None)
        atomic(policy, (json.dumps(document, indent=2) + '\n').encode())
        atomic(OUTPUT / 'completed.json', (json.dumps({'completed_at': datetime.now().astimezone().isoformat(), 'hosts': proofs}, indent=2) + '\n').encode())
        append_progress_log('- Default account 970376923108 drained before 13:30 CEST deletion. All four benchmark executables absent and PostgreSQL services stopped. All report/log files saved with checked SHA-256 inventories; existing validators published every accepted complete q1024 tuple to local remote YAML authorities. Invalid/incomplete reports retained as evidence, not approved measurements. No AWS resources deleted by this operation. Other accounts and local remain scheduled.', title='default AWS account retirement complete')
        print('RETIREMENT COMPLETE', flush=True)


if __name__ == '__main__':
    main()
