/**
 * @file core_read_path.cppm
 * @brief Trait-driven admitted grouped-SoA query read path.
 */
module;

#include <libassert/assert.hpp>

export module bioseq.indexing.minimizer_hashmap:prepared_reader_core_read_path;

import std;
import :exact_simd;
import :grouped_soa;
import :prepared_reader_directory;
import :prepared_reader_receipts;
import bioseq.chaining.core;
import bioseq.common.checked_arithmetic;
import bioseq.common.logging;
import bioseq.common.radix_sort;
import bioseq.common.seqan_adapter;
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.type;
import bioseq.instrumentation;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.mode;
import bioseq.parallelism.pipeline_ops;
import bioseq.parallelism.pool;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.storage.lazy_sequence;
import bioseq.storage.scanner;
import bioseq.storage.size;

export import :prepared_reader_core_read_path_declarations;
export import :prepared_reader_core_read_path_admission;
export import :prepared_reader_core_read_path_query_mapping;
export import :prepared_reader_core_read_path_record_decode;
export import :prepared_reader_core_read_path_read_execution;
