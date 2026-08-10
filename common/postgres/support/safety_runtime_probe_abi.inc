/**
 * @file common/postgres/support/safety_runtime_probe_abi.inc
 * @brief Shared fixed-layout ABI for PostgreSQL ERROR-safety instrumentation.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace bioseq::postgres::support::safety_runtime_probe_abi {

/** Current independently compiled probe ABI revision. */
inline constexpr std::uint32_t k_abi_revision = 2;

/** PostgreSQL rendezvous variable shared with the runtime safety harness. */
inline constexpr char k_rendezvous_name[] = "bioseq.safety_runtime_probe";

/** Event categories recorded by the PostgreSQL error-safety harness. */
enum class event_kind : std::uint32_t {
    destructor = 1,
    cleanup_complete = 2,
    postgres_cleanup = 3,
};

/** Records one named runtime-safety event. */
using record_event_function = void (*)(std::uint32_t kind,
                                       const char* name,
                                       std::size_t name_size) noexcept;

/** Records arrival at one generated runtime-safety site. */
using record_site_visit_function = void (*)(const char* site_reference,
                                            std::size_t site_reference_size) noexcept;

/** Consumes one named injected fault and reports whether it was armed. */
using consume_fault_function = std::uint32_t (*)(const char* fault_key,
                                                 std::size_t fault_key_size) noexcept;

/** Fixed-layout callback table published through PostgreSQL rendezvous. */
struct probe_api final {
    /** ABI revision expected by both publisher and consumer. */
    std::uint32_t abi_revision;

    /** Complete byte size of this callback table. */
    std::uint32_t struct_size;

    /** Event recorder supplied by the instrumentation harness. */
    record_event_function record_event;

    /** Generated-site recorder supplied by the instrumentation harness. */
    record_site_visit_function record_site_visit;

    /** Fault-consumption callback supplied by the instrumentation harness. */
    consume_fault_function consume_fault;
};

static_assert(std::is_standard_layout_v<probe_api>);
static_assert(offsetof(probe_api, abi_revision) == 0);
static_assert(offsetof(probe_api, struct_size) == sizeof(std::uint32_t));
static_assert(offsetof(probe_api, record_event) == 2 * sizeof(std::uint32_t));
static_assert(offsetof(probe_api, record_site_visit) ==
              (2 * sizeof(std::uint32_t)) + sizeof(record_event_function));
static_assert(offsetof(probe_api, consume_fault) == (2 * sizeof(std::uint32_t)) +
                                                        sizeof(record_event_function) +
                                                        sizeof(record_site_visit_function));
static_assert(sizeof(probe_api) == (2 * sizeof(std::uint32_t)) + sizeof(record_event_function) +
                                       sizeof(record_site_visit_function) +
                                       sizeof(consume_fault_function));
static_assert(sizeof(probe_api) <= 64);

} // namespace bioseq::postgres::support::safety_runtime_probe_abi
