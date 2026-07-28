module;
#include "common.h"

#include <optional>
#include <string_view>

export module bioseq.config;

export import bioseq.sql_guc_settings;
export import bioseq.scheduler.sql_guc_settings;
export import bioseq.storage.sql_guc_settings;
export import bioseq.aligner.sql_guc_settings;

export using CompressionType = bioseq::CompressionType;
export inline constexpr CompressionType COMPRESSION_TYPE_INVALID =
    CompressionType::invalid;
export inline constexpr CompressionType COMPRESSION_NONE =
    CompressionType::none;
export inline constexpr CompressionType COMPRESSION_PG_LZ =
    CompressionType::pglz;
#if defined(HAVE_CITUS_LIBLZ4) && HAVE_CITUS_LIBLZ4
export inline constexpr CompressionType COMPRESSION_LZ4 =
    CompressionType::lz4;
#endif
#if defined(HAVE_LIBZSTD) && HAVE_LIBZSTD
export inline constexpr CompressionType COMPRESSION_ZSTD =
    CompressionType::zstd;
#endif
export inline constexpr int COMPRESSION_COUNT =
    static_cast<int>(CompressionType::count);

export constexpr const char* BIOSEQ_COLUMN_MODULE_NAME = "bioseq_column";
export constexpr const char* BIOSEQ_COLUMN_SETOPTIONS_HOOK_SYM =
    "BioseqColumnTableSetOptions_hook";

export constexpr const char* OPTION_NAME_COMPRESSION_TYPE = "compression";
export constexpr const char* OPTION_NAME_STRIPE_ROW_COUNT = "stripe_row_limit";
export constexpr const char* OPTION_NAME_CHUNK_ROW_COUNT =
    "chunk_group_row_limit";

export constexpr int STRIPE_ROW_COUNT_MINIMUM =
    bioseq::stripe_row_count_minimum;
export constexpr int STRIPE_ROW_COUNT_MAXIMUM =
    bioseq::stripe_row_count_maximum;
export constexpr int CHUNK_ROW_COUNT_MINIMUM =
    bioseq::chunk_row_count_minimum;
export constexpr int CHUNK_ROW_COUNT_MAXIMUM =
    bioseq::chunk_row_count_maximum;
export constexpr int COMPRESSION_LEVEL_MIN =
    bioseq::compression_level_minimum;
export constexpr int COMPRESSION_LEVEL_MAX =
    bioseq::compression_level_maximum;

export constexpr int DEFAULT_STRIPE_ROW_COUNT =
    bioseq::default_stripe_row_count;
export constexpr int DEFAULT_CHUNK_ROW_COUNT =
    bioseq::default_chunk_row_count;
export constexpr int DEFAULT_COMPRESSION_TYPE =
    static_cast<int>(bioseq::default_compression_type);

export constexpr int BIOSEQ_COLUMN_VERSION_MAJOR = 2;
export constexpr int BIOSEQ_COLUMN_VERSION_MINOR = 0;
export constexpr int BIOSEQ_COLUMN_TUPLE_COST_MULTIPLIER = 10;
export constexpr int BIOSEQ_COLUMN_POSTSCRIPT_SIZE_LENGTH = 1;
export constexpr int BIOSEQ_COLUMN_POSTSCRIPT_SIZE_MAX = 256;
export constexpr Size BIOSEQ_COLUMN_BYTES_PER_PAGE =
    (BLCKSZ - SizeOfPageHeaderData);
export constexpr int BIOSEQ_COLUMN_ROW_MASK_CHUNK_SIZE = 10000;
export constexpr int CUSTOM_SCAN_VECTORIZED_AGGREGATE = 1;

/** Parse a supported compression method name. */
export CompressionType ParseCompressionType(const char* value)
{
    Assert(value != nullptr);
    if (strncmp(value, "none", NAMEDATALEN) == 0) {
        return CompressionType::none;
    }
    if (strncmp(value, "pglz", NAMEDATALEN) == 0) {
        return CompressionType::pglz;
    }
#if defined(HAVE_CITUS_LIBLZ4) && HAVE_CITUS_LIBLZ4
    if (strncmp(value, "lz4", NAMEDATALEN) == 0) {
        return CompressionType::lz4;
    }
#endif
#if defined(HAVE_LIBZSTD) && HAVE_LIBZSTD
    if (strncmp(value, "zstd", NAMEDATALEN) == 0) {
        return CompressionType::zstd;
    }
#endif
    return CompressionType::invalid;
}

/** Return the SQL spelling of a supported compression method. */
export const char* CompressionTypeStr(CompressionType value)
{
    switch (value) {
        case CompressionType::none:
            return "none";
        case CompressionType::pglz:
            return "pglz";
#if defined(HAVE_CITUS_LIBLZ4) && HAVE_CITUS_LIBLZ4
        case CompressionType::lz4:
            return "lz4";
#endif
#if defined(HAVE_LIBZSTD) && HAVE_LIBZSTD
        case CompressionType::zstd:
            return "zstd";
#endif
        case CompressionType::invalid:
        case CompressionType::count:
            return nullptr;
    }
    return nullptr;
}

export using IndexPayloadStorageBackend =
    bioseq::IndexPayloadStorageBackend;

export constexpr int index_payload_storage_backend_wire_value(
        IndexPayloadStorageBackend backend) noexcept {
    return static_cast<int>(backend);
}

export constexpr bool is_valid_index_payload_storage_backend_wire_value(
        int value) noexcept {
    return value == index_payload_storage_backend_wire_value(
            IndexPayloadStorageBackend::postgres_relation)
        || value == index_payload_storage_backend_wire_value(
            IndexPayloadStorageBackend::mapped_file);
}

export constexpr std::optional<IndexPayloadStorageBackend>
decode_index_payload_storage_backend(int value) noexcept {
    return is_valid_index_payload_storage_backend_wire_value(value)
        ? std::optional{static_cast<IndexPayloadStorageBackend>(value)}
        : std::nullopt;
}

export using MpfFormat5LookupRoute =
    bioseq::aligner::MpfFormat5LookupRoute;

export constexpr int mpf_format5_lookup_route_wire_value(
        MpfFormat5LookupRoute route) noexcept {
    return static_cast<int>(route);
}

export constexpr std::optional<MpfFormat5LookupRoute>
decode_mpf_format5_lookup_route(int value) noexcept {
    return value == mpf_format5_lookup_route_wire_value(
            MpfFormat5LookupRoute::format5_control_scans)
        || value == mpf_format5_lookup_route_wire_value(
            MpfFormat5LookupRoute::compact_h32_direct)
        ? std::optional{static_cast<MpfFormat5LookupRoute>(value)}
        : std::nullopt;
}

static_assert(index_payload_storage_backend_wire_value(
    IndexPayloadStorageBackend::postgres_relation) == 0);
static_assert(index_payload_storage_backend_wire_value(
    IndexPayloadStorageBackend::mapped_file) == 1);
static_assert(decode_index_payload_storage_backend(0)
    == IndexPayloadStorageBackend::postgres_relation);
static_assert(decode_index_payload_storage_backend(1)
    == IndexPayloadStorageBackend::mapped_file);
static_assert(!decode_index_payload_storage_backend(-1).has_value());
static_assert(!decode_index_payload_storage_backend(2).has_value());
static_assert(decode_mpf_format5_lookup_route(0)
    == MpfFormat5LookupRoute::format5_control_scans);
static_assert(decode_mpf_format5_lookup_route(1)
    == MpfFormat5LookupRoute::compact_h32_direct);
static_assert(!decode_mpf_format5_lookup_route(-1).has_value());
static_assert(!decode_mpf_format5_lookup_route(2).has_value());
