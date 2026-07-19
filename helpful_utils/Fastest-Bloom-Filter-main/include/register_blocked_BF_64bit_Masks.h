#pragma once

#include "base.h"

#include <cmath>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <random>

namespace bloom_filters {
// A set of pre-generated bit masks from a 64-bit word. https://save-buffer.github.io/bloom_filter.html
struct BloomFilterMasks {
	// Generate all masks as a single bit vector. Each bit offset in this bit
	// vector corresponds to a single mask.
	// In each consecutive kBitsPerMask bits, there must be between
	// kMinBitsSet and kMaxBitsSet bits set.
	BloomFilterMasks() {
		std::seed_seq seed {0, 0, 0, 0, 0, 0, 0, 0};
		std::mt19937 re(seed);
		std::uniform_int_distribution<uint64_t> rd;

		auto random = [&re, &rd](uint64_t min_value, uint64_t max_value) {
			return min_value + rd(re) % (max_value - min_value + 1);
		};

		memset(masks_, 0, kTotalBytes);

		// Prepare the first mask
		uint64_t num_bits_set = random(kMinBitsSet, kMaxBitsSet);
		for (uint64_t i = 0; i < num_bits_set; ++i) {
			while (true) {
				uint64_t bit_pos = random(0, kBitsPerMask - 1);
				if (!GetBit(masks_, bit_pos)) {
					SetBit(masks_, bit_pos);
					break;
				}
			}
		}

		uint64_t num_bits_total = kNumMasks + kBitsPerMask - 1;
		for (uint64_t i = kBitsPerMask; i < num_bits_total; ++i) {
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

	uint64_t Mask(uint64_t hash) const {
		// (Last kNumMasks Used) The lowest bits of hash are used to pick mask index.
		int mask_id = static_cast<int>(hash & (kNumMasks - 1));
		uint64_t result = GetMask(mask_id);

		// () The next set of hash bits is used to pick the amount of bit rotation of the mask.
		int rotation = static_cast<int>((hash >> kLogNumMasks) & 63);
		result = ROTL64(result, rotation);

		return result;
	}

	// Masks are 57 bits long because then they can be accessed at an
	// arbitrary bit offset using a single unaligned 64-bit load instruction.
	static constexpr int kBitsPerMask = 57;
	static constexpr uint64_t kFullMask = (1ULL << kBitsPerMask) - 1;

	// Minimum and maximum number of bits set in each mask.
	// This constraint is enforced when generating the bit masks.
	// Values should be close to each other and chosen as to minimize a Bloom
	// filter false positives rate.
	static constexpr int kMinBitsSet = 4;
	static constexpr int kMaxBitsSet = 5;

	// Number of generated masks.
	// Having more masks to choose will improve false positives rate of Bloom
	// filter but will also use more memory, which may lead to more CPU cache
	// misses.
	// The chosen value results in using only a few cache-lines for mask lookups,
	// while providing a good variety of available bit masks.
	static constexpr int kLogNumMasks = 10;
	static constexpr int kNumMasks = 1 << kLogNumMasks;

	// Data of masks. Masks are stored in a single bit vector. Nth mask is
	// kBitsPerMask bits starting at bit offset N.
	static constexpr int kTotalBytes = (kNumMasks + 64) / 8;
	uint8_t masks_[kTotalBytes];

private:
	bool GetBit(const uint8_t *data, uint64_t bit_pos) const {
		return (data[bit_pos / 8] >> (bit_pos % 8)) & 1;
	}
	void SetBit(uint8_t *data, uint64_t bit_pos) {
		data[bit_pos / 8] |= (1 << (bit_pos % 8));
	}

	uint64_t ROTL64(uint64_t x, int r) const {
		return (x << r) | (x >> ((64 - r) & 63));
	}

	uint64_t GetMask(int bit_offset) const {
		uint64_t value;
		std::memcpy(&value, masks_ + bit_offset / 8, sizeof(uint64_t));
		return (value >> (bit_offset % 8)) & kFullMask;
	}
};

const static BloomFilterMasks masks_;

class RegisterBlockedBF64BitMasks {
public:
	const uint64_t MAX_NUM_BLOCKS = (1UL << 40);
	static constexpr auto MIN_NUM_BITS = 512;

public:
	explicit RegisterBlockedBF64BitMasks(size_t n_key, uint32_t n_bits_per_key) {
		uint32_t min_bits = std::max<uint32_t>(MIN_NUM_BITS, n_key * n_bits_per_key);
		num_blocks = (min_bits >> 6) + 1;
		num_blocks_log = static_cast<uint32_t>(std::log2(num_blocks)) + 1;
		num_blocks = std::min(static_cast<uint64_t>(1ULL << num_blocks_log), MAX_NUM_BLOCKS);

		blocks.resize(num_blocks);
		std::cout << "BF Size: " << num_blocks * 8 / 1024 << " KiB\n";
	}

public:
	inline void Insert(size_t num, uint64_t *key) {
		InsertInternal(num, key, blocks.data());
	}

	inline size_t Lookup(size_t num, uint64_t *key, uint32_t *out) {
		return LookupInternal(num, key, blocks.data(), out);
	}

public:
	size_t LookupInternal(size_t num, uint64_t *BF_RESTRICT key, uint64_t *BF_RESTRICT bf,
	                      uint32_t *BF_RESTRICT out) const {
		for (size_t i = 0; i < num; i++) {
			uint32_t block = (key[i] >> 24) & (num_blocks - 1);
			uint64_t mask = masks_.Mask(key[i]);
			out[i] = (bf[block] & mask) == mask;
		}
		return num;
	}

	void InsertInternal(size_t num, uint64_t *BF_RESTRICT key, uint64_t *BF_RESTRICT bf) const {
		for (size_t i = 0; i < num; i++) {
			uint32_t block = (key[i] >> 24) & (num_blocks - 1);
			uint64_t mask = masks_.Mask(key[i]);
			bf[block] |= mask;
		}
	}

private:
	size_t num_blocks;
	size_t num_blocks_log;
	std::vector<uint64_t> blocks;
};
} // namespace bloom_filters