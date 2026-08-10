import bioseq.cache.benchmark;

int main(int argc, char** argv) {
    return bioseq::cache_benchmark::run(
        argc, argv, bioseq::cache_benchmark::scenario::standalone);
}
