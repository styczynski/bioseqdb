#pragma once

#include <cstdint>
#include <optional>
#include <sched.h>

namespace perf {
/**
 * The Process represents a process ID for monitoring specific processes or the calling process.
 */
class Process
{
public:
  static Process Any;
  static Process Calling;

  explicit Process(const pid_t process_id)
    : _process_id(process_id)
  {
  }

  /**
   * @return The process ID.
   */
  explicit operator pid_t() const noexcept { return _process_id; }

  /**
   * @return True if this process represents any process, false otherwise.
   */
  [[nodiscard]] bool is_any() const noexcept { return _process_id == Any._process_id; }

  /**
   * @return True if this process represents the calling process, false otherwise.
   */
  [[nodiscard]] bool is_calling() const noexcept { return _process_id == Calling._process_id; }

  /**
   * Compares two processes for equality.
   *
   * @param other Process to compare with.
   * @return True if both processes have the same process ID, false otherwise.
   */
  [[nodiscard]] bool operator==(const Process other) const noexcept { return _process_id == other._process_id; }

  /**
   * Compares two processes for equality.
   *
   * @param process_id Process to compare with.
   * @return True if both processes have the same process ID, false otherwise.
   */
  [[nodiscard]] bool operator==(const pid_t process_id) const noexcept { return _process_id == process_id; }

private:
  pid_t _process_id;
};

/**
 * The CpuCore represents a CPU core ID for monitoring specific cores or any core.
 */
class CpuCore
{
public:
  static CpuCore Any;

  explicit CpuCore(const std::uint16_t cpu_core_id)
    : _cpu_core_id(cpu_core_id)
  {
  }

  /**
   * @return The CPU core ID.
   */
  explicit operator std::int32_t() const noexcept { return _cpu_core_id; }

  /**
   * @return True if this CPU core represents any core, false otherwise.
   */
  [[nodiscard]] bool is_any() const noexcept { return _cpu_core_id == Any._cpu_core_id; }

  /**
   * Compares two CPU cores for equality.
   *
   * @param other CPU core to compare with.
   * @return True if both cores have the same CPU core ID, false otherwise.
   */
  [[nodiscard]] bool operator==(const CpuCore other) const noexcept { return _cpu_core_id == other._cpu_core_id; }

  /**
   * Compares two CPU cores for equality.
   *
   * @param other CPU core to compare with.
   * @return True if both cores have the same CPU core ID, false otherwise.
   */
  [[nodiscard]] bool operator==(const std::uint16_t other) const noexcept { return _cpu_core_id == other; }

private:
  std::int32_t _cpu_core_id;

  explicit CpuCore(const std::int32_t cpu_core_id)
    : _cpu_core_id(cpu_core_id)
  {
  }
};

/**
 * The Config specifies the configuration for monitoring and sampling performance counters,
 * including hardware counter limits, monitored scopes, and target process/CPU selection.
 */
class Config
{
public:
  Config() noexcept;
  Config(const std::uint8_t max_groups, const std::uint8_t max_counters_per_group) noexcept
    : _num_physical_counters(max_groups)
    , _num_events_per_physical_counter(max_counters_per_group)
  {
  }
  ~Config() noexcept = default;
  Config(const Config&) noexcept = default;
  Config& operator=(const Config&) noexcept = default;

  Config& operator=(Config&&) noexcept = default;
  Config(Config&&) noexcept = default;

  /**
   * @return Number of physical hardware counters.
   */
  [[nodiscard]] std::uint8_t num_physical_counters() const noexcept { return _num_physical_counters; }

  /**
   * @return Maximum number of events per physical performance counter.
   */
  [[nodiscard]] std::uint8_t num_events_per_physical_counter() const noexcept
  {
    return _num_events_per_physical_counter;
  }

  /**
   * @return True if child threads will be monitored, false otherwise.
   */
  [[nodiscard]] bool is_include_child_threads() const noexcept { return _is_include_child_threads; }

  /**
   * @return True if kernel-activity will be monitored, false otherwise.
   */
  [[nodiscard]] bool is_include_kernel() const noexcept { return _is_include_kernel; }

  /**
   * @return True if user-activity will be monitored, false otherwise.
   */
  [[nodiscard]] bool is_include_user() const noexcept { return _is_include_user; }

  /**
   * @return True if hypervisor-activity will be monitored, false otherwise.
   */
  [[nodiscard]] bool is_include_hypervisor() const noexcept { return _is_include_hypervisor; }

  /**
   * @return True if idle-activity will be monitored, false otherwise.
   */
  [[nodiscard]] bool is_include_idle() const noexcept { return _is_include_idle; }

  /**
   * @return True if guest-activity will be monitored, false otherwise.
   */
  [[nodiscard]] bool is_include_guest() const noexcept { return _is_include_guest; }

  /**
   * @return True if host-activity will be monitored, false otherwise.
   */
  [[nodiscard]] bool is_include_host() const noexcept { return _is_include_host; }

  /**
   * @return True if pinning enabled, false otherwise.
   */
  [[nodiscard]] bool is_pinned() const noexcept { return _is_pinned; }

  /**
   * @return True if debug mode is enabled, false otherwise.
   */
  [[nodiscard]] bool is_debug() const noexcept { return _is_debug; }

  /**
   * @return CPU core configuration.
   */
  [[nodiscard]] CpuCore cpu_core() const noexcept { return _cpu_core; }

  /**
   * @return Process configuration.
   */
  [[nodiscard]] Process process() const noexcept { return _process; }

  /**
   * Specify the number of maximum physical hardware counters.
   *
   * @param num_physical_counters Number of maximum physical hardware counters.
   */
  void num_physical_counters(const std::uint8_t num_physical_counters) noexcept
  {
    _num_physical_counters = num_physical_counters;
  }

  /**
   * Specify the maximum number of events per physical performance counter.
   *
   * @param num_events_per_physical_counter Number of events per physical performance counter.
   */
  void num_events_per_physical_counter(const std::uint8_t num_events_per_physical_counter) noexcept
  {
    _num_events_per_physical_counter = num_events_per_physical_counter;
  }

  /**
   * If set, child threads from the recording thread will be monitored.
   *
   * @param is_include_child_threads Flag indicating that child threads should be monitored.
   */
  void include_child_threads(const bool is_include_child_threads) noexcept
  {
    _is_include_child_threads = is_include_child_threads;
  }

  /**
   * If set, kernel-activity will be monitored.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/perf-paranoid.md#adjusting-monitoring-configuration
   *
   * @param is_include_kernel Flag indicating that kernel-activity should be monitored.
   */
  void include_kernel(const bool is_include_kernel) noexcept { _is_include_kernel = is_include_kernel; }

  /**
   * If set, user-activity will be monitored.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/perf-paranoid.md#adjusting-monitoring-configuration
   *
   * @param is_include_user Flag indicating that user-activity should be monitored.
   */
  void include_user(const bool is_include_user) noexcept { _is_include_user = is_include_user; }

  /**
   * If set, user-activity will be monitored.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/perf-paranoid.md#adjusting-monitoring-configuration
   *
   * @param is_include_hypervisor Flag indicating that hypervisor-activity should be monitored.
   */
  void include_hypervisor(const bool is_include_hypervisor) noexcept { _is_include_hypervisor = is_include_hypervisor; }

  /**
   * If set, idle-activity will be monitored.
   *
   * @param is_include_idle Flag indicating that idle-activity should be monitored.
   */
  void include_idle(const bool is_include_idle) noexcept { _is_include_idle = is_include_idle; }

  /**
   * If set, guest-activity will be monitored.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/perf-paranoid.md#adjusting-monitoring-configuration
   *
   * @param is_include_guest Flag indicating that guest-activity should be monitored.
   */
  void include_guest(const bool is_include_guest) noexcept { _is_include_guest = is_include_guest; }

  /**
   * If set, host-activity will be monitored.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/perf-paranoid.md#adjusting-monitoring-configuration
   *
   * @param is_include_host Flag indicating that guest-activity should be monitored.
   */
  void include_host(const bool is_include_host) noexcept { _is_include_host = is_include_host; }

  /**
   * If pinned is set to true (false by default), events will be kept on CPU if possible.
   *
   * @param is_pinned If set to true, events will be kept on CPU (if possible).
   */
  void pinned(const bool is_pinned) noexcept { _is_pinned = is_pinned; }

  /**
   * @deprecated Use pinned(bool) instead. Will be removed in v1.0.
   */
  [[deprecated("Use pinned(bool) instead. Will be removed in v1.0.")]] void is_pinned(const bool is_pinned) noexcept
  {
    _is_pinned = is_pinned;
  }

  /**
   * If debug is set to true (false by default), the counter configuration will be dumped to the console upon opening
   * the counter (in both sampling and monitoring mode). This is especially useful when debugging counter
   * configurations.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/recording.md#troubleshooting-counter-configurations
   *
   * @param is_debug If set to true, counter configurations will be dumped to console.
   */
  void debug(const bool is_debug) noexcept { _is_debug = is_debug; }

  /**
   * @deprecated Use debug(bool) instead. Will be removed in v1.0.
   */
  [[deprecated("Use debug(bool) instead. Will be removed in v1.0.")]] void is_debug(const bool is_debug) noexcept
  {
    _is_debug = is_debug;
  }

  /**
   * If specified, the EventCounter or Sampler will monitor only that specified CPU.
   *
   * @param cpu_core CPU core to monitor.
   */
  void cpu_core(const CpuCore cpu_core) noexcept { _cpu_core = cpu_core; }

  /**
   * If specified, the EventCounter or Sampler will monitor only that specified CPU.
   *
   * @param cpu_core_id CPU core to monitor.
   */
  void cpu_core(const std::uint16_t cpu_core_id) noexcept { _cpu_core = CpuCore{ cpu_core_id }; }

  /**
   * If specified, the EventCounter or Sampler will only monitor that specified process.
   *
   * @param process Process to monitor.
   */
  void process(const Process process) noexcept { _process = process; }

  /**
   * If specified, the EventCounter or Sampler will only monitor that specified process.
   *
   * @param process_id Process to monitor.
   */
  void process(const pid_t process_id) noexcept { _process = Process{ process_id }; }

private:
  std::uint8_t _num_physical_counters{ 5U };
  std::uint8_t _num_events_per_physical_counter{ 4U };

  bool _is_include_child_threads{ false };
  bool _is_include_kernel{ true };
  bool _is_include_user{ true };
  bool _is_include_hypervisor{ true };
  bool _is_include_idle{ true };
  bool _is_include_guest{ true };
  bool _is_include_host{ true };

  bool _is_pinned{ false };

  bool _is_debug{ false };

  CpuCore _cpu_core{ CpuCore::Any };
  Process _process{ Process::Calling };
};
}