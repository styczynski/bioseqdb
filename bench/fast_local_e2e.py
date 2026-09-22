#!/usr/bin/env python3
"""Local-only, one-sample q1024 queue retaining compatible persisted indexes.

Uses the already-built E2E executable exclusively through bioseq-build MCP.
Standard local dispatch must remain paused. Remote queues are not modified.
The separate YAML keeps the existing approved-row fields and explicitly labels
its different sampling methodology. Failed samples never become timings.
"""
import argparse
import copy
import fcntl
import json
import os
from pathlib import Path
import subprocess
import sys
import time

import yaml
import aggregate_e2e_algorithm_matrix as audit
import manage_e2e_fleet as fleet
import recover_e2e_measurements as recovery

ROOT = recovery.ROOT
OUTPUT = ROOT / 'reporting/bioseq_benchmark_e2e/fast_local'
CONFIGS = ROOT / 'bench/e2e_fast_local'
RESULTS = ROOT / 'ALL_RESULTS_TUPLES_FAST_LOCAL.yaml'
STATE = OUTPUT / 'state.json'


def load_yaml(path):
    return yaml.load(path.read_bytes(), Loader=recovery.AuthorityLoader)


def event(kind, **fields):
    row = {'timestamp_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
           'event': kind, **fields}
    with (OUTPUT / 'events.jsonl').open('a') as out:
        out.write(json.dumps(row) + '\n')
    print(json.dumps(row), flush=True)


def priority(child):
    """Exhaust each compatible index group before changing its dataset/producer."""
    i = child['execution_cell']['identity']
    seed = i['requested_minimizer_algorithm'] or ''
    chain = i['requested_chaining_algorithm'] or ''
    align = i['requested_base_alignment_algorithm'] or ''
    return (i['index_mode'] != 'just_index',
            i['bioseq_index_name'] != 'minimizers_per_fragment',
            'bounded' not in seed, 'vulkan' in seed, seed,
            i['kmer_size'], i['window_size'], i['target_size_gbp'], i['alphabet'],
            {'simd_minimap2': 0, 'range_minimum_query': 1}.get(chain, 2),
            'vulkan' in chain, chain,
            {'block_aligner': 0, 'ksw2': 1}.get(align, 2), child['ordinal'])


def document():
    if RESULTS.exists():
        return load_yaml(RESULTS)
    standard = load_yaml(ROOT / 'ALL_RESULTS_TUPLES.yaml')
    # Reuse existing descriptive/identity fields, but do not carry historical
    # counters or historical preservation assertions into this new authority.
    out = {key: copy.deepcopy(standard[key]) for key in (
        'schema', 'version', 'status', 'authority', 'campaign_identity',
        'stable_key_contract', 'terminology', 'benchmark_host_profile', 'matrix_scope')}
    out['purpose'] = (
        'Self-contained authority for FAST LOCAL q1024 E2E measurements authorized '
        '2026-09-22. One sample per previously unapproved local tuple; P50 equals '
        'that single observation and is not a five-repeat median estimate. '
        'All just_index groups precede no_index. A compatible persisted index '
        'is reused across chaining/alignment configurations for the same target '
        'content, alphabet, index type, producer and k/w. PostgreSQL restarts before '
        'each measurement; OS filesystem cache is not cleared. Index creation, '
        'restart and post-measurement validation are untimed. Original five-repeat '
        'results remain separate and unchanged. Existing approved-row fields are '
        'preserved; run_count=1, timing_scope and comments identify this methodology. '
        'approved_results is the sole numerical authority; no external files are '
        'needed to interpret it. The standard five-repeat document validator must '
        'not be used to relabel these rows as standard measurements.')
    out['authority']['full_matrix_complete'] = False
    out['authority']['sealed_verification_manifest_required_for_append'] = False
    out['approved_results'] = []
    return out


def write_document(out):
    standard = load_yaml(ROOT / 'ALL_RESULTS_TUPLES.yaml')
    excluded = {row['stable_key'] for row in standard['approved_results']}
    out['matrix_scope']['repetitions_per_cell'] = 1
    out['matrix_scope']['total_expected_cells'] = standard['matrix_scope']['total_expected_cells'] - len(excluded)
    out['matrix_scope']['total_expected_runs'] = out['matrix_scope']['total_expected_cells']
    out['matrix_scope']['projection_description'] = (
        'Full allowed dimensions; expected cells/runs exclude already approved standard-local tuples. '
        'One sample is planned per remaining logical tuple. Failed attempts do not count as approved.')
    metrics = copy.deepcopy(standard['metric_definitions'])
    metrics['a']['aggregation'] = 'One accepted sample; P50 equals its value.'
    metrics['a']['per_repeat_definition'] = 'Framework measured-iteration window for mapping with CIGAR and MAPQ.'
    metrics['b']['aggregation'] = 'One accepted sample after subtracting its CIGAR/MAPQ interval union.'
    metrics['p50_method'] = 'Single-sample identity, not a statistical five-repeat estimate.'
    metrics['clock_semantics']['framework_outer'] = 'Monotonic duration of the one framework measured-iteration window.'
    metrics['timing_scope'] = {
        'allowed_values': [audit.FAST_LOCAL_TIMING_SCOPE],
        'classification': 'non_metric_observation_scope',
        'p50_partition_rule': 'FAST LOCAL one-sample methodology; never pool with standard five-repeat rows.'}
    out['metric_definitions'] = metrics
    out['validity_rules'] = copy.deepcopy(standard['validity_rules'])
    out['validity_rules']['current_campaign_expected_repeat_count'] = 1
    out['validity_rules']['minimum_repeat_count'] = 1
    out['validity_rules']['requirements'][0] = 'The row has accepted status and exactly one complete accepted sample.'
    out['validity_rules']['documented_fallback_exception'] = (
        'Any admitted known fallback is described per row; requested_path is false when a different engine ran.')
    out['approved_results'].sort(key=lambda row: row['stable_key'])
    for row in out['approved_results']:
        audit._validate_approved_tuple_row(row, audit.Q1024_CAMPAIGN_ID,
            out['matrix_scope'], 'fast local row', allow_legacy_preservation=False,
            require_field_order=True, fast_local=True)
    ordered = {key: out[key] for key in (
        'schema', 'version', 'status', 'purpose', 'authority', 'campaign_identity',
        'stable_key_contract', 'terminology', 'metric_definitions', 'validity_rules', 'benchmark_host_profile', 'matrix_scope',
        'approved_results')}
    recovery.atomic(RESULTS, yaml.dump(ordered, Dumper=yaml.CSafeDumper,
        sort_keys=False, allow_unicode=True, width=100, explicit_start=True).encode())


def make_config(child):
    config = json.loads((ROOT / child['path']).read_bytes())
    config['bench_e2e']['matrix']['for'] = {
        name: 1 for name in config['bench_e2e']['matrix']['for']}
    config['bench_e2e']['description'] = (
        'FAST LOCAL single-sample q1024; retained compatible index; cold PostgreSQL '
        'before timing; separate publication. ' + child['stable_key'])
    # Keep the existing singleton schedule and all biological/data parameters.
    # In particular, do not change RNG seed, query count, algorithms or prefilter.
    path = CONFIGS / ('tuple_' + str(child['ordinal']).zfill(4) + '.json')
    recovery.atomic(path, (json.dumps(config, indent=2) + '\n').encode())
    return path


def launch(child, receipt, path):
    persisted = child['execution_cell']['identity']['index_mode'] == 'just_index'
    bioseq = child['execution_cell']['identity']['engine'] == 'bioseq'
    environment = os.environ | {
        'BIOSEQ_E2E_REUSE_FROZEN_BINARY': '1',
        'BIOSEQ_E2E_KEEP_ARTIFACTS': '0',
        'BIOSEQ_E2E_RETAIN_MATRIX_JUST_INDEX_TARGET': '1' if persisted and bioseq else '0',
        'BIOSEQ_E2E_SKIP_INDEX_BASELINE_SETUP': '1' if persisted and bioseq else '0',
        'BIOSEQ_E2E_SKIP_MINIMAP2_BASELINE_SETUP': '0',
        'BIOSEQ_E2E_JUST_INDEX_SETUP_ONLY': '',
        'BIOSEQ_E2E_JUST_INDEX_SETUP_PIPELINE_MODE': 'task_parallel',
    }
    messages = [
        {'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {
            'protocolVersion': '2024-11-05', 'capabilities': {},
            'clientInfo': {'name': 'fast-local-e2e', 'version': '1'}}},
        {'jsonrpc': '2.0', 'method': 'notifications/initialized', 'params': {}},
        {'jsonrpc': '2.0', 'id': 2, 'method': 'tools/call', 'params': {
            'name': 'start_build', 'arguments': {'mode': 'benchmark-e2e',
                'run_only': True, 'debug': False, 'reuse_build_receipt': receipt,
                'bench_e2e_config': str(path), 'filter': {'benchmarks': ['bench_e2e']}}}}]
    result = subprocess.run([sys.executable, 'build_mcp/server.py'], cwd=ROOT,
        input='\n'.join(map(json.dumps, messages)) + '\n', capture_output=True,
        text=True, timeout=150, env=environment)
    recovery.atomic(OUTPUT / ('launch_' + str(child['ordinal']) + '.log'),
                    (result.stdout + result.stderr).encode())
    assert not result.returncode and ' is scheduled (' in result.stdout, result.stdout[-2000:]
    observed = fleet.monitor._local_state()
    assert observed['bench_e2e_config'] == str(path), observed
    return observed


def publish(child, config_path, report_paths, out):
    baselines, conflicts = recovery.family_baselines(recovery.COLLECTED / 'local')
    assert not conflicts, conflicts
    for report_name in report_paths:
        path = Path(report_name)
        if not path.is_absolute():
            path = ROOT / path
        if not path.name.startswith('report_') or path.suffix != '.json':
            continue
        report = json.loads(path.read_bytes())
        config = json.loads(config_path.read_bytes())
        raw, _, _, _ = audit._validate_raw_sidecar(path, report, config,
            {'expected_raw_records': 1, 'cells': [child['execution_cell']]}, expected_repeats=1)
        record = audit._validate_record(raw[0], {}, audit.Q1024_CAMPAIGN_ID, 1024, True)
        assert record['stable_key'] == child['stable_key']
        identity = dict(record['identity'], engine='minimap2', bioseq_index_name=None,
            requested_minimizer_algorithm=None, requested_chaining_algorithm=None,
            requested_base_alignment_algorithm=None)
        records = [record]
        if record['identity']['engine'] == 'bioseq':
            key = audit._stable_key_for_identity(identity)
            assert key in baselines, 'Missing independently validated local minimap2 baseline'
            records.append(next(r for r in baselines[key][1] if r['repeat'] == 0))
        rows = audit._aggregate(records, expected_repeats=1)
        row = next(r for r in rows if r['stable_key'] == child['stable_key'])
        event('audited', ordinal=child['ordinal'], status=row['status'], comments=row['COMMENTS'])
        if row['status'] != 'accepted':
            return False
        if row['stable_key'] not in {r['stable_key'] for r in out['approved_results']}:
            out['approved_results'].append(recovery.approved_row(row, fast_local=True))
            write_document(out)
        return True
    raise RuntimeError('Terminal fast run has no complete report to audit')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--daemon', action='store_true')
    parser.add_argument('--plan', action='store_true')
    args = parser.parse_args()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    if args.daemon:
        with (OUTPUT / 'controller.log').open('ab') as log:
            process = subprocess.Popen([sys.executable, __file__], cwd=ROOT,
                stdin=subprocess.DEVNULL, stdout=log, stderr=log, start_new_session=True)
        print(json.dumps({'pid': process.pid})); return
    lock = (OUTPUT / 'controller.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    recovery.atomic(OUTPUT / 'controller.pid', (str(os.getpid()) + '\n').encode())
    children = sorted(json.loads(recovery.MANIFEST.read_bytes())['children'], key=priority)
    by_ordinal = {c['ordinal']: c for c in children}
    state = json.loads(STATE.read_bytes()) if STATE.exists() else {'attempted': [], 'active': None}
    out = document(); write_document(out)
    while True:
        maintenance = json.loads((ROOT / 'bench/e2e_fleet_maintenance.json').read_bytes())
        assert maintenance['hosts']['local']['paused'], 'Standard local queue must remain paused'
        standard = load_yaml(ROOT / 'ALL_RESULTS_TUPLES.yaml')
        approved = {r['stable_key'] for r in standard['approved_results']} | {
            r['stable_key'] for r in out['approved_results']}
        remaining = [c for c in children if c['stable_key'] not in approved
                     and c['stable_key'] not in state['attempted']]
        if args.plan:
            print(json.dumps({'standard_approved': len({r['stable_key'] for r in standard['approved_results']}),
                'fast_approved': len(out['approved_results']), 'remaining': len(remaining),
                'first_ordinals': [c['ordinal'] for c in remaining[:24]]}, indent=2)); return
        current = fleet.monitor._local_state()
        if current.get('status') == 'running':
            time.sleep(60); continue
        if current.get('cleanup_unverified'):
            event('blocked', reason='MCP cleanup unverified'); time.sleep(60); continue
        if state['active']:
            active = state['active']; child = by_ordinal[active['ordinal']]
            assert current.get('bench_e2e_config') == active['config'], 'Local run owner changed'
            try:
                publish(child, Path(active['config']), current.get('report_files', []), out)
            except Exception as exc:
                event('audit_failed', ordinal=child['ordinal'], detail=str(exc))
            state['active'] = None
            recovery.atomic(STATE, (json.dumps(state, indent=2) + '\n').encode())
            continue
        # A daemon restart can happen after MCP admission but before our state
        # commit. Recover that exact config owner instead of rerunning its tuple.
        config = Path(current.get('bench_e2e_config') or '')
        if config.parent == CONFIGS and config.stem.startswith('tuple_'):
            ordinal = int(config.stem.removeprefix('tuple_'))
            child = by_ordinal[ordinal]
            if child['stable_key'] not in state['attempted']:
                state['attempted'].append(child['stable_key'])
                state['active'] = {'ordinal': ordinal, 'config': str(config)}
                recovery.atomic(STATE, (json.dumps(state, indent=2) + '\n').encode())
                continue
        if not state.get('standard_drained'):
            fleet.collect('local', 'before_fast_drain_' + str(int(time.time())))
            subprocess.run([sys.executable, 'bench/recover_e2e_measurements.py', '--publish', '--host', 'local'],
                           cwd=ROOT, check=True, timeout=300)
            state['standard_drained'] = True
            recovery.atomic(STATE, (json.dumps(state, indent=2) + '\n').encode())
            continue
        if not remaining:
            event('queue_exhausted', fast_approved=len(out['approved_results'])); return
        child = remaining[0]
        if fleet.shutil.disk_usage(ROOT).free < 12 * 1024**3:
            fleet.reclaim_idle_local_indexes()
            if fleet.shutil.disk_usage(ROOT).free < 12 * 1024**3:
                event('blocked', reason='Less than 12 GiB available after safe idle cleanup')
                time.sleep(60); continue
        if time.time() - state.get('last_log', 0) >= 3600:
            fleet.append_progress_log(
                f'Fast local (separate one-sample methodology): {len(out["approved_results"])} approved; '
                f'{len(remaining)} not yet attempted; {len(state["attempted"])} submitted. '
                'Standard local queue remains paused; remote five-repeat queues unchanged.',
                title='fast-local queue checkpoint')
            state['last_log'] = time.time()
        receipt = current.get('build_receipt_id')
        assert receipt, 'No valid existing local frozen build receipt'
        path = make_config(child)
        observed = launch(child, receipt, path)
        state['attempted'].append(child['stable_key'])
        state['active'] = {'ordinal': child['ordinal'], 'config': str(path)}
        recovery.atomic(STATE, (json.dumps(state, indent=2) + '\n').encode())
        event('dispatched', ordinal=child['ordinal'], status=observed['status'], build_receipt=receipt)
        time.sleep(60)


if __name__ == '__main__':
    main()
