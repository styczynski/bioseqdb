#!/usr/bin/env python3
"""Recover complete FAST local reports using the existing auditor; never dispatch."""
import json
from pathlib import Path

import fast_local_e2e as runner


def collect():
    children = json.loads(runner.recovery.MANIFEST.read_text())['children']
    by_key = {child['stable_key']: child for child in children}
    out = runner.document()
    approved = {row['stable_key'] for row in out['approved_results']}
    approved.update(row['stable_key'] for row in runner.load_yaml(
        runner.ROOT / 'ALL_RESULTS_TUPLES.yaml')['approved_results'])
    summary = dict(complete_single_sample_reports=0, already_collected=0,
                   newly_accepted=[], unaccepted=[])
    root = runner.ROOT / 'reporting/bioseq_benchmark_e2e'
    for path in sorted(root.glob('report_[0-9]*.json')):
        report = json.loads(path.read_text())
        evidence = report.get('raw_evidence') or {}
        if evidence.get('complete') is not True or evidence.get('record_count') != 1:
            continue
        raw_path = path.parent / evidence['path']
        with raw_path.open() as stream:
            raw = next((json.loads(line) for line in stream
                        if '"kind":"header"' not in line.replace(' ', '')), None)
        if not raw or raw.get('benchmark_shape', {}).get('expected_query_rows') != 1024:
            continue
        p, a = raw.get('pairing', {}), raw.get('params', {})
        engine = 'minimap2' if raw['algorithm'].startswith('minimap2') else 'bioseq'
        identity = dict(zip(runner.audit.STABLE_KEY_FIELDS, [
            runner.audit.Q1024_CAMPAIGN_ID, p.get('dataset_key'), p.get('alphabet'),
            p.get('target_size_bp', 0) // 1_000_000_000, p.get('kmer_size'),
            p.get('window_size'), p.get('mode'), engine,
            p.get('index_key') if engine == 'bioseq' else None,
            a.get('minimizer_algorithm') if engine == 'bioseq' else None,
            a.get('chaining_algorithm') if engine == 'bioseq' else None,
            a.get('base_alignment_algorithm') if engine == 'bioseq' else None]))
        key = runner.audit._stable_key_for_identity(identity)
        if key not in by_key:
            continue
        summary['complete_single_sample_reports'] += 1
        if key in approved:
            summary['already_collected'] += 1
            continue
        child = by_key[key]
        config = runner.CONFIGS / f"tuple_{child['ordinal']:04d}.json"
        try:
            if not config.is_file():
                raise RuntimeError('No matching original FAST configuration')
            accepted = runner.publish(child, config, [str(path)], out)
            if accepted:
                approved.add(key)
                summary['newly_accepted'].append(child['ordinal'])
            else:
                raise RuntimeError('Existing auditor rejected the measurement')
        except Exception as error:
            summary['unaccepted'].append(dict(report=path.name,
                ordinal=child['ordinal'], reason=str(error)))
    runner.event('final_collection_scan', **summary)
    return summary


if __name__ == '__main__':
    print(json.dumps(collect(), indent=2))
