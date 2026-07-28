/*-------------------------------------------------------------------------
 *
 * pg_utils/safety_runtime_probe.cppm
 *
 * Evidence-only observability for generated SQL-boundary runtime tests.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

extern "C" {
#include "fmgr.h"
}

#include "safety_runtime_probe_abi.inc"

#include <cstddef>
#include <cstdint>
#include <string_view>

export module bioseq.pg_utils.safety_runtime_probe;

#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES) \
    && !defined(BIOSEQ_SAFETY_RUNTIME_PROFILE)
#error "BIOSEQ_SAFETY_RUNTIME_PROBES requires BIOSEQ_SAFETY_RUNTIME_PROFILE"
#endif

export namespace bioseq::pg_utils::safety_runtime_probe {

namespace detail {

[[nodiscard]] inline const safety_runtime_probe_abi::api_v2*
active_api() noexcept {
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
    void** const slot = find_rendezvous_variable(
        safety_runtime_probe_abi::rendezvous_name);
    if (slot == nullptr || *slot == nullptr) {
        return nullptr;
    }
    const auto* api =
        static_cast<const safety_runtime_probe_abi::api_v2*>(*slot);
    if (api->abi_version != safety_runtime_probe_abi::version
        || api->struct_size != sizeof(*api)
        || api->record_event == nullptr
        || api->record_site_visit == nullptr
        || api->consume_fault == nullptr) {
        return nullptr;
    }
    return api;
#else
    return nullptr;
#endif
}

inline void record(
        safety_runtime_probe_abi::event_kind kind,
        std::string_view name) noexcept {
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
    const auto* api = active_api();
    if (api != nullptr) {
        api->record_event(
            static_cast<std::uint32_t>(kind),
            name.data(),
            name.size());
    }
#else
    static_cast<void>(kind);
    static_cast<void>(name);
#endif
}

}  // namespace detail

/** Record one C++ destructor that actually ran in the armed invocation. */
inline void record_destructor(std::string_view name) noexcept {
    detail::record(
        safety_runtime_probe_abi::event_kind::destructor,
        name);
}

/** Record successful completion of one active PostgreSQL cleanup action. */
inline void record_cleanup_complete(std::string_view name) noexcept {
    detail::record(
        safety_runtime_probe_abi::event_kind::cleanup_complete,
        name);
}

/**
 * Record arrival at one immutable runtime probe site.
 *
 * Generated boundaries pass the sealed site ID. Shared bridge code passes
 * its authenticated literal fault key, which the armed control DSO resolves
 * to that case's sealed inventory entry.
 */
inline void record_site_visit(
        std::string_view site_reference) noexcept {
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
    const auto* api = detail::active_api();
    if (api != nullptr) {
        api->record_site_visit(
            site_reference.data(), site_reference.size());
    }
#else
    static_cast<void>(site_reference);
#endif
}

/** Consume a named evidence-only injected fault at most once. */
[[nodiscard]] inline bool consume_fault(
        std::string_view fault_key) noexcept {
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
    const auto* api = detail::active_api();
    return api != nullptr
        && api->consume_fault(
               fault_key.data(), fault_key.size())
            == 1;
#else
    static_cast<void>(fault_key);
    return false;
#endif
}

/** A test-only C++ lifetime whose destructor records real unwinding. */
class destructor_sentinel final {
public:
    explicit destructor_sentinel(
            std::string_view name) noexcept
        : name_{name} {}

    destructor_sentinel(const destructor_sentinel&) = delete;
    destructor_sentinel& operator=(const destructor_sentinel&) = delete;
    destructor_sentinel(destructor_sentinel&&) = delete;
    destructor_sentinel& operator=(destructor_sentinel&&) = delete;

    ~destructor_sentinel() noexcept {
        record_destructor(name_);
    }

private:
    std::string_view name_;
};

}  // namespace bioseq::pg_utils::safety_runtime_probe
