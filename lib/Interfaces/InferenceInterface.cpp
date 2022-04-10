//===- SideEffectInterfaces.cpp - SideEffects in MLIR ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "sophgo/Interfaces/InferenceInterface.h"

using namespace mlir;

namespace sophgo {

int omp_schedule(int count) {
  return (count + omp_get_num_threads() - 1) / omp_get_num_threads();
}

void relu(float *src, float *dst, int64_t size, mlir::Type elem_type) {
#pragma omp parallel for schedule(static, omp_schedule(size))
  for (int64_t i = 0; i < size; ++i) {
    dst[i] = src[i] > 0 ? src[i] : 0;
    if (elem_type && elem_type.isInteger(8)) {
      float max = elem_type.isUnsignedInteger(8) ? 255.0: 127.0;
      dst[i] = std::min(dst[i], max);
    }
  }
}

} // namespace sophgo

/// Include the definitions of the side effect interfaces.
#include "sophgo/Interfaces/InferenceInterface.cpp.inc"
