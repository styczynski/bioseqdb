/**
 * @file compression.cppm
 * @brief Compress and decompress column buffers with PGLZ, LZ4 or Zstandard.
 *
 * LZ4 and Zstandard are available when included in the build. PGLZ buffers
 * begin  with a header containing the encoded length and original byte count.
 * Decompression checks the result against the size stored in column metadata.
 */
module;

#include <bioseq/pg_compat.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#if HAVE_CITUS_LIBLZ4
#include <lz4.h>
#endif

#if HAVE_LIBZSTD
#include <zstd.h>
#endif

export module bioseq.postgres.storage.compression;

import bioseq.common.checked_arithmetic;
import bioseq.common.logging;
import bioseq.postgres.configuration.core;
import bioseq.postgres.support;

/**
 * @brief Header containing a PGLZ buffer's encoded and original sizes.
 *
 * Keep the PostgreSQL varlena header first. Its macros read the encoded length,
 * and raw_size is checked against column metadata before decompression.
 */
struct compressed_column_header
{
	int32_t vl_len_;              /* PostgreSQL varlena header Access through its macros. */
	int32_t raw_size;
};

static_assert(std::is_standard_layout_v<compressed_column_header>);
static_assert(sizeof(compressed_column_header) == 8);
static_assert(offsetof(compressed_column_header, vl_len_) == 0);
static_assert(offsetof(compressed_column_header, raw_size) == 4);
static_assert(sizeof(decltype(compressed_column_header::vl_len_)) == 4);
static_assert(sizeof(decltype(compressed_column_header::raw_size)) == 4);
inline constexpr uint64_t pglz_maximum_output_overhead = 4;
static_assert(
	static_cast<uint64_t>(PGLZ_MAX_OUTPUT(0))
	== pglz_maximum_output_overhead);

/**
 * @brief Report that a column buffer exceeds PostgreSQL's allocation limit.
 * @param[in] operation Operation named in the error message.
 * @param[in] byte_count Requested size in bytes.
 */
[[noreturn]] static void
raise_compression_size_error(const char* operation, uint64_t byte_count)
{
	bioseq::postgres::support::raise_sql_error(
		ERRCODE_PROGRAM_LIMIT_EXCEEDED,
		{
			.message = fmt::format(
				"cannot {} a column buffer of {} bytes",
				operation,
				byte_count),
			.detail = fmt::format(
				"PostgreSQL StringInfo allocations are limited to {} bytes.",
				static_cast<uint64_t>(MaxAllocSize - 1)),
		});
}

/**
 * @brief Report a corrupt compressed column  buffer as a PostgreSQL error.
 * @param[in] detail Explanation included in the error report.
 */
[[noreturn]] static void
raise_corrupt_compressed_buffer(std::string detail)
{
	bioseq::postgres::support::raise_sql_error(
		ERRCODE_DATA_CORRUPTED,
		{
			.message = "cannot decompress the column buffer",
			.detail = std::move(detail),
		});
}

/**
 * @brief Reject invalid input or output buffers before compression.
 * @param[in] input_buffer Source buffer with data and a nonnegative length.
 * @param[in] output_buffer Initialized destination with allocated storage.
 */
static void
validate_compression_buffers(StringInfo input_buffer, StringInfo output_buffer)
{
	if (input_buffer == nullptr || input_buffer->data == nullptr
		|| input_buffer->len < 0)
	{
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{
				.message = "cannot compress an invalid column input buffer",
			});
	}
	if (output_buffer == nullptr || output_buffer->data == nullptr
		|| output_buffer->maxlen <= 0)
	{
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{
				.message = "cannot write to an invalid column compression buffer",
			});
	}
}

/**
 * @brief Check that a byte count fits in a PostgreSQL StringInfo allocaton.
 * @param[in] byte_count Requested size, in bytes.
 * @param[in] operation Operation named in a size-limit error.
 * @return The size as an int, after checking both allocation and integer limits.
 */
[[nodiscard]] static int
validate_string_info_size(uint64_t byte_count, const char* operation)
{
	const uint64_t maximum_string_info_size = std::min<uint64_t>(
		static_cast<uint64_t>((std::numeric_limits<int>::max)()),
		static_cast<uint64_t>(MaxAllocSize - 1));
	if (byte_count > maximum_string_info_size)
	{
		raise_compression_size_error(operation, byte_count);
	}
	return static_cast<int>(byte_count);
}

/**
 * @brief Read a compression algorithm from its SQL name.
 * @param[in] value Non-null, null-terminated name.
 * @return The algorithm, or invalid if this  build does not support the name.
 */
export [[nodiscard]] bioseq::compression_algorithm
parse_compression_algorithm(const char* value) noexcept
{
	Assert(value != nullptr);
	if (std::strncmp(value, "none", NAMEDATALEN) == 0)
	{
		return bioseq::compression_algorithm::none;
	}
	if (std::strncmp(value, "pglz", NAMEDATALEN) == 0)
	{
		return bioseq::compression_algorithm::pglz;
	}
#if HAVE_CITUS_LIBLZ4
	if (std::strncmp(value, "lz4", NAMEDATALEN) == 0)
	{
		return bioseq::compression_algorithm::lz4;
	}
#endif
#if HAVE_LIBZSTD
	if (std::strncmp(value, "zstd", NAMEDATALEN) == 0)
	{
		return bioseq::compression_algorithm::zstd;
	}
#endif
	return bioseq::compression_algorithm::invalid;
}

/**
 * @brief Return a compression algorithm's SQL name.
 * @param[in] value Algorithm to name.
 * @return A static string, or nullptr if this build does not support the value.
 */
export [[nodiscard]] const char*
compression_algorithm_name(bioseq::compression_algorithm value) noexcept
{
	switch (value)
	{
		case bioseq::compression_algorithm::none:
			return "none";
		case bioseq::compression_algorithm::pglz:
			return "pglz";
#if HAVE_CITUS_LIBLZ4
		case bioseq::compression_algorithm::lz4:
			return "lz4";
#endif
#if HAVE_LIBZSTD
		case bioseq::compression_algorithm::zstd:
			return "zstd";
#endif
		case bioseq::compression_algorithm::invalid:
		case bioseq::compression_algorithm::count:
			return nullptr;
	}
	return nullptr;
}

/// @brief Returns the fixed PGLZ column header size in bytes.
/// @return Header size used by both encoder and decoder.
[[gnu::always_inline]] [[nodiscard]] inline constexpr int32_t
compressed_column_header_size() noexcept
{
	return static_cast<int32_t>(sizeof(compressed_column_header));
}

/// @brief Reads the raw byte count from a validated PGLZ column header.
/// @param[in] compressed_buffer Buffer beginning with `compressed_column_header`.
/// @return Declared decompressed size in bytes.
[[gnu::always_inline]] [[nodiscard]] inline int32_t
compressed_column_raw_size(const char* compressed_buffer) noexcept
{
	return reinterpret_cast<const compressed_column_header*>(compressed_buffer)->raw_size;
}

/// @brief Locates the PGLZ payload after its fixed header.
/// @param[in] compressed_buffer Writable buffer beginning with the header.
/// @return Pointer to the first compressed payload byte.
[[gnu::always_inline]] [[nodiscard]] inline char*
compressed_column_payload(char* compressed_buffer) noexcept
{
	return compressed_buffer + compressed_column_header_size();
}

/// @brief Writes the raw byte count into a PGLZ column header.
/// @param[in,out] compressed_buffer Writable buffer beginning with the header.
/// @param[in] raw_size Decompressed size in bytes.
[[gnu::always_inline]] inline void
set_compressed_column_raw_size(char* compressed_buffer, int32_t raw_size) noexcept
{
	reinterpret_cast<compressed_column_header*>(compressed_buffer)->raw_size = raw_size;
}


/**
 * @brief Try to compress a serialized column buffer with the selected algorithm.
 * @param[in] input_buffer Source bytes, borrowed for this call.
 * @param[in,out] output_buffer Scratch buffer containing the result on success.
 * @param[in] compression_algorithm Compression algorithm to use.
 * @param[in] compression_level Zstandard compression level. Ignored by other algorithms.
 * @return true if compression succeeded. On false, the caller keeps the source bytes.
 * @note Invalid buffers and  sizes beyond PostgreSQL's limit raise errors.
 */
export bool
compress_column_buffer(StringInfo input_buffer,
			   StringInfo output_buffer,
			   bioseq::compression_algorithm compression_algorithm,
			   int compression_level)
{
	validate_compression_buffers(input_buffer, output_buffer);

	switch (compression_algorithm)
	{
#if HAVE_CITUS_LIBLZ4
		case bioseq::compression_algorithm::lz4:
		{
			const int maximum_length = LZ4_compressBound(input_buffer->len);
			if (maximum_length <= 0)
			{
				raise_compression_size_error(
					"compress", static_cast<uint64_t>(input_buffer->len));
			}
			const int output_capacity = validate_string_info_size(
				static_cast<uint64_t>(maximum_length), "compress");

			resetStringInfo(output_buffer);
			enlargeStringInfo(output_buffer, output_capacity);

			const int compressed_size = LZ4_compress_default(
				input_buffer->data,
				output_buffer->data,
				input_buffer->len,
				output_capacity);
			if (compressed_size <= 0)
			{
				bioseq_log_pg(DEBUG1,
					 "failure in LZ4_compress_default, input size={}, output size={}",
					 input_buffer->len, maximum_length);
				return false;
			}

			bioseq_log_pg(DEBUG1, "compressed {} bytes to {} bytes", input_buffer->len,
				 compressed_size);

			output_buffer->len = compressed_size;
			output_buffer->data[output_buffer->len] = '\0';
			return true;
		}
#endif

#if HAVE_LIBZSTD
		case bioseq::compression_algorithm::zstd:
		{
			const size_t maximum_length = ZSTD_compressBound(input_buffer->len);
			if (ZSTD_isError(maximum_length))
			{
				bioseq_report<logging_level::warning, logging_target::postgres>()
					.with_message("zstd compression bound failed")
					.with_detail("{}", ZSTD_getErrorName(maximum_length));
				return false;
			}
			const int output_capacity = validate_string_info_size(
				static_cast<uint64_t>(maximum_length), "compress");

			resetStringInfo(output_buffer);
			enlargeStringInfo(output_buffer, output_capacity);

			const size_t compressed_size = ZSTD_compress(output_buffer->data,
											  static_cast<size_t>(output_capacity),
											  input_buffer->data,
												  input_buffer->len,
												  compression_level);

			if (ZSTD_isError(compressed_size))
			{
				bioseq_report<logging_level::warning, logging_target::postgres>().with_message("zstd compression failed").with_detail("{}", ZSTD_getErrorName(compressed_size));
				return false;
			}
			if (compressed_size > static_cast<size_t>(output_capacity))
			{
				raise_compression_size_error(
					"store compressed", static_cast<uint64_t>(compressed_size));
			}

			output_buffer->len = static_cast<int>(compressed_size);
			output_buffer->data[output_buffer->len] = '\0';
			return true;
		}
#endif

		case bioseq::compression_algorithm::pglz:
		{
			const std::optional<uint64_t> maximum_payload_length =
				bioseq::common::checked_add(
					static_cast<uint64_t>(input_buffer->len),
					pglz_maximum_output_overhead);
			if (!maximum_payload_length.has_value()
				|| *maximum_payload_length
					> static_cast<uint64_t>((std::numeric_limits<int>::max)()))
			{
				raise_compression_size_error(
					"compress", static_cast<uint64_t>(input_buffer->len));
			}
			const int pglz_payload_capacity =
				static_cast<int>(*maximum_payload_length);
			const std::optional<uint64_t> maximum_length =
				bioseq::common::checked_add(
					static_cast<uint64_t>(pglz_payload_capacity),
					static_cast<uint64_t>(compressed_column_header_size()));
			if (!maximum_length.has_value())
			{
				raise_compression_size_error(
					"compress", static_cast<uint64_t>(input_buffer->len));
			}
			const int output_capacity =
				validate_string_info_size(*maximum_length, "compress");
			bool compression_result = false;

			resetStringInfo(output_buffer);
			enlargeStringInfo(output_buffer, output_capacity);

			const int32_t compressed_byte_count = pglz_compress(input_buffer->data,
													  input_buffer->len,
											  compressed_column_payload(output_buffer->data),
													  PGLZ_strategy_always);
			if (compressed_byte_count >= 0)
			{
				const std::optional<uint64_t> total_compressed_size =
					bioseq::common::checked_add(
						static_cast<uint64_t>(compressed_byte_count),
						static_cast<uint64_t>(compressed_column_header_size()));
				if (!total_compressed_size.has_value()
					|| *total_compressed_size
						> static_cast<uint64_t>(output_capacity))
				{
					raise_compression_size_error(
						"compress", static_cast<uint64_t>(input_buffer->len));
				}
				set_compressed_column_raw_size(
					output_buffer->data,
					static_cast<int32_t>(input_buffer->len));
				SET_VARSIZE_COMPRESSED(output_buffer->data,
					static_cast<int32_t>(*total_compressed_size));
				compression_result = true;
			}

			if (compression_result)
			{
				output_buffer->len = bioseq::postgres::support::varlena_4byte_size(output_buffer->data);
				output_buffer->data[output_buffer->len] = '\0';
			}

			return compression_result;
		}

		default:
		{
			return false;
		}
	}
}


/**
 * @brief Decompress a column buffer and check its expected size
 * @param[in] buffer Compressed bytes, or the original buffer for none.
 * @param[in] compression_algorithm Algorithm used to write the buffer.
 * @param[in] decompressed_size Expected number of bytes. Ignored for none.
 * @return The input for none, otherwise a buffer in the current PostgreSQL memory context.
 * @note Corrupt compressed dtta and size mismatches raise PostgreSQL errors.
 */
export StringInfo
decompress_column_buffer(StringInfo buffer,
					 bioseq::compression_algorithm compression_algorithm,
					 uint64_t decompressed_size)
{
	if (compression_algorithm != bioseq::compression_algorithm::none
		&& (buffer == nullptr || buffer->data == nullptr || buffer->len < 0))
	{
		raise_corrupt_compressed_buffer(
			"The compressed input does not contain a valid byte buffer.");
	}

	switch (compression_algorithm)
	{
		case bioseq::compression_algorithm::none:
		{
			return buffer;
		}

#if HAVE_CITUS_LIBLZ4
		case bioseq::compression_algorithm::lz4:
		{
			const int expected_size =
				validate_string_info_size(decompressed_size, "decompress");
			StringInfo decompressed_buffer = makeStringInfo();
			enlargeStringInfo(decompressed_buffer, expected_size);

			const int lz4_decompressed_size = LZ4_decompress_safe(
				buffer->data,
				decompressed_buffer->data,
				buffer->len,
				expected_size);

			if (lz4_decompressed_size != expected_size)
			{
				raise_corrupt_compressed_buffer(
					fmt::format(
						"Expected {} bytes, but received {} bytes",
						expected_size,
						lz4_decompressed_size));
			}

			decompressed_buffer->len = expected_size;
			decompressed_buffer->data[decompressed_buffer->len] = '\0';

			return decompressed_buffer;
		}
#endif

#if HAVE_LIBZSTD
		case bioseq::compression_algorithm::zstd:
		{
			const int expected_size =
				validate_string_info_size(decompressed_size, "decompress");
			StringInfo decompressed_buffer = makeStringInfo();
			enlargeStringInfo(decompressed_buffer, expected_size);

			const size_t zstd_decompress_size = ZSTD_decompress(
				decompressed_buffer->data,
				static_cast<size_t>(expected_size),
				buffer->data,
				static_cast<size_t>(buffer->len));
			if (ZSTD_isError(zstd_decompress_size))
			{
				raise_corrupt_compressed_buffer(
					fmt::format(
						"zstd decompression failed: {}",
						ZSTD_getErrorName(zstd_decompress_size)));
			}

			if (zstd_decompress_size != static_cast<size_t>(expected_size))
			{
				raise_corrupt_compressed_buffer(
					fmt::format(
						"Expected {} bytes, but received {} bytes",
						expected_size,
						zstd_decompress_size));
			}

			decompressed_buffer->len = expected_size;
			decompressed_buffer->data[decompressed_buffer->len] = '\0';

			return decompressed_buffer;
		}
#endif

		case bioseq::compression_algorithm::pglz:
		{
			if (buffer == nullptr || buffer->data == nullptr
				|| buffer->len < compressed_column_header_size())
			{
					raise_corrupt_compressed_buffer(
					"The PGLZ payload is shorter than its 8-byte header.");
			}
			if (!VARATT_IS_4B_C(buffer->data))
			{
				raise_corrupt_compressed_buffer(
					"The PGLZ payload does not have a compressed 4-byte "
					"varlena header.");
			}

			const uint32_t encoded_size = bioseq::postgres::support::varlena_4byte_size(buffer->data);
			if (encoded_size
				< static_cast<uint32_t>(compressed_column_header_size())
				|| encoded_size != static_cast<uint32_t>(buffer->len))
			{
				raise_corrupt_compressed_buffer(
					fmt::format(
						"The PGLZ header declares {} bytes, but the input "
						"contains {} bytes.",
						encoded_size,
						buffer->len));
			}

			const int32_t stored_raw_size =
				compressed_column_raw_size(buffer->data);
			if (stored_raw_size < 0
				|| static_cast<uint64_t>(stored_raw_size) != decompressed_size)
			{
				raise_corrupt_compressed_buffer(
					fmt::format(
						"The PGLZ header declares a raw size of {}, but {} "
						"bytes were requested.",
						stored_raw_size,
						decompressed_size));
			}
			const int decompressed_data_size =
				validate_string_info_size(decompressed_size, "decompress");
			const uint32_t compressed_data_size =
				encoded_size
				- static_cast<uint32_t>(compressed_column_header_size());
			if (compressed_data_size
				> static_cast<uint32_t>((std::numeric_limits<int32_t>::max)()))
			{
				raise_corrupt_compressed_buffer(
					"The PGLZ payload exceeds the decoder's signed size range.");
			}

			const size_t allocation_size =
				static_cast<size_t>(decompressed_data_size) + 1;
			char *decompressed_data =
				bioseq::postgres::support::allocate_zeroed_storage<char>(
					allocation_size);

			const int32_t decompressed_byte_count = pglz_decompress(
				compressed_column_payload(buffer->data),
				static_cast<int32_t>(compressed_data_size),
				decompressed_data,
				static_cast<int32_t>(decompressed_data_size),
				true);

			if (decompressed_byte_count != decompressed_data_size)
			{
				raise_corrupt_compressed_buffer(
					fmt::format(
						"Expected {} bytes, but the PGLZ decoder produced {} bytes.",
						decompressed_data_size,
						decompressed_byte_count));
			}

			StringInfo decompressed_buffer =
				bioseq::postgres::support::allocate_zeroed_object<StringInfoData>();
			decompressed_buffer->data = decompressed_data;
			decompressed_buffer->len = decompressed_data_size;
			decompressed_buffer->maxlen = decompressed_data_size + 1;
			decompressed_buffer->data[decompressed_buffer->len] = '\0';

			return decompressed_buffer;
		}

		default:
		{
			bioseq::postgres::support::raise_sql_error(
				ERRCODE_INVALID_PARAMETER_VALUE,
				{
					.message = fmt::format(
						"cannot decompress an unsupported compression type {}",
						static_cast<int>(compression_algorithm)),
				});
		}
	}
}
