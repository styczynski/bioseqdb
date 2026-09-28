/**
 * @file active_read_policy.cppm
 * @brief Record where an active index choice came from.
 */
module;

#include <cstdint>
#include <string_view>

export module bioseq.postgres.storage.indexing.active_read_policy;

export namespace bioseq::storage::indexing {

/// Origin of the active-read index selected for a relation column.
enum class active_read_policy_source : std::uint8_t {
    unresolved,
    non_relation,
    configured_row,
    authoritative_no_row_default,
};

/** Return a policy source's diagnostic name, or "invalid" for an unknown value. */
[[nodiscard]] constexpr std::string_view
active_read_policy_resolution_source_name(const active_read_policy_source source) noexcept {
    switch (source) {
    case active_read_policy_source::unresolved:
        return "unresolved";
    case active_read_policy_source::non_relation:
        return "non_relation";
    case active_read_policy_source::configured_row:
        return "configured_row";
    case active_read_policy_source::authoritative_no_row_default:
        return "authoritative_no_row_default";
    }
    return "invalid";
}

static_assert(active_read_policy_resolution_source_name(active_read_policy_source::unresolved) ==
              "unresolved");
static_assert(active_read_policy_resolution_source_name(active_read_policy_source::non_relation) ==
              "non_relation");
static_assert(active_read_policy_resolution_source_name(
                  active_read_policy_source::configured_row) == "configured_row");
static_assert(active_read_policy_resolution_source_name(
                  active_read_policy_source::authoritative_no_row_default) ==
              "authoritative_no_row_default");

} // namespace bioseq::storage::indexing
