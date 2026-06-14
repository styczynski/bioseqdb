/*-------------------------------------------------------------------------
 *
 * types/pg_compact_sequence.cppm
 *
 * Postgres custom type `bioseq.seq(alphabet, width)` — a bit-packed
 * sequence datatype with lexicographic btree comparator, round-trippable
 * text I/O, binary wire format, and 1-arg projection helpers
 * (bioseq.len, bioseq.unit_len).
 *
 * Design notes (DOC0258): this translation unit no longer carries any
 * sequence-layout code. The struct, dispatch helpers, factories, and
 * range view all live in `bioseq_sequences:sequences`. This file
 * provides only:
 *   - the `pgseq_t` alias (= compact_sequence_stdc<…>)
 *   - the three postgres allocator/reader/writer lambdas
 *   - the `pg_seq_arg<S>` argument extractor
 *   - thin SQL entry points that bind the above
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

extern "C" {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wregister"
#include "libpq/pqformat.h"
#pragma GCC diagnostic pop
}

#include <bit>
#include <cerrno>
#include <compare>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

export module bioseq_types.pg_compact_sequence;

import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_sequences.simd;
import pg_utils;

extern "C++" {

export constexpr int32_t PG_SEQ_TYPMOD_UNCONSTRAINED = -1;

// ----------------------------------------------------------------------------
// pgseq_t — the postgres-specific instantiation of compact_sequence_stdc.
// The supported_alphabets pack is preserved verbatim from the legacy
// dispatch_pgseq ladder. Adding a new alphabet is a one-token edit here.
// ----------------------------------------------------------------------------

export using pgseq_supported_alphabets =
    supported_alphabets<
        alphabet_entry<seq::dna4, "dna4">,
        alphabet_entry<seq::dna5, "dna5">>;
export using pgseq_t =
    compact_sequence_stdc<pgseq_supported_alphabets>;

// ============================================================================
// DRIFT GUARDS — DOC0261 R6 / R6.1.
//
// The static_asserts below are intentional duplicates of the production
// pack's display-name strings. They exist to make any future drift between
// the test-side name fixtures (in `integration_tests/common.cppm`'s
// `Dna4Entry`/`Dna5Entry` tag structs) and the production pack fail at
// build time — not at integration-test runtime. The literal strings here
// must NOT be edited independently; if the pack ever grows or changes
// a display name, update the pack first, then update the matching guard
// here AND the `Dna4Entry`/`Dna5Entry` tag structs in lockstep.
// ============================================================================

// DOC0261 R6.1 — the generated CSV must be byte-identical to the
// historical hand-written errhint.
static_assert(pgseq_supported_alphabets::valid_alphabet_names_csv()
              == std::string_view{"'dna4', 'dna5'"},
              "valid_alphabet_names_csv must match today's hand-written hint "
              "byte-for-byte — DOC0261 R6.1");

// DOC0261 §11 — integration-test target compiles outside this TU's
// link domain (libpq-driven, no Postgres-extension symbols available),
// so the integration_tests.common module exposes its own
// `Dna4Entry`/`Dna5Entry` tag aliases.
static_assert(
    pgseq_supported_alphabets::entry_for<seq::dna4>::alphabet_name_sv ==
        std::string_view{"dna4"},
    "integration_tests::Dna4Entry::alphabet_name_sv drifted from the "
    "production pack — DOC0261 R6 source-of-truth promise");
static_assert(
    pgseq_supported_alphabets::entry_for<seq::dna5>::alphabet_name_sv ==
        std::string_view{"dna5"},
    "integration_tests::Dna5Entry::alphabet_name_sv drifted from the "
    "production pack — DOC0261 R6 source-of-truth promise");

// ----------------------------------------------------------------------------
// pg_alloc — the postgres-specific allocator lambda. Returns nullptr for
// BOTH oversize and OOM; never `ereport`s. This is the load-bearing
// contract that makes the alloc safe to call from inside a C++ frame
// that owns transient destructors (DOC0258 §13.1.1). Each SQL entry
// point checks for nullptr after the factory returns and `ereport`s
// from a frame where no destructors are pending.
// ----------------------------------------------------------------------------

export inline constexpr auto pg_alloc =
    [](std::size_t byte_count) -> pgseq_t* {
        if (byte_count > static_cast<std::size_t>(MaxAllocSize)) {
            return nullptr;
        }
        // The factory body (`stdc_convert_from_cstr`, `convert_from(recv)`,
        // `reserve`, …) is responsible for writing `vl_len_` to the same
        // `byte_count` we asked for. We deliberately do NOT call
        // `SET_VARSIZE` here — the factory's write is the authoritative
        // one and pre-setting would be redundant.
        return static_cast<pgseq_t*>(
            palloc_extended(byte_count,
                            MCXT_ALLOC_NO_OOM | MCXT_ALLOC_ZERO));
    };

static_assert(stdc_allocator<decltype(pg_alloc), pgseq_t>,
              "pg_alloc must satisfy stdc_allocator for pgseq_t");

// ----------------------------------------------------------------------------
// pg_recv_byte_reader — templated lambda for `convert_from(recv)`. Reads
// one storage unit of the recorded byte width from a StringInfoData
// buffer in network byte order.
// ----------------------------------------------------------------------------

export inline constexpr auto pg_recv_byte_reader =
    []<class Alphabet, class Out>(
        StringInfoData& buf, std::uint8_t byte_width) -> Out {
        switch (byte_width) {
            case 1: return static_cast<Out>(pq_getmsgint(&buf, 1));
            case 2: return static_cast<Out>(pq_getmsgint(&buf, 2));
            case 4: return static_cast<Out>(pq_getmsgint(&buf, 4));
            case 8: return static_cast<Out>(pq_getmsgint64(&buf));
        }
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                 errmsg("pg_recv_byte_reader: invalid byte_width %u",
                        static_cast<unsigned>(byte_width))));
        __builtin_unreachable();
    };

// ----------------------------------------------------------------------------
// pg_send_byte_writer — templated lambda for `send_to`. Writes one
// storage unit of the recorded byte width to a StringInfo buffer in
// network byte order.
// ----------------------------------------------------------------------------

export inline constexpr auto pg_send_byte_writer =
    []<class In>(StringInfoData& buf, std::uint64_t unit,
                 std::uint8_t byte_width) {
        switch (byte_width) {
            case 1: pq_sendint8(&buf, static_cast<std::uint8_t>(unit)); return;
            case 2: pq_sendint16(&buf, static_cast<std::uint16_t>(unit)); return;
            case 4: pq_sendint32(&buf, static_cast<std::uint32_t>(unit)); return;
            case 8: pq_sendint64(&buf, static_cast<std::uint64_t>(unit)); return;
        }
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                 errmsg("pg_send_byte_writer: invalid byte_width %u",
                        static_cast<unsigned>(byte_width))));
    };

// ----------------------------------------------------------------------------
// pg_seq_arg — argument extractor that detoasts and reinterprets an
// argument as `S*` (defaults to pgseq_t*). Replaces the
// DatumGetPgCompactSequence / PG_GETARG_PGCOMPACTSEQUENCE_P macros.
// ----------------------------------------------------------------------------

export template<class S = pgseq_t>
[[nodiscard]] inline S* pg_seq_arg(FunctionCallInfo fcinfo, int n) {
    return reinterpret_cast<S*>(PG_DETOAST_DATUM(PG_GETARG_DATUM(n)));
}

} // extern "C++"

// ============================================================================
// SQL-callable Datum functions. All use CXX_PG_FUNCTION_INFO_V1 which handles
// the V1 fmgr record declaration with correct extern "C" linkage.
// ============================================================================

extern "C" {

/*
 * bioseq.seq_typmod_in: parse ('alphabet_name', width_bits) → packed int32.
 * Receives cstring[] from Postgres; rejects unknown alphabet / unsupported
 * width with ereport(ERROR, ERRCODE_INVALID_PARAMETER_VALUE).
 */
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_typmod_in);
Datum pg_compact_sequence_typmod_in(PG_FUNCTION_ARGS)
{
	ArrayType* arg_array = PG_GETARG_ARRAYTYPE_P(0);

	if (ARR_NDIM(arg_array) != 1)
		ereport(ERROR,
		        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		         errmsg("bioseq.seq typmod must be a one-dimensional argument list")));

	if (ARR_HASNULL(arg_array) && array_contains_nulls(arg_array))
		ereport(ERROR,
		        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
		         errmsg("bioseq.seq typmod arguments must not be NULL")));

	Datum* elem_datums = nullptr;
	bool*  elem_nulls  = nullptr;
	int    n_elems     = 0;
	// CSTRING: typlen=-2, typbyval=false, typalign='c'.
	deconstruct_array(arg_array, CSTRINGOID, -2, false, 'c',
	                  &elem_datums, &elem_nulls, &n_elems);

	if (n_elems != 2)
		ereport(ERROR,
		        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		         errmsg("bioseq.seq takes exactly 2 type modifiers (alphabet name, storage width); got %d", n_elems),
		         errhint("Example: bioseq.seq('dna4', 64)")));

	const char* alphabet_name = DatumGetCString(elem_datums[0]);
	const char* width_str     = DatumGetCString(elem_datums[1]);

	// DOC0261 §8.1 — the strcmp ladder collapses to a single pack
	// lookup. Adding a new alphabet to the pack adds zero lines here,
	// and the `errhint` text is regenerated from the same pack so
	// adding an alphabet also auto-updates the error message.
	const auto code_opt =
		pgseq_supported_alphabets::try_index_of_name(alphabet_name);
	if (!code_opt.has_value()) {
		constexpr auto valid_names_csv =
			pgseq_supported_alphabets::valid_alphabet_names_csv();
		ereport(ERROR,
		        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		         errmsg("bioseq.seq alphabet \"%s\" is not supported", alphabet_name),
		         errhint("Valid alphabets: %.*s",
		             static_cast<int>(valid_names_csv.size()),
		             valid_names_csv.data())));
	}
	const uint8_t alphabet_code = static_cast<uint8_t>(*code_opt);

	char* endptr = nullptr;
	errno = 0;
	const long width_long = strtol(width_str, &endptr, 10);
	if (endptr == width_str || *endptr != '\0' || errno != 0
	    || (width_long != 8 && width_long != 16 && width_long != 32 && width_long != 64))
		ereport(ERROR,
		        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		         errmsg("bioseq.seq width \"%s\" is not supported", width_str),
		         errhint("Supported widths: 8, 16, 32, 64")));

	const uint8_t byte_width = static_cast<uint8_t>(width_long / 8);
	const int32_t packed = compact_sequence_stdc_type_params{
		.alphabet_code           = alphabet_code,
		.storage_unit_byte_width = byte_width,
	}.encode();
	bioseq_log<DBG>("[bioseq] pg: seq_typmod_in alphabet='{}' width={} -> packed={}",
	        alphabet_name, width_long, packed);
	PG_RETURN_INT32(packed);
}

/*
 * bioseq.seq_typmod_out: render packed int32 as "('dna4',64)".
 * Returning "" signals "no typmod suffix" (used for typmod = -1).
 */
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_typmod_out);
Datum pg_compact_sequence_typmod_out(PG_FUNCTION_ARGS)
{
	const int32 typmod = PG_GETARG_INT32(0);
	if (typmod < 0)
		PG_RETURN_CSTRING(pstrdup(""));

	const auto tp = compact_sequence_stdc_type_params::decode(typmod);
	const std::string_view alphabet_name =
		pgseq_supported_alphabets::name_of_alphabet_code(tp.alphabet_code);
	const unsigned width_bits =
		static_cast<unsigned>(tp.storage_unit_byte_width) * 8u;

	char* result = static_cast<char*>(palloc(64));
	snprintf(result, 64, "('%.*s',%u)",
	         static_cast<int>(alphabet_name.size()), alphabet_name.data(),
	         width_bits);
	PG_RETURN_CSTRING(result);
}

/*
 * bioseq.seq_in: cstring → varlena. Typmod is mandatory (supplied by the
 * column descriptor). Encoding uses convert_from(cstr, …, pg_alloc) which
 * routes through encode_sequence_simd internally.
 */
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_in);
Datum pg_compact_sequence_in(PG_FUNCTION_ARGS)
{
	const char* lit    = PG_GETARG_CSTRING(0);
	const int32 typmod = PG_GETARG_INT32(2);

	compact_sequence_stdc_type_params tp;
	if (typmod < 0) {
		// Postgres passes typmod = -1 for free-standing literals (the
		// cast target's typmod is applied later via bioseq.seq_cast).
		// Default to dna5/uint8 because dna5 is a superset of dna4.
		tp = {
			.alphabet_code = pgseq_supported_alphabets::index_of<seq::dna5>(),
			.storage_unit_byte_width = 1u,
		};
	} else {
		tp = compact_sequence_stdc_type_params::decode(typmod);
	}
	const std::size_t length = std::strlen(lit);

	// Oversize check before the factory call so we can give a precise
	// ERRCODE_PROGRAM_LIMIT_EXCEEDED on inputs that would never fit;
	// otherwise pg_alloc returning nullptr is ambiguous (could be OOM).
	const std::size_t upper_bound_bytes =
		pgseq_t::header_bytes() + 8u
		+ static_cast<std::size_t>(length) * 8u;
	if (upper_bound_bytes > static_cast<std::size_t>(MaxAllocSize))
		ereport(ERROR,
		        (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
		         errmsg("bioseq.seq input length %zu exceeds MaxAllocSize",
		                length)));

	pgseq_t* hdr = nullptr;
	pgseq_supported_alphabets::dispatch_alphabet_by_code(tp.alphabet_code,
		[&]<class A>() {
			hdr = ::bioseq::simd::N_NEON::stdc_convert_from_cstr<pgseq_t, A>(
				tp.storage_unit_byte_width,
				static_cast<pgseq_t::bp_size_t>(length),
				lit,
				pg_alloc);
		});
	if (hdr == nullptr)
		ereport(ERROR,
		        (errcode(ERRCODE_OUT_OF_MEMORY),
		         errmsg("out of memory in bioseq.seq_in")));

	bioseq_log<DBG>("[bioseq] pg: seq_in alphabet_code={} byte_width={} length={} -> bp_size={}",
	        tp.alphabet_code, tp.storage_unit_byte_width, length,
	        static_cast<unsigned>(hdr->bp_size));
	PG_RETURN_POINTER(hdr);
}

/*
 * bioseq.seq_out: varlena → cstring. Uses convert_to to dispatch to
 * decode_sequence_simd over the in-place range view.
 */
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_out);
Datum pg_compact_sequence_out(PG_FUNCTION_ARGS)
{
	pgseq_t* hdr = pg_seq_arg<pgseq_t>(fcinfo, 0);

	std::string decoded;
	hdr->convert_to([&]<class S>(const compact_sequence_range<S>& seq) {
		decoded = ::bioseq::simd::N_NEON::decode_sequence_simd<S>(seq);
	});

	char* out = static_cast<char*>(palloc(decoded.size() + 1));
	std::memcpy(out, decoded.data(), decoded.size());
	out[decoded.size()] = '\0';
	bioseq_log<DBG>("[bioseq] pg: seq_out bp_size={} -> '{}'",
	        static_cast<unsigned>(hdr->bp_size), decoded);
	PG_FREE_IF_COPY(hdr, 0);
	PG_RETURN_CSTRING(out);
}

/*
 * bioseq.seq_cast(bioseq.seq, typmod, explicit) — applied by Postgres
 * when the source typmod differs from the target typmod. Wraps
 * recode_to_type_params which picks the same-alphabet fast path
 * (memcpy + padding recompute) versus the cross-alphabet ASCII round
 * trip.
 */
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_cast);
Datum pg_compact_sequence_cast(PG_FUNCTION_ARGS)
{
	pgseq_t*    src           = pg_seq_arg<pgseq_t>(fcinfo, 0);
	const int32 target_typmod = PG_GETARG_INT32(1);

	if (target_typmod < 0)
		PG_RETURN_POINTER(src);

	const auto target = compact_sequence_stdc_type_params::decode(target_typmod);
	if (src->get_type_params() == target)
		PG_RETURN_POINTER(src);

	pgseq_t* result =
		::bioseq::simd::N_NEON::stdc_recode_to_type_params(*src, target, pg_alloc);
	if (result == nullptr)
		ereport(ERROR,
		        (errcode(ERRCODE_OUT_OF_MEMORY),
		         errmsg("out of memory in bioseq.seq_cast")));

	bioseq_log<DBG>("[bioseq] pg: seq_cast ({},{}) -> ({},{}) bp_size={}",
	        static_cast<unsigned>(src->alphabet_code),
	        static_cast<unsigned>(src->storage_unit_byte_width),
	        static_cast<unsigned>(target.alphabet_code),
	        static_cast<unsigned>(target.storage_unit_byte_width),
	        static_cast<unsigned>(result->bp_size));
	PG_FREE_IF_COPY(src, 0);
	PG_RETURN_POINTER(result);
}

/*
 * bioseq.seq_send: varlena → bytea (binary wire format).
 *
 * Wire layout (preserved verbatim from the legacy implementation —
 * the storage width is encoded as the LOG2 width_code 0..3, not the
 * raw byte count, for backwards binary-COPY compatibility):
 *   offset size  field
 *     0     1   alphabet_code
 *     1     1   width_code = log2(storage_unit_byte_width)
 *     2     2   padding_letters  (uint16, network byte order)
 *     4     4   bp_size          (uint32, NBO)
 *     8     4   unit_count       (uint32, NBO)
 *    12     *   unit_count storage units in NBO at the recorded width
 */
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_send);
Datum pg_compact_sequence_send(PG_FUNCTION_ARGS)
{
	pgseq_t* hdr = pg_seq_arg<pgseq_t>(fcinfo, 0);
	StringInfoData buf;
	pq_begintypsend(&buf);

	const uint8_t width_code =
		  hdr->storage_unit_byte_width == 1 ? 0u
		: hdr->storage_unit_byte_width == 2 ? 1u
		: hdr->storage_unit_byte_width == 4 ? 2u
		: 3u;

	pq_sendint8(&buf, hdr->alphabet_code);
	pq_sendint8(&buf, width_code);
	pq_sendint16(&buf, hdr->padding_letters);
	pq_sendint32(&buf, hdr->bp_size);
	pq_sendint32(&buf, static_cast<uint32>(hdr->unit_count()));

	hdr->send_to(buf, pg_send_byte_writer);

	PG_FREE_IF_COPY(hdr, 0);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * bioseq.seq_recv: wire format → varlena.
 */
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_recv);
Datum pg_compact_sequence_recv(PG_FUNCTION_ARGS)
{
	StringInfo  buf    = reinterpret_cast<StringInfo>(PG_GETARG_POINTER(0));
	const int32 typmod = PG_GETARG_INT32(2);

	const uint8_t alphabet_code = static_cast<uint8_t>(pq_getmsgint(buf, 1));
	const uint8_t width_code    = static_cast<uint8_t>(pq_getmsgint(buf, 1));
	if (alphabet_code >= pgseq_supported_alphabets::alphabet_count
	    || width_code > 3u)
		ereport(ERROR,
		        (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
		         errmsg("bioseq.seq binary: invalid alphabet_code=%u or width_code=%u",
		                static_cast<unsigned>(alphabet_code),
		                static_cast<unsigned>(width_code))));
	const uint8_t byte_width = static_cast<uint8_t>(1u << width_code);

	const uint16_t padding_letters = static_cast<uint16_t>(pq_getmsgint(buf, 2));
	const uint32_t bp_size_w       = static_cast<uint32_t>(pq_getmsgint(buf, 4));
	const uint32_t unit_count_w    = static_cast<uint32_t>(pq_getmsgint(buf, 4));

	if (typmod >= 0) {
		const auto t = compact_sequence_stdc_type_params::decode(typmod);
		if (t.alphabet_code != alphabet_code
		    || t.storage_unit_byte_width != byte_width)
			ereport(ERROR,
			        (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
			         errmsg("bioseq.seq binary: typmod (%u,%u) does not match wire (%u,%u)",
			                static_cast<unsigned>(t.alphabet_code),
			                static_cast<unsigned>(t.storage_unit_byte_width),
			                static_cast<unsigned>(alphabet_code),
			                static_cast<unsigned>(byte_width))));
	}

	// DoS guard — match the legacy "we have enough bytes left" check.
	const uint64_t required = static_cast<uint64_t>(unit_count_w) * byte_width;
	const uint64_t available = static_cast<uint64_t>(buf->len - buf->cursor);
	if (required > available)
		ereport(ERROR,
		        (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
		         errmsg("bioseq.seq binary: unit_count=%u width=%u needs %llu bytes, only %llu remain",
		                unit_count_w, static_cast<unsigned>(byte_width),
		                static_cast<unsigned long long>(required),
		                static_cast<unsigned long long>(available))));

	// Oversize check before factory call so ERRCODE_PROGRAM_LIMIT_EXCEEDED
	// stays distinct from ERRCODE_OUT_OF_MEMORY.
	const std::size_t upper_bound_bytes =
		pgseq_t::header_bytes() + 8u + static_cast<std::size_t>(required);
	if (upper_bound_bytes > static_cast<std::size_t>(MaxAllocSize))
		ereport(ERROR,
		        (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
		         errmsg("bioseq.seq binary payload %zu bytes exceeds MaxAllocSize",
		                static_cast<std::size_t>(required))));

	// Per-alphabet padding/bp_size validation — uses dispatch_instance so
	// letters_per_unit is constexpr and we don't hand-hardcode alphabets.
	bool padding_ok = false;
	std::size_t lpu = 0;
	pgseq_t::dispatch_instance(alphabet_code, byte_width,
		[&]<class S>() {
			lpu = compact_sequence_range<S>::traits::letters_per_unit;
			padding_ok = padding_letters < lpu
			    || (unit_count_w == 0 && padding_letters == 0);
		});
	if (!padding_ok)
		ereport(ERROR,
		        (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
		         errmsg("bioseq.seq binary: padding_letters=%u invalid for letters_per_unit=%zu (unit_count=%u)",
		                static_cast<unsigned>(padding_letters), lpu, unit_count_w)));

	const uint64_t derived_bp = (unit_count_w == 0)
		? uint64_t{0}
		: (static_cast<uint64_t>(unit_count_w) * lpu - padding_letters);
	if (derived_bp != bp_size_w)
		ereport(ERROR,
		        (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
		         errmsg("bioseq.seq binary: bp_size=%u inconsistent with derived=%llu (unit_count=%u, padding=%u)",
		                bp_size_w, static_cast<unsigned long long>(derived_bp),
		                unit_count_w, static_cast<unsigned>(padding_letters))));

	pgseq_t* hdr = nullptr;
	pgseq_supported_alphabets::dispatch_alphabet_by_code(alphabet_code,
		[&]<class A>() {
			hdr = pgseq_t::convert_from<A>(
				byte_width,
				padding_letters,
				static_cast<pgseq_t::bp_size_t>(bp_size_w),
				unit_count_w,
				*buf,
				pg_recv_byte_reader,
				pg_alloc);
		});
	if (hdr == nullptr)
		ereport(ERROR,
		        (errcode(ERRCODE_OUT_OF_MEMORY),
		         errmsg("out of memory in bioseq.seq_recv")));

	pq_getmsgend(buf);
	PG_RETURN_POINTER(hdr);
}

} // extern "C"

namespace {

// DOC0260 §5.2 — locator stub marker (bit 7 of storage_unit_byte_width).
// Replicated inline here so pg_compact_sequence does not need to import
// bioseq_storage (which would close a module-graph cycle with
// bioseq_table:access_method). The discriminator and the
// `total_bp_size` field offset inside the stub's data[] match the
// canonical definition in `bucket_math.cppm` (the
// `static_assert(offsetof(seq_locator, total_bp_size) == 16)` there is
// the source of truth — if it ever fires, update this offset in
// lockstep).
//
// DOC0264 §5 — Phase C v2 locator layout (32 bytes):
//   offset 0  version          (1 byte)
//   offset 1  bucket_idx       (1 byte)
//   offset 2  _pad_2           (2 bytes)
//   offset 4  shard_idx        (4 bytes)
//   offset 8  num_fragments    (4 bytes)
//   offset 12 _pad_4           (4 bytes)
//   offset 16 total_bp_size    (8 bytes)
//   offset 24 first_sub_row_no (8 bytes)
constexpr std::uint8_t kLocatorStubMarkerLocal = 0x80;
constexpr std::size_t  kLocatorTotalBpSizeOffset = 16;

[[nodiscard]] [[gnu::always_inline]]
inline bool pgseq_is_locator_stub(const pgseq_t* hdr) noexcept {
	return hdr != nullptr
	    && (static_cast<std::uint8_t>(hdr->storage_unit_byte_width)
	        & kLocatorStubMarkerLocal) != 0u;
}

[[nodiscard]] [[gnu::always_inline]]
inline std::uint64_t pgseq_locator_total_bp_size(const pgseq_t* hdr) noexcept {
	std::uint64_t out = 0;
	std::memcpy(&out,
	            reinterpret_cast<const char*>(hdr->data)
	                + kLocatorTotalBpSizeOffset,
	            sizeof(out));
	return out;
}

[[nodiscard]] inline int pg_compact_sequence_cmp_internal(
	const pgseq_t* a, const pgseq_t* b)
{
	// DOC0260 §5.5 / R12 — when either operand is a locator stub the real
	// payload lives in a sub-table; comparing the stub bytes via
	// `stdc_compare` would dereference the encoded pointer fields as if
	// they were bit-packed letters and crash. The columnar writer's
	// chunk-skip min/max computation (`UpdateChunkSkipNodeMinMax`) IS
	// invoked on stubs because the main-relation rows hold stubs in place
	// of the payload (R11 keeps the slot's tts_values pointing at the
	// original sequence for `ExecInsertIndexTuples`, but
	// `BioseqColumnWriteRow` itself receives the stub via `values_for_write`).
	// Provide a stub-safe ordering keyed on the encoded `total_bp_size`
	// (the only stub field with a meaningful, monotone value). Real-real
	// comparisons still go through `stdc_compare`; stub-vs-real is a
	// rare cross-shape case that we serialise by length so the chunk-skip
	// stats stay self-consistent.
	const bool a_stub = pgseq_is_locator_stub(a);
	const bool b_stub = pgseq_is_locator_stub(b);
	if (a_stub || b_stub) {
		const std::uint64_t la = a_stub ? pgseq_locator_total_bp_size(a)
		                                : static_cast<std::uint64_t>(a->bp_size);
		const std::uint64_t lb = b_stub ? pgseq_locator_total_bp_size(b)
		                                : static_cast<std::uint64_t>(b->bp_size);
		if (la < lb) return -1;
		if (la > lb) return  1;
		return 0;
	}

	if (a->get_type_params() != b->get_type_params()
	    && a->alphabet_code != b->alphabet_code)
		ereport(ERROR,
		        (errcode(ERRCODE_DATATYPE_MISMATCH),
		         errmsg("bioseq.seq cmp: mismatched alphabets (lhs=%u, rhs=%u)",
		                static_cast<unsigned>(a->alphabet_code),
		                static_cast<unsigned>(b->alphabet_code))));

	const auto ord = ::bioseq::simd::N_NEON::stdc_compare(*a, *b);
	if (ord == std::strong_ordering::less)    return -1;
	if (ord == std::strong_ordering::greater) return  1;
	return 0;
}
} // anonymous namespace

extern "C" {

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_cmp);
Datum pg_compact_sequence_cmp(PG_FUNCTION_ARGS)
{
	pgseq_t* a = pg_seq_arg<pgseq_t>(fcinfo, 0);
	pgseq_t* b = pg_seq_arg<pgseq_t>(fcinfo, 1);
	const int32 result = pg_compact_sequence_cmp_internal(a, b);
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_INT32(result);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_lt);
Datum pg_compact_sequence_lt(PG_FUNCTION_ARGS)
{
	pgseq_t* a = pg_seq_arg<pgseq_t>(fcinfo, 0);
	pgseq_t* b = pg_seq_arg<pgseq_t>(fcinfo, 1);
	const bool result = pg_compact_sequence_cmp_internal(a, b) < 0;
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_BOOL(result);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_le);
Datum pg_compact_sequence_le(PG_FUNCTION_ARGS)
{
	pgseq_t* a = pg_seq_arg<pgseq_t>(fcinfo, 0);
	pgseq_t* b = pg_seq_arg<pgseq_t>(fcinfo, 1);
	const bool result = pg_compact_sequence_cmp_internal(a, b) <= 0;
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_BOOL(result);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_eq);
Datum pg_compact_sequence_eq(PG_FUNCTION_ARGS)
{
	pgseq_t* a = pg_seq_arg<pgseq_t>(fcinfo, 0);
	pgseq_t* b = pg_seq_arg<pgseq_t>(fcinfo, 1);
	const bool result = pg_compact_sequence_cmp_internal(a, b) == 0;
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_BOOL(result);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_ne);
Datum pg_compact_sequence_ne(PG_FUNCTION_ARGS)
{
	pgseq_t* a = pg_seq_arg<pgseq_t>(fcinfo, 0);
	pgseq_t* b = pg_seq_arg<pgseq_t>(fcinfo, 1);
	const bool result = pg_compact_sequence_cmp_internal(a, b) != 0;
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_BOOL(result);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_ge);
Datum pg_compact_sequence_ge(PG_FUNCTION_ARGS)
{
	pgseq_t* a = pg_seq_arg<pgseq_t>(fcinfo, 0);
	pgseq_t* b = pg_seq_arg<pgseq_t>(fcinfo, 1);
	const bool result = pg_compact_sequence_cmp_internal(a, b) >= 0;
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_BOOL(result);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_gt);
Datum pg_compact_sequence_gt(PG_FUNCTION_ARGS)
{
	pgseq_t* a = pg_seq_arg<pgseq_t>(fcinfo, 0);
	pgseq_t* b = pg_seq_arg<pgseq_t>(fcinfo, 1);
	const bool result = pg_compact_sequence_cmp_internal(a, b) > 0;
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_BOOL(result);
}

/*
 * bioseq.len(bioseq.seq) -> int8 (BIGINT). DOC0260 §1.1 #10 — widened
 * from int4 so sequences with bp_size > INT32_MAX (Phase C and beyond)
 * are not truncated. For a locator stub we read total_bp_size from the
 * embedded seq_locator payload; for an inline sequence we read
 * hdr->bp_size as before.
 *
 * Layout (mirrored from DOC0264 §5 / seq_locator in bucket_math.cppm):
 *   offset 0  version          (1 byte)
 *   offset 1  bucket_idx       (1 byte)
 *   offset 2  _pad_2           (2 bytes)
 *   offset 4  shard_idx        (4 bytes)
 *   offset 8  num_fragments    (4 bytes)
 *   offset 12 _pad_4           (4 bytes)
 *   offset 16 total_bp_size    (8 bytes)   <-- this TU reads here
 *   offset 24 first_sub_row_no (8 bytes)
 *
 * The 16-byte offset is anchored by
 *   static_assert(offsetof(seq_locator, total_bp_size) == 16, ...)
 * in bucket_math.cppm; if the seq_locator layout ever changes, that
 * static_assert fires and forces a matching update here. We cannot
 * `import bioseq_storage` from this TU because it closes a module-graph
 * cycle through bioseq_table:access_method.
 */
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_len);
Datum pg_compact_sequence_len(PG_FUNCTION_ARGS)
{
	pgseq_t* hdr = pg_seq_arg<pgseq_t>(fcinfo, 0);
	std::int64_t bp_size = 0;
	const std::uint8_t sub_w = static_cast<std::uint8_t>(hdr->storage_unit_byte_width);
	if ((sub_w & 0x80u) != 0u) {
		std::uint64_t total = 0;
		std::memcpy(&total, hdr->data + kLocatorTotalBpSizeOffset,
		            sizeof(total));
		bp_size = static_cast<std::int64_t>(total);
	} else {
		bp_size = static_cast<std::int64_t>(hdr->bp_size);
	}
	PG_FREE_IF_COPY(hdr, 0);
	PG_RETURN_INT64(bp_size);
}

/*
 * bioseq.unit_len(bioseq.seq) -> int8 (BIGINT). For a locator stub we
 * compute the unit count from total_bp_size and the alphabet's
 * bits_per_char. The bits_per_char value is looked up at compile time
 * from `pgseq_supported_alphabets` via `dispatch_alphabet_by_code`, so
 * adding a new alphabet to the pack does NOT silently produce wrong
 * unit counts (DOC0145 §4 — static_assert / compile-time mapping
 * instead of hardcoded magic).
 */
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_unit_len);
Datum pg_compact_sequence_unit_len(PG_FUNCTION_ARGS)
{
	pgseq_t* hdr = pg_seq_arg<pgseq_t>(fcinfo, 0);
	std::int64_t units = 0;
	const std::uint8_t sub_w = static_cast<std::uint8_t>(hdr->storage_unit_byte_width);
	if ((sub_w & 0x80u) != 0u) {
		std::uint64_t total = 0;
		std::memcpy(&total, hdr->data + kLocatorTotalBpSizeOffset,
		            sizeof(total));
		const std::uint8_t logical_w =
			static_cast<std::uint8_t>(sub_w & 0x0Fu);
		const auto alphabet_code =
			static_cast<typename pgseq_t::alphabet_code_t>(hdr->alphabet_code);
		std::size_t bits_per_char = 0;
		pgseq_supported_alphabets::dispatch_alphabet_by_code(
			alphabet_code,
			[&bits_per_char]<class A>() {
				bits_per_char = static_cast<std::size_t>(
					seqan3::detail::ceil_log2(
						static_cast<std::size_t>(seqan3::alphabet_size<A>)));
			});
		const std::size_t letters_per_unit = bits_per_char > 0
			? (static_cast<std::size_t>(logical_w) * 8u) / bits_per_char
			: 0u;
		const std::size_t computed_units = letters_per_unit > 0
			? (static_cast<std::size_t>(total) + letters_per_unit - 1u) / letters_per_unit
			: 0u;
		units = static_cast<std::int64_t>(computed_units);
	} else {
		units = static_cast<std::int64_t>(hdr->unit_count());
	}
	PG_FREE_IF_COPY(hdr, 0);
	PG_RETURN_INT64(units);
}

} // extern "C"
