/**
 * @file common/postgres/support/safety_runtime_probe.cppm
 * @brief Record cleanup events and inject faults in PostgreSQL error tests.
 */
module;

extern "C" {
#include "postgres.h"

#include "fmgr.h"
}

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "safety_runtime_probe_abi.inc"

export module bioseq.postgres.support:safety_runtime_probe;

#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES) && !defined(BIOSEQ_SAFETY_RUNTIME_PROFILE)
#error "BIOSEQ_SAFETY_RUNTIME_PROBES requires BIOSEQ_SAFETY_RUNTIME_PROFILE"
#endif

export namespace bioseq::postgres::support::safety_runtime_probe {

namespace detail {

/**
 * Find the test callbacks when runtime probes are enabled.
 * Return nullptr if the table is absent or its revision, size  or required
 * callbacks do not match.
 */
[[nodiscard]] inline const safety_runtime_probe_abi::probe_api* find_active_api() noexcept {
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
    void** const slot = find_rendezvous_variable(safety_runtime_probe_abi::k_rendezvous_name);
    if (slot == nullptr || *slot == nullptr) {
        return nullptr;
    }
    const auto* api = static_cast<const safety_runtime_probe_abi::probe_api*>(*slot);
    if (api->abi_revision != safety_runtime_probe_abi::k_abi_revision ||
        api->struct_size != sizeof(*api) || api->record_event == nullptr ||
        api->record_site_visit == nullptr || api->consume_fault == nullptr) {
        return nullptr;
    }
    return api;
#else
    return nullptr;
#endif
}

/** Send an event to the test harness when probes are enabled and registered. */
inline void record_event(safety_runtime_probe_abi::event_kind kind,
                         std::string_view name) noexcept {
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
    const auto* api = find_active_api();
    if (api != nullptr) {
        api->record_event(static_cast<std::uint32_t>(kind), name.data(), name.size());
    }
#else
    static_cast<void>(kind);
    static_cast<void>(name);
#endif
}

} // namespace detail

/** Records one C++ destructor that ran during an instrumented invocation. */
inline void record_destructor(std::string_view name) noexcept {
    detail::record_event(safety_runtime_probe_abi::event_kind::destructor, name);
}

/** Records successful completion of one PostgreSQL cleanup action. */
inline void record_cleanup_complete(std::string_view name) noexcept {
    detail::record_event(safety_runtime_probe_abi::event_kind::cleanup_complete, name);
}

/** Record that execution reached a generated safety-check site */
inline void record_site_visit(std::string_view site_reference) noexcept {
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
    const auto* api = detail::find_active_api();
    if (api != nullptr) {
        api->record_site_visit(site_reference.data(), site_reference.size());
    }
#else
    static_cast<void>(site_reference);
#endif
}

/** Consumes a named injected fault at most once. */
[[nodiscard]] inline bool consume_fault(std::string_view fault_key) noexcept {
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
    const auto* api = detail::find_active_api();
    return api != nullptr && api->consume_fault(fault_key.data(), fault_key.size()) == 1;
#else
    static_cast<void>(fault_key);
    return false;
#endif
}

/** Test object that records when its C++ destructor runs. */
class destructor_sentinel final {
  public:
    /** Borrow an event name that must stay alive until this object is destroyed. */
    explicit destructor_sentinel(std::string_view name) noexcept : name_{name} {}

    destructor_sentinel(const destructor_sentinel&) = delete;
    destructor_sentinel& operator=(const destructor_sentinel&) = delete;
    destructor_sentinel(destructor_sentinel&&) = delete;
    destructor_sentinel& operator=(destructor_sentinel&&) = delete;

    /** Records the destructor event. */
    ~destructor_sentinel() noexcept {
        record_destructor(name_);
    }

  private:
    std::string_view name_;
};

} // namespace bioseq::postgres::support::safety_runtime_probe
