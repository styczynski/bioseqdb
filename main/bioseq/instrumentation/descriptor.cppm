/**
 * @file instrumentation/descriptor.cppm
 * @brief RegionDescriptor / LoopDescriptor + register_*_static helpers.
 *
 * A descriptor is registered once per (name, file, line) tuple via the
 * static-local idiom at the call site:
 *   static const auto* desc = register_region_static("ksw2.extend");
 *
 * Registration is cold-path: the first call acquires a mutex, deduplicates
 * against the global hash table, allocates a fresh descriptor block from
 * the global pool when needed, returns a stable pointer. Subsequent calls
 * return the cached pointer (one load + branch).
 */
module;

#include <atomic>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <source_location>
#include <string_view>
#include <vector>

export module bioseq.instr.descriptor;

import bioseq.instr.config;

export namespace bioseq::instr {

enum class Kind : std::uint8_t { region, loop, counter, gauge, sample };

/// A descriptor is the immutable metadata for one named profiling site.
/// 64 bytes; cache-line aligned. The fields are laid out so that the
/// hottest reads (id, name, kind, flags) are in the first 32 bytes.
struct alignas(64) RegionDescriptor {
    std::uint32_t  id;              // assigned at registration; 0 = invalid
    std::uint32_t  name_hash;       // FNV-1a (fast equality during dedup)
    const char*    name;            // points into .rodata or descriptor pool
    const char*    file;
    std::uint32_t  line;
    std::uint16_t  color;           // unused; reserved for visualisation
    std::uint8_t   kind;            // Kind enum
    std::uint8_t   flags;
    std::uint16_t  highway_lanes;   // 0 if not a SIMD region
    std::uint8_t   _pad0[6];
    const char*    highway_target;  // nullptr if not a SIMD region
    const char*    arena_name;      // nullptr if not arena-attributed
    std::uint8_t   _pad1[8];
};
static_assert(sizeof(RegionDescriptor) == 64);
static_assert(alignof(RegionDescriptor) == 64);

/// Loop descriptor extends RegionDescriptor with iteration-related metadata.
struct alignas(64) LoopDescriptor : RegionDescriptor {
    std::uint32_t  default_subsample_rate;
    std::uint16_t  work_unit_kind;
    std::uint8_t   _pad2[58];   // pad to 128-byte cache line pair
};
static_assert(sizeof(LoopDescriptor) == 128);

/// Cold-path one-time registration. The static-local idiom at the call
/// site makes the first call run this; subsequent calls return the cached
/// pointer.
[[nodiscard, gnu::cold]]
const RegionDescriptor* register_region_static(
    std::string_view name,
    std::source_location loc = std::source_location::current()) noexcept;

[[nodiscard, gnu::cold]]
const LoopDescriptor* register_loop_static(
    std::string_view name,
    std::source_location loc = std::source_location::current()) noexcept;

[[nodiscard, gnu::cold]]
const LoopDescriptor* register_simd_loop_static(
    std::string_view base_name,
    std::string_view target_name,
    std::uint16_t lanes,
    std::source_location loc = std::source_location::current()) noexcept;

/// Lookup by id. Used by the summary writer to walk every registered
/// descriptor.
[[nodiscard]] const RegionDescriptor* descriptor_lookup(std::uint32_t id) noexcept;

/// Number of descriptors registered so far.
[[nodiscard]] std::uint32_t descriptor_count() noexcept;

}  // namespace bioseq::instr

// =====================================================================
// Implementation. Lives in the same module unit so we don't need a
// separate .cpp; bioseq's build is one big translation across .cppm
// files.
// =====================================================================

namespace bioseq::instr::detail {

constexpr std::uint32_t kBlockSize = 256;
constexpr std::uint32_t kMaxBlocks = 4096;

struct DescriptorRegistry {
    std::array<std::atomic<RegionDescriptor*>, kMaxBlocks> blocks{};
    std::atomic<std::uint32_t>                             next_id{0};
    std::mutex                                             dedup_mutex;
    std::vector<RegionDescriptor*>                         dedup_table;

    DescriptorRegistry() {
        dedup_table.reserve(256);
    }
};

[[nodiscard]] inline DescriptorRegistry& registry() noexcept {
    static DescriptorRegistry r;
    return r;
}

/// FNV-1a 32-bit hash. Used for name dedup and as a name_hash field on
/// the descriptor.
[[nodiscard, gnu::always_inline]]
inline std::uint32_t fnv1a_32(std::string_view s) noexcept {
    std::uint32_t h = 2166136261u;
    for (char c : s) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 16777619u;
    }
    return h;
}

/// Cold-path block allocator. Allocates a fresh 256-descriptor block
/// (16 KiB) when the current block runs out. Allocation uses operator new
/// — the descriptor pool is process-wide and never freed.
[[nodiscard, gnu::cold]]
inline RegionDescriptor* allocate_block() noexcept {
    auto* block = static_cast<RegionDescriptor*>(
        ::operator new(sizeof(RegionDescriptor) * kBlockSize,
                       std::align_val_t{alignof(RegionDescriptor)}));
    for (std::uint32_t i = 0; i < kBlockSize; ++i) {
        new (block + i) RegionDescriptor{};
    }
    return block;
}

[[nodiscard, gnu::cold]]
inline RegionDescriptor* register_impl(
    std::string_view name,
    std::source_location loc,
    Kind kind,
    std::string_view target_name,
    std::uint16_t lanes) noexcept
{
    auto& reg = registry();
    std::lock_guard<std::mutex> lock(reg.dedup_mutex);

    const auto hash = fnv1a_32(name);

    // Linear search the dedup table. Typical sizes are ~50 — small enough.
    for (auto* d : reg.dedup_table) {
        if (d->name_hash == hash
            && d->line == loc.line()
            && std::string_view{d->name} == name
            && d->highway_lanes == lanes
            && (target_name.empty()
                ? (d->highway_target == nullptr)
                : (d->highway_target != nullptr
                   && std::string_view{d->highway_target} == target_name)))
        {
            return d;
        }
    }

    // raw_id is the 0-based slot index across the descriptor table
    // (next_id starts at 0). The stored desc->id is shifted +1 so that
    // 0 stays reserved as the "invalid" sentinel for callers that pass
    // a default-constructed Region.desc.
    const auto raw_id    = reg.next_id.fetch_add(1, std::memory_order_relaxed);
    const auto block_idx = raw_id / kBlockSize;
    const auto slot_idx  = raw_id % kBlockSize;
    if (block_idx >= kMaxBlocks) {
        // Out of descriptor space. Return a sentinel (the first descriptor)
        // so the caller's static-local stays non-null but loses uniqueness.
        // Practically unreachable.
        return reg.blocks[0].load(std::memory_order_acquire) + 0;
    }

    auto* block = reg.blocks[block_idx].load(std::memory_order_acquire);
    if (block == nullptr) {
        block = allocate_block();
        reg.blocks[block_idx].store(block, std::memory_order_release);
    }
    auto* desc = block + slot_idx;

    desc->id            = raw_id + 1;
    desc->name_hash     = hash;
    desc->name          = name.data();        // borrow the literal pointer
    desc->file          = loc.file_name();
    desc->line          = loc.line();
    desc->color         = 0;
    desc->kind          = static_cast<std::uint8_t>(kind);
    desc->flags         = 0;
    desc->highway_lanes = lanes;
    desc->highway_target = target_name.empty() ? nullptr : target_name.data();
    desc->arena_name    = nullptr;

    reg.dedup_table.push_back(desc);
    return desc;
}

}  // namespace bioseq::instr::detail

namespace bioseq::instr {

[[nodiscard, gnu::cold]]
inline const RegionDescriptor* register_region_static(
    std::string_view name,
    std::source_location loc) noexcept
{
    return detail::register_impl(name, loc, Kind::region, {}, 0);
}

[[nodiscard, gnu::cold]]
inline const LoopDescriptor* register_loop_static(
    std::string_view name,
    std::source_location loc) noexcept
{
    auto* d = detail::register_impl(name, loc, Kind::loop, {}, 0);
    return static_cast<const LoopDescriptor*>(d);
}

[[nodiscard, gnu::cold]]
inline const LoopDescriptor* register_simd_loop_static(
    std::string_view base_name,
    std::string_view target_name,
    std::uint16_t lanes,
    std::source_location loc) noexcept
{
    auto* d = detail::register_impl(base_name, loc, Kind::loop, target_name, lanes);
    return static_cast<const LoopDescriptor*>(d);
}

[[nodiscard]]
inline const RegionDescriptor* descriptor_lookup(std::uint32_t id) noexcept {
    if (id == 0) return nullptr;
    const auto raw_id = id - 1;
    const auto block_idx = raw_id / detail::kBlockSize;
    const auto slot_idx  = raw_id % detail::kBlockSize;
    if (block_idx >= detail::kMaxBlocks) return nullptr;
    auto* block = detail::registry().blocks[block_idx].load(std::memory_order_acquire);
    if (block == nullptr) return nullptr;
    return block + slot_idx;
}

[[nodiscard]]
inline std::uint32_t descriptor_count() noexcept {
    return detail::registry().next_id.load(std::memory_order_relaxed);
}

}  // namespace bioseq::instr
