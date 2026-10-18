import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import * as loader from '../../static/js/project-benchmark-loader.mjs';

globalThis.document = new EventTarget();
document.querySelector = () => null;
globalThis.window = new EventTarget();
window.innerWidth = 1280;
window.innerHeight = 900;
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

// Updating a tooltip must not rebuild comparison rows and their event handlers.
// Exercise the actual binding path, while making a full table render fail.
globalThis.Node = { TEXT_NODE: 3, DOCUMENT_FRAGMENT_NODE: 11 };
globalThis.requestAnimationFrame = () => {};
const tooltipOnly = new ProjectComponent();
tooltipOnly.renderVals = () => { throw new Error('tooltip rebuilt the table'); };
let boundTooltip;
tooltipOnly.root = { querySelector: () => ({ replaceWith: (node) => { boundTooltip = node; } }) };
tooltipOnly.template = { content: { querySelector: () => ({ cloneNode: () => ({
  nodeType: Node.TEXT_NODE,
  nodeValue: '[[ tipTitle ]]|[[ tipSub ]]|[[ tipNote ]]|[[ tipOpacity ]]|[[ hasTip ]]|[[ tipX ]]|[[ tipY ]]',
}) }) } };
tooltipOnly.state.tip = {
  title: 'Measured alignment', sub: 'A · Complete time', note: 'Published source',
  x: 120, y: 240, rows: [{ k: 'baseline value', v: '1.25 ms' }],
  hist: [{ h: 100, color: '#74c8a8' }],
};
tooltipOnly.renderTip();
assert.equal(boundTooltip.nodeValue, 'Measured alignment|A · Complete time|Published source|1|true|120|240');
assert.deepEqual(tooltipOnly.renderTipVals().tipRows, tooltipOnly.state.tip.rows);
assert.deepEqual(tooltipOnly.renderTipVals().tipHist, tooltipOnly.state.tip.hist);
assert.equal(tooltipOnly.renderTipVals().tipHasHist, true);
assert.equal(tooltipOnly.renderTipVals().tipHasNote, true);
tooltipOnly.state.tip = null;
tooltipOnly.renderTip();
assert.equal(boundTooltip.nodeValue, '|||0|false|-9999|-9999');
assert.deepEqual(tooltipOnly.renderTipVals().tipRows, []);
assert.deepEqual(tooltipOnly.renderTipVals().tipHist, []);

// Escape must dismiss hover-only tooltips on the page and embedded chapter
// tables, even when no tooltip trigger owns keyboard focus.
const tooltipComponents = [true, true, false].map((isConnected) => {
  const component = new ProjectComponent();
  component.render = () => {};
  component.tipRenders = 0;
  component.renderTip = () => { component.tipRenders++; };
  component.mount({ isConnected }, {}, {});
  component.mount(component.root, component.template, {});
  component.state.tip = { title: 'Hovered benchmark cell' };
  return component;
});
const key = (value) => {
  const event = new Event('keydown');
  Object.defineProperty(event, 'key', { value });
  document.dispatchEvent(event);
};
key('ArrowDown');
assert.ok(tooltipComponents.every((component) => component.state.tip));
key('Escape');
assert.ok(tooltipComponents.slice(0, 2).every((component) => component.state.tip === null));
assert.ok(tooltipComponents[2].state.tip, 'detached components must ignore document events');
key('Escape');
assert.deepEqual(tooltipComponents.map((component) => component.tipRenders), [1, 1, 0]);
// A focus-induced scroll can deliver the previous cell's mouseleave after
// the new focus event. It must not dismiss or replace the focused cell's data.
const owner = tooltipComponents[0];
const target = () => ({ isConnected: true, getBoundingClientRect: () => ({ left: 100, top: 200, bottom: 240, width: 140 }) });
const oldCell = target(), focusedCell = target(), pointerCell = target();
document.activeElement = null;
owner.showTip({ type: 'mouseenter', currentTarget: oldCell }, { title: 'old cell' });
document.activeElement = focusedCell;
owner.showTip({ type: 'focus', currentTarget: focusedCell }, { title: 'focused cell' });
owner.hideTip({ type: 'mouseleave', currentTarget: oldCell });
owner.showTip({ type: 'mouseenter', currentTarget: pointerCell }, { title: 'layout entered cell' });
owner.showTip({ type: 'mousemove', currentTarget: pointerCell, movementX: 0, movementY: 0 }, { title: 'stationary pointer' });
assert.equal(owner.state.tip.title, 'focused cell');
owner.hideTip({ type: 'mouseleave', currentTarget: focusedCell });
assert.equal(owner.state.tip.title, 'focused cell', 'keyboard focus survives its own pointer leave');

// Moving the pointer deliberately must still hand control back to hover,
// even if the old cell keeps keyboard focus.
owner.showTip({ type: 'mousemove', currentTarget: pointerCell, movementX: 2, movementY: 0 }, { title: 'pointer cell' });
assert.equal(owner.state.tip.title, 'pointer cell');
owner.hideTip({ type: 'blur', currentTarget: focusedCell });
assert.equal(owner.state.tip.title, 'pointer cell', 'stale blur must not dismiss the new owner');
owner.hideTip({ type: 'mouseleave', currentTarget: pointerCell });
assert.equal(owner.state.tip, null);
owner.showTip({ type: 'focus', currentTarget: focusedCell }, { title: 'focused again' });
key('Escape');
assert.equal(owner.state.tip, null);
assert.equal(owner.tipTarget, null, 'Escape releases keyboard ownership for the next hover');
owner.showTip({ type: 'mouseenter', currentTarget: oldCell }, { title: 'after Escape' });
assert.equal(owner.state.tip.title, 'after Escape');
owner.showTip({ type: 'focus', currentTarget: focusedCell }, { title: 'blurred focus' });
document.activeElement = null;
owner.hideTip({ type: 'blur', currentTarget: focusedCell });
assert.equal(owner.state.tip, null, 'leaving keyboard focus dismisses its tooltip');
const rightCell = { isConnected: true, getBoundingClientRect: () => ({ left: 1000, top: 200, bottom: 240, width: 140 }) };
document.activeElement = rightCell;
owner.showTip({ type: 'focus', currentTarget: rightCell }, { title: 'viewport resize' });
assert.equal(owner.state.tip.x, 894);
window.innerWidth = 420;
window.dispatchEvent(new Event('resize'));
assert.equal(owner.state.tip.x, 58, 'resizing repositions the existing focused tooltip');
assert.equal(owner.state.tip.title, 'viewport resize');
key('Escape');

console.log(`Verified ${model.order.length} panels, ${cells} cells and ${measured} measured A/B pairs with same-column baselines.`);
