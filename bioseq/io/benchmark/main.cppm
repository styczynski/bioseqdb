/**
 * @file main.cppm
 * @brief Process launcher for the Bioseq IO benchmark.
 */

import bioseq.io.benchmark;

/** Pass command-line arguments to the IO benchmark runner. */
int main(const int argument_count, char** const arguments) {
    return bioseq::io::benchmark::run(argument_count, arguments);
}
