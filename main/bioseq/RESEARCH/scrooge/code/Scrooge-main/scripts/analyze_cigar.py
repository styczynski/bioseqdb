import pandas
import re
import Bio
from Bio import AlignIO, SeqRecord
from Bio.AlignIO import MultipleSeqAlignment
from pathlib import Path
from datetime import datetime

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
            res += list(zip(
                range(before[0], after[0]),
                range(before[1], after[1])
                ))
    return res

DICT_LOOKUP_PREFIX = 9000#9000 #algorithm data might not contain the full read or reference length
def get_correct_matches(algorithm_data, groundtruth_matches_map, swap_indels):
    print("prepare alg_refreads")
    alg_refreads = (hash(refread) for refread in zip(
        [r[:DICT_LOOKUP_PREFIX].upper() for r in algorithm_data['reference']],
        [r[:DICT_LOOKUP_PREFIX].upper() for r in algorithm_data['read']],
    ))
    print("prepare alg_matchess")
    if not swap_indels:
        alg_matchess = (cigar_to_match_coords(cig) for cig in  algorithm_data['cigar'])
    else:
        alg_matchess = (cigar_to_match_coords(cig.replace('D', '_').replace('I', 'D').replace('_', 'I')) for cig in  algorithm_data['cigar'])
    print("prepare pair_indices")
    pair_indices = algorithm_data['pair_idx']

    res = []
    for alg_refread, alg_matches, pair_idx in zip(alg_refreads, alg_matchess, pair_indices):
        groundtruth_matches = groundtruth_matches_map.get(alg_refread)
        if groundtruth_matches is None:
            print(f'could not find groundtruth for pair {pair_idx}, skipping')
            continue
        correct_matches = set(alg_matches) & set(groundtruth_matches)

        #print(f'{correct_matches}/{len(groundtruth_matches)} {(len(groundtruth_matches)-correct_matches):>4}')
        res.append({
            'correct_matches': len(correct_matches),
            'groundtruth_matches': len(groundtruth_matches),
            'alg_matches': len(alg_matches)
        })

    return res

def get_match_statistics(cigar_path, maf_path, out_path, grouped_by=['algorithm']):
    print("loading df...", end=" ")
    data = pandas.read_csv(cigar_path)
    print("done")

    print("reading maf...", end=" ")
    all_maf_alignments = AlignIO.parse(maf_path, "maf")
    print("done")

    print("preparing groundtruth alignments...", end=" ")
    groundtruth_alignments = (ma for ma in all_maf_alignments if all([sr.annotations['strand']==1 for sr in ma]))
    del all_maf_alignments
    groundtruth_matches_map = {
        hash(tuple(seqrec.seq.replace('-', '')[:DICT_LOOKUP_PREFIX].upper() for seqrec in ma)) :
        ma_to_matches(ma)
        for i, ma in enumerate(groundtruth_alignments)
        #if i < 10
    }
    print("done")

    #algorithms = data['algorithm'].unique()
    all_match_counts = []
    #for algorithm in algorithms:
    grouped = data.groupby(grouped_by)
    for i, (grouped_by_params, subdata) in enumerate(grouped):
        print(f"[{datetime.now()}] match counts for {i}/{len(grouped)} {grouped_by_params}...", end=" ")
        #indices = (data['algorithm'] == algorithm) & data['cigar'].notnull() & data['reference'].notnull() & data['read'].notnull()
        indices = subdata['cigar'].notnull() & subdata['reference'].notnull() & subdata['read'].notnull()
        subdata = subdata[indices]
        if grouped_by == ['algorithm'] and grouped_by_params == 'wfa_adaptive':
            swap_indels = True
        else:
            swap_indels = False
        match_counts = get_correct_matches(subdata, groundtruth_matches_map, swap_indels)
        print("done")
        for match_count in match_counts:
            if len(grouped_by) > 1:
                for k, v in zip(grouped_by, grouped_by_params):
                    match_count[k] = v
            else:
                match_count[grouped_by[0]] = grouped_by_params
        all_match_counts += match_counts

    df = pandas.DataFrame(all_match_counts)
    df.to_csv(out_path)

#get_match_statistics('profile/pbsim_groundtruth_all_accuracy_cigar.csv', 'datasets/pbsim_groundtruth/candidates.maf', 'profile/pbsim_groundtruth_all_matches.csv', grouped_by=['algorithm'])
#get_match_statistics('profile/pbsim_groundtruth_cpu_accuracy_sweep_wo_cigar.csv', 'datasets/pbsim_groundtruth/candidates.maf', 'profile/pbsim_groundtruth_wo_matches.csv', grouped_by=['W', 'O'])
#get_match_statistics('profile/pbsim_groundtruth_cpu_accuracy_sweep_wo_cigar_1.csv', 'datasets/pbsim_groundtruth/candidates.maf', 'profile/pbsim_groundtruth_wo_matches.csv', grouped_by=['W', 'O'])
get_match_statistics('profile/pbsim_groundtruth_all_accuracy_cigar.csv', 'datasets/pbsim_groundtruth/candidates.maf', 'profile/pbsim_groundtruth_all_matches_wfaadaptive_gact.csv', grouped_by=['algorithm'])
