/*-------------------------------------------------------------------------
 *
 * scheduler/dispatch_buffer_api_impl.cppm
 *
 * Implementation of the narrow TupleDispatchBuffer API declared in
 * dispatch_buffer_api.cppm.  This is the only custom-scan-side translation
 * unit that imports bioseq.scheduler.dispatch_buffer.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

module bioseq.scheduler.dispatch_buffer_api;

import bioseq.highway.dispatch_contract;
import bioseq.scheduler.dispatch_buffer;
import bioseq.sequences.minimap_port;
import bioseq.sequences.pipe;
import bioseq.indexing.type;
import bioseq.types.pg_compact_sequence;
import bioseq.storage.scanner_pg;

namespace bioseq::scheduler {

bioseq::simd_dispatch::HighwayDispatchInfo
highway_dispatch_info_for_plan() noexcept {
    return bioseq::simd_dispatch::registered_highway_dispatch_info();
}

namespace {

using BufferImpl = TupleDispatchBuffer;

[[nodiscard]] BufferImpl* as_buffer(void* ptr) noexcept {
    return static_cast<BufferImpl*>(ptr);
}

[[nodiscard]] const BufferImpl* as_buffer(const void* ptr) noexcept {
    return static_cast<const BufferImpl*>(ptr);
}

} // namespace

DispatchBufferRuntime::DispatchBufferRuntime(
    const std::size_t estimated_tuple_count)
    : impl(new BufferImpl(estimated_tuple_count)) {}

DispatchBufferRuntime::~DispatchBufferRuntime() {
    delete as_buffer(impl);
    impl = nullptr;
}

DispatchBufferRuntime::DispatchBufferRuntime(
    DispatchBufferRuntime&& rhs) noexcept
    : impl(rhs.impl) {
    rhs.impl = nullptr;
}

DispatchBufferRuntime& DispatchBufferRuntime::operator=(
    DispatchBufferRuntime&& rhs) noexcept {
    if (this != &rhs) {
        delete as_buffer(impl);
        impl = rhs.impl;
        rhs.impl = nullptr;
    }
    return *this;
}

void DispatchBufferRuntime::configure_params(
    const bioseq::simd::Minimap2Params& params,
    const bioseq::indexing::IndexReadRequirement index_read_requirement,
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

void DispatchBufferRuntime::replace_executor_snapshot(
    Snapshot executor_snapshot) {
    as_buffer(impl)->replace_executor_snapshot(executor_snapshot);
}

void DispatchBufferRuntime::bind_sequence_source_provider(
        bioseq::storage::pg_sequence_source_provider* provider) {
    as_buffer(impl)->bind_sequence_source_provider(provider);
}

void DispatchBufferRuntime::clear_sequence_source_provider_binding() noexcept {
    if (impl != nullptr) {
        as_buffer(impl)->clear_sequence_source_provider_binding();
    }
}

void DispatchBufferRuntime::bind_persisted_gpu_operation_evidence_sink(
    bioseq::pipeline_stats::PersistedGpuOperationEvidenceLedger& sink) {
    as_buffer(impl)->bind_persisted_gpu_operation_evidence_sink(sink);
}

void DispatchBufferRuntime::bind_slice_async_owner(
    slice_async_owner_guard* owner) {
    as_buffer(impl)->bind_slice_async_owner(owner);
}

void DispatchBufferRuntime::release_per_row_slice_async_owner_after_drain() {
    as_buffer(impl)->release_per_row_slice_async_owner_after_drain();
}

void DispatchBufferRuntime::release_join_slice_async_owner_after_drain() {
	as_buffer(impl)->release_join_slice_async_owner_after_drain();
}

void DispatchBufferRuntime::configure_mmsearch(
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

void DispatchBufferRuntime::configure_mmsearch_per_row(
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

void DispatchBufferRuntime::configure_mmsearch_per_row_evaluated(
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

void DispatchBufferRuntime::apply_typmod_coalesce(
    std::vector<char> coalesced_bytes) {
    as_buffer(impl)->apply_typmod_coalesce(std::move(coalesced_bytes));
}

void DispatchBufferRuntime::clear_mmsearch_context(const bool clear_extras) {
    as_buffer(impl)->clear_mmsearch_context(clear_extras);
}

void DispatchBufferRuntime::add_extra_const_query(
    std::vector<char> query_varlena_bytes) {
    as_buffer(impl)->add_extra_const_query(std::move(query_varlena_bytes));
}

void DispatchBufferRuntime::configure_mmsearch_join(
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

void DispatchBufferRuntime::begin_join_target(
    TupleTableSlot* locator_passthrough_target) {
    as_buffer(impl)->begin_join_target(locator_passthrough_target);
}

namespace {

[[nodiscard]] std::vector<BufferImpl::JoinQueryRef> translate_join_queries(
    const std::span<const DispatchJoinQueryRef> queries) {
    std::vector<BufferImpl::JoinQueryRef> translated;
    translated.reserve(queries.size());
    for (const DispatchJoinQueryRef& query : queries) {
        translated.push_back(BufferImpl::JoinQueryRef{
            query.ordinal, query.query_slot});
    }
    return translated;
}

} // namespace

void DispatchBufferRuntime::prepare_join_target_queries(
    const std::span<const DispatchJoinQueryRef> all_queries) {
    const auto translated = translate_join_queries(all_queries);
    as_buffer(impl)->prepare_join_target_queries(translated);
}

void DispatchBufferRuntime::set_join_query_batch(
    const std::span<const DispatchJoinQueryRef> queries) {
    const auto translated = translate_join_queries(queries);
    as_buffer(impl)->set_join_query_batch(translated);
}

bool DispatchBufferRuntime::run_join_query_batch_and_build_cursors() {
    return as_buffer(impl)->run_join_query_batch_and_build_cursors();
}

void DispatchBufferRuntime::finish_join_target() {
    as_buffer(impl)->finish_join_target();
}

void DispatchBufferRuntime::reset_join_runtime_for_rescan() {
    as_buffer(impl)->reset_join_runtime_for_rescan();
}

void DispatchBufferRuntime::abandon_join_runtime_after_pg_error_noexcept()
    noexcept {
    if (impl != nullptr) {
        as_buffer(impl)->abandon_join_runtime_after_pg_error_noexcept();
    }
}

void DispatchBufferRuntime::submit_scan(TupleTableSlot* scan_slot) {
    as_buffer(impl)->submit_scan(scan_slot);
}

void DispatchBufferRuntime::submit_scan_evaluated(
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

bool DispatchBufferRuntime::ready_to_dispatch() const {
    return as_buffer(impl)->ready_to_dispatch();
}

bool DispatchBufferRuntime::run_batch_and_build_cursors() {
    return as_buffer(impl)->run_batch_and_build_cursors();
}

void DispatchBufferRuntime::clear_slots() {
    as_buffer(impl)->clear_slots();
}

std::optional<DispatchNextOutput> DispatchBufferRuntime::read_next_output() {
    auto next = as_buffer(impl)->read_next_output();
    if (!next.has_value()) {
        return std::nullopt;
    }
    return DispatchNextOutput{
        next->entry->scan_slot_copy,
        next->entry->sequence_rowno,
        next->query_ordinal,
        next->match,
    };
}

bioseq::pipeline_stats::PipelineExecutionStats
DispatchBufferRuntime::execution_stats() const {
    return as_buffer(impl)->execution_stats();
}

std::size_t DispatchBufferRuntime::extra_query_count() const {
    return as_buffer(impl)->extra_query_count();
}

bool DispatchBufferRuntime::has_finished_writing() const {
    return as_buffer(impl)->has_finished_writing();
}

bool DispatchBufferRuntime::mmsearch_active() const {
    return as_buffer(impl)->mmsearch_active();
}

bool DispatchBufferRuntime::per_row_self_alignment() const {
    return as_buffer(impl)->per_row_self_alignment();
}

bool DispatchBufferRuntime::coalesce_active() const {
    return as_buffer(impl)->coalesce_active();
}

std::uint8_t DispatchBufferRuntime::mmsearch_alphabet_code() const {
    return as_buffer(impl)->mmsearch_alphabet_code();
}

std::uint8_t DispatchBufferRuntime::mmsearch_storage_unit_byte_width() const {
    return as_buffer(impl)->mmsearch_storage_unit_byte_width();
}

AttrNumber DispatchBufferRuntime::target_sequence_attno() const noexcept {
    return impl == nullptr
        ? InvalidAttrNumber : as_buffer(impl)->target_sequence_attno();
}

AttrNumber DispatchBufferRuntime::per_row_query_sequence_attno() const noexcept {
    return impl == nullptr
        ? InvalidAttrNumber
        : as_buffer(impl)->per_row_query_sequence_attno();
}

} // namespace bioseq::scheduler
