/**
 * @file api_implementation.cppm
 * @brief Adapts the narrow dispatch-buffer API to its private implementation.
 */
/*-------------------------------------------------------------------------
 *
 * scheduler/dispatch_buffer/api_implementation.cppm
 *
 * Implementation of the narrow tuple_dispatch_buffer API declared in
 * dispatch_buffer/api.cppm.  This is the only custom-scan-side translation
 * unit that imports bioseq.postgres.scheduler.dispatch_buffer.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <bioseq/pg_compat.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

module bioseq.postgres.scheduler.dispatch_buffer.api;

import bioseq.highway.dispatch_contract;
import bioseq.postgres.scheduler.dispatch_buffer;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.indexing.type;
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.storage.scanner;

namespace bioseq::scheduler {

/** @return The immutable Highway plan registered for scheduler dispatch. */
bioseq::simd_dispatch::highway_dispatch_plan
highway_dispatch_plan_for_plan() noexcept {
    return bioseq::simd_dispatch::registered_highway_dispatch_plan();
}

namespace {

using buffer_implementation = tuple_dispatch_buffer;

/** @return The private mutable buffer represented by an opaque API pointer. */
[[nodiscard]] buffer_implementation* as_buffer(void* ptr) noexcept {
    return static_cast<buffer_implementation*>(ptr);
}

/** @return The private const buffer represented by an opaque API pointer. */
[[nodiscard]] const buffer_implementation* as_buffer(const void* ptr) noexcept {
    return static_cast<const buffer_implementation*>(ptr);
}

} // namespace

dispatch_buffer_runtime::dispatch_buffer_runtime(
    const std::size_t estimated_tuple_count)
    : impl(new buffer_implementation(estimated_tuple_count)) {}

dispatch_buffer_runtime::~dispatch_buffer_runtime() {
    delete as_buffer(impl);
    impl = nullptr;
}

dispatch_buffer_runtime::dispatch_buffer_runtime(
    dispatch_buffer_runtime&& rhs) noexcept
    : impl(rhs.impl) {
    rhs.impl = nullptr;
}

dispatch_buffer_runtime& dispatch_buffer_runtime::operator=(
    dispatch_buffer_runtime&& rhs) noexcept {
    if (this != &rhs) {
        delete as_buffer(impl);
        impl = rhs.impl;
        rhs.impl = nullptr;
    }
    return *this;
}

void dispatch_buffer_runtime::configure_params(
    const bioseq::simd::minimap2_parameters& params,
    const bioseq::indexing::index_read_requirement index_read_requirement,
    const bool read_sequence_index,
    const Oid scan_user_relid,
    const Oid scan_user_database_oid,
    const Oid scan_user_tablespace_oid,
    const Oid scan_user_main_relfilenode,
    Relation executor_relation,
    Snapshot executor_snapshot,
    MemoryContext executor_query_context) {
    as_buffer(impl)->configure_params(
        params,
        index_read_requirement,
        read_sequence_index,
        scan_user_relid,
        scan_user_database_oid,
        scan_user_tablespace_oid,
        scan_user_main_relfilenode,
        executor_relation,
        executor_snapshot,
        executor_query_context);
}

void dispatch_buffer_runtime::replace_executor_snapshot(
    Snapshot executor_snapshot) {
    as_buffer(impl)->replace_executor_snapshot(executor_snapshot);
}

void dispatch_buffer_runtime::bind_sequence_source_provider(
        bioseq::storage::pg_sequence_source_provider* provider) {
    as_buffer(impl)->bind_sequence_source_provider(provider);
}

void dispatch_buffer_runtime::clear_sequence_source_provider_binding() noexcept {
    if (impl != nullptr) {
        as_buffer(impl)->clear_sequence_source_provider_binding();
    }
}

void dispatch_buffer_runtime::bind_persisted_gpu_operation_stats_sink(
    bioseq::pipeline_stats::persisted_gpu_operation_accumulator& sink) {
    as_buffer(impl)->bind_persisted_gpu_operation_stats_sink(sink);
}

void dispatch_buffer_runtime::bind_slice_async_owner(
    slice_async_owner_guard* owner) {
    as_buffer(impl)->bind_slice_async_owner(owner);
}

void dispatch_buffer_runtime::release_per_row_slice_async_owner_after_drain() {
    as_buffer(impl)->release_per_row_slice_async_owner_after_drain();
}

void dispatch_buffer_runtime::release_join_slice_async_owner_after_drain() {
	as_buffer(impl)->release_join_slice_async_owner_after_drain();
}

void dispatch_buffer_runtime::configure_mmsearch(
    const AttrNumber seq_attno,
    std::string column_name,
    std::vector<char> query_varlena_bytes,
    const std::uint8_t alphabet_code,
    const std::uint8_t storage_unit_byte_width,
    const AttrNumber primary_key_attno) {
    as_buffer(impl)->configure_mmsearch(
        seq_attno,
        std::move(column_name),
        std::move(query_varlena_bytes),
        alphabet_code,
        storage_unit_byte_width,
        primary_key_attno);
}

void dispatch_buffer_runtime::configure_mmsearch_per_row(
    const AttrNumber seq_attno,
    std::string column_name,
    const AttrNumber per_row_query_attno,
    const bool per_row_self_alignment,
    const std::uint8_t alphabet_code,
    const std::uint8_t storage_unit_byte_width,
    const std::uint8_t query_alphabet_code,
    const std::uint8_t query_storage_unit_byte_width,
    const AttrNumber primary_key_attno) {
    as_buffer(impl)->configure_mmsearch_per_row(
        seq_attno,
        std::move(column_name),
        per_row_query_attno,
        per_row_self_alignment,
        alphabet_code,
        storage_unit_byte_width,
        query_alphabet_code,
        query_storage_unit_byte_width,
        primary_key_attno);
}

void dispatch_buffer_runtime::configure_mmsearch_per_row_evaluated(
    const AttrNumber seq_attno,
    std::string column_name,
    const std::uint8_t alphabet_code,
    const std::uint8_t storage_unit_byte_width,
    const std::uint8_t query_alphabet_code,
    const std::uint8_t query_storage_unit_byte_width,
    const AttrNumber primary_key_attno) {
    as_buffer(impl)->configure_mmsearch_per_row_evaluated(
        seq_attno,
        std::move(column_name),
        alphabet_code,
        storage_unit_byte_width,
        query_alphabet_code,
        query_storage_unit_byte_width,
        primary_key_attno);
}

void dispatch_buffer_runtime::apply_typmod_coalesce(
    std::vector<char> coalesced_bytes) {
    as_buffer(impl)->apply_typmod_coalesce(std::move(coalesced_bytes));
}

void dispatch_buffer_runtime::clear_mmsearch_context(const bool clear_extras) {
    as_buffer(impl)->clear_mmsearch_context(clear_extras);
}

void dispatch_buffer_runtime::add_extra_const_query(
    std::vector<char> query_varlena_bytes) {
    as_buffer(impl)->add_extra_const_query(std::move(query_varlena_bytes));
}

void dispatch_buffer_runtime::configure_mmsearch_join(
    const Oid target_relation_oid,
    const AttrNumber target_storage_seq_attno,
    const AttrNumber target_slot_seq_resno,
    const AttrNumber target_slot_rowno_resno,
    const AttrNumber query_slot_seq_resno,
    std::string target_column_name,
    const std::uint8_t alphabet_code,
    const std::uint8_t storage_unit_byte_width,
    const std::uint8_t query_alphabet_code,
    const std::uint8_t query_storage_unit_byte_width) {
    as_buffer(impl)->configure_mmsearch_join(
        target_relation_oid,
        target_storage_seq_attno,
        target_slot_seq_resno,
        target_slot_rowno_resno,
        query_slot_seq_resno,
        std::move(target_column_name),
        alphabet_code,
        storage_unit_byte_width,
        query_alphabet_code,
        query_storage_unit_byte_width);
}

void dispatch_buffer_runtime::begin_join_target(
    TupleTableSlot* locator_passthrough_target) {
    as_buffer(impl)->begin_join_target(locator_passthrough_target);
}

namespace {

/** @return Internal join-query references preserving ordinal and slot identity. */
[[nodiscard]] std::vector<buffer_implementation::join_query_ref> translate_join_queries(
    const std::span<const dispatch_join_query_ref> queries) {
    std::vector<buffer_implementation::join_query_ref> translated;
    translated.reserve(queries.size());
    for (const dispatch_join_query_ref& query : queries) {
        translated.push_back(buffer_implementation::join_query_ref{
            query.ordinal, query.query_slot});
    }
    return translated;
}

} // namespace

void dispatch_buffer_runtime::prepare_join_target_queries(
    const std::span<const dispatch_join_query_ref> all_queries) {
    const auto translated = translate_join_queries(all_queries);
    as_buffer(impl)->prepare_join_target_queries(translated);
}

void dispatch_buffer_runtime::set_join_query_batch(
    const std::span<const dispatch_join_query_ref> queries) {
    const auto translated = translate_join_queries(queries);
    as_buffer(impl)->set_join_query_batch(translated);
}

bool dispatch_buffer_runtime::run_join_query_batch_and_build_cursors() {
    return as_buffer(impl)->run_join_query_batch_and_build_cursors();
}

void dispatch_buffer_runtime::finish_join_target() {
    as_buffer(impl)->finish_join_target();
}

void dispatch_buffer_runtime::reset_join_runtime_for_rescan() {
    as_buffer(impl)->reset_join_runtime_for_rescan();
}

void dispatch_buffer_runtime::abandon_join_runtime_after_pg_error_noexcept()
    noexcept {
    if (impl != nullptr) {
        as_buffer(impl)->abandon_join_runtime_after_pg_error_noexcept();
    }
}

void dispatch_buffer_runtime::submit_scan(TupleTableSlot* scan_slot) {
    as_buffer(impl)->submit_scan(scan_slot);
}

void dispatch_buffer_runtime::submit_scan_evaluated(
    TupleTableSlot* scan_slot,
    const Datum target,
    const bool target_is_null,
    const bool has_query_override,
    const Datum query,
    const bool query_is_null) {
    as_buffer(impl)->submit_scan_evaluated(
        scan_slot,
        target,
        target_is_null,
        has_query_override,
        query,
        query_is_null);
}

bool dispatch_buffer_runtime::ready_to_dispatch() const {
    return as_buffer(impl)->ready_to_dispatch();
}

bool dispatch_buffer_runtime::run_batch_and_build_cursors() {
    return as_buffer(impl)->run_batch_and_build_cursors();
}

void dispatch_buffer_runtime::clear_slots() {
    as_buffer(impl)->clear_slots();
}

std::optional<dispatch_next_output> dispatch_buffer_runtime::read_next_output() {
    auto next = as_buffer(impl)->read_next_output();
    if (!next.has_value()) {
        return std::nullopt;
    }
    return dispatch_next_output{
        next->entry->scan_slot_copy,
        next->entry->sequence_rowno,
        next->query_ordinal,
        next->match,
    };
}

bioseq::pipeline_stats::pipeline_execution_stats
dispatch_buffer_runtime::execution_stats() const {
    return as_buffer(impl)->execution_stats();
}

std::size_t dispatch_buffer_runtime::extra_query_count() const {
    return as_buffer(impl)->extra_query_count();
}

bool dispatch_buffer_runtime::has_finished_writing() const {
    return as_buffer(impl)->has_finished_writing();
}

bool dispatch_buffer_runtime::mmsearch_active() const {
    return as_buffer(impl)->mmsearch_active();
}

bool dispatch_buffer_runtime::per_row_self_alignment() const {
    return as_buffer(impl)->per_row_self_alignment();
}

bool dispatch_buffer_runtime::coalesce_active() const {
    return as_buffer(impl)->coalesce_active();
}

std::uint8_t dispatch_buffer_runtime::mmsearch_alphabet_code() const {
    return as_buffer(impl)->mmsearch_alphabet_code();
}

std::uint8_t dispatch_buffer_runtime::mmsearch_storage_unit_byte_width() const {
    return as_buffer(impl)->mmsearch_storage_unit_byte_width();
}

AttrNumber dispatch_buffer_runtime::target_sequence_attno() const noexcept {
    return impl == nullptr
        ? InvalidAttrNumber : as_buffer(impl)->target_sequence_attno();
}

AttrNumber dispatch_buffer_runtime::per_row_query_sequence_attno() const noexcept {
    return impl == nullptr
        ? InvalidAttrNumber
        : as_buffer(impl)->per_row_query_sequence_attno();
}

} // namespace bioseq::scheduler
