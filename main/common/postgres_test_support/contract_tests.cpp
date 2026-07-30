#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

import bioseq.test.postgres;

#if !defined(BIOSEQ_POSTGRES_TEST_PROCESS_CONTRACT_CHILD_PATH)
#error "BIOSEQ_POSTGRES_TEST_PROCESS_CONTRACT_CHILD_PATH must name the process contract child"
#endif

using namespace std::chrono_literals;
using namespace bioseq::postgres_test;

static_assert(requires(bioseq::libpq::connection& connection) {
    connection.exec(
        "BEGIN", 1s, bioseq::libpq::transaction_policy::allow_open);
    connection.current_transaction_state();
    connection.exec_single_row_observed("SELECT 1", 1s);
    connection.set_execution_observer(nullptr, "contract");
    connection.execution_observer_ptr();
});
static_assert(std::is_same_v<
    decltype(std::declval<bioseq::libpq::connection&>()
                 .exec_single_row_observed("SELECT 1", 1s)),
    bioseq::libpq::single_row_observed_result>);

namespace {

struct report_state_reset {
    ~report_state_reset() noexcept {
        clear_report_case_context();
        try {
            clear_process_sql_transcript();
        } catch (...) {
        }
        clear_cleanup_observations();
    }
};

class scoped_environment_override {
public:
    scoped_environment_override(const char* name, const char* value)
        : name_(name) {
        if (const char* current = std::getenv(name)) {
            original_ = current;
        }
        if (::setenv(name, value, 1) != 0) {
            throw std::runtime_error(
                "cannot configure process-environment contract");
        }
    }

    scoped_environment_override(const scoped_environment_override&) = delete;
    scoped_environment_override& operator=(
        const scoped_environment_override&) = delete;

    ~scoped_environment_override() noexcept {
        if (original_) {
            static_cast<void>(
                ::setenv(name_.c_str(), original_->c_str(), 1));
        } else {
            static_cast<void>(::unsetenv(name_.c_str()));
        }
    }

private:
    std::string name_;
    std::optional<std::string> original_;
};

class temporary_resource_journal_tree {
public:
    temporary_resource_journal_tree()
        : root_(
              std::filesystem::temp_directory_path()
              / ("bioseq-resource-journal-contract-"
                 + std::to_string(::getpid()) + "-"
                 + std::to_string(
                     std::chrono::steady_clock::now()
                         .time_since_epoch()
                         .count()))) {
        if (!std::filesystem::create_directories(data_directory())) {
            throw std::runtime_error(
                "cannot create resource-journal contract tree");
        }
    }

    temporary_resource_journal_tree(
        const temporary_resource_journal_tree&) = delete;
    temporary_resource_journal_tree& operator=(
        const temporary_resource_journal_tree&) = delete;

    ~temporary_resource_journal_tree() noexcept {
        set_supervision_journal_fault_for_testing(
            supervision_journal_fault::none);
        disable_supervision_state();
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    [[nodiscard]] std::filesystem::path owned_root() const {
        return root_ / "owned";
    }

    [[nodiscard]] std::filesystem::path data_directory() const {
        return owned_root() / "data";
    }

    [[nodiscard]] std::filesystem::path state_directory() const {
        return root_ / "state";
    }

private:
    std::filesystem::path root_;
};

class temporary_preinstalled_stage_tree {
public:
    temporary_preinstalled_stage_tree()
        : root_(
              std::filesystem::temp_directory_path()
              / ("bioseq-stage-contract-"
                 + std::to_string(::getpid()) + "-"
                 + std::to_string(
                     std::chrono::steady_clock::now()
                         .time_since_epoch()
                         .count()))),
          stage_root_(root_ / "stage"),
          case_root_(
              root_ / ("bioseq-pg-test-contract-"
                       + std::to_string(::getpid()))),
          source_root_(root_ / "source"),
          bin_directory_(stage_root_ / "pg" / "bin"),
          shared_directory_(stage_root_ / "pg" / "share"),
          library_directory_(stage_root_ / "pg" / "lib"),
          control_file_(source_root_ / "bioseq_cache.control"),
          install_sql_(source_root_ / "bioseq_cache--1.0.sql"),
          normal_dso_(source_root_ / "bioseq_cache.so"),
          override_dso_(source_root_ / "peer.so"),
          bioseq_control_(source_root_ / "bioseq.control"),
          bioseq_install_sql_(source_root_ / "bioseq--1.0.sql"),
          bioseq_dso_(source_root_ / "bioseq.so"),
          manifest_(root_ / "stage-manifest.json") {
        std::filesystem::create_directories(bin_directory_);
        std::filesystem::create_directories(
            shared_directory_ / "extension");
        std::filesystem::create_directories(library_directory_);
        std::filesystem::create_directories(source_root_);
        std::filesystem::create_directories(case_root_);
        write(case_root_ / ".bioseq-private-cluster",
              case_root_.filename().string() + "\n");
        for (const std::string_view tool :
             {"postgres", "initdb", "pg_ctl",
              "createdb", "dropdb", "psql"}) {
            write(bin_directory_ / tool, std::string(tool) + "\n");
        }
        for (const std::string_view sentinel :
             {"postgres.bki",
              "postgresql.conf.sample",
              "pg_hba.conf.sample",
              "pg_ident.conf.sample",
              "information_schema.sql",
              "system_functions.sql",
              "system_views.sql",
              "system_constraints.sql",
              "snowball_create.sql"}) {
            write(
                shared_directory_ / sentinel,
                std::string(sentinel) + " contract\n");
        }
        for (const std::string_view directory :
             {"timezone", "timezonesets", "tsearch_data"}) {
            std::filesystem::create_directories(
                shared_directory_ / directory);
        }
        write(
            library_directory_ / plpgsql_filename(),
            "plpgsql contract module\n");
        write(
            shared_directory_ / "contract-runtime.dat",
            "runtime contract payload\n");
        write(
            control_file_,
            "comment = 'stage contract'\n"
            "default_version = '1.0'\n"
            "module_pathname = '$libdir/bioseq_cache'\n");
        write(install_sql_, "SELECT 1;\n");
        write(normal_dso_, "normal cache DSO\n");
        write(override_dso_, "provider-peer DSO\n");
        write(
            bioseq_control_,
            "comment = 'Bioseq stage contract'\n"
            "default_version = '1.0'\n"
            "module_pathname = '$libdir/bioseq'\n");
        write(bioseq_install_sql_, "SELECT 2;\n");
        write(bioseq_dso_, "normal Bioseq DSO\n");
        std::filesystem::copy_file(
            control_file_,
            shared_directory_ / "extension" / control_file_.filename());
        std::filesystem::copy_file(
            install_sql_,
            shared_directory_ / "extension" / install_sql_.filename());
        std::filesystem::copy_file(
            normal_dso_,
            library_directory_ / normal_dso_.filename());
        std::filesystem::copy_file(
            bioseq_control_,
            shared_directory_ / "extension"
                / bioseq_control_.filename());
        std::filesystem::copy_file(
            bioseq_install_sql_,
            shared_directory_ / "extension"
                / bioseq_install_sql_.filename());
        std::filesystem::copy_file(
            bioseq_dso_,
            library_directory_ / bioseq_dso_.filename());
        write_manifest();
    }

    temporary_preinstalled_stage_tree(
        const temporary_preinstalled_stage_tree&) = delete;
    temporary_preinstalled_stage_tree& operator=(
        const temporary_preinstalled_stage_tree&) = delete;

    ~temporary_preinstalled_stage_tree() noexcept {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    [[nodiscard]] extension_artifact normal_artifact() const {
        return {
            .name = "bioseq_cache",
            .control_file = control_file_,
            .install_sql = install_sql_,
            .dso = normal_dso_};
    }

    [[nodiscard]] extension_artifact override_artifact() const {
        extension_artifact artifact = normal_artifact();
        artifact.dso = override_dso_;
        return artifact;
    }

    [[nodiscard]] extension_artifact bioseq_artifact() const {
        return {
            .name = "bioseq",
            .control_file = bioseq_control_,
            .install_sql = bioseq_install_sql_,
            .dso = bioseq_dso_};
    }

    [[nodiscard]] const std::filesystem::path& stage_root() const noexcept {
        return stage_root_;
    }

    [[nodiscard]] const std::filesystem::path& manifest() const noexcept {
        return manifest_;
    }

    [[nodiscard]] const std::filesystem::path& bin_directory() const noexcept {
        return bin_directory_;
    }

    [[nodiscard]] std::filesystem::path control_directory() const {
        return shared_directory_ / "extension";
    }

    [[nodiscard]] const std::filesystem::path& shared_directory()
        const noexcept {
        return shared_directory_;
    }

    [[nodiscard]] const std::filesystem::path& library_directory()
        const noexcept {
        return library_directory_;
    }

    [[nodiscard]] std::filesystem::path runtime_library_directory() const {
        return case_root_ / "runtime-library";
    }

    [[nodiscard]] const std::filesystem::path& override_dso() const noexcept {
        return override_dso_;
    }

    void corrupt_manifest() {
        write(manifest_, "{");
    }

    void replace_manifest_once(
        std::string_view needle,
        std::string_view replacement) {
        std::string contents = read(manifest_);
        const std::size_t position = contents.find(needle);
        if (position == std::string::npos
            || contents.find(needle, position + needle.size())
                != std::string::npos) {
            throw std::runtime_error(
                "stage contract manifest replacement is not unique");
        }
        contents.replace(position, needle.size(), replacement);
        write(manifest_, contents);
    }

    void replace_tool_with_symlink(std::string_view tool) {
        const std::filesystem::path staged_tool =
            bin_directory_ / tool;
        const std::filesystem::path target =
            source_root_ / (std::string(tool) + "-symlink-target");
        write(target, "symlink target\n");
        std::filesystem::remove(staged_tool);
        std::filesystem::create_symlink(target, staged_tool);
    }

    void reseal_mismatched_cache_control() {
        write(
            shared_directory_ / "extension"
                / control_file_.filename(),
            "comment = 'mutated staged control'\n"
            "default_version = '1.0'\n"
            "module_pathname = '$libdir/bioseq_cache'\n");
        write_manifest();
    }

    void use_distinct_cache_module_name() {
        write(
            control_file_,
            "comment = 'stage contract'\n"
            "default_version = '1.0'\n"
            "module_pathname = '$libdir/cache_module'\n");
        write(
            shared_directory_ / "extension"
                / control_file_.filename(),
            read(control_file_));
        const std::filesystem::path renamed_source =
            source_root_ / "cache_module.so";
        std::filesystem::rename(normal_dso_, renamed_source);
        std::filesystem::rename(
            library_directory_ / normal_dso_.filename(),
            library_directory_ / renamed_source.filename());
        normal_dso_ = renamed_source;
        write_manifest();
    }

    void write_manifest_with_reordered_tools() {
        write_manifest(
            {"initdb", "postgres", "pg_ctl",
             "createdb", "dropdb", "psql"});
    }

    void write_manifest_with_duplicate_tools() {
        write_manifest(
            {"postgres", "postgres", "pg_ctl",
             "createdb", "dropdb", "psql"});
    }

    void write_manifest_with_duplicate_artifact() {
        write_manifest(
            {"postgres", "initdb", "pg_ctl",
             "createdb", "dropdb", "psql"},
            true);
    }

    void write_manifest_with_reordered_runtime_entries() {
        write_manifest(
            {"postgres", "initdb", "pg_ctl",
             "createdb", "dropdb", "psql"},
            false,
            true);
    }

    void corrupt_extension_aggregate_hash() {
        corrupt_hash_after(
            "\"extension_artifacts\": {",
            "\"aggregate_sha256\": \"");
    }

    void corrupt_runtime_aggregate_hash() {
        corrupt_hash_after(
            "\"runtime_closure\": {",
            "\"aggregate_sha256\": \"");
    }

    void corrupt_loader_aggregate_hash() {
        corrupt_hash_after(
            "\"loader_dependencies\": {",
            "\"aggregate_sha256\": \"");
    }

    void add_overlay_loader_dependency() {
        const std::filesystem::path staged_file =
            library_directory_ / normal_dso_.filename();
        const std::string source =
            "/pg/lib/" + normal_dso_.filename().string();
        const std::string staged =
            "pg/lib/" + normal_dso_.filename().string();
        const std::uintmax_t size =
            std::filesystem::file_size(staged_file);
        const std::string digest = sha256_file(staged_file).hex();
        const std::string record =
            "source:" + source + '\n'
            + "staged:" + staged + '\n'
            + "size:" + std::to_string(size) + '\n'
            + "sha256:" + digest + '\n';
        const std::string empty_block =
            "    \"loader_dependencies\": {\n"
            "      \"entry_count\": 0,\n"
            "      \"aggregate_sha256\": \""
            + sha256("").hex() + "\",\n"
            "      \"entries\": []\n"
            "    },\n";
        const std::string overlapping_block =
            "    \"loader_dependencies\": {\n"
            "      \"entry_count\": 1,\n"
            "      \"aggregate_sha256\": \""
            + sha256(record).hex() + "\",\n"
            "      \"entries\": [\n"
            "        {\"source\": \"" + source
            + "\", \"staged\": \"" + staged
            + "\", \"size\": " + std::to_string(size)
            + ", \"sha256\": \"" + digest + "\"}\n"
            "      ]\n"
            "    },\n";
        replace_manifest_once(empty_block, overlapping_block);
    }

    void corrupt_tool_hash(std::string_view tool) {
        corrupt_hash_after(
            "\"tool\": \"" + std::string(tool) + "\"",
            "\"sha256\": \"");
    }

    void add_runtime_file() {
        write(
            shared_directory_ / "unexpected-runtime.dat",
            "unexpected runtime file\n");
    }

    void delete_runtime_file() {
        if (!std::filesystem::remove(
                shared_directory_ / "contract-runtime.dat")) {
            throw std::runtime_error(
                "cannot delete runtime-closure contract file");
        }
    }

    void tamper_runtime_file() {
        write(
            shared_directory_ / "contract-runtime.dat",
            "tampered runtime payload\n");
    }

    void add_runtime_symlink() {
        const std::filesystem::path target =
            source_root_ / "runtime-symlink-target";
        write(target, "runtime symlink target\n");
        std::filesystem::create_symlink(
            target, shared_directory_ / "runtime-symlink");
    }

    void add_runtime_special_file() {
        const std::filesystem::path fifo =
            shared_directory_ / "runtime-fifo";
        if (::mkfifo(fifo.c_str(), 0600) != 0) {
            throw std::runtime_error(
                "cannot create runtime-closure FIFO contract: "
                + std::string(std::strerror(errno)));
        }
    }

    void add_unknown_runtime_closure_member() {
        replace_manifest_once(
            "  \"runtime_closure\": {\n",
            "  \"runtime_closure\": {\n"
            "    \"unknown\": null,\n");
    }

    void downgrade_manifest_to_v2() {
        replace_manifest_once(
            "\"schema\": \"stage_manifest/v3\"",
            "\"schema\": \"stage_manifest/v2\"");
    }

private:
    static void write(
        const std::filesystem::path& path,
        std::string_view contents) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << contents;
        output.close();
        if (!output) {
            throw std::runtime_error(
                "cannot write stage contract fixture: " + path.string());
        }
    }

    [[nodiscard]] static std::string read(
        const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error(
                "cannot read stage contract fixture: " + path.string());
        }
        std::string result{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        if (input.bad()) {
            throw std::runtime_error(
                "failed reading stage contract fixture: "
                + path.string());
        }
        return result;
    }

    [[nodiscard]] static std::string json_text(std::string_view text) {
        std::string result;
        for (const char character : text) {
            if (character == '\\' || character == '"') {
                result.push_back('\\');
            }
            result.push_back(character);
        }
        return result;
    }

    [[nodiscard]] static std::string json_string(
        const std::filesystem::path& path) {
        return json_text(path.string());
    }

    [[nodiscard]] static constexpr std::string_view plpgsql_filename() {
#if defined(__APPLE__)
        return "plpgsql.dylib";
#else
        return "plpgsql.so";
#endif
    }

    struct runtime_fixture_entry {
        std::string root;
        std::string path;
        std::string kind;
        std::string source_kind;
        std::string source_path;
        std::optional<std::string> source_link_target;
        std::string resolved_source_path;
        std::optional<std::uintmax_t> size;
        std::optional<std::string> digest;
    };

    [[nodiscard]] std::set<std::pair<std::string, std::string>>
    runtime_overlay_coordinates() const {
        return {
            {"sharedir",
             "extension/" + control_file_.filename().string()},
            {"sharedir",
             "extension/" + install_sql_.filename().string()},
            {"sharedir",
             "extension/" + bioseq_control_.filename().string()},
            {"sharedir",
             "extension/" + bioseq_install_sql_.filename().string()},
            {"pkglibdir", normal_dso_.filename().string()},
            {"pkglibdir", bioseq_dso_.filename().string()}};
    }

    [[nodiscard]] std::vector<runtime_fixture_entry> runtime_entries(
        bool reorder) const {
        const std::array roots{
            std::pair{
                std::string_view{"bindir"},
                std::pair{
                    std::filesystem::path{"/pg/bin"},
                    bin_directory_}},
            std::pair{
                std::string_view{"sharedir"},
                std::pair{
                    std::filesystem::path{"/pg/share"},
                    shared_directory_}},
            std::pair{
                std::string_view{"pkglibdir"},
                std::pair{
                    std::filesystem::path{"/pg/lib"},
                    library_directory_}}};
        const auto excluded = runtime_overlay_coordinates();
        std::vector<runtime_fixture_entry> result;
        for (const auto& [root_name, paths] : roots) {
            const auto& [source_root, staged_root] = paths;
            std::vector<std::filesystem::path> descendants;
            for (const auto& entry :
                 std::filesystem::recursive_directory_iterator(
                     staged_root)) {
                descendants.push_back(entry.path());
            }
            std::ranges::sort(
                descendants,
                [&](const std::filesystem::path& lhs,
                    const std::filesystem::path& rhs) {
                    return lhs.lexically_relative(staged_root)
                               .generic_string()
                        < rhs.lexically_relative(staged_root)
                              .generic_string();
                });
            for (const std::filesystem::path& descendant : descendants) {
                const std::string relative =
                    descendant.lexically_relative(staged_root)
                        .generic_string();
                if (excluded.contains({
                        std::string(root_name), relative})) {
                    continue;
                }
                const bool directory =
                    std::filesystem::is_directory(descendant);
                const std::string source =
                    (source_root / relative).generic_string();
                runtime_fixture_entry entry{
                    .root = std::string(root_name),
                    .path = relative,
                    .kind = directory ? "dir" : "file",
                    .source_kind = directory ? "dir" : "file",
                    .source_path = source,
                    .source_link_target = std::nullopt,
                    .resolved_source_path = source,
                    .size = std::nullopt,
                    .digest = std::nullopt};
                if (!directory) {
                    entry.size =
                        std::filesystem::file_size(descendant);
                    entry.digest = sha256_file(descendant).hex();
                }
                result.push_back(std::move(entry));
            }
        }
        if (reorder) {
            if (result.size() < 2U) {
                throw std::runtime_error(
                    "runtime-closure contract requires two entries");
            }
            std::swap(result[0], result[1]);
        }
        return result;
    }

    [[nodiscard]] static std::string runtime_entry_record(
        const runtime_fixture_entry& entry) {
        return "root:" + entry.root + '\n'
            + "path:" + entry.path + '\n'
            + "kind:" + entry.kind + '\n'
            + "source_kind:" + entry.source_kind + '\n'
            + "source_path:" + entry.source_path + '\n'
            + "source_link_target:"
            + entry.source_link_target.value_or("-") + '\n'
            + "resolved_source_path:" + entry.resolved_source_path + '\n'
            + "size:"
            + (entry.size ? std::to_string(*entry.size) : "-") + '\n'
            + "sha256:" + entry.digest.value_or("-") + '\n';
    }

    [[nodiscard]] static std::string runtime_entry_json(
        const runtime_fixture_entry& entry) {
        std::string result;
        result += "      {\"root\": \"" + json_text(entry.root);
        result += "\", \"path\": \"" + json_text(entry.path);
        result += "\", \"kind\": \"" + json_text(entry.kind);
        result += "\", \"source_kind\": \""
            + json_text(entry.source_kind);
        result += "\", \"source_path\": \""
            + json_text(entry.source_path);
        result += "\", \"source_link_target\": ";
        if (entry.source_link_target) {
            result += "\"" + json_text(*entry.source_link_target) + "\"";
        } else {
            result += "null";
        }
        result += ", \"resolved_source_path\": \""
            + json_text(entry.resolved_source_path) + "\", \"size\": ";
        if (entry.size) {
            result += std::to_string(*entry.size);
        } else {
            result += "null";
        }
        result += ", \"sha256\": ";
        if (entry.digest) {
            result += "\"" + *entry.digest + "\"";
        } else {
            result += "null";
        }
        result += "}";
        return result;
    }

    [[nodiscard]] std::string required_sentinels_json() const {
        struct sentinel_fixture {
            std::string_view name;
            std::string_view root;
            std::string_view path;
            std::string_view kind;
        };
        const std::array sentinels{
            sentinel_fixture{
                "postgres_bki", "sharedir", "postgres.bki", "file"},
            sentinel_fixture{
                "postgresql_conf_sample", "sharedir",
                "postgresql.conf.sample", "file"},
            sentinel_fixture{
                "pg_hba_conf_sample", "sharedir",
                "pg_hba.conf.sample", "file"},
            sentinel_fixture{
                "pg_ident_conf_sample", "sharedir",
                "pg_ident.conf.sample", "file"},
            sentinel_fixture{
                "information_schema_sql", "sharedir",
                "information_schema.sql", "file"},
            sentinel_fixture{
                "system_functions_sql", "sharedir",
                "system_functions.sql", "file"},
            sentinel_fixture{
                "system_views_sql", "sharedir",
                "system_views.sql", "file"},
            sentinel_fixture{
                "system_constraints_sql", "sharedir",
                "system_constraints.sql", "file"},
            sentinel_fixture{
                "snowball_create_sql", "sharedir",
                "snowball_create.sql", "file"},
            sentinel_fixture{
                "timezone", "sharedir", "timezone", "dir"},
            sentinel_fixture{
                "timezonesets", "sharedir", "timezonesets", "dir"},
            sentinel_fixture{
                "tsearch_data", "sharedir", "tsearch_data", "dir"},
            sentinel_fixture{
                "extension", "sharedir", "extension", "dir"},
            sentinel_fixture{
                "plpgsql", "pkglibdir", plpgsql_filename(), "file"}};
        std::string result;
        for (std::size_t index = 0; index != sentinels.size(); ++index) {
            const sentinel_fixture& sentinel = sentinels[index];
            if (index != 0U) {
                result += ",\n";
            }
            result += "      \"" + std::string(sentinel.name)
                + "\": {\"root\": \"" + std::string(sentinel.root)
                + "\", \"path\": \"" + std::string(sentinel.path)
                + "\", \"kind\": \"" + std::string(sentinel.kind)
                + "\", \"sha256\": ";
            if (sentinel.kind == "dir") {
                result += "null}";
                continue;
            }
            const std::filesystem::path root =
                sentinel.root == "sharedir"
                ? shared_directory_
                : library_directory_;
            result += "\""
                + sha256_file(root / sentinel.path).hex()
                + "\"}";
        }
        return result;
    }

    void corrupt_hash_after(
        std::string_view anchor,
        std::string_view hash_label = {}) {
        std::string contents = read(manifest_);
        const std::size_t anchor_position = contents.find(anchor);
        if (anchor_position == std::string::npos) {
            throw std::runtime_error(
                "stage contract hash anchor is absent");
        }
        std::size_t hash_position = anchor_position;
        if (!hash_label.empty()) {
            hash_position = contents.find(hash_label, anchor_position);
            if (hash_position == std::string::npos) {
                throw std::runtime_error(
                    "stage contract hash label is absent");
            }
            hash_position += hash_label.size();
        } else {
            hash_position += anchor.size();
        }
        contents.replace(hash_position, 64U, 64U, '0');
        write(manifest_, contents);
    }

    [[nodiscard]] std::string artifact_json(
        std::string_view name,
        const std::filesystem::path& source_control,
        const std::filesystem::path& source_sql,
        const std::filesystem::path& source_dso) const {
        const std::filesystem::path staged_control =
            shared_directory_ / "extension"
            / source_control.filename();
        const std::string source_control_digest =
            sha256_file(source_control).hex();
        const std::string staged_control_digest =
            sha256_file(staged_control).hex();
        const std::string sql_digest = sha256_file(source_sql).hex();
        const std::string dso_digest = sha256_file(source_dso).hex();
        std::string result;
        result += "    {\n";
        result += "      \"name\": \"" + std::string(name) + "\",\n";
        result += "      \"module_pathname_override\": null,\n";
        result += "      \"effective_module_name\": \""
            + source_dso.stem().string() + "\",\n";
        result += "      \"control\": {\"source\": \""
            + json_string(source_control)
            + "\", \"filename\": \""
            + source_control.filename().string()
            + "\", \"staged\": \"pg/share/extension/"
            + source_control.filename().string()
            + "\", \"source_sha256\": \"" + source_control_digest
            + "\", \"sha256\": \"" + staged_control_digest + "\"},\n";
        result += "      \"install_sql\": {\"source\": \""
            + json_string(source_sql)
            + "\", \"filename\": \"" + source_sql.filename().string()
            + "\", \"staged\": \"pg/share/extension/"
            + source_sql.filename().string()
            + "\", \"version\": \"1.0\", \"sha256\": \""
            + sql_digest + "\"},\n";
        result += "      \"library\": {\"source\": \""
            + json_string(source_dso)
            + "\", \"filename\": \"" + source_dso.filename().string()
            + "\", \"staged\": \"pg/lib/"
            + source_dso.filename().string()
            + "\", \"sha256\": \"" + dso_digest + "\"}\n";
        result += "    }";
        return result;
    }

    [[nodiscard]] std::string aggregate_record() const {
        const auto append =
            [](std::string& record,
               std::string_view name,
               const std::filesystem::path& control,
               const std::filesystem::path& sql,
               const std::filesystem::path& dso,
               const std::filesystem::path& staged_control) {
                record += "artifact:" + std::string(name) + '\n';
                record += "module_pathname_override:\n";
                record += "control:" + control.filename().string()
                    + ':' + sha256_file(staged_control).hex() + '\n';
                record += "install_sql:" + sql.filename().string()
                    + ':' + sha256_file(sql).hex() + '\n';
                record += "library:" + dso.filename().string()
                    + ':' + sha256_file(dso).hex() + '\n';
            };
        std::string record;
        append(
            record, "bioseq", bioseq_control_, bioseq_install_sql_,
            bioseq_dso_,
            shared_directory_ / "extension"
                / bioseq_control_.filename());
        append(
            record, "bioseq_cache", control_file_, install_sql_,
            normal_dso_,
            shared_directory_ / "extension"
                / control_file_.filename());
        return record;
    }

    void write_manifest(
        std::array<std::string_view, 6> tool_order = {
            "postgres", "initdb", "pg_ctl",
            "createdb", "dropdb", "psql"},
        bool duplicate_cache_artifact = false,
        bool reorder_runtime_entries = false) {
        const std::string cache_json = artifact_json(
            "bioseq_cache", control_file_, install_sql_, normal_dso_);
        const std::string bioseq_json = artifact_json(
            "bioseq", bioseq_control_, bioseq_install_sql_, bioseq_dso_);
        const std::vector<runtime_fixture_entry> closure_entries =
            runtime_entries(reorder_runtime_entries);
        std::string closure_entry_json;
        std::string closure_aggregate_record;
        for (std::size_t index = 0;
             index != closure_entries.size();
             ++index) {
            if (index != 0U) {
                closure_entry_json += ",\n";
            }
            closure_entry_json +=
                runtime_entry_json(closure_entries[index]);
            closure_aggregate_record +=
                runtime_entry_record(closure_entries[index]);
        }
        std::string signatures;
        for (std::size_t index = 0; index != tool_order.size(); ++index) {
            const std::string tool{tool_order[index]};
            if (index != 0U) {
                signatures += ",\n";
            }
            signatures +=
                "    {\"tool\": \"" + tool
                + "\", \"staged_path\": \"pg/bin/" + tool
                + "\", \"version\": \"" + tool
                + " (PostgreSQL) 17.0-contract\", \"sha256\": \""
                + sha256_file(bin_directory_ / tool).hex() + "\"}";
        }

        std::string contents;
        contents += "{\n";
        contents += "  \"schema\": \"stage_manifest/v3\",\n";
        contents +=
            "  \"postgresql_version\": "
            "\"PostgreSQL 17.0-contract\",\n";
        contents +=
            "  \"postgresql_configure\": \"--prefix=/pg\",\n";
        contents +=
            "  \"source\": {\"bindir\": \"/pg/bin\", "
            "\"sharedir\": \"/pg/share\", "
            "\"pkglibdir\": \"/pg/lib\"},\n";
        contents +=
            "  \"staged\": {\"bindir\": \"pg/bin\", "
            "\"sharedir\": \"pg/share\", "
            "\"pkglibdir\": \"pg/lib\"},\n";
        contents += "  \"runtime_closure\": {\n";
        contents +=
            "    \"algorithm\": "
            "\"postgresql-materialized-package/v1\",\n";
        contents += "    \"entry_count\": "
            + std::to_string(closure_entries.size()) + ",\n";
        contents += "    \"aggregate_sha256\": \""
            + sha256(closure_aggregate_record).hex() + "\",\n";
        contents += "    \"roots\": {\n";
        contents +=
            "      \"bindir\": {\"reported_source\": \"/pg/bin\", "
            "\"canonical_source\": \"/pg/bin\", "
            "\"staged\": \"pg/bin\"},\n";
        contents +=
            "      \"sharedir\": {\"reported_source\": \"/pg/share\", "
            "\"canonical_source\": \"/pg/share\", "
            "\"staged\": \"pg/share\"},\n";
        contents +=
            "      \"pkglibdir\": {\"reported_source\": \"/pg/lib\", "
            "\"canonical_source\": \"/pg/lib\", "
            "\"staged\": \"pg/lib\"}\n";
        contents += "    },\n";
        contents += "    \"required_sentinels\": {\n";
        contents += required_sentinels_json() + "\n";
        contents += "    },\n";
        contents += "    \"loader_dependencies\": {\n";
        contents += "      \"entry_count\": 0,\n";
        contents += "      \"aggregate_sha256\": \""
            + sha256("").hex() + "\",\n";
        contents += "      \"entries\": []\n";
        contents += "    },\n";
        contents += "    \"entries\": [\n";
        contents += closure_entry_json + "\n";
        contents += "    ]\n";
        contents += "  },\n";
        contents += "  \"extension_artifacts\": {\n";
        contents += "    \"aggregate_sha256\": \""
            + sha256(aggregate_record()).hex() + "\",\n";
        contents += "    \"artifacts\": [\n";
        contents += cache_json + ",\n" + bioseq_json;
        if (duplicate_cache_artifact) {
            contents += ",\n" + cache_json;
        }
        contents += "\n    ]\n";
        contents += "  },\n";
        contents +=
            "  \"expected_tool_paths\": {"
            "\"postgres\": \"pg/bin/postgres\", "
            "\"initdb\": \"pg/bin/initdb\", "
            "\"pg_ctl\": \"pg/bin/pg_ctl\", "
            "\"createdb\": \"pg/bin/createdb\", "
            "\"dropdb\": \"pg/bin/dropdb\", "
            "\"psql\": \"pg/bin/psql\"},\n";
        contents += "  \"tool_signatures\": [\n";
        contents += signatures + "\n";
        contents += "  ]\n";
        contents += "}\n";
        write(manifest_, contents);
    }

    std::filesystem::path root_;
    std::filesystem::path stage_root_;
    std::filesystem::path case_root_;
    std::filesystem::path source_root_;
    std::filesystem::path bin_directory_;
    std::filesystem::path shared_directory_;
    std::filesystem::path library_directory_;
    std::filesystem::path control_file_;
    std::filesystem::path install_sql_;
    std::filesystem::path normal_dso_;
    std::filesystem::path override_dso_;
    std::filesystem::path bioseq_control_;
    std::filesystem::path bioseq_install_sql_;
    std::filesystem::path bioseq_dso_;
    std::filesystem::path manifest_;
};

}  // namespace

TEST_CASE("libpq first-row observation has explicit empty-result semantics",
          "[postgres-test-support][unit][libpq][first-row]") {
    const bioseq::libpq::single_row_observed_result observation;
    CHECK_FALSE(observation.time_to_first_row.has_value());
    CHECK(observation.elapsed == 0ns);
    CHECK(observation.rows.rows() == 0);
}

TEST_CASE("private cluster configuration is portable and stage-first",
          "[postgres-test-support][unit][cluster][configuration]") {
    const std::filesystem::path socket_directory{"/private/socket"};
    const std::array<std::string, 1> preload_libraries{"bioseq_cache"};
    const std::array<std::pair<std::string, std::string>, 1> startup_gucs{{
        {"bioseq.query_cache_optimisation_mode", "on"},
    }};

    const std::string configuration = render_private_cluster_configuration(
        54321, socket_directory, std::nullopt,
        preload_libraries, startup_gucs);

    CHECK(configuration.find("extension_control_path")
          == std::string::npos);
    CHECK(configuration.find(
              "dynamic_library_path = '$libdir'\n")
          != std::string::npos);
    CHECK(configuration.find("/private/stage") == std::string::npos);

    const std::filesystem::path runtime_library_directory{
        "/private/case/runtime-library"};
    const std::string override_configuration =
        render_private_cluster_configuration(
            54321, socket_directory, runtime_library_directory,
            preload_libraries, startup_gucs);
    CHECK(override_configuration.find(
              "dynamic_library_path = "
              "'/private/case/runtime-library:$libdir'\n")
          != std::string::npos);
}

TEST_CASE("preinstalled stage attestation consumes suite artifacts in place",
          "[postgres-test-support][unit][artifact][preinstalled-stage]") {
    temporary_preinstalled_stage_tree tree;
    const std::array artifacts{tree.normal_artifact()};

    const extension_stage stage = attest_preinstalled_extension_stage(
        tree.stage_root(),
        tree.manifest(),
        tree.bin_directory(),
        tree.runtime_library_directory(),
        artifacts,
        {});

    CHECK(stage.stage_root
          == std::filesystem::canonical(tree.stage_root()));
    REQUIRE(stage.manifest_file.has_value());
    CHECK(*stage.manifest_file
          == std::filesystem::canonical(tree.manifest()));
    REQUIRE(stage.manifest_digest.has_value());
    CHECK(*stage.manifest_digest == sha256_file(tree.manifest()));
    CHECK(stage.shared_directory
          == std::filesystem::canonical(tree.shared_directory()));
    CHECK(stage.control_directory
          == std::filesystem::canonical(tree.control_directory()));
    CHECK(stage.library_directory
          == std::filesystem::canonical(tree.library_directory()));
    CHECK_FALSE(stage.runtime_library_directory.has_value());
    CHECK_FALSE(std::filesystem::exists(
        tree.runtime_library_directory()));
    REQUIRE(stage.extensions.size() == 1);
    CHECK(stage.extensions.front().staged_control_file.parent_path()
          == stage.control_directory);
    CHECK(stage.extensions.front().staged_install_sql.parent_path()
          == stage.control_directory);
    CHECK(stage.extensions.front().staged_dso.parent_path()
          == stage.library_directory);
}

TEST_CASE("preinstalled stage permits only an exact named runtime DSO override",
          "[postgres-test-support][unit][artifact][preinstalled-stage]") {
    temporary_preinstalled_stage_tree tree;
    const std::array artifacts{tree.override_artifact()};
    const std::set<std::string> allowed{"bioseq_cache"};

    const extension_stage stage = attest_preinstalled_extension_stage(
        tree.stage_root(),
        tree.manifest(),
        tree.bin_directory(),
        tree.runtime_library_directory(),
        artifacts,
        allowed);

    REQUIRE(stage.runtime_library_directory.has_value());
    CHECK(*stage.runtime_library_directory
          == std::filesystem::canonical(
              tree.runtime_library_directory()));
    REQUIRE(stage.extensions.size() == 1);
    const staged_extension_artifact& artifact = stage.extensions.front();
    CHECK(artifact.staged_control_file.parent_path()
          == std::filesystem::canonical(tree.control_directory()));
    CHECK(artifact.staged_install_sql.parent_path()
          == std::filesystem::canonical(tree.control_directory()));
    CHECK(artifact.staged_dso.parent_path()
          == *stage.runtime_library_directory);
    CHECK(artifact.staged_dso.filename() == "bioseq_cache.so");
    CHECK(sha256_file(artifact.staged_dso)
          == sha256_file(tree.override_dso()));
    CHECK(std::distance(
              std::filesystem::directory_iterator(
                  *stage.runtime_library_directory),
              std::filesystem::directory_iterator{})
          == 1);
}

TEST_CASE("runtime DSO override preserves the attested module identity",
          "[postgres-test-support][unit][artifact][preinstalled-stage]"
          "[override][module-name]") {
    temporary_preinstalled_stage_tree tree;
    tree.use_distinct_cache_module_name();
    const std::array artifacts{tree.override_artifact()};
    const extension_stage stage = attest_preinstalled_extension_stage(
        tree.stage_root(), tree.manifest(), tree.bin_directory(),
        tree.runtime_library_directory(), artifacts,
        std::set<std::string>{"bioseq_cache"});

    REQUIRE(stage.extensions.size() == 1);
    CHECK(
        stage.extensions.front().staged_dso.filename()
        == "cache_module.so");
    REQUIRE(stage.runtime_library_directory.has_value());
    CHECK(
        stage.extensions.front().staged_dso.parent_path()
        == *stage.runtime_library_directory);
    CHECK(
        sha256_file(stage.extensions.front().staged_dso)
        == sha256_file(tree.override_dso()));
    const std::array<std::string, 1> preloads{"cache_module"};
    const std::array<std::pair<std::string, std::string>, 0> gucs{};
    const std::string configuration =
        render_private_cluster_configuration(
            54321, std::filesystem::path{"/private/socket"},
            stage.runtime_library_directory, preloads, gucs);
    CHECK(
        configuration.find(
            stage.runtime_library_directory->string()
            + ":$libdir")
        != std::string::npos);
}

TEST_CASE("preinstalled stage rejects an unauthorized runtime DSO mismatch",
          "[postgres-test-support][unit][artifact][preinstalled-stage]") {
    temporary_preinstalled_stage_tree tree;
    const std::array artifacts{tree.override_artifact()};

    CHECK_THROWS_WITH(
        attest_preinstalled_extension_stage(
            tree.stage_root(),
            tree.manifest(),
            tree.bin_directory(),
            tree.runtime_library_directory(),
            artifacts,
            {}),
        Catch::Matchers::ContainsSubstring(
            "without explicit authorization"));
    CHECK_FALSE(std::filesystem::exists(
        tree.runtime_library_directory()));
}

TEST_CASE("preinstalled stage binds the configured PostgreSQL bin coordinate",
          "[postgres-test-support][unit][artifact][preinstalled-stage]") {
    temporary_preinstalled_stage_tree tree;
    const std::array artifacts{tree.normal_artifact()};

    CHECK_THROWS_WITH(
        attest_preinstalled_extension_stage(
            tree.stage_root(),
            tree.manifest(),
            tree.library_directory(),
            tree.runtime_library_directory(),
            artifacts,
            {}),
        Catch::Matchers::ContainsSubstring(
            "bindir does not belong"));
}

TEST_CASE("preinstalled stage accepts caller-ordered manifest subsets",
          "[postgres-test-support][unit][artifact][preinstalled-stage]"
          "[subset][order]") {
    temporary_preinstalled_stage_tree tree;
    const std::array subset{tree.bioseq_artifact()};
    const extension_stage selected =
        attest_preinstalled_extension_stage(
            tree.stage_root(),
            tree.manifest(),
            tree.bin_directory(),
            tree.runtime_library_directory(),
            subset,
            {});
    REQUIRE(selected.extensions.size() == 1);
    CHECK(selected.extensions.front().name == "bioseq");

    const std::array reversed{
        tree.bioseq_artifact(), tree.normal_artifact()};
    const extension_stage ordered =
        attest_preinstalled_extension_stage(
            tree.stage_root(),
            tree.manifest(),
            tree.bin_directory(),
            tree.runtime_library_directory(),
            reversed,
            {});
    REQUIRE(ordered.extensions.size() == 2);
    CHECK(ordered.extensions[0].name == "bioseq");
    CHECK(ordered.extensions[1].name == "bioseq_cache");
}

TEST_CASE("preinstalled stage rejects corrupt closed-schema manifests",
          "[postgres-test-support][unit][artifact][preinstalled-stage]"
          "[corrupt][schema]") {
    temporary_preinstalled_stage_tree tree;
    const std::array artifacts{tree.normal_artifact()};
    tree.corrupt_manifest();
    CHECK_THROWS_WITH(
        attest_preinstalled_extension_stage(
            tree.stage_root(), tree.manifest(), tree.bin_directory(),
            tree.runtime_library_directory(), artifacts, {}),
        Catch::Matchers::ContainsSubstring(
            "invalid preinstalled PostgreSQL stage manifest"));
}

TEST_CASE("preinstalled stage rejects missing unknown and duplicate members",
          "[postgres-test-support][unit][artifact][preinstalled-stage]"
          "[missing][unknown][duplicate]") {
    temporary_preinstalled_stage_tree tree;
    const std::array artifacts{tree.normal_artifact()};

    SECTION("missing required member") {
        tree.replace_manifest_once(
            "  \"postgresql_configure\": \"--prefix=/pg\",\n", "");
        CHECK_THROWS_WITH(
            attest_preinstalled_extension_stage(
                tree.stage_root(), tree.manifest(),
                tree.bin_directory(),
                tree.runtime_library_directory(), artifacts, {}),
            Catch::Matchers::ContainsSubstring(
                "missing or unknown members"));
    }

    SECTION("unknown member") {
        tree.replace_manifest_once(
            "  \"schema\": \"stage_manifest/v3\",\n",
            "  \"schema\": \"stage_manifest/v3\",\n"
            "  \"unknown\": \"forbidden\",\n");
        CHECK_THROWS_WITH(
            attest_preinstalled_extension_stage(
                tree.stage_root(), tree.manifest(),
                tree.bin_directory(),
                tree.runtime_library_directory(), artifacts, {}),
            Catch::Matchers::ContainsSubstring(
                "missing or unknown members"));
    }

    SECTION("duplicate JSON member") {
        tree.replace_manifest_once(
            "  \"schema\": \"stage_manifest/v3\",\n",
            "  \"schema\": \"stage_manifest/v3\",\n"
            "  \"schema\": \"stage_manifest/v3\",\n");
        CHECK_THROWS_WITH(
            attest_preinstalled_extension_stage(
                tree.stage_root(), tree.manifest(),
                tree.bin_directory(),
                tree.runtime_library_directory(), artifacts, {}),
            Catch::Matchers::ContainsSubstring(
                "duplicate object member"));
    }

    SECTION("duplicate extension artifact") {
        tree.write_manifest_with_duplicate_artifact();
        CHECK_THROWS_WITH(
            attest_preinstalled_extension_stage(
                tree.stage_root(), tree.manifest(),
                tree.bin_directory(),
                tree.runtime_library_directory(), artifacts, {}),
            Catch::Matchers::ContainsSubstring(
                "duplicate extension"));
    }
}

TEST_CASE("preinstalled stage rejects symlinked staged tools",
          "[postgres-test-support][unit][artifact][preinstalled-stage]"
          "[symlink][tools]") {
    temporary_preinstalled_stage_tree tree;
    const std::array artifacts{tree.normal_artifact()};
    tree.replace_tool_with_symlink("psql");
    CHECK_THROWS_WITH(
        attest_preinstalled_extension_stage(
            tree.stage_root(), tree.manifest(), tree.bin_directory(),
            tree.runtime_library_directory(), artifacts, {}),
        Catch::Matchers::ContainsSubstring(
            "missing or symlink stage component"));
}

TEST_CASE("preinstalled stage rejects reordered tool signatures",
          "[postgres-test-support][unit][artifact][preinstalled-stage]"
          "[order][tools]") {
    temporary_preinstalled_stage_tree tree;
    const std::array artifacts{tree.normal_artifact()};
    SECTION("reordered") {
        tree.write_manifest_with_reordered_tools();
        CHECK_THROWS_WITH(
            attest_preinstalled_extension_stage(
                tree.stage_root(), tree.manifest(),
                tree.bin_directory(),
                tree.runtime_library_directory(), artifacts, {}),
            Catch::Matchers::ContainsSubstring(
                "wrong order or duplicate tools"));
    }
    SECTION("duplicate") {
        tree.write_manifest_with_duplicate_tools();
        CHECK_THROWS_WITH(
            attest_preinstalled_extension_stage(
                tree.stage_root(), tree.manifest(),
                tree.bin_directory(),
                tree.runtime_library_directory(), artifacts, {}),
            Catch::Matchers::ContainsSubstring(
                "wrong order or duplicate tools"));
    }
}

TEST_CASE("preinstalled stage rejects a resealed null-override control mismatch",
          "[postgres-test-support][unit][artifact][preinstalled-stage]"
          "[control-mismatch]") {
    temporary_preinstalled_stage_tree tree;
    const std::array artifacts{tree.normal_artifact()};
    tree.reseal_mismatched_cache_control();
    CHECK_THROWS_WITH(
        attest_preinstalled_extension_stage(
            tree.stage_root(), tree.manifest(), tree.bin_directory(),
            tree.runtime_library_directory(), artifacts, {}),
        Catch::Matchers::ContainsSubstring(
            "null module_pathname_override requires identical"));
}

TEST_CASE("preinstalled stage rejects a staged PostgreSQL tool hash mismatch",
          "[postgres-test-support][unit][artifact][preinstalled-stage]"
          "[tool-hash]") {
    temporary_preinstalled_stage_tree tree;
    const std::array artifacts{tree.normal_artifact()};
    tree.corrupt_tool_hash("psql");
    CHECK_THROWS_WITH(
        attest_preinstalled_extension_stage(
            tree.stage_root(), tree.manifest(), tree.bin_directory(),
            tree.runtime_library_directory(), artifacts, {}),
        Catch::Matchers::ContainsSubstring(
            "staged PostgreSQL tool SHA-256 mismatch"));
}

TEST_CASE("preinstalled stage rejects an extension aggregate mismatch",
          "[postgres-test-support][unit][artifact][preinstalled-stage]"
          "[aggregate]") {
    temporary_preinstalled_stage_tree tree;
    const std::array artifacts{tree.normal_artifact()};
    tree.corrupt_extension_aggregate_hash();
    CHECK_THROWS_WITH(
        attest_preinstalled_extension_stage(
            tree.stage_root(), tree.manifest(), tree.bin_directory(),
            tree.runtime_library_directory(), artifacts, {}),
        Catch::Matchers::ContainsSubstring(
            "extension aggregate SHA-256 mismatch"));
}

TEST_CASE("preinstalled stage requires the closed v3 runtime manifest",
          "[postgres-test-support][unit][artifact][preinstalled-stage]"
          "[runtime-closure][schema]") {
    temporary_preinstalled_stage_tree tree;
    const std::array artifacts{tree.normal_artifact()};
    tree.downgrade_manifest_to_v2();
    CHECK_THROWS_WITH(
        attest_preinstalled_extension_stage(
            tree.stage_root(), tree.manifest(), tree.bin_directory(),
            tree.runtime_library_directory(), artifacts, {}),
        Catch::Matchers::ContainsSubstring("stage_manifest/v3"));
}

TEST_CASE("preinstalled stage rejects runtime closure divergence",
          "[postgres-test-support][unit][artifact][preinstalled-stage]"
          "[runtime-closure]") {
    temporary_preinstalled_stage_tree tree;
    const std::array artifacts{tree.normal_artifact()};
    const auto attest = [&]() {
        return attest_preinstalled_extension_stage(
            tree.stage_root(), tree.manifest(), tree.bin_directory(),
            tree.runtime_library_directory(), artifacts, {});
    };

    SECTION("addition") {
        tree.add_runtime_file();
        CHECK_THROWS_WITH(
            attest(),
            Catch::Matchers::ContainsSubstring(
                "additions or deletions"));
    }

    SECTION("deletion") {
        tree.delete_runtime_file();
        CHECK_THROWS_WITH(
            attest(),
            Catch::Matchers::ContainsSubstring(
                "additions or deletions"));
    }

    SECTION("symlink") {
        tree.add_runtime_symlink();
        CHECK_THROWS_WITH(
            attest(),
            Catch::Matchers::ContainsSubstring(
                "contains a symlink"));
    }

    SECTION("special file") {
        tree.add_runtime_special_file();
        CHECK_THROWS_WITH(
            attest(),
            Catch::Matchers::ContainsSubstring(
                "contains a special file"));
    }

    SECTION("file tamper") {
        tree.tamper_runtime_file();
        CHECK_THROWS_WITH(
            attest(),
            Catch::Matchers::ContainsSubstring(
                "type, size, or SHA-256 mismatch"));
    }

    SECTION("aggregate mismatch") {
        tree.corrupt_runtime_aggregate_hash();
        CHECK_THROWS_WITH(
            attest(),
            Catch::Matchers::ContainsSubstring(
                "runtime closure aggregate SHA-256 mismatch"));
    }

    SECTION("empty loader aggregate mismatch") {
        tree.corrupt_loader_aggregate_hash();
        CHECK_THROWS_WITH(
            attest(),
            Catch::Matchers::ContainsSubstring(
                "loader dependency aggregate SHA-256 mismatch"));
    }

    SECTION("loader and explicit overlay collision") {
        tree.add_overlay_loader_dependency();
        CHECK_THROWS_WITH(
            attest(),
            Catch::Matchers::ContainsSubstring(
                "overlaps an explicit extension overlay coordinate"));
    }

    SECTION("entry order") {
        tree.write_manifest_with_reordered_runtime_entries();
        CHECK_THROWS_WITH(
            attest(),
            Catch::Matchers::ContainsSubstring(
                "not sorted unique"));
    }

    SECTION("unknown closure member") {
        tree.add_unknown_runtime_closure_member();
        CHECK_THROWS_WITH(
            attest(),
            Catch::Matchers::ContainsSubstring(
                "runtime_closure"));
    }
}

TEST_CASE("process report collectors preserve case SQL and cleanup evidence",
          "[postgres-test-support][unit][report]") {
    const report_state_reset reset;
    clear_process_sql_transcript();
    clear_cleanup_observations();
    set_report_case_context({
        .test_case = "report collector contract",
        .section = "observer"});

    bioseq::libpq::result command;
    process_sql_transcript().command_succeeded(
        "admin", "SELECT 1", command, 2ms);
    const bioseq::libpq::query_error expected{
        "expected failure", "22000"};
    process_sql_transcript().command_failed(
        "session", "SELECT fail()", expected, 3ms);
    bioseq::libpq::result oversized;
    oversized.columns_.push_back(
        {.name = "payload", .type_oid = 25});
    oversized.rows_.push_back({
        {.is_null = false,
         .text = std::string(64U * 1024U + 1U, 'x')}});
    process_sql_transcript().command_succeeded(
        "session", "SELECT oversized_payload", oversized, 4ms);

    const auto transcript = process_sql_transcript_entries();
    REQUIRE(transcript.size() == 3);
    CHECK(transcript[0].test_case == "report collector contract");
    CHECK(transcript[0].section == "observer");
    CHECK(transcript[0].status == sql_transcript_status::succeeded);
    CHECK(transcript[1].sqlstate == "22000");
    CHECK(transcript[1].status == sql_transcript_status::failed);
    CHECK(transcript[2].rows_truncated);
    CHECK(transcript[2].sampled_rows.empty());

    record_cleanup_observation(
        std::filesystem::path{"/tmp/bioseq-report-contract"},
        true, "removed",
        resource_report{
            .peak_rss_bytes = 1024,
            .observed_temp_bytes = 2048});
    const cleanup_report cleanup = aggregate_cleanup_report();
    CHECK(cleanup.succeeded);
    REQUIRE(cleanup_observations().size() == 1);
    CHECK(aggregate_resource_report().observed_temp_bytes >= 2048);

}

TEST_CASE("resource phase crash journal preserves exact boundary samples",
          "[postgres-test-support][unit][resource][journal]") {
    temporary_resource_journal_tree tree;
    {
        std::ofstream postmaster_pid(
            tree.data_directory() / "postmaster.pid",
            std::ios::trunc);
        postmaster_pid << ::getpid() << '\n';
        postmaster_pid.close();
        REQUIRE(static_cast<bool>(postmaster_pid));
    }
    configure_supervision_state(tree.state_directory());
    initialize_supervision_resource_journal();

    resource_sampling_report live;
    {
        resource_sampler sampler{
            tree.owned_root(), tree.data_directory(), 1h, 1h};
        sampler.observe_phase_boundary("contract-boundary");
        live = sampler.finish();
    }
    const resource_sampling_report recovered =
        load_supervision_resource_report(tree.state_directory());

    REQUIRE(live.phase_boundaries.size() == 1);
    REQUIRE(recovered.phase_boundaries.size() == 1);
    const resource_phase_boundary& expected = live.phase_boundaries.front();
    const resource_phase_boundary& observed =
        recovered.phase_boundaries.front();
    REQUIRE(expected.postmaster_process_tree_rss_bytes.has_value());
    REQUIRE(expected.owned_root_bytes.has_value());
    CHECK(observed.phase == expected.phase);
    CHECK(observed.elapsed == expected.elapsed);
    CHECK(
        observed.postmaster_process_tree_rss_bytes
        == expected.postmaster_process_tree_rss_bytes);
    CHECK(observed.owned_root_bytes == expected.owned_root_bytes);
}

TEST_CASE("watchdog decision has one atomic winner",
          "[postgres-test-support][unit][supervision][journal]") {
    temporary_resource_journal_tree tree;
    configure_supervision_state(tree.state_directory());

    REQUIRE(supervision_registries_empty(tree.state_directory()));
    register_supervised_cluster(supervised_cluster_record{
        .root = tree.owned_root(),
        .data_directory = tree.data_directory()});
    CHECK_FALSE(supervision_registries_empty(tree.state_directory()));
    REQUIRE(unregister_supervised_cluster(tree.owned_root()));
    REQUIRE(supervision_registries_empty(tree.state_directory()));

    REQUIRE(try_claim_supervision_decision(
        tree.state_directory(), "disarmed"));
    CHECK(supervision_decision_claimed(tree.state_directory()));
    CHECK_FALSE(try_claim_supervision_decision(
        tree.state_directory(), "timeout"));
}

TEST_CASE("watchdog decision requires durable complete publication",
          "[postgres-test-support][unit][supervision][decision][fault]") {
    const auto exercise =
        [](supervision_journal_fault fault) {
        temporary_resource_journal_tree tree;
        configure_supervision_state(tree.state_directory());
        set_supervision_journal_fault_for_testing(fault);

        CHECK_FALSE(try_claim_supervision_decision(
            tree.state_directory(), "disarmed"));
        set_supervision_journal_fault_for_testing(
            supervision_journal_fault::none);
        CHECK_FALSE(supervision_decision_claimed(
            tree.state_directory()));
        CHECK_FALSE(std::filesystem::exists(
            tree.state_directory() / "decision"));

        REQUIRE(try_claim_supervision_decision(
            tree.state_directory(), "disarmed"));
        CHECK(supervision_decision_claimed(
            tree.state_directory()));
    };

    SECTION("open") {
        exercise(supervision_journal_fault::open);
    }
    SECTION("write") {
        exercise(supervision_journal_fault::write);
    }
    SECTION("file fsync") {
        exercise(supervision_journal_fault::fsync);
    }
    SECTION("close") {
        exercise(supervision_journal_fault::close);
    }
    SECTION("atomic publish") {
        exercise(supervision_journal_fault::rename);
    }
    SECTION("directory sync") {
        exercise(supervision_journal_fault::directory_sync);
    }
}

TEST_CASE("failed watchdog decision publication is never observable",
          "[postgres-test-support][unit][supervision][decision][concurrent]") {
    temporary_resource_journal_tree tree;
    configure_supervision_state(tree.state_directory());
    set_supervision_journal_fault_for_testing(
        supervision_journal_fault::directory_sync);

    const pid_t publisher = ::fork();
    REQUIRE(publisher >= 0);
    if (publisher == 0) {
        const bool published = try_claim_supervision_decision(
            tree.state_directory(), "disarmed");
        ::_exit(published ? 0 : 1);
    }

    const auto deadline =
        std::chrono::steady_clock::now() + 2s;
    bool observed_candidate = false;
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code error;
        if (std::filesystem::exists(
                tree.state_directory() / "decision", error)
            && !error) {
            observed_candidate = true;
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(observed_candidate);

    CHECK_FALSE(supervision_decision_claimed(
        tree.state_directory()));
    int status{};
    pid_t waited{};
    do {
        waited = ::waitpid(publisher, &status, 0);
    } while (waited < 0 && errno == EINTR);
    REQUIRE(waited == publisher);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);

    set_supervision_journal_fault_for_testing(
        supervision_journal_fault::none);
    CHECK_FALSE(std::filesystem::exists(
        tree.state_directory() / "decision"));
    CHECK_FALSE(supervision_decision_claimed(
        tree.state_directory()));
    REQUIRE(try_claim_supervision_decision(
        tree.state_directory(), "timeout"));
    CHECK(supervision_decision_claimed(
        tree.state_directory()));
}

TEST_CASE("live-resource publication and removal remain retryable",
          "[postgres-test-support][unit][supervision][journal][retry]") {
    temporary_resource_journal_tree tree;
    configure_supervision_state(tree.state_directory());
    const supervised_cluster_record cluster{
        .root = tree.owned_root(),
        .data_directory = tree.data_directory()};

    set_supervision_journal_fault_for_testing(
        supervision_journal_fault::open);
    CHECK_THROWS(register_supervised_cluster(cluster));
    set_supervision_journal_fault_for_testing(
        supervision_journal_fault::none);
    REQUIRE_NOTHROW(register_supervised_cluster(cluster));
    const auto registered = load_supervision_registries(
        tree.state_directory());
    REQUIRE(registered.has_value());
    REQUIRE(registered->clusters.size() == 1);
    CHECK_FALSE(remove_supervision_state(
        tree.state_directory()));

    set_supervision_journal_fault_for_testing(
        supervision_journal_fault::write);
    CHECK_FALSE(unregister_supervised_cluster(
        tree.owned_root()));
    set_supervision_journal_fault_for_testing(
        supervision_journal_fault::none);
    CHECK_FALSE(load_supervision_registries(
        tree.state_directory()));
    REQUIRE(unregister_supervised_cluster(
        tree.owned_root()));
    REQUIRE(supervision_registries_empty(
        tree.state_directory()));
    REQUIRE(remove_supervision_state(
        tree.state_directory()));
    CHECK_FALSE(std::filesystem::exists(
        tree.state_directory()));
}

TEST_CASE("live-resource registry publication failures stay dirty",
          "[postgres-test-support][unit][supervision][journal][fault]") {
    const auto exercise =
        [](supervision_journal_fault fault) {
        temporary_resource_journal_tree tree;
        configure_supervision_state(tree.state_directory());
        set_supervision_journal_fault_for_testing(fault);

        CHECK_THROWS(register_supervised_cluster(
            supervised_cluster_record{
                .root = tree.owned_root(),
                .data_directory = tree.data_directory()}));
        set_supervision_journal_fault_for_testing(
            supervision_journal_fault::none);

        CHECK(std::filesystem::is_regular_file(
            tree.state_directory() / "registries.dirty"));
        CHECK_FALSE(load_supervision_registries(
            tree.state_directory()));
        CHECK_FALSE(supervision_registries_empty(
            tree.state_directory()));
        CHECK_FALSE(remove_supervision_state(
            tree.state_directory()));
    };

    SECTION("open") {
        exercise(supervision_journal_fault::open);
    }
    SECTION("write") {
        exercise(supervision_journal_fault::write);
    }
    SECTION("rename") {
        exercise(supervision_journal_fault::rename);
    }
}

TEST_CASE("live-resource registry loading rejects invalid evidence",
          "[postgres-test-support][unit][supervision][journal][strict]") {
    temporary_resource_journal_tree tree;
    configure_supervision_state(tree.state_directory());

    SECTION("missing journal") {
        REQUIRE(std::filesystem::remove(
            tree.state_directory() / "processes.tsv"));
    }
    SECTION("malformed journal") {
        std::ofstream output(
            tree.state_directory() / "clusters.tsv",
            std::ios::trunc);
        output << "malformed\n";
        output.close();
        REQUIRE(static_cast<bool>(output));
    }
    SECTION("generation mismatch") {
        std::ofstream output(
            tree.state_directory() / "registries.clean",
            std::ios::trunc);
        output << "bioseq-supervision-registry-v1\t999999\n";
        output.close();
        REQUIRE(static_cast<bool>(output));
    }
    SECTION("incomplete publication") {
        std::ofstream output(
            tree.state_directory() / "clusters.tsv.partial",
            std::ios::trunc);
        output << "incomplete";
        output.close();
        REQUIRE(static_cast<bool>(output));
    }

    CHECK_FALSE(load_supervision_registries(
        tree.state_directory()));
    CHECK_FALSE(supervision_registries_empty(
        tree.state_directory()));
    CHECK_FALSE(remove_supervision_state(
        tree.state_directory()));
}

TEST_CASE("supervised child launch is dirty before posix_spawn",
          "[postgres-test-support][unit][supervision][journal][launch]") {
    temporary_resource_journal_tree tree;
    configure_supervision_state(tree.state_directory());

    const auto intent = begin_supervised_process_launch();
    REQUIRE(intent.has_value());
    REQUIRE(intent->supervision_required);
    CHECK(std::filesystem::is_regular_file(
        tree.state_directory() / "registries.dirty"));
    CHECK_FALSE(load_supervision_registries(
        tree.state_directory()));

    REQUIRE(cancel_supervised_process_launch(*intent));
    CHECK_FALSE(std::filesystem::exists(
        tree.state_directory() / "registries.dirty"));
    REQUIRE(supervision_registries_empty(
        tree.state_directory()));
}

TEST_CASE("unjournaled supervised child spawn fails synchronously",
          "[postgres-test-support][unit][supervision][journal][process]") {
    temporary_resource_journal_tree tree;
    configure_supervision_state(tree.state_directory());
    const std::filesystem::path sleeper{"/bin/sleep"};
    REQUIRE(std::filesystem::is_regular_file(sleeper));
    set_supervision_journal_fault_for_testing(
        supervision_journal_fault::open);

    CHECK_THROWS_AS(
        spawned_process::spawn(process_options{
            .executable = sleeper,
            .arguments = {"30"},
            .supervise = true}),
        process_error);
    set_supervision_journal_fault_for_testing(
        supervision_journal_fault::none);

    CHECK(std::filesystem::is_regular_file(
        tree.state_directory() / "registries.dirty"));
    CHECK_FALSE(load_supervision_registries(
        tree.state_directory()));
}

TEST_CASE("supervised spawn preserves exact argv zero and PostgreSQL PATH",
          "[postgres-test-support][unit][process][postgres-binary-identity]") {
    temporary_resource_journal_tree tree;
    configure_supervision_state(tree.state_directory());

    const std::filesystem::path executable =
        std::filesystem::canonical(
            std::filesystem::path{
                BIOSEQ_POSTGRES_TEST_PROCESS_CONTRACT_CHILD_PATH});
    const std::filesystem::path preferred_directory =
        executable.parent_path();
    const std::filesystem::path poisoned_directory =
        tree.state_directory() / "host-postgres-bin";
    REQUIRE(std::filesystem::create_directories(poisoned_directory));
    const std::filesystem::path output =
        tree.state_directory() / "process-contract-child.log";
    const scoped_environment_override host_pg{
        "PGHOST", "host-postgres.invalid"};
    const scoped_environment_override host_pgdata{
        "PGDATA", poisoned_directory.c_str()};

    spawned_process child = spawned_process::spawn(process_options{
        .executable = executable,
        .environment = {{"PATH", poisoned_directory.string()}},
        .preferred_path_directory = preferred_directory,
        .output_file = output,
        .supervise = true,
        .supervision_data_directory = tree.data_directory()});
    const process_exit exit = child.wait_for(5s);
    REQUIRE(exit.succeeded());

    const std::string captured = child.captured_output();
    CHECK(captured.find("argv0=" + executable.string() + "\n")
          != std::string::npos);
    CHECK(captured.find(
              "PATH=" + preferred_directory.string() + ":"
              + poisoned_directory.string() + "\n")
          != std::string::npos);
    CHECK(captured.find("PGHOST=<unset>\n") != std::string::npos);
    CHECK(captured.find("PGDATA=<unset>\n") != std::string::npos);
    CHECK(supervision_registries_empty(tree.state_directory()));
}

TEST_CASE("supervised process identity rejects a changed start token",
          "[postgres-test-support][unit][supervision][identity]") {
    const auto process = capture_supervised_process(
        static_cast<std::uint64_t>(::getpid()));
    REQUIRE(process.has_value());
    REQUIRE(supervised_process_matches(*process));

    supervised_process_record stale = *process;
    stale.start_token += "-reused";
    CHECK_FALSE(supervised_process_matches(stale));

    supervised_process_record moved_group = *process;
    ++moved_group.process_group;
    CHECK(supervised_process_matches(moved_group));
    CHECK_FALSE(supervised_process_group_matches(moved_group));
}

TEST_CASE("startup discovery refuses pidfile identity without live PGDATA binding",
          "[postgres-test-support][unit][supervision][start-window]") {
    temporary_resource_journal_tree tree;
    {
        std::ofstream marker(
            tree.owned_root() / ".bioseq-private-cluster",
            std::ios::trunc);
        marker << "contract\n";
        marker.close();
        REQUIRE(static_cast<bool>(marker));
    }
    {
        std::ofstream postmaster_pid(
            tree.data_directory() / "postmaster.pid",
            std::ios::trunc);
        postmaster_pid << ::getpid() << '\n'
                       << std::filesystem::canonical(
                              tree.data_directory()).string()
                       << "\n1\n";
        postmaster_pid.close();
        REQUIRE(static_cast<bool>(postmaster_pid));
    }
    const auto same_pid_and_pidfile = capture_supervised_process(
        static_cast<std::uint64_t>(::getpid()), {},
        tree.data_directory());
    REQUIRE(same_pid_and_pidfile.has_value());
    CHECK_FALSE(supervised_postmaster_matches_data_directory(
        *same_pid_and_pidfile));
}

TEST_CASE("typed watchdog snapshots preserve prior report evidence",
          "[postgres-test-support][unit][supervision][report]") {
    temporary_resource_journal_tree tree;
    configure_supervision_state(tree.state_directory());

    const std::array cases{
        test_case_report{
            .test_case = "case\tone",
            .section = "section\none",
            .status = "passed",
            .message = "message",
            .duration = 17ms}};
    const std::array phases{
        phase_duration_report{
            .ordinal = 3,
            .test_case = "case\tone",
            .section = "section\none",
            .phase = "load",
            .status = "completed",
            .duration = 19ms}};
    const std::array artifacts{
        artifact_report{
            .name = "bioseq_cache",
            .preinstalled_stage_root = "/suite/stage",
            .stage_manifest = "/metadata/stage-manifest.json",
            .stage_manifest_sha256 = "manifest-digest",
            .control_file = "/source/cache.control",
            .install_sql = "/source/cache.sql",
            .dso = "/source/cache.so",
            .staged_control_file = "/stage/cache.control",
            .staged_install_sql = "/stage/cache.sql",
            .staged_dso = "/stage/cache.so",
            .control_sha256 = "control-digest",
            .install_sql_sha256 = "sql-digest",
            .dso_sha256 = "dso-digest",
            .staged_control_sha256 = "staged-control-digest"}};
    const std::array probes{
        migration_probe_record{
            .stable_id = "cache.contract",
            .test_case = "case\tone",
            .section = "section\none",
            .permutation = "mode=cache",
            .passed = true,
            .ordinal = 5}};

    REQUIRE(publish_supervision_test_case_reports(cases));
    REQUIRE(publish_supervision_phase_duration_reports(phases));
    REQUIRE(publish_supervision_artifact_reports(artifacts));
    REQUIRE(publish_supervision_migration_probe_records(probes));

    const auto recovered_cases =
        load_supervision_test_case_reports(tree.state_directory());
    const auto recovered_phases =
        load_supervision_phase_duration_reports(tree.state_directory());
    const auto recovered_artifacts =
        load_supervision_artifact_reports(tree.state_directory());
    const auto recovered_probes =
        load_supervision_migration_probe_records(tree.state_directory());
    REQUIRE(recovered_cases.has_value());
    REQUIRE(recovered_phases.has_value());
    REQUIRE(recovered_artifacts.has_value());
    REQUIRE(recovered_probes.has_value());
    REQUIRE(recovered_cases->size() == 1);
    REQUIRE(recovered_phases->size() == 1);
    REQUIRE(recovered_artifacts->size() == 1);
    REQUIRE(recovered_probes->size() == 1);
    CHECK(recovered_cases->front().test_case == cases.front().test_case);
    CHECK(recovered_cases->front().section == cases.front().section);
    CHECK(recovered_cases->front().duration == cases.front().duration);
    CHECK(recovered_phases->front().ordinal == phases.front().ordinal);
    CHECK(recovered_phases->front().phase == phases.front().phase);
    CHECK(recovered_phases->front().duration == phases.front().duration);
    CHECK(
        recovered_artifacts->front().staged_dso
        == artifacts.front().staged_dso);
    CHECK(
        recovered_artifacts->front().preinstalled_stage_root
        == artifacts.front().preinstalled_stage_root);
    CHECK(
        recovered_artifacts->front().stage_manifest
        == artifacts.front().stage_manifest);
    CHECK(
        recovered_artifacts->front().stage_manifest_sha256
        == artifacts.front().stage_manifest_sha256);
    CHECK(
        recovered_artifacts->front().install_sql_sha256
        == artifacts.front().install_sql_sha256);
    CHECK(recovered_probes->front().stable_id == probes.front().stable_id);
    CHECK(recovered_probes->front().passed);
    CHECK(recovered_probes->front().ordinal == probes.front().ordinal);
}

TEST_CASE("postgres test support SHA-256 matches the standard vector",
          "[postgres-test-support][unit]") {
    REQUIRE(
        sha256("abc").hex()
        == "ba7816bf8f01cfea414140de5dae2223"
           "b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("postgres test support parses typed recursive EXPLAIN JSON",
          "[postgres-test-support][unit]") {
    const explain_document document = explain_document::parse(
        R"([{"Plan":{"Node Type":"Custom Scan",)"
        R"("Custom Plan Provider":"BioseqCacheScan",)"
        R"("Actual Loops":0,"Plans":[{"Node Type":"Seq Scan"}]}}])");
    const auto nodes =
        document.find_plan_nodes("Custom Plan Provider", "BioseqCacheScan");
    REQUIRE(nodes.size() == 1);
    REQUIRE(explain_property_integer(*nodes.front(), "Actual Loops") == 0);
    REQUIRE(document.plan_nodes().size() == 2);
}

TEST_CASE("postgres test support enforces migration probe identity and counts",
          "[postgres-test-support][unit]") {
    clear_migration_probes();
    {
        migration_probe_scope scope(
            "probe contract", "single", "value=1");
        record_migration_probe("support.probe.contract");
        REQUIRE_THROWS_AS(
            record_migration_probe("support.probe.contract"),
            migration_probe_error);
    }
    const std::array expected{
        expected_migration_probe{
            .stable_id = "support.probe.contract",
            .expected_count = 1}};
    REQUIRE_NOTHROW(validate_migration_probes(expected));
    clear_migration_probes();
}

TEST_CASE("canonical row hash distinguishes NULL from empty text",
          "[postgres-test-support][unit]") {
    exact_row_hasher null_hash;
    const std::array null_row{
        nullable_text_cell{.is_null = true, .bytes = {}}};
    null_hash.append_row(null_row);

    exact_row_hasher empty_hash;
    const std::array empty_row{
        nullable_text_cell{.is_null = false, .bytes = {}}};
    empty_hash.append_row(empty_row);

    REQUIRE(null_hash.finalize() != empty_hash.finalize());

    exact_row_hasher normative;
    const std::array mixed_row{
        nullable_text_cell{.is_null = true, .bytes = {}},
        nullable_text_cell{.is_null = false, .bytes = {}}};
    normative.append_row(mixed_row);
    REQUIRE(
        normative.finalize().hex()
        == "48a1b931ccb6063e8f71c216c9f04d1c"
           "0bd426edf4ce8ee4c98601f6c603b206");
}
