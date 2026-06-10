/**
 * @file phase_b_abi.cppm
 * @brief DOC0307 Phase B — primitive ABI structs shared by the Vulkan runtime,
 *        minimizer-sketch, GPU-index, and seeding modules.
 *
 * This module deliberately imports NOTHING from `bioseq_gpu.vulkan.runtime` or
 * the minimizer/index/seeding modules, so it can sit at the bottom of the Phase
 * B dependency graph. It owns the host-side wire records (`CandidateWire`,
 * `SortRangeWire`, `VulkanSketchParams`, `VulkanRankLut`) whose byte layout must
 * stay identical to the GLSL structs the shaders declare, plus the small enums
 * that select the shader specialisation key (alphabet kind / minimizer word
 * width). Every layout is pinned with `static_assert` against the field offsets
 * the SPIR-V reflection asserts at pipeline-registration time (DOC0307 §4.3).
 */
module;

#include <cstddef>
#include <cstdint>
#include <vector>

export module bioseq_gpu.vulkan.phase_b_abi;

export namespace bioseq::gpu::vulkan {

/// Which lowest-level shader specialisation reads the packed sequence. Only the
/// shader body specialises on this; every C++ function above the shader call
/// stays alphabet-generic (DOC0307 §0, §9).
enum class AlphabetKernelKind : std::uint8_t {
    dna4_nt4 = 0,
    dna5_nt4 = 1,
    generic = 2,
};

/// Minimizer storage width selector. Mirrors `MinimizerRepr<L,K>::storage_t`
/// (16/32/64-bit single integer) plus the 128-bit `u64x2` fallback.
enum class MinimizerWordKind : std::uint8_t {
    u16 = 0,
    u32 = 1,
    u64 = 2,
    u64x2 = 3,
};

/// Rank → nt4 code lookup uploaded to the candidate shader. `rank_to_code`
/// maps an alphabet rank to its forward nt4 code (>=4 == ambiguity/reset);
/// `rank_to_rc_code` is the reverse-complement code (DOC0307 §6.1).
struct VulkanRankLut {
    std::uint32_t rank_to_code[256]{};
    std::uint32_t rank_to_rc_code[256]{};
};
static_assert(sizeof(VulkanRankLut) == 2048);

/// Push-constant/uniform record carrying the runtime sketch parameters. The
/// candidate and emit shaders read distinct subsets of this block; the C++
/// side fills it once from `make_sequence_upload_view` + `minimizer_wire_traits`.
struct VulkanSketchParams {
    std::uint32_t bp_size = 0;
    std::uint32_t window_size = 10;
    std::uint32_t kmer_size = 15;
    std::uint32_t hash_bits = 0;
    std::uint32_t hash_shift = 0;
    std::uint32_t pos_bits = 0;
    std::uint32_t minimizer_word_kind = 0;
    std::uint32_t minimizer_words32 = 0;
    std::uint32_t alphabet_kind = 0;
    std::uint32_t unit_bits = 0;
    std::uint32_t bits_per_char = 0;
    std::uint32_t letters_per_unit = 0;
    std::uint32_t alphabet_size = 0;
    std::uint32_t seq_word_offset = 0;
    std::uint32_t char_mask = 0;
    std::uint32_t padding_letters = 0;
};
static_assert(sizeof(VulkanSketchParams) == 64);

/// One candidate k-mer ending at a base — the GLSL `Candidate` struct in the
/// `minimizer_candidates` shader is byte-identical to this. Runtime shader
/// registration asserts the SSBO array stride == `candidate_wire_stride`.
struct CandidateWire {
    std::uint64_t hash = 0;
    std::uint32_t pos = 0;
    std::uint32_t strand = 0;
    std::uint32_t valid = 0;
    std::uint32_t base_valid = 0;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
};
static_assert(sizeof(CandidateWire) == 32);
static_assert(offsetof(CandidateWire, hash) == 0);
static_assert(offsetof(CandidateWire, pos) == 8);
static_assert(offsetof(CandidateWire, strand) == 12);
static_assert(offsetof(CandidateWire, valid) == 16);
static_assert(offsetof(CandidateWire, base_valid) == 20);
inline constexpr std::uint32_t candidate_wire_stride =
    static_cast<std::uint32_t>(sizeof(CandidateWire));

/// One sortable anchor segment `(begin, length)` for the bitonic anchor sort.
struct SortRangeWire {
    std::uint32_t begin = 0;
    std::uint32_t length = 0;
};
static_assert(sizeof(SortRangeWire) == 8);
static_assert(offsetof(SortRangeWire, begin) == 0);
static_assert(offsetof(SortRangeWire, length) == 4);

/// Result of a seed-lookup roundtrip: the downloaded `Anchor` bytes plus the
/// real anchor count. The caller memcpys `count * sizeof(Anchor)` into the
/// pipeline `ArenaSlab<Anchor>`.
struct SeedLookupResult {
    std::vector<std::byte> host_bytes;
    std::uint64_t count = 0;
};

} // namespace bioseq::gpu::vulkan
