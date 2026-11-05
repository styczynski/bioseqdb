/** @file Quality index reads retain the source owner's exact MVCC snapshot. */
module;

#include <bioseq/pg_compat.h>

module bioseq.postgres.storage.indexing.quality_summary_reader;

import std;
import bioseq.sequences;
import bioseq.sequences.compact_sequence_typmod;
import bioseq.sequences.quality_summary;
import bioseq.indexing.symbol_counts;
import bioseq.indexing.type;
import bioseq.storage.layout;
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.storage.scanner.source_provider_contract;
import bioseq.postgres.storage.indexing.handles;
import bioseq.postgres.storage.indexing.chunked_storage;

namespace bioseq::storage::indexing {

void install_quality_source_summary_reader() noexcept {
    sequence_source_detail::read_quality_source_index = +[](
            sequence_source_detail::pg_sequence_source_domain& domain,
            pg_sequence_read_session& session, const sequence_locator& locator,
            compact_sequence_stdc_type_params type, std::uint64_t row_number)
        -> std::expected<std::optional<bioseq::sequences::quality_summary>, bioseq::sequences::quality_error> {
        using namespace bioseq::sequences;
        if (!domain.registry || !domain.registered_snapshot || !domain.read_caches
            || !is_quality_alphabet_code(type.alphabet_code))
            return std::unexpected(quality_error::source_read);
        const auto& registry = *domain.registry;
        auto store = open_sequence_storage(registry.user_relid, registry.database_oid,
            registry.tablespace_oid, registry.main_relfilenode, domain.column_attno,
            bioseq::indexing::k_symbol_counts_index_name, locator, row_number,
            domain.read_caches, session.payload_context);
        std::expected<std::optional<quality_summary>, quality_error> result{std::nullopt};
        try {
            store.bind_read_snapshot(domain.registered_snapshot);
            if (store.chunk_byte_size() != 0) ++session.quality_statistics.payload_validations;
            pgseq_supported_quality_alphabets::dispatch_alphabet_by_code(type.alphabet_code,
                [&]<class Alphabet>() {
                    const auto payload = bioseq::indexing::symbol_counts_index<sequence_index_storage>
                        ::read_quality<Alphabet>(store, locator.source_epoch, locator.logical_length);
                    if (!payload) result = std::unexpected(payload.error());
                    else if (*payload) {
                        result = (**payload).summary;
                        domain.quality_cached_counts.fill(0);
                        std::copy((**payload).counts.begin(), (**payload).counts.end(),
                            domain.quality_cached_counts.begin());
                        domain.quality_cached_counts_valid = true;
                    }
                });
            store.close_checked();
        } catch (...) { store.abandon_after_pg_error_noexcept(); throw; }
        return result;
    };
}

} // namespace bioseq::storage::indexing
