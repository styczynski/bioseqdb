/**
 * @file index_parts/output.cppm
 * @brief Defines validated replay of the next buffered scheduler result.
 */
module;

#include <bioseq/pg_compat.h>
extern "C" {
#include "executor/instrument.h"
#include "utils/snapmgr.h"
}

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// The chunked-scanner dispatch path is always compiled as a permanent part of
// the fragmented pipeline tree.
// The `bioseq.testing.chunked_scanner.enabled` GUC routes row sequences through a
// synthetic chunked scanner end-to-end. The
// `bioseq.testing.in_memory_scanner.enabled` GUC forces the materialise-then-
// in_memory baseline. Without either GUC the production path promotes locator
// values through the statement-owned source provider and runs from its
// canonical lazy facade.
module bioseq.postgres.scheduler.dispatch_buffer:output;

import bioseq.postgres.scheduler.dispatch_buffer;

import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.postgres.support;
import bioseq.postgres.support.datums;
import bioseq.postgres.configuration.core;
import bioseq.indexing.type;
import bioseq.postgres.storage.indexing.handles;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.storage.lazy_sequence; // allocation observer for route-aware sequence payload tracing
import bioseq.postgres.scheduler.dispatch_processor; // fixed-shape batch_processor, heavy pipeline in its own module
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.pipeline.minimap_port;
import bioseq.sequences.cigar;
import bioseq.pipeline.pipe;
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.storage.manager;
import bioseq.storage.scanner;
import bioseq.storage.layout;
import bioseq.postgres.storage.scanner; // statement-owned source provider for locator targets
import bioseq.parallelism.memory_admission;
import bioseq.postgres.scheduler.supervisor;
import bioseq.postgres.column.table_access;

namespace bioseq::scheduler {

/** @brief Implements the read next output contract declared by the stable buffer facade. */
[[nodiscard]] std::optional<tuple_dispatch_buffer::next_output>
tuple_dispatch_buffer::read_next_output() {
    if (join_mode) {
        const cursor_boundary boundary =
            classify_output_cursor(join_output_position, join_output_cursors.size());
        if (boundary == cursor_boundary::exact_eof) {
            return std::nullopt;
        }
        if (boundary == cursor_boundary::corrupt) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq batch join output cursor position is corrupt"),
                     errdetail("position=%zu size=%zu",
                               join_output_position,
                               join_output_cursors.size())));
        }
        const join_output_cursor cursor = join_output_cursors[join_output_position++];
        if (buffered_entries.size() != 1 || cursor.query_index >= join_queries.size()) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq batch join output cursor references missing input")));
        }
        const std::uint64_t ordinal = join_queries[cursor.query_index].ordinal;
        auto found = join_match_map.find(ordinal);
        if (found == join_match_map.end() || cursor.match_index >= found->second.size()) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq batch join output cursor references a missing match")));
        }
        return next_output{
            &buffered_entries.front(),
            ordinal,
            &found->second[cursor.match_index],
        };
    }
    const cursor_boundary boundary =
        classify_output_cursor(static_cast<std::int64_t>(output_position), output_cursors.size());
    if (boundary == cursor_boundary::exact_eof) {
        return std::nullopt;
    }
    if (boundary == cursor_boundary::corrupt) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq output cursor position is corrupt"),
                 errdetail("position=%d size=%zu", output_position, output_cursors.size())));
    }
    const output_cursor& cursor = output_cursors[output_position];
    output_position++;
    if (cursor.scan_entry_index >= buffered_entries.size()) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq output cursor references a missing scan entry"),
                 errdetail(
                     "entry=%zu buffered=%zu", cursor.scan_entry_index, buffered_entries.size())));
    }
    buffered_scan_entry& entry = buffered_entries[cursor.scan_entry_index];
    auto hit = match_map.find(static_cast<int32_t>(cursor.scan_entry_index));
    if (hit == match_map.end() || cursor.match_index >= hit->second.size()) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq output cursor references a missing alignment match"),
                 errdetail("entry=%zu match=%zu available=%zu",
                           cursor.scan_entry_index,
                           cursor.match_index,
                           hit == match_map.end() ? std::size_t{0} : hit->second.size())));
    }
    return next_output{&entry, 0, &hit->second[cursor.match_index]};
}

} // namespace bioseq::scheduler
