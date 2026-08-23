/**
 * @file
 * @brief Defines exact source-promotion records and scan-runtime declarations.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_scan_runtime_source_lifecycle_contracts;

import std;
import :custom_scan_scan_runtime_state_contracts;

export {
    /** @brief Promote bound slice sources into the requested result memory context. */
    void promote_slice_project_sources(scan_state* state,
                                       TupleTableSlot* slot,
                                       MemoryContext result_context,
                                       bool allow_locator_passthrough,
                                       slice_project_source_slot_layout layout,
                                       TupleDesc expected_descriptor);

    /** @brief Selects the point at which a source datum must remain valid. */
    enum class mmsearch_source_capture_role : std::uint8_t {
        evaluated_operand,
        output_projection,
    };

    /** @brief Records one physical source datum for restoration after promotion. */
    struct mmsearch_source_restore_entry {
        AttrNumber attno = InvalidAttrNumber;
        Datum datum{};
    };

    /**
     * @brief Holds source datums that must survive temporary locator promotion.
     *
     * Four physical sources cover the ordinary target/query/output case without
     * a per-match_result allocation. Wider projections spill into the executor
     * per-tuple MemoryContext, which PostgreSQL releases even when ERROR skips C++
     * destructors. The object is populated before PG_TRY and never mutated in
     * its setjmp region, so either storage form is safe to read from PG_CATCH.
     */
    struct mmsearch_source_restore {
        static constexpr std::size_t inline_capacity = 4u;
        std::array<mmsearch_source_restore_entry, inline_capacity> inline_entries{};
        mmsearch_source_restore_entry* overflow_entries = nullptr;
        std::size_t count = 0u;

        /** @brief Returns a mutable inline or overflow entry by index. */
        [[nodiscard]] mmsearch_source_restore_entry& entry(const std::size_t index) noexcept {
            return overflow_entries == nullptr ? inline_entries[index] : overflow_entries[index];
        }

        /** @brief Returns an inline or overflow entry by index. */
        [[nodiscard]] const mmsearch_source_restore_entry&
        entry(const std::size_t index) const noexcept {
            return overflow_entries == nullptr ? inline_entries[index] : overflow_entries[index];
        }
    };

    /** @brief Captures sources required at the selected lifetime boundary. */
    [[nodiscard]] mmsearch_source_restore
    capture_mmsearch_sources(bioseq_scan_runtime& runtime,
                             TupleTableSlot* slot,
                             MemoryContext restore_context,
                             mmsearch_source_capture_role role);

    /** @brief Promotes captured locator sources into the output memory context. */
    [[nodiscard]] std::size_t promote_mmsearch_sources(
        bioseq_scan_runtime& runtime,
        TupleTableSlot* slot,
        MemoryContext output_context,
        const mmsearch_source_restore& restore);

    /** @brief Restores source datums after temporary promotion or an error path. */
    void restore_mmsearch_sources(TupleTableSlot* slot,
                                  const mmsearch_source_restore& restore) noexcept;

    /** @brief Verifies that every captured source datum has been restored exactly. */
    [[nodiscard]] bool mmsearch_sources_restored_exactly(
        TupleTableSlot* slot,
        const mmsearch_source_restore& restore) noexcept;

    /** @brief Carries the strict qual result and evaluated target/query operands for one row. */
    struct mmsearch_row_evaluation {
        bool qual_passed = false;
        Datum target{};
        bool target_is_null = true;
        Datum query{};
        bool query_is_null = true;
    };

    /** @brief Derive pre-evaluation and output source lifetimes from finalized executor trees. */
    void derive_mmsearch_source_lifetime_roles(bioseq_scan_runtime& runtime,
                                               Plan* plan,
                                               TupleDesc relation_descriptor);
}

/** @brief Release the lazy provider binding and trace attachment owned by one scan runtime. */
void release_scan_runtime_lazy_resources(bioseq_scan_runtime& runtime, bool is_commit);
