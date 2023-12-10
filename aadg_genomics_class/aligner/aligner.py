"""
  Implementation of universal sequence aligner

  @Piotr Styczyński 2023 <piotr@styczynski.in>
  MIT LICENSE
  Algorithms for genomic data analysis | AADG | MIM UW | Bioinformatyka
"""
from aadg_genomics_class.minimizer.extender import RegionMatch
from .needleman_wunsch import nw_align

def normalize_pos(pos, len):
    return min(max(pos, 0), len-1)

def align(
    region: RegionMatch,
    target_seq,
    query_seq,
    kmer_len,
    full_query_len,
    score_match,
    score_mismatch,
    score_gap,
):
    relative_extension = kmer_len*3

    q_begin, q_end = region.q_begin-relative_extension, region.q_end+(kmer_len-1)+relative_extension
    t_begin, t_end = region.t_begin-relative_extension, min(region.t_end, region.t_begin + full_query_len)+(kmer_len-1)+relative_extension

    q_begin, q_end = normalize_pos(q_begin, len(query_seq)), normalize_pos(q_end, len(query_seq))
    t_begin, t_end = normalize_pos(t_begin, len(target_seq)), normalize_pos(t_end, len(target_seq))

    t_pad_left, t_pad_right, q_pad_left, q_pad_right = nw_align(
        target=target_seq[t_begin:t_end],
        query=query_seq[q_begin:q_end],
        score_match=score_match,
        score_mismatch=score_mismatch,
        score_gap=score_gap,
    )

    q_begin, q_end = q_begin + t_pad_left, q_end - t_pad_right
    t_begin, t_end = t_begin + q_pad_left, t_end - q_pad_right

    return (t_begin, t_end)