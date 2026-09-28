#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

import bioseq.cache.core;

namespace {

using bioseq::cache::copy_dependency_vector_entries;
using bioseq::cache::database_definition_guard_entry_valid;
using bioseq::cache::database_guard_entry_valid;
using bioseq::cache::database_guard_requires_completion;
using bioseq::cache::database_write_guard_entry_valid;
using bioseq::cache::dependency_completion_handshake_valid;
using bioseq::cache::dependency_fingerprint_material_bytes;
using bioseq::cache::dependency_kind;
using bioseq::cache::dependency_vector_entry;
using bioseq::cache::dependency_vector_envelope_bytes;
using bioseq::cache::dependency_vector_envelope_error;
using bioseq::cache::dependency_vector_header;
using bioseq::cache::dependency_vector_payload_checksum;
using bioseq::cache::initialize_dependency_fingerprint_material;
using bioseq::cache::initialize_dependency_vector_envelope;
using bioseq::cache::k_dependency_vector_entry_bytes;
using bioseq::cache::k_dependency_vector_header_bytes;
using bioseq::cache::k_maximum_dependencies_per_generation;
using bioseq::cache::physical_identity_kind;
using bioseq::cache::validate_dependency_vector_envelope;
using bioseq::cache::write_observability;

void write_u32(std::span<std::byte> output, std::size_t offset, std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        output[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

void write_u64(std::span<std::byte> output, std::size_t offset, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        output[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

void refresh_payload_checksum(std::span<std::byte> envelope) noexcept {
    const std::span<const std::byte> payload = envelope.subspan(k_dependency_vector_header_bytes);
    write_u64(envelope, 32, dependency_vector_payload_checksum(payload));
}

[[nodiscard]] dependency_vector_entry entry(std::uint32_t object_oid) noexcept {
    return dependency_vector_entry{
        .version =
            {
                .key =
                    {
                        .database_oid = 7,
                        .class_oid = 1,
                        .object_oid = object_oid,
                        .kind = dependency_kind::relation_content,
                    },
                .committed_epoch = static_cast<std::uint64_t>(object_oid) + 10U,
                .physical_identity =
                    {
                        .kind = physical_identity_kind::none,
                    },
                .observability = write_observability::executor_utility_only,
            },
        .record_generation = static_cast<std::uint64_t>(object_oid) + 100U,
    };
}

[[nodiscard]] dependency_vector_entry database_write_guard() noexcept {
    return dependency_vector_entry{
        .version =
            {
                .key =
                    {
                        .database_oid = 7,
                        .object_oid = 7,
                        .kind = dependency_kind::database_write_epoch,
                    },
                .committed_epoch = 91,
                .physical_identity =
                    {
                        .kind = physical_identity_kind::none,
                    },
                .observability = write_observability::database_wide_fallback,
            },
        .record_generation = 93,
    };
}

[[nodiscard]] dependency_vector_entry database_definition_guard() noexcept {
    dependency_vector_entry result = database_write_guard();
    result.version.key.kind = dependency_kind::database_definition_epoch;
    return result;
}

static_assert(std::is_trivially_copyable_v<dependency_vector_entry>);
static_assert(std::is_standard_layout_v<dependency_vector_entry>);
static_assert(std::is_trivially_copyable_v<dependency_vector_header>);
static_assert(std::is_standard_layout_v<dependency_vector_header>);

TEST_CASE("dependency vector envelope validates canonical pointer-free bytes",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
    const std::vector entries{entry(11), entry(12), entry(13)};
    std::vector<std::byte> bytes(dependency_vector_envelope_bytes(entries.size()));
    std::fill(bytes.begin(), bytes.end(), std::byte{0xa5});

    const auto initialized = initialize_dependency_vector_envelope(bytes, entries);
    REQUIRE(initialized);
    REQUIRE(initialized.header.entry_count == entries.size());
    REQUIRE(initialized.header.total_bytes == bytes.size());
    REQUIRE(initialized.header.payload_checksum != 0);
    REQUIRE(bytes[k_dependency_vector_header_bytes + 18] == std::byte{});
    REQUIRE(bytes[k_dependency_vector_header_bytes + 19] == std::byte{});
    REQUIRE(bytes[k_dependency_vector_header_bytes + 20] == std::byte{});
    REQUIRE(bytes[k_dependency_vector_header_bytes + 76] == std::byte{});

    const auto validated = validate_dependency_vector_envelope(bytes);
    REQUIRE(validated);
    REQUIRE(validated.header.entry_count == entries.size());
    REQUIRE(validated.header.payload_checksum == initialized.header.payload_checksum);
    std::vector<dependency_vector_entry> decoded(entries.size());
    REQUIRE(copy_dependency_vector_entries(bytes, decoded));
    REQUIRE(decoded == entries);
}

TEST_CASE("dependency vector envelope detects shape checksum and semantics",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
    SECTION("payload corruption") {
        const std::vector entries{entry(11), entry(12)};
        std::vector<std::byte> bytes(dependency_vector_envelope_bytes(entries.size()));
        REQUIRE(initialize_dependency_vector_envelope(bytes, entries));
        bytes.back() ^= std::byte{1};
        REQUIRE(validate_dependency_vector_envelope(bytes).error ==
                dependency_vector_envelope_error::checksum_mismatch);
    }

    SECTION("forged count") {
        const std::vector entries{entry(11)};
        std::vector<std::byte> bytes(dependency_vector_envelope_bytes(entries.size()));
        REQUIRE(initialize_dependency_vector_envelope(bytes, entries));
        write_u32(bytes, 16, k_maximum_dependencies_per_generation + 1U);
        REQUIRE(validate_dependency_vector_envelope(bytes).error ==
                dependency_vector_envelope_error::count_exceeded);
    }

    SECTION("nonzero reserved entry bytes with a matching checksum") {
        const std::vector entries{entry(11)};
        std::vector<std::byte> bytes(dependency_vector_envelope_bytes(entries.size()));
        REQUIRE(initialize_dependency_vector_envelope(bytes, entries));
        bytes[k_dependency_vector_header_bytes + 20] = std::byte{1};
        refresh_payload_checksum(bytes);
        REQUIRE(validate_dependency_vector_envelope(bytes).error ==
                dependency_vector_envelope_error::invalid_entry);
    }

    SECTION("nonzero canonical physical tail with a matching checksum") {
        const std::vector entries{entry(11)};
        std::vector<std::byte> bytes(dependency_vector_envelope_bytes(entries.size()));
        REQUIRE(initialize_dependency_vector_envelope(bytes, entries));
        bytes[k_dependency_vector_header_bytes + 40] = std::byte{1};
        refresh_payload_checksum(bytes);
        REQUIRE(validate_dependency_vector_envelope(bytes).error ==
                dependency_vector_envelope_error::invalid_entry);
    }

    SECTION("invalid ABA generation") {
        std::vector entries{entry(11)};
        entries.front().record_generation = 0;
        std::vector<std::byte> bytes(dependency_vector_envelope_bytes(entries.size()));
        REQUIRE(initialize_dependency_vector_envelope(bytes, entries).error ==
                dependency_vector_envelope_error::invalid_entry);
    }

    SECTION("duplicate and unsorted keys") {
        const std::vector duplicate{entry(11), entry(11)};
        std::vector<std::byte> duplicate_bytes(dependency_vector_envelope_bytes(duplicate.size()));
        REQUIRE(initialize_dependency_vector_envelope(duplicate_bytes, duplicate).error ==
                dependency_vector_envelope_error::non_canonical);

        const std::vector unsorted{entry(12), entry(11)};
        std::vector<std::byte> unsorted_bytes(dependency_vector_envelope_bytes(unsorted.size()));
        REQUIRE(initialize_dependency_vector_envelope(unsorted_bytes, unsorted).error ==
                dependency_vector_envelope_error::non_canonical);
    }
}

TEST_CASE("dependency vector cap is exact and never truncates",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
    std::vector<dependency_vector_entry> maximum;
    maximum.reserve(k_maximum_dependencies_per_generation);
    for (std::uint32_t index = 0; index < k_maximum_dependencies_per_generation; ++index) {
        maximum.push_back(entry(index + 1U));
    }
    std::vector<std::byte> maximum_bytes(dependency_vector_envelope_bytes(maximum.size()));
    REQUIRE(maximum_bytes.size() ==
            k_dependency_vector_header_bytes +
                static_cast<std::size_t>(k_maximum_dependencies_per_generation) *
                    k_dependency_vector_entry_bytes);
    REQUIRE(initialize_dependency_vector_envelope(maximum_bytes, maximum));
    REQUIRE(validate_dependency_vector_envelope(maximum_bytes));

    maximum.push_back(entry(k_maximum_dependencies_per_generation + 1U));
    REQUIRE(dependency_vector_envelope_bytes(maximum.size()) == 0);
    std::span<std::byte> no_storage{};
    REQUIRE(initialize_dependency_vector_envelope(no_storage, maximum).error ==
            dependency_vector_envelope_error::count_exceeded);
}

TEST_CASE("dependency fingerprint material includes the dependency epoch guard",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
    const std::array entries{entry(11), entry(12)};
    std::vector<std::byte> envelope(dependency_vector_envelope_bytes(entries.size()));
    REQUIRE(initialize_dependency_vector_envelope(envelope, entries));

    std::vector<std::byte> plan_only(dependency_fingerprint_material_bytes(entries.size(), false));
    REQUIRE(initialize_dependency_fingerprint_material(
        plan_only, envelope, std::span<const dependency_vector_entry>{}));
    REQUIRE(plan_only == envelope);

    const dependency_vector_entry guard = database_write_guard();
    const std::array guard_span{guard};
    std::vector<std::byte> guarded(dependency_fingerprint_material_bytes(entries.size(), true));
    REQUIRE(initialize_dependency_fingerprint_material(guarded, envelope, guard_span));
    REQUIRE(guarded.size() == envelope.size() + k_dependency_vector_entry_bytes);
    REQUIRE(std::equal(envelope.begin(), envelope.end(), guarded.begin()));

    auto newer_epoch = guard;
    ++newer_epoch.version.committed_epoch;
    const std::array newer_epoch_span{newer_epoch};
    std::vector<std::byte> newer_epoch_material(guarded.size());
    REQUIRE(initialize_dependency_fingerprint_material(
        newer_epoch_material, envelope, newer_epoch_span));
    REQUIRE(newer_epoch_material != guarded);

    auto newer_record = guard;
    ++newer_record.record_generation;
    const std::array newer_record_span{newer_record};
    std::vector<std::byte> newer_record_material(guarded.size());
    REQUIRE(initialize_dependency_fingerprint_material(
        newer_record_material, envelope, newer_record_span));
    REQUIRE(newer_record_material != guarded);

    const std::array definition_span{database_definition_guard()};
    std::vector<std::byte> definition_material(guarded.size());
    REQUIRE(
        initialize_dependency_fingerprint_material(definition_material, envelope, definition_span));
    REQUIRE(definition_material != guarded);
    auto newer_definition = database_definition_guard();
    ++newer_definition.version.committed_epoch;
    const std::array newer_definition_span{newer_definition};
    std::vector<std::byte> newer_definition_material(guarded.size());
    REQUIRE(initialize_dependency_fingerprint_material(
        newer_definition_material, envelope, newer_definition_span));
    REQUIRE(newer_definition_material != definition_material);

    // Changing the record generation keeps the fingerprint input the same size.
    REQUIRE(newer_record_material.size() == guarded.size());
}

TEST_CASE("reserved database guards have kind-derived completion semantics",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
    const dependency_vector_entry write = database_write_guard();
    const dependency_vector_entry definition = database_definition_guard();
    CHECK(database_write_guard_entry_valid(write));
    CHECK_FALSE(database_definition_guard_entry_valid(write));
    CHECK(database_definition_guard_entry_valid(definition));
    CHECK_FALSE(database_write_guard_entry_valid(definition));
    CHECK(database_guard_entry_valid(write));
    CHECK(database_guard_entry_valid(definition));
    CHECK(database_guard_requires_completion(write));
    CHECK_FALSE(database_guard_requires_completion(definition));
}

TEST_CASE("snapshot binding and continuing completion are independent",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
    constexpr std::uint64_t old_producer_completion = 41;
    constexpr std::uint64_t fresh_binding = 52;

    SECTION("zero binding is never an attestation") {
        CHECK_FALSE(dependency_completion_handshake_valid(0, 0, fresh_binding, 0));
    }

    SECTION("a copied snapshot accepts only its validated fresh binding") {
        CHECK(dependency_completion_handshake_valid(0, fresh_binding, fresh_binding, 0));
        CHECK_FALSE(dependency_completion_handshake_valid(0, fresh_binding, fresh_binding + 1U, 0));
        CHECK_FALSE(dependency_completion_handshake_valid(
            old_producer_completion, fresh_binding, fresh_binding, 0));
    }

    SECTION("complete validation ignores the producer raw counter") {
        CHECK(old_producer_completion != fresh_binding);
        CHECK(dependency_completion_handshake_valid(0, fresh_binding, fresh_binding, 0));
    }

    SECTION("conservative in-flight validation retains raw equality") {
        CHECK_FALSE(dependency_completion_handshake_valid(
            0, fresh_binding, fresh_binding, old_producer_completion));
        CHECK(
            dependency_completion_handshake_valid(0, fresh_binding, fresh_binding, fresh_binding));
    }
}

} // namespace
