/**
 * @file vulkan_minimizers.cppm
 * @brief Vulkan minimizer sketching and  target-index construction.
 *
 * Exports input preparation, GPU dispatch, and result owners for resident and
 * lazy sequences. Supports batched queries and overlapping target segments.
 * Unsupported input or unavailable resources produce a rejection or exception.
 * These entry points do not switch to CPU sketching.
 */
module;

#include <libassert/assert.hpp>

#include <cstring>
#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.sequences.kmers.vulkan_minimizers;

import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.hashers.base;
import bioseq.common.checked_arithmetic;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.instrumentation;

/** Device record layouts, rejection details, and alphabet checks. */
export import :contract;
/** Packed upload buffers and overlapping sequence intervals. */
export import :upload;
/** Host and device result owners with their memory reservations. */
export import :memory;
/** GPU sketching and result download for one  input window. */
export import :sketch_dispatch;
/** GPU target-index construction from prepared input. */
export import :index_materialization;
/** Single-sequence and batched query sketching. */
export import :batch;
/** GPU target indexes for a whole sequence or a packed interval. */
export import :index_routes;
/** Collectors for overlapping GPU sequence segments. */
export import :segmented;
/** Lazy-sequence sketching and index-building entry points. */
export import :entry_points;
