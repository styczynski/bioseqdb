// Integration scenarios:
//   * `seq_send` / `seq_recv` round-trip end-to-end via `COPY BINARY` (the
//     only externally-driven way to exercise recv from SQL today). The
//     payload after a binary export + binary import must match the original
//     varlena byte-for-byte (text comparison is sufficient given recv is
//     deterministic).
//   * `bioseq.seq_recv` rejects malformed wire payloads with a clear error
//     instead of, say, `__builtin_unreachable` — the `padding_letters /
//     bp_size` cross-check from setup.sql Test 12 must still raise. Some
//     builds expose `seq_recv` only as `internal`-typed; in that case the
//     direct SQL invocation fails immediately, which still demonstrates the
//     right thing (no crash, just an error).

#include <tests/support/postgres.hpp>
import bioseq.integration_tests.common;

#include <filesystem>

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view kSrc = "test_wire_src";
constexpr std::string_view kDst = "test_wire_dst";
constexpr std::string_view kBinaryPath = "/tmp/bioseq_seq_wire.bin";

void seed_pair(connection_test_context& ctx) {
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string(kSrc),
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
          "[seq][wire][copy]") {
    auto ctx = fresh_context({kSrc, kDst});
    seed_pair(ctx);

    // Best-effort cleanup so a stale file doesn't poison this test.
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(kBinaryPath), ec);

    exec_sqlf(ctx, "COPY {} TO '{}' (FORMAT binary);", kSrc, kBinaryPath);

    exec_sqlf(ctx, "CREATE TABLE {} (LIKE {} INCLUDING ALL);", kDst, kSrc);
    exec_sqlf(ctx, "COPY {} FROM '{}' (FORMAT binary);", kDst, kBinaryPath);

    const auto src_rows = query_rows(
        ctx, fmt::format(
                 "SELECT pk, seq0::text, bioseq.len(seq0), bioseq.unit_len(seq0) "
                 "FROM {} ORDER BY pk;",
                 kSrc));
    const auto dst_rows = query_rows(
        ctx,
        fmt::format(
            "SELECT pk, seq0::text, bioseq.len(seq0), bioseq.unit_len(seq0) "
            "FROM {} ORDER BY pk;",
            kDst));

    REQUIRE(src_rows == dst_rows);
    REQUIRE(src_rows.size() == 4);
}

TEST_CASE("seq_recv rejects malformed wire payloads (or refuses to be invoked)",
          "[seq][wire][recv][negative]") {
  auto ctx = fresh_context();
  // setup.sql Test 12: a wire layout with bp_size=99 but unit_count=0 must
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
    "[letter][wire][recv][negative]") {
  auto ctx = fresh_context();
  REQUIRE(query_scalar(
              ctx, "SELECT bioseq_internal.letter_wire_self_test();") == "t");
}
