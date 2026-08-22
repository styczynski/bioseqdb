/**
 * @file file_kind_test.cpp
 * @brief Verifies sequence file suffix classification.
 */
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.common.logging;
import bioseq.io.options;
import bioseq.io.file_kind;

namespace {

using bioseq::io::detect_sequence_file_kind;
using bioseq::io::sequence_file_format;

} // namespace

TEST_CASE("file_utils: FASTA extensions map to FASTA",
          "[component:bioseq][layer:unit][feature:io]") {
    for (const std::string_view path :
         {"a.fasta", "a.fa", "a.fna", "/tmp/dir.with.dots/genome.fasta"}) {
        const auto kind = detect_sequence_file_kind(path);
        REQUIRE(kind.has_value());
        REQUIRE(kind->format == sequence_file_format::fasta);
        REQUIRE(kind->is_gzip_suffix == false);
    }
}

TEST_CASE("file_utils: FASTQ extensions map to FASTQ",
          "[component:bioseq][layer:unit][feature:io]") {
    for (const std::string_view path : {"reads.fastq", "reads.fq", "reads.fnq"}) {
        const auto kind = detect_sequence_file_kind(path);
        REQUIRE(kind.has_value());
        REQUIRE(kind->format == sequence_file_format::fastq);
        REQUIRE(kind->is_gzip_suffix == false);
    }
}

TEST_CASE("file_utils: case-insensitive extension match",
          "[component:bioseq][layer:unit][feature:io]") {
    const auto kind = detect_sequence_file_kind("a.FASTA");
    REQUIRE(kind.has_value());
    REQUIRE(kind->format == sequence_file_format::fasta);
    REQUIRE(kind->is_gzip_suffix == false);

    const auto kind_q = detect_sequence_file_kind("READS.FQ");
    REQUIRE(kind_q.has_value());
    REQUIRE(kind_q->format == sequence_file_format::fastq);
}

TEST_CASE("file_utils: gzip suffixes are stripped and flagged",
          "[component:bioseq][layer:unit][feature:io]") {
    const auto fastq_gz = detect_sequence_file_kind("a.fastq.gz");
    REQUIRE(fastq_gz.has_value());
    REQUIRE(fastq_gz->format == sequence_file_format::fastq);
    REQUIRE(fastq_gz->is_gzip_suffix == true);

    const auto fasta_bgz = detect_sequence_file_kind("x.fasta.bgz");
    REQUIRE(fasta_bgz.has_value());
    REQUIRE(fasta_bgz->format == sequence_file_format::fasta);
    REQUIRE(fasta_bgz->is_gzip_suffix == true);

    const auto fa_bgzf = detect_sequence_file_kind("genome.fa.bgzf");
    REQUIRE(fa_bgzf.has_value());
    REQUIRE(fa_bgzf->format == sequence_file_format::fasta);
    REQUIRE(fa_bgzf->is_gzip_suffix == true);
}

TEST_CASE("file_utils: unrecognised extension is an error",
          "[component:bioseq][layer:unit][feature:io]") {
    // Unknown extension.
    REQUIRE_FALSE(detect_sequence_file_kind("a.txt").has_value());
    // No extension at all.
    REQUIRE_FALSE(detect_sequence_file_kind("a").has_value());
    // A bare extension with no stem (size == suffix size) is rejected: the
    // `stem.size() > suffix.size()` guard requires at least one stem char.
    REQUIRE_FALSE(detect_sequence_file_kind(".fasta").has_value());
    // A gzip suffix on an unknown base is still an error.
    REQUIRE_FALSE(detect_sequence_file_kind("a.txt.gz").has_value());
    // The error message names the offending path so the pg caller can ereport.
    const auto err = detect_sequence_file_kind("weird.dat");
    REQUIRE_FALSE(err.has_value());
    REQUIRE(err.error().find("weird.dat") != std::string::npos);
}
