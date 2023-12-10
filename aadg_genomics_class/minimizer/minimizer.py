import numpy as np
from itertools import chain
from numpy.lib.stride_tricks import sliding_window_view
from dataclasses import dataclass
from typing import Dict, Any, Set, Optional
from aadg_genomics_class.sequences import sequence_complement

MASKS: Optional[Dict[int, int]] = None
MAX_KMER_SIZE = 64

def generate_mask(
    kmer_len: int,
) -> int:
    global MASKS
    if not MASKS:
        MASKS = dict()
        ret = 3
        for i in range(MAX_KMER_SIZE+1):
            ret = (ret << 2) | 3
            MASKS[i] = ret
    return MASKS[kmer_len]

@dataclass
class MinimizerIndex:
    index: Dict[int, Any]
    kmers: Set[int]

def get_minimizers(
    seq_arr,
    kmer_len,
    window_len,
) -> MinimizerIndex:

    sequence_len = len(seq_arr)
    r_seq_arr = sequence_complement(seq_arr)

    mask = generate_mask(kmer_len)

    uadd = np.frompyfunc(lambda x, y: ((x << 2) | y) & mask, 2, 1)
    # We take into account only kmers[kmer_len-1:]
    kmers = uadd.accumulate(seq_arr, dtype=object).astype(int)
    kmers[:kmer_len-2] = 0

    r_kmers = uadd.accumulate(r_seq_arr, dtype=object).astype(int)
    r_kmers[:kmer_len-2] = 0

    kmers_min_pos = np.add(np.argmin(sliding_window_view(kmers, window_shape=window_len), axis=1), range(sequence_len - window_len + 1))
    r_kmers_min_pos = np.add(np.argmin(sliding_window_view(r_kmers, window_shape=window_len), axis=1), range(sequence_len - window_len + 1))

    tt = np.argmin(np.column_stack((
        r_kmers[r_kmers_min_pos],
        kmers[kmers_min_pos],
    )), axis=1).astype(dtype=bool)

    u = np.column_stack((
        np.where(tt, kmers[kmers_min_pos], r_kmers[r_kmers_min_pos]),
        np.where(tt, kmers_min_pos, r_kmers_min_pos),
        tt,
    ))[kmer_len:]

    u = u[u[:, 0].argsort()]

    a = np.unique(u, axis=0)
    unique_idx = np.unique(a[:, 0], return_index=True)[1][1:]
    zzz = np.split(a[:, 1:], unique_idx)

    result = dict(zip(chain([a[0, 0]], a[unique_idx, 0]), zzz))

    return MinimizerIndex(
        index=result,
        kmers=set(result.keys()),
    )