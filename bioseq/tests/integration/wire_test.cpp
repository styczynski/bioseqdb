// Integration scenarios:
//   * `seq_send` / `seq_recv` round-trip end-to-end via `COPY BINARY` (the
//     only externally-driven way to exercise recv from SQL today). The
//     payload after a binary export + binary import must match the original
//     varlena byte-for-byte (text comparison is sufficient given recv is
//     deterministic).
//   * `bioseq.seq_recv` rejects malformed wire payloads with a clear error
//     instead of, say, `__builtin_unreachable` — the `padding_letters /
//     bp_size` cross-check must still raise. Some
//     builds expose `seq_recv` only as `internal`-typed; in that case the
//     direct SQL invocation fails immediately with an error and no crash.

#include <tests/support/postgres.hpp>
import bioseq.tests.integration.support;

#include <filesystem>

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view k_source_table = "wire_source";
constexpr std::string_view k_destination_table = "wire_destination";
constexpr std::string_view k_binary_path = "/tmp/bioseq_seq_wire.bin";

void seed_pair(connection_test_context& ctx) {
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string(k_source_table),
            .typmod = {.alphabet = "dna4", .width = 64},
            .seq_columns = 1,
        },
        {
            seq_row{.pk = 23,  .sequences = {"ACTAGGCACAT"}},
            seq_row{.pk = 101, .sequences = {"GGGGTTTTACTAGGCACATACTAGGCACATACT"}},
            seq_row{.pk = 69,
                    .sequences = {"ACGTACGTACGTACGTACGTACGTACGTACGTACGTACGTACGT"}},
            seq_row{.pk = 42,  .sequences = {"TTTTTT"}},
        });
}

}  // namespace

TEST_CASE("COPY BINARY round-trip preserves bioseq.seq payloads",
          "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
    auto ctx = fresh_context({k_source_table, k_destination_table});
    seed_pair(ctx);

    // Best-effort cleanup prevents an existing file from affecting the run.
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(k_binary_path), ec);

    exec_sqlf(ctx, "COPY {} TO '{}' (FORMAT binary);", k_source_table, k_binary_path);

    exec_sqlf(ctx, "CREATE TABLE {} (LIKE {} INCLUDING ALL);", k_destination_table, k_source_table);
    exec_sqlf(ctx, "COPY {} FROM '{}' (FORMAT binary);", k_destination_table, k_binary_path);

    const auto src_rows = query_rows(
        ctx, fmt::format(
                 "SELECT pk, seq0::text, bioseq.len(seq0), bioseq.unit_len(seq0) "
                 "FROM {} ORDER BY pk;",
                 k_source_table));
    const auto dst_rows = query_rows(
        ctx,
        fmt::format(
            "SELECT pk, seq0::text, bioseq.len(seq0), bioseq.unit_len(seq0) "
            "FROM {} ORDER BY pk;",
            k_destination_table));

    REQUIRE(src_rows == dst_rows);
    REQUIRE(src_rows.size() == 4);
}

TEST_CASE("seq_recv rejects malformed wire payloads (or refuses to be invoked)",
          "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();
  // A wire layout with bp_size=99 but unit_count=0 must
  // not match the alphabet/width-derived bp_size. Some builds declare
  // seq_recv as `internal`-only; in that case the call itself fails. Both
  // outcomes are acceptable — we only require that the connection stays
  // healthy and the call did NOT silently succeed.
  REQUIRE(sql_fails(
      ctx, "SELECT bioseq.seq_recv("
           "    e'\\\\x0000000000000000630000000000'::bytea, 0::oid, -1);"));

  // Sanity check: connection is still usable after the failed call.
  REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE(
    "letter_recv rejects unknown versions, bad ranks, and typmod mismatch",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();
  REQUIRE(query_scalar(
              ctx, "SELECT bioseq_internal.letter_wire_self_test();") == "t");
}
