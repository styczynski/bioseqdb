#pragma once

#include "base.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <bitset>
#include <iostream>
#include <vector>
#include <immintrin.h>  // For AVX2 operations

namespace bloom_filters {

inline void PrintMaskInBits(const __m256i& mask) {
	// Create an array to store the 256-bit register as 8 32-bit integers
	alignas(32) int values[8];

	// Store the contents of the __m256i register into the array
	_mm256_store_si256(reinterpret_cast<__m256i*>(values), mask);

	// Print each value in binary format (32 bits)
	for (int i = 0; i < 8; ++i) {
		std::cout << "Lane " << i << ": " << std::bitset<32>(values[i]) << '\t';
	}
	std::cout << std::endl;
}

class ImpalaBlockedBF64Bit {
public:
    // Maximum number of blocks allowed for the Bloom Filter
    static constexpr uint32_t MAX_NUM_BLOCKS = (1ULL << 31);

    // Minimum number of bits that should be used in the Bloom Filter
    static constexpr uint32_t MIN_NUM_BITS = 256;

	static constexpr auto SIMD_ALIGNMENT = 64;

    explicit ImpalaBlockedBF64Bit(size_t n_key, uint32_t n_bits_per_key) {
        uint32_t min_bits = std::max<uint32_t>(MIN_NUM_BITS, n_key * n_bits_per_key);
    	num_blocks = std::min(min_bits >> 8, MAX_NUM_BLOCKS);
        num_blocks_log = static_cast<uint32_t>(std::log2(num_blocks)) + 1;
        num_blocks = std::min(1U << num_blocks_log, MAX_NUM_BLOCKS);  // Ensure num_blocks doesn't exceed the max

        blocks.resize(num_blocks << 3);  // Resize the blocks vector to hold the necessary number of blocks
        std::cout << "BF Size: " << num_blocks * 4 * 8 / 1024 << " KiB\n";  // Output the size of the filter
    }

    inline void Insert(size_t num, uint64_t* key) {
        InsertInternal(num, key, blocks.data());
    }

    inline size_t Lookup(size_t num, uint64_t* key, uint32_t* out) {
        return LookupInternal(num, key, blocks.data(), out);
    }

private:
    void InsertInternal(size_t num, uint64_t* BF_RESTRICT key, uint32_t* BF_RESTRICT bf) const {
		for (uint32_t i = 0; i < num; i++){
			uint32_t block = key[i] & (num_blocks - 1);
        	const __m256i mask = MakeMask(key[i] >> 32);  // Generate the mask based on the key
			__m256i* bucket = &reinterpret_cast<__m256i*>(bf)[block];  // Access the appropriate bucket
        	_mm256_store_si256(bucket, _mm256_or_si256(*bucket, mask));  // Perform the insert operation using OR
		}
    }


    size_t LookupInternal(size_t num, uint64_t* BF_RESTRICT key, uint32_t* BF_RESTRICT bf,
                          uint32_t* BF_RESTRICT out) const {
		for (uint32_t i = 0; i < num; i++){
			uint32_t block = key[i] & (num_blocks - 1);
	        const __m256i mask = MakeMask(key[i] >> 32);  // Generate the mask based on the key
	        const __m256i bucket = reinterpret_cast<__m256i*>(bf)[block];  // Access the appropriate bucket
	        out[i] = _mm256_testc_si256(bucket, mask);  // Check if the mask bits are present in the buckets
		}
    	return num;
    }


	// Use AND + XOR instead of testc
	// const __m256i and_result = _mm256_and_si256(bucket, mask);
	// const __m256i xor_result = _mm256_xor_si256(and_result, mask);
	// out[i] = _mm256_testz_si256(xor_result, xor_result);

	// Extract values and check manually
	// uint32_t mask_vals[8], bucket_vals[8];
	// _mm256_storeu_si256(reinterpret_cast<__m256i*>(mask_vals), mask);
	// _mm256_storeu_si256(reinterpret_cast<__m256i*>(bucket_vals), bucket);

	// bool all_present = true;
	// for (int j = 0; j < 8; j++) {
	// 	if ((bucket_vals[j] & mask_vals[j]) != mask_vals[j]) {
	// 		all_present = false;
	// 		break;
	// 	}
	// }
	// out[i] = all_present ? 1 : 0;

    static inline __m256i MakeMask(const uint32_t hash) {
        const __m256i ones = _mm256_set1_epi32(1);  // Set all bits to 1
    	const __m256i rehash = _mm256_setr_epi32(0x47b6137bU, 0x44974d91U, 0x8824ad5bU, 0xa2b7289dU,
		                                         0x705495c7U, 0x2df1424bU, 0x9efc4947U, 0x5c6bfb31U);  // Constants for rehashing
        __m256i hash_data = _mm256_set1_epi32(hash);  // Load the hash into a YMM register
        hash_data = _mm256_mullo_epi32(rehash, hash_data);  // Multiply hash by constants
        hash_data = _mm256_srli_epi32(hash_data, 27);  // Right shift to get top 5 bits
        return _mm256_sllv_epi32(ones, hash_data);  // Shift the ones to the positions of the hash bits
    }

private:
    // Constants used for rehashing
    constexpr static size_t NUM_CONSTANTS = 8;
    static constexpr uint32_t BLOOM_HASH_CONSTANTS[8] = {
        0x47b6137bU, 0x44974d91U, 0x8824ad5bU, 0xa2b7289dU,
        0x705495c7U, 0x2df1424bU, 0x9efc4947U, 0x5c6bfb31U
    };

    uint32_t num_blocks;          // Number of blocks in the Bloom Filter
    uint32_t num_blocks_log;      // Log2 of the number of blocks (for optimization)
	// Align the blocks vector to 64-byte boundaries to ensure SIMD compatibility
	std::vector<uint32_t, AlignedAllocator<uint32_t, SIMD_ALIGNMENT>> blocks; // Internal array to hold the Bloom Filter blocks
};

} // namespace bloom_filters
