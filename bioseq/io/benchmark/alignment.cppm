/** @file Matched SAM/BAM IO comparison against the pinned samtools HTSlib. */
module;
#include <hwy/highway.h>
#include <libassert/assert.hpp>
#include <new>
#include <unistd.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <mach/vm_page_size.h>
#endif
// Pinned HTSlib plugin ABI: benchmark-only bounded memory transport.
#include <hfile_internal.h>
#include <htslib/bgzf.h>
#include <htslib/sam.h>
#include <htslib/hts.h>
#include <simdjson.h>

module bioseq.io.benchmark:alignment;
import std;
import :common;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.instrumentation;
import bioseq.io.sinks;
import bioseq.io.compression.aligned_byte_buffer;
import bioseq.parallelism.memory_admission;
import bioseq.common.seqan_adapter;
import bioseq.gpu.vulkan.shader_cache;
import bioseq.sequences;
import bioseq.sequences.cigar;
import bioseq.simd.utils;
import bioseq.io.record_schema;
import bioseq.io.reader;
import bioseq.io.write_pipeline;
import bioseq.io.alignment;
import bioseq.io.identifier_parser.preservation;
import bioseq.storage.lazy_sequence_view;
import bioseq.storage.size;
import bioseq.parallelism.pool;
import bioseq.bench.framework;

namespace bioseq::io::benchmark::detail {

/** File identity is sampled without reading the fixture's contents. */
struct alignment_file_identity {
    std::uint64_t device{}, inode{}, bytes{};
    std::int64_t modified_seconds{}, modified_nanoseconds{}, changed_seconds{}, changed_nanoseconds{};
    bool operator==(const alignment_file_identity&) const = default;
    /** Require a regular file and retain timestamps at their native precision. */
    [[nodiscard]] static std::expected<alignment_file_identity, std::string> from_descriptor(int descriptor) {
        struct stat status{};
        if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0)
            return std::unexpected("alignment input identity is unavailable or not a regular file");
#ifdef __APPLE__
        const auto modified = status.st_mtimespec, changed = status.st_ctimespec;
#else
        const auto modified = status.st_mtim, changed = status.st_ctim;
#endif
        return alignment_file_identity{static_cast<std::uint64_t>(status.st_dev),
            static_cast<std::uint64_t>(status.st_ino), static_cast<std::uint64_t>(status.st_size),
            modified.tv_sec, modified.tv_nsec, changed.tv_sec, changed.tv_nsec};
    }
    /** Open without following replacement symlinks and close before returning. */
    [[nodiscard]] static std::expected<alignment_file_identity, std::string> open(const std::string& path) {
        const auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (descriptor < 0) return std::unexpected("cannot open alignment input identity");
        auto identity = from_descriptor(descriptor);
        if (::close(descriptor) != 0) return std::unexpected("cannot close alignment input identity");
        return identity;
    }
};

/** Sum the full validated BGZF stream, including BAM header bytes, without decoding.
 * The caller has already traversed the file with the pinned CLI, validating
 * payload CRC/ISIZE. This framing-only walk borrows at most64MiB plus page
 * alignment, retains no per-member metadata and checks the file identity twice.
 */
[[nodiscard]] std::expected<std::uint64_t, std::string> alignment_uncompressed_bam_bytes(
        const std::string& path, const alignment_file_identity& expected) {
    struct descriptor_owner {
        int descriptor{-1};
        ~descriptor_owner() { if (descriptor >= 0) ::close(descriptor); }
    } input{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (input.descriptor < 0) return std::unexpected("cannot open validated BAM byte count");
    const auto before = alignment_file_identity::from_descriptor(input.descriptor);
    if (!before || *before != expected)
        return std::unexpected("validated BAM identity changed before byte count");
    constexpr std::size_t maximum_window = 64u << 20u;
    static_assert(maximum_window >= alignment::bgzf_member_limit);
    std::uint64_t offset{}, total{};
    while (offset < expected.bytes) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(maximum_window, expected.bytes - offset));
        auto mapped = mapped_read_window::open(input.descriptor, offset, count);
        if (!mapped) return std::unexpected(mapped.error());
        const std::span<const std::uint8_t> bytes{
            reinterpret_cast<const std::uint8_t*>(mapped->bytes().data()), mapped->bytes().size()};
        std::size_t consumed{};
        while (consumed < bytes.size()) {
            const auto available = bytes.size() - consumed;
            // A final partial member may cross this mapping boundary. Remap
            // from its start before parsing; only actual file EOF may truncate.
            if (available < alignment::bgzf_member_limit && expected.bytes - offset - consumed > available) break;
            const auto member = alignment::inspect_bgzf_member(bytes.subspan(consumed), offset + consumed);
            if (!member) return std::unexpected(member.error());
            if (member->member.empty() || member->member.size() > available)
                return std::unexpected("validated BAM framing made invalid progress");
            const auto next = common::checked_add(total, static_cast<std::uint64_t>(member->uncompressed_bytes));
            if (!next) return std::unexpected("validated BAM uncompressed byte count overflows");
            total = *next;
            consumed += member->member.size();
        }
        if (consumed == 0u) return std::unexpected("validated BAM byte count made no progress");
        offset += consumed;
    }
    const auto after = alignment_file_identity::from_descriptor(input.descriptor);
    const auto named = alignment_file_identity::open(path);
    if (!after || !named || *after != expected || *named != expected)
        return std::unexpected("validated BAM identity changed during byte count");
    return total;
}

/** Distinguish cache requests from observed residency; failed preparation cannot qualify. */
struct alignment_cache_observation {
    bool requested{}, invalidated{}, observed{}, cold{};
    std::uint64_t pages{}, resident_pages{}, bytes{}, resident_bytes{}, request_ns{}, observation_ns{};
    std::int64_t observed_at_ns{};
    int request_errno{}, observation_errno{};
    std::string error;
};

/** One bounded invalidation pass followed by a separate complete residency observation. */
[[nodiscard]] alignment_cache_observation prepare_alignment_cold_input(
        const std::string& path, const alignment_file_identity& expected) {
    alignment_cache_observation result;
    const auto failure = [&result](std::string message) {
        result.error = std::move(message);
        return result;
    };
#ifndef __APPLE__
    (void)path; (void)expected;
    return failure("observed-cold alignment input is unsupported on this platform");
#else
    constexpr std::size_t maximum_window = 64u << 20u;
    constexpr std::size_t maximum_bitmap = 64u << 10u;
    const auto page_query = ::sysconf(_SC_PAGESIZE);
    if (page_query <= 0 || static_cast<std::uint64_t>(page_query) != vm_page_size
        || vm_page_size != vm_kernel_page_size || !std::has_single_bit(static_cast<std::size_t>(page_query))
        || static_cast<std::size_t>(page_query) > maximum_window)
        return failure("observed-cold input has unsupported page geometry");
    const auto page_bytes = static_cast<std::size_t>(page_query);
    const auto bitmap_window = common::checked_mul(page_bytes, maximum_bitmap);
    if (!bitmap_window) return failure("cold input bitmap geometry overflows");
    const auto window_bytes = std::min(maximum_window, *bitmap_window);
    std::array<char, maximum_bitmap> residency;
    struct descriptor_owner {
        int descriptor{-1};
        ~descriptor_owner() { if (descriptor >= 0) ::close(descriptor); }
    } input{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (input.descriptor < 0) return failure("cannot open cold alignment input");
    const auto before = alignment_file_identity::from_descriptor(input.descriptor);
    if (!before || *before != expected) return failure("cold input identity changed before preparation");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{60};
    const auto request_begin = bioseq::instrumentation::clock::now_ns();
    result.requested = true;
    for (std::uint64_t offset{}; offset < expected.bytes;) {
        if (std::chrono::steady_clock::now() > deadline) return failure("cold input preparation deadline exceeded");
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(window_bytes, expected.bytes - offset));
        auto mapped = mapped_read_window::open(input.descriptor, offset, count);
        if (!mapped) return failure(mapped.error());
        // This PROT_READ mapping is never dereferenced or COW-modified. Success
        // is only an invalidation request; mincore below supplies the evidence.
        if (::msync(const_cast<std::byte*>(mapped->bytes().data()), count, MS_SYNC | MS_INVALIDATE) != 0) {
            result.request_errno = errno;
            bioseq_log<logging_level::error>("cold invalidation failed: path={} offset={} errno={}", path, offset, result.request_errno);
            return failure("cold input invalidation request failed");
        }
        offset += count;
    }
    result.request_ns = static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns() - request_begin);
    result.invalidated = true;
    const auto observation_begin = bioseq::instrumentation::clock::now_ns();
    for (std::uint64_t offset{}; offset < expected.bytes;) {
        if (std::chrono::steady_clock::now() > deadline) return failure("cold input observation deadline exceeded");
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(window_bytes, expected.bytes - offset));
        auto mapped = mapped_read_window::open(input.descriptor, offset, count);
        if (!mapped) return failure(mapped.error());
        const auto page_count = count / page_bytes + static_cast<std::size_t>(count % page_bytes != 0u);
        std::fill_n(residency.begin(), page_count, static_cast<char>(0xff));
        if (::mincore(reinterpret_cast<caddr_t>(const_cast<std::byte*>(mapped->bytes().data())), count, residency.data()) != 0) {
            result.observation_errno = errno;
            bioseq_log<logging_level::error>("cold residency observation failed: path={} offset={} errno={}", path, offset, result.observation_errno);
            return failure("cold input residency observation failed");
        }
        for (std::size_t page{}; page < page_count; ++page) {
            if ((static_cast<unsigned char>(residency[page]) & MINCORE_INCORE) != 0u) {
                ++result.resident_pages;
                result.resident_bytes += std::min(page_bytes, count - page * page_bytes);
            }
        }
        result.pages += page_count;
        result.bytes += count;
        offset += count;
    }
    result.observed_at_ns = bioseq::instrumentation::clock::now_ns();
    result.observation_ns = static_cast<std::uint64_t>(result.observed_at_ns - observation_begin);
    result.observed = result.bytes == expected.bytes;
    result.cold = result.observed && result.resident_pages == 0u;
    const auto after = alignment_file_identity::from_descriptor(input.descriptor);
    const auto named = alignment_file_identity::open(path);
    if (!after || !named || *after != expected || *named != expected)
        return failure("cold input identity changed during preparation");
    return result;
#endif
}

/** Retained original and native representations prepared outside measured writer runs. */
struct alignment_fixture {
    std::string alphabet;
    std::size_t cardinality{};
    std::size_t native_text_capacity{};
    alignment_file_identity sam_identity, bam_identity;
    std::uint64_t bam_uncompressed_bytes{}, native_sequence_symbols{};
    int compression_level{6};
    std::string sam_path, bam_path, output_path;
    std::vector<std::unique_ptr<decoded_record>> original;
    std::vector<bam1_t*> native;
    std::vector<std::string> original_encoding, native_encoding;
    sam_hdr_t* header{};
    record_source_metadata metadata;
    bool valid{};
    /** Release the baseline library's owned records and header outside measured loops. */
    ~alignment_fixture() {
        for (auto* record : native) bam_destroy1(record);
        if (header) sam_hdr_destroy(header);
    }
    alignment_fixture() = default;
    alignment_fixture(const alignment_fixture&) = delete;
};

/** Fixture-only owned encoding, materialized before the measured operation. */
struct alignment_comparison_sink {
    std::string bytes;
    [[nodiscard]] std::expected<void, std::string> append(std::string_view text) {
        bytes.append(text);
        return {};
    }
};

/** Compare emitted canonical chunks without retaining another record-sized copy. */
struct alignment_observation_sink {
    bioseq::bench::framework::ordered_output_observation& observation;
    [[nodiscard]] std::expected<void, std::string> append(std::string_view text) {
        if (!observation.append(text)) return std::unexpected("measured original record differs");
        return {};
    }
};

/** HTSlib's formatter reuses one bounded record buffer for measured consumption. */
struct alignment_native_text {
    kstring_t value{};
    ~alignment_native_text() { std::free(value.s); }
};

/** Immutable native fields borrowed from one retained little-endian BAM record. */
struct alignment_codec_row {
    alignment::bam_record_view native;
    alignment::cigar_wire_measurement cigar;
    alignment::bam_record_measurement measured;
    std::vector<auxiliary_field_view> auxiliary;
    std::size_t auxiliary_count{};
};

/** Iteration-local expectations disappear before any full IO iteration begins. */
struct alignment_codec_fixture {
    parallelism::memory_permit permit;
    std::vector<std::string> bam_encoding;
    std::vector<alignment_codec_row> rows;
    std::size_t maximum_bam_bytes{};
};

/** Pinned HTSlib backend state borrows one stable, admitted output span. */
struct alignment_codec_backend {
    alignment::alignment_window_sink destination;
    bool failed{};
    bool discarding{};
    /** Copy a complete fragment or latch failure without throwing through C. */
    [[nodiscard]] static ssize_t write(hFILE* file, const void* source, std::size_t count) noexcept {
        alignment_codec_backend* state{};
        std::memcpy(&state, reinterpret_cast<const std::byte*>(file) + sizeof(hFILE), sizeof(state));
        if (count > static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())) {
            state->failed = true; errno = EOVERFLOW; return -1;
        }
        if (state->discarding) return static_cast<ssize_t>(count);
        auto& output = state->destination;
        if (output.written > output.destination.size() || count > output.destination.size() - output.written) {
            state->failed = true; errno = ENOSPC; return -1;
        }
        if (count != 0u) std::memcpy(output.destination.data() + output.written, source, count);
        output.written += count;
        return static_cast<ssize_t>(count);
    }
    /** No read/seek is part of the memory-only encoder contract. */
    [[nodiscard]] static ssize_t read(hFILE*, void*, std::size_t) noexcept { errno = ENOTSUP; return -1; }
    [[nodiscard]] static off_t seek(hFILE*, off_t, int) noexcept { errno = ENOTSUP; return -1; }
    /** Memory destinations need no persistence action and remain caller-owned. */
    [[nodiscard]] static int flush(hFILE*) noexcept { return 0; }
    [[nodiscard]] static int close(hFILE*) noexcept { return 0; }
    /** Define the existing vendor callback table only after its callbacks. */
    [[nodiscard]] static const hFILE_backend& methods() noexcept {
        static const hFILE_backend value{&read, &write, &seek, &flush, &close};
        return value;
    }
};

/** One stable scratch owner per partition; allocations precede the measured window. */
struct alignment_codec_scratch {
    parallelism::memory_permit permit;
    compression::aligned_buffer<std::uint8_t> bytes;
    alignment_codec_backend backend;
    kstring_t text{};
    std::size_t text_capacity{};
    hFILE* file{};
    BGZF* stream{};
    alignment_codec_scratch() = default;
    alignment_codec_scratch(const alignment_codec_scratch&) = delete;
    alignment_codec_scratch& operator=(const alignment_codec_scratch&) = delete;
    /** Close once, including failed writes, without retrying an already freed hFILE. */
    [[nodiscard]] bool finish() noexcept {
        if (stream == nullptr) return !backend.failed;
        const bool success = !backend.failed && herrno(file) == 0;
        // hclose frees hFILE on error, while pinned bgzf_close returns before
        // freeing BGZF then. Drain failed bytes into a bounded discard callback
        // and clear only that transport error; the sample failure stays latched.
        backend.discarding = true;
        hclearerr(file);
        auto* closing = std::exchange(stream, nullptr);
        file = nullptr;
        return bgzf_close(closing) == 0 && success;
    }
    ~alignment_codec_scratch() { (void)finish(); std::free(text.s); }
    /** Admit equal prepared capacities; no scratch growth is allowed during encoding. */
    [[nodiscard]] std::expected<void, std::string> prepare(std::size_t capacity, bool bam) {
        capacity = std::max(capacity, std::size_t{1u});
        constexpr std::size_t transport_bytes = 128u << 10u;
        const auto arrays = common::checked_mul(capacity, std::size_t{4u});
        const auto charge = arrays ? common::checked_add(*arrays,
            sizeof(alignment_codec_scratch) + transport_bytes + 8192u) : std::nullopt;
        if (!charge) return std::unexpected("codec scratch charge overflows");
        auto admitted = reserve_io_memory(*charge);
        if (!admitted) return std::unexpected("codec scratch admission refused");
        permit = std::move(*admitted);
        if (!bam) {
            if (ks_resize(&text, capacity) < 0) return std::unexpected("codec SAM scratch allocation failed");
            text_capacity = text.m;
            capacity = text_capacity;
        }
        const auto allocated = bytes.extend(capacity);
        if (!allocated) return std::unexpected(allocated.error());
        backend.destination = {*allocated};
        if (!bam) return {};
        // The pinned plugin ABI is confined to this benchmark module. Store a
        // trivially copied pointer in trailing vendor-allocated bytes, avoiding
        // assumptions about a C++ derived hFILE object's lifetime or layout.
        file = hfile_init(sizeof(hFILE) + sizeof(alignment_codec_backend*), "w", transport_bytes);
        if (file == nullptr) return std::unexpected("codec hFILE allocation failed");
        // Pinned hfile_init's POSIX allocation branch can return a descriptor
        // with a null buffer after a positive posix_memalign error code.
        if (file->buffer == nullptr) {
            hfile_destroy(file); file = nullptr;
            return std::unexpected("codec hFILE buffer allocation failed");
        }
        file->backend = &alignment_codec_backend::methods();
        auto* state = &backend;
        std::memcpy(reinterpret_cast<std::byte*>(file) + sizeof(hFILE), &state, sizeof(state));
        stream = bgzf_hopen(file, "wu");
        if (stream == nullptr) {
            const auto closed = hclose(file); file = nullptr;
            return std::unexpected(closed == 0 ? "codec BGZF allocation failed" : "codec hFILE cleanup failed");
        }
        return {};
    }
    /** Begin another row only after the previous row was fully flushed/observed. */
    void reset() noexcept { backend.destination.written = 0u; text.l = 0u; }
    /** Reuse the same plain-stream transport on both BAM implementations. */
    [[nodiscard]] std::expected<void, std::string> append(std::span<const std::uint8_t> source) {
        if (source.empty()) return {};
        if (source.size() > static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())) {
            backend.failed = true; return std::unexpected("codec BAM append length overflows");
        }
        if (bgzf_write_small(stream, source.data(), source.size()) != static_cast<ssize_t>(source.size())) {
            backend.failed = true; return std::unexpected("codec BAM append failed");
        }
        return {};
    }
    /** Make the entire row visible to the common once-per-row observation. */
    [[nodiscard]] std::expected<void, std::string> flush() {
        if (bgzf_flush(stream) != 0 || backend.failed) {
            backend.failed = true; return std::unexpected("codec BAM flush failed");
        }
        return {};
    }
    /** Borrow only the bytes actually written, never unused capacity. */
    [[nodiscard]] std::string_view output() const noexcept {
        return {reinterpret_cast<const char*>(bytes.data.get()), backend.destination.written};
    }
};

/** Untimed existing-owner acceptance covers transport crossings and failure cleanup. */
[[nodiscard]] std::expected<void, std::string> verify_alignment_codec_transport() {
    constexpr std::size_t boundary = 128u << 10u;
    alignment_codec_scratch scratch;
    if (auto prepared = scratch.prepare(boundary + 1u, true); !prepared) return prepared;
    std::array<std::uint8_t, 64u << 10u> input;
    input.fill(0xa5u);
    for (const auto count : {boundary - 1u, boundary, boundary + 1u}) {
        scratch.reset();
        for (std::size_t remaining = count; remaining != 0u;) {
            const auto bytes = std::min(remaining, input.size());
            if (auto written = scratch.append(std::span<const std::uint8_t>{input}.first(bytes)); !written) return written;
            remaining -= bytes;
        }
        if (auto flushed = scratch.flush(); !flushed) return flushed;
        if (scratch.output().size() != count || !std::ranges::all_of(scratch.output(), [](char value) {
                return static_cast<unsigned char>(value) == 0xa5u;
            })) return std::unexpected("codec transport boundary preflight differs");
    }
    scratch.reset();
    scratch.backend.destination.destination = scratch.backend.destination.destination.first(1u);
    const auto written = scratch.append(std::span<const std::uint8_t>{input}.first(2u));
    const auto flushed = scratch.flush();
    if ((!written || !flushed) && scratch.backend.failed) {
        if (scratch.finish() || scratch.stream != nullptr || scratch.file != nullptr)
            return std::unexpected("codec transport refusal cleanup lost its failure");
        return {};
    }
    return std::unexpected("codec transport capacity refusal was not observed");
}

/** Format checked native fields without regenerating projection or preservation payloads. */
template<alignment::alignment_byte_sink Sink>
[[nodiscard]] std::expected<void, std::string> emit_alignment_native_codec(
        Sink& sink, const alignment_codec_row& row, const sam_hdr_t* header, bool bam,
        bioseq::simd::sequence_rank_transform_kernel transform) {
    const auto& native = row.native;
    const alignment::bam_cigar_source source{native.cigar_words};
    const alignment::cigar_wire_recipe recipe{.source = source.borrow()};
    if (bam) {
        if (auto result = alignment::emit_bam_record_prefix(sink, native.qname, native.core, row.measured); !result) return result;
        if (auto result = alignment::emit_bam_cigar(sink, recipe, row.cigar, native.core, row.measured.long_cigar); !result) return result;
        if (auto result = sink.append(native.sequence_nibbles); !result) return result;
        if (auto result = sink.append(native.quality); !result) return result;
        for (const auto& field : row.auxiliary)
            if (auto result = alignment::emit_bam_auxiliary(sink, field); !result) return result;
        if (row.measured.long_cigar) return alignment::emit_bam_long_cigar(sink, recipe, row.cigar);
        return {};
    }
    const std::string_view reference = native.core.reference_id < 0 ? "*" : header->target_name[native.core.reference_id];
    const std::string_view mate = native.core.mate_reference_id < 0 ? "*" : header->target_name[native.core.mate_reference_id];
    if (auto result = alignment::emit_sam_record_prefix(sink, native.qname, reference, mate,
            native.core, recipe, row.cigar); !result) return result;
    constexpr std::array<std::uint8_t, 1> star{'*'}, tab{'\t'};
    constexpr auto tile_capacity = bioseq::simd::sequence_rank_io_chunk_capacity / 2u * 2u;
    static_assert(tile_capacity != 0u);
    std::array<std::uint8_t, tile_capacity> decoded, encoded;
    constexpr auto letters = [] {
        std::array<std::uint32_t, 16> result{};
        for (std::size_t index{}; index < result.size(); ++index)
            result[index] = static_cast<std::uint8_t>(alignment::bam_nucleotide_symbols[index]);
        return result;
    }();
    const auto length = static_cast<std::size_t>(native.core.sequence_length);
    if (length == 0u) {
        if (auto result = sink.append(star); !result) return result;
    }
    for (std::size_t position{}; position < length;) {
        const auto count = std::min(tile_capacity, length - position);
        auto ranks = std::span{decoded}.first(count);
        if (!alignment::decode_bam_wire_tile(native.sequence_nibbles.subspan(position / 2u,
                count / 2u + count % 2u), ranks, transform)
            || !transform(bioseq::simd::sequence_rank_transform::map_bytes, ranks, letters.size(),
                letters, std::span{encoded}.first(count)))
            return std::unexpected("codec SAM nucleotide tile failed");
        if (auto result = sink.append(std::span<const std::uint8_t>{encoded}.first(count)); !result) return result;
        position += count;
    }
    if (auto result = sink.append(tab); !result) return result;
    if (native.quality_missing) {
        if (auto result = sink.append(star); !result) return result;
    } else {
        constexpr auto qualities = alignment::quality_wire_table<seq::phred94, true>();
        for (std::size_t position{}; position < native.quality.size();) {
            const auto count = std::min(tile_capacity, native.quality.size() - position);
            if (!transform(bioseq::simd::sequence_rank_transform::map_bytes,
                    native.quality.subspan(position, count), qualities.size(), qualities,
                    std::span{encoded}.first(count)))
                return std::unexpected("codec SAM quality tile failed");
            if (auto result = sink.append(std::span<const std::uint8_t>{encoded}.first(count)); !result) return result;
            position += count;
        }
    }
    for (const auto& field : row.auxiliary)
        if (auto result = alignment::emit_sam_auxiliary(sink, field); !result) return result;
    return {};
}

/** Prepare wire expectations and borrowed descriptors once outside this iteration's clock. */
[[nodiscard]] std::expected<std::unique_ptr<alignment_codec_fixture>, std::string>
prepare_alignment_codec_fixture(const alignment_fixture& fixture,
        bioseq::simd::sequence_rank_transform_kernel transform) {
    std::size_t charge = 2u * sizeof(alignment_codec_fixture) + 128u;
    std::size_t maximum_bam_bytes{};
    for (const auto* record : fixture.native) {
        if (record->l_data < record->core.l_extranul) return std::unexpected("codec native size is invalid");
        const auto length = common::checked_add(static_cast<std::size_t>(record->l_data - record->core.l_extranul),
            std::size_t{36u} + (record->core.n_cigar > 65535u ? 16u : 0u));
        const auto doubled = length ? common::checked_mul(*length, std::size_t{2u}) : std::nullopt;
        const auto row_charge = doubled ? common::checked_add(*doubled,
            2u * (sizeof(std::string) + sizeof(alignment_codec_row)) + 256u) : std::nullopt;
        const auto total = row_charge ? common::checked_add(charge, *row_charge) : std::nullopt;
        if (!total) return std::unexpected("codec fixture charge overflows");
        charge = *total;
        maximum_bam_bytes = std::max(maximum_bam_bytes, *length);
    }
    auto admitted = reserve_io_memory(charge);
    if (!admitted) return std::unexpected("codec fixture admission refused");
    auto result = std::make_unique<alignment_codec_fixture>();
    result->permit = std::move(*admitted);
    result->maximum_bam_bytes = maximum_bam_bytes;
    result->bam_encoding.resize(fixture.native.size());
    result->rows.resize(fixture.native.size());
    alignment_codec_scratch scratch;
    if (auto prepared = scratch.prepare(result->maximum_bam_bytes, true); !prepared)
        return std::unexpected(prepared.error());
    std::size_t auxiliary_charge{};
    for (std::size_t ordinal{}; ordinal < fixture.native.size(); ++ordinal) {
        scratch.reset();
        if (bam_write1(scratch.stream, fixture.native[ordinal]) < 0) {
            scratch.backend.failed = true; return std::unexpected("codec native expectation write failed");
        }
        if (auto flushed = scratch.flush(); !flushed) return std::unexpected(flushed.error());
        auto& bytes = result->bam_encoding[ordinal];
        bytes.assign(scratch.output());
        auto& row = result->rows[ordinal];
        const auto parsed = alignment::parse_bam_record(
            {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()},
            static_cast<std::uint32_t>(fixture.header->n_targets), transform);
        if (!parsed || parsed->consumed_bytes != bytes.size())
            return std::unexpected(parsed ? "codec expectation has trailing bytes" : parsed.error());
        row.native = *parsed;
        std::size_t offset{}, auxiliary_count{}, auxiliary_bytes{};
        while (offset < row.native.auxiliary.size()) {
            const auto field = alignment::parse_bam_auxiliary(row.native.auxiliary.subspan(offset));
            if (!field) return std::unexpected(field.error());
            if (!(row.native.long_cigar && field->field.tag == std::array{'C', 'G'})) {
                ++auxiliary_count;
                auxiliary_bytes += field->consumed_bytes;
            }
            offset += field->consumed_bytes;
        }
        const auto view_bytes = common::checked_mul(auxiliary_count, 2u * sizeof(auxiliary_field_view));
        const auto total = view_bytes ? common::checked_add(auxiliary_charge, *view_bytes) : std::nullopt;
        if (!total) return std::unexpected("codec auxiliary charge overflows");
        auxiliary_charge = *total;
        row.auxiliary_count = auxiliary_count;
        const alignment::bam_cigar_source source{row.native.cigar_words};
        const alignment::cigar_wire_recipe recipe{.source = source.borrow()};
        const auto measured = alignment::measure_wire_cigar(recipe);
        if (!measured) return std::unexpected(measured.error());
        row.cigar = *measured;
        const auto wire = alignment::measure_bam_record(row.native.qname, row.native.core, row.cigar, auxiliary_bytes);
        if (!wire) return std::unexpected(wire.error());
        row.measured = *wire;
    }
    // All existing wire owners are already charged. Reserve exactly the sum of
    // pending view allocations once, rather than probing host policy per row.
    const auto wanted = common::checked_add(result->permit.bytes(), static_cast<std::uint64_t>(auxiliary_charge));
    if (!wanted || !grow_io_memory(result->permit, *wanted))
        return std::unexpected("codec auxiliary admission refused");
    for (std::size_t ordinal{}; ordinal < fixture.native.size(); ++ordinal) {
        auto& row = result->rows[ordinal];
        row.auxiliary.reserve(row.auxiliary_count);
        std::size_t offset{};
        while (offset < row.native.auxiliary.size()) {
            const auto field = alignment::parse_bam_auxiliary(row.native.auxiliary.subspan(offset));
            if (!field) return std::unexpected(field.error());
            if (!(row.native.long_cigar && field->field.tag == std::array{'C', 'G'})) row.auxiliary.push_back(field->field);
            offset += field->consumed_bytes;
        }
        scratch.reset();
        if (auto encoded = emit_alignment_native_codec(scratch, row, fixture.header, true, transform); !encoded)
            return std::unexpected(encoded.error());
        if (auto flushed = scratch.flush(); !flushed) return std::unexpected(flushed.error());
        if (scratch.output() != result->bam_encoding[ordinal]) return std::unexpected("native BAM codec preflight differs");
    }
    if (!scratch.finish()) return std::unexpected("codec expectation close failed");
    return result;
}

/** Reuse one equal-capacity SAM owner to validate both encoders before timing. */
[[nodiscard]] std::expected<void, std::string> verify_alignment_native_sam(
        const alignment_fixture& fixture, const alignment_codec_fixture& codec,
        alignment_codec_scratch& scratch, bioseq::simd::sequence_rank_transform_kernel transform) {
    for (std::size_t ordinal{}; ordinal < fixture.native.size(); ++ordinal) {
        scratch.reset();
        const auto encoded = emit_alignment_native_codec(scratch.backend.destination,
            codec.rows[ordinal], fixture.header, false, transform);
        if (!encoded) return encoded;
        if (scratch.output() != fixture.native_encoding[ordinal]
            || sam_format1(fixture.header, fixture.native[ordinal], &scratch.text) < 0
            || scratch.text.m != scratch.text_capacity
            || std::string_view{scratch.text.s, scratch.text.l} != fixture.native_encoding[ordinal])
            return std::unexpected("native SAM codec preflight differs");
    }
    return {};
}

/** Bounded native edge records exercise the same owners without changing timed inputs. */
[[nodiscard]] std::expected<void, std::string> verify_alignment_native_boundaries(
        const alignment_fixture& fixture, bioseq::simd::sequence_rank_transform_kernel transform) {
    // Header duplication, one 196-KiB SAM line, four native records, formatter
    // capacity and their old/new string peaks fit this explicit scratch allowance.
    // Nested codec expectations/SAM output retain their own existing-ledger permits.
    auto admitted = reserve_io_memory(4u << 20u);
    if (!admitted) return std::unexpected("native boundary preflight admission refused");
    alignment_fixture probe;
    probe.header = sam_hdr_dup(fixture.header);
    if (probe.header == nullptr || probe.header->n_targets == 0)
        return std::unexpected("native boundary header preparation failed");
    probe.native.reserve(4u);
    probe.native_encoding.reserve(4u);
    alignment_native_text parsed_text, expected;
    const auto target = std::string{probe.header->target_name[0]};
    for (std::size_t ordinal{}; ordinal < 4u; ++ordinal) {
        std::string line;
        if (ordinal == 0u) {
            line = "codec_odd\t0\t" + target + "\t1\t60\t3M\t*\t0\t0\tA=N\t*"
                "\tXA:A:K\tXZ:Z:text\tXH:H:00AF\tXI:i:-2\tXU:i:4000000000\tXB:B:i,-1,2\tXF:f:1.5";
        } else if (ordinal == 1u) {
            line = "codec_single\t0\t" + target + "\t1\t60\t1M\t*\t0\t0\tA\t!";
        } else if (ordinal == 2u) {
            line = "codec_empty\t4\t*\t0\t0\t*\t*\t0\t0\t*\t*";
        } else {
            line = "codec_long\t0\t" + target + "\t1\t60\t";
            for (std::size_t run{}; run < 32768u; ++run) line.append("1M1I");
            line.append("\t*\t0\t0\t");
            line.append(65536u, 'A');
            line.append("\t*");
        }
        parsed_text.value.l = 0u;
        if (kputsn(line.data(), line.size(), &parsed_text.value) < 0)
            return std::unexpected("native boundary SAM allocation failed");
        auto* record = bam_init1();
        if (record == nullptr) return std::unexpected("native boundary BAM allocation failed");
        probe.native.push_back(record);
        if (sam_parse1(&parsed_text.value, probe.header, record) < 0)
            return std::unexpected("native boundary SAM parse failed");
        if (ordinal == 1u) bam_get_qual(record)[0] = 9u;
        if (ordinal == 3u && record->core.n_cigar != 65536u)
            return std::unexpected("native long-CIGAR preflight shape differs");
        if (sam_format1(probe.header, record, &expected.value) < 0)
            return std::unexpected("native boundary SAM format failed");
        probe.native_encoding.emplace_back(expected.value.s, expected.value.l);
        probe.native_text_capacity = expected.value.m;
    }
    auto codec = prepare_alignment_codec_fixture(probe, transform);
    if (!codec) return std::unexpected(codec.error());
    if (!(**codec).rows.back().measured.long_cigar)
        return std::unexpected("native long-CIGAR transport was not selected");
    alignment_codec_scratch scratch;
    if (auto prepared = scratch.prepare(probe.native_text_capacity, false); !prepared) return prepared;
    return verify_alignment_native_sam(probe, **codec, scratch, transform);
}

/** Validate publication and decode every native record with the pinned CLI outside timing. */
[[nodiscard]] std::expected<void, std::string> validate_alignment_cli(
        const std::string& path, std::size_t expected_records) {
    const bioseq::gpu::vulkan::run_capture_limits limits{
        .maximum_output_bytes = 64u << 10u};
    const std::array<std::string, 5> quickcheck_arguments{
        BIOSEQ_SAMTOOLS_EXECUTABLE, "quickcheck", "-v", "--", path};
    const auto checked = bioseq::gpu::vulkan::run_capture(quickcheck_arguments, limits);
    if (!checked) return std::unexpected("samtools quickcheck for " + path + ": " + checked.error());
    // quickcheck checks headers/EOF only. view -c traverses and decodes the
    // complete stream, including BGZF CRC and record framing, without a large capture.
    const std::array<std::string, 5> view_arguments{
        BIOSEQ_SAMTOOLS_EXECUTABLE, "view", "-c", "--", path};
    const auto counted = bioseq::gpu::vulkan::run_capture(view_arguments, limits);
    if (!counted) return std::unexpected("samtools view for " + path + ": " + counted.error());
    std::string_view count_text{*counted};
    while (!count_text.empty() && (count_text.back() == '\n' || count_text.back() == '\r'))
        count_text.remove_suffix(1);
    std::uint64_t count{};
    const auto parsed = std::from_chars(count_text.data(), count_text.data() + count_text.size(), count);
    if (parsed.ec != std::errc{} || parsed.ptr != count_text.data() + count_text.size()
        || count != expected_records)
        return std::unexpected("samtools view count for " + path + " differs from "
            + std::to_string(expected_records) + ": " + *counted);
    return {};
}

/** All selected alphabets share one benchmark case and the same alignment geometry. */
struct alignment_benchmark_input {
    std::map<std::string, std::shared_ptr<alignment_fixture>> fixtures;
    std::string case_name;
    std::size_t record_count{}, record_length{};
    int compression_level{6};
    bool warm_inputs{};
};

/** Timing is supplied by the framework; these counters prove work and file validity. */
struct alignment_measurement {
    std::size_t records{}, bytes{};
    bool valid{};
    bool observed_content{};
    alignment_cache_observation cache;
    std::uint64_t observation_to_measurement_ns{}, measured_ns{};
    std::string error;
};

/** The arena outlives all iteration-owned codec scratch and native expectations. */
struct alignment_iteration_preparation {
    io_pool_scope pool;
    std::unique_ptr<alignment_codec_fixture> codec;
    std::vector<std::unique_ptr<alignment_codec_scratch>> scratch;
    bioseq::simd::sequence_rank_transform_kernel transform{};
    alignment_cache_observation cache;
    std::string error;
    explicit alignment_iteration_preparation(std::int64_t workers) : pool{workers}, error{pool.error} {}
    alignment_iteration_preparation(const alignment_iteration_preparation&) = delete;
    alignment_iteration_preparation(alignment_iteration_preparation&&) noexcept = default;
    /** Close every vendor owner before restoring the pool; failure remains observable. */
    [[nodiscard]] bool finish_codec() noexcept {
        bool valid = true;
        for (auto& owner : scratch) if (!owner->finish()) valid = false;
        scratch.clear();
        codec.reset();
        return valid;
    }
};

/** Prepare identical capacities and scheduling, or fail before any measured work. */
[[nodiscard]] alignment_iteration_preparation prepare_alignment_iteration(std::string_view algorithm,
        const parameter_map& parameters, const alignment_benchmark_input& input, std::size_t repeat) {
    const auto requested = parameters.at("workers").as_integer();
    alignment_iteration_preparation preparation{requested};
    const auto operation = parameters.at("operation").as_string();
    const auto format = parameters.at("format").as_string();
    if (operation != "read" && operation != "write" && operation != "read_cold" && operation != "native_encode")
        preparation.error = "unknown alignment benchmark operation";
    if (format != "sam" && format != "bam") preparation.error = "unknown alignment benchmark format";
    if (algorithm != "bioseq_alignment" && algorithm != "samtools_alignment")
        preparation.error = "unknown alignment benchmark algorithm";
    if (requested != 1 && requested != 4) preparation.error = "alignment worker budget is unsupported";
    if (!preparation.error.empty()) return preparation;
    const auto& fixture = *input.fixtures.at(std::string{parameters.at("alphabet").as_string()});
    if (!fixture.valid) { preparation.error = "fixture preparation failed"; return preparation; }
    const auto& path = format == "bam" ? fixture.bam_path : fixture.sam_path;
    if (operation == "native_encode") {
        if (auto transport = verify_alignment_codec_transport(); !transport) {
            preparation.error = transport.error(); return preparation;
        }
        preparation.transform = alignment::sequence_codec_kernel();
        if (preparation.transform == nullptr) { preparation.error = "native codec SIMD kernel is unavailable"; return preparation; }
        if (auto boundaries = verify_alignment_native_boundaries(fixture, preparation.transform); !boundaries) {
            preparation.error = boundaries.error(); return preparation;
        }
        auto codec = prepare_alignment_codec_fixture(fixture, preparation.transform);
        if (!codec) { preparation.error = codec.error(); return preparation; }
        preparation.codec = std::move(*codec);
        const auto capacity = format == "bam" ? preparation.codec->maximum_bam_bytes : fixture.native_text_capacity;
        const auto partitions = std::min(static_cast<std::size_t>(requested), fixture.native.size());
        for (std::size_t index{}; index < partitions; ++index) {
            auto scratch = std::make_unique<alignment_codec_scratch>();
            if (auto allocated = scratch->prepare(capacity, format == "bam"); !allocated) {
                preparation.error = allocated.error(); return preparation;
            }
            preparation.scratch.push_back(std::move(scratch));
        }
        if (format == "sam" && !preparation.scratch.empty()) {
            if (auto checked = verify_alignment_native_sam(fixture, *preparation.codec,
                    *preparation.scratch.front(), preparation.transform); !checked) {
                preparation.error = checked.error(); return preparation;
            }
        }
    } else if (operation == "read_cold") {
        const auto& identity = format == "bam" ? fixture.bam_identity : fixture.sam_identity;
        preparation.cache = prepare_alignment_cold_input(path, identity);
        if (!preparation.cache.error.empty()) preparation.error = preparation.cache.error;
        else if (!preparation.cache.cold) preparation.error = "cold input retains observed resident pages";
        bioseq_log<logging_level::info>(
            "alignment cache: case={} algorithm={} alphabet={} format={} operation={} workers={} repeat={} "
            "path={} device={} inode={} bytes={} mtime={}.{} ctime={}.{} requested={} invalidated={} "
            "observed={} observed_pages={} resident_pages={} resident_bytes={} request_ns={} observation_ns={} "
            "observed_at_ns={} cold={} request_errno={} observation_errno={} error={}",
            input.case_name, algorithm, fixture.alphabet, format, operation, requested, repeat,
            path, identity.device, identity.inode, identity.bytes, identity.modified_seconds, identity.modified_nanoseconds,
            identity.changed_seconds, identity.changed_nanoseconds, preparation.cache.requested, preparation.cache.invalidated,
            preparation.cache.observed, preparation.cache.pages, preparation.cache.resident_pages,
            preparation.cache.resident_bytes, preparation.cache.request_ns, preparation.cache.observation_ns,
            preparation.cache.observed_at_ns, preparation.cache.cold, preparation.cache.request_errno,
            preparation.cache.observation_errno, preparation.error);
    } else if (input.warm_inputs && operation == "read") {
        std::ifstream stream{path, std::ios::binary};
        std::array<char, 64u << 10u> buffer;
        if (!stream) { preparation.error = "cannot warm the measured input"; return preparation; }
        while (stream.read(buffer.data(), buffer.size())) {}
        if (!stream.eof()) { preparation.error = "input warmup read failed"; return preparation; }
    }
    auto& pool = parallelism::global_pool::instance();
    const auto workers = pool.maximum_concurrency();
    const bool codec = operation == "native_encode";
    bioseq_log<logging_level::info>(
        "alignment budget: algorithm={} alphabet={} format={} operation={} repeat={} "
        "requested_workers={} pool_limit={} framework_limit={} hts_worker_threads={} "
        "hts_dispatch_threads={} caller_threads=1 active_workers=unobserved cache_preparation={} public_bam_level={}",
        algorithm, fixture.alphabet, format, operation, repeat, requested, workers,
        preparation.pool.framework_limit, codec || workers == 1u ? 0u : workers - 2u,
        codec || workers == 1u ? 0u : 1u,
        codec || operation == "write" ? "resident" : operation == "read_cold" ? "residency_observation"
            : input.warm_inputs ? "pre_read_completed" : "legacy_unspecified", codec ? -1 : input.compression_level);
    return preparation;
}

/** Encode independent, ordered partitions using identical scratch and byte consumption. */
[[nodiscard]] alignment_measurement run_alignment_native_codec(std::string_view algorithm,
        const parameter_map& parameters, const alignment_benchmark_input& input,
        alignment_iteration_preparation& preparation) {
    alignment_measurement result;
    if (!preparation.error.empty() || !preparation.codec) {
        result.error = preparation.error.empty() ? "native codec preparation is missing" : preparation.error;
        return result;
    }
    const auto& fixture = *input.fixtures.at(std::string{parameters.at("alphabet").as_string()});
    const bool bam = parameters.at("format").as_string() == "bam";
    const bool bioseq = algorithm == "bioseq_alignment";
    const auto& expected = bam ? preparation.codec->bam_encoding : fixture.native_encoding;
    struct partition_result { std::size_t records{}, bytes{}; bool valid{}; std::string error; };
    std::array<partition_result, 4> results;
    const auto count = preparation.scratch.size();
    const auto run_partition = [&fixture, &preparation, &expected, &results, count, bam, bioseq](std::size_t partition) {
        auto& status = results[partition];
        const auto begin = fixture.native.size() / count * partition + std::min(fixture.native.size() % count, partition);
        const auto end = fixture.native.size() / count * (partition + 1u)
            + std::min(fixture.native.size() % count, partition + 1u);
        auto& scratch = *preparation.scratch[partition];
        bioseq::bench::framework::ordered_output_observation observation{
            std::span<const std::string>{expected}.subspan(begin, end - begin)};
        for (auto ordinal = begin; ordinal < end; ++ordinal) {
            scratch.reset();
            std::string_view emitted;
            if (bam) {
                if (bioseq) {
                    const auto encoded = emit_alignment_native_codec(scratch, preparation.codec->rows[ordinal],
                        fixture.header, true, preparation.transform);
                    if (!encoded) { status.error = encoded.error(); return; }
                } else if (bam_write1(scratch.stream, fixture.native[ordinal]) < 0) {
                    scratch.backend.failed = true; status.error = "native HTSlib BAM write failed"; return;
                }
                if (auto flushed = scratch.flush(); !flushed) { status.error = flushed.error(); return; }
                emitted = scratch.output();
            } else if (bioseq) {
                const auto encoded = emit_alignment_native_codec(scratch.backend.destination,
                    preparation.codec->rows[ordinal], fixture.header, false, preparation.transform);
                if (!encoded) { status.error = encoded.error(); return; }
                emitted = scratch.output();
            } else {
                if (sam_format1(fixture.header, fixture.native[ordinal], &scratch.text) < 0
                    || scratch.text.m != scratch.text_capacity) {
                    status.error = "native HTSlib SAM formatting failed or grew scratch"; return;
                }
                emitted = {scratch.text.s, scratch.text.l};
            }
            if (!observation.begin_record() || !observation.append(emitted) || !observation.end_record()) {
                status.error = "measured native codec bytes differ"; return;
            }
            const auto total = common::checked_add(status.bytes, emitted.size());
            if (!total) { status.error = "native codec byte count overflows"; return; }
            status.bytes = *total;
        }
        status.records = observation.records();
        status.valid = observation.complete();
    };
    if (count == 1u) run_partition(0u);
    else if (count != 0u) {
        auto group = parallelism::global_pool::instance().make_task_group();
        for (std::size_t partition{}; partition < count; ++partition)
            group.run([&run_partition, partition] { run_partition(partition); });
        group.wait();
    }
    result.valid = true;
    for (std::size_t partition{}; partition < count; ++partition) {
        const auto& status = results[partition];
        const auto bytes = common::checked_add(result.bytes, status.bytes);
        if (!status.valid || !bytes) {
            result.valid = false; result.error = status.error.empty() ? "native codec partition failed" : status.error;
        } else result.bytes = *bytes;
        result.records += status.records;
    }
    result.observed_content = result.valid && result.records == fixture.native.size();
    result.valid = result.observed_content;
    return result;
}

/** Borrow admitted immutable rows; the fixture retains them for the complete export. */
struct alignment_fixture_batch {
    const alignment_fixture* fixture{};
    std::size_t begin{}, count{};
    /** Number of borrowed rows in this batch. */
    [[nodiscard]] std::size_t size() const { return count; }
    /** Return a stable original record view. */
    [[nodiscard]] export_record_view row(std::size_t index) const { return fixture->original[begin + index]->record; }
    /** Decoded owners already retain their shared-memory permits. */
    [[nodiscard]] std::size_t accounted_capacity_bytes() const { return 0; }
};

/** A single forward traversal over pre-materialized source records. */
struct alignment_fixture_source {
    using batch_type = alignment_fixture_batch;
    const alignment_fixture& fixture;
    std::size_t position{};
    /** Known source types are carried by the manifest metadata. */
    [[nodiscard]] std::span<const record_column> columns() const { return {}; }
    /** Borrow original dictionaries without adding native header or payload copies. */
    [[nodiscard]] const record_source_metadata& metadata() const { return fixture.metadata; }
    /** Issue one bounded row batch, never executing a source twice. */
    [[nodiscard]] std::expected<std::optional<batch_type>, record_error> next() {
        if (position == fixture.original.size()) return std::optional<batch_type>{};
        const auto count = std::min<std::size_t>(512, fixture.original.size() - position);
        batch_type batch{&fixture, position, count};
        position += count;
        return std::optional{batch};
    }
    /** Cancellation stops future batches; the fixture owns all borrowed storage. */
    void cancel() { position = fixture.original.size(); }
};
static_assert(export_record_source<alignment_fixture_source>);

/** Generate only requested reference ranks; the synthetic fixture has a defined periodic reference. */
[[nodiscard]] const std::uint8_t* read_alignment_reference(const void* context,
        bioseq::storage::sequence_size_t offset, std::size_t count, std::span<std::uint8_t> scratch) {
    const auto& fixture = *static_cast<const alignment_fixture*>(context);
    for (std::size_t index = 0; index < count; ++index)
        scratch[index] = static_cast<std::uint8_t>((offset + index) % fixture.cardinality);
    return scratch.data();
}

/** Build true matches, one insertion/deletion/mismatch, and real soft clipping. */
[[nodiscard]] std::expected<std::unique_ptr<decoded_record>, std::string> make_alignment_record(
        std::uint8_t alphabet, std::size_t cardinality, std::size_t ordinal, std::size_t length,
        const bioseq::storage::borrowed_rank_window_source& reference) {
    auto record = std::make_unique<decoded_record>();
    record->record.ordinal = ordinal;
    record->record.reference_sequence = reference;
    const auto name = "read_" + std::to_string(ordinal);
    if (auto field = record->field("header", name); !field) return std::unexpected(field.error());
    if (auto field = record->field("qname", name); !field) return std::unexpected(field.error());
    record->record.identity.template_id = ordinal + 1;
    record->record.identity.segment_id = ordinal + 1;
    record->record.identity.reference_id = 1;
    record->record.identity.classification = alignment_classification_source::explicit_flags;
    record->record.alignment.emplace();
    auto& alignment = *record->record.alignment;
    alignment.flags = 0;
    alignment.mapping_quality = 60;
    alignment.reference_name = "reference";
    alignment.reference_length = 100000000;
    alignment.query_length = length;
    alignment.query_start = 3;
    alignment.query_end = length;
    alignment.target_start = (ordinal * 17u) % (100000000 - length);
    alignment.target_end = *alignment.target_start + length - 3;
    alignment.sequence_orientation = record_orientation::reference_forward;
    alignment.form = cigar_form::sam_complete;
    if (auto context = record->field("header_context", "1"); !context)
        return std::unexpected(context.error());
    const std::array<std::pair<char, std::uint64_t>, 7> operations{{
        {'S', 3}, {'=', 20}, {'I', 1}, {'=', 20}, {'D', 1}, {'X', 1}, {'=', length - 45}}};
    if (auto begun = record->begin_cigar(true); !begun) return std::unexpected(begun.error());
    std::vector<std::uint8_t> ranks;
    ranks.reserve(length);
    auto reference_position = *alignment.target_start;
    for (const auto [operation, count] : operations) {
        const cigar_run run{static_cast<std::uint32_t>(count), seqan3::assign_char_to(operation, cigar_symbol{})};
        if (auto appended = record->append_cigar_run(run); !appended) return std::unexpected(appended.error());
        for (std::uint64_t index = 0; index < count; ++index) {
            if (operation != 'D') ranks.push_back(static_cast<std::uint8_t>(
                (reference_position + (operation == 'X' ? 1u : 0u)) % cardinality));
            if (operation != 'I' && operation != 'S') ++reference_position;
        }
    }
    if (auto ended = record->end_cigar(); !ended) return std::unexpected(ended.error());
    const auto quality_code = supported_storage_alphabets::index_of<seq::phred42>();
    for (const auto role : {record_field_role::sequence, record_field_role::quality}) {
        const preserved_sequence_descriptor descriptor{
            role == record_field_role::sequence ? alphabet : quality_code, true, length, preservation_storage::rank_hex};
        if (auto begun = record->begin_sequence(role, descriptor); !begun) return std::unexpected(begun.error());
        if (role == record_field_role::quality)
            for (std::size_t index = 0; index < ranks.size(); ++index) ranks[index] = static_cast<std::uint8_t>((ordinal + index) % 42);
        if (auto appended = record->append_sequence_ranks(role, ranks); !appended) return std::unexpected(appended.error());
        if (auto ended = record->end_sequence(role); !ended) return std::unexpected(ended.error());
    }
    if (auto metadata = record->metadata("sample", "text", "alignment benchmark", false); !metadata)
        return std::unexpected(metadata.error());
    const auto frozen = record->freeze();
    if (!frozen) return std::unexpected(frozen.error());
    return record;
}

/** Estimate original compact records, baseline native records and bounded worker slabs. */
[[nodiscard]] run_constraints estimate_alignment_resources(std::string_view, std::string_view,
        simdjson::dom::element data) {
    const auto rows = static_cast<std::size_t>(read_int_field(data, "rows", 4096));
    const auto length = static_cast<std::size_t>(read_int_field(data, "length", 150));
    // Original fixtures retain their old scope. Additional codec expectations
    // exist only inside codec preparation; this remains a framework estimate.
    const auto symbols = common::checked_mul(length, std::size_t{32u});
    const auto per_row = symbols ? common::checked_add(*symbols, std::size_t{16384u}) : std::nullopt;
    const auto all_rows = per_row ? common::checked_mul(rows, *per_row) : std::nullopt;
    const auto base = all_rows ? common::checked_mul(*all_rows,
        static_cast<std::size_t>(supported_biological_alphabets::alphabet_count)) : std::nullopt;
    const auto extra = all_rows ? common::checked_add(*all_rows, std::size_t{64u << 20u}) : std::nullopt;
    return {.base_memory = base.value_or(std::numeric_limits<std::size_t>::max()),
        .extra_memory = extra.value_or(std::numeric_limits<std::size_t>::max())};
}

/** Produce matched SAM/BAM fixtures and retain HTSlib's native records outside timing. */
[[nodiscard]] std::shared_ptr<alignment_fixture> build_alignment_fixture(std::string_view directory,
        std::string alphabet, std::size_t rows, std::size_t length, int compression_level) {
    auto fixture = std::make_shared<alignment_fixture>();
    fixture->alphabet = alphabet;
    fixture->compression_level = compression_level;
    const auto prefix = std::string{directory} + "/bioseq_alignment_" + std::to_string(getpid()) + "_" + alphabet;
    fixture->sam_path = prefix + ".sam";
    fixture->bam_path = prefix + ".bam";
    fixture->output_path = prefix + "_output";
    const auto code = supported_biological_alphabets::try_index_of_name(alphabet.c_str());
    if (!code || length < 46 || length >= 100000000
        || compression_level < 0 || compression_level > 9) return fixture;
    const auto cardinality = supported_biological_alphabets::cardinality_for_code(*code);
    if (!cardinality) return fixture;
    fixture->cardinality = *cardinality;
    const bioseq::storage::borrowed_rank_window_source reference{
        .context = fixture.get(), .read_at = read_alignment_reference,
        .symbol_count = 100000000, .alphabet_code = *code, .worker_shareable = true};
    fixture->metadata.declared_alphabet_codes.push_back(*code);
    fixture->metadata.declared_quality_alphabet_codes.push_back(supported_storage_alphabets::index_of<seq::phred42>());
    fixture->metadata.header_contexts.push_back({1, {"@HD\tVN:1.6\tSO:unknown"}});
    fixture->metadata.replay_header_lines.emplace(); // Selected native header has only HD/SQ.
    fixture->metadata.references.push_back({.identity = 1, .name = "reference", .length = 100000000,
        .alphabet_code = *code, .context_id = 1});
    fixture->metadata.reference_dictionary_complete = true;
    fixture->original.reserve(rows);
    fixture->original_encoding.reserve(rows);
    fixture->native_encoding.reserve(rows);
    for (std::size_t ordinal = 0; ordinal < rows; ++ordinal) {
        auto record = make_alignment_record(*code, *cardinality, ordinal, length, reference);
        if (!record) { bioseq_log<logging_level::error>("alignment fixture: {}", record.error()); return fixture; }
        alignment_comparison_sink expected;
        const identifier_parser::preservation_options preservation{
            .sequence_storage = preservation_storage::rank_hex,
            .quality_storage = preservation_storage::rank_hex};
        if (auto encoded = identifier_parser::append_preservation_record(expected,
                (*record)->record, preservation); !encoded) return fixture;
        fixture->original_encoding.push_back(std::move(expected.bytes));
        fixture->original.push_back(std::move(*record));
    }
    for (const auto format : {sequence_file_format::sam, sequence_file_format::bam}) {
        alignment_fixture_source source{*fixture};
        writer_options options;
        options.format = format;
        options.overwrite = true;
        options.compression_level = compression_level;
        const auto written = write_records(source, format == sequence_file_format::sam ? fixture->sam_path : fixture->bam_path, options);
        if (!written || *written != rows) {
            bioseq_log<logging_level::error>("alignment fixture writer: {}", written ? "record count mismatch" : written.error().message);
            return fixture;
        }
        const auto validated = validate_alignment_cli(
            format == sequence_file_format::sam ? fixture->sam_path : fixture->bam_path, rows);
        if (!validated) {
            bioseq_log<logging_level::error>("alignment fixture CLI validation: {}", validated.error());
            return fixture;
        }
    }
    samFile* input = sam_open(fixture->sam_path.c_str(), "r");
    if (!input) return fixture;
    fixture->header = sam_hdr_read(input);
    if (!fixture->header) { sam_close(input); return fixture; }
    alignment_native_text text;
    for (std::size_t ordinal = 0; ordinal < rows; ++ordinal) {
        auto* record = bam_init1();
        if (!record || sam_read1(input, fixture->header, record) < 0) {
            if (record) bam_destroy1(record);
            sam_close(input);
            return fixture;
        }
        fixture->native.push_back(record);
        const auto symbols = record->core.l_qseq < 0 ? std::nullopt
            : common::checked_add(fixture->native_sequence_symbols, static_cast<std::uint64_t>(record->core.l_qseq));
        if (!symbols) { sam_close(input); return fixture; }
        fixture->native_sequence_symbols = *symbols;
        text.value.l = 0;
        if (sam_format1(fixture->header, record, &text.value) < 0) {
            sam_close(input);
            return fixture;
        }
        fixture->native_encoding.emplace_back(text.value.s, text.value.l);
        fixture->native_text_capacity = text.value.m;
    }
    fixture->valid = sam_close(input) == 0;
    const auto sam_identity = alignment_file_identity::open(fixture->sam_path);
    const auto bam_identity = alignment_file_identity::open(fixture->bam_path);
    if (!sam_identity || !bam_identity) fixture->valid = false;
    else {
        fixture->sam_identity = *sam_identity; fixture->bam_identity = *bam_identity;
        const auto bytes = alignment_uncompressed_bam_bytes(fixture->bam_path, fixture->bam_identity);
        if (!bytes) {
            fixture->valid = false;
            bioseq_log<logging_level::error>("alignment fixture uncompressed byte count: {}", bytes.error());
        } else fixture->bam_uncompressed_bytes = *bytes;
    }
    bioseq_log<logging_level::info>("alignment baseline: identity={} HTSlib {} features {}; alphabet={}; "
        "bioseq_backend={}; bam_public_level={}; observed_read_consumption=canonical_exact_comparison",
        BIOSEQ_SAMTOOLS_BASELINE_ID, hts_version(), hts_feature_string(), alphabet,
        alignment::bgzf_compression_backend, compression_level);
    return fixture;
}

/** Materialize only selected alphabet fixtures for this one comparison case. */
[[nodiscard]] alignment_benchmark_input build_alignment_input(std::string_view directory, std::string_view case_name,
        simdjson::dom::element data, const run_constraints& constraints) {
    alignment_benchmark_input input;
    input.case_name = case_name;
    input.record_count = static_cast<std::size_t>(read_int_field(data, "rows", 4096));
    input.record_length = static_cast<std::size_t>(read_int_field(data, "length", 150));
    input.compression_level = static_cast<int>(read_int_field(data, "compression_level", 6));
    input.warm_inputs = read_int_field(data, "warm_inputs", 0) != 0;
    for (const auto& tuple : constraints.execution_tuples) {
        const auto alphabet = std::string{tuple.params.at("alphabet").as_string()};
        if (!input.fixtures.contains(alphabet))
            input.fixtures.emplace(alphabet, build_alignment_fixture(directory, alphabet, input.record_count,
                input.record_length, input.compression_level));
    }
    return input;
}

/** Observe exact restored records as the measured callback delivers them. */
struct alignment_comparison_consumer {
    const alignment_fixture& fixture;
    bioseq::bench::framework::ordered_output_observation observation;
    bool header_matches{};
    explicit alignment_comparison_consumer(const alignment_fixture& value)
        : fixture{value}, observation{value.original_encoding} {};
    /** Validate original dictionaries and source alphabet declarations. */
    [[nodiscard]] std::expected<void, std::string> header(const record_source_metadata& metadata) {
        if (metadata.declared_alphabet_codes != fixture.metadata.declared_alphabet_codes
            || metadata.declared_quality_alphabet_codes != fixture.metadata.declared_quality_alphabet_codes
            || metadata.reference_dictionary_complete != fixture.metadata.reference_dictionary_complete
            || metadata.replay_header_lines != fixture.metadata.replay_header_lines
            || metadata.header_contexts.size() != fixture.metadata.header_contexts.size()
            || metadata.references.size() != fixture.metadata.references.size())
            return std::unexpected("restored manifest shape differs");
        for (std::size_t index = 0; index < metadata.header_contexts.size(); ++index) {
            const auto& expected = fixture.metadata.header_contexts[index];
            const auto& observed = metadata.header_contexts[index];
            if (expected.id != observed.id || expected.original_header_lines != observed.original_header_lines)
                return std::unexpected("restored header context differs");
        }
        for (std::size_t index = 0; index < metadata.references.size(); ++index) {
            const auto& expected = fixture.metadata.references[index];
            const auto& observed = metadata.references[index];
            if (expected.identity != observed.identity || expected.name != observed.name
                || expected.length != observed.length || expected.alphabet_code != observed.alphabet_code
                || expected.context_id != observed.context_id || expected.circular != observed.circular
                || expected.attributes.size() != observed.attributes.size())
                return std::unexpected("restored reference differs");
            for (std::size_t attribute = 0; attribute < expected.attributes.size(); ++attribute)
                if (expected.attributes[attribute].tag != observed.attributes[attribute].tag
                    || expected.attributes[attribute].value != observed.attributes[attribute].value)
                    return std::unexpected("restored reference attribute differs");
        }
        header_matches = true;
        return {};
    }
    /** Compare exact canonical originals, including compact native ranks, after private validation. */
    [[nodiscard]] std::expected<void, std::string> record(decoded_record& record) {
        if (!observation.begin_record()) return std::unexpected("unexpected extra or reordered record");
        alignment_observation_sink observed{observation};
        const identifier_parser::preservation_options options{
            .sequence_storage = preservation_storage::rank_hex,
            .quality_storage = preservation_storage::rank_hex};
        if (auto encoded = identifier_parser::append_preservation_record(observed,
                record.record, options); !encoded) return encoded;
        if (!observation.end_record()) return std::unexpected("restored original record differs");
        return {};
    }
};

/** Execute one matched baseline or Bioseq load/write, with identical worker budget and level. */
[[nodiscard]] alignment_measurement run_alignment(std::string_view algorithm, const parameter_map& parameters,
        const alignment_benchmark_input& input, alignment_iteration_preparation* preparation = nullptr) {
    const auto operation = parameters.at("operation").as_string();
    if (operation != "read" && operation != "write" && operation != "read_cold" && operation != "native_encode")
        return alignment_measurement{.error = "unknown alignment benchmark operation"};
    if (operation == "native_encode") {
        if (preparation == nullptr) return alignment_measurement{.error = "native codec requires iteration preparation"};
        return run_alignment_native_codec(algorithm, parameters, input, *preparation);
    }
    if (operation == "read_cold" && (preparation == nullptr || !preparation->cache.cold))
        return alignment_measurement{.error = "cold read requires observed-cold preparation"};
    const auto& fixture = *input.fixtures.at(std::string{parameters.at("alphabet").as_string()});
    const auto format = parameters.at("format").as_string() == "bam" ? sequence_file_format::bam : sequence_file_format::sam;
    const bool reading = operation == "read" || operation == "read_cold";
    const auto workers = static_cast<std::size_t>(parameters.at("workers").as_integer());
    const bool bioseq = algorithm == "bioseq_alignment";
    alignment_measurement result;
    if (algorithm != "bioseq_alignment" && algorithm != "samtools_alignment") {
        result.error = "unknown alignment benchmark algorithm"; return result;
    }
    if (parameters.at("format").as_string() != "sam" && parameters.at("format").as_string() != "bam") {
        result.error = "unknown alignment benchmark format"; return result;
    }
    if (preparation != nullptr) result.cache = preparation->cache;
    if (!fixture.valid) { result.error = "fixture preparation failed"; return result; }
    const auto& path = format == sequence_file_format::bam ? fixture.bam_path : fixture.sam_path;
    const auto output = fixture.output_path + (format == sequence_file_format::bam ? ".bam" : ".sam");
    if (reading && bioseq) {
        alignment_comparison_consumer consumer{fixture};
        reader_options options;
        options.concurrency.worker_count = workers;
        options.io.parallel_gzip_thread_count = workers;
        const auto read = read_alignment_records(path, format, consumer, options);
        if (!read) { result.error = read.error(); return result; }
        result.records = consumer.observation.records();
        result.observed_content = consumer.header_matches && consumer.observation.complete();
        result.valid = result.observed_content && result.records == input.record_count;
    } else if (!reading && bioseq) {
        alignment_fixture_source source{fixture};
        writer_options options;
        options.format = format; options.overwrite = true; options.compression_level = input.compression_level;
        concurrency_options concurrency; concurrency.worker_count = workers;
        const auto written = write_records(source, output, options, concurrency);
        if (!written) { result.error = written.error().message; return result; }
        result.records = *written;
        result.valid = result.records == input.record_count;
    } else {
        const auto mode = reading ? std::string{"r"} : format == sequence_file_format::bam
            ? "wb" + std::to_string(input.compression_level) : std::string{"w"};
        samFile* file = sam_open((reading ? path : output).c_str(), mode.c_str());
        if (!file) { result.error = "HTSlib open failed"; return result; }
        // Pinned HTSlib adds one I/O/dispatcher thread besides its worker pool
        // (BGZF and SAM), so reserve both that thread and the calling thread.
        bool valid = workers <= 1 || hts_set_threads(file, static_cast<int>(workers - 2)) == 0;
        if (reading) {
            auto* header = sam_hdr_read(file);
            auto* record = bam_init1();
            int status = -2;
            bioseq::bench::framework::ordered_output_observation observation{fixture.native_encoding};
            alignment_native_text text;
            if (header && record && valid) {
                valid = sam_hdr_length(fixture.header) == sam_hdr_length(header)
                    && std::string_view{sam_hdr_str(fixture.header), sam_hdr_length(fixture.header)}
                        == std::string_view{sam_hdr_str(header), sam_hdr_length(header)};
                while (valid && (status = sam_read1(file, header, record)) >= 0) {
                    text.value.l = 0;
                    valid = sam_format1(header, record, &text.value) >= 0
                        && observation.begin_record()
                        && observation.append(std::string_view{text.value.s, text.value.l})
                        && observation.end_record();
                    ++result.records;
                }
            }
            result.observed_content = valid && status == -1 && observation.complete();
            valid = result.observed_content;
            if (record) bam_destroy1(record);
            if (header) sam_hdr_destroy(header);
        } else {
            valid = valid && sam_hdr_write(file, fixture.header) == 0;
            for (const auto* record : fixture.native) {
                if (!valid || sam_write1(file, fixture.header, record) < 0) { valid = false; break; }
                ++result.records;
            }
        }
        result.valid = sam_close(file) == 0 && valid && result.records == input.record_count;
    }
    std::error_code error;
    result.bytes = std::filesystem::file_size(reading ? path : output, error);
    if (error) { result.valid = false; result.error = error.message(); }
    return result;
}

/** Compare native semantic text from HTSlib, including all mandatory and auxiliary fields. */
[[nodiscard]] bool compare_alignment_output(const alignment_fixture& fixture, const std::string& path) {
    samFile* file = sam_open(path.c_str(), "r");
    if (!file) return false;
    auto* header = sam_hdr_read(file);
    auto* record = bam_init1();
    bool equal = header && record;
    if (equal) equal = sam_hdr_length(fixture.header) == sam_hdr_length(header)
        && std::string_view{sam_hdr_str(fixture.header), sam_hdr_length(fixture.header)}
            == std::string_view{sam_hdr_str(header), sam_hdr_length(header)};
    kstring_t expected{}, observed{};
    for (std::size_t index = 0; equal && index < fixture.native.size(); ++index) {
        expected.l = 0; observed.l = 0;
        equal = sam_read1(file, header, record) >= 0
            && sam_format1(fixture.header, fixture.native[index], &expected) >= 0
            && sam_format1(header, record, &observed) >= 0
            && expected.l == observed.l
            && std::string_view{expected.s, expected.l} == std::string_view{observed.s, observed.l};
    }
    if (equal) equal = sam_read1(file, header, record) == -1;
    std::free(expected.s); std::free(observed.s);
    if (record) bam_destroy1(record);
    if (header) sam_hdr_destroy(header);
    return sam_close(file) == 0 && equal;
}


/** Check original and projected fields outside timing; failed evidence is a blocking nonfinite sample. */
[[nodiscard]] std::map<std::string, double> evaluate_alignment(std::string_view,
        const parameter_map& parameters, const alignment_benchmark_input& input,
        const alignment_measurement& output) {
    const auto operation = parameters.at("operation").as_string();
    const bool codec = operation == "native_encode";
    const auto& fixture = *input.fixtures.at(std::string{parameters.at("alphabet").as_string()});
    const auto format = parameters.at("format").as_string() == "bam" ? sequence_file_format::bam : sequence_file_format::sam;
    const auto path = parameters.at("operation").as_string() == "write"
        ? fixture.output_path + (format == sequence_file_format::bam ? ".bam" : ".sam")
        : format == sequence_file_format::bam ? fixture.bam_path : fixture.sam_path;
    const bool reading = operation == "read" || operation == "read_cold";
    bool valid = output.valid && (!(reading || codec) || output.observed_content)
        && (codec || compare_alignment_output(fixture, path));
    if (operation == "read_cold") valid = valid && output.cache.cold;
    std::string error = output.error;
    if (valid && !codec) {
        const auto validated = validate_alignment_cli(path, input.record_count);
        valid = validated.has_value();
        if (!validated) error = validated.error();
    }
    if (valid && !codec) {
        const auto workers = parameters.at("workers").as_integer();
        io_pool_scope validation_scope{workers};
        alignment_comparison_consumer consumer{fixture};
        if (!validation_scope.error.empty()) {
            valid = false;
            error = validation_scope.error;
        } else {
            reader_options options;
            options.concurrency.worker_count = static_cast<std::size_t>(workers);
            options.io.parallel_gzip_thread_count = static_cast<std::size_t>(workers);
            const auto restored = read_alignment_records(path, format, consumer, options);
            valid = restored && consumer.header_matches && consumer.observation.complete()
                && consumer.observation.records() == input.record_count;
            if (!restored) error = restored.error();
        }
    }
    std::uint64_t uncompressed_bytes{};
    if (valid && !codec) {
        if (reading) {
            const auto& expected = format == sequence_file_format::bam ? fixture.bam_identity : fixture.sam_identity;
            const auto identity = alignment_file_identity::open(path);
            if (!identity || *identity != expected || identity->bytes != output.bytes) {
                valid = false; error = "input identity or physical byte count changed before evaluation";
            } else uncompressed_bytes = format == sequence_file_format::bam
                ? fixture.bam_uncompressed_bytes : fixture.sam_identity.bytes;
        } else {
            // Count this actual output: semantically equal BAM encodings may
            // retain different auxiliary integer widths or header spellings.
            const auto identity = alignment_file_identity::open(path);
            if (!identity || identity->bytes != output.bytes) {
                valid = false; error = "output identity or physical byte count changed before evaluation";
            } else if (format == sequence_file_format::sam) uncompressed_bytes = identity->bytes;
            else {
                const auto bytes = alignment_uncompressed_bam_bytes(path, *identity);
                if (!bytes) { valid = false; error = bytes.error(); }
                else uncompressed_bytes = *bytes;
            }
        }
        if (valid && (output.bytes == 0u || output.measured_ns == 0u)) {
            valid = false; error = "full-file byte metrics require nonzero bytes and elapsed time";
        }
    }
    const bool full_file_metrics = valid && !codec;
    if (!valid) bioseq_log<logging_level::error>("alignment IO validation failed: {}", error);
    return {{"record_count", static_cast<double>(output.records)}, {"bytes_on_disk", codec ? 0.0 : static_cast<double>(output.bytes)},
        {"input_symbols", static_cast<double>(input.record_count) * input.record_length},
        {"compression_level", !codec && format == sequence_file_format::bam ? static_cast<double>(input.compression_level) : 0.0},
        {"input_pre_read_completed", input.warm_inputs && operation == "read" ? 1.0 : 0.0},
        {"encoded_bytes", codec ? static_cast<double>(output.bytes) : 0.0},
        {"native_sequence_symbols", static_cast<double>(fixture.native_sequence_symbols)},
        {"full_file_byte_metrics_present", full_file_metrics ? 1.0 : 0.0},
        {"uncompressed_file_bytes", full_file_metrics ? static_cast<double>(uncompressed_bytes) : 0.0},
        {"uncompressed_to_physical_ratio", full_file_metrics
            ? static_cast<double>(uncompressed_bytes) / static_cast<double>(output.bytes) : 0.0},
        {"uncompressed_file_bytes_per_second", full_file_metrics
            ? static_cast<double>(uncompressed_bytes) * 1.0e9 / static_cast<double>(output.measured_ns) : 0.0},
        {"native_codec_control", codec ? 1.0 : 0.0},
        {"cache_invalidation_requested", output.cache.requested ? 1.0 : 0.0},
        {"cache_invalidation_succeeded", output.cache.invalidated ? 1.0 : 0.0},
        {"cache_residency_observed", output.cache.observed ? 1.0 : 0.0},
        {"cache_observed_pages", static_cast<double>(output.cache.pages)},
        {"cache_observed_resident_pages", static_cast<double>(output.cache.resident_pages)},
        {"cache_observed_file_bytes", static_cast<double>(output.cache.bytes)},
        {"cache_observed_resident_bytes", static_cast<double>(output.cache.resident_bytes)},
        {"cache_request_ns", static_cast<double>(output.cache.request_ns)},
        {"cache_observation_ns", static_cast<double>(output.cache.observation_ns)},
        {"cache_observation_to_measurement_ns", static_cast<double>(output.observation_to_measurement_ns)},
        {"cache_cold_observed", output.cache.cold ? 1.0 : 0.0},
        {"full_record_match", valid ? 1.0 : std::numeric_limits<double>::quiet_NaN()}};
}

/** Remove only generated benchmark data; pinned source and build artifacts are retained. */
void cleanup_alignment(std::string_view, std::string_view, simdjson::dom::element,
        const alignment_benchmark_input& input) {
    for (const auto& [alphabet, fixture] : input.fixtures) {
        std::error_code error;
        for (const auto& path : {fixture->sam_path, fixture->bam_path,
                fixture->output_path + ".sam", fixture->output_path + ".bam"}) std::filesystem::remove(path, error);
    }
}
} // namespace bioseq::io::benchmark::detail
