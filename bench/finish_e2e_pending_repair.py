#!/usr/bin/env python3
"""Finish one held-worker repair without interrupting its current build.

Wait for the exact current build, sync the reviewed local source patch batch,
perform one incremental MCP build in the same preset, then verify q1024 tuples.
No periodic agent polling, no blind rebuild retries, no result-validation bypass.
"""
import argparse
import fcntl
import hashlib
import json
import subprocess
import sys
import time

import manage_e2e_fleet as fleet


def observe(host):
    return fleet.monitor._remote_state(*fleet.HOSTS[host])


def wait_terminal(host, *, receipt=None, config=None):
    """Wait without touching a live build/benchmark; fail on changed ownership."""
    while True:
        state = observe(host)
        if state.get('status') in ('unreachable', 'unreadable'):
            time.sleep(60)
            continue
        if receipt and state.get('build_receipt_id') != receipt:
            raise RuntimeError('Build ownership changed: ' + str(state))
        if config and state.get('bench_e2e_config') != config:
            raise RuntimeError('Verification ownership changed: ' + str(state))
        if state.get('status') in fleet.TERMINAL_STATUSES:
            return state
        time.sleep(60)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True, choices=sorted(fleet.HOSTS))
    parser.add_argument('--wait-receipt', required=True)
    parser.add_argument('--preset', required=True)
    parser.add_argument('--source', required=True)
    parser.add_argument('--sha256', required=True)
    parser.add_argument('--extra-patch', action='append', default=[], metavar='PATH:SHA256')
    parser.add_argument('--verify-ordinal', required=True, type=int, action='append')
    parser.add_argument('--daemon', action='store_true')
    args = parser.parse_args()
    output = fleet.OUTPUT / ('pending_repair_' + args.host)
    output.mkdir(exist_ok=True)
    if args.daemon:
        with (output / 'worker.log').open('ab') as log:
            process = subprocess.Popen([sys.executable, __file__,
                *(v for v in sys.argv[1:] if v != '--daemon')], cwd=fleet.ROOT,
                stdin=subprocess.DEVNULL, stdout=log, stderr=log, start_new_session=True)
        print(json.dumps({'pid': process.pid, 'log': str(output / 'worker.log')}))
        return
    with (output / 'worker.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        source = fleet.ROOT / args.source
        assert source.is_relative_to(fleet.ROOT) and '..' not in source.parts
        maintenance = fleet.ROOT / 'bench/e2e_fleet_maintenance.json'
        hold = json.loads(maintenance.read_bytes())['hosts'][args.host]
        assert hold['paused'] and not hold.get('resume_after_build_receipt')
        fleet.event('pending_patch_waiting', host=args.host, receipt=args.wait_receipt)
        state = wait_terminal(args.host, receipt=args.wait_receipt)
        assert state['status'] == 'completed' and state['build_only'], state
        patches = [(args.source, args.sha256),
                   *(value.rsplit(':', 1) for value in args.extra_patch)]
        for path, digest in patches:
            reviewed = fleet.ROOT / path
            assert reviewed.is_relative_to(fleet.ROOT) and '..' not in reviewed.parts
            assert hashlib.sha256(reviewed.read_bytes()).hexdigest() == digest, 'Reviewed source changed: ' + path
        fleet.sync_files(args.host, tuple(path for path, _ in patches))
        log = fleet.mcp_start(args.host, {'mode': 'benchmark-e2e', 'build_only': True,
            'fresh': False, 'debug': False}, 'identity_repair', build_preset=args.preset)
        state = observe(args.host)
        receipt = state.get('build_receipt_id')
        assert receipt and receipt != args.wait_receipt and state.get('build_only'), state
        fleet.atomic(output / 'admitted.json', (json.dumps({
            'receipt': receipt, 'patches': patches,
            'log': log, 'verification_ordinals': args.verify_ordinal}, indent=2) + '\n').encode())
        fleet.event('pending_patch_building', host=args.host, receipt=receipt)
        state = wait_terminal(args.host, receipt=receipt)
        assert state['status'] == 'completed' and state['build_only'], state
        terminals = {}
        for ordinal in args.verify_ordinal:
            child = next(c for c in json.loads(fleet.MANIFEST.read_bytes())['children']
                         if c['ordinal'] == ordinal)
            # Hold remains active so the ordinary controller cannot double-dispatch.
            fleet.dispatch(args.host, receipt, child)
            fleet.event('pending_patch_verifying', host=args.host, ordinal=ordinal, receipt=receipt)
            terminal = wait_terminal(args.host, config=child['path'])
            assert not terminal.get('cleanup_unverified'), terminal
            assert fleet.collect(args.host, 'targeted_repair_' + receipt + '_' + str(ordinal))
            terminals[ordinal] = terminal['status']
        subprocess.run([sys.executable, 'bench/recover_e2e_measurements.py', '--publish',
                        '--host', args.host], cwd=fleet.ROOT, check=True, timeout=300)
        document = json.loads(maintenance.read_bytes())
        document['hosts'][args.host].update(paused=False, build_receipt_id=receipt,
            reason='Incremental support repair built; targeted q1024 output collected and audited')
        fleet.atomic(maintenance, (json.dumps(document, indent=2) + '\n').encode())
        # A failed tuple is never approved by this orchestration script. Resume
        # other useful work even if its separate numeric auditor rejects it.
        fleet.event('pending_patch_released', host=args.host, verification_status=terminals)
        fleet.append_progress_log(
            f'{args.host}: one incremental identity-support patch built as {receipt}; '
            f'q1024 verification terminal statuses {terminals}. '
            'Saved output independently audited; normal queue released. Build success alone is not numerical sign-off.',
            title='Targeted identity-support repair checkpoint')


if __name__ == '__main__':
    try:
        main()
    except Exception as exc:
        fleet.event('pending_patch_failed', detail=str(exc))
        fleet.append_progress_log(
            'Targeted pending repair stopped without releasing maintenance: ' + str(exc),
            title='Targeted repair needs attention')
        raise
