/**
 * @file hash_major_test.cpp
 * @brief Tests hash-major minimizer wire codecs and selective readers.
 *
 * The tests run inside `bioseq::simd::HWY_NAMESPACE` because they share the
 * fixed-target minimizer test environment.
 */

#include <hwy/highway.h>
#include <sys/mman.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.context;
import bioseq.common.seqan_adapter;
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.sequences;
import bioseq.chaining.core;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.sequences.kmers.minimap2_sketch_baseline;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.runtime_contract;
import bioseq.gpu.vulkan.runtime;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.indexing.symbol_counts;
import bioseq.parallelism.memory_admission;
import bioseq.indexing.registry;
import bioseq.common.hashing; //  shared SHA-256

import bioseq.tests.support.indexing_storage_fakes;

import bioseq.tests.unit.indexing.minimizer_hashmap_support;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {
using namespace minimizer_hashmap_test_support;


namespace {

/// Construct the byte-pinned generation-head fixture.
[[nodiscard]] constexpr bioseq::indexing::minimizer_generation_head
golden_generation_head() noexcept {
    return {
        .layout = bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local,
        .generation_id = 0x0102030405060708ULL,
        .source_identity = 0x1112131415161718ULL,
        .storage_contract_checksum = 0x2122232425262728ULL,
        .manifest_checksum = 0x3132333435363738ULL,
        .source_content_checksum = 0x4142434445464748ULL,
    };
}
} // namespace

/// In-memory source and sink for the three hash-major logical streams.
struct hash_major_memory_image {
    /// Record one logical read and its byte interval.
    struct read_event {
        bioseq::indexing::hash_major_minimizer_stream_kind kind{};
        std::uint64_t offset = 0;
        std::size_t byte_count = 0;

        [[nodiscard]] bool operator==(const read_event&) const = default;
    };

    std::array<std::vector<std::byte>, 3> streams{};
    std::vector<bioseq::indexing::hash_major_minimizer_stream_kind> append_order{};
    std::vector<read_event> reads{};
    std::vector<std::vector<bioseq::indexing::hash_major_minimizer_authorized_control_range>>
        control_waves{};
    std::vector<std::vector<bioseq::indexing::hash_major_minimizer_authorized_posting_range>>
        posting_waves{};
    bioseq::parallelism::memory_admission_controller* cap_controller = nullptr;
    std::optional<std::uint64_t> cap_after_directory_read{};
    std::optional<std::uint64_t> cap_after_control_wave{};
    std::size_t size_calls = 0u;
    bool use_wave_callbacks = true;

    /// Convert a persisted stream kind to its fixture-array index.
    [[nodiscard]] static std::size_t
    index(bioseq::indexing::hash_major_minimizer_stream_kind kind) {
        return static_cast<std::size_t>(kind) - 1u;
    }

    /// Append bytes to one logical stream.
    static bool append_callback(void* context,
                                bioseq::indexing::hash_major_minimizer_stream_kind kind,
                                std::span<const std::byte> bytes) {
        auto& self = *static_cast<hash_major_memory_image*>(context);
        auto& stream = self.streams[index(kind)];
        stream.insert(stream.end(), bytes.begin(), bytes.end());
        self.append_order.push_back(kind);
        return true;
    }

    /// Read one checked byte interval and record the operation.
    static bool read_callback(void* context,
                              bioseq::indexing::hash_major_minimizer_stream_kind kind,
                              std::uint64_t offset,
                              std::span<std::byte> output) {
        auto& self = *static_cast<hash_major_memory_image*>(context);
        const auto& stream = self.streams[index(kind)];
        if (offset > stream.size() || output.size() > stream.size() - offset) {
            return false;
        }
        std::copy_n(
            stream.begin() + static_cast<std::size_t>(offset), output.size(), output.begin());
        self.reads.push_back(read_event{
            .kind = kind,
            .offset = offset,
            .byte_count = output.size(),
        });
        if (kind == bioseq::indexing::hash_major_minimizer_stream_kind::directory &&
            self.cap_controller != nullptr && self.cap_after_directory_read) {
            self.cap_controller->set_index_memory_cap_bytes(*self.cap_after_directory_read);
        }
        return true;
    }

    /// Return one logical stream size and record the query.
    static std::uint64_t size_callback(void* context,
                                       bioseq::indexing::hash_major_minimizer_stream_kind kind) {
        auto& self = *static_cast<hash_major_memory_image*>(context);
        ++self.size_calls;
        return self.streams[index(kind)].size();
    }

    /// Deliver authorized control ranges through the wave callback.
    static bool control_wave_callback(
        void* context,
        std::span<const bioseq::indexing::hash_major_minimizer_authorized_control_range> ranges,
        void* consumer_context,
        bioseq::indexing::hash_major_minimizer_consume_control_block_fn consume) {
        auto& self = *static_cast<hash_major_memory_image*>(context);
        self.control_waves.emplace_back(ranges.begin(), ranges.end());
        const auto& stream =
            self.streams[index(bioseq::indexing::hash_major_minimizer_stream_kind::controls)];
        for (std::size_t ordinal = 0u; ordinal < ranges.size(); ++ordinal) {
            const auto& range = ranges[ordinal];
            if (range.control_byte_offset > stream.size() ||
                range.byte_count > stream.size() - range.control_byte_offset) {
                return false;
            }
            const auto bytes = std::span<const std::byte>{stream}.subspan(
                static_cast<std::size_t>(range.control_byte_offset), range.byte_count);
            self.reads.push_back(read_event{
                .kind = bioseq::indexing::hash_major_minimizer_stream_kind::controls,
                .offset = range.control_byte_offset,
                .byte_count = range.byte_count,
            });
            if (!consume(consumer_context, static_cast<std::uint32_t>(ordinal), bytes)) {
                return false;
            }
        }
        if (self.cap_controller != nullptr && self.cap_after_control_wave) {
            self.cap_controller->set_index_memory_cap_bytes(*self.cap_after_control_wave);
        }
        return true;
    }

    /// Deliver authorized posting ranges in bounded read slabs.
    static bool posting_wave_callback(
        void* context,
        std::span<const bioseq::indexing::hash_major_minimizer_authorized_posting_range> ranges,
        void* consumer_context,
        bioseq::indexing::hash_major_minimizer_consume_posting_bytes_fn consume) {
        auto& self = *static_cast<hash_major_memory_image*>(context);
        self.posting_waves.emplace_back(ranges.begin(), ranges.end());
        const auto& stream =
            self.streams[index(bioseq::indexing::hash_major_minimizer_stream_kind::postings)];
        for (std::size_t ordinal = 0u; ordinal < ranges.size(); ++ordinal) {
            const auto& range = ranges[ordinal];
            if (range.posting_byte_offset > stream.size() ||
                range.byte_count > stream.size() - range.posting_byte_offset) {
                return false;
            }
            std::uint64_t relative = 0u;
            while (relative < range.byte_count) {
                const std::size_t byte_count = static_cast<std::size_t>(std::min<std::uint64_t>(
                    bioseq::indexing::k_hash_major_minimizer_read_slab_bytes,
                    range.byte_count - relative));
                const std::uint64_t absolute = range.posting_byte_offset + relative;
                const auto bytes = std::span<const std::byte>{stream}.subspan(
                    static_cast<std::size_t>(absolute), byte_count);
                self.reads.push_back(read_event{
                    .kind = bioseq::indexing::hash_major_minimizer_stream_kind::postings,
                    .offset = absolute,
                    .byte_count = byte_count,
                });
                if (!consume(
                        consumer_context, static_cast<std::uint32_t>(ordinal), relative, bytes)) {
                    return false;
                }
                relative += byte_count;
            }
        }
        return true;
    }

    /// Return the erased logical-stream sink bound to this image.
    [[nodiscard]] bioseq::indexing::hash_major_minimizer_logical_stream_sink_ref sink() {
        return {
            .context = this,
            .append_bytes = &append_callback,
        };
    }

    /// Return the erased logical-stream source bound to this image.
    [[nodiscard]] bioseq::indexing::hash_major_minimizer_logical_stream_source_ref source() {
        return {
            .context = this,
            .read_bytes = &read_callback,
            .logical_size = &size_callback,
            .read_control_wave = use_wave_callbacks ? &control_wave_callback : nullptr,
            .read_posting_wave = use_wave_callbacks ? &posting_wave_callback : nullptr,
        };
    }

    /// Select recorded reads for one logical stream.
    [[nodiscard]] std::vector<read_event>
    reads_of(bioseq::indexing::hash_major_minimizer_stream_kind kind) const {
        std::vector<read_event> selected;
        std::ranges::copy_if(reads, std::back_inserter(selected), [kind](const read_event& event) {
            return event.kind == kind;
        });
        return selected;
    }
};

/// Construct hash-major generation options for one target length.
[[nodiscard]] bioseq::indexing::hash_major_minimizer_generation_options
hash_major_test_options(std::uint64_t target_length) {
    return bioseq::indexing::hash_major_minimizer_generation_options{
        .generation_id = 91u,
        .query_key = 17u,
        .source_epoch = 23u,
        .source_content_crc64 = 0x0123456789abcdefULL,
        .target_length = target_length,
        .storage_profile =
            bioseq::indexing::hash_major_minimizer_storage_profile{
                .hash_scheme_identity = 0u,
                .hash_scheme_revision = 0u,
                .alphabet_code = 0u,
                .kmer_size = 15u,
                .window_size = 10u,
                .hash_width_bytes = 4u,
                .stored_posting_width_bytes =
                    static_cast<std::uint8_t>(target_length <= (std::uint64_t{1} << 31u) ? 4u : 8u),
                .position_encoding_revision = 1u,
                .hash_semantics_revision = 1u,
                .count_semantics_revision = 1u,
                .producer_backend_identity = 1u,
                .producer_backend_revision = 1u,
                .controls_per_block = bioseq::indexing::k_hash_major_minimizer_controls_per_block,
                .hash_seed = 0u,
            },
    };
}

/// Derive the reader identity expected from generation options.
[[nodiscard]] bioseq::indexing::expected_hash_major_minimizer_generation
hash_major_expected(const bioseq::indexing::hash_major_minimizer_generation_options& options) {
    return {
        .generation_id = options.generation_id,
        .query_key = options.query_key,
        .source_epoch = options.source_epoch,
        .source_content_crc64 = options.source_content_crc64,
        .target_length = options.target_length,
        .storage_profile = options.storage_profile,
    };
}

/// Describe a canonical 32-bit hash query for one storage profile.
[[nodiscard]] bioseq::indexing::canonical_query_hash_view hash_major_query_view(
    std::span<const std::uint32_t> hashes,
    const bioseq::indexing::hash_major_minimizer_storage_profile& storage_profile) {
    return {
        .values32 = hashes,
        .width_bytes = 4u,
        .storage_width_bytes = 4u,
        .storage_profile_digest =
            bioseq::indexing::hash_major_minimizer_storage_profile_digest(storage_profile),
    };
}

/// Describe a canonical 64-bit hash query for one storage profile.
[[nodiscard]] bioseq::indexing::canonical_query_hash_view hash_major_query_view(
    std::span<const std::uint64_t> hashes,
    const bioseq::indexing::hash_major_minimizer_storage_profile& storage_profile) {
    return {
        .values64 = hashes,
        .width_bytes = 8u,
        .storage_width_bytes = 8u,
        .storage_profile_digest =
            bioseq::indexing::hash_major_minimizer_storage_profile_digest(storage_profile),
    };
}

/// Encode a byte range as lowercase hexadecimal text.
[[nodiscard]] std::string hash_major_hex(std::span<const std::byte> bytes) {
    constexpr std::string_view digits{"0123456789abcdef"};
    std::string output;
    output.resize(bytes.size() * 2u);
    for (std::size_t index = 0u; index < bytes.size(); ++index) {
        const std::uint8_t value = std::to_integer<std::uint8_t>(bytes[index]);
        output[index * 2u] = digits[value >> 4u];
        output[index * 2u + 1u] = digits[value & 0x0fu];
    }
    return output;
}

/// Recompute a mutated hash-major control block checksum.
void recompute_hash_major_control_crc(std::vector<std::byte>& bytes) {
    using bioseq::indexing::crc64_ecma182_accumulator;
    REQUIRE(bytes.size() >= bioseq::indexing::k_hash_major_minimizer_control_header_bytes);
    crc64_ecma182_accumulator crc;
    crc.update(std::span<const std::byte>{bytes}.first(48u));
    crc.update(std::span<const std::byte>{bytes}.subspan(
        bioseq::indexing::k_hash_major_minimizer_control_header_bytes));
    test_write_le<std::uint64_t>(std::span<std::byte>{bytes}, 48u, crc.checksum());
}

/// Own a small encoded hash-major image and its source pairs.
struct hash_major_fixture {
    hash_major_memory_image image{};
    bioseq::indexing::hash_major_minimizer_generation_options options{};
    bioseq::indexing::hash_major_minimizer_encoded_metadata metadata{};
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{};
};

/// Build the three-hash fixture used by hash-major reader tests.
[[nodiscard]] hash_major_fixture make_hash_major_small_fixture() {
    using namespace bioseq::indexing;
    hash_major_fixture fixture;
    fixture.options = hash_major_test_options(100u);
    hash_major_minimizer_logical_encoder encoder{fixture.options, fixture.image.sink()};
    const std::array<std::uint64_t, 2> first{2u, 4u};
    const std::array<std::uint64_t, 1> middle{10u};
    const std::array<std::uint64_t, 2> last{20u, 21u};
    REQUIRE(encoder.append_hash(10u, first).has_value());
    REQUIRE(encoder.append_hash(20u, middle).has_value());
    REQUIRE(encoder.append_hash(30u, last).has_value());
    auto metadata = encoder.finish();
    REQUIRE(metadata.has_value());
    fixture.metadata = *metadata;
    fixture.pairs = {
        {10u, 2u},
        {10u, 4u},
        {20u, 10u},
        {30u, 20u},
        {30u, 21u},
    };
    return fixture;
}

TEST_CASE("hash-major exact wire codecs preserve the grouped-SoA layout",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    STATIC_REQUIRE(minimizer_generation_head::encoded_bytes == 64u);
    STATIC_REQUIRE(minimizer_generation_manifest::encoded_bytes == 176u);
    STATIC_REQUIRE(k_hash_major_minimizer_head_bytes == 224u);
    STATIC_REQUIRE(k_hash_major_minimizer_manifest_bytes == 336u);
    STATIC_REQUIRE(k_hash_major_minimizer_directory_entry_bytes == 80u);
    STATIC_REQUIRE(sizeof(exact_global_count_exception) == 16u);
    STATIC_REQUIRE(sizeof(posting_request) == 24u);
    STATIC_REQUIRE(sizeof(hash_major_minimizer_authorized_control_range) == 24u);
    STATIC_REQUIRE(sizeof(hash_major_minimizer_authorized_posting_range) == 48u);

    auto fixture = make_hash_major_small_fixture();
    REQUIRE(fixture.metadata.encoded_head.size() == 224u);
    REQUIRE(fixture.metadata.encoded_manifest.size() == 336u);
    REQUIRE(fixture.image.append_order == std::vector<hash_major_minimizer_stream_kind>{
                                              hash_major_minimizer_stream_kind::postings,
                                              hash_major_minimizer_stream_kind::controls,
                                              hash_major_minimizer_stream_kind::directory});
    REQUIRE(std::to_integer<char>(fixture.metadata.encoded_head[0]) == 'B');
    REQUIRE(std::to_integer<char>(fixture.metadata.encoded_head[6]) == '6');
    REQUIRE(std::to_integer<char>(fixture.metadata.encoded_manifest[0]) == 'B');
    REQUIRE(std::to_integer<char>(fixture.metadata.encoded_manifest[6]) == '6');

    const auto decoded_head = decode_hash_major_minimizer_head(fixture.metadata.encoded_head);
    const auto decoded_manifest =
        decode_hash_major_minimizer_manifest(fixture.metadata.encoded_manifest);
    REQUIRE(decoded_head.has_value());
    REQUIRE(decoded_manifest.has_value());
    REQUIRE(*decoded_head == fixture.metadata.head);
    REQUIRE(*decoded_manifest == fixture.metadata.manifest);
    const std::array<std::byte, 16> expected_source_identity{
        std::byte{0x9e},
        std::byte{0xe7},
        std::byte{0x39},
        std::byte{0xaa},
        std::byte{0x80},
        std::byte{0xf7},
        std::byte{0x43},
        std::byte{0xdc},
        std::byte{0x1f},
        std::byte{0xf3},
        std::byte{0x35},
        std::byte{0x3e},
        std::byte{0xa5},
        std::byte{0x04},
        std::byte{0xe3},
        std::byte{0xaa},
    };
    const std::array<std::byte, 32> expected_storage_profile_digest{
        std::byte{0x18}, std::byte{0xdb}, std::byte{0x2e}, std::byte{0x20}, std::byte{0x33},
        std::byte{0x2c}, std::byte{0xeb}, std::byte{0x74}, std::byte{0xbe}, std::byte{0xb2},
        std::byte{0x4d}, std::byte{0xda}, std::byte{0x8e}, std::byte{0xf6}, std::byte{0xa5},
        std::byte{0xd2}, std::byte{0x82}, std::byte{0x17}, std::byte{0x34}, std::byte{0x18},
        std::byte{0x50}, std::byte{0xbe}, std::byte{0x23}, std::byte{0x94}, std::byte{0x7f},
        std::byte{0xd8}, std::byte{0x46}, std::byte{0x35}, std::byte{0x26}, std::byte{0x2e},
        std::byte{0xa1}, std::byte{0xfb},
    };
    REQUIRE(decoded_head->source_identity == expected_source_identity);
    REQUIRE(decoded_head->storage_profile_digest == expected_storage_profile_digest);
    REQUIRE(decoded_manifest->unique_hash_count == 3u);
    REQUIRE(decoded_manifest->total_posting_count == 5u);
    REQUIRE(decoded_manifest->streams[0].logical_bytes == 80u);
    REQUIRE(decoded_manifest->streams[2].logical_bytes == 20u);
    REQUIRE(decoded_manifest->streams[0].first_row_ordinal == 0u);
    REQUIRE(decoded_manifest->streams[1].first_row_ordinal == 1u);
    REQUIRE(decoded_manifest->streams[2].first_row_ordinal == 2u);
    REQUIRE(decoded_manifest->streams[0].stream_crc64 == 0xd9cbbef0ecb8f6a4ULL);
    REQUIRE(decoded_manifest->streams[1].stream_crc64 == 0xbfd39c3c3e488c19ULL);
    REQUIRE(decoded_manifest->streams[2].stream_crc64 == 0x5cf59f98c6058b27ULL);
    REQUIRE(decoded_manifest->generation_crc64 == 0xedd9cd57f12d5423ULL);
    REQUIRE(decoded_head->manifest_payload_crc64 == 0xe8bab5bdeba98b87ULL);
    REQUIRE(decoded_head->manifest_address_bound_row_crc64 == 0x8fdbf93d04832856ULL);
    REQUIRE(decoded_head->head_crc64 == 0xdc5d0729f8d20fc8ULL);

    // These are independent full-byte wire images, not values round-tripped
    // through the implementation under test. They pin every reserved byte,
    // descriptor ordinal, checksum field, and packed control tail.
    REQUIRE(hash_major_hex(fixture.metadata.encoded_head) ==
            "42534551474836000600e000000000005b000000000000001100000000000000"
            "17000000000000009ee739aa80f743dc1ff3353ea504e3aaefcdab8967452301"
            "18db2e20332ceb74beb24dda8ef6a5d28217341850be23947fd84635262ea1fb"
            "0000000000000000000000000000000000000000000000005001000001000000"
            "562883043df9db8f878ba9ebbdb5bae8c80fd2f829075ddc0000000000000000"
            "0000000000000000000000000000000000000000000000000000000000000000"
            "0000000000000000000000000000000000000000000000000000000000000000");
    REQUIRE(hash_major_hex(fixture.metadata.encoded_manifest) ==
            "425345514e4c36000600c000000000005b000000000000001700000000000000"
            "9ee739aa80f743dc1ff3353ea504e3aaefcdab89674523016400000000000000"
            "0300000000000000050000000000000000000000000000000f000a0004040003"
            "0100010001000100010000000010000018db2e20332ceb74beb24dda8ef6a5d2"
            "8217341850be23947fd84635262ea1fba4f6b8ecf0becbd923542df157cdd9ed"
            "4800000000000000140000000000000001000000000000000000000000000000"
            "0100010050000000010000000000000050000000000000000100000000000000"
            "a4f6b8ecf0becbd9000000000000000002000100000000000300000000000000"
            "48000000000000000100000000000000198c483e3c9cd3bf0100000000000000"
            "0300010004000000050000000000000014000000000000000100000000000000"
            "278b05c6989ff55c0200000000000000");
    REQUIRE(hash_major_hex(fixture.image.streams[0]) ==
            "0a000000000000001e0000000000000000000000000000000000000000000000"
            "4800000003000000000000000000000000000000000000001400000000000000"
            "050000000000000099128aad8d85ba2d");
    REQUIRE(hash_major_hex(fixture.image.streams[1]) ==
            "0a000000000000000300000005100000050000000000000000000000000000"
            "0002000000060000000000000000000000dc72c00206f5e8fd278b05c6989f"
            "f55c4051020001000200");
    REQUIRE(hash_major_hex(fixture.image.streams[2]) == "02000000040000000a0000001400000015000000");

    SECTION("borrowed canonical lower_bound is hard-bounded by its radix") {
        const std::array<std::uint32_t, 5> hashes{1u, 2u, 0x0001'0000u, 0x0001'0001u, 0x0002'0000u};
        canonical_query_hash_view view{
            .values32 = std::span<const std::uint32_t>{hashes},
            .width_bytes = 4u,
            .storage_width_bytes = 4u,
        };
        REQUIRE(view.lower_bound(2u, 4u, 0x0002'0000u) == 4u);
        REQUIRE(view.lower_bound(2u, 4u, 0x0001'0001u) == 3u);
    }

    SECTION("head and manifest corruption fail closed") {
        auto bad_head = fixture.metadata.encoded_head;
        bad_head[208] ^= std::byte{1};
        REQUIRE_FALSE(decode_hash_major_minimizer_head(bad_head).has_value());

        auto bad_manifest = fixture.metadata.encoded_manifest;
        bad_manifest[198] ^= std::byte{1};
        REQUIRE_FALSE(decode_hash_major_minimizer_manifest(bad_manifest).has_value());
    }
}

TEST_CASE("hash-major control codec pins zero delta and exact count exceptions",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    const std::array<std::uint64_t, 1> hashes{42u};
    const std::array<std::uint64_t, 1> counts{65'535u};
    auto encoded = encode_hash_major_minimizer_control_block(
        hashes, counts, 0u, 0x9988776655443322ULL, 4u, 4u);
    REQUIRE(encoded.has_value());
    REQUIRE(encoded->size() == 64u + 2u + 12u);
    auto decoded = decode_hash_major_minimizer_control_block(*encoded, 4u, 4u);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->header.hash_delta_bit_width == 0u);
    REQUIRE(decoded->header.hash_bytes == 0u);
    REQUIRE(decoded->header.count_bytes == 2u);
    REQUIRE(decoded->header.exception_bytes == 12u);
    const std::array<std::byte, 8> expected_control_crc_le{
        std::byte{0x4b},
        std::byte{0xac},
        std::byte{0x08},
        std::byte{0xa1},
        std::byte{0x07},
        std::byte{0x6a},
        std::byte{0x71},
        std::byte{0x29},
    };
    REQUIRE(std::ranges::equal(std::span<const std::byte>{*encoded}.subspan(48u, 8u),
                               expected_control_crc_le));
    REQUIRE(decoded->hashes == std::vector<std::uint64_t>{42u});
    REQUIRE(decoded->exact_counts == std::vector<std::uint64_t>{65'535u});

    SECTION("payload checksum") {
        auto corrupt = *encoded;
        corrupt[65] ^= std::byte{1};
        REQUIRE_FALSE(decode_hash_major_minimizer_control_block(corrupt, 4u, 4u).has_value());
    }

    SECTION("noncanonical delta width") {
        const std::array<std::uint64_t, 2> two_hashes{42u, 43u};
        const std::array<std::uint64_t, 2> two_counts{1u, 1u};
        auto canonical =
            encode_hash_major_minimizer_control_block(two_hashes, two_counts, 0u, 0u, 4u, 4u);
        REQUIRE(canonical.has_value());
        auto corrupt = *canonical;
        REQUIRE(std::to_integer<std::uint8_t>(corrupt[12]) == 1u);
        corrupt[12] = std::byte{2};
        corrupt[64] = std::byte{4};
        recompute_hash_major_control_crc(corrupt);
        const auto rejected = decode_hash_major_minimizer_control_block(corrupt, 4u, 4u);
        REQUIRE_FALSE(rejected.has_value());
        REQUIRE(rejected.error() == hash_major_minimizer_error::control);
    }

    SECTION("reserved flags and packed tail bits") {
        const std::array<std::uint64_t, 2> two_hashes{42u, 43u};
        const std::array<std::uint64_t, 2> two_counts{1u, 1u};
        auto canonical =
            encode_hash_major_minimizer_control_block(two_hashes, two_counts, 0u, 0u, 4u, 4u);
        REQUIRE(canonical.has_value());

        auto reserved = *canonical;
        reserved[14] = std::byte{1};
        recompute_hash_major_control_crc(reserved);
        REQUIRE_FALSE(decode_hash_major_minimizer_control_block(reserved, 4u, 4u));

        auto tail = *canonical;
        tail[64] |= std::byte{0x80};
        recompute_hash_major_control_crc(tail);
        const auto rejected = decode_hash_major_minimizer_control_block(tail, 4u, 4u);
        REQUIRE_FALSE(rejected.has_value());
        REQUIRE(rejected.error() == hash_major_minimizer_error::reserved_nonzero);
    }

    SECTION("exception records are exact, ordered, and exhaustive") {
        const std::array<std::uint64_t, 2> two_hashes{42u, 43u};
        const std::array<std::uint64_t, 2> two_counts{65'535u, 65'536u};
        auto canonical =
            encode_hash_major_minimizer_control_block(two_hashes, two_counts, 0u, 0u, 4u, 4u);
        REQUIRE(canonical.has_value());
        REQUIRE(canonical->size() == 64u + 1u + 4u + 24u);

        auto duplicate = *canonical;
        test_write_le<std::uint16_t>(std::span<std::byte>{duplicate}, 81u, 0u);
        recompute_hash_major_control_crc(duplicate);
        REQUIRE_FALSE(decode_hash_major_minimizer_control_block(duplicate, 4u, 4u));

        auto missing = *canonical;
        missing.resize(missing.size() - 12u);
        test_write_le<std::uint32_t>(std::span<std::byte>{missing}, 40u, 12u);
        recompute_hash_major_control_crc(missing);
        REQUIRE_FALSE(decode_hash_major_minimizer_control_block(missing, 4u, 4u));

        auto trailing = *canonical;
        trailing.push_back(std::byte{0});
        REQUIRE_FALSE(decode_hash_major_minimizer_control_block(trailing, 4u, 4u));
    }
}

TEST_CASE("grouped-SoA and hash-major tagged dispatch is exact-length only",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    auto fixture = make_hash_major_small_fixture();
    const auto tagged_hash_major_head =
        decode_tagged_minimizer_generation_head(fixture.metadata.encoded_head);
    const auto tagged_hash_major_manifest =
        decode_tagged_minimizer_generation_manifest(fixture.metadata.encoded_manifest);
    REQUIRE(tagged_hash_major_head.has_value());
    REQUIRE(tagged_hash_major_head->index() == 1u);
    REQUIRE(tagged_hash_major_manifest.has_value());
    REQUIRE(tagged_hash_major_manifest->index() == 1u);

    const auto shape = derive_grouped_soa_minimizer_profile(
        k_minimap_scheme_identity, k_minimizer_persisted_dna4_alphabet_code, 15u, 8u, 10u);
    REQUIRE(shape.has_value());
    auto grouped_soa_head = golden_generation_head();
    grouped_soa_head.layout = minimizer_generation_layout::grouped_soa_segment_local;
    const auto encoded_grouped_soa_head = encode_minimizer_generation_head(grouped_soa_head);
    REQUIRE(encoded_grouped_soa_head.has_value());
    auto grouped_soa_manifest = golden_generation_manifest();
    grouped_soa_manifest.layout = minimizer_generation_layout::grouped_soa_segment_local;
    grouped_soa_manifest.alphabet_code = k_minimizer_persisted_dna4_alphabet_code;
    grouped_soa_manifest.kmer_size = 15u;
    grouped_soa_manifest.gpu_abi_revision = k_grouped_soa_gpu_abi_revision;
    assign_grouped_soa_profile(grouped_soa_manifest, *shape);
    const auto encoded_grouped_soa_manifest =
        encode_minimizer_generation_manifest(grouped_soa_manifest);
    REQUIRE(encoded_grouped_soa_manifest.has_value());
    const auto tagged_grouped_soa_head =
        decode_tagged_minimizer_generation_head(*encoded_grouped_soa_head);
    const auto tagged_grouped_soa_manifest =
        decode_tagged_minimizer_generation_manifest(*encoded_grouped_soa_manifest);
    REQUIRE(tagged_grouped_soa_head.has_value());
    REQUIRE(tagged_grouped_soa_head->index() == 0u);
    REQUIRE(tagged_grouped_soa_manifest.has_value());
    REQUIRE(tagged_grouped_soa_manifest->index() == 0u);

    for (const std::size_t wrong_size : {128u, 159u, 161u, 224u, 335u, 337u}) {
        INFO("wrong tagged manifest size " << wrong_size);
        std::vector<std::byte> bytes(wrong_size);
        std::copy_n(fixture.metadata.encoded_manifest.begin(),
                    std::min(wrong_size, fixture.metadata.encoded_manifest.size()),
                    bytes.begin());
        const auto decoded = decode_tagged_minimizer_generation_manifest(bytes);
        REQUIRE_FALSE(decoded.has_value());
        REQUIRE(decoded.error() == tagged_minimizer_generation_encoding_error::encoded_size);
    }
    for (const std::size_t wrong_size : {63u, 65u, 223u, 225u}) {
        INFO("wrong tagged head size " << wrong_size);
        std::vector<std::byte> bytes(wrong_size);
        std::copy_n(fixture.metadata.encoded_head.begin(),
                    std::min(wrong_size, fixture.metadata.encoded_head.size()),
                    bytes.begin());
        const auto decoded = decode_tagged_minimizer_generation_head(bytes);
        REQUIRE_FALSE(decoded.has_value());
        REQUIRE(decoded.error() == tagged_minimizer_generation_encoding_error::encoded_size);
    }

    std::array<std::byte, k_hash_major_minimizer_head_bytes> padded_grouped_soa_head{};
    std::copy(encoded_grouped_soa_head->begin(),
              encoded_grouped_soa_head->end(),
              padded_grouped_soa_head.begin());
    const auto rejected_padded_head =
        decode_tagged_minimizer_generation_head(padded_grouped_soa_head);
    REQUIRE_FALSE(rejected_padded_head.has_value());
    REQUIRE(rejected_padded_head.error() == tagged_minimizer_generation_encoding_error::hash_major);
    std::array<std::byte, k_hash_major_minimizer_manifest_bytes> padded_grouped_soa_manifest{};
    std::copy(encoded_grouped_soa_manifest->begin(),
              encoded_grouped_soa_manifest->end(),
              padded_grouped_soa_manifest.begin());
    const auto rejected_padded_manifest =
        decode_tagged_minimizer_generation_manifest(padded_grouped_soa_manifest);
    REQUIRE_FALSE(rejected_padded_manifest.has_value());
    REQUIRE(rejected_padded_manifest.error() ==
            tagged_minimizer_generation_encoding_error::hash_major);
}

TEST_CASE("hash-major selective reader requests only authorized postings and matches baseline",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    auto fixture = make_hash_major_small_fixture();
    const std::array<std::uint32_t, 3> queries{10u, 30u, 999u};
    auto reader = hash_major_minimizer_selective_reader::open(
        bioseq::parallelism::memory_admission_controller::instance(),
        fixture.metadata.encoded_head,
        fixture.metadata.encoded_manifest,
        hash_major_expected(fixture.options),
        fixture.image.source(),
        hash_major_query_view(queries, fixture.options.storage_profile),
        2u);
    REQUIRE(reader.has_value());
    REQUIRE(reader->target_length() == 100u);
    REQUIRE(fixture.image.posting_waves.size() == 1u);
    REQUIRE(fixture.image.posting_waves[0].size() == 2u);
    REQUIRE(fixture.image.posting_waves[0][0].control_block_ordinal == 0u);
    REQUIRE(fixture.image.posting_waves[0][0].posting_byte_offset == 0u);
    REQUIRE(fixture.image.posting_waves[0][0].byte_count == 8u);
    REQUIRE(fixture.image.posting_waves[0][1].control_block_ordinal == 0u);
    REQUIRE(fixture.image.posting_waves[0][1].posting_byte_offset == 12u);
    REQUIRE(fixture.image.posting_waves[0][1].byte_count == 8u);
    REQUIRE(fixture.image.control_waves.size() == 1u);
    REQUIRE(reader->posting_wave_peak_owner_bytes() <= k_hash_major_minimizer_read_slab_bytes);
    REQUIRE(reader->control_wave_peak_owner_bytes() <=
            k_hash_major_minimizer_control_read_wave_blocks *
                sizeof(hash_major_minimizer_authorized_control_range));
    REQUIRE(reader->transient_authority_retained_bytes() == 0u);
    const auto posting_reads = fixture.image.reads_of(hash_major_minimizer_stream_kind::postings);
    REQUIRE(posting_reads.size() == 2u);
    REQUIRE(posting_reads[0].offset == 0u);
    REQUIRE(posting_reads[0].byte_count == 8u);
    REQUIRE(posting_reads[1].offset == 12u);
    REQUIRE(posting_reads[1].byte_count == 8u);

    minimizer_hashmap_fragment_index baseline;
    auto baseline_pairs = fixture.pairs;
    baseline.build_from_pairs(std::span<std::pair<std::uint64_t, std::uint64_t>>{baseline_pairs},
                              100u);
    for (const std::uint64_t hash : std::array{10u, 30u, 999u}) {
        const auto spans = reader->lookup_spans(hash);
        REQUIRE(spans.positions_complete());
        REQUIRE(spans.well_formed());
        const auto expected = baseline.lookup(hash);
        REQUIRE(std::ranges::equal(spans.contiguous_positions32(), expected));
    }
    REQUIRE_THROWS_AS(reader->lookup_spans(20u), persisted_minimizer_lookup_error);

    SECTION("scalar logical-read fallback preserves the same authority") {
        auto scalar_fixture = make_hash_major_small_fixture();
        scalar_fixture.image.use_wave_callbacks = false;
        const auto scalar_reader = hash_major_minimizer_selective_reader::open(
            bioseq::parallelism::memory_admission_controller::instance(),
            scalar_fixture.metadata.encoded_head,
            scalar_fixture.metadata.encoded_manifest,
            hash_major_expected(scalar_fixture.options),
            scalar_fixture.image.source(),
            hash_major_query_view(queries, scalar_fixture.options.storage_profile),
            2u);
        REQUIRE(scalar_reader.has_value());
        REQUIRE(scalar_fixture.image.control_waves.empty());
        REQUIRE(scalar_fixture.image.posting_waves.empty());
        REQUIRE(scalar_reader->memory_stats().generic_source_fallback_calls > 0u);
        REQUIRE(std::ranges::equal(scalar_reader->lookup_spans(10u).contiguous_positions32(),
                                   std::array<std::uint32_t, 2>{2u, 4u}));
        REQUIRE(std::ranges::equal(scalar_reader->lookup_spans(30u).contiguous_positions32(),
                                   std::array<std::uint32_t, 2>{20u, 21u}));
    }
}

TEST_CASE("hash-major reader preserves a generated K20 u64 query profile",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t selected_hash = (std::uint64_t{1u} << 39u) | 0x35u;
    constexpr std::uint64_t other_hash = selected_hash + 9u;
    auto options = hash_major_test_options(100u);
    constexpr auto algorithm_identity =
        test_generation_algorithm_identity<seq::dna4, std::uint64_t>(
            minimizer_algorithm::simd_minimap2_reference,
            100u,
            20u,
            15u);
    const auto profile = hash_major_minimizer_profile_for_backend(
        minimizer_sketch_backend::exact_simd,
        algorithm_identity,
        k_minimizer_persisted_dna4_alphabet_code,
        options.target_length);
    REQUIRE(profile.has_value());
    REQUIRE(profile->hash_width_bytes == sizeof(std::uint64_t));
    REQUIRE(profile->kmer_size == 20u);
    REQUIRE(profile->window_size == 15u);
    options.storage_profile = *profile;

    hash_major_memory_image image;
    hash_major_minimizer_logical_encoder encoder{options, image.sink()};
    const std::array<std::uint64_t, 1> selected_positions{12u};
    const std::array<std::uint64_t, 1> other_positions{18u};
    REQUIRE(encoder.append_hash(selected_hash, selected_positions).has_value());
    REQUIRE(encoder.append_hash(other_hash, other_positions).has_value());
    const auto metadata = encoder.finish();
    REQUIRE(metadata.has_value());

    const std::array<std::uint32_t, 1> narrow_queries{0x35u};
    const auto rejected = hash_major_minimizer_selective_reader::open(
        bioseq::parallelism::memory_admission_controller::instance(),
        metadata->encoded_head,
        metadata->encoded_manifest,
        hash_major_expected(options),
        image.source(),
        hash_major_query_view(narrow_queries, options.storage_profile),
        10u);
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error() == hash_major_minimizer_error::storage_contract_mismatch);
    REQUIRE(image.reads.empty());

    const std::array<std::uint64_t, 1> queries{selected_hash};
    auto reader = hash_major_minimizer_selective_reader::open(
        bioseq::parallelism::memory_admission_controller::instance(),
        metadata->encoded_head,
        metadata->encoded_manifest,
        hash_major_expected(options),
        image.source(),
        hash_major_query_view(queries, options.storage_profile),
        10u);
    REQUIRE(reader.has_value());
    const auto selected = reader->lookup_spans(selected_hash);
    REQUIRE(selected.positions_complete());
    REQUIRE(selected.contiguous_positions32().size() == 1u);
    REQUIRE(selected.contiguous_positions32().front() == selected_positions.front());
    REQUIRE_THROWS_AS(reader->lookup_spans(other_hash), persisted_minimizer_lookup_error);

    auto invalid_profile = options.storage_profile;
    invalid_profile.hash_width_bytes = sizeof(std::uint32_t);
    REQUIRE_FALSE(is_valid_hash_major_minimizer_storage_profile(invalid_profile,
                                                                 options.target_length));
}

TEST_CASE("hash-major applies max_occ before posting IO with exact heavy count",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    hash_major_memory_image image;
    constexpr std::uint64_t count = 65'535u;
    const auto options = hash_major_test_options(70'000u);
    hash_major_minimizer_logical_encoder encoder{options, image.sink()};
    REQUIRE(encoder.begin_hash(42u, count).has_value());
    for (std::uint64_t bp = 0u; bp < count; ++bp) {
        const auto appended = encoder.append_posting(bp << 1u);
        if (!appended) {
            FAIL("hash-major streaming posting append failed at " << bp);
        }
    }
    REQUIRE(encoder.end_hash().has_value());
    auto metadata = encoder.finish();
    REQUIRE(metadata.has_value());
    const std::array<std::uint32_t, 1> query{42u};
    auto reader = hash_major_minimizer_selective_reader::open(
        bioseq::parallelism::memory_admission_controller::instance(),
        metadata->encoded_head,
        metadata->encoded_manifest,
        hash_major_expected(options),
        image.source(),
        hash_major_query_view(query, options.storage_profile),
        10u);
    REQUIRE(reader.has_value());
    const auto spans = reader->lookup_spans(42u);
    REQUIRE_FALSE(spans.positions_complete());
    REQUIRE(spans.global_occurrence_count() == count);
    REQUIRE(spans.well_formed());
    REQUIRE(reader->exact_count_exceptions().size() == 1u);
    REQUIRE(image.reads_of(hash_major_minimizer_stream_kind::postings).empty());
}

TEST_CASE("hash-major admission is exact at every pre-I/O barrier",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    const std::array<std::uint32_t, 3> queries{10u, 20u, 30u};
    hash_major_minimizer_memory_stats evidence{};
    {
        auto fixture = make_hash_major_small_fixture();
        bioseq::parallelism::memory_admission_controller admission;
        const auto reader = hash_major_minimizer_selective_reader::open(
            admission,
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            hash_major_expected(fixture.options),
            fixture.image.source(),
            hash_major_query_view(queries, fixture.options.storage_profile),
            2u);
        REQUIRE(reader.has_value());
        evidence = reader->memory_stats();
        REQUIRE(evidence.directory_stage_peak_admitted_bytes != 0u);
        REQUIRE(evidence.control_stage_peak_admitted_bytes != 0u);
        REQUIRE(evidence.posting_stage_peak_admitted_bytes != 0u);
        REQUIRE(evidence.peak_admitted_bytes == std::max({
                                                    evidence.directory_stage_peak_admitted_bytes,
                                                    evidence.control_stage_peak_admitted_bytes,
                                                    evidence.posting_stage_peak_admitted_bytes,
                                                }));
        REQUIRE(evidence.retained_owner_bytes == reader->admitted_bytes());
        REQUIRE(evidence.directory_stream_crc_receipts == 1u);
        REQUIRE(evidence.control_block_crc_receipts == evidence.selected_block_count);
        REQUIRE(evidence.control_semantic_receipts == evidence.selected_block_count);
        REQUIRE(evidence.posting_ranges_delivered == evidence.posting_semantic_receipts);
        REQUIRE(evidence.posting_range_crc_receipts <= evidence.posting_ranges_delivered);
        REQUIRE(evidence.generic_source_fallback_calls == 0u);
        REQUIRE(admission.reserved_bytes() == reader->admitted_bytes());
    }

    SECTION("directory peak minus one denies before source size or read") {
        auto fixture = make_hash_major_small_fixture();
        bioseq::parallelism::memory_admission_controller admission;
        admission.set_index_memory_cap_bytes(evidence.directory_stage_peak_admitted_bytes - 1u);
        const auto reader = hash_major_minimizer_selective_reader::open(
            admission,
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            hash_major_expected(fixture.options),
            fixture.image.source(),
            hash_major_query_view(queries, fixture.options.storage_profile),
            2u);
        REQUIRE_FALSE(reader.has_value());
        REQUIRE(reader.error() == hash_major_minimizer_error::admission);
        REQUIRE(fixture.image.size_calls == 0u);
        REQUIRE(fixture.image.reads.empty());
        REQUIRE(fixture.image.control_waves.empty());
        REQUIRE(fixture.image.posting_waves.empty());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("control peak minus one denies before a control callback") {
        auto fixture = make_hash_major_small_fixture();
        bioseq::parallelism::memory_admission_controller admission;
        admission.set_index_memory_cap_bytes(evidence.directory_stage_peak_admitted_bytes);
        fixture.image.cap_controller = &admission;
        fixture.image.cap_after_directory_read = evidence.control_stage_peak_admitted_bytes - 1u;
        const auto reader = hash_major_minimizer_selective_reader::open(
            admission,
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            hash_major_expected(fixture.options),
            fixture.image.source(),
            hash_major_query_view(queries, fixture.options.storage_profile),
            2u);
        REQUIRE_FALSE(reader.has_value());
        REQUIRE(reader.error() == hash_major_minimizer_error::admission);
        REQUIRE(fixture.image.size_calls == 3u);
        REQUIRE(fixture.image.reads_of(hash_major_minimizer_stream_kind::directory).size() == 1u);
        REQUIRE(fixture.image.control_waves.empty());
        REQUIRE(fixture.image.posting_waves.empty());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("posting peak minus one denies before a posting callback") {
        auto fixture = make_hash_major_small_fixture();
        bioseq::parallelism::memory_admission_controller admission;
        admission.set_index_memory_cap_bytes(evidence.peak_admitted_bytes);
        fixture.image.cap_controller = &admission;
        fixture.image.cap_after_control_wave = evidence.posting_stage_peak_admitted_bytes - 1u;
        const auto reader = hash_major_minimizer_selective_reader::open(
            admission,
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            hash_major_expected(fixture.options),
            fixture.image.source(),
            hash_major_query_view(queries, fixture.options.storage_profile),
            2u);
        REQUIRE_FALSE(reader.has_value());
        REQUIRE(reader.error() == hash_major_minimizer_error::admission);
        REQUIRE_FALSE(fixture.image.control_waves.empty());
        REQUIRE(fixture.image.reads_of(hash_major_minimizer_stream_kind::postings).empty());
        REQUIRE(fixture.image.posting_waves.empty());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("move construction transfers the sole retained permit") {
        auto fixture = make_hash_major_small_fixture();
        bioseq::parallelism::memory_admission_controller admission;
        auto made = hash_major_minimizer_selective_reader::open(
            admission,
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            hash_major_expected(fixture.options),
            fixture.image.source(),
            hash_major_query_view(queries, fixture.options.storage_profile),
            2u);
        REQUIRE(made.has_value());
        const std::uint64_t retained_bytes = made->admitted_bytes();
        hash_major_minimizer_selective_reader moved{std::move(*made)};
        REQUIRE(made->admitted_bytes() == 0u);
        REQUIRE(made->memory_stats().retained_owner_bytes == 0u);
        REQUIRE(moved.admitted_bytes() == retained_bytes);
        REQUIRE(admission.reserved_bytes() == retained_bytes);
        REQUIRE(std::ranges::equal(moved.lookup_spans(10u).contiguous_positions32(),
                                   std::array<std::uint32_t, 2>{2u, 4u}));
    }
}

TEST_CASE("hash-major authenticates every caller expectation before stream IO",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    auto fixture = make_hash_major_small_fixture();
    const std::array<std::uint32_t, 1> query{10u};
    std::vector<expected_hash_major_minimizer_generation> mismatches;

    auto mismatch = hash_major_expected(fixture.options);
    ++mismatch.generation_id;
    mismatches.push_back(mismatch);
    mismatch = hash_major_expected(fixture.options);
    ++mismatch.query_key;
    mismatches.push_back(mismatch);
    mismatch = hash_major_expected(fixture.options);
    ++mismatch.source_epoch;
    mismatches.push_back(mismatch);
    mismatch = hash_major_expected(fixture.options);
    mismatch.source_content_crc64 ^= 1u;
    mismatches.push_back(mismatch);
    mismatch = hash_major_expected(fixture.options);
    ++mismatch.target_length;
    mismatches.push_back(mismatch);
    mismatch = hash_major_expected(fixture.options);
    mismatch.storage_profile.producer_backend_identity = 2u;
    mismatches.push_back(mismatch);

    for (std::size_t index = 0u; index < mismatches.size(); ++index) {
        INFO("expected-generation mismatch ordinal " << index);
        const auto reader = hash_major_minimizer_selective_reader::open(
            bioseq::parallelism::memory_admission_controller::instance(),
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            mismatches[index],
            fixture.image.source(),
            hash_major_query_view(query, fixture.options.storage_profile),
            2u);
        REQUIRE_FALSE(reader.has_value());
        REQUIRE(reader.error() == hash_major_minimizer_error::expected_generation);
        REQUIRE(fixture.image.reads.empty());
        REQUIRE(fixture.image.control_waves.empty());
        REQUIRE(fixture.image.posting_waves.empty());
    }
}

TEST_CASE("hash-major authenticates fully selected blocks and whole streams",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    const std::array<std::uint32_t, 3> all_queries{10u, 20u, 30u};

    SECTION("full selected posting block CRC") {
        auto fixture = make_hash_major_small_fixture();
        auto& postings = fixture.image.streams[hash_major_memory_image::index(
            hash_major_minimizer_stream_kind::postings)];
        test_write_le<std::uint32_t>(std::span<std::byte>{postings}, 4u, 6u);
        const auto reader = hash_major_minimizer_selective_reader::open(
            bioseq::parallelism::memory_admission_controller::instance(),
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            hash_major_expected(fixture.options),
            fixture.image.source(),
            hash_major_query_view(all_queries, fixture.options.storage_profile),
            2u);
        REQUIRE_FALSE(reader.has_value());
        REQUIRE(reader.error() == hash_major_minimizer_error::checksum);
    }

    for (const std::size_t stream_index : {1u, 2u}) {
        INFO("whole-stream CRC descriptor ordinal " << stream_index);
        auto fixture = make_hash_major_small_fixture();
        auto manifest = fixture.metadata.manifest;
        manifest.streams[stream_index].stream_crc64 ^= 1u;
        const auto encoded_manifest = encode_hash_major_minimizer_manifest(manifest);
        REQUIRE(encoded_manifest.has_value());
        auto head = fixture.metadata.head;
        head.manifest_payload_crc64 = crc64_ecma182(*encoded_manifest);
        head.manifest_address_bound_row_crc64 =
            crc64_index_chunk_row(head.generation_id,
                                  0,
                                  0,
                                  0u,
                                  static_cast<std::uint32_t>(encoded_manifest->size()),
                                  *encoded_manifest);
        const auto encoded_head = encode_hash_major_minimizer_head(head);
        REQUIRE(encoded_head.has_value());
        const auto reader = hash_major_minimizer_selective_reader::open(
            bioseq::parallelism::memory_admission_controller::instance(),
            *encoded_head,
            *encoded_manifest,
            hash_major_expected(fixture.options),
            fixture.image.source(),
            hash_major_query_view(all_queries, fixture.options.storage_profile),
            2u);
        REQUIRE_FALSE(reader.has_value());
        REQUIRE(reader.error() == hash_major_minimizer_error::checksum);
    }
}

TEST_CASE("hash-major selected corruption fails while unselected bytes stay unread",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    SECTION("selected duplicate position fails semantic validation") {
        auto fixture = make_hash_major_small_fixture();
        auto& postings = fixture.image.streams[hash_major_memory_image::index(
            hash_major_minimizer_stream_kind::postings)];
        std::copy_n(postings.begin(), 4u, postings.begin() + 4u);
        const std::array<std::uint32_t, 1> query{10u};
        auto reader = hash_major_minimizer_selective_reader::open(
            bioseq::parallelism::memory_admission_controller::instance(),
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            hash_major_expected(fixture.options),
            fixture.image.source(),
            hash_major_query_view(query, fixture.options.storage_profile),
            2u);
        REQUIRE_FALSE(reader.has_value());
        REQUIRE(reader.error() == hash_major_minimizer_error::order);
    }
    SECTION("corrupt non-query posting is not fetched") {
        auto fixture = make_hash_major_small_fixture();
        auto& postings = fixture.image.streams[hash_major_memory_image::index(
            hash_major_minimizer_stream_kind::postings)];
        postings[8] = std::byte{0xff};
        postings[9] = std::byte{0xff};
        postings[10] = std::byte{0xff};
        postings[11] = std::byte{0xff};
        const std::array<std::uint32_t, 2> query{10u, 30u};
        auto reader = hash_major_minimizer_selective_reader::open(
            bioseq::parallelism::memory_admission_controller::instance(),
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            hash_major_expected(fixture.options),
            fixture.image.source(),
            hash_major_query_view(query, fixture.options.storage_profile),
            2u);
        REQUIRE(reader.has_value());
        for (const auto& read :
             fixture.image.reads_of(hash_major_minimizer_stream_kind::postings)) {
            REQUIRE((read.offset + read.byte_count <= 8u || read.offset >= 12u));
        }
    }
    SECTION("empty query reads directory but no controls or postings") {
        auto fixture = make_hash_major_small_fixture();
        const std::array<std::uint32_t, 0> query{};
        auto reader = hash_major_minimizer_selective_reader::open(
            bioseq::parallelism::memory_admission_controller::instance(),
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            hash_major_expected(fixture.options),
            fixture.image.source(),
            hash_major_query_view(query, fixture.options.storage_profile),
            2u);
        REQUIRE(reader.has_value());
        REQUIRE(fixture.image.reads_of(hash_major_minimizer_stream_kind::controls).empty());
        REQUIRE(fixture.image.reads_of(hash_major_minimizer_stream_kind::postings).empty());
    }
}

TEST_CASE("hash-major widens authorized u32 and preserves required u64 postings",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    const std::uint64_t target_length = (std::uint64_t{1} << 31u) + 10u;
    hash_major_memory_image image;
    const auto options = hash_major_test_options(target_length);
    hash_major_minimizer_logical_encoder encoder{options, image.sink()};
    const std::array<std::uint64_t, 2> positions{
        ((std::uint64_t{1} << 31u) << 1u),
        (((std::uint64_t{1} << 31u) + 1u) << 1u) | 1u,
    };
    REQUIRE(encoder.append_hash(7u, positions).has_value());
    auto metadata = encoder.finish();
    REQUIRE(metadata.has_value());
    REQUIRE(metadata->manifest.storage_profile.stored_posting_width_bytes == 8u);
    const std::array<std::uint32_t, 1> query{7u};
    auto reader = hash_major_minimizer_selective_reader::open(
        bioseq::parallelism::memory_admission_controller::instance(),
        metadata->encoded_head,
        metadata->encoded_manifest,
        hash_major_expected(options),
        image.source(),
        hash_major_query_view(query, options.storage_profile),
        2u);
    REQUIRE(reader.has_value());
    REQUIRE(std::ranges::equal(reader->lookup_spans(7u).contiguous_positions(), positions));
}

TEST_CASE("hash-major posting-width and target-length boundaries are exact",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;

    SECTION("2^31 target retains the largest legal u32 posting") {
        constexpr std::uint64_t target_length = std::uint64_t{1} << 31u;
        hash_major_memory_image image;
        const auto options = hash_major_test_options(target_length);
        REQUIRE(options.storage_profile.stored_posting_width_bytes == 4u);
        hash_major_minimizer_logical_encoder encoder{options, image.sink()};
        const std::array<std::uint64_t, 1> positions{(std::numeric_limits<std::uint32_t>::max)()};
        REQUIRE(encoder.append_hash(7u, positions).has_value());
        const auto metadata = encoder.finish();
        REQUIRE(metadata.has_value());
        const std::array<std::uint32_t, 1> query{7u};
        const auto reader = hash_major_minimizer_selective_reader::open(
            bioseq::parallelism::memory_admission_controller::instance(),
            metadata->encoded_head,
            metadata->encoded_manifest,
            hash_major_expected(options),
            image.source(),
            hash_major_query_view(query, options.storage_profile),
            1u);
        REQUIRE(reader.has_value());
        const auto retained = reader->lookup_spans(7u).contiguous_positions32();
        REQUIRE(retained.size() == 1u);
        REQUIRE(retained.front() == positions.front());
    }

    SECTION("2^31+1 target requires u64") {
        constexpr std::uint64_t target_length = (std::uint64_t{1} << 31u) + 1u;
        const auto options = hash_major_test_options(target_length);
        REQUIRE(options.storage_profile.stored_posting_width_bytes == 8u);
        REQUIRE(
            is_valid_hash_major_minimizer_storage_profile(options.storage_profile, target_length));
    }

    SECTION("2^63 target retains UINT64_MAX and rejects the next length") {
        constexpr std::uint64_t target_length = std::uint64_t{1} << 63u;
        hash_major_memory_image image;
        const auto options = hash_major_test_options(target_length);
        hash_major_minimizer_logical_encoder encoder{options, image.sink()};
        const std::array<std::uint64_t, 1> positions{(std::numeric_limits<std::uint64_t>::max)()};
        REQUIRE(encoder.append_hash(7u, positions).has_value());
        const auto metadata = encoder.finish();
        REQUIRE(metadata.has_value());
        const std::array<std::uint32_t, 1> query{7u};
        const auto reader = hash_major_minimizer_selective_reader::open(
            bioseq::parallelism::memory_admission_controller::instance(),
            metadata->encoded_head,
            metadata->encoded_manifest,
            hash_major_expected(options),
            image.source(),
            hash_major_query_view(query, options.storage_profile),
            1u);
        REQUIRE(reader.has_value());
        const auto retained = reader->lookup_spans(7u).contiguous_positions();
        REQUIRE(retained.size() == 1u);
        REQUIRE(retained.front() == positions.front());

        constexpr std::uint64_t invalid_target = target_length + 1u;
        const auto invalid_options = hash_major_test_options(invalid_target);
        REQUIRE_FALSE(is_valid_hash_major_minimizer_storage_profile(invalid_options.storage_profile,
                                                                    invalid_target));
        hash_major_memory_image rejected_image;
        hash_major_minimizer_logical_encoder rejected_encoder{invalid_options,
                                                              rejected_image.sink()};
        REQUIRE_FALSE(rejected_encoder.begin_hash(7u, 1u).has_value());

        auto invalid_manifest = metadata->encoded_manifest;
        test_write_le<std::uint64_t>(invalid_manifest, 56u, invalid_target);
        const auto decoded = decode_hash_major_minimizer_manifest(invalid_manifest);
        REQUIRE_FALSE(decoded.has_value());
        REQUIRE(decoded.error() == hash_major_minimizer_error::stream_descriptor);
    }
}

TEST_CASE("hash-major row ordinals and bounded waves span physical rows exactly",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    constexpr std::uint32_t hash_count = k_hash_major_minimizer_controls_per_block + 1u;
    hash_major_memory_image image;
    const auto options = hash_major_test_options(5'000u);
    hash_major_minimizer_logical_encoder encoder{options, image.sink()};
    for (std::uint64_t hash = 1u; hash <= hash_count; ++hash) {
        const std::array<std::uint64_t, 1> position{(hash % options.target_length) << 1u};
        REQUIRE(encoder.append_hash(hash, position).has_value());
    }
    const auto metadata = encoder.finish();
    REQUIRE(metadata.has_value());
    REQUIRE(metadata->manifest.directory_entry_count == 2u);
    REQUIRE(image.append_order == std::vector<hash_major_minimizer_stream_kind>{
                                      hash_major_minimizer_stream_kind::postings,
                                      hash_major_minimizer_stream_kind::postings,
                                      hash_major_minimizer_stream_kind::postings,
                                      hash_major_minimizer_stream_kind::controls,
                                      hash_major_minimizer_stream_kind::directory,
                                      hash_major_minimizer_stream_kind::postings,
                                      hash_major_minimizer_stream_kind::controls,
                                      hash_major_minimizer_stream_kind::directory});
    REQUIRE(metadata->manifest.streams[0].row_count == 1u);
    REQUIRE(metadata->manifest.streams[0].first_row_ordinal == 0u);
    REQUIRE(metadata->manifest.streams[1].row_count == 2u);
    REQUIRE(metadata->manifest.streams[1].first_row_ordinal == 1u);
    REQUIRE(metadata->manifest.streams[2].row_count == 3u);
    REQUIRE(metadata->manifest.streams[2].first_row_ordinal == 3u);

    const std::array<std::uint32_t, 3> queries{
        1u, k_hash_major_minimizer_controls_per_block, hash_count};
    const auto reader = hash_major_minimizer_selective_reader::open(
        bioseq::parallelism::memory_admission_controller::instance(),
        metadata->encoded_head,
        metadata->encoded_manifest,
        hash_major_expected(options),
        image.source(),
        hash_major_query_view(queries, options.storage_profile),
        1u);
    REQUIRE(reader.has_value());
    REQUIRE(image.control_waves.size() == 1u);
    REQUIRE(image.control_waves.front().size() == 2u);
    REQUIRE(reader->posting_wave_peak_owner_bytes() <= k_hash_major_minimizer_read_slab_bytes);
    REQUIRE(reader->transient_authority_retained_bytes() == 0u);

    const auto& directory_bytes =
        image.streams[hash_major_memory_image::index(hash_major_minimizer_stream_kind::directory)];
    for (const auto& wave : image.posting_waves) {
        for (const auto& range : wave) {
            REQUIRE(range.control_block_ordinal < 2u);
            const auto entry = decode_hash_major_minimizer_directory_entry(
                std::span<const std::byte>{directory_bytes}.subspan(
                    static_cast<std::size_t>(range.control_block_ordinal) *
                        k_hash_major_minimizer_directory_entry_bytes,
                    k_hash_major_minimizer_directory_entry_bytes));
            REQUIRE(entry.has_value());
            REQUIRE(range.posting_byte_offset >= entry->posting_byte_offset);
            REQUIRE(range.byte_count <= entry->posting_byte_offset + entry->posting_byte_length -
                                            range.posting_byte_offset);
        }
    }
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
