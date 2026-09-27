/**
 * @file typed_support_parts/source_provider_adapters.cppm
 * @brief Opens lazy views of expanded PostgreSQL sequence expressions.
 */
module;

extern "C" {
#include "postgres.h"
#include "utils/errcodes.h"
#include "varatt.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <new>

export module bioseq.postgres.scheduler.dispatch_processor.typed_support.source_provider_adapters;

import std;
import bioseq.postgres.scheduler.dispatch_processor.checked_alignment_calls;
import bioseq.postgres.support;
import bioseq.postgres.types.compact_sequence;

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

/**
 * Checks an expanded sequence's type and passes its lazy view to the callback.
 * Reuses the scanner's read session when available. Otherwise opens a session
 * for the callback and closes or aborts it on return or error.
 */
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

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
