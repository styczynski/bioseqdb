/** @file Ordered admitted record encoding and publication through the shared IO pool. */
module;
#include <hwy/highway.h>
#include <libassert/assert.hpp>
#include <new>

export module bioseq.io.write_pipeline;

import std;
import bioseq.io.options;
import bioseq.io.file_kind;
import bioseq.io.record_schema;
import bioseq.io.sinks;
import bioseq.io.compression.aligned_byte_buffer;
import bioseq.parallelism.pool;
import bioseq.parallelism.concepts;
import bioseq.parallelism.mode;
import bioseq.parallelism.memory_admission;
import bioseq.common.checked_arithmetic;
import bioseq.common.seqan_adapter;
import bioseq.io.alignment;
import bioseq.io.alignment.sequence_codec;
import bioseq.io.identifier_parser.canonical;
import bioseq.io.identifier_parser.preservation;
import bioseq.sequences.rank_pack;
import bioseq.sequences.simd;
import bioseq.storage.size;
import bioseq.storage.lazy_sequence_view;
import bioseq.sequences;
import bioseq.simd.utils;
import bioseq.sequences.cigar;

export namespace bioseq::io {

/** Public record counts are bounded before any successful file publication. */
inline constexpr std::uint64_t maximum_export_records =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

/** One admitted immutable output block and persistent compression workspace. */
struct output_block {
    parallelism::memory_permit permit;
    std::vector<std::uint8_t> input;
    std::vector<std::uint8_t> compressed;
    alignment::bgzf_encoder encoder;
    std::size_t input_size{}, output_size{};
    std::uint64_t file_offset{};
    std::string error;
};

/** Ordered bounded output uses caller-thread serialization and detached TBB block jobs.
 * The pool never receives a sequence source, PG session, or row owner. All live
 * blocks own admission permits; a full wave drains before any block is reused.
 */
class ordered_output_sink {
    posix_pwrite_sink file_;
    std::vector<std::unique_ptr<output_block>> blocks_;
    sequence_compression compression_{};
    std::size_t filled_{};
    bool finished_{};
    std::string failure_;
    io_caller_checkpoint checkpoint_;
    std::size_t bytes_since_checkpoint_{};
    parallelism::scoped_task_group jobs_;

    /** Append framing only after all ordered block tasks have joined. */
    [[nodiscard]] std::expected<void, std::string> append_final_bytes(std::span<const std::uint8_t> bytes) {
        const auto offset = file_.reserve_offset(bytes.size());
        if (!offset) return std::unexpected(offset.error());
        const auto written = file_.write_at(std::as_bytes(bytes), *offset);
        if (!written) return std::unexpected(written.error());
        return {};
    }

    /** Join encoders, reserve offsets in source order, then issue disjoint writes. */
    [[nodiscard]] std::expected<void, std::string> drain() {
        jobs_.wait();
        checkpoint_();
        for (std::size_t index = 0u; index < filled_; ++index) {
            auto& block = *blocks_[index];
            if (!block.error.empty()) return std::unexpected(block.error);
            const auto offset = file_.reserve_offset(block.output_size);
            if (!offset) return std::unexpected(offset.error());
            block.file_offset = *offset;
        }
        auto& pool = parallelism::global_pool::instance();
        const auto write_blocks = [this](std::size_t begin, std::size_t end) {
            for (auto index = begin; index < end; ++index) {
                auto& block = *blocks_[index];
                const auto bytes = compression_ == sequence_compression::none
                    ? std::span<const std::uint8_t>{block.input}.first(block.output_size)
                    : std::span<const std::uint8_t>{block.compressed}.first(block.output_size);
                const auto written = file_.write_at(std::as_bytes(bytes), block.file_offset);
                if (!written) block.error = written.error();
            }
        };
        if (filled_ > 1u && parallelism::has_parallel_dispatch(pool.get_mode()))
            pool.parallel_for(0u, filled_, 1u, write_blocks);
        else write_blocks(0u, filled_);
        checkpoint_();
        for (std::size_t index = 0u; index < filled_; ++index) {
            auto& block = *blocks_[index];
            if (!block.error.empty()) return std::unexpected(block.error);
            block.input_size = block.output_size = 0u;
        }
        filled_ = 0u;
        if (blocks_.size() > 1u && parallelism::has_parallel_dispatch(pool.get_mode()))
            jobs_ = pool.make_task_group();
        return {};
    }

    /** Hand one filled plain block to a detached compression task. */
    [[nodiscard]] std::expected<void, std::string> submit() {
        auto* block = blocks_[filled_].get();
        if (block->input_size == 0u) return {};
        if (compression_ == sequence_compression::none) block->output_size = block->input_size;
        else {
            const auto framing = compression_;
            auto encode_block = [block, framing] {
                const auto input = std::span<const std::uint8_t>{block->input}.first(block->input_size);
                const auto encoded = framing == sequence_compression::gzip
                    ? block->encoder.template encode<alignment::gzip_member_framing::ordinary>(input, block->compressed)
                    : block->encoder.encode(input, block->compressed);
                if (!encoded) block->error = encoded.error();
                else block->output_size = *encoded;
            };
            if (blocks_.size() > 1u && parallelism::has_parallel_dispatch(
                    parallelism::global_pool::instance().get_mode()))
                jobs_.run(std::move(encode_block));
            else encode_block();
        }
        ++filled_;
        if (filled_ == blocks_.size()) return drain();
        return {};
    }

public:
    /** Create private staging state before accepting any bytes. */
    ordered_output_sink() = default;
    ordered_output_sink(const ordered_output_sink&) = delete;
    ordered_output_sink& operator=(const ordered_output_sink&) = delete;
    /** Join before releasing compressed blocks or their private output file. */
    ~ordered_output_sink() { jobs_.wait(); }

    /** Admit as many bounded worker slots as the shared budget currently allows. */
    [[nodiscard]] static std::expected<std::unique_ptr<ordered_output_sink>, std::string> open(
            std::string_view path, const writer_options& options,
            sequence_compression compression, const concurrency_options& concurrency,
            io_caller_checkpoint checkpoint = {}) {
        checkpoint();
        if (options.chunk_capacity_bytes == 0u)
            return std::unexpected(std::string{"writer chunk capacity must be positive"});
        auto result = std::make_unique<ordered_output_sink>();
        result->checkpoint_ = checkpoint;
        auto opened = posix_pwrite_sink::try_open(path, options.overwrite, options.maximum_output_bytes);
        if (!opened) return std::unexpected(opened.error());
        result->file_ = std::move(*opened);
        result->compression_ = compression;
        auto& pool = parallelism::global_pool::instance();
        auto workers = concurrency.worker_count;
        if (workers == 0u)
            workers = std::max<std::size_t>(1u, parallelism::default_concurrency_limit());
        // Match the reader's first-use bootstrap; an active scheduler keeps its
        // existing arena and policy, including an intentionally serial owner.
        if (!pool.is_active()) pool.configure(parallelism::pool_config{
            .maximum_concurrency = static_cast<unsigned>(std::min<std::size_t>(workers, 64u)),
            .mode = parallelism::pipeline_concurrency_mode::dispatch_parallel});
        workers = std::max<std::size_t>(1u,
            std::min<std::size_t>(workers, pool.maximum_concurrency()));
        if (!parallelism::has_parallel_dispatch(pool.get_mode())) workers = 1u;
        // Plain output has no independent compression work. One staging block
        // avoids dispatch and same-file write contention; formatting may still
        // use the caller's full participant budget in record_format_wave.
        const auto maximum_slots = compression == sequence_compression::none ? std::size_t{1u}
            : std::max(std::size_t{1u}, std::min(workers,
                std::max(std::size_t{1u}, concurrency.io_buffer_pool_size)));
        const auto input_capacity = compression == sequence_compression::none
            ? std::min(options.chunk_capacity_bytes, std::size_t{1024u * 1024u})
            : alignment::bgzf_payload_limit;
        const auto output_capacity = compression == sequence_compression::none ? 0u : alignment::bgzf_member_limit;
        // Compressor allocations acquire their exact existing-ledger permits in the backend.
        const auto retained = sizeof(output_block) + input_capacity + output_capacity;
        auto& admission = parallelism::memory_admission_controller::instance();
        for (std::size_t index = 0u; index < maximum_slots; ++index) {
            auto permit = admission.try_reserve(retained);
            if (!permit) break;
            auto block = std::make_unique<output_block>();
            block->permit = std::move(*permit);
            block->input.resize(input_capacity);
            block->compressed.resize(output_capacity);
            if (compression != sequence_compression::none) {
                const auto initialized = block->encoder.open(options.compression_level);
                if (!initialized) {
                    if (result->blocks_.empty()) return std::unexpected(initialized.error());
                    // Identical earlier slots validated the level and codec bound.
                    // Retire this incomplete slot and keep the admitted worker prefix.
                    break;
                }
            }
            result->blocks_.push_back(std::move(block));
        }
        if (result->blocks_.empty())
            return std::unexpected(std::string{"memory admission cannot reserve one output block"});
        if (result->blocks_.size() > 1u && parallelism::has_parallel_dispatch(pool.get_mode()))
            result->jobs_ = pool.make_task_group();
        return result;
    }

    /** Copy a synchronous fragment into admitted blocks; no source owner escapes. */
    [[nodiscard]] std::expected<void, std::string> append(std::span<const std::uint8_t> bytes) {
        if (!failure_.empty()) return std::unexpected(failure_);
        if (finished_) return std::unexpected(std::string{"output stream is already finished"});
        while (!bytes.empty()) {
            auto& block = *blocks_[filled_];
            constexpr std::size_t checkpoint_bytes = 64u * 1024u;
            const auto count = std::min({bytes.size(), block.input.size() - block.input_size,
                checkpoint_bytes - bytes_since_checkpoint_});
            std::memcpy(block.input.data() + block.input_size, bytes.data(), count);
            block.input_size += count;
            bytes = bytes.subspan(count);
            bytes_since_checkpoint_ += count;
            if (bytes_since_checkpoint_ == checkpoint_bytes) {
                checkpoint_();
                bytes_since_checkpoint_ = 0u;
            }
            if (block.input_size == block.input.size()) {
                const auto scheduled = submit();
                if (!scheduled) { failure_ = scheduled.error(); return std::unexpected(failure_); }
            }
        }
        return {};
    }

    /** Text and binary serializers share one byte stream and one buffer owner. */
    [[nodiscard]] std::expected<void, std::string> append(std::string_view bytes) {
        return append(std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
    }

    /** Drain all work, append the required BGZF terminator, and publish atomically. */
    [[nodiscard]] std::expected<void, std::string> finish() {
        if (finished_) return failure_.empty() ? std::expected<void, std::string>{} : std::unexpected(failure_);
        if (!failure_.empty()) return std::unexpected(failure_);
        if (blocks_[filled_]->input_size != 0u) {
            const auto scheduled = submit();
            if (!scheduled) { failure_ = scheduled.error(); return std::unexpected(failure_); }
        }
        if (filled_ != 0u) {
            const auto drained = drain();
            if (!drained) { failure_ = drained.error(); return std::unexpected(failure_); }
        }
        if (compression_ == sequence_compression::bgzf) {
            const auto written = append_final_bytes(alignment::bgzf_eof_marker);
            if (!written) { failure_ = written.error(); return std::unexpected(failure_); }
        } else if (compression_ == sequence_compression::gzip && bytes_written() == 0u) {
            auto& block = *blocks_.front();
            const auto encoded = block.encoder.template encode<alignment::gzip_member_framing::ordinary>({}, block.compressed);
            if (!encoded) { failure_ = encoded.error(); return std::unexpected(failure_); }
            const auto written = append_final_bytes(std::span<const std::uint8_t>{block.compressed}.first(*encoded));
            if (!written) { failure_ = written.error(); return std::unexpected(failure_); }
        }
        checkpoint_();
        const auto published = file_.publish(checkpoint_);
        if (!published) { failure_ = published.error(); return std::unexpected(failure_); }
        finished_ = true;
        return {};
    }

    /** Join the current compression stage before another bounded CPU wave begins. */
    void wait_for_compression() {
        jobs_.wait();
        // scoped_task_group::wait releases its arena owner; later block submissions need a fresh group.
        auto& pool = parallelism::global_pool::instance();
        if (blocks_.size() > 1u && parallelism::has_parallel_dispatch(pool.get_mode()))
            jobs_ = pool.make_task_group();
        checkpoint_();
    }

    /** Stable reserved byte count remains observable after successful publication. */
    [[nodiscard]] std::uint64_t bytes_written() const noexcept {
        return file_.size_bytes();
    }
};

/** Count deterministic serializer fragments with checked uint64 arithmetic. */
struct output_measurement_sink {
    std::uint64_t bytes{};
    /** Count fixed-width encoded bytes without touching payload; emission validates the source. */
    [[nodiscard]] std::expected<void, std::string> count_only(std::uint64_t count) {
        if (count > std::numeric_limits<std::uint64_t>::max() - bytes)
            return std::unexpected(std::string{"serialized record byte size overflows"});
        bytes += count;
        return {};
    }
    /** Measure binary fragments without allocating output. */
    [[nodiscard]] std::expected<void, std::string> append(std::span<const std::uint8_t> input) {
        return count_only(input.size());
    }
    /** Measure text fragments through the same checked counter. */
    [[nodiscard]] std::expected<void, std::string> append(std::string_view input) {
        return append(std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(input.data()), input.size()});
    }
};

/** Insert FASTA line breaks at stable logical symbol boundaries. */
template<class Sink>
struct fasta_body_sink {
    Sink& destination;
    std::size_t width{}, column{};
    /** Consume one bounded ASCII sequence tile without a full decoded sequence. */
    [[nodiscard]] std::expected<void, std::string> append(std::span<const std::uint8_t> bytes) {
        while (!bytes.empty()) {
            if (width != 0u && column == width) {
                const auto newline = destination.append("\n");
                if (!newline) return newline;
                column = 0u;
            }
            const auto count = width == 0u ? bytes.size() : std::min(bytes.size(), width - column);
            const auto written = destination.append(bytes.first(count));
            if (!written) return written;
            if (width != 0u) column += count;
            bytes = bytes.subspan(count);
        }
        return {};
    }
};

/** Pick the canonical scalar discriminator without serializing biological columns. */
[[nodiscard]] inline char metadata_discriminator(column_value_kind kind) {
    switch (kind) {
        case column_value_kind::signed_integer: return 'i';
        case column_value_kind::unsigned_integer: return 'u';
        case column_value_kind::float32: case column_value_kind::float64: return 'f';
        case column_value_kind::boolean: return 'b';
        case column_value_kind::bytes: return 'x';
        case column_value_kind::json: return 'j';
        default: return 's';
    }
}

/** Identify quality encodings whose standard wire scores cannot reconstruct original ranks. */
[[nodiscard]] inline bool quality_requires_preservation(const storage::borrowed_rank_window_source& source) {
    bool required = true;
    if (source.alphabet_code >= supported_storage_alphabets::alphabet_count) return true;
    supported_storage_alphabets::dispatch_alphabet_by_code(source.alphabet_code,
        [&required]<class Alphabet>() {
            if constexpr (sequences::quality_alphabet<Alphabet>)
                required = sequences::quality_alphabet_traits<Alphabet>::legacy;
        });
    return required;
}

/** Emit one canonical FASTA/Q row, preserving original alphabet and scalar types. */
template<class Sink>
[[nodiscard]] std::expected<void, std::string> emit_text_record(Sink& sink,
        const export_record_view& original, sequence_file_format format,
        const writer_options& options, bioseq::simd::sequence_rank_transform_kernel transform,
        std::optional<std::uint8_t> absent_sequence_alphabet = {},
        std::optional<std::uint8_t> absent_quality_alphabet = {}) {
    using namespace identifier_parser;
    const auto alphabet = original.sequence ? std::optional{original.sequence->alphabet_code} : absent_sequence_alphabet;
    if (!alphabet || *alphabet >= supported_biological_alphabets::alphabet_count)
        return std::unexpected(std::string{"FASTA/FASTQ needs a bound biological sequence alphabet, including for NULL values"});
    if (format == sequence_file_format::fastq && !original.quality && !absent_quality_alphabet)
        return std::unexpected(std::string{"FASTQ quality column is missing; select or map a typed quality column (SQL NULL is allowed)"});
    if (format == sequence_file_format::fastq && original.sequence && original.quality
        && original.quality->symbol_count != original.sequence->symbol_count)
        return std::unexpected(std::string{"FASTQ present quality must have exactly the sequence length"});
    const auto fields_bytes = common::checked_mul(original.metadata.size(), sizeof(canonical_field_view));
    if (!fields_bytes) return std::unexpected(std::string{"metadata field capacity overflows"});
    auto fields_permit = parallelism::memory_admission_controller::instance().try_reserve(*fields_bytes);
    if (!fields_permit) return std::unexpected(std::string{"metadata field views exceed available memory"});
    std::vector<canonical_field_view> fields;
    fields.reserve(original.metadata.size());
    for (const auto& field : original.metadata) {
        if (field.kind == column_value_kind::biological_sequence || field.kind == column_value_kind::quality_sequence
            || field.kind == column_value_kind::cigar_sequence || field.kind == column_value_kind::alignment_record)
            continue;
        fields.push_back({field.name, field.is_null ? std::optional<std::string_view>{}
            : std::optional<std::string_view>{field.bytes}, metadata_discriminator(field.kind)});
    }
    auto row = original;
    if (format == sequence_file_format::fasta) row.quality.reset();
    preservation_options preservation{.text_format = true,
        .absent_sequence_alphabet = absent_sequence_alphabet,
        .absent_quality_alphabet = format == sequence_file_format::fastq ? absent_quality_alphabet : std::nullopt};
    if (row.quality) {
        if (quality_requires_preservation(*row.quality) || !row.sequence)
            preservation.quality_storage = preservation_storage::rank_hex;
    }
    if (auto written = sink.append(format == sequence_file_format::fasta ? ">" : "@"); !written) return written;
    const auto header = append_canonical_header(sink, row.header.value_or(std::string_view{}),
        supported_biological_alphabets::name_of_alphabet_code(*alphabet),
        [&row, &preservation](auto& encoded) { return append_preservation_record(encoded, row, preservation); }, fields);
    if (!header) return header;
    if (auto written = sink.append("\n"); !written) return written;
    if (row.sequence) {
        std::optional<std::expected<std::size_t, std::string>> sequence_written;
        fasta_body_sink<Sink> body{sink, format == sequence_file_format::fasta ? options.fasta_line_width : 0u};
        supported_biological_alphabets::dispatch_alphabet_by_code(row.sequence->alphabet_code,
            [&row, &body, transform, &sequence_written]<class Alphabet>() {
                sequence_written = alignment::emit_native_sequence<Alphabet, false, false>(body, *row.sequence, transform);
            });
        if (!sequence_written) return std::unexpected(std::string{"unsupported sequence alphabet"});
        if (!*sequence_written) return std::unexpected(sequence_written->error());
    }
    if (auto written = sink.append("\n"); !written) return written;
    if (format == sequence_file_format::fastq) {
        if (auto written = sink.append("+\n"); !written) return written;
        if (row.quality && row.sequence) {
            std::optional<std::expected<std::size_t, std::string>> quality_written;
            if (row.quality->alphabet_code >= supported_storage_alphabets::alphabet_count)
                return std::unexpected(std::string{"unsupported quality alphabet"});
            supported_storage_alphabets::dispatch_alphabet_by_code(row.quality->alphabet_code,
                [&row, &sink, transform, &quality_written]<class Alphabet>() {
                    if constexpr (sequences::quality_alphabet<Alphabet>)
                        quality_written = alignment::emit_quality<Alphabet, false, true>(sink, *row.quality, false, false, transform);
                });
            if (!quality_written) return std::unexpected(std::string{"unsupported quality alphabet"});
            if (!*quality_written) return std::unexpected(quality_written->error());
        } else if (row.sequence) {
            std::array<std::uint8_t, bioseq::simd::sequence_rank_io_chunk_capacity> unknown{};
            unknown.fill(static_cast<std::uint8_t>('!'));
            for (std::uint64_t position = 0u; position < row.sequence->symbol_count;) {
                const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(unknown.size(), row.sequence->symbol_count - position));
                if (auto written = sink.append(std::span<const std::uint8_t>{unknown}.first(count)); !written) return written;
                position += count;
            }
        }
        if (auto written = sink.append("\n"); !written) return written;
    }
    return {};
}

/** Emit alignment sequence in its required native orientation using bounded alphabet dispatch. */
template<bool Packed, class Sink>
[[nodiscard]] std::expected<void, std::string> emit_alignment_sequence(Sink& sink,
        const export_record_view& row, bool reverse, bioseq::simd::sequence_rank_transform_kernel transform) {
    if (!row.sequence || row.sequence->symbol_count == 0u) {
        if constexpr (!Packed) return sink.append("*");
        return {};
    }
    if (row.sequence->alphabet_code >= supported_biological_alphabets::alphabet_count)
        return std::unexpected(std::string{"alignment sequence alphabet is not biological"});
    std::optional<std::expected<std::size_t, std::string>> outcome;
    supported_biological_alphabets::dispatch_alphabet_by_code(row.sequence->alphabet_code,
        [&sink, &row, reverse, transform, &outcome]<class Alphabet>() {
            if constexpr (seqan3::aminoacid_alphabet<Alphabet>) {
                if (reverse) outcome = alignment::emit_protein_sequence<Alphabet, true, Packed>(sink, *row.sequence, transform);
                else outcome = alignment::emit_protein_sequence<Alphabet, false, Packed>(sink, *row.sequence, transform);
            } else {
                if (reverse) outcome = alignment::emit_native_sequence<Alphabet, true, Packed>(sink, *row.sequence, transform);
                else outcome = alignment::emit_native_sequence<Alphabet, false, Packed>(sink, *row.sequence, transform);
            }
        });
    if (!outcome) return std::unexpected(std::string{"alignment sequence alphabet is unsupported"});
    if (!*outcome) return std::unexpected(outcome->error());
    return {};
}

/** Emit standard numeric/text qualities, or the format's truthful missing marker. */
template<bool Packed, class Sink>
[[nodiscard]] std::expected<void, std::string> emit_alignment_quality(Sink& sink,
        const export_record_view& row, bool reverse, bool protein_triplets,
        std::size_t native_length, bioseq::simd::sequence_rank_transform_kernel transform) {
    if (!row.sequence || !row.quality || row.quality->symbol_count == 0u) {
        if constexpr (!Packed) return sink.append("*");
        constexpr auto capacity = bioseq::simd::sequence_rank_io_chunk_capacity;
        std::array<std::uint8_t, capacity> absent{};
        absent.fill(255u);
        for (std::size_t position = 0u; position < native_length;) {
            const auto count = std::min(capacity, native_length - position);
            const auto written = sink.append(std::span<const std::uint8_t>{absent}.first(count));
            if (!written) return written;
            position += count;
        }
        return {};
    }
    if (!row.sequence || row.quality->symbol_count != row.sequence->symbol_count
        || row.quality->alphabet_code >= supported_storage_alphabets::alphabet_count)
        return std::unexpected(std::string{"alignment quality length/alphabet differs from sequence"});
    std::optional<std::expected<std::size_t, std::string>> outcome;
    supported_storage_alphabets::dispatch_alphabet_by_code(row.quality->alphabet_code,
        [&sink, &row, reverse, protein_triplets, transform, &outcome]<class Alphabet>() {
            if constexpr (sequences::quality_alphabet<Alphabet>) {
                if (reverse) outcome = alignment::emit_quality<Alphabet, true, !Packed>(
                    sink, *row.quality, protein_triplets, !Packed, transform);
                else outcome = alignment::emit_quality<Alphabet, false, !Packed>(
                    sink, *row.quality, protein_triplets, !Packed, transform);
            }
        });
    if (!outcome) return std::unexpected(std::string{"alignment quality alphabet is unsupported"});
    if (!*outcome) return std::unexpected(outcome->error());
    return {};
}


/** Exact generated NM/MD storage; short tags stay local, large tags use admitted/file storage. */
class derived_alignment_tags {
    admitted_byte_buffer md_bytes_;
    // Only the initialized prefix is exposed; no speculative memory reservation
    // remains live while query/reference callbacks execute.
    std::array<std::byte, 512> inline_md_;
    std::size_t inline_md_size_{};
    bool md_streamed_{};
    std::array<std::byte, 4> nm_bytes_{};
    std::uint64_t differences_{};
    bool active_{};
public:
    /** Keep every borrowed field address inside its one formatting scope. */
    derived_alignment_tags() = default;
    derived_alignment_tags(const derived_alignment_tags&) = delete;
    derived_alignment_tags& operator=(const derived_alignment_tags&) = delete;
    derived_alignment_tags(derived_alignment_tags&&) = delete;
    derived_alignment_tags& operator=(derived_alignment_tags&&) = delete;

    /** Fill local MD storage, transferring its prefix once on admitted streaming overflow. */
    [[nodiscard]] std::expected<void, std::string> append(std::span<const std::uint8_t> bytes) {
        if (active_) return std::unexpected(std::string{"derived alignment tags are already sealed"});
        if (!md_streamed_ && bytes.size() <= inline_md_.size() - inline_md_size_) {
            if (!bytes.empty())
                std::memcpy(inline_md_.data() + inline_md_size_, bytes.data(), bytes.size());
            inline_md_size_ += bytes.size();
            return {};
        }
        if (!md_streamed_) {
            if (const auto prefix = md_bytes_.append(std::span<const std::byte>{inline_md_}.first(inline_md_size_));
                !prefix) return prefix;
            md_streamed_ = true;
        }
        return md_bytes_.append(std::as_bytes(bytes));
    }

    /** Derive real native-base tags once through independent bounded alphabet adapters. */
    template<class Resolved>
    [[nodiscard]] std::expected<void, std::string> prepare(const export_record_view& row,
            const Resolved& resolved, bool reverse,
            bioseq::simd::sequence_rank_transform_kernel transform) {
        if (!row.sequence || !row.reference_sequence || (resolved.native.core.flags & 4u) != 0u) return {};
        if (!resolved.reference_length || *resolved.reference_length != row.reference_sequence->symbol_count)
            return std::unexpected(std::string{"MD/NM reference sequence length disagrees with dictionary"});
        const auto reference_protein = alignment::protein_alphabet_code(row.reference_sequence->alphabet_code);
        if (!reference_protein || *reference_protein != resolved.protein_triplets)
            return std::unexpected(std::string{"MD/NM query and reference projection domains differ"});
        const auto query = alignment::alignment_wire_source::make(*row.sequence, reverse, transform);
        const auto reference = alignment::alignment_wire_source::make(*row.reference_sequence, false, transform);
        if (!query) return std::unexpected(query.error());
        if (!reference) return std::unexpected(reference.error());
        const auto differences = alignment::derive_alignment_md(*this, *query, *reference,
            resolved.native.cigar, static_cast<std::uint64_t>(resolved.native.core.position), resolved.reference_is_circular);
        if (!differences) return std::unexpected(differences.error());
        if (*differences > std::numeric_limits<std::uint32_t>::max())
            return std::unexpected(std::string{"derived NM exceeds native representation limits"});
        differences_ = *differences;
        if (md_streamed_)
            if (const auto sealed = md_bytes_.seal(); !sealed) return sealed;
        for (std::size_t index{}; index < nm_bytes_.size(); ++index)
            nm_bytes_[index] = static_cast<std::byte>((differences_ >> (8u * index)) & 255u);
        active_ = true;
        return {};
    }

    /** Whether real query/reference data made the standard tags derivable. */
    [[nodiscard]] bool active() const noexcept { return active_; }
    /** Borrow exact generated fields; this owner outlives preservation and native serialization. */
    [[nodiscard]] std::array<auxiliary_field_view, 2> fields() const {
        const auto md = !active_ ? std::span<const std::byte>{}
            : md_streamed_ ? md_bytes_.view()
                : std::span<const std::byte>{inline_md_}.first(inline_md_size_);
        return {auxiliary_field_view{{'N', 'M'}, differences_ <= std::numeric_limits<std::int32_t>::max() ? 'i' : 'I', 0, nm_bytes_},
            auxiliary_field_view{{'M', 'D'}, 'Z', 0, md}};
    }
};

/** Emit every measured native field plus the exact original-record preservation payload. */
template<class Sink, class Resolved>
[[nodiscard]] std::expected<void, std::string> emit_alignment_record(Sink& sink,
        const export_record_view& row, const Resolved& resolved, sequence_file_format format,
        bioseq::simd::sequence_rank_transform_kernel transform) {
    using namespace alignment;
    using namespace identifier_parser;
    const bool binary = format == sequence_file_format::bam;
    const auto& native = resolved.native;
    const bool reverse = (native.core.flags & 16u) != 0u
        && (!row.alignment || row.alignment->sequence_orientation == record_orientation::original_query);
    if (row.quality && row.sequence && row.quality->symbol_count != row.sequence->symbol_count)
        return std::unexpected(std::string{"alignment quality length differs from original sequence"});
    derived_alignment_tags derived;
    if (const auto prepared = derived.prepare(row, resolved, reverse, transform); !prepared) return prepared;
    const auto derived_fields = derived.fields();
    const auto original_auxiliary = row.alignment ? row.alignment->auxiliary : std::span<const auxiliary_field_view>{};
    const auto auxiliary_capacity = common::checked_add(original_auxiliary.size(), derived.active() ? std::size_t{2u} : std::size_t{0u});
    if (!auxiliary_capacity) return std::unexpected(std::string{"auxiliary view count overflows"});
    const auto auxiliary_view_bytes = common::checked_mul(*auxiliary_capacity, sizeof(auxiliary_field_view));
    if (!auxiliary_view_bytes) return std::unexpected(std::string{"auxiliary view size overflows"});
    parallelism::memory_permit views_permit;
    std::array<auxiliary_field_view, 8> inline_auxiliary;
    std::vector<auxiliary_field_view> overflow_auxiliary;
    std::span<auxiliary_field_view> auxiliary_storage;
    if (*auxiliary_capacity <= inline_auxiliary.size()) {
        auxiliary_storage = std::span{inline_auxiliary}.first(*auxiliary_capacity);
    } else {
        auto admitted = parallelism::memory_admission_controller::instance().try_reserve(*auxiliary_view_bytes);
        if (!admitted) return std::unexpected(std::string{"auxiliary views exceed available memory"});
        views_permit = std::move(*admitted);
        overflow_auxiliary.resize(*auxiliary_capacity);
        auxiliary_storage = overflow_auxiliary;
    }
    std::size_t projected_auxiliary_size{};
    std::bitset<52 * 62> occupied_tags;
    std::uint64_t auxiliary_bytes{};
    for (const auto& field : original_auxiliary) {
        const auto validated = validate_auxiliary_field(field);
        if (!validated) return std::unexpected(validated.error());
        const auto tag = *auxiliary_tag_index(field.tag);
        if (occupied_tags[tag]) return std::unexpected(std::string{"alignment contains duplicate auxiliary tags"});
        occupied_tags.set(tag);
        if (!retain_original_native_auxiliary(field, resolved.protein_triplets, !binary,
                native.mate_projection_suppressed)) continue;
        if (derived.active() && derived_alignment_auxiliary_tag(field.tag)) {
            const auto& generated = derived_fields[field.tag[0] == 'N' ? 0u : 1u];
            if (!resolved.protein_triplets && !auxiliary_logically_equal(field, generated))
                return std::unexpected(std::string{"supplied NM/MD contradicts the actual sequence/reference alignment"});
            continue;
        }
        const auto measured = measure_bam_auxiliary(field);
        if (!measured || *measured > std::numeric_limits<std::uint64_t>::max() - auxiliary_bytes)
            return std::unexpected(measured ? std::string{"auxiliary size overflows"} : measured.error());
        auxiliary_bytes += *measured;
        auxiliary_storage[projected_auxiliary_size++] = field;
    }
    if (derived.active()) {
        for (const auto& field : derived_fields) {
            const auto measured = measure_bam_auxiliary(field);
            if (!measured || *measured > std::numeric_limits<std::uint64_t>::max() - auxiliary_bytes)
                return std::unexpected(measured ? std::string{"derived auxiliary size overflows"} : measured.error());
            auxiliary_bytes += *measured;
            auxiliary_storage[projected_auxiliary_size++] = field;
        }
    }
    const std::span<const auxiliary_field_view> projected_auxiliary =
        auxiliary_storage.first(projected_auxiliary_size);
    preservation_options preservation{
        .synthetic_codons = resolved.protein_triplets,
        .projected_flag = native.core.flags,
        .projected_qname = resolved.qname,
        // Preserve resolved Boolean mappings and group election before native mate projection.
        .original_flag = resolved.flags,
        .sequence_storage = resolved.protein_triplets ? preservation_storage::rank_hex : preservation_storage::native,
        .quality_storage = preservation_storage::native,
        .absent_sequence_alphabet = resolved.sequence_alphabet,
        .absent_quality_alphabet = resolved.quality_alphabet,
        .projected_auxiliary = projected_auxiliary};
    if (row.quality && (!row.sequence || quality_requires_preservation(*row.quality)
        || (!binary && !resolved.protein_triplets && row.quality->symbol_count == 1u)))
        preservation.quality_storage = preservation_storage::rank_hex;
    if (binary) {
        output_measurement_sink payload_measure;
        const auto measured_payload = append_preservation_record(payload_measure, row, preservation);
        if (!measured_payload) return measured_payload;
        if (payload_measure.bytes > std::numeric_limits<std::uint64_t>::max() - 4u
            || auxiliary_bytes > std::numeric_limits<std::uint64_t>::max() - payload_measure.bytes - 4u)
            return std::unexpected(std::string{"preservation payload size overflows"});
        auxiliary_bytes += payload_measure.bytes + 4u;
    }
    if (binary) {
        const auto measured = measure_bam_record(resolved.qname, native.core, native.cigar_size, auxiliary_bytes);
        if (!measured) return std::unexpected(measured.error());
        if (auto written = emit_bam_record_prefix(sink, resolved.qname, native.core, *measured); !written) return written;
        if (auto written = emit_bam_cigar(sink, native.cigar, native.cigar_size, native.core, measured->long_cigar); !written) return written;
        if (auto written = emit_alignment_sequence<true>(sink, row, reverse, transform); !written) return written;
        if (auto written = emit_alignment_quality<true>(sink, row, reverse, resolved.protein_triplets,
                static_cast<std::size_t>(native.core.sequence_length), transform); !written) return written;
        for (const auto& field : projected_auxiliary)
            if (auto written = emit_bam_auxiliary(sink, field); !written) return written;
        constexpr std::array<std::uint8_t, 3> preservation_tag{'b', 'q', 'Z'};
        if (auto written = sink.append(preservation_tag); !written) return written;
        if (auto written = append_preservation_record(sink, row, preservation); !written) return written;
        constexpr std::array<std::uint8_t, 1> terminator{};
        if (auto written = sink.append(terminator); !written) return written;
        if (measured->long_cigar)
            if (auto written = emit_bam_long_cigar(sink, native.cigar, native.cigar_size); !written) return written;
    } else {
        if (auto written = emit_sam_record_prefix(sink, resolved.qname, resolved.reference_name,
                resolved.mate_reference_name, native.core, native.cigar, native.cigar_size); !written) return written;
        if (auto written = emit_alignment_sequence<false>(sink, row, reverse, transform); !written) return written;
        if (auto written = sink.append("\t"); !written) return written;
        if (auto written = emit_alignment_quality<false>(sink, row, reverse, resolved.protein_triplets,
                static_cast<std::size_t>(native.core.sequence_length), transform); !written) return written;
        for (const auto& field : projected_auxiliary) {
            if (auto written = emit_sam_auxiliary(sink, field); !written) return written;
        }
        if (auto written = sink.append("\tbq:Z:"); !written) return written;
        if (auto written = append_preservation_record(sink, row, preservation); !written) return written;
        if (auto written = sink.append("\n"); !written) return written;
    }
    return {};
}

/** Caller-only traversal view adds cancellation to the existing bounded cursors.
 * No sequence, CIGAR or scalar payload is copied; descriptors retain original
 * owners. This adapter is scoped to serial formatting/replay/measurement only.
 */
struct checkpointed_export_record {
    struct rank_window {
        storage::borrowed_rank_window_source source;
        io_caller_checkpoint checkpoint;
        /** Poll once per bounded native-rank read, then use the checked source. */
        [[nodiscard]] static const std::uint8_t* read(const void* context,
                storage::sequence_size_t from, std::size_t count, std::span<std::uint8_t> scratch) {
            const auto& window = *static_cast<const rank_window*>(context);
            window.checkpoint();
            const auto ranks = window.source.read_checked(from, count, scratch);
            return ranks ? ranks->data() : nullptr;
        }
    };
    struct cigar_window {
        borrowed_cigar_run_source source;
        io_caller_checkpoint checkpoint;
        /** Poll once per bounded canonical-run tile without changing coalescing. */
        [[nodiscard]] static std::expected<cigar_run_tile, cigar_error> read(
                const void* context, std::uint64_t token, std::span<cigar_run> scratch) {
            const auto& window = *static_cast<const cigar_window*>(context);
            window.checkpoint();
            if (window.source.read_tile == nullptr)
                return std::unexpected(cigar_error{cigar_error_code::invalid_encoding});
            return window.source.read_tile(window.source.context, token, scratch);
        }
    };
    export_record_view row;
    std::array<rank_window, 3> rank_windows;
    cigar_window cigar;

    /** Borrow source payloads and attach checks only when the caller requested them. */
    checkpointed_export_record(const export_record_view& source, io_caller_checkpoint checkpoint)
        : row(source) {
        if (checkpoint.poll == nullptr) return;
        const std::array slots{std::addressof(row.sequence), std::addressof(row.quality),
            std::addressof(row.reference_sequence)};
        for (std::size_t index{}; index < slots.size(); ++index) {
            if (!*slots[index]) continue;
            auto& view = **slots[index];
            rank_windows[index] = {view, checkpoint};
            view.context = std::addressof(rank_windows[index]);
            view.read_at = rank_window::read;
            view.worker_shareable = false;
        }
        if (row.alignment && row.alignment->cigar) {
            cigar = {*row.alignment->cigar, checkpoint};
            row.alignment->cigar = borrowed_cigar_run_source{std::addressof(cigar), cigar_window::read, false};
        }
    }
    checkpointed_export_record(const checkpointed_export_record&) = delete;
    checkpointed_export_record& operator=(const checkpointed_export_record&) = delete;
    checkpointed_export_record(checkpointed_export_record&&) = delete;
    checkpointed_export_record& operator=(checkpointed_export_record&&) = delete;
};

/** Append replay fragments to an unpublished positioned file. */
struct replay_byte_sink {
    posix_pwrite_sink& file;
    /** Preserve exact bytes and surface short-write failures through the common sink. */
    [[nodiscard]] std::expected<void, std::string> append(std::span<const std::uint8_t> bytes) {
        const auto written = file.append_bytes(std::as_bytes(bytes));
        if (!written) return std::unexpected(written.error());
        return {};
    }
    /** Text metadata shares the same replay owner as compact sequence words. */
    [[nodiscard]] std::expected<void, std::string> append(std::string_view bytes) {
        return append({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
    }
};

/** Pack one source directly to the replay file through bounded native-rank windows. */
template<sequence_alphabet Alphabet>
[[nodiscard]] std::expected<std::uint64_t, std::string> append_replay_sequence(
        posix_pwrite_sink& file, const storage::borrowed_rank_window_source& source) {
    using configuration = sequence_custom_alphabet<Alphabet, std::uint64_t>;
    using traits = typename compact_sequence_range<configuration>::traits;
    compact_file_word_buffer words{file};
    sequences::checked_rank_pack_sink<configuration, sequences::invalid_rank_policy::reject,
        sequences::pack_capacity_policy::grow_vector, compact_file_unit_sink> writer{compact_file_unit_sink{words}};
    std::array<std::uint8_t, bioseq::simd::sequence_rank_io_chunk_capacity> scratch{};
    for (std::uint64_t position = 0u; position < source.symbol_count;) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(scratch.size(), source.symbol_count - position));
        const auto ranks = source.read_checked(position, count, scratch);
        if (!ranks) return std::unexpected(std::string{"cannot read sequence for compact replay"});
        const auto status = bioseq::simd::active_target::append_native_ranks_chunk(*ranks, writer);
        if (status != sequences::rank_pack_status::ok)
            return std::unexpected(words.error.empty() ? std::string{"compact replay rank packing failed"} : words.error);
        position += count;
    }
    const auto required = source.symbol_count / traits::letters_per_unit
        + static_cast<std::uint64_t>(source.symbol_count % traits::letters_per_unit != 0u);
    const auto finished = writer.finish(source.symbol_count, required);
    if (!finished || !words.flush())
        return std::unexpected(words.error.empty() ? std::string{"compact replay sequence length mismatch"} : words.error);
    return required * sizeof(std::uint64_t);
}

/** One bounded detached record job; the caller retains all borrowed scalar backing. */
struct record_format_job {
    parallelism::memory_permit permit;
    export_record_view row;
    export_record_view original_row;
    std::optional<alignment::resolved_alignment_record> resolved;
    std::array<std::unique_ptr<record_sequence_owner>, 3> sequences;
    /** Preserve global reference coordinates while owning only the aligned original-symbol interval. */
    struct reference_window {
        storage::borrowed_rank_window_source compact;
        std::uint64_t begin{}, end{}, full_length{};
        /** Reject any read outside the detached interval before translating to compact-local coordinates. */
        [[nodiscard]] static const std::uint8_t* read(const void* context, std::uint64_t from,
                std::size_t count, std::span<std::uint8_t> scratch) {
            const auto& window = *static_cast<const reference_window*>(context);
            if (from < window.begin || from > window.end || count > window.end - from) return nullptr;
            const auto ranks = window.compact.read_checked(from - window.begin, count, scratch);
            return ranks ? ranks->data() : nullptr;
        }
        /** Expose full dictionary geometry without claiming resident storage outside the admitted interval. */
        [[nodiscard]] storage::borrowed_rank_window_source borrow() const {
            return {this, &read, full_length, compact.alphabet_code, storage::sequence_transform::identity, true};
        }
    } reference;
    std::vector<cigar_run> cigar_runs;
    cigar_span_source cigar_source;
    compression::chunk_byte_buffer output;
    std::size_t written{};
    bool retry_serial{};
    std::string error;

    /** Append within the admitted bound; oversized records resume on the serial streaming path. */
    [[nodiscard]] std::expected<void, std::string> append(std::span<const std::uint8_t> bytes) {
        if (bytes.size() > output.size - written) {
            retry_serial = true;
            return std::unexpected(std::string{"record exceeds parallel formatting capacity"});
        }
        std::memcpy(output.data.get() + written, bytes.data(), bytes.size());
        written += bytes.size();
        return {};
    }
    /** Text and binary serializers share the same bounded result. */
    [[nodiscard]] std::expected<void, std::string> append(std::string_view bytes) {
        return append({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
    }

    /** Replace a backend-only recipe using bounded tiles and the existing compact owner. */
    [[nodiscard]] std::expected<void, std::string> detach_sequence(
            std::optional<storage::borrowed_rank_window_source>& source, std::size_t slot) {
        if (!source || source->worker_shareable) return {};
        auto owner = make_record_sequence_owner(source->alphabet_code, source->symbol_count, false);
        if (!owner) return std::unexpected(owner.error());
        std::array<std::uint8_t, bioseq::simd::sequence_rank_io_chunk_capacity> scratch{};
        for (std::uint64_t position{}; position < source->symbol_count;) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(scratch.size(), source->symbol_count - position));
            const auto ranks = source->read_checked(position, count, scratch);
            if (!ranks) return std::unexpected(std::string{"cannot detach backend sequence window"});
            if (auto appended = (*owner)->append(*ranks); !appended) return appended;
            position += count;
        }
        if (auto finished = (*owner)->finish(); !finished) return finished;
        sequences[slot] = std::move(*owner);
        source = sequences[slot]->source();
        return {};
    }
};

/** Bounded formatting wave over neutral immutable views; joins before borrowed owners retire. */
class record_format_wave {
    std::vector<std::unique_ptr<record_format_job>> jobs_;
    parallelism::scoped_task_group tasks_;
    std::size_t maximum_jobs_{}, worker_limit_{}, started_{};
    sequence_file_format format_{};
    const writer_options& options_;
    bioseq::simd::sequence_rank_transform_kernel transform_{};
    std::optional<std::uint8_t> absent_sequence_, absent_quality_;

    /** Partition a frozen wave into at most the requested number of existing-pool tasks. */
    void run_pending() {
        if (!parallelism::has_parallel_dispatch(parallelism::global_pool::instance().get_mode())) return;
        const auto workers = std::min(worker_limit_, jobs_.size());
        started_ = workers;
        for (std::size_t worker{}; worker < workers; ++worker) {
            tasks_.run([this, worker, workers] {
                for (auto index = worker; index < jobs_.size(); index += workers) {
                    auto& scheduled = *jobs_[index];
                    const auto outcome = scheduled.resolved
                        ? emit_alignment_record(scheduled, scheduled.row, *scheduled.resolved, format_, transform_)
                        : emit_text_record(scheduled, scheduled.row, format_, options_, transform_, absent_sequence_, absent_quality_);
                    if (!outcome) scheduled.error = outcome.error();
                }
            });
        }
    }
public:
    /** Use the existing shared pool; one worker leaves the low-overhead serial path enabled. */
    record_format_wave(const concurrency_options& concurrency, sequence_file_format format,
            const writer_options& options, bioseq::simd::sequence_rank_transform_kernel transform,
            std::optional<std::uint8_t> absent_sequence, std::optional<std::uint8_t> absent_quality)
        : format_{format}, options_{options}, transform_{transform},
          absent_sequence_{absent_sequence}, absent_quality_{absent_quality} {
        auto& pool = parallelism::global_pool::instance();
        const auto workers = concurrency.worker_count == 0u ? static_cast<std::size_t>(pool.maximum_concurrency())
            : concurrency.worker_count;
        worker_limit_ = std::min(workers, static_cast<std::size_t>(pool.maximum_concurrency()));
        if (!parallelism::has_parallel_dispatch(pool.get_mode())) worker_limit_ = 1u;
        maximum_jobs_ = worker_limit_ < 2u ? 0u : std::min(std::size_t{16u}, worker_limit_) * 4u;
        if (maximum_jobs_ != 0u) tasks_ = pool.make_task_group();
    }
    /** Wait before any caller-owned backing can be released on an error path. */
    ~record_format_wave() { tasks_.wait(); }
    /** Whether another source row would exceed the bounded admitted wave. */
    [[nodiscard]] bool full() const noexcept { return maximum_jobs_ != 0u && jobs_.size() >= maximum_jobs_; }
    /** Whether source acquisition must release completed jobs before retrying admission. */
    [[nodiscard]] bool empty() const noexcept { return jobs_.empty(); }
    /** Join without emitting; error-scope guards call this before destroying a source batch. */
    void wait() { tasks_.wait(); }

    /** Detach bounded callbacks on the caller, then schedule only plain IO work.
     * A whole reference is never copied: only a bounded linear aligned interval can be detached.
     */
    [[nodiscard]] std::expected<bool, std::string> enqueue(const export_record_view& original,
            alignment::alignment_export_context* context, io_caller_checkpoint checkpoint = {}) {
        if (maximum_jobs_ == 0u || full()) return false;
        constexpr std::size_t output_limit = 512u * 1024u;
        constexpr std::size_t cigar_limit = 1024u;
        constexpr std::uint64_t reference_window_limit = output_limit / 16u;
        const bool detach_reference = original.reference_sequence && !original.reference_sequence->worker_shareable;
        if (detach_reference && (context == nullptr || !original.alignment
            || !original.alignment->target_start || !original.alignment->target_end
            || *original.alignment->target_end < *original.alignment->target_start
            || *original.alignment->target_end > original.reference_sequence->symbol_count
            || *original.alignment->target_end - *original.alignment->target_start > reference_window_limit)) return false;
        std::uint64_t estimate = 8192u;
        const auto charge = [&estimate, output_limit](std::uint64_t count, std::uint64_t multiplier) {
            if (count > (output_limit - std::min<std::uint64_t>(estimate, output_limit)) / multiplier) return false;
            estimate += count * multiplier;
            return estimate <= output_limit;
        };
        // This is a conservative admission estimate, not a wire-size correctness assumption.
        // A serializer exceeding it safely retries once through the streaming path.
        if ((original.sequence && !charge(original.sequence->symbol_count, 16u))
            || (original.quality && !charge(original.quality->symbol_count, 8u))
            || (original.header && !charge(original.header->size(), 18u))
            || (original.qname && !charge(original.qname->size(), 6u))) return false;
        for (const auto& field : original.metadata)
            if (!charge(field.name.size(), 12u) || !charge(field.qualified_type.size(), 6u)
                || !charge(field.bytes.size(), 18u) || !charge(1u, 256u)) return false;
        if (original.alignment) {
            if (!charge(original.alignment->reference_name.size(), 6u)
                || !charge(original.alignment->mate_reference.size(), 6u)) return false;
            for (const auto& field : original.alignment->auxiliary)
                if (!charge(field.payload.size(), 12u) || !charge(1u, 256u)) return false;
        }
        const auto job_bytes = estimate + sizeof(record_format_job) + cigar_limit * sizeof(cigar_run) + 256u;
        const auto context_bytes = context == nullptr ? 0u : context->resolution_admission_bytes(original);
        auto admitted = parallelism::memory_admission_controller::instance().try_reserve(job_bytes + context_bytes);
        if (!admitted) return false;
        // One live admission probe, then independent owners with their original lifetimes.
        // Refusal takes the same drain/serial fallback before invoking any source callback.
        auto job_permit = admitted->split_off(job_bytes);
        io_memory_credit context_credit{std::move(*admitted)};
        auto job = std::make_unique<record_format_job>();
        job->permit = std::move(job_permit);
        job->row = original;
        job->original_row = original;
        // Fresh aligned storage has exactly the admitted capacity. Only the
        // initialized written prefix becomes visible after the worker joins.
        if (const auto allocated = job->output.extend(static_cast<std::size_t>(estimate)); !allocated)
            return false;
        if (context != nullptr) {
            const checkpointed_export_record checked{job->row, checkpoint};
            auto resolved = context->resolve(checked.row, &context_credit);
            context_credit.release();
            if (!resolved) return std::unexpected(resolved.error());
            if (detach_reference && resolved->reference_is_circular) return false;
            resolved->native.cigar.source = job->row.alignment ? job->row.alignment->cigar : std::nullopt;
            job->resolved = std::move(*resolved);
        }
        if (detach_reference) {
            const auto begin = *original.alignment->target_start;
            const auto end = *original.alignment->target_end;
            alignment::alignment_rank_slice slice{*original.reference_sequence, begin, static_cast<std::size_t>(end - begin)};
            std::optional<storage::borrowed_rank_window_source> compact = slice.borrow();
            if (auto detached = job->detach_sequence(compact, 2u); !detached) return false;
            job->reference = {*compact, begin, end, original.reference_sequence->symbol_count};
            job->row.reference_sequence = job->reference.borrow();
        }
        if (auto detached = job->detach_sequence(job->row.sequence, 0u); !detached) return false;
        if (auto detached = job->detach_sequence(job->row.quality, 1u); !detached) return false;
        if (job->row.alignment && job->row.alignment->cigar && !job->row.alignment->cigar->worker_shareable) {
            job->cigar_runs.reserve(cigar_limit);
            borrowed_cigar_cursor cursor{*job->row.alignment->cigar};
            for (;;) {
                const auto run = cursor.next();
                if (!run) return std::unexpected(std::string{"cannot detach backend CIGAR"});
                if (!*run) break;
                if (job->cigar_runs.size() == cigar_limit) return false;
                job->cigar_runs.push_back(**run);
            }
            job->cigar_source.runs = job->cigar_runs;
            job->row.alignment->cigar = job->cigar_source.borrow();
        }
        if (job->resolved && job->row.alignment)
            job->resolved->native.cigar.source = job->row.alignment->cigar;
        jobs_.push_back(std::move(job));
        return true;
    }

    /** Emit completed independent results in source order, releasing jobs only on the caller. */
    template<class SerialWriter>
    [[nodiscard]] std::expected<void, record_error> drain(ordered_output_sink& output, SerialWriter&& serial) {
        output.wait_for_compression();
        if (jobs_.size() >= 4u) run_pending();
        tasks_.wait();
        for (auto& job : jobs_) {
            if (started_ == 0u || job->retry_serial || !job->error.empty()) {
                // Release speculative output/compact copies before retrying under pressure.
                // Replaying the retained immutable row never reruns the source query.
                const auto row = job->original_row;
                job.reset();
                if (auto result = serial(row); !result) return result;
            } else if (auto result = output.append(job->output.view().first(job->written)); !result)
                return std::unexpected(record_error{job->row.ordinal, std::nullopt, result.error()});
            job.reset();
        }
        jobs_.clear();
        started_ = 0u;
        if (maximum_jobs_ != 0u && parallelism::has_parallel_dispatch(
                parallelism::global_pool::instance().get_mode()))
            tasks_ = parallelism::global_pool::instance().make_task_group();
        return {};
    }
};

/** Fixed-width original CIGAR runs in a private replay file; no full run vector is restored. */
struct replay_cigar_source {
    const posix_pwrite_sink* file{};
    std::uint64_t offset{}, run_count{};
    /** Read one bounded tile through the existing canonical run-record decoder. */
    [[nodiscard]] static std::expected<cigar_run_tile, cigar_error> read(const void* context,
            std::uint64_t token, std::span<cigar_run> output) {
        if (context == nullptr)
            return std::unexpected(cigar_error{cigar_error_code::invalid_encoding});
        const auto& source = *static_cast<const replay_cigar_source*>(context);
        if (source.file == nullptr || token > source.run_count
            || source.offset > source.file->size_bytes()
            || source.run_count > (source.file->size_bytes() - source.offset) / sizeof(cigar_run))
            return std::unexpected(cigar_error{cigar_error_code::invalid_encoding});
        constexpr auto capacity = bioseq::simd::sequence_rank_io_chunk_capacity;
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
            std::min(output.size(), capacity), source.run_count - token));
        std::array<std::byte, sizeof(cigar_run) * capacity> bytes;
        const auto read_bytes = source.file->read_at(source.offset + token * sizeof(cigar_run),
            std::span{bytes}.first(count * sizeof(cigar_run)));
        if (!read_bytes || *read_bytes != count * sizeof(cigar_run))
            return std::unexpected(cigar_error{cigar_error_code::invalid_encoding});
        for (std::size_t index = 0u; index < count; ++index) {
            const auto decoded = decode_cigar_run_record<cigar_symbol>(
                std::span{bytes}.subspan(index * sizeof(cigar_run), sizeof(cigar_run)));
            if (!decoded) return std::unexpected(decoded.error());
            output[index] = *decoded;
        }
        return cigar_run_tile{output.first(count), token + count, token + count == source.run_count};
    }
    /** Borrow a fresh rewindable run recipe for measurement or serialization. */
    [[nodiscard]] borrowed_cigar_run_source borrow() const { return {this, &read, true}; }
};

/** Stable private replay row owns restored scalars and borrows immutable compact file ranges. */
struct replayed_record {
    parallelism::memory_permit backing_permit;
    decoded_record decoded;
    std::array<compact_file_rank_source, 3> sequences;
    replay_cigar_source cigar;
};

/** Compact spill is created only when retained admitted source owners exhaust available memory. */
class record_replay_file {
    static constexpr std::uint64_t magic = 0x31594c5045525142ULL;
    static constexpr std::size_t header_words = 36u;
    posix_pwrite_sink file_;
    std::uint64_t cursor_{};
    std::uint64_t maximum_restore_bytes_{};
    bool sealed_{};
public:
    /** Largest bounded scalar reconstruction allocation estimated from observed records. */
    [[nodiscard]] std::uint64_t restore_capacity_bytes() const noexcept { return maximum_restore_bytes_; }

    /** Open another private file in the destination directory; it is never published. */
    [[nodiscard]] static std::expected<record_replay_file, std::string> open(std::string_view output_path) {
        auto opened = posix_pwrite_sink::try_open(output_path, false);
        if (!opened) return std::unexpected(opened.error());
        record_replay_file replay;
        replay.file_ = std::move(*opened);
        return replay;
    }

    /** Stream scalars, original CIGAR and compact sequence words without decoding whole sequences. */
    [[nodiscard]] std::expected<void, std::string> append(const export_record_view& source,
            io_caller_checkpoint checkpoint = {}) {
        const checkpointed_export_record checked{source, checkpoint};
        const auto& original = checked.row;
        if (sealed_) return std::unexpected(std::string{"compact replay is already sealed"});
        const auto header_offset = file_.reserve_offset(header_words * sizeof(std::uint64_t));
        if (!header_offset) return std::unexpected(header_offset.error());
        std::array<std::uint64_t, header_words> header{};
        header[0] = magic;
        header[3] = original.ordinal;
        const auto save_optional = [&header](std::size_t bit, std::size_t index, auto value) {
            if (value) { header[4] |= std::uint64_t{1u} << bit; header[index] = static_cast<std::uint64_t>(*value); }
        };
        save_optional(0u, 5u, original.identity.template_id);
        save_optional(1u, 6u, original.identity.segment_id);
        save_optional(2u, 7u, original.identity.reference_id);
        save_optional(3u, 8u, original.identity.match_group_id);
        save_optional(4u, 9u, original.identity.dp_score);
        save_optional(5u, 10u, original.identity.chain_score);
        header[11] = static_cast<std::uint64_t>(original.identity.classification);
        header[12] = original.identity.completes_match_group;
        header[15] = original.alignment && original.alignment->source_bound_interpretation;
        if (original.alignment)
            for (std::size_t bit = 0u; bit < original.alignment->flag_constraints.size(); ++bit)
                if (original.alignment->flag_constraints[bit]) {
                    header[13] |= std::uint64_t{1u} << bit;
                    if (*original.alignment->flag_constraints[bit]) header[14] |= std::uint64_t{1u} << bit;
                }
        auto scalar_row = original;
        if (scalar_row.alignment) scalar_row.alignment->cigar.reset();
        // Private replay has no native projection yet. Keep both snapshot fields
        // absent and retain an unknown original FLAG rather than inventing unmapped.
        const identifier_parser::preservation_options preservation{};
        replay_byte_sink sink{file_};
        const auto scalar_begin = file_.size_bytes();
        const auto written = identifier_parser::append_preservation_record(sink, scalar_row, preservation);
        if (!written) return written;
        header[2] = file_.size_bytes() - scalar_begin;
        const auto scalar_capacity = common::checked_mul(header[2], std::uint64_t{4u});
        if (!scalar_capacity || *scalar_capacity > std::numeric_limits<std::uint64_t>::max() - sizeof(replayed_record) - 16384u)
            return std::unexpected(std::string{"compact replay restore capacity overflows"});
        maximum_restore_bytes_ = std::max(maximum_restore_bytes_, *scalar_capacity + sizeof(replayed_record) + 16384u);
        const std::array sources{original.sequence, original.quality, original.reference_sequence};
        for (std::size_t index = 0u; index < sources.size(); ++index) {
            if (!sources[index]) continue;
            const auto& source = *sources[index];
            const auto first = 16u + 5u * index;
            header[first] = 1u;
            header[first + 1u] = source.alphabet_code;
            header[first + 2u] = source.symbol_count;
            header[first + 3u] = file_.size_bytes();
            std::optional<std::expected<std::uint64_t, std::string>> encoded;
            if (source.alphabet_code >= supported_storage_alphabets::alphabet_count)
                return std::unexpected(std::string{"unsupported replay alphabet"});
            supported_storage_alphabets::dispatch_alphabet_by_code(source.alphabet_code,
                [this, &source, &encoded]<class Alphabet>() {
                    if constexpr (!cigar_operation_alphabet<Alphabet>) encoded = append_replay_sequence<Alphabet>(file_, source);
                });
            if (!encoded) return std::unexpected(std::string{"unsupported replay alphabet"});
            if (!*encoded) return std::unexpected(encoded->error());
            header[first + 4u] = **encoded;
        }
        if (original.alignment && original.alignment->cigar) {
            header[31] = 1u;
            header[32] = file_.size_bytes();
            borrowed_cigar_cursor cursor{*original.alignment->cigar};
            std::array<std::byte, sizeof(cigar_run) * bioseq::simd::sequence_rank_io_chunk_capacity> bytes;
            std::size_t used{};
            for (;;) {
                const auto run = cursor.next();
                if (!run) return std::unexpected(std::string{"cannot read CIGAR for compact replay"});
                if (!*run) break;
                const auto encoded = encode_cigar_run_record(**run);
                std::copy(encoded.begin(), encoded.end(), bytes.begin() + used);
                used += encoded.size();
                ++header[33];
                if (used == bytes.size()) {
                    if (auto appended = file_.append_bytes(bytes); !appended)
                        return std::unexpected(appended.error());
                    used = 0u;
                }
            }
            if (used != 0u)
                if (auto appended = file_.append_bytes(std::span{bytes}.first(used)); !appended)
                    return std::unexpected(appended.error());
        }
        header[1] = file_.size_bytes() - *header_offset;
        if constexpr (std::endian::native == std::endian::big)
            for (auto& word : header) word = std::byteswap(word);
        const auto committed = file_.write_at(std::as_bytes(std::span{header}), *header_offset);
        if (!committed) return std::unexpected(committed.error());
        return {};
    }

    /** Seal before exposing any immutable replay source to consumers. */
    [[nodiscard]] std::expected<void, std::string> seal() {
        const auto result = file_.seal();
        if (result) sealed_ = true;
        return result;
    }

    /** Restore one row's scalar owner and attach bounded compact-file recipes. */
    [[nodiscard]] std::expected<std::optional<std::unique_ptr<replayed_record>>, std::string> next() {
        if (!sealed_) return std::unexpected(std::string{"compact replay is not sealed"});
        if (cursor_ == file_.size_bytes()) return std::optional<std::unique_ptr<replayed_record>>{};
        std::array<std::uint64_t, header_words> header{};
        const auto loaded = file_.read_at(cursor_, std::as_writable_bytes(std::span{header}));
        if (!loaded || *loaded != sizeof(header)) return std::unexpected(std::string{"compact replay header is truncated"});
        if constexpr (std::endian::native == std::endian::big)
            for (auto& word : header) word = std::byteswap(word);
        if (header[0] != magic || header[1] < sizeof(header) || header[1] > file_.size_bytes() - cursor_
            || header[2] > header[1] - sizeof(header) || header[2] > std::numeric_limits<std::size_t>::max())
            return std::unexpected(std::string{"compact replay header geometry is invalid"});
        auto scratch_permit = parallelism::memory_admission_controller::instance().try_reserve(sizeof(replayed_record));
        if (!scratch_permit) return std::unexpected(std::string{"cannot admit compact replay metadata decoding"});
        const auto mapped = file_.map_read_at(cursor_ + sizeof(header), static_cast<std::size_t>(header[2]));
        if (!mapped) return std::unexpected(mapped.error());
        auto owner = std::make_unique<replayed_record>();
        owner->backing_permit = std::move(*scratch_permit);
        const auto json_bytes = mapped->bytes();
        const auto decoded = identifier_parser::decode_preservation_record(
            {reinterpret_cast<const char*>(json_bytes.data()), json_bytes.size()}, owner->decoded, false, json_bytes.size());
        if (!decoded) return std::unexpected(decoded.error());
        auto& row = owner->decoded.record;
        row.ordinal = header[3];
        const auto optional_value = [&header](std::size_t bit, std::size_t index) -> std::optional<std::uint64_t> {
            return (header[4] & (std::uint64_t{1u} << bit)) != 0u ? std::optional{header[index]} : std::nullopt;
        };
        row.identity.template_id = optional_value(0u, 5u);
        row.identity.segment_id = optional_value(1u, 6u);
        row.identity.reference_id = optional_value(2u, 7u);
        row.identity.match_group_id = optional_value(3u, 8u);
        if (const auto score = optional_value(4u, 9u)) row.identity.dp_score = std::bit_cast<std::int64_t>(*score);
        if (const auto score = optional_value(5u, 10u)) row.identity.chain_score = std::bit_cast<std::int64_t>(*score);
        row.identity.classification = static_cast<alignment_classification_source>(header[11]);
        row.identity.completes_match_group = header[12] != 0u;
        if (header[15] > 1u || (header[15] != 0u && !row.alignment))
            return std::unexpected(std::string{"compact replay alignment interpretation is invalid"});
        if (row.alignment) {
            row.alignment->source_bound_interpretation = header[15] != 0u;
            for (std::size_t bit = 0u; bit < row.alignment->flag_constraints.size(); ++bit)
                if ((header[13] & (std::uint64_t{1u} << bit)) != 0u)
                    row.alignment->flag_constraints[bit] = (header[14] & (std::uint64_t{1u} << bit)) != 0u;
        }
        constexpr std::array roles{record_field_role::sequence, record_field_role::quality, record_field_role::reference_sequence};
        for (std::size_t index = 0u; index < roles.size(); ++index) {
            const auto first = 16u + 5u * index;
            if (header[first] == 0u) continue;
            if (header[first + 1u] > 255u || header[first + 3u] < cursor_ + sizeof(header)
                || header[first + 3u] > cursor_ + header[1]
                || header[first + 4u] > cursor_ + header[1] - header[first + 3u])
                return std::unexpected(std::string{"compact replay sequence geometry is invalid"});
            auto& source = owner->sequences[index];
            source = {&file_, header[first + 3u], header[first + 2u], header[first + 4u], static_cast<std::uint8_t>(header[first + 1u])};
            if (index == 2u) {
                const preserved_sequence_descriptor descriptor{source.alphabet_code, true, source.symbol_count, preservation_storage::native};
                if (const auto begun = owner->decoded.begin_sequence(roles[index], descriptor); !begun) return std::unexpected(begun.error());
            }
            const auto borrowed = source.borrow();
            if (!borrowed) return std::unexpected(borrowed.error());
            const auto attached = owner->decoded.attach_native_sequence(roles[index], *borrowed);
            if (!attached) return std::unexpected(attached.error());
        }
        if (header[31] != 0u) {
            if (header[32] < cursor_ + sizeof(header) || header[32] > cursor_ + header[1]
                || header[33] > (cursor_ + header[1] - header[32]) / sizeof(cigar_run))
                return std::unexpected(std::string{"compact replay CIGAR geometry is invalid"});
            owner->cigar = {&file_, header[32], header[33]};
            const auto attached = owner->decoded.attach_cigar_source(owner->cigar.borrow());
            if (!attached) return std::unexpected(attached.error());
        }
        const auto frozen = owner->decoded.freeze();
        if (!frozen) return std::unexpected(frozen.error());
        cursor_ += header[1];
        return std::optional<std::unique_ptr<replayed_record>>{std::move(owner)};
    }
};

/** Own immutable context storage permits through dictionary finalization and replay. */
struct export_context_memory {
    parallelism::memory_permit permit;
    bool memory_pressure{};
    /** Acquire a checked incremental reservation; no new budgeting policy is introduced. */
    [[nodiscard]] static std::expected<void, std::string> acquire(void* context, std::size_t increment,
            io_memory_credit* credit) {
        auto& owner = *static_cast<export_context_memory*>(context);
        owner.memory_pressure = false;
        if (increment > std::numeric_limits<std::uint64_t>::max() - owner.permit.bytes())
            return std::unexpected(std::string{"alignment context admission overflows"});
        const auto required = owner.permit.bytes() + increment;
        const bool growing = static_cast<bool>(owner.permit);
        if (!grow_io_memory(owner.permit, required, credit)) {
            owner.memory_pressure = true;
            return std::unexpected(std::string{growing ? "cannot grow alignment context"
                : "cannot admit alignment context"});
        }
        return {};
    }
};

/** Run a source exactly once; batches and every backend callback stay on this thread. */
template<export_record_source Source>
[[nodiscard]] std::expected<std::uint64_t, record_error> write_records(
        Source& source, std::string_view path,
        const writer_options& options = default_writer_options,
        const concurrency_options& concurrency = concurrency_options{}) {
    io_caller_checkpoint checkpoint;
    if constexpr (requires(Source& candidate) { { candidate.checkpoint() } -> std::same_as<void>; }) {
        checkpoint = {std::addressof(source), [](void* context) {
            static_cast<Source*>(context)->checkpoint();
        }};
    }
    checkpoint();
    const auto format = resolve_output_format(path, options);
    if (!format) return std::unexpected(record_error{0u, std::nullopt, format.error()});
    const auto transform = alignment::sequence_codec_kernel();
    if (transform == nullptr)
        return std::unexpected(record_error{0u, std::nullopt, "no compiled SIMD sequence codec is available"});
    const auto opened = ordered_output_sink::open(path, options, format->compression, concurrency, checkpoint);
    if (!opened) return std::unexpected(record_error{0u, std::nullopt, opened.error()});
    auto& output = **opened;
    std::uint64_t records{};
    std::optional<std::uint8_t> absent_sequence_alphabet;
    std::optional<std::uint8_t> absent_quality_alphabet;
    if (source.metadata().declared_alphabet_codes.size() == 1u)
        absent_sequence_alphabet = source.metadata().declared_alphabet_codes.front();
    if (source.metadata().declared_quality_alphabet_codes.size() == 1u)
        absent_quality_alphabet = source.metadata().declared_quality_alphabet_codes.front();
    if (!absent_sequence_alphabet || !absent_quality_alphabet) {
        std::optional<std::uint8_t> sequence_candidate, quality_candidate;
        bool sequence_conflict{}, quality_conflict{};
        for (const auto& column : source.columns()) {
            if (!column.alphabet_code) continue;
            if (column.kind == column_value_kind::biological_sequence) {
                sequence_conflict = sequence_conflict || (sequence_candidate && *sequence_candidate != *column.alphabet_code);
                sequence_candidate = column.alphabet_code;
            } else if (column.kind == column_value_kind::quality_sequence) {
                quality_conflict = quality_conflict || (quality_candidate && *quality_candidate != *column.alphabet_code);
                quality_candidate = column.alphabet_code;
            }
        }
        if (!absent_sequence_alphabet && !sequence_conflict) absent_sequence_alphabet = sequence_candidate;
        if (!absent_quality_alphabet && !quality_conflict) absent_quality_alphabet = quality_candidate;
    }
    const bool text = format->format == sequence_file_format::fasta || format->format == sequence_file_format::fastq;
    export_context_memory context_memory;
    alignment::alignment_export_context alignment_context;
    if (!text) {
        const auto initialized = alignment_context.initialize(source.metadata(), options,
            alignment::alignment_context_admission{&context_memory, &export_context_memory::acquire});
        if (!initialized) return std::unexpected(record_error{0u, std::nullopt, initialized.error()});
    }
    // A declared complete dictionary and alphabet schema permit immediate imported-row output.
    bool streaming_alignment = !text && source.metadata().reference_dictionary_complete
        && !source.metadata().declared_alphabet_codes.empty();
    if (streaming_alignment) {
        for (const auto& column : source.columns()) {
            if (column.kind != column_value_kind::biological_sequence && column.kind != column_value_kind::quality_sequence) continue;
            const auto& declared = column.kind == column_value_kind::biological_sequence
                ? source.metadata().declared_alphabet_codes : source.metadata().declared_quality_alphabet_codes;
            if (!column.alphabet_code || std::find(declared.begin(), declared.end(), *column.alphabet_code) == declared.end()) {
                streaming_alignment = false;
                break;
            }
        }
    }
    bool header_emitted{};
    if (streaming_alignment) {
        if (const auto finalized = alignment_context.finalize(); !finalized)
            return std::unexpected(record_error{0u, std::nullopt, finalized.error()});
        if (const auto header = alignment_context.emit_header(output, format->format); !header)
            return std::unexpected(record_error{0u, std::nullopt, header.error()});
        header_emitted = true;
    }
    const auto write_row = [&alignment_context, &output, &format, &options, text, transform,
            absent_sequence_alphabet, absent_quality_alphabet, checkpoint](const export_record_view& source_row)
        -> std::expected<void, record_error> {
        const checkpointed_export_record checked{source_row, checkpoint};
        const auto& row = checked.row;
        if (text) {
            const auto written = emit_text_record(output, row, format->format, options, transform,
                absent_sequence_alphabet, absent_quality_alphabet);
            if (!written) return std::unexpected(record_error{row.ordinal, std::nullopt, written.error()});
            return {};
        }
        const auto resolved = alignment_context.resolve(row);
        if (!resolved) return std::unexpected(record_error{row.ordinal, std::nullopt, resolved.error()});
        const auto written = emit_alignment_record(output, row, *resolved, format->format, transform);
        if (!written) return std::unexpected(record_error{row.ordinal, std::nullopt, written.error()});
        return {};
    };
    // The backend batch owns its session; this caller-owned queue never enters a task closure.
    struct retained_batch {
        typename Source::batch_type batch;
        std::size_t first_row{};
    };
    // Both lists release nodes on pop/clear; this shared permit outlives every node.
    parallelism::memory_permit queue_permit;
    std::list<retained_batch> retained;
    std::optional<record_replay_file> replay;
    // Owners remain on the source thread; only detached immutable views enter task closures.
    std::list<typename Source::batch_type> formatting_batches;
    /** Admit one node across both queues, including its aligned linked-node prefix. */
    const auto admit_batch_queue_entry = [&retained, &formatting_batches, &queue_permit]()
        -> std::expected<bool, std::string> {
        const auto count = common::checked_add(retained.size(), formatting_batches.size());
        const auto next_count = count ? common::checked_add(*count, std::size_t{1u}) : std::nullopt;
        const auto overhead = std::max(std::size_t{128u}, alignof(retained_batch));
        const auto entry_bytes = common::checked_add(sizeof(retained_batch), overhead);
        const auto required = next_count && entry_bytes
            ? common::checked_mul(*next_count, *entry_bytes) : std::nullopt;
        if (!required) return std::unexpected(std::string{"writer batch queue capacity overflows"});
        if (queue_permit) return queue_permit.grow_to(*required);
        auto reservation = parallelism::memory_admission_controller::instance().try_reserve(*required);
        if (!reservation) return false;
        queue_permit = std::move(*reservation);
        return true;
    };
    std::vector<std::unique_ptr<replayed_record>> formatting_replay_rows;
    record_format_wave formatting{concurrency, format->format, options, transform,
        absent_sequence_alphabet, absent_quality_alphabet};
    const auto drain_formatting = [&formatting, &formatting_batches, &formatting_replay_rows, &output, &write_row, checkpoint]()
        -> std::expected<void, record_error> {
        const auto drained = formatting.drain(output, write_row);
        checkpoint();
        if (!drained) return drained;
        formatting_batches.clear();
        formatting_replay_rows.clear();
        // Preserve peak queue admission while a popped retained batch may still be local.
        return {};
    };
    const auto schedule_row = [&formatting, &drain_formatting, &write_row, &alignment_context, &options, text, checkpoint](export_record_view row)
        -> std::expected<bool, record_error> {
        checkpoint();
        // Interpret a shallow row copy once for serial, worker and preservation paths.
        // Source-bound facts survive private spill and cannot be redefined by options.
        if (!text && row.alignment) {
            auto& alignment = *row.alignment;
            if (options.sequence_orientation) {
                if (alignment.source_bound_interpretation
                    && *options.sequence_orientation != alignment.sequence_orientation)
                    return std::unexpected(record_error{row.ordinal, std::nullopt,
                        "Explicit sequence_orientation disagrees with the source alignment"});
                alignment.sequence_orientation = *options.sequence_orientation;
            }
            if (options.alignment_cigar_form) {
                if (alignment.source_bound_interpretation && *options.alignment_cigar_form != alignment.form)
                    return std::unexpected(record_error{row.ordinal, std::nullopt,
                        "Explicit cigar_form disagrees with the source alignment"});
                alignment.form = *options.alignment_cigar_form;
            }
        }
        if (formatting.full()) {
            if (const auto drained = drain_formatting(); !drained) return std::unexpected(drained.error());
        }
        const auto queued = formatting.enqueue(row, text ? nullptr : &alignment_context, checkpoint);
        if (queued && *queued) return true;
        // A speculative detachment/context failure is rechecked after its private allocations retire.
        // Semantic failures remain errors in the authoritative serial path; pressure can now make progress.
        if (const auto drained = drain_formatting(); !drained) return std::unexpected(drained.error());
        if (const auto written = write_row(row); !written) return std::unexpected(written.error());
        return false;
    };
    struct batch_task_guard {
        record_format_wave& wave;
        bool active{true};
        /** Join before a currently borrowed caller-owned batch leaves its scope. */
        ~batch_task_guard() { if (active) wave.wait(); }
    };
    const auto spill_retained = [&retained, &formatting_batches, &queue_permit, &replay, path, checkpoint]()
        -> std::expected<void, std::string> {
        if (!replay) {
            auto opened_replay = record_replay_file::open(path);
            if (!opened_replay) return std::unexpected(opened_replay.error());
            replay.emplace(std::move(*opened_replay));
        }
        while (!retained.empty()) {
            const auto& entry = retained.front();
            for (std::size_t index = entry.first_row; index < entry.batch.size(); ++index) {
                checkpoint();
                const auto stored = replay->append(entry.batch.row(index), checkpoint);
                if (!stored) return stored;
            }
            retained.pop_front();
        }
        if (formatting_batches.empty()) queue_permit.release();
        return {};
    };
    for (;;) {
        checkpoint();
        auto batch = source.next();
        if (!batch && batch.error().code == record_error_code::memory_pressure && !formatting.empty()) {
            const auto drained = drain_formatting();
            if (!drained) { source.cancel(); return std::unexpected(drained.error()); }
            batch = source.next(); // Retry the retained pending tuple after completed waves release admission.
        }
        if (!batch && batch.error().code == record_error_code::memory_pressure && !retained.empty()) {
            const auto spilled = spill_retained();
            if (!spilled) { source.cancel(); return std::unexpected(record_error{records, std::nullopt, spilled.error()}); }
            batch = source.next(); // The source retains its pending tuple; SQL is never rerun.
        }
        if (!batch) { source.cancel(); return std::unexpected(batch.error()); }
        if (!*batch) break;
        batch_task_guard guard{formatting};
        bool batch_has_jobs{};
        auto retained_begin = (**batch).size();
        for (std::size_t index = 0u; index < (**batch).size(); ++index) {
            checkpoint();
            const auto row = (**batch).row(index);
            if (records == maximum_export_records) {
                source.cancel();
                return std::unexpected(record_error{row.ordinal, std::nullopt, "output row count exceeds signed bigint"});
            }
            if (streaming_alignment && alignment_context.needs_retained_suffix(row)) {
                const auto drained = drain_formatting();
                if (!drained) { source.cancel(); return std::unexpected(drained.error()); }
                batch_has_jobs = false;
                streaming_alignment = false;
            }
            if (!text && !streaming_alignment && retained_begin == (**batch).size()) retained_begin = index;
            auto written = text ? std::expected<void, std::string>{} : alignment_context.observe(row);
            if (!written && !text && context_memory.memory_pressure && !formatting.empty()) {
                const auto drained = drain_formatting();
                if (!drained) { source.cancel(); return std::unexpected(drained.error()); }
                batch_has_jobs = false;
                written = alignment_context.observe(row);
            }
            if (!written && !text && context_memory.memory_pressure && !retained.empty()) {
                const auto spilled = spill_retained();
                if (!spilled) { source.cancel(); return std::unexpected(record_error{row.ordinal, std::nullopt, spilled.error()}); }
                written = alignment_context.observe(row); // observe precharges before any mutation.
            }
            if (!written) { source.cancel(); return std::unexpected(record_error{row.ordinal, std::nullopt, written.error()}); }
            if (text || streaming_alignment) {
                const auto scheduled = schedule_row(row);
                if (!scheduled) { source.cancel(); return std::unexpected(scheduled.error()); }
                batch_has_jobs = batch_has_jobs || *scheduled;
            }
            ++records;
        }
        if (!text && retained_begin < (**batch).size()) {
            const auto admitted = admit_batch_queue_entry();
            if (!admitted) { source.cancel(); return std::unexpected(record_error{records, std::nullopt, admitted.error()}); }
            if (!*admitted) {
                const auto spilled = spill_retained();
                if (!spilled) { source.cancel(); return std::unexpected(record_error{records, std::nullopt, spilled.error()}); }
                for (std::size_t index = retained_begin; index < (**batch).size(); ++index) {
                    checkpoint();
                    const auto stored = replay->append((**batch).row(index), checkpoint);
                    if (!stored) { source.cancel(); return std::unexpected(record_error{records, std::nullopt, stored.error()}); }
                }
            } else retained.push_back({std::move(**batch), retained_begin});
        } else if (batch_has_jobs && !formatting.empty()) {
            const auto admitted = admit_batch_queue_entry();
            if (!admitted) { source.cancel(); return std::unexpected(record_error{records, std::nullopt, admitted.error()}); }
            if (*admitted) formatting_batches.push_back(std::move(**batch));
            else {
                // The current local batch and its guard retain borrowed data through this drain.
                const auto drained = drain_formatting();
                if (!drained) { source.cancel(); return std::unexpected(drained.error()); }
            }
        }
        guard.active = false;
    }
    if (const auto drained = drain_formatting(); !drained) return std::unexpected(drained.error());
    if (!text) {
        auto finalized = header_emitted ? alignment_context.finish_suffix_observation() : alignment_context.finalize(true);
        if (!finalized && context_memory.memory_pressure && !retained.empty()) {
            const auto spilled = spill_retained();
            if (!spilled) return std::unexpected(record_error{0u, std::nullopt, spilled.error()});
            finalized = header_emitted ? alignment_context.finish_suffix_observation() : alignment_context.finalize(true);
        }
        if (!finalized) return std::unexpected(record_error{0u, std::nullopt, finalized.error()});
        if (!header_emitted) {
            const auto header = alignment_context.emit_header(output, format->format);
            if (!header) return std::unexpected(record_error{0u, std::nullopt, header.error()});
        }
        if (replay) {
            auto restore_headroom = parallelism::memory_admission_controller::instance().try_reserve(replay->restore_capacity_bytes());
            if (!restore_headroom && !retained.empty()) {
                const auto spilled = spill_retained();
                if (!spilled) return std::unexpected(record_error{0u, std::nullopt, spilled.error()});
                restore_headroom = parallelism::memory_admission_controller::instance().try_reserve(replay->restore_capacity_bytes());
            }
            if (!restore_headroom) return std::unexpected(record_error{0u, std::nullopt,
                "cannot admit compact replay scalar restoration", record_error_code::memory_pressure});
            restore_headroom.reset(); // Individual restoration owners acquire their actual capacities.
            const auto sealed = replay->seal();
            if (!sealed) return std::unexpected(record_error{0u, std::nullopt, sealed.error()});
            for (;;) {
                if (!formatting.empty()) {
                    // Replay restoration may need most of the remaining budget; don't retain speculative output over it.
                    if (const auto drained = drain_formatting(); !drained) return std::unexpected(drained.error());
                }
                checkpoint();
                auto row = replay->next();
                if (!row) return std::unexpected(record_error{0u, std::nullopt, row.error()});
                if (!*row) break;
                // Drain before acquiring another owner if the bounded wave is full.
                if (formatting.full()) {
                    if (const auto drained = drain_formatting(); !drained) return std::unexpected(drained.error());
                }
                formatting_replay_rows.push_back(std::move(**row));
                const auto view = formatting_replay_rows.back()->decoded.record;
                // schedule_row may drain earlier jobs; retain this owner outside that drain.
                auto current = std::move(formatting_replay_rows.back());
                formatting_replay_rows.pop_back();
                batch_task_guard guard{formatting};
                const auto scheduled = schedule_row(view);
                if (!scheduled) { formatting.wait(); return std::unexpected(scheduled.error()); }
                if (*scheduled) formatting_replay_rows.push_back(std::move(current));
                guard.active = false;
            }
        }
        while (!retained.empty()) {
            // Keep the current source owner outside the drained prior-wave owner queue.
            auto current = std::move(retained.front());
            retained.pop_front();
            batch_task_guard guard{formatting};
            bool has_jobs{};
            for (std::size_t index = current.first_row; index < current.batch.size(); ++index) {
                const auto scheduled = schedule_row(current.batch.row(index));
                if (!scheduled) { formatting.wait(); return std::unexpected(scheduled.error()); }
                has_jobs = has_jobs || *scheduled;
            }
            // The removed retained node already admitted this smaller formatting node.
            if (has_jobs && !formatting.empty()) formatting_batches.push_back(std::move(current.batch));
            guard.active = false;
        }
    }
    if (const auto drained = drain_formatting(); !drained) return std::unexpected(drained.error());
    const auto finished = output.finish();
    if (!finished) return std::unexpected(record_error{records, std::nullopt, finished.error()});
    return records;
}

} // namespace bioseq::io
