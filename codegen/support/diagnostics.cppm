module;

#include <cstddef>
#include <cstdint>
#include <string>
#include <optional>
#include <filesystem>
#include <vector>

export module bioseq.codegen.support:diagnostics;

export namespace bioseq::codegen {

/// @brief Severity assigned to a generator diagnostic.
enum class diagnostic_severity : std::uint8_t { note, warning, error };

/// @brief Generator operation that produced a diagnostic.
enum class diagnostic_operation : std::uint8_t {
    unspecified,
    doxygen_conversion,
    sql_catalog_read,
    sql_execution,
    database_isolation,
};

/// @brief Source range associated with generated-code diagnostics.
struct source_span {
    std::filesystem::path path;
    std::size_t first_line{};
    std::size_t first_column{};
    std::size_t last_line{};
    std::size_t last_column{};
};

/// @brief Typed diagnostic emitted by a code-generation operation.
struct diagnostic {
    diagnostic_severity severity{};
    diagnostic_operation operation{};
    std::string code;
    std::string message;
    std::optional<source_span> source;
    std::optional<std::string> subject_identifier;
    std::vector<std::string> notes;
};

}  // namespace bioseq::codegen
