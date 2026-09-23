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


def wait_for_local_run():
    """Notice terminal MCP state promptly without polling SQL or build metadata.

    The normal controller loop still verifies ownership and cleanup before
    collection/admission. While a job runs, only its small state file is read;
    expensive result/queue scans remain at most once per minute.
    """
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        time.sleep(min(5, max(0, deadline - time.monotonic())))
        try:
            status = json.loads(Path(fleet.monitor.STATE_PATH).read_bytes()).get(
                'benchmark-e2e', {}).get('status')
        except (OSError, ValueError):
            return  # The normal observation path handles unreadable state.
        if status != 'running':
            return


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


def index_group(child):
    identity = child['execution_cell']['identity']
    return tuple(identity[key] for key in (
        'index_mode', 'engine', 'dataset', 'alphabet', 'target_size_gbp',
        'bioseq_index_name', 'requested_minimizer_algorithm', 'kmer_size', 'window_size'))


def retire_completed_group():
    """Drop only idle benchmark scratch tables after their index batch is done."""
    assert fleet.monitor._local_state().get('status') in fleet.TERMINAL_STATUSES
    psql = '/opt/homebrew/opt/postgresql@17/bin/psql'
    sql = """
      SET lock_timeout='5s';
      -- A fresh psql backend has not necessarily initialized the extension.
      -- Load its existing DROP hooks before retiring benchmark relations.
      LOAD 'bioseq';
      DO $cleanup$
      DECLARE item record;
      BEGIN
        IF EXISTS (SELECT FROM pg_stat_activity WHERE datname=current_database()
                   AND pid<>pg_backend_pid() AND backend_type='client backend') THEN
          RAISE EXCEPTION 'benchmark cleanup requires no other client sessions';
        END IF;
        FOR item IN SELECT tablename FROM pg_tables WHERE schemaname='public'
          AND tablename ~ '^e2e[mq]_[a-z0-9]+_k[0-9]+_w[0-9]+_[0-9a-f]{16}$'
        LOOP
          EXECUTE format('DROP TABLE public.%I CASCADE', item.tablename);
          RAISE NOTICE 'retired benchmark scratch table %', item.tablename;
        END LOOP;
        -- Repair leftovers from earlier maintenance sessions that dropped the
        -- owner without loading the extension's object-access hooks. Only an
        -- empty benchmark database may have its extension catalogs reset.
        IF current_database() = 'bioseq'
           AND EXISTS (SELECT FROM bioseq_internal.minimizer_generation_storage s
                       WHERE NOT EXISTS (SELECT FROM pg_class c WHERE c.oid=s.user_relid))
           AND NOT EXISTS (
             SELECT FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace
             WHERE n.nspname NOT IN ('pg_catalog','information_schema','bioseq','bioseq_internal')
               AND n.nspname NOT LIKE 'pg_toast%'
               AND n.nspname NOT LIKE 'pg_temp%'
               AND c.relkind IN ('r','p','v','m','f','S')) THEN
          DROP EXTENSION bioseq CASCADE;
          CREATE EXTENSION bioseq;
          RAISE NOTICE 'reset empty benchmark extension after orphaned generation owner';
        END IF;
      END $cleanup$;
    """
    result = subprocess.run([psql, '-U', 'postgres', '-d', 'bioseq', '-v',
                             'ON_ERROR_STOP=1', '-c', sql], capture_output=True,
                            text=True, timeout=60, check=True)
    event('completed_index_group_retired', detail=result.stderr,
          recovery='Regenerate benchmark scratch tables/indexes; results retained')
    # There are no clients left; unreferenced, unopened image files do not need
    # the ordinary one-hour grace period at this explicit batch boundary.
    fleet.reclaim_idle_local_indexes(minimum_free_gib=48, orphan_grace_seconds=0)


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
        raw, manifest, sidecar, _ = audit._validate_raw_sidecar(path, report, config,
            {'expected_raw_records': 1, 'cells': [child['execution_cell']]}, expected_repeats=1)
        source = {
            'report': str(path.relative_to(ROOT)),
            'raw_sidecar': str(sidecar.relative_to(ROOT)),
            'run_id': manifest['run_id'],
            'schedule_sorted_checksum': manifest['schedule_sorted_checksum'],
            'run_receipt_id': None, 'run_receipt_sha256': None,
        }
        record = audit._validate_record(raw[0], source, audit.Q1024_CAMPAIGN_ID, 1024, True,
                                        require_control_queries=False)
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
            approved = recovery.approved_row(row, fast_local=True)
            if raw[0].get('gate8_sql') is None and row['engine'] == 'bioseq':
                approved['comments'] += ' Extra EXPLAIN ANALYZE/cache-off/vectorization-off mapping controls were disabled by user request; measured-statement checks were retained.'
            out['approved_results'].append(approved)
            write_document(out)
        return True
    raise RuntimeError('Terminal fast run has no complete report to audit')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--daemon', action='store_true')
    parser.add_argument('--plan', action='store_true')
    parser.add_argument('--retry-setup-failure', action='append', type=int, default=[],
                        help='Explicitly requeue an unmeasured tuple after its setup failure is diagnosed and fixed')
    parser.add_argument('--reason', help='Required explanation for an explicit setup retry')
    args = parser.parse_args()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    if args.daemon:
        from e2e_queue_supervisor import start
        print(json.dumps({'supervisor_pid': start('fast_local')})); return
    lock = (OUTPUT / 'controller.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    recovery.atomic(OUTPUT / 'controller.pid', (str(os.getpid()) + '\n').encode())
    children = sorted(json.loads(recovery.MANIFEST.read_bytes())['children'], key=priority)
    by_ordinal = {c['ordinal']: c for c in children}
    state = json.loads(STATE.read_bytes()) if STATE.exists() else {'attempted': [], 'active': None}
    if args.retry_setup_failure:
        assert args.reason, '--reason is required for a diagnosed setup retry'
        latest = {}
        for line in (OUTPUT / 'events.jsonl').read_text().splitlines():
            row = json.loads(line)
            if row['event'] in ('audit_failed', 'audited'):
                latest[row['ordinal']] = row
        approved = {r['stable_key'] for r in document()['approved_results']} | {
            r['stable_key'] for r in load_yaml(ROOT / 'ALL_RESULTS_TUPLES.yaml')['approved_results']}
        retry_keys = set()
        for ordinal in set(args.retry_setup_failure):
            child = by_ordinal[ordinal]
            assert ordinal != (state.get('active') or {}).get('ordinal'), 'Cannot requeue an active owner'
            assert child['stable_key'] not in approved, 'Already approved; reuse that result'
            failure = latest.get(ordinal, {})
            assert (failure.get('event') == 'audit_failed' and failure.get('detail')
                    == 'Terminal fast run has no complete report to audit'), 'No proven unmeasured attempt'
            retry_keys.add(child['stable_key'])
        state['attempted'] = [k for k in state['attempted'] if k not in retry_keys]
        recovery.atomic(STATE, (json.dumps(state, indent=2) + '\n').encode())
        event('setup_failures_requeued', ordinals=sorted(set(args.retry_setup_failure)), reason=args.reason)
        return
    if 'last_ordinal' not in state and (OUTPUT / 'events.jsonl').exists():
        # MCP build-only state replaces the last run's config. Retain the
        # index group across a binary repair instead of switching datasets.
        for line in (OUTPUT / 'events.jsonl').read_text().splitlines():
            event_row = json.loads(line)
            if event_row.get('event') == 'dispatched':
                state['last_ordinal'] = event_row['ordinal']
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
            wait_for_local_run(); continue
        if current.get('cleanup_unverified'):
            event('blocked', reason='MCP cleanup unverified'); time.sleep(60); continue
        if state['active']:
            active = state['active']; child = by_ordinal[active['ordinal']]
            state['last_ordinal'] = active['ordinal']
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
        quarantine_path = ROOT / 'bench/e2e_fast_quarantine.json'
        quarantine = json.loads(quarantine_path.read_bytes()).get('families', {}) if quarantine_path.exists() else {}
        if 'local' in quarantine:
            time.sleep(60); continue
        rollout_path = ROOT / 'reporting/bioseq_benchmark_e2e/fast_remote/state.json'
        if rollout_path.exists() and 'local' not in json.loads(rollout_path.read_bytes()).get('ready', {}):
            # The central controller retired the old payload for a repair.
            # Do not race its build admission with another old-binary tuple.
            time.sleep(60); continue
        if not state.get('standard_drained'):
            fleet.collect('local', 'before_fast_drain_' + str(int(time.time())))
            subprocess.run([sys.executable, 'bench/recover_e2e_measurements.py', '--publish', '--host', 'local'],
                           cwd=ROOT, check=True, timeout=300)
            state['standard_drained'] = True
            recovery.atomic(STATE, (json.dumps(state, indent=2) + '\n').encode())
            continue
        if not remaining:
            event('queue_exhausted', fast_approved=len(out['approved_results'])); return
        # A fresh target/index can temporarily occupy much more than its final
        # image. Preserve controller/report headroom; do not repeat the 4-Gbp
        # ENOSPC failure admitted by the former flat 12-GiB check. This is an
        # operational estimate, not a promise that every tuple fits that size.
        previous = by_ordinal.get(int(config.stem.removeprefix('tuple_'))) \
            if config.parent == CONFIGS and config.stem.startswith('tuple_') else by_ordinal.get(state.get('last_ordinal'))
        if previous:
            same_group = [c for c in remaining if index_group(c) == index_group(previous)]
            remaining = same_group + [c for c in remaining if index_group(c) != index_group(previous)]
        if previous and index_group(previous) != index_group(remaining[0]):
            retire_completed_group()

        def required_space(child):
            identity = child['execution_cell']['identity']
            if (current.get('status') == 'completed' and not current.get('build_only') and previous
                    and identity['index_mode'] == 'just_index'
                    and index_group(child) == index_group(previous)):
                return 8 * 1024**3
            return (8 + 8 * identity['target_size_gbp']) * 1024**3

        free = fleet.shutil.disk_usage(ROOT).free
        if free < required_space(remaining[0]):
            fleet.reclaim_idle_local_indexes(minimum_free_gib=48)
            free = fleet.shutil.disk_usage(ROOT).free
        just_index = [c for c in remaining
                      if c['execution_cell']['identity']['index_mode'] == 'just_index']
        fitting = [c for c in (just_index or remaining) if required_space(c) <= free]
        if not fitting:
            event('blocked', reason='Insufficient headroom for a fresh index; tuples remain pending',
                  free_gib=round(free / 1024**3, 2))
            time.sleep(60); continue
        child = fitting[0]
        if child is not remaining[0]:
            event('capacity_deferred', preferred_ordinal=remaining[0]['ordinal'],
                  selected_ordinal=child['ordinal'], free_gib=round(free / 1024**3, 2))
        if time.time() - state.get('last_log', 0) >= 3600:
            fleet.append_progress_log(
                f'Fast local (separate one-sample methodology): {len(out["approved_results"])} approved; '
                f'{len(remaining)} not yet attempted; {len(state["attempted"])} submitted. '
                'Standard local queue remains paused; AWS queues transition independently to their separate fast authorities.',
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
        wait_for_local_run()


if __name__ == '__main__':
    main()
