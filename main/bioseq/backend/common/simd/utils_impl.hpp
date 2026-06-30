#pragma once


HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

template<class D, size_t index = 0, typename stored_type = hn::TFromD<D>>
HWY_INLINE stored_type extract_last(D d, hn::Vec<D> v) {
    // ExtractLane on a constant index lowers to a single hardware extract
    // (e.g. NEON `vgetq_lane_s16(v, 7)`), avoiding the previous round-trip
    // through a stack-resident HWY_ALIGN array.
    return hn::ExtractLane(v, hn::Lanes(d) - 1 - index);
}

template<typename D, size_t index = 0>
HWY_INLINE hn::Vec<D> broadcast_last(D d, hn::Vec<D> v) {
    // BroadcastLane requires a compile-time lane index. On NEON / SSE / AVX
    // (our targets), Lanes == MaxLanes, so MaxLanes-1 is the runtime last
    // lane and lowers to a single instruction (e.g. `vdupq_laneq_s16(v, 7)`
    // or `vpbroadcastw`). Leave the safe fallback for SVE/RVV in place.
    constexpr size_t lane_idx = hn::MaxLanes(d) - 1 - index;
    if constexpr (hn::MaxLanes(d) <= 64) {
        return hn::BroadcastLane<lane_idx>(v);
    } else {
        return hn::Set(d, extract_last<D, index>(d, v));
    }
}

template<class D, size_t first_count = 1, typename stored_type = hn::TFromD<D>>
HWY_INLINE hn::Vec<D> insert_first(D d, hn::Vec<D> v, stored_type val) {
    return hn::IfThenElse(hn::FirstN(d, first_count), hn::Set(d, val), v);
}

/// Extract lane at runtime index.
template<class D, typename stored_type = hn::TFromD<D>>
HWY_INLINE stored_type extract_lane(D d, hn::Vec<D> v, size_t lane) {
    HWY_ALIGN stored_type tmp[hn::MaxLanes(d)];
    hn::Store(v, d, tmp);
    return tmp[lane];
}

/// Find the first lane equal to max_val. Returns lane index.
template<class D, typename stored_type = hn::TFromD<D>>
HWY_INLINE size_t lane_hargmax(D d, hn::Vec<D> v, stored_type max_val) {
    const auto idx = hn::FindFirstTrue(d, hn::Eq(v, hn::Set(d, max_val)));
    DEBUG_ASSERT(idx >= 0, "ba_hargmax: max_val must exist in vector");
    return static_cast<size_t>(idx >= 0 ? idx : 0);
}

/// Lane-independent left shift by N i16 positions.
/// Each 128-bit sub-lane shifts independently; low positions fill with zero.
/// Maps to _mm256_bslli_epi128 on AVX2.
template<size_t N, class D>
HWY_INLINE hn::Vec<D> lanes_shift_left_zero(D d, hn::Vec<D> v) {
    return hn::ShiftLeftBytes<N * sizeof(typename hn::TFromD<D>)>(d, v);
}

/// Shift vector `a` left by 1 lane, borrowing the last lane from `b`.
/// Result[0] = b[L-1], Result[i>0] = a[i-1].
///
/// Implemented via CombineShiftRightLanes<L-1>(a, b): concatenates [a:b] and
/// shifts right by L-1 lanes — equivalent to a single hardware EXT/PALIGNR.
/// Avoids the previous Store-then-Load round-trip through a stack array.
template<class D, typename stored_type = hn::TFromD<D>>
HWY_INLINE hn::Vec<D> lanes_shift_left_1_borrow(D d, hn::Vec<D> a, hn::Vec<D> b) {
    constexpr size_t shift_n = hn::MaxLanes(d) - 1;
    if constexpr (hn::MaxLanes(d) <= 64) {
        return hn::CombineShiftRightLanes<shift_n>(d, a, b);
    } else {
        // SVE/RVV fallback (we don't currently target these).
        auto shifted = hn::SlideUpLanes(d, a, 1);
        return hn::IfThenElse(hn::FirstN(d, 1),
                              broadcast_last(d, b),
                              shifted);
    }
}

// template <class D, class VecT>
// std::string hwy_str(const D& d, const VecT& v);

template <typename T, typename D>
inline constexpr std::vector<T> extract_lanes(const D& d, const hn::Vec<D>& input) {
	HWY_ALIGN T storage_buffer[hn::MaxLanes(d)];
	hn::StoreU(input, d, storage_buffer);
	const size_t lanes_no = hn::Lanes(d);
	//DEBUG_ASSERT(lane_no < hn::Lanes(d), "Lane no within bounds");
	return std::vector<T>(storage_buffer, storage_buffer + lanes_no);
}

template <typename T, typename D>
inline constexpr std::vector<T> extract_lanes(const D& d, const hn::Mask<D>& input) {
	return extract_lanes<T>(d, hn::VecFromMask(d, input));
}

template<typename T, typename B>
struct simd_array_allocator {
	static B allocate(const size_t& len) {
		// bioseq_log<DBG>("simd alloc {}", len);
		return hwy::AllocateAligned<T>(len);
	}
};

template <typename T, typename InfoT, typename I = void>
struct hwy_vector_string_printer {
	static inline std::string format_block(const InfoT info, const void* ptr) {
		char str[100];
		ToString(info, ptr, str);
		return std::string(str);
	}
};

template <class D, typename T, class VecT = hn::Vec<D>>
std::vector<T> hwy_to_vector(D& d, const VecT& v) {
	constexpr size_t lane_u = 0;
	constexpr size_t max_lanes = 16;

	const size_t N = hn::Lanes(d);
	#if HWY_TARGET == HWY_RVV
	auto storage = hn::AllocateAligned<T>(N);
	T* HWY_RESTRICT lanes = storage.get();
	#else
	// This works around an SVE compile error on GCC 11 and 12. Calling
	// AllocateAligned here would seem to require it be marked with HWY_ATTR.
	HWY_ALIGN T lanes[hn::MaxLanes(d)];
	#endif
	hn::Store(v, d, lanes);

	return std::vector<T>(lanes, lanes+N);
};

template <class D, typename I = void, class VecT = hn::Vec<D>>
requires (
    requires (D& d, const VecT& v) {
        { Store(v, d, nullptr) };
    }
)
auto hwy_str(D& d, const VecT& v) {
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
	hwy::detail::TypeName(info, N, type_name);

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

template <typename T>
inline constexpr std::pair<T, T> int_div_floor(const T& val, const T& div) {
	return std::make_pair(val / div, val % div);
}

/// DOC0261 §4.5 / §12.6 — SIMD-local pack iteration primitive. Keep this
/// textual helper inside the active Highway target namespace so consumers do
/// not need a second module import while already inside target-dispatched code.
template <std::size_t N, class F>
[[gnu::always_inline]] HWY_INLINE constexpr void for_each_index(F&& fn) {
    [&]<std::size_t... Is>(std::index_sequence<Is...>) [[gnu::always_inline]] {
        (fn(std::integral_constant<std::size_t, Is>{}), ...);
    }(std::make_index_sequence<N>{});
}

template<typename D_IN, typename D_OUT>
concept d_simd_cmp_3x = sizeof(hwy::MakeWide<hwy::MakeWide<hwy::MakeWide<hn::TFromD<D_IN>>>>) == sizeof(hn::TFromD<D_OUT>);

template<typename D_IN, typename D_OUT>
concept d_simd_cmp_2x = sizeof(hwy::MakeWide<hwy::MakeWide<hn::TFromD<D_IN>>>) == sizeof(hn::TFromD<D_OUT>);

template<typename D_IN, typename D_OUT>
concept d_simd_cmp_1x = sizeof(hwy::MakeWide<hn::TFromD<D_IN>>) == sizeof(hn::TFromD<D_OUT>);

template<typename D_IN, typename D_OUT>
concept d_simd_cmp = sizeof(hn::TFromD<D_IN>) == sizeof(hn::TFromD<D_OUT>);

template<typename D>
concept d_simd_resizeable_x3 = sizeof(hwy::MakeWide<hwy::MakeWide<hwy::MakeWide<hn::TFromD<D>>>>) <= 8;

template<typename D>
concept d_simd_resizeable_x2 = sizeof(hwy::MakeWide<hwy::MakeWide<hn::TFromD<D>>>) <= 8;

template<typename D>
concept d_simd_resizeable_x1 = sizeof(hwy::MakeWide<hn::TFromD<D>>) <= 8;

template <class D, class VecT = hn::Vec<D>, typename p_matrix_t = VecT*>
class square_lane_matrix {
private:
	static constexpr void transpose1(D& d, p_matrix_t& matrix) {};

	static constexpr void transpose2(D& d, p_matrix_t& matrix) {
		const VecT v0 = matrix[0]; // A B
		const VecT v1 = matrix[1]; // C D
		const VecT t1 = InterleaveUpper(d, v0, v1); // A C
		const VecT t0 = InterleaveLower(d, v0, v1); // B D
		matrix[0] = t0;
		matrix[1] = t1;
	};

	static constexpr void transpose4(D& d, p_matrix_t& matrix) requires (!d_simd_resizeable_x1<D>) {
		ASSERT(false);
	}

	static constexpr void transpose4(D& d, p_matrix_t& matrix) requires (d_simd_resizeable_x1<D>) {
		const hn::RepartitionToWide<D> dw;

		const VecT v0 = matrix[0];
		const VecT v1 = matrix[1];
		const VecT v2 = matrix[2];
		const VecT v3 = matrix[3];

		const VecT t0 = InterleaveLower(d, v0, v1);
		const VecT t1 = InterleaveLower(d, v2, v3);
		const VecT t2 = InterleaveUpper(d, v0, v1);
		const VecT t3 = InterleaveUpper(d, v2, v3);

		matrix[0] = BitCast(d, InterleaveLower(dw, BitCast(dw, t0), BitCast(dw, t1)));
		matrix[1] = BitCast(d, InterleaveUpper(dw, BitCast(dw, t0), BitCast(dw, t1)));
		matrix[2] = BitCast(d, InterleaveLower(dw, BitCast(dw, t2), BitCast(dw, t3)));
		matrix[3] = BitCast(d, InterleaveUpper(dw, BitCast(dw, t2), BitCast(dw, t3)));
	};

	static constexpr void transpose8(D& d, p_matrix_t& matrix) requires (!d_simd_resizeable_x2<D>) {
		ASSERT(false);
	}

	static constexpr void transpose8(D& d, p_matrix_t& matrix) requires (d_simd_resizeable_x2<D>) {
		const hn::RepartitionToWide<D> dw;
		
		const VecT v0 = matrix[0];
		const VecT v1 = matrix[1];
		const VecT v2 = matrix[2];
		const VecT v3 = matrix[3];

		const VecT v4 = matrix[4];
		const VecT v5 = matrix[5];
		const VecT v6 = matrix[6];
		const VecT v7 = matrix[7];

		const VecT t0 = InterleaveLower(d, v0, v1);
		const VecT t1 = InterleaveLower(d, v2, v3);
		const VecT t2 = InterleaveLower(d, v4, v5);
		const VecT t3 = InterleaveLower(d, v6, v7); 

		const VecT t4 = InterleaveUpper(d, v0, v1);
		const VecT t5 = InterleaveUpper(d, v2, v3);
		const VecT t6 = InterleaveUpper(d, v4, v5);
		const VecT t7 = InterleaveUpper(d, v6, v7);
		
		const VecT e0 = BitCast(d, InterleaveLower(dw, BitCast(dw, t0), BitCast(dw, t1)));
		const VecT e1 = BitCast(d, InterleaveUpper(dw, BitCast(dw, t0), BitCast(dw, t1)));
		const VecT e2 = BitCast(d, InterleaveLower(dw, BitCast(dw, t2), BitCast(dw, t3)));
		const VecT e3 = BitCast(d, InterleaveUpper(dw, BitCast(dw, t2), BitCast(dw, t3)));
		
		const VecT e4 = BitCast(d, InterleaveLower(dw, BitCast(dw, t4), BitCast(dw, t5)));
		const VecT e5 = BitCast(d, InterleaveUpper(dw, BitCast(dw, t4), BitCast(dw, t5)));
		const VecT e6 = BitCast(d, InterleaveLower(dw, BitCast(dw, t6), BitCast(dw, t7)));
		const VecT e7 = BitCast(d, InterleaveUpper(dw, BitCast(dw, t6), BitCast(dw, t7)));

		const hn::RepartitionToWideX2<D> dww;

		matrix[0] = BitCast(d, InterleaveLower(dww, BitCast(dww, e0), BitCast(dww, e2)));
		matrix[1] = BitCast(d, InterleaveUpper(dww, BitCast(dww, e0), BitCast(dww, e2)));
		
		matrix[2] = BitCast(d, InterleaveLower(dww, BitCast(dww, e1), BitCast(dww, e3)));
		matrix[3] = BitCast(d, InterleaveUpper(dww, BitCast(dww, e1), BitCast(dww, e3)));

		matrix[4] = BitCast(d, InterleaveLower(dww, BitCast(dww, e4), BitCast(dww, e6)));
		matrix[5] = BitCast(d, InterleaveUpper(dww, BitCast(dww, e4), BitCast(dww, e6)));

		matrix[6] = BitCast(d, InterleaveLower(dww, BitCast(dww, e5), BitCast(dww, e7)));
		matrix[7] = BitCast(d, InterleaveUpper(dww, BitCast(dww, e5), BitCast(dww, e7)));
	};

	static constexpr void transpose16(D& d, p_matrix_t& matrix) requires (!d_simd_resizeable_x3<D>) {
		ASSERT(false);
	}

	static constexpr void transpose16(D& d, p_matrix_t& matrix) requires (d_simd_resizeable_x3<D>) {
		auto matrix_lower = &matrix[8];
		transpose8(d, matrix);
		transpose8(d, matrix_lower);

		const VecT e0 = matrix[0];
		const VecT e1 = matrix[1];
		const VecT e2 = matrix[2];
		const VecT e3 = matrix[3];
		const VecT e4 = matrix[4];
		const VecT e5 = matrix[5];
		const VecT e6 = matrix[6];
		const VecT e7 = matrix[7];

		const VecT e8 = matrix[8];
		const VecT e9 = matrix[9];
		const VecT e10 = matrix[10];
		const VecT e11 = matrix[11];
		const VecT e12 = matrix[12];
		const VecT e13 = matrix[13];
		const VecT e14 = matrix[14];
		const VecT e15 = matrix[15];

		const hn::RepartitionToWideX3<D> dww;
		
		matrix[0] = BitCast(d, InterleaveLower(dww, BitCast(dww, e0), BitCast(dww, e8)));
		matrix[1] = BitCast(d, InterleaveUpper(dww, BitCast(dww, e0), BitCast(dww, e8)));
		matrix[2] = BitCast(d, InterleaveLower(dww, BitCast(dww, e1), BitCast(dww, e9)));
		matrix[3] = BitCast(d, InterleaveUpper(dww, BitCast(dww, e1), BitCast(dww, e9)));
		matrix[4] = BitCast(d, InterleaveLower(dww, BitCast(dww, e2), BitCast(dww, e10)));
		matrix[5] = BitCast(d, InterleaveUpper(dww, BitCast(dww, e2), BitCast(dww, e10)));
		matrix[6] = BitCast(d, InterleaveLower(dww, BitCast(dww, e3), BitCast(dww, e11)));
		matrix[7] = BitCast(d, InterleaveUpper(dww, BitCast(dww, e3), BitCast(dww, e11)));

		matrix[8] = BitCast(d, InterleaveLower(dww, BitCast(dww, e4), BitCast(dww, e12)));
		matrix[9] = BitCast(d, InterleaveUpper(dww, BitCast(dww, e4), BitCast(dww, e12)));
		matrix[10] = BitCast(d, InterleaveLower(dww, BitCast(dww, e5), BitCast(dww, e13)));
		matrix[11] = BitCast(d, InterleaveUpper(dww, BitCast(dww, e5), BitCast(dww, e13)));
		matrix[12] = BitCast(d, InterleaveLower(dww, BitCast(dww, e6), BitCast(dww, e14)));
		matrix[13] = BitCast(d, InterleaveUpper(dww, BitCast(dww, e6), BitCast(dww, e14)));
		matrix[14] = BitCast(d, InterleaveLower(dww, BitCast(dww, e7), BitCast(dww, e15)));
		matrix[15] = BitCast(d, InterleaveUpper(dww, BitCast(dww, e7), BitCast(dww, e15)));
	};
public:
	using vec_t = VecT;
	using matrix_t = p_matrix_t;

	p_matrix_t matrix;

	explicit square_lane_matrix(p_matrix_t p_matrix): matrix(p_matrix) {}

	// template<typename T>
	// constexpr std::vector<std::vector<T>> to_vectors(D& d) const {
	// 	std::vector<std::vector<T>> ret;
	// 	const size_t rows = hn::Lanes(d);
	// 	ret.resize(rows);
	// 	for(size_t i = 0; i < rows; ++i) {
	// 		hn::Store();
	// 		ret[i] = ;
	// 	}
	// }

	template<typename T>
	std::vector<std::vector<T>> to_scalar_2d(D& d) const {
		std::vector<std::vector<T>> data;
		const size_t rows_to_load = hn::Lanes(d);
		data.resize(rows_to_load);
		for(size_t i = 0; i < rows_to_load; ++i) {
			data[i] = hwy_to_vector<D, T, VecT>(d, matrix[i]);
		}
		return data;
	}

	template<typename T, size_t K> 
	constexpr size_t fill_with_offsets(
		D& d, const std::array<size_t, K> offsets, const T* data
	) {
		assert(K >= hn::Lanes(d));
		assert(std::is_sorted(offsets));
		
		const size_t rows_to_load = hn::Lanes(d);
		for(size_t i = 0; i < rows_to_load; ++i) {
			matrix[i] = hn::LoadU(d, &(data[offsets[i]]));
		}
		return rows_to_load;
	}

	template<typename T, size_t K>
	constexpr std::pair<size_t, size_t> partial_fill_with_offsets(
		D& d, const size_t data_available, const std::array<size_t, K> offsets, const T* data
	) {
		DEBUG_ASSERT(K >= hn::Lanes(d), "Number of offsets cannot be larger than number of lanes");

		const size_t rows_to_load = hn::Lanes(d);
		size_t partial_filled_rows = 0;
		size_t full_rows_count = 0;
		size_t i = 0;
		for(; i < K; ++i) {
			const size_t offset = offsets[i];
			if (offset + rows_to_load > data_available) [[unlikely]] {
				// We cannot load entire row
				if (offset >= data_available) [[unlikely]] {
					matrix[i] = hn::Zero(d);
				} else {
					const size_t rows_to_load = data_available - offset;
					DEBUG_ASSERT(offset + rows_to_load <= data_available, "Cannot load past available data");
					matrix[i] = hn::LoadN(d, &(data[offset]), rows_to_load);
					partial_filled_rows = rows_to_load;
				}
			} else {
				matrix[i] = hn::LoadU(d, &(data[offset]));
				++full_rows_count;
			}
		}
		for(; i < rows_to_load; ++i) { 
			matrix[i] = hn::Zero(d);
		}
		return std::make_pair(full_rows_count, partial_filled_rows);
	}

	// OLD IMPL
	// template<typename T, size_t K>
	// constexpr std::pair<size_t, size_t> partial_fill_with_offsets(
	// 	D& d, const size_t data_available, const std::array<size_t, K> offsets, const T* data
	// ) {
	// 	assert(K >= hn::Lanes(d));
	// 	assert(std::is_sorted(offsets));

	// 	const size_t rows_to_load = hn::Lanes(d);
	// 	size_t i = 0;
	// 	size_t data_p = 0;
	// 	size_t partial_filled_rows = 0;
	// 	for(; i < rows_to_load; ++i) {
	// 		data_p = offsets[i];
	// 		if (data_p + rows_to_load > data_available) {
	// 			// We cannot load entire row
	// 			partial_filled_rows = data_p + rows_to_load - data_available;
	// 			break;
	// 		}
	// 		matrix[i] = hn::LoadU(d, &(data[data_p]));
	// 	}
	// 	const size_t full_rows_count = i;
	// 	if (i < rows_to_load && partial_filled_rows > 0) {
	// 		matrix[i] = hn::LoadN(d, &(data[data_p]), partial_filled_rows);
	// 		++i;
	// 	}
	// 	for(; i < hn::Lanes(d); ++i) {
	// 		matrix[i] = Zero(d);
	// 	}
	// 	return std::make_pair(full_rows_count, partial_filled_rows);
	// }

	template<typename T> 
	constexpr size_t fill(
		D& d, const size_t context, const T* data
	) {
		assert(context <= hn::Lanes(d));
		const size_t rows_to_load = hn::Lanes(d);
		const size_t row_offset = rows_to_load - context;
		for(size_t i = 0; i < rows_to_load; ++i) {
			matrix[i] = hn::LoadU(d, &(data[i * row_offset]));
		}
		return rows_to_load * row_offset;
	}

	template<typename T>
	constexpr std::pair<size_t, size_t> partial_fill(
		D& d, const size_t data_available, const size_t context, const T* data
	) {
		assert(context <= hn::Lanes(d));

		if (data_available < context) [[unlikely]] {
			const size_t loaded_units = std::min(data_available, hn::Lanes(d));
			matrix[0] = hn::LoadN(d, data, loaded_units);
			for(size_t i = 1; i < hn::Lanes(d); ++i) {
				matrix[i] = Zero(d);
			}
			const size_t loaded_rows = 0;
			return std::make_pair(loaded_rows, loaded_units);
		} else {
			//const size_t data_available_cap = std::min(data_available, hn::Lanes(d) * hn::Lanes(d));
			const size_t row_offset = hn::Lanes(d) - context;
			const auto& [full_rows_count, last_row_lanes] = int_div_floor(data_available-context, row_offset);
			const size_t rows_to_load = 1+full_rows_count; //data_available_cap / hn::Lanes(d);
			size_t partial_filled_rows = 0;
			size_t i = 0;
			for(; i < rows_to_load; ++i) {
				matrix[i] = hn::LoadU(d, &(data[i * row_offset]));
			}
			if (last_row_lanes > 0 && i < hn::Lanes(d)) {
				partial_filled_rows = context+last_row_lanes;
				matrix[i] = hn::LoadN(d, &(data[i * row_offset]), partial_filled_rows);
				++i;
			}
			for(; i < hn::Lanes(d); ++i) {
				matrix[i] = Zero(d);
			}
			return std::make_pair(full_rows_count, partial_filled_rows);
			//rows_to_load * row_offset + last_row_lanes;
		}
	}

	inline constexpr void transpose(
		D& d
	) {
		// For size = 2
		if (hn::Lanes(d) == 1) {
			transpose1(d, matrix);
		} else if (hn::Lanes(d) == 2) {
			transpose2(d, matrix);
		} else if (hn::Lanes(d) == 4) {
			transpose4(d, matrix);
		} else if (hn::Lanes(d) == 8) {
			transpose8(d, matrix);
		} else if (hn::Lanes(d) == 16) {
			transpose16(d, matrix);
		} else {
			ASSERT(false, fmt::format("square_lane_matrix does not support transpose() for lane number: {}", hn::Lanes(d)));
		}
	}
};

template<typename D, typename T, typename VecT = hn::Vec<D>>
class simd_from_vector {
private:
	hwy::AlignedFreeUniquePtr<VecT[]> storage;
public:
	explicit simd_from_vector(
		const D& d,
		std::vector<T> data
	) {
		const int units_size = data.size() / hn::Lanes(d);
    	storage = hwy::AllocateAligned<T>(units_size);
		T const* write_iter = static_cast<T const*>(data.data());
    	for(size_t i=0; i<units_size; ++i) {
			storage[i] = hn::Load(d, &write_iter[i*hn::Lanes(d)]);
		}
	}

	VecT* data() const {
		return *storage;
	}
};

template <class D, typename I = void, class VecT = hn::Vec<D>, typename p_matrix_t = VecT*>
auto hwy_str(D& d, const square_lane_matrix<D, VecT, p_matrix_t>& v) {
	const size_t dim = hn::Lanes(d);
	std::string buf = fmt::format("LMatrix [{} x {}] [", dim, dim);
	for(size_t i = 0; i < dim; ++i) {
		buf += "\n" + fmt::format("  {}: {}", i, hwy_str<D, I, VecT>(d, v.matrix[i]));
	}
	buf += "\n]";
	return buf;
}

template <typename I, typename D, typename ValT>
auto hwy_str_as(D& d, const ValT& v) {
	return hwy_str<D, I>(d, v);
}


template <class D_IN, class vec_in_t = hn::Vec<D_IN>>
struct vector_upscaler {
public:
	// using x1_upscaled_t = std::array<hn::RepartitionToWide<D_IN>, 2>;
	// using x2_upscaled_t = std::array<hn::RepartitionToWide<hn::RepartitionToWide<D_IN>>, 4>;
private:
	// static constexpr x1_upscaled_t upscale1(D_IN& d_in, vec_in_t v) requires (!d_simd_resizeable_x1<D_IN>) {
	// 	std::unreachable();
	// };

	// static constexpr x1_upscaled_t upscale1(D_IN& d_in, vec_in_t v) requires (d_simd_resizeable_x1<D_IN>) {
	// 	const hn::RepartitionToWide<D_IN> d_out;
	// 	vec_in_t z0 = Zero(d_in);
	// 	return x1_upscaled_t{
	// 		BitCast(d_out, InterleaveLower(d_in, v, z0)),
	// 		BitCast(d_out, InterleaveUpper(d_in, v, z0)),
	// 	};
	// };

	// static constexpr x2_upscaled_t upscale2(D_IN& d_in, vec_in_t v) requires (!d_simd_resizeable_x2<D_IN>) {
	// 	std::unreachable();
	// };

	// static constexpr x2_upscaled_t upscale2(D_IN& d_in, vec_in_t v) requires (d_simd_resizeable_x2<D_IN>) {
	// 	const hn::RepartitionToWide<D_IN> dw;
	// 	const hn::RepartitionToWideX2<D_IN> dww;
	// 	using vec_dw = hn::Vec<decltype(dw)>;
	// 	vec_in_t z0 = Zero(d_in);
	// 	vec_dw v0 = Zero(dw);
	// 	vec_dw v1 = BitCast(dw, InterleaveLower(d_in, v, z0));
	// 	vec_dw v2 = BitCast(dw, InterleaveUpper(d_in, v, z0));
	// 	return x2_upscaled_t{
	// 		BitCast(dww, InterleaveLower(dw, v1, v0)),
	// 		BitCast(dww, InterleaveUpper(dw, v1, v0)),
	// 		BitCast(dww, InterleaveLower(dw, v2, v0)),
	// 		BitCast(dww, InterleaveUpper(dw, v2, v0)),
	// 	};
	// };
public:

	template <class D_OUT, class vec_out_t = hn::Vec<D_OUT>>
	struct rescaled_type { using type = void; };

	template <class D_OUT> requires (d_simd_cmp<D_IN, D_OUT>)
	struct rescaled_type<D_OUT> { using type = std::array<hn::Vec<D_OUT>, 1>; };

	template <class D_OUT> requires (d_simd_cmp_1x<D_IN, D_OUT>)
	struct rescaled_type<D_OUT> { using type = std::array<hn::Vec<D_OUT>, 2>; };

	template <class D_OUT> requires (d_simd_cmp_2x<D_IN, D_OUT>)
	struct rescaled_type<D_OUT> { using type = std::array<hn::Vec<D_OUT>, 4>; };

	template <class D_OUT> requires (d_simd_cmp_3x<D_IN, D_OUT>)
	struct rescaled_type<D_OUT> { using type = std::array<hn::Vec<D_OUT>, 8>; };

	template <class D_OUT>
	using rescaled_t = rescaled_type<D_OUT>::type;

	template <class D_OUT, class vec_out_t = hn::Vec<D_OUT>>
	[[always_inline]] static constexpr rescaled_t<D_OUT> upscale(
		D_IN& d_in,
		D_OUT& d_out,
		vec_in_t v
	) requires (d_simd_cmp<D_IN, D_OUT>) {
		return {v};
	};

	template <class D_OUT, class vec_out_t = hn::Vec<D_OUT>>
	[[always_inline]] static constexpr rescaled_t<D_OUT> upscale(
		D_IN& d_in,
		D_OUT& d_out,
		vec_in_t v
	) requires (d_simd_cmp_1x<D_IN, D_OUT>) {
		vec_in_t z0 = Zero(d_in);
		return {
			BitCast(d_out, InterleaveLower(d_in, v, z0)),
			BitCast(d_out, InterleaveUpper(d_in, v, z0)),
		};
	};

	template <class D_OUT, class vec_out_t = hn::Vec<D_OUT>>
	[[always_inline]] static constexpr rescaled_t<D_OUT> upscale(
		D_IN& d_in,
		D_OUT& d_out,
		vec_in_t v
	) requires (d_simd_cmp_2x<D_IN, D_OUT>) {
		const hn::RepartitionToWide<D_IN> dw;
		//const hn::RepartitionToWideX2<D_IN> dww;
		using vec_dw = hn::Vec<decltype(dw)>;
		vec_in_t z0 = Zero(d_in);
		vec_dw v0 = Zero(dw);
		vec_dw v1 = BitCast(dw, InterleaveLower(d_in, v, z0));
		vec_dw v2 = BitCast(dw, InterleaveUpper(d_in, v, z0));
		return {
			BitCast(d_out, InterleaveLower(dw, v1, v0)),
			BitCast(d_out, InterleaveUpper(dw, v1, v0)),
			BitCast(d_out, InterleaveLower(dw, v2, v0)),
			BitCast(d_out, InterleaveUpper(dw, v2, v0)),
		};
	};

	template <class D_OUT, class vec_out_t = hn::Vec<D_OUT>>
	[[always_inline]] static constexpr rescaled_t<D_OUT> upscale(
		D_IN& d_in,
		D_OUT& d_out,
		vec_in_t v
	) requires (d_simd_cmp_3x<D_IN, D_OUT>) {
		const hn::RepartitionToWide<D_IN> dw;
		using vec_dw = hn::Vec<decltype(dw)>;
		const hn::RepartitionToWideX2<D_IN> dww;
		using vec_dww = hn::Vec<decltype(dww)>;
		vec_in_t z0 = Zero(d_in);
		vec_dw v0 = Zero(dw);
		vec_dw v1 = BitCast(dw, InterleaveLower(d_in, v, z0));
		vec_dw v2 = BitCast(dw, InterleaveUpper(d_in, v, z0));
		vec_dww t0 = Zero(dww);
		vec_dww t1 = BitCast(dww, InterleaveLower(dw, v1, v0));
		vec_dww t2 = BitCast(dww, InterleaveUpper(dw, v1, v0));
		vec_dww t3 = BitCast(dww, InterleaveLower(dw, v2, v0));
		vec_dww t4 = BitCast(dww, InterleaveUpper(dw, v2, v0));
		return {
			BitCast(d_out, InterleaveLower(dww, t1, t0)),
			BitCast(d_out, InterleaveUpper(dww, t1, t0)),
			BitCast(d_out, InterleaveLower(dww, t2, t0)),
			BitCast(d_out, InterleaveUpper(dww, t2, t0)),
			BitCast(d_out, InterleaveLower(dww, t3, t0)),
			BitCast(d_out, InterleaveUpper(dww, t3, t0)),
			BitCast(d_out, InterleaveLower(dww, t4, t0)),
			BitCast(d_out, InterleaveUpper(dww, t4, t0)),
		};
	};
};

inline void dosomething() {
	const hn::ScalableTag<uint8_t> d_in;
	using D_IN = decltype(d_in);

	const hn::ScalableTag<uint64_t> d_out;
	using D_OUT = decltype(d_out);

    using vec_in_t = hn::Vec<D_IN>;
	using vec_out_t = hn::Vec<D_OUT>;

	uint8_t data[36] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36};
	vec_in_t v = hn::LoadU(d_in, &(data[0]));

	typename vector_upscaler<D_IN>::rescaled_t<D_OUT> outp = vector_upscaler<D_IN>::upscale(d_in, d_out, v);
	bioseq_log_lazy<DBG>([&] {
		bioseq_log<DBG>("this is input    => {}", hwy_str<D_IN, void, vec_in_t>(d_in, v));
		for (auto& scaled : outp) {
			bioseq_log<DBG>("this is upscaled => {}", hwy_str<D_OUT, void, vec_out_t>(d_out, scaled));
		}
	});
}

/// Vectorized fast_log2 approximation using IEEE 754 bit tricks.
///
/// Computes an approximate log2 for each int32 lane by:
///   1. Converting int32 -> float
///   2. Extracting the IEEE 754 exponent as the integer part
///   3. Normalizing the mantissa to [1, 2)
///   4. Evaluating a degree-2 polynomial for the fractional part
///
/// Polynomial coefficients: log2(x) ~ exponent + (-0.34484843*m + 2.02466578)*m - 0.67487759
/// Accurate to ~3 significant figures for input >= 2.
///
/// Matches minimap2's mg_log2() and TAL's mg_log2_avx2().
///
/// @tparam D_I  Highway int32 tag type
/// @tparam D_F  Highway float32 tag type (must have same lane count as D_I)
/// @param d_int    Highway int32 tag
/// @param d_float  Highway float32 tag
/// @param input_int  Vector of int32 values (must be >= 2 for accuracy)
/// @return Vector of float32 log2 approximations
template<class D_I, class D_F>
HWY_INLINE hn::Vec<D_F> vectorized_fast_log2(
    D_I d_int, D_F d_float, hn::Vec<D_I> input_int)
{
    const auto input_as_float = hn::ConvertTo(d_float, input_int);
    const auto float_as_int_bits = hn::BitCast(d_int, input_as_float);

    // Extract exponent: ((bits >> 23) & 255) - 128
    const auto raw_exponent = hn::And(hn::ShiftRight<23>(float_as_int_bits), hn::Set(d_int, 255));
    const auto biased_exponent = hn::Sub(raw_exponent, hn::Set(d_int, 128));
    const auto exponent_as_float = hn::ConvertTo(d_float, biased_exponent);

    // Isolate mantissa and normalize to [1, 2)
    const auto exponent_clear_mask = hn::Set(d_int, ~(255 << 23));
    const auto exponent_rebias = hn::Set(d_int, 127 << 23);
    const auto mantissa_bits = hn::Add(
        hn::And(float_as_int_bits, exponent_clear_mask), exponent_rebias);
    const auto normalized_mantissa = hn::BitCast(d_float, mantissa_bits);

    // Polynomial: (-0.34484843 * m + 2.02466578) * m - 0.67487759
    constexpr float poly_c1 = -0.34484843f;
    constexpr float poly_c2 =  2.02466578f;
    constexpr float poly_c3 =  0.67487759f;

    const auto inner_term = hn::MulAdd(
        hn::Set(d_float, poly_c1), normalized_mantissa, hn::Set(d_float, poly_c2));
    const auto fractional_part = hn::MulSub(inner_term, normalized_mantissa, hn::Set(d_float, poly_c3));

    return hn::Add(exponent_as_float, fractional_part);
}

}
HWY_AFTER_NAMESPACE();
