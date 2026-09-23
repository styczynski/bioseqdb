#!/usr/bin/env python3
"""Roll out control-free E2E binaries, then run grouped one-sample AWS queues.

All execution/build/import goes through bioseq-build MCP. Existing runs finish
and are collected before ownership changes. One warm-tree build per family is
copied to replicas; no per-tuple builds. Local files are the results authority.
"""
import argparse
import copy
from concurrent.futures import ThreadPoolExecutor
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tarfile
import time

import yaml
import aggregate_e2e_algorithm_matrix as audit
import fast_local_e2e as local
import manage_e2e_fleet as fleet
import recover_e2e_measurements as recovery
from replicate_e2e_runtime import rpc

ROOT = fleet.ROOT
OUTPUT = ROOT / 'reporting/bioseq_benchmark_e2e/fast_remote'
STATE = OUTPUT / 'state.json'
CONFIGS = ROOT / 'bench/e2e_fast_remote'
LEADERS = {'c7g': 'test3-c7g', 'm4': 'test3-m4',
           'm7a': 'test3-m7a', 'm7i': 'test4-m7i', 'local': 'local'}
# These workers were checked to have populated Clang object caches. Imported
# runtime-only replicas must not accidentally be used for clean builds.
BUILD_CANDIDATES = {'c7g': ('test3-c7g', 'test4-c7g'), 'm4': ('test3-m4',),
                    'm7a': ('test3-m7a', 'test4-m7a'),
                    'm7i': ('test4-m7i', 'test3-m7i'), 'local': ('local',)}
MEMORY_PATCH = 'common/memory_budget/index.cppm'
CONTROL = tuple(str(p.relative_to(ROOT)) for p in (ROOT / 'build_mcp').rglob('*.py'))
EXPLANATION = (
    'Single-sample coverage campaign authorized 2026-09-22. P50 is the single '
    'observation, not a five-run estimate. Compatible just_index tuples are '
    'pre-grouped by dataset, alphabet, index type, minimizer producer and k/w; '
    'the index is reused across chaining/alignment configurations. PostgreSQL '
    'restarts before each measured statement; OS filesystem cache is not cleared. '
    'Extra EXPLAIN ANALYZE/cache-off/vectorization-off mapping controls are disabled. '
    'Measured-statement correctness, exact minimap2 match counts, position tolerance, '
    'actual stages/fallbacks, prefilter-off and timing intervals remain checked. '
    'Index setup is untimed for just_index; no_index still includes transient '
    'index construction in its measured query. Never pool with five-run results. '
)


def emit(kind, **fields):
    row = {'timestamp_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
           'event': kind, **fields}
    with (OUTPUT / 'events.jsonl').open('a') as handle:
        handle.write(json.dumps(row) + '\n')
    print(json.dumps(row), flush=True)


def save(state):
    recovery.atomic(STATE, (json.dumps(state, indent=2) + '\n').encode())


def result_path(family):
    return ROOT / ('ALL_RESULTS_TUPLES_FAST_REMOTE_' + family.upper() + '.yaml')


def standard_keys(family):
    keys = set()
    for host, *_ in fleet.monitor.registered_hosts(include_retired=True):
        if fleet.family(host) == family:
            document = local.load_yaml(recovery.authority_path(host))
            keys.update(r['stable_key'] for r in document['approved_results']
                        if r['status'] == 'accepted')
    return keys


def write_results(family, document, children):
    standard = local.load_yaml(recovery.authority_path('default-' + family))
    local_standard = local.load_yaml(ROOT / 'ALL_RESULTS_TUPLES.yaml')
    excluded = standard_keys(family)
    scope = copy.deepcopy(standard['matrix_scope'])
    scope.update(repetitions_per_cell=1,
                 total_expected_cells=len({c['stable_key'] for c in children} - excluded),
                 projection_description='CPU matrix, excluding approved standard tuples across all replicas of this family; one sample per remaining tuple.')
    scope['total_expected_runs'] = scope['total_expected_cells']
    scope['execution_location'] = 'remote'
    for key in ('bioseq_minimizer_algorithms', 'bioseq_chaining_algorithms', 'bioseq_base_alignment_algorithms'):
        scope[key] = [value for value in scope[key] if 'vulkan' not in value and value != 'ksw2_batched']
    scope['bioseq_expected_cells'] = sum(c['execution_cell']['identity']['engine']=='bioseq'
                                        and c['stable_key'] not in excluded for c in children)
    scope['minimap2_expected_cells'] = sum(c['execution_cell']['identity']['engine']=='minimap2'
                                          and c['stable_key'] not in excluded for c in children)
    document['matrix_scope'] = scope
    document['benchmark_host_profile']['replicas'] = [
        local.load_yaml(recovery.authority_path(host))['remote_host_binding']
        # Retired hosts remain provenance for already collected measurements.
        for host, *_ in fleet.monitor.registered_hosts(include_retired=True) if fleet.family(host) == family]
    metrics = copy.deepcopy(local_standard['metric_definitions'])
    for name in ('a', 'b'):
        metrics[name]['aggregation'] = 'One accepted sample; P50 equals its value.'
    metrics['p50_method'] = 'Single-sample identity, not a five-repeat median estimate.'
    metrics['a']['per_repeat_definition'] = 'Framework measured-iteration window for the one mapping operation, including CIGAR and MAPQ; no untimed setup or control queries.'
    metrics['clock_semantics']['framework_outer'] = 'Monotonic duration of the one framework measured-iteration window.'
    metrics['timing_scope'] = {'allowed_values': [audit.FAST_REMOTE_TIMING_SCOPE],
                             'classification': 'non_metric_observation_scope',
                             'p50_partition_rule': 'Do not pool fast and standard measurements.'}
    document['metric_definitions'] = metrics
    document['validity_rules'] = copy.deepcopy(local_standard['validity_rules'])
    document['validity_rules'].update(current_campaign_expected_repeat_count=1, minimum_repeat_count=1)
    document['validity_rules']['requirements'][0] = 'Exactly one complete accepted q1024 sample, with measured-statement checks; extra control queries are not required.'
    document['validity_rules']['documented_fallback_exception'] = 'Any admitted known fallback is disclosed per row and is not eligible as its requested algorithm path.'
    document['campaign_identity']['execution_scope'] = 'remote_fast_single_sample_campaign'
    document['approved_results'].sort(key=lambda row: row['stable_key'])
    for row in document['approved_results']:
        audit._validate_approved_tuple_row(row, audit.Q1024_CAMPAIGN_ID, scope,
            'fast remote row', allow_legacy_preservation=False, require_field_order=True, fast_local=True)
    recovery.atomic(result_path(family), yaml.dump(document, Dumper=yaml.CSafeDumper,
                    sort_keys=False, allow_unicode=True, width=100, explicit_start=True).encode())


def document_for(family, children):
    if result_path(family).exists():
        document = local.load_yaml(result_path(family))
        write_results(family, document, children)
        return document
    standard = local.load_yaml(ROOT / 'ALL_RESULTS_TUPLES.yaml')
    document = {key: copy.deepcopy(standard[key]) for key in (
        'schema', 'version', 'status', 'purpose', 'authority', 'campaign_identity',
        'stable_key_contract', 'terminology', 'metric_definitions', 'validity_rules',
        'benchmark_host_profile', 'matrix_scope', 'approved_results')}
    document['purpose'] = 'Self-contained FAST REMOTE ' + family.upper() + ' result authority. ' + EXPLANATION + 'All timings and interpretation are embedded; no external file references are required. Replica ownership is recorded in each row comment.'
    document['authority']['full_matrix_complete'] = False
    document['authority']['sealed_verification_manifest_required_for_append'] = False
    document['benchmark_host_profile'] = {
        'family': family, 'gpu': 'hardware unavailable; Vulkan excluded',
        'replicas': [local.load_yaml(recovery.authority_path(host))['remote_host_binding']
                     for host, *_ in fleet.monitor.registered_hosts(include_retired=True)
                     if fleet.family(host) == family]}
    document['approved_results'] = []
    write_results(family, document, children)
    return document


def group_key(child):
    return json.dumps(local.index_group(child))


def hold(host, reason):
    path = ROOT / 'bench/e2e_fleet_maintenance.json'
    # Only this controller changes fast ownership; preserve other maintenance.
    with (OUTPUT / 'maintenance.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        document = json.loads(path.read_bytes())
        document['hosts'].setdefault(host, {}).update(paused=True, reason=reason, owner='fast_remote_e2e')
        document['hosts'][host].pop('resume_after_build_receipt', None)
        recovery.atomic(path, (json.dumps(document, indent=2) + '\n').encode())


def observe(host):
    observed = fleet.monitor._local_state() if host == 'local' else fleet.monitor._remote_state(*fleet.HOSTS[host])
    if observed.get('status') in fleet.TERMINAL_STATUSES and observed.get('cleanup_unverified'):
        # MCP status reuses the exact-owner cleanup proof without restarting,
        # signalling, or admitting any job. Never clear this flag ourselves.
        command = [sys.executable, 'build_mcp/server.py'] if host == 'local' else fleet.ssh(host) + [
            'cd /home/ubuntu/bioseqdb && ' + fleet.ENVIRONMENT + 'python3 build_mcp/server.py']
        try:
            rpc(command, 'status', {'mode': 'benchmark-e2e'})
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
            # An observation failure is not terminal cleanup evidence and must
            # not stop healthy workers elsewhere in this controller's fleet.
            return observed | {'cleanup_revalidation_error': str(exc)}
        observed = fleet.monitor._local_state() if host == 'local' else fleet.monitor._remote_state(*fleet.HOSTS[host])
    return observed


def drain_standard(host, state, observed):
    if observed.get('status') == 'missing':
        # A newly imported replica has no previous benchmark to collect.
        return
    token = observed.get('log_file') or 'unknown'
    if state.setdefault('drained', {}).get(host) == token:
        return
    if host == 'local':
        # Finish publication owned by the previous local fast daemon, if any.
        previous = json.loads(local.STATE.read_bytes()) if local.STATE.exists() else {}
        active = previous.get('active')
        if active:
            child = next(c for c in json.loads(fleet.MANIFEST.read_bytes())['children']
                         if c['ordinal'] == active['ordinal'])
            assert observed.get('bench_e2e_config') == active['config'], observed
            try:
                local.publish(child, Path(active['config']), observed.get('report_files', []), local.document())
            except Exception as exc:
                emit('local_drain_audit_failed', detail=str(exc))
            previous['active'] = None
            recovery.atomic(local.STATE, (json.dumps(previous, indent=2) + '\n').encode())
    assert fleet.collect(host, 'fast_transition_' + str(int(time.time())))
    subprocess.run([sys.executable, 'bench/recover_e2e_measurements.py', '--publish', '--host', host],
                   cwd=ROOT, check=True, timeout=300, stdout=subprocess.DEVNULL)
    state['drained'][host] = token
    save(state)


def build_source_paths(host):
    """Use the authoritative source tree, not a hand-maintained repair shortlist.

    Only an admitted build boundary calls this; frozen per-tuple launches never
    synchronize or rebuild production code. Results, fixtures and build trees
    are not part of the transfer. Content-checksum sync preserves warm objects
    for every byte-identical source, even when local Git mtimes changed.
    """
    if host == 'local':
        return ('bench/e2e/index.cppm',)
    inventory = subprocess.check_output([
        'git', '-C', str(ROOT), 'ls-files', '-z', '--cached', '--others',
        '--exclude-standard', '--', 'bioseq', 'bioseq_caching', 'common',
        'codegen', 'cmake', 'bench', 'bench_framework',
        'CMakeLists.txt', 'CMakePresets.json'], text=True).split('\0')
    suffixes = {'.cppm', '.cpp', '.h', '.hpp', '.cmake', '.in', '.py', '.sql', '.control'}
    return tuple(sorted({p for p in inventory if p and (ROOT / p).is_file()
        and (Path(p).suffix in suffixes or Path(p).name in ('CMakeLists.txt', 'CMakePresets.json'))}))


def rollout(host, observed, state):
    """At a terminal boundary build once on leaders, import once on replicas."""
    if host in state['ready'] or observed.get('status') == 'running':
        return
    new_replica = observed.get('status') == 'missing' and host not in BUILD_CANDIDATES[fleet.family(host)]
    if (observed.get('status') not in fleet.TERMINAL_STATUSES and not new_replica) or observed.get('cleanup_unverified'):
        return
    family = fleet.family(host)
    leader = LEADERS[family]
    if host == leader:
        progress = state['builds'].get(host)
        if progress:
            if (observed.get('status') == 'failed'
                    and 'source graph changed during build' in (observed.get('notes') or '')
                    and state.setdefault('source_seal_retries', {}).get(host, 0) < 1):
                # No clean/reconfigure: Ninja reuses every completed object.
                state['source_seal_retries'][host] = 1
                state.setdefault('rollout_history', []).append({'host': host, 'build': progress,
                    'reason': 'One incremental retry after source-change seal rejection'})
                state['builds'].pop(host); save(state)
                emit('source_seal_retry', host=host)
                return
            if observed.get('build_receipt_id') != progress['receipt']:
                raise RuntimeError('Build owner changed: ' + host)
            if observed['status'] != 'completed' or not observed.get('build_only'):
                raise RuntimeError('One-time fast build failed: ' + host + ' ' + str(observed))
            required_paths = build_source_paths(host)
            # Queue/audit Python changes do not alter the measured executable.
            # They are synchronized for operation, but must not force another
            # C++ build merely because scheduling changed while Clang ran.
            required = {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest()
                        for p in required_paths
                        if not (p.startswith('bench/') and p.endswith('.py'))}
            if any(progress.get('patch_sha256', {}).get(p) != digest for p, digest in required.items()):
                state.setdefault('rollout_history', []).append({'host': host, 'build': progress,
                    'reason': 'Completed build predates authoritative source batch; incremental update required'})
                state['builds'].pop(host); save(state)
                emit('repair_batch_incremental_update', host=host)
                return
            if family in ('m4', 'm7a', 'm7i'):
                receipt = observed['build_receipt_id']
                if state.setdefault('isa_verified', {}).get(host) != receipt:
                    command = fleet.ssh(host) + ['cd /home/ubuntu/bioseqdb && ' + fleet.ENVIRONMENT + 'python3 build_mcp/server.py']
                    for function in ('is_alignment_kernel_table_complete', '::is_block_layout_valid()'):
                        inspection = rpc(command, 'inspect', {'target': 'postgresql_bioseq',
                            'function_name': function, 'debug': False, 'reuse_build_receipt': receipt,
                            'historical_artifact_only': True, 'llc_on_bc': False, 'max_matches': 1, 'snippet_lines': 12})
                        evidence = OUTPUT / ('isa_' + host + '_' + function.strip(':()') + '.json')
                        recovery.atomic(evidence, (json.dumps(inspection, indent=2) + '\n').encode())
                        assert inspection['summary']['total_matches_before_cap'] == 0, 'Unexpected shared ISA helper: ' + function
                    state['isa_verified'][host] = receipt
                    save(state); emit('isa_helper_symbols_absent', host=host, receipt=receipt)
            state['ready'][host] = observed['build_receipt_id']
            if MEMORY_PATCH in progress.get('patch_sha256', {}):
                state.setdefault('memory_charge_receipts', []).append(observed['build_receipt_id'])
            if host != 'local':
                for peer in fleet.HOSTS:
                    if fleet.family(peer) == family:
                        hold(peer, 'Fast campaign owns family; drain current standard tuple, then import same-family binary')
            save(state); emit('fast_binary_ready', host=host, receipt=observed['build_receipt_id'])
            if host == 'local':
                subprocess.run([sys.executable, 'bench/fast_local_e2e.py', '--daemon'], cwd=ROOT, check=True)
            return
        drain_standard(host, state, observed)
        paths = build_source_paths(host)
        fleet.sync_files(host, tuple(sorted(set((*CONTROL, *paths)))), checksum=True)
        preset = None if host == 'local' else ('native-arm64-clang22-linux' if family == 'c7g' else 'native-x64-clang22-linux')
        state['builds'][host] = {'receipt': None, 'status': 'admitting',
            'patch_sha256': {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in paths}}
        save(state)
        log = fleet.mcp_start(host, {'mode': 'benchmark-e2e', 'build_only': True,
                              'fresh': False, 'debug': False}, 'fast_controls_removed', build_preset=preset)
        admitted = observe(host)
        assert admitted.get('build_only') and admitted.get('build_receipt_id'), admitted
        state['builds'][host].update(receipt=admitted['build_receipt_id'], status='building', log=log)
        save(state); emit('fast_build_started', host=host, receipt=admitted['build_receipt_id'])
    elif leader in state['ready']:
        hold(host, 'Fast one-sample queue owns worker; importing verified same-family executable')
        drain_standard(host, state, observed)
        # Existing MCP export/import utility copies only runtime payload, not DB/data/results.
        completed = subprocess.run([sys.executable, 'bench/replace_e2e_runtime.py', leader, host],
                                   cwd=ROOT, capture_output=True, text=True, timeout=650)
        recovery.atomic(OUTPUT / ('import_' + host + '.log'), (completed.stdout + completed.stderr).encode())
        assert completed.returncode == 0, completed.stderr[-1500:]
        imported = json.loads(completed.stdout.splitlines()[-1])
        receipt = imported.get('build_receipt_id') or imported.get('receipt_id')
        assert receipt, imported
        state['ready'][host] = receipt
        if state['ready'][leader] in state.get('memory_charge_receipts', []):
            state.setdefault('memory_charge_receipts', []).append(receipt)
        save(state); emit('fast_binary_imported', host=host, receipt=receipt)


def make_config(child):
    config = json.loads((ROOT / child['path']).read_bytes())
    config['bench_e2e']['matrix']['for'] = {name: 1 for name in config['bench_e2e']['matrix']['for']}
    config['bench_e2e']['description'] = 'FAST REMOTE q1024, one measured sample, compatible-index reuse, no extra mapping controls. ' + child['stable_key']
    path = CONFIGS / ('tuple_' + str(child['ordinal']).zfill(4) + '.json')
    recovery.atomic(path, (json.dumps(config, indent=2) + '\n').encode())
    return path.relative_to(ROOT).as_posix()


def ensure_memory_profile(host):
    """Check the fleet's swap setup before admission; start missing zram only.

    Called at a terminal boundary. Never swap off an existing device, restart
    PostgreSQL, resize storage, or rebuild binaries here.
    """
    program = '''import json,pathlib,subprocess
def swap_rows():
 return {p[0]:p for line in pathlib.Path('/proc/swaps').read_text().splitlines()[1:] if (p:=line.split())}
rows=swap_rows(); repaired=False
if '/dev/zram0' not in rows:
 subprocess.run(['sudo','-n','systemctl','restart','bioseq-zram-swap.service'],check=True,timeout=20)
 repaired=True; rows=swap_rows()
ram_kib=int(next(line.split()[1] for line in pathlib.Path('/proc/meminfo').read_text().splitlines() if line.startswith('MemTotal:')))
zram=rows.get('/dev/zram0'); disk=rows.get('/swapfile')
if zram is None or int(zram[4])!=100: raise RuntimeError('RAM swap missing or priority is not 100')
if disk is None or int(disk[4])!=10: raise RuntimeError('Disk swap missing or priority is not 10')
if int(pathlib.Path('/sys/block/zram0/disksize').read_text())!=ram_kib*1024: raise RuntimeError('zram size differs from physical RAM')
if '[zstd]' not in pathlib.Path('/sys/block/zram0/comp_algorithm').read_text(): raise RuntimeError('zram does not use zstd')
if not 96*1024**2-64 <= int(disk[2]) <= 96*1024**2: raise RuntimeError('Disk swap differs from 96 GiB')
print(json.dumps({'zram_started':repaired,'zram_size_bytes':ram_kib*1024,'zram_algorithm':'zstd','zram_priority':100,'disk_swap_size_kib':int(disk[2]),'disk_swap_priority':10}))
'''
    checked = subprocess.run(fleet.ssh(host) + ['python3 -c ' + shlex.quote(program)],
                             capture_output=True, text=True, timeout=35)
    if checked.returncode:
        raise RuntimeError('Worker memory configuration requires repair: ' + checked.stderr[-1500:])
    result = json.loads(checked.stdout)
    if result['zram_started']:
        emit('missing_zram_started', host=host, **result)
    return result


def ensure_benchmark_priority(host):
    """Configure future cold PostgreSQL starts; never restart a live server here."""
    source = 'infra/package_build_infra/scripts/postgresql_e2e_priority.conf'
    fleet.sync_files(host, (source,), checksum=True)
    command = (
        'sudo -n install -d /etc/systemd/system/postgresql@17-main.service.d && '
        'if ! cmp -s /home/ubuntu/bioseqdb/' + source + ' '
        '/etc/systemd/system/postgresql@17-main.service.d/bioseq-e2e-priority.conf; then '
        'sudo -n install -m 0644 /home/ubuntu/bioseqdb/' + source + ' '
        '/etc/systemd/system/postgresql@17-main.service.d/bioseq-e2e-priority.conf && '
        'sudo -n systemctl daemon-reload || exit; fi; '
        'systemctl show postgresql@17-main.service --property=Nice --value')
    result = subprocess.run(fleet.ssh(host) + [command], capture_output=True,
                            text=True, timeout=35)
    if result.returncode or result.stdout.strip() != '-20':
        raise RuntimeError('Could not establish PostgreSQL cold-start nice=-20: '
                           + result.stdout[-100:] + result.stderr[-500:])
    return {'postgresql_cold_start_nice': -20, 'runner_launch_policy': 'best_effort_sudo_renice_-20'}


def launch(host, child, state):
    # Do this before marking a tuple attempted: a setup failure is retryable
    # and is not a failed measurement of that algorithm combination.
    state.setdefault('memory_profiles', {})[host] = ensure_memory_profile(host)
    state.setdefault('priority_profiles', {})[host] = ensure_benchmark_priority(host)
    config = make_config(child)
    fleet.sync_files(host, (*CONTROL, 'bench/aggregate_e2e_algorithm_matrix.py', config))
    identity = child['execution_cell']['identity']
    retained = identity['index_mode'] == 'just_index' and identity['engine'] == 'bioseq'
    env = {'BIOSEQ_E2E_REUSE_FROZEN_BINARY': '1', 'BIOSEQ_E2E_KEEP_ARTIFACTS': '0',
           'BIOSEQ_E2E_RETAIN_MATRIX_JUST_INDEX_TARGET': '1' if retained else '0',
           'BIOSEQ_E2E_SKIP_INDEX_BASELINE_SETUP': '1' if retained else '0',
           'BIOSEQ_E2E_SKIP_MINIMAP2_BASELINE_SETUP': '0',
           'BIOSEQ_E2E_JUST_INDEX_SETUP_ONLY': '',
           'BIOSEQ_E2E_JUST_INDEX_SETUP_PIPELINE_MODE': 'task_parallel'}
    # Durable reservation precedes admission, so a controller restart cannot duplicate it.
    state['active'][host] = {'ordinal': child['ordinal'], 'config': config, 'admitted': False}
    save(state)
    memory_options = {'memory_buffer_percent': 20} if fleet.family(host) == 'c7g' else {}
    log = fleet.mcp_start(host, {'mode': 'benchmark-e2e', 'run_only': True, 'debug': False,
        'reuse_build_receipt': state['ready'][host], 'bench_e2e_config': config,
        'filter': {'benchmarks': ['bench_e2e']}, **memory_options}, 'fast_' + str(child['ordinal']), environment=env)
    observed = observe(host)
    assert observed.get('bench_e2e_config') == config, observed
    state['active'][host].update(admitted=True, log=log)
    state['attempted'].setdefault(fleet.family(host), []).append(child['stable_key'])
    save(state); emit('fast_dispatched', host=host, ordinal=child['ordinal'], group=group_key(child))


def fetch_reports(host, observed):
    """Fetch exact terminal report and raw bytes, never arbitrary remote paths."""
    names = sorted({Path(p).name for p in observed.get('report_files', [])
                    if Path(p).name.startswith('report_') and Path(p).suffix == '.json'})
    assert names, 'No complete terminal report advertised'
    paths = [name for report in names for name in (report, report.replace('.json', '.raw.jsonl'))]
    assert all('/' not in p and '..' not in p for p in paths)
    command = 'cd /home/ubuntu/bioseqdb/reporting/bioseq_benchmark_e2e && tar -czf - -- ' + shlex.join(paths)
    result = subprocess.run(fleet.ssh(host) + [command], capture_output=True, timeout=90)
    assert result.returncode == 0, result.stderr.decode(errors='replace')[-1000:]
    evidence = OUTPUT / 'evidence' / host
    archive = evidence / ('terminal_' + str(int(time.time())) + '.tar.gz')
    recovery.atomic(archive, result.stdout)
    with tarfile.open(archive) as tar:
        for name in paths:
            member = tar.getmember(name)
            assert member.isfile()
            recovery.atomic(evidence / name, tar.extractfile(member).read())
    return [evidence / name for name in names]


def failure_evidence(host, observed):
    """Retain the bounded terminal log tail and its existing failure diagnostics."""
    result = {'terminal_status': observed.get('status'),
              'build_receipt_id': observed.get('build_receipt_id')}
    log = Path(observed.get('log_file') or '')
    if (log.parent != Path('/tmp') or not log.name.startswith('bioseq_build_mcp_')
            or log.suffix != '.log'):
        return result
    try:
        fetched = subprocess.run(fleet.ssh(host) + [
            'tail -c 65536 -- ' + shlex.quote(str(log))],
            capture_output=True, timeout=30)
        if fetched.returncode:
            result['log_fetch_error'] = fetched.stderr.decode(errors='replace')[-500:]
            return result
        destination = OUTPUT / 'evidence' / host / log.name
        recovery.atomic(destination, fetched.stdout)
        result['terminal_log_tail'] = str(destination.relative_to(ROOT))
        markers = ('MATRIX-SQL-FATAL', 'RAW-STATS-SERIALIZATION-FAIL',
                   'E2E memory admission refused', 'Illegal instruction',
                   'Assertion failed', 'SSL error', 'Segmentation fault',
                   'memory budget', 'No benchmark iterations', 'No iterations')
        diagnostics = [line for line in fetched.stdout.decode(errors='replace').splitlines()
                       if any(marker in line for marker in markers)]
        result['failure_diagnostics'] = diagnostics[-6:]
    except (OSError, subprocess.TimeoutExpired) as exc:
        result['log_fetch_error'] = str(exc)[-500:]
    return result


def publish(host, child, config, reports, documents, state, children):
    family = fleet.family(host)
    baselines, conflicts = recovery.family_baselines(recovery.COLLECTED / host)
    assert not conflicts, conflicts
    for record in state.setdefault('baseline_records', {}).get(family, {}).values():
        baselines.setdefault(record['stable_key'], (host, [record]))
    for path in reports:
        raw, manifest, sidecar, _ = audit._validate_raw_sidecar(path, json.loads(path.read_bytes()),
            json.loads((ROOT / config).read_bytes()),
            {'expected_raw_records': 1, 'cells': [child['execution_cell']]}, expected_repeats=1)
        source = {'report': str(path.relative_to(ROOT)), 'raw_sidecar': str(sidecar.relative_to(ROOT)),
                  'run_id': manifest['run_id'], 'schedule_sorted_checksum': manifest['schedule_sorted_checksum'],
                  'run_receipt_id': None, 'run_receipt_sha256': None}
        record = audit._validate_record(raw[0], source, audit.Q1024_CAMPAIGN_ID, 1024, True,
                                        require_control_queries=False)
        assert record['stable_key'] == child['stable_key']
        if record['identity']['engine'] == 'bioseq':
            assert raw[0].get('gate8_sql') is None, 'Old executable still ran extra controls; do not label as control-free'
        records = [record]
        baseline_donor = None
        if record['identity']['engine'] == 'bioseq':
            baseline_identity = dict(record['identity'], engine='minimap2', bioseq_index_name=None,
                requested_minimizer_algorithm=None, requested_chaining_algorithm=None,
                requested_base_alignment_algorithm=None)
            key = audit._stable_key_for_identity(baseline_identity)
            assert key in baselines, 'Missing saved canonical-fixture minimap2 raw baseline; measured sample retained for later audit'
            baseline_donor = baselines[key][0]
            records.append(next(r for r in baselines[key][1] if r['repeat'] == 0))
        row = next(r for r in audit._aggregate(records, expected_repeats=1) if r['stable_key'] == child['stable_key'])
        emit('fast_audited', host=host, ordinal=child['ordinal'], status=row['status'], comments=row['COMMENTS'])
        if row['status'] != 'accepted':
            return False
        if record['identity']['engine'] == 'minimap2':
            state['baseline_records'].setdefault(family, {})[record['stable_key']] = record
        result = recovery.approved_row(row, fast_local=True)
        result['timing_scope'] = audit.FAST_REMOTE_TIMING_SCOPE
        donor_note = (' Correctness-only minimap2 baseline from ' + baseline_donor
                      + ' for the same canonical q1024 dataset, index mode and k/w; '
                      + ('same-family evidence reused.' if fleet.family(baseline_donor) == family
                         else 'same-family raw evidence unavailable; all available cross-host donor count vectors agree.')) if baseline_donor else ''
        result['comments'] = ((row['COMMENTS'] + '; ') if row['COMMENTS'] else '') + EXPLANATION + 'Measured on ' + host + '.' + donor_note + ' Build/run receipt provenance is not required for publication; no timing was copied from another machine.'
        if family == 'c7g':
            result['comments'] += ' Framework fixture-admission RAM reserve is 20%; PostgreSQL/algorithm settings are unchanged.'
        if state['ready'][host] in state.get('memory_charge_receipts', []):
            result['comments'] += (' Linux process-memory accounting charges anonymous/shared RSS, '
                                   'private swap and explicit huge pages; file-backed RSS remains '
                                   'subject to the independent system-available-memory guard.')
        if host in state.get('priority_profiles', {}):
            result['comments'] += (' PostgreSQL cold starts are configured at nice=-20; '
                                   'the benchmark launcher requests inherited nice=-20 using passwordless sudo. '
                                   'This remains ordinary time-sharing scheduling, not real-time.')
        doc = documents[family]
        if result['stable_key'] not in {r['stable_key'] for r in doc['approved_results']}:
            doc['approved_results'].append(result)
            write_results(family, doc, children)
        if record['identity']['engine'] == 'bioseq' and family not in state.setdefault('verified_families', []):
            state['verified_families'].append(family)
        return True
    raise RuntimeError('No auditable report')


def retire_closed_hosts(state, by_ordinal):
    """Release closed-account reservations locally; retain results and failure evidence."""
    retirement = json.loads((ROOT / 'bench/e2e_fleet_retirement.json').read_bytes())['hosts']
    for host, policy in retirement.items():
        if not policy.get('retired') or host in state.get('retired_hosts', {}):
            continue
        ownership = {key: state.get(key, {}).pop(host) for key in (
            'active', 'ready', 'builds', 'groups', 'refresh_after_terminal')
            if host in state.get(key, {})}
        family = fleet.family(host)
        retries = {p['ordinal'] for p in state['pending']
                   if p['host'] == host and not p.get('reports')
                   and p['error'] in ('No complete terminal report advertised', 'No auditable report')}
        if ownership.get('active'):
            retries.add(ownership['active']['ordinal'])
        # Never turn an already measured/rejected report into an automatic rerun.
        measured = {p['ordinal'] for p in state['pending']
                    if fleet.family(p['host']) == family and p.get('reports')}
        retry_keys = {by_ordinal[n]['stable_key'] for n in retries - measured if n in by_ordinal}
        attempted = state['attempted'].get(family, [])
        removed = sum(k in retry_keys for k in attempted)
        state['attempted'][family] = [k for k in attempted if k not in retry_keys]
        state.setdefault('retired_hosts', {})[host] = {
            'at': time.time(), 'reason': policy.get('reason', 'Host permanently retired'),
            'ownership': ownership, 'unmeasured_attempts_released': removed}
        save(state)
        emit('closed_host_retired', host=host, unmeasured_attempts_released=removed,
             released_index_group=ownership.get('groups'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--daemon', action='store_true')
    parser.add_argument('--plan', action='store_true')
    parser.add_argument('--refresh-family', action='append', choices=tuple(LEADERS), default=[],
                        help='At an idle boundary, retire rollout receipts and retry failed setup rows; preserve all results')
    parser.add_argument('--reason', help='Required explanation for an explicit rollout refresh')
    parser.add_argument('--keep-index-groups', action='store_true',
                        help='With --refresh-family only: preserve group affinity when the repair leaves index semantics/format unchanged')
    parser.add_argument('--drain-running-peers', action='store_true',
                        help='Build on an idle leader now; collect running peers before replacing their runtimes')
    args = parser.parse_args()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    if args.daemon:
        from e2e_queue_supervisor import start
        print(json.dumps({'supervisor_pid': start('fast_remote')})); return
    with (OUTPUT / 'controller.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        recovery.atomic(OUTPUT / 'controller.pid', (str(os.getpid()) + '\n').encode())
        children = sorted((c for c in json.loads(fleet.MANIFEST.read_bytes())['children']
            if 'vulkan' not in json.dumps(c['execution_cell']['identity']).lower()
            and 'ksw2_batched' not in json.dumps(c['execution_cell']['identity'])), key=local.priority)
        by_ordinal = {c['ordinal']: c for c in children}
        state = json.loads(STATE.read_bytes()) if STATE.exists() else {
            'ready': {}, 'builds': {}, 'active': {}, 'attempted': {}, 'groups': {}, 'pending': []}
        LEADERS.update(state.get('leaders', {}))
        if args.refresh_family:
            assert args.reason, '--reason is required'
            for family in args.refresh_family:
                members = ['local'] if family == 'local' else [h for h in fleet.HOSTS if fleet.family(h) == family]
                draining = set()
                for host in members:
                    observed = observe(host)
                    if (args.drain_running_peers and host != LEADERS[family]
                            and observed.get('status') == 'running'):
                        assert host in state['active'] and state['active'][host].get('admitted'), 'Unknown running owner: ' + host
                        assert observed.get('bench_e2e_config') == state['active'][host]['config'], 'Running owner changed: ' + host
                        draining.add(host)
                        continue
                    assert observed.get('status') in (*fleet.TERMINAL_STATUSES, 'missing') and not observed.get('cleanup_unverified'), (host, observed)
                    assert host not in state['active'], 'Collect active terminal run before refresh: ' + host
                retired = {'family': family, 'reason': args.reason, 'at': time.time(), 'ready': {}, 'builds': {}}
                for host in members:
                    if host in draining:
                        state.setdefault('refresh_after_terminal', {})[host] = {
                            'reason': args.reason, 'keep_index_groups': args.keep_index_groups}
                        continue
                    for key in ('ready', 'builds'):
                        if host in state[key]:
                            retired[key][host] = state[key].pop(host)
                    if not args.keep_index_groups:
                        state['groups'].pop(host, None)
                # Only setup failures with no measured report become eligible
                # again. Rejected or accepted measured samples remain durable.
                retry_keys = {by_ordinal[p['ordinal']]['stable_key'] for p in state['pending']
                    if fleet.family(p['host']) == family and not p.get('reports')
                    and p['error'] in ('No complete terminal report advertised', 'No auditable report')}
                state['attempted'][family] = [k for k in state['attempted'].get(family, []) if k not in retry_keys]
                if family == 'local' and local.STATE.exists():
                    local_state = json.loads(local.STATE.read_bytes())
                    assert not local_state.get('active'), 'Drain the terminal local FAST run before refresh'
                    latest = {}
                    for line in (local.OUTPUT / 'events.jsonl').read_text().splitlines():
                        event = json.loads(line)
                        if event.get('event') in ('audit_failed', 'audited'):
                            latest[event['ordinal']] = event
                    all_children = {c['ordinal']: c for c in json.loads(fleet.MANIFEST.read_bytes())['children']}
                    local_retry = {all_children[ordinal]['stable_key'] for ordinal, event in latest.items()
                        if event['event'] == 'audit_failed'
                        and event.get('detail') == 'Terminal fast run has no complete report to audit'}
                    local_state['attempted'] = [k for k in local_state['attempted'] if k not in local_retry]
                    recovery.atomic(local.STATE, (json.dumps(local_state, indent=2) + '\n').encode())
                    retry_keys.update(local_retry)
                state.setdefault('rollout_history', []).append(retired)
                emit('rollout_refresh', family=family, reason=args.reason, setup_rows_requeued=len(retry_keys),
                     draining_peers=sorted(draining))
            save(state)
            return
        documents = {family: document_for(family, children) for family in LEADERS if family != 'local'}
        if args.plan:
            print(json.dumps({'cpu_tuples': len(children), 'groups': len({group_key(c) for c in children}),
                'first_ordinals': [c['ordinal'] for c in children[:24]], 'leaders': LEADERS}, indent=2)); return
        for candidates in BUILD_CANDIDATES.values():
            for host in candidates:
                hold(host, 'Fast campaign owns worker at next terminal boundary; first idle warm-cache worker builds once, siblings import')
        while True:
            quarantine_path = ROOT / 'bench/e2e_fast_quarantine.json'
            quarantined = json.loads(quarantine_path.read_bytes()).get('families', {}) if quarantine_path.exists() else {}
            registered = fleet.monitor.registered_hosts()
            fleet.HOSTS = {h[0]: h for h in registered}
            retire_closed_hosts(state, by_ordinal)
            hosts = ['local', *(h[0] for h in registered)]
            with ThreadPoolExecutor(max_workers=6) as pool:
                observed = dict(zip(hosts, pool.map(observe, hosts)))
            for family, candidates in BUILD_CANDIDATES.items():
                leader = LEADERS[family]
                if leader in state['builds'] or leader in state['ready']:
                    continue
                terminal = next((h for h in candidates
                    if observed[h].get('status') in fleet.TERMINAL_STATUSES
                    and not observed[h].get('cleanup_unverified')), None)
                if terminal and terminal != leader:
                    LEADERS[family] = terminal
                    state['leaders'] = dict(LEADERS)
                    save(state); emit('first_idle_builder_selected', host=terminal, family=family)
            for host in hosts:
                try:
                    if fleet.family(host) in quarantined:
                        continue
                    if host not in state['ready']:
                        rollout(host, observed[host], state)
                except Exception as exc:
                    emit('rollout_error', host=host, detail=str(exc))
            approved = {family: standard_keys(family) | {r['stable_key'] for r in doc['approved_results']}
                        for family, doc in documents.items()}
            for host in hosts:
                if host == 'local' or host not in state['ready']:
                    continue
                try:
                    current = observe(host)
                    if current.get('status') == 'running' or current.get('cleanup_unverified'):
                        continue
                    # An MCP-imported fresh replica has a verified frozen
                    # receipt in ready, but no benchmark run state until its
                    # first launch. Do not leave these workers idle forever.
                    fresh_import = (current.get('status') == 'missing'
                                    and host not in state['active'])
                    if current.get('status') not in fleet.TERMINAL_STATUSES and not fresh_import:
                        continue
                    family = fleet.family(host)
                    active = state['active'].get(host)
                    if active:
                        assert current.get('bench_e2e_config') == active['config'], 'Fast run owner changed'
                        child = by_ordinal[active['ordinal']]
                        if child['stable_key'] not in state['attempted'].setdefault(family, []):
                            state['attempted'][family].append(child['stable_key'])
                        reports = []
                        try:
                            reports = fetch_reports(host, current)
                            publish(host, child, active['config'], reports, documents, state, children)
                        except Exception as exc:
                            failure = failure_evidence(host, current)
                            state['pending'].append({'host': host, **active, 'error': str(exc),
                                                     'reports': [str(p) for p in reports], **failure})
                            emit('fast_audit_failed', host=host, ordinal=active['ordinal'],
                                 detail=str(exc), **failure)
                        state['active'].pop(host)
                        save(state)
                    deferred_refresh = state.get('refresh_after_terminal', {}).pop(host, None)
                    if deferred_refresh:
                        # Publication above always precedes replacement. The
                        # draining peer cannot start another old-runtime tuple.
                        retired = {'host': host, 'reason': deferred_refresh['reason'],
                                   'at': time.time(), 'ready': state['ready'].pop(host),
                                   'build': state['builds'].pop(host, None)}
                        state.setdefault('rollout_history', []).append(retired)
                        if (not deferred_refresh['keep_index_groups']
                                or current.get('status') in ('killed_timeout', 'aborted')):
                            state['groups'].pop(host, None)
                        retry_keys = {by_ordinal[p['ordinal']]['stable_key'] for p in state['pending']
                            if p['host'] == host and not p.get('reports')
                            and p['error'] in ('No complete terminal report advertised', 'No auditable report')}
                        state['attempted'][family] = [k for k in state['attempted'].get(family, []) if k not in retry_keys]
                        save(state); emit('drained_peer_ready_for_runtime_copy', host=host,
                                          reason=deferred_refresh['reason'])
                        continue
                    if family in quarantined:
                        continue
                    excluded = approved[family] | set(state['attempted'].get(family, []))
                    # A sibling can still be draining its last five-run tuple.
                    # Reserve it until its results have been fetched and audited.
                    standard_inflight = {observed[h].get('bench_e2e_config') for h in hosts
                        if h != 'local' and fleet.family(h) == family and h not in state['ready']}
                    excluded.update(c['stable_key'] for c in children if c['path'] in standard_inflight)
                    excluded.update(by_ordinal[a['ordinal']]['stable_key'] for h,a in state['active'].items()
                                    if fleet.family(h) == family)
                    remaining = [c for c in children if c['stable_key'] not in excluded]
                    just_index_active = any(
                        fleet.family(h) == family and by_ordinal[a['ordinal']]['execution_cell']['identity']['index_mode']=='just_index'
                        for h,a in state['active'].items())
                    just_index_draining = any(c['path'] in standard_inflight
                        and c['execution_cell']['identity']['index_mode']=='just_index' for c in children)
                    if (just_index_active or just_index_draining
                            or any(c['execution_cell']['identity']['index_mode']=='just_index' for c in remaining)):
                        remaining = [c for c in remaining if c['execution_cell']['identity']['index_mode']=='just_index']
                    owned = {group for h,group in state['groups'].items() if h != host and fleet.family(h)==family}
                    candidates = [c for c in remaining if group_key(c) not in owned]
                    current_group = state['groups'].get(host)
                    same_group = [c for c in candidates if group_key(c)==current_group]
                    if not candidates:
                        state['groups'].pop(host, None); save(state); continue
                    child = (same_group or candidates)[0]
                    state['groups'][host] = group_key(child)
                    launch(host, child, state)
                except Exception as exc:
                    emit('queue_error', host=host, detail=str(exc))
            # A missing raw baseline is not a reason to rerun a saved sample.
            # Revisit only those samples, and only when baseline availability changes.
            for pending in state['pending']:
                if (not any(message in pending['error'] for message in (
                        'Missing saved same-family minimap2 raw baseline',
                        'Missing saved canonical-fixture minimap2 raw baseline'))
                        or not pending.get('reports')):
                    continue
                family = fleet.family(pending['host'])
                available, _ = recovery.family_baselines(recovery.COLLECTED / pending['host'])
                signature = sorted(set(available) | set(state.get('baseline_records', {}).get(family, {})))
                if pending.get('baseline_keys') == signature:
                    continue
                pending['baseline_keys'] = signature
                try:
                    publish(pending['host'], by_ordinal[pending['ordinal']], pending['config'],
                            [Path(p) for p in pending['reports']], documents, state, children)
                    pending['error'] = 're-audited from saved evidence; no rerun'
                except Exception as exc:
                    emit('pending_baseline_audit', host=pending['host'], ordinal=pending['ordinal'], detail=str(exc))
            if time.time() - state.get('last_log', 0) > 3600:
                fleet.append_progress_log('Fast remote coverage: ' + ', '.join(
                    family.upper() + '=' + str(len(doc['approved_results'])) for family,doc in documents.items())
                    + '. Rolling one-time executable update removes extra full mapping controls; grouped one-sample queues own ready workers. Existing runs are drained, not cancelled.',
                    title='Fast remote campaign checkpoint')
                state['last_log'] = time.time()
            save(state)
            time.sleep(60)


if __name__ == '__main__':
    main()
