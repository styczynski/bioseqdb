/** @file Ordered streaming FASTA/FASTQ writer over the shared admitted IO output path. */
export module bioseq.io.writer;

import std;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.simd.utils;
import bioseq.io.options;
import bioseq.io.file_kind;
import bioseq.io.reader;
import bioseq.io.write_pipeline;
import bioseq.io.alignment.sequence_codec;

export namespace bioseq::io {

/** Low-level text writer consumes already selected headers in caller record order.
 * SQL canonical header/type preservation belongs to write_records. Encoding here
 * shares its bounded native sequence and detached compression/block output owners.
 */
template<sequence_alphabet Alphabet, sequence_file_format Format>
class sequence_writer {
    static_assert(Format == sequence_file_format::fasta || Format == sequence_file_format::fastq);
    std::unique_ptr<ordered_output_sink> output_;
    writer_options options_;
    bioseq::simd::sequence_rank_transform_kernel transform_{};
    std::string failure_;
public:
    using alphabet_config = sequence_custom_alphabet<Alphabet, std::uint64_t>;
    using batch_t = sequence_batch<alphabet_config>;
    using sequence_view = compact_sequence_view<alphabet_config>;

    /** Read one existing batch view through the shared compact-range decoder. */
    [[nodiscard]] static const std::uint8_t* read_sequence(const void* context,
            storage::sequence_size_t from, std::size_t count, std::span<std::uint8_t> scratch) {
        const auto& view = *static_cast<const sequence_view*>(context);
        if (from > view.bp_size() || count > view.bp_size() - from || count > scratch.size()) return nullptr;
        view.template to_encoded_into<false>(scratch, static_cast<std::size_t>(from), static_cast<std::size_t>(from) + count);
        return scratch.data();
    }

    /** Construct an unopened movable owner; successful publication is explicit. */
    sequence_writer() = default;
    sequence_writer(const sequence_writer&) = delete;
    sequence_writer& operator=(const sequence_writer&) = delete;
    sequence_writer(sequence_writer&&) noexcept = default;
    sequence_writer& operator=(sequence_writer&&) noexcept = default;
    /** Abandon unpublished output after joining detached jobs. */
    ~sequence_writer() = default;

    /** Open a private output with caller-selected shared-pool concurrency. */
    [[nodiscard]] static std::expected<sequence_writer, std::string> open(
            std::string_view path, const writer_options& options = default_writer_options,
            const concurrency_options& concurrency = concurrency_options{}) {
        sequence_writer writer;
        writer.options_ = options;
        writer.options_.format = Format;
        const auto format = resolve_output_format(path, writer.options_);
        if (!format) return std::unexpected(format.error());
        auto output = ordered_output_sink::open(path, writer.options_, format->compression, concurrency);
        if (!output) return std::unexpected(output.error());
        writer.output_ = std::move(*output);
        writer.transform_ = alignment::sequence_codec_kernel();
        if (writer.transform_ == nullptr) return std::unexpected(std::string{"no sequence SIMD codec is available"});
        return writer;
    }

    /** Stream one record without a whole decoded copy or fabricated quality values. */
    [[nodiscard]] bool write_record(std::string_view header, const sequence_view& sequence,
            std::span<const std::uint8_t> quality = {}) {
        if (!failure_.empty() || !output_) return false;
        if (header.find_first_of("\r\n") != std::string_view::npos) {
            failure_ = "raw FASTA/FASTQ header contains a record separator";
            return false;
        }
        const storage::borrowed_rank_window_source source{&sequence, &read_sequence,
            sequence.bp_size(), supported_storage_alphabets::template index_of<Alphabet>(),
            storage::sequence_transform::identity, true,
            storage::resident_packed_sequence{sequence.data(), sequence.size(), sequence.bp_size(),
                sizeof(std::uint64_t), sequence_view::traits::bits_per_char, 0u,
                static_cast<std::uint16_t>(sequence.padding())}};
        if constexpr (Format == sequence_file_format::fastq) {
            if (quality.size() != source.symbol_count || !transform_(
                    bioseq::simd::sequence_rank_transform::validate_fastq, quality, 0u, {}, {})) {
                failure_ = "FASTQ requires exactly one valid ASCII Phred quality per sequence symbol";
                return false;
            }
        }
        const auto emit = [this, header, &source, quality]() -> std::expected<void, std::string> {
            if (auto written = output_->append(Format == sequence_file_format::fasta ? ">" : "@"); !written) return written;
            if (auto written = output_->append(header); !written) return written;
            if (auto written = output_->append("\n"); !written) return written;
            fasta_body_sink<ordered_output_sink> body{*output_, Format == sequence_file_format::fasta ? options_.fasta_line_width : 0u};
            const auto sequence_bytes = alignment::emit_native_sequence<Alphabet, false, false>(body, source, transform_);
            if (!sequence_bytes) return std::unexpected(sequence_bytes.error());
            if constexpr (Format == sequence_file_format::fastq) {
                if (auto written = output_->append("\n+\n"); !written) return written;
                if (auto written = output_->append(quality); !written) return written;
            }
            return output_->append("\n");
        };
        const auto written = emit();
        if (!written) { failure_ = written.error(); return false; }
        return true;
    }

    /** Preserve exact batch and record order while output workers consume detached blocks. */
    [[nodiscard]] bool write_batch(const batch_t& batch) {
        for (std::size_t index = 0u; index < batch.size(); ++index)
            if (!write_record(batch.header(index), batch.sequence(index), batch.quality(index))) return false;
        return true;
    }

    /** Complete every outstanding write and atomically expose the final output. */
    [[nodiscard]] std::expected<void, std::string> close() {
        if (!failure_.empty()) return std::unexpected(failure_);
        if (!output_) return std::unexpected(std::string{"writer is not open"});
        const auto finished = output_->finish();
        if (!finished) failure_ = finished.error();
        return finished;
    }

    /** Return committed reservations, including after successful close. */
    [[nodiscard]] std::uint64_t bytes_reserved() const noexcept {
        return output_ ? output_->bytes_written() : 0u;
    }
};

/** Write existing text batches through one ordered writer and explicit publication. */
template<sequence_alphabet Alphabet, sequence_file_format Format, class Path, class Batches>
[[nodiscard]] std::expected<std::size_t, std::string> write_sequences_file(
        Path&& path, Batches&& batches, const writer_options& options = default_writer_options) {
    auto opened = sequence_writer<Alphabet, Format>::open(std::string_view{std::forward<Path>(path)}, options);
    if (!opened) return std::unexpected(opened.error());
    std::size_t records{};
    for (const auto& batch : batches) {
        if (!opened->write_batch(batch)) return std::unexpected(opened->close().error());
        if (batch.size() > std::numeric_limits<std::size_t>::max() - records)
            return std::unexpected(std::string{"text output record count overflows"});
        records += batch.size();
    }
    if (auto finished = opened->close(); !finished) return std::unexpected(finished.error());
    return records;
}

} // namespace bioseq::io
