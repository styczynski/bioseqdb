module;
#include <hwy/highway.h>
#include <hwy/print-inl.h>
#include <hwy/aligned_allocator.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

export module bioseq.simd.utils;
import bioseq.common.mlogging;
import bioseq.common.templating;

// The marked source is the single owner of target-sensitive SIMD helpers.
// Replay it here only after this static module has fixed HWY_TARGET.
HWY_BEFORE_NAMESPACE();

#define BIOSEQ_SIMD_UTILS_STATIC_FACADE 1
#define target HWY_NAMESPACE
#include "highway_utils.cppm"
#undef target
#undef BIOSEQ_SIMD_UTILS_STATIC_FACADE

HWY_AFTER_NAMESPACE();

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
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

void dosomething() {
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

}  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

}  // export
