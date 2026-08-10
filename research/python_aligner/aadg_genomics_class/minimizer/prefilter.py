"""
  Prefiltering ultities.

  @Piotr Styczyński 2023 <piotr@styczynski.in>
  MIT LICENSE
  Algorithms for genomic data analysis | AADG | MIM UW | Bioinformatyka
"""
from aadg_genomics_class.minimizer.minimizer import MinimizerIndex
import math
from aadg_genomics_class.monitoring.logs import LOGS
import numpy as np

def cleanup_kmer_index(
    index: MinimizerIndex,
    kmers_cutoff_f: float,
):
    keys = np.array(list(index.index.keys()))
    counts = np.array([index.index[kmer].size for kmer in keys])
    start_index = keys.size - math.floor(keys.size * kmers_cutoff_f)
    kmers_to_remove = np.lexsort((keys, counts))[start_index:]
    kmers_before = len(index.kmers)
    for kmer_pos in kmers_to_remove:
        index.index.pop(keys[kmer_pos], None)
        index.kmers.remove(keys[kmer_pos])
    kmers_after = len(index.kmers)
    LOGS.prefilter.info(f"Reduced target kmer count by prefiltering: {kmers_before} -> {kmers_after} (Eliminated {math.floor((kmers_before-kmers_after)/kmers_before*100000)/1000}% top kmers with f={kmers_cutoff_f})")

