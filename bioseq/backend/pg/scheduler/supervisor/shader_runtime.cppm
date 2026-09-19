/**
 * @file scheduler/supervisor/shader_runtime.cppm
 * @brief PostgreSQL Vulkan shader-cache and runtime supervision implementation.
 */
module;
#include <bioseq/pg_compat.h>

extern "C" {
#include "storage/condition_variable.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"

#include "miscadmin.h"
#include "port/atomics.h"
}

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <vulkan/vulkan.h>

module bioseq.postgres.scheduler.supervisor;
import bioseq.common.logging;
import bioseq.postgres.configuration.aligner;
import bioseq.postgres.configuration.scheduler;
import bioseq.instrumentation.clock;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.gpu.vulkan.runtime;
import bioseq.gpu.vulkan.shader_cache;
import bioseq.gpu.vulkan.shader_variants;
import bioseq.postgres.support;

namespace bioseq::postgres::scheduler {

static_assert(k_shader_cache_ok_wire_value ==
              static_cast<std::uint32_t>(bioseq::gpu::vulkan::shader_cache_error::ok));

/**
 * @brief Maps a shader-cache diagnostic to its shared wire error.
 * @param[in] error Current cache or compiler diagnostic.
 * @return Stable shader-cache error category.
 */
[[nodiscard]] bioseq::gpu::vulkan::shader_cache_error
classify_shader_cache_error(std::string_view error) noexcept {
    using bioseq::gpu::vulkan::shader_cache_error;
    if (error.find("metal unavailable") != std::string_view::npos ||
        error.find("No such file") != std::string_view::npos) {
        return shader_cache_error::no_toolchain;
    }
    if (error.find("glslang") != std::string_view::npos ||
        error.find("shader compile") != std::string_view::npos) {
        return shader_cache_error::glslang;
    }
    if (error.find("SPIR-V to MSL") != std::string_view::npos) {
        return shader_cache_error::spirv_to_msl;
    }
    if (error.find("spawn failed") != std::string_view::npos) {
        return shader_cache_error::metal_spawn;
    }
    if (error.find("exited with status") != std::string_view::npos ||
        error.find("terminated by signal") != std::string_view::npos) {
        return shader_cache_error::metal_exit;
    }
    if (error.find("ABI") != std::string_view::npos ||
        error.find("layout mismatch") != std::string_view::npos ||
        error.find("stride") != std::string_view::npos) {
        return shader_cache_error::abi_scan;
    }
    return shader_cache_error::metadata_io;
}

/**
 * @brief Returns the PostgreSQL-owned runtime cache root.
 * @return A path below `DataDir`, or the process directory when it is unavailable.
 */
[[nodiscard]] std::filesystem::path runtime_cache_root() {
    const char* data_dir = DataDir != nullptr ? DataDir : ".";
    return std::filesystem::path(data_dir) / "bioseq_runtime_cache";
}

/**
 * @brief Resolves the cache directory for one shader-input digest.
 * @param[in] digest Canonical shader-cache identity.
 * @return Digest-specific cache directory.
 */
[[nodiscard]] std::filesystem::path runtime_cache_dir_from_digest(std::string_view digest) {
    return runtime_cache_root() / std::string(digest);
}

/**
 * @brief Publishes validated cache paths before Vulkan runtime construction.
 * @param[in] cache_dir Directory containing the sealed shader cache.
 * @return Success or the first environment update diagnostic.
 */
[[nodiscard]] std::expected<void, std::string>
publish_runtime_cache_env(const std::filesystem::path& cache_dir) {
    // Exact command bytes are carried only by the admitted sealed seed/configuration.
    // A pathname environment override is never authority in immutable mode.
    if (::unsetenv("BIOSEQ_MOLTENVK_COMMAND_METALLIB") != 0) {
        const int saved_errno = errno;
        return std::unexpected(
            fmt::format("unsetenv BIOSEQ_MOLTENVK_COMMAND_METALLIB failed: errno {} ({})",
                        saved_errno,
                        std::strerror(saved_errno)));
    }
    if (::setenv("BIOSEQ_MOLTENVK_BINARY_ARCHIVE_DIR", cache_dir.c_str(), 1) != 0) {
        const int saved_errno = errno;
        return std::unexpected(
            fmt::format("setenv BIOSEQ_MOLTENVK_BINARY_ARCHIVE_DIR failed: errno {} ({})",
                        saved_errno,
                        std::strerror(saved_errno)));
    }
    const std::filesystem::path sealed_manifest =
        bioseq::gpu::vulkan::sealed_shader_cache_manifest_path(cache_dir);
    if (::setenv("BIOSEQ_GPU_SEALED_CACHE_MANIFEST", sealed_manifest.c_str(), 1) != 0) {
        const int saved_errno = errno;
        return std::unexpected(
            fmt::format("setenv BIOSEQ_GPU_SEALED_CACHE_MANIFEST failed: errno {} ({})",
                        saved_errno,
                        std::strerror(saved_errno)));
    }
    return {};
}

/**
 * @brief Copies the published cache digest from scheduler shared memory.
 * @return The current digest, or an empty string when it is unavailable.
 */
[[nodiscard]] std::string current_runtime_cache_digest() {
    if (g_shared == nullptr) {
        return {};
    }
    // Mirror the shared C buffer so readers never observe a partially terminated string.
    char local[sizeof(g_shared->cache_digest_hex)]{};
    std::memcpy(local, g_shared->cache_digest_hex, sizeof(local));
    local[sizeof(local) - 1] = '\0';
    return std::string(local);
}

/**
 * @brief Builds the device-independent shader inputs used before runtime probing.
 * @return Conservative limits and stable toolchain identity.
 */
[[nodiscard]] bioseq::gpu::vulkan::shader_cache_build_inputs conservative_shader_inputs() {
    bioseq::gpu::vulkan::shader_cache_build_inputs inputs{};
    inputs.variants.max_workgroup_invocations = 256u;
    inputs.variants.max_shared_memory_bytes = 32u * 1024u;
    inputs.msl = bioseq::gpu::vulkan::msl_version{3u, 2u, 0u};
    inputs.moltenvk_build = "sealed-exact-command-source-options-v1";
    inputs.device_uuid = "conservative-no-device-probe";
    return inputs;
}

/**
 * @brief Computes the platform cache digest before selecting a sealed-cache directory.
 * @param[in] inputs Stable shader-cache build inputs.
 * @return Base digest off Apple, or the digest bound to the once-qualified helper on Apple.
 */
[[nodiscard]] std::expected<std::string, std::string> runtime_shader_cache_digest(
    const bioseq::gpu::vulkan::shader_cache_build_inputs& inputs) {
#if !defined(__APPLE__)
    return bioseq::gpu::vulkan::shader_cache_digest_hex(inputs);
#else
    // Sealing and receipt validation call load_once() again, but the process store
    // returns this retained image without another qualification or filesystem hash.
    auto helper = bioseq::gpu::vulkan::qualified_metal_archive_helper_process_store_instance()
                      .load_once();
    if (!helper) {
        return std::unexpected(
            fmt::format("qualify archive helper before cache selection: {}", helper.error()));
    }
    return bioseq::gpu::vulkan::shader_cache_digest_hex_with_qualified_helper(inputs, **helper);
#endif
}

/**
 * @brief Enumerates and canonicalizes every static shader variant.
 * @param[in] inputs Limits and toolchain identity defining the variant set.
 * @return Canonical variant keys, or a validation diagnostic.
 */
[[nodiscard]] std::expected<std::vector<bioseq::gpu::vulkan::shader_variant_key>, std::string>
exact_aot_shader_keys(const bioseq::gpu::vulkan::shader_cache_build_inputs& inputs) {
    std::vector<bioseq::gpu::vulkan::shader_variant_key> keys;
    bioseq::gpu::vulkan::for_each_static_shader_variant(
        inputs.variants,
        [&keys](const bioseq::gpu::vulkan::shader_variant_key& key) { keys.push_back(key); });
    return bioseq::gpu::vulkan::canonical_sealed_shader_cache_keys(keys);
}

/**
 * @brief Returns the process-lifetime sealed shader seed store.
 * @return Process-local store owning the accepted seed image.
 */
[[nodiscard]] bioseq::gpu::vulkan::sealed_shader_cache_process_store&
process_shader_seed_store() noexcept {
    static bioseq::gpu::vulkan::sealed_shader_cache_process_store store;
    return store;
}

/**
 * @brief Loads one validated seed identity into the process store.
 * @param[in] cache_dir Directory containing the sealed artifacts.
 * @param[in] digest Expected cache digest.
 * @param[in] keys Expected canonical variant order.
 * @return The accepted process seed, or a validation diagnostic.
 */
[[nodiscard]] std::expected<
    std::shared_ptr<const bioseq::gpu::vulkan::sealed_shader_cache_seed_image>,
    std::string>
load_process_shader_seed_once(const std::filesystem::path& cache_dir,
                              std::string_view digest,
                              std::span<const bioseq::gpu::vulkan::shader_variant_key> keys) {
    return process_shader_seed_store().load_once(cache_dir, digest, keys);
}

/**
 * @brief Samples the accepted process shader seed.
 * @return Shared ownership of the current seed, or null before preparation.
 */
[[nodiscard]] std::shared_ptr<const bioseq::gpu::vulkan::sealed_shader_cache_seed_image>
process_shader_seed_snapshot() {
    return process_shader_seed_store().snapshot();
}

/**
 * @brief Installs archive configuration derived from one accepted seed.
 * @param[in] seed Accepted sealed shader seed.
 * @return Success or an archive configuration diagnostic.
 */
[[nodiscard]] std::expected<void, std::string> install_verified_runtime_archive_configuration(
    std::shared_ptr<const bioseq::gpu::vulkan::sealed_shader_cache_seed_image> seed) {
    if (!seed || !seed->is_accepted()) {
        return std::unexpected("runtime archive configuration has no accepted process seed image");
    }
    auto configuration =
        bioseq::gpu::vulkan::make_runtime_metal_archive_configuration_image(std::move(seed));
    if (!configuration) {
        return std::unexpected(
            fmt::format("prepare admitted Metal archive configuration: {}", configuration.error()));
    }
    auto installed =
        bioseq::gpu::vulkan::install_runtime_metal_archive_configuration(std::move(*configuration));
    if (!installed) {
        return std::unexpected(
            fmt::format("install sealed Metal archive configuration: {}", installed.error()));
    }
    return {};
}

/**
 * @brief Creates or validates the supervisor-owned exact shader seed.
 * @return Success, or the first cache, compiler, or sealing diagnostic.
 */
[[nodiscard]] std::expected<void, std::string> run_shader_cache_aot_phase() {
#if !defined(__APPLE__)
    return {};
#else
    const auto inputs = conservative_shader_inputs();
    auto selected_digest = runtime_shader_cache_digest(inputs);
    if (!selected_digest) {
        return std::unexpected(selected_digest.error());
    }
    const std::string& digest = *selected_digest;
    const std::filesystem::path cache_dir = runtime_cache_dir_from_digest(digest);
    std::error_code filesystem_error;
    std::filesystem::create_directories(cache_dir, filesystem_error);
    if (filesystem_error) {
        return std::unexpected(fmt::format("create shader cache dir {} failed: {}",
                                           cache_dir.string(),
                                           filesystem_error.message()));
    }
    auto keys = exact_aot_shader_keys(inputs);
    if (!keys) {
        return std::unexpected(fmt::format("canonicalize exact AOT shader keys: {}", keys.error()));
    }
    const std::filesystem::path manifest_path =
        bioseq::gpu::vulkan::sealed_shader_cache_manifest_path(cache_dir);
    const bool manifest_exists = std::filesystem::exists(manifest_path, filesystem_error);
    if (filesystem_error) {
        return std::unexpected(fmt::format("stat sealed shader cache manifest {} failed: {}",
                                           manifest_path.string(),
                                           filesystem_error.message()));
    }
    if (manifest_exists) {
        auto sealed = bioseq::gpu::vulkan::load_validated_sealed_shader_cache_manifest(
            cache_dir, digest, *keys);
        if (!sealed) {
            return std::unexpected(
                fmt::format("existing sealed shader cache is invalid and will not be repaired: {}",
                            sealed.error()));
        }
        if (g_shared != nullptr) {
            std::snprintf(g_shared->cache_digest_hex,
                          sizeof(g_shared->cache_digest_hex),
                          "%s",
                          digest.c_str());
        }
        return {};
    }

    bioseq::parallelism::global_pool::instance().configure(bioseq::parallelism::pool_config{
        .maximum_concurrency = effective_max_concurrency(static_cast<unsigned>(
            std::max(0, bioseq::scheduler::settings::backend::max_concurrency))),
        .mode = bioseq::parallelism::pipeline_concurrency_mode_from_int(
            static_cast<int>(bioseq::scheduler::settings::pipeline::mode.get_value())),
    });

    constexpr std::uint64_t k_peak_compile_bytes = 256ull << 20;
    std::size_t compile_cursor = 0;
    while (compile_cursor < keys->size()) {
        const unsigned requested =
            static_cast<unsigned>(std::min<std::size_t>(4, keys->size() - compile_cursor));
        auto wave = bioseq::parallelism::try_reserve_budgeted_wave(requested, k_peak_compile_bytes);
        if (!wave.is_admitted) {
            return std::unexpected("shader cache AOT memory admission failed");
        }
        const std::size_t wave_begin = compile_cursor;
        const std::size_t wave_end =
            std::min<std::size_t>(keys->size(), compile_cursor + wave.fanout);
        std::mutex error_mu;
        std::string first_error;
        bioseq::parallelism::global_pool::instance().parallel_for(
            wave_begin,
            wave_end,
            1,
            [&cache_dir, &error_mu, &first_error, &inputs, &keys](const std::size_t range_begin,
                                                                  const std::size_t range_end) {
                for (std::size_t key_index = range_begin; key_index < range_end; ++key_index) {
                    auto compiled = bioseq::gpu::vulkan::ensure_compiled_for_sealed_seed(
                        cache_dir, (*keys)[key_index], inputs.msl);
                    if (!compiled) {
                        std::lock_guard<std::mutex> error_lock(error_mu);
                        if (first_error.empty()) {
                            first_error = fmt::format(
                                "{}: {}",
                                bioseq::gpu::vulkan::cache_file_stem((*keys)[key_index]),
                                compiled.error());
                        }
                    }
                }
            });
        if (!first_error.empty()) {
            return std::unexpected(first_error);
        }
        compile_cursor = wave_end;
    }
    if (auto command_library =
            bioseq::gpu::vulkan::ensure_moltenvk_command_library_for_sealed_seed(cache_dir);
        !command_library) {
        return std::unexpected(
            fmt::format("seal exact MoltenVK command library: {}", command_library.error()));
    }
    auto sealed = bioseq::gpu::vulkan::seal_shader_cache_manifest(cache_dir, digest, *keys);
    if (!sealed) {
        return std::unexpected(fmt::format("seal exact shader AOT seed: {}", sealed.error()));
    }
    if (g_shared != nullptr) {
        std::snprintf(
            g_shared->cache_digest_hex, sizeof(g_shared->cache_digest_hex), "%s", digest.c_str());
    }
    return {};
#endif
}

/**
 * @brief Initializes runtime assets owned by the scheduler supervisor.
 * @return Success or a diagnostic safe for shared publication.
 */
[[nodiscard]] std::expected<void, std::string> load_bioseq_runtime() {
    return run_shader_cache_aot_phase();
}

/**
 * @brief Publishes terminal runtime initialization state and wakes every waiter.
 * @param[in] initialization_result Runtime initialization outcome.
 */
void publish_runtime_init_result(std::expected<void, std::string> initialization_result) noexcept {
    if (g_shared == nullptr) {
        return;
    }
    if (initialization_result) {
        pg_atomic_write_u32(
            &g_shared->runtime_init_errc,
            static_cast<std::uint32_t>(bioseq::gpu::vulkan::shader_cache_error::ok));
    } else {
        g_shared->cache_digest_hex[0] = '\0';
        pg_atomic_write_u32(
            &g_shared->runtime_init_errc,
            static_cast<std::uint32_t>(classify_shader_cache_error(initialization_result.error())));
        bioseq_log<logging_level::warning, logging_target::postgres>(
            "[bioseq] runtime init failed: {}", initialization_result.error());
    }
    pg_write_barrier();
    pg_atomic_write_u32(&g_shared->runtime_init_state,
                        static_cast<std::uint32_t>(initialization_result
                                                       ? runtime_initialization_state::ready
                                                       : runtime_initialization_state::failed));
    ConditionVariableBroadcast(&g_shared->runtime_init_cv);
}

/**
 * @brief Checks whether a PostgreSQL process number addresses a live slot.
 * @param[in] procno Shared process number.
 * @return True when `ProcGlobal` contains the slot.
 */
[[nodiscard]] bool procno_in_range(std::uint32_t procno) noexcept {
    return procno != static_cast<std::uint32_t>(INVALID_PROC_NUMBER) && ProcGlobal != nullptr &&
           procno < static_cast<std::uint32_t>(ProcGlobal->allProcCount);
}

/**
 * @brief Creates or validates the backend-local sealed shader cache.
 * @return The prepared cache directory, or a diagnostic.
 */
[[nodiscard]] std::expected<std::filesystem::path, std::string>
prepare_backend_local_shader_cache_env();

/** @brief Wakes the currently published supervisor process when it is reachable. */
void wake_supervisor_if_known() {
    if (g_shared == nullptr) {
        return;
    }
    const std::uint32_t procno = pg_atomic_read_u32(&g_shared->supervisor_pgprocno);
    if (procno_in_range(procno)) {
        Latch* latch = &GetPGProcByNumber(static_cast<ProcNumber>(procno))->procLatch;
        bioseq::postgres::support::invoke_pg_as_cpp([latch] { SetLatch(latch); });
    }
}

/** @brief Runs PostgreSQL interrupt processing inside an error bridge. */
inline void pg_check_for_interrupts_as_cpp() {
    bioseq::postgres::support::invoke_pg_as_cpp([] { CHECK_FOR_INTERRUPTS(); });
}

/**
 * @brief Prepares one PostgreSQL condition variable for bridged sleep.
 * @param[in,out] condition Shared condition variable used by the waiter.
 */
inline void pg_condition_variable_prepare_to_sleep_as_cpp(ConditionVariable* condition) {
    bioseq::postgres::support::invoke_pg_as_cpp(
        [condition] { ConditionVariablePrepareToSleep(condition); });
}

/** @brief Cancels the current process condition-variable sleep preparation. */
inline void pg_condition_variable_cancel_sleep_as_cpp() {
    bioseq::postgres::support::invoke_pg_as_cpp([] { ConditionVariableCancelSleep(); });
}

/**
 * @brief Sleeps on a PostgreSQL condition variable inside an error bridge.
 * @param[in,out] condition Shared condition variable used by the waiter.
 * @param[in] timeout_ms Maximum sleep duration in milliseconds.
 */
inline void pg_condition_variable_timed_sleep_as_cpp(ConditionVariable* condition,
                                                     const long timeout_ms) {
    bioseq::postgres::support::invoke_pg_as_cpp([condition, timeout_ms] {
        (void)ConditionVariableTimedSleep(condition, timeout_ms, PG_WAIT_EXTENSION);
    });
}

/**
 * @brief Raises the terminal PostgreSQL error for unavailable GPU runtime state.
 * @param[in] detail Stable diagnostic copied before entering the PostgreSQL leaf.
 */
[[noreturn]] void report_runtime_init_unavailable(std::string_view detail) {
    // Own the rendered detail in the ordinary C++ frame.  The PG leaf captures
    // only stable pointers and therefore cannot longjmp across this string.
    const std::string diagnostic{detail};
    const char* diagnostic_cstr = diagnostic.c_str();
    bioseq::postgres::support::invoke_pg_as_cpp([diagnostic_cstr] {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg_internal("%s", "Vulkan aligner execution failed"),
                 errdetail_internal("%s", diagnostic_cstr),
                 errhint("%s",
                         "Selected PostgreSQL Vulkan execution requires a successful "
                         "bioseq GPU runtime initialization; use the backend-local helper "
                         "path or the shared-preload supervisor.")));
    });
    pg_unreachable();
}

/**
 * @brief Waits for shared initialization or prepares a backend-local runtime cache.
 *
 * A successful return fixes the process cache identity before Vulkan singleton access.
 */
void wait_for_runtime_ready() {
    static bool backend_runtime_ready = false;
    if (backend_runtime_ready) {
        return;
    }
    // PostgreSQL single-user mode has no postmaster child and launches no
    // registered background workers.  Waiting on preload-owned shared state
    // would therefore time out forever.  The process is not forked, so the
    // normal in-process Vulkan/compiler path is safe and needs no supervisor
    // archive handoff.
    if (is_standalone_backend_process()) {
        backend_runtime_ready = true;
        return;
    }
    if (g_shared == nullptr) {
        auto prepared = prepare_backend_local_shader_cache_env();
        if (!prepared) {
            report_runtime_init_unavailable(
                fmt::format("backend-local GPU runtime preparation failed: {}", prepared.error()));
        }
        backend_runtime_ready = true;
        return;
    }

    constexpr std::uint64_t k_deadline_ns = 120ull * 1'000'000'000ull;
    const std::uint64_t deadline =
        static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns()) + k_deadline_ns;
    bool sleep_prepared = false;
    auto cancel_sleep = [&sleep_prepared] {
        if (sleep_prepared) {
            pg_condition_variable_cancel_sleep_as_cpp();
            sleep_prepared = false;
        }
    };
    try {
        pg_condition_variable_prepare_to_sleep_as_cpp(&g_shared->runtime_init_cv);
        sleep_prepared = true;
        for (;;) {
            pg_check_for_interrupts_as_cpp();
            const auto runtime_state = static_cast<runtime_initialization_state>(
                pg_atomic_read_u32(&g_shared->runtime_init_state));
            pg_read_barrier();
            if (runtime_state == runtime_initialization_state::ready) {
                const std::string digest = current_runtime_cache_digest();
#if defined(__APPLE__)
                if (digest.empty()) {
                    cancel_sleep();
                    report_runtime_init_unavailable(
                        "load_bioseq_runtime reported ready without a shader-cache digest");
                }
#endif
                if (!digest.empty()) {
                    const std::filesystem::path cache_dir = runtime_cache_dir_from_digest(digest);
                    const auto inputs = conservative_shader_inputs();
                    auto keys = exact_aot_shader_keys(inputs);
                    if (!keys) {
                        const std::string diagnostic =
                            fmt::format("canonicalize exact AOT shader keys: {}", keys.error());
                        cancel_sleep();
                        report_runtime_init_unavailable(diagnostic);
                    }
                    auto seed = load_process_shader_seed_once(cache_dir, digest, *keys);
                    if (!seed) {
                        const std::string diagnostic = fmt::format(
                            "supervisor shader cache seal validation failed: {}", seed.error());
                        cancel_sleep();
                        report_runtime_init_unavailable(diagnostic);
                    }
                    if (auto installed = install_verified_runtime_archive_configuration(*seed);
                        !installed) {
                        const std::string diagnostic = installed.error();
                        cancel_sleep();
                        report_runtime_init_unavailable(diagnostic);
                    }
                    if (auto published = publish_runtime_cache_env(cache_dir); !published) {
                        const std::string diagnostic = published.error();
                        cancel_sleep();
                        report_runtime_init_unavailable(diagnostic);
                    }
                }
                backend_runtime_ready = true;
                cancel_sleep();
                return;
            }
            if (runtime_state == runtime_initialization_state::failed) {
                const std::uint32_t errc = pg_atomic_read_u32(&g_shared->runtime_init_errc);
                const std::string diagnostic =
                    fmt::format("load_bioseq_runtime failed (errc {}); GPU unavailable", errc);
                cancel_sleep();
                report_runtime_init_unavailable(diagnostic);
            }
            if (static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns()) > deadline) {
                cancel_sleep();
                report_runtime_init_unavailable(
                    "load_bioseq_runtime unavailable (supervisor down / timeout)");
            }
            pg_condition_variable_timed_sleep_as_cpp(&g_shared->runtime_init_cv, 1000L);
        }
    } catch (...) {
        if (sleep_prepared) {
            try {
                cancel_sleep();
            } catch (...) {
                // Preserve the initiating PostgreSQL/C++ error.  A valid prepared
                // condition-variable cancellation is not expected to fail.
            }
        }
        throw;
    }
}

/** @brief Captures one claimed shader slot generation and immutable variant key. */
struct compile_claim {
    std::size_t slot_index = 0;
    std::uint32_t generation = 0;
    bioseq::gpu::vulkan::shader_variant_key key{};
};

/**
 * @brief Claims the first requested shader slot for supervisor compilation.
 * @return A claim snapshot, or absence when no request is ready.
 */
[[nodiscard]] std::optional<compile_claim> try_claim_shader_compile_slot() noexcept {
    if (g_shared == nullptr) {
        return std::nullopt;
    }
    for (std::size_t slot_index = 0; slot_index < scheduler_shared_state::k_compile_queue_size;
         ++slot_index) {
        shader_compile_slot& slot = g_shared->compile_queue[slot_index];
        std::uint32_t expected = static_cast<std::uint32_t>(compile_slot_state::requested);
        if (!pg_atomic_compare_exchange_u32(
                &slot.state,
                &expected,
                static_cast<std::uint32_t>(compile_slot_state::compiling))) {
            continue;
        }
        pg_read_barrier();
        return compile_claim{
            .slot_index = slot_index,
            .generation = pg_atomic_read_u32(&slot.generation),
            .key = slot.key,
        };
    }
    return std::nullopt;
}

/**
 * @brief Clears one owned shader slot and publishes it as free.
 * @param[in] slot_index Shared compile-queue index.
 */
void release_shader_compile_slot(std::size_t slot_index) noexcept {
    if (g_shared == nullptr || slot_index >= scheduler_shared_state::k_compile_queue_size) {
        return;
    }
    shader_compile_slot& slot = g_shared->compile_queue[slot_index];
    slot.key = bioseq::gpu::vulkan::shader_variant_key{};
    slot.requester_pid = 0;
    slot.errc = static_cast<std::uint32_t>(bioseq::gpu::vulkan::shader_cache_error::ok);
    pg_atomic_fetch_add_u32(&slot.generation, 1);
    pg_write_barrier();
    pg_atomic_write_u32(&slot.state, static_cast<std::uint32_t>(compile_slot_state::free));
}

/**
 * @brief Claims terminal slot ownership before returning it to the queue.
 * @param[in] slot_index Shared compile-queue index.
 * @param[in] completed_state Expected terminal publication state.
 * @return True when this caller released the matching terminal slot.
 */
[[nodiscard]] bool
try_release_completed_shader_compile_slot(const std::size_t slot_index,
                                          const compile_slot_state completed_state) noexcept {
    if (g_shared == nullptr || slot_index >= scheduler_shared_state::k_compile_queue_size ||
        (completed_state != compile_slot_state::done_ok &&
         completed_state != compile_slot_state::done_error)) {
        return false;
    }
    shader_compile_slot& slot = g_shared->compile_queue[slot_index];
    std::uint32_t expected = static_cast<std::uint32_t>(completed_state);
    if (!pg_atomic_compare_exchange_u32(
            &slot.state, &expected, static_cast<std::uint32_t>(compile_slot_state::filling))) {
        return false;
    }
    release_shader_compile_slot(slot_index);
    return true;
}

/**
 * @brief Publishes one shader compilation result and wakes slot ownership cleanup.
 * @param[in] claim Claimed slot generation and variant key.
 * @param[in] errc Stable shader-cache result code.
 * @param[in] compile_succeeded Whether compilation completed successfully.
 */
void finish_shader_compile_slot(const compile_claim& claim,
                                bioseq::gpu::vulkan::shader_cache_error errc,
                                bool compile_succeeded) noexcept {
    if (g_shared == nullptr || claim.slot_index >= scheduler_shared_state::k_compile_queue_size) {
        return;
    }
    shader_compile_slot& slot = g_shared->compile_queue[claim.slot_index];
    slot.errc = static_cast<std::uint32_t>(errc);
    pg_atomic_fetch_add_u32(&slot.generation, 1);
    pg_write_barrier();
    pg_atomic_write_u32(&slot.state,
                        static_cast<std::uint32_t>(compile_succeeded
                                                       ? compile_slot_state::done_ok
                                                       : compile_slot_state::done_error));
    pg_read_barrier();
    // A cancelled waiter marks its request orphaned.  Whichever side observes
    // the cancellation after the Done publication owns returning the slot to the
    // queue; this prevents cancellation from permanently exhausting the queue.
    if (slot.requester_pid == 0) {
        (void)try_release_completed_shader_compile_slot(
            claim.slot_index,
            compile_succeeded ? compile_slot_state::done_ok : compile_slot_state::done_error);
    }
}

/**
 * @brief Cancels an unclaimed request or marks an active request as orphaned.
 * @param[in] slot_index Shared compile-queue index.
 * @param[in] generation Request generation observed by the waiter.
 */
void abandon_shader_compile_request(const std::size_t slot_index,
                                    const std::uint32_t generation) noexcept {
    if (g_shared == nullptr || slot_index >= scheduler_shared_state::k_compile_queue_size) {
        return;
    }
    shader_compile_slot& slot = g_shared->compile_queue[slot_index];
    const std::uint32_t observed_generation = pg_atomic_read_u32(&slot.generation);
    const auto observed_state = static_cast<compile_slot_state>(pg_atomic_read_u32(&slot.state));
    std::uint32_t expected = static_cast<std::uint32_t>(compile_slot_state::requested);
    if (observed_generation == generation && observed_state == compile_slot_state::requested &&
        pg_atomic_compare_exchange_u32(
            &slot.state, &expected, static_cast<std::uint32_t>(compile_slot_state::filling))) {
        release_shader_compile_slot(slot_index);
        return;
    }

    if (observed_generation == generation && observed_state == compile_slot_state::compiling) {
        slot.requester_pid = 0;
        pg_write_barrier();
        const auto observed = static_cast<compile_slot_state>(pg_atomic_read_u32(&slot.state));
        if (observed == compile_slot_state::done_ok || observed == compile_slot_state::done_error) {
            (void)try_release_completed_shader_compile_slot(slot_index, observed);
        }
        return;
    }
    if (observed_state == compile_slot_state::done_ok ||
        observed_state == compile_slot_state::done_error) {
        slot.requester_pid = 0;
        pg_write_barrier();
        (void)try_release_completed_shader_compile_slot(slot_index, observed_state);
    }
}

/**
 * @brief Publishes one exact shader variant into the shared compile queue.
 * @param[in] key Canonical shader variant identity.
 * @return Slot and generation identity, or a queue diagnostic.
 */
[[nodiscard]] std::expected<std::pair<std::size_t, std::uint32_t>, std::string>
publish_shader_compile_request(const bioseq::gpu::vulkan::shader_variant_key& key) {
    if (g_shared == nullptr) {
        return std::unexpected("shader compile queue unavailable");
    }
    for (std::size_t slot_index = 0; slot_index < scheduler_shared_state::k_compile_queue_size;
         ++slot_index) {
        shader_compile_slot& slot = g_shared->compile_queue[slot_index];
        std::uint32_t expected = static_cast<std::uint32_t>(compile_slot_state::free);
        if (!pg_atomic_compare_exchange_u32(
                &slot.state, &expected, static_cast<std::uint32_t>(compile_slot_state::filling))) {
            continue;
        }
        slot.key = key;
        slot.requester_pid = static_cast<std::uint32_t>(MyProcPid);
        slot.errc = static_cast<std::uint32_t>(bioseq::gpu::vulkan::shader_cache_error::ok);
        const std::uint32_t generation = pg_atomic_fetch_add_u32(&slot.generation, 1) + 1u;
        pg_write_barrier();
        pg_atomic_write_u32(&slot.state, static_cast<std::uint32_t>(compile_slot_state::requested));
        try {
            wake_supervisor_if_known();
        } catch (...) {
            abandon_shader_compile_request(slot_index, generation);
            throw;
        }
        return std::pair<std::size_t, std::uint32_t>{slot_index, generation};
    }
    return std::unexpected("shader compile queue full");
}

/** @brief Submits shader variants to the shared supervisor and waits for completion. */
struct pg_supervisor_compile_service {
    /**
     * @brief Compiles one canonical variant through the shared queue.
     * @param[in] key Canonical shader variant identity.
     * @return Success or a queue, compiler, cancellation, or timeout diagnostic.
     */
    [[nodiscard]] std::expected<void, std::string>
    ensure_variant_compiled(bioseq::gpu::vulkan::shader_variant_key key) {
        auto published = publish_shader_compile_request(key);
        if (!published) {
            return std::unexpected(published.error());
        }
        const auto [slot_index, generation] = *published;
        constexpr std::uint64_t k_deadline_ns = 120ull * 1'000'000'000ull;
        const std::uint64_t deadline =
            static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns()) + k_deadline_ns;
        bool sleep_prepared = false;
        auto cancel_sleep = [&sleep_prepared] {
            if (sleep_prepared) {
                pg_condition_variable_cancel_sleep_as_cpp();
                sleep_prepared = false;
            }
        };
        try {
            pg_condition_variable_prepare_to_sleep_as_cpp(&g_shared->compile_done_cv);
            sleep_prepared = true;
            for (;;) {
                pg_check_for_interrupts_as_cpp();
                shader_compile_slot& slot = g_shared->compile_queue[slot_index];
                const auto slot_state =
                    static_cast<compile_slot_state>(pg_atomic_read_u32(&slot.state));
                const std::uint32_t observed_generation = pg_atomic_read_u32(&slot.generation);
                if ((slot_state == compile_slot_state::done_ok ||
                     slot_state == compile_slot_state::done_error) &&
                    observed_generation != generation) {
                    pg_read_barrier();
                    const std::uint32_t error_code = slot.errc;
                    cancel_sleep();
                    (void)try_release_completed_shader_compile_slot(slot_index, slot_state);
                    if (slot_state == compile_slot_state::done_ok) {
                        return {};
                    }
                    return std::unexpected(fmt::format("shader compile failed for {} (errc {})",
                                                       bioseq::gpu::vulkan::cache_file_stem(key),
                                                       error_code));
                }
                if (static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns()) >
                    deadline) {
                    const std::string diagnostic =
                        fmt::format("shader compile timed out for {}",
                                    bioseq::gpu::vulkan::cache_file_stem(key));
                    cancel_sleep();
                    abandon_shader_compile_request(slot_index, generation);
                    return std::unexpected(diagnostic);
                }
                pg_condition_variable_timed_sleep_as_cpp(&g_shared->compile_done_cv, 1000L);
            }
        } catch (...) {
            if (sleep_prepared) {
                try {
                    cancel_sleep();
                } catch (...) {
                }
            }
            abandon_shader_compile_request(slot_index, generation);
            throw;
        }
    }
};

[[nodiscard]] std::expected<std::filesystem::path, std::string>
prepare_backend_local_shader_cache_env() {
    const auto inputs = conservative_shader_inputs();
    auto selected_digest = runtime_shader_cache_digest(inputs);
    if (!selected_digest) {
        return std::unexpected(selected_digest.error());
    }
    const std::string& digest = *selected_digest;
    const std::filesystem::path cache_dir = runtime_cache_dir_from_digest(digest);
    auto keys = exact_aot_shader_keys(inputs);
    if (!keys) {
        return std::unexpected(fmt::format("canonicalize exact AOT shader keys: {}", keys.error()));
    }
    const std::filesystem::path manifest_path =
        bioseq::gpu::vulkan::sealed_shader_cache_manifest_path(cache_dir);
    std::error_code filesystem_error;
    const bool manifest_exists = std::filesystem::exists(manifest_path, filesystem_error);
    if (filesystem_error) {
        return std::unexpected(fmt::format("stat sealed shader cache manifest {} failed: {}",
                                           manifest_path.string(),
                                           filesystem_error.message()));
    }
    if (manifest_exists) {
        auto seed = load_process_shader_seed_once(cache_dir, digest, *keys);
        if (!seed) {
            return std::unexpected(
                fmt::format("existing sealed shader cache is invalid and will not be repaired: {}",
                            seed.error()));
        }
        if (auto installed = install_verified_runtime_archive_configuration(*seed); !installed) {
            return std::unexpected(installed.error());
        }
        if (auto published = publish_runtime_cache_env(cache_dir); !published) {
            return std::unexpected(published.error());
        }
        return cache_dir;
    }

    for (const auto& key : *keys) {
        auto compiled =
            bioseq::gpu::vulkan::ensure_compiled_for_sealed_seed(cache_dir, key, inputs.msl);
        if (!compiled) {
            return std::unexpected(fmt::format("backend-local exact AOT compile {}: {}",
                                               bioseq::gpu::vulkan::cache_file_stem(key),
                                               compiled.error()));
        }
    }
    if (auto command_library =
            bioseq::gpu::vulkan::ensure_moltenvk_command_library_for_sealed_seed(cache_dir);
        !command_library) {
        return std::unexpected(fmt::format(
            "backend-local exact MoltenVK command library: {}", command_library.error()));
    }
    auto sealed = bioseq::gpu::vulkan::seal_shader_cache_manifest(cache_dir, digest, *keys);
    if (!sealed) {
        return std::unexpected(
            fmt::format("seal backend-local exact shader AOT seed: {}", sealed.error()));
    }
    auto seed = load_process_shader_seed_once(cache_dir, digest, *keys);
    if (!seed) {
        return std::unexpected(
            fmt::format("load newly sealed backend-local shader seed: {}", seed.error()));
    }
    if (auto installed = install_verified_runtime_archive_configuration(*seed); !installed) {
        return std::unexpected(installed.error());
    }
    if (auto published = publish_runtime_cache_env(cache_dir); !published) {
        return std::unexpected(published.error());
    }
    return cache_dir;
}

/** @brief Compiles shader variants directly into one backend-local cache. */
struct pg_backend_compile_service {
    std::filesystem::path cache_dir;
    bioseq::gpu::vulkan::msl_version msl;

    /**
     * @brief Compiles one canonical variant into this service cache.
     * @param[in] key Canonical shader variant identity.
     * @return Success or the compiler diagnostic.
     */
    [[nodiscard]] std::expected<void, std::string>
    ensure_variant_compiled(bioseq::gpu::vulkan::shader_variant_key key) {
        return bioseq::gpu::vulkan::ensure_compiled(cache_dir, key, msl);
    }
};

/**
 * @brief Warms and validates every backend-local shader-cache variant.
 * @return Stable multiline status including variant and artifact identities.
 */
[[nodiscard]] std::string warmup_backend_local_shader_cache_report() {
    std::ostringstream report;
#if !defined(__APPLE__)
    report << "pid=" << static_cast<long>(MyProcPid) << " platform=non-apple\n"
           << "RESULT: ok variants=0 failed=0\n";
    return report.str();
#else
    const auto inputs = conservative_shader_inputs();
    auto prepared = prepare_backend_local_shader_cache_env();
    if (!prepared) {
        report << "pid=" << static_cast<long>(MyProcPid) << '\n'
               << "RESULT: failed\n"
               << "error=" << prepared.error() << '\n';
        return report.str();
    }

    const std::filesystem::path& cache_dir = *prepared;
    report << "pid=" << static_cast<long>(MyProcPid)
           << " supervisor=" << (g_shared != nullptr ? 1 : 0) << " cache_dir=" << cache_dir.string()
           << '\n';
    if (const char* env = std::getenv("BIOSEQ_MOLTENVK_COMMAND_METALLIB"); env != nullptr) {
        report << "env BIOSEQ_MOLTENVK_COMMAND_METALLIB=" << env << '\n';
    }
    if (const char* env = std::getenv("BIOSEQ_MOLTENVK_BINARY_ARCHIVE_DIR"); env != nullptr) {
        report << "env BIOSEQ_MOLTENVK_BINARY_ARCHIVE_DIR=" << env << '\n';
    }

    auto keys = exact_aot_shader_keys(inputs);
    if (!keys) {
        report << "RESULT: failed\n"
               << "error=canonicalize exact AOT shader keys: " << keys.error() << '\n';
        return report.str();
    }
    const std::string digest = cache_dir.filename().string();
    const auto seed = process_shader_seed_snapshot();
    if (!seed || !seed->is_accepted() || seed->manifest().cache_digest != digest ||
        seed->manifest().keys != *keys) {
        report << "RESULT: failed\n"
               << "error=process-owned shader seed unavailable after preparation\n";
        return report.str();
    }
    const auto& sealed = seed->manifest();
    report << "sealed_manifest="
           << bioseq::gpu::vulkan::sealed_shader_cache_manifest_path(cache_dir).string()
           << " seed_bytes=" << sealed.seed_size
           << " manifest_checksum=" << sealed.manifest_checksum
           << " seed_load_count=" << seed->receipt().load_count()
           << " seed_load_ns=" << seed->receipt().load_ns() << '\n';

    std::size_t failed = 0;
    for (const auto& key : *keys) {
        const std::string stem = bioseq::gpu::vulkan::cache_file_stem(key);
        auto compatible = bioseq::gpu::vulkan::make_sealed_shader_cache_key_view(*seed, key);
        if (!compatible) {
            ++failed;
            report << "variant " << stem << " kind=" << key.kind << " tile=" << key.tile_size
                   << " local=" << key.local_size << " max_span=" << key.max_span
                   << " status=failed error=" << compatible.error() << '\n';
            continue;
        }
        const auto key_iterator = std::find(sealed.keys.begin(), sealed.keys.end(), key);
        const std::uint32_t key_index =
            static_cast<std::uint32_t>(std::distance(sealed.keys.begin(), key_iterator));
        const auto* metallib = bioseq::gpu::vulkan::sealed_shader_cache_detail::find_artifact_image(
            seed->artifacts(),
            bioseq::gpu::vulkan::sealed_shader_cache_artifact_kind::shader_metallib,
            key_index,
            stem + ".metallib");
        const auto* meta = bioseq::gpu::vulkan::sealed_shader_cache_detail::find_artifact_image(
            seed->artifacts(),
            bioseq::gpu::vulkan::sealed_shader_cache_artifact_kind::shader_meta,
            key_index,
            stem + ".meta");
        std::ostringstream archive_bytes;
        bool first_archive = true;
        for (const auto& archive : seed->artifacts()) {
            if (archive.descriptor.key_index != key_index ||
                archive.descriptor.kind !=
                    bioseq::gpu::vulkan::sealed_shader_cache_artifact_kind::metal_binary_archive) {
                continue;
            }
            if (!first_archive) {
                archive_bytes << ',';
            }
            first_archive = false;
            archive_bytes << archive.descriptor.filename << ':' << archive.bytes.size();
        }
        report << "variant " << stem << " kind=" << key.kind << " tile=" << key.tile_size
               << " local=" << key.local_size << " max_span=" << key.max_span << " status=ok"
               << " metallib_bytes=" << (metallib ? metallib->bytes.size() : 0u)
               << " meta_bytes=" << (meta ? meta->bytes.size() : 0u)
               << " archive_bytes=" << archive_bytes.str() << '\n';
    }
    report << "RESULT: " << (failed == 0 ? "ok" : "failed") << " variants=" << keys->size()
           << " failed=" << failed << '\n';
    return report.str();
#endif
}

/**
 * @brief Prepares the shader provider before runtime pipeline creation.
 * @return Success or the shared or backend-local preparation diagnostic.
 */
[[nodiscard]] std::expected<void, std::string> pg_shader_provider_prepare_runtime() {
    if (g_shared != nullptr) {
        wait_for_runtime_ready();
        return {};
    }
    auto prepared = prepare_backend_local_shader_cache_env();
    if (!prepared) {
        return std::unexpected(prepared.error());
    }
    return {};
}

/**
 * @brief Loads one exact shader module from the accepted process seed.
 * @param[in] device Vulkan device receiving the module.
 * @param[in] key Canonical shader variant identity.
 * @param[in] create_shader_module Vulkan creation callback for the active loader.
 * @return Loaded module ownership, or an identity or Vulkan diagnostic.
 */
[[nodiscard]] std::expected<bioseq::gpu::vulkan::shader_module_load, std::string>
pg_shader_provider_load_module(VkDevice device,
                               const bioseq::gpu::vulkan::shader_variant_key& key,
                               PFN_vkCreateShaderModule create_shader_module) {
    const auto inputs = conservative_shader_inputs();
    auto keys = exact_aot_shader_keys(inputs);
    if (!keys) {
        return std::unexpected(fmt::format("canonicalize exact AOT shader keys: {}", keys.error()));
    }
    if (g_shared != nullptr) {
        wait_for_runtime_ready();
    } else if (!process_shader_seed_snapshot()) {
        auto prepared = prepare_backend_local_shader_cache_env();
        if (!prepared)
            return std::unexpected(prepared.error());
    }
    const auto seed = process_shader_seed_snapshot();
    if (!seed || !seed->is_accepted() || seed->manifest().keys != *keys) {
        return std::unexpected("process-owned sealed shader seed is unavailable or incompatible");
    }
    return bioseq::gpu::vulkan::load_sealed_cached_module(device, *seed, key, create_shader_module);
}

/** @brief Installs the PostgreSQL shader provider before first runtime access. */
void install_pg_shader_provider_if_available() noexcept {
#if defined(__APPLE__)
    if (process_shared_preload_libraries_in_progress || is_standalone_backend_process()) {
        return;
    }
    static bioseq::gpu::vulkan::runtime_shader_provider provider{
        &pg_shader_provider_prepare_runtime, &pg_shader_provider_load_module};
    bioseq::gpu::vulkan::install_runtime_shader_provider(&provider);
#endif
}

/**
 * @brief Prepares PostgreSQL shader state before accessing the Vulkan singleton.
 *
 * MoltenVK consumes cache configuration while creating device and pipeline state. Every
 * PostgreSQL operation completes inside an error bridge before control returns here.
 */
void prepare_pg_vulkan_runtime_ready() {
    install_pg_shader_provider_if_available();
    wait_for_runtime_ready();
}

/**
 * @brief Samples the process-local Vulkan runtime after PostgreSQL preparation.
 * @return Device availability, limits, and initialization diagnostic.
 */
vulkan_runtime_status_snapshot snapshot_vulkan_runtime_status() {
    prepare_pg_vulkan_runtime_ready();
    auto& runtime = bioseq::gpu::vulkan::runtime::instance();
    const auto& features = runtime.features();
    return vulkan_runtime_status_snapshot{
        .available = features.available,
        .device_name = std::string{features.device_name.data()},
        .max_workgroup_count_x = static_cast<std::int32_t>(std::min<std::uint32_t>(
            features.max_workgroup_count_x,
            static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))),
        .scalar_block_layout = features.scalar_block_layout,
        .initialization_error = runtime.initialization_error(),
        .device_local_bytes = static_cast<std::int64_t>(std::min<std::uint64_t>(
            features.device_local_bytes,
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))),
        .software_device = features.software_device,
    };
}

/** @brief Claims and compiles up to four ready supervisor shader requests. */
void service_pending_shader_compiles() {
    if (g_shared == nullptr) {
        return;
    }
    const std::string digest = current_runtime_cache_digest();
    if (digest.empty()) {
        return;
    }
    const std::filesystem::path cache_dir = runtime_cache_dir_from_digest(digest);
    const auto inputs = conservative_shader_inputs();
    int done = 0;
    while (done < 4) {
        auto claim = try_claim_shader_compile_slot();
        if (!claim) {
            break;
        }
        auto compiled = bioseq::gpu::vulkan::ensure_compiled(cache_dir, claim->key, inputs.msl);
        finish_shader_compile_slot(*claim,
                                   compiled ? bioseq::gpu::vulkan::shader_cache_error::ok
                                            : classify_shader_cache_error(compiled.error()),
                                   compiled.has_value());
        ++done;
    }
    if (done > 0) {
        ConditionVariableBroadcast(&g_shared->compile_done_cv);
    }
}
} // namespace bioseq::postgres::scheduler
