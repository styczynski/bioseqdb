#pragma once

#include "base.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <vector>

namespace bloom_filters {
class RegisterBlockedBF2x32Bit {
public:
	const uint64_t MAX_NUM_BLOCKS = (1ULL << 31);
	static constexpr auto MIN_NUM_BITS = 512;

public:
	explicit RegisterBlockedBF2x32Bit(size_t n_key, uint32_t n_bits_per_key) {
		uint32_t min_bits = std::max<uint32_t>(MIN_NUM_BITS, n_key * n_bits_per_key);
		num_blocks = (min_bits >> 5) + 1;
		num_blocks_log = static_cast<uint32_t>(std::log2(num_blocks)) + 1;
		num_blocks = std::min(static_cast<uint64_t>(1ULL << num_blocks_log), MAX_NUM_BLOCKS);

		blocks.resize(num_blocks);
		std::cout << "BF Size: " << num_blocks * 4 / 1024 << " KiB\n";
	}

public:
	inline void Insert(uint32_t num, uint64_t *key) {
		InsertInternal(num, key, blocks.data());
	}

	inline uint32_t Lookup(uint32_t num, uint64_t *key, uint32_t *out) {
		return LookupInternal(num, key, blocks.data(), out);
	}

public:
	uint32_t LookupInternal(uint32_t num, uint64_t *BF_RESTRICT key, uint32_t *BF_RESTRICT bf,
	                        uint32_t *BF_RESTRICT out) const {
		for (int i = 0; i < num; i++) {
			uint32_t key_high = key[i] >> 32;
			uint32_t key_low = key[i];
			// We have to do some operators on key_high, otherwise the compiler will use 8 * 64 gathers instead of 16 *
			// 32 gathers.
			uint32_t block = (key_high >> 1) & (num_blocks - 1);
			uint32_t mask = (1 << (key_low & 31)) | (1 << ((key_low >> 5) & 31)) | (1 << ((key_low >> 10) & 31)) |
			                (1 << ((key_low >> 15) & 31)) | (1 << ((key_low >> 20) & 31));
			out[i] = (bf[block] & mask) == mask;
		}
		return num;
	}

	void InsertInternal(uint32_t num, uint64_t *BF_RESTRICT key, uint32_t *BF_RESTRICT bf) const {
		for (int i = 0; i < num; i++) {
			uint32_t key_high = key[i] >> 32;
			uint32_t key_low = key[i];
			// We have to do some operators on key_high, otherwise the compiler will use 8 * 64 gathers instead of 16 *
			// 32 gathers.
			uint32_t block = (key_high >> 1) & (num_blocks - 1);
			uint32_t mask = (1 << (key_low & 31)) | (1 << ((key_low >> 5) & 31)) | (1 << ((key_low >> 10) & 31)) |
			                (1 << ((key_low >> 15) & 31)) | (1 << ((key_low >> 20) & 31));
			bf[block] |= mask;
		}
	}

private:
	uint32_t num_blocks;
	uint32_t num_blocks_log;
	std::vector<uint32_t> blocks;
};
} // namespace bloom_filters