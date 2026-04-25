#pragma once
#include <cstdint>
#include <vector>

namespace perf {
/**
 * Implements mechanisms to parse raw data collected by the fetch PMU based on AMD's Instruction Based Sampling.
 * Some of these information provided by IBS are not accessible through the perf_event_open interface.
 */
class IBSFetchDecoder
{
public:
  explicit IBSFetchDecoder(const std::vector<std::byte>& raw_data) noexcept
    : _fetch_data(reinterpret_cast<const FetchData*>(raw_data.data() + /* 4 byte offset */ 4U))
  {
  }

  IBSFetchDecoder(const IBSFetchDecoder&) = default;
  IBSFetchDecoder(IBSFetchDecoder&&) noexcept = default;
  ~IBSFetchDecoder() noexcept = default;
  IBSFetchDecoder& operator=(const IBSFetchDecoder&) = default;
  IBSFetchDecoder& operator=(IBSFetchDecoder&&) noexcept = default;

  [[nodiscard]] bool is_valid() const noexcept { return _fetch_data->_fetch_control.is_fetch_valid; }
  [[nodiscard]] bool is_op_cache_miss() const noexcept { return _fetch_data->_fetch_control.is_fetch_op_cache_miss; }
  [[nodiscard]] bool is_instruction_cache_miss() const noexcept
  {
    return _fetch_data->_fetch_control.is_instruction_cache_miss;
  }
  [[nodiscard]] bool is_l2_miss() const noexcept { return _fetch_data->_fetch_control.is_fetch_l2_miss; }
  [[nodiscard]] bool is_l3_miss() const noexcept { return _fetch_data->_fetch_control.is_fetch_l3_miss; }
  [[nodiscard]] bool is_l1_tlb_miss() const noexcept { return _fetch_data->_fetch_control.is_l1_tlb_miss; }
  [[nodiscard]] bool is_l2_tlb_miss() const noexcept { return _fetch_data->_fetch_control.is_l2_tlb_miss; }
  [[nodiscard]] std::uint8_t l1_tlb_page_size() const noexcept
  {
    return static_cast<std::uint8_t>(_fetch_data->_fetch_control.l1_tlb_page_size);
  }
  [[nodiscard]] bool is_complete() const noexcept { return _fetch_data->_fetch_control.is_fetch_complete; }
  [[nodiscard]] std::uint16_t latency() const noexcept { return _fetch_data->_fetch_control.fetch_latency; }

  [[nodiscard]] std::uintptr_t linear_instruction_address() const noexcept
  {
    return _fetch_data->_linear_instruction_address;
  }

  [[nodiscard]] std::uintptr_t physical_instruction_address() const noexcept
  {
    return _fetch_data->_physical_instruction_address *
           static_cast<std::uint64_t>(_fetch_data->_fetch_control.is_physical_address_valid);
  }

  [[nodiscard]] bool is_physical_instruction_address_valid() const noexcept
  {
    return _fetch_data->_fetch_control.is_physical_address_valid;
  }

private:
  struct FetchControl
  {
    std::uint64_t fetch_max_count : 16, fetch_count : 16, fetch_latency : 16, is_fetch_enable : 1, is_fetch_valid : 1,
      is_fetch_complete : 1, is_instruction_cache_miss : 1, is_physical_address_valid : 1, l1_tlb_page_size : 2,
      is_l1_tlb_miss : 1, is_l2_tlb_miss : 1, is_random_tagging_enabled : 1, is_fetch_l2_miss : 1, is_l3_miss_only : 1,
      is_fetch_op_cache_miss : 1, is_fetch_l3_miss : 1, reserved0 : 2;
  };

  struct FetchControlExtended
  {
    std::uint64_t itlb_refill_latency : 16, reserved0 : 48;
  };

  struct FetchData
  {
    FetchControl _fetch_control;
    std::uintptr_t _linear_instruction_address;
    std::uintptr_t _physical_instruction_address;
  };

  const FetchData* _fetch_data{ nullptr };
};

/**
 * Implements mechanisms to parse raw data collected by the execution PMU based on AMD's Instruction Based Sampling.
 * Some of these information provided by IBS are not accessible through the perf_event_open interface.
 */
class IBSOpDecoder
{
public:
  explicit IBSOpDecoder(const std::vector<std::byte>& raw_data) noexcept
    : _execution_data(reinterpret_cast<const ExecutionData*>(raw_data.data() + /* 4 byte offset */ 4U))
  {
  }

  IBSOpDecoder(const IBSOpDecoder&) = default;
  IBSOpDecoder(IBSOpDecoder&&) noexcept = default;
  ~IBSOpDecoder() noexcept = default;
  IBSOpDecoder& operator=(const IBSOpDecoder&) = default;
  IBSOpDecoder& operator=(IBSOpDecoder&&) noexcept = default;

  [[nodiscard]] std::uint16_t completion_to_retire_latency() const noexcept
  {
    return _execution_data->_op_data1.completion_to_retire_count;
  }
  [[nodiscard]] std::uint16_t tag_to_retire_latency() const noexcept
  {
    return _execution_data->_op_data1.tag_to_retire_count;
  }
  [[nodiscard]] bool is_return_operation() const noexcept { return _execution_data->_op_data1.is_return_operation; }
  [[nodiscard]] bool is_branch_taken_operation() const noexcept
  {
    return _execution_data->_op_data1.is_brn_taken_operation;
  }
  [[nodiscard]] bool is_branch_mispredicted_operation() const noexcept
  {
    return _execution_data->_op_data1.is_brn_misp_operation;
  }
  [[nodiscard]] bool is_branch_retired_operation() const noexcept
  {
    return _execution_data->_op_data1.is_brn_ret_operation;
  }
  [[nodiscard]] bool is_branch() const noexcept
  {
    return is_branch_taken_operation() || is_branch_mispredicted_operation() || is_branch_retired_operation() ||
           is_branch_fuse();
  }
  [[nodiscard]] bool is_branch_fuse() const noexcept { return _execution_data->_op_data1.is_brn_fuse; }
  [[nodiscard]] bool is_microcode() const noexcept { return _execution_data->_op_data1.is_microcode; }
  [[nodiscard]] bool is_remote_node() const noexcept { return _execution_data->_op_data2.is_remote_node; }
  [[nodiscard]] bool is_cache_hit() const noexcept { return _execution_data->_op_data2.is_cache_hit; }
  [[nodiscard]] bool is_load_operation() const noexcept { return _execution_data->_op_data3.is_load_operation; }
  [[nodiscard]] bool is_store_operation() const noexcept { return _execution_data->_op_data3.is_store_operation; }
  [[nodiscard]] bool is_software_prefetch() const noexcept { return _execution_data->_op_data3.is_software_prefetch; }
  [[nodiscard]] bool is_l1_data_tlb_miss() const noexcept
  {
    return _execution_data->_op_data3.is_data_cache_l1_tlb_miss;
  }
  [[nodiscard]] bool is_l2_data_tlb_miss() const noexcept
  {
    return _execution_data->_op_data3.is_data_cache_l2_tlb_miss;
  }
  [[nodiscard]] bool is_l1_data_tlb_hit_2m() const noexcept
  {
    return _execution_data->_op_data3.is_data_cache_l1_tlb_hit_2m;
  }
  [[nodiscard]] bool is_l1_data_tlb_hit_1g() const noexcept
  {
    return _execution_data->_op_data3.is_data_cache_l1_tlb_hit_1g;
  }
  [[nodiscard]] bool is_l2_data_tlb_hit_2m() const noexcept
  {
    return _execution_data->_op_data3.is_data_cache_l2_tlb_hit_2m;
  }
  [[nodiscard]] bool is_l2_data_tlb_hit_1g() const noexcept
  {
    return _execution_data->_op_data3.is_data_cache_l2_tlb_hit_1g;
  }
  [[nodiscard]] bool is_data_cache_miss() const noexcept { return _execution_data->_op_data3.is_data_cache_miss; }
  [[nodiscard]] bool is_data_cache_misaligned_access() const noexcept
  {
    return _execution_data->_op_data3.is_data_cache_misaligned_access;
  }
  [[nodiscard]] bool is_data_cache_write_combine_access() const noexcept
  {
    return _execution_data->_op_data3.is_data_cache_write_combine_access;
  }
  [[nodiscard]] bool is_data_cache_uncachable_access() const noexcept
  {
    return _execution_data->_op_data3.is_data_cache_uncachable_access;
  }
  [[nodiscard]] bool is_data_cache_locked_operation() const noexcept
  {
    return _execution_data->_op_data3.is_data_cache_locked_operation;
  }
  [[nodiscard]] bool is_data_cache_miss_no_mab_allocation() const noexcept
  {
    return _execution_data->_op_data3.is_data_cache_miss_no_mab_allocation;
  }
  [[nodiscard]] bool is_l2_miss() const noexcept { return _execution_data->_op_data3.is_l2_miss; }
  [[nodiscard]] std::uint8_t access_mem_width() const noexcept
  {
    return static_cast<std::uint8_t>(_execution_data->_op_data3.op_mem_width);
  }
  [[nodiscard]] std::uint8_t num_open_mem_requests() const noexcept
  {
    return static_cast<std::uint8_t>(_execution_data->_op_data3.num_op_data_cache_miss_open_mem_requests);
  }
  [[nodiscard]] std::uint16_t data_cache_miss_latency() const noexcept
  {
    return _execution_data->_op_data3.data_cache_miss_latency;
  }
  [[nodiscard]] std::uint16_t tlb_refill_latency() const noexcept
  {
    return _execution_data->_op_data3.tlb_refill_latency;
  }

  [[nodiscard]] std::uintptr_t linear_instruction_address() const noexcept
  {
    return _execution_data->_linear_instruction_address;
  }

  [[nodiscard]] std::uintptr_t linear_memory_address() const noexcept
  {
    return _execution_data->_linear_memory_address *
           static_cast<std::uint64_t>(_execution_data->_op_data3.is_data_cache_linear_address_valid);
  }

  [[nodiscard]] std::uintptr_t physical_memory_address() const noexcept
  {
    return _execution_data->_physical_memory_address *
           static_cast<std::uint64_t>(_execution_data->_op_data3.is_data_cache_physical_address_valid);
  }

  [[nodiscard]] std::uintptr_t branch_target_address() const noexcept
  {
    return _execution_data->_branch_target_address;
  }

private:
  struct OpData1
  {
    std::uint64_t completion_to_retire_count : 16, tag_to_retire_count : 16, reserved0 : 2, is_return_operation : 1,
      is_brn_taken_operation : 1, is_brn_misp_operation : 1, is_brn_ret_operation : 1, is_rip_invalid : 1,
      is_brn_fuse : 1, is_microcode : 1, reserved1 : 23;
  };

  struct OpData2
  {
    std::uint64_t data_source_lo : 3, reserved0 : 1, is_remote_node : 1, is_cache_hit : 1, data_source_hi : 2,
      reserved1 : 56;
  };

  struct OpData3
  {
    std::uint64_t is_load_operation : 1, is_store_operation : 1, is_data_cache_l1_tlb_miss : 1,
      is_data_cache_l2_tlb_miss : 1, is_data_cache_l1_tlb_hit_2m : 1, is_data_cache_l1_tlb_hit_1g : 1,
      is_data_cache_l2_tlb_hit_2m : 1, is_data_cache_miss : 1, is_data_cache_misaligned_access : 1, reserved0 : 4,
      is_data_cache_write_combine_access : 1, is_data_cache_uncachable_access : 1, is_data_cache_locked_operation : 1,
      is_data_cache_miss_no_mab_allocation : 1, is_data_cache_linear_address_valid : 1,
      is_data_cache_physical_address_valid : 1, is_data_cache_l2_tlb_hit_1g : 1, is_l2_miss : 1,
      is_software_prefetch : 1, op_mem_width : 4, num_op_data_cache_miss_open_mem_requests : 6,
      data_cache_miss_latency : 16, tlb_refill_latency : 16;
  };

  struct ExecutionData
  {
    std::uint64_t _execution_control_register;
    std::uintptr_t _linear_instruction_address;
    OpData1 _op_data1;
    OpData2 _op_data2;
    OpData3 _op_data3;
    std::uintptr_t _linear_memory_address;
    std::uintptr_t _physical_memory_address;
    std::uintptr_t _branch_target_address;
  };

  const ExecutionData* _execution_data{ nullptr };
};
}