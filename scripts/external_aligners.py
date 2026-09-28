"""Isolated, remote-only external benchmark worker exposed by bioseq-build MCP."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
STATE = ROOT / "bench/e2e_external/.state"
TOOLS = ["blastn", "mmseqs2", "winnowmap", "lexicmap", "harness"]


def sha256(path):
    value=hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda:stream.read(1024*1024),b''):value.update(chunk)
    return value.hexdigest()


def verify_source(tool):
    if tool=='harness':return {'harness_source':sha256(ROOT/'bench/e2e_external/index.cppm')}
    directory=ROOT/('bench/tool_'+tool)
    manifest=directory/'source_manifest.json'
    files=json.loads(manifest.read_text())['files']
    for relative,expected in files.items():
        path=(directory/'source'/relative).resolve()
        if not path.is_relative_to(directory/'source') or sha256(path)!=expected:
            raise ValueError('Remote source differs from local manifest: '+relative)
    return {'source_manifest_sha256':sha256(manifest),'verified_files':len(files)}


def tool_external_aligners(action="status", tool="blastn", config=None):
    if platform.system() != "Linux" or not Path("/opt/bioseq/PROVISION_STATUS").exists():
        raise RuntimeError("External work requires a provisioned Linux AWS benchmark host")
    if action not in ["build", "run", "status", "publish", "cancel", "recover"] or tool not in TOOLS:
        raise ValueError("Unknown external benchmark action or tool")
    STATE.mkdir(parents=True, exist_ok=True)
    state_path = STATE / "worker.json"
    with (STATE / "lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        current = json.loads(state_path.read_text()) if state_path.exists() else {}
        if action=='recover':
            boot_seconds=int(next(line.split()[1] for line in Path('/proc/stat').read_text().splitlines() if line.startswith('btime ')))
            if current.get('status')!='running':return json.dumps(current)
            if current.get('started_ns',time.time_ns())>=boot_seconds*1_000_000_000:
                raise ValueError('Recovery only permits workers interrupted by a proven host reboot')
            reason='Host reboot interrupted this invocation; partial repetitions are excluded and the entire cell must restart.'
            (STATE/('interrupted_worker_'+str(current['started_ns'])+'.json')).write_text(json.dumps(current,indent=2))
            if current.get('action')=='run':
                label=Path(current['config']).stem
                for report_path in (ROOT/'bench/e2e_external/results'/label).glob('*/report.json'):
                    report=json.loads(report_path.read_text())
                    if report.get('status')!='running':continue
                    subprocess.run(['sudo','-n',sys.executable,'-c',
                        "from pathlib import Path; import json,sys; p=Path(sys.argv[1]); "
                        "r=json.loads(p.read_text()); assert r['status']=='running'; "
                        "p.with_name('report.before_reboot_recovery.json').write_bytes(p.read_bytes()); "
                        "r.update(status='invalid',error=sys.argv[2]); t=p.with_suffix('.tmp'); "
                        "t.write_text(json.dumps(r,indent=2)); t.replace(p)",str(report_path),reason],check=True)
            current.update(status='failed',error=reason,finished_ns=time.time_ns())
            state_path.write_text(json.dumps(current))
            return json.dumps(current)
        if action=='cancel':
            if current.get('status')!='running' or current.get('action')!='run' or current.get('tool')!=tool:
                raise ValueError('Cancel requires the matching active external run')
            owner=current['pid']; descendants={owner}; processes={}
            for proc in Path('/proc').iterdir():
                if not proc.name.isdigit():continue
                try:
                    fields=(proc/'stat').read_text().rsplit(')',1)[1].split()
                    processes[int(proc.name)]=(int(fields[1]),int(fields[2]),fields[19])
                except (OSError,ValueError):pass
            while True:
                expanded=descendants|{pid for pid,(parent,group,start) in processes.items() if parent in descendants}
                if expanded==descendants:break
                descendants=expanded
            groups=set()
            for pid in descendants:
                try:
                    executable=Path((Path('/proc')/str(pid)/'cmdline').read_bytes().split(b'\0')[0].decode()).resolve()
                    if executable.is_relative_to(ROOT/('bench/tool_'+tool+'/bin')):
                        groups.add(processes[pid][1])
                except (OSError,ValueError):pass
            for group in groups:
                subprocess.run(['sudo','-n','kill','-TERM','--','-'+str(group)],check=True)
            return json.dumps({'cancel_requested':True,'owned_groups':sorted(groups),'worker_pid':owner})
        if action == "status" or current.get("status") == "running":
            if current.get("log"):
                current["tail"] = Path(current["log"]).read_text(errors="replace")[-6000:]
            return json.dumps(current)
        if action == "run":
            path = Path(config or "").resolve()
            if not path.is_relative_to(ROOT / "bench/e2e_external/configs") or not path.is_file():
                raise ValueError("Run requires an existing external-only config")
        stamp = str(time.time_ns())
        log_path = STATE / (stamp + ".log")
        current = {"status": "running", "action": action, "tool": tool,
                   "config": config, "started_ns": time.time_ns(), "log": str(log_path)}
        state_path.write_text(json.dumps(current))
        with log_path.open("w") as log:
            child = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), stamp],
                cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        current["pid"] = child.pid
        state_path.write_text(json.dumps(current))
        return json.dumps(current)


def worker():
    state_path = STATE / "worker.json"
    with (STATE / "lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        state = json.loads(state_path.read_text())
    try:
        if platform.system() != "Linux":
            raise RuntimeError("No local builds")
        if state["action"] == "build":
            source_receipt=verify_source(state['tool'])
            script = ROOT / ("bench/e2e_external/build.sh" if state["tool"] == "harness"
                             else "bench/tool_" + state["tool"] + "/build.sh")
            subprocess.run(["bash", str(script)], cwd=ROOT, check=True, timeout=14400)
            if state['tool']=='winnowmap':
                subprocess.run([sys.executable,str(ROOT/'bench/tool_winnowmap/test_index_roundtrip.py')],
                               cwd=ROOT,check=True,timeout=300)
            if state['tool']=='mmseqs2':
                subprocess.run([sys.executable,str(ROOT/'bench/tool_mmseqs2/test_diagonal_counter.py')],
                               cwd=ROOT,check=True,timeout=120)
            if state['tool']=='lexicmap':
                subprocess.run([sys.executable,str(ROOT/'bench/tool_lexicmap/test_reference_encoding.py')],
                               cwd=ROOT,check=True,timeout=240)
            directory=ROOT/('bench/e2e_external/build' if state['tool']=='harness' else 'bench/tool_'+state['tool']+'/bin')
            receipt={'tool':state['tool'],'source':source_receipt,'build_script_sha256':sha256(script),
                     'compiler':subprocess.check_output(['clang','--version'],text=True),
                     'artifacts':{str(p.relative_to(ROOT)):sha256(p) for p in directory.iterdir() if p.is_file() and os.access(p,os.X_OK)},
                     'log':state['log'],'log_sha256':sha256(state['log']),'finished_ns':time.time_ns()}
            receipt['seal_sha256']=hashlib.sha256(json.dumps(receipt,sort_keys=True,separators=(',',':')).encode()).hexdigest()
            (STATE/(state['tool']+'_build_receipt.json')).write_text(json.dumps(receipt,indent=2))
        elif state["action"] == "publish":
            subprocess.run(["sudo", "-n", sys.executable, str(ROOT / "bench/e2e_external/publish.py")],
                           cwd=ROOT, check=True, timeout=3600)
        else:
            subprocess.run(["sudo", "-n", str(ROOT / "bench/e2e_external/build/e2e_external"),
                "run", state["config"]], cwd=ROOT, check=True, timeout=43200)
        state["status"] = "completed"
    except Exception as error:
        state["status"] = "failed"
        state["error"] = str(error)
    state["finished_ns"] = time.time_ns()
    state_path.write_text(json.dumps(state))


if __name__ == "__main__":
    worker()
