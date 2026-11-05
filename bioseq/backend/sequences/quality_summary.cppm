/** @file Exact native-quality arithmetic and the symbol_counts quality wire codec. NO_POSTGRES. */
export module bioseq.sequences.quality_summary;

import std;
import bioseq.sequences;
import bioseq.parallelism.memory_admission;
import bioseq.simd.utils;

export namespace bioseq::sequences {

using quality_sum = __int128;
using quality_unsigned_sum = unsigned __int128;
inline constexpr std::uint64_t quality_max_length = std::numeric_limits<std::int64_t>::max();

enum class quality_error : std::uint8_t {
    invalid_rank, invalid_summary, length_overflow, source_read,
    invalid_payload, unsupported_version, stale_source, memory_limit
};

struct quality_summary {
    std::uint64_t length{};
    quality_sum sum{};
    std::int16_t minimum{};
    std::int16_t maximum{};
    bool operator==(const quality_summary&) const = default;
};

/** Validate before widened arithmetic; even empty inputs must have canonical fields. */
template<quality_alphabet Alphabet>
[[nodiscard]] constexpr bool valid_quality_summary(const quality_summary& value) noexcept {
    using traits = quality_alphabet_traits<Alphabet>;
    if (value.length > quality_max_length) return false;
    if (value.length == 0) return value == quality_summary{};
    return value.minimum >= traits::score_offset && value.maximum <= traits::score_max
        && value.minimum <= value.maximum
        && value.sum >= quality_sum{value.minimum} * value.length
        && value.sum <= quality_sum{value.maximum} * value.length;
}

template<quality_alphabet Alphabet>
[[nodiscard]] constexpr std::expected<quality_summary, quality_error> merge_quality_summaries(
        const quality_summary& left, const quality_summary& right) noexcept {
    if (!valid_quality_summary<Alphabet>(left) || !valid_quality_summary<Alphabet>(right))
        return std::unexpected(quality_error::invalid_summary);
    if (right.length > quality_max_length - left.length)
        return std::unexpected(quality_error::length_overflow);
    if (!left.length) return right;
    if (!right.length) return left;
    return quality_summary{left.length + right.length, left.sum + right.sum,
        std::min(left.minimum, right.minimum), std::max(left.maximum, right.maximum)};
}

template<quality_alphabet Alphabet>
[[nodiscard]] constexpr std::expected<quality_summary, quality_error> summarize_quality_ranks(
        std::span<const std::uint8_t> ranks) noexcept {
    using traits = quality_alphabet_traits<Alphabet>;
    if (ranks.size() > quality_max_length) return std::unexpected(quality_error::length_overflow);
    quality_summary result{};
    if (!std::is_constant_evaluated()) {
        for (std::size_t offset = 0; offset < ranks.size();) {
            const auto count = std::min<std::size_t>(4096, ranks.size() - offset);
            const auto part = bioseq::simd::summarize_native_rank_chunk(ranks.subspan(offset, count), traits::cardinality);
            if (!part.valid) return std::unexpected(quality_error::invalid_rank);
            const auto low = static_cast<std::int16_t>(part.minimum + traits::score_offset);
            const auto high = static_cast<std::int16_t>(part.maximum + traits::score_offset);
            result.minimum = result.length ? std::min(result.minimum, low) : low;
            result.maximum = result.length ? std::max(result.maximum, high) : high;
            result.length += count;
            result.sum += quality_sum{part.sum} + quality_sum{traits::score_offset} * count;
            offset += count;
        }
        return result;
    }
    for (const auto rank : ranks) {
        if (rank >= traits::cardinality) return std::unexpected(quality_error::invalid_rank);
        const auto score = static_cast<std::int16_t>(static_cast<int>(rank) + traits::score_offset);
        result.minimum = result.length ? std::min(result.minimum, score) : score;
        result.maximum = result.length ? std::max(result.maximum, score) : score;
        ++result.length;
        result.sum += score;
    }
    return result;
}

template<quality_alphabet Alphabet>
[[nodiscard]] constexpr std::expected<quality_summary, quality_error> summarize_quality_histogram(
        std::span<const std::int64_t> counts) noexcept {
    using traits = quality_alphabet_traits<Alphabet>;
    constexpr auto slots = std::size_t{1} << std::bit_width(traits::cardinality - 1);
    if (counts.size() != slots) return std::unexpected(quality_error::invalid_summary);
    quality_summary result{};
    for (std::size_t rank = 0; rank < slots; ++rank) {
        if (counts[rank] < 0 || (rank >= traits::cardinality && counts[rank] != 0))
            return std::unexpected(quality_error::invalid_rank);
        const auto count = static_cast<std::uint64_t>(counts[rank]);
        if (count > quality_max_length - result.length)
            return std::unexpected(quality_error::length_overflow);
        if (!count) continue;
        const auto score = static_cast<std::int16_t>(static_cast<int>(rank) + traits::score_offset);
        result.minimum = result.length ? result.minimum : score;
        result.maximum = score;
        result.length += count;
        result.sum += quality_sum{score} * count;
    }
    return result;
}

/** Reduce a checked borrowed window without ownership changes or materialization. */
template<quality_alphabet Alphabet, class Source>
[[nodiscard]] std::expected<quality_summary, quality_error> summarize_quality_source(
        const Source& source, std::uint64_t from, std::uint64_t to) {
    if (from > to || to > source.symbol_count || to - from > quality_max_length)
        return std::unexpected(quality_error::length_overflow);
    std::array<std::uint8_t, 4096> scratch{};
    quality_summary result{};
    while (from < to) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(scratch.size(), to - from));
        const auto ranks = source.read_checked(from, count, scratch);
        if (!ranks) return std::unexpected(quality_error::source_read);
        auto part = summarize_quality_ranks<Alphabet>(*ranks);
        if (!part) return std::unexpected(part.error());
        auto merged = merge_quality_summaries<Alphabet>(result, *part);
        if (!merged) return std::unexpected(merged.error());
        result = *merged;
        from += count;
    }
    return result;
}

/** Memoized full-node reduction of an immutable DAG. Identity can coalesce repeated
 * source windows as well as nodes. Orientation does not affect native quality scores.
 * Explicit frames avoid recursion on deeply nested concatenations. Every retained
 * node/frame is covered by the shared memory-admission controller. */
template<quality_alphabet Alphabet, class Node, class Identity, class Children, class Leaf>
[[nodiscard]] std::expected<quality_summary, quality_error> summarize_quality_dag(
        Node root, Identity identity, Children children, Leaf leaf) {
    if (!root) return quality_summary{};
    using key_type = decltype(identity(root));
    struct frame { Node node; bool reduce; };
    auto permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(4096);
    if (!permit) return std::unexpected(quality_error::memory_limit);
    // Destroy all retained storage before returning its reservation.
    std::map<key_type, quality_summary> memo;
    std::vector<frame> stack;
    stack.push_back({root, false});
    while (!stack.empty()) {
        // std::map node overhead and vector growth are conservatively charged.
        const auto entries = memo.size() + stack.size() + 4;
        constexpr auto charge = 4 * (sizeof(key_type) + sizeof(quality_summary) + sizeof(frame) + 64);
        if (entries > std::numeric_limits<std::uint64_t>::max() / charge
            || !permit->grow_to(std::max<std::uint64_t>(4096, entries * charge)))
            return std::unexpected(quality_error::memory_limit);
        const auto current = stack.back(); stack.pop_back();
        if (!current.node) continue;
        const auto key = identity(current.node);
        if (memo.contains(key)) continue;
        const auto parts = children(current.node);
        if (parts) {
            if (!current.reduce) {
                stack.push_back({current.node, true});
                stack.push_back({parts->second, false}); stack.push_back({parts->first, false});
                continue;
            }
            const auto a = parts->first ? memo.at(identity(parts->first)) : quality_summary{};
            const auto b = parts->second ? memo.at(identity(parts->second)) : quality_summary{};
            const auto combined = merge_quality_summaries<Alphabet>(a, b);
            if (!combined) return combined;
            memo.emplace(key, *combined);
        } else {
            const auto value = leaf(current.node);
            if (!value) return value;
            if (!valid_quality_summary<Alphabet>(*value)) return std::unexpected(quality_error::invalid_summary);
            memo.emplace(key, *value);
        }
    }
    return memo.at(identity(root));
}

/** Plain fixed-size sort key. Division occurs during extraction, never comparison. */
struct quality_mean_key {
    quality_summary summary{};
    quality_sum quotient{};
    std::uint64_t remainder{};
    [[nodiscard]] bool valid() const noexcept { return summary.length != 0; }
};

[[nodiscard]] constexpr quality_mean_key make_quality_mean_key(quality_summary value) noexcept {
    quality_mean_key result{value};
    if (!value.length) return result;
    result.quotient = value.sum / value.length;
    quality_sum remainder = value.sum % value.length;
    if (remainder < 0) { --result.quotient; remainder += value.length; }
    result.remainder = static_cast<std::uint64_t>(remainder);
    return result;
}

/** Nonempty keys only. NULL ordering belongs to the caller. Products stay below 2^126. */
[[nodiscard]] constexpr int compare_quality_means(
        const quality_mean_key& left, const quality_mean_key& right) noexcept {
    if (left.summary.length == right.summary.length)
        return (left.summary.sum > right.summary.sum) - (left.summary.sum < right.summary.sum);
    if (left.quotient != right.quotient)
        return (left.quotient > right.quotient) ? 1 : -1;
    const auto a = quality_unsigned_sum{left.remainder} * right.summary.length;
    const auto b = quality_unsigned_sum{right.remainder} * left.summary.length;
    return (a > b) - (a < b);
}

[[nodiscard]] inline std::string quality_sum_decimal(quality_sum value) {
    const bool negative = value < 0;
    auto magnitude = negative ? quality_unsigned_sum{0} - static_cast<quality_unsigned_sum>(value)
                              : static_cast<quality_unsigned_sum>(value);
    std::string result;
    do { result.push_back(static_cast<char>('0' + magnitude % 10)); magnitude /= 10; } while (magnitude);
    if (negative) result.push_back('-');
    std::reverse(result.begin(), result.end());
    return result;
}

/** Round half away from zero on the 10^-40 grid without scaling the numerator. */
[[nodiscard]] inline std::optional<std::string> quality_mean_decimal(quality_summary value) {
    if (!value.length) return std::nullopt;
    const bool negative = value.sum < 0;
    const auto magnitude = negative ? quality_unsigned_sum{0} - static_cast<quality_unsigned_sum>(value.sum)
                                    : static_cast<quality_unsigned_sum>(value.sum);
    auto integral = magnitude / value.length;
    auto remainder = magnitude % value.length;
    std::array<char, 40> digits{};
    // Each remainder is < 2^63; multiplying by ten cannot overflow 128 bits.
    for (auto& digit : digits) {
        remainder *= 10;
        digit = static_cast<char>('0' + remainder / value.length);
        remainder %= value.length;
    }
    if (remainder * 2 >= value.length) {
        bool carry = true;
        for (std::size_t i = digits.size(); i && carry;) {
            auto& digit = digits[--i];
            if (digit == '9') digit = '0';
            else { ++digit; carry = false; }
        }
        if (carry) ++integral;
    }
    auto result = quality_sum_decimal(static_cast<quality_sum>(integral));
    auto used = digits.size();
    while (used && digits[used - 1] == '0') --used;
    if (used) { result.push_back('.'); result.append(digits.data(), used); }
    if (negative && (integral || used)) result.insert(result.begin(), '-');
    return result;
}

namespace quality_wire {
template<std::unsigned_integral Integer>
constexpr void put(std::span<std::byte> bytes, std::size_t offset, Integer value) noexcept {
    for (std::size_t i = 0; i < sizeof(Integer); ++i)
        bytes[offset + i] = static_cast<std::byte>(value >> (i * 8));
}
template<std::unsigned_integral Integer>
[[nodiscard]] constexpr Integer get(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    Integer result{};
    for (std::size_t i = 0; i < sizeof(Integer); ++i)
        result |= static_cast<Integer>(std::to_integer<unsigned char>(bytes[offset + i])) << (i * 8);
    return result;
}
}

template<quality_alphabet Alphabet>
inline constexpr std::size_t quality_histogram_slots = std::size_t{1}
    << std::bit_width(quality_alphabet_traits<Alphabet>::cardinality - 1u);

template<quality_alphabet Alphabet>
struct quality_index_payload {
    quality_summary summary{};
    std::uint64_t source_epoch{};
    std::array<std::int64_t, quality_histogram_slots<Alphabet>> counts{};
};

/** Explicit little endian; no compiler struct or __int128 representation is serialized. */
template<quality_alphabet Alphabet>
[[nodiscard]] std::expected<std::array<std::byte, 64 + 8 * quality_histogram_slots<Alphabet>>, quality_error>
encode_quality_index(const quality_index_payload<Alphabet>& payload) {
    const auto reduced = summarize_quality_histogram<Alphabet>(payload.counts);
    if (!reduced || *reduced != payload.summary || !payload.source_epoch
        || payload.source_epoch > quality_max_length)
        return std::unexpected(quality_error::invalid_summary);
    using namespace quality_wire;
    using traits = quality_alphabet_traits<Alphabet>;
    std::array<std::byte, 64 + 8 * quality_histogram_slots<Alphabet>> bytes{};
    bytes[0] = std::byte{'Q'}; bytes[1] = std::byte{'S'};
    bytes[2] = std::byte{'C'}; bytes[3] = std::byte{'1'};
    put<std::uint16_t>(bytes, 4, 1); put<std::uint16_t>(bytes, 6, 64);
    put<std::uint8_t>(bytes, 8, supported_storage_alphabets::index_of<Alphabet>());
    put<std::uint8_t>(bytes, 9, traits::legacy);
    put<std::uint8_t>(bytes, 10, std::bit_width(traits::cardinality - 1));
    put<std::uint8_t>(bytes, 11, payload.summary.length != 0);
    put<std::uint16_t>(bytes, 12, payload.counts.size());
    put<std::uint64_t>(bytes, 16, payload.summary.length);
    const auto bits = static_cast<quality_unsigned_sum>(payload.summary.sum);
    put<std::uint64_t>(bytes, 24, static_cast<std::uint64_t>(bits));
    put<std::uint64_t>(bytes, 32, static_cast<std::uint64_t>(bits >> 64));
    put<std::uint16_t>(bytes, 40, static_cast<std::uint16_t>(payload.summary.minimum));
    put<std::uint16_t>(bytes, 42, static_cast<std::uint16_t>(payload.summary.maximum));
    put<std::uint64_t>(bytes, 48, payload.source_epoch);
    for (std::size_t rank = 0; rank < payload.counts.size(); ++rank)
        put<std::uint64_t>(bytes, 64 + rank * 8, payload.counts[rank]);
    return bytes;
}

template<quality_alphabet Alphabet>
[[nodiscard]] std::expected<quality_index_payload<Alphabet>, quality_error> decode_quality_index(
        std::span<const std::byte> bytes, std::uint64_t expected_epoch, std::uint64_t expected_length) {
    using namespace quality_wire;
    using traits = quality_alphabet_traits<Alphabet>;
    if (bytes.size() < 64 || bytes[0] != std::byte{'Q'} || bytes[1] != std::byte{'S'}
        || bytes[2] != std::byte{'C'} || bytes[3] != std::byte{'1'})
        return std::unexpected(quality_error::invalid_payload);
    if (get<std::uint16_t>(bytes, 4) != 1)
        return std::unexpected(quality_error::unsupported_version);
    if (bytes.size() != 64 + 8 * quality_histogram_slots<Alphabet>
        || get<std::uint16_t>(bytes, 6) != 64
        || get<std::uint8_t>(bytes, 8) != supported_storage_alphabets::index_of<Alphabet>()
        || get<std::uint8_t>(bytes, 9) != traits::legacy
        || get<std::uint8_t>(bytes, 10) != std::bit_width(traits::cardinality - 1)
        || get<std::uint8_t>(bytes, 11) > 1
        || get<std::uint16_t>(bytes, 12) != quality_histogram_slots<Alphabet>
        || get<std::uint16_t>(bytes, 14) || get<std::uint32_t>(bytes, 44)
        || get<std::uint64_t>(bytes, 56)) return std::unexpected(quality_error::invalid_payload);
    quality_index_payload<Alphabet> result{};
    result.summary.length = get<std::uint64_t>(bytes, 16);
    const auto bits = quality_unsigned_sum{get<std::uint64_t>(bytes, 24)}
        | (quality_unsigned_sum{get<std::uint64_t>(bytes, 32)} << 64);
    // C++20 integral conversion is congruent modulo 2^128, including negative sums.
    result.summary.sum = static_cast<quality_sum>(bits);
    result.summary.minimum = static_cast<std::int16_t>(get<std::uint16_t>(bytes, 40));
    result.summary.maximum = static_cast<std::int16_t>(get<std::uint16_t>(bytes, 42));
    result.source_epoch = get<std::uint64_t>(bytes, 48);
    if (!result.source_epoch || result.source_epoch > quality_max_length
        || (result.summary.length != 0) != (get<std::uint8_t>(bytes, 11) != 0))
        return std::unexpected(quality_error::invalid_payload);
    for (std::size_t rank = 0; rank < result.counts.size(); ++rank)
        result.counts[rank] = static_cast<std::int64_t>(get<std::uint64_t>(bytes, 64 + rank * 8));
    const auto reduced = summarize_quality_histogram<Alphabet>(result.counts);
    if (!reduced || *reduced != result.summary)
        return std::unexpected(quality_error::invalid_payload);
    if (result.source_epoch != expected_epoch || result.summary.length != expected_length)
        return std::unexpected(quality_error::stale_source);
    return result;
}

} // namespace bioseq::sequences
