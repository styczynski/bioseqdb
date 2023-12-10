#
# @Piotr Styczyński 2023 <piotr@styczynski.in>
# MIT LICENSE
# Algorithms for genomic data analysis | AADG | MIM UW | Bioinformatyka
#
import numpy as np
from numba import njit

@njit(cache=True, fastmath=True)
def nw_align(
    target,
    query,
    score_match,
    score_mismatch,
    score_gap,
):
    """
        Implementation of Needleman-Wunsch algorithm for arbitrary Numpy array sequences.
        The algorithm expects two numeric-like sequences and scores used to compute the best alignment.
        The result is tuple of found elements:
            - t_pad_left - Where first position of target was aligned to query
            - t_pad_right - Where the last position of target was aligned to query
            - q_pad_left - Where the first position of query was aligned to target
            - q_pad_right - Where the last position of query was aligned to target
        
        For example for:

            ACTA-GT-C-AAA-
            ---CT-ACTGAA--

            We could get (0, 1, 3, 2)

            Target has 0 dashes on the left side
            Target has 1 dash on the right side
            Query has 3 dashes on the left side
            Query has 2 dashes on the right side

    @Piotr Styczyński 2023
    """
    target_len, query_len = len(target), len(query)

    # Hold pointers to allow reconstruction of a sequence later
    score_pointers = np.zeros((target_len + 1, query_len + 1))
    score_pointers[:,0], score_pointers[0,:] = 3, 4

    # Optimal scores
    optimal_scores = np.zeros((target_len + 1, query_len + 1))
    optimal_scores[:,0], optimal_scores[0,:] = np.linspace(0, -target_len * score_gap, target_len + 1), np.linspace(0, -query_len * score_gap, query_len + 1)

    # Temporary score table
    scores = np.zeros(3)

    for target_pos in range(target_len):
        for query_pos in range(query_len):
            if target[target_pos] != query[query_pos]:
                scores[0] = optimal_scores[target_pos, query_pos] - score_mismatch
            else:
                scores[0] = optimal_scores[target_pos, query_pos] + score_match
            scores[1] = optimal_scores[target_pos, query_pos+1] - score_gap
            scores[2] = optimal_scores[target_pos+1, query_pos] - score_gap
            local_max = np.max(scores)
            optimal_scores[target_pos+1, query_pos+1] = local_max
            if scores[2] == local_max:
                score_pointers[target_pos+1, query_pos+1] += 4
            if scores[1] == local_max:
                score_pointers[target_pos+1, query_pos+1] += 3
            if scores[0] == local_max:
                score_pointers[target_pos+1, query_pos+1] += 2

    # Now we need to traverse the matrix and reconstruct the array
    # We hold *_ending_space to see if our traverse sequence starts with sequence of dashes: '-'
    # If it starts with dashes (the result sequence will be reversed), that means we have padding at the start of the sequence
    # We also look for consecutive sequences of dashes using *_longest_space variable
    # At the end we will read it to see what number of dashes occurr at the end of the sequence (remember everything is reversed) 
    t_ending_space, t_ending_space_len = True, 0
    q_ending_space, q_ending_space_len = True, 0
    t_longest_space, q_longest_space = 0, 0
    target_pos, query_pos = target_len, query_len

    while target_pos > 0 or query_pos > 0:
        if score_pointers[target_pos, query_pos] in [3, 5, 7, 9]:
            t_ending_space = False
            t_longest_space = 0
            q_longest_space += 1
            if q_ending_space:
                q_ending_space_len += 1
            target_pos -= 1
        elif score_pointers[target_pos, query_pos] in [2, 5, 6, 9]:
            t_ending_space = False
            q_ending_space = False
            t_longest_space = 0
            q_longest_space = 0
            target_pos -= 1
            query_pos -= 1
        elif score_pointers[target_pos, query_pos] in [4, 6, 7, 9]:
            q_ending_space = False
            t_longest_space == 1
            q_longest_space = 0
            if t_ending_space:
                t_ending_space_len += 1
            query_pos -= 1

    # Return paddings of target and query sequence in the resulting alignment.
    # For example for:
    #  ACTA-GT-C-AAA-
    #  ---CT-ACTGAA--
    # We would get (0, 1, 3, 2)
    #   Target has 0 dashes on the left side
    #   Target has 1 dash on the right side
    #   Query has 3 dashes on the left side
    #   Query has 2 dashes on the right side
    #
    return (t_longest_space, t_ending_space_len, q_longest_space, q_ending_space_len)