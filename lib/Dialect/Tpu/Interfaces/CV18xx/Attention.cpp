//===----------------------------------------------------------------------===//
//
// Copyright (C) 2022 Sophgo Technologies Inc.  All rights reserved.
//
// TPU-MLIR is licensed under the 2-Clause BSD License except for the
// third-party components.
//
//===----------------------------------------------------------------------===//

#include "tpu_mlir/Backend/BM168x/BM1684X.h"
#include "tpu_mlir/Dialect/Tpu/IR/TpuOps.h"
#include "tpu_mlir/Dialect/Tpu/Transforms/BM168x/WeightReorder.h"
#include "tpu_mlir/Support/Module.h"

using namespace tpu_mlir::backend;
using namespace tpu_mlir::bm1684x;

// =========================================
// GlobalGenInterface
// =========================================
void tpu::AttentionOp::codegen_global_cv18xx(int64_t layer_id) {
  llvm_unreachable("Not supported now");
}

// =========================================
// LocalGenInterface
// =========================================
int64_t tpu::AttentionOp::getBufferSize_cv18xx(int64_t in_lmem_bytes,
                                                 int64_t out_lmem_bytes,
                                                 int64_t in_nslice, int64_t in_hslice,
                                                 int64_t out_nslice,
                                                 int64_t out_hslice) {
  llvm_unreachable("Not supported now");
  return 0;
}

void tpu::AttentionOp::codegen_local_cv18xx(int64_t n_step, int64_t h_step,
                                              int64_t layer_id) {
  llvm_unreachable("Not supported now");
}