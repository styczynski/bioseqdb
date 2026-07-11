#include "common_tests.h"
#include "common.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>
#include <vulkan/vulkan.h>

import bioseq.gpu.vulkan.shader_cache;
import bioseq.gpu.vulkan.shader_compiler;
import bioseq.gpu.vulkan.shader_variants;

namespace {

namespace gv = bioseq::gpu::vulkan;

bool shader_module_create_called = false;

VKAPI_ATTR VkResult VKAPI_CALL record_shader_module_create(
    VkDevice,
    const VkShaderModuleCreateInfo*,
    const VkAllocationCallbacks*,
    VkShaderModule*)
{
    shader_module_create_called = true;
    return VK_SUCCESS;
}

struct NoPublishCompileService {
    int calls = 0;

    [[nodiscard]] std::expected<void, std::string> ensure_variant_compiled(
        gv::ShaderVariantKey)
    {
        ++calls;
        return {};
    }
};

struct TempDir {
    std::filesystem::path path;

    TempDir()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path()
             / ("bioseq_shader_cache_test_" + std::to_string(::getpid())
                + "_" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

[[nodiscard]] bool contains_key(
    const std::vector<gv::ShaderVariantKey>& keys,
    gv::ShaderVariantKey needle)
{
    return std::find(keys.begin(), keys.end(), needle) != keys.end();
}

[[nodiscard]] bool is_hex_digest(std::string_view digest)
{
    return digest.size() == 16
        && std::all_of(digest.begin(), digest.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

[[nodiscard]] std::vector<std::byte> bytes_of(std::string_view text)
{
    std::vector<std::byte> out(text.size());
    std::memcpy(out.data(), text.data(), text.size());
    return out;
}

}  // namespace

TEST_CASE("DOC0341 shader variants enumerate the runtime AOT set",
          "[shader_cache][doc0341]") {
    gv::VulkanStaticVariantInputs inputs{
        .max_workgroup_invocations = 256,
        .max_shared_memory_bytes = 32u * 1024u,
        .default_window_size = 10,
    };

    std::vector<gv::ShaderVariantKey> keys;
    gv::for_each_static_shader_variant(inputs, [&](gv::ShaderVariantKey key) {
        INFO("variant stem: " << gv::cache_file_stem(key));
        CHECK(gv::valid_shader_variant_key(key));
        keys.push_back(key);
    });

    REQUIRE(keys.size() == 36);
    auto sorted = keys;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return gv::cache_file_stem(a) < gv::cache_file_stem(b);
    });
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());

    CHECK(gv::choose_chaining_variant(inputs) == gv::ChainingPipelineKey{32u, 32u});
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::chain_tiled_dp_u64, 32u, 32u, 0u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::seed_lookup_anchors, 384u, 128u, 256u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::phase_c_sketch_tile, 384u, 64u, 10u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::phase_c_coordinate_vote_reduce, 384u, 128u, 1u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::phase_c_coordinate_vote_reduce_batch, 384u, 128u, 1u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::phase_c_persisted_directory_select, 384u, 128u, 1u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::align_extend_banded_affine, 384u, 1u, 512u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::align_extend_simple_cigar, 384u, 1u, 512u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::align_extend_simple_cigar, 384u, 64u, 512u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::generate_units, 384u, 256u, 64u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::seq_sort_extract_key_dna4, 384u, 256u, 0u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::seq_sort_tiebreak_generic, 384u, 256u, 0u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::seq_sort_global_compare_dna4, 384u, 256u, 0u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::seq_sort_dense_scatter_dna4, 384u, 256u, 0u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::seq_sort_prefix_check_dna4, 384u, 256u, 0u)));

    CHECK_FALSE(gv::valid_shader_variant_key(gv::make_shader_key(
        gv::ShaderKind::chain_tiled_dp_u64, 384u, 256u, 0u)));
    CHECK_FALSE(gv::valid_shader_variant_key(gv::make_shader_key(
        gv::ShaderKind::generate_units, 384u, 256u, 128u)));
}

TEST_CASE("DOC0371 persisted directory selector shader compiles with exact bindings",
          "[shader_cache][doc0371][persisted-directory]") {
    const auto key = gv::make_shader_key(
        gv::ShaderKind::phase_c_persisted_directory_select,
        384u, 128u, 1u);
    REQUIRE(gv::valid_shader_variant_key(key));
    const auto spirv = gv::compile_shader_spirv(key.to_request());
    REQUIRE_FALSE(spirv.empty());
    const auto layout = gv::validate_spirv_layout(key, spirv);
    REQUIRE(layout.has_value());
}

TEST_CASE("score-only affine extension has one provider-compatible AOT variant",
          "[shader_cache][alignment-extension][no-fallback]") {
    const auto key = gv::make_shader_key(
        gv::ShaderKind::align_extend_banded_affine, 384u, 1u, 512u);
    REQUIRE(gv::valid_shader_variant_key(key));
    REQUIRE(gv::checked_shader_kind(key.kind)
            == gv::ShaderKind::align_extend_banded_affine);
    REQUIRE(gv::expected_shader_workgroup_size(key)
            == (gv::ShaderWorkgroupSize{1u, 1u, 1u}));
    const auto spirv = gv::compile_shader_spirv(key.to_request());
    REQUIRE_FALSE(spirv.empty());
    REQUIRE(gv::validate_spirv_layout(key, spirv));
    const auto msl = gv::spirv_to_msl(spirv, key, gv::MslVersion{});
    REQUIRE(msl);
    CHECK(msl->wg_x == 1u);
    CHECK(msl->wg_y == 1u);
    CHECK(msl->wg_z == 1u);
}

TEST_CASE("fixed radix workgroups have one authoritative cache ABI",
          "[shader_cache][workgroup-abi]") {
    const std::array kinds{
        gv::ShaderKind::phase_c_sort_radix_histogram,
        gv::ShaderKind::phase_c_sort_radix_scan,
        gv::ShaderKind::phase_c_sort_radix_scatter,
    };
    for (const gv::ShaderKind kind : kinds) {
        const gv::ShaderVariantKey key =
            gv::make_shader_key(kind, 384u, 1u, 1u);
        CAPTURE(gv::cache_file_stem(key));
        REQUIRE(gv::valid_shader_variant_key(key));

        // The established key uses local_size=1 as a selector, while the
        // algorithm has one invocation per radix bin. Reflection, validation,
        // and archive selection must agree on the compiled 256-lane geometry.
        const gv::ShaderWorkgroupSize expected =
            gv::expected_shader_workgroup_size(key);
        REQUIRE(expected == (gv::ShaderWorkgroupSize{256u, 1u, 1u}));

        const auto spirv = gv::compile_shader_spirv(key.to_request());
        REQUIRE_FALSE(spirv.empty());
        const auto msl = gv::spirv_to_msl(spirv, key, gv::MslVersion{});
        REQUIRE(msl);
        CHECK(msl->wg_x == expected.x);
        CHECK(msl->wg_y == expected.y);
        CHECK(msl->wg_z == expected.z);
    }
}

TEST_CASE("DOC0341 shader cache digest changes with variant ABI inputs",
          "[shader_cache][doc0341]") {
    gv::ShaderCacheBuildInputs inputs{};
    inputs.variants = gv::VulkanStaticVariantInputs{
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

TEST_CASE("DOC0341 shader cache meta and MoltenVK header payload are exact",
          "[shader_cache][doc0341]") {
    TempDir temp;
    const gv::ShaderVariantKey key = gv::make_shader_key(
        gv::ShaderKind::generate_units, 384u, 256u, 64u);
    constexpr std::string_view entry = "main0";

    gv::ShaderCacheMeta meta{};
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
    REQUIRE(gv::cache_entry_complete(temp.path, key));

    std::string decoded_entry;
    const auto decoded_meta = gv::read_meta(
        gv::cache_meta_path(temp.path, key), &decoded_entry);
    REQUIRE(decoded_meta);
    CHECK(decoded_meta->key == key);
    CHECK(decoded_meta->wg_x == 256u);
    CHECK(decoded_entry == entry);

    // A cache entry with the right key but a different compiled workgroup ABI
    // is not reusable.  This is the local compatibility check complementing
    // the digest-level resource-binding ABI revision.
    gv::ShaderCacheMeta incompatible_meta = meta;
    incompatible_meta.wg_x = 128u;
    const std::vector<std::byte> incompatible_meta_bytes =
        gv::meta_to_bytes(incompatible_meta, entry);
    auto incompatible_meta_write = gv::durable_write_file(
        gv::cache_meta_path(temp.path, key),
        std::span<const std::byte>(
            incompatible_meta_bytes.data(), incompatible_meta_bytes.size()));
    REQUIRE(incompatible_meta_write);
    CHECK_FALSE(gv::cache_metallib_entry_complete(temp.path, key));
    CHECK_FALSE(gv::cache_entry_complete(temp.path, key));

    shader_module_create_called = false;
    const auto loader_rejected = gv::load_cached_module(
        VK_NULL_HANDLE, temp.path, key, &record_shader_module_create);
    REQUIRE_FALSE(loader_rejected);
    CHECK(loader_rejected.error().find("meta workgroup mismatch")
          != std::string::npos);
    CHECK_FALSE(shader_module_create_called);

    NoPublishCompileService no_publish_service{};
    const auto post_compile_rejected = gv::load_module(
        VK_NULL_HANDLE,
        temp.path,
        key,
        &record_shader_module_create,
        no_publish_service);
    REQUIRE_FALSE(post_compile_rejected);
    CHECK(post_compile_rejected.error().find("complete compatible cache entry")
          != std::string::npos);
    CHECK(post_compile_rejected.error().find(
              "metadata incompatible: meta workgroup mismatch")
          != std::string::npos);
    CHECK(no_publish_service.calls == 1);
    CHECK_FALSE(shader_module_create_called);

    const auto module_code = gv::make_moltenvk_metallib_module_code(
        gv::cache_metallib_path(temp.path, key));
    REQUIRE(module_code);
    CHECK(module_code->header_size_bytes == gv::moltenvk_msl_spirv_header_size());
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

TEST_CASE("fixed radix cache accepts reflected workgroup and exact archive",
          "[shader_cache][workgroup-abi]") {
    TempDir temp;
    const gv::ShaderVariantKey key = gv::make_shader_key(
        gv::ShaderKind::phase_c_sort_radix_histogram, 384u, 1u, 1u);
    constexpr std::string_view entry = "main0";

    gv::ShaderCacheMeta meta{};
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

    // Publication uses reflected threads, so a 256-thread shader needs only
    // the exact base archive; completeness must not invent a duplicate
    // `.max_threads_256.metalar` from the key's sentinel local_size.
    CHECK(gv::cache_binary_archive_paths(temp.path, key).size() == 1u);
    REQUIRE(gv::cache_entry_status(temp.path, key));
    REQUIRE(gv::cache_entry_complete(temp.path, key));

    shader_module_create_called = false;
    NoPublishCompileService should_not_compile{};
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
