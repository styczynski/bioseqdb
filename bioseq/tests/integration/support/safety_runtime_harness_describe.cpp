/** @file Safety runtime catalog-description command family. */

module;

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <fmt/ranges.h>
#include <libpq-fe.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <spawn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

module bioseq.tests.integration.safety_runtime_harness_support;

namespace bioseq::tests::integration::safety_runtime_harness {

/// Emit the public catalog of cases for this compiled profile.
[[nodiscard]] int describe(
        const command_line& command) {
    require_exact_options(
        command, {"--profile", "--output"});
    const std::string profile =
        require_option(command, "--profile");
    if (profile != k_profile) {
        fail("describe", "requested profile differs from executable");
    }
    const fs::path output =
        fs::absolute(require_option(command, "--output"))
            .lexically_normal();
    const case_plan plan = load_case_plan();
    json cases = json::array();
    for (const auto& [case_id_value, definition] : plan.cases) {
        static_cast<void>(case_id_value);
        cases.push_back(definition.catalog);
    }
    const json catalog{
        {"schema_revision", 2},
        {"profile", profile},
        {"case_plan_sha256", k_case_plan_sha256},
        {"test_executable_sha256", self_sha256(command)},
        {"cases", std::move(cases)}};
    write_atomic(output, canonical_json(catalog));
    return 0;
}

}  // namespace bioseq::tests::integration::safety_runtime_harness
