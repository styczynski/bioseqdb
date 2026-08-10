#include <cstdint>
#include <type_traits>

import bioseq.postgres.support;

namespace {

struct oid_entry_contract {
    std::uint32_t key = 0;
    void* value = nullptr;
};

using oid_htab_contract =
    bioseq::postgres::support::transaction_scoped_hash_table<oid_entry_contract>;

static_assert(sizeof(bioseq::postgres::support::postgres_list<void>) == sizeof(void*));
static_assert(std::is_standard_layout_v<bioseq::postgres::support::postgres_list<void>>);
static_assert(sizeof(bioseq::postgres::support::postgres_bitmapset) == sizeof(void*));
static_assert(std::is_standard_layout_v<bioseq::postgres::support::postgres_bitmapset>);
static_assert(sizeof(bioseq::postgres::support::pg_node_view) == sizeof(void*));
static_assert(std::is_standard_layout_v<bioseq::postgres::support::pg_node_view>);

static_assert(!std::is_copy_constructible_v<bioseq::postgres::support::memory_context_scope>);
static_assert(std::is_nothrow_destructible_v<bioseq::postgres::support::memory_context_scope>);
static_assert(!std::is_copy_constructible_v<oid_htab_contract>);

static_assert(!std::is_copy_constructible_v<bioseq::postgres::support::scoped_spi_connection>);
static_assert(!std::is_move_constructible_v<bioseq::postgres::support::scoped_spi_connection>);
static_assert(std::is_nothrow_destructible_v<bioseq::postgres::support::scoped_spi_connection>);

} // namespace
