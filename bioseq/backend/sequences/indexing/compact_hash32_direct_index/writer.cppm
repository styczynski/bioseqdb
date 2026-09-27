/**
 * @file writer.cppm
 * @brief Build direct minimizer indexes from sorted runs or unsorted occurrences.
 */
export module bioseq.indexing.compact_hash32_direct_index:writer;

import std;
import :wire_format;

namespace bioseq::indexing::compact_hash32_detail {

/**
 * @brief Add an integer's little-endian bytes to the source checksum.
 * @tparam p_unsigned_integer Unsigned integer type determining the byte count.
 * @param[in,out] crc Checksum state to update.
 * @param[in] value Integer to include.
 */
template <class p_unsigned_integer>
    requires std::is_unsigned_v<p_unsigned_integer>
void crc_update_le(crc64_state& crc, p_unsigned_integer value) noexcept {
    crc.update_little_endian(value);
}

/** @brief Output positions and counts tracked while writing the compact image. */
struct compact_hash32_writer_state {
    std::span<std::byte> image{};
    const compact_hash32_image_plan* plan = nullptr;
    std::uint64_t next_rank_checkpoint = 0u;
    std::uint64_t inline_singletons_written = 0u;
    std::uint64_t materialized_hashes_written = 0u;
    std::uint64_t materialized_positions_written = 0u;
};

/**
 * @brief Check sorted hash lists while counting or writing their accepted positions.
 *
 * Planning uses a null writer. During the second scan, write into the planned
 * sections and reject counts that would exceed them. Both scans checksum the
 * complete source, including lists excluded by the occurrence limit.
 */
class compact_hash32_scan_state {
  private:
    const compact_hash32_build_options& options_;
    compact_hash32_geometry_counts geometry_{};
    compact_hash32_writer_state* writer_ = nullptr;
    compact_hash32_scan_receipt receipt_{};
    crc64_state stream_crc_{};
    std::optional<direct_inverted_index_error> error_{};
    compact_hash32_sorted_run_header active_{};
    bool active_run_ = false;
    bool active_accepted_ = false;
    bool have_previous_hash_ = false;
    std::uint32_t previous_hash_ = 0u;
    std::uint64_t active_positions_seen_ = 0u;
    std::uint64_t active_descriptor_offset_ = 0u;
    bool active_inline_singleton_ = false;
    std::array<std::uint64_t, 2> previous_bp_{};
    std::array<bool, 2> have_previous_bp_{};

    /** @brief Records the first scan error and returns it for every later callback. */
    [[nodiscard]] std::expected<void, direct_inverted_index_error>
    fail_(direct_inverted_index_error error) noexcept {
        if (!error_)
            error_ = error;
        return std::unexpected(*error_);
    }

    /** @brief Check that a write fits inside its planned section and the output image. */
    [[nodiscard]] bool writer_range_within_(std::uint64_t offset,
                                            std::uint64_t bytes,
                                            std::uint64_t section_offset,
                                            std::uint64_t section_bytes) const noexcept {
        if (writer_ == nullptr || writer_->plan == nullptr)
            return false;
        std::uint64_t end = 0u;
        std::uint64_t section_end = 0u;
        return checked_add(offset, bytes, end) &&
               checked_add(section_offset, section_bytes, section_end) &&
               offset >= section_offset && end <= section_end && end <= writer_->image.size();
    }

    /** @brief Stop a replay whose running counts already exceed the planned totals. */
    [[nodiscard]] std::expected<void, direct_inverted_index_error>
    validate_replay_prefix_(bool accepted,
                            std::uint64_t accepted_hash_before,
                            std::uint64_t accepted_posting_before,
                            const compact_hash32_sorted_run_header& header) noexcept {
        if (writer_ == nullptr)
            return {};
        if (writer_->plan == nullptr ||
            receipt_.source_hash_count > writer_->plan->source_hash_count ||
            receipt_.source_posting_count > writer_->plan->source_posting_count ||
            receipt_.accepted_hash_count > writer_->plan->accepted_hash_count ||
            receipt_.accepted_posting_count > writer_->plan->accepted_posting_count ||
            receipt_.inline_singleton_count > writer_->plan->inline_singleton_count ||
            receipt_.rejected_hash_count > writer_->plan->rejected_hash_count ||
            receipt_.rejected_posting_count > writer_->plan->rejected_posting_count ||
            (accepted && (accepted_hash_before >= writer_->plan->accepted_hash_count ||
                          accepted_posting_before > writer_->plan->accepted_posting_count ||
                          header.global_count >
                              writer_->plan->accepted_posting_count - accepted_posting_before))) {
            return fail_(direct_inverted_index_error::replay_mismatch);
        }
        return {};
    }

    /** @brief Finish the current hash only after receiving its declared position count. */
    [[nodiscard]] std::expected<void, direct_inverted_index_error> finalize_active_() noexcept {
        if (!active_run_)
            return {};
        if (active_positions_seen_ != active_.global_count) {
            return fail_(direct_inverted_index_error::source_read);
        }
        active_run_ = false;
        active_accepted_ = false;
        return {};
    }

    /** @brief Fill rank checkpoints up to this group with the number of earlier accepted hashes. */
    [[nodiscard]] std::expected<void, direct_inverted_index_error>
    write_rank_through_(std::uint64_t checkpoint, std::uint64_t accepted_before) noexcept {
        if (writer_ == nullptr)
            return {};
        while (writer_->next_rank_checkpoint <= checkpoint) {
            if (accepted_before > (std::numeric_limits<std::uint32_t>::max)()) {
                return fail_(direct_inverted_index_error::fixed_width);
            }
            std::uint64_t byte_delta = 0u;
            std::uint64_t byte_offset = 0u;
            if (writer_->next_rank_checkpoint >= geometry_.rank_checkpoint_count ||
                !checked_multiply(writer_->next_rank_checkpoint, 4u, byte_delta) ||
                !checked_add(writer_->plan->rank_offset, byte_delta, byte_offset) ||
                !writer_range_within_(
                    byte_offset, 4u, writer_->plan->rank_offset, writer_->plan->rank_bytes)) {
                return fail_(direct_inverted_index_error::replay_mismatch);
            }
            write_le<std::uint32_t>(writer_->image,
                                    static_cast<std::size_t>(byte_offset),
                                    static_cast<std::uint32_t>(accepted_before));
            ++writer_->next_rank_checkpoint;
        }
        return {};
    }

  public:
    /**
     * @brief Prepare a source scan for counting or writing positions.
     * @param[in] options Source identity, occurrence cap, and hash domain.
     * @param[in] geometry Bitmap and rank-table sizes for that domain.
     * @param[in,out] writer Output state, or null to count without writing.
     */
    compact_hash32_scan_state(const compact_hash32_build_options& options,
                              compact_hash32_geometry_counts geometry,
                              compact_hash32_writer_state* writer) noexcept
        : options_(options), geometry_(geometry), writer_(writer) {}

    /**
     * @brief Start the next hash list and decide whether its occurrence count is accepted.
     * @param[in] header Hash, total position count, and number on strand zero.
     * @return Success, or an error for invalid order, counts, or replay data.
     */
    [[nodiscard]] std::expected<void, direct_inverted_index_error>
    begin_run(const compact_hash32_sorted_run_header& header) noexcept {
        if (error_)
            return std::unexpected(*error_);
        if (auto finalized = finalize_active_(); !finalized) {
            return finalized;
        }
        if (header.global_count == 0u || header.target_strand_zero_count > header.global_count ||
            (have_previous_hash_ && header.hash <= previous_hash_) ||
            static_cast<std::uint64_t>(header.hash) >= geometry_.hash_count) {
            return fail_(have_previous_hash_ && header.hash <= previous_hash_
                             ? direct_inverted_index_error::order
                             : direct_inverted_index_error::occurrence_range);
        }
        std::uint64_t next = 0u;
        if (!checked_add(receipt_.source_hash_count, 1u, receipt_.source_hash_count) ||
            !checked_add(receipt_.source_posting_count,
                         header.global_count,
                         receipt_.source_posting_count)) {
            return fail_(direct_inverted_index_error::arithmetic_overflow);
        }
        const bool accepted = header.global_count <= options_.max_occ_cap;
        const bool inline_singleton =
            accepted && options_.identity.target_length <= (std::uint64_t{1u} << 31u) &&
            header.global_count == 1u;
        const std::uint64_t accepted_hash_before = receipt_.accepted_hash_count;
        const std::uint64_t accepted_posting_before = receipt_.accepted_posting_count;
        if (accepted) {
            if (header.global_count > (std::numeric_limits<std::uint32_t>::max)() ||
                header.target_strand_zero_count > (std::numeric_limits<std::uint32_t>::max)() ||
                !checked_add(receipt_.accepted_hash_count, 1u, receipt_.accepted_hash_count) ||
                !checked_add(receipt_.accepted_posting_count,
                             header.global_count,
                             receipt_.accepted_posting_count) ||
                (inline_singleton && !checked_add(receipt_.inline_singleton_count,
                                                  1u,
                                                  receipt_.inline_singleton_count)) ||
                receipt_.accepted_posting_count > (std::numeric_limits<std::uint32_t>::max)()) {
                return fail_(direct_inverted_index_error::fixed_width);
            }
        } else if (!checked_add(receipt_.rejected_hash_count, 1u, receipt_.rejected_hash_count) ||
                   !checked_add(receipt_.rejected_posting_count,
                                header.global_count,
                                receipt_.rejected_posting_count)) {
            return fail_(direct_inverted_index_error::arithmetic_overflow);
        }
        if (!checked_add(receipt_.accepted_hash_count, receipt_.rejected_hash_count, next) ||
            next != receipt_.source_hash_count) {
            return fail_(direct_inverted_index_error::arithmetic_overflow);
        }

        if (auto prefix = validate_replay_prefix_(
                accepted, accepted_hash_before, accepted_posting_before, header);
            !prefix) {
            return prefix;
        }

        crc_update_le(stream_crc_, header.hash);
        crc_update_le(stream_crc_, header.global_count);
        crc_update_le(stream_crc_, header.target_strand_zero_count);

        if (writer_ != nullptr) {
            const auto address = resolve_compact_hash32_address(header.hash, options_.geometry);
            if (!address)
                return fail_(address.error());
            if (auto ranks =
                    write_rank_through_(address->rank_checkpoint_ordinal, accepted_hash_before);
                !ranks) {
                return ranks;
            }
            if (accepted) {
                std::uint64_t membership_offset = 0u;
                std::uint64_t descriptor_delta = 0u;
                std::uint64_t descriptor_offset = 0u;
                if (address->membership_byte_ordinal >= writer_->plan->membership_bytes ||
                    !checked_add(writer_->plan->membership_offset,
                                 address->membership_byte_ordinal,
                                 membership_offset) ||
                    !writer_range_within_(membership_offset,
                                          1u,
                                          writer_->plan->membership_offset,
                                          writer_->plan->membership_bytes) ||
                    accepted_hash_before >= writer_->plan->accepted_hash_count ||
                    !checked_multiply(accepted_hash_before,
                                      writer_->plan->descriptor_width_bytes,
                                      descriptor_delta) ||
                    !checked_add(
                        writer_->plan->descriptors_offset, descriptor_delta, descriptor_offset) ||
                    !writer_range_within_(descriptor_offset,
                                          writer_->plan->descriptor_width_bytes,
                                          writer_->plan->descriptors_offset,
                                          writer_->plan->descriptors_bytes) ||
                    writer_->plan->layout_revision !=
                        k_compact_hash32_direct_index_layout_revision ||
                    writer_->plan->descriptor_width_bytes !=
                        k_compact_hash32_direct_index_descriptor_bytes ||
                    header.global_count > (std::numeric_limits<std::uint16_t>::max)() ||
                    header.target_strand_zero_count > (std::numeric_limits<std::uint16_t>::max)()) {
                    return fail_(direct_inverted_index_error::replay_mismatch);
                }
                writer_->image[static_cast<std::size_t>(membership_offset)] |=
                    static_cast<std::byte>(std::uint8_t{1u} << address->membership_bit);
                write_le<std::uint32_t>(
                    writer_->image,
                    static_cast<std::size_t>(descriptor_offset),
                    inline_singleton
                        ? 0u
                        : static_cast<std::uint32_t>(writer_->materialized_positions_written));
                write_le<std::uint16_t>(writer_->image,
                                        static_cast<std::size_t>(descriptor_offset + 4u),
                                        static_cast<std::uint16_t>(header.global_count));
                write_le<std::uint16_t>(
                    writer_->image,
                    static_cast<std::size_t>(descriptor_offset + 6u),
                    static_cast<std::uint16_t>(header.target_strand_zero_count));
                active_descriptor_offset_ = descriptor_offset;
                active_inline_singleton_ = inline_singleton;
                if (!inline_singleton) {
                    ++writer_->materialized_hashes_written;
                }
            }
        }

        active_ = header;
        active_run_ = true;
        active_accepted_ = accepted;
        active_positions_seen_ = 0u;
        previous_bp_.fill(0u);
        have_previous_bp_.fill(false);
        previous_hash_ = header.hash;
        have_previous_hash_ = true;
        return {};
    }

    /**
     * @brief Check each batch and write accepted positions when an output image is supplied.
     * @param[in] encoded_positions Positions ordered by strand, then coordinate.
     * @return Success, or the first count, ordering, coordinate, or replay error.
     */
    [[nodiscard]] std::expected<void, direct_inverted_index_error>
    consume_positions(std::span<const std::uint64_t> encoded_positions) noexcept {
        if (error_)
            return std::unexpected(*error_);
        if (!active_run_ ||
            encoded_positions.size() > active_.global_count - active_positions_seen_) {
            return fail_(direct_inverted_index_error::source_read);
        }
        for (const std::uint64_t encoded : encoded_positions) {
            const bool strand_one = (encoded & 1u) != 0u;
            const bool expected_strand = active_positions_seen_ >= active_.target_strand_zero_count;
            const std::uint64_t bp = encoded >> 1u;
            if (strand_one != expected_strand || bp >= options_.identity.target_length ||
                (have_previous_bp_[strand_one] && bp < previous_bp_[strand_one])) {
                return fail_(strand_one != expected_strand ? direct_inverted_index_error::order
                                                           : direct_inverted_index_error::posting);
            }
            crc_update_le(stream_crc_, encoded);
            if (writer_ != nullptr && active_accepted_) {
                if (active_inline_singleton_) {
                    if (writer_->plan == nullptr || writer_->plan->posting_width_bytes != 4u ||
                        active_.global_count != 1u || active_positions_seen_ != 0u ||
                        encoded > (std::numeric_limits<std::uint32_t>::max)() ||
                        !writer_range_within_(active_descriptor_offset_,
                                              4u,
                                              writer_->plan->descriptors_offset,
                                              writer_->plan->descriptors_bytes)) {
                        return fail_(direct_inverted_index_error::replay_mismatch);
                    }
                    write_le<std::uint32_t>(writer_->image,
                                            static_cast<std::size_t>(active_descriptor_offset_),
                                            static_cast<std::uint32_t>(encoded));
                    ++writer_->inline_singletons_written;
                    previous_bp_[strand_one] = bp;
                    have_previous_bp_[strand_one] = true;
                    ++active_positions_seen_;
                    continue;
                }
                std::uint64_t posting_delta = 0u;
                std::uint64_t posting_offset = 0u;
                if (writer_->plan == nullptr ||
                    writer_->materialized_positions_written >=
                        writer_->plan->materialized_posting_count ||
                    !checked_multiply(writer_->materialized_positions_written,
                                      writer_->plan->posting_width_bytes,
                                      posting_delta) ||
                    !checked_add(writer_->plan->postings_offset, posting_delta, posting_offset) ||
                    !writer_range_within_(posting_offset,
                                          writer_->plan->posting_width_bytes,
                                          writer_->plan->postings_offset,
                                          writer_->plan->postings_bytes)) {
                    return fail_(direct_inverted_index_error::replay_mismatch);
                }
                if (writer_->plan->posting_width_bytes == 4u) {
                    if (encoded > (std::numeric_limits<std::uint32_t>::max)()) {
                        return fail_(direct_inverted_index_error::posting);
                    }
                    write_le<std::uint32_t>(writer_->image,
                                            static_cast<std::size_t>(posting_offset),
                                            static_cast<std::uint32_t>(encoded));
                } else {
                    write_le<std::uint64_t>(
                        writer_->image, static_cast<std::size_t>(posting_offset), encoded);
                }
                ++writer_->materialized_positions_written;
            }
            previous_bp_[strand_one] = bp;
            have_previous_bp_[strand_one] = true;
            ++active_positions_seen_;
        }
        return {};
    }

    /**
     * @brief Complete the last list and return the source counts and checksum.
     * Writing also fills the remaining rank checkpoints and checks output totals.
     * @return Scan result, or the first validation or replay error.
     */
    [[nodiscard]] std::expected<compact_hash32_scan_receipt, direct_inverted_index_error>
    finish() noexcept {
        if (error_)
            return std::unexpected(*error_);
        if (auto finalized = finalize_active_(); !finalized) {
            return std::unexpected(finalized.error());
        }
        if (writer_ != nullptr) {
            if (receipt_.accepted_hash_count > (std::numeric_limits<std::uint32_t>::max)() &&
                writer_->next_rank_checkpoint < geometry_.rank_checkpoint_count) {
                return std::unexpected(direct_inverted_index_error::fixed_width);
            }
            while (writer_->next_rank_checkpoint < geometry_.rank_checkpoint_count) {
                std::uint64_t delta = 0u;
                std::uint64_t offset = 0u;
                if (!checked_multiply(writer_->next_rank_checkpoint, 4u, delta) ||
                    !checked_add(writer_->plan->rank_offset, delta, offset) ||
                    !writer_range_within_(
                        offset, 4u, writer_->plan->rank_offset, writer_->plan->rank_bytes)) {
                    return std::unexpected(direct_inverted_index_error::replay_mismatch);
                }
                write_le<std::uint32_t>(writer_->image,
                                        static_cast<std::size_t>(offset),
                                        static_cast<std::uint32_t>(receipt_.accepted_hash_count));
                ++writer_->next_rank_checkpoint;
            }
            if (writer_->inline_singletons_written != receipt_.inline_singleton_count ||
                writer_->inline_singletons_written != writer_->plan->inline_singleton_count ||
                writer_->materialized_hashes_written != writer_->plan->materialized_hash_count ||
                writer_->materialized_positions_written !=
                    writer_->plan->materialized_posting_count) {
                return std::unexpected(direct_inverted_index_error::replay_mismatch);
            }
        }
        receipt_.sorted_stream_crc64 = stream_crc_.checksum();
        return receipt_;
    }

    /** @brief Forward the receiver callback to this scanner's begin_run(). */
    [[nodiscard]] static std::expected<void, direct_inverted_index_error>
    begin_thunk(void* context, const compact_hash32_sorted_run_header& header) noexcept {
        return static_cast<compact_hash32_scan_state*>(context)->begin_run(header);
    }

    /** @brief Forward the receiver callback to this scanner's consume_positions(). */
    [[nodiscard]] static std::expected<void, direct_inverted_index_error>
    consume_thunk(void* context, std::span<const std::uint64_t> positions) noexcept {
        return static_cast<compact_hash32_scan_state*>(context)->consume_positions(positions);
    }
};

/**
 * @brief Read every sorted hash list to size or write a compact index.
 * @param[in] source Replayable source consumed during this call.
 * @param[in] options Source identity, occurrence cap, and hash domain.
 * @param[in,out] writer Output state, or null to count without writing.
 * @return Source counts and checksum, or an error from reading or validation.
 */
[[nodiscard]] inline std::expected<compact_hash32_scan_receipt, direct_inverted_index_error>
scan_source(const compact_hash32_sorted_run_source& source,
            const compact_hash32_build_options& options,
            compact_hash32_writer_state* writer = nullptr) {
    const auto geometry = geometry_counts(options.geometry);
    if (!geometry)
        return std::unexpected(geometry.error());
    compact_hash32_scan_state state{options, *geometry, writer};
    const compact_hash32_sorted_run_receiver receiver{
        .context = &state,
        .begin_run_fn = &compact_hash32_scan_state::begin_thunk,
        .consume_positions_fn = &compact_hash32_scan_state::consume_thunk,
    };
    if (auto scanned = source.scan_sorted_runs(receiver); !scanned) {
        return std::unexpected(scanned.error());
    }
    return state.finish();
}

} // namespace bioseq::indexing::compact_hash32_detail

export namespace bioseq::indexing {

/**
 * @brief Replay the source into the compact image's planned byte ranges.
 *
 * Clear the destination before writing. An error can leave a partial image.
 * On success, source counts and checksum match the planning scan, and each
 * stored section has its own checksum in the completed header.
 *
 * @param[in] source Replayable hash lists in strictly increasing hash order.
 * @param[in] options Settings used for the planning scan.
 * @param[in] planned Layout and source counts from that scan.
 * @param[out] image Destination with exactly the planned byte count.
 * @return Success, or a settings, replay, source, or encoding error.
 */
[[nodiscard]] inline std::expected<void, direct_inverted_index_error>
build_compact_hash32_direct_index_into(const compact_hash32_sorted_run_source& source,
                                       const compact_hash32_build_options& options,
                                       const compact_hash32_image_plan& planned,
                                       std::span<std::byte> image) {
    using namespace compact_hash32_detail;
    const compact_hash32_scan_receipt claimed_receipt{
        .source_hash_count = planned.source_hash_count,
        .source_posting_count = planned.source_posting_count,
        .accepted_hash_count = planned.accepted_hash_count,
        .accepted_posting_count = planned.accepted_posting_count,
        .inline_singleton_count = planned.inline_singleton_count,
        .rejected_hash_count = planned.rejected_hash_count,
        .rejected_posting_count = planned.rejected_posting_count,
        .sorted_stream_crc64 = planned.sorted_stream_crc64,
    };
    const auto canonical = layout_from_receipt(options, claimed_receipt);
    if (!canonical || *canonical != planned || planned.image_bytes > options.max_image_bytes ||
        planned.image_bytes >
            static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()) ||
        image.size() != planned.image_bytes) {
        return std::unexpected(direct_inverted_index_error::storage_contract_mismatch);
    }
    std::fill(image.begin(), image.end(), std::byte{0});
    compact_hash32_writer_state writer{
        .image = image,
        .plan = &planned,
    };
    const auto replay = scan_source(source, options, &writer);
    if (!replay)
        return std::unexpected(replay.error());
    const compact_hash32_scan_receipt expected{
        .source_hash_count = planned.source_hash_count,
        .source_posting_count = planned.source_posting_count,
        .accepted_hash_count = planned.accepted_hash_count,
        .accepted_posting_count = planned.accepted_posting_count,
        .inline_singleton_count = planned.inline_singleton_count,
        .rejected_hash_count = planned.rejected_hash_count,
        .rejected_posting_count = planned.rejected_posting_count,
        .sorted_stream_crc64 = planned.sorted_stream_crc64,
    };
    if (*replay != expected) {
        return std::unexpected(direct_inverted_index_error::replay_mismatch);
    }

    // Read the completed section bytes to calculate their checksum.
    const auto section = [&image](std::uint64_t offset, std::uint64_t bytes) noexcept {
        return std::span<const std::byte>{image}.subspan(static_cast<std::size_t>(offset),
                                                         static_cast<std::size_t>(bytes));
    };
    const std::uint64_t membership_crc =
        crc64(section(planned.membership_offset, planned.membership_bytes));
    const std::uint64_t rank_crc = crc64(section(planned.rank_offset, planned.rank_bytes));
    const std::uint64_t descriptors_crc =
        crc64(section(planned.descriptors_offset, planned.descriptors_bytes));
    const std::uint64_t postings_crc =
        crc64(section(planned.postings_offset, planned.postings_bytes));

    write_le<std::uint64_t>(image, 0u, k_compact_hash32_direct_index_magic);
    write_le<std::uint32_t>(image, 8u, planned.layout_revision);
    write_le<std::uint32_t>(image, 12u, k_compact_hash32_direct_index_header_bytes);
    write_le<std::uint32_t>(image, 16u, 0u);
    image[20u] = static_cast<std::byte>(planned.posting_width_bytes);
    image[21u] = static_cast<std::byte>(planned.geometry.hash_domain_bits);
    write_le<std::uint16_t>(image, 22u, planned.descriptor_width_bytes);
    write_le<std::uint64_t>(image, 24u, planned.identity.generation_id);
    write_le<std::uint64_t>(image, 32u, planned.identity.source_identity);
    write_le<std::uint64_t>(image, 40u, planned.identity.storage_contract_checksum);
    write_le<std::uint64_t>(image, 48u, planned.identity.target_length);
    write_le<std::uint32_t>(image, 56u, planned.max_occ_cap);
    write_le<std::uint32_t>(image, 60u, k_compact_hash32_direct_index_rank_group_bits);
    write_le<std::uint64_t>(image, 64u, planned.source_hash_count);
    write_le<std::uint64_t>(image, 72u, planned.source_posting_count);
    write_le<std::uint64_t>(image, 80u, planned.accepted_hash_count);
    write_le<std::uint64_t>(image, 88u, planned.accepted_posting_count);
    write_le<std::uint64_t>(image, 96u, planned.rejected_hash_count);
    write_le<std::uint64_t>(image, 104u, planned.rejected_posting_count);
    write_le<std::uint64_t>(image, 112u, planned.membership_offset);
    write_le<std::uint64_t>(image, 120u, planned.membership_bytes);
    write_le<std::uint64_t>(image, 128u, planned.rank_offset);
    write_le<std::uint64_t>(image, 136u, planned.rank_bytes);
    write_le<std::uint64_t>(image, 144u, planned.descriptors_offset);
    write_le<std::uint64_t>(image, 152u, planned.descriptors_bytes);
    write_le<std::uint64_t>(image, 160u, planned.postings_offset);
    write_le<std::uint64_t>(image, 168u, planned.postings_bytes);
    write_le<std::uint64_t>(image, k_compact_hash32_membership_crc_offset, membership_crc);
    write_le<std::uint64_t>(image, k_compact_hash32_rank_crc_offset, rank_crc);
    write_le<std::uint64_t>(image, k_compact_hash32_descriptors_crc_offset, descriptors_crc);
    write_le<std::uint64_t>(image, k_compact_hash32_postings_crc_offset, postings_crc);
    write_le<std::uint64_t>(image, 216u, planned.image_bytes);
    write_le<std::uint64_t>(image, k_compact_hash32_stream_crc_offset, planned.sorted_stream_crc64);
    write_le<std::uint32_t>(image, 232u, planned.geometry.max_hash);
    write_le<std::uint64_t>(
        image, k_compact_hash32_inline_singleton_count_offset, planned.inline_singleton_count);
    const std::uint64_t header_checksum = header_crc(
        std::span<const std::byte>{image}.first(k_compact_hash32_direct_index_header_bytes),
        k_compact_hash32_header_crc_offset,
        k_direct_inverted_index_checksum_bytes);
    write_le<std::uint64_t>(image, k_compact_hash32_header_crc_offset, header_checksum);
    return {};
}

} // namespace bioseq::indexing

namespace bioseq::indexing::direct_index_detail {

/** @brief An input occurrence with its target-fragment number added. */
struct direct_index_prepared_occurrence {
    std::uint32_t hash = 0u;
    std::uint32_t fragment = 0u;
    std::uint64_t target_bp = 0u;
    bool strand_one = false;
};

/** @brief One hash's range in the sorted occurrences, with strand and fragment counts. */
struct direct_index_hash_group {
    std::uint32_t hash = 0u;
    std::uint64_t begin = 0u;
    std::uint64_t count = 0u;
    std::uint64_t strand_zero_count = 0u;
    std::uint32_t summary_count = 0u;
};

/** @brief Temporary sorted data, section sizes, and the bucket-to-group table. */
struct direct_index_prepared_build {
    direct_index_image_plan plan{};
    std::vector<direct_index_prepared_occurrence> occurrences{};
    std::vector<direct_index_hash_group> groups{};
    std::vector<std::uint64_t> group_by_slot{};
};

/**
 * @brief Choose a power-of-two table with at most 7/8 of its buckets occupied.
 * @param[in] unique_hash_count Number of distinct hashes to store.
 * @param[in] override_count Requested table size, or zero to calculate it.
 * @return Bucket count, or an invalid-size or overflow error.
 */
[[nodiscard]] constexpr std::expected<std::uint64_t, direct_inverted_index_error>
choose_bucket_count(std::uint64_t unique_hash_count, std::uint64_t override_count) noexcept {
    if (override_count != 0u) {
        if (!power_of_two(override_count) || override_count < 8u) {
            return std::unexpected(direct_inverted_index_error::bucket_count);
        }
        if (unique_hash_count > override_count - override_count / 8u) {
            return std::unexpected(direct_inverted_index_error::bucket_count);
        }
        return override_count;
    }
    std::uint64_t numerator = 0u;
    if (!checked_multiply(unique_hash_count, 8u, numerator)) {
        return std::unexpected(direct_inverted_index_error::arithmetic_overflow);
    }
    const std::uint64_t minimum = std::max<std::uint64_t>(8u, ceil_div(numerator, 7u));
    const auto selected = next_power_of_two(minimum);
    if (!selected) {
        return std::unexpected(direct_inverted_index_error::bucket_count);
    }
    return *selected;
}

/**
 * @brief Sort occurrences, group equal hashes, and assign their dictionary slots.
 *
 * Check coordinates and configured memory limits before building the output
 * image. Optional fragment summaries require positions grouped by fragment
 * within each hash.
 *
 * @param[in] input Hashes, target positions, and strands in any order.
 * @param[in] options Source identity, fragment settings, and memory limits.
 * @return Prepared data and image layout, or a validation or allocation error.
 */
[[nodiscard]] inline std::expected<direct_index_prepared_build, direct_inverted_index_error>
prepare_build(std::span<const direct_index_occurrence> input,
              const direct_index_build_options& options) {
    if (options.identity.target_length > (std::uint64_t{1u} << 63u) ||
        options.identity.generation_id == 0u || options.identity.source_identity == 0u ||
        options.identity.storage_contract_checksum == 0u ||
        input.size() > options.max_occurrences) {
        return std::unexpected(direct_inverted_index_error::invalid_options);
    }
    std::uint64_t prepared_bytes = 0u;
    std::uint64_t group_upper_bound_bytes = 0u;
    std::uint64_t pre_dictionary_scratch = 0u;
    if (!checked_multiply(input.size(), sizeof(direct_index_prepared_occurrence), prepared_bytes) ||
        !checked_multiply(input.size(), sizeof(direct_index_hash_group), group_upper_bound_bytes) ||
        !checked_add(prepared_bytes, group_upper_bound_bytes, pre_dictionary_scratch)) {
        return std::unexpected(direct_inverted_index_error::arithmetic_overflow);
    }
    if (pre_dictionary_scratch > options.max_build_scratch_bytes) {
        return std::unexpected(direct_inverted_index_error::admission);
    }

    try {
        direct_index_prepared_build prepared;
        prepared.occurrences.reserve(input.size());
        prepared.groups.reserve(input.size());
        for (const direct_index_occurrence& occurrence : input) {
            if (occurrence.target_bp >= options.identity.target_length) {
                return std::unexpected(direct_inverted_index_error::occurrence_range);
            }
            std::uint32_t fragment = 0u;
            if (options.fragment_span_bp != 0u) {
                const std::uint64_t wide_fragment = occurrence.target_bp / options.fragment_span_bp;
                if (wide_fragment >= k_direct_inverted_index_no_fragment) {
                    return std::unexpected(direct_inverted_index_error::occurrence_range);
                }
                fragment = static_cast<std::uint32_t>(wide_fragment);
            }
            prepared.occurrences.push_back(direct_index_prepared_occurrence{
                .hash = occurrence.hash,
                .fragment = fragment,
                .target_bp = occurrence.target_bp,
                .strand_one = occurrence.target_strand_one,
            });
        }
        const bool summaries = options.fragment_span_bp != 0u;
        // Keep each hash's positions together, ordered by optional fragment, strand, and coordinate.
        std::sort(prepared.occurrences.begin(),
                  prepared.occurrences.end(),
                  [summaries](const direct_index_prepared_occurrence& left,
                              const direct_index_prepared_occurrence& right) noexcept {
                      if (left.hash != right.hash)
                          return left.hash < right.hash;
                      if (summaries && left.fragment != right.fragment) {
                          return left.fragment < right.fragment;
                      }
                      if (left.strand_one != right.strand_one) {
                          return left.strand_one < right.strand_one;
                      }
                      return left.target_bp < right.target_bp;
                  });

        std::uint64_t summary_count = 0u;
        for (std::size_t begin = 0u; begin < prepared.occurrences.size();) {
            std::size_t end = begin + 1u;
            while (end < prepared.occurrences.size() &&
                   prepared.occurrences[end].hash == prepared.occurrences[begin].hash) {
                ++end;
            }
            std::uint64_t strand_zero_count = 0u;
            std::uint32_t group_summaries = 0u;
            std::uint32_t previous_fragment = 0u;
            bool have_fragment = false;
            for (std::size_t ordinal = begin; ordinal < end; ++ordinal) {
                strand_zero_count += !prepared.occurrences[ordinal].strand_one;
                if (summaries && (!have_fragment ||
                                  previous_fragment != prepared.occurrences[ordinal].fragment)) {
                    if (group_summaries == (std::numeric_limits<std::uint32_t>::max)()) {
                        return std::unexpected(direct_inverted_index_error::arithmetic_overflow);
                    }
                    ++group_summaries;
                    ++summary_count;
                    previous_fragment = prepared.occurrences[ordinal].fragment;
                    have_fragment = true;
                }
            }
            prepared.groups.push_back(direct_index_hash_group{
                .hash = prepared.occurrences[begin].hash,
                .begin = begin,
                .count = end - begin,
                .strand_zero_count = strand_zero_count,
                .summary_count = group_summaries,
            });
            begin = end;
        }
        const std::uint64_t unique_hash_count = prepared.groups.size();
        if (unique_hash_count > options.max_unique_hashes ||
            summary_count > options.max_run_summaries) {
            return std::unexpected(direct_inverted_index_error::admission);
        }
        const auto bucket_count =
            choose_bucket_count(unique_hash_count, options.bucket_count_override);
        if (!bucket_count || *bucket_count > options.max_bucket_count ||
            *bucket_count > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
            return std::unexpected(bucket_count ? direct_inverted_index_error::admission
                                                : bucket_count.error());
        }
        const std::uint8_t posting_width =
            options.identity.target_length <= (std::uint64_t{1u} << 31u) ? 4u : 8u;
        const auto layout = layout_from_counts(
            input.size(), unique_hash_count, *bucket_count, summary_count, posting_width);
        if (!layout)
            return std::unexpected(layout.error());
        prepared.plan = *layout;
        if (prepared.plan.image_bytes > options.max_image_bytes ||
            prepared.plan.image_bytes >
                static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
            return std::unexpected(direct_inverted_index_error::admission);
        }
        std::uint64_t slots_bytes = 0u;
        std::uint64_t scratch_bytes = 0u;
        if (!checked_multiply(*bucket_count, sizeof(std::uint64_t), slots_bytes) ||
            !checked_add(prepared_bytes, group_upper_bound_bytes, scratch_bytes) ||
            !checked_add(scratch_bytes, slots_bytes, scratch_bytes)) {
            return std::unexpected(direct_inverted_index_error::arithmetic_overflow);
        }
        prepared.plan.estimated_build_scratch_bytes = scratch_bytes;
        if (scratch_bytes > options.max_build_scratch_bytes) {
            return std::unexpected(direct_inverted_index_error::admission);
        }

        prepared.group_by_slot.assign(static_cast<std::size_t>(*bucket_count),
                                      (std::numeric_limits<std::uint64_t>::max)());
        const std::uint64_t mask = *bucket_count - 1u;
        for (std::uint64_t group_ordinal = 0u; group_ordinal < prepared.groups.size();
             ++group_ordinal) {
            std::uint64_t slot =
                static_cast<std::uint64_t>(
                    mix_hash32(prepared.groups[static_cast<std::size_t>(group_ordinal)].hash)) &
                mask;
            std::uint64_t probes = 0u;
            while (prepared.group_by_slot[static_cast<std::size_t>(slot)] !=
                   (std::numeric_limits<std::uint64_t>::max)()) {
                slot = (slot + 1u) & mask;
                if (++probes == *bucket_count) {
                    return std::unexpected(direct_inverted_index_error::dictionary);
                }
            }
            prepared.group_by_slot[static_cast<std::size_t>(slot)] = group_ordinal;
        }
        return prepared;
    } catch (const std::bad_alloc&) {
        return std::unexpected(direct_inverted_index_error::allocation);
    } catch (const std::length_error&) {
        return std::unexpected(direct_inverted_index_error::allocation);
    }
}

} // namespace bioseq::indexing::direct_index_detail

export namespace bioseq::indexing {

/**
 * @brief Build a direct hash-table index from target occurrences in any order.
 *
 * Store posting lists beside an open-addressed hash dictionary, with optional
 * summaries for target fragments. Fields use little-endian byte order. Positions
 * use 32 or 64 bits depending on target length, with strand in the low bit.
 * Section checksums detect damage when the image is read.
 *
 * @param[in] input Occurrences read during this call.
 * @param[in] options Source identity, fragment settings, and size limits.
 * @return Owned image bytes, or a validation, size-limit, or allocation error.
 */
[[nodiscard]] inline std::expected<std::vector<std::byte>, direct_inverted_index_error>
build_direct_inverted_index(std::span<const direct_index_occurrence> input,
                            const direct_index_build_options& options) {
    using namespace direct_index_detail;
    auto prepared = prepare_build(input, options);
    if (!prepared)
        return std::unexpected(prepared.error());
    const direct_index_image_plan& plan = prepared->plan;
    try {
        std::vector<std::byte> image(static_cast<std::size_t>(plan.image_bytes), std::byte{0});
        std::span<std::byte> bytes{image};
        const bool summaries = options.fragment_span_bp != 0u;

        std::uint64_t dense_ordinal = 0u;
        std::uint64_t posting_cursor = plan.postings_offset;
        std::uint64_t summary_cursor = plan.summaries_offset;
        std::uint64_t occupied_count = 0u;
        const std::uint64_t rank_group_count = direct_index_detail::ceil_div(
            plan.bucket_count, k_direct_inverted_index_rank_group_buckets);
        for (std::uint64_t slot = 0u; slot < plan.bucket_count; ++slot) {
            if (slot % k_direct_inverted_index_rank_group_buckets == 0u) {
                write_le<std::uint64_t>(
                    bytes,
                    static_cast<std::size_t>(plan.rank_offset +
                                             (slot / k_direct_inverted_index_rank_group_buckets) *
                                                 8u),
                    occupied_count);
            }
            const std::uint64_t group_ordinal =
                prepared->group_by_slot[static_cast<std::size_t>(slot)];
            if (group_ordinal == (std::numeric_limits<std::uint64_t>::max)()) {
                continue;
            }
            bytes[static_cast<std::size_t>(plan.occupancy_offset + slot / 8u)] |=
                static_cast<std::byte>(std::uint8_t{1u} << (slot % 8u));
            ++occupied_count;
            const direct_index_hash_group& group =
                prepared->groups[static_cast<std::size_t>(group_ordinal)];
            const std::uint64_t control_offset =
                plan.controls_offset + dense_ordinal * k_direct_inverted_index_control_bytes;
            write_le<std::uint32_t>(bytes, static_cast<std::size_t>(control_offset), group.hash);
            write_le<std::uint32_t>(bytes,
                                    static_cast<std::size_t>(control_offset + 4u),
                                    summaries ? k_flag_run_summaries : 0u);
            write_le<std::uint64_t>(
                bytes, static_cast<std::size_t>(control_offset + 8u), group.count);
            write_le<std::uint64_t>(
                bytes, static_cast<std::size_t>(control_offset + 16u), group.strand_zero_count);
            write_le<std::uint64_t>(
                bytes, static_cast<std::size_t>(control_offset + 24u), posting_cursor);
            write_le<std::uint64_t>(bytes,
                                    static_cast<std::size_t>(control_offset + 32u),
                                    summaries ? summary_cursor : 0u);
            write_le<std::uint32_t>(
                bytes, static_cast<std::size_t>(control_offset + 40u), group.summary_count);

            const std::size_t group_begin = static_cast<std::size_t>(group.begin);
            const std::size_t group_end = static_cast<std::size_t>(group.begin + group.count);
            if (summaries) {
                std::uint64_t summary_posting_cursor = posting_cursor;
                std::size_t run_begin = group_begin;
                while (run_begin < group_end) {
                    std::size_t run_end = run_begin + 1u;
                    while (run_end < group_end && prepared->occurrences[run_end].fragment ==
                                                      prepared->occurrences[run_begin].fragment) {
                        ++run_end;
                    }
                    std::uint64_t run_strand_zero = 0u;
                    for (std::size_t ordinal = run_begin; ordinal < run_end; ++ordinal) {
                        run_strand_zero += !prepared->occurrences[ordinal].strand_one;
                    }
                    write_le<std::uint32_t>(bytes,
                                            static_cast<std::size_t>(summary_cursor),
                                            prepared->occurrences[run_begin].fragment);
                    write_le<std::uint64_t>(bytes,
                                            static_cast<std::size_t>(summary_cursor + 8u),
                                            summary_posting_cursor);
                    write_le<std::uint64_t>(
                        bytes, static_cast<std::size_t>(summary_cursor + 16u), run_strand_zero);
                    write_le<std::uint64_t>(
                        bytes, static_cast<std::size_t>(summary_cursor + 24u), run_end - run_begin);
                    std::uint64_t run_posting_bytes = 0u;
                    if (!checked_multiply(
                            run_end - run_begin, plan.posting_width_bytes, run_posting_bytes) ||
                        !checked_add(
                            summary_posting_cursor, run_posting_bytes, summary_posting_cursor)) {
                        return std::unexpected(direct_inverted_index_error::arithmetic_overflow);
                    }
                    summary_cursor += k_direct_inverted_index_run_summary_bytes;
                    run_begin = run_end;
                }
                std::uint64_t group_posting_bytes = 0u;
                std::uint64_t group_posting_end = 0u;
                if (!checked_multiply(group.count, plan.posting_width_bytes, group_posting_bytes) ||
                    !checked_add(posting_cursor, group_posting_bytes, group_posting_end) ||
                    summary_posting_cursor != group_posting_end) {
                    return std::unexpected(direct_inverted_index_error::arithmetic_overflow);
                }
            }
            for (std::size_t ordinal = group_begin; ordinal < group_end; ++ordinal) {
                const direct_index_prepared_occurrence& occurrence = prepared->occurrences[ordinal];
                const std::uint64_t encoded = (occurrence.target_bp << 1u) |
                                              static_cast<std::uint64_t>(occurrence.strand_one);
                if (plan.posting_width_bytes == 4u) {
                    write_le<std::uint32_t>(bytes,
                                            static_cast<std::size_t>(posting_cursor),
                                            static_cast<std::uint32_t>(encoded));
                } else {
                    write_le<std::uint64_t>(
                        bytes, static_cast<std::size_t>(posting_cursor), encoded);
                }
                posting_cursor += plan.posting_width_bytes;
            }
            ++dense_ordinal;
        }
        write_le<std::uint64_t>(bytes,
                                static_cast<std::size_t>(plan.rank_offset + rank_group_count * 8u),
                                occupied_count);
        if (dense_ordinal != plan.unique_hash_count || occupied_count != plan.unique_hash_count ||
            posting_cursor != plan.image_bytes ||
            summary_cursor != plan.summaries_offset + plan.summaries_bytes) {
            return std::unexpected(direct_inverted_index_error::control);
        }

        // Calculate a checksum from the completed section bytes.
        const auto section_crc = [&image](std::uint64_t offset, std::uint64_t count) noexcept {
            return direct_index_detail::crc64(std::span<const std::byte>{image}.subspan(
                static_cast<std::size_t>(offset), static_cast<std::size_t>(count)));
        };
        const std::uint64_t occupancy_crc =
            section_crc(plan.occupancy_offset, plan.occupancy_bytes);
        const std::uint64_t rank_crc = section_crc(plan.rank_offset, plan.rank_bytes);
        const std::uint64_t controls_crc = section_crc(plan.controls_offset, plan.controls_bytes);
        const std::uint64_t summaries_crc =
            section_crc(plan.summaries_offset, plan.summaries_bytes);
        const std::uint64_t postings_crc = section_crc(plan.postings_offset, plan.postings_bytes);

        write_le<std::uint64_t>(bytes, 0u, k_direct_inverted_index_magic);
        write_le<std::uint32_t>(bytes, 8u, k_direct_inverted_index_layout_revision);
        write_le<std::uint32_t>(bytes, 12u, k_direct_inverted_index_header_bytes);
        write_le<std::uint32_t>(bytes, 16u, summaries ? k_flag_run_summaries : 0u);
        bytes[20u] = static_cast<std::byte>(plan.posting_width_bytes);
        write_le<std::uint64_t>(bytes, 24u, options.identity.generation_id);
        write_le<std::uint64_t>(bytes, 32u, options.identity.source_identity);
        write_le<std::uint64_t>(bytes, 40u, options.identity.storage_contract_checksum);
        write_le<std::uint64_t>(bytes, 48u, options.identity.target_length);
        write_le<std::uint64_t>(bytes, 56u, plan.occurrence_count);
        write_le<std::uint64_t>(bytes, 64u, plan.unique_hash_count);
        write_le<std::uint64_t>(bytes, 72u, plan.bucket_count);
        write_le<std::uint64_t>(bytes, 80u, plan.occupancy_offset);
        write_le<std::uint64_t>(bytes, 88u, plan.occupancy_bytes);
        write_le<std::uint64_t>(bytes, 96u, plan.rank_offset);
        write_le<std::uint64_t>(bytes, 104u, plan.rank_bytes);
        write_le<std::uint64_t>(bytes, 112u, plan.controls_offset);
        write_le<std::uint64_t>(bytes, 120u, plan.controls_bytes);
        write_le<std::uint64_t>(bytes, 128u, plan.summaries_offset);
        write_le<std::uint64_t>(bytes, 136u, plan.summaries_bytes);
        write_le<std::uint64_t>(bytes, 144u, plan.postings_offset);
        write_le<std::uint64_t>(bytes, 152u, plan.postings_bytes);
        write_le<std::uint64_t>(bytes, 160u, options.fragment_span_bp);
        write_le<std::uint64_t>(bytes, 168u, plan.run_summary_count);
        write_le<std::uint64_t>(bytes, k_direct_inverted_index_occupancy_crc_offset, occupancy_crc);
        write_le<std::uint64_t>(bytes, k_direct_inverted_index_rank_crc_offset, rank_crc);
        write_le<std::uint64_t>(bytes, k_direct_inverted_index_controls_crc_offset, controls_crc);
        write_le<std::uint64_t>(bytes, k_direct_inverted_index_summaries_crc_offset, summaries_crc);
        write_le<std::uint64_t>(bytes, k_direct_inverted_index_postings_crc_offset, postings_crc);
        write_le<std::uint64_t>(bytes, 224u, plan.image_bytes);
        write_le<std::uint32_t>(bytes, 232u, k_direct_inverted_index_rank_group_buckets);
        write_le<std::uint32_t>(bytes, 236u, k_direct_inverted_index_control_bytes);
        write_le<std::uint32_t>(bytes, 240u, k_direct_inverted_index_run_summary_bytes);
        write_le<std::uint32_t>(bytes, 244u, k_direct_inverted_index_hash_mix_id);
        const std::uint64_t header_checksum = header_crc(
            std::span<const std::byte>{image}.first(k_direct_inverted_index_header_bytes),
            k_direct_inverted_index_header_crc_offset,
            k_direct_inverted_index_checksum_bytes);
        write_le<std::uint64_t>(bytes, k_direct_inverted_index_header_crc_offset, header_checksum);
        return image;
    } catch (const std::bad_alloc&) {
        return std::unexpected(direct_inverted_index_error::allocation);
    } catch (const std::length_error&) {
        return std::unexpected(direct_inverted_index_error::allocation);
    }
}

} // namespace bioseq::indexing
