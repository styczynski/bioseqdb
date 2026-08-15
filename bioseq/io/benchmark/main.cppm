/**
 * @file main.cppm
 * @brief Process launcher for the Bioseq IO benchmark.
 */

import bioseq.io.benchmark;

/** Delegate all benchmark behavior to its named module owner. */
int main(const int argument_count, char** const arguments) {
    return bioseq::io::benchmark::run(argument_count, arguments);
}
