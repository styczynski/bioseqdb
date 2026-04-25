#include <perfcpp/exception.hpp>
#include <perfcpp/hardware_info.h>
#include <perfcpp/sample/trigger.hpp>

std::vector<std::tuple<std::string_view, std::string_view, perf::CounterConfig>>
perf::Cycles::resolve(const CounterDefinition& counter_definition) const
{
  const auto counter_configs = counter_definition.counter(std::string_view{ "cycles" });
  if (counter_configs.empty()) {
    throw CannotFindEventError{ std::string_view{ "cycles" } };
  }
  return counter_configs;
}

std::optional<std::tuple<std::string_view, std::string_view, perf::CounterConfig>>
perf::Cycles::resolve(const CounterDefinition& counter_definition, const std::string_view pmu_name) const
{
  return counter_definition.counter(pmu_name, std::string_view{ "cycles" });
}

std::vector<std::tuple<std::string_view, std::string_view, perf::CounterConfig>>
perf::MemoryLoads::resolve(const CounterDefinition& counter_definition) const
{
  if (!HardwareInfo::is_intel()) {
    throw EventRequiresSpecificVendorError{ "Intel", "mem-loads" };
  }

  const auto counter_configs = counter_definition.counter(std::string_view{ "mem-loads" });
  if (counter_configs.empty()) {
    throw CannotFindEventError{ std::string_view{ "mem-loads" } };
  }

  /// Build one entry per PMU variant, patching config1 with the user-specified minimum latency
  /// (perf_event_attr.config1 is where the kernel reads the ldlat filter).
  auto result = std::vector<std::tuple<std::string_view, std::string_view, CounterConfig>>{};
  result.reserve(counter_configs.size());
  for (const auto& [pmu_name, event_name, base] : counter_configs) {
    result.emplace_back(
      pmu_name,
      event_name,
      CounterConfig{ base.type(), base.configs()[0U], this->_min_latency, base.configs()[2U], base.is_fixed() });
  }
  return result;
}

std::optional<std::tuple<std::string_view, std::string_view, perf::CounterConfig>>
perf::MemoryLoads::resolve(const CounterDefinition& counter_definition, const std::string_view pmu_name) const
{
  if (!HardwareInfo::is_intel()) {
    throw EventRequiresSpecificVendorError{ "Intel", "mem-loads" };
  }

  auto event = counter_definition.counter(pmu_name, std::string_view{ "mem-loads" });
  if (!event.has_value()) {
    return std::nullopt;
  }

  /// Patch config1 with the user-specified minimum latency (perf_event_attr.config1 = ldlat).
  const auto& base = std::get<2>(event.value());
  return std::tuple{ std::get<0>(event.value()),
                     std::get<1>(event.value()),
                     CounterConfig{
                       base.type(), base.configs()[0U], this->_min_latency, base.configs()[2U], base.is_fixed() } };
}

std::vector<std::tuple<std::string_view, std::string_view, perf::CounterConfig>>
perf::MemoryStores::resolve(const CounterDefinition& counter_definition) const
{
  if (!HardwareInfo::is_intel()) {
    throw EventRequiresSpecificVendorError{ "Intel", "mem-stores" };
  }

  const auto counter_configs = counter_definition.counter(std::string_view{ "mem-stores" });
  if (counter_configs.empty()) {
    throw CannotFindEventError{ std::string_view{ "mem-stores" } };
  }

  /// Return all PMU variants unchanged; mem-stores needs no config patching.
  return counter_configs;
}

std::optional<std::tuple<std::string_view, std::string_view, perf::CounterConfig>>
perf::MemoryStores::resolve(const CounterDefinition& counter_definition, const std::string_view pmu_name) const
{
  if (!HardwareInfo::is_intel()) {
    throw EventRequiresSpecificVendorError{ "Intel", "mem-stores" };
  }

  return counter_definition.counter(pmu_name, std::string_view{ "mem-stores" });
}

std::vector<std::tuple<std::string_view, std::string_view, perf::CounterConfig>>
perf::MemoryLoadsAux::resolve(const CounterDefinition& counter_definition) const
{
  if (!HardwareInfo::is_intel()) {
    throw EventRequiresSpecificVendorError{ "Intel", "mem-loads-aux" };
  }

  const auto counter_configs = counter_definition.counter(std::string_view{ "mem-loads-aux" });
  if (counter_configs.empty()) {
    throw CannotFindEventError{ std::string_view{ "mem-loads-aux" } };
  }

  return counter_configs;
}

std::optional<std::tuple<std::string_view, std::string_view, perf::CounterConfig>>
perf::MemoryLoadsAux::resolve(const CounterDefinition& counter_definition, const std::string_view pmu_name) const
{
  if (!HardwareInfo::is_intel()) {
    throw EventRequiresSpecificVendorError{ "Intel", "mem-loads-aux" };
  }

  return counter_definition.counter(pmu_name, std::string_view{ "mem-loads-aux" });
}

std::vector<std::tuple<std::string_view, std::string_view, perf::CounterConfig>>
perf::IbsFetch::resolve(const CounterDefinition& counter_definition) const
{
  /// IBS Fetch has a single dedicated PMU.
  if (auto event = this->resolve(counter_definition, std::string_view{ "ibs_fetch" }); event.has_value()) {
    return { std::move(event.value()) };
  }

  throw CannotFindEventError{ std::string_view{ "ibs_fetch" } };
}

std::optional<std::tuple<std::string_view, std::string_view, perf::CounterConfig>>
perf::IbsFetch::resolve(const CounterDefinition& counter_definition, const std::string_view pmu_name) const
{
  if (!HardwareInfo::is_amd()) {
    throw EventRequiresSpecificVendorError{ "AMD", "ibs_fetch" };
  }

  /// Look up the base ibs_fetch event on the requested PMU.
  const auto base_event = counter_definition.counter(pmu_name, std::string_view{ "ibs_fetch" });
  if (!base_event.has_value()) {
    return std::nullopt;
  }

  /// Get ibs_fetch event.
  const auto& [base_pmu_name, event_name, base_config] = base_event.value();

  /// Get IBS information from perf subsystem.
  const auto& ibs_info = HardwareInfo::amd_ibs();

  /// Build the config value from the flags.
  auto config_value = 0ULL;
  if (this->_is_rand) {
    if (const auto rand_bit = ibs_info.fetch_rand_bit(); rand_bit.has_value()) {
      config_value |= 1ULL << rand_bit.value();
    } else {
      throw EventDoesNotSupportIBSFeatureError{"ibs_fetch", "randomization"};
    }
  }

  if (this->_is_l3_miss_only) {
    if (const auto l3_miss_bit = ibs_info.fetch_l3_miss_only_bit(); l3_miss_bit.has_value()) {
      config_value |= 1ULL << l3_miss_bit.value();
    } else {
      throw EventDoesNotSupportIBSFeatureError{"ibs_fetch", "l3 miss filtering"};
    }
  }


  return std::make_tuple(base_pmu_name, event_name, CounterConfig{ base_config.type(), config_value });
}

std::vector<std::tuple<std::string_view, std::string_view, perf::CounterConfig>>
perf::IbsOp::resolve(const CounterDefinition& counter_definition) const
{
  /// IBS Op has a single dedicated PMU.
  if (auto event = this->resolve(counter_definition, std::string_view{ "ibs_op" }); event.has_value()) {
    return { std::move(event.value()) };
  }

  throw CannotFindEventError{ std::string_view{ "ibs_op" } };
}

std::optional<std::tuple<std::string_view, std::string_view, perf::CounterConfig>>
perf::IbsOp::resolve(const CounterDefinition& counter_definition, const std::string_view pmu_name) const
{
  if (!HardwareInfo::is_amd()) {
    throw EventRequiresSpecificVendorError{ "AMD", "ibs_op" };
  }

  /// Look up the base ibs_op event on the requested PMU.
  const auto base_event = counter_definition.counter(pmu_name, std::string_view{ "ibs_op" });
  if (!base_event.has_value()) {
    return std::nullopt;
  }

  const auto& [base_pmu_name, event_name, base_config] = base_event.value();
  const auto& ibs_info = HardwareInfo::amd_ibs();

  /// Build the config value from the flags.
  auto config_value = 0ULL;
  if (this->_is_uop) {
    if (const auto uops_bit = ibs_info.op_uops_bit(); uops_bit.has_value()) {
      config_value |= 1ULL << uops_bit.value();
    }
    else {
      throw EventDoesNotSupportIBSFeatureError{"ibs_op", "micro operations"};
    }
  }

  if (this->_is_l3_miss_only) {
    if (const auto l3_miss_bit = ibs_info.op_l3_miss_only_bit(); l3_miss_bit.has_value()) {
      config_value |= 1ULL << l3_miss_bit.value();
    } else {
      throw EventDoesNotSupportIBSFeatureError{"ibs_op", "l3 miss filtering"};
    }
  }

  return std::make_tuple(base_pmu_name, event_name, CounterConfig{ base_config.type(), config_value });
}
