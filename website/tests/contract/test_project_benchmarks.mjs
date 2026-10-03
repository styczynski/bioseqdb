import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import * as loader from '../../static/js/project-benchmark-loader.mjs';

globalThis.document = { querySelector: () => null };
globalThis.HTMLElement = class {};
globalThis.HTMLTemplateElement = class {};
const { ProjectComponent } = await import('../../static/js/benchmarks-design.mjs');
const json = JSON.parse(readFileSync(new URL('../../static/project-benchmarks.json', import.meta.url)));
const model = loader.buildModel([{ kind: 'project', status: 'ok', json, url: 'project-benchmarks.json', name: 'project-benchmarks.json' }]);
assert.equal(model.order.length, json.counts.panels);
let cells = 0;
let measured = 0;
for (const key of model.order) {
  const benchmark = model.benchmarks[key];
  const run = benchmark.runs[0];
  for (const metric of run.metricSet) {
    const field = metric === 'wall_clock_ns' ? 'a_ns' : 'b_ns';
    const table = loader.buildTable(run, { metric, stat: 'P50', region: 'all_regions', direction: -1, baselineAlgo: 'minimap2' });
    assert.equal(table.baseline, 'minimap2');
    assert.equal(table.groups.length, 1);
    assert.equal(table.groups[0].rows.length, benchmark.projectPanel.rows.length);
    for (const row of table.groups[0].rows) {
      for (const [i, cell] of row.values.entries()) {
        const source = row.cells[table.cols[i]];
        assert.equal(cell.value, source[field]);
        if (metric === 'wall_clock_ns') { cells++; if (cell.value !== null) measured++; }
        assert.equal(cell.sample.p.P50, source[field]);
        assert.equal(cell.sample.p.P99, undefined, 'published medians must not invent percentiles');
        if (cell.ratio !== null) assert.equal(cell.ratio, cell.base / cell.value);
        if (cell.isBest) assert.ok(table.groups[0].rows.every((other) => other.cells[table.cols[i]].eligible === false || other.values[i].value === null || cell.value <= other.values[i].value));
      }
    }
  }
  const component = new ProjectComponent();
  component.lib = loader;
  component.state = { ...component.state, loading: false, model, files: [], sel: key, metric: 'wall_clock_ns' };
  const view = component.renderVals();
  assert.equal(view.groups[0].rows.length, benchmark.projectPanel.rows.length);
  assert.deepEqual(view.statOpts.map((s) => s.value), ['P50']);
  assert.ok(view.cols.every((c) => c.name && c.sub));
}
assert.equal(cells, json.counts.cells);
assert.equal(measured, json.counts.measured);
const run = model.benchmarks['aws-2-k15-w10'].runs[0];
const custom = loader.buildTable(run, { metric: 'wall_clock_ns', direction: -1, baselineAlgo: 'mpf-bounded-simd-block' });
assert.equal(custom.baseline, 'mpf-bounded-simd-block');
assert.ok(custom.groups[0].rows.find((r) => r.algo === custom.baseline).values.every((cell) => cell.ratio === null));
console.log(`Verified ${model.order.length} panels, ${cells} cells and ${measured} measured A/B pairs with same-column baselines.`);
