module;

#include "common.h"

#include <vector>
#include <type_traits>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <typeinfo>
#include <charconv>
#include <utility> // __cpp_lib_to_chars may be defined here as currently documented.
#include <version> // From C++20 onwards, all feature macros should be defined here.
#include <cassert>
#include <concepts>
#include <sstream>
#include <ranges>
#include <bit>
#include <cassert>
#include <cmath>
#include <stdexcept>
#include <seqan3/contrib/sdsl-lite.hpp>


export module bioseq.common.sequan_compat;

import bioseq.common.mlogging;

export {
#include <seqan3/alphabet/adaptation/all.hpp>
#include <seqan3/alphabet/alphabet_base.hpp>
#include <seqan3/alphabet/nucleotide/all.hpp>
#include <seqan3/alphabet/aminoacid/all.hpp>

#include <seqan3/utility/math.hpp>

//include <seqan3/core/debug_stream.hpp>
#include <seqan3/search/views/minimiser_hash.hpp>

//include <seqan3/alphabet/cigar/all.hpp>
}
#include <seqan3/alphabet/concept.hpp>



template <typename alphabet_type>
    requires (
        seqan3::alphabet<std::remove_cvref_t<alphabet_type>> &&
        !std::integral<std::remove_cvref_t<alphabet_type>>
    )
struct fmt::formatter<alphabet_type>: formatter<std::string> {
  // parse is inherited from formatter<string_view>.
  template <typename FormatContext>
  auto format(alphabet_type symbol, FormatContext& ctx) const {
    return formatter<std::string>::format(std::string(1, seqan3::to_char(symbol)), ctx);
  }
};


export namespace seq = seqan3;
