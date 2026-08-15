/**
 * @file codec_fixtures.cppm
 * @brief Defines the complete generated-codec fixture matrix and 14-codec receipt.
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

export module bioseq.postgres.support.row_codec_runtime_test_support:codec_fixtures;

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
import :contract_core;
import :catalog_lifecycle;
import :toast_faults;

namespace bioseq::postgres::support::row_codec_runtime_test_support {

[[nodiscard]] table_rows::bioseq_aligner_options full_aligner_options() {
    return {
        .kmer_size = 1,
        .window_size = 2,
        .max_gap = 3,
        .max_gap_ref = 4,
        .bandwidth = 5,
        .bandwidth_long = 6,
        .max_chain_skip = 7,
        .max_chain_iter = 8,
        .min_seed_count = 9,
        .min_chain_score = 10,
        .chain_gap_scale = 11.0,
        .chain_skip_scale = 12.0,
        .mask_level = 13.0,
        .mask_len = 14,
        .pri_ratio = 15.0,
        .best_n = 16,
        .match_score = 17,
        .mismatch_pen = 18,
        .gap_open1 = 19,
        .gap_ext1 = 20,
        .gap_open2 = 21,
        .gap_ext2 = 22,
        .sc_ambi = 23,
        .zdrop = 24,
        .end_bonus = 25,
        .max_occ = 26,
        .mid_occ = 27,
        .block_min_size = 28,
        .block_max_size = 29,
        .chaining_algorithm = 30,
        .base_alignment_algorithm = 31,
        .minimizer_algorithm = 32,
        .vulkan_enabled = true,
        .vulkan_seed_hits_enabled = false,
        .vulkan_index_cache_enabled = true,
        .index_read_requirement = 36,
    };
}

[[nodiscard]] runtime_receipt_entry exercise_index_table_shard() {
    using row = indexing_rows::index_table_shard;
    using codec = indexing_codecs::index_table_shard_codec;
    static_assert(codec::field_count == 9);
    return with_relation_descriptor<indexing_codecs::index_table_shard_codec>(
        "bioseq_internal", "index_table_shard", [](TupleDesc descriptor, Relation relation) {
            const auto make_row = [](MemoryContext context) {
                return row{
                    .database_oid = probe_oid(71),
                    .tablespace_oid = probe_oid(72),
                    .main_relfilenode = probe_oid(73),
                    .column_attno = 10,
                    .index_name = make_text("shard-index", context),
                    .shard_idx = 11,
                    .index_relid = probe_oid(74),
                    .bytes_written = 12,
                    .is_full = false,
                };
            };
            const auto make_live_row = [](MemoryContext context) {
                return row{
                    .database_oid = probe_oid(71),
                    .tablespace_oid = probe_oid(72),
                    .main_relfilenode = probe_oid(73),
                    .column_attno = 10,
                    .index_name = make_external_text(context, 0x388071U),
                    .shard_idx = 11,
                    .index_relid = probe_oid(74),
                    .bytes_written = 12,
                    .is_full = false,
                };
            };
            const auto make_updated_row = [make_row](MemoryContext context) {
                row value = make_row(context);
                value.bytes_written = 24;
                value.is_full = true;
                return value;
            };
            const auto make_live_updated_row = [make_live_row](MemoryContext context) {
                row value = make_live_row(context);
                value.bytes_written = 24;
                value.is_full = true;
                return value;
            };
            return exercise_codec<codec>(
                descriptor,
                relation,
                "sql:table",
                "storage_indexing_pg",
                make_row,
                make_row,
                make_live_row,
                make_updated_row,
                make_live_updated_row,
                [](codec::update& update, MemoryContext) {
                    update.set<&row::bytes_written>(static_cast<std::int64_t>(24))
                        .set<&row::is_full>(true);
                },
                [] -> std::optional<ScanKeyData> {
                    return verify_equal_scan_key<codec, &row::database_oid>(
                        probe_oid(71), ObjectIdGetDatum(probe_oid(71)));
                });
        });
}

[[nodiscard]] std::vector<runtime_receipt_entry> exercise_all_codecs() {
    advance_probe_invocation();
    std::vector<runtime_receipt_entry> receipts;
    receipts.reserve(14);

    receipts.push_back(with_relation_descriptor<metadata_codecs::row_mask_codec>(
        "bioseq_internal", "row_mask", [](TupleDesc descriptor, Relation relation) {
            using row = metadata_rows::row_mask;
            using codec = metadata_codecs::row_mask_codec;
            static_assert(codec::field_count == 8);
            const auto make_null_row = [](MemoryContext) {
                return row{
                    .id = probe_int64(1),
                    .storage_id = probe_int64(2),
                    .stripe_id = probe_int64(3),
                    .chunk_id = 4,
                    .start_row_number = probe_int64(5),
                    .end_row_number = probe_int64(6),
                    .deleted_rows = 7,
                    .mask = std::nullopt,
                };
            };
            const auto make_full_row = [](MemoryContext context) {
                return row{
                    .id = probe_int64(1),
                    .storage_id = probe_int64(2),
                    .stripe_id = probe_int64(3),
                    .chunk_id = 4,
                    .start_row_number = probe_int64(5),
                    .end_row_number = probe_int64(6),
                    .deleted_rows = 7,
                    .mask = make_bytea("row-mask", context),
                };
            };
            const auto make_live_row = [](MemoryContext context) {
                return row{
                    .id = probe_int64(1),
                    .storage_id = probe_int64(2),
                    .stripe_id = probe_int64(3),
                    .chunk_id = 4,
                    .start_row_number = probe_int64(5),
                    .end_row_number = probe_int64(6),
                    .deleted_rows = 7,
                    .mask = make_external_bytea(context, 0x388001U),
                };
            };
            const auto make_updated_row = [make_full_row](MemoryContext context) {
                row value = make_full_row(context);
                value.deleted_rows = 17;
                value.mask = std::nullopt;
                return value;
            };
            const auto make_live_updated_row = [make_live_row](MemoryContext context) {
                row value = make_live_row(context);
                value.deleted_rows = 17;
                value.mask = std::nullopt;
                return value;
            };
            return exercise_codec<codec>(
                descriptor,
                relation,
                "sql:table",
                "storage_metadata",
                make_null_row,
                make_full_row,
                make_live_row,
                make_updated_row,
                make_live_updated_row,
                [](codec::update& update, MemoryContext) {
                    update.set<&row::deleted_rows>(static_cast<std::int32_t>(17))
                        .set_null<&row::mask>();
                },
                [] -> std::optional<ScanKeyData> {
                    return verify_equal_scan_key<codec, &row::storage_id>(
                        probe_int64(2), Int64GetDatum(probe_int64(2)));
                });
        }));

    receipts.push_back(with_relation_descriptor<metadata_codecs::options_codec>(
        "bioseq_internal", "options", [](TupleDesc descriptor, Relation relation) {
            using row = metadata_rows::options;
            using codec = metadata_codecs::options_codec;
            static_assert(codec::field_count == 5);
            const auto make_row = [](MemoryContext) {
                return row{
                    .regclass = probe_oid(11),
                    .chunk_group_row_limit = 11,
                    .stripe_row_limit = 12,
                    .compression_level = 13,
                    .compression = make_name("zstd"),
                };
            };
            const auto make_updated_row = [make_row](MemoryContext context) {
                row value = make_row(context);
                value.compression_level = 23;
                return value;
            };
            return exercise_codec<codec>(
                descriptor,
                relation,
                "sql:table",
                "storage_metadata",
                make_row,
                make_row,
                make_row,
                make_updated_row,
                make_updated_row,
                [](codec::update& update, MemoryContext) {
                    update.set<&row::compression_level>(static_cast<std::int32_t>(23));
                },
                [] -> std::optional<ScanKeyData> {
                    return verify_equal_scan_key<codec, &row::regclass>(
                        probe_oid(11), ObjectIdGetDatum(probe_oid(11)));
                });
        }));

    receipts.push_back(with_relation_descriptor<metadata_codecs::stripe_codec>(
        "bioseq_internal", "stripe", [](TupleDesc descriptor, Relation relation) {
            using row = metadata_rows::stripe;
            using codec = metadata_codecs::stripe_codec;
            static_assert(codec::field_count == 9);
            const auto make_row = [](MemoryContext) {
                return row{
                    .storage_id = probe_int64(21),
                    .stripe_num = probe_int64(22),
                    .file_offset = 23,
                    .data_length = 24,
                    .column_count = 25,
                    .chunk_row_count = 26,
                    .row_count = 27,
                    .chunk_group_count = 28,
                    .first_row_number = 29,
                };
            };
            const auto make_updated_row = [make_row](MemoryContext context) {
                row value = make_row(context);
                value.row_count = 37;
                return value;
            };
            return exercise_codec<codec>(
                descriptor,
                relation,
                "sql:table",
                "storage_metadata",
                make_row,
                make_row,
                make_row,
                make_updated_row,
                make_updated_row,
                [](codec::update& update, MemoryContext) {
                    update.set<&row::row_count>(static_cast<std::int64_t>(37));
                },
                [] -> std::optional<ScanKeyData> {
                    return verify_equal_scan_key<codec, &row::storage_id>(
                        probe_int64(21), Int64GetDatum(probe_int64(21)));
                });
        }));

    receipts.push_back(with_relation_descriptor<metadata_codecs::chunk_group_codec>(
        "bioseq_internal", "chunk_group", [](TupleDesc descriptor, Relation relation) {
            using row = metadata_rows::chunk_group;
            using codec = metadata_codecs::chunk_group_codec;
            static_assert(codec::field_count == 5);
            const auto make_row = [](MemoryContext) {
                return row{
                    .storage_id = probe_int64(31),
                    .stripe_num = probe_int64(32),
                    .chunk_group_num = 33,
                    .row_count = 34,
                    .deleted_rows = 35,
                };
            };
            const auto make_updated_row = [make_row](MemoryContext context) {
                row value = make_row(context);
                value.deleted_rows = 45;
                return value;
            };
            return exercise_codec<codec>(
                descriptor,
                relation,
                "sql:table",
                "storage_metadata",
                make_row,
                make_row,
                make_row,
                make_updated_row,
                make_updated_row,
                [](codec::update& update, MemoryContext) {
                    update.set<&row::deleted_rows>(static_cast<std::int64_t>(45));
                },
                [] -> std::optional<ScanKeyData> {
                    return verify_equal_scan_key<codec, &row::storage_id>(
                        probe_int64(31), Int64GetDatum(probe_int64(31)));
                });
        }));

    receipts.push_back(with_relation_descriptor<metadata_codecs::chunk_codec>(
        "bioseq_internal", "chunk", [](TupleDesc descriptor, Relation relation) {
            using row = metadata_rows::chunk;
            using codec = metadata_codecs::chunk_codec;
            static_assert(codec::field_count == 14);
            const auto null_row = [](MemoryContext) {
                return row{
                    .storage_id = probe_int64(41),
                    .stripe_num = probe_int64(42),
                    .attr_num = 43,
                    .chunk_group_num = 44,
                    .minimum_value = std::nullopt,
                    .maximum_value = std::nullopt,
                    .value_stream_offset = 45,
                    .value_stream_length = 46,
                    .exists_stream_offset = 47,
                    .exists_stream_length = 48,
                    .value_compression_type = 49,
                    .value_compression_level = 50,
                    .value_decompressed_length = 51,
                    .value_count = 52,
                };
            };
            const auto full_row = [](MemoryContext context) {
                return row{
                    .storage_id = probe_int64(41),
                    .stripe_num = probe_int64(42),
                    .attr_num = 43,
                    .chunk_group_num = 44,
                    .minimum_value = make_bytea("chunk-min", context),
                    .maximum_value = make_bytea("chunk-max", context),
                    .value_stream_offset = 45,
                    .value_stream_length = 46,
                    .exists_stream_offset = 47,
                    .exists_stream_length = 48,
                    .value_compression_type = 49,
                    .value_compression_level = 50,
                    .value_decompressed_length = 51,
                    .value_count = 52,
                };
            };
            const auto live_row = [](MemoryContext context) {
                return row{
                    .storage_id = probe_int64(41),
                    .stripe_num = probe_int64(42),
                    .attr_num = 43,
                    .chunk_group_num = 44,
                    .minimum_value = make_external_bytea(context, 0x388041U),
                    .maximum_value = make_external_bytea(context, 0x388042U),
                    .value_stream_offset = 45,
                    .value_stream_length = 46,
                    .exists_stream_offset = 47,
                    .exists_stream_length = 48,
                    .value_compression_type = 49,
                    .value_compression_level = 50,
                    .value_decompressed_length = 51,
                    .value_count = 52,
                };
            };
            const auto updated_row = [full_row](MemoryContext context) {
                row value = full_row(context);
                value.minimum_value = std::nullopt;
                value.maximum_value = make_bytea("chunk-updated", context);
                return value;
            };
            const auto live_updated_row = [live_row](MemoryContext context) {
                row value = live_row(context);
                value.minimum_value = std::nullopt;
                value.maximum_value = make_bytea("chunk-updated", context);
                return value;
            };
            return exercise_codec<codec>(
                descriptor,
                relation,
                "sql:table",
                "storage_metadata",
                null_row,
                full_row,
                live_row,
                updated_row,
                live_updated_row,
                [](codec::update& update, MemoryContext context) {
                    update.set_null<&row::minimum_value>().set<&row::maximum_value>(
                        std::optional<bytea*>{make_bytea("chunk-updated", context)}, context);
                },
                [] -> std::optional<ScanKeyData> {
                    return verify_equal_scan_key<codec, &row::storage_id>(
                        probe_int64(41), Int64GetDatum(probe_int64(41)));
                });
        }));

    receipts.push_back(with_relation_descriptor<indexing_codecs::index_policy_codec>(
        "bioseq_internal", "index_policy", [](TupleDesc descriptor, Relation relation) {
            using row = indexing_rows::index_policy;
            using codec = indexing_codecs::index_policy_codec;
            static_assert(codec::field_count == 7);
            const auto make_row = [](MemoryContext context) {
                return row{
                    .user_relid = probe_oid(51),
                    .main_relfilenode = probe_oid(52),
                    .column_attno = 8,
                    .index_name = make_text("policy-index", context),
                    .enabled = true,
                    .update_on_insert = false,
                    .is_active_read = true,
                };
            };
            const auto make_live_row = [](MemoryContext context) {
                return row{
                    .user_relid = probe_oid(51),
                    .main_relfilenode = probe_oid(52),
                    .column_attno = 8,
                    .index_name = make_external_text(context, 0x388051U),
                    .enabled = true,
                    .update_on_insert = false,
                    .is_active_read = true,
                };
            };
            const auto make_updated_row = [make_row](MemoryContext context) {
                row value = make_row(context);
                value.index_name = make_text("policy-updated", context);
                value.enabled = false;
                return value;
            };
            const auto make_live_updated_row = [make_live_row](MemoryContext context) {
                row value = make_live_row(context);
                value.index_name = make_text("policy-updated", context);
                value.enabled = false;
                return value;
            };
            return exercise_codec<codec>(
                descriptor,
                relation,
                "sql:table",
                "storage_indexing_pg",
                make_row,
                make_row,
                make_live_row,
                make_updated_row,
                make_live_updated_row,
                [](codec::update& update, MemoryContext context) {
                    update.set<&row::index_name>(make_text("policy-updated", context), context)
                        .set<&row::enabled>(false);
                },
                [] -> std::optional<ScanKeyData> {
                    return verify_equal_scan_key<codec, &row::user_relid>(
                        probe_oid(51), ObjectIdGetDatum(probe_oid(51)));
                });
        }));

    receipts.push_back(with_relation_descriptor<indexing_codecs::metadata_age_log_codec>(
        "bioseq_internal", "metadata_age_log", [](TupleDesc descriptor, Relation relation) {
            using row = indexing_rows::metadata_age_log;
            using codec = indexing_codecs::metadata_age_log_codec;
            static_assert(codec::field_count == 5);
            const auto make_row = [](MemoryContext context) {
                return row{
                    .user_relid = probe_oid(61),
                    .main_relfilenode = probe_oid(62),
                    .column_attno = 9,
                    .index_name = make_text("age-index", context),
                    .last_updated = static_cast<TimestampTz>(388001),
                };
            };
            const auto make_live_row = [](MemoryContext context) {
                return row{
                    .user_relid = probe_oid(61),
                    .main_relfilenode = probe_oid(62),
                    .column_attno = 9,
                    .index_name = make_external_text(context, 0x388061U),
                    .last_updated = static_cast<TimestampTz>(388001),
                };
            };
            const auto make_updated_row = [make_row](MemoryContext context) {
                row value = make_row(context);
                value.last_updated = 388002;
                return value;
            };
            const auto make_live_updated_row = [make_live_row](MemoryContext context) {
                row value = make_live_row(context);
                value.last_updated = 388002;
                return value;
            };
            return exercise_codec<codec>(
                descriptor,
                relation,
                "sql:table",
                "storage_indexing_pg",
                make_row,
                make_row,
                make_live_row,
                make_updated_row,
                make_live_updated_row,
                [](codec::update& update, MemoryContext) {
                    update.set<&row::last_updated>(static_cast<TimestampTz>(388002));
                },
                [] -> std::optional<ScanKeyData> {
                    return verify_equal_scan_key<codec, &row::user_relid>(
                        probe_oid(61), ObjectIdGetDatum(probe_oid(61)));
                });
        }));

    receipts.push_back(exercise_index_table_shard());

    receipts.push_back(with_relation_descriptor<metadata_codecs::seq_sub_table_codec>(
        "bioseq_internal", "seq_sub_table", [](TupleDesc descriptor, Relation relation) {
            using row = metadata_rows::seq_sub_table;
            using codec = metadata_codecs::seq_sub_table_codec;
            static_assert(codec::field_count == 11);
            const auto make_row = [](MemoryContext) {
                return row{
                    .database_oid = probe_oid(81),
                    .tablespace_oid = probe_oid(82),
                    .main_relfilenode = probe_oid(83),
                    .column_attno = 12,
                    .bucket_idx = 13,
                    .shard_idx = 14,
                    .sub_table_relid = probe_oid(84),
                    .sidecar_relid = probe_oid(85),
                    .bytes_written = 15,
                    .is_full = false,
                    .total_payload_bytes = 16,
                };
            };
            const auto make_updated_row = [make_row](MemoryContext context) {
                row value = make_row(context);
                value.bytes_written = 30;
                value.is_full = true;
                return value;
            };
            return exercise_codec<codec>(
                descriptor,
                relation,
                "sql:table",
                "storage_metadata",
                make_row,
                make_row,
                make_row,
                make_updated_row,
                make_updated_row,
                [](codec::update& update, MemoryContext) {
                    update.set<&row::bytes_written>(static_cast<std::int64_t>(30))
                        .set<&row::is_full>(true);
                },
                [] -> std::optional<ScanKeyData> {
                    return verify_equal_scan_key<codec, &row::database_oid>(
                        probe_oid(81), ObjectIdGetDatum(probe_oid(81)));
                });
        }));

    receipts.push_back(with_relation_descriptor<manager_codecs::minimizer_generation_storage_codec>(
        "bioseq_internal",
        "minimizer_generation_storage",
        [](TupleDesc descriptor, Relation relation) {
            using row = manager_rows::minimizer_generation_storage;
            using codec = manager_codecs::minimizer_generation_storage_codec;
            static_assert(codec::field_count == 18);
            static_assert(nullable_field_count<codec>() == 1);
            const auto null_row = [](MemoryContext) {
                return row{
                    .database_oid = probe_oid(91),
                    .tablespace_oid = probe_oid(92),
                    .main_relfilenode = probe_oid(93),
                    .user_relid = probe_oid(94),
                    .column_attno = 15,
                    .index_name = make_name("minimizers_per_fragment"),
                    .query_key = 16,
                    .generation_id = 17,
                    .provider = 0,
                    .payload_relation_oid = probe_oid(95),
                    .payload_database_oid = probe_oid(91),
                    .payload_tablespace_oid = probe_oid(92),
                    .payload_relfilenode = probe_oid(96),
                    .payload_relation_name = make_name("payload_relation"),
                    .image_bytes = 4096,
                    .header_checksum = 18,
                    .retire_full_xid = std::nullopt,
                    .state = 1,
                };
            };
            const auto full_row = [](MemoryContext) {
                return row{
                    .database_oid = probe_oid(91),
                    .tablespace_oid = probe_oid(92),
                    .main_relfilenode = probe_oid(93),
                    .user_relid = probe_oid(94),
                    .column_attno = 15,
                    .index_name = make_name("minimizers_per_fragment"),
                    .query_key = 16,
                    .generation_id = 17,
                    .provider = 0,
                    .payload_relation_oid = probe_oid(95),
                    .payload_database_oid = probe_oid(91),
                    .payload_tablespace_oid = probe_oid(92),
                    .payload_relfilenode = probe_oid(96),
                    .payload_relation_name = make_name("payload_relation"),
                    .image_bytes = 4096,
                    .header_checksum = 18,
                    .retire_full_xid =
                        FullTransactionIdFromU64(static_cast<std::uint64_t>(probe_int64(97))),
                    .state = 2,
                };
            };
            const auto updated_row = [full_row](MemoryContext context) {
                row value = full_row(context);
                value.image_bytes = 8192;
                return value;
            };
            return exercise_codec<codec>(
                descriptor,
                relation,
                "sql:table",
                "storage_manager",
                null_row,
                full_row,
                full_row,
                updated_row,
                updated_row,
                [](codec::update& update, MemoryContext) {
                    update.set<&row::image_bytes>(static_cast<std::int64_t>(8192));
                },
                [] -> std::optional<ScanKeyData> {
                    return verify_equal_scan_key<codec, &row::database_oid>(
                        probe_oid(91), ObjectIdGetDatum(probe_oid(91)));
                });
        }));

    receipts.push_back(
        with_relation_descriptor<manager_codecs::minimizer_generation_reaper_state_codec>(
            "bioseq_internal",
            "minimizer_generation_reaper_state",
            [](TupleDesc descriptor, Relation relation) {
                using row = manager_rows::minimizer_generation_reaper_state;
                using codec = manager_codecs::minimizer_generation_reaper_state_codec;
                static_assert(codec::field_count == 5);
                const auto make_row = [](MemoryContext) {
                    return row{
                        .database_oid = probe_oid(101),
                        .tablespace_root_ordinal = 19,
                        .shard_ordinal = 20,
                        .last_canonical_filename = "inline-reaper-name",
                        .pass = 21,
                    };
                };
                const auto make_live_row = [](MemoryContext) {
                    return row{
                        .database_oid = probe_oid(101),
                        .tablespace_root_ordinal = 19,
                        .shard_ordinal = 20,
                        .last_canonical_filename = make_external_string(0x388101U),
                        .pass = 21,
                    };
                };
                const auto make_updated_row = [make_row](MemoryContext context) {
                    row value = make_row(context);
                    value.last_canonical_filename = "inline-reaper-updated";
                    value.pass = 42;
                    return value;
                };
                const auto make_live_updated_row = [make_live_row](MemoryContext context) {
                    row value = make_live_row(context);
                    value.last_canonical_filename = "inline-reaper-updated";
                    value.pass = 42;
                    return value;
                };
                return exercise_codec<codec>(
                    descriptor,
                    relation,
                    "sql:table",
                    "storage_manager",
                    make_row,
                    make_row,
                    make_live_row,
                    make_updated_row,
                    make_live_updated_row,
                    [](codec::update& update, MemoryContext context) {
                        update
                            .set<&row::last_canonical_filename>(
                                std::string{"inline-reaper-updated"}, context)
                            .set<&row::pass>(static_cast<std::int64_t>(42));
                    },
                    [] -> std::optional<ScanKeyData> {
                        return verify_equal_scan_key<codec, &row::database_oid>(
                            probe_oid(101), ObjectIdGetDatum(probe_oid(101)));
                    });
            }));

    receipts.push_back(with_relation_descriptor<manager_codecs::sequence_source_version_codec>(
        "bioseq_internal", "sequence_source_version", [](TupleDesc descriptor, Relation relation) {
            using row = manager_rows::sequence_source_version;
            using codec = manager_codecs::sequence_source_version_codec;
            static_assert(codec::field_count == 20);
            const auto null_row = [](MemoryContext) {
                return row{
                    .source_epoch = probe_int64(111),
                    .user_relid = probe_oid(112),
                    .database_oid = probe_oid(113),
                    .tablespace_oid = probe_oid(114),
                    .main_relfilenode = probe_oid(115),
                    .main_storage_id = 52,
                    .column_attno = 16,
                    .locator_version = 3,
                    .bucket_idx = 17,
                    .shard_idx = 18,
                    .fragment_relid = std::nullopt,
                    .fragment_storage_id = 0,
                    .first_sub_row_no = 0,
                    .num_fragments = 0,
                    .total_bp_size = 53,
                    .alphabet_code = 4,
                    .storage_unit_byte_width = 1,
                    .padding_letters = 0,
                    .source_content_checksum = 54,
                    .state = 0,
                };
            };
            const auto full_row = [](MemoryContext) {
                return row{
                    .source_epoch = probe_int64(111),
                    .user_relid = probe_oid(112),
                    .database_oid = probe_oid(113),
                    .tablespace_oid = probe_oid(114),
                    .main_relfilenode = probe_oid(115),
                    .main_storage_id = 52,
                    .column_attno = 16,
                    .locator_version = 3,
                    .bucket_idx = 17,
                    .shard_idx = 18,
                    .fragment_relid = probe_oid(116),
                    .fragment_storage_id = 55,
                    .first_sub_row_no = 56,
                    .num_fragments = 57,
                    .total_bp_size = 58,
                    .alphabet_code = 4,
                    .storage_unit_byte_width = 1,
                    .padding_letters = 0,
                    .source_content_checksum = 59,
                    .state = 1,
                };
            };
            const auto updated_row = [full_row](MemoryContext context) {
                row value = full_row(context);
                value.state = 2;
                return value;
            };
            return exercise_codec<codec>(
                descriptor,
                relation,
                "sql:table",
                "storage_manager",
                null_row,
                full_row,
                full_row,
                updated_row,
                updated_row,
                [](codec::update& update, MemoryContext) {
                    update.set<&row::state>(static_cast<std::int16_t>(2));
                },
                [] -> std::optional<ScanKeyData> {
                    return verify_equal_scan_key<codec, &row::source_epoch>(
                        probe_int64(111), Int64GetDatum(probe_int64(111)));
                });
        }));

    receipts.push_back(with_composite_descriptor<table_codecs::bioseq_aligner_options_codec>(
        "$extension", "bioseq_aligner_options", [](TupleDesc descriptor, Relation relation) {
            using row = table_rows::bioseq_aligner_options;
            using codec = table_codecs::bioseq_aligner_options_codec;
            static_assert(codec::field_count == 36);
            const auto make_null_row = [](MemoryContext) { return row{}; };
            const auto make_full_row = [](MemoryContext) { return full_aligner_options(); };
            const auto make_updated_row = [make_full_row](MemoryContext context) {
                row value = make_full_row(context);
                value.kmer_size = std::nullopt;
                value.window_size = std::optional<std::int32_t>{88};
                return value;
            };
            return exercise_codec<codec>(
                descriptor,
                relation,
                "sql:type",
                "table",
                make_null_row,
                make_full_row,
                make_full_row,
                make_updated_row,
                make_updated_row,
                [](codec::update& update, MemoryContext) {
                    update.set_null<&row::kmer_size>().set<&row::window_size>(
                        std::optional<std::int32_t>{88});
                },
                [] -> std::optional<ScanKeyData> {
                    static_assert([] consteval {
                        for (const auto& field : codec::expected_descriptor) {
                            if (!field.nullable) {
                                return false;
                            }
                        }
                        return true;
                    }());
                    return std::nullopt;
                });
        }));

    receipts.push_back(with_composite_descriptor<load_codecs::column_mapping_codec>(
        "bioseq", "column_mapping", [](TupleDesc descriptor, Relation relation) {
            using row = load_rows::column_mapping;
            using codec = load_codecs::column_mapping_codec;
            static_assert(codec::field_count == 2);
            if (relation != nullptr) {
                contract_failure("installed column_mapping type unexpectedly "
                                 "has a live relation");
            }
            const auto make_null_row = [](MemoryContext) {
                return row{
                    .column_name = std::nullopt,
                    .data_source = std::nullopt,
                };
            };
            const auto make_full_row = [](MemoryContext context) {
                return row{
                    .column_name = make_text("column", context),
                    .data_source = make_text("source", context),
                };
            };
            const auto make_live_row = [](MemoryContext context) {
                return row{
                    .column_name = make_external_text(context, 0x388201U),
                    .data_source = make_external_text(context, 0x388202U),
                };
            };
            const auto make_updated_row = [make_full_row](MemoryContext context) {
                row value = make_full_row(context);
                value.column_name = std::nullopt;
                value.data_source = make_text("updated-source", context);
                return value;
            };
            const auto make_live_updated_row = [make_live_row](MemoryContext context) {
                row value = make_live_row(context);
                value.column_name = std::nullopt;
                value.data_source = make_text("updated-source", context);
                return value;
            };
            return with_column_mapping_toast_carrier<codec>(
                descriptor,
                [make_null_row,
                 make_full_row,
                 make_live_row,
                 make_updated_row,
                 make_live_updated_row,
                 descriptor](TupleDesc carrier_descriptor, Relation carrier_relation) {
                    return exercise_codec_with_external<codec>(
                        descriptor,
                        nullptr,
                        carrier_relation,
                        carrier_descriptor,
                        "sql:type",
                        "storage_load_pg",
                        make_null_row,
                        make_full_row,
                        make_live_row,
                        make_updated_row,
                        make_live_updated_row,
                        [](codec::update& update, MemoryContext context) {
                            update.set_null<&row::column_name>().set<&row::data_source>(
                                std::optional<text*>{make_text("updated-source", context)},
                                context);
                        },
                        [] -> std::optional<ScanKeyData> {
                            static_assert(codec::expected_descriptor[0].nullable);
                            static_assert(codec::expected_descriptor[1].nullable);
                            return std::nullopt;
                        });
                });
        }));

    if (receipts.size() != 14) {
        contract_failure("runtime receipt does not contain exactly 14 codecs");
    }
    exercise_reaper_external_toast();
    return receipts;
}

} // namespace bioseq::postgres::support::row_codec_runtime_test_support
