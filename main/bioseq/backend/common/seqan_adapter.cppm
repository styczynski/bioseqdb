/**
 * @file
 * @brief SeqAn alphabet adapter and formatting integration.
 */
module;

#include <concepts>
#include <string>
#include <type_traits>

#include <seqan3/contrib/sdsl-lite.hpp>

export module bioseq.common.seqan_adapter;

import bioseq.common.logging;

export {
#include <seqan3/alphabet/adaptation/all.hpp>
#include <seqan3/alphabet/alphabet_base.hpp>
#include <seqan3/alphabet/aminoacid/all.hpp>
#include <seqan3/alphabet/nucleotide/all.hpp>
#include <seqan3/search/views/minimiser_hash.hpp>
#include <seqan3/utility/math.hpp>
}

#include <seqan3/alphabet/concept.hpp>

/**
 * @brief Formats a non-integral SeqAn alphabet symbol as its character representation.
 *
 * @tparam p_alphabet SeqAn alphabet type accepted by `seqan3::to_char`.
 */
template <typename p_alphabet>
    requires seqan3::alphabet<std::remove_cvref_t<p_alphabet>> &&
             (!std::integral<std::remove_cvref_t<p_alphabet>>)
struct fmt::formatter<p_alphabet> : formatter<std::string> {
    /**
     * @brief Writes one alphabet symbol through the string formatter.
     *
     * @tparam p_format_context fmt output-context type.
     * @param[in] symbol Alphabet symbol to render.
     * @param[in,out] context Destination formatting context.
     * @return Iterator immediately following the rendered character.
     */
    template <typename p_format_context>
    auto format(p_alphabet symbol, p_format_context& context) const {
        return formatter<std::string>::format(
            std::string(1u, seqan3::to_char(symbol)),
            context);
    }
};

/// @brief Concise namespace alias for the SeqAn API exported by this adapter.
export namespace seq = seqan3;
