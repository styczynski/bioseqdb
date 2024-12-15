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
import multiprocessing as mp

from time import time_ns
from itertools import chain
from numpy.lib.stride_tricks import sliding_window_view
from operator import itemgetter
from collections import defaultdict

import math
import numpy as np
import mmh3

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

DATASET_CHUNK_SIZE = 60000

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

def load_data_class(class_name, datasets_paths, is_test):
    
    moment_start = time_ns()
    test_mul = 10 if is_test else 1
    reads_per_chunk = DATASET_CHUNK_SIZE * test_mul
    total_est_chunks = round(1000000/reads_per_chunk * 3)

    MAX_SIG_LEN_PER_CLASS = 1000000
    MAX_KMER_VALUE = 1000000000
    sig_per_step = round(MAX_SIG_LEN_PER_CLASS / total_est_chunks * 2.5)

    #SIG_LEN = 50000 * (2 if is_test else 1)
    seq_buffer = ""
    loaded_reads = 0
    total_reads = 0
    sig1 = []
    sig2 = []
    
    for fasta_file_gz in datasets_paths:
        part_sig1 = []
        part_sig2 = []

        for line in chain(gzip.open(fasta_file_gz, 'rt'), [">"]):
            if line[0] != '>':
                read = line.rstrip()
                total_reads += 1
                seq_buffer += read
                if __debug__:
                    if len(read) > DATASET_READ_MAX_SIZE:
                        print(f"Read exceeds max read size. Actual size: {len(read)} (max: {DATASET_READ_MAX_SIZE})")
                        exit(1)
                seq_buffer += "N" * (DATASET_READ_MAX_SIZE - len(read) + KMER_LEN)
                loaded_reads += 1
            elif loaded_reads > reads_per_chunk or len(line) == 1:
                if not is_test:
                    print(f"class={class_name}: Load dataset chunk {fasta_file_gz}")
                #seq_buffer_b = b''.join((mmh3.mmh3_x64_128_digest(seq_buffer[i:i+DATASET_READ_MAX_SIZE]) for i in range(0, len(seq_buffer), DATASET_READ_MAX_SIZE)))
                #seq_arr = np.frombuffer(seq_buffer_b, dtype=np.uint32)

                # Loaded training sequence
                seq_arr = MAPPING_FN(array(list(seq_buffer)))
                sequence_len = len(seq_arr)
                mask = generate_mask(KMER_LEN)

                # Function to compute kmer value based on the previous (on the left side) kmer value and new nucleotide
                uadd = frompyfunc(lambda x, y: ((x << 2) | y) & mask, 2, 1)

                # This computes values for kmers
                # uadd/accumulate combintation is pretty performant and I found no better way to speed things up
                kmers = uadd.accumulate(seq_arr, dtype=object).astype(int)
                #kmers[:KMER_LEN-2] = 0
                kmers = kmers[KMER_LEN-2:]
                # kmers = sort(kmers)

                #kappa = kmers[kmers < MAX_KMER_VALUE]
                kappa = kmers
                kappa = np.column_stack(np.unique(kappa, return_counts=True))   
                if is_test:
                    kappa[:,1] *= -1   
                kappa = kappa[kappa[:,0].argsort()]    

                part_sig1 += list(map(tuple(kappa[:sig_per_step].tolist())))
                part_sig2 += list(map(tuple(kappa[-sig_per_step:].tolist())))
          
                del kappa
                del kmers
                del seq_arr

                # Clear buffer
                seq_buffer = ""
                loaded_reads = 0
                #del seq_arr
        
        sig1 = sorted(sig1+part_sig1)[:MAX_SIG_LEN_PER_CLASS]
        sig2 = sorted(sig2+part_sig2)[:MAX_SIG_LEN_PER_CLASS]
            
    sig = (sig1, sig2)
    moment_end = time_ns()

    #print(f"Train: Total sig length for cls={class_name} is {sum((len(s) for s in sig))}")
    if not is_test:
        print(f"Train: Total sig length for cls={class_name} is {len(sig1)+len(sig2)}")
    speed_1m_sec = (((moment_end-moment_start) * (1000000 / total_reads)) // 10000000) / 100
    return (speed_1m_sec, sig)

def measure_class_distance(truth_class, test_path, test_sig, classes, training_classes):
    scores = []
    for cls in classes:
        doc1 = training_classes[cls]
        doc2 = test_sig

        # a1 = set(ds1[0])
        # b1 = set(ds1[1])
        # a2 = set(ds2[0])
        # b2 = set(ds2[1])

        # similarity_score = len(a1.intersection(a2)) / len(a1.union(a2)) + len(b1.intersection(b2)) / len(b1.union(b2))
        # ds1, ds2 = doc1, doc2
        # a1 = set(ds1[0])
        # b1 = set(ds1[1])
        # a2 = set(ds2[0])
        # b2 = set(ds2[1])
        # similarity_score = len(a1.intersection(a2)) / len(a1.union(a2)) + len(b1.intersection(b2)) / len(b1.union(b2))
        
        
        # similarity_score = 0
        # for ii in range(2):
        #     points = 0
        #     points_all = 0
        #     for kmer in doc1[ii]:
        #         if kmer in doc2[ii]:
        #             points += 1 + math.log(min(doc1[ii][kmer], doc2[ii][kmer]))
        #             points_all += 1 + math.log(max(doc1[ii][kmer], doc2[ii][kmer]))
        #         else:
        #             points_all += 1 + math.log(doc1[ii][kmer])
        #     for kmer in doc2[ii]:
        #         if kmer not in doc1[ii]:
        #             points_all += 1 + math.log(doc2[ii][kmer])
        #     similarity_score += points / points_all
        # scores.append(similarity_score)

        similarity_score = 0
        for ii in range(2):
            points = 0
            points_all = 0
            
            occ1 = 0
            occ2 = 0
            last_kmer = 0
            for (kmer, occ) in sorted(doc1[ii] + doc2[ii]):
                if kmer != last_kmer:
                    #print(f"{kmer} -> {occ1} {occ2}")
                    if occ1 > 0 and occ2 > 0:
                        points += 1 + math.log(min(occ1, occ2))
                        points_all += 1 + math.log(max(occ1, occ2))
                    elif occ1 > 0:
                        points_all += 1 + math.log(occ1)
                    elif occ2 > 0:
                        points_all += 1 + math.log(occ2)
                    occ1 = 0
                    occ2 = 0
                    if occ < 0:
                        occ2 += -occ
                    else:
                        occ1 += occ
                    last_kmer = kmer
                else:
                    if occ < 0:
                        occ2 += -occ
                    else:
                        occ1 += occ

            similarity_score += points / points_all
        scores.append(similarity_score)


    top_classes = [cls for (_, cls) in sorted([(scores[i], classes[i]) for i in range(len(classes))], reverse=True)]
    print(f"--> Classified {test_path} of {truth_class} as {top_classes}")
    return scores

from pympler.asizeof import asizeof
def _size_mb(data):
    size_mb = (asizeof(data) // 1000 // 10) / 100
    return size_mb

def load_data_class_mp(class_name, datasets_paths, is_test):
    (speed, sig) = load_data_class(class_name, datasets_paths, is_test)
    return (class_name, speed, sig, _size_mb(sig))

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
    l_1m_speeds = []

    gt_cls = dict()
    with open(ground_truth_file) as gt_file:
        k = (line.split('\t') for line in gt_file)
        next(k)
        for tokens in k:
            gt_cls[tokens[0]] = tokens[1]    

    #  training_datasets: class -> Array<dataset.path>
    training_datasets = defaultdict(lambda: [])
    with open(training_file_path) as training_file:
            training_datasets = defaultdict(lambda: [])
            k = (line.split('\t') for line in training_file)
            next(k)
            for tokens in k:
                training_datasets[tokens[1]].append(tokens[0])
    classes = list(sorted(training_datasets.keys()))
    
    pool = mp.Pool(processes=12)

    # Train
    # Multiprocessing
    # Setup a list of processes that we want to run
    results = pool.starmap(load_data_class_mp, [(cls, training_datasets[cls], False) for cls in classes])
    training_classes = { cls: cls_sig for (cls, _, cls_sig, _) in results }
    l_1m_speeds += [speed_1m for (_, speed_1m, _, _) in results]

    print(f"Signature size per class:")
    for (cls, _, _, size_mb) in results:
        print(f" ==> Class {cls} stores {size_mb} MB")
    print("==============================")
    print(f"TOTAL MEM ALLOCATED FOR SIGNATURES: {_size_mb(training_classes)} MB")
    print("==============================")

    #training_classes = { cls: load_data_class(cls, training_datasets[cls]) for cls in classes }

    # Test
    output_buf = ["\t".join(["fasta_file", *classes])+"\n"]
    p_args = []
    with open(testing_file_path) as testing_file:
            k = (line.strip() for line in testing_file)
            next(k)
            for testing_dataset_path in k:
                #p_args.append((testing_file_path, [testing_dataset_path],))
                (speed_1m, test_sig) = load_data_class("unknown_test", [testing_dataset_path], True)
                output_line = testing_dataset_path + "\t" + "\t".join([str(dist) for dist in measure_class_distance(gt_cls[testing_dataset_path], testing_dataset_path, test_sig, classes, training_classes)]) + "\n"
                print(output_line)
                output_buf.append(output_line)
                l_1m_speeds.append(speed_1m)
    # results = [pool.apply(load_data_class_mp, args=args) for args in p_args]
    # results_d = { key: value for (key, _, value) in results}
    # with open(testing_file_path) as testing_file:
    #         k = (line.strip() for line in testing_file)
    #         next(k)
    #         for testing_dataset_path in k:
    #             test_sig = results_d[testing_file_path]
    #             output_line = testing_dataset_path + "\t" + "\t".join([str(dist) for dist in measure_class_distance(test_sig, classes, training_classes)]) + "\n"
    #             output_buf.append(output_line)
    # l_1m_speeds += [speed_1m for (_, speed_1m, _) in results]

    # Output
    with open(output_file_path, 'w') as output_file:
        output_file.writelines(output_buf)

    moment_end = time_ns()
    print(f"Wrote records to {output_file_path} in {((moment_end-moment_start) // 10000000)/100} sec.")
    speed_1m_sec = sum(l_1m_speeds) / len(l_1m_speeds)
    print(f"Avg. speed per 1M reads: {speed_1m_sec} sec. ({(speed_1m_sec // 6)/10} min.) / max. 120 sec.")

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
        ground_truth_file=sys_argv[4],
    )

if __name__ == '__main__':
    run_alignment_cli()
