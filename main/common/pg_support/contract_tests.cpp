#include <cstdint>
#include <type_traits>

import bioseq.pg_support;

namespace {

struct oid_entry_contract {
    std::uint32_t key = 0;
    void* value = nullptr;
};

using generic_htab_contract =
    bioseq::pg_support::TxnScopedHtab<std::uint32_t>;
using oid_htab_contract =
    bioseq::pg_support::OidEntryTxnScopedHtab<oid_entry_contract>;

static_assert(sizeof(PList<void>) == sizeof(void*));
static_assert(std::is_standard_layout_v<PList<void>>);
static_assert(sizeof(PBitmapset) == sizeof(void*));
static_assert(std::is_standard_layout_v<PBitmapset>);
static_assert(
    sizeof(bioseq::pg_utils::pg_node_view) == sizeof(void*));
static_assert(
    std::is_standard_layout_v<bioseq::pg_utils::pg_node_view>);

static_assert(
    !std::is_copy_constructible_v<
        bioseq::pg_support::MemoryContextScope>);
static_assert(
    std::is_nothrow_destructible_v<
        bioseq::pg_support::MemoryContextScope>);
static_assert(!std::is_copy_constructible_v<generic_htab_contract>);
static_assert(!std::is_copy_constructible_v<oid_htab_contract>);

static_assert(
    !std::is_copy_constructible_v<
        bioseq::pg_support::ScopedSpiConnection>);
static_assert(
    !std::is_move_constructible_v<
        bioseq::pg_support::ScopedSpiConnection>);
static_assert(
    std::is_nothrow_destructible_v<
        bioseq::pg_support::ScopedSpiConnection>);

}  // namespace
