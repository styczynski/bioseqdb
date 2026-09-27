/**
 * @file metal_archive/main.cppm
 * @brief Process launcher for the exec-clean Metal archive builder.
 */

import bioseq.gpu.metal_archive.builder;

/**
 * @brief Delegates the exec-clean process entry point to the archive-builder
 * module owner.
 *
 * @param[in] argument_count Number of command-line arguments, including the
 * executable name.
 * @param[in] arguments Mutable null-terminated command-line argument array.
 * @return Archive-builder process status.
 */
auto main(const int argument_count, char** const arguments) -> int {
    return bioseq::gpu::metal_archive::run_binary_archive_builder(argument_count, arguments);
}
