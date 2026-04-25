#pragma once

#include <cstdint>
#include <limits>

namespace perf {
/**
 * The precision controls the skid, which refers to the amount of
 * instructions between the event and the kernel records the sample.
 *
 * For more information see "precise_ip" on https://man7.org/linux/man-pages/man2/perf_event_open.2.html.
 */
enum class Precision : std::uint8_t
{
  /// The recorded instruction pointer may land anywhere within a broad, implementation-defined window around the real
  /// instruction.
  AllowArbitrarySkid = 0U,

  /// The recorded instruction pointer must have a constant, repeatable skid offset (still non-zero) so the displacement
  /// is predictable even if not exact.
  MustHaveConstantSkid = 1U,

  /// Request–but do not insist on—zero skid, asking the PMU for exact instruction pointer attribution while allowing
  /// fallback on CPUs that cannot guarantee it.
  RequestZeroSkid = 2U,

  /// Require zero skid: the sample instruction pointer must be the exact triggering instruction; if the hardware cannot
  /// provide this, perf-cpp will lower the precision.
  MustHaveZeroSkid = 3U,
};
}