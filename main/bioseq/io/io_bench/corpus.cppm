/**
 * @file corpus.cppm
 * @brief Synthetic corpus generation for the IO bench.
 *
 * Each cell of the bench matrix needs a deterministic on-disk corpus
 * the readers can race over. The matrix runner reuses one corpus
 * across all readers in the cell so the comparison measures parsing
 * speed, not generator variance.
 *
 * Strategy:
 *   1. Pick (record_count, mean_record_length) from the
 *      `IoLengthBucket` so the corpus is in the multi-MB range —
 *      enough for steady-state throughput to dominate startup costs.
 *   2. For each record, emit `>seq_<i>\n` followed by a body sampled
 *      from `{A, C, G, T}` (FASTA dna4 fast-path domain). Body lines
 *      are wrapped at 80 columns.
 *   3. For FASTQ, emit `@seq_<i>\nACGT...\n+\nIII...\n` with
 *      Phred+33 = `'I'` (Q40) for every base. Single-line FASTQ — the
 *      v1 multi-line FASTQ qual path is correctness-tested elsewhere.
 *
 * Corpora are written to `std::filesystem::temp_directory_path()` with
 * a (pid, counter) suffix so concurrent test runs don't collide. The
 * caller is responsible for `fs::remove(...)` after the test —
 * typically via the matrix runner's per-cell scope guard.
 */
module;

#include "common.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

export module bioseq_io_bench.corpus;

import mlogging;
import bioseq_io_bench.samples;

export {

namespace bioseq::io_bench {

namespace fs = std::filesystem;

/// Per-bucket corpus shape. Sized so the on-disk file ranges from
/// ~2 MiB (SHORT) to ~50 MiB (VERY_LONG). Big enough that the
/// throughput measurement is dominated by parser cost, small enough
/// that the bench-fast iteration cycle stays under 1 minute per
/// cell.
struct CorpusShape {
    std::size_t record_count;
    std::size_t mean_record_length_bp;
    std::size_t target_bytes_on_disk;  // approximate
};

[[nodiscard]] inline CorpusShape corpus_shape_for(IoLengthBucket bucket) noexcept {
    switch (bucket) {
        case IoLengthBucket::SHORT:
            return {/*record_count=*/40'000,
                    /*mean_record_length_bp=*/200,
                    /*target_bytes_on_disk=*/9 * 1024 * 1024};
        case IoLengthBucket::MEDIUM:
            return {/*record_count=*/5'000,
                    /*mean_record_length_bp=*/2'000,
                    /*target_bytes_on_disk=*/11 * 1024 * 1024};
        case IoLengthBucket::LONG:
            return {/*record_count=*/200,
                    /*mean_record_length_bp=*/50'000,
                    /*target_bytes_on_disk=*/11 * 1024 * 1024};
        case IoLengthBucket::VERY_LONG:
            return {/*record_count=*/15,
                    /*mean_record_length_bp=*/1'000'000,
                    /*target_bytes_on_disk=*/16 * 1024 * 1024};
        case IoLengthBucket::REAL_HUMAN_GENOME:
            // Real-data path — see `try_open_real_corpus`. The
            // record_count is observed at warm-up time (the
            // matrix_runner calls every reader once before timed
            // measurement; the first reader's count becomes the
            // reference). Returning 0 here means "matrix_runner uses
            // the warm-up observation instead of this constant".
            return {/*record_count=*/0,
                    /*mean_record_length_bp=*/0,        // unused
                    /*target_bytes_on_disk=*/234 * 1024 * 1024};
        case IoLengthBucket::REAL_HUMAN_GENOME_LARGE_FASTQ_6GB:
            // Real-data FASTQ. ~6 GiB on disk. Same warm-up record
            // discovery as the FASTA real-data cell.
            return {/*record_count=*/0,
                    /*mean_record_length_bp=*/0,
                    /*target_bytes_on_disk=*/static_cast<std::size_t>(6) * 1024 * 1024 * 1024};
    }
    return {};
}

/// Generated-corpus handle. For synthetic-generated cells the matrix
/// runner deletes the file via `delete_corpus(...)`; for real-data
/// cells (`is_real_data == true`) the path is shared by every run
/// and never deleted.
struct CorpusHandle {
    std::string path;
    std::size_t record_count = 0;
    std::size_t bytes_on_disk = 0;
    std::vector<std::size_t> per_record_length_bp;
    IoFormat format = IoFormat::FASTA;
    IoLengthBucket length_bucket = IoLengthBucket::SHORT;
    bool is_real_data = false;
};

namespace detail {

[[nodiscard]] inline std::string allocate_temp_path(std::string_view tag) {
    static std::atomic<std::uint64_t> counter{0};
    fs::path temp_directory_root = fs::temp_directory_path();
    fs::path corpus_file_path = temp_directory_root / fmt::format(
        "bioseq_io_bench_{}_{}_{}_{}",
        tag,
        ::getpid(),
        counter.fetch_add(1, std::memory_order_relaxed),
        std::random_device{}() & 0xFFFFFFu);
    return corpus_file_path.string();
}

}  // namespace detail

/// Generate a synthetic corpus matching `format` × `length_bucket` and
/// write it to a temp file. The file ends with a trailing newline so
/// the parser's `is_eof` path doesn't have to special-case the last
/// record. Spec §18.4.
[[nodiscard]] inline CorpusHandle generate_corpus(
    IoFormat format,
    IoLengthBucket length_bucket,
    std::uint64_t seed) {
    const CorpusShape shape = corpus_shape_for(length_bucket);
    const std::string tag = fmt::format("{}_{}",
        io_format_name(format),
        io_length_bucket_name(length_bucket));
    std::string path = detail::allocate_temp_path(tag);

    CorpusHandle handle;
    handle.path = path;
    handle.format = format;
    handle.length_bucket = length_bucket;
    handle.per_record_length_bp.reserve(shape.record_count);

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> base_dist(0, 3);
    std::normal_distribution<double> length_dist(
        static_cast<double>(shape.mean_record_length_bp),
        static_cast<double>(shape.mean_record_length_bp) * 0.1);
    constexpr std::array<char, 4> dna4_chars{'A', 'C', 'G', 'T'};
    constexpr std::size_t kLineWrap = 80;

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        loge("[bioseq_io_bench.corpus] failed to open '{}' for writing",
             handle.path);
        return handle;
    }

    std::string buffered_chunk;
    buffered_chunk.reserve(64 * 1024);
    std::size_t bytes_written = 0;

    auto flush_chunk = [&]() {
        if (buffered_chunk.empty()) return;
        out.write(buffered_chunk.data(),
                  static_cast<std::streamsize>(buffered_chunk.size()));
        bytes_written += buffered_chunk.size();
        buffered_chunk.clear();
    };

    for (std::size_t record_index = 0; record_index < shape.record_count;
         ++record_index) {
        const double length_sample = length_dist(rng);
        const std::size_t record_length = static_cast<std::size_t>(
            std::max(8.0, length_sample));
        handle.per_record_length_bp.push_back(record_length);

        // Header.
        const std::string header_line = (format == IoFormat::FASTA)
            ? fmt::format(">seq_{}\n", record_index)
            : fmt::format("@seq_{}\n", record_index);
        buffered_chunk.append(header_line);
        if (buffered_chunk.size() >= 32 * 1024) flush_chunk();

        // Body.
        if (format == IoFormat::FASTA) {
            std::size_t emitted_letters = 0;
            while (emitted_letters < record_length) {
                const std::size_t this_line =
                    std::min(kLineWrap, record_length - emitted_letters);
                for (std::size_t letter_index = 0; letter_index < this_line;
                     ++letter_index) {
                    buffered_chunk.push_back(dna4_chars[base_dist(rng)]);
                }
                buffered_chunk.push_back('\n');
                emitted_letters += this_line;
                if (buffered_chunk.size() >= 32 * 1024) flush_chunk();
            }
        } else {
            // FASTQ — single-line seq + '+' + single-line qual,
            // each newline-terminated.
            for (std::size_t letter_index = 0; letter_index < record_length;
                 ++letter_index) {
                buffered_chunk.push_back(dna4_chars[base_dist(rng)]);
            }
            buffered_chunk.push_back('\n');
            buffered_chunk.push_back('+');
            buffered_chunk.push_back('\n');
            // Phred+33 'I' = Q40.
            buffered_chunk.append(record_length, 'I');
            buffered_chunk.push_back('\n');
            if (buffered_chunk.size() >= 32 * 1024) flush_chunk();
        }
    }

    flush_chunk();
    out.flush();
    out.close();

    handle.record_count = shape.record_count;
    handle.bytes_on_disk = bytes_written;

    JustLog("[bioseq_io_bench.corpus] generated '{}' ({} records, {} bytes)",
            handle.path, handle.record_count, handle.bytes_on_disk);
    return handle;
}

inline void delete_corpus(const CorpusHandle& handle) noexcept {
    if (handle.is_real_data) return;  // shared file, never delete
    std::error_code error_code;
    fs::remove(handle.path, error_code);
}

/// Try to map a real-data length bucket to a file under
/// `BIOSEQ_IO_TEST_DATA_DIR` and load it as a CorpusHandle. Returns
/// an empty handle (path empty) when the file is missing — the
/// matrix runner then logs and skips the cell rather than failing.
[[nodiscard]] inline CorpusHandle try_open_real_corpus(
    std::string_view test_data_dir,
    IoFormat format,
    IoLengthBucket length_bucket) {
    CorpusHandle handle;
    handle.format = format;
    handle.length_bucket = length_bucket;
    handle.is_real_data = true;

    if (length_bucket != IoLengthBucket::REAL_HUMAN_GENOME &&
        length_bucket != IoLengthBucket::REAL_HUMAN_GENOME_LARGE_FASTQ_6GB) {
        loge("[bioseq_io_bench.corpus] try_open_real_corpus called with "
             "non-real bucket");
        return handle;
    }
    fs::path data_root{std::string{test_data_dir}};
    fs::path file_path;
    if (length_bucket == IoLengthBucket::REAL_HUMAN_GENOME) {
        if (format != IoFormat::FASTA) return handle;
        file_path = data_root / "homo_sapiens_GRCh38_dna4.fasta";
    } else {
        if (format != IoFormat::FASTQ) return handle;
        file_path = data_root /
            "1000_genomes_human_SRR062634_sample_1_dna5.fastq";
    }
    std::error_code error_code;
    if (!fs::exists(file_path, error_code)) {
        loge("[bioseq_io_bench.corpus] real-data file missing: '{}'",
             file_path.string());
        return handle;
    }
    const auto on_disk_size = fs::file_size(file_path, error_code);
    if (error_code) {
        loge("[bioseq_io_bench.corpus] cannot stat '{}': {}",
             file_path.string(), error_code.message());
        return handle;
    }
    handle.path = file_path.string();
    handle.bytes_on_disk = static_cast<std::size_t>(on_disk_size);
    handle.record_count = corpus_shape_for(length_bucket).record_count;
    JustLog("[bioseq_io_bench.corpus] using real corpus '{}' ({} bytes)",
            handle.path, handle.bytes_on_disk);
    return handle;
}

}  // namespace bioseq::io_bench

}  // export
