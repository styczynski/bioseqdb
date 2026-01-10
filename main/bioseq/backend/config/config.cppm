module;
#include "common.h"
#include "types.h"

export module config;

export const config_enum_entry bioseq_compression_options[] =
{
	{ "none", COMPRESSION_NONE, false },
	{ "pglz", COMPRESSION_PG_LZ, false },
#if HAVE_CITUS_LIBLZ4
	{ "lz4", COMPRESSION_LZ4, false },
#endif
#if HAVE_LIBZSTD
	{ "zstd", COMPRESSION_ZSTD, false },
#endif
	{ NULL, 0, false }
};

/*
 * ParseCompressionType converts a string to a compression type.
 * For compression algorithms that are invalid or not compiled, it
 * returns COMPRESSION_TYPE_INVALID.
 */
export CompressionType ParseCompressionType(const char *compressionTypeString)
{
	Assert(compressionTypeString != NULL);

	for (int compressionIndex = 0;
		 bioseq_compression_options[compressionIndex].name != NULL;
		 compressionIndex++)
	{
		const char *compressionName = bioseq_compression_options[compressionIndex].name;
		if (strncmp(compressionTypeString, compressionName, NAMEDATALEN) == 0)
		{
			return static_cast<CompressionType>(bioseq_compression_options[compressionIndex].val);
		}
	}

	return COMPRESSION_TYPE_INVALID;
}


/*
 * CompressionTypeStr returns string representation of a compression type.
 * For compression algorithms that are invalid or not compiled, it
 * returns NULL.
 */
export const char* CompressionTypeStr(CompressionType requestedType)
{
	for (int compressionIndex = 0;
		 bioseq_compression_options[compressionIndex].name != NULL;
		 compressionIndex++)
	{
		CompressionType compressionType =
			static_cast<CompressionType>(bioseq_compression_options[compressionIndex].val);
		if (compressionType == requestedType)
		{
			return bioseq_compression_options[compressionIndex].name;
		}
	}

	return NULL;
}
