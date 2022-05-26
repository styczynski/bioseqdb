#include "bwa.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <optional>
extern "C" {
#include <postgres.h>
#include <fmgr.h>
#include <funcapi.h>
#include <miscadmin.h>
#include <executor/spi.h>
#include <catalog/pg_type.h>
}

#define raise_pg_error(code, msg) ereport(ERROR, (errcode(code)), msg);

struct PgNucleotideSequence {
    char vl_len[4];
    char nucleotides[];

    std::string_view text() const {
        return {nucleotides, VARSIZE(this) - 4};
    }

    char* begin() {
        return nucleotides;
    }

    char* end() {
        return nucleotides + VARSIZE(this) - 4;
    }

    static PgNucleotideSequence* palloc(size_t len) {
        auto ptr = static_cast<PgNucleotideSequence*>(::palloc(4 + len));
        SET_VARSIZE(ptr, 4 + len);
        return ptr;
    }

    static PgNucleotideSequence* from_view(std::string_view seq) {
        auto ptr = palloc(seq.size());
        std::copy(seq.begin(), seq.end(), ptr->nucleotides);
        return ptr;
    }
};

namespace {

// Lowercase nucleotides should not be allowed to be stored in the database. Their meaning in non-standardized, and some
// libraries can handle them poorly (for example, by replacing them with Ns). They should be handled before importing
// them into the database, in order to make the internals more robust and prevent accidental usage. A valid option when
// importing is replacing them with uppercase ones, as their most common use is for repeating but valid nucleotides.
const std::string_view allowedNucleotides = "ACGTNWSMKRYSBDHV";

text *string_view_to_text(std::string_view s) {
    text *result = (text *) palloc(s.size() + VARHDRSZ);

    SET_VARSIZE(result, s.size() + VARHDRSZ);
    memcpy(VARDATA(result), s.data(), s.size());

    return result;
}

}

extern "C" {

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(nuclseq_in);
Datum nuclseq_in(PG_FUNCTION_ARGS) {
    std::string_view text = PG_GETARG_CSTRING(0);
    for (char chr : text) {
        if (std::find(allowedNucleotides.begin(), allowedNucleotides.end(), chr) == allowedNucleotides.end()) {
            raise_pg_error(ERRCODE_INVALID_TEXT_REPRESENTATION,
                    errmsg("invalid nucleotide in nuclseq_in: '%c'", chr));
        }
    }

    auto nuclseq = PgNucleotideSequence::palloc(text.size());
    std::copy(text.begin(), text.end(), nuclseq->nucleotides);
    PG_RETURN_POINTER(nuclseq);
}

PG_FUNCTION_INFO_V1(nuclseq_out);
Datum nuclseq_out(PG_FUNCTION_ARGS) {
    auto nucls = reinterpret_cast<PgNucleotideSequence*>(PG_DETOAST_DATUM(PG_GETARG_POINTER(0)))->text();
    auto text = static_cast<char*>(palloc(nucls.size() + 1));
    std::copy(nucls.begin(), nucls.end(), text);
    text[nucls.size()] = '\0';
    PG_RETURN_CSTRING(text);
}

PG_FUNCTION_INFO_V1(nuclseq_len);
Datum nuclseq_len(PG_FUNCTION_ARGS) {
    auto nucls = reinterpret_cast<PgNucleotideSequence*>(PG_DETOAST_DATUM(PG_GETARG_POINTER(0)))->text();
    PG_RETURN_UINT64(nucls.size());
}

PG_FUNCTION_INFO_V1(nuclseq_content);
Datum nuclseq_content(PG_FUNCTION_ARGS) {
    auto nucls = reinterpret_cast<PgNucleotideSequence*>(PG_DETOAST_DATUM(PG_GETARG_POINTER(0)))->text();
    std::string_view needle = PG_GETARG_CSTRING(1);
    if (needle.length() != 1 || std::find(allowedNucleotides.begin(), allowedNucleotides.end(), needle[0]) == allowedNucleotides.end()) {
        raise_pg_error(ERRCODE_INVALID_PARAMETER_VALUE,
                errmsg("invalid nucleotide in nuclseq_content: '%s'", needle.data()));
    }

    auto matches = static_cast<double>(std::count(nucls.begin(), nucls.end(), needle[0]));
    PG_RETURN_FLOAT8(matches / nucls.size());
}

PG_FUNCTION_INFO_V1(nuclseq_complement);
Datum nuclseq_complement(PG_FUNCTION_ARGS) {
    auto nucls = reinterpret_cast<PgNucleotideSequence*>(PG_DETOAST_DATUM(PG_GETARG_POINTER(0)))->text();
    auto complement = PgNucleotideSequence::palloc(nucls.size());
    for (size_t i=0; i<nucls.size(); ++i) {
        if (nucls[i] == 'A') {
            complement->nucleotides[i] = 'C';
        } else if (nucls[i] == 'C') {
            complement->nucleotides[i] = 'A';
        } else if (nucls[i] == 'T') {
            complement->nucleotides[i] = 'G';
        } else if (nucls[i] == 'G') {
            complement->nucleotides[i] = 'T';
        } else if (nucls[i] == 'N') {
            complement->nucleotides[i] = 'N';
        }
    }
    PG_RETURN_POINTER(complement);
}

PG_FUNCTION_INFO_V1(nuclseq_reverse);
Datum nuclseq_reverse(PG_FUNCTION_ARGS) {
    auto nucls = reinterpret_cast<PgNucleotideSequence*>(PG_DETOAST_DATUM(PG_GETARG_POINTER(0)))->text();
    auto complement = PgNucleotideSequence::palloc(nucls.size());
    std::reverse_copy(nucls.begin(), nucls.end(), complement->begin());
    PG_RETURN_POINTER(complement);
}

}

namespace {

template<typename F>
Portal iterate_nuclseq_table(const char* sql, Oid nuclseq_oid, F f) {
    Portal portal = SPI_cursor_open_with_args("iterate", sql, 0, nullptr, nullptr, nullptr, true, 0);
    long batch_size = 1;

    SPI_cursor_fetch(portal, true, batch_size);
    while (SPI_processed > 0 && SPI_tuptable != NULL) {
        int n = SPI_processed;
        SPITupleTable* tuptable = SPI_tuptable;
        TupleDesc tupdesc = tuptable->tupdesc;

        switch(SPI_gettypeid(tupdesc, 1)) {
            case INT2OID:
            case INT4OID:
            case INT8OID:
                break;
            default:
            raise_pg_error(ERRCODE_DATATYPE_MISMATCH, errmsg("expected column of integer"));
        }

        if (SPI_gettypeid(tupdesc, 2) != nuclseq_oid)
            raise_pg_error(ERRCODE_DATATYPE_MISMATCH, errmsg("expected column of nuclseqs"));


        for(int i = 0 ; i < n; i++) {
            HeapTuple tup = tuptable->vals[i];

            char* id = SPI_getvalue(tup, tupdesc, 1);
            char* seq = SPI_getvalue(tup, tupdesc, 2);
            f(id, seq);
        }

//        SPI_freetuptable(tuptable);
        SPI_cursor_fetch(portal, true, batch_size);
    }
    return portal;

}


BwaIndex bwa_index_from_query(const char* sql, Oid nuclseq_oid) {
    std::vector<BwaSequence> usv;
    Portal portal = iterate_nuclseq_table(sql, nuclseq_oid, [&](auto id, auto seq){
        // TODO: useless copying
        usv.push_back({id, seq});
    });
    BwaIndex bwa(usv);
    SPI_cursor_close(portal);
    bwa.options->max_occ = std::max<int>(500, 2 * usv.size());

    return bwa;
}

void assert_can_return_set(ReturnSetInfo* rsi) {
    if (rsi == NULL || !IsA(rsi, ReturnSetInfo)) {
        raise_pg_error(ERRCODE_FEATURE_NOT_SUPPORTED,
                errmsg("set-valued function called in context that cannot accept a set"));
    }
    if (!(rsi->allowedModes & SFRM_Materialize)) {
        raise_pg_error(ERRCODE_FEATURE_NOT_SUPPORTED,
                errmsg("materialize mode required, but it is not allowed in this context"));
    }
}

TupleDesc get_retval_tupledesc(const FunctionCallInfo& fcinfo) {
    TupleDesc tupledesc;

    switch (get_call_result_type(fcinfo, nullptr, &tupledesc)) {
        case TYPEFUNC_COMPOSITE:
            break;
        case TYPEFUNC_RECORD:
            raise_pg_error(ERRCODE_FEATURE_NOT_SUPPORTED,
                    errmsg("function returning record called in context that cannot accept type record"));
            break;
        default:
            raise_pg_error(ERRCODE_FEATURE_NOT_SUPPORTED,
                    errmsg("return type must be a row type"));
            break;
    }

    return tupledesc;
}

Tuplestorestate* create_tuplestore(ReturnSetInfo* rsi, TupleDesc& tupledesc) {
    MemoryContext per_query_ctx = rsi->econtext->ecxt_per_query_memory;
    MemoryContext old_ctx = MemoryContextSwitchTo(per_query_ctx);

    tupledesc = CreateTupleDescCopy(tupledesc);
    tupledesc = BlessTupleDesc(tupledesc);

    Tuplestorestate* tupstore = tuplestore_begin_heap(SFRM_Materialize_Random, false, work_mem);
    MemoryContextSwitchTo(old_ctx);

    return tupstore;
}

HeapTuple build_tuple_bwa(std::optional<std::string_view> query_id_view, const BwaMatch& match, TupleDesc& tupledesc) {
    int ref_id = 0, query_id = 0;
    std::from_chars(match.ref_id.cbegin(), match.ref_id.cend(), ref_id);
    if (query_id_view.has_value()) {
        std::from_chars(query_id_view->cbegin(), query_id_view->cend(), query_id);
    }

    std::array<bool, 15> nulls;
    std::array<Datum, 15> values { {
        Int32GetDatum(ref_id),
        PointerGetDatum(PgNucleotideSequence::from_view(match.ref_subseq)),
        Int32GetDatum((int) match.ref_match_begin),
        Int32GetDatum((int) match.ref_match_end),
        Int32GetDatum((int) match.ref_match_len),
        Int32GetDatum(query_id),
        PointerGetDatum(PgNucleotideSequence::from_view(match.query_subseq)),
        Int32GetDatum(match.query_match_begin),
        Int32GetDatum(match.query_match_end),
        Int32GetDatum(match.query_match_len),
        BoolGetDatum(match.is_primary),
        BoolGetDatum(match.is_secondary),
        BoolGetDatum(match.is_reverse),
        PointerGetDatum(string_view_to_text(match.cigar)),
        Int32GetDatum(match.score),
    } };

    nulls.fill(false);
    nulls[5] = !query_id_view.has_value();

    return heap_form_tuple(tupledesc, values.data(), nulls.data());
}

}

extern "C" {

PG_FUNCTION_INFO_V1(nuclseq_search_bwa);
Datum nuclseq_search_bwa(PG_FUNCTION_ARGS) {
    ReturnSetInfo* rsi = reinterpret_cast<ReturnSetInfo*>(fcinfo->resultinfo);
    assert_can_return_set(rsi);

    std::string_view nucls = reinterpret_cast<PgNucleotideSequence*>(PG_DETOAST_DATUM(PG_GETARG_POINTER(0)))->text();
    const char* reference_sql = PG_GETARG_CSTRING(1);

    if (int ret = SPI_connect(); ret < 0)
        elog(ERROR, "connectby: SPI_connect returned %d", ret);


    TupleDesc ret_tupdesc = get_retval_tupledesc(fcinfo);

    Oid nuclseq_oid = TupleDescAttr(ret_tupdesc, 1)->atttypid;
    BwaIndex bwa = bwa_index_from_query(reference_sql, nuclseq_oid);
    SPI_finish();

    Tuplestorestate* ret_tupstore = create_tuplestore(rsi, ret_tupdesc);
    AttInMetadata* attr_input_meta = TupleDescGetAttInMetadata(ret_tupdesc);

    std::vector<BwaMatch> aligns = bwa.align_sequence(nucls);

    for (BwaMatch& row : aligns) {
        HeapTuple tuple = build_tuple_bwa(std::nullopt, row, ret_tupdesc);
        tuplestore_puttuple(ret_tupstore, tuple);
        heap_freetuple(tuple);
    }

    rsi->returnMode = SFRM_Materialize;
    rsi->setResult = ret_tupstore;
    rsi->setDesc = ret_tupdesc;
    return (Datum) nullptr;
}

PG_FUNCTION_INFO_V1(nuclseq_multi_search_bwa);
Datum nuclseq_multi_search_bwa(PG_FUNCTION_ARGS) {
    ReturnSetInfo* rsi = reinterpret_cast<ReturnSetInfo*>(fcinfo->resultinfo);
    assert_can_return_set(rsi);

    const char* query_sql = PG_GETARG_CSTRING(0);
    const char* reference_sql = PG_GETARG_CSTRING(1);

    if (int ret = SPI_connect(); ret < 0)
        elog(ERROR, "connectby: SPI_connect returned %d", ret);

    TupleDesc ret_tupdesc = get_retval_tupledesc(fcinfo);
    Oid nuclseq_oid = TupleDescAttr(ret_tupdesc, 1)->atttypid;
    BwaIndex bwa = bwa_index_from_query(reference_sql, nuclseq_oid);
    Tuplestorestate* ret_tupstore = create_tuplestore(rsi, ret_tupdesc);
    AttInMetadata* attr_input_meta = TupleDescGetAttInMetadata(ret_tupdesc);

    iterate_nuclseq_table(query_sql, nuclseq_oid, [&](auto id, auto nuclseq){
        std::vector<BwaMatch> aligns = bwa.align_sequence(nuclseq);

        for (BwaMatch& row : aligns) {
            HeapTuple tuple = build_tuple_bwa(id, row, ret_tupdesc);
            tuplestore_puttuple(ret_tupstore, tuple);
            heap_freetuple(tuple);
        }
    });

    SPI_finish();

    rsi->returnMode = SFRM_Materialize;
    rsi->setResult = ret_tupstore;
    rsi->setDesc = ret_tupdesc;
    return (Datum) nullptr;
}

}
