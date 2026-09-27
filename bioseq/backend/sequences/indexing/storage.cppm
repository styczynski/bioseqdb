/**
 * @file storage.cppm
 * @brief Storage interfaces and checksums shared by sequence indexes.
 *
 * Each index layout requires different read and write methods. The concepts
 * below check  those methods without depending on PostgreSQL.
 */
module;
#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
export module bioseq.indexing.storage;
import bioseq.indexing.type;
import bioseq.parallelism.memory_admission;
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::indexing {

/// Generator polynomial for persisted CRC-64/ECMA-182 checksums.
inline constexpr std::uint64_t k_crc64_ecma182_polynomial = 0x42F0E1EBA9EA3693ULL;

} // namespace bioseq::indexing

namespace bioseq::indexing::crc64_ecma182_detail {

/** @brief Stores the result of a CRC transformation for each of its 64 input bits. */
using linear_operator = std::array<std::uint64_t, 64>;

/** @brief Builds the byte-at-a-time CRC lookup table. */
[[nodiscard]] consteval std::array<std::uint64_t, 256> make_table() noexcept {
    std::array<std::uint64_t, 256> table{};
    for (std::size_t index = 0; index < table.size(); ++index) {
        std::uint64_t value = static_cast<std::uint64_t>(index) << 56u;
        for (unsigned bit = 0; bit < 8u; ++bit) {
            value = (value & (std::uint64_t{1} << 63u)) != 0
                ? (value << 1u) ^ k_crc64_ecma182_polynomial
                : value << 1u;
        }
        table[index] = value;
    }
    return table;
}

/// @brief Byte-at-a-time CRC lookup table.
inline constexpr auto table = make_table();

/** @brief CRC lookup tables for processing sixteen bytes at a time. */
using crc64_slicing_tables =
    std::array<std::array<std::uint64_t, 256>, 16>;

/**
 * @brief Builds tables that combine sixteen input bytes into one CRC updat.
 * Each table accounts for the number of bytes following its input byte.
 */
[[nodiscard]] consteval crc64_slicing_tables make_slicing_tables() noexcept {
    crc64_slicing_tables tables{};
    tables[0] = table;
    for (std::size_t slice = 1; slice < tables.size(); ++slice) {
        for (std::size_t index = 0; index < tables[slice].size(); ++index) {
            const std::uint64_t previous = tables[slice - 1u][index];
            tables[slice][index] = table[previous >> 56u] ^ (previous << 8u);
        }
    }
    return tables;
}

/// @brief Lookup tables used by the sixteen-byte CRC loop.
inline constexpr auto slicing_tables = make_slicing_tables();

/**
 * @brief Loads an unaligned 64-bit word in big-endian order.
 * @param bytes Address of at least eight bytes
 * @return Decoded word
 */
[[nodiscard]] inline std::uint64_t load_big_endian_u64(
        const std::byte* bytes) noexcept {
    std::uint64_t value;
    std::memcpy(&value, bytes, sizeof(value));
    if constexpr (std::endian::native == std::endian::little) {
        value = std::byteswap(value);
    }
    return value;
}

/**
 * @brief Advances a CRC state by eight bytes represented as one word.
 * @param value Current state XORed with the next input word
 * @return Updated CRC state
 */
[[nodiscard]] inline std::uint64_t slicing_eight(
        std::uint64_t value) noexcept {
    return slicing_tables[7][(value >> 56u) & 0xffu] ^ slicing_tables[6][(value >> 48u) & 0xffu] ^
           slicing_tables[5][(value >> 40u) & 0xffu] ^ slicing_tables[4][(value >> 32u) & 0xffu] ^
           slicing_tables[3][(value >> 24u) & 0xffu] ^ slicing_tables[2][(value >> 16u) & 0xffu] ^
           slicing_tables[1][(value >> 8u) & 0xffu] ^ slicing_tables[0][value & 0xffu];
}

/**
 * @brief Updates the CRC in sixteen-byte groups, then handles any remaining bytes.
 * @param checksum CRC of the preceding bytes.
 * @param bytes Input byte address.
 * @param byte_count Number of input bytes.
 */
[[nodiscard]] inline std::uint64_t update_bulk(
        std::uint64_t checksum,
        const std::byte* bytes,
        std::size_t byte_count) noexcept {
    while (byte_count >= 16u) {
        const std::uint64_t first = checksum
            ^ load_big_endian_u64(bytes);
        const std::uint64_t second = load_big_endian_u64(bytes + 8u);
        checksum = slicing_tables[15][(first >> 56u) & 0xffu] ^
                   slicing_tables[14][(first >> 48u) & 0xffu] ^
                   slicing_tables[13][(first >> 40u) & 0xffu] ^
                   slicing_tables[12][(first >> 32u) & 0xffu] ^
                   slicing_tables[11][(first >> 24u) & 0xffu] ^
                   slicing_tables[10][(first >> 16u) & 0xffu] ^
                   slicing_tables[9][(first >> 8u) & 0xffu] ^ slicing_tables[8][first & 0xffu] ^
                   slicing_tables[7][(second >> 56u) & 0xffu] ^
                   slicing_tables[6][(second >> 48u) & 0xffu] ^
                   slicing_tables[5][(second >> 40u) & 0xffu] ^
                   slicing_tables[4][(second >> 32u) & 0xffu] ^
                   slicing_tables[3][(second >> 24u) & 0xffu] ^
                   slicing_tables[2][(second >> 16u) & 0xffu] ^
                   slicing_tables[1][(second >> 8u) & 0xffu] ^ slicing_tables[0][second & 0xffu];
        bytes += 16u;
        byte_count -= 16u;
    }
    if (byte_count >= 8u) {
        checksum = slicing_eight(
            checksum ^ load_big_endian_u64(bytes));
        bytes += 8u;
        byte_count -= 8u;
    }
    while (byte_count != 0u) {
        const std::uint8_t table_index = static_cast<std::uint8_t>(
            (checksum >> 56u) ^ std::to_integer<std::uint8_t>(*bytes));
        checksum = table[table_index] ^ (checksum << 8u);
        ++bytes;
        --byte_count;
    }
    return checksum;
}

/**
 * @brief Applies a precomputed CRC transformation  to a checksum.
 * The transformation stores the result for each input bit, which can be
 * combined with XOR to obtain the result for the whole checksum.
 */
[[nodiscard]] constexpr std::uint64_t apply_operator(
        const linear_operator& operation,
        std::uint64_t value) noexcept {
    std::uint64_t result = 0;
    while (value != 0u) {
        const std::size_t bit = static_cast<std::size_t>(
            std::countr_zero(value));
        result ^= operation[bit];
        value &= value - 1u;
    }
    return result;
}

/** @brief Combines a CRC transformation with itself so it advances twice as far */
[[nodiscard]] constexpr linear_operator square_operator(
        const linear_operator& operation) noexcept {
    linear_operator squared{};
    for (std::size_t bit = 0; bit < squared.size(); ++bit) {
        squared[bit] = apply_operator(operation, operation[bit]);
    }
    return squared;
}

/**
 * @brief Builds the operator that advances a CRC by one zero byte.
 * @return One-byte advance operator
 */
[[nodiscard]] constexpr linear_operator zero_byte_operator() noexcept {
    linear_operator operation{};
    for (std::size_t bit = 0; bit < operation.size(); ++bit) {
        const std::uint64_t basis = std::uint64_t{1} << bit;
        operation[bit] = (basis & (std::uint64_t{1} << 63u)) != 0
            ? (basis << 1u) ^ k_crc64_ecma182_polynomial
            : basis << 1u;
    }
    operation = square_operator(operation); // two zero bits
    operation = square_operator(operation); // four zero bits
    operation = square_operator(operation); // one zero byte
    return operation;
}

/** @brief Builds the CRC transformation that leaves every input unchanged. */
[[nodiscard]] constexpr linear_operator identity_operator() noexcept {
    linear_operator identity{};
    for (std::size_t bit = 0; bit < identity.size(); ++bit) {
        identity[bit] = std::uint64_t{1} << bit;
    }
    return identity;
}

/**
 * @brief Combines two CRC transformations, applying inner first and outer second.
 */
[[nodiscard]] constexpr linear_operator compose_operators(
        const linear_operator& outer,
        const linear_operator& inner) noexcept {
    linear_operator composition{};
    for (std::size_t bit = 0; bit < composition.size(); ++bit) {
        composition[bit] = apply_operator(outer, inner[bit]);
    }
    return composition;
}

/**
 * @brief Precomputes how appending a fixed number of zero bytes changes a CRC.
 */
[[nodiscard]] consteval linear_operator make_zero_byte_advance_operator(
        std::uint64_t zero_byte_count) noexcept {
    linear_operator result = identity_operator();
    linear_operator power = zero_byte_operator();
    while (zero_byte_count != 0u) {
        if ((zero_byte_count & 1u) != 0u) {
            result = compose_operators(power, result);
        }
        zero_byte_count >>= 1u;
        if (zero_byte_count != 0u) {
            power = square_operator(power);
        }
    }
    return result;
}

// Precompute the 8,064-byte case because full payload rows use this size.
// Combining their checksums then needs one transformation.
/// @brief Payload size of a  full persisted row.
inline constexpr std::uint64_t full_row_byte_count = 8'064u;
/// @brief Precomputed advance operator for a full persisted payload row.
inline constexpr linear_operator full_row_zero_byte_operator =
    make_zero_byte_advance_operator(full_row_byte_count);

/** @brief Computes the CRC after appending zero bytes during compile-time, evaluation. */
[[nodiscard]] constexpr std::uint64_t advance_zero_bytes_constexpr(
        std::uint64_t checksum,
        std::uint64_t zero_byte_count) noexcept {
    linear_operator power = zero_byte_operator();
    while (zero_byte_count != 0u) {
        if ((zero_byte_count & 1u) != 0u) {
            checksum = apply_operator(power, checksum);
        }
        zero_byte_count >>= 1u;
        if (zero_byte_count != 0u) {
            power = square_operator(power);
        }
    }
    return checksum;
}

/// @brief CRC transformations for appending 1, 2, 4, ... zero bytes.
inline const auto zero_byte_operator_powers = [] {
    std::array<linear_operator, 64> powers{};
    powers[0] = zero_byte_operator();
    for (std::size_t power = 1; power < powers.size(); ++power) {
        powers[power] = square_operator(powers[power - 1u]);
    }
    return powers;
}();

} // namespace bioseq::indexing::crc64_ecma182_detail

export namespace bioseq::indexing {

/** @brief Computes CRC-64/ECMA-182 as bytes are added. */
struct crc64_ecma182_accumulator {
    /// @brief CRC of the bytes added so far.
    std::uint64_t value = 0;

    /**
     * @brief Adds one byte to the checksum state.
     * @param byte Byte to append
     */
    constexpr void update_byte(std::uint8_t byte) noexcept {
        const std::uint8_t table_index =
            static_cast<std::uint8_t>((value >> 56u) ^ byte);
        value = crc64_ecma182_detail::table[table_index] ^ (value << 8u);
    }

    /**
     * @brief Adds an unsigned integer in fixed little-endian byte order.
     * @tparam p_unsigned_integer Unsigned integer type
     * @param input Value to append
     */
    template <class p_unsigned_integer>
    requires (std::is_unsigned_v<p_unsigned_integer>)
    constexpr void update_little_endian(p_unsigned_integer input) noexcept {
        for (std::size_t byte = 0; byte < sizeof(p_unsigned_integer); ++byte) {
            update_byte(static_cast<std::uint8_t>(
                input & p_unsigned_integer{0xff}));
            input >>= 8u;
        }
    }

    /**
     * @brief Adds an arbitrary byte span to the checksum state.
     * @param bytes Bytes to append
     */
    constexpr void update(std::span<const std::byte> bytes) noexcept {
        if consteval {
            for (std::byte byte : bytes) {
                update_byte(std::to_integer<std::uint8_t>(byte));
            }
        } else {
            value = crc64_ecma182_detail::update_bulk(
                value, bytes.data(), bytes.size());
        }
    }

    /**
     * @brief Adds the raw bytes of a string view to the checksum state.
     * @param bytes Bytes to append
     */
    constexpr void update(std::string_view bytes) noexcept {
        if consteval {
            for (char byte : bytes) {
                update_byte(static_cast<std::uint8_t>(byte));
            }
        } else {
            value = crc64_ecma182_detail::update_bulk(
                value,
                reinterpret_cast<const std::byte*>(bytes.data()),
                bytes.size());
        }
    }

    /**
     * @brief Returns the current checksum.
     * @return Current CRC state
     */
    [[nodiscard]] constexpr std::uint64_t checksum() const noexcept {
        return value;
    }
};

/**
 * @brief Computes CRC-64/ECMA-182 for one byte span.
 * @param bytes Bytes to checksum
 * @return CRC value
 */
[[nodiscard]] constexpr std::uint64_t crc64_ecma182(
        std::span<const std::byte> bytes) noexcept {
    crc64_ecma182_accumulator crc;
    crc.update(bytes);
    return crc.checksum();
}

/**
 * @brief Computes CRC-64/ECMA-182 for a string view's raw bytes.
 * @param bytes Bytes to checksum
 * @return CRC value
 */
[[nodiscard]] constexpr std::uint64_t crc64_ecma182(
        std::string_view bytes) noexcept {
    crc64_ecma182_accumulator crc;
    crc.update(bytes);
    return crc.checksum();
}

/**
 * @brief Computes the CRC after appending zero_byte_count zero bytes.
 * Precomputed transformations avoid visiting each zero byte.
 */
[[nodiscard]] constexpr std::uint64_t crc64_ecma182_advance_zero_bytes(
        std::uint64_t checksum,
        std::uint64_t zero_byte_count) noexcept {
    if (zero_byte_count == crc64_ecma182_detail::full_row_byte_count) {
        return crc64_ecma182_detail::apply_operator(
            crc64_ecma182_detail::full_row_zero_byte_operator, checksum);
    }

    if consteval {
        return crc64_ecma182_detail::advance_zero_bytes_constexpr(
            checksum, zero_byte_count);
    }

    std::size_t power = 0;
    while (zero_byte_count != 0) {
        if ((zero_byte_count & 1u) != 0) {
            checksum = crc64_ecma182_detail::apply_operator(
                crc64_ecma182_detail::zero_byte_operator_powers[power], checksum);
        }
        zero_byte_count >>= 1u;
        ++power;
    }
    return checksum;
}

/**
 * @brief Computes the CRC of two byte ranges joined together from their CRCs.
 * @param left_checksum CRC of the first range.
 * @param right_checksum CRC of the seond range.
 * @param right_byte_count Number of bytes in the second range.
 */
[[nodiscard]] inline std::uint64_t crc64_ecma182_concatenate(
        std::uint64_t left_checksum,
        std::uint64_t right_checksum,
        std::uint64_t right_byte_count) noexcept {
    return crc64_ecma182_advance_zero_bytes(
        left_checksum, right_byte_count) ^ right_checksum;
}

/**
 * @brief Checksums a row address and payload length in little-endian order.
 *
 * Including the generation, segment, sub-key and chunk number lets readers
 * detect a payload stored under the wrong  row address.
 */
[[nodiscard]] constexpr std::uint64_t crc64_index_chunk_row_prefix(
        std::uint64_t generation_id,
        std::int32_t position_segment,
        index_sub_key_t sub_key,
        std::uint32_t chunk_no,
        std::uint32_t chunk_bytes) noexcept {
    if consteval {
        crc64_ecma182_accumulator crc;
        crc.update_little_endian(generation_id);
        crc.update_little_endian(
            std::bit_cast<std::uint32_t>(position_segment));
        crc.update_little_endian(std::bit_cast<std::uint64_t>(sub_key));
        crc.update_little_endian(chunk_no);
        crc.update_little_endian(chunk_bytes);
        return crc.checksum();
    } else {
        std::array<std::byte, 28> encoded{};
        std::size_t offset = 0u;
        // Encode each address field in the same byte order on every host.
        const auto append_little_endian =
            [&encoded, &offset]<class p_unsigned_integer>(p_unsigned_integer input) {
                for (std::size_t byte = 0u; byte < sizeof(p_unsigned_integer); ++byte) {
                    encoded[offset++] = static_cast<std::byte>(input & 0xffu);
                    input >>= 8u;
                }
            };
        append_little_endian(generation_id);
        append_little_endian(
            std::bit_cast<std::uint32_t>(position_segment));
        append_little_endian(std::bit_cast<std::uint64_t>(sub_key));
        append_little_endian(chunk_no);
        append_little_endian(chunk_bytes);
        crc64_ecma182_accumulator crc;
        crc.update(encoded);
        return crc.checksum();
    }
}

/**
 * @brief Checksums a persisted row address, payload length and payload bytes.
 * chunk_bytes is included as supplied. The caller checks it against blob.size().
 */
[[nodiscard]] constexpr std::uint64_t crc64_index_chunk_row(
        std::uint64_t generation_id,
        std::int32_t position_segment,
        index_sub_key_t sub_key,
        std::uint32_t chunk_no,
        std::uint32_t chunk_bytes,
        std::span<const std::byte> blob) noexcept {
    crc64_ecma182_accumulator crc{crc64_index_chunk_row_prefix(
        generation_id, position_segment, sub_key, chunk_no, chunk_bytes)};
    crc.update(blob);
    return crc.checksum();
}

static_assert(crc64_ecma182("123456789") == 0x6C40DF5F0B497347ULL,
              "CRC-64/ECMA-182 check value must stay pinned");

/** @brief Says whether to write the current tuple batch before or after an append. */
struct index_tuple_batch_decision {
    /// @brief Whether the proposed append respects both limits.
    bool valid = false;
    /// @brief Whether pending tuples must be flushed before the append
    bool flush_before = false;
    /// @brief Whether the append fills either configured limit.
    bool flush_after = false;
};

/**
 * @brief Plans when to write a batch so the next tuple fits its row and byte limits.
 *
 * Flush the existing batch first if needed. Flush the new batch after appending
 * if it reaches either limit. Invalid  sizes or inconsistent counters are rejected.
 */
[[nodiscard]] constexpr index_tuple_batch_decision
plan_index_tuple_batch_append(
        std::uint64_t pending_rows,
        std::uint64_t pending_bytes,
        std::uint64_t tuple_bytes,
        std::uint64_t maximum_rows,
        std::uint64_t maximum_bytes) noexcept {
    if (tuple_bytes == 0 || maximum_rows == 0 || maximum_bytes == 0
        || tuple_bytes > maximum_bytes
        || pending_rows > maximum_rows || pending_bytes > maximum_bytes
        || (pending_rows == 0) != (pending_bytes == 0)) {
        return {};
    }
    const bool flush_before = pending_rows != 0
        && (pending_rows == maximum_rows
            || tuple_bytes > maximum_bytes - pending_bytes);
    const std::uint64_t base_rows = flush_before ? 0 : pending_rows;
    const std::uint64_t base_bytes = flush_before ? 0 : pending_bytes;
    const std::uint64_t rows_after = base_rows + 1u;
    const std::uint64_t bytes_after = base_bytes + tuple_bytes;
    return index_tuple_batch_decision{
        .valid = rows_after <= maximum_rows
            && bytes_after <= maximum_bytes,
        .flush_before = flush_before,
        .flush_after = rows_after == maximum_rows
            || bytes_after == maximum_bytes,
    };
}

/** @brief Checks that storage returned exactly the bytes submitted for publication. */
[[nodiscard]] constexpr bool is_fixed_wire_returned_exactly(
        std::span<const std::byte> proven,
        std::span<const std::byte> returned) noexcept {
    return proven.size() == returned.size()
        && std::ranges::equal(proven, returned);
}

/**
 * @brief Checks the callback used to fill a typed chunk buffer.
 *
 * Storage supplies a zeroed buffer and its capacity in elements. Return the
 * next chunk number to visit, or the current number to advance by one.
 */
template <class p_function, class p_value>
concept index_chunk_writer_fn = requires (
        p_function&& fn,
        std::size_t chunk_no,
        p_value* buf,
        std::size_t capacity_units) {
    { fn(chunk_no, buf, capacity_units) } -> std::same_as<std::size_t>;
};

/** @brief Chooses whether a chunk reae may add rows to the cache. */
enum class index_chunk_cache_policy : std::uint8_t {
    /// @brief Allow checked rows to be added to the cache.
    populate_residency,
    /// @brief Deliver checked rows without adding them to the cache.
    bypass_one_shot,
};

/** @brief A sub-key range [begin, end) in,  one generation and position segment. */
struct index_sub_key_range {
    /// @brief Generation containing the range.
    std::uint64_t generation_id = 0;
    /// @brief Position segment containing the range.
    std::int32_t position_segment = 0;
    /// @brief First included sub-key.
    index_sub_key_t begin = 0;
    /// @brief One-past-last sub-key.
    index_sub_key_t end = 0;
};

/** @brief Expected chunks, byte count and checksum for one stored sub-key. */
struct index_sub_key_request {
    /// @brief Generation containing the requested chunks.
    std::uint64_t generation_id = 0;
    /// @brief Position segment containing the requested chunks.
    std::int32_t position_segment = 0;
    /// @brief Requested logical sub-key.
    index_sub_key_t sub_key = 0;
    /// @brief First expected chunk ordinal.
    std::uint32_t first_chunk_no = 0;
    /// @brief Exact number of expected chunks.
    std::uint32_t expected_chunk_count = 0;
    /// @brief Exact payload bytes across all expected chunks.
    std::uint64_t expected_total_bytes = 0;
    /// @brief CRC of the requested payloads joined in chunk order.
    std::uint64_t expected_checksum = 0;
};

/** @brief Address of one stored index chunk. */
struct index_chunk_address {
    /// @brief Generation containing the chunk.
    std::uint64_t generation_id = 0;
    /// @brief Position segment containing the chunk.
    std::int32_t position_segment = 0;
    /// @brief Logical index sub-key.
    index_sub_key_t sub_key = 0;
    /// @brief Chunk number within the sub-key.
    std::uint32_t chunk_no = 0;

    /** @brief Compares every persisted address component. */
    [[nodiscard]] bool operator==(const index_chunk_address&) const = default;
};

/**
 * @brief Adds a checked row payload to the CRC of preceding payloads.
 *
 * The row checksum includes its address and length Remove that prefix
 * algebraically, then combine the payload CRC without reading its bytes again.
 * @pre validated_row_checksum has been checked against the row bytes.
 */
[[nodiscard]] inline std::uint64_t
crc64_append_validated_index_chunk_payload(
        std::uint64_t current_extent_checksum,
        const index_chunk_address& address,
        std::uint32_t chunk_bytes,
        std::uint64_t validated_row_checksum) noexcept {
    const std::uint64_t prefix_checksum = crc64_index_chunk_row_prefix(
        address.generation_id,
        address.position_segment,
        address.sub_key,
        address.chunk_no,
        chunk_bytes);
    return validated_row_checksum
        ^ crc64_ecma182_advance_zero_bytes(
            current_extent_checksum ^ prefix_checksum, chunk_bytes);
}

/** @brief Counts storage  work and delivered data for one bulk read. */
struct index_chunk_read_stats {
    /// @brief Relation opens performed by the read.
    std::uint64_t relation_opens = 0;
    /// @brief Index scans performed by the read.
    std::uint64_t index_scans = 0;
    /// @brief Sub-key ranges requested within a generation and segment.
    std::uint64_t requested_ranges = 0;
    /// @brief Heap rows visited by the scan.
    std::uint64_t heap_rows_read = 0;
    /// @brief External or compressed PostgreSQL values read.
    std::uint64_t toast_values_read = 0;
    /// @brief Bytes produced when decoding PostgreSQL TOAST values.
    std::uint64_t detoast_bytes = 0;
    /// @brief Bytes copied explicitly by the reader.
    std::uint64_t explicit_copy_bytes = 0;
    /// @brief Distinct sub-keys requested by the caller.
    std::uint64_t wanted_subkeys = 0;
    /// @brief Distinct requested sub-keys found in storage.
    std::uint64_t read_subkeys = 0;
    /// @brief Requested groups of consecutive chunks.
    std::uint64_t wanted_extents = 0;
    /// @brief Chunk groups read with the expected size and checksum.
    std::uint64_t read_extents = 0;
    /// @brief Exact chunks requested across all extents.
    std::uint64_t wanted_chunks = 0;
    /// @brief Unrequested rows skipped while scanning.
    std::uint64_t skipped_rows = 0;
    /// @brief Number of unrequested rows the read plan allows scanning.
    std::uint64_t skip_budget_rows = 0;
    /// @brief Repeated or out-of-order chunk reads and rejected group execution attempts.
    std::uint64_t repeated_chunk_reads = 0;
    /// @brief Validated chunks delivered to the callback.
    std::uint64_t delivered_chunks = 0;
    /// @brief Validated payload bytes delivered to the callback.
    std::uint64_t delivered_bytes = 0;
};

/**
 * @brief Checks the callback that receives a checked chunk addrss and payload.
 * The payload is borrowed for the duration of the callback.
 */
template <class p_function>
concept index_chunk_reader_fn = requires (
        p_function&& sink,
        const index_chunk_address& address,
        const char* payload,
        std::size_t payload_bytes) {
    { sink(address, payload, payload_bytes) } -> std::same_as<void>;
};

/** @brief Callback used to check storage methods that read an unqualified range. */
struct index_chunk_range_probe {
    /// @brief Accepts a  sub-key and borrowed chunk bytes.
    void operator()(index_sub_key_t, const char*, std::size_t) const noexcept {}
};

/** @brief Callback used to check storage methods that read a fully addressed range. */
struct index_qualified_chunk_range_probe {
    /// @brief Accepts a chunk address and borrowed payload bytes.
    void operator()(const index_chunk_address&, const char*, std::size_t) const noexcept {}
};

/** @brief Callback used to check sub-key enumeration methods. */
struct index_sub_key_probe {
    /// @brief Accepts one logical persisted sub-key.
    void operator()(index_sub_key_t) const noexcept {}
};

/** @brief Callback used to check typed chunk writers. */
template <class p_value>
struct index_chunk_writer_probe {
    /// @brief Accepts a typed buffer and returns zero for the signature check.
    std::size_t operator()(
            std::size_t, p_value*, std::size_t) const noexcept {
        return std::size_t{0};
    }
};

/** @brief Callback used to check batches of merged positions. */
struct index_merge_batch_probe {
    /// @brief Accepts a batch of positions in sorted order.
    void operator()(std::span<const std::uint64_t>) const noexcept {}
};

/** @brief Callback used to check merged runs grouped by hash. */
struct index_merge_runs_probe {
    /** @brief Checks that a merged position range reports its size and visits batches. */
    template <class p_positions>
    void operator()(std::uint64_t, p_positions&& positions) const {
        (void)positions.total_count();
        positions.for_each_batch(index_merge_batch_probe{});
    }
};

/** @brief Callback used to check table storage sequence loaders. */
struct index_sequence_loader_probe {
    /**
     * @brief Accepts one lazy sequence and its table-local coordinates.
     * @tparam p_lazy_sequence Lazy sequence type supplied by storage
     */
    template <class p_lazy_sequence>
    void operator()(
            p_lazy_sequence&, std::size_t, std::size_t) const noexcept {}
};

/** @brief Whether storage provides the methods required by the given index  layout */
template <class p_storage, sequence_index_layout p_index_type>
constexpr inline bool is_index_chunked_storage_for_v = false;

// Per-sequence storage addressed by sub-key, and chunk number.
template <class p_storage>
constexpr inline bool is_index_chunked_storage_for_v<
    p_storage, sequence_index_layout::fragment_chunks> =
    requires (p_storage& s, const p_storage& cs, index_sub_key_t sub_key,
              std::size_t chunk_no, std::size_t bytes) {
        { cs.number_of_sub_keys()                 } -> std::convertible_to<std::size_t>;
        { cs.number_of_chunks(sub_key)            } -> std::convertible_to<std::size_t>;
        { cs.chunk_byte_size(sub_key, chunk_no)   } -> std::convertible_to<std::size_t>;
        { s.template read_chunk<std::int64_t>(sub_key, chunk_no) } -> std::same_as<const std::int64_t*>;
        { s.template read_chunk<char>(sub_key, chunk_no)         } -> std::same_as<const char*>;
        { s.resize(sub_key, bytes)                } -> std::same_as<void>;
        { s.resize_chunks(sub_key, std::span<const std::size_t>{}) } -> std::same_as<void>;
        { s.prefetch_chunk(sub_key, chunk_no)     } -> std::same_as<void>;
        // Every fragment store supports reads by sub-key range.
        // Generation readers also provide ranges with complete row addresses.
        { s.read_chunks_in_range(sub_key, sub_key, index_chunk_range_probe{}) } -> std::same_as<void>;
        { cs.for_each_sub_key(index_sub_key_probe{}) } -> std::same_as<void>;
        { s.template write_to_each<std::int64_t>(
              sub_key, index_chunk_writer_probe<std::int64_t>{}) }
            -> std::same_as<void>;
        { s.template write_to_each<char>(
              sub_key, index_chunk_writer_probe<char>{}) }
            -> std::same_as<void>;
        { s.clear_entity()                        } -> std::same_as<void>;
        { s.clear(std::string_view{})             } -> std::same_as<void>;
        // Sorted runs keep their memory reservations until they are merged or released.
        { s.spill_sorted_run(std::span<const std::pair<std::uint64_t,std::uint64_t>>{}) } -> std::convertible_to<std::size_t>;
        { s.spill_sorted_run_owned(std::vector<std::pair<std::uint64_t,std::uint64_t>>{}) } -> std::convertible_to<std::size_t>;
        { s.spill_sorted_run_owned_admitted(
              std::vector<std::pair<std::uint64_t,std::uint64_t>>{},
              std::uint64_t{},
              bioseq::parallelism::memory_permit{}) } -> std::convertible_to<std::size_t>;
        { s.merge_runs(index_merge_runs_probe{}) } -> std::same_as<void>;
        { s.release_resident_runs_noexcept() } noexcept -> std::same_as<void>;
        { s.release_runs_noexcept() } noexcept -> std::same_as<void>;
        { s.release_runs()                        } -> std::same_as<void>;
        { s.run_buffer_bytes()                    } -> std::convertible_to<std::size_t>;
    };

// Per-sequence storage with at most one inline chunk.
template <class p_storage>
constexpr inline bool is_index_chunked_storage_for_v<
    p_storage, sequence_index_layout::sequence_inline> =
    requires (p_storage& s, const p_storage& cs, std::size_t bytes) {
        { cs.number_of_chunks()                   } -> std::convertible_to<std::size_t>;   // zero for an empty payload, otherwise one
        { cs.chunk_byte_size()                    } -> std::convertible_to<std::size_t>;
        { s.template read_chunk<std::int64_t>()   } -> std::same_as<const std::int64_t*>;
        { s.resize(bytes)                         } -> std::same_as<void>;
        { s.template write_to_each<std::int64_t>(
              index_chunk_writer_probe<std::int64_t>{}) }
            -> std::same_as<void>;
        { s.prefetch_chunk()                      } -> std::same_as<void>;
        { s.clear_entity()                        } -> std::same_as<void>;
        { s.clear(std::string_view{})             } -> std::same_as<void>;
    };

// Table storage uses chunk numbers and can isit every sequence in the table.
template <class p_storage>
constexpr inline bool is_index_chunked_storage_for_v<
    p_storage, sequence_index_layout::table_chunks> =
    requires (p_storage& s, const p_storage& cs, std::size_t chunk_no,
              std::size_t bytes) {
        { cs.number_of_chunks()                   } -> std::convertible_to<std::size_t>;
        { cs.chunk_byte_size(chunk_no)            } -> std::convertible_to<std::size_t>;
        { s.template read_chunk<std::int64_t>(chunk_no) } -> std::same_as<const std::int64_t*>;
        { s.resize(bytes)                         } -> std::same_as<void>;
        { s.prefetch_chunk(chunk_no)              } -> std::same_as<void>;
        { s.template write_to_each<std::int64_t>(
              index_chunk_writer_probe<std::int64_t>{}) }
            -> std::same_as<void>;
        { s.template write_to<std::int64_t>(
              chunk_no, index_chunk_writer_probe<std::int64_t>{}) }
            -> std::same_as<void>;
        { s.for_each_sequence(index_sequence_loader_probe{}) }
            -> std::same_as<void>;
        { s.clear(std::string_view{})             } -> std::same_as<void>;
    };

/** @brief Requires the storage methods used by the given index layout. */
template <class p_storage, sequence_index_layout p_index_type>
concept is_index_chunked_storage_for =
    is_index_chunked_storage_for_v<p_storage, p_index_type>;

/** @brief Requires storage methods for  at least one supported index layout. */
template <class p_storage>
concept is_index_chunked_storage =
       is_index_chunked_storage_for<
           p_storage, sequence_index_layout::fragment_chunks>
    || is_index_chunked_storage_for<
           p_storage, sequence_index_layout::sequence_inline>
    || is_index_chunked_storage_for<
           p_storage, sequence_index_layout::table_chunks>;

/**
 * @brief Requires sorted-run storage that can own buffers and their memory reservations.
 * Runs can be merged by hash and released after use.
 */
template <class p_storage>
concept is_external_sort_spill_storage =
    requires (p_storage& s) {
        { s.spill_sorted_run(std::span<const std::pair<std::uint64_t,std::uint64_t>>{}) } -> std::convertible_to<std::size_t>;
        { s.spill_sorted_run_owned(std::vector<std::pair<std::uint64_t,std::uint64_t>>{}) } -> std::convertible_to<std::size_t>;
        { s.spill_sorted_run_owned_admitted(
              std::vector<std::pair<std::uint64_t,std::uint64_t>>{},
              std::uint64_t{},
              bioseq::parallelism::memory_permit{}) } -> std::convertible_to<std::size_t>;
        { s.merge_runs(index_merge_runs_probe{}) } -> std::same_as<void>;
        { s.release_resident_runs_noexcept() } noexcept -> std::same_as<void>;
        { s.release_runs_noexcept() } noexcept -> std::same_as<void>;
        { s.release_runs()     } -> std::same_as<void>;
        { s.run_buffer_bytes() } -> std::convertible_to<std::size_t>;
    };

}  // namespace bioseq::indexing
