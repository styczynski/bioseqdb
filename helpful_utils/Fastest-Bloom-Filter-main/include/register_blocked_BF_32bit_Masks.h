#pragma once

#include "base.h"

#include <cmath>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <vector>
#include <random>

namespace bloom_filters {
struct BloomFilterMasks32 {
	BloomFilterMasks32() {
		std::seed_seq seed {0, 0, 0, 0, 0, 0, 0, 0};
		std::mt19937 re(seed);
		std::uniform_int_distribution<uint32_t> rd;

		auto random = [&re, &rd](uint32_t min_value, uint32_t max_value) {
			return min_value + rd(re) % (max_value - min_value + 1);
		};

		std::memset(masks_, 0, kTotalBytes);

		uint32_t num_bits_set = random(kMinBitsSet, kMaxBitsSet);
		for (uint32_t i = 0; i < num_bits_set; i++) {
			while (true) {
				uint32_t bit_pos = random(0, kBitsPerMask - 1);
				if (!GetBit(masks_, bit_pos)) {
					SetBit(masks_, bit_pos);
					break;
				}
			}
		}

		uint32_t num_bits_total = kNumMasks + kBitsPerMask - 1;
		for (uint32_t i = kBitsPerMask; i < num_bits_total; i++) {
			int bit_leaving = GetBit(masks_, i - kBitsPerMask) ? 1 : 0;
			if (bit_leaving == 1 && num_bits_set == kMinBitsSet) {
				SetBit(masks_, i);
				continue;
			}
			if (bit_leaving == 0 && num_bits_set == kMaxBitsSet) {
				continue;
			}
			if (random(0, kBitsPerMask * 2 - 1) < kMinBitsSet + kMaxBitsSet) {
				SetBit(masks_, i);
				if (bit_leaving == 0) {
					++num_bits_set;
				}
			} else {
				if (bit_leaving == 1) {
					--num_bits_set;
				}
			}
		}
	}

	uint32_t Mask(uint32_t hash) const {
		int mask_id = static_cast<int>(hash & (kNumMasks - 1));
		uint32_t result = GetMask(mask_id);

		int rotation = static_cast<int>((hash >> kLogNumMasks) & 31);
		result = ROTL32(result, rotation);
		return result;
	}

	static constexpr int kBitsPerMask = 25;
	static constexpr uint32_t kFullMask = (1U << kBitsPerMask) - 1;

	static constexpr int kMinBitsSet = 4;
	static constexpr int kMaxBitsSet = 5;

	static constexpr int kLogNumMasks = 10;
	static constexpr int kNumMasks = 1 << kLogNumMasks;

	static constexpr int kTotalBytes = (kNumMasks + 32) / 8;
	uint8_t masks_[kTotalBytes];

private:
	bool GetBit(const uint8_t *data, uint32_t bit_pos) const {
		return (data[bit_pos / 8] >> (bit_pos % 8)) & 1;
	}

	void SetBit(uint8_t *data, uint32_t bit_pos) {
		data[bit_pos / 8] |= (1 << (bit_pos % 8));
	}

	uint32_t ROTL32(uint32_t x, int r) const {
		return (x << r) | (x >> ((32 - r) & 31));
	}

	uint32_t GetMask(int bit_offset) const {
		uint32_t value;
		std::memcpy(&value, masks_ + bit_offset / 8, sizeof(uint32_t));
		return (value >> (bit_offset % 8)) & kFullMask;
	}
};

const static BloomFilterMasks32 masks32_;

class RegisterBlockedBF32BitMasks {
public:
	const uint32_t MAX_NUM_BLOCKS = (1 << 17);
	static constexpr auto MIN_NUM_BITS = 512;

public:
	explicit RegisterBlockedBF32BitMasks(size_t n_key, uint32_t n_bits_per_key) {
		uint32_t min_bits = std::max<uint32_t>(MIN_NUM_BITS, n_key * n_bits_per_key);
		num_blocks = (min_bits >> 5) + 1;
		num_blocks_log = static_cast<uint32_t>(std::log2(num_blocks)) + 1;
		num_blocks = std::min(1U << num_blocks_log, MAX_NUM_BLOCKS);

		blocks.resize(num_blocks);
		std::cout << "BF Size: " << num_blocks * 4 / 1024 << " KiB\n";
	}

public:
	inline void Insert(uint32_t num, uint32_t *key) {
		InsertInternal(num, key, blocks.data());
	}

	inline uint32_t Lookup(uint32_t num, uint32_t *key, uint32_t *out) {
		return LookupInternal(num, key, blocks.data(), out);
	}

public:
	uint32_t LookupInternal(uint32_t num, uint32_t *BF_RESTRICT key, uint32_t *BF_RESTRICT bf,
	                        uint32_t *BF_RESTRICT out) const {
		for (uint32_t i = 0; i < num; i++) {
			uint32_t block = (key[i] >> 15) & (num_blocks - 1);
			uint32_t mask = masks32_.Mask(key[i]);
			out[i] = (bf[block] & mask) == mask;
		}
		return num;
	}

	void InsertInternal(uint32_t num, uint32_t *BF_RESTRICT key, uint32_t *BF_RESTRICT bf) const {
		for (uint32_t i = 0; i < num; i++) {
			uint32_t block = (key[i] >> 15) & (num_blocks - 1);
			uint32_t mask = masks32_.Mask(key[i]);
			bf[block] |= mask;
		}
	}

private:
	uint32_t num_blocks;
	uint32_t num_blocks_log;
	std::vector<uint32_t> blocks;
};
} // namespace bloom_filters