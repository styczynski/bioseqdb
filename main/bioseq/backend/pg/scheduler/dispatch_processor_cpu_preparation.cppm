module;

#include "common.h"
#include "headers/pg_build_config.h"

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

module bioseq.scheduler.dispatch_processor:cpu_preparation_support;

import bioseq.scheduler.dispatch_processor;
import bioseq.common.sequan_compat;
import bioseq.highway.dispatch_contract;
import bioseq.pg_utils;
import bioseq.indexing.type;
import bioseq.indexing.traits;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.pipe;
import bioseq.types.pg_compact_sequence;
import bioseq.storage.lazy;
import bioseq.storage.scanner;
import bioseq.storage.scanner_pg;
import bioseq.parallelism.concepts;
import bioseq.parallelism.memory_admission;

namespace bioseq::simd::cpu_preparation_detail {

/*
 * Format-6 preparation fans one sketch pass into the authenticated canonical
 * u32 union and, when separately admitted, the full per-query minimizer vector
 * later consumed by alignment.  This removes the typed row path's second
 * sketch without introducing a Q-sized conversion owner.
 *
 * The collector deliberately exposes the same small surface used by the
 * scalar and ExactSimd kernels.  Capacity remains charged to the prepared
 * target's aggregate permits, including vector reallocation overlap.  Retained
 * per-query growth is best-effort; canonical collection remains fail-closed.
 */
class AdmittedCanonicalU32QueryCollector {
public:
	using minimizer_type = MinimapSketchData;

	AdmittedCanonicalU32QueryCollector(
			std::vector<std::uint32_t>& destination,
			bioseq::parallelism::MemoryPermit& permit,
			std::uint64_t& retained_bytes,
			std::vector<MinimapSketchData>* retained_query = nullptr,
			bioseq::parallelism::MemoryPermit*
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

	[[nodiscard]] std::size_t size() const noexcept {
		return destination_.size();
	}

	[[nodiscard]] bool reserve(const std::size_t required) {
		return ensure_capacity(required);
	}

	[[always_inline]] void collect(const MinimapSketchData& minimizer) {
		append_hash(minimizer.kmer());
		append_retained(minimizer);
	}

	[[always_inline]] void collect(MinimapSketchData&& minimizer) {
		append_hash(minimizer.kmer());
		append_retained(std::move(minimizer));
	}

	[[nodiscard, always_inline]] bool append_records(
			const std::span<const MinimapSketchData> records) {
		const std::size_t old_size = destination_.size();
		if (records.size() > destination_.max_size() - old_size
			|| !ensure_capacity(old_size + records.size())) {
			failed_ = true;
			return false;
		}
		for (const MinimapSketchData& record : records) {
			const std::uint64_t hash = record.kmer();
			hash_width_valid_ = hash_width_valid_
				&& hash <= (std::numeric_limits<std::uint32_t>::max)();
			destination_.push_back(static_cast<std::uint32_t>(hash));
		}
		append_retained_records(records);
		return true;
	}

	template<class M = MinimapSketchData>
	[[nodiscard, always_inline]] bool append_raw_storage(
			const typename M::storage_t* raw,
			const std::size_t count)
		requires (
			std::same_as<M, MinimapSketchData>
			&& requires { typename M::storage_t; }
			&& std::is_trivially_copyable_v<M>
			&& sizeof(M) == sizeof(typename M::storage_t))
	{
		const std::size_t old_size = destination_.size();
		if (count > destination_.max_size() - old_size
			|| !ensure_capacity(old_size + count)) {
			failed_ = true;
			return false;
		}
		for (std::size_t index = 0; index < count; ++index) {
			const std::uint64_t hash =
				MinimapSketchData::from_raw_storage(raw[index]).kmer();
			hash_width_valid_ =
				hash_width_valid_
				&& hash
					<= (std::numeric_limits<std::uint32_t>::max)();
			destination_.push_back(static_cast<std::uint32_t>(hash));
		}
		append_retained_raw(raw, count);
		return true;
	}

	[[nodiscard]] bool ok() const noexcept {
		return !failed_;
	}

	[[nodiscard]] bool hash_width_valid() const noexcept {
		return hash_width_valid_;
	}

	[[nodiscard]] bool retention_complete() const noexcept {
		return retention_complete_;
	}

private:
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

	void abandon_retention() noexcept {
		if (!retention_complete_
			|| retained_query_ == nullptr
			|| retained_query_permit_ == nullptr
			|| retained_query_bytes_ == nullptr) {
			retention_complete_ = false;
			return;
		}
		const std::uint64_t released_bytes =
			bioseq::parallelism::saturating_mul(
				retained_query_->capacity(),
				sizeof(MinimapSketchData));
		{
			std::vector<MinimapSketchData> empty;
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
			bioseq::parallelism::saturating_mul(
				old_capacity, sizeof(MinimapSketchData));
		const std::uint64_t predecessor_bytes =
			bioseq::parallelism::saturating_mul(
				predecessor_capacity, sizeof(MinimapSketchData));
		const std::uint64_t requested_bytes =
			bioseq::parallelism::saturating_mul(
				requested_capacity, sizeof(MinimapSketchData));
		const std::uint64_t allocation_headroom =
			bioseq::parallelism::saturating_mul(
				requested_bytes,
				bioseq::parallelism::
					kVectorPreallocationHeadroomFactor);
		const std::uint64_t retained_other_bytes =
			*retained_query_bytes_ >= old_bytes
			? *retained_query_bytes_ - old_bytes
			: (std::numeric_limits<std::uint64_t>::max)();
		const std::uint64_t logical_predecessor_peak =
			bioseq::parallelism::saturating_add(
				retained_other_bytes, predecessor_bytes);
		const std::uint64_t peak =
			bioseq::parallelism::saturating_add(
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
			bioseq::parallelism::saturating_mul(
				retained_query_->capacity(),
				sizeof(MinimapSketchData));
		if (measured_bytes
				== (std::numeric_limits<std::uint64_t>::max)()
			|| measured_bytes > allocation_headroom) {
			{
				std::vector<MinimapSketchData> empty;
				retained_query_->swap(empty);
			}
			*retained_query_bytes_ -= old_bytes;
			retained_query_permit_->shrink_to(*retained_query_bytes_);
			retention_complete_ = false;
			return false;
		}
		const std::uint64_t reconciled_bytes =
			bioseq::parallelism::saturating_add(
				*retained_query_bytes_ - old_bytes,
				measured_bytes);
		if (reconciled_bytes
				== (std::numeric_limits<std::uint64_t>::max)()) {
			{
				std::vector<MinimapSketchData> empty;
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

	template<class Value>
	[[always_inline]] void append_retained(Value&& minimizer) {
		if (!retention_complete_) return;
		if (retained_query_->size() == retained_query_->max_size()
			|| !ensure_retained_capacity(retained_query_->size() + 1u)) {
			return;
		}
		retained_query_->push_back(std::forward<Value>(minimizer));
	}

	template<class Storage>
	[[always_inline]] void append_retained_raw(
			const Storage* raw,
			const std::size_t count) {
		if (!retention_complete_) return;
		const std::size_t old_size = retained_query_->size();
		if (count > retained_query_->max_size() - old_size
			|| !ensure_retained_capacity(old_size + count)) {
			return;
		}
		for (std::size_t index = 0u; index < count; ++index) {
			retained_query_->push_back(
				MinimapSketchData::from_raw_storage(raw[index]));
		}
	}

	[[always_inline]] void append_retained_records(
			const std::span<const MinimapSketchData> records) {
		if (!retention_complete_) return;
		const std::size_t old_size = retained_query_->size();
		if (records.size() > retained_query_->max_size() - old_size
			|| !ensure_retained_capacity(old_size + records.size())) {
			return;
		}
		retained_query_->insert(
			retained_query_->end(), records.begin(), records.end());
	}

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
			bioseq::parallelism::saturating_mul(
				old_capacity, sizeof(std::uint32_t));
		const std::uint64_t predecessor_bytes =
			bioseq::parallelism::saturating_mul(
				predecessor_capacity, sizeof(std::uint32_t));
		const std::uint64_t requested_bytes =
			bioseq::parallelism::saturating_mul(
				requested_capacity, sizeof(std::uint32_t));
		const std::uint64_t allocation_headroom =
			bioseq::parallelism::saturating_mul(
				requested_bytes,
				bioseq::parallelism::
					kVectorPreallocationHeadroomFactor);
		const std::uint64_t retained_other_bytes =
			retained_bytes_ >= old_bytes
			? retained_bytes_ - old_bytes
			: (std::numeric_limits<std::uint64_t>::max)();
		const std::uint64_t logical_predecessor_peak =
			bioseq::parallelism::saturating_add(
				retained_other_bytes, predecessor_bytes);
		const std::uint64_t peak =
			bioseq::parallelism::saturating_add(
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
			bioseq::parallelism::saturating_mul(
				destination_.capacity(), sizeof(std::uint32_t));
		if (measured_bytes
				== (std::numeric_limits<std::uint64_t>::max)()
			|| measured_bytes > allocation_headroom) {
			failed_ = true;
			return false;
		}
		retained_bytes_ =
			bioseq::parallelism::saturating_add(
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
	bioseq::parallelism::MemoryPermit& permit_;
	std::uint64_t& retained_bytes_;
	std::vector<MinimapSketchData>* retained_query_ = nullptr;
	bioseq::parallelism::MemoryPermit*
		retained_query_permit_ = nullptr;
	std::uint64_t* retained_query_bytes_ = nullptr;
	bool failed_ = false;
	bool hash_width_valid_ = true;
	bool retention_complete_ = false;
};

void release_uncommitted_retained_query(
		std::vector<MinimapSketchData>& minimizers,
		bioseq::parallelism::MemoryPermit& permit,
		std::uint64_t& retained_bytes) noexcept {
	const std::uint64_t released_bytes =
		bioseq::parallelism::saturating_mul(
			minimizers.capacity(), sizeof(MinimapSketchData));
	{
		std::vector<MinimapSketchData> empty;
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

[[nodiscard]] bool try_commit_retained_query_sketch(
		const std::uint64_t query_ordinal,
		const std::uint64_t query_bp_size,
		std::vector<MinimapSketchData>&& minimizers,
		pipe::MinimizerRangeExecutionProof proof,
		std::vector<BatchProcessor::CpuPreparedQuerySketch>& retained_queries,
		bioseq::parallelism::MemoryPermit& permit,
		std::uint64_t& retained_bytes) {
	if (!retained_queries.empty()
		&& retained_queries.back().query_ordinal >= query_ordinal) {
		bioseq::pg_utils::invoke_pg_as_cpp([] {
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("BioseqBatchJoin retained query ordinals are not strictly ordered")));
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
			bioseq::parallelism::saturating_mul(
				retained_queries.capacity(),
				sizeof(BatchProcessor::CpuPreparedQuerySketch));
		const std::uint64_t requested_bytes =
			bioseq::parallelism::saturating_mul(
				required,
				sizeof(BatchProcessor::CpuPreparedQuerySketch));
		const std::uint64_t allocation_headroom =
			bioseq::parallelism::saturating_mul(
				requested_bytes,
				bioseq::parallelism::
					kVectorPreallocationHeadroomFactor);
		const std::uint64_t peak =
			bioseq::parallelism::saturating_add(
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
			bioseq::parallelism::saturating_mul(
				retained_queries.capacity(),
				sizeof(BatchProcessor::CpuPreparedQuerySketch));
		if (measured_bytes
				== (std::numeric_limits<std::uint64_t>::max)()
			|| measured_bytes > allocation_headroom) {
			bioseq::pg_utils::invoke_pg_as_cpp([] {
				ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("BioseqBatchJoin retained query cache capacity exceeded admission")));
			});
			pg_unreachable();
		}
		retained_bytes =
			bioseq::parallelism::saturating_add(
				retained_bytes - old_bytes, measured_bytes);
		if (retained_bytes
				== (std::numeric_limits<std::uint64_t>::max)()) {
			bioseq::pg_utils::invoke_pg_as_cpp([] {
				ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("BioseqBatchJoin retained query cache capacity overflow")));
			});
			pg_unreachable();
		}
		permit.shrink_to(retained_bytes);
	}
	retained_queries.push_back(BatchProcessor::CpuPreparedQuerySketch{
		.query_ordinal = query_ordinal,
		.query_bp_size = query_bp_size,
		.minimizers = std::move(minimizers),
		.proof = std::move(proof),
	});
	return true;
}

template <class Alphabet, class DestinationValue>
void append_cpu_prepared_query_sketch_view(
		const bioseq::storage::basic_lazy_sequence_view<Alphabet> query_view,
		const std::uint64_t query_ordinal,
		const std::int32_t kmer_size,
		const std::int32_t window_size,
		const MinimizerSketchBackend sketch_backend,
		const ::bioseq::simd_dispatch::HighwayKernelTable* highway_kernels,
		std::vector<DestinationValue>& destination,
		bioseq::parallelism::MemoryPermit& destination_permit,
		std::uint64_t& retained_destination_bytes,
		std::vector<BatchProcessor::CpuPreparedQuerySketch>*
			retained_queries,
		bioseq::parallelism::MemoryPermit* retained_query_permit,
		std::uint64_t* retained_query_bytes) {
	static_assert(
		std::is_same_v<DestinationValue, MinimapSketchData>
		|| std::is_same_v<DestinationValue, std::uint32_t>);
	if (!minimizer_query_length_is_representable<MinimapSketchData>(
			query_view.bp_size())) {
		bioseq::pg_utils::invoke_pg_as_cpp([] {
			ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("BioseqBatchJoin query exceeds the 28-bit minimizer position domain")));
		});
		pg_unreachable();
	}
	const auto fail = [](const char* message) -> void {
		bioseq::pg_utils::invoke_pg_as_cpp([message] {
			ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY), errmsg("%s", message)));
		});
		pg_unreachable();
	};
	if constexpr (std::is_same_v<DestinationValue, std::uint32_t>) {
		std::vector<MinimapSketchData> retained_query;
		AdmittedCanonicalU32QueryCollector collector{
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
		auto proof = pipe::minimizer_dispatch_indexed_query_whole<
			MinimapSketchData>(
			collector,
			query_view,
			static_cast<std::size_t>(window_size),
			static_cast<std::size_t>(kmer_size),
			sketch_backend,
			highway_kernels);
		if (!collector.hash_width_valid()) {
			bioseq::pg_utils::invoke_pg_as_cpp([] {
				ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("Format-6 canonical query hash exceeds its authenticated u32 profile")));
			});
			pg_unreachable();
		}
		if (!collector.ok()) {
			fail("BioseqBatchJoin canonical query-hash destination growth cannot be admitted");
		}
		if (!pipe::minimizer_range_proof_matches_backend(
				sketch_backend,
				proof,
				/*require_nonempty_vector_work=*/false,
				pipe::ExactSimdRangeRoute::require_continuation)) {
			bioseq::pg_utils::invoke_pg_as_cpp([] {
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("BioseqBatchJoin prepared query sketch proof does not match the selected backend")));
			});
			pg_unreachable();
		}
		if (collector.retention_complete()) {
			(void)try_commit_retained_query_sketch(
				query_ordinal,
				static_cast<std::uint64_t>(query_view.bp_size()),
				std::move(retained_query),
				std::move(proof),
				*retained_queries,
				*retained_query_permit,
				*retained_query_bytes);
		}
		return;
	} else {
	auto collector_permit =
		bioseq::parallelism::MemoryAdmissionController::instance().try_reserve(
			0u);
	if (!collector_permit) {
		fail("BioseqBatchJoin query sketch collector admission owner is unavailable");
	}
	admitted_sketch_collector_t<
		MinimapSketchData,
		bioseq::parallelism::MemoryPermit> collector{
			std::move(*collector_permit),
			bioseq::parallelism::kVectorPreallocationHeadroomFactor};
	const auto proof = pipe::minimizer_dispatch_indexed_query_whole<
		MinimapSketchData>(
		collector,
		query_view,
		static_cast<std::size_t>(window_size),
		static_cast<std::size_t>(kmer_size),
		sketch_backend,
		highway_kernels);
	if (!collector.ok()) {
		fail("BioseqBatchJoin query sketch collector growth cannot be admitted");
	}
	if (!pipe::minimizer_range_proof_matches_backend(
			sketch_backend,
			proof,
			/*require_nonempty_vector_work=*/false,
			pipe::ExactSimdRangeRoute::require_continuation)) {
		bioseq::pg_utils::invoke_pg_as_cpp([] {
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("BioseqBatchJoin prepared query sketch proof does not match the selected backend")));
		});
		pg_unreachable();
	}
	const auto& sketches = collector.extract_sketches_data();
	const std::uint64_t measured_collector_bytes =
		bioseq::parallelism::saturating_mul(
			sketches.capacity(), sizeof(MinimapSketchData));
	if (measured_collector_bytes
			== (std::numeric_limits<std::uint64_t>::max)()
		|| measured_collector_bytes
			!= collector.admitted_capacity_bytes()) {
		fail("BioseqBatchJoin query sketch collector capacity disagrees with admission");
	}
	if (sketches.size() > destination.max_size() - destination.size()) {
		fail("BioseqBatchJoin query sketch destination size overflow");
	}
	const std::size_t required = destination.size() + sketches.size();
	if (required > destination.capacity()) {
		const std::uint64_t old_destination_bytes =
			bioseq::parallelism::saturating_mul(
				destination.capacity(), sizeof(DestinationValue));
		const std::uint64_t allocation_headroom =
			bioseq::parallelism::saturating_mul(
				bioseq::parallelism::saturating_mul(
					required, sizeof(DestinationValue)),
				bioseq::parallelism::kVectorPreallocationHeadroomFactor);
		const std::uint64_t peak = bioseq::parallelism::saturating_add(
			retained_destination_bytes, allocation_headroom);
		if (old_destination_bytes
				== (std::numeric_limits<std::uint64_t>::max)()
			|| allocation_headroom
				== (std::numeric_limits<std::uint64_t>::max)()
			|| peak == (std::numeric_limits<std::uint64_t>::max)()
			|| retained_destination_bytes < old_destination_bytes
			|| !destination_permit.grow_to(peak)) {
			fail("BioseqBatchJoin query sketch destination growth cannot be admitted");
		}
		destination.reserve(required);
		const std::uint64_t measured_destination_bytes =
			bioseq::parallelism::saturating_mul(
				destination.capacity(), sizeof(DestinationValue));
		if (measured_destination_bytes
				== (std::numeric_limits<std::uint64_t>::max)()
			|| measured_destination_bytes > allocation_headroom) {
			fail("BioseqBatchJoin query sketch destination capacity exceeded admission");
		}
		retained_destination_bytes = bioseq::parallelism::saturating_add(
			retained_destination_bytes - old_destination_bytes,
			measured_destination_bytes);
		if (retained_destination_bytes
			== (std::numeric_limits<std::uint64_t>::max)()) {
			fail("BioseqBatchJoin retained query sketch capacity overflow");
		}
		destination_permit.shrink_to(retained_destination_bytes);
	}
	destination.insert(
		destination.end(),
		sketches.begin(), sketches.end());
	}
}

template <class Alphabet, std::size_t Width, class DestinationValue>
void append_cpu_prepared_query_sketch(
		const bioseq::pg_types::seq& header,
		const std::uint64_t query_ordinal,
		const std::int32_t kmer_size,
		const std::int32_t window_size,
		const MinimizerSketchBackend sketch_backend,
		const ::bioseq::simd_dispatch::HighwayKernelTable* highway_kernels,
		std::vector<DestinationValue>& destination,
		bioseq::parallelism::MemoryPermit& destination_permit,
		std::uint64_t& retained_destination_bytes,
		std::vector<BatchProcessor::CpuPreparedQuerySketch>* retained_queries,
		bioseq::parallelism::MemoryPermit* retained_query_permit,
		std::uint64_t* retained_query_bytes) {
	using sequence_t = compact_sequence_stdc_instance<
		bioseq::pg_types::seq, Alphabet, Width>;
	compact_sequence_range<sequence_t> query{header};
	append_cpu_prepared_query_sketch_view<Alphabet, DestinationValue>(
		bioseq::storage::as_lazy_sequence_view(query),
		query_ordinal, kmer_size, window_size, sketch_backend, highway_kernels,
		destination, destination_permit, retained_destination_bytes,
		retained_queries, retained_query_permit, retained_query_bytes);
}


template <class Alphabet, class DestinationValue>
void append_cpu_prepared_query_recipe_sketch(
		const Datum recipe,
		const pg_sequence_descriptor& descriptor,
		pg_sequence_read_session& session,
		const bioseq::storage::sequence_payload_allocation_observer*
			payload_observer,
		const std::uint64_t query_ordinal,
		const std::int32_t kmer_size,
		const std::int32_t window_size,
		const MinimizerSketchBackend sketch_backend,
		const ::bioseq::simd_dispatch::HighwayKernelTable* highway_kernels,
		std::vector<DestinationValue>& destination,
		bioseq::parallelism::MemoryPermit& destination_permit,
		std::uint64_t& retained_destination_bytes,
		std::vector<BatchProcessor::CpuPreparedQuerySketch>*
			retained_queries,
		bioseq::parallelism::MemoryPermit* retained_query_permit,
		std::uint64_t* retained_query_bytes) {
	auto* owner = bioseq::pg_utils::invoke_pg_as_cpp(
		[&session, recipe, descriptor] {
			return ::make_pg_sequence_call_owner<Alphabet>(
				session, recipe, descriptor);
		});
	append_cpu_prepared_query_sketch_view<Alphabet, DestinationValue>(
		owner->view().with_payload_allocation_observer(payload_observer),
		query_ordinal, kmer_size, window_size,
		sketch_backend, highway_kernels,
		destination, destination_permit,
		retained_destination_bytes, retained_queries,
		retained_query_permit, retained_query_bytes);
}

template <class Alphabet, std::size_t Width>
[[nodiscard]] BatchProcessor::CpuPreparationStatus
prepare_independently_admitted_query_sketch(
		const bioseq::pg_types::seq& header,
		const std::uint64_t query_ordinal,
		const std::int32_t kmer_size,
		const std::int32_t window_size,
		const MinimizerSketchBackend sketch_backend,
		const ::bioseq::simd_dispatch::HighwayKernelTable* highway_kernels,
		BatchProcessor::CpuPreparedQuerySketch& destination) {
	using sequence_t = compact_sequence_stdc_instance<
		bioseq::pg_types::seq, Alphabet, Width>;
	compact_sequence_range<sequence_t> query{header};
	auto query_owner = bioseq::storage::make_lazy_in_memory(query);
	const auto query_view =
		bioseq::storage::as_lazy_sequence_view(query_owner);
	if (!minimizer_query_length_is_representable<MinimapSketchData>(
			static_cast<bioseq::storage::sequence_size_t>(query.bp_size()))) {
		return BatchProcessor::CpuPreparationStatus::query_length_unsupported;
	}
	auto permit =
		bioseq::parallelism::MemoryAdmissionController::instance().try_reserve(
			0u);
	if (!permit) {
		return BatchProcessor::CpuPreparationStatus::admission_unavailable;
	}
	admitted_sketch_collector_t<
		MinimapSketchData,
		bioseq::parallelism::MemoryPermit> collector{
			std::move(*permit),
			bioseq::parallelism::kVectorPreallocationHeadroomFactor};
	auto proof = pipe::minimizer_dispatch_indexed_query_whole<
		MinimapSketchData>(
		collector,
		query_view,
		static_cast<std::size_t>(window_size),
		static_cast<std::size_t>(kmer_size),
		sketch_backend,
		highway_kernels);
	if (!collector.ok()) {
		return BatchProcessor::CpuPreparationStatus::collector_growth_failed;
	}
	if (!pipe::minimizer_range_proof_matches_backend(
			sketch_backend,
			proof,
			/*require_nonempty_vector_work=*/false,
			pipe::ExactSimdRangeRoute::require_continuation)) {
		return BatchProcessor::CpuPreparationStatus::proof_invalid;
	}
	auto owned = std::move(collector).release_owned_storage();
	const std::uint64_t measured_bytes =
		bioseq::parallelism::saturating_mul(
			owned.first.capacity(), sizeof(MinimapSketchData));
	if (measured_bytes
			== (std::numeric_limits<std::uint64_t>::max)()
		|| measured_bytes != owned.second.bytes()) {
		return BatchProcessor::CpuPreparationStatus::capacity_mismatch;
	}
	destination = BatchProcessor::CpuPreparedQuerySketch{
		.query_ordinal = query_ordinal,
		.query_bp_size = static_cast<std::uint64_t>(query.bp_size()),
		.parallel_preparation_permit =
			std::optional<bioseq::parallelism::MemoryPermit>{
				std::move(owned.second)},
		.minimizers = std::move(owned.first),
		.proof = std::move(proof),
	};
	return BatchProcessor::CpuPreparationStatus::ready;
}


template <class Alphabet, std::size_t Width>
[[nodiscard]] const BatchProcessor::CpuPreparationTypmodOps&
typmod_ops() noexcept {
	static constexpr BatchProcessor::CpuPreparationTypmodOps operations{
		.alphabet_code = static_cast<std::uint8_t>(
			pgseq_supported_alphabets::index_of<Alphabet>()),
		.storage_unit_byte_width = static_cast<std::uint8_t>(Width),
		.append_header_sketch =
			&append_cpu_prepared_query_sketch<
				Alphabet, Width, MinimapSketchData>,
		.append_header_u32 =
			&append_cpu_prepared_query_sketch<
				Alphabet, Width, std::uint32_t>,
		.append_recipe_sketch =
			&append_cpu_prepared_query_recipe_sketch<
				Alphabet, MinimapSketchData>,
		.append_recipe_u32 =
			&append_cpu_prepared_query_recipe_sketch<
				Alphabet, std::uint32_t>,
		.prepare_independent =
			&prepare_independently_admitted_query_sketch<Alphabet, Width>,
	};
	static_assert(operations.complete());
	return operations;
}

} // namespace bioseq::simd::cpu_preparation_detail
