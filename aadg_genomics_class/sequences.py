from Bio import SeqRecord, SeqIO
import numpy as np
from typing import Iterable

MAPPING = dict(
    A=1,
    a=1,
    c=0,
    C=0,
    g=3,
    G=3,
    t=2,
    T=2,
)

COMPLEMENT_MAPPING = {
    1: 2,
    0: 3,
    3: 0,
    2: 1,
}

MAPPING_FN = np.vectorize(MAPPING.get)
COMPLEMENT_MAPPING_FN = np.vectorize(COMPLEMENT_MAPPING.get)

def format_sequences(src: Iterable[SeqRecord]):
    result = {record.id: MAPPING_FN(np.array(record.seq)) for record in src}
    return result, list(result.keys())

def iter_sequences(src: Iterable[SeqRecord]):
    return ((record.id, MAPPING_FN(np.array(record.seq))) for record in src)

def sequence_complement(seq):
     return COMPLEMENT_MAPPING_FN(seq)