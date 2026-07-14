#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

import bioseq.cache.core;

namespace {

using bioseq::cache::TransactionWriteLedgerV1;
using bioseq::cache::WriteObservationResultV1;
using bioseq::cache::WriteSubtransactionResultV1;

using Ledger = TransactionWriteLedgerV1<std::uint64_t, 4, 3>;

TEST_CASE("write observations merge released savepoints and deduplicate keys",
          "[cache][write-observation]") {
  Ledger ledger;
  REQUIRE(ledger.observe(1, 10) == WriteObservationResultV1::Recorded);
  REQUIRE(ledger.observe(2, 20) == WriteObservationResultV1::Recorded);
  REQUIRE(ledger.observe(2, 10) == WriteObservationResultV1::Recorded);
  REQUIRE(ledger.finish_subtransaction(2, 1, true) ==
          WriteSubtransactionResultV1::Committed);

  std::array<std::uint64_t, 4> keys{};
  REQUIRE(ledger.copy_unique(keys) == 2);
  REQUIRE(keys[0] == 10);
  REQUIRE(keys[1] == 20);
  REQUIRE(ledger.observation_count() == 2);
  REQUIRE(ledger.frame_count() == 1);
  REQUIRE(ledger.transaction_dirty());
  REQUIRE_FALSE(ledger.coarse());
}

TEST_CASE("write observations discard rolled-back savepoint keys",
          "[cache][write-observation]") {
  Ledger ledger;
  REQUIRE(ledger.observe(1, 10) == WriteObservationResultV1::Recorded);
  REQUIRE(ledger.observe(2, 20) == WriteObservationResultV1::Recorded);
  REQUIRE(ledger.finish_subtransaction(2, 1, false) ==
          WriteSubtransactionResultV1::Aborted);

  std::array<std::uint64_t, 4> keys{};
  REQUIRE(ledger.copy_unique(keys) == 1);
  REQUIRE(keys[0] == 10);
  REQUIRE_FALSE(ledger.touches(20));
  REQUIRE(ledger.touches(10));
}

TEST_CASE("coarse fallback remains sticky after savepoint rollback",
          "[cache][write-observation]") {
  Ledger ledger;
  ledger.observe_coarse(2);
  REQUIRE(ledger.finish_subtransaction(2, 1, false) ==
          WriteSubtransactionResultV1::Aborted);
  REQUIRE(ledger.coarse());
  REQUIRE(ledger.transaction_dirty());
  REQUIRE(ledger.touches(999));

  ledger.reset();
  REQUIRE_FALSE(ledger.coarse());
  REQUIRE_FALSE(ledger.transaction_dirty());
}

TEST_CASE("bounded key and frame pressure fail closed to coarse mode",
          "[cache][write-observation]") {
  Ledger key_pressure;
  REQUIRE(key_pressure.observe(1, 1) == WriteObservationResultV1::Recorded);
  REQUIRE(key_pressure.observe(1, 2) == WriteObservationResultV1::Recorded);
  REQUIRE(key_pressure.observe(1, 3) == WriteObservationResultV1::Recorded);
  REQUIRE(key_pressure.observe(1, 4) == WriteObservationResultV1::Recorded);
  REQUIRE(key_pressure.observe(1, 5) ==
          WriteObservationResultV1::CoarseFallback);
  REQUIRE(key_pressure.coarse());
  const std::size_t key_count = key_pressure.observation_count();
  const std::size_t frame_count = key_pressure.frame_count();
  REQUIRE(key_pressure.observe(99, 99) ==
          WriteObservationResultV1::CoarseFallback);
  key_pressure.observe_coarse(100);
  REQUIRE(key_pressure.observation_count() == key_count);
  REQUIRE(key_pressure.frame_count() == frame_count);

  TransactionWriteLedgerV1<std::uint64_t, 8, 2> frame_pressure;
  REQUIRE(frame_pressure.observe(1, 1) == WriteObservationResultV1::Recorded);
  REQUIRE(frame_pressure.observe(2, 2) == WriteObservationResultV1::Recorded);
  REQUIRE(frame_pressure.observe(3, 3) ==
          WriteObservationResultV1::CoarseFallback);
  REQUIRE(frame_pressure.coarse());
}

TEST_CASE("top-level cleanup removes every transaction observation",
          "[cache][write-observation]") {
  Ledger ledger;
  REQUIRE(ledger.observe(1, 10) == WriteObservationResultV1::Recorded);
  ledger.observe_coarse(1);
  ledger.reset();

  std::array<std::uint64_t, 4> keys{};
  REQUIRE(ledger.copy_unique(keys) == 0);
  REQUIRE(ledger.observation_count() == 0);
  REQUIRE(ledger.frame_count() == 0);
  REQUIRE_FALSE(ledger.transaction_dirty());
}

} // namespace
