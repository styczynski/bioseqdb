#ifndef NO_POSTGRES
#error "external rank-pack adapter proof must compile with NO_POSTGRES"
#endif

#ifndef BIOSEQ_PG_PACK_DENYLIST_ENABLED
#error "external rank-pack adapter proof requires the PG source denylist"
#endif

#if defined(POSTGRES_H) || defined(FMGR_H) || defined(PG_VERSION_NUM) \
    || defined(ereport) || defined(PG_FUNCTION_ARGS)
#error "PostgreSQL header or API macro leaked into the external rank-pack path"
#endif

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

import bioseq.parallelism.memory_admission;
import bioseq.sequences;
import bioseq.sequences.rank_pack;
import bioseq.sequences.rank_pack_core;
import bioseq.storage.lazy_sequence;
import bioseq.storage.scanner;

TEST_CASE("external-api executes the PG-free fixed-span and admitted adapters",
          "[component:external_api][layer:integration][feature:external_source]") {
    using instance = sequence_custom_alphabet<seq::dna4, std::uint32_t>;
    using sink = bioseq::sequences::fixed_rank_pack_unit_sink<std::uint32_t>;
    using writer_type = bioseq::sequences::checked_rank_pack_sink<
        instance,
        bioseq::sequences::invalid_rank_policy::reject,
        bioseq::sequences::pack_capacity_policy::fixed_span,
        sink>;

    constexpr std::array<std::uint8_t, 9> ranks{
        0u, 1u, 2u, 3u, 3u, 2u, 1u, 0u, 1u};
    std::array<std::uint32_t, 1> fixed_units{};
    writer_type writer{sink{std::span<std::uint32_t>{fixed_units}}};
    REQUIRE(writer.append_ranks(ranks)
            == bioseq::sequences::rank_pack_status::ok);
    const auto fixed = writer.finish(ranks.size(), fixed_units.size());
    REQUIRE(fixed.status == bioseq::sequences::rank_pack_status::ok);
    REQUIRE(fixed.emitted_symbols == ranks.size());
    REQUIRE(fixed.written_units == fixed_units.size());

    std::vector<seq::dna4> letters(ranks.size());
    for (std::size_t index = 0u; index < ranks.size(); ++index) {
        letters[index].assign_rank(ranks[index]);
    }
    compact_sequence_range<instance> range{letters};
    const auto source = bioseq::storage::make_lazy_sequence_view(range);
    const auto access = bioseq::storage::plan_sequence_access(
        source,
        bioseq::storage::sequence_access_requirement::bounded_random_window,
        0u,
        source.bp_size(),
        sizeof(std::uint32_t));
    REQUIRE(access.route
            == bioseq::storage::sequence_access_route::materialize_admitted);

    auto& admission =
        bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    struct restore_cap {
        bioseq::parallelism::memory_admission_controller* admission;
        std::uint64_t cap;
        ~restore_cap() { admission->set_index_memory_cap_bytes(cap); }
    } restore{std::addressof(admission), previous_cap};
    admission.set_index_memory_cap_bytes(0u);
    const std::uint64_t baseline = admission.reserved_bytes();
    auto permit = admission.try_reserve(access.admission_bytes);
    REQUIRE(permit.has_value());
    {
        auto owner = bioseq::storage::materialize_packed_sequence(
            source,
            access.from,
            access.to,
            access.requested_width,
            std::move(*permit));
        REQUIRE(admission.reserved_bytes()
                == baseline + access.admission_bytes);
        REQUIRE(owner.descriptor().storage_unit_width
                == sizeof(std::uint32_t));
        REQUIRE(owner.descriptor().unit_count == fixed_units.size());
        REQUIRE(*static_cast<const std::uint32_t*>(owner.descriptor().units)
                == fixed_units[0]);

        std::array<std::uint8_t, ranks.size()> observed{};
        owner.view().template to_encoded_into<false, false>(
            observed, 0u, observed.size());
        REQUIRE(observed == ranks);
    }
    REQUIRE(admission.reserved_bytes() == baseline);
}
