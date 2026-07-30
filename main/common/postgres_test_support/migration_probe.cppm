module;

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.test.postgres:migration_probe;

export namespace bioseq::postgres_test {

/// @brief Catch case/section/permutation attached to subsequently passed probes.
struct migration_probe_context {
    std::string test_case;
    std::string section;
    std::string permutation;
};

/// @brief One runtime proof that the associated Catch assertions have passed.
struct migration_probe_record {
    std::string stable_id;
    std::string test_case;
    std::string section;
    std::string permutation;
    bool passed{true};
    std::uint64_t ordinal{};
};

/// @brief Expected runtime count loaded from one non-retired ledger row.
struct expected_migration_probe {
    std::string stable_id;
    std::size_t expected_count{1};
};

/// @brief Invalid/duplicate/missing migration evidence.
class migration_probe_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

}  // namespace bioseq::postgres_test

namespace bioseq::postgres_test::migration_probe_detail {

struct registry {
    std::mutex mutex;
    std::vector<migration_probe_record> records;
    std::uint64_t next_ordinal{};
};

/// @brief Returns the one process-wide, thread-safe probe registry.
[[nodiscard]] inline registry& process_registry() {
    static registry value;
    return value;
}

inline thread_local migration_probe_context current_context;

/// @brief Requires a durable, report-safe stable probe identity.
inline void validate_stable_id(std::string_view stable_id) {
    if (stable_id.empty()) {
        throw migration_probe_error("migration probe id is empty");
    }
    for (const char character : stable_id) {
        const bool allowed =
            std::isalnum(static_cast<unsigned char>(character)) != 0
            || character == '_' || character == '-' || character == '.'
            || character == ':';
        if (!allowed) {
            throw migration_probe_error("invalid migration probe id: "
                                        + std::string(stable_id));
        }
    }
}

}  // namespace bioseq::postgres_test::migration_probe_detail

export namespace bioseq::postgres_test {

/// @brief Replaces the current thread context (used by Catch listener callbacks).
inline void set_migration_probe_context(migration_probe_context context) {
    migration_probe_detail::current_context = std::move(context);
}

/// @brief Clears the current thread context at the end of a Catch case.
inline void clear_migration_probe_context() noexcept {
    migration_probe_detail::current_context = {};
}

/// @brief Updates Catch-owned fields while preserving a scoped permutation.
inline void update_migration_probe_case_section(
    std::string test_case,
    std::string section) {
    migration_probe_detail::current_context.test_case =
        std::move(test_case);
    migration_probe_detail::current_context.section =
        std::move(section);
}

/// @brief Sets the calling thread's Catch context and restores it on destruction.
class migration_probe_scope {
public:
    explicit migration_probe_scope(migration_probe_context context)
        : previous_(std::exchange(
              migration_probe_detail::current_context, std::move(context))) {}

    migration_probe_scope(std::string test_case,
                          std::string section = {},
                          std::string permutation = {})
        : migration_probe_scope(migration_probe_context{
              .test_case = std::move(test_case),
              .section = std::move(section),
              .permutation = std::move(permutation)}) {}

    migration_probe_scope(const migration_probe_scope&) = delete;
    migration_probe_scope& operator=(const migration_probe_scope&) = delete;
    migration_probe_scope(migration_probe_scope&&) = delete;
    migration_probe_scope& operator=(migration_probe_scope&&) = delete;

    ~migration_probe_scope() noexcept {
        migration_probe_detail::current_context = std::move(previous_);
    }

private:
    migration_probe_context previous_;
};

/// @brief Records one stable id after its associated Catch assertion(s) pass.
///
/// The custom runner supplies a nonempty test-case context. Re-emitting the
/// same id in the same case/section/permutation is always an error; legitimate
/// parameterized repetitions use distinct permutation strings and a ledger
/// expected_count greater than one.
inline void record_migration_probe(std::string_view stable_id) {
    migration_probe_detail::validate_stable_id(stable_id);
    const migration_probe_context& context =
        migration_probe_detail::current_context;
    if (context.test_case.empty()) {
        throw migration_probe_error(
            "record_migration_probe requires an active test-case context");
    }
    auto& registry = migration_probe_detail::process_registry();
    std::scoped_lock lock(registry.mutex);
    const auto duplicate = std::find_if(
        registry.records.begin(), registry.records.end(),
        [&stable_id, &context](const migration_probe_record& record) {
            return record.stable_id == stable_id
                && record.test_case == context.test_case
                && record.section == context.section
                && record.permutation == context.permutation;
        });
    if (duplicate != registry.records.end()) {
        throw migration_probe_error("duplicate migration probe emission: "
                                    + std::string(stable_id));
    }
    registry.records.push_back(migration_probe_record{
        .stable_id = std::string(stable_id),
        .test_case = context.test_case,
        .section = context.section,
        .permutation = context.permutation,
        .passed = true,
        .ordinal = registry.next_ordinal++});
}

/// @brief Returns a stable ordinal-ordered snapshot of all emitted probes.
[[nodiscard]] inline std::vector<migration_probe_record>
migration_probe_records() {
    auto& registry = migration_probe_detail::process_registry();
    std::scoped_lock lock(registry.mutex);
    return registry.records;
}

/// @brief Invalidates emitted proof when its owning Catch case did not pass.
inline void reconcile_migration_probe_case(
    std::string_view test_case,
    bool passed) noexcept {
    if (passed) {
        return;
    }
    try {
        auto& registry = migration_probe_detail::process_registry();
        std::scoped_lock lock(registry.mutex);
        for (migration_probe_record& record : registry.records) {
            if (record.test_case == test_case) {
                record.passed = false;
            }
        }
    } catch (...) {
    }
}

/// @brief Clears runtime evidence before a new Catch session starts.
inline void clear_migration_probes() {
    auto& registry = migration_probe_detail::process_registry();
    std::scoped_lock lock(registry.mutex);
    registry.records.clear();
    registry.next_ordinal = 0;
}

/// @brief Requires exact per-id counts and rejects unlisted emitted probes.
inline void validate_migration_probes(
    std::span<const expected_migration_probe> expected) {
    std::map<std::string, std::size_t, std::less<>> expected_counts;
    for (const expected_migration_probe& item : expected) {
        migration_probe_detail::validate_stable_id(item.stable_id);
        if (item.expected_count == 0
            || !expected_counts.emplace(
                    item.stable_id, item.expected_count).second) {
            throw migration_probe_error(
                "duplicate or zero-count expected migration probe: "
                + item.stable_id);
        }
    }
    std::map<std::string, std::size_t, std::less<>> actual_counts;
    for (const migration_probe_record& record : migration_probe_records()) {
        if (!record.passed) {
            throw migration_probe_error("non-passing migration probe: "
                                        + record.stable_id);
        }
        ++actual_counts[record.stable_id];
    }
    for (const auto& [stable_id, count] : actual_counts) {
        const auto found = expected_counts.find(stable_id);
        if (found == expected_counts.end()) {
            throw migration_probe_error("unlisted migration probe emitted: "
                                        + stable_id);
        }
        if (found->second != count) {
            throw migration_probe_error(
                "migration probe count mismatch for " + stable_id
                + ": expected " + std::to_string(found->second)
                + ", observed " + std::to_string(count));
        }
    }
    for (const auto& [stable_id, count] : expected_counts) {
        const auto found = actual_counts.find(stable_id);
        const std::size_t observed =
            found == actual_counts.end() ? 0 : found->second;
        if (observed != count) {
            throw migration_probe_error(
                "missing migration probe " + stable_id + ": expected "
                + std::to_string(count) + ", observed "
                + std::to_string(observed));
        }
    }
}

}  // namespace bioseq::postgres_test
