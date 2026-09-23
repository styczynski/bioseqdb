/**
 * @file
 * @brief Target-neutral callable table for one fixed Highway target.
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

/** @brief Type-erased entry for one alphabet-specific alignment table. */
struct alignment_table_entry final {
    /** Scheduler-owned packed alphabet code. */
    std::uint8_t alphabet_code = 0;
    /** Completeness result computed while the table retains its static type. */
    bool typed_table_complete = false;
    /** Pointer to alignment_kernels for alphabet_code. */
    const void* typed_table = nullptr;
};

static_assert(std::is_standard_layout_v<alignment_table_entry>);
static_assert(std::is_trivially_copyable_v<alignment_table_entry>);

} // namespace bioseq::simd

export namespace bioseq::simd_dispatch {

/** @brief Neutral ABI for one fixed-target SneakySnake batch implementation. */
using sneaky_snake_simd_batch_kernel =
    std::vector<::bioseq::prefilter::prefilter_result> (*)(
        const ::bioseq::prefilter::prefilter_batch_input&,
        ::bioseq::prefilter::sneaky_snake_options);

/** @brief Fixed-target functions shared by multiple scheduler families. */
struct highway_common_kernels final {
    /** Fixed-target SneakySnake batch implementation. */
    sneaky_snake_simd_batch_kernel sneaky_snake_simd_batch = nullptr;
};

static_assert(std::is_standard_layout_v<highway_common_kernels>);
static_assert(std::is_trivially_copyable_v<highway_common_kernels>);

/** @brief Fixed-target dynamic-programming chaining functions. */
struct highway_chaining_kernels final {
    /** Fixed-target SIMD anchor-chaining implementation. */
    ::bioseq::chaining::chain_anchors_simd_kernel
        chain_anchors_simd = nullptr;
};

/**
 * @brief Empty indexing-family record in the generated Highway source record.
 *
 * Index construction consumes minimizer_kernels directly, so this family has
 * no independent callable ABI.
 */
struct highway_indexing_kernels final {};

static_assert(std::is_standard_layout_v<highway_chaining_kernels>);
static_assert(std::is_trivially_copyable_v<highway_chaining_kernels>);
static_assert(std::is_standard_layout_v<highway_indexing_kernels>);
static_assert(std::is_trivially_copyable_v<highway_indexing_kernels>);

/**
 * @brief ODR-stable neutral identity for one concrete alphabet type.
 *
 * Fixed-target binders and neutral callers take the address of the same inline
 * variable specialization. The address is therefore stable across module
 * boundaries without a duplicate type-to-code mapping.
 */
using highway_alphabet_identity = const void*;

namespace highway_alphabet_identity_detail {

/**
 * @brief Address anchor providing one identity per concrete alphabet type.
 * @tparam p_alphabet Concrete alphabet represented by this specialization.
 */
template <class p_alphabet>
inline std::byte type_identity_anchor{};

} // namespace highway_alphabet_identity_detail

/**
 * @brief Return the program-wide identity token for one alphabet type.
 * @tparam p_alphabet Concrete alphabet whose token is requested.
 * @return Address-stable neutral identity for `p_alphabet`.
 */
template <class p_alphabet>
[[nodiscard]] constexpr auto highway_alphabet_identity_for() noexcept -> highway_alphabet_identity {
    return &highway_alphabet_identity_detail::type_identity_anchor<
        p_alphabet>;
}

/** @brief Scheduler alphabet code paired with its concrete C++ type identity. */
struct highway_alphabet_identity_entry final {
    /** Address-stable identity of the concrete alphabet type. */
    highway_alphabet_identity type_identity = nullptr;
    /** Scheduler-owned packed alphabet code. */
    std::uint8_t alphabet_code =
        ::bioseq::minimizers::k_unbound_packed_alphabet_code;
};

static_assert(std::is_standard_layout_v<highway_alphabet_identity_entry>);
static_assert(std::is_trivially_copyable_v<highway_alphabet_identity_entry>);

/**
 * @brief Return the stable source-record key for one kernel family.
 * @param kernel Kernel family whose source-record key is requested.
 * @return Byte-stable source-record key, or `unknown` for the sentinel and
 * unsupported values.
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
 * @brief Construct the one-bit mask for a kernel family.
 * @param kernel Kernel family whose bit is requested.
 * @return One-bit mask for a runnable kernel, or an empty mask for the
 * sentinel and unsupported values.
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
 * @brief Immutable kernel table selected by the single runtime dispatch.
 *
 * The table contains only target-neutral function signatures. Availability,
 * alphabet identity, and typed alignment entries are validated together so a
 * caller cannot combine records from different fixed targets.
 */
struct highway_kernel_table final {
    /** Kernel families supplied by this target. */
    highway_kernel_mask available_kernels{};
    /** Alphabet identities ordered by scheduler alphabet code. */
    std::span<const highway_alphabet_identity_entry> alphabet_identities{};
    /** Fixed-target minimizer implementations. */
    ::bioseq::minimizers::minimizer_kernels minimizers{};
    /** Fixed-target functions shared by scheduler families. */
    highway_common_kernels common{};
    /** Fixed-target chaining functions. */
    highway_chaining_kernels chaining{};
    /** Empty indexing-family source record. */
    highway_indexing_kernels indexing{};
    /** Alignment tables in the same order as alphabet_identities. */
    std::span<const ::bioseq::simd::alignment_table_entry>
        alignment_by_alphabet{};

    /**
     * @brief Test whether this target binds one kernel family.
     * @param kernel Kernel family to inspect.
     * @return `true` when the corresponding availability bit is set.
     */
    [[nodiscard]] auto has_kernel(const highway_kernel kernel) const noexcept -> bool {
        const auto index = static_cast<std::size_t>(kernel);
        return index < available_kernels.size() && available_kernels[index];
    }

    /**
     * @brief Test whether every requested kernel family is available.
     * @param required Kernel-family mask required by the caller.
     * @return `true` when all requested bits are available.
     */
    [[nodiscard]] auto has_all_kernels(const highway_kernel_mask& required) const noexcept -> bool {
        return (available_kernels & required) == required;
    }

    /**
     * @brief Resolve an alphabet entry by scheduler code.
     * @param alphabet_code Scheduler-owned packed alphabet code.
     * @return The unique matching entry, or `nullptr` when no unique match
     * exists.
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
     * @brief Resolve the scheduler entry for a concrete alphabet type.
     * @tparam p_alphabet Concrete alphabet selected by the caller.
     * @return The unique matching entry, or `nullptr` when no unique match
     * exists.
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
     * @brief Validate the complete fixed-target dispatch table.
     * @return `true` only when every family is bound and the alphabet and
     * alignment records form a unique, strictly ordered one-to-one mapping.
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

/** @brief Neutral binding returned by one fixed-target binder. */
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
