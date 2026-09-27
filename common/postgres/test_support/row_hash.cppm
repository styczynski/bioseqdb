module;

#include <array>
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

export module bioseq.test.postgres:row_hash;

import bioseq.libpq.client;

export namespace bioseq::postgres_test {

/// @brief A SHA-256 digest with stable lowercase hexadecimal rendering.
struct sha256_digest {
    std::array<std::uint8_t, 32> bytes{};

    /// @brief Returns the 64-character lowercase hexadecimal representation.
    [[nodiscard]] std::string render_hex() const {
        constexpr std::string_view k_digits{"0123456789abcdef"};
        std::string rendered;
        rendered.resize(bytes.size() * 2);
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            rendered[index * 2] = k_digits[bytes[index] >> 4U];
            rendered[index * 2 + 1] =
                k_digits[bytes[index] & 0x0fU];
        }
        return rendered;
    }

    friend bool operator==(const sha256_digest&, const sha256_digest&) = default;
};

/// Incremental SHA-256 using fixed-size buffers for test files and row data.
class sha256_hasher {
public:
    sha256_hasher() = default;

    /**
     * Add bytes to the hash.
     * Calling update after finalize throws a logic_error.
     */
    void update(std::span<const std::byte> bytes) {
        if (finalized_) {
            throw std::logic_error("cannot update a finalized SHA-256 stream");
        }
        total_bytes_ += bytes.size();
        std::size_t offset = 0;
        if (buffer_size_ != 0) {
            const std::size_t copied =
                std::min(buffer_.size() - buffer_size_, bytes.size());
            for (std::size_t index = 0; index < copied; ++index) {
                buffer_[buffer_size_ + index] =
                    static_cast<std::uint8_t>(bytes[index]);
            }
            buffer_size_ += copied;
            offset += copied;
            if (buffer_size_ == buffer_.size()) {
                transform(buffer_);
                buffer_size_ = 0;
            }
        }
        while (offset + buffer_.size() <= bytes.size()) {
            std::array<std::uint8_t, 64> block{};
            for (std::size_t index = 0; index < block.size(); ++index) {
                block[index] = static_cast<std::uint8_t>(bytes[offset + index]);
            }
            transform(block);
            offset += block.size();
        }
        while (offset < bytes.size()) {
            buffer_[buffer_size_++] = static_cast<std::uint8_t>(bytes[offset++]);
        }
    }

    /// @brief Appends the exact bytes in a string view, including embedded NULs.
    void update(std::string_view bytes) {
        update(std::as_bytes(std::span(bytes.data(), bytes.size())));
    }

    /// Finish the hash and return its digest. Later calls return the same digest.
    [[nodiscard]] sha256_digest finalize() {
        if (!finalized_) {
            const std::uint64_t bit_count = total_bytes_ * 8U;
            buffer_[buffer_size_++] = 0x80U;
            if (buffer_size_ > 56) {
                while (buffer_size_ < buffer_.size()) {
                    buffer_[buffer_size_++] = 0;
                }
                transform(buffer_);
                buffer_size_ = 0;
            }
            while (buffer_size_ < 56) {
                buffer_[buffer_size_++] = 0;
            }
            for (std::size_t index = 0; index < 8; ++index) {
                buffer_[56 + index] = static_cast<std::uint8_t>(
                    bit_count >> ((7U - index) * 8U));
            }
            transform(buffer_);
            buffer_size_ = 0;
            for (std::size_t word = 0; word < state_.size(); ++word) {
                for (std::size_t byte = 0; byte < 4; ++byte) {
                    digest_.bytes[word * 4 + byte] = static_cast<std::uint8_t>(
                        state_[word] >> ((3U - byte) * 8U));
                }
            }
            finalized_ = true;
        }
        return digest_;
    }

private:
    static constexpr std::array<std::uint32_t, 64> k_round_constants{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
        0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
        0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
        0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

    /// @brief Applies one SHA-256 compression block.
    void transform(const std::array<std::uint8_t, 64>& block) noexcept {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            words[index] =
                (static_cast<std::uint32_t>(block[index * 4]) << 24U)
                | (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16U)
                | (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8U)
                | static_cast<std::uint32_t>(block[index * 4 + 3]);
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const std::uint32_t schedule_sigma_zero =
                std::rotr(words[index - 15], 7)
                ^ std::rotr(words[index - 15], 18)
                ^ (words[index - 15] >> 3U);
            const std::uint32_t schedule_sigma_one =
                std::rotr(words[index - 2], 17)
                ^ std::rotr(words[index - 2], 19)
                ^ (words[index - 2] >> 10U);
            words[index] = words[index - 16] + schedule_sigma_zero
                + words[index - 7] + schedule_sigma_one;
        }

        std::uint32_t working_a = state_[0];
        std::uint32_t working_b = state_[1];
        std::uint32_t working_c = state_[2];
        std::uint32_t working_d = state_[3];
        std::uint32_t working_e = state_[4];
        std::uint32_t working_f = state_[5];
        std::uint32_t working_g = state_[6];
        std::uint32_t working_h = state_[7];
        for (std::size_t index = 0; index < words.size(); ++index) {
            const std::uint32_t round_sigma_one =
                std::rotr(working_e, 6)
                ^ std::rotr(working_e, 11)
                ^ std::rotr(working_e, 25);
            const std::uint32_t choice =
                (working_e & working_f) ^ (~working_e & working_g);
            const std::uint32_t first_temporary_word =
                working_h + round_sigma_one + choice
                + k_round_constants[index] + words[index];
            const std::uint32_t round_sigma_zero =
                std::rotr(working_a, 2)
                ^ std::rotr(working_a, 13)
                ^ std::rotr(working_a, 22);
            const std::uint32_t majority =
                (working_a & working_b)
                ^ (working_a & working_c)
                ^ (working_b & working_c);
            const std::uint32_t second_temporary_word =
                round_sigma_zero + majority;
            working_h = working_g;
            working_g = working_f;
            working_f = working_e;
            working_e = working_d + first_temporary_word;
            working_d = working_c;
            working_c = working_b;
            working_b = working_a;
            working_a = first_temporary_word + second_temporary_word;
        }
        state_[0] += working_a;
        state_[1] += working_b;
        state_[2] += working_c;
        state_[3] += working_d;
        state_[4] += working_e;
        state_[5] += working_f;
        state_[6] += working_g;
        state_[7] += working_h;
    }

    std::array<std::uint32_t, 8> state_{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffer_size_{};
    std::uint64_t total_bytes_{};
    sha256_digest digest_{};
    bool finalized_{};
};

/// @brief Hashes an in-memory byte string with SHA-256.
[[nodiscard]] inline sha256_digest hash_sha256_bytes(std::string_view bytes) {
    sha256_hasher hasher;
    hasher.update(bytes);
    return hasher.finalize();
}

/// Hash a file in chunks without loading the whole file into memory
[[nodiscard]] inline sha256_digest hash_sha256_file(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open artifact for SHA-256: "
                                 + path.string());
    }
    sha256_hasher hasher;
    std::array<char, 64 * 1024> chunk{};
    while (input) {
        input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto read = input.gcount();
        if (read > 0) {
            hasher.update(std::string_view(
                chunk.data(), static_cast<std::size_t>(read)));
        }
    }
    if (!input.eof()) {
        throw std::runtime_error("failed while hashing artifact: "
                                 + path.string());
    }
    return hasher.finalize();
}

/// @brief Non-owning view of one nullable libpq text-format field.
struct result_text_cell {
    bool is_null{};
    std::string_view bytes;
};

/**
 * Hash ordered query results using the BIOSEQ_CACHE_ROWS_V1 byte format.
 * Include row positions, column counts and cell lengths so boundaries remain
 * unambiguous. Separate NULL from empty text with a marker  for each cell.
 */
class ordered_result_hasher {
public:
    ordered_result_hasher() {
        constexpr std::array<char, 21> k_header{
            'B', 'I', 'O', 'S', 'E', 'Q', '_', 'C', 'A', 'C', 'H',
            'E', '_', 'R', 'O', 'W', 'S', '_', 'V', '1', '\0'};
        hasher_.update(std::string_view(k_header.data(), k_header.size()));
    }

    /// Hash the next row, preserving its position, NULL flags and cell boundaries.
    void append_row(std::span<const result_text_cell> columns) {
        if (finalized_) {
            throw std::logic_error("cannot append to a finalized row hash");
        }
        append_u64(row_count_);
        if (columns.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::overflow_error("row column count exceeds uint32");
        }
        append_u32(static_cast<std::uint32_t>(columns.size()));
        for (const result_text_cell& column : columns) {
            if (column.is_null) {
                append_byte(0x00U);
                continue;
            }
            append_byte(0x01U);
            append_u64(column.bytes.size());
            hasher_.update(column.bytes);
        }
        ++row_count_;
    }

    /// Read and hash one row from an owned libpq result.
    void append_result_row(
        const bioseq::libpq::command_result& result,
        std::size_t row) {
        if (row >= result.row_count()) {
            throw std::out_of_range("row hash row index is out of range");
        }
        std::vector<result_text_cell> columns;
        columns.reserve(static_cast<std::size_t>(result.column_count()));
        for (std::size_t column = 0;
             column < result.column_count(); ++column) {
            const bool is_null = result.cell_is_null(row, column);
            columns.push_back(result_text_cell{
                .is_null = is_null,
                .bytes = is_null ? std::string_view{} : result.text_value(row, column)});
        }
        append_row(columns);
    }

    /// Finish the row stream with its end marker and row count, then return, the digest.
    [[nodiscard]] sha256_digest finalize() {
        if (!finalized_) {
            append_byte(0xffU);
            append_u64(row_count_);
            digest_ = hasher_.finalize();
            finalized_ = true;
        }
        return digest_;
    }

    /// Return how many rows have been added.
    [[nodiscard]] std::uint64_t row_count() const noexcept { return row_count_; }

private:
    /// @brief Appends one raw byte to the SHA stream.
    void append_byte(std::uint8_t value) {
        const std::array<std::byte, 1> encoded{static_cast<std::byte>(value)};
        hasher_.update(encoded);
    }

    /// @brief Appends an unsigned 32-bit integer in network byte order.
    void append_u32(std::uint32_t value) {
        std::array<std::byte, 4> encoded{};
        for (std::size_t index = 0; index < encoded.size(); ++index) {
            encoded[index] = static_cast<std::byte>(
                value >> ((3U - index) * 8U));
        }
        hasher_.update(encoded);
    }

    /// @brief Appends an unsigned 64-bit integer in network byte order.
    void append_u64(std::uint64_t value) {
        std::array<std::byte, 8> encoded{};
        for (std::size_t index = 0; index < encoded.size(); ++index) {
            encoded[index] = static_cast<std::byte>(
                value >> ((7U - index) * 8U));
        }
        hasher_.update(encoded);
    }

    sha256_hasher hasher_;
    std::uint64_t row_count_{};
    sha256_digest digest_{};
    bool finalized_{};
};

/// Hash all rows of a libpq result in their current order using BIOSEQ_CACHE_ROWS_V1.
[[nodiscard]] inline sha256_digest hash_ordered_result_rows(
    const bioseq::libpq::command_result& result) {
    ordered_result_hasher hasher;
    for (std::size_t row = 0; row < result.row_count(); ++row) {
        hasher.append_result_row(result, row);
    }
    return hasher.finalize();
}

}  // namespace bioseq::postgres_test
