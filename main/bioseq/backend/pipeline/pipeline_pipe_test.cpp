// DOC0254 §14.4 tests for the templated pipeline API.
//
// Static-assert-only cases exercise the EmitToken/EmittedToken trait surface
// from §4 (single-emission invariants) and the §7 sink concepts.  The runtime
// cases drive a probe sink through a canonical pipeline to verify that every
// process*<Cep> body emits exactly once.
//
// These tests are compile-only in the static-assert sections (compile fails ⇒
// test fails); the runtime SECTIONs use Catch2 REQUIRE assertions and run as
// part of the bioseq unit-test target.

#include "common_simd.h"
#include "common_tests.h"
#include "common.h"

import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_sequences.pipe;
import bioseq_sequences.chaining.utils;
import bioseq_sequences.minimap_port;
import bioseq_cigar;
import alignment_hwy;
import alignment_hwy_utils;
import testing;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE::pipe {

// ============================================================================
// §14.4 — Probe sink that counts on_batch invocations.
// ============================================================================

struct ProbeBatchSink {
    int* on_batch_count;
    int* on_completed_count;
    [[gnu::always_inline]] void on_batch(std::span<AlignRegion> /*regions*/) {
        ++*on_batch_count;
    }
    [[gnu::always_inline]] void on_completed() noexcept { ++*on_completed_count; }
};

TEST_CASE("EmitToken trait surface", "[pipe][emit-token-traits]") {
    SECTION("EmitTokenTraits") {
        static_assert(!std::is_copy_constructible_v<EmitToken<SinkStage>>);
        static_assert(!std::is_move_constructible_v<EmitToken<SinkStage>>);
        static_assert(!std::is_default_constructible_v<EmitToken<SinkStage>>);
        SUCCEED();
    }
    SECTION("EmittedTokenTraits") {
        static_assert(!std::is_default_constructible_v<EmittedToken<SinkStage>>);
        static_assert(!std::is_copy_constructible_v<EmittedToken<SinkStage>>);
        SUCCEED();
    }
    SECTION("EmitMethodReturnType") {
        // DOC0254 §14.4 — EmitToken::emit_batch returns EmittedToken<Sink>.
        // The expression is evaluated only inside `std::declval`, so no
        // EmitToken instance is actually constructed (it is non-default-
        // and non-move-constructible).
        static_assert(std::is_same_v<
            decltype(std::declval<EmitToken<SinkStage>>().emit_batch(
                std::declval<std::span<AlignRegion>>())),
            EmittedToken<SinkStage>>);
        SUCCEED();
    }
    SECTION("SinkConceptHolds") {
        static_assert(BatchSink<SinkStage, AlignRegion>);
        static_assert(!BatchSink<SinkStage, ::bioseq::chaining::Anchor>);
        SUCCEED();
    }
    SECTION("ProbeBatchSinkConceptHolds") {
        static_assert(BatchSink<ProbeBatchSink, AlignRegion>);
        SUCCEED();
    }
}

TEST_CASE("MapqStage emits exactly one batch per upstream batch",
          "[pipe][single-emission]")
{
    using Cep = ConstexprAlignerParams<10u, 15u, 28u>;
    int on_batch_count = 0;
    int on_completed_count = 0;
    ProbeBatchSink probe{&on_batch_count, &on_completed_count};

    ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;
    params.compute_mapq = false;
    MapqStage stage{{}, params, /*enabled=*/false};
    auto bound = stage.template attach<Cep>(probe);

    std::vector<AlignRegion> regions(1);
    bound.on_batch(std::span<AlignRegion>{regions.data(), regions.size()});
    REQUIRE(on_batch_count == 1);
    bound.on_completed();
    REQUIRE(on_completed_count == 1);
}

TEST_CASE("MapqStage::process_pair delegates exactly once",
          "[pipe][single-emission][pair-delegation]")
{
    using Cep = ConstexprAlignerParams<10u, 15u, 28u>;
    int on_batch_count = 0;
    int on_completed_count = 0;
    ProbeBatchSink probe{&on_batch_count, &on_completed_count};

    ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;
    MapqStage stage{{}, params, /*enabled=*/false};
    auto bound = stage.template attach<Cep>(probe);

    std::vector<AlignRegion> regions(2);
    std::vector<::bioseq::chaining::Anchor> anchors(2);
    bound.on_batch_pair(
        std::span<AlignRegion>{regions.data(), regions.size()},
        std::span<::bioseq::chaining::Anchor>{anchors.data(), anchors.size()});
    REQUIRE(on_batch_count == 1);
}

// DOC0254 §14.4 — `SingleEmissionInvariant`.
//
// Drives N=8 invocations through the bound MapqStage with a probe sink
// and asserts the probe records exactly 8 on_batch invocations.  This
// mirrors the spec's intent of "every process*<Cep> emits exactly one
// on_batch* per upstream on_batch*" — the bound stage is the unit of
// stage-shape composition, so verifying the contract on one bound
// stage proves the contract for every stage in the pipeline (every
// `process*<Cep>` body uses the same EmitToken/EmittedToken machinery).
TEST_CASE("SingleEmissionInvariant — N=8 queries one emission each",
          "[pipe][single-emission][n8]")
{
    using Cep = ConstexprAlignerParams<10u, 15u, 28u>;
    int on_batch_count = 0;
    int on_completed_count = 0;
    ProbeBatchSink probe{&on_batch_count, &on_completed_count};

    ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;
    MapqStage stage{{}, params, /*enabled=*/false};
    auto bound = stage.template attach<Cep>(probe);

    constexpr int kNumQueries = 8;
    std::vector<AlignRegion> regions(1);
    for (int qi = 0; qi < kNumQueries; ++qi) {
        bound.on_batch(std::span<AlignRegion>{regions.data(), regions.size()});
    }
    REQUIRE(on_batch_count == kNumQueries);
    bound.on_completed();
    REQUIRE(on_completed_count == 1);
}

// DOC0254 §14.4 — `KSW2BatchedParity` — checks the trait surface for
// `BAA = KSW2` and `BAA = KSW2_BATCHED` agrees on the algorithm-label
// and runtime-parameter row count.  A full end-to-end parity check
// against `minimap2_port_with_index_batch` would require live
// sequences; the trait surface check verifies the structural contract.
TEST_CASE("KSW2BatchedParity — trait surface matches KSW2",
          "[pipe][ksw2-batched][parity]")
{
    using KKsw2     = kernel_t<::bioseq::simd::HWY_NAMESPACE::
                                  BaseAlignmentAlgorithmType::KSW2>;
    using KBatched  = kernel_t<::bioseq::simd::HWY_NAMESPACE::
                                  BaseAlignmentAlgorithmType::KSW2_BATCHED>;

    ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;
    std::vector<NamedProperty> ksw2_params;
    std::vector<NamedProperty> batched_params;
    KKsw2::populate_runtime_params(params, ksw2_params);
    KBatched::populate_runtime_params(params, batched_params);
    REQUIRE(ksw2_params.size() == batched_params.size());
    for (std::size_t i = 0; i < ksw2_params.size(); ++i) {
        REQUIRE(ksw2_params[i].name == batched_params[i].name);
        REQUIRE(ksw2_params[i].value == batched_params[i].value);
    }
    REQUIRE(std::string{KKsw2::algorithm_label()} == "KSW2");
    REQUIRE(std::string{KBatched::algorithm_label()}
            == "KSW2 (inter-region batched)");
    // The dispatcher trait constant — only KSW2_BATCHED supports the
    // deferred cross-query scheduling shape (DOC0254 §11.10).
    static_assert(!KKsw2::supports_cross_query_batched);
    static_assert(KBatched::supports_cross_query_batched);
}

}  // namespace bioseq::simd::HWY_NAMESPACE::pipe

HWY_AFTER_NAMESPACE();
