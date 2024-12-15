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
import mmh3

from numpy import sort, frompyfunc, vectorize, uint8, int64, uint32, array, concatenate, add, argmin, arange, column_stack, unique, split, empty, ix_, take, diff
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

# mask[k] := mask used to calculate hash for k-mer of length k
_global_kmer_masks = None
_global_occ_mask = None

# Makes sure 0 <= pos <= len
def normalize_pos(pos, len):
    return min(max(pos, 0), len)

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
    

    mask, occ_mask, signo_mask = generate_masks(KMER_LEN)
 # MAGIC_MAX = ((((mask << (OCC_MASK_LEN)) | occ_mask) << 1) | 1) #| (signo_mask)
    # MAGIC_MAX_L = len(str(bin(MAGIC_MAX)).replace('0b', ''))
    # print(f"magic max = {MAGIC_MAX_L} / 63")
    # print(bin(MAGIC_MAX))
    # print(signo_mask)
   

    moment_start = time_ns()
    test_mul = 10 if is_test else 1
    reads_per_chunk = DATASET_CHUNK_SIZE * test_mul
    total_est_chunks = round(1000000/reads_per_chunk * 3)

    MAX_SIG_LEN_PER_CLASS = 3000000
    MAX_KMER_VALUE = 1000000000
    sig_per_step = 70000 * (2 if is_test else 1) #round(MAX_SIG_LEN_PER_CLASS / total_est_chunks * 0.82)
    print(f"sig per step is {sig_per_step}")

    seq_buffer = ""
    loaded_reads = 0
    total_reads = 0
    sig = np.array([], dtype=int64)
    split_no = 0
    
    for fasta_file_gz in datasets_paths:
        part_sig1 = dict()
        part_sig2 = dict()

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
                    print(f"class={class_name}: Load dataset chunk {fasta_file_gz}: {total_reads // 30000}%")
                #seq_buffer_b = b''.join((mmh3.mmh3_x64_128_digest(seq_buffer[i:i+DATASET_READ_MAX_SIZE]) for i in range(0, len(seq_buffer), DATASET_READ_MAX_SIZE)))
                #seq_arr = np.frombuffer(seq_buffer_b, dtype=np.uint32)

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
                kappa = np.column_stack(np.unique(kmers, return_counts=True))      
                kappa = kappa[kappa[:,0].argsort()]      

                for (kmer, occ_count) in kappa[:sig_per_step].tolist():
                    part_sig1[kmer] = part_sig1.get(kmer, 0) + occ_count
                for (kmer, occ_count) in kappa[-sig_per_step:].tolist():
                    part_sig2[kmer] = part_sig2.get(kmer, 0) + occ_count

                #part_sig1 += list(map(tuple, kappa[:sig_per_step].tolist()))
                #part_sig2 += list(map(tuple, kappa[-sig_per_step:].tolist()))

                if (split_no != total_reads // 200000 and not is_test) or len(line) == 1:
                    if not is_test:
                        print(f"SPLIT LOAD {fasta_file_gz} no. {split_no}")
                    split_no = total_reads // 200000
                    if is_test:
                        sig = np.concatenate((
                            sig,
                            np.array([((((kmer << OCC_MASK_LEN) | ((occ) & occ_mask)) << 1) | 1 ) for (kmer, occ) in part_sig1.items()], dtype=int64),
                            np.array([((((kmer << OCC_MASK_LEN) | ((occ) & occ_mask)) << 1) | 1 | signo_mask ) for (kmer, occ) in part_sig2.items()], dtype=int64)
                        ), dtype=int64)
                        #sig1x = np.array([((((kmer << OCC_MASK_LEN) | (occ & OCC_MASK)) << 1) | 1 ) for (kmer, occ) in part_sig1.items()])
                        #sig2x = np.array([((((kmer << OCC_MASK_LEN) | (occ & OCC_MASK)) << 1) | 1 | SIGNO_MASK ) for (kmer, occ) in part_sig2.items()])
                    else:
                        sig = np.concatenate((
                            sig,
                            np.array([((((kmer << OCC_MASK_LEN) | ((occ) & occ_mask)) << 1) ) for (kmer, occ) in part_sig1.items()], dtype=int64),
                            np.array([((((kmer << OCC_MASK_LEN) | ((occ) & occ_mask)) << 1) | signo_mask ) for (kmer, occ) in part_sig2.items()], dtype=int64)
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
                #del seq_arr
        
        del part_sig1
        del part_sig2
        gc_collect()
        gc_collect()

    #sig = (np.array(sig1), np.array(sig2))
    # Compress and process
    moment_end = time_ns()

    #del sig1x
    #del sig2x 
    #del sig1 
    #del sig2

    #print(f"Train: Total sig length for cls={class_name} is {sum((len(s) for s in sig))}")
    if not is_test:
        print(f"Train: Total sig length for cls={class_name} is {len(sig)}")
    speed_1m_sec = (((moment_end-moment_start) * (1000000 / total_reads)) // 10000000) / 100
    gc_collect()
    return (speed_1m_sec, sig)

def measure_class_distance(truth_class, test_path, test_sig, classes, training_classes):
    moment_start = time_ns()
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
        combine = np.sort(np.concatenate((doc1,doc2), dtype=int64))
        split_pos = np.argmax((combine & signo_mask) != 0)
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
    top_classes = [cls for (_, cls) in sorted([(scores[i], classes[i]) for i in range(len(classes))], reverse=True)]
    moment_end = time_ns()
    print(f"--> Classified {test_path} of {truth_class} as {top_classes}")
    speed_1m_sec = (((moment_end-moment_start) * (1000000 / 100000)) // 10000000) / 100
    print(f"Avg. speed per 1M reads: {speed_1m_sec} sec. ({(speed_1m_sec // 6)/10} min.) / max. 120 sec.")
    return scores

from pympler.asizeof import asizeof
def _size_mb(data):
    size_mb = (asizeof(data) // 1000 // 10) / 100
    return size_mb

def load_data_class_mp(class_name, datasets_paths, is_test):
    (speed, sig) = load_data_class(class_name, datasets_paths, is_test)
    return (class_name, speed, sig, _size_mb(sig))

def process_test_mp(testing_dataset_path, preload_test, speed_1m, test_sig, training_classes, classes, gt_cls):
    return (testing_dataset_path + "\t" + "\t".join([str(dist) for dist in measure_class_distance(gt_cls[testing_dataset_path], testing_dataset_path, test_sig, classes, training_classes)]) + "\n")

# Entrypoint to the aligner
def run_classifier_pipeline(
    training_file_path: str,
    testing_file_path: str,
    output_file_path: str,
    ground_truth_file: str,
    test_dump_file: str,
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
    l_1m_speeds = []

    # import pickle
    # with open(test_dump_file, 'rb') as dump_file:
    #     training_classes = pickle.load(dump_file)
    # print(f"[FETCH] Loaded pretrain data")
    # print("==============================")

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

    # Train
    # Multiprocessing
    # Setup a list of processes that we want to run
    if preload_train:
        import pickle
        with open(test_dump_file, 'rb') as dump_file:
            training_classes = pickle.load(dump_file)
        print(f"Loaded pretrain data")
        print("==============================")
    else:
        if super_debug:
            import multiprocessing as mp
            pool = mp.Pool(processes=12)
            results = pool.starmap(load_data_class_mp, [(cls, training_datasets[cls], False) for cls in classes])
            training_classes = { cls: cls_sig for (cls, _, cls_sig, _) in results }
            l_1m_speeds += [speed_1m for (_, speed_1m, _, _) in results]

            print(f"Signature size per class:")
            for (cls, _, _, size_mb) in results:
                print(f" ==> Class {cls} stores {size_mb} MB")
            print("==============================")
        else:
            #training_classes = dict()
            for cls in classes:
                (speed_1m, sig) = load_data_class(cls, training_datasets[cls], False)
                training_classes[cls] = sig
                l_1m_speeds.append(speed_1m)
        if dump_file:
            import pickle
            with open(test_dump_file, 'wb') as dump_file:
                pickle.dump(training_classes, dump_file)
                print(f"Dumped train data to {test_dump_file}")

    #print(f"TOTAL MEM ALLOCATED FOR SIGNATURES: {_size_mb(training_classes)} MB")
    #print("==============================")

    #training_classes = { cls: load_data_class(cls, training_datasets[cls]) for cls in classes }

    #return 0

    # Test
    output_buf = ["\t".join(["fasta_file", *classes])+"\n"]
    if not super_debug:
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
    else:
        import multiprocessing as mp
        pool = mp.Pool(processes=12)
        p_args = []
        with open(testing_file_path) as testing_file:
                k = (line.strip() for line in testing_file)
                next(k)
                for testing_dataset_path in k:
                    p_args.append((testing_dataset_path, [testing_dataset_path], True))
        p_args2 = []
        if preload_test:
            allall = []
            with open(test_dump_file+".test.pkl", 'rb') as dump_file:
                allall = pickle.load(dump_file)
            for (testing_dataset_path, speed_1m, test_sig, xxx) in allall:
                p_args2.append((testing_dataset_path, preload_test, speed_1m, test_sig, training_classes, classes, gt_cls))
                l_1m_speeds.append(speed_1m)
            for output_line in pool.starmap(process_test_mp, p_args2):
                print(output_line)
                output_buf.append(output_line)
        else:
            allall = []
            for (testing_dataset_path, speed_1m, test_sig, xxx) in pool.starmap(load_data_class_mp, p_args):
                p_args2.append((testing_dataset_path, preload_test, speed_1m, test_sig, training_classes, classes, gt_cls))
                l_1m_speeds.append(speed_1m)
                if dump_tests:
                    allall.append((testing_dataset_path, speed_1m, test_sig, xxx))
            if dump_tests:
                import pickle
                with open(test_dump_file+".test.pkl", 'wb') as dump_file:
                    pickle.dump(allall, dump_file)
                    print(f"Dumped test data to {test_dump_file+'.test.pkl'}")
            for output_line in pool.starmap(process_test_mp, p_args2):
                #output_line = testing_dataset_path + "\t" + "\t".join([str(dist) for dist in measure_class_distance(gt_cls[testing_dataset_path], testing_dataset_path, test_sig, classes, training_classes)]) + "\n"
                print(output_line)
                output_buf.append(output_line)

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

    if super_debug:
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
        test_dump_file=sys_argv[5],
    )

if __name__ == '__main__':
    run_alignment_cli()
