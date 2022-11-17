//===----------------------------------------------------------------------===//
//
// Copyright (C) 2022 Sophgo Technologies Inc.  All rights reserved.
//
// TPU-MLIR is licensed under the 2-Clause BSD License except for the
// third-party components.
//
//===----------------------------------------------------------------------===//

#include "tpu_mlir/Backend/BM168x/BM1684x.h"
#include "tpu_mlir/Dialect/Tpu/IR/TpuOps.h"
#include "tpu_mlir/Support/Helper/Module.h"
#include "tpu_mlir/Support/Helper/Quant.h"

using namespace mlir;
using namespace tpu_mlir;
using namespace tpu_mlir::helper;
using namespace tpu_mlir::backend;

// =========================================
// GlobalGenInterface
// =========================================

// int8
void tpu::MulConstOp::codegen_global_bm1684x() {
  int64_t n, c, h, w;
  Module::getNCHW(output(), n, c, h, w);
  auto op = getOperation();
  auto input_spec = BM168x::get_input_spec(op);
  auto output_spec = BM168x::get_output_spec(op);
  constbinary_global_spec_t param = {0};
  param.common.binary_type = BINARY_MUL;
  param.common.if_relu = do_relu();
  param.common.relu_upper_limit = relu_limit().convertToDouble();
  if (Quant::isUniformQuantized(input())) {
    param.common.B_const_val = 1; // coeff has been merge in multiplier&&rshift
    param.common.B_dtype = DTYPE_INT8;
    param.common.inversed = 0;
    param.common.scale_A = multiplier();
    param.common.rshift_A = rshift();
    BM168x::instance(Module::getChip(op))->call_global_func(
        "backend_api_constbinary_global", &param, sizeof(param),
        input_spec->data(), output_spec->data());
  } else {
    param.common.B_const_val = const_val().convertToDouble();
    param.common.B_dtype = DTYPE_FP32;
    param.common.inversed = 0;

    BM168x::instance(Module::getChip(op))->call_global_func(
        "backend_api_constbinary_global", &param, sizeof(param),
        input_spec->data(), output_spec->data());
  }
}

// =========================================
// LocalGenInterface
// =========================================

static bool is_sign(DATA_TYPE_T dtype) {
  return !(dtype == DTYPE_UINT8 || dtype == DTYPE_UINT16 ||
           dtype == DTYPE_UINT32);
}

int64_t tpu::MulConstOp::getBufferSize_bm1684x(
    int64_t in_lmem_bytes, int64_t out_lmem_bytes, int64_t in_nslice,
    int64_t in_hslice, int64_t out_nslice, int64_t out_hslice) {
  int64_t buffer_size = 0;
  auto dtype_A = BM168x::getDataType(input());
  if (dtype_A == DTYPE_INT8 || dtype_A == DTYPE_UINT8) {
    buffer_size = in_lmem_bytes * 2;
  }
  return buffer_size;
}

void tpu::MulConstOp::codegen_local_bm1684x(int64_t n_step, int64_t h_step) {
  int64_t n, c, h, w;
  Module::getNCHW(input(), n, c, h, w);
  auto op = getOperation();
  auto input_spec = BM168x::get_input_spec(op);
  auto output_spec = BM168x::get_output_spec(op);
  auto gi = getGroupInfo(n_step, h_step);
  auto in_gi = LocalGenInterface::getGroupInfo(input(), n_step, h_step);
  constbinary_local_spec_t param = {0};

  param.common.binary_type = BINARY_MUL;
  param.common.if_relu = do_relu();
  param.common.relu_upper_limit = relu_limit().convertToDouble();
  if (Quant::isUniformQuantized(input())) {
    param.common.B_const_val = 1; // coeff has been merge in multiplier&&rshift
    param.common.B_dtype = DTYPE_INT8;
    param.common.inversed = 0;
    param.common.scale_A = multiplier();
    param.common.rshift_A = rshift();
  } else {
    param.common.B_const_val = const_val().convertToDouble();
    param.common.B_dtype = DTYPE_FP32; // assume coeff is fp32
    param.common.inversed = 0;
  }
  local_sec_info_t sec_info = {0};
  sec_info.n_slice = in_gi.n_slice;
  sec_info.d_slice = 1;
  sec_info.h_slice = in_gi.h_slice;
  sec_info.h_idx = in_gi.h_idx;
  sec_info.is_h_split = !(in_gi.h_idx == 0 && in_gi.h_slice == h);
  sec_info.w_slice = w;
  sec_info.out_n_slice = gi.n_slice;
  sec_info.out_h_idx = gi.h_idx;
  sec_info.out_h_slice = gi.h_slice;
  sec_info.out_w_slice = w;

  BM168x::instance(Module::getChip(op))->call_local_func("backend_api_constbinary_local", &param,
                                      sizeof(param), &sec_info,
                                      input_spec->data(), output_spec->data());
}
