#include <perfcpp/exception.hpp>
#include <perfcpp/feature.h>

std::string
perf::CannotOpenCounterError::create_error_message_from_code(const std::int64_t error_code)
{
  switch (error_code) {
    case ENOENT:
      return "configuration might not be valid (e.g., unsupported event)";
    case E2BIG:
      return "perf_event_attr.size was not configured properly – this could be a bug in the perf-cpp library";
    case EACCES:
      return "insufficient access rights to start the counter, e.g., profiling a not user-owned process or "
             "perf_event_paranoid value too high (see "
             "https://jmuehlig.github.io/perf-cpp/perf-paranoid/)";
#ifndef PERFCPP_NO_ERROR_EBUSY /// Busy error is reported since Linux 4.1
    case EBUSY:
      return "another event has exclusive access to the PMU";
#endif
    case EINVAL:
      return "counter is configured with an invalid argument (e.g., too high sample frequency, unknown CPU, invalid "
             "sample type)";
    case EMFILE:
      return "too many open file descriptors (e.g., too many opened counters?)";
    case ENODEV:
      return "configured with feature that does not exist on this CPU";
    case EOVERFLOW:
      return "maximal callchain stack size is higher than the maximum (see /proc/sys/kernel/perf_event_max_stack)";
    case EPERM:
      return "one of the following features is set but not supported: excluding hypervisor, excluding idle, "
             "excluding "
             "user, or excluding kernel";
    case ESRCH:
      return "specified process does not exist";
    default:
      return "perf_event_open failed with unknown error";
  }
}

std::string
perf::IoctlError::create_error_message_from_code(const std::int64_t error_code)
{
  switch (error_code) {
    case EBADF:
      return "file descriptor is not valid";
    case EFAULT:
      return "references inaccessible memory area";
    case ENOTTY:
      return "file descriptor cannot be used";
    default:
      return "::ioctl failed with unknown error";
  }
}