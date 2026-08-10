module;

/*
 * PostgreSQL-side control DSO for runtime safety validation.
 *
 * The Bioseq extension communicates with this helper through a backend-local
 * rendezvous API. Keeping the SQL-callable control functions here preserves
 * the extension DSO's generated fmgr export closure.
 */

extern "C" {
#include "postgres.h"

#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/errcodes.h"
#include "utils/guc_tables.h"
#include "varatt.h"
}

#include <common/postgres/support/safety_runtime_probe_abi.inc>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

#include <dlfcn.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

export module bioseq.tests.integration.support.safety_runtime_control;

extern "C" {
PG_MODULE_MAGIC;
}

namespace {

namespace probe_abi =
    bioseq::postgres::support::safety_runtime_probe_abi;

constexpr std::size_t token_capacity = 65;
constexpr std::size_t profile_capacity = 32;
constexpr std::size_t site_id_capacity = 80;
constexpr std::size_t fault_key_capacity = 129;
constexpr std::size_t event_name_capacity = 65;
constexpr std::size_t event_capacity = 32;
constexpr std::size_t guc_name_capacity = 256;
constexpr std::size_t guc_contract_row_capacity = 256;
constexpr std::size_t guc_contract_output_capacity =
    64 * 1024;
constexpr std::string_view backend_scoped_vacuum_fault_key =
    "invoke_pg_as_cpp.vacuum_cleanup_restore";
constexpr std::string_view terminal_finish_error_fault_key =
    "scoped_spi_session.emergency_finish_error";
constexpr std::string_view terminal_finish_non_ok_fault_key =
    "scoped_spi_session.emergency_finish_non_ok";
constexpr std::string_view terminal_finish_error_event =
    "scoped_spi_session.terminal_guard.finish_error";
constexpr std::string_view terminal_finish_non_ok_event =
    "scoped_spi_session.terminal_guard.finish_non_ok";
constexpr std::string_view terminal_marker_schema =
    "bioseq.postgres_error_safety.terminal_marker";

enum class probe_relation : std::uint8_t {
    natural_at_site,
    injected_routes_to_site,
    injected_after_site_success,
};

struct event_counter final {
    probe_abi::event_kind kind =
        probe_abi::event_kind::destructor;
    std::array<char, event_name_capacity> name{};
    std::uint64_t count = 0;
};

struct probe_state final {
    bool enabled = false;
    bool inside_target = false;
    bool hooks_installed = false;
    bool api_published = false;
    Oid target_oid = InvalidOid;
    std::array<char, token_capacity> token{};
    std::array<char, profile_capacity> profile{};
    std::array<char, site_id_capacity> site_id{};
    std::array<char, fault_key_capacity> fault_key{};
    std::array<char, PATH_MAX> mapped_target_path{};
    std::array<char, PATH_MAX> marker_directory_path{};
    int marker_directory_fd = -1;
    bool terminal_marker_written = false;
    probe_relation relation =
        probe_relation::natural_at_site;
    std::uint64_t start_count = 0;
    std::uint64_t end_count = 0;
    std::uint64_t abort_count = 0;
    std::uint64_t site_visit_count = 0;
    std::uint64_t fault_trigger_count = 0;
    std::uint64_t causal_sequence = 0;
    std::uint64_t site_visit_sequence = 0;
    std::uint64_t fault_trigger_sequence = 0;
    std::uint64_t invalid_count = 0;
    std::uint64_t overflow_count = 0;
    std::array<event_counter, event_capacity> events{};
    std::size_t event_count = 0;
    needs_fmgr_hook_type previous_needs_hook = nullptr;
    fmgr_hook_type previous_fmgr_hook = nullptr;
    void** api_slot = nullptr;
};

probe_state state{};

[[nodiscard]] bool backend_scoped_case() noexcept {
    return state.enabled
        && std::string_view{state.fault_key.data()}
            == backend_scoped_vacuum_fault_key;
}

[[nodiscard]] bool bounded_copy(
        char* destination,
        std::size_t destination_size,
        std::string_view source) noexcept {
    if (destination == nullptr
        || destination_size == 0
        || source.empty()
        || source.size() >= destination_size
        || source.find('\0') != std::string_view::npos) {
        return false;
    }
    std::memcpy(destination, source.data(), source.size());
    destination[source.size()] = '\0';
    return true;
}

[[nodiscard]] bool lowercase_hex_token(
        std::string_view token) noexcept {
    if (token.size() != 64) {
        return false;
    }
    for (const char character : token) {
        if (!((character >= '0' && character <= '9')
              || (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool valid_profile(
        std::string_view profile) noexcept {
    return profile == "production"
        || profile == "test_indexes";
}

[[nodiscard]] bool valid_site_id(
        std::string_view site_id) noexcept {
    constexpr std::string_view prefix = "probe:";
    return site_id.size() == prefix.size() + 64
        && site_id.substr(0, prefix.size()) == prefix
        && lowercase_hex_token(site_id.substr(prefix.size()));
}

[[nodiscard]] bool parse_relation(
        std::string_view name,
        probe_relation& relation) noexcept {
    if (name == "natural_at_site") {
        relation = probe_relation::natural_at_site;
        return true;
    }
    if (name == "injected_routes_to_site") {
        relation =
            probe_relation::injected_routes_to_site;
        return true;
    }
    if (name == "injected_after_site_success") {
        relation =
            probe_relation::injected_after_site_success;
        return true;
    }
    return false;
}

[[nodiscard]] const char* relation_name(
        probe_relation relation) noexcept {
    switch (relation) {
        case probe_relation::natural_at_site:
            return "natural_at_site";
        case probe_relation::injected_routes_to_site:
            return "injected_routes_to_site";
        case probe_relation::injected_after_site_success:
            return "injected_after_site_success";
    }
    return "invalid";
}

[[nodiscard]] bool valid_event_name(
        std::string_view name) noexcept {
    if (name.empty() || name.size() >= event_name_capacity) {
        return false;
    }
    for (const char character : name) {
        if (!((character >= 'a' && character <= 'z')
              || (character >= '0' && character <= '9')
              || character == '_'
              || character == '.')) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool valid_fault_key(
        std::string_view key) noexcept {
    if (key.empty() || key.size() >= fault_key_capacity) {
        return false;
    }
    for (const char character : key) {
        if (!((character >= 'a' && character <= 'z')
              || (character >= '0' && character <= '9')
              || character == '_'
              || character == '.')) {
            return false;
        }
    }
    return true;
}

void increment(std::uint64_t& value) noexcept {
    if (value == std::numeric_limits<std::uint64_t>::max()) {
        state.overflow_count = 1;
        return;
    }
    ++value;
}

struct terminal_guard_contract final {
    std::string_view fault_key;
    std::string_view event_name;
    std::string_view guard_kind;
};

constexpr std::array<terminal_guard_contract, 2>
    terminal_guard_contracts{{
        {
            terminal_finish_error_fault_key,
            terminal_finish_error_event,
            "finish_error",
        },
        {
            terminal_finish_non_ok_fault_key,
            terminal_finish_non_ok_event,
            "finish_non_ok",
        },
    }};

[[nodiscard]] const terminal_guard_contract*
find_terminal_guard_contract(
        probe_abi::event_kind kind,
        std::string_view event_name) noexcept {
    if (kind != probe_abi::event_kind::cleanup_complete) {
        return nullptr;
    }
    for (const terminal_guard_contract& contract
         : terminal_guard_contracts) {
        if (event_name == contract.event_name) {
            return &contract;
        }
    }
    return nullptr;
}

[[nodiscard]] bool write_complete(
        int file_descriptor,
        std::string_view payload) noexcept {
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const ssize_t written = write(
            file_descriptor,
            payload.data() + offset,
            payload.size() - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

void persist_terminal_marker(
        const terminal_guard_contract& contract) noexcept {
    if (state.terminal_marker_written
        || state.marker_directory_fd < 0
        || contract.fault_key
            != std::string_view{state.fault_key.data()}
        || state.relation
            != probe_relation::injected_routes_to_site
        || state.fault_trigger_count != 1
        || state.site_visit_count != 1
        || state.fault_trigger_sequence != 1
        || state.site_visit_sequence != 2
        || MyProcPid <= 0) {
        increment(state.invalid_count);
        return;
    }

    struct stat directory_status {};
    if (fstat(
            state.marker_directory_fd,
            &directory_status) != 0
        || !S_ISDIR(directory_status.st_mode)
        || directory_status.st_uid != geteuid()
        || (directory_status.st_mode & 07777) != 0700) {
        increment(state.invalid_count);
        return;
    }

    std::array<char, 96> marker_name{};
    const int marker_name_size = pg_snprintf(
        marker_name.data(),
        marker_name.size(),
        "terminal-%s.marker",
        state.token.data());
    if (marker_name_size <= 0
        || static_cast<std::size_t>(marker_name_size)
            >= marker_name.size()) {
        increment(state.invalid_count);
        return;
    }

    std::array<char, 768> payload{};
    const int payload_size = pg_snprintf(
        payload.data(),
        payload.size(),
        "{\"schema\":\"%.*s\",\"schema_revision\":1,"
        "\"token\":\"%s\","
        "\"site_id\":\"%s\",\"fault_key\":\"%s\","
        "\"guard_kind\":\"%.*s\",\"backend_pid\":%d}\n",
        static_cast<int>(terminal_marker_schema.size()),
        terminal_marker_schema.data(),
        state.token.data(),
        state.site_id.data(),
        state.fault_key.data(),
        static_cast<int>(contract.guard_kind.size()),
        contract.guard_kind.data(),
        MyProcPid);
    if (payload_size <= 0
        || static_cast<std::size_t>(payload_size)
            >= payload.size()) {
        increment(state.invalid_count);
        return;
    }

    const int marker_fd = openat(
        state.marker_directory_fd,
        marker_name.data(),
        O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC,
        0600);
    if (marker_fd < 0) {
        increment(state.invalid_count);
        return;
    }

    bool durable = fchmod(marker_fd, 0600) == 0;
    struct stat marker_status {};
    durable = durable
        && fstat(marker_fd, &marker_status) == 0
        && S_ISREG(marker_status.st_mode)
        && marker_status.st_uid == geteuid()
        && (marker_status.st_mode & 07777) == 0600
        && marker_status.st_nlink == 1
        && write_complete(
            marker_fd,
            std::string_view{
                payload.data(),
                static_cast<std::size_t>(payload_size)})
        && fsync(marker_fd) == 0;
    if (close(marker_fd) != 0) {
        durable = false;
    }
    if (durable
        && fsync(state.marker_directory_fd) == 0) {
        state.terminal_marker_written = true;
        return;
    }
    increment(state.invalid_count);
}

void append_event(
        probe_abi::event_kind kind,
        std::string_view name) noexcept {
    if (!valid_event_name(name)) {
        increment(state.invalid_count);
        return;
    }
    for (std::size_t index = 0;
         index < state.event_count;
         ++index) {
        event_counter& event = state.events[index];
        if (event.kind == kind
            && name == std::string_view{event.name.data()}) {
            increment(event.count);
            return;
        }
    }
    if (state.event_count >= state.events.size()) {
        increment(state.overflow_count);
        return;
    }
    event_counter& event = state.events[state.event_count];
    if (!bounded_copy(
            event.name.data(), event.name.size(), name)) {
        increment(state.invalid_count);
        return;
    }
    event.kind = kind;
    event.count = 1;
    ++state.event_count;
}

void record_event_api(
        std::uint32_t raw_kind,
        const char* raw_name,
        std::size_t name_size) noexcept {
    if (!state.enabled
        || (!state.inside_target
            && !backend_scoped_case())
        || raw_name == nullptr) {
        increment(state.invalid_count);
        return;
    }
    const auto kind =
        static_cast<probe_abi::event_kind>(raw_kind);
    if (kind != probe_abi::event_kind::destructor
        && kind != probe_abi::event_kind::cleanup_complete) {
        increment(state.invalid_count);
        return;
    }
    const std::string_view event_name{raw_name, name_size};
    append_event(kind, event_name);
    if (const terminal_guard_contract* const contract =
            find_terminal_guard_contract(kind, event_name);
        contract != nullptr) {
        persist_terminal_marker(*contract);
    }
}

void record_site_visit_api(
        const char* raw_site_reference,
        std::size_t site_reference_size) noexcept {
    if (!state.enabled
        || (!state.inside_target
            && !backend_scoped_case())
        || raw_site_reference == nullptr) {
        increment(state.invalid_count);
        return;
    }
    const std::string_view site_reference{
        raw_site_reference, site_reference_size};
    const bool sealed_site_reference =
        valid_site_id(site_reference);
    const bool literal_fault_reference =
        valid_fault_key(site_reference);
    if (!sealed_site_reference
        && !literal_fault_reference) {
        increment(state.invalid_count);
        return;
    }
    const bool exact_sealed_site =
        sealed_site_reference
        && site_reference
            == std::string_view{state.site_id.data()};
    const bool exact_literal_fault =
        literal_fault_reference
        && site_reference
            == std::string_view{state.fault_key.data()};
    if (!exact_sealed_site && !exact_literal_fault) {
        return;
    }
    if (state.site_visit_count != 0) {
        increment(state.invalid_count);
        return;
    }
    switch (state.relation) {
        case probe_relation::natural_at_site:
        case probe_relation::injected_after_site_success:
            if (state.fault_trigger_count != 0) {
                increment(state.invalid_count);
                return;
            }
            break;
        case probe_relation::injected_routes_to_site:
            if (state.fault_trigger_count != 1) {
                increment(state.invalid_count);
                return;
            }
            break;
    }
    increment(state.site_visit_count);
    increment(state.causal_sequence);
    state.site_visit_sequence = state.causal_sequence;
}

std::uint32_t consume_fault_api(
        const char* raw_key,
        std::size_t key_size) noexcept {
    if (!state.enabled
        || (!state.inside_target
            && !backend_scoped_case())
        || raw_key == nullptr) {
        increment(state.invalid_count);
        return 0;
    }
    const std::string_view key{raw_key, key_size};
    if (!valid_fault_key(key)) {
        increment(state.invalid_count);
        return 0;
    }
    if (key != std::string_view{state.fault_key.data()}) {
        return 0;
    }
    if (state.relation == probe_relation::natural_at_site
        || state.fault_trigger_count != 0) {
        increment(state.invalid_count);
        return 0;
    }
    if ((state.relation
             == probe_relation::injected_routes_to_site
         && state.site_visit_count != 0)
        || (state.relation
                == probe_relation::injected_after_site_success
            && state.site_visit_count != 1)) {
        increment(state.invalid_count);
        return 0;
    }
    increment(state.fault_trigger_count);
    increment(state.causal_sequence);
    state.fault_trigger_sequence = state.causal_sequence;
    return state.fault_trigger_count == 1 ? 1U : 0U;
}

const probe_abi::probe_api api{
    .abi_revision = probe_abi::k_abi_revision,
    .struct_size = sizeof(probe_abi::probe_api),
    .record_event = &record_event_api,
    .record_site_visit = &record_site_visit_api,
    .consume_fault = &consume_fault_api,
};

[[nodiscard]] bool needs_hook_callback(Oid function_oid) {
    if (state.enabled && function_oid == state.target_oid) {
        return true;
    }
    return state.previous_needs_hook != nullptr
        && state.previous_needs_hook(function_oid);
}

void fmgr_hook_callback(
        FmgrHookEventType event,
        FmgrInfo* function,
        Datum* private_state) {
    if (state.previous_fmgr_hook != nullptr) {
        state.previous_fmgr_hook(
            event, function, private_state);
    }
    if (!state.enabled
        || function == nullptr
        || function->fn_oid != state.target_oid) {
        return;
    }
    switch (event) {
        case FHET_START:
            if (state.inside_target) {
                increment(state.invalid_count);
            }
            increment(state.start_count);
            state.inside_target = true;
            return;
        case FHET_END:
            if (!state.inside_target) {
                increment(state.invalid_count);
            }
            increment(state.end_count);
            state.inside_target = false;
            return;
        case FHET_ABORT:
            if (!state.inside_target) {
                increment(state.invalid_count);
            }
            increment(state.abort_count);
            state.inside_target = false;
            append_event(
                probe_abi::event_kind::postgres_cleanup,
                "fmgr_abort_observed");
            return;
    }
    increment(state.invalid_count);
}

void clear_case_state() noexcept {
    if (state.marker_directory_fd >= 0) {
        if (close(state.marker_directory_fd) != 0) {
            increment(state.invalid_count);
        }
        state.marker_directory_fd = -1;
    }
    state.enabled = false;
    state.inside_target = false;
    state.target_oid = InvalidOid;
    state.token = {};
    state.profile = {};
    state.site_id = {};
    state.fault_key = {};
    state.mapped_target_path = {};
    state.marker_directory_path = {};
    state.terminal_marker_written = false;
    state.relation = probe_relation::natural_at_site;
    state.start_count = 0;
    state.end_count = 0;
    state.abort_count = 0;
    state.site_visit_count = 0;
    state.fault_trigger_count = 0;
    state.causal_sequence = 0;
    state.site_visit_sequence = 0;
    state.fault_trigger_sequence = 0;
    state.invalid_count = 0;
    state.overflow_count = 0;
    state.events = {};
    state.event_count = 0;
    state.api_slot = nullptr;
}

void publish_api() {
    void** const slot =
        find_rendezvous_variable(probe_abi::k_rendezvous_name);
    if (slot == nullptr
        || (*slot != nullptr
            && *slot != const_cast<probe_abi::probe_api*>(&api))) {
        ereport(
            ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq safety runtime rendezvous is unavailable")));
    }
    *slot = const_cast<probe_abi::probe_api*>(&api);
    state.api_slot = slot;
    state.api_published = true;
}

void unpublish_api() {
    if (!state.api_published) {
        return;
    }
    if (state.api_slot == nullptr
        || *state.api_slot
            != const_cast<probe_abi::probe_api*>(&api)) {
        ereport(
            ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq safety runtime rendezvous changed while armed")));
    }
    *state.api_slot = nullptr;
    state.api_slot = nullptr;
    state.api_published = false;
}

void install_hooks() {
    if (state.hooks_installed
        || needs_fmgr_hook == &needs_hook_callback
        || fmgr_hook == &fmgr_hook_callback) {
        ereport(
            ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq safety runtime fmgr hook is already installed")));
    }
    state.previous_needs_hook = needs_fmgr_hook;
    state.previous_fmgr_hook = fmgr_hook;
    needs_fmgr_hook = &needs_hook_callback;
    fmgr_hook = &fmgr_hook_callback;
    state.hooks_installed = true;
}

void uninstall_hooks() {
    if (!state.hooks_installed) {
        return;
    }
    if (needs_fmgr_hook != &needs_hook_callback
        || fmgr_hook != &fmgr_hook_callback) {
        ereport(
            ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq safety runtime fmgr hook changed while armed")));
    }
    needs_fmgr_hook = state.previous_needs_hook;
    fmgr_hook = state.previous_fmgr_hook;
    state.previous_needs_hook = nullptr;
    state.previous_fmgr_hook = nullptr;
    state.hooks_installed = false;
}

[[nodiscard]] std::string_view text_view(text* value) noexcept {
    return std::string_view{
        VARDATA_ANY(value),
        static_cast<std::size_t>(VARSIZE_ANY_EXHDR(value))};
}

void require_token(std::string_view token) {
    if (!state.enabled
        || token != std::string_view{state.token.data()}) {
        ereport(
            ERROR,
            (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
             errmsg("bioseq safety runtime probe token does not match")));
    }
}

[[nodiscard]] const char* event_kind_name(
        probe_abi::event_kind kind) noexcept {
    switch (kind) {
        case probe_abi::event_kind::destructor:
            return "destructor";
        case probe_abi::event_kind::cleanup_complete:
            return "cleanup";
        case probe_abi::event_kind::postgres_cleanup:
            return "postgres_cleanup";
    }
    return "invalid";
}

struct marker_directory_attestation final {
    int file_descriptor = -1;
    std::array<char, PATH_MAX> canonical_path{};
};

[[nodiscard]] marker_directory_attestation
attest_marker_directory(std::string_view expected_path) {
    if (expected_path.empty()
        || expected_path.size() >= PATH_MAX
        || expected_path.front() != '/'
        || expected_path.find('\0') != std::string_view::npos
        || expected_path.find_first_of("\t\r\n")
            != std::string_view::npos) {
        ereport(
            ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("bioseq safety runtime marker directory is invalid")));
    }

    std::array<char, PATH_MAX> requested_path{};
    if (!bounded_copy(
            requested_path.data(),
            requested_path.size(),
            expected_path)) {
        ereport(
            ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("bioseq safety runtime marker directory is invalid")));
    }

    std::array<char, PATH_MAX> resolved_path{};
    std::array<char, PATH_MAX> resolved_data_directory{};
    if (DataDir == nullptr
        || realpath(
               requested_path.data(),
               resolved_path.data()) == nullptr
        || realpath(
               DataDir,
               resolved_data_directory.data()) == nullptr
        || expected_path
            != std::string_view{resolved_path.data()}) {
        ereport(
            ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg(
                 "bioseq safety runtime marker directory is not canonical")));
    }

    const std::string_view data_directory{
        resolved_data_directory.data()};
    const std::string_view marker_directory{
        resolved_path.data()};
    if (data_directory == "/"
        || marker_directory.size()
            <= data_directory.size() + 1
        || marker_directory.substr(0, data_directory.size())
            != data_directory
        || marker_directory[data_directory.size()] != '/'
        || marker_directory.substr(data_directory.size() + 1)
               .find('/')
            != std::string_view::npos) {
        ereport(
            ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg(
                 "bioseq safety runtime marker directory must be "
                 "a direct child of DataDir")));
    }

    struct stat path_status {};
    if (lstat(requested_path.data(), &path_status) != 0
        || !S_ISDIR(path_status.st_mode)
        || path_status.st_uid != geteuid()
        || (path_status.st_mode & 07777) != 0700) {
        ereport(
            ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg(
                 "bioseq safety runtime marker directory is not "
                 "a secure owned 0700 directory")));
    }

    const int data_directory_fd = open(
        resolved_data_directory.data(),
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (data_directory_fd < 0) {
        ereport(
            ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg(
                 "bioseq safety runtime cannot securely open DataDir")));
    }
    const std::string_view child_name =
        marker_directory.substr(data_directory.size() + 1);
    std::array<char, PATH_MAX> child_name_buffer{};
    if (!bounded_copy(
            child_name_buffer.data(),
            child_name_buffer.size(),
            child_name)) {
        close(data_directory_fd);
        ereport(
            ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("bioseq safety runtime marker directory is invalid")));
    }
    const int marker_directory_fd = openat(
        data_directory_fd,
        child_name_buffer.data(),
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    const int data_directory_close_status =
        close(data_directory_fd);
    if (marker_directory_fd < 0
        || data_directory_close_status != 0) {
        if (marker_directory_fd >= 0) {
            close(marker_directory_fd);
        }
        ereport(
            ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg(
                 "bioseq safety runtime cannot securely open "
                 "the marker directory")));
    }

    struct stat descriptor_status {};
    if (fstat(marker_directory_fd, &descriptor_status) != 0
        || !S_ISDIR(descriptor_status.st_mode)
        || descriptor_status.st_dev != path_status.st_dev
        || descriptor_status.st_ino != path_status.st_ino
        || descriptor_status.st_uid != geteuid()
        || (descriptor_status.st_mode & 07777) != 0700) {
        close(marker_directory_fd);
        ereport(
            ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg(
                 "bioseq safety runtime marker directory changed "
                 "during attestation")));
    }

    marker_directory_attestation result{};
    result.file_descriptor = marker_directory_fd;
    if (!bounded_copy(
            result.canonical_path.data(),
            result.canonical_path.size(),
            marker_directory)) {
        close(marker_directory_fd);
        ereport(
            ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg(
                 "bioseq safety runtime marker directory is too long")));
    }
    return result;
}

void attest_target_image(
        Oid target_oid,
        std::string_view expected_path) {
    if (expected_path.empty()
        || expected_path.size() >= PATH_MAX
        || expected_path.find('\0') != std::string_view::npos
        || expected_path.find_first_of("\t\r\n")
            != std::string_view::npos) {
        ereport(
            ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("bioseq safety runtime target path is invalid")));
    }
    FmgrInfo function{};
    fmgr_info(target_oid, &function);
    Dl_info image{};
    if (function.fn_addr == nullptr
        || dladdr(
               reinterpret_cast<void*>(function.fn_addr),
               &image)
            == 0
        || image.dli_fname == nullptr) {
        ereport(
            ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq safety runtime cannot resolve target image")));
    }
    std::array<char, PATH_MAX> resolved{};
    if (realpath(image.dli_fname, resolved.data()) == nullptr
        || expected_path
            != std::string_view{resolved.data()}) {
        ereport(
            ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq safety runtime target image differs")));
    }
    if (!bounded_copy(
            state.mapped_target_path.data(),
            state.mapped_target_path.size(),
            std::string_view{resolved.data()})) {
        ereport(
            ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("bioseq safety runtime target path is too long")));
    }
}

struct guc_hook_presence final {
    bool check = false;
    bool assign = false;
    bool show = false;
};

[[nodiscard]] bool inspect_guc_hooks(
        const config_generic* record,
        guc_hook_presence& hooks) noexcept {
    if (record == nullptr) {
        return false;
    }
    switch (record->vartype) {
        case PGC_BOOL: {
            const auto* const typed =
                reinterpret_cast<const config_bool*>(record);
            hooks = {
                typed->check_hook != nullptr,
                typed->assign_hook != nullptr,
                typed->show_hook != nullptr,
            };
            return true;
        }
        case PGC_INT: {
            const auto* const typed =
                reinterpret_cast<const config_int*>(record);
            hooks = {
                typed->check_hook != nullptr,
                typed->assign_hook != nullptr,
                typed->show_hook != nullptr,
            };
            return true;
        }
        case PGC_REAL: {
            const auto* const typed =
                reinterpret_cast<const config_real*>(record);
            hooks = {
                typed->check_hook != nullptr,
                typed->assign_hook != nullptr,
                typed->show_hook != nullptr,
            };
            return true;
        }
        case PGC_STRING: {
            const auto* const typed =
                reinterpret_cast<const config_string*>(record);
            hooks = {
                typed->check_hook != nullptr,
                typed->assign_hook != nullptr,
                typed->show_hook != nullptr,
            };
            return true;
        }
        case PGC_ENUM: {
            const auto* const typed =
                reinterpret_cast<const config_enum*>(record);
            hooks = {
                typed->check_hook != nullptr,
                typed->assign_hook != nullptr,
                typed->show_hook != nullptr,
            };
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool valid_bioseq_guc_name(
        std::string_view name) noexcept {
    constexpr std::string_view prefix = "bioseq.";
    if (name.size() <= prefix.size()
        || name.substr(0, prefix.size()) != prefix
        || name.back() == '.'
        || name.find("..") != std::string_view::npos) {
        return false;
    }
    for (const char character : name) {
        if (!((character >= 'a' && character <= 'z')
              || (character >= '0' && character <= '9')
              || character == '_'
              || character == '.')) {
            return false;
        }
    }
    return true;
}

}  // namespace

extern "C" {

PG_FUNCTION_INFO_V1(bioseq_safety_runtime_probe_arm);
PG_FUNCTION_INFO_V1(bioseq_safety_runtime_probe_snapshot);
PG_FUNCTION_INFO_V1(bioseq_safety_runtime_probe_disarm);
PG_FUNCTION_INFO_V1(bioseq_safety_runtime_guc_internal_contract);

PGDLLEXPORT Datum
bioseq_safety_runtime_guc_internal_contract(PG_FUNCTION_ARGS) {
    if (PG_NARGS() != 0) {
        ereport(
            ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg(
                 "bioseq safety runtime GUC internal contract "
                 "requires no arguments")));
    }

    int variable_count = 0;
    config_generic** const variables =
        get_guc_variables(&variable_count);
    if (variable_count < 0
        || (variable_count != 0 && variables == nullptr)) {
        ereport(
            ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg(
                 "bioseq safety runtime cannot enumerate GUCs")));
    }

    std::array<
        config_generic*,
        guc_contract_row_capacity> records{};
    std::size_t record_count = 0;
    constexpr std::string_view prefix = "bioseq.";
    for (int index = 0; index < variable_count; ++index) {
        config_generic* const record = variables[index];
        if (record == nullptr || record->name == nullptr
            || std::strncmp(
                   record->name,
                   prefix.data(),
                   prefix.size()) != 0) {
            continue;
        }
        const std::size_t name_size =
            strnlen(record->name, guc_name_capacity);
        if (name_size == guc_name_capacity
            || !valid_bioseq_guc_name(
                std::string_view{record->name, name_size})) {
            ereport(
                ERROR,
                (errcode(ERRCODE_INVALID_NAME),
                 errmsg(
                     "bioseq safety runtime encountered "
                     "a malformed bioseq GUC name")));
        }
        if (record_count >= records.size()) {
            ereport(
                ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg(
                     "bioseq safety runtime GUC internal "
                     "contract exceeds 256 rows")));
        }
        records[record_count] = record;
        ++record_count;
    }

    std::sort(
        records.begin(),
        records.begin()
            + static_cast<std::ptrdiff_t>(record_count),
        [](const config_generic* left,
           const config_generic* right) {
            return std::strcmp(left->name, right->name) < 0;
        });
    for (std::size_t index = 1;
         index < record_count;
         ++index) {
        if (std::strcmp(
                records[index - 1]->name,
                records[index]->name) == 0) {
            ereport(
                ERROR,
                (errcode(ERRCODE_DUPLICATE_OBJECT),
                 errmsg(
                     "bioseq safety runtime encountered "
                     "a duplicate bioseq GUC name")));
        }
    }

    StringInfoData output;
    initStringInfo(&output);
    appendStringInfoString(
        &output,
        "bioseq_guc_internal_contract\nrevision=1\n");
    for (std::size_t index = 0;
         index < record_count;
         ++index) {
        const config_generic* const record = records[index];
        guc_hook_presence hooks{};
        if (!inspect_guc_hooks(record, hooks)) {
            ereport(
                ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg(
                     "bioseq safety runtime encountered "
                     "an unknown bioseq GUC type")));
        }
        std::array<char, 512> row{};
        const int row_size = pg_snprintf(
            row.data(),
            row.size(),
            "%s\t%d\t%d\t%d\t%d\n",
            record->name,
            record->flags,
            hooks.check ? 1 : 0,
            hooks.assign ? 1 : 0,
            hooks.show ? 1 : 0);
        if (row_size <= 0
            || static_cast<std::size_t>(row_size)
                >= row.size()
            || static_cast<std::size_t>(output.len)
                    + static_cast<std::size_t>(row_size)
                > guc_contract_output_capacity) {
            ereport(
                ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg(
                     "bioseq safety runtime GUC internal "
                     "contract exceeds 64 KiB")));
        }
        appendBinaryStringInfo(
            &output, row.data(), row_size);
    }
    PG_RETURN_TEXT_P(
        cstring_to_text_with_len(output.data, output.len));
}

PGDLLEXPORT Datum
bioseq_safety_runtime_probe_arm(PG_FUNCTION_ARGS) {
    if (PG_NARGS() != 8) {
        ereport(
            ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("bioseq safety runtime arm requires eight arguments")));
    }
    for (int argument = 0; argument < 8; ++argument) {
        if (PG_ARGISNULL(argument)) {
            ereport(
                ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg(
                     "bioseq safety runtime arm arguments "
                     "must not be null")));
        }
    }
    const Oid target_oid = PG_GETARG_OID(0);
    const std::string_view token =
        text_view(PG_GETARG_TEXT_PP(1));
    const std::string_view site_id =
        text_view(PG_GETARG_TEXT_PP(2));
    const std::string_view fault_key =
        text_view(PG_GETARG_TEXT_PP(3));
    const std::string_view relation_name_value =
        text_view(PG_GETARG_TEXT_PP(4));
    const std::string_view profile =
        text_view(PG_GETARG_TEXT_PP(5));
    const std::string_view expected_target_path =
        text_view(PG_GETARG_TEXT_PP(6));
    const std::string_view marker_directory_path =
        text_view(PG_GETARG_TEXT_PP(7));
    probe_relation relation{};

    if (!OidIsValid(target_oid)
        || !lowercase_hex_token(token)
        || !valid_site_id(site_id)
        || !valid_fault_key(fault_key)
        || !parse_relation(relation_name_value, relation)
        || !valid_profile(profile)
        || state.enabled
        || state.hooks_installed
        || state.api_published) {
        ereport(
            ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("bioseq safety runtime arm input or state is invalid")));
    }

    clear_case_state();
    state.target_oid = target_oid;
    if (!bounded_copy(
            state.token.data(), state.token.size(), token)
        || !bounded_copy(
            state.profile.data(), state.profile.size(), profile)
        || !bounded_copy(
            state.site_id.data(),
            state.site_id.size(),
            site_id)
        || !bounded_copy(
            state.fault_key.data(),
            state.fault_key.size(),
            fault_key)) {
        clear_case_state();
        ereport(
            ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("bioseq safety runtime arm input is too long")));
    }
    attest_target_image(target_oid, expected_target_path);
    const marker_directory_attestation marker_directory =
        attest_marker_directory(marker_directory_path);
    state.marker_directory_fd =
        marker_directory.file_descriptor;
    state.marker_directory_path =
        marker_directory.canonical_path;
    state.relation = relation;
    publish_api();
    install_hooks();
    state.enabled = true;
    PG_RETURN_VOID();
}

PGDLLEXPORT Datum
bioseq_safety_runtime_probe_snapshot(PG_FUNCTION_ARGS) {
    if (PG_NARGS() != 1) {
        ereport(
            ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("bioseq safety runtime snapshot requires one argument")));
    }
    require_token(text_view(PG_GETARG_TEXT_PP(0)));
    if (needs_fmgr_hook != &needs_hook_callback
        || fmgr_hook != &fmgr_hook_callback
        || state.api_slot == nullptr
        || *state.api_slot
            != const_cast<probe_abi::probe_api*>(&api)) {
        ereport(
            ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq safety runtime control state changed")));
    }

    StringInfoData output;
    initStringInfo(&output);
    appendStringInfo(
        &output,
        "v2\t%s\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t"
        "%s\t%s\t%s\t%s\t%s\t%llu\t%llu\n",
        state.profile.data(),
        static_cast<unsigned long long>(state.start_count),
        static_cast<unsigned long long>(state.end_count),
        static_cast<unsigned long long>(state.abort_count),
        static_cast<unsigned long long>(state.site_visit_count),
        static_cast<unsigned long long>(state.fault_trigger_count),
        static_cast<unsigned long long>(state.invalid_count),
        static_cast<unsigned long long>(state.overflow_count),
        state.inside_target ? "true" : "false",
        state.mapped_target_path.data(),
        state.site_id.data(),
        state.fault_key.data(),
        relation_name(state.relation),
        static_cast<unsigned long long>(
            state.site_visit_sequence),
        static_cast<unsigned long long>(
            state.fault_trigger_sequence));
    for (std::size_t index = 0;
         index < state.event_count;
         ++index) {
        const event_counter& event = state.events[index];
        appendStringInfo(
            &output,
            "event\t%s\t%s\t%llu\n",
            event_kind_name(event.kind),
            event.name.data(),
            static_cast<unsigned long long>(event.count));
    }
    PG_RETURN_TEXT_P(
        cstring_to_text_with_len(output.data, output.len));
}

PGDLLEXPORT Datum
bioseq_safety_runtime_probe_disarm(PG_FUNCTION_ARGS) {
    if (PG_NARGS() != 1) {
        ereport(
            ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("bioseq safety runtime disarm requires one argument")));
    }
    require_token(text_view(PG_GETARG_TEXT_PP(0)));
    uninstall_hooks();
    unpublish_api();
    clear_case_state();
    PG_RETURN_VOID();
}

}  // extern "C"
