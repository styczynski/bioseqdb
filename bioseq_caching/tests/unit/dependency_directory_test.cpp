#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

import bioseq.cache.core;

using namespace bioseq::cache;

namespace {

template <class p_value>
[[nodiscard]] p_value* object_at(std::span<std::byte> region, std::uint32_t offset) noexcept {
    if (offset > region.size() || sizeof(p_value) > region.size() - offset) {
        return nullptr;
    }
    return reinterpret_cast<p_value*>(region.data() + offset);
}

[[nodiscard]] dependency_key make_dependency_key(std::uint32_t ordinal) noexcept {
    return dependency_key{
        .database_oid = 11,
        .class_oid = 1'259,
        .object_oid = ordinal + 1U,
        .kind = dependency_kind::relation_content,
    };
}

[[nodiscard]] physical_identity make_physical_identity(std::uint8_t marker) noexcept {
    physical_identity identity{
        .kind = physical_identity_kind::provider_opaque,
        .length = 1,
    };
    identity.bytes[0] = static_cast<std::byte>(marker);
    return identity;
}

[[nodiscard]] dependency_reference_request
reference_request(std::uint32_t ordinal,
                  dependency_reference_kind kind = dependency_reference_kind::owner,
                  physical_identity identity = {}) noexcept {
    return dependency_reference_request{
        .key = make_dependency_key(ordinal),
        .physical_identity = identity,
        .observability = write_observability::provider_complete,
        .kind = kind,
    };
}

[[nodiscard]] dependency_transition_resolution
make_transition_resolution(dependency_transition_outcome outcome,
                           const dependency_reference& reference) noexcept {
    return dependency_transition_resolution{
        .outcome = outcome,
        .physical_identity_action = dependency_physical_identity_action::preserve,
        .physical_identity = reference.physical_identity,
    };
}

struct callback_harness {
    std::span<std::byte> region{};
    std::uint64_t sequence = 100;
    std::uint32_t wake_calls = 0;
    std::array<dependency_terminal_stage, 16> terminal_stages{};
    std::array<dependency_epoch_snapshot, 16> terminal_snapshots{};
    std::size_t terminal_stage_count = 0;
    bool fail_next_sequence = false;
    bool fail_wake = false;
    bool corrupt_retiring_record = false;

    static std::uint64_t next_sequence(void* opaque) noexcept {
        auto& self = *static_cast<callback_harness*>(opaque);
        if (self.corrupt_retiring_record) {
            auto* header = object_at<dependency_directory_header>(self.region, 0);
            if (header != nullptr) {
                for (std::uint32_t partition_index = 0;
                     partition_index < header->config.partition_count;
                     ++partition_index) {
                    auto* partition = object_at<dependency_directory_partition>(
                        self.region,
                        header->layout.partitions_offset +
                            partition_index * sizeof(dependency_directory_partition));
                    if (partition == nullptr) {
                        continue;
                    }
                    for (std::uint32_t record_index = 0;
                         record_index < partition->physical_capacity;
                         ++record_index) {
                        auto* record = object_at<shared_dependency_epoch>(
                            self.region,
                            partition->record_offset +
                                record_index * sizeof(shared_dependency_epoch));
                        if (record != nullptr &&
                            record->slot_state == directory_slot_state::retiring) {
                            ++record->record_generation;
                            self.corrupt_retiring_record = false;
                            break;
                        }
                    }
                    if (!self.corrupt_retiring_record) {
                        break;
                    }
                }
            }
        }
        if (self.fail_next_sequence) {
            self.fail_next_sequence = false;
            return 0;
        }
        return ++self.sequence;
    }

    static bool wake_partition(void* opaque, std::uint32_t) noexcept {
        auto& self = *static_cast<callback_harness*>(opaque);
        ++self.wake_calls;
        return !self.fail_wake;
    }

    static void observe_terminal_stage(void* opaque,
                                       dependency_terminal_stage stage,
                                       const dependency_epoch_snapshot& snapshot) noexcept {
        auto& self = *static_cast<callback_harness*>(opaque);
        if (self.terminal_stage_count < self.terminal_stages.size()) {
            self.terminal_stages[self.terminal_stage_count] = stage;
            self.terminal_snapshots[self.terminal_stage_count] = snapshot;
            ++self.terminal_stage_count;
        }
    }

    [[nodiscard]] dependency_directory_callbacks callbacks() noexcept {
        return dependency_directory_callbacks{
            .context = this,
            .lock_mode = dependency_directory_lock_mode::already_locked,
            .next_sequence = &callback_harness::next_sequence,
            .wake_partition = &callback_harness::wake_partition,
            .observe_terminal_stage = &callback_harness::observe_terminal_stage,
        };
    }
};

class directory_fixture {
  public:
    explicit directory_fixture(dependency_directory_config config = {}) {
        const dependency_directory_preload_result preload =
            make_dependency_directory_preload_request(config);
        if (!preload) {
            throw std::runtime_error("invalid dependency directory fixture");
        }
        request = preload.request;
        if (request.layout.total_bytes % sizeof(std::uint64_t) != 0) {
            throw std::runtime_error("unaligned dependency directory layout");
        }
        words.resize(request.layout.total_bytes / sizeof(std::uint64_t));
        const dependency_directory_error initialized =
            initialize_dependency_directory(bytes(), request);
        if (initialized != dependency_directory_error::none) {
            throw std::runtime_error("dependency directory initialization failed");
        }
        harness.region = bytes();
        const dependency_directory_attach_result attached =
            attach_dependency_directory(bytes(), harness.callbacks());
        if (!attached) {
            throw std::runtime_error("dependency directory attachment failed");
        }
        directory = attached.directory;
    }

    [[nodiscard]] std::span<std::byte> bytes() noexcept {
        return {
            reinterpret_cast<std::byte*>(words.data()),
            request.layout.total_bytes,
        };
    }

    [[nodiscard]] dependency_directory_header* header() noexcept {
        return object_at<dependency_directory_header>(bytes(), 0);
    }

    [[nodiscard]] dependency_directory_partition* partition(std::uint32_t index) noexcept {
        return object_at<dependency_directory_partition>(
            bytes(),
            request.layout.partitions_offset + index * sizeof(dependency_directory_partition));
    }

    [[nodiscard]] shared_dependency_epoch* record(const dependency_reference& reference) noexcept {
        return object_at<shared_dependency_epoch>(bytes(), reference.record_offset);
    }

    void aim_reclaim_at(const dependency_reference& reference) noexcept {
        dependency_directory_partition* value = partition(reference.partition_index);
        value->reclaim_cursor =
            (reference.record_offset - value->record_offset) / sizeof(shared_dependency_epoch);
    }

    dependency_directory_preload_request request{};
    std::vector<std::uint64_t> words{};
    callback_harness harness{};
    dependency_directory_attachment directory{};
};

struct lock_trace {
    std::array<bool, k_maximum_dependency_directory_partitions> held{};
    std::uint32_t acquires = 0;
    std::uint32_t releases = 0;
    bool contract_failed = false;

    static void acquire(void* opaque, std::uint32_t partition) noexcept {
        auto& self = *static_cast<lock_trace*>(opaque);
        if (partition >= self.held.size() || self.held[partition]) {
            self.contract_failed = true;
            return;
        }
        self.held[partition] = true;
        ++self.acquires;
    }

    static void release(void* opaque, std::uint32_t partition) noexcept {
        auto& self = *static_cast<lock_trace*>(opaque);
        if (partition >= self.held.size() || !self.held[partition]) {
            self.contract_failed = true;
            return;
        }
        self.held[partition] = false;
        ++self.releases;
    }
};

TEST_CASE("dependency directory sizing clamps logical and physical budgets",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
    CHECK(dependency_directory_logical_capacity_for_backends(0) == 4'096);
    CHECK(dependency_directory_logical_capacity_for_backends(7) == 4'096);
    CHECK(dependency_directory_logical_capacity_for_backends(8) == 4'096);
    CHECK(dependency_directory_logical_capacity_for_backends(9) == 4'608);
    CHECK(dependency_directory_logical_capacity_for_backends(512) == 262'144);
    CHECK(dependency_directory_logical_capacity_for_backends(
              (std::numeric_limits<std::uint32_t>::max)()) == 262'144);

    CHECK(dependency_directory_physical_capacity(4'095) == 0);
    CHECK(dependency_directory_physical_capacity(4'096) == 8'192);
    CHECK(dependency_directory_physical_capacity(4'097) == 16'384);
    CHECK(dependency_directory_physical_capacity(131'072) == 262'144);
    CHECK(dependency_directory_physical_capacity(262'144) == 524'288);
    CHECK(dependency_directory_physical_capacity(262'145) == 0);
}

TEST_CASE("dependency directory rejects every bounded config violation",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
    dependency_directory_config config{};

    config.logical_capacity = 4'095;
    CHECK_FALSE(make_dependency_directory_preload_request(config));
    config.logical_capacity = 262'145;
    CHECK_FALSE(make_dependency_directory_preload_request(config));

    config = {};
    config.partition_count = 0;
    CHECK_FALSE(make_dependency_directory_preload_request(config));
    config.partition_count = 3;
    CHECK_FALSE(make_dependency_directory_preload_request(config));
    config.partition_count = 2'048;
    CHECK_FALSE(make_dependency_directory_preload_request(config));

    config = {};
    config.reclaim_scan_limit = 0;
    CHECK_FALSE(make_dependency_directory_preload_request(config));
    config.reclaim_scan_limit = k_maximum_dependency_vector_size + 1U;
    CHECK_FALSE(make_dependency_directory_preload_request(config));

    config = {};
    config.pressure_percent = 69;
    CHECK_FALSE(make_dependency_directory_preload_request(config));
    config = {};
    config.backend_reference_limit = 0;
    CHECK_FALSE(make_dependency_directory_preload_request(config));
    config.backend_reference_limit = k_maximum_dependency_reference_count + 1U;
    CHECK_FALSE(make_dependency_directory_preload_request(config));
    config = {};
    config.generation_reference_limit = 0;
    CHECK_FALSE(make_dependency_directory_preload_request(config));
    config.generation_reference_limit = k_maximum_dependency_reference_count + 1U;
    CHECK_FALSE(make_dependency_directory_preload_request(config));
    config = {};
    config.reserved = 1;
    CHECK_FALSE(make_dependency_directory_preload_request(config));
}

TEST_CASE("valid partition quotas are exact nonzero and below the backstop",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
    const std::array<dependency_directory_config, 3> configs{
        dependency_directory_config{
            .logical_capacity = 4'096,
            .partition_count = 1,
        },
        dependency_directory_config{
            .logical_capacity = 4'097,
            .partition_count = 16,
        },
        dependency_directory_config{
            .logical_capacity = 262'144,
            .partition_count = 1'024,
        },
    };

    for (const dependency_directory_config& config : configs) {
        directory_fixture fixture{config};
        std::uint64_t quota_sum = 0;
        for (std::uint32_t partition_index = 0; partition_index < config.partition_count;
             ++partition_index) {
            const dependency_directory_partition* partition = fixture.partition(partition_index);
            REQUIRE(partition != nullptr);
            CHECK(partition->logical_capacity != 0);
            CHECK(static_cast<std::uint64_t>(partition->logical_capacity) * 2U <=
                  partition->physical_capacity);
            CHECK(static_cast<std::uint64_t>(partition->logical_capacity) * 100U <
                  static_cast<std::uint64_t>(partition->physical_capacity) * 70U);
            quota_sum += partition->logical_capacity;
        }
        CHECK(quota_sum == config.logical_capacity);
        CHECK(fixture.request.layout.physical_capacity >= config.logical_capacity * 2U);
    }
}

TEST_CASE("dependency directory requires an exact aligned region",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
    const dependency_directory_preload_result preload = make_dependency_directory_preload_request();
    REQUIRE(preload);
    const std::uint32_t total = preload.request.layout.total_bytes;
    std::vector<std::uint64_t> words(total / sizeof(std::uint64_t) + 2U);
    std::span<std::byte> storage{
        reinterpret_cast<std::byte*>(words.data()),
        words.size() * sizeof(std::uint64_t),
    };

    CHECK(initialize_dependency_directory(storage.first(total - 1U), preload.request) ==
          dependency_directory_error::region_too_small);
    CHECK(initialize_dependency_directory(storage.first(total + sizeof(std::uint64_t)),
                                          preload.request) ==
          dependency_directory_error::region_size_mismatch);
    CHECK(initialize_dependency_directory(storage.subspan(1, total), preload.request) ==
          dependency_directory_error::region_misaligned);
    REQUIRE(initialize_dependency_directory(storage.first(total), preload.request) ==
            dependency_directory_error::none);

    callback_harness harness{.region = storage.first(total)};
    const dependency_directory_attach_result oversized = attach_dependency_directory(
        storage.first(total + sizeof(std::uint64_t)), harness.callbacks());
    CHECK(oversized.error == dependency_directory_error::region_size_mismatch);
}

TEST_CASE("attach validates the exact layout while holding each lock",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
    const dependency_directory_preload_result preload = make_dependency_directory_preload_request();
    REQUIRE(preload);
    std::vector<std::uint64_t> words(preload.request.layout.total_bytes / sizeof(std::uint64_t));
    std::span<std::byte> region{
        reinterpret_cast<std::byte*>(words.data()),
        preload.request.layout.total_bytes,
    };
    REQUIRE(initialize_dependency_directory(region, preload.request) ==
            dependency_directory_error::none);

    lock_trace trace{};
    dependency_directory_callbacks callbacks{
        .context = &trace,
        .lock_mode = dependency_directory_lock_mode::acquire_release,
        .acquire_partition = &lock_trace::acquire,
        .release_partition = &lock_trace::release,
    };
    const dependency_directory_attach_result attached =
        attach_dependency_directory(region, callbacks);
    REQUIRE(attached);
    CHECK(trace.acquires == preload.request.config.partition_count);
    CHECK(trace.releases == trace.acquires);
    CHECK_FALSE(trace.contract_failed);

    dependency_directory_callbacks implicit_no_lock{};
    CHECK(attach_dependency_directory(region, implicit_no_lock).error ==
          dependency_directory_error::invalid_argument);
    dependency_directory_callbacks malformed{
        .lock_mode = dependency_directory_lock_mode::already_locked,
        .acquire_partition = &lock_trace::acquire,
    };
    CHECK(attach_dependency_directory(region, malformed).error ==
          dependency_directory_error::invalid_argument);
}

TEST_CASE("attach rejects ABI layout partition and record corruption",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
    SECTION("ABI") {
        directory_fixture fixture{};
        fixture.header()->abi_revision += 1U;
        CHECK(attach_dependency_directory(fixture.bytes(), fixture.harness.callbacks()).error ==
              dependency_directory_error::abi_mismatch);
    }
    SECTION("immutable layout") {
        directory_fixture fixture{};
        ++fixture.header()->layout.physical_capacity;
        CHECK(attach_dependency_directory(fixture.bytes(), fixture.harness.callbacks()).error ==
              dependency_directory_error::corrupt_layout);
    }
    SECTION("partition quota") {
        directory_fixture fixture{};
        ++fixture.partition(0)->logical_capacity;
        CHECK(attach_dependency_directory(fixture.bytes(), fixture.harness.callbacks()).error ==
              dependency_directory_error::corrupt_layout);
    }
    SECTION("transient retiring state") {
        directory_fixture fixture{};
        auto acquired = acquire_dependency_reference(
            fixture.directory, reference_request(1), fixture.harness.callbacks());
        REQUIRE(acquired);
        fixture.record(acquired.reference)->slot_state = directory_slot_state::retiring;
        CHECK(attach_dependency_directory(fixture.bytes(), fixture.harness.callbacks()).error ==
              dependency_directory_error::corrupt_directory);
    }
    SECTION("bounded counter") {
        directory_fixture fixture{};
        auto acquired = acquire_dependency_reference(
            fixture.directory, reference_request(2), fixture.harness.callbacks());
        REQUIRE(acquired);
        fixture.record(acquired.reference)->owner_ref_count =
            fixture.header()->config.backend_reference_limit + 1U;
        CHECK(attach_dependency_directory(fixture.bytes(), fixture.harness.callbacks()).error ==
              dependency_directory_error::corrupt_directory);
    }
}

TEST_CASE("full equality reuses records and refcounts stop at configured caps",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
    dependency_directory_config config{};
    config.backend_reference_limit = 2;
    config.generation_reference_limit = 1;
    directory_fixture fixture{config};

    auto first = acquire_dependency_reference(
        fixture.directory, reference_request(10), fixture.harness.callbacks());
    REQUIRE(first);
    CHECK(first.outcome == dependency_admission_outcome::inserted);
    auto second = acquire_dependency_reference(
        fixture.directory, reference_request(10), fixture.harness.callbacks());
    REQUIRE(second);
    CHECK(second.outcome == dependency_admission_outcome::existing);
    CHECK(second.reference.record_offset == first.reference.record_offset);
    CHECK(second.reference.record_generation == first.reference.record_generation);
    CHECK(second.snapshot.owner_ref_count == 2);

    const auto overflow = acquire_dependency_reference(
        fixture.directory, reference_request(10), fixture.harness.callbacks());
    CHECK(overflow.error == dependency_directory_error::counter_overflow);
    CHECK(overflow.coarse_fallback_required);

    auto generation =
        acquire_dependency_reference(fixture.directory,
                                     reference_request(10, dependency_reference_kind::generation),
                                     fixture.harness.callbacks());
    REQUIRE(generation);
    const auto generation_overflow =
        acquire_dependency_reference(fixture.directory,
                                     reference_request(10, dependency_reference_kind::generation),
                                     fixture.harness.callbacks());
    CHECK(generation_overflow.error == dependency_directory_error::counter_overflow);

    dependency_reference_request mismatch = reference_request(10);
    mismatch.observability = write_observability::executor_utility_only;
    const auto observability =
        acquire_dependency_reference(fixture.directory, mismatch, fixture.harness.callbacks());
    CHECK(observability.error == dependency_directory_error::observability_mismatch);
    CHECK(observability.coarse_fallback_required);

    dependency_reference_request reserved_request = reference_request(11);
    reserved_request.reserved[0] = 1;
    CHECK(acquire_dependency_reference(
              fixture.directory, reserved_request, fixture.harness.callbacks())
              .error == dependency_directory_error::invalid_argument);

    dependency_directory_callbacks reserved_callbacks = fixture.harness.callbacks();
    reserved_callbacks.reserved[0] = 1;
    CHECK(acquire_dependency_reference(fixture.directory, reference_request(12), reserved_callbacks)
              .error == dependency_directory_error::invalid_argument);
}

TEST_CASE("logical quota rejects before the physical 70 percent backstop",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
    dependency_directory_config config{};
    config.partition_count = 1;
    directory_fixture fixture{config};
    std::vector<dependency_reference> references;
    references.reserve(config.logical_capacity);
    for (std::uint32_t ordinal = 0; ordinal < config.logical_capacity; ++ordinal) {
        const auto acquired = acquire_dependency_reference(
            fixture.directory, reference_request(ordinal), fixture.harness.callbacks());
        REQUIRE(acquired);
        references.push_back(acquired.reference);
    }

    const dependency_directory_partition* partition = fixture.partition(0);
    REQUIRE(partition != nullptr);
    CHECK(partition->occupied_count == partition->logical_capacity);
    CHECK(static_cast<std::uint64_t>(partition->occupied_count) * 100U <
          static_cast<std::uint64_t>(partition->physical_capacity) * 70U);

    const auto rejected =
        acquire_dependency_reference(fixture.directory,
                                     reference_request(config.logical_capacity + 1U),
                                     fixture.harness.callbacks());
    CHECK(rejected.error == dependency_directory_error::pressure_limit);
    CHECK(rejected.pressure_observed);
    CHECK(rejected.coarse_fallback_required);
    CHECK(rejected.outcome == dependency_admission_outcome::pressure_fallback);

    const auto stats =
        read_dependency_directory_stats(fixture.directory, fixture.harness.callbacks());
    REQUIRE(stats);
    CHECK(stats.stats.logical_capacity == 4'096);
    CHECK(stats.stats.physical_capacity == 8'192);
    CHECK(stats.stats.pressure_event_count == 1);
    CHECK(stats.stats.pressure_fallback_count == 1);
    CHECK(stats.stats.reclaim_attempt_count == 0);
}

TEST_CASE("RETIRING reclaim preserves ABA identity and rolls back faults",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
    SECTION("successful reclaim") {
        directory_fixture fixture{};
        auto acquired = acquire_dependency_reference(
            fixture.directory, reference_request(100), fixture.harness.callbacks());
        REQUIRE(acquired);
        const dependency_reference stale_copy = acquired.reference;
        REQUIRE(release_dependency_reference(
            fixture.directory, acquired.reference, fixture.harness.callbacks()));
        fixture.aim_reclaim_at(stale_copy);
        const auto reclaimed = reclaim_dependency_partition(
            fixture.directory, stale_copy.partition_index, 1, fixture.harness.callbacks());
        REQUIRE(reclaimed);
        CHECK(reclaimed.reclaimed_count == 1);
        CHECK(fixture.record(stale_copy)->slot_state == directory_slot_state::tombstone);
        CHECK(fixture.record(stale_copy)->record_generation == stale_copy.record_generation);
        CHECK(
            snapshot_dependency(fixture.directory, stale_copy, fixture.harness.callbacks()).error ==
            dependency_directory_error::stale_reference);
        CHECK(deep_validate_dependency_directory(fixture.directory, fixture.harness.callbacks()) ==
              dependency_directory_error::none);
    }

    SECTION("sequence failure restores OCCUPIED") {
        directory_fixture fixture{};
        auto acquired = acquire_dependency_reference(
            fixture.directory, reference_request(101), fixture.harness.callbacks());
        REQUIRE(acquired);
        const dependency_reference stale_copy = acquired.reference;
        REQUIRE(release_dependency_reference(
            fixture.directory, acquired.reference, fixture.harness.callbacks()));
        fixture.aim_reclaim_at(stale_copy);
        fixture.harness.fail_next_sequence = true;
        const auto reclaimed = reclaim_dependency_partition(
            fixture.directory, stale_copy.partition_index, 1, fixture.harness.callbacks());
        CHECK(reclaimed.error == dependency_directory_error::sequence_unavailable);
        CHECK(reclaimed.coarse_fallback_required);
        CHECK(fixture.record(stale_copy)->slot_state == directory_slot_state::occupied);
        CHECK(deep_validate_dependency_directory(fixture.directory, fixture.harness.callbacks()) ==
              dependency_directory_error::none);
    }

    SECTION("ABA recheck failure restores the captured record") {
        directory_fixture fixture{};
        auto acquired = acquire_dependency_reference(
            fixture.directory, reference_request(102), fixture.harness.callbacks());
        REQUIRE(acquired);
        const dependency_reference stale_copy = acquired.reference;
        REQUIRE(release_dependency_reference(
            fixture.directory, acquired.reference, fixture.harness.callbacks()));
        fixture.aim_reclaim_at(stale_copy);
        fixture.harness.corrupt_retiring_record = true;
        const auto reclaimed = reclaim_dependency_partition(
            fixture.directory, stale_copy.partition_index, 1, fixture.harness.callbacks());
        CHECK(reclaimed.error == dependency_directory_error::corrupt_directory);
        CHECK(reclaimed.coarse_fallback_required);
        CHECK(fixture.record(stale_copy)->slot_state == directory_slot_state::occupied);
        CHECK(fixture.record(stale_copy)->record_generation == stale_copy.record_generation);
        const auto stats =
            read_dependency_directory_stats(fixture.directory, fixture.harness.callbacks());
        REQUIRE(stats);
        CHECK(stats.stats.retiring_recheck_failure_count == 1);
    }
}

TEST_CASE("physical identity is stored and incompatible acquisition fails closed",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
    directory_fixture fixture{};
    const physical_identity identity = make_physical_identity(0x31);
    auto acquired = acquire_dependency_reference(
        fixture.directory,
        reference_request(190, dependency_reference_kind::ticket, identity),
        fixture.harness.callbacks());
    REQUIRE(acquired);
    CHECK(acquired.reference.physical_identity == identity);
    CHECK(acquired.snapshot.physical_identity == identity);

    const auto incompatible = acquire_dependency_reference(
        fixture.directory,
        reference_request(190, dependency_reference_kind::ticket, make_physical_identity(0x32)),
        fixture.harness.callbacks());
    CHECK(incompatible.error == dependency_directory_error::physical_identity_mismatch);
    CHECK(incompatible.coarse_fallback_required);
    CHECK_FALSE(incompatible.reference.active);
    REQUIRE(release_dependency_reference(
        fixture.directory, acquired.reference, fixture.harness.callbacks()));
}

TEST_CASE("transition identity preserve rejects mismatch without mutation",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
    directory_fixture fixture{};
    const physical_identity identity = make_physical_identity(0x41);
    auto acquired = acquire_dependency_reference(
        fixture.directory,
        reference_request(191, dependency_reference_kind::owner, identity),
        fixture.harness.callbacks());
    REQUIRE(acquired);
    REQUIRE(begin_dependency_transition(
        fixture.directory, acquired.reference, fixture.harness.callbacks()));
    const dependency_epoch_snapshot before =
        snapshot_dependency(fixture.directory, acquired.reference, fixture.harness.callbacks())
            .snapshot;

    const auto rejected = finish_dependency_transition(
        fixture.directory,
        acquired.reference,
        dependency_transition_resolution{
            .outcome = dependency_transition_outcome::committed,
            .physical_identity_action = dependency_physical_identity_action::preserve,
            .physical_identity = make_physical_identity(0x42),
        },
        fixture.harness.callbacks());
    CHECK(rejected.error == dependency_directory_error::physical_identity_mismatch);
    CHECK_FALSE(rejected.applied);
    CHECK(acquired.reference.transition_active);
    const auto unchanged =
        snapshot_dependency(fixture.directory, acquired.reference, fixture.harness.callbacks());
    REQUIRE(unchanged);
    CHECK(unchanged.snapshot.committed_epoch == before.committed_epoch);
    CHECK(unchanged.snapshot.change_generation == before.change_generation);
    CHECK(unchanged.snapshot.transition_count == 1);
    CHECK(unchanged.snapshot.physical_identity == identity);

    REQUIRE(finish_dependency_transition(
        fixture.directory,
        acquired.reference,
        make_transition_resolution(dependency_transition_outcome::aborted, acquired.reference),
        fixture.harness.callbacks()));
    REQUIRE(release_dependency_reference(
        fixture.directory, acquired.reference, fixture.harness.callbacks()));
}

TEST_CASE("committed identity replacement invalidates snapshots but not releases",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
    directory_fixture fixture{};
    const physical_identity old_identity = make_physical_identity(0x51);
    const physical_identity new_identity = make_physical_identity(0x52);
    auto owner = acquire_dependency_reference(
        fixture.directory,
        reference_request(192, dependency_reference_kind::owner, old_identity),
        fixture.harness.callbacks());
    auto ticket = acquire_dependency_reference(
        fixture.directory,
        reference_request(192, dependency_reference_kind::ticket, old_identity),
        fixture.harness.callbacks());
    auto generation = acquire_dependency_reference(
        fixture.directory,
        reference_request(192, dependency_reference_kind::generation, old_identity),
        fixture.harness.callbacks());
    REQUIRE(owner);
    REQUIRE(ticket);
    REQUIRE(generation);
    const dependency_vector_entry persisted{
        .version =
            {
                .key = generation.snapshot.key,
                .committed_epoch = generation.snapshot.committed_epoch,
                .physical_identity = old_identity,
                .observability = generation.snapshot.observability,
            },
        .record_generation = generation.snapshot.record_generation,
    };
    REQUIRE(begin_dependency_transition(
        fixture.directory, owner.reference, fixture.harness.callbacks()));
    const auto replaced = finish_dependency_transition(
        fixture.directory,
        owner.reference,
        dependency_transition_resolution{
            .outcome = dependency_transition_outcome::committed,
            .physical_identity_action = dependency_physical_identity_action::replace,
            .physical_identity = new_identity,
        },
        fixture.harness.callbacks());
    REQUIRE(replaced);
    CHECK(replaced.snapshot.physical_identity == new_identity);
    CHECK(owner.reference.physical_identity == new_identity);
    CHECK(replaced.snapshot.record_generation != persisted.record_generation);
    CHECK(owner.reference.record_generation == replaced.snapshot.record_generation);

    const auto stale_snapshot =
        snapshot_dependency(fixture.directory, ticket.reference, fixture.harness.callbacks());
    CHECK(stale_snapshot.error == dependency_directory_error::stale_reference);
    REQUIRE(release_dependency_reference(
        fixture.directory, ticket.reference, fixture.harness.callbacks()));
    REQUIRE(release_dependency_generation_entry(
        fixture.directory, persisted, fixture.harness.callbacks()));
    REQUIRE(release_dependency_reference(
        fixture.directory, owner.reference, fixture.harness.callbacks()));
}

TEST_CASE("known abort preserves the old physical identity",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
    directory_fixture fixture{};
    const physical_identity old_identity = make_physical_identity(0x61);
    const physical_identity new_identity = make_physical_identity(0x62);
    auto owner = acquire_dependency_reference(
        fixture.directory,
        reference_request(193, dependency_reference_kind::owner, old_identity),
        fixture.harness.callbacks());
    auto ticket = acquire_dependency_reference(
        fixture.directory,
        reference_request(193, dependency_reference_kind::ticket, old_identity),
        fixture.harness.callbacks());
    REQUIRE(owner);
    REQUIRE(ticket);
    const std::uint64_t old_epoch = owner.snapshot.committed_epoch;
    const std::uint64_t old_record_generation = owner.snapshot.record_generation;
    REQUIRE(begin_dependency_transition(
        fixture.directory, owner.reference, fixture.harness.callbacks()));

    const auto rejected_replacement = finish_dependency_transition(
        fixture.directory,
        owner.reference,
        dependency_transition_resolution{
            .outcome = dependency_transition_outcome::known_aborted,
            .physical_identity_action = dependency_physical_identity_action::replace,
            .physical_identity = new_identity,
        },
        fixture.harness.callbacks());
    CHECK(rejected_replacement.error == dependency_directory_error::invalid_argument);
    CHECK(owner.reference.transition_active);

    const auto aborted = finish_dependency_transition(
        fixture.directory,
        owner.reference,
        make_transition_resolution(dependency_transition_outcome::known_aborted, owner.reference),
        fixture.harness.callbacks());
    REQUIRE(aborted);
    CHECK(aborted.snapshot.physical_identity == old_identity);
    CHECK(aborted.snapshot.committed_epoch == old_epoch);
    CHECK(aborted.snapshot.record_generation == old_record_generation);
    CHECK(owner.reference.physical_identity == old_identity);
    CHECK(owner.reference.record_generation == old_record_generation);
    const auto ticket_snapshot =
        snapshot_dependency(fixture.directory, ticket.reference, fixture.harness.callbacks());
    REQUIRE(ticket_snapshot);
    CHECK(ticket_snapshot.snapshot.physical_identity == old_identity);
    CHECK(ticket_snapshot.snapshot.committed_epoch == old_epoch);
    CHECK(ticket_snapshot.snapshot.record_generation == old_record_generation);
    REQUIRE(release_dependency_reference(
        fixture.directory, ticket.reference, fixture.harness.callbacks()));
    REQUIRE(release_dependency_reference(
        fixture.directory, owner.reference, fixture.harness.callbacks()));
}

TEST_CASE("transition capabilities are linear and owner release is guarded",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
    directory_fixture fixture{};
    auto first = acquire_dependency_reference(
        fixture.directory, reference_request(200), fixture.harness.callbacks());
    auto second = acquire_dependency_reference(
        fixture.directory, reference_request(200), fixture.harness.callbacks());
    REQUIRE(first);
    REQUIRE(second);

    const auto begun = begin_dependency_transition(
        fixture.directory, first.reference, fixture.harness.callbacks());
    REQUIRE(begun);
    CHECK(begun.applied);
    CHECK(begun.diagnostic_success);
    CHECK(first.reference.transition_active);
    const std::uint64_t epoch_before = begun.snapshot.committed_epoch;
    const std::uint64_t change_before = begun.snapshot.change_generation;

    const auto replayed_begin = begin_dependency_transition(
        fixture.directory, first.reference, fixture.harness.callbacks());
    CHECK(replayed_begin.error == dependency_directory_error::invalid_argument);
    CHECK_FALSE(replayed_begin.applied);

    const auto active_release = release_dependency_reference(
        fixture.directory, first.reference, fixture.harness.callbacks());
    CHECK(active_release.error == dependency_directory_error::invalid_state);
    CHECK_FALSE(active_release.applied);
    const auto other_release = release_dependency_reference(
        fixture.directory, second.reference, fixture.harness.callbacks());
    REQUIRE(other_release);
    CHECK(other_release.snapshot.owner_ref_count == 1);
    CHECK(other_release.snapshot.transition_count == 1);
    const auto final_support_release = release_dependency_reference(
        fixture.directory, first.reference, fixture.harness.callbacks());
    CHECK(final_support_release.error == dependency_directory_error::invalid_state);
    CHECK_FALSE(final_support_release.applied);
    dependency_reference copied_support = first.reference;
    copied_support.transition_active = false;
    const auto copied_support_release = release_dependency_reference(
        fixture.directory, copied_support, fixture.harness.callbacks());
    CHECK(copied_support_release.error == dependency_directory_error::invalid_state);
    CHECK_FALSE(copied_support_release.applied);

    const auto finished = finish_dependency_transition(
        fixture.directory,
        first.reference,
        make_transition_resolution(dependency_transition_outcome::aborted, first.reference),
        fixture.harness.callbacks());
    REQUIRE(finished);
    CHECK(finished.applied);
    CHECK(finished.diagnostic_success);
    CHECK_FALSE(first.reference.transition_active);
    CHECK(finished.snapshot.transition_count == 0);
    CHECK(finished.snapshot.committed_epoch == epoch_before + 1U);
    CHECK(finished.snapshot.change_generation > change_before);
    CHECK(fixture.harness.wake_calls == 1);
    REQUIRE(fixture.harness.terminal_stage_count == 4);
    CHECK(fixture.harness.terminal_stages[0] == dependency_terminal_stage::epoch_published);
    CHECK(fixture.harness.terminal_stages[1] == dependency_terminal_stage::transition_decremented);
    CHECK(fixture.harness.terminal_stages[2] ==
          dependency_terminal_stage::change_generation_published);
    CHECK(fixture.harness.terminal_stages[3] == dependency_terminal_stage::waiters_broadcast);
    CHECK(fixture.harness.terminal_snapshots[0].committed_epoch == epoch_before + 1U);
    CHECK(fixture.harness.terminal_snapshots[0].transition_count == 1);
    CHECK(fixture.harness.terminal_snapshots[0].change_generation == change_before);
    CHECK(fixture.harness.terminal_snapshots[1].committed_epoch == epoch_before + 1U);
    CHECK(fixture.harness.terminal_snapshots[1].transition_count == 0);
    CHECK(fixture.harness.terminal_snapshots[1].change_generation == change_before);
    CHECK(fixture.harness.terminal_snapshots[2].change_generation > change_before);
    CHECK(fixture.harness.terminal_snapshots[3].committed_epoch ==
          fixture.harness.terminal_snapshots[2].committed_epoch);
    CHECK(fixture.harness.terminal_snapshots[3].transition_count ==
          fixture.harness.terminal_snapshots[2].transition_count);
    CHECK(fixture.harness.terminal_snapshots[3].change_generation ==
          fixture.harness.terminal_snapshots[2].change_generation);

    CHECK(release_dependency_reference(
        fixture.directory, first.reference, fixture.harness.callbacks()));
    CHECK_FALSE(release_dependency_reference(
        fixture.directory, second.reference, fixture.harness.callbacks()));
}

TEST_CASE("begin failure is no-mutation and invalid finish cannot decrement",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
    directory_fixture fixture{};
    auto acquired = acquire_dependency_reference(
        fixture.directory, reference_request(210), fixture.harness.callbacks());
    REQUIRE(acquired);

    fixture.harness.fail_next_sequence = true;
    const auto failed_begin = begin_dependency_transition(
        fixture.directory, acquired.reference, fixture.harness.callbacks());
    CHECK(failed_begin.error == dependency_directory_error::sequence_unavailable);
    CHECK_FALSE(failed_begin.applied);
    CHECK_FALSE(failed_begin.diagnostic_success);
    CHECK_FALSE(acquired.reference.transition_active);
    CHECK(fixture.record(acquired.reference)->transition_count == 0);

    REQUIRE(begin_dependency_transition(
        fixture.directory, acquired.reference, fixture.harness.callbacks()));
    const auto invalid_finish = finish_dependency_transition(
        fixture.directory,
        acquired.reference,
        make_transition_resolution(static_cast<dependency_transition_outcome>(255),
                                   acquired.reference),
        fixture.harness.callbacks());
    CHECK(invalid_finish.error == dependency_directory_error::invalid_argument);
    CHECK_FALSE(invalid_finish.applied);
    CHECK(acquired.reference.transition_active);
    CHECK(fixture.record(acquired.reference)->transition_count == 1);

    REQUIRE(finish_dependency_transition(
        fixture.directory,
        acquired.reference,
        make_transition_resolution(dependency_transition_outcome::aborted, acquired.reference),
        fixture.harness.callbacks()));
}

TEST_CASE("terminal sequence failure decrements and wakes without stranding",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
    directory_fixture fixture{};
    auto acquired = acquire_dependency_reference(
        fixture.directory, reference_request(220), fixture.harness.callbacks());
    REQUIRE(acquired);
    REQUIRE(begin_dependency_transition(
        fixture.directory, acquired.reference, fixture.harness.callbacks()));
    const std::uint64_t epoch_before = fixture.record(acquired.reference)->committed_epoch;

    fixture.harness.fail_next_sequence = true;
    const auto finished = finish_dependency_transition(
        fixture.directory,
        acquired.reference,
        make_transition_resolution(dependency_transition_outcome::committed, acquired.reference),
        fixture.harness.callbacks());
    CHECK(finished.error == dependency_directory_error::sequence_unavailable);
    CHECK(finished.applied);
    CHECK_FALSE(finished.diagnostic_success);
    CHECK(finished.coarse_fallback_required);
    CHECK_FALSE(acquired.reference.transition_active);
    CHECK(finished.snapshot.transition_count == 0);
    CHECK(finished.snapshot.committed_epoch == epoch_before + 1U);
    CHECK(fixture.harness.wake_calls == 1);

    const auto released = release_dependency_reference(
        fixture.directory, acquired.reference, fixture.harness.callbacks());
    CHECK(released.applied);
    CHECK(released.error == dependency_directory_error::emergency_fallback);
}

TEST_CASE("terminal wake failure decrements and latches coarse fallback",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
    directory_fixture fixture{};
    auto acquired = acquire_dependency_reference(
        fixture.directory, reference_request(230), fixture.harness.callbacks());
    REQUIRE(acquired);
    REQUIRE(begin_dependency_transition(
        fixture.directory, acquired.reference, fixture.harness.callbacks()));

    fixture.harness.fail_wake = true;
    const auto finished = finish_dependency_transition(
        fixture.directory,
        acquired.reference,
        make_transition_resolution(dependency_transition_outcome::aborted, acquired.reference),
        fixture.harness.callbacks());
    CHECK(finished.error == dependency_directory_error::wake_failed);
    CHECK(finished.applied);
    CHECK_FALSE(finished.diagnostic_success);
    CHECK(finished.coarse_fallback_required);
    CHECK(finished.snapshot.transition_count == 0);
    CHECK_FALSE(acquired.reference.transition_active);

    const auto stats =
        read_dependency_directory_stats(fixture.directory, fixture.harness.callbacks());
    REQUIRE(stats);
    CHECK(stats.stats.wake_failure_count == 1);
    CHECK(stats.stats.emergency_fallback_count >= 1);
}

TEST_CASE("terminal epoch overflow still resolves the active transition",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
    directory_fixture fixture{};
    auto acquired = acquire_dependency_reference(
        fixture.directory, reference_request(235), fixture.harness.callbacks());
    REQUIRE(acquired);
    REQUIRE(begin_dependency_transition(
        fixture.directory, acquired.reference, fixture.harness.callbacks()));
    fixture.record(acquired.reference)->committed_epoch =
        (std::numeric_limits<std::uint64_t>::max)();

    const auto finished = finish_dependency_transition(
        fixture.directory,
        acquired.reference,
        make_transition_resolution(dependency_transition_outcome::committed, acquired.reference),
        fixture.harness.callbacks());
    CHECK(finished.error == dependency_directory_error::counter_overflow);
    CHECK(finished.applied);
    CHECK_FALSE(finished.diagnostic_success);
    CHECK(finished.coarse_fallback_required);
    CHECK(finished.snapshot.transition_count == 0);
    CHECK_FALSE(acquired.reference.transition_active);
    CHECK(fixture.harness.wake_calls == 1);
}

TEST_CASE("missing wake contract cannot mutate a terminal transition",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
    directory_fixture fixture{};
    auto acquired = acquire_dependency_reference(
        fixture.directory, reference_request(240), fixture.harness.callbacks());
    REQUIRE(acquired);
    REQUIRE(begin_dependency_transition(
        fixture.directory, acquired.reference, fixture.harness.callbacks()));

    dependency_directory_callbacks missing_wake = fixture.harness.callbacks();
    missing_wake.wake_partition = nullptr;
    const auto rejected = finish_dependency_transition(
        fixture.directory,
        acquired.reference,
        make_transition_resolution(dependency_transition_outcome::aborted, acquired.reference),
        missing_wake);
    CHECK(rejected.error == dependency_directory_error::invalid_argument);
    CHECK_FALSE(rejected.applied);
    CHECK(acquired.reference.transition_active);
    CHECK(fixture.record(acquired.reference)->transition_count == 1);

    REQUIRE(finish_dependency_transition(
        fixture.directory,
        acquired.reference,
        make_transition_resolution(dependency_transition_outcome::aborted, acquired.reference),
        fixture.harness.callbacks()));
}

TEST_CASE("copied transition replay fails open after the real finish",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
    directory_fixture fixture{};
    auto acquired = acquire_dependency_reference(
        fixture.directory, reference_request(250), fixture.harness.callbacks());
    REQUIRE(acquired);
    REQUIRE(begin_dependency_transition(
        fixture.directory, acquired.reference, fixture.harness.callbacks()));
    dependency_reference copied = acquired.reference;
    REQUIRE(finish_dependency_transition(
        fixture.directory,
        acquired.reference,
        make_transition_resolution(dependency_transition_outcome::aborted, acquired.reference),
        fixture.harness.callbacks()));

    const auto replay = finish_dependency_transition(
        fixture.directory,
        copied,
        make_transition_resolution(dependency_transition_outcome::aborted, copied),
        fixture.harness.callbacks());
    CHECK(replay.error == dependency_directory_error::counter_underflow);
    CHECK_FALSE(replay.applied);
    CHECK(replay.coarse_fallback_required);
    CHECK(fixture.record(acquired.reference)->transition_count == 0);
}

TEST_CASE("persisted generation entry releases its exact ABA reference",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
    directory_fixture fixture{};
    auto acquired =
        acquire_dependency_reference(fixture.directory,
                                     reference_request(260, dependency_reference_kind::generation),
                                     fixture.harness.callbacks());
    REQUIRE(acquired);
    const dependency_vector_entry entry{
        .version =
            {
                .key = acquired.snapshot.key,
                .committed_epoch = acquired.snapshot.committed_epoch,
                .physical_identity =
                    {
                        .kind = physical_identity_kind::none,
                    },
                .observability = acquired.snapshot.observability,
            },
        .record_generation = acquired.snapshot.record_generation,
    };

    const auto released =
        release_dependency_generation_entry(fixture.directory, entry, fixture.harness.callbacks());
    REQUIRE(released);
    CHECK(released.snapshot.generation_ref_count == 0);
    CHECK(fixture.record(acquired.reference)->generation_ref_count == 0);

    const auto replay =
        release_dependency_generation_entry(fixture.directory, entry, fixture.harness.callbacks());
    CHECK(replay.error == dependency_directory_error::counter_underflow);
    CHECK_FALSE(replay.applied);
    CHECK(replay.coarse_fallback_required);
}

TEST_CASE("cumulative diagnostic aggregation detects overflow",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
    dependency_directory_config config{};
    config.partition_count = 2;
    directory_fixture fixture{config};
    fixture.partition(0)->pressure_event_count = (std::numeric_limits<std::uint64_t>::max)();
    fixture.partition(1)->pressure_event_count = 1;
    const auto stats =
        read_dependency_directory_stats(fixture.directory, fixture.harness.callbacks());
    CHECK(stats.error == dependency_directory_error::counter_overflow);
}

} // namespace
