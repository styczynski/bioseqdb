#pragma once

#include <cstdlib>
#include <new>
#ifndef BF_RESTRICT
#if defined(_MSC_VER)
#define BF_RESTRICT __restrict
#elif defined(__GNUC__) || defined(__clang__)
#define BF_RESTRICT __restrict__
#else
#define BF_RESTRICT
#endif
#endif

#include <cstdint>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <cstdint>

namespace bloom_filters {
// I use the same hash fucntion as in DuckDB.
inline uint64_t MurmurHash64(uint64_t x) {
	x ^= x >> 32;
	x *= 0xd6e8feb86659fd93U;
	x ^= x >> 32;
	x *= 0xd6e8feb86659fd93U;
	x ^= x >> 32;
	return x;
}

inline uint32_t MurmurHash32(uint32_t x) {
	x ^= x >> 16;
	x *= 0xd6e8feb9U;
	x ^= x >> 16;
	x *= 0xd6e8feb9U;
	x ^= x >> 16;
	return x;
}

inline void HashVector(size_t num, const uint64_t *key, uint64_t *hashes) {
	for (size_t i = 0; i < num; i++) {
		hashes[i] = MurmurHash64(key[i]);
	}
}

inline void HashVector(size_t num, const uint64_t *key, uint32_t *hashes) {
	for (size_t i = 0; i < num; i++) {
		hashes[i] = MurmurHash32(key[i]);
	}
}

// 64-byte aligned allocator for cache-sectorized Bloom filter
template <typename T, std::size_t Alignment>
class AlignedAllocator {
public:
	using value_type = T;

	AlignedAllocator() noexcept = default;

	template <class U>
	explicit AlignedAllocator(const AlignedAllocator<U, Alignment> &) noexcept {
	}

	T *allocate(std::size_t n) {
		void *ptr = nullptr;
		if (posix_memalign(&ptr, Alignment, n * sizeof(T)) != 0) {
			throw std::bad_alloc();
		}
		return static_cast<T *>(ptr);
	}

	void deallocate(T *p, std::size_t) noexcept {
		std::free(p);
	}

	template <class U>
	struct rebind {
		using other = AlignedAllocator<U, Alignment>;
	};

	using propagate_on_container_move_assignment = std::true_type;
	using is_always_equal = std::false_type;
};

template <class T1, std::size_t A1, class T2, std::size_t A2>
bool operator==(const AlignedAllocator<T1, A1> &, const AlignedAllocator<T2, A2> &) {
	return A1 == A2;
}

template <class T1, std::size_t A1, class T2, std::size_t A2>
bool operator!=(const AlignedAllocator<T1, A1> &, const AlignedAllocator<T2, A2> &) {
	return A1 != A2;
}
} // namespace bloom_filters