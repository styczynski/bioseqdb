#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstdint>

#include <htslib/htslib/sam.h>
extern "C" {
#include <bwa/bwt.h>
// Internal libbwa symbol, not exported through any of the headers.
int is_bwt(ubyte_t *T, int n);
}

#include "bwa.h"
#include "sequence.h"

#define raise_pg_error(code, msg) ereport(ERROR, (errcode(code)), msg);

inline namespace {
    // Modifined version of original bwa implementaion adjusted to our requirements.
    bwt_t* pac2bwt(const std::vector<ubyte_t>& pac_forward) {
        const ubyte_t* pac = pac_forward.data();
        size_t pac_len = pac_forward.size() * 4;

        // Calloc here is needed.
        bwt_t* bwt = (bwt_t*) calloc(1, sizeof(bwt_t));
        bwt->seq_len = pac_len * 2;
        bwt->bwt_size = (bwt->seq_len + 15) >> 4;

        ubyte_t* buf = (ubyte_t*) malloc(bwt->seq_len + 1);
        buf[bwt->seq_len] = 0;

        // TODO: optimize
        // Forward
        for (size_t i = 0; i < pac_len ; i++) {
            buf[i] = pac_raw_get(pac, i);
            bwt->L2[1 + buf[i]]++;
            bwt->L2[4 - buf[i]]++;
        }

        // Backward complement
        for (ssize_t i = pac_len - 1; i >= 0 ; i--)
            buf[2 * pac_len - 1 - i] = 0b11 - pac_raw_get(pac, i);

        for (int i = 2; i <= 4; ++i)
            bwt->L2[i] += bwt->L2[i - 1];

        bwt->primary = is_bwt(buf, bwt->seq_len);
        bwt->bwt = (u_int32_t*) calloc(bwt->bwt_size, 4);
        for (size_t i = 0; i < bwt->seq_len; ++i)
            bwt->bwt[i>>4] |= buf[i] << ((15 - (i & 15)) << 1);
        free(buf);
        return bwt;
    }

    std::string extract_reference_subseq(bwaidx_t* index, int64_t ref_begin, int64_t ref_end) {
        // TODO directly return PgNucleotideSequence (low priority).
        std::string subseq(ref_end - ref_begin, '?');
        for (int64_t i = 0; i < subseq.size(); ++i)
            subseq[i] = "ACGT"[pac_raw_get(index->pac, ref_begin + i)];
        // TODO: Use binary search to look for relevant holes (low priority).
        for (bntamb1_t* hole = index->bns->ambs; hole != index->bns->ambs + index->bns->n_holes; ++hole) {
            int64_t left_intersect = std::max(hole->offset, ref_begin);
            int64_t right_intersect = std::min(hole->offset + hole->len, ref_end);
            for (int64_t i = left_intersect; i < right_intersect; ++i)
                subseq[i - ref_begin] = hole->amb;
        }
        return subseq;
    }

    std::string cigar_compressed_to_string(const uint32_t *raw, int len) {
        std::string cigar;
        for (int i = 0; i < len; ++i) {
            cigar += std::to_string(bam_cigar_oplen(raw[i]));
            cigar += bam_cigar_opchr(raw[i]);
        }
        return cigar;
    }
}

BwaIndex::BwaIndex(): index(nullptr), pac_forward(), holes(), annotations(), options(mem_opt_init()) {}

void BwaIndex::add_ref_sequence(int64_t id, const NucleotideSequence& seq) {
    int64_t offset = pac_forward.size() * 4;
    auto& ref = annotations.emplace_back(bntann1_t {
        .offset = offset,
        .len = (int32_t) seq.len,
        .n_ambs = (int32_t) seq.holes_num,
        .gi = 0,
        .name = reinterpret_cast<char*>(id),
        .anno = nullptr,
    });

    // Reference sequence may be hundred of megabytes big, so we force usage of memcopy;
    // TODO: resize initilizes data. Use something else.
    size_t old_size = pac_forward.size();
    pac_forward.resize(old_size + pac_byte_size(seq.len));
    std::copy_n(seq.pac(), pac_byte_size(seq.len), pac_forward.data() + old_size);

    // There is not so much of holes in standand genome, so nicer code is better.
    std::transform(seq.holes(), seq.holes() + seq.holes_num, std::back_inserter(holes), [&offset](const auto& hole) {
        bntamb1_t ret = hole;
        ret.offset += offset;
        return hole;
    });
}

void BwaIndex::build() {
    if (pac_forward.empty())
        return;

    bwt_t* bwt = pac2bwt(pac_forward);
    bwt_bwtupdate_core(bwt);
    bwt_cal_sa(bwt, 32);
    bwt_gen_cnt_table(bwt);

    bntseq_t* bns = (bntseq_t*) calloc(1, sizeof(bntseq_t));
    bns->seed = 11;
    bns->l_pac = pac_forward.size() * 4;
    bns->n_seqs = annotations.size();
    bns->ambs = holes.data();
    bns->n_holes = holes.size();
    bns->anns = annotations.data();

    index = (bwaidx_t*) calloc(1, sizeof(bwaidx_t));
    index->bwt = bwt;
    index->bns = bns;
    index->pac = pac_forward.data();
}


BwaIndex::~BwaIndex() {
    // Manual deleation prevents libbwa from running free on vector.data().
    if (index != nullptr) {
        bwt_destroy(index->bwt);
        free(index->bns);
        free(index);
    }
    free(options);
}

std::vector<BwaMatch> BwaIndex::align_sequence(const NucleotideSequence& seq) const {
    if(pac_forward.empty())
        return {};
    // bwa algorithm is mainly used with very short query sequences (< 100 symbols) so cost of to_malloc_text here
    // is minimal.
    char* raw_query = seq.to_text_palloc();
    std::string_view query(raw_query);

    mem_alnreg_v aligns = mem_align1(options, index->bwt, index->bns, index->pac, query.length(), query.data()); // get all the hits (was c_str())
    std::vector<BwaMatch> matches;
    for (mem_alnreg_t* align = aligns.a; align != aligns.a + aligns.n; ++align) {
        // BWA returns the align->rid indicating which reference sequence was matched, but some fields refer to
        // positions withing the concatenated sequence, rather than in the specific sequence. For example, you need to
        // subtract ref_offset from align->rb and align->re to get positions relative to the original reference
        // sequence.
        // TODO: How do rb/re fields look in reverse matches?
        int64_t ref_offset = index->bns->anns[align->rid].offset;
        mem_aln_t details = mem_reg2aln(options, index->bns, index->pac, query.length(), query.data(), align);
        matches.push_back({
            .ref_id = reinterpret_cast<int64_t>(index->bns->anns[align->rid].name),
            .ref_subseq = extract_reference_subseq(index, align->rb, align->re),
            .ref_match_begin = static_cast<int32_t>(align->rb - ref_offset),
            .ref_match_end = static_cast<int32_t>(align->re - ref_offset),
            .ref_match_len = static_cast<int32_t>(align->re - align->rb),
            .query_subseq = query.substr(align->qb, align->qe - align->qb),
            .query_match_begin = align->qb,
            .query_match_end = align->qe,
            .query_match_len = align->qe - align->qb,
            .is_primary = (details.flag & BAM_FSECONDARY) == 0,
            .is_secondary = (details.flag & BAM_FSECONDARY) != 0,
            .is_reverse = details.is_rev != 0,
            // TODO: Revert the CIGAR string and/or subsequences when the match is reversed?
            .cigar = cigar_compressed_to_string(details.cigar, details.n_cigar),
            .score = details.score,
        });
        free(details.cigar);
    }

    free(aligns.a);
    return matches;
}
