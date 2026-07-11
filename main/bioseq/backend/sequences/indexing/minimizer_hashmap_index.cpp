/**
 * @file minimizer_hashmap_index.cpp
 * @brief Boost-backed implementation for MinimizerHashmapFragmentIndex.
 */
module;

#include "common.h"

#include <hwy/highway.h>

#include <boost/unordered/unordered_flat_map.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <limits>
#include <span>
#include <utility>

module bioseq.indexing.minimizer_hashmap;

import bioseq.common.radix_sort;
import bioseq.indexing.type;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.two_stacks;

namespace bioseq::indexing::detail {

struct MinimizerHashmapFragmentIndexTable {
    boost::unordered_flat_map<std::uint64_t, std::uint64_t> table;
};

}  // namespace bioseq::indexing::detail

namespace bioseq::indexing {

namespace {

using ExactStatus = bioseq::simd::HWY_NAMESPACE::TwoStackContinuationStatus;
using ExactProof = bioseq::simd::HWY_NAMESPACE::TwoStackExecutionProof;

[[nodiscard]] constexpr ExactSimdIndexStatus bridge_status(
        ExactStatus status) noexcept {
    switch (status) {
        case ExactStatus::ok: return ExactSimdIndexStatus::ok;
        case ExactStatus::invalid_window_size:
            return ExactSimdIndexStatus::invalid_window_size;
        case ExactStatus::invalid_kmer_size:
            return ExactSimdIndexStatus::invalid_kmer_size;
        case ExactStatus::invalid_static_configuration:
            return ExactSimdIndexStatus::invalid_static_configuration;
        case ExactStatus::unsupported_alphabet:
            return ExactSimdIndexStatus::unsupported_alphabet;
        case ExactStatus::unsupported_storage:
            return ExactSimdIndexStatus::unsupported_storage;
        case ExactStatus::unsupported_representation:
            return ExactSimdIndexStatus::unsupported_representation;
        case ExactStatus::unsupported_vector_shape:
            return ExactSimdIndexStatus::unsupported_vector_shape;
        case ExactStatus::not_initialized:
            return ExactSimdIndexStatus::not_initialized;
        case ExactStatus::already_finalized:
            return ExactSimdIndexStatus::already_finalized;
        case ExactStatus::invalid_state:
            return ExactSimdIndexStatus::invalid_state;
        case ExactStatus::invalid_range:
            return ExactSimdIndexStatus::invalid_range;
        case ExactStatus::non_contiguous_range:
            return ExactSimdIndexStatus::non_contiguous_range;
        case ExactStatus::arithmetic_overflow:
            return ExactSimdIndexStatus::arithmetic_overflow;
        case ExactStatus::collector_overflow:
            return ExactSimdIndexStatus::collector_overflow;
    }
    return ExactSimdIndexStatus::invalid_state;
}

[[nodiscard]] constexpr ExactSimdIndexExecutionProof bridge_proof(
        const ExactProof& proof) noexcept {
    return ExactSimdIndexExecutionProof{
        .bp_consumed = proof.bp_consumed,
        .minimizers_emitted = proof.minimizers_emitted,
        .vector_batches = proof.vector_batches,
        .masked_tail_lanes = proof.masked_tail_lanes,
        .packed_decode_batches = proof.packed_decode_batches,
        .packed_decode_lanes = proof.packed_decode_lanes,
        .ambiguous_symbols = proof.ambiguous_symbols,
        .ambiguous_reset_batches = proof.ambiguous_reset_batches,
        .scalar_backend_branches = proof.scalar_backend_branches,
        .scalar_tail_symbols = proof.scalar_tail_symbols,
        .scalar_hash_symbols = proof.scalar_hash_symbols,
        .scalar_window_transitions = proof.scalar_window_transitions,
        .scalar_event_records = proof.scalar_event_records,
        .scalar_sort_records = proof.scalar_sort_records,
        .scalar_dedup_records = proof.scalar_dedup_records,
        .fail_closed_rejections = proof.fail_closed_rejections,
        .saturated = proof.saturated,
    };
}

[[nodiscard]] constexpr ExactSimdIndexBridgeResult rejected_bridge_result(
        ExactSimdIndexStatus status) noexcept {
    return {
        .status = status,
        .proof = ExactSimdIndexExecutionProof{.fail_closed_rejections = 1},
    };
}

struct ErasedExactCollector {
    using minimizer_type = bioseq::simd::HWY_NAMESPACE::MinimapSketchData;

    const ExactSimdIndexBridgeRequest* request = nullptr;

    [[nodiscard]] bool append_raw_storage(
            const std::uint64_t* raw,
            std::size_t count) {
        return request->append_raw(
            request->collector_context, raw, count);
    }
};

template<class Alphabet, class StorageUnit>
[[nodiscard]] ExactSimdIndexBridgeResult collect_exact_width(
        const ExactSimdIndexBridgeRequest& request) {
    using sequence_config = sequence_custom_alphabet<Alphabet, StorageUnit>;
    using range_type = compact_sequence_range<sequence_config>;
    using traits = typename range_type::traits;
    using State = bioseq::simd::HWY_NAMESPACE::
        TwoStackContinuationState<0, 0, traits>;
    constexpr std::size_t lpu = traits::letters_per_unit;

    if (request.read_fragment == nullptr
        || request.release_fragment == nullptr
        || request.append_raw == nullptr
        || request.sequence_context == nullptr
        || request.collector_context == nullptr) {
        return rejected_bridge_result(ExactSimdIndexStatus::invalid_state);
    }
    if (request.storage_unit_bytes != sizeof(StorageUnit)
        || request.letters_per_unit != lpu) {
        return rejected_bridge_result(ExactSimdIndexStatus::unsupported_storage);
    }
    if (request.fragment_units == 0
        || request.lo_unit > request.hi_unit
        || request.hi_unit > request.total_units
        || request.lo_unit
            > (std::numeric_limits<std::size_t>::max)() / lpu
        || request.hi_unit
            > (std::numeric_limits<std::size_t>::max)() / lpu
        || request.total_units
            > (std::numeric_limits<std::size_t>::max)() / lpu
        || request.total_bp > request.total_units * lpu) {
        return rejected_bridge_result(ExactSimdIndexStatus::invalid_range);
    }

    const std::size_t consume_begin_bp = request.lo_unit * lpu;
    const std::size_t consume_end_bp = std::min(
        request.hi_unit * lpu, request.total_bp);
    State state;
    const ExactStatus reset =
        bioseq::simd::HWY_NAMESPACE::reset_two_stack_nthash(
            state,
            request.window_size,
            request.kmer_size,
            request.emitted_position_base);
    if (reset != ExactStatus::ok) {
        return {
            .status = bridge_status(reset),
            .proof = bridge_proof(state.proof),
        };
    }

    const std::size_t first_fragment =
        request.lo_unit / request.fragment_units;
    const std::size_t fragment_end = request.hi_unit == 0
        ? 0
        : 1 + (request.hi_unit - 1) / request.fragment_units;
    if (fragment_end > request.fragment_count
        || request.fragment_units
            > (std::numeric_limits<std::size_t>::max)() / lpu) {
        return rejected_bridge_result(ExactSimdIndexStatus::invalid_range);
    }
    const std::size_t fragment_bp_capacity = request.fragment_units * lpu;
    ErasedExactCollector collector{.request = &request};
    ExactStatus consume_status = ExactStatus::ok;
    for (std::size_t fragment_no = first_fragment;
         fragment_no < fragment_end;
         ++fragment_no) {
        if (fragment_no
                > (std::numeric_limits<std::size_t>::max)()
                    / fragment_bp_capacity) {
            return rejected_bridge_result(
                ExactSimdIndexStatus::arithmetic_overflow);
        }
        const ExactSimdIndexPackedFragment fragment =
            request.read_fragment(request.sequence_context, fragment_no);
        if (fragment.storage_units != 0 && fragment.data == nullptr) {
            request.release_fragment(request.sequence_context, fragment_no);
            return rejected_bridge_result(ExactSimdIndexStatus::invalid_range);
        }
        const std::size_t fragment_begin_bp =
            fragment_no * fragment_bp_capacity;
        const std::size_t packed_bp_capacity = fragment.storage_units
                > (std::numeric_limits<std::size_t>::max)() / lpu
            ? (std::numeric_limits<std::size_t>::max)()
            : fragment.storage_units * lpu;
        const std::size_t fragment_bp_size = std::min(
            packed_bp_capacity,
            request.total_bp - std::min(request.total_bp, fragment_begin_bp));
        const std::size_t fragment_consume_begin = std::max(
            consume_begin_bp, fragment_begin_bp);
        const std::size_t fragment_consume_end = std::min(
            consume_end_bp, fragment_begin_bp + fragment_bp_size);
        try {
            consume_status = bioseq::simd::HWY_NAMESPACE::
                consume_two_stack_nthash_packed_fragment<
                    0, 0,
                    bioseq::simd::HWY_NAMESPACE::MinimapSketchData>(
                        state,
                        collector,
                        std::span<const StorageUnit>{
                            static_cast<const StorageUnit*>(fragment.data),
                            fragment.storage_units},
                        fragment_bp_size,
                        fragment_begin_bp,
                        fragment_consume_begin,
                        fragment_consume_end);
        } catch (...) {
            request.release_fragment(request.sequence_context, fragment_no);
            throw;
        }
        request.release_fragment(request.sequence_context, fragment_no);
        if (consume_status != ExactStatus::ok) break;
    }
    if (consume_status != ExactStatus::ok) {
        return {
            .status = bridge_status(consume_status),
            .proof = bridge_proof(state.proof),
        };
    }
    const auto finalized =
        bioseq::simd::HWY_NAMESPACE::finalize_two_stack_nthash(
            state, collector);
    return {
        .status = bridge_status(finalized.status),
        .proof = bridge_proof(finalized.proof),
    };
}

template<class Alphabet>
[[nodiscard]] ExactSimdIndexBridgeResult collect_exact_alphabet(
        const ExactSimdIndexBridgeRequest& request) {
    switch (request.storage_unit_bytes) {
        case 1:
            return collect_exact_width<Alphabet, std::uint8_t>(request);
        case 2:
            return collect_exact_width<Alphabet, std::uint16_t>(request);
        case 4:
            return collect_exact_width<Alphabet, std::uint32_t>(request);
        case 8:
            return collect_exact_width<Alphabet, std::uint64_t>(request);
        default:
            return rejected_bridge_result(
                ExactSimdIndexStatus::unsupported_storage);
    }
}

}  // namespace

ExactSimdIndexBridgeResult collect_exact_simd_index_erased(
        const ExactSimdIndexBridgeRequest& request) {
    switch (request.alphabet) {
        case ExactSimdIndexAlphabet::dna4:
            return collect_exact_alphabet<seq::dna4>(request);
        case ExactSimdIndexAlphabet::dna5:
            return collect_exact_alphabet<seq::dna5>(request);
    }
    return rejected_bridge_result(ExactSimdIndexStatus::unsupported_alphabet);
}

MinimizerHashmapFragmentIndex::MinimizerHashmapFragmentIndex()
    : table_(std::make_unique<detail::MinimizerHashmapFragmentIndexTable>())
{}

MinimizerHashmapFragmentIndex::~MinimizerHashmapFragmentIndex() = default;

MinimizerHashmapFragmentIndex::MinimizerHashmapFragmentIndex(
        const MinimizerHashmapFragmentIndex& other)
    : table_(std::make_unique<detail::MinimizerHashmapFragmentIndexTable>()),
      positions(other.positions),
      target_len(other.target_len)
{
    if (other.table_ != nullptr) {
        *table_ = *other.table_;
    }
}

MinimizerHashmapFragmentIndex& MinimizerHashmapFragmentIndex::operator=(
        const MinimizerHashmapFragmentIndex& other) {
    if (this == &other) return *this;
    if (table_ == nullptr) {
        table_ = std::make_unique<detail::MinimizerHashmapFragmentIndexTable>();
    }
    if (other.table_ != nullptr) {
        *table_ = *other.table_;
    } else {
        table_->table.clear();
    }
    positions = other.positions;
    target_len = other.target_len;
    return *this;
}

MinimizerHashmapFragmentIndex::MinimizerHashmapFragmentIndex(
        MinimizerHashmapFragmentIndex&& other) noexcept = default;

MinimizerHashmapFragmentIndex& MinimizerHashmapFragmentIndex::operator=(
        MinimizerHashmapFragmentIndex&& other) noexcept = default;

void MinimizerHashmapFragmentIndex::clear_table() {
    if (table_ == nullptr) {
        table_ = std::make_unique<detail::MinimizerHashmapFragmentIndexTable>();
    }
    table_->table.clear();
}

void MinimizerHashmapFragmentIndex::reserve_table(std::size_t bucket_count) {
    if (table_ == nullptr) {
        table_ = std::make_unique<detail::MinimizerHashmapFragmentIndexTable>();
    }
    table_->table.reserve(bucket_count);
}

void MinimizerHashmapFragmentIndex::emplace_table(
        std::uint64_t hash, std::uint64_t packed) {
    if (table_ == nullptr) {
        table_ = std::make_unique<detail::MinimizerHashmapFragmentIndexTable>();
    }
    table_->table.emplace(hash, packed);
}

std::size_t MinimizerHashmapFragmentIndex::table_size() const noexcept {
    return table_ == nullptr ? 0u : table_->table.size();
}

bool MinimizerHashmapFragmentIndex::table_empty() const noexcept {
    return table_size() == 0u;
}

void MinimizerHashmapFragmentIndex::for_each_table_entry_erased(
        void* ctx,
        void (*emit)(void* ctx, std::uint64_t hash, std::uint64_t packed)) const {
    if (table_ == nullptr) return;
    for (const auto& [hash, packed] : table_->table) {
        emit(ctx, hash, packed);
    }
}

void MinimizerHashmapFragmentIndex::build_from_pairs(
        std::span<std::pair<std::uint64_t, std::uint64_t>> pairs,
        sequence_size_t target_length) {
    target_len = target_length;
    clear_table();
    positions.clear();
    const std::size_t n = pairs.size();
    if (n == 0) return;
    if (n > UINT32_MAX) {
        index_corruption_panic(
            "build_from_pairs: >UINT32_MAX positions (pos_offset is u32)");
    }
    bioseq::sorting::radix_sort(
        pairs.data(), pairs.data() + n,
        [](const auto& p) noexcept { return p.first; });
    positions.resize(n);
    reserve_table(n / 2 + 16);
    std::size_t i = 0;
    while (i < n) {
        const std::uint64_t kmer = pairs[i].first;
        std::size_t j = i;
        while (j < n && pairs[j].first == kmer) {
            positions[j] = pairs[j].second;
            ++j;
        }
        if (j - i > UINT32_MAX) {
            index_corruption_panic(
                "build_from_pairs: kmer count > u32 (packed count field)");
        }
        emplace_table(
            kmer,
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(i)) << 32)
                | static_cast<std::uint32_t>(j - i));
        i = j;
    }
}

std::span<const std::uint64_t> MinimizerHashmapFragmentIndex::lookup(
        std::uint64_t kmer_hash) const {
    if (table_ == nullptr) return {};
    const auto iter = table_->table.find(kmer_hash);
    if (iter == table_->table.end()) {
        return {};
    }
    const std::uint64_t packed = iter->second;
    const std::uint32_t off = static_cast<std::uint32_t>(packed >> 32);
    const std::uint32_t count = static_cast<std::uint32_t>(packed);
    return std::span<const std::uint64_t>(positions.data() + off, count);
}

void MinimizerHashmapFragmentIndex::add_position_offset(sequence_size_t offset) {
    if (offset == 0) return;
    for (std::uint64_t& encoded : positions) {
        const std::uint64_t pos =
            (encoded >> 1) + static_cast<std::uint64_t>(offset);
        const std::uint64_t strand = encoded & 1ULL;
        encoded = (pos << 1) | strand;
    }
}

std::size_t MinimizerHashmapFragmentIndex::serialize_binary_size() const {
    return kSerializeHeaderBytes
         + kSerializeBucketBytes * table_size()
         + 8 * positions.size();
}

void MinimizerHashmapFragmentIndex::serialize_to_binary(char* output) const {
    const std::uint32_t bucket_count =
        static_cast<std::uint32_t>(table_size());
    const std::uint64_t total_positions =
        static_cast<std::uint64_t>(positions.size());
    const SerializedHeader header{
        kSerializeMagic, kSerializeVersion, target_len, bucket_count, 0u,
        total_positions,
    };
    std::memcpy(output, &header, sizeof(header));
    char* bucket_cursor = output + kSerializeHeaderBytes;
    char* position_cursor = bucket_cursor + kSerializeBucketBytes * bucket_count;
    for_each_table_entry(
        [&bucket_cursor](std::uint64_t hash, std::uint64_t packed) {
            const std::uint32_t pos_off =
                static_cast<std::uint32_t>(packed >> 32);
            const std::uint32_t pos_cnt =
                static_cast<std::uint32_t>(packed);
            const SerializedBucket bucket{hash, pos_off, pos_cnt};
            std::memcpy(bucket_cursor, &bucket, sizeof(bucket));
            bucket_cursor += kSerializeBucketBytes;
        });
    if (total_positions > 0) {
        std::memcpy(position_cursor, positions.data(), total_positions * 8);
    }
}

MinimizerHashmapFragmentIndex
MinimizerHashmapFragmentIndex::deserialize_from_binary(
        const char* input, std::size_t input_size) {
    if (input_size < kSerializeHeaderBytes) {
        index_corruption_panic(
            "MinimizerHashmapFragmentIndex deserialize: truncated blob");
    }
    SerializedHeader header{};
    std::memcpy(&header, input, sizeof(header));
    if (header.magic != kSerializeMagic) {
        index_corruption_panic(
            "MinimizerHashmapFragmentIndex deserialize: magic mismatch");
    }
    if (header.version != kSerializeVersion) {
        index_corruption_panic(
            "MinimizerHashmapFragmentIndex deserialize: version mismatch");
    }
    const std::size_t expected_size =
        kSerializeHeaderBytes
        + kSerializeBucketBytes * header.bucket_count
        + 8 * header.total_positions;
    if (input_size != expected_size) {
        index_corruption_panic(
            "MinimizerHashmapFragmentIndex deserialize: size mismatch");
    }
    MinimizerHashmapFragmentIndex idx;
    idx.target_len = header.target_len;
    idx.reserve_table(header.bucket_count);
    const char* bucket_cursor = input + kSerializeHeaderBytes;
    const char* position_cursor =
        bucket_cursor + kSerializeBucketBytes * header.bucket_count;
    idx.positions.resize(header.total_positions);
    if (header.total_positions > 0) {
        std::memcpy(
            idx.positions.data(), position_cursor, header.total_positions * 8);
    }
    for (std::uint32_t b = 0; b < header.bucket_count; ++b) {
        SerializedBucket bucket{};
        std::memcpy(&bucket, bucket_cursor, sizeof(bucket));
        bucket_cursor += kSerializeBucketBytes;
        if (static_cast<std::uint64_t>(bucket.pos_offset) + bucket.pos_count
            > header.total_positions) {
            index_corruption_panic(
                "MinimizerHashmapFragmentIndex deserialize: bucket range exceeds total_positions");
        }
        const std::uint64_t packed =
            (static_cast<std::uint64_t>(bucket.pos_offset) << 32)
            | static_cast<std::uint64_t>(bucket.pos_count);
        idx.emplace_table(bucket.hash, packed);
    }
    return idx;
}

}  // namespace bioseq::indexing
