#include "base.h"

#include <cmath>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <vector>

#define BF_RESTRICT __restrict__

namespace bloom_filters {
class NewCacheSectorizedBF32Bit {
public:
	const uint32_t MAX_NUM_BLOCKS = (1 << 24);
	static constexpr auto MIN_NUM_BITS = 512;
	static constexpr auto SIMD_BATCH_SIZE = 32;
	static constexpr auto SIMD_ALIGNMENT = 64;

public:
	explicit NewCacheSectorizedBF32Bit(size_t n_key, uint32_t n_bits_per_key) {
		uint32_t min_bits = std::max<uint32_t>(MIN_NUM_BITS, n_key * n_bits_per_key);
		num_blocks_ = (min_bits >> 5) + 1;
		num_blocks_log_ = static_cast<uint32_t>(std::log2(num_blocks_)) + 1;
		num_blocks_ = std::min(1U << num_blocks_log_, MAX_NUM_BLOCKS);
		blocks_.resize(num_blocks_);
		std::cout << "BF Size: " << num_blocks_ * 4 / 1024 << " KiB\n";
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
		return ((key_lo & ((1 << 17) - 1)) + ((key_hi << 14) & (((1 << 9) - 1) << 17))) & (num_blocks_ - 1);
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

		size_t unaligned_num = (SIMD_ALIGNMENT - size_t(key) % SIMD_ALIGNMENT) / sizeof(uint64_t);
		for (size_t i = 0; i < unaligned_num; i++) {
			out[i] = LookupOne(key[i * 2], key[i * 2 + 1], bf);
		}
		size_t aligned_end = (num - unaligned_num) / SIMD_BATCH_SIZE * SIMD_BATCH_SIZE + unaligned_num;
		uint32_t *BF_RESTRICT aligned_key =
		    reinterpret_cast<uint32_t * BF_RESTRICT>(__builtin_assume_aligned(&key[unaligned_num * 2], SIMD_ALIGNMENT));

		for (size_t i = 0; i < aligned_end - unaligned_num; i += SIMD_BATCH_SIZE) {
#pragma clang loop vectorize(enable) unroll(enable) interleave(enable)
			for (int j = 0; j < SIMD_BATCH_SIZE; j++) {
				uint32_t key_lo = aligned_key[i * 2 + j * 2];
				uint32_t key_hi = aligned_key[i * 2 + j * 2 + 1];

				uint32_t block1 = GetBlock1(key_lo, key_hi);
				uint32_t mask1 = GetMask1(key_lo);
				uint32_t block2 = GetBlock2(key_hi, block1);
				uint32_t mask2 = GetMask2(key_hi);

				out[i + unaligned_num + j] = ((bf[block1] & mask1) == mask1) & ((bf[block2] & mask2) == mask2);
			}
		}

		for (size_t i = aligned_end; i < num; i++) {
			out[i] = LookupOne(key[i * 2], key[i * 2 + 1], bf);
		}
		return num;
	}

	inline void CacheSectorizedInsert(size_t num, uint64_t *BF_RESTRICT key64, uint32_t *BF_RESTRICT bf) {
		uint32_t *BF_RESTRICT key = reinterpret_cast<uint32_t * BF_RESTRICT>(key64);

		size_t unaligned_num = (SIMD_ALIGNMENT - size_t(key) % SIMD_ALIGNMENT) / sizeof(uint64_t);
		for (size_t i = 0; i < unaligned_num; i++) {
			InsertOne(key[i * 2], key[i * 2 + 1], bf);
		}
		size_t aligned_end = (num - unaligned_num) / SIMD_BATCH_SIZE * SIMD_BATCH_SIZE + unaligned_num;
		uint32_t *BF_RESTRICT aligned_key =
		    reinterpret_cast<uint32_t * BF_RESTRICT>(__builtin_assume_aligned(&key[unaligned_num * 2], SIMD_ALIGNMENT));

		for (size_t i = 0; i < aligned_end - unaligned_num; i += SIMD_BATCH_SIZE) {
			uint32_t block1[SIMD_BATCH_SIZE], mask1[SIMD_BATCH_SIZE];
			uint32_t block2[SIMD_BATCH_SIZE], mask2[SIMD_BATCH_SIZE];
#pragma clang loop vectorize(enable) unroll(enable) interleave(enable)
			for (int j = 0; j < SIMD_BATCH_SIZE; j++) {
				uint32_t key_lo = aligned_key[i * 2 + j * 2];
				uint32_t key_hi = aligned_key[i * 2 + j * 2 + 1];
				block1[j] = GetBlock1(key_lo, key_hi);
				mask1[j] = GetMask1(key_lo);
				block2[j] = GetBlock2(key_hi, block1[j]);
				mask2[j] = GetMask2(key_hi);
			}

#pragma clang loop unroll_count(SIMD_BATCH_SIZE)
			for (int j = 0; j < SIMD_BATCH_SIZE; j++) {
				bf[block1[j]] |= mask1[j];
				bf[block2[j]] |= mask2[j];
			}
		}
		for (size_t i = aligned_end; i < num; i++) {
			InsertOne(key[i * 2], key[i * 2 + 1], bf);
		}
	}

	uint32_t num_blocks_;
	uint32_t num_blocks_log_;
	std::vector<uint32_t, AlignedAllocator<uint32_t, SIMD_ALIGNMENT>> blocks_;
};
} // namespace bloom_filters