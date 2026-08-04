module;

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

export module bioseq.codegen.safety:fmgr_ledger;

export namespace bioseq::codegen::safety {

enum class fmgr_migration_disposition {
    unchanged,
    retired_symbol,
    shared_symbol_split,
    bpchar_identity_repair,
    native_cpp_replacement,
};

struct fmgr_migration_entry {
    std::string_view sql_identity;
    std::string_view target_symbol;
    std::string_view legacy_symbol;
    fmgr_migration_disposition disposition;
};

inline constexpr std::array<std::string_view, 1>
doc0388_external_fmgr_symbols{
    "bioseq_external_handler",
};

inline constexpr std::array<fmgr_migration_entry, 272>
doc0388_fmgr_migration_ledger{{
    {"bioseq.bioseq_handler(internal)", "bioseq_handler", "bioseq_handler", fmgr_migration_disposition::unchanged},
    {"pg_catalog.alter_bioseq_table_set(regclass,int,int,name,int)", "alter_bioseq_table_set", "alter_bioseq_table_set", fmgr_migration_disposition::unchanged},
    {"pg_catalog.alter_bioseq_table_reset(regclass,bool,bool,bool,bool)", "alter_bioseq_table_reset", "alter_bioseq_table_reset", fmgr_migration_disposition::unchanged},
    {"bioseq.vacuum_seq_storage(regclass)", "vacuum_seq_storage", "", fmgr_migration_disposition::native_cpp_replacement},
    {"bioseq.reap_orphan_seq_sub_tables()", "reap_orphan_seq_sub_tables", "", fmgr_migration_disposition::native_cpp_replacement},
    {"bioseq_internal.upgrade_bioseq_storage(regclass)", "upgrade_bioseq_storage", "upgrade_bioseq_storage", fmgr_migration_disposition::unchanged},
    {"bioseq_internal.downgrade_bioseq_storage(regclass)", "downgrade_bioseq_storage", "downgrade_bioseq_storage", fmgr_migration_disposition::unchanged},
    {"bioseq.get_storage_id(regclass)", "get_storage_id", "bioseq_relation_storageid", fmgr_migration_disposition::retired_symbol},
    {"vemptycount(int8)", "vemptycount", "vemptycount", fmgr_migration_disposition::unchanged},
    {"vanycount(int8,\"any\")", "vanycount", "vanycount", fmgr_migration_disposition::unchanged},
    {"vint2int4avg(internal)", "vint2int4avg", "vint2int4avg", fmgr_migration_disposition::unchanged},
    {"vint2sum(int8,int2)", "vint2sum", "vint2sum", fmgr_migration_disposition::unchanged},
    {"vint2acc(internal,int2)", "vint2acc", "vint2acc", fmgr_migration_disposition::unchanged},
    {"vint2larger(int2,int2)", "vint2larger", "vint2larger", fmgr_migration_disposition::unchanged},
    {"vint2smaller(int2,int2)", "vint2smaller", "vint2smaller", fmgr_migration_disposition::unchanged},
    {"vint4sum(int8,int4)", "vint4sum", "vint4sum", fmgr_migration_disposition::unchanged},
    {"vint4acc(internal,int4)", "vint4acc", "vint4acc", fmgr_migration_disposition::unchanged},
    {"vint4larger(int4,int4)", "vint4larger", "vint4larger", fmgr_migration_disposition::unchanged},
    {"vint4smaller(int4,int4)", "vint4smaller", "vint4smaller", fmgr_migration_disposition::unchanged},
    {"vint8acc(internal,int8)", "vint8acc", "vint8acc", fmgr_migration_disposition::unchanged},
    {"vint8sum(internal)", "vint8sum", "vint8sum", fmgr_migration_disposition::unchanged},
    {"vint8avg(internal)", "vint8avg", "vint8avg", fmgr_migration_disposition::unchanged},
    {"vint8larger(int8,int8)", "vint8larger", "vint8larger", fmgr_migration_disposition::unchanged},
    {"vint8smaller(int8,int8)", "vint8smaller", "vint8smaller", fmgr_migration_disposition::unchanged},
    {"vdatelarger(date,date)", "vdatelarger", "vdatelarger", fmgr_migration_disposition::unchanged},
    {"vdatesmaller(date,date)", "vdatesmaller", "vdatesmaller", fmgr_migration_disposition::unchanged},
    {"vbpchareq(char,char)", "vbpchareq", "vchareq", fmgr_migration_disposition::bpchar_identity_repair},
    {"vbpcharne(char,char)", "vbpcharne", "vcharne", fmgr_migration_disposition::bpchar_identity_repair},
    {"vbpchargt(char,char)", "vbpchargt", "vchargt", fmgr_migration_disposition::bpchar_identity_repair},
    {"vbpcharlt(char,char)", "vbpcharlt", "vcharlt", fmgr_migration_disposition::bpchar_identity_repair},
    {"vbpcharle(char,char)", "vbpcharle", "vcharle", fmgr_migration_disposition::bpchar_identity_repair},
    {"vbpcharge(char,char)", "vbpcharge", "vcharge", fmgr_migration_disposition::bpchar_identity_repair},
    {"vint2eq(int2,int2)", "vint2eq", "vint2eq", fmgr_migration_disposition::unchanged},
    {"vint2ne(int2,int2)", "vint2ne", "vint2ne", fmgr_migration_disposition::unchanged},
    {"vint2gt(int2,int2)", "vint2gt", "vint2gt", fmgr_migration_disposition::unchanged},
    {"vint2lt(int2,int2)", "vint2lt", "vint2lt", fmgr_migration_disposition::unchanged},
    {"vint2le(int2,int2)", "vint2le", "vint2le", fmgr_migration_disposition::unchanged},
    {"vint2ge(int2,int2)", "vint2ge", "vint2ge", fmgr_migration_disposition::unchanged},
    {"vint24eq(int2,int4)", "vint24eq", "vint24eq", fmgr_migration_disposition::unchanged},
    {"vint24ne(int2,int4)", "vint24ne", "vint24ne", fmgr_migration_disposition::unchanged},
    {"vint24gt(int2,int4)", "vint24gt", "vint24gt", fmgr_migration_disposition::unchanged},
    {"vint24lt(int2,int4)", "vint24lt", "vint24lt", fmgr_migration_disposition::unchanged},
    {"vint24le(int2,int4)", "vint24le", "vint24le", fmgr_migration_disposition::unchanged},
    {"vint24ge(int2,int4)", "vint24ge", "vint24ge", fmgr_migration_disposition::unchanged},
    {"vint28eq(int2,int8)", "vint28eq", "vint28eq", fmgr_migration_disposition::unchanged},
    {"vint28ne(int2,int8)", "vint28ne", "vint28ne", fmgr_migration_disposition::unchanged},
    {"vint28gt(int2,int8)", "vint28gt", "vint28gt", fmgr_migration_disposition::unchanged},
    {"vint28lt(int2,int8)", "vint28lt", "vint28lt", fmgr_migration_disposition::unchanged},
    {"vint28le(int2,int8)", "vint28le", "vint28le", fmgr_migration_disposition::unchanged},
    {"vint28ge(int2,int8)", "vint28ge", "vint28ge", fmgr_migration_disposition::unchanged},
    {"vint4eq(int4,int4)", "vint4eq", "vint4eq", fmgr_migration_disposition::unchanged},
    {"vint4ne(int4,int4)", "vint4ne", "vint4ne", fmgr_migration_disposition::unchanged},
    {"vint4gt(int4,int4)", "vint4gt", "vint4gt", fmgr_migration_disposition::unchanged},
    {"vint4lt(int4,int4)", "vint4lt", "vint4lt", fmgr_migration_disposition::unchanged},
    {"vint4le(int4,int4)", "vint4le", "vint4le", fmgr_migration_disposition::unchanged},
    {"vint4ge(int4,int4)", "vint4ge", "vint4ge", fmgr_migration_disposition::unchanged},
    {"vint42eq(int4,int2)", "vint42eq", "vint42eq", fmgr_migration_disposition::unchanged},
    {"vint42ne(int4,int2)", "vint42ne", "vint42ne", fmgr_migration_disposition::unchanged},
    {"vint42gt(int4,int2)", "vint42gt", "vint42gt", fmgr_migration_disposition::unchanged},
    {"vint42lt(int4,int2)", "vint42lt", "vint42lt", fmgr_migration_disposition::unchanged},
    {"vint42le(int4,int2)", "vint42le", "vint42le", fmgr_migration_disposition::unchanged},
    {"vint42ge(int4,int2)", "vint42ge", "vint42ge", fmgr_migration_disposition::unchanged},
    {"vint48eq(int4,int8)", "vint48eq", "vint48eq", fmgr_migration_disposition::unchanged},
    {"vint48ne(int4,int8)", "vint48ne", "vint48ne", fmgr_migration_disposition::unchanged},
    {"vint48gt(int4,int8)", "vint48gt", "vint48gt", fmgr_migration_disposition::unchanged},
    {"vint48lt(int4,int8)", "vint48lt", "vint48lt", fmgr_migration_disposition::unchanged},
    {"vint48le(int4,int8)", "vint48le", "vint48le", fmgr_migration_disposition::unchanged},
    {"vint48ge(int4,int8)", "vint48ge", "vint48ge", fmgr_migration_disposition::unchanged},
    {"vint8eq(int8,int8)", "vint8eq", "vint8eq", fmgr_migration_disposition::unchanged},
    {"vint8ne(int8,int8)", "vint8ne", "vint8ne", fmgr_migration_disposition::unchanged},
    {"vint8gt(int8,int8)", "vint8gt", "vint8gt", fmgr_migration_disposition::unchanged},
    {"vint8lt(int8,int8)", "vint8lt", "vint8lt", fmgr_migration_disposition::unchanged},
    {"vint8le(int8,int8)", "vint8le", "vint8le", fmgr_migration_disposition::unchanged},
    {"vint8ge(int8,int8)", "vint8ge", "vint8ge", fmgr_migration_disposition::unchanged},
    {"vint82eq(int8,int2)", "vint82eq", "vint82eq", fmgr_migration_disposition::unchanged},
    {"vint82ne(int8,int2)", "vint82ne", "vint82ne", fmgr_migration_disposition::unchanged},
    {"vint82gt(int8,int2)", "vint82gt", "vint82gt", fmgr_migration_disposition::unchanged},
    {"vint82lt(int8,int2)", "vint82lt", "vint82lt", fmgr_migration_disposition::unchanged},
    {"vint82le(int8,int2)", "vint82le", "vint82le", fmgr_migration_disposition::unchanged},
    {"vint82ge(int8,int2)", "vint82ge", "vint82ge", fmgr_migration_disposition::unchanged},
    {"vint84eq(int8,int4)", "vint84eq", "vint84eq", fmgr_migration_disposition::unchanged},
    {"vint84ne(int8,int4)", "vint84ne", "vint84ne", fmgr_migration_disposition::unchanged},
    {"vint84gt(int8,int4)", "vint84gt", "vint84gt", fmgr_migration_disposition::unchanged},
    {"vint84lt(int8,int4)", "vint84lt", "vint84lt", fmgr_migration_disposition::unchanged},
    {"vint84le(int8,int4)", "vint84le", "vint84le", fmgr_migration_disposition::unchanged},
    {"vint84ge(int8,int4)", "vint84ge", "vint84ge", fmgr_migration_disposition::unchanged},
    {"vdate_eq(date,date)", "vdate_eq", "vdate_eq", fmgr_migration_disposition::unchanged},
    {"vdate_ne(date,date)", "vdate_ne", "vdate_ne", fmgr_migration_disposition::unchanged},
    {"vdate_gt(date,date)", "vdate_gt", "vdate_gt", fmgr_migration_disposition::unchanged},
    {"vdate_lt(date,date)", "vdate_lt", "vdate_lt", fmgr_migration_disposition::unchanged},
    {"vdate_le(date,date)", "vdate_le", "vdate_le", fmgr_migration_disposition::unchanged},
    {"vdate_ge(date,date)", "vdate_ge", "vdate_ge", fmgr_migration_disposition::unchanged},
    {"vtime_eq(time,time)", "vtime_eq", "vtime_eq", fmgr_migration_disposition::unchanged},
    {"vtime_ne(time,time)", "vtime_ne", "vtime_ne", fmgr_migration_disposition::unchanged},
    {"vtime_gt(time,time)", "vtime_gt", "vtime_gt", fmgr_migration_disposition::unchanged},
    {"vtime_lt(time,time)", "vtime_lt", "vtime_lt", fmgr_migration_disposition::unchanged},
    {"vtime_le(time,time)", "vtime_le", "vtime_le", fmgr_migration_disposition::unchanged},
    {"vtime_ge(time,time)", "vtime_ge", "vtime_ge", fmgr_migration_disposition::unchanged},
    {"vtimestamp_eq(timestamp,timestamp)", "vtimestamp_eq", "vtimestamp_eq", fmgr_migration_disposition::unchanged},
    {"vtimestamp_ne(timestamp,timestamp)", "vtimestamp_ne", "vtimestamp_ne", fmgr_migration_disposition::unchanged},
    {"vtimestamp_gt(timestamp,timestamp)", "vtimestamp_gt", "vtimestamp_gt", fmgr_migration_disposition::unchanged},
    {"vtimestamp_lt(timestamp,timestamp)", "vtimestamp_lt", "vtimestamp_lt", fmgr_migration_disposition::unchanged},
    {"vtimestamp_le(timestamp,timestamp)", "vtimestamp_le", "vtimestamp_le", fmgr_migration_disposition::unchanged},
    {"vtimestamp_ge(timestamp,timestamp)", "vtimestamp_ge", "vtimestamp_ge", fmgr_migration_disposition::unchanged},
    {"vtimestamptz_eq(timestamptz,timestamptz)", "vtimestamptz_eq", "vtimestamptz_eq", fmgr_migration_disposition::unchanged},
    {"vtimestamptz_ne(timestamptz,timestamptz)", "vtimestamptz_ne", "vtimestamptz_ne", fmgr_migration_disposition::unchanged},
    {"vtimestamptz_gt(timestamptz,timestamptz)", "vtimestamptz_gt", "vtimestamptz_gt", fmgr_migration_disposition::unchanged},
    {"vtimestamptz_lt(timestamptz,timestamptz)", "vtimestamptz_lt", "vtimestamptz_lt", fmgr_migration_disposition::unchanged},
    {"vtimestamptz_le(timestamptz,timestamptz)", "vtimestamptz_le", "vtimestamptz_le", fmgr_migration_disposition::unchanged},
    {"vtimestamptz_ge(timestamptz,timestamptz)", "vtimestamptz_ge", "vtimestamptz_ge", fmgr_migration_disposition::unchanged},
    {"mmsearch_support(internal)", "mmsearch_support", "mmsearch_support", fmgr_migration_disposition::unchanged},
    {"bioseq_placeholder_support(internal)", "bioseq_placeholder_support", "bioseq_placeholder_support", fmgr_migration_disposition::unchanged},
    {"bioseq.score(anyelement)", "score", "bioseq_score_stub", fmgr_migration_disposition::retired_symbol},
    {"bioseq.dp_score(anyelement)", "dp_score", "bioseq_dp_score_stub", fmgr_migration_disposition::retired_symbol},
    {"bioseq.query_len(anyelement)", "query_len", "bioseq_query_len_stub", fmgr_migration_disposition::retired_symbol},
    {"bioseq.query_start(anyelement)", "query_start", "bioseq_query_start_stub", fmgr_migration_disposition::retired_symbol},
    {"bioseq.query_end(anyelement)", "query_end", "bioseq_query_end_stub", fmgr_migration_disposition::retired_symbol},
    {"bioseq.target_len(anyelement)", "target_len", "bioseq_target_len_stub", fmgr_migration_disposition::retired_symbol},
    {"bioseq.target_start(anyelement)", "target_start", "bioseq_target_start_stub", fmgr_migration_disposition::retired_symbol},
    {"bioseq.target_end(anyelement)", "target_end", "bioseq_target_end_stub", fmgr_migration_disposition::retired_symbol},
    {"bioseq.num_matches(anyelement)", "num_matches", "bioseq_num_matches_stub", fmgr_migration_disposition::retired_symbol},
    {"bioseq.block_length(anyelement)", "block_length", "bioseq_block_length_stub", fmgr_migration_disposition::retired_symbol},
    {"bioseq.edit(anyelement)", "edit", "bioseq_edit_stub", fmgr_migration_disposition::retired_symbol},
    {"bioseq.mapq(anyelement)", "mapq", "bioseq_mapq_stub", fmgr_migration_disposition::retired_symbol},
    {"bioseq.chain_score(anyelement)", "chain_score", "bioseq_chain_score_stub", fmgr_migration_disposition::retired_symbol},
    {"bioseq.dp_max_score(anyelement)", "dp_max_score", "bioseq_dp_max_score_stub", fmgr_migration_disposition::retired_symbol},
    {"bioseq.seed_count(anyelement)", "seed_count", "bioseq_seed_count_stub", fmgr_migration_disposition::retired_symbol},
    {"bioseq.secondary_chain_score(anyelement)", "secondary_chain_score", "bioseq_secondary_chain_score_stub", fmgr_migration_disposition::retired_symbol},
    {"bioseq.is_reverse(anyelement)", "is_reverse", "bioseq_is_reverse_stub", fmgr_migration_disposition::retired_symbol},
    {"bioseq.is_primary(anyelement)", "is_primary", "bioseq_is_primary_stub", fmgr_migration_disposition::retired_symbol},
    {"bioseq.has_cigar(anyelement)", "has_cigar", "bioseq_has_cigar_stub", fmgr_migration_disposition::retired_symbol},
    {"bioseq.divergence(anyelement)", "divergence", "bioseq_divergence_stub", fmgr_migration_disposition::retired_symbol},
    {"bioseq.cigar(anyelement)", "cigar", "bioseq_cigar_stub", fmgr_migration_disposition::retired_symbol},
    {"bioseq.seq_in(cstring,oid,integer)", "seq_in", "pg_compact_sequence_in", fmgr_migration_disposition::retired_symbol},
    {"bioseq.seq_out(bioseq.seq)", "seq_out", "pg_compact_sequence_out", fmgr_migration_disposition::retired_symbol},
    {"bioseq.seq_recv(internal,oid,integer)", "seq_recv", "pg_compact_sequence_recv", fmgr_migration_disposition::retired_symbol},
    {"bioseq.seq_send(bioseq.seq)", "seq_send", "pg_compact_sequence_send", fmgr_migration_disposition::retired_symbol},
    {"bioseq.seq_typmod_in(cstring[])", "seq_typmod_in", "pg_compact_sequence_typmod_in", fmgr_migration_disposition::retired_symbol},
    {"bioseq.seq_typmod_out(integer)", "seq_typmod_out", "pg_compact_sequence_typmod_out", fmgr_migration_disposition::retired_symbol},
    {"bioseq.letter_in(cstring,oid,integer)", "letter_in", "letter_in", fmgr_migration_disposition::unchanged},
    {"bioseq.letter_out(bioseq.letter)", "letter_out", "letter_out", fmgr_migration_disposition::unchanged},
    {"bioseq.letter_recv(internal,oid,integer)", "letter_recv", "letter_recv", fmgr_migration_disposition::unchanged},
    {"bioseq.letter_send(bioseq.letter)", "letter_send", "letter_send", fmgr_migration_disposition::unchanged},
    {"bioseq.letter_typmod_in(cstring[])", "letter_typmod_in", "letter_typmod_in", fmgr_migration_disposition::unchanged},
    {"bioseq.letter_typmod_out(integer)", "letter_typmod_out", "letter_typmod_out", fmgr_migration_disposition::unchanged},
    {"bioseq.letter(bioseq.letter,integer,boolean)", "letter", "letter_typmod_cast", fmgr_migration_disposition::retired_symbol},
    {"bioseq.letter_to_text(bioseq.letter)", "letter_to_text", "letter_to_text", fmgr_migration_disposition::unchanged},
    {"bioseq.letter_eq(bioseq.letter,bioseq.letter)", "letter_eq", "letter_eq", fmgr_migration_disposition::unchanged},
    {"bioseq.letter_ne(bioseq.letter,bioseq.letter)", "letter_ne", "letter_ne", fmgr_migration_disposition::unchanged},
    {"bioseq.letter_lt(bioseq.letter,bioseq.letter)", "letter_lt", "letter_lt", fmgr_migration_disposition::unchanged},
    {"bioseq.letter_le(bioseq.letter,bioseq.letter)", "letter_le", "letter_le", fmgr_migration_disposition::unchanged},
    {"bioseq.letter_gt(bioseq.letter,bioseq.letter)", "letter_gt", "letter_gt", fmgr_migration_disposition::unchanged},
    {"bioseq.letter_ge(bioseq.letter,bioseq.letter)", "letter_ge", "letter_ge", fmgr_migration_disposition::unchanged},
    {"bioseq.letter_cmp(bioseq.letter,bioseq.letter)", "letter_cmp", "letter_cmp", fmgr_migration_disposition::unchanged},
    {"bioseq.letter_hash(bioseq.letter)", "letter_hash", "letter_hash", fmgr_migration_disposition::unchanged},
    {"bioseq.letter_hash_extended(bioseq.letter,bigint)", "letter_hash_extended", "letter_hash_extended", fmgr_migration_disposition::unchanged},
    {"bioseq.seq_subscript_handler(internal)", "seq_subscript_handler", "seq_subscript_handler", fmgr_migration_disposition::unchanged},
    {"bioseq.seq_cast(bioseq.seq,integer,boolean)", "seq_cast", "pg_compact_sequence_cast", fmgr_migration_disposition::retired_symbol},
    {"bioseq.seq_cmp(bioseq.seq,bioseq.seq)", "seq_cmp", "pg_compact_sequence_cmp", fmgr_migration_disposition::retired_symbol},
    {"bioseq.seq_lt(bioseq.seq,bioseq.seq)", "seq_lt", "pg_compact_sequence_lt", fmgr_migration_disposition::retired_symbol},
    {"bioseq.seq_le(bioseq.seq,bioseq.seq)", "seq_le", "pg_compact_sequence_le", fmgr_migration_disposition::retired_symbol},
    {"bioseq.seq_eq(bioseq.seq,bioseq.seq)", "seq_eq", "pg_compact_sequence_eq", fmgr_migration_disposition::retired_symbol},
    {"bioseq.seq_ne(bioseq.seq,bioseq.seq)", "seq_ne", "pg_compact_sequence_ne", fmgr_migration_disposition::retired_symbol},
    {"bioseq.seq_ge(bioseq.seq,bioseq.seq)", "seq_ge", "pg_compact_sequence_ge", fmgr_migration_disposition::retired_symbol},
    {"bioseq.seq_gt(bioseq.seq,bioseq.seq)", "seq_gt", "pg_compact_sequence_gt", fmgr_migration_disposition::retired_symbol},
    {"bioseq.seq_concat(bioseq.seq,bioseq.seq)", "seq_concat", "seq_concat", fmgr_migration_disposition::unchanged},
    {"bioseq.seq_concat_text(bioseq.seq,text)", "seq_concat_text", "seq_concat_text", fmgr_migration_disposition::unchanged},
    {"bioseq.text_concat_seq(text,bioseq.seq)", "text_concat_seq", "text_concat_seq", fmgr_migration_disposition::unchanged},
    {"bioseq.seq_concat_varchar(bioseq.seq,varchar)", "seq_concat_varchar", "seq_concat_varchar", fmgr_migration_disposition::unchanged},
    {"bioseq.varchar_concat_seq(varchar,bioseq.seq)", "varchar_concat_seq", "varchar_concat_seq", fmgr_migration_disposition::unchanged},
    {"bioseq.seq_concat_text_array(bioseq.seq,text[])", "seq_concat_text_array", "seq_concat_text_array", fmgr_migration_disposition::unchanged},
    {"bioseq.text_array_concat_seq(text[],bioseq.seq)", "text_array_concat_seq", "text_array_concat_seq", fmgr_migration_disposition::unchanged},
    {"bioseq.seq_concat_letter_array(bioseq.seq,bioseq.letter[])", "seq_concat_letter_array", "seq_concat_letter_array", fmgr_migration_disposition::unchanged},
    {"bioseq.letter_array_concat_seq(bioseq.letter[],bioseq.seq)", "letter_array_concat_seq", "letter_array_concat_seq", fmgr_migration_disposition::unchanged},
    {"bioseq.reverse(bioseq.seq)", "reverse", "seq_reverse", fmgr_migration_disposition::retired_symbol},
    {"bioseq.reverse_complement(bioseq.seq)", "reverse_complement", "seq_reverse_complement", fmgr_migration_disposition::retired_symbol},
    {"bioseq.array_cat(bioseq.seq,bioseq.seq)", "array_cat", "seq_concat", fmgr_migration_disposition::shared_symbol_split},
    {"bioseq.array_length(bioseq.seq,integer)", "array_length", "seq_array_length", fmgr_migration_disposition::retired_symbol},
    {"bioseq.cardinality(bioseq.seq)", "cardinality", "seq_cardinality", fmgr_migration_disposition::retired_symbol},
    {"bioseq.to_text(bioseq.seq)", "to_text", "seq_to_text", fmgr_migration_disposition::retired_symbol},
    {"bioseq.to_varchar(bioseq.seq)", "to_varchar", "seq_to_varchar", fmgr_migration_disposition::retired_symbol},
    {"bioseq.to_array(bioseq.seq)", "to_array", "seq_to_array", fmgr_migration_disposition::retired_symbol},
    {"bioseq.from_array(bioseq.letter[],integer)", "from_array", "seq_from_letter_array_infer", fmgr_migration_disposition::retired_symbol},
    {"bioseq.from_array(bioseq.letter[],text,integer)", "from_array", "seq_from_letter_array_named", fmgr_migration_disposition::retired_symbol},
    {"bioseq.seq_hash(bioseq.seq)", "seq_hash", "seq_hash", fmgr_migration_disposition::unchanged},
    {"bioseq.seq_hash_extended(bioseq.seq,bigint)", "seq_hash_extended", "seq_hash_extended", fmgr_migration_disposition::unchanged},
    {"bioseq_internal.slice_trace_self_test()", "slice_trace_self_test", "slice_trace_self_test", fmgr_migration_disposition::unchanged},
    {"bioseq_internal.slice_payload_allocation_probe(integer)", "slice_payload_allocation_probe", "slice_payload_allocation_probe", fmgr_migration_disposition::unchanged},
    {"bioseq_internal.slice_trace_worker_failure_probe(integer)", "slice_trace_worker_failure_probe", "slice_trace_worker_failure_probe", fmgr_migration_disposition::unchanged},
    {"bioseq_internal.arm_slice_dsm_fixture(text)", "arm_slice_dsm_fixture", "bioseq_internal_arm_slice_dsm_fixture", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.synthetic_lazy_sequence(bigint)", "synthetic_lazy_sequence", "synthetic_lazy_sequence", fmgr_migration_disposition::unchanged},
    {"bioseq_internal.sequence_piece_count(bioseq.seq)", "sequence_piece_count", "sequence_piece_count", fmgr_migration_disposition::unchanged},
    {"bioseq_internal.materialization_size_self_test()", "materialization_size_self_test", "materialization_size_self_test", fmgr_migration_disposition::unchanged},
    {"bioseq_internal.piece_tree_self_test()", "piece_tree_self_test", "piece_tree_self_test", fmgr_migration_disposition::unchanged},
    {"bioseq_internal.expanded_ownership_self_test()", "expanded_ownership_self_test", "expanded_ownership_self_test", fmgr_migration_disposition::unchanged},
    {"bioseq_internal.letter_wire_self_test()", "letter_wire_self_test", "letter_wire_self_test", fmgr_migration_disposition::unchanged},
    {"bioseq_internal.letter_hook_chain_calls()", "letter_hook_chain_calls", "letter_hook_chain_calls", fmgr_migration_disposition::unchanged},
    {"bioseq_internal.arm_sequence_source_failure(text)", "arm_sequence_source_failure", "bioseq_internal_arm_sequence_source_failure", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.disarm_sequence_source_failure()", "disarm_sequence_source_failure", "bioseq_internal_disarm_sequence_source_failure", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.reset_sequence_source_counters()", "reset_sequence_source_counters", "bioseq_internal_reset_sequence_source_counters", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.sequence_source_counters()", "sequence_source_counters", "bioseq_internal_sequence_source_counters", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.sequence_source_wrong_thread_probe()", "sequence_source_wrong_thread_probe", "bioseq_internal_sequence_source_wrong_thread_probe", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.sequence_source_fault_contract_probe()", "sequence_source_fault_contract_probe", "bioseq_internal_sequence_source_fault_contract_probe", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.sequence_source_route_alias_probe()", "sequence_source_route_alias_probe", "bioseq_internal_sequence_source_route_alias_probe", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.sequence_facade_orientation_probe(bioseq.seq)", "sequence_facade_orientation_probe", "bioseq_internal_sequence_facade_orientation_probe", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.sequence_source_pre_prepare_probe()", "sequence_source_pre_prepare_probe", "bioseq_internal_sequence_source_pre_prepare_probe", fmgr_migration_disposition::retired_symbol},
    {"bioseq.len(bioseq.seq)", "len", "pg_compact_sequence_len", fmgr_migration_disposition::retired_symbol},
    {"bioseq.unit_len(bioseq.seq)", "unit_len", "pg_compact_sequence_unit_len", fmgr_migration_disposition::retired_symbol},
    {"bioseq_symbol_count_support(internal)", "bioseq_symbol_count_support", "bioseq_symbol_count_support", fmgr_migration_disposition::unchanged},
    {"bioseq.symbol_count(bioseq.seq,text)", "symbol_count", "bioseq_symbol_count", fmgr_migration_disposition::retired_symbol},
    {"mmsearch(bioseq.seq,bioseq.seq)", "mmsearch", "mmsearch", fmgr_migration_disposition::unchanged},
    {"mmsearch(bioseq.seq,bioseq.seq,bioseq_aligner_options)", "mmsearch", "mmsearch", fmgr_migration_disposition::unchanged},
    {"bioseq.list_metadata()", "list_metadata", "bioseq_list_metadata", fmgr_migration_disposition::retired_symbol},
    {"bioseq.update_index(regclass,text)", "update_index", "bioseq_update_index", fmgr_migration_disposition::retired_symbol},
    {"bioseq.load(regclass,text,text,bioseq.column_mapping[])", "load", "bioseq_load", fmgr_migration_disposition::retired_symbol},
    {"bioseq.last_index_merge_run_count()", "last_index_merge_run_count", "bioseq_last_index_merge_run_count", fmgr_migration_disposition::retired_symbol},
    {"bioseq.last_alignment_execution_stats()", "last_alignment_execution_stats", "bioseq_last_alignment_execution_stats", fmgr_migration_disposition::retired_symbol},
    {"bioseq.last_alignment_execution_stats_v2()", "last_alignment_execution_stats_v2", "bioseq_last_alignment_execution_stats_v2", fmgr_migration_disposition::retired_symbol},
    {"bioseq.last_alignment_format5_reader_memory_evidence()", "last_alignment_format5_reader_memory_evidence", "bioseq_last_alignment_format5_reader_memory_evidence", fmgr_migration_disposition::retired_symbol},
    {"bioseq.last_alignment_format5_reader_memory_evidence_v2()", "last_alignment_format5_reader_memory_evidence_v2", "bioseq_last_alignment_format5_reader_memory_evidence_v2", fmgr_migration_disposition::retired_symbol},
    {"bioseq.last_alignment_format5_reader_memory_evidence_v3()", "last_alignment_format5_reader_memory_evidence_v3", "bioseq_last_alignment_format5_reader_memory_evidence_v3", fmgr_migration_disposition::retired_symbol},
    {"bioseq.last_alignment_format5_reader_memory_evidence_v4()", "last_alignment_format5_reader_memory_evidence_v4", "bioseq_last_alignment_format5_reader_memory_evidence_v4", fmgr_migration_disposition::retired_symbol},
    {"bioseq.last_alignment_format5_reader_memory_evidence_v5()", "last_alignment_format5_reader_memory_evidence_v5", "bioseq_last_alignment_format5_reader_memory_evidence_v5", fmgr_migration_disposition::retired_symbol},
    {"bioseq.last_alignment_format5_reader_memory_evidence_v6()", "last_alignment_format5_reader_memory_evidence_v6", "bioseq_last_alignment_format5_reader_memory_evidence_v6", fmgr_migration_disposition::retired_symbol},
    {"bioseq.last_alignment_format6_reader_evidence()", "last_alignment_format6_reader_evidence", "bioseq_last_alignment_format6_reader_evidence", fmgr_migration_disposition::retired_symbol},
    {"bioseq.last_alignment_persisted_gpu_operation_evidence_v1()", "last_alignment_persisted_gpu_operation_evidence_v1", "bioseq_last_alignment_persisted_gpu_operation_evidence_v1", fmgr_migration_disposition::retired_symbol},
    {"bioseq.last_alignment_persisted_gpu_operation_evidence_v2()", "last_alignment_persisted_gpu_operation_evidence_v2", "bioseq_last_alignment_persisted_gpu_operation_evidence_v2", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.reset_active_read_policy_debug()", "reset_active_read_policy_debug", "bioseq_internal_reset_active_read_policy_debug", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.active_read_policy_debug()", "active_read_policy_debug", "bioseq_internal_active_read_policy_debug", fmgr_migration_disposition::retired_symbol},
    {"bioseq.last_mps_build_stats()", "last_mps_build_stats", "bioseq_last_mps_build_stats", fmgr_migration_disposition::retired_symbol},
    {"bioseq.scheduler_mps_build_stats()", "scheduler_mps_build_stats", "bioseq_scheduler_mps_build_stats", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.arm_async_mps_worker_failure(text)", "arm_async_mps_worker_failure", "bioseq_internal_arm_async_mps_worker_failure", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.disarm_async_mps_worker_failure()", "disarm_async_mps_worker_failure", "bioseq_internal_disarm_async_mps_worker_failure", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.async_mps_worker_boundary_stats()", "async_mps_worker_boundary_stats", "bioseq_internal_async_mps_worker_boundary_stats", fmgr_migration_disposition::retired_symbol},
    {"bioseq.scheduler_restart()", "scheduler_restart", "bioseq_scheduler_restart", fmgr_migration_disposition::retired_symbol},
    {"bioseq.scheduler_stats()", "scheduler_stats", "bioseq_scheduler_stats", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.cpu_prepared_target_same_object_reprepare_probe()", "cpu_prepared_target_same_object_reprepare_probe", "bioseq_internal_cpu_prepared_target_same_object_reprepare_probe", fmgr_migration_disposition::retired_symbol},
    {"bioseq.gpu_warmup_shader_cache()", "gpu_warmup_shader_cache", "bioseq_gpu_warmup_shader_cache", fmgr_migration_disposition::retired_symbol},
    {"bioseq.vulkan_runtime_status()", "vulkan_runtime_status", "bioseq_vulkan_runtime_status", fmgr_migration_disposition::retired_symbol},
    {"bioseq.set_index_policy(regclass,text,text)", "set_index_policy", "bioseq_set_index_policy", fmgr_migration_disposition::retired_symbol},
    {"bioseq.set_active_read_index(regclass,text)", "set_active_read_index", "bioseq_set_active_read_index", fmgr_migration_disposition::retired_symbol},
    {"bioseq.drop_all_index_tables()", "drop_all_index_tables", "bioseq_drop_all_index_tables", fmgr_migration_disposition::retired_symbol},
    {"bioseq.index_table_row_count(regclass,text)", "index_table_row_count", "bioseq_index_table_row_count", fmgr_migration_disposition::retired_symbol},
    {"bioseq.inspect_cold_no_index_absence(regclass,text)", "inspect_cold_no_index_absence", "bioseq_inspect_cold_no_index_absence", fmgr_migration_disposition::retired_symbol},
    {"bioseq.inspect_ready_minimizer_generation(regclass,text,bigint)", "inspect_ready_minimizer_generation", "bioseq_inspect_ready_minimizer_generation", fmgr_migration_disposition::retired_symbol},
    {"bioseq.inspect_ready_minimizer_format6_generation(regclass,text,bigint)", "inspect_ready_minimizer_format6_generation", "bioseq_inspect_ready_minimizer_format6_generation", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.format6_parallel_scan_probe(regclass,text,bigint,integer)", "format6_parallel_scan_probe", "bioseq_internal_format6_parallel_scan_probe", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.build_minimizer_fixture(regclass,text,bigint,smallint,smallint,smallint,text)", "build_minimizer_fixture", "bioseq_internal_rev3_test_build_fixture", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.read_minimizer_fixture(regclass,text,bigint,smallint,smallint,smallint,text)", "read_minimizer_fixture", "bioseq_internal_rev3_test_read_fixture", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.scan_minimizer_generation_fixture(regclass,text,bigint,smallint,smallint,smallint,text)", "scan_minimizer_generation_fixture", "bioseq_internal_rev3_test_read_full_generation_fixture", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.remove_sequence_fragment(regclass,text,bigint,integer)", "remove_sequence_fragment", "bioseq_internal_rev3_test_remove_source_fragment", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.copy_primary_index_shape(regclass,regclass,text)", "copy_primary_index_shape", "bioseq_internal_rev3_test_copy_primary_index_shape", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.rewrite_source_checksum(regclass,bigint,bigint)", "rewrite_source_checksum", "bioseq_internal_rev3_test_rewrite_source_checksum", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.exercise_generation_session_failure(regclass,text,bigint,text)", "exercise_generation_session_failure", "bioseq_internal_rev3_test_session_failure", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.arm_inline_sequence_pg_failure(text)", "arm_inline_sequence_pg_failure", "bioseq_internal_arm_inline_sequence_pg_failure", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.inspect_generation_storage_transition(regclass,text,bigint,text)", "inspect_generation_storage_transition", "bioseq_internal_rev3_test_generation_storage_state", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.build_ready_mpf_generation(regclass,text,bigint,text)", "build_ready_mpf_generation", "bioseq_internal_rev3_test_build_ready_mpf_generation", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.build_ready_mps_generation(regclass,text,bigint,text)", "build_ready_mps_generation", "bioseq_internal_rev3_test_build_ready_mps_generation", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.teardown_diagnostics()", "teardown_diagnostics", "bioseq_internal_teardown_diagnostics", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.match_owner_finalization_scope_diagnostics(boolean)", "match_owner_finalization_scope_diagnostics", "bioseq_internal_match_owner_finalization_scope_diagnostics", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.batch_join_projection_guard_probe()", "batch_join_projection_guard_probe", "bioseq_internal_batch_join_projection_guard_probe", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.match_helper_wrapper_propagation_probe()", "match_helper_wrapper_propagation_probe", "bioseq_internal_match_helper_wrapper_propagation_probe", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.base_match_executor_translation_probe()", "base_match_executor_translation_probe", "bioseq_internal_base_match_executor_translation_probe", fmgr_migration_disposition::retired_symbol},
    {"bioseq.index_distinct_sub_keys(regclass,text,bigint)", "index_distinct_sub_keys", "bioseq_index_distinct_sub_keys", fmgr_migration_disposition::retired_symbol},
    {"bioseq.column_row_number(regclass,tid)", "column_row_number", "bioseq_column_row_number", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.write_index_chunk(regclass,text,bigint,bigint,int,bytea)", "write_index_chunk", "bioseq_test_write_index_chunk", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.delete_index_manifest(regclass,text,bigint)", "delete_index_manifest", "bioseq_test_delete_index_manifest", fmgr_migration_disposition::retired_symbol},
    {"bioseq.create_sample_table(text,integer,bigint,text,bigint)", "create_sample_table", "bioseq_create_sample_table", fmgr_migration_disposition::retired_symbol},
    {"bioseq.inspect_locator(bioseq.seq)", "inspect_locator", "bioseq_locator_inspect", fmgr_migration_disposition::retired_symbol},
    {"bioseq.create_query_target_sample_table(text,integer,bigint,double precision,bigint,double precision,double precision,double precision,double precision,double precision,double precision,bigint)", "create_query_target_sample_table", "bioseq_create_query_target_sample_table", fmgr_migration_disposition::retired_symbol},
    {"bioseq_internal.highway_baseline_runnable_targets_for_test()", "highway_baseline_runnable_targets_for_test", "", fmgr_migration_disposition::native_cpp_replacement},
    {"bioseq_internal.set_highway_target_for_test(bigint)", "set_highway_target_for_test", "", fmgr_migration_disposition::native_cpp_replacement},
 }};

namespace detail {

[[nodiscard]] inline std::string_view fmgr_identity_leaf(
    std::string_view identity) {
    const std::size_t arguments = identity.find('(');
    const std::string_view qualified =
        arguments == std::string_view::npos
        ? identity
        : identity.substr(0, arguments);
    const std::size_t separator = qualified.rfind('.');
    return separator == std::string_view::npos
        ? qualified
        : qualified.substr(separator + 1);
}

[[nodiscard]] inline std::string retired_symbol_class(
    std::string_view symbol) {
    if (symbol.ends_with("_stub")) return "stub";
    if (symbol.starts_with("pg_compact_sequence_")) return "compact";
    if (symbol.starts_with("bioseq_internal_")) return "internal";
    if (symbol.starts_with("bioseq_")) return "bioseq";
    return "other";
}

[[nodiscard]] inline std::string canonical_fmgr_identity(
    std::string_view identity) {
    std::string result;
    result.reserve(identity.size());
    for (std::size_t offset = 0; offset < identity.size();) {
        const unsigned char character =
            static_cast<unsigned char>(identity[offset]);
        if (!std::isalnum(character) && identity[offset] != '_') {
            result.push_back(identity[offset++]);
            continue;
        }
        const std::size_t begin = offset++;
        while (offset < identity.size()) {
            const unsigned char next =
                static_cast<unsigned char>(identity[offset]);
            if (!std::isalnum(next) && identity[offset] != '_') {
                break;
            }
            ++offset;
        }
        const std::string_view token =
            identity.substr(begin, offset - begin);
        if (token == "int2") {
            result += "smallint";
        } else if (token == "int4" || token == "int") {
            result += "integer";
        } else if (token == "int8") {
            result += "bigint";
        } else if (token == "bool") {
            result += "boolean";
        } else {
            result += token;
        }
    }
    return result;
}

}  // namespace detail

inline void validate_doc0388_fmgr_migration_ledger() {
    std::set<std::string_view, std::less<>> identities;
    std::map<std::string_view, std::size_t, std::less<>> target_counts;
    std::set<std::string_view, std::less<>> retired;
    std::set<std::string_view, std::less<>> shared_split_symbols;
    std::map<std::string, std::size_t, std::less<>> retired_classes;
    std::size_t unchanged = 0;
    std::size_t repaired = 0;
    std::size_t native = 0;
    std::size_t shared_splits = 0;

    for (const fmgr_migration_entry& entry :
         doc0388_fmgr_migration_ledger) {
        if (entry.sql_identity.empty()
            || entry.target_symbol.empty()
            || detail::fmgr_identity_leaf(entry.sql_identity)
                != entry.target_symbol
            || !identities.insert(entry.sql_identity).second) {
            throw std::runtime_error(
                "DOC0388 fmgr migration ledger has an invalid or duplicate "
                "target identity");
        }
        ++target_counts[entry.target_symbol];
        switch (entry.disposition) {
            case fmgr_migration_disposition::unchanged:
                if (entry.legacy_symbol != entry.target_symbol) {
                    throw std::runtime_error(
                        "DOC0388 unchanged fmgr row changed its symbol");
                }
                ++unchanged;
                break;
            case fmgr_migration_disposition::retired_symbol:
                if (entry.legacy_symbol.empty()
                    || entry.legacy_symbol == entry.target_symbol
                    || !retired.insert(entry.legacy_symbol).second) {
                    throw std::runtime_error(
                        "DOC0388 retired fmgr symbol is empty, unchanged, "
                        "or duplicated");
                }
                ++retired_classes[
                    detail::retired_symbol_class(entry.legacy_symbol)];
                break;
            case fmgr_migration_disposition::shared_symbol_split:
                if (entry.legacy_symbol.empty()
                    || entry.legacy_symbol == entry.target_symbol
                    || !shared_split_symbols
                            .insert(entry.legacy_symbol)
                            .second) {
                    throw std::runtime_error(
                        "DOC0388 shared-symbol split is empty or "
                        "unchanged, or duplicated");
                }
                ++shared_splits;
                break;
            case fmgr_migration_disposition::bpchar_identity_repair:
                if (!entry.target_symbol.starts_with("vbpchar")
                    || !entry.legacy_symbol.starts_with("vchar")) {
                    throw std::runtime_error(
                        "DOC0388 BPCHAR repair row has the wrong symbols");
                }
                ++repaired;
                break;
            case fmgr_migration_disposition::native_cpp_replacement:
                if (!entry.legacy_symbol.empty()) {
                    throw std::runtime_error(
                        "DOC0388 native C++ replacement has a legacy C "
                        "symbol");
                }
                ++native;
                break;
        }
    }

    std::set<std::string_view, std::less<>> shared;
    for (const auto& [symbol, count] : target_counts) {
        if (count > 1) shared.insert(symbol);
        if (count > 2) {
            throw std::runtime_error(
                "DOC0388 fmgr target symbol has more than two identities");
        }
    }
    const std::set<std::string_view, std::less<>> expected_shared{
        "from_array", "mmsearch"};
    const std::set<std::string_view, std::less<>> external_symbols{
        doc0388_external_fmgr_symbols.begin(),
        doc0388_external_fmgr_symbols.end()};
    const std::set<std::string_view, std::less<>>
        expected_external_symbols{
            "bioseq_external_handler",
        };
    const bool retired_symbol_reused = std::ranges::any_of(
        retired,
        [&target_counts, &external_symbols](
            std::string_view symbol) {
            return target_counts.contains(symbol)
                || external_symbols.contains(symbol);
        });
    const bool shared_split_target_missing = std::ranges::any_of(
        shared_split_symbols,
        [&target_counts](std::string_view symbol) {
            return !target_counts.contains(symbol);
        });

    if (identities.size() != 272
        || target_counts.size() != 270
        || shared != expected_shared
        || external_symbols != expected_external_symbols
        || retired_symbol_reused
        || shared_split_target_missing
        || retired.size() != 117
        || unchanged != 144
        || shared_splits != 1
        || repaired != 6
        || native != 4
        || retired_classes["bioseq"] != 37
        || retired_classes["internal"] != 33
        || retired_classes["stub"] != 21
        || retired_classes["compact"] != 16
        || retired_classes["other"] != 10) {
        throw std::runtime_error(
            "DOC0388 fmgr migration ledger cardinalities are stale");
    }
}

[[nodiscard]] inline std::vector<std::string>
doc0388_retired_c_symbols() {
    validate_doc0388_fmgr_migration_ledger();
    std::vector<std::string> result;
    result.reserve(117);
    for (const fmgr_migration_entry& entry :
         doc0388_fmgr_migration_ledger) {
        if (entry.disposition
            == fmgr_migration_disposition::retired_symbol) {
            result.emplace_back(entry.legacy_symbol);
        }
    }
    std::ranges::sort(result);
    return result;
}

inline void validate_doc0388_fmgr_target(
    std::span<const std::string> identities,
    std::span<const std::string> symbols) {
    validate_doc0388_fmgr_migration_ledger();

    std::set<std::string, std::less<>> expected_identities;
    std::set<std::string, std::less<>> expected_symbols;
    for (const fmgr_migration_entry& entry :
         doc0388_fmgr_migration_ledger) {
        expected_identities.insert(
            detail::canonical_fmgr_identity(entry.sql_identity));
        expected_symbols.emplace(entry.target_symbol);
    }
    std::set<std::string, std::less<>> actual_identities;
    std::set<std::string, std::less<>> actual_symbols;
    for (const std::string& identity : identities) {
        actual_identities.insert(
            detail::canonical_fmgr_identity(identity));
    }
    for (const std::string& symbol : symbols) {
        actual_symbols.insert(symbol);
    }

    if (actual_identities.size() != identities.size()
        || actual_symbols.size() != symbols.size()
        || !std::ranges::equal(
            expected_identities, actual_identities)
        || !std::ranges::equal(
            expected_symbols, actual_symbols)) {
        const auto append_set_difference =
            [](std::string& message,
               std::string_view label,
               const auto& left,
               const auto& right) {
                std::vector<std::string> difference;
                std::ranges::set_difference(
                    left, right, std::back_inserter(difference));
                if (difference.empty()) return;
                message += "; ";
                message += label;
                message += "=[";
                for (std::size_t index = 0;
                     index < difference.size();
                     ++index) {
                    if (index != 0) message += ", ";
                    message += difference[index];
                }
                message += "]";
            };
        std::string message =
            "generated SQL C-routine closure differs from the reviewed "
            "DOC0388 fmgr migration ledger; identities expected="
            + std::to_string(expected_identities.size())
            + " actual=" + std::to_string(actual_identities.size())
            + " (input=" + std::to_string(identities.size()) + ")"
            + "; symbols expected="
            + std::to_string(expected_symbols.size())
            + " actual=" + std::to_string(actual_symbols.size())
            + " (input=" + std::to_string(symbols.size()) + ")";
        append_set_difference(
            message,
            "missing identities",
            expected_identities,
            actual_identities);
        append_set_difference(
            message,
            "unexpected identities",
            actual_identities,
            expected_identities);
        append_set_difference(
            message,
            "missing symbols",
            expected_symbols,
            actual_symbols);
        append_set_difference(
            message,
            "unexpected symbols",
            actual_symbols,
            expected_symbols);
        throw std::runtime_error(
            message);
    }
}

}  // namespace bioseq::codegen::safety
