/**
 * @file cpu_preparation.cppm
 * @brief Implements typmod-specific admitted CPU query preparation.
 */
module;

#include <bioseq/pg_compat.h>
#include <bioseq/pg_build_profile.h>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

module bioseq.postgres.scheduler.dispatch_processor:cpu_preparation;

import bioseq.postgres.scheduler.dispatch_processor;
import bioseq.common.checked_arithmetic;
import bioseq.common.seqan_adapter;
import bioseq.highway.dispatch_contract;
import bioseq.postgres.support;
import bioseq.indexing.type;
import bioseq.indexing.traits;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.pipeline.pipe;
import bioseq.postgres.types.compact_sequence;
import bioseq.storage.lazy_sequence;
import bioseq.storage.scanner;
import bioseq.postgres.storage.scanner;
import bioseq.parallelism.concepts;
import bioseq.parallelism.memory_admission;

namespace bioseq::simd::cpu_preparation_detail {

/**
 * @brief Collects canonical query hashes with optional retained sketches.
 *
 * Hash-major preparation fans one sketch pass into the authenticated canonical
 * u32 union and, when separately admitted, the full per-query minimizer vector
 * later consumed by alignment.  This removes the typed row path's second
 * sketch without introducing a Q-sized conversion owner.
 *
 * The collector deliberately exposes the same small surface used by the
 * scalar and two_stack_nthash kernels.  Capacity remains charged to the prepared
 * target's aggregate permits, including vector reallocation overlap.  Retained
 * per-query growth is best-effort; canonical collection remains fail-closed.
 */
class admitted_canonical_u32_query_collector {
public:
	using minimizer_type = minimap_sketch_data;

	admitted_canonical_u32_query_collector(
			std::vector<std::uint32_t>& destination,
			bioseq::parallelism::memory_permit& permit,
			std::uint64_t& retained_bytes,
			std::vector<minimap_sketch_data>* retained_query = nullptr,
			bioseq::parallelism::memory_permit*
				retained_query_permit = nullptr,
			std::uint64_t* retained_query_bytes = nullptr) noexcept
		: destination_(destination),
		  permit_(permit),
		  retained_bytes_(retained_bytes),
		  retained_query_(retained_query),
		  retained_query_permit_(retained_query_permit),
		  retained_query_bytes_(retained_query_bytes),
		  retention_complete_(
			retained_query != nullptr
			&& retained_query_permit != nullptr
			&& retained_query_bytes != nullptr) {}

	/** @return The number of canonical hashes collected so far. */
	[[nodiscard]] std::size_t size() const noexcept {
		return destination_.size();
	}

	/** @return True when canonical capacity for `required` hashes was admitted. */
	[[nodiscard]] bool reserve(const std::size_t required) {
		return ensure_capacity(required);
	}

	/** @brief Appends one copied minimizer to both enabled representations. */
	[[always_inline]] void collect(const minimap_sketch_data& minimizer) {
		append_hash(minimizer.kmer());
		append_retained(minimizer);
	}

	/** @brief Appends one moved minimizer to both enabled representations. */
	[[always_inline]] void collect(minimap_sketch_data&& minimizer) {
		append_hash(minimizer.kmer());
		append_retained(std::move(minimizer));
	}

	/** @return True when a minimizer range was appended without admission failure. */
	[[nodiscard, always_inline]] bool append_records(
			const std::span<const minimap_sketch_data> records) {
		const std::size_t old_size = destination_.size();
		if (records.size() > destination_.max_size() - old_size
			|| !ensure_capacity(old_size + records.size())) {
			failed_ = true;
			return false;
		}
		for (const minimap_sketch_data& record : records) {
			const std::uint64_t hash = record.kmer();
			hash_width_valid_ = hash_width_valid_
				&& hash <= (std::numeric_limits<std::uint32_t>::max)();
			destination_.push_back(static_cast<std::uint32_t>(hash));
		}
		append_retained_records(records);
		return true;
	}

	/** @return True when raw minimizer storage was decoded and appended. */
	template<class p_minimizer = minimap_sketch_data>
	[[nodiscard, always_inline]] bool append_raw_storage(
			const typename p_minimizer::storage_type* raw,
			const std::size_t count)
		requires (
			std::same_as<p_minimizer, minimap_sketch_data>
			&& requires { typename p_minimizer::storage_type; }
			&& std::is_trivially_copyable_v<p_minimizer>
			&& sizeof(p_minimizer) == sizeof(typename p_minimizer::storage_type))
	{
		const std::size_t old_size = destination_.size();
		if (count > destination_.max_size() - old_size
			|| !ensure_capacity(old_size + count)) {
			failed_ = true;
			return false;
		}
		for (std::size_t index = 0; index < count; ++index) {
			const std::uint64_t hash =
				minimap_sketch_data::from_raw_storage(raw[index]).kmer();
			hash_width_valid_ =
				hash_width_valid_
				&& hash
					<= (std::numeric_limits<std::uint32_t>::max)();
			destination_.push_back(static_cast<std::uint32_t>(hash));
		}
		append_retained_raw(raw, count);
		return true;
	}

	/** @return True when canonical collection has not failed. */
	[[nodiscard]] bool is_ok() const noexcept {
		return !failed_;
	}

	/** @return True when every hash fits the authenticated 32-bit profile. */
	[[nodiscard]] bool hash_width_valid() const noexcept {
		return hash_width_valid_;
	}

	/** @return True when the optional retained sketch stayed fully admitted. */
	[[nodiscard]] bool retention_complete() const noexcept {
		return retention_complete_;
	}

private:
	/** @brief Appends one checked canonical hash. */
	[[always_inline]] void append_hash(const std::uint64_t hash) {
		hash_width_valid_ =
			hash_width_valid_
			&& hash <= (std::numeric_limits<std::uint32_t>::max)();
		if (destination_.size() == destination_.max_size()
			|| !ensure_capacity(destination_.size() + 1u)) {
			failed_ = true;
			return;
		}
		destination_.push_back(static_cast<std::uint32_t>(hash));
	}

	/** @brief Releases optional retained storage after a best-effort failure. */
	void abandon_retention() noexcept {
		if (!retention_complete_
			|| retained_query_ == nullptr
			|| retained_query_permit_ == nullptr
			|| retained_query_bytes_ == nullptr) {
			retention_complete_ = false;
			return;
		}
		const std::uint64_t released_bytes =
			bioseq::common::saturating_mul(
				retained_query_->capacity(),
				sizeof(minimap_sketch_data));
		{
			std::vector<minimap_sketch_data> empty;
			retained_query_->swap(empty);
		}
		if (released_bytes
				== (std::numeric_limits<std::uint64_t>::max)()
			|| *retained_query_bytes_ < released_bytes) {
			/* The caller's final measured-capacity reconciliation remains the
			 * correctness authority.  Retain the current permit reservation
			 * on impossible accounting instead of releasing unowned bytes. */
			retention_complete_ = false;
			return;
		}
		*retained_query_bytes_ -= released_bytes;
		retained_query_permit_->shrink_to(*retained_query_bytes_);
		retention_complete_ = false;
	}

	/** @return True when optional retained-sketch capacity is available. */
	[[nodiscard]] bool ensure_retained_capacity(
			const std::size_t required) {
		if (!retention_complete_) return false;
		if (required <= retained_query_->capacity()) return true;
		if (required > retained_query_->max_size()) {
			abandon_retention();
			return false;
		}

		const std::size_t old_capacity = retained_query_->capacity();
		std::size_t predecessor_capacity = old_capacity;
		std::size_t requested_capacity = 8u;
		if (old_capacity != 0u) {
			requested_capacity =
				old_capacity <= retained_query_->max_size() / 2u
				? old_capacity * 2u
				: required;
		}
		/* A coarse exact-representation batch must expose the same last
		 * predecessor-plus-replacement admission peak as sequential appends,
		 * without making every intermediate allocation. */
		while (requested_capacity < required) {
			predecessor_capacity = requested_capacity;
			if (requested_capacity
					> retained_query_->max_size() / 2u) {
				requested_capacity = required;
				break;
			}
			requested_capacity *= 2u;
		}
		requested_capacity = std::max(requested_capacity, required);
		const std::uint64_t old_bytes =
			bioseq::common::saturating_mul(
				old_capacity, sizeof(minimap_sketch_data));
		const std::uint64_t predecessor_bytes =
			bioseq::common::saturating_mul(
				predecessor_capacity, sizeof(minimap_sketch_data));
		const std::uint64_t requested_bytes =
			bioseq::common::saturating_mul(
				requested_capacity, sizeof(minimap_sketch_data));
		const std::uint64_t allocation_headroom =
			bioseq::common::saturating_mul(
				requested_bytes,
				bioseq::parallelism::
					k_vector_preallocation_headroom_factor);
		const std::uint64_t retained_other_bytes =
			*retained_query_bytes_ >= old_bytes
			? *retained_query_bytes_ - old_bytes
			: (std::numeric_limits<std::uint64_t>::max)();
		const std::uint64_t logical_predecessor_peak =
			bioseq::common::saturating_add(
				retained_other_bytes, predecessor_bytes);
		const std::uint64_t peak =
			bioseq::common::saturating_add(
				logical_predecessor_peak, allocation_headroom);
		if (old_bytes == (std::numeric_limits<std::uint64_t>::max)()
			|| predecessor_bytes
				== (std::numeric_limits<std::uint64_t>::max)()
			|| requested_bytes
				== (std::numeric_limits<std::uint64_t>::max)()
			|| allocation_headroom
				== (std::numeric_limits<std::uint64_t>::max)()
			|| logical_predecessor_peak
				== (std::numeric_limits<std::uint64_t>::max)()
			|| peak == (std::numeric_limits<std::uint64_t>::max)()
			|| *retained_query_bytes_ < old_bytes
			|| !retained_query_permit_->grow_to(peak)) {
			abandon_retention();
			return false;
		}
		try {
			retained_query_->reserve(requested_capacity);
		} catch (const std::bad_alloc&) {
			retained_query_permit_->shrink_to(*retained_query_bytes_);
			abandon_retention();
			return false;
		} catch (...) {
			retained_query_permit_->shrink_to(*retained_query_bytes_);
			throw;
		}
		const std::uint64_t measured_bytes =
			bioseq::common::saturating_mul(
				retained_query_->capacity(),
				sizeof(minimap_sketch_data));
		if (measured_bytes
				== (std::numeric_limits<std::uint64_t>::max)()
			|| measured_bytes > allocation_headroom) {
			{
				std::vector<minimap_sketch_data> empty;
				retained_query_->swap(empty);
			}
			*retained_query_bytes_ -= old_bytes;
			retained_query_permit_->shrink_to(*retained_query_bytes_);
			retention_complete_ = false;
			return false;
		}
		const std::uint64_t reconciled_bytes =
			bioseq::common::saturating_add(
				*retained_query_bytes_ - old_bytes,
				measured_bytes);
		if (reconciled_bytes
				== (std::numeric_limits<std::uint64_t>::max)()) {
			{
				std::vector<minimap_sketch_data> empty;
				retained_query_->swap(empty);
			}
			*retained_query_bytes_ -= old_bytes;
			retained_query_permit_->shrink_to(*retained_query_bytes_);
			retention_complete_ = false;
			return false;
		}
		*retained_query_bytes_ = reconciled_bytes;
		retained_query_permit_->shrink_to(*retained_query_bytes_);
		return true;
	}

	template<class p_value>
	[[always_inline]] void append_retained(p_value&& minimizer) {
		if (!retention_complete_) return;
		if (retained_query_->size() == retained_query_->max_size()
			|| !ensure_retained_capacity(retained_query_->size() + 1u)) {
			return;
		}
		retained_query_->push_back(std::forward<p_value>(minimizer));
	}

	template<class p_storage>
	[[always_inline]] void append_retained_raw(
			const p_storage* raw,
			const std::size_t count) {
		if (!retention_complete_) return;
		const std::size_t old_size = retained_query_->size();
		if (count > retained_query_->max_size() - old_size
			|| !ensure_retained_capacity(old_size + count)) {
			return;
		}
		for (std::size_t index = 0u; index < count; ++index) {
			retained_query_->push_back(
				minimap_sketch_data::from_raw_storage(raw[index]));
		}
	}

	[[always_inline]] void append_retained_records(
			const std::span<const minimap_sketch_data> records) {
		if (!retention_complete_) return;
		const std::size_t old_size = retained_query_->size();
		if (records.size() > retained_query_->max_size() - old_size
			|| !ensure_retained_capacity(old_size + records.size())) {
			return;
		}
		retained_query_->insert(
			retained_query_->end(), records.begin(), records.end());
	}

	/** @return True when canonical destination capacity is admitted and available. */
	[[nodiscard]] bool ensure_capacity(const std::size_t required) {
		if (failed_) return false;
		if (required <= destination_.capacity()) return true;
		if (required > destination_.max_size()) {
			failed_ = true;
			return false;
		}

		const std::size_t old_capacity = destination_.capacity();
		std::size_t predecessor_capacity = old_capacity;
		std::size_t requested_capacity = 8u;
		if (old_capacity != 0u) {
			requested_capacity =
				old_capacity <= destination_.max_size() / 2u
				? old_capacity * 2u
				: required;
		}
		/* Preserve the established sequential geometric admission cliff for
		 * coarse exact-representation batches while allocating only once. */
		while (requested_capacity < required) {
			predecessor_capacity = requested_capacity;
			if (requested_capacity > destination_.max_size() / 2u) {
				requested_capacity = required;
				break;
			}
			requested_capacity *= 2u;
		}
		requested_capacity = std::max(requested_capacity, required);

		const std::uint64_t old_bytes =
			bioseq::common::saturating_mul(
				old_capacity, sizeof(std::uint32_t));
		const std::uint64_t predecessor_bytes =
			bioseq::common::saturating_mul(
				predecessor_capacity, sizeof(std::uint32_t));
		const std::uint64_t requested_bytes =
			bioseq::common::saturating_mul(
				requested_capacity, sizeof(std::uint32_t));
		const std::uint64_t allocation_headroom =
			bioseq::common::saturating_mul(
				requested_bytes,
				bioseq::parallelism::
					k_vector_preallocation_headroom_factor);
		const std::uint64_t retained_other_bytes =
			retained_bytes_ >= old_bytes
			? retained_bytes_ - old_bytes
			: (std::numeric_limits<std::uint64_t>::max)();
		const std::uint64_t logical_predecessor_peak =
			bioseq::common::saturating_add(
				retained_other_bytes, predecessor_bytes);
		const std::uint64_t peak =
			bioseq::common::saturating_add(
				logical_predecessor_peak, allocation_headroom);
		if (old_bytes == (std::numeric_limits<std::uint64_t>::max)()
			|| predecessor_bytes
				== (std::numeric_limits<std::uint64_t>::max)()
			|| requested_bytes
				== (std::numeric_limits<std::uint64_t>::max)()
			|| allocation_headroom
				== (std::numeric_limits<std::uint64_t>::max)()
			|| logical_predecessor_peak
				== (std::numeric_limits<std::uint64_t>::max)()
			|| peak == (std::numeric_limits<std::uint64_t>::max)()
			|| retained_bytes_ < old_bytes
			|| !permit_.grow_to(peak)) {
			failed_ = true;
			return false;
		}

		try {
			destination_.reserve(requested_capacity);
		} catch (...) {
			permit_.shrink_to(retained_bytes_);
			throw;
		}
		const std::uint64_t measured_bytes =
			bioseq::common::saturating_mul(
				destination_.capacity(), sizeof(std::uint32_t));
		if (measured_bytes
				== (std::numeric_limits<std::uint64_t>::max)()
			|| measured_bytes > allocation_headroom) {
			failed_ = true;
			return false;
		}
		retained_bytes_ =
			bioseq::common::saturating_add(
				retained_bytes_ - old_bytes, measured_bytes);
		if (retained_bytes_
				== (std::numeric_limits<std::uint64_t>::max)()) {
			failed_ = true;
			return false;
		}
		permit_.shrink_to(retained_bytes_);
		return true;
	}

	std::vector<std::uint32_t>& destination_;
	bioseq::parallelism::memory_permit& permit_;
	std::uint64_t& retained_bytes_;
	std::vector<minimap_sketch_data>* retained_query_ = nullptr;
	bioseq::parallelism::memory_permit*
		retained_query_permit_ = nullptr;
	std::uint64_t* retained_query_bytes_ = nullptr;
	bool failed_ = false;
	bool hash_width_valid_ = true;
	bool retention_complete_ = false;
};

/** @brief Releases an uncommitted retained query and shrinks its permit. */
void release_uncommitted_retained_query(
		std::vector<minimap_sketch_data>& minimizers,
		bioseq::parallelism::memory_permit& permit,
		std::uint64_t& retained_bytes) noexcept {
	const std::uint64_t released_bytes =
		bioseq::common::saturating_mul(
			minimizers.capacity(), sizeof(minimap_sketch_data));
	{
		std::vector<minimap_sketch_data> empty;
		minimizers.swap(empty);
	}
	if (released_bytes
			== (std::numeric_limits<std::uint64_t>::max)()
		|| retained_bytes < released_bytes) {
		return;
	}
	retained_bytes -= released_bytes;
	permit.shrink_to(retained_bytes);
}

/** @return True when one retained query sketch was admitted and committed. */
[[nodiscard]] bool try_commit_retained_query_sketch(
		const std::uint64_t query_ordinal,
		const std::uint64_t query_bp_size,
		std::vector<minimap_sketch_data>&& minimizers,
		pipe::minimizer_range_execution_stats execution_stats,
		std::vector<batch_processor::cpu_prepared_query_sketch>& retained_queries,
		bioseq::parallelism::memory_permit& permit,
		std::uint64_t& retained_bytes) {
	if (!retained_queries.empty()
		&& retained_queries.back().query_ordinal >= query_ordinal) {
		bioseq::postgres::support::invoke_pg_as_cpp([] {
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("bioseq batch join retained query ordinals are not strictly ordered")));
		});
		pg_unreachable();
	}
	if (retained_queries.size() == retained_queries.max_size()) {
		release_uncommitted_retained_query(
			minimizers, permit, retained_bytes);
		return false;
	}
	if (retained_queries.size() == retained_queries.capacity()) {
		const std::size_t required = retained_queries.size() + 1u;
		const std::uint64_t old_bytes =
			bioseq::common::saturating_mul(
				retained_queries.capacity(),
				sizeof(batch_processor::cpu_prepared_query_sketch));
		const std::uint64_t requested_bytes =
			bioseq::common::saturating_mul(
				required,
				sizeof(batch_processor::cpu_prepared_query_sketch));
		const std::uint64_t allocation_headroom =
			bioseq::common::saturating_mul(
				requested_bytes,
				bioseq::parallelism::
					k_vector_preallocation_headroom_factor);
		const std::uint64_t peak =
			bioseq::common::saturating_add(
				retained_bytes, allocation_headroom);
		if (old_bytes == (std::numeric_limits<std::uint64_t>::max)()
			|| requested_bytes
				== (std::numeric_limits<std::uint64_t>::max)()
			|| allocation_headroom
				== (std::numeric_limits<std::uint64_t>::max)()
			|| peak == (std::numeric_limits<std::uint64_t>::max)()
			|| retained_bytes < old_bytes
			|| !permit.grow_to(peak)) {
			release_uncommitted_retained_query(
				minimizers, permit, retained_bytes);
			return false;
		}
		try {
			retained_queries.reserve(required);
		} catch (const std::bad_alloc&) {
			permit.shrink_to(retained_bytes);
			release_uncommitted_retained_query(
				minimizers, permit, retained_bytes);
			return false;
		} catch (...) {
			permit.shrink_to(retained_bytes);
			throw;
		}
		const std::uint64_t measured_bytes =
			bioseq::common::saturating_mul(
				retained_queries.capacity(),
				sizeof(batch_processor::cpu_prepared_query_sketch));
		if (measured_bytes
				== (std::numeric_limits<std::uint64_t>::max)()
			|| measured_bytes > allocation_headroom) {
			bioseq::postgres::support::invoke_pg_as_cpp([] {
				ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("bioseq batch join retained query cache capacity exceeded admission")));
			});
			pg_unreachable();
		}
		retained_bytes =
			bioseq::common::saturating_add(
				retained_bytes - old_bytes, measured_bytes);
		if (retained_bytes
				== (std::numeric_limits<std::uint64_t>::max)()) {
			bioseq::postgres::support::invoke_pg_as_cpp([] {
				ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("bioseq batch join retained query cache capacity overflow")));
			});
			pg_unreachable();
		}
		permit.shrink_to(retained_bytes);
	}
	retained_queries.push_back(batch_processor::cpu_prepared_query_sketch{
		.query_ordinal = query_ordinal,
		.query_bp_size = query_bp_size,
		.minimizers = std::move(minimizers),
		.execution_stats = std::move(execution_stats),
	});
	return true;
}

/** @brief Appends one lazy query view to a prepared sketch representation. */
template <class p_alphabet, class p_destination_value>
void append_cpu_prepared_query_sketch_view(
		const bioseq::storage::lazy_sequence_view<p_alphabet> query_view,
		const std::uint64_t query_ordinal,
		const std::int32_t kmer_size,
		const std::int32_t window_size,
		const minimizer_sketch_backend sketch_backend,
		const ::bioseq::simd_dispatch::highway_kernel_table* highway_kernels,
		std::vector<p_destination_value>& destination,
		bioseq::parallelism::memory_permit& destination_permit,
		std::uint64_t& retained_destination_bytes,
		std::vector<batch_processor::cpu_prepared_query_sketch>*
			retained_queries,
		bioseq::parallelism::memory_permit* retained_query_permit,
		std::uint64_t* retained_query_bytes) {
	static_assert(
		std::is_same_v<p_destination_value, minimap_sketch_data>
		|| std::is_same_v<p_destination_value, std::uint32_t>);
	if (!is_minimizer_query_length_representable<minimap_sketch_data>(
			query_view.bp_size())) {
		bioseq::postgres::support::invoke_pg_as_cpp([] {
			ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("bioseq batch join query exceeds the 28-bit minimizer position domain")));
		});
		pg_unreachable();
	}
	const auto fail = [](const char* message) -> void {
		bioseq::postgres::support::invoke_pg_as_cpp([message] {
			ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY), errmsg("%s", message)));
		});
		pg_unreachable();
	};
	if constexpr (std::is_same_v<p_destination_value, std::uint32_t>) {
		std::vector<minimap_sketch_data> retained_query;
		admitted_canonical_u32_query_collector collector{
			destination,
			destination_permit,
			retained_destination_bytes,
			retained_queries != nullptr
					&& retained_query_permit != nullptr
					&& retained_query_bytes != nullptr
				? &retained_query
				: nullptr,
			retained_query_permit,
			retained_query_bytes};
		auto execution_stats = pipe::minimizer_dispatch_indexed_query_whole<
			minimap_sketch_data>(
			collector,
			query_view,
			static_cast<std::size_t>(window_size),
			static_cast<std::size_t>(kmer_size),
			sketch_backend,
			highway_kernels);
		if (!collector.hash_width_valid()) {
			bioseq::postgres::support::invoke_pg_as_cpp([] {
				ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("hash-major canonical query hash exceeds its authenticated u32 profile")));
			});
			pg_unreachable();
		}
		if (!collector.is_ok()) {
			fail("bioseq batch join canonical query-hash destination growth cannot be admitted");
		}
		if (!pipe::minimizer_range_stats_matches_backend(
				sketch_backend,
				execution_stats,
				/*require_nonempty_vector_work=*/false,
				pipe::exact_simd_range_route::require_continuation)) {
			bioseq::postgres::support::invoke_pg_as_cpp([] {
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("bioseq batch join prepared query sketch stats do not match the selected backend")));
			});
			pg_unreachable();
		}
		if (collector.retention_complete()) {
			(void)try_commit_retained_query_sketch(
				query_ordinal,
				static_cast<std::uint64_t>(query_view.bp_size()),
				std::move(retained_query),
				std::move(execution_stats),
				*retained_queries,
				*retained_query_permit,
				*retained_query_bytes);
		}
		return;
	} else {
	auto collector_permit =
		bioseq::parallelism::memory_admission_controller::instance().try_reserve(
			0u);
	if (!collector_permit) {
		fail("bioseq batch join query sketch collector admission owner is unavailable");
	}
	admitted_sketch_collector<
		minimap_sketch_data,
		bioseq::parallelism::memory_permit> collector{
			std::move(*collector_permit),
			bioseq::parallelism::k_vector_preallocation_headroom_factor};
	const auto execution_stats = pipe::minimizer_dispatch_indexed_query_whole<
		minimap_sketch_data>(
		collector,
		query_view,
		static_cast<std::size_t>(window_size),
		static_cast<std::size_t>(kmer_size),
		sketch_backend,
		highway_kernels);
	if (!collector.is_ok()) {
		fail("bioseq batch join query sketch collector growth cannot be admitted");
	}
	if (!pipe::minimizer_range_stats_matches_backend(
			sketch_backend,
			execution_stats,
			/*require_nonempty_vector_work=*/false,
			pipe::exact_simd_range_route::require_continuation)) {
		bioseq::postgres::support::invoke_pg_as_cpp([] {
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("bioseq batch join prepared query sketch stats do not match the selected backend")));
		});
		pg_unreachable();
	}
	const auto& sketches = collector.extract_sketches_data();
	const std::uint64_t measured_collector_bytes =
		bioseq::common::saturating_mul(
			sketches.capacity(), sizeof(minimap_sketch_data));
	if (measured_collector_bytes
			== (std::numeric_limits<std::uint64_t>::max)()
		|| measured_collector_bytes
			!= collector.admitted_capacity_bytes()) {
		fail("bioseq batch join query sketch collector capacity disagrees with admission");
	}
	if (sketches.size() > destination.max_size() - destination.size()) {
		fail("bioseq batch join query sketch destination size overflow");
	}
	const std::size_t required = destination.size() + sketches.size();
	if (required > destination.capacity()) {
		const std::uint64_t old_destination_bytes =
			bioseq::common::saturating_mul(
				destination.capacity(), sizeof(p_destination_value));
		const std::uint64_t allocation_headroom =
			bioseq::common::saturating_mul(
				bioseq::common::saturating_mul(
					required, sizeof(p_destination_value)),
				bioseq::parallelism::k_vector_preallocation_headroom_factor);
		const std::uint64_t peak = bioseq::common::saturating_add(
			retained_destination_bytes, allocation_headroom);
		if (old_destination_bytes
				== (std::numeric_limits<std::uint64_t>::max)()
			|| allocation_headroom
				== (std::numeric_limits<std::uint64_t>::max)()
			|| peak == (std::numeric_limits<std::uint64_t>::max)()
			|| retained_destination_bytes < old_destination_bytes
			|| !destination_permit.grow_to(peak)) {
			fail("bioseq batch join query sketch destination growth cannot be admitted");
		}
		destination.reserve(required);
		const std::uint64_t measured_destination_bytes =
			bioseq::common::saturating_mul(
				destination.capacity(), sizeof(p_destination_value));
		if (measured_destination_bytes
				== (std::numeric_limits<std::uint64_t>::max)()
			|| measured_destination_bytes > allocation_headroom) {
			fail("bioseq batch join query sketch destination capacity exceeded admission");
		}
		retained_destination_bytes = bioseq::common::saturating_add(
			retained_destination_bytes - old_destination_bytes,
			measured_destination_bytes);
		if (retained_destination_bytes
			== (std::numeric_limits<std::uint64_t>::max)()) {
			fail("bioseq batch join retained query sketch capacity overflow");
		}
		destination_permit.shrink_to(retained_destination_bytes);
	}
	destination.insert(
		destination.end(),
		sketches.begin(), sketches.end());
	}
}

/** @brief Adapts one flat compact query into admitted CPU preparation. */
template <class p_alphabet, std::size_t p_width, class p_destination_value>
void append_cpu_prepared_query_sketch(
		const bioseq::pg_types::seq& header,
		const std::uint64_t query_ordinal,
		const std::int32_t kmer_size,
		const std::int32_t window_size,
		const minimizer_sketch_backend sketch_backend,
		const ::bioseq::simd_dispatch::highway_kernel_table* highway_kernels,
		std::vector<p_destination_value>& destination,
		bioseq::parallelism::memory_permit& destination_permit,
		std::uint64_t& retained_destination_bytes,
		std::vector<batch_processor::cpu_prepared_query_sketch>* retained_queries,
		bioseq::parallelism::memory_permit* retained_query_permit,
		std::uint64_t* retained_query_bytes) {
	using sequence_t = compact_sequence_stdc_instance<
		bioseq::pg_types::seq, p_alphabet, p_width>;
	compact_sequence_range<sequence_t> query{header};
	append_cpu_prepared_query_sketch_view<p_alphabet, p_destination_value>(
		bioseq::storage::make_lazy_sequence_view(query),
		query_ordinal, kmer_size, window_size, sketch_backend, highway_kernels,
		destination, destination_permit, retained_destination_bytes,
		retained_queries, retained_query_permit, retained_query_bytes);
}


/** @brief Adapts one recipe-backed query into admitted CPU preparation. */
template <class p_alphabet, class p_destination_value>
void append_cpu_prepared_query_recipe_sketch(
		const Datum recipe,
		const pg_sequence_descriptor& descriptor,
		pg_sequence_read_session& session,
		const bioseq::storage::sequence_payload_allocation_observer*
			payload_observer,
		const std::uint64_t query_ordinal,
		const std::int32_t kmer_size,
		const std::int32_t window_size,
		const minimizer_sketch_backend sketch_backend,
		const ::bioseq::simd_dispatch::highway_kernel_table* highway_kernels,
		std::vector<p_destination_value>& destination,
		bioseq::parallelism::memory_permit& destination_permit,
		std::uint64_t& retained_destination_bytes,
		std::vector<batch_processor::cpu_prepared_query_sketch>*
			retained_queries,
		bioseq::parallelism::memory_permit* retained_query_permit,
		std::uint64_t* retained_query_bytes) {
	auto* owner = bioseq::postgres::support::invoke_pg_as_cpp(
		[&session, recipe, descriptor] {
			return ::make_pg_sequence_call_owner<p_alphabet>(
				session, recipe, descriptor);
		});
	append_cpu_prepared_query_sketch_view<p_alphabet, p_destination_value>(
		owner->view().with_payload_allocation_observer(payload_observer),
		query_ordinal, kmer_size, window_size,
		sketch_backend, highway_kernels,
		destination, destination_permit,
		retained_destination_bytes, retained_queries,
		retained_query_permit, retained_query_bytes);
}

/** @return The terminal state of independently admitted query preparation. */
template <class p_alphabet, std::size_t p_width>
[[nodiscard]] batch_processor::cpu_preparation_status
prepare_independently_admitted_query_sketch(
		const bioseq::pg_types::seq& header,
		const std::uint64_t query_ordinal,
		const std::int32_t kmer_size,
		const std::int32_t window_size,
		const minimizer_sketch_backend sketch_backend,
		const ::bioseq::simd_dispatch::highway_kernel_table* highway_kernels,
		batch_processor::cpu_prepared_query_sketch& destination) {
	using sequence_t = compact_sequence_stdc_instance<
		bioseq::pg_types::seq, p_alphabet, p_width>;
	compact_sequence_range<sequence_t> query{header};
	auto query_owner = bioseq::storage::make_resident_lazy_sequence(query);
	const auto query_view =
		bioseq::storage::make_lazy_sequence_view(query_owner);
	if (!is_minimizer_query_length_representable<minimap_sketch_data>(
			static_cast<bioseq::storage::sequence_size_t>(query.bp_size()))) {
		return batch_processor::cpu_preparation_status::query_length_unsupported;
	}
	auto permit =
		bioseq::parallelism::memory_admission_controller::instance().try_reserve(
			0u);
	if (!permit) {
		return batch_processor::cpu_preparation_status::admission_unavailable;
	}
	admitted_sketch_collector<
		minimap_sketch_data,
		bioseq::parallelism::memory_permit> collector{
			std::move(*permit),
			bioseq::parallelism::k_vector_preallocation_headroom_factor};
	auto execution_stats = pipe::minimizer_dispatch_indexed_query_whole<
		minimap_sketch_data>(
		collector,
		query_view,
		static_cast<std::size_t>(window_size),
		static_cast<std::size_t>(kmer_size),
		sketch_backend,
		highway_kernels);
	if (!collector.is_ok()) {
		return batch_processor::cpu_preparation_status::collector_growth_failed;
	}
	if (!pipe::minimizer_range_stats_matches_backend(
			sketch_backend,
			execution_stats,
			/*require_nonempty_vector_work=*/false,
			pipe::exact_simd_range_route::require_continuation)) {
		return batch_processor::cpu_preparation_status::validation_failed;
	}
	auto owned = std::move(collector).release_owned_storage();
	const std::uint64_t measured_bytes =
		bioseq::common::saturating_mul(
			owned.first.capacity(), sizeof(minimap_sketch_data));
	if (measured_bytes
			== (std::numeric_limits<std::uint64_t>::max)()
		|| measured_bytes != owned.second.bytes()) {
		return batch_processor::cpu_preparation_status::capacity_mismatch;
	}
	destination = batch_processor::cpu_prepared_query_sketch{
		.query_ordinal = query_ordinal,
		.query_bp_size = static_cast<std::uint64_t>(query.bp_size()),
		.parallel_preparation_permit =
			std::optional<bioseq::parallelism::memory_permit>{
				std::move(owned.second)},
		.minimizers = std::move(owned.first),
		.execution_stats = std::move(execution_stats),
	};
	return batch_processor::cpu_preparation_status::ready;
}


/** @return The immutable callable table for one alphabet and storage width. */
template <class p_alphabet, std::size_t p_width>
[[nodiscard]] const batch_processor::cpu_preparation_typmod_operations&
typmod_ops() noexcept {
	static constexpr batch_processor::cpu_preparation_typmod_operations operations{
		.alphabet_code = static_cast<std::uint8_t>(
			pgseq_supported_alphabets::index_of<p_alphabet>()),
		.storage_unit_byte_width = static_cast<std::uint8_t>(p_width),
		.append_header_sketch =
			&append_cpu_prepared_query_sketch<
				p_alphabet, p_width, minimap_sketch_data>,
		.append_header_u32 =
			&append_cpu_prepared_query_sketch<
				p_alphabet, p_width, std::uint32_t>,
		.append_recipe_sketch =
			&append_cpu_prepared_query_recipe_sketch<
				p_alphabet, minimap_sketch_data>,
		.append_recipe_u32 =
			&append_cpu_prepared_query_recipe_sketch<
				p_alphabet, std::uint32_t>,
		.prepare_independent =
			&prepare_independently_admitted_query_sketch<p_alphabet, p_width>,
	};
	static_assert(operations.complete());
	return operations;
}

} // namespace bioseq::simd::cpu_preparation_detail
