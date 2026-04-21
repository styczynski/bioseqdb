/*-------------------------------------------------------------------------
 *
 * types/pg_compact_sequence.cppm
 *
 * Postgres custom type `bioseq.seq(alphabet, width)` — a bit-packed
 * sequence datatype with lexicographic btree comparator, round-trippable
 * text I/O, binary wire format, and 1-arg projection helpers
 * (bioseq.len, bioseq.unit_len).
 *
 * Design notes: see main/bioseq/RESEARCH/SPEC_HOW_TO_CONNECT_ALIGNER_3.md
 *
 *-------------------------------------------------------------------------
 */
module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

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

export module bioseq_types.pg_compact_sequence;

import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_sequences.simd;
import pg_utils;

extern "C++" {

// ----------------------------------------------------------------------------
// Typmod packing/unpacking.
//
// Packed int32 layout:
//   bits  0..7   width_code  (0 -> 8, 1 -> 16, 2 -> 32, 3 -> 64 bit storage)
//   bits  8..15  alphabet_code (0 -> dna4, 1 -> dna5)
//   bits 16..31  reserved, must be zero
//
// -1 signals "unconstrained" (no typmod). All valid packed values are in
// [0, 512), keeping the signed int non-negative.
// ----------------------------------------------------------------------------

constexpr int32_t PG_SEQ_TYPMOD_UNCONSTRAINED = -1;

[[nodiscard]] inline constexpr int32_t pack_pgseq_typmod(uint8_t alphabet_code, uint8_t width_code) noexcept {
	return static_cast<int32_t>(alphabet_code) * 256 + static_cast<int32_t>(width_code);
}

struct UnpackedPgSeqTypmod {
	uint8_t alphabet_code;
	uint8_t width_code;
};

[[nodiscard]] inline constexpr UnpackedPgSeqTypmod unpack_pgseq_typmod(int32_t typmod) noexcept {
	return UnpackedPgSeqTypmod{
		static_cast<uint8_t>((typmod >> 8) & 0xFF),
		static_cast<uint8_t>(typmod & 0xFF),
	};
}

[[nodiscard]] inline constexpr uint8_t width_code_to_bits(uint8_t width_code) noexcept {
	return static_cast<uint8_t>(8u << width_code);
}

[[nodiscard]] inline constexpr uint8_t width_code_to_bytes(uint8_t width_code) noexcept {
	return static_cast<uint8_t>(1u << width_code);
}

template<typename T> inline constexpr uint8_t width_code_for =
	(sizeof(T) == 1 ? 0u : sizeof(T) == 2 ? 1u : sizeof(T) == 4 ? 2u : sizeof(T) == 8 ? 3u : 255u);
static_assert(width_code_for<uint8_t>  == 0);
static_assert(width_code_for<uint16_t> == 1);
static_assert(width_code_for<uint32_t> == 2);
static_assert(width_code_for<uint64_t> == 3);

// ----------------------------------------------------------------------------
// On-disk / in-memory layout. ALIGNMENT=double in CREATE TYPE guarantees the
// 8-byte alignment of the struct pointer, so data[] at offset 16 is 8-byte
// aligned for uint64_t loads. All fields are native byte order in memory;
// SEND/RECV normalise to network byte order on the wire.
// ----------------------------------------------------------------------------

// alignas(8) so that any compiler-chosen alignment of this struct (even when
// embedded in other C++ containers) keeps data[] on an 8-byte boundary for
// native uint64_t loads. Runtime varlenas are already MAXALIGN-aligned by
// palloc and heap-tuple storage with typalign='d', so this is defensive.
struct alignas(8) PgCompactSequenceHdr {
	int32_t  vl_len_;           // 4 bytes — varlena header
	uint8_t  alphabet_code;     // 1 byte
	uint8_t  width_code;        // 1 byte
	uint16_t padding_letters;   // 2 bytes
	uint32_t bp_size;           // 4 bytes
	uint32_t reserved;          // 4 bytes padding for 8-byte alignment of data
	char     data[FLEXIBLE_ARRAY_MEMBER];
};

static_assert(sizeof(PgCompactSequenceHdr) == 16,
              "PgCompactSequenceHdr must be exactly 16 bytes so data[] lands on 8-byte boundary");
static_assert(alignof(PgCompactSequenceHdr) == 8,
              "PgCompactSequenceHdr must be 8-byte aligned for uint64_t payload loads");

constexpr int PG_SEQ_HEADER_SIZE_BYTES = 16;

[[nodiscard]] inline size_t pg_seq_payload_bytes(const PgCompactSequenceHdr* hdr) {
	return static_cast<size_t>(VARSIZE(hdr)) - PG_SEQ_HEADER_SIZE_BYTES;
}

[[nodiscard]] inline size_t pg_seq_unit_count(const PgCompactSequenceHdr* hdr) {
	return pg_seq_payload_bytes(hdr) / width_code_to_bytes(hdr->width_code);
}

[[nodiscard]] inline char* pg_seq_data_ptr(PgCompactSequenceHdr* hdr) {
	return reinterpret_cast<char*>(hdr) + PG_SEQ_HEADER_SIZE_BYTES;
}

[[nodiscard]] inline const char* pg_seq_data_ptr(const PgCompactSequenceHdr* hdr) {
	return reinterpret_cast<const char*>(hdr) + PG_SEQ_HEADER_SIZE_BYTES;
}

// ----------------------------------------------------------------------------
// Allocation helper — palloc0 guarantees zero-filled tail for clean hashing.
// ----------------------------------------------------------------------------
[[nodiscard]] inline PgCompactSequenceHdr* init_pg_compact_sequence(
	uint8_t alphabet_code, uint8_t width_code,
	size_t unit_count, size_t bp_size, uint16_t padding_letters)
{
	const size_t payload_bytes = unit_count * width_code_to_bytes(width_code);
	const size_t total_bytes   = PG_SEQ_HEADER_SIZE_BYTES + payload_bytes;

	if (total_bytes > static_cast<size_t>(MaxAllocSize))
		ereport(ERROR,
		        (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
		         errmsg("bioseq.seq value too large: %zu bytes exceeds MaxAllocSize",
		                total_bytes)));

	auto* hdr = static_cast<PgCompactSequenceHdr*>(palloc0(total_bytes));
	SET_VARSIZE(hdr, total_bytes);
	hdr->alphabet_code   = alphabet_code;
	hdr->width_code      = width_code;
	hdr->padding_letters = padding_letters;
	hdr->bp_size         = static_cast<uint32_t>(bp_size);
	hdr->reserved        = 0;
	return hdr;
}

// ----------------------------------------------------------------------------
// Runtime-indexed dispatch to one of eight template instantiations.
// ----------------------------------------------------------------------------

template<class OnMatch>
[[gnu::always_inline]] inline decltype(auto) dispatch_pgseq(
	uint8_t alphabet_code, uint8_t width_code, OnMatch&& on_match)
{
	switch (alphabet_code) {
		case 0:
			switch (width_code) {
				case 0: return on_match.template operator()<sequence_custom_alphabet<seq::dna4, uint8_t >>();
				case 1: return on_match.template operator()<sequence_custom_alphabet<seq::dna4, uint16_t>>();
				case 2: return on_match.template operator()<sequence_custom_alphabet<seq::dna4, uint32_t>>();
				case 3: return on_match.template operator()<sequence_custom_alphabet<seq::dna4, uint64_t>>();
			}
			break;
		case 1:
			switch (width_code) {
				case 0: return on_match.template operator()<sequence_custom_alphabet<seq::dna5, uint8_t >>();
				case 1: return on_match.template operator()<sequence_custom_alphabet<seq::dna5, uint16_t>>();
				case 2: return on_match.template operator()<sequence_custom_alphabet<seq::dna5, uint32_t>>();
				case 3: return on_match.template operator()<sequence_custom_alphabet<seq::dna5, uint64_t>>();
			}
			break;
	}
	ereport(ERROR,
	        (errcode(ERRCODE_INTERNAL_ERROR),
	         errmsg("bioseq.seq dispatch: invalid (alphabet_code=%u, width_code=%u)",
	                static_cast<unsigned>(alphabet_code),
	                static_cast<unsigned>(width_code))));
	__builtin_unreachable();
}

// ----------------------------------------------------------------------------
// make_seq_range_from_pg copies storage bytes into a freshly-owned
// compact_sequence_range<Dst>. This is a POC-level copy; §14.1 follow-up
// will switch to a zero-copy span-based view.
// ----------------------------------------------------------------------------

template<class Dst>
requires is_sequence_custom_alphabet<Dst>
[[nodiscard]] inline compact_sequence_range<Dst> make_seq_range_from_pg(const PgCompactSequenceHdr* hdr)
{
	using storage_unit_t = typename Dst::storage_unit;
	using seq_traits = typename compact_sequence_range<Dst>::traits;
	// Payload bytes must be a whole multiple of sizeof(storage_unit_t).
	// A mismatch here signals a corrupt header (truncated tuple, disk
	// bit-flip, or manual pg_catalog mischief) — reject loudly.
	const size_t payload_bytes = pg_seq_payload_bytes(hdr);
	if ((payload_bytes % sizeof(storage_unit_t)) != 0) {
		ereport(ERROR,
		        (errcode(ERRCODE_DATA_CORRUPTED),
		         errmsg("bioseq.seq header: payload %zu bytes is not a multiple of %zu (storage_unit size)",
		                payload_bytes, sizeof(storage_unit_t))));
	}
	// Reserved field must be zero — writers always set it to 0 via palloc0
	// + explicit init_pg_compact_sequence. A non-zero value here implies a
	// hand-edited or foreign-produced varlena.
	if (hdr->reserved != 0) {
		ereport(ERROR,
		        (errcode(ERRCODE_DATA_CORRUPTED),
		         errmsg("bioseq.seq header: reserved field must be 0 (got %u)", hdr->reserved)));
	}
	const size_t unit_count = payload_bytes / sizeof(storage_unit_t);
	// Validate padding_letters: must be strictly less than letters_per_unit
	// (or 0 if unit_count is 0). Otherwise bp_size() would underflow size_t
	// via the computation `size()*lpu - padding`.
	if (hdr->padding_letters >= seq_traits::letters_per_unit ||
	    (unit_count == 0 && hdr->padding_letters != 0)) {
		ereport(ERROR,
		        (errcode(ERRCODE_DATA_CORRUPTED),
		         errmsg("bioseq.seq header: padding_letters=%u invalid for letters_per_unit=%zu",
		                static_cast<unsigned>(hdr->padding_letters),
		                seq_traits::letters_per_unit)));
	}
	// Cross-check the stored bp_size against what the range-trait derivation
	// would compute. If someone hand-edited the header, catch it early.
	const size_t derived_bp = unit_count * seq_traits::letters_per_unit - hdr->padding_letters;
	if (static_cast<uint32_t>(derived_bp) != hdr->bp_size) {
		ereport(ERROR,
		        (errcode(ERRCODE_DATA_CORRUPTED),
		         errmsg("bioseq.seq header: bp_size=%u does not match derived=%zu (unit_count=%zu, padding=%u)",
		                hdr->bp_size, derived_bp, unit_count,
		                static_cast<unsigned>(hdr->padding_letters))));
	}
	compact_sequence_range<Dst> seq{};
	seq._data.assign(unit_count, storage_unit_t{0});
	if (unit_count > 0) {
		std::memcpy(seq._data.data(),
		            pg_seq_data_ptr(hdr),
		            unit_count * sizeof(storage_unit_t));
	}
	seq._padding = hdr->padding_letters;
	return seq;
}

template<class Dst>
requires is_sequence_custom_alphabet<Dst>
[[nodiscard]] inline PgCompactSequenceHdr* make_pg_from_seq_range(
	const compact_sequence_range<Dst>& seq,
	uint8_t alphabet_code, uint8_t width_code)
{
	using storage_unit_t = typename Dst::storage_unit;
	const size_t unit_count = seq.size();
	const uint16_t padding_letters = static_cast<uint16_t>(seq.padding());
	const size_t bp_size = seq.bp_size();

	PgCompactSequenceHdr* hdr = init_pg_compact_sequence(
		alphabet_code, width_code, unit_count, bp_size, padding_letters);
	if (unit_count > 0) {
		std::memcpy(pg_seq_data_ptr(hdr),
		            seq._data.data(),
		            unit_count * sizeof(storage_unit_t));
	}
	return hdr;
}

} // extern "C++"

// ============================================================================
// SQL-callable Datum functions. All use CXX_PG_FUNCTION_INFO_V1 which handles
// the V1 fmgr record declaration with correct extern "C" linkage.
// ============================================================================

#define DatumGetPgCompactSequence(x) \
    reinterpret_cast<PgCompactSequenceHdr*>(PG_DETOAST_DATUM(x))
#define PG_GETARG_PGCOMPACTSEQUENCE_P(n) \
    DatumGetPgCompactSequence(PG_GETARG_DATUM(n))
#define PG_RETURN_PGCOMPACTSEQUENCE_P(x) PG_RETURN_POINTER(x)

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

	uint8_t alphabet_code;
	if (strcmp(alphabet_name, "dna4") == 0)      alphabet_code = 0;
	else if (strcmp(alphabet_name, "dna5") == 0) alphabet_code = 1;
	else
		ereport(ERROR,
		        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		         errmsg("bioseq.seq alphabet \"%s\" is not supported", alphabet_name),
		         errhint("Valid alphabets: 'dna4', 'dna5'")));

	char* endptr = nullptr;
	errno = 0;
	const long width_long = strtol(width_str, &endptr, 10);
	if (endptr == width_str || *endptr != '\0' || errno != 0)
		ereport(ERROR,
		        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		         errmsg("bioseq.seq width \"%s\" is not an integer", width_str)));

	uint8_t width_code;
	switch (width_long) {
		case 8:  width_code = 0; break;
		case 16: width_code = 1; break;
		case 32: width_code = 2; break;
		case 64: width_code = 3; break;
		default:
			ereport(ERROR,
			        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			         errmsg("bioseq.seq width %ld is not supported", width_long),
			         errhint("Supported widths: 8, 16, 32, 64")));
			__builtin_unreachable();
	}

	const int32_t packed = pack_pgseq_typmod(alphabet_code, width_code);
	JustLog("[bioseq] pg: seq_typmod_in alphabet='{}' width={} -> packed={}",
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

	const auto unpacked = unpack_pgseq_typmod(typmod);
	const char* alphabet_name =
		(unpacked.alphabet_code == 0) ? "dna4"
	      : (unpacked.alphabet_code == 1) ? "dna5"
	      : "?";
	const uint8_t width_bits = width_code_to_bits(unpacked.width_code);

	char* result = static_cast<char*>(palloc(64));
	snprintf(result, 64, "('%s',%u)", alphabet_name, static_cast<unsigned>(width_bits));
	PG_RETURN_CSTRING(result);
}

/*
 * bioseq.seq_in: cstring → varlena. Typmod is mandatory (supplied by the
 * column descriptor). Encoding uses encode_sequence_simd for the dispatched
 * (alphabet, width) pair.
 */
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_in);
Datum pg_compact_sequence_in(PG_FUNCTION_ARGS)
{
	const char* lit    = PG_GETARG_CSTRING(0);
	/* arg 1 = type oid (unused) */
	const int32 typmod = PG_GETARG_INT32(2);

	// Postgres passes typmod = -1 for free-standing literals (the cast
	// target's typmod is applied later via bioseq.seq_cast). Default to
	// dna5/uint8 because dna5 is a superset of dna4 — 'N' and ambiguity
	// codes round-trip cleanly, and the cast to dna4 loses them exactly
	// once, in the same way seqan3's own dna4 assignment does.
	uint8_t alphabet_code;
	uint8_t width_code;
	if (typmod < 0) {
		alphabet_code = 1;  // dna5
		width_code    = 0;  // uint8
	} else {
		const auto unpacked = unpack_pgseq_typmod(typmod);
		alphabet_code = unpacked.alphabet_code;
		width_code    = unpacked.width_code;
	}
	const size_t length = strlen(lit);

	PgCompactSequenceHdr* hdr = dispatch_pgseq(alphabet_code, width_code,
		[&]<class Dst>() -> PgCompactSequenceHdr* {
			auto seq = bioseq::simd::N_NEON::encode_sequence_simd<Dst>(lit, length);
			return make_pg_from_seq_range<Dst>(seq, alphabet_code, width_code);
		});

	JustLog("[bioseq] pg: seq_in alphabet_code={} width_code={} length={} -> bp_size={} unit_count={}",
	        alphabet_code, width_code, length, hdr->bp_size, pg_seq_unit_count(hdr));
	PG_RETURN_PGCOMPACTSEQUENCE_P(hdr);
}

/*
 * bioseq.seq_out: varlena → cstring.
 */
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_out);
Datum pg_compact_sequence_out(PG_FUNCTION_ARGS)
{
	PgCompactSequenceHdr* hdr = PG_GETARG_PGCOMPACTSEQUENCE_P(0);

	const std::string decoded = dispatch_pgseq(hdr->alphabet_code, hdr->width_code,
		[&]<class Dst>() -> std::string {
			auto range = make_seq_range_from_pg<Dst>(hdr);
			return bioseq::simd::N_NEON::decode_sequence_simd<Dst>(range);
		});

	char* out = static_cast<char*>(palloc(decoded.size() + 1));
	std::memcpy(out, decoded.data(), decoded.size());
	out[decoded.size()] = '\0';

	JustLog("[bioseq] pg: seq_out bp_size={} -> '{}'", hdr->bp_size, decoded);
	PG_FREE_IF_COPY(hdr, 0);
	PG_RETURN_CSTRING(out);
}

/*
 * bioseq.seq_cast(bioseq.seq, typmod, explicit) — applied by Postgres
 * when the source typmod (in the stored varlena header) differs from the
 * target typmod (from the CAST destination / column declaration).
 *
 * Strategy: decode the source to ASCII via decode_sequence_simd then re-
 * encode at the target (alphabet, width). Lossy by construction when the
 * source had characters outside the target alphabet (e.g. dna5 N → dna4 A).
 */
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_cast);
Datum pg_compact_sequence_cast(PG_FUNCTION_ARGS)
{
	PgCompactSequenceHdr* src = PG_GETARG_PGCOMPACTSEQUENCE_P(0);
	const int32 target_typmod = PG_GETARG_INT32(1);
	/* arg 2 = bool explicit (unused) */

	if (target_typmod < 0) {
		// No typmod at target — pass value through unchanged.
		PG_RETURN_POINTER(src);
	}

	const auto target = unpack_pgseq_typmod(target_typmod);
	if (src->alphabet_code == target.alphabet_code &&
	    src->width_code    == target.width_code) {
		// Already matches — pass-through.
		PG_RETURN_POINTER(src);
	}

	const std::string decoded = dispatch_pgseq(src->alphabet_code, src->width_code,
		[&]<class Src>() -> std::string {
			auto range = make_seq_range_from_pg<Src>(src);
			return bioseq::simd::N_NEON::decode_sequence_simd<Src>(range);
		});

	PgCompactSequenceHdr* result = dispatch_pgseq(target.alphabet_code, target.width_code,
		[&]<class Dst>() -> PgCompactSequenceHdr* {
			auto seq = bioseq::simd::N_NEON::encode_sequence_simd<Dst>(
				decoded.data(), decoded.size());
			return make_pg_from_seq_range<Dst>(seq, target.alphabet_code, target.width_code);
		});

	JustLog("[bioseq] pg: seq_cast ({},{}) -> ({},{}) bp_size={} unit_count={}",
	        static_cast<unsigned>(src->alphabet_code), static_cast<unsigned>(src->width_code),
	        static_cast<unsigned>(target.alphabet_code), static_cast<unsigned>(target.width_code),
	        result->bp_size, pg_seq_unit_count(result));

	PG_FREE_IF_COPY(src, 0);
	PG_RETURN_POINTER(result);
}

/*
 * bioseq.seq_send: varlena → bytea (binary wire format).
 *
 * Wire layout:
 *   offset size  field
 *     0     1   alphabet_code
 *     1     1   width_code
 *     2     2   padding_letters  (uint16, network byte order)
 *     4     4   bp_size          (uint32, NBO)
 *     8     4   unit_count       (uint32, NBO; sanity-checked on recv)
 *    12     *   unit_count storage units, each at native width (1/2/4/8
 *              bytes), in network byte order.
 */
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_send);
Datum pg_compact_sequence_send(PG_FUNCTION_ARGS)
{
	PgCompactSequenceHdr* hdr = PG_GETARG_PGCOMPACTSEQUENCE_P(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint8(&buf, hdr->alphabet_code);
	pq_sendint8(&buf, hdr->width_code);
	pq_sendint16(&buf, hdr->padding_letters);
	pq_sendint32(&buf, hdr->bp_size);

	const size_t unit_count = pg_seq_unit_count(hdr);
	pq_sendint32(&buf, static_cast<uint32>(unit_count));

	const char* const base = pg_seq_data_ptr(hdr);
	switch (hdr->width_code) {
		case 0:
			for (size_t i = 0; i < unit_count; ++i)
				pq_sendint8(&buf, reinterpret_cast<const uint8_t*>(base)[i]);
			break;
		case 1:
			for (size_t i = 0; i < unit_count; ++i)
				pq_sendint16(&buf, reinterpret_cast<const uint16_t*>(base)[i]);
			break;
		case 2:
			for (size_t i = 0; i < unit_count; ++i)
				pq_sendint32(&buf, reinterpret_cast<const uint32_t*>(base)[i]);
			break;
		case 3:
			for (size_t i = 0; i < unit_count; ++i)
				pq_sendint64(&buf, reinterpret_cast<const uint64_t*>(base)[i]);
			break;
		default:
			ereport(ERROR,
			        (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
			         errmsg("bioseq.seq_send: invalid width_code=%u", hdr->width_code)));
	}

	PG_FREE_IF_COPY(hdr, 0);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * bioseq.seq_recv: wire format → varlena.
 */
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_recv);
Datum pg_compact_sequence_recv(PG_FUNCTION_ARGS)
{
	StringInfo buf  = reinterpret_cast<StringInfo>(PG_GETARG_POINTER(0));
	/* arg 1 = type oid */
	const int32 typmod = PG_GETARG_INT32(2);

	const uint8_t  alphabet_code = static_cast<uint8_t>(pq_getmsgint(buf, 1));
	const uint8_t  width_code    = static_cast<uint8_t>(pq_getmsgint(buf, 1));

	// Validate early so we don't read further untrusted fields on a clearly
	// malformed header.
	if (alphabet_code > 1 || width_code > 3)
		ereport(ERROR,
		        (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
		         errmsg("bioseq.seq binary: invalid alphabet_code=%u or width_code=%u",
		                static_cast<unsigned>(alphabet_code),
		                static_cast<unsigned>(width_code))));

	const uint16_t padding_letters = static_cast<uint16_t>(pq_getmsgint(buf, 2));
	const uint32_t bp_size         = static_cast<uint32_t>(pq_getmsgint(buf, 4));
	const uint32_t unit_count      = static_cast<uint32_t>(pq_getmsgint(buf, 4));

	if (typmod >= 0) {
		const auto t = unpack_pgseq_typmod(typmod);
		if (t.alphabet_code != alphabet_code || t.width_code != width_code)
			ereport(ERROR,
			        (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
			         errmsg("bioseq.seq binary: typmod (%u,%u) does not match wire (%u,%u)",
			                static_cast<unsigned>(t.alphabet_code),
			                static_cast<unsigned>(t.width_code),
			                static_cast<unsigned>(alphabet_code),
			                static_cast<unsigned>(width_code))));
	}

	// DoS guard: the wire's unit_count field is untrusted. Ensure we have
	// enough bytes left to even read that many units before calling palloc
	// for potentially gigabytes. `buf->len - buf->cursor` is the remaining
	// message payload; compare against the total bytes we're about to read.
	const uint8_t  width_bytes = width_code_to_bytes(width_code);
	const uint64_t required_bytes = static_cast<uint64_t>(unit_count) * width_bytes;
	const uint64_t available_bytes = static_cast<uint64_t>(buf->len - buf->cursor);
	if (required_bytes > available_bytes)
		ereport(ERROR,
		        (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
		         errmsg("bioseq.seq binary: unit_count=%u width=%u needs %llu bytes, only %llu remain",
		                unit_count, static_cast<unsigned>(width_bytes),
		                static_cast<unsigned long long>(required_bytes),
		                static_cast<unsigned long long>(available_bytes))));

	// Sanity check: derived bp_size must match the wire's bp_size, padding
	// must be less than letters_per_unit (or 0 for empty unit_count).
	const uint8_t bits_per_char_w = (alphabet_code == 0) ? 2u : 3u;
	const size_t  letters_per_unit_w =
		(static_cast<size_t>(width_bytes) * 8u) / bits_per_char_w;
	if (padding_letters >= letters_per_unit_w ||
	    (unit_count == 0 && padding_letters != 0))
		ereport(ERROR,
		        (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
		         errmsg("bioseq.seq binary: padding_letters=%u invalid for letters_per_unit=%zu (unit_count=%u)",
		                static_cast<unsigned>(padding_letters), letters_per_unit_w, unit_count)));
	const uint64_t derived_bp = static_cast<uint64_t>(unit_count) * letters_per_unit_w
	                            - padding_letters;
	if (derived_bp != bp_size)
		ereport(ERROR,
		        (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
		         errmsg("bioseq.seq binary: bp_size=%u inconsistent with derived=%llu (unit_count=%u, padding=%u)",
		                bp_size, static_cast<unsigned long long>(derived_bp),
		                unit_count, static_cast<unsigned>(padding_letters))));

	PgCompactSequenceHdr* hdr = init_pg_compact_sequence(
		alphabet_code, width_code, unit_count, bp_size, padding_letters);

	char* const base = pg_seq_data_ptr(hdr);
	switch (width_code) {
		case 0:
			for (uint32_t i = 0; i < unit_count; ++i)
				reinterpret_cast<uint8_t*>(base)[i] =
					static_cast<uint8_t>(pq_getmsgint(buf, 1));
			break;
		case 1:
			for (uint32_t i = 0; i < unit_count; ++i)
				reinterpret_cast<uint16_t*>(base)[i] =
					static_cast<uint16_t>(pq_getmsgint(buf, 2));
			break;
		case 2:
			for (uint32_t i = 0; i < unit_count; ++i)
				reinterpret_cast<uint32_t*>(base)[i] =
					static_cast<uint32_t>(pq_getmsgint(buf, 4));
			break;
		case 3:
			for (uint32_t i = 0; i < unit_count; ++i)
				reinterpret_cast<uint64_t*>(base)[i] =
					static_cast<uint64_t>(pq_getmsgint64(buf));
			break;
	}

	pq_getmsgend(buf);
	PG_RETURN_PGCOMPACTSEQUENCE_P(hdr);
}

/*
 * bioseq.seq_cmp: lexicographic comparator (the btree support function).
 * Returns -1, 0, or 1 based on compare_sequences_simd (which walks bp by
 * bp — see §3.5 of the spec for why unsigned-unit compare is wrong).
 */
} // extern "C"

namespace {
[[nodiscard]] inline int pg_compact_sequence_cmp_internal(
	const PgCompactSequenceHdr* a, const PgCompactSequenceHdr* b)
{
	if (a->alphabet_code != b->alphabet_code || a->width_code != b->width_code)
		ereport(ERROR,
		        (errcode(ERRCODE_DATATYPE_MISMATCH),
		         errmsg("bioseq.seq cmp: mismatched types (lhs=(%u,%u), rhs=(%u,%u))",
		                static_cast<unsigned>(a->alphabet_code),
		                static_cast<unsigned>(a->width_code),
		                static_cast<unsigned>(b->alphabet_code),
		                static_cast<unsigned>(b->width_code))));

	return dispatch_pgseq(a->alphabet_code, a->width_code,
		[&]<class Dst>() -> int {
			auto ar = make_seq_range_from_pg<Dst>(a);
			auto br = make_seq_range_from_pg<Dst>(b);
			const auto ord = bioseq::simd::N_NEON::compare_sequences_simd(ar, br);
			if (ord == std::strong_ordering::less)    return -1;
			if (ord == std::strong_ordering::greater) return  1;
			return 0;
		});
}
} // anonymous namespace

extern "C" {

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_cmp);
Datum pg_compact_sequence_cmp(PG_FUNCTION_ARGS)
{
	PgCompactSequenceHdr* a = PG_GETARG_PGCOMPACTSEQUENCE_P(0);
	PgCompactSequenceHdr* b = PG_GETARG_PGCOMPACTSEQUENCE_P(1);
	const int32 result = pg_compact_sequence_cmp_internal(a, b);
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_INT32(result);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_lt);
Datum pg_compact_sequence_lt(PG_FUNCTION_ARGS)
{
	PgCompactSequenceHdr* a = PG_GETARG_PGCOMPACTSEQUENCE_P(0);
	PgCompactSequenceHdr* b = PG_GETARG_PGCOMPACTSEQUENCE_P(1);
	const bool result = pg_compact_sequence_cmp_internal(a, b) < 0;
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_BOOL(result);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_le);
Datum pg_compact_sequence_le(PG_FUNCTION_ARGS)
{
	PgCompactSequenceHdr* a = PG_GETARG_PGCOMPACTSEQUENCE_P(0);
	PgCompactSequenceHdr* b = PG_GETARG_PGCOMPACTSEQUENCE_P(1);
	const bool result = pg_compact_sequence_cmp_internal(a, b) <= 0;
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_BOOL(result);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_eq);
Datum pg_compact_sequence_eq(PG_FUNCTION_ARGS)
{
	PgCompactSequenceHdr* a = PG_GETARG_PGCOMPACTSEQUENCE_P(0);
	PgCompactSequenceHdr* b = PG_GETARG_PGCOMPACTSEQUENCE_P(1);
	const bool result = pg_compact_sequence_cmp_internal(a, b) == 0;
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_BOOL(result);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_ne);
Datum pg_compact_sequence_ne(PG_FUNCTION_ARGS)
{
	PgCompactSequenceHdr* a = PG_GETARG_PGCOMPACTSEQUENCE_P(0);
	PgCompactSequenceHdr* b = PG_GETARG_PGCOMPACTSEQUENCE_P(1);
	const bool result = pg_compact_sequence_cmp_internal(a, b) != 0;
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_BOOL(result);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_ge);
Datum pg_compact_sequence_ge(PG_FUNCTION_ARGS)
{
	PgCompactSequenceHdr* a = PG_GETARG_PGCOMPACTSEQUENCE_P(0);
	PgCompactSequenceHdr* b = PG_GETARG_PGCOMPACTSEQUENCE_P(1);
	const bool result = pg_compact_sequence_cmp_internal(a, b) >= 0;
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_BOOL(result);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_gt);
Datum pg_compact_sequence_gt(PG_FUNCTION_ARGS)
{
	PgCompactSequenceHdr* a = PG_GETARG_PGCOMPACTSEQUENCE_P(0);
	PgCompactSequenceHdr* b = PG_GETARG_PGCOMPACTSEQUENCE_P(1);
	const bool result = pg_compact_sequence_cmp_internal(a, b) > 0;
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_BOOL(result);
}

/*
 * bioseq.len(bioseq.seq) → int4. Returns the bp_size recorded in the header.
 * Guards against bp_size > INT32_MAX (reachable for ~1GB payloads in
 * dna4/uint8). Callers wanting int8 semantics can cast the result.
 */
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_len);
Datum pg_compact_sequence_len(PG_FUNCTION_ARGS)
{
	PgCompactSequenceHdr* hdr = PG_GETARG_PGCOMPACTSEQUENCE_P(0);
	const uint32_t bp_size_raw = hdr->bp_size;
	if (bp_size_raw > static_cast<uint32_t>(PG_INT32_MAX)) {
		PG_FREE_IF_COPY(hdr, 0);
		ereport(ERROR,
		        (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
		         errmsg("bioseq.len: bp_size %u exceeds int4 range", bp_size_raw)));
	}
	const int32 bp_size = static_cast<int32>(bp_size_raw);
	PG_FREE_IF_COPY(hdr, 0);
	PG_RETURN_INT32(bp_size);
}

/*
 * bioseq.unit_len(bioseq.seq) → int4. Returns the storage unit count.
 */
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_unit_len);
Datum pg_compact_sequence_unit_len(PG_FUNCTION_ARGS)
{
	PgCompactSequenceHdr* hdr = PG_GETARG_PGCOMPACTSEQUENCE_P(0);
	const size_t unit_count = pg_seq_unit_count(hdr);
	if (unit_count > static_cast<size_t>(PG_INT32_MAX)) {
		PG_FREE_IF_COPY(hdr, 0);
		ereport(ERROR,
		        (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
		         errmsg("bioseq.unit_len: value %zu exceeds int4 range", unit_count)));
	}
	const int32 result = static_cast<int32>(unit_count);
	PG_FREE_IF_COPY(hdr, 0);
	PG_RETURN_INT32(result);
}

} // extern "C"
