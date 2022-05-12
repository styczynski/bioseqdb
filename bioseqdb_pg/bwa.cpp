#include "bwa.h"

#include <cstring>
#include <stdexcept>

#include <htslib/htslib/sam.h>
extern "C" {
// Internal libbwa symbol, not exported through any of the headers.
int is_bwt(ubyte_t *T, int n);
}

#define _set_pac(pac, l, c) ((pac)[(l)>>2] |= (c)<<((~(l)&3)<<1))
#define _get_pac(pac, l) ((pac)[(l)>>2]>>((~(l)&3)<<1)&3)

inline namespace {

std::string cigar_compressed_to_string(const uint32_t *raw, int len) {
    std::string cigar;
    for (int i = 0; i < len; ++i) {
        cigar += std::to_string(bam_cigar_oplen(raw[i]));
        cigar += bam_cigar_opchr(raw[i]);
    }
    return cigar;
}

std::string_view slice_match(std::string_view nucleotides, const uint32_t *cigar_raw, int cigar_len) {
    size_t start = 0;
    size_t len = 0;
    // Skip first N block, then use bam_cigar_type to check whether further blocks use seq from the query to
    // calculate the length of the match in the queried sequence.
    for (int i = 0; i < cigar_len; ++i) {
        if (i == 0 && bam_cigar_op(cigar_raw[i]) == BAM_CREF_SKIP)
            start = bam_cigar_oplen(cigar_raw[i]);
        else if (bam_cigar_type(bam_cigar_op(cigar_raw[i])) & 1)
            len += bam_cigar_oplen(cigar_raw[i]);
    }
    return nucleotides.substr(start, len);
}

}

BwaIndex::BwaIndex() {
    idx = nullptr;
    memopt = mem_opt_init();
    memopt->flag |= MEM_F_SOFTCLIP;
}

BwaIndex::~BwaIndex() {
    if (idx)
        bwa_idx_destroy(idx);
    if (memopt)
        free(memopt);
}

void BwaIndex::build_index(const std::vector<BwaSequence>& ref_seqs) {
    assert(ref_seqs.size() > 0);
    for (auto&& ref_seq : ref_seqs)
        assert(!(ref_seq.id.empty() || ref_seq.seq.empty()));
    assert(idx == nullptr);

    // allocate memory for idx
    idx = (bwaidx_t*)calloc(1, sizeof(bwaidx_t));;

    // construct the forward-only pac
    uint8_t* fwd_pac = seqlib_make_pac(ref_seqs, true); //true->for_only

    // construct the forward-reverse pac ("packed" 2 bit sequence)
    uint8_t* pac = seqlib_make_pac(ref_seqs, false); // don't write, becasue only used to make BWT

    size_t tlen = 0;
    for (auto&& ref_seq : ref_seqs)
        tlen += ref_seq.seq.length();

#ifdef DEBUG_BWATOOLS
    std::cerr << "ref seq length: " << tlen << std::endl;
#endif

    // make the bwt
    bwt_t *bwt;
    bwt = seqlib_bwt_pac2bwt(pac, tlen*2); // *2 for fwd and rev
    bwt_bwtupdate_core(bwt);
    free(pac); // done with fwd-rev pac

    // construct sa from bwt and occ. adds it to bwt struct
    bwt_cal_sa(bwt, 32);
    bwt_gen_cnt_table(bwt);

    // make the bns
    bntseq_t * bns = (bntseq_t*) calloc(1, sizeof(bntseq_t));
    bns->l_pac = tlen;
    bns->n_seqs = ref_seqs.size();
    bns->seed = 11;
    bns->n_holes = 0;

    // make the anns
    bns->anns = (bntann1_t*)calloc(ref_seqs.size(), sizeof(bntann1_t));
    size_t offset = 0;
    for (size_t k = 0; k < ref_seqs.size(); ++k) {
        seqlib_add_to_anns(ref_seqs[k].id, ref_seqs[k].seq, &bns->anns[k], offset);
        offset += ref_seqs[k].seq.length();
    }

    //ambs is "holes", like N bases
    bns->ambs = 0; //(bntamb1_t*)calloc(1, sizeof(bntamb1_t));

    // make the in-memory idx struct
    idx->bwt = bwt;
    idx->bns = bns;
    idx->pac = fwd_pac;

}

std::vector<BwaMatch> BwaIndex::align_sequence(std::string_view read_nucleotides) const {
    assert(idx != nullptr);

    mem_alnreg_v ar = mem_align1(memopt, idx->bwt, idx->bns, idx->pac, read_nucleotides.length(), read_nucleotides.data()); // get all the hits (was c_str())

    // TOOD: Free memory.

    std::vector<BwaMatch> matches;
    for (size_t i = 0; i < ar.n; ++i) {
        mem_aln_t a = mem_reg2aln(memopt, idx->bns, idx->pac, read_nucleotides.length(), read_nucleotides.data(), &ar.a[i]);
        matches.push_back({
            .ref_id = std::string(idx->bns->anns[ar.a[i].rid].name),
            .ref_match_begin = ar.a[i].rb,
            .ref_match_end = ar.a[i].re,
            .query_subseq = std::string(slice_match(read_nucleotides, a.cigar, a.n_cigar)),
            .query_match_begin = ar.a[i].qb,
            .query_match_end = ar.a[i].qe,
            .is_primary = (a.flag & BAM_FSECONDARY) == 0,
            .is_secondary = (a.flag & BAM_FSECONDARY) != 0,
            .is_reverse = a.is_rev != 0,
            .cigar = cigar_compressed_to_string(a.cigar, a.n_cigar),
            .score = a.score,
        });
    }
    return matches;

//        // if alignment is reverse, set it
//        if (a[i].is_rev)
//            b.b->core.flag |= BAM_FREVERSE;
//
//        // allocate the cigar. Reverse if aligned to neg strand, since mem_aln_t stores
//        // cigars relative to referemce string oreiatnion, not forward alignment
//        memcpy(b.b->data + b.b->core.l_qname, (uint8_t*)a[i].cigar, a[i].n_cigar<<2);
//
//        // convert N to S or H
//        int new_val = hardclip ? BAM_CHARD_CLIP : BAM_CSOFT_CLIP;
//        uint32_t * cigr = bam_get_cigar(b.b);
//        for (int k = 0; k < b.b->core.n_cigar; ++k) {
//            if ( (cigr[k] & BAM_CIGAR_MASK) == BAM_CREF_SKIP) {
//                cigr[k] &= ~BAM_CIGAR_MASK;
//                cigr[k] |= new_val;
//            }
//        }
//
//        int slen = new_seq.length();
//        int j = 0;
//        if (a[i].is_rev) {
//            for (int i = slen-1; i >= 0; --i) {
//
//                // bad idea but works for now
//                // this is REV COMP things
//                uint8_t base = 15;
//                if (new_seq.at(i) == 'T')
//                    base = 1;
//                else if (new_seq.at(i) == 'G')
//                    base = 2;
//                else if (new_seq.at(i) == 'C')
//                    base = 4;
//                else if (new_seq.at(i) == 'A')
//                    base = 8;
//
//                m_bases[j >> 1] &= ~(0xF << ((~j & 1) << 2));   ///< zero out previous 4-bit base encoding
//                m_bases[j >> 1] |= base << ((~j & 1) << 2);  ///< insert new 4-bit base encoding
//                ++j;
//            }
//        } else {
//            for (int i = 0; i < slen; ++i) {
//                // bad idea but works for now
//                uint8_t base = 15;
//                if (new_seq.at(i) == 'A')
//                    base = 1;
//                else if (new_seq.at(i) == 'C')
//                    base = 2;
//                else if (new_seq.at(i) == 'G')
//                    base = 4;
//                else if (new_seq.at(i) == 'T')
//                    base = 8;
//
//                m_bases[i >> 1] &= ~(0xF << ((~i & 1) << 2));   ///< zero out previous 4-bit base encoding
//                m_bases[i >> 1] |= base << ((~i & 1) << 2);  ///< insert new 4-bit base encoding
//
//            }
//        }
//
}

// modified from bwa (heng li)
uint8_t* BwaIndex::seqlib_add1(const kseq_t *seq, bntseq_t *bns, uint8_t *pac, int64_t *m_pac, int *m_seqs, int *m_holes, bntamb1_t **q)
{
    bntann1_t *p;
    int lasts;
    if (bns->n_seqs == *m_seqs) {
        *m_seqs <<= 1;
        bns->anns = (bntann1_t*)realloc(bns->anns, *m_seqs * sizeof(bntann1_t));
    }
    p = bns->anns + bns->n_seqs;
    p->name = strdup((char*)seq->name.s);
    p->anno = seq->comment.l > 0? strdup((char*)seq->comment.s) : strdup("(null)");
    p->gi = 0; p->len = seq->seq.l;
    p->offset = (bns->n_seqs == 0)? 0 : (p-1)->offset + (p-1)->len;
    p->n_ambs = 0;
    for (size_t i = lasts = 0; i < seq->seq.l; ++i) {
        int c = nst_nt4_table[(int)seq->seq.s[i]];
        if (c >= 4) { // N
            if (lasts == seq->seq.s[i]) { // contiguous N
                ++(*q)->len;
            } else {
                if (bns->n_holes == *m_holes) {
                    (*m_holes) <<= 1;
                    bns->ambs = (bntamb1_t*)realloc(bns->ambs, (*m_holes) * sizeof(bntamb1_t));
                }
                *q = bns->ambs + bns->n_holes;
                (*q)->len = 1;
                (*q)->offset = p->offset + i;
                (*q)->amb = seq->seq.s[i];
                ++p->n_ambs;
                ++bns->n_holes;
            }
        }
        lasts = seq->seq.s[i];
        { // fill buffer
            if (c >= 4) c = lrand48()&3;
            if (bns->l_pac == *m_pac) { // double the pac size
                *m_pac <<= 1;
                pac = (uint8_t*)realloc(pac, *m_pac/4);
                memset(pac + bns->l_pac/4, 0, (*m_pac - bns->l_pac)/4);
            }
            _set_pac(pac, bns->l_pac, c);
            ++bns->l_pac;
        }
    }
    ++bns->n_seqs;

    return pac;
}

// modified from bwa (heng li)
uint8_t* BwaIndex::seqlib_make_pac(const std::vector<BwaSequence>& v, bool for_only)
{

    bntseq_t * bns = (bntseq_t*)calloc(1, sizeof(bntseq_t));
    uint8_t *pac = 0;
    int32_t m_seqs, m_holes;
    int64_t m_pac, l;
    bntamb1_t *q;

    bns->seed = 11; // fixed seed for random generator
    m_seqs = m_holes = 8; m_pac = 0x10000;
    bns->anns = (bntann1_t*)calloc(m_seqs, sizeof(bntann1_t));
    bns->ambs = (bntamb1_t*)calloc(m_holes, sizeof(bntamb1_t));
    pac = (uint8_t*) calloc(m_pac/4, 1);
    q = bns->ambs;

    // move through the unaligned sequences
    for (size_t k = 0; k < v.size(); ++k) {

        // make the ref name kstring
        kstring_t * name = (kstring_t*)malloc(1 * sizeof(kstring_t));
        name->l = v[k].id.length() + 1;
        name->m = v[k].id.length() + 3;
        name->s = (char*)calloc(name->m, sizeof(char));
        memcpy(name->s, v[k].id.data(), v[k].id.length());
        name->s[v[k].id.length()] = '\0';

        // make the sequence kstring
        kstring_t * t = (kstring_t*)malloc(sizeof(kstring_t));
        t->l = v[k].seq.length();
        t->m = v[k].seq.length() + 2;
        //t->s = (char*)calloc(v[k].Seq.length(), sizeof(char));
        t->s = (char*)malloc(t->m);
        memcpy(t->s, v[k].seq.data(), v[k].seq.length());

        // put into a kstring
        kseq_t *ks = (kseq_t*)calloc(1, sizeof(kseq_t));
        ks->seq = *t;
        ks->name = *name;

        // make the forward only pac
        pac = seqlib_add1(ks, bns, pac, &m_pac, &m_seqs, &m_holes, &q);

        // clear it out
        free(name->s);
        free(name);
        free(t->s);
        free(t);
        //free(ks->name.s);
        //free(ks->seq.s);
        //free(ks->f->buf);
        //free(
        free(ks);
        // NOTE free kstring_t?
        //kseq_destroy(s);
    }

    if (!for_only)
    {
        // add the reverse complemented sequence
        m_pac = (bns->l_pac * 2 + 3) / 4 * 4;
        pac = (uint8_t*)realloc(pac, m_pac/4);
        memset(pac + (bns->l_pac+3)/4, 0, (m_pac - (bns->l_pac+3)/4*4) / 4);
        for (l = bns->l_pac - 1; l >= 0; --l, ++bns->l_pac)
            _set_pac(pac, bns->l_pac, 3-_get_pac(pac, l));
    }

    bns_destroy(bns);

    return pac;
}

// modified from bwa (heng li)
bwt_t *BwaIndex::seqlib_bwt_pac2bwt(const uint8_t *pac, int bwt_seq_lenr)
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

// modified from bwa (heng li)
bntann1_t* BwaIndex::seqlib_add_to_anns(std::string_view name, std::string_view seq, bntann1_t* ann, size_t offset)
{

    ann->offset = offset;
    ann->name = (char*)malloc(name.length()+1); // +1 for \0
    strncpy(ann->name, name.data(), name.length());
    ann->name[name.length()] = '\0';
    ann->anno = (char*)malloc(7);
    strcpy(ann->anno, "(null)\0");
    ann->len = seq.length();
    ann->n_ambs = 0; // number of "holes"
    ann->gi = 0; // gi?
    ann->is_alt = 0;

    return ann;
}
