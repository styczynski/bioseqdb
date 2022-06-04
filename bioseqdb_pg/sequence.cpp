#include <algorithm>
#include <cstdint>
#include <random>

#include "sequence.h"

inline namespace {

char complement_symbol(char c) {
    switch (c) {
        case 'A': return 'T';
        case 'C': return 'G';
        case 'G': return 'C';
        case 'T': return 'A';
        case 'N': return 'N';
        case 'W': return 'W';
        case 'S': return 'S';
        case 'M': return 'K';
        case 'K': return 'M';
        case 'R': return 'Y';
        case 'Y': return 'R';
        case 'B': return 'V';
        case 'D': return 'H';
        case 'H': return 'D';
        case 'V': return 'B';
    }

    return 'N';
}

template<typename F>
void for_each_block(const NucleotideSequence& nucls, F f) {
    uint32_t p = 0;
    for(uint32_t i = 0 ; i < nucls.holes_num ; i++) {
        const auto& hole = nucls.holes()[i];

        if(hole.offset > p)
            f(p, hole.offset);
        p = static_cast<uint32_t>(hole.offset + hole.len);
    }

    if (p < nucls.len)
        f(p, nucls.len);
}

uint32_t calculate_num_of_holes(std::string_view str) {
    uint32_t count = 0;
    char prev_chr = 0;

    for(auto chr : str) {
        if (prev_chr != chr && nuclcode_from_char(chr) >= 4)
            count += 1;
        prev_chr = chr;
    }

    return count;
}

NucleotideSequence* alloc_raw_nucls(uint32_t holes_num, uint32_t len) {
    // Postgresql requires logicaly same values to have same bits, so we use zero alloc to fill paddings of bntamb1_t.
    const auto size = 4 * sizeof(uint32_t) + holes_num * sizeof(bntamb1_t) + pac_byte_size(len);
    const auto ptr = static_cast<NucleotideSequence*>(palloc0(size));

    SET_VARSIZE(ptr, size);
    ptr->holes_num = holes_num;
    ptr->len = len;
    ptr->padded_len = pac_byte_size(len) * 4;

    return ptr;
}

void inplace_to_text(const NucleotideSequence& nucls, char* text) {
    const ubyte_t* pac = nucls.pac();

    for(uint32_t i = 0 ; i < nucls.len; i++)
        text[i] = "ACGT"[pac_raw_get(pac, i)];

    for(const bntamb1_t* hole = nucls.holes() ; hole < nucls.holes() + nucls.holes_num ; hole++)
        std::fill(text + hole->offset, text + hole->offset + hole->len, hole->amb);

    text[nucls.len] = '\0';
}

}

uint32_t NucleotideSequence::occurences(char chr) const {
    auto pac = this->pac();
    auto holes = this->holes();

    const ubyte_t code = nuclcode_from_char(chr);
    uint32_t count = 0;

    if (code >= 4) {
        for(uint32_t i = 0 ; i < holes_num ; i++) {
            if (holes[i].amb == chr)
                count += holes[i].len;
        }
    } else {
        for_each_block(*this, [&](uint32_t p, uint32_t q) {
            for(uint32_t i = p; i < q ; i++) {
                if(pac_raw_get(pac, i) == code)
                    count++;
            }
        });
    }

    return count;
};

NucleotideSequence* NucleotideSequence::complement() const {
    auto com_nucls = alloc_raw_nucls(holes_num, len);
    auto com_pac = com_nucls->pac();
    auto com_holes = com_nucls->holes();
    auto pac = this->pac();

    std::copy_n(holes(), holes_num, com_holes);
    for(uint32_t i = 0 ; i < holes_num ; i++) {
        const auto& hole = holes()[i];
        com_holes[i].amb = complement_symbol(hole.amb);
        for(uint32_t j = hole.offset ; j < hole.offset + hole.len ; j++)
            pac_raw_set(com_pac, j, pac_raw_get(pac, j));
    }

    for_each_block(*this, [&](uint32_t p, uint32_t q) {
        for(uint32_t i = p; i < q ; i++) {
            pac_raw_set(com_pac, i, 0b11 - pac_raw_get(pac, i));
        }
    });

    return com_nucls;
};

NucleotideSequence* NucleotideSequence::reverse() const {
    auto rev_nucls = alloc_raw_nucls(holes_num, len);
    auto rev_pac = rev_nucls->pac();
    auto rev_holes = rev_nucls->holes();
    auto pac = this->pac();
    auto holes = this->holes();
    std::minstd_rand rng(holes_num ^ len);

    for_each_block(*this, [&](uint32_t p, uint32_t q) {
        for(uint32_t i = p; i < q ; i++) {
            pac_raw_set(rev_pac, len - i - 1, pac_raw_get(pac, i));
        }
    });

    for(uint32_t i = 0  ; i < holes_num ; i++) {
        const auto& hole = holes[i];
        auto& rev_hole = rev_holes[holes_num - i - 1];
        rev_hole = hole;
        rev_hole.offset = len - hole.offset - 1;

        for(uint32_t j = rev_hole.offset ; j < rev_hole.offset + rev_hole.len ; j++)
            pac_raw_set(rev_pac, j, rng() & 0b11);
    }

    for(uint32_t i = len; i < padded_len ; i++)
        pac_raw_set(rev_pac, i, rng() & 0b11);

    return rev_nucls;
}

char* NucleotideSequence::to_text_palloc() const {
    auto text = reinterpret_cast<char*>(palloc(len + 1));
    inplace_to_text(*this, text);
    return text;
}

char* NucleotideSequence::to_text_malloc() const {
    auto text = reinterpret_cast<char*>(malloc(len + 1));
    inplace_to_text(*this, text);
    return text;
}

NucleotideSequence* nuclseq_from_text(std::string_view str) {
    uint32_t holes_num = calculate_num_of_holes(str);
    NucleotideSequence* nucls = alloc_raw_nucls(holes_num, str.size());
    auto pac = nucls->pac();

    // libbwa requires random values inside holes, but again we want them to be deterministic => lcg
    std::minstd_rand rng(holes_num ^ str.size());
    bntamb1_t* hole = nucls->holes();
    char prev_chr = 0;

    for(uint32_t idx = 0 ; idx < str.size() ; idx++) {
        char chr = str[idx];
        ubyte_t code = nuclcode_from_char(chr);

        if (code >= 4) {
            if (prev_chr == chr) {
                hole->len++;
            } else {
                hole++;
                hole->amb = chr;
                hole->offset = idx;
                hole->len = 1;
            }
            pac_raw_set(pac, idx, rng() & 0b11);
        }
        else {
            pac_raw_set(pac, idx, code);
        }

        prev_chr = chr;
    }

    for(uint32_t i = nucls->len ; i < nucls->padded_len ; i++)
        pac_raw_set(pac, i, rng() & 0b11);

    return nucls;
}
