#!/usr/bin/env python3
"""Restart failed queue controllers without cancelling their MCP-owned jobs."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
QUEUES = {'fast_remote': 'fast_remote_e2e.py', 'fast_local': 'fast_local_e2e.py'}


def start(queue):
    directory = ROOT / 'reporting/bioseq_benchmark_e2e' / queue
    directory.mkdir(parents=True, exist_ok=True)
    with (directory / 'supervisor.log').open('ab') as log:
        process = subprocess.Popen([sys.executable, __file__, queue], cwd=ROOT,
            stdin=subprocess.DEVNULL, stdout=log, stderr=log, start_new_session=True)
    return process.pid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('queue', choices=QUEUES)
    args = parser.parse_args()
    directory = ROOT / 'reporting/bioseq_benchmark_e2e' / args.queue
    with (directory / 'supervisor.lock').open('a') as owner:
        try:
            fcntl.flock(owner, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return
        from recover_e2e_measurements import atomic
        atomic(directory / 'supervisor.pid', (str(os.getpid()) + '\n').encode())
        child = None
        while True:
            try:
                if args.queue == 'fast_local':
                    maintenance = json.loads((ROOT / 'bench/e2e_fleet_maintenance.json').read_bytes())
                    if maintenance['hosts']['local'].get('owner') == 'package_release_fleet':
                        if child is not None and child.poll() is None:
                            child.terminate()  # Controller only; never its detached MCP job.
                        return
                if child is not None:
                    code = child.poll()
                    if code is None:
                        time.sleep(60)
                        continue
                    child = None
                    if code == 0:  # A normally exhausted local queue stays done.
                        return
                # Adopt an existing controller by its lock, not a stale PID.
                with (directory / 'controller.lock').open('a') as lock:
                    try:
                        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    except BlockingIOError:
                        time.sleep(60)
                        continue
                if args.queue == 'fast_local':
                    state = json.loads((directory.parent / 'fast_remote/state.json').read_bytes())
                    if 'local' not in state.get('ready', {}):
                        time.sleep(60)
                        continue
                with (directory / 'controller.log').open('ab') as log:
                    child = subprocess.Popen([sys.executable, str(ROOT / 'bench' / QUEUES[args.queue])],
                        cwd=ROOT, stdin=subprocess.DEVNULL, stdout=log, stderr=log)
                print(time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                      'controller_started', args.queue, child.pid, flush=True)
            except (OSError, ValueError) as exc:
                # In particular, ENOSPC must not kill the supervisor too.
                try:
                    print('supervisor_retry', str(exc), flush=True)
                except OSError:
                    pass
            time.sleep(60)


if __name__ == '__main__':
    main()
