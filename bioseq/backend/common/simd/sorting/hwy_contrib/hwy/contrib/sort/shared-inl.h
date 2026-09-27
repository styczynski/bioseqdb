// Copyright 2021 Google LLC
// Copyright 2025 Arm Limited and/or its affiliates <open-source-office@arm.com>
// SPDX-License-Identifier: Apache-2.0
// SPDX-License-Identifier: BSD-3-Clause
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Definitions shared between vqsort-inl and sorting_networks-inl.

// Normal include guard for target-independent parts
#ifndef HIGHWAY_HWY_CONTRIB_SORT_SHARED_INL_H_
#define HIGHWAY_HWY_CONTRIB_SORT_SHARED_INL_H_

#include "hwy/base.h"

namespace hwy {

// Based on https://github.com/numpy/numpy/issues/16313#issuecomment-641897028
static HWY_INLINE uint64_t RandomBits(uint64_t* HWY_RESTRICT state) {
  const uint64_t a = state[0];
  const uint64_t b = state[1];
  const uint64_t w = state[2] + 1;
  const uint64_t next = a ^ w;
  state[0] = (b + (b << 3)) ^ (b >> 11);
  const uint64_t rot = (b << 24) | (b >> 40);
  state[1] = rot + next;
  state[2] = w;
  return next;
}

// Sizes used by the sorting networks and partition loops. Changes here
// require matching changes in the code that loads and stores those groups.
struct SortConstants {
  // Maximum lanes in one row of the sorting network. At least 8 are needed
  // so an 8-row network can sort the 128-byte pivot sample with 16-bit lanes.
#if HWY_COMPILER_MSVC || HWY_IS_DEBUG_BUILD
  static constexpr size_t kMaxCols = 8;  // avoid build timeout/stack overflow
#else
  static constexpr size_t kMaxCols = 16;  // enough for u32 in 512-bit vector
#endif

  // Sixteen rows keep the network small enough for AVX2 while using the extra
  // registers available on AVX-512, SVE, and RVV. Larger networks also need more
  // comparison steps and generate more code.
  static constexpr size_t kMaxRows = 16;

  // A compile-time lanes-per-key value lets the compiler simplify the division.
  template <size_t kLPK>
  static constexpr HWY_INLINE size_t BaseCaseNumLanes(size_t N) {
    // Use 8-row networks for fewer than four keys per vector and 16 rows for
    // wider vectors.
    return (((N / kLPK) >= 4) ? kMaxRows : 8) * HWY_MIN(N, kMaxCols);
  }

  // Partition handles four vectors per iteration to spread loop overhead
  // over more work. The explicit offsets in that loop must change with this
  // constant.
  static constexpr size_t kPartitionUnroll = 4;

  // Number of lanes in a 64-byte group sampled for a pivot. Sort caps vector
  // size at 64 bytes, so a sample group holds whole vectors and fits the usual
  // cache-line size.
  static constexpr HWY_INLINE size_t LanesPerChunk(size_t sizeof_t) {
    return 64 / sizeof_t;
  }

  template <typename T>
  static constexpr HWY_INLINE size_t SampleLanes() {
    return 2 * LanesPerChunk(sizeof(T));  // Stored samples
  }

  static constexpr HWY_INLINE size_t PartitionBufNum(size_t N) {
    // Reserve both sides of the temporary partition buffer. This also covers
    // small inputs that skip the main loop and need up to 3 * kPartitionUnroll + 1
    // vectors on each side.
    return 2 * (3 * kPartitionUnroll + 1) * N;
  }

  // Buffer capacity for pivot sampling, base-case sorting, and partitioning.
  template <typename T, size_t kLPK>
  static constexpr HWY_INLINE size_t BufNum(size_t N) {
    // BaseCase may write one padding vector, and SortSamples uses the space
    // after samples as the buffer.
    return HWY_MAX(SampleLanes<T>() + BaseCaseNumLanes<kLPK>(N) + N,
                   PartitionBufNum(N));
  }

  // Translates vector_size to lanes and returns size in bytes.
  template <typename T, size_t kLPK>
  static constexpr HWY_INLINE size_t BufBytes(size_t vector_size) {
    return BufNum<T, kLPK>(vector_size / sizeof(T)) * sizeof(T);
  }

  // Largest buffer size among the supported lane types.
  template <size_t kLPK>
  static constexpr HWY_INLINE size_t MaxBufBytes(size_t vector_size) {
    // If 2 lanes per key, it's a 128-bit key with u64 lanes.
    return kLPK == 2 ? BufBytes<uint64_t, 2>(vector_size)
                     : HWY_MAX((BufBytes<uint16_t, 1>(vector_size)),
                               HWY_MAX((BufBytes<uint32_t, 1>(vector_size)),
                                       (BufBytes<uint64_t, 1>(vector_size))));
  }
};

static_assert(SortConstants::MaxBufBytes<1>(64) <= 1664, "Unexpectedly high");
static_assert(SortConstants::MaxBufBytes<2>(64) <= 1664, "Unexpectedly high");

}  // namespace hwy

#endif  // HIGHWAY_HWY_CONTRIB_SORT_SHARED_INL_H_

// Per-target
// clang-format off
#if defined(HIGHWAY_HWY_CONTRIB_SORT_SHARED_TOGGLE) == defined(HWY_TARGET_TOGGLE) // NOLINT
// clang-format on
#ifdef HIGHWAY_HWY_CONTRIB_SORT_SHARED_TOGGLE
#undef HIGHWAY_HWY_CONTRIB_SORT_SHARED_TOGGLE
#else
#define HIGHWAY_HWY_CONTRIB_SORT_SHARED_TOGGLE
#endif

#include "hwy/highway.h"

// Disable VQSort for unsupported targets and compiler combinations with
// known build failures. These include optimized MSVC, Armv7 debug, Arm64
// ASan, and older RISC-V GCC builds. SVE also has sanitizer and scalable-vector
// restrictions below.
// GCC issue: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=97696
// Clang SVE sanitizer issue: b/335157772
#undef VQSORT_ENABLED
#undef VQSORT_COMPILER_COMPATIBLE

#if (HWY_COMPILER_MSVC && !HWY_IS_DEBUG_BUILD) || \
    (HWY_ARCH_ARM_V7 && HWY_IS_DEBUG_BUILD) ||    \
    (HWY_ARCH_ARM_A64 && HWY_IS_ASAN) ||          \
    (HWY_ARCH_RISCV && HWY_COMPILER_GCC_ACTUAL < 1400)
#define VQSORT_COMPILER_COMPATIBLE 0
#else
#define VQSORT_COMPILER_COMPATIBLE 1
#endif

#if (HWY_TARGET == HWY_SCALAR) || !VQSORT_COMPILER_COMPATIBLE ||    \
    ((HWY_TARGET & HWY_ALL_SVE) && defined(SAFESTACK_SANITIZER)) || \
    ((HWY_TARGET & HWY_ALL_SVE) && HWY_HAVE_SCALABLE) ||            \
    defined(HWY_DISABLE_VQSORT)
#define VQSORT_ENABLED 0
#else
#define VQSORT_ENABLED 1
#endif

namespace hwy {
namespace HWY_NAMESPACE {

// Default tag / vector width selector.
#if HWY_TARGET == HWY_RVV
// Use LMUL = 1/2. For 64-bit lanes, VSETVLI emulates this width.
template <typename T>
using SortTag = ScalableTag<T, -1>;
#else
template <typename T>
using SortTag = ScalableTag<T>;
#endif

// NOLINTNEXTLINE(google-readability-namespace-comments)
}  // namespace HWY_NAMESPACE
}  // namespace hwy

#endif  // HIGHWAY_HWY_CONTRIB_SORT_SHARED_TOGGLE
