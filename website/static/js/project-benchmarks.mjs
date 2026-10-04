// Render chapter excerpts with the table and tooltip copied from the supplied
// benchmark design. A and B retain the paper's top/bottom order.
import { ProjectComponent } from './benchmarks-design.mjs';
import * as loader from './project-benchmark-loader.mjs';

class Excerpt extends ProjectComponent {
  renderVals() {
    const primary = super.renderVals();
    const metric = this.state.metric;
    this.state.metric = 'without_cigar_mapq_ns';
    const secondary = super.renderVals();
    this.state.metric = metric;
    const panel = this.bench()?.projectPanel;
    if (!panel) return primary;
    const allowedRows = this.rowIds || [];
    const allowedColumns = this.columnIds || [];
    const selected = panel.columns.map((c, i) => i).filter((i) => !allowedColumns.length || allowedColumns.includes(panel.columns[i].id));
    primary.cols = selected.map((i) => primary.cols[i]);
    primary.groups.forEach((group, g) => {
      group.span = selected.length + 1;
      group.rows.forEach((row, r) => {
        row.cells = selected.map((i) => ({ ...row.cells[i],
          secondText: secondary.groups[g].rows[r].cells[i].text,
          secondColor: secondary.groups[g].rows[r].cells[i].valueColor,
          secondChipColor: secondary.groups[g].rows[r].cells[i].chipColor,
          secondArrow: secondary.groups[g].rows[r].cells[i].arrow,
          secondRatioText: secondary.groups[g].rows[r].cells[i].ratioText,
        }));
      });
      group.rows = group.rows.filter((_, i) => !allowedRows.length || allowedRows.includes(panel.rows[i].id));
    });
    return primary;
  }
}

const excerpts = [...document.querySelectorAll('.project-benchmark[data-table]')];
if (excerpts.length) {
  Promise.all([
    loader.loadSources(loader.DEFAULT_SOURCES),
    fetch(new URL('../project-benchmark-template.json', import.meta.url)).then(async (response) => {
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const template = await response.json();
      if (typeof template.html !== 'string') throw new Error('Invalid benchmark table template');
      return template.html;
    }),
  ]).then(([files, markup]) => {
    const model = loader.buildModel(files);
    const template = document.createElement('template');
    template.innerHTML = markup; // Build-owned template; JSON is bound as text.
    for (const root of excerpts) {
      const key = root.dataset.table;
      if (!model.benchmarks[key]) throw new Error(`Unknown benchmark panel: ${key}`);
      const component = new Excerpt();
      component.lib = loader;
      component.rowIds = (root.dataset.rows || '').split(',').map((s) => s.trim()).filter(Boolean);
      component.columnIds = (root.dataset.columns || '').split(',').map((s) => s.trim()).filter(Boolean);
      component.state = { ...component.state, loading: false, model, files, sel: key, metric: 'wall_clock_ns', region: 'all_regions', baseline: 'minimap2' };
      root.classList.add('benchmarks-design-page');
      component.mount(root, template, {});
    }
  }).catch((error) => {
    for (const root of excerpts) {
      const message = document.createElement('p');
      message.textContent = `Benchmark table could not be loaded: ${error.message || error}`;
      root.append(message);
    }
  });
}
