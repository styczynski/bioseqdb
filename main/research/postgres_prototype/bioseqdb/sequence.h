#pragma once

#include <cstdint>
#include <string_view> 

extern "C" {
#include <bwa/bwt.h>
#include <bwa/bwamem.h>
#include <postgres.h>
#include <catalog/pg_type.h>
}

static_assert(sizeof(bntamb1_t) == 16, "This should not happen");
static_assert(alignof(bntamb1_t) == 8, "This should not happen");

constexpr std::string_view allowed_nucleotides = "ACGTNWSMKRYBDHV";

struct NucleotideSequence {
    uint32_t occurences(char symbol) const;
    size_t length() const { return len; }

    const bntamb1_t* holes() const { return reinterpret_cast<const bntamb1_t*>(data); }
    const ubyte_t* pac() const { return reinterpret_cast<const ubyte_t*>(data + holes_num * sizeof(bntamb1_t)); }

    bntamb1_t* holes() { return reinterpret_cast<bntamb1_t*>(data); }
    ubyte_t* pac() { return reinterpret_cast<ubyte_t*>(data + holes_num * sizeof(bntamb1_t)); }

    NucleotideSequence* complement() const;
    NucleotideSequence* reverse() const;
    char* to_text_palloc() const;

    static int compare(const NucleotideSequence& lhs, const NucleotideSequence& rhs);

    char vl_len[4];
    uint32_t holes_num;
    uint32_t len;
    ubyte_t data[];
};

NucleotideSequence* nuclseq_from_text(std::string_view str);

bool operator==(const NucleotideSequence& left, const NucleotideSequence& right);
bool operator!=(const NucleotideSequence& left, const NucleotideSequence& right);
bool operator<(const NucleotideSequence& left, const NucleotideSequence& right);
bool operator<=(const NucleotideSequence& left, const NucleotideSequence& right);
bool operator>(const NucleotideSequence& left, const NucleotideSequence& right);
bool operator>=(const NucleotideSequence& left, const NucleotideSequence& right);

static inline size_t pac_byte_size(size_t x) { return x / 4 + (x % 4 != 0 ? 1 : 0); }

static inline int32_t nuclcode_from_char(char chr) {
    return nst_nt4_table[static_cast<unsigned char>(chr)];
}

static inline uint8_t pac_raw_get(const ubyte_t* pac, size_t index) {
    return pac[index >> 2] >> ((~index & 3) << 1) & 3;
}

static inline void pac_raw_set(ubyte_t* pac, size_t index, uint8_t value) {
    pac[index >> 2] |= value << ((~index & 3) << 1);
}
