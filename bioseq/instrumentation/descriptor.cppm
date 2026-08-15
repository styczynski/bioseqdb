/**
 * @file instrumentation/descriptor.cppm
 * @brief Stable metadata for named instrumentation sites.
 *
 * A descriptor is registered once per (name, file, line) tuple via the
 * static-local idiom at the call site:
 *   static const auto* descriptor = register_static_region("ksw2.extend");
 *
 * Registration is cold-path: the first call acquires a mutex, deduplicates
 * against the global hash table, allocates a fresh descriptor block from
 * the global pool when needed, returns a stable pointer. Subsequent calls
 * return the cached pointer (one load + branch).
 */
module;

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <new>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

export module bioseq.instrumentation.descriptor;

import bioseq.instrumentation.configuration;

export namespace bioseq::instrumentation {

/** @brief Category encoded in a metric descriptor. */
enum class metric_kind : std::uint8_t { region, loop, counter, gauge, sample };

/// A descriptor is the immutable metadata for one named profiling site.
/// 64 bytes and cache-line aligned. The fields are laid out so that the
/// hottest reads (id, name, kind, flags) are in the first 32 bytes.
struct alignas(64) region_descriptor {
    std::uint32_t  id;              // assigned at registration; 0 = invalid
    std::uint32_t  name_hash;       // FNV-1a (fast equality during dedup)
    const char*    name;            // points into .rodata or descriptor pool
    const char*    file;
    std::uint32_t  line;
    std::uint16_t  color;           // reserved for visualisation
    metric_kind    kind;
    std::uint8_t   flags;
    std::uint16_t  highway_lanes;   // 0 if not a SIMD region
    std::uint8_t   reserved_after_lanes[6];
    const char*    highway_target;  // nullptr if not a SIMD region
    const char*    arena_name;      // nullptr if not arena-attributed
    std::uint8_t   reserved_tail[8];
};
static_assert(sizeof(region_descriptor) == 64);
static_assert(alignof(region_descriptor) == 64);

/// Loop descriptor extends region_descriptor with iteration-related metadata.
struct alignas(64) loop_descriptor : region_descriptor {
    std::uint32_t  default_subsample_rate;
    std::uint16_t  work_unit_kind;
    std::uint8_t   reserved_tail[58];
};
static_assert(sizeof(loop_descriptor) == 128);

/// Cold-path one-time registration. The static-local idiom at the call
/// site makes the first call run this. Subsequent calls return the cached
/// pointer.
[[nodiscard, gnu::cold]]
inline const region_descriptor* register_static_region(
    std::string_view name,
    std::source_location loc = std::source_location::current()) noexcept;

/** @brief Register or find a counter descriptor for one source location. */
[[nodiscard, gnu::cold]]
inline const region_descriptor* register_static_counter(
    std::string_view name,
    std::source_location loc = std::source_location::current()) noexcept;

/** @brief Register or find a gauge descriptor for one source location. */
[[nodiscard, gnu::cold]]
inline const region_descriptor* register_static_gauge(
    std::string_view name,
    std::source_location loc = std::source_location::current()) noexcept;

/** @brief Register or find a scalar loop descriptor. */
[[nodiscard, gnu::cold]]
inline const loop_descriptor* register_static_loop(
    std::string_view name,
    std::source_location loc = std::source_location::current()) noexcept;

/** @brief Register or find a loop descriptor for a SIMD target and lane count. */
[[nodiscard, gnu::cold]]
inline const loop_descriptor* register_static_simd_loop(
    std::string_view base_name,
    std::string_view target_name,
    std::uint16_t lanes,
    std::source_location loc = std::source_location::current()) noexcept;

/// Lookup by id. Used by the summary writer to walk every registered
/// descriptor.
[[nodiscard]] inline const region_descriptor* find_descriptor(std::uint32_t id) noexcept;

/// Number of descriptors registered so far.
[[nodiscard]] inline std::uint32_t registered_descriptor_count() noexcept;

}  // namespace bioseq::instrumentation

namespace bioseq::instrumentation::detail {

inline constexpr std::uint32_t descriptor_block_size = 256;
inline constexpr std::uint32_t maximum_descriptor_blocks = 4096;

/**
 * @brief Storage slot large enough for either public descriptor layout.
 *
 * The slot keeps loop descriptors from overlapping the following region
 * descriptor while retaining the 64-byte and 128-byte public layouts.
 */
inline constexpr std::size_t descriptor_slot_size = sizeof(loop_descriptor);
inline constexpr std::size_t descriptor_slot_alignment = alignof(loop_descriptor);

struct descriptor_registry {
    std::array<std::atomic<std::byte*>, maximum_descriptor_blocks> blocks{};
    std::atomic<std::uint32_t> next_id{0};
    std::mutex dedup_mutex;
    std::vector<region_descriptor*> dedup_table;
    std::deque<std::string> owned_strings;

    descriptor_registry() {
        dedup_table.reserve(256);
    }
};

[[nodiscard]] inline descriptor_registry& registry() noexcept {
    static descriptor_registry instance;
    return instance;
}

/// FNV-1a 32-bit hash. Used for name dedup and as a name_hash field on
/// the descriptor.
[[nodiscard, gnu::always_inline]]
inline std::uint32_t fnv1a_32(std::string_view value) noexcept {
    std::uint32_t hash = 2166136261u;
    for (char character : value) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= 16777619u;
    }
    return hash;
}

/// Cold-path block allocator. Allocates a fresh 256-descriptor block
/// (16 KiB) when the current block runs out. Allocation uses operator new
/// — the descriptor pool is process-wide and never freed.
[[nodiscard, gnu::cold]]
inline std::byte* allocate_block() noexcept {
    return static_cast<std::byte*>(::operator new(
        descriptor_slot_size * descriptor_block_size,
        std::align_val_t{descriptor_slot_alignment}));
}

[[nodiscard, gnu::cold]]
inline region_descriptor* register_impl(
    std::string_view name,
    std::source_location loc,
    metric_kind kind,
    std::string_view target_name,
    std::uint16_t lanes) noexcept
{
    auto& descriptor_registry = registry();
    std::lock_guard<std::mutex> lock(descriptor_registry.dedup_mutex);

    const auto hash = fnv1a_32(name);

    // Linear search the dedup table. Typical sizes are ~50 — small enough.
    for (auto* descriptor : descriptor_registry.dedup_table) {
        if (descriptor->name_hash == hash
            && descriptor->line == loc.line()
            && descriptor->kind == kind
            && std::string_view{descriptor->name} == name
            && descriptor->highway_lanes == lanes
            && (target_name.empty()
                ? (descriptor->highway_target == nullptr)
                : (descriptor->highway_target != nullptr
                   && std::string_view{descriptor->highway_target} == target_name)))
        {
            return descriptor;
        }
    }

    // raw_id is the 0-based slot index across the descriptor table
    // (next_id starts at 0). The stored desc->id is shifted +1 so that
    // 0 stays reserved as the "invalid" sentinel for callers that pass
    // a default-constructed region_measurement.desc.
    const auto raw_id = descriptor_registry.next_id.fetch_add(1, std::memory_order_relaxed);
    const auto block_idx = raw_id / descriptor_block_size;
    const auto slot_idx  = raw_id % descriptor_block_size;
    if (block_idx >= maximum_descriptor_blocks) {
        // Out of descriptor space. Return a sentinel (the first descriptor)
        // so the caller's static-local stays non-null but loses uniqueness.
        // Practically unreachable.
        auto* first_block = descriptor_registry.blocks[0].load(std::memory_order_acquire);
        return reinterpret_cast<region_descriptor*>(first_block);
    }

    auto* block = descriptor_registry.blocks[block_idx].load(std::memory_order_acquire);
    if (block == nullptr) {
        block = allocate_block();
        descriptor_registry.blocks[block_idx].store(block, std::memory_order_release);
    }
    void* const slot = static_cast<void*>(
        block + slot_idx * descriptor_slot_size);
    auto* descriptor = kind == metric_kind::loop
        ? static_cast<region_descriptor*>(new (slot) loop_descriptor{})
        : new (slot) region_descriptor{};

    descriptor_registry.owned_strings.emplace_back(name);
    descriptor->id = raw_id + 1;
    descriptor->name_hash = hash;
    descriptor->name = descriptor_registry.owned_strings.back().c_str();
    descriptor->file = loc.file_name();
    descriptor->line = loc.line();
    descriptor->color = 0;
    descriptor->kind = kind;
    descriptor->flags = 0;
    descriptor->highway_lanes = lanes;
    if (target_name.empty()) {
        descriptor->highway_target = nullptr;
    } else {
        descriptor_registry.owned_strings.emplace_back(target_name);
        descriptor->highway_target = descriptor_registry.owned_strings.back().c_str();
    }
    descriptor->arena_name = nullptr;

    descriptor_registry.dedup_table.push_back(descriptor);
    return descriptor;
}

}  // namespace bioseq::instrumentation::detail

namespace bioseq::instrumentation {

[[nodiscard, gnu::cold]]
inline const region_descriptor* register_static_region(
    std::string_view name,
    std::source_location loc) noexcept
{
    return detail::register_impl(name, loc, metric_kind::region, {}, 0);
}

[[nodiscard, gnu::cold]]
inline const region_descriptor* register_static_counter(
    std::string_view name,
    std::source_location loc) noexcept
{
    return detail::register_impl(name, loc, metric_kind::counter, {}, 0);
}

[[nodiscard, gnu::cold]]
inline const region_descriptor* register_static_gauge(
    std::string_view name,
    std::source_location loc) noexcept
{
    return detail::register_impl(name, loc, metric_kind::gauge, {}, 0);
}

[[nodiscard, gnu::cold]]
inline const loop_descriptor* register_static_loop(
    std::string_view name,
    std::source_location loc) noexcept
{
    auto* descriptor = detail::register_impl(name, loc, metric_kind::loop, {}, 0);
    return static_cast<const loop_descriptor*>(descriptor);
}

[[nodiscard, gnu::cold]]
inline const loop_descriptor* register_static_simd_loop(
    std::string_view base_name,
    std::string_view target_name,
    std::uint16_t lanes,
    std::source_location loc) noexcept
{
    auto* descriptor = detail::register_impl(base_name, loc, metric_kind::loop, target_name, lanes);
    return static_cast<const loop_descriptor*>(descriptor);
}

[[nodiscard]]
inline const region_descriptor* find_descriptor(std::uint32_t id) noexcept {
    if (id == 0) return nullptr;
    const auto raw_id = id - 1;
    const auto block_idx = raw_id / detail::descriptor_block_size;
    const auto slot_idx  = raw_id % detail::descriptor_block_size;
    if (block_idx >= detail::maximum_descriptor_blocks) return nullptr;
    auto* block = detail::registry().blocks[block_idx].load(std::memory_order_acquire);
    if (block == nullptr) return nullptr;
    return reinterpret_cast<const region_descriptor*>(
        block + slot_idx * detail::descriptor_slot_size);
}

[[nodiscard]]
inline std::uint32_t registered_descriptor_count() noexcept {
    return detail::registry().next_id.load(std::memory_order_relaxed);
}

}  // namespace bioseq::instrumentation
