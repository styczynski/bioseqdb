#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <tests/support/common.hpp>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>
#include <vulkan/vulkan.h>

import bioseq.gpu.vulkan.shader_cache;
import bioseq.gpu.vulkan.shader_compiler;
import bioseq.gpu.vulkan.shader_layout;
import bioseq.gpu.vulkan.shader_variants;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;
import bioseq.prefilter.types;

namespace {

namespace gv = bioseq::gpu::vulkan;
namespace pf = bioseq::prefilter;

bool shader_module_create_called = false;
bool shader_module_payload_matches = false;
std::vector<std::byte> expected_shader_module_metallib;
std::filesystem::path seal_identity_mutation_path;
bool seal_identity_mutation_succeeded = false;

/** @brief Mutates the staged seal artifact without changing its byte length. */
void mutate_seal_artifact_same_size() noexcept {
    seal_identity_mutation_succeeded = false;
    const int fd = ::open(
        seal_identity_mutation_path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;
    std::byte value{};
    if (::pread(fd, &value, 1u, 0) == 1) {
        value ^= std::byte{0x01u};
        seal_identity_mutation_succeeded =
            ::pwrite(fd, &value, 1u, 0) == 1 && ::fsync(fd) == 0;
    }
    (void)::close(fd);
}

/** @brief Records that the Vulkan shader-module callback was invoked. */
VKAPI_ATTR VkResult VKAPI_CALL record_shader_module_create(
    VkDevice,
    const VkShaderModuleCreateInfo*,
    const VkAllocationCallbacks*,
    VkShaderModule*)
{
    shader_module_create_called = true;
    return VK_SUCCESS;
}

/** @brief Validates the exact retained metallib payload passed to Vulkan. */
VKAPI_ATTR VkResult VKAPI_CALL record_exact_shader_module_create(
    VkDevice,
    const VkShaderModuleCreateInfo* create_info,
    const VkAllocationCallbacks*,
    VkShaderModule*)
{
    shader_module_create_called = true;
    const std::size_t header_bytes = gv::moltenvk_msl_spirv_header_size();
    shader_module_payload_matches = create_info != nullptr
        && create_info->pCode != nullptr
        && create_info->codeSize
            == header_bytes + expected_shader_module_metallib.size()
        && std::memcmp(
            reinterpret_cast<const std::byte*>(create_info->pCode)
                + header_bytes,
            expected_shader_module_metallib.data(),
            expected_shader_module_metallib.size()) == 0;
    return VK_SUCCESS;
}

/** @brief Counts compile requests without publishing cache artifacts. */
struct no_publish_compile_service {
    int calls = 0;

    [[nodiscard]] std::expected<void, std::string> ensure_variant_compiled(
        gv::shader_variant_key)
    {
        ++calls;
        return {};
    }
};

/** @brief Owns an isolated shader-cache test directory. */
struct temporary_directory {
    std::filesystem::path path;

    temporary_directory()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path()
             / ("bioseq_shader_cache_test_" + std::to_string(::getpid())
                + "_" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }

    ~temporary_directory()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

/** @brief Restores one process environment variable when the test scope ends. */
struct scoped_environment_variable {
    std::string name;
    std::optional<std::string> previous;

    scoped_environment_variable(std::string variable, std::string_view value)
        : name(std::move(variable))
    {
        if (const char* existing = std::getenv(name.c_str()); existing != nullptr) {
            previous = existing;
        }
        REQUIRE(::setenv(name.c_str(), std::string(value).c_str(), 1) == 0);
    }

    ~scoped_environment_variable()
    {
        if (previous) {
            (void)::setenv(name.c_str(), previous->c_str(), 1);
        } else {
            (void)::unsetenv(name.c_str());
        }
    }
};

/** @brief Resets the process-local archive-helper store around one test scope. */
struct scoped_archive_helper_store_reset {
    scoped_archive_helper_store_reset() {
        REQUIRE(gv::qualified_metal_archive_helper_process_store_instance()
                    .reset_for_test());
    }
    ~scoped_archive_helper_store_reset() {
        (void)gv::qualified_metal_archive_helper_process_store_instance()
            .reset_for_test();
    }
};

/**
 * @brief Reports whether a shader key occurs in a key vector.
 * @param[in] keys Shader keys to inspect.
 * @param[in] needle Shader key to find.
 * @return `true` when `needle` is present.
 */
[[nodiscard]] bool contains_key(
    const std::vector<gv::shader_variant_key>& keys,
    gv::shader_variant_key needle)
{
    return std::find(keys.begin(), keys.end(), needle) != keys.end();
}

/**
 * @brief Validates the canonical lowercase shader-cache digest form.
 * @param[in] digest Candidate digest text.
 * @return `true` for a sixteen-character lowercase hexadecimal digest.
 */
[[nodiscard]] bool is_hex_digest(std::string_view digest)
{
    return digest.size() == 16 && std::all_of(digest.begin(), digest.end(), [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

/**
 * @brief Copies string bytes into a byte vector.
 * @param[in] text Source bytes.
 * @return Owned byte vector.
 */
[[nodiscard]] std::vector<std::byte> bytes_of(std::string_view text)
{
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    return bytes;
}

/**
 * @brief Reads an entire fixture file as bytes.
 * @param[in] path Fixture path.
 * @return Complete file contents.
 */
[[nodiscard]] std::vector<std::byte> read_file_bytes(
    const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input);
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    REQUIRE(size >= 0);
    input.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    REQUIRE(input);
    return bytes;
}

/**
 * @brief Writes a matching qualification receipt for a synthetic archive.
 * @param[in] archive Archive path.
 * @param[in] archive_bytes Archive payload.
 * @param[in] key Shader cache key.
 * @param[in] metallib_bytes Metallib payload bound by the receipt.
 * @param[in] entry_name Metal entrypoint name.
 * @param[in] max_threads Maximum pipeline threads per threadgroup.
 */
void write_fake_archive_qualification(
    const std::filesystem::path& archive,
    std::span<const std::byte> archive_bytes,
    const gv::shader_variant_key& key,
    std::span<const std::byte> metallib_bytes,
    std::string_view entry_name,
    std::uint64_t max_threads = 256u)
{
#if defined(__APPLE__)
    const auto helper =
        gv::qualified_metal_archive_helper_process_store_instance().load_once();
    REQUIRE(helper);
    const std::string helper_source = (**helper).source;
    const std::uint64_t helper_size = (**helper).executable_size;
    const std::array<std::uint8_t, 32> helper_sha256 =
        (**helper).executable_sha256;
#else
    const auto portable_helper = bytes_of("portable-helper-fixture");
    const std::string helper_source = "installed-sibling";
    const std::uint64_t helper_size =
        static_cast<std::uint64_t>(portable_helper.size());
    const std::array<std::uint8_t, 32> helper_sha256 =
        gv::sealed_shader_cache_detail::sha256(portable_helper);
#endif
    const gv::metal_archive_qualification_receipt qualification{
        .wire_revision = gv::sealed_shader_cache_detail::
            k_archive_qualification_wire_revision,
        .shader_key = key,
        .metallib_size = static_cast<std::uint64_t>(metallib_bytes.size()),
        .metallib_sha256 =
            gv::sealed_shader_cache_detail::sha256(metallib_bytes),
        .entry_name = std::string(entry_name),
        .max_total_threads_per_threadgroup = max_threads,
        .threadgroup_size_is_multiple = false,
        .negative_error = gv::metal_archive_negative_error_fingerprint{
            .domain = "MTLLibraryErrorDomain",
            .code = 6,
        },
        .archive_size = static_cast<std::uint64_t>(archive_bytes.size()),
        .archive_sha256 =
            gv::sealed_shader_cache_detail::sha256(archive_bytes),
        .helper_wire_revision = 2u,
        .helper_build_identity = "bioseq_gpu_archive_builder",
        .helper_source = helper_source,
        .helper_executable_size = helper_size,
        .helper_executable_sha256 = helper_sha256,
        .helper_metal_registry_id = 0x1234u,
    };
    const auto qualification_bytes = gv::sealed_shader_cache_detail::
        serialize_metal_archive_qualification_receipt(qualification);
    REQUIRE(qualification_bytes);
    REQUIRE(gv::durable_write_file(
        gv::cache_binary_archive_qualification_path(archive),
        *qualification_bytes));
}

/**
 * @brief Writes a complete synthetic sealed-cache artifact set.
 * @param[in] cache_dir Destination cache directory.
 * @param[in] keys Shader variants to materialize.
 */
void write_fake_sealed_shader_artifacts(
    const std::filesystem::path& cache_dir,
    std::span<const gv::shader_variant_key> keys)
{
    constexpr std::string_view entry = "main0";
    for (const gv::shader_variant_key& key : keys) {
        const gv::shader_workgroup_size workgroup =
            gv::expected_shader_workgroup_size(key);
        gv::shader_cache_metadata meta{};
        meta.key = key;
        meta.wg_x = workgroup.x;
        meta.wg_y = workgroup.y;
        meta.wg_z = workgroup.z;
        meta.entry_len = static_cast<std::uint32_t>(entry.size());
        const auto meta_bytes = gv::meta_to_bytes(meta, entry);
        REQUIRE(gv::durable_write_file(
            gv::cache_meta_path(cache_dir, key), meta_bytes));
        const auto metallib = bytes_of(
            "test-metallib-" + gv::cache_file_stem(key));
        REQUIRE(gv::durable_write_file(
            gv::cache_metallib_path(cache_dir, key), metallib));
        const auto archives = gv::cache_binary_archive_paths(cache_dir, key);
        const std::uint64_t exact_threads =
            static_cast<std::uint64_t>(workgroup.x)
            * static_cast<std::uint64_t>(workgroup.y)
            * static_cast<std::uint64_t>(workgroup.z);
        for (std::size_t archive_ordinal = 0u;
             archive_ordinal < archives.size(); ++archive_ordinal) {
            const std::filesystem::path& archive = archives[archive_ordinal];
            const auto archive_bytes = bytes_of(
                "qualified-test-archive-" + archive.filename().string());
            REQUIRE(gv::durable_write_file(archive, archive_bytes));
            write_fake_archive_qualification(
                archive, archive_bytes, key, metallib, entry,
                archive_ordinal == 0u ? exact_threads : 256u);
        }
    }
}

/**
 * @brief Counts one SPIR-V opcode in a structurally valid module.
 * @param[in] words SPIR-V words.
 * @param[in] opcode Opcode to count.
 * @return Occurrence count, or zero for malformed input.
 */
[[nodiscard]] std::size_t spirv_opcode_count(
    std::span<const std::uint32_t> words,
    std::uint16_t opcode) noexcept
{
    constexpr std::uint32_t spirv_magic = 0x07230203u;
    if (words.size() < 5u || words.front() != spirv_magic) return 0u;
    std::size_t count = 0u;
    for (std::size_t offset = 5u; offset < words.size();) {
        const std::uint32_t instruction = words[offset];
        const std::size_t word_count = instruction >> 16u;
        if (word_count == 0u || word_count > words.size() - offset) {
            return 0u;
        }
        if (static_cast<std::uint16_t>(instruction & 0xffffu) == opcode) {
            ++count;
        }
        offset += word_count;
    }
    return count;
}

}  // namespace

TEST_CASE("shader variants enumerate the runtime AOT set",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    gv::vulkan_static_variant_inputs inputs{
        .max_workgroup_invocations = 256,
        .max_shared_memory_bytes = 32u * 1024u,
        .default_window_size = 10,
    };

    std::vector<gv::shader_variant_key> keys;
    gv::for_each_static_shader_variant(inputs, [&keys](gv::shader_variant_key key) {
        INFO("variant stem: " << gv::cache_file_stem(key));
        CHECK(gv::is_shader_variant_key_valid(key));
        keys.push_back(key);
    });

    REQUIRE(keys.size() == 43);
    auto sorted = keys;
    std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
        return gv::cache_file_stem(left) < gv::cache_file_stem(right);
    });
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());

    CHECK(gv::choose_chaining_variant(inputs) == gv::chaining_pipeline_key{32u, 32u});
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::chain_tiled_dp_u64, 32u, 32u, 0u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::seed_lookup_anchors, 384u, 128u, 256u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::ordered_sketch_tile, 384u, 64u, 10u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::coordinate_vote_reduce, 384u, 128u, 1u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::coordinate_vote_reduce_batch, 384u, 128u, 1u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::grouped_soa_seed_count, 384u, 128u, 3u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::grouped_soa_seed_emit, 384u, 128u, 3u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::align_extend_banded_affine, 384u, 1u, 512u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::align_extend_simple_cigar, 384u, 1u, 512u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::align_extend_simple_cigar, 384u, 64u, 512u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::sneaky_snake_rank_batch, 384u, 64u, 512u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::gatekeeper_rank_batch, 384u, 64u, 128u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::gatekeeper_rank_batch_l32_e2, 384u, 64u, 128u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::gatekeeper_rank_batch_l32_e4, 384u, 64u, 128u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::gatekeeper_rank_batch_l64_e2, 384u, 64u, 128u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::gatekeeper_rank_batch_l64_e4, 384u, 64u, 128u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 64u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::seq_sort_extract_key_dna4, 384u, 256u, 0u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::seq_sort_tiebreak_generic, 384u, 256u, 0u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::seq_sort_global_compare_dna4, 384u, 256u, 0u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::seq_sort_dense_scatter_dna4, 384u, 256u, 0u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::seq_sort_prefix_check_dna4, 384u, 256u, 0u)));

    CHECK_FALSE(gv::is_shader_variant_key_valid(gv::make_shader_key(
        gv::shader_kind::chain_tiled_dp_u64, 384u, 256u, 0u)));
    CHECK_FALSE(gv::is_shader_variant_key_valid(gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 128u)));
}

TEST_CASE("grouped-SoA current ABI direct seed shaders pin raw SoA and local-position ABI",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    STATIC_REQUIRE(gv::k_gpu_grouped_soa_abi_revision == 3u);
    STATIC_REQUIRE(gv::k_gpu_grouped_soa_partition_required_flags == 7u);
    STATIC_REQUIRE(sizeof(
        gv::gpu_grouped_soa_resident_partition_descriptor) == 104u);
    STATIC_REQUIRE(alignof(
        gv::gpu_grouped_soa_resident_partition_descriptor) == 8u);
    STATIC_REQUIRE(offsetof(
        gv::gpu_grouped_soa_resident_partition_descriptor, hash_byte_offset) == 16u);
    STATIC_REQUIRE(offsetof(
        gv::gpu_grouped_soa_resident_partition_descriptor, control_count) == 48u);
    STATIC_REQUIRE(offsetof(
        gv::gpu_grouped_soa_resident_partition_descriptor, low_hash) == 72u);
    STATIC_REQUIRE(offsetof(
        gv::gpu_grouped_soa_resident_partition_descriptor, segment_end_bp) == 96u);
    STATIC_REQUIRE(sizeof(gv::gpu_grouped_soa_seed_push) == 72u);
    STATIC_REQUIRE(offsetof(gv::gpu_grouped_soa_seed_push, status_index) == 68u);
    STATIC_REQUIRE(gv::encode_gpu_grouped_soa_seed_status(
        gv::gpu_grouped_soa_seed_status::contract) == 1u);
    STATIC_REQUIRE(gv::encode_gpu_grouped_soa_seed_status(
        gv::gpu_grouped_soa_seed_status::emission) == 16u);
    STATIC_REQUIRE(std::is_standard_layout_v<
        gv::gpu_grouped_soa_resident_partition_descriptor>);
    constexpr gv::gpu_grouped_soa_resident_partition_descriptor descriptor{
        .partition_id = 3u,
        .directory_ordinal = 0u,
        .position_segment = 0u,
        .flags = gv::k_gpu_grouped_soa_partition_required_flags,
        .hash_byte_offset = 0u,
        .prefix_byte_offset = 4u,
        .count_byte_offset = 12u,
        .position_byte_offset = 0u,
        .control_count = 2u,
        .position_count = 3u,
        .hash_width = 2u,
        .offset_width = 2u,
        .count_width = 2u,
        .position_width = 2u,
        .low_hash = 7u,
        .high_hash = 11u,
        .segment_base_bp = 100u,
        .segment_end_bp = 200u,
    };
    STATIC_REQUIRE(gv::is_gpu_grouped_soa_partition_descriptor_valid(
        descriptor, 16u, 6u));
    constexpr auto invalid_descriptor = [](auto value) {
        value.position_width = 1u;
        return value;
    }(descriptor);
    STATIC_REQUIRE_FALSE(gv::is_gpu_grouped_soa_partition_descriptor_valid(
        invalid_descriptor, 16u, 6u));

    CHECK(gv::descriptor_binding_count(
        gv::descriptor_layout_kind::grouped_soa_seed) == 9u);
    CHECK(gv::descriptor_layout_for(
        gv::shader_kind::grouped_soa_seed_count)
        == gv::descriptor_layout_kind::grouped_soa_seed);
    CHECK(gv::descriptor_layout_for(
        gv::shader_kind::grouped_soa_seed_emit)
        == gv::descriptor_layout_kind::grouped_soa_seed);
    CHECK(gv::push_constant_size(
        gv::shader_kind::grouped_soa_seed_count) == 72u);
    CHECK(gv::push_constant_size(
        gv::shader_kind::grouped_soa_seed_emit) == 72u);

    const std::array kinds{
        gv::shader_kind::grouped_soa_seed_count,
        gv::shader_kind::grouped_soa_seed_emit,
    };
    std::vector<gv::shader_variant_key> static_keys;
    gv::for_each_static_shader_variant(
        gv::vulkan_static_variant_inputs{},
        [&static_keys](gv::shader_variant_key key) { static_keys.push_back(key); });
    for (const gv::shader_kind kind : kinds) {
        const auto key = gv::make_shader_key(kind, 384u, 128u, 3u);
        CHECK(gv::checked_shader_kind(key.kind) == kind);
        CHECK(gv::is_shader_variant_key_valid(key));
        CHECK_FALSE(gv::is_shader_variant_key_valid(
            gv::make_shader_key(kind, 384u, 128u, 1u)));
        CHECK(std::count(static_keys.begin(), static_keys.end(), key) == 1);
        const gv::shader_compile_request request{
            .kind = kind,
            .tile_size = 384u,
            .local_size = 128u,
            .max_span = 3u,
        };
        const auto spirv = gv::compile_shader_spirv(request);
        REQUIRE_FALSE(spirv.empty());
        CHECK(spirv_opcode_count(spirv, 241u) > 0u);  // OpAtomicOr status.
        const auto layout = gv::scan_spirv_layout(spirv);
        CHECK(layout.has_binding_array_stride(0u, 1u, 104u));
        CHECK(layout.has_binding_struct_offsets(
            0u, 1u, std::array<std::uint32_t, 18>{
                0u, 4u, 8u, 12u, 16u, 24u, 32u, 40u, 48u,
                52u, 56u, 60u, 64u, 68u, 72u, 80u, 88u, 96u}));
        CHECK(layout.has_push_constant_offsets(
            std::array<std::uint32_t, 18>{
                0u, 4u, 8u, 12u, 16u, 20u, 24u, 28u, 32u,
                36u, 40u, 44u, 48u, 52u, 56u, 60u, 64u, 68u}));
    }
}

[[nodiscard]] gv::persisted_grouped_soa_coordinate_job_audit
make_grouped_soa_coordinate_hit_audit(
    const std::size_t problem_index,
    const std::uint64_t operation_identity)
{
    static gv::buffer resident_descriptors;
    static gv::buffer resident_controls;
    static gv::buffer resident_positions;
    static gv::buffer query_minimizers;
    const auto identity = [](const gv::buffer* object,
                             const std::uint64_t token,
                             const std::uint64_t bytes) {
        return gv::runtime_buffer_identity{
            .object = object,
            .owner = reinterpret_cast<const gv::runtime*>(1u),
            .buffer_handle = token,
            .memory_handle = token + 100u,
            .bytes = bytes,
            .allocation_bytes = bytes,
        };
    };
    gv::persisted_grouped_soa_coordinate_job_audit audit{};
    audit.operation_identity = operation_identity;
    audit.query_authority_identity = 200u;
    audit.resident_descriptors = identity(
        &resident_descriptors, 1u, 104u);
    audit.resident_controls = identity(&resident_controls, 2u, 64u);
    audit.resident_positions = identity(&resident_positions, 3u, 64u);
    audit.query_minimizer_buffer = identity(&query_minimizers, 4u, 64u);
    audit.device_uuid[0] = 1u;
    audit.descriptor_bytes = 104u;
    audit.control_bytes = 64u;
    audit.position_bytes = 64u;
    audit.position_count = 8u;
    audit.max_occ_cap = 500u;
    audit.partition_count = 1u;
    audit.abi_revision = gv::k_gpu_grouped_soa_abi_revision;
    audit.generation_id = 300u;
    audit.source_identity = 400u;
    audit.cold_load_identity = 500u;
    audit.target_len = 10'000u;
    audit.problem_index = problem_index;
    audit.query_index = static_cast<std::uint32_t>(problem_index);
    audit.logical_target_row = 600u + problem_index;
    audit.logical_query_ordinal = 700u + problem_index;
    audit.query_record = gv::gpu_seed_query_record_wire{
        .minimizer_offset = static_cast<std::uint32_t>(problem_index * 4u),
        .minimizer_count = 4u,
        .query_len = 120u,
        .flags = 0u,
    };
    audit.result = gv::gpu_coordinate_result_wire{
        .query_start = 1u,
        .query_end = 119u,
        .target_start = 100u + problem_index,
        .target_end = 218u + problem_index,
        .match_length = 118u,
        .block_length = 118u,
        .chain_score = 42,
        .seed_count = 7u,
        .is_reverse = 0u,
        .is_valid = 1u,
    };
    audit.configuration =
        gv::make_persisted_grouped_soa_coordinate_config_identity(
            15u,
            500,
            gv::gpu_seed_coordinate_algorithm::vulkan_tiled_dp,
            gv::chaining_pipeline_key{
                .tile_size = 384u,
                .local_size = 128u,
            },
            5'000,
            5'000,
            500,
            3'000,
            0.01F,
            0.005F,
            25,
            40,
            3,
            16u);
    audit.query_tile_count = 2u;
    audit.query_count_dispatches = 1u;
    audit.query_scan_dispatches = 3u;
    audit.query_emit_dispatches = 1u;
    audit.query_completed_roundtrips = 2u;
    audit.direct_count_dispatches = 1u;
    audit.direct_scan_dispatches = 3u;
    audit.direct_emit_dispatches = 1u;
    audit.anchor_count = 7u;
    audit.sort_operations = 1u;
    audit.range_build_dispatches = 1u;
    audit.range_count = 1u;
    audit.chain_dispatches = 1u;
    audit.chain_backtrack_dispatches = 1u;
    audit.coordinate_dispatches = 1u;
    const std::uint64_t receipt_base = 1'000u + problem_index * 10u;
    audit.count_submit_wait_identity = receipt_base + 1u;
    audit.emit_submit_wait_identity = receipt_base + 2u;
    audit.sort_range_submit_wait_identity = receipt_base + 3u;
    audit.chain_coordinate_submit_wait_identity = receipt_base + 4u;
    audit.count_status_checks = 1u;
    audit.emit_status_checks = 1u;
    audit.result_readbacks = 1u;
    return audit;
}

[[nodiscard]] gv::persisted_grouped_soa_coordinate_job_expectation
expectation_for_grouped_soa_coordinate_audit(
    const gv::persisted_grouped_soa_coordinate_job_audit& audit)
{
    return gv::persisted_grouped_soa_coordinate_job_expectation{
        .problem_index = audit.problem_index,
        .query_authority_identity = audit.query_authority_identity,
        .resident_descriptors = audit.resident_descriptors,
        .resident_controls = audit.resident_controls,
        .resident_positions = audit.resident_positions,
        .query_minimizer_buffer = audit.query_minimizer_buffer,
        .device_uuid = audit.device_uuid,
        .descriptor_bytes = audit.descriptor_bytes,
        .control_bytes = audit.control_bytes,
        .position_bytes = audit.position_bytes,
        .position_count = audit.position_count,
        .max_occ_cap = audit.max_occ_cap,
        .partition_count = audit.partition_count,
        .abi_revision = audit.abi_revision,
        .generation_id = audit.generation_id,
        .source_identity = audit.source_identity,
        .cold_load_identity = audit.cold_load_identity,
        .target_len = audit.target_len,
        .query_index = audit.query_index,
        .logical_target_row = audit.logical_target_row,
        .logical_query_ordinal = audit.logical_query_ordinal,
        .query_record = audit.query_record,
        .result = audit.result,
        .configuration = audit.configuration,
    };
}

TEST_CASE("seed jobs require strict XOR resident representation",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    gv::buffer source_slots;
    gv::buffer source_positions;
    gv::gpu_grouped_soa_resident_view grouped_soa_view{};

    const gv::gpu_seed_coordinate_job source_direct{
        .slots = &source_slots,
        .positions = &source_positions,
        .target_len = 100u,
    };
    CHECK(gv::classify_gpu_seed_index_representation(source_direct)
          == gv::gpu_seed_index_representation::source_direct_sorted_dense);

    const gv::gpu_seed_coordinate_job grouped_soa{
        .target_len = 100u,
        .grouped_soa = &grouped_soa_view,
    };
    CHECK(gv::classify_gpu_seed_index_representation(grouped_soa)
          == gv::gpu_seed_index_representation::grouped_soa);

    const gv::gpu_seed_coordinate_job ambiguous{
        .slots = &source_slots,
        .positions = &source_positions,
        .target_len = 100u,
        .grouped_soa = &grouped_soa_view,
    };
    CHECK(gv::classify_gpu_seed_index_representation(ambiguous)
          == gv::gpu_seed_index_representation::invalid);

    const gv::gpu_seed_coordinate_job grouped_soa_with_source_metadata{
        .target_len = 100u,
        .position_count = 1u,
        .grouped_soa = &grouped_soa_view,
    };
    CHECK(gv::classify_gpu_seed_index_representation(
              grouped_soa_with_source_metadata)
          == gv::gpu_seed_index_representation::invalid);

    const gv::gpu_seed_coordinate_job incomplete_source{
        .slots = &source_slots,
        .target_len = 100u,
    };
    CHECK(gv::classify_gpu_seed_index_representation(incomplete_source)
          == gv::gpu_seed_index_representation::invalid);
}

TEST_CASE("persisted grouped-SoA coordinate receipt is strict and one-shot",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    STATIC_REQUIRE(!std::is_copy_constructible_v<
                   gv::device_query_sketch_authorization>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<
                   gv::device_query_sketch_authorization>);
    STATIC_REQUIRE(!std::is_copy_constructible_v<
                   gv::persisted_grouped_soa_coordinate_operation_receipt>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<
                   gv::persisted_grouped_soa_coordinate_operation_receipt>);
    STATIC_REQUIRE(!std::is_copy_constructible_v<
                   gv::persisted_grouped_soa_coordinate_job_receipt>);

    auto hit = make_grouped_soa_coordinate_hit_audit(0u, 100u);
    REQUIRE(hit.is_internally_consistent());
    const auto as_zero_minimizer_query = [](auto audit) {
        audit.query_record.minimizer_count = 0u;
        audit.result = {};
        audit.direct_count_dispatches = 0u;
        audit.direct_scan_dispatches = 0u;
        audit.direct_emit_dispatches = 0u;
        audit.anchor_count = 0u;
        audit.sort_operations = 0u;
        audit.range_build_dispatches = 0u;
        audit.range_count = 0u;
        audit.chain_dispatches = 0u;
        audit.chain_backtrack_dispatches = 0u;
        audit.coordinate_dispatches = 0u;
        audit.count_submit_wait_identity = 0u;
        audit.emit_submit_wait_identity = 0u;
        audit.sort_range_submit_wait_identity = 0u;
        audit.chain_coordinate_submit_wait_identity = 0u;
        audit.count_status_checks = 0u;
        audit.emit_status_checks = 0u;
        audit.result_readbacks = 0u;
        return audit;
    };

    SECTION("every mandatory dispatch and completion is required") {
        auto missing = hit;
        missing.query_count_dispatches = 0u;
        CHECK_FALSE(missing.is_internally_consistent());
        missing = hit;
        missing.query_scan_dispatches = 2u;
        CHECK_FALSE(missing.is_internally_consistent());
        missing = hit;
        missing.query_emit_dispatches = 0u;
        CHECK_FALSE(missing.is_internally_consistent());
        missing = hit;
        missing.query_completed_roundtrips = 1u;
        CHECK_FALSE(missing.is_internally_consistent());
        missing = hit;
        missing.direct_count_dispatches = 0u;
        CHECK_FALSE(missing.is_internally_consistent());
        missing = hit;
        missing.direct_scan_dispatches = 2u;
        CHECK_FALSE(missing.is_internally_consistent());
        missing = hit;
        missing.direct_emit_dispatches = 0u;
        CHECK_FALSE(missing.is_internally_consistent());
        missing = hit;
        missing.sort_operations = 0u;
        CHECK_FALSE(missing.is_internally_consistent());
        missing = hit;
        missing.range_build_dispatches = 0u;
        CHECK_FALSE(missing.is_internally_consistent());
        missing = hit;
        missing.chain_dispatches = 0u;
        CHECK_FALSE(missing.is_internally_consistent());
        missing = hit;
        missing.chain_backtrack_dispatches = 0u;
        CHECK_FALSE(missing.is_internally_consistent());
        missing = hit;
        missing.coordinate_dispatches = 0u;
        CHECK_FALSE(missing.is_internally_consistent());
        missing = hit;
        missing.result_readbacks = 0u;
        CHECK_FALSE(missing.is_internally_consistent());
        missing = hit;
        missing.count_status_checks = 0u;
        CHECK_FALSE(missing.is_internally_consistent());
        missing = hit;
        missing.emit_status_checks = 0u;
        CHECK_FALSE(missing.is_internally_consistent());
    }

    SECTION("tamper and forbidden-route counters fail closed") {
        auto tampered = hit;
        tampered.status_or = 1u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.coordinate_vote_dispatches = 1u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.cpu_or_simd_biological_calls = 1u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.recompute_calls = 1u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.fallback_calls = 1u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.query_record.flags = 1u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.query_authority_identity = 0u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.resident_controls = {};
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.resident_controls.buffer_handle =
            hit.resident_descriptors.buffer_handle;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.resident_controls.memory_handle =
            hit.resident_descriptors.memory_handle;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.descriptor_bytes =
            hit.resident_descriptors.bytes + 1u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.partition_count += 1u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.control_bytes = hit.resident_controls.bytes + 1u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.position_bytes = hit.resident_positions.bytes + 1u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.device_uuid = {};
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.configuration.coordinate_algorithm =
            gv::gpu_seed_coordinate_algorithm::coordinate_vote;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.configuration.probe_limit = 0u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.configuration.max_occ =
            static_cast<std::int32_t>(hit.max_occ_cap + 1u);
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.chain_coordinate_submit_wait_identity =
            hit.count_submit_wait_identity;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.result.is_valid = 2u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.result.is_reverse = 9u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.result.query_end = tampered.query_record.query_len + 1u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.result.target_start = tampered.result.target_end;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.result.seed_count = -1;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.result.match_length = tampered.result.block_length + 1u;
        CHECK_FALSE(tampered.is_internally_consistent());
    }

    SECTION("data-dependent no-hit stages remain ineligible") {
        const auto zero_query = as_zero_minimizer_query(hit);
        CHECK_FALSE(zero_query.is_internally_consistent());
        auto zero_tile_query = zero_query;
        zero_tile_query.query_tile_count = 0u;
        CHECK_FALSE(zero_tile_query.is_internally_consistent());

        auto no_anchor = hit;
        no_anchor.result = {};
        no_anchor.direct_emit_dispatches = 0u;
        no_anchor.anchor_count = 0u;
        no_anchor.sort_operations = 0u;
        no_anchor.range_build_dispatches = 0u;
        no_anchor.range_count = 0u;
        no_anchor.chain_dispatches = 0u;
        no_anchor.chain_backtrack_dispatches = 0u;
        no_anchor.coordinate_dispatches = 0u;
        no_anchor.emit_submit_wait_identity = 0u;
        no_anchor.sort_range_submit_wait_identity = 0u;
        no_anchor.chain_coordinate_submit_wait_identity = 0u;
        no_anchor.emit_status_checks = 0u;
        no_anchor.result_readbacks = 0u;
        CHECK(no_anchor.is_internally_consistent());

        auto no_range = hit;
        no_range.result = {};
        no_range.range_count = 0u;
        no_range.chain_dispatches = 0u;
        no_range.chain_backtrack_dispatches = 0u;
        no_range.coordinate_dispatches = 0u;
        no_range.chain_coordinate_submit_wait_identity = 0u;
        no_range.result_readbacks = 0u;
        CHECK(no_range.is_internally_consistent());
    }

    SECTION("two logical jobs move and consume exactly once") {
        std::array audits{
            make_grouped_soa_coordinate_hit_audit(0u, 900u),
            make_grouped_soa_coordinate_hit_audit(1u, 900u),
        };
        std::array expectations{
            expectation_for_grouped_soa_coordinate_audit(audits[0]),
            expectation_for_grouped_soa_coordinate_audit(audits[1]),
        };
        CHECK(gv::persisted_grouped_soa_coordinate_operation_receipt::
              self_test_one_shot_contract(audits, expectations));

        auto unsupported_zero_audits = audits;
        unsupported_zero_audits[1] =
            as_zero_minimizer_query(unsupported_zero_audits[1]);
        std::array unsupported_zero_expectations{
            expectation_for_grouped_soa_coordinate_audit(
                unsupported_zero_audits[0]),
            expectation_for_grouped_soa_coordinate_audit(
                unsupported_zero_audits[1]),
        };
        CHECK_FALSE(gv::persisted_grouped_soa_coordinate_operation_receipt::
                    self_test_one_shot_contract(
                        unsupported_zero_audits,
                        unsupported_zero_expectations));

        const auto config_tamper_fails = [&audits, &expectations](auto mutate) {
            auto tampered = expectations;
            mutate(tampered[0].configuration);
            return !gv::persisted_grouped_soa_coordinate_operation_receipt::
                self_test_one_shot_contract(audits, tampered);
        };
        CHECK(config_tamper_fails([](auto& config) {
            config.kmer_size += 1u;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.max_occ += 1;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.coordinate_algorithm =
                gv::gpu_seed_coordinate_algorithm::coordinate_vote;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.chain_key.tile_size += 1u;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.chain_key.local_size += 1u;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.max_dist_x += 1;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.max_dist_y += 1;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.bandwidth += 1;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.max_iter += 1;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.gap_penalty_bits ^= 1u;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.skip_penalty_bits ^= 1u;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.max_chain_skip += 1;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.min_chain_score += 1;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.min_seed_count += 1;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.probe_limit += 1u;
        }));

        expectations[0].result.target_end += 1u;
        CHECK_FALSE(gv::persisted_grouped_soa_coordinate_operation_receipt::
                    self_test_one_shot_contract(audits, expectations));
    }
}

TEST_CASE("seed lookup AOT shader pins six-buffer key-emission ABI",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    const auto key = gv::make_shader_key(
        gv::shader_kind::seed_lookup_anchors, 384u, 128u, 256u);
    REQUIRE(gv::is_shader_variant_key_valid(key));
    REQUIRE(gv::descriptor_binding_count(
        gv::descriptor_layout_kind::seed_lookup) == 6u);
    REQUIRE(gv::push_constant_size(
        gv::shader_kind::seed_lookup_anchors) == 44u);

    const auto spirv = gv::compile_shader_spirv(key.to_request());
    REQUIRE_FALSE(spirv.empty());
    REQUIRE(gv::validate_spirv_layout(key, spirv));
}

TEST_CASE("score-only affine extension has one provider-contract AOT variant",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    const auto key = gv::make_shader_key(
        gv::shader_kind::align_extend_banded_affine, 384u, 1u, 512u);
    REQUIRE(gv::is_shader_variant_key_valid(key));
    REQUIRE(gv::checked_shader_kind(key.kind)
            == gv::shader_kind::align_extend_banded_affine);
    REQUIRE(gv::expected_shader_workgroup_size(key)
            == (gv::shader_workgroup_size{1u, 1u, 1u}));
    const auto spirv = gv::compile_shader_spirv(key.to_request());
    REQUIRE_FALSE(spirv.empty());
    REQUIRE(gv::validate_spirv_layout(key, spirv));
    const auto msl = gv::spirv_to_msl(spirv, key, gv::msl_version{});
    REQUIRE(msl);
    CHECK(msl->wg_x == 1u);
    CHECK(msl->wg_y == 1u);
    CHECK(msl->wg_z == 1u);
}

TEST_CASE("SneakySnake rank batch has an exact provider ABI",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    STATIC_REQUIRE(gv::k_gpu_sneaky_snake_submaze_length == 512u);
    STATIC_REQUIRE(gv::k_gpu_sneaky_snake_max_edits == 31u);
    STATIC_REQUIRE(static_cast<std::uint32_t>(pf::prefilter_status::success)
                   == 0u);
    STATIC_REQUIRE(static_cast<std::uint32_t>(
                       pf::prefilter_status::conservative_unequal) == 1u);
    STATIC_REQUIRE(static_cast<std::uint32_t>(
                       pf::prefilter_status::unsupported_conservative) == 2u);
    STATIC_REQUIRE(sizeof(pf::prefilter_job) == 20u);
    STATIC_REQUIRE(alignof(pf::prefilter_job) == 4u);
    STATIC_REQUIRE(offsetof(pf::prefilter_job, max_edits) == 16u);
    STATIC_REQUIRE(sizeof(pf::prefilter_result) == 16u);
    STATIC_REQUIRE(alignof(pf::prefilter_result) == 4u);
    STATIC_REQUIRE(offsetof(pf::prefilter_result, is_accepted) == 8u);
    STATIC_REQUIRE(sizeof(gv::gpu_prefilter_push) == 16u);
    STATIC_REQUIRE(offsetof(gv::gpu_prefilter_push, mode) == 4u);
    STATIC_REQUIRE(offsetof(gv::gpu_prefilter_push, query_rank_count) == 8u);
    STATIC_REQUIRE(offsetof(gv::gpu_prefilter_push, target_rank_count) == 12u);

    const auto key = gv::make_shader_key(
        gv::shader_kind::sneaky_snake_rank_batch, 384u, 64u, 512u);
    REQUIRE(gv::is_shader_variant_key_valid(key));
    CHECK_FALSE(gv::is_shader_variant_key_valid(gv::make_shader_key(
        gv::shader_kind::sneaky_snake_rank_batch, 384u, 32u, 512u)));
    CHECK_FALSE(gv::is_shader_variant_key_valid(gv::make_shader_key(
        gv::shader_kind::sneaky_snake_rank_batch, 384u, 64u, 511u)));
    REQUIRE(gv::checked_shader_kind(key.kind)
            == gv::shader_kind::sneaky_snake_rank_batch);
    REQUIRE(gv::descriptor_layout_for(key.to_request().kind)
            == gv::descriptor_layout_kind::align_extension);
    REQUIRE(gv::descriptor_binding_count(
        gv::descriptor_layout_for(key.to_request().kind)) == 4u);
    REQUIRE(gv::push_constant_size(key.to_request().kind) == 16u);
    REQUIRE(gv::expected_shader_workgroup_size(key)
            == (gv::shader_workgroup_size{64u, 1u, 1u}));

    const std::string body = gv::shader_body_for_digest(
        gv::shader_kind::sneaky_snake_rank_batch);
    CHECK(body.find("k_maximum_supported_edits = 31u") != std::string::npos);
    CHECK(body.find("uint query_rank_count;") != std::string::npos);
    CHECK(body.find("uint target_rank_count;") != std::string::npos);
    CHECK(body.find("prefilter_job_is_in_bounds(job)") != std::string::npos);
    CHECK(body.find("subgroup") == std::string::npos);
    CHECK(body.find("atomic") == std::string::npos);

    const auto spirv = gv::compile_shader_spirv(key.to_request());
    REQUIRE_FALSE(spirv.empty());
    const auto scan = gv::scan_spirv_layout(spirv);
    CHECK(scan.has_binding_array_stride(0u, 0u, 4u));
    CHECK(scan.has_binding_array_stride(0u, 1u, 4u));
    CHECK(scan.has_binding_array_stride(0u, 2u, 20u));
    CHECK(scan.has_binding_struct_offsets(
        0u, 2u,
        std::array<std::uint32_t, 5>{0u, 4u, 8u, 12u, 16u}));
    CHECK(scan.has_binding_array_stride(0u, 3u, 16u));
    CHECK(scan.has_binding_struct_offsets(
        0u, 3u,
        std::array<std::uint32_t, 4>{0u, 4u, 8u, 12u}));
    CHECK(scan.has_push_constant_offsets(
        std::array<std::uint32_t, 4>{0u, 4u, 8u, 12u}));
    REQUIRE(gv::validate_spirv_layout(key, spirv));

    const auto msl = gv::spirv_to_msl(spirv, key, gv::msl_version{});
    REQUIRE(msl);
    CHECK(msl->wg_x == 64u);
    CHECK(msl->wg_y == 1u);
    CHECK(msl->wg_z == 1u);
}

TEST_CASE("GateKeeper generic and production shaders share one bounds-aware ABI",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    STATIC_REQUIRE(gv::k_gpu_gatekeeper_max_length == 128u);
    STATIC_REQUIRE(gv::k_gpu_gatekeeper_max_edits == 8u);
    STATIC_REQUIRE(sizeof(pf::prefilter_job) == 20u);
    STATIC_REQUIRE(sizeof(pf::prefilter_result) == 16u);
    STATIC_REQUIRE(sizeof(gv::gpu_prefilter_push) == 16u);

    const auto key = gv::make_shader_key(
        gv::shader_kind::gatekeeper_rank_batch, 384u, 64u, 128u);
    REQUIRE(gv::is_shader_variant_key_valid(key));
    CHECK_FALSE(gv::is_shader_variant_key_valid(gv::make_shader_key(
        gv::shader_kind::gatekeeper_rank_batch, 384u, 32u, 128u)));
    CHECK_FALSE(gv::is_shader_variant_key_valid(gv::make_shader_key(
        gv::shader_kind::gatekeeper_rank_batch, 384u, 64u, 127u)));
    CHECK_FALSE(gv::is_shader_variant_key_valid(gv::make_shader_key(
        gv::shader_kind::gatekeeper_rank_batch, 383u, 64u, 128u)));
    REQUIRE(gv::checked_shader_kind(key.kind)
            == gv::shader_kind::gatekeeper_rank_batch);
    REQUIRE(gv::descriptor_layout_for(key.to_request().kind)
            == gv::descriptor_layout_kind::align_extension);
    REQUIRE(gv::descriptor_binding_count(
        gv::descriptor_layout_for(key.to_request().kind)) == 4u);
    REQUIRE(gv::push_constant_size(key.to_request().kind) == 16u);
    REQUIRE(gv::expected_shader_workgroup_size(key)
            == (gv::shader_workgroup_size{64u, 1u, 1u}));

    struct production_route_expectation {
        gv::shader_kind kind;
        std::uint32_t length;
        std::uint32_t max_edits;
        std::size_t replay_index;
    };
    const std::array production_routes{
        production_route_expectation{
            gv::shader_kind::gatekeeper_rank_batch_l32_e2, 32u, 2u, 3u},
        production_route_expectation{
            gv::shader_kind::gatekeeper_rank_batch_l32_e4, 32u, 4u, 4u},
        production_route_expectation{
            gv::shader_kind::gatekeeper_rank_batch_l64_e2, 64u, 2u, 5u},
        production_route_expectation{
            gv::shader_kind::gatekeeper_rank_batch_l64_e4, 64u, 4u, 6u},
    };

    std::array<bool, gv::k_rank_prefilter_replay_route_count>
        observed_replay_indices{};
    const auto check_replay_route = [
        &observed_replay_indices](
            gv::shader_kind kind,
            std::uint32_t push_mode,
            std::size_t expected_index) {
        const std::optional<std::size_t> replay_index =
            gv::rank_prefilter_replay_index(kind, push_mode);
        REQUIRE(replay_index);
        REQUIRE(*replay_index < observed_replay_indices.size());
        CHECK(*replay_index == expected_index);
        CHECK_FALSE(observed_replay_indices[*replay_index]);
        observed_replay_indices[*replay_index] = true;
    };
    check_replay_route(
        gv::shader_kind::sneaky_snake_rank_batch, 1u, 0u);
    check_replay_route(
        gv::shader_kind::sneaky_snake_rank_batch, 8u, 1u);
    check_replay_route(
        gv::shader_kind::gatekeeper_rank_batch, 0u, 2u);

    const gv::gatekeeper_rank_batch_route empty_route =
        gv::resolve_gatekeeper_rank_batch_route(
            std::span<const pf::prefilter_job>{});
    CHECK(empty_route.kind == gv::shader_kind::gatekeeper_rank_batch);
    CHECK(empty_route.replay_index == 2u);

    std::vector<std::string> rank_prefilter_cache_stems{
        gv::cache_file_stem(key)};
    for (const production_route_expectation& production_route
         : production_routes) {
        const gv::shader_kind production_kind = production_route.kind;
        const auto production_key = gv::make_shader_key(
            production_kind, 384u, 64u, 128u);
        const std::string production_cache_stem =
            gv::cache_file_stem(production_key);
        CAPTURE(production_cache_stem);
        CHECK(std::find(
            rank_prefilter_cache_stems.begin(),
            rank_prefilter_cache_stems.end(),
            production_cache_stem) == rank_prefilter_cache_stems.end());
        rank_prefilter_cache_stems.push_back(production_cache_stem);
        REQUIRE(gv::is_shader_variant_key_valid(production_key));
        REQUIRE(gv::descriptor_layout_for(production_kind)
                == gv::descriptor_layout_kind::align_extension);
        REQUIRE(gv::push_constant_size(production_kind) == 16u);
        check_replay_route(
            production_kind, 0u, production_route.replay_index);

        const std::array production_jobs{
            pf::prefilter_job{
                .query_offset = 0u,
                .target_offset = 0u,
                .query_length = production_route.length,
                .target_length = production_route.length,
                .max_edits = production_route.max_edits,
            },
            pf::prefilter_job{
                .query_offset = production_route.length,
                .target_offset = production_route.length,
                .query_length = production_route.length,
                .target_length = production_route.length,
                .max_edits = production_route.max_edits,
            },
        };
        const gv::gatekeeper_rank_batch_route resolved_route =
            gv::resolve_gatekeeper_rank_batch_route(production_jobs);
        CHECK(resolved_route.kind == production_kind);
        CHECK(resolved_route.replay_index == production_route.replay_index);

        const std::string production_body =
            gv::shader_body_for_digest(production_kind);
        CHECK(production_body.find(
            "#define gatekeeper_specialized_length "
            + std::to_string(production_route.length))
              != std::string::npos);
        CHECK(production_body.find(
            "#define gatekeeper_specialized_edits "
            + std::to_string(production_route.max_edits))
              != std::string::npos);

        const auto production_spirv =
            gv::compile_shader_spirv(production_key.to_request());
        REQUIRE_FALSE(production_spirv.empty());
        const auto production_scan =
            gv::scan_spirv_layout(production_spirv);
        CHECK(production_scan.has_binding_array_stride(0u, 0u, 4u));
        CHECK(production_scan.has_binding_array_stride(0u, 1u, 4u));
        CHECK(production_scan.has_binding_array_stride(0u, 2u, 20u));
        CHECK(production_scan.has_binding_struct_offsets(
            0u, 2u,
            std::array<std::uint32_t, 5>{0u, 4u, 8u, 12u, 16u}));
        CHECK(production_scan.has_binding_array_stride(0u, 3u, 16u));
        CHECK(production_scan.has_binding_struct_offsets(
            0u, 3u,
            std::array<std::uint32_t, 4>{0u, 4u, 8u, 12u}));
        CHECK(production_scan.has_push_constant_offsets(
            std::array<std::uint32_t, 4>{0u, 4u, 8u, 12u}));
        REQUIRE(gv::validate_spirv_layout(
            production_key, production_spirv));

        const auto production_msl = gv::spirv_to_msl(
            production_spirv, production_key, gv::msl_version{});
        REQUIRE(production_msl);
        CHECK(production_msl->wg_x == 64u);
        CHECK(production_msl->wg_y == 1u);
        CHECK(production_msl->wg_z == 1u);
    }
    CHECK(std::all_of(
        observed_replay_indices.begin(),
        observed_replay_indices.end(),
        [](bool observed) { return observed; }));
    CHECK_FALSE(gv::rank_prefilter_replay_index(
        gv::shader_kind::sneaky_snake_rank_batch, 0u));
    CHECK_FALSE(gv::rank_prefilter_replay_index(
        gv::shader_kind::gatekeeper_rank_batch, 1u));

    const std::array mixed_production_jobs{
        pf::prefilter_job{
            .query_offset = 0u,
            .target_offset = 0u,
            .query_length = 32u,
            .target_length = 32u,
            .max_edits = 2u,
        },
        pf::prefilter_job{
            .query_offset = 32u,
            .target_offset = 32u,
            .query_length = 64u,
            .target_length = 64u,
            .max_edits = 2u,
        },
    };
    const gv::gatekeeper_rank_batch_route mixed_route =
        gv::resolve_gatekeeper_rank_batch_route(mixed_production_jobs);
    CHECK(mixed_route.kind == gv::shader_kind::gatekeeper_rank_batch);
    CHECK(mixed_route.replay_index == 2u);

    const std::array unaligned_production_job{
        pf::prefilter_job{
            .query_offset = 1u,
            .target_offset = 0u,
            .query_length = 32u,
            .target_length = 32u,
            .max_edits = 2u,
        },
    };
    const gv::gatekeeper_rank_batch_route unaligned_route =
        gv::resolve_gatekeeper_rank_batch_route(unaligned_production_job);
    CHECK(unaligned_route.kind == gv::shader_kind::gatekeeper_rank_batch);
    CHECK(unaligned_route.replay_index == 2u);

    const std::string body = gv::shader_body_for_digest(
        gv::shader_kind::gatekeeper_rank_batch);
    CHECK(body.find("k_maximum_gatekeeper_length = 128u")
          != std::string::npos);
    CHECK(body.find("k_maximum_gatekeeper_edits = 8u")
          != std::string::npos);
    CHECK(body.find("prefilter_job_is_in_bounds(job)")
          != std::string::npos);
    CHECK(body.find("set_mask_bit(shifted_mask, shift - 1u)")
          != std::string::npos);
    CHECK(body.find("set_mask_bit(shifted_mask, length - shift)")
          != std::string::npos);
    CHECK(body.find("exact_edit_distance_is_above_threshold(job)")
          != std::string::npos);
    CHECK(body.find("k_hamming_exact_rejection_mask")
          != std::string::npos);
    CHECK(body.find("subgroup") == std::string::npos);
    CHECK(body.find("atomic") == std::string::npos);

    const auto spirv = gv::compile_shader_spirv(key.to_request());
    REQUIRE_FALSE(spirv.empty());
    const auto scan = gv::scan_spirv_layout(spirv);
    CHECK(scan.has_binding_array_stride(0u, 0u, 4u));
    CHECK(scan.has_binding_array_stride(0u, 1u, 4u));
    CHECK(scan.has_binding_array_stride(0u, 2u, 20u));
    CHECK(scan.has_binding_struct_offsets(
        0u, 2u,
        std::array<std::uint32_t, 5>{0u, 4u, 8u, 12u, 16u}));
    CHECK(scan.has_binding_array_stride(0u, 3u, 16u));
    CHECK(scan.has_binding_struct_offsets(
        0u, 3u,
        std::array<std::uint32_t, 4>{0u, 4u, 8u, 12u}));
    CHECK(scan.has_push_constant_offsets(
        std::array<std::uint32_t, 4>{0u, 4u, 8u, 12u}));
    REQUIRE(gv::validate_spirv_layout(key, spirv));

    const auto msl = gv::spirv_to_msl(spirv, key, gv::msl_version{});
    REQUIRE(msl);
    CHECK(msl->wg_x == 64u);
    CHECK(msl->wg_y == 1u);
    CHECK(msl->wg_z == 1u);
}

TEST_CASE("fixed radix workgroups have one authoritative cache ABI",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    const std::array kinds{
        gv::shader_kind::radix_sort_histogram,
        gv::shader_kind::radix_sort_scan,
        gv::shader_kind::radix_sort_scatter,
    };
    for (const gv::shader_kind kind : kinds) {
        const gv::shader_variant_key key =
            gv::make_shader_key(kind, 384u, 1u, 1u);
        CAPTURE(gv::cache_file_stem(key));
        REQUIRE(gv::is_shader_variant_key_valid(key));

        // The established key uses local_size=1 as a selector, while the
        // algorithm has one invocation per radix bin. Reflection, validation,
        // and archive selection must agree on the compiled 256-lane geometry.
        const gv::shader_workgroup_size expected =
            gv::expected_shader_workgroup_size(key);
        REQUIRE(expected == (gv::shader_workgroup_size{256u, 1u, 1u}));

        const auto spirv = gv::compile_shader_spirv(key.to_request());
        REQUIRE_FALSE(spirv.empty());
        const auto msl = gv::spirv_to_msl(spirv, key, gv::msl_version{});
        REQUIRE(msl);
        CHECK(msl->wg_x == expected.x);
        CHECK(msl->wg_y == expected.y);
        CHECK(msl->wg_z == expected.z);
    }
}

TEST_CASE("shader cache digest changes with variant ABI inputs",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    gv::shader_cache_build_inputs inputs{};
    inputs.variants = gv::vulkan_static_variant_inputs{
        .max_workgroup_invocations = 256,
        .max_shared_memory_bytes = 32u * 1024u,
        .default_window_size = 10,
    };
    inputs.moltenvk_build = "unit-test-moltenvk";
    inputs.device_uuid = "unit-test-device";

    const std::string digest_a = gv::shader_cache_digest_hex(inputs);
    const std::string digest_b = gv::shader_cache_digest_hex(inputs);
    REQUIRE(is_hex_digest(digest_a));
    CHECK(digest_a == digest_b);

    inputs.variants.default_window_size = 11;
    const std::string digest_c = gv::shader_cache_digest_hex(inputs);
    REQUIRE(is_hex_digest(digest_c));
    CHECK(digest_c != digest_a);
}

TEST_CASE("sealed shader cache manifest fixes exact AOT membership and bytes",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
    const auto abc = bytes_of("abc");
    constexpr std::array<std::uint8_t, 32> abc_sha256{
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    };
    CHECK(gv::sealed_shader_cache_detail::sha256(abc) == abc_sha256);

    temporary_directory temp;
    constexpr std::string_view digest = "0123456789abcdef";
    const gv::shader_variant_key key_64 = gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 64u);
    const gv::shader_variant_key key_32 = gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 32u);
    const std::array requested{key_64, key_32};
    write_fake_sealed_shader_artifacts(temp.path, requested);

    const auto sealed = gv::seal_shader_cache_manifest(
        temp.path, digest, requested);
    REQUIRE(sealed);
    REQUIRE(sealed->keys.size() == requested.size());
    CHECK(std::none_of(
        sealed->artifacts.begin(),
        sealed->artifacts.end(),
        [](const gv::sealed_shader_cache_artifact& artifact) {
            return artifact.kind
                == gv::sealed_shader_cache_artifact_kind::reserved_command_metallib;
        }));
    CHECK(sealed->keys[0] == key_32);
    CHECK(sealed->keys[1] == key_64);
    CHECK(sealed->seed_size > 0u);
    CHECK(sealed->manifest_checksum != 0u);
    CHECK(std::filesystem::file_size(
              gv::sealed_shader_cache_manifest_path(temp.path)) > 0u);

    // Expected membership is a set at the API, but the stored wire image is
    // canonical and exact. Reordering the request cannot change acceptance.
    const std::array reverse_expected{key_64, key_32};
    const auto loaded = gv::load_validated_sealed_shader_cache_manifest(
        temp.path, digest, reverse_expected);
    REQUIRE(loaded);
    CHECK(loaded->seed_sha256 == sealed->seed_sha256);
    CHECK(loaded->artifacts == sealed->artifacts);

    shader_module_create_called = false;
    const auto module = gv::load_sealed_cached_module(
        VK_NULL_HANDLE,
        temp.path,
        digest,
        reverse_expected,
        key_64,
        &record_shader_module_create);
    REQUIRE(module);
    CHECK(shader_module_create_called);

    shader_module_create_called = false;
    const gv::shader_variant_key nonmember = gv::make_shader_key(
        gv::shader_kind::align_extend_banded_affine, 384u, 1u, 512u);
    const auto rejected = gv::load_sealed_cached_module(
        VK_NULL_HANDLE,
        temp.path,
        digest,
        reverse_expected,
        nonmember,
        &record_shader_module_create);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().find("not a member of the sealed AOT seed")
          != std::string::npos);
    CHECK_FALSE(shader_module_create_called);

    const std::array wrong_expected{key_64};
    const auto wrong_membership =
        gv::load_validated_sealed_shader_cache_manifest(
            temp.path, digest, wrong_expected);
    REQUIRE_FALSE(wrong_membership);
    CHECK(wrong_membership.error().find("key membership mismatch")
          != std::string::npos);
}

TEST_CASE("sealed shader module uses the exact already-hashed metallib image",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
    temporary_directory temp;
    constexpr std::string_view digest = "1122334455667788";
    const gv::shader_variant_key key = gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 64u);
    const std::array keys{key};
    write_fake_sealed_shader_artifacts(temp.path, keys);
    REQUIRE(gv::seal_shader_cache_manifest(temp.path, digest, keys));

    auto image = gv::load_sealed_shader_cache_key_image(
        temp.path, digest, keys, key);
    REQUIRE(image);
    expected_shader_module_metallib = image->metallib_bytes;
    const auto replacement = bytes_of("mutated-after-verified-image-load");
    REQUIRE(gv::durable_write_file(
        gv::cache_metallib_path(temp.path, key), replacement));

    shader_module_create_called = false;
    shader_module_payload_matches = false;
    const auto module = gv::create_shader_module_from_sealed_key_image(
        VK_NULL_HANDLE, *image, &record_exact_shader_module_create);
    REQUIRE(module);
    CHECK(shader_module_create_called);
    CHECK(shader_module_payload_matches);

    // A fresh provider call sees the mutation and fails before Vulkan. The
    // The verified image remains immutable and needs no pathname reopen.
    shader_module_create_called = false;
    const auto rejected = gv::load_sealed_cached_module(
        VK_NULL_HANDLE,
        temp.path,
        digest,
        keys,
        key,
        &record_shader_module_create);
    REQUIRE_FALSE(rejected);
    CHECK_FALSE(shader_module_create_called);
    CHECK((rejected.error().find("artifact size mismatch") != std::string::npos
           || rejected.error().find("artifact SHA-256 mismatch")
               != std::string::npos));
    expected_shader_module_metallib.clear();
}

TEST_CASE("process seed reads once and survives source mutation and deletion",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
    STATIC_REQUIRE_FALSE(std::is_default_constructible_v<
                         gv::sealed_shader_cache_seed_receipt>);
    STATIC_REQUIRE_FALSE(std::is_default_constructible_v<
                         gv::sealed_shader_cache_seed_image>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<
                         gv::sealed_shader_cache_seed_image>);

    temporary_directory temp;
    constexpr std::string_view digest = "22446688aaccee00";
    const gv::shader_variant_key key = gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 64u);
    const gv::shader_variant_key earlier_key = gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 32u);
    // This intentionally matches producer traversal rather than manifest
    // ordering; every production consumer uses the shared canonical contract.
    const std::array keys{key, earlier_key};
    const auto canonical = gv::canonical_sealed_shader_cache_keys(keys);
    REQUIRE(canonical);
    REQUIRE(canonical->front() == earlier_key);
    write_fake_sealed_shader_artifacts(temp.path, keys);
    REQUIRE(gv::seal_shader_cache_manifest(temp.path, digest, keys));

    gv::sealed_shader_cache_process_store process_store;
    auto first = process_store.load_once(temp.path, digest, keys);
    REQUIRE(first);
    const std::array reverse_keys{earlier_key, key};
    auto second = process_store.load_once(temp.path, digest, reverse_keys);
    REQUIRE(second);
    CHECK(first->get() == second->get());
    CHECK((*first)->receipt().identity() == (*second)->receipt().identity());
    CHECK(process_store.filesystem_load_count() == 1u);
    const auto seed = *first;
    REQUIRE(seed->is_accepted());
    const auto& receipt = seed->receipt();
    CHECK(receipt.load_count() == 1u);
    CHECK(receipt.manifest_read_count() == 1u);
    CHECK(receipt.artifact_read_count() == seed->artifacts().size());
    CHECK(receipt.artifact_bytes() == seed->manifest().seed_size);
    CHECK(receipt.manifest_bytes() == seed->manifest_bytes().size());
    CHECK(receipt.load_ns() > 0u);
    CHECK(receipt.admission_count() == 1u);
    CHECK(receipt.peak_admitted_bytes() >=
          receipt.retained_capacity_bytes());
    CHECK(receipt.retained_capacity_bytes()
          == seed->owned_capacity_bytes());
    CHECK_FALSE(receipt.admission_saturated());

    auto key_view = gv::make_sealed_shader_cache_key_view(*seed, key);
    REQUIRE(key_view);
    expected_shader_module_metallib.assign(
        key_view->metallib_bytes.begin(), key_view->metallib_bytes.end());

    const auto archive = std::find_if(
        seed->artifacts().begin(), seed->artifacts().end(),
        [](const gv::sealed_shader_cache_artifact_image& image) {
            return image.descriptor.kind
                == gv::sealed_shader_cache_artifact_kind::metal_binary_archive;
        });
    REQUIRE(archive != seed->artifacts().end());
    const auto qualification = std::find_if(
        seed->artifacts().begin(), seed->artifacts().end(),
        [&archive](const gv::sealed_shader_cache_artifact_image& image) {
            return image.descriptor.kind
                    == gv::sealed_shader_cache_artifact_kind::
                        metal_binary_archive_qualification
                && image.descriptor.filename
                    == archive->descriptor.filename + ".qualification";
        });
    REQUIRE(qualification != seed->artifacts().end());
    REQUIRE(gv::sealed_shader_cache_detail::
        parse_metal_archive_qualification_receipt(qualification->bytes));

    REQUIRE(gv::durable_write_file(
        gv::cache_metallib_path(temp.path, key), bytes_of("mutated")));
    std::error_code remove_error;
    std::filesystem::remove_all(temp.path, remove_error);
    REQUIRE_FALSE(remove_error);

    shader_module_create_called = false;
    shader_module_payload_matches = false;
    const auto module = gv::load_sealed_cached_module(
        VK_NULL_HANDLE, *seed, key, &record_exact_shader_module_create);
    REQUIRE(module);
    CHECK(shader_module_create_called);
    CHECK(shader_module_payload_matches);

    auto& admission =
        bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    admission.set_index_memory_cap_bytes(admission.reserved_bytes());
    shader_module_create_called = false;
    shader_module_payload_matches = false;
    const auto denied_module = gv::load_sealed_cached_module(
        VK_NULL_HANDLE, *seed, key, &record_exact_shader_module_create);
    admission.set_index_memory_cap_bytes(previous_cap);
    REQUIRE_FALSE(denied_module);
    CHECK(denied_module.error().find("module admission denied")
          != std::string::npos);
    CHECK_FALSE(shader_module_create_called);

    shader_module_create_called = false;
    shader_module_payload_matches = false;
    const auto second_module = gv::load_sealed_cached_module(
        VK_NULL_HANDLE, *seed, key, &record_exact_shader_module_create);
    REQUIRE(second_module);
    CHECK(shader_module_create_called);
    CHECK(shader_module_payload_matches);
    CHECK(gv::sealed_shader_cache_detail::sha256(archive->bytes)
          == archive->descriptor.sha256);
    expected_shader_module_metallib.clear();
}

TEST_CASE("process seed rejects oversized manifest before artifact allocation",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
    temporary_directory temp;
    constexpr std::string_view digest = "33557799bbddff11";
    const gv::shader_variant_key key = gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 64u);
    const std::array keys{key};
    write_fake_sealed_shader_artifacts(temp.path, keys);
    const auto sealed = gv::seal_shader_cache_manifest(
        temp.path, digest, keys);
    REQUIRE(sealed);

    gv::sealed_shader_cache_manifest oversized = *sealed;
    oversized.seed_size =
        gv::sealed_shader_cache_detail::k_maximum_seed_image_bytes + 1u;
    const auto wire =
        gv::sealed_shader_cache_detail::serialize_manifest(oversized);
    REQUIRE(wire);
    REQUIRE(gv::durable_write_file(
        gv::sealed_shader_cache_manifest_path(temp.path), wire->first));
    const auto rejected = gv::load_validated_sealed_shader_cache_seed(
        temp.path, digest, keys);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().find("exceeds bounded ownership")
          != std::string::npos);
}

TEST_CASE("seal rejects oversized seed before publishing or reading it",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
    temporary_directory temp;
    constexpr std::string_view digest = "22446688aaccee00";
    const gv::shader_variant_key key = gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 64u);
    const std::array keys{key};
    write_fake_sealed_shader_artifacts(temp.path, keys);
    std::error_code error;
    std::filesystem::resize_file(
        gv::cache_metallib_path(temp.path, key),
        gv::sealed_shader_cache_detail::k_maximum_seed_image_bytes + 1u,
        error);
    REQUIRE_FALSE(error);
    const auto sealed = gv::seal_shader_cache_manifest(
        temp.path, digest, keys);
    REQUIRE_FALSE(sealed);
    CHECK(sealed.error().find("beyond byte cap") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(
        gv::sealed_shader_cache_manifest_path(temp.path)));
    CHECK_FALSE(std::filesystem::exists(
        gv::sealed_shader_cache_manifest_path(temp.path).string() + ".tmp"));
}

TEST_CASE("seal validates the admitted candidate before atomic publication",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
    temporary_directory temp;
    constexpr std::string_view digest = "1133557799bbddff";
    const gv::shader_variant_key key = gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 64u);
    const std::array keys{key};
    write_fake_sealed_shader_artifacts(temp.path, keys);
    REQUIRE(gv::durable_write_file(
        gv::cache_meta_path(temp.path, key), bytes_of("bad-meta")));
    const auto sealed = gv::seal_shader_cache_manifest(
        temp.path, digest, keys);
    REQUIRE_FALSE(sealed);
    CHECK(sealed.error().find("metadata") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(
        gv::sealed_shader_cache_manifest_path(temp.path)));
    CHECK_FALSE(std::filesystem::exists(
        gv::sealed_shader_cache_manifest_path(temp.path).string() + ".tmp"));
}

TEST_CASE("seal rejects same-size mutation between read and publication",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
    temporary_directory temp;
    constexpr std::string_view digest = "0011223344556677";
    const gv::shader_variant_key key = gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 64u);
    const std::array keys{key};
    write_fake_sealed_shader_artifacts(temp.path, keys);
    seal_identity_mutation_path = gv::cache_metallib_path(temp.path, key);
    seal_identity_mutation_succeeded = false;
    const auto sealed = gv::seal_shader_cache_manifest_for_test(
        temp.path, digest, keys, &mutate_seal_artifact_same_size);
    seal_identity_mutation_path.clear();
    REQUIRE(seal_identity_mutation_succeeded);
    REQUIRE_FALSE(sealed);
    CHECK(sealed.error().find("identity changed before publication")
          != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(
        gv::sealed_shader_cache_manifest_path(temp.path)));
    CHECK_FALSE(std::filesystem::exists(
        gv::sealed_shader_cache_manifest_path(temp.path).string() + ".tmp"));
}

TEST_CASE("private archive staging canonicalizes a symlinked temp parent",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
#if !defined(__APPLE__)
    SKIP("private Metal archive snapshots are Apple-only");
#else
    temporary_directory temp;
    const std::filesystem::path real_parent = temp.path / "real";
    const std::filesystem::path linked_parent = temp.path / "linked";
    std::error_code error;
    std::filesystem::create_directory(real_parent, error);
    REQUIRE_FALSE(error);
    std::filesystem::create_directory_symlink(
        real_parent, linked_parent, error);
    REQUIRE_FALSE(error);

    const auto retained = bytes_of("archive-source-is-already-deleted");
    const std::filesystem::path source = temp.path / "source.metalar";
    REQUIRE(gv::durable_write_file(source, retained));
    std::filesystem::remove(source, error);
    REQUIRE_FALSE(error);
    REQUIRE_FALSE(std::filesystem::exists(source));

    const auto snapshot = gv::verify_runtime_private_archive_snapshot_for_test(
        "safe-test.metalar", retained, linked_parent);
    REQUIRE(snapshot);
    CHECK(snapshot->snapshot_path.is_absolute());
    CHECK(snapshot->snapshot_path.parent_path()
          == std::filesystem::canonical(real_parent));
    CHECK(snapshot->streamed_bytes == retained.size());
    CHECK(snapshot->scratch_capacity_bytes == 64u * 1024u);
    CHECK(snapshot->exact_seed_compared);
    CHECK(snapshot->tamper_rejected);
    CHECK_FALSE(std::filesystem::exists(snapshot->snapshot_path));
#endif
}

TEST_CASE("sealed shader cache rejects artifact, wire, and symlink mutation",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
    temporary_directory temp;
    constexpr std::string_view digest = "fedcba9876543210";
    const gv::shader_variant_key key = gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 64u);
    const std::array keys{key};
    write_fake_sealed_shader_artifacts(temp.path, keys);
    REQUIRE(gv::seal_shader_cache_manifest(temp.path, digest, keys));

    const std::filesystem::path metallib =
        gv::cache_metallib_path(temp.path, key);
    const std::vector<std::byte> original_metallib = read_file_bytes(metallib);
    const auto corrupted = bytes_of("same-cache-name-different-metallib");
    REQUIRE(gv::durable_write_file(metallib, corrupted));
    auto rejected = gv::load_validated_sealed_shader_cache_manifest(
        temp.path, digest, keys);
    REQUIRE_FALSE(rejected);
    CHECK((rejected.error().find("artifact size mismatch") != std::string::npos
           || rejected.error().find("artifact SHA-256 mismatch")
               != std::string::npos));
    REQUIRE(gv::durable_write_file(metallib, original_metallib));
    REQUIRE(gv::load_validated_sealed_shader_cache_manifest(
        temp.path, digest, keys));

    const std::filesystem::path qualification =
        gv::cache_binary_archive_qualification_path(
            gv::cache_binary_archive_path(temp.path, key));
    const std::vector<std::byte> original_qualification =
        read_file_bytes(qualification);
    std::vector<std::byte> corrupt_qualification = original_qualification;
    REQUIRE(corrupt_qualification.size() > 50u);
    corrupt_qualification[50u] ^= std::byte{0x01};
    REQUIRE(gv::durable_write_file(qualification, corrupt_qualification));
    rejected = gv::load_validated_sealed_shader_cache_manifest(
        temp.path, digest, keys);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().find("artifact SHA-256 mismatch")
          != std::string::npos);
    REQUIRE(gv::durable_write_file(
        qualification, original_qualification));
    REQUIRE(gv::load_validated_sealed_shader_cache_manifest(
        temp.path, digest, keys));

    const std::filesystem::path manifest_path =
        gv::sealed_shader_cache_manifest_path(temp.path);
    const std::vector<std::byte> original_manifest =
        read_file_bytes(manifest_path);
    std::vector<std::byte> corrupt_manifest = original_manifest;
    REQUIRE(corrupt_manifest.size() > 8u);
    corrupt_manifest.back() ^= std::byte{0x01};
    REQUIRE(gv::durable_write_file(manifest_path, corrupt_manifest));
    rejected = gv::load_validated_sealed_shader_cache_manifest(
        temp.path, digest, keys);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().find("checksum mismatch") != std::string::npos);
    REQUIRE(gv::durable_write_file(manifest_path, original_manifest));

    const std::filesystem::path owned_metallib = metallib.string() + ".owned";
    std::error_code ec;
    std::filesystem::rename(metallib, owned_metallib, ec);
    REQUIRE_FALSE(ec);
    std::filesystem::create_symlink(
        owned_metallib.filename(), metallib, ec);
    REQUIRE_FALSE(ec);
    rejected = gv::load_validated_sealed_shader_cache_manifest(
        temp.path, digest, keys);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().find("must not be a symlink") != std::string::npos);
}

TEST_CASE("archive helper success sentinel is unique and final",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    CHECK(gv::archive_helper_transcript_is_exact_success("qualification record\nRESULT: ok\n"));
    CHECK_FALSE(
        gv::archive_helper_transcript_is_exact_success("qualification record\nRESULT: failed\n"));
    CHECK_FALSE(gv::archive_helper_transcript_is_exact_success(
        "RESULT: ok\ntrailing output\n"));
    CHECK_FALSE(gv::archive_helper_transcript_is_exact_success(
        "RESULT: failed\nRESULT: ok\n"));
    CHECK_FALSE(gv::archive_helper_transcript_is_exact_success("RESULT: ok"));
}

TEST_CASE("archive miss NSError fingerprint and receipt are canonical",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    constexpr std::string_view transcript =
        "same-descriptor empty-archive negative control: ok\n"
        "NEGATIVE_CONTROL_NSError domain_hex="
        "4d544c4c6962726172794572726f72446f6d61696e code=-7\n"
        "RESULT: ok\n";
    const auto fingerprint =
        gv::parse_archive_helper_negative_error_fingerprint(transcript);
    REQUIRE(fingerprint);
    CHECK(fingerprint->domain == "MTLLibraryErrorDomain");
    CHECK(fingerprint->code == -7);

    CHECK_FALSE(gv::parse_archive_helper_negative_error_fingerprint(
        "NEGATIVE_CONTROL_NSError domain_hex=4D54 code=1\nRESULT: ok\n"));
    CHECK_FALSE(gv::parse_archive_helper_negative_error_fingerprint(
        "NEGATIVE_CONTROL_NSError domain_hex=4d54 code=nope\nRESULT: ok\n"));
    CHECK_FALSE(gv::parse_archive_helper_negative_error_fingerprint(
        "prefix NEGATIVE_CONTROL_NSError domain_hex=4d54 code=1\n"
        "RESULT: ok\n"));
    CHECK_FALSE(gv::parse_archive_helper_negative_error_fingerprint(
        std::string{transcript} + std::string{transcript}));

    const auto helper_qualification =
        gv::parse_archive_helper_qualification(
            "HELPER_QUALIFICATION wire_revision=2 "
            "build=bioseq_gpu_archive_builder registry_id=4660\n"
            "RESULT: ok\n");
    REQUIRE(helper_qualification);
    CHECK(helper_qualification->wire_revision == 2u);
    CHECK(helper_qualification->build_identity
          == "bioseq_gpu_archive_builder");
    CHECK(helper_qualification->metal_registry_id == 4660u);
    CHECK_FALSE(gv::parse_archive_helper_qualification(
        "HELPER_QUALIFICATION wire_revision=1 "
        "build=bioseq_gpu_archive_builder registry_id=4660\n"));
    CHECK_FALSE(gv::parse_archive_helper_qualification(
        "HELPER_QUALIFICATION wire_revision=2 build=other registry_id=4660\n"));
    CHECK_FALSE(gv::parse_archive_helper_qualification(
        "HELPER_QUALIFICATION wire_revision=2 "
        "build=bioseq_gpu_archive_builder registry_id=0\n"));

    const auto archive = bytes_of("qualified-archive-byte-image");
    const auto metallib = bytes_of("qualified-metallib-byte-image");
    const auto helper = bytes_of("qualified-helper-byte-image");
    const gv::shader_variant_key key = gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 64u);
    const gv::metal_archive_qualification_receipt receipt{
        .wire_revision = gv::sealed_shader_cache_detail::
            k_archive_qualification_wire_revision,
        .shader_key = key,
        .metallib_size = static_cast<std::uint64_t>(metallib.size()),
        .metallib_sha256 =
            gv::sealed_shader_cache_detail::sha256(metallib),
        .entry_name = "main0",
        .max_total_threads_per_threadgroup = 256u,
        .threadgroup_size_is_multiple = false,
        .negative_error = *fingerprint,
        .archive_size = static_cast<std::uint64_t>(archive.size()),
        .archive_sha256 = gv::sealed_shader_cache_detail::sha256(archive),
        .helper_wire_revision = 2u,
        .helper_build_identity = "bioseq_gpu_archive_builder",
        .helper_source = "installed-sibling",
        .helper_executable_size = static_cast<std::uint64_t>(helper.size()),
        .helper_executable_sha256 =
            gv::sealed_shader_cache_detail::sha256(helper),
        .helper_metal_registry_id = 0x1234u,
    };
    auto wire = gv::sealed_shader_cache_detail::
        serialize_metal_archive_qualification_receipt(receipt);
    REQUIRE(wire);
    const auto parsed = gv::sealed_shader_cache_detail::
        parse_metal_archive_qualification_receipt(*wire);
    REQUIRE(parsed);
    CHECK(*parsed == receipt);

    REQUIRE(wire->size() > 50u);
    wire->at(50u) ^= std::byte{0x01};
    const auto corrupted = gv::sealed_shader_cache_detail::
        parse_metal_archive_qualification_receipt(*wire);
    REQUIRE_FALSE(corrupted);
    CHECK(corrupted.error().find("checksum mismatch") != std::string::npos);
}

TEST_CASE("failed archive helper output is never promoted",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:subprocess]") {
#if !defined(__APPLE__)
    SKIP("Metal archive helper is an Apple-only publication path");
#else
    temporary_directory temp;
    const std::filesystem::path helper = temp.path / "false_archive_helper.sh";
    const auto script = bytes_of(
        "#!/bin/sh\n"
        "printf 'partial archive bytes' > \"$2\"\n"
        "printf 'qualification failed without success sentinel\\n'\n"
        "exit 0\n");
    REQUIRE(gv::durable_write_file(helper, script));
    REQUIRE(::chmod(helper.c_str(), 0700) == 0);
    scoped_environment_variable test_only_env{
        "BIOSEQ_GPU_ARCHIVE_HELPER_TEST_ONLY", "1"};
    scoped_environment_variable helper_env{
        "BIOSEQ_GPU_ARCHIVE_HELPER", helper.string()};
    scoped_archive_helper_store_reset helper_store_reset;

    const gv::shader_variant_key key = gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 64u);
    const std::array keys{key};
    write_fake_sealed_shader_artifacts(temp.path, keys);
    constexpr std::string_view forged_entry = "wrong_semantic_entry";
    const gv::shader_workgroup_size workgroup =
        gv::expected_shader_workgroup_size(key);
    gv::shader_cache_metadata forged_meta{};
    forged_meta.key = key;
    forged_meta.wg_x = workgroup.x;
    forged_meta.wg_y = workgroup.y;
    forged_meta.wg_z = workgroup.z;
    forged_meta.entry_len = static_cast<std::uint32_t>(forged_entry.size());
    REQUIRE(gv::durable_write_file(
        gv::cache_meta_path(temp.path, key),
        gv::meta_to_bytes(forged_meta, forged_entry)));
    REQUIRE(gv::cache_entry_complete(temp.path, key));
    const std::vector<std::byte> stale_meta = read_file_bytes(
        gv::cache_meta_path(temp.path, key));
    const std::vector<std::byte> stale_metallib = read_file_bytes(
        gv::cache_metallib_path(temp.path, key));
    const std::filesystem::path final =
        gv::cache_binary_archive_path(temp.path, key);
    REQUIRE(std::filesystem::exists(final));

    // A merely nonempty preexisting archive is not reusable while building a
    // replacement seed. Qualification removes it first. Failure leaves nothing
    // that a later manifest publication could accidentally bless.
    const auto result = gv::ensure_compiled_for_sealed_seed(
        temp.path, key, gv::msl_version{});
    REQUIRE_FALSE(result);
    CHECK(result.error().find("without exact success sentinel")
          != std::string::npos);
    CHECK(read_file_bytes(gv::cache_meta_path(temp.path, key)) != stale_meta);
    CHECK(read_file_bytes(gv::cache_metallib_path(temp.path, key))
          != stale_metallib);
    CHECK_FALSE(std::filesystem::exists(final));
    CHECK_FALSE(std::filesystem::exists(final.string() + ".tmp"));
    const auto cannot_seal = gv::seal_shader_cache_manifest(
        temp.path, "0123456789abcdef", keys);
    REQUIRE_FALSE(cannot_seal);
    CHECK(cannot_seal.error().find(
              "cannot seal non-regular shader artifact")
          != std::string::npos);
    CHECK(cannot_seal.error().find(final.string()) != std::string::npos);
#endif
}

TEST_CASE("archive helper qualification is terminal and hashes once",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
#if !defined(__APPLE__)
    SKIP("archive helper qualification is Apple-only");
#else
    temporary_directory temp;
    const std::filesystem::path helper = temp.path / "helper.sh";
    const auto helper_bytes = bytes_of("#!/bin/sh\nexit 1\n");
    REQUIRE(gv::durable_write_file(helper, helper_bytes));
    REQUIRE(::chmod(helper.c_str(), 0700) == 0);
    scoped_environment_variable test_only{
        "BIOSEQ_GPU_ARCHIVE_HELPER_TEST_ONLY", "1"};
    scoped_environment_variable override{
        "BIOSEQ_GPU_ARCHIVE_HELPER", helper.string()};

    gv::qualified_metal_archive_helper_process_store store;
    const auto first = store.load_once();
    REQUIRE(first);
    CHECK((**first).source == "test-env-override");
    CHECK(store.qualification_attempt_count() == 1u);
    CHECK(store.filesystem_hash_count() == 1u);
    REQUIRE(gv::durable_write_file(helper, bytes_of("mutated")));
    const auto second = store.load_once();
    REQUIRE(second);
    CHECK(first->get() == second->get());
    CHECK((**second).executable_bytes == helper_bytes);
    CHECK(store.qualification_attempt_count() == 1u);
    CHECK(store.filesystem_hash_count() == 1u);

    const std::filesystem::path metallib = temp.path / "input.metallib";
    REQUIRE(gv::durable_write_file(metallib, bytes_of("metallib")));
    const int metallib_fd = ::open(
        metallib.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    REQUIRE(metallib_fd >= 0);
    auto staged = gv::stage_archive_helper_invocation(**first, metallib_fd);
    REQUIRE(staged);
    const std::filesystem::path staged_path = staged->helper_path();
    const std::filesystem::path staged_directory = staged_path.parent_path();
    CHECK(staged->helper_identity_matches());
    CHECK(std::filesystem::exists(staged_path));
    REQUIRE(staged->finish());
    CHECK_FALSE(std::filesystem::exists(staged_path));
    CHECK_FALSE(std::filesystem::exists(staged_directory));
    REQUIRE(::close(metallib_fd) == 0);

    const std::filesystem::path symlink = temp.path / "helper-link";
    REQUIRE(::symlink(helper.c_str(), symlink.c_str()) == 0);
    scoped_environment_variable symlink_override{
        "BIOSEQ_GPU_ARCHIVE_HELPER", symlink.string()};
    gv::qualified_metal_archive_helper_process_store failed_store;
    CHECK_FALSE(failed_store.load_once());
    CHECK_FALSE(failed_store.load_once());
    CHECK(failed_store.qualification_attempt_count() == 1u);
    CHECK(failed_store.filesystem_hash_count() == 0u);

    REQUIRE(::chmod(helper.c_str(), 0702) == 0);
    scoped_environment_variable writable_override{
        "BIOSEQ_GPU_ARCHIVE_HELPER", helper.string()};
    gv::qualified_metal_archive_helper_process_store writable_store;
    CHECK_FALSE(writable_store.load_once());
    CHECK(writable_store.qualification_attempt_count() == 1u);
    CHECK(writable_store.filesystem_hash_count() == 0u);
#endif
}

TEST_CASE("archive helper override is explicitly test-only",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
#if !defined(__APPLE__)
    SKIP("archive helper qualification is Apple-only");
#else
    temporary_directory temp;
    const std::filesystem::path helper = temp.path / "helper.sh";
    REQUIRE(gv::durable_write_file(helper, bytes_of("#!/bin/sh\nexit 1\n")));
    REQUIRE(::chmod(helper.c_str(), 0700) == 0);
    scoped_environment_variable test_only{
        "BIOSEQ_GPU_ARCHIVE_HELPER_TEST_ONLY", "0"};
    scoped_environment_variable override{
        "BIOSEQ_GPU_ARCHIVE_HELPER", helper.string()};
    gv::qualified_metal_archive_helper_process_store store;
    CHECK_FALSE(store.load_once());
    CHECK_FALSE(store.load_once());
    CHECK(store.qualification_attempt_count() == 1u);
    CHECK(store.filesystem_hash_count() == 0u);
#endif
}

TEST_CASE("shader cache metadata and MoltenVK header payload are exact",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
    temporary_directory temp;
    const gv::shader_variant_key key = gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 64u);
    constexpr std::string_view entry = "main0";

    gv::shader_cache_metadata meta{};
    meta.key = key;
    meta.wg_x = 256u;
    meta.wg_y = 1u;
    meta.wg_z = 1u;
    meta.entry_len = static_cast<std::uint32_t>(entry.size());

    const std::vector<std::byte> meta_bytes = gv::meta_to_bytes(meta, entry);
    auto meta_write = gv::durable_write_file(
        gv::cache_meta_path(temp.path, key),
        std::span<const std::byte>(meta_bytes.data(), meta_bytes.size()));
    REQUIRE(meta_write);

    const std::vector<std::byte> metallib = bytes_of("not-a-real-metallib");
    auto metallib_write = gv::durable_write_file(
        gv::cache_metallib_path(temp.path, key),
        std::span<const std::byte>(metallib.data(), metallib.size()));
    REQUIRE(metallib_write);

    // cache_entry_complete also requires a non-empty .metalar binary archive on Apple.
    const std::vector<std::byte> fake_archive = bytes_of("not-a-real-metalar");
    auto archive_write = gv::durable_write_file(
        gv::cache_binary_archive_path(temp.path, key),
        std::span<const std::byte>(fake_archive.data(), fake_archive.size()));
    REQUIRE(archive_write);
    write_fake_archive_qualification(
        gv::cache_binary_archive_path(temp.path, key), fake_archive,
        key, metallib, entry);
    REQUIRE(gv::cache_entry_complete(temp.path, key));

    std::string decoded_entry;
    const auto decoded_meta = gv::read_meta(
        gv::cache_meta_path(temp.path, key), &decoded_entry);
    REQUIRE(decoded_meta);
    CHECK(decoded_meta->key == key);
    CHECK(decoded_meta->wg_x == 256u);
    CHECK(decoded_entry == entry);

    // A cache entry with the right key but a different compiled workgroup ABI
    // is not reusable. This checks the metadata contract alongside the
    // digest-level resource-binding ABI revision.
    gv::shader_cache_metadata mismatched_meta = meta;
    mismatched_meta.wg_x = 128u;
    const std::vector<std::byte> mismatched_meta_bytes =
        gv::meta_to_bytes(mismatched_meta, entry);
    auto mismatched_meta_write = gv::durable_write_file(
        gv::cache_meta_path(temp.path, key),
        std::span<const std::byte>(
            mismatched_meta_bytes.data(), mismatched_meta_bytes.size()));
    REQUIRE(mismatched_meta_write);
    CHECK_FALSE(gv::cache_metallib_entry_complete(temp.path, key));
    CHECK_FALSE(gv::cache_entry_complete(temp.path, key));

    shader_module_create_called = false;
    const auto loader_rejected = gv::load_cached_module(
        VK_NULL_HANDLE, temp.path, key, &record_shader_module_create);
    REQUIRE_FALSE(loader_rejected);
    CHECK(loader_rejected.error().find("meta workgroup mismatch")
          != std::string::npos);
    CHECK_FALSE(shader_module_create_called);

    no_publish_compile_service no_publish_service{};
    const auto post_compile_rejected = gv::load_module(
        VK_NULL_HANDLE,
        temp.path,
        key,
        &record_shader_module_create,
        no_publish_service);
    REQUIRE_FALSE(post_compile_rejected);
    CHECK(post_compile_rejected.error().find("cache entry")
          != std::string::npos);
    CHECK(post_compile_rejected.error().find("meta workgroup mismatch")
          != std::string::npos);
    CHECK(no_publish_service.calls == 1);
    CHECK_FALSE(shader_module_create_called);

    auto& admission =
        bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t reserved_before = admission.reserved_bytes();
    {
        const auto module_code = gv::make_moltenvk_metallib_module_code(
            gv::cache_metallib_path(temp.path, key));
        REQUIRE(module_code);
        CHECK(module_code->admission_count == 1u);
        CHECK(module_code->peak_admitted_bytes
              >= module_code->retained_capacity_bytes);
        CHECK(module_code->retained_capacity_bytes > 0u);
        CHECK_FALSE(module_code->admission_saturated);
        CHECK(module_code->permit.bytes()
              == module_code->retained_capacity_bytes);
        CHECK(module_code->header_size_bytes
              == gv::moltenvk_msl_spirv_header_size());
        CHECK(module_code->code_size_bytes
              == gv::moltenvk_msl_spirv_header_size() + metallib.size());
        CHECK(reinterpret_cast<std::uintptr_t>(module_code->words.data())
              % alignof(std::uint32_t) == 0u);

        const auto* submitted =
            reinterpret_cast<const std::byte*>(module_code->words.data());
        REQUIRE(module_code->header_size_bytes >= sizeof(std::uint32_t));
        std::uint32_t magic = 0;
        std::memcpy(&magic, submitted, sizeof(magic));
        CHECK(magic == gv::moltenvk_msl_compiled_code_magic());
        CHECK(std::memcmp(
                  submitted + module_code->header_size_bytes,
                  metallib.data(),
                  metallib.size()) == 0);
    }
    CHECK(admission.reserved_bytes() == reserved_before);
}

TEST_CASE("fixed radix cache accepts reflected workgroup and exact archive",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
    temporary_directory temp;
    const gv::shader_variant_key key = gv::make_shader_key(
        gv::shader_kind::radix_sort_histogram, 384u, 1u, 1u);
    constexpr std::string_view entry = "main0";

    gv::shader_cache_metadata meta{};
    meta.key = key;
    meta.wg_x = 256u;
    meta.wg_y = 1u;
    meta.wg_z = 1u;
    meta.entry_len = static_cast<std::uint32_t>(entry.size());
    const std::vector<std::byte> meta_bytes = gv::meta_to_bytes(meta, entry);
    REQUIRE(gv::durable_write_file(
        gv::cache_meta_path(temp.path, key),
        std::span<const std::byte>(meta_bytes.data(), meta_bytes.size())));

    const std::vector<std::byte> metallib = bytes_of("fixed-radix-metallib");
    REQUIRE(gv::durable_write_file(
        gv::cache_metallib_path(temp.path, key),
        std::span<const std::byte>(metallib.data(), metallib.size())));
    const std::vector<std::byte> archive = bytes_of("fixed-radix-metalar");
    REQUIRE(gv::durable_write_file(
        gv::cache_binary_archive_path(temp.path, key),
        std::span<const std::byte>(archive.data(), archive.size())));
    write_fake_archive_qualification(
        gv::cache_binary_archive_path(temp.path, key), archive,
        key, metallib, entry);

    // Publication uses reflected threads, so a 256-thread shader needs only
    // the exact base archive; completeness must not invent a duplicate
    // `.max_threads_256.metalar` from the key's sentinel local_size.
    CHECK(gv::cache_binary_archive_paths(temp.path, key).size() == 1u);
    REQUIRE(gv::cache_entry_status(temp.path, key));
    REQUIRE(gv::cache_entry_complete(temp.path, key));

    shader_module_create_called = false;
    no_publish_compile_service should_not_compile{};
    const auto loaded = gv::load_module(
        VK_NULL_HANDLE,
        temp.path,
        key,
        &record_shader_module_create,
        should_not_compile);
    REQUIRE(loaded);
    CHECK(loaded->wg_x == 256u);
    CHECK(should_not_compile.calls == 0);
    CHECK(shader_module_create_called);
}

namespace {

[[nodiscard]] gv::runtime_metal_archive_configuration
accepted_runtime_metal_archive_configuration() {
    temporary_directory temp;
    constexpr std::string_view digest = "0123456789abcdef";
    const gv::shader_variant_key key = gv::make_shader_key(
        gv::shader_kind::chain_tiled_dp_u64, 32u, 32u, 0u);
    const std::array keys{key};
    REQUIRE(gv::cache_binary_archive_paths(temp.path, key).size() == 2u);
    write_fake_sealed_shader_artifacts(temp.path, keys);
    REQUIRE(gv::seal_shader_cache_manifest(temp.path, digest, keys));
    auto loaded = gv::load_validated_sealed_shader_cache_seed(
        temp.path, digest, keys);
    REQUIRE(loaded);
    const auto seed = std::make_shared<const gv::sealed_shader_cache_seed_image>(
        std::move(*loaded));
    gv::runtime_metal_archive_configuration configuration{};
    configuration.seed_image = seed;
    configuration.vulkan_device_uuid[0] = 0x42u;
    configuration.metal_device_registry_id = 0x1234u;
    for (std::size_t ordinal = 0u;
         ordinal < seed->artifacts().size(); ++ordinal) {
        const auto& artifact = seed->artifacts()[ordinal];
        if (artifact.descriptor.kind
            != gv::sealed_shader_cache_artifact_kind::metal_binary_archive) {
            continue;
        }
        const auto* qualification =
            gv::sealed_shader_cache_detail::find_artifact_image(
                seed->artifacts(),
                gv::sealed_shader_cache_artifact_kind::
                    metal_binary_archive_qualification,
                artifact.descriptor.key_index,
                artifact.descriptor.filename + ".qualification");
        REQUIRE(qualification != nullptr);
        const auto parsed = gv::sealed_shader_cache_detail::
            parse_metal_archive_qualification_receipt(qualification->bytes);
        REQUIRE(parsed);
        if (configuration.qualification_receipt_count == 0u) {
            configuration.qualification_wire_revision =
                parsed->wire_revision;
            configuration.helper_wire_revision =
                parsed->helper_wire_revision;
            configuration.helper_build_identity =
                parsed->helper_build_identity;
            configuration.helper_source = parsed->helper_source;
            configuration.helper_executable_size =
                parsed->helper_executable_size;
            configuration.helper_executable_sha256 =
                parsed->helper_executable_sha256;
            configuration.helper_metal_registry_id =
                parsed->helper_metal_registry_id;
        }
        ++configuration.qualification_receipt_count;
        configuration.archives.push_back(gv::runtime_metal_archive_spec{
            .artifact_ordinal = static_cast<std::uint32_t>(ordinal),
            .artifact_filename = artifact.descriptor.filename,
            .size = artifact.descriptor.size,
            .sha256 = artifact.descriptor.sha256,
            .negative_error_domain = parsed->negative_error.domain,
            .negative_error_code = parsed->negative_error.code,
        });
    }
    return configuration;
}

[[nodiscard]] gv::runtime_metal_archive_pipeline_audit
accepted_runtime_metal_archive_audit() {
    const auto configuration = accepted_runtime_metal_archive_configuration();
    const auto archive_identity = gv::make_runtime_metal_archive_set_identity(
        configuration.archives);
    gv::runtime_metal_archive_pipeline_audit audit{};
    audit.receipt_identity = 7u;
    audit.scope_identity = 8u;
    const auto& manifest = configuration.seed_image->manifest();
    const auto& seed_receipt = configuration.seed_image->receipt();
    audit.cache_digest = manifest.cache_digest;
    audit.seed_sha256 = manifest.seed_sha256;
    audit.seed_size = manifest.seed_size;
    audit.manifest_checksum = manifest.manifest_checksum;
    audit.seed_receipt_identity = seed_receipt.identity();
    audit.seed_load_count = seed_receipt.load_count();
    audit.seed_manifest_read_count = seed_receipt.manifest_read_count();
    audit.seed_artifact_read_count = seed_receipt.artifact_read_count();
    audit.seed_manifest_bytes = seed_receipt.manifest_bytes();
    audit.seed_artifact_bytes = seed_receipt.artifact_bytes();
    audit.seed_load_ns = seed_receipt.load_ns();
    audit.seed_admission_count = seed_receipt.admission_count();
    audit.seed_peak_admitted_bytes = seed_receipt.peak_admitted_bytes();
    audit.seed_retained_capacity_bytes =
        seed_receipt.retained_capacity_bytes();
    audit.seed_admission_saturated = seed_receipt.admission_saturated();
    audit.shader_key = gv::make_shader_key(
        gv::shader_kind::chain_tiled_dp_u64, 32u, 32u, 0u);
    audit.expected_entry_name = "main0";
    audit.observed_entry_name = audit.expected_entry_name;
    audit.matched_descriptor_functions = 1u;
    audit.expected_archives = archive_identity;
    audit.loaded_archives = archive_identity;
    audit.archive_pre_post_identity_checks = archive_identity.archive_count;
    audit.pipeline_pre_identity_checks = 0u;
    audit.pipeline_post_identity_checks = 0u;
    audit.command_interpose_installed = true;
    audit.command_interpose_immutable_fail_closed = true;
    audit.configured_vulkan_device_uuid = configuration.vulkan_device_uuid;
    audit.runtime_vulkan_device_uuid = configuration.vulkan_device_uuid;
    audit.api_version = 0x00403000u;
    audit.driver_version = 0x01020304u;
    audit.vendor_id = 0x0000106bu;
    audit.device_id = 0x00000001u;
    audit.pipeline_cache_uuid[0] = 0x42u;
    audit.pipeline_cache_uuid[15] = 0x24u;
    audit.cache_qualification = gv::runtime_metal_archive_process_qualification{
        .wire_revision = 2u,
        .archive_set = archive_identity,
        .receipt_count = configuration.qualification_receipt_count,
        .hash_passes = 1u,
        .hashed_bytes = archive_identity.total_size,
        .private_files_created = archive_identity.archive_count,
        .private_files_unlinked = archive_identity.archive_count,
        .live_private_paths = 0u,
        .pipeline_filesystem_hash_passes = 0u,
        .pipeline_filesystem_hashed_bytes = 0u,
        .helper_wire_revision = configuration.helper_wire_revision,
        .helper_build_identity = "bioseq_gpu_archive_builder",
        .helper_source = "installed-sibling",
        .helper_executable_size = configuration.helper_executable_size,
        .helper_executable_sha256 =
            configuration.helper_executable_sha256,
        .helper_metal_registry_id =
            configuration.helper_metal_registry_id,
        .vulkan_device_uuid = audit.runtime_vulkan_device_uuid,
        .api_version = audit.api_version,
        .driver_version = audit.driver_version,
        .vendor_id = audit.vendor_id,
        .device_id = audit.device_id,
        .pipeline_cache_uuid = audit.pipeline_cache_uuid,
        .seed_receipt_identity = audit.seed_receipt_identity,
        .seed_sha256 = audit.seed_sha256,
        .seed_size = audit.seed_size,
        .manifest_checksum = audit.manifest_checksum,
        .saturated = false,
    };
    audit.cache_qualification.identity =
        gv::runtime_metal_archive_process_qualification_identity(
            audit.cache_qualification);
    audit.pipeline_create_ns = 17u;
    audit.expected_metal_registry_id =
        configuration.metal_device_registry_id;
    audit.command_loader_metal_registry_id =
        configuration.metal_device_registry_id;
    audit.command_source_methods_present = 2u;
    audit.command_source_wrapper_methods = 2u;
    audit.loader_metal_registry_id = configuration.metal_device_registry_id;
    audit.intercepted_metal_registry_id =
        configuration.metal_device_registry_id;
    audit.scoped_interceptions = 1u;
    audit.descriptor_replacements = 1u;
    audit.fail_on_miss_enforcements = 1u;
    audit.same_descriptor_negative_controls = 1u;
    audit.negative_control_misses = 1u;
    audit.expected_negative_error_domain =
        configuration.archives.front().negative_error_domain;
    audit.expected_negative_error_code =
        configuration.archives.front().negative_error_code;
    audit.observed_negative_error_domain =
        audit.expected_negative_error_domain;
    audit.observed_negative_error_code = audit.expected_negative_error_code;
    audit.original_sync_negative_calls = 1u;
    audit.actual_sync_calls = 1u;
    audit.actual_async_calls = 0u;
    audit.outer_vulkan_pipeline_succeeded = true;
    return audit;
}

}  // namespace

TEST_CASE("immutable live gate uses cached scalar configuration result",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    const auto before = gv::capture_runtime_metal_archive_live_gate_result();
    REQUIRE_FALSE(before.installed_accepted);
    for (std::size_t iteration = 0u; iteration < 8u; ++iteration) {
        gv::probe_immutable_metal_policy_live_for_test();
    }
    const auto after = gv::capture_runtime_metal_archive_live_gate_result();
    CHECK(after.install_validation_count == before.install_validation_count);
    CHECK(after.live_gate_calls == before.live_gate_calls + 8u);
    CHECK(after.live_configuration_scan_count == 0u);
}

TEST_CASE("immutable Metal archive configuration binds the exact ordered set",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
    auto& admission =
        bioseq::parallelism::memory_admission_controller::instance();
    auto accepted = accepted_runtime_metal_archive_configuration();
    REQUIRE(accepted.is_valid());
    const std::uint64_t retained_seed_bytes =
        accepted.seed_image->receipt().retained_capacity_bytes();
    const std::uint64_t reserved_with_seed = admission.reserved_bytes();
    REQUIRE(reserved_with_seed >= retained_seed_bytes);
    // The first qualification also process-retains the installed helper. Keep
    // that intentional singleton reservation in the baseline while proving
    // that this configuration's seed and derived image release independently.
    const std::uint64_t process_qualification_baseline =
        reserved_with_seed - retained_seed_bytes;
    const auto identity = gv::make_runtime_metal_archive_set_identity(
        accepted.archives);
    CHECK(identity.is_valid());
    CHECK(identity.archive_count == 2u);
    CHECK(identity.total_size > 0u);
    {
        const auto admitted =
            gv::make_runtime_metal_archive_configuration_image(
                accepted.seed_image,
                accepted.vulkan_device_uuid,
                accepted.metal_device_registry_id);
        REQUIRE(admitted);
        REQUIRE((*admitted)->is_valid());
        CHECK((*admitted)->configuration() == accepted);
        CHECK((*admitted)->retained_capacity_bytes() > 0u);
        CHECK((*admitted)->retained_capacity_bytes()
              <= (*admitted)->peak_admitted_bytes());
    }
    CHECK(admission.reserved_bytes() == reserved_with_seed);

    SECTION("configuration rejects missing immutable identities") {
        auto invalid = accepted;
        invalid.seed_image.reset();
        CHECK_FALSE(invalid.is_valid());

        // Device identities may be unknown to the supervisor before runtime
        // selects VkPhysicalDevice/MTLDevice. Zero is an explicit request for
        // runtime to seal the actual identities; the receipt never accepts
        // zero after that sealing step.
        auto runtime_sealed = accepted;
        runtime_sealed.vulkan_device_uuid = {};
        runtime_sealed.metal_device_registry_id = 0u;
        CHECK(runtime_sealed.is_valid());
    }

    SECTION("archive entries are retained-image-bound and strictly ordered") {
        auto invalid = accepted;
        invalid.archives.clear();
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        invalid.archives[0].artifact_filename += ".other";
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        ++invalid.archives[0].artifact_ordinal;
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        invalid.archives[0].size = 0u;
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        invalid.archives[0].sha256 = {};
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        invalid.archives[0].negative_error_domain.clear();
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        ++invalid.archives[1].negative_error_code;
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        invalid.archives[1].negative_error_domain += ".other";
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        std::swap(invalid.archives[0], invalid.archives[1]);
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        invalid.archives[1] = invalid.archives[0];
        CHECK_FALSE(invalid.is_valid());
    }

    SECTION("helper qualification metadata is exact") {
        auto invalid = accepted;
        invalid.qualification_wire_revision = 1u;
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        --invalid.qualification_receipt_count;
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        invalid.helper_wire_revision = 1u;
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        invalid.helper_build_identity += ".other";
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        invalid.helper_source = "test-env-override";
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        ++invalid.helper_executable_size;
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        invalid.helper_executable_sha256 = {};
        CHECK_FALSE(invalid.is_valid());
        invalid = accepted;
        invalid.helper_metal_registry_id = 0u;
        CHECK_FALSE(invalid.is_valid());
    }

    SECTION("every byte identity component changes the ordered set") {
        auto changed = accepted;
        changed.archives[0].artifact_filename += ".other";
        CHECK(gv::make_runtime_metal_archive_set_identity(changed.archives)
              != identity);
        changed = accepted;
        ++changed.archives[0].artifact_ordinal;
        CHECK(gv::make_runtime_metal_archive_set_identity(changed.archives)
              != identity);
        changed = accepted;
        ++changed.archives[0].size;
        CHECK(gv::make_runtime_metal_archive_set_identity(changed.archives)
              != identity);
        changed = accepted;
        changed.archives[0].sha256[1] = 0x77u;
        CHECK(gv::make_runtime_metal_archive_set_identity(changed.archives)
              != identity);
        changed = accepted;
        changed.archives[0].negative_error_domain += ".other";
        CHECK(gv::make_runtime_metal_archive_set_identity(changed.archives)
              != identity);
        changed = accepted;
        ++changed.archives[0].negative_error_code;
        CHECK(gv::make_runtime_metal_archive_set_identity(changed.archives)
              != identity);
    }
    accepted = {};
    CHECK(admission.reserved_bytes() == process_qualification_baseline);
}

TEST_CASE("immutable Metal archive pipeline receipt is strict and retained",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
    STATIC_REQUIRE(!std::is_copy_constructible_v<
                   gv::runtime_metal_archive_pipeline_receipt>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<
                   gv::runtime_metal_archive_pipeline_receipt>);

    const auto accepted = accepted_runtime_metal_archive_audit();
    REQUIRE(accepted.is_internally_consistent());
    CHECK(gv::runtime_metal_archive_pipeline_receipt::
          self_test_move_and_reuse_contract(accepted));

    SECTION("exact expected and loaded archive sets must agree") {
        auto invalid = accepted;
        ++invalid.loaded_archives.archive_count;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        ++invalid.loaded_archives.total_size;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        ++invalid.loaded_archives.ordered_checksum;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        --invalid.archive_pre_post_identity_checks;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        --invalid.pipeline_pre_identity_checks;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        --invalid.pipeline_post_identity_checks;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.unsealed_metal_source_requests = 1u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.metal_source_fallback_calls = 1u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.manifest_checksum = 0u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.cache_digest.clear();
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.cache_digest = "0000000000000000";
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.cache_digest[0] = 'A';
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.seed_sha256 = {};
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.seed_size = 0u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.seed_receipt_identity = 0u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.seed_load_count = 0u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.seed_manifest_read_count = 0u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.seed_artifact_read_count = 0u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        ++invalid.seed_artifact_bytes;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.seed_load_ns = 0u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.seed_admission_count = 0u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.seed_retained_capacity_bytes =
            invalid.seed_peak_admitted_bytes + 1u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.seed_admission_saturated = true;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.shader_key = {};
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.observed_entry_name += ".other";
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.matched_descriptor_functions = 0u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.command_interpose_installed = false;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.command_interpose_immutable_fail_closed = false;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        ++invalid.command_loader_metal_registry_id;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        --invalid.command_source_methods_present;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        --invalid.command_source_wrapper_methods;
        CHECK_FALSE(invalid.is_internally_consistent());
    }

    SECTION("Vulkan loader and intercepted Metal device identities are exact") {
        auto invalid = accepted;
        invalid.runtime_vulkan_device_uuid[1] = 0x44u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.api_version = 0u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.driver_version = 0u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.pipeline_cache_uuid = {};
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.pipeline_create_ns = 0u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        ++invalid.loader_metal_registry_id;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        ++invalid.intercepted_metal_registry_id;
        CHECK_FALSE(invalid.is_internally_consistent());
    }

    SECTION("every scoped call replaces archives enforces miss and runs control") {
        auto invalid = accepted;
        --invalid.descriptor_replacements;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        --invalid.fail_on_miss_enforcements;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        --invalid.same_descriptor_negative_controls;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        --invalid.negative_control_misses;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        --invalid.original_sync_negative_calls;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        ++invalid.actual_async_calls;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        ++invalid.scoped_interceptions;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.observed_negative_error_domain += ".other";
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        ++invalid.observed_negative_error_code;
        CHECK_FALSE(invalid.is_internally_consistent());
    }

    SECTION("outside nested sticky and failed outer execution fail closed") {
        auto invalid = accepted;
        invalid.outside_scope_interceptions = 1u;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.nested_scope = true;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.sticky_failure = true;
        CHECK_FALSE(invalid.is_internally_consistent());
        invalid = accepted;
        invalid.outer_vulkan_pipeline_succeeded = false;
        CHECK_FALSE(invalid.is_internally_consistent());
    }

    SECTION("process qualification equations reject every filesystem shortcut") {
        const auto reject = [&accepted](auto mutate) {
            auto invalid = accepted;
            mutate(invalid.cache_qualification);
            invalid.cache_qualification.identity =
                gv::runtime_metal_archive_process_qualification_identity(
                    invalid.cache_qualification);
            CHECK_FALSE(invalid.is_internally_consistent());
        };
        reject([](auto& value) { value.hash_passes = 2u; });
        reject([](auto& value) { ++value.hashed_bytes; });
        reject([](auto& value) { --value.private_files_created; });
        reject([](auto& value) { --value.private_files_unlinked; });
        reject([](auto& value) { value.live_private_paths = 1u; });
        reject([](auto& value) {
            value.pipeline_filesystem_hash_passes = 1u;
        });
        reject([](auto& value) {
            value.pipeline_filesystem_hashed_bytes = 1u;
        });
        reject([](auto& value) { value.helper_source = "test-env-override"; });
        reject([](auto& value) { value.helper_metal_registry_id = 0u; });
        reject([](auto& value) { value.vulkan_device_uuid[0] ^= 0x80u; });
        reject([](auto& value) { ++value.api_version; });
        reject([](auto& value) { ++value.driver_version; });
        reject([](auto& value) { ++value.vendor_id; });
        reject([](auto& value) { ++value.device_id; });
        reject([](auto& value) { value.pipeline_cache_uuid[0] ^= 0x80u; });
        reject([](auto& value) { value.seed_sha256[0] ^= 0x80u; });
        reject([](auto& value) { ++value.seed_size; });
        reject([](auto& value) { ++value.manifest_checksum; });

        auto invalid_identity = accepted;
        invalid_identity.cache_qualification.identity = 0u;
        CHECK_FALSE(invalid_identity.is_internally_consistent());
    }
}

TEST_CASE("runtime pipeline PREPARED and USED receipts are causal and one-shot",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:filesystem]") {
    STATIC_REQUIRE(!std::is_copy_constructible_v<
                   gv::runtime_pipeline_operation_token>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<
                   gv::runtime_pipeline_operation_token>);
    STATIC_REQUIRE(!std::is_copy_constructible_v<
                   gv::persisted_grouped_soa_coordinate_accepted_operation>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<
                   gv::persisted_grouped_soa_coordinate_accepted_operation>);
    STATIC_REQUIRE(!std::is_copy_constructible_v<
                   gv::runtime_gpu_cache_metrics_delta>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<
                   gv::runtime_gpu_cache_metrics_delta>);

    auto& admission =
        bioseq::parallelism::memory_admission_controller::instance();
    const auto base_audit = accepted_runtime_metal_archive_audit();
    REQUIRE(base_audit.is_internally_consistent());
    // Constructing the first audit also once-qualifies and process-retains the
    // installed helper executable. That reservation intentionally lives until
    // process shutdown, so leak checks begin after this process-wide baseline.
    const std::uint64_t reserved_after_process_qualification =
        admission.reserved_bytes();
    CHECK(gv::runtime_pipeline_operation_token::
          self_test_causal_collector_contract(
              base_audit));
    CHECK(admission.reserved_bytes()
          == reserved_after_process_qualification);
    CHECK(gv::runtime::
          self_test_persisted_grouped_soa_finalizer_preclaim_contract(
              base_audit));
    CHECK(admission.reserved_bytes()
          == reserved_after_process_qualification);
    CHECK(gv::runtime::self_test_gpu_cache_partial_delta_contract(
        base_audit));
    CHECK(admission.reserved_bytes()
          == reserved_after_process_qualification);
}

TEST_CASE("runtime GPU environment identity is canonical and byte exact",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    gv::vulkan_feature_set features{};
    features.api_version = 0x01020304u;
    features.driver_version = 0xa0b0c0d0u;
    features.vendor_id = 0x0000106bu;
    features.device_id = 0x0000002au;
    for (std::size_t i = 0u; i < features.pipeline_cache_uuid.size(); ++i) {
        features.pipeline_cache_uuid[i] = static_cast<std::uint8_t>(i);
    }
    const auto identity = gv::canonical_vulkan_driver_identity(features);
    CHECK(std::string(identity.data())
          == "api=01020304;driver=a0b0c0d0;vendor=0000106b;"
             "device=0000002a;pcuuid=000102030405060708090a0b0c0d0e0f");
    const auto terminator = std::find(
        identity.begin(), identity.end(), '\0');
    REQUIRE(terminator != identity.end());
    CHECK(std::all_of(
        std::next(terminator), identity.end(),
        [](char byte) { return byte == '\0'; }));

    auto changed = features;
    ++changed.api_version;
    CHECK(gv::canonical_vulkan_driver_identity(changed) != identity);
    changed = features;
    ++changed.driver_version;
    CHECK(gv::canonical_vulkan_driver_identity(changed) != identity);
    changed = features;
    ++changed.vendor_id;
    CHECK(gv::canonical_vulkan_driver_identity(changed) != identity);
    changed = features;
    ++changed.device_id;
    CHECK(gv::canonical_vulkan_driver_identity(changed) != identity);
    changed = features;
    std::swap(changed.pipeline_cache_uuid[0],
              changed.pipeline_cache_uuid[1]);
    CHECK(gv::canonical_vulkan_driver_identity(changed) != identity);
}

TEST_CASE("runtime GPU cache delta is bounded by its pre-finalizer maximum",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    const gv::runtime_gpu_cache_metrics_audit maximum{
        .token_identity = 77u,
        .gpu_pipeline_cache_loads = 1u,
        .gpu_pipeline_cache_seed_bytes = 4096u,
        .gpu_pipeline_cache_load_ns = 11u,
        .gpu_pipeline_creates = 8u,
        .gpu_pipeline_creation_ns = 88u,
    };
    const gv::runtime_gpu_cache_metrics_audit first = maximum;
    const gv::runtime_gpu_cache_metrics_audit later{
        .token_identity = 77u,
    };
    REQUIRE(maximum.is_internally_consistent());
    CHECK(first.is_componentwise_bounded_by(maximum));
    CHECK(later.is_componentwise_bounded_by(maximum));
    auto invalid = later;
    invalid.token_identity = 78u;
    CHECK_FALSE(invalid.is_componentwise_bounded_by(maximum));
    invalid = first;
    ++invalid.gpu_pipeline_creation_ns;
    CHECK_FALSE(invalid.is_componentwise_bounded_by(maximum));
    invalid = first;
    invalid.saturated = true;
    CHECK_FALSE(invalid.is_componentwise_bounded_by(maximum));
}

TEST_CASE("grouped-SoA radix pass count is exact at byte boundaries",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    const auto passes = [](std::uint64_t target_end) {
        return gv::runtime_grouped_soa_radix_pass_count(
            0u, target_end + 1u);
    };
    CHECK(passes(0u) == 1u);                      // 1 coord + strand
    CHECK(passes(0x7fu) == 1u);                   // 7 coord + strand
    CHECK(passes(0x80u) == 2u);                   // 8 coord + strand
    CHECK(passes(0xffu) == 2u);                   // 8 coord + strand
    CHECK(passes(0x100u) == 2u);                  // 9 coord + strand
    CHECK(passes(0x7fffu) == 2u);                 // 15 coord + strand
    CHECK(passes(0x8000u) == 3u);                 // 16 coord + strand
    CHECK(passes(0xffffu) == 3u);                 // 16 coord + strand
    CHECK(passes(0x10000u) == 3u);                // 17 coord + strand
    CHECK(passes(9'999'999u) == 4u);              // FAST_LONG: 24 + strand
    CHECK(passes(499'999'999u) == 4u);            // XXL_HIFI: 29 + strand
    CHECK(passes(std::uint64_t{1} << 62u) == 8u); // 63 coord + strand
    CHECK_FALSE(gv::runtime_grouped_soa_radix_pass_count(0u, 0u));
    CHECK_FALSE(gv::runtime_grouped_soa_radix_pass_count(
        0u, (std::uint64_t{1} << 63u) + 1u));
    CHECK_FALSE(gv::runtime_grouped_soa_radix_pass_count(
        (std::numeric_limits<std::uint64_t>::max)(), 2u));
}
