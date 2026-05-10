import { describe, test, expect } from 'vitest';
import * as config from './config';

describe('config', () => {
  describe('getDefaults', () => {
    test('should return sensible defaults', () => {
      const defaults = config.getDefaults();

      expect(defaults.orientation).to.equal('horizontal');
      expect(defaults.width).to.equal(80);
      // height is not set in defaults - it's set dynamically based on orientation in buildOptions
      expect(defaults.height).to.be.undefined;
      expect(defaults.labels).to.be.true;
      expect(defaults.colorLabels).to.be.true;
      expect(defaults.sort).to.be.true;
      expect(defaults.char).to.equal('█');
      // fill is opt-in, not a default
      expect(defaults.fill).to.be.undefined;
    });

    test('should include structure defaults', () => {
      const defaults = config.getDefaults();

      expect(defaults.structure).to.exist;
      expect(defaults.structure?.x).to.equal('═');
      expect(defaults.structure?.y).to.equal('╢');
      expect(defaults.structure?.axis).to.equal('║');
      expect(defaults.structure?.bottomLeft).to.equal('╚');
    });
  });

  describe('buildOptions', () => {
    test('should start with defaults', () => {
      const opts = config.buildOptions({});

      expect(opts.width).to.equal(80);
      expect(opts.orientation).to.equal('horizontal');
    });

    test('should override defaults with options', () => {
      const opts = config.buildOptions({
        width: 120,
        height: 40,
        sort: false,
        percentage: true
      });

      expect(opts.width).to.equal(120);
      expect(opts.height).to.equal(40);
      expect(opts.sort).to.be.false;
      expect(opts.percentage).to.be.true;
    });

    test('should clean yargs options', () => {
      const opts = config.buildOptions({
        _: [1, 2, 3],
        $0: 'chartscii',
        'color-labels': false,
        'bar-size': 2
      } as any);

      expect(opts).to.not.have.property('_');
      expect(opts).to.not.have.property('$0');
      expect(opts.colorLabels).to.be.false;
      expect(opts.barSize).to.equal(2);
    });

    test('should handle structure options from yargs', () => {
      const opts = config.buildOptions({
        'structure-x': '─',
        'structure-y': '│',
        'structure-axis': '┃',
        'structure-bottom-left': '└'
      } as any);

      expect(opts.structure?.x).to.equal('─');
      expect(opts.structure?.y).to.equal('│');
      expect(opts.structure?.axis).to.equal('┃');
      expect(opts.structure?.bottomLeft).to.equal('└');
    });

    test('should preserve structure when merging', () => {
      const opts = config.buildOptions({
        structure: {
          x: '─'
        }
      } as any);

      // Should preserve other structure properties
      expect(opts.structure?.x).to.equal('─');
      expect(opts.structure?.y).to.equal('╢'); // default preserved
      expect(opts.structure?.axis).to.equal('║'); // default preserved
    });

    test('should handle empty options', () => {
      const opts = config.buildOptions({});
      expect(opts.width).to.equal(80); // defaults preserved
    });

    test('should handle partial structure', () => {
      const opts = config.buildOptions({
        structure: {
          x: '='
        }
      } as any);

      expect(opts.structure?.x).to.equal('=');
      expect(opts.structure?.y).to.equal('╢');
    });

    test('should handle zero values', () => {
      const opts = config.buildOptions({
        padding: 0,
        barSize: 0
      });

      expect(opts.padding).to.equal(0);
      expect(opts.barSize).to.equal(0);
    });

    test('should pass through chart type', () => {
      const opts = config.buildOptions({ type: 'line' } as any);
      expect((opts as any).type).to.equal('line');
    });

    test('should drop default bar type', () => {
      const opts = config.buildOptions({ type: 'bar' } as any);
      expect(opts).to.not.have.property('type');
    });

    test('should default sort to false for non-bar types', () => {
      const opts = config.buildOptions({ type: 'line' } as any);
      expect(opts.sort).to.be.false;
    });

    test('should respect explicit sort on non-bar types', () => {
      const opts = config.buildOptions({ type: 'line', sort: true } as any);
      expect(opts.sort).to.be.true;
    });

    test('should pass through variant, points, pointChar, richLabels', () => {
      const opts = config.buildOptions({
        type: 'step',
        variant: 'smooth',
        points: true,
        'point-char': '◈',
        'rich-labels': false,
      } as any);
      expect((opts as any).variant).to.equal('smooth');
      expect((opts as any).points).to.be.true;
      expect((opts as any).pointChar).to.equal('◈');
      expect((opts as any).richLabels).to.be.false;
    });

    test('should split comma color into per-series array', () => {
      const opts = config.buildOptions({ color: 'red,green,blue' } as any);
      expect(opts.color).to.deep.equal(['red', 'green', 'blue']);
    });

    test('should preserve gradient string', () => {
      const opts = config.buildOptions({ color: 'gradient(red,blue)' } as any);
      expect(opts.color).to.equal('gradient(red,blue)');
    });

    test('should parse status color map', () => {
      const opts = config.buildOptions({ color: '0:red,1:green,2:yellow' } as any);
      expect(opts.color).to.deep.equal({ '0': 'red', '1': 'green', '2': 'yellow' });
    });

    test('should keep single color as string', () => {
      const opts = config.buildOptions({ color: 'auto' } as any);
      expect(opts.color).to.equal('auto');
    });

    test('should build legend config from sub-options', () => {
      const opts = config.buildOptions({
        legend: true,
        'legend-position': 'bottom',
        'legend-align': 'right',
        'legend-values': ['Q1', 'Q2', 'Q3'],
      } as any);
      expect(opts.legend).to.deep.equal({
        enabled: true,
        values: ['Q1', 'Q2', 'Q3'],
        position: 'bottom',
        align: 'right',
      });
    });

    test('should set legend to true when only --legend is passed', () => {
      const opts = config.buildOptions({ legend: true } as any);
      expect(opts.legend).to.be.true;
    });

    test('should leave legend unset when not requested', () => {
      const opts = config.buildOptions({} as any);
      expect(opts.legend).to.be.undefined;
    });

    test('should build labelFormat from {label} template', () => {
      const opts = config.buildOptions({ 'label-format': '[{label}]' } as any);
      expect(opts.labelFormat).to.be.a('function');
      expect(opts.labelFormat!('Q1')).to.equal('[Q1]');
    });

    test('should build valueLabelFormat that handles single values', () => {
      const opts = config.buildOptions({ 'value-label-format': '${value}' } as any);
      expect(opts.valueLabelFormat).to.be.a('function');
      expect((opts as any).valueLabelFormat(['100'])).to.equal('$100');
    });

    test('should join stacked values with `|` for {value} placeholder', () => {
      const opts = config.buildOptions({ 'value-label-format': '${value}' } as any);
      expect((opts as any).valueLabelFormat(['100', '50', '30'])).to.equal('$100|50|30');
    });

    test('should support indexed {value:N} placeholders for stacked', () => {
      const opts = config.buildOptions({
        'value-label-format': '{value:0}↑{value:1}↓{value:2}',
      } as any);
      expect((opts as any).valueLabelFormat(['100', '50', '30'])).to.equal('100↑50↓30');
    });

    test('should return empty for {value:N} out-of-range index', () => {
      const opts = config.buildOptions({ 'value-label-format': '{value:5}' } as any);
      expect((opts as any).valueLabelFormat(['100'])).to.equal('');
    });
  });
});
