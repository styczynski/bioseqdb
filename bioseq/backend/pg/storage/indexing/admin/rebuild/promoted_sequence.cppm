/**
 * @file admin/rebuild/promoted_sequence.cppm
 * @brief Keep a stored sequence readable for one index-build callback.
 */
module;

#include <bioseq/pg_compat.h>

#include <exception>
#include <utility>

export module bioseq.postgres.storage.indexing.admin.rebuild.promoted_sequence;

import bioseq.postgres.support;
import bioseq.postgres.storage.scanner;
import bioseq.postgres.types.compact_sequence;
import bioseq.sequences;

export namespace bioseq::storage::indexing {

/**
 * Open a read session for the promoted sequence and pass its lazy view to the
 * callback. The view is valid only during that call. Close the session on
 * success, or abort it before rethrowing a PostgreSQL or C++ failure.
 */
template <sequence_alphabet p_alphabet, class p_callback>
inline void with_promoted_pg_sequence_view(Datum promoted_datum,
                                           MemoryContext parent_context,
                                           p_callback&& callback) {
    const pg_sequence_descriptor descriptor = bioseq::postgres::support::invoke_pg_as_cpp(
        [promoted_datum] { return classify_and_describe_sequence(promoted_datum); });
    std::exception_ptr callback_failure;
    pg_sequence_read_session* volatile session = nullptr;
    PG_TRY();
    {
        auto* local_session = begin_pg_sequence_read_session(parent_context);
        session = local_session;
        try {
            auto* owner =
                make_pg_sequence_call_owner<p_alphabet>(*local_session, promoted_datum, descriptor);
            std::forward<p_callback>(callback)(owner->view());
            finish_pg_sequence_read_session(local_session, true);
        } catch (...) {
            callback_failure = std::current_exception();
            abort_pg_sequence_read_session_noexcept(local_session);
        }
        session = nullptr;
    }
    PG_CATCH();
    {
        abort_pg_sequence_read_session_noexcept(const_cast<pg_sequence_read_session*>(session));
        session = nullptr;
        PG_RE_THROW();
    }
    PG_END_TRY();
    if (callback_failure != nullptr) {
        std::rethrow_exception(std::move(callback_failure));
    }
}

} // namespace bioseq::storage::indexing
