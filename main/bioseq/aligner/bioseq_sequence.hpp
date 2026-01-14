// SPDX-FileCopyrightText: 2006-2025 Knut Reinert & Freie Universität Berlin
// SPDX-FileCopyrightText: 2016-2025 Knut Reinert & MPI für molekulare Genetik
// SPDX-License-Identifier: BSD-3-Clause

/*!\file
 * \brief Provides seqan3::pg_sequence.
 * \author Hannes Hauswedell <hannes.hauswedell AT fu-berlin.de>
 */

#pragma once

#include <concepts>
#include <iterator>
#include <ranges>
#include <type_traits>

#include <seqan3/alphabet/all.hpp>
#include <seqan3/alphabet/detail/alphabet_proxy.hpp>
#include <seqan3/alphabet/views/to_char.hpp>
#include <seqan3/alphabet/views/to_rank.hpp>
#include <seqan3/contrib/sdsl-lite.hpp>
#include <seqan3/core/concept/cereal.hpp>
#include <seqan3/core/range/detail/random_access_iterator.hpp>
#include <seqan3/utility/math.hpp>
#include <seqan3/utility/views/convert.hpp>
