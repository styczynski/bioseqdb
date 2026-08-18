/**
 * @file active_read_policy.cppm
 * @brief Declares the authority behind one active-read index selection.
 */
export module bioseq.postgres.storage.indexing.active_read_policy;

import std;

export namespace bioseq::storage::indexing {

/// Origin of the active-read index selected for a relation column.
enum class active_read_policy_source : std::uint8_t {
    unresolved,
    non_relation,
    configured_row,
    authoritative_no_row_default,
};

/**
 * @brief Returns the stable diagnostic name for a policy source.
 * @param[in] source Policy source to name.
 * @return A static string view, or `invalid` for an unknown enumerator.
 */
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
