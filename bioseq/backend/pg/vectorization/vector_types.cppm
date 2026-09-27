/**
 * @file
 * Store a batch of rows as one value array per column for vector execution
 * The slot owns copied values and can return selected rows as scalar tuples.
 */

/*
 * A vector slot holds up to vector_column_capacity rows. The  columnar scan and
 * vector functions share these records directly, so their field order and
 * types must agree across modules.
 */
module;

#include <algorithm>
#include <array>
#include <bioseq/pg_compat.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

export module bioseq.postgres.vectorization.types;

import bioseq.common.checked_arithmetic;
import bioseq.postgres.support.datums;
import bioseq.postgres.support;
import bioseq.postgres.support.runtime_diagnostics;

/** Maximum scalar rows stored in one vector batch. */
export constexpr std::uint32_t vector_column_capacity = 10'000;

/**
 * Fixed-size values are copied into the column buffer. Variable-length values
 * use pointers to owned copies, so they survive reuse of the input tuple slot.
 * stored_by_value controls how fetch_att reads each stored value.
 */
export enum class vector_storage_kind : std::uint8_t { inline_fixed, owned_varlena_datum };

/** Values and NULL flags for one column in a vector batch. */
export struct vector_column_storage {
    /** Number of populated rows in this column. */
    std::uint32_t lane_count;
    /** Bytes reserved for each row in data. */
    std::uint16_t element_width;
    /** Whether fetch_att reads each lane as an inline Datum value. */
    bool stored_by_value;
    /** Whether data holds fixed-size values or pointers to owned copies. */
    vector_storage_kind storage_kind;
    /** Memory context used to allocate variable-length value copies. */
    MemoryContext storage_context;
    /** Aligned value  buffer, with element_width bytes per row. */
    Datum* data;
    /** True for each NULL row value. */
    std::array<bool, vector_column_capacity> is_null;
    /** Optional source row numbers, borrowed from the containing batch slot. */
    std::uint64_t* source_row_numbers;

    /** View the NULL flags for populated rows. */
    [[nodiscard]] std::span<bool> nulls() noexcept {
        return {is_null.data(), lane_count};
    }

    /** Read the NULL flags for populated rows. */
    [[nodiscard]] std::span<const bool> nulls() const noexcept {
        return {is_null.data(), lane_count};
    }

    /**
     * View populated values without copying. p_value must match the stored
     * element width and representation.
     */
    template <class p_value> [[nodiscard]] std::span<p_value> values() noexcept {
        return {reinterpret_cast<p_value*>(data), lane_count};
    }

    /**
     * Read populated values without copying. p_value must match the stored
     * element width and representation.
     */
    template <class p_value> [[nodiscard]] std::span<const p_value> values() const noexcept {
        return {reinterpret_cast<const p_value*>(data), lane_count};
    }
};

/** PostgreSQL tuple slot holding a batch with one value array per column. */
export struct vector_tuple_table_slot_storage {
    /** Virtual tuple slot kept first for PostgreSQL slot operations. */
    VirtualTupleTableSlot virtual_slot;
    /** Number of populated rows. */
    std::uint32_t row_count;
    /** True for rows that pass the ector filters. */
    std::array<bool, vector_column_capacity> row_is_active;
    /** Source row number for each batch row. */
    std::array<std::uint64_t, vector_column_capacity> source_row_numbers;
};

/**
 * Mark an argument as a  scalar constant or a vector column Keep the int-sized
 * tag first in vector_operand_storage because, vector functions read it directly.
 */
export enum class vector_operand_storage_kind : int { constant, column };

/** One vector function argument: a scalar Datum or a column storage pointer. */
export struct vector_operand_storage {
    vector_operand_storage_kind kind;
    Datum datum;
};

/** PostgreSQL BIGINT aggregate state, with a 128-bit sum to limit overflow. */
export struct int128_aggregate_state_storage {
    bool calculate_sum_of_squares;
    int64 count;
    int128 sum;
    int128 sum_of_squares;
};

/** Count and sum stored in the two-BIGINT array for SMALLINT and INTEGER AVG. */
export struct int64_aggregate_state_storage {
    int64 count;
    int64 sum;
};

/* Check the field offsets shared by vector functions and aggregate states. */
namespace {

/** Round a field offset up to its required memory alignment. */
[[nodiscard]] consteval std::size_t aligned_offset(std::size_t offset,
                                                   std::size_t alignment) noexcept {
    return (offset + alignment - 1) / alignment * alignment;
}

static_assert(std::is_standard_layout_v<vector_operand_storage>);
static_assert(offsetof(vector_operand_storage, kind) == 0);
static_assert(std::is_same_v<std::underlying_type_t<vector_operand_storage_kind>, int>);
static_assert(offsetof(vector_operand_storage, datum) ==
              aligned_offset(sizeof(vector_operand_storage_kind), alignof(Datum)));
static_assert(std::is_standard_layout_v<vector_column_storage>);
static_assert(offsetof(vector_column_storage, lane_count) == 0);
static_assert(sizeof(std::array<bool, vector_column_capacity>) ==
              sizeof(bool) * vector_column_capacity);
static_assert(alignof(std::array<bool, vector_column_capacity>) == alignof(bool));
static_assert(std::is_trivially_copyable_v<std::array<bool, vector_column_capacity>>);
static_assert(offsetof(vector_column_storage, source_row_numbers) ==
              aligned_offset(offsetof(vector_column_storage, is_null) +
                                 sizeof(bool) * vector_column_capacity,
                             alignof(std::uint64_t*)));
static_assert(std::is_standard_layout_v<vector_tuple_table_slot_storage>);
static_assert(offsetof(vector_tuple_table_slot_storage, virtual_slot) == 0);
static_assert(sizeof(std::array<std::uint64_t, vector_column_capacity>) ==
              sizeof(std::uint64_t) * vector_column_capacity);
static_assert(alignof(std::array<std::uint64_t, vector_column_capacity>) == alignof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<std::array<std::uint64_t, vector_column_capacity>>);
static_assert(offsetof(vector_tuple_table_slot_storage, source_row_numbers) ==
              aligned_offset(offsetof(vector_tuple_table_slot_storage, row_is_active) +
                                 sizeof(bool) * vector_column_capacity,
                             alignof(std::uint64_t)));
static_assert(offsetof(VirtualTupleTableSlot, base) == 0);
static_assert(std::is_standard_layout_v<int128_aggregate_state_storage>);
static_assert(offsetof(int128_aggregate_state_storage, calculate_sum_of_squares) == 0);
static_assert(offsetof(int128_aggregate_state_storage, count) ==
              aligned_offset(sizeof(bool), alignof(int64)));
static_assert(offsetof(int128_aggregate_state_storage, sum) ==
              aligned_offset(offsetof(int128_aggregate_state_storage, count) + sizeof(int64),
                             alignof(int128)));
static_assert(offsetof(int128_aggregate_state_storage, sum_of_squares) ==
              offsetof(int128_aggregate_state_storage, sum) + sizeof(int128));
static_assert(sizeof(int128_aggregate_state_storage) ==
              offsetof(int128_aggregate_state_storage, sum_of_squares) + sizeof(int128));
static_assert(std::is_standard_layout_v<int64_aggregate_state_storage>);
static_assert(offsetof(int64_aggregate_state_storage, count) == 0);
static_assert(offsetof(int64_aggregate_state_storage, sum) == sizeof(int64));
static_assert(sizeof(int64_aggregate_state_storage) == 2 * sizeof(int64));

} // namespace

/**
 * Allocate an empty column and its fixed-capacity buffer in the current  context.
 * Keep source_row_numbers as a borrowed pointer. storage_context is used later
 * for copies of variable-length values.
 */
export [[nodiscard]] vector_column_storage*
allocate_vector_column(std::int16_t element_width,
                       bool stored_by_value,
                       std::uint64_t* source_row_numbers,
                       vector_storage_kind storage_kind = vector_storage_kind::inline_fixed,
                       MemoryContext storage_context = CurrentMemoryContext) {
    vector_column_storage* column =
        static_cast<vector_column_storage*>(palloc0(sizeof(vector_column_storage)));

    column->lane_count = 0;
    column->data = static_cast<Datum*>(palloc0(bioseq::common::saturating_mul(
        static_cast<std::uint64_t>(element_width), vector_column_capacity)));
    column->element_width = static_cast<uint16>(element_width);
    column->stored_by_value = stored_by_value;
    column->storage_kind = storage_kind;
    column->storage_context = storage_context;
    column->source_row_numbers = source_row_numbers;

    return column;
}

namespace {

/** Record whether an observed input value is compressed, external or inline. */
void note_owned_varlena_source_form(Datum source) noexcept {
    if (!bioseq::postgres::support::runtime_diagnostics::is_bpchar_source_observation_active())
        return;

    const Pointer raw_varlena = DatumGetPointer(source);
    using enum bioseq::postgres::support::runtime_diagnostics::bpchar_source_form;
    auto source_form = unknown;
    if (raw_varlena != nullptr) {
        if (VARATT_IS_EXTERNAL_ONDISK(raw_varlena))
            source_form = external;
        else if (VARATT_IS_COMPRESSED(raw_varlena))
            source_form = compressed;
        else if (VARATT_IS_4B_U(raw_varlena))
            source_form = four_byte_uncompressed;
        else if (VARATT_IS_SHORT(raw_varlena))
            source_form = short_header;
    }
    bioseq::postgres::support::runtime_diagnostics::record_bpchar_source_form(
        source_form,
        bioseq::postgres::support::runtime_diagnostics::bpchar_source_site::vector_copy);
}

/**
 * Free owned variable-length values and empty the batch for reuse.
 * Keep the column buffers ready for the next batch.
 */
void cleanup_vector_slot_contents(
    vector_tuple_table_slot_storage* vector_slot,
    bioseq::postgres::support::runtime_diagnostics::vector_payload_cleanup_kind cleanup_kind) {
    bioseq::postgres::support::runtime_diagnostics::record_vector_cleanup(cleanup_kind);
    TupleDesc tuple_descriptor = vector_slot->virtual_slot.base.tts_tupleDescriptor;
    if (tuple_descriptor == nullptr)
        return;

    for (int attribute_index = 0; attribute_index < tuple_descriptor->natts; ++attribute_index) {
        vector_column_storage* column = reinterpret_cast<vector_column_storage*>(
            DatumGetPointer(vector_slot->virtual_slot.base.tts_values[attribute_index]));
        if (column == nullptr)
            continue;

        if (column->storage_kind == vector_storage_kind::owned_varlena_datum) {
            for (uint32 row_index = 0; row_index < column->lane_count; ++row_index) {
                Datum& datum = column->data[row_index];
                if (!column->is_null[row_index] && DatumGetPointer(datum) != nullptr) {
                    bioseq::postgres::support::runtime_diagnostics::
                        record_vector_payload_cleanup_free(cleanup_kind);
                    pfree(DatumGetPointer(datum));
                }
                datum = static_cast<Datum>(0);
            }
        }
        std::fill_n(column->is_null.data(), vector_column_capacity, true);
        column->lane_count = 0;
    }

    std::fill_n(vector_slot->row_is_active.data(), vector_column_capacity, true);
    vector_slot->row_count = 0;
}

/** Clear the batch contents, then clear the PostgreSQL virtual siot. */
void clear_vector_tuple_slot(TupleTableSlot* slot) {
    cleanup_vector_slot_contents(
        reinterpret_cast<vector_tuple_table_slot_storage*>(slot),
        bioseq::postgres::support::runtime_diagnostics::vector_payload_cleanup_kind::slot_clear);
    TTSOpsVirtual.clear(slot);
}

/** Free batch values and column buffers when PostgreSQL releases the slot. */
void release_vector_tuple_slot(TupleTableSlot* slot) {
    auto* vector_slot = reinterpret_cast<vector_tuple_table_slot_storage*>(slot);
    cleanup_vector_slot_contents(
        vector_slot,
        bioseq::postgres::support::runtime_diagnostics::vector_payload_cleanup_kind::slot_release);
    TTSOpsVirtual.clear(slot);

    TupleDesc tuple_descriptor = slot->tts_tupleDescriptor;
    if (tuple_descriptor == nullptr) {
        TTSOpsVirtual.release(slot);
        return;
    }
    for (int attribute_index = 0; attribute_index < tuple_descriptor->natts; ++attribute_index) {
        vector_column_storage* column = reinterpret_cast<vector_column_storage*>(
            DatumGetPointer(slot->tts_values[attribute_index]));
        if (column == nullptr)
            continue;
        pfree(column->data);
        pfree(column);
        slot->tts_values[attribute_index] = static_cast<Datum>(0);
        slot->tts_isnull[attribute_index] = true;
    }
    TTSOpsVirtual.release(slot);
}

/** Materializing this slot needs no copy because it already owns its values. */
void materialize_vector_tuple_slot(TupleTableSlot*) {
    /* Fixed-size values are inline, and  variable-length values have owned copies. */
}

/** Report that PostgreSQL's generic slot-copy operation cannot copy a batch */
void copy_vector_tuple_slot(TupleTableSlot*, TupleTableSlot*) {
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("copying a vector batch slot is not supported")));
}

/** Rejects conversion of a vector batch to a heap tuple. */
HeapTuple copy_vector_heap_tuple(TupleTableSlot*) {
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("a vector batch slot cannot be converted to a heap tuple")));
    pg_unreachable();
}

/** Rejects conversion of a vector batch to a minimal tuple. */
MinimalTuple copy_vector_minimal_tuple(TupleTableSlot*
#if PG_VERSION_NUM >= 180000
    // PostgreSQL 18 adds an argument for reserved bytes before the tuple.
    // The batch conversion remains unsupported.
    , Size
#endif
) {
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("a vector batch slot cannot be converted to a minimal tuple")));
    pg_unreachable();
}

/** Return the PostgreSQL slot callbacks for storing and freeing vector batches. */
[[nodiscard]] const TupleTableSlotOps* vector_tuple_slot_ops() {
    static const TupleTableSlotOps operations = [] {
        TupleTableSlotOps configured_operations = TTSOpsVirtual;
        configured_operations.base_slot_size = sizeof(vector_tuple_table_slot_storage);
        configured_operations.clear = clear_vector_tuple_slot;
        configured_operations.release = release_vector_tuple_slot;
        configured_operations.materialize = materialize_vector_tuple_slot;
        configured_operations.copyslot = copy_vector_tuple_slot;
        configured_operations.copy_heap_tuple = copy_vector_heap_tuple;
        configured_operations.copy_minimal_tuple = copy_vector_minimal_tuple;
        return configured_operations;
    }();
    return &operations;
}

} // namespace

/** Tests whether a slot uses Bioseq's vector batch operations. */
export [[nodiscard]] bool is_vector_tuple_table_slot(const TupleTableSlot* slot) noexcept {
    return slot != nullptr && slot->tts_ops == vector_tuple_slot_ops();
}

/** Create a vector slot with one empty, owned column, per tuple attribute. */
export TupleTableSlot* create_vector_tuple_table_slot(TupleDesc tuple_desc) {
    TupleTableSlot* slot =
        MakeTupleTableSlot(CreateTupleDescCopy(tuple_desc), vector_tuple_slot_ops()
#if PG_VERSION_NUM >= 190000
                           , 0  // Preserve the pre-PG19 slot flags.
#endif
        );

    TupleDesc slot_tuple_descriptor = slot->tts_tupleDescriptor;

    vector_tuple_table_slot_storage* vector_slot =
        reinterpret_cast<vector_tuple_table_slot_storage*>(slot);

    /* No rows are active before the first batch is populated. */
    std::fill_n(vector_slot->row_is_active.data(), vector_column_capacity, false);

    for (int attribute_index = 0; attribute_index < slot_tuple_descriptor->natts;
         ++attribute_index) {
        const Oid column_type_oid = bioseq::postgres::support::tuple_descriptor_attribute(slot_tuple_descriptor, attribute_index)->atttypid;

        int16_t declared_type_width = 0;
        bool declared_stored_by_value = false;
        get_typlenbyval(column_type_oid, &declared_type_width, &declared_stored_by_value);

        const std::int16_t element_width = declared_type_width == -1
                                               ? static_cast<std::int16_t>(sizeof(Datum))
                                               : declared_type_width;
        const vector_storage_kind storage_kind = declared_type_width == -1
                                                     ? vector_storage_kind::owned_varlena_datum
                                                     : vector_storage_kind::inline_fixed;

        vector_column_storage* column =
            allocate_vector_column(element_width,
                                   declared_type_width == -1 || declared_stored_by_value,
                                   vector_slot->source_row_numbers.data(),
                                   storage_kind,
                                   slot->tts_mcxt);

        vector_slot->virtual_slot.base.tts_values[attribute_index] = PointerGetDatum(column);
        vector_slot->virtual_slot.base.tts_isnull[attribute_index] = false;
    }

    vector_slot->virtual_slot.base.tts_nvalid = tuple_desc->natts;

    return slot;
}

/**
 * Copy selected attributes of one batch row into a scalar virtual slot.
 * Attribute indices are zero-based. By-reference values still  point into the
 * batch. Keep those values unchanged while the scalar tuple is in use.
 */
export void extract_tuple_from_vector_slot(TupleTableSlot* output_slot,
                                           vector_tuple_table_slot_storage* vector_slot,
                                           std::int32_t row_index,
                                           List* attr_needed_list) {
    /*
     * ExecStoreVirtualTuple marks every attribute valid, including those this
     * scan did not request. Start with NULLs so omitted attributes cannot expose
     * values left over from the previous row.
     */
    bioseq::postgres::support::pg_slot_datums_view{output_slot}.clear_all_to_null();
    output_slot->tts_nvalid = 0;
    foreach_int(attribute_number, attr_needed_list) {
        if (!bioseq::postgres::support::tuple_descriptor_attribute(output_slot->tts_tupleDescriptor, attribute_number)->attisdropped) {
            vector_column_storage* column = reinterpret_cast<vector_column_storage*>(
                vector_slot->virtual_slot.base.tts_values[attribute_number]);
            std::int8_t* raw_column_data =
                reinterpret_cast<std::int8_t*>(column->data) + column->element_width * row_index;
            output_slot->tts_values[attribute_number] =
                fetch_att(raw_column_data, column->stored_by_value, column->element_width);
            output_slot->tts_isnull[attribute_number] = column->is_null[row_index];
        }
    }

    ExecStoreVirtualTuple(output_slot);
    /* Keep the physical relation OID. The jaller sets this row's TID. */
    output_slot->tts_tableOid = vector_slot->virtual_slot.base.tts_tableOid;
}

/**
 * Write or replace one row value in a column. Copy variable-length values into
 * the column's memory context, freeing any previous owned value at that  row.
 * Report an error when row_index is outside the batch capacity.
 */
export void write_datum_to_vector_column(vector_column_storage* column,
                                         Datum input_datum,
                                         bool is_null,
                                         std::int32_t row_index) {
    if (row_index < 0 || static_cast<uint32>(row_index) >= vector_column_capacity)
        bioseq::postgres::support::invoke_pg_as_cpp([row_index] {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("vector row index %d exceeds the batch capacity", row_index)));
        });

    if (column->storage_kind == vector_storage_kind::owned_varlena_datum &&
        static_cast<uint32>(row_index) < column->lane_count && !column->is_null[row_index] &&
        DatumGetPointer(column->data[row_index]) != nullptr) {
        bioseq::postgres::support::runtime_diagnostics::record_vector_payload_overwrite_free();
        pfree(DatumGetPointer(column->data[row_index]));
        column->data[row_index] = static_cast<Datum>(0);
    }
    column->is_null[row_index] = is_null;
    auto* write_position =
        reinterpret_cast<int8_t*>(column->data) + column->element_width * row_index;

    if (!is_null) {
        if (column->storage_kind == vector_storage_kind::owned_varlena_datum) {
            note_owned_varlena_source_form(input_datum);
            const Pointer original_pointer = DatumGetPointer(input_datum);
            auto* varlena_source = bioseq::postgres::support::invoke_pg_as_cpp(
                [input_datum] { return PG_DETOAST_DATUM_PACKED(input_datum); });
            const Size data_length = VARSIZE_ANY(varlena_source);
            void* owned_copy = bioseq::postgres::support::invoke_pg_as_cpp([column, data_length] {
                return MemoryContextAlloc(column->storage_context, data_length);
            });
            std::memcpy(owned_copy, varlena_source, data_length);
            bioseq::postgres::support::runtime_diagnostics::record_vector_payload_allocation();
            if (reinterpret_cast<Pointer>(varlena_source) != original_pointer) {
                bioseq::postgres::support::runtime_diagnostics::record_detoast_temporary_free();
                pfree(varlena_source);
            }
            *reinterpret_cast<Datum*>(write_position) = PointerGetDatum(owned_copy);
        } else if (column->stored_by_value) {
            store_att_byval(write_position, input_datum, column->element_width);
        } else {
            std::memcpy(write_position, DatumGetPointer(input_datum), column->element_width);
        }
    } else {
        std::memset(write_position, 0, column->element_width);
    }

    column->lane_count = std::max(column->lane_count, static_cast<uint32>(row_index + 1));
}

/**
 * Copy all attributes of a scalar tuple into the requested batch row.
 * Update each column's populated count. The caller sets the batch row count
 * and source-row mapping.
 */
export void write_tuple_to_vector_slot(TupleTableSlot* input_slot,
                                       vector_tuple_table_slot_storage* vector_slot,
                                       std::int32_t row_index) {
    TupleDesc tuple_descriptor = input_slot->tts_tupleDescriptor;

    for (int attribute_index = 0; attribute_index < tuple_descriptor->natts; ++attribute_index) {
        vector_column_storage* column = reinterpret_cast<vector_column_storage*>(
            vector_slot->virtual_slot.base.tts_values[attribute_index]);
        write_datum_to_vector_column(column,
                                     input_slot->tts_values[attribute_index],
                                     input_slot->tts_isnull[attribute_index],
                                     row_index);
    }
}

/** Free owned row values and reset the batch while keeping its column buffers. */
export void reset_vector_tuple_table_slot(vector_tuple_table_slot_storage* vector_slot) {
    cleanup_vector_slot_contents(vector_slot,
                                 bioseq::postgres::support::runtime_diagnostics::
                                     vector_payload_cleanup_kind::explicit_cleanup);
}
