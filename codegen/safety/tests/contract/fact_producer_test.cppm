module;

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Program.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef BIOSEQ_PREPARE_TEST_CLANG_BINARY
#error "Safety fact contracts require the configured Clang binary"
#endif

export module bioseq.codegen.safety:tests.contract.fact_producer;

import :fact_producer;
import :serialization;
import bioseq.codegen.support;

export namespace bioseq::codegen::safety {

namespace fact_contract_detail {

[[nodiscard]] inline const function_flow& require_function(
    const std::vector<function_flow>& functions,
    std::string_view name) {
    const auto found = std::ranges::find(
        functions, name, &function_flow::qualified_name);
    if (found == functions.end()) {
        throw std::runtime_error(
            "compiler-fact contract lacks function "
            + std::string{name});
    }
    return *found;
}

template <class p_operation>
inline void require_scan_failure(
    p_operation&& operation,
    std::string_view expected) {
    try {
        operation();
    } catch (const std::runtime_error& error) {
        if (std::string_view{error.what()}.contains(expected)) {
            return;
        }
        throw std::runtime_error(
            "bridge contract failed with unexpected "
            "diagnostic: " + std::string{error.what()});
    }
    throw std::runtime_error(
        "bridge contract accepted adversarial source: "
        + std::string{expected});
}

[[nodiscard]] inline std::string replace_once(
    std::string_view source,
    std::string_view needle,
    std::string_view replacement) {
    std::string result{source};
    const std::size_t offset = result.find(needle);
    if (offset == std::string::npos
        || result.find(needle, offset + needle.size())
            != std::string::npos) {
        throw std::runtime_error(
            "bridge mutation does not have one exact target");
    }
    result.replace(offset, needle.size(), replacement);
    return result;
}

[[nodiscard]] inline source_range fixture_range(
    std::size_t begin,
    std::size_t end) {
    return {
        .begin = {
            .byte_offset = begin,
            .line = 1,
            .column = 1,
        },
        .end = {
            .byte_offset = end,
            .line = 1,
            .column = 2,
        },
    };
}

class temporary_bridge_root final {
public:
    temporary_bridge_root() {
        const std::filesystem::path temporary_directory =
            std::filesystem::temp_directory_path();
        const auto nonce =
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();
        for (std::size_t attempt = 0; attempt < 1024; ++attempt) {
            const std::filesystem::path candidate =
                temporary_directory
                / ("bioseq_bridge_contract_"
                    + std::to_string(nonce)
                    + "_" + std::to_string(attempt));
            std::error_code create_error;
            if (std::filesystem::create_directory(
                    candidate, create_error)) {
                root_ = candidate;
                return;
            }
            if (create_error
                && create_error
                    != std::errc::file_exists) {
                throw std::runtime_error(
                    "cannot create bridge-contract temporary root: "
                    + create_error.message());
            }
        }
        throw std::runtime_error(
            "cannot allocate a unique bridge-contract temporary root");
    }

    temporary_bridge_root(const temporary_bridge_root&) = delete;
    temporary_bridge_root& operator=(
        const temporary_bridge_root&) = delete;

    ~temporary_bridge_root() {
        std::error_code cleanup_error;
        std::filesystem::remove_all(root_, cleanup_error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return root_;
    }

private:
    std::filesystem::path root_;
};

struct bridge_fixture {
    std::unique_ptr<temporary_bridge_root> root_owner;
    std::filesystem::path root;
    std::filesystem::path header_root;
    provenance_bundle provenance;
    std::vector<std::string> arguments;
    std::vector<std::string> implementation_arguments;
};

[[nodiscard]] inline std::filesystem::path
compile_bridge_primary_interface(
    const std::filesystem::path& root) {
    static constexpr std::string_view source = R"cpp(
export module bioseq.postgres.sql_boundary;
export namespace bioseq::postgres::support {
class subtransaction final {
public:
    void commit();
};
}
)cpp";
    const std::filesystem::path source_path =
        root / "common/postgres/sql_boundary/primary_interface.cppm";
    const std::filesystem::path pcm_path =
        root / "pcm/bioseq.postgres.sql_boundary.pcm";
    const std::filesystem::path diagnostics_path =
        root / "pcm/bioseq.postgres.sql_boundary.stderr";
    bioseq::codegen::publish_artifact_if_changed(source_path, source);
    std::filesystem::create_directories(pcm_path.parent_path());

    const std::string clang_binary = BIOSEQ_PREPARE_TEST_CLANG_BINARY;
    const std::vector<std::string> arguments{
        clang_binary,
        "-std=c++26",
        "-Xclang",
        "-fbuiltin-headers-in-system-modules",
        "-x",
        "c++-module",
        "--precompile",
        source_path.string(),
        "-o",
        pcm_path.string(),
    };
    std::vector<llvm::StringRef> argument_refs;
    argument_refs.reserve(arguments.size());
    for (const std::string& argument : arguments) {
        argument_refs.emplace_back(argument);
    }
    const std::string diagnostics_storage = diagnostics_path.string();
    const std::array<std::optional<llvm::StringRef>, 3> redirects{
        std::nullopt,
        std::nullopt,
        llvm::StringRef{diagnostics_storage},
    };
    std::string execution_error;
    bool execution_failed = false;
    const int status = llvm::sys::ExecuteAndWait(
        clang_binary,
        argument_refs,
        std::nullopt,
        llvm::ArrayRef<std::optional<llvm::StringRef>>{redirects},
        30,
        0,
        &execution_error,
        &execution_failed);
    std::error_code size_error;
    const std::uintmax_t pcm_size =
        std::filesystem::file_size(pcm_path, size_error);
    if (execution_failed || status != 0 || size_error || pcm_size == 0) {
        const std::string diagnostics =
            std::filesystem::is_regular_file(diagnostics_path)
            ? bioseq::codegen::read_artifact_bytes(diagnostics_path)
            : std::string{};
        throw std::runtime_error(
            "cannot compile bridge-contract primary interface BMI: status="
            + std::to_string(status)
            + (execution_error.empty()
                ? std::string{}
                : ", execution=" + execution_error)
            + (diagnostics.empty()
                ? std::string{}
                : ", diagnostics=" + diagnostics));
    }
    return pcm_path;
}

[[nodiscard]] inline bridge_fixture make_bridge_fixture() {
    static constexpr std::string_view header = R"cpp(#pragma once
struct ErrorData {};
void pg_contract_leaf();
void BeginInternalSubTransaction(void*);
void ReleaseCurrentSubTransaction();
void RollbackAndReleaseCurrentSubTransaction();
[[noreturn]] void ReThrowError(ErrorData*);
#define PG_TRY() do { if (true) {
#define PG_CATCH() } else {
#define PG_END_TRY() } } while (false)
)cpp";
    static constexpr std::string_view authority_header =
        "#pragma once\nvoid pfree(void*);\n";
    static constexpr std::string_view exception_header =
        "#pragma once\nnamespace std { struct exception {}; }\n";
    static constexpr std::string_view functional_header =
        R"cpp(#pragma once
namespace std {
template<class p_function>
auto invoke(p_function& call) -> decltype(call()) {
    return call();
}
}
)cpp";
    static constexpr std::string_view string_view_header =
        R"cpp(#pragma once
namespace std {
template<class p_character>
struct basic_string_view {
    constexpr basic_string_view(const p_character*) noexcept {}
};
using string_view = basic_string_view<char>;
}
)cpp";
    static constexpr std::string_view stdexcept_header =
        R"cpp(#pragma once
#include <exception>
namespace std {
struct runtime_error : exception {
    explicit runtime_error(const char*) {}
};
}
)cpp";
    auto root_owner = std::make_unique<temporary_bridge_root>();
    const std::filesystem::path root = root_owner->path();
    const std::filesystem::path header_root =
        root / "postgresql_include";
    const std::filesystem::path standard_root =
        root / "cxx_include";
    std::error_code directory_error;
    std::filesystem::create_directories(
        header_root / "utils", directory_error);
    if (directory_error) {
        throw std::runtime_error(
            "cannot create bridge-contract headers");
    }
    std::filesystem::create_directories(
        standard_root, directory_error);
    if (directory_error) {
        throw std::runtime_error(
            "cannot create bridge-contract standard headers");
    }
    bioseq::codegen::publish_artifact_if_changed(
        header_root / "utils/elog.h", header);
    bioseq::codegen::publish_artifact_if_changed(
        header_root / "utils/palloc.h", authority_header);
    bioseq::codegen::publish_artifact_if_changed(
        standard_root / "exception", exception_header);
    bioseq::codegen::publish_artifact_if_changed(
        standard_root / "functional", functional_header);
    bioseq::codegen::publish_artifact_if_changed(
        standard_root / "string_view", string_view_header);
    bioseq::codegen::publish_artifact_if_changed(
        standard_root / "stdexcept", stdexcept_header);
    const std::size_t macros_begin =
        header.find("#define PG_TRY");
    const std::size_t macros_end =
        header.find('\n', header.find("#define PG_END_TRY"));
    const std::size_t rethrow_begin =
        header.find("[[noreturn]] void ReThrowError");
    const std::size_t rethrow_end =
        header.find('\n', rethrow_begin);
    if (macros_begin == std::string_view::npos
        || macros_end == std::string_view::npos
        || rethrow_begin == std::string_view::npos
        || rethrow_end == std::string_view::npos) {
        throw std::runtime_error(
            "bridge fixture spans are unavailable");
    }
    const std::string whole_sha =
        bioseq::common::sha256_hex(header);
    provenance_bundle provenance{
        .postgresql_tag = "REL_17_10",
        .postgresql_revision =
            "25c49f3a4a742ba283f5cc43cc7f1d361552e917",
        .records = {
            {
                .record_id = "pg17.pg_try_rethrow_macros",
                .kind = provenance_kind::bridge_primitive,
                .spelling = "PG_TRY/PG_CATCH/PG_END_TRY",
                .upstream_file = "src/include/utils/elog.h",
                .upstream_span = fixture_range(
                    macros_begin, macros_end),
                .upstream_file_sha256 = whole_sha,
                .fragment_sha256 =
                    bioseq::common::sha256_hex(
                        header.substr(
                            macros_begin,
                            macros_end - macros_begin)),
                .rationale =
                    "Authenticates contract PG exception macros.",
            },
            {
                .record_id = "pg17.rethrow_declarations",
                .kind = provenance_kind::bridge_primitive,
                .spelling = "ReThrowError",
                .upstream_file = "src/include/utils/elog.h",
                .upstream_span = fixture_range(
                    rethrow_begin, rethrow_end),
                .upstream_file_sha256 = whole_sha,
                .fragment_sha256 =
                    bioseq::common::sha256_hex(
                        header.substr(
                            rethrow_begin,
                            rethrow_end - rethrow_begin)),
                .rationale =
                    "Authenticates contract rethrow declaration.",
            },
            {
                .record_id = "pg17.palloc_header_authority",
                .kind = provenance_kind::bridge_primitive,
                .spelling = "pfree",
                .upstream_file = "src/include/utils/palloc.h",
                .upstream_span = fixture_range(
                    0, authority_header.size()),
                .upstream_file_sha256 =
                    bioseq::common::sha256_hex(
                        authority_header),
                .fragment_sha256 =
                    bioseq::common::sha256_hex(
                        authority_header),
                .rationale =
                    "Provides an independent contract header pin.",
            },
        },
    };
    const std::filesystem::path primary_module_pcm =
        compile_bridge_primary_interface(root);
    std::vector<std::string> arguments{
        "-I" + standard_root.string(),
        "-I" + header_root.string(),
        "-x",
        "c++-module",
    };
    std::vector<std::string> implementation_arguments{
        "-I" + standard_root.string(),
        "-I" + header_root.string(),
        "-x",
        "c++",
    };
    implementation_arguments.emplace_back(
        "-fmodule-file=bioseq.postgres.sql_boundary="
        + primary_module_pcm.string());
    return {
        .root_owner = std::move(root_owner),
        .root = root,
        .header_root = header_root,
        .provenance = std::move(provenance),
        .arguments = std::move(arguments),
        .implementation_arguments =
            std::move(implementation_arguments),
    };
}

[[nodiscard]] inline std::filesystem::path module_path(
    const bridge_fixture& fixture,
    std::string_view suffix) {
    const std::filesystem::path result =
        fixture.root / std::filesystem::path{suffix};
    std::error_code directory_error;
    std::filesystem::create_directories(
        result.parent_path(), directory_error);
    if (directory_error) {
        throw std::runtime_error(
            "cannot create bridge-contract module path");
    }
    return result;
}

[[nodiscard]] inline bool has_bridge_evidence(
    const std::vector<function_flow>& functions,
    bridge_kind expected) {
    return std::ranges::any_of(
        functions,
        [&](const function_flow& function) {
            return std::ranges::any_of(
                function.calls,
                [&](const flow_call_site& call) {
                    return call.bridge_transition == expected
                        && call.bridge_primitive_evidence
                            .has_value();
                });
        });
}

inline void run_bridge_contracts() {
    bridge_fixture fixture = make_bridge_fixture();
    const std::filesystem::path context_path = module_path(
        fixture,
        "common/postgres/sql_boundary/call_context_implementation.cppm");
    const std::filesystem::path error_boundary_path = module_path(
        fixture,
        "common/postgres/support/error_boundary.cppm");
    const std::filesystem::path boundary_path = module_path(
        fixture,
        "common/postgres/sql_boundary/index.cppm");
    const auto scan = [&](
        const std::filesystem::path& path,
        std::string_view source,
        std::string_view profile) {
        return scan_safety_flow_from_code(
            path,
            source,
            fixture.arguments,
            profile,
            std::addressof(fixture.provenance));
    };
    const auto scan_context = [&context_path, &fixture](
        std::string_view source,
        std::string_view profile) {
        return scan_safety_flow_from_code(
            context_path,
            source,
            fixture.implementation_arguments,
            profile,
            std::addressof(fixture.provenance));
    };

    static constexpr std::string_view subtransaction_positive =
        R"cpp(module;
#include <utils/elog.h>
module bioseq.postgres.sql_boundary;
namespace bioseq::postgres::support {
void subtransaction::commit() {
    PG_TRY();
    {
        ReleaseCurrentSubTransaction();
    }
    PG_CATCH();
    {
    }
    PG_END_TRY();
}
}
)cpp";
    if (!has_bridge_evidence(
            scan_context(
                subtransaction_positive,
                "bridge-subtransaction-positive"),
            bridge_kind::subtransaction)) {
        throw std::runtime_error(
            "positive SQL-subtransaction bridge evidence is absent");
    }

    static constexpr std::string_view subtransaction_decoy =
        R"cpp(module;
#include <utils/elog.h>
module bioseq.postgres.sql_boundary;
namespace bioseq::postgres::support {
void subtransaction::commit() {
    /* PG_TRY */
    ReleaseCurrentSubTransaction();
    static constexpr const char* decoy = "PG_CATCH";
    (void) decoy;
}
}
)cpp";
    require_scan_failure(
        [&] {
            static_cast<void>(scan_context(
                subtransaction_decoy,
                "bridge-subtransaction-decoy"));
        },
        "no unique authenticated PG_TRY region");

    static constexpr std::string_view subtransaction_multiple =
        R"cpp(module;
#include <utils/elog.h>
module bioseq.postgres.sql_boundary;
namespace bioseq::postgres::support {
void subtransaction::commit() {
    PG_TRY();
    {
    }
    PG_CATCH();
    {
    }
    PG_END_TRY();
    PG_TRY();
    {
        ReleaseCurrentSubTransaction();
    }
    PG_CATCH();
    {
    }
    PG_END_TRY();
}
}
)cpp";
    require_scan_failure(
        [&] {
            static_cast<void>(scan_context(
                subtransaction_multiple,
                "bridge-subtransaction-multiple"));
        },
        "no unique authenticated PG_TRY region");

    static constexpr std::string_view subtransaction_redefined =
        R"cpp(module;
#include <utils/elog.h>
#undef PG_TRY
#undef PG_CATCH
#undef PG_END_TRY
#define PG_TRY() do { if (true) {
#define PG_CATCH() } else {
#define PG_END_TRY() } } while (false)
module bioseq.postgres.sql_boundary;
namespace bioseq::postgres::support {
void subtransaction::commit() {
    PG_TRY();
    {
        ReleaseCurrentSubTransaction();
    }
    PG_CATCH();
    {
    }
    PG_END_TRY();
}
}
)cpp";
    require_scan_failure(
        [&] {
            static_cast<void>(scan_context(
                subtransaction_redefined,
                "bridge-subtransaction-redefined"));
        },
        "active PG macro definition is not pinned");

    static constexpr std::string_view subtransaction_wrapped =
        R"cpp(module;
#include <utils/elog.h>
#define PROJECT_TRY() PG_TRY()
module bioseq.postgres.sql_boundary;
namespace bioseq::postgres::support {
void subtransaction::commit() {
    PROJECT_TRY();
    {
        ReleaseCurrentSubTransaction();
    }
    PG_CATCH();
    {
    }
    PG_END_TRY();
}
}
)cpp";
    require_scan_failure(
        [&] {
            static_cast<void>(scan_context(
                subtransaction_wrapped,
                "bridge-subtransaction-wrapped"));
        },
        "active PG macro definition is not pinned");

    static constexpr std::string_view subtransaction_catch =
        R"cpp(module;
#include <utils/elog.h>
module bioseq.postgres.sql_boundary;
namespace bioseq::postgres::support {
void subtransaction::commit() {
    PG_TRY();
    {
    }
    PG_CATCH();
    {
        ReleaseCurrentSubTransaction();
    }
    PG_END_TRY();
}
}
)cpp";
    require_scan_failure(
        [&] {
            static_cast<void>(scan_context(
                subtransaction_catch,
                "bridge-subtransaction-catch"));
        },
        "transition is outside authenticated PG_TRY then branch");

    static constexpr std::string_view invoke_positive =
        R"cpp(module;
#include <utils/elog.h>
export module bioseq.postgres.support:error_boundary;
export namespace bioseq::postgres::support {
using postgres_callback_body = void (*)(void*);
struct postgres_invoke_state {
    void* callback_context;
    postgres_callback_body body;
    ErrorData* postgres_error_data;
    bool error_capture_failed;
};
using MemoryContext = void*;
using ResourceOwner = void*;
ErrorData* CopyErrorData();
MemoryContext MemoryContextSwitchTo(MemoryContext);
void FlushErrorState();
struct postgres_error_capture_failure {};
void capture_current_postgres_error_as_cpp(
        postgres_invoke_state* state,
        MemoryContext error_owner,
        MemoryContext restore_context,
        ResourceOwner) {
    MemoryContextSwitchTo(error_owner);
    PG_TRY();
    {
        state->postgres_error_data = CopyErrorData();
        MemoryContextSwitchTo(restore_context);
        FlushErrorState();
    }
    PG_CATCH();
    {
        state->error_capture_failed = true;
        MemoryContextSwitchTo(restore_context);
        FlushErrorState();
    }
    PG_END_TRY();
}
inline void invoke_pg_as_cpp_erased(
        void* callback_context,
        postgres_callback_body body) {
    auto* state = new postgres_invoke_state{
        callback_context, body, nullptr, false};
    PG_TRY();
    {
        if (state->body != nullptr) {
            state->body(state->callback_context);
        }
    }
    PG_CATCH();
    {
        capture_current_postgres_error_as_cpp(
            state, nullptr, nullptr, nullptr);
    }
    PG_END_TRY();
    if (state->error_capture_failed) {
        delete state;
        throw postgres_error_capture_failure{};
    }
    delete state;
}
template<class p_function>
void invoke_pg_as_cpp(p_function&& function) {
    struct call_state {
        p_function* function;
    } state{&function};
    invoke_pg_as_cpp_erased(
        &state,
        +[](void* opaque) {
            auto* call = static_cast<call_state*>(opaque);
            (*call->function)();
        });
}
void invoke_root() {
    invoke_pg_as_cpp([] { pg_contract_leaf(); });
}
}
)cpp";
    if (!has_bridge_evidence(
            scan(
                error_boundary_path,
                invoke_positive,
                "bridge-invoke-positive"),
            bridge_kind::invoke_pg_as_cpp)) {
        throw std::runtime_error(
            "positive invoke_pg_as_cpp bridge evidence is absent");
    }
    require_scan_failure(
        [&] {
            const std::string mutation = replace_once(
                invoke_positive,
                "inline void invoke_pg_as_cpp_erased(",
                "void invoke_pg_as_cpp_erased(");
            static_cast<void>(scan(
                error_boundary_path,
                mutation,
                "bridge-invoke-erased-not-inline"));
        },
        "erased bridge definition is not explicitly inline");
    require_scan_failure(
        [&] {
            const std::string mutation = replace_once(
                invoke_positive,
                "state->postgres_error_data = CopyErrorData();",
                "state->postgres_error_data = nullptr;");
            static_cast<void>(scan(
                error_boundary_path,
                mutation,
                "bridge-invoke-copy-unguarded"));
        },
        "error-capture helper call inventory differs");
    require_scan_failure(
        [&] {
            const std::string mutation = replace_once(
                invoke_positive,
                "state->error_capture_failed = true;",
                "state->error_capture_failed = false;");
            static_cast<void>(scan(
                error_boundary_path,
                mutation,
                "bridge-invoke-capture-flag-cleared"));
        },
        "CopyErrorData/FlushErrorState recovery is not exact");
    require_scan_failure(
        [&] {
            const std::string mutation = replace_once(
                invoke_positive,
                "MemoryContextSwitchTo(restore_context);\n"
                "        FlushErrorState();\n"
                "    }\n"
                "    PG_CATCH();",
                "MemoryContextSwitchTo(restore_context);\n"
                "    }\n"
                "    PG_CATCH();");
            static_cast<void>(scan(
                error_boundary_path,
                mutation,
                "bridge-invoke-success-flush-removed"));
        },
        "error-capture helper call inventory differs");
    require_scan_failure(
        [&] {
            const std::string mutation = replace_once(
                invoke_positive,
                "throw postgres_error_capture_failure{};",
                "(void) state;");
            static_cast<void>(scan(
                error_boundary_path,
                mutation,
                "bridge-invoke-cpp-fallback-removed"));
        },
        "error-capture failure is not converted to C++");

    static constexpr std::string_view call_context_positive =
        R"cpp(module;
#include <utils/elog.h>
export module bioseq.postgres.support:error_boundary;
export namespace bioseq::postgres::support {
using postgres_callback_body = void (*)(void*);
struct postgres_invoke_state {
    void* callback_context;
    postgres_callback_body body;
    ErrorData* postgres_error_data;
    bool error_capture_failed;
};
using MemoryContext = void*;
using ResourceOwner = void*;
ErrorData* CopyErrorData();
MemoryContext MemoryContextSwitchTo(MemoryContext);
void FlushErrorState();
struct postgres_error_capture_failure {};
void capture_current_postgres_error_as_cpp(
        postgres_invoke_state* state,
        MemoryContext error_owner,
        MemoryContext restore_context,
        ResourceOwner) {
    MemoryContextSwitchTo(error_owner);
    PG_TRY();
    {
        state->postgres_error_data = CopyErrorData();
        MemoryContextSwitchTo(restore_context);
        FlushErrorState();
    }
    PG_CATCH();
    {
        state->error_capture_failed = true;
        MemoryContextSwitchTo(restore_context);
        FlushErrorState();
    }
    PG_END_TRY();
}
inline void invoke_pg_across_transactions_as_cpp_erased(
        void* callback_context,
        postgres_callback_body body) {
    auto* state = new postgres_invoke_state{
        callback_context, body, nullptr, false};
    PG_TRY();
    {
        if (state->body != nullptr) {
            state->body(state->callback_context);
        }
    }
    PG_CATCH();
    {
        capture_current_postgres_error_as_cpp(
            state, nullptr, nullptr, nullptr);
    }
    PG_END_TRY();
    if (state->error_capture_failed) {
        delete state;
        throw postgres_error_capture_failure{};
    }
    delete state;
}
template<class p_function>
void invoke_pg_across_transactions_as_cpp(p_function&& function) {
    struct call_state {
        p_function* function;
    } state{&function};
    invoke_pg_across_transactions_as_cpp_erased(
        &state,
        +[](void* opaque) {
            auto* call = static_cast<call_state*>(opaque);
            (*call->function)();
        });
}
void call_context_root() {
    invoke_pg_across_transactions_as_cpp(
        [] { pg_contract_leaf(); });
}
}
)cpp";
    if (!has_bridge_evidence(
            scan(
                error_boundary_path,
                call_context_positive,
                "bridge-call-context-positive"),
            bridge_kind::cross_transaction_callback)) {
        throw std::runtime_error(
            "positive SQL-call-context bridge evidence is absent");
    }

    static constexpr std::string_view invoke_token_decoy =
        R"cpp(module;
#include <utils/elog.h>
export module bioseq.postgres.support:error_boundary;
export namespace bioseq::postgres::support {
template<class p_function>
void invoke_pg_as_cpp(p_function&& function) {
    static constexpr const char* decoy =
        "invoke_pg_as_cpp_erased";
    (void) decoy;
    (void) sizeof(function);
}
void invoke_root() {
    invoke_pg_as_cpp([] { pg_contract_leaf(); });
}
}
)cpp";
    require_scan_failure(
        [&] {
            static_cast<void>(scan(
                error_boundary_path,
                invoke_token_decoy,
                "bridge-invoke-token-decoy"));
        },
        "bridge wrapper has no exact single erased call");

    static constexpr std::string_view invoke_unprotected_erased =
        R"cpp(module;
#include <utils/elog.h>
export module bioseq.postgres.support:error_boundary;
export namespace bioseq::postgres::support {
using postgres_callback_body = void (*)(void*);
struct postgres_invoke_state {
    void* callback_context;
    postgres_callback_body body;
};
inline void invoke_pg_as_cpp_erased(
        void* callback_context,
        postgres_callback_body body) {
    auto* state = new postgres_invoke_state{
        callback_context, body};
    PG_TRY();
    {
    }
    PG_CATCH();
    {
    }
    PG_END_TRY();
    state->body(state->callback_context);
    delete state;
}
template<class p_function>
void invoke_pg_as_cpp(p_function&& function) {
    struct call_state {
        p_function* function;
    } state{&function};
    invoke_pg_as_cpp_erased(
        &state,
        +[](void* opaque) {
            auto* call = static_cast<call_state*>(opaque);
            (*call->function)();
        });
}
void invoke_root() {
    invoke_pg_as_cpp([] { pg_contract_leaf(); });
}
}
)cpp";
    require_scan_failure(
        [&] {
            static_cast<void>(scan(
                error_boundary_path,
                invoke_unprotected_erased,
                "bridge-invoke-unprotected-erased"));
        },
        "erased callback is outside authenticated PG_TRY "
        "then branch");

    static constexpr std::string_view
        invoke_constexpr_init_escape =
        R"cpp(module;
#include <utils/elog.h>
export module bioseq.postgres.support:error_boundary;
export namespace bioseq::postgres::support {
using postgres_callback_body = void (*)(void*);
struct postgres_invoke_state {
    void* callback_context;
    postgres_callback_body body;
};
inline void invoke_pg_as_cpp_erased(
        void* callback_context,
        postgres_callback_body body) {
    auto* state = new postgres_invoke_state{
        callback_context, body};
    PG_TRY();
    {
        state->body(state->callback_context);
    }
    PG_CATCH();
    {
    }
    PG_END_TRY();
    delete state;
}
template<class p_function>
void invoke_pg_as_cpp(p_function&& function) {
    if constexpr (
        int escaped = (function(), 0);
        true) {
        (void) escaped;
    }
    struct call_state {
        p_function* function;
    } state{&function};
    invoke_pg_as_cpp_erased(
        &state,
        +[](void* opaque) {
            auto* call = static_cast<call_state*>(opaque);
            (*call->function)();
        });
}
void invoke_root() {
    invoke_pg_as_cpp([] { pg_contract_leaf(); });
}
}
)cpp";
    require_scan_failure(
        [&] {
            static_cast<void>(scan(
                error_boundary_path,
                invoke_constexpr_init_escape,
                "bridge-invoke-constexpr-init-escape"));
        },
        "bridge wrapper calls its callable outside the exact "
        "erased trampoline");

    static constexpr std::string_view tail_positive =
        R"cpp(module;
#include <exception>
#include <functional>
#include <utils/elog.h>
export module bioseq.postgres.sql_boundary;
export namespace bioseq::postgres::support {
struct postgres_error {};
struct postgres_error_capture_failure {};
template<class p_function>
int invoke_postgres_callback(p_function&& call) {
    ErrorData* exact_error = nullptr;
    try {
        return std::invoke(call);
    } catch (postgres_error&) {
    } catch (const postgres_error_capture_failure&) {
    } catch (const std::exception&) {
    } catch (...) {
    }
    ReThrowError(exact_error);
}
int boundary_root() {
    return invoke_postgres_callback([] { return 0; });
}
}
)cpp";
    if (!has_bridge_evidence(
            scan(
                boundary_path,
                tail_positive,
                "bridge-tail-positive"),
            bridge_kind::generated_boundary_tail)) {
        throw std::runtime_error(
            "positive generated-tail bridge evidence is absent");
    }

    static constexpr std::string_view tail_token_decoy =
        R"cpp(module;
#include <utils/elog.h>
export module bioseq.postgres.sql_boundary;
export namespace bioseq::postgres::support {
template<class p_function>
int invoke_postgres_callback(p_function&&) {
    static constexpr const char* decoy =
        "postgres_error";
    (void) decoy;
    ReThrowError(nullptr);
}
int boundary_root() {
    return invoke_postgres_callback([] { return 0; });
}
}
)cpp";
    require_scan_failure(
        [&] {
            static_cast<void>(scan(
                boundary_path,
                tail_token_decoy,
                "bridge-tail-token-decoy"));
        },
        "generated tail is not after exact catch/unwind region");

    static constexpr std::string_view tail_inside_handler =
        R"cpp(module;
#include <exception>
#include <functional>
#include <utils/elog.h>
export module bioseq.postgres.sql_boundary;
export namespace bioseq::postgres::support {
struct postgres_error {};
template<class p_function>
int invoke_postgres_callback(p_function&& call) {
    try {
        return std::invoke(call);
    } catch (postgres_error&) {
        ReThrowError(nullptr);
    } catch (const std::exception&) {
    } catch (...) {
    }
    return 0;
}
int boundary_root() {
    return invoke_postgres_callback([] { return 0; });
}
}
)cpp";
    require_scan_failure(
        [&] {
            static_cast<void>(scan(
                boundary_path,
                tail_inside_handler,
                "bridge-tail-inside-handler"));
        },
        "generated tail is not after exact catch/unwind region");

    static constexpr std::string_view tail_wrong_handlers =
        R"cpp(module;
#include <exception>
#include <functional>
#include <utils/elog.h>
export module bioseq.postgres.sql_boundary;
export namespace bioseq::postgres::support {
struct postgres_error {};
template<class p_function>
int invoke_postgres_callback(p_function&& call) {
    try {
        return std::invoke(call);
    } catch (postgres_error) {
    } catch (std::exception&) {
    } catch (...) {
    }
    ReThrowError(nullptr);
}
int boundary_root() {
    return invoke_postgres_callback([] { return 0; });
}
}
)cpp";
    require_scan_failure(
        [&] {
            static_cast<void>(scan(
                boundary_path,
                tail_wrong_handlers,
                "bridge-tail-wrong-handlers"));
        },
        "generated tail is not after exact catch/unwind region");
}

[[nodiscard]] inline const runtime_probe_fact& require_runtime_probe(
    const isolated_compiler_fact_scan& facts,
    std::string_view key,
    runtime_relation relation) {
    const auto found = std::ranges::find(
        facts.runtime_probes,
        key,
        &runtime_probe_fact::fault_key);
    if (found == facts.runtime_probes.end()
        || found->relation != relation) {
        throw std::runtime_error(
            "compiler-fact contract lacks runtime probe "
            + std::string{key});
    }
    return *found;
}

inline void run_runtime_probe_contracts() {
    bridge_fixture fixture = make_bridge_fixture();
    const std::filesystem::path path = module_path(
        fixture,
        "bioseq/runtime_probe_contract.cppm");
    const std::filesystem::path boundary_path = module_path(
        fixture,
        "common/postgres/sql_boundary/index.cppm");
    const auto scan = [&](
        std::string_view source,
        std::string_view profile) {
        return scan_safety_facts_from_code(
            path,
            source,
            fixture.arguments,
            profile,
            std::addressof(fixture.provenance));
    };
    static constexpr std::string_view positive = R"cpp(module;
#include <utils/elog.h>
export module bioseq.runtime_probe.contract;
namespace bioseq::postgres::support::safety_runtime_probe {
void record_site_visit(const char*) noexcept;
bool consume_fault(const char*) noexcept;
}
using bioseq::postgres::support::safety_runtime_probe::consume_fault;
using bioseq::postgres::support::safety_runtime_probe::record_site_visit;
void natural_probe() {
    record_site_visit("fixture.natural");
    pg_contract_leaf();
}
void routed_probe() {
    if (consume_fault("fixture.routes")) {
        record_site_visit("fixture.routes");
        pg_contract_leaf();
    }
}
void after_success_probe() {
    pg_contract_leaf();
    record_site_visit("fixture.after");
    if (consume_fault("fixture.after")) {
        ReThrowError(nullptr);
    }
}
)cpp";
    const isolated_compiler_fact_scan first =
        scan(positive, "runtime-probe-positive");
    const isolated_compiler_fact_scan second =
        scan(positive, "runtime-probe-positive");
    const runtime_probe_fact& natural =
        require_runtime_probe(
            first,
            "fixture.natural",
            runtime_relation::natural_at_site);
    const runtime_probe_fact& routes =
        require_runtime_probe(
            first,
            "fixture.routes",
            runtime_relation::injected_routes_to_site);
    const runtime_probe_fact& after =
        require_runtime_probe(
            first,
            "fixture.after",
            runtime_relation::
                injected_after_site_success);
    if (first.runtime_probes.size() != 3
        || natural.trigger_call_stable_id
        || !routes.trigger_call_stable_id
        || !after.trigger_call_stable_id
        || natural.site_call_stable_id.empty()
        || natural.visit_call_stable_id.empty()
        || render_compiler_analysis_facts_json(
               compiler_analysis_facts{
                   .profile =
                       compilation_profile::production,
                   .compiler_profile = "contract",
                   .clang_version = "contract",
                   .source_list_sha256 =
                       std::string(64, '1'),
                   .compile_database_sha256 =
                       std::string(64, '2'),
                   .producer_executable_sha256 =
                       std::string(64, '3'),
                   .compile_graph_sha256 =
                       std::string(64, '4'),
                   .pcm_manifest_sha256 =
                       std::string(64, '5'),
                   .sql_manifest_sha256 =
                       std::string(64, '6'),
                   .root_usrs = {"contract"},
                   .functions = first.functions,
                   .runtime_probes =
                       first.runtime_probes,
               })
            != render_compiler_analysis_facts_json(
               compiler_analysis_facts{
                   .profile =
                       compilation_profile::production,
                   .compiler_profile = "contract",
                   .clang_version = "contract",
                   .source_list_sha256 =
                       std::string(64, '1'),
                   .compile_database_sha256 =
                       std::string(64, '2'),
                   .producer_executable_sha256 =
                       std::string(64, '3'),
                   .compile_graph_sha256 =
                       std::string(64, '4'),
                   .pcm_manifest_sha256 =
                       std::string(64, '5'),
                   .sql_manifest_sha256 =
                       std::string(64, '6'),
                   .root_usrs = {"contract"},
                   .functions = second.functions,
                   .runtime_probes =
                       second.runtime_probes,
               })) {
        throw std::runtime_error(
            "runtime-probe compiler facts are incomplete or "
            "nondeterministic");
    }

    static constexpr std::string_view throwing_member =
        R"cpp(module;
#include <utils/elog.h>
export module bioseq.runtime_probe.contract;
namespace bioseq::postgres::support::safety_runtime_probe {
void record_site_visit(const char*) noexcept;
bool consume_fault(const char*) noexcept;
}
struct throwing_member_call {
    void run();
};
void invalid_probe(throwing_member_call& member) {
    if (bioseq::postgres::support::safety_runtime_probe::
            consume_fault("fixture.throwing.member")) {
        member.run();
        bioseq::postgres::support::safety_runtime_probe::
            record_site_visit("fixture.throwing.member");
        pg_contract_leaf();
    }
}
)cpp";
    require_scan_failure(
        [&] {
            static_cast<void>(scan(
                throwing_member,
                "runtime-throwing-member"));
        },
        "does not prove exactly one supported CFG relation");

    static constexpr std::string_view sibling_exit = R"cpp(module;
#include <utils/elog.h>
export module bioseq.runtime_probe.contract;
namespace bioseq::postgres::support::safety_runtime_probe {
void record_site_visit(const char*) noexcept;
bool consume_fault(const char*) noexcept;
}
void invalid_probe(bool escape) {
    if (bioseq::postgres::support::safety_runtime_probe::
            consume_fault("fixture.sibling.exit")) {
        if (escape) return;
        bioseq::postgres::support::safety_runtime_probe::
            record_site_visit("fixture.sibling.exit");
        pg_contract_leaf();
    }
}
)cpp";
    require_scan_failure(
        [&] {
            static_cast<void>(
                scan(sibling_exit, "runtime-sibling-exit"));
        },
        "does not prove exactly one supported CFG relation");

    static constexpr std::string_view descriptor_bound = R"cpp(module;
#include <string_view>
#include <utils/elog.h>
export module bioseq.runtime_probe.contract;
namespace bioseq::postgres::support::safety_runtime_probe {
void record_site_visit(std::string_view) noexcept;
bool consume_fault(std::string_view) noexcept;
}
namespace bioseq::postgres::support {
struct sql_boundary_probe_descriptor final {
    std::string_view site_id;
    std::string_view fault_key;
};
template<class p_function>
int invoke_postgres_callback(
        sql_boundary_probe_descriptor probe,
        p_function&& call) {
    const int result = call();
    pg_contract_leaf();
    safety_runtime_probe::record_site_visit(probe.site_id);
    if (safety_runtime_probe::consume_fault(probe.fault_key)) {
        pg_contract_leaf();
    }
    return result;
}
int generated_wrapper() {
    return invoke_postgres_callback(
        sql_boundary_probe_descriptor{
            .site_id =
                "probe:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            .fault_key = "fixture.descriptor",
        },
        [] { return 0; });
}
}
)cpp";
    const isolated_compiler_fact_scan descriptor_facts =
        scan(
            descriptor_bound,
            "runtime-descriptor-binding");
    const runtime_probe_fact& descriptor_probe =
        require_runtime_probe(
            descriptor_facts,
            "fixture.descriptor",
            runtime_relation::
                injected_after_site_success);
    if (!descriptor_probe.trigger_call_stable_id) {
        throw std::runtime_error(
            "literal SQL-boundary descriptor did not bind "
            "symbolic runtime-probe arguments");
    }

    static constexpr std::string_view production_descriptor =
        R"cpp(module;
#include <functional>
#include <stdexcept>
#include <string_view>
#include <utils/elog.h>
export module bioseq.postgres.sql_boundary;
namespace bioseq::postgres::support::safety_runtime_probe {
void record_site_visit(std::string_view) noexcept;
bool consume_fault(std::string_view) noexcept;
}
namespace bioseq::postgres::support {
struct postgres_error {
    ErrorData* release_error_data() noexcept;
};
struct postgres_error_capture_failure {};
struct sql_boundary_probe_descriptor final {
    std::string_view site_id;
    std::string_view fault_key;
};
template<class p_function>
int invoke_postgres_callback(
        sql_boundary_probe_descriptor probe,
        p_function&& call) {
    ErrorData* exact_error = nullptr;
    bool standard_error = false;
    bool unknown_error = false;
    try {
        const int result = std::invoke(call);
        if (safety_runtime_probe::consume_fault(
                probe.fault_key)) {
            throw std::runtime_error{
                "injected boundary exception"};
        }
        return result;
    } catch (postgres_error& error) {
        exact_error = error.release_error_data();
    } catch (const postgres_error_capture_failure&) {
        standard_error = true;
    } catch (const std::exception&) {
        standard_error = true;
    } catch (...) {
        unknown_error = true;
    }
    if (exact_error != nullptr) {
        ReThrowError(exact_error);
    }
    safety_runtime_probe::record_site_visit(probe.site_id);
    pg_contract_leaf();
    return standard_error || unknown_error ? 1 : 0;
}
int generated_wrapper() {
    return invoke_postgres_callback(
        sql_boundary_probe_descriptor{
            .site_id =
                "probe:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
                "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
            .fault_key =
                "generated_boundary_tail.std_exception",
        },
        [] { return 0; });
}
int generated_multi_wrapper(int arity) {
    switch (arity) {
        case 1:
            return invoke_postgres_callback(
                sql_boundary_probe_descriptor{
                    .site_id =
                        "probe:cccccccccccccccccccccccccccccccc"
                        "cccccccccccccccccccccccccccccccc",
                    .fault_key =
                        "generated_boundary_tail.std_exception",
                },
                [] { return 1; });
        case 2:
            return invoke_postgres_callback(
                sql_boundary_probe_descriptor{
                    .site_id =
                        "probe:dddddddddddddddddddddddddddddddd"
                        "dddddddddddddddddddddddddddddddd",
                    .fault_key =
                        "generated_boundary_tail.std_exception",
                },
                [] { return 2; });
        default:
            pg_contract_leaf();
            __builtin_unreachable();
    }
}
}
)cpp";
    const isolated_compiler_fact_scan production_facts =
        scan_safety_facts_from_code(
            boundary_path,
            production_descriptor,
            fixture.arguments,
            "runtime-production-descriptor",
            std::addressof(fixture.provenance));
    const runtime_probe_fact& production_probe =
        require_runtime_probe(
            production_facts,
            "generated_boundary_tail.std_exception",
            runtime_relation::injected_routes_to_site);
    const function_flow& multi_wrapper =
        require_function(
            production_facts.functions,
            "bioseq::postgres::support::generated_multi_wrapper");
    const auto default_error = std::ranges::find_if(
        multi_wrapper.calls,
        [](const flow_call_site& call) {
            return std::ranges::any_of(
                call.targets,
                [](const flow_call_target& target) {
                    return target.target_qualified_name
                        == "pg_contract_leaf";
                });
        });
    const std::size_t direct_boundaries =
        std::ranges::count_if(
            multi_wrapper.calls,
            [](const flow_call_site& call) {
                return call.resolution
                        == resolution_status::direct
                    && call.target_set_complete
                    && call.targets.size() == 1
                    && call.targets.front()
                           .target_qualified_name
                        == "bioseq::postgres::support::"
                           "invoke_postgres_callback";
            });
    if (!production_probe.trigger_call_stable_id
        || production_probe.site_call_stable_id
            == *production_probe.trigger_call_stable_id
        || production_probe.visit_call_stable_id
            == *production_probe.trigger_call_stable_id
        || production_facts.runtime_probes.size() != 3
        || !std::ranges::all_of(
            production_facts.runtime_probes,
            [](const runtime_probe_fact& probe) {
                return probe.fault_key
                        == "generated_boundary_tail.std_exception"
                    && probe.relation
                        == runtime_relation::
                            injected_routes_to_site
                    && probe.trigger_call_stable_id.has_value();
            })
        || direct_boundaries != 2
        || default_error == multi_wrapper.calls.end()
        || default_error->resolution
            != resolution_status::direct
        || !default_error->target_set_complete
        || default_error->targets.size() != 1
        || default_error->targets.front().domain
            != callee_domain::postgresql_server
        || std::ranges::any_of(
            default_error->live_objects,
            [](const flow_object& object) {
                return !object.trivially_destructible;
            })) {
        throw std::runtime_error(
            "production boundary or multi-arity lifetime-free "
            "default did not preserve routed probe authority");
    }

    static constexpr std::string_view conflicting_specialization =
        R"cpp(module;
#include <string_view>
#include <utils/elog.h>
export module bioseq.runtime_probe.contract;
namespace bioseq::postgres::support {
struct sql_boundary_probe_descriptor final {
    std::string_view site_id;
    std::string_view fault_key;
};
template<class p_function>
int invoke_postgres_callback(
        sql_boundary_probe_descriptor,
        p_function&& call) {
    return call();
}
struct shared_call final {
    int operator()() const {
        return 0;
    }
};
int generated_wrapper(bool first) {
    shared_call call;
    if (first) {
        return invoke_postgres_callback(
            sql_boundary_probe_descriptor{
                .site_id =
                    "probe:ffffffffffffffffffffffffffffffff"
                    "ffffffffffffffffffffffffffffffff",
                .fault_key = "fixture.shared",
            },
            call);
    }
    return invoke_postgres_callback(
        sql_boundary_probe_descriptor{
            .site_id =
                "probe:00000000000000000000000000000000"
                "00000000000000000000000000000000",
            .fault_key = "fixture.shared",
        },
        call);
}
}
)cpp";
    require_scan_failure(
        [&] {
            static_cast<void>(scan(
                conflicting_specialization,
                "runtime-conflicting-specialization"));
        },
        "one SQL boundary specialization is bound to "
        "conflicting literal probe descriptors");

    static constexpr std::string_view invalid_descriptor = R"cpp(module;
#include <string_view>
#include <utils/elog.h>
export module bioseq.runtime_probe.contract;
namespace bioseq::postgres::support {
struct sql_boundary_probe_descriptor final {
    std::string_view site_id;
    std::string_view fault_key;
};
template<class p_function>
int invoke_postgres_callback(
        sql_boundary_probe_descriptor,
        p_function&& call) {
    return call();
}
int generated_wrapper() {
    return invoke_postgres_callback(
        sql_boundary_probe_descriptor{
            .site_id = "probe:not-a-sealed-site",
            .fault_key = "fixture.descriptor",
        },
        [] { return 0; });
}
}
)cpp";
    require_scan_failure(
        [&] {
            static_cast<void>(scan(
                invalid_descriptor,
                "runtime-invalid-descriptor"));
        },
        "invalid site id or fault key");

    static constexpr std::string_view local_descriptor = R"cpp(module;
#include <string_view>
#include <utils/elog.h>
export module bioseq.runtime_probe.contract;
namespace bioseq::postgres::support {
struct sql_boundary_probe_descriptor final {
    std::string_view site_id;
    std::string_view fault_key;
};
template<class p_function>
int invoke_postgres_callback(
        sql_boundary_probe_descriptor,
        p_function&& call) {
    return call();
}
int generated_wrapper() {
    const sql_boundary_probe_descriptor boundary_probe{
        .site_id =
            "probe:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
            "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
        .fault_key = "fixture.local",
    };
    return invoke_postgres_callback(
        boundary_probe,
        [] { return 0; });
}
}
)cpp";
    require_scan_failure(
        [&] {
            static_cast<void>(scan(
                local_descriptor,
                "runtime-local-descriptor"));
        },
        "not one exact two-field aggregate initializer");

    static constexpr std::string_view computed_key = R"cpp(module;
#include <utils/elog.h>
export module bioseq.runtime_probe.contract;
namespace bioseq::postgres::support::safety_runtime_probe {
void record_site_visit(const char*) noexcept;
}
void invalid_probe() {
    const char* key = "fixture.computed";
    bioseq::postgres::support::safety_runtime_probe::
        record_site_visit(key);
    pg_contract_leaf();
}
)cpp";
    require_scan_failure(
        [&] {
            static_cast<void>(
                scan(computed_key, "runtime-computed-key"));
        },
        "must be one ordinary narrow string literal");

    static constexpr std::string_view lookalike_string_view =
        R"cpp(module;
#include <utils/elog.h>
export module bioseq.runtime_probe.contract;
namespace adversarial {
template<class p_character>
struct basic_string_view {
    constexpr basic_string_view(const p_character*) noexcept {}
};
using string_view = basic_string_view<char>;
}
namespace bioseq::postgres::support::safety_runtime_probe {
void record_site_visit(adversarial::string_view) noexcept;
}
void invalid_probe() {
    bioseq::postgres::support::safety_runtime_probe::
        record_site_visit("fixture.lookalike");
    pg_contract_leaf();
}
)cpp";
    require_scan_failure(
        [&] {
            static_cast<void>(scan(
                lookalike_string_view,
                "runtime-lookalike-string-view"));
        },
        "must be one ordinary narrow string literal");

    static constexpr std::string_view throwing_probe = R"cpp(module;
#include <utils/elog.h>
export module bioseq.runtime_probe.contract;
namespace bioseq::postgres::support::safety_runtime_probe {
void record_site_visit(const char*);
}
void invalid_probe() {
    bioseq::postgres::support::safety_runtime_probe::
        record_site_visit("fixture.throwing");
    pg_contract_leaf();
}
)cpp";
    require_scan_failure(
        [&] {
            static_cast<void>(
                scan(throwing_probe, "runtime-throwing-probe"));
        },
        "exact noexcept declaration");

    static constexpr std::string_view indirect_probe = R"cpp(module;
#include <utils/elog.h>
export module bioseq.runtime_probe.contract;
namespace bioseq::postgres::support::safety_runtime_probe {
void record_site_visit(const char*) noexcept;
}
void invalid_probe() {
    void (*const visit)(const char*) =
        &bioseq::postgres::support::safety_runtime_probe::
            record_site_visit;
    visit("fixture.indirect");
    pg_contract_leaf();
}
)cpp";
    require_scan_failure(
        [&] {
            static_cast<void>(
                scan(indirect_probe, "runtime-indirect-probe"));
        },
        "must resolve directly");

    static constexpr std::string_view negated_trigger = R"cpp(module;
#include <utils/elog.h>
export module bioseq.runtime_probe.contract;
namespace bioseq::postgres::support::safety_runtime_probe {
void record_site_visit(const char*) noexcept;
bool consume_fault(const char*) noexcept;
}
void invalid_probe() {
    if (!bioseq::postgres::support::safety_runtime_probe::
            consume_fault("fixture.negated")) {
        bioseq::postgres::support::safety_runtime_probe::
            record_site_visit("fixture.negated");
        pg_contract_leaf();
    }
}
)cpp";
    require_scan_failure(
        [&] {
            static_cast<void>(
                scan(negated_trigger, "runtime-negated-trigger"));
        },
        "unnegated direct condition");

    static constexpr std::string_view ambiguous_site = R"cpp(module;
#include <utils/elog.h>
export module bioseq.runtime_probe.contract;
namespace bioseq::postgres::support::safety_runtime_probe {
void record_site_visit(const char*) noexcept;
}
void invalid_probe(bool branch) {
    bioseq::postgres::support::safety_runtime_probe::
        record_site_visit("fixture.ambiguous");
    if (branch) {
        pg_contract_leaf();
    } else {
        ReThrowError(nullptr);
    }
}
)cpp";
    require_scan_failure(
        [&] {
            static_cast<void>(
                scan(ambiguous_site, "runtime-ambiguous-site"));
        },
        "does not immediately dominate one exact");

    static constexpr std::string_view guarded_visit = R"cpp(module;
#include <utils/elog.h>
export module bioseq.runtime_probe.contract;
namespace bioseq::postgres::support::safety_runtime_probe {
void record_site_visit(const char*) noexcept;
}
void invalid_probe(bool enabled) {
    if (enabled) {
        bioseq::postgres::support::safety_runtime_probe::
            record_site_visit("fixture.guarded");
    }
    pg_contract_leaf();
}
)cpp";
    require_scan_failure(
        [&] {
            static_cast<void>(
                scan(guarded_visit, "runtime-guarded-visit"));
        },
        "does not dominate its recoverable ERROR call");

    static constexpr std::string_view bypassed_after_success =
        R"cpp(module;
#include <utils/elog.h>
export module bioseq.runtime_probe.contract;
namespace bioseq::postgres::support::safety_runtime_probe {
void record_site_visit(const char*) noexcept;
bool consume_fault(const char*) noexcept;
}
void invalid_probe(bool bypass) {
    pg_contract_leaf();
    if (bypass) {
        return;
    }
    bioseq::postgres::support::safety_runtime_probe::
        record_site_visit("fixture.bypass");
    if (bioseq::postgres::support::safety_runtime_probe::
            consume_fault("fixture.bypass")) {
        ReThrowError(nullptr);
    }
}
)cpp";
    require_scan_failure(
        [&] {
            static_cast<void>(scan(
                bypassed_after_success,
                "runtime-bypassed-after-success"));
        },
        "does not prove exactly one supported CFG relation");

    static constexpr std::string_view mismatched_keys = R"cpp(module;
#include <utils/elog.h>
export module bioseq.runtime_probe.contract;
namespace bioseq::postgres::support::safety_runtime_probe {
void record_site_visit(const char*) noexcept;
bool consume_fault(const char*) noexcept;
}
void invalid_probe() {
    if (bioseq::postgres::support::safety_runtime_probe::
            consume_fault("fixture.trigger")) {
        bioseq::postgres::support::safety_runtime_probe::
            record_site_visit("fixture.visit");
        pg_contract_leaf();
    }
}
)cpp";
    require_scan_failure(
        [&] {
            static_cast<void>(
                scan(mismatched_keys, "runtime-mismatched-keys"));
        },
        "has no matching authenticated site visit");
}

inline void run_callback_root_contracts() {
    static constexpr std::string_view source = R"cpp(
struct TableAmRoutine {
    int type;
    void (*relation_vacuum)();
};
struct CustomScanMethods {
    const char* CustomName;
    void (*CreateCustomScanState)();
};
struct MemoryContextCallback {
    void (*func)(void*);
    void* arg;
};
struct ErrorContextCallback {
    ErrorContextCallback* previous;
    void (*callback)(void*);
    void* arg;
};
struct BackgroundWorker {
    char bgw_function_name[64];
};
int pg_snprintf(char*, unsigned long, const char*, ...);
void RegisterBackgroundWorker(BackgroundWorker*);
using GucEnumCheckHook = bool (*)(int*, void**, int);
using GucEnumAssignHook = void (*)(int, void*);
using GucShowHook = const char* (*)();
void DefineCustomBoolVariable(
    const char*,
    const char*,
    const char*,
    bool*,
    bool,
    int,
    int,
    GucEnumCheckHook,
    GucEnumAssignHook,
    GucShowHook);
template<class p_enum>
struct postgres_enum_registration {
    GucEnumCheckHook check_hook;
    GucEnumAssignHook assign_hook;
    GucShowHook show_hook;
};
using planner_hook_type = void (*)();
planner_hook_type planner_hook = nullptr;
using XactCallback = void (*)(int, void*);
void RegisterXactCallback(XactCallback, void*);

enum class contract_mode { disabled, enabled };
void vacuum_callback() {}
void scan_callback() {}
void planner_callback() {}
void xact_callback(int, void*) {}
void memory_callback(void*) {}
void error_callback(void*) {}
bool guc_check(int*, void**, int) { return true; }
void guc_assign(int, void*) {}
const char* guc_show() { return "enabled"; }
bool primitive_guc_check(int*, void**, int) { return true; }
void primitive_guc_assign(int, void*) {}
const char* primitive_guc_show() { return "enabled"; }
void unrelated_callback() {}
extern "C" void background_main(int) {}
extern "C" void unregistered_background_main(int) {}
bool primitive_guc_enabled = false;

TableAmRoutine table_methods{
    .type = 1,
    .relation_vacuum = vacuum_callback,
};
CustomScanMethods scan_methods{
    .CustomName = "contract",
    .CreateCustomScanState = scan_callback,
};
struct project_only_callbacks {
    void (*callback)();
};
project_only_callbacks unrelated{.callback = unrelated_callback};
postgres_enum_registration<contract_mode> guc_registration{
    .check_hook = &guc_check,
    .assign_hook = &guc_assign,
    .show_hook = &guc_show,
};

void install_callbacks(MemoryContextCallback* reset) {
    planner_hook = &planner_callback;
    RegisterXactCallback(xact_callback, nullptr);
    reset->func = &memory_callback;
    DefineCustomBoolVariable(
        "bioseq.contract",
        "contract",
        nullptr,
        &primitive_guc_enabled,
        false,
        0,
        0,
        &primitive_guc_check,
        &primitive_guc_assign,
        &primitive_guc_show);
    struct error_guard {
        ErrorContextCallback callback;
        bool armed;
    };
    error_guard guard{
        .callback = {
            .previous = nullptr,
            .callback = &error_callback,
            .arg = nullptr,
        },
        .armed = true,
    };
    (void) guard;
    BackgroundWorker worker{};
    pg_snprintf(
        worker.bgw_function_name,
        sizeof(worker.bgw_function_name),
        "background_main");
    RegisterBackgroundWorker(&worker);
}

void configure_unregistered_worker() {
    BackgroundWorker worker{};
    pg_snprintf(
        worker.bgw_function_name,
        sizeof(worker.bgw_function_name),
        "unregistered_background_main");
}
)cpp";
    const std::filesystem::path path =
        std::filesystem::temp_directory_path()
        / "bioseq_callback_root_contract.cpp";
    const isolated_compiler_fact_scan facts =
        scan_safety_facts_from_code(
            path, source, {"-std=c++26"});
    const auto has_root =
        [&](std::string_view family,
            std::string_view slot,
            std::string_view target) {
            return std::ranges::any_of(
                facts.callback_roots,
                [&](const postgres_callback_root& root) {
                    return root.family == family
                        && root.slot == slot
                        && root.target_qualified_name == target
                        && root.stable_callback_id.starts_with(
                            "pg-callback:")
                        && !root.owner_usr.empty()
                        && !root.registration.source_file.empty()
                        && !root.target_definition.source_file.empty();
                });
        };
    if (!has_root(
            "table_am",
            "relation_vacuum",
            "vacuum_callback")
        || !has_root(
            "custom_scan",
            "CreateCustomScanState",
            "scan_callback")
        || !has_root(
            "postgres_hook",
            "planner_hook",
            "planner_callback")
        || !has_root(
            "xact_callback",
            "RegisterXactCallback",
            "xact_callback")
        || !has_root(
            "memory_context",
            "func",
            "memory_callback")
        || !has_root(
            "error_context",
            "callback",
            "error_callback")
        || !has_root(
            "background_worker",
            "bgw_function_name",
            "background_main")
        || !has_root(
            "guc_callback",
            "check_hook",
            "guc_check")
        || !has_root(
            "guc_callback",
            "assign_hook",
            "guc_assign")
        || !has_root(
            "guc_callback",
            "show_hook",
            "guc_show")
        || !has_root(
            "guc_callback",
            "check_hook",
            "primitive_guc_check")
        || !has_root(
            "guc_callback",
            "assign_hook",
            "primitive_guc_assign")
        || !has_root(
            "guc_callback",
            "show_hook",
            "primitive_guc_show")) {
        throw std::runtime_error(
            "PostgreSQL callback-root discovery is incomplete");
    }
    if (std::ranges::any_of(
            facts.callback_roots,
            [](const postgres_callback_root& root) {
                return root.target_qualified_name
                        == "unrelated_callback"
                    || root.target_qualified_name
                        == "unregistered_background_main";
            })) {
        throw std::runtime_error(
            "non-PostgreSQL callback storage was treated as a "
            "PostgreSQL callback root");
    }
    require_scan_failure(
        [&] {
            const std::string mutation = replace_once(
                source,
                "\"background_main\"",
                "\"missing_background_main\"");
            static_cast<void>(scan_safety_facts_from_code(
                path, mutation, {"-std=c++26"}));
        },
        "BackgroundWorker entry point does not resolve to one exact "
        "owned definition");
}

inline void run_definition_backed_template_redeclaration_contract() {
    bridge_fixture fixture = make_bridge_fixture();
    const std::filesystem::path system_root =
        fixture.root / "template_redeclaration_system_headers";
    std::error_code directory_error;
    std::filesystem::create_directories(
        system_root, directory_error);
    if (directory_error) {
        throw std::runtime_error(
            "cannot create template-redeclaration contract headers");
    }
    bioseq::codegen::publish_artifact_if_changed(
        system_root / "declaration.h",
        "#pragma once\n"
        "template<class p_value>\n"
        "p_value&& coherent_candidate(p_value&& value);\n");
    bioseq::codegen::publish_artifact_if_changed(
        system_root / "definition.h",
        "#pragma once\n"
        "template<class p_value>\n"
        "p_value&& coherent_candidate(p_value&& value) {\n"
        "    return static_cast<p_value&&>(value);\n"
        "}\n");
    const std::vector<std::string> arguments{
        "-I" + fixture.header_root.string(),
        "-isystem",
        system_root.string(),
    };
    static constexpr std::string_view source = R"cpp(
#include <declaration.h>
void use_declared_specialization() {
    int value = 0;
    static_cast<void>(coherent_candidate(value));
}
#include <definition.h>
void use_defined_specialization() {
    long value = 0;
    static_cast<void>(coherent_candidate(value));
}
)cpp";
    const std::vector<function_flow> facts =
        scan_safety_flow_from_code(
            module_path(
                fixture,
                "definition_backed_template_redeclaration.cpp"),
            source,
            arguments,
            "definition-backed-template-redeclaration",
            std::addressof(fixture.provenance));
    static_cast<void>(require_function(
        facts, "use_declared_specialization"));
    static_cast<void>(require_function(
        facts, "use_defined_specialization"));
}

inline void run_named_module_structural_nttp_usr_contract() {
    bridge_fixture fixture = make_bridge_fixture();
    static constexpr std::string_view source = R"cpp(
export module bioseq.codegen.safety.structural_usr_contract;
export namespace bioseq::contract {
template<unsigned long p_size>
struct fixed_string {
    char value[p_size]{};

    constexpr fixed_string(const char (&text)[p_size]) {
        for (unsigned long index = 0; index < p_size; ++index) {
            value[index] = text[index];
        }
    }
};
template<fixed_string p_name>
struct named_value {
    template<class p_handler>
    static void dispatch(p_handler&& handler) {
        handler();
    }
};
using locator_value = named_value<"locator">;
[[nodiscard]] inline bool structural_target(
        const locator_value*) noexcept {
    return true;
}
void structural_root() {
    locator_value value{};
    static_cast<void>(structural_target(&value));
    locator_value::dispatch([] {});
}
}
)cpp";
    const std::vector<function_flow> facts =
        scan_safety_flow_from_code(
            module_path(
                fixture,
                "named_module_structural_nttp.cppm"),
            source,
            {"-x", "c++-module"},
            "named-module-structural-nttp");
    const function_flow& target = require_function(
        facts, "bioseq::contract::structural_target");
    const function_flow& root = require_function(
        facts, "bioseq::contract::structural_root");
    const auto call = std::ranges::find_if(
        root.calls,
        [&](const flow_call_site& candidate) {
            return candidate.targets.size() == 1
                && candidate.targets.front()
                       .target_qualified_name
                    == target.qualified_name;
        });
    const flow_call_target* dispatch_target = nullptr;
    for (const flow_call_site& candidate : root.calls) {
        for (const flow_call_target& candidate_target :
             candidate.targets) {
            if (candidate_target.target_qualified_name.ends_with(
                    "::dispatch")) {
                dispatch_target = std::addressof(candidate_target);
            }
        }
    }
    const auto dispatch_definition =
        dispatch_target == nullptr
        ? facts.end()
        : std::ranges::find(
              facts,
              dispatch_target->target_usr,
              &function_flow::stable_usr);
    static constexpr std::string_view fallback_prefix{
        "clang-fallback:function:"
        "bioseq.codegen.safety.structural_usr_contract:"};
    if (!target.stable_usr.starts_with(fallback_prefix)
        || call == root.calls.end()
        || call->targets.front().target_usr
            != target.stable_usr
        || dispatch_target == nullptr
        || !dispatch_target->target_usr.starts_with(
            fallback_prefix)
        || dispatch_definition == facts.end()) {
        throw std::runtime_error(
            "named-module structural-NTTP function identity "
            "is not exact");
    }
}

inline void
run_distinct_declaration_provenance_candidate_conflict_contract() {
    bridge_fixture fixture = make_bridge_fixture();
    const std::filesystem::path system_root =
        fixture.root / "system_headers";
    std::error_code directory_error;
    std::filesystem::create_directories(
        system_root, directory_error);
    if (directory_error) {
        throw std::runtime_error(
            "cannot create provenance-candidate contract headers");
    }
    bioseq::codegen::publish_artifact_if_changed(
        system_root / "first.h",
        "#pragma once\nvoid external_candidate();\n");
    bioseq::codegen::publish_artifact_if_changed(
        system_root / "second.h",
        "#pragma once\nextern void external_candidate();\n");
    const std::vector<std::string> arguments{
        "-I" + fixture.header_root.string(),
        "-isystem",
        system_root.string(),
    };
    static constexpr std::string_view conflicting = R"cpp(
#include <first.h>
void first_use() {
    external_candidate();
}
#include <second.h>
void second_use() {
    external_candidate();
}
)cpp";
    require_scan_failure(
        [&] {
            static_cast<void>(scan_safety_flow_from_code(
                module_path(
                    fixture,
                    "provenance_candidate_contract.cpp"),
                conflicting,
                arguments,
                "provenance-candidate-conflict",
                std::addressof(fixture.provenance)));
        },
        "conflicting compiler provenance candidates");
}

inline void run_incoherent_pg_candidate_range_contract() {
    const std::filesystem::path header_root =
        std::filesystem::temp_directory_path()
        / "bioseq_safety_incoherent_pg_candidate_server";
    std::error_code directory_error;
    std::filesystem::create_directories(
        header_root / "utils", directory_error);
    if (directory_error) {
        throw std::runtime_error(
            "cannot create incoherent PostgreSQL candidate root");
    }
    static constexpr std::string_view pair_header =
        R"cpp(#pragma once
#define BIOSEQ_DECLARE_PG_PAIR() \
    int first_pg_candidate(); \
    int second_pg_candidate()
BIOSEQ_DECLARE_PG_PAIR();
)cpp";
    static constexpr std::string_view auxiliary_header =
        "#pragma once\nvoid pg_candidate_auxiliary();\n";
    bioseq::codegen::publish_artifact_if_changed(
        header_root / "pair.h", pair_header);
    bioseq::codegen::publish_artifact_if_changed(
        header_root / "utils/palloc.h", auxiliary_header);
    provenance_bundle provenance{
        .postgresql_tag = "REL_17_10",
        .postgresql_revision =
            "25c49f3a4a742ba283f5cc43cc7f1d361552e917",
        .records = {
            {
                .record_id = "contract.pg_pair_header",
                .kind = provenance_kind::bridge_primitive,
                .spelling = "BIOSEQ_DECLARE_PG_PAIR",
                .upstream_file = "src/include/pair.h",
                .upstream_file_sha256 =
                    bioseq::common::sha256_hex(pair_header),
                .fragment_sha256 =
                    bioseq::common::sha256_hex(pair_header),
                .rationale =
                    "Authenticates a partial macro declaration fixture.",
            },
            {
                .record_id = "contract.pg_pair_auxiliary",
                .kind = provenance_kind::bridge_primitive,
                .spelling = "pg_candidate_auxiliary",
                .upstream_file =
                    "src/include/utils/palloc.h",
                .upstream_file_sha256 =
                    bioseq::common::sha256_hex(auxiliary_header),
                .fragment_sha256 =
                    bioseq::common::sha256_hex(auxiliary_header),
                .rationale =
                    "Supplies an independent PostgreSQL header pin.",
            },
        },
    };
    static constexpr std::string_view source = R"cpp(
#include <pair.h>
void call_partial_pg_candidate() {
    first_pg_candidate();
}
)cpp";
    require_scan_failure(
        [&] {
            static_cast<void>(scan_safety_flow_from_code(
                std::filesystem::temp_directory_path()
                    / "bioseq_safety_incoherent_pg_candidate.cpp",
                source,
                {"-I" + header_root.string()},
                "incoherent-pg-candidate-contract",
                std::addressof(provenance)));
        },
        "incoherent or ambiguous declaration range");
}

}  // namespace fact_contract_detail

inline void run_fact_producer_contract_self_tests() {
    static constexpr std::string_view source = R"cpp(
struct guard {
    guard() {}
    ~guard() {}
};
struct base_a {
    ~base_a() {}
};
struct base_b {
    ~base_b() {}
};
struct derived : base_a, base_b {
    ~derived() {}
};
void leaf() {}
#define CALL_LEAF() leaf()
void root(bool branch) {
    guard outer;
    if (branch) {
        guard inner;
        CALL_LEAF();
    } else {
        leaf();
    }
}
using callback = void (*)();
void unresolved(callback operation) {
    operation();
}
void first_target() {}
void second_target() {}
void mutable_pointer() {
    callback operation = &first_target;
    operation = &second_target;
    operation();
}
namespace bioseq::postgres::support {
template <typename p_operation>
void invoke_pg_as_cpp(p_operation&& operation) {
    operation();
}
}
void stored_lambda_capture() {
    guard captured;
    const auto operation = [&captured]() { leaf(); };
    bioseq::postgres::support::invoke_pg_as_cpp(operation);
}
void parameter_lifetime(guard parameter) {
    leaf();
}
void catch_lifetime() {
    try {
        throw guard{};
    } catch (guard caught) {
        leaf();
    }
}
void cleanup_guard(guard*) {}
void cleanup_lifetime() {
    guard cleaned __attribute__((cleanup(cleanup_guard)));
    leaf();
}
void accepts_unsigned(unsigned long long) {}
void oversized_unsigned_constant() {
    accepts_unsigned(18446744073709551615ULL);
}
void control_flow_assembly() {
    asm volatile("" ::: "memory");
}
void discarded_constexpr_branch() {
    if constexpr (false) {
        guard discarded;
        leaf();
    }
}
)cpp";
    const std::filesystem::path path =
        std::filesystem::temp_directory_path()
        / "bioseq_safety_fact_contract.cpp";
    const std::vector<function_flow> first =
        scan_safety_flow_from_code(path, source, {});
    const std::vector<function_flow> second =
        scan_safety_flow_from_code(path, source, {});
    static constexpr std::string_view pg_header =
        "#pragma once\nvoid pg_contract_call();\n";
    static constexpr std::string_view pg_elog_header =
        R"cpp(#pragma once
#define ERROR 20
bool errstart(int, const char*);
void errfinish(const char*, int, const char*);
int errcode(int);
#define pg_attribute_printf(f, a) \
    __attribute__((format(printf, f, a)))
int errmsg(const char*, ...) pg_attribute_printf(1, 2);
#define ereport_domain(elevel, domain, ...) \
    do { \
        if (errstart((elevel), (domain))) \
            __VA_ARGS__, errfinish(__FILE__, __LINE__, __func__); \
    } while (false)
#define ereport(elevel, ...) \
    ereport_domain((elevel), nullptr, __VA_ARGS__)
)cpp";
    static constexpr std::string_view pg_authority_header =
        "#pragma once\nvoid pg_contract_auxiliary();\n";
    const std::filesystem::path pg_header_root =
        std::filesystem::temp_directory_path()
        / "bioseq_safety_pg17_contract_server";
    std::error_code directory_error;
    std::filesystem::create_directories(
        pg_header_root, directory_error);
    if (directory_error) {
        throw std::runtime_error(
            "cannot create PostgreSQL authority contract root");
    }
    bioseq::codegen::publish_artifact_if_changed(
        pg_header_root / "pg_contract.h",
        pg_header);
    directory_error.clear();
    std::filesystem::create_directories(
        pg_header_root / "utils", directory_error);
    if (directory_error) {
        throw std::runtime_error(
            "cannot create PostgreSQL authority contract "
            "subdirectory");
    }
    bioseq::codegen::publish_artifact_if_changed(
        pg_header_root / "utils/palloc.h",
        pg_authority_header);
    bioseq::codegen::publish_artifact_if_changed(
        pg_header_root / "utils/elog.h",
        pg_elog_header);
    const std::size_t report_macro_begin =
        pg_elog_header.find("#define ereport_domain");
    if (report_macro_begin == std::string_view::npos) {
        throw std::runtime_error(
            "cannot locate PostgreSQL report-macro contract");
    }
    provenance_bundle pg_provenance{
        .postgresql_tag = "REL_17_10",
        .postgresql_revision =
            "25c49f3a4a742ba283f5cc43cc7f1d361552e917",
        .records = {
            {
                .record_id = "contract.pg_header",
                .kind = provenance_kind::bridge_primitive,
                .spelling = "pg_contract_call",
                .upstream_file =
                    "src/include/pg_contract.h",
                .upstream_file_sha256 =
                    bioseq::common::sha256_hex(pg_header),
                .fragment_sha256 =
                    bioseq::common::sha256_hex(pg_header),
                .rationale =
                    "Authenticates the isolated PostgreSQL header root.",
            },
            {
                .record_id = "pg17.ereport_macro",
                .kind = provenance_kind::recoverable_error_seed,
                .spelling = "ereport(ERROR, ...)",
                .upstream_file =
                    "src/include/utils/elog.h",
                .upstream_span =
                    fact_contract_detail::fixture_range(
                        report_macro_begin,
                        pg_elog_header.size()),
                .upstream_file_sha256 =
                    bioseq::common::sha256_hex(
                        pg_elog_header),
                .fragment_sha256 =
                    bioseq::common::sha256_hex(
                        pg_elog_header.substr(
                            report_macro_begin)),
                .rationale =
                    "Authenticates nested PostgreSQL report macros.",
                .severities = {
                    postgres_severity::recoverable_error,
                },
            },
            {
                .record_id = "contract.pg_authority_header",
                .kind = provenance_kind::bridge_primitive,
                .spelling = "pg_contract_auxiliary",
                .upstream_file =
                    "src/include/utils/palloc.h",
                .upstream_file_sha256 =
                    bioseq::common::sha256_hex(
                        pg_authority_header),
                .fragment_sha256 =
                    bioseq::common::sha256_hex(
                        pg_authority_header),
                .rationale =
                    "Supplies an independent PostgreSQL header pin.",
            },
        },
    };
    static constexpr std::string_view pg_source = R"cpp(
#include <pg_contract.h>
void pg_domain_root() {
    pg_contract_call();
}
)cpp";
    const std::vector<function_flow> pg_functions =
        scan_safety_flow_from_code(
            std::filesystem::temp_directory_path()
                / "bioseq_safety_pg_domain_contract.cpp",
            pg_source,
            {"-I" + pg_header_root.string()},
            "pg-domain-contract",
            std::addressof(pg_provenance));
    static constexpr std::string_view pg_report_source = R"cpp(
#include <utils/elog.h>
void pg_report_root() {
    ereport(ERROR, (errcode(1), errmsg("contract")));
}
#undef ereport
#define ereport(elevel, ...) __VA_ARGS__
)cpp";
    const std::vector<function_flow> pg_report_functions =
        scan_safety_flow_from_code(
            std::filesystem::temp_directory_path()
                / "bioseq_safety_pg_report_contract.cpp",
            pg_report_source,
            {"-I" + pg_header_root.string()},
            "pg-report-contract",
            std::addressof(pg_provenance));
    const auto create_header_symlink = [](
        const std::filesystem::path& target,
        const std::filesystem::path& link) {
        std::error_code error;
        if (std::filesystem::exists(link, error)
            || std::filesystem::is_symlink(link, error)) {
            error.clear();
            if (std::filesystem::equivalent(
                    target, link, error)
                && !error) {
                return;
            }
            error.clear();
            std::filesystem::remove(link, error);
            if (error) {
                throw std::runtime_error(
                    "cannot reset PostgreSQL header symlink: "
                    + link.string());
            }
        }
        error.clear();
        std::filesystem::create_symlink(target, link, error);
        if (error) {
            std::error_code equivalent_error;
            if (std::filesystem::equivalent(
                    target, link, equivalent_error)
                && !equivalent_error) {
                return;
            }
            throw std::runtime_error(
                "cannot create PostgreSQL header symlink: "
                + link.string());
        }
    };
    const auto prepare_pg_header_tree = [&](
        const std::filesystem::path& root) {
        std::error_code error;
        std::filesystem::create_directories(
            root / "utils", error);
        if (error) {
            throw std::runtime_error(
                "cannot create physical PostgreSQL header tree");
        }
        bioseq::codegen::publish_artifact_if_changed(
            root / "pg_contract.h", pg_header);
        bioseq::codegen::publish_artifact_if_changed(
            root / "utils/elog.h", pg_elog_header);
        bioseq::codegen::publish_artifact_if_changed(
            root / "utils/palloc.h", pg_authority_header);
    };
    const std::filesystem::path symlink_physical_root =
        std::filesystem::temp_directory_path()
        / "bioseq_safety_pg17_symlink_physical_server";
    const std::filesystem::path symlink_include_root =
        std::filesystem::temp_directory_path()
        / "bioseq_safety_pg17_symlink_include_server";
    prepare_pg_header_tree(symlink_physical_root);
    directory_error.clear();
    std::filesystem::create_directories(
        symlink_include_root / "utils", directory_error);
    if (directory_error) {
        throw std::runtime_error(
            "cannot create PostgreSQL symlink-farm include root");
    }
    for (const std::filesystem::path& relative : {
             std::filesystem::path{"pg_contract.h"},
             std::filesystem::path{"utils/elog.h"},
             std::filesystem::path{"utils/palloc.h"}}) {
        create_header_symlink(
            symlink_physical_root / relative,
            symlink_include_root / relative);
    }
    const std::vector<function_flow> symlink_pg_report_functions =
        scan_safety_flow_from_code(
            std::filesystem::temp_directory_path()
                / "bioseq_safety_symlink_pg_report_contract.cpp",
            pg_report_source,
            {"-I" + symlink_include_root.string()},
            "symlink-pg-report-contract",
            std::addressof(pg_provenance));

    const std::filesystem::path split_physical_root =
        std::filesystem::temp_directory_path()
        / "bioseq_safety_pg17_split_physical_server";
    const std::filesystem::path split_include_root =
        std::filesystem::temp_directory_path()
        / "bioseq_safety_pg17_split_include_server";
    prepare_pg_header_tree(split_physical_root);
    directory_error.clear();
    std::filesystem::create_directories(
        split_include_root / "utils", directory_error);
    if (directory_error) {
        throw std::runtime_error(
            "cannot create split PostgreSQL include root");
    }
    create_header_symlink(
        symlink_physical_root / "pg_contract.h",
        split_include_root / "pg_contract.h");
    create_header_symlink(
        symlink_physical_root / "utils/elog.h",
        split_include_root / "utils/elog.h");
    create_header_symlink(
        split_physical_root / "utils/palloc.h",
        split_include_root / "utils/palloc.h");
    fact_contract_detail::require_scan_failure(
        [&] {
            static_cast<void>(scan_safety_flow_from_code(
                std::filesystem::temp_directory_path()
                    / "bioseq_safety_split_pg_report_contract.cpp",
                pg_report_source,
                {"-I" + split_include_root.string()},
                "split-pg-report-contract",
                std::addressof(pg_provenance)));
        },
        "no include root authenticated");
    static constexpr std::string_view spoofed_pg_report_source =
        R"cpp(
#include <utils/elog.h>
#undef ereport
#define ereport(elevel, ...) __VA_ARGS__
void spoofed_pg_report_root() {
    ereport(ERROR, (errcode(1)));
}
)cpp";
    fact_contract_detail::require_scan_failure(
        [&] {
            static_cast<void>(scan_safety_flow_from_code(
                std::filesystem::temp_directory_path()
                    / "bioseq_safety_spoofed_pg_report_contract.cpp",
                spoofed_pg_report_source,
                {"-I" + pg_header_root.string()},
                "spoofed-pg-report-contract",
                std::addressof(pg_provenance)));
        },
        "report macro expansion is not authenticated");
    provenance_bundle stale_pg_provenance = pg_provenance;
    stale_pg_provenance.records.front()
        .upstream_file_sha256 = std::string(64, '0');
    bool stale_pg_root_rejected = false;
    try {
        static_cast<void>(scan_safety_flow_from_code(
            std::filesystem::temp_directory_path()
                / "bioseq_safety_stale_pg_domain_contract.cpp",
            pg_source,
            {"-I" + pg_header_root.string()},
            "stale-pg-domain-contract",
            std::addressof(stale_pg_provenance)));
    } catch (const std::runtime_error& error) {
        stale_pg_root_rejected =
            std::string_view{error.what()}.contains(
                "no include root authenticated");
    }
    provenance_bundle single_pin_provenance = pg_provenance;
    single_pin_provenance.records.resize(1);
    bool single_pin_provenance_rejected = false;
    try {
        static_cast<void>(scan_safety_flow_from_code(
            std::filesystem::temp_directory_path()
                / "bioseq_safety_single_pin_pg_domain_contract.cpp",
            pg_source,
            {"-I" + pg_header_root.string()},
            "single-pin-pg-domain-contract",
            std::addressof(single_pin_provenance)));
    } catch (const std::runtime_error& error) {
        single_pin_provenance_rejected =
            std::string_view{error.what()}.contains(
                "one pinned header cannot authenticate");
    }
    const std::filesystem::path one_header_root =
        std::filesystem::temp_directory_path()
        / "bioseq_safety_pg17_one_header_contract_server";
    std::filesystem::create_directories(
        one_header_root, directory_error);
    if (directory_error) {
        throw std::runtime_error(
            "cannot create one-header PostgreSQL contract root");
    }
    bioseq::codegen::publish_artifact_if_changed(
        one_header_root / "pg_contract.h",
        pg_header);
    bool one_header_root_rejected = false;
    try {
        static_cast<void>(scan_safety_flow_from_code(
            std::filesystem::temp_directory_path()
                / "bioseq_safety_one_header_pg_domain_contract.cpp",
            pg_source,
            {"-I" + one_header_root.string()},
            "one-header-pg-domain-contract",
            std::addressof(pg_provenance)));
    } catch (const std::runtime_error& error) {
        one_header_root_rejected =
            std::string_view{error.what()}.contains(
                "no include root authenticated");
    }
    const function_flow& root =
        fact_contract_detail::require_function(first, "root");
    const function_flow& pg_domain_root =
        fact_contract_detail::require_function(
            pg_functions, "pg_domain_root");
    const function_flow& pg_report_root =
        fact_contract_detail::require_function(
            pg_report_functions, "pg_report_root");
    const function_flow& symlink_pg_report_root =
        fact_contract_detail::require_function(
            symlink_pg_report_functions, "pg_report_root");
    const function_flow& derived_destructor =
        fact_contract_detail::require_function(
            first, "derived::~derived");
    const function_flow& unresolved =
        fact_contract_detail::require_function(
            first, "unresolved");
    const function_flow& mutable_pointer =
        fact_contract_detail::require_function(
            first, "mutable_pointer");
    const function_flow& stored_lambda =
        fact_contract_detail::require_function(
            first, "stored_lambda_capture");
    const function_flow& parameter_lifetime =
        fact_contract_detail::require_function(
            first, "parameter_lifetime");
    const function_flow& catch_lifetime =
        fact_contract_detail::require_function(
            first, "catch_lifetime");
    const function_flow& cleanup_lifetime =
        fact_contract_detail::require_function(
            first, "cleanup_lifetime");
    const function_flow& oversized_unsigned =
        fact_contract_detail::require_function(
            first, "oversized_unsigned_constant");
    const function_flow& assembly =
        fact_contract_detail::require_function(
            first, "control_flow_assembly");
    const function_flow& discarded_constexpr =
        fact_contract_detail::require_function(
            first, "discarded_constexpr_branch");
    const auto oversized_call = std::ranges::find_if(
        oversized_unsigned.calls,
        [](const flow_call_site& call) {
            return std::ranges::any_of(
                call.targets,
                [](const flow_call_target& target) {
                    return target.target_qualified_name
                        == "accepts_unsigned";
                });
        });
    const auto has_target = [](
        const flow_call_site& call,
        std::string_view expected) {
        return std::ranges::any_of(
            call.targets,
            [&](const flow_call_target& target) {
                return target.target_qualified_name == expected;
            });
    };
    const auto report_argument_call = std::ranges::find_if(
        pg_report_root.calls,
        [&](const flow_call_site& call) {
            return has_target(call, "errcode");
        });
    const bool exact_report_macro_chain =
        report_argument_call != pg_report_root.calls.end()
        && !report_argument_call->macro_chain.empty()
        && std::ranges::count(
               report_argument_call->macro_chain,
               std::string{"ereport"},
               &macro_expansion_record::name)
            == 1
        && std::ranges::count(
               report_argument_call->macro_chain,
               std::string{"ereport_domain"},
               &macro_expansion_record::name)
            <= 1
        && std::ranges::all_of(
            report_argument_call->macro_chain,
            [](const macro_expansion_record& expansion) {
                return (expansion.name != "ereport"
                        && expansion.name != "ereport_domain")
                    || expansion.definition.source_file.ends_with(
                        "/utils/elog.h");
            });
    if (!root.cfg_complete
        || !root.lifetime_analysis_complete
        || root.cfg_block_count < 2
        || root.calls.empty()
        || !root.implicit_call_edges_complete
        || root.calls.size()
            != root.ast_callsite_count
                + root.cfg_implicit_callsite_count
        || root.lifetime_object_ids.empty()
        || root.lifetime_inventory_sha256.size() != 64
        || std::ranges::none_of(
            root.calls,
            [](const flow_call_site& call) {
                return !call.macro_chain.empty()
                    && call.macro_chain.front().name
                        == "CALL_LEAF";
            })
        || std::ranges::none_of(
            root.calls,
            [](const flow_call_site& call) {
                return std::ranges::any_of(
                    call.live_objects,
                    [](const flow_object& object) {
                        return !object.trivially_destructible
                            && object.canonical_type
                                .contains("guard");
                    });
            })) {
        throw std::runtime_error(
            "positive compiler-fact AST/CFG/lifetime contract "
            "changed");
    }
    const auto require_adversarial_fact = [](
        bool condition,
        std::string_view predicate) {
        if (!condition) {
            throw std::runtime_error(
                "adversarial compiler-fact contract failed: "
                + std::string{predicate});
        }
    };
    require_adversarial_fact(
        !unresolved.unresolved_indirect_targets.empty(),
        "unresolved indirect target inventory");
    require_adversarial_fact(
        !mutable_pointer.unresolved_indirect_targets.empty(),
        "mutable function-pointer target inventory");
    require_adversarial_fact(
        std::ranges::any_of(
            stored_lambda.calls,
            [](const flow_call_site& call) {
                return call.bridge_transition
                        == bridge_kind::invoke_pg_as_cpp
                    && call.analyzer_entered_bridge_callable
                    && std::ranges::any_of(
                        call.callable_capture_objects,
                        [](const flow_object& object) {
                            return object.origin
                                == crossing_origin::lambda_capture;
                        });
            }),
        "stored bridge lambda capture inventory");
    require_adversarial_fact(
        std::ranges::any_of(
            parameter_lifetime.calls,
            [](const flow_call_site& call) {
                return std::ranges::any_of(
                    call.live_objects,
                    [](const flow_object& object) {
                        return object.canonical_type
                            .contains("guard");
                    });
            }),
        "parameter lifetime inventory");
    require_adversarial_fact(
        std::ranges::any_of(
            catch_lifetime.calls,
            [](const flow_call_site& call) {
                return std::ranges::any_of(
                    call.live_objects,
                    [](const flow_object& object) {
                        return object.origin
                            == crossing_origin::catch_parameter;
                    });
            }),
        "catch-parameter lifetime inventory");
    require_adversarial_fact(
        cleanup_lifetime.cfg_implicit_callsite_count != 0,
        "cleanup attribute implicit edge");
    require_adversarial_fact(
        cleanup_lifetime.calls.size()
            == cleanup_lifetime.ast_callsite_count
                + cleanup_lifetime.cfg_implicit_callsite_count,
        "cleanup callsite accounting");
    require_adversarial_fact(
        oversized_call != oversized_unsigned.calls.end(),
        "oversized integer callsite");
    require_adversarial_fact(
        oversized_call->constant_integer_arguments.size() == 1,
        "oversized integer argument inventory");
    require_adversarial_fact(
        oversized_call->constant_integer_arguments.front().ordinal
            == 0,
        "oversized integer argument ordinal");
    require_adversarial_fact(
        !oversized_call->constant_integer_arguments.front()
             .value.has_value(),
        "oversized integer rejection");
    require_adversarial_fact(
        std::ranges::any_of(
            derived_destructor.calls,
            [&](const flow_call_site& call) {
                return has_target(
                    call, "base_a::~base_a");
            }),
        "first implicit base destructor edge");
    require_adversarial_fact(
        std::ranges::any_of(
            derived_destructor.calls,
            [&](const flow_call_site& call) {
                return has_target(
                    call, "base_b::~base_b");
            }),
        "second implicit base destructor edge");
    require_adversarial_fact(
        std::ranges::any_of(
            pg_domain_root.calls,
            [](const flow_call_site& call) {
                return std::ranges::any_of(
                    call.targets,
                    [](const flow_call_target& target) {
                        return target.target_qualified_name
                                == "pg_contract_call"
                            && target.domain
                                == callee_domain::
                                    postgresql_server;
                    });
            }),
        "authenticated PostgreSQL target domain");
    require_adversarial_fact(
        stale_pg_root_rejected,
        "stale PostgreSQL header-root rejection");
    require_adversarial_fact(
        single_pin_provenance_rejected,
        "single-pin PostgreSQL authority rejection");
    require_adversarial_fact(
        one_header_root_rejected,
        "one-header PostgreSQL root rejection");
    require_adversarial_fact(
        !assembly.control_flow_assembly_sites.empty(),
        "control-flow assembly inventory");
    require_adversarial_fact(
        exact_report_macro_chain,
        "finite authenticated forwarded report-macro chain");
    require_adversarial_fact(
        !symlink_pg_report_root.calls.empty(),
        "physical PostgreSQL symlink-farm header authority");
    require_adversarial_fact(
        discarded_constexpr.ast_callsite_count == 0,
        "discarded constexpr explicit-call exclusion");
    require_adversarial_fact(
        discarded_constexpr.calls.size()
            == discarded_constexpr.cfg_implicit_callsite_count,
        "discarded constexpr implicit-only call accounting");
    require_adversarial_fact(
        std::ranges::none_of(
            discarded_constexpr.calls,
            [&](const flow_call_site& call) {
                return has_target(call, "leaf");
            }),
        "discarded constexpr leaf exclusion");
    if (first.size() != second.size()) {
        throw std::runtime_error(
            "compiler-fact isolated pass is nondeterministic");
    }
    for (std::size_t index = 0; index < first.size(); ++index) {
        if (first[index].stable_usr != second[index].stable_usr
            || first[index].body_sha256
                != second[index].body_sha256
            || first[index].cfg_sha256
                != second[index].cfg_sha256
            || first[index].lifetime_inventory_sha256
                != second[index].lifetime_inventory_sha256
            || first[index].calls.size()
                != second[index].calls.size()) {
            throw std::runtime_error(
                "compiler-fact isolated facts changed between "
                "identical passes");
        }
    }
    fact_contract_detail::run_bridge_contracts();
    fact_contract_detail::run_callback_root_contracts();
    fact_contract_detail::run_runtime_probe_contracts();
    fact_contract_detail::
        run_named_module_structural_nttp_usr_contract();
    fact_contract_detail::
        run_definition_backed_template_redeclaration_contract();
    fact_contract_detail::
        run_distinct_declaration_provenance_candidate_conflict_contract();
    fact_contract_detail::
        run_incoherent_pg_candidate_range_contract();
}

}  // namespace bioseq::codegen::safety
