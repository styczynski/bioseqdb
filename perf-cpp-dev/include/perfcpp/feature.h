#pragma once

#include <linux/version.h>
#ifdef PERFCPP_TEST_LINUX_VERSION_CODE
#undef LINUX_VERSION_CODE
#define LINUX_VERSION_CODE PERFCPP_TEST_LINUX_VERSION_CODE
#endif

/// The features of the perf subsystem have evolved over time (more precisely over Linux Kernel generations).
/// In this file, we define some preprocessor variables to keep up with older Linux Kernel versions without yielding
/// errors at compile- and runtime.
/// The documentation for the perf_event_open system call (https://man7.org/linux/man-pages/man2/perf_event_open.2.html)
/// has a great overview of features added in various versions.
/// For the time being, we support Linux 4.0 and newer.

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 1, 0)
#define PERFCPP_NO_ERROR_EBUSY
#define PERFCPP_NO_MMAP_DATA_SIZE
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 2, 0)
#define PERFCPP_NO_SAMPLE_BRANCH_IND_JUMP
#define PERFCPP_NO_RECORD_LOST_SAMPLES
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 3, 0)
#define PERFCPP_NO_RECORD_SWITCH
#define PERFCPP_NO_BRANCH_STACK_CYCLES
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 0)
#define PERFCPP_NO_SAMPLE_BRANCH_CALL
#define PERFCPP_NO_COUNT_SW_BPF_OUTPUT
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 8, 0)
#define PERFCPP_NO_SAMPLE_MAX_STACK
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 13, 0)
#define PERFCPP_NO_SAMPLE_PHYS_ADDR
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
#define PERFCPP_NO_MEM_REMOTE
#define PERFCPP_NO_MEM_SNOOPX
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 17, 0)
#define PERFCPP_NO_RECORD_MISC_SWITCH_OUT_PREEMPT
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 5, 0)
#define PERFCPP_NO_SAMPLE_AUX
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)
#define PERFCPP_NO_RECORD_CGROUP
#define PERFCPP_NO_SAMPLE_CGROUP
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0)
#define PERFCPP_NO_SAMPLE_DATA_PAGE_SIZE
#define PERFCPP_NO_SAMPLE_CODE_PAGE_SIZE
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0)
#define PERFCPP_NO_SAMPLE_WEIGHT_STRUCT
#define PERFCPP_NO_MEM_BLK
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 13, 0)
#define PERFCPP_NO_CGROUP_SWITCHES
#define PERFCPP_NO_INHERIT_THREAD
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 16, 0)
#define PERFCPP_NO_MEM_HOPS_0
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 17, 0)
#define PERFCPP_NO_MEM_HOPS_1_3
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 0, 0)
#define PERFCPP_NO_FORMAT_LOST
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
#define PERFCPP_NO_MEM_LVLNUM
#define PERFCPP_NO_MEM_LVLNUM_PMEM
#define PERFCPP_NO_MEM_LVLNUM_IO
#define PERFCPP_NO_MEM_LVLNUM_CXL
#define PERFCPP_NO_MEM_SNOOPX_PEER
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
#define PERFCPP_NO_MEM_LVLNUM_UNC
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
#define PERFCPP_NO_MEM_LVLNUM_L2_MHB
#endif
