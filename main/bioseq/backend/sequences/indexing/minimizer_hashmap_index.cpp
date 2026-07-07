/**
 * @file minimizer_hashmap_index.cpp
 * @brief Boost-backed implementation for MinimizerHashmapFragmentIndex.
 */
module;

#include "common.h"

#include <boost/unordered/unordered_flat_map.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <utility>

module bioseq.indexing.minimizer_hashmap;

import bioseq.common.radix_sort;
import bioseq.indexing.type;

namespace bioseq::indexing::detail {

struct MinimizerHashmapFragmentIndexTable {
    boost::unordered_flat_map<std::uint64_t, std::uint64_t> table;
};

}  // namespace bioseq::indexing::detail

namespace bioseq::indexing {

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
