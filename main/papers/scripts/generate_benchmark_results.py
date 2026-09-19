#!/usr/bin/env python3
"""Generate thesis result tables manually (requires PyYAML from papers/requirements.txt).

Run: python main/papers/scripts/generate_benchmark_results.py
No build-system integration. --check checks the committed generated output without writing.
The five YAML approved_results datasets are authoritative. No benchmark is executed.
"""
from __future__ import annotations
import argparse
from collections import Counter
from decimal import Decimal, ROUND_HALF_UP
import hashlib
import itertools
import json
from pathlib import Path
import sys
import yaml

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / 'main/papers/src/content/bioseqdb/thesis/chapters/16_benchmarking/generated_results.tex'
HOSTS = ('Local', 'M7A', 'M7I', 'M4', 'C7G')
FILES = dict(zip(HOSTS, ['ALL_RESULTS_TUPLES.yaml'] + [f'ALL_RESULTS_TUPLES_REMOTE_{h}.yaml' for h in HOSTS[1:]]))
CURRENT = 'query_only_framework_iteration_v1'
HISTORICAL = 'historical_outer_e2e_v1'
STAGES = ('minimizer', 'chaining', 'base_alignment')
MINIMIZERS = {'scalar': 'Scalar', 'simd_minimap2_reference': 'SIMD-ref', 'two_stack_nthash': 'Two-stack', 'bounded_nthash_simd': 'Bounded', 'vulkan_minimap_integer': 'Vulkan'}
CHAINS = {'scalar_minimap2': 'Scalar-DP', 'simd_minimap2': 'SIMD-DP', 'range_minimum_query': 'RMQ', 'vulkan_tiled_dynamic_programming': 'Vulkan-DP'}
BASES = {'ksw2': 'KSW2', 'block_aligner': 'Block'}
CONFIGS = list(itertools.product(('just_index', 'no_index'), (4, 2), ((15, 10), (20, 15)), ('dna4', 'dna5')))
BASELINE = ('minimap2', 'minimap2_internal', None, None, None)


def tex(value):
    """Escape metadata as literal LaTeX text."""
    replacements = {'\\': r'\textbackslash{}', '&': r'\&', '%': r'\%', '$': r'\$', '#': r'\#', '_': r'\_', '{': r'\{', '}': r'\}', '~': r'\textasciitilde{}', '^': r'\textasciicircum{}'}
    return ''.join(replacements.get(c, c) for c in str(value))


def duration(ns):
    """Round to tenths using integers, carrying seconds into minutes and hours."""
    ticks = (ns + 50_000_000) // 100_000_000
    hours, ticks = divmod(ticks, 36000)
    minutes, ticks = divmod(ticks, 600)
    seconds = f'{ticks / 10:g}'
    if hours:
        return f'{hours}h {minutes}m {seconds}s'
    if minutes:
        return f'{minutes}m {seconds}s'
    return f'{seconds}s' if ticks else '<0.1s'


def comparison(value, reference):
    """Use symmetric 5% ties, relative latency percent, ratios above 30%."""
    if reference is None:
        return r'\textcolor{gray}{n/a}'
    low, high = sorted((value, reference))
    tie = 100 * high <= 105 * low
    color, arrow = ('gray', r'\blacktriangleright') if tie else (('ForestGreen', r'\blacktriangle') if value < reference else ('BrickRed', r'\blacktriangledown'))
    delta = abs(value - reference)
    if delta * 100 > reference * 30:
        ratio = (Decimal(high) / Decimal(low)).quantize(Decimal('.1'), rounding=ROUND_HALF_UP)
        number = f'{ratio}' + r'\times'
    else:
        percent = (Decimal(delta) * 100 / Decimal(reference)).quantize(Decimal('1'), rounding=ROUND_HALF_UP)
        number = f'{percent}' + r'\%'
    return rf'\textcolor{{{color}}}{{$ {arrow}\,{number} $}}'


def row_key(row):
    return (row['pipeline'], row['index_type'], *(row['requested_algorithms'][s] for s in STAGES))


def config_key(row):
    return (row['mode'], row['target_size_gbp'], (row['kmer_size'], row['window_size']), row['alphabet'])


def validate_row(row, unavailable=False):
    """Reject invalid, ambiguous or unsupported publication inputs."""
    expected = [row['campaign_id'], row['dataset'], row['alphabet'], row['target_size_gbp'], row['kmer_size'], row['window_size'], row['mode'], row['pipeline'], row['index_type'] if row['pipeline'] == 'bioseq' else None, *(row['requested_algorithms'][s] for s in STAGES)]
    assert row['stable_key_schema'] == 'bioseq-e2e-cell-v2'
    assert row['stable_key'].startswith('bioseq-e2e-cell-v2:')
    assert json.loads(row['stable_key'].split(':', 1)[1]) == expected, 'stable-key projection mismatch'
    assert row['dataset'] == f"matrix_{row['target_size_gbp']}gbp_{row['alphabet']}_k{row['kmer_size']}_w{row['window_size']}"
    assert row['campaign_id'] == 'bioseq-e2e-algorithm-matrix-local-v1'
    assert config_key(row) in CONFIGS
    assert row['query_count'] == 1024 and row['prefilter'] is False
    assert row['metrics']['unit'] == 'ns'
    if row['pipeline'] == 'bioseq':
        assert row['index_type'] in ('minimizers_per_fragment', 'minimizers_per_sequence')
        assert row['pipeline_mode'] == 'task_parallel'
        assert row['requested_algorithms']['minimizer'] in MINIMIZERS
        assert row['requested_algorithms']['chaining'] in CHAINS
        assert row['requested_algorithms']['base_alignment'] in BASES
    else:
        assert row_key(row) == BASELINE
    if unavailable:
        assert row['status'] == 'hardware_unavailable' and row['execution_attempted'] is False
        assert row['run_count'] == 0
        assert row['metrics']['p50_a'] is None and row['metrics']['p50_b'] is None
        return
    assert row['status'] == 'accepted'
    assert type(row['run_count']) is int and row['run_count'] == 5, 'update the five-repeat methodology before publishing other repeat counts'
    assert row['timing_scope'] in (CURRENT, HISTORICAL)
    assert row['exact_baseline_match_count_equal'] is True
    assert row['comparison_eligibility']['effective_path'] is True, 'unapproved effective-path comparison'
    a, b = (row['metrics'][k] for k in ('p50_a', 'p50_b'))
    assert type(a) is int and type(b) is int and 0 < b <= a
    assert row['execution_conformance'] in ('exact_requested_path', 'exact_requested_path_user_preserved', 'external_minimap2_baseline', 'documented_scalar_minimizer_fallback')
    if row['execution_conformance'] == 'documented_scalar_minimizer_fallback':
        assert row['observed_algorithms'] == dict(row['requested_algorithms'], minimizer='scalar')
        assert row['comparison_eligibility']['requested_path'] is False
    else:
        assert row['observed_algorithms'] == row['requested_algorithms']


class Snapshot:
    """An immutable-in-memory, validated read of the five publication authorities."""
    def __init__(self, root):
        self.raw, self.docs, self.rows, self.unavailable = {}, {}, {}, set()
        self.seen = set()
        for host, name in FILES.items():
            raw = (root / name).read_bytes()
            doc = yaml.safe_load(raw)
            self.raw[host], self.docs[host] = raw, doc
            if host != 'Local':
                assert doc['remote_host_binding']['machine_id'] == host
            identities = set()
            for row in doc['approved_results']:
                validate_row(row)
                identity = (row['stable_key'], row['timing_scope'])
                assert identity not in identities, f'duplicate observation {host} {identity}'
                identities.add(identity)
                key = (host, row['timing_scope'], row_key(row), config_key(row))
                assert key not in self.rows, f'display collision: {key}'
                self.rows[key] = row
            for row in doc.get('hardware_unavailable_cells', []):
                validate_row(row, True)
                key = (host, CURRENT, row_key(row), config_key(row))
                assert key not in self.rows and key not in self.unavailable
                self.unavailable.add(key)
        for key, row in self.rows.items():
            if row['pipeline'] == 'bioseq':
                assert (key[0], key[1], BASELINE, key[3]) in self.rows, f'missing matched baseline {key}'

    def minimum(self, host, scope, config, metric, bioseq_only=False, execution=None):
        candidates = [r['metrics'][metric] for (h, s, rk, cfg), r in self.rows.items()
                      if h == host and s == scope and (config is None or cfg == config)
                      and (not bioseq_only or r['pipeline'] == 'bioseq')
                      and (execution is None or rk == execution)
                      and r['comparison_eligibility']['effective_path']]
        return min(candidates) if candidates else None

    def cell(self, host, scope, execution, config, remote):
        key = (host, scope, execution, config)
        row = self.rows.get(key)
        if row is None:
            return r'\textcolor{gray}{---\textsuperscript{g}}' if key in self.unavailable else r'\textcolor{gray}{\ensuremath{\cdot}}'
        self.seen.add(key)
        marker = r'\textsuperscript{f}' if row['execution_conformance'] == 'documented_scalar_minimizer_fallback' else (r'\textsuperscript{p}' if row['execution_conformance'] == 'exact_requested_path_user_preserved' else '')
        parts = []
        for metric in ('p50_a', 'p50_b'):
            value = row['metrics'][metric]
            if row['pipeline'] == 'bioseq':
                ref = self.rows[(host, scope, BASELINE, config)]['metrics'][metric]
            else:
                ref = self.minimum(host, scope, config, metric, bioseq_only=True)
            best = self.minimum(host, scope, None if remote else config, metric, execution=execution if remote else None)
            timing = tex(duration(value)).replace(' ', r'\,')
            if value >= 3_600_000_000_000:
                timing = r'{\fontsize{6.2}{7}\selectfont ' + timing + '}'
            if value == best:
                timing = r'\colorbox{BenchBest}{\strut ' + timing + '}'
            parts += [timing, r'{\fontsize{5.8}{6.4}\selectfont ' + comparison(value, ref) + marker + '}']
        return r'\begin{tabular}{@{}c@{}}' + r'\\[-.2pt]'.join(parts) + r'\end{tabular}'


def execution_label(key):
    if key == BASELINE:
        return 'minimap2', r'\textbf{Baseline}'
    _, index, minimizer, chain, base = key
    label = ('MPF' if index == 'minimizers_per_fragment' else 'MPS') + ' / ' + MINIMIZERS[minimizer]
    return 'BioseqDB', r'\begin{tabular}{@{}l@{}}' + tex(label) + r'\\' + tex(CHAINS[chain] + ' / ' + BASES[base]) + r'\end{tabular}'


def hardware_table(snapshot):
    local = snapshot.docs['Local']['benchmark_host_profile']
    remote = [snapshot.docs[h]['remote_host_binding'] for h in HOSTS[1:]]
    rows = [
        ('Machine', [local['machine']['name']] + [h['instance_type'] for h in remote]),
        ('CPU', [local['machine']['chip']] + [h['cpu']['model'].replace('Intel(R) ', '').replace('(R)', '').replace(' CPU ', ' ') for h in remote]),
        ('Architecture', [local['architecture']['name']] + [h['architecture'] for h in remote]),
        ('OS version / kernel', [f"macOS {local['operating_system']['version']} / {local['operating_system']['kernel_release']}"] + [h['receipt_host']['system'] + ' / ' + h['receipt_host']['release'] for h in remote]),
        ('OS build / distribution', [local['operating_system']['build']] + ['Distribution not recorded'] * 4),
        ('Logical / physical cores', [f"{local['cpu']['logical_cores']} / {local['cpu']['physical_cores']} (6P + 2E)"] + [f"{h['cpu']['vcpu_count']} / {h['cpu']['core_count']}" for h in remote]),
        ('Physical RAM', [local['memory']['human'] + ' unified'] + [f"{h['memory']['physical_mib'] / 1024:g} GiB" for h in remote]),
        ('GPU', [f"{local['gpu']['model']}, {local['gpu']['core_count']} cores"] + ['None available'] * 4),
        ('Storage', [local['physical_storage']['model']] + [f"EBS {h['storage']['volume_type']}" for h in remote]),
        ('Disk capacity', [f"{local['physical_storage']['capacity_bytes'] / 1e9:.2f} GB"] + [f"{h['storage']['capacity_gib']} GiB" for h in remote]),
        ('Provisioned disk I/O', ['Not recorded'] + [f"{h['storage']['provisioned_iops']:,} IOPS / {h['storage']['provisioned_throughput_mib_per_s']} MiB/s" for h in remote]),
        ('Free disk / free RAM', ['No uniform per-repeat record'] * 5),
    ]
    out = [r'\subsection{Recorded machines and completed coverage}', r'\begin{table}[H]\centering\small', r'\caption{Machine inventory captured with the benchmark summaries. Capacities are not free resources at execution time.}', r'\begin{tabularx}{\linewidth}{@{}p{24mm}*{5}{>{\raggedright\arraybackslash}X}@{}}\toprule', 'Property & ' + ' & '.join(HOSTS) + r'\\\midrule']
    for title, values in rows:
        out.append(tex(title) + ' & ' + ' & '.join(tex(v) for v in values) + r'\\[3pt]')
    out += [r'\bottomrule\end{tabularx}\end{table}', r'\noindent The AWS summaries record the Linux kernel but do not seal an OS distribution version. The local legacy capture does not seal the MoltenVK implementation identity. Unrecorded values are not inferred from the current machine.']
    out += [r'\begin{table}[H]\centering\small', r'\caption{Approved snapshot coverage. Observations are separate for each timing scope.}', r'\begin{tabular}{lrrrrr}\toprule Host & Logical tuples & Observations & Repeats & BioseqDB obs. & GPU unavailable\\\midrule']
    for host in HOSTS:
        rows = snapshot.docs[host]['approved_results']
        out.append(f"{host} & {len({r['stable_key'] for r in rows})} & {len(rows)} & {sum(r['run_count'] for r in rows)} & {sum(r['pipeline']=='bioseq' for r in rows)} & {len(snapshot.docs[host].get('hardware_unavailable_cells', []))}" + r'\\')
    out += [r'\bottomrule\end{tabular}\end{table}']
    total = len(snapshot.rows)
    runs = sum(r['run_count'] for r in snapshot.rows.values())
    scopes = Counter(r['timing_scope'] for r in snapshot.docs['Local']['approved_results'])
    out += [f'This snapshot contains {total} timing observations and {runs:,} accepted repeats. The local results contain {scopes[CURRENT]} current mapping observations and {scopes[HISTORICAL]} historical outer-E2E observations. Every published observation has five accepted repeats. The planned local matrix has 1,296 logical cells and 6,480 repeats. It remains incomplete. Unavailable-GPU counts refer only to the remote authorities\' recorded eligible subsets.']
    return out


def header(configs, remote):
    """Render a contiguous hierarchy with machine separators in the lowest tier."""
    expanded = [(cfg, h) for cfg in configs for h in (HOSTS[1:] if remote else ('Local',))]
    levels = [lambda c, h: 'Index' if c[0] == 'just_index' else 'No index', lambda c, h: f'{c[1]} Gbp', lambda c, h: f'k={c[2][0]}, w={c[2][1]}', lambda c, h: c[3].upper()]
    if remote:
        levels.append(lambda c, h: h)
    out = []
    for level, getter in enumerate(levels):
        labels = [(tuple(get(c, h) for get in levels[:level + 1]), getter(c, h)) for c, h in expanded]
        groups = [(key[1], len(list(group))) for key, group in itertools.groupby(labels)]
        left = 'Engine & Execution' if level == 0 else ' & '
        out.append(left + ' & ' + ' & '.join(rf'\multicolumn{{{count}}}{{c|}}{{\textbf{{{tex(label)}}}}}' for label, count in groups) + r'\\')
    return out


def result_pages(snapshot, scope, remote, configs, title):
    hosts = HOSTS[1:] if remote else ('Local',)
    relevant = {key[2] for key in set(snapshot.rows) | snapshot.unavailable if key[0] in hosts and key[1] == scope and key[3] in configs}
    # Keep row order identical across panels, and repeat baseline as a reading aid.
    keys = sorted(relevant - {BASELINE})
    page_count = max(1, (len(keys) + 6) // 7)
    batches = [keys[i * len(keys) // page_count:(i + 1) * len(keys) // page_count]
               for i in range(page_count)]
    out = []
    for page, batch in enumerate(batches, 1):
        out += [r'\clearpage', r'\noindent{\large\bfseries ' + tex(title) + rf'\hfill {page}/{len(batches)}' + r'}\par\smallskip', r'{\fontsize{7.1}{8.1}\selectfont\setlength{\tabcolsep}{1pt}\renewcommand{\arraystretch}{1.12}', r'\noindent\begin{tabular}{@{}p{14mm}p{27mm}|' + ('p{13.55mm}|' * 16) + r'@{}}\hline']
        out += header(configs, remote)
        out.append(r'\hline')
        for n, key in enumerate([BASELINE] + batch):
            engine, label = execution_label(key)
            engine_label = r'\textbf{' + engine + '}' if n <= 1 else ''
            cells = [snapshot.cell(h, scope, key, cfg, remote) for cfg in configs for h in hosts]
            assert len(cells) == 16
            out.append(engine_label + ' & ' + label + ' & ' + ' & '.join(cells) + r'\\[3pt]\hline')
        out += [r'\end{tabular}\par}', r'\smallskip{\footnotesize\noindent Each cell: A, comparison, B, comparison. Times cover 1,024 queries. $\cdot$ = no approved observation. ---\textsuperscript{g} = GPU unavailable, not executed. n/a = no measured comparison.\par', r'\noindent \textsuperscript{f}SIMD-ref request observed as scalar, compared only as that effective path. \textsuperscript{p}Accepted by explicit preservation after wrapper postflight failure, no run receipt claimed.\par', r'\noindent MPF/MPS = minimizers per fragment/sequence. Row order: index representation / minimizer, then chaining / base alignment. SIMD-ref is the reference minimizer, Bounded is bounded ntHash.\par']
        if remote:
            out += [r'\noindent Blue marks the minimum for each metric and machine across configuration columns within the same execution row, across all AWS panels. This compares different workloads, not algorithm superiority.\par}']
        else:
            out += [r'\noindent Blue marks each metric\textquotesingle s column minimum over measured execution rows in this timing scope. Baseline comparisons use the fastest measured BioseqDB effective path separately for A and B.\par}']
    return out


def render(snapshot):
    out = ['% Generated by main/papers/scripts/generate_benchmark_results.py. Do not edit.', '% Source byte hashes (regenerate manually after approved results change).']
    for host, raw in snapshot.raw.items():
        out.append('% ' + FILES[host] + ' sha256=' + hashlib.sha256(raw).hexdigest())
    out += [r'\definecolor{BenchBest}{HTML}{DDECF8}', r'\newgeometry{left=10mm,right=10mm,top=12mm,bottom=12mm}', r'\begin{landscape}']
    out += hardware_table(snapshot)
    out += result_pages(snapshot, CURRENT, False, CONFIGS, 'Local machine: current mapping interval')
    out += result_pages(snapshot, HISTORICAL, False, CONFIGS, 'Local machine: historical outer E2E (separate scope)')
    for size, kw in itertools.product((4, 2), ((15, 10), (20, 15))):
        configs = [cfg for cfg in CONFIGS if cfg[1:3] == (size, kw)]
        out += result_pages(snapshot, CURRENT, True, configs, f'AWS: {size} Gbp, k={kw[0]}, w={kw[1]} (current mapping interval)')
    out += [r'\end{landscape}', r'\restoregeometry', '']
    assert snapshot.seen == set(snapshot.rows), f'unrendered observations: {set(snapshot.rows) - snapshot.seen}'
    return '\n'.join(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--output', type=Path, default=OUTPUT)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    if not __debug__:
        parser.error('Do not run with -O: input validation uses assertions.')
    snapshot = Snapshot(args.root)
    result = render(snapshot)
    for host, name in FILES.items():
        if (args.root / name).read_bytes() != snapshot.raw[host]:
            raise RuntimeError(f'{name} changed during generation. Rerun for a consistent snapshot.')
    if args.check:
        if not args.output.exists() or args.output.read_text() != result:
            print(f'Stale generated results: {args.output}', file=sys.stderr)
            return 1
    else:
        args.output.write_text(result)
    print(f"{'Verified' if args.check else 'Generated'} {args.output}: {len(snapshot.rows)} observations, {sum(r['run_count'] for r in snapshot.rows.values())} repeats; every observation rendered.")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
