#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

namespace {

constexpr std::uint64_t reference_bases = 2'000'000'000ULL;
constexpr std::uint64_t gibibyte = 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t mebibyte = 1024ULL * 1024ULL;
constexpr std::uint64_t minimum_total_ram = 16ULL * gibibyte;
constexpr std::uint64_t minimum_available_ram = 8ULL * gibibyte;
constexpr std::uint64_t minimum_data_space =
    32ULL * gibibyte > 12ULL * reference_bases
    ? 32ULL * gibibyte
    : 12ULL * reference_bases;
constexpr std::uint64_t io_probe_bytes = gibibyte;
constexpr double minimum_io_mib_per_second = 150.0;
constexpr std::int64_t coordinate_tolerance = 50'000;

constexpr std::string_view alignment_query{
    "SELECT id, "
    "bioseq.target_start(id), "
    "bioseq.target_end(id), "
    "bioseq.target_len(id), "
    "bioseq.query_start(id), "
    "bioseq.query_end(id), "
    "bioseq.query_len(id), "
    "bioseq.is_reverse(id) "
    "FROM cache_bioseq_2gbp "
    "WHERE mmsearch(target, query) "
    "ORDER BY id, "
    "bioseq.target_start(id), "
    "bioseq.target_end(id), "
    "bioseq.target_len(id), "
    "bioseq.query_start(id), "
    "bioseq.query_end(id), "
    "bioseq.query_len(id), "
    "bioseq.is_reverse(id)"};

struct memory_capacity {
    std::uint64_t total{};
    std::uint64_t available{};
};

struct io_probe_result {
    double write_mib_per_second{};
    double read_mib_per_second{};
};

class probe_file_guard {
public:
    explicit probe_file_guard(std::filesystem::path path)
        : path_(std::move(path)) {}
    probe_file_guard(const probe_file_guard&) = delete;
    probe_file_guard& operator=(const probe_file_guard&) = delete;
    ~probe_file_guard() noexcept {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::uint64_t checked_product(long lhs, long rhs) {
    if (lhs <= 0 || rhs <= 0) {
        return 0;
    }
    const auto left = static_cast<std::uint64_t>(lhs);
    const auto right = static_cast<std::uint64_t>(rhs);
    if (left > std::numeric_limits<std::uint64_t>::max() / right) {
        throw std::overflow_error("physical memory size overflow");
    }
    return left * right;
}

[[nodiscard]] memory_capacity host_memory_capacity() {
#if defined(__APPLE__)
    std::uint64_t total{};
    std::size_t total_size = sizeof(total);
    if (::sysctlbyname("hw.memsize", &total, &total_size, nullptr, 0) != 0) {
        throw std::runtime_error("cannot read hw.memsize");
    }
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_statistics64_data_t statistics{};
    if (::host_statistics64(
            mach_host_self(), HOST_VM_INFO64,
            reinterpret_cast<host_info64_t>(&statistics), &count)
        != KERN_SUCCESS) {
        throw std::runtime_error("cannot read host VM statistics");
    }
    vm_size_t page_size{};
    if (::host_page_size(mach_host_self(), &page_size) != KERN_SUCCESS) {
        throw std::runtime_error("cannot read host page size");
    }
    const std::uint64_t available_pages =
        static_cast<std::uint64_t>(statistics.free_count)
        + static_cast<std::uint64_t>(statistics.inactive_count);
    return memory_capacity{
        .total = total,
        .available = available_pages
            * static_cast<std::uint64_t>(page_size)};
#else
    return memory_capacity{
        .total = checked_product(
            ::sysconf(_SC_PHYS_PAGES), ::sysconf(_SC_PAGESIZE)),
        .available = checked_product(
            ::sysconf(_SC_AVPHYS_PAGES), ::sysconf(_SC_PAGESIZE))};
#endif
}

void write_all(int descriptor, const std::byte* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t written =
            ::write(descriptor, data + offset, size - offset);
        if (written <= 0) {
            throw std::runtime_error("sequential I/O probe write failed");
        }
        offset += static_cast<std::size_t>(written);
    }
}

void read_all(int descriptor, std::byte* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t received =
            ::read(descriptor, data + offset, size - offset);
        if (received <= 0) {
            throw std::runtime_error("sequential I/O probe read failed");
        }
        offset += static_cast<std::size_t>(received);
    }
}

[[nodiscard]] io_probe_result qualify_data_filesystem(
    const std::filesystem::path& directory) {
    const std::filesystem::path probe_path =
        directory / ".bioseq-cache-2gbp-io-probe";
    probe_file_guard cleanup{probe_path};
    const int descriptor = ::open(
        probe_path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (descriptor < 0) {
        throw std::runtime_error("cannot create 2 Gbp I/O probe file");
    }
    try {
        std::vector<std::byte> block(
            8ULL * mebibyte, std::byte{0xa5});
        const std::size_t iterations =
            static_cast<std::size_t>(io_probe_bytes / block.size());
        const auto write_started = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < iterations; ++index) {
            write_all(descriptor, block.data(), block.size());
        }
        if (::fsync(descriptor) != 0) {
            throw std::runtime_error("cannot fsync 2 Gbp I/O probe");
        }
        const auto write_finished = std::chrono::steady_clock::now();
        if (::lseek(descriptor, 0, SEEK_SET) < 0) {
            throw std::runtime_error("cannot rewind 2 Gbp I/O probe");
        }
        const auto read_started = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < iterations; ++index) {
            read_all(descriptor, block.data(), block.size());
        }
        const auto read_finished = std::chrono::steady_clock::now();
        static_cast<void>(::close(descriptor));
        const double write_seconds =
            std::chrono::duration<double>(
                write_finished - write_started).count();
        const double read_seconds =
            std::chrono::duration<double>(
                read_finished - read_started).count();
        return io_probe_result{
            .write_mib_per_second =
                static_cast<double>(io_probe_bytes / mebibyte)
                / write_seconds,
            .read_mib_per_second =
                static_cast<double>(io_probe_bytes / mebibyte)
                / read_seconds};
    } catch (...) {
        static_cast<void>(::close(descriptor));
        throw;
    }
}

[[nodiscard]] std::int64_t parse_i64(std::string_view text) {
    std::int64_t value{};
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::runtime_error("alignment coordinate is not int64");
    }
    return value;
}

[[nodiscard]] std::int64_t absolute_difference(std::int64_t lhs,
                                               std::int64_t rhs) {
    return lhs >= rhs ? lhs - rhs : rhs - lhs;
}

void configure_canonical_cpu_path(
    bioseq::libpq::connection& connection) {
    bioseq::cache_test::exec(
        connection,
        "SET bioseq.aligner.vulkan_enabled = off; "
        "SET bioseq.aligner.vulkan_seed_hits_enabled = off; "
        "SET bioseq.aligner.vulkan_index_cache_enabled = off; "
        "SET bioseq.aligner.vulkan_fused_frontend_enabled = off; "
        "SET bioseq.aligner.vulkan_gpu_index_build_enabled = off; "
        "SET bioseq.aligner.vulkan_extend_enabled = off; "
        "SET bioseq.enable_custom_scan = on; "
        "SET bioseq.enable_column_cache = on; "
        "SET bioseq.read_sequence_index = on");
}

[[nodiscard]] bool has_partial_file(
    const std::filesystem::path& root) {
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator iterator{
             root, std::filesystem::directory_options::skip_permission_denied,
             error},
         end;
         !error && iterator != end;
         iterator.increment(error)) {
        if (iterator->is_regular_file(error)
            && iterator->path().extension() == ".partial") {
            return true;
        }
        error.clear();
    }
    return false;
}

}  // namespace

TEST_CASE("exactly 2 Gbp Bioseq result produces once and replays exactly",
          "[cache][integration][bioseq][large][2gbp]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "exactly 2 Gbp Bioseq result produces once and replays exactly"};
    const auto test_started = std::chrono::steady_clock::now();
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            true, bioseq::cache_test::preload_order::cache_then_bioseq)};
    auto& admin = fixture.admin();
    auto data = fixture.connect(30s);

    const memory_capacity memory = host_memory_capacity();
    const std::filesystem::space_info data_space =
        std::filesystem::space(fixture.cluster().data_directory());
    const unsigned int logical_cores = std::thread::hardware_concurrency();
    INFO("total RAM bytes: " << memory.total);
    INFO("available RAM bytes: " << memory.available);
    INFO("PGDATA available bytes: " << data_space.available);
    INFO("logical cores: " << logical_cores);
    REQUIRE(memory.total >= minimum_total_ram);
    REQUIRE(memory.available >= minimum_available_ram);
    REQUIRE(data_space.available >= minimum_data_space);
    REQUIRE(logical_cores >= 8);
    const io_probe_result io =
        qualify_data_filesystem(fixture.cluster().data_directory());
    INFO("sequential write MiB/s: " << io.write_mib_per_second);
    INFO("sequential read MiB/s: " << io.read_mib_per_second);
    REQUIRE(io.write_mib_per_second >= minimum_io_mib_per_second);
    REQUIRE(io.read_mib_per_second >= minimum_io_mib_per_second);
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.host_qualified");

    configure_canonical_cpu_path(admin);
    configure_canonical_cpu_path(data);
    bioseq::cache_test::exec(
        admin,
        "SET bioseq.update_index_on_insert = off; "
        "SELECT bioseq.create_query_target_sample_table("
        "'cache_bioseq_2gbp', 1, 5000000, 100000.0, 2000000000, "
        "0.001, 0.0005, 0.0005, 1.0, 1.0, 0.5, 3219876)",
        20min);
    bioseq::cache_test::exec(
        admin,
        "SELECT bioseq.update_index("
        "'cache_bioseq_2gbp'::regclass, "
        "'minimizers_per_fragment'); "
        "SELECT bioseq.set_active_read_index("
        "'cache_bioseq_2gbp'::regclass, "
        "'minimizers_per_fragment')",
        20min);
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.exact_fixture_created");
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.persisted_index_active");

    bioseq::cache_test::exec(
        admin,
        "SET bioseq.query_cache_optimisation_mode = 'disabled'");
    bioseq::cache_test::exec(
        data,
        "SET bioseq.query_cache_optimisation_mode = 'disabled'");
    const bioseq::libpq::result oracle =
        data.exec(alignment_query, 20min);
    REQUIRE(oracle.rows() > 0);
    const auto oracle_digest =
        bioseq::postgres_test::exact_row_hash(oracle);
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.disabled_oracle_nonempty");

    bioseq::cache_test::exec(
        admin,
        "SELECT bioseq_cache.invalidate_all(); "
        "SELECT bioseq_cache.reset_stats()");
    bioseq::cache_test::exec(
        data,
        "SET bioseq.query_cache_optimisation_mode = 'same'");
    const auto before_cold =
        bioseq::cache_test::read_cache_stats(admin);
    const bioseq::libpq::result cold =
        data.exec(alignment_query, 20min);
    const auto after_cold =
        bioseq::cache_test::read_cache_stats(admin);
    REQUIRE(cold.rows() > 0);
    CHECK(bioseq::postgres_test::exact_row_hash(cold) == oracle_digest);
    const auto cold_delta = after_cold - before_cold;
    CHECK(cold_delta.produced == 1);
    CHECK(cold_delta.completed_hits == 0);
    CHECK(cold_delta.fallbacks == 0);
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.cold_identity");
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.cold_counter_contract");

    const bioseq::libpq::result truth = admin.exec(
        "SELECT id, truth_target_start, truth_target_end "
        "FROM cache_bioseq_2gbp ORDER BY id",
        5min);
    REQUIRE(truth.rows() > 0);
    for (int row = 0; row < cold.rows(); ++row) {
        const std::int64_t id = parse_i64(cold.value(row, 0));
        bool found = false;
        for (int truth_row = 0; truth_row < truth.rows(); ++truth_row) {
            if (parse_i64(truth.value(truth_row, 0)) != id) {
                continue;
            }
            const std::int64_t target_start =
                parse_i64(cold.value(row, 1));
            const std::int64_t target_end =
                parse_i64(cold.value(row, 2));
            const std::int64_t target_len =
                parse_i64(cold.value(row, 3));
            const std::int64_t truth_start =
                parse_i64(truth.value(truth_row, 1));
            const std::int64_t truth_end =
                parse_i64(truth.value(truth_row, 2));
            CHECK(target_start >= 0);
            CHECK(target_start < target_end);
            CHECK(target_end <=
                  static_cast<std::int64_t>(reference_bases));
            CHECK(target_len ==
                  static_cast<std::int64_t>(reference_bases));
            CHECK(absolute_difference(target_start, truth_start)
                  <= coordinate_tolerance);
            CHECK(absolute_difference(target_end, truth_end)
                  <= coordinate_tolerance);
            found = true;
            break;
        }
        CHECK(found);
    }
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.coordinates_match_truth");
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.target_len_exact");

    const bioseq::libpq::result warm =
        data.exec(alignment_query, 5min);
    const auto after_warm =
        bioseq::cache_test::read_cache_stats(admin);
    CHECK(bioseq::postgres_test::exact_row_hash(warm) == oracle_digest);
    const auto warm_delta = after_warm - after_cold;
    CHECK(warm_delta.produced == 0);
    CHECK(warm_delta.completed_hits == 1);
    CHECK(warm_delta.fallbacks == 0);
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.warm_identity");
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.warm_counter_contract");

    CHECK(bioseq::cache_test::integer(
              admin,
              "SELECT count(*) FROM bioseq_cache.entries() "
              "WHERE kind = 'ExactFinalResult' "
              "AND state = 'COMPLETE' "
              "AND rows > 0 "
              "AND logical_bytes > 0 "
              "AND stored_bytes > 0")
          == 1);
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.complete_entry");

    const auto explain = bioseq::postgres_test::run_explain(
        data, alignment_query, true, 5min);
    const auto cache_nodes =
        explain.find_plan_nodes("Custom Plan Provider", "BioseqCacheScan");
    REQUIRE(cache_nodes.size() == 1);
    CHECK(bioseq::postgres_test::explain_property_string(
              *cache_nodes.front(), "Actual Cache Action")
          == "[Hit] exact final result");
    const bioseq::postgres_test::json_value* children =
        cache_nodes.front()->find("Plans");
    REQUIRE(children != nullptr);
    REQUIRE(children->as_array() != nullptr);
    REQUIRE_FALSE(children->as_array()->empty());
    CHECK(bioseq::postgres_test::explain_property_number(
              children->as_array()->front(), "Actual Loops")
          == 0.0);
    const auto after_explain =
        bioseq::cache_test::read_cache_stats(admin);
    CHECK((after_explain - after_warm).completed_hits == 1);
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.explain_hit");
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.child_not_executed");

    bioseq::cache_test::exec(
        data, "SET bioseq.enable_column_cache = off");
    const auto column_cache_off =
        data.exec(alignment_query, 5min);
    const auto after_column_cache_off =
        bioseq::cache_test::read_cache_stats(admin);
    CHECK(bioseq::postgres_test::exact_row_hash(column_cache_off)
          == oracle_digest);
    CHECK((after_column_cache_off - after_explain).completed_hits == 1);
    CHECK((after_column_cache_off - after_explain).fallbacks == 0);
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.column_cache_off_hit");

    CHECK(bioseq::cache_test::integer(
              admin,
              "SELECT count(*) FROM bioseq_cache.entries() "
              "WHERE state = 'BUILDING'")
          == 0);
    const auto references = admin.exec(
        "SELECT reader_pins, waiter_references, admission_reservations "
        "FROM bioseq_cache_test.planner_reference_counts()");
    REQUIRE(references.rows() == 1);
    CHECK(references.value(0, 0) == "0");
    CHECK(references.value(0, 1) == "0");
    CHECK(references.value(0, 2) == "0");
    CHECK_FALSE(has_partial_file(fixture.cluster().data_directory()));
    CHECK(std::chrono::steady_clock::now() - test_started < 105min);
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.cleanup_invariants");
}
