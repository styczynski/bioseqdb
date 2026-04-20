/*-------------------------------------------------------------------------
 *
 * columnar_compression.cppm
 *
 * Compression / decompression dispatch over the configured compression
 * algorithms (none, pglz, optional lz4, optional zstd). The pglz path
 * also serialises a small header (raw size + varlena) at the start of
 * the compressed buffer so the decompressor can size the output without
 * a separate sidecar.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include "types.h"

#if HAVE_CITUS_LIBLZ4
#include <lz4.h>
#endif

#if HAVE_LIBZSTD
#include <zstd.h>
#endif

export module storage_compression;

import pg_utils;

/*
 * Header prepended to a pglz-compressed buffer; combines the standard
 * varlena length header with a separate rawsize field so decompression
 * can allocate the right output buffer without external metadata.
 */
struct ColumnarCompressHeader
{
	int32_t vl_len_;              /* varlena header (do not touch directly!) */
	int32_t rawsize;
};

/* Inline helpers replacing the original token-pasting macros. They accept
 * the raw varlena byte pointer (typically StringInfo->data) and reinterpret
 * its prefix as ColumnarCompressHeader. */

[[gnu::always_inline]] inline int32_t columnar_compress_hdrsz()
{
	return static_cast<int32_t>(sizeof(ColumnarCompressHeader));
}

[[gnu::always_inline]] [[nodiscard]] inline int32_t columnar_compress_rawsize(const char* compressed_buffer)
{
	return reinterpret_cast<const ColumnarCompressHeader *>(compressed_buffer)->rawsize;
}

[[gnu::always_inline]] [[nodiscard]] inline char* columnar_compress_rawdata(char* compressed_buffer)
{
	return compressed_buffer + columnar_compress_hdrsz();
}

[[gnu::always_inline]] inline void columnar_compress_set_rawsize(char* compressed_buffer, int32_t raw_size)
{
	reinterpret_cast<ColumnarCompressHeader *>(compressed_buffer)->rawsize = raw_size;
}


/*
 * CompressBuffer compresses the given buffer with the given compression type
 * outputBuffer enlarged to contain compressed data. The function returns true
 * if compression is done, returns false if compression is not done.
 * outputBuffer is valid only if the function returns true.
 */
export bool
CompressBuffer(StringInfo inputBuffer,
			   StringInfo outputBuffer,
			   CompressionType compressionType,
			   int compressionLevel)
{
	switch (compressionType)
	{
#if HAVE_CITUS_LIBLZ4
		case COMPRESSION_LZ4:
		{
			int maximumLength = LZ4_compressBound(inputBuffer->len);

			resetStringInfo(outputBuffer);
			enlargeStringInfo(outputBuffer, maximumLength);

			int compressedSize = LZ4_compress_default(inputBuffer->data,
													  outputBuffer->data,
													  inputBuffer->len, maximumLength);
			if (compressedSize <= 0)
			{
				pg_log(DEBUG1,
					 "failure in LZ4_compress_default, input size={}, output size={}",
					 inputBuffer->len, maximumLength);
				return false;
			}

			pg_log(DEBUG1, "compressed {} bytes to {} bytes", inputBuffer->len,
				 compressedSize);

			outputBuffer->len = compressedSize;
			return true;
		}
#endif

#if HAVE_LIBZSTD
		case COMPRESSION_ZSTD:
		{
			int maximumLength = ZSTD_compressBound(inputBuffer->len);

			resetStringInfo(outputBuffer);
			enlargeStringInfo(outputBuffer, maximumLength);

			size_t compressedSize = ZSTD_compress(outputBuffer->data,
												  outputBuffer->maxlen,
												  inputBuffer->data,
												  inputBuffer->len,
												  compressionLevel);

			if (ZSTD_isError(compressedSize))
			{
				PgReport(WARNING).msg("zstd compression failed").detail("{}", ZSTD_getErrorName(compressedSize));
				return false;
			}

			outputBuffer->len = compressedSize;
			return true;
		}
#endif

		case COMPRESSION_PG_LZ:
		{
			uint64_t maximumLength = PGLZ_MAX_OUTPUT(inputBuffer->len) +
								   columnar_compress_hdrsz();
			bool compressionResult = false;

			resetStringInfo(outputBuffer);
			enlargeStringInfo(outputBuffer, maximumLength);

			int32_t compressedByteCount = pglz_compress(inputBuffer->data,
													  inputBuffer->len,
													  columnar_compress_rawdata(outputBuffer->data),
													  PGLZ_strategy_always);
			if (compressedByteCount >= 0)
			{
				columnar_compress_set_rawsize(outputBuffer->data, inputBuffer->len);
				SET_VARSIZE_COMPRESSED(outputBuffer->data,
									   compressedByteCount + columnar_compress_hdrsz());
				compressionResult = true;
			}

			if (compressionResult)
			{
				outputBuffer->len = VARSIZE(outputBuffer->data);
			}

			return compressionResult;
		}

		default:
		{
			return false;
		}
	}
}


/*
 * DecompressBuffer decompresses the given buffer with the given compression
 * type. This function returns the buffer as-is when no compression is applied.
 */
export StringInfo
DecompressBuffer(StringInfo buffer,
				 CompressionType compressionType,
				 uint64_t decompressedSize)
{
	switch (compressionType)
	{
		case COMPRESSION_NONE:
		{
			return buffer;
		}

#if HAVE_CITUS_LIBLZ4
		case COMPRESSION_LZ4:
		{
			StringInfo decompressedBuffer = makeStringInfo();
			enlargeStringInfo(decompressedBuffer, decompressedSize);

			int lz4DecompressSize = LZ4_decompress_safe(buffer->data,
														decompressedBuffer->data,
														buffer->len,
														decompressedSize);

			if (lz4DecompressSize != decompressedSize)
			{
				PgReport(ERROR).msg("cannot decompress the buffer").detail("Expected {} bytes, but received {} bytes", decompressedSize, lz4DecompressSize);
			}

			decompressedBuffer->len = decompressedSize;

			return decompressedBuffer;
		}
#endif

#if HAVE_LIBZSTD
		case COMPRESSION_ZSTD:
		{
			StringInfo decompressedBuffer = makeStringInfo();
			enlargeStringInfo(decompressedBuffer, decompressedSize);

			size_t zstdDecompressSize = ZSTD_decompress(decompressedBuffer->data,
														decompressedSize,
														buffer->data,
														buffer->len);
			if (ZSTD_isError(zstdDecompressSize))
			{
				PgReport(ERROR).msg("zstd decompression failed").detail("{}", ZSTD_getErrorName(zstdDecompressSize));
			}

			if (zstdDecompressSize != decompressedSize)
			{
				PgReport(ERROR).msg("unexpected decompressed size").detail("Expected {}, received {}", decompressedSize, zstdDecompressSize);
			}

			decompressedBuffer->len = decompressedSize;

			return decompressedBuffer;
		}
#endif

		case COMPRESSION_PG_LZ:
		{
			uint32_t compressedDataSize = VARSIZE(buffer->data) - columnar_compress_hdrsz();
			uint32_t decompressedDataSize = columnar_compress_rawsize(buffer->data);

			if (compressedDataSize + columnar_compress_hdrsz() != buffer->len)
			{
				PgReport(ERROR).msg("cannot decompress the buffer").detail("Expected {} bytes, but received {} bytes", compressedDataSize, buffer->len);
			}

			char *decompressedData = make1<char>(decompressedDataSize);

			int32_t decompressedByteCount = pglz_decompress(columnar_compress_rawdata(buffer->data),
														  compressedDataSize,
														  decompressedData,
														  decompressedDataSize, true);

			if (decompressedByteCount < 0)
			{
				PgReport(ERROR).msg("cannot decompress the buffer").detail("compressed data is corrupted");
			}

			StringInfo decompressedBuffer = make0<StringInfoData>();
			decompressedBuffer->data = decompressedData;
			decompressedBuffer->len = decompressedDataSize;
			decompressedBuffer->maxlen = decompressedDataSize;

			return decompressedBuffer;
		}

		default:
		{
			pg_log(ERROR, "unexpected compression type: {}", compressionType);
		}
	}
}
