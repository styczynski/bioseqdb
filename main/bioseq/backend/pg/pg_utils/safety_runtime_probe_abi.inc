#pragma once

#include <cstddef>
#include <cstdint>

namespace bioseq::pg_utils::safety_runtime_probe_abi {

inline constexpr std::uint32_t version = 2;
inline constexpr char rendezvous_name[] =
    "bioseq.safety_runtime_probe.v2";

enum class event_kind : std::uint32_t {
    destructor = 1,
    cleanup_complete = 2,
    postgres_cleanup = 3,
};

using record_event_fn = void (*)(
    std::uint32_t kind,
    const char* name,
    std::size_t name_size) noexcept;

using record_site_visit_fn = void (*)(
    const char* site_reference,
    std::size_t site_reference_size) noexcept;

using consume_fault_fn = std::uint32_t (*)(
    const char* fault_key,
    std::size_t fault_key_size) noexcept;

struct api_v2 final {
    std::uint32_t abi_version;
    std::uint32_t struct_size;
    record_event_fn record_event;
    record_site_visit_fn record_site_visit;
    consume_fault_fn consume_fault;
};

static_assert(sizeof(api_v2) <= 64);

}  // namespace bioseq::pg_utils::safety_runtime_probe_abi
