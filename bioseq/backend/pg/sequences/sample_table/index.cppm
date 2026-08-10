/*-------------------------------------------------------------------------
 *
 * sequences/sample_table/index.cppm
 *
 * SQL owners for streaming synthetic Bioseq table generation and locator
 * metadata inspection.
 *
 *-------------------------------------------------------------------------
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
#include "nodes/makefuncs.h"
}

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

export module bioseq.postgres.sequences.sample_table;

import bioseq.common.logging;
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.support;
import bioseq.postgres.support.sql_boundary;
import bioseq.common.seqan_adapter;
import bioseq.parallelism.memory_admission;
import bioseq.sequences;
import bioseq.sequences.generation;
import bioseq.postgres.storage.manager;
import bioseq.storage.size;
import bioseq.storage.layout;
import bioseq.postgres.table.io;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.physical;
import bioseq.postgres.types.compact_sequence;
import :sql_rows;

using bioseq::storage::sequence_size_t;

namespace bioseq::sequences::generation::pg {

// dna4: 2 bits per letter, packed LSB-first per storage unit.
constexpr std::uint8_t  kDna4AlphabetCode = 0;
constexpr std::uint8_t  kDna4StorageWidth = 8;   // uint64
constexpr std::size_t   kDna4LettersPerUnit = 32;
constexpr std::size_t   kFragmentStorageUnits = 1024;
constexpr std::size_t   kFragmentBp =
    kFragmentStorageUnits * kDna4LettersPerUnit;  // 32 768

class detoasted_sequence final {
public:
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

    ~detoasted_sequence() noexcept {
        if (owns_value_ && value_ != nullptr) {
            pfree(value_);
        }
    }

    [[nodiscard]] bioseq::pg_types::seq* get() const noexcept {
        return value_;
    }

private:
    Datum source_;
    bioseq::pg_types::seq* value_;
    bool owns_value_;
};

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
                context.parent_td, &TTSOpsVirtual);
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

void close_sample_relation_after_writes(
        sample_table_write_context& context,
        bool commit) {
    std::exception_ptr failure;
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
    if (context.caches != nullptr) {
        attempt([&context, commit] {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [&context, commit] {
                    context.caches->close_all(commit);
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

template <bioseq::sequences::generation::packed_sequence_config R>
[[nodiscard]] bioseq::storage::pending_sequence_source insert_compact_range_streamed(
    const compact_sequence_range<R>& seq,
    Relation parent_rel,
    TupleDesc parent_td,
    std::int16_t attno,
    std::uint64_t main_storage_id,
    sequence_size_t cap_bytes,
    bioseq::storage::seq_caches* caches) {
    using traits = typename compact_sequence_range<R>::traits;
    using unit_t = typename traits::storage_unit;
    static_assert(sizeof(unit_t) == kDna4StorageWidth,
                  "create_query_target_sample_table expects dna4/u64 storage");
    static_assert(traits::letters_per_unit == kDna4LettersPerUnit,
                  "create_query_target_sample_table expects dna4/u64 packing");

    const sequence_size_t total_bp = static_cast<sequence_size_t>(seq.bp_size());
    const std::uint16_t padding = static_cast<std::uint16_t>(seq.padding());
    auto producer = [&seq, total_bp, padding](
        std::size_t frag_no,
        bioseq::pg_types::seq* dst,
        MemoryContext /*scratch*/,
        bool is_last) noexcept {
        const sequence_size_t frag_global_bp_start =
            static_cast<sequence_size_t>(frag_no) *
            static_cast<sequence_size_t>(kFragmentBp);
        const sequence_size_t this_fragment_bp = is_last
            ? (total_bp - frag_global_bp_start)
            : static_cast<sequence_size_t>(kFragmentBp);
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
            units * sizeof(unit_t));
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
                    kDna4AlphabetCode,
                    kDna4StorageWidth,
                    total_bp,
                    padding,
                    cap_bytes,
                    caches,
                    producer);
        });
}

[[nodiscard]] std::uint64_t estimate_query_pair_peak_bytes(
    const bioseq::sequences::generation::generator_params& params) noexcept {
    const std::uint64_t mean = static_cast<std::uint64_t>(
        std::max<std::size_t>(1, params.fragment_length_mean));
    const std::uint64_t query_units =
        (mean + kDna4LettersPerUnit - 1) / kDna4LettersPerUnit;
    const std::uint64_t query_bytes =
        bioseq::parallelism::saturating_mul(query_units, sizeof(std::uint64_t));
    const std::uint64_t rank_scratch =
        bioseq::parallelism::saturating_mul(mean, 2u);
    return bioseq::parallelism::saturating_add(query_bytes, rank_scratch);
}

}  // namespace bioseq::sequences::generation::pg

export namespace bioseq::sequences::generation::pg::sql_routines {

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
/**
 * @brief Creates a Bioseq table containing streamed synthetic dna4 sequences.
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
        std::optional<std::int64_t> seed) {
    using namespace bioseq::sequences::generation::pg;

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
    if (resolved_alphabet != "dna4") {
        bioseq::postgres::support::raise_sql_error(
            ERRCODE_INVALID_PARAMETER_VALUE,
            {
                .message =
                    "bioseq.create_sample_table supports only the dna4 "
                    "alphabet",
            });
    }
    const std::uint64_t seed_value = static_cast<std::uint64_t>(
        seed.value_or(std::int64_t{42}));

    sample_table_write_context context{};
    std::exception_ptr failure;
    try {
        const Oid parent_relid = create_sample_relation(
            "bioseq.create_sample_table",
            *table_name,
            "(id BIGSERIAL PRIMARY KEY, "
            "seq bioseq.seq('dna4', 32)) USING bioseq;");
        open_sample_relation_for_writes(
            context, parent_relid);

        constexpr std::int16_t seq_attno = 2;
        const sequence_size_t total_bp =
            static_cast<sequence_size_t>(sequence_bp);
        const std::uint16_t last_fragment_padding =
            static_cast<std::uint16_t>(
                sequence_bp == 0
                    ? 0
                    : ((total_bp + kDna4LettersPerUnit - 1)
                        / kDna4LettersPerUnit)
                        * kDna4LettersPerUnit - total_bp);
        std::mt19937_64 rng{seed_value};

        for (std::int32_t seq_no = 0;
             seq_no < sequence_count;
             ++seq_no) {
            auto producer =
                [&rng, total_bp, last_fragment_padding](
                    std::size_t frag_no,
                    bioseq::pg_types::seq* dst,
                    MemoryContext /*scratch*/,
                    bool is_last) noexcept {
                    const sequence_size_t frag_global_bp_start =
                        static_cast<sequence_size_t>(frag_no)
                        * static_cast<sequence_size_t>(kFragmentBp);
                    const sequence_size_t this_fragment_bp =
                        is_last
                            ? (total_bp - frag_global_bp_start)
                            : static_cast<sequence_size_t>(
                                kFragmentBp);
                    dst->bp_size =
                        static_cast<
                            bioseq::pg_types::seq::bp_size_t>(
                                this_fragment_bp);
                    dst->padding_letters =
                        is_last
                            ? last_fragment_padding
                            : std::uint16_t{0};
                    auto* units =
                        reinterpret_cast<std::uint64_t*>(
                            dst->data);
                    sequence_size_t bp_emitted = 0;
                    std::size_t unit_idx = 0;
                    while (bp_emitted < this_fragment_bp) {
                        const sequence_size_t letters_this_unit =
                            std::min<sequence_size_t>(
                                static_cast<sequence_size_t>(
                                    kDna4LettersPerUnit),
                                this_fragment_bp - bp_emitted);
                        std::uint64_t packed = 0;
                        const std::uint64_t random_bits = rng();
                        for (sequence_size_t i = 0;
                             i < letters_this_unit;
                             ++i) {
                            const std::uint64_t letter =
                                (random_bits >> (i * 2)) & 0x3ULL;
                            packed |= letter << (i * 2);
                        }
                        units[unit_idx++] = packed;
                        bp_emitted += letters_this_unit;
                    }
                };

            auto pending_source =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [&context, total_bp,
                     last_fragment_padding, &producer] {
                        return bioseq::storage::
                            bioseq_default_storage_manager::
                                insert_sequence_streamed(
                                    context.parent_relid,
                                    context.main_storage_id,
                                    context.parent_rel->rd_locator.dbOid,
                                    context.parent_rel->rd_locator.spcOid,
                                    context.parent_rel->rd_locator.relNumber,
                                    seq_attno,
                                    context.parent_td,
                                    kDna4AlphabetCode,
                                    kDna4StorageWidth,
                                    total_bp,
                                    last_fragment_padding,
                                    context.cap_bytes,
                                    context.caches,
                                    producer);
                    });
            const bioseq::storage::sequence_locator locator =
                pending_source.locator();
            bioseq::pg_types::seq* const stub =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [&context, total_bp,
                     last_fragment_padding, &locator] {
                        return bioseq::storage::
                            alloc_locator_stub_in(
                                get_column_write_per_tuple_context(
                                    context.write_state),
                                kDna4AlphabetCode,
                                kDna4StorageWidth,
                                last_fragment_padding,
                                total_bp,
                                locator);
                    });

            bioseq::postgres::support::invoke_pg_as_cpp(
                [&context, stub, seq_no] {
                    bioseq::postgres::support::pg_slot_datums_view
                        slot_values{context.slot};
                    slot_values.set(
                        1,
                        static_cast<std::int64_t>(seq_no + 1));
                    slot_values.set_raw(
                        2,
                        bioseq::postgres::support::pg_datum::from_pointer(
                            stub));
                    slot_values.store_virtual();
                    Datum write_values[2] = {
                        context.slot->tts_values[0],
                        context.slot->tts_values[1],
                    };
                    bool write_nulls[2] = {false, false};
                    (void) write_column_row(
                        context.write_state,
                        write_values,
                        write_nulls);
                });
            bioseq::postgres::support::invoke_pg_as_cpp(
                [&pending_source] {
                    pending_source.publish_after_parent_row();
                });
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
    return context.parent_relid;
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
/**
 * @brief Creates synthetic query-target pairs with ground-truth coordinates.
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
        std::optional<std::int64_t> seed) {
    using namespace bioseq::sequences::generation::pg;
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

    sample_table_write_context context{};
    std::exception_ptr failure;
    try {
        const Oid parent_relid = create_sample_relation(
            "bioseq.create_query_target_sample_table",
            *table_name,
            "(id BIGSERIAL PRIMARY KEY, "
            "query bioseq.seq('dna4', 64), "
            "target bioseq.seq('dna4', 64), "
            "truth_target_start BIGINT, "
            "truth_target_end BIGINT, "
            "is_reverse BOOLEAN, "
            "effective_error_rate DOUBLE PRECISION, "
            "original_query_length BIGINT) USING bioseq;");
        open_sample_relation_for_writes(
            context, parent_relid);

        constexpr std::int16_t query_attno = 2;
        constexpr std::int16_t target_attno = 3;
        gen_ns::alphabet_sequence_generator<
            seq::dna4, std::uint64_t> generator{params};
        auto reference = generator.generate_reference();

        for (std::size_t first = 0;
             first < params.fragment_count;) {
            auto wave =
                bioseq::parallelism::try_reserve_budgeted_wave(
                    4096u,
                    estimate_query_pair_peak_bytes(params));
            const std::size_t batch_count =
                std::min<std::size_t>(
                    wave.admitted ? wave.fanout : 1u,
                    params.fragment_count - first);
            auto batch = generator.generate_fragment_pairs(
                reference, first, batch_count);

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
                                    kDna4AlphabetCode,
                                    kDna4StorageWidth,
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
                                    kDna4AlphabetCode,
                                    kDna4StorageWidth,
                                    target_padding,
                                    target_total_bp,
                                    target_locator);
                        });

                bioseq::postgres::support::invoke_pg_as_cpp(
                    [&context, &pair, first, local,
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
                        slot_values.store_virtual();

                        Datum write_values[8];
                        bool write_nulls[8];
                        for (int index = 0; index < 8; ++index) {
                            write_values[index] =
                                context.slot->tts_values[index];
                            write_nulls[index] = false;
                        }
                        (void) write_column_row(
                            context.write_state,
                            write_values,
                            write_nulls);
                    });
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [&query_source] {
                        query_source.publish_after_parent_row();
                    });
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [&target_source] {
                        target_source.publish_after_parent_row();
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
    return context.parent_relid;
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
})]]
/**
 * @brief Decodes locator version, shard, fragment range, source epoch, and
 * padding fields from a sequence value.
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
