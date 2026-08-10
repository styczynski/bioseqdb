module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.sql.renderer:tests.contract.compact_fmgr;

import :fmgr;
import bioseq.codegen.sql.collector;
import bioseq.codegen.sql.graph;

export namespace bioseq::codegen::sql {

namespace compact_fmgr_contract_detail {

inline void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error{"compact fmgr contract failed: " +
                             std::string{message}};
  }
}

struct routine_owner final {
  std::string_view partition;
  std::string_view symbol;
  std::size_t sql_arity;
};

inline constexpr std::array<routine_owner, 65> expected_routines{{
    {"array_string", "seq_concat_text_array", 2},
    {"array_string", "text_array_concat_seq", 2},
    {"array_string", "seq_concat_letter_array", 2},
    {"array_string", "letter_array_concat_seq", 2},
    {"array_string", "array_length", 2},
    {"array_string", "cardinality", 1},
    {"array_string", "to_text", 1},
    {"array_string", "to_varchar", 1},
    {"array_string", "to_array", 1},
    {"array_string", "from_array", 2},
    {"array_string", "from_array", 3},
    {"concat_reverse", "seq_concat", 2},
    {"concat_reverse", "seq_concat_text", 2},
    {"concat_reverse", "text_concat_seq", 2},
    {"concat_reverse", "seq_concat_varchar", 2},
    {"concat_reverse", "varchar_concat_seq", 2},
    {"concat_reverse", "reverse", 1},
    {"concat_reverse", "reverse_complement", 1},
    {"concat_reverse", "array_cat", 2},
    {"content", "seq_cmp", 2},
    {"content", "seq_lt", 2},
    {"content", "seq_le", 2},
    {"content", "seq_eq", 2},
    {"content", "seq_ne", 2},
    {"content", "seq_ge", 2},
    {"content", "seq_gt", 2},
    {"content", "seq_hash", 1},
    {"content", "seq_hash_extended", 2},
    {"io", "seq_typmod_in", 1},
    {"io", "seq_typmod_out", 1},
    {"io", "seq_in", 3},
    {"io", "seq_out", 1},
    {"io", "seq_cast", 3},
    {"io", "seq_send", 1},
    {"io", "seq_recv", 3},
    {"io", "len", 1},
    {"io", "unit_len", 1},
    {"letter", "letter_lt", 2},
    {"letter", "letter_le", 2},
    {"letter", "letter_eq", 2},
    {"letter", "letter_ne", 2},
    {"letter", "letter_ge", 2},
    {"letter", "letter_gt", 2},
    {"letter", "letter_cmp", 2},
    {"letter", "letter_hash", 1},
    {"letter", "letter_hash_extended", 2},
    {"letter", "letter_hook_chain_calls", 0},
    {"letter", "letter_typmod_in", 1},
    {"letter", "letter_typmod_out", 1},
    {"letter", "letter_in", 3},
    {"letter", "letter_out", 1},
    {"letter", "letter_recv", 3},
    {"letter", "letter_send", 1},
    {"letter", "letter_wire_self_test", 0},
    {"letter", "letter", 3},
    {"letter", "letter_to_text", 1},
    {"packed_output", "slice_trace_self_test", 0},
    {"packed_output", "slice_payload_allocation_probe", 1},
    {"packed_output", "slice_trace_worker_failure_probe", 1},
    {"packed_output", "synthetic_lazy_sequence", 1},
    {"packed_output", "materialization_size_self_test", 0},
    {"packed_output", "sequence_piece_count", 1},
    {"packed_output", "expanded_ownership_self_test", 0},
    {"subscript", "seq_subscript_handler", 1},
    {"tree_ops", "piece_tree_self_test", 0},
}};

inline constexpr std::array<std::string_view, 9> expected_imports{{
    "import :array_string;",
    "import :concat_reverse;",
    "import :content;",
    "import :flat_abi;",
    "import :io;",
    "import :letter;",
    "import :packed_output;",
    "import :subscript;",
    "import :tree_ops;",
}};

[[nodiscard]] inline normalized_entity routine(const routine_owner &owner) {
  std::vector<routine_parameter> parameters;
  parameters.reserve(owner.sql_arity);
  std::string identity =
      "sql:function:bioseq." + std::string{owner.symbol} + "(";
  for (std::size_t index = 0; index < owner.sql_arity; ++index) {
    if (index != 0)
      identity += ",";
    identity += "boolean";
    parameters.push_back(routine_parameter{
        .name = "argument_" + std::to_string(index),
        .cpp_type = "bool",
        .sql_type = "boolean",
    });
  }
  identity += ")";

  const std::string source_module =
      "bioseq.postgres.types.compact_sequence:" +
      std::string{owner.partition};
  return normalized_entity{
      .source =
          collected_entity{
              .kind = entity_kind::function,
              .annotation_name = "function",
              .name = std::string{owner.symbol},
              .cpp_qualified_name = "bioseq::pg_types::sql_routines::" +
                                    std::string{owner.symbol},
              .source_module = source_module,
              .source_path = "bioseq/backend/pg/types/compact_sequence/" +
                             std::string{owner.partition} + ".cppm",
              .source_range =
                  {
                      .begin = {.line = 1},
                      .end = {.line = 1},
                  },
              .schema = "bioseq",
              .cpp_return_type = "bool",
              .parameters = std::move(parameters),
              .result_sql_type = "boolean",
          },
      .stable_identity = identity,
      .installed_identity = std::move(identity),
  };
}

[[nodiscard]] inline std::vector<std::string_view> expected_symbols() {
  std::vector<std::string_view> result;
  result.reserve(expected_routines.size());
  for (const routine_owner &owner : expected_routines) {
    result.push_back(owner.symbol);
  }
  std::ranges::sort(result);
  result.erase(std::ranges::unique(result).begin(), result.end());
  return result;
}

} // namespace compact_fmgr_contract_detail

inline void run_compact_fmgr_contract_tests() {
  sql_graph graph;
  for (const compact_fmgr_contract_detail::routine_owner &owner :
       compact_fmgr_contract_detail::expected_routines) {
    graph.entities.push_back(compact_fmgr_contract_detail::routine(owner));
  }

  const std::vector<rendered_fmgr_entrypoint_module> modules =
      render_fmgr_entrypoint_modules(graph);
  compact_fmgr_contract_detail::require(
      modules.size() == 1,
      "all natural compact owners emit one generated partition");
  const rendered_fmgr_entrypoint_module &module = modules.front();
  compact_fmgr_contract_detail::require(
      module.owner_key == "postgres_types_compact_sequence",
      "compact owners share the configured owner key");
  compact_fmgr_contract_detail::require(
      module.source_module
          == "bioseq.postgres.types.compact_sequence:array_string",
      "representative source owner is deterministic");
  compact_fmgr_contract_detail::require(
      module.module_name == "bioseq.postgres.types.compact_sequence:"
                            "generated_sql_entrypoints",
      "generated module is the compact-sequence sibling partition");

  for (const std::string_view expected_import :
       compact_fmgr_contract_detail::expected_imports) {
    compact_fmgr_contract_detail::require(
        module.content.contains(expected_import),
        "generated partition imports every natural owner and ABI "
        "support partition");
  }
  const std::vector<std::string_view> expected_symbols =
      compact_fmgr_contract_detail::expected_symbols();
  compact_fmgr_contract_detail::require(
      compact_fmgr_contract_detail::expected_routines.size() == 65 &&
          expected_symbols.size() == 64,
      "production census has 65 routine identities and 64 symbols");
  compact_fmgr_contract_detail::require(
      module.wrappers.size() == expected_symbols.size(),
      "all production compact symbols are emitted");

  std::vector<std::string_view> actual_symbols;
  actual_symbols.reserve(module.wrappers.size());
  for (const rendered_fmgr_wrapper &wrapper : module.wrappers) {
    actual_symbols.push_back(wrapper.public_symbol);
    if (wrapper.public_symbol == "from_array") {
      compact_fmgr_contract_detail::require(
          wrapper.overloads.size() == 2 &&
              wrapper.overloads[0].sql_arity == 2 &&
              wrapper.overloads[1].sql_arity == 3,
          "from_array retains both production overloads");
    }
  }
  compact_fmgr_contract_detail::require(
      std::ranges::equal(actual_symbols, expected_symbols),
      "generated wrapper symbol census is exact");
}

} // namespace bioseq::codegen::sql
