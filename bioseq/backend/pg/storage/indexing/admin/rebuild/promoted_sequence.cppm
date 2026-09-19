/**
 * @file admin/rebuild/promoted_sequence.cppm
 * @brief Snapshot-bound promoted sequence callback support for index rebuilds.
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
 * @brief Expose one promoted PostgreSQL sequence through its canonical lazy facade.
 * @tparam p_alphabet Sequence alphabet selected from the stored representation.
 * @tparam p_callback Callable accepting the borrowed lazy sequence view.
 * @param promoted_datum Source-domain-promoted sequence datum.
 * @param parent_context Context that owns the read session and facade adapter.
 * @param callback Operation executed while the session and view remain valid.
 *
 * The callback borrows the view only for the call. The function closes the read
 * session before returning, aborts it on either PostgreSQL or C++ failure, and
 * rethrows the original failure after cleanup.
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
