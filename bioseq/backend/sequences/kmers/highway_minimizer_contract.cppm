/**
 * @file highway_minimizer_contract.cppm
 * @brief Target-neutral ABI between the scheduler and generated minimizers.
 *
 * No Highway type is reachable from this module.  Generated fixed-target
 * modules consume these records, while neutral scheduler code owns their
 * storage and lifetime.
 */
module;

#include <algorithm>
#include <array>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.sequences.kmers.highway_minimizer_contract;

import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.storage.size;

export import :representation;
export import :collectors;
export import :execution;
export import :kernel;
