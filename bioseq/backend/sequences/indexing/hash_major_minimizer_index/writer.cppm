/**
 * @file hash_major_minimizer_index/writer.cppm
 * @brief Ordered hash-major stream emission and publication metadata assembly.
 */
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <utility>
#include <vector>

export module bioseq.indexing.hash_major_minimizer_index:writer;

import :codec;
import bioseq.indexing.storage;

export namespace bioseq::indexing {

/// Callback that appends bytes to one logical stream without reordering them.
using hash_major_minimizer_append_logical_bytes_fn = bool (*)(
    void*, hash_major_minimizer_stream_kind, std::span<const std::byte>);

/** @brief Non-owning append callback for the three logical output streams. */
struct hash_major_minimizer_logical_stream_sink_ref {
    void* context = nullptr;
    hash_major_minimizer_append_logical_bytes_fn append_bytes = nullptr;

    [[nodiscard]] bool append(
            hash_major_minimizer_stream_kind kind,
            std::span<const std::byte> bytes) const {
        return append_bytes != nullptr && append_bytes(context, kind, bytes);
    }
};

/** @brief Decoded and byte-exact encoded publication metadata. */
struct hash_major_minimizer_encoded_metadata {
    hash_major_minimizer_head head{};
    hash_major_minimizer_manifest manifest{};
    encoded_hash_major_minimizer_head encoded_head{};
    encoded_hash_major_minimizer_manifest encoded_manifest{};
};

/** @brief Streaming hash-major encoder that emits ordered authenticated streams. */
class hash_major_minimizer_logical_encoder {
private:
    hash_major_minimizer_generation_options options_{};
    hash_major_minimizer_logical_stream_sink_ref sink_{};
    std::array<crc64_ecma182_accumulator, 3> stream_crc_{};
    std::array<std::uint64_t, 3> stream_bytes_{};
    std::array<std::byte, k_hash_major_minimizer_row_payload_bytes> posting_buffer_{};
    std::size_t posting_buffer_bytes_ = 0u;

    std::vector<std::uint64_t> block_hashes_{};
    std::vector<std::uint64_t> block_counts_{};
    std::uint64_t block_posting_byte_offset_ = 0u;
    std::uint64_t block_first_posting_ordinal_ = 0u;
    crc64_ecma182_accumulator block_posting_crc_{};

    std::uint64_t unique_hash_count_ = 0u;
    std::uint64_t total_posting_count_ = 0u;
    std::uint64_t directory_entry_count_ = 0u;
    std::uint64_t previous_hash_ = 0u;
    bool have_previous_hash_ = false;

    std::uint64_t current_hash_ = 0u;
    std::uint64_t current_expected_count_ = 0u;
    std::uint64_t current_written_count_ = 0u;
    std::uint64_t current_previous_posting_ = 0u;
    bool current_have_posting_ = false;
    bool hash_open_ = false;
    bool finished_ = false;

    [[nodiscard]] static constexpr std::size_t stream_index_(
            hash_major_minimizer_stream_kind kind) noexcept {
        return static_cast<std::size_t>(kind) - 1u;
    }

    [[nodiscard]] std::expected<void, hash_major_minimizer_error> emit_(
            hash_major_minimizer_stream_kind kind,
            std::span<const std::byte> bytes) {
        const std::size_t index = stream_index_(kind);
        std::uint64_t next = 0u;
        if (bytes.empty()
            || !hash_major_minimizer_detail::checked_add(
                stream_bytes_[index], bytes.size(), next)) {
            return std::unexpected(
                bytes.empty() ? hash_major_minimizer_error::sink_write
                              : hash_major_minimizer_error::arithmetic_overflow);
        }
        if (!sink_.append(kind, bytes)) {
            return std::unexpected(hash_major_minimizer_error::sink_write);
        }
        stream_crc_[index].update(bytes);
        stream_bytes_[index] = next;
        return {};
    }

    [[nodiscard]] std::expected<void, hash_major_minimizer_error>
    flush_posting_buffer_() {
        if (posting_buffer_bytes_ == 0u) return {};
        if (!sink_.append(
                hash_major_minimizer_stream_kind::postings,
                std::span<const std::byte>{posting_buffer_}.first(
                    posting_buffer_bytes_))) {
            return std::unexpected(hash_major_minimizer_error::sink_write);
        }
        posting_buffer_bytes_ = 0u;
        return {};
    }

    [[nodiscard]] std::expected<void, hash_major_minimizer_error>
    append_posting_bytes_(std::span<const std::byte> bytes) {
        const std::size_t index = stream_index_(
            hash_major_minimizer_stream_kind::postings);
        std::uint64_t next = 0u;
        if (!hash_major_minimizer_detail::checked_add(
                stream_bytes_[index], bytes.size(), next)) {
            return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
        }
        if (posting_buffer_bytes_ + bytes.size() > posting_buffer_.size()) {
            if (auto flushed = flush_posting_buffer_(); !flushed) {
                return flushed;
            }
        }
        std::copy(
            bytes.begin(), bytes.end(),
            posting_buffer_.begin() + posting_buffer_bytes_);
        posting_buffer_bytes_ += bytes.size();
        stream_crc_[index].update(bytes);
        block_posting_crc_.update(bytes);
        stream_bytes_[index] = next;
        return {};
    }

    [[nodiscard]] std::expected<void, hash_major_minimizer_error>
    finish_block_() {
        using namespace hash_major_minimizer_detail;
        if (block_hashes_.empty()) return {};
        if (hash_open_) {
            return std::unexpected(hash_major_minimizer_error::state);
        }
        const std::uint8_t posting_width =
            options_.storage_profile.stored_posting_width_bytes;
        auto control = encode_hash_major_minimizer_control_block(
            block_hashes_,
            block_counts_,
            block_posting_byte_offset_,
            block_posting_crc_.checksum(),
            options_.storage_profile.hash_width_bytes,
            posting_width);
        if (!control) return std::unexpected(control.error());
        const std::uint64_t control_offset = stream_bytes_[stream_index_(
            hash_major_minimizer_stream_kind::controls)];
        if (control->size() > (std::numeric_limits<std::uint32_t>::max)()) {
            return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
        }
        const std::uint64_t block_posting_bytes =
            stream_bytes_[stream_index_(hash_major_minimizer_stream_kind::postings)]
            - block_posting_byte_offset_;
        std::uint64_t block_posting_count = 0u;
        for (const std::uint64_t count : block_counts_) {
            if (!checked_add(
                    block_posting_count, count, block_posting_count)) {
                return std::unexpected(
                    hash_major_minimizer_error::arithmetic_overflow);
            }
        }
        std::uint64_t expected_posting_bytes = 0u;
        if (!checked_multiply(
                block_posting_count,
                posting_width,
                expected_posting_bytes)
            || expected_posting_bytes != block_posting_bytes) {
            return std::unexpected(hash_major_minimizer_error::count);
        }
        // Publication order is postings first, then the control/directory
        // authority that makes those bytes discoverable.  In particular, a
        // partial final posting row must not remain buffered while metadata
        // for this block becomes externally visible.
        if (auto flushed = flush_posting_buffer_(); !flushed) {
            return flushed;
        }
        hash_major_minimizer_directory_entry entry{
            .first_hash = block_hashes_.front(),
            .last_hash = block_hashes_.back(),
            .control_block_ordinal = directory_entry_count_,
            .control_byte_offset = control_offset,
            .control_byte_length = static_cast<std::uint32_t>(control->size()),
            .control_count = static_cast<std::uint32_t>(block_hashes_.size()),
            .first_posting_ordinal = block_first_posting_ordinal_,
            .posting_byte_offset = block_posting_byte_offset_,
            .posting_byte_length = block_posting_bytes,
            .exact_posting_count = block_posting_count,
        };
        const std::uint64_t control_crc = read_le<std::uint64_t>(*control, 48u);
        entry.block_crc64 = hash_major_minimizer_block_crc64(
            entry, control_crc, block_posting_crc_.checksum());
        auto encoded_directory =
            encode_hash_major_minimizer_directory_entry(entry);
        if (!encoded_directory) {
            return std::unexpected(encoded_directory.error());
        }
        if (auto emitted = emit_(
                hash_major_minimizer_stream_kind::controls, *control);
            !emitted) {
            return emitted;
        }
        if (auto emitted = emit_(
                hash_major_minimizer_stream_kind::directory, *encoded_directory);
            !emitted) {
            return emitted;
        }
        if (directory_entry_count_
            == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
        }
        ++directory_entry_count_;
        block_hashes_.clear();
        block_counts_.clear();
        block_posting_byte_offset_ = stream_bytes_[stream_index_(
            hash_major_minimizer_stream_kind::postings)];
        block_first_posting_ordinal_ = total_posting_count_;
        block_posting_crc_ = {};
        return {};
    }

public:
    hash_major_minimizer_logical_encoder(
            hash_major_minimizer_generation_options options,
            hash_major_minimizer_logical_stream_sink_ref sink)
        : options_(options), sink_(sink) {
        block_hashes_.reserve(k_hash_major_minimizer_controls_per_block);
        block_counts_.reserve(k_hash_major_minimizer_controls_per_block);
    }

    hash_major_minimizer_logical_encoder(
        const hash_major_minimizer_logical_encoder&) = delete;
    hash_major_minimizer_logical_encoder& operator=(
        const hash_major_minimizer_logical_encoder&) = delete;

    [[nodiscard]] std::expected<void, hash_major_minimizer_error> begin_hash(
            std::uint64_t hash,
            std::uint64_t exact_count) {
        if (finished_ || hash_open_ || exact_count == 0u
            || !is_valid_hash_major_minimizer_storage_profile(
                options_.storage_profile, options_.target_length)
            || hash > hash_major_minimizer_max_hash(options_.storage_profile)
            || (have_previous_hash_ && hash <= previous_hash_)) {
            return std::unexpected(
                exact_count == 0u ? hash_major_minimizer_error::count
                                  : hash_major_minimizer_error::state);
        }
        if (block_hashes_.size() == k_hash_major_minimizer_controls_per_block) {
            if (auto finished = finish_block_(); !finished) return finished;
        }
        if (block_hashes_.empty()) {
            block_posting_byte_offset_ = stream_bytes_[stream_index_(
                hash_major_minimizer_stream_kind::postings)];
            block_first_posting_ordinal_ = total_posting_count_;
        }
        block_hashes_.push_back(hash);
        block_counts_.push_back(exact_count);
        current_hash_ = hash;
        current_expected_count_ = exact_count;
        current_written_count_ = 0u;
        current_previous_posting_ = 0u;
        current_have_posting_ = false;
        hash_open_ = true;
        return {};
    }

    [[nodiscard]] std::expected<void, hash_major_minimizer_error> append_posting(
            std::uint64_t encoded_position) {
        if (!hash_open_ || current_written_count_ >= current_expected_count_) {
            return std::unexpected(hash_major_minimizer_error::state);
        }
        const std::uint64_t global_bp = encoded_position >> 1u;
        if (global_bp >= options_.target_length
            || (current_have_posting_
                && encoded_position <= current_previous_posting_)
            || (options_.storage_profile.stored_posting_width_bytes == 4u
                && encoded_position
                    > (std::numeric_limits<std::uint32_t>::max)())) {
            return std::unexpected(
                global_bp >= options_.target_length
                    ? hash_major_minimizer_error::target_range
                    : hash_major_minimizer_error::order);
        }
        std::array<std::byte, 8> encoded{};
        if (options_.storage_profile.stored_posting_width_bytes == 4u) {
            hash_major_minimizer_detail::write_le<std::uint32_t>(
                encoded, 0u, static_cast<std::uint32_t>(encoded_position));
        } else {
            hash_major_minimizer_detail::write_le(encoded, 0u, encoded_position);
        }
        if (auto appended = append_posting_bytes_(std::span<const std::byte>{
                encoded.data(),
                options_.storage_profile.stored_posting_width_bytes});
            !appended) {
            return appended;
        }
        current_previous_posting_ = encoded_position;
        current_have_posting_ = true;
        ++current_written_count_;
        return {};
    }

    [[nodiscard]] std::expected<void, hash_major_minimizer_error> end_hash() {
        if (!hash_open_ || current_written_count_ != current_expected_count_) {
            return std::unexpected(hash_major_minimizer_error::count);
        }
        std::uint64_t next_total = 0u;
        if (!hash_major_minimizer_detail::checked_add(
                total_posting_count_,
                current_expected_count_,
                next_total)) {
            return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
        }
        total_posting_count_ = next_total;
        if (unique_hash_count_
            == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(hash_major_minimizer_error::arithmetic_overflow);
        }
        ++unique_hash_count_;
        previous_hash_ = current_hash_;
        have_previous_hash_ = true;
        hash_open_ = false;
        return {};
    }

    [[nodiscard]] std::expected<void, hash_major_minimizer_error> append_hash(
            std::uint64_t hash,
            std::span<const std::uint64_t> positions) {
        if (auto begun = begin_hash(hash, positions.size()); !begun) {
            return begun;
        }
        for (const std::uint64_t position : positions) {
            if (auto appended = append_posting(position); !appended) {
                return appended;
            }
        }
        return end_hash();
    }

    [[nodiscard]] std::expected<
        hash_major_minimizer_encoded_metadata, hash_major_minimizer_error>
    finish() {
        using namespace hash_major_minimizer_detail;
        if (finished_ || hash_open_ || options_.generation_id == 0u
            || !is_valid_hash_major_minimizer_storage_profile(
                options_.storage_profile, options_.target_length)) {
            return std::unexpected(hash_major_minimizer_error::state);
        }
        if (auto block = finish_block_(); !block) {
            return std::unexpected(block.error());
        }
        if (auto flushed = flush_posting_buffer_(); !flushed) {
            return std::unexpected(flushed.error());
        }
        const auto identity = hash_major_minimizer_source_identity(
            options_.query_key,
            options_.source_epoch,
            options_.source_content_crc64,
            options_.target_length);
        const auto storage_profile_digest =
            hash_major_minimizer_storage_profile_digest(options_.storage_profile);
        hash_major_minimizer_manifest manifest{
            .generation_id = options_.generation_id,
            .source_epoch = options_.source_epoch,
            .source_identity = identity,
            .source_content_crc64 = options_.source_content_crc64,
            .target_length = options_.target_length,
            .unique_hash_count = unique_hash_count_,
            .total_posting_count = total_posting_count_,
            .storage_profile = options_.storage_profile,
            .storage_profile_digest = storage_profile_digest,
            .authenticated_directory_crc64 =
                stream_crc_[stream_index_(
                    hash_major_minimizer_stream_kind::directory)].checksum(),
            .total_control_bytes = stream_bytes_[stream_index_(
                hash_major_minimizer_stream_kind::controls)],
            .total_posting_bytes = stream_bytes_[stream_index_(
                hash_major_minimizer_stream_kind::postings)],
            .directory_entry_count = directory_entry_count_,
        };
        const std::uint64_t directory_rows = ceil_div(
            stream_bytes_[0], k_hash_major_minimizer_row_payload_bytes);
        const std::uint64_t control_rows = ceil_div(
            stream_bytes_[1], k_hash_major_minimizer_row_payload_bytes);
        const std::uint64_t posting_rows = ceil_div(
            stream_bytes_[2], k_hash_major_minimizer_row_payload_bytes);
        std::uint64_t posting_first_row = 0u;
        std::uint64_t total_rows = 0u;
        if (!checked_add(
                directory_rows, control_rows, posting_first_row)
            || !checked_add(posting_first_row, posting_rows, total_rows)) {
            return std::unexpected(
                hash_major_minimizer_error::arithmetic_overflow);
        }
        manifest.streams = {
            hash_major_minimizer_stream_descriptor{
                .kind = hash_major_minimizer_stream_kind::directory,
                .encoding_revision = 1u,
                .element_width = static_cast<std::uint8_t>(
                    k_hash_major_minimizer_directory_entry_bytes),
                .element_count = directory_entry_count_,
                .logical_bytes = stream_bytes_[0],
                .row_count = directory_rows,
                .stream_crc64 = stream_crc_[0].checksum(),
                .first_row_ordinal = 0u,
            },
            hash_major_minimizer_stream_descriptor{
                .kind = hash_major_minimizer_stream_kind::controls,
                .encoding_revision = 1u,
                .element_width = 0u,
                .element_count = unique_hash_count_,
                .logical_bytes = stream_bytes_[1],
                .row_count = control_rows,
                .stream_crc64 = stream_crc_[1].checksum(),
                .first_row_ordinal = directory_rows,
            },
            hash_major_minimizer_stream_descriptor{
                .kind = hash_major_minimizer_stream_kind::postings,
                .encoding_revision = 1u,
                .element_width = options_.storage_profile
                    .stored_posting_width_bytes,
                .element_count = total_posting_count_,
                .logical_bytes = stream_bytes_[2],
                .row_count = posting_rows,
                .stream_crc64 = stream_crc_[2].checksum(),
                .first_row_ordinal = posting_first_row,
            },
        };
        auto encoded_manifest = encode_hash_major_minimizer_manifest(manifest);
        if (!encoded_manifest) {
            return std::unexpected(encoded_manifest.error());
        }
        auto decoded_manifest = decode_hash_major_minimizer_manifest(
            *encoded_manifest);
        if (!decoded_manifest) {
            return std::unexpected(decoded_manifest.error());
        }
        manifest = *decoded_manifest;
        hash_major_minimizer_head head{
            .generation_id = options_.generation_id,
            .query_key = options_.query_key,
            .source_epoch = options_.source_epoch,
            .source_identity = identity,
            .source_content_crc64 = options_.source_content_crc64,
            .storage_profile_digest = storage_profile_digest,
            .manifest_address_bound_row_crc64 = crc64_index_chunk_row(
                options_.generation_id,
                0,
                0,
                0u,
                static_cast<std::uint32_t>(encoded_manifest->size()),
                *encoded_manifest),
            .manifest_payload_crc64 = crc64_ecma182(*encoded_manifest),
        };
        auto encoded_head = encode_hash_major_minimizer_head(head);
        if (!encoded_head) return std::unexpected(encoded_head.error());
        auto decoded_head = decode_hash_major_minimizer_head(*encoded_head);
        if (!decoded_head) return std::unexpected(decoded_head.error());
        finished_ = true;
        return hash_major_minimizer_encoded_metadata{
            .head = *decoded_head,
            .manifest = manifest,
            .encoded_head = *encoded_head,
            .encoded_manifest = *encoded_manifest,
        };
    }
};

} // namespace bioseq::indexing
