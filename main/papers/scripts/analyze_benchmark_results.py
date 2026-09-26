#!/usr/bin/env python3
"""Print the thesis's matched-configuration audit as JSON; runs no benchmarks.

Uses the same minimum-A observation selection as generate_benchmark_results.py.
Run with the papers Python environment (PyYAML is required). Ratios describe
selected complete-query times, not paired repeats or isolated stage speedups.
"""
from __future__ import annotations

import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
from statistics import median

from generate_benchmark_results import ROOT, Snapshot, execution_id


def eligible(row):
    return (row['run_count'] == 5
            and row['execution_conformance'] == 'exact_requested_path'
            and row['requested_algorithms'] == row['observed_algorithms']
            and not any(str(value).startswith('vulkan')
                        for value in row['requested_algorithms'].values()))


def matched_pairs(snapshot):
    """Require complete triples for chaining, complete pairs for other choices."""
    rows = {(host, execution_id(execution), config): row
            for (host, _, execution, config), row in snapshot.rows.items()
            if row['pipeline'] == 'bioseq' and config[3] == 'dna4' and eligible(row)}
    sources = {(host, execution_id(execution), config): source
               for (host, _, execution, config), source in snapshot.sources.items()}
    cohorts = defaultdict(list)

    def add(name, keys):
        left, right = (rows[key] for key in keys)
        host, _, (mode, size, (k, w), alphabet) = keys[0]
        entry = dict(host=host, mode=mode, size_gbp=size, k=k, w=w, alphabet=alphabet)
        for field, key, row in zip(('numerator', 'denominator'), keys, (left, right)):
            entry[field] = dict(configuration=key[1], source=sources[key],
                                a_ns=row['metrics']['p50_a'], b_ns=row['metrics']['p50_b'])
        entry['a_ratio'] = left['metrics']['p50_a'] / right['metrics']['p50_a']
        cohorts[name].append(entry)

    for key in sorted(rows):
        host, execution, config = key
        if execution == 'mpf-bounded-scalar-block' and config[0] == 'just_index':
            triple = [(host, f'mpf-bounded-{chain}-block', config)
                      for chain in ('scalar', 'simd', 'rmq')]
            if all(candidate in rows for candidate in triple):
                same_source = len({sources[candidate] for candidate in triple}) == 1
                for name, left, right in (('simd_over_scalar', 1, 0),
                                          ('rmq_over_scalar', 2, 0),
                                          ('rmq_over_simd', 2, 1)):
                    add('chaining_' + name, (triple[left], triple[right]))
                    if same_source:
                        add('chaining_same_source_' + name, (triple[left], triple[right]))
        if execution.startswith('mpf-bounded-'):
            other = (host, execution.replace('mpf-', 'mps-', 1), config)
            if other in rows:
                add('mpf_over_mps', (key, other))
        if execution.startswith('mpf-') and execution.endswith('-block'):
            other = (host, execution.removesuffix('-block') + '-ksw2', config)
            if other in rows:
                add('block_over_ksw2', (key, other))
    return cohorts


def summarize(pairs):
    groups = defaultdict(list)
    for pair in pairs:
        region = 'Local' if pair['host'] == 'Local' else 'AWS'
        for profile in ('all', f"k{pair['k']}_w{pair['w']}"):
            groups[(region, pair['mode'], profile)].append(pair)
    out = []
    for (region, mode, profile), selected in sorted(groups.items()):
        wins = ties = 0
        for pair in selected:
            a, b = (pair[field]['a_ns'] for field in ('numerator', 'denominator'))
            tied = 100 * max(a, b) <= 105 * min(a, b)
            ties += tied
            wins += not tied and a < b
        out.append(dict(region=region, mode=mode, profile=profile, pairs=len(selected),
                        median_a_ratio=median(pair['a_ratio'] for pair in selected),
                        numerator_wins=wins, ties=ties,
                        numerator_losses=len(selected) - wins - ties))
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    args = parser.parse_args()
    snapshot = Snapshot(args.root)
    cohorts = matched_pairs(snapshot)
    result = dict(
        schema='bioseq.thesis.matched-configuration-audit.v1',
        selection='Minimum A per table cell; B, repeat count and source stay together.',
        filters=dict(repeats=5, alphabet='dna4', conformance='exact_requested_path',
                     cpu_only=True, chaining='Indexed MPF/Bounded/Block complete triples.',
                     layout='Bounded MPF/MPS pairs with chaining and base aligner fixed.',
                     alignment='MPF Block/KSW2 pairs with CPU minimizer and chaining fixed.',
                     sensitivity='Same-source chaining requires all three rows in one YAML.',
                     ties='Symmetric 5% band, using exact integer A comparisons.',
                     outliers='No elapsed-time cutoff; summarize ratios by their median.'),
        caveat='Machine classes can combine replicas/builds; ratios are descriptive, not causal.',
        sources={name: hashlib.sha256(raw).hexdigest() for name, raw in sorted(snapshot.raw.items())},
        cohorts={name: dict(summary=summarize(pairs), pairs=pairs)
                 for name, pairs in sorted(cohorts.items())})
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
