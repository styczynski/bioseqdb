module;
#include <hwy/aligned_allocator.h>
#include <hwy/highway.h>
#include <hwy/print-inl.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

export module bioseq.simd.utils;
import bioseq.common.mlogging;
import bioseq.common.templating;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;
using ::bioseq::common::templating::for_each_index;

template <typename T, typename D>
inline constexpr std::vector<T> extract_lanes(
    const D& d, const hn::Vec<D>& input)
{
    HWY_ALIGN T storage_buffer[hn::MaxLanes(d)];
    hn::StoreU(input, d, storage_buffer);
    return std::vector<T>(
        storage_buffer, storage_buffer + hn::Lanes(d));
}

template <typename T, typename D>
inline constexpr std::vector<T> extract_lanes(
    const D& d, const hn::Mask<D>& input)
{
    return extract_lanes<T>(d, hn::VecFromMask(d, input));
}

template <typename T, typename InfoT, typename I = void>
struct hwy_vector_string_printer {
    static inline std::string format_block(
        const InfoT info, const void* ptr)
    {
        char str[100];
        ToString(info, ptr, str);
        return std::string(str);
    }
};

template <class D, typename I = void, class VecT = hn::Vec<D>>
requires requires(D& d, const VecT& value) {
    Store(value, d, nullptr);
}
auto hwy_str(D& d, const VecT& value) {
    constexpr size_t lane_u = 0;
    constexpr size_t max_lanes = 16;

    const size_t lane_count = hn::Lanes(d);
    using T = hn::TFromD<D>;
#if HWY_TARGET == HWY_RVV
    auto storage = hn::AllocateAligned<T>(lane_count);
    T* HWY_RESTRICT lanes = storage.get();
#else
    HWY_ALIGN T lanes[hn::MaxLanes(d)];
#endif
    Store(value, d, lanes);

    const auto info = hwy::detail::MakeTypeInfo<T>();
    const uint8_t* array_bytes =
        reinterpret_cast<const uint8_t*>(lanes);

    char type_name[100];
    hwy::detail::TypeName(info, lane_count, type_name);

    const intptr_t lane = intptr_t(lane_u);
    const size_t begin = static_cast<size_t>(HWY_MAX(0, lane - 2));
    const size_t end = HWY_MIN(begin + max_lanes, lane_count);

    std::string buffer = fmt::format(
        "hwy::{} [{}+ ->]: [ ", type_name, static_cast<int>(begin));
    for (size_t i = begin; i < end; ++i) {
        const void* ptr = array_bytes + i * info.sizeof_t;
        const auto str =
            hwy_vector_string_printer<T, decltype(info), I>::format_block(
                info, ptr);
        buffer += i == end - 1
            ? fmt::format("{}", str)
            : fmt::format("{}, ", str);
    }
    if (begin >= end) {
        buffer += " (out of bounds)";
    }
    buffer += " ]";
    return buffer;
}

template<typename D, typename T, typename VecT = hn::Vec<D>>
class simd_from_vector {
private:
    hwy::AlignedFreeUniquePtr<VecT[]> storage;

public:
    explicit simd_from_vector(const D& d, std::vector<T> data) {
        const int units_size = data.size() / hn::Lanes(d);
        storage = hwy::AllocateAligned<T>(units_size);
        T const* write_iter = static_cast<T const*>(data.data());
        for (size_t i = 0; i < units_size; ++i) {
            storage[i] = hn::Load(d, &write_iter[i * hn::Lanes(d)]);
        }
    }

    VecT* data() const {
        return *storage;
    }
};

template <typename I, typename D, typename ValT>
auto hwy_str_as(D& d, const ValT& value) {
    return hwy_str<D, I>(d, value);
}

}  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

}
