/* Citus full name as a string */
#define CITUS_NAME "Citus"

/* Citus edition as a string */
#define CITUS_EDITION "community"

/* Extension version expected by this Citus build */
#define CITUS_EXTENSIONVERSION "14.0-1"

/* Citus major version as a string */
#define CITUS_MAJORVERSION "14.0"

/* Citus version as a string */
#define CITUS_VERSION "14.0devel"

/* Citus version as a number */
#define CITUS_VERSION_NUM 140000

/* A string containing the version number, platform, and C compiler */
#define CITUS_VERSION_STR "Citus 14.0devel on arm-apple-darwin24.6.0, compiled by Apple clang version 16.0.0 (clang-1600.0.26.6), 64-bit"

/* Define to 1 if you have the `curl' library (-lcurl). */
#define HAVE_LIBCURL 1

/* Define to 1 if you have the `liblz4' library (-llz4). */
#undef HAVE_CITUS_LIBLZ4

/* Define to 1 if you have the `libzstd' library (-lzstd). */
#undef HAVE_LIBZSTD

/* Base URL for statistics collection and update checks */
#define REPORTS_BASE_URL "https://reports.citusdata.com"
