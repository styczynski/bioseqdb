/**
 * @file cigar.cppm
 * @brief Canonical RLE sequence codec and position-aware borrowed ranges.
 */
export module bioseq.sequences:packed_sequence_cigar;

import std;
export import bioseq.sequences.cigar_alphabet;
import bioseq.common.checked_arithmetic;
import :packed_sequence_core;
import :packed_sequence_range;

export namespace bioseq {

inline constexpr std::uint32_t cigar_checkpoint_stride = 64u;

/** Self-describing prefix of a canonical run-encoded payload. */
struct cigar_payload_header {
    std::uint32_t run_count{};
    std::uint32_t checkpoint_count{};
};
/** Prefix position before each stride-th run. */
struct cigar_checkpoint {
    std::uint32_t logical_position{};
    std::uint32_t run_index{};
};
/** Result of a logical position lookup, including an offset within its run. */
struct cigar_run_position {
    std::uint32_t run_index{};
    std::uint32_t logical_position{};
    std::uint32_t offset{};
};
static_assert(sizeof(cigar_payload_header) == 8u);
static_assert(sizeof(cigar_checkpoint) == 8u);

/** Calculate checkpoint count without overflowing an addition. */
[[nodiscard]] constexpr std::uint32_t cigar_checkpoint_count(std::uint32_t run_count) noexcept {
    return common::ceil_div(run_count, cigar_checkpoint_stride);
}
/** Return the byte address of a checkpoint in a validated payload. */
[[nodiscard]] constexpr std::size_t cigar_checkpoint_offset(std::size_t index) noexcept {
    return sizeof(cigar_payload_header) + index * sizeof(cigar_checkpoint);
}
/** Return the first run byte address in a validated payload. */
[[nodiscard]] constexpr std::size_t cigar_runs_offset(std::uint32_t run_count) noexcept {
    return cigar_checkpoint_offset(cigar_checkpoint_count(run_count));
}
/** Return a run byte address after the run count has been validated. */
template<cigar_operation_alphabet Alphabet>
[[nodiscard]] constexpr std::size_t cigar_run_offset(std::uint32_t run_count, std::size_t index) noexcept {
    return cigar_runs_offset(run_count) + index * sizeof(cigar_run_value<Alphabet>);
}

/** Measure canonical payload bytes, returning an error on size_t overflow. */
template<cigar_operation_alphabet Alphabet = cigar_symbol>
[[nodiscard]] constexpr std::expected<std::size_t, cigar_error>
measure_cigar_payload(std::uint32_t run_count) noexcept {
    const auto directory = common::checked_mul(std::size_t{cigar_checkpoint_count(run_count)}, sizeof(cigar_checkpoint));
    const auto records = common::checked_mul(std::size_t{run_count}, sizeof(cigar_run_value<Alphabet>));
    if (!directory || !records) return std::unexpected(cigar_error{cigar_error_code::output_too_large});
    const auto prefix = common::checked_add(sizeof(cigar_payload_header), *directory);
    const auto total = prefix ? common::checked_add(*prefix, *records) : std::nullopt;
    if (!total) return std::unexpected(cigar_error{cigar_error_code::output_too_large});
    return *total;
}

/** Bounds-checked byte reader over resident encoded storage. */
struct cigar_span_reader {
    std::span<const std::byte> bytes;
    /** Copy one requested range into caller scratch without retaining its address. */
    [[nodiscard]] std::expected<void, cigar_error> read_bytes(
        std::size_t offset, std::span<std::byte> destination) const noexcept {
        if (offset > bytes.size() || destination.size() > bytes.size() - offset)
            return std::unexpected(cigar_error{cigar_error_code::invalid_encoding, offset});
        if (!destination.empty()) std::memcpy(destination.data(), bytes.data() + offset, destination.size());
        return {};
    }
};

/** A bounded encoded-byte provider, implemented by resident or fragmented storage. */
template<class Reader>
concept cigar_byte_reader = requires(const Reader& reader, std::size_t offset, std::span<std::byte> bytes) {
    { reader.read_bytes(offset, bytes) } -> std::same_as<std::expected<void, cigar_error>>;
};

/** Decode and validate the self-describing prefix of a resident byte span. */
[[nodiscard]] inline std::expected<cigar_payload_header, cigar_error>
read_cigar_payload_header(std::span<const std::byte> payload) noexcept {
    if (payload.size() < sizeof(cigar_payload_header))
        return std::unexpected(cigar_error{cigar_error_code::invalid_encoding});
    cigar_payload_header header;
    std::memcpy(std::addressof(header), payload.data(), sizeof(header));
    if (header.checkpoint_count != cigar_checkpoint_count(header.run_count))
        return std::unexpected(cigar_error{cigar_error_code::invalid_encoding});
    return header;
}

/** Shared indexed decoder; never assumes the backing bytes are contiguous. */
template<cigar_operation_alphabet Alphabet, cigar_byte_reader Reader>
class cigar_rle_reader {
    Reader reader_;
    std::uint32_t logical_length_{};
    cigar_space space_{cigar_space::compatibility};
public:
    using alphabet_type = Alphabet;
    using run_type = cigar_run_value<Alphabet>;

    /** Borrow a provider and its already checked logical extent and space. */
    constexpr cigar_rle_reader(Reader reader, std::uint32_t logical_length,
                              cigar_space space = cigar_space::compatibility)
        : reader_(std::move(reader)), logical_length_(logical_length), space_(space) {}

    /** Read and validate prefix cardinalities through the byte provider. */
    [[nodiscard]] std::expected<cigar_payload_header, cigar_error> read_header() const {
        std::array<std::byte, sizeof(cigar_payload_header)> bytes{};
        const auto copied = reader_.read_bytes(0u, bytes);
        if (!copied) return std::unexpected(copied.error());
        return read_cigar_payload_header(bytes);
    }
    /** Read the number of physical canonical runs. */
    [[nodiscard]] std::expected<std::uint32_t, cigar_error> run_count() const {
        const auto header = read_header();
        if (!header) return std::unexpected(header.error());
        return header->run_count;
    }
    /** Read a checkpoint and check its structural range. */
    [[nodiscard]] std::expected<cigar_checkpoint, cigar_error> checkpoint_at(
        std::uint32_t index, const cigar_payload_header& header) const {
        if (index >= header.checkpoint_count)
            return std::unexpected(cigar_error{cigar_error_code::invalid_encoding, index});
        std::array<std::byte, sizeof(cigar_checkpoint)> bytes{};
        const auto copied = reader_.read_bytes(cigar_checkpoint_offset(index), bytes);
        if (!copied) return std::unexpected(copied.error());
        cigar_checkpoint checkpoint;
        std::memcpy(std::addressof(checkpoint), bytes.data(), sizeof(checkpoint));
        if (checkpoint.run_index != index * cigar_checkpoint_stride ||
            checkpoint.logical_position > logical_length_)
            return std::unexpected(cigar_error{cigar_error_code::invalid_encoding, index});
        return checkpoint;
    }
    /** Decode a record using a previously validated cardinality. */
    [[nodiscard]] std::expected<run_type, cigar_error> run_at(
        std::uint32_t index, std::uint32_t run_count_value) const {
        static_assert(sizeof(Alphabet) == sizeof(seqan3::alphabet_rank_t<Alphabet>));
        static_assert(sizeof(run_type) >= sizeof(typename run_type::length_type) + sizeof(seqan3::alphabet_rank_t<Alphabet>));
        if (index >= run_count_value)
            return std::unexpected(cigar_error{cigar_error_code::invalid_encoding, index});
        std::array<std::byte, sizeof(run_type)> bytes{};
        const auto copied = reader_.read_bytes(cigar_run_offset<Alphabet>(run_count_value, index), bytes);
        if (!copied) return std::unexpected(copied.error());
        typename run_type::length_type length{};
        using rank_type = seqan3::alphabet_rank_t<Alphabet>;
        rank_type rank{};
        std::memcpy(std::addressof(length), bytes.data(), sizeof(length));
        std::memcpy(std::addressof(rank), bytes.data() + sizeof(length), sizeof(rank));
        if (length == 0u) return std::unexpected(cigar_error{cigar_error_code::zero_count, index});
        if (rank >= seqan3::alphabet_size<Alphabet>)
            return std::unexpected(cigar_error{cigar_error_code::invalid_operation, index});
        for (std::size_t padding = sizeof(length) + sizeof(rank); padding < bytes.size(); ++padding)
            if (bytes[padding] != std::byte{})
                return std::unexpected(cigar_error{cigar_error_code::invalid_encoding, index});
        const auto operation = Alphabet{}.assign_rank(rank);
        if (!visible_in_space(operation, space_))
            return std::unexpected(cigar_error{cigar_error_code::invalid_operation, index});
        return run_type{length, operation};
    }
    /** Read one canonical record, checking the prefix before addressing it. */
    [[nodiscard]] std::expected<run_type, cigar_error> run_at(std::uint32_t index) const {
        const auto header = read_header();
        if (!header) return std::unexpected(header.error());
        return run_at(index, header->run_count);
    }
    /** Find a position in logarithmic checkpoint lookups and at most one stride. */
    [[nodiscard]] std::expected<cigar_run_position, cigar_error> seek(std::uint32_t position) const {
        if (position > logical_length_ || !valid_cigar_space(space_))
            return std::unexpected(cigar_error{cigar_error_code::invalid_encoding, position});
        const auto header = read_header();
        if (!header) return std::unexpected(header.error());
        if (position == logical_length_) return cigar_run_position{header->run_count, logical_length_, 0u};
        if (header->checkpoint_count == 0u)
            return std::unexpected(cigar_error{cigar_error_code::invalid_encoding, position});
        std::uint32_t lower = 0u;
        std::uint32_t upper = header->checkpoint_count;
        while (lower < upper) {
            const auto middle = lower + (upper - lower) / 2u;
            const auto checkpoint = checkpoint_at(middle, *header);
            if (!checkpoint) return std::unexpected(checkpoint.error());
            if (checkpoint->logical_position <= position) lower = middle + 1u;
            else upper = middle;
        }
        if (lower == 0u) return std::unexpected(cigar_error{cigar_error_code::invalid_encoding});
        const auto checkpoint = checkpoint_at(lower - 1u, *header);
        if (!checkpoint) return std::unexpected(checkpoint.error());
        std::uint32_t prefix = checkpoint->logical_position;
        const auto stop = std::min<std::uint64_t>(header->run_count,
            static_cast<std::uint64_t>(checkpoint->run_index) + cigar_checkpoint_stride);
        for (std::uint32_t index = checkpoint->run_index; index < stop; ++index) {
            const auto run = run_at(index, header->run_count);
            if (!run) return std::unexpected(run.error());
            const auto end = common::checked_add(prefix, run->length);
            if (!end || *end > logical_length_)
                return std::unexpected(cigar_error{cigar_error_code::invalid_encoding, index});
            if (position < *end) return cigar_run_position{index, prefix, position - prefix};
            prefix = *end;
        }
        return std::unexpected(cigar_error{cigar_error_code::invalid_encoding, position});
    }
};

/** Seek through a provider using the common indexed codec. */
template<cigar_operation_alphabet Alphabet, cigar_byte_reader Reader>
[[nodiscard]] std::expected<cigar_run_position, cigar_error> cigar_seek(
    const Reader& reader, std::uint32_t logical_length, std::uint32_t position,
    cigar_space space = cigar_space::compatibility) {
    return cigar_rle_reader<Alphabet, Reader>{reader, logical_length, space}.seek(position);
}
/** Read a record through a provider using its checked run count. */
template<cigar_operation_alphabet Alphabet, cigar_byte_reader Reader>
[[nodiscard]] std::expected<cigar_run_value<Alphabet>, cigar_error> read_cigar_run(
    const Reader& reader, std::uint32_t index, std::uint32_t run_count,
    cigar_space space = cigar_space::compatibility) {
    return cigar_rle_reader<Alphabet, Reader>{reader, 0u, space}.run_at(index, run_count);
}

/** Validate every record, exact extent, directory entry, and codec padding once. */
template<cigar_operation_alphabet Alphabet>
[[nodiscard]] std::expected<void, cigar_error> validate_cigar_payload(
    std::span<const std::byte> payload, std::uint32_t logical_length,
    cigar_space space = cigar_space::compatibility) {
    if (!valid_cigar_space(space)) return std::unexpected(cigar_error{cigar_error_code::invalid_encoding});
    const auto header = read_cigar_payload_header(payload);
    if (!header) return std::unexpected(header.error());
    const auto measured = measure_cigar_payload<Alphabet>(header->run_count);
    if (!measured || *measured != payload.size())
        return std::unexpected(cigar_error{cigar_error_code::invalid_encoding});
    const cigar_rle_reader<Alphabet, cigar_span_reader> reader{{payload}, logical_length, space};
    std::uint32_t prefix = 0u;
    std::optional<Alphabet> previous;
    for (std::uint32_t index = 0u; index < header->run_count; ++index) {
        if (index % cigar_checkpoint_stride == 0u) {
            const auto checkpoint = reader.checkpoint_at(index / cigar_checkpoint_stride, *header);
            if (!checkpoint || checkpoint->logical_position != prefix)
                return std::unexpected(cigar_error{cigar_error_code::invalid_encoding, index});
        }
        const auto run = reader.run_at(index, header->run_count);
        if (!run) return std::unexpected(run.error());
        if (previous && *previous == run->operation)
            return std::unexpected(cigar_error{cigar_error_code::invalid_encoding, index});
        const auto sum = common::checked_add(prefix, run->length);
        if (!sum) return std::unexpected(cigar_error{cigar_error_code::length_overflow, index});
        prefix = *sum;
        previous = run->operation;
    }
    if (prefix != logical_length)
        return std::unexpected(cigar_error{cigar_error_code::input_length_mismatch});
    return {};
}

/** Streaming codec sink over one exact caller-owned canonical payload allocation. */
template<cigar_operation_alphabet Alphabet>
class cigar_payload_writer {
    using run_type = cigar_run_value<Alphabet>;
    std::span<std::byte> destination_;
    std::uint32_t expected_runs_{};
    std::uint32_t written_runs_{};
    std::uint32_t logical_length_{};
    cigar_space space_;
    std::optional<Alphabet> previous_;
    std::optional<cigar_error> error_;
public:
    /** Bind an exact measured allocation; malformed geometry fails subsequent calls. */
    cigar_payload_writer(std::span<std::byte> destination, std::uint32_t run_count,
                         cigar_space space = cigar_space::compatibility)
        : destination_(destination), expected_runs_(run_count), space_(space) {
        const auto measured = measure_cigar_payload<Alphabet>(run_count);
        if (!valid_cigar_space(space) || !measured || *measured != destination.size()) {
            error_ = cigar_error{cigar_error_code::invalid_encoding};
            return;
        }
        const cigar_payload_header header{run_count, cigar_checkpoint_count(run_count)};
        std::memcpy(destination_.data(), std::addressof(header), sizeof(header));
    }
    /** Append one positive canonical record and its checkpoint without intermediate storage. */
    [[nodiscard]] std::expected<void, cigar_error> append(run_type run) {
        if (error_) return std::unexpected(*error_);
        if (written_runs_ == expected_runs_) error_ = cigar_error{cigar_error_code::output_too_large, written_runs_};
        else if (run.length == 0u) error_ = cigar_error{cigar_error_code::zero_count, written_runs_};
        else if (seqan3::to_rank(run.operation) >= seqan3::alphabet_size<Alphabet> || !visible_in_space(run.operation, space_))
            error_ = cigar_error{cigar_error_code::invalid_operation, written_runs_};
        else if (previous_ && *previous_ == run.operation)
            error_ = cigar_error{cigar_error_code::invalid_encoding, written_runs_};
        const auto sum = common::checked_add(logical_length_, run.length);
        if (!sum) error_ = cigar_error{cigar_error_code::length_overflow, written_runs_};
        if (error_) return std::unexpected(*error_);
        if (written_runs_ % cigar_checkpoint_stride == 0u) {
            const cigar_checkpoint checkpoint{logical_length_, written_runs_};
            std::memcpy(destination_.data() + cigar_checkpoint_offset(written_runs_ / cigar_checkpoint_stride),
                        std::addressof(checkpoint), sizeof(checkpoint));
        }
        std::array<std::byte, sizeof(run_type)> record{};
        const auto rank = seqan3::to_rank(run.operation);
        std::memcpy(record.data(), std::addressof(run.length), sizeof(run.length));
        std::memcpy(record.data() + sizeof(run.length), std::addressof(rank), sizeof(rank));
        std::memcpy(destination_.data() + cigar_run_offset<Alphabet>(expected_runs_, written_runs_),
                    record.data(), record.size());
        logical_length_ = *sum;
        previous_ = run.operation;
        ++written_runs_;
        return {};
    }
    /** Verify exact run cardinality and return the checked logical extent. */
    [[nodiscard]] std::expected<std::uint32_t, cigar_error> finish() const {
        if (error_) return std::unexpected(*error_);
        if (written_runs_ != expected_runs_)
            return std::unexpected(cigar_error{cigar_error_code::input_length_mismatch, written_runs_});
        return logical_length_;
    }
};

/** Write a canonical run span through the same streaming codec used by parsers. */
template<cigar_operation_alphabet Alphabet>
[[nodiscard]] std::expected<std::uint32_t, cigar_error> write_cigar_payload(
    std::span<std::byte> destination, std::span<const cigar_run_value<Alphabet>> runs,
    cigar_space space = cigar_space::compatibility) {
    if (runs.size() > std::numeric_limits<std::uint32_t>::max())
        return std::unexpected(cigar_error{cigar_error_code::output_too_large});
    cigar_payload_writer<Alphabet> writer{destination, static_cast<std::uint32_t>(runs.size()), space};
    for (const auto run : runs) {
        const auto appended = writer.append(run);
        if (!appended) return std::unexpected(appended.error());
    }
    return writer.finish();
}

}

export {

/** Borrow validated canonical RLE bytes and a logical position interval. */
template<sequence_alphabet Alphabet>
    requires bioseq::cigar_operation_alphabet<Alphabet>
struct compact_sequence_range<Alphabet> {
    using run_type = bioseq::cigar_run_value<Alphabet>;
    struct traits {
        using alphabet_type = Alphabet;
        using storage_unit = run_type;
        using letter_unit = seqan3::alphabet_rank_t<Alphabet>;
        static constexpr std::size_t alphabet_size = seqan3::alphabet_size<Alphabet>;
        static constexpr std::size_t alphabet_size_n = alphabet_size + 1u;
        static constexpr std::size_t bits_per_char = seqan3::detail::ceil_log2(alphabet_size);
        static constexpr bool is_bit_packed = false;
        static constexpr bool is_run_encoded = true;
    };
private:
    std::span<const std::byte> payload_{};
    std::uint32_t logical_length_{};
    std::uint32_t from_{};
    std::uint32_t to_{};
    bioseq::cigar_space space_{bioseq::cigar_space::compatibility};

    /** Construct the indexed resident reader without copying the payload. */
    [[nodiscard]] auto reader() const {
        return bioseq::cigar_rle_reader<Alphabet, bioseq::cigar_span_reader>{{payload_}, logical_length_, space_};
    }
    /** Locate a trusted logical boundary; validated backing must remain immutable. */
    [[nodiscard]] bioseq::cigar_run_position seek(std::uint32_t position) const {
        if (payload_.empty()) return {};
        return *reader().seek(position);
    }
    /** Read one trusted record; validated backing must remain immutable. */
    [[nodiscard]] run_type physical_run(std::uint32_t index) const {
        // The constructor's validation proof covers this immutable backing.
        const auto header = *bioseq::read_cigar_payload_header(payload_);
        const auto* record = payload_.data() + bioseq::cigar_run_offset<Alphabet>(header.run_count, index);
        typename run_type::length_type length{};
        seqan3::alphabet_rank_t<Alphabet> rank{};
        std::memcpy(std::addressof(length), record, sizeof(length));
        std::memcpy(std::addressof(rank), record + sizeof(length), sizeof(rank));
        return run_type{length, Alphabet{}.assign_rank(rank)};
    }
public:
    compact_sequence_range() = default;
    /** Borrow immutable previously validated bytes for their entire logical extent. */
    explicit compact_sequence_range(std::span<const std::byte> payload,
        std::uint32_t logical_length, bioseq::cigar_space space = bioseq::cigar_space::compatibility)
        : payload_(payload), logical_length_(logical_length), to_(logical_length), space_(space) {}

    /** Validate untrusted bytes once, then return a cheap borrowed view. */
    [[nodiscard]] static std::expected<compact_sequence_range, bioseq::cigar_error> from_payload(
        std::span<const std::byte> payload, std::uint32_t logical_length,
        bioseq::cigar_space space = bioseq::cigar_space::compatibility) {
        const auto validated = bioseq::validate_cigar_payload<Alphabet>(payload, logical_length, space);
        if (!validated) return std::unexpected(validated.error());
        return compact_sequence_range{payload, logical_length, space};
    }
    /** Return the selected logical position count. */
    [[nodiscard]] constexpr std::size_t bp_size() const noexcept { return to_ - from_; }
    /** Return the value's declared operation subalphabet. */
    [[nodiscard]] constexpr bioseq::cigar_space space() const noexcept { return space_; }
    /** Return the entire immutable encoded backing, including prefix and directory. */
    [[nodiscard]] constexpr std::span<const std::byte> encoded_bytes() const noexcept { return payload_; }
    /** Return bytes of the whole borrowed backing payload. */
    [[nodiscard]] constexpr std::size_t encoded_payload_bytes() const noexcept { return payload_.size(); }
    /** RLE storage has no unused logical symbol slots. */
    [[nodiscard]] static constexpr std::uint16_t padding() noexcept { return 0u; }
    /** Count physical records touched by this view, including clipped endpoint records. */
    [[nodiscard]] std::size_t run_count() const {
        if (from_ == to_) return 0u;
        const auto first = seek(from_);
        const auto last = seek(to_);
        return last.run_index - first.run_index + static_cast<std::size_t>(last.offset != 0u);
    }
    /** Preserve the compact API's physical-unit convention. */
    [[nodiscard]] std::size_t size() const { return run_count(); }
    /** Count touched run-record bytes, excluding prefix and checkpoint directory. */
    [[nodiscard]] std::size_t byte_size() const { return size() * sizeof(run_type); }
    /** Return a nested logical interval while borrowing the original backing. */
    [[nodiscard]] compact_sequence_range slice(std::size_t from, std::size_t to) const {
        auto view = *this;
        const auto length = bp_size();
        const auto lower = std::min(from, length);
        const auto upper = std::max(lower, std::min(to, length));
        view.from_ = from_ + static_cast<std::uint32_t>(lower);
        view.to_ = from_ + static_cast<std::uint32_t>(upper);
        return view;
    }

    /** Input iterator over clipped canonical runs; it never expands positions. */
    class run_iterator {
        const compact_sequence_range* view_{};
        std::uint32_t index_{};
        std::uint32_t position_{};
        std::uint32_t offset_{};
        run_type current_{};
    public:
        using value_type = run_type;
        using reference = value_type;
        using difference_type = std::ptrdiff_t;
        using iterator_concept = std::input_iterator_tag;
        using iterator_category = std::input_iterator_tag;
        run_iterator() = default;
        /** Bind an endpoint or seek once to the view's first record. */
        explicit run_iterator(const compact_sequence_range* view, bool at_end = false)
            : view_(view), position_(at_end ? view->to_ : view->from_) {
            if (!at_end && position_ != view->to_) {
                const auto found = view->seek(position_);
                index_ = found.run_index;
                offset_ = found.offset;
                current_ = view_->physical_run(index_);
                current_.length = std::min(current_.length - offset_, view_->to_ - position_);
            }
        }
        /** Read the cached record, already clipped to both selected boundaries. */
        [[nodiscard]] value_type operator*() const { return current_; }
        /** Advance by one physical record in constant time. */
        run_iterator& operator++() {
            position_ += current_.length;
            ++index_;
            offset_ = 0u;
            if (position_ != view_->to_) {
                current_ = view_->physical_run(index_);
                current_.length = std::min(current_.length, view_->to_ - position_);
            }
            return *this;
        }
        /** Preserve input-iterator postfix semantics. */
        run_iterator operator++(int) { auto previous = *this; ++*this; return previous; }
        /** Compare logical endpoints, independently of physical sentinel indices. */
        [[nodiscard]] bool operator==(const run_iterator& other) const noexcept {
            return view_ == other.view_ && position_ == other.position_;
        }
    };
    using iterator = run_iterator;
    /** Borrow a canonical run iterable whose lifetime is that of the view. */
    [[nodiscard]] auto runs() const {
        return std::ranges::subrange{run_iterator{this}, run_iterator{this, true}};
    }
    /** Begin the physical run range. */
    [[nodiscard]] run_iterator begin() const { return run_iterator{this}; }
    /** End the physical run range. */
    [[nodiscard]] run_iterator end() const { return run_iterator{this, true}; }
    /** Begin a constant physical run range. */
    [[nodiscard]] run_iterator cbegin() const { return begin(); }
    /** End a constant physical run range. */
    [[nodiscard]] run_iterator cend() const { return end(); }
    /** Access one clipped run by its zero-based run ordinal. */
    [[nodiscard]] run_type run_at(std::size_t index) const {
        const auto first = seek(from_);
        const auto last = seek(to_);
        const auto physical_index = first.run_index + static_cast<std::uint32_t>(index);
        auto run = physical_run(physical_index);
        if (physical_index == last.run_index && last.offset != 0u)
            run.length = last.offset;
        if (physical_index == first.run_index)
            run.length -= first.offset;
        return run;
    }

    /** Input iterator over expanded logical ranks or alphabet symbols. */
    template<iterator_mode Mode> class position_iterator {
        run_iterator run_{};
        run_iterator end_{};
        std::uint32_t remaining_{};
        run_type current_{};
    public:
        using value_type = std::conditional_t<Mode == alphabet_symbol, Alphabet, seqan3::alphabet_rank_t<Alphabet>>;
        using reference = value_type;
        using difference_type = std::ptrdiff_t;
        using iterator_concept = std::input_iterator_tag;
        using iterator_category = std::input_iterator_tag;
        position_iterator() = default;
        /** Bind a selected position stream without allocating expanded storage. */
        explicit position_iterator(run_iterator cursor, run_iterator end)
            : run_(cursor), end_(end) {
            if (run_ != end_) { current_ = *run_; remaining_ = current_.length; }
        }
        /** Read one logical position in the requested existing iterator mode. */
        [[nodiscard]] value_type operator*() const {
            if constexpr (Mode == alphabet_symbol) return current_.operation;
            else return seqan3::to_rank(current_.operation);
        }
        /** Advance one logical position, loading a run only at its boundary. */
        position_iterator& operator++() {
            if (--remaining_ == 0u) {
                ++run_;
                if (run_ != end_) { current_ = *run_; remaining_ = current_.length; }
            }
            return *this;
        }
        /** Preserve input-iterator postfix semantics. */
        position_iterator operator++(int) { auto previous = *this; ++*this; return previous; }
        /** Compare the run and remaining-count state. */
        [[nodiscard]] bool operator==(const position_iterator& other) const noexcept {
            return run_ == other.run_ && remaining_ == other.remaining_;
        }
    };
    /** Adapt a physical run boundary to lazy position iteration in the selected mode. */
    template<iterator_mode Mode>
    [[nodiscard]] auto iter(const run_iterator& cursor) const {
        return position_iterator<Mode>{cursor, end()};
    }
    /** Expand only a requested position window into caller-owned rank scratch. */
    template<bool IsReverseComplement, bool WriteReversed = false>
        requires (!IsReverseComplement && seqan3::alphabet_size<Alphabet> <=
                  std::numeric_limits<std::uint8_t>::max() + std::size_t{1})
    void to_encoded_into(std::span<std::uint8_t> destination, std::size_t from, std::size_t to) const {
        if (from >= to || to > bp_size() || destination.size() < to - from) return;
        const auto selected = slice(from, to);
        const auto length = to - from;
        std::size_t written = 0u;
        for (const auto run : selected.runs()) {
            const auto start = WriteReversed ? length - written - run.length : written;
            std::fill_n(destination.data() + start, run.length,
                        static_cast<std::uint8_t>(seqan3::to_rank(run.operation)));
            written += run.length;
        }
    }
    /** Expand positions lazily in the existing rank/symbol iterator vocabulary. */
    template<iterator_mode Mode> [[nodiscard]] auto iterable() const {
        return std::ranges::subrange{position_iterator<Mode>{begin(), end()}, position_iterator<Mode>{end(), end()}};
    }
    /** Read one positional rank through the indexed run directory. */
    [[nodiscard]] seqan3::alphabet_rank_t<Alphabet> operator[](std::size_t index) const {
        return seqan3::to_rank(physical_run(seek(from_ + static_cast<std::uint32_t>(index)).run_index).operation);
    }
    /** Format run text, never allocating one character per logical position. */
    [[nodiscard]] std::string to_string(bool = false) const {
        std::string text;
        std::array<char, std::numeric_limits<std::uint32_t>::digits10 + 2u> digits{};
        for (const auto run : runs()) {
            const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(), run.length);
            text.append(digits.data(), converted.ptr);
            text.push_back(seqan3::to_char(run.operation));
        }
        return text;
    }
    /** Compare canonical content with no expanded-position scratch. */
    [[nodiscard]] bool operator==(const compact_sequence_range& other) const {
        if (bp_size() != other.bp_size()) return false;
        return std::ranges::equal(runs(), other.runs());
    }
};
}
