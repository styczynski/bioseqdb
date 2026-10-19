#!/usr/bin/env python3
"""Render tutorial charts from the measured UCSC SQL snapshot.

Requires Matplotlib (3.10 used for the checked-in figures). No network requests.
Run from this directory:
    python render_charts.py --output ../../figures/blog/seqkit-workflows
The SVGs are transparent and switch palettes with the embedding page's
color-scheme. The measured GC denominator is A+C+G+T; N remains in lengths.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter, MultipleLocator

ROOT = Path(__file__).resolve().parent
DARK = {
    'text': '#eef4f9', 'muted': '#a9bccb', 'grid': '#28425b',
    'human': '#74c8a8', 'mouse': '#65b9dd', 'zebrafish': '#f4a467',
    'A': '#74c8a8', 'C': '#65b9dd', 'G': '#f4a467',
    'T': '#b4a0e5', 'N': '#94a9ba', 'bar_text': '#071320',
}
LIGHT = {
    '#eef4f9': '#173247', '#a9bccb': '#405c72', '#28425b': '#ccd9e4',
    '#74c8a8': '#167251', '#65b9dd': '#146c91', '#f4a467': '#a54a12',
    '#b4a0e5': '#7552a8', '#94a9ba': '#526a80', '#071320': '#ffffff',
}
LABELS = {'human': 'Human (hg38)', 'mouse': 'Mouse (mm39)',
          'zebrafish': 'Zebrafish (danRer11)'}
plt.rcParams.update({
    'font.family': 'DejaVu Sans', 'font.size': 14,
    'text.color': DARK['text'], 'axes.labelcolor': DARK['text'],
    'xtick.color': DARK['muted'], 'ytick.color': DARK['muted'],
    'axes.edgecolor': DARK['grid'], 'axes.spines.top': False,
    'axes.spines.right': False, 'axes.spines.left': False,
    'axes.spines.bottom': False, 'grid.color': DARK['grid'],
    'svg.fonttype': 'none', 'svg.hashsalt': 'bioseqdb-seqkit-ucsc-chrm',
    'savefig.transparent': True,
})


def prepare(width=12, height=6):
    fig, ax = plt.subplots(figsize=(width, height), dpi=72)
    fig.patch.set_alpha(0)
    ax.patch.set_alpha(0)
    ax.set_axisbelow(True)
    ax.tick_params(length=0, pad=9)
    return fig, ax


def heading(fig, title, subtitle):
    fig.text(.04, .94, title, ha='left', va='top', fontsize=21, weight='bold')
    fig.text(.04, .86, subtitle, ha='left', va='top', fontsize=13,
             color=DARK['muted'])


def write_svg(fig, path, label, description):
    fig.savefig(path, format='svg', metadata={'Date': None, 'Creator': 'BioseqDB render_charts.py'})
    plt.close(fig)
    source = path.read_text().replace("font-family: 'DejaVu Sans'", "font-family: Arial, sans-serif")
    source = re.sub(r'width="([0-9.]+)pt" height="([0-9.]+)pt"', r'width="\1" height="\2"', source, count=1)
    source = source.replace('<svg ', '<svg role="img" aria-labelledby="chart-title chart-description" ', 1)
    for dark in LIGHT:
        source = source.replace('fill: '+dark, 'fill: var(--theme-'+dark[1:]+', '+dark+')')
        source = source.replace('stroke: '+dark, 'stroke: var(--theme-'+dark[1:]+', '+dark+')')
    style = '<style>@media(prefers-color-scheme:light){:root{' + ''.join(
        '--theme-'+dark[1:]+':'+light+';' for dark, light in LIGHT.items()) + '}}</style>'
    from html import escape
    first_end = source.index('>', source.index('<svg ')) + 1
    source = source[:first_end] + '\n<title id="chart-title">'+escape(label)+'</title>\n<desc id="chart-description">'+escape(description)+'</desc>\n'+style + source[first_end:]
    path.write_text(source)
    parsed = ET.fromstring(source)
    assert parsed.attrib.get('viewBox')


def render(measurements, output):
    output.mkdir(parents=True, exist_ok=True)
    genomes = measurements['genomes']
    fig, ax = prepare(height=5.7)
    fig.subplots_adjust(left=.25, right=.86, bottom=.20, top=.72)
    heading(fig, 'Three mitochondrial chromosomes from UCSC',
            'The bars cover chrM, not a species\' whole nuclear genome.')
    for i, row in enumerate(genomes):
        ax.barh(i, row['length'], height=.48, color=DARK[row['species']])
        ax.text(row['length']+230, i, f"{row['length']:,} bp", va='center', fontsize=15)
    ax.set_yticks(range(3), [LABELS[r['species']] for r in genomes])
    ax.invert_yaxis()
    ax.set_xlim(0, 18100)
    ax.xaxis.set_major_locator(MultipleLocator(5000))
    ax.xaxis.set_major_formatter(FuncFormatter(lambda x, _: f'{x:,.0f}'))
    ax.grid(axis='x', alpha=.75)
    ax.set_xlabel('Sequence length (bp)', labelpad=15)
    write_svg(fig, output/'chromosome-lengths.svg', 'UCSC mitochondrial chromosome lengths',
              'Human hg38:16,569 bases. Mouse mm39:16,299 bases. Zebrafish danRer11:16,596 bases. All bars start at zero.')

    fig, ax = prepare(height=6.3)
    fig.subplots_adjust(left=.25, right=.95, bottom=.22, top=.65)
    heading(fig, 'A, C, G and T do not occur equally',
            'Counts in the same three mitochondrial records; no bases removed.')
    for i, row in enumerate(genomes):
        left = 0
        for base in 'ACGTN':
            count = row['counts'][base]
            ax.barh(i, count, left=left, height=.55, color=DARK[base],
                    label=base if i == 0 else None)
            if count >= 500:
                ax.text(left+count/2, i, f'{count:,}', ha='center', va='center',
                        color=DARK['bar_text'], fontsize=12, weight='bold')
            left += count
    ax.set_yticks(range(3), [LABELS[r['species']] for r in genomes])
    ax.invert_yaxis()
    ax.set_xlim(0, 17000)
    ax.xaxis.set_major_locator(MultipleLocator(5000))
    ax.xaxis.set_major_formatter(FuncFormatter(lambda x, _: f'{x:,.0f}'))
    ax.grid(axis='x', alpha=.75)
    ax.set_xlabel('Number of bases', labelpad=13)
    legend = ax.legend(ncol=5, loc='lower left', bbox_to_anchor=(0,1.04),
                       frameon=False, handlelength=1, columnspacing=1.35)
    for text in legend.get_texts():
        text.set_color(DARK['text'])
    fig.text(.04, .055, 'hg38 has 1 unresolved N. Its segment is too small to see at this scale.',
             fontsize=12, color=DARK['muted'])
    write_svg(fig, output/'base-composition.svg', 'Base composition of UCSC mitochondrial sequences',
              'Stacked counts of A, C, G, T and N. The single N in hg38 is included but too small to resolve visually. Exact counts are printed inside the A/C/G/T segments.')

    fig, ax = prepare(height=6.8)
    fig.subplots_adjust(left=.10, right=.95, bottom=.24, top=.66)
    heading(fig, 'GC content along chrM',
            'One point per window: 1,000 bases, except each shorter final window.')
    for species, marker in [('human','o'), ('mouse','s'), ('zebrafish','^')]:
        rows = [w for w in measurements['windows'] if w['species'] == species]
        ax.plot([(r['start0']+r['end0'])/2000 for r in rows],
                [r['gc_percent'] for r in rows], color=DARK[species], marker=marker,
                markersize=5.5, linewidth=2.1, label=LABELS[species])
        last = rows[-1]
        ax.plot([(last['start0']+last['end0'])/2000], [last['gc_percent']],
                marker=marker, markersize=10, markerfacecolor='none',
                markeredgecolor=DARK[species], markeredgewidth=1.5)
    ax.set_xlim(0,17)
    ax.set_ylim(20,55)
    ax.xaxis.set_major_locator(MultipleLocator(2))
    ax.yaxis.set_major_locator(MultipleLocator(10))
    ax.grid(alpha=.75)
    ax.set_xlabel('Window midpoint on its own chromosome (kb)', labelpad=14)
    ax.set_ylabel('GC (%)', labelpad=14)
    legend = ax.legend(ncol=3, loc='lower left', bbox_to_anchor=(0,1.06),
                       frameon=False, handlelength=1.7, columnspacing=1.1, fontsize=12)
    for text in legend.get_texts():
        text.set_color(DARK['text'])
    fig.text(.04, .105, 'Final windows: human 569 bp, mouse 299 bp, zebrafish 596 bp (larger markers).',
             fontsize=12, color=DARK['muted'])
    fig.text(.04, .055, 'GC = 100 x (G + C) / (A + C + G + T). Positions are not aligned between species.',
             fontsize=12, color=DARK['muted'])
    write_svg(fig, output/'gc-windows.svg', 'GC content in mitochondrial chromosome windows',
              'Seventeen windows for each species, plotted at their physical midpoint. Final windows have 569,299 and596 bases. GC excludes N. Lines connect neighboring summaries, not aligned positions across species.')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--measurements',type=Path,default=ROOT/'measurements.json')
    parser.add_argument('--output',type=Path,default=ROOT.parent.parent/'figures/blog/seqkit-workflows')
    args=parser.parse_args()
    measurements=json.loads(args.measurements.read_text())
    for row in measurements['genomes']+measurements['windows']:
        assert sum(row['counts'].values()) == row['length']
        canonical=sum(row['counts'][b] for b in 'ACGT')
        gc=100*(row['counts']['G']+row['counts']['C'])/canonical
        assert abs(gc-row['gc_percent'])<.000051
    render(measurements,args.output)
    print('Rendered chromosome-lengths.svg, base-composition.svg and gc-windows.svg')

if __name__=='__main__':
    main()
