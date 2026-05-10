import { CustomizationOptions } from './types';

export function getDefaults(): CustomizationOptions {
  return {
    orientation: 'horizontal',
    width: 80,
    padding: 1,
    labels: true,
    colorLabels: true,
    percentage: false,
    sort: true,
    reverse: false,
    naked: false,
    char: '█',
    title: '',
    color: '',
    theme: '',
    structure: {
      x: '═',
      y: '╢',
      axis: '║',
      topLeft: '╔',
      bottomLeft: '╚'
    }
  };
}

/**
 * Parse the user-facing `--color` value into the shape chartscii expects.
 *
 * - `gradient(...)` → preserved as a single string
 * - `key:value,key:value` → status color map (Record<string, string>)
 * - `red,green,blue` → string[] for per-series / [bull, bear]
 * - anything else → string
 *
 * Commas inside parentheses are not split (so multi-stop gradients survive).
 */
function parseColorValue(value: string): string | string[] | Record<string, string> {
  const trimmed = value.trim();

  if (/^gradient\s*\(/i.test(trimmed)) return trimmed;

  // Split on commas not inside parentheses.
  const parts: string[] = [];
  let depth = 0;
  let buf = '';
  for (const ch of trimmed) {
    if (ch === '(') depth++;
    else if (ch === ')') depth = Math.max(0, depth - 1);
    if (ch === ',' && depth === 0) {
      parts.push(buf.trim());
      buf = '';
      continue;
    }
    buf += ch;
  }
  if (buf.trim()) parts.push(buf.trim());

  if (parts.length <= 1) return trimmed;

  // Status color map: every entry has a `:` separator.
  if (parts.every(p => /^[^:]+:[^:]+$/.test(p))) {
    const map: Record<string, string> = {};
    for (const part of parts) {
      const idx = part.indexOf(':');
      const key = part.slice(0, idx).trim();
      const val = part.slice(idx + 1).trim();
      if (key) map[key] = val;
    }
    return map;
  }

  return parts;
}

export function buildOptions(options: Partial<CustomizationOptions>): CustomizationOptions {
  const clean = cleanYargsOptions(options);
  const defaults = getDefaults();

  if ((clean as any).width === 'auto') {
    clean.width = process.stdout.columns || 80;
  }
  if ((clean as any).height === 'auto') {
    clean.height = process.stdout.rows || 20;
  }

  const newStructure = clean.structure;
  delete clean.structure;

  const config = {
    ...defaults,
    ...clean
  };

  if (newStructure) {
    config.structure = {
      ...defaults.structure!,
      ...newStructure
    };
  }

  if (config.height === undefined) {
    const type = (config as any).type;
    if (type && type !== 'bar') {
      // line / step / scatter / candlestick / status need vertical space —
      // match the lib's default of 10.
      config.height = 10;
    } else {
      config.height = config.orientation === 'horizontal' ? 1 : 20;
    }
  }

  // Default sort to off for non-bar chart types when the user didn't set it.
  // line / step / scatter / candlestick / status are typically already
  // ordered along the x-axis and sorting would scramble them. The CLI
  // default of true still applies to bar charts; --sort / --no-sort wins.
  const type = (config as any).type;
  if (type && type !== 'bar' && (clean as any).sort === undefined) {
    config.sort = false;
  }

  return config as CustomizationOptions;
}

function cleanYargsOptions(options: any): Partial<CustomizationOptions> {
  const clean: any = { ...options };

  delete clean._;
  delete clean.$0;
  delete clean.help;
  delete clean.version;

  if (clean.color === '' || clean.color === undefined) {
    delete clean.color;
  } else if (typeof clean.color === 'string') {
    clean.color = parseColorValue(clean.color);
  }

  if (clean.theme === '') delete clean.theme;
  if (clean['fill-color'] === '') delete clean['fill-color'];
  if (clean['align-bars'] === '') delete clean['align-bars'];

  // Handle title config: convert title, title-color, title-align, and title-padding into TitleConfig
  const titleText = clean.title || '';
  const titleColor = clean['title-color'] || '';
  const titleAlign = clean['title-align'] || 'left';
  const titlePaddingStr = clean['title-padding'] || '';

  delete clean.title;
  delete clean['title-color'];
  delete clean['title-align'];
  delete clean['title-padding'];
  delete clean.titleColor;
  delete clean.titleAlign;
  delete clean.titlePadding;

  // Parse title padding (CSS-style: number, "v,h", or "top,right,bottom,left")
  let titlePadding: number | [number, number] | [number, number, number, number] | undefined;
  if (titlePaddingStr) {
    const parts = titlePaddingStr.split(',').map((p: string) => parseInt(p.trim(), 10));
    if (parts.length === 1 && !isNaN(parts[0])) {
      titlePadding = parts[0];
    } else if (parts.length === 2 && parts.every((p: number) => !isNaN(p))) {
      titlePadding = [parts[0], parts[1]];
    } else if (parts.length === 4 && parts.every((p: number) => !isNaN(p))) {
      titlePadding = [parts[0], parts[1], parts[2], parts[3]];
    }
  }

  if (titleText) {
    // Only create TitleConfig object if we have non-default options
    if (titleColor || titleAlign !== 'left' || titlePadding !== undefined) {
      clean.title = {
        text: titleText,
        ...(titleAlign !== 'left' && { align: titleAlign }),
        ...(titleColor && { color: titleColor }),
        ...(titlePadding !== undefined && { padding: titlePadding })
      };
    } else {
      clean.title = titleText;
    }
  }

  if (clean['color-labels'] !== undefined) {
    clean.colorLabels = clean['color-labels'];
    delete clean['color-labels'];
  }

  if (clean['value-labels'] !== undefined) {
    clean.valueLabels = clean['value-labels'];
    delete clean['value-labels'];
  }

  if (clean['value-labels-prefix'] !== undefined) {
    clean.valueLabelsPrefix = clean['value-labels-prefix'];
    delete clean['value-labels-prefix'];
  }

  if (clean['value-labels-floating-point'] !== undefined) {
    clean.valueLabelsFloatingPoint = clean['value-labels-floating-point'];
    delete clean['value-labels-floating-point'];
  }

  if (clean['max-value'] !== undefined) {
    clean.maxValue = clean['max-value'];
    delete clean['max-value'];
  }

  if (clean['bar-size'] !== undefined) {
    clean.barSize = clean['bar-size'];
    delete clean['bar-size'];
  }

  if (clean['fill-color'] !== undefined) {
    clean.fillColor = clean['fill-color'];
    delete clean['fill-color'];
  }

  if (clean['align-bars'] !== undefined) {
    clean.alignBars = clean['align-bars'];
    delete clean['align-bars'];
  }

  if (clean['stack-colors'] !== undefined && Array.isArray(clean['stack-colors']) && clean['stack-colors'].length > 0) {
    clean.stackColors = clean['stack-colors'];
    delete clean['stack-colors'];
  }

  if (clean['stack-labels'] !== undefined && Array.isArray(clean['stack-labels']) && clean['stack-labels'].length > 0) {
    clean.stackLabels = clean['stack-labels'];
    delete clean['stack-labels'];
  }

  if (clean['stack-value-labels'] !== undefined) {
    clean.stackValueLabels = clean['stack-value-labels'];
    delete clean['stack-value-labels'];
    if (clean.stackValueLabels) {
      clean.valueLabels = true;
    }
  }

  if (clean['point-char'] !== undefined) {
    if (clean['point-char'] !== '') {
      clean.pointChar = clean['point-char'];
    }
    delete clean['point-char'];
  }
  if (clean.pointChar === '') delete clean.pointChar;

  if (clean['rich-labels'] !== undefined) {
    clean.richLabels = clean['rich-labels'];
    delete clean['rich-labels'];
  }

  // Drop default 'bar' to avoid forcing the option into the merged config —
  // the lib defaults to 'bar' on its own and this leaves room for callers
  // who passed a non-default `type` via the API path.
  if (clean.type === 'bar') delete clean.type;

  // Build legend config from --legend / --legend-position / --legend-align /
  // --legend-values flags. Single boolean stays a boolean; combined flags
  // become a LegendConfig object.
  const legendEnabled = clean.legend;
  const legendPosition = clean['legend-position'];
  const legendAlign = clean['legend-align'];
  const legendValuesRaw = clean['legend-values'];
  delete clean.legend;
  delete clean['legend-position'];
  delete clean['legend-align'];
  delete clean['legend-values'];
  delete clean.legendPosition;
  delete clean.legendAlign;
  delete clean.legendValues;

  const legendValues = Array.isArray(legendValuesRaw) && legendValuesRaw.length > 0
    ? legendValuesRaw.map((v: unknown) => String(v))
    : undefined;
  const legendPositionExplicit = legendPosition && legendPosition !== 'top' ? legendPosition : undefined;
  const legendAlignExplicit = legendAlign && legendAlign !== 'left' ? legendAlign : undefined;

  if (legendEnabled || legendValues || legendPositionExplicit || legendAlignExplicit) {
    if (!legendValues && !legendPositionExplicit && !legendAlignExplicit) {
      clean.legend = true;
    } else {
      clean.legend = {
        enabled: true,
        ...(legendValues && { values: legendValues }),
        ...(legendPositionExplicit && { position: legendPositionExplicit }),
        ...(legendAlignExplicit && { align: legendAlignExplicit }),
      };
    }
  }

  // Handle label-format: convert string template to function
  // Note: yargs creates both 'label-format' and 'labelFormat' keys
  const labelFormatStr = clean['label-format'] || '';
  delete clean['label-format'];
  delete clean.labelFormat;
  if (labelFormatStr) {
    clean.labelFormat = (label: string) => labelFormatStr.replace(/\{label\}/g, label);
  }

  // Handle value-label-format: convert string template to function.
  // The lib calls valueLabelFormat with an array: `[v]` for regular bars,
  // `[v0, v1, v2, …]` for stacked. Templates support:
  //   {value}     → values joined with `|` (matches the lib's default)
  //   {value:N}   → segment at index N (or '' if out of range)
  //   {sep}       → join character ('|' by default)
  // Note: yargs creates both 'value-label-format' and 'valueLabelFormat' keys
  const valueLabelFormatStr = clean['value-label-format'] || '';
  delete clean['value-label-format'];
  delete clean.valueLabelFormat;
  if (valueLabelFormatStr) {
    clean.valueLabelFormat = (values: string[]) => {
      const arr = Array.isArray(values) ? values : [String(values)];
      let result = valueLabelFormatStr;
      result = result.replace(/\{value:(\d+)\}/g, (_: string, idx: string) => arr[parseInt(idx, 10)] ?? '');
      result = result.replace(/\{value\}/g, arr.join('|'));
      return result;
    };
  }

  const structure: any = {};
  let hasStructure = false;

  if (clean['structure-x'] || clean.structureX) {
    structure.x = clean['structure-x'] || clean.structureX;
    delete clean['structure-x'];
    delete clean.structureX;
    hasStructure = true;
  }

  if (clean['structure-y'] || clean.structureY) {
    structure.y = clean['structure-y'] || clean.structureY;
    delete clean['structure-y'];
    delete clean.structureY;
    hasStructure = true;
  }

  if (clean['structure-axis'] || clean.structureAxis) {
    structure.axis = clean['structure-axis'] || clean.structureAxis;
    delete clean['structure-axis'];
    delete clean.structureAxis;
    hasStructure = true;
  }

  if (clean['structure-bottom-left'] || clean.structureBottomLeft) {
    structure.bottomLeft = clean['structure-bottom-left'] || clean.structureBottomLeft;
    delete clean['structure-bottom-left'];
    delete clean.structureBottomLeft;
    hasStructure = true;
  }

  if (hasStructure) {
    clean.structure = structure;
  }

  return clean;
}
