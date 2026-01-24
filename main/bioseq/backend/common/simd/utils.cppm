module;
#include <hwy/highway.h>
#include <hwy/print-inl.h>
#include <hwy/aligned_allocator.h>

#include "common.h"

export module bioseq_simd.utils;
import mlogging;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// template <class D, class VecT>
// std::string hwy_str(const D& d, const VecT& v);

template <typename T, typename InfoT, typename I = void>
struct hwy_vector_string_printer {
	static inline std::string format_block(const InfoT info, const void* ptr) {
		char str[100];
		ToString(info, ptr, str);
		return std::string(str);
	}
};

template <class D, typename I = void, class VecT = hn::Vec<D>>
requires (
    requires (const D& d, const VecT& v) {
        { Store(v, d, nullptr) };
    }
)
auto hwy_str(const D& d, const VecT& v) {
	constexpr size_t lane_u = 0;
	constexpr size_t max_lanes = 16;

	const size_t N = hn::Lanes(d);

	using T = hn::TFromD<D>;
	#if HWY_TARGET == HWY_RVV
	auto storage = hn::AllocateAligned<T>(N);
	T* HWY_RESTRICT lanes = storage.get();
	#else
	// This works around an SVE compile error on GCC 11 and 12. Calling
	// AllocateAligned here would seem to require it be marked with HWY_ATTR.
	HWY_ALIGN T lanes[hn::MaxLanes(d)];
	#endif
	Store(v, d, lanes);

	const auto info = hwy::detail::MakeTypeInfo<T>();
	//hwy::detail::PrintArray(info, caption, lanes, N, lane_u, max_lanes);

	const uint8_t* array_bytes = reinterpret_cast<const uint8_t*>(lanes);

	char type_name[100];
	TypeName(info, N, type_name);

	const intptr_t lane = intptr_t(lane_u);
	const size_t begin = static_cast<size_t>(HWY_MAX(0, lane - 2));
	const size_t end = HWY_MIN(begin + max_lanes, N);

	std::string buf = fmt::format("hwy::{} [{}+ ->]: [ ", type_name, static_cast<int>(begin));

	// fprintf(stderr, "%s %s [%d+ ->]:\n  ", type_name, caption,
	// 		static_cast<int>(begin));
	for (size_t i = begin; i < end; ++i) {
		const void* ptr = array_bytes + i * info.sizeof_t;
		//char str[100];
		//ToString(info, ptr, str);
		const auto str = hwy_vector_string_printer<T, decltype(info), I>::format_block(info, ptr);
		if (i == end-1) {
			buf += fmt::format("{}", str);
		} else {
			buf += fmt::format("{}, ", str);
		}
	}
	if (begin >= end) {
		buf += " (out of bounds)";
	}
	buf += " ]";

	return buf;
}


template <class D, class VecT = hn::Vec<D>>
class LaneMatrix {
public:
	using MatT = hwy::AlignedFreeUniquePtr<VecT[]>;

	MatT matrix;

	LaneMatrix(const D& d) : matrix(hwy::AllocateAligned<VecT>(hn::Lanes(d))) {};

	template<typename Functor> 
	inline void fill(const D& d, Functor fn) {
		for(size_t i = 0; i < hn::Lanes(d); ++i) {
			matrix[i] = hn::LoadU(d, fn(i, hn::Lanes(d)));
		}
	}

    template<typename Functor> 
	inline void partial_fill(const D& d, const size_t data_available, Functor fn) {
        const size_t data_available_cap = std::min(data_available, hn::Lanes(d) * hn::Lanes(d));
        const size_t rows_to_load = data_available_cap / hn::Lanes(d);
        const size_t last_row_lanes = data_available_cap % hn::Lanes(d);
        size_t i = 0;
		for(; i < rows_to_load; ++i) {
			matrix[i] = hn::LoadU(d, fn(i, hn::Lanes(d)));
		}
        if (last_row_lanes > 0 && i < hn::Lanes(d)) {
            matrix[i] = hn::LoadN(d, fn(i, hn::Lanes(d)), last_row_lanes);
            ++i;
        }
        for(; i < hn::Lanes(d); ++i) {
			matrix[i] = Zero(d);
		}

	}

	inline VecT& operator[](const size_t&& index) const {
		return matrix[index];
	}

	inline void transpose(const D& d) {
		// For size = 2
		if (hn::Lanes(d) == 1) {
			// No-op
		} else if (hn::Lanes(d) == 2) {
			const VecT v0 = matrix[0]; // A B
			const VecT v1 = matrix[1]; // C D
			const VecT t1 = InterleaveUpper(d, v0, v1); // A C
			const VecT t0 = InterleaveLower(d, v0, v1); // B D
			matrix[0] = t0;
			matrix[1] = t1;
		}
	}
	// 	} else if (hn::Lanes(d) == 4) {
	// 		const hn::RepartitionToWide<D> dw;

	// 		const VecT v0 = matrix[0];
	// 		const VecT v1 = matrix[1];
	// 		const VecT v2 = matrix[2];
	// 		const VecT v3 = matrix[3];

	// 		const VecT t0 = InterleaveLower(d, v0, v1);
	// 		const VecT t1 = InterleaveLower(d, v2, v3);
	// 		const VecT t2 = InterleaveUpper(d, v0, v1);
	// 		const VecT t3 = InterleaveUpper(d, v2, v3);

	// 		matrix[0] = BitCast(d, InterleaveLower(dw, BitCast(dw, t0), BitCast(dw, t1)));
	// 		matrix[1] = BitCast(d, InterleaveUpper(dw, BitCast(dw, t0), BitCast(dw, t1)));
	// 		matrix[2] = BitCast(d, InterleaveLower(dw, BitCast(dw, t2), BitCast(dw, t3)));
	// 		matrix[3] = BitCast(d, InterleaveUpper(dw, BitCast(dw, t2), BitCast(dw, t3)));
	// 	} else if (hn::Lanes(d) == 8) {
	// 		const hn::RepartitionToWide<D> dw;
			
	// 		const VecT v0 = matrix[0];
	// 		const VecT v1 = matrix[1];
	// 		const VecT v2 = matrix[2];
	// 		const VecT v3 = matrix[3];

	// 		const VecT v4 = matrix[4];
	// 		const VecT v5 = matrix[5];
	// 		const VecT v6 = matrix[6];
	// 		const VecT v7 = matrix[7];

	// 		const VecT t0 = InterleaveLower(d, v0, v1);
	// 		const VecT t1 = InterleaveLower(d, v2, v3);
	// 		const VecT t2 = InterleaveLower(d, v4, v5);
	// 		const VecT t3 = InterleaveLower(d, v6, v7); 

	// 		const VecT t4 = InterleaveUpper(d, v0, v1);
	// 		const VecT t5 = InterleaveUpper(d, v2, v3);
	// 		const VecT t6 = InterleaveUpper(d, v4, v5);
	// 		const VecT t7 = InterleaveUpper(d, v6, v7);
			
	// 		const VecT e0 = BitCast(d, InterleaveLower(dw, BitCast(dw, t0), BitCast(dw, t1)));
	// 		const VecT e1 = BitCast(d, InterleaveUpper(dw, BitCast(dw, t0), BitCast(dw, t1)));
	// 		const VecT e2 = BitCast(d, InterleaveLower(dw, BitCast(dw, t2), BitCast(dw, t3)));
	// 		const VecT e3 = BitCast(d, InterleaveUpper(dw, BitCast(dw, t2), BitCast(dw, t3)));
			
	// 		const VecT e4 = BitCast(d, InterleaveLower(dw, BitCast(dw, t4), BitCast(dw, t5)));
	// 		const VecT e5 = BitCast(d, InterleaveUpper(dw, BitCast(dw, t4), BitCast(dw, t5)));
	// 		const VecT e6 = BitCast(d, InterleaveLower(dw, BitCast(dw, t6), BitCast(dw, t7)));
	// 		const VecT e7 = BitCast(d, InterleaveUpper(dw, BitCast(dw, t6), BitCast(dw, t7)));

	// 		const hn::RepartitionToWideX2<D> dww;

	// 		matrix[0] = BitCast(d, InterleaveLower(dww, BitCast(dww, e0), BitCast(dww, e2)));
	// 		matrix[1] = BitCast(d, InterleaveUpper(dww, BitCast(dww, e0), BitCast(dww, e2)));
			
	// 		matrix[2] = BitCast(d, InterleaveLower(dww, BitCast(dww, e1), BitCast(dww, e3)));
	// 		matrix[3] = BitCast(d, InterleaveUpper(dww, BitCast(dww, e1), BitCast(dww, e3)));

	// 		matrix[4] = BitCast(d, InterleaveLower(dww, BitCast(dww, e4), BitCast(dww, e6)));
	// 		matrix[5] = BitCast(d, InterleaveUpper(dww, BitCast(dww, e4), BitCast(dww, e6)));

	// 		matrix[6] = BitCast(d, InterleaveLower(dww, BitCast(dww, e5), BitCast(dww, e7)));
	// 		matrix[7] = BitCast(d, InterleaveUpper(dww, BitCast(dww, e5), BitCast(dww, e7)));
			
	// 	}
	// }
};

template <class D, typename I = void, class VecT = hn::Vec<D>>
auto hwy_str(const D& d, const LaneMatrix<D, VecT>& v) {
	const size_t dim = hn::Lanes(d);
	std::string buf = fmt::format("Matrix [{} x {}] [", dim, dim);
	for(size_t i = 0; i < dim; ++i) {
		buf += "\n" + fmt::format("  {}: {}", i, hwy_str<D, I, VecT>(d, v.matrix[i]));
	}
	buf += "\n]";
	return buf;
}

template <typename I, typename D, typename ValT>
auto hwy_str_as(const D& d, const ValT& v) {
	return hwy_str<D, I>(d, v);
}



}
HWY_AFTER_NAMESPACE();

}