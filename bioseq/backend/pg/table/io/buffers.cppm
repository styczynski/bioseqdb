/**
 * @file
 * @brief Shared allocation and cleanup helpers for table-I/O chunk buffers.
 */
module;

#include <bioseq/pg_compat.h>

#include <cstdint>

export module bioseq.postgres.table.io:buffers;

import bioseq.postgres.storage.metadata;
import bioseq.postgres.support;

namespace {

/** @return Whether a StringInfo is owned by the shared chunk cache. */
[[nodiscard]] bool is_cached_string_info(StringInfo buffer) noexcept {
    return reinterpret_cast<cached_string_info*>(buffer)->cached;
}

}  // namespace

/**
 * @brief Allocate per-column arrays for one decoded chunk group.
 * @param column_count Number of entries in `column_mask`.
 * @param column_mask Columns whose existence and value arrays are required.
 * @param row_count Number of rows represented by the chunk group.
 * @return Zero-initialized chunk metadata owned by the current memory context.
 */
export [[nodiscard]] chunk_data* allocate_chunk_data(
        std::uint32_t column_count,
        const bool* column_mask,
        std::uint32_t row_count) {
    chunk_data* data = bioseq::postgres::support::allocate_zeroed_object<chunk_data>();
    data->present_values = bioseq::postgres::support::allocate_zeroed_array<bool*>(column_count);
    data->values_by_column = bioseq::postgres::support::allocate_zeroed_array<Datum*>(column_count);
    data->referenced_value_buffers =
        bioseq::postgres::support::allocate_zeroed_storage<StringInfo>(column_count * sizeof(cached_string_info));
    data->column_count = column_count;
    data->row_count = row_count;

    for (std::uint32_t column_index = 0;
         column_index < column_count;
         ++column_index) {
        if (!column_mask[column_index]) {
            continue;
        }
        data->present_values[column_index] = bioseq::postgres::support::allocate_zeroed_array<bool>(row_count);
        data->values_by_column[column_index] = bioseq::postgres::support::allocate_zeroed_array<Datum>(row_count);
        data->referenced_value_buffers[column_index] = nullptr;
    }
    return data;
}

/**
 * @brief Release decoded value buffers that are not owned by the shared cache.
 * @param data Chunk metadata; null is accepted.
 */
export void free_chunk_value_buffers(chunk_data* data) noexcept {
    if (data == nullptr) {
        return;
    }

    for (std::uint32_t column_index = 0;
         column_index < data->column_count;
         ++column_index) {
        StringInfo buffer = data->referenced_value_buffers[column_index];
        if (buffer != nullptr && !is_cached_string_info(buffer)) {
            pfree(buffer->data);
            pfree(buffer);
        }
    }
}

/**
 * @brief Release existence/value arrays and the chunk metadata container.
 * @param data Chunk metadata; null is accepted.
 *
 * Call `free_chunk_value_buffers()` first when uncached serialized buffers
 * must also be released.
 */
export void free_chunk_data(chunk_data* data) noexcept {
    if (data == nullptr) {
        return;
    }

    for (std::uint32_t column_index = 0;
         column_index < data->column_count;
         ++column_index) {
        if (data->present_values[column_index] != nullptr) {
            pfree(data->present_values[column_index]);
        }
        if (data->values_by_column[column_index] != nullptr) {
            pfree(data->values_by_column[column_index]);
        }
    }

    pfree(data->present_values);
    pfree(data->values_by_column);
    pfree(data);
}
