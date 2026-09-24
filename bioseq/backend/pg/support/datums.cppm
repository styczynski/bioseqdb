/**
 * @file datums.cppm
 * @brief Provides zero-cost typed wrappers for PostgreSQL Datum values.
 *
 * Covers scalar conversion, nullable cells, fixed value/null arrays, tuple
 * updates, slot views, detoast ownership, and SPI cell extraction.
 */
module;

#include <libassert/assert.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <type_traits>

#include <bioseq/pg_compat.h>

export module bioseq.postgres.support.datums;

import bioseq.postgres.support;

export namespace bioseq::postgres::support {

/** @brief Defines typed conversion hooks to and from PostgreSQL Datum values. */
template<class p_value, class p_enable = void>
struct pg_datum_conversion
{
	static Datum from(p_value value) = delete;
	static p_value to(Datum value) = delete;
};

/** @brief Matches values with a supported conversion into Datum. */
template<class p_value>
concept is_convertible_to_datum = requires(p_value value) {
	{ pg_datum_conversion<std::remove_cvref_t<p_value>>::from(value) } -> std::same_as<Datum>;
};

/** @brief Matches values with a supported conversion from Datum. */
template<class p_value>
concept is_convertible_from_datum = requires(Datum value) {
	{ pg_datum_conversion<std::remove_cvref_t<p_value>>::to(value) } -> std::same_as<std::remove_cvref_t<p_value>>;
};

/** @brief Wraps one raw Datum without changing its representation or ownership. */
struct pg_datum
{
private:
	Datum value_ = static_cast<Datum>(0);

	/** @return PostgreSQL-owned zeroed storage for the requested byte count. */
	template<class p_value>
	[[nodiscard]] static inline p_value *
	alloc_zeroed_bytes(std::size_t byte_count)
	{
		return static_cast<p_value *>(palloc0(byte_count));
	}

public:
	constexpr pg_datum() noexcept = default;

	explicit constexpr pg_datum(Datum value) noexcept
		: value_(value)
	{
	}

	/** @return The wrapped raw Datum value. */
	[[nodiscard]] constexpr Datum
	raw() const noexcept
	{
		return value_;
	}

	constexpr operator Datum() const noexcept
	{
		return value_;
	}

	[[nodiscard]] friend constexpr bool
	operator==(pg_datum lhs, pg_datum rhs) noexcept
	{
		return lhs.value_ == rhs.value_;
	}

	/** @return A Datum wrapper converted from the supplied typed value. */
	template<class p_value>
	requires is_convertible_to_datum<p_value>
	[[nodiscard]] [[gnu::always_inline]] static inline pg_datum
	from(p_value value) noexcept
	{
		return pg_datum{pg_datum_conversion<std::remove_cvref_t<p_value>>::from(value)};
	}

	/** @return The typed value converted from a Datum wrapper. */
	template<class p_value>
	requires is_convertible_from_datum<p_value>
	[[nodiscard]] [[gnu::always_inline]] static inline p_value
	to(pg_datum value) noexcept
	{
		return pg_datum_conversion<std::remove_cvref_t<p_value>>::to(value.value_);
	}

	/** @return A wrapper preserving the supplied raw Datum bits. */
	[[nodiscard]] [[gnu::always_inline]] static inline pg_datum
	from_raw(Datum value) noexcept
	{
		return pg_datum{value};
	}

	/** @return A Datum wrapper containing a PostgreSQL object identifier. */
	[[nodiscard]] [[gnu::always_inline]] static inline pg_datum
	from_oid(Oid value) noexcept
	{
		return pg_datum{ObjectIdGetDatum(value)};
	}

	/** @return The PostgreSQL object identifier stored in the wrapper. */
	[[nodiscard]] [[gnu::always_inline]] static inline Oid
	to_oid(pg_datum value) noexcept
	{
		return DatumGetObjectId(value.value_);
	}

	/** @return A Datum wrapper containing an unsigned 32-bit value. */
	[[nodiscard]] [[gnu::always_inline]] static inline pg_datum
	from_uint32(std::uint32_t value) noexcept
	{
		return pg_datum{UInt32GetDatum(value)};
	}

	/** @return The unsigned 32-bit value stored in the wrapper. */
	[[nodiscard]] [[gnu::always_inline]] static inline std::uint32_t
	to_uint32(pg_datum value) noexcept
	{
		return DatumGetUInt32(value.value_);
	}

	/** @return A Datum wrapper containing an unsigned 64-bit value. */
	[[nodiscard]] [[gnu::always_inline]] static inline pg_datum
	from_uint64(std::uint64_t value) noexcept
	{
		return pg_datum{UInt64GetDatum(value)};
	}

	/** @return The unsigned 64-bit value stored in the wrapper. */
	[[nodiscard]] [[gnu::always_inline]] static inline std::uint64_t
	to_uint64(pg_datum value) noexcept
	{
		return DatumGetUInt64(value.value_);
	}

	/** @return A wrapper borrowing the supplied C string pointer. */
	[[nodiscard]] [[gnu::always_inline]] static inline pg_datum
	from_cstring(const char *value) noexcept
	{
		return pg_datum{CStringGetDatum(value)};
	}

	/** @return The borrowed C string pointer stored in the wrapper. */
	[[nodiscard]] [[gnu::always_inline]] static inline const char *
	to_cstring(pg_datum value) noexcept
	{
		return DatumGetCString(value.value_);
	}

	/** @return A wrapper owning PostgreSQL text converted from a C string. */
	[[nodiscard]] [[gnu::always_inline]] static inline pg_datum
	from_text_cstring(const char *value)
	{
		return pg_datum{CStringGetTextDatum(value)};
	}

	/** @return A packed detoasted text pointer following PostgreSQL ownership. */
	[[nodiscard]] [[gnu::always_inline]] static inline text *
	to_text_pp(pg_datum value)
	{
		return DatumGetTextPP(value.value_);
	}

	/** @return A detoasted bytea pointer following PostgreSQL ownership. */
	[[nodiscard]] [[gnu::always_inline]] inline bytea *
	to_bytea_p() const
	{
		return DatumGetByteaP(value_);
	}

	/** @return A packed detoasted bytea pointer following PostgreSQL ownership. */
	[[nodiscard]] [[gnu::always_inline]] inline bytea *
	to_bytea_pp() const
	{
		return DatumGetByteaPP(value_);
	}

	/** @return A wrapper borrowing the supplied pointer. */
	[[nodiscard]] [[gnu::always_inline]] static inline pg_datum
	from_pointer(const void *value) noexcept
	{
		return pg_datum{PointerGetDatum(value)};
	}

	/** @return The typed borrowed pointer stored in the wrapper. */
	template<class p_value>
	[[nodiscard]] [[gnu::always_inline]] static inline p_value *
	to_pointer(pg_datum value) noexcept
	{
		return reinterpret_cast<p_value *>(DatumGetPointer(value.value_));
	}

	/** @return A wrapper containing the supplied heap tuple. */
	[[nodiscard]] [[gnu::always_inline]] static inline pg_datum
	from_heap_tuple(HeapTuple value) noexcept
	{
		return pg_datum{HeapTupleGetDatum(value)};
	}

	/** @return The heap tuple header stored in the wrapper. */
	[[nodiscard]] [[gnu::always_inline]] static inline HeapTupleHeader
	to_heap_tuple_header(pg_datum value) noexcept
	{
		return DatumGetHeapTupleHeader(value.value_);
	}

	/** @return A wrapper containing the supplied PostgreSQL name. */
	[[nodiscard]] [[gnu::always_inline]] static inline pg_datum
	from_name(Name value) noexcept
	{
		return pg_datum{NameGetDatum(value)};
	}

	/** @return A PostgreSQL-owned copy using the supplied type metadata. */
	[[nodiscard]] inline pg_datum
	copy(bool datum_type_by_value, int datum_type_length) const
	{
		return pg_datum{datumCopy(value_, datum_type_by_value, datum_type_length)};
	}

	/** @brief Appends this Datum using PostgreSQL attribute serialization rules. */
	inline void
	append_serialized(StringInfo buffer,
					  bool datum_type_by_value,
					  int datum_type_length,
					  char datum_type_align) const
	{
		const auto datum_length =
			static_cast<std::uint32_t>(att_addlength_datum(0, datum_type_length, value_));
		const auto datum_length_aligned =
			static_cast<std::uint32_t>(att_align_nominal(datum_length, datum_type_align));

		enlargeStringInfo(buffer, static_cast<int>(datum_length_aligned));
		char *dst = buffer->data + buffer->len;
		std::memset(dst, 0, datum_length_aligned);

		if (datum_type_length > 0)
		{
			if (datum_type_by_value)
			{
				store_att_byval(dst, value_, datum_type_length);
			}
			else
			{
				std::memcpy(dst, DatumGetPointer(value_), datum_type_length);
			}
		}
		else
		{
			Assert(!datum_type_by_value);
			std::memcpy(dst, DatumGetPointer(value_), datum_length);
		}

		buffer->len += datum_length_aligned;
	}

	/** @return A bytea containing this Datum's physical attribute bytes. */
	[[nodiscard]] inline bytea *
	to_bytea(Form_pg_attribute attr) const
	{
		const int datum_length = att_addlength_datum(0, attr->attlen, value_);
		auto *result = alloc_zeroed_bytes<bytea>(static_cast<std::size_t>(datum_length) + VARHDRSZ);
		SET_VARSIZE(result, datum_length + VARHDRSZ);

		if (attr->attlen > 0)
		{
			if (attr->attbyval)
			{
				Datum value_storage{};
				store_att_byval(&value_storage, value_, attr->attlen);
				std::memcpy(VARDATA(result), &value_storage, attr->attlen);
			}
			else
			{
				std::memcpy(VARDATA(result), DatumGetPointer(value_), attr->attlen);
			}
		}
		else
		{
			std::memcpy(VARDATA(result), DatumGetPointer(value_), datum_length);
		}

		return result;
	}

	/** @return A Datum reconstructed from physical attribute bytes. */
	[[nodiscard]] static inline pg_datum
	from_bytea(bytea *bytes, Form_pg_attribute attr)
	{
		if (attr->attbyval)
		{
			return pg_datum{fetch_att(VARDATA_ANY(bytes), true, attr->attlen)};
		}

		char *binary_data = alloc_zeroed_bytes<char>(VARSIZE_ANY_EXHDR(bytes));
		std::memcpy(binary_data, VARDATA_ANY(bytes), VARSIZE_ANY_EXHDR(bytes));
		return pg_datum{fetch_att(binary_data, false, attr->attlen)};
	}
};

/** @brief Converts Boolean values through PostgreSQL's by-value Datum ABI. */
template<>
struct pg_datum_conversion<bool>
{
	[[nodiscard]] [[gnu::always_inline]] static inline Datum from(bool value) noexcept { return BoolGetDatum(value); }
	[[nodiscard]] [[gnu::always_inline]] static inline bool to(Datum value) noexcept { return DatumGetBool(value); }
};

/** @brief Converts signed 16-bit values through PostgreSQL's Datum ABI. */
template<>
struct pg_datum_conversion<std::int16_t>
{
	[[nodiscard]] [[gnu::always_inline]] static inline Datum from(std::int16_t value) noexcept { return Int16GetDatum(value); }
	[[nodiscard]] [[gnu::always_inline]] static inline std::int16_t to(Datum value) noexcept { return DatumGetInt16(value); }
};

/** @brief Converts signed 32-bit values through PostgreSQL's Datum ABI. */
template<>
struct pg_datum_conversion<std::int32_t>
{
	[[nodiscard]] [[gnu::always_inline]] static inline Datum from(std::int32_t value) noexcept { return Int32GetDatum(value); }
	[[nodiscard]] [[gnu::always_inline]] static inline std::int32_t to(Datum value) noexcept { return DatumGetInt32(value); }
};

/** @brief Converts signed 64-bit values through PostgreSQL's Datum ABI. */
template<>
struct pg_datum_conversion<std::int64_t>
{
	[[nodiscard]] [[gnu::always_inline]] static inline Datum from(std::int64_t value) noexcept { return Int64GetDatum(value); }
	[[nodiscard]] [[gnu::always_inline]] static inline std::int64_t to(Datum value) noexcept { return DatumGetInt64(value); }
};

/** @brief Covers PostgreSQL's distinct `int64` alias when required by the ABI. */
template<class p_value>
requires (std::same_as<p_value, int64> && !std::same_as<p_value, std::int64_t>)
struct pg_datum_conversion<p_value>
{
	[[nodiscard]] [[gnu::always_inline]] static inline Datum from(p_value value) noexcept { return Int64GetDatum(value); }
	[[nodiscard]] [[gnu::always_inline]] static inline p_value to(Datum value) noexcept { return static_cast<p_value>(DatumGetInt64(value)); }
};

/** @brief Preserves float object bits across PostgreSQL's by-value Datum ABI. */
template<>
struct pg_datum_conversion<float>
{
	[[nodiscard]] [[gnu::always_inline]] static inline Datum from(float value) noexcept
	{
		std::int32_t bits{};
		std::memcpy(&bits, &value, sizeof(bits));
		return Int32GetDatum(bits);
	}

	[[nodiscard]] [[gnu::always_inline]] static inline float to(Datum value) noexcept
	{
		const std::int32_t bits = DatumGetInt32(value);
		float result{};
		std::memcpy(&result, &bits, sizeof(result));
		return result;
	}
};

/** @brief Preserves double object bits across PostgreSQL's by-value Datum ABI. */
template<>
struct pg_datum_conversion<double>
{
	[[nodiscard]] [[gnu::always_inline]] static inline Datum from(double value) noexcept
	{
		std::int64_t bits{};
		std::memcpy(&bits, &value, sizeof(bits));
		return Int64GetDatum(bits);
	}

	[[nodiscard]] [[gnu::always_inline]] static inline double to(Datum value) noexcept
	{
		const std::int64_t bits = DatumGetInt64(value);
		double result{};
		std::memcpy(&result, &bits, sizeof(result));
		return result;
	}
};

/** @brief Couples one Datum with its PostgreSQL null flag. */
struct pg_nullable_datum
{
	pg_datum value{};
	bool is_null = true;

	/** @return A null cell with zeroed Datum storage. */
	[[nodiscard]] static constexpr pg_nullable_datum
	null() noexcept
	{
		return {};
	}

	/** @return A non-null cell borrowing the supplied Datum value. */
	[[nodiscard]] static constexpr pg_nullable_datum
	not_null(pg_datum datum) noexcept
	{
		return pg_nullable_datum{.value = datum, .is_null = false};
	}

	/** @return A non-null cell converted from a supported typed value. */
	template<class p_value>
	requires is_convertible_to_datum<p_value>
	[[nodiscard]] static inline pg_nullable_datum
	from(p_value value) noexcept
	{
		return not_null(pg_datum::from(value));
	}

	/** @return A nullable cell converted from an optional typed value. */
	template<class p_value>
	requires is_convertible_to_datum<p_value>
	[[nodiscard]] static inline pg_nullable_datum
	from_optional(const std::optional<p_value> &value) noexcept
	{
		return value.has_value() ? from(*value) : null();
	}

	/** @return The typed optional represented by this cell. */
	template<class p_value>
	requires is_convertible_from_datum<p_value>
	[[nodiscard]] inline std::optional<p_value>
	to_optional() const noexcept
	{
		if (is_null)
		{
			return std::nullopt;
		}
		return pg_datum::to<p_value>(value);
	}
};

/** @brief Owns fixed-capacity Datum and null arrays for tuple operations. */
template<std::size_t p_dimensions>
struct pg_datum_list
{
	static constexpr std::size_t dimensions = p_dimensions;

	std::array<Datum, p_dimensions> values{};
	std::array<bool, p_dimensions> nulls{};

	pg_datum_list() noexcept
	{
		nulls.fill(true);
	}

	/** @return A row whose attributes are all null. */
	[[nodiscard]] static pg_datum_list
	all_null() noexcept
	{
		return pg_datum_list{};
	}

	/** @return A row whose attributes are non-null zero Datums. */
	[[nodiscard]] static pg_datum_list
	all_nonnull_zeroed() noexcept
	{
		pg_datum_list row{};
		row.nulls.fill(false);
		return row;
	}

	/** @return The zero-based array index for a one-based attribute number. */
		[[nodiscard]] static std::size_t
		att_index(int attnum) noexcept
	{
		DEBUG_ASSERT(attnum >= 1 &&
					 static_cast<std::size_t>(attnum) <= p_dimensions,
					 "pg_datum_list attnum out of bounds");
		return static_cast<std::size_t>(attnum - 1);
	}

	/** @brief Checks that a tuple descriptor fits the fixed array capacity. */
		static void
		assert_tuple_desc_fits(TupleDesc tuple_desc) noexcept
	{
		DEBUG_ASSERT(tuple_desc != nullptr, "pg_datum_list tuple descriptor is null");
		DEBUG_ASSERT(tuple_desc->natts <= static_cast<int>(p_dimensions),
					 "pg_datum_list dimensions smaller than tuple descriptor natts");
	}

	[[nodiscard]] Datum *
	values_data() noexcept
	{
		return values.data();
	}

	[[nodiscard]] const Datum *
	values_data() const noexcept
	{
		return values.data();
	}

	[[nodiscard]] bool *
	nulls_data() noexcept
	{
		return nulls.data();
	}

	[[nodiscard]] const bool *
	nulls_data() const noexcept
	{
		return nulls.data();
	}

	/** @return The Datum at a one-based attribute number. */
	[[nodiscard]] pg_datum
	at(int attnum) const noexcept
	{
		return pg_datum::from_raw(values[att_index(attnum)]);
	}

	/** @return True when the selected attribute is null. */
	[[nodiscard]] bool
	is_null(int attnum) const noexcept
	{
		return nulls[att_index(attnum)];
	}

	/** @brief Converts and stores one non-null typed attribute. */
	template<class p_value>
	requires is_convertible_to_datum<p_value>
	void
	set(int attnum, p_value value) noexcept
	{
		const std::size_t attribute_index = att_index(attnum);
		values[attribute_index] = pg_datum::from(value).raw();
		nulls[attribute_index] = false;
	}

	/** @brief Stores one non-null raw Datum attribute. */
	void
	set_raw(int attnum, pg_datum value) noexcept
	{
		const std::size_t attribute_index = att_index(attnum);
		values[attribute_index] = value.raw();
		nulls[attribute_index] = false;
	}

	/** @brief Selects one PostgreSQL object-identifier replacement. */
	void
	set_oid(int attnum, Oid value) noexcept
	{
		set_raw(attnum, pg_datum::from_oid(value));
	}

	/** @brief Selects one unsigned 32-bit replacement. */
	void
	set_uint32(int attnum, std::uint32_t value) noexcept
	{
		set_raw(attnum, pg_datum::from_uint32(value));
	}

	/** @brief Selects one unsigned 64-bit replacement. */
	void
	set_uint64(int attnum, std::uint64_t value) noexcept
	{
		set_raw(attnum, pg_datum::from_uint64(value));
	}

	/** @brief Marks one attribute null and clears its Datum storage. */
	void
	set_null(int attnum) noexcept
	{
		const std::size_t attribute_index = att_index(attnum);
		values[attribute_index] = static_cast<Datum>(0);
		nulls[attribute_index] = true;
	}

	/** @brief Fills every null flag and clears values when marking null. */
	void
	fill_nulls(bool value) noexcept
	{
		nulls.fill(value);
		if (value)
		{
			values.fill(static_cast<Datum>(0));
		}
	}

	/** @brief Deforms a heap tuple into this fixed row. */
	void
	deform(HeapTuple tuple, TupleDesc tuple_desc) noexcept
	{
		assert_tuple_desc_fits(tuple_desc);
		heap_deform_tuple(tuple, tuple_desc, values.data(), nulls.data());
	}

	/** @return A fixed row deformed from the supplied heap tuple. */
	[[nodiscard]] static pg_datum_list
	deformed(HeapTuple tuple, TupleDesc tuple_desc) noexcept
	{
		pg_datum_list row{};
		row.deform(tuple, tuple_desc);
		return row;
	}

	/** @return A PostgreSQL heap tuple formed from this row. */
	[[nodiscard]] HeapTuple
	form_tuple(TupleDesc tuple_desc) noexcept
	{
		assert_tuple_desc_fits(tuple_desc);
		return heap_form_tuple(tuple_desc, values.data(), nulls.data());
	}

	/** @brief Appends this row to a PostgreSQL tuplestore. */
	void
	put_values(Tuplestorestate *tuplestore, TupleDesc tuple_desc) noexcept
	{
		assert_tuple_desc_fits(tuple_desc);
		tuplestore_putvalues(tuplestore, tuple_desc, values.data(), nulls.data());
	}
};

/** @brief Owns fixed Datum arrays describing selected tuple replacements. */
template<std::size_t p_dimensions>
struct pg_datum_update
{
	std::array<Datum, p_dimensions> values{};
	std::array<bool, p_dimensions> nulls{};
	std::array<bool, p_dimensions> replace{};

	pg_datum_update() noexcept
	{
		nulls.fill(true);
	}

	/** @return The zero-based array index for a one-based attribute number. */
	[[nodiscard]] static std::size_t
	att_index(int attnum) noexcept
	{
		DEBUG_ASSERT(attnum >= 1 &&
					 static_cast<std::size_t>(attnum) <= p_dimensions,
					 "pg_datum_update attnum out of bounds");
		return static_cast<std::size_t>(attnum - 1);
	}

	/** @brief Checks that a tuple descriptor fits the update array capacity. */
	static void
	assert_tuple_desc_fits(TupleDesc tuple_desc) noexcept
	{
		DEBUG_ASSERT(tuple_desc != nullptr, "pg_datum_update tuple descriptor is null");
		DEBUG_ASSERT(tuple_desc->natts <= static_cast<int>(p_dimensions),
					 "pg_datum_update dimensions smaller than tuple descriptor natts");
	}

	/** @brief Converts and selects one non-null typed replacement. */
	template<class p_value>
	requires is_convertible_to_datum<p_value>
	void
	set(int attnum, p_value value) noexcept
	{
		const std::size_t attribute_index = att_index(attnum);
		values[attribute_index] = pg_datum::from(value).raw();
		nulls[attribute_index] = false;
		replace[attribute_index] = true;
	}

	/** @brief Selects one non-null raw Datum replacement. */
	void
	set_raw(int attnum, pg_datum value) noexcept
	{
		const std::size_t attribute_index = att_index(attnum);
		values[attribute_index] = value.raw();
		nulls[attribute_index] = false;
		replace[attribute_index] = true;
	}

	void
	set_oid(int attnum, Oid value) noexcept
	{
		set_raw(attnum, pg_datum::from_oid(value));
	}

	void
	set_uint32(int attnum, std::uint32_t value) noexcept
	{
		set_raw(attnum, pg_datum::from_uint32(value));
	}

	void
	set_uint64(int attnum, std::uint64_t value) noexcept
	{
		set_raw(attnum, pg_datum::from_uint64(value));
	}

	/** @brief Selects a null replacement for one attribute. */
	void
	set_null(int attnum) noexcept
	{
		const std::size_t attribute_index = att_index(attnum);
		values[attribute_index] = static_cast<Datum>(0);
		nulls[attribute_index] = true;
		replace[attribute_index] = true;
	}

	/** @return A modified heap tuple using the selected replacement attributes. */
	[[nodiscard]] HeapTuple
	modify(HeapTuple old_tuple, TupleDesc tuple_desc) noexcept
	{
		assert_tuple_desc_fits(tuple_desc);
		return heap_modify_tuple(old_tuple, tuple_desc,
								 values.data(), nulls.data(), replace.data());
	}
};

/** @brief Borrows the Datum and null arrays of one PostgreSQL tuple slot. */
struct pg_slot_datums_view
{
	TupleTableSlot *slot = nullptr;

	/** @brief Checks that the borrowed slot and its arrays are present. */
	void
	assert_slot_arrays() const noexcept
	{
		DEBUG_ASSERT(slot != nullptr, "pg_slot_datums_view slot is null");
		DEBUG_ASSERT(slot->tts_tupleDescriptor != nullptr,
					 "pg_slot_datums_view tuple descriptor is null");
		DEBUG_ASSERT(slot->tts_values != nullptr && slot->tts_isnull != nullptr,
					 "pg_slot_datums_view slot arrays are null");
	}

	/** @return The tuple descriptor owned by the borrowed slot. */
	[[nodiscard]] TupleDesc
	tuple_desc() const noexcept
	{
		assert_slot_arrays();
		return slot->tts_tupleDescriptor;
	}

	/** @return The attribute count of the borrowed slot. */
	[[nodiscard]] int
	natts() const noexcept
	{
		assert_slot_arrays();
		return slot->tts_tupleDescriptor->natts;
	}

	[[nodiscard]] Datum *
	values_data() const noexcept
	{
		assert_slot_arrays();
		return slot->tts_values;
	}

	[[nodiscard]] bool *
	nulls_data() const noexcept
	{
		assert_slot_arrays();
		return slot->tts_isnull;
	}

	/** @brief Materializes every slot attribute into its Datum arrays. */
	void
	materialize_all() const noexcept
	{
		assert_slot_arrays();
		slot_getallattrs(slot);
	}

	/** @brief Clears the slot and marks every attribute null. */
	void
	clear_all_to_null() const noexcept
	{
		assert_slot_arrays();
		ExecClearTuple(slot);
		std::memset(slot->tts_values, 0, sizeof(Datum) * static_cast<std::size_t>(natts()));
		std::fill_n(slot->tts_isnull, natts(), true);
	}

	/** @return The Datum at a one-based attribute number. */
	[[nodiscard]] pg_datum
	at(int attnum) const noexcept
	{
		assert_slot_arrays();
		DEBUG_ASSERT(attnum >= 1 && attnum <= natts(),
					 "pg_slot_datums_view attnum out of bounds");
		return pg_datum::from_raw(slot->tts_values[attnum - 1]);
	}

	/** @return True when the selected slot attribute is null. */
	[[nodiscard]] bool
	is_null(int attnum) const noexcept
	{
		assert_slot_arrays();
		DEBUG_ASSERT(attnum >= 1 && attnum <= natts(),
					 "pg_slot_datums_view attnum out of bounds");
		return slot->tts_isnull[attnum - 1];
	}

	/** @brief Converts and stores one non-null typed slot attribute. */
	template<class p_value>
	requires is_convertible_to_datum<p_value>
	void
	set(int attnum, p_value value) const noexcept
	{
		assert_slot_arrays();
		DEBUG_ASSERT(attnum >= 1 && attnum <= natts(),
					 "pg_slot_datums_view attnum out of bounds");
		slot->tts_values[attnum - 1] = pg_datum::from(value).raw();
		slot->tts_isnull[attnum - 1] = false;
	}

	/** @brief Stores one non-null raw Datum slot attribute. */
	void
	set_raw(int attnum, pg_datum value) const noexcept
	{
		assert_slot_arrays();
		DEBUG_ASSERT(attnum >= 1 && attnum <= natts(),
					 "pg_slot_datums_view attnum out of bounds");
		slot->tts_values[attnum - 1] = value.raw();
		slot->tts_isnull[attnum - 1] = false;
	}

	/** @brief Marks one slot attribute null and clears its Datum bits. */
	void
	set_null(int attnum) const noexcept
	{
		assert_slot_arrays();
		DEBUG_ASSERT(attnum >= 1 && attnum <= natts(),
					 "pg_slot_datums_view attnum out of bounds");
		slot->tts_values[attnum - 1] = static_cast<Datum>(0);
		slot->tts_isnull[attnum - 1] = true;
	}

	/** @brief Marks the populated arrays as one virtual tuple. */
	void
	store_virtual() const noexcept
	{
		assert_slot_arrays();
		ExecStoreVirtualTuple(slot);
	}
};

/** @brief Owns a detoasted pointer only when PostgreSQL returned a copy. */
template<class p_value = varlena>
struct pg_detoasted_ptr
{
	p_value *ptr = nullptr;
	bool is_copy = false;

	pg_detoasted_ptr() = default;

	pg_detoasted_ptr(p_value *pointer, bool copied)
		: ptr(pointer), is_copy(copied)
	{
	}

	pg_detoasted_ptr(const pg_detoasted_ptr &) = delete;
	pg_detoasted_ptr &operator=(const pg_detoasted_ptr &) = delete;

	pg_detoasted_ptr(pg_detoasted_ptr &&other) noexcept
		: ptr(other.ptr), is_copy(other.is_copy)
	{
		other.ptr = nullptr;
		other.is_copy = false;
	}

	pg_detoasted_ptr &
	operator=(pg_detoasted_ptr &&other) noexcept
	{
		if (this != &other)
		{
			free_if_copy();
			ptr = other.ptr;
			is_copy = other.is_copy;
			other.ptr = nullptr;
			other.is_copy = false;
		}
		return *this;
	}

	/** @brief Frees a copied detoast result and clears this owner. */
	void
	free_if_copy() noexcept
	{
		if (is_copy && ptr != nullptr)
		{
			pfree(ptr);
		}
		ptr = nullptr;
		is_copy = false;
	}
};

/** @return A typed owner for a fully detoasted Datum. */
template<class p_value = varlena>
[[nodiscard]] inline pg_detoasted_ptr<p_value>
detoast_as(pg_datum datum)
{
	auto *detoasted = reinterpret_cast<p_value *>(PG_DETOAST_DATUM(datum.raw()));
	const bool copied =
		reinterpret_cast<void *>(detoasted) != reinterpret_cast<void *>(DatumGetPointer(datum.raw()));
	return pg_detoasted_ptr<p_value>{detoasted, copied};
}

/** @return A typed owner for a packed detoasted Datum. */
template<class p_value = varlena>
[[nodiscard]] inline pg_detoasted_ptr<p_value>
detoast_packed_as(pg_datum datum)
{
	auto *detoasted = reinterpret_cast<p_value *>(PG_DETOAST_DATUM_PACKED(datum.raw()));
	const bool copied =
		reinterpret_cast<void *>(detoasted) != reinterpret_cast<void *>(DatumGetPointer(datum.raw()));
	return pg_detoasted_ptr<p_value>{detoasted, copied};
}

/** @brief Frees a candidate only when it differs from the original Datum pointer. */
[[gnu::always_inline]] inline void
pfree_if_distinct_from(pg_datum original, const void *candidate) noexcept
{
	if (candidate != nullptr &&
		const_cast<void *>(candidate) != reinterpret_cast<void *>(DatumGetPointer(original.raw())))
	{
		pfree(const_cast<void *>(candidate));
	}
}

/**
 * @brief Detoasts extended tuple attributes into a copy-on-first-change array.
 * @return The original array or a PostgreSQL-owned replacement array.
 */
[[nodiscard]] inline Datum *
detoast_values(TupleDesc tuple_desc, Datum *original_values, bool *nulls)
{
	const int natts = tuple_desc->natts;
	Datum *values = original_values;

	for (int attribute_index = 0; attribute_index < natts; ++attribute_index)
	{
		if (!nulls[attribute_index]
			&& bioseq::postgres::support::tuple_descriptor_attribute(tuple_desc, attribute_index)->attlen == -1
			&& VARATT_IS_EXTENDED(DatumGetPointer(values[attribute_index])))
		{
			if (values == original_values)
			{
				values = static_cast<Datum *>(
					palloc0(sizeof(Datum) * static_cast<std::size_t>(natts)));
				std::memcpy(values, original_values, sizeof(Datum) * static_cast<std::size_t>(natts));
			}

			auto* detoasted_value = detoast_attr(reinterpret_cast<varlena*>(
				DatumGetPointer(values[attribute_index])));
			values[attribute_index] = PointerGetDatum(detoasted_value);
		}
	}

	return values;
}

/** @brief Couples one binary SPI result value with its null flag. */
struct pg_spi_cell
{
	pg_datum value{};
	bool is_null = true;

	/** @return The requested SPI tuple cell without changing Datum ownership. */
	[[nodiscard]] static inline pg_spi_cell
	get(HeapTuple tuple, TupleDesc tuple_desc, int attnum) noexcept
	{
		bool local_is_null = true;
		Datum raw = SPI_getbinval(tuple, tuple_desc, attnum, &local_is_null);
		return pg_spi_cell{.value = pg_datum::from_raw(raw), .is_null = local_is_null};
	}

	/** @return The typed optional represented by this SPI cell. */
	template<class p_value>
	requires is_convertible_from_datum<p_value>
	[[nodiscard]] std::optional<p_value>
	optional() const noexcept
	{
		if (is_null)
		{
			return std::nullopt;
		}
		return pg_datum::to<p_value>(value);
	}

	/** @return The optional PostgreSQL object identifier in this cell. */
	[[nodiscard]] std::optional<Oid>
	optional_oid() const noexcept
	{
		if (is_null)
		{
			return std::nullopt;
		}
		return pg_datum::to_oid(value);
	}

	/** @return The optional unsigned 32-bit value in this cell. */
	[[nodiscard]] std::optional<std::uint32_t>
	optional_uint32() const noexcept
	{
		if (is_null)
		{
			return std::nullopt;
		}
		return pg_datum::to_uint32(value);
	}

	/** @return The optional unsigned 64-bit value in this cell. */
	[[nodiscard]] std::optional<std::uint64_t>
	optional_uint64() const noexcept
	{
		if (is_null)
		{
			return std::nullopt;
		}
		return pg_datum::to_uint64(value);
	}
};

static_assert(sizeof(pg_datum) == sizeof(Datum));
static_assert(std::is_trivially_copyable_v<pg_datum>);
static_assert(!is_convertible_to_datum<Datum>);
static_assert(!is_convertible_from_datum<Datum>);
static_assert(is_convertible_to_datum<std::int64_t>);
static_assert(is_convertible_from_datum<std::int64_t>);
static_assert(is_convertible_to_datum<int64>);
static_assert(is_convertible_from_datum<int64>);
static_assert(!is_convertible_to_datum<std::uint32_t>);
static_assert(!is_convertible_from_datum<std::uint32_t>);
static_assert(!is_convertible_to_datum<Oid>);
static_assert(!is_convertible_from_datum<Oid>);
static_assert(!is_convertible_to_datum<std::uint64_t>);
static_assert(!is_convertible_from_datum<std::uint64_t>);
static_assert(!is_convertible_to_datum<uint64>);
static_assert(!is_convertible_from_datum<uint64>);
static_assert(!is_convertible_to_datum<const char *>);

} // namespace bioseq::postgres::support
