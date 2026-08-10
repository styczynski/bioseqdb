#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include <unistd.h>

import bioseq.test.postgres;
import bioseq.common.memory_budget;

using namespace std::chrono_literals;
using namespace bioseq::postgres_test;

static_assert(std::is_same_v<
    decltype(host_qualification_report::host_total_ram_bytes),
    std::uint64_t>);
static_assert(std::is_same_v<
    decltype(host_qualification_report::effective_cpu_cores),
    double>);

namespace {

class qualification_snapshot_tree {
public:
    qualification_snapshot_tree()
        : root_(
              std::filesystem::temp_directory_path()
              / ("bioseq-qualification-contract-"
                 + std::to_string(::getpid()) + "-"
                 + std::to_string(
                     std::chrono::steady_clock::now()
                         .time_since_epoch()
                         .count()))) {
        configure_supervision_state(root_);
    }

    qualification_snapshot_tree(
        const qualification_snapshot_tree&) = delete;
    qualification_snapshot_tree& operator=(
        const qualification_snapshot_tree&) = delete;

    ~qualification_snapshot_tree() noexcept {
        disable_supervision_state();
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }

private:
    std::filesystem::path root_;
};

}  // namespace

TEST_CASE("cgroup memory parser distinguishes unlimited and finite limits",
          "[postgres-test-support][unit][qualification][cgroup]") {
    CHECK_FALSE(parse_cgroup_memory_limit("max\n").has_value());
    REQUIRE(parse_cgroup_memory_limit("8589934592").has_value());
    CHECK(*parse_cgroup_memory_limit("8589934592")
          == 8ULL * qualification_gibibyte);
    CHECK(*parse_cgroup_memory_limit("0") == 0);
    CHECK_THROWS_AS(
        parse_cgroup_memory_limit("8GiB"), std::invalid_argument);
    CHECK_THROWS_AS(
        parse_cgroup_memory_limit(""), std::invalid_argument);
}

TEST_CASE("Darwin page queues do not double-count speculative pages",
          "[postgres-test-support][unit][qualification][memory]") {
    using bioseq::common::memory::
        darwin_conservative_available_page_bytes;

    constexpr std::uint64_t page_bytes = 16'384;
    // The illustrative speculative subset is already inside free_pages=100.
    const auto available = darwin_conservative_available_page_bytes(
        100, 50, page_bytes);
    REQUIRE(available.has_value());
    CHECK(*available == 150 * page_bytes);

    CHECK_FALSE(darwin_conservative_available_page_bytes(
        1, 1, 0).has_value());
    CHECK_FALSE(darwin_conservative_available_page_bytes(
        (std::numeric_limits<std::uint64_t>::max)(),
        1, page_bytes).has_value());
    CHECK_FALSE(darwin_conservative_available_page_bytes(
        (std::numeric_limits<std::uint64_t>::max)() / 2 + 1,
        0, 2).has_value());
}

TEST_CASE("nested cgroup memory limits constrain total and live headroom",
          "[postgres-test-support][unit][qualification][cgroup]") {
    constexpr std::array limits{
        cgroup_memory_limit_sample{
            .limit_bytes = 16ULL * qualification_gibibyte,
            .current_bytes = 15ULL * qualification_gibibyte},
        cgroup_memory_limit_sample{
            .limit_bytes = 8ULL * qualification_gibibyte,
            .current_bytes = 1ULL * qualification_gibibyte}};
    const effective_memory_capacity capacity =
        constrain_memory_capacity(
            64ULL * qualification_gibibyte,
            40ULL * qualification_gibibyte,
            limits);

    CHECK(capacity.total_bytes
          == 8ULL * qualification_gibibyte);
    CHECK(capacity.available_bytes
          == 1ULL * qualification_gibibyte);
    REQUIRE(capacity.total_limit_sample.has_value());
    REQUIRE(capacity.available_limit_sample.has_value());
    CHECK(*capacity.total_limit_sample == 1);
    CHECK(*capacity.available_limit_sample == 0);
}

TEST_CASE("cgroup CPU parser preserves quota and normalized cpuset capacity",
          "[postgres-test-support][unit][qualification][cgroup]") {
    CHECK_FALSE(parse_cgroup_cpu_max("max 100000\n").has_value());
    const auto quota =
        parse_cgroup_cpu_max("750000 100000");
    REQUIRE(quota.has_value());
    CHECK(quota->quota_microseconds == 750000);
    CHECK(quota->period_microseconds == 100000);

    REQUIRE(parse_cgroup_cpuset_count(
                "0-3,2-5,8,10-11\n")
                .has_value());
    CHECK(*parse_cgroup_cpuset_count(
              "0-3,2-5,8,10-11\n")
          == 9);
    CHECK_FALSE(parse_cgroup_cpuset_count(" \n").has_value());
    CHECK_THROWS_AS(
        parse_cgroup_cpu_max("0 100000"),
        std::invalid_argument);
    CHECK_THROWS_AS(
        parse_cgroup_cpu_max("100000 0"),
        std::invalid_argument);
    CHECK_THROWS_AS(
        parse_cgroup_cpuset_count("4-2"),
        std::invalid_argument);
    CHECK_THROWS_AS(
        parse_cgroup_cpuset_count("0-4294967295"),
        std::overflow_error);
}

TEST_CASE("effective CPU capacity intersects host affinity cpuset and quota",
          "[postgres-test-support][unit][qualification][cgroup]") {
    constexpr std::array quotas{
        cgroup_cpu_quota_sample{
            .quota_microseconds = 1'200'000,
            .period_microseconds = 100'000},
        cgroup_cpu_quota_sample{
            .quota_microseconds = 750'000,
            .period_microseconds = 100'000}};
    const effective_cpu_capacity capacity =
        constrain_cpu_capacity(16, 12, 10, quotas);

    CHECK(capacity.cores == 7.5);
    REQUIRE(capacity.quota_sample.has_value());
    CHECK(*capacity.quota_sample == 1);

    const effective_cpu_capacity affinity_only =
        constrain_cpu_capacity(
            0, 6, std::nullopt,
            std::span<const cgroup_cpu_quota_sample>{});
    CHECK(affinity_only.cores == 6.0);
    CHECK_FALSE(affinity_only.quota_sample.has_value());
}

TEST_CASE("hybrid controller selection falls through only when v2 is absent",
          "[postgres-test-support][unit][qualification][cgroup]") {
    CHECK(select_cgroup_controller_version(false, true)
          == cgroup_controller_version::v1);
    CHECK(select_cgroup_controller_version(true, true)
          == cgroup_controller_version::v2);
    CHECK(select_cgroup_controller_version(true, false)
          == cgroup_controller_version::v2);
    CHECK(select_cgroup_controller_version(false, false)
          == cgroup_controller_version::none);
}

TEST_CASE("cgroup namespace coordinates reject unresolved parents",
          "[postgres-test-support][unit][qualification][cgroup]") {
    CHECK(cgroup_namespace_path_is_safe("/"));
    CHECK(cgroup_namespace_path_is_safe("/user.slice/example"));
    CHECK_FALSE(cgroup_namespace_path_is_safe("/.."));
    CHECK_FALSE(cgroup_namespace_path_is_safe("/../.."));
    CHECK_FALSE(cgroup_namespace_path_is_safe("/safe/../escape"));
    CHECK_FALSE(cgroup_namespace_path_is_safe("relative/path"));

    CHECK(cgroup_membership_is_resolvable("/", "/"));
    CHECK_FALSE(cgroup_membership_is_resolvable("/", "/child"));
    CHECK(cgroup_membership_is_resolvable(
        "/child/task", "/child"));
    CHECK_FALSE(cgroup_membership_is_resolvable(
        "/sibling/task", "/child"));
}

TEST_CASE("scheduler affinity errors only resize for an undersized mask",
          "[postgres-test-support][unit][qualification][cgroup]") {
#if defined(__linux__)
    CHECK(scheduler_affinity_error_requires_resize(EINVAL));
    CHECK_FALSE(scheduler_affinity_error_requires_resize(EPERM));
#else
    CHECK_FALSE(scheduler_affinity_error_requires_resize(EINVAL));
#endif
}

TEST_CASE("Linux memory-node scope is normalized and fail-closed",
          "[postgres-test-support][unit][qualification][numa]") {
    CHECK_FALSE(
        classify_linux_memory_node_scope("0", "0").restricted);
    CHECK_FALSE(
        classify_linux_memory_node_scope(
            "0-2,1-3", "0-3").restricted);

    const linux_memory_node_scope restricted =
        classify_linux_memory_node_scope("0,2", "0-3");
    CHECK(restricted.allowed_nodes == 2);
    CHECK(restricted.online_nodes == 4);
    CHECK(restricted.restricted);

    CHECK_THROWS_AS(
        classify_linux_memory_node_scope("0-1", "2-3"),
        std::invalid_argument);
    CHECK_THROWS_AS(
        classify_linux_memory_node_scope("0-4", "0-3"),
        std::invalid_argument);
    CHECK_THROWS_AS(
        classify_linux_memory_node_scope("", "0"),
        std::invalid_argument);
    CHECK_THROWS_AS(
        classify_linux_memory_node_scope("4-2", "0-4"),
        std::invalid_argument);
    CHECK_THROWS_AS(
        classify_linux_memory_node_scope(
            "0-4294967295", "0-4294967295"),
        std::overflow_error);

    CHECK(parse_linux_mems_allowed_list(
              "Name:\ttest\nMems_allowed_list:\t0-3\n")
          == "0-3");
    CHECK_THROWS_AS(
        parse_linux_mems_allowed_list("Name:\ttest\n"),
        std::invalid_argument);
    CHECK_THROWS_AS(
        parse_linux_mems_allowed_list(
            "Mems_allowed_list:\t0\n"
            "Mems_allowed_list:\t0\n"),
        std::invalid_argument);
}

TEST_CASE("finite real parser is bounded and strict",
          "[postgres-test-support][unit][qualification][parse]") {
    const auto fractional = parse_finite_real("7.999999");
    REQUIRE(fractional.has_value());
    CHECK(*fractional == 7.999999);

    const auto exponent = parse_finite_real("-1.25e+2");
    REQUIRE(exponent.has_value());
    CHECK(*exponent == -125.0);

    const auto maximum =
        parse_finite_real("1.7976931348623157e+308");
    REQUIRE(maximum.has_value());
    CHECK(*maximum == std::numeric_limits<double>::max());

    CHECK_FALSE(parse_finite_real("").has_value());
    CHECK_FALSE(parse_finite_real(" 7.5").has_value());
    CHECK_FALSE(parse_finite_real("7.5 ").has_value());
    CHECK_FALSE(parse_finite_real("7.5tail").has_value());
    CHECK_FALSE(parse_finite_real("nan").has_value());
    CHECK_FALSE(parse_finite_real("inf").has_value());
    CHECK_FALSE(parse_finite_real("1e9999").has_value());
    CHECK_FALSE(
        parse_finite_real(std::string(129, '0')).has_value());
}

TEST_CASE("integration JSON preserves a failing fractional CPU capacity",
          "[postgres-test-support][unit][qualification][report]") {
    qualification_snapshot_tree tree;
    integration_test_report report;
    host_qualification_report qualification{
        .test_case = "fractional quota",
        .status =
            host_qualification_status::infrastructure_inconclusive,
        .detail = "effective CPU capacity is below the required minimum",
        .cgroup_cpu_quota_cores = 7.999999,
        .effective_cpu_cores = 7.999999,
        .required_logical_cores = 8};
    report.host_qualifications.push_back(qualification);
    const std::filesystem::path path = tree.root() / "report.json";
    write_integration_test_report(path, report);

    std::ifstream input(path);
    REQUIRE(input);
    const std::string json{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    constexpr std::string_view marker{
        "\"effective_cpu_cores\":"};
    const std::size_t marker_position = json.find(marker);
    REQUIRE(marker_position != std::string::npos);
    const std::size_t value_begin =
        marker_position + marker.size();
    const std::size_t value_end =
        json.find_first_of(",}", value_begin);
    REQUIRE(value_end != std::string::npos);
    const auto serialized = parse_finite_real(std::string_view{
        json.data() + value_begin, value_end - value_begin});
    REQUIRE(serialized.has_value());
    CHECK(*serialized == 7.999999);
    CHECK(*serialized < 8.0);
}

TEST_CASE("qualification watchdog snapshot round-trips complete evidence",
          "[postgres-test-support][unit][qualification][journal]") {
    qualification_snapshot_tree tree;
    host_qualification_report expected{
        .ordinal = 7,
        .test_case = "qualification snapshot",
        .status = host_qualification_status::passed,
        .detail = "host qualification passed",
        .total_ram_bytes = 8ULL * qualification_gibibyte,
        .available_ram_bytes = 3ULL * qualification_gibibyte,
        .logical_cores = 7,
        .host_total_ram_bytes = 64ULL * qualification_gibibyte,
        .host_available_ram_bytes = 32ULL * qualification_gibibyte,
        .effective_total_ram_bytes = 8ULL * qualification_gibibyte,
        .effective_available_ram_bytes =
            3ULL * qualification_gibibyte,
        .cgroup_memory_total_limit_bytes =
            8ULL * qualification_gibibyte,
        .cgroup_memory_headroom_limit_bytes =
            8ULL * qualification_gibibyte,
        .cgroup_memory_current_bytes =
            5ULL * qualification_gibibyte,
        .cgroup_memory_headroom_bytes =
            3ULL * qualification_gibibyte,
        .cgroup_memory_version = "v2",
        .cgroup_memory_total_source =
            "/sys/fs/cgroup/example/memory.max",
        .cgroup_memory_headroom_source =
            "/sys/fs/cgroup/example/memory.max",
        .host_logical_cores = 32,
        .affinity_logical_cores = 16,
        .cpuset_logical_cores = 12,
        .cgroup_cpu_quota_microseconds = 750'000,
        .cgroup_cpu_period_microseconds = 100'000,
        .cgroup_cpu_quota_cores = 7.5,
        .effective_cpu_cores = 7.5,
        .cgroup_cpu_version = "v2",
        .cgroup_cpu_source = "/sys/fs/cgroup/example/cpu.max",
        .required_total_ram_bytes = 8ULL * qualification_gibibyte,
        .required_available_ram_bytes =
            2ULL * qualification_gibibyte,
        .required_logical_cores = 4,
        .required_io_mib_per_second = 150.25,
        .filesystems = {
            filesystem_qualification_report{
                .role = "data",
                .path = "/tmp/qualification data",
                .capacity_bytes = 1000,
                .available_bytes = 900,
                .required_available_bytes = 800,
                .device_id = 42,
                .distinct_from_data = false,
                .passed = true}},
        .io = io_qualification_report{
            .path = "/tmp/qualification data/probe",
            .bytes = 4096,
            .write_duration = 11ms,
            .read_duration = 13ms,
            .write_mib_per_second = 151.5,
            .read_mib_per_second = 152.75,
            .cache_eviction_method = "posix_fadvise",
            .cache_eviction_succeeded = true,
            .passed = true},
        .duration = 29ms};
    const std::array reports{expected};

    REQUIRE(publish_supervision_host_qualification_reports(reports));
    const auto recovered =
        load_supervision_host_qualification_reports(tree.root());
    REQUIRE(recovered.has_value());
    REQUIRE(recovered->size() == 1);
    CHECK(recovered->front() == expected);
}
