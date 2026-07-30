module;

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.test.postgres:artifact;

import :row_hash;

export namespace bioseq::postgres_test {

/// @brief Build outputs required to install one extension in a private cluster.
struct extension_artifact {
    std::string name;
    std::filesystem::path control_file;
    std::filesystem::path install_sql;
    std::filesystem::path dso;
};

/// @brief Source/staged paths and SHA-256 evidence for one extension triple.
struct staged_extension_artifact {
    std::string name;
    std::filesystem::path source_control_file;
    std::filesystem::path source_install_sql;
    std::filesystem::path source_dso;
    std::filesystem::path staged_control_file;
    std::filesystem::path staged_install_sql;
    std::filesystem::path staged_dso;
    sha256_digest control_digest;
    sha256_digest install_sql_digest;
    sha256_digest dso_digest;
    sha256_digest staged_control_digest;
};

/// @brief Exact private directories holding staged extension artifacts.
struct extension_stage {
    std::filesystem::path stage_root;
    std::optional<std::filesystem::path> manifest_file;
    std::optional<sha256_digest> manifest_digest;
    std::filesystem::path control_directory;
    std::filesystem::path library_directory;
    std::optional<std::filesystem::path> runtime_library_directory;
    std::vector<staged_extension_artifact> extensions;
};

}  // namespace bioseq::postgres_test

namespace bioseq::postgres_test::artifact_detail {

inline void validate_extension_name(std::string_view name);
[[nodiscard]] inline std::filesystem::path require_regular_file(
    const std::filesystem::path& path,
    std::string_view label);
[[nodiscard]] inline std::string shared_library_suffix(
    const std::filesystem::path& source);
inline void copy_exact(const std::filesystem::path& source,
                       const std::filesystem::path& destination);

enum class manifest_json_kind {
    null,
    boolean,
    number,
    string,
    array,
    object,
};

struct manifest_json_value {
    manifest_json_kind kind{manifest_json_kind::null};
    std::string scalar;
    std::vector<manifest_json_value> array;
    std::map<std::string, manifest_json_value, std::less<>> object;
};

/// @brief Small strict JSON reader for the CMake-owned stage manifest.
///
/// PostgreSQL test support intentionally has no production JSON dependency.
/// This reader accepts the complete JSON grammar, rejects duplicate members,
/// and exposes only the typed fields used by the stage attestation below.
class manifest_json_reader {
public:
    explicit manifest_json_reader(std::string_view input) : input_(input) {}

    [[nodiscard]] manifest_json_value parse() {
        manifest_json_value result = value();
        whitespace();
        if (position_ != input_.size()) {
            fail("trailing content");
        }
        return result;
    }

private:
    [[noreturn]] void fail(std::string_view detail) const {
        throw std::invalid_argument(
            "invalid preinstalled PostgreSQL stage manifest at byte "
            + std::to_string(position_) + ": " + std::string(detail));
    }

    void whitespace() noexcept {
        while (position_ < input_.size()) {
            const char character = input_[position_];
            if (character != ' ' && character != '\t'
                && character != '\r' && character != '\n') {
                break;
            }
            ++position_;
        }
    }

    [[nodiscard]] bool consume(char expected) noexcept {
        whitespace();
        if (position_ >= input_.size()
            || input_[position_] != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    void require(char expected) {
        if (!consume(expected)) {
            fail(std::string("expected '") + expected + "'");
        }
    }

    [[nodiscard]] manifest_json_value value() {
        whitespace();
        if (position_ >= input_.size()) {
            fail("expected value");
        }
        switch (input_[position_]) {
        case 'n':
            literal("null");
            return {.kind = manifest_json_kind::null};
        case 't':
            literal("true");
            return {
                .kind = manifest_json_kind::boolean,
                .scalar = "true"};
        case 'f':
            literal("false");
            return {
                .kind = manifest_json_kind::boolean,
                .scalar = "false"};
        case '"':
            return {
                .kind = manifest_json_kind::string,
                .scalar = string()};
        case '[':
            return array();
        case '{':
            return object();
        default:
            if (input_[position_] == '-'
                || (input_[position_] >= '0'
                    && input_[position_] <= '9')) {
                return number();
            }
            fail("unexpected value token");
        }
    }

    void literal(std::string_view literal) {
        if (input_.substr(position_, literal.size()) != literal) {
            fail("invalid literal");
        }
        position_ += literal.size();
    }

    [[nodiscard]] static unsigned hex_digit(char character) {
        if (character >= '0' && character <= '9') {
            return static_cast<unsigned>(character - '0');
        }
        if (character >= 'a' && character <= 'f') {
            return static_cast<unsigned>(character - 'a' + 10);
        }
        if (character >= 'A' && character <= 'F') {
            return static_cast<unsigned>(character - 'A' + 10);
        }
        return 16U;
    }

    [[nodiscard]] std::uint32_t unicode_escape() {
        if (position_ + 4U > input_.size()) {
            fail("truncated Unicode escape");
        }
        std::uint32_t value{};
        for (unsigned index = 0; index != 4U; ++index) {
            const unsigned digit = hex_digit(input_[position_++]);
            if (digit == 16U) {
                fail("invalid Unicode escape");
            }
            value = (value << 4U) | digit;
        }
        return value;
    }

    static void append_utf8(std::string& output, std::uint32_t codepoint) {
        if (codepoint <= 0x7fU) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ffU) {
            output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
            output.push_back(
                static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else if (codepoint <= 0xffffU) {
            output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
            output.push_back(static_cast<char>(
                0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(
                static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else {
            output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
            output.push_back(static_cast<char>(
                0x80U | ((codepoint >> 12U) & 0x3fU)));
            output.push_back(static_cast<char>(
                0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(
                static_cast<char>(0x80U | (codepoint & 0x3fU)));
        }
    }

    [[nodiscard]] std::string string() {
        require('"');
        std::string result;
        while (position_ < input_.size()) {
            const unsigned char character =
                static_cast<unsigned char>(input_[position_++]);
            if (character == '"') {
                return result;
            }
            if (character < 0x20U) {
                fail("unescaped control character in string");
            }
            if (character != '\\') {
                result.push_back(static_cast<char>(character));
                continue;
            }
            if (position_ >= input_.size()) {
                fail("truncated string escape");
            }
            const char escape = input_[position_++];
            switch (escape) {
            case '"':
            case '\\':
            case '/':
                result.push_back(escape);
                break;
            case 'b':
                result.push_back('\b');
                break;
            case 'f':
                result.push_back('\f');
                break;
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 't':
                result.push_back('\t');
                break;
            case 'u': {
                std::uint32_t codepoint = unicode_escape();
                if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
                    if (position_ + 2U > input_.size()
                        || input_[position_] != '\\'
                        || input_[position_ + 1U] != 'u') {
                        fail("unpaired high Unicode surrogate");
                    }
                    position_ += 2U;
                    const std::uint32_t low = unicode_escape();
                    if (low < 0xdc00U || low > 0xdfffU) {
                        fail("invalid low Unicode surrogate");
                    }
                    codepoint = 0x10000U
                        + ((codepoint - 0xd800U) << 10U)
                        + (low - 0xdc00U);
                } else if (codepoint >= 0xdc00U
                           && codepoint <= 0xdfffU) {
                    fail("unpaired low Unicode surrogate");
                }
                append_utf8(result, codepoint);
                break;
            }
            default:
                fail("invalid string escape");
            }
        }
        fail("unterminated string");
    }

    [[nodiscard]] manifest_json_value number() {
        const std::size_t begin = position_;
        if (input_[position_] == '-') {
            ++position_;
        }
        if (position_ >= input_.size()) {
            fail("truncated number");
        }
        if (input_[position_] == '0') {
            ++position_;
        } else {
            if (input_[position_] < '1' || input_[position_] > '9') {
                fail("invalid number");
            }
            while (position_ < input_.size()
                   && input_[position_] >= '0'
                   && input_[position_] <= '9') {
                ++position_;
            }
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            const std::size_t fractional_begin = position_;
            while (position_ < input_.size()
                   && input_[position_] >= '0'
                   && input_[position_] <= '9') {
                ++position_;
            }
            if (fractional_begin == position_) {
                fail("invalid number fraction");
            }
        }
        if (position_ < input_.size()
            && (input_[position_] == 'e'
                || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size()
                && (input_[position_] == '+'
                    || input_[position_] == '-')) {
                ++position_;
            }
            const std::size_t exponent_begin = position_;
            while (position_ < input_.size()
                   && input_[position_] >= '0'
                   && input_[position_] <= '9') {
                ++position_;
            }
            if (exponent_begin == position_) {
                fail("invalid number exponent");
            }
        }
        return {
            .kind = manifest_json_kind::number,
            .scalar = std::string(input_.substr(begin, position_ - begin))};
    }

    [[nodiscard]] manifest_json_value array() {
        require('[');
        manifest_json_value result{.kind = manifest_json_kind::array};
        if (consume(']')) {
            return result;
        }
        for (;;) {
            result.array.push_back(value());
            if (consume(']')) {
                return result;
            }
            require(',');
        }
    }

    [[nodiscard]] manifest_json_value object() {
        require('{');
        manifest_json_value result{.kind = manifest_json_kind::object};
        if (consume('}')) {
            return result;
        }
        for (;;) {
            whitespace();
            if (position_ >= input_.size()
                || input_[position_] != '"') {
                fail("expected object member name");
            }
            std::string name = string();
            require(':');
            if (!result.object.emplace(name, value()).second) {
                fail("duplicate object member: " + name);
            }
            if (consume('}')) {
                return result;
            }
            require(',');
        }
    }

    std::string_view input_;
    std::size_t position_{};
};

[[nodiscard]] inline const manifest_json_value& object_member(
    const manifest_json_value& object,
    std::string_view name) {
    if (object.kind != manifest_json_kind::object) {
        throw std::invalid_argument(
            "preinstalled PostgreSQL stage manifest member is not an object");
    }
    const auto member = object.object.find(name);
    if (member == object.object.end()) {
        throw std::invalid_argument(
            "preinstalled PostgreSQL stage manifest is missing member: "
            + std::string(name));
    }
    return member->second;
}

[[nodiscard]] inline const std::string& string_member(
    const manifest_json_value& object,
    std::string_view name) {
    const manifest_json_value& member = object_member(object, name);
    if (member.kind != manifest_json_kind::string) {
        throw std::invalid_argument(
            "preinstalled PostgreSQL stage manifest member must be a string: "
            + std::string(name));
    }
    return member.scalar;
}

inline void require_exact_object_members(
    const manifest_json_value& object,
    std::initializer_list<std::string_view> expected,
    std::string_view label) {
    if (object.kind != manifest_json_kind::object) {
        throw std::invalid_argument(
            std::string(label) + " must be a JSON object");
    }
    if (object.object.size() != expected.size()) {
        throw std::invalid_argument(
            std::string(label) + " has missing or unknown members");
    }
    for (const std::string_view name : expected) {
        if (!object.object.contains(name)) {
            throw std::invalid_argument(
                std::string(label) + " is missing required member: "
                + std::string(name));
        }
    }
}

[[nodiscard]] inline const std::vector<manifest_json_value>& array_member(
    const manifest_json_value& object,
    std::string_view name) {
    const manifest_json_value& member = object_member(object, name);
    if (member.kind != manifest_json_kind::array) {
        throw std::invalid_argument(
            "preinstalled PostgreSQL stage manifest member must be an array: "
            + std::string(name));
    }
    return member.array;
}

[[nodiscard]] inline std::optional<std::string> nullable_string_member(
    const manifest_json_value& object,
    std::string_view name) {
    const manifest_json_value& member = object_member(object, name);
    if (member.kind == manifest_json_kind::null) {
        return std::nullopt;
    }
    if (member.kind != manifest_json_kind::string) {
        throw std::invalid_argument(
            "preinstalled PostgreSQL stage manifest member must be null "
            "or a string: " + std::string(name));
    }
    return member.scalar;
}

[[nodiscard]] inline bool path_is_within(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate) {
    auto root_part = root.begin();
    auto candidate_part = candidate.begin();
    while (root_part != root.end()) {
        if (candidate_part == candidate.end()
            || *root_part != *candidate_part) {
            return false;
        }
        ++root_part;
        ++candidate_part;
    }
    return true;
}

[[nodiscard]] inline std::filesystem::path require_real_directory(
    const std::filesystem::path& path,
    std::string_view label) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_directory(status)
        || std::filesystem::is_symlink(status)) {
        throw std::invalid_argument(
            std::string(label) + " is not a real non-symlink directory: "
            + path.string());
    }
    const std::filesystem::path canonical =
        std::filesystem::canonical(path, error);
    if (error) {
        throw std::invalid_argument(
            "cannot resolve " + std::string(label) + ": " + path.string());
    }
    return canonical;
}

[[nodiscard]] inline std::filesystem::path resolve_stage_coordinate(
    const std::filesystem::path& stage_root,
    std::string_view relative_text,
    std::string_view label,
    bool directory) {
    const std::filesystem::path relative{relative_text};
    if (relative.empty() || relative.is_absolute()
        || relative.has_root_name() || relative.has_root_directory()
        || relative.lexically_normal() != relative) {
        throw std::invalid_argument(
            std::string(label) + " is not a normalized relative stage path: "
            + std::string(relative_text));
    }
    std::filesystem::path cursor = stage_root;
    for (const std::filesystem::path& component : relative) {
        if (component == "." || component == ".." || component.empty()) {
            throw std::invalid_argument(
                std::string(label) + " escapes the stage root");
        }
        cursor /= component;
        std::error_code component_error;
        const auto component_status =
            std::filesystem::symlink_status(cursor, component_error);
        if (component_error || !std::filesystem::exists(component_status)
            || std::filesystem::is_symlink(component_status)) {
            throw std::invalid_argument(
                std::string(label)
                + " contains a missing or symlink stage component: "
                + cursor.string());
        }
    }
    std::error_code error;
    const std::filesystem::path canonical =
        std::filesystem::canonical(cursor, error);
    if (error || !path_is_within(stage_root, canonical)) {
        throw std::invalid_argument(
            std::string(label) + " is outside the stage root");
    }
    const auto status = std::filesystem::symlink_status(canonical, error);
    const bool correct_type =
        directory ? std::filesystem::is_directory(status)
                  : std::filesystem::is_regular_file(status);
    if (error || !correct_type || std::filesystem::is_symlink(status)) {
        throw std::invalid_argument(
            std::string(label) + " has the wrong filesystem type: "
            + canonical.string());
    }
    return canonical;
}

inline void require_digest(
    const std::filesystem::path& path,
    std::string_view expected,
    std::string_view label) {
    const bool valid_hex =
        expected.size() == 64U
        && std::ranges::all_of(
            expected,
            [](char character) {
                return (character >= '0' && character <= '9')
                    || (character >= 'a' && character <= 'f');
            });
    if (!valid_hex || sha256_file(path).hex() != expected) {
        throw std::invalid_argument(
            std::string(label) + " SHA-256 mismatch: " + path.string());
    }
}

[[nodiscard]] inline std::string read_regular_file(
    const std::filesystem::path& path,
    std::string_view label) {
    const std::filesystem::path canonical =
        require_regular_file(path, label);
    std::ifstream input(canonical, std::ios::binary);
    if (!input) {
        throw std::invalid_argument(
            "cannot open " + std::string(label) + ": "
            + canonical.string());
    }
    std::string contents{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    if (input.bad()) {
        throw std::invalid_argument(
            "cannot read " + std::string(label) + ": "
            + canonical.string());
    }
    return contents;
}

inline void require_exact_file_copy(
    const std::filesystem::path& source,
    const std::filesystem::path& staged,
    std::string_view label) {
    if (read_regular_file(source, label)
        != read_regular_file(staged, label)) {
        throw std::invalid_argument(
            std::string(label) + " source and staged bytes differ");
    }
}

[[nodiscard]] inline std::filesystem::path prepare_runtime_library_directory(
    const std::filesystem::path& requested) {
    if (requested.empty() || requested.filename() != "runtime-library") {
        throw std::invalid_argument(
            "runtime DSO override directory must be the exact "
            "runtime-library child");
    }
    const std::filesystem::path parent =
        require_real_directory(requested.parent_path(),
                               "private cluster root");
    if (!parent.filename().string().starts_with("bioseq-pg-test-")
        || !std::filesystem::is_regular_file(
            parent / ".bioseq-private-cluster")) {
        throw std::invalid_argument(
            "runtime DSO override directory is not marker-owned");
    }
    std::error_code error;
    if (!std::filesystem::create_directory(requested, error) || error) {
        throw std::invalid_argument(
            "cannot create fresh marker-owned runtime DSO directory: "
            + requested.string());
    }
    return require_real_directory(
        requested, "runtime DSO override directory");
}

inline void validate_manifest_extension_name(
    std::string_view name,
    std::string_view label) {
    if (name.empty()
        || name.front() < 'a' || name.front() > 'z') {
        throw std::invalid_argument(
            std::string(label)
            + " must start with a lower-case ASCII letter");
    }
    for (const char character : name) {
        if ((character < 'a' || character > 'z')
            && (character < '0' || character > '9')
            && character != '_') {
            throw std::invalid_argument(
                std::string(label)
                + " is not a lower-case extension identity");
        }
    }
}

inline void validate_extension_version(std::string_view version) {
    if (version.empty()
        || !((version.front() >= 'A' && version.front() <= 'Z')
             || (version.front() >= 'a' && version.front() <= 'z')
             || (version.front() >= '0' && version.front() <= '9'))) {
        throw std::invalid_argument(
            "stage manifest extension version is invalid");
    }
    for (const char character : version) {
        const bool allowed =
            (character >= 'A' && character <= 'Z')
            || (character >= 'a' && character <= 'z')
            || (character >= '0' && character <= '9')
            || character == '.' || character == '_'
            || character == '+' || character == '-';
        if (!allowed) {
            throw std::invalid_argument(
                "stage manifest extension version is invalid");
        }
    }
}

struct validated_manifest_artifact {
    std::string name;
    std::optional<std::string> module_pathname_override;
    std::string effective_module_name;
    std::filesystem::path source_control;
    std::filesystem::path source_install_sql;
    std::filesystem::path source_dso;
    std::filesystem::path staged_control;
    std::filesystem::path staged_install_sql;
    std::filesystem::path staged_dso;
    std::string control_filename;
    std::string install_sql_filename;
    std::string library_filename;
    std::string source_control_sha256;
    std::string control_sha256;
    std::string install_sql_sha256;
    std::string library_sha256;
};

[[nodiscard]] inline validated_manifest_artifact
validate_manifest_artifact(
    const manifest_json_value& artifact,
    const std::filesystem::path& canonical_root,
    const std::filesystem::path& control_directory,
    const std::filesystem::path& library_directory) {
    require_exact_object_members(
        artifact,
        {"name", "module_pathname_override", "effective_module_name",
         "control", "install_sql", "library"},
        "stage manifest extension artifact");
    validated_manifest_artifact result;
    result.name = string_member(artifact, "name");
    validate_manifest_extension_name(
        result.name, "stage manifest extension name");
    result.module_pathname_override =
        nullable_string_member(artifact, "module_pathname_override");
    if (result.module_pathname_override) {
        validate_manifest_extension_name(
            *result.module_pathname_override,
            "stage manifest module_pathname_override");
    }
    result.effective_module_name =
        string_member(artifact, "effective_module_name");
    if (result.module_pathname_override) {
        if (result.effective_module_name
            != *result.module_pathname_override) {
            throw std::invalid_argument(
                "stage manifest effective module name disagrees with "
                "module_pathname_override: " + result.name);
        }
    }

    const manifest_json_value& control =
        object_member(artifact, "control");
    require_exact_object_members(
        control,
        {"source", "filename", "staged", "source_sha256", "sha256"},
        "stage manifest control artifact");
    const manifest_json_value& install_sql =
        object_member(artifact, "install_sql");
    require_exact_object_members(
        install_sql,
        {"source", "filename", "staged", "version", "sha256"},
        "stage manifest install SQL artifact");
    const manifest_json_value& library =
        object_member(artifact, "library");
    require_exact_object_members(
        library,
        {"source", "filename", "staged", "sha256"},
        "stage manifest library artifact");

    result.control_filename = string_member(control, "filename");
    result.install_sql_filename = string_member(install_sql, "filename");
    result.library_filename = string_member(library, "filename");
    const std::string& extension_version =
        string_member(install_sql, "version");
    validate_extension_version(extension_version);
    if (result.control_filename != result.name + ".control"
        || result.install_sql_filename
            != result.name + "--" + extension_version + ".sql") {
        throw std::invalid_argument(
            "stage manifest extension filenames are not canonical: "
            + result.name);
    }

    result.source_control = require_regular_file(
        string_member(control, "source"),
        "manifest source control file");
    result.source_install_sql = require_regular_file(
        string_member(install_sql, "source"),
        "manifest source install SQL");
    result.source_dso = require_regular_file(
        string_member(library, "source"),
        "manifest source extension DSO");
    if (result.source_control.filename() != result.control_filename
        || result.source_install_sql.filename()
            != result.install_sql_filename
        || result.source_dso.filename() != result.library_filename) {
        throw std::invalid_argument(
            "stage manifest source basename disagrees with its filename: "
            + result.name);
    }

    result.staged_control = resolve_stage_coordinate(
        canonical_root,
        string_member(control, "staged"),
        "staged extension control", false);
    result.staged_install_sql = resolve_stage_coordinate(
        canonical_root,
        string_member(install_sql, "staged"),
        "staged extension install SQL", false);
    result.staged_dso = resolve_stage_coordinate(
        canonical_root,
        string_member(library, "staged"),
        "staged extension DSO", false);
    if (result.staged_control.parent_path() != control_directory
        || result.staged_install_sql.parent_path() != control_directory
        || result.staged_dso.parent_path() != library_directory
        || result.staged_control.filename() != result.control_filename
        || result.staged_install_sql.filename()
            != result.install_sql_filename
        || result.staged_dso.filename() != result.library_filename) {
        throw std::invalid_argument(
            "stage manifest artifact path disagrees with its PostgreSQL "
            "installation coordinate: " + result.name);
    }

    result.source_control_sha256 =
        string_member(control, "source_sha256");
    result.control_sha256 = string_member(control, "sha256");
    result.install_sql_sha256 = string_member(install_sql, "sha256");
    result.library_sha256 = string_member(library, "sha256");
    require_digest(
        result.source_control, result.source_control_sha256,
        "manifest source control");
    require_digest(
        result.staged_control, result.control_sha256,
        "staged extension control");
    require_digest(
        result.source_install_sql, result.install_sql_sha256,
        "manifest source install SQL");
    require_digest(
        result.staged_install_sql, result.install_sql_sha256,
        "staged extension install SQL");
    require_digest(
        result.source_dso, result.library_sha256,
        "manifest source extension DSO");
    require_digest(
        result.staged_dso, result.library_sha256,
        "staged extension DSO");
    if (!result.module_pathname_override) {
        if (result.source_control_sha256 != result.control_sha256) {
            throw std::invalid_argument(
                "null module_pathname_override requires identical "
                "source/staged control hashes: " + result.name);
        }
        require_exact_file_copy(
            result.source_control, result.staged_control,
            "extension control");
        const std::string expected_module_name =
            result.source_dso.filename().string().substr(
                0, result.source_dso.filename().string().find(
                       shared_library_suffix(result.source_dso)));
        if (result.effective_module_name != expected_module_name) {
            throw std::invalid_argument(
                "stage manifest effective module name disagrees with "
                "the source DSO: " + result.name);
        }
    }
    return result;
}

[[nodiscard]] inline std::filesystem::path normalized_absolute_metadata_path(
    std::string_view text,
    std::string_view label) {
    const std::filesystem::path path{text};
    if (path.empty() || !path.is_absolute()
        || path.lexically_normal() != path) {
        throw std::invalid_argument(
            std::string(label)
            + " must be an absolute normalized PostgreSQL path");
    }
    return path;
}

inline void validate_postgresql_tools(
    const manifest_json_value& expected_paths,
    const manifest_json_value& tool_signatures,
    const std::filesystem::path& canonical_root,
    const std::filesystem::path& manifest_bin_dir,
    const std::filesystem::path& staged_bin_relative) {
    constexpr std::array<std::string_view, 6> tools{
        "postgres", "initdb", "pg_ctl", "createdb", "dropdb", "psql"};
    require_exact_object_members(
        expected_paths,
        {"postgres", "initdb", "pg_ctl", "createdb", "dropdb", "psql"},
        "stage manifest expected_tool_paths");
    if (tool_signatures.kind != manifest_json_kind::array
        || tool_signatures.array.size() != tools.size()) {
        throw std::invalid_argument(
            "stage manifest tool_signatures must contain exactly six tools");
    }
    std::set<std::string> observed_tools;
    for (std::size_t index = 0; index != tools.size(); ++index) {
        const std::string tool{tools[index]};
        const std::filesystem::path expected_relative =
            (staged_bin_relative / tool).lexically_normal();
        const std::string& expected_path_text =
            string_member(expected_paths, tool);
        if (std::filesystem::path(expected_path_text)
                != expected_relative) {
            throw std::invalid_argument(
                "stage manifest expected tool path is not canonical: "
                + tool);
        }

        const manifest_json_value& signature =
            tool_signatures.array[index];
        require_exact_object_members(
            signature, {"tool", "staged_path", "version", "sha256"},
            "stage manifest tool signature");
        const std::string& signature_tool =
            string_member(signature, "tool");
        if (signature_tool != tool
            || !observed_tools.insert(signature_tool).second) {
            throw std::invalid_argument(
                "stage manifest tool signatures have wrong order or "
                "duplicate tools");
        }
        if (string_member(signature, "staged_path")
            != expected_path_text) {
            throw std::invalid_argument(
                "stage manifest tool signature path disagrees with "
                "expected_tool_paths: " + tool);
        }
        static_cast<void>(string_member(signature, "version"));
        const std::filesystem::path staged_tool =
            resolve_stage_coordinate(
                canonical_root, expected_path_text,
                "staged PostgreSQL tool", false);
        if (staged_tool.parent_path() != manifest_bin_dir
            || staged_tool.filename() != tool) {
            throw std::invalid_argument(
                "staged PostgreSQL tool escapes the manifest bindir: "
                + tool);
        }
        require_digest(
            staged_tool, string_member(signature, "sha256"),
            "staged PostgreSQL tool");
    }
}

[[nodiscard]] inline std::string extension_aggregate_record(
    const std::map<std::string, validated_manifest_artifact, std::less<>>&
        artifacts) {
    std::string record;
    for (const auto& [name, artifact] : artifacts) {
        record += "artifact:" + name + '\n';
        record += "module_pathname_override:"
            + artifact.module_pathname_override.value_or("") + '\n';
        record += "control:" + artifact.control_filename + ':'
            + artifact.control_sha256 + '\n';
        record += "install_sql:" + artifact.install_sql_filename + ':'
            + artifact.install_sql_sha256 + '\n';
        record += "library:" + artifact.library_filename + ':'
            + artifact.library_sha256 + '\n';
    }
    return record;
}

}  // namespace bioseq::postgres_test::artifact_detail

export namespace bioseq::postgres_test {

/// @brief Attests and selects artifacts from one immutable preinstalled stage.
///
/// The caller's artifact vector is a name-unique subset whose caller order is
/// preserved; it need not follow manifest order. It remains mandatory because
/// its source paths are independently hashed.
/// Only a name present in @p allowed_runtime_dso_overrides may substitute a
/// different DSO; its control and SQL always remain the suite-stage files.
[[nodiscard]] inline extension_stage attest_preinstalled_extension_stage(
    const std::filesystem::path& stage_root,
    const std::filesystem::path& stage_manifest,
    const std::filesystem::path& postgres_bin_dir,
    const std::filesystem::path& runtime_library_root,
    std::span<const extension_artifact> artifacts,
    const std::set<std::string>& allowed_runtime_dso_overrides) {
    if (artifacts.empty()) {
        throw std::invalid_argument(
            "preinstalled PostgreSQL stage requires at least one extension");
    }
    const std::filesystem::path canonical_root =
        artifact_detail::require_real_directory(
            stage_root, "preinstalled PostgreSQL stage root");
    const std::filesystem::path canonical_manifest =
        artifact_detail::require_regular_file(
            stage_manifest, "preinstalled PostgreSQL stage manifest");
    const std::string manifest_text =
        artifact_detail::read_regular_file(
            canonical_manifest, "preinstalled PostgreSQL stage manifest");
    const artifact_detail::manifest_json_value manifest =
        artifact_detail::manifest_json_reader(manifest_text).parse();
    sha256_hasher manifest_hasher;
    manifest_hasher.update(manifest_text);
    const sha256_digest parsed_manifest_digest =
        manifest_hasher.finalize();
    artifact_detail::require_exact_object_members(
        manifest,
        {"schema", "postgresql_version", "postgresql_configure",
         "source", "staged", "extension_artifacts",
         "expected_tool_paths", "tool_signatures"},
        "stage manifest root");
    if (artifact_detail::string_member(manifest, "schema")
        != "stage_manifest/v2") {
        throw std::invalid_argument(
            "preinstalled PostgreSQL stage manifest must use "
            "stage_manifest/v2");
    }
    const std::string& postgresql_version =
        artifact_detail::string_member(manifest, "postgresql_version");
    const std::string& postgresql_configure =
        artifact_detail::string_member(manifest, "postgresql_configure");
    constexpr std::string_view version_prefix{"PostgreSQL 17"};
    if (!postgresql_version.starts_with(version_prefix)
        || (postgresql_version.size() > version_prefix.size()
            && postgresql_version[version_prefix.size()] != '.'
            && postgresql_version[version_prefix.size()] != ' '
            && postgresql_version[version_prefix.size()] != '\t')) {
        throw std::invalid_argument(
            "preinstalled PostgreSQL stage must identify PostgreSQL 17");
    }
    static_cast<void>(postgresql_configure);

    const auto& source =
        artifact_detail::object_member(manifest, "source");
    artifact_detail::require_exact_object_members(
        source, {"bindir", "sharedir", "pkglibdir"},
        "stage manifest PostgreSQL source coordinates");
    const auto& staged =
        artifact_detail::object_member(manifest, "staged");
    artifact_detail::require_exact_object_members(
        staged, {"bindir", "sharedir", "pkglibdir"},
        "stage manifest PostgreSQL staged coordinates");
    const std::filesystem::path source_bindir =
        artifact_detail::normalized_absolute_metadata_path(
            artifact_detail::string_member(source, "bindir"),
            "source PostgreSQL bindir");
    const std::filesystem::path source_sharedir =
        artifact_detail::normalized_absolute_metadata_path(
            artifact_detail::string_member(source, "sharedir"),
            "source PostgreSQL sharedir");
    const std::filesystem::path source_pkglibdir =
        artifact_detail::normalized_absolute_metadata_path(
            artifact_detail::string_member(source, "pkglibdir"),
            "source PostgreSQL pkglibdir");
    const std::filesystem::path staged_bin_relative{
        artifact_detail::string_member(staged, "bindir")};
    const std::filesystem::path staged_share_relative{
        artifact_detail::string_member(staged, "sharedir")};
    const std::filesystem::path staged_pkglib_relative{
        artifact_detail::string_member(staged, "pkglibdir")};
    if (staged_bin_relative != source_bindir.relative_path()
        || staged_share_relative != source_sharedir.relative_path()
        || staged_pkglib_relative != source_pkglibdir.relative_path()) {
        throw std::invalid_argument(
            "staged PostgreSQL coordinates do not preserve the source "
            "installation layout");
    }
    const std::filesystem::path manifest_bin_dir =
        artifact_detail::resolve_stage_coordinate(
            canonical_root,
            staged_bin_relative.string(),
            "staged PostgreSQL bindir", true);
    const std::filesystem::path canonical_bin_dir =
        artifact_detail::require_real_directory(
            postgres_bin_dir, "configured PostgreSQL bindir");
    if (canonical_bin_dir != manifest_bin_dir) {
        throw std::invalid_argument(
            "configured PostgreSQL bindir does not belong to the "
            "preinstalled stage manifest");
    }
    const std::filesystem::path shared_directory =
        artifact_detail::resolve_stage_coordinate(
            canonical_root,
            staged_share_relative.string(),
            "staged PostgreSQL sharedir", true);
    const std::filesystem::path library_directory =
        artifact_detail::resolve_stage_coordinate(
            canonical_root,
            staged_pkglib_relative.string(),
            "staged PostgreSQL pkglibdir", true);
    const std::filesystem::path control_directory =
        artifact_detail::require_real_directory(
            shared_directory / "extension",
            "staged PostgreSQL extension directory");
    if (!artifact_detail::path_is_within(
            canonical_root, control_directory)
        || !artifact_detail::path_is_within(
            canonical_root, library_directory)) {
        throw std::invalid_argument(
            "preinstalled PostgreSQL stage directories escape their root");
    }

    const auto& extension_artifacts =
        artifact_detail::object_member(manifest, "extension_artifacts");
    artifact_detail::require_exact_object_members(
        extension_artifacts, {"aggregate_sha256", "artifacts"},
        "stage manifest extension_artifacts");
    const auto& manifest_artifacts =
        artifact_detail::array_member(extension_artifacts, "artifacts");
    if (manifest_artifacts.empty()) {
        throw std::invalid_argument(
            "stage manifest extension artifact vector must not be empty");
    }
    std::map<
        std::string,
        artifact_detail::validated_manifest_artifact,
        std::less<>>
        manifest_artifacts_by_name;
    std::set<std::string> manifest_library_basenames;
    for (const auto& manifest_artifact : manifest_artifacts) {
        auto validated = artifact_detail::validate_manifest_artifact(
            manifest_artifact, canonical_root,
            control_directory, library_directory);
        const std::string name = validated.name;
        if (manifest_artifacts_by_name.contains(name)) {
            throw std::invalid_argument(
                "duplicate extension in preinstalled stage manifest: "
                + name);
        }
        if (!manifest_library_basenames.insert(
                validated.library_filename).second) {
            throw std::invalid_argument(
                "duplicate library basename in preinstalled stage "
                "manifest: " + validated.library_filename);
        }
        manifest_artifacts_by_name.emplace(
            name, std::move(validated));
    }
    const std::string aggregate_record =
        artifact_detail::extension_aggregate_record(
            manifest_artifacts_by_name);
    const std::string& expected_aggregate =
        artifact_detail::string_member(
            extension_artifacts, "aggregate_sha256");
    if (sha256(aggregate_record).hex() != expected_aggregate) {
        throw std::invalid_argument(
            "stage manifest extension aggregate SHA-256 mismatch");
    }
    artifact_detail::validate_postgresql_tools(
        artifact_detail::object_member(
            manifest, "expected_tool_paths"),
        artifact_detail::object_member(
            manifest, "tool_signatures"),
        canonical_root, manifest_bin_dir, staged_bin_relative);
    std::set<std::string> selected_names;
    std::set<std::string> consumed_overrides;
    extension_stage result{
        .stage_root = canonical_root,
        .manifest_file = canonical_manifest,
        .manifest_digest = parsed_manifest_digest,
        .control_directory = control_directory,
        .library_directory = library_directory,
        .runtime_library_directory = std::nullopt,
        .extensions = {}};
    result.extensions.reserve(artifacts.size());

    for (const extension_artifact& artifact : artifacts) {
        artifact_detail::validate_extension_name(artifact.name);
        if (!selected_names.insert(artifact.name).second) {
            throw std::invalid_argument(
                "duplicate selected extension artifact name: "
                + artifact.name);
        }
        const auto manifest_entry =
            manifest_artifacts_by_name.find(artifact.name);
        if (manifest_entry == manifest_artifacts_by_name.end()) {
            throw std::invalid_argument(
                "selected extension is absent from preinstalled stage "
                "manifest: " + artifact.name);
        }
        const artifact_detail::validated_manifest_artifact&
            manifest_artifact = manifest_entry->second;
        if (manifest_artifact.module_pathname_override) {
            throw std::invalid_argument(
                "preinstalled stage control must retain its source "
                "module_pathname: " + artifact.name);
        }

        const std::filesystem::path source_control =
            artifact_detail::require_regular_file(
                artifact.control_file, "runner source control file");
        const std::filesystem::path source_sql =
            artifact_detail::require_regular_file(
                artifact.install_sql, "runner source install SQL");
        const std::filesystem::path source_dso =
            artifact_detail::require_regular_file(
                artifact.dso, "runner source extension DSO");
        if (manifest_artifact.source_control != source_control
            || manifest_artifact.source_install_sql != source_sql) {
            throw std::invalid_argument(
                "runner control/SQL source paths disagree with stage "
                "manifest for " + artifact.name);
        }
        artifact_detail::require_digest(
            source_control,
            manifest_artifact.source_control_sha256,
            "runner source control");
        if (source_control.filename()
                != manifest_artifact.staged_control.filename()
            || source_sql.filename()
                != manifest_artifact.staged_install_sql.filename()) {
            throw std::invalid_argument(
                "runner control/SQL basenames disagree with stage for "
                + artifact.name);
        }
        artifact_detail::require_digest(
            source_sql,
            manifest_artifact.install_sql_sha256,
            "runner source install SQL");
        const sha256_digest source_dso_digest = sha256_file(source_dso);
        const bool dso_is_override =
            source_dso != manifest_artifact.source_dso
            || source_dso_digest.hex()
                != manifest_artifact.library_sha256;
        std::filesystem::path consumed_dso =
            manifest_artifact.staged_dso;
        if (dso_is_override) {
            if (!allowed_runtime_dso_overrides.contains(artifact.name)) {
                throw std::invalid_argument(
                    "runner source DSO differs from preinstalled stage "
                    "without explicit authorization: " + artifact.name);
            }
            if (!consumed_overrides.insert(artifact.name).second) {
                throw std::invalid_argument(
                    "runtime DSO override was consumed more than once: "
                    + artifact.name);
            }
            if (!result.runtime_library_directory) {
                result.runtime_library_directory =
                    artifact_detail::prepare_runtime_library_directory(
                        runtime_library_root);
            }
            consumed_dso =
                *result.runtime_library_directory
                / manifest_artifact.library_filename;
            artifact_detail::copy_exact(source_dso, consumed_dso);
            artifact_detail::require_digest(
                consumed_dso, source_dso_digest.hex(),
                "runtime extension DSO override");
        }

        result.extensions.push_back(staged_extension_artifact{
            .name = artifact.name,
            .source_control_file = source_control,
            .source_install_sql = source_sql,
            .source_dso = source_dso,
            .staged_control_file =
                manifest_artifact.staged_control,
            .staged_install_sql =
                manifest_artifact.staged_install_sql,
            .staged_dso = consumed_dso,
            .control_digest = sha256_file(source_control),
            .install_sql_digest = sha256_file(source_sql),
            .dso_digest = source_dso_digest,
            .staged_control_digest =
                sha256_file(manifest_artifact.staged_control)});
    }

    if (consumed_overrides != allowed_runtime_dso_overrides) {
        throw std::invalid_argument(
            "runtime DSO override authorization was not consumed exactly");
    }
    return result;
}

}  // namespace bioseq::postgres_test

namespace bioseq::postgres_test::artifact_detail {

/// @brief Rejects names that cannot safely become SQL/configuration identities.
inline void validate_extension_name(std::string_view name) {
    if (name.empty() || name.size() > 63
        || !(std::islower(static_cast<unsigned char>(name.front()))
             || name.front() == '_')) {
        throw std::invalid_argument("invalid extension artifact name");
    }
    for (const char character : name) {
        if (!std::islower(static_cast<unsigned char>(character))
            && !std::isdigit(static_cast<unsigned char>(character))
            && character != '_') {
            throw std::invalid_argument("invalid extension artifact name: "
                                        + std::string(name));
        }
    }
}

/// @brief Resolves a non-symlink regular artifact file.
[[nodiscard]] inline std::filesystem::path require_regular_file(
    const std::filesystem::path& path,
    std::string_view label) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_regular_file(status)
        || std::filesystem::is_symlink(status)) {
        throw std::invalid_argument(std::string(label)
                                    + " is not a regular non-symlink file: "
                                    + path.string());
    }
    const std::filesystem::path canonical =
        std::filesystem::canonical(path, error);
    if (error) {
        throw std::invalid_argument("cannot resolve "
                                    + std::string(label) + ": "
                                    + path.string());
    }
    return canonical;
}

/// @brief Returns the platform suffix that PostgreSQL's loader will append.
[[nodiscard]] inline std::string shared_library_suffix(
    const std::filesystem::path& source) {
    const std::string filename = source.filename().string();
    constexpr std::array<std::string_view, 4> suffixes{
        ".dylib", ".bundle", ".dll", ".so"};
    for (const std::string_view suffix : suffixes) {
        const std::size_t position = filename.find(suffix);
        if (position != std::string::npos) {
            return std::string(filename.substr(position));
        }
    }
    throw std::invalid_argument("unrecognized PostgreSQL DSO suffix: "
                                + source.string());
}

/// @brief Overwrites only one explicit destination with one source artifact.
inline void copy_exact(const std::filesystem::path& source,
                       const std::filesystem::path& destination) {
    std::error_code error;
    std::filesystem::remove(destination, error);
    error.clear();
    if (!std::filesystem::copy_file(
            source, destination, std::filesystem::copy_options::none, error)
        || error) {
        throw std::runtime_error("cannot stage " + source.string() + " as "
                                 + destination.string() + ": "
                                 + error.message());
    }
}

/// @brief Escapes a PostgreSQL configuration/control quoted string.
[[nodiscard]] inline std::string control_literal(std::string_view value) {
    if (value.find('\n') != std::string_view::npos
        || value.find('\r') != std::string_view::npos) {
        throw std::invalid_argument("control value contains a newline");
    }
    std::string escaped{"'"};
    for (const char character : value) {
        if (character == '\'') {
            escaped.push_back('\'');
        }
        escaped.push_back(character);
    }
    escaped.push_back('\'');
    return escaped;
}

/// @brief Copies a control file and pins MODULE_PATHNAME to the staged DSO stem.
///
/// The staged control/install files form the fixture's immutable artifact
/// bundle. Rewriting only the staged control copy directs MODULE_PATHNAME to
/// the per-case DSO without mutating the source artifact or relying on a
/// server-version-specific control-file search setting.
inline void stage_control_file(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const std::filesystem::path& staged_dso_stem) {
    std::ifstream input(source);
    if (!input) {
        throw std::runtime_error("cannot read extension control file: "
                                 + source.string());
    }
    std::vector<std::string> lines;
    std::string line;
    bool replaced = false;
    while (std::getline(input, line)) {
        const std::size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos
            && std::string_view(line).substr(first).starts_with(
                "module_pathname")) {
            line = "module_pathname = "
                + control_literal(staged_dso_stem.string());
            replaced = true;
        }
        lines.push_back(std::move(line));
    }
    if (!input.eof()) {
        throw std::runtime_error("failed reading extension control file: "
                                 + source.string());
    }
    if (!replaced) {
        lines.push_back("module_pathname = "
                        + control_literal(staged_dso_stem.string()));
    }
    std::error_code error;
    std::filesystem::remove(destination, error);
    std::ofstream output(destination, std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot stage extension control file: "
                                 + destination.string());
    }
    for (const std::string& output_line : lines) {
        output << output_line << '\n';
    }
    output.close();
    if (!output) {
        throw std::runtime_error("failed staging extension control file: "
                                 + destination.string());
    }
}

}  // namespace bioseq::postgres_test::artifact_detail

export namespace bioseq::postgres_test {

/// @brief Validates and stages extension triples without glob deletion.
[[nodiscard]] inline extension_stage stage_extension_artifacts(
    const std::filesystem::path& root,
    std::span<const extension_artifact> artifacts) {
    if (artifacts.empty()) {
        throw std::invalid_argument(
            "private PostgreSQL staging requires at least one extension");
    }
    extension_stage stage{
        .stage_root = root,
        .manifest_file = std::nullopt,
        .manifest_digest = std::nullopt,
        .control_directory = root / "share" / "postgresql" / "extension",
        .library_directory = root / "lib" / "postgresql",
        .runtime_library_directory = std::nullopt,
        .extensions = {}};
    std::filesystem::create_directories(stage.control_directory);
    std::filesystem::create_directories(stage.library_directory);

    std::set<std::string> extension_names;
    stage.extensions.reserve(artifacts.size());
    for (const extension_artifact& artifact : artifacts) {
        artifact_detail::validate_extension_name(artifact.name);
        if (!extension_names.insert(artifact.name).second) {
            throw std::invalid_argument("duplicate extension artifact name: "
                                        + artifact.name);
        }
        const std::filesystem::path source_control =
            artifact_detail::require_regular_file(
                artifact.control_file, "control file");
        const std::filesystem::path source_sql =
            artifact_detail::require_regular_file(
                artifact.install_sql, "install SQL");
        const std::filesystem::path source_dso =
            artifact_detail::require_regular_file(artifact.dso, "extension DSO");

        const std::string sql_name = source_sql.filename().string();
        if (!sql_name.starts_with(artifact.name + "--")
            || !sql_name.ends_with(".sql")) {
            throw std::invalid_argument(
                "install SQL must use canonical name--version.sql basename: "
                + source_sql.string());
        }
        const std::string dso_suffix =
            artifact_detail::shared_library_suffix(source_dso);
        const std::filesystem::path staged_control =
            stage.control_directory / (artifact.name + ".control");
        const std::filesystem::path staged_sql =
            stage.control_directory / sql_name;
        const std::filesystem::path staged_dso =
            stage.library_directory / (artifact.name + dso_suffix);

        artifact_detail::copy_exact(source_sql, staged_sql);
        artifact_detail::copy_exact(source_dso, staged_dso);
        artifact_detail::stage_control_file(
            source_control, staged_control,
            stage.library_directory / artifact.name);

        stage.extensions.push_back(staged_extension_artifact{
            .name = artifact.name,
            .source_control_file = source_control,
            .source_install_sql = source_sql,
            .source_dso = source_dso,
            .staged_control_file = staged_control,
            .staged_install_sql = staged_sql,
            .staged_dso = staged_dso,
            .control_digest = sha256_file(source_control),
            .install_sql_digest = sha256_file(source_sql),
            .dso_digest = sha256_file(source_dso),
            .staged_control_digest = sha256_file(staged_control)});
    }
    return stage;
}

}  // namespace bioseq::postgres_test
