import matplotlib
import matplotlib.axes
import matplotlib.figure
import matplotlib.pyplot as plt
import matplotlib.transforms as transforms
from matplotlib.ticker import Formatter, ScalarFormatter, AutoLocator, LogLocator
import pandas
import itertools
import numpy as np
import math
import re
import Bio
from Bio import AlignIO, SeqRecord
from Bio.AlignIO import MultipleSeqAlignment
from pathlib import Path

#colors = [
#    (1.0, 0.0, 0.0), #r
#    (0.0, 1.0, 0.0), #g
#    (0.0, 0.0, 1.0), #b
#    (0.7, 0.5, 0.0), #yellow
#    (0.0, 0.8, 0.8), #turquoise
#    (1.0, 0.0, 1.0), #purple
#    (0.5, 0.0, 0.0), #dark r
#    (0.0, 0.5, 0.0), #dark g
#    (0.0, 0.0, 0.5), #dark b
#]

colors = [
    (0.0, 0.0, 0.0), #black
    (0.4, 0.4, 0.4), #gray
    (0.1, 0.5, 0.1), #dark g
    (0.2, 0.8, 0.2), #g
    (0.5, 0.1, 0.5), #r 
    (1.0, 0.2, 1.0), #purple
    (0.1, 0.1, 0.5), #dark b
    (0.2, 0.2, 1.0), #b
    (0.7, 0.5, 0.2), #yellow
    (0.2, 0.8, 0.8), #turquoise
    #(0.8, 0.4, 0.3), #dark r
]

#ours_color =  [0.2, 0.8, 0.4]
#ours_color = (49/255,163/255,84/255)
ours_color = [0.0, 0.6, 1]
ours_improvement_color = ours_color
baseline_color = [0.9, 0.9, 0.9]
baseline2_color = [0.8, 0.8, 0.8]
sr_name = 'Short Reads'
lr_name = 'Long Reads'
lrgt_name = 'Long Reads Groundtruth'

markers = [
    'o',
    '*',
    'd',
    'D',
    '^',
    'v',
    '<',
    '>'
]

def prepare_plot(fig, ax, ylines, left, right, have_labels=True, yminor=None, yticks_fontsize=None):
    if have_labels:
        labels = [(f"{y:,}") if (y < 100000) else (f'{y:.1e}') for y in ylines]
        ticks = [y for y in ylines]
        ax.set_yticks(ticks=ticks, labels=labels, size=yticks_fontsize)
    else:
        labels = []
        ticks = []

    if yminor is not None:
        ax.set_yticks(ticks=yminor, minor=True)
    ax.grid(visible=True, axis='both')
    ax.label_outer()
    width = right - left
    ax.set_xlim([left - width*0.05, right + width*0.05])

def corner_box_text(ax, corner_x:int, corner_y:int, text:str, pad=5.0, draw_box=True, **params):
    xy_axesfrac = corner_x, corner_y
    xytext_offset = (-pad if corner_x else pad, -pad if corner_y else pad)
    ha = 'right' if corner_x else 'left'
    va = 'top' if corner_y else 'bottom'

    ax.annotate(text, xy=xy_axesfrac, xytext=xytext_offset,
        xycoords='axes fraction', textcoords='offset points',
        bbox=dict(facecolor='white', alpha=1.0 if draw_box else 0, pad=pad),
        horizontalalignment=ha, verticalalignment=va,
        zorder=100,
        **params)

def centered_figlegend(fig, ax, handles_labels=None, borderpad=0.0, borderaxespad=0.4, **leg_args):
    handles_labels = handles_labels or ax.get_legend_handles_labels()
    fig.legend(*handles_labels,
        bbox_to_anchor=(0.5, 1),
        bbox_transform=transforms.blended_transform_factory(fig.transFigure, ax.transAxes),
        loc='lower center',
        borderpad=borderpad,
        borderaxespad=borderaxespad,
        **leg_args)

def plot_cpu_threads_WO_paper(threads_path, WO_path, output_path):    
    plt.rcParams['text.usetex'] = True
    boldfont = r'\fontfamily{qtm}\selectfont'
    regularfont = r'\fontfamily{cmr}\selectfont'

    def c(r255,g255,b255):
        return tuple((component/255 for component in (r255,g255,b255)))

    plot_labels = {
        (0, 0, 0): 'GenASM (CPU)',
        (0, 0, 1): 'Scrooge ET (CPU)',
        (1, 0, 0): 'Scrooge SENE (CPU)',
        (1, 0, 1): 'Scrooge SENE+ET (CPU)'
    }
    markers = {
        (0, 0, 0): 'X',
        (0, 0, 1): '^',
        (1, 0, 0): 'v',
        (1, 0, 1): 'o'
    }
    colors = {
        (0, 0, 0): c(0, 0, 0),
        (0, 0, 1): c(217,95,2),
        (1, 0, 0): c(27,158,119), #c(117,112,179),
        (1, 0, 1): ours_color#c(27,158,119)
    }

    datas = []
    for path in [threads_path, WO_path]:
        datas.append(pandas.read_csv(path,
            usecols=['W', 'O', 'SENE', 'DENT', 'early termination', 'threads', 'aligns/second'],
            dtype={'W' : int, 'O': int, 'SENE' : bool, 'DENT' : bool, 'early termination' : bool, 'threads': int, 'aligns/second': float}))
        datas[-1].rename(columns={'aligns/second':'throughput'}, inplace=True)
    datas[0].query(f'threads<=48', inplace=True)
    #datas[1].query(f'W<=128', inplace=True)
    x_keys = ['threads', 'W']
    x_labels = ['Threads', 'Window Size (W)']
    x_granularities = [8, 32]

    fig, axs = plt.subplots(1, 2, constrained_layout=True)
    y_granularity = 2000
    y_minor_granularity = 1000

    bottom, top = 0, int(math.ceil(max(datas[0]['throughput'] + datas[1]['throughput'])/y_granularity))*y_granularity
    for x_key, ax, data, x_granularity, x_label, name in zip(x_keys, axs, datas, x_granularities, x_labels, [rf'{boldfont}\textbf{{(a)}} {regularfont} CPU Scaling', rf'{boldfont}\textbf{{(b)}} {regularfont} CPU W Sensitivity']):
        left, right = min(data[x_key]), max(data[x_key])

        if ax==axs[0]:
            ax.set_ylabel(r"\textbf{Alignments per Second}", size=13, labelpad=1)
        else:
            ax.sharey(axs[0])

        ax.set_xticks(ticks=[left]+list(range(x_granularity, right+1, x_granularity)))
        ax.set_xticks(ticks=data[x_key].unique(), minor=True)
        ax.tick_params(axis='both', which='major', pad=1)

        ax.set_xlabel(f'\\textbf{{{x_label}}}', size=13, labelpad=1)

        corner_box_text(ax, 1, 1, name)
        prepare_plot(fig, ax, range(bottom, top, y_granularity), left, right, have_labels=ax==axs[0], yminor=range(bottom, top, y_minor_granularity))

        for  early_termination, sene, dent in itertools.product([False, True], [False, True], [False, True]):
            subdata=data.query(f'SENE=={sene}')
            subdata=subdata.query(f'DENT=={dent}')
            subdata=subdata.query(f'`early termination`=={early_termination}')
            subdata=subdata.groupby([x_key]).mean().reset_index()
            subdata=subdata.sort_values(by=[x_key])

            linewidth = 1.8
            markersize = 6
            linestyle= '-' #'-' if dp_mem_type=='global' else '--'
            opt_combination = (int(sene), int(dent), int(early_termination))
            if opt_combination in plot_labels:
                label = plot_labels[opt_combination]
                marker = markers[opt_combination]
                color = colors[opt_combination]
            else:
                continue

            ax.plot(subdata[x_key], subdata['throughput'], marker=marker, linewidth=linewidth, markersize=markersize, label=label, linestyle=linestyle, color=color)

    fig.set_size_inches(10, 3)
    centered_figlegend(fig, axs[0],
        ncol=4,
        handletextpad=0.08, columnspacing=1.5,
        framealpha=0, prop={'size': 12}
        )
    plt.savefig(output_path,
        bbox_inches = 'tight',
        pad_inches = 0,
        dpi=400)

def plot_cpu_threads_WO_supplementary(threads_path, WO_path, output_path):    
    plt.rcParams['text.usetex'] = True
    boldfont = r'\fontfamily{qtm}\selectfont'
    regularfont = r'\fontfamily{cmr}\selectfont'

    def c(r255,g255,b255):
        return tuple((component/255 for component in (r255,g255,b255)))

    plot_labels = {
        (0, 0, 0): 'GenASM (CPU)',
        (1, 0, 0): 'Scrooge SENE (CPU)',
        (0, 1, 0): 'Scrooge DENT (CPU)',
        (0, 0, 1): 'Scrooge ET (CPU)',
        (1, 1, 0): 'Scrooge SENE+DENT (CPU)',
        (1, 0, 1): 'Scrooge SENE+ET (CPU)',
        (0, 1, 1): 'Scrooge DENT+ET (CPU)',
        (1, 1, 1): 'Scrooge Full (CPU)'
    }
    markers = {
        (0, 0, 0): 'o',
        (1, 0, 0): 'v',
        (0, 1, 0): 5,
        (0, 0, 1): '^',
        (1, 1, 0): 6,
        (1, 0, 1): 'x',
        (0, 1, 1): 'X',
        (1, 1, 1): 'D'
    }
    colors = {
        (0, 0, 0): c(0, 0, 0),
        (0, 0, 1): c(217,95,2),
        (1, 0, 0): c(117,112,179),
        (1, 0, 1): c(27,158,119),
        (0, 1, 0): c(70, 230, 150),
        (1, 1, 0): c(255, 0, 0),
        (0, 1, 1): c(0, 0, 255),
        (1, 1, 1): c(255, 128, 255)
    }

    datas = []
    for path in [threads_path, WO_path]:
        datas.append(pandas.read_csv(path,
            usecols=['W', 'O', 'SENE', 'DENT', 'early termination', 'threads', 'aligns/second'],
            dtype={'W' : int, 'O': int, 'SENE' : bool, 'DENT' : bool, 'early termination' : bool, 'threads': int, 'aligns/second': float}))
        datas[-1].rename(columns={'aligns/second':'throughput'}, inplace=True)
    datas[0].query(f'threads<=48', inplace=True)
    #datas[1].query(f'W<=128', inplace=True)
    x_keys = ['threads', 'W']
    x_labels = ['Threads', 'Window Size (W)']
    x_granularities = [8, 32]

    fig, axs = plt.subplots(1, 2, constrained_layout=True)
    y_granularity = 2000
    y_minor_granularity = 1000

    bottom, top = 0, int(math.ceil(max(datas[0]['throughput'] + datas[1]['throughput'])/y_granularity))*y_granularity
    for x_key, ax, data, x_granularity, x_label, name in zip(x_keys, axs, datas, x_granularities, x_labels, [rf'{boldfont}\textbf{{(a)}} {regularfont} CPU Scaling', rf'{boldfont}\textbf{{(b)}} {regularfont} CPU W Sensitivity']):
        left, right = min(data[x_key]), max(data[x_key])

        if ax==axs[0]:
            ax.set_ylabel(r"\textbf{Alignments per Second}", size=13, labelpad=1)
        else:
            ax.sharey(axs[0])

        ax.set_xticks(ticks=[left]+list(range(x_granularity, right+1, x_granularity)))
        ax.set_xticks(ticks=data[x_key].unique(), minor=True)
        ax.tick_params(axis='both', which='major', pad=1)

        ax.set_xlabel(f'\\textbf{{{x_label}}}', size=13, labelpad=1)

        corner_box_text(ax, 1, 1, name)
        prepare_plot(fig, ax, range(bottom, top, y_granularity), left, right, have_labels=ax==axs[0], yminor=range(bottom, top, y_minor_granularity))

        for  early_termination, sene, dent in itertools.product([False, True], [False, True], [False, True]):
            subdata=data.query(f'SENE=={sene}')
            subdata=subdata.query(f'DENT=={dent}')
            subdata=subdata.query(f'`early termination`=={early_termination}')
            subdata=subdata.groupby([x_key]).mean().reset_index()
            subdata=subdata.sort_values(by=[x_key])

            linewidth = 1.8
            markersize = 6
            linestyle= '-' #'-' if dp_mem_type=='global' else '--'
            opt_combination = (int(sene), int(dent), int(early_termination))
            if opt_combination in plot_labels:
                label = plot_labels[opt_combination]
                marker = markers[opt_combination]
                color = colors[opt_combination]
            else:
                continue

            ax.plot(subdata[x_key], subdata['throughput'], marker=marker, linewidth=linewidth, markersize=markersize, label=label, linestyle=linestyle, color=color)

    fig.set_size_inches(10, 3)
    centered_figlegend(fig, axs[0],
        ncol=4,
        handletextpad=0.08, columnspacing=1.5,
        framealpha=0, prop={'size': 12}
        )
    plt.savefig(output_path,
        bbox_inches = 'tight',
        pad_inches = 0,
        dpi=400)

def plot_cpu_O(path, output_path):
    data = pandas.read_csv(path,
        usecols=['W', 'O', 'SENE', 'DENT', 'early termination', 'threads', 'aligns/second'],
        dtype={'W' : int, 'O': int, 'SENE' : bool, 'DENT' : bool, 'early termination' : bool, 'threads': int, 'aligns/second': float})

    fig, ax = plt.subplots()
    y_granularity = 2000
    left, right = min(data['O']), max(data['O'])
    bottom, top = 0, int(math.ceil(max(data['aligns/second']/y_granularity)))*y_granularity
    prepare_plot(fig, ax, range(bottom, top+1, y_granularity), left, right)

    for i, (sene, dent, early_termination) in enumerate(itertools.product([False, True], [False, True], [False, True])):
        subdata=data.query(f'SENE=={sene}').query(f'DENT=={dent}').query(f'`early termination`=={early_termination}')
        subdata.sort_values(by=['O'], inplace=True)

        marker = markers[i%len(markers)]
        linewidth = 1
        markersize = 6
        label=f"<{int(sene)},{int(dent)},{int(early_termination)}>"
        color = colors[int(sene)*4 + int(dent)*2 + int(early_termination)]

        ax.plot(subdata['O'], subdata['aligns/second'], color=color, label=label, marker=marker, markersize=markersize, linewidth=linewidth)


    ax.set_xlabel("Window Overlap (O)")
    ax.set_ylabel('Alignments per Second')

    plt.legend()

    plt.savefig(output_path)

def plot_gpu_threadblocks_paper(path, output_path):
    plt.rcParams['text.usetex'] = True
    boldfont = r'\fontfamily{qtm}\selectfont'
    regularfont = r'\fontfamily{cmr}\selectfont'

    data = pandas.read_csv(path,
        usecols=['W', 'O', 'sene', 'dent', 'early termination', 'threadblocks/sm', 'cigar sublist size', 'dp memory type', 'smem carveout percent', 'arch', 'gpu', 'sm count', 'available smem per sm (kiB)', 'used smem per threadblock (B)', 'throughput (aligns/s)'],
        dtype={'W' : int, 'O': int, 'sene' : bool, 'dent' : bool, 'early termination' : bool, 'threadblocks/sm': int, 'throughput (aligns/s)': float})

    data.rename(columns={'throughput (aligns/s)':'throughput'}, inplace=True)
    data['threadblocks'] = data['threadblocks/sm'] * data['sm count']

    def c(r255,g255,b255):
        return tuple((component/255 for component in (r255,g255,b255)))

    memory_labels = {
        'shared': rf'{boldfont} \textbf{{(a)}} {regularfont} Shared Memory',
        'global': rf'{boldfont} \textbf{{(b)}} {regularfont} Global Memory'
    }
    plot_labels = {
        (0, 0, 0): 'GenASM (GPU)',
        (1, 0, 0): 'Scrooge SENE (GPU)',
        (0, 1, 0): 'Scrooge DENT (GPU)',
        (1, 1, 1): 'Scrooge Full (GPU)'
    }
    markers = {
        (0, 0, 0): 'X',
        (1, 0, 0): '^',
        (0, 1, 0): 'v',
        (1, 1, 1): 'o'
    }
    colors = {
        (0, 0, 0): c(0, 0, 0),
        (1, 0, 0): c(217,95,2),
        (0, 1, 0): c(27,158,119),#c(117,112,179),
        (1, 1, 1): ours_color#c(27,158,119)
    }

    fig, axs = plt.subplots(1, 2, constrained_layout=True)
    y_granularity = 10000
    y_minor_granularity = 2000
    x_granularity = 4*min(data['threadblocks'])#5*min(data['threadblocks'])
    left, right = min(data['threadblocks']), max(data['threadblocks'])
    bottom, top = 0, int(math.ceil(max(data['throughput']/y_granularity)))*y_granularity
    for ax, (dp_mem_type, subdata) in zip(axs, reversed(list(data.groupby(['dp memory type'])))):
        if ax==axs[0]:
            ax.set_ylabel(r"\textbf{Alignments per Second}", labelpad=1, size=13)
            ax.set_ylim([bottom, max(data['throughput'])*1.05])
        else:
            ax.sharey(axs[0])
        ax.set_xticks(ticks=[left]+list(range(x_granularity, right+1, x_granularity)))
        ax.set_xticks(ticks=data['threadblocks'], minor=True)
        ax.tick_params(axis='both', which='major', pad=1)
        ax.set_xlabel(r"\textbf{Thread Blocks}", labelpad=1, size=13)
        prepare_plot(fig, ax, range(bottom, top, y_granularity), left, right, have_labels=ax==axs[0], yminor=range(bottom, top, y_minor_granularity))
        corner_box_text(ax, 1, 0, memory_labels[dp_mem_type], size=13)

        for dent, sene,  early_termination in itertools.product([False, True], [False, True], [False, True]):
            subdata=data.query(f'sene=={sene}')
            subdata=subdata.query(f'dent=={dent}')
            subdata=subdata.query(f'`early termination`=={early_termination}')
            subdata=subdata.query(f'`dp memory type`=="{dp_mem_type}"')
            subdata=subdata.groupby('threadblocks').mean().reset_index()
            subdata=subdata.sort_values(by='threadblocks')

            linewidth = 2
            markersize = 6
            linestyle= '-' #'-' if dp_mem_type=='global' else '--'
            #label = f"{int(sene)},{int(dent)},{int(early_termination)},{dp_mem_type}"
            opt_combination = (int(sene), int(dent), int(early_termination))
            if opt_combination in plot_labels:
                label = plot_labels[opt_combination]
                marker = markers[opt_combination]
                color = colors[opt_combination]
            else:
                continue

            ax.plot(subdata['threadblocks'], subdata['throughput'], marker=marker, linewidth=linewidth, markersize=markersize, label=label, linestyle=linestyle, color=color)

    fig.set_size_inches(10, 3)
    centered_figlegend(fig, axs[0],
        ncol=4,
        handletextpad=0.08, columnspacing=1.5,
        framealpha=0, prop={'size': 13}
        )
    plt.savefig(output_path,
        bbox_inches = 'tight',
        pad_inches = 0,
        dpi=400)

def plot_gpu_threadblocks_illumina_paper(path, output_path):
    data = pandas.read_csv(path,
        usecols=['W', 'O', 'sene', 'dent', 'early termination', 'threadblocks/sm', 'cigar sublist size', 'dp memory type', 'smem carveout percent', 'arch', 'gpu', 'sm count', 'available smem per sm (kiB)', 'used smem per threadblock (B)', 'throughput (aligns/s)'],
        dtype={'W' : int, 'O': int, 'sene' : bool, 'dent' : bool, 'early termination' : bool, 'threadblocks/sm': int, 'throughput (aligns/s)': float})

    data.rename(columns={'throughput (aligns/s)':'throughput'}, inplace=True)
    data['threadblocks'] = data['threadblocks/sm'] * data['sm count']

    def c(r255,g255,b255):
        return tuple((component/255 for component in (r255,g255,b255)))

    memory_labels = {
        'shared': 'Shared Memory',
        'global': 'Global Memory'
    }
    plot_labels = {
        (0, 0, 0): 'GenASM (GPU)',
        (1, 0, 0): 'Scrooge SENE (GPU)',
        (0, 0, 1): 'Scrooge ET (GPU)',
        (1, 0, 1): 'Scrooge SENE+ET (GPU)'
    }
    markers = {
        (0, 0, 0): 'o',
        (1, 0, 0): '^',
        (0, 0, 1): 'v',
        (1, 0, 1): 'X'
    }
    colors = {
        (0, 0, 0): c(0, 0, 0),
        (1, 0, 0): c(217,95,2),
        (0, 0, 1): c(117,112,179),
        (1, 0, 1): c(27,158,119)
    }

    fig, axs = plt.subplots(1, 2, constrained_layout=True)
    y_granularity = 2000000
    y_minor_granularity = 500000
    x_granularity = 5*min(data['sm count'])
    left, right = min(data['threadblocks']), max(data['threadblocks'])
    bottom, top = 0, int(max(data['throughput'])*1.05)#int(math.ceil(max(data['throughput']/y_granularity)))*y_granularity
    for ax, (dp_mem_type, subdata) in zip(axs, reversed(list(data.groupby(['dp memory type'])))):
        if ax==axs[0]:
            ax.set_ylabel("Alignments per Second", size=14)
            ax.set_ylim([bottom, top])
        else:
            ax.sharey(axs[0])
        #ax.set_xticks(ticks=[left]+list(range(x_granularity, right+1, x_granularity)))
        #ax.set_xticks(ticks=data['threadblocks'].unique(), minor=True)
        ax.set_xticks(ticks=data['threadblocks'].unique())
        ax.set_xlabel("Thread Blocks")
        prepare_plot(fig, ax, range(bottom, top, y_granularity), left, right, have_labels=ax==axs[0], yminor=range(bottom, top, y_minor_granularity), yticks_fontsize=11)
        corner_box_text(ax, 0, 1, memory_labels[dp_mem_type])

        for sene, dent, early_termination in itertools.product([False, True], [False, True], [False, True]):
            subdata=data.query(f'sene=={sene}')
            subdata=subdata.query(f'dent=={dent}')
            subdata=subdata.query(f'`early termination`=={early_termination}')
            subdata=subdata.query(f'`dp memory type`=="{dp_mem_type}"')
            subdata=subdata.groupby('threadblocks').mean().reset_index()
            subdata=subdata.sort_values(by='threadblocks')

            linewidth = 2
            markersize = 7
            linestyle= '-' #'-' if dp_mem_type=='global' else '--'
            #label = f"{int(sene)},{int(dent)},{int(early_termination)},{dp_mem_type}"
            opt_combination = (int(sene), int(dent), int(early_termination))
            if opt_combination in plot_labels:
                label = plot_labels[opt_combination]
                marker = markers[opt_combination]
                color = colors[opt_combination]
            else:
                continue

            ax.plot(subdata['threadblocks'], subdata['throughput'], marker=marker, linewidth=linewidth, markersize=markersize, label=label, linestyle=linestyle, color=color)

    fig.set_size_inches(10, 3)
    centered_figlegend(fig, axs[0],
        ncol=4,
        handletextpad=0.08, columnspacing=1.5,
        framealpha=0, prop={'size': 12}
        )
    plt.savefig(output_path,
        bbox_inches = 'tight',
        pad_inches = 0,
        dpi=400)

def plot_gpu_threadblocks_supplementary(path, output_path):
    plt.rcParams['text.usetex'] = True
    boldfont = r'\fontfamily{qtm}\selectfont'
    regularfont = r'\fontfamily{cmr}\selectfont'

    data = pandas.read_csv(path,
        usecols=['W', 'O', 'sene', 'dent', 'early termination', 'threadblocks/sm', 'cigar sublist size', 'dp memory type', 'smem carveout percent', 'arch', 'gpu', 'sm count', 'available smem per sm (kiB)', 'used smem per threadblock (B)', 'throughput (aligns/s)'],
        dtype={'W' : int, 'O': int, 'sene' : bool, 'dent' : bool, 'early termination' : bool, 'threadblocks/sm': int, 'throughput (aligns/s)': float})

    data.rename(columns={'throughput (aligns/s)':'throughput'}, inplace=True)
    data['threadblocks'] = data['threadblocks/sm'] * data['sm count']

    def c(r255,g255,b255):
        return tuple((component/255 for component in (r255,g255,b255)))

    memory_labels = {
        'shared': 'Shared Memory',
        'global': 'Global Memory'
    }
    plot_labels = {
        (0, 0, 0): 'GenASM (GPU)',
        (1, 0, 0): 'Scrooge SENE (GPU)',
        (0, 1, 0): 'Scrooge DENT (GPU)',
        (0, 0, 1): 'Scrooge ET (GPU)',
        (1, 1, 0): 'Scrooge SENE+DENT (GPU)',
        (1, 0, 1): 'Scrooge SENE+ET (GPU)',
        (0, 1, 1): 'Scrooge DENT+ET (GPU)',
        (1, 1, 1): 'Scrooge Full (GPU)'
    }
    markers = {
        (0, 0, 0): 'o',
        (1, 0, 0): '^',
        (0, 1, 0): 'v',
        (0, 0, 1): 4,
        (1, 1, 0): 5,
        (1, 0, 1): 'X',
        (0, 1, 1): 'D',
        (1, 1, 1): 'x'
    }
    colors = {
        (0, 0, 0): c(0, 0, 0),
        (1, 0, 0): c(217,95,2),
        (0, 1, 0): c(117,112,179),
        (0, 0, 1): c(255, 0, 0),
        (1, 1, 0): c(70, 230, 150),
        (1, 0, 1): c(0, 0, 255),
        (0, 1, 1): c(255, 128, 255),
        (1, 1, 1): c(27,158,119)
    }

    fig, axs = plt.subplots(1, 2, constrained_layout=True)
    y_granularity = 10000
    y_minor_granularity = 2000
    x_granularity = 4*min(data['threadblocks'])#5*min(data['threadblocks'])
    left, right = min(data['threadblocks']), max(data['threadblocks'])
    bottom, top = 0, int(math.ceil(max(data['throughput']/y_granularity)))*y_granularity
    for ax, (dp_mem_type, subdata) in zip(axs, reversed(list(data.groupby(['dp memory type'])))):
        if ax==axs[0]:
            ax.set_ylabel(r"\textbf{Alignments per Second}", labelpad=1, size=13)
            ax.set_ylim([bottom, max(data['throughput'])*1.05])
        else:
            ax.sharey(axs[0])
        ax.set_xticks(ticks=[left]+list(range(x_granularity, right+1, x_granularity)))
        ax.set_xticks(ticks=data['threadblocks'], minor=True)
        ax.tick_params(axis='both', which='major', pad=1)
        ax.set_xlabel(r"\textbf{Thread Blocks}", labelpad=1, size=13)
        prepare_plot(fig, ax, range(bottom, top, y_granularity), left, right, have_labels=ax==axs[0], yminor=range(bottom, top, y_minor_granularity))
        corner_box_text(ax, 1, 0, memory_labels[dp_mem_type], size=13)

        for dent, sene,  early_termination in itertools.product([False, True], [False, True], [False, True]):
            subdata=data.query(f'sene=={sene}')
            subdata=subdata.query(f'dent=={dent}')
            subdata=subdata.query(f'`early termination`=={early_termination}')
            subdata=subdata.query(f'`dp memory type`=="{dp_mem_type}"')
            subdata=subdata.groupby('threadblocks').mean().reset_index()
            subdata=subdata.sort_values(by='threadblocks')

            linewidth = 2
            markersize = 6
            linestyle= '-' #'-' if dp_mem_type=='global' else '--'
            #label = f"{int(sene)},{int(dent)},{int(early_termination)},{dp_mem_type}"
            opt_combination = (int(sene), int(dent), int(early_termination))
            if opt_combination in plot_labels:
                label = plot_labels[opt_combination]
                marker = markers[opt_combination]
                color = colors[opt_combination]
            else:
                continue

            ax.plot(subdata['threadblocks'], subdata['throughput'], marker=marker, linewidth=linewidth, markersize=markersize, label=label, linestyle=linestyle, color=color)

    fig.set_size_inches(10, 3)
    centered_figlegend(fig, axs[0],
        ncol=4,
        handletextpad=0.08, columnspacing=1.5,
        framealpha=0, prop={'size': 13}
        )
    plt.savefig(output_path,
        bbox_inches = 'tight',
        pad_inches = 0,
        dpi=400)

def plot_gpu_WO_paper(path, output_path):
    data = pandas.read_csv(path,
        usecols=['W', 'O', 'sene', 'dent', 'early termination', 'threadblocks/sm', 'cigar sublist size', 'dp memory type', 'smem carveout percent', 'arch', 'gpu', 'sm count', 'available smem per sm (kiB)', 'used smem per threadblock (B)', 'throughput (aligns/s)'],
        dtype={'W' : int, 'O': int, 'sene' : bool, 'dent' : bool, 'early termination' : bool, 'threadblocks/sm': int, 'throughput (aligns/s)': float})
    data.rename(columns={'throughput (aligns/s)':'throughput'}, inplace=True)
    #data.query('W<=128', inplace=True)

    def c(r255,g255,b255):
        return tuple((component/255 for component in (r255,g255,b255)))

    memory_labels = {
        'shared': 'Shared Memory',
        'global': 'Global Memory'
    }
    plot_labels = {
        (0, 0, 0): 'GenASM (GPU)',
        (1, 0, 0): 'Scrooge SENE (GPU)',
        (0, 1, 0): 'Scrooge DENT (GPU)',
        (1, 1, 1): 'Scrooge Full (GPU)'
    }
    markers = {
        (0, 0, 0): 'o',
        (1, 0, 0): '^',
        (0, 1, 0): 'v',
        (1, 1, 1): 'x'
    }
    colors = {
        (0, 0, 0): c(0, 0, 0),
        (1, 0, 0): c(217,95,2),
        (0, 1, 0): c(117,112,179),
        (1, 1, 1): c(27,158,119)
    }

    fig, axs = plt.subplots(1, 2, gridspec_kw={'width_ratios': [1, 2]}, constrained_layout=True)

    y_minor_granularity = 4000
    y_major_granularity = 20000
    x_granularity = 32
    bottom, top = 0, max(data['throughput'])
    bottom_hbar, top_hbar = 0, int(math.ceil(top/y_major_granularity))*y_major_granularity
    y_minor_ticks = list(range(bottom_hbar, top_hbar, y_minor_granularity))
    y_major_ticks = list(range(bottom_hbar, top_hbar, y_major_granularity))
    for ax, (dp_mem_type, subdata) in zip(axs, reversed(list(data.groupby(['dp memory type'])))):
        left, right = min(subdata['W']), max(subdata['W'])
        if ax==axs[0]:
            ax.set_ylabel("Alignments per Second")
        else:
            ax.sharey(axs[0])
        ax.set_xticks(ticks=[left]+list(range(x_granularity, right+1, x_granularity)))
        ax.set_xticks(ticks=subdata['W'], minor=True)
        ax.set_xlabel("Window Size (W)")
        prepare_plot(fig, ax, y_major_ticks, left, right, have_labels=ax==axs[0], yminor=y_minor_ticks)

        ax.annotate(memory_labels[dp_mem_type], xy=(1, 1), xytext=(-5.0, -5.0),
            xycoords='axes fraction', textcoords='offset points',
            bbox=dict(facecolor='white', alpha=0.8, pad=5.0),
            horizontalalignment='right', verticalalignment='top')

        for (sene, dent, early_termination), subdata in subdata.groupby(['sene', 'dent', 'early termination']):
            subdata=subdata.groupby('W').mean().reset_index()
            subdata=subdata.sort_values(by=['W'])

            markersize = 6
            linewidth = 1.5
            linestyle= '-' #'-' if dp_mem_type=='global' else '--'
            opt_combination = (int(sene), int(dent), int(early_termination))
            if opt_combination in plot_labels:
                label = plot_labels[opt_combination]
                marker = markers[opt_combination]
                color = colors[opt_combination]
            else:
                continue

            ax.plot(subdata['W'], subdata['throughput'], marker=marker, linewidth=linewidth, markersize=markersize, label=label, linestyle=linestyle, color=color)

    fig.set_size_inches(10, 3)
    centered_figlegend(fig, axs[0],
        ncol=4,
        handletextpad=0.08, columnspacing=1.5,
        framealpha=0, prop={'size': 12}
        )
    plt.savefig(output_path,
        bbox_inches = 'tight',
        pad_inches = 0,
        dpi=400)

def plot_gpu_WO_illumina_paper(path, output_path):
    data = pandas.read_csv(path,
        usecols=['W', 'O', 'sene', 'dent', 'early termination', 'threadblocks/sm', 'cigar sublist size', 'dp memory type', 'smem carveout percent', 'arch', 'gpu', 'sm count', 'available smem per sm (kiB)', 'used smem per threadblock (B)', 'throughput (aligns/s)'],
        dtype={'W' : int, 'O': int, 'sene' : bool, 'dent' : bool, 'early termination' : bool, 'threadblocks/sm': int, 'throughput (aligns/s)': float})
    data.rename(columns={'throughput (aligns/s)':'throughput'}, inplace=True)
    #data.query('W<=128', inplace=True)

    def c(r255,g255,b255):
        return tuple((component/255 for component in (r255,g255,b255)))

    memory_labels = {
        'shared': 'Shared Memory',
        'global': 'Global Memory'
    }
    plot_labels = {
        (0, 0, 0): 'GenASM (GPU)',
        (1, 0, 0): 'Scrooge SENE (GPU)',
        (0, 0, 1): 'Scrooge ET (GPU)',
        (1, 0, 1): 'Scrooge SENE+ET (GPU)'
    }
    markers = {
        (0, 0, 0): 'o',
        (1, 0, 0): '^',
        (0, 0, 1): 'v',
        (1, 0, 1): 'x'
    }
    colors = {
        (0, 0, 0): c(0, 0, 0),
        (1, 0, 0): c(217,95,2),
        (0, 0, 1): c(117,112,179),
        (1, 0, 1): c(27,158,119)
    }

    fig, axs = plt.subplots(1, 2, gridspec_kw={'width_ratios': [1, 2]}, constrained_layout=True)

    y_minor_granularity = 4000
    y_major_granularity = 20000
    x_granularity = 32
    bottom, top = 0, max(data['throughput'])
    bottom_hbar, top_hbar = 0, int(math.ceil(top/y_major_granularity))*y_major_granularity
    y_minor_ticks = list(range(bottom_hbar, top_hbar, y_minor_granularity))
    y_major_ticks = list(range(bottom_hbar, top_hbar, y_major_granularity))
    for ax, (dp_mem_type, subdata) in zip(axs, reversed(list(data.groupby(['dp memory type'])))):
        left, right = min(subdata['W']), max(subdata['W'])
        if ax==axs[0]:
            ax.set_ylabel("Alignments per Second")
        else:
            ax.sharey(axs[0])
        ax.set_xticks(ticks=[left]+list(range(x_granularity, right+1, x_granularity)))
        ax.set_xticks(ticks=subdata['W'], minor=True)
        ax.set_xlabel("Window Size (W)")
        prepare_plot(fig, ax, y_major_ticks, left, right, have_labels=ax==axs[0], yminor=y_minor_ticks)

        ax.annotate(memory_labels[dp_mem_type], xy=(1, 1), xytext=(-5.0, -5.0),
            xycoords='axes fraction', textcoords='offset points',
            bbox=dict(facecolor='white', alpha=0.8, pad=5.0),
            horizontalalignment='right', verticalalignment='top')

        for (sene, dent, early_termination), subdata in subdata.groupby(['sene', 'dent', 'early termination']):
            subdata=subdata.groupby('W').mean().reset_index()
            subdata=subdata.sort_values(by='W')

            markersize = 6
            linewidth = 1.5
            linestyle= '-' #'-' if dp_mem_type=='global' else '--'
            opt_combination = (int(sene), int(dent), int(early_termination))
            if opt_combination in plot_labels:
                label = plot_labels[opt_combination]
                marker = markers[opt_combination]
                color = colors[opt_combination]
            else:
                continue

            ax.plot(subdata['W'], subdata['throughput'], marker=marker, linewidth=linewidth, markersize=markersize, label=label, linestyle=linestyle, color=color)

    fig.set_size_inches(10, 3)
    centered_figlegend(fig, axs[0],
        ncol=4,
        handletextpad=0.08, columnspacing=1.5,
        framealpha=0, prop={'size': 12}
        )
    plt.savefig(output_path,
        bbox_inches = 'tight',
        pad_inches = 0,
        dpi=400)

def plot_gpu_WO_supplementary(path, output_path):
    data = pandas.read_csv(path,
        usecols=['W', 'O', 'sene', 'dent', 'early termination', 'threadblocks/sm', 'cigar sublist size', 'dp memory type', 'smem carveout percent', 'arch', 'gpu', 'sm count', 'available smem per sm (kiB)', 'used smem per threadblock (B)', 'throughput (aligns/s)'],
        dtype={'W' : int, 'O': int, 'sene' : bool, 'dent' : bool, 'early termination' : bool, 'threadblocks/sm': int, 'throughput (aligns/s)': float})
    data.rename(columns={'throughput (aligns/s)':'throughput'}, inplace=True)
    #data.query('W<=128', inplace=True)

    def c(r255,g255,b255):
        return tuple((component/255 for component in (r255,g255,b255)))

    memory_labels = {
        'shared': 'Shared Memory',
        'global': 'Global Memory'
    }
    plot_labels = {
        (0, 0, 0): 'GenASM (GPU)',
        (1, 0, 0): 'Scrooge SENE (GPU)',
        (0, 1, 0): 'Scrooge DENT (GPU)',
        (0, 0, 1): 'Scrooge ET (GPU)',
        (1, 1, 0): 'Scrooge SENE+DENT (GPU)',
        (1, 0, 1): 'Scrooge SENE+ET (GPU)',
        (0, 1, 1): 'Scrooge DENT+ET (GPU)',
        (1, 1, 1): 'Scrooge Full (GPU)'
    }
    markers = {
        (0, 0, 0): 'o',
        (1, 0, 0): '^',
        (0, 1, 0): 'v',
        (0, 0, 1): 4,
        (1, 1, 0): 5,
        (1, 0, 1): 'X',
        (0, 1, 1): 'D',
        (1, 1, 1): 'x'
    }
    colors = {
        (0, 0, 0): c(0, 0, 0),
        (1, 0, 0): c(217,95,2),
        (0, 1, 0): c(117,112,179),
        (0, 0, 1): c(255, 0, 0),
        (1, 1, 0): c(70, 230, 150),
        (1, 0, 1): c(0, 0, 255),
        (0, 1, 1): c(255, 128, 255),
        (1, 1, 1): c(27,158,119)
    }

    fig, axs = plt.subplots(1, 2, gridspec_kw={'width_ratios': [1, 2]}, constrained_layout=True)

    y_minor_granularity = 4000
    y_major_granularity = 20000
    x_granularity = 32
    bottom, top = 0, max(data['throughput'])
    bottom_hbar, top_hbar = 0, int(math.ceil(top/y_major_granularity))*y_major_granularity
    y_minor_ticks = list(range(bottom_hbar, top_hbar, y_minor_granularity))
    y_major_ticks = list(range(bottom_hbar, top_hbar, y_major_granularity))
    for ax, (dp_mem_type, subdata) in zip(axs, reversed(list(data.groupby(['dp memory type'])))):
        left, right = min(subdata['W']), max(subdata['W'])
        if ax==axs[0]:
            ax.set_ylabel("Alignments per Second")
        else:
            ax.sharey(axs[0])
        ax.set_xticks(ticks=[left]+list(range(x_granularity, right+1, x_granularity)))
        ax.set_xticks(ticks=subdata['W'], minor=True)
        ax.set_xlabel("Window Size (W)")
        prepare_plot(fig, ax, y_major_ticks, left, right, have_labels=ax==axs[0], yminor=y_minor_ticks)

        ax.annotate(memory_labels[dp_mem_type], xy=(1, 1), xytext=(-5.0, -5.0),
            xycoords='axes fraction', textcoords='offset points',
            bbox=dict(facecolor='white', alpha=0.8, pad=5.0),
            horizontalalignment='right', verticalalignment='top')

        for (sene, dent, early_termination), subdata in subdata.groupby(['sene', 'dent', 'early termination']):
            subdata=subdata.sort_values(by=['W'])

            markersize = 6
            linewidth = 1.5
            linestyle= '-' #'-' if dp_mem_type=='global' else '--'
            opt_combination = (int(sene), int(dent), int(early_termination))
            if opt_combination in plot_labels:
                label = plot_labels[opt_combination]
                marker = markers[opt_combination]
                color = colors[opt_combination]
            else:
                continue

            ax.plot(subdata['W'], subdata['throughput'], marker=marker, linewidth=linewidth, markersize=markersize, label=label, linestyle=linestyle, color=color)

    fig.set_size_inches(10, 3)
    centered_figlegend(fig, axs[0],
        ncol=4,
        handletextpad=0.08, columnspacing=1.5,
        framealpha=0, prop={'size': 12}
        )
    plt.savefig(output_path,
        bbox_inches = 'tight',
        pad_inches = 0,
        dpi=400)

def plot_gpu_O_illumina_paper(path, output_path):
    data = pandas.read_csv(path,
        usecols=['W', 'O', 'sene', 'dent', 'early termination', 'threadblocks/sm', 'cigar sublist size', 'dp memory type', 'smem carveout percent', 'arch', 'gpu', 'sm count', 'available smem per sm (kiB)', 'used smem per threadblock (B)', 'throughput (aligns/s)'],
        dtype={'W' : int, 'O': int, 'sene' : bool, 'dent' : bool, 'early termination' : bool, 'threadblocks/sm': int, 'throughput (aligns/s)': float})
    data.rename(columns={'throughput (aligns/s)':'throughput'}, inplace=True)
    #data.query('W<=128', inplace=True)

    def c(r255,g255,b255):
        return tuple((component/255 for component in (r255,g255,b255)))

    memory_labels = {
        'shared': 'Shared Memory',
        'global': 'Global Memory'
    }
    plot_labels = {
        (0, 0, 0): 'GenASM (GPU)',
        (1, 0, 0): 'Scrooge SENE (GPU)',
        (0, 0, 1): 'Scrooge ET (GPU)',
        (1, 0, 1): 'Scrooge SENE+ET (GPU)'
    }
    markers = {
        (0, 0, 0): 'o',
        (1, 0, 0): '^',
        (0, 0, 1): 'v',
        (1, 0, 1): 'x'
    }
    colors = {
        (0, 0, 0): c(0, 0, 0),
        (1, 0, 0): c(217,95,2),
        (0, 0, 1): c(117,112,179),
        (1, 0, 1): c(27,158,119)
    }

    fig, axs = plt.subplots(1, 2, constrained_layout=True)
    y_granularity = 2000000
    y_minor_granularity = 500000
    x_granularity = 8
    bottom, top = 0, max(data['throughput'])
    bottom_hbar, top_hbar = 0, int(math.ceil(top/y_granularity))*y_granularity
    for ax, (dp_mem_type, subdata) in zip(axs, reversed(list(data.groupby(['dp memory type'])))):
        left, right = min(subdata['O']), max(subdata['O'])
        prepare_plot(fig, ax, range(bottom_hbar, top_hbar, y_granularity), left, right, ax==axs[0], yminor=range(bottom_hbar, top_hbar, y_minor_granularity))
        if ax==axs[0]:
            ax.set_ylabel("Alignments per Second")
        else:
            ax.sharey(axs[0])
        #ax.set_xticks(ticks=[left]+list(range(x_granularity, right, x_granularity)) + [right])
        #ax.set_xticks(subdata['O'], minor=True)
        ax.set_xticks(subdata['O'].unique())
        ax.set_xlabel("Window Overlap (O)")
        corner_box_text(ax, 1, 1, memory_labels[dp_mem_type])

        for (sene, dent, early_termination), subdata in subdata.groupby(['sene', 'dent', 'early termination']):
            subdata=subdata.groupby('O').mean().reset_index()
            subdata=subdata.sort_values(by='O')

            linewidth = 1.5
            markersize = 6
            linestyle= '-' #'-' if dp_mem_type=='global' else '--'
            opt_combination = (int(sene), int(dent), int(early_termination))
            if opt_combination in plot_labels:
                label = plot_labels[opt_combination]
                marker = markers[opt_combination]
                color = colors[opt_combination]
            else:
                continue

            ax.plot(subdata['O'], subdata['throughput'], marker=marker, linewidth=linewidth, markersize=markersize, label=label, linestyle=linestyle, color=color)

    ax.set_ylim([bottom, top*1.05])
    fig.set_size_inches(10, 3)
    centered_figlegend(fig, axs[1],
        ncol=4,
        handletextpad=0.08, columnspacing=1.5,
        framealpha=0, prop={'size': 12}
        )
    plt.savefig(output_path,
        bbox_inches = 'tight',
        pad_inches = 0,
        dpi=400)

def plot_gpu_O_paper(path, output_path):
    data = pandas.read_csv(path,
        usecols=['W', 'O', 'sene', 'dent', 'early termination', 'threadblocks/sm', 'cigar sublist size', 'dp memory type', 'smem carveout percent', 'arch', 'gpu', 'sm count', 'available smem per sm (kiB)', 'used smem per threadblock (B)', 'throughput (aligns/s)'],
        dtype={'W' : int, 'O': int, 'sene' : bool, 'dent' : bool, 'early termination' : bool, 'threadblocks/sm': int, 'throughput (aligns/s)': float})
    data.rename(columns={'throughput (aligns/s)':'throughput'}, inplace=True)
    #data.query('W<=128', inplace=True)

    def c(r255,g255,b255):
        return tuple((component/255 for component in (r255,g255,b255)))

    memory_labels = {
        'shared': 'Shared Memory',
        'global': 'Global Memory'
    }
    plot_labels = {
        (0, 0, 0): 'GenASM (GPU)',
        (1, 0, 0): 'Scrooge SENE (GPU)',
        (0, 1, 0): 'Scrooge DENT (GPU)',
        (1, 1, 1): 'Scrooge Full (GPU)'
    }
    markers = {
        (0, 0, 0): 'o',
        (1, 0, 0): '^',
        (0, 1, 0): 'v',
        (1, 1, 1): 'x'
    }
    colors = {
        (0, 0, 0): c(0, 0, 0),
        (1, 0, 0): c(217,95,2),
        (0, 1, 0): c(117,112,179),
        (1, 1, 1): c(27,158,119)
    }

    fig, axs = plt.subplots(1, 2, constrained_layout=True)
    y_granularity = 10000
    y_minor_granularity = 2000
    x_granularity = 8
    bottom, top = 0, max(data['throughput'])
    bottom_hbar, top_hbar = 0, int(math.ceil(top/y_granularity))*y_granularity
    for ax, (dp_mem_type, subdata) in zip(axs, reversed(list(data.groupby(['dp memory type'])))):
        left, right = min(subdata['O']), max(subdata['O'])
        prepare_plot(fig, ax, range(bottom_hbar, top_hbar, y_granularity), left, right, ax==axs[0], yminor=range(bottom_hbar, top_hbar, y_minor_granularity))
        if ax==axs[0]:
            ax.set_ylabel("Alignments per Second")
        else:
            ax.sharey(axs[0])
        ax.set_xticks(ticks=[left]+list(range(x_granularity, right, x_granularity)) + [right])
        ax.set_xticks(subdata['O'], minor=True)
        ax.set_xlabel("Window Overlap (O)")
        corner_box_text(ax, 0, 1, memory_labels[dp_mem_type])

        for (sene, dent, early_termination), subdata in subdata.groupby(['sene', 'dent', 'early termination']):
            subdata=subdata.sort_values(by=['O'])

            linewidth = 1.5
            markersize = 6
            linestyle= '-' #'-' if dp_mem_type=='global' else '--'
            opt_combination = (int(sene), int(dent), int(early_termination))
            if opt_combination in plot_labels:
                label = plot_labels[opt_combination]
                marker = markers[opt_combination]
                color = colors[opt_combination]
            else:
                continue

            ax.plot(subdata['O'], subdata['throughput'], marker=marker, linewidth=linewidth, markersize=markersize, label=label, linestyle=linestyle, color=color)

    ax.set_ylim([bottom, top*1.05])
    fig.set_size_inches(10, 3)
    centered_figlegend(fig, axs[1],
        ncol=4,
        handletextpad=0.08, columnspacing=1.5,
        framealpha=0, prop={'size': 12}
        )
    plt.savefig(output_path,
        bbox_inches = 'tight',
        pad_inches = 0,
        dpi=400)

def plot_gpu_O_supplementary(path, output_path):
    data = pandas.read_csv(path,
        usecols=['W', 'O', 'sene', 'dent', 'early termination', 'threadblocks/sm', 'cigar sublist size', 'dp memory type', 'smem carveout percent', 'arch', 'gpu', 'sm count', 'available smem per sm (kiB)', 'used smem per threadblock (B)', 'throughput (aligns/s)'],
        dtype={'W' : int, 'O': int, 'sene' : bool, 'dent' : bool, 'early termination' : bool, 'threadblocks/sm': int, 'throughput (aligns/s)': float})
    data.rename(columns={'throughput (aligns/s)':'throughput'}, inplace=True)
    #data.query('W<=128', inplace=True)

    def c(r255,g255,b255):
        return tuple((component/255 for component in (r255,g255,b255)))

    memory_labels = {
        'shared': 'Shared Memory',
        'global': 'Global Memory'
    }
    plot_labels = {
        (0, 0, 0): 'GenASM (GPU)',
        (1, 0, 0): 'Scrooge SENE (GPU)',
        (0, 1, 0): 'Scrooge DENT (GPU)',
        (0, 0, 1): 'Scrooge ET (GPU)',
        (1, 1, 0): 'Scrooge SENE+DENT (GPU)',
        (1, 0, 1): 'Scrooge SENE+ET (GPU)',
        (0, 1, 1): 'Scrooge DENT+ET (GPU)',
        (1, 1, 1): 'Scrooge Full (GPU)'
    }
    markers = {
        (0, 0, 0): 'o',
        (1, 0, 0): '^',
        (0, 1, 0): 'v',
        (0, 0, 1): 4,
        (1, 1, 0): 5,
        (1, 0, 1): 'X',
        (0, 1, 1): 'D',
        (1, 1, 1): 'x'
    }
    colors = {
        (0, 0, 0): c(0, 0, 0),
        (1, 0, 0): c(217,95,2),
        (0, 1, 0): c(117,112,179),
        (0, 0, 1): c(255, 0, 0),
        (1, 1, 0): c(70, 230, 150),
        (1, 0, 1): c(0, 0, 255),
        (0, 1, 1): c(255, 128, 255),
        (1, 1, 1): c(27,158,119)
    }

    fig, axs = plt.subplots(1, 2, constrained_layout=True)
    y_granularity = 10000
    y_minor_granularity = 2000
    x_granularity = 8
    bottom, top = 0, max(data['throughput'])
    bottom_hbar, top_hbar = 0, int(math.ceil(top/y_granularity))*y_granularity
    for ax, (dp_mem_type, subdata) in zip(axs, reversed(list(data.groupby(['dp memory type'])))):
        left, right = min(subdata['O']), max(subdata['O'])
        prepare_plot(fig, ax, range(bottom_hbar, top_hbar, y_granularity), left, right, ax==axs[0], yminor=range(bottom_hbar, top_hbar, y_minor_granularity))
        if ax==axs[0]:
            ax.set_ylabel("Alignments per Second")
        else:
            ax.sharey(axs[0])
        ax.set_xticks(ticks=[left]+list(range(x_granularity, right, x_granularity)) + [right])
        ax.set_xticks(subdata['O'], minor=True)
        ax.set_xlabel("Window Overlap (O)")
        corner_box_text(ax, 0, 1, memory_labels[dp_mem_type])

        for (sene, dent, early_termination), subdata in subdata.groupby(['sene', 'dent', 'early termination']):
            subdata=subdata.sort_values(by=['O'])

            linewidth = 1.5
            markersize = 6
            linestyle= '-' #'-' if dp_mem_type=='global' else '--'
            opt_combination = (int(sene), int(dent), int(early_termination))
            if opt_combination in plot_labels:
                label = plot_labels[opt_combination]
                marker = markers[opt_combination]
                color = colors[opt_combination]
            else:
                continue

            ax.plot(subdata['O'], subdata['throughput'], marker=marker, linewidth=linewidth, markersize=markersize, label=label, linestyle=linestyle, color=color)

    ax.set_ylim([bottom, top*1.05])
    fig.set_size_inches(10, 3)
    centered_figlegend(fig, axs[1],
        ncol=4,
        handletextpad=0.08, columnspacing=1.5,
        framealpha=0, prop={'size': 12}
        )
    plt.savefig(output_path,
        bbox_inches = 'tight',
        pad_inches = 0,
        dpi=400)

def plot_darwin_scaling(path, output_path):
    data = pandas.read_csv(path,
        usecols=['algorithm', 'threads', 'thread_blocks', 'threads_per_block', 'arch', 'aligns/second'],
        dtype={'threads' : int, 'thread_blocks': int, 'threads_per_block' : int, 'aligns/second': float})

    data.rename(columns={'aligns/second':'throughput'}, inplace=True)

    fig, ax = plt.subplots()
    for (thread_blocks, threads_per_threadblock), subdata in data.groupby(['thread_blocks', 'threads_per_block']):
        subdata=subdata.sort_values(by=['threads'])

        color = None
        linestyle = None
        label = f"thread_blocks={thread_blocks} threads_per_block={threads_per_threadblock}"
        ax.plot(subdata['threads'], subdata['throughput'], label=label, linestyle=linestyle, color=color, linewidth=1)

    plt.legend()
    ax.set_ylabel('Alignments per Second')
    ax.set_xlabel('CPU Threads')

    plt.savefig(output_path)

def plot_baselines_threads(fig, ax, path, scrooge_path, have_ylabels=True):
    scrooge_sweep_data = pandas.read_csv(scrooge_path,
        usecols=['W', 'O', 'SENE', 'DENT', 'early termination', 'threads', 'aligns/second'],
        dtype={'threads' : int, 'threads': int, 'aligns/second': float})
    scrooge_sweep_data.rename(columns={'aligns/second':'throughput'}, inplace=True)

    genasm_indices = ~scrooge_sweep_data['SENE'] & ~scrooge_sweep_data['DENT'] & ~scrooge_sweep_data['early termination']
    genasm_data = scrooge_sweep_data[genasm_indices]
    genasm_data['algorithm'] = 'genasm_cpu'
    genasm_data = genasm_data[['algorithm', 'threads', 'throughput']]

    data = pandas.read_csv(path,
        usecols=['algorithm', 'threads', 'aligns/second'],
        dtype={'threads' : int, 'threads': int, 'aligns/second': float})
    data.rename(columns={'aligns/second':'throughput'}, inplace=True)
    data=data[data['algorithm'] != 'wfa_lm']
    data.loc[data['algorithm']=='genasm_cpu', 'algorithm'] = 'scrooge'

    data = pandas.concat([data, genasm_data])

    data=data[data['threads'] <= 48]
    indices = data['threads'].isin(list(range(4, 48+1, 4))) 
    data=data[indices]

    def c(r255,g255,b255):
        return tuple((component/255 for component in (r255,g255,b255)))

    plot_labels = {
        'genasm_cpu': 'GenASM',
        'scrooge': 'Scrooge (Full)',
        'edlib': 'Edlib',
        'ksw2_extz': 'KSW2 extz',
        'ksw2_extz2_sse': 'KSW2 extz2_sse',
        'wfa_exact': 'WFA exact',
        'wfa_adaptive': 'WFA adaptive',
        'wfa_lm': 'WFA lm'
    }
    markers = {
        'genasm_cpu': 'X',
        'scrooge': 'o',
        'edlib': '^',
        'ksw2_extz': 'v',
        'ksw2_extz2_sse': 'x',
        'wfa_exact': 4,
        'wfa_adaptive': 5,
        'wfa_lm': '+'
    }
    colors = {
        'genasm_cpu': c(117,0,0),
        'scrooge': ours_color,#c(117,112,179),
        'edlib': c(217,95,2),
        'ksw2_extz': c(27,100,70),
        'ksw2_extz2_sse': c(27,158,119),
        'wfa_exact': c(100, 100, 100),
        'wfa_adaptive': c(0, 0, 0),
        'wfa_lm': c(150, 150, 150)
    }

    alg_order = {
        'genasm_cpu': 6,
        'scrooge': 7,
        'edlib': 5,
        'ksw2_extz': 3,
        'ksw2_extz2_sse': 4,
        'wfa_exact': 2,
        'wfa_adaptive': 1,
        'wfa_lm': 0
    }

    left, right = min(data['threads']), max(data['threads'])
    x_granularity = 8
    xticks=[left]+list(range(x_granularity, right+1, x_granularity))
    ax.set_xticks(ticks=xticks, labels=[f'{x}' for x in xticks], fontsize=14)
    ax.set_xticks(ticks=data['threads'].unique(), minor=True)
    ax.tick_params(axis='both', which='major', pad=1)
    #ax.set_xticks(ticks=data['threads'].unique())

    prepare_plot(fig, ax, [1, 2, 3, 4, 5], left, right, yminor=np.arange(1.0, 5.6, 0.2), have_labels=have_ylabels, yticks_fontsize=14)
    #ax.set_yscale('log')

    for algorithm, subdata in sorted(data.groupby(['algorithm']), key=lambda x: alg_order[x[0]]):
        subdata=subdata.groupby('threads').mean().reset_index()
        subdata=subdata.sort_values(by='threads')

        color = colors[algorithm]
        linestyle = None
        label = plot_labels[algorithm]
        marker = markers[algorithm]
        linewidth=2
        ax.plot(subdata['threads'], subdata['throughput']/min(subdata['throughput']), label=label, linestyle=linestyle, color=color, linewidth=linewidth, marker=marker)

    ax.set_xlabel('\\textbf{CPU Threads}', size=14, labelpad=1)
    if have_ylabels:
        ax.set_ylabel('\\textbf{Speedup over 4 Threads}', size=14, labelpad=1)

def plot_baselines_threads_paper(pbsim_path, illumina_path, pbsim_scrooge_path, illumina_scrooge_path, output_path):
    plt.rcParams['text.usetex'] = True
    #font = r'\fontfamily{qtm}\selectfont'
    font = ''

    fig, axs = plt.subplots(1, 2, constrained_layout=True)
    
    plot_baselines_threads(fig, axs[0], pbsim_path, pbsim_scrooge_path)
    plot_baselines_threads(fig, axs[1], illumina_path, illumina_scrooge_path, have_ylabels=False)
    #corner_box_text(axs[0], 0, 1, f'{font} \\textbf{{(a)}} {lr_name}', size=12.5)
    #corner_box_text(axs[1], 0, 1, f'{font} \\textbf{{(b)}} {sr_name}', size=12.5)
    corner_box_text(axs[0], 0, 1, f'{font} {lr_name}', size=14)
    corner_box_text(axs[1], 0, 1, f'{font} {sr_name}', size=14)

    fig.set_size_inches(10, 3)
    centered_figlegend(fig, axs[0],
        ncol=7,
        handletextpad=0.08, columnspacing=1.0,
        framealpha=0, prop={'size': 14},
        handlelength=1, #handleheight=2.5
        )

    plt.savefig(output_path,
        bbox_inches = 'tight',
        pad_inches = 0,
        dpi=400)

def get_performance(gpu_path, darwin_path, cudaswpp_path, cpu_path, baselines_path):
    performance = {}

    gpu_data = pandas.read_csv(gpu_path, dtype={'sene': bool, 'dent': bool, 'early termination': bool})
    gpu_data.rename(columns={'throughput (aligns/s)':'throughput'}, inplace=True)
    performance['scrooge_gpu'] = gpu_data.query('sene==True').query('dent==True').query('`early termination`==True').query('`dp memory type`=="shared"').query('W==64').query('`threadblocks/sm`==28')['throughput'].mean()
    performance['genasm_gpu'] = gpu_data.query('sene==False').query('dent==False').query('`early termination`==False').query('W==64').query('`threadblocks/sm`==28')['throughput'].mean()

    darwin_data = pandas.read_csv(darwin_path)
    darwin_data.rename(columns={'aligns/second':'throughput'}, inplace=True)
    performance['darwin_gpu'] = darwin_data.query('thread_blocks==252').query('threads_per_block==64')['throughput'].mean()

    cudaswpp_data = pandas.read_csv(cudaswpp_path)
    cudaswpp_data.rename(columns={'aligns/second':'throughput'}, inplace=True)
    performance['cudaswpp'] = cudaswpp_data['throughput'].mean()

    cpu_data = pandas.read_csv(cpu_path, dtype={'SENE': bool, 'DENT': bool, 'early termination': bool})
    cpu_data.rename(columns={'aligns/second':'throughput'}, inplace=True)
    performance['scrooge_cpu'] = cpu_data.query('SENE==True').query('DENT==False').query('`early termination`==True').query('W==64').query('threads==48')['throughput'].mean()
    performance['genasm_cpu'] = cpu_data.query('SENE==False').query('DENT==False').query('`early termination`==False').query('W==64').query('threads==48')['throughput'].mean()

    baselines_data = pandas.read_csv(baselines_path)
    baselines_data.rename(columns={'aligns/second':'throughput'}, inplace=True)
    performance['ksw2_extz'] = baselines_data.query('algorithm=="ksw2_extz"').query('threads==48')['throughput'].mean()
    performance['ksw2_extz2_sse'] = baselines_data.query('algorithm=="ksw2_extz2_sse"').query('threads==48')['throughput'].mean()
    performance['edlib'] = baselines_data.query('algorithm=="edlib"').query('threads==48')['throughput'].mean()
    performance['wfa_exact'] = baselines_data.query('algorithm=="wfa_exact"').query('threads>=48')['throughput'].mean()
    performance['wfa_adaptive'] = baselines_data.query('algorithm=="wfa_adaptive"').query('threads==48')['throughput'].mean()
    performance['wfa_lm'] = baselines_data.query('algorithm=="wfa_lm"').query('threads==48')['throughput'].mean()

    return performance

def get_performance_sr(gpu_path, cudaswpp_path, cpu_path, baselines_path):
    performance = {}

    gpu_data = pandas.read_csv(gpu_path, dtype={'sene': bool, 'dent': bool, 'early termination': bool})
    gpu_data.rename(columns={'throughput (aligns/s)':'throughput'}, inplace=True)
    performance['scrooge_gpu'] = gpu_data.query('W==32').query('O==17').query('sene==True').query('dent==False').query('`early termination`==True').query('`dp memory type`=="shared"')['throughput'].mean()
    performance['genasm_gpu'] = gpu_data.query('W==32').query('O==17').query('sene==False').query('dent==False').query('`early termination`==False').query('`dp memory type`=="shared"')['throughput'].mean()

    cudaswpp_data = pandas.read_csv(cudaswpp_path)
    cudaswpp_data.rename(columns={'aligns/second':'throughput'}, inplace=True)
    performance['cudaswpp'] = cudaswpp_data['throughput'].mean()

    cpu_data = pandas.read_csv(cpu_path)
    cpu_data.rename(columns={'aligns/second':'throughput'}, inplace=True)
    performance['scrooge_cpu'] = cpu_data.query('W==32').query('O==17').query('SENE==True').query('DENT==False').query('`early termination`==True')['throughput'].mean()
    performance['genasm_cpu'] = cpu_data.query('W==32').query('O==17').query('SENE==False').query('DENT==False').query('`early termination`==False')['throughput'].mean()

    baselines_data = pandas.read_csv(baselines_path)
    baselines_data.rename(columns={'aligns/second':'throughput'}, inplace=True)
    performance['ksw2_extz'] = baselines_data.query('algorithm=="ksw2_extz"').query('threads==48')['throughput'].mean()
    performance['ksw2_extz2_sse'] = baselines_data.query('algorithm=="ksw2_extz2_sse"').query('threads==48')['throughput'].mean()
    performance['edlib'] = baselines_data.query('algorithm=="edlib"').query('threads==48')['throughput'].mean()
    performance['wfa_exact'] = baselines_data.query('algorithm=="wfa_exact"').query('threads==48')['throughput'].mean()
    performance['wfa_adaptive'] = baselines_data.query('algorithm=="wfa_adaptive"').query('threads==48')['throughput'].mean()
    performance['wfa_lm'] = baselines_data.query('algorithm=="wfa_lm"').query('threads==48')['throughput'].mean()

    return performance

def plot_accuracy(path, output_path, ax=None):
    data = pandas.read_csv(path,
        usecols=['algorithm', 'pair_idx', 'score'],
        dtype={'pair_idx' : int, 'score' : 'Int64'})

    print(f'max pair_idx={max(data["pair_idx"])}')

    algorithms = data['algorithm'].unique()
    reshaped_subdatas = [subdata.drop(columns='algorithm').rename(columns={'score':alg}) for (alg, subdata) in data.groupby(['algorithm'])]
    joined = reshaped_subdatas[0]
    for subdata in reshaped_subdatas[1:]:
        joined = joined.merge(subdata, on='pair_idx', how='outer')
    data = joined.fillna(0)
    data.query('edlib!=0', inplace=True)
    data.query('ksw2_extz>0.0', inplace=True)

    for algorithm in algorithms:
        #data[f'{algorithm}_normalized'] = data[algorithm]/data['ksw2_extz']
        #data[f'{algorithm}_normalized'] = data[algorithm]/data['edlib']
        data[f'{algorithm}_normalized'] = (data[algorithm] - data['edlib'])/data['edlib']

    #data.query('genasm_cpu_normalized>=0.6', inplace=True)
    #data.sort_values(by=['genasm_cpu_normalized'], inplace=True)
    #data.sort_values(by=['edlib_normalized'], inplace=True)
    data.sort_values(by=['ksw2_extz'], inplace=True)

    #print(data)

    algorithm_names = {
        'edlib': 'Edlib',
        'genasm_gpu': 'Scrooge GPU',
        'genasm_cpu': 'Scrooge',
        'darwin_gpu': 'Darwin GPU',
        'ksw2_extz': 'KSW2 extz'
    }

    own_fig = False
    if not ax:
        fig, ax = plt.subplots()
        own_fig=True

    max_x = 0
    for i, algorithm in enumerate(algorithms):
        if algorithm == 'ksw2_extz2_sse': continue
        #if algorithm == 'edlib': continue
        label = algorithm_names[algorithm]
        markersize = 4

        color = colors[i % len(colors)]

        subdata = data.copy()
        #subdata = subdata.sort_values(by=[f'{algorithm}_normalized'])
        subdata = subdata.sort_values(by=[f'{algorithm}'])
        #subdata.query(f'{algorithm}_normalized>0', inplace=True)
        #ax.scatter(list(range(1, len(data['pair_idx'])+1)), list(data[f'{algorithm}_normalized']), label=algorithm, color=color, s=1)
        #ax.plot(list(range(1, len(subdata['pair_idx'])+1)), list(subdata[f'{algorithm}_normalized']), label=algorithm, color=color, linewidth=1)
        #ax.scatter(list(range(1, len(subdata['pair_idx'])+1)), list(subdata[f'{algorithm}_normalized']), label=algorithm, s=0.5)
        #if algorithm == 'genasm_cpu':
        #    ax.scatter(list(range(1, len(subdata['pair_idx'])+1)), list(subdata[f'{algorithm}']), label=algorithm, s=0.5)
        #else:
        #    ax.plot(list(range(1, len(subdata['pair_idx'])+1)), list(subdata[f'{algorithm}']), label=algorithm, linewidth=1)
        ax.plot(list(range(1, len(subdata['pair_idx'])+1)), list(subdata[f'{algorithm}']), label=label, color=color, linewidth=1, markersize=markersize)
        max_x = max(max_x, len(subdata['pair_idx']))

    x_scale_threshold = 1000
    x_scale_scaling = 100
    def fwd(x):
        if x < x_scale_threshold:
            return x
        else:
            return (x - x_scale_threshold) / x_scale_scaling + x_scale_threshold

    def inv(x):
        if x < x_scale_threshold:
            return x
        else:
            return (x - x_scale_threshold) * x_scale_scaling + x_scale_threshold

    def fwds(xs):
        if len(xs) == 0: return xs
        return np.vectorize(fwd)(xs)

    def invs(xs):
        if len(xs) == 0: return xs
        return np.vectorize(inv)(xs)

    #ax.set_xscale('function', functions=(fwds, invs))

    #left_xres = 5
    #left_xticks = [max(1, x) for x in range(0, x_scale_threshold+1, 200)]##

    #right_xres = #5
    #right_xticks = [x for x in range(0, max_x, 50000) if x > x_scale_threshold]##

    #xticks = left_xticks + right_xticks
    #xlabels = [f'{x}' if x < 1000 else f'{x//1000}e3' for x in xticks]
    #xlabels = [f'{x:,}' for x in xticks]
    #ax.set_xticks(xticks, xlabels)

    y_granularity = 5000
    bottom = math.ceil(min(data['genasm_cpu']) / y_granularity) * y_granularity
    top = math.ceil(max(data['genasm_cpu']) / y_granularity) * y_granularity
    yticks = [y for y in range(bottom, top+1, y_granularity)]
    ylabels = [f'{y:,}' for y in yticks]
    ax.set_yticks(yticks, ylabels)

    ax.grid(True)
    
    #ax.set_ylim([0.7, 1.05])
    #ax.set_ylim([0.95, 1.05])
    #ax.set_ylim([0, 100])
    #ax.set_ylim([0.0, 0.1])
    #ax.set_ylim([0.0, 0.002])

    ax.set_xlabel('Input Pairs')
    ax.set_ylabel('Alignment Score')

    ax.legend()

    if own_fig:
        plt.savefig(output_path,
            bbox_inches = 'tight',
            pad_inches = 0,
            dpi=400)

def plot_accuracy_WO(path, all_accuracy_path, ax=None):
    data = pandas.read_csv(path,
        usecols=['W', 'O', 'pair_idx', 'score'],
        dtype={'W' : int, 'O' : int, 'pair_idx' : int, 'score' : 'Int64'})

    all_accuracy_data = pandas.read_csv(all_accuracy_path,
        usecols=['algorithm', 'pair_idx', 'score'],
        dtype={'pair_idx' : int, 'score' : 'Int64'})

    edlib_raw_data = all_accuracy_data.query('algorithm=="edlib"')
    bad_pair_indices = edlib_raw_data.query('score==0')['pair_idx']
    edlib_raw_data.drop(edlib_raw_data[edlib_raw_data.pair_idx.isin(bad_pair_indices)].index, inplace=True)
    edlib = {
        'median': edlib_raw_data['score'].median(),
        '10%': edlib_raw_data['score'].quantile(0.1),
        '1%': edlib_raw_data['score'].quantile(0.01),
        '0.1%': edlib_raw_data['score'].quantile(0.001),
    }

    statistics = []
    for (W, O), subdata in data.groupby(['W', 'O']):
        subdata.drop(subdata[subdata.pair_idx.isin(bad_pair_indices)].index, inplace=True)
        statistics.append({
            'W': W,
            'O': O,
            'median': subdata['score'].median(),
            'mean': subdata['score'].mean(),
            'min': subdata['score'].min(),
            '10%' : subdata['score'].quantile(0.1),
            '1%' : subdata['score'].quantile(0.01),
            '0.1%' : subdata['score'].quantile(0.001)
        })
    statistics = pandas.DataFrame(statistics, columns=['W', 'O', 'median', 'mean', 'min', '10%', '1%', '0.1%'])

    def c(r255,g255,b255):
        return tuple((component/255 for component in (r255,g255,b255)))
    colors = [
        [min(1.0*e, 1.0) for e in ours_color],#c(0,90,50),
        [min(1.2*e, 1.0) for e in ours_color],#c(35,139,69),
        [min(1.4*e, 1.0) for e in ours_color],#c(65,171,93),
        [min(1.6*e, 1.0) for e in ours_color],#c(116,196,118),
        c(153,0,13),
        c(203,24,29),
        c(239,59,44),
        c(251,106,74)
    ]

    max_x = 0

    own_fig = False
    if not ax:
        fig, ax = plt.subplots()
        own_fig=True

    #quantile_names = {
    #    'median' : 'median',
    #    '10%' :    '10th percentile',
    #    '1%' :     '1st percentile',
    #    '0.1%':    '0.1th percentile'
    #}
    quantile_names = {
        'median' : '0.5 (median)',
        '10%' :    '0.1',
        '1%' :     '0.01',
        '0.1%':    '0.001'
    }

    for i, quantile in enumerate(['median', '10%', '1%', '0.1%']):
        color = colors[i % len(colors)]
        marker = markers[i % len(markers)]
        linewidth = 1.5
        markersize = 6
        label = f'Scrooge {quantile_names[quantile]}'
        ax.plot(statistics['W'], statistics[quantile], label=label, color=color, marker=marker, markersize=markersize, linewidth=linewidth)

    min_x = min(statistics['W'])
    max_x = max(statistics['W'])
    left = min_x - (max_x - min_x)*0.05
    right= max_x + (max_x - min_x)*0.05

    for i, quantile in enumerate(['median', '10%', '1%', '0.1%']):
        linewidth = 1.5
        markersize = 6
        #color = (1.0, 0.75-0.25*i, 0.75-0.25*i,)
        color = colors[4+i]
        label = f'Edlib {quantile_names[quantile]}'
        ax.plot([left, right], [edlib[quantile]]*2, label=label, linestyle='--', color=color, markersize=markersize, linewidth=linewidth)

    x_values = list(statistics['W'].unique())
    x_major = [x_values[0]] + x_values[3::4]
    x_minor = x_values
    ax.set_xticks(x_major)
    ax.set_xticks(x_minor, minor=True)

    y_min = min(statistics['0.1%'])
    y_max = max(statistics['median'])
    y_range = (y_max - y_min)
    bottom, top = y_min - y_range*0.05, y_max + y_range*0.05

    y_granularity = 10**int(math.log10(y_range))
    y_major = list(
        range(
            y_granularity*math.floor(bottom/y_granularity),
            y_granularity*math.ceil(top/y_granularity),
            y_granularity
        )
    )
    ax.set_yticks(y_major, [f'{y:3,}' for y in y_major])
    ax.tick_params(axis='both', which='major', pad=1)
    ax.set_xlim([left, right])
    ax.set_ylim([bottom, top])

    ax.grid(True)

    ax.set_xlabel(r'\textbf{Window Size (W)}', size=14, labelpad=1)

    if own_fig:
        ax.legend()

        plt.savefig(f"{plots_dir}/accuracy_sweep_WO.{output_format}",
            bbox_inches = 'tight',
            pad_inches = 0,
            dpi=400)

def plot_accuracy_O(path, all_accuracy_path, ax=None):
    data = pandas.read_csv(path,
        usecols=['W', 'O', 'pair_idx', 'score'],
        dtype={'W' : int, 'O' : int, 'pair_idx' : int, 'score' : 'Int64'})

    all_accuracy_data = pandas.read_csv(all_accuracy_path,
        usecols=['algorithm', 'pair_idx', 'score'],
        dtype={'pair_idx' : int, 'score' : 'Int64'})

    edlib_raw_data = all_accuracy_data.query('algorithm=="edlib"')
    bad_pair_indices = edlib_raw_data.query('score==0')['pair_idx']
    edlib_raw_data.drop(edlib_raw_data[edlib_raw_data.pair_idx.isin(bad_pair_indices)].index, inplace=True)
    edlib = {
        'median': edlib_raw_data['score'].median(),
        '10%': edlib_raw_data['score'].quantile(0.1),
        '1%': edlib_raw_data['score'].quantile(0.01),
        '0.1%': edlib_raw_data['score'].quantile(0.001),
    }

    def c(r255,g255,b255):
        return tuple((component/255 for component in (r255,g255,b255)))
    colors = [
        [min(1.0*e, 1.0) for e in ours_color],#c(0,90,50),
        [min(1.2*e, 1.0) for e in ours_color],#c(35,139,69),
        [min(1.4*e, 1.0) for e in ours_color],#c(65,171,93),
        [min(1.6*e, 1.0) for e in ours_color],#c(116,196,118),
    ]

    statistics = []
    for (W, O), subdata in data.groupby(['W', 'O']):
        subdata.drop(subdata[subdata.pair_idx.isin(bad_pair_indices)].index, inplace=True)
        statistics.append({
            'W': W,
            'O': O,
            'median': subdata['score'].median(),
            'mean': subdata['score'].mean(),
            'min': subdata['score'].min(),
            '10%' : subdata['score'].quantile(0.1),
            '1%' : subdata['score'].quantile(0.01),
            '0.1%' : subdata['score'].quantile(0.001)
        })
    statistics = pandas.DataFrame(statistics, columns=['W', 'O', 'median', 'mean', 'min', '10%', '1%', '0.1%'])

    own_fig = False
    if not ax:
        fig, ax = plt.subplots()
        own_fig=True

    max_x = 0
    for i, (W, subdata) in enumerate(statistics.groupby(['W'])):
        color = colors[i % len(colors)]
        marker = markers[i % len(markers)]
        subdata.sort_values(by=[f'O'], inplace=True)
        linewidth = 1.5
        markersize = 6
        ax.plot(subdata['O'], subdata['1%'], label=f"W={W}", color=color, marker=marker, zorder=10-i, linewidth=linewidth, markersize=markersize)

    min_x = min(statistics['O'])
    max_x = max(statistics['O'])
    left = min_x - (max_x - min_x)*0.05
    right= max_x + (max_x - min_x)*0.05
    y_min = min(statistics['1%'])
    y_max = max(statistics['1%'])
    y_range = (y_max - y_min)
    ax.set_ylim([y_min - y_range*0.05, y_max + y_range*0.05])

    linewidth = 1.5
    markersize = 6
    label = f'Edlib'
    color = (1.0, 0.0, 0.0)
    ax.plot([left, right], [edlib['1%']]*2, label=label, linestyle='--', color=color, markersize=markersize, linewidth=linewidth, zorder=10)

    x_values = list(statistics['O'].unique())
    x_minor = x_values
    x_major = x_values[::4] + [max(x_values)]
    ax.set_xticks(x_major)
    ax.set_xticks(x_minor, minor=True)
    ax.set_xlim([left, right])

    y_min = min(statistics['1%'])
    y_max = max(statistics['1%'])
    y_range = (y_max - y_min)
    bottom, top = y_min - y_range*0.05, y_max + y_range*0.05

    y_granularity = 10**int(math.log10(y_range))
    y_major = list(
        range(
            y_granularity*math.floor(bottom/y_granularity),
            y_granularity*math.ceil(top/y_granularity),
            y_granularity
        )
    )
    ax.set_yticks(y_major, [f'{y:3,}' for y in y_major])
    ax.tick_params(axis='both', which='major', pad=1)
    ax.set_xlim([left, right])
    print(f'bottom={bottom} top={top} y_min={y_min} y_max={y_max} y_range={y_range}')
    ax.set_ylim([bottom, top])

    #y_granularity = 5000
    #bottom = math.ceil(min(statistics['1%']) / y_granularity) * y_granularity
    #top = math.floor(max(statistics['1%']) / y_granularity) * y_granularity
    #yticks = [y for y in range(bottom, top+1, y_granularity)]
    #ylabels = [f'{y:,}' for y in yticks]
    #ax.set_yticks(yticks, ylabels)

    ax.grid(True)
    ax.set_xlabel(r'\textbf{Window Overlap (O)}', size=14, labelpad=1)

    #fig.legend()

    if own_fig:
        plt.savefig(f"{plots_dir}/accuracy_sweep_O.{output_format}",
            bbox_inches = 'tight',
            pad_inches = 0,
            dpi=400)

def plot_accuracy_WO_both(long_reads_WO_path, long_reads_all_path, short_reads_WO_path, short_reads_all_path, output_path):
    plt.rcParams['text.usetex'] = True
    
    fig, axs = plt.subplots(1, 2, constrained_layout=True)

    plot_accuracy_WO(long_reads_WO_path, long_reads_all_path, ax=axs[0])
    plot_accuracy_WO(short_reads_WO_path, short_reads_all_path, ax=axs[1])

    for name, ax in zip([lrgt_name, sr_name], axs):
        corner_box_text(ax, 1, 0, name)

    axs[0].set_ylabel(r"\textbf{Alignment Score}", size=14, labelpad=1)

    fig.set_size_inches(10, 3)
    #plt.subplots_adjust(wspace=0.05)
    #fig.legend(*axs[0].get_legend_handles_labels(),
    #        bbox_to_anchor=(0.1, 0.87, 0.75, 0.1),
    #        #bbox_to_anchor=(0.5, 0.1),
    #        loc='lower center', ncol=4,
    #        #mode="expand",
    #        borderaxespad=0,
    #        frameon=False)
    centered_figlegend(fig, axs[0],
        ncol=4,
        handletextpad=0.08, columnspacing=1.5,
        framealpha=0, prop={'size': 13})

    plt.savefig(output_path,
        bbox_inches = 'tight',
        pad_inches = 0,
        dpi=400)

def plot_accuracy_O_both(long_reads_O_path, long_reads_all_path, short_reads_O_path, short_reads_all_path, output_path):
    plt.rcParams['text.usetex'] = True

#    fig, axs = plt.subplots(1, 2, constrained_layout=True)
    fig, axs = plt.subplots(1, 2)

    plot_accuracy_O(long_reads_O_path, long_reads_all_path, ax=axs[0])
    plot_accuracy_O(short_reads_O_path, short_reads_all_path, ax=axs[1])

    for name, ax in zip([lrgt_name, sr_name], axs):
        corner_box_text(ax, 1, 0, name)

    axs[0].set_ylabel(r"\textbf{1st Percentile Alignment Score}", size=14, labelpad=1)

    fig.set_size_inches(9, 3)
    #plt.subplots_adjust(wspace=0.15)
    #fig.legend(*axs[0].get_legend_handles_labels(),
    #        bbox_to_anchor=(0.1, 0.88, 0.8, 0.1),
    #        #bbox_to_anchor=(0.5, 0.1),
    #        loc='lower center', ncol=5,
    #        #mode="expand",
    #        borderaxespad=0,
    #        frameon=False)

    centered_figlegend(fig, axs[0],
        ncol=5,
        handletextpad=0.08, columnspacing=1.5,
        framealpha=0, prop={'size': 13})
    
    fig.tight_layout()

    plt.savefig(output_path,
        bbox_inches = 'tight',
        pad_inches = 0.1,
        dpi=400)

def plot_performance_bars(performance, ax, bottom, top):
    algorithms = list(performance.keys())
    throughputs = [performance[a] for a in algorithms]
    df = pandas.DataFrame({'algorithm': algorithms, 'throughput':throughputs})

    df.loc[df['algorithm']=='wfa_lm',         'label'] = 'WFA lm\n'
    df.loc[df['algorithm']=='wfa_exact',      'label'] = 'WFA exact\n'
    df.loc[df['algorithm']=='wfa_adaptive',   'label'] = 'WFA\nadaptive'
    df.loc[df['algorithm']=='edlib',          'label'] = 'Edlib'
    df.loc[df['algorithm']=='scrooge_gpu',    'label'] = 'Scrooge'
    df.loc[df['algorithm']=='scrooge_cpu',    'label'] = 'Scrooge'
    df.loc[df['algorithm']=='genasm_gpu',     'label'] = 'GenASM'
    df.loc[df['algorithm']=='genasm_cpu',     'label'] = 'GenASM'
    df.loc[df['algorithm']=='darwin_gpu',     'label'] = 'Darwin'
    df.loc[df['algorithm']=='ksw2_extz',      'label'] = 'KSW2\nextz'
    df.loc[df['algorithm']=='ksw2_extz2_sse', 'label'] = 'KSW2\nextz2_sse'
    df.loc[df['algorithm']=='cudaswpp',       'label'] = 'CUDASW++3.0\n'

    df.loc[df['algorithm']=='wfa_lm',         'x_pos'] =-2.0
    df.loc[df['algorithm']=='wfa_exact',      'x_pos'] =-1.0
    df.loc[df['algorithm']=='wfa_adaptive',   'x_pos'] = 0.0
    df.loc[df['algorithm']=='ksw2_extz',      'x_pos'] = 1.0
    df.loc[df['algorithm']=='ksw2_extz2_sse', 'x_pos'] = 2.0
    df.loc[df['algorithm']=='edlib',          'x_pos'] = 3.0
    df.loc[df['algorithm']=='genasm_cpu',     'x_pos'] = 4.0
    df.loc[df['algorithm']=='scrooge_cpu',    'x_pos'] = 5.0
    df.loc[df['algorithm']=='cudaswpp',       'x_pos'] = 7
    df.loc[df['algorithm']=='darwin_gpu',     'x_pos'] = 8
    df.loc[df['algorithm']=='genasm_gpu',     'x_pos'] = 9
    df.loc[df['algorithm']=='scrooge_gpu',    'x_pos'] = 10

    df.loc[df['algorithm']=='wfa_lm',         'color'] = 'baseline2'
    df.loc[df['algorithm']=='wfa_exact',      'color'] = 'baseline2'
    df.loc[df['algorithm']=='wfa_adaptive',   'color'] = 'baseline2'
    df.loc[df['algorithm']=='ksw2_extz',      'color'] = 'baseline2' 
    df.loc[df['algorithm']=='ksw2_extz2_sse', 'color'] = 'baseline2'
    df.loc[df['algorithm']=='edlib',          'color'] = 'baseline'
    df.loc[df['algorithm']=='genasm_cpu',     'color'] = 'baseline'
    df.loc[df['algorithm']=='scrooge_cpu',    'color'] = 'ours'
    df.loc[df['algorithm']=='darwin_gpu',     'color'] = 'baseline2'
    df.loc[df['algorithm']=='cudaswpp',       'color'] = 'baseline2'
    df.loc[df['algorithm']=='genasm_gpu',     'color'] = 'baseline'
    df.loc[df['algorithm']=='scrooge_gpu',    'color'] = 'ours'

    df.loc[df['algorithm']=='wfa_lm',         'problem'] = 'smith_waterman'
    df.loc[df['algorithm']=='wfa_exact',      'problem'] = 'smith_waterman'
    df.loc[df['algorithm']=='wfa_adaptive',   'problem'] = 'smith_waterman'
    df.loc[df['algorithm']=='ksw2_extz',      'problem'] = 'smith_waterman'
    df.loc[df['algorithm']=='ksw2_extz2_sse', 'problem'] = 'smith_waterman'
    df.loc[df['algorithm']=='edlib',          'problem'] = 'edit_distance'
    df.loc[df['algorithm']=='scrooge_cpu',    'problem'] = 'edit_distance'
    df.loc[df['algorithm']=='darwin_gpu',     'problem'] = 'smith_waterman'
    df.loc[df['algorithm']=='cudaswpp',       'problem'] = 'smith_waterman'
    df.loc[df['algorithm']=='scrooge_gpu',    'problem'] = 'edit_distance'
    df.loc[df['algorithm']=='genasm_cpu',     'problem'] = 'edit_distance'
    df.loc[df['algorithm']=='genasm_gpu',     'problem'] = 'edit_distance'

    major_ys = [10**i for i in range(8)]
    minor_ys = []
    for major_y in major_ys:
        minor_ys += [major_y * i for i in range(1, 10, 1)]

    #grid
    ax.set_axisbelow(True)

    #y axis
    ax.set_yscale('log')
    ax.set_yticks(major_ys)
    ax.set_yticks(minor_ys, minor=True)
    ax.set_ylim([bottom, top])
    ax.set_ylabel("alignments per second")

    #x axis
    ax.set_xticks([1.5, 8.5], labels=['2x Intel Xeon Gold 5118', 'NVIDIA A6000'])
    ax.tick_params(axis='x', which='both', length=0)

    #plot
    plt.rcParams['hatch.linewidth'] = 5
    for i, row in df.iterrows():
        hatch = '/' if row['problem']=='smith_waterman' else ''
        if row['color']=='baseline':
            color = baseline_color
        if row['color']=='baseline2':
            color = baseline2_color
        if row['color']=='ours':
            color = ours_color
        ax.bar([row['x_pos']], [row['throughput']], width=1, edgecolor='black', color=color)

    for name, x_pos in zip(df['label'], df['x_pos']):
        ax.text(
            x_pos, bottom*1.2,
            name,
            ha="left", va="center", rotation=90, rotation_mode='anchor',
            size=12, weight='bold', family='sans-serif', linespacing = 0.75,)

    arrows = [
        ('wfa_lm', 'scrooge_cpu'),
        ('wfa_exact', 'scrooge_cpu'),
        ('wfa_adaptive', 'scrooge_cpu'),
        ('ksw2_extz', 'scrooge_cpu'),
        ('ksw2_extz2_sse', 'scrooge_cpu'),
        ('edlib', 'scrooge_cpu'),
        ('genasm_cpu', 'scrooge_cpu'),
        ('scrooge_cpu', 'scrooge_gpu'),
        ('genasm_gpu', 'scrooge_gpu'),
        ('darwin_gpu', 'scrooge_gpu'),
        ('cudaswpp', 'scrooge_gpu')
    ]

    for improvement_ceil in ['scrooge_cpu', 'scrooge_gpu']:
        relevant_arrows = [(base, ceil) for (base, ceil) in arrows if ceil == improvement_ceil]
        relevant_xs = [df.query(f'algorithm=="{base}"')['x_pos'].iloc[0] for (base, ceil) in relevant_arrows]
        first_base_x = min(relevant_xs)
        ceil_x = df.query(f'algorithm=="{improvement_ceil}"')['x_pos'].iloc[0]
        y = df.query(f'algorithm=="{improvement_ceil}"')['throughput'].iloc[0]
        ax.plot([first_base_x, ceil_x], [y]*2, linewidth=0.5, linestyle='--', color=ours_improvement_color, zorder=-100)

    for algorithm, improvement_ceil in arrows:
        alg_tput = df.query(f'algorithm=="{algorithm}"')['throughput'].iloc[0]
        ceil_tput = df.query(f'algorithm=="{improvement_ceil}"')['throughput'].iloc[0]
        y_base = max(bottom, alg_tput)
        improvement = ceil_tput/alg_tput
        x = df.query(f'algorithm=="{algorithm}"')['x_pos'].iloc[0]
        if y_base > bottom:
            ax.annotate("",
                        xytext=(x, y_base),
                        xy=(x, ceil_tput),
                        arrowprops=dict(arrowstyle="->", linewidth=2, shrinkA=0, shrinkB=0, color=ours_improvement_color))
        else:
            dash_start = bottom*2
            ax.annotate("",
                        xytext=(x, dash_start),
                        xy=(x, ceil_tput),
                        arrowprops=dict(arrowstyle="->", linewidth=2, shrinkA=0, shrinkB=0, color=ours_improvement_color))
            ax.plot([x]*2, [bottom, dash_start],
                color = ours_improvement_color, linestyle=':',
                linewidth=2)
        ax.annotate(f"{improvement:.1f}x",
                    xy=(x, ceil_tput),
                    ha='left', va='bottom',
                    xytext=(0, 0),
                    textcoords='offset points',
                    family='sans-serif', weight='bold',
                    rotation=45, rotation_mode='anchor',
                    color=ours_improvement_color)

def plot_performance_bars_sr(performance, ax, bottom, top):
    algorithms = list(performance.keys())
    throughputs = [performance[a] for a in algorithms]
    df = pandas.DataFrame({'algorithm': algorithms, 'throughput':throughputs})


    df.loc[df['algorithm']=='wfa_lm',         'label'] = 'WFA lm\n'
    df.loc[df['algorithm']=='wfa_exact',      'label'] = 'WFA\nexact'
    df.loc[df['algorithm']=='wfa_adaptive',   'label'] = 'WFA\nadaptive'
    df.loc[df['algorithm']=='edlib',          'label'] = 'Edlib'
    df.loc[df['algorithm']=='scrooge_gpu',    'label'] = 'Scrooge'
    df.loc[df['algorithm']=='scrooge_cpu',    'label'] = 'Scrooge'
    df.loc[df['algorithm']=='genasm_gpu',     'label'] = 'GenASM'
    df.loc[df['algorithm']=='genasm_cpu',     'label'] = 'GenASM'
    #df.loc[df['algorithm']=='darwin_gpu',     'label'] = 'Darwin'
    df.loc[df['algorithm']=='ksw2_extz',      'label'] = 'KSW2\nextz'
    df.loc[df['algorithm']=='ksw2_extz2_sse', 'label'] = 'KSW2\nextz2_sse'
    df.loc[df['algorithm']=='cudaswpp',       'label'] = 'CUDASW++3.0'

    df.loc[df['algorithm']=='wfa_lm',         'x_pos'] =-2.0
    df.loc[df['algorithm']=='wfa_exact',      'x_pos'] =-1.0
    df.loc[df['algorithm']=='wfa_adaptive',   'x_pos'] = 0.0
    df.loc[df['algorithm']=='ksw2_extz',      'x_pos'] = 1.0
    df.loc[df['algorithm']=='ksw2_extz2_sse', 'x_pos'] = 2.0
    df.loc[df['algorithm']=='edlib',          'x_pos'] = 3.0
    df.loc[df['algorithm']=='genasm_cpu',     'x_pos'] = 4.0
    df.loc[df['algorithm']=='scrooge_cpu',    'x_pos'] = 5.0
    df.loc[df['algorithm']=='cudaswpp',       'x_pos'] = 7
    df.loc[df['algorithm']=='genasm_gpu',     'x_pos'] = 8
    df.loc[df['algorithm']=='scrooge_gpu',    'x_pos'] = 9

    df.loc[df['algorithm']=='wfa_lm',         'color'] = 'baseline2'
    df.loc[df['algorithm']=='wfa_exact',      'color'] = 'baseline2'
    df.loc[df['algorithm']=='wfa_adaptive',   'color'] = 'baseline2'
    df.loc[df['algorithm']=='ksw2_extz',      'color'] = 'baseline2' 
    df.loc[df['algorithm']=='ksw2_extz2_sse', 'color'] = 'baseline2'
    df.loc[df['algorithm']=='edlib',          'color'] = 'baseline'
    df.loc[df['algorithm']=='genasm_cpu',     'color'] = 'baseline'
    df.loc[df['algorithm']=='scrooge_cpu',    'color'] = 'ours'
    #df.loc[df['algorithm']=='darwin_gpu',     'color'] = 'baseline2'
    df.loc[df['algorithm']=='genasm_gpu',     'color'] = 'baseline'
    df.loc[df['algorithm']=='scrooge_gpu',    'color'] = 'ours'
    df.loc[df['algorithm']=='cudaswpp',       'color'] = 'baseline2'

    df.loc[df['algorithm']=='wfa_lm',         'problem'] = 'smith_waterman'
    df.loc[df['algorithm']=='wfa_exact',      'problem'] = 'smith_waterman'
    df.loc[df['algorithm']=='wfa_adaptive',   'problem'] = 'smith_waterman'
    df.loc[df['algorithm']=='ksw2_extz',      'problem'] = 'smith_waterman'
    df.loc[df['algorithm']=='ksw2_extz2_sse', 'problem'] = 'smith_waterman'
    df.loc[df['algorithm']=='edlib',          'problem'] = 'edit_distance'
    df.loc[df['algorithm']=='scrooge_cpu',    'problem'] = 'edit_distance'
    #df.loc[df['algorithm']=='darwin_gpu',     'problem'] = 'smith_waterman'
    df.loc[df['algorithm']=='scrooge_gpu',    'problem'] = 'edit_distance'
    df.loc[df['algorithm']=='genasm_cpu',     'problem'] = 'edit_distance'
    df.loc[df['algorithm']=='genasm_gpu',     'problem'] = 'edit_distance'
    df.loc[df['algorithm']=='cudaswpp',       'problem'] = 'smith_waterman'

    major_ys = [10**i for i in range(8)]
    minor_ys = []
    for major_y in major_ys:
        minor_ys += [major_y * i for i in range(1, 10, 1)]

    #grid
    ax.set_axisbelow(True)

    #y axis
    ax.set_yscale('log')
    ax.set_yticks(major_ys)
    ax.set_yticks(minor_ys, minor=True)
    ax.set_ylim([bottom, top])
    ax.set_ylabel("alignments per second")

    #x axis
    ax.set_xticks([1.5, 8], labels=['2x Intel Xeon Gold 5118', 'NVIDIA A6000'])
    ax.tick_params(axis='x', which='both', length=0)

    #plot
    plt.rcParams['hatch.linewidth'] = 5
    for i, row in df.iterrows():
        hatch = '/' if row['problem']=='smith_waterman' else ''
        if row['color']=='baseline':
            color = baseline_color
        if row['color']=='baseline2':
            color = baseline2_color
        if row['color']=='ours':
            color = ours_color
        ax.bar([row['x_pos']], [row['throughput']], width=1, edgecolor='black', color=color)

    for name, x_pos in zip(df['label'], df['x_pos']):
        #ax.text(x_pos, bottom*1.05, name, ha="center", va="bottom", rotation=90, size=13, weight='bold', family='sans-serif')
        ax.text(x_pos, bottom*1.05,
            name,
            ha="left", va="center", rotation=90, rotation_mode='anchor',
            size=12, weight='bold', family='sans-serif', linespacing = 0.75,)

    arrows = [
        ('wfa_lm', 'scrooge_cpu'),
        ('wfa_exact', 'scrooge_cpu'),
        ('wfa_adaptive', 'scrooge_cpu'),
        ('ksw2_extz', 'scrooge_cpu'),
        ('ksw2_extz2_sse', 'scrooge_cpu'),
        ('edlib', 'scrooge_cpu'),
        ('genasm_cpu', 'scrooge_cpu'),
        ('scrooge_cpu', 'scrooge_gpu'),
        ('genasm_gpu', 'scrooge_gpu'),
        ('cudaswpp', 'scrooge_gpu')
    ]

    for improvement_ceil in ['scrooge_cpu', 'scrooge_gpu']:
        relevant_arrows = [(base, ceil) for (base, ceil) in arrows if ceil == improvement_ceil]
        relevant_xs = [df.query(f'algorithm=="{base}"')['x_pos'].iloc[0] for (base, ceil) in relevant_arrows]
        first_base_x = min(relevant_xs)
        ceil_x = df.query(f'algorithm=="{improvement_ceil}"')['x_pos'].iloc[0]
        y = df.query(f'algorithm=="{improvement_ceil}"')['throughput'].iloc[0]
        ax.plot([first_base_x, ceil_x], [y]*2, linewidth=0.5, linestyle='--', color=ours_improvement_color, zorder=-100)

    for algorithm, improvement_ceil in arrows:
        alg_tput = df.query(f'algorithm=="{algorithm}"')['throughput'].iloc[0]
        ceil_tput = df.query(f'algorithm=="{improvement_ceil}"')['throughput'].iloc[0]
        y_base = max(bottom, alg_tput)
        improvement = ceil_tput/alg_tput
        x = df.query(f'algorithm=="{algorithm}"')['x_pos'].iloc[0]
        if y_base > bottom:
            ax.annotate("",
                        xytext=(x, y_base),
                        xy=(x, ceil_tput),
                        arrowprops=dict(arrowstyle="->", linewidth=2, shrinkA=0, shrinkB=0, color=ours_improvement_color))
        else:
            dash_start = bottom*1.5
            ax.annotate("",
                        xytext=(x, dash_start),
                        xy=(x, ceil_tput),
                        arrowprops=dict(arrowstyle="->", linewidth=2, shrinkA=0, shrinkB=0, color=ours_improvement_color))
            ax.plot([x]*2, [bottom, dash_start],
                color=ours_improvement_color, linestyle=':',
                linewidth=2)
        ax.annotate(f"{improvement:.1f}x",
                    xy=(x, ceil_tput),
                    ha='left', va='bottom',
                    xytext=(0, 0),
                    textcoords='offset points',
                    family='sans-serif', weight='bold',
                    rotation=45, rotation_mode='anchor',
                    color=ours_improvement_color)

def plot_performance_bars_both(long_gpu_path, long_darwin_path, long_cudaswpp_path, long_cpu_path, long_baselines_path, short_gpu_path, short_cudaswpp_path, short_cpu_path, short_baselines_path, output_path):
    fig, axs = plt.subplots(1, 2, constrained_layout=True)

    long_performance = get_performance(long_gpu_path, long_darwin_path, long_cudaswpp_path, long_cpu_path, long_baselines_path)
    plot_performance_bars(long_performance, axs[0], 4.1*10**1, 3*10**5)
    short_performance = get_performance_sr(short_gpu_path, short_cudaswpp_path, short_cpu_path, short_baselines_path)
    #plot_performance_bars_sr(short_performance, ax=axs[1])
    plot_performance_bars_sr(short_performance, axs[1], 2.3*10**5, 2*10**7)

    for name, ax in zip(['Long Reads', 'Short Reads'], axs):
        corner_box_text(ax, 0, 1, name)

    axs[0].set_ylabel("Alignments per Second")
    axs[1].set_ylabel("")

    fig.set_size_inches(10, 3.5)
    #fig.subplots_adjust(wspace=0.13)

    plt.rcParams['hatch.linewidth'] = 2
    labels = ['Baseline\nAffine Gap Cost', 'Baseline\nEdit Distance', 'Scrooge\nEdit Distance']
    handles = [
        #plt.Rectangle((0,0),1,1, facecolor='none', hatch='///', edgecolor='black'),
        #plt.Rectangle((0,0),1,1, facecolor='none', edgecolor='black'),
        #plt.Rectangle((0,0),1,1, facecolor=baseline_color, edgecolor='black'),
        #plt.Rectangle((0,0),1,1, facecolor=ours_color, edgecolor='black'),
        #plt.Rectangle((0,0),1,1, facecolor=baseline_color, hatch='///', edgecolor=(0.8,0.8,0.8)),
        #plt.Rectangle((0,0),1,1, facecolor=baseline_color, hatch='///', edgecolor=(0,0,0)),
        plt.Rectangle((0,0),1,1, facecolor=baseline2_color, edgecolor='black'),
        plt.Rectangle((0,0),1,1, facecolor=baseline_color, edgecolor='black'),
        plt.Rectangle((0,0),1,1, facecolor=ours_color, edgecolor='black'),
        ]
    #fig.legend(handles, labels,
    #    bbox_to_anchor=(0.2, 0.94, 0.6, 0.1),
    #    loc='upper center', ncol=4,
    #    mode="expand",
    #    borderaxespad=1,
    #    framealpha=0,
    #    handlelength=3,
    #    handleheight=3,)
    centered_figlegend(fig, axs[0], handles_labels=(handles,labels),
        ncol=4,
        handletextpad=0.4, columnspacing=5,
        framealpha=0, prop={'size': 12},
        handlelength=2, handleheight=2.5)

    plt.savefig(output_path,
        bbox_inches = 'tight',
        pad_inches = 0,
        dpi=400)

def get_performance_error_bars(gpu_path, darwin_path, cudaswpp_path, cpu_path, baselines_path):
    performance = {}

    gpu_data = pandas.read_csv(gpu_path, dtype={'sene': bool, 'dent': bool, 'early termination': bool})
    gpu_data.rename(columns={'throughput (aligns/s)':'throughput'}, inplace=True)
    performance['scrooge_gpu'] = gpu_data.query('sene==True').query('dent==True').query('`early termination`==True').query('`dp memory type`=="shared"').query('W==64').query('`threadblocks/sm`==28')['throughput']
    performance['genasm_gpu'] = gpu_data.query('sene==False').query('dent==False').query('`early termination`==False').query('W==64').query('`threadblocks/sm`==28')['throughput']

    darwin_data = pandas.read_csv(darwin_path)
    darwin_data.rename(columns={'aligns/second':'throughput'}, inplace=True)
    performance['darwin_gpu'] = darwin_data.query('thread_blocks==252').query('threads_per_block==64')['throughput']

    cudaswpp_data = pandas.read_csv(cudaswpp_path)
    cudaswpp_data.rename(columns={'aligns/second':'throughput'}, inplace=True)
    performance['cudaswpp'] = cudaswpp_data['throughput']

    cpu_data = pandas.read_csv(cpu_path, dtype={'SENE': bool, 'DENT': bool, 'early termination': bool})
    cpu_data.rename(columns={'aligns/second':'throughput'}, inplace=True)
    performance['scrooge_cpu'] = cpu_data.query('SENE==True').query('DENT==False').query('`early termination`==True').query('W==64').query('threads==48')['throughput']
    performance['genasm_cpu'] = cpu_data.query('SENE==False').query('DENT==False').query('`early termination`==False').query('W==64').query('threads==48')['throughput']

    baselines_data = pandas.read_csv(baselines_path)
    baselines_data.rename(columns={'aligns/second':'throughput'}, inplace=True)
    performance['ksw2_extz'] = baselines_data.query('algorithm=="ksw2_extz"').query('threads==48')['throughput']
    performance['ksw2_extz2_sse'] = baselines_data.query('algorithm=="ksw2_extz2_sse"').query('threads==48')['throughput']
    performance['edlib'] = baselines_data.query('algorithm=="edlib"').query('threads==48')['throughput']
    performance['wfa_exact'] = baselines_data.query('algorithm=="wfa_exact"').query('threads>=48')['throughput']
    performance['wfa_adaptive'] = baselines_data.query('algorithm=="wfa_adaptive"').query('threads==48')['throughput']
    performance['wfa_lm'] = baselines_data.query('algorithm=="wfa_lm"').query('threads==48')['throughput']

    return performance

def get_performance_error_bars_sr(gpu_path, cudaswpp_path, cpu_path, baselines_path):
    performance = {}

    gpu_data = pandas.read_csv(gpu_path, dtype={'sene': bool, 'dent': bool, 'early termination': bool})
    gpu_data.rename(columns={'throughput (aligns/s)':'throughput'}, inplace=True)
    performance['scrooge_gpu'] = gpu_data.query('W==32').query('O==17').query('sene==True').query('dent==False').query('`early termination`==True').query('`dp memory type`=="shared"')['throughput']
    performance['genasm_gpu'] = gpu_data.query('W==32').query('O==17').query('sene==False').query('dent==False').query('`early termination`==False').query('`dp memory type`=="shared"')['throughput']

    cudaswpp_data = pandas.read_csv(cudaswpp_path)
    cudaswpp_data.rename(columns={'aligns/second':'throughput'}, inplace=True)
    performance['cudaswpp'] = cudaswpp_data['throughput']

    cpu_data = pandas.read_csv(cpu_path)
    cpu_data.rename(columns={'aligns/second':'throughput'}, inplace=True)
    performance['scrooge_cpu'] = cpu_data.query('W==32').query('O==17').query('SENE==True').query('DENT==False').query('`early termination`==True')['throughput']
    performance['genasm_cpu'] = cpu_data.query('W==32').query('O==17').query('SENE==False').query('DENT==False').query('`early termination`==False')['throughput']

    baselines_data = pandas.read_csv(baselines_path)
    baselines_data.rename(columns={'aligns/second':'throughput'}, inplace=True)
    performance['ksw2_extz'] = baselines_data.query('algorithm=="ksw2_extz"').query('threads==48')['throughput']
    performance['ksw2_extz2_sse'] = baselines_data.query('algorithm=="ksw2_extz2_sse"').query('threads==48')['throughput']
    performance['edlib'] = baselines_data.query('algorithm=="edlib"').query('threads==48')['throughput']
    performance['wfa_exact'] = baselines_data.query('algorithm=="wfa_exact"').query('threads==48')['throughput']
    performance['wfa_adaptive'] = baselines_data.query('algorithm=="wfa_adaptive"').query('threads==48')['throughput']
    performance['wfa_lm'] = baselines_data.query('algorithm=="wfa_lm"').query('threads==48')['throughput']

    return performance

def plot_performance_error_bars(performance, ax, bottom, top):
    algorithms = list(performance.keys())

    labels={
        'wfa_lm'         : 'WFA lm',
        'wfa_exact'      : 'WFA exact',
        'wfa_adaptive'   : 'WFA adaptive',
        'edlib'          : 'Edlib',
        'scrooge_gpu'    : 'Scrooge',
        'scrooge_cpu'    : 'Scrooge',
        'genasm_gpu'     : 'GenASM',
        'genasm_cpu'     : 'GenASM',
        'darwin_gpu'     : 'Darwin',
        'ksw2_extz'      : 'KSW2 extz',
        'ksw2_extz2_sse' : 'KSW2 extz2_sse',
        'cudaswpp'       : 'CUDASW++3.0'
    }

    x_pos = {
    'wfa_lm'         :-2.0,
    'wfa_exact'      :-1.0,
    'wfa_adaptive'   : 0.0,
    'ksw2_extz'      : 1.0,
    'ksw2_extz2_sse' : 2.0,
    'edlib'          : 3.0,
    'genasm_cpu'     : 4.0,
    'scrooge_cpu'    : 5.0,
    'cudaswpp'       : 6.5,
    'darwin_gpu'     : 7.5,
    'genasm_gpu'     : 8.5,
    'scrooge_gpu'    : 9.5
    }

    major_ys = [10**i for i in range(8)]
    minor_ys = []
    for major_y in major_ys:
        minor_ys += [major_y * i for i in range(1, 10, 1)]

    #grid
    ax.set_axisbelow(True)

    #y axis
    ax.set_yscale('log')
    ax.set_yticks(major_ys)
    ax.set_yticks(minor_ys, minor=True)
    ax.set_ylim([bottom, top])
    ax.set_ylabel("alignments per second")

    xticks = []
    xtick_labels = []

    #plot
    for algorithm in algorithms:
        x = x_pos[algorithm]
        ys:pandas.Series = performance[algorithm]
        print(algorithm, list(ys))
        singelebox = ax.boxplot(ys, positions=[x], widths=0.8, patch_artist=True, showfliers=False)
        singelebox['boxes'][0].set_facecolor((0.3, 0.8, 0.1))
        singelebox['medians'][0].set_color((1.0, 0.0, 0.0))
        xticks.append(x)
        xtick_labels.append(labels[algorithm])

    ax.set_xticks(xticks, xtick_labels, size=8, rotation=45, rotation_mode='anchor', ha='right', va='top')
    ax.grid(True)

    ax.axvline(5.75, color=(0,0,0))
    ax.text(x=0.33, y=0.03, s='2x Intel Xeon Gold 5118', ha='center', va='center', transform=ax.transAxes, size=11)
    ax.text(x=0.83, y=0.03, s='NVIDIA A6000', ha='center', va='center', transform=ax.transAxes, size=11)

def plot_performance_error_bars_sr(performance, ax, bottom, top):
    algorithms = list(performance.keys())

    labels={
        'wfa_lm'         : 'WFA lm',
        'wfa_exact'      : 'WFA exact',
        'wfa_adaptive'   : 'WFA adaptive',
        'edlib'          : 'Edlib',
        'scrooge_gpu'    : 'Scrooge',
        'scrooge_cpu'    : 'Scrooge',
        'genasm_gpu'     : 'GenASM',
        'genasm_cpu'     : 'GenASM',
        'darwin_gpu'     : 'Darwin',
        'ksw2_extz'      : 'KSW2 extz',
        'ksw2_extz2_sse' : 'KSW2 extz2_sse',
        'cudaswpp'       : 'CUDASW++3.0'
    }

    x_pos = {
    'wfa_lm'         :-2.0,
    'wfa_exact'      :-1.0,
    'wfa_adaptive'   : 0.0,
    'ksw2_extz'      : 1.0,
    'ksw2_extz2_sse' : 2.0,
    'edlib'          : 3.0,
    'genasm_cpu'     : 4.0,
    'scrooge_cpu'    : 5.0,
    'cudaswpp'       : 6.5,
    'genasm_gpu'     : 7.5,
    'scrooge_gpu'    : 8.5
    }

    major_ys = [10**i for i in range(8)]
    minor_ys = []
    for major_y in major_ys:
        minor_ys += [major_y * i for i in range(1, 10, 1)]

    #grid
    ax.set_axisbelow(True)

    #y axis
    ax.set_yscale('log')
    ax.set_yticks(major_ys)
    ax.set_yticks(minor_ys, minor=True)
    ax.set_ylim([bottom, top])
    ax.set_ylabel("alignments per second")

    xticks = []
    xtick_labels = []

    #plot
    for algorithm in algorithms:
        x = x_pos[algorithm]
        ys:pandas.Series = performance[algorithm]
        print(algorithm, list(ys))
        singelebox = ax.boxplot(ys, positions=[x], widths=0.8, patch_artist=True, showfliers=False)
        singelebox['boxes'][0].set_facecolor((0.3, 0.8, 0.1))
        singelebox['medians'][0].set_color((1.0, 0.0, 0.0))
        xticks.append(x)
        xtick_labels.append(labels[algorithm])

    ax.set_xticks(xticks, xtick_labels, size=8, rotation=45, rotation_mode='anchor', ha='right', va='top')
    ax.grid(True)
    ax.axvline(5.75, color=(0,0,0))
    ax.text(x=0.35, y=0.03, s='2x Intel Xeon Gold 5118', ha='center', va='center', transform=ax.transAxes, size=11)
    ax.text(x=0.86, y=0.03, s='NVIDIA A6000', ha='center', va='center', transform=ax.transAxes, size=11)

def plot_performance_error_bars_both(long_gpu_path, long_darwin_path, long_cudaswpp_path, long_cpu_path, long_baselines_path, short_gpu_path, short_cudaswpp_path, short_cpu_path, short_baselines_path, output_path):
    fig, axs = plt.subplots(1, 2, constrained_layout=True)

    long_performance = get_performance_error_bars(long_gpu_path, long_darwin_path, long_cudaswpp_path, long_cpu_path, long_baselines_path)
    plot_performance_error_bars(long_performance, axs[0], 5*10**0, 1*10**5)
    short_performance = get_performance_error_bars_sr(short_gpu_path, short_cudaswpp_path, short_cpu_path, short_baselines_path)
    plot_performance_error_bars_sr(short_performance, axs[1], 10**5, 2*10**7)

    for name, ax in zip(['PMSIM2 chained', 'Illumina chained'], axs):
        corner_box_text(ax, 0, 1, name)

    axs[0].set_ylabel("Alignments per Second")
    axs[1].set_ylabel("")

    fig.set_size_inches(10, 3.5)
    #fig.subplots_adjust(wspace=0.13)

    plt.rcParams['hatch.linewidth'] = 2
    labels = ['Baseline\nAffine Gap Cost', 'Baseline\nEdit Distance', 'Scrooge\nEdit Distance']
    handles = [
        #plt.Rectangle((0,0),1,1, facecolor='none', hatch='///', edgecolor='black'),
        #plt.Rectangle((0,0),1,1, facecolor='none', edgecolor='black'),
        #plt.Rectangle((0,0),1,1, facecolor=baseline_color, edgecolor='black'),
        #plt.Rectangle((0,0),1,1, facecolor=ours_color, edgecolor='black'),
        #plt.Rectangle((0,0),1,1, facecolor=baseline_color, hatch='///', edgecolor=(0.8,0.8,0.8)),
        #plt.Rectangle((0,0),1,1, facecolor=baseline_color, hatch='///', edgecolor=(0,0,0)),
        plt.Rectangle((0,0),1,1, facecolor=baseline2_color, edgecolor='black'),
        plt.Rectangle((0,0),1,1, facecolor=baseline_color, edgecolor='black'),
        plt.Rectangle((0,0),1,1, facecolor=ours_color, edgecolor='black'),
        ]

    plt.savefig(output_path,
        bbox_inches = 'tight',
        pad_inches = 0,
        dpi=400)
        
def plot_A6000_roofline_model(ax=None):
    GIGA = 1_000_000_000
    #A6000
    global_memory_bw = 768 * GIGA #GB/s
    shared_memory_bw = 19353.6 * GIGA #GB/s
    peak_compute_64 = 4838.4 * GIGA #Gops

    #algorithm
    operational_intensity = 0.29 # ops/B

    top = 2.68*peak_compute_64
    bottom = top/441.6/10
    left = 0.25*10**-2.5
    right = 10**0.6
    
    own_fig = False
    if ax is None:
        own_fig = True
        fig, ax = plt.subplots()

    l = 10**-6
    r = 10**6

    def c(r255,g255,b255):
        return tuple((component/255 for component in (r255,g255,b255)))
    bandwidths = [
        ('Global Memory Bw.', global_memory_bw, c(8,48,107), c(158,202,225)),
        ('Shared Memory Bw.', shared_memory_bw, c(55,136,190), c(242,246,255))
    ]
    compute_throughputs = [
        ('Peak Comp. Tput.', peak_compute_64, c(0,109,44)),
    ]
    baseline_alg_color = c(153,52,4)
    baseline_alg_name = 'GenASM\nAlgorithm'

    annotate_font = {
        'size': 12.5,
        'weight': 'bold',
        'family': 'sans-serif'
    }

    roof = peak_compute_64
    for name, bw, line_color, fill_color in sorted(bandwidths, key=lambda x: -x[1]):
        ax.plot([l, r], [l * bw, r * bw], label = name, color = line_color)
        bw_compute_intersect_x = roof/bw
        ax.fill_between([left, bw_compute_intersect_x, right], [0]*3, [left*bw, roof, roof], color=fill_color)
    for name, tput, line_color in compute_throughputs:
        ax.plot([l, r], [tput]*2, label = name, color = line_color)
    ax.plot([operational_intensity]*2, [bottom, top], linestyle='--', color=baseline_alg_color, label="operational intensity")

    ax.set_xlabel(r'\fontfamily{ptm}\selectfont Operational Intensity (64-bit op/B)')
    ax.set_ylabel(r'\fontfamily{ptm}\selectfont Comp. Tput. (64-bit op/s)')
    ax.label_outer()
    ax.set_xlim([left, right])
    ax.set_ylim([bottom, top])
    ax.set_xscale('log')
    ax.set_yscale('log')
    ax.yaxis.set_tick_params(which='both', labelleft=True)

    for name, tput, line_color in compute_throughputs:
        ax.annotate(f"\\fontfamily{{ptm}}\\selectfont\\textbf{{{name}}}",
            (left, tput), xycoords='data',
            xytext=(2, 0), textcoords='offset points',
            horizontalalignment='left', verticalalignment='bottom',
            color = line_color,
            **annotate_font)
        ax.annotate(f"\\fontfamily{{ptm}}\\selectfont\\textbf{{{int(tput/GIGA):,} Gop/s}}",
            (left, tput), xycoords='data',
            xytext=(2, -2), textcoords='offset points',
            horizontalalignment='left', verticalalignment='top',
            color = line_color,
            **annotate_font)

    ax.annotate(f"\\fontfamily{{ptm}}\\selectfont\\textbf{{GenASM}}",
        (operational_intensity, bottom*6), xycoords='data',
        xytext=(3, 0), textcoords='offset points',
        horizontalalignment='left', verticalalignment='bottom',
        color = baseline_alg_color,
        **annotate_font)
    ax.annotate(f"\\fontfamily{{ptm}}\\selectfont\\textbf{{Algorithm}}",
        (operational_intensity, bottom*6), xycoords='data',
        xytext=(3, 0), textcoords='offset points',
        horizontalalignment='left', verticalalignment='top',
        color = baseline_alg_color,
        **annotate_font)

    sp1 = ax.transData.transform_point((1,1))
    sp2 = ax.transData.transform_point((2, 2))
    rise = (sp2[1] - sp1[1])
    run = (sp2[0] - sp1[0])
    slope_degrees = np.degrees(np.arctan2(rise, run))

    for name, bw, line_color, fill_color in bandwidths:
        if left *  bw >= bottom:
            pos = (left, left * bw)
            x_offset = 9
            y_offset = 4
        else:
            pos = (bottom / bw, bottom)
            x_offset = 18
            y_offset = 10

        ax.annotate(f"\\fontfamily{{ptm}}\\selectfont\\textbf{{{name}}}",
            pos, xycoords='data',
            textcoords='offset points', xytext=(x_offset,y_offset),
            horizontalalignment='left', verticalalignment='bottom',
            rotation=slope_degrees, rotation_mode='anchor',
            color = line_color,
            ma='center',
            **annotate_font)

        ax.annotate(f"\\fontfamily{{ptm}}\\selectfont\\textbf{{{int(bw/GIGA):,} GB/s}}",
            pos, xycoords='data',
            textcoords='offset points', xytext=(x_offset,y_offset),
            horizontalalignment='left', verticalalignment='top',
            rotation=slope_degrees, rotation_mode='anchor',
            color = line_color,
            ma='center',
            **annotate_font)

    if own_fig:
        plt.savefig(f"{plots_dir}/roofline_a6000.{output_format}",
            bbox_inches = 'tight',
            pad_inches = 0,
            dpi=400)

def plot_Xeon5118_roofline_model(ax=None):
    GIGA = 1_000_000_000
    #Xeon 5118
    dram_bw = 115.2 * GIGA #GB/s
    l3_bw = 414 * GIGA #GB/s
    l2_bw = 1435.2 * GIGA #GB/s
    l1_bw = 3670.8 * GIGA #GB/s
    peak_compute_64 = 110.4 * GIGA #Gops
    peak_compute_64_vec = 441.6 * GIGA #Gops

    #algorithm
    operational_intensity = 0.29 # ops/B

    top = 2*peak_compute_64_vec
    bottom = top/800#top/441.6
    left = 0.25*10**-2.5
    right = 10**0.4
    
    own_fig = False
    if ax is None:
        own_fig = True
        fig, ax = plt.subplots()

    l = 10**-6
    r = 10**6

    def c(r255,g255,b255):
        return tuple((component/255 for component in (r255,g255,b255)))
    bandwidths = [
        ('DRAM Bw.', dram_bw, c(8,48,107), c(158,202,225)),
        ('L3 Bw.', l3_bw, c(8,81,156), c(198,219,239)),
        ('L2 Bw.', l2_bw, c(33,113,181), c(222,235,247)),
        ('L1 Bw.', l1_bw, c(55,136,190), c(242,246,255))
    ]
    compute_throughputs = [
        ('Peak Scalar Tput.', peak_compute_64, c(0,68,27)),
        ('Peak AVX-512 Tput.', peak_compute_64_vec, c(0,109,44)),
    ]
    baseline_alg_color = c(153,52,4)

    annotate_font = {
        'size': 12.5,
        'weight': 'bold',
        'family': 'sans-serif'
    }

    roof = peak_compute_64_vec
    for name, bw, line_color, fill_color in sorted(bandwidths, key=lambda x: -x[1]):
        ax.plot([l, r], [l * bw, r * bw], label = name, color = line_color)
        bw_compute_intersect_x = roof/bw
        ax.fill_between([left, bw_compute_intersect_x, right], [0]*3, [left*bw, roof, roof], color=fill_color)
    for name, tput, line_color in compute_throughputs:
        ax.plot([l, r], [tput]*2, label = name, color = line_color)
    ax.plot([operational_intensity]*2, [bottom, top], linestyle='--', color=baseline_alg_color, label="operational intensity")

    ax.set_xlabel(r'\fontfamily{ptm}\selectfont Operational Intensity (64-bit op/B)')
    ax.set_ylabel(r'\fontfamily{ptm}\selectfont Comp. Tput. (64-bit op/s)')
    ax.label_outer()
    ax.set_xlim([left, right])
    ax.set_ylim([bottom, top])
    ax.set_xscale('log')
    ax.set_yscale('log')
    ax.yaxis.set_tick_params(which='both', labelleft=True)

    sp1 = ax.transData.transform_point((1,1))
    sp2 = ax.transData.transform_point((2, 2))
    rise = (sp2[1] - sp1[1])
    run = (sp2[0] - sp1[0])
    slope_degrees = np.degrees(np.arctan2(rise, run))

    for name, bw, line_color, fill_color in bandwidths:
        if left *  bw >= bottom:
            pos = (left, left * bw)
            x_offset = 8
            y_offset = x_offset * np.tan(np.radians(slope_degrees))
        else:
            pos = (bottom / bw, bottom)
            x_offset = 0
            y_offset = 0

        slopes_annotate_font = dict(annotate_font)
        #slopes_annotate_font['size'] *= 0.9
        ax.annotate(f"\\fontfamily{{ptm}}\\selectfont\\textbf{{{name} {int(bw/GIGA):,} GB/s}}",
            pos, xycoords='data',
            textcoords='offset points', xytext=(x_offset,y_offset),
            horizontalalignment='left', verticalalignment='bottom',
            rotation=slope_degrees, rotation_mode='anchor',
            color = line_color,
            ma='center',
            **slopes_annotate_font)

    for name, tput, line_color in compute_throughputs:
        ax.annotate(f"\\fontfamily{{ptm}}\\selectfont\\textbf{{{name}}}",
            (left, tput), xycoords='data',
            xytext=(2, 0), textcoords='offset points',
            horizontalalignment='left', verticalalignment='bottom',
            color = line_color,
            **annotate_font)
        ax.annotate(f"\\fontfamily{{ptm}}\\selectfont\\textbf{{{int(tput/GIGA):,} Gop/s}}",
            (left, tput), xycoords='data',
            xytext=(2, -2), textcoords='offset points',
            horizontalalignment='left', verticalalignment='top',
            color = line_color,
            **annotate_font)

    ax.annotate(f"\\fontfamily{{ptm}}\\selectfont\\textbf{{GenASM}}",
        (operational_intensity, bottom*4), xycoords='data',
        xytext=(3, 0), textcoords='offset points',
        horizontalalignment='left', verticalalignment='bottom',
        color = baseline_alg_color,
        **annotate_font)
    ax.annotate(f"\\fontfamily{{ptm}}\\selectfont\\textbf{{Algorithm}}",
        (operational_intensity, bottom*4), xycoords='data',
        xytext=(3, 0), textcoords='offset points',
        horizontalalignment='left', verticalalignment='top',
        color = baseline_alg_color,
        **annotate_font)

def plot_roofline_both(output_path):
    plt.rcParams['text.usetex'] = True
    fig, axs = plt.subplots(1, 2)
    fig.set_size_inches(10, 3)
    plt.subplots_adjust(wspace=0.15)

    plot_Xeon5118_roofline_model(ax=axs[0])
    plot_A6000_roofline_model(ax=axs[1])

    names = [
        r'\fontfamily{ptm}\selectfont \textbf{a)} Intel Xeon Gold 5118',
        r'\fontfamily{ptm}\selectfont \textbf{b)} NVIDIA A6000'
    ]

    for name, ax in zip(names, axs):
        #corner_box_text(ax, 1, 1, name, size=11, pad=2.0)
        ax.annotate(f"{name}", xy=(0.5, 1), xytext=(0, 0),
            xycoords='axes fraction', textcoords='offset points',
            #bbox=dict(facecolor='white', alpha=1.0, pad=0.0),
            horizontalalignment='center', verticalalignment='bottom',
            zorder=-100,
            usetex=True,
            size=14)

    plt.savefig(output_path,
        bbox_inches = 'tight',
        pad_inches = 0,
        dpi=400) 

def plot_asic_area(path, ax:matplotlib.axes.Axes, plot_config={}):
    df = pandas.read_csv(path)

    W64 = df.query('W==64').query(f'O>={0}').query(f'O<={48}')
    algs = {
        'GenASM' : (W64.query('sene==False').query('dent==False'), {'color': (0.0, 0.0, 0.0), 'linestyle':'--'}),
        'Scrooge': (W64.query('sene==True').query('dent==True'), {'color': (0, 0.7, 0.5)})
    }
    first, last = min(W64['O']), max(W64['O'])

    margin = 0.05
    width = last - first
    left, right = first - width*margin, last + width*margin
    bottom, top = 0, max(W64['area'])*1.1

    for alg, (rows, line_config) in algs.items():
        ax.plot(rows['O'], rows['area'], label=f'{alg}', **plot_config, **line_config)

    ax.set_xticks(range(first, last+1, 8))
    ax.set_xticks(W64['O'].unique(), minor=True)
    ax.tick_params(axis='both', which='major', pad=0.0, labelsize=11)

    ax.set_ylim([bottom, top])
    ax.set_xlim([left, right])

    ax.set_ylabel('Silicon Area (mm$^2$)', labelpad=0.0, fontsize=11)
    ax.set_xlabel('Window Overlap (O)')

    ax.grid(True)

    improv_x = 33
    y_base = min(algs['GenASM'][0].query(f'O=={improv_x}')['area'])
    y_improv = min(algs['Scrooge'][0].query(f'O=={improv_x}')['area'])
    ax.annotate("",
                xytext=(improv_x, y_base),
                xy=(improv_x, y_improv),
                arrowprops=dict(arrowstyle="->", linewidth=2, shrinkA=4, shrinkB=2))
    ax.annotate(f"{y_base/y_improv:.1f}x",
                    xy=(improv_x, y_improv),
                    ha='left', va='bottom',
                    xytext=(5, 0),
                    textcoords='offset points',
                    family='sans-serif', weight='bold')

def plot_asic_power(path, ax:matplotlib.axes.Axes, plot_config={}):
    df = pandas.read_csv(path)

    W64 = df.query('W==64').query(f'O>={0}').query(f'O<={48}')
    algs = {
        'GenASM' : (W64.query('sene==False').query('dent==False'), {'color': (0.0, 0.0, 0.0), 'linestyle':'--'}),
        'Scrooge': (W64.query('sene==True').query('dent==True'), {'color': (0, 0.7, 0.5)})
    }
    first, last = min(W64['O']), max(W64['O'])

    margin = 0.05
    width = last - first
    left, right = first - width*margin, last + width*margin
    bottom, top = 0, max(W64['power'] * 1000)*1.1

    for alg, (rows, line_config) in algs.items():
        ax.plot(rows['O'], rows['power'] * 1000, label=f'{alg}', **plot_config, **line_config)

    ax.set_xticks(range(first, last+1, 8), )
    ax.set_xticks(W64['O'].unique(), minor=True)
    ax.tick_params(axis='both', which='major', pad=0.0, labelsize=11)

    ax.set_ylim([bottom, top])
    ax.set_xlim([left, right])

    ax.set_ylabel('Power (mW)', labelpad=0.0, fontsize=11)
    ax.set_xlabel('Window Overlap (O)')

    ax.grid(True)

    improv_x = 33
    y_base = min(algs['GenASM'][0].query(f'O=={improv_x}')['power'] * 1000)
    y_improv = min(algs['Scrooge'][0].query(f'O=={improv_x}')['power'] * 1000)
    ax.annotate("",
                xytext=(improv_x, y_base),
                xy=(improv_x, y_improv),
                arrowprops=dict(arrowstyle="->", linewidth=2, shrinkA=4, shrinkB=2))
    ax.annotate(f"{y_base/y_improv:.1f}x",
                    xy=(improv_x, y_improv),
                    ha='left', va='bottom',
                    xytext=(5, 0),
                    textcoords='offset points',
                    family='sans-serif', weight='bold')

def plot_asic(path, output_file_path):
    fig, axs = plt.subplots(1, 2)
    fig.set_size_inches(6.4, 2)
    fig.subplots_adjust(wspace=0.3)

    plot_config = {
        'linewidth': 2,
    }
    plot_asic_area(path, axs[0], plot_config=plot_config)
    plot_asic_power(path, axs[1], plot_config=plot_config)

    plt.figlegend(*axs[0].get_legend_handles_labels(),
            bbox_to_anchor=(0.2, 0.95, 0.6, 0.1),
            #bbox_to_anchor=(0.5, 0.1),
            loc='upper center', ncol=2,
            mode="expand", borderaxespad=1,
            handletextpad=0.1,
            framealpha=0,
            prop={'size': 11})

    plt.savefig(output_file_path,
        bbox_inches = 'tight',
        pad_inches = 0,
        dpi=400) 

def ma_to_edits(ma: MultipleSeqAlignment):
    first_seq, second_seq = ma[0].seq, ma[1].seq
    edits = []
    for a, b in zip(first_seq, second_seq):
        if a == '-':
            edits.append('I')
        elif b == '-':
            edits.append('D')
        else:
            if a == b: edits.append('=')
            else: edits.append('X')
    return edits

def ma_to_matches(ma: MultipleSeqAlignment):
    first_seq, second_seq = ma[0].seq, ma[1].seq
    i, j = 0, 0
    coords = []
    for a, b in zip(first_seq, second_seq):
        if a == '-':
            j+=1
        elif b == '-':
            i+=1
        else:
            if a == b:
                coords.append((j, i))
            i+=1
            j+=1
    return coords

def cigar_to_match_coords(cigar):
    i,j = 0,0
    res = []

    runs = re.finditer(r"(\d+)([=XID])",cigar)
    for run in runs:
        edit_count = int(run.group(1))
        edit_type = run.group(2)

        before = (i,j)
        if edit_type=='=' or edit_type=='X':
            i+=edit_count
            j+=edit_count
        elif edit_type=='I':
            i+=edit_count
        elif edit_type=='D':
            j+=edit_count
        else:
            raise ValueError(f"cigar string has unknown edit type {edit_type}")
        after = (i,j)

        if edit_type=='=':
            mat_i, mat_j = before
            for idx in range(after[0] - before[0]):
                res.append((mat_i, mat_j))
                mat_i += 1
                mat_j += 1
    return res

def plot_accuracy(cigar_path, maf_path):
    print("loading df...", end=" ")
    data = pandas.read_csv(cigar_path,
        usecols=['algorithm', 'pair_idx', 'score', 'cigar', 'read', 'reference'],
        dtype={'pair_idx' : int, 'score' : 'Int64'})
    print("done")

    print("reading maf...", end=" ")
    all_maf_alignments = list(AlignIO.parse(maf_path, "maf"))
    print("done")
    groundtruth_alignments = [ma for ma in all_maf_alignments if all([sr.annotations['strand']==1 for sr in ma])]
    #groundtruth_alignments_map = {ma[0].seq.replace('-', '')[:9000] : ma for ma in groundtruth_alignments}
    #groundtruth_alignments_map = {(seqrec.seq.replace('-', '')[:9000] for seqrec in ma) : i for i, ma in enumerate(groundtruth_alignments)}
    groundtruth_alignments_map = {tuple(seqrec.seq.replace('-', '')[:9000] for seqrec in ma) : ma for ma in groundtruth_alignments}

    algorithms = data['algorithm'].unique()
    matched_bases_distributions = {alg: [] for alg in algorithms}
    for algorithm in algorithms:
        #if algorithm != 'ksw2_extz2_sse':
        #    continue
        indices = (data['algorithm'] == algorithm) & data['cigar'].notnull() & data['reference'].notnull() & data['read'].notnull()
        algorithm_data = data[indices]

        alg_refreads = list(zip(
            [r[:9000] for r in algorithm_data['reference']],
            [r[:9000] for r in algorithm_data['read']],
        ))

        #print(len(groundtruth_alignments_map), len(set(groundtruth_alignments_map)))
        #print(len(alg_refreads), len(set(alg_refreads)))
        #print(len(set(groundtruth_alignments_map) - set(alg_refreads)))
        #print(len(set(alg_refreads) - set(groundtruth_alignments_map)))
        #print()

        #for pair_idx, ref in [(pair_idx, r[:9000]) for (pair_idx, r) in zip(algorithm_data['pair_idx'], algorithm_data['reference'])]:
        #    print(pair_idx, groundtruth_alignments_map[ref])

        #for alg_ref, alg_cigar in zip(algorithm_data['reference'], algorithm_data['cigar']):
        #    print(len(alg_ref))

        #continue

        for alg_refread, alg_cigar in list(zip(alg_refreads, algorithm_data['cigar'])):
            alg_matches = cigar_to_match_coords(alg_cigar)
            groundtruth_aln = groundtruth_alignments_map.get(alg_refread)
            #if not groundtruth_aln:
            #    print(alg_ref[:100])
            #    continue
            groundtruth_matches = ma_to_matches(groundtruth_aln)
            correct_matches = len(list(set(alg_matches) & set(groundtruth_matches)))
            #print(f'{correct_matches}/{len(groundtruth_matches)} {(len(groundtruth_matches)-correct_matches):>4}')

            matched_bases_distributions[algorithm].append(correct_matches/len(groundtruth_matches))

            #if correct_matches/len(groundtruth_matches) < 0.98:
            #    fig, ax = plt.subplots()
            #    for matches, label in zip([alg_matches, groundtruth_matches], [algorithm, "groundtruth"]):
            #        xs, ys = zip(*matches)
            #        ax.plot(xs, ys, 'o', label=label)
            #    ax.legend()
            #    plt.show()

            #fig, ax = plt.subplots()
            #for matches in [alg_matches, groundtruth_matches]:
            #    xs, ys = zip(*matches)
            #    ax.plot(xs, ys)
            #plt.show()
            #exit()
            #if pandas.isnull(aln):
                #print(f"{algorithm} algorithm had no cigar string")
            #    pass
            #else:
                #print(aln)
                #print(cigar_to_match_coords(aln))
            #    pass
    
    fig, ax = plt.subplots()
    for algorithm in algorithms:
        mats = matched_bases_distributions[algorithm]
        ax.hist(mats, histtype='stepfilled', label=algorithm, bins=200, range=(0.0, 1.0), alpha=0.3)
        print(f'{algorithm} {sum(mats)/len(mats)}')
    ax.legend()
    plt.show()

def print_matched_bases(matches_path):
    df = pandas.read_csv(matches_path)

    for alg, subdata in df.groupby('algorithm'):
        print(f'{alg:15} {subdata["correct_matches"].mean()}')
    
    print(f'{"groundtruth":15} {subdata["groundtruth_matches"].mean()}')

def plot_matched_bases(all_matches_path, wo_matches_path, output_path):
    plt.rcParams['text.usetex'] = True

    def c(r255,g255,b255):
        return tuple((component/255 for component in (r255,g255,b255)))

    plot_labels = {
        'genasm_cpu': 'Scrooge',
        'edlib': 'Edlib',
        'ksw2_extz': 'KSW2 extz',
        'ksw2_extz2_sse': 'KSW2',#'KSW2 extz2_sse',
        'wfa_exact': 'WFA exact',
        'wfa_adaptive': 'WFA adaptive',
        'wfa_lm': 'WFA lm',
        'custom_gact': 'Darwin'
    }

    all_matches_df = pandas.read_csv(all_matches_path)
    all_matches_df = all_matches_df[all_matches_df['algorithm'] != 'genasm_cpu']

    all_correct_matches = {
        alg: subdata["correct_matches"].mean() for (alg, subdata) in all_matches_df.groupby('algorithm')
    }
    ground_truth_matches = all_matches_df['groundtruth_matches'].mean()

    wo_matches_df = pandas.read_csv(wo_matches_path)
    wo_matches_mean = wo_matches_df.groupby(['W', 'O']).mean().reset_index()

    min_x, max_x = min(wo_matches_mean['W']), max(wo_matches_mean['W'])
    range_x = max_x - min_x
    left, right = min_x - 0.02*range_x, max_x + 0.02*range_x

    fig, ax = plt.subplots()
    ax.grid()
    ax.set_xticks([min_x] + list(range(32, 256+1, 32)))
    ax.set_xticks(wo_matches_df['W'].unique(), minor=True)
    ax.tick_params(axis='both', which='major', pad=1)

    ax.set_xlabel(r'\textbf{Window Size (W)}', labelpad=1)
    ax.set_ylabel(r'\textbf{Correctly Aligned Bases (Fraction)}', size=11, labelpad=1)

    linestyles = [
        '-',
        '-.',
        '--',
        ':'
    ]

    colors = [
        c(100,100,220),
        c(217,95,2),
        c(27,158,119),
        c(200, 50, 200),
    ]

    ax.plot([left, right], [ground_truth_matches/ground_truth_matches]*2, '--', color=c(255, 0, 0), label='Ground Truth')
    for (alg, correct_matches), linestyle, color in zip(all_correct_matches.items(), linestyles, colors):
        linewidth=1.5
        label=plot_labels[alg]
        ax.plot([left, right], [correct_matches/ground_truth_matches]*2, linestyle=linestyle, linewidth=linewidth, color=color, label=label)

    linewidth=2
    color=ours_color
    label=plot_labels['genasm_cpu']
    ax.plot(wo_matches_mean['W'], wo_matches_mean['correct_matches']/ground_truth_matches, linewidth=linewidth, color=color, marker='o', label=label)
    ax.set_xlim(left, right)
        
    fig.set_size_inches(10, 3)
    centered_figlegend(fig, ax,
        ncol=6,
        handletextpad=0.2, columnspacing=1.5,
        framealpha=0, prop={'size': 12})

    plt.savefig(output_path,
        bbox_inches = 'tight',
        pad_inches = 0,
        dpi=400)
    

if __name__ == "__main__":
    plots_dir = "plots"
    Path(plots_dir).mkdir(parents=True, exist_ok=True)

    #output_format = "eps"
    #output_format = "svg"
    output_format = "pdf"
    #output_format = "png"

    plt.rcParams["font.family"] = "serif"
    plt.rcParams["font.size"] = 12.5

    plot_gpu_threadblocks_paper("profile/pbsim_chained_gpu_sweep_threadblocks.csv", f"{plots_dir}/pbsim_chained_gpu_threadblocks.{output_format}")
    plot_gpu_threadblocks_illumina_paper("profile/illumina_chained_gpu_sweep_threadblocks_W=32.csv", f"{plots_dir}/illumina_chained_gpu_threadblocks.{output_format}")
    plot_gpu_threadblocks_supplementary("profile/pbsim_chained_gpu_sweep_threadblocks.csv", f"{plots_dir}/pbsim_chained_gpu_threadblocks_supplementary.{output_format}")
    plot_gpu_WO_illumina_paper("profile/illumina_chained_gpu_sweep_WO.csv", f"{plots_dir}/illumina_chained_gpu_WO.{output_format}")
    plot_gpu_WO_paper("profile/pbsim_chained_gpu_sweep_WO.csv", f"{plots_dir}/pbsim_chained_gpu_WO.{output_format}")
    plot_gpu_WO_supplementary("profile/pbsim_chained_gpu_sweep_WO.csv", f"{plots_dir}/pbsim_chained_gpu_WO_supplementary.{output_format}")
    plot_gpu_O_illumina_paper("profile/illumina_chained_gpu_sweep_O_W=32.csv", f"{plots_dir}/illumina_chained_gpu_O.{output_format}")
    plot_gpu_O_paper("profile/pbsim_chained_gpu_sweep_O.csv", f"{plots_dir}/pbsim_chained_gpu_O.{output_format}")
    plot_gpu_O_supplementary("profile/pbsim_chained_gpu_sweep_O.csv", f"{plots_dir}/pbsim_chained_gpu_O_supplementary.{output_format}")
    plot_cpu_threads_WO_paper("profile/pbsim_chained_cpu_sweep_threads.csv", "profile/pbsim_chained_cpu_sweep_WO.csv", f"{plots_dir}/pbsim_chained_cpu_threads_WO.{output_format}")
    plot_cpu_threads_WO_paper("profile/pbsim_chained_cpu_sweep_threads.csv", "profile/illumina_chained_cpu_sweep_WO.csv", f"{plots_dir}/illumina_chained_cpu_threads_WO.{output_format}")
    plot_cpu_threads_WO_supplementary("profile/pbsim_chained_cpu_sweep_threads.csv", "profile/pbsim_chained_cpu_sweep_WO.csv", f"{plots_dir}/pbsim_chained_cpu_threads_WO_supplementary.{output_format}")
    plot_cpu_O("profile/pbsim_chained_cpu_sweep_O.csv", f"{plots_dir}/pbsim_chained_cpu_O.{output_format}")
    plot_darwin_scaling("profile/pbsim_chained_darwin_gpu_sweep.csv", f"{plots_dir}/pbsim_chained_darwin_gpu_scaling.{output_format}")
    plot_baselines_threads_paper("profile/pbsim_chained_baselines_sweep_threads.csv", "profile/illumina_chained_baselines_sweep_threads.csv", "profile/pbsim_chained_cpu_sweep_threads.csv", "profile/illumina_chained_cpu_sweep_threads_W=32.csv", f"{plots_dir}/baselines_threads.{output_format}")
    plot_performance_bars_both(
        "profile/pbsim_chained_gpu_sweep_threadblocks.csv",
        "profile/pbsim_chained_darwin_gpu_sweep.csv",
        "profile/pbsim_chained_cudaswpp.csv",
        "profile/pbsim_chained_cpu_sweep_threads.csv",
        "profile/pbsim_chained_baselines_sweep_threads.csv",
        "profile/illumina_chained_gpu_sweep_WO.csv",
        "profile/illumina_chained_cudaswpp.csv",
        "profile/illumina_chained_cpu_sweep_O_W=32.csv",
        "profile/illumina_chained_baselines_sweep_threads.csv",
        f"{plots_dir}/performance_bars_both.{output_format}"
        )
    plot_performance_error_bars_both(
        "profile/pbsim_chained_gpu_sweep_threadblocks.csv",
        "profile/pbsim_chained_darwin_gpu_sweep.csv",
        "profile/pbsim_chained_cudaswpp.csv",
        "profile/pbsim_chained_cpu_sweep_threads.csv",
        "profile/pbsim_chained_baselines_sweep_threads.csv",
        "profile/illumina_chained_gpu_sweep_WO.csv",
        "profile/illumina_chained_cudaswpp.csv",
        "profile/illumina_chained_cpu_sweep_O_W=32.csv",
        "profile/illumina_chained_baselines_sweep_threads.csv",
        f"{plots_dir}/performance_error_bars_both.{output_format}"
        )
    plot_accuracy("profile/pbsim_groundtruth_all_accuracy.csv", f"{plots_dir}/pbsim_groundtruth_all_accuracy.{output_format}")
    plot_accuracy("profile/pbsim_chained_all_accuracy.csv", f"{plots_dir}/pbsim_chainedd_all_accuracy.{output_format}")
    plot_accuracy_WO_both("profile/pbsim_groundtruth_cpu_accuracy_sweep_wo.csv", "profile/pbsim_groundtruth_all_accuracy.csv", "profile/illumina_chained_cpu_accuracy_sweep_wo.csv", "profile/illumina_chained_all_accuracy.csv", f"{plots_dir}/accuracy_sweep_WO_both.{output_format}")
    plot_accuracy_O_both("profile/pbsim_groundtruth_cpu_accuracy_sweep_o.csv", "profile/pbsim_groundtruth_all_accuracy.csv", "profile/illumina_chained_cpu_accuracy_sweep_o.csv", "profile/illumina_chained_all_accuracy.csv", f"{plots_dir}/accuracy_sweep_O_both.{output_format}")
    plot_roofline_both(f"{plots_dir}/roofline_both.{output_format}")
    plot_asic("profile/asic_numbers.csv", f"{plots_dir}/asic_numbers.{output_format}")
    plot_accuracy('profile/pbsim_groundtruth_all_accuracy_cigar.csv', 'datasets/pbsim_groundtruth/candidates.maf')
    print_matched_bases('profile/pbsim_groundtruth_all_matches.csv')
    plot_matched_bases('profile/pbsim_groundtruth_all_matches.csv', 'profile/pbsim_groundtruth_wo_matches.csv', f'{plots_dir}/matching_bases.{output_format}')

    plt.show()
