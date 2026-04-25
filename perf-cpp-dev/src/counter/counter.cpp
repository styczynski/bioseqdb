#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <perfcpp/counter/counter.hpp>
#include <perfcpp/exception.hpp>
#include <perfcpp/feature.h>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
#include <variant>

perf::Counter::~Counter()
{
  /// Close the counter, if not already done.
  this->close();
}

void
perf::Counter::open(const perf::Config& configuration, const bool is_live)
{
  /// Configure the perf event attribute as a normal counter.
  this->_event_attribute = this->create_perf_event_attribute(true, configuration);

  /// Live counter need to set the READ type.
  if (is_live) {
    this->_event_attribute.sample_type = PERF_SAMPLE_READ;
  }

  /// Enable the read format including timing.
  this->_event_attribute.read_format = Counter::create_perf_event_read_format(true, !is_live);
  this->_event_attribute.sample_type |= PERF_SAMPLE_IDENTIFIER;

  /// Open the counter via the perf subsystem.
  auto [file_descriptor, error_code] = this->try_open_via_perf_subsystem(configuration);
  this->_file_descriptor = std::move(file_descriptor);

  /// Read and set the counter's id.
  /// This is done before (possibly) printing the counter to include the counter id into printing.
  if (this->_file_descriptor.has_value()) {
    this->_id = this->read_id();
  }

  /// Print debug output, if requested.
  if (configuration.is_debug()) {
    std::cout << this->to_string(true, this->_file_descriptor, configuration.process(), configuration.cpu_core())
              << std::flush;
  }

  /// Notify the caller that opening the counter via the perf subsystem failed.
  if (!this->_file_descriptor.has_value()) {
    throw CannotOpenCounterError{ error_code };
  }

  /// Live counter use a single buffer page.
  if (is_live) {
    this->_mmap_buffer = std::make_unique<MmapBuffer>(this->_file_descriptor);
  }
}

void
perf::Counter::open(const perf::Config& configuration,
                    const perf::util::UniqueFileDescriptor& group_leader_file_descriptor)
{
  /// Configure the perf event attribute (including read format).
  this->_event_attribute = this->create_perf_event_attribute(false, configuration);
  this->_event_attribute.read_format = Counter::create_perf_event_read_format(false, true);

  /// Open the counter via the perf subsystem.
  auto [file_descriptor, error_code] =
    this->try_open_via_perf_subsystem(configuration, util::FileDescriptorView{ group_leader_file_descriptor });
  this->_file_descriptor = std::move(file_descriptor);

  /// Read and set the counter's id.
  /// This is done before (possibly) printing the counter to include the counter id into printing.
  if (this->_file_descriptor.has_value()) {
    this->_id = this->read_id();
  }

  /// Print debug output, if requested.
  if (configuration.is_debug()) {
    std::cout << this->to_string(false, group_leader_file_descriptor, configuration.process(), configuration.cpu_core())
              << std::flush;
  }

  /// Notify the caller that opening the counter via the perf subsystem failed.
  if (!this->_file_descriptor.has_value()) {
    throw CannotOpenCounterError{ error_code };
  }
}

void
perf::Counter::open(const perf::Config& config,
                    const std::uint64_t buffer_pages,
                    const SampleRecordingValues& sample_recording_values)
{
  /// Configure the perf event attribute for sampling.
  this->_event_attribute = this->create_perf_event_attribute(true, config, sample_recording_values);

  if (sample_recording_values.is_set(SampleRecordingValues::Field::PerformanceCounter)) {
    /// Enable the read format including timing.
    this->_event_attribute.read_format = Counter::create_perf_event_read_format(true, true);
  }

  /// Open the counter via the perf subsystem.
  auto [file_descriptor, error_code] =
    this->try_open_via_perf_subsystem(config, this->_config.precision().value_or(0U));
  this->_file_descriptor = std::move(file_descriptor);

  /// Read and set the counter's id.
  /// This is done before (possibly) printing the counter to include the counter id into printing.
  if (this->_file_descriptor.has_value()) {
    this->_id = this->read_id();
  }

  /// Print debug output, if requested.
  if (config.is_debug()) {
    std::cout << this->to_string(true, this->_file_descriptor, config.process(), config.cpu_core()) << std::flush;
  }

  /// Notify the caller that opening the counter via the perf subsystem failed.
  if (!this->_file_descriptor.has_value()) {
    throw CannotOpenCounterError{ error_code };
  }

  /// Create sample buffer to store samples.
  if (buffer_pages > 0ULL) {
    this->_mmap_buffer = std::make_unique<MmapBuffer>(this->_file_descriptor, buffer_pages);
  }
}

void
perf::Counter::open(const perf::Config& config,
                    const std::uint64_t buffer_pages,
                    const SampleRecordingValues& sample_recording_values,
                    const perf::util::UniqueFileDescriptor& group_leader_file_descriptor)
{
  /// Configure the perf event attribute for sampling.
  this->_event_attribute = this->create_perf_event_attribute(false, config, sample_recording_values);

  if (sample_recording_values.is_set(SampleRecordingValues::Field::PerformanceCounter)) {
    /// Enable the read format including timing.
    this->_event_attribute.read_format = Counter::create_perf_event_read_format(false, true);
  }

  /// Open the counter via the perf subsystem.
  auto [file_descriptor, error_code] = this->try_open_via_perf_subsystem(
    config, this->_config.precision().value_or(0U), util::FileDescriptorView{ group_leader_file_descriptor });
  this->_file_descriptor = std::move(file_descriptor);

  /// Read and set the counter's id.
  /// This is done before (possibly) printing the counter to include the counter id into printing.
  if (this->_file_descriptor.has_value()) {
    this->_id = this->read_id();
  }

  /// Print debug output, if requested.
  if (config.is_debug()) {
    std::cout << this->to_string(false, group_leader_file_descriptor, config.process(), config.cpu_core())
              << std::flush;
  }

  /// Notify the caller that opening the counter via the perf subsystem failed.
  if (!this->_file_descriptor.has_value()) {
    throw CannotOpenCounterError{ error_code };
  }

  /// Create sample buffer to store samples.
  if (buffer_pages > 0ULL) {
    this->_mmap_buffer = std::make_unique<MmapBuffer>(this->_file_descriptor, buffer_pages);
  }
}

void
perf::Counter::close()
{
  /// Close/un-map the mmap-ed buffer, if any.
  if (this->_mmap_buffer != nullptr) {
    this->_mmap_buffer.reset();
  }

  this->_file_descriptor.reset();
}

void
perf::Counter::enable() const
{
  if (::ioctl(this->_file_descriptor.value(), PERF_EVENT_IOC_RESET, 0) < 0) {
    throw CannotEnableCounter{ errno };
  }

  if (::ioctl(this->_file_descriptor.value(), PERF_EVENT_IOC_ENABLE, 0) < 0) {
    throw CannotEnableCounter{ errno };
  }
}

void
perf::Counter::disable() const
{
  if (::ioctl(this->_file_descriptor.value(), PERF_EVENT_IOC_DISABLE, 0) < 0) {
    throw CannotDisableCounter{ errno };
  }
}

std::optional<double>
perf::Counter::read_live() const
{
  if (this->_mmap_buffer != nullptr) {
    /// Read the value from mmap-ed buffer (via rdpmc instruction).
    if (const auto value = this->_mmap_buffer->read_performance_monitoring_counter(); value.has_value()) {
      /// Adjust the value via scale if the value is available.
      return static_cast<double>(value.value()) * this->scale();
    }
  }

  /// If there is no mmap-ed buffer or the value cannot be read, return nullopt.
  return std::nullopt;
}

std::uint64_t
perf::Counter::read_id() const
{
  auto id = std::uint64_t{};
  if (::ioctl(this->_file_descriptor.value(), PERF_EVENT_IOC_ID, &id) < 0) {
    throw CannotReadCounterId{ errno };
  }

  return id;
}

perf_event_attr
perf::Counter::create_perf_event_attribute(const bool is_disabled, const Config& configuration) const noexcept
{
  auto attribute = perf_event_attr{};

  /// Set all attributes to zero.
  std::memset(&attribute, 0, sizeof(perf_event_attr));

  /// Set all requested attributes.
  attribute.type = this->_config.type();
  attribute.size = sizeof(perf_event_attr);
  attribute.config = this->_config.configs()[0U];
  attribute.config1 = this->_config.configs()[1U];
  attribute.config2 = this->_config.configs()[2U];
  attribute.disabled = is_disabled;
  /// Fixed-function PMC events are pinned to their dedicated counter; generic events use the config flag.
  attribute.pinned = configuration.is_pinned() || (is_disabled && this->_config.is_fixed());

  attribute.inherit = configuration.is_include_child_threads();
  attribute.exclude_kernel = !configuration.is_include_kernel();
  attribute.exclude_user = !configuration.is_include_user();
  attribute.exclude_hv = !configuration.is_include_hypervisor();
  attribute.exclude_idle = !configuration.is_include_idle();
  attribute.exclude_guest = !configuration.is_include_guest();
  attribute.exclude_host = !configuration.is_include_host();

  return attribute;
}

perf_event_attr
perf::Counter::create_perf_event_attribute(const bool is_disabled,
                                           const Config& configuration,
                                           const SampleRecordingValues& sample_recording_values) const
{
  auto attribute = this->create_perf_event_attribute(is_disabled, configuration);

  /// Set the sample type for the group leader (or the counter after the auxiliary-event).
  attribute.sample_type = sample_recording_values.to_perf_sample_type();

  /// Sampling is not only indicated by the sample_type since "live" events (read without stopping the counter) also
  /// have a sample type but are not truly sampling. We assume that true sampling is only requested when
  /// period/frequency and precision is set since both are needed for sampling but not for reading counter without
  /// stopping.
  if (const auto period_or_frequency = this->_config.period_or_frequency();
      period_or_frequency.has_value() && this->_config.precision().has_value()) {
    attribute.sample_id_all = 1U;

    /// Set period of frequency, based on the PeriodOrFrequency variant.
    std::visit(PeriodOrFrequencyVisitor{ attribute }, period_or_frequency.value());

    /// Set sampled fields.
    attribute.branch_sample_type = sample_recording_values.branch_mask();
#ifndef PERFCPP_NO_SAMPLE_MAX_STACK /// Max sample stack is only supported since Linux 4.8
    attribute.sample_max_stack = sample_recording_values.max_call_stack();
#endif
    attribute.sample_regs_user = sample_recording_values.user_registers().mask();
    attribute.sample_regs_intr = sample_recording_values.kernel_registers().mask();
    attribute.sample_stack_user = sample_recording_values.max_user_stack();
#ifndef PERFCPP_NO_RECORD_SWITCH /// Record switch is supported since Linux 4.3.
    attribute.context_switch = sample_recording_values.is_set(SampleRecordingValues::Field::ContextSwitch);
#endif
#ifndef PERFCPP_NO_RECORD_CGROUP /// Recording cgroup is supported since Linux 5.7.
    attribute.cgroup = sample_recording_values.is_set(SampleRecordingValues::Field::CGroup);
#endif

    if (sample_recording_values.is_set(SampleRecordingValues::Field::MMapInformation)) {
      attribute.mmap = true;
      attribute.mmap2 = true;
    }
  }

  return attribute;
}

std::uint64_t
perf::Counter::create_perf_event_read_format(const bool is_include_time, const bool is_include_group) noexcept
{
  return (static_cast<std::uint64_t>(is_include_group) * PERF_FORMAT_GROUP) | PERF_FORMAT_ID |
         (static_cast<std::uint64_t>(is_include_time) *
          (PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING));
}

std::pair<perf::util::UniqueFileDescriptor, std::int32_t>
perf::Counter::try_open_via_perf_subsystem(const Config& configuration,
                                           const util::FileDescriptorView group_leader_file_descriptor)
{
  /// Finally, pass the configuration to the perf subsystem to open the hardware performance counter.
  const auto file_descriptor = ::syscall(__NR_perf_event_open,
                                         &this->_event_attribute,
                                         static_cast<pid_t>(configuration.process()),
                                         static_cast<std::int32_t>(configuration.cpu_core()),
                                         group_leader_file_descriptor.value(),
                                         0);

  const auto error_code = errno;
  return std::make_pair(util::UniqueFileDescriptor{ file_descriptor }, error_code);
}

std::pair<perf::util::UniqueFileDescriptor, std::int32_t>
perf::Counter::try_open_via_perf_subsystem(const perf::Config& configuration,
                                           std::uint8_t precision,
                                           const perf::util::FileDescriptorView group_leader_file_descriptor)
{
  auto file_descriptor = util::UniqueFileDescriptor{};
  auto error_code = std::int32_t{};

  /// Try to open the counter. For sampling, we might try to adjust the precision configuration (see
  /// Counter::is_precise_ip_adjustable).
  do {
    /// precision is only needed for sampling, not counting events and live events; thus, only set when it has a value.
    this->_event_attribute.precise_ip =
      precision & 0b11; /// Use only two bits as perf_event_attr.precision has only two bits.

    /// Try to open using the perf subsystem. This might fail. If precision is the reason (derived by the error code),
    /// we try to adjust the precision and try again (see Counter::is_precision_adjustable).
    std::tie(file_descriptor, error_code) =
      this->try_open_via_perf_subsystem(configuration, group_leader_file_descriptor);

    /// Repeat until success (file_descriptor has a "valid" value or trying again is hopeless.
  } while (!file_descriptor.has_value() && Counter::is_precision_adjustable(precision--, error_code));

  return std::make_pair(std::move(file_descriptor), error_code);
}

bool
perf::Counter::is_precision_adjustable(const std::uint8_t current_precise_ip, const std::int32_t error_code) noexcept
{
  /// When precision is already the lowest possible configuration (0 or lower), lowering has no impact.
  if (current_precise_ip < 1U || current_precise_ip > 3U) {
    return false;
  }

  /// EINVAL indicates an invalid argument, which could be a too high value for precision.
  /// Likewise, EOPNOTSUPP could indicate that such a high value of precision is not supported on the underlying
  /// machine. In both scenarios, we should decrease the value and try again.
  return error_code == EINVAL || error_code == EOPNOTSUPP;
}

std::string
perf::Counter::to_string(const bool is_group_leader,
                         const util::UniqueFileDescriptor& group_leader_file_descriptor,
                         const Process process,
                         const CpuCore cpu_core) const
{
  auto stream = std::stringstream{};

  stream << "Counter:\n"
         << "    id: " << this->_id << "\n"
         << "    file_descriptor: " << this->_file_descriptor.value() << "\n";

  /// Role (leader or member).
  if (is_group_leader) {
    stream << "    role: group leader\n";
  } else {
    stream << "    role: group member\n";
    if (group_leader_file_descriptor.has_value()) {
      stream << "    leader's file_descriptor: " << group_leader_file_descriptor.value() << "\n";
    }
  }

  /// Process
  stream << "    process: ";
  if (process.is_any()) {
    stream << "any (-1)\n";
  } else if (process.is_calling()) {
    stream << "calling (0)\n";
  } else {
    stream << static_cast<pid_t>(process) << "\n";
  }

  /// CPU
  stream << "    cpu: ";
  if (cpu_core.is_any()) {
    stream << "any (-1)\n";
  } else {
    stream << static_cast<std::int32_t>(cpu_core) << "\n";
  }

  /// Perf Event
  stream << "    perf_event_attr:\n"
         << "        type: " << this->_event_attribute.type << "\n"
         << "        size: " << this->_event_attribute.size << "\n"
         << "        config: 0x" << std::hex << this->_event_attribute.config << std::dec << "\n";

  /// Sample type
  if (this->_event_attribute.sample_type > 0U) {
    stream << "        sample_type: ";

    Counter::print_type_to_stream(stream,
                                  this->_event_attribute.sample_type,
                                  { { PERF_SAMPLE_IP, "IP" },
                                    { PERF_SAMPLE_TID, "TID" },
                                    { PERF_SAMPLE_TIME, "TIME" },
                                    { PERF_SAMPLE_ADDR, "ADDR" },
                                    { PERF_SAMPLE_READ, "READ" },
                                    { PERF_SAMPLE_CALLCHAIN, "CALLCHAIN" },
                                    { PERF_SAMPLE_CPU, "CPU" },
                                    { PERF_SAMPLE_PERIOD, "PERIOD" },
                                    { PERF_SAMPLE_STREAM_ID, "STREAM_ID" },
                                    { PERF_SAMPLE_RAW, "RAW" },
                                    { PERF_SAMPLE_BRANCH_STACK, "BRANCH_STACK" },
                                    { PERF_SAMPLE_REGS_USER, "REGS_USER" },
                                    { PERF_SAMPLE_STACK_USER, "STACK_USER" },
                                    { PERF_SAMPLE_WEIGHT, "WEIGHT" },
                                    { PERF_SAMPLE_DATA_SRC, "DATA_SRC" },
                                    { PERF_SAMPLE_IDENTIFIER, "IDENTIFIER" },
                                    { PERF_SAMPLE_REGS_INTR, "REGS_INTR" }
#ifndef PERFCPP_NO_SAMPLE_PHYS_ADDR /// Sampling for physical address is supported since Linux 4.13
                                    ,
                                    { PERF_SAMPLE_PHYS_ADDR, "PHYS_ADDR" }
#endif
#ifndef PERFCPP_NO_SAMPLE_CGROUP /// Sampling for cgroup is supported since Linux 5.7
                                    ,
                                    { PERF_SAMPLE_CGROUP, "CGROUP" }
#endif

#ifndef PERFCPP_NO_SAMPLE_DATA_PAGE_SIZE /// Sampling for data page size is supported since Linux 5.11
                                    ,
                                    { PERF_SAMPLE_DATA_PAGE_SIZE, "DATA_PAGE_SIZE" }
#endif

#ifndef PERFCPP_NO_SAMPLE_CODE_PAGE_SIZE /// Sampling for code page size is supported since Linux 5.11
                                    ,
                                    { PERF_SAMPLE_CODE_PAGE_SIZE, "PAGE_SIZE" }
#endif

#ifndef PERFCPP_NO_SAMPLE_WEIGHT_STRUCT /// Sampling for weight struct is supported since Linux 5.12
                                    ,
                                    { PERF_SAMPLE_WEIGHT_STRUCT, "WEIGHT_STRUCT" }
#endif
                                  });
  }

  /// Frequency or Period
  if (this->_event_attribute.freq > 0U && this->_event_attribute.sample_freq > 0U) {
    stream << "        sample_freq: " << this->_event_attribute.sample_freq << "\n";
  } else if (this->_event_attribute.sample_period > 0U) {
    stream << "        sample_period: " << this->_event_attribute.sample_period << "\n";
  }

  if (this->_event_attribute.precise_ip > 0U) {
    stream << "        precision: " << this->_event_attribute.precise_ip << "\n";
  }

  if (this->_event_attribute.mmap > 0U) {
    stream << "        mmap: " << this->_event_attribute.mmap << "\n";
  }

  if (this->_event_attribute.sample_id_all > 0U) {
    stream << "        sample_id_all: " << this->_event_attribute.sample_id_all << "\n";
  }

  if (this->_event_attribute.mmap2) {
    stream << "        mmap2: " << this->_event_attribute.mmap2 << "\n";
  }

  if (this->_event_attribute.comm) {
    stream << "        comm: " << this->_event_attribute.comm << "\n";
  }

  if (this->_event_attribute.comm_exec) {
    stream << "        comm_exec: " << this->_event_attribute.comm_exec << "\n";
  }

  if (this->_event_attribute.task) {
    stream << "        task: " << this->_event_attribute.task << "\n";
  }

  if (this->_event_attribute.enable_on_exec) {
    stream << "        enable_on_exec: " << this->_event_attribute.enable_on_exec << "\n";
  }

  /// Read format
  if (this->_event_attribute.read_format > 0U) {
    stream << "        read_format: ";
    Counter::print_type_to_stream(stream,
                                  this->_event_attribute.read_format,
                                  { { PERF_FORMAT_TOTAL_TIME_ENABLED, "TOTAL_TIME_ENABLED" },
                                    { PERF_FORMAT_TOTAL_TIME_RUNNING, "TOTAL_TIME_RUNNING" },
                                    { PERF_FORMAT_ID, "ID" },
                                    { PERF_FORMAT_GROUP, "GROUP" }
#ifndef PERFCPP_NO_FORMAT_LOST /// Reading lost values is supported since Linux 6.0
                                    ,
                                    { PERF_FORMAT_LOST, "LOST" }
#endif
                                  });
  }

  /// Branch type
  if (this->_event_attribute.branch_sample_type > 0U) {
    stream << "        branch_sample_type: ";
    Counter::print_type_to_stream(stream,
                                  this->_event_attribute.branch_sample_type,
                                  { { PERF_SAMPLE_BRANCH_USER, "BRANCH_USER" },
                                    { PERF_SAMPLE_BRANCH_KERNEL, "BRANCH_KERNEL" },
                                    { PERF_SAMPLE_BRANCH_HV, "BRANCH_HV" },
                                    { PERF_SAMPLE_BRANCH_ANY, "BRANCH_ANY" },
                                    { PERF_SAMPLE_BRANCH_ANY_CALL, "BRANCH_ANY_CALL" },
#ifndef PERFCPP_NO_SAMPLE_BRANCH_CALL /// Branch type "call" is supported since Linux 4.4
                                    { PERF_SAMPLE_BRANCH_CALL, "BRANCH_CALL" },
#endif
                                    { PERF_SAMPLE_BRANCH_IND_CALL, "BRANCH_IND_CALL" },
                                    { PERF_SAMPLE_BRANCH_ANY_RETURN, "BRANCH_ANY_RETURN" },
#ifndef PERFCPP_NO_SAMPLE_BRANCH_IND_JUMP /// Branch type "indirect jump" is supported since Linux 4.2
                                    { PERF_SAMPLE_BRANCH_IND_JUMP, "BRANCH_IND_JUMP" },
#endif
                                    { PERF_SAMPLE_BRANCH_ABORT_TX, "BRANCH_ABORT_TX" },
                                    { PERF_SAMPLE_BRANCH_IN_TX, "BRANCH_IN_TX" },
                                    { PERF_SAMPLE_BRANCH_NO_TX, "BRANCH_NO_TX" } });
  }

#ifndef PERFCPP_NO_SAMPLE_MAX_STACK /// Max sample stack is supported since Linux 4.8
  if (this->_event_attribute.sample_max_stack > 0U) {
    stream << "        sample_max_stack: " << this->_event_attribute.sample_max_stack << "\n";
  }
#endif

  if (this->_event_attribute.sample_regs_user > 0U) {
    stream << "        sample_regs_user: " << this->_event_attribute.sample_regs_user << "\n";
  }

  if (this->_event_attribute.sample_regs_intr > 0U) {
    stream << "        sample_regs_intr: " << this->_event_attribute.sample_regs_intr << "\n";
  }

  if (this->_event_attribute.config1 > 0U) {
    stream << "        config1: 0x" << std::hex << this->_event_attribute.config1 << std::dec << "\n";
  }
  if (this->_event_attribute.config2 > 0U) {
    stream << "        config2: 0x" << std::hex << this->_event_attribute.config2 << std::dec << "\n";
  }
  if (this->_event_attribute.disabled > 0U) {
    stream << "        disabled: " << this->_event_attribute.disabled << "\n";
  }
  if (this->_event_attribute.pinned > 0U) {
    stream << "        pinned: " << this->_event_attribute.pinned << "\n";
  }
  if (this->_event_attribute.inherit > 0U) {
    stream << "        inherit: " << this->_event_attribute.inherit << "\n";
  }
  if (this->_event_attribute.exclude_kernel > 0U) {
    stream << "        exclude_kernel: " << this->_event_attribute.exclude_kernel << "\n";
  }
  if (this->_event_attribute.exclude_user > 0U) {
    stream << "        exclude_user: " << this->_event_attribute.exclude_user << "\n";
  }
  if (this->_event_attribute.exclude_hv > 0U) {
    stream << "        exclude_hv: " << this->_event_attribute.exclude_hv << "\n";
  }
  if (this->_event_attribute.exclude_idle > 0U) {
    stream << "        exclude_idle: " << this->_event_attribute.exclude_idle << "\n";
  }
  if (this->_event_attribute.exclude_guest > 0U) {
    stream << "        exclude_guest: " << this->_event_attribute.exclude_guest << "\n";
  }
  if (this->_event_attribute.exclude_host > 0U) {
    stream << "        exclude_host: " << this->_event_attribute.exclude_host << "\n";
  }
#ifndef PERFCPP_NO_RECORD_SWITCH /// Context switch is supported since Linux 4.3
  if (this->_event_attribute.context_switch > 0U) {
    stream << "        context_switch: " << this->_event_attribute.context_switch << "\n";
  }
#endif
#ifndef PERFCPP_NO_RECORD_CGROUP /// cgroup is supported since Linux 5.7
  if (this->_event_attribute.cgroup > 0U) {
    stream << "        cgroup: " << this->_event_attribute.cgroup << "\n";
  }
#endif

  return stream.str();
}

void
perf::Counter::print_type_to_stream(std::stringstream& stream,
                                    const std::uint64_t mask,
                                    std::initializer_list<std::pair<std::uint64_t, std::string_view>>&& types)
{
  auto is_first = true;

  for (const auto& [type, name] : types) {
    if (static_cast<bool>(mask & type)) {
      if (!std::exchange(is_first, false)) {
        stream << " | ";
      }

      stream << name;
    }
  }

  stream << "\n";
}