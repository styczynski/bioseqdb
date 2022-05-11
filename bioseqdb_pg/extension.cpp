#include <algorithm>
#include <chrono>
#include <random>
#include <string>
#include <string_view>

extern "C" {
#include <postgres.h>
#include <fmgr.h>
#include <funcapi.h>
#include <miscadmin.h>
#include <executor/spi.h>
#include <catalog/pg_type.h>
}

#include <SeqLib/BWAWrapper.h>
#include <SeqLib/RefGenome.h>

#define raise_pg_error(code, msg) ereport(ERROR, (errcode(code)), msg); 

struct PgNucleotideSequence {
    char vl_len[4];
    char nucleotides[];

    std::string_view text() const {
        return {nucleotides, VARSIZE(this) - 4};
    }

    static PgNucleotideSequence* palloc(size_t len) {
        auto ptr = static_cast<PgNucleotideSequence*>(::palloc(4 + len));
        SET_VARSIZE(ptr, 4 + len);
        return ptr;
    }
};

namespace {

// Lowercase nucleotides should not be allowed to be stored in the database. Their meaning in non-standardized, and some
// libraries can handle them poorly (for example, by replacing them with Ns). They should be handled before importing
// them into the database, in order to make the internals more robust and prevent accidental usage. A valid option when
// importing is replacing them with uppercase ones, as their most common use is for repeating but valid nucleotides.
const std::string_view allowedNucleotides = "ACGTN";

template <typename T> std::string show(const T& x) {
    std::stringstream ss;
    ss << x;
    std::string s = ss.str();
    s.c_str();
    return std::move(s);
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

PG_FUNCTION_INFO_V1(yoyo_v1);
Datum yoyo_v1(PG_FUNCTION_ARGS) {
    if (SRF_IS_FIRSTCALL()) {
        FuncCallContext* funcctx = SRF_FIRSTCALL_INIT();
        MemoryContext oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        int num_tuples = PG_GETARG_INT32(0);
        if (num_tuples < 0) {
            raise_pg_error(ERRCODE_INVALID_PARAMETER_VALUE,
                    errmsg("number of rows cannot be negative"));
        }
        funcctx->max_calls = num_tuples;

        MemoryContextSwitchTo(oldcontext);
    }

    FuncCallContext* funcctx = SRF_PERCALL_SETUP();
    uint64_t result = funcctx->call_cntr;
    if (funcctx->call_cntr < funcctx->max_calls) {
        SRF_RETURN_NEXT(funcctx, UInt64GetDatum(result));
    } else {
        SRF_RETURN_DONE(funcctx);
    }
}

}

namespace {

std::string build_fetch_query(std::string_view table_name, std::string_view id_col_name, std::string_view seq_col_name) {
    std::stringstream sql_builder;
    sql_builder << "SELECT " <<  id_col_name << ", " << seq_col_name << " FROM "  << table_name;
    return sql_builder.str();
}

template<typename F>
void iterate_nuclseq_table(const std::string &sql, Oid nuclseq_oid, F f) {
    Portal portal = SPI_cursor_open_with_args("iterate", sql.c_str(), 0, nullptr, nullptr, nullptr, true, 0);
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

        SPI_freetuptable(tuptable);
        SPI_cursor_fetch(portal, true, batch_size);
    }
    SPI_cursor_close(portal);

}


SeqLib::BWAWrapper bwa_index_from_query(const std::string& sql, Oid nuclseq_oid) {
    SeqLib::UnalignedSequenceVector usv;
    SeqLib::BWAWrapper bwa;
    iterate_nuclseq_table(sql, nuclseq_oid, [&](auto id, auto seq){
        // TODO: useless copying
        usv.push_back({id, seq});
    });
    bwa.ConstructIndex(usv);

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
    Tuplestorestate* tupstore = tuplestore_begin_heap(SFRM_Materialize_Random, false, work_mem);
    MemoryContextSwitchTo(old_ctx);

    return tupstore;
}

}

extern "C" {

PG_FUNCTION_INFO_V1(nuclseq_search_bwa);
Datum nuclseq_search_bwa(PG_FUNCTION_ARGS) {
    ReturnSetInfo* rsi = reinterpret_cast<ReturnSetInfo*>(fcinfo->resultinfo);
    assert_can_return_set(rsi);

    std::string_view nucls = reinterpret_cast<PgNucleotideSequence*>(PG_DETOAST_DATUM(PG_GETARG_POINTER(0)))->text();
    std::string_view table_name = PG_GETARG_CSTRING(1);
    std::string_view id_col_name = PG_GETARG_CSTRING(2);
    std::string_view seq_col_name = PG_GETARG_CSTRING(3);
    bool hardclip = PG_GETARG_BOOL(4);
    double kswfops = PG_GETARG_FLOAT8(5);
    int max_secondary = PG_GETARG_INT32(6);

    if (int ret = SPI_connect(); ret < 0)
        elog(ERROR, "connectby: SPI_connect returned %d", ret);


    TupleDesc ret_tupdest = get_retval_tupledesc(fcinfo);
    
    std::string sql = build_fetch_query(table_name, id_col_name, seq_col_name);
    Oid nuclseq_oid = TupleDescAttr(ret_tupdest, 4)->atttypid;
    SeqLib::BWAWrapper bwa = bwa_index_from_query(sql, nuclseq_oid);
    SPI_finish();

    Tuplestorestate* ret_tupstore = create_tuplestore(rsi, ret_tupdest);
    AttInMetadata* attr_input_meta = TupleDescGetAttInMetadata(ret_tupdest);

    SeqLib::BamRecordVector aligns;
    bwa.AlignSequence(std::string(nucls), "1", aligns, hardclip, kswfops, max_secondary);

    for (SeqLib::BamRecord& row : aligns) {
        std::string target_name = bwa.ChrIDToName(row.ChrID());
        std::string target_start = show(row.Position());
        std::string target_end = show(row.PositionEnd());
        std::string target_len = show(row.Length());
        std::string target_aligned = show(row.Sequence());
        std::string result = show(row);

        char* values[7];
        values[0] = target_name.data();
        values[1] = target_start.data();
        values[2] = target_end.data();
        values[3] = target_len.data();
        values[4] = target_aligned.data();
        values[5] = result.data();

        HeapTuple tuple = BuildTupleFromCStrings(attr_input_meta, values);
        tuplestore_puttuple(ret_tupstore, tuple);
        heap_freetuple(tuple);
    }

    rsi->returnMode = SFRM_Materialize;
    rsi->setResult = ret_tupstore;
    rsi->setDesc = ret_tupdest;
    return (Datum) nullptr;
}

PG_FUNCTION_INFO_V1(nuclseq_multi_search_bwa);
Datum nuclseq_multi_search_bwa(PG_FUNCTION_ARGS) {
    ReturnSetInfo* rsi = reinterpret_cast<ReturnSetInfo*>(fcinfo->resultinfo);
    assert_can_return_set(rsi);

    std::string_view query_table_name = PG_GETARG_CSTRING(0);
    std::string_view id_query_col_name = PG_GETARG_CSTRING(1);
    std::string_view seq_query_col_name = PG_GETARG_CSTRING(2);

    std::string_view table_name = PG_GETARG_CSTRING(3);
    std::string_view id_col_name = PG_GETARG_CSTRING(4);
    std::string_view seq_col_name = PG_GETARG_CSTRING(5);

    bool hardclip = PG_GETARG_BOOL(6);
    double kswfops = PG_GETARG_FLOAT8(7);
    int max_secondary = PG_GETARG_INT32(8);

    if (int ret = SPI_connect(); ret < 0) 
        elog(ERROR, "connectby: SPI_connect returned %d", ret);

    TupleDesc ret_tupdest = get_retval_tupledesc(fcinfo);
    std::string isql = build_fetch_query(table_name, id_col_name, seq_col_name);
    Oid nuclseq_oid = TupleDescAttr(ret_tupdest, 5)->atttypid;
    SeqLib::BWAWrapper bwa = bwa_index_from_query(isql, nuclseq_oid);
    Tuplestorestate* ret_tupstore = create_tuplestore(rsi, ret_tupdest);
    AttInMetadata* attr_input_meta = TupleDescGetAttInMetadata(ret_tupdest);

    std::string qsql = build_fetch_query(table_name, id_col_name, seq_col_name);
    SeqLib::BamRecordVector aligns;
    iterate_nuclseq_table(qsql, nuclseq_oid, [&](auto id, auto nuclseq){
        bwa.AlignSequence(std::string(nuclseq), id, aligns, hardclip, kswfops, max_secondary);

        for (SeqLib::BamRecord& row : aligns) {
            std::string target_name = bwa.ChrIDToName(row.ChrID());
            std::string target_start = show(row.Position());
            std::string target_end = show(row.PositionEnd());
            std::string target_len = show(row.Length());
            std::string target_aligned = show(row.Sequence());
            std::string result = show(row);

            char* values[] = {
                id,
                target_name.data(),
                target_start.data(),
                target_end.data(),
                target_len.data(),
                target_aligned.data(),
                result.data(),
            };

            HeapTuple tuple = BuildTupleFromCStrings(attr_input_meta, values);
            tuplestore_puttuple(ret_tupstore, tuple);
            heap_freetuple(tuple);
        }

        aligns.clear();
    });

    SPI_finish();

    rsi->returnMode = SFRM_Materialize;
    rsi->setResult = ret_tupstore;
    rsi->setDesc = ret_tupdest;
    return (Datum) nullptr;
}

}
