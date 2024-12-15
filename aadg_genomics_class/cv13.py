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

import math
import numpy as np

from numpy import sort, frompyfunc, vectorize, uint8, int64, uint32, array, concatenate, add, argmin, argmax, sort, arange, column_stack, unique, split, empty, ix_, take, diff
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
KMER_LEN = 20 # was: 16
# Length of window to generate (k,w)-minimizer indices
WINDOW_LEN = 8 # was: 5

DATASET_READ_MAX_SIZE = 152

DATASET_CHUNK_SIZE = 60000

SIGNO_POS = 62
OCC_MASK_LEN = 16

_global_kmer_masks = None
_global_occ_mask = None

# Generate hash mask for given kmer length
# The mask will be just 2*k last bits on for given k-mer len: k
# e.g mask(3) = b...000000111111
def generate_masks(
    kmer_len: int,
) -> int:
    global _global_kmer_masks
    global _global_occ_mask
    if not _global_kmer_masks:
        _global_kmer_masks = dict()
        ret = 3
        for i in range(MAX_KMER_SIZE+1):
            ret = (ret << 2) | 3
            _global_kmer_masks[i] = ret
    if not _global_occ_mask:
        _global_occ_mask = 1
        for i in range(OCC_MASK_LEN-1):
            _global_occ_mask = (_global_occ_mask << 1) | 1
    return (_global_kmer_masks[kmer_len], _global_occ_mask, (1 << SIGNO_POS))


def load_data_class(class_name, datasets_paths, is_test):
    str_pad = "N" * KMER_LEN
    mask, occ_mask, signo_mask = generate_masks(KMER_LEN)

    moment_start = time_ns()
    test_mul = 3 if is_test else 1
    reads_per_chunk = DATASET_CHUNK_SIZE * test_mul
    total_est_chunks = round(1000000/reads_per_chunk * 3)

    MAX_SIG_LEN_PER_CLASS = 3000000
    MAX_KMER_VALUE = 1000000000
    sig_per_step = 70000 * (2 if is_test else 1)

    seq_buffer = ""
    loaded_reads = 0
    total_reads = 0
    sig = array([], dtype=int64)
    split_no = 0
    
    for fasta_file_gz in datasets_paths:
        part_sig1 = dict()
        part_sig2 = dict()

        for line in chain(gzip.open(fasta_file_gz, 'rt'), [">"]):
            if line[0] != '>':
                read = line.rstrip()
                total_reads += 1
                seq_buffer += read
                seq_buffer += str_pad
                loaded_reads += 1
            elif loaded_reads > reads_per_chunk or len(line) == 1:
                # Loaded training sequence
                seq_arr = MAPPING_FN(array(list(seq_buffer)))
                sequence_len = len(seq_arr)

                # Function to compute kmer value based on the previous (on the left side) kmer value and new nucleotide
                uadd = frompyfunc(lambda x, y: ((x << 2) | y) & mask, 2, 1)

                # This computes values for kmers
                # uadd/accumulate combintation is pretty performant and I found no better way to speed things up
                kmers = uadd.accumulate(seq_arr, dtype=object).astype(int)
                #kmers[:KMER_LEN-2] = 0
                kmers = kmers[KMER_LEN-2:]
                # kmers = sort(kmers)

                #kappa = kmers[kmers < MAX_KMER_VALUE]
                kappa = column_stack(unique(kmers, return_counts=True))      
                kappa = kappa[kappa[:,0].argsort()]      

                for (kmer, occ_count) in kappa[:sig_per_step].tolist():
                    part_sig1[kmer] = part_sig1.get(kmer, 0) + occ_count
                for (kmer, occ_count) in kappa[-sig_per_step:].tolist():
                    part_sig2[kmer] = part_sig2.get(kmer, 0) + occ_count

                if (split_no != total_reads // 200000) or len(line) == 1:
                    split_no = total_reads // 200000
                    if is_test:
                        sig = concatenate((
                            sig,
                            array([((((kmer << OCC_MASK_LEN) | ((occ) & occ_mask)) << 1) | 1 ) for (kmer, occ) in part_sig1.items()], dtype=int64),
                            array([((((kmer << OCC_MASK_LEN) | ((occ) & occ_mask)) << 1) | 1 | signo_mask ) for (kmer, occ) in part_sig2.items()], dtype=int64)
                        ), dtype=int64)
                    else:
                        sig = concatenate((
                            sig,
                            array([((((kmer << OCC_MASK_LEN) | ((occ) & occ_mask)) << 1) ) for (kmer, occ) in part_sig1.items()], dtype=int64),
                            array([((((kmer << OCC_MASK_LEN) | ((occ) & occ_mask)) << 1) | signo_mask ) for (kmer, occ) in part_sig2.items()], dtype=int64)
                        ), dtype=int64)
                    del part_sig1
                    del part_sig2
                    part_sig1 = dict()
                    part_sig2 = dict()
                    gc_collect()
                    gc_collect()
          
                del kappa
                del kmers
                del seq_arr

                # Clear buffer
                seq_buffer = ""
                loaded_reads = 0
        
        del part_sig1
        del part_sig2
        gc_collect()
        gc_collect()

    moment_end = time_ns()
    gc_collect()
    return sig

def measure_class_distance(truth_class, test_path, test_sig, classes, training_classes):
    mask, occ_mask, signo_mask = generate_masks(KMER_LEN)
    scores = []
    for cls in classes:
        doc1 = training_classes[cls]
        doc2 = test_sig
        similarity_score = 0
        points = 0
        points_all = 0
        occ1 = 0
        occ2 = 0
        last_kmer = 0
        combine = sort(concatenate((doc1,doc2), dtype=int64))
        split_pos = argmax((combine & signo_mask) != 0)
        for (sig_start, sig_end) in [(0, split_pos), (split_pos, len(combine))]:
            frag = combine[sig_start:sig_end]
            kmers = (((frag >> 1) & mask) >> OCC_MASK_LEN).tolist()
            occs = ((frag >> 1) & occ_mask).tolist()
            is_tests = (frag & 1).tolist()
            frag_len = len(frag)
            frag_last = frag_len - 1
            frag = None
            del frag
            gc_collect()
            for i in range(frag_len):
                is_test = is_tests[i]
                kmer = kmers[i]
                occ = occs[i]
                if i == frag_last:
                    similarity_score += points / points_all 
                    points = 0
                    points_all = 0
                    if occ1 > 0 and occ2 > 0:
                        points += len(bin(min(occ1, occ2)))
                        points_all += len(bin(max(occ1, occ2)))
                    elif occ1 > 0:
                        points_all += len(bin(occ1))
                    elif occ2 > 0:
                        points_all += len(bin(occ2))
                    occ1 = 0
                    occ2 = 0
                    if is_test:
                        occ2 += occ
                    else:
                        occ1 += occ
                elif kmer != last_kmer:
                    if occ1 > 0 and occ2 > 0:
                        points += len(bin(min(occ1, occ2)))
                        points_all += len(bin(max(occ1, occ2)))
                    elif occ1 > 0:
                        points_all += len(bin(occ1))
                    elif occ2 > 0:
                        points_all += len(bin(occ2))
                    occ1 = 0
                    occ2 = 0
                    if is_test:
                        occ2 += occ
                    else:
                        occ1 += occ
                else:
                    if is_test:
                        occ2 += occ
                    else:
                        occ1 += occ
                last_kmer = kmer
        scores.append(similarity_score)
    del combine
    gc_collect()
    return scores

# Entrypoint to the aligner
def run_classifier_pipeline(
    training_file_path: str,
    testing_file_path: str,
    output_file_path: str,
):

    preload_train = True
    super_debug = True
    dump_file = False
    dump_tests = False
    preload_test = True

    # Thisable garbage collection, this should make memory usages a little bit better
    gc_disable()

    # Used to calculate the running time
    moment_start = time_ns()

    #  training_datasets: class -> Array<dataset.path>
    training_datasets = defaultdict(lambda: [])
    with open(training_file_path) as training_file:
            training_datasets = defaultdict(lambda: [])
            k = (line.split('\t') for line in training_file)
            next(k)
            for tokens in k:
                training_datasets[tokens[1]].append(tokens[0])
    classes = list(sorted(training_datasets.keys()))

    # Training
    training_classes = { cls: load_data_class(cls, training_datasets[cls], False) for cls in classes }
    
    # Evaluation
    output_buf = ["\t".join(["fasta_file", *classes])+"\n"]
    with open(testing_file_path) as testing_file:
        k = (line.strip() for line in testing_file)
        next(k)
        for testing_dataset_path in k:
            test_sig = load_data_class("unknown_test", [testing_dataset_path], True)
            output_line = testing_dataset_path + "\t" + "\t".join([str(dist) for dist in measure_class_distance(gt_cls[testing_dataset_path], testing_dataset_path, test_sig, classes, training_classes)]) + "\n"
            print(output_line)
            output_buf.append(output_line)
    
    # Output
    with open(output_file_path, 'w') as output_file:
        output_file.writelines(output_buf)

    moment_end = time_ns()
    print(f"Wrote records to {output_file_path} in {((moment_end-moment_start) // 10000000)/100} sec.")

def run_alignment_cli():
    run_classifier_pipeline(
        training_file_path=sys_argv[1],
        testing_file_path=sys_argv[2],
        output_file_path=sys_argv[3],
    )

if __name__ == '__main__':
    run_alignment_cli()
