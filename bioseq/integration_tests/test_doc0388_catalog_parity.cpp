#include <tests/support/postgres.hpp>

#include <cerrno>
#include <cstring>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <vector>

extern char **environ;

namespace {

/**
 * Run one exact evidence command, inheriting the test runner's diagnostic
 * streams while avoiding shell parsing and ambient command substitution.
 */
[[nodiscard]] int run_evidence_command(
    const std::vector<std::string> &arguments) {
  std::vector<std::string> mutable_arguments = arguments;
  std::vector<char *> argv;
  argv.reserve(mutable_arguments.size() + 1);
  for (std::string &argument : mutable_arguments) {
    argv.push_back(argument.data());
  }
  argv.push_back(nullptr);

  pid_t child{};
  const int spawn_status = posix_spawn(
      &child, argv.front(), nullptr, nullptr, argv.data(), environ);
  if (spawn_status != 0) {
    INFO("posix_spawn failed: " << std::strerror(spawn_status));
    return 128 + spawn_status;
  }
  int status{};
  while (waitpid(child, &status, 0) < 0) {
    if (errno == EINTR) {
      continue;
    }
    INFO("waitpid failed: " << std::strerror(errno));
    return 127;
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return 126;
}

/**
 * Build the common immutable argument prefix for the catalog-evidence
 * orchestrator.
 */
[[nodiscard]] std::vector<std::string> evidence_command_prefix() {
  return {
      BIOSEQ_DOC0388_CATALOG_PARITY_PYTHON,
      BIOSEQ_DOC0388_CATALOG_PARITY_SCRIPT,
  };
}

}  // namespace

TEST_CASE(
    "DOC0388 catalog parity bundle validator rejects mutations",
    "[doc0388][catalog][parity]") {
  auto command = evidence_command_prefix();
  command.emplace_back("self-test");
  REQUIRE(run_evidence_command(command) == 0);
}

TEST_CASE(
    "DOC0388 legacy and generated catalogs match on clean PG17 nodes",
    "[.][doc0388-hardening][catalog][parity][private-pg][relocatable]") {
  auto command = evidence_command_prefix();
  const std::vector<std::string> arguments{
      "run",
      "--repository-root",
      BIOSEQ_DOC0388_CATALOG_PARITY_REPOSITORY_ROOT,
      "--output-root",
      BIOSEQ_DOC0388_CATALOG_PARITY_OUTPUT_ROOT,
      "--cmake",
      BIOSEQ_DOC0388_CATALOG_PARITY_CMAKE,
      "--git",
      BIOSEQ_DOC0388_CATALOG_PARITY_GIT,
      "--pg-config",
      BIOSEQ_DOC0388_CATALOG_PARITY_PG_CONFIG,
      "--stage-script",
      BIOSEQ_DOC0388_CATALOG_PARITY_STAGE_SCRIPT,
      "--config-template",
      BIOSEQ_DOC0388_CATALOG_PARITY_CONFIG_TEMPLATE,
      "--control-file",
      BIOSEQ_DOC0388_CATALOG_PARITY_CONTROL_FILE,
      "--candidate-sql",
      BIOSEQ_DOC0388_CATALOG_PARITY_CANDIDATE_SQL,
      "--candidate-manifest",
      BIOSEQ_DOC0388_CATALOG_PARITY_CANDIDATE_MANIFEST,
      "--candidate-dso",
      BIOSEQ_DOC0388_CATALOG_PARITY_CANDIDATE_DSO,
      "--snapshot-query",
      BIOSEQ_DOC0388_CATALOG_PARITY_SNAPSHOT_QUERY,
      "--approved-delta",
      BIOSEQ_DOC0388_CATALOG_PARITY_APPROVED_DELTA,
      "--active-profile",
      BIOSEQ_DOC0388_CATALOG_PARITY_ACTIVE_PROFILE,
      "--timeout-seconds",
      "180",
  };
  command.insert(command.end(), arguments.begin(), arguments.end());
  REQUIRE(run_evidence_command(command) == 0);
}
