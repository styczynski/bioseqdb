/**
 * @file upload.cppm
 * @brief Packed upload views, canonical windows, and segment geometry.
 */
module;

#include <libassert/assert.hpp>

#include <cstring>
#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.sequences.kmers.vulkan_minimizers:upload;

import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.hashers.base;
import bioseq.common.checked_arithmetic;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.instrumentation;
import :contract;

export namespace bioseq::gpu::vulkan {

#ifdef BIOSEQ_TEST_INDEXES
/** @brief Captures admitted-window ownership counters exposed to unit tests. */
struct vulkan_admitted_window_test_snapshot {
  std::uint64_t admitted_materializations = 0u;
  std::uint64_t live_admitted_windows = 0u;
  std::uint64_t live_admitted_permit_bytes = 0u;
  std::uintptr_t live_admitted_buffer_address = 0u;
  std::uint64_t backend_thread_materializations = 0u;
  std::uint64_t off_backend_materializations = 0u;
};

namespace admitted_window_test_detail {
inline std::atomic<std::uint8_t> enabled{0u};
inline std::atomic<std::uint64_t> admitted_materializations{0u};
inline std::atomic<std::uint64_t> live_admitted_windows{0u};
inline std::atomic<std::uint64_t> live_admitted_permit_bytes{0u};
inline std::atomic<std::uintptr_t> live_admitted_buffer_address{0u};
inline std::atomic<std::uint64_t> backend_thread_materializations{0u};
inline std::atomic<std::uint64_t> off_backend_materializations{0u};
inline std::thread::id backend_thread{};
} // namespace admitted_window_test_detail

/** @brief Resets and enables admitted-window ownership tracking on this thread. */
inline void arm_vulkan_admitted_window_test_seam() noexcept {
  using namespace admitted_window_test_detail;
  enabled.store(0u, std::memory_order_release);
  admitted_materializations.store(0u, std::memory_order_relaxed);
  live_admitted_windows.store(0u, std::memory_order_relaxed);
  live_admitted_permit_bytes.store(0u, std::memory_order_relaxed);
  live_admitted_buffer_address.store(0u, std::memory_order_relaxed);
  backend_thread_materializations.store(0u, std::memory_order_relaxed);
  off_backend_materializations.store(0u, std::memory_order_relaxed);
  backend_thread = std::this_thread::get_id();
  enabled.store(1u, std::memory_order_release);
}

/** @brief Disables admitted-window ownership tracking. */
inline void reset_vulkan_admitted_window_test_seam() noexcept {
  admitted_window_test_detail::enabled.store(0u, std::memory_order_release);
}

/** @brief Returns an atomic snapshot of admitted-window test counters. */
[[nodiscard]] inline vulkan_admitted_window_test_snapshot
vulkan_admitted_window_test_snapshot() noexcept {
  using namespace admitted_window_test_detail;
  return {
      .admitted_materializations =
          admitted_materializations.load(std::memory_order_acquire),
      .live_admitted_windows =
          live_admitted_windows.load(std::memory_order_acquire),
      .live_admitted_permit_bytes =
          live_admitted_permit_bytes.load(std::memory_order_acquire),
      .live_admitted_buffer_address =
          live_admitted_buffer_address.load(std::memory_order_acquire),
      .backend_thread_materializations =
          backend_thread_materializations.load(std::memory_order_acquire),
      .off_backend_materializations =
          off_backend_materializations.load(std::memory_order_acquire),
  };
}

/**
 * @brief Records one admitted materialization while test tracking is enabled.
 * @param permit_bytes Bytes owned by the admission permit.
 * @param buffer_address Address of the admitted storage.
 * @return `true` when a matching end event must be recorded.
 */
[[nodiscard]] inline bool note_vulkan_admitted_window_begin(
    std::uint64_t permit_bytes, std::uintptr_t buffer_address) noexcept {
  using namespace admitted_window_test_detail;
  if (enabled.load(std::memory_order_acquire) == 0u) {
    return false;
  }
  admitted_materializations.fetch_add(1u, std::memory_order_relaxed);
  live_admitted_windows.fetch_add(1u, std::memory_order_release);
  live_admitted_permit_bytes.store(permit_bytes, std::memory_order_release);
  live_admitted_buffer_address.store(buffer_address,
                                     std::memory_order_release);
  if (std::this_thread::get_id() == backend_thread) {
    backend_thread_materializations.fetch_add(1u, std::memory_order_relaxed);
  } else {
    off_backend_materializations.fetch_add(1u, std::memory_order_relaxed);
  }
  return true;
}

/**
 * @brief Records destruction of a tracked admitted materialization.
 * @param tracked Whether the matching begin event was recorded.
 */
inline void note_vulkan_admitted_window_end(bool tracked) noexcept {
  if (tracked) {
    using namespace admitted_window_test_detail;
    live_admitted_buffer_address.store(0u, std::memory_order_release);
    live_admitted_permit_bytes.store(0u, std::memory_order_release);
    live_admitted_windows.fetch_sub(
        1u, std::memory_order_release);
  }
}
#endif

namespace admitted_sequence_detail {

/** @brief Describes borrowed packed storage for a compact range adapter. */
struct borrowed_packed_header {
  const void *data = nullptr;
  bioseq::storage::sequence_size_t bp_size = 0;
  std::uint8_t storage_unit_byte_width = 0;
  std::uint16_t padding_letters = 0;
};

/**
 * @brief Brands a borrowed descriptor with canonical 32-bit packed storage.
 * @tparam p_alphabet Logical sequence alphabet.
 */
template <sequence_alphabet p_alphabet> struct borrowed_u32_instance {
  using alphabet_type = p_alphabet;
  using storage_unit = std::uint32_t;
  using compact_sequence_stdc_type = borrowed_packed_header;

  static constexpr std::size_t letter_bit_width =
      bioseq::storage::lazy_sequence_view<
          p_alphabet>::traits::k_logical_bits_per_symbol;
  static constexpr std::size_t byte_width = sizeof(storage_unit);
  static constexpr bool is_valid_compact_sequence_stdc_instance = true;
};

/**
 * @brief Visits a descriptor backed by a real 32-bit object array.
 * @tparam p_alphabet Logical sequence alphabet.
 * @tparam p_function Callback receiving the compact range.
 * @param descriptor Resident descriptor with canonical 32-bit storage.
 * @param fn Callback invoked synchronously.
 * @return Callback result.
 */
template <sequence_alphabet p_alphabet, class p_function>
decltype(auto) with_typed_u32_range(
    const typename bioseq::storage::lazy_sequence_view<
        p_alphabet>::resident_packed_sequence &descriptor,
    p_function &&fn) {
  using instance = borrowed_u32_instance<p_alphabet>;
  using range_type = compact_sequence_range<instance>;
  borrowed_packed_header header{
      .data = descriptor.units,
      .bp_size = descriptor.logical_length,
      .storage_unit_byte_width = descriptor.storage_unit_width,
      .padding_letters = descriptor.trailing_padding,
  };
    range_type range{header};
  return std::forward<p_function>(fn)(range);
}

/**
 * @brief Steps through byte-addressed packed storage in logical unit strides.
 * @tparam p_unit_bytes Number of object-representation bytes per logical unit.
 */
template <std::size_t p_unit_bytes>
struct byte_packed_unit_iterator {
  const std::byte *address = nullptr;

  /** @brief Advances by a number of logical packed units. */
  byte_packed_unit_iterator &operator+=(std::size_t units) noexcept {
    if (units != 0u) {
      DEBUG_ASSERT(address != nullptr,
                   "nonempty byte upload iterator requires backing bytes");
      address += units * p_unit_bytes;
    }
    return *this;
  }

  /** @brief Returns an iterator advanced by logical packed units. */
  friend byte_packed_unit_iterator operator+(
      byte_packed_unit_iterator it, std::size_t units) noexcept {
    it += units;
    return it;
  }

  /** @brief Tests whether two iterators address the same byte. */
  friend bool operator==(const byte_packed_unit_iterator &,
                         const byte_packed_unit_iterator &) = default;
};

/**
 * @brief Exposes upload geometry for layout-compatible byte storage.
 *
 * The range has no dereference operation, so a differently typed backing
 * allocation cannot be read as 32-bit objects on the host.
 *
 * @tparam p_alphabet Logical sequence alphabet.
 */
template <sequence_alphabet p_alphabet>
class byte_upload_range {
public:
  using instance = borrowed_u32_instance<p_alphabet>;
  using traits = typename compact_sequence_range<instance>::traits;
  using iterator = byte_packed_unit_iterator<sizeof(std::uint32_t)>;
  static constexpr bool is_byte_addressed_upload_only = true;

private:
  using descriptor_type = typename bioseq::storage::lazy_sequence_view<
      p_alphabet>::resident_packed_sequence;
  descriptor_type descriptor_{};

public:
  /** @brief Binds a resident descriptor with canonical 32-bit byte geometry. */
  explicit byte_upload_range(const descriptor_type &descriptor) noexcept
      : descriptor_(descriptor) {
    DEBUG_ASSERT(
        descriptor.storage_unit_width == sizeof(std::uint32_t),
        "byte-addressed Vulkan range requires canonical u32 geometry");
  }

  /** @brief Returns the number of logical packed units. */
  [[nodiscard]] std::size_t size() const noexcept {
    return descriptor_.unit_count;
  }
  /** @brief Returns the packed byte count. */
  [[nodiscard]] std::size_t byte_size() const noexcept {
    return descriptor_.unit_count * sizeof(std::uint32_t);
  }
  /** @brief Returns the biological symbol count. */
  [[nodiscard]] std::size_t bp_size() const noexcept {
    return static_cast<std::size_t>(descriptor_.logical_length);
  }
  /** @brief Returns trailing padding symbols in the final packed unit. */
  [[nodiscard]] std::size_t padding() const noexcept {
    return descriptor_.trailing_padding;
  }
  /** @brief Returns an iterator to the first logical packed unit. */
  [[nodiscard]] iterator cbegin() const noexcept {
    return iterator{static_cast<const std::byte *>(descriptor_.units)};
  }
  /** @brief Returns an iterator past the final logical packed unit. */
  [[nodiscard]] iterator cend() const noexcept {
    return cbegin() + size();
  }
  // BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN byte_upload_range_address_adapter
  /** @brief Returns the object-representation address at an upload iterator. */
  [[nodiscard]] const std::byte *read_cptr(const iterator &it) const noexcept {
    return it.address;
  }
  // BIOSEQ_PRIVATE_PACKED_ACCESS_END byte_upload_range_address_adapter
};

/**
 * @brief Owns or borrows one canonical 32-bit upload window.
 * @tparam p_alphabet Logical sequence alphabet.
 */
template <sequence_alphabet p_alphabet> struct canonical_u32_window {
  using owner_type = bioseq::storage::admitted_packed_sequence<p_alphabet>;
  using descriptor_type = typename bioseq::storage::lazy_sequence_view<
      p_alphabet>::resident_packed_sequence;

  std::optional<owner_type> owner;
  descriptor_type descriptor{};
  bioseq::storage::sequence_resident_borrow_kind borrow_kind{
      bioseq::storage::sequence_resident_borrow_kind::none};
#ifdef BIOSEQ_TEST_INDEXES
  bool test_admitted_window_tracked = false;
#endif

  /** @brief Takes ownership of an admitted 32-bit materialization. */
  explicit canonical_u32_window(owner_type &&admitted)
      : owner(std::in_place, std::move(admitted)),
        descriptor(owner->descriptor())
#ifdef BIOSEQ_TEST_INDEXES
        ,
        test_admitted_window_tracked(note_vulkan_admitted_window_begin(
            owner->test_admission_bytes(), owner->test_storage_address()))
#endif
  {
  }

  /** @brief Borrows stable resident storage with its validated borrow kind. */
  explicit canonical_u32_window(
      const descriptor_type &borrowed,
      bioseq::storage::sequence_resident_borrow_kind kind) noexcept
      : descriptor(borrowed), borrow_kind(kind) {
    DEBUG_ASSERT(
        kind != bioseq::storage::sequence_resident_borrow_kind::none,
        "borrowed Vulkan canonical window requires a truthful borrow kind");
  }

  /** @brief Transfers ownership and any active test-tracking event. */
  canonical_u32_window(canonical_u32_window &&other) noexcept
      : owner(std::move(other.owner)), descriptor(other.descriptor),
        borrow_kind(other.borrow_kind)
#ifdef BIOSEQ_TEST_INDEXES
        ,
        test_admitted_window_tracked(
            std::exchange(other.test_admitted_window_tracked, false))
#endif
  {
  }
  /** @brief Ends test tracking before releasing admitted storage. */
  ~canonical_u32_window() {
#ifdef BIOSEQ_TEST_INDEXES
    note_vulkan_admitted_window_end(test_admitted_window_tracked);
#endif
  }
  canonical_u32_window &operator=(canonical_u32_window &&) = delete;
  canonical_u32_window(const canonical_u32_window &) = delete;
  canonical_u32_window &operator=(const canonical_u32_window &) = delete;
};

/**
 * @brief Visits canonical input through a type-correct upload surface.
 * @tparam p_alphabet Logical sequence alphabet.
 * @tparam p_function Callback receiving a typed or byte-addressed range.
 * @param window Owned or borrowed canonical upload window.
 * @param fn Callback invoked synchronously.
 * @return Callback result.
 */
template <sequence_alphabet p_alphabet, class p_function>
decltype(auto) with_canonical_u32_upload_source(
    const canonical_u32_window<p_alphabet> &window, p_function &&fn) {
  if (window.borrow_kind
      == bioseq::storage::sequence_resident_borrow_kind::
          reinterpreted_bytes) {
    byte_upload_range<p_alphabet> bytes{window.descriptor};
    return std::forward<p_function>(fn)(bytes);
  }
  DEBUG_ASSERT(
      window.owner.has_value()
          || window.borrow_kind
              == bioseq::storage::sequence_resident_borrow_kind::
                  native_typed_storage,
      "typed Vulkan range requires native or admitted u32 storage");
  return with_typed_u32_range<p_alphabet>(
      window.descriptor, std::forward<p_function>(fn));
}

/**
 * @brief Borrows or materializes one canonical 32-bit upload window.
 * @tparam p_alphabet Logical sequence alphabet.
 * @param sequence Lazy sequence source.
 * @param from Inclusive biological start position.
 * @param to Exclusive biological end position.
 * @param decline_out Optional rejection record.
 * @return Prepared window, or no value when admission or geometry fails.
 */
template <sequence_alphabet p_alphabet>
[[nodiscard]] std::optional<canonical_u32_window<p_alphabet>>
prepare_canonical_u32_window(
    bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
    bioseq::storage::sequence_size_t from, bioseq::storage::sequence_size_t to,
    vulkan_sketch_decline *decline_out) {
  if (from > to || to > sequence.bp_size()) {
    if (decline_out != nullptr) {
      decline_out->reason =
          vulkan_sketch_decline_reason::sequence_type_unsupported;
    }
    return std::nullopt;
  }
  const auto access = bioseq::storage::plan_sequence_access(
      sequence,
      bioseq::storage::sequence_access_requirement::contiguous_resident, from,
      to, sizeof(std::uint32_t),
      bioseq::storage::sequence_resident_borrow_policy::
          byte_upload_compatible);
  if (access.route
          == bioseq::storage::sequence_access_route::borrow_resident
      && access.resident.has_value()) {
    // All facade Vulkan entry points below use the range only to derive byte
    // addresses and upload its object representation; they never load a
    // requested-width storage unit on the CPU.  They also complete upload,
    // submission, fence drain, and result delivery before returning.  A
    // validated stable native descriptor or cross-width byte descriptor can
    // therefore be borrowed for that synchronous interval without an
    // admitted O(n) repack/copy.
    return std::optional<canonical_u32_window<p_alphabet>>{
        std::in_place, *access.resident, access.resident_borrow_kind};
  }
  bioseq::parallelism::memory_admission_failure failure{};
  auto permit =
      bioseq::parallelism::memory_admission_controller::instance().try_reserve(
          access.admission_bytes, &failure);
  if (!permit) {
    if (decline_out != nullptr) {
      decline_out->reason = vulkan_sketch_decline_reason::memory_admission_denied;
      decline_out->admission = failure;
    }
    return std::nullopt;
  }
  auto owner = bioseq::storage::materialize_packed_sequence(
      sequence, from, to, sizeof(std::uint32_t), std::move(*permit));
  return std::optional<canonical_u32_window<p_alphabet>>{std::in_place,
                                                       std::move(owner)};
}

/**
 * @brief Validates the alphabet, k-mer, and window shape for facade dispatch.
 * @tparam p_alphabet Logical sequence alphabet.
 * @param sequence Lazy sequence source.
 * @param window_size Window length in k-mers.
 * @param kmer_size K-mer length in biological symbols.
 * @param decline_out Optional rejection record.
 * @return `true` when the shape is supported.
 */
template <sequence_alphabet p_alphabet>
[[nodiscard]] bool validate_facade_sketch_shape(
        bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
        std::size_t window_size,
        std::size_t kmer_size,
        vulkan_sketch_decline* decline_out) noexcept {
    using instance = borrowed_u32_instance<p_alphabet>;
    using traits = typename compact_sequence_range<instance>::traits;
    if (decline_out != nullptr) {
        *decline_out = vulkan_sketch_decline{
            .bp_size = sequence.bp_size(),
            .window_size = window_size,
            .kmer_size = kmer_size,
        };
    }
    auto reject = [decline_out](vulkan_sketch_decline_reason reason) noexcept {
        if (decline_out != nullptr) {
            decline_out->reason = reason;
        }
        return false;
    };
    if constexpr (!vulkan_minimizer_alphabet_shape_supported<traits>()) {
        return reject(vulkan_sketch_decline_reason::alphabet_unsupported);
    }
    if (!vulkan_minimizer_kmer_size_supported<traits>(kmer_size)) {
        return reject(vulkan_sketch_decline_reason::kmer_size_unsupported);
    }
    if (!vulkan_minimizer_window_size_supported(window_size)) {
        return reject(vulkan_sketch_decline_reason::window_size_unsupported);
    }
    return true;
}

} // namespace admitted_sequence_detail

/**
 * @brief Requires the contiguous packed surface used by one Vulkan upload.
 * @tparam p_sequence Candidate packed sequence type.
 */
// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN vulkan_sketchable_sequence
template <class p_sequence>
concept vulkan_sketchable_sequence = requires (const p_sequence& seq) {
    typename std::remove_cvref_t<p_sequence>::traits;
    typename std::remove_cvref_t<p_sequence>::traits::storage_unit;
    typename std::remove_cvref_t<p_sequence>::traits::alphabet_type;
    { seq.cbegin() };
    { seq.byte_size() } -> std::convertible_to<std::uint64_t>;
    { seq.bp_size() } -> std::convertible_to<std::uint64_t>;
    { seq.padding() };
    { seq.read_cptr(seq.cbegin()) };
};
// BIOSEQ_PRIVATE_PACKED_ACCESS_END vulkan_sketchable_sequence

/** @brief Describes packed sequence bytes and shader decoding geometry. */
struct sequence_upload_view {
    const void* bytes = nullptr;
    std::uint64_t byte_size = 0;
    std::uint64_t padded_byte_size = 0;
    std::uint64_t bp_size = 0;
    std::uint32_t unit_bits = 0;
    std::uint32_t bits_per_char = 0;
    std::uint32_t letters_per_unit = 0;
    std::uint32_t alphabet_size = 0;
    std::uint32_t char_mask = 0;
    std::uint32_t padding_letters = 0;
    alphabet_kernel_kind alphabet_kind = alphabet_kernel_kind::generic;
};

/**
 * @brief Aligns an upload byte count to the shader's 32-bit word boundary.
 * @param byte_count Unpadded byte count.
 * @return Aligned count, or `UINT64_MAX` on overflow.
 */
[[nodiscard]] inline std::uint64_t aligned_upload_bytes(
    std::uint64_t byte_count) noexcept {
    return bioseq::common::checked_align_up(byte_count, std::uint64_t{4u})
        .value_or((std::numeric_limits<std::uint64_t>::max)());
}

/** @brief Describes a packed unit interval and its exact biological length. */
struct segment_upload_descriptor {
    std::size_t lo_unit = 0;
    std::size_t hi_unit = 0;
    std::uint64_t window_bp = 0;
    std::uint32_t tail_padding = 0;
};

/** @brief Describes one collected upload window and its emitted core interval. */
struct gpu_sketch_segment_window {
    std::size_t sub_lo_unit = 0;
    std::size_t sub_hi_unit = 0;
    std::size_t core_lo_unit = 0;
    std::size_t core_hi_unit = 0;
    std::uint64_t emit_base_bp = 0;
    std::uint64_t collected_bp = 0;
};

/**
 * @brief Computes biological context retained around a segmented core.
 * @tparam p_traits Packed sequence traits.
 * @param window_size Window length in k-mers.
 * @param kmer_size K-mer length in biological symbols.
 * @return Required context in biological symbols.
 */
template <class p_traits>
[[nodiscard]] constexpr std::uint64_t sketch_segment_context_bp(
    std::size_t window_size,
    std::size_t kmer_size) noexcept
{
    return static_cast<std::uint64_t>(window_size)
        + static_cast<std::uint64_t>(kmer_size)
        + 1u;
}

// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN sketch_segment_context_units
/**
 * @brief Converts required segment context to packed storage units.
 * @tparam p_traits Packed sequence traits.
 * @param window_size Window length in k-mers.
 * @param kmer_size K-mer length in biological symbols.
 * @return At least one packed storage unit.
 */
template <class p_traits>
[[nodiscard]] constexpr std::size_t sketch_segment_context_units(
    std::size_t window_size,
    std::size_t kmer_size) noexcept
{
    const std::uint64_t context_bp =
        sketch_segment_context_bp<p_traits>(window_size, kmer_size);
    const std::uint64_t lpu =
        std::max<std::uint64_t>(1u, p_traits::letters_per_unit);
    return std::max<std::size_t>(
        1u,
        static_cast<std::size_t>(bioseq::common::ceil_div(context_bp, lpu)));
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END sketch_segment_context_units

// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN compute_segment_descriptor
/**
 * @brief Clamps a packed unit interval and computes its biological geometry.
 * @tparam p_sequence Packed sequence type.
 * @param seq Source sequence.
 * @param lo_unit Inclusive packed unit index.
 * @param hi_unit Exclusive packed unit index.
 * @return Validated upload descriptor.
 */
template <class p_sequence>
[[nodiscard]] segment_upload_descriptor compute_segment_descriptor(
    const p_sequence& seq,
    std::size_t lo_unit,
    std::size_t hi_unit) noexcept
{
    using traits = typename std::remove_cvref_t<p_sequence>::traits;
    constexpr std::size_t letters_per_unit = traits::letters_per_unit;
    const std::size_t size_units = static_cast<std::size_t>(seq.size());
    const std::size_t clamped_lo = std::min(lo_unit, size_units);
    const std::size_t clamped_hi = std::min(std::max(hi_unit, clamped_lo), size_units);
    const std::uint32_t tail_padding =
        clamped_hi == size_units
            ? static_cast<std::uint32_t>(seq.padding())
            : 0u;
    const std::uint64_t raw_bp =
        static_cast<std::uint64_t>(clamped_hi - clamped_lo)
        * static_cast<std::uint64_t>(letters_per_unit);
    const std::uint64_t window_bp =
        raw_bp >= tail_padding ? raw_bp - tail_padding : 0u;
    return segment_upload_descriptor{
        .lo_unit = clamped_lo,
        .hi_unit = clamped_hi,
        .window_bp = window_bp,
        .tail_padding = tail_padding,
    };
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END compute_segment_descriptor

// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN make_sketch_segment_window
/**
 * @brief Expands a core unit interval with bounded minimizer context.
 * @tparam p_sequence Packed sequence type.
 * @param seq Source sequence.
 * @param descriptor Bounds of the complete source interval.
 * @param core_lo_unit Inclusive core unit index.
 * @param core_hi_unit Exclusive core unit index.
 * @param context_units Context retained on each available side.
 * @return Upload and emission geometry for the segment.
 */
template <class p_sequence>
[[nodiscard]] gpu_sketch_segment_window make_sketch_segment_window(
    const p_sequence& seq,
    const segment_upload_descriptor& descriptor,
    std::size_t core_lo_unit,
    std::size_t core_hi_unit,
    std::size_t context_units)
{
    using traits = typename std::remove_cvref_t<p_sequence>::traits;
    const std::size_t lpu = std::max<std::size_t>(1u, traits::letters_per_unit);
    const std::size_t core_lo =
        std::min(std::max(core_lo_unit, descriptor.lo_unit), descriptor.hi_unit);
    const std::size_t core_hi =
        std::min(std::max(core_hi_unit, core_lo), descriptor.hi_unit);
    const std::size_t sub_lo =
        core_lo > descriptor.lo_unit + context_units
            ? core_lo - context_units
            : descriptor.lo_unit;
    const std::size_t sub_hi =
        std::min(core_hi + context_units, descriptor.hi_unit);
    const segment_upload_descriptor sub_descriptor =
        compute_segment_descriptor(seq, sub_lo, sub_hi);
    return gpu_sketch_segment_window{
        .sub_lo_unit = sub_lo,
        .sub_hi_unit = sub_hi,
        .core_lo_unit = core_lo,
        .core_hi_unit = core_hi,
        .emit_base_bp = static_cast<std::uint64_t>(sub_lo) * lpu,
        .collected_bp = sub_descriptor.window_bp,
    };
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END make_sketch_segment_window

/**
 * @brief Bisects a segment core while restoring context on both halves.
 * @tparam p_sequence Packed sequence type.
 * @param seq Source sequence.
 * @param descriptor Bounds of the complete source interval.
 * @param window Segment to split.
 * @param context_units Context retained around each new core.
 * @param left Receives the lower core half.
 * @param right Receives the upper core half.
 * @return `false` when the core cannot be divided further.
 */
template <class p_sequence>
[[nodiscard]] bool split_sketch_segment_window(
    const p_sequence& seq,
    const segment_upload_descriptor& descriptor,
    const gpu_sketch_segment_window& window,
    std::size_t context_units,
    gpu_sketch_segment_window& left,
    gpu_sketch_segment_window& right)
{
    if (window.core_hi_unit <= window.core_lo_unit + 1u) {
        return false;
    }
    const std::size_t core_mid =
        window.core_lo_unit + (window.core_hi_unit - window.core_lo_unit) / 2u;
    if (core_mid == window.core_lo_unit || core_mid == window.core_hi_unit) {
        return false;
    }
    left = make_sketch_segment_window(
        seq,
        descriptor,
        window.core_lo_unit,
        core_mid,
        context_units);
    right = make_sketch_segment_window(
        seq,
        descriptor,
        core_mid,
        window.core_hi_unit,
        context_units);
    return left.core_lo_unit < left.core_hi_unit
        && right.core_lo_unit < right.core_hi_unit;
}

// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN admitted_sketch_collection_limit_bp
/**
 * @brief Selects a collection limit that fits current memory admission.
 * @tparam p_minimizer_type Packed output record.
 * @tparam p_traits Packed sequence traits.
 * @param window_size Window length in k-mers.
 * @param kmer_size K-mer length in biological symbols.
 * @param use Requested GPU sketch output purpose.
 * @return Biological-symbol limit for one dispatch.
 */
template <class p_minimizer_type, class p_traits>
[[nodiscard]] std::uint64_t admitted_sketch_collection_limit_bp(
    std::size_t window_size,
    std::size_t kmer_size,
    gpu_sketch_purpose use) noexcept
{
    using storage_unit = typename p_traits::storage_unit;
    constexpr std::uint64_t collected_floor_bp = 1'000'000u;
    // Bound one dispatch while retaining room for large target segments under
    // ordinary memory admission.
    constexpr std::uint64_t collected_ceil_bp = 96'000'000u;
    const std::uint64_t available =
        bioseq::parallelism::memory_admission_controller::instance()
            .allocatable_after_reservations();
    if (available == (std::numeric_limits<std::uint64_t>::max)()) {
        return collected_ceil_bp;
    }

    const std::uint64_t lpu =
        std::max<std::uint64_t>(1u, p_traits::letters_per_unit);
    std::uint64_t candidate = collected_ceil_bp;
    while (candidate > collected_floor_bp) {
        const std::uint64_t capacity = std::max<std::uint64_t>(64u, candidate);
        const std::uint64_t unit_count =
            bioseq::common::ceil_div(candidate, lpu);
        const std::uint64_t sequence_bytes = aligned_upload_bytes(
            bioseq::common::saturating_mul(
                unit_count,
                static_cast<std::uint64_t>(sizeof(storage_unit))));
        const std::uint64_t output_bytes =
            use == gpu_sketch_purpose::minimizer_words
                ? minimizer_wire_traits<p_minimizer_type>::device_bytes_for_count(capacity)
                : 0u;
        const gpu_sketch_memory_plan plan = make_gpu_sketch_memory_plan(
            candidate,
            sequence_bytes,
            sizeof(vulkan_rank_lut),
            capacity,
            output_bytes,
            use,
            static_cast<std::uint32_t>(kmer_size),
            static_cast<std::uint32_t>(window_size),
            static_cast<std::uint32_t>(lpu));
        if (plan.is_valid && plan.admission_bytes <= available / 2u) {
            return candidate;
        }
        candidate /= 2u;
    }
    return collected_floor_bp;
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END admitted_sketch_collection_limit_bp

// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN sketch_segment_windows
/**
 * @brief Tiles a source descriptor into context-preserving GPU windows.
 * @tparam p_minimizer_type Packed output record.
 * @tparam p_sequence Packed sequence type.
 * @param seq Source sequence.
 * @param descriptor Bounds of the complete source interval.
 * @param window_size Window length in k-mers.
 * @param kmer_size K-mer length in biological symbols.
 * @param use Requested GPU sketch output purpose.
 * @param collected_limit_override Optional fixed biological-symbol limit.
 * @return Ordered upload windows covering every core unit once.
 */
template <class p_minimizer_type, class p_sequence>
[[nodiscard]] std::vector<gpu_sketch_segment_window> sketch_segment_windows(
    const p_sequence& seq,
    const segment_upload_descriptor& descriptor,
    std::size_t window_size,
    std::size_t kmer_size,
    gpu_sketch_purpose use,
    std::uint64_t collected_limit_override = 0u)
{
    using traits = typename std::remove_cvref_t<p_sequence>::traits;
    const std::uint64_t collected_limit =
        collected_limit_override == 0u
            ? admitted_sketch_collection_limit_bp<p_minimizer_type, traits>(
                window_size, kmer_size, use)
            : collected_limit_override;
    const std::size_t lpu = std::max<std::size_t>(1u, traits::letters_per_unit);
    const std::uint64_t context_bp =
        sketch_segment_context_bp<traits>(window_size, kmer_size);
    constexpr std::uint64_t tile_safety_bp = 4096u;
    const std::uint64_t core_bp =
        collected_limit > context_bp * 2u + tile_safety_bp + 1u
            ? collected_limit - context_bp * 2u - tile_safety_bp
            : std::max<std::uint64_t>(1u, collected_limit / 2u);
    const std::size_t core_units =
        std::max<std::size_t>(1u, static_cast<std::size_t>(core_bp / lpu));
    const std::size_t context_units =
        sketch_segment_context_units<traits>(window_size, kmer_size);

    std::vector<gpu_sketch_segment_window> windows;
    if (descriptor.window_bp <= collected_limit) {
        windows.push_back(gpu_sketch_segment_window{
            .sub_lo_unit = descriptor.lo_unit,
            .sub_hi_unit = descriptor.hi_unit,
            .core_lo_unit = descriptor.lo_unit,
            .core_hi_unit = descriptor.hi_unit,
            .emit_base_bp = static_cast<std::uint64_t>(descriptor.lo_unit) * lpu,
            .collected_bp = descriptor.window_bp,
        });
        return windows;
    }

    for (std::size_t core_lo = descriptor.lo_unit;
         core_lo < descriptor.hi_unit;
         core_lo += core_units) {
        const std::size_t core_hi =
            std::min(core_lo + core_units, descriptor.hi_unit);
        windows.push_back(make_sketch_segment_window(
            seq,
            descriptor,
            core_lo,
            core_hi,
            context_units));
        if (core_hi == descriptor.hi_unit) {
            break;
        }
    }
    return windows;
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END sketch_segment_windows

// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN make_upload_view_from_window
/**
 * @brief Creates shader upload geometry for one packed window.
 * @tparam p_sequence Packed sequence type.
 * @tparam p_addressed_element Native storage or byte address element type.
 * @param base Address of the window's first packed unit.
 * @param descriptor Exact unit and biological interval.
 * @return Non-owning upload view.
 */
template <class p_sequence, class p_addressed_element>
[[nodiscard]] inline sequence_upload_view make_upload_view_from_window(
    const p_sequence&,
    const p_addressed_element* base,
    const segment_upload_descriptor& descriptor)
{
    using traits = typename std::remove_cvref_t<p_sequence>::traits;
    using logical_storage_unit = typename traits::storage_unit;
    static_assert(
        std::same_as<std::remove_cv_t<p_addressed_element>, logical_storage_unit>
        || std::same_as<std::remove_cv_t<p_addressed_element>, std::byte>
        || std::same_as<std::remove_cv_t<p_addressed_element>, unsigned char>,
        "Vulkan uploads accept native storage pointers or byte addresses");
    const std::uint64_t byte_size =
        static_cast<std::uint64_t>(descriptor.hi_unit - descriptor.lo_unit)
        * static_cast<std::uint64_t>(sizeof(logical_storage_unit));
    return sequence_upload_view{
        .bytes = byte_size == 0u ? nullptr : static_cast<const void*>(base),
        .byte_size = byte_size,
        .padded_byte_size = aligned_upload_bytes(byte_size),
        .bp_size = descriptor.window_bp,
        .unit_bits = static_cast<std::uint32_t>(
            sizeof(logical_storage_unit) * 8u),
        .bits_per_char = static_cast<std::uint32_t>(traits::bits_per_char),
        .letters_per_unit = static_cast<std::uint32_t>(traits::letters_per_unit),
        .alphabet_size = static_cast<std::uint32_t>(traits::alphabet_size),
        .char_mask = static_cast<std::uint32_t>(traits::char_mask),
        .padding_letters = descriptor.tail_padding,
        .alphabet_kind = alphabet_kernel_kind_of_traits<traits>(),
    };
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END make_upload_view_from_window

// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN make_sequence_upload_view
/**
 * @brief Creates a non-owning upload view for a contiguous packed sequence.
 * @tparam p_sequence Sequence satisfying `vulkan_sketchable_sequence`.
 * @param seq Source sequence.
 * @return Upload bytes and shader decoding geometry.
 */
template <vulkan_sketchable_sequence p_sequence>
[[nodiscard]] inline sequence_upload_view make_sequence_upload_view(const p_sequence& seq) {
    auto begin = seq.cbegin();
    const auto* ptr =
        seq.byte_size() == 0 ? nullptr : seq.read_cptr(begin);
    const segment_upload_descriptor descriptor{
        .lo_unit = 0,
        .hi_unit = static_cast<std::size_t>(seq.size()),
        .window_bp = static_cast<std::uint64_t>(seq.bp_size()),
        .tail_padding = static_cast<std::uint32_t>(seq.padding()),
    };
    return make_upload_view_from_window<p_sequence>(seq, ptr, descriptor);
}

/**
 * @brief Creates an upload view for an admitted packed sequence owner.
 *
 * The owner must outlive upload completion because the view does not copy or
 * acquire the backing storage.
 *
 * @tparam p_alphabet Logical sequence alphabet.
 * @param owner Admitted packed sequence owner.
 * @return Validated upload bytes and shader decoding geometry.
 * @throws vulkan_runtime_error If the resident descriptor is inconsistent.
 */
template <sequence_alphabet p_alphabet>
[[nodiscard]] inline sequence_upload_view make_sequence_upload_view(
    const bioseq::storage::admitted_packed_sequence<p_alphabet>& owner) {
    using lazy_traits = bioseq::storage::lazy_sequence_traits<p_alphabet>;
    const auto& descriptor = owner.descriptor();
    constexpr std::uint32_t bits = static_cast<std::uint32_t>(
        lazy_traits::k_logical_bits_per_symbol);
    const std::uint32_t letters_per_unit = static_cast<std::uint32_t>(
        bioseq::storage::packed_letters_per_unit(
            descriptor.storage_unit_width,
            static_cast<std::uint8_t>(bits)));
    const std::uint64_t expected_units = letters_per_unit == 0u
        ? (std::numeric_limits<std::uint64_t>::max)()
        : descriptor.logical_length / letters_per_unit
            + static_cast<std::uint64_t>(
                descriptor.logical_length % letters_per_unit != 0u);
    const bool geometry_overflow = letters_per_unit == 0u
        || expected_units > (std::numeric_limits<std::uint64_t>::max)()
            / std::max<std::uint32_t>(letters_per_unit, 1u);
    const std::uint64_t expected_padding = geometry_overflow
        ? (std::numeric_limits<std::uint64_t>::max)()
        : expected_units * letters_per_unit - descriptor.logical_length;
    const bool bytes_overflow = descriptor.unit_count
        > (std::numeric_limits<std::uint64_t>::max)()
            / std::max<std::uint8_t>(
                descriptor.storage_unit_width, 1u);
    if (descriptor.bit_order
            != bioseq::storage::lazy_sequence_view<p_alphabet>::
                packed_symbol_order::least_significant_symbol_first
        || descriptor.leading_symbol_offset != 0u
        || descriptor.bits_per_symbol != bits
        || letters_per_unit == 0u
        || geometry_overflow
        || descriptor.unit_count != expected_units
        || descriptor.trailing_padding != expected_padding
        || bytes_overflow
        || (descriptor.unit_count != 0u && descriptor.units == nullptr)) {
        throw vulkan_runtime_error(
            "admitted packed-sequence upload descriptor is invalid");
    }
    const std::uint64_t bytes = static_cast<std::uint64_t>(
        descriptor.unit_count) * descriptor.storage_unit_width;
    constexpr alphabet_kernel_kind kernel = [] {
        if constexpr (seqan3::nucleotide_alphabet<p_alphabet>
            && bits == 2u && seqan3::alphabet_size<p_alphabet> == 4u) {
            return alphabet_kernel_kind::dna4_nt4;
        } else if constexpr (seqan3::nucleotide_alphabet<p_alphabet>
            && bits == 3u && seqan3::alphabet_size<p_alphabet> == 5u) {
            return alphabet_kernel_kind::dna5_nt4;
        } else {
            return alphabet_kernel_kind::generic;
        }
    }();
    return sequence_upload_view{
        .bytes = bytes == 0u ? nullptr : descriptor.units,
        .byte_size = bytes,
        .padded_byte_size = aligned_upload_bytes(bytes),
        .bp_size = descriptor.logical_length,
        .unit_bits = static_cast<std::uint32_t>(
            descriptor.storage_unit_width * 8u),
        .bits_per_char = bits,
        .letters_per_unit = letters_per_unit,
        .alphabet_size = static_cast<std::uint32_t>(
            seqan3::alphabet_size<p_alphabet>),
        .char_mask = bits == 32u
            ? std::numeric_limits<std::uint32_t>::max()
            : static_cast<std::uint32_t>((std::uint64_t{1u} << bits) - 1u),
        .padding_letters = descriptor.trailing_padding,
        .alphabet_kind = kernel,
    };
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END make_sequence_upload_view

} // namespace bioseq::gpu::vulkan
