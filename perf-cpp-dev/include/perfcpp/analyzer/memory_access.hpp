#pragma once

#include <cstdint>
#include <list>
#include <perfcpp/analyzer/data_type.hpp>
#include <perfcpp/hardware_info.hpp>
#include <perfcpp/sample/result.hpp>
#include <perfcpp/sample/sample.hpp>
#include <set>
#include <string>
#include <string_view>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace perf::analyzer {
class MemoryAccessResult
{
public:
  MemoryAccessResult() = default;

  explicit MemoryAccessResult(std::vector<DataType>&& result) noexcept
    : _data_types(std::move(result))
  {
  }
  MemoryAccessResult(const MemoryAccessResult&) = default;
  MemoryAccessResult(MemoryAccessResult&&) noexcept = default;
  ~MemoryAccessResult() = default;
  MemoryAccessResult& operator=(const MemoryAccessResult&) = default;
  MemoryAccessResult& operator=(MemoryAccessResult&&) noexcept = default;

  [[nodiscard]] const std::vector<DataType>& data_types() const noexcept { return _data_types; }
  [[nodiscard]] std::vector<DataType>& data_types() noexcept { return _data_types; }

  [[nodiscard]] std::string to_string() const;
  [[nodiscard]] std::string to_json() const;
  [[nodiscard]] std::string to_csv(std::string_view data_type_name,
                                   char delimiter = ',',
                                   bool print_header = true) const;

private:
  std::vector<DataType> _data_types;

  class MemberStatistic
  {
  public:
    class Group
    {
    public:
      Group() noexcept = default;
      Group(const Group&) = default;
      Group(Group&&) noexcept = default;
      ~Group() noexcept = default;
      Group& operator=(const Group&) = default;
      Group& operator=(Group&&) noexcept = default;

      [[nodiscard]] std::uint64_t count() const noexcept { return _count; }
      [[nodiscard]] std::uint64_t average_cache_latency() const noexcept
      {
        return _count > 0U ? _cache_latency / _count : 0U;
      }
      [[nodiscard]] std::uint64_t average_instruction_latency() const noexcept
      {
        return _count > 0U ? _instr_latency / _count : 0U;
      }
      [[nodiscard]] std::uint64_t average_dtlb_latency() const noexcept
      {
        return _count > 0U ? _dtlb_latency / _count : 0U;
      }
      [[nodiscard]] std::uint64_t count_l1_hits() const noexcept { return _count_l1_hits; }
      [[nodiscard]] std::uint64_t count_mhb_hits() const noexcept { return _count_mhb_hits; }
      [[nodiscard]] std::uint64_t count_l2_hits() const noexcept { return _count_l2_hits; }
      [[nodiscard]] std::uint64_t count_l3_hits() const noexcept { return _count_l3_hits; }
      [[nodiscard]] std::uint64_t count_local_ram_hits() const noexcept { return _count_local_ram_hits; }
      [[nodiscard]] std::uint64_t count_remote_ram_hits() const noexcept { return _count_remote_ram_hits; }
      [[nodiscard]] std::uint64_t average_alloc_mab_entries() const noexcept
      {
        return _count > 0U ? _alloc_mab_entries / _count : 0U;
      }
      [[nodiscard]] std::uint64_t dtlb_hits() const noexcept { return _dtlb_hits; }
      [[nodiscard]] std::uint64_t stlb_hits() const noexcept { return _stlb_hits; }
      [[nodiscard]] std::uint64_t stlb_misses() const noexcept { return _stlb_misses; }

      Group& operator+=(const Sample& sample) noexcept
      {
        if (const auto data_src = sample.data_access().source(); data_src.has_value()) {
          ++_count;

          /// Instruction type and latency.
          _count_l1_hits += static_cast<std::uint64_t>(data_src->is_l1_hit());
          _count_mhb_hits += static_cast<std::uint64_t>(data_src->is_mhb_hit().value_or(false));
          _count_l2_hits += static_cast<std::uint64_t>(data_src->is_l2_hit());
          _count_l3_hits += static_cast<std::uint64_t>(data_src->is_l3_hit());
          _count_local_ram_hits += static_cast<std::uint64_t>(data_src->is_memory_hit() && !data_src->is_remote());
          _count_remote_ram_hits += static_cast<std::uint64_t>(data_src->is_memory_hit() && data_src->is_remote());

          if (HardwareInfo::is_intel()) {
            _cache_latency += sample.data_access().latency().cache_access().value_or(0U);
            _instr_latency += sample.instruction_execution().latency().instruction_retirement().value_or(0U);
          } else if (HardwareInfo::is_amd()) {
            _cache_latency += sample.data_access().latency().cache_miss().value_or(0U);
            _instr_latency += sample.instruction_execution().latency().uop_tag_to_completion().value_or(0U);
            _dtlb_latency += sample.data_access().latency().dtlb_refill().value_or(0U);

            _alloc_mab_entries += data_src->num_mhb_slots_allocated().value_or(0U);
          }

          _dtlb_hits += static_cast<std::uint64_t>(sample.data_access().tlb().is_l1_hit().value_or(false));
          _stlb_hits += static_cast<std::uint64_t>(sample.data_access().tlb().is_l2_hit().value_or(false));
          _stlb_misses += static_cast<std::uint64_t>(!sample.data_access().tlb().is_l1_hit().value_or(true) &&
                                                     !sample.data_access().tlb().is_l2_hit().value_or(true));
        }

        return *this;
      }

    private:
      std::uint64_t _count{ 0ULL };
      std::uint64_t _cache_latency{ 0ULL };
      std::uint64_t _instr_latency{ 0ULL };
      std::uint64_t _dtlb_latency{ 0ULL };
      std::uint64_t _count_l1_hits{ 0ULL };
      std::uint64_t _count_mhb_hits{ 0ULL };
      std::uint64_t _count_l2_hits{ 0ULL };
      std::uint64_t _count_l3_hits{ 0ULL };
      std::uint64_t _count_local_ram_hits{ 0ULL };
      std::uint64_t _count_remote_ram_hits{ 0ULL };
      std::uint64_t _alloc_mab_entries{ 0ULL };
      std::uint64_t _dtlb_hits{ 0ULL };
      std::uint64_t _stlb_hits{ 0ULL };
      std::uint64_t _stlb_misses{ 0ULL };
    };

    MemberStatistic() noexcept = default;
    MemberStatistic(const MemberStatistic&) = default;
    MemberStatistic(MemberStatistic&&) noexcept = default;
    ~MemberStatistic() noexcept = default;
    MemberStatistic& operator=(const MemberStatistic&) = default;
    MemberStatistic& operator=(MemberStatistic&&) noexcept = default;

    MemberStatistic& operator+=(const Sample& sample)
    {
      if (!sample.data_access().source().has_value()) {
        return *this;
      }

      if (!sample.data_access().type().has_value()) {
        return *this;
      }

      if (sample.data_access().is_load()) {
        _loads += sample;
      } else if (sample.data_access().is_software_prefetch()) {
        _software_prefetches += sample;
      } else if (sample.data_access().is_store()) {
        _stores += sample;
      }

      return *this;
    }

    [[nodiscard]] const Group& loads() const noexcept { return _loads; }
    [[nodiscard]] const Group& software_prefetches() const noexcept { return _software_prefetches; }
    [[nodiscard]] const Group& stores() const noexcept { return _stores; }

    [[nodiscard]] bool has_software_prefetch() const noexcept { return _software_prefetches.count() > 0U; }
    [[nodiscard]] bool has_stores() const noexcept { return _stores.count() > 0U; }

  private:
    Group _loads;
    Group _software_prefetches;
    Group _stores;
  };
};

class MemoryAccess
{
public:
  MemoryAccess() { _data_type_instances.reserve(128U); }

  MemoryAccess(const MemoryAccess&) = default;
  MemoryAccess(MemoryAccess&&) noexcept = default;
  ~MemoryAccess() = default;
  MemoryAccess& operator=(const MemoryAccess&) = default;
  MemoryAccess& operator=(MemoryAccess&&) noexcept = default;

  /**
   * Adds a data type to the analyzer.
   *
   * @param data_type Data type to add.
   */
  void add(DataType&& data_type);

  /**
   * Annotates the given object with the given type.
   *
   * @param data_type_name Name of the (registered) data type.
   * @param data_object Data object to annotate.
   * @param instance_name Tag to differentiate multiple instances of the same type (optional).
   */
  template<typename T>
  void annotate(const std::string_view data_type_name, T* data_object, std::string&& instance_name = "")
  {
    annotate(data_type_name, std::uintptr_t(data_object), instance_name);
  }

  /**
   * Annotates the given object with the given type.
   *
   * @param data_type_name Name of the (registered) data type.
   * @param data_object Data object to annotate.
   * @param instance_name Tag to differentiate multiple instances of the same type (optional).
   */
  template<typename T>
  void annotate(const std::string_view data_type_name, T* data_object, const std::string& instance_name)
  {
    annotate(data_type_name, std::uintptr_t(data_object), instance_name);
  }

  /**
   * Annotates the given object with the given type.
   *
   * @param data_type_name Name of the (registered) data type.
   * @param data_object Data object to annotate.
   * @param instance_name Tag to differentiate multiple instances of the same type (optional).
   */
  template<typename T>
  void annotate(const std::string_view data_type_name, const T* data_object, std::string&& instance_name = "")
  {
    annotate(data_type_name, std::uintptr_t(data_object), instance_name);
  }

  /**
   * Annotates the given object with the given type.
   *
   * @param data_type_name Name of the (registered) data type.
   * @param data_object Data object to annotate.
   * @param instance_name Tag to differentiate multiple instances of the same type (optional).
   */
  template<typename T>
  void annotate(const std::string_view data_type_name, const T* data_object, const std::string& instance_name)
  {
    annotate(data_type_name, std::uintptr_t(data_object), instance_name);
  }

  /**
   * Annotates the given object with the given type.
   *
   * @param data_type_name Name of the (registered) data type.
   * @param data_object Data object to annotate.
   * @param instance_name Tag to differentiate multiple instances of the same type (optional).
   */
  template<typename T>
  void annotate(const std::string_view data_type_name, const T& data_object, std::string&& instance_name = "")
  {
    annotate(data_type_name, std::uintptr_t(&data_object), instance_name);
  }

  /**
   * Annotates the given object with the given type.
   *
   * @param data_type_name Name of the (registered) data type.
   * @param data_object Data object to annotate.
   * @param instance_name Tag to differentiate multiple instances of the same type (optional).
   */
  template<typename T>
  void annotate(const std::string_view data_type_name, const T& data_object, const std::string& instance_name)
  {
    annotate(data_type_name, std::uintptr_t(&data_object), instance_name);
  }

  /**
   * Annotates the given objects with the given type.
   *
   * @param data_type_name Name of the (registered) data type.
   * @param data_objects Array of data objects to annotate.
   * @param size Size of the array.
   * @param instance_name Tag to differentiate multiple instances of the same type (optional).
   */
  template<typename T>
  void annotate(const std::string_view data_type_name,
                const T* data_objects,
                std::size_t size,
                std::string&& instance_name = "")
  {
    for (auto i = 0ULL; i < size; ++i) {
      annotate(data_type_name, std::uintptr_t(&data_objects[i]), instance_name);
    }
  }

  /**
   * Annotates container of objects with the given type.
   *
   * @param data_type_name Name of the (registered) data type.
   * @param begin Begin of the container.
   * @param end End of the container.
   * @param instance_name Tag to differentiate multiple instances of the same type (optional).
   */
  template<typename I>
  void annotate(const std::string_view data_type_name, I begin, I end, std::string&& instance_name = "")
  {
    annotate(data_type_name, begin, end, instance_name);
  }

  /**
   * Annotates container of objects with the given type.
   *
   * @param data_type_name Name of the (registered) data type.
   * @param begin Begin of the container.
   * @param end End of the container.
   * @param instance_name Tag to differentiate multiple instances of the same type (optional).
   */
  template<typename I>
  void annotate(const std::string_view data_type_name, I begin, I end, const std::string& instance_name)
  {
    for (auto iterator = begin; iterator != end; ++iterator) {
      annotate(data_type_name, std::uintptr_t(&*iterator), instance_name);
    }
  }

  /**
   * Annotates the given objects with the given type.
   *
   * @param data_type_name Name of the (registered) data type.
   * @param data_objects Data objects to annotate.
   * @param instance_name Tag to differentiate multiple instances of the same type (optional).
   */
  template<typename T>
  void annotate(const std::string_view data_type_name,
                const std::vector<T>& data_objects,
                std::string&& instance_name = "")
  {
    annotate(data_type_name, data_objects.cbegin(), data_objects.cend(), instance_name);
  }

  /**
   * Annotates the given objects with the given type.
   *
   * @param data_type_name Name of the (registered) data type.
   * @param data_objects Data objects to annotate.
   * @param instance_name Tag to differentiate multiple instances of the same type (optional).
   */
  template<typename T>
  void annotate(const std::string_view data_type_name,
                const std::unordered_set<T>& data_objects,
                std::string&& instance_name = "")
  {
    for (const auto& data_object : data_objects) {
      annotate(data_type_name, std::uintptr_t(&data_object), instance_name);
    }
  }

  /**
   * Annotates the given objects with the given type.
   *
   * @param data_type_name Name of the (registered) data type.
   * @param data_objects Data objects to annotate.
   * @param instance_name Tag to differentiate multiple instances of the same type (optional).
   */
  template<typename T>
  void annotate(const std::string_view data_type_name,
                const std::set<T>& data_objects,
                std::string&& instance_name = "")
  {
    for (const auto& data_object : data_objects) {
      annotate(data_type_name, std::uintptr_t(&data_object), instance_name);
    }
  }

  /**
   * Annotates the given objects with the given type.
   *
   * @param data_type_name Name of the (registered) data type.
   * @param data_objects Data objects to annotate.
   * @param instance_name Tag to differentiate multiple instances of the same type (optional).
   */
  template<typename T>
  void annotate(const std::string_view data_type_name,
                const std::list<T>& data_objects,
                std::string&& instance_name = "")
  {
    annotate(data_type_name, data_objects.cbegin(), data_objects.cend(), instance_name);
  }

  /**
   * Maps the given samples (with memory addresses) to data object earlier added to the analyzer.
   *
   * @param samples Samples to map.
   * @return A list of all data types enriched with samples that map to members of the data type.
   */
  MemoryAccessResult map(const std::vector<Sample>& samples)
  {
    return map(SampleResult{ std::vector<Sample>(samples) });
  }

  /**
   * Maps the given samples (with memory addresses) to data object earlier added to the analyzer.
   *
   * @param sample_result Samples to map.
   * @return A list of all data types enriched with samples that map to members of the data type.
   */
  MemoryAccessResult map(const SampleResult& sample_result);

private:
  /// List of all data types and their instances.
  std::vector<std::pair<DataType, std::unordered_map<std::string, std::vector<std::uintptr_t>>>> _data_type_instances;

  /**
   * Finds a registered data type with the given name.
   *
   * @param data_type_name Name of the data type to lookup.
   * @return Iterator of the data type.
   */
  std::vector<std::pair<DataType, std::unordered_map<std::string, std::vector<std::uintptr_t>>>>::iterator find(
    std::string_view data_type_name) noexcept;

  void annotate(std::string_view data_type_name, std::uintptr_t data_object, const std::string& instance_name);

  /**
   * Fills up the data objects with members in wholes (e.g., space between to members or space between the last member
   * and the end of the data object). This might highlight data objects that are not specified entirely.
   *
   * @param data_type Data type to fill up.
   */
  static void add_empty_attributes(DataType& data_type);

  class DataTypeInstanceComp
  {
  public:
    /**
     * Operator used for performing lower_bound in (instance, data_type) pairs.
     */
    bool operator()(const std::pair<std::uintptr_t, std::reference_wrapper<DataType>>& item,
                    const std::uintptr_t address) const
    {
      return std::get<0>(item) <= address;
    }

    /**
     * Operator used for sorting the (instance, data_type) pairs.
     */
    bool operator()(const std::pair<std::uintptr_t, std::reference_wrapper<DataType>>& left,
                    const std::pair<std::uintptr_t, std::reference_wrapper<DataType>>& right) const
    {
      return std::get<0>(left) < std::get<0>(right);
    }
  };
};
}