#!/usr/bin/env python3
"""Rebuild the Python-aligner article's charts from its auditable CSV sources.

Requires matplotlib. These are historical measurements, not new benchmark runs.
The course PDF plots were raster images: those values remain explicitly approximate.
"""
from __future__ import annotations

import csv
import html
import io
import json
import math
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator, FuncFormatter

from theme_palette import theme_svg

ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / 'website/blog/data/python-aligner'
OUT = ROOT / 'website/static/figures/blog/python-aligner'
COLORS = {'text': '#eef4f9', 'muted': '#a9bccb', 'grid': '#20364a',
          'cyan': '#65b9dd', 'orange': '#f4894f', 'mint': '#74c8a8',
          'purple': '#c5a8d6'}
plt.rcParams.update({'font.family': 'DejaVu Sans', 'font.size': 15,
                     'text.color': COLORS['text'], 'axes.labelcolor': COLORS['muted'],
                     'xtick.color': COLORS['muted'], 'ytick.color': COLORS['text'],
                     'axes.edgecolor': COLORS['grid'], 'svg.fonttype': 'none',
                     'svg.hashsalt': 'bioseq-python-aligner', 'savefig.transparent': True})


def rows(filename: str) -> list[dict[str, str]]:
    with (DATA / filename).open(newline='') as f:
        return list(csv.DictReader(f))


PROFILES = {(r['variant'], r['dataset'], r['metric']): r for r in rows('local-profiles.csv')}


def metric(variant: str, case: str, name: str, field: str = 'total') -> float:
    return float(PROFILES[(variant, case, name)][field])


def panel(ax):
    ax.set_facecolor('none')
    ax.set_axisbelow(True)
    for spine in ax.spines.values():
        spine.set_visible(False)
    ax.tick_params(axis='both', length=0, pad=9)
    ax.grid(axis='x', color=COLORS['grid'], linewidth=1)


def save(fig, filename: str, title: str, description: str):
    stream = io.StringIO()
    fig.savefig(stream, format='svg', metadata={'Date': None, 'Creator': 'BioseqDB chart generator'})
    content = stream.getvalue().replace("'DejaVu Sans'", "Arial, sans-serif")
    i = content.index('>', content.index('<svg'))
    content = content[:i] + ' role="img" aria-labelledby="figure-title figure-desc"' + content[i:]
    i = content.index('>', content.index('<svg')) + 1
    content = content[:i] + f'\n<title id="figure-title">{html.escape(title)}</title>\n<desc id="figure-desc">{html.escape(description)}</desc>\n' + content[i:]
    (OUT / filename).write_text(theme_svg(content))
    plt.close(fig)


def course_runtime():
    records = rows('course-runtime.csv')
    source = json.loads((DATA / 'sources.json').read_text())['course_results']
    order = source['eligible_submission_codes']
    for error, rate in [('a', '5%'), ('b', '10%')]:
        fig, axes = plt.subplots(3, 1, figsize=(6.4, 15.1))
        fig.subplots_adjust(left=.29, right=.83, top=.92, bottom=.052, hspace=.43)
        fig.text(.05, .975, f'Eligible submissions · {rate} read errors', size=19, weight='bold', va='top')
        fig.text(.05, .947, 'Minutes · log scale · approximate PDF values', size=13, color=COLORS['muted'])
        details=[]
        for ax, genome in zip(axes, ['1M', '5M', '20M']):
            case=f'{genome}/{error}'
            values={r['student_code']:float(r['runtime_minutes_approx']) for r in records if r['dataset']==case}
            panel(ax)
            ax.set_xscale('log');ax.set_xlim(.05, 150)
            ax.set_ylim(len(order)-.4, -.7)
            ax.xaxis.set_major_locator(FixedLocator([.1, 1, 10, 100]))
            ax.xaxis.set_major_formatter(FuncFormatter(lambda x, _:f'{x:g}'))
            ax.minorticks_off()
            ax.set_yticks(range(len(order)), order, fontsize=17)
            ax.tick_params(axis='x', labelsize=14)
            ax.set_title(f'{genome} bases · 1,000 reads', loc='left', pad=11, size=16, weight='bold', color=COLORS['text'])
            for i,code in enumerate(order):
                val=values[code]
                col=COLORS['orange' if code=='PS' else 'muted']
                ax.plot([.05,val],[i,i], color=col, linewidth=2, alpha=.45)
                ax.scatter([val],[i],color=col,s=75 if code=='PS' else 40,zorder=3)
                ax.text(1.045,i,f'~{val:g}',transform=ax.get_yaxis_transform(),va='center',size=16,color=col)
                details.append(f'{case} {code}: approximately {val:g} minutes')
        fig.text(.05,.014,'PS = this prototype. Eligibility assessed by the author.',size=12,color=COLORS['muted'])
        save(fig,f'course-runtime-{error}.svg',f'Eligible submission runtimes for {rate} error reads',
             'Historical course report, page 7. '+source['comparison_scope']+' Runtime values digitized from a raster plot and rounded; close rankings are uncertain. '+ '; '.join(details)+'.')


def course_quality():
    records=rows('course-ps-quality.csv')
    fig,ax=plt.subplots(figsize=(7,5.8));fig.subplots_adjust(left=.17,right=.92,top=.73,bottom=.18)
    panel(ax)
    cases=[r['dataset'] for r in records]
    vals=[int(r['incorrect_reads']) for r in records]
    ax.barh(range(6),vals,color=COLORS['orange'],height=.53)
    ax.set_yticks(range(6),cases);ax.invert_yaxis();ax.set_xlim(0,5)
    ax.xaxis.set_major_locator(FixedLocator(range(6)))
    for y,v in enumerate(vals):ax.text(v+.12,y,str(v),va='center',size=17,color=COLORS['orange'])
    ax.set_xlabel('Incorrect mappings / 1,000 reads',labelpad=14,size=14)
    fig.text(.05,.955,'PS mapped every read',size=21,weight='bold',va='top')
    fig.text(.05,.875,'0 unmapped · 8 incorrect out of 6,000',size=16,color=COLORS['mint'])
    fig.text(.05,.81,'Course tests: a ≈ 5% errors; b ≈ 10% errors.',size=13,color=COLORS['muted'])
    save(fig,'course-ps-quality.svg','PS mapping completeness and incorrect mappings',
         'The course report pages 3 through 6 show zero unmapped PS reads in every test. Incorrect mappings for 1M/a, 5M/a, 20M/a, 1M/b, 5M/b, 20M/b are 1, 3, 0, 0, 4, 0 respectively, with 1,000 reads per test. These are course results, separate from the development-slide checks.')


def course_memory():
    records=rows('course-ps-memory.csv')
    fig,ax=plt.subplots(figsize=(7,5.6));fig.subplots_adjust(left=.17,right=.85,top=.75,bottom=.17)
    panel(ax);vals=[float(r['memory_mb_approx']) for r in records]
    ax.barh(range(6),vals,color=COLORS['cyan'],height=.53)
    ax.set_yticks(range(6),[r['dataset'] for r in records]);ax.invert_yaxis()
    ax.set_xlim(0,1000);ax.xaxis.set_major_locator(FixedLocator([0,250,500,750,1000]));ax.tick_params(axis='x',labelsize=12)
    for y,v in enumerate(vals):ax.text(v+18,y,f'~{v:g}',va='center',size=15,color=COLORS['cyan'])
    ax.set_xlabel('Memory (MB)',labelpad=14,size=14)
    fig.text(.05,.955,'PS memory grew with the reference',size=19,weight='bold',va='top')
    fig.text(.05,.875,'Approximate values from the course PDF.',size=14,color=COLORS['muted'])
    fig.text(.05,.82,'The assignment allowed less than 1 GB.',size=14,color=COLORS['muted'])
    save(fig,'course-ps-memory.svg','PS memory use in the course report',
         'Approximate memory digitized from the raster plot on page 8. Both error levels use about 92 MB for 1M reference bases, 280 MB for 5M and 900 MB for 20M. The source does not provide original memory logs or specify its peak-memory measurement method.')


def local_bwt():
    fig,axes=plt.subplots(2,1,figsize=(7,7.1));fig.subplots_adjust(left=.32,right=.85,top=.76,bottom=.13,hspace=.9)
    variants=[('solution','Naive'),('bwt-dp-lists','Skew / lists'),('bwt-dp-numpy','Skew / NumPy')]
    details=[]
    for ax,case in zip(axes,['20Ma','20Mb']):
        panel(ax);vals=[metric(v,case,'read_bwt','mean') for v,_ in variants]
        ax.barh(range(3),vals,color=[COLORS['mint'],COLORS['cyan'],COLORS['orange']],height=.56)
        ax.set_yticks(range(3),[label for _,label in variants],size=14);ax.invert_yaxis();ax.set_xlim(0,12)
        ax.xaxis.set_major_locator(FixedLocator([0,4,8,12]));ax.tick_params(axis='x',labelsize=12)
        ax.set_title(case,loc='left',size=17,pad=13,color=COLORS['text'],weight='bold')
        for y,val in enumerate(vals):ax.text(val+.2,y,f'{val:.2f}',va='center',size=15)
        details.append(f'{case}: '+', '.join(f'{label} {val:.2f} ms' for (_,label),val in zip(variants,vals)))
    fig.text(.05,.965,'The small BWT routine',size=21,weight='bold',va='top')
    fig.text(.05,.90,'Mean milliseconds per recorded call',size=16,color=COLORS['muted'])
    fig.text(.05,.848,'Earlier v10 profiles · lower is faster',size=13,color=COLORS['muted'])
    fig.text(.05,.055,'20Ma: 1,000 calls. 20Mb: 1,001 calls.',size=13,color=COLORS['muted'])
    fig.text(.05,.02,'No repeated-run variance was recorded.',size=12,color=COLORS['muted'])
    save(fig,'local-bwt-implementations.svg','Local BWT implementation comparison',
         'Exact recorded mean timings from experiments_data.json, as compared in the project slides. '+ '; '.join(details)+'. These are calls within one historical profiling run, not repeated benchmark trials. The variants differ in their suffix-array implementation and representation.')


def local_dp():
    fig,axes=plt.subplots(2,1,figsize=(7,6.7));fig.subplots_adjust(left=.22,right=.84,top=.74,bottom=.12,hspace=1)
    details=[]
    for ax,case in zip(axes,['20Ma','20Mb']):
        panel(ax);vals=[metric(v,case,'read_dp')/1000 for v in ['solution','bwt-dp-numpy']]
        ax.barh(range(2),vals,color=[COLORS['mint'],COLORS['orange']],height=.5)
        ax.set_yticks([0,1],['Lists','NumPy'],size=16);ax.invert_yaxis();ax.set_xlim(0,4.2)
        ax.xaxis.set_major_locator(FixedLocator([0,1,2,3,4]));ax.tick_params(axis='x',labelsize=12)
        n=int(metric('solution',case,'read_dp','samples'))
        ax.set_title(f'{case} · {n} DP calls',loc='left',size=17,pad=14,color=COLORS['text'],weight='bold')
        for y,val in enumerate(vals):ax.text(val+.08,y,f'{val:.2f} s',va='center',size=15)
        details.append(f'{case}: lists {vals[0]:.5f} seconds, NumPy {vals[1]:.5f} seconds, {n} calls')
    fig.text(.05,.965,'NumPy did not help this DP loop',size=19,weight='bold',va='top')
    fig.text(.05,.89,'Total time in the fallback routine',size=16,color=COLORS['muted'])
    fig.text(.05,.83,'Earlier v10 profiles · seconds · lower is faster',size=13,color=COLORS['muted'])
    fig.text(.05,.035,'The sample is small; this is not a general NumPy ranking.',size=12,color=COLORS['muted'])
    save(fig,'local-dp-implementations.svg','Local dynamic-programming implementation comparison',
         'Exact total DP timings from experiments_data.json converted from milliseconds to seconds. '+ '; '.join(details)+'. Both implementations were called only a few times. The chart does not isolate allocation, copying, or scalar-operation costs.')


def local_phases():
    phases=[('ref_chunk_building','Reference index'),('read_index_building','Read indexes'),('read_lis','LIS routine'),('read_lis_cutoff','Candidate scoring'),('read_bwt','BWT ends'),('read_dp','DP fallback')]
    fig,axes=plt.subplots(2,1,figsize=(7,9.0));fig.subplots_adjust(left=.35,right=.84,top=.79,bottom=.11,hspace=.56)
    details=[]
    for ax,case in zip(axes,['20Ma','20Mb']):
        panel(ax);vals=[metric('solution',case,k)/1000 for k,_ in phases]
        ax.barh(range(6),vals,color=[COLORS['cyan']]+[COLORS['mint']]*5,height=.56)
        ax.set_yticks(range(6),[label for _,label in phases],size=13);ax.invert_yaxis();ax.set_xlim(0,23)
        ax.xaxis.set_major_locator(FixedLocator([0,10,20]));ax.tick_params(axis='x',labelsize=12)
        total=metric('solution',case,'total_execution')/1000
        ax.set_title(f'{case} · {total:.2f} s whole run',loc='left',size=15,pad=14,color=COLORS['text'],weight='bold')
        for y,val in enumerate(vals):ax.text(val+.4,y,f'{val:.2f}',va='center',size=13)
        details.append(case+': '+', '.join(f'{label} {val:.5f} seconds' for (_,label),val in zip(phases,vals)))
    fig.text(.05,.965,'Where the local runtime went',size=21,weight='bold',va='top')
    fig.text(.05,.907,'Seconds spent in each recorded phase',size=15,color=COLORS['muted'])
    fig.text(.05,.862,'Independent timers; not a complete partition.',size=13,color=COLORS['muted'])
    fig.text(.05,.035,'Earlier v10 profiles; separate from the course measurements.',size=12,color=COLORS['muted'])
    save(fig,'local-runtime-phases.svg','Local recorded runtime phases',
         'Exact phase totals from the solution variant of experiments_data.json. '+ '; '.join(details)+'. Phase bars are shown independently. They do not sum to the complete runtime and should not be treated as a full accounting of time.')


def concept(name: str, width: int, height: int, title: str, desc: str, body: str):
    svg = f'''<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" role="img" aria-labelledby="title desc">
<title id="title">{html.escape(title)}</title><desc id="desc">{html.escape(desc)}</desc>
<defs><marker id="arrow" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path d="M0 0 L10 5 L0 10" fill="#65b9dd"/></marker></defs>
<g font-family="Arial, sans-serif" fill="#eef4f9">{body}</g></svg>'''
    (OUT / name).write_text(theme_svg(svg))


def conceptual_figures():
    # Teaching examples use ordinary start coordinates and a clean two-bit code.
    body='<text x="24" y="42" font-size="29" font-weight="bold">An alignment makes edits visible</text>'
    body+='<text x="24" y="81" font-size="22" fill="#a9bccb">Gaps keep the surrounding letters lined up.</text>'
    seqs=[('Reference','ACTG-ACGT',154),('Read','ACTATAC-T',220)]
    accents={3:'#f4894f',4:'#65b9dd',7:'#c5a8d6'}
    for label,seq,y in seqs:
        body+=f'<text x="24" y="{y+8}" font-size="21">{label}</text>'
        for i,ch in enumerate(seq):
            x=153+i*47;col=accents.get(i,'#74c8a8')
            body+=f'<rect x="{x}" y="{y-27}" width="40" height="46" rx="7" fill="{col}" fill-opacity=".12" stroke="{col}"/><text x="{x+20}" y="{y+6}" text-anchor="middle" font-family="monospace" font-size="28" fill="{col}">{ch}</text>'
    for x,y,col,label in [(24,291,'#f4894f','Substitution'),(235,291,'#65b9dd','Insertion'),(424,291,'#c5a8d6','Deletion')]:
        body+=f'<circle cx="{x+7}" cy="{y-7}" r="6" fill="{col}"/><text x="{x+22}" y="{y}" font-size="23" fill="{col}">{label}</text>'
    body+='<text x="24" y="340" font-size="22" fill="#a9bccb">The mapper locates the interval containing this match.</text>'
    concept('alignment-edits.svg',610,370,'Aligned letters reveal substitutions, insertions and deletions','Reference ACTGACGT and read ACTATACT are aligned using one substitution, one insertion and one deletion. The inserted T aligns with a reference gap, and the deleted G aligns with a read gap. This schematic is separate from the executable mapping fixture.',body)

    body='<text x="24" y="43" font-size="29" font-weight="bold">The prototype, one read at a time</text>'
    steps=[('1','Index the reference','Process chunks and collect minimizer positions.'),('2','Sketch the read','Look up the same seed keys in the reference.'),('3','Build anchors','Pair reference and read positions for each key.'),('4','Find candidate regions','Heuristic LIS, gap scores, a short candidate list.'),('5','Refine the two ends','Try short BWT searches; use DP when needed.'),('6','Report coordinates','Return the selected interval, not a CIGAR string.')]
    for i,(n,title,desc) in enumerate(steps):
        y=76+i*116
        body+=f'<rect x="24" y="{y}" width="562" height="92" rx="15" fill="#0a1826" stroke="#20364a" stroke-width="2"/><circle cx="61" cy="{y+34}" r="18" fill="#65b9dd" fill-opacity=".15"/><text x="61" y="{y+42}" text-anchor="middle" font-size="23" fill="#65b9dd">{n}</text><text x="99" y="{y+37}" font-size="25" font-weight="bold">{title}</text><text x="45" y="{y+72}" font-size="21" fill="#a9bccb">{desc}</text>'
        if i<5:body+=f'<path d="M305 {y+94} V{y+109}" stroke="#65b9dd" stroke-width="2" marker-end="url(#arrow)"/>'
    concept('mapper-pipeline.svg',610,780,'The Python prototype mapping pipeline','Six stages: index reference chunks, sketch the read, build anchors from shared minimizer keys, shortlist candidate regions with the heuristic LIS and gap scoring, refine the ends with BWT and sometimes dynamic programming, and report a selected coordinate interval.',body)

    body='<text x="24" y="43" font-size="29" font-weight="bold">A minimum in each sliding window</text><text x="24" y="85" font-size="24" fill="#a9bccb">ACGTAC · k = 3 · w = 2 k-mers</text><text x="24" y="123" font-size="21" fill="#a9bccb">A=0, C=1, G=2, T=3 · positions start at zero</text>'
    kmers=[('ACG',6,0),('CGT',27,1),('GTA',44,2),('TAC',49,3)]
    for row in range(3):
        y=160+row*116
        body+=f'<text x="24" y="{y+37}" font-size="22">Window {row+1}</text>'
        for j in range(2):
            seq,key,pos=kmers[row+j];x=169+j*135;col='#65b9dd' if j==0 else '#a9bccb'
            body+=f'<rect x="{x}" y="{y}" width="117" height="76" rx="10" fill="{col}" fill-opacity=".12" stroke="{col}"/><text x="{x+58}" y="{y+30}" text-anchor="middle" font-family="monospace" font-size="25" fill="{col}">{seq}</text><text x="{x+58}" y="{y+60}" text-anchor="middle" font-size="21" fill="{col}">key {key}</text>'
        body+=f'<path d="M437 {y+36} H471" stroke="#65b9dd" stroke-width="2" marker-end="url(#arrow)"/><text x="491" y="{y+30}" font-size="23" fill="#65b9dd">keep {kmers[row][1]}</text><text x="491" y="{y+60}" font-size="21" fill="#a9bccb">at {row}</text>'
    body+='<text x="24" y="555" font-size="22" fill="#a9bccb">One selected position may belong to several windows.</text>'
    concept('minimizer-windows.svg',610,588,'Three sliding windows select minimizers in ACGTAC','Runnable teaching example with k=3 and w=2. Two-bit k-mer keys and start positions are ACG=6 at0, CGT=27 at1, GTA=44 at2, TAC=49 at3. Consecutive two-k-mer windows select keys6,27,44 at positions0,1,2. Ties choose the leftmost occurrence.',body)

    body='<text x="24" y="43" font-size="29" font-weight="bold">Increasing anchors can span two places</text><text x="24" y="84" font-size="22" fill="#a9bccb">Order alone does not identify a good local match.</text>'
    body+='<path d="M85 405 H571 M85 405 V120" stroke="#a9bccb" stroke-width="2"/><text x="313" y="455" text-anchor="middle" font-size="23" fill="#a9bccb">Reference position →</text><text x="24" y="115" font-size="22" fill="#a9bccb">Read</text><text x="24" y="144" font-size="22" fill="#a9bccb">pos.</text>'
    pts=[(111,374),(138,354),(165,334),(359,274),(392,247),(425,218),(458,191),(491,160)]
    for a,b in zip(pts,pts[1:]):body+=f'<path d="M{a[0]} {a[1]} L{b[0]} {b[1]}" stroke="#65b9dd" stroke-width="3" stroke-dasharray="7 6"/>'
    for x,y in pts:body+=f'<circle cx="{x}" cy="{y}" r="7" fill="#65b9dd"/>'
    body+='<rect x="335" y="132" width="180" height="168" rx="18" fill="#f4894f" fill-opacity=".09" stroke="#f4894f" stroke-width="2"/><text x="424" y="329" text-anchor="middle" font-size="23" fill="#f4894f">local candidate</text><text x="237" y="263" text-anchor="middle" font-size="22" fill="#a9bccb">large gap</text><path d="M236 274 V310" stroke="#a9bccb" stroke-width="2"/>'
    body+='<text x="24" y="507" font-size="23" fill="#a9bccb">The prototype limits gaps and scores local windows.</text><text x="24" y="542" font-size="21" fill="#a9bccb">This is a heuristic, not an optimal-chain guarantee.</text>'
    concept('anchors-and-locality.svg',610,574,'A monotone anchor chain can include a distant tail','Schematic reference-versus-read anchor scatter plot. An increasing chain joins three early anchors to a separate dense local group across a large reference gap. The prototype uses a distance cutoff and local-window scores instead of assuming the longest increasing chain is automatically one good mapping. This is a conceptual example, not a measured read.',body)

    body='<text x="24" y="43" font-size="29" font-weight="bold">More span is not always a better window</text><text x="24" y="84" font-size="23" fill="#a9bccb">Teaching score · read length 100 · k = 17</text>'
    for row,(name,ref,read,score,col) in enumerate([('Dense',[100,110,120,130],[0,10,20,30],'.300','#74c8a8'),('Sparse',[100,130,160,190],[0,10,20,30],'.105','#f4894f')]):
        y=131+row*203
        body+=f'<rect x="24" y="{y}" width="562" height="171" rx="15" fill="#0a1826" stroke="#20364a"/><text x="43" y="{y+35}" font-size="25" font-weight="bold" fill="{col}">{name}</text><text x="557" y="{y+35}" text-anchor="end" font-size="24" fill="{col}">score {score}</text>'
        body+=f'<text x="43" y="{y+78}" font-size="21" fill="#a9bccb">Ref.</text><text x="43" y="{y+131}" font-size="21" fill="#a9bccb">Read</text>'
        for i,(r,q) in enumerate(zip(ref,read)):
            x=172+i*110
            body+=f'<text x="{x}" y="{y+78}" text-anchor="middle" font-family="monospace" font-size="24">{r}</text><text x="{x}" y="{y+131}" text-anchor="middle" font-family="monospace" font-size="24">{q}</text><path d="M{x} {y+89} V{y+104}" stroke="{col}" stroke-width="2"/>'
    body+='<text x="24" y="554" font-size="22" fill="#a9bccb">The sparse chain pays for uncovered reference gaps.</text>'
    concept('candidate-gap-score.svg',610,588,'Dense versus sparse candidate anchor scores','Two executable teaching fixtures for the final mapper score formula. Dense anchors (100,0),(110,10),(120,20),(130,30) score0.300. Sparse anchors (100,0),(130,10),(160,20),(190,30) score0.105. Both have a read span of30; only the second has uncovered reference gaps beyond k=17. Read length is100. Coordinates are illustrative.',body)


def rolling_kmer_figure():
    body='<text x="24" y="43" font-size="29" font-weight="bold">Roll the key, keep the last k bases</text><text x="24" y="84" font-size="23" fill="#a9bccb">k = 3 · A=0, C=1, G=2, T=3</text>'
    steps=[('ACG','00 01 10','6','start 0'),('Shift left, append T','00 01 10 11','27','T = 11'),('Mask to six bits','01 10 11','27','CGT · start 1')]
    for i,(label,bits,value,note) in enumerate(steps):
        y=121+i*131
        body+=f'<rect x="24" y="{y}" width="562" height="105" rx="14" fill="#0a1826" stroke="#20364a" stroke-width="2"/><text x="43" y="{y+33}" font-size="23" font-weight="bold">{label}</text><text x="557" y="{y+33}" text-anchor="end" font-size="21" fill="#a9bccb">{note}</text><text x="43" y="{y+79}" font-family="monospace" font-size="31" fill="#65b9dd">{bits}</text><text x="557" y="{y+79}" text-anchor="end" font-size="25" fill="#74c8a8">= {value}</text>'
        if i<2:body+=f'<path d="M305 {y+108} V{y+123}" stroke="#65b9dd" stroke-width="2" marker-end="url(#arrow)"/>'
    body+='<text x="24" y="558" font-family="monospace" font-size="23" fill="#a9bccb">((6 &lt;&lt; 2) | 3) &amp; 63 = 27</text><text x="24" y="600" font-size="22" fill="#a9bccb">With fixed k, leading zero bits are unambiguous.</text>'
    concept('rolling-kmers.svg',610,631,'A rolling two-bit k-mer key','Teaching code A0,C1,G2,T3. ACG at start position0 has bits00 01 10, integer6. Shift6 left two bits, append T code3 and mask with six one bits63; the retained CGT at start position1 is01 10 11, integer27. Since the outgoing A has codezero, the mask changes no one bits in this particular step.',body)


def main():
    OUT.mkdir(parents=True,exist_ok=True)
    course_runtime();course_quality();course_memory();local_bwt();local_dp();local_phases();conceptual_figures();rolling_kmer_figure()
    measurements = {'sources': json.loads((DATA / 'sources.json').read_text()), 'course_runtime': rows('course-runtime.csv'), 'course_ps_quality': rows('course-ps-quality.csv'), 'course_ps_memory': rows('course-ps-memory.csv'), 'local_profiles': rows('local-profiles.csv')}
    (OUT / 'measurements.json').write_text(json.dumps(measurements, indent=2, ensure_ascii=False) + '\n')
    print(f'Generated 7 historical-result charts and 6 conceptual figures in {OUT.relative_to(ROOT)}')


if __name__ == '__main__':
    main()
