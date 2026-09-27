#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

import bioseq.cache.core;

namespace {

using bioseq::cache::attach_transition_ledger;
using bioseq::cache::begin_owner_transitions;
using bioseq::cache::claim_transition_owner;
using bioseq::cache::deep_validate_transition_ledger;
using bioseq::cache::dependency_key;
using bioseq::cache::dependency_kind;
using bioseq::cache::finish_owner_transitions;
using bioseq::cache::initialize_transition_ledger;
using bioseq::cache::rebuild_transition_dependencies;
using bioseq::cache::record_transition_dependency;
using bioseq::cache::repair_dead_transition_owner;
using bioseq::cache::shared_transition_owner_slot;
using bioseq::cache::transition_callback_result;
using bioseq::cache::transition_dependency_reference;
using bioseq::cache::transition_initial_reference_role;
using bioseq::cache::transition_ledger_attachment;
using bioseq::cache::transition_ledger_callbacks;
using bioseq::cache::transition_ledger_config;
using bioseq::cache::transition_ledger_error;
using bioseq::cache::transition_ledger_header;
using bioseq::cache::transition_ledger_metrics;
using bioseq::cache::transition_ledger_preload_request;
using bioseq::cache::transition_owner_claim;
using bioseq::cache::transition_owner_diagnostic;
using bioseq::cache::transition_owner_diagnostics;
using bioseq::cache::transition_owner_identity;
using bioseq::cache::transition_owner_state;
using bioseq::cache::transition_reference_kind;
using bioseq::cache::transition_repair_disposition;
using bioseq::cache::transition_resolution;

struct dependency_state {
    transition_dependency_reference identity{};
    std::uint32_t transition_count = 0;
    std::uint32_t owner_reference_count = 1;
    std::uint64_t committed_epoch = 1;
    std::uint64_t change_generation = 1;
    std::uint32_t waiter_broadcast_count = 0;
};

enum class finish_event : std::uint8_t {
    epoch_published = 0,
    transition_decremented,
    change_generation_published,
    waiters_broadcast,
};

struct finish_event_record {
    std::uint32_t object_oid = 0;
    finish_event event = finish_event::epoch_published;
};

struct callback_state {
    std::array<dependency_state, 300> dependencies{};
    std::size_t dependency_count = 0;
    std::array<std::uint32_t, 1024> locked_partitions{};
    std::size_t locked_partition_count = 0;
    std::array<std::uint32_t, 1024> reverse_unlock_order{};
    std::size_t reverse_unlock_count = 0;
    std::array<finish_event_record, 2048> finish_events{};
    std::size_t finish_event_count = 0;
    std::uint32_t owner_lock_depth = 0;
    std::uint32_t partition_lock_depth = 0;
    std::uint32_t begin_calls = 0;
    std::uint32_t finish_calls = 0;
    std::uint32_t release_calls = 0;
    std::uint32_t coarse_notifications = 0;
    std::uint32_t bootstrap_failures = 0;
    std::uint32_t fail_begin_call = (std::numeric_limits<std::uint32_t>::max)();
    std::uint32_t diagnostic_begin_call = (std::numeric_limits<std::uint32_t>::max)();
    std::uint32_t fail_finish_call = (std::numeric_limits<std::uint32_t>::max)();
    std::uint32_t diagnostic_finish_mask = 0;
    bool fail_partition_lock = false;
    bool owner_alive = true;
    bool lock_order_violation = false;
    bool finish_while_begin_waits = false;
    bool repair_while_begin_waits = false;
    bool replace_owner_during_liveness = false;
    transition_ledger_attachment interleave_ledger{};
    transition_owner_claim interleave_claim{};
    transition_owner_identity replacement_identity{};
    transition_dependency_reference replacement_emergency{};
    transition_owner_claim replacement_claim{};
    transition_ledger_error interleave_error = transition_ledger_error::none;
    transition_repair_disposition interleave_repair_disposition =
        transition_repair_disposition::none;

    [[nodiscard]] dependency_state*
    find(const transition_dependency_reference& reference) noexcept {
        for (std::size_t index = 0; index < dependency_count; ++index) {
            if (dependencies[index].identity.exact_identity_equal(reference)) {
                return &dependencies[index];
            }
        }
        return nullptr;
    }

    [[nodiscard]] transition_dependency_reference
    add_dependency(std::uint32_t object_oid,
                   std::uint32_t partition_index,
                   std::uint64_t record_generation = 0) noexcept {
        const std::uint64_t generation = record_generation == 0
                                             ? static_cast<std::uint64_t>(object_oid) * 10U + 1U
                                             : record_generation;
        const transition_dependency_reference reference{
            .key =
                dependency_key{
                    .database_oid = 9,
                    .class_oid = 1,
                    .object_oid = object_oid,
                    .object_sub_id = 0,
                    .kind = dependency_kind::relation_content,
                },
            .record_offset = object_oid * 64U,
            .partition_index = partition_index,
            .record_generation = generation,
            .kind = transition_reference_kind::owner,
            .active = true,
        };
        dependencies[dependency_count++] = dependency_state{
            .identity = reference,
        };
        return reference;
    }

    [[nodiscard]] transition_dependency_reference
    retain_dependency(const transition_dependency_reference& reference) noexcept {
        dependency_state* dependency = find(reference);
        if (dependency != nullptr) {
            ++dependency->owner_reference_count;
        }
        return reference;
    }

    void record_finish_event(const transition_dependency_reference& reference,
                             finish_event event) noexcept {
        if (finish_event_count < finish_events.size()) {
            finish_events[finish_event_count++] = finish_event_record{
                .object_oid = reference.key.object_oid,
                .event = event,
            };
        }
    }

    [[nodiscard]] static callback_state& from(void* context) noexcept {
        return *static_cast<callback_state*>(context);
    }

    static void lock_owner(void* context, std::uint32_t) noexcept {
        callback_state& state = from(context);
        ++state.owner_lock_depth;
    }

    static void unlock_owner(void* context, std::uint32_t) noexcept {
        callback_state& state = from(context);
        if (state.owner_lock_depth == 0) {
            state.lock_order_violation = true;
            return;
        }
        --state.owner_lock_depth;
    }

    static bool lock_dependencies(void* context,
                                  std::span<const std::uint32_t> partitions) noexcept {
        callback_state& state = from(context);
        if (state.owner_lock_depth != 0 || state.partition_lock_depth != 0 ||
            !std::is_sorted(partitions.begin(), partitions.end()) ||
            std::adjacent_find(partitions.begin(), partitions.end()) != partitions.end()) {
            state.lock_order_violation = true;
        }
        if (state.fail_partition_lock) {
            return false;
        }
        if (state.finish_while_begin_waits) {
            state.finish_while_begin_waits = false;
            const auto interleaved = finish_owner_transitions(state.interleave_ledger,
                                                              state.interleave_claim,
                                                              transition_resolution::aborted,
                                                              state.callbacks());
            state.interleave_error = interleaved.error;
        }
        if (state.repair_while_begin_waits) {
            state.repair_while_begin_waits = false;
            const auto interleaved =
                repair_dead_transition_owner(state.interleave_ledger,
                                             state.interleave_claim.owner.proc_number,
                                             state.callbacks());
            state.interleave_error = interleaved.error;
            state.interleave_repair_disposition = interleaved.disposition;
            return false;
        }
        state.locked_partition_count = partitions.size();
        for (std::size_t index = 0; index < partitions.size(); ++index) {
            state.locked_partitions[index] = partitions[index];
        }
        ++state.partition_lock_depth;
        return true;
    }

    static void unlock_dependencies(void* context,
                                    std::span<const std::uint32_t> partitions) noexcept {
        callback_state& state = from(context);
        if (state.owner_lock_depth != 0 || state.partition_lock_depth != 1 ||
            partitions.size() != state.locked_partition_count) {
            state.lock_order_violation = true;
        }
        state.reverse_unlock_count = partitions.size();
        for (std::size_t index = 0; index < partitions.size(); ++index) {
            state.reverse_unlock_order[index] = partitions[partitions.size() - index - 1U];
        }
        state.partition_lock_depth = 0;
    }

    static transition_callback_result begin(void* context,
                                            transition_dependency_reference* reference) noexcept {
        callback_state& state = from(context);
        const std::uint32_t call_index = state.begin_calls++;
        if (state.partition_lock_depth != 1 || state.owner_lock_depth != 1 ||
            reference == nullptr || reference->transition_active) {
            return transition_callback_result{
                .applied = false,
                .diagnostic_success = false,
                .coarse_fallback_required = true,
                .error = 1,
            };
        }
        dependency_state* dependency = state.find(*reference);
        if (dependency == nullptr || dependency->owner_reference_count == 0) {
            return transition_callback_result{
                .applied = false,
                .diagnostic_success = false,
                .coarse_fallback_required = true,
                .error = 2,
            };
        }
        if (call_index == state.fail_begin_call) {
            return transition_callback_result{
                .applied = false,
                .diagnostic_success = true,
                .coarse_fallback_required = true,
                .error = 3,
            };
        }
        ++dependency->transition_count;
        reference->transition_active = true;
        if (call_index == state.diagnostic_begin_call) {
            return transition_callback_result{
                .applied = true,
                .diagnostic_success = false,
                .coarse_fallback_required = true,
                .error = 4,
            };
        }
        return transition_callback_result{.applied = true};
    }

    static transition_callback_result finish(void* context,
                                             transition_dependency_reference* reference,
                                             transition_resolution resolution) noexcept {
        callback_state& state = from(context);
        const std::uint32_t call_index = state.finish_calls++;
        if (state.partition_lock_depth != 1 || state.owner_lock_depth != 1 ||
            reference == nullptr || !reference->transition_active) {
            return transition_callback_result{
                .applied = false,
                .diagnostic_success = false,
                .coarse_fallback_required = true,
                .error = 5,
            };
        }
        dependency_state* dependency = state.find(*reference);
        if (dependency == nullptr || dependency->transition_count == 0) {
            return transition_callback_result{
                .applied = false,
                .diagnostic_success = false,
                .coarse_fallback_required = true,
                .error = 6,
            };
        }
        if (call_index == state.fail_finish_call) {
            return transition_callback_result{
                .applied = false,
                .diagnostic_success = true,
                .coarse_fallback_required = true,
                .error = 7,
            };
        }
        static_cast<void>(resolution);
        ++dependency->committed_epoch;
        state.record_finish_event(*reference, finish_event::epoch_published);
        --dependency->transition_count;
        state.record_finish_event(*reference, finish_event::transition_decremented);
        ++dependency->change_generation;
        state.record_finish_event(*reference, finish_event::change_generation_published);
        ++dependency->waiter_broadcast_count;
        state.record_finish_event(*reference, finish_event::waiters_broadcast);
        reference->transition_active = false;
        const bool diagnostic_failure =
            call_index < 32U && (state.diagnostic_finish_mask & (1U << call_index)) != 0;
        return transition_callback_result{
            .applied = true,
            .diagnostic_success = !diagnostic_failure,
            .coarse_fallback_required = diagnostic_failure,
            .error = diagnostic_failure ? 8U : 0U,
        };
    }

    static transition_callback_result release(void* context,
                                              transition_dependency_reference* reference) noexcept {
        callback_state& state = from(context);
        ++state.release_calls;
        if (state.partition_lock_depth != 1 || state.owner_lock_depth != 1 ||
            reference == nullptr || reference->transition_active || !reference->active) {
            return transition_callback_result{
                .applied = false,
                .diagnostic_success = false,
                .coarse_fallback_required = true,
                .error = 9,
            };
        }
        dependency_state* dependency = state.find(*reference);
        if (dependency == nullptr || dependency->owner_reference_count == 0) {
            return transition_callback_result{
                .applied = false,
                .diagnostic_success = false,
                .coarse_fallback_required = true,
                .error = 10,
            };
        }
        --dependency->owner_reference_count;
        reference->active = false;
        return transition_callback_result{.applied = true};
    }

    static bool is_alive(void* context, const transition_owner_identity&) noexcept {
        callback_state& state = from(context);
        if (state.owner_lock_depth != 0 || state.partition_lock_depth != 0) {
            state.lock_order_violation = true;
        }
        if (state.replace_owner_during_liveness) {
            state.replace_owner_during_liveness = false;
            const auto finished = finish_owner_transitions(state.interleave_ledger,
                                                           state.interleave_claim,
                                                           transition_resolution::aborted,
                                                           state.callbacks());
            state.interleave_error = finished.error;
            const auto replacement = claim_transition_owner(state.interleave_ledger,
                                                            state.replacement_identity,
                                                            state.replacement_emergency,
                                                            state.callbacks());
            if (replacement) {
                state.replacement_claim = replacement.claim;
            } else {
                state.interleave_error = replacement.error;
            }
            return false;
        }
        return state.owner_alive;
    }

    static void mark_coarse(void* context, transition_ledger_error) noexcept {
        callback_state& state = from(context);
        if (state.owner_lock_depth != 0 || state.partition_lock_depth != 0) {
            state.lock_order_violation = true;
        }
        ++state.coarse_notifications;
    }

    static void mark_failed(void* context, transition_ledger_error) noexcept {
        callback_state& state = from(context);
        if (state.owner_lock_depth != 0 || state.partition_lock_depth != 0) {
            state.lock_order_violation = true;
        }
        ++state.bootstrap_failures;
    }

    [[nodiscard]] transition_ledger_callbacks callbacks() noexcept {
        return transition_ledger_callbacks{
            .context = this,
            .lock_owner_slot = lock_owner,
            .unlock_owner_slot = unlock_owner,
            .lock_partitions = lock_dependencies,
            .unlock_partitions_reverse = unlock_dependencies,
            .begin_transition_locked = begin,
            .finish_transition_and_wake_locked = finish,
            .release_owner_reference_locked = release,
            .owner_is_alive = is_alive,
            .mark_coarse_fallback = mark_coarse,
            .mark_bootstrap_failed = mark_failed,
        };
    }
};

struct ledger_fixture {
    callback_state callbacks_state{};
    transition_ledger_callbacks callbacks{};
    std::vector<std::max_align_t> storage{};
    transition_ledger_attachment ledger{};

    explicit ledger_fixture(std::uint32_t owners = 4,
                            std::uint32_t precise_references = 4,
                            std::uint32_t partitions = 8) {
        const auto request = transition_ledger_preload_request(transition_ledger_config{
            .owner_capacity = owners,
            .precise_reference_capacity = precise_references,
            .dependency_partition_count = partitions,
        });
        REQUIRE(request);
        const std::size_t words =
            (request.layout.total_bytes + sizeof(std::max_align_t) - 1U) / sizeof(std::max_align_t);
        storage.resize(words);
        std::span<std::byte> region(reinterpret_cast<std::byte*>(storage.data()),
                                    storage.size() * sizeof(std::max_align_t));
        REQUIRE(initialize_transition_ledger(region, request) == transition_ledger_error::none);
        const auto attached = attach_transition_ledger(region);
        REQUIRE(attached);
        ledger = attached.ledger;
        callbacks = callbacks_state.callbacks();
    }

    [[nodiscard]] transition_owner_identity identity(std::uint32_t slot,
                                                     std::uint64_t xid = 100) const noexcept {
        return transition_owner_identity{
            .proc_number = slot,
            .pid = static_cast<std::int32_t>(7000U + slot),
            .backend_nonce = 0xabc000U + slot,
            .backend_started_at = 500000 + slot,
            .full_xid = xid,
        };
    }

    [[nodiscard]] transition_owner_claim
    claim_owner(std::uint32_t slot,
                const transition_dependency_reference& emergency,
                std::uint64_t xid = 100) {
        const auto result =
            claim_transition_owner(ledger, identity(slot, xid), emergency, callbacks);
        REQUIRE(result);
        return result.claim;
    }

    [[nodiscard]] transition_ledger_header* header() noexcept {
        return reinterpret_cast<transition_ledger_header*>(ledger.region.data());
    }

    [[nodiscard]] shared_transition_owner_slot* owner_slot(std::uint32_t slot_index) noexcept {
        transition_ledger_header* header_value = header();
        return reinterpret_cast<shared_transition_owner_slot*>(ledger.region.data() +
                                                               header_value->layout.owners_offset) +
               slot_index;
    }

    [[nodiscard]] transition_dependency_reference*
    reference(std::uint32_t slot_index, std::uint32_t reference_index) noexcept {
        shared_transition_owner_slot* slot = owner_slot(slot_index);
        return reinterpret_cast<transition_dependency_reference*>(ledger.region.data() +
                                                                  slot->references_offset) +
               reference_index;
    }

    [[nodiscard]] bool references_are_zero(std::uint32_t slot_index) noexcept {
        const shared_transition_owner_slot* slot = owner_slot(slot_index);
        const std::span<const std::byte> bytes(ledger.region.data() + slot->references_offset,
                                               static_cast<std::size_t>(slot->reference_capacity) *
                                                   sizeof(transition_dependency_reference));
        return std::all_of(bytes.begin(), bytes.end(), [](std::byte value) noexcept {
            return value == std::byte{};
        });
    }

    [[nodiscard]] dependency_state&
    dependency(const transition_dependency_reference& reference) noexcept {
        dependency_state* value = callbacks_state.find(reference);
        REQUIRE(value != nullptr);
        return *value;
    }
};

static_assert(std::is_trivially_copyable_v<transition_owner_claim>);
static_assert(std::is_trivially_copyable_v<transition_dependency_reference>);
static_assert(std::is_standard_layout_v<transition_dependency_reference>);

void require_finish_contract_order(const callback_state& state, std::size_t expected_finish_count) {
    REQUIRE(state.finish_event_count == expected_finish_count * 4U);
    for (std::size_t index = 0; index < expected_finish_count; ++index) {
        const std::size_t first = index * 4U;
        REQUIRE(state.finish_events[first].event == finish_event::epoch_published);
        REQUIRE(state.finish_events[first + 1U].event == finish_event::transition_decremented);
        REQUIRE(state.finish_events[first + 2U].event == finish_event::change_generation_published);
        REQUIRE(state.finish_events[first + 3U].event == finish_event::waiters_broadcast);
        REQUIRE(state.finish_events[first].object_oid ==
                state.finish_events[first + 1U].object_oid);
        REQUIRE(state.finish_events[first].object_oid ==
                state.finish_events[first + 2U].object_oid);
        REQUIRE(state.finish_events[first].object_oid ==
                state.finish_events[first + 3U].object_oid);
    }
}

TEST_CASE("transition ledger computes bounded pointer-free layouts",
          "[component:cache][layer:unit][feature:transaction]") {
    const auto request = transition_ledger_preload_request(transition_ledger_config{
        .owner_capacity = 64,
        .precise_reference_capacity = 256,
        .dependency_partition_count = 16,
    });
    REQUIRE(request);
    REQUIRE(request.owner_lock_count == 64);
    REQUIRE(request.layout.references_per_owner == 257);
    REQUIRE(request.layout.total_bytes > request.layout.references_offset);

    REQUIRE_FALSE(transition_ledger_preload_request(transition_ledger_config{
        .owner_capacity = 1,
        .precise_reference_capacity = 257,
        .dependency_partition_count = 16,
    }));
    REQUIRE_FALSE(transition_ledger_preload_request(transition_ledger_config{
        .owner_capacity = 1,
        .precise_reference_capacity = 1,
        .dependency_partition_count = 3,
    }));
}

TEST_CASE("commit drains exact transitions and owner references once",
          "[component:cache][layer:unit][feature:transaction]") {
    ledger_fixture fixture;
    const auto emergency = fixture.callbacks_state.add_dependency(900, 7);
    const auto first = fixture.callbacks_state.add_dependency(101, 5);
    const auto second = fixture.callbacks_state.add_dependency(102, 1);
    const auto third = fixture.callbacks_state.add_dependency(103, 5);
    const auto owner = fixture.claim_owner(2, emergency);
    REQUIRE(record_transition_dependency(fixture.ledger, owner, first, fixture.callbacks));
    REQUIRE(record_transition_dependency(fixture.ledger, owner, second, fixture.callbacks));
    REQUIRE(record_transition_dependency(fixture.ledger, owner, third, fixture.callbacks));

    const auto begun = begin_owner_transitions(fixture.ledger, owner, fixture.callbacks);
    REQUIRE(begun);
    REQUIRE(begun.state == transition_owner_state::active);
    REQUIRE(begun.incremented_count == 3);
    REQUIRE(begun.outstanding_transition_count == 3);
    REQUIRE(fixture.callbacks_state.locked_partition_count == 2);
    REQUIRE(fixture.callbacks_state.locked_partitions[0] == 1);
    REQUIRE(fixture.callbacks_state.locked_partitions[1] == 5);
    REQUIRE(fixture.callbacks_state.reverse_unlock_count == 2);
    REQUIRE(fixture.callbacks_state.reverse_unlock_order[0] == 5);
    REQUIRE(fixture.callbacks_state.reverse_unlock_order[1] == 1);
    REQUIRE_FALSE(fixture.callbacks_state.lock_order_violation);

    const auto finished = finish_owner_transitions(
        fixture.ledger, owner, transition_resolution::committed, fixture.callbacks);
    REQUIRE(finished);
    REQUIRE(finished.state == transition_owner_state::free);
    REQUIRE(fixture.dependency(first).transition_count == 0);
    REQUIRE(fixture.dependency(second).transition_count == 0);
    REQUIRE(fixture.dependency(third).transition_count == 0);
    REQUIRE(fixture.dependency(first).committed_epoch == 2);
    REQUIRE(fixture.dependency(second).committed_epoch == 2);
    REQUIRE(fixture.dependency(third).committed_epoch == 2);
    REQUIRE(fixture.dependency(first).change_generation == 2);
    REQUIRE(fixture.dependency(second).change_generation == 2);
    REQUIRE(fixture.dependency(third).change_generation == 2);
    REQUIRE(fixture.dependency(first).waiter_broadcast_count == 1);
    REQUIRE(fixture.dependency(second).waiter_broadcast_count == 1);
    REQUIRE(fixture.dependency(third).waiter_broadcast_count == 1);
    REQUIRE(fixture.dependency(emergency).committed_epoch == 1);
    REQUIRE(fixture.dependency(emergency).owner_reference_count == 0);
    REQUIRE(fixture.callbacks_state.release_calls == 4);
    REQUIRE(fixture.callbacks_state.reverse_unlock_count == 3);
    REQUIRE(fixture.callbacks_state.reverse_unlock_order[0] == 7);
    REQUIRE(fixture.callbacks_state.reverse_unlock_order[1] == 5);
    REQUIRE(fixture.callbacks_state.reverse_unlock_order[2] == 1);
    require_finish_contract_order(fixture.callbacks_state, 3);
    REQUIRE(fixture.references_are_zero(2));
    const auto metrics = transition_ledger_metrics(fixture.ledger);
    REQUIRE(metrics);
    REQUIRE(metrics.metrics.successful_claim_count == 1);
    REQUIRE(metrics.metrics.current_owner_count == 0);
    REQUIRE(metrics.metrics.owner_high_water == 1);
    REQUIRE(metrics.metrics.current_transition_count == 0);
    REQUIRE(metrics.metrics.transition_high_water == 3);
    REQUIRE_FALSE(fixture.callbacks_state.lock_order_violation);
    REQUIRE(deep_validate_transition_ledger(fixture.ledger, fixture.callbacks) ==
            transition_ledger_error::none);

    const std::uint32_t finish_calls = fixture.callbacks_state.finish_calls;
    const auto replay = finish_owner_transitions(
        fixture.ledger, owner, transition_resolution::committed, fixture.callbacks);
    REQUIRE(replay.error == transition_ledger_error::stale_owner);
    REQUIRE(fixture.callbacks_state.finish_calls == finish_calls);
}

TEST_CASE("abort conservatively publishes epochs and wakes waiters",
          "[component:cache][layer:unit][feature:transaction]") {
    ledger_fixture fixture;
    const auto emergency = fixture.callbacks_state.add_dependency(900, 7);
    const auto precise = fixture.callbacks_state.add_dependency(101, 2);
    const auto owner = fixture.claim_owner(0, emergency);
    REQUIRE(record_transition_dependency(fixture.ledger, owner, precise, fixture.callbacks));
    REQUIRE(begin_owner_transitions(fixture.ledger, owner, fixture.callbacks));

    REQUIRE(finish_owner_transitions(
        fixture.ledger, owner, transition_resolution::aborted, fixture.callbacks));
    REQUIRE(fixture.dependency(precise).transition_count == 0);
    REQUIRE(fixture.dependency(precise).committed_epoch == 2);
    REQUIRE(fixture.dependency(precise).change_generation == 2);
    REQUIRE(fixture.dependency(precise).waiter_broadcast_count == 1);
    REQUIRE(fixture.dependency(precise).owner_reference_count == 0);
    REQUIRE(fixture.dependency(emergency).owner_reference_count == 0);
    require_finish_contract_order(fixture.callbacks_state, 1);
}

TEST_CASE("two owners on one dependency drain two to one to zero",
          "[component:cache][layer:unit][feature:transaction]") {
    ledger_fixture fixture(2, 4, 8);
    const auto emergency_zero = fixture.callbacks_state.add_dependency(900, 7);
    const auto emergency_one = fixture.callbacks_state.add_dependency(901, 6);
    const auto shared = fixture.callbacks_state.add_dependency(101, 2);
    const auto shared_second = fixture.callbacks_state.retain_dependency(shared);
    const auto owner_zero = fixture.claim_owner(0, emergency_zero, 100);
    const auto owner_one = fixture.claim_owner(1, emergency_one, 101);
    REQUIRE(record_transition_dependency(fixture.ledger, owner_zero, shared, fixture.callbacks));
    REQUIRE(
        record_transition_dependency(fixture.ledger, owner_one, shared_second, fixture.callbacks));
    REQUIRE(begin_owner_transitions(fixture.ledger, owner_zero, fixture.callbacks));
    REQUIRE(begin_owner_transitions(fixture.ledger, owner_one, fixture.callbacks));
    REQUIRE(fixture.dependency(shared).transition_count == 2);

    REQUIRE(finish_owner_transitions(
        fixture.ledger, owner_zero, transition_resolution::committed, fixture.callbacks));
    REQUIRE(fixture.dependency(shared).transition_count == 1);
    REQUIRE(fixture.dependency(shared).committed_epoch == 2);
    REQUIRE(finish_owner_transitions(
        fixture.ledger, owner_one, transition_resolution::committed, fixture.callbacks));
    REQUIRE(fixture.dependency(shared).transition_count == 0);
    REQUIRE(fixture.dependency(shared).committed_epoch == 3);
    REQUIRE(fixture.dependency(shared).change_generation == 3);
    REQUIRE(fixture.dependency(shared).waiter_broadcast_count == 2);
    REQUIRE(fixture.dependency(shared).owner_reference_count == 0);
    require_finish_contract_order(fixture.callbacks_state, 2);
    const auto metrics = transition_ledger_metrics(fixture.ledger);
    REQUIRE(metrics.metrics.current_transition_count == 0);
    REQUIRE(metrics.metrics.transition_high_water == 2);
    REQUIRE(metrics.metrics.current_owner_count == 0);
    REQUIRE(metrics.metrics.owner_high_water == 2);
}

TEST_CASE("every no-mutation begin failure rolls back its durable prefix",
          "[component:cache][layer:unit][feature:transaction]") {
    for (std::uint32_t failure_index = 0; failure_index < 3; ++failure_index) {
        ledger_fixture fixture;
        const auto emergency = fixture.callbacks_state.add_dependency(900, 7);
        std::array<transition_dependency_reference, 3> precise{
            fixture.callbacks_state.add_dependency(101, 5),
            fixture.callbacks_state.add_dependency(102, 1),
            fixture.callbacks_state.add_dependency(103, 3),
        };
        const auto owner = fixture.claim_owner(0, emergency);
        for (const auto& reference : precise) {
            REQUIRE(
                record_transition_dependency(fixture.ledger, owner, reference, fixture.callbacks));
        }
        fixture.callbacks_state.fail_begin_call = failure_index;
        const auto failed = begin_owner_transitions(fixture.ledger, owner, fixture.callbacks);
        REQUIRE(failed.error == transition_ledger_error::dependency_begin_failed);
        REQUIRE(failed.state == transition_owner_state::collecting);
        REQUIRE(failed.coarse_fallback_required);
        REQUIRE(failed.incremented_count == 0);
        REQUIRE(failed.outstanding_transition_count == 0);
        for (std::uint32_t index = 0; index < precise.size(); ++index) {
            REQUIRE(fixture.dependency(precise[index]).transition_count == 0);
            REQUIRE(fixture.dependency(precise[index]).committed_epoch ==
                    (index < failure_index ? 2 : 1));
        }

        fixture.callbacks_state.fail_begin_call = (std::numeric_limits<std::uint32_t>::max)();
        fixture.callbacks_state.begin_calls = 0;
        const auto coarse_begin = begin_owner_transitions(fixture.ledger, owner, fixture.callbacks);
        REQUIRE(coarse_begin);
        REQUIRE(coarse_begin.incremented_count == 1);
        REQUIRE(fixture.dependency(emergency).transition_count == 1);
        REQUIRE(finish_owner_transitions(
            fixture.ledger, owner, transition_resolution::committed, fixture.callbacks));
        REQUIRE(fixture.dependency(emergency).transition_count == 0);
        REQUIRE(fixture.dependency(emergency).committed_epoch == 2);
    }
}

TEST_CASE("post-apply begin diagnostics also drain every prefix member",
          "[component:cache][layer:unit][feature:transaction]") {
    for (std::uint32_t failure_index = 0; failure_index < 3; ++failure_index) {
        ledger_fixture fixture;
        const auto emergency = fixture.callbacks_state.add_dependency(900, 7);
        std::array<transition_dependency_reference, 3> precise{
            fixture.callbacks_state.add_dependency(101, 5),
            fixture.callbacks_state.add_dependency(102, 1),
            fixture.callbacks_state.add_dependency(103, 3),
        };
        const auto owner = fixture.claim_owner(0, emergency);
        for (const auto& reference : precise) {
            REQUIRE(
                record_transition_dependency(fixture.ledger, owner, reference, fixture.callbacks));
        }
        fixture.callbacks_state.diagnostic_begin_call = failure_index;
        const auto failed = begin_owner_transitions(fixture.ledger, owner, fixture.callbacks);
        REQUIRE(failed.error == transition_ledger_error::dependency_begin_failed);
        REQUIRE_FALSE(failed.bootstrap_failed);
        REQUIRE(failed.outstanding_transition_count == 0);
        for (const auto& reference : precise) {
            REQUIRE(fixture.dependency(reference).transition_count == 0);
        }
    }
}

TEST_CASE("finish diagnostics cannot strand transitions or stop the drain",
          "[component:cache][layer:unit][feature:transaction]") {
    ledger_fixture fixture;
    const auto emergency = fixture.callbacks_state.add_dependency(900, 7);
    std::array<transition_dependency_reference, 3> precise{
        fixture.callbacks_state.add_dependency(101, 5),
        fixture.callbacks_state.add_dependency(102, 1),
        fixture.callbacks_state.add_dependency(103, 3),
    };
    const auto owner = fixture.claim_owner(0, emergency);
    for (const auto& reference : precise) {
        REQUIRE(record_transition_dependency(fixture.ledger, owner, reference, fixture.callbacks));
    }
    REQUIRE(begin_owner_transitions(fixture.ledger, owner, fixture.callbacks));
    fixture.callbacks_state.diagnostic_finish_mask = 0b111U;

    const auto finished = finish_owner_transitions(
        fixture.ledger, owner, transition_resolution::committed, fixture.callbacks);
    REQUIRE(finished.error == transition_ledger_error::dependency_finish_failed);
    REQUIRE(finished.state == transition_owner_state::free);
    REQUIRE_FALSE(finished.bootstrap_failed);
    REQUIRE(finished.diagnostic_failure_count == 3);
    REQUIRE(fixture.callbacks_state.finish_calls == 3);
    REQUIRE(fixture.callbacks_state.coarse_notifications == 1);
    require_finish_contract_order(fixture.callbacks_state, 3);
    const auto metrics = transition_ledger_metrics(fixture.ledger);
    REQUIRE(metrics.metrics.finish_failure_count == 1);
    REQUIRE(metrics.metrics.diagnostic_failure_count == 3);
    REQUIRE(metrics.metrics.coarse_fallback_count >= 1);
    for (const auto& reference : precise) {
        REQUIRE(fixture.dependency(reference).transition_count == 0);
        REQUIRE(fixture.dependency(reference).committed_epoch == 2);
    }
}

TEST_CASE("an unapplied finish remains exactly repairable without replaying peers",
          "[component:cache][layer:unit][feature:transaction]") {
    ledger_fixture fixture;
    const auto emergency = fixture.callbacks_state.add_dependency(900, 7);
    std::array<transition_dependency_reference, 3> precise{
        fixture.callbacks_state.add_dependency(101, 5),
        fixture.callbacks_state.add_dependency(102, 1),
        fixture.callbacks_state.add_dependency(103, 3),
    };
    const auto owner = fixture.claim_owner(0, emergency);
    for (const auto& reference : precise) {
        REQUIRE(record_transition_dependency(fixture.ledger, owner, reference, fixture.callbacks));
    }
    REQUIRE(begin_owner_transitions(fixture.ledger, owner, fixture.callbacks));
    fixture.callbacks_state.fail_finish_call = 1;

    const auto failed = finish_owner_transitions(
        fixture.ledger, owner, transition_resolution::committed, fixture.callbacks);
    REQUIRE(failed.error == transition_ledger_error::bootstrap_failed);
    REQUIRE(failed.state == transition_owner_state::repairing);
    REQUIRE(failed.outstanding_transition_count == 1);
    REQUIRE(fixture.callbacks_state.finish_calls == 3);
    REQUIRE(fixture.dependency(precise[0]).transition_count == 0);
    REQUIRE(fixture.dependency(precise[1]).transition_count == 1);
    REQUIRE(fixture.dependency(precise[2]).transition_count == 0);

    fixture.callbacks_state.fail_finish_call = (std::numeric_limits<std::uint32_t>::max)();
    const std::uint32_t prior_finish_calls = fixture.callbacks_state.finish_calls;
    const auto repaired = finish_owner_transitions(
        fixture.ledger, owner, transition_resolution::committed, fixture.callbacks);
    REQUIRE(repaired);
    REQUIRE(repaired.state == transition_owner_state::free);
    REQUIRE(fixture.callbacks_state.finish_calls == prior_finish_calls + 1);
    for (const auto& reference : precise) {
        REQUIRE(fixture.dependency(reference).transition_count == 0);
        REQUIRE(fixture.dependency(reference).committed_epoch == 2);
    }
}

TEST_CASE("full owner identity and dependency generation reject ABA replay",
          "[component:cache][layer:unit][feature:transaction]") {
    ledger_fixture fixture;
    const auto emergency = fixture.callbacks_state.add_dependency(900, 7);
    const auto precise = fixture.callbacks_state.add_dependency(101, 2, 11);
    const auto owner = fixture.claim_owner(1, emergency, 100);

    std::array<transition_owner_claim, 5> stale_claims{
        owner,
        owner,
        owner,
        owner,
        owner,
    };
    stale_claims[0].owner.pid += 1;
    stale_claims[1].owner.backend_nonce += 1;
    stale_claims[2].owner.backend_started_at += 1;
    stale_claims[3].owner.full_xid += 1;
    stale_claims[4].claim_sequence += 1;
    for (const auto& stale : stale_claims) {
        REQUIRE(
            record_transition_dependency(fixture.ledger, stale, precise, fixture.callbacks).error ==
            transition_ledger_error::stale_owner);
    }

    REQUIRE(record_transition_dependency(fixture.ledger, owner, precise, fixture.callbacks));
    auto reused_slot = precise;
    reused_slot.record_generation = 12;
    const auto aba =
        record_transition_dependency(fixture.ledger, owner, reused_slot, fixture.callbacks);
    REQUIRE(aba.error == transition_ledger_error::dependency_aba_mismatch);
    REQUIRE(aba.coarse_fallback_required);
    REQUIRE_FALSE(aba.reference_consumed);

    REQUIRE(finish_owner_transitions(
        fixture.ledger, owner, transition_resolution::aborted, fixture.callbacks));
    const auto next_emergency = fixture.callbacks_state.add_dependency(901, 6);
    const auto replacement = fixture.claim_owner(1, next_emergency, 101);
    REQUIRE(begin_owner_transitions(fixture.ledger, owner, fixture.callbacks).error ==
            transition_ledger_error::stale_owner);
}

TEST_CASE("shared claim source is monotonic and refuses wrap",
          "[component:cache][layer:unit][feature:transaction]") {
    ledger_fixture fixture;
    const auto emergency = fixture.callbacks_state.add_dependency(900, 7);
    const auto first = fixture.claim_owner(0, emergency);
    REQUIRE(finish_owner_transitions(
        fixture.ledger, first, transition_resolution::aborted, fixture.callbacks));

    const auto next_emergency = fixture.callbacks_state.add_dependency(901, 6);
    const auto second = fixture.claim_owner(0, next_emergency, 101);
    REQUIRE(second.claim_sequence > first.claim_sequence);
    REQUIRE(finish_owner_transitions(
        fixture.ledger, second, transition_resolution::aborted, fixture.callbacks));

    std::atomic_ref<std::uint64_t>(fixture.header()->next_claim_sequence)
        .store((std::numeric_limits<std::uint64_t>::max)());
    const auto exhausted_emergency = fixture.callbacks_state.add_dependency(902, 5);
    const auto refused = claim_transition_owner(
        fixture.ledger, fixture.identity(0, 102), exhausted_emergency, fixture.callbacks);
    REQUIRE(refused.error == transition_ledger_error::sequence_unavailable);
    REQUIRE(refused.coarse_fallback_required);
}

TEST_CASE("savepoint rebuild releases rolled back refs and preserves survivors",
          "[component:cache][layer:unit][feature:transaction]") {
    ledger_fixture fixture;
    const auto emergency = fixture.callbacks_state.add_dependency(900, 7);
    const auto first = fixture.callbacks_state.add_dependency(101, 5);
    const auto rolled_back = fixture.callbacks_state.add_dependency(102, 2);
    const auto third = fixture.callbacks_state.add_dependency(103, 1);
    const auto owner = fixture.claim_owner(0, emergency);
    REQUIRE(record_transition_dependency(fixture.ledger, owner, first, fixture.callbacks));
    REQUIRE(record_transition_dependency(fixture.ledger, owner, rolled_back, fixture.callbacks));
    REQUIRE(record_transition_dependency(fixture.ledger, owner, third, fixture.callbacks));

    const std::array retained{first, third};
    const auto rebuilt =
        rebuild_transition_dependencies(fixture.ledger, owner, retained, fixture.callbacks);
    REQUIRE(rebuilt);
    REQUIRE(rebuilt.precise_reference_count == 2);
    REQUIRE_FALSE(rebuilt.coarse_fallback_required);
    REQUIRE(fixture.dependency(rolled_back).owner_reference_count == 0);
    REQUIRE(fixture.dependency(first).owner_reference_count == 1);
    REQUIRE(fixture.dependency(third).owner_reference_count == 1);

    REQUIRE(begin_owner_transitions(fixture.ledger, owner, fixture.callbacks));
    REQUIRE(fixture.dependency(first).transition_count == 1);
    REQUIRE(fixture.dependency(third).transition_count == 1);
    REQUIRE(fixture.dependency(rolled_back).transition_count == 0);
    REQUIRE(finish_owner_transitions(
        fixture.ledger, owner, transition_resolution::committed, fixture.callbacks));
    REQUIRE(fixture.references_are_zero(0));
}

TEST_CASE("the first precise reference can claim and transition the owner slot",
          "[component:cache][layer:unit][feature:transaction]") {
    ledger_fixture fixture(1, 2, 8);
    const auto first = fixture.callbacks_state.add_dependency(101, 5);
    const auto second = fixture.callbacks_state.add_dependency(102, 2);
    const auto owner =
        claim_transition_owner(fixture.ledger,
                               fixture.identity(0),
                               first,
                               fixture.callbacks,
                               transition_initial_reference_role::precise_and_fallback);
    REQUIRE(owner);
    REQUIRE(owner.reference_consumed);
    REQUIRE(owner.precise_reference_count == 1);
    REQUIRE(record_transition_dependency(fixture.ledger, owner.claim, second, fixture.callbacks));

    REQUIRE(begin_owner_transitions(fixture.ledger, owner.claim, fixture.callbacks));
    REQUIRE(fixture.dependency(first).transition_count == 1);
    REQUIRE(fixture.dependency(second).transition_count == 1);
    REQUIRE(finish_owner_transitions(
        fixture.ledger, owner.claim, transition_resolution::committed, fixture.callbacks));
    REQUIRE(fixture.dependency(first).transition_count == 0);
    REQUIRE(fixture.dependency(first).owner_reference_count == 0);
    REQUIRE(fixture.dependency(second).owner_reference_count == 0);
    REQUIRE(fixture.references_are_zero(0));
}

TEST_CASE("the initial precise reference counts against owner capacity",
          "[component:cache][layer:unit][feature:transaction]") {
    ledger_fixture fixture(1, 2, 8);
    const auto first = fixture.callbacks_state.add_dependency(101, 5);
    const auto second = fixture.callbacks_state.add_dependency(102, 2);
    const auto third = fixture.callbacks_state.add_dependency(103, 1);
    const auto owner =
        claim_transition_owner(fixture.ledger,
                               fixture.identity(0),
                               first,
                               fixture.callbacks,
                               transition_initial_reference_role::precise_and_fallback);
    REQUIRE(owner);
    REQUIRE(record_transition_dependency(fixture.ledger, owner.claim, second, fixture.callbacks));
    const auto refused =
        record_transition_dependency(fixture.ledger, owner.claim, third, fixture.callbacks);
    REQUIRE(refused.error == transition_ledger_error::capacity_exceeded);
    REQUIRE(refused.coarse_fallback_required);
    REQUIRE(finish_owner_transitions(
        fixture.ledger, owner.claim, transition_resolution::aborted, fixture.callbacks));
}

TEST_CASE("savepoint rebuild preserves the initial precise reference",
          "[component:cache][layer:unit][feature:transaction]") {
    ledger_fixture fixture(1, 3, 8);
    const auto first = fixture.callbacks_state.add_dependency(101, 5);
    const auto second = fixture.callbacks_state.add_dependency(102, 2);
    const auto rolled_back = fixture.callbacks_state.add_dependency(103, 1);
    const auto owner =
        claim_transition_owner(fixture.ledger,
                               fixture.identity(0),
                               first,
                               fixture.callbacks,
                               transition_initial_reference_role::precise_and_fallback);
    REQUIRE(owner);
    REQUIRE(record_transition_dependency(fixture.ledger, owner.claim, second, fixture.callbacks));
    REQUIRE(
        record_transition_dependency(fixture.ledger, owner.claim, rolled_back, fixture.callbacks));

    const std::array retained{second};
    const auto rebuilt =
        rebuild_transition_dependencies(fixture.ledger, owner.claim, retained, fixture.callbacks);
    REQUIRE(rebuilt);
    REQUIRE(rebuilt.precise_reference_count == 2);
    REQUIRE(fixture.dependency(first).owner_reference_count == 1);
    REQUIRE(fixture.dependency(second).owner_reference_count == 1);
    REQUIRE(fixture.dependency(rolled_back).owner_reference_count == 0);
    REQUIRE(begin_owner_transitions(fixture.ledger, owner.claim, fixture.callbacks));
    REQUIRE(fixture.dependency(first).transition_count == 1);
    REQUIRE(fixture.dependency(second).transition_count == 1);
    REQUIRE(finish_owner_transitions(
        fixture.ledger, owner.claim, transition_resolution::aborted, fixture.callbacks));
}

TEST_CASE("invalid rebuild and record input make coarse sticky",
          "[component:cache][layer:unit][feature:transaction]") {
    SECTION("invalid emergency") {
        ledger_fixture fixture;
        auto invalid = fixture.callbacks_state.add_dependency(900, 7);
        invalid.active = false;
        const auto rejected =
            claim_transition_owner(fixture.ledger, fixture.identity(0), invalid, fixture.callbacks);
        REQUIRE(rejected.error == transition_ledger_error::invalid_dependency_reference);
        REQUIRE(rejected.coarse_fallback_required);
        REQUIRE(fixture.callbacks_state.coarse_notifications == 1);
        const auto metrics = transition_ledger_metrics(fixture.ledger);
        REQUIRE(metrics.metrics.bookkeeping_failure_count == 1);
        REQUIRE(metrics.metrics.coarse_fallback_count == 1);
    }

    SECTION("invalid record") {
        ledger_fixture fixture;
        const auto emergency = fixture.callbacks_state.add_dependency(900, 7);
        auto invalid = fixture.callbacks_state.add_dependency(101, 2);
        invalid.active = false;
        const auto owner = fixture.claim_owner(0, emergency);
        const auto rejected =
            record_transition_dependency(fixture.ledger, owner, invalid, fixture.callbacks);
        REQUIRE(rejected.error == transition_ledger_error::invalid_dependency_reference);
        REQUIRE(rejected.coarse_fallback_required);
        REQUIRE(begin_owner_transitions(fixture.ledger, owner, fixture.callbacks));
        REQUIRE(fixture.dependency(emergency).transition_count == 1);
    }

    SECTION("duplicate rebuild") {
        ledger_fixture fixture;
        const auto emergency = fixture.callbacks_state.add_dependency(900, 7);
        const auto precise = fixture.callbacks_state.add_dependency(101, 2);
        const auto owner = fixture.claim_owner(0, emergency);
        REQUIRE(record_transition_dependency(fixture.ledger, owner, precise, fixture.callbacks));
        const std::array duplicate{precise, precise};
        const auto rejected =
            rebuild_transition_dependencies(fixture.ledger, owner, duplicate, fixture.callbacks);
        REQUIRE(rejected.error == transition_ledger_error::subtransaction_rebuild_failed);
        REQUIRE(rejected.coarse_fallback_required);
    }
}

TEST_CASE("duplicate exact refs never double count transitions",
          "[component:cache][layer:unit][feature:transaction]") {
    ledger_fixture fixture;
    const auto emergency = fixture.callbacks_state.add_dependency(900, 7);
    const auto precise = fixture.callbacks_state.add_dependency(101, 2);
    const auto owner = fixture.claim_owner(0, emergency);
    REQUIRE(record_transition_dependency(fixture.ledger, owner, precise, fixture.callbacks));
    const auto duplicate = fixture.callbacks_state.retain_dependency(precise);
    const auto rejected =
        record_transition_dependency(fixture.ledger, owner, duplicate, fixture.callbacks);
    REQUIRE(rejected.error == transition_ledger_error::duplicate_dependency_reference);
    REQUIRE_FALSE(rejected.reference_consumed);
    REQUIRE(rejected.coarse_fallback_required);
    --fixture.dependency(duplicate).owner_reference_count;

    REQUIRE(begin_owner_transitions(fixture.ledger, owner, fixture.callbacks));
    REQUIRE(fixture.dependency(emergency).transition_count == 1);
    REQUIRE(fixture.dependency(precise).transition_count == 0);
    REQUIRE(finish_owner_transitions(
        fixture.ledger, owner, transition_resolution::committed, fixture.callbacks));
}

TEST_CASE("precise capacity pressure switches atomically to emergency coarse",
          "[component:cache][layer:unit][feature:transaction]") {
    ledger_fixture fixture(1, 2, 8);
    const auto emergency = fixture.callbacks_state.add_dependency(900, 7);
    const auto first = fixture.callbacks_state.add_dependency(101, 1);
    const auto second = fixture.callbacks_state.add_dependency(102, 2);
    const auto overflow = fixture.callbacks_state.add_dependency(103, 3);
    const auto owner = fixture.claim_owner(0, emergency);
    REQUIRE(record_transition_dependency(fixture.ledger, owner, first, fixture.callbacks));
    REQUIRE(record_transition_dependency(fixture.ledger, owner, second, fixture.callbacks));
    const auto rejected =
        record_transition_dependency(fixture.ledger, owner, overflow, fixture.callbacks);
    REQUIRE(rejected.error == transition_ledger_error::capacity_exceeded);
    REQUIRE(rejected.coarse_fallback_required);
    REQUIRE_FALSE(rejected.reference_consumed);

    const auto begun = begin_owner_transitions(fixture.ledger, owner, fixture.callbacks);
    REQUIRE(begun);
    REQUIRE(begun.incremented_count == 1);
    REQUIRE(fixture.dependency(emergency).transition_count == 1);
    REQUIRE(fixture.dependency(first).transition_count == 0);
    REQUIRE(fixture.dependency(second).transition_count == 0);
    REQUIRE(finish_owner_transitions(
        fixture.ledger, owner, transition_resolution::committed, fixture.callbacks));
    REQUIRE(fixture.dependency(emergency).committed_epoch == 2);
}

TEST_CASE("the 257th unique precise key forces emergency coarse",
          "[component:cache][layer:unit][feature:transaction]") {
    ledger_fixture fixture(1, 256, 16);
    const auto emergency = fixture.callbacks_state.add_dependency(900, 15);
    std::array<transition_dependency_reference, 257> precise{};
    const auto owner = fixture.claim_owner(0, emergency);
    for (std::uint32_t index = 0; index < precise.size(); ++index) {
        precise[index] = fixture.callbacks_state.add_dependency(1000U + index, index % 16U);
        const auto recorded =
            record_transition_dependency(fixture.ledger, owner, precise[index], fixture.callbacks);
        if (index < 256U) {
            REQUIRE(recorded);
            REQUIRE(recorded.reference_consumed);
        } else {
            REQUIRE(recorded.error == transition_ledger_error::capacity_exceeded);
            REQUIRE(recorded.coarse_fallback_required);
            REQUIRE_FALSE(recorded.reference_consumed);
            --fixture.dependency(precise[index]).owner_reference_count;
        }
    }
    const auto begun = begin_owner_transitions(fixture.ledger, owner, fixture.callbacks);
    REQUIRE(begun);
    REQUIRE(begun.incremented_count == 1);
    REQUIRE(fixture.dependency(emergency).transition_count == 1);
    REQUIRE(finish_owner_transitions(
        fixture.ledger, owner, transition_resolution::committed, fixture.callbacks));
}

TEST_CASE("partition lock failure mutates nothing and retries coarse",
          "[component:cache][layer:unit][feature:transaction]") {
    ledger_fixture fixture;
    const auto emergency = fixture.callbacks_state.add_dependency(900, 7);
    const auto precise = fixture.callbacks_state.add_dependency(101, 2);
    const auto owner = fixture.claim_owner(0, emergency);
    REQUIRE(record_transition_dependency(fixture.ledger, owner, precise, fixture.callbacks));
    fixture.callbacks_state.fail_partition_lock = true;
    const auto failed = begin_owner_transitions(fixture.ledger, owner, fixture.callbacks);
    REQUIRE(failed.error == transition_ledger_error::partition_lock_failed);
    REQUIRE(failed.state == transition_owner_state::collecting);
    REQUIRE(fixture.dependency(precise).transition_count == 0);
    REQUIRE_FALSE(fixture.callbacks_state.lock_order_violation);

    fixture.callbacks_state.fail_partition_lock = false;
    REQUIRE(begin_owner_transitions(fixture.ledger, owner, fixture.callbacks));
    REQUIRE(fixture.dependency(emergency).transition_count == 1);
}

TEST_CASE("exact abort cancels a transitioning begin without guessing",
          "[component:cache][layer:unit][feature:transaction]") {
    ledger_fixture fixture;
    const auto emergency = fixture.callbacks_state.add_dependency(900, 7);
    const auto precise = fixture.callbacks_state.add_dependency(101, 2);
    const auto owner = fixture.claim_owner(0, emergency);
    REQUIRE(record_transition_dependency(fixture.ledger, owner, precise, fixture.callbacks));
    fixture.callbacks_state.finish_while_begin_waits = true;
    fixture.callbacks_state.interleave_ledger = fixture.ledger;
    fixture.callbacks_state.interleave_claim = owner;

    const auto begun = begin_owner_transitions(fixture.ledger, owner, fixture.callbacks);
    REQUIRE(begun.error == transition_ledger_error::stale_owner);
    REQUIRE(begun.state == transition_owner_state::free);
    REQUIRE_FALSE(begun.bootstrap_failed);
    REQUIRE(fixture.callbacks_state.interleave_error == transition_ledger_error::none);
    REQUIRE(fixture.dependency(precise).transition_count == 0);
    REQUIRE(fixture.dependency(precise).owner_reference_count == 0);
    REQUIRE(fixture.dependency(emergency).owner_reference_count == 0);
    REQUIRE_FALSE(fixture.callbacks_state.lock_order_violation);
}

TEST_CASE("transitioning finish drains only the published prefix",
          "[component:cache][layer:unit][feature:transaction]") {
    ledger_fixture fixture;
    const auto emergency = fixture.callbacks_state.add_dependency(900, 7);
    const auto first = fixture.callbacks_state.add_dependency(101, 5);
    const auto second = fixture.callbacks_state.add_dependency(102, 2);
    const auto third = fixture.callbacks_state.add_dependency(103, 1);
    const auto owner = fixture.claim_owner(0, emergency);
    REQUIRE(record_transition_dependency(fixture.ledger, owner, first, fixture.callbacks));
    REQUIRE(record_transition_dependency(fixture.ledger, owner, second, fixture.callbacks));
    REQUIRE(record_transition_dependency(fixture.ledger, owner, third, fixture.callbacks));

    shared_transition_owner_slot* slot = fixture.owner_slot(0);
    slot->transition_target_count = 3;
    std::atomic_ref<std::uint32_t>(slot->incremented_count).store(1);
    std::atomic_ref<std::uint32_t>(slot->outstanding_transition_count).store(1);
    fixture.reference(0, 1)->transition_active = true;
    fixture.dependency(first).transition_count = 1;
    std::atomic_ref<std::uint32_t>(fixture.header()->metrics.current_transition_count).store(1);
    std::atomic_ref<std::uint32_t>(fixture.header()->metrics.transition_high_water).store(1);
    std::atomic_ref<std::uint32_t>(slot->state)
        .store(static_cast<std::uint32_t>(transition_owner_state::transitioning));
    REQUIRE(deep_validate_transition_ledger(fixture.ledger, fixture.callbacks) ==
            transition_ledger_error::none);

    REQUIRE(finish_owner_transitions(
        fixture.ledger, owner, transition_resolution::aborted, fixture.callbacks));
    REQUIRE(fixture.callbacks_state.finish_calls == 1);
    REQUIRE(fixture.dependency(first).committed_epoch == 2);
    REQUIRE(fixture.dependency(second).committed_epoch == 1);
    REQUIRE(fixture.dependency(third).committed_epoch == 1);
    REQUIRE(fixture.dependency(first).transition_count == 0);
    REQUIRE(fixture.references_are_zero(0));
}

TEST_CASE("dead collecting repair rechecks the token and releases references",
          "[component:cache][layer:unit][feature:transaction]") {
    ledger_fixture fixture;
    const auto emergency = fixture.callbacks_state.add_dependency(900, 7);
    const auto precise = fixture.callbacks_state.add_dependency(101, 2);
    const auto owner = fixture.claim_owner(0, emergency);
    REQUIRE(record_transition_dependency(fixture.ledger, owner, precise, fixture.callbacks));

    fixture.callbacks_state.owner_alive = true;
    const auto alive = repair_dead_transition_owner(fixture.ledger, 0, fixture.callbacks);
    REQUIRE(alive.error == transition_ledger_error::owner_alive);
    REQUIRE(alive.disposition == transition_repair_disposition::owner_still_alive);

    fixture.callbacks_state.owner_alive = false;
    const auto repaired = repair_dead_transition_owner(fixture.ledger, 0, fixture.callbacks);
    REQUIRE(repaired);
    REQUIRE(repaired.disposition == transition_repair_disposition::cleared_dead_collecting);
    REQUIRE(fixture.dependency(emergency).owner_reference_count == 0);
    REQUIRE(fixture.dependency(precise).owner_reference_count == 0);
    REQUIRE_FALSE(fixture.callbacks_state.lock_order_violation);
    REQUIRE(transition_ledger_metrics(fixture.ledger).metrics.repaired_collecting_count == 1);
}

TEST_CASE("liveness ABA recheck never clears a replacement owner",
          "[component:cache][layer:unit][feature:transaction]") {
    ledger_fixture fixture;
    const auto old_emergency = fixture.callbacks_state.add_dependency(900, 7);
    const auto precise = fixture.callbacks_state.add_dependency(101, 2);
    const auto replacement_emergency = fixture.callbacks_state.add_dependency(901, 6);
    const auto old_owner = fixture.claim_owner(0, old_emergency, 100);
    REQUIRE(record_transition_dependency(fixture.ledger, old_owner, precise, fixture.callbacks));
    fixture.callbacks_state.interleave_ledger = fixture.ledger;
    fixture.callbacks_state.interleave_claim = old_owner;
    fixture.callbacks_state.replacement_identity = fixture.identity(0, 101);
    fixture.callbacks_state.replacement_emergency = replacement_emergency;
    fixture.callbacks_state.replace_owner_during_liveness = true;

    const auto repair = repair_dead_transition_owner(fixture.ledger, 0, fixture.callbacks);
    REQUIRE(repair.error == transition_ledger_error::stale_owner);
    REQUIRE(fixture.callbacks_state.interleave_error == transition_ledger_error::none);
    REQUIRE(fixture.callbacks_state.replacement_claim);
    REQUIRE(fixture.callbacks_state.replacement_claim.claim_sequence > old_owner.claim_sequence);
    REQUIRE(fixture.dependency(old_emergency).owner_reference_count == 0);
    REQUIRE(fixture.dependency(precise).owner_reference_count == 0);
    REQUIRE(fixture.dependency(replacement_emergency).owner_reference_count == 1);
    REQUIRE(finish_owner_transitions(fixture.ledger,
                                     fixture.callbacks_state.replacement_claim,
                                     transition_resolution::aborted,
                                     fixture.callbacks));
}

TEST_CASE("dead transitioning owner fails closed without a guessed decrement",
          "[component:cache][layer:unit][feature:transaction]") {
    ledger_fixture fixture;
    const auto emergency = fixture.callbacks_state.add_dependency(900, 7);
    const auto precise = fixture.callbacks_state.add_dependency(101, 2);
    const auto owner = fixture.claim_owner(0, emergency);
    REQUIRE(record_transition_dependency(fixture.ledger, owner, precise, fixture.callbacks));
    fixture.callbacks_state.owner_alive = false;
    fixture.callbacks_state.repair_while_begin_waits = true;
    fixture.callbacks_state.interleave_ledger = fixture.ledger;
    fixture.callbacks_state.interleave_claim = owner;

    const auto begun = begin_owner_transitions(fixture.ledger, owner, fixture.callbacks);
    REQUIRE(begun.error == transition_ledger_error::partition_lock_failed);
    REQUIRE(fixture.callbacks_state.interleave_error == transition_ledger_error::bootstrap_failed);
    REQUIRE(fixture.callbacks_state.interleave_repair_disposition ==
            transition_repair_disposition::fatal_transition_owner);
    REQUIRE(fixture.callbacks_state.finish_calls == 0);
    REQUIRE(fixture.dependency(precise).transition_count == 0);
    REQUIRE(fixture.callbacks_state.bootstrap_failures == 1);
    const auto metrics = transition_ledger_metrics(fixture.ledger);
    REQUIRE(metrics.metrics.fatal_owner_count == 1);
    REQUIRE(finish_owner_transitions(
        fixture.ledger, owner, transition_resolution::aborted, fixture.callbacks));
}

TEST_CASE("dead active or repairing owner fails bootstrap without guessing",
          "[component:cache][layer:unit][feature:transaction]") {
    SECTION("active owner") {
        ledger_fixture fixture;
        const auto emergency = fixture.callbacks_state.add_dependency(900, 7);
        const auto precise = fixture.callbacks_state.add_dependency(101, 2);
        const auto owner = fixture.claim_owner(0, emergency);
        REQUIRE(record_transition_dependency(fixture.ledger, owner, precise, fixture.callbacks));
        REQUIRE(begin_owner_transitions(fixture.ledger, owner, fixture.callbacks));
        fixture.callbacks_state.owner_alive = false;

        const auto repair = repair_dead_transition_owner(fixture.ledger, 0, fixture.callbacks);
        REQUIRE(repair.error == transition_ledger_error::bootstrap_failed);
        REQUIRE(repair.disposition == transition_repair_disposition::fatal_transition_owner);
        REQUIRE(repair.bootstrap_failed);
        REQUIRE(fixture.dependency(precise).transition_count == 1);
        REQUIRE(fixture.callbacks_state.finish_calls == 0);
        REQUIRE(fixture.callbacks_state.bootstrap_failures == 1);
        REQUIRE(transition_ledger_metrics(fixture.ledger).metrics.fatal_owner_count == 1);
    }

    SECTION("partially repairing owner") {
        ledger_fixture fixture;
        const auto emergency = fixture.callbacks_state.add_dependency(900, 7);
        const auto precise = fixture.callbacks_state.add_dependency(101, 2);
        const auto owner = fixture.claim_owner(0, emergency);
        REQUIRE(record_transition_dependency(fixture.ledger, owner, precise, fixture.callbacks));
        REQUIRE(begin_owner_transitions(fixture.ledger, owner, fixture.callbacks));
        fixture.callbacks_state.fail_finish_call = 0;
        REQUIRE(finish_owner_transitions(
                    fixture.ledger, owner, transition_resolution::committed, fixture.callbacks)
                    .bootstrap_failed);
        fixture.callbacks_state.owner_alive = false;

        const std::uint32_t finish_calls = fixture.callbacks_state.finish_calls;
        const auto repair = repair_dead_transition_owner(fixture.ledger, 0, fixture.callbacks);
        REQUIRE(repair.error == transition_ledger_error::bootstrap_failed);
        REQUIRE(fixture.callbacks_state.finish_calls == finish_calls);
        REQUIRE(fixture.dependency(precise).transition_count == 1);
    }
}

TEST_CASE("diagnostics are deterministic bounded and redact no pointers",
          "[component:cache][layer:unit][feature:transaction]") {
    ledger_fixture fixture;
    const auto emergency_two = fixture.callbacks_state.add_dependency(902, 2);
    const auto emergency_zero = fixture.callbacks_state.add_dependency(900, 0);
    const auto owner_two = fixture.claim_owner(2, emergency_two, 102);
    const auto owner_zero = fixture.claim_owner(0, emergency_zero, 100);

    std::array<transition_owner_diagnostic, 1> rows{};
    const auto diagnostics = transition_owner_diagnostics(fixture.ledger, rows, fixture.callbacks);
    REQUIRE(diagnostics);
    REQUIRE(diagnostics.total_non_free == 2);
    REQUIRE(diagnostics.copied_count == 1);
    REQUIRE(diagnostics.truncated);
    REQUIRE(rows[0].slot_index == 0);
    REQUIRE(rows[0].owner == owner_zero.owner);
    REQUIRE(rows[0].retained_owner_reference_count == 1);
}

} // namespace
