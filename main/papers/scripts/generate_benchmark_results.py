#!/usr/bin/env python3
"""Generate thesis result tables manually (requires PyYAML from papers/requirements.txt).

Run: python main/papers/scripts/generate_benchmark_results.py
No build-system integration. --check checks the committed generated output without writing.
The YAML approved_results datasets are authoritative. No benchmark is executed.
"""
from __future__ import annotations
import argparse
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
REMOTE_FALLBACKS = ('TEST3', 'TEST4', 'TEST7', 'TEST2')
EXTERNAL_FILES = {h: f'ALL_RESULTS_TUPLES_E2E_EXTERNAL_REMOTE_{h}.yaml' for h in HOSTS[1:]}
EXTERNAL_ALIGNERS = ('blastn', 'winnowmap', 'lexicmap')
ALIGNER_ORDER = ('minimap2', 'bioseq', *EXTERNAL_ALIGNERS)
BIOSEQ_COMPARISON = ('bioseq', 'minimizers_per_fragment', 'bounded_nthash_simd', 'simd_minimap2', 'block_aligner')
EXTERNAL_SCOPE = 'external_process_iteration_v1'
CURRENT = 'query_only_framework_iteration_v1'
HISTORICAL = 'historical_outer_e2e_v1'
FAST_LOCAL = 'query_only_framework_iteration_fast_local_single_sample_v1'
FAST_REMOTE = 'query_only_framework_iteration_fast_remote_single_sample_v1'
DISPLAY_SCOPE = 'mapping_comparison'  # Display grouping only; each row keeps its actual timing_scope.
FAST_NOTE = 'FAST: one measured batch of 1,024 queries. The reported P50 is that single datapoint, not a five-repeat estimate. Samples are not pooled with standard runs.'
STAGES = ('minimizer', 'chaining', 'base_alignment')
MINIMIZERS = {'scalar': 'Scalar', 'simd_minimap2_reference': 'SIMD-ref', 'two_stack_nthash': 'Two-stack', 'bounded_nthash_simd': 'Bounded', 'vulkan_minimap_integer': 'Vulkan'}
CHAINS = {'scalar_minimap2': 'Scalar-DP', 'simd_minimap2': 'SIMD-DP', 'range_minimum_query': 'RMQ', 'vulkan_tiled_dynamic_programming': 'Vulkan-DP'}
BASES = {'ksw2': 'KSW2', 'block_aligner': 'Block'}
CONFIGS = list(itertools.product(('just_index', 'no_index'), (4, 2), ((15, 10), (20, 15)), ('dna4', 'dna5')))
BASELINE = ('minimap2', 'minimap2_internal', None, None, None)
VULKAN_SUMMARY = ('bioseq', 'vulkan_all', None, None, None)


def is_vulkan(execution):
    return any(isinstance(value, str) and value.startswith('vulkan') for value in execution)


def row_tint(execution):
    if execution == BASELINE:
        return r'\rowcolor{BenchBaseline}[0.5pt][0.5pt]'
    if execution == BIOSEQ_COMPARISON:
        return r'\rowcolor{BenchPreferred}[0.5pt][0.5pt]'
    return ''


def measurement_cell(parts):
    """Keep timing backgrounds inside the surrounding row and column rules."""
    return (r'\hspace*{1pt}\begin{tabular}[t]{@{}c@{}}\rule{0pt}{12pt}'
            + r'\\[-.2pt]'.join(parts) + r'\end{tabular}')


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


def validate_row(row, unavailable=False, fast_scope=None):
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
    assert type(row['run_count']) is int and row['run_count'] == (1 if fast_scope else 5)
    assert row['timing_scope'] in ((fast_scope,) if fast_scope else (CURRENT, HISTORICAL))
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
    """First complete observation per tuple; keep FAST scope and sample count intact."""
    def __init__(self, root):
        self.raw, self.docs, self.rows, self.unavailable = {}, {}, {}, set()
        self.sources, self.absent_sources = {}, set()
        self.source_counts, self.expected_tuples = {}, set()
        self.seen = set()
        self.excluded_historical = 0
        for host, default_name in FILES.items():
            candidates = [(default_name, host)]
            if host != 'Local':
                candidates += [(f'ALL_RESULTS_TUPLES_REMOTE_{tag}_{host}.yaml', f'{tag}_{host}')
                               for tag in REMOTE_FALLBACKS]
                candidates.append((f'ALL_RESULTS_TUPLES_FAST_REMOTE_{host}.yaml', None))
            else:
                candidates.append(('ALL_RESULTS_TUPLES_FAST_LOCAL.yaml', None))
            for name, machine_id in candidates:
                fast_scope = (FAST_LOCAL if host == 'Local' else FAST_REMOTE) if machine_id is None else None
                path = root / name
                if name != default_name and not path.exists():
                    self.absent_sources.add(name)
                    continue
                raw = path.read_bytes()
                doc = yaml.safe_load(raw)
                self.raw[name] = raw
                if name == default_name:
                    self.docs[host] = doc
                if fast_scope:
                    profile = doc['benchmark_host_profile']
                    if host == 'Local':
                        assert profile['machine'] == self.docs[host]['benchmark_host_profile']['machine'], f'wrong local host in {name}'
                    else:
                        assert profile['family'].upper() == host, f'wrong FAST family in {name}'
                        assert profile['replicas'], f'missing FAST host inventory in {name}'
                        assert all(binding['instance_type'] == self.docs[host]['remote_host_binding']['instance_type']
                                   and binding['machine_id'].split('_')[-1] == host
                                   for binding in profile['replicas']), f'wrong FAST host in {name}'
                elif host != 'Local':
                    binding = doc['remote_host_binding']
                    assert binding['machine_id'] == machine_id, f'wrong host in {name}'
                    assert binding['instance_type'] == self.docs[host]['remote_host_binding']['instance_type'], f'wrong instance class in {name}'
                identities, display_keys, unavailable_keys = set(), set(), set()
                for row in doc.get('approved_results') or []:
                    identity = (row['stable_key'], row['timing_scope'])
                    assert identity not in identities, f'duplicate observation {name} {identity}'
                    identities.add(identity)
                    # An empty placeholder is not a measurement and cannot block fallback.
                    metrics = row.get('metrics') or {}
                    if all(metrics.get(metric) is None for metric in ('p50_a', 'p50_b')):
                        continue
                    validate_row(row, fast_scope=fast_scope)
                    if row['timing_scope'] == HISTORICAL:
                        self.excluded_historical += 1
                        continue
                    self.expected_tuples.add((host, row['stable_key']))
                    key = (host, DISPLAY_SCOPE, row_key(row), config_key(row))
                    assert key not in display_keys, f'display collision in {name}: {key}'
                    display_keys.add(key)
                    if key not in self.rows:
                        self.rows[key] = row
                        self.sources[key] = name
                for row in doc.get('hardware_unavailable_cells') or []:
                    validate_row(row, True)
                    key = (host, DISPLAY_SCOPE, row_key(row), config_key(row))
                    assert key not in display_keys and key not in unavailable_keys, f'conflicting disposition in {name}: {key}'
                    unavailable_keys.add(key)
                self.unavailable.update(unavailable_keys)
                self.source_counts[name] = len(display_keys)
        # A real result from any priority source supersedes an unmeasured disposition.
        self.unavailable.difference_update(self.rows)
        assert {(key[0], row['stable_key']) for key, row in self.rows.items()} == self.expected_tuples, 'deduplicated source coverage mismatch'
        for key, row in self.rows.items():
            if row['pipeline'] == 'bioseq':
                assert (key[0], key[1], BASELINE, key[3]) in self.rows, f'missing matched baseline {key}'

    def minimum(self, host, scope, config, metric, bioseq_only=False):
        candidates = [r['metrics'][metric] for (h, s, rk, cfg), r in self.rows.items()
                      if h == host and s == scope and cfg == config
                      and (not bioseq_only or r['pipeline'] == 'bioseq')
                      and r['comparison_eligibility']['effective_path']]
        return min(candidates) if candidates else None

    def winner(self, host, scope, config, metric):
        """Select one column winner; exact ties use the first displayed execution."""
        candidates = [(rk, r) for (h, s, rk, cfg), r in self.rows.items()
                      if (h, s, cfg) == (host, scope, config)]
        return min(candidates, key=lambda item: (item[1]['metrics'][metric], item[0] != BASELINE, item[0]))[0] if candidates else None

    def cell(self, host, scope, execution, config, show_best=True):
        if execution == VULKAN_SUMMARY:
            unavailable = any(h == host and s == scope and cfg == config and is_vulkan(rk)
                              for h, s, rk, cfg in self.unavailable)
            return r'\textcolor{gray}{---\textsuperscript{g}}' if unavailable else r'\textcolor{gray}{\ensuremath{\cdot}}'
        key = (host, scope, execution, config)
        row = self.rows.get(key)
        if row is None:
            return r'\textcolor{gray}{---\textsuperscript{g}}' if key in self.unavailable else r'\textcolor{gray}{\ensuremath{\cdot}}'
        self.seen.add(key)
        marker = r'\textsuperscript{f}' if row['execution_conformance'] == 'documented_scalar_minimizer_fallback' else (r'\textsuperscript{p}' if row['execution_conformance'] == 'exact_requested_path_user_preserved' else '')
        if row['run_count'] == 1:
            marker += r'\textsuperscript{s}'
        parts = []
        for metric in ('p50_a', 'p50_b'):
            value = row['metrics'][metric]
            if row['pipeline'] == 'bioseq':
                ref = self.rows[(host, scope, BASELINE, config)]['metrics'][metric]
            else:
                ref = self.minimum(host, scope, config, metric, bioseq_only=True)
            timing = tex(duration(value)).replace(' ', r'\,')
            if value >= 3_600_000_000_000:
                timing = r'{\fontsize{6.2}{7}\selectfont ' + timing + '}'
            if show_best and execution == self.winner(host, scope, config, metric):
                timing = r'\colorbox{BenchBest}{\strut ' + timing + '}'
            parts += [timing, r'{\fontsize{5.8}{6.4}\selectfont ' + comparison(value, ref) + marker + '}']
        return measurement_cell(parts)


def execution_label(key):
    if key == BASELINE:
        return 'minimap2', r'\textbf{Baseline}'
    if key == VULKAN_SUMMARY:
        return 'BioseqDB', r'Vulkan (all modes)'
    _, index, minimizer, chain, base = key
    label = ('MPF' if index == 'minimizers_per_fragment' else 'MPS') + ' / ' + MINIMIZERS[minimizer]
    return 'BioseqDB', r'\begin{tabular}[t]{@{}l@{}}\rule{0pt}{12pt}' + tex(label) + r'\\' + tex(CHAINS[chain] + ' / ' + BASES[base]) + r'\end{tabular}'


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
    out += [r'\par Remote cells use the first available approved result in this order: the default summary, TEST3, TEST4, TEST7, TEST2, then FAST. Local cells use the default summary, then FAST LOCAL. A and B come from the same observation. The inventory describes the default hosts. Fallback measurements may come from other instances of the same machine class, and comparisons may combine campaigns.\par']
    out += [r'\begin{table}[H]\centering\small', r'\caption{Published snapshot coverage after excluding mismeasured historical timings.}', r'\begin{tabular}{lrrrrr}\toprule Host & Logical tuples & Observations & Repeats & BioseqDB obs. & GPU unavailable\\\midrule']
    for host in HOSTS:
        rows = [r for (h, _, _, _), r in snapshot.rows.items() if h == host]
        out.append(f"{host} & {len({r['stable_key'] for r in rows})} & {len(rows)} & {sum(r['run_count'] for r in rows)} & {sum(r['pipeline']=='bioseq' for r in rows)} & {sum(h == host for h, _, _, _ in snapshot.unavailable)}" + r'\\')
    out += [r'\bottomrule\end{tabular}\end{table}']
    total = len(snapshot.rows)
    runs = sum(r['run_count'] for r in snapshot.rows.values())
    out += [f'The published BioseqDB campaign snapshot contains {total} timing observations and {runs:,} accepted repeats. FAST entries retain their separate single-sample timing scope and are marked with a superscript s. The {snapshot.excluded_historical} historical outer-E2E observations are treated as mismeasurements and excluded from coverage and comparisons. Standard observations have five accepted repeats. FAST observations have one. The planned local matrix has 1,296 logical cells and 6,480 repeats. It remains incomplete. Unavailable-GPU counts refer only to the remote authorities\' recorded eligible subsets.']
    return out


def header(configs, remote, first_labels='Engine & Execution'):
    """Render a contiguous hierarchy with machine separators in the lowest tier."""
    expanded = [(cfg, h) for cfg in configs for h in (HOSTS[1:] if remote else ('Local',))]
    levels = [lambda c, h: 'Index' if c[0] == 'just_index' else 'No index', lambda c, h: f'{c[1]} Gbp', lambda c, h: f'k={c[2][0]}, w={c[2][1]}', lambda c, h: c[3].upper()]
    if remote:
        levels.append(lambda c, h: h)
    out = []
    for level, getter in enumerate(levels):
        labels = [(tuple(get(c, h) for get in levels[:level + 1]), getter(c, h)) for c, h in expanded]
        groups = [(key[1], len(list(group))) for key, group in itertools.groupby(labels)]
        left = first_labels if level == 0 else ' & '
        out.append(left + ' & ' + ' & '.join(rf'\multicolumn{{{count}}}{{c|}}{{\textbf{{{tex(label)}}}}}' for label, count in groups) + r'\\')
    return out


def result_pages(snapshot, scope, remote, configs, title):
    hosts = HOSTS[1:] if remote else ('Local',)
    relevant = {key[2] for key in set(snapshot.rows) | snapshot.unavailable if key[0] in hosts and key[1] == scope and key[3] in configs}
    # Keep row order identical across panels, and repeat baseline as a reading aid.
    keys = sorted(relevant - {BASELINE})
    vulkan_keys = {key for key in keys if is_vulkan(key)}
    has_vulkan_results = any(h in hosts and s == scope and cfg in configs and rk in vulkan_keys
                             for h, s, rk, cfg in snapshot.rows)
    if vulkan_keys and not has_vulkan_results:
        keys = [key for key in keys if key not in vulkan_keys] + [VULKAN_SUMMARY]
    page_count = max(1, (len(keys) + 6) // 7)
    batches = [keys[i * len(keys) // page_count:(i + 1) * len(keys) // page_count]
               for i in range(page_count)]
    out = []
    for page, batch in enumerate(batches, 1):
        out += [r'\clearpage', r'\noindent{\large\bfseries ' + tex(title) + rf'\hfill {page}/{len(batches)}' + r'}\par\smallskip', r'{\fontsize{7.1}{8.1}\selectfont\setlength{\tabcolsep}{1pt}\setlength{\fboxsep}{2pt}\renewcommand{\arraystretch}{1.12}', r'\noindent\begin{tabular}{@{}p{14mm}p{27mm}|' + ('p{13.55mm}|' * 16) + r'@{}}\hline']
        out += header(configs, remote)
        out.append(r'\hline')
        for n, key in enumerate([BASELINE] + batch):
            engine, label = execution_label(key)
            engine_label = r'\textbf{' + engine + '}' if n <= 1 or key == VULKAN_SUMMARY else ''
            if key == VULKAN_SUMMARY:
                engine_label = r'\rule{0pt}{12pt}' + engine_label
            cells = [snapshot.cell(h, scope, key, cfg, show_best=(key != BASELINE or page == 1)) for cfg in configs for h in hosts]
            assert len(cells) == 16
            out.append(row_tint(key) + engine_label + ' & ' + label + ' & ' + ' & '.join(cells) + r'\\[3pt]\hline')
        out += [r'\end{tabular}\par}', r'\smallskip{\footnotesize\noindent Each cell: A, comparison, B, comparison. Times cover 1,024 queries. $\cdot$ = no approved observation. ---\textsuperscript{g} = GPU unavailable, not executed. n/a = no measured comparison.\par', r'\noindent \textsuperscript{f}SIMD-ref request observed as scalar, compared only as that effective path. \textsuperscript{p}Accepted by explicit preservation after wrapper postflight failure, no run receipt claimed.\par', r'\noindent MPF/MPS = minimizers per fragment/sequence. Row order: index representation / minimizer, then chaining / base alignment. SIMD-ref is the reference minimizer, Bounded is bounded ntHash.\par']
        if any(r['run_count'] == 1 and h in hosts and cfg in configs and rk in [BASELINE] + batch
               for (h, _, rk, cfg), r in snapshot.rows.items()):
            out.append(r'\noindent\textsuperscript{s}' + tex(FAST_NOTE) + r'\par')
        out += [r'\noindent Blue = one lowest A and one lowest B down each machine/workload column, across continuation pages. Exact ties select the first row. Repeated baselines are marked only on page 1.\par',
                r'\noindent Light gray = minimap2 baseline. Pale yellow = preferred MPF / Bounded / SIMD-DP / Block configuration, not a claim of winning every column.\par',
                r'\noindent When all Vulkan modes are unmeasured, they share one row. Baseline comparisons use the fastest measured BioseqDB effective path separately for A and B.\par}']
    return out


def validate_external(row, host):
    """Validate accepted external identities and independently recompute their P50s."""
    assert row['stable_key_schema'] == 'bioseq-e2e-cell-v2'
    assert row['stable_key'].startswith('bioseq-e2e-cell-v2:')
    assert row['campaign_id'] == 'bioseq-e2e-external-algorithm-matrix-v1'
    assert row['timing_scope'] == EXTERNAL_SCOPE
    assert row['pipeline'] in EXTERNAL_ALIGNERS
    expected = [row['campaign_id'], row['dataset'], row['alphabet'], row['target_size_gbp'],
                row['kmer_size'], row['window_size'], row['mode'], row['pipeline'], None, None, None, None]
    assert json.loads(row['stable_key'].split(':', 1)[1]) == expected
    assert config_key(row) in CONFIGS
    assert row['dataset'] == f"matrix_{row['target_size_gbp']}gbp_{row['alphabet']}_k{row['kmer_size']}_w{row['window_size']}"
    assert row['index_type'] == row['pipeline'] + '_native'
    assert all(row[key] == dict.fromkeys(STAGES) for key in ('requested_algorithms', 'observed_algorithms'))
    assert row['measurement_host_binding']['machine_id'] == host
    assert row['execution_conformance'] == 'native_external_aligner'
    assert row['status'] == 'accepted' and row['run_count'] == 5 and row['query_count'] == 1024
    assert row['exact_baseline_match_count_equal'] is True
    assert row['comparison_eligibility']['effective_path'] is True
    assert row['metrics']['unit'] == 'ns'
    assert row['capabilities']['kmer'] and row['capabilities']['gapped_output']
    evidence = row['evidence']
    fixture = evidence['fixture']
    assert fixture['alphabet'] == row['alphabet']
    assert fixture['length'] == row['target_size_gbp'] * 1_000_000_000
    repeats = evidence['repeats']
    assert len(repeats) == 5
    for rep in repeats:
        a, b, union = (rep[key] for key in ('a_ns', 'b_ns', 'cigar_or_mapq_union_ns'))
        assert all(type(v) is int for v in (a, b, union)) and 0 < b <= a
        assert a - b == union
        assert rep['match_counts'] == [1] * 1024
    for metric, repeat_key in (('p50_a', 'a_ns'), ('p50_b', 'b_ns')):
        assert type(row['metrics'][metric]) is int
        assert row['metrics'][metric] == sorted(r[repeat_key] for r in repeats)[2], 'external median mismatch'


EXTERNAL_NOTES = {
    'a': 'BLAST DNA5 uses an N score of -3, compared with -1 in the baseline.',
    'b': 'LexicMap stores reference N as A. Queries retain their bases, and edit-script checks use that native reference encoding.',
    'c': 'External input uses two reference records for the 4 Gbp sequence to stay within signed 32-bit coordinates.',
    'd': 'Winnowmap uses overlapping 250 Mb records and 499M index batches after an out-of-memory failure. Results are merged globally. This can affect MAPQ.',
    'e': 'Winnowmap uses 1999M index batches and merges the results from multiple index parts.',
    'f': 'Swap configuration was not recorded before this external measurement. Later host snapshots do not establish its earlier swap state.',
    's': FAST_NOTE,
    'g': 'Retained C7G minimap2 measurement comes from the earlier campaign with documented paging pressure.',
}


def external_markers(row):
    """Use each row's evidence, not generic comments describing other profiles."""
    marks = set()
    evidence = row['evidence']
    if row['pipeline'] == 'blastn' and row.get('ambiguity_scoring_qualification'):
        marks.add('a')
    if row['pipeline'] == 'lexicmap' and row['alphabet'] == 'dna5':
        assert 'reference N is stored as A' in row['alignment_scoring_qualification']['ambiguity_handling']
        marks.add('b')
    if evidence['reference_layout']['input_records'] == 2:
        marks.add('c')
    preparations = [evidence.get('setup_provenance')] + [r.get('index_preparation_metadata') for r in evidence['repeats']]
    for prep in preparations:
        if not prep:
            continue
        if prep.get('partitioned'):
            assert prep['index_batch'] == '499M' and prep['maximum_record_bases'] == 250_000_000
            marks.add('d')
        elif prep.get('multipart'):
            assert prep['index_batch'] == '1999M'
            marks.add('e')
    if evidence.get('memory_environment', {}).get('swap') == 'not_recorded_before_zram_audit':
        marks.add('f')
    return marks


class ExternalComparison:
    """Explicitly join different campaigns by instance class and visible workload.

    This does not relabel timing scopes or claim identical physical hosts/fixtures.
    Only the exact requested and observed Bioseq path may populate its row.
    """
    def __init__(self, snapshot, root):
        self.raw, self.docs, self.rows, self.seen = {}, {}, {}, set()
        self.sources = {}
        for host in HOSTS[1:]:
            # Both reference rows come exclusively from the non-external remote authority.
            for aligner, execution in (('minimap2', BASELINE), ('bioseq', BIOSEQ_COMPARISON)):
                for cfg in CONFIGS:
                    row = snapshot.rows.get((host, DISPLAY_SCOPE, execution, cfg))
                    if row is not None:
                        if aligner == 'bioseq':
                            assert row_key(row) == BIOSEQ_COMPARISON
                            assert row['execution_conformance'] == 'exact_requested_path'
                            assert row['requested_algorithms'] == row['observed_algorithms']
                            assert row['comparison_eligibility']['requested_path'] is True
                        self.rows[(host, aligner, cfg)] = row
                        self.sources[(host, aligner, cfg)] = snapshot.sources[(host, DISPLAY_SCOPE, execution, cfg)]
            raw = (root / EXTERNAL_FILES[host]).read_bytes()
            doc = yaml.safe_load(raw)
            self.raw[host], self.docs[host] = raw, doc
            assert doc['authority']['normative_datasets'] == ['approved_results']
            assert doc['authority']['profile'] == 'aws_external_aligners_v1'
            host_binding = doc['remote_host_binding']
            assert host_binding['machine_id'] == host
            assert host_binding['instance_type'] == snapshot.docs[host]['remote_host_binding']['instance_type']
            for row in doc['approved_results']:
                if row['pipeline'] not in EXTERNAL_ALIGNERS:
                    continue  # External minimap2 cannot replace the baseline. MMseqs2 is also omitted.
                validate_external(row, host)
                assert row['measurement_host_binding'] == host_binding, 'external row host differs from its file'
                key = (host, row['pipeline'], config_key(row))
                assert key not in self.rows, f'duplicate external display cell: {key}'
                assert (host, 'minimap2', key[2]) in self.rows, f'missing matching minimap2 workload: {key}'
                self.rows[key] = row

    def cell(self, host, aligner, cfg, page_notes):
        key = (host, aligner, cfg)
        row = self.rows.get(key)
        if row is None:
            return r'\textcolor{gray}{\ensuremath{\cdot}}'
        self.seen.add(key)
        marks = external_markers(row) if aligner in EXTERNAL_ALIGNERS else ({'g'} if host == 'C7G' and self.sources[key] == FILES[host] else set())
        if row['run_count'] == 1:
            marks.add('s')
        page_notes.update(marks)
        marker = r'\textsuperscript{' + ','.join(sorted(marks)) + '}' if marks else ''
        counterpart = self.rows.get((host, 'bioseq' if aligner == 'minimap2' else 'minimap2', cfg))
        parts = []
        for metric in ('p50_a', 'p50_b'):
            value = row['metrics'][metric]
            reference = counterpart['metrics'][metric] if counterpart else None
            winner = min((name for name in ALIGNER_ORDER if (host, name, cfg) in self.rows),
                         key=lambda name: (self.rows[(host, name, cfg)]['metrics'][metric], ALIGNER_ORDER.index(name)))
            timing = tex(duration(value)).replace(' ', r'\,')
            if value >= 3_600_000_000_000:
                timing = r'{\fontsize{6.2}{7}\selectfont ' + timing + '}'
            if aligner == winner:
                timing = r'\colorbox{BenchBest}{\strut ' + timing + '}'
            parts += [timing, r'{\fontsize{5.8}{6.4}\selectfont ' + comparison(value, reference) + (marker if metric == 'p50_a' else '') + '}']
        return measurement_cell(parts)


def external_description(external):
    """Plain-language methods, explicitly distinguishing these campaign boundaries."""
    paragraphs = [r'\clearpage\subsection{Comparison with other aligners}\label{sec:external-aligner-results}',
            r'The final table places minimap2 and one fixed BioseqDB configuration alongside blastn, Winnowmap and LexicMap. Both minimap2 and BioseqDB timings come from the corresponding machine\textquotesingle s non-external remote summary, matched by machine class, target size, alphabet, $(k,w)$ and index mode. External-campaign minimap2 timings are excluded. BioseqDB always uses minimizers per fragment (MPF), bounded ntHash SIMD minimizers, SIMD-DP chaining and Block Aligner. Missing observations are marked with a dot. The same default / TEST3 / TEST4 / TEST7 / TEST2 / FAST priority applies to both rows. FAST entries carry a single-sample footnote. No other BioseqDB configuration or historical timing scope is substituted.',
            f'The table contains {len(external.rows)} distinct observations and {sum(r["run_count"] for r in external.rows.values()):,} accepted repeats. Only approved results are included. Qualification runs, interrupted attempts and the small-query checks used before the full runs are excluded.',
            r'\paragraph{Measurement procedure.}',
            r'\begin{itemize}',
            r'\item Each external observation has five fresh-process repeats of 1,024 queries. A is the median wall time, including executable startup, index loading, mapping and writing output. Fixture generation and validation are outside the timer. The index mode prepares the native on-disk index beforehand. No-index mode creates it inside the measured interval, including tool-specific preparation such as counting frequent k-mers or partitioning the reference. Operating-system caches are not purged.',
            r'\item The external campaign checks one primary occurrence per query, its position and strand, and an edit script reconstructed from the gapped output. These files include the five raw timing samples and returned-count vectors. This is additional evidence beyond the median-only summaries used for the earlier tables. MAPQ is checked only where the tool provides it. No MAPQ value is invented for blastn or LexicMap.',
            r'\item B is computed separately in each repeat by subtracting the union of the tool\textquotesingle s instrumented alignment and available MAPQ intervals, then taking the median. BLAST subtracts traceback-search regions. LexicMap subtracts wavefront-alignment calls. Winnowmap subtracts its native alignment and MAPQ intervals. These are different boundaries, so B is a tool-specific remaining time, not the duration of a common seeding-and-chaining stage.',
            r'\end{itemize}',
            r'\paragraph{How the tools were configured.}',
            r'\begin{itemize}',
            r'\item \textbf{blastn 2.17.0+.} The word size equals the requested $k$. There is no minimizer-window setting, so $w$ labels the fixture only. Low-complexity masking is disabled, and each reported alignment must cover at least 80\% of the query to remove short chance matches. Match reward 2, mismatch penalty 4, gap-open penalty 4 and gap-extension penalty 2 reproduce the baseline\textquotesingle s first affine-gap branch. BLAST has no second branch. The default E-value threshold of 10 is retained. DNA5 cells are marked because BLAST scores N differently.',
            r'\item \textbf{Winnowmap.} Both $k$ and $w$ are retained. Frequent k-mers are counted with Meryl to build weighted minimizers. This preparation is included in no-index time. Baseline scoring and first-stage bandwidth are retained, while Winnowmap\textquotesingle s additional structural-variant-aware stage keeps its native settings. Secondary alignments are suppressed. The benchmark build restores persisted-index output and reconstructs the weighting filter on loading. It also initializes a repeat-length variable to prevent undefined MAPQ/output behaviour. Memory-limited and multipart runs carry cell-specific notes.',
            r'\item \textbf{LexicMap.} The requested $k$ is retained, but LexicHash masks replace minimizer windows, so $w$ is inapplicable. The native 20,000 masks, gap filling between sparsely seeded regions and low-complexity rejection remain enabled. Query coverage is at least 80\%. Its native wavefront-alignment costs are match 0, mismatch 4 and gap open/extension 6/2. Its separate reported-score calculation uses match 2, mismatch -3 and gaps 5/2. These settings are not rewritten to mimic minimap2. References are divided into records of at most 268,435,455 bases with overlap equal to the longest query, and output coordinates are restored to the logical reference. DNA5 cells are marked because its native reference encoding replaces N with A.',
            r'\end{itemize}',
            r'\paragraph{What the comparison establishes.}',
            r'The external runs used new instances of the same EC2 types, not the physical instances used for the retained minimap2 and BioseqDB results. External volumes are 300 GiB unencrypted gp3, compared with 500 GiB encrypted gp3 in the earlier campaign. Both provision 6,000 IOPS and 500 MiB/s. The external host bindings record four benchmark threads, nice $-20$ and Clang 18.1.3, whereas the earlier campaign used Clang 22.1.8. Compressed-RAM swap is recorded for many external runs. Its presence does not prove how much swapping occurred during each measurement.',
            r'The workload dimensions agree, but the timing boundaries, reference layouts, scoring and resource conditions are not identical. In particular, the retained framework timings exclude process-startup setup, and BioseqDB no-index mode constructs a transient index rather than the external tools\textquotesingle{} on-disk indexes. The summaries do not establish cross-campaign byte-for-byte fixture identity. Ratios therefore describe the published elapsed times under these qualified conditions, rather than an isolated causal effect of the aligner.',
            r'The table retains the green/red/gray triangles, 5\% tie band and $\times$ notation above 30\%. Every non-baseline row compares with minimap2 in the same machine-class/workload column. Minimap2 compares only with the displayed fixed BioseqDB configuration, not another faster BioseqDB row. Blue marks one lowest reported A and one lowest reported B down the five rows in each column. Exact ties select the first displayed row. Light gray identifies the minimap2 baseline. Pale yellow identifies the preferred MPF / Bounded / SIMD-DP / Block configuration, independently of the measured winners. For B, both the ratio and the highlight compare native residual times with different work removed, and do not establish superiority at an equivalent processing stage. Dots mean no approved timing, not failure or hardware unavailability.']
    return [line + (r'\par' if line and not line.startswith('\\') else '') for line in paragraphs]


def external_pages(external):
    out = external_description(external)
    out += [r'\newgeometry{left=10mm,right=10mm,top=12mm,bottom=12mm}', r'\begin{landscape}', r'\refstepcounter{table}\label{tab:external-aligners}']
    labels = {'minimap2': ('minimap2', 'Retained baseline'), 'bioseq': ('BioseqDB', r'\begin{tabular}[t]{@{}l@{}}\rule{0pt}{12pt}MPF / Bounded\\SIMD-DP / Block\end{tabular}'),
              'blastn': ('blastn', r'Native\textsuperscript{*}'), 'winnowmap': ('winnowmap', r'Native\textsuperscript{*}'), 'lexicmap': ('lexicmap', r'Native\textsuperscript{*}')}
    for panel, (size, kw) in enumerate(itertools.product((4, 2), ((15, 10), (20, 15))), 1):
        configs = [cfg for cfg in CONFIGS if cfg[1:3] == (size, kw)]
        notes = set()
        out += [r'\clearpage', rf'\noindent{{\large\bfseries Table \thetable: aligner comparison, {size} Gbp, k={kw[0]}, w={kw[1]}\hfill {panel}/4}}\par\smallskip',
                r'\noindent{\footnotesize Different campaigns and native timing boundaries. A/B ratios are descriptive, subject to the measurement notes.}\par\smallskip',
                r'{\fontsize{7.1}{8.1}\selectfont\setlength{\tabcolsep}{1pt}\setlength{\fboxsep}{2pt}\renewcommand{\arraystretch}{1.12}',
                r'\noindent\begin{tabular}{@{}p{15mm}p{26mm}|' + ('p{13.55mm}|' * 16) + r'@{}}\hline']
        out += header(configs, True, 'Aligner & Configuration') + [r'\hline']
        for aligner in ALIGNER_ORDER:
            name, label = labels[aligner]
            cells = [external.cell(h, aligner, cfg, notes) for cfg in configs for h in HOSTS[1:]]
            tint = row_tint(BASELINE if aligner == 'minimap2' else BIOSEQ_COMPARISON) if aligner in ('minimap2', 'bioseq') else ''
            out.append(tint + tex(name) + ' & ' + label + ' & ' + ' & '.join(cells) + r'\\[3pt]\hline')
        out += [r'\end{tabular}\par}', r'\smallskip{\footnotesize\noindent Each cell: A, comparison, B, comparison. Batches of 1,024 queries. Five repeats unless marked s (FAST: one sample). Blue = one lowest A and one lowest B down each machine/workload column.\par',
                r'\noindent Light gray = minimap2 baseline. Pale yellow = preferred MPF / Bounded / SIMD-DP / Block configuration, not a claim of winning every column. Exact ties select the first row.\par',
                r'\noindent Ratios use minimap2 as reference, except minimap2 itself uses the fixed BioseqDB row. $\cdot$ = no approved timing. n/a = no measured counterpart.\par',
                r'\noindent Both minimap2 and BioseqDB use the matching non-external remote measurements.\par',
                r'\noindent\textsuperscript{*}External A includes process startup and output. B removes different native stages and is not an equivalent-work comparison. BLAST and LexicMap have no $w$ or MAPQ equivalent.\par']
        for marker in sorted(notes):
            out.append(r'\noindent\textsuperscript{' + marker + '}' + tex(EXTERNAL_NOTES[marker]) + r'\par')
        out.append('}')
    out += [r'\end{landscape}', r'\restoregeometry']
    assert external.seen == set(external.rows), 'external comparison silently omitted approved observations'
    return out


def render(snapshot, external):
    out = ['% Generated by main/papers/scripts/generate_benchmark_results.py. Do not edit.', '% Source byte hashes (regenerate manually after approved results change).']
    for name, raw in snapshot.raw.items():
        out.append('% ' + name + ' sha256=' + hashlib.sha256(raw).hexdigest())
    for name in snapshot.raw:
        count = sum(source == name for source in snapshot.sources.values())
        out.append(f'% {name}: available={snapshot.source_counts[name]}, selected={count}, shadowed={snapshot.source_counts[name] - count}')
    for host, raw in external.raw.items():
        out.append('% ' + EXTERNAL_FILES[host] + ' sha256=' + hashlib.sha256(raw).hexdigest())
    out += [r'\definecolor{BenchBest}{HTML}{DDECF8}', r'\definecolor{BenchBaseline}{HTML}{F4F4F4}', r'\definecolor{BenchPreferred}{HTML}{FFFBEA}', r'\newgeometry{left=10mm,right=10mm,top=12mm,bottom=12mm}', r'\begin{landscape}']
    out += hardware_table(snapshot)
    out += result_pages(snapshot, DISPLAY_SCOPE, False, CONFIGS, 'Local machine: mapping interval')
    for size, kw in itertools.product((4, 2), ((15, 10), (20, 15))):
        configs = [cfg for cfg in CONFIGS if cfg[1:3] == (size, kw)]
        out += result_pages(snapshot, DISPLAY_SCOPE, True, configs, f'AWS: {size} Gbp, k={kw[0]}, w={kw[1]} (mapping interval)')
    out += [r'\end{landscape}', r'\restoregeometry', '']
    out += external_pages(external)
    assert {(key[0], snapshot.rows[key]['stable_key']) for key in snapshot.seen} == snapshot.expected_tuples, 'rendered tuples differ from deduplicated source union'
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
    external = ExternalComparison(snapshot, args.root)
    result = render(snapshot, external)
    for name, raw in snapshot.raw.items():
        if (args.root / name).read_bytes() != raw:
            raise RuntimeError(f'{name} changed during generation. Rerun for a consistent snapshot.')
    if any((args.root / name).exists() for name in snapshot.absent_sources):
        raise RuntimeError('A fallback source appeared during generation. Rerun for a consistent snapshot.')
    for host, name in EXTERNAL_FILES.items():
        if (args.root / name).read_bytes() != external.raw[host]:
            raise RuntimeError(f'{name} changed during generation. Rerun for a consistent snapshot.')
    if args.check:
        if not args.output.exists() or args.output.read_text() != result:
            print(f'Stale generated results: {args.output}', file=sys.stderr)
            return 1
    else:
        args.output.write_text(result)
    print(f"{'Verified' if args.check else 'Generated'} {args.output}: {len(snapshot.rows)} observations, {sum(r['run_count'] for r in snapshot.rows.values())} repeats; final aligner table {len(external.rows)} observations; every selected observation rendered.")
    for name in snapshot.raw:
        count = sum(source == name for source in snapshot.sources.values())
        print(f'  {name}: {snapshot.source_counts[name]} available, {count} selected, {snapshot.source_counts[name] - count} shadowed')
    for host in HOSTS:
        selected = [r for (h, _, _, _), r in snapshot.rows.items() if h == host]
        expected = sum(h == host for h, _ in snapshot.expected_tuples)
        print(f'  Coverage {host}: {len(selected)}/{expected} unique tuples rendered; {sum(r["run_count"] == 1 for r in selected)} FAST')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
