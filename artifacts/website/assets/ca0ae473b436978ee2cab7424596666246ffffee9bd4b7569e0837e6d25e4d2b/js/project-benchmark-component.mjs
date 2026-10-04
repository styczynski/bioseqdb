// Presentation seams for the unmodified supplied benchmark component.
export function adaptProjectComponent(Component) {
  return class ProjectComponent extends Component {
    async load(sources) {
      await super.load(sources);
      const key = new URLSearchParams(location.search).get('table');
      if (key && this.state.model?.benchmarks[key]) {
        this.setState({ sel: key, open: { [key]: true } });
        this.resetForBench(key);
      }
    }
    sampleRows(sample) {
      if (!sample?.published) return super.sampleRows(sample);
      const cell = sample.source;
      return [
        { k: 'A · complete', v: cell.a_ns == null ? '—' : this.lib.formatNs(cell.a_ns), color: '#cfe0ec' },
        { k: 'B · without CIGAR/MAPQ', v: cell.b_ns == null ? '—' : this.lib.formatNs(cell.b_ns), color: '#cfe0ec' },
        { k: 'samples', v: cell.samples == null ? '—' : String(cell.samples), color: '#cfe0ec' },
        { k: 'status', v: cell.status, color: '#cfe0ec' },
        { k: 'timing scope', v: cell.timing_scope || '—', color: '#9db1c2' },
        { k: 'source', v: cell.source || '—', color: '#9db1c2' },
        ...(cell.note ? [{ k: 'note', v: String(cell.note), color: '#ffc9a8' }] : []),
      ];
    }
    renderVals() {
      const values = super.renderVals();
      const panel = this.bench()?.projectPanel;
      if (!panel) return values;
      values.benchDescription = '';
      values.benchChips = [];
      values.statOpts = [{ value: 'P50', label: 'P50 (published median)' }];
      values.metricOpts = [{ value: 'wall_clock_ns', label: 'A · Complete time' }, { value: 'without_cigar_mapq_ns', label: 'B · Without CIGAR/MAPQ' }];
      values.regionOpts = [{ value: 'all_regions', label: 'Complete measured interval' }];
      values.baselineOpts = panel.rows.map((r) => ({ value: r.id, label: r.label }));
      values.tableNote = '1,024 queries · P50 · baseline ' + (panel.rows.find((r) => r.id === values.baseline)?.label || 'minimap2');
      values.cols.forEach((column, i) => { column.name = panel.columns[i].label; column.sub = panel.columns[i].detail; });
      values.casesList.forEach((entry, i) => {
        entry.name = panel.columns[i].label;
        entry.iterLabel = '1,024 queries';
        entry.concLabel = '';
      });
      values.algosList.forEach((entry, i) => {
        const row = panel.rows[i];
        entry.name = row.label;
        entry.state = Object.values(row.cells).some((cell) => cell.a_ns != null) ? 'measured' : 'unavailable';
        entry.params = [{ name: 'configuration', type: '', values: row.label, description: '' }];
      });
      values.groups.forEach((group) => group.rows.forEach((row) => {
        const source = panel.rows.find((r) => r.id === row.algo);
        row.algo = source.label;
        row.nameColor = source.preferred ? '#f2d28c' : row.isBaseline ? '#b9c5cf' : '#e2edf5';
        row.rowBg = source.preferred ? 'rgba(242,210,140,.08)' : row.isBaseline ? 'rgba(190,200,210,.08)' : '#0a1826';
        row.cells.forEach((cell, i) => {
          const observation = source.cells[panel.columns[i].id];
          if (observation.a_ns == null) {
            cell.text = observation.status === 'failed' ? '×' : observation.status === 'hardware_unavailable' ? '—g' : '·';
            cell.ratioText = '';
          }
        });
      }));
      return values;
    }
  };
}
