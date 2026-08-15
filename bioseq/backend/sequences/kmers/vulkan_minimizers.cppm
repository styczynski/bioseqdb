/**
 * @file vulkan_minimizers.cppm
 * @brief Vulkan minimizer backend for resident and lazy compact sequences.
 *
 * Owns minimizer wire traits, direct packed-sequence upload views, alphabet
 * rank tables, memory admission, host materialization, and reusable device
 * output. Unsupported sequence, alphabet, and coordinate shapes are reported
 * explicitly and never routed through a silent CPU fallback.
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

export import :contract;
export import :upload;
export import :memory;
export import :sketch_dispatch;
export import :index_materialization;
export import :batch;
export import :index_routes;
export import :segmented;
export import :entry_points;
