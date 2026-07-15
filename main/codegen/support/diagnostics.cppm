module;

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <filesystem>

export module bioseq.codegen.support:diagnostics;

export namespace bioseq::codegen {

enum class diagnostic_severity : std::uint8_t { note, warning, error };

struct source_span {
    std::filesystem::path path;
    std::size_t first_line{}, first_column{}, last_line{}, last_column{};
};

struct diagnostic {
    diagnostic_severity severity{};
    std::string phase, code, message;
    std::optional<source_span> source;
    std::optional<std::string> subject_id;
    std::vector<std::string> notes;
};

}
