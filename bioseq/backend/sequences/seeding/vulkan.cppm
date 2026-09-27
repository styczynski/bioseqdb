/**
 * @file vulkan.cppm
 * @brief Vulkan target lookup for matching query minimizers.
 *
 * Exports lookup entry points, prefix counting, memory planning, and failure
 * diagnostics. Returned GPU seeds are checked and converted to chaining anchors.
 */
module;

export module bioseq.seeding.vulkan;

/** Seed record layout checks, rejection details, and count limits. */
export import :diagnostics;
/** Prefix counts and offsets from mapped or host target indexes. */
export import :prefix_authority;
/** Device, host, and output-buffer memory plans. */
export import :reservation;
/** GPU lookup, result checks, and output ownership. */
export import :lookup;
