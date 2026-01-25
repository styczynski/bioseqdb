module;

#include "common.h"
#include <boost/serialization/strong_typedef.hpp>


export module algo.circular_append_buffer;

import mlogging;
import sequan_compat;

export {

BOOST_STRONG_TYPEDEF(size_t, circular_append_buffer_iter);

template <typename T, size_t S, bool p_fixed_capacity = false>
class circular_append_buffer {
public:
    using iterator = circular_append_buffer_iter;
    static constexpr size_t real_size = seqan3::detail::ceil_log2(S);

    static constexpr size_t size_mask = (1 << real_size) - 1;
private:
	T _buffer[real_size];
	iterator _buf_pos;
	iterator _prev_pos;
	//size_t _capacity;

    [[no_unique_address]] std::conditional_t<p_fixed_capacity, std::monostate, iterator> _capacity;
    
public:

    explicit constexpr circular_append_buffer(
        size_t capacity
    ) requires (p_fixed_capacity) : _buf_pos(0), _prev_pos(0) {}

	explicit constexpr circular_append_buffer(
		size_t capacity
	) requires (!p_fixed_capacity) : _buf_pos(0), _prev_pos(0), _capacity(capacity) {}

    static constexpr iterator get_begin() {
        return iterator(0);
    }

    constexpr void increment_iter(iterator& iter) requires (!p_fixed_capacity) {
        if (++iter == _capacity) {
            iter = 0;
        }
    }

    constexpr void increment_iter(iterator& iter) requires (p_fixed_capacity) {
        iter = (iter + 1) & size_mask;
    }

    void push_back(T&& value) {
		_prev_pos = _buf_pos;
		_buffer[_buf_pos] = value;
		increment_iter(_buf_pos);
	};

	iterator last_write_pos() const {
		return _prev_pos;
	};

	T& last() {
		return _buffer[_prev_pos];
	}

	template<typename F>
	inline void unsafe_filled_for_each_previous(F fn) requires (!p_fixed_capacity) {
		for (auto j = _buf_pos; j < _capacity; ++j) {
			fn(j, _buffer[j]);
		}
		for (auto j = get_begin(); j < _buf_pos; ++j) {
			fn(j, _buffer[j]);
		}
	}

	template<typename F>
	inline void unsafe_filled_for_each(F fn) requires (!p_fixed_capacity) {
		for (auto j = _buf_pos; j < _capacity; ++j) {
			fn(j, _buffer[j]);
		}
		for (auto j = get_begin(); j <= _buf_pos; ++j) {
			fn(j, _buffer[j]);
		}
	}
};



}