/*-------------------------------------------------------------------------
 *
 * sequences/generation/sample_table_udf.cppm
 *
 * DOC0264 §10.2 — `bioseq.create_sample_table` SQL UDF.
 *
 * Creates a `USING bioseq` table with `n_sequences` synthetic dna4
 * sequences of `sequence_bp_size` letters each, streaming fragments
 * through Phase C's `insert_sequence_streamed` so the peak RSS during
 * insert is bounded by one fragment's worth of state (~8 KiB), not by
 * sequence_bp_size. Only `dna4` alphabet is shipped in Phase C — other
 * alphabets are rejected (DOC0267 D-EXTENSIBILITY).
 *
 *-------------------------------------------------------------------------
 */
module;

#include <hwy/highway.h>

#include "common.h"

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
}

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <string>

export module bioseq_sequences.sample_table_udf;

import mlogging;
import pg_utils;
import sequan_compat;
import bioseq_sequences;
import bioseq_sequences.generation;
import bioseq_storage;
import bioseq_storage_size;
import bioseq_table_io;
import storage_metadata;                  // DOC0322 §7 — bioseq_init_write_state
import bioseq_types.pg_compact_sequence;

using bioseq::storage::sequence_size_t;

namespace bioseq::sequences::generation::pg {

// dna4: 2 bits per letter, packed LSB-first per storage unit.
constexpr std::uint8_t  kDna4AlphabetCode = 0;
constexpr std::uint8_t  kDna4StorageWidth = 8;   // uint64
constexpr std::size_t   kDna4LettersPerUnit = 32;
constexpr std::size_t   kFragmentStorageUnits = 1024;
constexpr std::size_t   kFragmentBp =
    kFragmentStorageUnits * kDna4LettersPerUnit;  // 32 768

}  // namespace bioseq::sequences::generation::pg

extern "C" {

CXX_PG_FUNCTION_INFO_V1(bioseq_create_sample_table);
Datum bioseq_create_sample_table(PG_FUNCTION_ARGS)
{
    using namespace bioseq::sequences::generation::pg;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2)) {
        bioseq_log<ERR, PG>( "bioseq.create_sample_table: table_name, n_sequences, "
               "sequence_bp_size are required");
    }
    const char* const table_name_c = TextDatumGetCString(PG_GETARG_DATUM(0));
    const std::int32_t n_sequences = PG_GETARG_INT32(1);
    if (n_sequences < 1) {
        bioseq_log<ERR, PG>( "bioseq.create_sample_table: n_sequences must be >= 1");
    }
    if (n_sequences > 100'000'000) {
        bioseq_log<ERR, PG>( "bioseq.create_sample_table: n_sequences {} exceeds the "
               "safety cap (100M)", n_sequences);
    }
    const std::int64_t sequence_bp = PG_GETARG_INT64(2);
    if (sequence_bp < 0) {
        bioseq_log<ERR, PG>( "bioseq.create_sample_table: sequence_bp_size must be >= 0");
    }
    std::string alphabet = "dna4";
    if (!PG_ARGISNULL(3)) {
        char* alpha_c = TextDatumGetCString(PG_GETARG_DATUM(3));
        alphabet.assign(alpha_c);
    }
    if (alphabet != "dna4") {
        bioseq_log<ERR, PG>(
            "bioseq.create_sample_table: alphabet '{}' is not supported in Phase C. "
            "Only 'dna4' is shipped. See DOC0267 D-EXTENSIBILITY for the path "
            "to dna5 / aa20 / aa27 / dna_iupac.", alphabet);
    }
    const std::uint64_t seed = PG_ARGISNULL(4)
        ? std::uint64_t{42}
        : static_cast<std::uint64_t>(PG_GETARG_INT64(4));

    // (1) CREATE TABLE foo (id BIGSERIAL PRIMARY KEY, seq bioseq.seq('dna4', 32)) USING bioseq;
    StringInfo ddl = makeStringInfo();
    appendStringInfo(ddl,
        "CREATE TABLE %s (id BIGSERIAL PRIMARY KEY, "
        "seq bioseq.seq('dna4', 32)) USING bioseq;",
        quote_identifier(table_name_c));
    if (SPI_connect() != SPI_OK_CONNECT) {
        bioseq_log<ERR, PG>( "bioseq.create_sample_table: SPI_connect failed");
    }
    const int rc = SPI_execute(ddl->data, /*read_only=*/false, /*tcount=*/0);
    if (rc != SPI_OK_UTILITY) {
        SPI_finish();
        bioseq_log<ERR, PG>( "bioseq.create_sample_table: CREATE TABLE failed for {} (SPI rc={})",
               table_name_c, rc);
    }
    SPI_finish();

    const Oid public_ns = get_namespace_oid("public", false);
    const Oid parent_relid = get_relname_relid(table_name_c, public_ns);
    if (!OidIsValid(parent_relid)) {
        bioseq_log<ERR, PG>( "bioseq.create_sample_table: failed to locate created table {}",
               table_name_c);
    }
    // DOC0264 §10.2 — pin parent_rel under TopTransactionResourceOwner so it
    // survives the internal sub-transactions that `insert_sequence_streamed`
    // creates inside `allocate_new_shard_and_open` (SPI CREATE TABLE for
    // sub-tables). Otherwise table_close fails with "relcache reference not
    // owned by resource owner Portal" — mirrors the Phase A pattern in
    // sequence_storage_manager.cppm `open_relation_cached`.
    Relation parent_rel = nullptr;
    {
        ResourceOwner saved_owner = CurrentResourceOwner;
        CurrentResourceOwner       = TopTransactionResourceOwner;
        parent_rel = table_open(parent_relid, RowExclusiveLock);
        CurrentResourceOwner       = saved_owner;
    }
    TupleDesc parent_td = RelationGetDescr(parent_rel);

    // (2) Build a TupleTableSlot with TTSOpsVirtual.
    TupleTableSlot* slot = MakeTupleTableSlot(parent_td, &TTSOpsVirtual);

    // (3) Write-state cap snapshot — one per UDF call.
    BioseqColumnWriteState* write_state = bioseq_init_write_state(
        parent_rel, parent_td, parent_relid, GetCurrentSubTransactionId());
    if (write_state->seq_caches == nullptr) {
        auto* fresh = bioseq::storage::alloc_seq_caches(write_state->stripeWriteContext);
        fresh->side                = bioseq::storage::seq_caches::Side::Write;
        fresh->lock_mode           = RowExclusiveLock;
        fresh->main_database_oid   = parent_rel->rd_locator.dbOid;
        fresh->main_tablespace_oid = parent_rel->rd_locator.spcOid;
        fresh->main_relfilenode    = parent_rel->rd_locator.relNumber;
        fresh->cap_bytes_snapshot  = bioseq::storage::read_max_sub_table_cap_bytes();
        write_state->seq_caches    = fresh;
    }
    auto* caches = static_cast<bioseq::storage::seq_caches*>(write_state->seq_caches);
    const auto cap_bytes = caches->cap_bytes_snapshot;

    // (4) Per-sequence: stream fragments, build stub, parent-row insert.
    constexpr std::int16_t seq_attno = 2;  // 1-based: id is 1, seq is 2
    const sequence_size_t total_bp = static_cast<sequence_size_t>(sequence_bp);
    const std::uint16_t last_fragment_padding = static_cast<std::uint16_t>(
        sequence_bp == 0
            ? 0
            : ((total_bp + kDna4LettersPerUnit - 1) / kDna4LettersPerUnit) *
                kDna4LettersPerUnit - total_bp);

    // Stream-stateful RNG (one for the whole UDF; consolidated-review I39).
    std::mt19937_64 rng{seed};

    for (std::int32_t seq_no = 0; seq_no < n_sequences; ++seq_no) {
        auto producer = [&rng, total_bp, last_fragment_padding](
            std::size_t frag_no, pgseq_t* dst, MemoryContext /*scratch*/,
            bool is_last) noexcept
        {
            const sequence_size_t frag_global_bp_start =
                static_cast<sequence_size_t>(frag_no) *
                static_cast<sequence_size_t>(kFragmentBp);
            const sequence_size_t this_fragment_bp = is_last
                ? (total_bp - frag_global_bp_start)
                : static_cast<sequence_size_t>(kFragmentBp);
            dst->bp_size         = static_cast<pgseq_t::bp_size_t>(this_fragment_bp);
            dst->padding_letters = is_last ? last_fragment_padding : std::uint16_t{0};
            std::uint64_t* units = reinterpret_cast<std::uint64_t*>(dst->data);
            // 2-bit dna4 packing, 32 letters per uint64. Use the upper 64
            // bits of a single rng() call per unit — std::mt19937_64 emits
            // 64 random bits per call, which is exactly 32 dna4 letters
            // already in random order. No re-rolling per letter.
            sequence_size_t bp_emitted = 0;
            std::size_t unit_idx = 0;
            while (bp_emitted < this_fragment_bp) {
                const sequence_size_t letters_this_unit =
                    std::min<sequence_size_t>(
                        static_cast<sequence_size_t>(kDna4LettersPerUnit),
                        this_fragment_bp - bp_emitted);
                std::uint64_t packed = 0;
                const std::uint64_t r = rng();
                for (sequence_size_t i = 0; i < letters_this_unit; ++i) {
                    const std::uint64_t letter_2bit = (r >> (i * 2)) & 0x3ULL;
                    packed |= letter_2bit << (i * 2);
                }
                units[unit_idx] = packed;
                ++unit_idx;
                bp_emitted += letters_this_unit;
            }
        };

        const bioseq::storage::seq_locator locator =
            bioseq::storage::bioseq_default_storage_manager::insert_sequence_streamed(
                parent_rel->rd_locator.dbOid,
                parent_rel->rd_locator.spcOid,
                parent_rel->rd_locator.relNumber,
                seq_attno, parent_td,
                kDna4AlphabetCode, kDna4StorageWidth,
                total_bp,
                last_fragment_padding,
                cap_bytes,
                caches,
                producer);

        // Build the locator-stub varlena in the write-state's per-tuple ctx,
        // then put it into slot and write the parent row directly via the
        // columnar writer (bypassing TAM rewrite which would re-detect the
        // stub and skip, and also bypassing sidecar — sidecar minimizer
        // index for synthetic test data is recomputed at mmsearch time).
        pgseq_t* stub = bioseq::storage::alloc_locator_stub_in(
            BioseqColumnWritePerTupleContext(write_state),
            kDna4AlphabetCode, kDna4StorageWidth,
            last_fragment_padding, total_bp, locator);

        slot->tts_values[0] = Int64GetDatum(static_cast<std::int64_t>(seq_no + 1));
        slot->tts_values[1] = PointerGetDatum(stub);
        slot->tts_isnull[0] = false;
        slot->tts_isnull[1] = false;
        ExecStoreVirtualTuple(slot);

        Datum  write_values[2] = { slot->tts_values[0], slot->tts_values[1] };
        bool   write_nulls[2]  = { false, false };
        (void) BioseqColumnWriteRow(write_state, write_values, write_nulls);
    }

    // (5) Flush write states + close the sub-table seq_caches' Relations +
    // close parent_rel under the same ResourceOwner everything was opened
    // with (TopTransactionResourceOwner) — DOC0264 §10.2 / Phase A
    // §7.9 / open_relation_cached's Write-side pattern. Otherwise PG
    // raises "relcache reference not owned by resource owner Portal" at
    // close time.
    //
    // We deliberately do NOT run ANALYZE here. The columnar AM buffers
    // pending row writes in the active stripe until the surrounding xact
    // commits (or the stripe fills). Running ANALYZE in the same xact
    // reads through the bioseq custom-scan path which hydrates each row
    // via `fetch_payload` → which can't see the buffered fragment rows
    // (they're not yet visible to the sub-table read state). Operators who
    // need fresh planner stats run `ANALYZE table_name;` in a separate
    // session after the UDF call returns.
    ExecDropSingleTupleTableSlot(slot);
    {
        ResourceOwner saved_owner = CurrentResourceOwner;
        CurrentResourceOwner       = TopTransactionResourceOwner;
        caches->close_all(/*commit=*/true);
        table_close(parent_rel, NoLock);
        CurrentResourceOwner       = saved_owner;
    }

    PG_RETURN_OID(parent_relid);
}

// DOC0274 — `bioseq.create_query_target_sample_table(...)` UDF.
//
// Inlines the same simulation model `generate_test_sequences` uses (uniform
// substitutions, normally-distributed indel lengths, optional reverse-
// complement flipping) but emits one pair at a time and streams it straight
// to the bioseq sub-table via `insert_sequence_streamed`. Peak RSS is
// `reference_length` bytes (the shared reference vector) + one mutated-
// fragment vector — independent of `fragment_count`. This keeps the 5 Gbp
// `OUR_TEST_BASED_ON_XXL_HIFI` config viable in a single backend process.
//
// Schema:
//   id                     BIGSERIAL PRIMARY KEY
//   query                  bioseq.seq('dna4', 64)   -- uint64 storage; matches the
//   target                 bioseq.seq('dna4', 64)   -- UDF's uint64 packing exactly
//   truth_target_start     BIGINT
//   truth_target_end       BIGINT
//   is_reverse             BOOLEAN
//   effective_error_rate   DOUBLE PRECISION
//   original_query_length  BIGINT
//
// `query` and `target` are inserted via `insert_sequence_streamed` so the
// per-fragment sub-table rows are written without ever materialising the
// full payload in RAM (target sizes typically exceed MaxAllocSize for this
// helper's intended use cases — e.g. 5 Gbp HiFi references).
CXX_PG_FUNCTION_INFO_V1(bioseq_create_query_target_sample_table);
Datum bioseq_create_query_target_sample_table(PG_FUNCTION_ARGS)
{
    using namespace bioseq::sequences::generation::pg;
    namespace gen_ns = bioseq::sequences::generation::HWY_NAMESPACE;

    if (PG_ARGISNULL(0)) {
        bioseq_log<ERR, PG>( "bioseq.create_query_target_sample_table: table_name required");
    }
    const char* const table_name_c = TextDatumGetCString(PG_GETARG_DATUM(0));

    // GeneratorParams: positional args mirror the JSON schema in
    // bench_v10_configuration.json's data block. Defaults match the
    // generator's struct.
    gen_ns::GeneratorParams params;
    params.fragment_count       = PG_ARGISNULL(1)
        ? params.fragment_count       : static_cast<std::size_t>(PG_GETARG_INT32(1));
    params.fragment_length_mean = PG_ARGISNULL(2)
        ? params.fragment_length_mean : static_cast<std::size_t>(PG_GETARG_INT64(2));
    params.fragment_length_stddev = PG_ARGISNULL(3)
        ? params.fragment_length_stddev : PG_GETARG_FLOAT8(3);
    params.reference_length     = PG_ARGISNULL(4)
        ? params.reference_length     : static_cast<std::size_t>(PG_GETARG_INT64(4));
    params.substitution_rate    = PG_ARGISNULL(5)
        ? params.substitution_rate    : PG_GETARG_FLOAT8(5);
    params.insertion_rate       = PG_ARGISNULL(6)
        ? params.insertion_rate       : PG_GETARG_FLOAT8(6);
    params.deletion_rate        = PG_ARGISNULL(7)
        ? params.deletion_rate        : PG_GETARG_FLOAT8(7);
    params.insertion_length_mean = PG_ARGISNULL(8)
        ? params.insertion_length_mean : PG_GETARG_FLOAT8(8);
    params.deletion_length_mean  = PG_ARGISNULL(9)
        ? params.deletion_length_mean  : PG_GETARG_FLOAT8(9);
    params.reverse_complement_rate = PG_ARGISNULL(10)
        ? params.reverse_complement_rate : PG_GETARG_FLOAT8(10);
    params.seed = PG_ARGISNULL(11)
        ? params.seed : static_cast<std::uint64_t>(PG_GETARG_INT64(11));

    if (params.fragment_count == 0) {
        bioseq_log<ERR, PG>( "bioseq.create_query_target_sample_table: fragment_count must be >= 1");
    }
    if (params.reference_length == 0) {
        bioseq_log<ERR, PG>( "bioseq.create_query_target_sample_table: reference_length must be >= 1");
    }

    // (1) CREATE TABLE. The seq columns use typmod ('dna4', 64) so the
    // storage_unit_byte_width (encoded as width_bits / 8 = 64/8 = 8) matches
    // the uint64 packing this UDF emits via insert_sequence_streamed —
    // without that match the read path's compact_sequence_range<dna4,uint32>
    // would re-interpret the bytes incorrectly (treating each pair of uint32
    // halves of a uint64 as separate units), corrupting every byte past the
    // first 16 letters of every fragment.
    StringInfo ddl = makeStringInfo();
    appendStringInfo(ddl,
        "CREATE TABLE %s ("
        "id BIGSERIAL PRIMARY KEY, "
        "query bioseq.seq('dna4', 64), "
        "target bioseq.seq('dna4', 64), "
        "truth_target_start BIGINT, "
        "truth_target_end BIGINT, "
        "is_reverse BOOLEAN, "
        "effective_error_rate DOUBLE PRECISION, "
        "original_query_length BIGINT"
        ") USING bioseq;",
        quote_identifier(table_name_c));
    if (SPI_connect() != SPI_OK_CONNECT) {
        bioseq_log<ERR, PG>( "bioseq.create_query_target_sample_table: SPI_connect failed");
    }
    const int rc = SPI_execute(ddl->data, /*read_only=*/false, /*tcount=*/0);
    if (rc != SPI_OK_UTILITY) {
        SPI_finish();
        bioseq_log<ERR, PG>( "bioseq.create_query_target_sample_table: CREATE TABLE failed "
               "for {} (SPI rc={})", table_name_c, rc);
    }
    SPI_finish();

    const Oid public_ns = get_namespace_oid("public", false);
    const Oid parent_relid = get_relname_relid(table_name_c, public_ns);
    if (!OidIsValid(parent_relid)) {
        bioseq_log<ERR, PG>( "bioseq.create_query_target_sample_table: failed to locate "
               "created table {}", table_name_c);
    }
    Relation parent_rel = nullptr;
    {
        ResourceOwner saved_owner = CurrentResourceOwner;
        CurrentResourceOwner       = TopTransactionResourceOwner;
        parent_rel = table_open(parent_relid, RowExclusiveLock);
        CurrentResourceOwner       = saved_owner;
    }
    TupleDesc parent_td = RelationGetDescr(parent_rel);
    TupleTableSlot* slot = MakeTupleTableSlot(parent_td, &TTSOpsVirtual);

    BioseqColumnWriteState* write_state = bioseq_init_write_state(
        parent_rel, parent_td, parent_relid, GetCurrentSubTransactionId());
    if (write_state->seq_caches == nullptr) {
        auto* fresh = bioseq::storage::alloc_seq_caches(write_state->stripeWriteContext);
        fresh->side                = bioseq::storage::seq_caches::Side::Write;
        fresh->lock_mode           = RowExclusiveLock;
        fresh->main_database_oid   = parent_rel->rd_locator.dbOid;
        fresh->main_tablespace_oid = parent_rel->rd_locator.spcOid;
        fresh->main_relfilenode    = parent_rel->rd_locator.relNumber;
        fresh->cap_bytes_snapshot  = bioseq::storage::read_max_sub_table_cap_bytes();
        write_state->seq_caches    = fresh;
    }
    auto* caches = static_cast<bioseq::storage::seq_caches*>(write_state->seq_caches);
    const auto cap_bytes = caches->cap_bytes_snapshot;

    constexpr std::int16_t query_attno  = 2;
    constexpr std::int16_t target_attno = 3;

    // Pack `count` dna4 letters from `src` into LSB-first 2-bit packed
    // uint64_t units at `dst`. Mirrors the bench_alphabet_config layout but
    // at 32 letters per uint64 (dna4 + uint64 storage). Uses `const auto*` so
    // the seq::dna4 type doesn't have to be named at the extern "C" call site
    // — clang's C++26 module visibility rules complain about a named seq::
    // type in a lambda parameter even with `import sequan_compat;` present.
    auto pack_letters_to_units =
        [](const auto* src, std::size_t count, std::uint64_t* dst) noexcept {
            std::size_t bp = 0;
            std::size_t unit_idx = 0;
            while (bp < count) {
                const std::size_t letters_this_unit =
                    std::min<std::size_t>(kDna4LettersPerUnit, count - bp);
                std::uint64_t packed = 0;
                for (std::size_t i = 0; i < letters_this_unit; ++i) {
                    const std::uint64_t letter_2bit =
                        static_cast<std::uint64_t>(src[bp + i].to_rank()) & 0x3ULL;
                    packed |= letter_2bit << (i * 2);
                }
                dst[unit_idx] = packed;
                ++unit_idx;
                bp += letters_this_unit;
            }
        };

    // (2) Build the shared random reference once. Stays alive for the whole
    // UDF call — every pair's target is the SAME reference (just stored once
    // per row in the sub-table since Phase C has no cross-row dedup). Uses
    // `auto` so the seq::dna4 element type doesn't have to be named at the
    // extern "C" call site (module-visibility avoidance — see comment above).
    std::mt19937_64 rng{params.seed};
    auto reference = gen_ns::detail::build_random_reference<seq::dna4>(
        params.reference_length, rng);

    // Pre-compute target storage metadata — every row's target is the same
    // reference, so the padding/total_bp/num_fragments are identical too.
    const sequence_size_t target_total_bp =
        static_cast<sequence_size_t>(reference.size());
    const std::uint16_t target_padding = static_cast<std::uint16_t>(
        target_total_bp == 0
            ? 0
            : (((target_total_bp + kDna4LettersPerUnit - 1) / kDna4LettersPerUnit)
                   * kDna4LettersPerUnit - target_total_bp));

    std::uniform_real_distribution<double> probability(0.0, 1.0);

    for (std::size_t pair_no = 0; pair_no < params.fragment_count; ++pair_no) {
        const std::size_t fragment_length = gen_ns::detail::sample_fragment_length(
            params.fragment_length_mean, params.fragment_length_stddev,
            params.reference_length, rng);
        std::uniform_int_distribution<std::size_t> start_dist(
            0, params.reference_length - fragment_length);
        const std::size_t fragment_start = start_dist(rng);
        const std::size_t fragment_end = fragment_start + fragment_length;
        decltype(reference) mutated_fragment(
            reference.begin() + static_cast<std::ptrdiff_t>(fragment_start),
            reference.begin() + static_cast<std::ptrdiff_t>(fragment_end));
        const bool is_reverse = probability(rng) < params.reverse_complement_rate;
        if (is_reverse) {
            gen_ns::detail::reverse_complement_in_place(mutated_fragment);
        }
        gen_ns::detail::apply_uniform_substitutions(
            mutated_fragment, params.substitution_rate, rng);
        mutated_fragment = gen_ns::detail::apply_indels(
            mutated_fragment,
            params.insertion_rate, params.insertion_length_mean,
            params.insertion_length_stddev,
            params.deletion_rate, params.deletion_length_mean,
            params.deletion_length_stddev,
            rng);

        const sequence_size_t query_total_bp =
            static_cast<sequence_size_t>(mutated_fragment.size());
        const std::uint16_t query_padding = static_cast<std::uint16_t>(
            query_total_bp == 0
                ? 0
                : (((query_total_bp + kDna4LettersPerUnit - 1) / kDna4LettersPerUnit)
                       * kDna4LettersPerUnit - query_total_bp));

        // (3) Stream the query column.
        auto query_producer = [&mutated_fragment, query_total_bp, query_padding,
                               &pack_letters_to_units](
            std::size_t frag_no, pgseq_t* dst, MemoryContext /*scratch*/, bool is_last) noexcept
        {
            const sequence_size_t frag_global_bp_start =
                static_cast<sequence_size_t>(frag_no)
                * static_cast<sequence_size_t>(kFragmentBp);
            const sequence_size_t this_fragment_bp = is_last
                ? (query_total_bp - frag_global_bp_start)
                : static_cast<sequence_size_t>(kFragmentBp);
            dst->bp_size         = static_cast<pgseq_t::bp_size_t>(this_fragment_bp);
            dst->padding_letters = is_last ? query_padding : std::uint16_t{0};
            pack_letters_to_units(
                mutated_fragment.data() + frag_global_bp_start,
                this_fragment_bp,
                reinterpret_cast<std::uint64_t*>(dst->data));
        };
        const bioseq::storage::seq_locator query_loc =
            bioseq::storage::bioseq_default_storage_manager::insert_sequence_streamed(
                parent_rel->rd_locator.dbOid,
                parent_rel->rd_locator.spcOid,
                parent_rel->rd_locator.relNumber,
                query_attno, parent_td,
                kDna4AlphabetCode, kDna4StorageWidth,
                query_total_bp, query_padding, cap_bytes,
                caches, query_producer);

        // (4) Stream the target column — same reference for every pair.
        auto target_producer = [&reference, target_total_bp, target_padding,
                                &pack_letters_to_units](
            std::size_t frag_no, pgseq_t* dst, MemoryContext /*scratch*/, bool is_last) noexcept
        {
            const sequence_size_t frag_global_bp_start =
                static_cast<sequence_size_t>(frag_no)
                * static_cast<sequence_size_t>(kFragmentBp);
            const sequence_size_t this_fragment_bp = is_last
                ? (target_total_bp - frag_global_bp_start)
                : static_cast<sequence_size_t>(kFragmentBp);
            dst->bp_size         = static_cast<pgseq_t::bp_size_t>(this_fragment_bp);
            dst->padding_letters = is_last ? target_padding : std::uint16_t{0};
            pack_letters_to_units(
                reference.data() + frag_global_bp_start,
                this_fragment_bp,
                reinterpret_cast<std::uint64_t*>(dst->data));
        };
        const bioseq::storage::seq_locator target_loc =
            bioseq::storage::bioseq_default_storage_manager::insert_sequence_streamed(
                parent_rel->rd_locator.dbOid,
                parent_rel->rd_locator.spcOid,
                parent_rel->rd_locator.relNumber,
                target_attno, parent_td,
                kDna4AlphabetCode, kDna4StorageWidth,
                target_total_bp, target_padding, cap_bytes,
                caches, target_producer);

        // (5) Build stubs + write the parent row tuple.
        pgseq_t* query_stub = bioseq::storage::alloc_locator_stub_in(
            BioseqColumnWritePerTupleContext(write_state),
            kDna4AlphabetCode, kDna4StorageWidth,
            query_padding, query_total_bp, query_loc);
        pgseq_t* target_stub = bioseq::storage::alloc_locator_stub_in(
            BioseqColumnWritePerTupleContext(write_state),
            kDna4AlphabetCode, kDna4StorageWidth,
            target_padding, target_total_bp, target_loc);

        const double effective_error_rate =
            params.substitution_rate + params.insertion_rate + params.deletion_rate;

        slot->tts_values[0] = Int64GetDatum(static_cast<std::int64_t>(pair_no + 1));
        slot->tts_values[1] = PointerGetDatum(query_stub);
        slot->tts_values[2] = PointerGetDatum(target_stub);
        slot->tts_values[3] = Int64GetDatum(static_cast<std::int64_t>(fragment_start));
        slot->tts_values[4] = Int64GetDatum(static_cast<std::int64_t>(fragment_end));
        slot->tts_values[5] = BoolGetDatum(is_reverse);
        slot->tts_values[6] = Float8GetDatum(effective_error_rate);
        slot->tts_values[7] = Int64GetDatum(static_cast<std::int64_t>(fragment_length));
        for (int idx = 0; idx < 8; ++idx) {
            slot->tts_isnull[idx] = false;
        }
        ExecStoreVirtualTuple(slot);

        Datum  write_values[8];
        bool   write_nulls[8];
        for (int idx = 0; idx < 8; ++idx) {
            write_values[idx] = slot->tts_values[idx];
            write_nulls[idx]  = false;
        }
        (void) BioseqColumnWriteRow(write_state, write_values, write_nulls);
    }

    ExecDropSingleTupleTableSlot(slot);
    {
        ResourceOwner saved_owner = CurrentResourceOwner;
        CurrentResourceOwner       = TopTransactionResourceOwner;
        caches->close_all(/*commit=*/true);
        table_close(parent_rel, NoLock);
        CurrentResourceOwner       = saved_owner;
    }

    PG_RETURN_OID(parent_relid);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_locator_inspect);
Datum bioseq_locator_inspect(PG_FUNCTION_ARGS)
{
    pgseq_t* hdr = pg_seq_arg<pgseq_t>(fcinfo, 0);
    TupleDesc out_td;
    if (get_call_result_type(fcinfo, /*resultTypeId=*/nullptr, &out_td)
        != TYPEFUNC_COMPOSITE)
    {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("function returning record called in context that "
                        "cannot accept type record")));
    }
    BlessTupleDesc(out_td);

    Datum values[7] = { 0, 0, 0, 0, 0, 0, 0 };
    bool  nulls[7]  = { false, false, false, false, false, false, false };

    if (!bioseq::storage::is_locator_stub(hdr)) {
        // For an inline (non-stub) value, return (version=0, bucket_idx=0,
        // shard_idx=0, num_fragments=0, total_bp_size=hdr->bp_size,
        // first_sub_row_no=0, total_padding=hdr->padding_letters) so the
        // function never raises on non-stub inputs — useful for ad-hoc
        // diagnostics on inline literals.
        values[0] = Int16GetDatum(static_cast<std::int16_t>(0));
        values[1] = Int16GetDatum(static_cast<std::int16_t>(0));
        values[2] = Int32GetDatum(static_cast<std::int32_t>(0));
        values[3] = Int64GetDatum(static_cast<std::int64_t>(0));
        values[4] = Int64GetDatum(static_cast<std::int64_t>(hdr->bp_size));
        values[5] = Int64GetDatum(static_cast<std::int64_t>(0));
        values[6] = Int32GetDatum(static_cast<std::int32_t>(hdr->padding_letters));
        HeapTuple tup = heap_form_tuple(out_td, values, nulls);
        Datum result = HeapTupleGetDatum(tup);
        PG_FREE_IF_COPY(hdr, 0);
        PG_RETURN_DATUM(result);
    }

    const auto loc = bioseq::storage::decode_locator_from_stub(hdr);
    values[0] = Int16GetDatum(static_cast<std::int16_t>(loc.version));
    values[1] = Int16GetDatum(static_cast<std::int16_t>(loc.bucket_idx));
    values[2] = Int32GetDatum(static_cast<std::int32_t>(loc.shard_idx));
    values[3] = Int64GetDatum(static_cast<std::int64_t>(loc.num_fragments));
    if (loc.total_bp_size >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        bioseq_log<ERR, PG>( "bioseq.locator_inspect: total_bp_size {} exceeds INT64_MAX",
               loc.total_bp_size);
    }
    values[4] = Int64GetDatum(static_cast<std::int64_t>(loc.total_bp_size));
    values[5] = Int64GetDatum(static_cast<std::int64_t>(loc.first_sub_row_no));
    values[6] = Int32GetDatum(static_cast<std::int32_t>(hdr->padding_letters));

    HeapTuple tup = heap_form_tuple(out_td, values, nulls);
    Datum result = HeapTupleGetDatum(tup);
    PG_FREE_IF_COPY(hdr, 0);
    PG_RETURN_DATUM(result);
}

}  // extern "C"
