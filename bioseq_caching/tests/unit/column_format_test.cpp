#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

import bioseq.cache.core;

using namespace bioseq::cache;

namespace {

struct test_checksums {
    [[nodiscard]] std::expected<std::uint32_t, checksum_error>
    block_crc32c(std::span<const std::byte> bytes) const noexcept {
        std::uint32_t hash = 2'166'136'261u;
        for (const std::byte byte : bytes) {
            hash ^= std::to_integer<std::uint8_t>(byte);
            hash *= 16'777'619u;
        }
        return hash;
    }

    [[nodiscard]] std::expected<cache_digest_256, checksum_error>
    sha256(std::span<const std::byte> bytes) const noexcept {
        std::array<std::uint64_t, 4> lanes{
            0xcbf29ce484222325ULL,
            0x9e3779b97f4a7c15ULL,
            0x6a09e667f3bcc909ULL,
            0xbb67ae8584caa73bULL,
        };
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            const std::uint64_t value = std::to_integer<std::uint8_t>(bytes[index]);
            for (std::size_t lane = 0; lane < lanes.size(); ++lane) {
                lanes[lane] ^= value + index + lane * 0x9d;
                lanes[lane] *= 0x100000001b3ULL + lane * 2u;
                lanes[lane] ^= lanes[lane] >> (17u + lane);
            }
        }
        cache_digest_256 digest{};
        for (std::size_t lane = 0; lane < lanes.size(); ++lane) {
            for (std::size_t byte = 0; byte < 8; ++byte) {
                digest.bytes[lane * 8u + byte] =
                    static_cast<std::uint8_t>(lanes[lane] >> (byte * 8u));
            }
        }
        return digest;
    }

    [[nodiscard]] std::expected<cache_digest_256, checksum_error>
    manifest_sha256(std::span<const cache_digest_256> block_digests) const noexcept {
        std::vector<std::byte> bytes;
        bytes.reserve(block_digests.size() * 32u);
        for (const cache_digest_256& digest : block_digests) {
            for (const std::uint8_t byte : digest.bytes) {
                bytes.push_back(static_cast<std::byte>(byte));
            }
        }
        return sha256(bytes);
    }
};

struct unavailable_checksums {
    [[nodiscard]] std::expected<std::uint32_t, checksum_error>
    block_crc32c(std::span<const std::byte>) const noexcept {
        return std::unexpected(checksum_error::unavailable);
    }

    [[nodiscard]] std::expected<cache_digest_256, checksum_error>
    sha256(std::span<const std::byte>) const noexcept {
        return std::unexpected(checksum_error::unavailable);
    }

    [[nodiscard]] std::expected<cache_digest_256, checksum_error>
    manifest_sha256(std::span<const cache_digest_256>) const noexcept {
        return std::unexpected(checksum_error::unavailable);
    }
};

constexpr column_store_identity k_identity{
    .postgres_major = 18,
    .boot_nonce = 0x1234'5678'9abc'def0ULL,
    .database_oid = 42,
    .owner_role_oid = 84,
    .datum_width = sizeof(std::uintptr_t),
};

std::array<column_schema, 3> mixed_schema() {
    return {{
        {
            .name = "accepted",
            .descriptor =
                {
                    .type_oid = 16,
                    .typmod = -1,
                    .type_length = 1,
                    .by_value = 1,
                    .type_alignment = 1,
                    .encoding = column_encoding::boolean_bits,
                    .flags = k_column_flag_nullable,
                },
            .semantic_id = {.low = 1},
        },
        {
            .name = "score",
            .descriptor =
                {
                    .type_oid = 23,
                    .typmod = -1,
                    .type_length = 4,
                    .by_value = 1,
                    .type_alignment = 4,
                    .encoding = column_encoding::fixed_by_value,
                    .flags = k_column_flag_nullable,
                },
            .semantic_id = {.low = 2},
        },
        {
            .name = "payload",
            .descriptor =
                {
                    .type_oid = 17,
                    .typmod = -1,
                    .type_length = -1,
                    .by_value = 0,
                    .type_alignment = 1,
                    .encoding = column_encoding::binary_send,
                    .flags = k_column_flag_nullable,
                    .send_function_oid = 1,
                    .receive_function_oid = 2,
                },
            .semantic_id = {.low = 3},
        },
    }};
}

template <std::size_t p_size>
std::span<const std::byte> bytes_of(const std::array<std::byte, p_size>& bytes) {
    return bytes;
}

std::span<const std::byte> text_bytes(std::string_view text) {
    return std::as_bytes(std::span<const char>{text.data(), text.size()});
}

template <class p_builder> void append_mixed_rows(p_builder& builder) {
    const std::array<std::byte, 1> boolean_true{std::byte{1}};
    const std::array<std::byte, 1> boolean_false{std::byte{0}};
    const std::array<std::byte, 4> score_10{
        std::byte{10}, std::byte{0}, std::byte{0}, std::byte{0}};
    const std::array<std::byte, 4> score_20{
        std::byte{20}, std::byte{0}, std::byte{0}, std::byte{0}};
    const std::array<std::byte, 4> score_30{
        std::byte{30}, std::byte{0}, std::byte{0}, std::byte{0}};

    const std::array<encoded_column_value, 3> row0{{
        {.bytes = bytes_of(boolean_true)},
        {.bytes = bytes_of(score_10)},
        {.bytes = text_bytes("alpha")},
    }};
    const std::array<encoded_column_value, 3> row1{{
        {.is_null = true},
        {.is_null = true},
        {.bytes = text_bytes("")},
    }};
    const std::array<encoded_column_value, 3> row2{{
        {.bytes = bytes_of(boolean_false)},
        {.bytes = bytes_of(score_20)},
        {.is_null = true},
    }};
    const std::array<encoded_column_value, 3> row3{{
        {.bytes = bytes_of(boolean_true)},
        {.bytes = bytes_of(score_30)},
        {.bytes = text_bytes("omega")},
    }};

    REQUIRE(builder.append_row(row0).has_value());
    REQUIRE(builder.append_row(row1).has_value());
    REQUIRE(builder.append_row(row2).has_value());
    REQUIRE(builder.append_row(row3).has_value());
}

struct capturing_block_sink {
    struct token_type {
        std::size_t slot = 0;

        [[nodiscard]] bool operator==(const token_type&) const noexcept = default;
    };

    [[nodiscard]] std::expected<token_type, format_error>
    store(column_store_block_kind kind, std::uint64_t ordinal, std::span<const std::byte> bytes) {
        const std::size_t call = store_calls++;
        if (fail_on_store.has_value() && call == *fail_on_store) {
            return std::unexpected(format_error::sink_failed);
        }
        const std::size_t slot = blocks.size();
        blocks.emplace_back(bytes.begin(), bytes.end());
        kinds.push_back(kind);
        ordinals.push_back(ordinal);
        discard_counts.push_back(0);
        return token_type{.slot = slot};
    }

    void discard(const token_type& token) noexcept {
        if (token.slot < discard_counts.size()) {
            ++discard_counts[token.slot];
        }
    }

    [[nodiscard]] std::vector<std::byte>
    assemble(std::span<const token_type> logical_tokens) const {
        std::vector<std::byte> bytes;
        for (const token_type& token : logical_tokens) {
            bytes.insert(bytes.end(), blocks[token.slot].begin(), blocks[token.slot].end());
        }
        return bytes;
    }

    [[nodiscard]] bool every_stored_block_discarded_once() const noexcept {
        for (const std::uint32_t count : discard_counts) {
            if (count != 1) {
                return false;
            }
        }
        return true;
    }

    std::optional<std::size_t> fail_on_store;
    std::size_t store_calls = 0;
    std::vector<std::vector<std::byte>> blocks;
    std::vector<column_store_block_kind> kinds;
    std::vector<std::uint64_t> ordinals;
    std::vector<std::uint32_t> discard_counts;
};

struct counting_block_sink {
    using token_type = std::uint32_t;

    [[nodiscard]] std::expected<token_type, format_error>
    store(column_store_block_kind, std::uint64_t, std::span<const std::byte>) {
        return stored++;
    }

    void discard(const token_type&) noexcept {
        ++discarded;
    }

    std::uint32_t stored = 0;
    std::uint32_t discarded = 0;
};

struct capturing_physical_sink {
    struct token_type {
        std::size_t slot = 0;

        [[nodiscard]] bool operator==(const token_type&) const noexcept = default;
    };

    [[nodiscard]] std::expected<token_type, format_error>
    store_physical(std::span<const std::byte> bytes) {
        const std::size_t call = store_calls++;
        if (fail_on_store.has_value() && call == *fail_on_store) {
            return std::unexpected(format_error::sink_failed);
        }
        const std::size_t slot = blocks.size();
        blocks.emplace_back(bytes.begin(), bytes.end());
        discard_counts.push_back(0);
        return token_type{.slot = slot};
    }

    void discard_physical(const token_type& token) noexcept {
        if (token.slot < discard_counts.size()) {
            ++discard_counts[token.slot];
        }
    }

    [[nodiscard]] std::vector<std::byte> assemble(std::span<const token_type> tokens) const {
        std::vector<std::byte> bytes;
        for (const token_type& token : tokens) {
            bytes.insert(bytes.end(), blocks[token.slot].begin(), blocks[token.slot].end());
        }
        return bytes;
    }

    [[nodiscard]] bool every_stored_block_discarded_once() const noexcept {
        for (const std::uint32_t count : discard_counts) {
            if (count != 1) {
                return false;
            }
        }
        return true;
    }

    std::optional<std::size_t> fail_on_store;
    std::size_t store_calls = 0;
    std::vector<std::vector<std::byte>> blocks;
    std::vector<std::uint32_t> discard_counts;
};

struct metadata_heavy_physical_sink {
    struct token_type {
        std::array<
            std::byte,
            (k_maximum_column_store_metadata_bytes / k_maximum_column_store_physical_blocks) + 1u>
            bytes{};
    };

    [[nodiscard]] std::expected<token_type, format_error>
    store_physical(std::span<const std::byte>) {
        return std::unexpected(format_error::sink_failed);
    }

    void discard_physical(const token_type&) noexcept {}
};

} // namespace

TEST_CASE("streaming output is byte-identical across mixed multi-groups",
          "[component:cache][layer:unit][feature:column_format]") {
    auto schema = mixed_schema();
    constexpr row_group_limits limits{.rows = 2, .value_bytes = 1'024};
    test_checksums checksums;
    column_store_builder retained(k_identity, schema, limits);
    capturing_block_sink sink;
    streaming_column_store_builder streamed(k_identity, schema, checksums, sink, limits);

    append_mixed_rows(retained);
    append_mixed_rows(streamed);
    REQUIRE(streamed.row_count() == 4);
    REQUIRE(streamed.pending_row_count() == 2);
    REQUIRE(streamed.emitted_row_group_count() == 1);
    REQUIRE(streamed.retained_value_bytes() <= limits.value_bytes);
    REQUIRE(streamed.retained_metadata_bytes() <= k_maximum_column_store_metadata_bytes);

    const auto expected = retained.seal(checksums);
    REQUIRE(expected.has_value());
    auto actual = streamed.finalize();
    REQUIRE(actual.has_value());
    REQUIRE(actual->logical_bytes() == expected->size());
    REQUIRE(actual->row_count() == 4);
    REQUIRE(actual->row_group_count() == 2);
    REQUIRE(sink.kinds == std::vector<column_store_block_kind>{
                              column_store_block_kind::row_group,
                              column_store_block_kind::row_group,
                              column_store_block_kind::header,
                              column_store_block_kind::schema,
                              column_store_block_kind::index,
                              column_store_block_kind::footer,
                          });
    const std::vector<std::byte> assembled = sink.assemble(actual->tokens());
    REQUIRE(assembled == *expected);
    const auto view = validate_column_store(assembled, k_identity, schema, checksums);
    REQUIRE(view.has_value());
    REQUIRE(view->value(0, 0)->boolean_value);
    REQUIRE(view->value(1, 0)->is_null);
    REQUIRE(view->value(2, 1)->bytes[0] == std::byte{20});
    REQUIRE(view->value(3, 2)->bytes.size() == 5);

    auto corrupt = assembled;
    corrupt[k_row_group_header_bytes] ^= std::byte{1};
    REQUIRE_FALSE(validate_column_store(corrupt, k_identity, schema, checksums).has_value());
    actual->mark_published();
}

TEST_CASE("streaming preserves empty, aligned fixed, and flat-varlena wire bytes",
          "[component:cache][layer:unit][feature:column_format]") {
    test_checksums checksums;
    auto mixed = mixed_schema();
    column_store_builder empty_retained(k_identity, mixed);
    capturing_block_sink empty_sink;
    streaming_column_store_builder empty_streamed(k_identity, mixed, checksums, empty_sink);
    const auto empty_expected = empty_retained.seal(checksums);
    auto empty_actual = empty_streamed.finalize();
    REQUIRE(empty_expected.has_value());
    REQUIRE(empty_actual.has_value());
    REQUIRE(empty_sink.assemble(empty_actual->tokens()) == *empty_expected);
    empty_actual->mark_published();

    const std::array<column_schema, 2> schema{{
        {
            .name = "fixed_ref",
            .descriptor =
                {
                    .type_oid = 1'000,
                    .typmod = -1,
                    .type_length = 3,
                    .by_value = 0,
                    .type_alignment = 8,
                    .encoding = column_encoding::fixed_by_reference,
                    .flags = k_column_flag_nullable,
                },
            .semantic_id = {.low = 10},
        },
        {
            .name = "flat_text",
            .descriptor =
                {
                    .type_oid = 25,
                    .typmod = -1,
                    .type_length = -1,
                    .by_value = 0,
                    .type_alignment = 4,
                    .encoding = column_encoding::flat_varlena,
                    .flags = k_column_flag_nullable,
                },
            .semantic_id = {.low = 11},
        },
    }};
    const std::array<std::byte, 3> fixed0{std::byte{1}, std::byte{2}, std::byte{3}};
    const std::array<std::byte, 3> fixed1{std::byte{4}, std::byte{5}, std::byte{6}};
    const std::array<encoded_column_value, 2> row0{{
        {.bytes = fixed0},
        {.is_null = true},
    }};
    const std::array<encoded_column_value, 2> row1{{
        {.is_null = true},
        {.bytes = text_bytes("varlena")},
    }};
    const std::array<encoded_column_value, 2> row2{{
        {.bytes = fixed1},
        {.bytes = text_bytes("")},
    }};
    constexpr row_group_limits limits{.rows = 2, .value_bytes = 32};
    column_store_builder retained(k_identity, schema, limits);
    capturing_block_sink sink;
    streaming_column_store_builder streamed(k_identity, schema, checksums, sink, limits);
    for (const auto& row : {row0, row1, row2}) {
        REQUIRE(retained.append_row(row).has_value());
        REQUIRE(streamed.append_row(row).has_value());
    }
    const auto expected = retained.seal(checksums);
    auto actual = streamed.finalize();
    REQUIRE(expected.has_value());
    REQUIRE(actual.has_value());
    const auto assembled = sink.assemble(actual->tokens());
    REQUIRE(assembled == *expected);
    const auto view = validate_column_store(assembled, k_identity, schema, checksums);
    REQUIRE(view.has_value());
    REQUIRE(view->row_group_count() == 2);
    REQUIRE(view->value(0, 0)->bytes[0] == std::byte{1});
    REQUIRE(view->value(1, 0)->is_null);
    REQUIRE(view->value(0, 1)->is_null);
    REQUIRE(view->value(1, 1)->bytes.size() == 7);
    actual->mark_published();
}

TEST_CASE("streaming row-value boundaries and hard caps fail before retention",
          "[component:cache][layer:unit][feature:column_format]") {
    test_checksums checksums;
    const std::array<column_schema, 1> schema{{
        {
            .name = "bytes",
            .descriptor =
                {
                    .type_oid = 17,
                    .typmod = -1,
                    .type_length = -1,
                    .by_value = 0,
                    .type_alignment = 1,
                    .encoding = column_encoding::binary_send,
                    .send_function_oid = 1,
                    .receive_function_oid = 2,
                },
        },
    }};
    constexpr row_group_limits limits{.rows = 4, .value_bytes = 5};
    const std::array<encoded_column_value, 1> three{{
        {.bytes = text_bytes("abc")},
    }};
    const std::array<encoded_column_value, 1> two{{
        {.bytes = text_bytes("de")},
    }};
    const std::array<encoded_column_value, 1> one{{
        {.bytes = text_bytes("f")},
    }};
    column_store_builder retained(k_identity, schema, limits);
    capturing_block_sink sink;
    streaming_column_store_builder streamed(k_identity, schema, checksums, sink, limits);
    for (const auto& row : {three, two, one}) {
        REQUIRE(retained.append_row(row).has_value());
        REQUIRE(streamed.append_row(row).has_value());
    }
    REQUIRE(streamed.emitted_row_group_count() == 1);
    const auto expected = retained.seal(checksums);
    auto actual = streamed.finalize();
    REQUIRE(expected.has_value());
    REQUIRE(actual.has_value());
    REQUIRE(sink.assemble(actual->tokens()) == *expected);
    actual->mark_published();

    const std::array<encoded_column_value, 1> six{{
        {.bytes = text_bytes("123456")},
    }};
    capturing_block_sink rejected_sink;
    streaming_column_store_builder rejected(k_identity, schema, checksums, rejected_sink, limits);
    REQUIRE(rejected.append_row(six).error() == format_error::row_group_value_limit_exceeded);
    REQUIRE(rejected.row_count() == 0);
    REQUIRE(rejected.pending_row_count() == 0);
    REQUIRE(rejected_sink.store_calls == 0);

    capturing_block_sink invalid_limits_sink;
    streaming_column_store_builder invalid_limits(
        k_identity,
        schema,
        checksums,
        invalid_limits_sink,
        {.rows = k_default_row_group_rows + 1u, .value_bytes = 1});
    REQUIRE(invalid_limits.append_row(one).error() == format_error::invalid_row_group_limits);
    REQUIRE(invalid_limits_sink.store_calls == 0);

    REQUIRE(projected_column_store_bytes(k_maximum_column_store_bytes - 152u, 0).value() ==
            k_maximum_column_store_bytes);
    REQUIRE(projected_column_store_bytes(k_maximum_column_store_bytes - 151u, 0).error() ==
            format_error::maximum_store_bytes_exceeded);
    REQUIRE(projected_column_store_bytes((std::numeric_limits<std::uint64_t>::max)(), 0).error() ==
            format_error::arithmetic_overflow);
    REQUIRE(projected_column_store_bytes(
                0, static_cast<std::uint64_t>(k_maximum_column_store_row_groups) + 1u)
                .error() == format_error::too_many_row_groups);
}

TEST_CASE("streaming row-group metadata is capped and cleaned exactly once",
          "[component:cache][layer:unit][feature:column_format]") {
    test_checksums checksums;
    const std::array<column_schema, 0> schema{};
    const std::array<encoded_column_value, 0> row{};
    counting_block_sink sink;
    {
        streaming_column_store_builder streamed(
            k_identity, schema, checksums, sink, {.rows = 1, .value_bytes = 1});
        for (std::uint32_t index = 0; index <= k_maximum_column_store_row_groups; ++index) {
            INFO("row ordinal " << index);
            REQUIRE(streamed.append_row(row).has_value());
        }
        REQUIRE(streamed.emitted_row_group_count() == k_maximum_column_store_row_groups);
        REQUIRE(streamed.retained_metadata_bytes() <= k_maximum_column_store_metadata_bytes);
        REQUIRE(streamed.finalize().error() == format_error::too_many_row_groups);
        REQUIRE(sink.stored == k_maximum_column_store_row_groups);
        REQUIRE(sink.discarded == sink.stored);
    }
    REQUIRE(sink.discarded == sink.stored);
}

TEST_CASE("streaming sink failures discard every temporary token once",
          "[component:cache][layer:unit][feature:column_format]") {
    test_checksums checksums;
    const std::array<column_schema, 0> schema{};
    const std::array<encoded_column_value, 0> row{};

    capturing_block_sink group_failure;
    group_failure.fail_on_store = 1;
    {
        streaming_column_store_builder streamed(
            k_identity, schema, checksums, group_failure, {.rows = 1, .value_bytes = 1});
        REQUIRE(streamed.append_row(row).has_value());
        REQUIRE(streamed.append_row(row).has_value());
        REQUIRE(streamed.append_row(row).error() == format_error::sink_failed);
        REQUIRE(group_failure.blocks.size() == 1);
        REQUIRE(group_failure.every_stored_block_discarded_once());
        REQUIRE(streamed.append_row(row).error() == format_error::sink_failed);
    }
    REQUIRE(group_failure.every_stored_block_discarded_once());

    capturing_block_sink terminal_failure;
    terminal_failure.fail_on_store = 4;
    {
        streaming_column_store_builder streamed(
            k_identity, schema, checksums, terminal_failure, {.rows = 1, .value_bytes = 1});
        REQUIRE(streamed.append_row(row).has_value());
        REQUIRE(streamed.append_row(row).has_value());
        REQUIRE(streamed.finalize().error() == format_error::sink_failed);
        REQUIRE(terminal_failure.blocks.size() == 4);
        REQUIRE(terminal_failure.every_stored_block_discarded_once());
    }
    REQUIRE(terminal_failure.every_stored_block_discarded_once());

    capturing_block_sink abandoned;
    {
        streaming_column_store_builder streamed(k_identity, schema, checksums, abandoned);
        REQUIRE(streamed.append_row(row).has_value());
        {
            auto result = streamed.finalize();
            REQUIRE(result.has_value());
            REQUIRE_FALSE(abandoned.every_stored_block_discarded_once());
        }
        REQUIRE(abandoned.every_stored_block_discarded_once());
    }
    REQUIRE(abandoned.every_stored_block_discarded_once());
}

TEST_CASE("coalescing sink publishes physical bytes in logical block order",
          "[component:cache][layer:unit][feature:column_format]") {
    auto schema = mixed_schema();
    constexpr row_group_limits limits{.rows = 2, .value_bytes = 1'024};
    test_checksums checksums;
    column_store_builder retained(k_identity, schema, limits);
    capturing_physical_sink physical;
    coalescing_column_store_sink coalesced(physical, 256, 32);
    streaming_column_store_builder streamed(k_identity, schema, checksums, coalesced, limits);
    append_mixed_rows(retained);
    append_mixed_rows(streamed);

    const auto expected = retained.seal(checksums);
    auto logical = streamed.finalize();
    REQUIRE(expected.has_value());
    REQUIRE(logical.has_value());
    const auto manifest = coalesced.manifest_tokens(logical->tokens());
    REQUIRE(manifest.has_value());
    REQUIRE(manifest->size() <= k_maximum_column_store_physical_blocks);
    REQUIRE(manifest->size() == coalesced.physical_block_count());
    REQUIRE(coalesced.retained_buffer_bytes() == 0);
    REQUIRE(coalesced.retained_metadata_bytes() <= k_maximum_column_store_metadata_bytes);
    for (const auto& block : physical.blocks) {
        REQUIRE(block.size() <= 256);
    }
    REQUIRE_FALSE(manifest->empty());
    REQUIRE((*manifest)[0].slot != 0);
    const auto assembled = physical.assemble(*manifest);
    REQUIRE(assembled == *expected);
    REQUIRE(validate_column_store(assembled, k_identity, schema, checksums).has_value());

    coalesced.mark_published();
    logical->mark_published();
    REQUIRE(physical.discard_counts == std::vector<std::uint32_t>(physical.blocks.size(), 0));
}

TEST_CASE("coalescing sink cleans group, prefix, and suffix flush failures",
          "[component:cache][layer:unit][feature:column_format]") {
    const std::array<std::byte, 4> full{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    const std::array<std::byte, 1> one{std::byte{5}};
    const std::array<std::byte, 3> three{std::byte{6}, std::byte{7}, std::byte{8}};

    for (std::size_t failure_call = 0; failure_call < 3; ++failure_call) {
        capturing_physical_sink physical;
        physical.fail_on_store = failure_call;
        {
            coalescing_column_store_sink coalesced(physical, 4, 32);
            std::vector<column_store_logical_block_token> tokens;
            const auto group = coalesced.store(column_store_block_kind::row_group, 0, full);
            if (failure_call == 0) {
                REQUIRE(group.error() == format_error::sink_failed);
            } else {
                REQUIRE(group.has_value());
                tokens.push_back(*group);
                const auto header = coalesced.store(column_store_block_kind::header, 0, one);
                REQUIRE(header.has_value());
                tokens.push_back(*header);
                const auto schema = coalesced.store(column_store_block_kind::schema, 0, three);
                if (failure_call == 1) {
                    REQUIRE(schema.error() == format_error::sink_failed);
                } else {
                    REQUIRE(schema.has_value());
                    tokens.push_back(*schema);
                    const auto index = coalesced.store(column_store_block_kind::index, 0, one);
                    REQUIRE(index.has_value());
                    tokens.push_back(*index);
                    const auto footer = coalesced.store(column_store_block_kind::footer, 0, three);
                    REQUIRE(footer.error() == format_error::sink_failed);
                }
            }
            for (const auto& token : tokens) {
                coalesced.discard(token);
            }
            REQUIRE(physical.every_stored_block_discarded_once());
        }
        REQUIRE(physical.every_stored_block_discarded_once());
    }
}

TEST_CASE("coalescing sink rejects physical manifest overflow fail-open",
          "[component:cache][layer:unit][feature:column_format]") {
    const std::array<std::byte, 1> one{std::byte{1}};
    capturing_physical_sink physical;
    {
        coalescing_column_store_sink coalesced(physical, 1, k_maximum_column_store_physical_blocks);
        std::vector<column_store_logical_block_token> tokens;
        for (std::uint32_t index = 0; index < k_maximum_column_store_physical_blocks; ++index) {
            auto stored = coalesced.store(column_store_block_kind::row_group, index, one);
            REQUIRE(stored.has_value());
            tokens.push_back(*stored);
        }
        REQUIRE(coalesced
                    .store(column_store_block_kind::row_group,
                           k_maximum_column_store_physical_blocks,
                           one)
                    .error() == format_error::physical_block_limit_exceeded);
        REQUIRE(physical.blocks.size() == k_maximum_column_store_physical_blocks);
        REQUIRE(physical.every_stored_block_discarded_once());
        for (const auto& token : tokens) {
            coalesced.discard(token);
        }
    }
    REQUIRE(physical.every_stored_block_discarded_once());

    metadata_heavy_physical_sink heavy;
    coalescing_column_store_sink rejected(heavy, 4, 32);
    REQUIRE(rejected.store(column_store_block_kind::row_group, 0, one).error() ==
            format_error::metadata_limit_exceeded);
}

TEST_CASE("validity bitmaps are canonical at byte boundaries",
          "[component:cache][layer:unit][feature:column_format]") {
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

TEST_CASE("validity bitmap boundary sweep round trips packed booleans",
          "[component:cache][layer:unit][feature:column_format]") {
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

TEST_CASE("column store round trips null, boolean, fixed, and binary values",
          "[component:cache][layer:unit][feature:column_format]") {
    auto schema = mixed_schema();
    column_store_builder builder(k_identity, schema, {.rows = 2, .value_bytes = 1'024});
    append_mixed_rows(builder);
    REQUIRE(builder.row_count() == 4);

    test_checksums checksums;
    const auto sealed = builder.seal(checksums);
    REQUIRE(sealed.has_value());
    const auto view = validate_column_store(*sealed, k_identity, schema, checksums);
    REQUIRE(view.has_value());
    REQUIRE(view->row_count() == 4);
    REQUIRE(view->column_count() == 3);
    REQUIRE(view->row_group_count() == 2);
    REQUIRE(view->schema() == std::vector<column_schema>(schema.begin(), schema.end()));

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

TEST_CASE("prevalidated column chunks preserve row-group order and nulls",
          "[component:cache][layer:unit][feature:column_format]") {
    auto schema = mixed_schema();
    column_store_builder builder(k_identity, schema, {.rows = 2, .value_bytes = 1'024});
    append_mixed_rows(builder);
    test_checksums checksums;
    const auto sealed = builder.seal(checksums);
    REQUIRE(sealed.has_value());
    const auto view = validate_column_store(*sealed, k_identity, schema, checksums);
    REQUIRE(view.has_value());

    const auto score_chunks = view->column_chunks(1);
    REQUIRE(score_chunks.has_value());
    REQUIRE(score_chunks->size() == 2);
    REQUIRE((*score_chunks)[0].first_row_ordinal == 0);
    REQUIRE((*score_chunks)[1].first_row_ordinal == 2);
    for (const auto& chunk : *score_chunks) {
        REQUIRE(chunk.row_count == 2);
        REQUIRE(chunk.encoding == column_encoding::fixed_by_value);
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
    REQUIRE((*boolean_chunks)[0].encoding == column_encoding::boolean_bits);
    REQUIRE((*boolean_chunks)[0].fixed_width == 0);
    REQUIRE((*boolean_chunks)[0].values.size() == 1);

    const auto payload_chunks = view->column_chunks(2);
    REQUIRE(payload_chunks.has_value());
    REQUIRE(payload_chunks->size() == 2);
    REQUIRE((*payload_chunks)[0].encoding == column_encoding::binary_send);
    REQUIRE((*payload_chunks)[0].offsets.size() == 3 * sizeof(std::uint64_t));
    REQUIRE_FALSE(view->column_chunks(3).has_value());
}

TEST_CASE("binary-send encoding accepts PostgreSQL pass-by-value types",
          "[component:cache][layer:unit][feature:column_format]") {
    const std::array<column_schema, 1> schema{{
        {
            .name = "binary_int4",
            .descriptor =
                {
                    .type_oid = 23,
                    .typmod = -1,
                    .type_length = 4,
                    .by_value = 1,
                    .type_alignment = 4,
                    .encoding = column_encoding::binary_send,
                    .send_function_oid = 1,
                    .receive_function_oid = 2,
                },
            .semantic_id = {.low = 4},
        },
    }};
    const std::array<std::byte, 4> encoded{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{42}};
    const std::array<encoded_column_value, 1> row{{{.bytes = encoded}}};
    column_store_builder builder(k_identity, schema);
    REQUIRE(builder.append_row(row).has_value());
    test_checksums checksums;
    const auto blob = builder.seal(checksums);
    REQUIRE(blob.has_value());
    const auto view = validate_column_store(*blob, k_identity, schema, checksums);
    REQUIRE(view.has_value());
    REQUIRE(view->value(0, 0)->bytes.size() == encoded.size());
    REQUIRE(view->value(0, 0)->bytes[3] == std::byte{42});
}

TEST_CASE("schema digest commits names and complete SQL type metadata",
          "[component:cache][layer:unit][feature:column_format]") {
    test_checksums checksums;
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

TEST_CASE("row-group row target splits at minus, at, and plus one",
          "[component:cache][layer:unit][feature:column_format]") {
    test_checksums checksums;
    const std::array<column_schema, 0> schema{};
    const std::array<encoded_column_value, 0> values{};
    const auto group_count_for = [&checksums, &schema, &values](std::uint32_t rows) {
        column_store_builder builder(k_identity, schema, {.rows = 4, .value_bytes = 1'024});
        for (std::uint32_t row = 0; row < rows; ++row) {
            REQUIRE(builder.append_row(values).has_value());
        }
        auto blob = builder.seal(checksums);
        REQUIRE(blob.has_value());
        auto view = validate_column_store(*blob, k_identity, schema, checksums);
        REQUIRE(view.has_value());
        return view->row_group_count();
    };

    REQUIRE(group_count_for(3) == 1);
    REQUIRE(group_count_for(4) == 1);
    REQUIRE(group_count_for(5) == 2);
}

TEST_CASE("fixed by-reference storage honors its declared alignment",
          "[component:cache][layer:unit][feature:column_format]") {
    const std::array<column_schema, 1> schema{{
        {
            .name = "fixed_ref",
            .descriptor =
                {
                    .type_oid = 1'000,
                    .typmod = -1,
                    .type_length = 3,
                    .by_value = 0,
                    .type_alignment = 8,
                    .encoding = column_encoding::fixed_by_reference,
                },
            .semantic_id = {.low = 9},
        },
    }};
    const std::array<std::byte, 3> payload{std::byte{1}, std::byte{2}, std::byte{3}};
    const std::array<std::byte, 3> second_payload{std::byte{4}, std::byte{5}, std::byte{6}};
    const std::array<encoded_column_value, 1> row{{{.bytes = payload}}};
    const std::array<encoded_column_value, 1> second_row{{
        {.bytes = second_payload},
    }};
    column_store_builder builder(k_identity, schema);
    REQUIRE(builder.append_row(row).has_value());
    REQUIRE(builder.append_row(second_row).has_value());
    test_checksums checksums;
    const auto blob = builder.seal(checksums);
    REQUIRE(blob.has_value());

    std::size_t cursor = 68;
    const auto index_offset = read_little_endian<std::uint64_t>(*blob, cursor);
    REQUIRE(index_offset.has_value());
    cursor = static_cast<std::size_t>(*index_offset);
    REQUIRE(read_little_endian<std::uint64_t>(*blob, cursor).value() == 1);
    const auto group_offset = read_little_endian<std::uint64_t>(*blob, cursor);
    REQUIRE(group_offset.has_value());
    cursor = static_cast<std::size_t>(*group_offset) + k_row_group_header_bytes +
             4u * sizeof(std::uint64_t);
    const auto values_offset = read_little_endian<std::uint64_t>(*blob, cursor);
    REQUIRE(values_offset.has_value());
    REQUIRE((*group_offset + *values_offset) % 8u == 0);

    const auto view = validate_column_store(*blob, k_identity, schema, checksums);
    REQUIRE(view.has_value());
    REQUIRE(view->value(0, 0)->bytes.size() == payload.size());
    REQUIRE(view->value(0, 0)->bytes[0] == std::byte{1});
    REQUIRE(view->value(1, 0)->bytes[0] == std::byte{4});
}

TEST_CASE("opposite-endian metadata is rejected before allocation",
          "[component:cache][layer:unit][feature:column_format]") {
    auto schema = mixed_schema();
    column_store_builder builder(k_identity, schema);
    append_mixed_rows(builder);
    test_checksums checksums;
    auto blob = builder.seal(checksums);
    REQUIRE(blob.has_value());
    (*blob)[40] = static_cast<std::byte>(static_cast<std::uint8_t>(data_endian::big));
    REQUIRE(validate_column_store(*blob, k_identity, schema, checksums).error() ==
            format_error::endian_mismatch);
}

TEST_CASE("empty and zero-column result shapes remain first-class",
          "[component:cache][layer:unit][feature:column_format]") {
    test_checksums checksums;
    auto schema = mixed_schema();
    column_store_builder empty(k_identity, schema);
    const auto empty_blob = empty.seal(checksums);
    REQUIRE(empty_blob.has_value());
    const auto empty_view = validate_column_store(*empty_blob, k_identity, schema, checksums);
    REQUIRE(empty_view.has_value());
    REQUIRE(empty_view->row_count() == 0);
    REQUIRE(empty_view->row_group_count() == 0);

    const std::array<column_schema, 0> no_schema{};
    column_store_builder zero_columns(k_identity, no_schema, {.rows = 2, .value_bytes = 16});
    const std::array<encoded_column_value, 0> no_values{};
    REQUIRE(zero_columns.append_row(no_values).has_value());
    REQUIRE(zero_columns.append_row(no_values).has_value());
    REQUIRE(zero_columns.append_row(no_values).has_value());
    const auto zero_blob = zero_columns.seal(checksums);
    REQUIRE(zero_blob.has_value());
    const auto zero_view = validate_column_store(*zero_blob, k_identity, no_schema, checksums);
    REQUIRE(zero_view.has_value());
    REQUIRE(zero_view->row_count() == 3);
    REQUIRE(zero_view->column_count() == 0);
    REQUIRE(zero_view->row_group_count() == 2);
}

TEST_CASE("builder rejects schema, arity, NULL, width, and boolean violations",
          "[component:cache][layer:unit][feature:column_format]") {
    test_checksums checksums;
    auto schema = mixed_schema();
    column_store_builder builder(k_identity, schema);

    const std::array<encoded_column_value, 0> no_values{};
    REQUIRE(builder.append_row(no_values).error() == format_error::column_count_mismatch);

    auto nonnullable_schema = schema;
    nonnullable_schema[0].descriptor.flags = 0;
    column_store_builder nonnullable(k_identity, nonnullable_schema);
    const std::array<std::byte, 4> fixed{std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}};
    const std::array<encoded_column_value, 3> null_row{{
        {.is_null = true},
        {.bytes = fixed},
        {.bytes = text_bytes("x")},
    }};
    REQUIRE(nonnullable.append_row(null_row).error() == format_error::invalid_null_bitmap);

    const std::array<std::byte, 1> invalid_boolean{std::byte{2}};
    const std::array<encoded_column_value, 3> bad_boolean{{
        {.bytes = invalid_boolean},
        {.bytes = fixed},
        {.bytes = text_bytes("x")},
    }};
    REQUIRE(builder.append_row(bad_boolean).error() == format_error::invalid_boolean);

    const std::array<std::byte, 3> short_fixed{std::byte{1}, std::byte{2}, std::byte{3}};
    const std::array<std::byte, 1> valid_boolean{std::byte{1}};
    const std::array<encoded_column_value, 3> bad_fixed{{
        {.bytes = valid_boolean},
        {.bytes = short_fixed},
        {.bytes = text_bytes("x")},
    }};
    REQUIRE(builder.append_row(bad_fixed).error() == format_error::invalid_fixed_width);

    auto unsupported_schema = schema;
    unsupported_schema[2].descriptor.encoding = column_encoding::dictionary;
    column_store_builder unsupported(k_identity, unsupported_schema);
    REQUIRE(unsupported.seal(checksums).error() == format_error::unsupported_encoding);

    auto invalid_alignment_schema = schema;
    invalid_alignment_schema[1].descriptor.type_alignment = 3;
    column_store_builder invalid_alignment(k_identity, invalid_alignment_schema);
    REQUIRE(invalid_alignment.seal(checksums).error() == format_error::invalid_schema);
}

TEST_CASE("every stored byte is covered by validation or a checksum",
          "[component:cache][layer:unit][feature:column_format]") {
    auto schema = mixed_schema();
    column_store_builder builder(k_identity, schema, {.rows = 2, .value_bytes = 1'024});
    append_mixed_rows(builder);
    test_checksums checksums;
    const auto sealed = builder.seal(checksums);
    REQUIRE(sealed.has_value());

    for (std::size_t index = 0; index < sealed->size(); ++index) {
        auto corrupt = *sealed;
        corrupt[index] ^= std::byte{0x01};
        INFO("corrupt byte offset " << index);
        REQUIRE_FALSE(validate_column_store(corrupt, k_identity, schema, checksums).has_value());
    }
}

TEST_CASE("all truncation points, identities, and unavailable checksums fail closed",
          "[component:cache][layer:unit][feature:column_format]") {
    auto schema = mixed_schema();
    column_store_builder builder(k_identity, schema, {.rows = 2, .value_bytes = 1'024});
    append_mixed_rows(builder);
    test_checksums checksums;
    const auto sealed = builder.seal(checksums);
    REQUIRE(sealed.has_value());

    for (std::size_t length = 0; length < sealed->size(); ++length) {
        INFO("truncated length " << length);
        REQUIRE_FALSE(
            validate_column_store(
                std::span<const std::byte>(*sealed).first(length), k_identity, schema, checksums)
                .has_value());
    }

    auto wrong_identity = k_identity;
    ++wrong_identity.boot_nonce;
    REQUIRE(validate_column_store(*sealed, wrong_identity, schema, checksums).error() ==
            format_error::identity_mismatch);

    auto wrong_schema = schema;
    wrong_schema[0].name = "wrong_name";
    REQUIRE(validate_column_store(*sealed, k_identity, wrong_schema, checksums).error() ==
            format_error::schema_mismatch);

    unavailable_checksums unavailable;
    REQUIRE(builder.seal(unavailable).error() == format_error::checksum_unavailable);
    REQUIRE(validate_column_store(*sealed, k_identity, schema, unavailable).error() ==
            format_error::checksum_unavailable);
}

TEST_CASE("dictionary and block-reference policies fail safely",
          "[component:cache][layer:unit][feature:column_format]") {
    REQUIRE(dictionary_is_beneficial(100, 30, 20));
    REQUIRE_FALSE(dictionary_is_beneficial(50, 30, 20));
    REQUIRE_FALSE(dictionary_is_beneficial((std::numeric_limits<std::uint64_t>::max)(),
                                           (std::numeric_limits<std::uint64_t>::max)(),
                                           1));

    const block_reference valid{
        .tier = block_backing_tier::file,
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
    invalid.tier = block_backing_tier::dsa;
    REQUIRE(validate_block_reference(invalid, 150));
    invalid.flags = 1;
    REQUIRE_FALSE(validate_block_reference(invalid, 150));
    invalid = valid;
    invalid.length = 0;
    REQUIRE_FALSE(validate_block_reference(invalid, 150));
}
