/**
 * @file cpu_preparation.cppm
 * @brief Implements typmod-specific admitted CPU query preparation.
 */
module;

extern "C" {
#include "postgres.h"
#include "utils/errcodes.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <new>

module bioseq.postgres.scheduler.dispatch_processor:cpu_preparation;

import std;
import bioseq.postgres.scheduler.dispatch_processor;
import bioseq.common.checked_arithmetic;
import bioseq.common.seqan_adapter;
import bioseq.highway.dispatch_contract;
import bioseq.postgres.support;
import bioseq.indexing.type;
import bioseq.indexing.traits;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.minimizer_physical_registry;
import bioseq.pipeline.pipe;
import bioseq.pipeline.runtime_contract;
import bioseq.postgres.types.compact_sequence;
import bioseq.storage.lazy_sequence;
import bioseq.storage.scanner;
import bioseq.postgres.storage.scanner;
import bioseq.parallelism.concepts;
import bioseq.parallelism.memory_admission;

namespace bioseq::simd::cpu_preparation_detail {

/** @return True after absorbing one independently admitted physical query owner. */
[[nodiscard]] bool try_commit_physical_query_sketch(
    batch_processor::cpu_prepared_query_sketch&& prepared,
    std::vector<batch_processor::cpu_prepared_query_sketch>& retained_queries,
    bioseq::parallelism::memory_permit& permit,
    std::uint64_t& retained_bytes) {
    if (!prepared.parallel_preparation_permit || !prepared.minimizers.valid() ||
        prepared.minimizers.capacity_bytes() != prepared.parallel_preparation_permit->bytes() ||
        (!retained_queries.empty() &&
         retained_queries.back().query_ordinal >= prepared.query_ordinal) ||
        retained_queries.size() == retained_queries.max_size()) {
        return false;
    }
    if (retained_queries.size() == retained_queries.capacity()) {
        const std::size_t required = retained_queries.size() + 1u;
        const std::size_t old_capacity = retained_queries.capacity();
        const std::size_t requested_capacity =
            old_capacity <= retained_queries.max_size() / 2u
                ? std::max(required, old_capacity * 2u)
                : required;
        const std::uint64_t old_bytes = bioseq::common::saturating_mul(
            retained_queries.capacity(), sizeof(batch_processor::cpu_prepared_query_sketch));
        const std::uint64_t requested_bytes = bioseq::common::saturating_mul(
            requested_capacity, sizeof(batch_processor::cpu_prepared_query_sketch));
        const std::uint64_t allocation_headroom = bioseq::common::saturating_mul(
            requested_bytes, bioseq::parallelism::k_vector_preallocation_headroom_factor);
        const std::uint64_t peak = bioseq::common::saturating_add(retained_bytes,
                                                                  allocation_headroom);
        if (old_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            requested_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            allocation_headroom == (std::numeric_limits<std::uint64_t>::max)() ||
            peak == (std::numeric_limits<std::uint64_t>::max)() || retained_bytes < old_bytes ||
            !permit.grow_to(peak)) {
            return false;
        }
        try {
            retained_queries.reserve(requested_capacity);
        } catch (...) {
            permit.shrink_to(retained_bytes);
            throw;
        }
        const std::uint64_t measured_bytes = bioseq::common::saturating_mul(
            retained_queries.capacity(), sizeof(batch_processor::cpu_prepared_query_sketch));
        const std::uint64_t reconciled = bioseq::common::saturating_add(
            retained_bytes - old_bytes, measured_bytes);
        if (measured_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            measured_bytes > allocation_headroom ||
            reconciled == (std::numeric_limits<std::uint64_t>::max)()) {
            return false;
        }
        retained_bytes = reconciled;
        permit.shrink_to(retained_bytes);
    }
    const std::uint64_t physical_bytes = prepared.minimizers.capacity_bytes();
    const std::uint64_t reconciled =
        bioseq::common::saturating_add(retained_bytes, physical_bytes);
    if (reconciled == (std::numeric_limits<std::uint64_t>::max)() ||
        !permit.absorb(std::move(*prepared.parallel_preparation_permit))) {
        return false;
    }
    prepared.parallel_preparation_permit.reset();
    retained_bytes = reconciled;
    retained_queries.push_back(std::move(prepared));
    return true;
}

/** Append decoded hashes after admitting the replacement vector allocation. */
template <class p_hash>
[[nodiscard]] bool append_physical_query_hashes(
    const ::bioseq::minimizers::minimizer_physical_batch_view records,
    std::vector<p_hash>& destination,
    bioseq::parallelism::memory_permit& permit,
    std::uint64_t& retained_bytes) {
    static_assert(std::same_as<p_hash, std::uint32_t> || std::same_as<p_hash, std::uint64_t>);
    if (records.size() > destination.max_size() - destination.size()) {
        return false;
    }
    const std::size_t required = destination.size() + records.size();
    if (required > destination.capacity()) {
        std::size_t requested_capacity = destination.capacity() == 0u
                                             ? std::min<std::size_t>(8u, destination.max_size())
                                             : destination.capacity();
        while (requested_capacity < required) {
            if (requested_capacity > destination.max_size() / 2u) {
                requested_capacity = required;
                break;
            }
            requested_capacity *= 2u;
        }
        requested_capacity = std::max(requested_capacity, required);
        const std::uint64_t old_bytes =
            bioseq::common::saturating_mul(destination.capacity(), sizeof(p_hash));
        const std::uint64_t requested_bytes =
            bioseq::common::saturating_mul(requested_capacity, sizeof(p_hash));
        const std::uint64_t allocation_headroom = bioseq::common::saturating_mul(
            requested_bytes, bioseq::parallelism::k_vector_preallocation_headroom_factor);
        const std::uint64_t peak =
            bioseq::common::saturating_add(retained_bytes, allocation_headroom);
        if (old_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            requested_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            allocation_headroom == (std::numeric_limits<std::uint64_t>::max)() ||
            peak == (std::numeric_limits<std::uint64_t>::max)() || retained_bytes < old_bytes ||
            !permit.grow_to(peak)) {
            return false;
        }
        try {
            destination.reserve(requested_capacity);
        } catch (...) {
            permit.shrink_to(retained_bytes);
            throw;
        }
        const std::uint64_t measured =
            bioseq::common::saturating_mul(destination.capacity(), sizeof(p_hash));
        const std::uint64_t reconciled =
            bioseq::common::saturating_add(retained_bytes - old_bytes, measured);
        if (measured == (std::numeric_limits<std::uint64_t>::max)() ||
            measured > allocation_headroom ||
            reconciled == (std::numeric_limits<std::uint64_t>::max)()) {
            return false;
        }
        retained_bytes = reconciled;
        permit.shrink_to(retained_bytes);
    }
    if constexpr (std::same_as<p_hash, std::uint32_t>) {
        bool hashes_fit = true;
        records.for_each_decoded(
            [&hashes_fit](const std::size_t,
                          const ::bioseq::minimizers::minimizer_physical_record_value minimizer) {
                hashes_fit = hashes_fit &&
                             minimizer.kmer() <=
                                 (std::numeric_limits<std::uint32_t>::max)();
            });
        if (!hashes_fit) {
            return false;
        }
    }
    records.for_each_decoded(
        [&destination](const std::size_t,
                       const ::bioseq::minimizers::minimizer_physical_record_value minimizer) {
            destination.push_back(static_cast<p_hash>(minimizer.kmer()));
        });
    return destination.size() == required;
}

/** Prepare one query in the concrete record selected before instantiation. */
template <class p_alphabet>
[[nodiscard]] batch_processor::cpu_preparation_status prepare_physical_query_sketch_view(
    const bioseq::storage::lazy_sequence_view<p_alphabet> query_view,
    const std::uint64_t query_ordinal,
    const std::int32_t kmer_size,
    const std::int32_t window_size,
    const minimizer_sketch_backend sketch_backend,
    const ::bioseq::minimizers::minimizer_physical_layout_selection& execution_layout,
    const ::bioseq::simd_dispatch::highway_kernel_table* highway_kernels,
    batch_processor::cpu_prepared_query_sketch& destination) {
    const auto expected_backend = pipe::minimizer_sketch_backend_for_effective_algorithm(
        execution_layout.effective_algorithm);
    if (destination.parallel_preparation_permit || destination.minimizers.valid() ||
        kmer_size < 0 || window_size < 0 ||
        static_cast<std::uint64_t>(kmer_size) >
            (std::numeric_limits<std::uint16_t>::max)() ||
        static_cast<std::uint64_t>(window_size) >
            (std::numeric_limits<std::uint16_t>::max)() ||
        !expected_backend || *expected_backend != sketch_backend ||
        execution_layout.runtime_kmer_size != static_cast<std::uint16_t>(kmer_size) ||
        execution_layout.runtime_window_size != static_cast<std::uint16_t>(window_size)) {
        return batch_processor::cpu_preparation_status::validation_failed;
    }
    return ::bioseq::minimizers::visit_minimizer_physical_layout(
        execution_layout,
        [query_view,
         query_ordinal,
         kmer_size,
         window_size,
         sketch_backend,
         &execution_layout,
         highway_kernels,
         &destination]<class p_record>() -> batch_processor::cpu_preparation_status {
            if (!pipe::minimizer_execution_layout_matches_record<p_record, p_alphabet>(
                    execution_layout,
                    static_cast<std::uint16_t>(kmer_size),
                    static_cast<std::uint16_t>(window_size))) {
                return batch_processor::cpu_preparation_status::validation_failed;
            }
            if (!is_minimizer_query_length_representable<p_record>(query_view.bp_size())) {
                return batch_processor::cpu_preparation_status::query_length_unsupported;
            }
            auto permit =
                bioseq::parallelism::memory_admission_controller::instance().try_reserve(0u);
            if (!permit) {
                return batch_processor::cpu_preparation_status::admission_unavailable;
            }
            admitted_sketch_collector<p_record, bioseq::parallelism::memory_permit> collector{
                std::move(*permit),
                bioseq::parallelism::k_vector_preallocation_headroom_factor};
            auto execution_stats = pipe::minimizer_dispatch_indexed_query_whole<p_record>(
                collector,
                query_view,
                static_cast<std::size_t>(window_size),
                static_cast<std::size_t>(kmer_size),
                sketch_backend,
                highway_kernels,
                &execution_layout);
            if (!collector.is_ok()) {
                return batch_processor::cpu_preparation_status::collector_growth_failed;
            }
            if (!pipe::minimizer_range_stats_matches_effective_algorithm(
                    execution_layout.effective_algorithm,
                    execution_stats,
                    /*require_nonempty_vector_work=*/false,
                    pipe::exact_simd_range_route::require_continuation)) {
                return batch_processor::cpu_preparation_status::validation_failed;
            }
            auto released = std::move(collector).release_owned_storage();
            // These ordered locals own the released pair immediately: every
            // return below destroys the vector before its adjacent permit.
            bioseq::parallelism::memory_permit storage_permit = std::move(released.second);
            std::vector<p_record> physical_records = std::move(released.first);
            const std::uint64_t measured =
                bioseq::common::saturating_mul(physical_records.capacity(), sizeof(p_record));
            if (measured == (std::numeric_limits<std::uint64_t>::max)() ||
                measured != storage_permit.bytes()) {
                return batch_processor::cpu_preparation_status::capacity_mismatch;
            }
            ::bioseq::minimizers::minimizer_physical_batch_owner physical_owner;
            try {
                physical_owner =
                    ::bioseq::minimizers::minimizer_physical_batch_owner::from_owned_host(
                        execution_layout, std::move(physical_records), storage_permit);
            } catch (const std::bad_alloc&) {
                return batch_processor::cpu_preparation_status::admission_unavailable;
            }
            destination = batch_processor::cpu_prepared_query_sketch{
                query_ordinal,
                static_cast<std::uint64_t>(query_view.bp_size()),
                std::optional<bioseq::parallelism::memory_permit>{
                    std::move(storage_permit)},
                std::move(physical_owner),
                std::move(execution_stats)};
            return batch_processor::cpu_preparation_status::ready;
        });
}

/** Select and authenticate the fixed PG query-cache record before mutation. */
template <class p_alphabet, std::size_t p_width>
[[nodiscard]] ::bioseq::minimizers::minimizer_physical_layout_selection
select_cpu_query_execution_layout(const minimizer_algorithm requested_algorithm,
                                  const std::uint64_t maximum_query_bp,
                                  const std::int32_t kmer_size,
                                  const std::int32_t window_size) {
    if (kmer_size < 0 || window_size < 0 ||
        static_cast<std::uint64_t>(kmer_size) >
            (std::numeric_limits<std::uint16_t>::max)() ||
        static_cast<std::uint64_t>(window_size) >
            (std::numeric_limits<std::uint16_t>::max)()) {
        throw std::invalid_argument{"CPU query minimizer layout is outside its registry"};
    }
    const auto selection = pipe::select_minimizer_execution_layout_prefer_native<p_alphabet>(
        requested_algorithm,
        maximum_query_bp,
        static_cast<std::uint16_t>(kmer_size),
        static_cast<std::uint16_t>(window_size),
        static_cast<std::uint8_t>(p_width * 8u));
    return selection;
}

/** @brief Appends one lazy query view to a prepared sketch representation. */
template <class p_alphabet, class p_destination_value>
void append_cpu_prepared_query_sketch_view(
    const bioseq::storage::lazy_sequence_view<p_alphabet> query_view,
    const std::uint64_t query_ordinal,
    const std::int32_t kmer_size,
    const std::int32_t window_size,
    const minimizer_sketch_backend sketch_backend,
    const ::bioseq::minimizers::minimizer_physical_layout_selection& execution_layout,
    const ::bioseq::simd_dispatch::highway_kernel_table* highway_kernels,
    std::vector<p_destination_value>& destination,
    bioseq::parallelism::memory_permit& destination_permit,
    std::uint64_t& retained_destination_bytes,
    std::vector<batch_processor::cpu_prepared_query_sketch>* retained_queries,
    bioseq::parallelism::memory_permit* retained_query_permit,
    std::uint64_t* retained_query_bytes) {
    static_assert(std::is_same_v<p_destination_value, minimap_sketch_data> ||
                  std::is_same_v<p_destination_value, std::uint32_t> ||
                  std::is_same_v<p_destination_value, std::uint64_t>);
    const auto expected_backend = pipe::minimizer_sketch_backend_for_effective_algorithm(
        execution_layout.effective_algorithm);
    if (kmer_size < 0 || window_size < 0 ||
        static_cast<std::uint64_t>(kmer_size) >
            (std::numeric_limits<std::uint16_t>::max)() ||
        static_cast<std::uint64_t>(window_size) >
            (std::numeric_limits<std::uint16_t>::max)() ||
        !expected_backend.has_value() || *expected_backend != sketch_backend ||
        execution_layout.runtime_kmer_size != static_cast<std::uint16_t>(kmer_size) ||
        execution_layout.runtime_window_size != static_cast<std::uint16_t>(window_size)) {
        throw std::invalid_argument{
            "CPU prepared query disagrees with its generated execution layout"};
    }
    if constexpr (std::is_same_v<p_destination_value, minimap_sketch_data>) {
        if (!pipe::minimizer_execution_layout_matches_record<minimap_sketch_data, p_alphabet>(
                execution_layout,
                static_cast<std::uint16_t>(kmer_size),
                static_cast<std::uint16_t>(window_size))) {
            throw std::invalid_argument{
                "legacy CPU query destination requires its registered K15 record"};
        }
    }
    if constexpr (std::is_same_v<p_destination_value, minimap_sketch_data>) {
        if (!is_minimizer_query_length_representable<minimap_sketch_data>(query_view.bp_size())) {
            bioseq::postgres::support::invoke_pg_as_cpp([] {
                ereport(
                    ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("bioseq batch join query exceeds the 28-bit minimizer position domain")));
            });
            pg_unreachable();
        }
    }
    const auto fail = [](const char* message) -> void {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [message] { ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("%s", message))); });
        pg_unreachable();
    };
    if constexpr (std::is_same_v<p_destination_value, minimap_sketch_data>) {
        auto collector_permit =
            bioseq::parallelism::memory_admission_controller::instance().try_reserve(0u);
        if (!collector_permit) {
            fail("bioseq batch join query sketch collector admission owner is unavailable");
        }
        admitted_sketch_collector<minimap_sketch_data, bioseq::parallelism::memory_permit>
            collector{std::move(*collector_permit),
                      bioseq::parallelism::k_vector_preallocation_headroom_factor};
        const auto execution_stats =
            pipe::minimizer_dispatch_indexed_query_whole<minimap_sketch_data>(
                collector,
                query_view,
                static_cast<std::size_t>(window_size),
                static_cast<std::size_t>(kmer_size),
                sketch_backend,
                highway_kernels,
                &execution_layout);
        if (!collector.is_ok()) {
            fail("bioseq batch join query sketch collector growth cannot be admitted");
        }
        if (!pipe::minimizer_range_stats_matches_effective_algorithm(
                execution_layout.effective_algorithm,
                execution_stats,
                /*require_nonempty_vector_work=*/false,
                pipe::exact_simd_range_route::require_continuation)) {
            bioseq::postgres::support::invoke_pg_as_cpp([] {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq batch join prepared query sketch stats do not match the "
                                "selected backend")));
            });
            pg_unreachable();
        }
        const auto& sketches = collector.extract_sketches_data();
        const std::uint64_t measured_collector_bytes =
            bioseq::common::saturating_mul(sketches.capacity(), sizeof(minimap_sketch_data));
        if (measured_collector_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            measured_collector_bytes != collector.admitted_capacity_bytes()) {
            fail("bioseq batch join query sketch collector capacity disagrees with admission");
        }
        if (sketches.size() > destination.max_size() - destination.size()) {
            fail("bioseq batch join query sketch destination size overflow");
        }
        const std::size_t required = destination.size() + sketches.size();
        if (required > destination.capacity()) {
            const std::uint64_t old_destination_bytes =
                bioseq::common::saturating_mul(destination.capacity(), sizeof(p_destination_value));
            const std::uint64_t allocation_headroom = bioseq::common::saturating_mul(
                bioseq::common::saturating_mul(required, sizeof(p_destination_value)),
                bioseq::parallelism::k_vector_preallocation_headroom_factor);
            const std::uint64_t peak =
                bioseq::common::saturating_add(retained_destination_bytes, allocation_headroom);
            if (old_destination_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                allocation_headroom == (std::numeric_limits<std::uint64_t>::max)() ||
                peak == (std::numeric_limits<std::uint64_t>::max)() ||
                retained_destination_bytes < old_destination_bytes ||
                !destination_permit.grow_to(peak)) {
                fail("bioseq batch join query sketch destination growth cannot be admitted");
            }
            destination.reserve(required);
            const std::uint64_t measured_destination_bytes =
                bioseq::common::saturating_mul(destination.capacity(), sizeof(p_destination_value));
            if (measured_destination_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                measured_destination_bytes > allocation_headroom) {
                fail("bioseq batch join query sketch destination capacity exceeded admission");
            }
            retained_destination_bytes = bioseq::common::saturating_add(
                retained_destination_bytes - old_destination_bytes, measured_destination_bytes);
            if (retained_destination_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
                fail("bioseq batch join retained query sketch capacity overflow");
            }
            destination_permit.shrink_to(retained_destination_bytes);
        }
        destination.insert(destination.end(), sketches.begin(), sketches.end());
    } else {
        if (static_cast<std::uint8_t>(execution_layout.hash_width) !=
            sizeof(p_destination_value)) {
            throw std::invalid_argument{
                "canonical CPU query destination disagrees with its generated hash width"};
        }
        if (retained_queries == nullptr || retained_query_permit == nullptr ||
            retained_query_bytes == nullptr) {
            throw std::logic_error{
                "canonical CPU query preparation requires its retained physical owner"};
        }
        batch_processor::cpu_prepared_query_sketch prepared;
        const auto status = prepare_physical_query_sketch_view(
            query_view,
            query_ordinal,
            kmer_size,
            window_size,
            sketch_backend,
            execution_layout,
            highway_kernels,
            prepared);
        switch (status) {
        case batch_processor::cpu_preparation_status::ready:
            break;
        case batch_processor::cpu_preparation_status::query_length_unsupported:
            bioseq::postgres::support::invoke_pg_as_cpp([] {
                ereport(ERROR,
                        (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                         errmsg("bioseq batch join query exceeds its selected minimizer position "
                                "domain")));
            });
            pg_unreachable();
        case batch_processor::cpu_preparation_status::validation_failed:
        case batch_processor::cpu_preparation_status::unsupported_typmod:
        case batch_processor::cpu_preparation_status::pending:
            throw std::logic_error{
                "canonical CPU query preparation disagrees with its generated record"};
        case batch_processor::cpu_preparation_status::admission_unavailable:
        case batch_processor::cpu_preparation_status::collector_growth_failed:
        case batch_processor::cpu_preparation_status::capacity_mismatch:
            fail("bioseq batch join canonical query sketch cannot be admitted");
        }
        if (!append_physical_query_hashes<p_destination_value>(prepared.minimizers.view(),
                                                              destination,
                                                              destination_permit,
                                                              retained_destination_bytes)) {
            fail("bioseq batch join canonical query union cannot be admitted");
        }
        if (!try_commit_physical_query_sketch(std::move(prepared),
                                              *retained_queries,
                                              *retained_query_permit,
                                              *retained_query_bytes)) {
            fail("bioseq batch join canonical query owner cannot be retained");
        }
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
    const ::bioseq::minimizers::minimizer_physical_layout_selection& execution_layout,
    const ::bioseq::simd_dispatch::highway_kernel_table* highway_kernels,
    std::vector<p_destination_value>& destination,
    bioseq::parallelism::memory_permit& destination_permit,
    std::uint64_t& retained_destination_bytes,
    std::vector<batch_processor::cpu_prepared_query_sketch>* retained_queries,
    bioseq::parallelism::memory_permit* retained_query_permit,
    std::uint64_t* retained_query_bytes) {
    using sequence_t = compact_sequence_stdc_instance<bioseq::pg_types::seq, p_alphabet, p_width>;
    compact_sequence_range<sequence_t> query{header};
    append_cpu_prepared_query_sketch_view<p_alphabet, p_destination_value>(
        bioseq::storage::make_lazy_sequence_view(query),
        query_ordinal,
        kmer_size,
        window_size,
        sketch_backend,
        execution_layout,
        highway_kernels,
        destination,
        destination_permit,
        retained_destination_bytes,
        retained_queries,
        retained_query_permit,
        retained_query_bytes);
}

/** @brief Adapts one recipe-backed query into admitted CPU preparation. */
template <class p_alphabet, class p_destination_value>
void append_cpu_prepared_query_recipe_sketch(
    const Datum recipe,
    const pg_sequence_descriptor& descriptor,
    pg_sequence_read_session& session,
    const bioseq::storage::sequence_payload_allocation_observer* payload_observer,
    const std::uint64_t query_ordinal,
    const std::int32_t kmer_size,
    const std::int32_t window_size,
    const minimizer_sketch_backend sketch_backend,
    const ::bioseq::minimizers::minimizer_physical_layout_selection& execution_layout,
    const ::bioseq::simd_dispatch::highway_kernel_table* highway_kernels,
    std::vector<p_destination_value>& destination,
    bioseq::parallelism::memory_permit& destination_permit,
    std::uint64_t& retained_destination_bytes,
    std::vector<batch_processor::cpu_prepared_query_sketch>* retained_queries,
    bioseq::parallelism::memory_permit* retained_query_permit,
    std::uint64_t* retained_query_bytes) {
    auto* query_owner = bioseq::postgres::support::invoke_pg_as_cpp([&session, recipe, descriptor] {
        return ::make_pg_sequence_call_owner<p_alphabet>(session, recipe, descriptor);
    });
    append_cpu_prepared_query_sketch_view<p_alphabet, p_destination_value>(
        query_owner->view().with_payload_allocation_observer(payload_observer),
        query_ordinal,
        kmer_size,
        window_size,
        sketch_backend,
        execution_layout,
        highway_kernels,
        destination,
        destination_permit,
        retained_destination_bytes,
        retained_queries,
        retained_query_permit,
        retained_query_bytes);
}

/** @return The terminal state of independently admitted query preparation. */
template <class p_alphabet, std::size_t p_width>
[[nodiscard]] batch_processor::cpu_preparation_status prepare_independently_admitted_query_sketch(
    const bioseq::pg_types::seq& header,
    const std::uint64_t query_ordinal,
    const std::int32_t kmer_size,
    const std::int32_t window_size,
    const minimizer_sketch_backend sketch_backend,
    const ::bioseq::minimizers::minimizer_physical_layout_selection& execution_layout,
    const ::bioseq::simd_dispatch::highway_kernel_table* highway_kernels,
    batch_processor::cpu_prepared_query_sketch& destination) {
    using sequence_t = compact_sequence_stdc_instance<bioseq::pg_types::seq, p_alphabet, p_width>;
    compact_sequence_range<sequence_t> query{header};
    auto query_owner = bioseq::storage::make_resident_lazy_sequence(query);
    const auto query_view = bioseq::storage::make_lazy_sequence_view(query_owner);
    return prepare_physical_query_sketch_view(query_view,
                                              query_ordinal,
                                              kmer_size,
                                              window_size,
                                              sketch_backend,
                                              execution_layout,
                                              highway_kernels,
                                              destination);
}

/** @return The immutable callable table for one alphabet and storage width. */
template <class p_alphabet, std::size_t p_width>
[[nodiscard]] const batch_processor::cpu_preparation_typmod_operations& typmod_ops() noexcept {
    static constexpr batch_processor::cpu_preparation_typmod_operations operations{
        .alphabet_code =
            static_cast<std::uint8_t>(pgseq_supported_alphabets::index_of<p_alphabet>()),
        .storage_unit_byte_width = static_cast<std::uint8_t>(p_width),
        .select_query_layout = &select_cpu_query_execution_layout<p_alphabet, p_width>,
        .append_header_sketch =
            &append_cpu_prepared_query_sketch<p_alphabet, p_width, minimap_sketch_data>,
        .append_header_u32 = &append_cpu_prepared_query_sketch<p_alphabet, p_width, std::uint32_t>,
        .append_header_u64 = &append_cpu_prepared_query_sketch<p_alphabet, p_width, std::uint64_t>,
        .append_recipe_sketch =
            &append_cpu_prepared_query_recipe_sketch<p_alphabet, minimap_sketch_data>,
        .append_recipe_u32 = &append_cpu_prepared_query_recipe_sketch<p_alphabet, std::uint32_t>,
        .append_recipe_u64 = &append_cpu_prepared_query_recipe_sketch<p_alphabet, std::uint64_t>,
        .prepare_independent = &prepare_independently_admitted_query_sketch<p_alphabet, p_width>,
    };
    static_assert(operations.complete());
    return operations;
}

template const batch_processor::cpu_preparation_typmod_operations&
typmod_ops<seq::dna4, 8>() noexcept;
#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
template const batch_processor::cpu_preparation_typmod_operations&
typmod_ops<seq::dna5, 8>() noexcept;
#endif
#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_COORDINATES)
template const batch_processor::cpu_preparation_typmod_operations&
typmod_ops<seq::dna4, 1>() noexcept;
template const batch_processor::cpu_preparation_typmod_operations&
typmod_ops<seq::dna4, 2>() noexcept;
template const batch_processor::cpu_preparation_typmod_operations&
typmod_ops<seq::dna4, 4>() noexcept;
template const batch_processor::cpu_preparation_typmod_operations&
typmod_ops<seq::dna5, 1>() noexcept;
template const batch_processor::cpu_preparation_typmod_operations&
typmod_ops<seq::dna5, 2>() noexcept;
template const batch_processor::cpu_preparation_typmod_operations&
typmod_ops<seq::dna5, 4>() noexcept;
#endif

} // namespace bioseq::simd::cpu_preparation_detail
