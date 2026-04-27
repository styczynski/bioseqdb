////JUNK

// Some junk tests

// Junk
#include <iostream>
#include <vector>
#include <array>
#include <memory>
#include <cstdint>
#include <ranges>
#include <algorithm>

namespace detail {
    template<typename Value, std::size_t... Is>
    consteval Value get_bitmask_seq(std::index_sequence<Is...>) {
        return ((static_cast<Value>(1) << static_cast<Value>(Is)) | ...);
    }
    };
    
    template<typename Value, std::size_t bits_per_letter, std::size_t letter_no>
    consteval Value get_letter_shift() {
        return static_cast<Value>(bits_per_letter * letter_no);
    };
    
    template<typename Value, std::size_t bits_per_letter>
    consteval Value get_unit_bitmask() {
        return detail::get_bitmask_seq<Value>(std::make_index_sequence<bits_per_letter>{});
    };
    
    template<typename Value, std::size_t bits_per_letter, std::size_t letter_no>
    consteval Value get_letter_bitmask() {
        return get_unit_bitmask<Value, bits_per_letter>() << get_letter_shift<Value, bits_per_letter, letter_no>();
    };
    
    template<typename Value, std::size_t subunit_bits_count, std::size_t bits_per_letter, std::size_t letter_count, std::size_t letter_no>
    consteval Value get_narrow_subunit_shift() {
        constexpr std::size_t subunits_per_value = sizeof(Value) * 8 / subunit_bits_count; // how many letters we can do per value
        
        // constexpr std::size_t letters_per_subunit = subunit_bits_count / bits_per_letter;
        // constexpr std::size_t max_offset = (letter_count / letters_per_subunit) * letters_per_subunit;
        // constexpr std::size_t current_offset = letter_no * (sizeof(Value) * 8 / );
        // if constexpr (current_offset >= max_offset) {
        //     return static_cast<Value>(max_offset * bits_per_letter);
        // } else {
        //     return static_cast<Value>((max_offset - current_offset) * bits_per_letter);
        // }
        // 
        //return get_narrow_subunit_shift<Value, subunit_bits_count, bits_per_letter, letter_no>()
    };
    
    template<typename Value, std::size_t bits_per_letter, std::size_t letter_no>
    Value extract_unit_letter(const Value& val) {
        return (val & get_letter_bitmask<Value, bits_per_letter, letter_no>()) >> get_letter_shift<Value, bits_per_letter, letter_no>();
    };
    
    namespace detail {
    template<typename Value, std::size_t bits_per_letter, std::size_t letter_count, std::size_t... Is>
    auto extract_letters_from_unit(std::index_sequence<Is...>, const Value& val) -> std::array<Value, sizeof...(Is)> {
        return std::array<Value, sizeof...(Is)>{ extract_unit_letter<Value, bits_per_letter, letter_count - Is - 1>(val) ... };
    };
    
    template<typename Value, std::size_t bits_per_letter, std::size_t letter_count, std::size_t... Is>
    consteval auto get_unit_letter_bitmask_array(std::index_sequence<Is...>) -> std::array<Value, sizeof...(Is)> {
        return std::array<Value, sizeof...(Is)>{ (get_letter_bitmask<Value, bits_per_letter, letter_count - Is - 1>()) ... };
    };
    
    template<typename Value, std::size_t bits_per_letter, std::size_t letter_count, std::size_t... Is>
    consteval auto get_unit_letter_shifts_array(std::index_sequence<Is...>) -> std::array<Value, sizeof...(Is)> {
        return std::array<Value, sizeof...(Is)>{ (get_letter_shift<Value, bits_per_letter, letter_count - Is - 1>()) ... };
    };
    
    template<typename Value, std::size_t subunit_bits_count, std::size_t bits_per_letter, std::size_t letter_count, std::size_t... Is>
    consteval auto get_narrow_subunit_letter_shifts_array(std::index_sequence<Is...>) -> std::array<Value, sizeof...(Is)> {
        return std::array<Value, sizeof...(Is)>{ (get_narrow_subunit_shift<Value, subunit_bits_count, bits_per_letter, letter_count, Is>()) ... };
    };
    
    };
    
    template<
        typename Value,
        std::size_t bits_per_letter,
        std::size_t letter_count = (sizeof(Value) * 8) / bits_per_letter
    >
    constexpr auto extract_letters_from_unit(const Value& val) -> std::array<Value, letter_count> {
        return detail::extract_letters_from_unit<Value, bits_per_letter, letter_count>(std::make_index_sequence<letter_count>{}, val);
    };
    
    template<
        class D,
        typename Value,
        std::size_t bits_per_letter,
        typename output_type = uint8_t,
        typename output_vec_t = hn::Vec<hn::ScalableTag<output_type>>,
        typename vec_t = hn::Vec<D>,
        std::size_t letter_count = (sizeof(Value) * 8) / bits_per_letter
    >
    constexpr auto extract_letters_from_unit_simd(D& d, const Value val) {
        constexpr size_t subunit_bits_count = sizeof(output_type) * 8;
        hn::ScalableTag<output_type> output_d;
    
        HWY_ALIGN constexpr std::array<Value, letter_count> masks = detail::get_unit_letter_bitmask_array<Value, bits_per_letter, letter_count>(std::make_index_sequence<letter_count>{});
        HWY_ALIGN constexpr std::array<Value, letter_count> shifts = detail::get_unit_letter_shifts_array<Value, bits_per_letter, letter_count>(std::make_index_sequence<letter_count>{});
        HWY_ALIGN constexpr std::array<Value, letter_count> subunits_shifts = detail::get_narrow_subunit_letter_shifts_array<Value, subunit_bits_count, bits_per_letter, letter_count>(std::make_index_sequence<letter_count>{});
    
        //static const vec_t masks_vec = hn::LoadN(d, masks.data(), letter_count);
        //static const vec_t shifts_vec = hn::LoadN(d, shifts.data(), letter_count);
        static const vec_t subunits_shifts_vec = hn::LoadN(d, subunits_shifts.data(), letter_count);
        
        const output_vec_t parts_vec = BitCast(output_d, hn::Shr(hn::Set(d, val), subunits_shifts_vec));
    
        std::ranges::for_each(subunits_shifts, [](auto i){ JustLog("subunits_shifts -> {}", i); });
        JustLog("HELLO! subunits_shifts_vec = {}", hwy_str<D, void, vec_t>(d, subunits_shifts_vec));
    
        //hn::Shr(hn::Set(d, val), subunits_shifts);
    
        //return hn::Shr(hn::And(hn::Set(d, val), masks_vec), shifts_vec);
    };
    
    void testextractionlol() {
        uint64_t test = 17280019210;
        hn::ScalableTag<uint64_t> d;
        extract_letters_from_unit_simd<decltype(d), uint64_t, 4>(d, test);
        //auto ret = extract_letters_from_unit_simd<decltype(d), uint64_t, 4>(d, test);
        //JustLog("HELLO! testextractionlol() = {}", hwy_str<decltype(d), void, decltype(ret)>(d, ret));
    };
    
    TEST_CASE("HENLO!", "[lol]") {
        testextractionlol();
    }