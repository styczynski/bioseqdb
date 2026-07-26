/*-------------------------------------------------------------------------
 *
 * runtime_test_counters.cppm
 *
 * Test-build observability for BPCHAR/vector ownership transitions.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <cstdint>

export module bioseq.pg_utils.runtime_test_counters;

export namespace bioseq::pg_utils::runtime_test_counters {

struct bpchar_source_form_snapshot final {
    std::uint64_t short_datums = 0;
    std::uint64_t four_byte_uncompressed_datums = 0;
    std::uint64_t compressed_datums = 0;
    std::uint64_t external_datums = 0;
    std::uint64_t unknown_datums = 0;
};

struct bpchar_lifetime_snapshot final {
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
    bpchar_source_form_snapshot vector_copy_sources{};
    bpchar_source_form_snapshot view_decode_sources{};
};

enum class bpchar_source_form : std::uint8_t {
    short_header,
    four_byte_uncompressed,
    compressed,
    external,
    unknown,
};

enum class bpchar_source_site : std::uint8_t {
    vector_copy,
    view_decode,
};

enum class vector_payload_cleanup_kind : std::uint8_t {
    explicit_cleanup,
    slot_clear,
    slot_release,
};

#if defined(BIOSEQ_TEST_INDEXES)
inline thread_local bpchar_lifetime_snapshot
    bpchar_lifetime_state;

inline void reset_bpchar_lifetime() noexcept {
    bpchar_lifetime_state = {};
}

inline void begin_bpchar_source_observation() noexcept {
    bpchar_lifetime_state.source_observation_active = true;
    ++bpchar_lifetime_state.source_observation_sessions;
}

inline void end_bpchar_source_observation() noexcept {
    if (bpchar_lifetime_state.source_observation_active) {
        bpchar_lifetime_state.source_observation_active = false;
        ++bpchar_lifetime_state.source_observation_completions;
    }
}

[[nodiscard]] inline bool
bpchar_source_observation_active() noexcept {
    return bpchar_lifetime_state.source_observation_active;
}

inline void note_view_construction(bool owned_copy) noexcept {
    ++bpchar_lifetime_state.live_views;
    ++bpchar_lifetime_state.view_constructions;
    if (owned_copy)
        ++bpchar_lifetime_state.view_owned_copies;
}

inline void note_view_destruction(bool owned_copy) noexcept {
    if (bpchar_lifetime_state.live_views != 0)
        --bpchar_lifetime_state.live_views;
    ++bpchar_lifetime_state.view_destructions;
    if (owned_copy)
        ++bpchar_lifetime_state.view_owned_frees;
}

inline void note_vector_payload_allocation() noexcept {
    ++bpchar_lifetime_state.vector_payload_allocations;
}

inline void note_payload_free() noexcept {
    if (bpchar_lifetime_state.live_views != 0)
        ++bpchar_lifetime_state.payload_frees_while_view_live;
}

inline void note_vector_payload_overwrite_free() noexcept {
    note_payload_free();
    ++bpchar_lifetime_state.vector_payload_overwrite_frees;
}

inline void note_vector_payload_cleanup_free(
        vector_payload_cleanup_kind kind) noexcept {
    note_payload_free();
    ++bpchar_lifetime_state.vector_payload_cleanup_frees;
    switch (kind) {
        case vector_payload_cleanup_kind::explicit_cleanup:
            ++bpchar_lifetime_state
                .vector_payload_explicit_cleanup_frees;
            break;
        case vector_payload_cleanup_kind::slot_clear:
            ++bpchar_lifetime_state.vector_payload_clear_frees;
            break;
        case vector_payload_cleanup_kind::slot_release:
            ++bpchar_lifetime_state.vector_payload_release_frees;
            break;
    }
}

inline void note_vector_cleanup_call(
        vector_payload_cleanup_kind kind) noexcept {
    switch (kind) {
        case vector_payload_cleanup_kind::explicit_cleanup:
            ++bpchar_lifetime_state.explicit_cleanup_calls;
            break;
        case vector_payload_cleanup_kind::slot_clear:
            ++bpchar_lifetime_state.slot_clear_calls;
            break;
        case vector_payload_cleanup_kind::slot_release:
            ++bpchar_lifetime_state.slot_release_calls;
            break;
    }
}

inline void note_detoast_temporary_free() noexcept {
    ++bpchar_lifetime_state.detoast_temporary_frees;
}

inline void note_bpchar_source_form(
        bpchar_source_form form,
        bpchar_source_site site) noexcept {
    if (!bpchar_lifetime_state.source_observation_active) {
        return;
    }
    bpchar_source_form_snapshot& site_snapshot =
        site == bpchar_source_site::vector_copy
        ? bpchar_lifetime_state.vector_copy_sources
        : bpchar_lifetime_state.view_decode_sources;
    switch (form) {
        case bpchar_source_form::short_header:
            ++bpchar_lifetime_state.source_short_datums;
            ++site_snapshot.short_datums;
            break;
        case bpchar_source_form::four_byte_uncompressed:
            ++bpchar_lifetime_state
                .source_four_byte_uncompressed_datums;
            ++site_snapshot.four_byte_uncompressed_datums;
            break;
        case bpchar_source_form::compressed:
            ++bpchar_lifetime_state.source_compressed_datums;
            ++site_snapshot.compressed_datums;
            break;
        case bpchar_source_form::external:
            ++bpchar_lifetime_state.source_external_datums;
            ++site_snapshot.external_datums;
            break;
        case bpchar_source_form::unknown:
            ++bpchar_lifetime_state.source_unknown_datums;
            ++site_snapshot.unknown_datums;
            break;
    }
}

inline void note_bpchar_context_reset() noexcept {
    ++bpchar_lifetime_state.context_resets;
}

inline void note_bpchar_context_reuse() noexcept {
    ++bpchar_lifetime_state.context_reuses;
}

[[nodiscard]] inline bpchar_lifetime_snapshot
bpchar_lifetime() noexcept {
    return bpchar_lifetime_state;
}
#else
inline void reset_bpchar_lifetime() noexcept {}
inline void begin_bpchar_source_observation() noexcept {}
inline void end_bpchar_source_observation() noexcept {}
[[nodiscard]] inline bool
bpchar_source_observation_active() noexcept {
    return false;
}
inline void note_view_construction(bool) noexcept {}
inline void note_view_destruction(bool) noexcept {}
inline void note_vector_payload_allocation() noexcept {}
inline void note_vector_payload_overwrite_free() noexcept {}
inline void note_vector_payload_cleanup_free(
    vector_payload_cleanup_kind) noexcept {}
inline void note_vector_cleanup_call(
    vector_payload_cleanup_kind) noexcept {}
inline void note_detoast_temporary_free() noexcept {}
inline void note_bpchar_source_form(
    bpchar_source_form, bpchar_source_site) noexcept {}
inline void note_bpchar_context_reset() noexcept {}
inline void note_bpchar_context_reuse() noexcept {}

[[nodiscard]] inline bpchar_lifetime_snapshot
bpchar_lifetime() noexcept {
    return {};
}
#endif

}  // namespace bioseq::pg_utils::runtime_test_counters
