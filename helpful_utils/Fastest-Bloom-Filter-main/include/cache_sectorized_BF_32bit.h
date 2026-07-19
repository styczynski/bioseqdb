#include "base.h"

#include <cmath>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <vector>

#define BF_RESTRICT __restrict__

namespace bloom_filters {
class CacheSectorizedBF32Bit {
public:
	const uint32_t MAX_NUM_BLOCKS = (1 << 26);
	static constexpr auto MIN_NUM_BITS = 512;
	static constexpr auto SIMD_BATCH_SIZE = 16;
	static constexpr auto SIMD_ALIGNMENT = 64;

public:
	explicit CacheSectorizedBF32Bit(size_t n_key, uint32_t n_bits_per_key) {
		uint32_t min_bits = std::max<uint32_t>(MIN_NUM_BITS, n_key * n_bits_per_key);
		num_blocks = (min_bits >> 5) + 1;
		num_blocks_log = static_cast<uint32_t>(std::log2(num_blocks)) + 1;
		num_blocks = std::min(1U << num_blocks_log, MAX_NUM_BLOCKS);
		blocks_.resize(num_blocks);
		std::cout << "BF Size: " << num_blocks * 4 / 1024 << " KiB\n";
	}

public:
	inline uint32_t Lookup(uint32_t num, uint64_t *key, uint32_t *out) {
		return CacheSectorizedLookup(num, key, blocks_.data(), out);
	}
	inline void Insert(uint32_t num, uint64_t *key) {
		return CacheSectorizedInsert(num, key, blocks_.data());
	}

private:
	// key_lo |5:bit3|5:bit2|5:bit1|  13:block   |4:sector1 | bit layout (32:total)
	// key_hi |5:bit4|5:bit3|5:bit2|5:bit1|9:block|3:sector2| bit layout (32:total)
	inline uint32_t GetMask1(uint32_t key_lo) const {
		// 3 bits in key_lo
		return (1u << ((key_lo >> 17) & 31)) | (1u << ((key_lo >> 22) & 31)) | (1u << ((key_lo >> 27) & 31));
	}

	inline uint32_t GetMask2(uint32_t key_hi) const {
		// 4 bits in key_hi
		return (1u << ((key_hi >> 12) & 31)) | (1u << ((key_hi >> 17) & 31)) | (1u << ((key_hi >> 22) & 31)) |
		       (1u << ((key_hi >> 27) & 31));
	}

	inline uint32_t GetBlock1(uint32_t key_lo, uint32_t key_hi) const {
		// block: 13 bits in key_lo and 9 bits in key_hi
		// sector 1: 4 bits in key_lo
		return ((key_lo & ((1 << 17) - 1)) + ((key_hi << 14) & (((1 << 9) - 1) << 17))) & (num_blocks - 1);
	}

	inline uint32_t GetBlock2(uint32_t key_hi, uint32_t block1) const {
		// sector 2: 3 bits in key_hi
		return block1 ^ (8 + (key_hi & 7));
	}

	inline void InsertOne(uint32_t key_lo, uint32_t key_hi, uint32_t *BF_RESTRICT bf) {
		uint32_t block1 = GetBlock1(key_lo, key_hi);
		uint32_t mask1 = GetMask1(key_lo);
		uint32_t block2 = GetBlock2(key_hi, block1);
		uint32_t mask2 = GetMask2(key_hi);
		bf[block1] |= mask1;
		bf[block2] |= mask2;
	}

	inline bool LookupOne(uint32_t key_lo, uint32_t key_hi, const uint32_t *BF_RESTRICT bf) const {
		uint32_t block1 = GetBlock1(key_lo, key_hi);
		uint32_t mask1 = GetMask1(key_lo);
		uint32_t block2 = GetBlock2(key_hi, block1);
		uint32_t mask2 = GetMask2(key_hi);
		return ((bf[block1] & mask1) == mask1) & ((bf[block2] & mask2) == mask2);
	}

	uint32_t CacheSectorizedLookup(int num, const uint64_t *BF_RESTRICT key64, const uint32_t *BF_RESTRICT bf,
	                               uint32_t *BF_RESTRICT out) const {
		const uint32_t *BF_RESTRICT key = reinterpret_cast<const uint32_t * BF_RESTRICT>(key64);
		for (int i = 0; i + SIMD_BATCH_SIZE <= num; i += SIMD_BATCH_SIZE) {
			uint32_t block1[SIMD_BATCH_SIZE], mask1[SIMD_BATCH_SIZE];
			uint32_t block2[SIMD_BATCH_SIZE], mask2[SIMD_BATCH_SIZE];

			// #pragma clang loop vectorize(enable) unroll(enable) interleave(enable)
			for (int j = 0; j < SIMD_BATCH_SIZE; j++) {
				int p = i + j;
				uint32_t key_lo = key[p + p];
				uint32_t key_hi = key[p + p + 1];
				block1[j] = GetBlock1(key_lo, key_hi);
				mask1[j] = GetMask1(key_lo);
				block2[j] = GetBlock2(key_hi, block1[j]);
				mask2[j] = GetMask2(key_hi);
			}

			for (int j = 0; j < SIMD_BATCH_SIZE; j++) {
				out[i + j] = ((bf[block1[j]] & mask1[j]) == mask1[j]) & ((bf[block2[j]] & mask2[j]) == mask2[j]);
			}
		}

		// unaligned tail
		for (int i = num & ~(SIMD_BATCH_SIZE - 1); i < num; i++) {
			out[i] = LookupOne(key[i + i], key[i + i + 1], bf);
		}
		return num;
	}

	inline void CacheSectorizedInsert(int num, uint64_t *BF_RESTRICT key64, uint32_t *BF_RESTRICT bf) {
		const uint32_t *BF_RESTRICT key = reinterpret_cast<const uint32_t * BF_RESTRICT>(key64);
		for (int i = 0; i + SIMD_BATCH_SIZE <= num; i += SIMD_BATCH_SIZE) {
			uint32_t block1[SIMD_BATCH_SIZE], mask1[SIMD_BATCH_SIZE];
			uint32_t block2[SIMD_BATCH_SIZE], mask2[SIMD_BATCH_SIZE];

			// #pragma clang loop vectorize(enable) unroll(enable) interleave(enable)
			for (int j = 0; j < SIMD_BATCH_SIZE; j++) {
				int p = i + j;
				uint32_t key_lo = key[p + p];
				uint32_t key_hi = key[p + p + 1];
				block1[j] = GetBlock1(key_lo, key_hi);
				mask1[j] = GetMask1(key_lo);
				block2[j] = GetBlock2(key_hi, block1[j]);
				mask2[j] = GetMask2(key_hi);
			}

			for (int j = 0; j < SIMD_BATCH_SIZE; j++) {
				bf[block1[j]] |= mask1[j];
				bf[block2[j]] |= mask2[j];
			}
		}

		// unaligned tail
		for (int i = num & ~(SIMD_BATCH_SIZE - 1); i < num; i++) {
			InsertOne(key[i + i], key[i + i + 1], bf);
		}
	}

	uint32_t num_blocks;
	uint32_t num_blocks_log;
	std::vector<uint32_t, AlignedAllocator<uint32_t, SIMD_ALIGNMENT>> blocks_;
};
} // namespace bloom_filters