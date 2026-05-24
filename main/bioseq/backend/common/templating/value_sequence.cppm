/**
 * @file value_sequence.cppm
 * @brief Non-type parameter pack carrier with no constraint on `T`.
 *
 * Replaces `std::integer_sequence<T, Vs...>` which requires `T` to be
 * integral per the C++ standard — libc++'s primary template rejects
 * enum NTTP packs. `value_sequence<T, Vs...>` carries enum, size_t,
 * or any class-type NTTP pack uniformly. Used by
 * `enum_dispatch_pack` / `value_dispatch_smallest_ge` and by every
 * pipeline registry's `value_list`.
 */
module;

#include <cstddef>

export module bioseq_common.templating.value_sequence;

export namespace bioseq::common::templating {

template <class T, T... Vs>
struct value_sequence {
    using value_type = T;
    static constexpr std::size_t size() noexcept { return sizeof...(Vs); }
};

} // namespace
