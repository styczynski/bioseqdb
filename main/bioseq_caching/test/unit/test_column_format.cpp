#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

import bioseq.cache.core;

using namespace bioseq::cache;

namespace {

struct TestChecksums {
    [[nodiscard]] std::expected<std::uint32_t, ChecksumError> block_crc32c(
        std::span<const std::byte> bytes) const noexcept {
        std::uint32_t hash = 2'166'136'261u;
        for (const std::byte byte : bytes) {
            hash ^= std::to_integer<std::uint8_t>(byte);
            hash *= 16'777'619u;
        }
        return hash;
    }

    [[nodiscard]] std::expected<CacheDigest256, ChecksumError> sha256(
        std::span<const std::byte> bytes) const noexcept {
        std::array<std::uint64_t, 4> lanes{
            0xcbf29ce484222325ULL,
            0x9e3779b97f4a7c15ULL,
            0x6a09e667f3bcc909ULL,
            0xbb67ae8584caa73bULL,
        };
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            const std::uint64_t value =
                std::to_integer<std::uint8_t>(bytes[index]);
            for (std::size_t lane = 0; lane < lanes.size(); ++lane) {
                lanes[lane] ^= value + index + lane * 0x9d;
                lanes[lane] *= 0x100000001b3ULL + lane * 2u;
                lanes[lane] ^= lanes[lane] >> (17u + lane);
            }
        }
        CacheDigest256 digest{};
        for (std::size_t lane = 0; lane < lanes.size(); ++lane) {
            for (std::size_t byte = 0; byte < 8; ++byte) {
                digest.bytes[lane * 8u + byte] = static_cast<std::uint8_t>(
                    lanes[lane] >> (byte * 8u));
            }
        }
        return digest;
    }

    [[nodiscard]] std::expected<CacheDigest256, ChecksumError>
    manifest_sha256(
        std::span<const CacheDigest256> block_digests) const noexcept {
        std::vector<std::byte> bytes;
        bytes.reserve(block_digests.size() * 32u);
        for (const CacheDigest256& digest : block_digests) {
            for (const std::uint8_t byte : digest.bytes) {
                bytes.push_back(static_cast<std::byte>(byte));
            }
        }
        return sha256(bytes);
    }
};

struct UnavailableChecksums {
    [[nodiscard]] std::expected<std::uint32_t, ChecksumError> block_crc32c(
        std::span<const std::byte>) const noexcept {
        return std::unexpected(ChecksumError::Unavailable);
    }

    [[nodiscard]] std::expected<CacheDigest256, ChecksumError> sha256(
        std::span<const std::byte>) const noexcept {
        return std::unexpected(ChecksumError::Unavailable);
    }

    [[nodiscard]] std::expected<CacheDigest256, ChecksumError>
    manifest_sha256(
        std::span<const CacheDigest256>) const noexcept {
        return std::unexpected(ChecksumError::Unavailable);
    }
};

constexpr ColumnStoreIdentity kIdentity{
    .postgres_major = 18,
    .boot_nonce = 0x1234'5678'9abc'def0ULL,
    .database_oid = 42,
    .owner_role_oid = 84,
    .datum_width = sizeof(std::uintptr_t),
};

std::array<ColumnSchema, 3> mixed_schema() {
    return {{
        {
            .name = "accepted",
            .descriptor = {
                .type_oid = 16,
                .typmod = -1,
                .type_length = 1,
                .by_value = 1,
                .type_alignment = 1,
                .encoding = ColumnEncoding::BooleanBits,
                .flags = kColumnFlagNullable,
            },
            .semantic_id = {.low = 1},
        },
        {
            .name = "score",
            .descriptor = {
                .type_oid = 23,
                .typmod = -1,
                .type_length = 4,
                .by_value = 1,
                .type_alignment = 4,
                .encoding = ColumnEncoding::FixedByValue,
                .flags = kColumnFlagNullable,
            },
            .semantic_id = {.low = 2},
        },
        {
            .name = "payload",
            .descriptor = {
                .type_oid = 17,
                .typmod = -1,
                .type_length = -1,
                .by_value = 0,
                .type_alignment = 1,
                .encoding = ColumnEncoding::BinarySend,
                .flags = kColumnFlagNullable,
                .send_function_oid = 1,
                .receive_function_oid = 2,
            },
            .semantic_id = {.low = 3},
        },
    }};
}

template <std::size_t Size>
std::span<const std::byte> bytes_of(
    const std::array<std::byte, Size>& bytes) {
    return bytes;
}

std::span<const std::byte> text_bytes(std::string_view text) {
    return std::as_bytes(std::span<const char>{text.data(), text.size()});
}

void append_mixed_rows(ColumnStoreBuilder& builder) {
    const std::array<std::byte, 1> boolean_true{std::byte{1}};
    const std::array<std::byte, 1> boolean_false{std::byte{0}};
    const std::array<std::byte, 4> score_10{
        std::byte{10}, std::byte{0}, std::byte{0}, std::byte{0}};
    const std::array<std::byte, 4> score_20{
        std::byte{20}, std::byte{0}, std::byte{0}, std::byte{0}};
    const std::array<std::byte, 4> score_30{
        std::byte{30}, std::byte{0}, std::byte{0}, std::byte{0}};

    const std::array<EncodedColumnValue, 3> row0{{
        {.bytes = bytes_of(boolean_true)},
        {.bytes = bytes_of(score_10)},
        {.bytes = text_bytes("alpha")},
    }};
    const std::array<EncodedColumnValue, 3> row1{{
        {.is_null = true},
        {.is_null = true},
        {.bytes = text_bytes("")},
    }};
    const std::array<EncodedColumnValue, 3> row2{{
        {.bytes = bytes_of(boolean_false)},
        {.bytes = bytes_of(score_20)},
        {.is_null = true},
    }};
    const std::array<EncodedColumnValue, 3> row3{{
        {.bytes = bytes_of(boolean_true)},
        {.bytes = bytes_of(score_30)},
        {.bytes = text_bytes("omega")},
    }};

    REQUIRE(builder.append_row(row0).has_value());
    REQUIRE(builder.append_row(row1).has_value());
    REQUIRE(builder.append_row(row2).has_value());
    REQUIRE(builder.append_row(row3).has_value());
}

} // namespace

TEST_CASE("validity bitmaps are canonical at byte boundaries") {
    const std::array<bool, 10> valid{
        true, false, true, true, false, false, true, false, true, false};
    const auto encoded = encode_validity(valid);
    REQUIRE(encoded.size() == 2);
    REQUIRE(encoded[0] == std::byte{0x4d});
    REQUIRE(encoded[1] == std::byte{0x01});
    REQUIRE(unused_tail_bits_are_zero(encoded, valid.size()));
    REQUIRE(decode_validity(encoded, valid.size()).has_value());

    auto corrupt_tail = encoded;
    corrupt_tail[1] |= std::byte{0x80};
    REQUIRE_FALSE(unused_tail_bits_are_zero(corrupt_tail, valid.size()));
    REQUIRE_FALSE(decode_validity(corrupt_tail, valid.size()).has_value());
    REQUIRE_FALSE(decode_validity(encoded, 17).has_value());
}

TEST_CASE("validity bitmap boundary sweep round trips packed booleans") {
    for (const std::size_t size : {
             std::size_t{0},
             std::size_t{1},
             std::size_t{7},
             std::size_t{8},
             std::size_t{9},
             std::size_t{63},
             std::size_t{64},
             std::size_t{65},
             std::size_t{4'096},
         }) {
        std::vector<bool> validity(size);
        for (std::size_t index = 0; index < size; ++index) {
            validity[index] = index % 3u != 1u;
        }
        const auto encoded = encode_validity(validity);
        const auto decoded = decode_validity(encoded, size);
        INFO("bitmap size " << size);
        REQUIRE(decoded.has_value());
        REQUIRE(*decoded == validity);
        REQUIRE(unused_tail_bits_are_zero(encoded, size));
    }
}

TEST_CASE("column store round trips null, boolean, fixed, and binary values") {
    auto schema = mixed_schema();
    ColumnStoreBuilder builder(kIdentity, schema, {.rows = 2, .value_bytes = 1'024});
    append_mixed_rows(builder);
    REQUIRE(builder.row_count() == 4);

    TestChecksums checksums;
    const auto sealed = builder.seal(checksums);
    REQUIRE(sealed.has_value());
    const auto view = validate_column_store(
        *sealed, kIdentity, schema, checksums);
    REQUIRE(view.has_value());
    REQUIRE(view->row_count() == 4);
    REQUIRE(view->column_count() == 3);
    REQUIRE(view->row_group_count() == 2);
    REQUIRE(view->schema() == std::vector<ColumnSchema>(
        schema.begin(), schema.end()));

    const auto bool_true = view->value(0, 0);
    REQUIRE(bool_true.has_value());
    REQUIRE(bool_true->is_boolean);
    REQUIRE(bool_true->boolean_value);
    REQUIRE(view->value(1, 0)->is_null);
    REQUIRE_FALSE(view->value(2, 0)->boolean_value);

    const auto score = view->value(2, 1);
    REQUIRE(score.has_value());
    REQUIRE(score->bytes.size() == 4);
    REQUIRE(score->bytes[0] == std::byte{20});
    REQUIRE(view->value(1, 1)->is_null);

    const auto payload = view->value(0, 2);
    REQUIRE(payload.has_value());
    REQUIRE(payload->bytes.size() == 5);
    REQUIRE(payload->bytes[0] == static_cast<std::byte>('a'));
    REQUIRE(view->value(1, 2)->bytes.empty());
    REQUIRE(view->value(2, 2)->is_null);
    REQUIRE_FALSE(view->value(4, 0).has_value());
    REQUIRE_FALSE(view->value(0, 3).has_value());
}

TEST_CASE("prevalidated column chunks preserve row-group order and nulls") {
    auto schema = mixed_schema();
    ColumnStoreBuilder builder(
        kIdentity, schema, {.rows = 2, .value_bytes = 1'024});
    append_mixed_rows(builder);
    TestChecksums checksums;
    const auto sealed = builder.seal(checksums);
    REQUIRE(sealed.has_value());
    const auto view = validate_column_store(
        *sealed, kIdentity, schema, checksums);
    REQUIRE(view.has_value());

    const auto score_chunks = view->column_chunks(1);
    REQUIRE(score_chunks.has_value());
    REQUIRE(score_chunks->size() == 2);
    REQUIRE((*score_chunks)[0].first_row_ordinal == 0);
    REQUIRE((*score_chunks)[1].first_row_ordinal == 2);
    for (const auto& chunk : *score_chunks) {
        REQUIRE(chunk.row_count == 2);
        REQUIRE(chunk.encoding == ColumnEncoding::FixedByValue);
        REQUIRE(chunk.fixed_width == 4);
        REQUIRE(chunk.fixed_stride == 4);
        REQUIRE(chunk.offsets.empty());
        REQUIRE(chunk.values.size() == 8);
    }
    REQUIRE((*score_chunks)[0].valid(0));
    REQUIRE_FALSE((*score_chunks)[0].valid(1));
    REQUIRE((*score_chunks)[1].valid(0));
    REQUIRE((*score_chunks)[1].valid(1));
    REQUIRE_FALSE((*score_chunks)[1].valid(2));
    REQUIRE((*score_chunks)[0].values[0] == std::byte{10});
    REQUIRE((*score_chunks)[0].values[4] == std::byte{0});
    REQUIRE((*score_chunks)[1].values[0] == std::byte{20});
    REQUIRE((*score_chunks)[1].values[4] == std::byte{30});

    const auto boolean_chunks = view->column_chunks(0);
    REQUIRE(boolean_chunks.has_value());
    REQUIRE(boolean_chunks->size() == 2);
    REQUIRE((*boolean_chunks)[0].encoding == ColumnEncoding::BooleanBits);
    REQUIRE((*boolean_chunks)[0].fixed_width == 0);
    REQUIRE((*boolean_chunks)[0].values.size() == 1);

    const auto payload_chunks = view->column_chunks(2);
    REQUIRE(payload_chunks.has_value());
    REQUIRE(payload_chunks->size() == 2);
    REQUIRE((*payload_chunks)[0].encoding == ColumnEncoding::BinarySend);
    REQUIRE((*payload_chunks)[0].offsets.size()
        == 3 * sizeof(std::uint64_t));
    REQUIRE_FALSE(view->column_chunks(3).has_value());
}

TEST_CASE("binary-send encoding accepts PostgreSQL pass-by-value types") {
    const std::array<ColumnSchema, 1> schema{{
        {
            .name = "binary_int4",
            .descriptor = {
                .type_oid = 23,
                .typmod = -1,
                .type_length = 4,
                .by_value = 1,
                .type_alignment = 4,
                .encoding = ColumnEncoding::BinarySend,
                .send_function_oid = 1,
                .receive_function_oid = 2,
            },
            .semantic_id = {.low = 4},
        },
    }};
    const std::array<std::byte, 4> encoded{
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{42}};
    const std::array<EncodedColumnValue, 1> row{{{.bytes = encoded}}};
    ColumnStoreBuilder builder(kIdentity, schema);
    REQUIRE(builder.append_row(row).has_value());
    TestChecksums checksums;
    const auto blob = builder.seal(checksums);
    REQUIRE(blob.has_value());
    const auto view = validate_column_store(*blob, kIdentity, schema, checksums);
    REQUIRE(view.has_value());
    REQUIRE(view->value(0, 0)->bytes.size() == encoded.size());
    REQUIRE(view->value(0, 0)->bytes[3] == std::byte{42});
}

TEST_CASE("schema digest commits names and complete SQL type metadata") {
    TestChecksums checksums;
    auto base = mixed_schema();
    const auto original = schema_digest(base, checksums);
    REQUIRE(original.has_value());

    auto changed = base;
    changed[0].name = "renamed";
    REQUIRE(schema_digest(changed, checksums).value() != *original);
    changed = base;
    ++changed[0].descriptor.type_oid;
    REQUIRE(schema_digest(changed, checksums).value() != *original);
    changed = base;
    ++changed[0].descriptor.typmod;
    REQUIRE(schema_digest(changed, checksums).value() != *original);
    changed = base;
    ++changed[0].descriptor.collation_oid;
    REQUIRE(schema_digest(changed, checksums).value() != *original);
    changed = base;
    ++changed[0].semantic_id.low;
    REQUIRE(schema_digest(changed, checksums).value() != *original);
}

TEST_CASE("row-group row target splits at minus, at, and plus one") {
    TestChecksums checksums;
    const std::array<ColumnSchema, 0> schema{};
    const std::array<EncodedColumnValue, 0> values{};
    const auto group_count_for = [&](std::uint32_t rows) {
        ColumnStoreBuilder builder(
            kIdentity, schema, {.rows = 4, .value_bytes = 1'024});
        for (std::uint32_t row = 0; row < rows; ++row) {
            REQUIRE(builder.append_row(values).has_value());
        }
        auto blob = builder.seal(checksums);
        REQUIRE(blob.has_value());
        auto view = validate_column_store(
            *blob, kIdentity, schema, checksums);
        REQUIRE(view.has_value());
        return view->row_group_count();
    };

    REQUIRE(group_count_for(3) == 1);
    REQUIRE(group_count_for(4) == 1);
    REQUIRE(group_count_for(5) == 2);
}

TEST_CASE("fixed by-reference storage honors its declared alignment") {
    const std::array<ColumnSchema, 1> schema{{
        {
            .name = "fixed_ref",
            .descriptor = {
                .type_oid = 1'000,
                .typmod = -1,
                .type_length = 3,
                .by_value = 0,
                .type_alignment = 8,
                .encoding = ColumnEncoding::FixedByReference,
            },
            .semantic_id = {.low = 9},
        },
    }};
    const std::array<std::byte, 3> payload{
        std::byte{1}, std::byte{2}, std::byte{3}};
    const std::array<std::byte, 3> second_payload{
        std::byte{4}, std::byte{5}, std::byte{6}};
    const std::array<EncodedColumnValue, 1> row{{{.bytes = payload}}};
    const std::array<EncodedColumnValue, 1> second_row{{
        {.bytes = second_payload},
    }};
    ColumnStoreBuilder builder(kIdentity, schema);
    REQUIRE(builder.append_row(row).has_value());
    REQUIRE(builder.append_row(second_row).has_value());
    TestChecksums checksums;
    const auto blob = builder.seal(checksums);
    REQUIRE(blob.has_value());

    std::size_t cursor = 68;
    const auto index_offset = read_little_endian<std::uint64_t>(*blob, cursor);
    REQUIRE(index_offset.has_value());
    cursor = static_cast<std::size_t>(*index_offset);
    REQUIRE(read_little_endian<std::uint64_t>(*blob, cursor).value() == 1);
    const auto group_offset = read_little_endian<std::uint64_t>(*blob, cursor);
    REQUIRE(group_offset.has_value());
    cursor = static_cast<std::size_t>(*group_offset)
        + kRowGroupHeaderBytes + 4u * sizeof(std::uint64_t);
    const auto values_offset = read_little_endian<std::uint64_t>(*blob, cursor);
    REQUIRE(values_offset.has_value());
    REQUIRE((*group_offset + *values_offset) % 8u == 0);

    const auto view = validate_column_store(
        *blob, kIdentity, schema, checksums);
    REQUIRE(view.has_value());
    REQUIRE(view->value(0, 0)->bytes.size() == payload.size());
    REQUIRE(view->value(0, 0)->bytes[0] == std::byte{1});
    REQUIRE(view->value(1, 0)->bytes[0] == std::byte{4});
}

TEST_CASE("opposite-endian metadata is rejected before allocation") {
    auto schema = mixed_schema();
    ColumnStoreBuilder builder(kIdentity, schema);
    append_mixed_rows(builder);
    TestChecksums checksums;
    auto blob = builder.seal(checksums);
    REQUIRE(blob.has_value());
    (*blob)[40] = static_cast<std::byte>(
        static_cast<std::uint8_t>(DataEndian::Big));
    REQUIRE(
        validate_column_store(*blob, kIdentity, schema, checksums).error()
        == FormatError::EndianMismatch);
}

TEST_CASE("empty and zero-column result shapes remain first-class") {
    TestChecksums checksums;
    auto schema = mixed_schema();
    ColumnStoreBuilder empty(kIdentity, schema);
    const auto empty_blob = empty.seal(checksums);
    REQUIRE(empty_blob.has_value());
    const auto empty_view = validate_column_store(
        *empty_blob, kIdentity, schema, checksums);
    REQUIRE(empty_view.has_value());
    REQUIRE(empty_view->row_count() == 0);
    REQUIRE(empty_view->row_group_count() == 0);

    const std::array<ColumnSchema, 0> no_schema{};
    ColumnStoreBuilder zero_columns(
        kIdentity, no_schema, {.rows = 2, .value_bytes = 16});
    const std::array<EncodedColumnValue, 0> no_values{};
    REQUIRE(zero_columns.append_row(no_values).has_value());
    REQUIRE(zero_columns.append_row(no_values).has_value());
    REQUIRE(zero_columns.append_row(no_values).has_value());
    const auto zero_blob = zero_columns.seal(checksums);
    REQUIRE(zero_blob.has_value());
    const auto zero_view = validate_column_store(
        *zero_blob, kIdentity, no_schema, checksums);
    REQUIRE(zero_view.has_value());
    REQUIRE(zero_view->row_count() == 3);
    REQUIRE(zero_view->column_count() == 0);
    REQUIRE(zero_view->row_group_count() == 2);
}

TEST_CASE("builder rejects schema, arity, NULL, width, and boolean violations") {
    TestChecksums checksums;
    auto schema = mixed_schema();
    ColumnStoreBuilder builder(kIdentity, schema);

    const std::array<EncodedColumnValue, 0> no_values{};
    REQUIRE(
        builder.append_row(no_values).error()
        == FormatError::ColumnCountMismatch);

    auto nonnullable_schema = schema;
    nonnullable_schema[0].descriptor.flags = 0;
    ColumnStoreBuilder nonnullable(kIdentity, nonnullable_schema);
    const std::array<std::byte, 4> fixed{
        std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}};
    const std::array<EncodedColumnValue, 3> null_row{{
        {.is_null = true},
        {.bytes = fixed},
        {.bytes = text_bytes("x")},
    }};
    REQUIRE(
        nonnullable.append_row(null_row).error()
        == FormatError::InvalidNullBitmap);

    const std::array<std::byte, 1> invalid_boolean{std::byte{2}};
    const std::array<EncodedColumnValue, 3> bad_boolean{{
        {.bytes = invalid_boolean},
        {.bytes = fixed},
        {.bytes = text_bytes("x")},
    }};
    REQUIRE(
        builder.append_row(bad_boolean).error()
        == FormatError::InvalidBoolean);

    const std::array<std::byte, 3> short_fixed{
        std::byte{1}, std::byte{2}, std::byte{3}};
    const std::array<std::byte, 1> valid_boolean{std::byte{1}};
    const std::array<EncodedColumnValue, 3> bad_fixed{{
        {.bytes = valid_boolean},
        {.bytes = short_fixed},
        {.bytes = text_bytes("x")},
    }};
    REQUIRE(
        builder.append_row(bad_fixed).error()
        == FormatError::InvalidFixedWidth);

    auto unsupported_schema = schema;
    unsupported_schema[2].descriptor.encoding = ColumnEncoding::Dictionary;
    ColumnStoreBuilder unsupported(kIdentity, unsupported_schema);
    REQUIRE(
        unsupported.seal(checksums).error()
        == FormatError::UnsupportedEncoding);

    auto invalid_alignment_schema = schema;
    invalid_alignment_schema[1].descriptor.type_alignment = 3;
    ColumnStoreBuilder invalid_alignment(kIdentity, invalid_alignment_schema);
    REQUIRE(
        invalid_alignment.seal(checksums).error()
        == FormatError::InvalidSchema);
}

TEST_CASE("every stored byte is covered by validation or a checksum") {
    auto schema = mixed_schema();
    ColumnStoreBuilder builder(kIdentity, schema, {.rows = 2, .value_bytes = 1'024});
    append_mixed_rows(builder);
    TestChecksums checksums;
    const auto sealed = builder.seal(checksums);
    REQUIRE(sealed.has_value());

    for (std::size_t index = 0; index < sealed->size(); ++index) {
        auto corrupt = *sealed;
        corrupt[index] ^= std::byte{0x01};
        INFO("corrupt byte offset " << index);
        REQUIRE_FALSE(validate_column_store(
            corrupt, kIdentity, schema, checksums).has_value());
    }
}

TEST_CASE("all truncation points, identities, and unavailable checksums fail closed") {
    auto schema = mixed_schema();
    ColumnStoreBuilder builder(kIdentity, schema, {.rows = 2, .value_bytes = 1'024});
    append_mixed_rows(builder);
    TestChecksums checksums;
    const auto sealed = builder.seal(checksums);
    REQUIRE(sealed.has_value());

    for (std::size_t length = 0; length < sealed->size(); ++length) {
        INFO("truncated length " << length);
        REQUIRE_FALSE(validate_column_store(
            std::span<const std::byte>(*sealed).first(length),
            kIdentity,
            schema,
            checksums).has_value());
    }

    auto wrong_identity = kIdentity;
    ++wrong_identity.boot_nonce;
    REQUIRE(
        validate_column_store(
            *sealed, wrong_identity, schema, checksums).error()
        == FormatError::IdentityMismatch);

    auto wrong_schema = schema;
    wrong_schema[0].name = "wrong_name";
    REQUIRE(
        validate_column_store(
            *sealed, kIdentity, wrong_schema, checksums).error()
        == FormatError::SchemaMismatch);

    UnavailableChecksums unavailable;
    REQUIRE(
        builder.seal(unavailable).error()
        == FormatError::ChecksumUnavailable);
    REQUIRE(
        validate_column_store(
            *sealed, kIdentity, schema, unavailable).error()
        == FormatError::ChecksumUnavailable);
}

TEST_CASE("dictionary and block-reference policies fail safely") {
    REQUIRE(dictionary_is_beneficial(100, 30, 20));
    REQUIRE_FALSE(dictionary_is_beneficial(50, 30, 20));
    REQUIRE_FALSE(dictionary_is_beneficial(
        (std::numeric_limits<std::uint64_t>::max)(),
        (std::numeric_limits<std::uint64_t>::max)(),
        1));

    const BlockReferenceV1 valid{
        .tier = BlockBackingTierV1::File,
        .handle_or_file_id = 1,
        .offset = 100,
        .length = 50,
    };
    REQUIRE(validate_block_reference(valid, 150));
    auto invalid = valid;
    invalid.offset = (std::numeric_limits<std::uint64_t>::max)();
    REQUIRE_FALSE(validate_block_reference(invalid, 150));
    invalid = valid;
    invalid.handle_or_file_id = 0;
    REQUIRE_FALSE(validate_block_reference(invalid, 150));
    invalid = valid;
    invalid.tier = BlockBackingTierV1::Dsa;
    REQUIRE(validate_block_reference(invalid, 150));
    invalid.flags = 1;
    REQUIRE_FALSE(validate_block_reference(invalid, 150));
    invalid = valid;
    invalid.length = 0;
    REQUIRE_FALSE(validate_block_reference(invalid, 150));
}
