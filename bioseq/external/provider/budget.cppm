/**
 * @file external/provider/budget.cppm
 * @brief Checked execution budgets and cancellation-aware capacity leases.
 */
module;

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

export module bioseq.external.provider.budget;

import bioseq.external.provider.base;
import bioseq.external.provider.errors;
import bioseq.external.provider.query;
import bioseq.external.provider.capabilities;
import bioseq.common.checked_arithmetic;

/// Defines budget, gate, admission, and binding interfaces.
///
/// Provider core owns erased interfaces and the in-process gate. The admission
/// controller adapter remains outside provider core.
export namespace bioseq::external::provider {

/// Immutable identity binding for one prepared execution.
struct external_execution_binding {
    external_execution_key canonical_query_digest{};
    external_execution_key provider_plan_digest{};
    query_target target{};
    sequence_delivery_mode delivery{};
};

/// Requested resource limits used to derive an execution budget.
struct external_budget_request {
    query_target target{};
    sequence_delivery_mode delivery{};
    std::span<const provider_id> selected_providers;
    std::uint64_t expected_result_bytes{};
    std::uint64_t maximum_wire_bytes{};
    std::uint64_t maximum_decoded_bytes{};
    std::uint64_t maximum_record_bytes{};
    std::size_t requested_queue_bytes{};
    std::size_t requested_queue_items{};
    std::size_t requested_in_flight_transfers{};
};

/// Admitted resource budget for one external execution.
struct external_execution_budget {
    std::uint64_t admitted_bytes{};
    std::uint64_t reserved_capacity_bytes{};
    std::size_t queue_bytes{};
    std::size_t queue_items{};
    std::size_t metadata_page_bytes{};
    std::size_t error_body_bytes{};
    std::size_t transfer_carry_bytes{};
    std::size_t producer_staging_bytes{};
    std::size_t private_context_bytes{};
    std::size_t parser_scratch_bytes{};
    std::size_t decoder_bytes{};
    std::size_t archive_metadata_bytes{};
    std::size_t materialized_record_bytes{};
    std::size_t in_flight_transfers{};
    std::uint64_t maximum_wire_bytes{};
    std::uint64_t maximum_decoded_bytes{};
    std::uint64_t maximum_record_bytes{};
    std::uint64_t maximum_archive_entry_bytes{};
    bioseq::common::expansion_ratio_limit maximum_expansion{};

    /// Derives a checked budget within the admitted byte limit.
    static std::expected<external_execution_budget, external_error>
    make(const external_budget_request& request, std::uint64_t admitted_bytes) {
        external_execution_budget budget;
        budget.admitted_bytes = admitted_bytes;
        budget.queue_bytes = request.requested_queue_bytes;
        budget.queue_items = request.requested_queue_items;
        budget.metadata_page_bytes = 4U * 1024U * 1024U;
        budget.error_body_bytes = 64U * 1024U;
        const bool sequence_delivery = request.delivery != sequence_delivery_mode::not_applicable;
        budget.transfer_carry_bytes = sequence_delivery ? 1U * 1024U * 1024U : 0U;
        budget.producer_staging_bytes = 1U * 1024U * 1024U;
        budget.private_context_bytes = 2U * 1024U * 1024U;
        // Conservative upper bounds over the real runtime buffers. Parser
        // scratch covers the
        // bounded rechunk buffer's full capacity (chunk_bases ~1 MiB + one fed
        // block). Decoder state covers the zlib inflate window plus
        // input page. Archive metadata covers the libarchive manifest/index
        // spool. Metadata-only (not_applicable) scans decode/archive nothing and
        // parse only a bounded JSON page.
        budget.parser_scratch_bytes = sequence_delivery ? 2U * 1024U * 1024U : 1U * 1024U * 1024U;
        budget.decoder_bytes = sequence_delivery ? 512U * 1024U : 0U;
        budget.archive_metadata_bytes = sequence_delivery ? 1U * 1024U * 1024U : 0U;
        budget.in_flight_transfers = request.requested_in_flight_transfers;
        budget.maximum_wire_bytes = request.maximum_wire_bytes;
        budget.maximum_decoded_bytes = request.maximum_decoded_bytes;
        budget.maximum_record_bytes = request.maximum_record_bytes;
        budget.maximum_expansion = {
            .numerator = 1000, .denominator = 1, .grace_bytes = 1U * 1024U * 1024U};
        if (request.delivery == sequence_delivery_mode::materialized_record) {
            budget.materialized_record_bytes = request.maximum_record_bytes;
        }

        // Simultaneous-peak equation: private context, metadata pages, and
        // staging scale by producer_count.
        // Curl carry scales by transfer_count. The canonical plan snapshot, the
        // channel capacity (including fixed per-item overhead), parser scratch,
        // decoder state, archive metadata, the error reserve, and the
        // materialized-record datum are each counted once. All arithmetic is
        // checked/saturating.
        const std::uint64_t producers =
            request.selected_providers.empty()
                ? 1U
                : static_cast<std::uint64_t>(request.selected_providers.size());
        const std::uint64_t transfers =
            budget.in_flight_transfers == 0
                ? 1U
                : static_cast<std::uint64_t>(budget.in_flight_transfers);
        constexpr std::uint64_t k_queue_item_overhead_bytes = 64U;
        constexpr std::uint64_t k_control_snapshot_bytes = 256U * 1024U;
        const std::uint64_t queue_effective = bioseq::common::saturating_add(
            static_cast<std::uint64_t>(budget.queue_bytes),
            bioseq::common::saturating_mul(static_cast<std::uint64_t>(budget.queue_items),
                                           k_queue_item_overhead_bytes));
        const std::uint64_t per_provider = bioseq::common::saturating_mul(
            producers,
            static_cast<std::uint64_t>(budget.private_context_bytes + budget.metadata_page_bytes +
                                       budget.producer_staging_bytes));
        const std::uint64_t per_transfer = bioseq::common::saturating_mul(
            transfers, static_cast<std::uint64_t>(budget.transfer_carry_bytes));
        std::uint64_t peak = 0;
        for (const std::uint64_t add :
             {k_control_snapshot_bytes,
              queue_effective,
              per_provider,
              per_transfer,
              static_cast<std::uint64_t>(budget.parser_scratch_bytes),
              static_cast<std::uint64_t>(budget.decoder_bytes),
              static_cast<std::uint64_t>(budget.archive_metadata_bytes),
              static_cast<std::uint64_t>(budget.error_body_bytes),
              static_cast<std::uint64_t>(budget.materialized_record_bytes)}) {
            peak = bioseq::common::saturating_add(peak, add);
        }
        budget.reserved_capacity_bytes = peak;
        if (peak > admitted_bytes) {
            return std::unexpected(
                make_external_error(external_error_kind::response_too_large,
                                    "external budget peak exceeds admitted bytes"));
        }
        return budget;
    }
};

/// Cancellation-aware lease returned by a gate acquisition.
class external_gate_lease {
  public:
    external_gate_lease() = default;
    external_gate_lease(const external_gate_lease&) = delete;
    external_gate_lease& operator=(const external_gate_lease&) = delete;
    external_gate_lease(external_gate_lease&& other) noexcept
        : owner_(other.owner_), units_(other.units_), release_fn_(other.release_fn_) {
        other.owner_ = nullptr;
        other.release_fn_ = nullptr;
    }
    external_gate_lease& operator=(external_gate_lease&& other) noexcept {
        if (this != &other) {
            release_noexcept();
            owner_ = other.owner_;
            units_ = other.units_;
            release_fn_ = other.release_fn_;
            other.owner_ = nullptr;
            other.release_fn_ = nullptr;
        }
        return *this;
    }
    ~external_gate_lease() {
        release_noexcept();
    }
    /// Returns the capacity units retained by this lease.
    [[nodiscard]] std::uint64_t units() const noexcept {
        return units_;
    }
    /// Releases retained capacity when the lease is engaged.
    void release_noexcept() noexcept {
        if (owner_ != nullptr && release_fn_ != nullptr) {
            release_fn_(owner_, units_);
        }
        owner_ = nullptr;
        release_fn_ = nullptr;
    }

    external_gate_lease(void* owner,
                        std::uint64_t units,
                        void (*release)(void*, std::uint64_t) noexcept) noexcept
        : owner_(owner), units_(units), release_fn_(release) {}

  private:
    void* owner_{};
    std::uint64_t units_{};
    void (*release_fn_)(void*, std::uint64_t) noexcept {};
};

/// Non-owning byte-gate interface used by provider slots.
struct shared_external_byte_gate_ref {
    void* object{};
    std::expected<external_gate_lease, external_error> (*acquire_fn)(void*,
                                                                     std::uint64_t,
                                                                     std::stop_token){};
    void (*cancel_waiters_fn)(void*) noexcept {};

    /// Acquires byte capacity or returns a checked external error.
    [[nodiscard]] std::expected<external_gate_lease, external_error>
    acquire(std::uint64_t units, std::stop_token stop) const {
        return acquire_fn(object, units, stop);
    }
};

/// Non-owning transfer-slot gate interface used by provider slots.
struct shared_external_transfer_gate_ref {
    void* object{};
    std::expected<external_gate_lease, external_error> (*acquire_fn)(void*, std::stop_token){};
    void (*cancel_waiters_fn)(void*) noexcept {};
};

/// Fixed reserves and shared gates assigned to one provider slot.
struct external_slot_budget {
    provider_id provider;
    std::uint64_t fixed_reserved_bytes{};
    std::size_t queue_bytes{};
    std::size_t queue_items{};
    std::size_t metadata_page_bytes{};
    std::size_t staging_message_bytes{};
    std::size_t transfer_carry_bytes{};
    shared_external_byte_gate_ref dynamic_bytes;
    shared_external_transfer_gate_ref transfer_slots;
};

/// Borrowed view of an aggregate execution budget and its provider slots.
struct external_budget_allocation {
    external_execution_budget aggregate;
    std::span<const external_slot_budget> slots;
};

/// Checked in-process byte and transfer-slot gate.
class shared_external_gate_state {
  public:
    /// Constructs gates with fixed byte and transfer capacities.
    explicit shared_external_gate_state(std::uint64_t byte_capacity,
                                        std::uint32_t transfer_capacity) noexcept
        : byte_capacity_(byte_capacity), transfer_capacity_(transfer_capacity) {}

    /// Acquires byte capacity until released by the returned lease.
    [[nodiscard]] std::expected<external_gate_lease, external_error>
    acquire_bytes(std::uint64_t units, std::stop_token stop) {
        if (stop.stop_requested() || cancelled_.load()) {
            return std::unexpected(
                make_external_error(external_error_kind::cancelled, "gate cancelled"));
        }
        std::uint64_t current = byte_used_.load();
        while (true) {
            const std::optional<std::uint64_t> next = bioseq::common::checked_add(current, units);
            if (!next.has_value() || *next > byte_capacity_) {
                return std::unexpected(make_external_error(external_error_kind::response_too_large,
                                                           "external byte gate exhausted"));
            }
            if (byte_used_.compare_exchange_weak(current, *next)) {
                return external_gate_lease(this, units, &release_bytes_thunk);
            }
        }
    }

    /// Acquires one transfer slot until released by the returned lease.
    [[nodiscard]] std::expected<external_gate_lease, external_error>
    acquire_transfer(std::stop_token stop) {
        if (stop.stop_requested() || cancelled_.load()) {
            return std::unexpected(
                make_external_error(external_error_kind::cancelled, "gate cancelled"));
        }
        std::uint32_t current = transfer_used_.load();
        while (true) {
            if (current >= transfer_capacity_) {
                return std::unexpected(make_external_error(external_error_kind::response_too_large,
                                                           "external transfer gate exhausted"));
            }
            if (transfer_used_.compare_exchange_weak(current, current + 1)) {
                return external_gate_lease(this, 1, &release_transfer_thunk);
            }
        }
    }

    /// Marks pending and future acquisitions as cancelled.
    void cancel_waiters() noexcept {
        cancelled_.store(true);
    }

    /// Returns the configured byte capacity.
    [[nodiscard]] std::uint64_t byte_capacity() const noexcept {
        return byte_capacity_;
    }
    /// Returns the configured transfer-slot capacity.
    [[nodiscard]] std::uint32_t transfer_capacity() const noexcept {
        return transfer_capacity_;
    }

    /// Returns a non-owning byte-gate interface.
    shared_external_byte_gate_ref byte_ref() noexcept {
        return {
            .object = this, .acquire_fn = &byte_acquire_thunk, .cancel_waiters_fn = &cancel_thunk};
    }
    /// Returns a non-owning transfer-gate interface.
    shared_external_transfer_gate_ref transfer_ref() noexcept {
        return {.object = this,
                .acquire_fn = &transfer_acquire_thunk,
                .cancel_waiters_fn = &cancel_thunk};
    }

  private:
    static void release_bytes_thunk(void* self, std::uint64_t units) noexcept {
        static_cast<shared_external_gate_state*>(self)->byte_used_.fetch_sub(units);
    }
    static void release_transfer_thunk(void* self, std::uint64_t) noexcept {
        static_cast<shared_external_gate_state*>(self)->transfer_used_.fetch_sub(1);
    }
    static std::expected<external_gate_lease, external_error>
    byte_acquire_thunk(void* self, std::uint64_t units, std::stop_token stop) {
        return static_cast<shared_external_gate_state*>(self)->acquire_bytes(units, stop);
    }
    static std::expected<external_gate_lease, external_error>
    transfer_acquire_thunk(void* self, std::stop_token stop) {
        return static_cast<shared_external_gate_state*>(self)->acquire_transfer(stop);
    }
    static void cancel_thunk(void* self) noexcept {
        static_cast<shared_external_gate_state*>(self)->cancel_waiters();
    }

    std::uint64_t byte_capacity_{};
    std::uint32_t transfer_capacity_{};
    std::atomic<std::uint64_t> byte_used_{0};
    std::atomic<std::uint32_t> transfer_used_{0};
    std::atomic<bool> cancelled_{false};
};

/// Owns gate state and per-provider slot budgets.
class owning_external_budget_allocation {
  public:
    owning_external_budget_allocation(const owning_external_budget_allocation&) = delete;
    owning_external_budget_allocation& operator=(const owning_external_budget_allocation&) = delete;
    owning_external_budget_allocation(owning_external_budget_allocation&&) noexcept = default;
    owning_external_budget_allocation&
    operator=(owning_external_budget_allocation&&) noexcept = default;
    ~owning_external_budget_allocation() = default;

    /// Returns fixed and retained dynamic storage in bytes.
    [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
        return sizeof(owning_external_budget_allocation) +
               slots_.capacity() * sizeof(external_slot_budget);
    }

    /// Returns a borrowed allocation view backed by this object.
    [[nodiscard]] external_budget_allocation make_view() const noexcept {
        return {.aggregate = aggregate_, .slots = slots_};
    }

    /// Cancels gate acquisitions when gate state is present.
    void cancel_waiters() noexcept {
        if (gates_ != nullptr) {
            gates_->cancel_waiters();
        }
    }

    /// Builds a checked allocation and provider-slot assignments.
    static std::expected<owning_external_budget_allocation, external_error>
    make(const external_budget_request& request,
         std::span<const provider_plan_estimate> estimates,
         std::uint64_t admitted_bytes) {
        auto aggregate = external_execution_budget::make(request, admitted_bytes);
        if (!aggregate.has_value()) {
            return std::unexpected(aggregate.error());
        }
        // Per-slot fixed floor: private context
        // + one metadata page + one staging message + one transfer carry, plus
        // the provider's plan estimate. Build the slots and accumulate the fixed
        // reserve first. The shared byte gate then draws only from the headroom
        // left under the permit, so the disjoint-reserve invariant
        //   sum(slot.fixed_reserved_bytes) + gate.capacity <= admitted_bytes
        // holds by construction (the gate no longer defaults to the whole
        // permit, which would have made the invariant unsatisfiable).
        std::vector<external_slot_budget> slots;
        slots.reserve(request.selected_providers.size());
        std::uint64_t sum_fixed = 0;
        std::size_t index = 0;
        for (const provider_id provider : request.selected_providers) {
            external_slot_budget slot;
            slot.provider = provider;
            slot.queue_bytes = aggregate->queue_bytes;
            slot.queue_items = aggregate->queue_items;
            slot.metadata_page_bytes = aggregate->metadata_page_bytes;
            slot.staging_message_bytes = aggregate->producer_staging_bytes;
            slot.transfer_carry_bytes = aggregate->transfer_carry_bytes;
            std::uint64_t fixed = static_cast<std::uint64_t>(aggregate->private_context_bytes) +
                                  static_cast<std::uint64_t>(aggregate->metadata_page_bytes) +
                                  static_cast<std::uint64_t>(aggregate->producer_staging_bytes) +
                                  static_cast<std::uint64_t>(aggregate->transfer_carry_bytes);
            if (index < estimates.size()) {
                fixed = bioseq::common::saturating_add(fixed, estimates[index].plan_bytes);
            }
            slot.fixed_reserved_bytes = fixed;
            sum_fixed = bioseq::common::saturating_add(sum_fixed, fixed);
            slots.push_back(slot);
            ++index;
        }
        if (sum_fixed > aggregate->admitted_bytes) {
            return std::unexpected(
                make_external_error(external_error_kind::response_too_large,
                                    "external per-slot fixed reserves exceed admitted bytes"));
        }
        const std::uint64_t headroom = aggregate->admitted_bytes - sum_fixed;
        const std::uint64_t gate_capacity =
            (aggregate->maximum_decoded_bytes == 0 || aggregate->maximum_decoded_bytes > headroom)
                ? headroom
                : aggregate->maximum_decoded_bytes;
        const std::uint32_t transfer_capacity = static_cast<std::uint32_t>(
            aggregate->in_flight_transfers == 0 ? 1 : aggregate->in_flight_transfers);
        auto gates = std::make_unique<shared_external_gate_state>(gate_capacity, transfer_capacity);
        for (external_slot_budget& slot : slots) {
            slot.dynamic_bytes = gates->byte_ref();
            slot.transfer_slots = gates->transfer_ref();
        }
        return owning_external_budget_allocation(*aggregate, std::move(gates), std::move(slots));
    }

  private:
    owning_external_budget_allocation(external_execution_budget aggregate,
                                      std::unique_ptr<shared_external_gate_state> gates,
                                      std::vector<external_slot_budget> slots) noexcept
        : aggregate_(aggregate), gates_(std::move(gates)), slots_(std::move(slots)) {}

    external_execution_budget aggregate_;
    std::unique_ptr<shared_external_gate_state> gates_;
    std::vector<external_slot_budget> slots_;
};

/// Type-erased memory permit with single-release ownership.
class erased_memory_permit {
  public:
    erased_memory_permit() = default;
    erased_memory_permit(const erased_memory_permit&) = delete;
    erased_memory_permit& operator=(const erased_memory_permit&) = delete;
    erased_memory_permit(erased_memory_permit&& other) noexcept
        : owner_(other.owner_), destroy_fn_(other.destroy_fn_) {
        other.owner_ = nullptr;
        other.destroy_fn_ = nullptr;
    }
    erased_memory_permit& operator=(erased_memory_permit&& other) noexcept {
        if (this != &other) {
            release_noexcept();
            owner_ = other.owner_;
            destroy_fn_ = other.destroy_fn_;
            other.owner_ = nullptr;
            other.destroy_fn_ = nullptr;
        }
        return *this;
    }
    ~erased_memory_permit() {
        release_noexcept();
    }
    /// Releases the retained permit when engaged.
    void release_noexcept() noexcept {
        if (owner_ != nullptr && destroy_fn_ != nullptr) {
            destroy_fn_(owner_);
        }
        owner_ = nullptr;
        destroy_fn_ = nullptr;
    }

    erased_memory_permit(void* owner, void (*destroy)(void*) noexcept) noexcept
        : owner_(owner), destroy_fn_(destroy) {}

  private:
    void* owner_{};
    void (*destroy_fn_)(void*) noexcept {};
};

/// Owns a permit, validated allocation, and immutable execution binding.
class external_execution_admission {
  public:
    external_execution_admission(const external_execution_admission&) = delete;
    external_execution_admission& operator=(const external_execution_admission&) = delete;
    external_execution_admission(external_execution_admission&&) noexcept = default;
    external_execution_admission& operator=(external_execution_admission&&) noexcept = default;
    ~external_execution_admission() {
        release_noexcept();
    }

    /// Returns the current borrowed budget allocation.
    [[nodiscard]] external_budget_allocation allocation() const noexcept {
        return allocation_.has_value() ? allocation_->make_view() : external_budget_allocation{};
    }
    /// Returns the immutable execution binding.
    [[nodiscard]] const external_execution_binding& binding() const noexcept {
        return binding_;
    }
    /// Returns the provider-plan execution key.
    [[nodiscard]] external_execution_key execution_key() const noexcept {
        return binding_.provider_plan_digest;
    }
    /// Releases allocation state before releasing its memory permit.
    void release_noexcept() noexcept {
        allocation_.reset();
        permit_.reset();
    }

    /// Adopts a validated permit, allocation, and execution binding.
    static external_execution_admission adopt(erased_memory_permit&& permit,
                                              owning_external_budget_allocation&& allocation,
                                              external_execution_binding binding) noexcept {
        return external_execution_admission(std::move(permit), std::move(allocation), binding);
    }

  private:
    external_execution_admission(erased_memory_permit&& permit,
                                 owning_external_budget_allocation&& allocation,
                                 external_execution_binding binding) noexcept
        : permit_(std::move(permit)), allocation_(std::move(allocation)), binding_(binding) {}

    std::optional<erased_memory_permit> permit_;
    std::optional<owning_external_budget_allocation> allocation_;
    external_execution_binding binding_{};
};

} // namespace bioseq::external::provider
