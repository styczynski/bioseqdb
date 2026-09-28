/**
 * @file
 * @brief Function  table for one Highway CPU target, using shared C++ types.
 */
module;

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

export module bioseq.highway.kernel_table_contract;

export import bioseq.pipeline.runtime_contract;
export import bioseq.alignment.contracts;
export import bioseq.sequences.kmers.highway_minimizer_contract;
export import bioseq.prefilter.sneaky_snake;
export import bioseq.chaining.simd;

export namespace bioseq::simd {

/** @brief Stores an alignment table pointer together with its alphabet code. */
struct alignment_table_entry final {
    /** Scheduler-owned packed alphabet code. */
    std::uint8_t alphabet_code = 0;
    /** Whether all functions were set when the typed table was built. */
    bool typed_table_complete = false;
    /** Pointer to alignment_kernels for alphabet_code. */
    const void* typed_table = nullptr;
};

static_assert(std::is_standard_layout_v<alignment_table_entry>);
static_assert(std::is_trivially_copyable_v<alignment_table_entry>);

} // namespace bioseq::simd

export namespace bioseq::simd_dispatch {

/** @brief Function signature for a SneakySnake batch on one CPU target. */
using sneaky_snake_simd_batch_kernel =
    std::vector<::bioseq::prefilter::prefilter_result> (*)(
        const ::bioseq::prefilter::prefilter_batch_input&,
        ::bioseq::prefilter::sneaky_snake_options);

/** @brief Functions shared by several pipeline schedulers on one CPU target. */
struct highway_common_kernels final {
    /** Fixed-target SneakySnake batch implementation. */
    sneaky_snake_simd_batch_kernel sneaky_snake_simd_batch = nullptr;
};

static_assert(std::is_standard_layout_v<highway_common_kernels>);
static_assert(std::is_trivially_copyable_v<highway_common_kernels>);

/** @brief Chaining functions for one CPU target. */
struct highway_chaining_kernels final {
    /** Fixed-target SIMD anchor-chaining implementation. */
    ::bioseq::chaining::chain_anchors_simd_kernel
        chain_anchors_simd = nullptr;
};

/**
 * @brief Placeholder for indexing functions in generated tables.
 *
 * Index construction calls the minimizer functions directly, so this
 * record is empty.
 */
struct highway_indexing_kernels final {};

static_assert(std::is_standard_layout_v<highway_chaining_kernels>);
static_assert(std::is_trivially_copyable_v<highway_chaining_kernels>);
static_assert(std::is_standard_layout_v<highway_indexing_kernels>);
static_assert(std::is_trivially_copyable_v<highway_indexing_kernels>);

/**
 * @brief Identifies an alphabet type by an address shared across modules.
 *
 * Both  target-specific code and its callers use the same inline variable
 * for a given alphabet. This avoids maintaining a second type-to-code map.
 */
using highway_alphabet_identity = const void*;

namespace highway_alphabet_identity_detail {

/**
 * @brief One inline variable per alphabet, identified by its addiess
 * @tparam p_alphabet Alphabet type to identify.
 */
template <class p_alphabet>
inline std::byte type_identity_anchor{};

} // namespace highway_alphabet_identity_detail

/**
 * @brief Returns the shared address that identifies an alphabet type.
 * @tparam p_alphabet Alphabet type to identify.
 */
template <class p_alphabet>
[[nodiscard]] constexpr auto highway_alphabet_identity_for() noexcept -> highway_alphabet_identity {
    return &highway_alphabet_identity_detail::type_identity_anchor<
        p_alphabet>;
}

/** @brief Pairs an alphabet's C++ type identity with its scheduler code. */
struct highway_alphabet_identity_entry final {
    /** Address identifying the alphabet type. */
    highway_alphabet_identity type_identity = nullptr;
    /** Scheduler-owned packed alphabet code. */
    std::uint8_t alphabet_code =
        ::bioseq::minimizers::k_unbound_packed_alphabet_code;
};

static_assert(std::is_standard_layout_v<highway_alphabet_identity_entry>);
static_assert(std::is_trivially_copyable_v<highway_alphabet_identity_entry>);

/**
 * @brief Returns the fixed name used for a kernel family in generated records.
 * @return Kernel name, or "unknown" for an invalid value or the count sentinel.
 */
[[nodiscard]] constexpr auto highway_kernel_name(const highway_kernel kernel) noexcept
    -> std::string_view {
    switch (kernel) {
        case highway_kernel::minimizer_minimap2_reference:
            return "minimizer::minimap2_reference";
        case highway_kernel::minimizer_two_stack_nthash:
            return "minimizer::two_stack_nthash";
        case highway_kernel::minimizer_bounded_nthash:
            return "minimizer::bounded_nthash";
        case highway_kernel::chaining_chain_anchors_simd:
            return "chaining::chain_anchors_simd";
        case highway_kernel::prefilter_sneaky_snake_simd_batch:
            return "prefilter::sneaky_snake_simd_batch";
        case highway_kernel::alignment_ksw2:
            return "alignment::ksw2";
        case highway_kernel::alignment_ksw2_batched:
            return "alignment::ksw2_batched";
        case highway_kernel::alignment_block_aligner:
            return "alignment::block_aligner";
        case highway_kernel::count:
            break;
    }
    return "unknown";
}

/**
 * @brief Returns a mask,  with the selected kernel family's bit set.
 *
 * Invalid values and the count sentinel produce an empty mask.
 */
[[nodiscard]] constexpr auto highway_kernel_mask_for(const highway_kernel kernel) noexcept
    -> highway_kernel_mask {
    highway_kernel_mask result{};
    const auto index = static_cast<std::size_t>(kernel);
    if (index < result.size()) {
        result.set(index);
    }
    return result;
}

/**
 * @brief Functions and alphabet tables for the selected Highway CPU target.
 *
 * Function signatures use shared types. The completeness check verifies
 * that required functions are present and each alphabet has one matching
 * alignment table.
 */
struct highway_kernel_table final {
    /** Kernel families supplied by this target. */
    highway_kernel_mask available_kernels{};
    /** Alphabet identities ordered by scheduler alphabet code. */
    std::span<const highway_alphabet_identity_entry> alphabet_identities{};
    /** Fixed-target minimizer implementations. */
    ::bioseq::minimizers::minimizer_kernels minimizers{};
    /** Functions used by several pipeline schedulers. */
    highway_common_kernels common{};
    /** Fixed-target chaining functions. */
    highway_chaining_kernels chaining{};
    /** Placeholder for indexing, which uses the minimizer table directly. */
    highway_indexing_kernels indexing{};
    /** Alignment tables in the same order as alphabet_identities. */
    std::span<const ::bioseq::simd::alignment_table_entry>
        alignment_by_alphabet{};

    /**
     * @brief Checks the availability bit  for one kernel family.
     */
    [[nodiscard]] auto has_kernel(const highway_kernel kernel) const noexcept -> bool {
        const auto index = static_cast<std::size_t>(kernel);
        return index < available_kernels.size() && available_kernels[index];
    }

    /**
     * @brief Checks that every requested kernel bit is available.
     */
    [[nodiscard]] auto has_all_kernels(const highway_kernel_mask& required) const noexcept -> bool {
        return (available_kernels & required) == required;
    }

    /**
     * @brief Finds an alphabet by its scheduler code.
     * @return Matching pntry, or nullptr if missing or duplicated.
     */
    [[nodiscard]] auto resolve_alphabet_identity(const std::uint8_t alphabet_code) const noexcept
        -> const highway_alphabet_identity_entry* {
        const highway_alphabet_identity_entry* resolved = nullptr;
        for (const highway_alphabet_identity_entry& entry
             : alphabet_identities) {
            if (entry.alphabet_code != alphabet_code) continue;
            if (resolved != nullptr) return nullptr;
            resolved = &entry;
        }
        return resolved;
    }

    /**
     * @brief Finds an alphabet by its C++ type.
     * @return Matching entry, or nullptr if missing or duplicated.
     */
    template <class p_alphabet>
    [[nodiscard]] auto resolve_alphabet_identity() const noexcept
        -> const highway_alphabet_identity_entry* {
        const auto expected = highway_alphabet_identity_for<p_alphabet>();
        const highway_alphabet_identity_entry* resolved = nullptr;
        for (const highway_alphabet_identity_entry& entry
             : alphabet_identities) {
            if (entry.type_identity != expected) continue;
            if (resolved != nullptr) return nullptr;
            resolved = &entry;
        }
        return resolved;
    }

    /**
     * @brief Checks required functions and alphabet-to-alignment table mappings.
     *
     * Every family must be available Alphabet codes must increase strictly,
     * type identities must be unique, and each typed alignment table must
     * be present and complete.
     */
    // Called both by fixed-target binders and neutral dispatch. Do not allow
    // an out-of-line validation body to inherit another caller's ISA.
    [[nodiscard, gnu::always_inline]] auto is_complete() const noexcept -> bool {
        if (!available_kernels.all()
            || minimizers.minimap2_reference == nullptr
            || minimizers.two_stack_nthash == nullptr
            || minimizers.bounded_nthash == nullptr
            || common.sneaky_snake_simd_batch == nullptr
            || chaining.chain_anchors_simd == nullptr
            || alphabet_identities.empty()
            || alphabet_identities.size() != alignment_by_alphabet.size()) {
            return false;
        }
        for (std::size_t index = 0; index < alphabet_identities.size();
             ++index) {
            const auto& identity = alphabet_identities[index];
            const auto& alignment = alignment_by_alphabet[index];
            if (identity.type_identity == nullptr
                || !alignment.typed_table_complete
                || alignment.typed_table == nullptr
                || alignment.alphabet_code != identity.alphabet_code
                || (index != 0u
                    && alphabet_identities[index - 1u].alphabet_code
                        >= identity.alphabet_code)) {
                return false;
            }
            for (std::size_t previous = 0u; previous < index; ++previous) {
                if (alphabet_identities[previous].type_identity
                    == identity.type_identity) {
                    return false;
                }
            }
        }
        return true;
    }
};

/** @brief Connects one compiled CPU target to its  name and function table. */
struct highway_target_binding final {
    /** Exact single-bit Highway target identity. */
    std::int64_t target = 0;
    /** Highway-owned display name for target. */
    const char* target_name = nullptr;
    /** Complete kernel table for target. */
    const highway_kernel_table* kernels = nullptr;
};

static_assert(std::is_standard_layout_v<highway_target_binding>);
static_assert(std::is_trivially_copyable_v<highway_target_binding>);


} // namespace bioseq::simd_dispatch
