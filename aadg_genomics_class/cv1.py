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

_total_loaded_reads = 0

def load_dataset(fasta_file_gz):
    global _total_loaded_reads
    SIG_LEN = 5000
    seq_buffer = ""
    sig1 = []
    sig2 = []
    base_chunk_size = DATASET_CHUNK_SIZE * DATASET_READ_MAX_SIZE
    for line in chain(gzip.open(fasta_file_gz, 'rt'), [">"]):
        if line[0] != '>':
            read = line.rstrip()
            _total_loaded_reads += 1
            seq_buffer += read
            if __debug__:
                if len(read) > DATASET_READ_MAX_SIZE:
                    print(f"Read exceeds max read size. Actual size: {len(read)} (max: {DATASET_READ_MAX_SIZE})")
                    exit(1)
            seq_buffer += "N" * (DATASET_READ_MAX_SIZE - len(read))
        elif len(seq_buffer) > base_chunk_size or len(line) == 1:
            print(f"Load dataset chunk {fasta_file_gz}")
            # Loaded training sequence
            seq_arr = MAPPING_FN(array(list(seq_buffer)))
            sequence_len = len(seq_arr)
            mask = generate_mask(KMER_LEN)

            # Function to compute kmer value based on the previous (on the left side) kmer value and new nucleotide
            uadd = frompyfunc(lambda x, y: ((x << 2) | y) & mask, 2, 1)

            # This computes values for kmers
            # uadd/accumulate combintation is pretty performant and I found no better way to speed things up
            kmers = uadd.accumulate(seq_arr, dtype=object).astype(int)
            kmers[:KMER_LEN-2] = 0
            kmers = sort(kmers)
            
            sig1 += kmers[KMER_LEN-2:KMER_LEN-2+SIG_LEN].tolist()
            sig2 += kmers[-SIG_LEN:].tolist()

            # Clear buffer
            seq_buffer = ""
            del seq_arr

    return (sig1, sig2)

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

    # training_data [assigned_class][i] -> 
    training_data = defaultdict(lambda: [])

    got_header = False
    with open(training_file_path) as traing_file:
        for line in traing_file:
            if got_header:
                split_line = line.split("\t")
                if len(split_line) < 2:
                    continue
                [fasta_file_gz, assigned_class, *_] = split_line
                training_data[assigned_class].append(load_dataset(fasta_file_gz))
            got_header = True
    
    got_header = False
    classes = sorted(training_data.keys())
    output_buf = ["\t".join(["fasta_file", *classes])+"\n"]
    with open(testing_file_path) as testing_file:
        for line in testing_file:
            if got_header:
                fasta_file_gz = line.strip()
                if len(fasta_file_gz) < 1:
                    continue
                ds_test = load_dataset(fasta_file_gz)
                dists = [max(dist_dataset(ds_train, ds_test) for ds_train in training_data[data_class]) for data_class in classes]
                output_line = fasta_file_gz + "\t" + "\t".join([str(dist) for dist in dists]) + "\n"
                output_buf.append(output_line)
            got_header = True
    with open(output_file_path, 'w') as output_file:
        output_file.writelines(output_buf)

    moment_end = time_ns()
    print(f"Wrote records to {output_file_path} in {((moment_end-moment_start) // 10000000)/100} sec.")
    speed_1m_sec = (((moment_end-moment_start) * (1000000 / _total_loaded_reads)) // 10000000) / 100
    print(f"Avg. speed per 1M reads: {speed_1m_sec} sec. ({(speed_1m_sec // 6)/10} min.)")

    if __debug__:
        import pandas as pd
        from sklearn.metrics import roc_auc_score
        output = pd.read_csv(output_file_path, sep='\t')
        ground_truth = pd.read_csv(ground_truth_file, sep='\t')
        # Ensure that fasta_files match
        if not all(output['fasta_file'] == ground_truth['fasta_file']):
            raise ValueError("fasta_files in output and ground truth files do not match.")

        # Get the list of classes from the output file (excluding the 'fasta_file' column)
        classes = output.columns[1:]

        # Calculate AUC-ROC for each class
        auc_scores = []
        for this_class in classes:
            # Ground truth binary labels for the current class
            true_labels = (ground_truth.iloc[:, 1] == this_class).astype(int)

            # Predicted values for the current class
            predicted_scores = output[this_class]

            # Calculate AUC-ROC if there is at least one positive and one negative label
            if true_labels.nunique() > 1:
                auc = roc_auc_score(true_labels, predicted_scores)
                auc_scores.append(auc)
                print(f"AUC-ROC for class {this_class}: {auc:.4f}")
            else:
                print(f"Skipping class {this_class} due to lack of positive/negative samples.")

        # Compute the average AUC-ROC
        average_auc = sum(auc_scores) / len(auc_scores) if auc_scores else 0
        print(f"Average AUC-ROC across all classes: {average_auc:.4f}")

def run_alignment_cli():
    run_classifier_pipeline(
        training_file_path=sys_argv[1],
        testing_file_path=sys_argv[2],
        output_file_path=sys_argv[3],
        ground_truth_file="./testing_ground_truth.tsv",
    )

if __name__ == '__main__':
    run_alignment_cli()
