/**
 * @file catalog_metadata.cppm
 * @brief Defines physical catalog naming, metadata-age tracking, locks, and generation identifiers.
 */
module;
#include <bioseq/pg_compat.h>
extern "C" {
#include "access/heapam.h"
#include "access/nbtree.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/namespace.h" // RelnameGetRelid
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_type_d.h"
#include "commands/sequence.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
}
export module bioseq.postgres.storage.indexing.chunked_storage:catalog_metadata;
import std;
import bioseq.common.logging;
import bioseq.postgres.support; // bioseq::postgres::support::try_open_postgres_relation, bioseq::postgres::support::find_primary_key_oid

import :catalog_contracts;
export namespace bioseq::storage::indexing {
/// Compile-time-ish abbreviation for an index name (keeps the physical name
/// inside NAMEDATALEN). Unknown names fall back to a stable short hash.
[[nodiscard]] inline std::string short_index_for(std::string_view name) {
    if (name == k_minimizers_per_fragment_index_name)
        return "mpf";
    if (name == k_minimizers_per_sequence_index_name)
        return "mps";
    if (name == k_symbol_counts_index_name)
        return "sc";
    if (name == "test_table_rowcount")
        return "ttr";
    // generic fallback: first 4 chars + length, still deterministic
    std::string out = "ix";
    for (char c : name) {
        if (out.size() >= 6)
            break;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            out.push_back(c);
    }
    return out;
}

/// Stable 64-bit FNV-1a over the logical identity tuple, hex-rendered.
[[nodiscard]] inline std::uint64_t
identity_hash64(Oid db, Oid spc, Oid relfn, std::int16_t attno, std::string_view name) {
    std::uint64_t hash_value = 1469598103934665603ull;
    const auto mix_word = [&hash_value](std::uint64_t word) {
        for (int byte_index = 0; byte_index < 8; ++byte_index) {
            hash_value ^= word & 0xffu;
            hash_value *= 1099511628211ull;
            word >>= 8u;
        }
    };
    mix_word(db);
    mix_word(spc);
    mix_word(relfn);
    mix_word(static_cast<std::uint64_t>(static_cast<std::uint16_t>(attno)));
    for (const char character : name) {
        hash_value ^= static_cast<std::uint8_t>(character);
        hash_value *= 1099511628211ull;
    }
    return hash_value;
}

[[noreturn]] inline void derived_index_name_too_long(std::string_view name) {
    const char* name_data = name.empty() ? "" : name.data();
    const int name_size = static_cast<int>(
        std::min(name.size(), static_cast<std::size_t>((std::numeric_limits<int>::max)())));
    const unsigned long long full_size = static_cast<unsigned long long>(name.size());
    bioseq::postgres::support::invoke_pg_as_cpp([name_data, name_size, full_size] {
        ereport(ERROR,
                (errcode(ERRCODE_NAME_TOO_LONG),
                 errmsg("bioseq index: derived name is too long"),
                 errdetail("bytes=%llu name=%.*s", full_size, name_size, name_data)));
    });
    pg_unreachable();
}

/// Format one canonical per-sequence index relation name.
[[nodiscard]] inline std::string format_seqidx_name(std::string_view name,
                                                    Oid db,
                                                    Oid spc,
                                                    Oid relfn,
                                                    std::int16_t attno,
                                                    std::uint8_t bucket,
                                                    std::uint32_t shard) {
    std::string out = fmt::format("seqidx_{}_{:016x}_{}_{}",
                                  short_index_for(name),
                                  identity_hash64(db, spc, relfn, attno, name),
                                  static_cast<unsigned>(bucket),
                                  static_cast<unsigned>(shard));
    if (out.size() >= NAMEDATALEN)
        derived_index_name_too_long(out);
    return out;
}
/// Format one canonical table-wide index relation name.
[[nodiscard]] inline std::string format_seqidxtab_name(
    std::string_view name, Oid db, Oid spc, Oid relfn, std::int16_t attno, std::int32_t shard) {
    std::string out = fmt::format("seqidxtab_{}_{:016x}_{}",
                                  short_index_for(name),
                                  identity_hash64(db, spc, relfn, attno, name),
                                  shard);
    if (out.size() >= NAMEDATALEN)
        derived_index_name_too_long(out);
    return out;
}

/// Resolve the internal schema from within an existing PostgreSQL boundary.
[[nodiscard]] inline Oid bioseq_internal_ns_pg_leaf() {
    return get_namespace_oid("bioseq_internal", false);
}

/// Resolve the internal schema through the C++ PostgreSQL boundary.
[[nodiscard]] inline Oid bioseq_internal_ns() {
    return bioseq::postgres::support::invoke_pg_as_cpp([] { return bioseq_internal_ns_pg_leaf(); });
}

/// Runs one DDL statement inside a rollback-safe SQL subtransaction.
inline void run_internal_ddl(const char* ddl) {
    auto subtransaction = bioseq::postgres::support::subtransaction::begin();
    bioseq::postgres::support::spi_session spi;
    const int connect_status = spi.connect();
    if (connect_status != SPI_OK_CONNECT) {
        minimizer_generation_storage_error("SPI_connect failed while running index DDL");
    }
    const int execute_status = bioseq::postgres::support::invoke_pg_as_cpp(
        [ddl] { return SPI_execute(ddl, /*read_only=*/false, /*tcount=*/0); });
    if (execute_status != SPI_OK_UTILITY && execute_status != SPI_OK_SELECT) {
        minimizer_generation_storage_error("index DDL returned an unexpected SPI status");
    }
    if (spi.finish() != SPI_OK_FINISH) {
        minimizer_generation_storage_error("SPI_finish failed while running index DDL");
    }
    subtransaction.commit();
}

/// Persistence keyword inherited from the user relation (UNLOGGED if parent is).
[[nodiscard]] inline const char* persistence_kw_for(Oid user_relid) {
    char persist = 'p';
    Relation r = bioseq::postgres::support::try_open_postgres_relation(user_relid, AccessShareLock);
    if (r != nullptr) {
        persist = r->rd_rel->relpersistence;
        relation_close(r, AccessShareLock);
    }
    return persist == 'u' ? "UNLOGGED" : "";
}

/// Raise a generation-storage error from within a PostgreSQL leaf.
[[noreturn]] inline void minimizer_generation_storage_error_pg_leaf(std::string_view message) {
    ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq current-generation index: %.*s",
                    static_cast<int>(message.size()),
                    message.data())));
    pg_unreachable();
}

/// Translate a generation-storage PostgreSQL error for C++ unwinding.
[[noreturn]] void minimizer_generation_storage_error_as_cpp(std::string_view message) {
    bioseq::postgres::support::invoke_pg_as_cpp(
        [message] { minimizer_generation_storage_error_pg_leaf(message); });
    pg_unreachable();
}

/// Re-enter PostgreSQL only after all owning C++ values have unwound.
[[noreturn]] void minimizer_generation_storage_error(std::string_view message) {
    minimizer_generation_storage_error_as_cpp(message);
    pg_unreachable();
}

/// Validate the exact metadata-age-log primary-key descriptor.
inline void validate_metadata_age_log_primary_key(Relation relation) {
    const Oid primary_oid = bioseq::postgres::support::invoke_pg_as_cpp(
        [relation] { return bioseq::postgres::support::find_primary_key_oid(relation); });
    if (!OidIsValid(primary_oid)) {
        minimizer_generation_storage_error("metadata_age_log primary key is missing");
    }
    Relation primary = bioseq::postgres::support::invoke_pg_as_cpp(
        [primary_oid] { return index_open(primary_oid, AccessShareLock); });
    const Form_pg_index form = primary->rd_index;
    constexpr std::array<AttrNumber, 4> k_expected_columns{1, 2, 3, 4};
    bool valid = form != nullptr && form->indisvalid && form->indisready && form->indislive &&
                 form->indisunique && form->indisprimary &&
                 form->indnkeyatts == static_cast<std::int16_t>(k_expected_columns.size());
    for (std::size_t index = 0; valid && index < k_expected_columns.size(); ++index) {
        valid = form->indkey.values[index] == k_expected_columns[index];
    }
    bioseq::postgres::support::invoke_pg_as_cpp(
        [primary] { index_close(primary, AccessShareLock); });
    if (!valid) {
        minimizer_generation_storage_error("metadata_age_log primary key shape changed");
    }
}

/**
 * Insert or refresh one metadata-age row using the generated row codec as the
 * sole C++/Datum boundary. SPI owns only the fixed SQL statement. No value is
 * string-formatted into SQL.
 */
[[nodiscard]] inline std::uint64_t upsert_metadata_age_log_pg(Oid user_relid,
                                                              Oid main_relfilenode,
                                                              std::int16_t column_attno,
                                                              std::string_view index_name) {
    const Oid namespace_oid = bioseq_internal_ns();
    const Oid relation_oid = bioseq::postgres::support::invoke_pg_as_cpp(
        [namespace_oid] { return get_relname_relid("metadata_age_log", namespace_oid); });
    if (!OidIsValid(relation_oid)) {
        minimizer_generation_storage_error("metadata_age_log catalog relation is missing");
    }

    Relation relation = bioseq::postgres::support::invoke_pg_as_cpp(
        [relation_oid] { return table_open(relation_oid, RowExclusiveLock); });
    MemoryContext operation_context = nullptr;
    try {
        operation_context = bioseq::postgres::support::invoke_pg_as_cpp([] {
            return AllocSetContextCreate(
                CurrentMemoryContext, "metadata_age_log upsert operation", ALLOCSET_SMALL_SIZES);
        });
        if (RelationGetNamespace(relation) != namespace_oid ||
            relation->rd_rel->relkind != RELKIND_RELATION ||
            std::string_view{RelationGetRelationName(relation)} != "metadata_age_log") {
            minimizer_generation_storage_error("metadata_age_log catalog identity changed");
        }
        metadata_age_log_codec::validate_descriptor(RelationGetDescr(relation));
        validate_metadata_age_log_primary_key(relation);

        text* const name_value =
            bioseq::postgres::support::invoke_pg_as_cpp([index_name, operation_context] {
                MemoryContext previous = MemoryContextSwitchTo(operation_context);
                text* const result = cstring_to_text_with_len(index_name.data(),
                                                              static_cast<int>(index_name.size()));
                MemoryContextSwitchTo(previous);
                return result;
            });
        auto encoded = metadata_age_log_codec::encode(
            metadata_age_log_catalog_row{
                .user_relid = user_relid,
                .main_relfilenode = main_relfilenode,
                .column_attno = column_attno,
                .index_name = name_value,
                .last_updated = GetCurrentTimestamp(),
            },
            operation_context);
        const auto tuple_data = encoded.tuple_data(RelationGetDescr(relation));
        std::array<Oid, metadata_age_log_codec::field_count> argument_types{
            OIDOID,
            OIDOID,
            INT2OID,
            TEXTOID,
            TIMESTAMPTZOID,
        };
        std::array<Datum, metadata_age_log_codec::field_count> argument_values{};
        std::copy_n(tuple_data.values, argument_values.size(), argument_values.begin());

        bioseq::postgres::support::spi_session spi;
        const int connect_status = spi.connect();
        if (connect_status != SPI_OK_CONNECT) {
            minimizer_generation_storage_error("metadata_age_log SPI_connect failed");
        }
        static constexpr char k_statement[] =
            "INSERT INTO bioseq_internal.metadata_age_log "
            "(user_relid, main_relfilenode, column_attno, "
            " index_name, last_updated) "
            "VALUES ($1, $2, $3, $4, $5) "
            "ON CONFLICT "
            "(user_relid, main_relfilenode, column_attno, index_name) "
            "DO UPDATE SET last_updated = EXCLUDED.last_updated";
        const int execute_status =
            bioseq::postgres::support::invoke_pg_as_cpp([&argument_types, &argument_values] {
                return SPI_execute_with_args(k_statement,
                                             static_cast<int>(argument_types.size()),
                                             argument_types.data(),
                                             argument_values.data(),
                                             /*Nulls=*/nullptr,
                                             /*read_only=*/false,
                                             /*tcount=*/0);
            });
        const std::uint64_t affected_rows = SPI_processed;
        if (execute_status != SPI_OK_INSERT || affected_rows != 1) {
            minimizer_generation_storage_error(fmt::format("metadata_age_log upsert failed "
                                                           "(SPI status={}, affected rows={})",
                                                           execute_status,
                                                           affected_rows));
        }
        if (spi.finish() != SPI_OK_FINISH) {
            minimizer_generation_storage_error("metadata_age_log SPI_finish failed");
        }
        MemoryContext finished_context = std::exchange(operation_context, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_context] { MemoryContextDelete(finished_context); });
        Relation relation_to_close = std::exchange(relation, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [relation_to_close] { table_close(relation_to_close, RowExclusiveLock); });
        return affected_rows;
    } catch (...) {
        const std::exception_ptr original = std::current_exception();
        if (operation_context != nullptr) {
            MemoryContext doomed_context = std::exchange(operation_context, nullptr);
            try {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [doomed_context] { MemoryContextDelete(doomed_context); });
            } catch (...) {
                std::terminate();
            }
        }
        if (relation != nullptr) {
            try {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [relation] { table_close(relation, RowExclusiveLock); });
            } catch (...) {
                std::terminate();
            }
        }
        std::rethrow_exception(original);
    }
}

/**
 * Delete one metadata-age row through generated codec-encoded key Datums.
 */
[[nodiscard]] inline std::uint64_t delete_metadata_age_log_pg(Oid user_relid,
                                                              Oid main_relfilenode,
                                                              std::int16_t column_attno,
                                                              std::string_view index_name) {
    const Oid namespace_oid = bioseq_internal_ns();
    const Oid relation_oid = bioseq::postgres::support::invoke_pg_as_cpp(
        [namespace_oid] { return get_relname_relid("metadata_age_log", namespace_oid); });
    if (!OidIsValid(relation_oid)) {
        minimizer_generation_storage_error("metadata_age_log catalog relation is missing");
    }

    Relation relation = bioseq::postgres::support::invoke_pg_as_cpp(
        [relation_oid] { return table_open(relation_oid, RowExclusiveLock); });
    MemoryContext operation_context = nullptr;
    try {
        operation_context = bioseq::postgres::support::invoke_pg_as_cpp([] {
            return AllocSetContextCreate(
                CurrentMemoryContext, "metadata_age_log delete operation", ALLOCSET_SMALL_SIZES);
        });
        if (RelationGetNamespace(relation) != namespace_oid ||
            relation->rd_rel->relkind != RELKIND_RELATION ||
            std::string_view{RelationGetRelationName(relation)} != "metadata_age_log") {
            minimizer_generation_storage_error("metadata_age_log catalog identity changed");
        }
        metadata_age_log_codec::validate_descriptor(RelationGetDescr(relation));
        validate_metadata_age_log_primary_key(relation);

        text* const name_value =
            bioseq::postgres::support::invoke_pg_as_cpp([index_name, operation_context] {
                MemoryContext previous = MemoryContextSwitchTo(operation_context);
                text* const result = cstring_to_text_with_len(index_name.data(),
                                                              static_cast<int>(index_name.size()));
                MemoryContextSwitchTo(previous);
                return result;
            });
        auto encoded = metadata_age_log_codec::encode(
            metadata_age_log_catalog_row{
                .user_relid = user_relid,
                .main_relfilenode = main_relfilenode,
                .column_attno = column_attno,
                .index_name = name_value,
                .last_updated = GetCurrentTimestamp(),
            },
            operation_context);
        const auto tuple_data = encoded.tuple_data(RelationGetDescr(relation));
        std::array<Oid, 4> argument_types{
            OIDOID,
            OIDOID,
            INT2OID,
            TEXTOID,
        };
        std::array<Datum, 4> argument_values{};
        std::copy_n(tuple_data.values, argument_values.size(), argument_values.begin());

        bioseq::postgres::support::spi_session spi;
        const int connect_status = spi.connect();
        if (connect_status != SPI_OK_CONNECT) {
            minimizer_generation_storage_error("metadata_age_log SPI_connect failed");
        }
        static constexpr char k_statement[] = "DELETE FROM bioseq_internal.metadata_age_log "
                                              "WHERE user_relid = $1 "
                                              "  AND main_relfilenode = $2 "
                                              "  AND column_attno = $3 "
                                              "  AND index_name = $4";
        const int execute_status =
            bioseq::postgres::support::invoke_pg_as_cpp([&argument_types, &argument_values] {
                return SPI_execute_with_args(k_statement,
                                             static_cast<int>(argument_types.size()),
                                             argument_types.data(),
                                             argument_values.data(),
                                             /*Nulls=*/nullptr,
                                             /*read_only=*/false,
                                             /*tcount=*/0);
            });
        const std::uint64_t affected_rows = SPI_processed;
        if (execute_status != SPI_OK_DELETE || affected_rows > 1) {
            minimizer_generation_storage_error(fmt::format("metadata_age_log delete failed "
                                                           "(SPI status={}, affected rows={})",
                                                           execute_status,
                                                           affected_rows));
        }
        if (spi.finish() != SPI_OK_FINISH) {
            minimizer_generation_storage_error("metadata_age_log SPI_finish failed");
        }
        MemoryContext finished_context = std::exchange(operation_context, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_context] { MemoryContextDelete(finished_context); });
        Relation relation_to_close = std::exchange(relation, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [relation_to_close] { table_close(relation_to_close, RowExclusiveLock); });
        return affected_rows;
    } catch (...) {
        const std::exception_ptr original = std::current_exception();
        if (operation_context != nullptr) {
            MemoryContext doomed_context = std::exchange(operation_context, nullptr);
            try {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [doomed_context] { MemoryContextDelete(doomed_context); });
            } catch (...) {
                std::terminate();
            }
        }
        if (relation != nullptr) {
            try {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [relation] { table_close(relation, RowExclusiveLock); });
            } catch (...) {
                std::terminate();
            }
        }
        std::rethrow_exception(original);
    }
}

/// Stable administration-lock key shared by reset and publication. The hash
/// remains byte-for-byte identical across both operations.
[[nodiscard]] inline std::uint64_t
index_advisory_xact_lock_key(Oid db,
                             Oid tablespace,
                             Oid relfilenode,
                             std::int16_t attno,
                             std::string_view index_name) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    hash ^= 0xB10D2790u;
    const auto mix_word = [&hash](std::uint64_t value) noexcept {
        for (std::uint32_t byte = 0; byte < 8; ++byte) {
            hash ^= value & 0xffu;
            hash *= 1099511628211ULL;
            value >>= 8u;
        }
    };
    mix_word(db);
    mix_word(tablespace);
    mix_word(relfilenode);
    mix_word(static_cast<std::uint16_t>(attno));
    for (char byte : index_name) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

/// Acquire the one existing transaction-scoped logical-index administration
/// lock. Read sessions deliberately never call this helper.
inline void acquire_index_advisory_xact_lock(
    Oid db, Oid tablespace, Oid relfilenode, std::int16_t attno, std::string_view index_name) {
    const std::int64_t signed_key = std::bit_cast<std::int64_t>(
        index_advisory_xact_lock_key(db, tablespace, relfilenode, attno, index_name));
    bioseq::postgres::support::invoke_pg_as_cpp([signed_key] {
        DirectFunctionCall1(pg_advisory_xact_lock_int8, Int64GetDatum(signed_key));
    });
}

/// Sequence-generator relation mismatch observed before ID allocation.
enum class minimizer_generation_sequence_error : std::uint8_t {
    none,
    missing,
    wrong_namespace,
    wrong_relkind,
    wrong_persistence,
    missing_catalog_row,
    wrong_type,
    wrong_bounds,
    cycling,
};

[[nodiscard]] constexpr std::string_view
minimizer_generation_sequence_error_name(minimizer_generation_sequence_error error) noexcept {
    switch (error) {
    case minimizer_generation_sequence_error::none:
        return "none";
    case minimizer_generation_sequence_error::missing:
        return "missing";
    case minimizer_generation_sequence_error::wrong_namespace:
        return "wrong namespace";
    case minimizer_generation_sequence_error::wrong_relkind:
        return "not a sequence";
    case minimizer_generation_sequence_error::wrong_persistence:
        return "not permanent";
    case minimizer_generation_sequence_error::missing_catalog_row:
        return "missing pg_sequence row";
    case minimizer_generation_sequence_error::wrong_type:
        return "not BIGINT";
    case minimizer_generation_sequence_error::wrong_bounds:
        return "wrong positive non-wrapping bounds";
    case minimizer_generation_sequence_error::cycling:
        return "cycles";
    }
    return "unknown";
}

/// Inspection result for the generation identifier sequence.
struct minimizer_generation_sequence_inspection {
    minimizer_generation_sequence_error error = minimizer_generation_sequence_error::none;
    Oid sequence_oid = InvalidOid;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == minimizer_generation_sequence_error::none && OidIsValid(sequence_oid);
    }
};

/// Validate the SQL installation object's exact non-wrapping BIGINT contract.
[[nodiscard]] inline minimizer_generation_sequence_inspection
inspect_minimizer_generation_sequence_contract() {
    const Oid internal_namespace =
        bioseq::postgres::support::invoke_pg_as_cpp([] { return bioseq_internal_ns(); });
    const Oid sequence_oid = indexing_entity_handles::minimizer_generation_id_seq.resolve_oid(
        bioseq::postgres::support::catalog_missing_policy::return_invalid);
    if (!OidIsValid(sequence_oid)) {
        return {.error = minimizer_generation_sequence_error::missing};
    }
    Relation sequence_relation = bioseq::postgres::support::invoke_pg_as_cpp([sequence_oid] {
        return bioseq::postgres::support::try_open_postgres_relation(sequence_oid, AccessShareLock);
    });
    if (sequence_relation == nullptr) {
        return {.error = minimizer_generation_sequence_error::missing};
    }
    minimizer_generation_sequence_error error = minimizer_generation_sequence_error::none;
    if (RelationGetNamespace(sequence_relation) != internal_namespace) {
        error = minimizer_generation_sequence_error::wrong_namespace;
    } else if (sequence_relation->rd_rel->relkind != RELKIND_SEQUENCE) {
        error = minimizer_generation_sequence_error::wrong_relkind;
    } else if (sequence_relation->rd_rel->relpersistence != RELPERSISTENCE_PERMANENT) {
        error = minimizer_generation_sequence_error::wrong_persistence;
    }
    bioseq::postgres::support::invoke_pg_as_cpp(
        [sequence_relation] { relation_close(sequence_relation, AccessShareLock); });
    if (error != minimizer_generation_sequence_error::none) {
        return {.error = error};
    }
    HeapTuple sequence_tuple = bioseq::postgres::support::invoke_pg_as_cpp(
        [sequence_oid] { return SearchSysCache1(SEQRELID, ObjectIdGetDatum(sequence_oid)); });
    if (!HeapTupleIsValid(sequence_tuple)) {
        return {.error = minimizer_generation_sequence_error::missing_catalog_row};
    }
    const Form_pg_sequence sequence = reinterpret_cast<Form_pg_sequence>(GETSTRUCT(sequence_tuple));
    if (sequence->seqtypid != INT8OID) {
        error = minimizer_generation_sequence_error::wrong_type;
    } else if (sequence->seqincrement != 1 || sequence->seqmin != 1 ||
               sequence->seqmax != (std::numeric_limits<std::int64_t>::max)() ||
               sequence->seqstart != 1 || sequence->seqcache != 1) {
        error = minimizer_generation_sequence_error::wrong_bounds;
    } else if (sequence->seqcycle) {
        error = minimizer_generation_sequence_error::cycling;
    }
    bioseq::postgres::support::invoke_pg_as_cpp(
        [sequence_tuple] { ReleaseSysCache(sequence_tuple); });
    return {.error = error,
            .sequence_oid =
                error == minimizer_generation_sequence_error::none ? sequence_oid : InvalidOid};
}

/// Allocate a positive signed-SQL generation ID. This helper never creates the
/// sequence. Installation owns that atomic DDL step.
[[nodiscard]] std::uint64_t allocate_minimizer_generation_id_pg() {
    const minimizer_generation_sequence_inspection sequence =
        inspect_minimizer_generation_sequence_contract();
    if (!sequence) {
        minimizer_generation_storage_error_as_cpp(
            fmt::format("owned generation sequence contract mismatch: {}",
                        minimizer_generation_sequence_error_name(sequence.error)));
    }
    const std::int64_t generation_id = bioseq::postgres::support::invoke_pg_as_cpp(
        [&sequence] { return nextval_internal(sequence.sequence_oid, false); });
    if (generation_id <= 0) {
        minimizer_generation_storage_error_as_cpp(
            "generation sequence exhausted its positive BIGINT range");
    }
    return static_cast<std::uint64_t>(generation_id);
}

} // namespace bioseq::storage::indexing
