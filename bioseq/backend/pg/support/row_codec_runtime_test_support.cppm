/**
 * @file row_codec_runtime_test_support.cppm
 * @brief Test generated conversions between C++ rows and PostgreSQL tuples.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "access/detoast.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_type_d.h"
#include "commands/extension.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "miscadmin.h"
#include "varatt.h"
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fmt/format.h>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.postgres.support.row_codec_runtime_test_support;

import bioseq.postgres.sql_boundary;
import bioseq.postgres.storage.indexing.sql_row_codecs;
import bioseq.postgres.storage.indexing.sql_rows;
import bioseq.postgres.storage.io.sql_row_codecs;
import bioseq.postgres.storage.io.sql_rows;
import bioseq.postgres.storage.manager.sql_row_codecs;
import bioseq.postgres.storage.manager.sql_rows;
import bioseq.postgres.storage.metadata.sql_row_codecs;
import bioseq.postgres.storage.metadata.sql_rows;
import bioseq.postgres.table.sql_row_codecs;
import bioseq.postgres.table.sql_rows;

#if defined(BIOSEQ_TEST_INDEXES)

import :contract_core;
import :catalog_lifecycle;
import :toast_faults;
import :codec_fixtures;

export namespace bioseq::postgres::support::row_codec_runtime_test_support {

/**
 * Test all generated row codecs and compare their result report with the
 * manifest-based report supplied by the integration test. Roll back test changes.
 */
void exercise_all(std::string_view expected_receipt) {
    auto transaction = subtransaction::begin();
    try {
        const std::string actual = render_receipt(exercise_all_codecs());
        if (actual != expected_receipt) {
            contract_failure("manifest-joined receipt mismatch; expected=" +
                             std::string{expected_receipt} + " actual=" + actual);
        }
        transaction.rollback();
    } catch (...) {
        rethrow_after_rollback(transaction);
    }
}

/** Exercise only the generated index-table-shard codec and roll it back. */
void exercise_index_table_shard_only() {
    auto transaction = subtransaction::begin();
    try {
        advance_probe_invocation();
        static_cast<void>(exercise_index_table_shard());
        transaction.rollback();
    } catch (...) {
        rethrow_after_rollback(transaction);
    }
}

/**
 * Check that a detoasting error reaches C++ and runs its destructors.
 * The test text contains an external TOAST pointer to a nonexistent relation.
 */
void exercise_reaper_text_bridge_error() {
    auto transaction = subtransaction::begin();
    try {
        exercise_reaper_text_bridge_error_impl();
        transaction.rollback();
    } catch (...) {
        rethrow_after_rollback(transaction);
    }
}

} // namespace bioseq::postgres::support::row_codec_runtime_test_support

#endif
