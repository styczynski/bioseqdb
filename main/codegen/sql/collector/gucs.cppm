module;

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.sql.collector:gucs;

import :model;
import :options;
import bioseq.codegen.clang_structural;
import bioseq.codegen.support;

export namespace bioseq::codegen::sql {

enum class guc_profile : std::uint8_t {
    production,
    test_indexes,
};

enum class guc_kind : std::uint8_t {
    boolean,
    integer,
    real,
    string,
    enumeration,
};

struct collected_guc_enum_value {
    std::string name;
    std::string cpp_name;
    std::string wire_expression;
    std::string active_condition;
    std::optional<std::string> alias_of;
    bool hidden{};
    bool compiled{};
};

struct collected_guc_authority_enum_value {
    std::string name;
    std::string wire_expression;
    bool hidden{};
    std::string feature_predicate;
    std::optional<std::string> alias_of;
};

struct collected_guc_authority_omission {
    std::string name;
    std::string wire_expression;
};

struct collected_guc_authority {
    std::string source_owner;
    std::string group;
    std::string storage_kind;
    std::string legacy_storage;
    std::string default_expression;
    std::optional<std::string> minimum_expression;
    std::optional<std::string> maximum_expression;
    std::string context;
    std::vector<std::string> flags;
    std::optional<std::string> unit;
    std::array<std::optional<std::string>, 3> hooks;
    std::string feature_predicate;
    std::vector<std::string> profiles;
    std::optional<std::string> default_semantic;
    std::vector<collected_guc_authority_enum_value> enum_values;
    std::vector<collected_guc_authority_omission> omissions;
};

struct collected_guc_live_authority {
    std::string vartype;
    std::string context;
    std::optional<std::string> unit;
    std::string boot_value;
    std::string reset_value;
    std::string current_value;
    std::optional<std::string> minimum;
    std::optional<std::string> maximum;
    std::vector<std::string> visible_enum_values;
    std::string short_description;
    std::optional<std::string> extra_description;
    std::int64_t raw_flags{};
    bool visible_in_show_all{true};
    bool included_in_sample{true};
    std::array<bool, 3> hook_presence{};
};

struct collected_guc {
    std::size_t registration_ordinal{};
    std::string name;
    std::string owner_key;
    std::string member;
    guc_kind kind{guc_kind::boolean};
    std::string cpp_qualified_name;
    std::string source_module;
    std::filesystem::path source_path;
    std::string source_sha256;
    std::string active_condition;
    std::string default_expression;
    std::optional<std::string> minimum_expression;
    std::optional<std::string> maximum_expression;
    std::string context;
    std::vector<std::string> flags;
    std::optional<std::string> unit;
    std::string short_description;
    std::string long_description;
    std::string enum_cpp_name;
    std::vector<collected_guc_enum_value> enum_values;
    bool reset_plan_cache{};
    std::optional<std::string> authored_assign_hook;
    collected_guc_authority authority;
    collected_guc_live_authority generated_live_authority;
};

struct guc_inventory {
    guc_profile profile{guc_profile::production};
    std::filesystem::path ledger_path;
    std::string ledger_sha256;
    std::vector<collected_guc> gucs;
};

[[nodiscard]] inline constexpr std::string_view guc_profile_name(
    guc_profile profile) noexcept {
    switch (profile) {
        case guc_profile::production: return "production";
        case guc_profile::test_indexes: return "test_indexes";
    }
    return "unknown";
}

[[nodiscard]] inline constexpr std::string_view guc_kind_name(
    guc_kind kind) noexcept {
    switch (kind) {
        case guc_kind::boolean: return "bool";
        case guc_kind::integer: return "integer";
        case guc_kind::real: return "real";
        case guc_kind::string: return "string";
        case guc_kind::enumeration: return "enum";
    }
    return "unknown";
}

namespace guc_detail {

struct json_value {
    enum class kind {
        null,
        boolean,
        number,
        string,
        array,
        object,
    } type{kind::null};
    bool boolean{};
    std::int64_t number{};
    std::string string;
    std::vector<json_value> array;
    std::map<std::string, json_value, std::less<>> object;
};

class json_parser final {
public:
    explicit json_parser(std::string_view input)
        : input_{input} {}

    [[nodiscard]] json_value parse() {
        json_value result = parse_value();
        skip_space();
        if (cursor_ != input_.size()) {
            fail("trailing content");
        }
        return result;
    }

private:
    [[noreturn]] void fail(std::string_view reason) const {
        throw std::runtime_error(
            "invalid GUC parity ledger JSON at byte "
            + std::to_string(cursor_) + ": " + std::string{reason});
    }

    void skip_space() noexcept {
        while (cursor_ < input_.size()) {
            const char value = input_[cursor_];
            if (value != ' ' && value != '\t' && value != '\n'
                && value != '\r') {
                break;
            }
            ++cursor_;
        }
    }

    [[nodiscard]] bool consume(char expected) noexcept {
        skip_space();
        if (cursor_ >= input_.size() || input_[cursor_] != expected) {
            return false;
        }
        ++cursor_;
        return true;
    }

    void require(char expected) {
        if (!consume(expected)) {
            fail(std::string{"expected '"} + expected + "'");
        }
    }

    [[nodiscard]] json_value parse_value() {
        skip_space();
        if (cursor_ >= input_.size()) fail("expected a value");
        const char value = input_[cursor_];
        if (value == '"') {
            json_value result;
            result.type = json_value::kind::string;
            result.string = parse_string();
            return result;
        }
        if (value == '[') return parse_array();
        if (value == '{') return parse_object();
        if (value == '-' || (value >= '0' && value <= '9')) {
            return parse_number();
        }
        if (input_.substr(cursor_).starts_with("true")) {
            cursor_ += 4;
            json_value result;
            result.type = json_value::kind::boolean;
            result.boolean = true;
            return result;
        }
        if (input_.substr(cursor_).starts_with("false")) {
            cursor_ += 5;
            json_value result;
            result.type = json_value::kind::boolean;
            return result;
        }
        if (input_.substr(cursor_).starts_with("null")) {
            cursor_ += 4;
            return {};
        }
        fail("unrecognized value");
    }

    [[nodiscard]] std::string parse_string() {
        require('"');
        std::string result;
        while (cursor_ < input_.size()) {
            const char value = input_[cursor_++];
            if (value == '"') return result;
            if (static_cast<unsigned char>(value) < 0x20U) {
                fail("control character in string");
            }
            if (value != '\\') {
                result.push_back(value);
                continue;
            }
            if (cursor_ >= input_.size()) fail("unfinished escape");
            const char escaped = input_[cursor_++];
            switch (escaped) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                default:
                    fail("unsupported escape in ASCII ledger");
            }
        }
        fail("unterminated string");
    }

    [[nodiscard]] json_value parse_number() {
        skip_space();
        const std::size_t begin = cursor_;
        if (input_[cursor_] == '-') ++cursor_;
        if (cursor_ >= input_.size()
            || input_[cursor_] < '0' || input_[cursor_] > '9') {
            fail("invalid integer");
        }
        while (cursor_ < input_.size()
               && input_[cursor_] >= '0' && input_[cursor_] <= '9') {
            ++cursor_;
        }
        std::int64_t parsed{};
        const auto [end, error] = std::from_chars(
            input_.data() + begin,
            input_.data() + cursor_,
            parsed);
        if (error != std::errc{} || end != input_.data() + cursor_) {
            fail("integer is outside the signed 64-bit range");
        }
        json_value result;
        result.type = json_value::kind::number;
        result.number = parsed;
        return result;
    }

    [[nodiscard]] json_value parse_array() {
        require('[');
        json_value result;
        result.type = json_value::kind::array;
        if (consume(']')) return result;
        for (;;) {
            result.array.push_back(parse_value());
            if (consume(']')) return result;
            require(',');
        }
    }

    [[nodiscard]] json_value parse_object() {
        require('{');
        json_value result;
        result.type = json_value::kind::object;
        if (consume('}')) return result;
        for (;;) {
            skip_space();
            if (cursor_ >= input_.size() || input_[cursor_] != '"') {
                fail("object key must be a string");
            }
            std::string key = parse_string();
            require(':');
            if (!result.object.emplace(
                    std::move(key), parse_value()).second) {
                fail("duplicate object key");
            }
            if (consume('}')) return result;
            require(',');
        }
    }

    std::string_view input_;
    std::size_t cursor_{};
};

[[nodiscard]] inline const json_value& object_member(
    const json_value& object,
    std::string_view name) {
    if (object.type != json_value::kind::object) {
        throw std::runtime_error(
            "GUC parity ledger root/member is not an object");
    }
    const auto found = object.object.find(name);
    if (found == object.object.end()) {
        throw std::runtime_error(
            "GUC parity ledger is missing '" + std::string{name} + "'");
    }
    return found->second;
}

[[nodiscard]] inline std::string json_string(
    const json_value& value,
    std::string_view field) {
    if (value.type != json_value::kind::string) {
        throw std::runtime_error(
            "GUC parity ledger field '" + std::string{field}
            + "' is not a string");
    }
    return value.string;
}

[[nodiscard]] inline std::optional<std::string> nullable_json_string(
    const json_value& value,
    std::string_view field) {
    if (value.type == json_value::kind::null) return std::nullopt;
    return json_string(value, field);
}

[[nodiscard]] inline std::size_t json_size(
    const json_value& value,
    std::string_view field) {
    if (value.type != json_value::kind::number || value.number < 0) {
        throw std::runtime_error(
            "GUC parity ledger field '" + std::string{field}
            + "' is not a nonnegative integer");
    }
    return static_cast<std::size_t>(value.number);
}

[[nodiscard]] inline bool json_bool(
    const json_value& value,
    std::string_view field) {
    if (value.type != json_value::kind::boolean) {
        throw std::runtime_error(
            "GUC parity ledger field '" + std::string{field}
            + "' is not boolean");
    }
    return value.boolean;
}

struct ledger_enum_value {
    std::string name;
    std::string wire_expression;
    bool hidden{};
    std::string feature_predicate;
    std::optional<std::string> alias_of;
};

struct ledger_omission {
    std::string name;
    std::string wire_expression;
};

struct ledger_row {
    std::size_t ordinal{};
    std::string source_owner;
    std::string name;
    std::string group;
    std::string member;
    std::string kind;
    std::string storage_kind;
    std::string legacy_storage;
    std::string default_expression;
    std::optional<std::string> minimum_expression;
    std::optional<std::string> maximum_expression;
    std::string context;
    std::vector<std::string> flags;
    std::optional<std::string> unit;
    std::array<std::optional<std::string>, 3> hooks;
    std::string feature_predicate;
    std::vector<std::string> profiles;
    std::optional<std::string> default_semantic;
    std::vector<ledger_enum_value> enum_values;
    std::vector<ledger_omission> omissions;
};

struct parsed_ledger {
    std::vector<ledger_row> rows;
    std::map<std::string, std::size_t, std::less<>> profile_counts;
};

[[nodiscard]] inline std::vector<std::string> string_array(
    const json_value& value,
    std::string_view field) {
    if (value.type != json_value::kind::array) {
        throw std::runtime_error(
            "GUC parity ledger field '" + std::string{field}
            + "' is not an array");
    }
    std::vector<std::string> result;
    result.reserve(value.array.size());
    for (const json_value& item : value.array) {
        result.push_back(json_string(item, field));
    }
    return result;
}

[[nodiscard]] inline parsed_ledger parse_ledger(
    std::string_view content) {
    const json_value root = json_parser{content}.parse();
    if (root.type != json_value::kind::object) {
        throw std::runtime_error(
            "GUC parity ledger root is not an object");
    }
    const std::set<std::string, std::less<>> expected_root_keys{
        "baseline_legacy_description_hash_is_non_authoritative",
        "columns",
        "description_authority",
        "ordinal_base",
        "profiles",
        "rows",
        "schema_version",
        "source_owner_order",
    };
    std::set<std::string, std::less<>> observed_root_keys;
    for (const auto& [name, value] : root.object) {
        static_cast<void>(value);
        observed_root_keys.emplace(name);
    }
    if (observed_root_keys != expected_root_keys) {
        throw std::runtime_error(
            "GUC parity ledger root schema differs from version 1");
    }
    if (json_size(object_member(root, "schema_version"), "schema_version")
        != 1) {
        throw std::runtime_error(
            "unsupported GUC parity ledger schema version");
    }
    if (json_string(
            object_member(root, "description_authority"),
            "description_authority")
        != "cleaned_doxygen_only") {
        throw std::runtime_error(
            "GUC descriptions must be authoritative cleaned Doxygen");
    }
    if (!json_bool(
            object_member(
                root,
                "baseline_legacy_description_hash_is_non_authoritative"),
            "baseline_legacy_description_hash_is_non_authoritative")) {
        throw std::runtime_error(
            "legacy GUC description hashes cannot be generation authority");
    }
    if (json_size(object_member(root, "ordinal_base"), "ordinal_base")
        != 0) {
        throw std::runtime_error(
            "GUC parity ledger ordinals must be zero based");
    }
    if (string_array(
            object_member(root, "source_owner_order"),
            "source_owner_order")
        != std::vector<std::string>{
            "column", "custom_scan", "batch_sort_scan"}) {
        throw std::runtime_error(
            "GUC parity ledger source-owner order changed");
    }

    const json_value& columns_value = object_member(root, "columns");
    const std::vector<std::string> columns =
        string_array(columns_value, "columns");
    const std::vector<std::string> expected_columns{
        "registration_ordinal",
        "source_owner",
        "name",
        "group",
        "member",
        "kind",
        "storage_kind",
        "storage",
        "default_expression",
        "minimum_expression",
        "maximum_expression",
        "context",
        "flags",
        "unit",
        "hooks",
        "feature_predicate",
        "profiles",
        "baseline_legacy_description_sha256",
        "default_semantic",
        "enum_rows_name_wire_hidden_feature_alias_of",
        "enum_omissions_name_wire",
    };
    if (columns != expected_columns) {
        throw std::runtime_error(
            "GUC parity ledger columns differ from version 1");
    }
    std::map<std::string, std::size_t, std::less<>> column;
    for (std::size_t index = 0; index < columns.size(); ++index) {
        if (!column.emplace(columns[index], index).second) {
            throw std::runtime_error(
                "duplicate GUC parity ledger column '" + columns[index]
                + "'");
        }
    }
    parsed_ledger result;
    const json_value& profiles = object_member(root, "profiles");
    if (profiles.type != json_value::kind::object) {
        throw std::runtime_error(
            "GUC parity ledger profiles are not an object");
    }
    if (profiles.object.size() != 2
        || !profiles.object.contains("production")
        || !profiles.object.contains("test_indexes")) {
        throw std::runtime_error(
            "GUC parity ledger profile schema changed");
    }
    for (const auto& [name, descriptor] : profiles.object) {
        if (descriptor.type != json_value::kind::object
            || descriptor.object.size() != 1
            || !descriptor.object.contains("row_count")) {
            throw std::runtime_error(
                "GUC parity ledger profile descriptor schema changed");
        }
        result.profile_counts.emplace(
            name,
            json_size(
                object_member(descriptor, "row_count"),
                "profiles.row_count"));
    }

    const json_value& rows = object_member(root, "rows");
    if (rows.type != json_value::kind::array) {
        throw std::runtime_error(
            "GUC parity ledger rows are not an array");
    }
    result.rows.reserve(rows.array.size());
    const auto cell = [&column](const json_value& row, std::string_view name)
        -> const json_value& {
        if (row.type != json_value::kind::array) {
            throw std::runtime_error(
                "GUC parity ledger row is not an array");
        }
        const auto column_it = column.find(name);
        if (column_it == column.end()) {
            throw std::runtime_error(
                "GUC parity ledger lacks required column '"
                + std::string{name} + "'");
        }
        const std::size_t index = column_it->second;
        if (index >= row.array.size()) {
            throw std::runtime_error(
                "short GUC parity ledger row for column '"
                + std::string{name} + "'");
        }
        return row.array[index];
    };
    for (const json_value& row_value : rows.array) {
        if (row_value.type != json_value::kind::array
            || row_value.array.size() != columns.size()) {
            throw std::runtime_error(
                "GUC parity ledger row width differs from version 1");
        }
        const json_value& hooks_value = cell(row_value, "hooks");
        if (hooks_value.type != json_value::kind::array
            || hooks_value.array.size() != 3) {
            throw std::runtime_error(
                "GUC parity ledger hooks must contain three fields");
        }
        std::array<std::optional<std::string>, 3> hooks{
            nullable_json_string(hooks_value.array[0], "hooks.check"),
            nullable_json_string(hooks_value.array[1], "hooks.assign"),
            nullable_json_string(hooks_value.array[2], "hooks.show"),
        };
        const std::string baseline_description_hash = json_string(
            cell(row_value, "baseline_legacy_description_sha256"),
            "baseline_legacy_description_sha256");
        if (baseline_description_hash.size() != 64
            || !std::ranges::all_of(
                baseline_description_hash,
                [](char value) {
                    return (value >= '0' && value <= '9')
                        || (value >= 'a' && value <= 'f');
                })) {
            throw std::runtime_error(
                "GUC legacy description hash is not lowercase SHA-256");
        }
        ledger_row row{
            .ordinal = json_size(
                cell(row_value, "registration_ordinal"),
                "registration_ordinal"),
            .source_owner = json_string(
                cell(row_value, "source_owner"), "source_owner"),
            .name = json_string(cell(row_value, "name"), "name"),
            .group = json_string(cell(row_value, "group"), "group"),
            .member = json_string(cell(row_value, "member"), "member"),
            .kind = json_string(cell(row_value, "kind"), "kind"),
            .storage_kind = json_string(
                cell(row_value, "storage_kind"), "storage_kind"),
            .legacy_storage = json_string(
                cell(row_value, "storage"), "storage"),
            .default_expression = json_string(
                cell(row_value, "default_expression"),
                "default_expression"),
            .minimum_expression = nullable_json_string(
                cell(row_value, "minimum_expression"),
                "minimum_expression"),
            .maximum_expression = nullable_json_string(
                cell(row_value, "maximum_expression"),
                "maximum_expression"),
            .context = json_string(
                cell(row_value, "context"), "context"),
            .flags = string_array(
                cell(row_value, "flags"), "flags"),
            .unit = nullable_json_string(
                cell(row_value, "unit"), "unit"),
            .hooks = std::move(hooks),
            .feature_predicate = json_string(
                cell(row_value, "feature_predicate"),
                "feature_predicate"),
            .profiles = string_array(
                cell(row_value, "profiles"), "profiles"),
            .default_semantic = nullable_json_string(
                cell(row_value, "default_semantic"),
                "default_semantic"),
        };
        const json_value& enum_rows = cell(
            row_value,
            "enum_rows_name_wire_hidden_feature_alias_of");
        if (enum_rows.type != json_value::kind::array) {
            throw std::runtime_error(
                "GUC enum rows are not an array");
        }
        for (const json_value& enum_row : enum_rows.array) {
            if (enum_row.type != json_value::kind::array
                || enum_row.array.size() != 5) {
                throw std::runtime_error(
                    "GUC enum row must contain five fields");
            }
            row.enum_values.push_back(ledger_enum_value{
                .name = json_string(enum_row.array[0], "enum.name"),
                .wire_expression =
                    json_string(enum_row.array[1], "enum.wire"),
                .hidden = json_size(
                    enum_row.array[2], "enum.hidden") != 0,
                .feature_predicate =
                    json_string(enum_row.array[3], "enum.feature"),
                .alias_of = nullable_json_string(
                    enum_row.array[4], "enum.alias_of"),
            });
        }
        const json_value& omissions = cell(
            row_value, "enum_omissions_name_wire");
        if (omissions.type != json_value::kind::array) {
            throw std::runtime_error(
                "GUC enum omissions are not an array");
        }
        for (const json_value& omission : omissions.array) {
            if (omission.type != json_value::kind::array
                || omission.array.size() != 2) {
                throw std::runtime_error(
                    "GUC enum omission must contain two fields");
            }
            row.omissions.push_back(ledger_omission{
                .name = json_string(omission.array[0], "omission.name"),
                .wire_expression =
                    json_string(omission.array[1], "omission.wire"),
            });
        }
        result.rows.push_back(std::move(row));
    }
    std::ranges::sort(
        result.rows, {}, &ledger_row::ordinal);
    std::set<std::string, std::less<>> row_names;
    std::set<std::string, std::less<>> legacy_storages;
    std::map<std::string, std::size_t, std::less<>>
        observed_profile_counts;
    for (const auto& [profile, count] : result.profile_counts) {
        static_cast<void>(count);
        observed_profile_counts.emplace(profile, 0);
    }
    const auto blank_feature_predicate =
        [](std::string_view value) noexcept {
            return value.empty()
                || std::ranges::all_of(
                    value,
                    [](char item) {
                        return item == ' ' || item == '\t'
                            || item == '\n' || item == '\r';
                    });
        };
    for (std::size_t index = 0; index < result.rows.size(); ++index) {
        const ledger_row& row = result.rows[index];
        if (row.ordinal != index) {
            throw std::runtime_error(
                "GUC parity ledger registration ordinals are not dense");
        }
        if (!row_names.emplace(row.name).second) {
            throw std::runtime_error(
                "duplicate GUC parity ledger row '" + row.name + "'");
        }
        if (!legacy_storages.emplace(row.legacy_storage).second) {
            throw std::runtime_error(
                "duplicate GUC parity ledger legacy storage '"
                + row.legacy_storage + "'");
        }
        if (blank_feature_predicate(row.feature_predicate)) {
            throw std::runtime_error(
                "GUC parity ledger row '" + row.name
                + "' has an empty feature predicate");
        }
        for (const ledger_enum_value& value : row.enum_values) {
            if (blank_feature_predicate(value.feature_predicate)) {
                throw std::runtime_error(
                    "GUC parity ledger enum value '" + row.name + "."
                    + value.name + "' has an empty feature predicate");
            }
        }
        const std::map<std::string, std::string, std::less<>>
            storage_kind_by_guc_kind{
                {"bool", "bool"},
                {"enum", "int32_enum"},
                {"integer", "int32"},
                {"real", "double"},
                {"string", "string"},
            };
        const auto expected_storage =
            storage_kind_by_guc_kind.find(row.kind);
        if (expected_storage == storage_kind_by_guc_kind.end()
            || expected_storage->second != row.storage_kind) {
            throw std::runtime_error(
                "GUC parity ledger row '" + row.name
                + "' has an invalid kind/storage ABI pair");
        }
        if (std::ranges::any_of(
                row.hooks,
                [](const std::optional<std::string>& hook) {
                    return hook.has_value();
                })) {
            throw std::runtime_error(
                "GUC parity ledger schema version 1 requires null hooks");
        }
        if (row.name
                != row.group + "."
                    + row.member) {
            throw std::runtime_error(
                "GUC parity ledger row '" + row.name
                + "' has inconsistent group/member identity");
        }
        if (row.source_owner != "column"
            && row.source_owner != "custom_scan"
            && row.source_owner != "batch_sort_scan") {
            throw std::runtime_error(
                "GUC parity ledger row '" + row.name
                + "' has an unknown source owner");
        }
        std::set<std::string, std::less<>> row_profiles;
        for (const std::string& profile : row.profiles) {
            if (!row_profiles.emplace(profile).second) {
                throw std::runtime_error(
                    "GUC parity ledger row '" + row.name
                    + "' repeats profile '" + profile + "'");
            }
            const auto known = observed_profile_counts.find(profile);
            if (known == observed_profile_counts.end()) {
                throw std::runtime_error(
                    "GUC parity ledger row '" + row.name
                    + "' references unknown profile '" + profile + "'");
            }
            ++known->second;
        }
        const std::vector<std::string> expected_profiles =
            row.name == "bioseq.test_force_paper_index_failure"
            ? std::vector<std::string>{"test_indexes"}
            : std::vector<std::string>{
                "production", "test_indexes"};
        if (row.profiles != expected_profiles) {
            throw std::runtime_error(
                "GUC parity ledger row '" + row.name
                + "' has an unexpected profile selection");
        }
    }
    for (const auto& [profile, expected] : result.profile_counts) {
        const std::size_t observed =
            observed_profile_counts.at(profile);
        if (observed != expected) {
            throw std::runtime_error(
                "GUC parity ledger profile '" + profile + "' declares "
                + std::to_string(expected) + " rows but selects "
                + std::to_string(observed));
        }
    }
    return result;
}

[[nodiscard]] inline std::string lower_ascii(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char item : value) {
        result.push_back(
            item >= 'A' && item <= 'Z'
            ? static_cast<char>(item - 'A' + 'a')
            : item);
    }
    return result;
}

[[nodiscard]] inline std::string normalized_expression(
    std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char item : value) {
        if (item == ' ' || item == '\t' || item == '\n'
            || item == '\r' || item == '\'' || item == '_') {
            continue;
        }
        result.push_back(
            item >= 'A' && item <= 'Z'
            ? static_cast<char>(item - 'A' + 'a')
            : item);
    }
    while (!result.empty()
           && (result.back() == 'u' || result.back() == 'l')) {
        result.pop_back();
    }
    if (result == "compressionlevelmin") {
        result = "compressionlevelminimum";
    } else if (result == "compressionlevelmax") {
        result = "compressionlevelmaximum";
    } else if (result == "striperowcountminimum") {
        result = "striperowcountminimum";
    } else if (result == "striperowcountmaximum") {
        result = "striperowcountmaximum";
    } else if (result == "chunkrowcountminimum") {
        result = "chunkrowcountminimum";
    } else if (result == "chunkrowcountmaximum") {
        result = "chunkrowcountmaximum";
    }
    return result;
}

[[nodiscard]] inline bool numeric_expression(
    std::string_view value,
    long double& result) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const char item : value) {
        if (item == '\'' || item == ' ' || item == '\t'
            || item == '\n' || item == '\r') {
            continue;
        }
        normalized.push_back(item);
    }
    while (!normalized.empty()
           && (normalized.back() == 'u' || normalized.back() == 'U'
               || normalized.back() == 'l'
               || normalized.back() == 'L')) {
        normalized.pop_back();
    }
    if (normalized.empty() || normalized.contains("<<")) return false;
    char* end = nullptr;
    result = std::strtold(normalized.c_str(), &end);
    return end != normalized.c_str() && end == normalized.c_str()
        + normalized.size();
}

[[nodiscard]] inline bool equivalent_expression(
    std::string_view authored,
    std::string_view baseline) {
    if (normalized_expression(authored)
        == normalized_expression(baseline)) {
        return true;
    }
    long double left{};
    long double right{};
    return numeric_expression(authored, left)
        && numeric_expression(baseline, right) && left == right;
}

[[nodiscard]] inline std::string natural_guc_name(
    std::string_view cpp_qualified_name) {
    std::string path{cpp_qualified_name};
    std::size_t offset = 0;
    while ((offset = path.find("::", offset)) != std::string::npos) {
        path.replace(offset, 2, ".");
        ++offset;
    }
    const std::string marker = ".settings.";
    if (const std::size_t settings = path.find(marker);
        settings != std::string::npos) {
        path.erase(settings, marker.size() - 1);
    }
    return path;
}

[[nodiscard]] inline std::string candidate_cpp_storage(
    std::string_view group,
    std::string_view member) {
    std::string result{group};
    std::size_t offset = 0;
    while ((offset = result.find('.', offset)) != std::string::npos) {
        result.replace(offset, 1, "::");
        offset += 2;
    }
    result += "::settings::";
    result += member;
    return result;
}

[[nodiscard]] inline std::string candidate_source_module(
    std::string_view group) {
    return std::string{group} + ".sql_guc_settings";
}

[[nodiscard]] inline std::string compact_cpp_type(
    std::string_view spelling) {
    std::string compact;
    compact.reserve(spelling.size());
    for (const char value : spelling) {
        if (value == ' ' || value == '\t' || value == '\n'
            || value == '\r' || value == '_') {
            continue;
        }
        compact.push_back(
            value >= 'A' && value <= 'Z'
            ? static_cast<char>(value - 'A' + 'a')
            : value);
    }
    return compact;
}

[[nodiscard]] inline std::string storage_kind_from_entity(
    const collected_entity& entity) {
    const std::string compact = compact_cpp_type(
        entity.cpp_return_type);
    if (compact == "bool") return "bool";
    if (compact == "int" || compact == "int32t"
        || compact == "std::int32t") {
        return "int32";
    }
    if (compact == "double") return "double";
    if (compact.contains("postgresenumstorage<")) {
        return "int32_enum";
    }
    if (compact.contains("char*") || compact.contains("string")) {
        return "string";
    }
    throw std::runtime_error(
        "unsupported C++ storage ABI for GUC '" + entity.name
        + "': " + entity.cpp_return_type);
}

[[nodiscard]] inline guc_kind kind_from_entity(
    const collected_entity& entity) {
    const std::string compact = compact_cpp_type(
        entity.cpp_return_type);
    if (compact.contains("postgresenumstorage<")) {
        return guc_kind::enumeration;
    }
    if (compact == "bool") return guc_kind::boolean;
    if (compact == "int" || compact == "int32t") {
        return guc_kind::integer;
    }
    if (compact == "double") return guc_kind::real;
    if (compact.contains("char*") || compact.contains("string")) {
        return guc_kind::string;
    }
    throw std::runtime_error(
        "unsupported C++ storage type for GUC '" + entity.name
        + "': " + entity.cpp_return_type);
}

[[nodiscard]] inline std::string option_leaf(
    const collected_entity& entity,
    std::string_view name,
    std::string_view fallback) {
    const option_value* option = find_option(entity, name);
    if (!option) return std::string{fallback};
    const std::string& expression = option->expression;
    const std::size_t separator = expression.rfind("::");
    return separator == std::string::npos
        ? expression
        : expression.substr(separator + 2);
}

[[nodiscard]] inline std::vector<std::string> guc_flags(
    const collected_entity& entity) {
    const option_value* option = find_option(entity, "flags");
    if (!option) return {};
    static constexpr std::string_view names[] = {
        "no_show_all",
        "not_in_sample",
        "unit_kb",
        "unit_mb",
        "unit_blocks",
        "unit_ms",
        "unit_s",
        "unit_min",
    };
    std::vector<std::string> result;
    for (const std::string_view name : names) {
        if (option->expression.contains(name)
            && !name.starts_with("unit_")) {
            result.emplace_back(name);
        }
    }
    return result;
}

[[nodiscard]] inline std::optional<std::string> guc_unit(
    const collected_entity& entity) {
    const option_value* option = find_option(entity, "flags");
    if (!option) return std::nullopt;
    static constexpr std::pair<std::string_view, std::string_view>
        units[] = {
            {"unit_kb", "kb"},
            {"unit_mb", "mb"},
            {"unit_blocks", "blocks"},
            {"unit_ms", "ms"},
            {"unit_s", "s"},
            {"unit_min", "min"},
        };
    for (const auto& [token, spelling] : units) {
        if (option->expression.contains(token)) {
            return std::string{spelling};
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline std::string context_name(
        const collected_entity& entity) {
    const std::string value = option_leaf(entity, "context", "user");
    if (value == "user") return "user_set";
    if (value == "superuser") return "superuser_set";
    return value;
}

[[nodiscard]] inline bool resets_plan_cache(
        const collected_entity& entity) {
    const std::string value =
        option_leaf(entity, "plan_cache", "unchanged");
    if (value == "unchanged") return false;
    if (value == "reset") return true;
    throw std::runtime_error(
        "GUC '" + entity.cpp_qualified_name
        + "' plan_cache policy must be unchanged or reset");
}

[[nodiscard]] inline std::optional<std::string> range_option(
    const collected_entity& entity,
    std::string_view name) {
    if (const option_value* option = find_option(entity, name)) {
        return option->expression;
    }
    return std::nullopt;
}

[[nodiscard]] inline std::string enum_type_leaf(
    std::string_view cpp_type) {
    const std::size_t marker = cpp_type.find("postgres_enum_storage");
    if (marker == std::string_view::npos) return {};
    const std::size_t open = cpp_type.find('<', marker);
    if (open == std::string_view::npos) return {};
    int depth = 0;
    for (std::size_t index = open; index < cpp_type.size(); ++index) {
        if (cpp_type[index] == '<') {
            ++depth;
        } else if (cpp_type[index] == '>') {
            --depth;
            if (depth == 0) {
                std::string value{cpp_type.substr(
                    open + 1, index - open - 1)};
                const std::size_t separator = value.rfind("::");
                return separator == std::string::npos
                    ? value
                    : value.substr(separator + 2);
            }
        }
    }
    return {};
}

[[nodiscard]] inline std::string enum_default_leaf(
    std::string_view initializer) {
    const auto trim = [](std::string_view value) noexcept {
        while (!value.empty()
               && (value.front() == ' ' || value.front() == '\t'
                   || value.front() == '\n' || value.front() == '\r')) {
            value.remove_prefix(1);
        }
        while (!value.empty()
               && (value.back() == ' ' || value.back() == '\t'
                   || value.back() == '\n' || value.back() == '\r')) {
            value.remove_suffix(1);
        }
        return value;
    };
    initializer = trim(initializer);
    if (initializer.ends_with('}')) {
        const std::size_t open = initializer.find('{');
        if (open != std::string_view::npos) {
            initializer = trim(initializer.substr(
                open + 1, initializer.size() - open - 2));
        }
    }
    const std::size_t separator = initializer.rfind("::");
    if (separator == std::string_view::npos) {
        const bool identifier =
            !initializer.empty()
            && ((initializer.front() >= 'a'
                    && initializer.front() <= 'z')
                || (initializer.front() >= 'A'
                    && initializer.front() <= 'Z')
                || initializer.front() == '_')
            && std::ranges::all_of(
                initializer.substr(1),
                [](char value) {
                    return (value >= 'a' && value <= 'z')
                        || (value >= 'A' && value <= 'Z')
                        || (value >= '0' && value <= '9')
                        || value == '_';
                });
        return identifier ? std::string{initializer} : std::string{};
    }
    std::size_t end = separator + 2;
    while (end < initializer.size()
           && ((initializer[end] >= 'a' && initializer[end] <= 'z')
               || (initializer[end] >= 'A' && initializer[end] <= 'Z')
               || (initializer[end] >= '0' && initializer[end] <= '9')
               || initializer[end] == '_')) {
        ++end;
    }
    return std::string{initializer.substr(
        separator + 2, end - separator - 2)};
}

[[nodiscard]] inline bool condition_enabled_for_profile(
    std::string_view condition,
    guc_profile profile) {
    if (condition.empty() || condition == "true") return true;
    if (condition.contains("BIOSEQ_TEST_INDEXES")) {
        return profile == guc_profile::test_indexes;
    }
    return true;
}

[[nodiscard]] inline std::string canonical_feature_predicate(
    std::string_view condition) {
    const auto trim = [](std::string_view value) noexcept {
        while (!value.empty()
               && (value.front() == ' ' || value.front() == '\t'
                   || value.front() == '\n'
                   || value.front() == '\r')) {
            value.remove_prefix(1);
        }
        while (!value.empty()
               && (value.back() == ' ' || value.back() == '\t'
                   || value.back() == '\n'
                   || value.back() == '\r')) {
            value.remove_suffix(1);
        }
        return value;
    };
    if (condition.empty()) return "true";
    condition = trim(condition);
    if (condition.empty()) {
        throw std::runtime_error(
            "GUC feature predicate is whitespace-only");
    }
    bool removed_wrapper = false;
    while (condition.size() >= 2
           && condition.front() == '('
           && condition.back() == ')') {
        int depth = 0;
        bool whole_expression_wrapped = true;
        for (std::size_t index = 0; index < condition.size(); ++index) {
            if (condition[index] == '(') {
                ++depth;
            } else if (condition[index] == ')') {
                --depth;
                if (depth < 0
                    || (depth == 0
                        && index + 1 != condition.size())) {
                    whole_expression_wrapped = false;
                    break;
                }
            }
        }
        if (!whole_expression_wrapped || depth != 0) break;
        condition = trim(condition.substr(1, condition.size() - 2));
        removed_wrapper = true;
    }
    if (condition.empty() && removed_wrapper) {
        throw std::runtime_error(
            "GUC feature predicate has an empty parenthesized "
            "expression");
    }
    return std::string{condition};
}

[[nodiscard]] inline std::string effective_feature_predicate(
    std::string_view owner_condition,
    std::string_view member_condition) {
    const std::string owner =
        canonical_feature_predicate(owner_condition);
    const std::string member =
        canonical_feature_predicate(member_condition);
    if (owner == "true") return member;
    if (member == "true" || member == owner) return owner;
    return "(" + owner + ") && (" + member + ")";
}

[[nodiscard]] inline std::string condition_at(
    std::string_view source,
    std::size_t offset) {
    const auto trim = [](std::string_view value) noexcept {
        while (!value.empty()
               && (value.front() == ' ' || value.front() == '\t'
                   || value.front() == '\n'
                   || value.front() == '\r')) {
            value.remove_prefix(1);
        }
        while (!value.empty()
               && (value.back() == ' ' || value.back() == '\t'
                   || value.back() == '\n'
                   || value.back() == '\r')) {
            value.remove_suffix(1);
        }
        return value;
    };
    std::vector<std::string> stack;
    std::size_t line_begin = 0;
    while (line_begin < offset) {
        const std::size_t newline = source.find('\n', line_begin);
        const std::size_t line_end = std::min(
            newline == std::string_view::npos ? source.size() : newline,
            offset);
        const std::string_view line = trim(
            source.substr(line_begin, line_end - line_begin));
        if (line.starts_with("#if ")) {
            stack.emplace_back(trim(line.substr(4)));
        } else if (line.starts_with("#ifdef ")) {
            stack.emplace_back(
                "defined(" + std::string{trim(line.substr(7))} + ")");
        } else if (line.starts_with("#ifndef ")) {
            stack.emplace_back(
                "!defined(" + std::string{trim(line.substr(8))} + ")");
        } else if (line.starts_with("#elif ") && !stack.empty()) {
            stack.back() = std::string{trim(line.substr(6))};
        } else if (line == "#else" && !stack.empty()) {
            stack.back() = "!(" + stack.back() + ")";
        } else if (line == "#endif" && !stack.empty()) {
            stack.pop_back();
        }
        if (newline == std::string_view::npos) break;
        line_begin = newline + 1;
    }
    std::string result;
    for (const std::string& condition : stack) {
        if (!result.empty()) result += " && ";
        result += "(" + condition + ")";
    }
    return result;
}

[[nodiscard]] inline bool identifier_token(
    std::string_view token) noexcept {
    if (token.empty()
        || !((token.front() >= 'a' && token.front() <= 'z')
             || (token.front() >= 'A' && token.front() <= 'Z')
             || token.front() == '_')) {
        return false;
    }
    return std::ranges::all_of(
        token.substr(1),
        [](char value) {
            return (value >= 'a' && value <= 'z')
                || (value >= 'A' && value <= 'Z')
                || (value >= '0' && value <= '9') || value == '_';
        });
}

struct parsed_enumerator {
    std::string name;
    std::string qualified_name;
    std::string initializer_text;
    std::string active_condition;
};

struct parsed_enum {
    std::string qualified_name;
    std::map<std::string, parsed_enumerator, std::less<>> values;
};

[[nodiscard]] inline parsed_enum parse_enum(
    const collected_program& program,
    const collected_entity& entity,
    std::string_view leaf) {
    const auto unit = std::ranges::find(
        program.translation_units,
        entity.source_path,
        &clang_structural::translation_unit::source_path);
    if (unit == program.translation_units.end()) {
        throw std::runtime_error(
            "cannot find translation unit for enum GUC '" + entity.name
            + "'");
    }
    const std::string source = read_file(unit->source_path);
    const auto& tokens = unit->tokens;
    std::size_t enum_name = tokens.size();
    std::size_t open = tokens.size();
    std::size_t close = tokens.size();
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        if (tokens[index].spelling != "enum") continue;
        std::size_t cursor = index + 1;
        if (cursor < tokens.size()
            && (tokens[cursor].spelling == "class"
                || tokens[cursor].spelling == "struct")) {
            ++cursor;
        }
        if (cursor >= tokens.size()
            || tokens[cursor].spelling != leaf) {
            continue;
        }
        enum_name = cursor;
        while (cursor < tokens.size()
               && tokens[cursor].spelling != "{"
               && tokens[cursor].spelling != ";") {
            ++cursor;
        }
        if (cursor >= tokens.size()
            || tokens[cursor].spelling != "{") {
            continue;
        }
        open = cursor;
        int depth = 1;
        while (++cursor < tokens.size()) {
            if (tokens[cursor].spelling == "{") {
                ++depth;
            } else if (tokens[cursor].spelling == "}") {
                --depth;
                if (depth == 0) {
                    close = cursor;
                    break;
                }
            }
        }
        if (close != tokens.size()) break;
    }
    if (enum_name == tokens.size() || close == tokens.size()) {
        throw std::runtime_error(
            "cannot resolve enum type '" + std::string{leaf}
            + "' used by GUC '" + entity.name + "'");
    }

    const std::string settings_marker = "::settings::";
    const std::size_t settings =
        entity.cpp_qualified_name.find(settings_marker);
    if (settings == std::string::npos) {
        throw std::runtime_error(
            "enum GUC '" + entity.name
            + "' is not nested in a natural settings owner");
    }
    parsed_enum result{
        .qualified_name =
            entity.cpp_qualified_name.substr(0, settings)
            + "::" + std::string{leaf},
    };
    const auto parse_segment = [
        &result,
        &source,
        &tokens](std::size_t begin, std::size_t end) {
        while (begin < end
               && (tokens[begin].spelling == ","
                   || tokens[begin].spelling == ";")) {
            ++begin;
        }
        std::size_t equals = end;
        std::size_t name_token = end;
        int parentheses = 0;
        int braces = 0;
        int brackets = 0;
        for (std::size_t index = begin; index < end; ++index) {
            const std::string& spelling = tokens[index].spelling;
            if (spelling == "(") ++parentheses;
            else if (spelling == ")") --parentheses;
            else if (spelling == "{") ++braces;
            else if (spelling == "}") --braces;
            else if (spelling == "[") ++brackets;
            else if (spelling == "]") --brackets;
            else if (spelling == "=" && parentheses == 0
                     && braces == 0 && brackets == 0) {
                equals = index;
                break;
            } else if (parentheses == 0 && braces == 0
                       && brackets == 0
                       && identifier_token(spelling)) {
                name_token = index;
            }
        }
        if (name_token == end) return;
        const std::string name = tokens[name_token].spelling;
        std::string initializer;
        if (equals != end && equals + 1 < end) {
            const std::size_t byte_begin =
                tokens[equals + 1].range.begin.byte_offset;
            const std::size_t byte_end =
                tokens[end - 1].range.end.byte_offset;
            initializer = source.substr(
                byte_begin, byte_end - byte_begin);
        }
        const std::size_t offset =
            tokens[name_token].range.begin.byte_offset;
        result.values.emplace(
            name,
            parsed_enumerator{
                .name = name,
                .qualified_name = result.qualified_name + "::" + name,
                .initializer_text = std::move(initializer),
                .active_condition = condition_at(source, offset),
            });
    };

    std::size_t segment_begin = open + 1;
    int parentheses = 0;
    int braces = 0;
    int brackets = 0;
    for (std::size_t index = segment_begin; index < close; ++index) {
        const std::string& spelling = tokens[index].spelling;
        if (spelling == "(") ++parentheses;
        else if (spelling == ")") --parentheses;
        else if (spelling == "{") ++braces;
        else if (spelling == "}") --braces;
        else if (spelling == "[") ++brackets;
        else if (spelling == "]") --brackets;
        else if (spelling == "," && parentheses == 0
                 && braces == 0 && brackets == 0) {
            parse_segment(segment_begin, index);
            segment_begin = index + 1;
        }
    }
    parse_segment(segment_begin, close);
    if (result.values.empty()) {
        throw std::runtime_error(
            "enum type '" + result.qualified_name
            + "' has no values");
    }
    return result;
}

struct value_exception {
    bool omit{};
    std::optional<std::string> alias;
    bool alias_hidden{};
};

[[nodiscard]] inline std::map<
    std::string,
    value_exception,
    std::less<>>
guc_value_exceptions(
    const collected_program& program,
    std::string_view enum_cpp_name) {
    std::map<std::string, value_exception, std::less<>> result;
    for (const collected_entity& entity : program.entities) {
        if (entity.kind != entity_kind::guc_value
            || entity.enclosing_cpp_name != enum_cpp_name) {
            continue;
        }
        value_exception exception;
        if (const option_value* omit = find_option(entity, "omit")) {
            exception.omit =
                normalized_expression(omit->expression) == "true";
        }
        if (const option_value* aliases = find_option(entity, "aliases")) {
            const std::string& expression = aliases->expression;
            const std::size_t spelling = expression.find(".spelling=");
            if (spelling != std::string::npos) {
                const std::size_t quote = expression.find('"', spelling);
                const std::size_t close = quote == std::string::npos
                    ? std::string::npos
                    : expression.find('"', quote + 1);
                if (close != std::string::npos) {
                    exception.alias = expression.substr(
                        quote + 1, close - quote - 1);
                }
            }
            exception.alias_hidden =
                normalized_expression(expression).contains(
                    ".hidden=true");
        }
        result.emplace(entity.name, std::move(exception));
    }
    return result;
}

inline void validate_documentation(const collected_entity& entity) {
    if (!entity.documentation.has_explicit_brief
        || entity.documentation.brief.empty()) {
        throw std::runtime_error(
            "GUC '" + natural_guc_name(entity.cpp_qualified_name)
            + "' requires an explicit Doxygen @brief");
    }
    const std::string documentation = lower_ascii(
        entity.documentation.brief + " "
        + entity.documentation.details);
    static constexpr std::string_view forbidden[] = {
        "doc0",
        "prototype",
        " phase ",
        " stage ",
    };
    for (const std::string_view token : forbidden) {
        if (documentation.contains(token)) {
            throw std::runtime_error(
                "GUC '" + natural_guc_name(entity.cpp_qualified_name)
                + "' documentation describes project history instead of "
                  "current behavior");
        }
    }
}

inline void validate_scalar_parity(
    const collected_entity& entity,
    const ledger_row& row,
    guc_kind kind) {
    const auto require_equivalent = [&row](
                                         std::string_view authored,
                                         std::string_view baseline,
                                         std::string_view field) {
        if (!equivalent_expression(authored, baseline)) {
            throw std::runtime_error(
                "GUC '" + row.name + "' " + std::string{field}
                + " differs from the parity ledger: authored '"
                + std::string{authored} + "', ledger '"
                + std::string{baseline} + "'");
        }
    };
    require_equivalent(
        entity.initializer_expression,
        row.default_expression,
        "default");
    if (kind == guc_kind::integer || kind == guc_kind::real) {
        const std::optional<std::string> minimum =
            range_option(entity, "minimum");
        const std::optional<std::string> maximum =
            range_option(entity, "maximum");
        if (!minimum || !maximum || !row.minimum_expression
            || !row.maximum_expression) {
            throw std::runtime_error(
                "numeric GUC '" + row.name
                + "' must retain explicit parity bounds");
        }
        require_equivalent(
            *minimum, *row.minimum_expression, "minimum");
        require_equivalent(
            *maximum, *row.maximum_expression, "maximum");
    }
}

[[nodiscard]] inline std::vector<collected_guc_enum_value>
collect_enum_values(
    const collected_program& program,
    const collected_entity& entity,
    const ledger_row& row,
    std::string& enum_cpp_name) {
    const std::string enum_leaf = enum_type_leaf(entity.cpp_return_type);
    const parsed_enum parsed =
        parse_enum(program, entity, enum_leaf);
    enum_cpp_name = parsed.qualified_name;
    const auto& declarations = parsed.values;
    const auto exceptions =
        guc_value_exceptions(program, enum_cpp_name);
    // Transitional source-only contract fixtures have no compiler profile and
    // therefore cannot expose Clang's post-preprocessing enumerator set. Keep
    // those fixtures useful by admitting only lexically unconditional values;
    // profiled generation must always use the exact compiled declarations.
    const bool source_only_fixture =
        program.compiler_profile.empty();
    const auto compiled_enumerator =
        [&program, source_only_fixture](
            const parsed_enumerator& value) {
            if (source_only_fixture) {
                return canonical_feature_predicate(
                           value.active_condition)
                    == "true";
            }
            for (const clang_structural::translation_unit& unit :
                 program.translation_units) {
                if (std::ranges::any_of(
                        unit.declarations,
                        [&value](
                            const clang_structural::declaration_record&
                                declaration) {
                            return declaration.kind
                                    == clang_structural::
                                        declaration_kind::enumerator
                                && declaration.qualified_name
                                    == value.qualified_name;
                        })) {
                    return true;
                }
            }
            return false;
        };

    if (!row.default_semantic) {
        throw std::runtime_error(
            "enum GUC '" + row.name
            + "' has no semantic default in the parity ledger");
    }
    const std::string authored_default =
        enum_default_leaf(entity.initializer_expression);
    const bool feature_selected_compression_default =
        authored_default == "default_compression_type"
        && *row.default_semantic == "best_compiled_algorithm";
    if (authored_default != *row.default_semantic
        && !feature_selected_compression_default) {
        throw std::runtime_error(
            "enum GUC '" + row.name + "' default is '"
            + authored_default + "', expected '" + *row.default_semantic
            + "'");
    }

    std::set<std::string, std::less<>> accounted;
    std::vector<collected_guc_enum_value> result;
    result.reserve(row.enum_values.size());
    for (const ledger_enum_value& expected : row.enum_values) {
        if (expected.alias_of) {
            const auto exception = exceptions.find(*expected.alias_of);
            if (exception == exceptions.end()
                || exception->second.alias != expected.name
                || exception->second.alias_hidden != expected.hidden) {
                std::string detail =
                    exception == exceptions.end()
                    ? "no matching exception owner"
                    : "declared alias '"
                        + exception->second.alias.value_or("<none>")
                        + "', hidden="
                        + (exception->second.alias_hidden
                            ? "true"
                            : "false");
                throw std::runtime_error(
                    "enum GUC '" + row.name + "' alias '"
                    + expected.name + "' is not declared as a guc_value "
                      "exception (" + detail + "; enum owner '"
                    + enum_cpp_name + "')");
            }
            const auto canonical = declarations.find(*expected.alias_of);
            if (canonical == declarations.end()) {
                throw std::runtime_error(
                    "enum GUC '" + row.name + "' alias target '"
                    + *expected.alias_of + "' does not exist");
            }
            const std::string& actual_wire =
                canonical->second.initializer_text;
            const bool planner_debug_alias =
                row.name == "bioseq.planner_debug_level"
                && *expected.alias_of == "debug2"
                && equivalent_expression(actual_wire, "13")
                && equivalent_expression(
                    expected.wire_expression, "DEBUG2");
            if (!planner_debug_alias
                && !equivalent_expression(
                    actual_wire, expected.wire_expression)) {
                throw std::runtime_error(
                    "enum GUC '" + row.name + "' alias '"
                    + expected.name
                    + "' wire value differs from declared target '"
                    + *expected.alias_of + "'");
            }
            const std::string actual_feature_predicate =
                effective_feature_predicate(
                    entity.active_condition,
                    canonical->second.active_condition);
            if (actual_feature_predicate
                != canonical_feature_predicate(
                    expected.feature_predicate)) {
                throw std::runtime_error(
                    "enum GUC '" + row.name + "' alias '"
                    + expected.name
                    + "' feature gating differs from the parity ledger");
            }
            result.push_back(collected_guc_enum_value{
                .name = expected.name,
                .cpp_name = canonical->second.qualified_name,
                .wire_expression = canonical->second.initializer_text,
                .active_condition = canonical->second.active_condition,
                .alias_of = expected.alias_of,
                .hidden = expected.hidden,
                .compiled = compiled_enumerator(canonical->second),
            });
            continue;
        }
        const auto declaration = declarations.find(expected.name);
        if (declaration == declarations.end()) {
            throw std::runtime_error(
                "enum GUC '" + row.name + "' value '"
                + expected.name + "' does not exist");
        }
        accounted.insert(expected.name);
        const std::string actual_wire =
            declaration->second.initializer_text;
        const std::string actual_feature_predicate =
            effective_feature_predicate(
                entity.active_condition,
                declaration->second.active_condition);
        if (actual_feature_predicate
            != canonical_feature_predicate(
                expected.feature_predicate)) {
            throw std::runtime_error(
                "enum GUC '" + row.name + "' value '"
                + expected.name
                + "' feature gating differs from the parity ledger");
        }
        const bool planner_constant =
            row.name == "bioseq.planner_debug_level"
            && ((expected.name == "debug5"
                    && equivalent_expression(actual_wire, "10"))
                || (expected.name == "debug4"
                    && equivalent_expression(actual_wire, "11"))
                || (expected.name == "debug3"
                    && equivalent_expression(actual_wire, "12"))
                || (expected.name == "debug2"
                    && equivalent_expression(actual_wire, "13"))
                || (expected.name == "debug1"
                    && equivalent_expression(actual_wire, "14"))
                || (expected.name == "log"
                    && equivalent_expression(actual_wire, "15"))
                || (expected.name == "info"
                    && equivalent_expression(actual_wire, "17"))
                || (expected.name == "notice"
                    && equivalent_expression(actual_wire, "18"))
                || (expected.name == "warning"
                    && equivalent_expression(actual_wire, "19")));
        if (!planner_constant
            && !equivalent_expression(
                actual_wire, expected.wire_expression)) {
            throw std::runtime_error(
                "enum GUC '" + row.name + "' value '"
                + expected.name + "' changed wire value");
        }
        result.push_back(collected_guc_enum_value{
            .name = expected.name,
            .cpp_name = declaration->second.qualified_name,
            .wire_expression = actual_wire,
            .active_condition =
                declaration->second.active_condition,
            .hidden = expected.hidden,
            .compiled = compiled_enumerator(declaration->second),
        });
    }
    for (const ledger_omission& omission : row.omissions) {
        const auto declaration = declarations.find(omission.name);
        const auto exception = exceptions.find(omission.name);
        if (declaration == declarations.end()
            || exception == exceptions.end()
            || !exception->second.omit) {
            throw std::runtime_error(
                "enum GUC '" + row.name + "' omission '"
                + omission.name
                + "' is not declared as a guc_value exception");
        }
        if (!equivalent_expression(
                declaration->second.initializer_text,
                omission.wire_expression)) {
            throw std::runtime_error(
                "enum GUC '" + row.name + "' omitted value '"
                + omission.name + "' changed wire value");
        }
        accounted.insert(omission.name);
    }
    for (const auto& [name, declaration] : declarations) {
        static_cast<void>(declaration);
        if (!accounted.contains(name)) {
            throw std::runtime_error(
                "enum GUC '" + row.name + "' has unaccounted value '"
                + name + "'");
        }
    }
    return result;
}

[[nodiscard]] inline std::string_view trim_expression(
        std::string_view value) noexcept {
    while (!value.empty()
           && (value.front() == ' ' || value.front() == '\t'
               || value.front() == '\n' || value.front() == '\r')) {
        value.remove_prefix(1);
    }
    while (!value.empty()
           && (value.back() == ' ' || value.back() == '\t'
               || value.back() == '\n' || value.back() == '\r')) {
        value.remove_suffix(1);
    }
    return value;
}

class integer_expression_parser final {
public:
    explicit integer_expression_parser(std::string_view input)
        : input_{input} {}

    [[nodiscard]] std::int64_t parse() {
        const std::int64_t result = parse_bitwise_or();
        skip_space();
        if (cursor_ != input_.size()) {
            fail("trailing token");
        }
        return result;
    }

private:
    [[noreturn]] void fail(std::string_view reason) const {
        throw std::runtime_error(
            "cannot resolve generated integer GUC expression '"
            + std::string{input_} + "' at byte "
            + std::to_string(cursor_) + ": " + std::string{reason});
    }

    void skip_space() noexcept {
        while (cursor_ < input_.size()
               && (input_[cursor_] == ' '
                   || input_[cursor_] == '\t'
                   || input_[cursor_] == '\n'
                   || input_[cursor_] == '\r')) {
            ++cursor_;
        }
    }

    [[nodiscard]] bool consume(std::string_view token) noexcept {
        skip_space();
        if (!input_.substr(cursor_).starts_with(token)) {
            return false;
        }
        cursor_ += token.size();
        return true;
    }

    [[nodiscard]] std::int64_t parse_primary() {
        skip_space();
        if (consume("(")) {
            const std::int64_t value = parse_bitwise_or();
            if (!consume(")")) fail("missing closing parenthesis");
            return value;
        }
        const std::size_t begin = cursor_;
        while (cursor_ < input_.size()
               && ((input_[cursor_] >= '0'
                    && input_[cursor_] <= '9')
                   || input_[cursor_] == '\'')) {
            ++cursor_;
        }
        if (begin == cursor_) fail("expected an integer literal");
        std::string digits;
        digits.reserve(cursor_ - begin);
        for (const char item : input_.substr(begin, cursor_ - begin)) {
            if (item != '\'') digits.push_back(item);
        }
        while (cursor_ < input_.size()
               && (input_[cursor_] == 'u'
                   || input_[cursor_] == 'U'
                   || input_[cursor_] == 'l'
                   || input_[cursor_] == 'L')) {
            ++cursor_;
        }
        std::int64_t value = 0;
        const auto parsed = std::from_chars(
            digits.data(), digits.data() + digits.size(), value);
        if (parsed.ec != std::errc{}
            || parsed.ptr != digits.data() + digits.size()) {
            fail("invalid or overflowing integer literal");
        }
        return value;
    }

    [[nodiscard]] std::int64_t parse_unary() {
        if (consume("+")) return parse_unary();
        if (consume("-")) return -parse_unary();
        if (consume("~")) return ~parse_unary();
        return parse_primary();
    }

    [[nodiscard]] std::int64_t parse_multiplicative() {
        std::int64_t result = parse_unary();
        while (true) {
            if (consume("*")) {
                result *= parse_unary();
            } else if (consume("/")) {
                const std::int64_t divisor = parse_unary();
                if (divisor == 0) fail("division by zero");
                result /= divisor;
            } else if (consume("%")) {
                const std::int64_t divisor = parse_unary();
                if (divisor == 0) fail("remainder by zero");
                result %= divisor;
            } else {
                return result;
            }
        }
    }

    [[nodiscard]] std::int64_t parse_additive() {
        std::int64_t result = parse_multiplicative();
        while (true) {
            if (consume("+")) result += parse_multiplicative();
            else if (consume("-")) result -= parse_multiplicative();
            else return result;
        }
    }

    [[nodiscard]] std::int64_t parse_shift() {
        std::int64_t result = parse_additive();
        while (true) {
            if (consume("<<")) {
                const std::int64_t amount = parse_additive();
                if (amount < 0 || amount >= 63) {
                    fail("invalid left-shift amount");
                }
                result <<= amount;
            } else if (consume(">>")) {
                const std::int64_t amount = parse_additive();
                if (amount < 0 || amount >= 63) {
                    fail("invalid right-shift amount");
                }
                result >>= amount;
            } else {
                return result;
            }
        }
    }

    [[nodiscard]] std::int64_t parse_bitwise_and() {
        std::int64_t result = parse_shift();
        while (consume("&")) result &= parse_shift();
        return result;
    }

    [[nodiscard]] std::int64_t parse_bitwise_xor() {
        std::int64_t result = parse_bitwise_and();
        while (consume("^")) result ^= parse_bitwise_and();
        return result;
    }

    [[nodiscard]] std::int64_t parse_bitwise_or() {
        std::int64_t result = parse_bitwise_xor();
        while (consume("|")) result |= parse_bitwise_xor();
        return result;
    }

    std::string_view input_;
    std::size_t cursor_ = 0;
};

[[nodiscard]] inline bool bare_numeric_identifier(
        std::string_view value) noexcept {
    value = trim_expression(value);
    if (value.empty()
        || !((value.front() >= 'a' && value.front() <= 'z')
             || (value.front() >= 'A' && value.front() <= 'Z')
             || value.front() == '_')) {
        return false;
    }
    return std::ranges::all_of(
        value.substr(1),
        [](char item) {
            return (item >= 'a' && item <= 'z')
                || (item >= 'A' && item <= 'Z')
                || (item >= '0' && item <= '9') || item == '_';
        });
}

[[nodiscard]] inline std::optional<std::string>
source_only_integer_initializer(
    const clang_structural::translation_unit& unit,
    std::string_view identifier) {
    const std::string source = read_file(unit.source_path);
    const auto& tokens = unit.tokens;
    std::optional<std::string> result;
    for (std::size_t index = 0; index + 2 < tokens.size(); ++index) {
        if (tokens[index].spelling != identifier
            || tokens[index + 1].spelling != "="
            || canonical_feature_predicate(condition_at(
                   source,
                   tokens[index].range.begin.byte_offset))
                != "true") {
            continue;
        }

        bool constexpr_declaration = false;
        for (std::size_t cursor = index; cursor > 0; --cursor) {
            const std::string& prior = tokens[cursor - 1].spelling;
            if (prior == ";" || prior == "{"
                || prior == "}") {
                break;
            }
            if (prior == "constexpr") {
                constexpr_declaration = true;
            }
        }
        if (!constexpr_declaration) continue;

        std::size_t terminator = tokens.size();
        int parentheses = 0;
        int brackets = 0;
        int braces = 0;
        for (std::size_t cursor = index + 2;
             cursor < tokens.size();
             ++cursor) {
            const std::string& spelling = tokens[cursor].spelling;
            if (spelling == "(") ++parentheses;
            else if (spelling == ")") --parentheses;
            else if (spelling == "[") ++brackets;
            else if (spelling == "]") --brackets;
            else if (spelling == "{") ++braces;
            else if (spelling == "}") --braces;
            else if (spelling == ";" && parentheses == 0
                     && brackets == 0 && braces == 0) {
                terminator = cursor;
                break;
            }
            if (parentheses < 0 || brackets < 0 || braces < 0) {
                throw std::runtime_error(
                    "malformed source-only integer GUC constant '"
                    + std::string{identifier} + "'");
            }
        }
        if (terminator == tokens.size()
            || terminator == index + 2) {
            throw std::runtime_error(
                "cannot resolve source-only integer GUC constant '"
                + std::string{identifier} + "'");
        }
        const std::size_t begin =
            tokens[index + 2].range.begin.byte_offset;
        const std::size_t end =
            tokens[terminator - 1].range.end.byte_offset;
        const std::string initializer =
            source.substr(begin, end - begin);
        if (result) {
            throw std::runtime_error(
                "ambiguous source-only integer GUC constant '"
                + std::string{identifier} + "'");
        }
        result = initializer;
    }
    return result;
}

[[nodiscard]] inline std::string resolve_integer_expression(
    const collected_program& program,
    const collected_entity& owner,
    std::string_view expression,
    std::size_t depth = 0) {
    if (depth > 16) {
        throw std::runtime_error(
            "generated integer GUC expression recursion is too deep");
    }
    expression = trim_expression(expression);
    if (expression == "INT_MAX") {
        return std::to_string(std::numeric_limits<int>::max());
    }
    if (expression == "INT_MIN") {
        return std::to_string(std::numeric_limits<int>::min());
    }
    if (bare_numeric_identifier(expression)) {
        const auto unit = std::ranges::find(
            program.translation_units,
            owner.source_path,
            &clang_structural::translation_unit::source_path);
        if (unit == program.translation_units.end()) {
            throw std::runtime_error(
                "cannot resolve generated integer GUC owner source");
        }
        const auto declaration = std::ranges::find_if(
            unit->declarations,
            [expression](
                const clang_structural::declaration_record& candidate) {
                return candidate.kind
                        == clang_structural::declaration_kind::variable
                    && candidate.name == expression
                    && !candidate.initializer_text.empty();
            });
        if (declaration == unit->declarations.end()) {
            if (program.compiler_profile.empty()) {
                if (const std::optional<std::string> initializer =
                        source_only_integer_initializer(
                            *unit, expression)) {
                    return resolve_integer_expression(
                        program,
                        owner,
                        *initializer,
                        depth + 1);
                }
            }
            throw std::runtime_error(
                "cannot resolve generated integer GUC identifier '"
                + std::string{expression} + "' in "
                + owner.source_path.string());
        }
        return resolve_integer_expression(
            program,
            owner,
            declaration->initializer_text,
            depth + 1);
    }
    return std::to_string(
        integer_expression_parser{expression}.parse());
}

[[nodiscard]] inline std::string resolve_real_expression(
        std::string_view expression) {
    expression = trim_expression(expression);
    std::string normalized;
    normalized.reserve(expression.size());
    for (const char item : expression) {
        if (item != '\'') normalized.push_back(item);
    }
    char* end = nullptr;
    const double value = std::strtod(normalized.c_str(), &end);
    if (end == normalized.c_str() || *end != '\0') {
        throw std::runtime_error(
            "cannot resolve generated real GUC expression '"
            + normalized + "'");
    }
    std::array<char, 128> buffer{};
    const auto rendered = std::to_chars(
        buffer.data(),
        buffer.data() + buffer.size(),
        value,
        std::chars_format::general);
    if (rendered.ec != std::errc{}) {
        throw std::runtime_error(
            "cannot render generated real GUC expression '"
            + normalized + "'");
    }
    return std::string{
        buffer.data(),
        static_cast<std::size_t>(rendered.ptr - buffer.data())};
}

[[nodiscard]] inline std::int64_t raw_guc_flags(
        const collected_guc& guc) {
    std::int64_t result = 0;
    for (const std::string& flag : guc.flags) {
        if (flag == "no_show_all") result |= 0x000004;
        else if (flag == "not_in_sample") result |= 0x000080;
        else {
            throw std::runtime_error(
                "cannot resolve generated raw GUC flag '" + flag + "'");
        }
    }
    if (guc.unit) {
        if (*guc.unit == "kb") result |= 0x01000000;
        else if (*guc.unit == "blocks") result |= 0x02000000;
        else if (*guc.unit == "mb") result |= 0x04000000;
        else if (*guc.unit == "ms") result |= 0x10000000;
        else if (*guc.unit == "s") result |= 0x20000000;
        else if (*guc.unit == "min") result |= 0x30000000;
        else {
            throw std::runtime_error(
                "cannot resolve generated raw GUC unit '" + *guc.unit
                + "'");
        }
    }
    return result;
}

[[nodiscard]] inline collected_guc_live_authority
make_generated_live_authority(
    const collected_program& program,
    const collected_entity& entity,
    const collected_guc& guc) {
    collected_guc_live_authority result{
        .vartype = std::string{guc_kind_name(guc.kind)},
        .context =
            guc.context == "sighup"
            ? "sighup"
            : guc.context == "superuser_set"
            ? "superuser"
            : guc.context == "user_set" ? "user" : std::string{},
        .unit =
            guc.unit
            ? std::optional<std::string>{
                  *guc.unit == "mb"
                      ? std::string{"MB"}
                      : *guc.unit}
            : std::nullopt,
        .short_description = guc.short_description,
        .extra_description =
            guc.long_description.empty()
            ? std::nullopt
            : std::optional<std::string>{guc.long_description},
        .raw_flags = raw_guc_flags(guc),
        .visible_in_show_all =
            !std::ranges::contains(guc.flags, "no_show_all"),
        .included_in_sample =
            !std::ranges::contains(guc.flags, "not_in_sample"),
        .hook_presence = {
            guc.authority.hooks[0].has_value(),
            guc.authority.hooks[1].has_value(),
            guc.authority.hooks[2].has_value(),
        },
    };
    if (result.context.empty()) {
        throw std::runtime_error(
            "cannot resolve generated live context for GUC '"
            + guc.name + "'");
    }
    switch (guc.kind) {
        case guc_kind::boolean:
            if (guc.default_expression == "true") {
                result.boot_value = "on";
            } else if (guc.default_expression == "false") {
                result.boot_value = "off";
            } else {
                throw std::runtime_error(
                    "cannot resolve generated boolean boot value for GUC '"
                    + guc.name + "'");
            }
            break;
        case guc_kind::integer:
            result.boot_value = resolve_integer_expression(
                program, entity, guc.default_expression);
            result.minimum = resolve_integer_expression(
                program, entity, *guc.minimum_expression);
            result.maximum = resolve_integer_expression(
                program, entity, *guc.maximum_expression);
            break;
        case guc_kind::real:
            result.boot_value =
                resolve_real_expression(guc.default_expression);
            result.minimum =
                resolve_real_expression(*guc.minimum_expression);
            result.maximum =
                resolve_real_expression(*guc.maximum_expression);
            break;
        case guc_kind::string: {
            std::string_view expression =
                trim_expression(guc.default_expression);
            if (expression == "nullptr" || expression == "NULL") {
                result.boot_value.clear();
            } else if (
                expression.size() >= 2
                && expression.front() == '"'
                && expression.back() == '"') {
                result.boot_value = std::string{
                    expression.substr(1, expression.size() - 2)};
            } else {
                throw std::runtime_error(
                    "cannot resolve generated string boot value for GUC '"
                    + guc.name + "'");
            }
            break;
        }
        case guc_kind::enumeration:
            for (const collected_guc_enum_value& value :
                 guc.enum_values) {
                if (value.compiled && !value.hidden) {
                    result.visible_enum_values.push_back(value.name);
                }
            }
            if (result.visible_enum_values.empty()
                || !guc.authority.default_semantic) {
                throw std::runtime_error(
                    "cannot resolve generated enum values for GUC '"
                    + guc.name + "'");
            }
            result.boot_value =
                *guc.authority.default_semantic
                    == "best_compiled_algorithm"
                ? result.visible_enum_values.back()
                : *guc.authority.default_semantic;
            if (!std::ranges::contains(
                    result.visible_enum_values,
                    result.boot_value)) {
                throw std::runtime_error(
                    "generated enum boot value is not visible for GUC '"
                    + guc.name + "'");
            }
            break;
    }
    result.reset_value = result.boot_value;
    result.current_value = result.boot_value;
    return result;
}

}  // namespace guc_detail

[[nodiscard]] inline guc_inventory collect_guc_inventory(
    const collected_program& program,
    const std::filesystem::path& ledger_path,
    guc_profile profile) {
    const std::string ledger_content = read_file(ledger_path);
    const guc_detail::parsed_ledger ledger =
        guc_detail::parse_ledger(ledger_content);
    const std::string profile_spelling{guc_profile_name(profile)};
    const auto expected_profile_count =
        ledger.profile_counts.find(profile_spelling);
    if (expected_profile_count == ledger.profile_counts.end()) {
        throw std::runtime_error(
            "GUC parity ledger has no profile '" + profile_spelling + "'");
    }

    std::map<std::string, const guc_detail::ledger_row*, std::less<>>
        selected_ledger;
    for (const guc_detail::ledger_row& row : ledger.rows) {
        if (!std::ranges::contains(row.profiles, profile_spelling)) {
            continue;
        }
        selected_ledger.emplace(row.name, &row);
    }
    if (selected_ledger.size() != expected_profile_count->second) {
        throw std::runtime_error(
            "GUC parity ledger profile '" + profile_spelling
            + "' selects " + std::to_string(selected_ledger.size())
            + " rows; expected "
            + std::to_string(expected_profile_count->second));
    }

    std::map<std::string, const collected_entity*, std::less<>>
        selected_annotated;
    for (const collected_entity& entity : program.entities) {
        if (entity.kind != entity_kind::guc) continue;
        if (!guc_detail::condition_enabled_for_profile(
                entity.active_condition, profile)) {
            continue;
        }
        const std::string name =
            guc_detail::natural_guc_name(entity.cpp_qualified_name);
        if (!selected_annotated.emplace(name, &entity).second) {
            throw std::runtime_error(
                "duplicate annotated GUC '" + name
                + "' in profile '" + profile_spelling + "'");
        }
    }
    if (selected_annotated.size() != selected_ledger.size()) {
        throw std::runtime_error(
            "annotated GUC profile '" + profile_spelling + "' has "
            + std::to_string(selected_annotated.size())
            + " rows; parity ledger selects "
            + std::to_string(selected_ledger.size()));
    }
    for (const auto& [name, entity] : selected_annotated) {
        static_cast<void>(entity);
        if (!selected_ledger.contains(name)) {
            throw std::runtime_error(
                "annotated GUC '" + name + "' is not selected by parity "
                "ledger profile '" + profile_spelling + "'");
        }
    }
    for (const auto& [name, row] : selected_ledger) {
        static_cast<void>(row);
        if (!selected_annotated.contains(name)) {
            throw std::runtime_error(
                "GUC parity ledger row '" + name
                + "' has no annotated owner in profile '"
                + profile_spelling + "'");
        }
    }

    guc_inventory result{
        .profile = profile,
        .ledger_path = ledger_path,
        .ledger_sha256 = sha256_hex(ledger_content),
    };
    for (const guc_detail::ledger_row& row : ledger.rows) {
        if (!selected_ledger.contains(row.name)) continue;
        const auto found = selected_annotated.find(row.name);
        const collected_entity& entity = *found->second;
        guc_detail::validate_documentation(entity);
        const std::string expected_source_module =
            guc_detail::candidate_source_module(row.group);
        const std::string expected_cpp_storage =
            guc_detail::candidate_cpp_storage(
                row.group, row.member);
        if (entity.source_module != expected_source_module
            || entity.source_path.generic_string().contains(
                "/backend/pg/")) {
            throw std::runtime_error(
                "GUC '" + row.name
                + "' must be owned by candidate module '"
                + expected_source_module
                + "' outside the PostgreSQL adapter");
        }
        if (entity.cpp_qualified_name != expected_cpp_storage) {
            throw std::runtime_error(
                "GUC '" + row.name + "' backing cell is '"
                + entity.cpp_qualified_name + "', expected '"
                + expected_cpp_storage + "'");
        }
        if (entity.name != row.member) {
            throw std::runtime_error(
                "GUC '" + row.name + "' member differs from ledger '"
                + row.member + "'");
        }
        const guc_kind kind =
            guc_detail::kind_from_entity(entity);
        if (guc_kind_name(kind) != row.kind) {
            throw std::runtime_error(
                "GUC '" + row.name + "' kind differs from ledger");
        }
        if (guc_detail::storage_kind_from_entity(entity)
            != row.storage_kind) {
            throw std::runtime_error(
                "GUC '" + row.name
                + "' candidate storage ABI differs from ledger");
        }
        if (guc_detail::context_name(entity) != row.context) {
            throw std::runtime_error(
                "GUC '" + row.name + "' context differs from ledger");
        }
        std::vector<std::string> flags =
            guc_detail::guc_flags(entity);
        std::ranges::sort(flags);
        std::vector<std::string> expected_flags = row.flags;
        std::ranges::sort(expected_flags);
        if (flags != expected_flags
            || guc_detail::guc_unit(entity) != row.unit) {
            throw std::runtime_error(
                "GUC '" + row.name + "' flags or unit differ from ledger");
        }
        const std::string actual_feature_predicate =
            guc_detail::canonical_feature_predicate(
                entity.active_condition);
        const std::string expected_feature_predicate =
            guc_detail::canonical_feature_predicate(
                row.feature_predicate);
        if (actual_feature_predicate != expected_feature_predicate) {
            throw std::runtime_error(
                "GUC '" + row.name
                + "' feature gating differs from ledger: candidate '"
                + actual_feature_predicate + "', ledger '"
                + expected_feature_predicate + "'");
        }
        if (kind != guc_kind::enumeration) {
            guc_detail::validate_scalar_parity(entity, row, kind);
        }
        collected_guc guc{
            .registration_ordinal = row.ordinal,
            .name = row.name,
            .owner_key = row.group == "bioseq"
                ? "bioseq"
                : row.group.substr(row.group.find('.') + 1),
            .member = row.member,
            .kind = kind,
            .cpp_qualified_name = entity.cpp_qualified_name,
            .source_module = entity.source_module,
            .source_path = entity.source_path,
            .source_sha256 = entity.source_sha256,
            .active_condition = entity.active_condition,
            .default_expression = entity.initializer_expression,
            .minimum_expression =
                guc_detail::range_option(entity, "minimum"),
            .maximum_expression =
                guc_detail::range_option(entity, "maximum"),
            .context = row.context,
            .flags = std::move(flags),
            .unit = row.unit,
            .short_description = entity.documentation.brief,
            .long_description = entity.documentation.details,
            .reset_plan_cache =
                guc_detail::resets_plan_cache(entity),
            .authored_assign_hook = row.hooks[1],
            .authority = collected_guc_authority{
                .source_owner = row.source_owner,
                .group = row.group,
                .storage_kind = row.storage_kind,
                .legacy_storage = row.legacy_storage,
                .default_expression = row.default_expression,
                .minimum_expression = row.minimum_expression,
                .maximum_expression = row.maximum_expression,
                .context = row.context,
                .flags = row.flags,
                .unit = row.unit,
                .hooks = row.hooks,
                .feature_predicate = row.feature_predicate,
                .profiles = row.profiles,
                .default_semantic = row.default_semantic,
            },
        };
        if (guc.reset_plan_cache) {
            guc.authority.hooks[1] =
                "assign_" + guc.member + "_and_reset_plan_cache";
        }
        guc.authority.enum_values.reserve(row.enum_values.size());
        for (const guc_detail::ledger_enum_value& value :
             row.enum_values) {
            guc.authority.enum_values.push_back(
                collected_guc_authority_enum_value{
                    .name = value.name,
                    .wire_expression = value.wire_expression,
                    .hidden = value.hidden,
                    .feature_predicate = value.feature_predicate,
                    .alias_of = value.alias_of,
                });
        }
        guc.authority.omissions.reserve(row.omissions.size());
        for (const guc_detail::ledger_omission& omission :
             row.omissions) {
            guc.authority.omissions.push_back(
                collected_guc_authority_omission{
                    .name = omission.name,
                    .wire_expression = omission.wire_expression,
                });
        }
        if (kind == guc_kind::enumeration) {
            guc.enum_values = guc_detail::collect_enum_values(
                program, entity, row, guc.enum_cpp_name);
            const std::string default_leaf =
                guc_detail::enum_default_leaf(
                    entity.initializer_expression);
            if (default_leaf == "default_compression_type") {
                const std::size_t owner =
                    entity.cpp_qualified_name.find("::settings::");
                if (owner == std::string::npos) {
                    throw std::runtime_error(
                        "feature-selected enum default lacks a settings owner");
                }
                guc.default_expression =
                    entity.cpp_qualified_name.substr(0, owner)
                    + "::" + default_leaf;
            } else {
                guc.default_expression =
                    guc.enum_cpp_name + "::" + default_leaf;
            }
        }
        guc.generated_live_authority =
            guc_detail::make_generated_live_authority(
                program, entity, guc);
        result.gucs.push_back(std::move(guc));
    }
    if (result.gucs.size() != expected_profile_count->second) {
        throw std::runtime_error(
            "generated GUC profile '" + profile_spelling + "' has "
            + std::to_string(result.gucs.size()) + " rows; expected "
            + std::to_string(expected_profile_count->second));
    }
    return result;
}

}  // namespace bioseq::codegen::sql
