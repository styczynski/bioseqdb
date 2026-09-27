/**
 * @file kernel.cppm
 * @brief Shared packed-input views, output callbacks and minimizer kernel calls.
 */
module;

#include <algorithm>
#include <array>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.sequences.kmers.highway_minimizer_contract:kernel;

import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.storage.size;
import :representation;
import :collectors;
import :execution;

export namespace bioseq::minimizers {

/** @brief Sentinel for a packed view whose alphabet has not been set. */
inline constexpr std::uint8_t k_unbound_packed_alphabet_code =
    (std::numeric_limits<std::uint8_t>::max)();

/**
 * @brief Borrows packed symbols in the local range [begin_symbol, end_symbol).
 *
 * storage starts at an aligned storage word. Symbol offsets are relative
 * to that pointer, with the first symbol in the lowest bits of each word.
 * Continuation state tracks the position in the full sequence.
 */
struct packed_sequence_kernel_view final {
    const std::byte* storage = nullptr;
    std::size_t storage_units = 0u;
    std::size_t begin_symbol = 0u;
    std::size_t end_symbol = 0u;
    std::uint8_t alphabet_code = k_unbound_packed_alphabet_code;
    std::uint8_t bits_per_symbol = 0u;
    std::uint8_t storage_unit_bytes = 0u;
    bool final_segment = false;
};

/** @brief Writable space for a batch of one specified minimizer record format. */
struct minimizer_sink_batch_view final {
    void* data = nullptr;
    std::size_t capacity = 0u;
    minimizer_output_layout layout = minimizer_output_layout::unsupported;
    std::size_t record_size = 0u;
    std::size_t record_alignment = 0u;
};

static_assert(std::is_standard_layout_v<minimizer_sink_batch_view>);
static_assert(std::is_trivially_copyable_v<minimizer_sink_batch_view>);

/**
 * @brief Lets a kernel acquire output space and commit batches of records.
 *
 * acquire returns uninitialized storage. The collector checks its format,
 * size and alignment before constructing records there. append_raw_u64 is
 * an optional callback for sending packed 64-bit records directly.
 */
struct minimizer_sink final {
    void* state = nullptr;
    minimizer_sink_batch_view (*acquire)(
        void*, std::size_t minimum) = nullptr;
    void (*commit)(void*, std::size_t initialized) noexcept = nullptr;
    bool (*append_raw_u64)(
        void*,
        minimizer_output_layout,
        const std::uint64_t*,
        std::size_t) noexcept = nullptr;
};

/** @brief Describes one packed fragment borrowed for a synchronous kernel call. */
struct packed_sequence_kernel_fragment final {
    const std::byte* storage = nullptr;
    std::size_t storage_units = 0u;
    std::size_t fragment_bp_size = 0u;
    std::size_t fragment_begin_bp = 0u;
    std::size_t consume_begin_bp = 0u;
    std::size_t consume_end_bp = 0u;
};

/**
 * @brief Reads and releases borrowed fragments during one synchronous kernel call.
 * The kernel keeps its continuation state while consuming the fragment interval.
 */
struct packed_sequence_fragment_source final {
    void* state = nullptr;
    std::size_t first_fragment = 0u;
    std::size_t fragment_end = 0u;
    packed_sequence_kernel_fragment (*read)(void*, std::size_t) = nullptr;
    void (*release)(void*, std::size_t) noexcept = nullptr;

    /** @brief Reports whether either fragment callback is configured. */
    [[nodiscard]] constexpr bool is_present() const noexcept {
        return read != nullptr || release != nullptr;
    }
};

/**
 * @brief Writes minimizer records directly into batches supplied by the output sink.
 *
 * Requests space lazily, commits full batches and keeps a final partial batch
 * until flush() is called. The caller must check is_ok() because collect()
 * cannot report a rejected sink.
 */
template<class p_minimizer_type>
class minimizer_sink_collector {
public:
    using minimizer_type = p_minimizer_type;
    static constexpr std::size_t k_batch_capacity = 256u;
    static constexpr minimizer_output_layout k_output_layout =
        k_minimizer_output_layout<p_minimizer_type>;

    static_assert(k_output_layout != minimizer_output_layout::unsupported);
    static_assert(std::is_standard_layout_v<p_minimizer_type>);
    static_assert(std::is_trivially_copyable_v<p_minimizer_type>);
    static_assert(std::is_trivially_destructible_v<p_minimizer_type>);

    /** @brief Binds the sink and checks that acquire and commit callbacks are present. */
    explicit minimizer_sink_collector(minimizer_sink sink) noexcept
        : sink_(sink),
          failed_(sink_.acquire == nullptr || sink_.commit == nullptr) {}

    /**
     * @brief Queues one movable minimizer record.
     * @param sketch Record transferred into the pending batch.
     */
    void collect(p_minimizer_type&& sketch) {
        enqueue(std::move(sketch));
    }

    /**
     * @brief Queues one copied minimizer record.
     * @param sketch Record copied into the pending batch.
     */
    void collect(const p_minimizer_type& sketch) {
        enqueue(sketch);
    }

    /**
     * @brief Sends packed records through the raw callback or reconstructs them in batches.
     * Flushes pending records first.
     * @return false if the sink rejects raw records or supplies invalid output storage.
     */
    template<class p_record_type = p_minimizer_type>
    [[nodiscard]] bool append_raw_storage(
        const typename p_record_type::storage_type* raw,
        std::size_t count)
        requires (
            std::same_as<p_record_type, p_minimizer_type>
            && requires { typename p_record_type::storage_type; }
            && std::is_trivially_copyable_v<p_record_type>
            && sizeof(p_record_type) == sizeof(typename p_record_type::storage_type)
            && requires (typename p_record_type::storage_type raw_value, p_record_type sketch) {
                { p_record_type::from_raw_storage(raw_value) } -> std::same_as<p_record_type>;
                { sketch.kmer() } -> std::convertible_to<std::uint64_t>;
                { sketch.last_pos() }
                    -> std::convertible_to<std::uint64_t>;
                { sketch.strand() } -> std::convertible_to<std::uint64_t>;
            })
    {
        if (!flush() || failed_) {
            return false;
        }
        if (count == 0u) {
            return true;
        }
        if constexpr (
            std::same_as<typename p_record_type::storage_type, std::uint64_t>
            && k_minimizer_output_layout<p_record_type>
                != minimizer_output_layout::unsupported) {
            if (sink_.append_raw_u64 != nullptr) {
                if (!sink_.append_raw_u64(
                        sink_.state,
                        k_minimizer_output_layout<p_record_type>,
                        raw,
                        count)) {
                    failed_ = true;
                    return false;
                }
                emitted_ += count;
                return true;
            }
        }
        std::size_t offset = 0u;
        while (offset < count) {
            const std::size_t chunk =
                std::min(k_batch_capacity, count - offset);
            if (!acquire_slots(chunk)) {
                return false;
            }
            for (std::size_t i = 0u; i < chunk; ++i) {
                std::construct_at(
                    pending_ + i,
                    p_record_type::from_raw_storage(raw[offset + i]));
            }
            buffered_ = chunk;
            if (!flush()) {
                return false;
            }
            offset += chunk;
        }
        return true;
    }

    /** @brief Reports whether the sink is usable without allocating or reserving records. */
    [[nodiscard]] bool reserve(std::size_t) const noexcept {
        return !failed_;
    }

    /** @brief Returns the number of committed and buffered records. */
    [[nodiscard]] std::size_t size() const noexcept {
        return emitted_ + buffered_;
    }

    /** @brief Returns the number of committed records, including direct raw appends. */
    [[nodiscard]] std::size_t committed_size() const noexcept {
        return emitted_;
    }

    /** @brief Reports whether the sink and output-buffer checks have succeeded. */
    [[nodiscard]] bool is_ok() const noexcept {
        return !failed_;
    }

    /** @brief Commits the pending batch to the sink. */
    [[nodiscard]] bool flush() {
        if (failed_) {
            return false;
        }
        if (buffered_ == 0u) {
            return true;
        }
        sink_.commit(sink_.state, buffered_);
        emitted_ += buffered_;
        pending_ = nullptr;
        buffered_ = 0u;
        return true;
    }

private:
    /**
     * @brief Requests space for at least minimum records and checks its format and alignment.
     * @return false if the returned buffer is incompatible or too small.
     */
    [[nodiscard]] bool acquire_slots(const std::size_t minimum) {
        const minimizer_sink_batch_view batch =
            sink_.acquire(sink_.state, minimum);
        if (batch.data == nullptr
            || batch.capacity < minimum
            || batch.layout != k_output_layout
            || batch.record_size != sizeof(p_minimizer_type)
            || batch.record_alignment != alignof(p_minimizer_type)
            || reinterpret_cast<std::uintptr_t>(batch.data)
                    % alignof(p_minimizer_type) != 0u) {
            failed_ = true;
            return false;
        }
        pending_ = static_cast<p_minimizer_type*>(batch.data);
        return true;
    }

    /** @brief Copies or moves one record into the current output batch. */
    template<class p_value_type>
    void enqueue(p_value_type&& sketch) {
        if (failed_) {
            return;
        }
        if (buffered_ == k_batch_capacity && !flush()) {
            return;
        }
        if (buffered_ == 0u) {
            if (!acquire_slots(k_batch_capacity)) {
                return;
            }
        }
        // Copy the complete record so fields such as a 128-bit record's span
        // survive unchanged. Rebuilding from hash, position and strand
        // would lose an explicitly supplied span.
        std::construct_at(
            pending_ + buffered_, std::forward<p_value_type>(sketch));
        ++buffered_;
    }

    minimizer_sink sink_{};
    p_minimizer_type* pending_ = nullptr;
    std::size_t buffered_ = 0u;
    std::size_t emitted_ = 0u;
    bool failed_ = false;
};

namespace kernel_detail {

/** @brief Header used to borrow packed memory as a compact sequence range. */
struct borrowed_packed_header {
    const void* data = nullptr;
    std::size_t bp_size = 0u;
    std::uint8_t storage_unit_byte_width = 0u;
    std::uint16_t padding_letters = 0u;
};

/** @brief Associates the borrowed header with an alphabet and storage word type. */
template<sequence_alphabet p_alphabet, class p_storage_unit>
struct borrowed_packed_instance {
    using alphabet_type = p_alphabet;
    using storage_unit = p_storage_unit;
    using compact_sequence_stdc_type = borrowed_packed_header;

    static constexpr std::size_t letter_bit_width =
        seqan3::detail::ceil_log2(seqan3::alphabet_size<p_alphabet>);
    static constexpr std::size_t byte_width = sizeof(p_storage_unit);
    static constexpr bool is_valid_compact_sequence_stdc_instance = true;
};

/**
 * @brief Calls function with a compact range that borrows the packed storage.
 * Checks symbol width, storage size and alignment before the call.
 * The constructed range covers [0, end_symbol). The caller handles begin_symbol.
 * @return false if the packed view is inconsistent.
 */
template<
    sequence_alphabet p_alphabet,
    class p_storage_unit,
    class p_function>
[[nodiscard]] bool with_borrowed_compact_range(
    const packed_sequence_kernel_view& sequence,
    p_function&& function)
{
    using instance = borrowed_packed_instance<p_alphabet, p_storage_unit>;
    using range_type = compact_sequence_range<instance>;
    constexpr std::size_t bits_per_symbol = instance::letter_bit_width;
    constexpr std::size_t letters_per_unit =
        sizeof(p_storage_unit) * 8u / bits_per_symbol;

    if (sequence.bits_per_symbol != bits_per_symbol
        || sequence.storage_unit_bytes != sizeof(p_storage_unit)
        || sequence.end_symbol < sequence.begin_symbol
        || (sequence.end_symbol != 0u && sequence.storage == nullptr)
        || (sequence.storage != nullptr
            && reinterpret_cast<std::uintptr_t>(sequence.storage)
                % alignof(p_storage_unit) != 0u)) {
        return false;
    }

    const std::size_t bp_size = sequence.end_symbol;
    if (bp_size > (std::numeric_limits<std::size_t>::max)()
            - (letters_per_unit - 1u)) {
        return false;
    }
    const std::size_t expected_units =
        (bp_size + letters_per_unit - 1u) / letters_per_unit;
    if (sequence.storage_units != expected_units) {
        return false;
    }
    const std::size_t padding =
        expected_units * letters_per_unit - bp_size;
    if (padding > (std::numeric_limits<std::uint16_t>::max)()) {
        return false;
    }

    borrowed_packed_header header{
        .data = sequence.storage,
        .bp_size = bp_size,
        .storage_unit_byte_width =
            static_cast<std::uint8_t>(sizeof(p_storage_unit)),
        .padding_letters = static_cast<std::uint16_t>(padding),
    };
    range_type range{header};
    std::forward<p_function>(function)(range);
    return true;
}

} // namespace kernel_detail

/** Shared completion status for minimizer kernel calls. */
enum class minimizer_kernel_status : std::uint8_t {
    ok = 0u,
    invalid_sequence_view,
    invalid_configuration,
    invalid_continuation,
    sink_rejected,
};

/** Algorithm identity recorded in minimizer execution statistics. */
enum class minimizer_kernel_algorithm : std::uint8_t {
    none = 0u,
    minimap2_reference,
    two_stack_nthash,
    bounded_nthash,
};

/**
 * Selects the two-stack algorithm after the scheduler has chosen a CPU target.
 * One path carries state across ordered fragments. The other scans a complete
 * resident sequence through vector streams.
 */
enum class two_stack_route : std::uint8_t {
    chronological_continuation = 0u,
    resident_vector_stream = 1u,
};

/** @brief Input view, minimizer settings and output sink for one kernel call. */
struct minimizer_kernel_invocation final {
    packed_sequence_kernel_view sequence{};
    packed_sequence_fragment_source fragments{};
    std::size_t window = 0u;
    std::size_t kmer = 0u;
    std::size_t emitted_position_base = 0u;
    minimizer_output_layout output_layout = minimizer_output_layout::unsupported;
    std::uint64_t output_hash_mask = minimap_sketch_data::k_hash_field_mask;
    std::uint64_t output_position_mask = minimap_sketch_data::k_position_field_mask;
    ::bioseq::minimizers::two_stack_route two_stack_route =
        ::bioseq::minimizers::two_stack_route::chronological_continuation;
    minimizer_sink sink{};
};

/** Work counters for the minimap2 reference SIMD kernel. */
struct reference_minimizer_execution_stats final {
    std::uint64_t kernel_calls = 0u;
    std::uint64_t vector_callback_batches = 0u;
    std::uint64_t vector_lane_visits = 0u;
    std::uint64_t last_storage_unit_bytes = 0u;
    std::uint64_t scalable_storage_fallback_calls = 0u;
    std::uint64_t scalable_storage_fallback_input_symbols = 0u;
    std::uint64_t scalable_storage_fallback_minimizers_emitted = 0u;
};

/** @brief Groups work counters by algorithm and records the selected CPU target. */
struct minimizer_execution_stats final {
    minimizer_kernel_algorithm algorithm = minimizer_kernel_algorithm::none;
    reference_minimizer_execution_stats reference{};
    two_stack_execution_stats two_stack{};
    bounded_nthash_execution_stats bounded_nthash{};
    std::int64_t target = 0;
};

/** Shared result format returned by registered minimizer kernels. */
struct minimizer_kernel_result final {
    minimizer_execution_stats stats{};
    std::size_t emitted = 0u;
    minimizer_kernel_status status = minimizer_kernel_status::ok;
    /** Unchanged numeric value of the selected family's native status enum. */
    std::uint32_t family_status = 0u;

    /** @brief Reports whether the kernel call succeeded. */
    [[nodiscard]] constexpr bool is_ok() const noexcept {
        return status == minimizer_kernel_status::ok;
    }
};

/** Function signature used for every entry in the minimizer dispatch table. */
using minimizer_kernel_function = minimizer_kernel_result (*)(
    minimizer_kernel_invocation&);

/** Minimizer implementations compiled for one Highway CPU target. */
struct minimizer_kernels final {
    minimizer_kernel_function minimap2_reference = nullptr;
    minimizer_kernel_function two_stack_nthash = nullptr;
    minimizer_kernel_function bounded_nthash = nullptr;
};

static_assert(std::is_standard_layout_v<packed_sequence_kernel_view>);
static_assert(std::is_trivially_copyable_v<packed_sequence_kernel_view>);
static_assert(std::is_standard_layout_v<minimizer_sink>);
static_assert(std::is_trivially_copyable_v<minimizer_sink>);
static_assert(std::is_standard_layout_v<packed_sequence_kernel_fragment>);
static_assert(std::is_trivially_copyable_v<packed_sequence_kernel_fragment>);
static_assert(std::is_standard_layout_v<packed_sequence_fragment_source>);
static_assert(std::is_trivially_copyable_v<packed_sequence_fragment_source>);
static_assert(std::is_standard_layout_v<minimizer_kernel_invocation>);
static_assert(std::is_trivially_copyable_v<minimizer_kernel_invocation>);
static_assert(std::is_standard_layout_v<reference_minimizer_execution_stats>);
static_assert(std::is_trivially_copyable_v<reference_minimizer_execution_stats>);
static_assert(std::is_standard_layout_v<minimizer_execution_stats>);
static_assert(std::is_trivially_copyable_v<minimizer_execution_stats>);
static_assert(std::is_standard_layout_v<minimizer_kernel_result>);
static_assert(std::is_trivially_copyable_v<minimizer_kernel_result>);
static_assert(std::is_standard_layout_v<minimizer_kernels>);
static_assert(std::is_trivially_copyable_v<minimizer_kernels>);

} // namespace bioseq::minimizers
