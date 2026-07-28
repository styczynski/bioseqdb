module;

#include <fmt/format.h>
#include <simdjson.h>

#include <chrono>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

export module bioseq.bench.sql_codegen_guardrail;

import bench_framework;
import bioseq.integration_tests.common;

#ifndef BIOSEQ_SQL_GUARDRAIL_CONFIG_PATH
#error "BIOSEQ_SQL_GUARDRAIL_CONFIG_PATH must be defined by the build system"
#endif
#ifndef BIOSEQ_SQL_GUARDRAIL_CALIBRATION_CONFIG_PATH
#error "BIOSEQ_SQL_GUARDRAIL_CALIBRATION_CONFIG_PATH must be defined by the build system"
#endif
#ifndef BIOSEQ_SQL_GUARDRAIL_SQL_FILES
#error "BIOSEQ_SQL_GUARDRAIL_SQL_FILES must be defined by the build system"
#endif
#ifndef BIOSEQ_SQL_GUARDRAIL_JSON_FILES
#error "BIOSEQ_SQL_GUARDRAIL_JSON_FILES must be defined by the build system"
#endif
#ifndef BIOSEQ_SQL_GUARDRAIL_CPP_FILES
#error "BIOSEQ_SQL_GUARDRAIL_CPP_FILES must be defined by the build system"
#endif
#ifndef BIOSEQ_SQL_GUARDRAIL_QUALIFICATION_PATH
#error "BIOSEQ_SQL_GUARDRAIL_QUALIFICATION_PATH must be defined by the build system"
#endif
#ifndef BIOSEQ_SQL_GUARDRAIL_TOOL_PATH
#error "BIOSEQ_SQL_GUARDRAIL_TOOL_PATH must be defined by the build system"
#endif
#ifndef BIOSEQ_SQL_GUARDRAIL_PYTHON
#error "BIOSEQ_SQL_GUARDRAIL_PYTHON must be defined by the build system"
#endif
#ifndef BIOSEQ_SQL_GUARDRAIL_COMPARATOR_DSO_PATH
#error "BIOSEQ_SQL_GUARDRAIL_COMPARATOR_DSO_PATH must be defined by the build system"
#endif
#ifndef BIOSEQ_SQL_GUARDRAIL_REPOSITORY_ROOT
#error "BIOSEQ_SQL_GUARDRAIL_REPOSITORY_ROOT must be defined by the build system"
#endif
#ifndef BIOSEQ_SQL_GUARDRAIL_BUILD_DIR
#error "BIOSEQ_SQL_GUARDRAIL_BUILD_DIR must be defined by the build system"
#endif
#ifndef BIOSEQ_SQL_GUARDRAIL_ARTIFACT_INVENTORY_PATH
#error "BIOSEQ_SQL_GUARDRAIL_ARTIFACT_INVENTORY_PATH must be defined by the build system"
#endif

namespace {

namespace bf = bioseq::bench_framework;
namespace it = bioseq_int_tests;

constexpr std::int64_t kArrayRows = 16'384;
constexpr std::int64_t kArraySequenceLength = 16;
constexpr std::int64_t kTypedWrapperRows = 16'384;
constexpr int kMmsearchStatementsPerSample = 8;
constexpr std::int64_t kAlignerOptionsFieldCount = 36;
constexpr std::int64_t kByReferenceRowCodecFieldCount = 8;
constexpr std::int64_t kComparedRowCodecFieldCount =
    kAlignerOptionsFieldCount + kByReferenceRowCodecFieldCount;
constexpr std::int64_t kMmsearchArity2ExpectedRowsPerStatement = 1;
constexpr std::int64_t kMmsearchArity3ExpectedRowsPerStatement = 1;
constexpr std::int32_t kRowCodecIterations = 4'096;
constexpr std::string_view kArrayInputTable =
    "bioseq_codegen_array_input";
constexpr std::string_view kMmsearchInputTable =
    "bioseq_codegen_mmsearch_input";
constexpr std::string_view kMmsearchSelfSequence =
    "AAAAAAAAAAAAAAAAAAAAAAACTATCATTCATACTTCTATCATTCATACTTAAAAAAAA";

struct ArtifactSize final {
    std::uint64_t bytes = 0;
    std::uint64_t files = 0;
};

struct GuardrailQualification final {
    bool calibration = false;
    bool live_measurement_attested = false;
    std::uint64_t run_nonce = 0;
    std::int64_t baseline_kind = 0;
    std::uint64_t generator_cold_ns = 0;
    std::uint64_t generator_noop_ns = 0;
    std::uint64_t generator_peak_rss_bytes = 0;
    std::uint64_t compile_postgresql_bioseq_ns = 0;
    std::uint64_t compile_bioseq_bench_e2e_ns = 0;
    std::uint64_t baseline_compile_postgresql_bioseq_ns = 0;
    std::uint64_t baseline_compile_bioseq_bench_e2e_ns = 0;
    std::uint64_t from_array_arity_2_ns_limit = 0;
    std::uint64_t from_array_arity_3_ns_limit = 0;
    double typed_wrapper_ratio_limit = 0.0;
    std::uint64_t mmsearch_arity_2_ns_limit = 0;
    std::uint64_t mmsearch_arity_3_ns_limit = 0;
    double row_codec_ratio_limit = 0.0;
    ArtifactSize expected_sql;
    ArtifactSize expected_json;
    ArtifactSize expected_cpp;
};

[[noreturn]] void fail(std::string_view message) {
    std::fprintf(
        stderr,
        "[sql_codegen_guardrail] %.*s\n",
        static_cast<int>(message.size()),
        message.data());
    std::fflush(stderr);
    std::abort();
}

[[nodiscard]] ArtifactSize artifact_size(
        std::string_view encoded_paths,
        std::string_view artifact_class) {
    ArtifactSize result;
    std::set<std::string> seen;
    std::size_t start = 0;
    while (start <= encoded_paths.size()) {
        const std::size_t separator = encoded_paths.find('|', start);
        const std::size_t length =
            separator == std::string_view::npos
            ? encoded_paths.size() - start
            : separator - start;
        const std::string path_text{encoded_paths.substr(start, length)};
        if (path_text.empty()) {
            fail(fmt::format(
                "{} generated artifact inventory contains an empty path",
                artifact_class));
        }
        if (!seen.insert(path_text).second) {
            fail(fmt::format(
                "{} generated artifact inventory contains duplicate path: {}",
                artifact_class,
                path_text));
        }

        const std::filesystem::path path{path_text};
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error) {
            fail(fmt::format(
                "{} generated artifact is unavailable: {}",
                artifact_class,
                path_text));
        }
        const std::uintmax_t bytes =
            std::filesystem::file_size(path, error);
        if (error) {
            fail(fmt::format(
                "cannot size {} generated artifact {}: {}",
                artifact_class,
                path_text,
                error.message()));
        }
        result.bytes += static_cast<std::uint64_t>(bytes);
        ++result.files;

        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 1;
    }
    if (result.files == 0) {
        fail(fmt::format(
            "{} generated artifact inventory is empty",
            artifact_class));
    }
    return result;
}

[[nodiscard]] std::string shell_quote(std::string_view value) {
    std::string result{"'"};
    for (const char character : value) {
        if (character == '\'') {
            result += "'\"'\"'";
        } else {
            result.push_back(character);
        }
    }
    result.push_back('\'');
    return result;
}

[[nodiscard]] std::string sql_string_literal(std::string_view value) {
    std::string result{"'"};
    for (const char character : value) {
        if (character == '\0') {
            fail("SQL string literal contains a NUL byte");
        }
        result.push_back(character);
        if (character == '\'') {
            result.push_back('\'');
        }
    }
    result.push_back('\'');
    return result;
}

void validate_live_artifact(
        std::string_view command_name,
        std::string_view artifact_option,
        std::string_view artifact_path) {
    const char* benchmark_executable =
        std::getenv("BIOSEQ_SQL_GUARDRAIL_BENCHMARK_EXECUTABLE");
    if (benchmark_executable == nullptr
        || *benchmark_executable == '\0') {
        fail(
            "BIOSEQ_SQL_GUARDRAIL_BENCHMARK_EXECUTABLE is required; "
            "run this qualification through bioseq-builder bench-e2e");
    }
    std::string command =
        shell_quote(BIOSEQ_SQL_GUARDRAIL_PYTHON)
        + " "
        + shell_quote(BIOSEQ_SQL_GUARDRAIL_TOOL_PATH)
        + " "
        + std::string(command_name)
        + " "
        + std::string(artifact_option)
        + " "
        + shell_quote(artifact_path)
        + " --repository-root "
        + shell_quote(BIOSEQ_SQL_GUARDRAIL_REPOSITORY_ROOT)
        + " --build-dir "
        + shell_quote(BIOSEQ_SQL_GUARDRAIL_BUILD_DIR)
        + " --artifact-inventory "
        + shell_quote(BIOSEQ_SQL_GUARDRAIL_ARTIFACT_INVENTORY_PATH)
        + " --benchmark-executable "
        + shell_quote(benchmark_executable)
        + " --comparator-dso "
        + shell_quote(BIOSEQ_SQL_GUARDRAIL_COMPARATOR_DSO_PATH)
        + " --generator-executable "
        + shell_quote(BIOSEQ_SQL_GUARDRAIL_GENERATOR_EXECUTABLE_PATH)
        + " --production-dso "
        + shell_quote(BIOSEQ_SQL_GUARDRAIL_PRODUCTION_DSO_PATH)
        + " --installed-production-dso "
        + shell_quote(
            BIOSEQ_SQL_GUARDRAIL_INSTALLED_PRODUCTION_DSO_PATH);
    if (command_name == "validate-qualification-live") {
        const char* baseline_override = std::getenv(
            "BIOSEQ_SQL_GUARDRAIL_REVIEWED_BASELINE_PATH");
        const std::string_view baseline_path =
            baseline_override != nullptr
                && *baseline_override != '\0'
            ? std::string_view{baseline_override}
            : std::string_view{
                BIOSEQ_SQL_GUARDRAIL_REVIEWED_BASELINE_PATH};
        command += " --baseline " + shell_quote(baseline_path);
        const char* authority = std::getenv(
            "BIOSEQ_SQL_GUARDRAIL_MCP_AUTHORITY_PATH");
        const char* authority_socket = std::getenv(
            "BIOSEQ_SQL_GUARDRAIL_MCP_AUTHORITY_SOCKET");
        const char* run_nonce = std::getenv(
            "BIOSEQ_SQL_GUARDRAIL_RUN_NONCE");
        if (authority == nullptr || *authority == '\0'
            || authority_socket == nullptr || *authority_socket == '\0'
            || run_nonce == nullptr || *run_nonce == '\0') {
            fail(
                "live qualification requires MCP authority and an exact "
                "workflow nonce");
        }
        command += " --mcp-authority "
            + shell_quote(authority)
            + " --mcp-authority-socket "
            + shell_quote(authority_socket)
            + " --expected-run-nonce "
            + shell_quote(run_nonce);
    }
    const int status = std::system(command.c_str());
    if (status != 0) {
        fail(fmt::format(
            "SQL-codegen evidence is missing, stale, tampered, or "
            "not bound to the live source/binaries: {}",
            artifact_path));
    }
}

[[nodiscard]] std::string read_text_file(
        const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        fail(fmt::format(
            "cannot open SQL-codegen qualification: {}",
            path.string()));
    }
    return {
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{},
    };
}

[[nodiscard]] simdjson::dom::object require_object(
        simdjson::dom::object parent,
        std::string_view key) {
    simdjson::dom::object result;
    if (parent.at_key(key).get(result) != simdjson::SUCCESS) {
        fail(fmt::format(
            "SQL-codegen qualification field '{}' is not an object",
            key));
    }
    return result;
}

[[nodiscard]] std::string require_string(
        simdjson::dom::object parent,
        std::string_view key) {
    std::string_view result;
    if (parent.at_key(key).get(result) != simdjson::SUCCESS
        || result.empty()) {
        fail(fmt::format(
            "SQL-codegen qualification field '{}' is not a string",
            key));
    }
    return std::string{result};
}

[[nodiscard]] double require_positive_number(
        simdjson::dom::object parent,
        std::string_view key) {
    double result = 0.0;
    if (parent.at_key(key).get(result) != simdjson::SUCCESS
        || !std::isfinite(result)
        || result <= 0.0) {
        fail(fmt::format(
            "SQL-codegen qualification field '{}' is not positive",
            key));
    }
    return result;
}

[[nodiscard]] std::uint64_t require_positive_integer(
        simdjson::dom::object parent,
        std::string_view key) {
    const double value = require_positive_number(parent, key);
    if (value > static_cast<double>(
            std::numeric_limits<std::uint64_t>::max())
        || std::floor(value) != value) {
        fail(fmt::format(
            "SQL-codegen qualification field '{}' is not an integer",
            key));
    }
    return static_cast<std::uint64_t>(value);
}

[[nodiscard]] GuardrailQualification load_qualification() {
    const char* run_nonce_text =
        std::getenv("BIOSEQ_SQL_GUARDRAIL_RUN_NONCE");
    if (run_nonce_text == nullptr || *run_nonce_text == '\0') {
        fail(
            "BIOSEQ_SQL_GUARDRAIL_RUN_NONCE is required; run through "
            "the bioseq-builder bench-e2e lane");
    }
    std::uint64_t run_nonce = 0;
    const std::string_view run_nonce_view{run_nonce_text};
    const auto [nonce_end, nonce_error] = std::from_chars(
        run_nonce_view.data(),
        run_nonce_view.data() + run_nonce_view.size(),
        run_nonce);
    if (nonce_error != std::errc{}
        || nonce_end != run_nonce_view.data() + run_nonce_view.size()
        || run_nonce == 0
        || run_nonce > 9'007'199'254'740'991ULL) {
        fail(
            "BIOSEQ_SQL_GUARDRAIL_RUN_NONCE must be a positive "
            "exact IEEE-754 integer");
    }
    const char* calibration =
        std::getenv("BIOSEQ_SQL_GUARDRAIL_CALIBRATION");
    if (calibration != nullptr
        && std::string_view{calibration} == "1") {
        const char* measurement = std::getenv(
            "BIOSEQ_SQL_GUARDRAIL_CALIBRATION_MEASUREMENT");
        if (measurement == nullptr || *measurement == '\0') {
            fail(
                "calibration requires "
                "BIOSEQ_SQL_GUARDRAIL_CALIBRATION_MEASUREMENT");
        }
        validate_live_artifact(
            "validate-measurement-live",
            "--measurement",
            measurement);
        GuardrailQualification result;
        result.calibration = true;
        result.live_measurement_attested = true;
        result.run_nonce = run_nonce;
        result.from_array_arity_2_ns_limit =
            std::numeric_limits<std::uint64_t>::max();
        result.from_array_arity_3_ns_limit =
            std::numeric_limits<std::uint64_t>::max();
        result.typed_wrapper_ratio_limit =
            std::numeric_limits<double>::infinity();
        result.mmsearch_arity_2_ns_limit =
            std::numeric_limits<std::uint64_t>::max();
        result.mmsearch_arity_3_ns_limit =
            std::numeric_limits<std::uint64_t>::max();
        result.row_codec_ratio_limit =
            std::numeric_limits<double>::infinity();
        return result;
    }

    const char* qualification_override = std::getenv(
        "BIOSEQ_SQL_GUARDRAIL_QUALIFICATION_PATH");
    const std::string_view qualification_path =
        qualification_override != nullptr
            && *qualification_override != '\0'
        ? std::string_view{qualification_override}
        : std::string_view{BIOSEQ_SQL_GUARDRAIL_QUALIFICATION_PATH};
    validate_live_artifact(
        "validate-qualification-live",
        "--qualification",
        qualification_path);
    const std::string body = read_text_file(
        std::filesystem::path{qualification_path});
    simdjson::dom::parser parser;
    simdjson::dom::element document;
    if (parser.parse(body).get(document) != simdjson::SUCCESS) {
        fail("SQL-codegen qualification is not valid JSON");
    }
    simdjson::dom::object root;
    if (document.get(root) != simdjson::SUCCESS) {
        fail("SQL-codegen qualification root is not an object");
    }
    if (require_string(root, "status") != "pass") {
        fail("SQL-codegen qualification status is not pass");
    }
    GuardrailQualification result;
    result.live_measurement_attested = true;
    result.run_nonce = run_nonce;
    const std::string kind = require_string(root, "baseline_kind");
    if (kind == "stage0") {
        result.baseline_kind = 1;
    } else if (kind == "recovered_post_cutover") {
        result.baseline_kind = 2;
    } else {
        fail("SQL-codegen qualification has an invalid baseline kind");
    }

    const simdjson::dom::object observed =
        require_object(root, "observed");
    result.generator_cold_ns =
        require_positive_integer(observed, "generator_cold_ns");
    result.generator_noop_ns =
        require_positive_integer(observed, "generator_noop_ns");
    result.generator_peak_rss_bytes = require_positive_integer(
        observed, "generator_peak_rss_bytes");
    result.compile_postgresql_bioseq_ns = require_positive_integer(
        observed, "compile_postgresql_bioseq_ns");
    result.compile_bioseq_bench_e2e_ns = require_positive_integer(
        observed, "compile_bioseq_bench_e2e_ns");
    const simdjson::dom::object baselines =
        require_object(root, "baselines");
    result.baseline_compile_postgresql_bioseq_ns =
        require_positive_integer(
            baselines,
            "compile_postgresql_bioseq_ns");
    result.baseline_compile_bioseq_bench_e2e_ns =
        require_positive_integer(
            baselines,
            "compile_bioseq_bench_e2e_ns");

    const simdjson::dom::object limits =
        require_object(root, "limits");
    result.from_array_arity_2_ns_limit =
        require_positive_integer(limits, "from_array_arity_2_ns");
    result.from_array_arity_3_ns_limit =
        require_positive_integer(limits, "from_array_arity_3_ns");
    result.typed_wrapper_ratio_limit =
        require_positive_number(
            limits,
            "typed_wrapper_generated_to_handwritten_ratio");
    result.mmsearch_arity_2_ns_limit =
        require_positive_integer(limits, "mmsearch_arity_2_ns");
    result.mmsearch_arity_3_ns_limit =
        require_positive_integer(limits, "mmsearch_arity_3_ns");
    result.row_codec_ratio_limit =
        require_positive_number(
            limits,
            "row_codec_generated_to_handwritten_ratio");

    const simdjson::dom::object artifacts =
        require_object(root, "expected_artifacts");
    const auto artifact = [&artifacts](std::string_view group) {
        const simdjson::dom::object item =
            require_object(artifacts, group);
        return ArtifactSize{
            .bytes = require_positive_integer(item, "bytes"),
            .files = require_positive_integer(item, "files"),
        };
    };
    result.expected_sql = artifact("sql");
    result.expected_json = artifact("json");
    result.expected_cpp = artifact("cpp");
    return result;
}

[[nodiscard]] const GuardrailQualification&
load_qualification_once() {
    static const GuardrailQualification qualification =
        load_qualification();
    return qualification;
}

[[nodiscard]] long long query_int(
        const it::connection_test_context& connection,
        std::string_view sql) {
    return std::stoll(it::query_scalar(connection, sql));
}

struct SqlCodegenGuardrailInput final {
    std::shared_ptr<it::connection_test_context> connection;
    GuardrailQualification qualification;
    std::int64_t expected_array_sum = 0;
    std::int64_t expected_typed_wrapper_count = 0;
    std::int64_t expected_mmsearch_arity_2_count = 0;
    std::int64_t expected_mmsearch_arity_3_count = 0;
    bool mmsearch_arity_2_custom_scan = false;
    bool mmsearch_arity_3_custom_scan = false;
    std::string generated_typed_wrapper_sql;
    std::string handwritten_typed_wrapper_sql;
    std::string generated_scalar_row_codec_sql;
    std::string handwritten_scalar_row_codec_sql;
    std::string generated_by_reference_row_codec_sql;
    std::string handwritten_by_reference_row_codec_sql;
    ArtifactSize generated_sql;
    ArtifactSize generated_json;
    ArtifactSize generated_cpp;
};

struct SqlCodegenGuardrailOutput final {
    std::int64_t result_value = 0;
    std::int64_t expected_value = 0;
    std::uint64_t query_ns = 0;
    std::uint64_t generated_query_ns = 0;
    std::uint64_t handwritten_query_ns = 0;
    double generated_to_handwritten_ratio = 0.0;
    std::int64_t arity = 0;
    std::int64_t generated_typed_wrapper = 0;
    std::int64_t direct_fmgr_wrapper_path = 0;
    std::int64_t planner_custom_scan_path = 0;
    std::int64_t generated_row_codec_fields = 0;
    std::int64_t correctness_passed = 0;
    std::int64_t performance_gate_passed = 0;
    std::int64_t build_qualification_passed = 0;
    std::int64_t qualification_baseline_kind = 0;
    std::int64_t live_measurement_attested = 0;
    std::uint64_t run_nonce = 0;
    std::uint64_t generator_cold_ns = 0;
    std::uint64_t generator_noop_ns = 0;
    std::uint64_t generator_peak_rss_bytes = 0;
    std::uint64_t compile_postgresql_bioseq_ns = 0;
    std::uint64_t compile_bioseq_bench_e2e_ns = 0;
    double compile_postgresql_bioseq_delta_percent = 0.0;
    double compile_bioseq_bench_e2e_delta_percent = 0.0;
    ArtifactSize generated_sql;
    ArtifactSize generated_json;
    ArtifactSize generated_cpp;
};

struct PairedRuntimeSampleWriter final {
    std::ofstream output;
    std::map<std::string, std::uint64_t> next_ordinal;

    PairedRuntimeSampleWriter() {
        const char* raw_path = std::getenv(
            "BIOSEQ_SQL_GUARDRAIL_PAIRED_SAMPLES_RAW_PATH");
        if (raw_path == nullptr || *raw_path == '\0') {
            fail(
                "BIOSEQ_SQL_GUARDRAIL_PAIRED_SAMPLES_RAW_PATH is "
                "required; run through the MCP guardrail workflow");
        }
        const std::filesystem::path path{raw_path};
        if (std::filesystem::exists(path)) {
            fail(fmt::format(
                "paired runtime sample path already exists: {}",
                path.string()));
        }
        output.open(path, std::ios::out | std::ios::trunc);
        if (!output) {
            fail(fmt::format(
                "cannot create paired runtime sample file: {}",
                path.string()));
        }
    }

    void append(
            std::string_view algorithm,
            const SqlCodegenGuardrailOutput& sample) {
        const auto ordinal =
            ++next_ordinal[std::string{algorithm}];
        output << fmt::format(
            "{{\"schema_version\":1,\"run_nonce\":{},"
            "\"algorithm\":\"{}\",\"sample_ordinal\":{},"
            "\"generated_query_ns\":{},"
            "\"handwritten_query_ns\":{},"
            "\"generated_to_handwritten_ratio\":{:.17g}}}\n",
            sample.run_nonce,
            algorithm,
            ordinal,
            sample.generated_query_ns,
            sample.handwritten_query_ns,
            sample.generated_to_handwritten_ratio);
        output.flush();
        if (!output) {
            fail("cannot persist paired runtime sample evidence");
        }
    }
};

[[nodiscard]] PairedRuntimeSampleWriter&
paired_runtime_sample_writer() {
    static PairedRuntimeSampleWriter writer;
    return writer;
}

[[nodiscard]] bf::RunConstraints estimate_resources(
        std::string_view,
        std::string_view,
        simdjson::dom::element) {
    return {
        .base_memory = 16u * 1024u * 1024u,
        .extra_memory = 4u * 1024u * 1024u,
    };
}

[[nodiscard]] SqlCodegenGuardrailInput make_input(
        std::string_view,
        std::string_view,
        simdjson::dom::element,
        const bf::RunConstraints&) {
    SqlCodegenGuardrailInput input;
    input.connection =
        std::make_shared<it::connection_test_context>();
    auto& connection = *input.connection;

    input.generated_sql = artifact_size(
        BIOSEQ_SQL_GUARDRAIL_SQL_FILES, "SQL");
    input.generated_json = artifact_size(
        BIOSEQ_SQL_GUARDRAIL_JSON_FILES, "JSON");
    input.generated_cpp = artifact_size(
        BIOSEQ_SQL_GUARDRAIL_CPP_FILES, "C++");
    input.qualification = load_qualification_once();
    if (input.qualification.calibration) {
        input.qualification.expected_sql = input.generated_sql;
        input.qualification.expected_json = input.generated_json;
        input.qualification.expected_cpp = input.generated_cpp;
    } else {
        const auto require_artifact_match = [](
                std::string_view group,
                ArtifactSize observed,
                ArtifactSize qualified) {
            if (observed.bytes != qualified.bytes
                || observed.files != qualified.files) {
                fail(fmt::format(
                    "{} artifact inventory differs from the reviewed "
                    "qualification: observed={}/{} qualified={}/{}",
                    group,
                    observed.files,
                    observed.bytes,
                    qualified.files,
                    qualified.bytes));
            }
        };
        require_artifact_match(
            "SQL",
            input.generated_sql,
            input.qualification.expected_sql);
        require_artifact_match(
            "JSON",
            input.generated_json,
            input.qualification.expected_json);
        require_artifact_match(
            "C++",
            input.generated_cpp,
            input.qualification.expected_cpp);
    }

    it::exec_sql(connection, "SET max_parallel_workers_per_gather = 0;");
    it::exec_sql(
        connection,
        "SET bioseq.aligner.vulkan_enabled = off;");

    it::exec_sqlf(
        connection,
        "DROP TABLE IF EXISTS {}",
        kArrayInputTable);
    it::exec_sqlf(
        connection,
        "CREATE TEMP TABLE {} AS "
        "SELECT g::integer AS row_id, "
        "       bioseq.to_array("
        "         (CASE WHEN (g & 1) = 0 "
        "          THEN 'ACGTACGTACGTACGT' "
        "          ELSE 'TGCATGCATGCATGCA' END)"
        "         ::bioseq.seq('dna4', 64)) AS letters, "
        "       64::integer AS width_bits "
        "FROM generate_series(1, {}) AS rows(g)",
        kArrayInputTable,
        kArrayRows);
    input.expected_array_sum =
        kArrayRows * kArraySequenceLength;

    const std::string extension_schema = it::query_scalar(
        connection,
        "SELECT pg_catalog.quote_ident(n.nspname) "
        "  FROM pg_catalog.pg_extension AS e "
        "  JOIN pg_catalog.pg_namespace AS n "
        "    ON n.oid = e.extnamespace "
        " WHERE e.extname = 'bioseq';");
    if (extension_schema.empty()) {
        fail("cannot resolve the installed bioseq extension schema");
    }
    const std::string comparator_dso =
        sql_string_literal(BIOSEQ_SQL_GUARDRAIL_COMPARATOR_DSO_PATH);
    it::exec_sqlf(
        connection,
        "CREATE OR REPLACE FUNCTION "
        "pg_temp.doc0388_handwritten_vemptycount(bigint) "
        "RETURNS bigint "
        "AS {}, 'doc0388_handwritten_vemptycount' "
        "LANGUAGE C IMMUTABLE PARALLEL SAFE;",
        comparator_dso);
    it::exec_sqlf(
        connection,
        "CREATE OR REPLACE FUNCTION "
        "pg_temp.doc0388_generated_aligner_options_codec("
        "    {}.bioseq_aligner_options, integer) "
        "RETURNS bigint "
        "AS {}, 'doc0388_generated_aligner_options_codec' "
        "LANGUAGE C IMMUTABLE PARALLEL SAFE;",
        extension_schema,
        comparator_dso);
    it::exec_sqlf(
        connection,
        "CREATE OR REPLACE FUNCTION "
        "pg_temp.doc0388_handwritten_aligner_options_codec("
        "    {}.bioseq_aligner_options, integer) "
        "RETURNS bigint "
        "AS {}, 'doc0388_handwritten_aligner_options_codec' "
        "LANGUAGE C IMMUTABLE PARALLEL SAFE;",
        extension_schema,
        comparator_dso);
    it::exec_sqlf(
        connection,
        "CREATE OR REPLACE FUNCTION "
        "pg_temp.doc0388_generated_row_mask_codec("
        "    bioseq_internal.row_mask, integer) "
        "RETURNS bigint "
        "AS {}, 'doc0388_generated_row_mask_codec' "
        "LANGUAGE C IMMUTABLE PARALLEL SAFE;",
        comparator_dso);
    it::exec_sqlf(
        connection,
        "CREATE OR REPLACE FUNCTION "
        "pg_temp.doc0388_handwritten_row_mask_codec("
        "    bioseq_internal.row_mask, integer) "
        "RETURNS bigint "
        "AS {}, 'doc0388_handwritten_row_mask_codec' "
        "LANGUAGE C IMMUTABLE PARALLEL SAFE;",
        comparator_dso);

    const std::string generated_from_array_arity_2_sql = fmt::format(
        "SELECT sum(bioseq.len("
        "  bioseq.from_array(letters, width_bits))) "
        "FROM {}",
        kArrayInputTable);
    const std::string generated_from_array_arity_3_sql = fmt::format(
        "SELECT sum(bioseq.len("
        "  bioseq.from_array(letters, 'dna4', width_bits))) "
        "FROM {}",
        kArrayInputTable);
    if (query_int(connection, generated_from_array_arity_2_sql)
            != input.expected_array_sum
        || query_int(connection, generated_from_array_arity_3_sql)
            != input.expected_array_sum) {
        fail("generated from_array arity preflight is incorrect");
    }

    input.generated_typed_wrapper_sql = fmt::format(
        "SELECT sum({}.vemptycount(g::bigint)) "
        "FROM generate_series(1, {}) AS values(g)",
        extension_schema,
        kTypedWrapperRows);
    input.handwritten_typed_wrapper_sql = fmt::format(
        "SELECT sum(pg_temp.doc0388_handwritten_vemptycount("
        "  g::bigint)) "
        "FROM generate_series(1, {}) AS values(g)",
        kTypedWrapperRows);
    input.expected_typed_wrapper_count =
        kTypedWrapperRows * (kTypedWrapperRows + 1) / 2;
    if (query_int(connection, input.generated_typed_wrapper_sql)
            != input.expected_typed_wrapper_count
        || query_int(connection, input.handwritten_typed_wrapper_sql)
            != input.expected_typed_wrapper_count) {
        fail("generated/handwritten typed-wrapper preflight disagrees");
    }

    const std::string aligner_options_expression = fmt::format(
        "{}.aligner_options("
        "kmer_size => 15, window_size => 10, max_gap => 5000, "
        "chain_gap_scale => 0.8, pri_ratio => 0.8, best_n => 5, "
        "vulkan_enabled => false, index_read_requirement => 1)",
        extension_schema);
    const std::string row_mask_expression =
        "(ROW("
        "101::bigint, 202::bigint, 303::bigint, 4::integer, "
        "505::bigint, 606::bigint, 7::integer, "
        "pg_catalog.decode('00010203fefdfc', 'hex'))"
        "::bioseq_internal.row_mask)";
    input.generated_scalar_row_codec_sql = fmt::format(
        "SELECT pg_temp.doc0388_generated_aligner_options_codec("
        "{}, {})",
        aligner_options_expression,
        kRowCodecIterations);
    input.handwritten_scalar_row_codec_sql = fmt::format(
        "SELECT pg_temp.doc0388_handwritten_aligner_options_codec("
        "{}, {})",
        aligner_options_expression,
        kRowCodecIterations);
    input.generated_by_reference_row_codec_sql = fmt::format(
        "SELECT pg_temp.doc0388_generated_row_mask_codec({}, {})",
        row_mask_expression,
        kRowCodecIterations);
    input.handwritten_by_reference_row_codec_sql = fmt::format(
        "SELECT pg_temp.doc0388_handwritten_row_mask_codec({}, {})",
        row_mask_expression,
        kRowCodecIterations);
    const std::string generated_scalar_preflight = it::query_scalar(
        connection,
        input.generated_scalar_row_codec_sql);
    const std::string handwritten_scalar_preflight = it::query_scalar(
        connection,
        input.handwritten_scalar_row_codec_sql);
    if (generated_scalar_preflight != handwritten_scalar_preflight) {
        fail(fmt::format(
            "generated/handwritten scalar row-codec comparators "
            "disagree: "
            "generated={} handwritten={}",
            generated_scalar_preflight,
            handwritten_scalar_preflight));
    }
    const std::string generated_by_reference_preflight =
        it::query_scalar(
            connection,
            input.generated_by_reference_row_codec_sql);
    const std::string handwritten_by_reference_preflight =
        it::query_scalar(
            connection,
            input.handwritten_by_reference_row_codec_sql);
    if (generated_by_reference_preflight
            != handwritten_by_reference_preflight) {
        fail(fmt::format(
            "generated/handwritten by-reference row-codec comparators "
            "disagree: generated={} handwritten={}",
            generated_by_reference_preflight,
            handwritten_by_reference_preflight));
    }

    it::exec_sql(
        connection,
        "SET bioseq.update_index_on_insert = on;");
    it::create_seq_table(
        connection,
        it::seq_table_spec{
            .name = std::string(kMmsearchInputTable),
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 2,
            .using_bioseq = true,
        },
        {
            it::seq_row{
                .pk = 1,
                .sequences = {
                    std::string(kMmsearchSelfSequence),
                    std::string(kMmsearchSelfSequence),
                },
            },
        });
    const std::string mmsearch_arity_2 = fmt::format(
        "SELECT count(*) FROM {} "
        "WHERE mmsearch(seq0, seq1) "
        "  AND bioseq.query_start(seq0) = 0 "
        "  AND bioseq.target_start(seq0) = 0 "
        "  AND bioseq.query_end(seq0) = bioseq.len(seq1) "
        "  AND bioseq.target_end(seq0) = bioseq.len(seq0)",
        kMmsearchInputTable);
    const std::string mmsearch_arity_3 = fmt::format(
        "SELECT count(*) FROM {} "
        "WHERE mmsearch(seq0, seq1, aligner_options()) "
        "  AND bioseq.query_start(seq0) = 0 "
        "  AND bioseq.target_start(seq0) = 0 "
        "  AND bioseq.query_end(seq0) = bioseq.len(seq1) "
        "  AND bioseq.target_end(seq0) = bioseq.len(seq0)",
        kMmsearchInputTable);
    const auto arity_2_plan =
        it::explain_text<true>(connection, mmsearch_arity_2);
    const auto arity_3_plan =
        it::explain_text<true>(connection, mmsearch_arity_3);
    input.mmsearch_arity_2_custom_scan =
        it::plan_contains(
            arity_2_plan, "Custom Scan (BioseqScan)");
    input.mmsearch_arity_3_custom_scan =
        it::plan_contains(
            arity_3_plan, "Custom Scan (BioseqScan)");
    if (!input.mmsearch_arity_2_custom_scan
        || !input.mmsearch_arity_3_custom_scan) {
        fail(
            "mmsearch guardrail fixture did not select "
            "Custom Scan (BioseqScan) for both arities");
    }
    const std::int64_t arity_2_count =
        query_int(connection, mmsearch_arity_2);
    const std::int64_t arity_3_count =
        query_int(connection, mmsearch_arity_3);
    if (arity_2_count != kMmsearchArity2ExpectedRowsPerStatement
        || arity_3_count != kMmsearchArity3ExpectedRowsPerStatement) {
        fail(fmt::format(
            "mmsearch deterministic fixture preflight failed: "
            "arity2={} expected2={} arity3={} expected3={}",
            arity_2_count,
            kMmsearchArity2ExpectedRowsPerStatement,
            arity_3_count,
            kMmsearchArity3ExpectedRowsPerStatement));
    }
    input.expected_mmsearch_arity_2_count =
        kMmsearchArity2ExpectedRowsPerStatement
        * kMmsearchStatementsPerSample;
    input.expected_mmsearch_arity_3_count =
        kMmsearchArity3ExpectedRowsPerStatement
        * kMmsearchStatementsPerSample;
    return input;
}

void cleanup_input(
        std::string_view,
        std::string_view,
        simdjson::dom::element,
        const SqlCodegenGuardrailInput& input) {
    if (input.connection != nullptr) {
        it::exec_sqlf(
            *input.connection,
            "DROP TABLE IF EXISTS {} CASCADE",
            kMmsearchInputTable);
    }
}

[[nodiscard]] SqlCodegenGuardrailOutput run_case(
        std::string_view algorithm,
        const bf::ParamMap&,
        const SqlCodegenGuardrailInput& input) {
    SqlCodegenGuardrailOutput output{
        .build_qualification_passed =
            input.qualification.calibration ? 0 : 1,
        .qualification_baseline_kind =
            input.qualification.baseline_kind,
        .live_measurement_attested =
            input.qualification.live_measurement_attested ? 1 : 0,
        .run_nonce = input.qualification.run_nonce,
        .generator_cold_ns =
            input.qualification.generator_cold_ns,
        .generator_noop_ns =
            input.qualification.generator_noop_ns,
        .generator_peak_rss_bytes =
            input.qualification.generator_peak_rss_bytes,
        .compile_postgresql_bioseq_ns =
            input.qualification.compile_postgresql_bioseq_ns,
        .compile_bioseq_bench_e2e_ns =
            input.qualification.compile_bioseq_bench_e2e_ns,
        .generated_sql = input.generated_sql,
        .generated_json = input.generated_json,
        .generated_cpp = input.generated_cpp,
    };
    if (!input.qualification.calibration) {
        output.compile_postgresql_bioseq_delta_percent =
            (static_cast<double>(
                 input.qualification.compile_postgresql_bioseq_ns)
             / static_cast<double>(
                 input.qualification
                     .baseline_compile_postgresql_bioseq_ns)
             - 1.0)
            * 100.0;
        output.compile_bioseq_bench_e2e_delta_percent =
            (static_cast<double>(
                 input.qualification.compile_bioseq_bench_e2e_ns)
             / static_cast<double>(
                 input.qualification
                     .baseline_compile_bioseq_bench_e2e_ns)
             - 1.0)
            * 100.0;
    }
    const auto timed_query = [&input](
            std::string_view sql,
            int statement_count) {
        std::int64_t value = 0;
        const auto started = std::chrono::steady_clock::now();
        for (int statement = 0;
             statement < statement_count;
             ++statement) {
            value += query_int(*input.connection, sql);
        }
        const auto finished = std::chrono::steady_clock::now();
        const std::uint64_t elapsed =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<
                    std::chrono::nanoseconds>(
                    finished - started)
                    .count());
        if (elapsed == 0) {
            fail("SQL-codegen guardrail observed a zero-duration query");
        }
        return std::pair{value, elapsed};
    };

    if (algorithm == "qualification_reference") {
        output.build_qualification_passed = 0;
        output.qualification_baseline_kind = 0;
        output.live_measurement_attested = 0;
        output.run_nonce = 0;
        output.performance_gate_passed = 0;
        output.generator_cold_ns = 0;
        output.generator_noop_ns = 0;
        output.generator_peak_rss_bytes = 0;
        output.compile_postgresql_bioseq_ns = 0;
        output.compile_bioseq_bench_e2e_ns = 0;
        output.compile_postgresql_bioseq_delta_percent = 0.0;
        output.compile_bioseq_bench_e2e_delta_percent = 0.0;
        output.generated_sql = {};
        output.generated_json = {};
        output.generated_cpp = {};
        output.expected_value = 0;
        output.result_value = 0;
    } else if (algorithm == "from_array_arity_2") {
        output.arity = 2;
        output.generated_typed_wrapper = 1;
        output.direct_fmgr_wrapper_path = 1;
        output.expected_value = input.expected_array_sum;
        const std::string generated_sql = fmt::format(
            "SELECT sum(bioseq.len("
            "  bioseq.from_array(letters, width_bits))) "
            "FROM {}",
            kArrayInputTable);
        const auto generated = timed_query(generated_sql, 1);
        output.result_value = generated.first;
        output.generated_query_ns = generated.second;
        output.query_ns = output.generated_query_ns;
        output.performance_gate_passed =
            output.generated_query_ns
                <= input.qualification.from_array_arity_2_ns_limit
            ? 1 : 0;
    } else if (algorithm == "from_array_arity_3") {
        output.arity = 3;
        output.generated_typed_wrapper = 1;
        output.direct_fmgr_wrapper_path = 1;
        output.expected_value = input.expected_array_sum;
        const std::string generated_sql = fmt::format(
            "SELECT sum(bioseq.len("
            "  bioseq.from_array(letters, 'dna4', width_bits))) "
            "FROM {}",
            kArrayInputTable);
        const auto generated = timed_query(generated_sql, 1);
        output.result_value = generated.first;
        output.generated_query_ns = generated.second;
        output.query_ns = output.generated_query_ns;
        output.performance_gate_passed =
            output.generated_query_ns
                <= input.qualification.from_array_arity_3_ns_limit
            ? 1 : 0;
    } else if (algorithm
               == "typed_wrapper_generated_vs_handwritten") {
        output.arity = 1;
        output.generated_typed_wrapper = 1;
        output.direct_fmgr_wrapper_path = 1;
        output.expected_value = input.expected_typed_wrapper_count;
        const auto generated = timed_query(
            input.generated_typed_wrapper_sql, 1);
        const auto handwritten = timed_query(
            input.handwritten_typed_wrapper_sql, 1);
        output.result_value = generated.first;
        if (handwritten.first != output.expected_value) {
            fail("handwritten typed-wrapper comparator is incorrect");
        }
        output.generated_query_ns = generated.second;
        output.handwritten_query_ns = handwritten.second;
        output.query_ns = output.generated_query_ns;
        output.generated_to_handwritten_ratio =
            static_cast<double>(output.generated_query_ns)
            / static_cast<double>(output.handwritten_query_ns);
        output.performance_gate_passed =
            output.generated_to_handwritten_ratio
                <= input.qualification.typed_wrapper_ratio_limit
            ? 1 : 0;
    } else if (algorithm == "mmsearch_arity_2") {
        output.arity = 2;
        output.planner_custom_scan_path =
            input.mmsearch_arity_2_custom_scan ? 1 : 0;
        output.expected_value =
            input.expected_mmsearch_arity_2_count;
        const std::string sql = fmt::format(
            "SELECT count(*) FROM {} "
            "WHERE mmsearch(seq0, seq1) "
            "  AND bioseq.query_start(seq0) = 0 "
            "  AND bioseq.target_start(seq0) = 0 "
            "  AND bioseq.query_end(seq0) = bioseq.len(seq1) "
            "  AND bioseq.target_end(seq0) = bioseq.len(seq0)",
            kMmsearchInputTable);
        const auto generated = timed_query(
            sql, kMmsearchStatementsPerSample);
        output.result_value = generated.first;
        output.generated_query_ns = generated.second;
        output.query_ns = output.generated_query_ns;
        output.performance_gate_passed =
            output.generated_query_ns
                <= input.qualification.mmsearch_arity_2_ns_limit
            ? 1 : 0;
    } else if (algorithm == "mmsearch_arity_3") {
        output.arity = 3;
        output.planner_custom_scan_path =
            input.mmsearch_arity_3_custom_scan ? 1 : 0;
        output.generated_row_codec_fields =
            kAlignerOptionsFieldCount;
        output.expected_value =
            input.expected_mmsearch_arity_3_count;
        const std::string sql = fmt::format(
            "SELECT count(*) FROM {} "
            "WHERE mmsearch(seq0, seq1, aligner_options()) "
            "  AND bioseq.query_start(seq0) = 0 "
            "  AND bioseq.target_start(seq0) = 0 "
            "  AND bioseq.query_end(seq0) = bioseq.len(seq1) "
            "  AND bioseq.target_end(seq0) = bioseq.len(seq0)",
            kMmsearchInputTable);
        const auto generated = timed_query(
            sql, kMmsearchStatementsPerSample);
        output.result_value = generated.first;
        output.generated_query_ns = generated.second;
        output.query_ns = output.generated_query_ns;
        output.performance_gate_passed =
            output.generated_query_ns
                <= input.qualification.mmsearch_arity_3_ns_limit
            ? 1 : 0;
    } else if (algorithm == "row_codec_generated_vs_handwritten") {
        output.generated_row_codec_fields =
            kComparedRowCodecFieldCount;
        const auto generated_scalar = timed_query(
            input.generated_scalar_row_codec_sql, 1);
        const auto handwritten_scalar = timed_query(
            input.handwritten_scalar_row_codec_sql, 1);
        if (generated_scalar.first != handwritten_scalar.first) {
            fail(fmt::format(
                "scalar row-codec correctness mismatch: "
                "generated={} handwritten={}",
                generated_scalar.first,
                handwritten_scalar.first));
        }
        const auto generated_by_reference = timed_query(
            input.generated_by_reference_row_codec_sql, 1);
        const auto handwritten_by_reference = timed_query(
            input.handwritten_by_reference_row_codec_sql, 1);
        if (generated_by_reference.first
                != handwritten_by_reference.first) {
            fail(fmt::format(
                "by-reference row-codec correctness mismatch: "
                "generated={} handwritten={}",
                generated_by_reference.first,
                handwritten_by_reference.first));
        }
        if (generated_scalar.second
                > std::numeric_limits<std::uint64_t>::max()
                    - generated_by_reference.second
            || handwritten_scalar.second
                > std::numeric_limits<std::uint64_t>::max()
                    - handwritten_by_reference.second) {
            fail("row-codec comparator duration sum overflow");
        }
        output.result_value = 1;
        output.expected_value = 1;
        output.generated_query_ns =
            generated_scalar.second + generated_by_reference.second;
        output.handwritten_query_ns =
            handwritten_scalar.second
            + handwritten_by_reference.second;
        output.query_ns = output.generated_query_ns;
        output.generated_to_handwritten_ratio =
            static_cast<double>(output.generated_query_ns)
            / static_cast<double>(output.handwritten_query_ns);
        output.performance_gate_passed =
            output.generated_to_handwritten_ratio
                <= input.qualification.row_codec_ratio_limit
            ? 1 : 0;
    } else {
        fail(fmt::format(
            "unknown benchmark case: {}",
            algorithm));
    }

    if (output.result_value != output.expected_value) {
        fail(fmt::format(
            "correctness failure for {}: observed={} expected={}",
            algorithm,
            output.result_value,
            output.expected_value));
    }
    output.correctness_passed =
        algorithm == "qualification_reference" ? 0 : 1;
    if (algorithm
            == "typed_wrapper_generated_vs_handwritten"
        || algorithm
            == "row_codec_generated_vs_handwritten") {
        paired_runtime_sample_writer().append(
            algorithm, output);
    }
    return output;
}

[[nodiscard]] std::map<std::string, double> evaluate(
        std::string_view,
        const bf::ParamMap&,
        const SqlCodegenGuardrailInput&,
        const SqlCodegenGuardrailOutput& output) {
    return {
        {"query_ns", static_cast<double>(output.query_ns)},
        {"generated_query_ns",
         static_cast<double>(output.generated_query_ns)},
        {"handwritten_query_ns",
         static_cast<double>(output.handwritten_query_ns)},
        {"generated_to_handwritten_ratio",
         output.generated_to_handwritten_ratio},
        {"correctness_passed",
         static_cast<double>(output.correctness_passed)},
        {"arity", static_cast<double>(output.arity)},
        {"generated_typed_wrapper",
         static_cast<double>(output.generated_typed_wrapper)},
        {"direct_fmgr_wrapper_path",
         static_cast<double>(output.direct_fmgr_wrapper_path)},
        {"planner_custom_scan_path",
         static_cast<double>(output.planner_custom_scan_path)},
        {"generated_row_codec_fields",
         static_cast<double>(output.generated_row_codec_fields)},
        {"performance_gate_passed",
         static_cast<double>(output.performance_gate_passed)},
        {"build_qualification_passed",
         static_cast<double>(output.build_qualification_passed)},
        {"qualification_baseline_kind",
         static_cast<double>(output.qualification_baseline_kind)},
        {"live_measurement_attested",
         static_cast<double>(output.live_measurement_attested)},
        {"run_nonce", static_cast<double>(output.run_nonce)},
        {"generator_cold_ns",
         static_cast<double>(output.generator_cold_ns)},
        {"generator_noop_ns",
         static_cast<double>(output.generator_noop_ns)},
        {"generator_peak_rss_bytes",
         static_cast<double>(output.generator_peak_rss_bytes)},
        {"compile_postgresql_bioseq_ns",
         static_cast<double>(output.compile_postgresql_bioseq_ns)},
        {"compile_bioseq_bench_e2e_ns",
         static_cast<double>(output.compile_bioseq_bench_e2e_ns)},
        {"compile_postgresql_bioseq_delta_percent",
         output.compile_postgresql_bioseq_delta_percent},
        {"compile_bioseq_bench_e2e_delta_percent",
         output.compile_bioseq_bench_e2e_delta_percent},
        {"generated_sql_bytes",
         static_cast<double>(output.generated_sql.bytes)},
        {"generated_sql_files",
         static_cast<double>(output.generated_sql.files)},
        {"generated_json_bytes",
         static_cast<double>(output.generated_json.bytes)},
        {"generated_json_files",
         static_cast<double>(output.generated_json.files)},
        {"generated_cpp_bytes",
         static_cast<double>(output.generated_cpp.bytes)},
        {"generated_cpp_files",
         static_cast<double>(output.generated_cpp.files)},
    };
}

}  // namespace

export namespace bioseq::bench {

[[nodiscard]] const char* sql_codegen_guardrail_configuration() {
    const char* calibration =
        std::getenv("BIOSEQ_SQL_GUARDRAIL_CALIBRATION");
    return calibration != nullptr
            && std::string_view{calibration} == "1"
        ? BIOSEQ_SQL_GUARDRAIL_CALIBRATION_CONFIG_PATH
        : BIOSEQ_SQL_GUARDRAIL_CONFIG_PATH;
}

[[nodiscard]] bioseq::bench_framework::ErasedBenchmark
make_sql_codegen_guardrail_benchmark() {
    return bioseq::bench_framework::create_benchmark<
        SqlCodegenGuardrailInput,
        SqlCodegenGuardrailOutput>(
        "Generated PostgreSQL boundary and planner/row-codec guardrail",
        "bench_sql_codegen_guardrail",
        sql_codegen_guardrail_configuration(),
        estimate_resources,
        make_input,
        run_case,
        evaluate,
        cleanup_input);
}

}  // namespace bioseq::bench
