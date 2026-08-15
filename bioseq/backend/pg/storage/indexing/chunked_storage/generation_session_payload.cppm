/**
 * @file generation_session_payload.cppm
 * @brief Defines grouped-generation payload planning and section admission.
 */
module;
#include <bioseq/pg_compat.h>
#include <libassert/assert.hpp>

extern "C" {
#include "access/heapam.h"
#include "access/nbtree.h"
#include "access/parallel.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h" // RelnameGetRelid
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_opclass_d.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_type_d.h"
#include "commands/sequence.h"
#include "commands/tablespace.h"
#include "executor/instrument.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "storage/buffile.h"
#include "storage/dsm.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/procarray.h"
#include "storage/relfilelocator.h"
#include "storage/shm_toc.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/xid8.h"

#include "common/relpath.h"
#include "miscadmin.h"
#include "port.h"
#include "port/atomics.h"
}

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <expected>
#include <fcntl.h>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <queue>
#include <span>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <tuple>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>
export module bioseq.postgres.storage.indexing.chunked_storage:generation_session_payload;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.memory_budget;
import bioseq.postgres.support; // bioseq::postgres::support::try_open_postgres_relation, bioseq::postgres::support::find_primary_key_oid
import bioseq.postgres.support.datums;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.storage.manager; // seq_caches, sequence_locator, format helpers
import bioseq.storage.layout;           // sequence_locator
import bioseq.storage.size;             // sequence_size_t
import bioseq.indexing.type; // sequence_index_layout, index_sub_key_t, k_max_index_chunk_size_bytes, sentinels
import bioseq.indexing.storage;           // the concepts
import bioseq.indexing.minimizer_hashmap; // encoded header/bucket validation
import bioseq.indexing.minimizer_fragment_image; // explicit fixed-endian minimizer fragment image image ABI
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.minimizer_fragment_compact_hash32_adapter;
import bioseq.postgres.table.io; // begin_column_read/read_next_column_row/end_column_read/column_read_state
import bioseq.postgres.storage.scanner;        // PostgreSQL-backed lazy sequence access
import bioseq.storage.lazy_sequence;           // lazy_sequence
import bioseq.postgres.types.compact_sequence; // bioseq::pg_types::seq, dispatch_instance
import bioseq.sequences;                       // compact_sequence_range
import bioseq.sequences.simd;                  // canonical bounded base decoding
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.postgres.storage.indexing.residency;
import bioseq.postgres.storage.indexing.sorted_runs;
import bioseq.instrumentation.clock;
import bioseq.postgres.column.table_access;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.physical;
import bioseq.postgres.storage.indexing.sql_row_codecs;
import bioseq.postgres.storage.manager.sql_row_codecs;
import bioseq.postgres.sql.generated.core.storage_indexing_pg_chunked_storage.entity_handles;
import :catalog;
import :fragment_image;
export import :generation_session_contract;
export import :generation_session_lifecycle;
export import :generation_session_metadata;
export namespace bioseq::storage::indexing {

inline void index_storage_fragment_generation_read_session::plan_section_groups_(
    payload_section section, std::span<const minimizer_generation_payload_read_group> groups) {
    try {
        const bool position_section = section == payload_section::positions;
        const operation_state required_state =
            position_section ? operation_state::controls_complete : operation_state::metadata_ready;
        if (state_ != required_state || !metadata_) {
            fail_(position_section
                      ? "position payload requests were planned before control payload completed "
                        "or more than once"
                      : "control payload requests were planned before metadata or more than once");
        }
        if (!planned_groups_.empty() || next_group_ != 0 || plan_permit_ ||
            !runtime_requests_.empty() || runtime_permit_ || active_scan_ != nullptr) {
            fail_("payload section planning found leaked prior-section state");
        }
        if (!position_section &&
            (!authorized_position_ordinals_.empty() || control_authorization_permit_)) {
            fail_("control payload authorization state was already populated");
        }
        std::uint64_t request_count = 0;
        std::uint64_t requested_rows = 0;
        std::uint64_t requested_bytes = 0;
        for (const minimizer_generation_payload_read_group& group : groups) {
            checked_add_(
                request_count, group.requests.size(), "payload plan request count overflow");
            for (const index_sub_key_request& request : group.requests) {
                checked_add_(requested_rows,
                             request.expected_chunk_count,
                             "payload requested-row telemetry overflow");
                checked_add_(requested_bytes,
                             request.expected_total_bytes,
                             "payload requested-byte telemetry overflow");
            }
        }
        std::uint64_t plan_bytes = 0;
        if (groups.size() > (std::numeric_limits<std::uint64_t>::max)() / sizeof(planned_group) ||
            request_count >
                (std::numeric_limits<std::uint64_t>::max)() / sizeof(index_sub_key_request) ||
            (!position_section &&
             request_count > (std::numeric_limits<std::uint64_t>::max)() / sizeof(std::uint32_t))) {
            fail_("payload plan admission size overflow");
        }
        plan_bytes = groups.size() * sizeof(planned_group);
        checked_add_(plan_bytes,
                     request_count * sizeof(index_sub_key_request),
                     "payload plan admission size overflow");
        if (!position_section && request_count != 0) {
            const std::uint64_t authorization_bytes = request_count * sizeof(std::uint32_t);
            auto authorization_permit =
                bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                    authorization_bytes);
            if (!authorization_permit) {
                fail_("control payload authorization memory admission was denied");
            }
            control_authorization_permit_ = std::move(*authorization_permit);
            authorized_position_ordinals_.clear();
            authorized_position_ordinals_.reserve(static_cast<std::size_t>(request_count));
        }
        if (plan_bytes != 0) {
            auto plan_permit =
                bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                    plan_bytes);
            if (!plan_permit) {
                fail_("payload plan memory admission was denied");
            }
            plan_permit_ = std::move(*plan_permit);
        }
        planned_groups_.clear();
        planned_groups_.reserve(groups.size());
        std::optional<index_sub_key_range> previous_range;
        const auto entries = metadata_->directory.entries();
        const std::uint32_t extent_payload_bytes =
            bioseq::indexing::minimizer_extent_payload_bytes(metadata_->manifest.layout);
        if (extent_payload_bytes == 0) {
            fail_("full-generation wire quantum is invalid");
        }
        std::uint64_t authorization_checks = 0;
        for (const minimizer_generation_payload_read_group& input : groups) {
            const index_sub_key_range& range = input.range;
            const auto projected_range = project_extent_range_(range);
            if (range.generation_id != metadata_->manifest.generation_id ||
                range.position_segment < 0 || !projected_range ||
                projected_range->position_extent != position_section || input.requests.empty()) {
                fail_(position_section ? "position payload group range is empty, unqualified, or "
                                         "outside the position namespace"
                                       : "control payload group range is empty, unqualified, or "
                                         "outside the control namespace");
            }
            if (previous_range) {
                const auto previous_key = std::tuple{previous_range->generation_id,
                                                     previous_range->position_segment,
                                                     previous_range->begin};
                const auto current_key =
                    std::tuple{range.generation_id, range.position_segment, range.begin};
                if (!(previous_key < current_key) ||
                    (previous_range->generation_id == range.generation_id &&
                     previous_range->position_segment == range.position_segment &&
                     range.begin < previous_range->end)) {
                    fail_("payload group ranges are not sorted and disjoint");
                }
            }
            previous_range = range;
            using directory_key_type = std::pair<std::int32_t, index_sub_key_t>;
            const auto entry_less_than_key = [](const grouped_soa_bucket_directory_entry& entry,
                                                const directory_key_type& key) noexcept {
                return directory_key_type{entry.position_segment, entry.sub_key} < key;
            };
            const auto first_entry = std::lower_bound(
                entries.begin(),
                entries.end(),
                directory_key_type{range.position_segment, projected_range->logical_begin},
                entry_less_than_key);
            const auto end_entry = std::lower_bound(
                first_entry,
                entries.end(),
                directory_key_type{range.position_segment, projected_range->logical_end},
                entry_less_than_key);
            const std::size_t first_ordinal =
                static_cast<std::size_t>(first_entry - entries.begin());
            const std::size_t end_ordinal = static_cast<std::size_t>(end_entry - entries.begin());
            if (first_ordinal == end_ordinal) {
                fail_("payload group range covers no directory entry");
            }
            std::size_t request_entry_cursor = first_ordinal;
            std::uint64_t wanted_chunks = 0;
            std::optional<std::tuple<index_sub_key_t, std::uint32_t>> previous_request;
            std::uint64_t previous_request_end = 0;
            for (const index_sub_key_request& request : input.requests) {
                const auto request_key = std::tuple{request.sub_key, request.first_chunk_no};
                if (previous_request && !(*previous_request < request_key)) {
                    fail_("payload requests are not strictly sorted");
                }
                if (previous_request && std::get<0>(*previous_request) == request.sub_key &&
                    request.first_chunk_no < previous_request_end) {
                    fail_("payload request intervals overlap");
                }
                previous_request = request_key;
                previous_request_end = static_cast<std::uint64_t>(request.first_chunk_no) +
                                       request.expected_chunk_count;
                const auto logical_request_sub_key = project_minimizer_extent_sub_key(
                    request.sub_key, projected_range->position_extent);
                if (request.generation_id != range.generation_id ||
                    request.position_segment != range.position_segment ||
                    request.sub_key < range.begin || request.sub_key >= range.end ||
                    !logical_request_sub_key || request.expected_chunk_count == 0 ||
                    previous_request_end == 0 ||
                    previous_request_end - 1u >
                        static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())) {
                    fail_("payload request is outside its qualified range");
                }
                while (request_entry_cursor < end_ordinal &&
                       entries[request_entry_cursor].sub_key < *logical_request_sub_key) {
                    ++request_entry_cursor;
                }
                if (request_entry_cursor == end_ordinal ||
                    entries[request_entry_cursor].sub_key != *logical_request_sub_key ||
                    !request_matches_extent_(
                        request, entries[request_entry_cursor], projected_range->position_extent)) {
                    fail_("payload request does not equal one directory extent");
                }
                const std::uint32_t directory_ordinal = entries[request_entry_cursor].partition_id;
                if (!position_section) {
                    if (!authorized_position_ordinals_.empty() &&
                        authorized_position_ordinals_.back() >= directory_ordinal) {
                        fail_("control payload directory authorizations are not strictly sorted");
                    }
                    authorized_position_ordinals_.push_back(directory_ordinal);
                } else {
                    if (!std::binary_search(authorized_position_ordinals_.begin(),
                                            authorized_position_ordinals_.end(),
                                            directory_ordinal)) {
                        fail_("position payload request has no delivered control extent");
                    }
                    checked_add_(
                        authorization_checks, 1, "position control-authorization counter overflow");
                }
                checked_add_(wanted_chunks,
                             request.expected_chunk_count,
                             "payload wanted chunk count overflow");
            }
            std::uint64_t declared_chunks = 0;
            for (std::size_t ordinal = first_ordinal; ordinal < end_ordinal; ++ordinal) {
                checked_add_(declared_chunks,
                             projected_range->position_extent
                                 ? entries[ordinal].position_chunk_count
                                 : entries[ordinal].control_chunk_count,
                             "payload declared chunk count overflow");
            }
            if (declared_chunks < wanted_chunks) {
                fail_("payload skip budget is smaller than requested chunks");
            }
            planned_groups_.push_back(planned_group{
                .range = range,
                .requests = input.requests,
                .first_directory_ordinal = first_ordinal,
                .end_directory_ordinal = end_ordinal,
                .skip_budget_rows = declared_chunks - wanted_chunks,
                .position_extent = projected_range->position_extent,
            });
        }
        auto& section_stats = position_section ? stats_.positions : stats_.controls;
        checked_add_(section_stats.plan_calls, 1, "payload section plan-call counter overflow");
        checked_add_(section_stats.planned_request_groups,
                     planned_groups_.size(),
                     "payload section planned-group counter overflow");
        checked_add_(section_stats.control_authorization_checks,
                     authorization_checks,
                     "position control-authorization counter overflow");
        checked_add_(stats_.bulk_read_calls, 1, "payload plan-call counter overflow");
        checked_add_(stats_.planned_request_groups,
                     planned_groups_.size(),
                     "payload planned-group counter overflow");
        if (!position_section) {
            if (planned_groups_.empty()) {
                stats_.cold.route_empty = 1;
            } else {
                stats_.cold.route_selective = 1;
            }
        }
        stats_.cold.add(stats_.cold.route_candidate_rows, requested_rows);
        stats_.cold.add(stats_.cold.route_candidate_bytes, requested_bytes);
        stats_.cold.add(stats_.cold.payload_rows_requested, requested_rows);
        stats_.cold.add(stats_.cold.payload_bytes_requested, requested_bytes);
        stats_.cold.add(stats_.cold.route_selective_cost, requested_rows);
        const std::uint64_t group_penalty =
            planned_groups_.size() > cold_index_preparation_stats::k_sql_bigint_max /
                                         k_ordered_range_scan_penalty_rows
                ? cold_index_preparation_stats::k_sql_bigint_max
                : static_cast<std::uint64_t>(planned_groups_.size()) *
                      k_ordered_range_scan_penalty_rows;
        stats_.cold.add(stats_.cold.route_selective_cost, group_penalty);
        next_group_ = 0;
        if (position_section) {
            release_control_authorization_();
            state_ = planned_groups_.empty() ? operation_state::payload_complete
                                             : operation_state::positions_planned;
        } else {
            state_ = planned_groups_.empty() ? operation_state::controls_complete
                                             : operation_state::controls_planned;
        }
    } catch (...) {
        state_ = operation_state::failed;
        throw;
    }
}

inline void index_storage_fragment_generation_read_session::plan_control_groups(
    std::span<const minimizer_generation_payload_read_group> groups) {
    plan_section_groups_(payload_section::controls, groups);
}

inline void index_storage_fragment_generation_read_session::plan_position_groups(
    std::span<const minimizer_generation_payload_read_group> groups) {
    plan_section_groups_(payload_section::positions, groups);
}

} // namespace bioseq::storage::indexing
