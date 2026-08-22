/**
 * @file
 * @brief Canonical declarations for Bioseq build-time policy.
 *
 * This module is the authored input to the build-config generator. The build
 * compiles the generated module with the same module name, not this source.
 */
export module bioseq.build_config;

export namespace bioseq::build_config {

/**
 * @brief Selects the semantic template-flatten profile.
 * @remark Unit: profile_ordinal. Domain: 0..5. Default: 0U.
 */
inline constexpr unsigned int bioseq_template_flatten_profile [[bioseq::build_config(
    "BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_PROFILE", "profile_ordinal", "0..5")]] = 0U;

/**
 * @brief Enables every authored alphabet at the uint64 coordinate width.
 * @remark Unit: boolean. Domain: false|true. Default: false.
 */
inline constexpr bool bioseq_template_flatten_has_all_alphabets_uint64 [[bioseq::build_config(
    "BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64", "boolean", "false|true")]] =
    false;

/**
 * @brief Enables every authored coordinate width.
 * @remark Unit: boolean. Domain: false|true. Default: false.
 */
inline constexpr bool bioseq_template_flatten_has_all_coordinates [[bioseq::build_config(
    "BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_HAS_ALL_COORDINATES", "boolean", "false|true")]] = false;

/**
 * @brief Enables adaptive sequence-length specializations.
 * @remark Unit: boolean. Domain: false|true. Default: false.
 */
inline constexpr bool bioseq_template_flatten_has_adaptive_lengths [[bioseq::build_config(
    "BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_HAS_ADAPTIVE_LENGTHS", "boolean", "false|true")]] = false;

/**
 * @brief Enables extended k-mer coordinate specializations.
 * @remark Unit: boolean. Domain: false|true. Default: false.
 */
inline constexpr bool bioseq_template_flatten_has_extended_kmer_coordinates
    [[bioseq::build_config("BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_HAS_EXTENDED_KMER_COORDINATES",
                           "boolean",
                           "false|true")]] = false;

/**
 * @brief Enables the complete specialization surface.
 * @remark Unit: boolean. Domain: false|true. Default: false.
 */
inline constexpr bool bioseq_template_flatten_has_full_specialization [[bioseq::build_config(
    "BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_HAS_FULL_SPECIALIZATION", "boolean", "false|true")]] =
    false;

/**
 * @brief Gives the generated typed scheduler owner count.
 * @remark Unit: count. Domain: 7|14|56. Default: 7U.
 */
inline constexpr unsigned int bioseq_scheduler_typed_heavy_unit_count [[bioseq::build_config(
    "BIOSEQ_BUILD_CONFIG_SCHEDULER_TYPED_HEAVY_UNIT_COUNT", "count", "7|14|56")]] = 7U;

/**
 * @brief Enables test-only index surfaces in the configured build.
 * @remark Unit: boolean. Domain: false|true. Default: false.
 */
inline constexpr bool bioseq_test_indexes_enabled
    [[bioseq::build_config("BIOSEQ_BUILD_CONFIG_TEST_INDEXES_ENABLED", "boolean", "false|true")]] =
        false;

/**
 * @brief Enables the noninstalled cache test-support profile.
 * @remark Unit: boolean. Domain: false|true. Default: false.
 */
inline constexpr bool bioseq_cache_testing_enabled
    [[bioseq::build_config("BIOSEQ_BUILD_CONFIG_CACHE_TESTING_ENABLED", "boolean", "false|true")]] =
        false;

/**
 * @brief Enables generated external API clients and their PostgreSQL entrypoints.
 * @remark Unit: boolean. Domain: false|true. Default: false.
 */
inline constexpr bool bioseq_external_api_clients_enabled [[bioseq::build_config(
    "BIOSEQ_BUILD_CONFIG_EXTERNAL_API_CLIENTS_ENABLED", "boolean", "false|true")]] = false;

/**
 * @brief Sets the retry interval for a conditional vacuum truncate lock.
 * @remark Unit: milliseconds. Domain: 1..2147483647. Default: 50.
 */
inline constexpr int bioseq_vacuum_truncate_lock_wait_interval_milliseconds
    [[bioseq::build_config("BIOSEQ_BUILD_CONFIG_VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL_MILLISECONDS",
                           "milliseconds",
                           "1..2147483647")]] = 50;

/**
 * @brief Sets the total conditional vacuum truncate lock timeout.
 * @remark Unit: milliseconds. Domain: 1..2147483647. Default: 4500.
 */
inline constexpr int bioseq_vacuum_truncate_lock_timeout_milliseconds
    [[bioseq::build_config("BIOSEQ_BUILD_CONFIG_VACUUM_TRUNCATE_LOCK_TIMEOUT_MILLISECONDS",
                           "milliseconds",
                           "1..2147483647")]] = 4500;

} // namespace bioseq::build_config
