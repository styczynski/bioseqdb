/**
 * bench-loader.js — generic loader/normaliser for bench_framework artifacts.
 *
 * Point it at any number of URLs (configs, reports, or manifests). It sniffs
 * each document, merges configuration metadata with measured samples and
 * returns one model the UI can render without knowing anything about a
 * specific benchmark.
 *
 *   import { loadSources, buildModel, DEFAULT_SOURCES } from './bench-loader.js';
 *   const files = await loadSources(DEFAULT_SOURCES, (f) => console.log(f.url, f.status));
 *   const model = buildModel(files);
 *
 * Remote auto-discovery: a source URL may point at
 *   - a bench_configuration.json          (has `params` + `test_cases`)
 *   - a report_<N>.json                   (has `benchmarks` + `meta`)
 *   - a manifest: ["a.json","b.json"] or {"sources":[...]} or {"files":[...]}
 *     (relative entries resolve against the manifest URL, so a CI job can drop
 *      an index next to its reports and the site needs no edit)
 */

/** Single entry point: the CI-published manifest. Override per page via the
 *  `manifestUrl` prop to point at a different server or branch. */
export const DEFAULT_SOURCES = ['benchmarks-manifest.json'];

const RESERVED = new Set(['params', 'test_cases', 'regression_rules', 'meta', 'benchmarks']);

export function classify(json) {
  if (!json || typeof json !== 'object') return 'unknown';
  if (Array.isArray(json)) return 'manifest';
  if (json.benchmarks && typeof json.benchmarks === 'object') return 'report';
  if (json.params && json.test_cases) return 'config';
  if (Array.isArray(json.sources) || Array.isArray(json.files)) return 'manifest';
  return 'unknown';
}

function manifestEntries(json, baseUrl) {
  const list = Array.isArray(json) ? json : (json.sources || json.files || []);
  return list
    .map((e) => (typeof e === 'string' ? e : e && (e.url || e.path)))
    .filter(Boolean)
    .map((u) => new URL(u, baseUrl).href);
}

/** Fetch every source (expanding manifests one level). Never throws. */
export async function loadSources(sources, onProgress, _depth = 0) {
  const out = [];
  const jobs = sources.map(async (src) => {
    const raw = typeof src === 'string' ? src : src.url;
    let url = raw;
    try { url = new URL(raw, location.href).href; } catch (_) { /* keep raw */ }
    const rec = { url, name: raw.split('/').pop() || raw, status: 'loading', kind: 'unknown' };
    onProgress && onProgress(rec);
    try {
      if (src && src.json !== undefined) {
        rec.json = src.json;
        rec.name = src.name || rec.name;
      } else {
        const res = await fetch(url, { cache: 'no-cache' });
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        rec.json = await res.json();
        rec.bytes = Number(res.headers.get('content-length')) || undefined;
      }
      rec.kind = classify(rec.json);
      if (rec.kind === 'manifest' && _depth < 2) {
        const nested = await loadSources(manifestEntries(rec.json, url), onProgress, _depth + 1);
        rec.status = 'manifest';
        rec.expanded = nested.length;
        out.push(rec, ...nested);
        return;
      }
      rec.status = rec.kind === 'unknown' ? 'error' : 'ok';
      if (rec.kind === 'unknown') rec.error = 'unrecognised JSON shape';
    } catch (err) {
      rec.status = 'error';
      rec.error = String(err.message || err);
    }
    out.push(rec);
  });
  await Promise.all(jobs);
  return out;
}

/* ---------------------------------------------------------------- stats -- */

function pctFromArray(sorted, p) {
  if (!sorted.length) return null;
  return sorted[Math.floor(p * (sorted.length - 1))];
}

/** Accepts the rich {stats,percentiles,…} object OR a plain sample array. */
export function normaliseSamples(raw) {
  if (raw == null) return null;
  if (Array.isArray(raw)) {
    const vals = raw.filter((v) => typeof v === 'number' && isFinite(v));
    if (!vals.length) return null;
    const s = [...vals].sort((a, b) => a - b);
    const sum = s.reduce((a, b) => a + b, 0);
    const mean = sum / s.length;
    const variance = s.reduce((a, b) => a + (b - mean) ** 2, 0) / s.length;
    return {
      n: s.length, min: s[0], max: s[s.length - 1], mean, mean90: mean, sum,
      stddev: Math.sqrt(variance),
      p: { P01: pctFromArray(s, 0.01), P20: pctFromArray(s, 0.2), P50: pctFromArray(s, 0.5), P80: pctFromArray(s, 0.8), P99: pctFromArray(s, 0.99) },
      top: s.slice(-10).reverse(), bottom: s.slice(0, 10), hist: null, synthetic: true,
    };
  }
  if (typeof raw !== 'object') return null;
  const st = raw.stats || {};
  const pc = raw.percentiles || {};
  return {
    n: raw.samples_count ?? raw.count ?? null,
    min: st.min ?? null, max: st.max ?? null, mean: st.mean ?? null,
    mean90: st.mean90 ?? null, sum: st.sum ?? null, stddev: st.stddev ?? null,
    p: { P01: pc.P01 ?? null, P20: pc.P20 ?? null, P50: pc.P50 ?? null, P80: pc.P80 ?? null, P99: pc.P99 ?? null },
    top: (raw['k-best'] && raw['k-best'].top10) || null,
    bottom: (raw['k-best'] && raw['k-best'].bottom10) || null,
    hist: raw.histogram || null,
  };
}

export const STAT_KEYS = [
  { key: 'P50', label: 'P50 (median)' },
  { key: 'P01', label: 'P01' },
  { key: 'P20', label: 'P20' },
  { key: 'P80', label: 'P80' },
  { key: 'P99', label: 'P99' },
  { key: 'min', label: 'min' },
  { key: 'max', label: 'max' },
  { key: 'mean', label: 'mean' },
  { key: 'mean90', label: 'mean90' },
  { key: 'sum', label: 'sum' },
  { key: 'stddev', label: 'stddev' },
];

export function readStat(sample, statKey) {
  if (!sample) return null;
  if (sample.p && Object.prototype.hasOwnProperty.call(sample.p, statKey)) return sample.p[statKey];
  return sample[statKey] ?? null;
}

/* ------------------------------------------------------------ direction -- */

/** −1 = lower is better, +1 = higher is better, 0 = neutral (no colouring). */
export function metricDirection(name) {
  const n = String(name).toLowerCase();
  if (/(_ns|_ms|_us|_seconds|_latency|latency_|_cycles|_instructions|_branch_misses)$/.test(n) || /_ns\b/.test(n)) return -1;
  if (/(mismatch|miss_rate|false_accept|false_reject|error|offset_bp|regression|violation|stddev)/.test(n)) return -1;
  if (/(per_second|throughput|_ipc|correct_fraction|found_fraction|_accuracy|speedup)/.test(n)) return 1;
  if (/(rejection_rate|_rate)$/.test(n)) return 0;
  return 0;
}

/* --------------------------------------------------------------- format -- */

const SI = [
  { u: 'T', d: 1e12 }, { u: 'G', d: 1e9 }, { u: 'M', d: 1e6 }, { u: 'k', d: 1e3 },
];

function sig(v, digits = 3) {
  if (v === 0) return '0';
  const a = Math.abs(v);
  if (a >= 100) return v.toFixed(0);
  if (a >= 10) return v.toFixed(1);
  if (a >= 1) return v.toFixed(2);
  return v.toPrecision(digits);
}

export function formatValue(v, metric) {
  if (v == null || !isFinite(v)) return '—';
  const n = String(metric || '').toLowerCase();
  if (/_ns$/.test(n) || /_ns\b/.test(n)) return formatNs(v);
  if (/bytes|_bp_total|payload_bytes/.test(n) && !/offset/.test(n)) return formatBytes(v);
  if (/per_second/.test(n)) {
    for (const s of SI) if (Math.abs(v) >= s.d) return `${sig(v / s.d)} ${s.u}/s`;
    return `${sig(v)} /s`;
  }
  if (/fraction|_rate$/.test(n)) return `${sig(v * 100)}%`;
  if (Number.isInteger(v) && Math.abs(v) < 1e6) return v.toLocaleString('en-US');
  for (const s of SI) if (Math.abs(v) >= s.d) return `${sig(v / s.d)}${s.u}`;
  return sig(v);
}

export function formatNs(v) {
  const a = Math.abs(v);
  if (a >= 6e10) return `${sig(v / 6e10)} min`;
  if (a >= 1e9) return `${sig(v / 1e9)} s`;
  if (a >= 1e6) return `${sig(v / 1e6)} ms`;
  if (a >= 1e3) return `${sig(v / 1e3)} µs`;
  return `${sig(v)} ns`;
}

export function formatBytes(v) {
  const a = Math.abs(v);
  if (a >= 1 << 30) return `${sig(v / (1 << 30))} GiB`;
  if (a >= 1 << 20) return `${sig(v / (1 << 20))} MiB`;
  if (a >= 1024) return `${sig(v / 1024)} KiB`;
  return `${sig(v)} B`;
}

export function paramsLabel(params) {
  const ks = Object.keys(params || {}).filter((k) => k !== '_');
  if (!ks.length) return '';
  return ks.map((k) => `${k}=${params[k]}`).join(' · ');
}

/* ---------------------------------------------------------------- model -- */

function ensureBench(model, key) {
  if (!model.benchmarks[key]) {
    model.benchmarks[key] = {
      key, title: key, description: '', tags: [], fast: null, concurrency: null,
      iterationOrder: null, configUrl: null, reportUrls: [],
      testCases: {}, caseOrder: [], iterations: {},
      algorithms: {}, algoOrder: [], matrixOver: {},
      metrics: {}, metricOrder: [], regions: [], runs: [],
      regressionRules: [], findings: { violations: [], regressions: [], sanity: [], paired: [] },
      hasResults: false,
    };
    model.order.push(key);
  }
  return model.benchmarks[key];
}

function ingestConfig(model, file) {
  const j = file.json;
  const globalCases = j.test_cases || {};
  const globalParams = j.params || {};
  const rules = Array.isArray(j.regression_rules) ? j.regression_rules : [];
  for (const [key, block] of Object.entries(j)) {
    if (RESERVED.has(key) || !block || typeof block !== 'object' || !block.matrix) continue;
    const b = ensureBench(model, key);
    b.configUrl = file.url;
    b.description = block.description || b.description;
    b.tags = block.tags || [];
    b.fast = block.fast ?? null;
    b.concurrency = block.concurrency || null;
    b.iterationOrder = block.iteration_order || null;
    b.acceptance = block.matrix.acceptance || null;
    b.byRegions = !!block.matrix.by_regions;
    b.iterations = block.matrix.for || {};
    for (const tc of Object.keys(block.matrix.for || {})) {
      if (!b.testCases[tc]) {
        b.testCases[tc] = globalCases[tc] || {};
        b.caseOrder.push(tc);
      }
    }
    b.matrixOver = block.matrix.over || {};
    for (const algo of Object.keys(b.matrixOver)) {
      if (!b.algorithms[algo]) {
        b.algorithms[algo] = globalParams[algo] || {};
        b.algoOrder.push(algo);
      }
    }
    for (const [m, spec] of Object.entries(block.matrix.metrics || {})) {
      if (!b.metrics[m]) b.metricOrder.push(m);
      b.metrics[m] = Object.assign({ type: 'builtin' }, b.metrics[m], spec);
    }
    b.regressionRules = rules.filter(
      (r) => !r.benchmarks || (Array.isArray(r.benchmarks) && r.benchmarks.includes(key))
    );
  }
}

function ingestReport(model, file) {
  const j = file.json;
  const meta = j.meta || {};
  for (const [key, rb] of Object.entries(j.benchmarks || {})) {
    const b = ensureBench(model, key);
    b.hasResults = true;
    b.reportUrls.push(file.url);
    b.title = rb.title || b.title;
    if (rb.description) b.description = rb.description;
    b.location = rb.location || b.location;

    const run = {
      id: `${file.name}`, url: file.url, meta,
      created: meta.created || null, commit: meta.commit || null,
      dirty: !!meta.dirty, rows: [], caseSet: [], regionSet: [], metricSet: [],
      rawSamples: rb.raw_samples || null,
    };
    const caseSet = new Set(), regionSet = new Set(), metricSet = new Set();

    for (const et of rb.execution_tuples || []) {
      const algo = Object.keys(et.tuple || {})[0];
      if (!algo) continue;
      const params = et.tuple[algo] || {};
      const row = {
        id: `${algo}#${JSON.stringify(params)}`,
        algo, params, paramsLabel: paramsLabel(params),
        cells: {},
      };
      for (const [tc, regions] of Object.entries(et.test_cases || {})) {
        caseSet.add(tc);
        row.cells[tc] = {};
        for (const [region, metrics] of Object.entries(regions || {})) {
          regionSet.add(region);
          row.cells[tc][region] = {};
          for (const [m, raw] of Object.entries(metrics || {})) {
            metricSet.add(m);
            row.cells[tc][region][m] = normaliseSamples(raw);
          }
        }
      }
      run.rows.push(row);
      if (!b.algorithms[algo]) { b.algorithms[algo] = {}; b.algoOrder.push(algo); }
    }
    run.caseSet = [...caseSet];
    run.regionSet = [...regionSet].sort((a, b2) => (a === 'all_regions' ? -1 : b2 === 'all_regions' ? 1 : a.localeCompare(b2)));
    run.metricSet = [...metricSet].sort();
    b.runs.push(run);

    for (const tc of run.caseSet) {
      if (!b.testCases[tc]) { b.testCases[tc] = {}; b.caseOrder.push(tc); }
      if (rb.test_case_descriptions && rb.test_case_descriptions[tc] && !b.testCases[tc].description) {
        b.testCases[tc] = Object.assign({}, b.testCases[tc], { description: rb.test_case_descriptions[tc] });
      }
    }
    for (const m of run.metricSet) if (!b.metrics[m]) { b.metrics[m] = { type: 'reported' }; b.metricOrder.push(m); }
    b.regions = [...new Set([...(b.regions || []), ...run.regionSet])];

    const pick = (arr) => (arr || []).filter((e) => e.benchmark === key || !e.benchmark);
    b.findings.violations.push(...pick(j.rule_violations));
    b.findings.regressions.push(...pick(j.regressions));
    b.findings.paired.push(...pick(j.paired_rule_evidence));
    b.findings.sanity.push(...(j.sanity_warnings || []));
  }
}

export function buildModel(files) {
  const model = { benchmarks: {}, order: [], sources: files, errors: [] };
  for (const f of files) if (f.status === 'ok' && f.kind === 'config') ingestConfig(model, f);
  for (const f of files) if (f.status === 'ok' && f.kind === 'report') ingestReport(model, f);
  for (const f of files) if (f.status === 'error') model.errors.push(f);
  for (const f of files) {
    if (f.status !== 'ok') continue;
    f.benchmarkKeys = f.kind === 'report'
      ? Object.keys(f.json.benchmarks || {})
      : Object.keys(f.json).filter((k) => !RESERVED.has(k) && f.json[k] && f.json[k].matrix);
  }
  for (const key of model.order) {
    const bm = model.benchmarks[key];
    // richest report first (most tuples × cases), newest wins ties
    bm.runs.sort((a, z) =>
      (z.rows.length * Math.max(1, z.caseSet.length)) - (a.rows.length * Math.max(1, a.caseSet.length)) ||
      String(z.created || '').localeCompare(String(a.created || '')));
  }
  model.order.sort((a, b) => {
    const A = model.benchmarks[a], B = model.benchmarks[b];
    if (A.hasResults !== B.hasResults) return A.hasResults ? -1 : 1;
    return a.localeCompare(b);
  });
  return model;
}

/* ------------------------------------------------------------ compare ---- */

/**
 * Build a comparison table.
 *
 * `matrix.over` sweeps parameters *per algorithm*, so a row is only comparable
 * to rows carrying the same parameter values. Rows are therefore collected into
 * groups keyed by the parameters every algorithm in the run declares (the
 * intersection of the tuples' parameter names — `max_edits=8` groups with
 * `max_edits=8` and never with `max_edits=2`). Parameters only one algorithm
 * owns stay as per-row detail. Ratios, baselines and the per-column winner are
 * computed strictly inside a group.
 */
export function buildTable(run, { metric, region, stat, baselineAlgo, direction, cases, algoOrder }) {
  const cols = (cases && cases.length ? cases : run.caseSet).filter((tc) =>
    run.rows.some((r) => r.cells[tc] && r.cells[tc][region] && r.cells[tc][region][metric])
  );
  const rows = run.rows
    .map((r) => ({
      ...r,
      values: cols.map((tc) => {
        const s = r.cells[tc] && r.cells[tc][region] && r.cells[tc][region][metric];
        return { sample: s || null, value: readStat(s, stat) };
      }),
    }))
    .filter((r) => r.values.some((v) => v.value != null));

  const algos = [...new Set(rows.map((r) => r.algo))];
  const baseAlgo = algos.includes(baselineAlgo) ? baselineAlgo : algos[0];
  const algoIdx = (a) => { const i = (algoOrder || []).indexOf(a); return i < 0 ? 999 : i; };

  // parameters declared by every algorithm in this run = the grouping axis
  const perAlgoKeys = algos.map((a) => {
    const r = rows.find((x) => x.algo === a);
    return Object.keys(r.params).filter((k) => k !== '_');
  });
  const shared = (perAlgoKeys[0] || []).filter((k) => perAlgoKeys.every((ks) => ks.includes(k)));

  const keyOf = (r) => shared.map((k) => `${k}=${r.params[k]}`).join(' · ');
  const detailOf = (r) => Object.keys(r.params)
    .filter((k) => k !== '_' && !shared.includes(k))
    .map((k) => `${k}=${r.params[k]}`).join(' · ');

  const map = new Map();
  for (const r of rows) {
    const key = keyOf(r);
    if (!map.has(key)) map.set(key, { key, label: key, params: {}, rows: [] });
    const g = map.get(key);
    for (const k of shared) g.params[k] = r.params[k];
    r.detail = detailOf(r);
    g.rows.push(r);
  }

  const groups = [...map.values()];
  for (const g of groups) {
    g.rows.sort((a, z) => algoIdx(a.algo) - algoIdx(z.algo));
    const peer = g.rows.find((r) => r.algo === baseAlgo) || g.rows[0];
    g.baselineLabel = peer ? peer.algo + (peer.detail ? ' · ' + peer.detail : '') : null;
    g.span = cols.length + 1;
    // per-column winner inside this group
    const best = cols.map((_, i) => {
      if (!direction) return null;
      let win = null;
      for (const r of g.rows) {
        const v = r.values[i].value;
        if (v == null) continue;
        if (win == null || (direction < 0 ? v < win.v : v > win.v)) win = { v, id: r.id };
      }
      const withValue = g.rows.filter((r) => r.values[i].value != null).length;
      return win && withValue > 1 ? win.id : null;
    });
    for (const r of g.rows) {
      r.isBaseline = peer && r.id === peer.id;
      r.groupKey = g.key;
      r.values.forEach((cell, i) => {
        cell.base = peer ? peer.values[i].value : null;
        cell.isBest = best[i] === r.id;
        const base = cell.base;
        if (base == null || cell.value == null || base === 0 || r.isBaseline) { cell.ratio = null; return; }
        cell.ratio = direction < 0 ? base / cell.value : cell.value / base;
        cell.deltaPct = ((cell.value - base) / Math.abs(base)) * 100;
        cell.better = direction === 0 ? 0 : (cell.ratio > 1.0005 ? 1 : cell.ratio < 0.9995 ? -1 : 0);
      });
    }
  }

  return { cols, groups, algos, baseline: baseAlgo, groupedBy: shared };
}
