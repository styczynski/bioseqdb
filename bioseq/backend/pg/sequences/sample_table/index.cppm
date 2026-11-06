/**
 * @file index.cppm
 * @brief Create synthetic sequence tables and inspect their  storage locators.
 */
module;

#include <hwy/highway.h>

#include <bioseq/pg_compat.h>

extern "C" {
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "utils/lsyscache.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "miscadmin.h"
#include "fmgr.h"
#include "catalog/namespace.h"
#include "catalog/dependency.h"
#include "utils/fmgrprotos.h"
#include "nodes/makefuncs.h"
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.postgres.sequences.sample_table;

import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.hashing;
import bioseq.simd.utils;
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.support;
import bioseq.postgres.support.datums;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.support.sql_boundary;
import bioseq.common.seqan_adapter;
import bioseq.parallelism.memory_admission;
import bioseq.sequences;
import bioseq.sequences.generation;
import bioseq.postgres.storage.manager;
import bioseq.storage.size;
import bioseq.storage.layout;
import bioseq.storage.scanner;
import bioseq.postgres.table.io;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.physical;
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.sequences.sample_table.sql_rows;
import bioseq.postgres.sequences.random;
import bioseq.postgres.scheduler.supervisor;

using bioseq::storage::sequence_size_t;

namespace bioseq::sequences::generation::pg {

constexpr std::size_t k_fragment_storage_units = bioseq::storage::default_fragment_storage_units;

/** Free a detoasted sequence only when PostgreSQL made a separate copy. */
class detoasted_sequence final {
public:
    /** Open the sequence and remember whether its detoasted copy ieeds freeing. */
    explicit detoasted_sequence(Datum source)
        : source_{source},
          value_{bioseq::postgres::support::invoke_pg_as_cpp([source] {
              return reinterpret_cast<bioseq::pg_types::seq*>(
                  PG_DETOAST_DATUM(source));
          })},
          owns_value_{
              reinterpret_cast<Pointer>(value_) != DatumGetPointer(source_)} {
    }

    detoasted_sequence(const detoasted_sequence&) = delete;
    detoasted_sequence& operator=(const detoasted_sequence&) = delete;

    /** Releases the detoasted copy while leaving an inline source untouched. */
    ~detoasted_sequence() noexcept {
        if (owns_value_ && value_ != nullptr) {
            pfree(value_);
        }
    }

    /** @return Sequence header. Keep a borrowed input alive while using this wrapper. */
    [[nodiscard]] bioseq::pg_types::seq* get() const noexcept {
        return value_;
    }

private:
    Datum source_;
    bioseq::pg_types::seq* value_;
    bool owns_value_;
};

/**
 * Keep the table and sequence writers needed to insert sample rows.
 * Close, the slot, caches, and relation with close_sample_relation_after_writes
 * after insertion succeeds or fails.
 */
struct sample_table_write_context {
    Oid parent_relid = InvalidOid;
    Relation parent_rel = nullptr;
    TupleDesc parent_td = nullptr;
    TupleTableSlot* slot = nullptr;
    column_write_state* write_state = nullptr;
    bioseq::storage::seq_caches* caches = nullptr;
    sequence_size_t cap_bytes = 0;
    std::uint64_t main_storage_id = 0;
};

/** Validated optional quality-column identity; the biological source decides its length. */
struct sample_quality_column {
    std::string definition;
    std::uint8_t alphabet_code{};
};

/** Validate and quote a requested quality column before any table is created. */
[[nodiscard]] std::optional<sample_quality_column> resolve_sample_quality_column(
        const std::optional<std::string>& name,
        const std::optional<std::string>& alphabet,
        std::span<const std::string_view> reserved_names) {
    if (!name) return std::nullopt;
    if (name->empty() || name->size() >= NAMEDATALEN
        || name->find('\0') != std::string::npos
        || std::ranges::find(reserved_names, *name) != reserved_names.end()) {
        bioseq::postgres::support::raise_sql_error(ERRCODE_INVALID_PARAMETER_VALUE,
            {.message = "quality_column must be a distinct nonempty column name shorter than 64 bytes"});
    }
    const auto selected = bioseq::postgres::support::invoke_pg_as_cpp([&alphabet] {
        return parse_alphabet_name_checked(alphabet.value_or("phred63").c_str(), "quality_column");
    });
    if (!is_quality_alphabet_code(selected)) {
        bioseq::postgres::support::raise_sql_error(ERRCODE_INVALID_PARAMETER_VALUE,
            {.message = "quality_alphabet must name a supported quality alphabet"});
    }
    const auto quoted = bioseq::postgres::support::invoke_pg_as_cpp([&name] {
        return std::string{quote_identifier(name->c_str())};
    });
    return sample_quality_column{
        .definition = ", " + quoted + " bioseq.seq('"
            + std::string{supported_alphabet_name(selected)} + "', 64)",
        .alphabet_code = selected,
    };
}

/** The parent row's locator and its existing deferred storage-publication token. */
struct generated_sample_source {
    bioseq::pg_types::seq* value;
    bioseq::storage::pending_sequence_source publication;
};

/** Generate any rank-packed alphabet directly into the existing streamed storage writer.
 * The returned token must be published only after its parent row has been written. */
[[nodiscard]] generated_sample_source prepare_random_sample_source(
        sample_table_write_context& context, std::uint8_t alphabet_code,
        std::int16_t attribute, sequence_size_t length,
        std::uint64_t seed, std::uint64_t ordinal) {
    std::optional<generated_sample_source> output;
    pgseq_supported_rank_packed_alphabets::dispatch_alphabet_by_code(alphabet_code,
        [&context, alphabet_code, attribute, length, seed, ordinal, &output]<class Alphabet>() {
        using Configuration = sequence_custom_alphabet<Alphabet, std::uint64_t>;
        using Traits = typename compact_sequence_range<Configuration>::traits;
        constexpr auto letters_per_unit = Traits::letters_per_unit;
        constexpr auto fragment_symbols = k_fragment_storage_units * letters_per_unit;
        static constexpr auto cdf = make_integer_cdf(base_alphabet_probability<Alphabet>::probability());
        const auto padding = static_cast<std::uint16_t>(
            length % letters_per_unit == 0u ? 0u : letters_per_unit - length % letters_per_unit);
        auto producer = [length, padding, seed, ordinal](
                std::size_t fragment, bioseq::pg_types::seq* destination,
                MemoryContext, bool last) {
            const auto first_symbol = static_cast<sequence_size_t>(fragment) * fragment_symbols;
            const auto count = last ? length - first_symbol
                                    : static_cast<sequence_size_t>(fragment_symbols);
            destination->bp_size = static_cast<bioseq::pg_types::seq::bp_size_t>(count);
            destination->padding_letters = last ? padding : std::uint16_t{0};
            const auto unit_count = count / letters_per_unit
                + static_cast<sequence_size_t>(count % letters_per_unit != 0u);
            generate_units_cpu_for_sequence<Traits>(
                {reinterpret_cast<std::uint64_t*>(destination->data),
                 static_cast<std::size_t>(unit_count)},
                length, ordinal, 0u, cdf, seed,
                static_cast<std::uint64_t>(fragment) * k_fragment_storage_units);
        };
        auto pending = bioseq::postgres::support::invoke_pg_as_cpp(
            [&context, alphabet_code, attribute, length, padding, &producer] {
            return bioseq::storage::bioseq_default_storage_manager::insert_sequence_streamed(
                context.parent_relid, context.main_storage_id,
                context.parent_rel->rd_locator.dbOid,
                context.parent_rel->rd_locator.spcOid,
                context.parent_rel->rd_locator.relNumber,
                attribute, context.parent_td, alphabet_code, sizeof(std::uint64_t),
                length, padding, context.cap_bytes, context.caches, producer);
        });
        const auto locator = pending.locator();
        auto* value = bioseq::postgres::support::invoke_pg_as_cpp(
            [&context, alphabet_code, length, padding, &locator] {
            return bioseq::storage::alloc_locator_stub_in(
                get_column_write_per_tuple_context(context.write_state), alphabet_code,
                sizeof(std::uint64_t), padding, length, locator);
        });
        output.emplace(generated_sample_source{value, std::move(pending)});
    });
    return std::move(*output);
}

/** Raise a PostgreSQL error that identifies the failing sample-table routine */
[[noreturn]] void raise_sample_table_error(
        const char* routine,
        std::string message,
        std::optional<std::string> detail = std::nullopt) {
    bioseq::postgres::support::raise_sql_error(
        ERRCODE_INTERNAL_ERROR,
        {
            .message = std::string{routine} + ": " + std::move(message),
            .detail = std::move(detail),
        });
}

/** Create a sample table in the current creation namespace and return  its OID. */
[[nodiscard]] Oid create_sample_relation(
        const char* routine,
        const std::string& table_name,
        const char* column_definition) {
    const Oid creation_namespace =
        bioseq::postgres::support::invoke_pg_as_cpp([&table_name] {
            RangeVar* relation = makeRangeVar(
                nullptr, pstrdup(table_name.c_str()), -1);
            return RangeVarGetCreationNamespace(relation);
        });
    const std::string quoted_name =
        bioseq::postgres::support::invoke_pg_as_cpp([&table_name] {
            return std::string{quote_identifier(table_name.c_str())};
        });
    std::string ddl;
    ddl.reserve(
        std::char_traits<char>::length(column_definition)
        + quoted_name.size() + 32);
    ddl.append("CREATE TABLE ");
    ddl.append(quoted_name);
    ddl.push_back(' ');
    ddl.append(column_definition);

    bioseq::postgres::support::spi_session session;
    const int connect_status = session.connect();
    if (connect_status != SPI_OK_CONNECT) {
        raise_sample_table_error(
            routine,
            "could not connect to SPI",
            "SPI_connect status=" + std::to_string(connect_status));
    }
    const int execute_status =
        bioseq::postgres::support::invoke_pg_as_cpp([&ddl] {
            return SPI_execute(
                ddl.c_str(), /*read_only=*/false, /*tcount=*/0);
        });
    if (execute_status != SPI_OK_UTILITY) {
        raise_sample_table_error(
            routine,
            "could not create the requested table",
            "SPI_execute status=" + std::to_string(execute_status));
    }
    const Oid created_relation =
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&table_name, creation_namespace] {
                return get_relname_relid(
                    table_name.c_str(), creation_namespace);
            });
    if (!OidIsValid(created_relation)) {
        raise_sample_table_error(
            routine,
            "could not locate the newly created table");
    }
    return created_relation;
}

/** Open the new sample table and prepare its row and sequence-fragment writers. */
void open_sample_relation_for_writes(
        sample_table_write_context& context,
        Oid parent_relid) {
    context.parent_relid = parent_relid;

    const ResourceOwner saved_owner = CurrentResourceOwner;
    context.parent_rel =
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&context, saved_owner] {
                CurrentResourceOwner = TopTransactionResourceOwner;
                Relation relation =
                    table_open(context.parent_relid, RowExclusiveLock);
                CurrentResourceOwner = saved_owner;
                return relation;
            });
    context.parent_td =
        bioseq::postgres::support::invoke_pg_as_cpp([&context] {
            return RelationGetDescr(context.parent_rel);
        });
    context.slot =
        bioseq::postgres::support::invoke_pg_as_cpp([&context] {
            return MakeTupleTableSlot(
                context.parent_td, &TTSOpsVirtual
#if PG_VERSION_NUM >= 190000
                , 0
#endif
            );
        });
    const SubTransactionId subtransaction =
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            return GetCurrentSubTransactionId();
        });
    context.write_state =
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&context, subtransaction] {
                return initialize_column_write_state(
                    context.parent_rel,
                    context.parent_td,
                    context.parent_relid,
                    subtransaction);
            });
    if (context.write_state->seq_caches == nullptr) {
        auto* fresh =
            bioseq::postgres::support::invoke_pg_as_cpp([&context] {
                return bioseq::storage::alloc_seq_caches(
                    MemoryContextGetParent(
                        context.write_state->stripe_write_context));
            });
        fresh->side = bioseq::storage::seq_caches::access_side::write;
        fresh->lock_mode = RowExclusiveLock;
        fresh->main_database_oid =
            context.parent_rel->rd_locator.dbOid;
        fresh->main_tablespace_oid =
            context.parent_rel->rd_locator.spcOid;
        fresh->main_relfilenode =
            context.parent_rel->rd_locator.relNumber;
        fresh->cap_bytes_snapshot =
            bioseq::storage::read_max_sub_table_cap_bytes();
        context.write_state->seq_caches = fresh;
    }
    context.caches =
        static_cast<bioseq::storage::seq_caches*>(
            context.write_state->seq_caches);
    context.cap_bytes = context.caches->cap_bytes_snapshot;
    context.main_storage_id =
        bioseq::postgres::support::invoke_pg_as_cpp([&context] {
            return column_storage_id(
                context.parent_rel, false);
        });
}

/** Finish the parent and fragment writers before their primary-key build.
 * The registered parent flush adapter also flushes its sequence-fragment caches.
 * Writer frames retain transaction ownership on failure; caller cleanup closes
 * only the relation and any cache which has not been handed back to its frame. */
void finish_sample_write_states(sample_table_write_context& context) {
    const Oid parent_file_number = context.parent_rel->rd_locator.relNumber;
    const auto subtransaction = bioseq::postgres::support::invoke_pg_as_cpp([] {
        return GetCurrentSubTransactionId();
    });
    bioseq::postgres::support::invoke_pg_as_cpp([parent_file_number, subtransaction] {
        flush_relation_write_state(parent_file_number, subtransaction);
        CommandCounterIncrement();
    });

    std::vector<Oid> fragment_file_numbers;
    if (context.caches != nullptr) {
        fragment_file_numbers.reserve(context.caches->entries.size());
        for (const auto& cached : context.caches->entries) {
            if (cached.second.rel != nullptr)
                fragment_file_numbers.push_back(cached.second.rel->rd_locator.relNumber);
        }
    }
    for (const Oid file_number : fragment_file_numbers) {
        bioseq::postgres::support::invoke_pg_as_cpp([file_number, subtransaction] {
            const ResourceOwner previous_owner = CurrentResourceOwner;
            CurrentResourceOwner = TopTransactionResourceOwner;
            pop_relation_column_write_state(file_number, subtransaction, 0, true);
            CurrentResourceOwner = previous_owner;
        });
    }
    // Popping the parent destroys its cache. Clear borrowed pointers first so a
    // later failure cannot make resource cleanup follow those freed objects.
    context.write_state = nullptr;
    context.caches = nullptr;
    bioseq::postgres::support::invoke_pg_as_cpp([parent_file_number, subtransaction] {
        const ResourceOwner previous_owner = CurrentResourceOwner;
        CurrentResourceOwner = TopTransactionResourceOwner;
        pop_relation_column_write_state(parent_file_number, subtransaction, 0, true);
        CurrentResourceOwner = previous_owner;
        CommandCounterIncrement();
    });
}

/** Finalize the primary key and owned serial sequence after direct sample writes.
 * The caller has closed its tuple slot and parent relation before this utility
 * command; the existing table-AM index builder supplies visible rows and TIDs.
 * Rows with empty sequence values still receive ordinary primary-key entries. */
void finalize_sample_identifiers(Oid parent_relid, std::int64_t last_id) {
    const auto qualified_index_name = bioseq::postgres::support::invoke_pg_as_cpp([parent_relid] {
        Relation parent = table_open(parent_relid, AccessShareLock);
        const Oid primary_key = bioseq::postgres::support::find_primary_key_oid(parent);
        table_close(parent, AccessShareLock);
        if (!OidIsValid(primary_key))
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                errmsg("sample table primary-key index is missing")));
        return std::string{quote_qualified_identifier(
            get_namespace_name(get_rel_namespace(primary_key)), get_rel_name(primary_key))};
    });
    bioseq::postgres::support::spi_session session;
    if (session.connect() != SPI_OK_CONNECT)
        raise_sample_table_error("sample generation", "could not connect to SPI for primary-key rebuild");
    const auto statement = "REINDEX INDEX " + qualified_index_name;
    const auto status = bioseq::postgres::support::invoke_pg_as_cpp([&statement] {
        return SPI_execute(statement.c_str(), false, 0);
    });
    if (status != SPI_OK_UTILITY)
        raise_sample_table_error("sample generation", "could not rebuild the sample primary key",
            "SPI_execute status=" + std::to_string(status));
    bioseq::postgres::support::invoke_pg_as_cpp([parent_relid, last_id] {
        List* sequences = getOwnedSequences(parent_relid);
        if (list_length(sequences) != 1)
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                errmsg("sample table must own exactly one serial sequence")));
        const Oid sequence = linitial_oid(sequences);
        list_free(sequences);
        (void)DirectFunctionCall3(setval3_oid, ObjectIdGetDatum(sequence),
            Int64GetDatum(last_id), BoolGetDatum(true));
    });
}

/**
 * Close the sample table's acquired resources, trying every cleanup action
 * before rethrowing the first failure.
 * @param commit Whether to finish published writes and save fragment-table byte counts.
 */
void close_sample_relation_after_writes(
        sample_table_write_context& context,
        bool commit) {
    std::exception_ptr failure;
    /* Run every cleanup action even when an earlier action throws. */
    const auto attempt = [&failure](auto&& operation) {
        try {
            operation();
        } catch (...) {
            if (failure == nullptr) {
                failure = std::current_exception();
            }
        }
    };

    if (context.slot != nullptr) {
        attempt([&context] {
            bioseq::postgres::support::invoke_pg_as_cpp([&context] {
                ExecDropSingleTupleTableSlot(context.slot);
            });
            context.slot = nullptr;
        });
    }
    if (commit && failure == nullptr && context.write_state != nullptr) {
        attempt([&context] { finish_sample_write_states(context); });
    }
    if (context.caches != nullptr) {
        const bool save_cache_changes = commit && failure == nullptr;
        attempt([&context, save_cache_changes] {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [&context, save_cache_changes] {
                    context.caches->close_all(save_cache_changes);
                });
            context.caches = nullptr;
        });
    }
    if (context.parent_rel != nullptr) {
        attempt([&context] {
            const ResourceOwner saved_owner = CurrentResourceOwner;
            bioseq::postgres::support::invoke_pg_as_cpp(
                [&context, saved_owner] {
                    CurrentResourceOwner =
                        TopTransactionResourceOwner;
                    table_close(context.parent_rel, NoLock);
                    CurrentResourceOwner = saved_owner;
                });
            context.parent_rel = nullptr;
        });
    }

    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

/**
 * Write a packed biological sequence in storage fragments.
 * @tparam p_sequence_config Sequence configuration using 64-bit packed units.
 * @return Pending source to publish after writing its parent row.
 */
template <bioseq::sequences::generation::packed_sequence_config p_sequence_config>
[[nodiscard]] bioseq::storage::pending_sequence_source insert_compact_range_streamed(
    const compact_sequence_range<p_sequence_config>& seq,
    Relation parent_rel,
    TupleDesc parent_td,
    std::int16_t attno,
    std::uint64_t main_storage_id,
    sequence_size_t cap_bytes,
    bioseq::storage::seq_caches* caches) {
    using traits = typename compact_sequence_range<p_sequence_config>::traits;
    using storage_unit_type = typename traits::storage_unit;
    constexpr auto alphabet_code = supported_biological_alphabets::template index_of<typename traits::alphabet_type>();
    constexpr auto storage_width = sizeof(storage_unit_type);
    constexpr auto fragment_symbols = k_fragment_storage_units * traits::letters_per_unit;

    const sequence_size_t total_bp = static_cast<sequence_size_t>(seq.bp_size());
    const std::uint16_t padding = static_cast<std::uint16_t>(seq.padding());
    /* Copy each requested packed fragment directly into storage-manager memory. */
    auto producer = [&seq, total_bp, padding](
        std::size_t frag_no,
        bioseq::pg_types::seq* dst,
        MemoryContext /*scratch*/,
        bool is_last) noexcept {
        const sequence_size_t frag_global_bp_start =
            static_cast<sequence_size_t>(frag_no) *
            static_cast<sequence_size_t>(fragment_symbols);
        const sequence_size_t this_fragment_bp = is_last
            ? (total_bp - frag_global_bp_start)
            : static_cast<sequence_size_t>(fragment_symbols);
        dst->bp_size = static_cast<bioseq::pg_types::seq::bp_size_t>(this_fragment_bp);
        dst->padding_letters = is_last ? padding : std::uint16_t{0};

        const std::size_t unit0 =
            static_cast<std::size_t>(frag_global_bp_start / traits::letters_per_unit);
        const std::size_t units =
            static_cast<std::size_t>(
                (this_fragment_bp + traits::letters_per_unit - 1) /
                traits::letters_per_unit);
        std::memcpy(
            dst->data,
            seq.packed_units.data() + unit0,
            units * sizeof(storage_unit_type));
    };

    return bioseq::postgres::support::invoke_pg_as_cpp(
        [parent_rel, parent_td, attno, main_storage_id,
         cap_bytes, caches, total_bp, padding, &producer] {
            return bioseq::storage::bioseq_default_storage_manager::
                insert_sequence_streamed(
                    RelationGetRelid(parent_rel),
                    main_storage_id,
                    parent_rel->rd_locator.dbOid,
                    parent_rel->rd_locator.spcOid,
                    parent_rel->rd_locator.relNumber,
                    attno,
                    parent_td,
                    alphabet_code,
                    storage_width,
                    total_bp,
                    padding,
                    cap_bytes,
                    caches,
                    producer);
        });
}

/**
 * Account for bounded rank scratch and retained pair descriptors in a wave.
 * Each query owns a separate permit for its exact packed output allocation.
 */
template<packed_sequence_config Configuration>
[[nodiscard]] std::uint64_t estimate_query_pair_peak_bytes(
    const bioseq::sequences::generation::generator_params&) noexcept {
    using generator_type = bioseq::sequences::generation::sequence_generator<Configuration>;
    return 2u * bioseq::simd::sequence_rank_io_chunk_capacity
        + sizeof(typename generator_type::query_target_pair);
}

}  // namespace bioseq::sequences::generation::pg

export namespace bioseq::sequences::generation::pg::sql_routines {

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::volatile_,
    .parallel = bioseq::sql::parallel_safety::restricted,
    .support = "random_support",
})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]]
/**
 * @brief Generate a native random sequence with an inferred or explicit alphabet.
 * Length bounds count symbols; storage_bits selects packing independently.
 * NULL max_length selects min_length. With no mean, zero stddev samples uniformly
 * within differing bounds; positive stddev samples a normal length at the midpoint.
 * An explicit mean with zero stddev gives a constant. Lengths are rounded and
 * clamped to the inclusive bounds.
 * @param alphabet Registered alphabet, or NULL for INSERT/cast inference.
 * @param storage_bits Build-admitted packing width; defaults to the inferred width or 64.
 * @param min_length Inclusive minimum symbol count; NULL selects 1000; zero is supported.
 * @param max_length Inclusive maximum symbol count; NULL selects min_length.
 * @param length_mean Finite mean; NULL selects uniform sampling at zero stddev,
 * or midpoint-centered normal sampling when stddev is positive.
 * @param length_stddev Finite nonnegative standard deviation; NULL or omitted selects zero.
 * @param seed Optional reproducible call-site stream seed.
 * @return A sequence in the resolved native layout.
 */
std::optional<Datum> random_seq(
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"NULL"}})]] std::optional<std::string> alphabet,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"NULL"}})]] std::optional<std::int32_t> storage_bits,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"1000"}})]] std::optional<std::int64_t> min_length,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"NULL"}})]] std::optional<std::int64_t> max_length,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"NULL"}})]] std::optional<double> length_mean,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"0"}})]] std::optional<double> length_stddev,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"NULL"}})]] std::optional<std::int64_t> seed,
    bioseq::postgres::support::function_call_context call) {
    bioseq::postgres::random::request request{
        .kind = bioseq::postgres::random::request_kind::sequence,
        .alphabet = std::move(alphabet), .storage_bits = storage_bits,
        .minimum = min_length, .maximum = max_length, .mean = length_mean, .stddev = length_stddev,
        .seed = seed.transform([](std::int64_t value) { return static_cast<std::uint64_t>(value); })};
    return bioseq::postgres::random::generate_scalar(std::move(request), call);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::volatile_,
    .parallel = bioseq::sql::parallel_safety::restricted,
    .support = "random_support",
})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]]
/**
 * @brief Mutate a sequence while preserving its alphabet, packing width and CIGAR space.
 * CIGAR edits act on logical coordinates and keep adjacent runs canonical.
 * @param source Native input sequence; NULL returns NULL.
 * @param substitution_rate Probability of replacing a visited source symbol.
 * @param insertion_rate Probability of inserting after a visited source symbol.
 * @param deletion_rate Probability of deleting following source symbols.
 * @param insertion_length_mean Mean inserted event length before rounding and clamping to zero.
 * @param deletion_length_mean Mean deleted event length before rounding and clamping to zero.
 * @param insertion_length_stddev Nonnegative inserted event length deviation.
 * @param deletion_length_stddev Nonnegative deleted event length deviation.
 * @param seed Optional reproducible call-site stream seed.
 * @return Mutated native output, or NULL for a NULL source.
 */
std::optional<Datum> random_mutate(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] std::optional<Datum> source,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"0.01"}})]] std::optional<double> substitution_rate,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"0.0005"}})]] std::optional<double> insertion_rate,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"0.0005"}})]] std::optional<double> deletion_rate,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"2"}})]] std::optional<double> insertion_length_mean,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"2"}})]] std::optional<double> deletion_length_mean,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"1"}})]] std::optional<double> insertion_length_stddev,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"1"}})]] std::optional<double> deletion_length_stddev,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"NULL"}})]] std::optional<std::int64_t> seed,
    bioseq::postgres::support::function_call_context call) {
    bioseq::postgres::random::request request{
        .kind = bioseq::postgres::random::request_kind::mutation,
        .seed = seed.transform([](std::int64_t value) { return static_cast<std::uint64_t>(value); }),
        .source = source};
    request.mutation.substitution_rate = substitution_rate.value_or(0.01);
    request.mutation.insertion_rate = insertion_rate.value_or(0.0005);
    request.mutation.deletion_rate = deletion_rate.value_or(0.0005);
    request.mutation.insertion_length_mean = insertion_length_mean.value_or(2.0);
    request.mutation.deletion_length_mean = deletion_length_mean.value_or(2.0);
    request.mutation.insertion_length_stddev = insertion_length_stddev.value_or(1.0);
    request.mutation.deletion_length_stddev = deletion_length_stddev.value_or(1.0);
    return bioseq::postgres::random::generate_scalar(std::move(request), call);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::volatile_,
    .parallel = bioseq::sql::parallel_safety::restricted,
    .support = "random_support",
})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::pg::text})]]
/**
 * @brief Generate a parser-compatible header without a record marker or newline.
 * Length bounds count the alphanumeric component; structured formats add grammar
 * bytes. NULL or auto selects plain output. Length sampling follows random_seq.
 * @param header_format Structured header grammar, default plain.
 * @param min_length Inclusive component minimum, at least one; NULL selects 16.
 * @param max_length Inclusive component maximum; NULL selects min_length.
 * @param length_mean Finite mean; NULL selects uniform sampling at zero stddev,
 * or midpoint-centered normal sampling when stddev is positive.
 * @param length_stddev Finite nonnegative component-length deviation; NULL or omitted selects zero.
 * @param seed Optional reproducible call-site stream seed.
 * @return Header text suitable for the selected bioseq.load parser.
 */
std::optional<Datum> random_header(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.header_format">,
        .default_value = bioseq::sql::checked_sql{"'plain'"}})]] std::optional<Datum> header_format,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"16"}})]] std::optional<std::int64_t> min_length,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"NULL"}})]] std::optional<std::int64_t> max_length,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"NULL"}})]] std::optional<double> length_mean,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"0"}})]] std::optional<double> length_stddev,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"NULL"}})]] std::optional<std::int64_t> seed,
    bioseq::postgres::support::function_call_context call) {
    bioseq::postgres::random::request request{
        .kind = bioseq::postgres::random::request_kind::header,
        .minimum = min_length, .maximum = max_length, .mean = length_mean, .stddev = length_stddev,
        .seed = seed.transform([](std::int64_t value) { return static_cast<std::uint64_t>(value); }),
        .header_format = header_format};
    return bioseq::postgres::random::generate_scalar(std::move(request), call);
}

/** Bind direct sequence casts and charge scalar generation's real planner cost. */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::pg::internal})]]
Datum random_support(
    [[bioseq_sql::param({.sql_type = bioseq::sql::pg::internal})]] Datum request_datum) {
    return bioseq::postgres::random::planner_support(request_datum);
}


[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
/**
 * Create a Bioseq table of random native-rank sequences, writing each seqence in
 * fragments as it is generated.
 * @param table_name Unqualified table name in the current creation namespace.
 * @param n_sequences Number of rows, from 1 to 100 million.
 * @param sequence_bp_size Nonnegative length of each sequence in base pairs.
 * @param alphabet Supported biological alphabet name.
 * @param seed Random seed, defaulting to 42.
 * @param quality_column Optional quality column name; its length matches seq.
 * @param quality_alphabet Registered quality alphabet, default phred63.
 * @return Created table OID.
 *
 * ```sql
 * -- Generate a small reproducible DNA dataset with seed 42, inspect its rows, then roll back the example table.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * SELECT bioseq.create_sample_table('doc_generated', 3, 512, 'dna4', 42);
 * SELECT * FROM doc_generated LIMIT 3;
 * ROLLBACK;
 * ```
 */
[[nodiscard]] Oid create_sample_table(
        std::optional<std::string> table_name,
        std::optional<std::int32_t> n_sequences,
        std::optional<std::int64_t> sequence_bp_size,
        [[bioseq_sql::param({
            .default_value = bioseq::sql::checked_sql{"'dna4'"},
        })]]
        std::optional<std::string> alphabet,
        [[bioseq_sql::param({
            .default_value = bioseq::sql::checked_sql{"42"},
        })]]
        std::optional<std::int64_t> seed,
        [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"NULL"}})]]
        std::optional<std::string> quality_column,
        [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"'phred63'"}})]]
        std::optional<std::string> quality_alphabet) {
    if (!table_name.has_value() || !n_sequences.has_value()
        || !sequence_bp_size.has_value()) {
        bioseq::postgres::support::raise_sql_error(
            ERRCODE_NULL_VALUE_NOT_ALLOWED,
            {
                .message =
                    "bioseq.create_sample_table requires table_name, "
                    "n_sequences, and sequence_bp_size",
            });
    }
    const std::int32_t sequence_count = *n_sequences;
    if (sequence_count < 1) {
        bioseq::postgres::support::raise_sql_error(
            ERRCODE_INVALID_PARAMETER_VALUE,
            {
                .message =
                    "bioseq.create_sample_table n_sequences must be at "
                    "least 1",
            });
    }
    if (sequence_count > 100'000'000) {
        bioseq::postgres::support::raise_sql_error(
            ERRCODE_INVALID_PARAMETER_VALUE,
            {
                .message =
                    "bioseq.create_sample_table n_sequences exceeds the "
                    "100 million safety limit",
            });
    }
    const std::int64_t sequence_bp = *sequence_bp_size;
    if (sequence_bp < 0) {
        bioseq::postgres::support::raise_sql_error(
            ERRCODE_INVALID_PARAMETER_VALUE,
            {
                .message =
                    "bioseq.create_sample_table sequence_bp_size must not "
                    "be negative",
            });
    }
    const std::string resolved_alphabet =
        alphabet.value_or(std::string{"dna4"});
    const auto selected_alphabet = supported_biological_alphabets::try_index_of_name(resolved_alphabet.c_str());
    if (!selected_alphabet) {
        bioseq::postgres::support::raise_sql_error(
            ERRCODE_INVALID_PARAMETER_VALUE,
            {
                .message =
                    "bioseq.create_sample_table requires a supported biological alphabet",
            });
    }
    const std::uint64_t seed_value = static_cast<std::uint64_t>(
        seed.value_or(std::int64_t{42}));

    static constexpr std::array reserved_names{std::string_view{"id"}, std::string_view{"seq"}};
    const auto quality = resolve_sample_quality_column(quality_column, quality_alphabet, reserved_names);
    const auto quality_seed = bioseq::common::splitmix64(seed_value ^ 0x7175616c697479ULL);
    bioseq::postgres::support::invoke_pg_as_cpp([] { bioseq::postgres::scheduler::backend_sync(); });
    sample_table_write_context context{};
    std::exception_ptr failure;
    try {
        const std::string columns = "(id BIGSERIAL PRIMARY KEY, seq bioseq.seq('"
            + resolved_alphabet + "', 64)" + (quality ? quality->definition : "") + ") USING bioseq;";
        const auto relation = create_sample_relation("bioseq.create_sample_table", *table_name, columns.c_str());
        open_sample_relation_for_writes(context, relation);
        for (std::int32_t sequence_index = 0; sequence_index < sequence_count; ++sequence_index) {
            bioseq::postgres::support::invoke_pg_as_cpp([] { CHECK_FOR_INTERRUPTS(); });
            auto sequence = prepare_random_sample_source(context, *selected_alphabet, 2,
                static_cast<sequence_size_t>(sequence_bp), seed_value,
                static_cast<std::uint64_t>(sequence_index));
            std::optional<generated_sample_source> scores;
            if (quality) {
                // Isolate row identity before the legacy length-dependent chunk mapping.
                const auto row_seed = bioseq::common::splitmix64(
                    quality_seed ^ static_cast<std::uint64_t>(sequence_index));
                scores.emplace(prepare_random_sample_source(context, quality->alphabet_code, 3,
                    static_cast<sequence_size_t>(sequence_bp), row_seed, 0u));
            }
            bioseq::postgres::support::invoke_pg_as_cpp([&context, &sequence, &scores, sequence_index] {
                bioseq::postgres::support::pg_slot_datums_view values{context.slot};
                values.set(1, static_cast<std::int64_t>(sequence_index) + 1);
                values.set_raw(2, bioseq::postgres::support::pg_datum::from_pointer(sequence.value));
                if (scores) values.set_raw(3, bioseq::postgres::support::pg_datum::from_pointer(scores->value));
                values.store_virtual();
                (void)write_column_row(context.write_state, context.slot->tts_values, context.slot->tts_isnull);
            });
            bioseq::postgres::support::invoke_pg_as_cpp([&sequence, &scores] {
                sequence.publication.publish_after_parent_row();
                if (scores) scores->publication.publish_after_parent_row();
            });
        }
    } catch (...) {
        failure = std::current_exception();
    }
    try {
        close_sample_relation_after_writes(context, failure == nullptr);
    } catch (...) {
        if (failure == nullptr) failure = std::current_exception();
    }
    if (failure != nullptr) std::rethrow_exception(failure);
    finalize_sample_identifiers(context.parent_relid, sequence_count);
    return context.parent_relid;
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
/**
 * Create native query-target pairs with known source coordinates. Generate  one
 * reference, sample, query fragments, and add substitutions or indels using
 * the supplied rates. Each row stores the reference as its target, plus the
 * query's original interval and strand Null generator settings use
 * generator_params defaults.
 * @param table_name Unqualified table name in the current creation namespace.
 * @param fragment_count Number of query-target rows.
 * @param fragment_length_mean Mean query length before edits, in bases.
 * @param fragment_length_stddev Standard deviation of that length, in bases.
 * @param reference_length Reference length in bases.
 * @param substitution_rate Substitution  probability per source base.
 * @param insertion_rate Insertion-event probability per source base.
 * @param deletion_rate Deletion-event probability per source base.
 * @param insertion_length_mean Mean inserted run length in baser.
 * @param deletion_length_mean Mean deleted run length in bases.
 * @param reverse_complement_rate Probability of reverse-complementing a query.
 * @param seed Random seed.
 * @param alphabet Native biological alphabet; proteins use forward ground truth.
 * @param quality_column Optional quality column name; its length matches the final mutated query.
 * @param quality_alphabet Registered quality alphabet, default phred63.
 * @return Created table OID after publishing each query and target source.
 *
 * ```sql
 * -- Generate a small reproducible DNA dataset with seed 42, inspect its rows, then roll back the example table.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * SELECT bioseq.create_query_target_sample_table('doc_generated', 3, 512, 0.0, 4096,
 *   0.01, 0.0, 0.0, 1.0, 1.0, 0.0, 42);
 * SELECT * FROM doc_generated LIMIT 3;
 * ROLLBACK;
 * ```
 */
[[nodiscard]] Oid create_query_target_sample_table(
        std::optional<std::string> table_name,
        std::optional<std::int32_t> fragment_count,
        std::optional<std::int64_t> fragment_length_mean,
        std::optional<double> fragment_length_stddev,
        std::optional<std::int64_t> reference_length,
        std::optional<double> substitution_rate,
        std::optional<double> insertion_rate,
        std::optional<double> deletion_rate,
        std::optional<double> insertion_length_mean,
        std::optional<double> deletion_length_mean,
        std::optional<double> reverse_complement_rate,
        std::optional<std::int64_t> seed,
        [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"'dna4'"}})]]
        std::optional<std::string> alphabet,
        [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"NULL"}})]]
        std::optional<std::string> quality_column,
        [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"'phred63'"}})]]
        std::optional<std::string> quality_alphabet) {
    namespace gen_ns = bioseq::sequences::generation;

    if (!table_name.has_value()) {
        bioseq::postgres::support::raise_sql_error(
            ERRCODE_NULL_VALUE_NOT_ALLOWED,
            {
                .message =
                    "bioseq.create_query_target_sample_table requires "
                    "table_name",
            });
    }
    gen_ns::generator_params params;
    if (fragment_count.has_value()) {
        if (*fragment_count < 1) {
            bioseq::postgres::support::raise_sql_error(
                ERRCODE_INVALID_PARAMETER_VALUE,
                {
                    .message =
                        "bioseq.create_query_target_sample_table "
                        "fragment_count must be at least 1",
                });
        }
        params.fragment_count =
            static_cast<std::size_t>(*fragment_count);
    }
    if (fragment_length_mean.has_value()) {
        if (*fragment_length_mean < 0) {
            bioseq::postgres::support::raise_sql_error(
                ERRCODE_INVALID_PARAMETER_VALUE,
                {
                    .message =
                        "bioseq.create_query_target_sample_table "
                        "fragment_length_mean must not be negative",
                });
        }
        params.fragment_length_mean =
            static_cast<std::size_t>(*fragment_length_mean);
    }
    if (fragment_length_stddev.has_value()) {
        params.fragment_length_stddev = *fragment_length_stddev;
    }
    if (reference_length.has_value()) {
        if (*reference_length < 1) {
            bioseq::postgres::support::raise_sql_error(
                ERRCODE_INVALID_PARAMETER_VALUE,
                {
                    .message =
                        "bioseq.create_query_target_sample_table "
                        "reference_length must be at least 1",
                });
        }
        params.reference_length =
            static_cast<std::size_t>(*reference_length);
    }
    if (substitution_rate.has_value()) {
        params.substitution_rate = *substitution_rate;
    }
    if (insertion_rate.has_value()) {
        params.insertion_rate = *insertion_rate;
    }
    if (deletion_rate.has_value()) {
        params.deletion_rate = *deletion_rate;
    }
    if (insertion_length_mean.has_value()) {
        params.insertion_length_mean = *insertion_length_mean;
    }
    if (deletion_length_mean.has_value()) {
        params.deletion_length_mean = *deletion_length_mean;
    }
    if (reverse_complement_rate.has_value()) {
        params.reverse_complement_rate = *reverse_complement_rate;
    }
    if (seed.has_value()) {
        params.seed = static_cast<std::uint64_t>(*seed);
    }

    if (!std::isfinite(params.fragment_length_stddev) || params.fragment_length_stddev < 0.0
        || !gen_ns::validate_mutation_parameters(params)) {
        bioseq::postgres::support::raise_sql_error(ERRCODE_INVALID_PARAMETER_VALUE,
            {.message = "sample length deviations must be finite and nonnegative, and mutation rates must be finite probabilities"});
    }
    static constexpr std::array reserved_names{
        std::string_view{"id"}, std::string_view{"query"}, std::string_view{"target"},
        std::string_view{"truth_target_start"}, std::string_view{"truth_target_end"},
        std::string_view{"is_reverse"}, std::string_view{"effective_error_rate"},
        std::string_view{"original_query_length"}};
    const auto quality = resolve_sample_quality_column(quality_column, quality_alphabet, reserved_names);
    const auto quality_seed = bioseq::common::splitmix64(params.seed ^ 0x7175616c697479ULL);
    bioseq::postgres::support::invoke_pg_as_cpp([] { bioseq::postgres::scheduler::backend_sync(); });

    const std::string resolved_alphabet = alphabet.value_or(std::string{"dna4"});
    const auto selected_alphabet = supported_biological_alphabets::try_index_of_name(resolved_alphabet.c_str());
    if (!selected_alphabet) bioseq::postgres::support::raise_sql_error(ERRCODE_INVALID_PARAMETER_VALUE,
        {.message = "bioseq.create_query_target_sample_table requires a supported biological alphabet"});
    Oid created_relation = InvalidOid;
    supported_biological_alphabets::dispatch_alphabet_by_code(*selected_alphabet,
        [&created_relation, &table_name, &resolved_alphabet, &params, &reverse_complement_rate, &quality, quality_seed]<class Alphabet>() {
        using Configuration = sequence_custom_alphabet<Alphabet, std::uint64_t>;
        constexpr auto alphabet_code = supported_biological_alphabets::template index_of<Alphabet>();
        constexpr auto storage_width = sizeof(std::uint64_t);
        if constexpr (!seqan3::nucleotide_alphabet<Alphabet>) {
            if (reverse_complement_rate.value_or(0.0) != 0.0)
                bioseq::postgres::support::raise_sql_error(ERRCODE_INVALID_PARAMETER_VALUE,
                    {.message = "protein fixtures require reverse_complement_rate = 0"});
            params.reverse_complement_rate = 0.0;
        }
        sample_table_write_context context{};
        std::exception_ptr failure;
        try {
            const std::string columns = "(id BIGSERIAL PRIMARY KEY, query bioseq.seq('"
                + resolved_alphabet + "', 64), target bioseq.seq('" + resolved_alphabet
                + "', 64), truth_target_start BIGINT, truth_target_end BIGINT, is_reverse BOOLEAN, "
                  "effective_error_rate DOUBLE PRECISION, original_query_length BIGINT"
                + (quality ? quality->definition : "") + ") USING bioseq;";
            const Oid parent_relid = create_sample_relation(
                "bioseq.create_query_target_sample_table",
                *table_name,
                columns.c_str());
            open_sample_relation_for_writes(
                context, parent_relid);

            constexpr std::int16_t query_attno = 2;
            constexpr std::int16_t target_attno = 3;
            gen_ns::alphabet_sequence_generator<
                Alphabet, std::uint64_t> generator{params};
            using ReferenceTraits = typename compact_sequence_range<Configuration>::traits;
            const auto reference_units = bioseq::common::ceil_div(
                static_cast<std::uint64_t>(params.reference_length),
                static_cast<std::uint64_t>(ReferenceTraits::letters_per_unit));
            const auto reference_bytes = bioseq::common::checked_mul(
                reference_units, sizeof(typename ReferenceTraits::storage_unit));
            const auto reference_bound = reference_bytes ? bioseq::common::checked_mul(
                *reference_bytes, bioseq::parallelism::k_vector_preallocation_headroom_factor)
                : std::optional<std::uint64_t>{};
            if (!reference_bound) {
                bioseq::postgres::support::raise_sql_error(ERRCODE_PROGRAM_LIMIT_EXCEEDED,
                    {.message = "query sample reference allocation size overflows admission"});
            }
            // Declare the permit first so every shared reference is destroyed before
            // releasing its exact packed capacity, including during exception unwinding.
            auto reference_admission = bioseq::parallelism::try_reserve_budgeted_wave(
                1u, *reference_bound);
            if (!reference_admission.is_admitted) {
                bioseq::postgres::support::raise_sql_error(ERRCODE_OUT_OF_MEMORY,
                    {.message = "query sample generation could not reserve its shared reference"});
            }
            auto reference = generator.generate_reference();
            const auto reference_capacity_bytes = bioseq::common::checked_mul(
                static_cast<std::uint64_t>(reference->packed_units.capacity()),
                sizeof(typename ReferenceTraits::storage_unit));
            if (!reference_capacity_bytes
                || *reference_capacity_bytes > reference_admission.permit.bytes()) {
                bioseq::postgres::support::raise_sql_error(ERRCODE_PROGRAM_LIMIT_EXCEEDED,
                    {.message = "query sample reference allocation exceeds admitted capacity"});
            }
            reference_admission.permit.shrink_to(*reference_capacity_bytes);

            for (std::size_t first = 0;
                 first < params.fragment_count;) {
                auto requested = static_cast<unsigned>(
                    std::min<std::size_t>(4096u, params.fragment_count - first));
                bioseq::parallelism::budgeted_wave wave;
                std::vector<typename decltype(generator)::query_target_pair> batch;
                std::size_t batch_count{};
                for (;;) {
                    wave = bioseq::parallelism::try_reserve_budgeted_wave(requested,
                        estimate_query_pair_peak_bytes<Configuration>(params));
                    if (!wave.is_admitted) {
                        bioseq::postgres::support::raise_sql_error(ERRCODE_OUT_OF_MEMORY,
                            {.message = "query sample generation could not reserve its batch scratch"});
                    }
                    batch_count = wave.fanout;
                    batch = generator.generate_fragment_pairs(reference, first, batch_count);
                    if (batch.size() == batch_count) break;
                    // Failed output owners have been released; retry the same deterministic
                    // prefix with less retained output and scratch memory.
                    wave = {};
                    if (batch_count == 1u) {
                        bioseq::postgres::support::raise_sql_error(ERRCODE_OUT_OF_MEMORY,
                            {.message = "query sample generation could not admit or materialize one row"});
                    }
                    requested = static_cast<unsigned>(batch_count / 2u);
                }

                for (std::size_t local = 0;
                     local < batch.size();
                     ++local) {
                    const auto& pair = batch[local];
                    const sequence_size_t query_total_bp =
                        static_cast<sequence_size_t>(
                            pair.query.bp_size());
                    const std::uint16_t query_padding =
                        static_cast<std::uint16_t>(
                            pair.query.padding());
                    const sequence_size_t target_total_bp =
                        static_cast<sequence_size_t>(
                            pair.target->bp_size());
                    const std::uint16_t target_padding =
                        static_cast<std::uint16_t>(
                            pair.target->padding());

                    auto query_source =
                        insert_compact_range_streamed(
                            pair.query,
                            context.parent_rel,
                            context.parent_td,
                            query_attno,
                            context.main_storage_id,
                            context.cap_bytes,
                            context.caches);
                    auto target_source =
                        insert_compact_range_streamed(
                            *pair.target,
                            context.parent_rel,
                            context.parent_td,
                            target_attno,
                            context.main_storage_id,
                            context.cap_bytes,
                            context.caches);
                    std::optional<generated_sample_source> scores;
                    if (quality) {
                        const auto row_seed = bioseq::common::splitmix64(
                            quality_seed ^ static_cast<std::uint64_t>(first + local));
                        scores.emplace(prepare_random_sample_source(context, quality->alphabet_code, 9,
                            query_total_bp, row_seed, 0u));
                    }
                    const bioseq::storage::sequence_locator query_locator =
                        query_source.locator();
                    const bioseq::storage::sequence_locator target_locator =
                        target_source.locator();

                    bioseq::pg_types::seq* const query_stub =
                        bioseq::postgres::support::invoke_pg_as_cpp(
                            [&context, query_padding, query_total_bp,
                             &query_locator] {
                                return bioseq::storage::
                                    alloc_locator_stub_in(
                                        get_column_write_per_tuple_context(
                                            context.write_state),
                                        alphabet_code,
                                        storage_width,
                                        query_padding,
                                        query_total_bp,
                                        query_locator);
                            });
                    bioseq::pg_types::seq* const target_stub =
                        bioseq::postgres::support::invoke_pg_as_cpp(
                            [&context, target_padding, target_total_bp,
                             &target_locator] {
                                return bioseq::storage::
                                    alloc_locator_stub_in(
                                        get_column_write_per_tuple_context(
                                            context.write_state),
                                        alphabet_code,
                                        storage_width,
                                        target_padding,
                                        target_total_bp,
                                        target_locator);
                            });

                    bioseq::postgres::support::invoke_pg_as_cpp(
                        [&context, &pair, &scores, first, local,
                         query_stub, target_stub] {
                            bioseq::postgres::support::pg_slot_datums_view
                                slot_values{context.slot};
                            slot_values.set(
                                1,
                                static_cast<std::int64_t>(
                                    first + local + 1));
                            slot_values.set_raw(
                                2,
                                bioseq::postgres::support::pg_datum::from_pointer(
                                    query_stub));
                            slot_values.set_raw(
                                3,
                                bioseq::postgres::support::pg_datum::from_pointer(
                                    target_stub));
                            slot_values.set(
                                4,
                                static_cast<std::int64_t>(
                                    pair.truth_target_start));
                            slot_values.set(
                                5,
                                static_cast<std::int64_t>(
                                    pair.truth_target_end));
                            slot_values.set(6, pair.is_reverse);
                            slot_values.set(
                                7, pair.effective_error_rate);
                            slot_values.set(
                                8,
                                static_cast<std::int64_t>(
                                    pair.original_query_length));
                            if (scores) {
                                slot_values.set_raw(9,
                                    bioseq::postgres::support::pg_datum::from_pointer(scores->value));
                            }
                            slot_values.store_virtual();
                            (void) write_column_row(context.write_state,
                                context.slot->tts_values, context.slot->tts_isnull);
                        });
                    bioseq::postgres::support::invoke_pg_as_cpp(
                        [&query_source] {
                            query_source.publish_after_parent_row();
                        });
                    bioseq::postgres::support::invoke_pg_as_cpp(
                        [&target_source, &scores] {
                            target_source.publish_after_parent_row();
                            if (scores) scores->publication.publish_after_parent_row();
                        });
                }
                first += batch_count;
            }
        } catch (...) {
            failure = std::current_exception();
        }

        try {
            close_sample_relation_after_writes(
                context, failure == nullptr);
        } catch (...) {
            if (failure == nullptr) {
                failure = std::current_exception();
            }
        }
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
        finalize_sample_identifiers(context.parent_relid, static_cast<std::int64_t>(params.fragment_count));
        created_relation = context.parent_relid;
    });
    return created_relation;
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
})]]
/**
 * Return one row describing the sequence's stored locator and padding.
 * For an inline sequence, return its length and padding with  zero locator
 * fields. Detoast the input only for this call.
 *
 * ```sql
 * -- Inspect the physical sequence locator supplied by a Bioseq table scan.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT id, bioseq.inspect_locator(seq) FROM doc_sequences;
 * ROLLBACK;
 * ```

 * @remark website.field version: Locator payload format version; zero for an inline sequence.
 * @remark website.field bucket_idx: Storage bucket containing the sequence fragments; zero for an inline sequence.
 * @remark website.field shard_idx: Shard number within the storage bucket; zero for an inline sequence.
 * @remark website.field num_fragments: Number of stored sequence fragments; zero for an inline sequence.
 * @remark website.field total_bp_size: Logical length of the sequence in letters, excluding padding.
 * @remark website.field first_sub_row_no: Logical row number of the first sequence fragment; zero for an inline sequence.
 * @remark website.field source_epoch: Positive source-content revision used to detect stale locators; zero for an inline sequence.
 * @remark website.field total_padding: Number of padding letters stored in the sequence header.
*/
[[nodiscard]]
bioseq::postgres::support::table_rows<
    bioseq::sequences::generation::pg::result_rows::locator_metadata>
inspect_locator(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
        })]]
        Datum seq) {
    using row_type =
        bioseq::sequences::generation::pg::result_rows::locator_metadata;
    bioseq::sequences::generation::pg::detoasted_sequence input{seq};
    bioseq::pg_types::seq* const hdr = input.get();
    row_type row{};
    if (!bioseq::storage::is_locator_stub(hdr)) {
        row.total_bp_size = static_cast<std::int64_t>(hdr->bp_size);
        row.total_padding =
            static_cast<std::int32_t>(hdr->padding_letters);
        return bioseq::postgres::support::table_rows<row_type>{
            std::vector<row_type>{row}};
    }

    const auto loc = bioseq::postgres::support::invoke_pg_as_cpp([hdr] {
        return bioseq::storage::decode_locator_from_stub(hdr);
    });
    if (loc.logical_length >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        bioseq::postgres::support::raise_sql_error(
            ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
            {
                .message =
                    "bioseq.inspect_locator total_bp_size exceeds bigint",
            });
    }
    if (loc.source_epoch == 0
        || loc.source_epoch
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::int64_t>::max)())) {
        bioseq::postgres::support::raise_sql_error(
            ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
            {
                .message =
                    "bioseq.inspect_locator source_epoch is outside "
                    "positive bigint",
            });
    }
    row.version = static_cast<std::int16_t>(loc.payload_version);
    row.bucket_idx = static_cast<std::int16_t>(loc.bucket_index);
    row.shard_idx = static_cast<std::int32_t>(loc.shard_index);
    row.num_fragments = static_cast<std::int64_t>(loc.fragment_count);
    row.total_bp_size = static_cast<std::int64_t>(loc.logical_length);
    row.first_sub_row_no =
        static_cast<std::int64_t>(loc.first_fragment_row);
    row.source_epoch = static_cast<std::int64_t>(loc.source_epoch);
    row.total_padding = static_cast<std::int32_t>(hdr->padding_letters);
    return bioseq::postgres::support::table_rows<row_type>{
        std::vector<row_type>{row}};
}

}  // namespace bioseq::sequences::generation::pg::sql_routines
