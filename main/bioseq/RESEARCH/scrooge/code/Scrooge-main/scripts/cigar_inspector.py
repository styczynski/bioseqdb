import argparse
import re
import itertools
import pandas

from matplotlib import pyplot as plt
from matplotlib.ticker import Formatter, ScalarFormatter, AutoLocator
from pathlib import Path

def transpose(table):
    return list(zip(*table))

def cigar_to_coords(cigar):
    i,j = 0,0
    res = [(i, j)]

    runs = re.finditer(r"(\d+)([=XID])",cigar)
    for run in runs:
        edit_count = int(run.group(1))
        edit_type = run.group(2)

        if edit_type=='=' or edit_type=='X':
            i+=edit_count
            j+=edit_count
        elif edit_type=='I':
            i+=edit_count
        elif edit_type=='D':
            j+=edit_count
        else:
            raise ValueError(f"cigar string has unknown edit type {edit_type}")

        res.append((i,j))

    return res

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
            res.append((before, after))
    return res

def plot_cigar(ax, cigar, **plot_config):
    alignment_path = cigar_to_coords(cigar)
    xs, ys = transpose(alignment_path)
    ax.plot(xs, ys, **plot_config)

def plot_cigar_matches(ax, cigar, **plot_config):
    match_segments = cigar_to_match_coords(cigar)
    match_segments_with_nones = [(match_start,match_end,(None,None)) for (match_start,match_end) in match_segments]
    flattened_matches = itertools.chain(*match_segments_with_nones)
    xs, ys = transpose(flattened_matches)
    ax.plot(xs, ys, **plot_config)

def plot_genasm_windows(ax:plt.Axes, read, reference, cigar, colors, W, O):
    alignment_path = cigar_to_coords(cigar)
    window_starts = [(0, 0)]
    for i, j in alignment_path:
        if i > window_starts[-1][0] + W-O or \
        j > window_starts[-1][1] + W-O:
            window_starts.append((i, j))

    for i, window_start in enumerate(window_starts):
        window_end = (
            min(window_start[0] + W, len(read)),
            min(window_start[1] + W, len(reference))
        )
        window_polygon = [
            window_start,
            (window_start[0], window_end[1]),
            window_end,
            (window_end[0], window_start[1]),
            window_start
        ]
        xs, ys = zip(*window_polygon)
        ax.plot(xs, ys, color=colors[i%len(colors)])

def cigar_inspector(read, reference, algorithms, W=None, O=None):
    fig, ax = plt.subplots(1,1, figsize=(8, 8))

    for name, cigar, color in algorithms:
        plot_cigar(ax, cigar, label=f"{name} edits", color=color, linestyle='--', linewidth=0.5)
        plot_cigar_matches(ax, cigar, label=f"{name} matches", color=color, linestyle='-', linewidth=1)
        print(name)
        if name==f'Scrooge W={W}' and W is not None and O is not None:
            window_colors = [
                (1.0, 0.0, 0.0),
                (0.6, 0.0, 0.0),
                (0.3, 0.0, 0.0),
            ]
            plot_genasm_windows(ax, read, reference, cigar, window_colors, W, O)

    ax.invert_yaxis()
    ax.autoscale(enable=True, axis="both", tight=True)
    ax.grid(True)
    fig.legend(bbox_to_anchor=[0.5, 1.0], loc='upper center', ncols=3)
    ax.set_xlabel("Read", weight='bold')
    ax.set_ylabel("Reference", weight='bold')
    ax.xaxis.set_label_position('top')
    ax.xaxis.tick_top()
    ax.set_box_aspect(1)

    def on_lims_change(event_ax):
        x_a, x_b = sorted(event_ax.get_xlim())
        y_a, y_b = sorted(event_ax.get_ylim())
        x_a, x_b = int(x_a), int(x_b)
        y_a, y_b = int(y_a), int(y_b)
        x_range = x_b - x_a
        y_range = y_b - y_a
        if max(x_range, y_range) < 100:
            x_indices = [i for i in range(x_a, x_b+1) if i >= 0 and i < len(read)]
            y_indices = [i for i in range(y_a, y_b+1) if i >= 0 and i < len(reference)]
            ax.set_xticks([i+0.5 for i in x_indices])
            ax.set_xticklabels([(f"{i}\n" if i %10==0 else "") + f"{read[i]}" for i in x_indices])
            ax.set_yticks([i+0.5 for i in y_indices])
            ax.set_yticklabels([(f"{i}" if i %10==0 else "") + f"{reference[i]}" for i in y_indices])
        else:
            ax.xaxis.set_major_locator(AutoLocator())
            ax.xaxis.set_major_formatter(ScalarFormatter())
            ax.yaxis.set_major_locator(AutoLocator())
            ax.yaxis.set_major_formatter(ScalarFormatter())

    ax.callbacks.connect('xlim_changed', on_lims_change)
    ax.callbacks.connect('ylim_changed', on_lims_change)

    fig.show()

def plot_worst_alignment_path(file_path, worst_idx, W=None, O=None, WO_sweep_path=None):
    print("loading df")
    data = pandas.read_csv(file_path,
        usecols=['algorithm', 'pair_idx', 'score', 'cigar', 'read', 'reference'],
        dtype={'pair_idx' : int, 'score' : 'Int64'})
    print("done")

    algorithms = list(data['algorithm'].unique())
    reshaped_subdatas = [subdata.drop(columns='algorithm').rename(columns={'score':alg,'cigar':f'{alg}_cigar'}) for (alg, subdata) in data.groupby(['algorithm'])]

    if WO_sweep_path:
        print("loading df")
        data = pandas.read_csv(WO_sweep_path,
            usecols=['W', 'pair_idx', 'score', 'cigar', 'read', 'reference'],
            dtype={'pair_idx' : int, 'score' : 'Int64'})
        print("done")

        reshaped_subdatas += [subdata.drop(columns='W').rename(columns={'score':f'Scrooge W={int(W)}','cigar':f'Scrooge W={int(W)}_cigar'}) for (W, subdata) in data.groupby(['W'])]
        print(algorithms)
        Ws = [int(W) for W in data['W'].unique() if W <= 32]
        algorithms += [f'Scrooge W={W}' for W in Ws]

    joined = reshaped_subdatas[0]
    for subdata in reshaped_subdatas[1:]:
        joined = joined.merge(subdata, on='pair_idx', how='outer')
    data = joined.fillna(0)
    data.query('edlib!=0', inplace=True)

    for algorithm in algorithms:
        data[f'{algorithm}_normalized'] = data[algorithm]/data['edlib']

#    data.sort_values(by=['genasm_cpu_normalized'], inplace=True)
    data.sort_values(by=['Scrooge W=16'], inplace=True)

    alg_colors = {
        'edlib': (1.0,0.0,0.0),
        'genasm_cpu': (0.0,1.0,0.0),
        'ksw2_extz2_sse': (0.0,0.0,0.0),
        'Scrooge W=16': (0.0,0.5,0.0),
        'Scrooge W=32': (0.0,1.0,0.0)
    }

    renaming = {
        'edlib': 'Edlib'
    }

    algorithms.remove('genasm_cpu')
    algorithms.remove('ksw2_extz2_sse')

    pair = data.iloc[worst_idx]
    #print(pair['pair_idx'])
    #print(pair['read'])
    #print(pair['reference'])

    algs = [(renaming.get(alg_name, alg_name), pair[f'{alg_name}_cigar'], alg_colors.get(alg_name, (0.0,0.0,0.0))) for alg_name in algorithms]
    cigar_inspector(pair['read'], pair['reference'], algs, W, O)
    plt.show()

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('cigar_file_path', type=Path, help='path to *_accuracy_cigar.csv file produced with profile.py')
    parser.add_argument('worst_idx', type=int, help='idx-th worst alignment to plot')
    parser.add_argument('--W', type=int, help='plot GenASM\'s windows with the given W')
    parser.add_argument('--O', type=int, help='plot GenASM\'s windows with the given O')
    parser.add_argument('--WO_sweep_path', type=Path, help='path to *_accuracy_cigar.csv file produced with profile.py')
    args = parser.parse_args()

    plot_worst_alignment_path(args.cigar_file_path, args.worst_idx, args.W, args.O, args.WO_sweep_path)
