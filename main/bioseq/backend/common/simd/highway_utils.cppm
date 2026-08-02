/** Canonical generator input for minimizer-owned Highway utilities. */
module;
#include <hwy/highway.h>

export module bioseq.highway.sources.simd_utils;

import std;
import bioseq.common.templating;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;
using ::bioseq::common::templating::for_each_index;

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
		std::abort();
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
		std::abort();
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
		std::abort();
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
		if (K < hn::Lanes(d)) [[unlikely]] {
			std::abort();
		}

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
					if (offset + rows_to_load > data_available) [[unlikely]] {
						std::abort();
					}
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
			std::abort();
		}
	}
};

} // namespace target
} // namespace bioseq::simd
