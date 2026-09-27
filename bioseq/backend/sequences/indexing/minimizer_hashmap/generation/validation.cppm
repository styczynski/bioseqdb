/**
 * @file minimizer_hashmap/generation/validation.cppm
 * @brief Check a generation's row order and checksums before validating its active head.
 */
export module bioseq.indexing.minimizer_hashmap:generation_validation;

import std;
import :exact_simd;
import :grouped_soa;
import :prepared_reader;
import :compact_direct;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.common.checked_arithmetic;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.chaining.core;
import bioseq.common.radix_sort;
import bioseq.instrumentation;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;
import :generation_contracts;
import :generation_stream;
import :generation_publication;

export namespace bioseq::indexing {

/// Check a complete  generation as stored rows are reread in order
/// Verify row addresses, sizes and CRCs, extent CRCs, and the final checksum
/// of payload-row checksums. Payload values are not decoded by this validator.
class minimizer_generation_row_validator {
  private:
    /// Constructor tag available after metadata validation succeeds.
    struct validated_tag {};

    encoded_grouped_soa_generation generation_{};
    std::size_t entry_index_ = 0;
    std::uint32_t payload_chunk_ = 0;
    std::uint32_t directory_chunk_ = 1;
    crc64_ecma182_accumulator extent_crc_{};
    crc64_ecma182_accumulator payload_row_checksums_{};
    bool position_payload_stage_ = false;
    bool manifest_seen_ = false;
    bool ready_validated_ = false;

    /// Copy generation metadata after create() checks it.
    explicit minimizer_generation_row_validator(validated_tag,
                                                const encoded_grouped_soa_generation& generation)
        : generation_(generation) {}

    /// Match one fixed metadata row against its address, bytes, and checksum.
    [[nodiscard]] static std::expected<void, minimizer_generation_encoding_error>
    validate_fixed_row(const encoded_minimizer_fixed_row& row,
                       const index_chunk_address& expected_address,
                       std::span<const std::byte> expected_bytes) noexcept {
        if (row.address != expected_address || row.byte_count != expected_bytes.size() ||
            row.byte_count > row.storage.size() ||
            !std::ranges::equal(row.bytes(), expected_bytes)) {
            return std::unexpected(minimizer_generation_encoding_error::row_order);
        }
        const std::uint64_t expected_checksum = crc64_index_chunk_row(row.address.generation_id,
                                                                      row.address.position_segment,
                                                                      row.address.sub_key,
                                                                      row.address.chunk_no,
                                                                      row.byte_count,
                                                                      row.bytes());
        if (row.row_checksum != expected_checksum) {
            return std::unexpected(minimizer_generation_encoding_error::checksum);
        }
        return {};
    }

  public:
    /// Check generation metadata, encoded head and manifest rows, and chunk counts
    /// before accepting the stored row stream.
    [[nodiscard]] static std::expected<minimizer_generation_row_validator,
                                       minimizer_generation_encoding_error>
    create(const encoded_grouped_soa_generation& generation) {
        if (auto metadata = validate_minimizer_generation_metadata(
                generation.head, generation.manifest, generation.directory);
            !metadata) {
            return std::unexpected(metadata.error());
        }
        const auto manifest_bytes =
            encode_minimizer_generation_manifest_record(generation.manifest);
        const auto head_bytes = encode_minimizer_generation_head(generation.head);
        if (!manifest_bytes || !head_bytes) {
            return std::unexpected(!manifest_bytes ? manifest_bytes.error() : head_bytes.error());
        }
        if (auto fixed = validate_fixed_row(generation.manifest_row,
                                            index_chunk_address{
                                                .generation_id = generation.manifest.generation_id,
                                                .position_segment = -1,
                                                .sub_key = k_directory_sub_key,
                                                .chunk_no = 0,
                                            },
                                            *manifest_bytes);
            !fixed) {
            return std::unexpected(fixed.error());
        }
        if (auto fixed = validate_fixed_row(generation.unpublished_head_row,
                                            index_chunk_address{
                                                .generation_id = 0,
                                                .position_segment = -1,
                                                .sub_key = k_generation_head_sub_key,
                                                .chunk_no = 0,
                                            },
                                            *head_bytes);
            !fixed) {
            return std::unexpected(fixed.error());
        }
        for (const grouped_soa_bucket_directory_entry& entry : generation.directory.entries()) {
            if (entry.control_chunk_count >
                (std::numeric_limits<std::uint32_t>::max)() - entry.position_chunk_count) {
                return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
            }
        }
        return minimizer_generation_row_validator{validated_tag{}, generation};
    }

    /// Check the next payload, directory, or manifest row and advance the reader.
    /// Reject extra rows after the manifest. A failed validation may  already
    /// have changed counters, so discard the validator after an error.
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    accept(const minimizer_generation_row_view& row) noexcept {
        if (ready_validated_ || manifest_seen_) {
            return std::unexpected(minimizer_generation_encoding_error::row_count);
        }
        const std::uint32_t extent_payload_bytes =
            minimizer_extent_payload_bytes(generation_.manifest.layout);
        if (extent_payload_bytes == 0) {
            return std::unexpected(minimizer_generation_encoding_error::unsupported_storage_layout);
        }
        if (entry_index_ < generation_.directory.entries().size()) {
            const grouped_soa_bucket_directory_entry& entry =
                generation_.directory.entries()[entry_index_];
            if (entry.control_chunk_count >
                (std::numeric_limits<std::uint32_t>::max)() - entry.position_chunk_count) {
                return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
            }
            const bool grouped_soa = generation_.manifest.layout ==
                                     minimizer_generation_layout::grouped_soa_segment_local;
            const bool control =
                grouped_soa ? !position_payload_stage_ : payload_chunk_ < entry.control_chunk_count;
            const std::uint32_t extent_chunks =
                control ? entry.control_chunk_count : entry.position_chunk_count;
            const std::uint32_t total_chunks =
                grouped_soa ? extent_chunks
                            : entry.control_chunk_count + entry.position_chunk_count;
            if (payload_chunk_ >= total_chunks) {
                return std::unexpected(minimizer_generation_encoding_error::row_count);
            }
            const auto position_sub_key = derive_minimizer_position_extent_sub_key(entry.sub_key);
            if (!position_sub_key) {
                return std::unexpected(position_sub_key.error());
            }
            const std::uint32_t extent_chunk = control ? payload_chunk_
                                               : grouped_soa
                                                   ? payload_chunk_
                                                   : payload_chunk_ - entry.control_chunk_count;
            const std::uint64_t extent_bytes =
                control ? entry.control_encoded_bytes : entry.position_encoded_bytes;
            if (extent_chunk > (std::numeric_limits<std::uint64_t>::max)() / extent_payload_bytes) {
                return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
            }
            const std::uint64_t offset =
                static_cast<std::uint64_t>(extent_chunk) * extent_payload_bytes;
            if (offset >= extent_bytes) {
                return std::unexpected(minimizer_generation_encoding_error::byte_count);
            }
            const std::uint64_t expected_bytes =
                std::min<std::uint64_t>(extent_payload_bytes, extent_bytes - offset);
            if (row.address.generation_id != generation_.manifest.generation_id ||
                row.address.position_segment != entry.position_segment ||
                row.address.sub_key != (control ? entry.sub_key : *position_sub_key) ||
                row.address.chunk_no != extent_chunk || row.bytes.size() != expected_bytes) {
                return std::unexpected(minimizer_generation_encoding_error::row_order);
            }
            const std::uint64_t expected_checksum =
                crc64_index_chunk_row(row.address.generation_id,
                                      row.address.position_segment,
                                      row.address.sub_key,
                                      row.address.chunk_no,
                                      static_cast<std::uint32_t>(row.bytes.size()),
                                      row.bytes);
            if (row.row_checksum != expected_checksum) {
                return std::unexpected(minimizer_generation_encoding_error::checksum);
            }
            extent_crc_.update(row.bytes);
            payload_row_checksums_.update_little_endian(row.row_checksum);
            ++payload_chunk_;
            const bool extent_complete =
                grouped_soa
                    ? payload_chunk_ == extent_chunks
                    : payload_chunk_ == (control ? entry.control_chunk_count : total_chunks);
            if (extent_complete && control) {
                if (extent_crc_.checksum() != entry.control_checksum) {
                    return std::unexpected(minimizer_generation_encoding_error::checksum);
                }
                extent_crc_ = {};
            }
            if (extent_complete && !control) {
                if (extent_crc_.checksum() != entry.position_checksum) {
                    return std::unexpected(minimizer_generation_encoding_error::checksum);
                }
                extent_crc_ = {};
                payload_chunk_ = 0;
                ++entry_index_;
            }
            if (grouped_soa && extent_complete && control) {
                payload_chunk_ = 0;
                ++entry_index_;
                if (entry_index_ == generation_.directory.entries().size()) {
                    entry_index_ = 0;
                    position_payload_stage_ = true;
                }
            }
            return {};
        }
        if (directory_chunk_ <= generation_.manifest.directory_chunk_count) {
            const std::uint64_t chunk_index = static_cast<std::uint64_t>(directory_chunk_ - 1u);
            if (chunk_index > (std::numeric_limits<std::uint64_t>::max)() / extent_payload_bytes) {
                return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
            }
            const std::uint64_t offset64 = chunk_index * extent_payload_bytes;
            if (offset64 >= generation_.directory.encoded_bytes().size() ||
                offset64 > (std::numeric_limits<std::size_t>::max)()) {
                return std::unexpected(minimizer_generation_encoding_error::directory_size);
            }
            const std::size_t offset = static_cast<std::size_t>(offset64);
            const std::size_t count = std::min<std::size_t>(
                extent_payload_bytes, generation_.directory.encoded_bytes().size() - offset);
            const auto expected_bytes =
                generation_.directory.encoded_bytes().subspan(offset, count);
            if (row.address !=
                    index_chunk_address{
                        .generation_id = generation_.manifest.generation_id,
                        .position_segment = -1,
                        .sub_key = k_directory_sub_key,
                        .chunk_no = directory_chunk_,
                    } ||
                !std::ranges::equal(row.bytes, expected_bytes)) {
                return std::unexpected(minimizer_generation_encoding_error::row_order);
            }
            const std::uint64_t checksum =
                crc64_index_chunk_row(row.address.generation_id,
                                      row.address.position_segment,
                                      row.address.sub_key,
                                      row.address.chunk_no,
                                      static_cast<std::uint32_t>(row.bytes.size()),
                                      row.bytes);
            if (row.row_checksum != checksum) {
                return std::unexpected(minimizer_generation_encoding_error::checksum);
            }
            ++directory_chunk_;
            return {};
        }
        if (!manifest_seen_) {
            if (row.address != generation_.manifest_row.address ||
                row.row_checksum != generation_.manifest_row.row_checksum ||
                !std::ranges::equal(row.bytes, generation_.manifest_row.bytes())) {
                return std::unexpected(minimizer_generation_encoding_error::row_order);
            }
            manifest_seen_ = true;
            return {};
        }
        return std::unexpected(minimizer_generation_encoding_error::row_count);
    }

    /// Require every expected row and compare the ordered payload checksum.
    /// Repeated calls succeed once validation is complet.
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error> finish_ready() noexcept {
        if (ready_validated_) {
            return {};
        }
        if (entry_index_ != generation_.directory.entries().size() || payload_chunk_ != 0 ||
            (generation_.manifest.layout ==
                 minimizer_generation_layout::grouped_soa_segment_local &&
             !generation_.directory.entries().empty() && !position_payload_stage_) ||
            directory_chunk_ != generation_.manifest.directory_chunk_count + 1u ||
            !manifest_seen_) {
            return std::unexpected(minimizer_generation_encoding_error::row_count);
        }
        if (payload_row_checksums_.checksum() != generation_.manifest.payload_checksum) {
            return std::unexpected(minimizer_generation_encoding_error::checksum);
        }
        ready_validated_ = true;
        return {};
    }

    /// Compare the active-head row with the expected head after finish_ready()
    /// has accepted the complete generation.
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    validate_publication_head(const minimizer_generation_row_view& row) const noexcept {
        if (!ready_validated_) {
            return std::unexpected(minimizer_generation_encoding_error::row_count);
        }
        const encoded_minimizer_fixed_row& expected = generation_.unpublished_head_row;
        if (row.address != expected.address || row.row_checksum != expected.row_checksum ||
            !std::ranges::equal(row.bytes, expected.bytes())) {
            return std::unexpected(minimizer_generation_encoding_error::row_order);
        }
        return {};
    }
};

} // namespace bioseq::indexing
