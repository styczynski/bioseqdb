/**
 * @file supported_indexes.cppm
 * @brief Declares the PostgreSQL registry of supported sequence indexes.
 */
module;

#include <libassert/assert.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

export module bioseq.postgres.storage.indexing.supported_indexes;

import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.registry;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.indexing.symbol_counts;
import bioseq.postgres.storage.indexing.chunked_storage;

export namespace bioseq::storage::indexing {

#ifdef BIOSEQ_TEST_INDEXES
/// A table-chunk index used to exercise table storage in integration tests.
template <class p_storage> struct test_table_count_index {
    static_assert(bioseq::indexing::is_index_chunked_storage_for<
                  p_storage,
                  bioseq::indexing::sequence_index_layout::table_chunks>);
    static constexpr bioseq::indexing::sequence_index_layout index_type =
        bioseq::indexing::sequence_index_layout::table_chunks;
    static constexpr std::string_view index_name = "test_table_rowcount";
    using storage_type = p_storage;

    /// Count every sequence and store the result in one table chunk.
    static void build(p_storage& storage) {
        std::int64_t count = 0;
        storage.for_each_sequence(
            [&count](auto& /*lazy*/, std::size_t /*rowno*/, std::size_t /*total*/) { ++count; });
        storage.resize(sizeof(std::int64_t)); // one chunk
        storage.template write_to<std::int64_t>(
            0, [&count](std::size_t c, std::int64_t* buf, std::size_t cap) -> std::size_t {
                DEBUG_ASSERT(c == 0 && cap == 1, "test_table_rowcount: single int64 chunk");
                buf[0] = count;
                return c;
            });
    }

    /// Clear the prior value and rebuild the table count.
    static void rebuild(p_storage& storage) {
        storage.clear(index_name);
        build(storage);
    }
};
#endif

} // namespace bioseq::storage::indexing

#ifdef BIOSEQ_TEST_INDEXES
export namespace bioseq::indexing {

/// Index metadata for the integration-only table row-count index.
template <> struct index_descriptor<bioseq::storage::indexing::test_table_count_index> {
    static constexpr sequence_index_layout index_type = sequence_index_layout::table_chunks;
    static constexpr std::string_view index_name = "test_table_rowcount";
    static constexpr bool allows_update_on_insert = false;
};

} // namespace bioseq::indexing
#endif

export namespace bioseq::storage::indexing {

/// Registry of every production index and the integration-only table index.
using pg_supported_indexes =
    bioseq::indexing::supported_indexes<fragment_index_storage,
                                        sequence_index_storage,
                                        table_index_storage,
                                        bioseq::indexing::minimizers_per_fragment_index,
                                        bioseq::indexing::direct_minimizer_bucket_index,
                                        bioseq::indexing::symbol_counts_index
#ifdef BIOSEQ_TEST_INDEXES
                                        ,
                                        test_table_count_index
#endif
                                        >;

} // namespace bioseq::storage::indexing
