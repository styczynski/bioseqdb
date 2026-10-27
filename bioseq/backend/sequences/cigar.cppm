/** @file @brief Canonical CIGAR algorithms, expression contracts and native adapters. */
module;

#include <libassert/assert.hpp>

export module bioseq.sequences.cigar;

import std;
export import bioseq.sequences.cigar_alphabet;
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.storage.lazy_sequence;
import bioseq.storage.coordinates;
import bioseq.storage.size;
import bioseq.common.checked_arithmetic;
import bioseq.common.context;
import bioseq.common.hashing;
import bioseq.simd.sorting;
import bioseq.simd.utils;

export namespace bioseq {

/** Canonical parser output metrics; logical length is independent of text bytes. */
struct cigar_parse_summary {
    std::size_t run_count{};
    storage::sequence_size_t logical_length{};
};

/** Bounded canonical-run destination borrowing exact caller-admitted storage. */
template<cigar_operation_alphabet Alphabet = cigar_symbol>
struct cigar_run_span_sink {
    using value_type = cigar_run_value<Alphabet>;
    std::span<value_type> storage;
    std::size_t used{};

    /** Return whether no positive run has yet been written. */
    [[nodiscard]] bool empty() const noexcept { return used == 0u; }
    /** Return the initialized run count, independently of capacity. */
    [[nodiscard]] std::size_t size() const noexcept { return used; }
    /** Borrow the first caller-owned run slot. */
    [[nodiscard]] value_type* data() const noexcept { return storage.data(); }
    /** Access the last initialized run for canonical coalescing. */
    [[nodiscard]] value_type& back() const noexcept {
        DEBUG_ASSERT(used != 0u, "CIGAR bounded run sink has a trailing run");
        return storage[used - 1u];
    }
    /** Append one run only when its pre-admitted capacity remains available. */
    [[nodiscard]] std::expected<void, cigar_error> try_push_back(value_type run) {
        if (used >= storage.size())
            return std::unexpected(cigar_error{cigar_error_code::output_too_large, used});
        storage[used++] = run;
        return {};
    }
};

/** Append a positive run, coalescing its predecessor in caller-admitted storage. */
template<class Vector>
[[nodiscard]] std::expected<void, cigar_error> append_canonical_run(
        Vector& output, typename Vector::value_type incoming) {
    using run_type = typename Vector::value_type;
    if (incoming.length == 0u)
        return std::unexpected(cigar_error{cigar_error_code::zero_count});
    if (seqan3::to_rank(incoming.operation) >=
            seqan3::alphabet_size<typename run_type::alphabet_type>)
        return std::unexpected(cigar_error{cigar_error_code::invalid_operation});
    if (!output.empty() && output.back().operation == incoming.operation) {
        const auto combined = common::checked_add(output.back().length, incoming.length);
        if (!combined)
            return std::unexpected(cigar_error{cigar_error_code::length_overflow});
        output.back().length = *combined;
    } else {
        if constexpr (requires { output.try_push_back(incoming); }) {
            return output.try_push_back(incoming);
        } else {
            output.push_back(incoming);
        }
    }
    return {};
}

/** Strictly parse complete text and emit canonical retained runs to a checked sink. */
template<cigar_operation_alphabet Alphabet = cigar_symbol, class Sink>
[[nodiscard]] std::expected<cigar_parse_summary, cigar_error> parse_cigar_runs(
        std::string_view text, Sink&& sink,
        cigar_space space = cigar_space::compatibility) {
    if (!valid_cigar_space(space))
        return std::unexpected(cigar_error{cigar_error_code::invalid_encoding});
    using run_type = cigar_run_value<Alphabet>;
    using length_type = typename run_type::length_type;
    std::optional<run_type> pending;
    cigar_parse_summary summary;
    storage::sequence_size_t original_length{};
    std::size_t input_offset{};
    while (input_offset < text.size()) {
        const auto count_offset = input_offset;
        if (text[input_offset] < '1' || text[input_offset] > '9')
            return std::unexpected(cigar_error{
                text[input_offset] == '0' ? cigar_error_code::zero_count
                                          : cigar_error_code::invalid_text,
                input_offset});
        while (input_offset < text.size() && text[input_offset] >= '0'
                && text[input_offset] <= '9') ++input_offset;
        if (input_offset == text.size())
            return std::unexpected(cigar_error{cigar_error_code::invalid_text, input_offset});
        length_type length{};
        const auto parsed = std::from_chars(text.data() + count_offset,
                                           text.data() + input_offset, length);
        if (parsed.ec == std::errc::result_out_of_range)
            return std::unexpected(cigar_error{cigar_error_code::count_overflow, count_offset});
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + input_offset)
            return std::unexpected(cigar_error{cigar_error_code::invalid_text, count_offset});
        const char character = text[input_offset++];
        if (!seqan3::char_is_valid_for<Alphabet>(character))
            return std::unexpected(cigar_error{cigar_error_code::invalid_operation, input_offset - 1u});
        const auto total = common::checked_add(original_length,
                                               static_cast<storage::sequence_size_t>(length));
        if (!total || *total > (std::numeric_limits<length_type>::max)())
            return std::unexpected(cigar_error{cigar_error_code::length_overflow, count_offset});
        original_length = *total;
        const auto operation = Alphabet{}.assign_char(character);
        if (!visible_in_space(operation, space)) continue;
        summary.logical_length += length;
        if (pending && pending->operation == operation) {
            const auto combined = common::checked_add(pending->length, length);
            if (!combined)
                return std::unexpected(cigar_error{cigar_error_code::length_overflow, count_offset});
            pending->length = *combined;
        } else {
            if (pending) {
                auto written = std::invoke(sink, *pending);
                if (!written) return std::unexpected(written.error());
                ++summary.run_count;
            }
            pending = run_type{length, operation};
        }
    }
    if (pending) {
        auto written = std::invoke(sink, *pending);
        if (!written) return std::unexpected(written.error());
        ++summary.run_count;
    }
    return summary;
}

/** Validate and measure canonical text without allocating or materializing runs. */
template<cigar_operation_alphabet Alphabet = cigar_symbol>
[[nodiscard]] std::expected<cigar_parse_summary, cigar_error> measure_cigar_text(
        std::string_view text, cigar_space space = cigar_space::compatibility) {
    return parse_cigar_runs<Alphabet>(text,
        [](cigar_run_value<Alphabet>) -> std::expected<void, cigar_error> { return {}; }, space);
}

/** Compile checked operation identities once; repeated whitelist entries are harmless. */
template<cigar_operation_alphabet Alphabet>
[[nodiscard]] std::expected<cigar_operation_mask<Alphabet>, cigar_error>
compile_cigar_mask(std::span<const std::string_view> operations) {
    cigar_operation_mask<Alphabet> mask;
    for (std::size_t index = 0; index < operations.size(); ++index) {
        const auto text = operations[index];
        if (text.size() != 1u || !seqan3::char_is_valid_for<Alphabet>(text.front()))
            return std::unexpected(cigar_error{cigar_error_code::invalid_operation, index});
        mask.set(seqan3::to_rank(Alphabet{}.assign_char(text.front())));
    }
    return mask;
}

/** External BAM/KSW word codec delegates identity to the alphabet-owned native mapping. */
struct native_cigar_codec {
    using word_type = std::uint32_t;
    static constexpr std::size_t opcode_bits = 4u;
    static constexpr word_type opcode_mask = (word_type{1} << opcode_bits) - 1u;
    static constexpr word_type maximum_count =
        (std::numeric_limits<word_type>::max)() >> opcode_bits;

    /** Map a standard character or registered extension to its external ABI code. */
    [[nodiscard]] static constexpr std::optional<std::uint8_t> opcode(cigar_symbol operation) {
        return cigar_native_opcode(operation);
    }

    /** Decode a checked positive native run, deriving inverse mapping from the alphabet. */
    [[nodiscard]] static constexpr std::expected<cigar_run, cigar_error> decode(word_type word) {
        const auto count = word >> opcode_bits;
        if (count == 0u)
            return std::unexpected(cigar_error{cigar_error_code::zero_count});
        const auto operation = cigar_symbol_from_native_opcode(
            static_cast<std::uint8_t>(word & opcode_mask));
        if (operation) return cigar_run{count, *operation};
        return std::unexpected(cigar_error{cigar_error_code::invalid_operation});
    }
};

/** Append/split a native run without ever overflowing its 28-bit count field. */
template<class Vector>
[[nodiscard]] std::expected<void, cigar_error> push_native_cigar(
        Vector& output, cigar_symbol operation, storage::sequence_size_t length) {
    if (length == 0u) return std::unexpected(cigar_error{cigar_error_code::zero_count});
    const auto opcode = native_cigar_codec::opcode(operation);
    if (!opcode || *opcode > native_cigar_codec::opcode_mask)
        return std::unexpected(cigar_error{cigar_error_code::invalid_operation});
    if (!output.empty() && (output.back() & native_cigar_codec::opcode_mask) == *opcode) {
        const auto old_length = output.back() >> native_cigar_codec::opcode_bits;
        const auto additional = std::min<storage::sequence_size_t>(
            length, native_cigar_codec::maximum_count - old_length);
        output.back() += static_cast<std::uint32_t>(additional)
            << native_cigar_codec::opcode_bits;
        length -= additional;
    }
    while (length != 0u) {
        const auto chunk = std::min<storage::sequence_size_t>(length,
                                                           native_cigar_codec::maximum_count);
        output.push_back((static_cast<std::uint32_t>(chunk)
                          << native_cigar_codec::opcode_bits) | *opcode);
        length -= chunk;
    }
    return {};
}

/** Import native words into caller-admitted canonical run storage. */
template<class Vector>
[[nodiscard]] std::expected<void, cigar_error> decode_native_cigar(
        std::span<const std::uint32_t> input, Vector& output) {
    for (std::size_t index = 0; index < input.size(); ++index) {
        auto decoded = native_cigar_codec::decode(input[index]);
        if (!decoded) { auto error = decoded.error(); error.input_offset = index;
            return std::unexpected(error); }
        auto appended = append_canonical_run(output, *decoded);
        if (!appended) return appended;
    }
    return {};
}

/** Cursor contracts distinguish clean end from recoverable read failure. */
template<class Cursor>
concept cigar_run_cursor = requires(Cursor& cursor) {
    typename Cursor::run_type;
    requires cigar_operation_alphabet<typename Cursor::run_type::alphabet_type>;
    { cursor.next() } -> std::same_as<std::expected<std::optional<typename Cursor::run_type>, cigar_error>>;
};

/** Portable expression capability implemented by the existing PG tree and admitted flat backend. */
template<class Backend>
concept cigar_sequence_backend = requires(Backend& backend,
        typename Backend::sequence_view_type view, typename Backend::sequence_type& owned,
        storage::sequence_size_t from, storage::sequence_size_t to,
        std::span<const typename Backend::run_type> runs) {
    requires cigar_operation_alphabet<typename Backend::alphabet_type>;
    requires std::same_as<typename Backend::run_type, cigar_run_value<typename Backend::alphabet_type>>;
    requires std::same_as<typename Backend::run_cursor_type::run_type, typename Backend::run_type>;
    requires cigar_run_cursor<typename Backend::run_cursor_type>;
    { backend.borrow(owned) } -> std::same_as<typename Backend::sequence_view_type>;
    { backend.release(owned) } noexcept -> std::same_as<void>;
    { backend.length(view) } -> std::same_as<storage::sequence_size_t>;
    { backend.space(view) } -> std::same_as<cigar_space>;
    { backend.runs(view, from, to) } -> std::same_as<typename Backend::run_cursor_type>;
    { backend.with_space(view, cigar_space::compatibility) }
        -> std::same_as<std::expected<typename Backend::sequence_type, cigar_error>>;
    { backend.slice(view, from, to) } -> std::same_as<std::expected<typename Backend::sequence_type, cigar_error>>;
    { backend.concat(view, view) } -> std::same_as<std::expected<typename Backend::sequence_type, cigar_error>>;
    { backend.reverse(view) } -> std::same_as<std::expected<typename Backend::sequence_type, cigar_error>>;
    { backend.from_runs(runs, cigar_space::compatibility) }
        -> std::same_as<std::expected<typename Backend::sequence_type, cigar_error>>;
};

/** Count selected logical positions by adding run weights once. */
template<cigar_sequence_backend Backend>
[[nodiscard]] std::expected<storage::sequence_size_t, cigar_error> cigar_count(
        Backend& backend, typename Backend::sequence_view_type view,
        const cigar_operation_mask<typename Backend::alphabet_type>& selected) {
    storage::sequence_size_t count{};
    auto cursor = backend.runs(view, 0u, backend.length(view));
    for (;;) {
        auto next = cursor.next();
        if (!next) return std::unexpected(next.error());
        if (!*next) return count;
        const auto run = **next;
        if (selected.test(seqan3::to_rank(run.operation))) {
            const auto combined = common::checked_add(count,
                static_cast<storage::sequence_size_t>(run.length));
            if (!combined) return std::unexpected(cigar_error{cigar_error_code::length_overflow});
            count = *combined;
        }
    }
}

/** Count canonical operations rather than physical records or expanded positions. */
template<cigar_sequence_backend Backend>
[[nodiscard]] std::expected<std::size_t, cigar_error> cigar_op_count(
        Backend& backend, typename Backend::sequence_view_type view) {
    std::size_t count{};
    auto cursor = backend.runs(view, 0u, backend.length(view));
    for (;;) {
        auto next = cursor.next();
        if (!next) return std::unexpected(next.error());
        if (!*next) return count;
        ++count;
    }
}

/** Count one biological coordinate axis, rejecting opaque operations before use. */
template<cigar_sequence_backend Backend>
[[nodiscard]] std::expected<storage::sequence_size_t, cigar_error> cigar_extent(
        Backend& backend, typename Backend::sequence_view_type view, cigar_space space) {
    if (!valid_cigar_space(space) || space == cigar_space::compatibility)
        return std::unexpected(cigar_error{cigar_error_code::unsupported_geometry});
    storage::sequence_size_t count{};
    auto cursor = backend.runs(view, 0u, backend.length(view));
    for (;;) {
        auto next = cursor.next();
        if (!next) return std::unexpected(next.error());
        if (!*next) return count;
        const auto run = **next;
        if (!geometry_supported(run.operation))
            return std::unexpected(cigar_error{cigar_error_code::unsupported_geometry});
        if (visible_in_space(run.operation, space)) {
            const auto total = common::checked_add(count,
                static_cast<storage::sequence_size_t>(run.length));
            if (!total) return std::unexpected(cigar_error{cigar_error_code::length_overflow});
            count = *total;
        }
    }
}

/** Resolve canonical run indexing with the existing safe negative-index helper. */
template<cigar_sequence_backend Backend>
[[nodiscard]] std::expected<std::optional<typename Backend::run_type>, cigar_error> cigar_op_at(
        Backend& backend, typename Backend::sequence_view_type view, std::int64_t index) {
    auto count = cigar_op_count(backend, view);
    if (!count) return std::unexpected(count.error());
    const auto normalized = storage::normalize_element_index(index, *count);
    if (!normalized) return std::optional<typename Backend::run_type>{};
    auto cursor = backend.runs(view, 0u, backend.length(view));
    for (std::size_t position = 0;; ++position) {
        auto next = cursor.next();
        if (!next) return std::unexpected(next.error());
        if (!*next || position == *normalized) return *next;
    }
}

/** Translate a whole-run interval into one lazy positional slice. */
template<cigar_sequence_backend Backend>
[[nodiscard]] std::expected<typename Backend::sequence_type, cigar_error> cigar_op_slice(
        Backend& backend, typename Backend::sequence_view_type view,
        std::int64_t from = 0, std::int64_t to = (std::numeric_limits<std::int64_t>::max)()) {
    auto count = cigar_op_count(backend, view);
    if (!count) return std::unexpected(count.error());
    const auto first = storage::normalize_slice_bound(from, *count);
    const auto last = std::max(first, storage::normalize_slice_bound(to, *count));
    storage::sequence_size_t logical_position{}, logical_from{}, logical_to{};
    auto cursor = backend.runs(view, 0u, backend.length(view));
    for (std::size_t index = 0;; ++index) {
        if (index == first) logical_from = logical_position;
        if (index == last) { logical_to = logical_position; break; }
        auto next = cursor.next();
        if (!next) return std::unexpected(next.error());
        if (!*next) { logical_to = logical_position; break; }
        logical_position += (**next).length;
    }
    return backend.slice(view, logical_from, logical_to);
}

/** Exact allocation-free content equality over canonical streams. */
template<cigar_sequence_backend Backend>
[[nodiscard]] std::expected<bool, cigar_error> equal_cigar(
        Backend& backend, typename Backend::sequence_view_type left,
        typename Backend::sequence_view_type right) {
    if (backend.length(left) != backend.length(right)) return false;
    auto left_cursor = backend.runs(left, 0u, backend.length(left));
    auto right_cursor = backend.runs(right, 0u, backend.length(right));
    for (;;) {
        auto left_run = left_cursor.next();
        if (!left_run) return std::unexpected(left_run.error());
        auto right_run = right_cursor.next();
        if (!right_run) return std::unexpected(right_run.error());
        if (!*left_run || !*right_run) return !*left_run && !*right_run;
        if ((**left_run).length != (**right_run).length ||
            (**left_run).operation != (**right_run).operation) return false;
    }
}

/** Measurements collected when a newly loaded canonical run is observed exactly once. */
struct cigar_order_summary {
    storage::sequence_size_t total_matches{};
    storage::sequence_size_t largest_match{};
    std::size_t match_intervals{};
};

/** Fused exact lexical decision and match-profile facts. */
struct cigar_order_scan {
    cigar_order_summary left;
    cigar_order_summary right;
    std::strong_ordering lexical_order{std::strong_ordering::equal};
};

/** Summarize contiguous match roles, independently of physical/canonical run boundaries. */
struct cigar_match_interval_collector {
    cigar_order_summary summary;
    storage::sequence_size_t pending{};

    /** Flush one completed match interval, optionally writing its caller-admitted profile. */
    void flush(std::span<storage::sequence_size_t> profile = {}) {
        if (pending == 0u) return;
        if (!profile.empty()) profile[summary.match_intervals] = pending;
        summary.largest_match = std::max(summary.largest_match, pending);
        ++summary.match_intervals;
        pending = 0u;
    }

    /** Add a run to the profile, merging neighboring matching semantic classes. */
    template<cigar_operation_alphabet Alphabet>
    void observe(cigar_run_value<Alphabet> run,
                 std::span<storage::sequence_size_t> profile = {}) {
        if (is_reported_match(cigar_alphabet_traits<Alphabet>::role(run.operation))) {
            summary.total_matches += run.length;
            pending += run.length;
        } else {
            flush(profile);
        }
    }
};

/** Scan both complete streams once; overlap steps never double-count loaded runs. */
template<cigar_sequence_backend Backend>
[[nodiscard]] std::expected<cigar_order_scan, cigar_error> scan_cigar_order(
        Backend& backend, typename Backend::sequence_view_type left,
        typename Backend::sequence_view_type right) {
    auto left_cursor = backend.runs(left, 0u, backend.length(left));
    auto right_cursor = backend.runs(right, 0u, backend.length(right));
    cigar_match_interval_collector left_profile, right_profile;
    std::optional<typename Backend::run_type> left_run, right_run;
    bool left_end{}, right_end{};
    auto lexical = std::strong_ordering::equal;
    while (!left_end || !right_end) {
        if (!left_run && !left_end) {
            auto next = left_cursor.next();
            if (!next) return std::unexpected(next.error());
            left_run = *next;
            left_end = !left_run;
            if (left_run) left_profile.observe(*left_run);
        }
        if (!right_run && !right_end) {
            auto next = right_cursor.next();
            if (!next) return std::unexpected(next.error());
            right_run = *next;
            right_end = !right_run;
            if (right_run) right_profile.observe(*right_run);
        }
        if (left_run && right_run) {
            if (lexical == 0 && left_run->operation != right_run->operation)
                lexical = seqan3::to_rank(left_run->operation) <=> seqan3::to_rank(right_run->operation);
            const auto shared = std::min(left_run->length, right_run->length);
            left_run->length -= shared;
            right_run->length -= shared;
            if (left_run->length == 0u) left_run.reset();
            if (right_run->length == 0u) right_run.reset();
        } else if (left_end && right_run) {
            if (lexical == 0) lexical = std::strong_ordering::less;
            right_run.reset();
        } else if (right_end && left_run) {
            if (lexical == 0) lexical = std::strong_ordering::greater;
            left_run.reset();
        }
    }
    left_profile.flush();
    right_profile.flush();
    return cigar_order_scan{left_profile.summary, right_profile.summary, lexical};
}

/** Lazy acquisition of the existing admitted allocation context. */
template<class Provider>
concept cigar_scratch_provider = requires(Provider& provider) {
    { provider.acquire() } -> std::same_as<
        std::expected<std::reference_wrapper<context::context>, cigar_error>>;
};

/** Fill a measured match profile using the same collector as the fused first pass. */
template<cigar_sequence_backend Backend>
[[nodiscard]] std::expected<void, cigar_error> collect_cigar_match_profile(
        Backend& backend, typename Backend::sequence_view_type view,
        std::span<storage::sequence_size_t> destination) {
    cigar_match_interval_collector collector;
    auto cursor = backend.runs(view, 0u, backend.length(view));
    for (;;) {
        auto next = cursor.next();
        if (!next) return std::unexpected(next.error());
        if (!*next) break;
        collector.observe(**next, destination);
    }
    collector.flush(destination);
    if (collector.summary.match_intervals != destination.size())
        return std::unexpected(cigar_error{cigar_error_code::invalid_encoding});
    return {};
}

/** Semantic total order: total matches, descending match intervals, then logical ranks. */
template<cigar_sequence_backend Backend, cigar_scratch_provider Provider>
[[nodiscard]] std::expected<std::strong_ordering, cigar_error> compare_cigar(
        Backend& backend, typename Backend::sequence_view_type left,
        typename Backend::sequence_view_type right, Provider& provider) {
    const auto scanned = scan_cigar_order(backend, left, right);
    if (!scanned) return std::unexpected(scanned.error());
    if (scanned->lexical_order == 0) return std::strong_ordering::equal;
    if (scanned->left.total_matches != scanned->right.total_matches)
        return scanned->left.total_matches <=> scanned->right.total_matches;
    if (scanned->left.largest_match != scanned->right.largest_match)
        return scanned->left.largest_match <=> scanned->right.largest_match;
    if (scanned->left.match_intervals <= 1u && scanned->right.match_intervals <= 1u)
        return scanned->lexical_order;
    const auto count = common::checked_add(scanned->left.match_intervals,
                                           scanned->right.match_intervals);
    if (!count || !common::checked_mul(*count, sizeof(storage::sequence_size_t)))
        return std::unexpected(cigar_error{cigar_error_code::output_too_large});
    auto acquired = provider.acquire();
    if (!acquired) return std::unexpected(acquired.error());
    try {
        auto& memory = acquired->get();
        const auto scope = memory.template push<context::arena::scratch>();
        auto profiles = memory.template alloc_scratch_array<storage::sequence_size_t>(*count);
        auto left_profile = profiles.first(scanned->left.match_intervals);
        auto right_profile = profiles.subspan(scanned->left.match_intervals);
        auto left_collected = collect_cigar_match_profile(backend, left, left_profile);
        if (!left_collected) return std::unexpected(left_collected.error());
        auto right_collected = collect_cigar_match_profile(backend, right, right_profile);
        if (!right_collected) return std::unexpected(right_collected.error());
        simd::sorting::sort_unsigned_64_descending(left_profile.data(), left_profile.size());
        simd::sorting::sort_unsigned_64_descending(right_profile.data(), right_profile.size());
        const auto compared = std::lexicographical_compare_three_way(
            left_profile.begin(), left_profile.end(), right_profile.begin(), right_profile.end());
        return compared == 0 ? scanned->lexical_order : compared;
    } catch (const std::bad_alloc&) {
        return std::unexpected(cigar_error{cigar_error_code::allocation_rejected});
    } catch (const std::length_error&) {
        return std::unexpected(cigar_error{cigar_error_code::output_too_large});
    }
}


/** Borrowed prepared sorting key; its context-owned payload and profile outlive comparison. */
template<cigar_operation_alphabet Alphabet>
struct cigar_order_key {
    compact_sequence_range<Alphabet> canonical;
    cigar_order_summary summary;
    std::span<const storage::sequence_size_t> descending_match_lengths;
};

/** Compute a batch operand's match summary in one weighted pass. */
template<cigar_sequence_backend Backend>
[[nodiscard]] std::expected<cigar_order_summary, cigar_error> summarize_cigar_order(
        Backend& backend, typename Backend::sequence_view_type view) {
    cigar_match_interval_collector collector;
    auto cursor = backend.runs(view, 0u, backend.length(view));
    for (;;) {
        auto next = cursor.next();
        if (!next) return std::unexpected(next.error());
        if (!*next) break;
        collector.observe(**next);
    }
    collector.flush();
    return collector.summary;
}

/** Compare immutable prepared keys with no allocation or worker-side PostgreSQL access. */
template<cigar_operation_alphabet Alphabet>
[[nodiscard]] std::strong_ordering compare_cigar_order_keys(
        const cigar_order_key<Alphabet>& left, const cigar_order_key<Alphabet>& right) {
    if (left.summary.total_matches != right.summary.total_matches)
        return left.summary.total_matches <=> right.summary.total_matches;
    if (left.summary.largest_match != right.summary.largest_match)
        return left.summary.largest_match <=> right.summary.largest_match;
    const auto profile_order = std::lexicographical_compare_three_way(
        left.descending_match_lengths.begin(), left.descending_match_lengths.end(),
        right.descending_match_lengths.begin(), right.descending_match_lengths.end());
    if (profile_order != 0) return profile_order;
    auto left_iterator = left.canonical.begin();
    const auto left_end = left.canonical.end();
    auto right_iterator = right.canonical.begin();
    const auto right_end = right.canonical.end();
    std::optional<cigar_run_value<Alphabet>> left_run, right_run;
    for (;;) {
        if (!left_run && left_iterator != left_end) left_run = *left_iterator++;
        if (!right_run && right_iterator != right_end) right_run = *right_iterator++;
        if (!left_run || !right_run)
            return static_cast<bool>(left_run) <=> static_cast<bool>(right_run);
        if (left_run->operation != right_run->operation)
            return seqan3::to_rank(left_run->operation) <=> seqan3::to_rank(right_run->operation);
        const auto shared = std::min(left_run->length, right_run->length);
        left_run->length -= shared;
        right_run->length -= shared;
        if (left_run->length == 0u) left_run.reset();
        if (right_run->length == 0u) right_run.reset();
    }
}

/** Append one uniquely owned piece to an optional aggregate and retire obsolete roots. */
template<class Backend>
[[nodiscard]] std::expected<void, cigar_error> append_cigar_owned_piece(
        Backend& backend, std::optional<typename Backend::sequence_type>& aggregate,
        typename Backend::sequence_type piece) {
    if (!aggregate) { aggregate.emplace(std::move(piece)); return {}; }
    auto joined = backend.concat(backend.borrow(*aggregate), backend.borrow(piece));
    backend.release(piece);
    if (!joined) return std::unexpected(joined.error());
    backend.release(*aggregate);
    aggregate.reset();
    aggregate.emplace(std::move(*joined));
    return {};
}

/** Visit selected canonical runs once, preserving their original absolute coordinates. */
template<cigar_sequence_backend Backend, class Predicate, class Emit>
[[nodiscard]] std::expected<void, cigar_error> visit_selected_cigar_runs(
        Backend& backend, typename Backend::sequence_view_type view,
        Predicate&& keep, Emit&& emit) {
    storage::sequence_size_t position{};
    auto cursor = backend.runs(view, 0u, backend.length(view));
    for (;;) {
        auto next = cursor.next();
        if (!next) return std::unexpected(next.error());
        if (!*next) return {};
        const auto run = **next;
        if (std::invoke(keep, run, position)) {
            auto emitted = std::invoke(emit, run, position);
            if (!emitted) return emitted;
        }
        position += run.length;
    }
}

/** Select runs through a representation accumulator, sharing source intervals for lazy backends. */
template<cigar_sequence_backend Backend, class Predicate>
[[nodiscard]] std::expected<typename Backend::sequence_type, cigar_error> select_cigar_intervals(
        Backend& backend, typename Backend::sequence_view_type view, Predicate&& keep) {
    if constexpr (requires {
        { backend.select_runs(view, std::forward<Predicate>(keep)) }
            -> std::same_as<std::expected<typename Backend::sequence_type, cigar_error>>;
    }) {
        return backend.select_runs(view, std::forward<Predicate>(keep));
    } else {
        std::optional<typename Backend::sequence_type> aggregate;
        std::optional<storage::sequence_size_t> interval_start;
        storage::sequence_size_t interval_end{};
        auto flush_interval = [&backend, view, &aggregate, &interval_start, &interval_end]()
                -> std::expected<void, cigar_error> {
            if (!interval_start) return {};
            auto piece = backend.slice(view, *interval_start, interval_end);
            if (!piece) return std::unexpected(piece.error());
            auto appended = append_cigar_owned_piece(backend, aggregate, std::move(*piece));
            if (!appended) return appended;
            interval_start.reset();
            return {};
        };
        auto emit = [&interval_start, &interval_end, &flush_interval](
                typename Backend::run_type run, storage::sequence_size_t position)
                -> std::expected<void, cigar_error> {
            if (interval_start && position != interval_end) {
                auto flushed = flush_interval();
                if (!flushed) return flushed;
            }
            if (!interval_start) interval_start = position;
            interval_end = position + run.length;
            return {};
        };
        const auto selected = visit_selected_cigar_runs(
            backend, view, std::forward<Predicate>(keep), emit);
        if (!selected) {
            if (aggregate) backend.release(*aggregate);
            return std::unexpected(selected.error());
        }
        const auto flushed = flush_interval();
        if (!flushed) {
            if (aggregate) backend.release(*aggregate);
            return std::unexpected(flushed.error());
        }
        if (aggregate) return std::move(*aggregate);
        return backend.from_runs({}, backend.space(view));
    }
}

/** Keep an identity whitelist without expanding any retained operation run. */
template<cigar_sequence_backend Backend>
[[nodiscard]] std::expected<typename Backend::sequence_type, cigar_error> cigar_filter(
        Backend& backend, typename Backend::sequence_view_type view,
        const cigar_operation_mask<typename Backend::alphabet_type>& selected) {
    return select_cigar_intervals(backend, view,
        [&selected](typename Backend::run_type run, storage::sequence_size_t) {
            return selected.test(seqan3::to_rank(run.operation));
        });
}

/** Filter to a target subalphabet and publish new metadata without rewriting shared sources. */
template<cigar_sequence_backend Backend>
[[nodiscard]] std::expected<typename Backend::sequence_type, cigar_error> cigar_convert(
        Backend& backend, typename Backend::sequence_view_type view, cigar_space space) {
    if (!valid_cigar_space(space))
        return std::unexpected(cigar_error{cigar_error_code::invalid_encoding});
    auto filtered = cigar_filter(backend, view, cigar_space_mask<typename Backend::alphabet_type>(space));
    if (!filtered) return std::unexpected(filtered.error());
    auto bound = backend.with_space(backend.borrow(*filtered), space);
    backend.release(*filtered);
    return bound;
}

/** Replace a half-open position interval using slices and error-atomic concatenation. */
template<cigar_sequence_backend Backend>
[[nodiscard]] std::expected<typename Backend::sequence_type, cigar_error> cigar_replace(
        Backend& backend, typename Backend::sequence_view_type original,
        typename Backend::sequence_view_type replacement, std::int64_t from,
        std::int64_t to) {
    const auto first = storage::normalize_slice_bound(from, backend.length(original));
    const auto last = std::max(first, storage::normalize_slice_bound(to, backend.length(original)));
    const auto unchanged = backend.length(original) - (last - first);
    const auto final_length = common::checked_add(unchanged, backend.length(replacement));
    if (!final_length || *final_length > (std::numeric_limits<typename Backend::run_type::length_type>::max)())
        return std::unexpected(cigar_error{cigar_error_code::length_overflow});
    auto prefix = backend.slice(original, 0u, first);
    if (!prefix) return std::unexpected(prefix.error());
    auto joined = backend.concat(backend.borrow(*prefix), replacement);
    backend.release(*prefix);
    if (!joined) return std::unexpected(joined.error());
    auto postfix = backend.slice(original, last, backend.length(original));
    if (!postfix) { backend.release(*joined); return std::unexpected(postfix.error()); }
    auto result = backend.concat(backend.borrow(*joined), backend.borrow(*postfix));
    backend.release(*joined);
    backend.release(*postfix);
    return result;
}

/** A clipped run with lazy slices covering the rest of the original sequence. */
template<class SequenceHandle, class Run = cigar_run>
struct cigar_match {
    SequenceHandle prefix;
    Run operation;
    SequenceHandle postfix;
};

/** Construct the requested match lazily; no prefix/postfix expansion is performed. */
template<cigar_sequence_backend Backend>
[[nodiscard]] std::expected<cigar_match<typename Backend::sequence_type, typename Backend::run_type>, cigar_error>
make_cigar_match(Backend& backend, typename Backend::sequence_view_type view,
                typename Backend::run_type run, storage::sequence_size_t start) {
    auto prefix = backend.slice(view, 0u, start);
    if (!prefix) return std::unexpected(prefix.error());
    auto postfix = backend.slice(view, start + run.length, backend.length(view));
    if (!postfix) { backend.release(*prefix); return std::unexpected(postfix.error()); }
    return cigar_match<typename Backend::sequence_type, typename Backend::run_type>{
        std::move(*prefix), run, std::move(*postfix)};
}

/** Find the longest clipped run; the earliest source position wins a length tie. */
template<cigar_sequence_backend Backend>
[[nodiscard]] std::expected<std::optional<cigar_match<typename Backend::sequence_type, typename Backend::run_type>>, cigar_error>
cigar_largest(Backend& backend, typename Backend::sequence_view_type view,
              std::int64_t from = 0, std::int64_t to = (std::numeric_limits<std::int64_t>::max)()) {
    const auto first = storage::normalize_slice_bound(from, backend.length(view));
    const auto last = std::max(first, storage::normalize_slice_bound(to, backend.length(view)));
    auto cursor = backend.runs(view, first, last);
    std::optional<typename Backend::run_type> largest;
    storage::sequence_size_t position = first, largest_start{};
    for (;;) {
        auto next = cursor.next();
        if (!next) return std::unexpected(next.error());
        if (!*next) break;
        const auto run = **next;
        if (!largest || run.length > largest->length) { largest = run; largest_start = position; }
        position += run.length;
    }
    using match_type = cigar_match<typename Backend::sequence_type, typename Backend::run_type>;
    if (!largest) return std::optional<match_type>{};
    auto matched = make_cigar_match(backend, view, *largest, largest_start);
    if (!matched) return std::unexpected(matched.error());
    return std::optional<match_type>{std::move(*matched)};
}

/** Collect/sort only match coordinates, then transfer one pair of owned roots at a time to sink. */
template<cigar_sequence_backend Backend, cigar_scratch_provider Provider, class Sink>
[[nodiscard]] std::expected<void, cigar_error> cigar_find(
        Backend& backend, typename Backend::sequence_view_type view,
        typename Backend::alphabet_type operation, std::int64_t from,
        std::int64_t to, Provider& provider, Sink&& sink) {
    using position_type = typename Backend::run_type::length_type;
    constexpr auto position_bits = std::numeric_limits<position_type>::digits;
    constexpr auto position_max = (std::numeric_limits<position_type>::max)();
    static_assert(position_bits * 2u <= std::numeric_limits<std::uint64_t>::digits);
    const auto first = storage::normalize_slice_bound(from, backend.length(view));
    const auto last = std::max(first, storage::normalize_slice_bound(to, backend.length(view)));
    std::size_t match_count{};
    auto cursor = backend.runs(view, first, last);
    for (;;) {
        auto next = cursor.next();
        if (!next) return std::unexpected(next.error());
        if (!*next) break;
        if ((**next).operation == operation) ++match_count;
    }
    if (match_count == 0u) return {};
    if (!common::checked_mul(match_count, sizeof(std::uint64_t)))
        return std::unexpected(cigar_error{cigar_error_code::output_too_large});
    auto acquired = provider.acquire();
    if (!acquired) return std::unexpected(acquired.error());
    try {
        auto& memory = acquired->get();
        const auto scope = memory.template push<context::arena::scratch>();
        auto keys = memory.template alloc_scratch_array<std::uint64_t>(match_count);
        auto second = backend.runs(view, first, last);
        storage::sequence_size_t position = first;
        std::size_t key_index{};
        for (;;) {
            auto next = second.next();
            if (!next) return std::unexpected(next.error());
            if (!*next) break;
            const auto run = **next;
            if (run.operation == operation) {
                if (position > position_max)
                    return std::unexpected(cigar_error{cigar_error_code::length_overflow});
                keys[key_index++] = (static_cast<std::uint64_t>(position_max - run.length)
                                      << position_bits) | position;
            }
            position += run.length;
        }
        simd::sorting::sort_unsigned_64_ascending(keys.data(), keys.size());
        for (const auto key : keys) {
            const auto length = static_cast<position_type>(position_max - (key >> position_bits));
            const auto start = static_cast<position_type>(key & position_max);
            auto match = make_cigar_match(backend, view, typename Backend::run_type{length, operation}, start);
            if (!match) return std::unexpected(match.error());
            // The sink consumes both owning tokens on success and on failure.
            auto emitted = std::invoke(sink, std::move(*match));
            if (!emitted) return std::unexpected(emitted.error());
        }
        return {};
    } catch (const std::bad_alloc&) {
        return std::unexpected(cigar_error{cigar_error_code::allocation_rejected});
    } catch (const std::length_error&) {
        return std::unexpected(cigar_error{cigar_error_code::output_too_large});
    }
}

/** Selectively strip one molecule's unmatched terminal runs while retaining the opposite axis. */
template<cigar_sequence_backend Backend>
[[nodiscard]] std::expected<typename Backend::sequence_type, cigar_error> cigar_strip(
        Backend& backend, typename Backend::sequence_view_type view, bool query_axis, bool left) {
    std::optional<storage::sequence_size_t> first_paired, last_paired_end;
    storage::sequence_size_t position{};
    auto cursor = backend.runs(view, 0u, backend.length(view));
    for (;;) {
        auto next = cursor.next();
        if (!next) return std::unexpected(next.error());
        if (!*next) break;
        const auto run = **next;
        if (!geometry_supported(run.operation))
            return std::unexpected(cigar_error{cigar_error_code::unsupported_geometry});
        if (is_paired(run.operation)) {
            if (!first_paired) first_paired = position;
            last_paired_end = position + run.length;
        }
        position += run.length;
    }
    return select_cigar_intervals(backend, view,
        [query_axis, left, first_paired, last_paired_end](typename Backend::run_type run,
                                                       storage::sequence_size_t start) {
            const bool candidate = query_axis ? is_query_strip_candidate(run.operation)
                                               : is_reference_strip_candidate(run.operation);
            const bool terminal = left ? (!first_paired || start < *first_paired)
                                       : (!last_paired_end || start >= *last_paired_end);
            return !candidate || !terminal;
        });
}

/** Count decimal digits without formatting any operation positions. */
template<std::unsigned_integral Integer>
[[nodiscard]] constexpr std::size_t cigar_decimal_digits(Integer count) noexcept {
    std::size_t digits = 1u;
    while (count >= 10u) { count /= 10u; ++digits; }
    return digits;
}

/** Measure canonical run text for final allocation by the host. */
template<cigar_sequence_backend Backend>
[[nodiscard]] std::expected<std::size_t, cigar_error> measure_cigar_format(
        Backend& backend, typename Backend::sequence_view_type view) {
    std::size_t bytes{};
    auto cursor = backend.runs(view, 0u, backend.length(view));
    for (;;) {
        auto next = cursor.next();
        if (!next) return std::unexpected(next.error());
        if (!*next) return bytes;
        const auto total = common::checked_add(bytes, cigar_decimal_digits((**next).length) + 1u);
        if (!total) return std::unexpected(cigar_error{cigar_error_code::output_too_large});
        bytes = *total;
    }
}

/** Stream decimal counts and alphabet characters directly into a pre-admitted text owner. */
template<cigar_sequence_backend Backend, class TextOwner>
[[nodiscard]] std::expected<void, cigar_error> format_cigar_to(
        Backend& backend, typename Backend::sequence_view_type view, TextOwner& output) {
    using length_type = typename Backend::run_type::length_type;
    std::array<char, std::numeric_limits<length_type>::digits10 + 2u> token{};
    auto cursor = backend.runs(view, 0u, backend.length(view));
    for (;;) {
        auto next = cursor.next();
        if (!next) return std::unexpected(next.error());
        if (!*next) return {};
        const auto run = **next;
        auto converted = std::to_chars(token.data(), token.data() + token.size() - 1u, run.length);
        if (converted.ec != std::errc{})
            return std::unexpected(cigar_error{cigar_error_code::output_too_large});
        *converted.ptr++ = seqan3::to_char(run.operation);
        auto appended = output.append(std::span<const char>{token.data(),
            static_cast<std::size_t>(converted.ptr - token.data())});
        if (!appended) return appended;
    }
}

/** Render an already canonical admitted alignment run range using measured text storage. */
template<std::ranges::forward_range Range>
[[nodiscard]] std::expected<std::string, cigar_error> format_cigar(const Range& runs) {
    using run_type = std::ranges::range_value_t<Range>;
    std::size_t bytes{};
    for (const auto run : runs) {
        if (run.length == 0u) return std::unexpected(cigar_error{cigar_error_code::zero_count});
        const auto total = common::checked_add(bytes, cigar_decimal_digits(run.length) + 1u);
        if (!total) return std::unexpected(cigar_error{cigar_error_code::output_too_large});
        bytes = *total;
    }
    std::string text;
    text.reserve(bytes);
    std::array<char, std::numeric_limits<typename run_type::length_type>::digits10 + 2u> token{};
    for (const auto run : runs) {
        auto converted = std::to_chars(token.data(), token.data() + token.size() - 1u, run.length);
        if (converted.ec != std::errc{}) return std::unexpected(cigar_error{cigar_error_code::output_too_large});
        *converted.ptr++ = seqan3::to_char(run.operation);
        text.append(token.data(), static_cast<std::size_t>(converted.ptr - token.data()));
    }
    return text;
}

/** Emit canonical run labels and signed boundary coordinates through caller-owned sinks. */
template<cigar_sequence_backend Backend, class OperationSink, class PositionSink>
[[nodiscard]] std::expected<void, cigar_error> cigar_to_arrays(
        Backend& backend, typename Backend::sequence_view_type view, std::int64_t initial_position,
        OperationSink&& operations, PositionSink&& positions) {
    const auto extent = backend.length(view);
    if (extent > static_cast<storage::sequence_size_t>((std::numeric_limits<std::int64_t>::max)()) ||
        initial_position > (std::numeric_limits<std::int64_t>::max)() - static_cast<std::int64_t>(extent))
        return std::unexpected(cigar_error{cigar_error_code::length_overflow});
    std::int64_t position = initial_position;
    auto boundary = std::invoke(positions, position);
    if (!boundary) return boundary;
    auto cursor = backend.runs(view, 0u, extent);
    for (;;) {
        auto next = cursor.next();
        if (!next) return std::unexpected(next.error());
        if (!*next) return {};
        const auto run = **next;
        auto operation = std::invoke(operations, seqan3::to_char(run.operation));
        if (!operation) return operation;
        position += run.length;
        boundary = std::invoke(positions, position);
        if (!boundary) return boundary;
    }
}

/** Hash a canonical stream with stable little-endian fields, excluding space and object padding. */
template<cigar_sequence_backend Backend>
[[nodiscard]] std::expected<std::uint64_t, cigar_error> hash_cigar(
        Backend& backend, typename Backend::sequence_view_type view, std::uint64_t seed = 0u) {
    auto hash = common::extend_fnv1a_64(common::hash_combine_64(
        common::k_fnv1a_64_standard_offset_basis, seed), std::string_view{"bioseq.cigar"});
    auto append_integer = [&hash]<std::unsigned_integral Integer>(Integer value) {
        std::array<std::byte, sizeof(Integer)> bytes{};
        for (auto& byte : bytes) { byte = static_cast<std::byte>(value & 0xffu); value >>= 8u; }
        hash = common::extend_fnv1a_64(hash, std::span<const std::byte>{bytes});
    };
    auto cursor = backend.runs(view, 0u, backend.length(view));
    for (;;) {
        auto next = cursor.next();
        if (!next) return std::unexpected(next.error());
        if (!*next) break;
        append_integer((**next).length);
        append_integer(seqan3::to_rank((**next).operation));
    }
    append_integer(backend.length(view));
    return common::splitmix64(hash);
}

/** Existing alignment summary, with N excluded from the historical block-length metric. */
struct cigar_alignment_summary {
    storage::sequence_size_t matches{};
    storage::sequence_size_t block_length{};
};

/** Summarize admitted traceback runs using alphabet-owned roles instead of native opcodes. */
template<std::ranges::input_range Range>
[[nodiscard]] cigar_alignment_summary summarize_alignment_cigar(const Range& runs) {
    cigar_alignment_summary summary;
    for (const auto run : runs) {
        const auto role = cigar_alphabet_traits<typename std::ranges::range_value_t<Range>::alphabet_type>::role(run.operation);
        if (is_reported_match(role)) summary.matches += run.length;
        if (is_paired(role) || role == cigar_role::insertion || role == cigar_role::deletion)
            summary.block_length += run.length;
    }
    return summary;
}


/** Append implicit paired seed span and any residual query/reference gap. */
template<class Vector>
[[nodiscard]] std::expected<void, cigar_error> append_implicit_seed_cigar(
        Vector& output, std::size_t query_length, std::size_t target_length) {
    const auto paired_length = std::min(query_length, target_length);
    const auto remainder = std::max(query_length, target_length) - paired_length;
    using length_type = typename Vector::value_type::length_type;
    if (paired_length > (std::numeric_limits<length_type>::max)() ||
        remainder > (std::numeric_limits<length_type>::max)())
        return std::unexpected(cigar_error{cigar_error_code::length_overflow});
    if (paired_length != 0u) {
        auto paired = append_canonical_run(output, typename Vector::value_type{
            static_cast<length_type>(paired_length), cigar_symbol{}.assign_char('M')});
        if (!paired) return paired;
    }
    if (remainder != 0u)
        return append_canonical_run(output, typename Vector::value_type{
            static_cast<length_type>(remainder),
            cigar_symbol{}.assign_char(query_length > target_length ? 'I' : 'D')});
    return {};
}

/** Compact non-PostgreSQL backend using the caller's already admitted result arena. */
template<cigar_operation_alphabet Alphabet = cigar_symbol>
class flat_cigar_backend {
public:
    using alphabet_type = Alphabet;
    using run_type = cigar_run_value<Alphabet>;
    using sequence_view_type = compact_sequence_range<Alphabet>;

    /** Move-only encoded backing; its context must outlive this owned result. */
    struct sequence_type {
        context::result_vector<std::byte> payload;
        std::uint32_t logical_length{};
        cigar_space declared_space{cigar_space::compatibility};
        /** Adopt an admitted encoded payload without copying it. */
        sequence_type(context::result_vector<std::byte> bytes, std::uint32_t length,
                      cigar_space space)
            : payload(std::move(bytes)), logical_length(length), declared_space(space) {}
        sequence_type(sequence_type&&) noexcept = default;
        sequence_type& operator=(sequence_type&&) noexcept = default;
        sequence_type(const sequence_type&) = delete;
        sequence_type& operator=(const sequence_type&) = delete;
    };

    /** A borrowed run traversal with independent state and no position expansion. */
    class run_cursor_type {
        sequence_view_type view_;
        typename sequence_view_type::run_iterator current_;
        typename sequence_view_type::run_iterator end_;
    public:
        using run_type = cigar_run_value<Alphabet>;
        /** Bind a validated immutable logical window. */
        explicit run_cursor_type(sequence_view_type view)
            : view_(view), current_(view_.begin()), end_(view_.end()) {}
        run_cursor_type(const run_cursor_type&) = delete;
        run_cursor_type(run_cursor_type&&) = delete;
        /** Return one canonical run, with clean end remaining sticky. */
        [[nodiscard]] std::expected<std::optional<run_type>, cigar_error> next() {
            if (current_ == end_) return std::optional<run_type>{};
            const auto run = *current_++;
            return std::optional<run_type>{run};
        }
    };
private:
    context::context& memory_;
public:
    /** Borrow existing admitted arenas, never create another budget service. */
    explicit flat_cigar_backend(context::context& memory) noexcept : memory_(memory) {}

    /** Borrow a validated range while its unique encoded owner remains alive. */
    [[nodiscard]] sequence_view_type borrow(sequence_type& owned) const {
        return sequence_view_type{{owned.payload.data(), owned.payload.size()},
                                  owned.logical_length, owned.declared_space};
    }
    /** Retire owned backing and invalidate the token; repeated release is harmless. */
    void release(sequence_type& owned) noexcept {
        context::result_vector<std::byte> empty(memory_.result_arena());
        owned.payload.swap(empty);
        owned.logical_length = 0u;
    }
    /** Return the selected logical extent. */
    [[nodiscard]] storage::sequence_size_t length(sequence_view_type view) const { return view.bp_size(); }
    /** Return the declared operation subalphabet. */
    [[nodiscard]] cigar_space space(sequence_view_type view) const { return view.space(); }
    /** Create an independent run cursor over a selected logical interval. */
    [[nodiscard]] run_cursor_type runs(sequence_view_type view, storage::sequence_size_t from,
                                       storage::sequence_size_t to) const {
        return run_cursor_type{view.slice(static_cast<std::size_t>(from), static_cast<std::size_t>(to))};
    }
    /** Canonicalize admitted runs and encode a result into the existing result pool. */
    [[nodiscard]] std::expected<sequence_type, cigar_error> from_runs(
            std::span<const run_type> input, cigar_space declared_space) {
        if (!valid_cigar_space(declared_space))
            return std::unexpected(cigar_error{cigar_error_code::invalid_encoding});
        try {
            const auto scope = memory_.template push<context::arena::scratch>();
            auto canonical = memory_.template make_scratch_vector<run_type>(input.size());
            storage::sequence_size_t total{};
            for (const auto run : input) {
                if (!visible_in_space(run.operation, declared_space))
                    return std::unexpected(cigar_error{cigar_error_code::invalid_operation});
                const auto combined = common::checked_add(total,
                    static_cast<storage::sequence_size_t>(run.length));
                if (!combined || *combined > (std::numeric_limits<typename run_type::length_type>::max)())
                    return std::unexpected(cigar_error{cigar_error_code::length_overflow});
                total = *combined;
                auto appended = append_canonical_run(canonical, run);
                if (!appended) return std::unexpected(appended.error());
            }
            if (canonical.size() > (std::numeric_limits<std::uint32_t>::max)())
                return std::unexpected(cigar_error{cigar_error_code::output_too_large});
            const auto bytes = measure_cigar_payload<Alphabet>(static_cast<std::uint32_t>(canonical.size()));
            if (!bytes) return std::unexpected(bytes.error());
            auto payload = memory_.template make_result_vector<std::byte>(*bytes);
            payload.resize(*bytes);
            auto encoded = write_cigar_payload<Alphabet>(std::span<std::byte>{payload.data(), payload.size()},
                std::span<const run_type>{canonical.data(), canonical.size()}, declared_space);
            if (!encoded) return std::unexpected(encoded.error());
            return sequence_type{std::move(payload), *encoded, declared_space};
        } catch (const std::bad_alloc&) {
            return std::unexpected(cigar_error{cigar_error_code::allocation_rejected});
        } catch (const std::length_error&) {
            return std::unexpected(cigar_error{cigar_error_code::output_too_large});
        }
    }
    /** Accumulate retained runs once; eager standalone output is encoded only after selection. */
    template<class Predicate>
    [[nodiscard]] std::expected<sequence_type, cigar_error> select_runs(
            sequence_view_type view, Predicate&& keep) {
        try {
            const auto scope = memory_.template push<context::arena::scratch>();
            // Grow only for retained runs through the existing admitted vector policy.
            auto selected = memory_.template make_scratch_vector<run_type>();
            auto emit = [&selected](run_type run, storage::sequence_size_t)
                    -> std::expected<void, cigar_error> {
                return append_canonical_run(selected, run);
            };
            const auto visited = visit_selected_cigar_runs(
                *this, view, std::forward<Predicate>(keep), emit);
            if (!visited) return std::unexpected(visited.error());
            return from_runs({selected.data(), selected.size()}, space(view));
        } catch (const std::bad_alloc&) {
            return std::unexpected(cigar_error{cigar_error_code::allocation_rejected});
        } catch (const std::length_error&) {
            return std::unexpected(cigar_error{cigar_error_code::output_too_large});
        }
    }
    /** Materialize a selected run window without expanding logical symbols. */
    [[nodiscard]] std::expected<sequence_type, cigar_error> slice(
            sequence_view_type view, storage::sequence_size_t from, storage::sequence_size_t to) {
        const auto selected = view.slice(static_cast<std::size_t>(from), static_cast<std::size_t>(to));
        try {
            const auto scope = memory_.template push<context::arena::scratch>();
            auto selected_runs = memory_.template make_scratch_vector<run_type>(selected.run_count());
            for (const auto run : selected.runs()) selected_runs.push_back(run);
            return from_runs({selected_runs.data(), selected_runs.size()}, selected.space());
        } catch (const std::bad_alloc&) {
            return std::unexpected(cigar_error{cigar_error_code::allocation_rejected});
        }
    }
    /** Join canonical runs in standalone storage; SQL uses the existing lazy tree instead. */
    [[nodiscard]] std::expected<sequence_type, cigar_error> concat(
            sequence_view_type left, sequence_view_type right) {
        const auto total = common::checked_add(length(left), length(right));
        if (!total || *total > (std::numeric_limits<typename run_type::length_type>::max)())
            return std::unexpected(cigar_error{cigar_error_code::length_overflow});
        const auto count = common::checked_add(left.run_count(), right.run_count());
        if (!count) return std::unexpected(cigar_error{cigar_error_code::output_too_large});
        try {
            const auto scope = memory_.template push<context::arena::scratch>();
            auto joined = memory_.template make_scratch_vector<run_type>(*count);
            for (const auto run : left.runs()) {
                auto appended = append_canonical_run(joined, run);
                if (!appended) return std::unexpected(appended.error());
            }
            for (const auto run : right.runs()) {
                auto appended = append_canonical_run(joined, run);
                if (!appended) return std::unexpected(appended.error());
            }
            return from_runs({joined.data(), joined.size()}, left.space() == right.space()
                ? left.space() : cigar_space::compatibility);
        } catch (const std::bad_alloc&) {
            return std::unexpected(cigar_error{cigar_error_code::allocation_rejected});
        }
    }
    /** Reverse run order without expanding their counts. */
    [[nodiscard]] std::expected<sequence_type, cigar_error> reverse(sequence_view_type view) {
        try {
            const auto scope = memory_.template push<context::arena::scratch>();
            auto reversed = memory_.template make_scratch_vector<run_type>(view.run_count());
            for (const auto run : view.runs()) reversed.push_back(run);
            std::reverse(reversed.begin(), reversed.end());
            return from_runs({reversed.data(), reversed.size()}, view.space());
        } catch (const std::bad_alloc&) {
            return std::unexpected(cigar_error{cigar_error_code::allocation_rejected});
        }
    }
    /** Rebind only validated membership; no discarded operations are invented. */
    [[nodiscard]] std::expected<sequence_type, cigar_error> with_space(sequence_view_type view,
                                                                     cigar_space declared_space) {
        if (!valid_cigar_space(declared_space))
            return std::unexpected(cigar_error{cigar_error_code::invalid_encoding});
        auto result = slice(view, 0u, length(view));
        if (!result) return std::unexpected(result.error());
        for (const auto run : view.runs()) {
            if (!visible_in_space(run.operation, declared_space)) {
                release(*result);
                return std::unexpected(cigar_error{cigar_error_code::invalid_operation});
            }
        }
        result->declared_space = declared_space;
        return result;
    }
};
static_assert(cigar_sequence_backend<flat_cigar_backend<cigar_symbol>>);

/** Generic biological-expression capability; semantic CIGAR decisions stay outside the adapter. */
template<class Backend>
concept sequence_projection_backend = requires(Backend& backend,
        typename Backend::sequence_view_type view, typename Backend::sequence_type& owned,
        typename Backend::alphabet_type symbol, storage::sequence_size_t from,
        storage::sequence_size_t to) {
    requires sequence_alphabet<typename Backend::alphabet_type>;
    { backend.borrow(owned) } -> std::same_as<typename Backend::sequence_view_type>;
    { backend.release(owned) } noexcept -> std::same_as<void>;
    { backend.length(view) } -> std::same_as<storage::sequence_size_t>;
    { backend.slice(view, from, to) } -> std::same_as<std::expected<typename Backend::sequence_type, cigar_error>>;
    { backend.concat(view, view) } -> std::same_as<std::expected<typename Backend::sequence_type, cigar_error>>;
    { backend.filled(symbol, to) } -> std::same_as<std::expected<typename Backend::sequence_type, cigar_error>>;
};

/** Alphabet customization for a default unknown symbol, validated before use. */
template<seqan3::alphabet Alphabet>
struct sequence_unknown_symbol_traits {
    /** Return a lossless nucleotide/protein unknown or leave the choice to the caller. */
    [[nodiscard]] static constexpr std::optional<Alphabet> unknown_symbol() {
        std::optional<char> character;
        if constexpr (seqan3::nucleotide_alphabet<Alphabet>) character = 'N';
        else if constexpr (seqan3::aminoacid_alphabet<Alphabet>) character = 'X';
        if (character && seqan3::char_is_valid_for<Alphabet>(*character)) {
            auto symbol = Alphabet{}.assign_char(*character);
            if (seqan3::to_char(symbol) == *character) return symbol;
        }
        return std::nullopt;
    }
};

/** Project known symbols geometrically or conservatively reconstruct the opposite molecule. */
template<cigar_sequence_backend CigarBackend, sequence_projection_backend BioBackend>
[[nodiscard]] std::expected<typename BioBackend::sequence_type, cigar_error> transform_cigar_sequence(
        CigarBackend& cigar_backend, typename CigarBackend::sequence_view_type cigar,
        BioBackend& biological_backend, typename BioBackend::sequence_view_type sequence,
        cigar_space from_space = cigar_space::query, cigar_space to_space = cigar_space::reference,
        bool reconstruct = false, bool assume_m_matches = false,
        std::optional<typename BioBackend::alphabet_type> explicit_filler = std::nullopt) {
    if (!valid_cigar_space(from_space) || !valid_cigar_space(to_space) ||
        from_space == cigar_space::compatibility || to_space == cigar_space::compatibility)
        return std::unexpected(cigar_error{cigar_error_code::unsupported_geometry});
    if (reconstruct && ((from_space != cigar_space::query && from_space != cigar_space::reference) ||
                        (to_space != cigar_space::query && to_space != cigar_space::reference)))
        return std::unexpected(cigar_error{cigar_error_code::unsupported_geometry});
    storage::sequence_size_t source_length{}, destination_length{};
    bool needs_filler{};
    auto cursor = cigar_backend.runs(cigar, 0u, cigar_backend.length(cigar));
    for (;;) {
        auto next = cursor.next();
        if (!next) return std::unexpected(next.error());
        if (!*next) break;
        const auto run = **next;
        const auto role = cigar_alphabet_traits<typename CigarBackend::alphabet_type>::role(run.operation);
        if (!geometry_supported(run.operation))
            return std::unexpected(cigar_error{cigar_error_code::unsupported_geometry});
        const bool source_visible = visible_in_space(run.operation, from_space);
        const bool destination_visible = visible_in_space(run.operation, to_space);
        if (source_visible) source_length += run.length;
        if (destination_visible) destination_length += run.length;
        const bool known = !reconstruct || role == cigar_role::paired_equal ||
            (role == cigar_role::paired_unspecified && assume_m_matches);
        needs_filler |= destination_visible && (!source_visible || !known);
    }
    if (source_length != biological_backend.length(sequence))
        return std::unexpected(cigar_error{cigar_error_code::input_length_mismatch});
    if (from_space == to_space) return biological_backend.slice(sequence, 0u, source_length);
    if (destination_length > (std::numeric_limits<std::uint32_t>::max)())
        return std::unexpected(cigar_error{cigar_error_code::length_overflow});
    auto filler = explicit_filler;
    if (needs_filler && !filler)
        filler = sequence_unknown_symbol_traits<typename BioBackend::alphabet_type>::unknown_symbol();
    if (needs_filler && !filler)
        return std::unexpected(cigar_error{cigar_error_code::unknown_symbol_unavailable});
    if (filler && seqan3::to_rank(*filler) >= seqan3::alphabet_size<typename BioBackend::alphabet_type>)
        return std::unexpected(cigar_error{cigar_error_code::invalid_operation});
    std::optional<typename BioBackend::sequence_type> aggregate;
    storage::sequence_size_t source_position{};
    auto second = cigar_backend.runs(cigar, 0u, cigar_backend.length(cigar));
    for (;;) {
        auto next = second.next();
        if (!next) { if (aggregate) biological_backend.release(*aggregate); return std::unexpected(next.error()); }
        if (!*next) break;
        const auto run = **next;
        const auto role = cigar_alphabet_traits<typename CigarBackend::alphabet_type>::role(run.operation);
        const bool source_visible = visible_in_space(run.operation, from_space);
        const bool destination_visible = visible_in_space(run.operation, to_space);
        if (destination_visible) {
            const bool known = source_visible && (!reconstruct || role == cigar_role::paired_equal ||
                (role == cigar_role::paired_unspecified && assume_m_matches));
            auto piece = known ? biological_backend.slice(sequence, source_position, source_position + run.length)
                               : biological_backend.filled(*filler, run.length);
            if (!piece) {
                if (aggregate) biological_backend.release(*aggregate);
                return std::unexpected(piece.error());
            }
            auto appended = append_cigar_owned_piece(biological_backend, aggregate, std::move(*piece));
            if (!appended) {
                if (aggregate) biological_backend.release(*aggregate);
                return std::unexpected(appended.error());
            }
        }
        if (source_visible) source_position += run.length;
    }
    if (aggregate) return std::move(*aggregate);
    return biological_backend.filled(typename BioBackend::alphabet_type{}, 0u);
}

/** Two directly written final text owners, without a second string-to-PG copy. */
template<class TextOwner>
struct cigar_msa { TextOwner reference; TextOwner query; };

/** Stream a selected biological interval into a text owner using bounded rank reads. */
template<sequence_alphabet Alphabet, class TextOwner>
    requires seqan3::alphabet<Alphabet>
[[nodiscard]] std::expected<void, cigar_error> append_cigar_sequence_text(
        TextOwner& output, storage::lazy_sequence_view<Alphabet> sequence,
        storage::sequence_size_t from, storage::sequence_size_t length) {
    constexpr std::size_t chunk_capacity = 1024u;
    std::array<std::uint8_t, chunk_capacity> ranks{};
    std::array<char, chunk_capacity> characters{};
    for (storage::sequence_size_t consumed = 0u; consumed < length;) {
        const auto count = static_cast<std::size_t>(std::min<storage::sequence_size_t>(length - consumed, chunk_capacity));
        sequence.template copy_ranks<false, false>(std::span<std::uint8_t>{ranks.data(), count},
            static_cast<std::size_t>(from + consumed), static_cast<std::size_t>(from + consumed + count));
        // Lazy readers provide one rank per byte; reuse the shared generic codec
        // without repacking or allocating a complete decoded sequence.
        bioseq::simd::active_target::decode_words_to_ascii_scalar<
            Alphabet, std::uint8_t, std::numeric_limits<std::uint8_t>::digits, 1u>(
                ranks.data(), reinterpret_cast<std::uint8_t*>(characters.data()), count);
        auto appended = output.append(std::span<const char>{characters.data(), count});
        if (!appended) return appended;
        consumed += count;
    }
    return {};
}

/** Render pairwise-visible columns, consuming clips without allocating their columns. */
template<cigar_sequence_backend Backend, sequence_alphabet ReferenceAlphabet,
         sequence_alphabet QueryAlphabet, class TextFactory>
    requires seqan3::alphabet<ReferenceAlphabet> && seqan3::alphabet<QueryAlphabet>
[[nodiscard]] std::expected<cigar_msa<typename TextFactory::text_type>, cigar_error> cigar_to_msa(
        Backend& backend, typename Backend::sequence_view_type cigar,
        storage::lazy_sequence_view<ReferenceAlphabet> reference,
        storage::lazy_sequence_view<QueryAlphabet> query, TextFactory& factory) {
    storage::sequence_size_t reference_length{}, query_length{}, columns{};
    auto measurement = backend.runs(cigar, 0u, backend.length(cigar));
    for (;;) {
        auto next = measurement.next();
        if (!next) return std::unexpected(next.error());
        if (!*next) break;
        const auto run = **next;
        if (!geometry_supported(run.operation))
            return std::unexpected(cigar_error{cigar_error_code::unsupported_geometry});
        if (visible_in_space(run.operation, cigar_space::reference)) reference_length += run.length;
        if (visible_in_space(run.operation, cigar_space::query)) query_length += run.length;
        if (visible_in_space(run.operation, cigar_space::pairwise)) columns += run.length;
    }
    if (reference_length != reference.bp_size() || query_length != query.bp_size())
        return std::unexpected(cigar_error{cigar_error_code::input_length_mismatch});
    auto reference_text = factory.make_text(columns);
    if (!reference_text) return std::unexpected(reference_text.error());
    auto query_text = factory.make_text(columns);
    if (!query_text) return std::unexpected(query_text.error());
    storage::sequence_size_t reference_position{}, query_position{};
    auto cursor = backend.runs(cigar, 0u, backend.length(cigar));
    for (;;) {
        auto next = cursor.next();
        if (!next) return std::unexpected(next.error());
        if (!*next) break;
        const auto run = **next;
        const bool on_reference = visible_in_space(run.operation, cigar_space::reference);
        const bool on_query = visible_in_space(run.operation, cigar_space::query);
        if (visible_in_space(run.operation, cigar_space::pairwise)) {
            auto emitted_reference = on_reference
                ? append_cigar_sequence_text(*reference_text, reference, reference_position, run.length)
                : reference_text->append_repeated('-', run.length);
            if (!emitted_reference) return std::unexpected(emitted_reference.error());
            auto emitted_query = on_query
                ? append_cigar_sequence_text(*query_text, query, query_position, run.length)
                : query_text->append_repeated('-', run.length);
            if (!emitted_query) return std::unexpected(emitted_query.error());
        }
        if (on_reference) reference_position += run.length;
        if (on_query) query_position += run.length;
    }
    auto reference_finished = reference_text->finish();
    if (!reference_finished) return std::unexpected(reference_finished.error());
    auto query_finished = query_text->finish();
    if (!query_finished) return std::unexpected(query_finished.error());
    return cigar_msa<typename TextFactory::text_type>{std::move(*reference_text), std::move(*query_text)};
}

} // namespace bioseq

export namespace bioseq::simd {

/** Internal DP traceback state has a sentinel and is distinct from an output alphabet. */
enum class block_cigar_operation : std::uint8_t {
    sentinel = 0, match = 1, equal = 2, mismatch = 3, insertion = 4, deletion = 5
};

/** Count exact edits using bounded existing SIMD readers for each ambiguous paired run. */
template<bool QueryReversed, sequence_alphabet Alphabet>
[[nodiscard]] std::expected<storage::sequence_size_t, cigar_error> exact_edit_distance_from_packed_cigar(
        storage::lazy_sequence_view<Alphabet> query, storage::lazy_sequence_view<Alphabet> target,
        std::size_t query_start, std::size_t query_end,
        std::size_t target_start, std::size_t target_end,
        std::span<const std::uint32_t> packed_cigar,
        sequence_rank_equality_kernel counter) {
    if (counter == nullptr)
        return std::unexpected(cigar_error{cigar_error_code::invalid_encoding});
    if (query_end < query_start || target_end < target_start)
        return std::unexpected(cigar_error{cigar_error_code::input_length_mismatch});
    if (query_end > query.bp_size() || target_end > target.bp_size())
        return std::unexpected(cigar_error{cigar_error_code::input_length_mismatch});
    const auto query_length = query_end - query_start;
    const auto target_length = target_end - target_start;
    std::size_t query_position{}, target_position{};
    storage::sequence_size_t edits{};
    for (std::size_t run_index = 0u; run_index < packed_cigar.size(); ++run_index) {
        const auto word = packed_cigar[run_index];
        const auto decoded = native_cigar_codec::decode(word);
        if (!decoded) {
            auto error = decoded.error();
            error.input_offset = run_index;
            return std::unexpected(error);
        }
        const auto run = *decoded;
        const auto role = operation_role(run.operation);
        const bool on_query = is_paired(role) || role == cigar_role::insertion;
        const bool on_target = is_paired(role) || role == cigar_role::deletion || role == cigar_role::reference_skip;
        if (!on_query && !on_target)
            return std::unexpected(cigar_error{cigar_error_code::unsupported_geometry, run_index});
        if ((on_query && run.length > query_length - query_position) ||
            (on_target && run.length > target_length - target_position))
            return std::unexpected(cigar_error{cigar_error_code::input_length_mismatch, run_index});
        if (role == cigar_role::paired_unspecified) {
            const auto compared = count_equal_sequence_symbols<QueryReversed>(
                query, target, query_start + query_position, target_start + target_position,
                run.length, counter);
            edits += run.length - compared.equal_symbols;
        } else if (role != cigar_role::paired_equal) {
            edits += run.length;
        }
        if (on_query) query_position += run.length;
        if (on_target) target_position += run.length;
    }
    if (query_position != query_length || target_position != target_length)
        return std::unexpected(cigar_error{cigar_error_code::input_length_mismatch, packed_cigar.size()});
    return edits;
}

/** Count seed substitutions with the shared bounded equality counter plus length difference. */
template<bool QueryReversed, sequence_alphabet Alphabet>
[[nodiscard]] std::expected<storage::sequence_size_t, cigar_error> exact_edit_distance_for_implicit_seed(
        storage::lazy_sequence_view<Alphabet> query, storage::lazy_sequence_view<Alphabet> target,
        std::size_t query_start, std::size_t query_end,
        std::size_t target_start, std::size_t target_end,
        sequence_rank_equality_kernel counter) {
    if (counter == nullptr)
        return std::unexpected(cigar_error{cigar_error_code::invalid_encoding});
    if (query_end < query_start || target_end < target_start)
        return std::unexpected(cigar_error{cigar_error_code::input_length_mismatch});
    if (query_end > query.bp_size() || target_end > target.bp_size())
        return std::unexpected(cigar_error{cigar_error_code::input_length_mismatch});
    const auto query_length = query_end - query_start;
    const auto target_length = target_end - target_start;
    const auto common_length = std::min(query_length, target_length);
    const auto compared = count_equal_sequence_symbols<QueryReversed>(
        query, target, query_start, target_start, common_length, counter);
    return std::max(query_length, target_length) - compared.equal_symbols;
}

} // namespace bioseq::simd
