/**
 * @file typed_support_parts/source_provider_adapters.cppm
 * @brief Adapts retained PostgreSQL sequence sources to typed lazy views.
 */
module;

#include <bioseq/pg_compat.h>

export module bioseq.postgres.scheduler.dispatch_processor.typed_support.source_provider_adapters;

import std;
import bioseq.postgres.scheduler.dispatch_processor.checked_alignment_calls;
import bioseq.postgres.scheduler.dispatch_processor.source_provider_promotion;
import bioseq.postgres.support;
import bioseq.sequences;
import bioseq.postgres.types.compact_sequence;
import bioseq.storage.lazy_sequence;
import bioseq.storage.layout;

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

/**
 * Promotes one locator through the statement source provider and passes its
 * typed lazy view to the callback.
 */
template <class p_alphabet, class p_scanner_pg_context, class p_function>
inline void with_source_provider_pg_locator(p_scanner_pg_context& context,
                                            const bioseq::storage::sequence_locator& locator,
                                            const AttrNumber column_attno,
                                            const std::uint8_t alphabet_code,
                                            const std::uint8_t storage_unit_byte_width,
                                            const std::uint16_t last_padding_letters,
                                            pg_sequence_read_session* active_read_session,
                                            p_function&& callback) {
    const Datum promoted =
        promote_exact_source_provider_pg_locator(static_cast<void*>(context.source_provider),
                                                 context.user_relid,
                                                 context.db_oid,
                                                 context.tablespace_oid,
                                                 context.main_relfilenode,
                                                 context.snapshot,
                                                 context.parent_mctx,
                                                 locator,
                                                 column_attno,
                                                 alphabet_code,
                                                 storage_unit_byte_width,
                                                 last_padding_letters);
    const pg_sequence_descriptor descriptor = bioseq::postgres::support::invoke_pg_as_cpp(
        [promoted] { return ::classify_and_describe_sequence(promoted); });
    if (!descriptor.alphabet_bound ||
        (descriptor.kind != pg_sequence_datum_kind::expanded_read_only &&
         descriptor.kind != pg_sequence_datum_kind::expanded_read_write) ||
        descriptor.alphabet_code != alphabet_code ||
        descriptor.storage_unit_byte_width != storage_unit_byte_width ||
        descriptor.bp_length != locator.logical_length) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_DATA_CORRUPTED,
            "source-provider promotion returned mismatched sequence metadata");
    }

    if (active_read_session != nullptr) {
        auto* sequence_owner = bioseq::postgres::support::invoke_pg_as_cpp(
            [active_read_session, promoted, descriptor] {
                return ::make_pg_sequence_call_owner<p_alphabet>(
                    *active_read_session, promoted, descriptor);
            });
        std::forward<p_function>(callback)(sequence_owner->view(), active_read_session);
        return;
    }

    auto* local_session =
        bioseq::postgres::support::invoke_pg_as_cpp([parent_mctx = context.parent_mctx] {
            return ::begin_pg_sequence_read_session(parent_mctx);
        });
    try {
        auto* sequence_owner =
            bioseq::postgres::support::invoke_pg_as_cpp([local_session, promoted, descriptor] {
                return ::make_pg_sequence_call_owner<p_alphabet>(
                    *local_session, promoted, descriptor);
            });
        std::forward<p_function>(callback)(sequence_owner->view(), local_session);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [local_session] { ::finish_pg_sequence_read_session(local_session, true); });
    } catch (...) {
        ::abort_pg_sequence_read_session_noexcept(local_session);
        throw;
    }
}

/** Passes one retained expanded sequence recipe to a typed callback. */
template <class p_alphabet, class p_scanner_pg_context, class p_function>
inline void with_pg_sequence_recipe(p_scanner_pg_context& context,
                                    const Datum recipe,
                                    const pg_sequence_descriptor& descriptor,
                                    const std::uint8_t alphabet_code,
                                    const std::uint8_t storage_unit_byte_width,
                                    p_function&& callback) {
    const bool expanded = recipe != Datum{0} &&
                          VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(recipe)) &&
                          (descriptor.kind == pg_sequence_datum_kind::expanded_read_only ||
                           descriptor.kind == pg_sequence_datum_kind::expanded_read_write);
    if (!expanded || !descriptor.alphabet_bound || descriptor.alphabet_code != alphabet_code ||
        descriptor.storage_unit_byte_width != storage_unit_byte_width ||
        context.parent_mctx == nullptr) {
        raise_typed_dispatch_pg_error_as_cpp(ERRCODE_DATA_CORRUPTED,
                                             "retained sequence recipe metadata is invalid");
    }

    if (context.read_session != nullptr) {
        auto* sequence_owner = bioseq::postgres::support::invoke_pg_as_cpp(
            [read_session = context.read_session, recipe, descriptor] {
                return ::make_pg_sequence_call_owner<p_alphabet>(*read_session, recipe, descriptor);
            });
        std::forward<p_function>(callback)(sequence_owner->view(), context.read_session);
        return;
    }

    auto* local_session =
        bioseq::postgres::support::invoke_pg_as_cpp([parent_mctx = context.parent_mctx] {
            return ::begin_pg_sequence_read_session(parent_mctx);
        });
    try {
        auto* sequence_owner =
            bioseq::postgres::support::invoke_pg_as_cpp([local_session, recipe, descriptor] {
                return ::make_pg_sequence_call_owner<p_alphabet>(
                    *local_session, recipe, descriptor);
            });
        std::forward<p_function>(callback)(sequence_owner->view(), local_session);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [local_session] { ::finish_pg_sequence_read_session(local_session, true); });
    } catch (...) {
        ::abort_pg_sequence_read_session_noexcept(local_session);
        throw;
    }
}

/** Promotes the target locator from a scanner context to its typed lazy view. */
template <class p_sequence, class p_scanner_pg_context, class p_function>
inline void with_source_provider_pg_target(p_scanner_pg_context& context,
                                           const bioseq::storage::sequence_locator& locator,
                                           const std::uint8_t alphabet_code,
                                           const std::uint8_t storage_unit_byte_width,
                                           const std::uint16_t last_padding_letters,
                                           pg_sequence_read_session* active_read_session,
                                           p_function&& callback) {
    using alphabet = typename compact_sequence_range<p_sequence>::traits::alphabet_type;
    with_source_provider_pg_locator<alphabet>(context,
                                              locator,
                                              static_cast<AttrNumber>(context.mmsearch_seq_attno),
                                              alphabet_code,
                                              storage_unit_byte_width,
                                              last_padding_letters,
                                              active_read_session,
                                              std::forward<p_function>(callback));
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
