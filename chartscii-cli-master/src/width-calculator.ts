import { InputData, CustomizationOptions } from './types';

function calculateMaxLabelWidth(data: InputData[], includePercentage: boolean): number {
  let maxWidth = 0;

  for (const item of data) {
    let labelWidth = 0;

    if (typeof item === 'number') {
      labelWidth = item.toString().length;
    } else if (item.label) {
      labelWidth = item.label.length;
    } else if (typeof item.value === 'number') {
      // Object with value but no label - use value as label
      labelWidth = item.value.toString().length;
    } else if (Array.isArray(item.value)) {
      // Stacked data - sum the values and use that as label
      const total = item.value.reduce((sum: number, v: any) => {
        return sum + (typeof v === 'number' ? v : v.value || 0);
      }, 0);
      labelWidth = total.toString().length;
    }

    // Add space for percentage if enabled (e.g., " (33.33%)" = 10 chars)
    if (includePercentage && labelWidth > 0) {
      labelWidth += 10;
    }

    maxWidth = Math.max(maxWidth, labelWidth);
  }

  return maxWidth;
}

export function calculateBarAreaWidth(
  targetWidth: number,
  data: InputData[],
  options: Partial<CustomizationOptions>
): number {
  // Non-bar chart types render onto a fixed-width canvas; the lib uses
  // `width` directly. Only the bar formatters consume the label / structure
  // / value-label gutter that this function compensates for.
  const type = (options as any).type;
  if (type && type !== 'bar') {
    return Math.max(10, targetWidth);
  }

  const orientation = options.orientation || 'horizontal';
  const isNaked = options.naked || false;
  const hasLabels = options.labels !== false; // default is true
  const hasPercentage = options.percentage || false;
  const hasValueLabels = options.valueLabels || false;

  if (orientation === 'horizontal') {
    // For horizontal charts: targetWidth = labelWidth + structure + barWidth + valueLabel
    let consumed = 0;

    if (hasLabels) {
      const maxLabelWidth = calculateMaxLabelWidth(data, hasPercentage);
      consumed += maxLabelWidth;
      // Add offset space (the space after label before structure)
      consumed += (hasPercentage ? 0 : 1);
    }

    if (!isNaked) {
      // Structure character (╢)
      consumed += 1;
    }

    if (hasValueLabels) {
      // Value labels appear at the end of bars (e.g., " 5.00")
      // Calculate max value label width
      let maxValueLabelWidth = 0;
      for (const item of data) {
        let value = 0;
        if (typeof item === 'number') {
          value = item;
        } else if (typeof item.value === 'number') {
          value = item.value;
        } else if (Array.isArray(item.value)) {
          value = item.value.reduce((sum: number, v: any) => {
            return sum + (typeof v === 'number' ? v : v.value || 0);
          }, 0);
        }

        // Format with floating point precision (default 2)
        const floatingPoint = options.valueLabelsFloatingPoint ?? 2;
        const valueStr = value.toFixed(floatingPoint);
        maxValueLabelWidth = Math.max(maxValueLabelWidth, valueStr.length + 1); // +1 for space before
      }
      consumed += maxValueLabelWidth;
    }

    // Remaining width is for bars
    return Math.max(10, targetWidth - consumed);
  } else {
    // For vertical charts: need to account for library's auto-calculation logic
    const barSize = options.barSize;
    const padding = options.padding;

    // Start with target minus structure char (if not naked)
    let availableWidth = isNaked ? targetWidth : targetWidth - 1;

    // When neither barSize nor padding is set, library calculates both based on the width we pass
    // The library's calculateDefaultDimensions has a +1 which causes overflow
    // We need to iteratively find the right width to pass
    if (barSize === undefined && padding === undefined) {
      const barCount = data.length;
      const charWidth = (options.char || '█').length;

      // Binary search for the width that produces closest to availableWidth
      let low = Math.floor(availableWidth * 0.8);
      let high = availableWidth;
      let bestWidth = availableWidth;
      let bestDiff = Infinity;

      for (let testWidth = low; testWidth <= high; testWidth++) {
        // Simulate library calculation
        const calcBarWidth = Math.floor(testWidth / barCount / charWidth) + 1;
        const calcPadding = Math.round(testWidth / barCount / charWidth);
        const defaultPadding = calcPadding <= calcBarWidth ? 0 : calcPadding - calcBarWidth;
        const output = barCount * (calcBarWidth * charWidth + defaultPadding);

        const diff = Math.abs(output - availableWidth);
        if (diff < bestDiff || (diff === bestDiff && output <= availableWidth)) {
          bestDiff = diff;
          bestWidth = testWidth;
          // If we found exact match or slight underflow, stop
          if (output === availableWidth || (output < availableWidth && diff <= 1)) {
            break;
          }
        }

        // If we're starting to overflow significantly, stop
        if (output > availableWidth + 5) break;
      }

      return Math.max(10, bestWidth);
    }

    // For all other cases (explicit barSize and/or padding), just pass availableWidth
    return Math.max(10, availableWidth);
  }
}
