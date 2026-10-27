/**
 * @file seqan_adapter.cppm
 * @brief Exports SeqAn alphabet helpers and formats symbols as characters.
 */
module;

// clang-format off
#include <bit>
#include <cassert>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstdlib>
#include <functional>
#include <memory>
#include <ranges>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>
#include <version>

#include <seqan3/contrib/sdsl-lite.hpp>
// clang-format on

export module bioseq.common.seqan_adapter;

import bioseq.common.logging;

export {
#include <seqan3/alphabet/adaptation/all.hpp>
#include <seqan3/alphabet/alphabet_base.hpp>
#include <seqan3/alphabet/aminoacid/all.hpp>
// Intentional pinned-dependency exception: SeqAn recommends cigar::operation,
// which aliases exactly the operation type below in this vendored revision.
// Importing that public alias requires the counted product: Clang 22/libc++26
// rejects its broad pod_tuple std::tuple_size specialization on incomplete
// std::tuple types and its exported internal-linkage tuple helper. Keep the
// exception here; this is not an API-stability claim for exposition_only.
// Bioseq owns counted RLE runs and needs only the dependency's operation layer.
#include <seqan3/alphabet/cigar/exposition_only/cigar_operation.hpp>
#include <seqan3/alphabet/nucleotide/all.hpp>
#include <seqan3/search/views/minimiser_hash.hpp>
#include <seqan3/utility/math.hpp>
}

#include <seqan3/alphabet/concept.hpp>

/**
 * @brief Prints a SeqAn alphabet symbol as a character.
 *
 * Integral types keep theif normal fmt formatting.
 * @tparam p_alphabet SeqAn alphabet type accepted by seqan3::to_char.
 */
template <typename p_alphabet>
    requires seqan3::alphabet<std::remove_cvref_t<p_alphabet>> &&
             (!std::integral<std::remove_cvref_t<p_alphabet>>)
struct fmt::formatter<p_alphabet> : formatter<std::string> {
    /**
     * @brief Converts one symbol to a character and applies fmt string formatting.
     * @return Iterator after the formatted character.
     */
    template <typename p_format_context>
    auto format(p_alphabet symbol, p_format_context& context) const {
        return formatter<std::string>::format(std::string(1u, seqan3::to_char(symbol)), context);
    }
};

/// @brief Short name for the exported SeqAn namespace.
export namespace seq = seqan3;
