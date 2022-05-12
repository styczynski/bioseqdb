#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <bwa/bwamem.h>
#include <bwa/kseq.h>
KSEQ_DECLARE(gzFile)
}

struct BwaSequence {
    std::string_view id;
    std::string_view seq;
};

struct BwaMatch {
    std::string ref_id;
    int64_t ref_match_begin;
    int64_t ref_match_end;
    std::string query_subseq;
    int query_match_begin;
    int query_match_end;
    bool is_primary;
    bool is_secondary;
    bool is_reverse;
    std::string cigar;
    int score;
};

class BwaIndex {
public:
    BwaIndex();

    ~BwaIndex();

    void build_index(const std::vector<BwaSequence>& v);

    std::vector<BwaMatch> align_sequence(std::string_view read_nucleotides) const;

private:
    // Store the options in memory
    mem_opt_t * memopt;

    // hold the full index structure
    bwaidx_t* idx;

    // overwrite the bwa bwt_pac2pwt function
    bwt_t *seqlib_bwt_pac2bwt(const uint8_t *pac, int bwt_seq_lenr);

    // add an anns (chr annotation structure)
    bntann1_t* seqlib_add_to_anns(std::string_view name, std::string_view seq, bntann1_t * ann, size_t offset);

    // overwrite the bwa-mem add1 function, which takes a sequence and adds to pac
    uint8_t* seqlib_add1(const kseq_t *seq, bntseq_t *bns, uint8_t *pac, int64_t *m_pac, int *m_seqs, int *m_holes, bntamb1_t **q);

    // make the pac structure (2-bit encoded packed sequence)
    uint8_t* seqlib_make_pac(const std::vector<BwaSequence>& v, bool for_only);
};
