module;

#include "common.h"
#include <boost/serialization/strong_typedef.hpp>


export module algo.circular_append_buffer;

import mlogging;
import sequan_compat;

export {

BOOST_STRONG_TYPEDEF(size_t, circular_append_buffer_iter);

template<typename T, typename B>
struct empty_circular_append_buffer_allocator {};

template <typename T, size_t S, size_t alignment = 16, bool p_fixed_capacity = false, typename base_array_type = void, template<typename Tt, typename Bt> typename circular_append_buffer_allocator = empty_circular_append_buffer_allocator>
class circular_append_buffer {
public:
    using iterator = circular_append_buffer_iter;
    static constexpr size_t real_size = 1 << seqan3::detail::ceil_log2(S);

    static constexpr size_t size_mask = real_size - 1;
private:
	alignas(alignment) std::conditional_t<std::is_same_v<base_array_type, void>, T[real_size], base_array_type> _buffer;
	
    //T _buffer[real_size];
	
    iterator _buf_pos;
	[[indeterminate]] iterator _prev_pos;
	//size_t _capacity;

    [[no_unique_address]] std::conditional_t<p_fixed_capacity, std::monostate, iterator> _capacity;
    
public:

    explicit constexpr circular_append_buffer() requires (p_fixed_capacity && std::is_same_v<base_array_type, void>) : _buf_pos(0) {}

	explicit constexpr circular_append_buffer(
		size_t capacity
	) requires (!p_fixed_capacity && std::is_same_v<base_array_type, void>) : _buf_pos(0), _capacity(capacity) {}

    explicit constexpr circular_append_buffer() requires (p_fixed_capacity && !std::is_same_v<base_array_type, void>) :
        _buf_pos(0),
        _buffer((circular_append_buffer_allocator<T, base_array_type>::allocate(real_size))) {}

	explicit constexpr circular_append_buffer(
		size_t capacity
	) requires (!p_fixed_capacity && !std::is_same_v<base_array_type, void>) : _buf_pos(0), _capacity(capacity),
    _buffer(std::move(circular_append_buffer_allocator<T, base_array_type>::allocate(capacity))) {}


    [[nodiscard]] static constexpr iterator get_begin() {
        return iterator(0);
    }

    [[nodiscard]] constexpr T& get_value_iter(const iterator& iter) const {
        return _buffer[iter];
    }

    [[nodiscard]] constexpr T* get_ptr_iter(const iterator& iter) const {
        return &_buffer[iter];
    }

	[[nodiscard]] constexpr size_t get_forward_capacity() const {
		return _capacity - _buf_pos;
	}

    constexpr void increment_iter(iterator& iter, size_t increment = 1) requires (!p_fixed_capacity) {
        static_assert(std::is_same_v<decltype(_capacity), iterator>);
        iter += increment;
        while (iter >= _capacity) {
            iter-= _capacity;
        }
    }

    constexpr void increment_iter(iterator& iter, size_t increment = 1) requires (p_fixed_capacity) {
        iter = (iter + increment) & size_mask;
    }

	// void push_back(const T value) {
	// 	_prev_pos = _buf_pos;
	// 	_buffer[_buf_pos] = value;
	// 	increment_iter(_buf_pos);
	// };

    void push_back(T&& value) {
		_prev_pos = _buf_pos;
		_buffer[_buf_pos] = value;
		increment_iter(_buf_pos);
	};

	[[nodiscard]] iterator last_write_pos() const {
		return _prev_pos;
	};

	[[nodiscard]] T& last() {
		return _buffer[_prev_pos];
	}

	template<typename F>
	inline void unsafe_filled_for_each(F fn) requires (!p_fixed_capacity) {
		size_t distance = _capacity - 1;
        [[assume( _buf_pos < _capacity )]];
		for (auto j = _buf_pos; j < _capacity; ++j, --distance) {
			fn(j, _buffer[j], distance);
		}
		for (auto j = get_begin(); j < _buf_pos; ++j, --distance) {
			fn(j, _buffer[j], distance);
		}
	}

	// template<typename F>
	// inline void unsafe_filled_for_each(F fn) requires (!p_fixed_capacity) {
	// 	size_t distance = _capacity - 1;
	// 	[[assume( _buf_pos < _capacity )]];
    //     for (auto j = _buf_pos; j < _capacity; ++j, --distance) {
	// 		fn(j, _buffer[j], distance);
	// 	}
	// 	for (auto j = get_begin(); j <= _buf_pos; ++j, --distance) {
	// 		fn(j, _buffer[j], distance);
	// 	}
	// }
};



}