/**
 * @file highway_minimizer_contract.cppm
 * @brief Shared input and output types for scheduled minimizer kernels.
 *
 * Defines packed inputs, record formats, collectors and result status.
 * The scheduler owns the buffers passed to CPU-specific generated kernels.
 * These interfaces can be used without Highway vector types.
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

/** Packed minimizer formats and the values they can store. */
export import :representation;
/** Collectors and the supported output formats. */
export import :collectors;
/** Algorithm choices, continuation state and work counters. */
export import :execution;
/** Borrowed sequence views and callbacks for invoking a selected kernel. */
export import :kernel;
