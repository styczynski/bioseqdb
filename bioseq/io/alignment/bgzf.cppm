/** @file Bounded BGZF framing and reusable raw-DEFLATE member encoding. */
module;

#include <new>
#include <libdeflate.h>

export module bioseq.io.alignment.bgzf;

import std;
import bioseq.common.checked_arithmetic;
import bioseq.common.heap_allocation;
import bioseq.io.compression.bit_reader;
import bioseq.io.compression.gzip_format;
import bioseq.io.simd;
import bioseq.io.sources;
import bioseq.io.options;
import bioseq.parallelism.pool;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.memory_admission;

export namespace bioseq::io::alignment {

/// Wire-format limits, independent of SIMD target width and worker count.
inline constexpr std::size_t bgzf_payload_limit = 65280;
inline constexpr std::size_t bgzf_member_limit = 65536;
inline constexpr std::size_t bgzf_header_bytes = 18;
inline constexpr std::size_t bgzf_trailer_bytes = 8;
inline constexpr std::array<std::uint8_t, 28> bgzf_eof_marker{
    0x1f, 0x8b, 0x08, 0x04, 0, 0, 0, 0, 0, 0xff,
    0x06, 0, 0x42, 0x43, 0x02, 0, 0x1b, 0, 0x03, 0,
    0, 0, 0, 0, 0, 0, 0, 0};

/** Checked member framing; spans borrow the supplied compressed source.
 * payload contains raw DEFLATE bytes. A decoder validates CRC/ISIZE after
 * decompression with validate_bgzf_payload before making records visible.
 */
struct bgzf_member_view {
    std::uint64_t compressed_offset{};
    std::span<const std::uint8_t> member;
    std::span<const std::uint8_t> payload;
    std::uint32_t checksum{};
    std::uint32_t uncompressed_bytes{};
    bool is_eof{};
};

/** Read one little-endian wire word from an already range-checked span. */
[[nodiscard]] inline std::uint32_t read_bgzf_u32(
    std::span<const std::uint8_t, 4> bytes) {
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

/** Frame one complete BGZF member without allocating or decoding its payload.
 * Unknown extra subfields are allowed; BC must be present exactly once.
 * The caller schedules independent members with the existing TBB admission path.
 */
[[nodiscard]] inline std::expected<bgzf_member_view, std::string>
inspect_bgzf_member(std::span<const std::uint8_t> bytes,
                    const std::uint64_t compressed_offset = 0) {
    compression::bit_reader reader{bytes};
    const auto header = compression::parse_gzip_header(reader, bytes);
    if (!header) return std::unexpected(header.error());
    if (!header->bgzf_member_bytes)
        return std::unexpected(std::string{"BGZF member has no BC size field"});
    const auto member_size = static_cast<std::size_t>(*header->bgzf_member_bytes);
    if (member_size > bytes.size())
        return std::unexpected(std::string{"BGZF member is truncated"});
    const auto header_size = header->end_bit_offset / 8;
    if (member_size < header_size + bgzf_trailer_bytes)
        return std::unexpected(std::string{"BGZF member cannot contain its trailer"});
    const auto trailer = bytes.subspan(member_size - bgzf_trailer_bytes, bgzf_trailer_bytes);
    const auto checksum = read_bgzf_u32(trailer.first<4>());
    const auto uncompressed = read_bgzf_u32(trailer.last<4>());
    if (uncompressed > bgzf_member_limit)
        return std::unexpected(std::string{"BGZF ISIZE exceeds 65536 bytes"});
    const auto member = bytes.first(member_size);
    return bgzf_member_view{
        compressed_offset, member,
        member.subspan(header_size, member_size - header_size - bgzf_trailer_bytes),
        checksum, uncompressed,
        member.size() == bgzf_eof_marker.size()
            && std::equal(member.begin(), member.end(), bgzf_eof_marker.begin())};
}

/** Validate independently decompressed member bytes before BAM framing.
 * The existing parallel decoder supplies exactly the member's output span.
 */
[[nodiscard]] inline std::expected<void, std::string> validate_bgzf_payload(
    const bgzf_member_view& member, std::span<const std::uint8_t> decoded) {
    if (decoded.size() != member.uncompressed_bytes)
        return std::unexpected(std::string{"BGZF ISIZE does not match decoded bytes"});
    const auto checksum = bioseq::io::simd::crc32_iso_hdlc_simd(
        0, decoded.data(), decoded.size());
    if (checksum != member.checksum)
        return std::unexpected(std::string{"BGZF CRC32 mismatch"});
    return {};
}

/** Framing is selected once per block; both forms share the same compressor. */
enum class gzip_member_framing : std::uint8_t { bgzf, ordinary };

/** One library allocation header retains the existing shared memory permit.
 * C allocation callbacks are the dependency boundary; allocation policy remains
 * in common.heap_allocation and parallelism.memory_admission.
 */
struct alignas(std::max_align_t) compression_allocation {
    parallelism::memory_permit permit;

    /** Reserve the allocator's measured upper bound before allocating a codec workspace.
     * Existing heap planning may throw on an unsupported allocator; the C ABI must
     * convert that cold initialization failure into a null allocation.
     */
    [[nodiscard]] static void* allocate(const std::size_t bytes) noexcept {
        try {
            const auto requested = common::checked_add(bytes, sizeof(compression_allocation));
            if (!requested) return nullptr;
            const auto plan = common::heap_allocation_detail::plan_heap_allocation(*requested);
            auto permit = parallelism::memory_admission_controller::instance().try_reserve(plan.allocation_requirement_bytes);
            if (!permit) return nullptr;
            std::size_t measured{};
            auto* allocation = common::heap_allocation_detail::allocate_measured(plan, measured);
            if (allocation == nullptr) return nullptr;
            permit->shrink_to(measured);
            auto* header = std::construct_at(static_cast<compression_allocation*>(allocation), std::move(*permit));
            return static_cast<void*>(header + 1);
        } catch (...) { return nullptr; }
    }

    /** Release codec storage before dropping the exact shared reservation. */
    static void release(void* payload) noexcept {
        if (payload == nullptr) return;
        auto* header = static_cast<compression_allocation*>(payload) - 1;
        auto permit = std::move(header->permit);
        std::destroy_at(header);
        std::free(header);
    }
};

inline constexpr std::string_view bgzf_compression_backend = "libdeflate " LIBDEFLATE_VERSION_STRING;

/** Worker-owned reusable libdeflate state; caller owns admitted input/output blocks.
 * Every library allocation is charged through per-compressor callbacks. No new
 * compressor algorithm, thread, pool or output buffer is implemented here.
 */
class bgzf_encoder {
    libdeflate_compressor* compressor_{};
public:
    /** Start closed; open performs fallible codec initialization once per worker. */
    bgzf_encoder() = default;
    bgzf_encoder(const bgzf_encoder&) = delete;
    bgzf_encoder& operator=(const bgzf_encoder&) = delete;
    bgzf_encoder(bgzf_encoder&&) = delete;
    bgzf_encoder& operator=(bgzf_encoder&&) = delete;

    /** Release the persistent codec workspace after its worker has drained. */
    ~bgzf_encoder() { close(); }

    /** Initialize the dependency's raw DEFLATE encoder at the requested common level. */
    [[nodiscard]] std::expected<void, std::string> open(const int level) {
        if (compressor_ != nullptr) return std::unexpected(std::string{"BGZF encoder is already open"});
        if (level < 0 || level > 9) return std::unexpected(std::string{"BGZF compression level must be in [0,9]"});
        // Match the pinned HTSlib BGZF public 0..9 scale, including its default
        // level6 -> libdeflate7 mapping (HTSlib1.25 bgzf.c:599-603).
        constexpr std::array<int, 10> htslib_levels{0, 1, 2, 3, 5, 6, 7, 8, 10, 12};
        const libdeflate_options options{sizeof(libdeflate_options), &compression_allocation::allocate, &compression_allocation::release};
        compressor_ = libdeflate_alloc_compressor_ex(htslib_levels[static_cast<std::size_t>(level)], &options);
        if (compressor_ == nullptr) return std::unexpected(std::string{"Cannot admit/allocate libdeflate compression workspace"});
        if (libdeflate_deflate_compress_bound(compressor_, bgzf_payload_limit)
            > bgzf_member_limit - bgzf_header_bytes - bgzf_trailer_bytes) {
            close();
            return std::unexpected(std::string{"libdeflate bound exceeds BGZF member capacity"});
        }
        return {};
    }

    /** Encode one member into caller storage; empty input still produces a valid stream. */
    template<gzip_member_framing Framing = gzip_member_framing::bgzf>
    [[nodiscard]] std::expected<std::size_t, std::string> encode(
        std::span<const std::uint8_t> input, std::span<std::uint8_t> output) {
        if (compressor_ == nullptr) return std::unexpected(std::string{"BGZF encoder is not open"});
        if (input.size() > bgzf_payload_limit || output.size() < bgzf_member_limit)
            return std::unexpected(std::string{"BGZF encoder input/output capacity is invalid"});
        constexpr auto header_bytes = Framing == gzip_member_framing::bgzf ? bgzf_header_bytes : std::size_t{10};
        const auto compressed_bytes = libdeflate_deflate_compress(compressor_, input.data(), input.size(),
            output.data() + header_bytes, bgzf_member_limit - header_bytes - bgzf_trailer_bytes);
        if (compressed_bytes == 0) return std::unexpected(std::string{"libdeflate could not fit its promised bounded destination"});
        const auto member_size = header_bytes + compressed_bytes + bgzf_trailer_bytes;
        std::copy_n(bgzf_eof_marker.begin(), header_bytes, output.begin());
        if constexpr (Framing == gzip_member_framing::bgzf) {
            const auto encoded_size = static_cast<std::uint16_t>(member_size - 1);
            output[16] = static_cast<std::uint8_t>(encoded_size);
            output[17] = static_cast<std::uint8_t>(encoded_size >> 8);
        } else output[3] = 0;
        const auto checksum = bioseq::io::simd::crc32_iso_hdlc_simd(0, input.data(), input.size());
        const auto trailer = member_size - bgzf_trailer_bytes;
        const auto input_size = static_cast<std::uint32_t>(input.size());
        for (std::size_t byte = 0; byte < 4; ++byte) {
            output[trailer + byte] = static_cast<std::uint8_t>(checksum >> (8 * byte));
            output[trailer + 4 + byte] = static_cast<std::uint8_t>(input_size >> (8 * byte));
        }
        return member_size;
    }

    /** Release initialized library storage once; harmless for an unopened encoder. */
    void close() noexcept {
        if (compressor_ != nullptr) {
            libdeflate_free_compressor(compressor_);
            compressor_ = nullptr;
        }
    }
};

/** Persistent independent-member inflater using the same proven dependency as samtools.
 * The shared IO source still owns exact BGZF framing, TBB scheduling and Highway
 * CRC validation; libdeflate owns only raw member inflation.
 */
class bgzf_decoder {
    libdeflate_decompressor* decompressor_{};
public:
    bgzf_decoder() = default;
    bgzf_decoder(const bgzf_decoder&) = delete;
    bgzf_decoder& operator=(const bgzf_decoder&) = delete;
    bgzf_decoder(bgzf_decoder&&) = delete;
    bgzf_decoder& operator=(bgzf_decoder&&) = delete;

    /** Release the reusable admitted inflater after its worker has joined. */
    ~bgzf_decoder() { if (decompressor_ != nullptr) libdeflate_free_decompressor(decompressor_); }

    /** Inflate exactly one independent member; reject short/extra input or output before delivery. */
    [[nodiscard]] std::expected<std::size_t, std::string> decode(
        const bgzf_member_view& member, std::span<std::uint8_t> output) {
        if (output.size() < member.uncompressed_bytes)
            return std::unexpected(std::string{"BGZF decoder output is not large enough"});
        if (decompressor_ == nullptr) {
            const libdeflate_options options{sizeof(libdeflate_options), &compression_allocation::allocate, &compression_allocation::release};
            decompressor_ = libdeflate_alloc_decompressor_ex(&options);
            if (decompressor_ == nullptr)
                return std::unexpected(std::string{"Cannot admit/allocate libdeflate decompression workspace"});
        }
        std::size_t consumed{}, produced{};
        // Output-pointer parameters are required solely by the dependency C ABI;
        // the public codec returns its validated count by value.
        const auto status = libdeflate_deflate_decompress_ex(decompressor_, member.payload.data(), member.payload.size(),
            output.data(), member.uncompressed_bytes, &consumed, &produced);
        if (status != LIBDEFLATE_SUCCESS || consumed != member.payload.size() || produced != member.uncompressed_bytes)
            return std::unexpected(std::string{"BGZF DEFLATE payload, consumed bytes or ISIZE is invalid"});
        if (const auto valid = validate_bgzf_payload(member, output.first(produced)); !valid)
            return std::unexpected(valid.error());
        return produced;
    }
};

/** Exact-member BGZF byte source with reusable admitted decoder slots and shared TBB. */
class bgzf_source {
    struct decode_slot {
        bgzf_decoder decoder;
        std::array<std::uint8_t, bgzf_member_limit> output{};
        std::array<std::uint8_t, bgzf_member_limit> compressed_input{};
        std::optional<bgzf_member_view> member;
        std::size_t size{};
        std::optional<std::string> error;
    };
    struct state {
        mmap_source mapping;
        std::optional<posix_read_source> streaming;
        bool streaming_eof{};
        std::span<const std::uint8_t> compressed;
        parallelism::memory_permit permit;
        std::vector<std::unique_ptr<decode_slot>> slots;
        std::size_t compressed_cursor{};
        std::size_t ready_slots{};
        std::size_t slot_index{};
        std::size_t slot_offset{};
        std::size_t eof_markers{};
        bool final_member_is_eof{};
        std::string error;

        /** Fill a bounded wire interval; clean EOF is reported only before a new member. */
        [[nodiscard]] std::expected<std::size_t, std::string> read_stream_bytes(std::span<std::uint8_t> destination) {
            std::size_t read{};
            while (read < destination.size()) {
                const auto result = streaming->read_into(destination.data() + read, destination.size() - read);
                if (result.has_error) return std::unexpected(streaming->last_error_message());
                if (result.bytes_read > destination.size() - read)
                    return std::unexpected(std::string{"BGZF compressed source exceeded its supplied buffer"});
                read += result.bytes_read;
                if (result.is_eof) { streaming_eof = true; break; }
                if (result.bytes_read == 0) return std::unexpected(std::string{"BGZF compressed source made no progress"});
            }
            return read;
        }

        /** Read one arbitrary-extra-field BGZF member using the shared extra-field validator. */
        [[nodiscard]] std::expected<std::optional<bgzf_member_view>, std::string> read_stream_member(decode_slot& slot) {
            if (streaming_eof) return std::optional<bgzf_member_view>{};
            auto bytes = std::span{slot.compressed_input};
            const auto fixed = read_stream_bytes(bytes.first(12));
            if (!fixed) return std::unexpected(fixed.error());
            if (*fixed == 0) return std::optional<bgzf_member_view>{};
            if (*fixed != 12 || bytes[0] != 0x1f || bytes[1] != 0x8b || bytes[2] != 8
                || (bytes[3] & 4u) == 0 || (bytes[3] & 0xe0u) != 0)
                return std::unexpected(std::string{"Sequential BAM input lacks a valid BGZF fixed header"});
            const auto extra_size = static_cast<std::size_t>(bytes[10]) | (static_cast<std::size_t>(bytes[11]) << 8);
            if (extra_size > bytes.size() - 20u)
                return std::unexpected(std::string{"BGZF extra fields cannot fit a complete member"});
            const auto extra = read_stream_bytes(bytes.subspan(12, extra_size));
            if (!extra) return std::unexpected(extra.error());
            if (*extra != extra_size) return std::unexpected(std::string{"BGZF extra fields are truncated"});
            const auto size = compression::parse_gzip_extra_fields(bytes.subspan(12, extra_size));
            if (!size) return std::unexpected(size.error());
            if (!*size || **size < 12u + extra_size + 8u || **size > bytes.size())
                return std::unexpected(std::string{"Sequential BGZF member lacks a valid BC size"});
            const auto rest_size = static_cast<std::size_t>(**size) - 12u - extra_size;
            const auto rest = read_stream_bytes(bytes.subspan(12u + extra_size, rest_size));
            if (!rest) return std::unexpected(rest.error());
            if (*rest != rest_size) return std::unexpected(std::string{"BGZF member payload/trailer is truncated"});
            const auto framed = inspect_bgzf_member(bytes.first(**size), compressed_cursor);
            if (!framed) return std::unexpected(framed.error());
            return std::optional<bgzf_member_view>{*framed};
        }

        /** Decode the next bounded member wave; no decoder or output storage is reallocated. */
        [[nodiscard]] bool refill() {
            ready_slots = 0;
            slot_index = 0;
            slot_offset = 0;
            while (ready_slots < slots.size()) {
                auto& slot = *slots[ready_slots];
                if (streaming) {
                    const auto framed = read_stream_member(slot);
                    if (!framed) { error = framed.error(); return false; }
                    if (!*framed) break;
                    slot.member = **framed;
                } else {
                    if (compressed_cursor == compressed.size()) break;
                    const auto framed = inspect_bgzf_member(compressed.subspan(compressed_cursor), compressed_cursor);
                    if (!framed) { error = framed.error(); return false; }
                    slot.member = *framed;
                }
                ++ready_slots;
                slot.error.reset();
                slot.size = 0;
                compressed_cursor += slot.member->member.size();
                final_member_is_eof = slot.member->is_eof;
                if (slot.member->is_eof) ++eof_markers;
            }
            if (ready_slots == 0) return true;
            auto& pool = parallelism::global_pool::instance();
            auto group = pool.make_task_group();
            for (std::size_t index = 0; index < ready_slots; ++index) {
                auto& slot = *slots[index];
                group.run(parallelism::task_body{[&slot]() {
                    const auto decoded = slot.decoder.decode(*slot.member, slot.output);
                    if (!decoded) slot.error = decoded.error();
                    else slot.size = *decoded;
                }});
            }
            group.wait();
            for (std::size_t index = 0; index < ready_slots; ++index) {
                if (slots[index]->error) { error = *slots[index]->error; return false; }
            }
            return true;
        }
    };
    std::unique_ptr<state> state_;
public:
    bgzf_source() = default;
    bgzf_source(const bgzf_source&) = delete;
    bgzf_source& operator=(const bgzf_source&) = delete;
    bgzf_source(bgzf_source&&) noexcept = default;
    bgzf_source& operator=(bgzf_source&&) noexcept = default;

    /** Admit and initialize the shared reusable member decoder/output slots once. */
    [[nodiscard]] static std::expected<bgzf_source, std::string> create(const reader_options& options) {
        auto& pool = parallelism::global_pool::instance();
        auto workers = options.io.parallel_gzip_thread_count;
        if (workers == 0) workers = options.concurrency.worker_count;
        if (workers == 0) workers = std::max<std::size_t>(1, parallelism::default_concurrency_limit());
        if (!pool.is_active()) pool.configure(parallelism::pool_config{
            .maximum_concurrency = static_cast<unsigned>(std::min<std::size_t>(workers, 64)),
            .mode = parallelism::pipeline_concurrency_mode::dispatch_parallel});
        workers = std::max<std::size_t>(1, std::min<std::size_t>(workers, pool.maximum_concurrency()));
        const auto slot_bytes = sizeof(decode_slot) + 2 * sizeof(std::unique_ptr<decode_slot>);
        if (workers > (std::numeric_limits<std::size_t>::max() - sizeof(state)) / slot_bytes)
            return std::unexpected(std::string{"BGZF decoder admission size overflows"});
        auto permit = parallelism::memory_admission_controller::instance().try_reserve(
            workers * slot_bytes + sizeof(state));
        while (!permit && workers > 1u) {
            workers = std::max<std::size_t>(1u, workers / 2u);
            permit = parallelism::memory_admission_controller::instance().try_reserve(workers * slot_bytes + sizeof(state));
        }
        if (!permit) return std::unexpected(std::string{"Cannot admit BGZF decoder workers"});
        bgzf_source result;
        result.state_ = std::make_unique<state>();
        result.state_->permit = std::move(*permit);
        result.state_->slots.reserve(workers);
        for (std::size_t index = 0; index < workers; ++index)
            result.state_->slots.push_back(std::make_unique<decode_slot>());
        return result;
    }

    /** Borrow a mapping for zero-copy member framing and parallel decompression. */
    [[nodiscard]] static std::expected<bgzf_source, std::string> try_open_from_mmap(
        mmap_source mapping, const reader_options& options) {
        const auto direct = mapping.direct_view();
        const std::span<const std::uint8_t> bytes{direct.first, direct.second};
        const auto first = inspect_bgzf_member(bytes);
        if (!first) return std::unexpected(first.error());
        auto result = create(options);
        if (!result) return std::unexpected(result.error());
        result->state_->mapping = std::move(mapping);
        result->state_->compressed = bytes;
        return result;
    }

    /** Read exact member frames sequentially while retaining the same parallel decode workers. */
    [[nodiscard]] static std::expected<bgzf_source, std::string> try_open(
        std::string_view path, const reader_options& options) {
        auto source = posix_read_source::try_open(path);
        if (!source) return std::unexpected(source.error());
        auto result = create(options);
        if (!result) return std::unexpected(result.error());
        result->state_->streaming.emplace(std::move(*source));
        return result;
    }

    /** Copy verified decompressed bytes in member order into a caller-owned buffer. */
    [[nodiscard]] source_read_result read_into(std::uint8_t* destination, const std::size_t capacity) {
        if (!state_ || !state_->error.empty()) return {0, true, true};
        std::size_t copied{};
        while (copied < capacity) {
            if (state_->slot_index == state_->ready_slots) {
                if (!state_->refill()) return {copied, true, true};
                if (state_->ready_slots == 0) return {copied, true, false};
            }
            const auto& slot = *state_->slots[state_->slot_index];
            const auto available = slot.size - state_->slot_offset;
            const auto count = std::min(available, capacity - copied);
            std::copy_n(slot.output.data() + state_->slot_offset, count, destination + copied);
            copied += count;
            state_->slot_offset += count;
            if (state_->slot_offset == slot.size) {
                ++state_->slot_index;
                state_->slot_offset = 0;
            }
        }
        return {copied, false, false};
    }

    /** Return the first framing, decompression or integrity failure. */
    [[nodiscard]] std::string last_error_message() const {
        return state_ ? state_->error : "BGZF source is not open";
    }

    /** Canonical BAM requires exactly one standard final EOF member. */
    [[nodiscard]] bool has_canonical_eof() const noexcept {
        return state_ && (state_->streaming ? state_->streaming_eof : state_->compressed_cursor == state_->compressed.size())
            && state_->final_member_is_eof && state_->eof_markers == 1;
    }
};

} // namespace bioseq::io::alignment
