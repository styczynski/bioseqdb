#!/usr/bin/env python3
"""Publish exactly the approved observations selected for chapter 16.

Uses the paper's validator, source priority and table registry; never reruns a
benchmark or invents samples from a published median. Missing observations,
GPU unavailability and failed runs remain explicit dispositions.
"""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / 'website/static/project-benchmarks.json'


def generate(root: Path) -> dict:
    spec = importlib.util.spec_from_file_location('paper_benchmarks', root / 'papers/scripts/generate_benchmark_results.py')
    paper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(paper)
    # The C loader has identical safe constructors and avoids spending minutes
    # parsing the large evidence inventories on every website rebuild.
    if hasattr(paper.yaml, 'CSafeLoader'):
        paper.yaml.safe_load = lambda source: paper.yaml.load(source, Loader=paper.yaml.CSafeLoader)
    results = root / 'research_benchmarks/paper/e2e'
    snapshot = paper.Snapshot(results)
    external = paper.ExternalComparison(snapshot, results)
    gpu = paper.GPUComparison(results)
    registry = paper.SliceRegistry()
    paper.render(snapshot, external, gpu, registry)
    records = {}
    for (host, scope, execution, config), row in snapshot.rows.items():
        records[('local' if host == 'Local' else f'aws-{config[1]}-k{config[2][0]}-w{config[2][1]}', paper.execution_id(execution), paper.column_id(config, host))] = (row, snapshot.sources[(host, scope, execution, config)])
    for (host, aligner, config), row in external.rows.items():
        records[(f'external-{config[1]}-k{config[2][0]}-w{config[2][1]}', aligner, paper.column_id(config, host))] = (row, external.sources.get((host, aligner, config), paper.EXTERNAL_FILES[host]))
    for (execution, config), row in gpu.rows.items():
        records[('vastai', paper.execution_id(execution), paper.column_id(config, 'Vastai'))] = (row, paper.GPU_FILE)
    dispositions = {(paper.execution_id(execution), paper.column_id(config, 'Vastai')): gpu.reason(row) for (execution, config), row in gpu.dispositions.items()}
    output = {'schema': 'bioseq.project-benchmarks.v1', 'tables': {}, 'sources': {}}
    for name, raw in {**snapshot.raw, **{paper.EXTERNAL_FILES[h]: raw for h, raw in external.raw.items()}, paper.GPU_FILE: gpu.raw}.items():
        output['sources'][name] = hashlib.sha256(raw).hexdigest()
    for table, (rows, columns) in registry.tables.items():
        panel = {'title': {'local': 'Local machine', 'vastai': 'Vast.ai GPU, Vulkan vs minimap2'}.get(table, table.replace('aws-', 'AWS: ').replace('external-', 'Aligner comparison: ')), 'columns': [], 'rows': []}
        for identifier, _ in columns:
            match = re.fullmatch(r'(index|noindex)-(\d)-k(\d+)-w(\d+)-(dna[45])-(.+)', identifier)
            mode, size, k, w, alphabet, host = match.groups()
            panel['columns'].append({'id': identifier, 'label': f'{host.upper() if host != "local" else "Local"} / {"Index" if mode == "index" else "No index"}', 'detail': f'{size} Gbp / {alphabet.upper()} / k={k}, w={w}'})
        for identifier, _, tint in rows:
            label = identifier
            if table.startswith('external-'):
                label = 'BioseqDB / MPF / Bounded / SIMD-DP / Block' if identifier == 'bioseq' else identifier
            elif identifier != 'minimap2' and identifier != 'vulkan-unavailable':
                index, minimizer, chain, base = identifier.split('-')
                label = ' / '.join((index.upper(), {'scalar': 'Scalar', 'simdref': 'SIMD-ref', 'twostack': 'Two-stack', 'bounded': 'Bounded', 'vulkan': 'Vulkan'}[minimizer], {'scalar': 'Scalar-DP', 'simd': 'SIMD-DP', 'rmq': 'RMQ', 'vulkan': 'Vulkan-DP'}[chain], {'block': 'Block', 'ksw2': 'KSW2'}[base]))
            elif identifier == 'vulkan-unavailable':
                label = 'Vulkan (GPU unavailable)'
            entry = {'id': identifier, 'label': label, 'preferred': tint == 'BenchPreferred', 'cells': {}}
            for column, _ in columns:
                record = records.get((table, identifier, column))
                if record:
                    row, source = record
                    cell = {'a_ns': row['metrics']['p50_a'], 'b_ns': row['metrics']['p50_b'], 'samples': row['run_count'], 'source': source, 'timing_scope': row['timing_scope'], 'status': row['status'], 'note': '', 'eligible': row.get('comparison_eligibility', {}).get('effective_path', True)}
                    if row.get('execution_conformance') == 'documented_scalar_minimizer_fallback':
                        cell['note'] = 'SIMD-ref ran as scalar. ' + str(cell['note'])
                    if table.startswith('external-') and row['pipeline'] in paper.EXTERNAL_ALIGNERS:
                        cell['note'] = ' '.join(paper.EXTERNAL_NOTES[mark] for mark in sorted(paper.external_markers(row)))
                else:
                    formatted = registry.cells[(table, identifier, column)]
                    reason = dispositions.get((identifier, column)) if table == 'vastai' else None
                    status = 'failed' if reason else 'hardware_unavailable' if 'textsuperscript{g}' in formatted else 'unmeasured'
                    cell = {'a_ns': None, 'b_ns': None, 'status': status, 'note': reason or ('GPU unavailable.' if status == 'hardware_unavailable' else 'No measurement.')}
                entry['cells'][column] = cell
            panel['rows'].append(entry)
        output['tables'][table] = panel
    output['counts'] = {'panels': len(registry.tables), 'cells': len(registry.cells), 'measured': sum(c['a_ns'] is not None for t in output['tables'].values() for r in t['rows'] for c in r['cells'].values())}
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--output', type=Path, default=OUTPUT)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    if not __debug__:
        parser.error('Do not run with -O: the source validator uses assertions.')
    payload = json.dumps(generate(args.root), ensure_ascii=False, separators=(',', ':')) + '\n'
    if args.check:
        if not args.output.exists() or args.output.read_text() != payload:
            sys.exit(f'Stale project benchmark data: {args.output}')
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(payload)
    print(f'{"Verified" if args.check else "Generated"} {args.output}')

if __name__ == '__main__':
    main()
