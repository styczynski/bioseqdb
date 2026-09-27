/**
 * @file
 * @brief Defines two debug-log specializations for file output.
 */
module;

module bioseq.common.logging;

import std;

template <>
auto bioseq_log<logging_level::debug, logging_target::file, std::string_view&, unsigned long long>(
    bioseq::common::logging_detail::format_with_location<std::string_view&, unsigned long long>
        format_location,
    std::string_view& route,
    unsigned long long&& total_rejections) -> void {
    if (!bioseq::common::logging_detail::is_logging_level_active<
            logging_level::debug>()) {
        return;
    }

    bioseq::common::logging_detail::write_file_log<logging_level::debug>(
        format_location,
        std::forward<std::string_view&>(route),
        std::forward<unsigned long long>(total_rejections));
}

template <>
auto bioseq_log<logging_level::debug,
                logging_target::file,
                unsigned long long,
                std::string_view&,
                unsigned long long>(
    bioseq::common::logging_detail::format_with_location<unsigned long long,
                                                         std::string_view&,
                                                         unsigned long long> format_location,
    unsigned long long&& sequence_row_number,
    std::string_view& index_name,
    unsigned long long&& payload_bytes) -> void {
    if (!bioseq::common::logging_detail::is_logging_level_active<
            logging_level::debug>()) {
        return;
    }

    bioseq::common::logging_detail::write_file_log<logging_level::debug>(
        format_location,
        std::forward<unsigned long long>(sequence_row_number),
        std::forward<std::string_view&>(index_name),
        std::forward<unsigned long long>(payload_bytes));
}
