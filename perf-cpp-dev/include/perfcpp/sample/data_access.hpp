#pragma once

#include <cstdint>
#include <optional>

namespace perf {

/**
 * Captures sampled characteristics of a data memory access.
 */
class DataAccess
{
public:
  enum class AccessType : std::uint8_t
  {
    Load,
    Store,
    SoftwarePrefetch
  };

  /**
   * Encodes where in the memory/cache hierarchy a data access was resolved.
   * This includes hits at various cache levels, memory, and properties of remote accesses.
   */
  class Source
  {
  public:
    /**
     * Set whether the access hit in L1 cache.
     * @param is_l1_hit True if access hit in L1 cache.
     */
    void l1_hit(const bool is_l1_hit) noexcept { _is_l1_hit = is_l1_hit; }

    /**
     * Set whether the access hit in the Memory Hierarchy Buffer (MHB).
     * @param is_mhb_hit True if MHB was hit.
     */
    void mhb_hit(const bool is_mhb_hit) noexcept { _is_mhb_hit = is_mhb_hit; }

    /**
     * Set the number of MHB slots allocated.
     * @param num_mhb_slots_allocated Number of slots used by MHB.
     */
    void num_mhb_slots_allocated(const std::uint8_t num_mhb_slots_allocated) noexcept
    {
      _num_mhb_slots_allocated = num_mhb_slots_allocated;
    }

    /**
     * Set whether the access hit in L2 cache.
     * @param is_l2_hit True if access hit in L2 cache.
     */
    void l2_hit(const bool is_l2_hit) noexcept { _is_l2_hit = is_l2_hit; }

    /**
     * Set whether the access hit in L3 cache.
     * @param is_l3_hit True if access hit in L3 cache.
     */
    void l3_hit(const bool is_l3_hit) noexcept { _is_l3_hit = is_l3_hit; }

    /**
     * Set whether the access hit in L4 cache.
     * @param is_l4_hit True if access hit in L4 cache.
     */
    void l4_hit(const bool is_l4_hit) noexcept { _is_l4_hit = is_l4_hit; }

    /**
     * Set whether the access hit in main memory.
     * @param is_memory_hit True if access hit DRAM.
     */
    void memory_hit(const bool is_memory_hit) noexcept { _is_memory_hit = is_memory_hit; }

    /**
     * Set whether the accessed memory is located remotely.
     * @param is_remote True if remote node was accessed.
     */
    void remote(const bool is_remote) noexcept { _is_remote = is_remote; }

    /**
     * Set the number of hops required to reach remote memory.
     * @param remote_hops Number of interconnect hops to memory.
     */
    void remote_hops(const std::uint8_t remote_hops) noexcept { _remote_hops = remote_hops; }

    /**
     * Set whether the accessed memory is marked as uncachable.
     * @param is_uncachable_memory True if memory is uncachable.
     */
    void uncachable_memory(const bool is_uncachable_memory) noexcept { _is_uncachable_memory = is_uncachable_memory; }

    /**
     * Set whether the accessed memory is write-combine.
     * @param is_write_combine_memory True if memory uses write-combine policy.
     */
    void write_combine_memory(const bool is_write_combine_memory) noexcept
    {
      _is_write_combine_memory = is_write_combine_memory;
    }

    /**
     * @return True if access hit L1 cache.
     */
    [[nodiscard]] bool is_l1_hit() const noexcept { return _is_l1_hit; }

    /**
     * @return True if access hit the MHB (if available).
     */
    [[nodiscard]] std::optional<bool> is_mhb_hit() const noexcept { return _is_mhb_hit; }

    /**
     * @return Number of MHB slots used, if applicable.
     */
    [[nodiscard]] std::optional<std::uint8_t> num_mhb_slots_allocated() const noexcept
    {
      return _num_mhb_slots_allocated;
    }

    /**
     * @return True if access hit L2 cache.
     */
    [[nodiscard]] bool is_l2_hit() const noexcept { return _is_l2_hit; }

    /**
     * @return True if access hit L3 cache.
     */
    [[nodiscard]] bool is_l3_hit() const noexcept { return _is_l3_hit; }

    /**
     * @return True if access hit L4 cache.
     */
    [[nodiscard]] bool is_l4_hit() const noexcept { return _is_l4_hit; }

    /**
     * @return True if access hit main memory.
     */
    [[nodiscard]] bool is_memory_hit() const noexcept { return _is_memory_hit; }

    /**
     * @return True if accessed memory was remote.
     */
    [[nodiscard]] bool is_remote() const noexcept { return _is_remote; }

    /**
     * @return True if the access was to a core on the same node.
     */
    [[nodiscard]] std::optional<bool> is_same_node_remote_core() const noexcept
    {
      return _remote_hops.has_value() ? std::make_optional(_remote_hops.value() == 0U) : std::nullopt;
    }

    /**
     * @return True if the access was to another node on the same socket.
     */
    [[nodiscard]] std::optional<bool> is_same_socket_remote_node() const noexcept
    {
      return _remote_hops.has_value() ? std::make_optional(_remote_hops.value() == 1U) : std::nullopt;
    }

    /**
     * @return True if the access was to another socket on the same board.
     */
    [[nodiscard]] std::optional<bool> is_same_board_remote_socket() const noexcept
    {
      return _remote_hops.has_value() ? std::make_optional(_remote_hops.value() == 2U) : std::nullopt;
    }

    /**
     * @return True if the access was to a remote board.
     */
    [[nodiscard]] std::optional<bool> is_remote_board() const noexcept
    {
      return _remote_hops.has_value() ? std::make_optional(_remote_hops.value() == 3U) : std::nullopt;
    }

    /**
     * @return True if memory is uncachable.
     */
    [[nodiscard]] std::optional<bool> is_uncachable_memory() const noexcept { return _is_uncachable_memory; }

    /**
     * @return True if memory uses write-combine policy.
     */
    [[nodiscard]] std::optional<bool> is_write_combine_memory() const noexcept { return _is_write_combine_memory; }

  private:
    bool _is_l1_hit{ false };
    std::optional<bool> _is_mhb_hit{ std::nullopt };
    std::optional<std::uint8_t> _num_mhb_slots_allocated{ std::nullopt };
    bool _is_l2_hit{ false };
    bool _is_l3_hit{ false };
    bool _is_l4_hit{ false };
    bool _is_memory_hit{ false };
    bool _is_remote{ false };
    std::optional<std::uint8_t> _remote_hops{ std::nullopt };
    std::optional<bool> _is_uncachable_memory{ std::nullopt };
    std::optional<bool> _is_write_combine_memory{ std::nullopt };
  };

  /**
   * Describes the TLB resolution for a data access.
   * Contains hit results and page sizes for L1 and L2 TLBs.
   */
  class TLB
  {
  public:
    /**
     * Set whether L1 TLB resolved the access.
     * @param is_l1_hit True if L1 TLB hit.
     */
    void l1_hit(const bool is_l1_hit) noexcept { _is_l1_hit = is_l1_hit; }

    /**
     * Set whether L2 TLB resolved the access.
     * @param is_l2_hit True if L2 TLB hit.
     */
    void l2_hit(const bool is_l2_hit) noexcept { _is_l2_hit = is_l2_hit; }

    /**
     * Set the page size resolved by L1 TLB.
     * @param l1_page_size Page size in bytes.
     */
    void l1_page_size(const std::uint64_t l1_page_size) noexcept { _l1_page_size = l1_page_size; }

    /**
     * Set the page size resolved by L2 TLB.
     * @param l2_page_size Page size in bytes.
     */
    void l2_page_size(const std::uint64_t l2_page_size) noexcept { _l2_page_size = l2_page_size; }

    /**
     * @return True if L1 TLB resolved the access.
     */
    [[nodiscard]] std::optional<bool> is_l1_hit() const noexcept { return _is_l1_hit; }

    /**
     * @return True if L2 TLB resolved the access.
     */
    [[nodiscard]] std::optional<bool> is_l2_hit() const noexcept { return _is_l2_hit; }

    /**
     * @return Page size resolved by L1 TLB.
     */
    [[nodiscard]] std::optional<std::uint64_t> l1_page_size() const noexcept { return _l1_page_size; }

    /**
     * @return Page size resolved by L2 TLB.
     */
    [[nodiscard]] std::optional<std::uint64_t> l2_page_size() const noexcept { return _l2_page_size; }

  private:
    std::optional<bool> _is_l1_hit{ std::nullopt };
    std::optional<bool> _is_l2_hit{ std::nullopt };
    std::optional<std::uint64_t> _l1_page_size{ std::nullopt };
    std::optional<std::uint64_t> _l2_page_size{ std::nullopt };
  };

  /**
   * Describes latency values associated with the memory access path, including
   * time to access data, refill the TLB, or handle cache misses.
   */
  class Latency
  {
  public:
    /**
     * Set total latency for accessing data.
     * @param cache_access Latency in cycles.
     */
    void cache_access(const std::uint32_t cache_access) noexcept { _cache_access = cache_access; }

    /**
     * Set total latency for accessing data.
     * @param cache_access Latency in cycles.
     */
    void cache_access(const std::optional<std::uint32_t> cache_access) noexcept { _cache_access = cache_access; }

    /**
     * Set latency specifically caused by a cache miss.
     * @param cache_miss Latency in cycles.
     */
    void cache_miss(const std::uint32_t cache_miss) noexcept { _cache_miss = cache_miss; }

    /**
     * Set latency specifically caused by a cache miss.
     * @param cache_miss Latency in cycles.
     */
    void cache_miss(const std::optional<std::uint32_t> cache_miss) noexcept { _cache_miss = cache_miss; }

    /**
     * Set latency due to DTLB refill.
     * @param dtlb_refill Latency in cycles.
     */
    void dtlb_refill(const std::uint32_t dtlb_refill) noexcept { _dtlb_refill = dtlb_refill; }

    /**
     * @return Latency of the data access in cycles.
     */
    [[nodiscard]] std::optional<std::uint32_t> cache_access() const noexcept { return _cache_access; }

    /**
     * @return Latency attributed to a cache miss.
     */
    [[nodiscard]] std::optional<std::uint32_t> cache_miss() const noexcept { return _cache_miss; }

    /**
     * @return Latency from a DTLB refill operation.
     */
    [[nodiscard]] std::optional<std::uint32_t> dtlb_refill() const noexcept { return _dtlb_refill; }

  private:
    std::optional<std::uint32_t> _cache_access{ std::nullopt };
    std::optional<std::uint32_t> _cache_miss{ std::nullopt };
    std::optional<std::uint32_t> _dtlb_refill{ std::nullopt };
  };

  /**
   * Represents the outcome of a cache snoop operation in a coherence protocol.
   */
  class Snoop
  {
  public:
    Snoop() noexcept = default;
    Snoop(const Snoop&) noexcept = default;
    Snoop(Snoop&&) noexcept = default;
    ~Snoop() noexcept = default;

    [[nodiscard]] Snoop& operator=(const Snoop&) noexcept = default;
    [[nodiscard]] Snoop& operator=(Snoop&&) noexcept = default;

    /**
     * Set whether the snoop was a hit.
     * @param is_hit True if the snoop found a matching cache line.
     */
    void hit(const bool is_hit) noexcept { _is_hit = is_hit; }

    /**
     * Set whether the snoop hit a modified (dirty) cache line.
     * @param is_hit_modified True if the snoop found a modified copy.
     */
    void hit_modified(const bool is_hit_modified) noexcept { _is_hit_modified = is_hit_modified; }

    /**
     * Set whether the line was forwarded to the requester.
     * @param is_forward True if a cache line was forwarded as part of the response.
     */
    void forward(const bool is_forward) noexcept { _is_forward = is_forward; }

    /**
     * Set whether the line was transferred from another peer.
     * @param is_transfer_from_peer True if data was transferred from a peer cache.
     */
    void transfer_from_peer(const bool is_transfer_from_peer) noexcept
    {
      _is_transfer_from_peer = is_transfer_from_peer;
    }

    /**
     * @return True if the snoop hit a matching cache line.
     */
    [[nodiscard]] std::optional<bool> is_hit() const noexcept { return _is_hit; }

    /**
     * @return True if the snoop hit a modified (dirty) cache line.
     */
    [[nodiscard]] std::optional<bool> is_hit_modified() const noexcept { return _is_hit_modified; }

    /**
     * @return True if the cache line was forwarded to the requester.
     */
    [[nodiscard]] std::optional<bool> is_forward() const noexcept { return _is_forward; }

    /**
     * @return True if the cache line was transferred from a peer cache.
     */
    [[nodiscard]] std::optional<bool> is_transfer_from_peer() const noexcept { return _is_transfer_from_peer; }

  private:
    std::optional<bool> _is_hit{ std::nullopt };
    std::optional<bool> _is_hit_modified{ std::nullopt };
    std::optional<bool> _is_forward{ std::nullopt };
    std::optional<bool> _is_transfer_from_peer{ std::nullopt };
  };

  /**
   * Set the type of the access.
   * @param access_type Type of the access.
   */
  void type(const AccessType access_type) noexcept { _access_type = access_type; }

  /**
   * Set whether the daa access is locked.
   * @param is_locked Lock indicator.
   */
  void locked(const std::optional<bool> is_locked) noexcept { _is_locked = is_locked; }

  /**
   * Set the logical memory address of the access.
   * @param logical_memory_address Virtual/logical memory address.
   */
  void logical_memory_address(const std::uintptr_t logical_memory_address) noexcept
  {
    _logical_memory_address = logical_memory_address;
  }

  /**
   * Set the physical memory address of the access.
   * @param physical_memory_address Physical address resolved.
   */
  void physical_memory_address(const std::uintptr_t physical_memory_address) noexcept
  {
    _physical_memory_address = physical_memory_address;
  }

  /**
   * Set the source characteristics of the data access.
   * @param source Populated Source instance.
   */
  void source(std::optional<Source> source) noexcept { _source = source; }

  /**
   * Set the snoop characteristics of the data access.
   * @param snoop Populated Snoop instance.
   */
  void snoop(std::optional<Snoop> snoop) noexcept { _snoop = snoop; }

  /**
   * Set whether the access incurred a misalignment penalty.
   * @param is_misalign_penalty True if misaligned access penalized.
   */
  void misalign_penalty(const bool is_misalign_penalty) noexcept { _is_misalign_penalty = is_misalign_penalty; }

  /**
   * Set the byte-width of the access.
   * @param access_width Size of access in bytes.
   */
  void access_width(std::uint8_t access_width) noexcept { _access_width = access_width; }

  /**
   * Set the page size backing the accessed memory.
   * @param page_size Page size in bytes.
   */
  void page_size(const std::uint64_t page_size) noexcept { _data_page_size = page_size; }

  /**
   * @return Type of the access.
   */
  [[nodiscard]] std::optional<AccessType> type() const noexcept { return _access_type; }

  /**
   * @return Lock indicator, if available. std::nullopt otherwise.
   */
  [[nodiscard]] std::optional<bool> is_locked() const noexcept { return _is_locked; }

  /**
   * @return True, if the access is a load.
   */
  [[nodiscard]] bool is_load() const { return _access_type.has_value() && _access_type.value() == AccessType::Load; }

  /**
   * @return True, if the access is a store.
   */
  [[nodiscard]] bool is_store() const { return _access_type.has_value() && _access_type.value() == AccessType::Store; }

  /**
   * @return True, if the access is a software prefetch.
   */
  [[nodiscard]] bool is_software_prefetch() const
  {
    return _access_type.has_value() && _access_type.value() == AccessType::SoftwarePrefetch;
  }

  /**
   * @return Logical (virtual) address accessed.
   */
  [[nodiscard]] std::optional<std::uintptr_t> logical_memory_address() const noexcept
  {
    return _logical_memory_address;
  }

  /**
   * @return Physical address accessed.
   */
  [[nodiscard]] std::optional<std::uintptr_t> physical_memory_address() const noexcept
  {
    return _physical_memory_address;
  }

  /**
   * @return Source information for where the access was resolved.
   */
  [[nodiscard]] const std::optional<Source>& source() const noexcept { return _source; }

  /**
   * @return Source information for where the access was resolved.
   */
  [[nodiscard]] std::optional<Source>& source() noexcept { return _source; }

  /**
   * @return TLB resolution state for this access.
   */
  [[nodiscard]] const TLB& tlb() const noexcept { return _tlb; }

  /**
   * @return TLB resolution state for this access.
   */
  [[nodiscard]] TLB& tlb() noexcept { return _tlb; }

  /**
   * @return Latency values associated with this access.
   */
  [[nodiscard]] const Latency& latency() const noexcept { return _latency; }

  /**
   * @return Latency values associated with this access.
   */
  [[nodiscard]] Latency& latency() noexcept { return _latency; }

  /**
   * @return Snoop information associated with this access.
   */
  [[nodiscard]] const std::optional<Snoop>& snoop() const noexcept { return _snoop; }

  /**
   * @return True if a misalignment penalty was incurred.
   */
  [[nodiscard]] std::optional<bool> is_misalign_penalty() const noexcept { return _is_misalign_penalty; }

  /**
   * @return Width of the data access in bytes.
   */
  [[nodiscard]] std::optional<std::uint8_t> access_width() const noexcept { return _access_width; }

  /**
   * @return Backing memory page size.
   */
  [[nodiscard]] std::optional<std::uint64_t> page_size() const noexcept { return _data_page_size; }

private:
  std::optional<AccessType> _access_type;
  std::optional<bool> _is_locked{ std::nullopt };
  std::optional<std::uintptr_t> _logical_memory_address;
  std::optional<std::uintptr_t> _physical_memory_address;
  std::optional<Source> _source;
  TLB _tlb;
  Latency _latency;
  std::optional<Snoop> _snoop{ std::nullopt };
  std::optional<bool> _is_misalign_penalty{ std::nullopt };
  std::optional<std::uint8_t> _access_width{ std::nullopt };
  std::optional<std::uint64_t> _data_page_size{ std::nullopt };
};

/**
 * Convert AccessType to its string representation.
 *
 * @param type The access type to convert.
 * @return String representation of the access type.
 */
[[nodiscard]] inline std::string
to_string(const DataAccess::AccessType type)
{
  switch (type) {
    case DataAccess::AccessType::Load:
      return "load";
    case DataAccess::AccessType::Store:
      return "store";
    case DataAccess::AccessType::SoftwarePrefetch:
      return "sw_prefetch";
  }
  return "unknown";
}

} // namespace perf