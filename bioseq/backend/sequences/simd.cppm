/**
 * @file simd.cppm
 * @brief Streaming and one-shot SIMD codecs for packed biological sequences.
 *
 * The interface selects Highway target implementations while retaining scalar
 * fallbacks for unsupported alphabet and storage-width combinations.
 *
 * @details Aggregates vector streaming, streaming codecs, kernel tables,
 * sequence adapters, and standard-layout helpers through one import surface.
 */
module;

#include <hwy/highway.h>
#include <libassert/assert.hpp>

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <functional>

#include <boost/integer.hpp>

export module bioseq.sequences.simd;

import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.rank_pack_core;
import bioseq.storage.lazy_sequence;

export import bioseq.simd.vector_stream;

export import :streaming;
export import :codec_kernels;
export import :sequence_adapters;
export import :standard_layout;
