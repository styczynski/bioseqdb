/**
 * @file
 * @brief Declares temporary sequence-locator replacement and restoration.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_scan_runtime_source_lifecycle_contracts;

import std;
import :custom_scan_scan_runtime_state_contracts;

export {
    /** @brief Replaces bound sequence locators with lazy values in the result memory context. */
    void promote_slice_project_sources(scan_state* state,
                                       TupleTableSlot* slot,
                                       MemoryContext result_context,
                                       bool allow_locator_passthrough,
                                       slice_project_source_slot_layout layout,
                                       TupleDesc expected_descriptor);

    /** @brief Selects sequence sources needed before alignment or while projecting a match. */
    enum class mmsearch_source_capture_role : std::uint8_t {
        evaluated_operand,
        output_projection,
    };

    /** @brief Saves a column number and its original sequence datum for restoration. */
    struct mmsearch_source_restore_entry {
        AttrNumber attno = InvalidAttrNumber;
        Datum datum{};
    };

    /**
     * @brief Saves source values while a slot temporarily holds promoted locators.
     *
     * Keeps four entries inline. Larger records use PostgreSQL memory, which is
     * released even when ERROR skips C++ destructors. Callers finish this record
     * before PG_TRY and leave it unchanged inside the protected region, so
     * PG_CATCH can still restore the slot.
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

    /** @brief Saves the original values needed for the selected evaluation step. */
    [[nodiscard]] mmsearch_source_restore
    capture_mmsearch_sources(bioseq_scan_runtime& runtime,
                             TupleTableSlot* slot,
                             MemoryContext restore_context,
                             mmsearch_source_capture_role role);

    /** @brief Makes captured sequence locators readable in the supplied memory context. */
    [[nodiscard]] std::size_t promote_mmsearch_sources(
        bioseq_scan_runtime& runtime,
        TupleTableSlot* slot,
        MemoryContext output_context,
        const mmsearch_source_restore& restore);

    /** @brief Puts saved source values back into the slot after evaluation or an error. */
    void restore_mmsearch_sources(TupleTableSlot* slot,
                                  const mmsearch_source_restore& restore) noexcept;

    /** @brief Checks that each saved datum is back in its original slot column. */
    [[nodiscard]] bool mmsearch_sources_restored_exactly(
        TupleTableSlot* slot,
        const mmsearch_source_restore& restore) noexcept;

    /** @brief Holds the filter result and evaluated mmsearch operands for one input row. */
    struct mmsearch_row_evaluation {
        bool qual_passed = false;
        Datum target{};
        bool target_is_null = true;
        Datum query{};
        bool query_is_null = true;
    };

    /** @brief Finds sequence sources needed before alignment and during match projection. */
    void derive_mmsearch_source_lifetime_roles(bioseq_scan_runtime& runtime,
                                               Plan* plan,
                                               TupleDesc relation_descriptor);
}

/** @brief Closes the scan's source provider and releases its tracing attachment. */
void release_scan_runtime_lazy_resources(bioseq_scan_runtime& runtime, bool is_commit);
