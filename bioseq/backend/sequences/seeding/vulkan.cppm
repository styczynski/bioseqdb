/**
 * @file vulkan.cppm
 * @brief Stable facade for Vulkan seed-hit lookup.
 *
 * @details Re-exports diagnostics, prefix-count authorities, exact memory
 * reservations, and selected-route lookup entry points for device seed
 * intersection queries.
 */
module;

export module bioseq.seeding.vulkan;

/** Seed-lookup ABI checks, decline diagnostics, and checked bounds. */
export import :diagnostics;
/** Mapped and host prefix-count authorities validated for seed lookup. */
export import :prefix_authority;
/** Host, device, and retained-output seed reservation plans. */
export import :reservation;
/** Selected-route Vulkan seed lookup and completion ownership. */
export import :lookup;
