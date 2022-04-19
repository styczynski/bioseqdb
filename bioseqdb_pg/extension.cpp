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
}

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

extern "C" {

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(nuclseq_in);
Datum nuclseq_in(PG_FUNCTION_ARGS) {
    std::string_view text = PG_GETARG_CSTRING(0);
    for (char chr : text) {
        if (chr != 'A' && chr != 'C' && chr != 'G' && chr != 'T') {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION)),
                    errmsg("invalid nucleotide in nuclseq_in: '%c'", chr));
        }
    }

    auto nuclseq = PgNucleotideSequence::palloc(text.size());
    std::copy(text.begin(), text.end(), nuclseq->nucleotides);
    PG_RETURN_POINTER(nuclseq);
}

PG_FUNCTION_INFO_V1(nuclseq_out);
Datum nuclseq_out(PG_FUNCTION_ARGS) {
    auto nucls = reinterpret_cast<PgNucleotideSequence*>(PG_GETARG_POINTER(0))->text();
    auto text = static_cast<char*>(palloc(nucls.size() + 1));
    std::copy(nucls.begin(), nucls.end(), text);
    text[nucls.size()] = '\0';
    PG_RETURN_CSTRING(text);
}

PG_FUNCTION_INFO_V1(nuclseq_len);
Datum nuclseq_len(PG_FUNCTION_ARGS) {
    auto nucls = reinterpret_cast<PgNucleotideSequence*>(PG_GETARG_POINTER(0))->text();
    PG_RETURN_UINT64(nucls.size());
}

PG_FUNCTION_INFO_V1(nuclseq_content);
Datum nuclseq_content(PG_FUNCTION_ARGS) {
    auto nucls = reinterpret_cast<PgNucleotideSequence*>(PG_GETARG_POINTER(0))->text();
    std::string_view needle = PG_GETARG_CSTRING(1);
    if (needle != "A" && needle != "C" && needle != "G" && needle != "T") {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE)),
                errmsg("invalid nucleotide in nuclseq_content: '%s'", needle.data()));
    }

    auto matches = static_cast<double>(std::count(nucls.begin(), nucls.end(), needle[0]));
    PG_RETURN_FLOAT8(matches / nucls.size());
}

PG_FUNCTION_INFO_V1(nuclseq_complement);
Datum nuclseq_complement(PG_FUNCTION_ARGS) {
    auto nucls = reinterpret_cast<PgNucleotideSequence*>(PG_GETARG_POINTER(0))->text();
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
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE)),
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

PG_FUNCTION_INFO_V1(yoyo_v2);
Datum yoyo_v2(PG_FUNCTION_ARGS) {
    ReturnSetInfo* rsi = reinterpret_cast<ReturnSetInfo*>(fcinfo->resultinfo);
    if (rsi == NULL || !IsA(rsi, ReturnSetInfo)) {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("set-valued function called in context that cannot accept a set")));
    }
    if (!(rsi->allowedModes & SFRM_Materialize)) {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("materialize mode required, but it is not allowed in this context")));
    }

    MemoryContext per_query_ctx = rsi->econtext->ecxt_per_query_memory;

    TupleDesc tupdesc;
    switch (get_call_result_type(fcinfo, nullptr, &tupdesc)) {
        case TYPEFUNC_COMPOSITE:
            break;
        case TYPEFUNC_RECORD:
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("function returning record called in context that cannot accept type record")));
            break;
        default:
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                    errmsg("return type must be a row type")));
            break;
    }

    MemoryContext old_ctx = MemoryContextSwitchTo(per_query_ctx);
    tupdesc = CreateTupleDescCopy(tupdesc);
    Tuplestorestate* tupstore = tuplestore_begin_heap(rsi->allowedModes & SFRM_Materialize_Random, false, work_mem);
    MemoryContextSwitchTo(old_ctx);

    AttInMetadata* attr_input_meta = TupleDescGetAttInMetadata(tupdesc);

    std::minstd_rand rng(std::chrono::high_resolution_clock::now().time_since_epoch().count());

    for (int i=0; i<6; ++i) {
        std::string alpha = std::to_string(i + 1);
        std::string beta = std::to_string((i + 1) * (i + 1));
        std::string gamma;
        for (int j=0; j<20; ++j) {
            gamma += "ACGT"[std::uniform_int_distribution<int>(0, 3)(rng)];
        }
        alpha.c_str();
        beta.c_str();
        gamma.c_str();

        char* values[3];
        values[0] = alpha.data();
        values[1] = beta.data();
        values[2] = gamma.data();

        HeapTuple tuple = BuildTupleFromCStrings(attr_input_meta, values);
        tuplestore_puttuple(tupstore, tuple);
        heap_freetuple(tuple);
    }

    rsi->returnMode = SFRM_Materialize;
    rsi->setResult = tupstore;
    rsi->setDesc = tupdesc;
    return (Datum) nullptr;
}

}
