#include "base.h"
#include "register_blocked_BF_32bit.h"
#include "register_blocked_BF_64bit.h"
#include "register_blocked_BF_32bit_Masks.h"
#include "register_blocked_BF_64bit_Masks.h"
#include "register_blocked_BF_2x32bit.h"
#include "cache_sectorized_BF_32bit.h"
#include "new_cache_sectorized_BF_32bit.h"
#include "impala_blocked_BF_64bit.h"
#include "impala_blocked_BF_64bit_avx512.h"

#include <cstdint>
#include <iostream>

#ifdef __x86_64__
#include <x86intrin.h>
#else
#include <chrono> // added for cross-platform timing
#endif

// Insert the GetCycleCount helper
uint64_t GetCycleCount() {
#ifdef __x86_64__
	return __rdtsc();
#else
	auto now = std::chrono::high_resolution_clock::now();
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
#endif
}

template <typename BloomFilterType, typename HashType>
void RunBenchmark(const std::string &title, size_t num_bits_per_key, size_t num_keys, size_t num_lookup_times) {
	// Create a Bloom filter
	BloomFilterType bf(num_keys, num_bits_per_key);

	// Prepare keys
	std::vector<uint64_t> keys(num_keys);
	for (size_t i = 0; i < num_keys; i++) {
		keys[i] = i;
	}
	std::vector<uint64_t> lookup_keys(num_keys);
	for (size_t i = 0; i < num_keys; i++) {
		lookup_keys[i] = i + num_keys;
	}

	std::vector<HashType> hashes(num_keys);

	// Insert
	uint64_t start = GetCycleCount(); // replaced __rdtsc() with GetCycleCount()
	bloom_filters::HashVector(num_keys, keys.data(), hashes.data());
	bf.Insert(num_keys, hashes.data());
	uint64_t end = GetCycleCount(); // replaced __rdtsc() with GetCycleCount()
	double insert_cpt = static_cast<double>(end - start) / static_cast<double>(num_keys);

	// Correctness Check
	{
		std::vector<uint32_t> out(num_keys, 0);
		bloom_filters::HashVector(num_keys, keys.data(), hashes.data());
		bf.Lookup(num_keys, hashes.data(), out.data());
		size_t positives = 0;
		for (size_t i = 0; i < num_keys; i++) {
			if (out[i] != 0) {
				positives++;
			} else {
				std::cout << out[i] << "\t";
			}
		}
		if (positives != num_keys) {
			std::cout << "ERROR: Correctness check failed! Passed queries: " << positives << "/" << num_keys << '\n';
		}
	}

	// Lookup
	const size_t lookupRepeat = std::max(num_lookup_times / num_keys, 1UL);
	std::vector<uint32_t> out(num_keys, 0);
	start = GetCycleCount(); // replaced __rdtsc() with GetCycleCount()
	for (size_t r = 0; r < lookupRepeat; r++) {
		bloom_filters::HashVector(num_keys, lookup_keys.data(), hashes.data());
		bf.Lookup(num_keys, hashes.data(), out.data());
	}
	end = GetCycleCount(); // replaced __rdtsc() with GetCycleCount()
	double lookup_cpt = static_cast<double>(end - start) / static_cast<double>(num_lookup_times);

	// False-positive rate
	size_t false_positives = 0;
	for (size_t i = 0; i < num_keys; i++) {
		if (out[i]) {
			false_positives++;
		}
	}
	double fp_rate = static_cast<double>(false_positives) / static_cast<double>(num_keys);

	std::cout << "[" << title << "]\n"
	          << "Insert took " << insert_cpt << " cycles per tuple\n"
	          << "Lookup took " << lookup_cpt << " cycles per tuple\n"
	          << "False-positive rate ~ " << fp_rate << "\n\n";
}

void ParseArgs(int argc, char *argv[], size_t &num_keys, size_t &num_bits_per_key, size_t &num_lookup_times) {
	if (argc != 4 && argc != 1) {
		std::cerr << "Usage: " << argv[0] << " <num_keys> <num_bits_per_key> <num_lookup_times>\n";
		exit(1);
	}

	if (argc == 4) {
		num_keys = 1 << std::stoi(argv[1]);
		num_bits_per_key = std::stoi(argv[2]);
		num_lookup_times = 1 << std::stoi(argv[3]);

		if (num_keys <= 0 || num_bits_per_key <= 0 || num_lookup_times <= 0) {
			std::cerr << "All arguments must be positive integers\n";
			exit(1);
		}
	}

	std::cout << "Number of keys: " << num_keys << "\n";
	std::cout << "Number of bits per key: " << num_bits_per_key << "\n";
	std::cout << "Number of lookup times: " << num_lookup_times << "\n\n";
}

int main(int argc, char *argv[]) {
	size_t num_keys = (1 << 17);
	size_t num_bits_per_key = 24;
	size_t num_lookup_times = std::max(1UL << 24, num_keys);

	ParseArgs(argc, argv, num_keys, num_bits_per_key, num_lookup_times);

	RunBenchmark<bloom_filters::RegisterBlockedBF32Bit, uint32_t>("32-bit Vectorized Register-Blocked BF",
	                                                              num_bits_per_key, num_keys, num_lookup_times);

	RunBenchmark<bloom_filters::RegisterBlockedBF32BitMasks, uint32_t>(
	    "32-bit Vectorized Register-Blocked BF with Masks", num_bits_per_key, num_keys, num_lookup_times);
	
	RunBenchmark<bloom_filters::RegisterBlockedBF64Bit, uint64_t>("64-bit Vectorized Register-Blocked BF",
	                                                              num_bits_per_key, num_keys, num_lookup_times);

	RunBenchmark<bloom_filters::RegisterBlockedBF64BitMasks, uint64_t>(
	    "64-bit Vectorized Register-Blocked BF with Masks", num_bits_per_key, num_keys, num_lookup_times);

	RunBenchmark<bloom_filters::RegisterBlockedBF2x32Bit, uint64_t>("2x32-bit Vectorized Register-Blocked BF",
	                                                                num_bits_per_key, num_keys, num_lookup_times);

	RunBenchmark<bloom_filters::CacheSectorizedBF32Bit, uint64_t>("32-bit Vectorized Cache-sectorized BF",
	                                                              num_bits_per_key, num_keys, num_lookup_times);

	RunBenchmark<bloom_filters::NewCacheSectorizedBF32Bit, uint64_t>(
	    "New 32-bit Vectorized Cache-sectorized BF (based on Peter's version)", num_bits_per_key, num_keys,
	    num_lookup_times);

	// RunBenchmark<bloom_filters::ImpalaBlockedBF64Bit, uint64_t>("Impala Blocked BF", num_bits_per_key, num_keys,
	// 	num_lookup_times);

	// RunBenchmark<bloom_filters::ImpalaBlockedBF64BitAVX512, uint64_t>("Impala Blocked BF", num_bits_per_key, num_keys,
	// 	num_lookup_times);

	return 0;
}