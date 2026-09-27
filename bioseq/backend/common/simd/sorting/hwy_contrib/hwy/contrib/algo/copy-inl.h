// Copyright 2022 Google LLC
// SPDX-License-Identifier: Apache-2.0
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

// Per-target include guard
#if defined(HIGHWAY_HWY_CONTRIB_ALGO_COPY_INL_H_) == \
    defined(HWY_TARGET_TOGGLE)  // NOLINT
#ifdef HIGHWAY_HWY_CONTRIB_ALGO_COPY_INL_H_
#undef HIGHWAY_HWY_CONTRIB_ALGO_COPY_INL_H_
#else
#define HIGHWAY_HWY_CONTRIB_ALGO_COPY_INL_H_
#endif

#include <stddef.h>
#include <stdint.h>

#include "hwy/highway.h"

HWY_BEFORE_NAMESPACE();
namespace hwy {
namespace HWY_NAMESPACE {

// SIMD fill  and copy operations for arrays that may be unaligned or unpadded.
// Each function handles the final partial vector.

// Fills `to`[0, `count`) with `value`.
template <class D, typename T = TFromD<D>>
void Fill(D d, T value, size_t count, T* HWY_RESTRICT to) {
  const size_t N = Lanes(d);
  const Vec<D> v = Set(d, value);

  size_t idx = 0;
  if (count >= N) {
    for (; idx <= count - N; idx += N) {
      StoreU(v, d, to + idx);
    }
  }

  // `count` was a multiple of the vector length `N`: already done.
  if (HWY_UNLIKELY(idx == count)) return;

  const size_t remaining = count - idx;
  HWY_DASSERT(0 != remaining && remaining < N);
  SafeFillN(remaining, value, d, to + idx);
}

// Copies `from`[0, `count`) to `to`, which must not overlap `from`.
template <class D, typename T = TFromD<D>>
void Copy(D d, const T* HWY_RESTRICT from, size_t count, T* HWY_RESTRICT to) {
  const size_t N = Lanes(d);

  size_t idx = 0;
  if (count >= N) {
    for (; idx <= count - N; idx += N) {
      const Vec<D> v = LoadU(d, from + idx);
      StoreU(v, d, to + idx);
    }
  }

  // `count` was a multiple of the vector length `N`: already done.
  if (HWY_UNLIKELY(idx == count)) return;

  const size_t remaining = count - idx;
  HWY_DASSERT(0 != remaining && remaining < N);
  SafeCopyN(remaining, d, from + idx, to + idx);
}

// Copies selected elements from `from` to `to`. Input order is preserved.
// `func` accepts a tag and vector and returns a mask with one decision per
// lane, using a functor with a templated operator() or a C++14 generic lambda.
// Returns the end pointer.
//
// Clang on Windows may require HWY_ATTR before the lambda body to avoid target
// mismatch errors. Supported element sizes are 16, 32, and 64 bits. The final
// partial vector may make `func` see some elements twice, but each selected
// element is copied only  once.
template <class D, class Func, typename T = TFromD<D>>
T* CopyIf(D d, const T* HWY_RESTRICT from, size_t count, T* HWY_RESTRICT to,
          const Func& func) {
  const size_t N = Lanes(d);

  size_t idx = 0;
  if (count >= N) {
    for (; idx <= count - N; idx += N) {
      const Vec<D> v = LoadU(d, from + idx);
      to += CompressBlendedStore(v, func(d, v), d, to);
    }
  }

  // `count` was a multiple of the vector length `N`: already done.
  if (HWY_UNLIKELY(idx == count)) return to;

#if HWY_MEM_OPS_MIGHT_FAULT
  // Read the remaining elements individually on targets where masked loads may fault.
  const CappedTag<T, 1> d1;
  for (; idx < count; ++idx) {
    using V1 = Vec<decltype(d1)>;
    // Workaround for -Waggressive-loop-optimizations on GCC 8
    // (iteration 2305843009213693951 invokes undefined behavior for T=i64)
    const uintptr_t addr = reinterpret_cast<uintptr_t>(from);
    const T* HWY_RESTRICT from_idx =
        reinterpret_cast<const T * HWY_RESTRICT>(addr + (idx * sizeof(T)));
    const V1 v = LoadU(d1, from_idx);
    // Store only selected elements so an output buffer sized to the number of
    // matches is sufficient.
    if (CountTrue(d1, func(d1, v)) == 0) continue;
    StoreU(v, d1, to);
    to += 1;
  }
#else
  // Start index of the last unaligned whole vector, ending at the array end.
  const size_t last = count - N;
  // Number of elements before `from` or already written.
  const size_t invalid = idx - last;
  HWY_DASSERT(0 != invalid && invalid < N);
  const Mask<D> mask = Not(FirstN(d, invalid));
  const Vec<D> v = MaskedLoad(mask, d, from + last);
  to += CompressBlendedStore(v, And(mask, func(d, v)), d, to);
#endif
  return to;
}

// NOLINTNEXTLINE(google-readability-namespace-comments)
}  // namespace HWY_NAMESPACE
}  // namespace hwy
HWY_AFTER_NAMESPACE();

#endif  // HIGHWAY_HWY_CONTRIB_ALGO_COPY_INL_H_
