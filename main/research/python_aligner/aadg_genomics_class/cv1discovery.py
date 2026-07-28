# Piotr Styczyński (ps386038)
# Fast and accurate seed-and-extend aligner for references ~ 20M and queries ~ 1k bases
# Usage: 
#
#    python3 mapper.py data_big/reference20M.fasta data_big/reads20Ma.fasta
#
from gc import collect as gc_collect, disable as gc_disable
from sys import maxsize, argv as sys_argv, exit
from functools import partial
import gzip

from time import time_ns
from itertools import chain
from numpy.lib.stride_tricks import sliding_window_view
from operator import itemgetter
from collections import defaultdict

from numpy import sort, frompyfunc, vectorize, uint8, uint32, array, concatenate, add, argmin, arange, column_stack, unique, split, empty, ix_, take, diff
from bisect import bisect_right

# Used to create kmer binary masks
MAX_KMER_SIZE = 64

# Mapping of special character used by suffix tables and similar structures
MAPPING_DOLLAR = 0
# Mapping of nucleotides
MAPPING = dict(
    C=1,
    A=2,
    T=3,
    G=4,
    N=5,
)
# Allphabet
ALPHABET = set(MAPPING.values())
# Alphabet with included special character
ALPHABET_DOLLAR = set([*ALPHABET, MAPPING_DOLLAR])

# Pairs of alphabet symbols (a, b) such that a > b
# Useful for O(a^2) iteration over alphabet
ALPHABET_LT_PAIRS = [(a, b) for a in ALPHABET for b in ALPHABET if b < a]

# Vectorized numpy function to map nucleotides from string
MAPPING_FN = vectorize(MAPPING.get, otypes=[uint8])

# Load reference in chunks of that size
REFERENCE_INDEX_CHUNK_SIZE = 800000

# Length of k-mer used to generate (k,w)-minimizer indices
KMER_LEN = 17 # was: 16
# Length of window to generate (k,w)-minimizer indices
WINDOW_LEN = 8 # was: 5

DATASET_READ_MAX_SIZE = 152

DATASET_CHUNK_SIZE = 10000

# mask[k] := mask used to calculate hash for k-mer of length k
_global_masks = None

# Makes sure 0 <= pos <= len
def normalize_pos(pos, len):
    return min(max(pos, 0), len)

# Generate hash mask for given kmer length
# The mask will be just 2*k last bits on for given k-mer len: k
# e.g mask(3) = b...000000111111
def generate_mask(
    kmer_len: int,
) -> int:
    global _global_masks
    if not _global_masks:
        _global_masks = dict()
        ret = 3
        for i in range(MAX_KMER_SIZE+1):
            ret = (ret << 2) | 3
            _global_masks[i] = ret
    return _global_masks[kmer_len]


# Claculate cartesian product of matches k-mers coorginates arrays (uint32)
# Formal definition: if a in A and b in B and c in C then (a,b,c) in pos_cartesian_product(A,B,C)
def pos_cartesian_product(*arrays):
    la = len(arrays)
    arr = empty([len(a) for a in arrays] + [la], dtype=uint32)
    for i, a in enumerate(ix_(*arrays)):
        arr[...,i] = a
    return arr.reshape(-1, la)

def dist_dataset(ds1, ds2):
    a1 = set(ds1[0])
    b1 = set(ds1[1])
    a2 = set(ds2[0])
    b2 = set(ds2[1])
    j1 = len(a1.intersection(a2)) / len(a1.union(a2)) + len(b1.intersection(b2)) / len(b1.union(b2))
    return j1

def load_dataset_meta(fasta_file_gz, assigned_class, is_train):
    print(f"{fasta_file_gz}: Scan dataset")
    s_read_len = []
    s_nucls = dict(A=[], C=[], T=[], G=[], N=[])
    s_nucls_totals = dict(A=0, C=0, T=0, G=0, N=0)
    all_reads = dict()
    all_reads_count = 0
    for line in chain(gzip.open(fasta_file_gz, 'rt'), [">"]):
        if line[0] != '>':
            read = line.rstrip()
            s_read_len.append(len(read))
            for nucl in s_nucls.keys():
                if len(s_nucls[nucl]) < len(read):
                    s_nucls[nucl] += [0] * (len(read) - len(s_nucls[nucl]))
            for (index, nucl) in enumerate(read):
                s_nucls[nucl][index] += 1
                s_nucls_totals[nucl] += 1
            all_reads_count += 1
            if read not in all_reads:
                all_reads[read] = 1
            else:
                all_reads[read] += 1
    s_nucls_totals_sum = sum(s_nucls_totals.values())
    print(f"{fasta_file_gz}: Dataset scanned")
    return dict(
        min_read_len=min(s_read_len),
        max_read_len=max(s_read_len),
        read_count=len(s_read_len),
        nucl_probs_pos={nucl: [prob/len(s_read_len) for prob in probs] for (nucl, probs) in s_nucls.items()},
        nucl_probs_tot={nucl: count/s_nucls_totals_sum for (nucl, count) in s_nucls_totals.items()},
        distinct_reads=len(all_reads.keys()) / all_reads_count,
        read_repeat_max_count=max(all_reads.values()),
        assigned_class=assigned_class,
        is_train=is_train,
    )

def process_meta(meta):
    final_meta_combined = dict()
    for (train_label, is_train, is_take_all) in [("train", True, False), ("test", False, False), ("all", True, True)]:
        cls_counts = dict()
        cls_counts_reads = dict()
        all_reads_count = 0
        selected_datasets = {name: ds for (name, ds) in meta.items() if is_take_all or ds["is_train"] == is_train}
        for ds in selected_datasets.values():
            cls = ds["assigned_class"]
            all_reads_count += ds["read_count"]
            if cls not in cls_counts:
                cls_counts[cls] = 1
                cls_counts_reads[cls] = ds["read_count"]
            else:
                cls_counts[cls] += 1
                cls_counts_reads[cls] += ds["read_count"]

        final_meta = dict()
        final_meta["datasets"] = selected_datasets
        final_meta["classes_count"] = len(cls_counts.keys())
        final_meta["datasets_per_class"] = cls_counts
        final_meta["reads_per_class"] = cls_counts_reads

        nucl_probs_pos_cls = dict()
        # Probability per class
        for cls in cls_counts.keys():
            nucl_probs_pos = dict(A=[], C=[], T=[], G=[], N=[])
            for ds in selected_datasets.values():
                if ds["assigned_class"] == cls:
                    for (nucl, probs) in ds["nucl_probs_pos"].items():
                        if len(nucl_probs_pos[nucl]) < len(probs):
                            nucl_probs_pos[nucl] += [0] * (len(probs) - len(nucl_probs_pos[nucl]))
                        for (index, prob) in enumerate(probs):
                            nucl_probs_pos[nucl][index] += prob * ds["read_count"]
            for nucl in nucl_probs_pos.keys():
                nucl_probs_pos[nucl] = [prob / cls_counts_reads[cls] for prob in nucl_probs_pos[nucl]]
            nucl_probs_pos_cls[cls] = nucl_probs_pos
        final_meta["nucl_probs_pos_per_class"] = nucl_probs_pos_cls

        # Probability per everything
        nucl_probs_pos = dict(A=[], C=[], T=[], G=[], N=[])
        for cls in nucl_probs_pos_cls.keys():
            for (nucl, probs) in nucl_probs_pos_cls[cls].items():
                if len(nucl_probs_pos[nucl]) < len(probs):
                    nucl_probs_pos[nucl] += [0] * (len(probs) - len(nucl_probs_pos[nucl]))
                for (index, prob) in enumerate(probs):
                    nucl_probs_pos[nucl][index] += prob * cls_counts_reads[cls]
        for nucl in nucl_probs_pos.keys():
            nucl_probs_pos[nucl] = [prob / all_reads_count for prob in nucl_probs_pos[nucl]]
        final_meta["nucl_probs_pos"] = nucl_probs_pos
        nucl_probs_tot = {nucl: sum(probs)/len(probs) for (nucl, probs) in nucl_probs_pos.items()}
        final_meta["nucl_probs_tot"] = nucl_probs_tot
        
        # Combine
        final_meta_combined[train_label] = final_meta
    return final_meta_combined

# Entrypoint to the aligner
def run_classifier_pipeline(
    training_file_path: str,
    testing_file_path: str,
    output_file_path: str,
    ground_truth_file: str,
):
    # Thisable garbage collection, this should make memory usages a little bit better
    gc_disable()

    # Used to calculate the running time
    moment_start = time_ns()

    meta = dict()
    got_header = False
    with open(training_file_path) as traing_file:
        for line in traing_file:
            if got_header:
                split_line = line.split("\t")
                if len(split_line) < 2:
                    continue
                [fasta_file_gz, assigned_class, *_] = split_line
                # assert fasta_file_gz not in meta
                meta[fasta_file_gz] = load_dataset_meta(fasta_file_gz, assigned_class, True)
            got_header = True
    
    got_header = False
    with open(testing_file_path) as testing_file:
        for line in testing_file:
            if got_header:
                fasta_file_gz = line.strip()
                if len(fasta_file_gz) < 1:
                    continue
                # assert fasta_file_gz not in meta
                meta[fasta_file_gz] = load_dataset_meta(fasta_file_gz, "", False)
            got_header = True
    with open(output_file_path, 'w') as output_file:
        import json
        output_file.write(json.dumps(process_meta(meta)))

    moment_end = time_ns()
    print(f"Wrote records to {output_file_path} in {((moment_end-moment_start) // 10000000)/100} sec.")
    #speed_1m_sec = (((moment_end-moment_start) * (1000000 / _total_loaded_reads)) // 10000000) / 100
    #print(f"Avg. speed per 1M reads: {speed_1m_sec} sec. ({(speed_1m_sec // 6)/10} min.)")

def run_alignment_cli():
    run_classifier_pipeline(
        training_file_path=sys_argv[1],
        testing_file_path=sys_argv[2],
        output_file_path=sys_argv[3],
        ground_truth_file="./testing_ground_truth.tsv",
    )

if __name__ == '__main__':
    run_alignment_cli()
