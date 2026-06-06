/**
 * @file file_utils_test.cpp
 * @brief DOC0286 §13 — NO_POSTGRES unit tests for
 *        `bioseq::io::infer_sequence_file_kind` (file_utils.cppm).
 *
 * Covers: .fasta / .fa / .FASTA (case-insensitive) / .fastq.gz / .fq /
 * x.fasta.bgz, the unrecognised-extension error path (a.txt, a), and the
 * "no stem" error (.fasta — the extension equals the whole name).
 */
#include "common_tests.h"
#include "common.h"

#include <string_view>

import mlogging;
import bioseq_io.options;
import bioseq_io.file_utils;

namespace {

using bioseq::io::infer_sequence_file_kind;
using bioseq::io::sequence_io_file_type;

}  // namespace

TEST_CASE("file_utils: FASTA extensions map to FASTA", "[io-file-utils]") {
    for (const std::string_view path : {"a.fasta", "a.fa", "a.fna",
                                        "/tmp/dir.with.dots/genome.fasta"}) {
        const auto kind = infer_sequence_file_kind(path);
        REQUIRE(kind.has_value());
        REQUIRE(kind->format == sequence_io_file_type::FASTA);
        REQUIRE(kind->is_gzip_suffix == false);
    }
}

TEST_CASE("file_utils: FASTQ extensions map to FASTQ", "[io-file-utils]") {
    for (const std::string_view path : {"reads.fastq", "reads.fq", "reads.fnq"}) {
        const auto kind = infer_sequence_file_kind(path);
        REQUIRE(kind.has_value());
        REQUIRE(kind->format == sequence_io_file_type::FASTQ);
        REQUIRE(kind->is_gzip_suffix == false);
    }
}

TEST_CASE("file_utils: case-insensitive extension match", "[io-file-utils]") {
    const auto kind = infer_sequence_file_kind("a.FASTA");
    REQUIRE(kind.has_value());
    REQUIRE(kind->format == sequence_io_file_type::FASTA);
    REQUIRE(kind->is_gzip_suffix == false);

    const auto kind_q = infer_sequence_file_kind("READS.FQ");
    REQUIRE(kind_q.has_value());
    REQUIRE(kind_q->format == sequence_io_file_type::FASTQ);
}

TEST_CASE("file_utils: gzip suffixes are stripped and flagged", "[io-file-utils]") {
    const auto fastq_gz = infer_sequence_file_kind("a.fastq.gz");
    REQUIRE(fastq_gz.has_value());
    REQUIRE(fastq_gz->format == sequence_io_file_type::FASTQ);
    REQUIRE(fastq_gz->is_gzip_suffix == true);

    const auto fasta_bgz = infer_sequence_file_kind("x.fasta.bgz");
    REQUIRE(fasta_bgz.has_value());
    REQUIRE(fasta_bgz->format == sequence_io_file_type::FASTA);
    REQUIRE(fasta_bgz->is_gzip_suffix == true);

    const auto fa_bgzf = infer_sequence_file_kind("genome.fa.bgzf");
    REQUIRE(fa_bgzf.has_value());
    REQUIRE(fa_bgzf->format == sequence_io_file_type::FASTA);
    REQUIRE(fa_bgzf->is_gzip_suffix == true);
}

TEST_CASE("file_utils: unrecognised extension is an error", "[io-file-utils]") {
    // Unknown extension.
    REQUIRE_FALSE(infer_sequence_file_kind("a.txt").has_value());
    // No extension at all.
    REQUIRE_FALSE(infer_sequence_file_kind("a").has_value());
    // A bare extension with no stem (size == suffix size) is rejected: the
    // `stem.size() > suffix.size()` guard requires at least one stem char.
    REQUIRE_FALSE(infer_sequence_file_kind(".fasta").has_value());
    // A gzip suffix on an unknown base is still an error.
    REQUIRE_FALSE(infer_sequence_file_kind("a.txt.gz").has_value());
    // The error message names the offending path so the pg caller can ereport.
    const auto err = infer_sequence_file_kind("weird.dat");
    REQUIRE_FALSE(err.has_value());
    REQUIRE(err.error().find("weird.dat") != std::string::npos);
}
