/**
 * @file metal_archive/main.cppm
 * @brief Entry point for the separate Metal archive-builder process.
 */

import bioseq.gpu.metal_archive.builder;

/**
 * @brief Passes command-line arguments to the archive builder and returns its exit status.
 */
auto main(const int argument_count, char** const arguments) -> int {
    return bioseq::gpu::metal_archive::run_binary_archive_builder(argument_count, arguments);
}
