/**
 * @file vulkan_minimizers.cppm
 * @brief Vulkan minimizer backend for resident and lazy compact sequences.
 *
 * @details Re-exports contract, upload, memory, sketch dispatch,
 * index materialization, batch, index-route, segmented, and entry-point
 * partitions. Owns minimizer wire traits, direct packed-sequence upload
 * views, alphabet rank tables, memory admission, host materialization, and
 * reusable device output. Unsupported sequence, alphabet, and coordinate shapes
 * are reported explicitly and never routed through a silent CPU fallback.
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

/** Minimizer wire traits, decline identities, and alphabet contracts. */
export import :contract;
/** Packed upload views, canonical windows, and segment geometry. */
export import :upload;
/** Host and device minimizer ownership and admission accounting. */
export import :memory;
/** Single-window Vulkan sketch dispatch and materialization. */
export import :sketch_dispatch;
/** Device target-index materialization from prepared uploads. */
export import :index_materialization;
/** Resident and batched Vulkan minimizer sketch routes. */
export import :batch;
/** Resident whole-target and segment index routes. */
export import :index_routes;
/** Strict segmented Vulkan minimizer collectors. */
export import :segmented;
/** Lazy-sequence Vulkan minimizer facade entry points. */
export import :entry_points;
