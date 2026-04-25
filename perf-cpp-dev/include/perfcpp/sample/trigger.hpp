#pragma once
#include <cstdint>
#include <optional>
#include <perfcpp/counter/counter.hpp>
#include <perfcpp/counter_definition.h>
#include <tuple>
#include <vector>

namespace perf {

/**
 * CPU cycles sampling trigger. Available on all supported hardware.
 */
class Cycles
{
public:
  /**
   * Resolves this trigger to all applicable (pmu_name, event_name, config) tuples for cycles.
   * Returns one entry per PMU (e.g. two entries on Intel hybrid CPUs with P- and E-cores).
   *
   * @param counter_definition Counter definition to look up the event configuration.
   * @return Per-PMU tuples of (pmu_name, event_name, config).
   */
  [[nodiscard]] std::vector<std::tuple<std::string_view, std::string_view, CounterConfig>> resolve(
    const CounterDefinition& counter_definition) const;

  /**
   * Resolves this trigger for a specific PMU.
   *
   * @param counter_definition Counter definition to look up the event configuration.
   * @param pmu_name Name of the PMU to resolve for.
   * @return Tuple of (pmu_name, event_name, config) if the event exists on that PMU.
   */
  [[nodiscard]] std::optional<std::tuple<std::string_view, std::string_view, CounterConfig>> resolve(
    const CounterDefinition& counter_definition,
    std::string_view pmu_name) const;

  /**
   * @return Transforms the trigger into a readable string.
   */
  [[nodiscard]] std::string to_string() const { return "cycles"; }
};

/**
 * Intel PEBS memory-load sampling. Supported on Haswell and newer.
 */
class MemoryLoads
{
public:
  explicit MemoryLoads(const std::uint64_t min_latency = 30U) noexcept
    : _min_latency(min_latency)
  {
  }

  /**
   * @return Minimum load latency in cycles; maps to perf_event_attr.config1 (ldlat).
   */
  [[nodiscard]] std::uint64_t min_latency() const noexcept { return _min_latency; }

  /**
   * Resolves this trigger to all applicable (pmu_name, event_name, config) tuples for mem-loads,
   * patching perf_event_attr.config1 with the configured minimum latency on each PMU variant.
   * Returns one entry per PMU (e.g. two entries on Intel hybrid CPUs with P- and E-cores).
   *
   * @param counter_definition Counter definition to look up the base event configuration.
   * @return Per-PMU tuples of (pmu_name, event_name, config) with the minimum latency applied.
   */
  [[nodiscard]] std::vector<std::tuple<std::string_view, std::string_view, CounterConfig>> resolve(
    const CounterDefinition& counter_definition) const;

  /**
   * Resolves this trigger for a specific PMU, patching config1 with the minimum latency.
   *
   * @param counter_definition Counter definition to look up the base event configuration.
   * @param pmu_name Name of the PMU to resolve for.
   * @return Tuple of (pmu_name, event_name, config) with the minimum latency applied, if the event exists on that PMU.
   */
  [[nodiscard]] std::optional<std::tuple<std::string_view, std::string_view, CounterConfig>> resolve(
    const CounterDefinition& counter_definition,
    std::string_view pmu_name) const;

  /**
   * @return Transforms the trigger into a readable string.
   */
  [[nodiscard]] std::string to_string() const { return "mem-loads"; }

private:
  /// Minimum load latency filter in cycles (ldlat).
  std::uint64_t _min_latency;
};

/**
 * Intel PEBS memory-store sampling. Supported on Haswell and newer.
 */
class MemoryStores
{
public:
  /**
   * Resolves this trigger to all applicable (pmu_name, event_name, config) tuples for mem-stores.
   * Returns one entry per PMU.
   *
   * @param counter_definition Counter definition to look up the event configuration.
   * @return Per-PMU tuples of (pmu_name, event_name, config).
   */
  [[nodiscard]] std::vector<std::tuple<std::string_view, std::string_view, CounterConfig>> resolve(
    const CounterDefinition& counter_definition) const;

  /**
   * Resolves this trigger for a specific PMU.
   *
   * @param counter_definition Counter definition to look up the event configuration.
   * @param pmu_name Name of the PMU to resolve for.
   * @return Tuple of (pmu_name, event_name, config) if the event exists on that PMU.
   */
  [[nodiscard]] std::optional<std::tuple<std::string_view, std::string_view, CounterConfig>> resolve(
    const CounterDefinition& counter_definition,
    std::string_view pmu_name) const;

  /**
   * @return Transforms the trigger into a readable string.
   */
  [[nodiscard]] std::string to_string() const { return "mem-stores"; }
};

/**
 * Intel PEBS auxiliary event for memory-load sampling. Required on Sapphire Rapids and newer.
 * When included as the leading trigger in a group alongside MemoryLoad, the auxiliary counter
 * is used as-provided rather than auto-inserted by the sampler.
 */
class MemoryLoadsAux
{
public:
  /**
   * Resolves this trigger to all applicable (pmu_name, event_name, config) tuples for mem-loads-aux.
   * Returns one entry per PMU.
   *
   * @param counter_definition Counter definition to look up the event configuration.
   * @return Per-PMU tuples of (pmu_name, event_name, config).
   */
  [[nodiscard]] std::vector<std::tuple<std::string_view, std::string_view, CounterConfig>> resolve(
    const CounterDefinition& counter_definition) const;

  /**
   * Resolves this trigger for a specific PMU.
   *
   * @param counter_definition Counter definition to look up the event configuration.
   * @param pmu_name Name of the PMU to resolve for.
   * @return Tuple of (pmu_name, event_name, config) if the event exists on that PMU.
   */
  [[nodiscard]] std::optional<std::tuple<std::string_view, std::string_view, CounterConfig>> resolve(
    const CounterDefinition& counter_definition,
    std::string_view pmu_name) const;

  /**
   * @return Transforms the trigger into a readable string.
   */
  [[nodiscard]] std::string to_string() const { return "mem-loads-aux"; }
};

/**
 * AMD Instruction Based Sampling — fetch pipeline.
 */
class IbsFetch
{
public:
  explicit IbsFetch(const bool is_rand = true, const bool is_l3_miss_only = false) noexcept
    : _is_rand(is_rand)
    , _is_l3_miss_only(is_l3_miss_only)
  {
  }

  /**
   * @return True if only L3-cache-missing fetches are sampled.
   */
  [[nodiscard]] bool is_l3_miss_only() const noexcept { return _is_l3_miss_only; }

  /**
   * @return True if randomised fetch-count tagging is enabled (IbsFetchCtl.IbsFetchRandEn).
   */
  [[nodiscard]] bool is_rand() const noexcept { return _is_rand; }

  /**
   * Resolves this trigger to the (pmu_name, event_name, config) tuple for the appropriate
   * ibs_fetch event variant. AMD IBS uses a single PMU so the result always has one entry.
   *
   * @param counter_definition Counter definition to look up the event configuration.
   * @return Single-element vector with (pmu_name, event_name, config) for the resolved variant.
   */
  [[nodiscard]] std::vector<std::tuple<std::string_view, std::string_view, CounterConfig>> resolve(
    const CounterDefinition& counter_definition) const;

  /**
   * Resolves this trigger for a specific PMU.
   *
   * @param counter_definition Counter definition to look up the event configuration.
   * @param pmu_name Name of the PMU to resolve for.
   * @return Tuple of (pmu_name, event_name, config) if the event exists on that PMU.
   */
  [[nodiscard]] std::optional<std::tuple<std::string_view, std::string_view, CounterConfig>> resolve(
    const CounterDefinition& counter_definition,
    std::string_view pmu_name) const;

  /**
   * @return Transforms the trigger into a readable string.
   */
  [[nodiscard]] std::string to_string() const { return "ibs_fetch"; }

private:
  /// Enables randomised fetch-count offset (rand_en bit).
  bool _is_rand;

  /// Restricts sampling to fetches that miss the L3 cache.
  bool _is_l3_miss_only;
};

/**
 * AMD Instruction Based Sampling — op (execute) pipeline.
 */
class IbsOp
{
public:
  explicit IbsOp(const bool is_uop = false, const bool is_l3_miss_only = false) noexcept
    : _is_uop(is_uop)
    , _is_l3_miss_only(is_l3_miss_only)
  {
  }

  /**
   * @return True if only L3-cache-missing ops are sampled.
   */
  [[nodiscard]] bool is_l3_miss_only() const noexcept { return _is_l3_miss_only; }

  /**
   * @return True if micro-op counting is enabled instead of dispatched-op counting (IbsOpCtl.IbsOpCntCtl / cnt_ctl).
   */
  [[nodiscard]] bool is_uop() const noexcept { return _is_uop; }

  /**
   * Resolves this trigger to the (pmu_name, event_name, config) tuple for the appropriate
   * ibs_op event variant, selected based on the is_uop and is_l3_miss_only flags.
   * AMD IBS uses a single PMU so the result always has one entry.
   *
   * @param counter_definition Counter definition to look up the event configuration.
   * @return Single-element vector with (pmu_name, event_name, config) for the resolved variant.
   */
  [[nodiscard]] std::vector<std::tuple<std::string_view, std::string_view, CounterConfig>> resolve(
    const CounterDefinition& counter_definition) const;

  /**
   * Resolves this trigger for a specific PMU.
   *
   * @param counter_definition Counter definition to look up the event configuration.
   * @param pmu_name Name of the PMU to resolve for.
   * @return Tuple of (pmu_name, event_name, config) if the event exists on that PMU.
   */
  [[nodiscard]] std::optional<std::tuple<std::string_view, std::string_view, CounterConfig>> resolve(
    const CounterDefinition& counter_definition,
    std::string_view pmu_name) const;

  /**
   * @return Transforms the trigger into a readable string.
   */
  [[nodiscard]] std::string to_string() const { return "ibs_op"; }

private:
  /// Switches the op counter to count micro-ops (cnt_ctl bit).
  bool _is_uop;

  /// Restricts sampling to ops that miss the L3 cache.
  bool _is_l3_miss_only;
};

}
