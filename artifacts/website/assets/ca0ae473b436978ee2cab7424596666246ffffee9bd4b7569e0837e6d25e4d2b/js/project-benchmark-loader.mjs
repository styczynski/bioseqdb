// The original design loader remains unchanged. This adapter exposes the paper's
// published medians through that same comparison model, with their provenance.
import * as original from './bench-loader.js';
export * from './bench-loader.js';
export const DEFAULT_SOURCES = [new URL('../project-benchmarks.json', import.meta.url).href];
export async function loadSources(sources) {
  const files = await original.loadSources(sources);
  return files.map((file) => {
    if (file.json?.schema !== 'bioseq.project-benchmarks.v1') return file;
    return { ...file, kind: 'project', status: 'ok' };
  });
}
export function buildModel(files) {
  const data = files.find((file) => file.kind === 'project' && file.status === 'ok');
  if (!data) return original.buildModel(files);
  const benchmarks = {};
  const order = Object.keys(data.json.tables);
  for (const key of order) {
    const panel = data.json.tables[key];
    benchmarks[key] = {
      key, title: panel.title, description: '', tags: [], hasResults: true,
      projectPanel: panel, algorithms: {}, algoOrder: panel.rows.map((r) => r.id),
      metricOrder: ['wall_clock_ns', 'without_cigar_mapq_ns'],
      metrics: { wall_clock_ns: { type: 'P50', description: 'A: the complete measured interval.' }, without_cigar_mapq_ns: { type: 'P50', description: 'B: the measured interval after subtracting the union of instrumented CIGAR and MAPQ in each repeat.' } },
      caseOrder: panel.columns.map((c) => c.id),
      testCases: Object.fromEntries(panel.columns.map((c) => [c.id, { description: c.label + ' / ' + c.detail, data: { size_label: c.detail } }])),
      iterations: {}, regions: ['all_regions'],
      runs: [{ id: 'Published benchmark results', url: data.url, meta: { project: true }, created: null, commit: null, rows: panel.rows.map((row) => ({ ...row, algo: row.id })), caseSet: panel.columns.map((c) => c.id), regionSet: ['all_regions'], metricSet: ['wall_clock_ns', 'without_cigar_mapq_ns'], projectPanel: panel }],
    };
  }
  data.benchmarkKeys = order;
  return { benchmarks, order, sources: files, errors: files.filter((f) => f.status === 'error') };
}
export function buildTable(run, options) {
  if (!run.projectPanel) return original.buildTable(run, options);
  const panel = run.projectPanel;
  const cols = (options.cases?.length ? options.cases : run.caseSet);
  const field = options.metric === 'without_cigar_mapq_ns' ? 'b_ns' : 'a_ns';
  const baseline = panel.rows.some((r) => r.id === options.baselineAlgo) ? options.baselineAlgo : 'minimap2';
  const peer = panel.rows.find((r) => r.id === baseline);
  const rows = panel.rows.map((row) => ({ ...row, algo: row.id, params: {}, detail: '', paramsLabel: '', isBaseline: row.id === baseline, values: cols.map((column) => {
    const cell = row.cells[column];
    return { value: cell?.[field] ?? null, sample: { published: true, source: cell, n: cell.samples, p: { P50: cell?.[field] ?? null } } };
  }) }));
  cols.forEach((column, i) => {
    const available = rows.filter((r) => r.values[i].value != null && r.cells[column].eligible !== false);
    const best = available.reduce((a, r) => !a || (options.direction > 0 ? r.values[i].value > a.values[i].value : r.values[i].value < a.values[i].value) ? r : a, null);
    rows.forEach((row) => {
      const cell = row.values[i];
      cell.base = peer?.cells[column]?.[field] ?? null;
      cell.isBest = options.direction !== 0 && best?.id === row.id && available.length > 1;
      cell.ratio = cell.value > 0 && cell.base > 0 && !row.isBaseline ? (options.direction > 0 ? cell.value / cell.base : cell.base / cell.value) : null;
      cell.deltaPct = cell.ratio == null ? null : (cell.value - cell.base) / cell.base * 100;
      cell.better = cell.ratio == null || options.direction === 0 ? 0 : cell.ratio > 1.0005 ? 1 : cell.ratio < .9995 ? -1 : 0;
    });
  });
  return { cols, algos: rows.map((r) => r.id), baseline, groupedBy: [], groups: [{ label: '', key: '', span: cols.length + 1, baselineLabel: peer?.label, rows }] };
}
