/**
 * Production runtime for the supplied BioseqDB benchmark design.
 *
 * The Component class below is copied from design_source/benchmarks.html. This
 * small adapter replaces only the prototype renderer: it expands the copied
 * sc-if/sc-for surface, binds events without inline handlers, and writes text
 * through DOM text nodes so report JSON never becomes executable markup.
 */

const bindingPattern = /\[\[\s*([^\]]+?)\s*\]\]/g;
const allowedStyleProperties = new Set([
  "background", "border", "border-color", "border-left", "box-shadow", "color",
  "height", "left", "opacity", "top", "transform",
]);

function valueAt(scope, expression) {
  const path = expression.trim();
  if (path === "true") return true;
  if (path === "false") return false;
  let value = scope;
  for (const part of path.split(".")) {
    if (value == null || !Object.prototype.hasOwnProperty.call(value, part)) {
      return undefined;
    }
    value = value[part];
  }
  return value;
}

function interpolate(value, scope) {
  return value.replace(bindingPattern, (_match, expression) => {
    const resolved = valueAt(scope, expression);
    return resolved == null ? "" : String(resolved);
  });
}

function applyDynamicStyles(element, template, scope) {
  for (const declaration of template.split(";")) {
    if (!declaration) continue;
    const separator = declaration.indexOf(":");
    if (separator <= 0) continue;
    const property = declaration.slice(0, separator).trim();
    const value = interpolate(declaration.slice(separator + 1), scope).trim();
    if (!allowedStyleProperties.has(property)) {
      throw new Error(`unsupported benchmark style binding: ${property}`);
    }
    if (/[;{}]|url\s*\(|expression\s*\(/i.test(value)) {
      throw new Error(`unsafe benchmark style value for ${property}`);
    }
    element.style.setProperty(property, value);
  }
}

function bindNode(node, scope) {
  if (node.nodeType === Node.TEXT_NODE) {
    node.nodeValue = interpolate(node.nodeValue || "", scope);
    return;
  }
  if (node.nodeType === Node.DOCUMENT_FRAGMENT_NODE) {
    for (const child of [...node.childNodes]) bindNode(child, scope);
    return;
  }
  if (!(node instanceof Element)) return;

  if (node.dataset.benchIf) {
    if (!valueAt(scope, node.dataset.benchIf)) {
      node.remove();
      return;
    }
    node.removeAttribute("data-bench-if");
  }
  if (node.dataset.benchFor) {
    const fragment = document.createDocumentFragment();
    const values = valueAt(scope, node.dataset.benchFor);
    for (const item of Array.isArray(values) ? values : []) {
      const clone = node.cloneNode(true);
      clone.removeAttribute("data-bench-for");
      clone.removeAttribute("data-bench-as");
      bindNode(clone, { ...scope, [node.dataset.benchAs]: item });
      fragment.append(clone);
    }
    node.replaceWith(fragment);
    return;
  }

  let boundValue;
  for (const attribute of [...node.attributes]) {
    if (attribute.name.startsWith("data-bench-")
        && ["click", "change", "mouseenter", "mouseleave"]
          .includes(attribute.name.slice("data-bench-".length))) {
      const eventName = attribute.name.slice("data-bench-".length);
      const handler = valueAt(scope, attribute.value);
      if (typeof handler === "function") node.addEventListener(eventName, handler);
      node.removeAttribute(attribute.name);
    } else if (attribute.name === "data-bench-style") {
      applyDynamicStyles(node, attribute.value, scope);
      node.removeAttribute(attribute.name);
    } else if (attribute.value.includes("[[")) {
      const value = interpolate(attribute.value, scope);
      node.setAttribute(attribute.name, value);
      if (attribute.name === "value") boundValue = value;
    }
  }
  for (const child of [...node.childNodes]) bindNode(child, scope);
  if (boundValue !== undefined && "value" in node) node.value = boundValue;
}

class DCLogic {
  props = {};

  setState(update) {
    const patch = typeof update === "function" ? update(this.state) : update;
    this.state = { ...this.state, ...patch };
    this.render();
  }

  mount(root, template, props) {
    this.root = root;
    this.template = template;
    this.props = props;
    this.render();
  }

  render() {
    if (!this.root || !this.template) return;
    const fragment = this.template.content.cloneNode(true);
    bindNode(fragment, this.renderVals());
    this.root.replaceChildren(fragment);
  }
}

class Component extends DCLogic {
  state = {
    loading: true, files: [], model: null, sel: null, view: 'compare',
    metric: null, region: 'all_regions', stat: 'P50', baseline: null,
    dir: null, runIdx: 0, open: {}, tip: null, showSources: true,
  };

  async componentDidMount() {
    this.lib = await import(this.props.loaderUrl);
    const manifest = (this.props.manifestUrl || '').trim();
    this.setState({ stat: this.props.defaultStat || 'P50' });
    await this.load(manifest ? [manifest] : this.lib.DEFAULT_SOURCES);
  }

  async load(sources) {
    this.sourceList = sources;
    this.setState({ loading: true });
    const files = await this.lib.loadSources(sources);
    const model = this.lib.buildModel(files);
    const richest = model.order
      .map((k) => {
        const bm = model.benchmarks[k], r = bm.runs[0];
        return { k, score: r ? r.rows.length * Math.max(1, r.caseSet.length) : 0 };
      })
      .sort((a, z) => z.score - a.score)[0];
    const sel = (model.benchmarks[this.state.sel] && this.state.sel) || (richest && richest.k) || model.order[0] || null;
    this.setState({ loading: false, files, model, sel, open: sel ? { [sel]: true } : {} });
    if (sel) this.resetForBench(sel, model);
  }

  resetForBench(key, model) {
    const b = (model || this.state.model).benchmarks[key];
    if (!b) return;
    const run = b.runs[0];
    const avail = run && run.metricSet.length ? run.metricSet : b.metricOrder;
    const metric = avail.includes('wall_clock_ns') ? 'wall_clock_ns' : (avail[0] || null);
    const region = run && run.regionSet.includes('all_regions') ? 'all_regions' : (run ? run.regionSet[0] : 'all_regions');
    this.setState({ metric, region, runIdx: 0, baseline: null, dir: null });
  }

  bench() { const m = this.state.model; return m && this.state.sel ? m.benchmarks[this.state.sel] : null; }
  run() { const b = this.bench(); return b && b.runs.length ? b.runs[Math.min(this.state.runIdx, b.runs.length - 1)] : null; }
  direction() {
    if (this.state.dir !== null) return this.state.dir;
    return this.lib ? this.lib.metricDirection(this.state.metric || '') : 0;
  }
  fmt(v) { return this.lib ? this.lib.formatValue(v, this.state.metric) : String(v); }

  showTip(e, payload) {
    const r = e.currentTarget.getBoundingClientRect();
    const w = 352;
    let x = Math.max(10, Math.min(r.left + r.width / 2 - w / 2, window.innerWidth - w - 10));
    let y = r.bottom + 12;
    if (y > window.innerHeight - 280) y = Math.max(10, r.top - 268);
    this.setState({ tip: Object.assign({ x, y }, payload) });
  }
  hideTip = () => { if (this.state.tip) this.setState({ tip: null }); };

  sampleRows(s) {
    if (!s) return [{ k: 'samples', v: 'none', color: '#7d94a6' }];
    const f = (v) => (v == null ? '—' : this.fmt(v));
    return [
      { k: 'samples', v: s.n == null ? '—' : String(s.n), color: '#cfe0ec' },
      { k: 'P01 / P20', v: f(s.p.P01) + ' / ' + f(s.p.P20), color: '#cfe0ec' },
      { k: 'P50', v: f(s.p.P50), color: '#8fd6ba' },
      { k: 'P80 / P99', v: f(s.p.P80) + ' / ' + f(s.p.P99), color: '#cfe0ec' },
      { k: 'min / max', v: f(s.min) + ' / ' + f(s.max), color: '#cfe0ec' },
      { k: 'mean / mean90', v: f(s.mean) + ' / ' + f(s.mean90), color: '#cfe0ec' },
      { k: 'stddev', v: f(s.stddev), color: '#9db1c2' },
    ];
  }
  histBars(s) {
    if (!s || !s.hist || !s.hist.length) return [];
    const max = Math.max(...s.hist.map((h) => h.count)) || 1;
    return s.hist.map((h) => ({ h: Math.round((h.count / max) * 100), color: h.count ? '#46b8de' : 'rgba(255,255,255,0.08)' }));
  }

  renderVals() {
    const st = this.state, model = st.model, lib = this.lib;
    const tip = st.tip;
    const out = {
      loading: st.loading, ready: false, empty: !st.loading && (!model || !model.order.length),
      onTipOut: this.hideTip,
      onReload: () => this.load(this.sourceList || []),
      onToggleSources: () => this.setState({ showSources: !st.showSources }),
      onTop: (e) => { e.preventDefault(); window.scrollTo({ top: 0, behavior: 'smooth' }); },
      showSources: st.showSources, srcCaret: st.showSources ? 'rotate(90deg)' : 'none',
      statusDot: st.loading ? '#f2a071' : (model && model.errors.length ? '#ef7a5a' : '#74c8a8'),
      statusText: st.loading ? 'loading' : (model ? model.order.length : 0) + ' benchmarks',
      sourceSummary: (st.files.filter((f) => f.status !== 'error').length) + '/' + st.files.length,
      sources: (st.files || []).map((f) => ({
        name: f.name, kind: f.status === 'error' ? 'err' : f.kind,
        dot: f.status === 'error' ? '#ef7a5a' : (f.kind === 'report' ? '#74c8a8' : f.kind === 'manifest' ? '#f2a071' : '#46b8de'),
        onEnter: (e) => this.showTip(e, {
          title: f.name, sub: f.status === 'error' ? 'unreachable' : f.kind,
          note: f.error || f.url,
          rows: (f.benchmarkKeys || []).slice(0, 9).map((k) => ({ k: 'benchmark', v: k, color: '#8fd6ba' })),
        }),
      })),
      tipX: tip ? tip.x : -9999, tipY: tip ? tip.y : -9999, tipOpacity: tip ? 1 : 0,
      hasTip: !!tip, tipTitle: tip ? tip.title : '', tipSub: tip ? (tip.sub || '') : '',
      tipNote: tip ? (tip.note || '') : '', tipHasNote: !!(tip && tip.note),
      tipRows: tip ? (tip.rows || []) : [],
      tipHasHist: !!(tip && tip.hist && tip.hist.length), tipHist: tip ? (tip.hist || []) : [],
      navGroups: [], metricOpts: [], statOpts: [], regionOpts: [], baselineOpts: [], runOpts: [],
      dirOpts: [], cols: [], groups: [], benchChips: [], casesList: [], algosList: [],
      metricsList: [],
      benchKey: '', benchTitle: '', benchDescription: '', viewLabel: '', tableNote: '',
      metric: st.metric || '', region: st.region || '', stat: st.stat, baseline: st.baseline || '',
      runIdx: String(st.runIdx), multiRun: false,
      isCompare: false, isCases: false, isAlgos: false, isMetrics: false,
    };
    if (!model || !model.order.length) return out;

    const VIEWS = [
      { id: 'compare', label: 'Comparison' },
      { id: 'cases', label: 'Test cases' }, { id: 'algos', label: 'Algorithms' },
      { id: 'metrics', label: 'Metrics' },
    ];

    out.navGroups = model.order.map((key) => {
      const b = model.benchmarks[key];
      const active = st.sel === key;
      const counts = {
        compare: b.runs.length ? String(b.runs[0].rows.length) : '—',
        cases: String(b.caseOrder.length), algos: String(b.algoOrder.length),
        metrics: String(b.metricOrder.length),
      };
      return {
        label: key, open: !!st.open[key],
        caret: st.open[key] ? 'rotate(90deg)' : 'none',
        headBg: active ? 'rgba(70,184,222,0.10)' : 'transparent',
        headColor: active ? '#bfe8f8' : (b.hasResults ? '#c2d4e1' : '#7d94a6'),
        badge: b.hasResults ? 'DATA' : 'CFG',
        badgeColor: b.hasResults ? '#0a2a1f' : '#8ba3b6',
        badgeBg: b.hasResults ? '#74c8a8' : 'rgba(255,255,255,0.07)',
        onToggle: () => {
          const open = Object.assign({}, st.open, { [key]: !st.open[key] });
          if (st.sel !== key) { this.setState({ sel: key, open }); this.resetForBench(key); }
          else this.setState({ open });
        },
        items: VIEWS.map((v) => {
          const on = active && st.view === v.id;
          return {
            label: v.label, count: counts[v.id],
            color: on ? '#eaf5fb' : '#9db1c2',
            bg: on ? 'rgba(255,255,255,0.06)' : 'transparent',
            bar: on ? '#f26522' : 'transparent',
            onClick: (e) => {
              e.preventDefault();
              if (st.sel !== key) { this.setState({ sel: key, view: v.id }); this.resetForBench(key); }
              else this.setState({ view: v.id });
            },
          };
        }),
      };
    });

    const b = this.bench();
    if (!b) return out;
    const run = this.run();
    out.ready = true;
    out.benchKey = b.key;
    out.benchTitle = b.title && b.title !== b.key ? b.title : b.key;
    out.benchDescription = b.description || 'No description declared in the configuration for this benchmark.';
    out.viewLabel = (VIEWS.find((v) => v.id === st.view) || VIEWS[0]).label;
    out['is' + st.view.charAt(0).toUpperCase() + st.view.slice(1)] = true;

    const chip = (text, tone, tipPayload) => {
      const tones = {
        teal: ['#a8e2f6', 'rgba(70,184,222,0.10)', 'rgba(70,184,222,0.28)'],
        mint: ['#a9e0c8', 'rgba(116,200,168,0.10)', 'rgba(116,200,168,0.28)'],
        amber: ['#ffc9a8', 'rgba(242,101,34,0.10)', 'rgba(242,101,34,0.30)'],
        grey: ['#a9bccb', 'rgba(255,255,255,0.045)', 'rgba(255,255,255,0.10)'],
      }[tone] || ['#a9bccb', 'rgba(255,255,255,0.045)', 'rgba(255,255,255,0.10)'];
      return {
        text, color: tones[0], bg: tones[1], border: tones[2],
        onEnter: tipPayload ? (e) => this.showTip(e, tipPayload) : () => {},
      };
    };
    out.benchChips = [];
    (b.tags || []).forEach((t) => out.benchChips.push(chip('#' + t, 'teal')));
    if (run) {
      out.benchChips.push(chip(run.id, 'mint', {
        title: run.id, sub: 'report artifact',
        note: run.url,
        rows: [
          { k: 'created', v: run.created || '—', color: '#cfe0ec' },
          { k: 'commit', v: run.commit ? run.commit.slice(0, 12) : '—', color: '#8fd6ba' },
          { k: 'worktree', v: run.dirty ? 'dirty' : 'clean', color: run.dirty ? '#f2a071' : '#8fd6ba' },
          { k: 'fast mode', v: String(!!run.meta.fast), color: '#cfe0ec' },
          { k: 'tuples', v: String(run.rows.length), color: '#cfe0ec' },
        ],
      }));
      if (run.commit) out.benchChips.push(chip(run.commit.slice(0, 7) + (run.dirty ? ' ·dirty' : ''), run.dirty ? 'amber' : 'grey'));
    } else {
      out.benchChips.push(chip('configuration only — no report published', 'amber'));
    }
    if (b.concurrency) out.benchChips.push(chip('bench_lock ' + (b.concurrency.bench_lock ? 'on' : 'off'), 'grey'));
    if (b.iterationOrder) out.benchChips.push(chip('order ' + b.iterationOrder.mode, 'grey'));

    /* ---- comparison table ---- */
    if (st.view === 'compare' && run) {
      const metric = st.metric || run.metricSet[0];
      const region = run.regionSet.includes(st.region) ? st.region : run.regionSet[0];
      const dir = this.direction();
      const table = lib.buildTable(run, {
        metric, region, stat: st.stat, baselineAlgo: st.baseline, direction: dir,
        algoOrder: b.algoOrder,
        cases: b.caseOrder.filter((c) => run.caseSet.includes(c)).concat(run.caseSet.filter((c) => !b.caseOrder.includes(c))),
      });

      out.metricOpts = run.metricSet.map((m) => ({ value: m, label: m }));
      out.statOpts = lib.STAT_KEYS.map((s) => ({ value: s.key, label: s.label }));
      out.regionOpts = run.regionSet.map((r) => ({ value: r, label: r }));
      out.baselineOpts = table.algos.map((a) => ({ value: a, label: a }));
      out.groupedBy = table.groupedBy;
      out.runOpts = b.runs.map((r, i) => ({ value: String(i), label: r.id + (r.created ? ' · ' + r.created.slice(0, 10) : '') }));
      out.multiRun = b.runs.length > 1;
      out.metric = metric; out.region = region;
      out.baseline = table.baseline || '';
      out.onMetric = (e) => this.setState({ metric: e.target.value, dir: null, tip: null });
      out.onStat = (e) => this.setState({ stat: e.target.value, tip: null });
      out.onRegion = (e) => this.setState({ region: e.target.value, tip: null });
      out.onBaseline = (e) => this.setState({ baseline: e.target.value, tip: null });
      out.onRun = (e) => this.setState({ runIdx: Number(e.target.value), tip: null });
      out.dirOpts = [
        { v: -1, label: '↓ lower is better' }, { v: 1, label: '↑ higher is better' }, { v: 0, label: '– neutral' },
      ].map((d) => ({
        label: d.label,
        bg: dir === d.v ? 'rgba(70,184,222,0.16)' : 'transparent',
        color: dir === d.v ? '#bfe8f8' : '#7d94a6',
        onClick: () => this.setState({ dir: d.v }),
      }));
      const nRows = table.groups.reduce((a, g) => a + g.rows.length, 0);
      out.tableNote = nRows + ' tuples × ' + table.cols.length + ' test cases · ' + st.stat + ' of ' + metric +
        (table.groupedBy.length ? ' · grouped by ' + table.groupedBy.join(', ') : '') +
        ' · baseline ' + table.baseline;

      const mspec = b.metrics[metric] || {};
      out.cols = table.cols.map((tc) => {
        const info = b.testCases[tc] || {};
        const data = info.data || {};
        return {
          name: tc, sub: info.data && info.data.size_label ? info.data.size_label : (b.iterations[tc] != null ? b.iterations[tc] + ' iters' : ''),
          onEnter: (e) => this.showTip(e, {
            title: tc, sub: 'test case',
            note: info.description || 'No description in the configuration.',
            rows: Object.keys(data).slice(0, 9).map((k) => ({ k, v: String(data[k]), color: '#cfe0ec' }))
              .concat(b.iterations[tc] != null ? [{ k: 'iterations', v: String(b.iterations[tc]), color: '#8fd6ba' }] : []),
          }),
        };
      });

      out.groups = table.groups.map((grp) => ({
        label: grp.label || 'no swept parameters',
        showHeader: table.groups.length > 1 || !!grp.label,
        span: table.cols.length + 1,
        rows: grp.rows.map((r) => {
        const schema = b.algorithms[r.algo] || {};
        return {
          algo: r.algo, detail: r.detail, hasDetail: !!r.detail,
          isBaseline: r.isBaseline,
          nameColor: r.isBaseline ? '#a9e0c8' : '#e2edf5',
          rowBg: r.isBaseline ? 'rgba(116,200,168,0.055)' : '#0a1826',
          onEnter: (e) => this.showTip(e, {
            title: r.algo, sub: 'execution tuple',
            note: (schema && Object.values(schema)[0] && Object.values(schema)[0].description) || '',
            rows: Object.keys(r.params).map((k) => ({
              k, v: String(r.params[k]),
              color: k === '_' ? '#7d94a6' : '#8fd6ba',
            })),
          }),
          cells: r.values.map((cell, i) => {
            const tc = table.cols[i];
            const ratio = cell.ratio;
            const better = cell.better || 0;
            const col = better > 0 ? '#74c8a8' : better < 0 ? '#ef7a5a' : '#7d94a6';
            return {
              text: cell.value == null ? '—' : lib.formatValue(cell.value, metric),
              valueColor: cell.value == null ? '#54697a' : '#eef4f9',
              bg: r.isBaseline ? 'rgba(116,200,168,0.045)' : (better > 0 ? 'rgba(116,200,168,0.05)' : better < 0 ? 'rgba(239,122,90,0.05)' : 'transparent'),
              ring: cell.isBest ? 'inset 0 0 0 2px #74c8a8' : 'none',
              arrow: r.isBaseline || ratio == null ? '' : (better > 0 ? '▲' : better < 0 ? '▼' : '='),
              ratioText: r.isBaseline ? 'baseline' : (ratio == null ? '' : ratio.toFixed(2) + '×'),
              chipColor: r.isBaseline ? '#74c8a8' : col,
              onEnter: (e) => this.showTip(e, {
                title: r.algo + ' · ' + tc,
                sub: metric + ' · ' + region,
                note: mspec.description || (b.testCases[tc] && b.testCases[tc].description) || '',
                rows: [
                  { k: st.stat + ' (shown)', v: cell.value == null ? '—' : lib.formatValue(cell.value, metric), color: '#f2f7fb' },
                  { k: 'parameters', v: grp.label || 'none swept', color: '#a8e2f6' },
                  { k: 'baseline', v: r.isBaseline ? 'this row' : (grp.baselineLabel || '—'), color: '#8fd6ba' },
                  { k: 'baseline value', v: cell.base == null ? '—' : lib.formatValue(cell.base, metric), color: '#cfe0ec' },
                  { k: 'vs baseline', v: ratio == null ? '—' : (ratio.toFixed(3) + '× · ' + (cell.deltaPct >= 0 ? '+' : '') + cell.deltaPct.toFixed(1) + '%'), color: col },
                  { k: 'rank', v: cell.isBest ? 'best in this group' : '—', color: cell.isBest ? '#74c8a8' : '#7d94a6' },
                ].concat(this.sampleRows(cell.sample)),
                hist: this.histBars(cell.sample),
              }),
            };
          }),
        };
      }),
      }));
    } else if (st.view === 'compare') {
      out.tableNote = 'No report published for this benchmark yet.';
      out.statOpts = lib.STAT_KEYS.map((s) => ({ value: s.key, label: s.label }));
      out.metricOpts = b.metricOrder.map((m) => ({ value: m, label: m }));
      out.regionOpts = [{ value: 'all_regions', label: 'all_regions' }];
      out.baselineOpts = [{ value: '', label: '—' }];
      out.dirOpts = [];
      out.onMetric = () => {}; out.onStat = (e) => this.setState({ stat: e.target.value });
      out.onRegion = () => {}; out.onBaseline = () => {}; out.onRun = () => {};
    }

    /* ---- test cases ---- */
    if (st.view === 'cases') {
      out.casesList = b.caseOrder.map((tc) => {
        const info = b.testCases[tc] || {};
        const data = info.data || {};
        return {
          name: tc,
          description: info.description || 'No description declared for this test case.',
          iterLabel: (b.iterations[tc] != null ? b.iterations[tc] : '?') + ' iterations',
          concLabel: 'concurrency ' + (info.concurrency != null ? info.concurrency : 'auto'),
          fields: Object.keys(data).map((k) => ({ k, v: String(data[k]) })),
        };
      });
    }

    /* ---- algorithms ---- */
    if (st.view === 'algos') {
      const measured = new Set(run ? run.rows.map((r) => r.algo) : []);
      out.algosList = b.algoOrder.map((name) => {
        const schema = b.algorithms[name] || {};
        const over = (b.matrixOver && b.matrixOver[name]) || {};
        const has = measured.has(name);
        return {
          name,
          state: has ? 'measured' : 'declared',
          stateColor: has ? '#0a2a1f' : '#8ba3b6',
          stateBg: has ? '#74c8a8' : 'rgba(255,255,255,0.07)',
          params: Object.keys(schema).length
            ? Object.keys(schema).map((p) => {
                const s = schema[p] || {};
                const swept = over[p];
                return {
                  name: p === '_' ? '(no parameters)' : p,
                  type: s.type || '—',
                  values: swept ? 'sweep: ' + swept.join(', ') : (s.enum ? s.enum.join(' | ') : (s.default !== undefined ? 'default ' + s.default : '')),
                  description: s.description || '',
                };
              })
            : [{ name: '(no schema)', type: '', values: '', description: 'This algorithm appears in a report but not in any loaded configuration.' }],
        };
      });
    }

    /* ---- metrics ---- */
    if (st.view === 'metrics') {
      out.metricsList = b.metricOrder.map((name) => {
        const spec = b.metrics[name] || {};
        const d = lib.metricDirection(name);
        return {
          name, kind: spec.type || 'builtin',
          kindColor: spec.type === 'evaluation' ? '#c9a8ff' : spec.type === 'counter' ? '#ffc9a8' : spec.type === 'region' ? '#8fd6ba' : '#7fd3ee',
          dirLabel: d < 0 ? '↓ lower' : d > 0 ? '↑ higher' : '– neutral',
          onOpen: (e) => { e.preventDefault(); this.setState({ view: 'compare', metric: name, dir: null }); },
        };
      });
    }

    return out;
  }
}
const root = document.querySelector("#benchmarks-app");
const template = document.querySelector("#benchmarks-app-template");
if (!(root instanceof HTMLElement) || !(template instanceof HTMLTemplateElement)) {
  throw new Error("benchmark application shell is missing");
}
const component = new Component();
component.mount(root, template, {
  loaderUrl: root.dataset.loaderUrl,
  manifestUrl: root.dataset.manifestUrl,
  defaultStat: "P50",
});
component.componentDidMount().catch((error) => {
  root.replaceChildren();
  const message = document.createElement("p");
  message.className = "benchmark-runtime-error";
  message.textContent = `Benchmark data could not be loaded: ${error.message || error}`;
  root.append(message);
});
