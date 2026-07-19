#pragma once

#include "base.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <bitset>
#include <iostream>
#include <vector>
#include <immintrin.h>  // For AVX512 operations

namespace bloom_filters {

inline void PrintMaskInBits(const __m512i& mask) {
    // Create an array to store the 512-bit register as 16 32-bit integers
    alignas(64) int values[16];

    // Store the contents of the __m512i register into the array
    _mm512_store_si512(reinterpret_cast<__m512i*>(values), mask);

    // Print each value in binary format (32 bits)
    for (int i = 0; i < 16; ++i) {
        std::cout << "Lane " << i << ": " << std::bitset<32>(values[i]) << '\t';
    }
    std::cout << std::endl;
}

class ImpalaBlockedBF64BitAVX512 {
public:
    // Maximum number of blocks allowed for the Bloom Filter
    static constexpr uint32_t MAX_NUM_BLOCKS = (1ULL << 31);

    // Minimum number of bits that should be used in the Bloom Filter
    static constexpr uint32_t MIN_NUM_BITS = 512;  // Increased for AVX512

    static constexpr auto SIMD_ALIGNMENT = 64;

    explicit ImpalaBlockedBF64BitAVX512(size_t n_key, uint32_t n_bits_per_key) {
        uint32_t min_bits = std::max<uint32_t>(MIN_NUM_BITS, n_key * n_bits_per_key);
        num_blocks = std::min(min_bits >> 9, MAX_NUM_BLOCKS);  // Changed from >>8 to >>9 (512 bits per block)
        num_blocks_log = static_cast<uint32_t>(std::log2(num_blocks)) + 1;
        num_blocks = std::min(1U << num_blocks_log, MAX_NUM_BLOCKS);  // Ensure num_blocks doesn't exceed the max

        blocks.resize(num_blocks << 4);  // Resize to hold 16 32-bit values per block (64 bytes)
        std::cout << "BF Size: " << num_blocks * 8 * 8 / 1024 << " KiB\n";  // Output the size of the filter
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
            const __m512i mask = MakeMask(key[i] >> 32);  // Generate the mask based on the key
            __m512i* bucket = &reinterpret_cast<__m512i*>(bf)[block];  // Access the appropriate bucket
            _mm512_store_si512(bucket, _mm512_or_si512(*bucket, mask));  // Perform the insert operation using OR
        }
    }

    size_t LookupInternal(size_t num, uint64_t* BF_RESTRICT key, uint32_t* BF_RESTRICT bf,
                          uint32_t* BF_RESTRICT out) const {
        for (uint32_t i = 0; i < num; i++){
            uint32_t block = key[i] & (num_blocks - 1);
            const __m512i mask = MakeMask(key[i] >> 32);  // Generate the mask based on the key
            const __m512i bucket = reinterpret_cast<__m512i*>(bf)[block];  // Access the appropriate bucket
            // Similar to AVX2's _mm256_testc_si256, we check if all bits in mask are present in bucket
            // _mm512_test_epi32_mask tests if (bucket & mask) != 0 for each lane
            // We want to check if (bucket & mask) == mask for each lane
            const __m512i and_result = _mm512_and_si512(bucket, mask);
            const __mmask16 test_result = _mm512_cmpeq_epi32_mask(and_result, mask);
            // If all 16 lanes match (all bits set), then the element might be present
            out[i] = (test_result == 0xFFFF) ? 1 : 0;
        }
        return num;
    }

    static inline __m512i MakeMask(const uint32_t hash) {
        const __m512i ones = _mm512_set1_epi32(1);  // Set all bits to 1
        // 16 different hash constants for AVX512 (16 lanes)
        const __m512i rehash = _mm512_setr_epi32(
            0x47b6137bU, 0x44974d91U, 0x8824ad5bU, 0xa2b7289dU,
            0x705495c7U, 0x2df1424bU, 0x9efc4947U, 0x5c6bfb31U,
            0x838e34f9U, 0x6d3b7e45U, 0x4f2a8c73U, 0x91d5b2a7U,
            0x3c8e69d1U, 0x7f4a2c85U, 0x5e9b3f21U, 0xa1c67b93U
        );
        __m512i hash_data = _mm512_set1_epi32(hash);  // Load the hash into a ZMM register
        hash_data = _mm512_mullo_epi32(rehash, hash_data);  // Multiply hash by constants
        hash_data = _mm512_srli_epi32(hash_data, 27);  // Right shift to get top 5 bits
        return _mm512_sllv_epi32(ones, hash_data);  // Shift the ones to the positions of the hash bits
    }

private:
    // Constants used for rehashing - expanded to 16 for AVX512
    constexpr static size_t NUM_CONSTANTS = 16;
    static constexpr uint32_t BLOOM_HASH_CONSTANTS[16] = {
        0x47b6137bU, 0x44974d91U, 0x8824ad5bU, 0xa2b7289dU,
        0x705495c7U, 0x2df1424bU, 0x9efc4947U, 0x5c6bfb31U,
        0x838e34f9U, 0x6d3b7e45U, 0x4f2a8c73U, 0x91d5b2a7U,
        0x3c8e69d1U, 0x7f4a2c85U, 0x5e9b3f21U, 0xa1c67b93U
    };

    uint32_t num_blocks;          // Number of blocks in the Bloom Filter
    uint32_t num_blocks_log;      // Log2 of the number of blocks (for optimization)
    // Align the blocks vector to 64-byte boundaries to ensure SIMD compatibility
    std::vector<uint32_t, AlignedAllocator<uint32_t, SIMD_ALIGNMENT>> blocks; // Internal array to hold the Bloom Filter blocks
};

} // namespace bloom_filters
