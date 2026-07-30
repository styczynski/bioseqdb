module;

#include <oneapi/tbb/task_group.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

export module bioseq.test.postgres:fixture;

import bioseq.libpq.client;
import :cluster;
import :report;
import :resource_sampler;

export namespace bioseq::postgres_test {

/// @brief Contract/assertion failure in the source-neutral fixture layer.
class fixture_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// @brief Structured observation of one expected PostgreSQL error.
struct sql_error_observation {
    std::string sqlstate;
    std::string message;
    std::string detail;
    std::string hint;
    std::string severity;
};

/// @brief Requires and returns one non-null text cell.
[[nodiscard]] inline std::string_view single_text_cell(
    const bioseq::libpq::result& result) {
    if (result.rows() != 1 || result.columns() != 1
        || result.is_null(0, 0)) {
        throw fixture_error("expected one non-null SQL result cell");
    }
    return result.value(0, 0);
}

/// @brief Requires and parses one signed 64-bit result cell.
[[nodiscard]] inline std::int64_t single_int64_cell(
    const bioseq::libpq::result& result) {
    const std::string_view text = single_text_cell(result);
    std::int64_t value{};
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw fixture_error("SQL result cell is not an int64: "
                            + std::string(text));
    }
    return value;
}

/// @brief Requires and parses one PostgreSQL Boolean text cell.
[[nodiscard]] inline bool single_bool_cell(
    const bioseq::libpq::result& result) {
    const std::string_view text = single_text_cell(result);
    if (text == "t") {
        return true;
    }
    if (text == "f") {
        return false;
    }
    throw fixture_error("SQL result cell is not Boolean: "
                        + std::string(text));
}

/// @brief Executes SQL and requires the exact expected five-character SQLSTATE.
[[nodiscard]] inline sql_error_observation expect_sqlstate(
    bioseq::libpq::connection& connection,
    std::string_view sql,
    std::string_view expected_sqlstate,
    std::chrono::milliseconds deadline,
    bioseq::libpq::transaction_policy policy =
        bioseq::libpq::transaction_policy::require_idle) {
    try {
        static_cast<void>(connection.exec(sql, deadline, policy));
    } catch (const bioseq::libpq::query_error& error) {
        if (error.sqlstate() != expected_sqlstate) {
            throw fixture_error(
                "expected SQLSTATE " + std::string(expected_sqlstate)
                + ", observed " + std::string(error.sqlstate())
                + ": " + error.what());
        }
        return sql_error_observation{
            .sqlstate = std::string(error.sqlstate()),
            .message = error.what(),
            .detail = std::string(error.detail()),
            .hint = std::string(error.hint()),
            .severity = std::string(error.severity())};
    }
    throw fixture_error("expected SQLSTATE "
                        + std::string(expected_sqlstate)
                        + ", but SQL succeeded");
}

/// @brief Polls a controller predicate until success or an absolute deadline.
template <class Predicate>
[[nodiscard]] bool wait_until(
    Predicate&& predicate,
    std::chrono::steady_clock::time_point deadline,
    std::chrono::milliseconds poll_interval =
        std::chrono::milliseconds(10)) {
    if (poll_interval <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("poll interval must be positive");
    }
    for (;;) {
        if (std::invoke(predicate)) {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(
            std::min(poll_interval,
                     std::chrono::duration_cast<std::chrono::milliseconds>(
                         deadline - now)));
    }
}

/// @brief Starts a private cluster and optionally installs all staged extensions.
class postgres_fixture {
public:
    explicit postgres_fixture(cluster_options options,
                              bool install_extensions = true)
        : extension_names_(collect_extension_names(options.extensions)),
          cluster_(std::move(options)) {
        try {
            cluster_.initialize();
            cluster_.start();
            if (automatic_report_collection_enabled()) {
                resource_sampler_.emplace(
                    cluster_.root_directory(),
                    cluster_.data_directory());
                resource_sampler_->observe_phase_boundary(
                    "cluster-started");
            }
            admin_.emplace(cluster_.connect(std::chrono::seconds(10)));
            attach_process_sql_transcript(*admin_, "admin");
            if (install_extensions) {
                this->install_extensions();
            }
        } catch (...) {
            cleanup_and_record();
            throw;
        }
    }

    postgres_fixture(const postgres_fixture&) = delete;
    postgres_fixture& operator=(const postgres_fixture&) = delete;

    ~postgres_fixture() noexcept {
        cleanup_and_record();
    }

    /// @brief Returns the owned cluster for restart/crash/backend controls.
    [[nodiscard]] private_cluster& cluster() noexcept { return cluster_; }
    [[nodiscard]] const private_cluster& cluster() const noexcept {
        return cluster_;
    }

    /// @brief Returns the long-lived administrative connection.
    [[nodiscard]] bioseq::libpq::connection& admin() {
        if (!admin_ || !admin_->is_idle()) {
            admin_.reset();
            admin_.emplace(cluster_.connect(std::chrono::seconds(10)));
            attach_process_sql_transcript(*admin_, "admin");
        }
        return *admin_;
    }

    /// @brief Replaces the admin session after an intentional server restart.
    void reconnect_admin() {
        admin_.reset();
        admin_.emplace(cluster_.connect(std::chrono::seconds(10)));
        attach_process_sql_transcript(*admin_, "admin");
    }

    /// @brief Opens an independent session under a caller deadline.
    [[nodiscard]] bioseq::libpq::connection connect(
        std::chrono::milliseconds deadline = std::chrono::seconds(10)) const {
        bioseq::libpq::connection connection =
            cluster_.connect(deadline);
        attach_process_sql_transcript(connection, "session");
        return connection;
    }

    /// @brief Runs ordinary CREATE EXTENSION in artifact-vector order.
    void install_extensions() {
        if (extensions_installed_) {
            throw fixture_error("fixture extensions are already installed");
        }
        for (const std::string& name : extension_names_) {
            const std::string identifier = admin().escape_identifier(name);
            static_cast<void>(admin().exec(
                "CREATE EXTENSION " + identifier,
                std::chrono::seconds(60)));
        }
        extensions_installed_ = true;
    }

    /// @brief Forces an exact resource sample at a workload phase boundary.
    void observe_resource_phase(std::string_view phase) noexcept {
        if (resource_sampler_) {
            resource_sampler_->observe_phase_boundary(phase);
        }
    }

private:
    void cleanup_and_record() noexcept {
        if (cleanup_recorded_) {
            return;
        }
        cleanup_recorded_ = true;
        try {
            admin_.reset();
            if (!automatic_report_collection_enabled()) {
                static_cast<void>(cluster_.cleanup());
                return;
            }
            const std::filesystem::path root =
                cluster_.root_directory();
            resource_report resources =
                capture_resource_report(
                    std::span<const std::filesystem::path>(&root, 1));
            if (resource_sampler_) {
                resources.postgres = resource_sampler_->finish();
                resource_sampler_.reset();
            }
            const cluster_cleanup_result& cleanup = cluster_.cleanup();
            record_cleanup_observation(
                root, cleanup.succeeded, cleanup.detail, resources);
        } catch (const std::exception& error) {
            record_cleanup_observation(
                cluster_.root_directory(), false, error.what());
        } catch (...) {
            record_cleanup_observation(
                cluster_.root_directory(), false,
                "unknown fixture cleanup failure");
        }
    }

    /// @brief Copies artifact names before cluster_options is moved.
    [[nodiscard]] static std::vector<std::string> collect_extension_names(
        std::span<const extension_artifact> extensions) {
        std::vector<std::string> names;
        names.reserve(extensions.size());
        for (const extension_artifact& extension : extensions) {
            names.push_back(extension.name);
        }
        return names;
    }

    std::vector<std::string> extension_names_;
    private_cluster cluster_;
    std::optional<bioseq::libpq::connection> admin_;
    std::optional<resource_sampler> resource_sampler_;
    bool extensions_installed_{};
    bool cleanup_recorded_{};
};

/// @brief Restores one session GUC to its exact prior value on destruction.
class scoped_guc {
public:
    scoped_guc(bioseq::libpq::connection& connection,
               std::string name,
               std::string_view value,
               std::chrono::milliseconds deadline =
                   std::chrono::seconds(10))
        : connection_(&connection),
          name_(std::move(name)),
          deadline_(deadline) {
        validate_setting_name(name_);
        const std::string literal_name = connection_->escape_literal(name_);
        const bioseq::libpq::result previous = connection_->exec(
            "SELECT current_setting(" + literal_name + ", true)",
            deadline_, bioseq::libpq::transaction_policy::allow_open);
        if (previous.rows() != 1 || previous.columns() != 1) {
            throw fixture_error("current_setting returned unexpected shape");
        }
        if (!previous.is_null(0, 0)) {
            previous_ = std::string(previous.value(0, 0));
        }
        set(value);
    }

    scoped_guc(const scoped_guc&) = delete;
    scoped_guc& operator=(const scoped_guc&) = delete;

    ~scoped_guc() noexcept {
        if (connection_ == nullptr) {
            return;
        }
        try {
            const std::string identifier =
                render_setting_name(*connection_, name_);
            if (previous_) {
                static_cast<void>(connection_->exec(
                    "SET " + identifier + " TO "
                        + connection_->escape_literal(*previous_),
                    deadline_, bioseq::libpq::transaction_policy::allow_open));
            } else {
                static_cast<void>(connection_->exec(
                    "RESET " + identifier, deadline_,
                    bioseq::libpq::transaction_policy::allow_open));
            }
        } catch (...) {
        }
    }

    /// @brief Changes the scoped GUC while preserving its original restore value.
    void set(std::string_view value) {
        const std::string identifier =
            render_setting_name(*connection_, name_);
        static_cast<void>(connection_->exec(
            "SET " + identifier + " TO "
                + connection_->escape_literal(value),
            deadline_, bioseq::libpq::transaction_policy::allow_open));
    }

private:
    /// @brief Restricts a GUC to dotted identifier syntax.
    static void validate_setting_name(std::string_view name) {
        if (name.empty()) {
            throw fixture_error("GUC name is empty");
        }
        for (const char character : name) {
            const bool allowed =
                (character >= 'a' && character <= 'z')
                || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9')
                || character == '_' || character == '.';
            if (!allowed) {
                throw fixture_error("invalid GUC name: "
                                    + std::string(name));
            }
        }
    }

    /// @brief Quotes every component of a dotted GUC name independently.
    [[nodiscard]] static std::string render_setting_name(
        const bioseq::libpq::connection& connection,
        std::string_view name) {
        std::string rendered;
        std::size_t begin = 0;
        while (begin < name.size()) {
            const std::size_t separator = name.find('.', begin);
            const std::size_t end =
                separator == std::string_view::npos ? name.size() : separator;
            if (end == begin) {
                throw fixture_error("GUC name has an empty component");
            }
            if (!rendered.empty()) {
                rendered.push_back('.');
            }
            rendered += connection.escape_identifier(
                name.substr(begin, end - begin));
            if (separator == std::string_view::npos) {
                break;
            }
            begin = separator + 1;
        }
        return rendered;
    }

    bioseq::libpq::connection* connection_{};
    std::string name_;
    std::optional<std::string> previous_;
    std::chrono::milliseconds deadline_;
};

/// @brief Creates and drops one exact schema around a test scope.
class scoped_schema {
public:
    scoped_schema(bioseq::libpq::connection& connection,
                  std::string name,
                  std::chrono::milliseconds deadline =
                      std::chrono::seconds(10))
        : connection_(&connection),
          name_(std::move(name)),
          deadline_(deadline) {
        const std::string identifier =
            connection_->escape_identifier(name_);
        static_cast<void>(connection_->exec(
            "CREATE SCHEMA " + identifier, deadline_,
            bioseq::libpq::transaction_policy::allow_open));
    }

    scoped_schema(const scoped_schema&) = delete;
    scoped_schema& operator=(const scoped_schema&) = delete;

    ~scoped_schema() noexcept {
        try {
            static_cast<void>(connection_->exec(
                "DROP SCHEMA IF EXISTS "
                    + connection_->escape_identifier(name_) + " CASCADE",
                deadline_, bioseq::libpq::transaction_policy::allow_open));
        } catch (...) {
        }
    }

    [[nodiscard]] std::string_view name() const noexcept { return name_; }

private:
    bioseq::libpq::connection* connection_{};
    std::string name_;
    std::chrono::milliseconds deadline_;
};

/// @brief Holds an explicit transaction across independently bounded commands.
class scoped_transaction {
public:
    explicit scoped_transaction(
        bioseq::libpq::connection& connection,
        std::chrono::milliseconds deadline = std::chrono::seconds(10))
        : connection_(&connection), deadline_(deadline) {
        if (connection_->current_transaction_state()
            != bioseq::libpq::transaction_state::idle) {
            throw fixture_error("scoped_transaction requires an idle session");
        }
        static_cast<void>(connection_->exec(
            "BEGIN", deadline_,
            bioseq::libpq::transaction_policy::allow_open));
    }

    scoped_transaction(const scoped_transaction&) = delete;
    scoped_transaction& operator=(const scoped_transaction&) = delete;

    ~scoped_transaction() noexcept {
        if (active_) {
            try {
                static_cast<void>(connection_->exec(
                    "ROLLBACK", deadline_,
                    bioseq::libpq::transaction_policy::allow_open));
            } catch (...) {
            }
        }
    }

    /// @brief Commits the active transaction.
    void commit() {
        require_active();
        static_cast<void>(connection_->exec(
            "COMMIT", deadline_,
            bioseq::libpq::transaction_policy::allow_open));
        active_ = false;
    }

    /// @brief Rolls back the active transaction.
    void rollback() {
        require_active();
        static_cast<void>(connection_->exec(
            "ROLLBACK", deadline_,
            bioseq::libpq::transaction_policy::allow_open));
        active_ = false;
    }

private:
    void require_active() const {
        if (!active_) {
            throw fixture_error("transaction scope is no longer active");
        }
    }

    bioseq::libpq::connection* connection_{};
    std::chrono::milliseconds deadline_;
    bool active_{true};
};

/// @brief Owns one named savepoint inside an existing transaction.
class scoped_savepoint {
public:
    scoped_savepoint(bioseq::libpq::connection& connection,
                     std::string name,
                     std::chrono::milliseconds deadline =
                         std::chrono::seconds(10))
        : connection_(&connection),
          name_(std::move(name)),
          deadline_(deadline) {
        if (connection_->current_transaction_state()
            != bioseq::libpq::transaction_state::in_transaction) {
            throw fixture_error("savepoint requires an active transaction");
        }
        static_cast<void>(connection_->exec(
            "SAVEPOINT " + connection_->escape_identifier(name_),
            deadline_, bioseq::libpq::transaction_policy::allow_open));
    }

    scoped_savepoint(const scoped_savepoint&) = delete;
    scoped_savepoint& operator=(const scoped_savepoint&) = delete;

    ~scoped_savepoint() noexcept {
        if (!active_) {
            return;
        }
        try {
            static_cast<void>(connection_->exec(
                "ROLLBACK TO SAVEPOINT "
                    + connection_->escape_identifier(name_),
                deadline_, bioseq::libpq::transaction_policy::allow_open));
            static_cast<void>(connection_->exec(
                "RELEASE SAVEPOINT "
                    + connection_->escape_identifier(name_),
                deadline_, bioseq::libpq::transaction_policy::allow_open));
        } catch (...) {
        }
    }

    /// @brief Keeps changes and removes the savepoint.
    void release() {
        if (!active_) {
            throw fixture_error("savepoint is no longer active");
        }
        static_cast<void>(connection_->exec(
            "RELEASE SAVEPOINT " + connection_->escape_identifier(name_),
            deadline_, bioseq::libpq::transaction_policy::allow_open));
        active_ = false;
    }

    /// @brief Rolls back changes and removes the savepoint.
    void rollback() {
        if (!active_) {
            throw fixture_error("savepoint is no longer active");
        }
        static_cast<void>(connection_->exec(
            "ROLLBACK TO SAVEPOINT "
                + connection_->escape_identifier(name_),
            deadline_, bioseq::libpq::transaction_policy::allow_open));
        release();
    }

private:
    bioseq::libpq::connection* connection_{};
    std::string name_;
    std::chrono::milliseconds deadline_;
    bool active_{true};
};

/// @brief Owns a two-phase transaction and cleans it through a fresh session.
class scoped_prepared_transaction {
public:
    scoped_prepared_transaction(
        private_cluster& cluster,
        std::string gid,
        std::chrono::milliseconds deadline = std::chrono::seconds(10))
        : cluster_(&cluster),
          session_(cluster.connect(deadline)),
          gid_(std::move(gid)),
          deadline_(deadline) {
        if (gid_.empty() || gid_.size() > 200
            || gid_.find('\0') != std::string::npos) {
            throw fixture_error("invalid prepared transaction GID");
        }
        attach_process_sql_transcript(*session_, "prepared-transaction");
        static_cast<void>(session_->exec(
            "BEGIN", deadline_,
            bioseq::libpq::transaction_policy::allow_open));
    }

    scoped_prepared_transaction(const scoped_prepared_transaction&) = delete;
    scoped_prepared_transaction& operator=(
        const scoped_prepared_transaction&) = delete;

    ~scoped_prepared_transaction() noexcept {
        try {
            if (state_ == state::open && session_) {
                static_cast<void>(session_->exec(
                    "ROLLBACK", deadline_,
                    bioseq::libpq::transaction_policy::allow_open));
            } else if (state_ == state::prepared
                       && cluster_->state() == cluster_state::running) {
                bioseq::libpq::connection cleanup =
                    cluster_->connect(deadline_);
                attach_process_sql_transcript(
                    cleanup, "prepared-transaction-cleanup");
                static_cast<void>(cleanup.exec(
                    "ROLLBACK PREPARED " + cleanup.escape_literal(gid_),
                    deadline_));
            }
        } catch (...) {
        }
    }

    /// @brief Returns the session in which work is added before PREPARE.
    [[nodiscard]] bioseq::libpq::connection& session() {
        if (!session_ || state_ != state::open) {
            throw fixture_error("prepared transaction session is not open");
        }
        return *session_;
    }

    /// @brief Persists the transaction under its GID.
    void prepare() {
        if (state_ != state::open) {
            throw fixture_error("prepared transaction is not open");
        }
        static_cast<void>(session_->exec(
            "PREPARE TRANSACTION " + session_->escape_literal(gid_),
            deadline_, bioseq::libpq::transaction_policy::allow_open));
        state_ = state::prepared;
    }

    /// @brief Commits the prepared transaction through a fresh session.
    void commit_prepared() {
        require_prepared();
        bioseq::libpq::connection admin = cluster_->connect(deadline_);
        attach_process_sql_transcript(
            admin, "prepared-transaction-commit");
        static_cast<void>(admin.exec(
            "COMMIT PREPARED " + admin.escape_literal(gid_), deadline_));
        state_ = state::finished;
    }

    /// @brief Rolls back the prepared transaction through a fresh session.
    void rollback_prepared() {
        require_prepared();
        bioseq::libpq::connection admin = cluster_->connect(deadline_);
        attach_process_sql_transcript(
            admin, "prepared-transaction-rollback");
        static_cast<void>(admin.exec(
            "ROLLBACK PREPARED " + admin.escape_literal(gid_), deadline_));
        state_ = state::finished;
    }

private:
    enum class state { open, prepared, finished };

    void require_prepared() const {
        if (state_ != state::prepared) {
            throw fixture_error("transaction has not been prepared");
        }
    }

    private_cluster* cluster_{};
    std::optional<bioseq::libpq::connection> session_;
    std::string gid_;
    std::chrono::milliseconds deadline_;
    state state_{state::open};
};

/// @brief Deadline-aware typed session passed to each independent TBB task.
class session_context {
public:
    /// @brief Constructs a task context from one independent typed connection.
    session_context(bioseq::libpq::connection connection,
                    std::chrono::steady_clock::time_point deadline,
                    sql_transcript* transcript = nullptr,
                    std::string session_name = {})
        : connection_(std::move(connection)),
          deadline_(deadline),
          transcript_(transcript),
          session_name_(std::move(session_name)) {}

    /// @brief Executes SQL using the task's remaining absolute deadline.
    [[nodiscard]] bioseq::libpq::result exec(
        std::string_view sql,
        bioseq::libpq::transaction_policy policy =
            bioseq::libpq::transaction_policy::require_idle) {
        const std::chrono::milliseconds budget = remaining();
        return transcript_ == nullptr
            ? connection_.exec(sql, budget, policy)
            : transcript_->execute(
                  connection_, session_name_, sql, budget, policy);
    }

    /// @brief Executes SQL with the smaller of a local budget and task deadline.
    [[nodiscard]] bioseq::libpq::result exec(
        std::string_view sql,
        std::chrono::milliseconds local_budget,
        bioseq::libpq::transaction_policy policy =
            bioseq::libpq::transaction_policy::require_idle) {
        const std::chrono::milliseconds budget =
            std::min(local_budget, remaining());
        return transcript_ == nullptr
            ? connection_.exec(sql, budget, policy)
            : transcript_->execute(
                  connection_, session_name_, sql, budget, policy);
    }

    /// @brief Quotes an identifier through the shared libpq connection.
    [[nodiscard]] std::string escape_identifier(std::string_view value) const {
        return connection_.escape_identifier(value);
    }

    /// @brief Quotes a literal through the shared libpq connection.
    [[nodiscard]] std::string escape_literal(std::string_view value) const {
        return connection_.escape_literal(value);
    }

    /// @brief Returns the task's absolute steady-clock deadline.
    [[nodiscard]] std::chrono::steady_clock::time_point deadline() const noexcept {
        return deadline_;
    }

private:
    [[nodiscard]] std::chrono::milliseconds remaining() const {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline_) {
            throw fixture_error("independent session deadline exceeded");
        }
        return std::max(
            std::chrono::milliseconds(1),
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline_ - now));
    }

    bioseq::libpq::connection connection_;
    std::chrono::steady_clock::time_point deadline_;
    sql_transcript* transcript_{};
    std::string session_name_;
};

/// @brief One named concurrent SQL/session operation.
struct session_task {
    std::string name;
    std::function<void(session_context&)> body;
};

/// @brief Nonthrowing observation of one independent session task.
struct session_task_outcome {
    std::string name;
    bool passed{};
    bool timed_out{};
    std::string error;
    std::chrono::milliseconds duration{};
};

/// @brief Runs independent sessions on TBB under one shared absolute deadline.
///
/// Task bodies must perform all SQL and waits through `session_context` and the
/// bounded `wait_until` helper so cancellation reaches a terminal state.
[[nodiscard]] inline std::vector<session_task_outcome>
run_independent_session_tasks(
    const private_cluster& cluster,
    std::span<const session_task> tasks,
    std::chrono::milliseconds deadline,
    sql_transcript* transcript = nullptr) {
    if (deadline <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("session-task deadline must be positive");
    }
    for (const session_task& task : tasks) {
        if (task.name.empty() || !task.body) {
            throw std::invalid_argument(
                "session tasks require a name and callable body");
        }
    }
    const auto finish = std::chrono::steady_clock::now() + deadline;
    std::vector<session_task_outcome> outcomes(tasks.size());
    std::atomic<std::size_t> remaining{tasks.size()};
    std::mutex completion_mutex;
    std::condition_variable completion;
    oneapi::tbb::task_group_context group_context;
    oneapi::tbb::task_group group(group_context);
    for (std::size_t index = 0; index < tasks.size(); ++index) {
        group.run([&cluster, &tasks, &outcomes, &remaining, &completion,
                   transcript, index, finish]() {
            const auto started = std::chrono::steady_clock::now();
            session_task_outcome outcome{.name = tasks[index].name};
            try {
                const auto now = std::chrono::steady_clock::now();
                if (now >= finish) {
                    throw fixture_error(
                        "session task started after overall deadline");
                }
                const auto connection_budget = std::max(
                    std::chrono::milliseconds(1),
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        finish - now));
                bioseq::libpq::connection connection =
                    cluster.connect(connection_budget);
                attach_process_sql_transcript(
                    connection, tasks[index].name);
                session_context context(
                    std::move(connection), finish,
                    transcript, tasks[index].name);
                tasks[index].body(context);
                outcome.passed = true;
            } catch (const std::exception& error) {
                outcome.error = error.what();
                outcome.timed_out =
                    std::chrono::steady_clock::now() >= finish;
            } catch (...) {
                outcome.error = "non-standard session task exception";
                outcome.timed_out =
                    std::chrono::steady_clock::now() >= finish;
            }
            outcome.duration =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started);
            outcomes[index] = std::move(outcome);
            remaining.fetch_sub(1, std::memory_order_release);
            completion.notify_all();
        });
    }
    {
        std::unique_lock lock(completion_mutex);
        if (!completion.wait_until(
                lock, finish,
                [&remaining]() {
                    return remaining.load(std::memory_order_acquire) == 0;
                })) {
            group_context.cancel_group_execution();
        }
    }
    group.wait();
    for (std::size_t index = 0; index < outcomes.size(); ++index) {
        if (outcomes[index].name.empty()) {
            outcomes[index] = session_task_outcome{
                .name = tasks[index].name,
                .passed = false,
                .timed_out = true,
                .error = "session task cancelled at overall deadline",
                .duration = deadline};
        }
    }
    return outcomes;
}

}  // namespace bioseq::postgres_test
