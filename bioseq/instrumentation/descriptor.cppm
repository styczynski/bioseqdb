/**
 * @file instrumentation/descriptor.cppm
 * @brief Register names and source locations for profiling measurements.
 *
 * Registration reuses matching descriptors and stores new ones at stable
 * addresses for later lookup.
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

export namespace bioseq::instrumentation {

/** @brief Category encoded in a metric descriptor. */
enum class metric_kind : std::uint8_t { region, loop, counter, gauge, sample };

/** @brief Name and source details for one profiling site, aligned to 64 bytes. */
struct alignas(64) region_descriptor {
    std::uint32_t  id;              ///< Registration id, with zero reserved as invalid.
    std::uint32_t  name_hash;       ///< FNV-1a hash used during deduplication.
    const char*    name;            ///< Name stored by the registry.
    const char*    file;
    std::uint32_t  line;
    std::uint16_t  color;           ///< Reserved visualization color.
    metric_kind    kind;
    std::uint8_t   flags;
    std::uint16_t  highway_lanes;   ///< SIMD lane count, or zero for scalar sites.
    std::uint8_t   reserved_after_lanes[6];
    const char*    highway_target;  ///< SIMD target name, or null for scalar sites.
    const char*    arena_name;      ///< Arena name, or null without attribution.
    std::uint8_t   reserved_tail[8];
};
static_assert(sizeof(region_descriptor) == 64);
static_assert(alignof(region_descriptor) == 64);

/// Region metadata with default loop sampling and work-unit settings.
struct alignas(64) loop_descriptor : region_descriptor {
    std::uint32_t  default_subsample_rate;
    std::uint16_t  work_unit_kind;
    std::uint8_t   reserved_tail[58];
};
static_assert(sizeof(loop_descriptor) == 128);

}  // namespace bioseq::instrumentation

namespace bioseq::instrumentation::detail {

inline constexpr std::uint32_t descriptor_block_size = 256;
inline constexpr std::uint32_t maximum_descriptor_blocks = 4096;

/**
 * @brief Slot size that fits a loop or region descriptor.
 *
 * Using the larger layout keeps a 128-byte loop descriptor from overlapping
 * the next slot.
 */
inline constexpr std::size_t descriptor_slot_size = sizeof(loop_descriptor);
inline constexpr std::size_t descriptor_slot_alignment = alignof(loop_descriptor);

/** @brief Storage for descriptors, their names, and duplicate lookup. */
struct descriptor_registry {
    std::array<std::atomic<std::byte*>, maximum_descriptor_blocks> blocks{};
    std::atomic<std::uint32_t> next_id{0};
    std::mutex dedup_mutex;
    std::vector<region_descriptor*> dedup_table;
    std::deque<std::string> owned_strings;

    /** @brief Reserve room for the first 256 descriptor entries. */
    descriptor_registry() {
        dedup_table.reserve(256);
    }
};

[[nodiscard]] inline descriptor_registry& registry() noexcept {
    static descriptor_registry instance;
    return instance;
}

/// Compute the 32-bit FNV-1a hash used to find matching names.
[[nodiscard, gnu::always_inline]]
inline std::uint32_t fnv1a_32(std::string_view value) noexcept {
    std::uint32_t hash = 2166136261u;
    for (char character : value) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= 16777619u;
    }
    return hash;
}

/// Allocate a 32 KiB block containing 256 descriptor slots.
/// Blocks remain allocated for the process lifetime.
[[nodiscard, gnu::cold]]
inline std::byte* allocate_block() noexcept {
    return static_cast<std::byte*>(::operator new(
        descriptor_slot_size * descriptor_block_size,
        std::align_val_t{descriptor_slot_alignment}));
}

/**
 * @brief Find a matching descriptor or register a new one.
 * @param[in] name Metric name copied into registry storage.
 * @param[in] loc Source location to report. Its line participates in matching.
 * @param[in] kind Metric type and descriptor layout.
 * @param[in] target_name SIMD target name, or empty for scalar code.
 * @param[in] lanes SIMD lane count, or zero for scalar code.
 * @return Stable descriptor, or the first descriptor if capacity is exhausted.
 */
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

    // Search registered descriptors for the same name, line, kind, and SIMD settings.
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

    // Store one-based IDs so zero can mark an invalid measurement.
    const auto raw_id = descriptor_registry.next_id.fetch_add(1, std::memory_order_relaxed);
    const auto block_idx = raw_id / descriptor_block_size;
    const auto slot_idx  = raw_id % descriptor_block_size;
    if (block_idx >= maximum_descriptor_blocks) {
        // Capacity is exhausted. Reuse the first descriptor, merging later
        // measurements into its identity.
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

export namespace bioseq::instrumentation {

/**
 * @brief Find or register a named region at this source line.
 * @param[in] name Metric name copied into registry storage.
 * @param[in] loc Source location to report. Its line participates in matching.
 * @return Stable region descriptor, or the first descriptor when storage is full.
 */
[[nodiscard, gnu::cold]]
inline const region_descriptor*
register_static_region(std::string_view name,
                       std::source_location loc = std::source_location::current()) noexcept {
    return detail::register_impl(name, loc, metric_kind::region, {}, 0);
}

/**
 * @brief Find or register a named counter at this source line.
 * @param[in] name Metric name copied into registry storage.
 * @param[in] loc Source location to report. Its line participates in matching.
 * @return Stable counter descriptor, or the first descriptor when storage is full.
 */
[[nodiscard, gnu::cold]]
inline const region_descriptor*
register_static_counter(std::string_view name,
                        std::source_location loc = std::source_location::current()) noexcept {
    return detail::register_impl(name, loc, metric_kind::counter, {}, 0);
}

/**
 * @brief Find or register a named gauge at this source line.
 * @param[in] name Metric name copied into registry storage.
 * @param[in] loc Source location to report. Its line participates in matching.
 * @return Stable gauge descriptor, or the first descriptor when storage is full.
 */
[[nodiscard, gnu::cold]]
inline const region_descriptor*
register_static_gauge(std::string_view name,
                      std::source_location loc = std::source_location::current()) noexcept {
    return detail::register_impl(name, loc, metric_kind::gauge, {}, 0);
}

/**
 * @brief Find or register a scalar loop descriptor.
 * @param[in] name Metric name copied into registry storage.
 * @param[in] loc Source location to report. Its line participates in matching.
 * @return Loop descriptor. Registration capacity must remain available.
 */
[[nodiscard, gnu::cold]]
inline const loop_descriptor*
register_static_loop(std::string_view name,
                     std::source_location loc = std::source_location::current()) noexcept {
    auto* descriptor = detail::register_impl(name, loc, metric_kind::loop, {}, 0);
    return static_cast<const loop_descriptor*>(descriptor);
}

/**
 * @brief Find or register a loop for a SIMD target and lane count.
 * @param[in] base_name Metric name copied into registry storage.
 * @param[in] target_name SIMD target name copied into registry storage.
 * @param[in] lanes SIMD lane count.
 * @param[in] loc Source location to report. Its line participates in matching.
 * @return Loop descriptor. Registration capacity must remain available.
 */
[[nodiscard, gnu::cold]]
inline const loop_descriptor*
register_static_simd_loop(std::string_view base_name,
                          std::string_view target_name,
                          std::uint16_t lanes,
                          std::source_location loc = std::source_location::current()) noexcept {
    auto* descriptor = detail::register_impl(base_name, loc, metric_kind::loop, target_name, lanes);
    return static_cast<const loop_descriptor*>(descriptor);
}

/**
 * @brief Look up the slot for a previously registered descriptor ID.
 * @param[in] id One-based registered ID. Zero is invalid.
 * @return Descriptor pointer, or null for zero, an out-of-range block, or an
 * unallocated block. The caller must supply an ID that was registered.
 */
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

/** @brief Return the number of descriptor identifiers issued so far. */
[[nodiscard]]
inline std::uint32_t registered_descriptor_count() noexcept {
    return detail::registry().next_id.load(std::memory_order_relaxed);
}

} // namespace bioseq::instrumentation
