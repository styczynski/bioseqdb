#pragma once

#include <array>
#include <cstdint>
#include <linux/perf_event.h>
#include <memory>
#include <optional>
#include <perfcpp/counter/config.hpp>
#include <perfcpp/counter/result.hpp>
#include <perfcpp/sample/mmap_buffer.hpp>
#include <perfcpp/sample/period.hpp>
#include <perfcpp/sample/precision.hpp>
#include <perfcpp/sample/recording_values.hpp>
#include <perfcpp/util/unique_file_descriptor.hpp>
#include <string>
#include <vector>

namespace perf {
class CounterConfig
{
public:
  CounterConfig(const std::uint32_t type,
                const std::uint64_t id,
                const std::uint64_t id_extension_1 = 0UL,
                const std::uint64_t id_extension_2 = 0UL,
                const bool is_fixed = false) noexcept
    : _type(type)
    , _configs({ id, id_extension_1, id_extension_2 })
    , _is_fixed(is_fixed)
  {
  }

  CounterConfig(const CounterConfig&) noexcept = default;
  CounterConfig(CounterConfig&&) noexcept = default;

  ~CounterConfig() noexcept = default;

  CounterConfig& operator=(const CounterConfig&) noexcept = default;
  CounterConfig& operator=(CounterConfig&&) noexcept = default;

  /**
   * Mark this event as using a fixed-function performance counter.
   * Fixed counters are dedicated to specific events and do not participate in generic PMC scheduling.
   *
   * @param is_fixed True if the event uses a fixed PMC.
   */
  void fixed(const bool is_fixed) noexcept { _is_fixed = is_fixed; }

  /**
   * Set the scale for calculating the event result.
   * @param scale Scale of the event.
   */
  void scale(const double scale) noexcept { _scale = scale; }

  /**
   * Set the precision if the event is used for sampling.
   * @param precision Precision.
   */
  void precision(const std::uint8_t precision) noexcept { _precision = precision; }

  /**
   * Set the precision if the event is used for sampling.
   * @param precision Precision.
   */
  void precision(const Precision precision) noexcept { _precision = static_cast<std::uint8_t>(precision); }

  /**
   * Set the period or frequency if the event is used for sampling.
   * @param period_or_frequency Period or frequency.
   */
  void period_or_frequency(const PeriodOrFrequency period_or_frequency) noexcept
  {
    _period_or_frequency = period_or_frequency;
  }

  /**
   * @return Type of the event, mostly referring to the PMU.
   */
  [[nodiscard]] std::uint32_t type() const noexcept { return _type; }

  /**
   * @return Configurations of the event.
   */
  [[nodiscard]] std::array<std::uint64_t, 3U> configs() const noexcept { return _configs; }

  /**
   * @return Scale of the event.
   */
  [[nodiscard]] double scale() const noexcept { return _scale; }

  /**
   * @return Precision, if the event is used for sampling.
   */
  [[nodiscard]] std::optional<std::uint8_t> precision() const noexcept { return _precision; }

  /**
   * @return Period or frequency, if the event is used for sampling.
   */
  [[nodiscard]] std::optional<PeriodOrFrequency> period_or_frequency() const noexcept { return _period_or_frequency; }

  /**
   * @return True if the event uses a fixed-function performance counter.
   */
  [[nodiscard]] bool is_fixed() const noexcept { return _is_fixed; }

  [[nodiscard]] bool operator==(const CounterConfig& other) const noexcept
  {
    return _type == other._type && _configs[0U] == other._configs[0U];
  }

private:
  /// Type of the event, mostly referring to the PMU.
  std::uint32_t _type;

  /// Configuration ids of the event.
  std::array<std::uint64_t, 3U> _configs;

  /// Scale of the event.
  double _scale{ 1.0 };

  /// Precision, if the event is used for sampling.
  std::optional<std::uint8_t> _precision{ std::nullopt };

  /// Period of frequency, if the event is used for sampling.
  std::optional<PeriodOrFrequency> _period_or_frequency{ std::nullopt };

  /// True if the event uses a fixed-function performance counter (e.g., instructions, cycles on Intel).
  bool _is_fixed{ false };
};

class Counter
{
public:
  /**
   * Copies a counter only by the configuration, not any state like file descriptor, event attribute, etc.
   *
   * @param other Counter to copy from.
   * @return A copy with the same configuration.
   */
  [[nodiscard]] static Counter copy_from_template(const Counter& other) noexcept { return Counter(other._config); }

  explicit Counter(CounterConfig config) noexcept
    : _config(config)
  {
  }

  Counter(Counter&&) noexcept = default;
  Counter(const Counter&) = delete;

  ~Counter();

  [[nodiscard]] Counter& operator=(Counter&&) noexcept = default;
  Counter& operator=(const Counter&) = delete;

  /**
   * @return ID of the counter.
   */
  [[nodiscard]] std::uint64_t id() const noexcept { return _id; }

  /**
   * @return The file descriptor of the counter; -1 if the counter was not opened (successfully).
   */
  [[nodiscard]] const util::UniqueFileDescriptor& file_descriptor() const noexcept { return _file_descriptor; }

  /**
   * Opens the counter via the perf subsystem.
   * The counter will be configured with the provided parameters.
   * After successfully open the counter, the counter's file descriptor will be set.
   * If the counter cannot be opened, it will throw an exception including the error number.
   *
   * @param configuration Configuration of the counter.
   * @param is_live True, if the counter can be read live.
   */
  void open(const Config& configuration, bool is_live);

  /**
   * Opens the counter via the perf subsystem.
   * The counter will be configured with the provided parameters.
   * After successfully open the counter, the counter's file descriptor will be set.
   * If the counter cannot be opened, it will throw an exception including the error number.
   *
   * @param configuration Configuration of the counter.
   * @param group_leader_file_descriptor File descriptor of the group leader.
   */
  void open(const Config& configuration, const util::UniqueFileDescriptor& group_leader_file_descriptor);

  /**
   * Opens the counter using via the perf subsystem.
   * The counter will be configured with the provided parameters.
   * After successfully open the counter, the counter's file descriptor will be set.
   * If the counter cannot be opened, it will throw an exception including the error number.
   *
   * @param config Configuration.
   * @param buffer_pages Number of pages allocated for user-level buffer.
   * @param sample_recording_values Values to record while sampling.
   */
  void open(const perf::Config& config,
            std::uint64_t buffer_pages,
            const SampleRecordingValues& sample_recording_values);

  /**
   * Opens the counter using via the perf subsystem.
   * The counter will be configured with the provided parameters.
   * After successfully open the counter, the counter's file descriptor will be set.
   * If the counter cannot be opened, it will throw an exception including the error number.
   *
   * @param config Configuration.
   * @param buffer_pages Number of pages allocated for user-level buffer.
   * @param sample_recording_values Values to record while sampling.
   * @param group_leader_file_descriptor File descriptor of the group leader.
   */
  void open(const perf::Config& config,
            std::uint64_t buffer_pages,
            const SampleRecordingValues& sample_recording_values,
            const util::UniqueFileDescriptor& group_leader_file_descriptor);

  /**
   * Closes the counter and resets the file descriptor.
   */
  void close();

  /**
   * Enables the counter.
   */
  void enable() const;

  /**
   * Disables the counter.
   */
  void disable() const;

  /**
   * Reads the counter "live" without stopping via the "rdpmc" instruction.
   * Note that this is only possible on x86 architectures.
   *
   * @return The current value of the counter.
   */
  [[nodiscard]] std::optional<double> read_live() const;

  /**
   * @return The sample buffer that manages the mmap-ed buffer for storing samples and/or live events.
   */
  [[nodiscard]] const std::unique_ptr<MmapBuffer>& mmap_buffer() const noexcept { return _mmap_buffer; }

  /**
   * @return Scale of the event, provided by the event configuration.
   */
  [[nodiscard]] double scale() const noexcept { return _config.scale(); }

  /**
   * @return The event attribute of the perf subsystem.
   */
  [[nodiscard]] const perf_event_attr& perf_event_attribute() const noexcept { return _event_attribute; }

  /**
   * Prints the configuration of the counter, borrowing the format of Linux perf.
   *
   * @param is_group_leader Flag, if the counter is the leader of the group.
   * @param group_leader_file_descriptor File descriptor of the group leader.
   * @param process Process the counter is tied to.
   * @param cpu_core CPU core the counter is tied to.
   * @return A string representing all configurations of this counter.
   */
  [[nodiscard]] std::string to_string(bool is_group_leader,
                                      const util::UniqueFileDescriptor& group_leader_file_descriptor,
                                      Process process,
                                      CpuCore cpu_core) const;

  [[nodiscard]] bool operator==(const CounterConfig& config) const noexcept { return _config == config; }

private:
  /// The config of an event.
  CounterConfig _config;

  /// The event description for the perf subsystem, will be configured when opening.
  perf_event_attr _event_attribute{};

  /// Id of an event, will be set by ::ioctl and is needed to match counter values from the perf reading format.
  std::uint64_t _id{ 0U };

  /// The file descriptor as returned by the perf subsystem when opening the counter.
  util::UniqueFileDescriptor _file_descriptor;

  /// Buffer used to store samples. The buffer mmaps a ringbuffer and handles overflows via a separate thread.
  /// Additionally, the MmapBuffer can read live events.
  std::unique_ptr<MmapBuffer> _mmap_buffer{ nullptr };

  /**
   * Creates a perf event of the counter.
   *
   * @param is_disabled True, if the counter is disabled (mostly true for the events but the first).
   * @param configuration Configuration to configure.
   * @return The initialized perf_event_attr.
   */
  [[nodiscard]] perf_event_attr create_perf_event_attribute(bool is_disabled,
                                                            const Config& configuration) const noexcept;

  /**
   * Creates a perf event of the counter for sampling.
   *
   * @param is_disabled  True, if the counter is disabled (mostly true for the events but the first).
   * @param configuration Configuration to configure.
   * @param sample_recording_values Values to record while sampling.
   * @return The initialized perf_event_attr.
   */
  [[nodiscard]] perf_event_attr create_perf_event_attribute(bool is_disabled,
                                                            const Config& configuration,
                                                            const SampleRecordingValues& sample_recording_values) const;

  /**
   * Configures the perf event read format.
   *
   * @param is_include_time If true, time is included.
   * @param is_include_group If true, will enable group reading to read multiple events from one physical hardware
   * counter.
   */
  [[nodiscard]] static std::uint64_t create_perf_event_read_format(bool is_include_time,
                                                                   bool is_include_group) noexcept;

  /**
   * Reads the counter's id.
   *
   * @return Id of the counter's file descriptor.
   */
  [[nodiscard]] std::uint64_t read_id() const;

  /**
   * Do the "final" perf_event_open system call with the provided parameters.
   *
   * @param configuration Configuration (including process id and CPU core id; the rest is ignored).
   * @param group_leader_file_descriptor View to the group leader's file descriptor.
   * @return File descriptor and error code, which is valid when the file descriptor has no value.
   */
  [[nodiscard]] std::pair<util::UniqueFileDescriptor, std::int32_t> try_open_via_perf_subsystem(
    const perf::Config& configuration,
    util::FileDescriptorView group_leader_file_descriptor = util::FileDescriptorView{});

  /**
   * Opens the perf subsystem event for sampling with the given configuration.
   * When the perf subsystem reports an error that refers to the precision, the precision will be lowered until success
   * or reaching zero but the opening call still fails.
   *
   * @param configuration Configuration for the perf subsystem, including CPU core id and process id.
   * @param precision Precision for sampling.
   * @param group_leader_file_descriptor View to the group leader's file descriptor.
   * @return File descriptor and error code, which is valid when the file descriptor has no value.
   */
  std::pair<util::UniqueFileDescriptor, std::int32_t> try_open_via_perf_subsystem(
    const Config& configuration,
    std::uint8_t precision,
    util::FileDescriptorView group_leader_file_descriptor = util::FileDescriptorView{});

  /**
   * Decides whether adjusting (i.e., decrementing) the precision configuration could help to open a hardware
   * performance counter if a previous attempt failed. This is only true for sampling, if the current precision is too
   * high and the error code indicates to do so (e.g., reporting an invalid argument).
   *
   * @param current_precise_ip The current value of the precision configuration.
   * @param error_code The error code when failing.
   * @return True, when the counter should try to open again.
   */
  [[nodiscard]] static bool is_precision_adjustable(std::uint8_t current_precise_ip, std::int32_t error_code) noexcept;

  /**
   * Prints a name of a type (e.g., sample, branch, ...) to the stream if the type is set in the mask.
   *
   * @param stream Stream to print the name of the type on.
   * @param mask Given mask to check if the type is set.
   * @param types List of (type,name) tuples to test in the mask.
   */
  static void print_type_to_stream(std::stringstream& stream,
                                   std::uint64_t mask,
                                   std::initializer_list<std::pair<std::uint64_t, std::string_view>>&& types);

  /**
   * Visitor for translating the period or frequency into the perf event attribute.
   */
  class PeriodOrFrequencyVisitor
  {
  public:
    explicit PeriodOrFrequencyVisitor(perf_event_attr& attribute) noexcept
      : _attribute(attribute)
    {
    }
    void operator()(const Period period) noexcept { _attribute.sample_period = period.get(); }
    void operator()(const Frequency frequency) noexcept
    {
      _attribute.freq = true;
      _attribute.sample_period = frequency.get();
    }

  private:
    perf_event_attr& _attribute;
  };
};
}