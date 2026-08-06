/**
 * @file mlogging_specializations.cpp
 * @brief Out-of-line definitions for compile-heavy logging specializations.
 */
module;

module bioseq.common.mlogging;

import std;

template<>
void bioseq_log<
    LoggingLevel::DBG,
    LoggingTarget::FILE,
    std::string_view&,
    unsigned long long>(
    bioseq_logging_detail::format_with_location<
        std::string_view&,
        unsigned long long> fmt_loc,
    std::string_view& route,
    unsigned long long&& total_rejections)
{
  if constexpr (!bioseq_logging_detail::compiled_level_enabled(
                    LoggingLevel::DBG)) {
    return;
  }

  if constexpr (LoggingLevel::DBG == LoggingLevel::DBG) {
    if (!bioseq_logging_detail::debug_logging_is_enabled()) {
      return;
    }
  }

  bioseq_logging_detail::file_log_impl<LoggingLevel::DBG>(
      fmt_loc,
      std::forward<std::string_view&>(route),
      std::forward<unsigned long long>(total_rejections));
}

template<>
void bioseq_log<
    LoggingLevel::DBG,
    LoggingTarget::FILE,
    unsigned long long,
    std::string_view&,
    unsigned long long>(
    bioseq_logging_detail::format_with_location<
        unsigned long long,
        std::string_view&,
        unsigned long long> fmt_loc,
    unsigned long long&& sequence_rowno,
    std::string_view& index_name,
    unsigned long long&& payload_bytes)
{
  if constexpr (!bioseq_logging_detail::compiled_level_enabled(
                    LoggingLevel::DBG)) {
    return;
  }

  if constexpr (LoggingLevel::DBG == LoggingLevel::DBG) {
    if (!bioseq_logging_detail::debug_logging_is_enabled()) {
      return;
    }
  }

  bioseq_logging_detail::file_log_impl<LoggingLevel::DBG>(
      fmt_loc,
      std::forward<unsigned long long>(sequence_rowno),
      std::forward<std::string_view&>(index_name),
      std::forward<unsigned long long>(payload_bytes));
}
