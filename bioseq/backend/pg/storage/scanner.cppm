/**
 * @file scanner.cppm
 * @brief Stable facade for PostgreSQL sequence scanner partitions.
 *
 * Re-exports fragmented scanning, lazy binding, source registry and
 * locator-backed source scanning contracts.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "access/xact.h"
#include "access/relation.h"
#include "access/table.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

export module bioseq.postgres.storage.scanner;

import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.postgres.configuration.core;
import bioseq.postgres.support;
import bioseq.common.seqan_adapter;              // seq::dna4 in static_assert below
import bioseq.postgres.storage.manager;     // seq_caches + open_sub_table_for_locator
import bioseq.storage.scanner;              // scanner concepts
import bioseq.storage.lazy_sequence;        // lazy_sequence + with_postgres_lazy_sequence
import bioseq.storage.coordinates;
import bioseq.storage.layout;
import bioseq.postgres.storage.physical;
import bioseq.storage.size;        // sequence_size_t
import bioseq.postgres.storage.metadata;    // stripe_metadata/find_stripe_by_row_number
import bioseq.sequences;           // compact_sequence_range, compact_sequence_stdc_instance
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.table.io;            // begin_column_read/read_column_row_by_number
import bioseq.parallelism.pool;

export import :fragment_scanner;
export import :lazy_binding;
export import :source_registry;
export import :source_scanner;

static_assert(__cplusplus >= 202302L, "bioseq requires C++23-or-later");
