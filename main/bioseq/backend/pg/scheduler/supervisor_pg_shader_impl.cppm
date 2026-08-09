/**
 * @file supervisor_pg_shader_impl.cppm
 * @brief PostgreSQL Vulkan shader-cache and runtime supervision implementation.
 */
module;
#include "common.h"

extern "C" {
#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/condition_variable.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
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
import bioseq.common.mlogging;
import bioseq.config;
import bioseq.instr;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.gpu.vulkan.runtime;
import bioseq.gpu.vulkan.shader_cache;
import bioseq.gpu.vulkan.shader_variants;
import bioseq.pg_utils;

namespace bioseq::parallelism::pg {

static_assert(kShaderCacheOkWireValue ==
    static_cast<std::uint32_t>(bioseq::gpu::vulkan::ShaderCacheErrc::Ok));

[[nodiscard]] bioseq::gpu::vulkan::ShaderCacheErrc classify_shader_cache_error(
        std::string_view error) noexcept {
    using bioseq::gpu::vulkan::ShaderCacheErrc;
    if (error.find("metal unavailable") != std::string_view::npos
        || error.find("No such file") != std::string_view::npos) {
        return ShaderCacheErrc::NoToolchain;
    }
    if (error.find("glslang") != std::string_view::npos
        || error.find("shader compile") != std::string_view::npos) {
        return ShaderCacheErrc::Glslang;
    }
    if (error.find("SPIR-V to MSL") != std::string_view::npos) {
        return ShaderCacheErrc::SpirvToMsl;
    }
    if (error.find("spawn failed") != std::string_view::npos) {
        return ShaderCacheErrc::MetalSpawn;
    }
    if (error.find("exited with status") != std::string_view::npos
        || error.find("terminated by signal") != std::string_view::npos) {
        return ShaderCacheErrc::MetalExit;
    }
    if (error.find("ABI") != std::string_view::npos
        || error.find("layout mismatch") != std::string_view::npos
        || error.find("stride") != std::string_view::npos) {
        return ShaderCacheErrc::AbiScan;
    }
    return ShaderCacheErrc::MetaIo;
}

[[nodiscard]] std::filesystem::path runtime_cache_root() {
    const char* data_dir = DataDir != nullptr ? DataDir : ".";
    return std::filesystem::path(data_dir) / "bioseq_runtime_cache";
}

[[nodiscard]] std::filesystem::path runtime_cache_dir_from_digest(
        std::string_view digest) {
    return runtime_cache_root() / std::string(digest);
}

[[nodiscard]] std::expected<void, std::string>
publish_runtime_cache_env(const std::filesystem::path& cache_dir) {
    // No exact MoltenVK command-source AOT capture exists yet. Never publish
    // the old no-op placeholder as immutable command evidence; Runtime's
    // recognized command-source interpose must fail closed instead.
    if (::unsetenv("BIOSEQ_MOLTENVK_COMMAND_METALLIB") != 0) {
        const int saved_errno = errno;
        return std::unexpected(fmt::format(
            "unsetenv BIOSEQ_MOLTENVK_COMMAND_METALLIB failed: errno {} ({})",
            saved_errno, std::strerror(saved_errno)));
    }
    if (::setenv(
            "BIOSEQ_MOLTENVK_BINARY_ARCHIVE_DIR", cache_dir.c_str(), 1) != 0) {
        const int saved_errno = errno;
        return std::unexpected(fmt::format(
            "setenv BIOSEQ_MOLTENVK_BINARY_ARCHIVE_DIR failed: errno {} ({})",
            saved_errno, std::strerror(saved_errno)));
    }
    const std::filesystem::path sealed_manifest =
        bioseq::gpu::vulkan::sealed_shader_cache_manifest_path(cache_dir);
    if (::setenv(
            "BIOSEQ_GPU_SEALED_CACHE_MANIFEST", sealed_manifest.c_str(), 1) != 0) {
        const int saved_errno = errno;
        return std::unexpected(fmt::format(
            "setenv BIOSEQ_GPU_SEALED_CACHE_MANIFEST failed: errno {} ({})",
            saved_errno, std::strerror(saved_errno)));
    }
    return {};
}

[[nodiscard]] std::string current_runtime_cache_digest() {
    if (g_shared == nullptr) {
        return {};
    }
    char local[sizeof(g_shared->cache_digest_hex)]{};
    std::memcpy(local, g_shared->cache_digest_hex, sizeof(local));
    local[sizeof(local) - 1] = '\0';
    return std::string(local);
}

[[nodiscard]] bioseq::gpu::vulkan::ShaderCacheBuildInputs conservative_shader_inputs() {
    bioseq::gpu::vulkan::ShaderCacheBuildInputs inputs{};
    inputs.variants.max_workgroup_invocations = 256u;
    inputs.variants.max_shared_memory_bytes = 32u * 1024u;
    inputs.variants.default_window_size =
        static_cast<std::uint32_t>(std::max(1, bioseq::aligner::settings::window_size));
    inputs.msl = bioseq::gpu::vulkan::MslVersion{3u, 2u, 0u};
    inputs.moltenvk_build = "conservative-no-device-probe";
    inputs.device_uuid = "conservative-no-device-probe";
    return inputs;
}

[[nodiscard]] std::expected<
    std::vector<bioseq::gpu::vulkan::ShaderVariantKey>, std::string>
exact_aot_shader_keys(
        const bioseq::gpu::vulkan::ShaderCacheBuildInputs& inputs) {
    std::vector<bioseq::gpu::vulkan::ShaderVariantKey> keys;
    bioseq::gpu::vulkan::for_each_static_shader_variant(
        inputs.variants,
        [&keys](const bioseq::gpu::vulkan::ShaderVariantKey& key) {
            keys.push_back(key);
        });
    return bioseq::gpu::vulkan::canonical_sealed_shader_cache_keys(keys);
}

[[nodiscard]] bioseq::gpu::vulkan::SealedShaderCacheProcessStore&
process_shader_seed_store() noexcept {
    static bioseq::gpu::vulkan::SealedShaderCacheProcessStore store;
    return store;
}

[[nodiscard]] std::expected<std::shared_ptr<
    const bioseq::gpu::vulkan::SealedShaderCacheSeedImage>, std::string>
load_process_shader_seed_once(
    const std::filesystem::path& cache_dir,
    std::string_view digest,
    std::span<const bioseq::gpu::vulkan::ShaderVariantKey> keys)
{
    return process_shader_seed_store().load_once(cache_dir, digest, keys);
}

[[nodiscard]] std::shared_ptr<
    const bioseq::gpu::vulkan::SealedShaderCacheSeedImage>
process_shader_seed_snapshot() {
    return process_shader_seed_store().snapshot();
}

[[nodiscard]] std::expected<void, std::string>
install_verified_runtime_archive_configuration(
    std::shared_ptr<
        const bioseq::gpu::vulkan::SealedShaderCacheSeedImage> seed)
{
    if (!seed || !seed->accepted()) {
        return std::unexpected(
            "runtime archive configuration has no accepted process seed image");
    }
    auto configuration = bioseq::gpu::vulkan::
        make_runtime_metal_archive_configuration_image(std::move(seed));
    if (!configuration) {
        return std::unexpected(fmt::format(
            "prepare admitted Metal archive configuration: {}",
            configuration.error()));
    }
    auto installed =
        bioseq::gpu::vulkan::install_runtime_metal_archive_configuration(
            std::move(*configuration));
    if (!installed) {
        return std::unexpected(fmt::format(
            "install sealed Metal archive configuration: {}",
            installed.error()));
    }
    return {};
}

[[nodiscard]] std::expected<void, std::string> run_shader_cache_aot_phase() {
#if !defined(__APPLE__)
    return {};
#else
    const auto inputs = conservative_shader_inputs();
    const std::string digest = bioseq::gpu::vulkan::shader_cache_digest_hex(inputs);
    const std::filesystem::path cache_dir = runtime_cache_dir_from_digest(digest);
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    if (ec) {
        return std::unexpected(fmt::format(
            "create shader cache dir {} failed: {}",
            cache_dir.string(), ec.message()));
    }
    auto keys = exact_aot_shader_keys(inputs);
    if (!keys) {
        return std::unexpected(fmt::format(
            "canonicalize exact AOT shader keys: {}", keys.error()));
    }
    const std::filesystem::path manifest_path =
        bioseq::gpu::vulkan::sealed_shader_cache_manifest_path(cache_dir);
    const bool manifest_exists = std::filesystem::exists(manifest_path, ec);
    if (ec) {
        return std::unexpected(fmt::format(
            "stat sealed shader cache manifest {} failed: {}",
            manifest_path.string(), ec.message()));
    }
    if (manifest_exists) {
        auto sealed =
            bioseq::gpu::vulkan::load_validated_sealed_shader_cache_manifest(
                cache_dir, digest, *keys);
        if (!sealed) {
            return std::unexpected(fmt::format(
                "existing sealed shader cache is invalid and will not be repaired: {}",
                sealed.error()));
        }
        if (g_shared != nullptr) {
            std::snprintf(
                g_shared->cache_digest_hex, sizeof(g_shared->cache_digest_hex),
                "%s", digest.c_str());
        }
        return {};
    }

    bioseq::parallelism::GlobalPool::instance().configure(
        bioseq::parallelism::PoolConfig{
            .max_concurrency = effective_max_concurrency(
                static_cast<unsigned>(std::max(0, bioseq::scheduler::settings::max_concurrency))),
            .mode = bioseq::parallelism::mode_from_int(
                static_cast<int>(bioseq::scheduler::settings::pipeline_concurrency_mode.value())),
        });

    constexpr std::uint64_t kPeakCompileBytes = 256ull << 20;
    std::size_t cursor = 0;
    while (cursor < keys->size()) {
        const unsigned requested = static_cast<unsigned>(
            std::min<std::size_t>(4, keys->size() - cursor));
        auto wave = bioseq::parallelism::try_reserve_budgeted_wave(
            requested, kPeakCompileBytes);
        if (!wave.admitted) {
            return std::unexpected("shader cache AOT memory admission failed");
        }
        const std::size_t begin = cursor;
        const std::size_t end = std::min<std::size_t>(
            keys->size(), cursor + wave.fanout);
        std::mutex error_mu;
        std::string first_error;
        bioseq::parallelism::GlobalPool::instance().parallel_for(
            begin, end, 1,
            [&](std::size_t b, std::size_t e) {
                for (std::size_t i = b; i < e; ++i) {
                    auto compiled =
                        bioseq::gpu::vulkan::ensure_compiled_for_sealed_seed(
                        cache_dir, (*keys)[i], inputs.msl);
                    if (!compiled) {
                        std::lock_guard<std::mutex> guard(error_mu);
                        if (first_error.empty()) {
                            first_error = fmt::format(
                                "{}: {}",
                                bioseq::gpu::vulkan::cache_file_stem((*keys)[i]),
                                compiled.error());
                        }
                    }
                }
            });
        if (!first_error.empty()) {
            return std::unexpected(first_error);
        }
        cursor = end;
    }
    auto sealed = bioseq::gpu::vulkan::seal_shader_cache_manifest(
        cache_dir, digest, *keys);
    if (!sealed) {
        return std::unexpected(fmt::format(
            "seal exact shader AOT seed: {}", sealed.error()));
    }
    if (g_shared != nullptr) {
        std::snprintf(
            g_shared->cache_digest_hex, sizeof(g_shared->cache_digest_hex),
            "%s", digest.c_str());
    }
    return {};
#endif
}

[[nodiscard]] std::expected<void, std::string> load_bioseq_runtime() {
    return run_shader_cache_aot_phase();
}

void publish_runtime_init_result(std::expected<void, std::string> result) noexcept {
    if (g_shared == nullptr) {
        return;
    }
    if (result) {
        pg_atomic_write_u32(
            &g_shared->runtime_init_errc,
            static_cast<std::uint32_t>(bioseq::gpu::vulkan::ShaderCacheErrc::Ok));
    } else {
        g_shared->cache_digest_hex[0] = '\0';
        pg_atomic_write_u32(
            &g_shared->runtime_init_errc,
            static_cast<std::uint32_t>(classify_shader_cache_error(result.error())));
        bioseq_log<WRN, PG>("[bioseq] runtime init failed: {}", result.error());
    }
    pg_write_barrier();
    pg_atomic_write_u32(
        &g_shared->runtime_init_state,
        static_cast<std::uint32_t>(
            result ? RuntimeInitState::Ready : RuntimeInitState::Failed));
    ConditionVariableBroadcast(&g_shared->runtime_init_cv);
}

[[nodiscard]] bool procno_in_range(std::uint32_t procno) noexcept {
    return procno != static_cast<std::uint32_t>(INVALID_PROC_NUMBER)
        && ProcGlobal != nullptr
        && procno < static_cast<std::uint32_t>(ProcGlobal->allProcCount);
}

[[nodiscard]] std::expected<std::filesystem::path, std::string>
prepare_backend_local_shader_cache_env();

void wake_supervisor_if_known() {
    if (g_shared == nullptr) {
        return;
    }
    const std::uint32_t procno = pg_atomic_read_u32(&g_shared->supervisor_pgprocno);
    if (procno_in_range(procno)) {
		Latch* latch =
			&GetPGProcByNumber(static_cast<ProcNumber>(procno))->procLatch;
		bioseq::pg_utils::invoke_pg_as_cpp(
			[latch] { SetLatch(latch); });
    }
}

inline void pg_check_for_interrupts_as_cpp() {
	bioseq::pg_utils::invoke_pg_as_cpp(
		[] { CHECK_FOR_INTERRUPTS(); });
}

inline void pg_condition_variable_prepare_to_sleep_as_cpp(
		ConditionVariable* condition) {
	bioseq::pg_utils::invoke_pg_as_cpp(
		[condition] { ConditionVariablePrepareToSleep(condition); });
}

inline void pg_condition_variable_cancel_sleep_as_cpp() {
	bioseq::pg_utils::invoke_pg_as_cpp(
		[] { ConditionVariableCancelSleep(); });
}

inline void pg_condition_variable_timed_sleep_as_cpp(
		ConditionVariable* condition,
		const long timeout_ms) {
	bioseq::pg_utils::invoke_pg_as_cpp([condition, timeout_ms] {
		(void)ConditionVariableTimedSleep(
			condition, timeout_ms, PG_WAIT_EXTENSION);
	});
}

[[noreturn]] void report_runtime_init_unavailable(std::string_view detail) {
	// Own the rendered detail in the ordinary C++ frame.  The PG leaf captures
	// only stable pointers and therefore cannot longjmp across this string.
	const std::string diagnostic{detail};
	const char* diagnostic_cstr = diagnostic.c_str();
	bioseq::pg_utils::invoke_pg_as_cpp([diagnostic_cstr] {
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
            report_runtime_init_unavailable(fmt::format(
                "backend-local GPU runtime preparation failed: {}",
                prepared.error()));
        }
        backend_runtime_ready = true;
        return;
    }

    constexpr std::uint64_t kDeadlineNs = 120ull * 1'000'000'000ull;
    const std::uint64_t deadline =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns()) + kDeadlineNs;
	bool sleep_prepared = false;
	auto cancel_sleep = [&sleep_prepared] {
		if (sleep_prepared) {
			pg_condition_variable_cancel_sleep_as_cpp();
			sleep_prepared = false;
		}
	};
	try {
		pg_condition_variable_prepare_to_sleep_as_cpp(
			&g_shared->runtime_init_cv);
		sleep_prepared = true;
		for (;;) {
			pg_check_for_interrupts_as_cpp();
			const auto state = static_cast<RuntimeInitState>(
				pg_atomic_read_u32(&g_shared->runtime_init_state));
			pg_read_barrier();
			if (state == RuntimeInitState::Ready) {
				const std::string digest = current_runtime_cache_digest();
#if defined(__APPLE__)
				if (digest.empty()) {
					cancel_sleep();
					report_runtime_init_unavailable(
						"load_bioseq_runtime reported ready without a shader-cache digest");
				}
#endif
				if (!digest.empty()) {
					const std::filesystem::path cache_dir =
						runtime_cache_dir_from_digest(digest);
					const auto inputs = conservative_shader_inputs();
					auto keys = exact_aot_shader_keys(inputs);
					if (!keys) {
						const std::string diagnostic = fmt::format(
							"canonicalize exact AOT shader keys: {}",
							keys.error());
						cancel_sleep();
						report_runtime_init_unavailable(diagnostic);
					}
					auto seed = load_process_shader_seed_once(
						cache_dir, digest, *keys);
					if (!seed) {
						const std::string diagnostic = fmt::format(
							"supervisor shader cache seal validation failed: {}",
							seed.error());
						cancel_sleep();
						report_runtime_init_unavailable(diagnostic);
					}
					if (auto installed =
							install_verified_runtime_archive_configuration(
								*seed);
						!installed) {
						const std::string diagnostic = installed.error();
						cancel_sleep();
						report_runtime_init_unavailable(diagnostic);
					}
					if (auto published = publish_runtime_cache_env(cache_dir);
						!published) {
						const std::string diagnostic = published.error();
						cancel_sleep();
						report_runtime_init_unavailable(diagnostic);
					}
				}
				backend_runtime_ready = true;
				cancel_sleep();
				return;
			}
			if (state == RuntimeInitState::Failed) {
				const std::uint32_t errc =
					pg_atomic_read_u32(&g_shared->runtime_init_errc);
				const std::string diagnostic = fmt::format(
					"load_bioseq_runtime failed (errc {}); GPU unavailable", errc);
				cancel_sleep();
				report_runtime_init_unavailable(diagnostic);
			}
			if (static_cast<std::uint64_t>(bioseq::instr::clock::now_ns())
				> deadline) {
				cancel_sleep();
				report_runtime_init_unavailable(
					"load_bioseq_runtime unavailable (supervisor down / timeout)");
			}
			pg_condition_variable_timed_sleep_as_cpp(
				&g_shared->runtime_init_cv, 1000L);
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

struct CompileClaim {
    std::size_t slot_index = 0;
    std::uint32_t generation = 0;
    bioseq::gpu::vulkan::ShaderVariantKey key{};
};

[[nodiscard]] std::optional<CompileClaim> try_claim_shader_compile_slot() noexcept {
    if (g_shared == nullptr) {
        return std::nullopt;
    }
    for (std::size_t slot_index = 0;
         slot_index < BioseqSchedulerShared::kCompileQueueSize;
         ++slot_index) {
        ShaderCompileSlot& slot = g_shared->compile_queue[slot_index];
        std::uint32_t expected =
            static_cast<std::uint32_t>(CompileSlotState::Requested);
        if (!pg_atomic_compare_exchange_u32(
                &slot.state, &expected,
                static_cast<std::uint32_t>(CompileSlotState::Compiling))) {
            continue;
        }
        pg_read_barrier();
        return CompileClaim{
            .slot_index = slot_index,
            .generation = pg_atomic_read_u32(&slot.generation),
            .key = slot.key,
        };
    }
    return std::nullopt;
}

void release_shader_compile_slot(std::size_t slot_index) noexcept;
[[nodiscard]] bool try_release_completed_shader_compile_slot(
	std::size_t slot_index,
	CompileSlotState completed_state) noexcept;

void finish_shader_compile_slot(
        const CompileClaim& claim,
        bioseq::gpu::vulkan::ShaderCacheErrc errc,
        bool ok) noexcept {
    if (g_shared == nullptr
        || claim.slot_index >= BioseqSchedulerShared::kCompileQueueSize) {
        return;
    }
    ShaderCompileSlot& slot = g_shared->compile_queue[claim.slot_index];
    slot.errc = static_cast<std::uint32_t>(errc);
    pg_atomic_fetch_add_u32(&slot.generation, 1);
    pg_write_barrier();
    pg_atomic_write_u32(
        &slot.state,
        static_cast<std::uint32_t>(
            ok ? CompileSlotState::DoneOk : CompileSlotState::DoneErr));
	pg_read_barrier();
	// A cancelled waiter marks its request orphaned.  Whichever side observes
	// the cancellation after the Done publication owns returning the slot to the
	// queue; this prevents cancellation from permanently exhausting the queue.
	if (slot.requester_pid == 0) {
		(void)try_release_completed_shader_compile_slot(
			claim.slot_index,
			ok ? CompileSlotState::DoneOk : CompileSlotState::DoneErr);
	}
}

void release_shader_compile_slot(std::size_t slot_index) noexcept {
    if (g_shared == nullptr || slot_index >= BioseqSchedulerShared::kCompileQueueSize) {
        return;
    }
    ShaderCompileSlot& slot = g_shared->compile_queue[slot_index];
    slot.key = bioseq::gpu::vulkan::ShaderVariantKey{};
    slot.requester_pid = 0;
    slot.errc = static_cast<std::uint32_t>(bioseq::gpu::vulkan::ShaderCacheErrc::Ok);
    pg_atomic_fetch_add_u32(&slot.generation, 1);
    pg_write_barrier();
    pg_atomic_write_u32(
        &slot.state,
        static_cast<std::uint32_t>(CompileSlotState::Free));
}

[[nodiscard]] bool try_release_completed_shader_compile_slot(
		const std::size_t slot_index,
		const CompileSlotState completed_state) noexcept {
	if (g_shared == nullptr
		|| slot_index >= BioseqSchedulerShared::kCompileQueueSize
		|| (completed_state != CompileSlotState::DoneOk
			&& completed_state != CompileSlotState::DoneErr)) {
		return false;
	}
	ShaderCompileSlot& slot = g_shared->compile_queue[slot_index];
	std::uint32_t expected = static_cast<std::uint32_t>(completed_state);
	if (!pg_atomic_compare_exchange_u32(
			&slot.state,
			&expected,
			static_cast<std::uint32_t>(CompileSlotState::Filling))) {
		return false;
	}
	release_shader_compile_slot(slot_index);
	return true;
}

void abandon_shader_compile_request(
		const std::size_t slot_index,
		const std::uint32_t generation) noexcept {
	if (g_shared == nullptr
		|| slot_index >= BioseqSchedulerShared::kCompileQueueSize) {
		return;
	}
	ShaderCompileSlot& slot = g_shared->compile_queue[slot_index];
	const std::uint32_t observed_generation =
		pg_atomic_read_u32(&slot.generation);
	const auto observed_state = static_cast<CompileSlotState>(
		pg_atomic_read_u32(&slot.state));
	std::uint32_t expected = static_cast<std::uint32_t>(
		CompileSlotState::Requested);
	if (observed_generation == generation
		&& observed_state == CompileSlotState::Requested
		&& pg_atomic_compare_exchange_u32(
			&slot.state,
			&expected,
			static_cast<std::uint32_t>(CompileSlotState::Filling))) {
		release_shader_compile_slot(slot_index);
		return;
	}

	if (observed_generation == generation
		&& observed_state == CompileSlotState::Compiling) {
		slot.requester_pid = 0;
		pg_write_barrier();
		const auto observed = static_cast<CompileSlotState>(
			pg_atomic_read_u32(&slot.state));
		if (observed == CompileSlotState::DoneOk
			|| observed == CompileSlotState::DoneErr) {
			(void)try_release_completed_shader_compile_slot(
				slot_index, observed);
		}
		return;
	}
	if (observed_state == CompileSlotState::DoneOk
		|| observed_state == CompileSlotState::DoneErr) {
		slot.requester_pid = 0;
		pg_write_barrier();
		(void)try_release_completed_shader_compile_slot(
			slot_index, observed_state);
	}
}

[[nodiscard]] std::expected<std::pair<std::size_t, std::uint32_t>, std::string>
publish_shader_compile_request(
        const bioseq::gpu::vulkan::ShaderVariantKey& key) {
    if (g_shared == nullptr) {
        return std::unexpected("shader compile queue unavailable");
    }
    for (std::size_t slot_index = 0;
         slot_index < BioseqSchedulerShared::kCompileQueueSize;
         ++slot_index) {
        ShaderCompileSlot& slot = g_shared->compile_queue[slot_index];
        std::uint32_t expected =
            static_cast<std::uint32_t>(CompileSlotState::Free);
        if (!pg_atomic_compare_exchange_u32(
                &slot.state, &expected,
                static_cast<std::uint32_t>(CompileSlotState::Filling))) {
            continue;
        }
        slot.key = key;
        slot.requester_pid = static_cast<std::uint32_t>(MyProcPid);
        slot.errc = static_cast<std::uint32_t>(bioseq::gpu::vulkan::ShaderCacheErrc::Ok);
        const std::uint32_t generation =
            pg_atomic_fetch_add_u32(&slot.generation, 1) + 1u;
        pg_write_barrier();
        pg_atomic_write_u32(
            &slot.state,
            static_cast<std::uint32_t>(CompileSlotState::Requested));
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

struct PgSupervisorCompileService {
    [[nodiscard]] std::expected<void, std::string> ensure_variant_compiled(
            bioseq::gpu::vulkan::ShaderVariantKey key) {
        auto published = publish_shader_compile_request(key);
        if (!published) {
            return std::unexpected(published.error());
        }
        const auto [slot_index, generation] = *published;
        constexpr std::uint64_t kDeadlineNs = 120ull * 1'000'000'000ull;
        const std::uint64_t deadline =
            static_cast<std::uint64_t>(bioseq::instr::clock::now_ns()) + kDeadlineNs;
		bool sleep_prepared = false;
		auto cancel_sleep = [&sleep_prepared] {
			if (sleep_prepared) {
				pg_condition_variable_cancel_sleep_as_cpp();
				sleep_prepared = false;
			}
		};
		try {
			pg_condition_variable_prepare_to_sleep_as_cpp(
				&g_shared->compile_done_cv);
			sleep_prepared = true;
			for (;;) {
				pg_check_for_interrupts_as_cpp();
				ShaderCompileSlot& slot = g_shared->compile_queue[slot_index];
				const auto state = static_cast<CompileSlotState>(
					pg_atomic_read_u32(&slot.state));
				const std::uint32_t observed_gen =
					pg_atomic_read_u32(&slot.generation);
				if ((state == CompileSlotState::DoneOk
						|| state == CompileSlotState::DoneErr)
					&& observed_gen != generation) {
					pg_read_barrier();
					const std::uint32_t errc = slot.errc;
					cancel_sleep();
					(void)try_release_completed_shader_compile_slot(
						slot_index, state);
					if (state == CompileSlotState::DoneOk) {
						return {};
					}
					return std::unexpected(fmt::format(
						"shader compile failed for {} (errc {})",
						bioseq::gpu::vulkan::cache_file_stem(key), errc));
				}
				if (static_cast<std::uint64_t>(bioseq::instr::clock::now_ns())
					> deadline) {
					const std::string diagnostic = fmt::format(
						"shader compile timed out for {}",
						bioseq::gpu::vulkan::cache_file_stem(key));
					cancel_sleep();
					abandon_shader_compile_request(slot_index, generation);
					return std::unexpected(diagnostic);
				}
				pg_condition_variable_timed_sleep_as_cpp(
					&g_shared->compile_done_cv, 1000L);
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
    const std::string digest = bioseq::gpu::vulkan::shader_cache_digest_hex(inputs);
    const std::filesystem::path cache_dir = runtime_cache_dir_from_digest(digest);
    auto keys = exact_aot_shader_keys(inputs);
    if (!keys) {
        return std::unexpected(fmt::format(
            "canonicalize exact AOT shader keys: {}", keys.error()));
    }
    const std::filesystem::path manifest_path =
        bioseq::gpu::vulkan::sealed_shader_cache_manifest_path(cache_dir);
    std::error_code ec;
    const bool manifest_exists = std::filesystem::exists(manifest_path, ec);
    if (ec) {
        return std::unexpected(fmt::format(
            "stat sealed shader cache manifest {} failed: {}",
            manifest_path.string(), ec.message()));
    }
    if (manifest_exists) {
        auto seed = load_process_shader_seed_once(cache_dir, digest, *keys);
        if (!seed) {
            return std::unexpected(fmt::format(
                "existing sealed shader cache is invalid and will not be repaired: {}",
                seed.error()));
        }
        if (auto installed = install_verified_runtime_archive_configuration(
                *seed);
            !installed) {
            return std::unexpected(installed.error());
        }
        if (auto published = publish_runtime_cache_env(cache_dir); !published) {
            return std::unexpected(published.error());
        }
        return cache_dir;
    }

    for (const auto& key : *keys) {
        auto compiled = bioseq::gpu::vulkan::ensure_compiled_for_sealed_seed(
            cache_dir, key, inputs.msl);
        if (!compiled) {
            return std::unexpected(fmt::format(
                "backend-local exact AOT compile {}: {}",
                bioseq::gpu::vulkan::cache_file_stem(key),
                compiled.error()));
        }
    }
    auto sealed = bioseq::gpu::vulkan::seal_shader_cache_manifest(
        cache_dir, digest, *keys);
    if (!sealed) {
        return std::unexpected(fmt::format(
            "seal backend-local exact shader AOT seed: {}", sealed.error()));
    }
    auto seed = load_process_shader_seed_once(cache_dir, digest, *keys);
    if (!seed) {
        return std::unexpected(fmt::format(
            "load newly sealed backend-local shader seed: {}", seed.error()));
    }
    if (auto installed = install_verified_runtime_archive_configuration(
            *seed);
        !installed) {
        return std::unexpected(installed.error());
    }
    if (auto published = publish_runtime_cache_env(cache_dir); !published) {
        return std::unexpected(published.error());
    }
    return cache_dir;
}

struct PgBackendLocalCompileService {
    std::filesystem::path cache_dir;
    bioseq::gpu::vulkan::MslVersion msl;

    [[nodiscard]] std::expected<void, std::string> ensure_variant_compiled(
            bioseq::gpu::vulkan::ShaderVariantKey key) {
        return bioseq::gpu::vulkan::ensure_compiled(cache_dir, key, msl);
    }
};

[[nodiscard]] std::string warmup_backend_local_shader_cache_report() {
    std::ostringstream out;
#if !defined(__APPLE__)
    out << "pid=" << static_cast<long>(MyProcPid)
        << " platform=non-apple\n"
        << "RESULT: ok variants=0 failed=0\n";
    return out.str();
#else
    const auto inputs = conservative_shader_inputs();
    auto prepared = prepare_backend_local_shader_cache_env();
    if (!prepared) {
        out << "pid=" << static_cast<long>(MyProcPid) << '\n'
            << "RESULT: failed\n"
            << "error=" << prepared.error() << '\n';
        return out.str();
    }

    const std::filesystem::path& cache_dir = *prepared;
    out << "pid=" << static_cast<long>(MyProcPid)
        << " supervisor=" << (g_shared != nullptr ? 1 : 0)
        << " cache_dir=" << cache_dir.string() << '\n';
    if (const char* env = std::getenv("BIOSEQ_MOLTENVK_COMMAND_METALLIB");
        env != nullptr) {
        out << "env BIOSEQ_MOLTENVK_COMMAND_METALLIB=" << env << '\n';
    }
    if (const char* env = std::getenv("BIOSEQ_MOLTENVK_BINARY_ARCHIVE_DIR");
        env != nullptr) {
        out << "env BIOSEQ_MOLTENVK_BINARY_ARCHIVE_DIR=" << env << '\n';
    }

    auto keys = exact_aot_shader_keys(inputs);
    if (!keys) {
        out << "RESULT: failed\n"
            << "error=canonicalize exact AOT shader keys: "
            << keys.error() << '\n';
        return out.str();
    }
    const std::string digest = cache_dir.filename().string();
    const auto seed = process_shader_seed_snapshot();
    if (!seed || !seed->accepted()
        || seed->manifest().cache_digest != digest
        || seed->manifest().keys != *keys) {
        out << "RESULT: failed\n"
            << "error=process-owned shader seed unavailable after preparation\n";
        return out.str();
    }
    const auto& sealed = seed->manifest();
    out << "sealed_manifest="
        << bioseq::gpu::vulkan::sealed_shader_cache_manifest_path(cache_dir).string()
        << " seed_bytes=" << sealed.seed_size
        << " manifest_checksum=" << sealed.manifest_checksum
        << " seed_load_count=" << seed->receipt().load_count()
        << " seed_load_ns=" << seed->receipt().load_ns() << '\n';

    std::size_t failed = 0;
    for (const auto& key : *keys) {
        const std::string stem = bioseq::gpu::vulkan::cache_file_stem(key);
        auto compatible =
            bioseq::gpu::vulkan::sealed_shader_cache_key_view(*seed, key);
        if (!compatible) {
            ++failed;
            out << "variant " << stem
                << " kind=" << key.kind
                << " tile=" << key.tile_size
                << " local=" << key.local_size
                << " max_span=" << key.max_span
                << " status=failed error=" << compatible.error() << '\n';
            continue;
        }
        const auto key_iterator = std::find(
            sealed.keys.begin(), sealed.keys.end(), key);
        const std::uint32_t key_index = static_cast<std::uint32_t>(
            std::distance(sealed.keys.begin(), key_iterator));
        const auto* metallib = bioseq::gpu::vulkan::
            sealed_shader_cache_detail::find_artifact_image(
                seed->artifacts(),
                bioseq::gpu::vulkan::SealedShaderCacheArtifactKind::
                    shader_metallib,
                key_index, stem + ".metallib");
        const auto* meta = bioseq::gpu::vulkan::
            sealed_shader_cache_detail::find_artifact_image(
                seed->artifacts(),
                bioseq::gpu::vulkan::SealedShaderCacheArtifactKind::shader_meta,
                key_index, stem + ".meta");
        std::ostringstream archive_bytes;
        bool first_archive = true;
        for (const auto& archive : seed->artifacts()) {
            if (archive.descriptor.key_index != key_index
                || archive.descriptor.kind
                    != bioseq::gpu::vulkan::SealedShaderCacheArtifactKind::
                        metal_binary_archive) {
                continue;
            }
            if (!first_archive) {
                archive_bytes << ',';
            }
            first_archive = false;
            archive_bytes << archive.descriptor.filename << ':'
                << archive.bytes.size();
        }
        out << "variant " << stem
            << " kind=" << key.kind
            << " tile=" << key.tile_size
            << " local=" << key.local_size
            << " max_span=" << key.max_span
            << " status=ok"
            << " metallib_bytes=" << (metallib ? metallib->bytes.size() : 0u)
            << " meta_bytes=" << (meta ? meta->bytes.size() : 0u)
            << " archive_bytes=" << archive_bytes.str()
            << '\n';
    }
    out << "RESULT: " << (failed == 0 ? "ok" : "failed")
        << " variants=" << keys->size()
        << " failed=" << failed << '\n';
    return out.str();
#endif
}

[[nodiscard]] std::expected<void, std::string>
pg_shader_provider_prepare_runtime() {
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

[[nodiscard]] std::expected<bioseq::gpu::vulkan::ShaderModuleLoad, std::string>
pg_shader_provider_load_module(
        VkDevice device,
        const bioseq::gpu::vulkan::ShaderVariantKey& key,
    PFN_vkCreateShaderModule create_shader_module) {
    const auto inputs = conservative_shader_inputs();
    auto keys = exact_aot_shader_keys(inputs);
    if (!keys) {
        return std::unexpected(fmt::format(
            "canonicalize exact AOT shader keys: {}", keys.error()));
    }
    if (g_shared != nullptr) {
        wait_for_runtime_ready();
    } else if (!process_shader_seed_snapshot()) {
        auto prepared = prepare_backend_local_shader_cache_env();
        if (!prepared) return std::unexpected(prepared.error());
    }
    const auto seed = process_shader_seed_snapshot();
    if (!seed || !seed->accepted()
        || seed->manifest().keys != *keys) {
        return std::unexpected(
            "process-owned sealed shader seed is unavailable or incompatible");
    }
    return bioseq::gpu::vulkan::load_sealed_cached_module(
        device, *seed, key, create_shader_module);
}

void install_pg_shader_provider_if_available() noexcept {
#if defined(__APPLE__)
    if (process_shared_preload_libraries_in_progress
        || is_standalone_backend_process()) {
        return;
    }
    static bioseq::gpu::vulkan::RuntimeShaderProvider provider{
        &pg_shader_provider_prepare_runtime,
        &pg_shader_provider_load_module};
    bioseq::gpu::vulkan::install_runtime_shader_provider(&provider);
#endif
}

/**
 * Prepare PostgreSQL process state before constructing/accessing the
 * process-local Vulkan Runtime singleton.
 *
 * MoltenVK consumes the command-metallib and binary-archive environment when
 * the singleton creates its device/pipeline state.  Installing the PostgreSQL
 * shader provider or publishing the digest-derived cache directory afterwards
 * is too late for that backend process.  Keep this ordering in one exported
 * function so status/diagnostic SQL cannot poison a later selected GPU scan.
 *
 * Every PostgreSQL-capable operation below the API is an invoke_pg_as_cpp leaf.
 * Consequently this function may throw PostgresErrorAsCpp, but only after that
 * leaf has completed PG_END_TRY.  Callers must be ordinary C++ orchestration or
 * catch at a PostgreSQL callback/UDF boundary after abandoning their C++ state.
 */
void prepare_pg_vulkan_runtime_ready() {
    install_pg_shader_provider_if_available();
    wait_for_runtime_ready();
}

VulkanRuntimeStatusSnapshot vulkan_runtime_status_snapshot() {
    prepare_pg_vulkan_runtime_ready();
    auto& runtime = bioseq::gpu::vulkan::Runtime::instance();
    const auto& features = runtime.features();
    return VulkanRuntimeStatusSnapshot{
        .available = features.available,
        .device_name = std::string{features.device_name.data()},
        .max_workgroup_count_x = static_cast<std::int32_t>(
            std::min<std::uint32_t>(
                features.max_workgroup_count_x,
                static_cast<std::uint32_t>(
                    std::numeric_limits<std::int32_t>::max()))),
        .scalar_block_layout = features.scalar_block_layout,
        .initialization_error = runtime.initialization_error(),
        .device_local_bytes = static_cast<std::int64_t>(
            std::min<std::uint64_t>(
                features.device_local_bytes,
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()))),
        .software_device = features.software_device,
    };
}

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
        auto compiled = bioseq::gpu::vulkan::ensure_compiled(
            cache_dir, claim->key, inputs.msl);
        finish_shader_compile_slot(
            *claim,
            compiled
                ? bioseq::gpu::vulkan::ShaderCacheErrc::Ok
                : classify_shader_cache_error(compiled.error()),
            compiled.has_value());
        ++done;
    }
    if (done > 0) {
        ConditionVariableBroadcast(&g_shared->compile_done_cv);
    }
}
} // namespace bioseq::parallelism::pg
