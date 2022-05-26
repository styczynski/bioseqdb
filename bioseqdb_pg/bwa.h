#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <bwa/bwamem.h>
}

struct BwaSequence {
    std::string_view id;
    std::string_view seq;
};

struct BwaMatch {
    std::string_view ref_id;
    std::string ref_subseq;
    int64_t ref_match_begin;
    int64_t ref_match_end;
    int64_t ref_match_len;
    std::string_view query_subseq;
    int query_match_begin;
    int query_match_end;
    int query_match_len;
    bool is_primary;
    bool is_secondary;
    bool is_reverse;
    std::string cigar;
    int score;
};

class BwaIndex {
public:
    explicit BwaIndex(const std::vector<BwaSequence>& refs);
    ~BwaIndex();

    std::vector<BwaMatch> align_sequence(std::string_view query) const;

    mem_opt_t* options;

private:
    bwaidx_t* index;
    bool is_empty;
};
