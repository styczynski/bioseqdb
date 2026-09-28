/**
 * @file runtime_diagnostics.cppm
 * @brief Test counters for BPCHAR storage, views and vector cleanup.
 * Production builds keep the same calls but do not collect statistics.
 */
module;

#include <cstdint>

export module bioseq.postgres.support.runtime_diagnostics;

export namespace bioseq::postgres::support::runtime_diagnostics {

/** Counts BPCHAR values by PostgreSQL storage form. */
struct bpchar_source_form_statistics final {
    std::uint64_t short_datums = 0;
    std::uint64_t four_byte_uncompressed_datums = 0;
    std::uint64_t compressed_datums = 0;
    std::uint64_t external_datums = 0;
    std::uint64_t unknown_datums = 0;
};

/** Counts BPCHAR views, copied data and cleanup operations. */
struct bpchar_lifetime_statistics final {
    std::uint64_t live_views = 0;
    std::uint64_t view_constructions = 0;
    std::uint64_t view_destructions = 0;
    std::uint64_t view_owned_copies = 0;
    std::uint64_t view_owned_frees = 0;
    std::uint64_t vector_payload_allocations = 0;
    std::uint64_t vector_payload_overwrite_frees = 0;
    std::uint64_t vector_payload_cleanup_frees = 0;
    std::uint64_t vector_payload_explicit_cleanup_frees = 0;
    std::uint64_t vector_payload_clear_frees = 0;
    std::uint64_t vector_payload_release_frees = 0;
    std::uint64_t detoast_temporary_frees = 0;
    std::uint64_t payload_frees_while_view_live = 0;
    std::uint64_t explicit_cleanup_calls = 0;
    std::uint64_t slot_clear_calls = 0;
    std::uint64_t slot_release_calls = 0;
    std::uint64_t context_resets = 0;
    std::uint64_t context_reuses = 0;
    std::uint64_t source_observation_sessions = 0;
    std::uint64_t source_observation_completions = 0;
    bool source_observation_active = false;
    std::uint64_t source_short_datums = 0;
    std::uint64_t source_four_byte_uncompressed_datums = 0;
    std::uint64_t source_compressed_datums = 0;
    std::uint64_t source_external_datums = 0;
    std::uint64_t source_unknown_datums = 0;
    bpchar_source_form_statistics vector_copy_sources{};
    bpchar_source_form_statistics view_decode_sources{};
};

/** Storage form of a PostgreSQL BPCHAR value */
enum class bpchar_source_form : std::uint8_t {
    short_header,
    four_byte_uncompressed,
    compressed,
    external,
    unknown,
};

/** Place where a BPCHAR value was inspected. */
enum class bpchar_source_site : std::uint8_t {
    vector_copy,
    view_decode,
};

/** @brief Identifies the operation responsible for vector payload cleanup. */
enum class vector_payload_cleanup_kind : std::uint8_t {
    explicit_cleanup,
    slot_clear,
    slot_release,
};

#if defined(BIOSEQ_TEST_INDEXES)
inline thread_local bpchar_lifetime_statistics
    bpchar_lifetime_statistics_state;

/** @brief Resets statistics for the current backend thread. */
inline void reset_bpchar_lifetime_statistics() noexcept {
    bpchar_lifetime_statistics_state = {};
}

/** Start recording the storage forms seen by BPCHAR readers. */
inline void begin_bpchar_source_observation_session() noexcept {
    bpchar_lifetime_statistics_state.source_observation_active = true;
    ++bpchar_lifetime_statistics_state.source_observation_sessions;
}

/** Stop recording BPCHAR storage forms and count the completed session. */
inline void end_bpchar_source_observation_session() noexcept {
    if (bpchar_lifetime_statistics_state.source_observation_active) {
        bpchar_lifetime_statistics_state.source_observation_active = false;
        ++bpchar_lifetime_statistics_state.source_observation_completions;
    }
}

/** @return Whether BPCHAR storage forms are  currently being counted. */
[[nodiscard]] inline bool
is_bpchar_source_observation_active() noexcept {
    return bpchar_lifetime_statistics_state.source_observation_active;
}

/** @brief Records construction of one BPCHAR view. */
inline void record_view_construction(bool owned_copy) noexcept {
    ++bpchar_lifetime_statistics_state.live_views;
    ++bpchar_lifetime_statistics_state.view_constructions;
    if (owned_copy)
        ++bpchar_lifetime_statistics_state.view_owned_copies;
}

/** @brief Records destruction of one BPCHAR view. */
inline void record_view_destruction(bool owned_copy) noexcept {
    if (bpchar_lifetime_statistics_state.live_views != 0)
        --bpchar_lifetime_statistics_state.live_views;
    ++bpchar_lifetime_statistics_state.view_destructions;
    if (owned_copy)
        ++bpchar_lifetime_statistics_state.view_owned_frees;
}

/** @brief Records allocation of one vector-owned payload. */
inline void record_vector_payload_allocation() noexcept {
    ++bpchar_lifetime_statistics_state.vector_payload_allocations;
}

/** Count, a payload free that occurs while any BPCHAR view is alive. */
inline void record_payload_free() noexcept {
    if (bpchar_lifetime_statistics_state.live_views != 0)
        ++bpchar_lifetime_statistics_state.payload_frees_while_view_live;
}

/** @brief Records payload release caused by vector-slot overwrite. */
inline void record_vector_payload_overwrite_free() noexcept {
    record_payload_free();
    ++bpchar_lifetime_statistics_state.vector_payload_overwrite_frees;
}

/** Count a payload frev under the supplied cleanup operation. */
inline void record_vector_payload_cleanup_free(
        vector_payload_cleanup_kind kind) noexcept {
    record_payload_free();
    ++bpchar_lifetime_statistics_state.vector_payload_cleanup_frees;
    switch (kind) {
        case vector_payload_cleanup_kind::explicit_cleanup:
            ++bpchar_lifetime_statistics_state
                .vector_payload_explicit_cleanup_frees;
            break;
        case vector_payload_cleanup_kind::slot_clear:
            ++bpchar_lifetime_statistics_state.vector_payload_clear_frees;
            break;
        case vector_payload_cleanup_kind::slot_release:
            ++bpchar_lifetime_statistics_state.vector_payload_release_frees;
            break;
    }
}

/** @brief Records invocation of a vector cleanup operation. */
inline void record_vector_cleanup(
        vector_payload_cleanup_kind kind) noexcept {
    switch (kind) {
        case vector_payload_cleanup_kind::explicit_cleanup:
            ++bpchar_lifetime_statistics_state.explicit_cleanup_calls;
            break;
        case vector_payload_cleanup_kind::slot_clear:
            ++bpchar_lifetime_statistics_state.slot_clear_calls;
            break;
        case vector_payload_cleanup_kind::slot_release:
            ++bpchar_lifetime_statistics_state.slot_release_calls;
            break;
    }
}

/** @brief Records release of one temporary detoast allocation. */
inline void record_detoast_temporary_free() noexcept {
    ++bpchar_lifetime_statistics_state.detoast_temporary_frees;
}

/** Count a BPCHAR storage form at the specified site while observation is enabled. */
inline void record_bpchar_source_form(
        bpchar_source_form form,
        bpchar_source_site site) noexcept {
    if (!bpchar_lifetime_statistics_state.source_observation_active) {
        return;
    }
    bpchar_source_form_statistics& site_statistics =
        site == bpchar_source_site::vector_copy
        ? bpchar_lifetime_statistics_state.vector_copy_sources
        : bpchar_lifetime_statistics_state.view_decode_sources;
    switch (form) {
        case bpchar_source_form::short_header:
            ++bpchar_lifetime_statistics_state.source_short_datums;
            ++site_statistics.short_datums;
            break;
        case bpchar_source_form::four_byte_uncompressed:
            ++bpchar_lifetime_statistics_state
                .source_four_byte_uncompressed_datums;
            ++site_statistics.four_byte_uncompressed_datums;
            break;
        case bpchar_source_form::compressed:
            ++bpchar_lifetime_statistics_state.source_compressed_datums;
            ++site_statistics.compressed_datums;
            break;
        case bpchar_source_form::external:
            ++bpchar_lifetime_statistics_state.source_external_datums;
            ++site_statistics.external_datums;
            break;
        case bpchar_source_form::unknown:
            ++bpchar_lifetime_statistics_state.source_unknown_datums;
            ++site_statistics.unknown_datums;
            break;
    }
}

/** @brief Records reset of the BPCHAR allocation context. */
inline void record_bpchar_context_reset() noexcept {
    ++bpchar_lifetime_statistics_state.context_resets;
}

/** @brief Records reuse of the BPCHAR allocation context. */
inline void record_bpchar_context_reuse() noexcept {
    ++bpchar_lifetime_statistics_state.context_reuses;
}

/** @return A copy of the current backend-thread statistics. */
[[nodiscard]] inline bpchar_lifetime_statistics
snapshot_bpchar_lifetime_statistics() noexcept {
    return bpchar_lifetime_statistics_state;
}
#else
inline void reset_bpchar_lifetime_statistics() noexcept {}
inline void begin_bpchar_source_observation_session() noexcept {}
inline void end_bpchar_source_observation_session() noexcept {}
[[nodiscard]] inline bool
is_bpchar_source_observation_active() noexcept {
    return false;
}
inline void record_view_construction(bool) noexcept {}
inline void record_view_destruction(bool) noexcept {}
inline void record_vector_payload_allocation() noexcept {}
inline void record_vector_payload_overwrite_free() noexcept {}
inline void record_vector_payload_cleanup_free(
    vector_payload_cleanup_kind) noexcept {}
inline void record_vector_cleanup(
    vector_payload_cleanup_kind) noexcept {}
inline void record_detoast_temporary_free() noexcept {}
inline void record_bpchar_source_form(
    bpchar_source_form, bpchar_source_site) noexcept {}
inline void record_bpchar_context_reset() noexcept {}
inline void record_bpchar_context_reuse() noexcept {}

[[nodiscard]] inline bpchar_lifetime_statistics
snapshot_bpchar_lifetime_statistics() noexcept {
    return {};
}
#endif

}  // namespace bioseq::postgres::support::runtime_diagnostics
