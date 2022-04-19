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

#include <SeqLib/BWAWrapper.h>
#include <SeqLib/RefGenome.h>

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

PG_FUNCTION_INFO_V1(nuclseq_search_bwa);
Datum nuclseq_search_bwa(PG_FUNCTION_ARGS) {
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

    std::string_view nucls = reinterpret_cast<PgNucleotideSequence*>(PG_GETARG_POINTER(0))->text();
    std::string_view fasta_path = PG_GETARG_CSTRING(1);
    std::string_view region_chr_name = PG_GETARG_CSTRING(2);
    int32_t p1 = PG_GETARG_INT32(3);
    int32_t p2 = PG_GETARG_INT32(4);
    std::string_view usv_name = PG_GETARG_CSTRING(5);
    bool hardclip = PG_GETARG_BOOL(6);
    double kswfops = PG_GETARG_FLOAT8(7);
    int max_secondary = PG_GETARG_INT32(8);

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

    SeqLib::RefGenome ref;
    SeqLib::BWAWrapper bwa;
    SeqLib::UnalignedSequenceVector usv;
    SeqLib::BamRecordVector results;
    std::string error;

    try {
        ref.LoadIndex(std::string{fasta_path});
        std::string seq = ref.QueryRegion(region_chr_name.data(), p1, p2);
        usv = {{usv_name.data(), seq}};
        bwa.ConstructIndex(usv);
        bwa.AlignSequence(std::string(nucls), "my_seq", results, hardclip, kswfops, max_secondary);
    } catch (const std::exception& ex) {
        error = ex.what();
    }
    error.c_str();

    for (int i = 0; i < results.size(); ++i) {
        SeqLib::BamRecord& row = results[i];

        std::string id = show(i);
        std::string target_start = show(row.Position());
        std::string target_end = show(row.PositionEnd());
        std::string target_len = show(row.Length());
        std::string target_aligned = show(row.Sequence());
        std::string result = show(row);

        char* values[7];
        values[0] = id.data();
        values[1] = target_start.data();
        values[2] = target_end.data();
        values[3] = target_len.data();
        values[4] = target_aligned.data();
        values[5] = result.data();
        values[6] = error.data();

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
