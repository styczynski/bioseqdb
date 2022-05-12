#include "bwa.h"

#include <algorithm>
#include <cstring>

#include <htslib/htslib/sam.h>
extern "C" {
// Internal libbwa symbol, not exported through any of the headers.
int is_bwt(ubyte_t *T, int n);
}

inline namespace {

class PacVector {
public:
    uint8_t get(uint64_t index) const;

    void set(uint64_t index, uint8_t value);

    void push_back(uint8_t value) {
        if (n % 4 == 0)
            pac.push_back(0);
        set(n++, value);
    }

    uint8_t* data() {
        return pac.data();
    }

    size_t size() const {
        return n;
    }

    size_t byte_size() const {
        return pac.size();
    }

private:
    std::vector<uint8_t> pac;
    size_t n = 0;
};

struct CompressedReference {
    PacVector pac_bwa;
    PacVector pac_forward;
    bntseq_t* bns;
};

char* make_c_string(std::string_view str_view) {
    char* c_str = (char*) malloc(str_view.length() + 1);
    std::copy(str_view.begin(), str_view.end(), c_str);
    c_str[str_view.length()] = '\0';
    return c_str;
}

uint8_t pac_raw_get(const uint8_t* pac, uint64_t index) {
    return pac[index >> 2] >> ((~index & 3) << 1) & 3;
}

void pac_raw_set(uint8_t* pac, uint64_t index, uint8_t value) {
    pac[index >> 2] |= value << ((~index & 3) << 1);
}

uint8_t PacVector::get(uint64_t index) const {
    return pac_raw_get(pac.data(), index);
}

void PacVector::set(uint64_t index, uint8_t value) {
    pac_raw_set(pac.data(), index, value);
}

char nuclcode_to_char(int nucl) {
    if (nucl == 0)
        return 'A';
    else if (nucl == 1)
        return 'C';
    else if (nucl == 2)
        return 'G';
    else if (nucl == 3)
        return 'T';
    else
        return 'N';
}

int nuclcode_from_char(char chr) {
    return nst_nt4_table[(int) chr];
}

// BWA requires nucleotide sequences to be passed using 2-bit encoding, where 0, 1, 2 and 3 correspond to ACGT
// respectively. Unknown nucleotides are represented by a random value, and description of groups of unknown values
// ("holes") is passed as an additional parameter.
void compress_reference_seq(const BwaSequence& ref, bntseq_t* bns, PacVector& pac, std::vector<bntamb1_t>& holes) {
    bntann1_t& ann = bns->anns[bns->n_seqs];
    ann.name = make_c_string(ref.id);
    ann.anno = make_c_string("(null)");
    ann.gi = 0;
    ann.len = ref.seq.length();
    ann.offset = bns->n_seqs == 0 ? 0 : bns->anns[bns->n_seqs - 1].offset + bns->anns[bns->n_seqs - 1].len;
    ann.n_ambs = 0;

    bntamb1_t* current_hole = nullptr;
    char prev_chr = 0;
    for (char chr : ref.seq) {
        int nucl = nuclcode_from_char(chr);

        if (nucl >= 4) {
            if (prev_chr == chr)
                ++current_hole->len;
            else {
                holes.push_back({
                    .offset = bns->l_pac,
                    .len = 1,
                    .amb = chr,
                });
                current_hole = &holes.back();
                ++ann.n_ambs;
                ++bns->n_holes;
            }
        }

        prev_chr = chr;

        if (nucl >= 4)
            nucl = lrand48() & 3;
        pac.push_back(nucl);
        ++bns->l_pac;
    }

    ++bns->n_seqs;
}

// The original sequence needs to be converted into three objects. To build an index, you need to pass a 2-bit encoded
// sequence of all reference nucleotides, concatenated with a 2-bit encoded sequence of the reverse of the complement of
// all reference nucleotides (pac_bwa). Extracting detailed results later requires a 2-bit encoded sequence of all
// reference nucleotides, without the reverse complement (pac_forward). The third object is the bns structure, which
// provides information about metadata and boundaries of individual reference sequences, as well as a list of all holes.
CompressedReference compress_reference(const std::vector<BwaSequence>& refs) {
    bntseq_t* bns = (bntseq_t*) calloc(1, sizeof(bntseq_t));
    // BWA encodes holes (unknown nucleotides) as a random valid nucleotide in its 2-bit encoding. This can produce
    // nondeterministic results, so the RNG seed is fixed to 11 to prevent this.
    bns->seed = 11;
    bns->anns = (bntann1_t*) calloc(refs.size(), sizeof(bntann1_t));

    PacVector pac_forward;

    std::vector<bntamb1_t> holes;
    holes.reserve(8);

    for (const BwaSequence& ref : refs)
        compress_reference_seq(ref, bns, pac_forward, holes);

    bns->ambs = (bntamb1_t*) calloc(holes.size(), sizeof(bntamb1_t));
    std::copy(holes.begin(), holes.end(), bns->ambs);

    PacVector pac_bwa = pac_forward;
    for (int64_t l = bns->l_pac - 1; l >= 0; --l)
        pac_bwa.push_back(3 - pac_bwa.get(l));

    return {
        .pac_bwa = pac_bwa,
        .pac_forward = pac_forward,
        .bns = bns,
    };
}

// modified from bwa (heng li)
bwt_t *seqlib_bwt_pac2bwt(const uint8_t *pac, int bwt_seq_lenr)
{

    bwt_t *bwt;
    ubyte_t *buf;
    int i;
    //FILE *fp;

    // initialization
    bwt = (bwt_t*)calloc(1, sizeof(bwt_t));
    bwt->seq_len = bwt_seq_lenr; //bwa_seq_len(fn_pac); //dummy
    bwt->bwt_size = (bwt->seq_len + 15) >> 4;
    //fp = xopen(fn_pac, "rb");

    // prepare sequence
    //pac_size = (bwt->seq_len>>2) + ((bwt->seq_len&3) == 0? 0 : 1);
    //buf2 = (ubyte_t*)calloc(pac_size, 1);
    //err_fread_noeof(buf2, 1, pac_size, fp);
    //err_fclose(fp);
    memset(bwt->L2, 0, 5 * 4);
    buf = (ubyte_t*)calloc(bwt->seq_len + 1, 1);
    for (i = 0; i < (int)bwt->seq_len; ++i) {
        buf[i] = pac[i>>2] >> ((3 - (i&3)) << 1) & 3;
        ++bwt->L2[1+buf[i]];
    }
    for (i = 2; i <= 4; ++i)
        bwt->L2[i] += bwt->L2[i-1];
    //free(buf2);

    // Burrows-Wheeler Transform
    bwt->primary = is_bwt(buf, bwt->seq_len);
    bwt->bwt = (u_int32_t*)calloc(bwt->bwt_size, 4);
    for (i = 0; i < (int)bwt->seq_len; ++i)
        bwt->bwt[i>>4] |= buf[i] << ((15 - (i&15)) << 1);
    free(buf);
    return bwt;
}

std::string extract_reference_subseq(bwaidx_t* index, int64_t ref_begin, int64_t ref_end) {
    std::string subseq(ref_end - ref_begin, '?');
    for (int64_t i = 0; i < subseq.size(); ++i)
        subseq[i] = nuclcode_to_char(pac_raw_get(index->pac, ref_begin + i));
    // TODO: Use binary search to look for relevant holes.
    for (bntamb1_t* hole = index->bns->ambs; hole != index->bns->ambs + index->bns->n_holes; ++hole) {
        int64_t left_intersect = std::max(hole->offset, ref_begin);
        int64_t right_intersect = std::min(hole->offset + hole->len, ref_end);
        for (int64_t i = left_intersect; i < right_intersect; ++i)
            subseq[i - ref_begin] = 'N';
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

BwaIndex::BwaIndex(const std::vector<BwaSequence>& refs): index(nullptr), options(mem_opt_init()), is_empty(refs.empty()) {
    if (is_empty)
        return;
    for (auto&& ref : refs)
        assert(!(ref.id.empty() || ref.seq.empty()));

    CompressedReference ref_compressed = compress_reference(refs);

    bwt_t* bwt = seqlib_bwt_pac2bwt(ref_compressed.pac_bwa.data(), ref_compressed.pac_bwa.size());
    bwt_bwtupdate_core(bwt);
    bwt_cal_sa(bwt, 32);
    bwt_gen_cnt_table(bwt);

    index = (bwaidx_t*) calloc(1, sizeof(bwaidx_t));
    index->bwt = bwt;
    index->bns = ref_compressed.bns;
    index->pac = (uint8_t*) malloc(ref_compressed.pac_forward.size());
    std::copy_n(ref_compressed.pac_forward.data(), ref_compressed.pac_forward.byte_size(), index->pac);
}

BwaIndex::~BwaIndex() {
    bwa_idx_destroy(index);
    free(options);
}

std::vector<BwaMatch> BwaIndex::align_sequence(std::string_view query) const {
    if (is_empty)
        return {};
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
            .ref_id = std::string_view(index->bns->anns[align->rid].name),
            .ref_subseq = extract_reference_subseq(index, align->rb, align->re),
            .ref_match_begin = align->rb - ref_offset,
            .ref_match_end = align->re - ref_offset,
            .ref_match_len = align->re - align->rb,
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
