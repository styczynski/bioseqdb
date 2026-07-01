/*-------------------------------------------------------------------------
 *
 * scheduler/dispatch_buffer_api.cpp
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
#include <string>
#include <utility>
#include <vector>

module bioseq.scheduler.dispatch_buffer_api;

import bioseq.scheduler.dispatch_buffer;
import bioseq.sequences.simd;
import bioseq.sequences.minimap_port;

namespace bioseq::scheduler {

namespace {

using BufferImpl = TupleDispatchBuffer;

[[nodiscard]] BufferImpl* as_buffer(void* ptr) noexcept {
    return static_cast<BufferImpl*>(ptr);
}

[[nodiscard]] const BufferImpl* as_buffer(const void* ptr) noexcept {
    return static_cast<const BufferImpl*>(ptr);
}

[[nodiscard]] DispatchGpuBatchExecutionStats copy_gpu_stats(
    const bioseq::simd::active_target::GpuBatchExecutionStats& src) {
    DispatchGpuBatchExecutionStats dst{};
    dst.gpu_batched_branch_taken = src.gpu_batched_branch_taken;
    dst.gpu_problem_count = src.gpu_problem_count;
    dst.gpu_wave_count = src.gpu_wave_count;
    dst.gpu_segment_indexes_built = src.gpu_segment_indexes_built;
    dst.gpu_segment_indexes_reused = src.gpu_segment_indexes_reused;
    dst.gpu_seed_dispatches = src.gpu_seed_dispatches;
    dst.gpu_chain_dispatches = src.gpu_chain_dispatches;
    dst.gpu_extend_dispatches = src.gpu_extend_dispatches;
    dst.cpu_fallback_rows = src.cpu_fallback_rows;
    dst.simd_fallback_rows = src.simd_fallback_rows;
    dst.vulkan_declines = src.vulkan_declines;
    dst.gpu_batch_pipeline_ns = src.gpu_batch_pipeline_ns;
    dst.gpu_query_sketch_ns = src.gpu_query_sketch_ns;
    dst.gpu_target_dedup_ns = src.gpu_target_dedup_ns;
    dst.gpu_target_plan_ns = src.gpu_target_plan_ns;
    dst.gpu_index_build_ns = src.gpu_index_build_ns;
    dst.gpu_problem_build_ns = src.gpu_problem_build_ns;
    dst.gpu_seed_chain_runtime_ns = src.gpu_seed_chain_runtime_ns;
    dst.gpu_cigar_jobs = src.gpu_cigar_jobs;
    dst.gpu_cigar_bounded_jobs = src.gpu_cigar_bounded_jobs;
    dst.gpu_cigar_exact_jobs = src.gpu_cigar_exact_jobs;
    dst.gpu_cigar_unit_gap_jobs = src.gpu_cigar_unit_gap_jobs;
    dst.gpu_cigar_gridded_jobs = src.gpu_cigar_gridded_jobs;
    dst.gpu_cigar_query_symbols = src.gpu_cigar_query_symbols;
    dst.gpu_cigar_target_symbols = src.gpu_cigar_target_symbols;
    dst.gpu_cigar_packed_words = src.gpu_cigar_packed_words;
    dst.gpu_cigar_traceback_words = src.gpu_cigar_traceback_words;
    dst.gpu_cigar_prepare_ns = src.gpu_cigar_prepare_ns;
    dst.gpu_cigar_symbol_pack_ns = src.gpu_cigar_symbol_pack_ns;
    dst.gpu_cigar_classify_ns = src.gpu_cigar_classify_ns;
    dst.gpu_cigar_runtime_ns = src.gpu_cigar_runtime_ns;
    dst.gpu_cigar_decode_ns = src.gpu_cigar_decode_ns;
    return dst;
}

[[nodiscard]] DispatchBatchExecutionStats copy_stats(
    const bioseq::simd::active_target::BatchExecutionStats& src) {
    DispatchBatchExecutionStats dst{};
    dst.targets_considered = src.targets_considered;
    dst.targets_with_prebuilt_index = src.targets_with_prebuilt_index;
    dst.targets_recomputed_minimizers = src.targets_recomputed_minimizers;
    dst.gpu = copy_gpu_stats(src.gpu);
    return dst;
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
    const bioseq::simd::active_target::Minimap2Params& params,
    const bool read_sequence_index,
    const Oid scan_user_relid,
    const Oid scan_user_database_oid,
    const Oid scan_user_tablespace_oid,
    const Oid scan_user_main_relfilenode) {
    as_buffer(impl)->configure_params(
        params,
        read_sequence_index,
        scan_user_relid,
        scan_user_database_oid,
        scan_user_tablespace_oid,
        scan_user_main_relfilenode);
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
    const AttrNumber primary_key_attno) {
    as_buffer(impl)->configure_mmsearch_per_row(
        seq_attno,
        std::move(column_name),
        per_row_query_attno,
        per_row_self_alignment,
        alphabet_code,
        storage_unit_byte_width,
        primary_key_attno);
}

void DispatchBufferRuntime::apply_typmod_coalesce(
    std::vector<char> coalesced_bytes) {
    as_buffer(impl)->apply_typmod_coalesce(std::move(coalesced_bytes));
}

void DispatchBufferRuntime::mark_alphabet_mismatch_recompute() {
    as_buffer(impl)->mark_alphabet_mismatch_recompute();
}

void DispatchBufferRuntime::clear_mmsearch_context(const bool clear_extras) {
    as_buffer(impl)->clear_mmsearch_context(clear_extras);
}

void DispatchBufferRuntime::add_extra_const_query(
    std::vector<char> query_varlena_bytes) {
    as_buffer(impl)->add_extra_const_query(std::move(query_varlena_bytes));
}

void DispatchBufferRuntime::submit_scan(TupleTableSlot* scan_slot) {
    as_buffer(impl)->submit_scan(scan_slot);
}

void DispatchBufferRuntime::run_batch_and_build_cursors() {
    as_buffer(impl)->run_batch_and_build_cursors();
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
        next->match,
    };
}

DispatchBatchExecutionStats DispatchBufferRuntime::execution_stats() const {
    return copy_stats(as_buffer(impl)->execution_stats());
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

bool DispatchBufferRuntime::recompute_due_to_alphabet_mismatch() const {
    return as_buffer(impl)->recompute_due_to_alphabet_mismatch();
}

std::uint8_t DispatchBufferRuntime::mmsearch_alphabet_code() const {
    return as_buffer(impl)->mmsearch_alphabet_code();
}

std::uint8_t DispatchBufferRuntime::mmsearch_storage_unit_byte_width() const {
    return as_buffer(impl)->mmsearch_storage_unit_byte_width();
}

std::uint8_t DispatchBufferRuntime::raw_query_alphabet_code() const {
    return as_buffer(impl)->raw_query_alphabet_code();
}

std::uint8_t DispatchBufferRuntime::raw_query_storage_unit_byte_width() const {
    return as_buffer(impl)->raw_query_storage_unit_byte_width();
}

} // namespace bioseq::scheduler
