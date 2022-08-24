//===----------------------------------------------------------------------===//
//
// Copyright (c) 2020-2030 by Sophgo Technologies Inc. All rights reserved.
//
// Licensed under the Apache License v2.0.
// See http://www.apache.org/licenses/LICENSE-2.0 for license information.
// SPDX-License-Identifier: Apache-2.0
//
//===----------------------------------------------------------------------===//

#include "tpu_mlir/Backend/BM168x/BM1684x.h"
#include "tpu_mlir/Dialect/Tpu/IR/TpuOps.h"
#include "tpu_mlir/Support/Dnnl/Conv.h"
#include "tpu_mlir/Support/Helper/Module.h"
#include "tpu_mlir/Support/Helper/Quant.h"
#include "tpu_mlir/Support/MathUtils.h"

using namespace mlir;
using namespace tpu_mlir;
using namespace tpu_mlir::helper;
using namespace tpu_mlir::backend;

// ======================================
// WeightReorderInterface
// ======================================

// convert (1, oc, 1, w) to (1, NPU_NUM, 1, DIV_UP(oc, NPU_NUM) * w)
template <typename T>
static void
reshape_coeff_for_broadcast_channel(std::shared_ptr<std::vector<T>> &coeff,
                                    std::vector<int64_t> &shape,
                                    bool align = false) {
  int64_t n, c, h, w;
  Module::getNCHW(shape, n, c, h, w);
  if (n != 1 || h != 1 || c <= BM1684x::NPU_NUM) {
    return;
  }
  int in_type_len = sizeof(T);
  auto old_w_align = align_up(w, BM1684x::instance().get_eu_num(in_type_len));

  // convert (1, oc, 1, w) to (1, NPU_NUM, 1, DIV_UP(oc, NPU_NUM) * w)
  int64_t new_c = BM1684x::NPU_NUM;
  auto c2w = ceiling_func(c, new_c);
  int64_t new_w = (align ? old_w_align : w) * (c2w - 1) + w;
  auto coeff_new = std::make_shared<std::vector<T>>(new_w * new_c, 0);
  for (uint i = 0; i < c2w; i++) {
    for (uint j = 0; j < new_c; j++) {
      if (i * new_c + j >= c) {
        break;
      }
      for (uint k = 0; k < w; k++) {
        uint src_idx = (i * new_c + j) * w + k;
        uint dst_idx = j * new_w + i * (align ? old_w_align : w) + k;
        coeff_new->at(dst_idx) = coeff->at(src_idx);
      }
    }
  }
  shape = {1, new_c, 1, new_w};
  coeff = coeff_new;
}

template <typename T>
static void filter_reorder(std::shared_ptr<std::vector<T>> &filter,
                           std::vector<int64_t> &shape) {
  int64_t oc, ic, kh, kw;
  Module::getNCHW(shape, oc, ic, kh, kw);
  auto type_bytes = sizeof(T);
  int64_t IC_PARALLEL = 64 / type_bytes;
  auto kernel_hw = kh * kw;
  int64_t new_ic = ceiling_func(ic, IC_PARALLEL);
  int64_t new_hw = kernel_hw * IC_PARALLEL;
  auto filter_new = std::make_shared<std::vector<T>>(oc * new_ic * new_hw, 0);
  for (int oc_idx = 0; oc_idx < oc; oc_idx++) {
    for (int ic_idx = 0; ic_idx < new_ic; ic_idx++) {
      for (int k_idx = 0; k_idx < kernel_hw; k_idx++) {
        for (int inner = 0; inner < IC_PARALLEL; inner++) {
          if (ic_idx * IC_PARALLEL + inner >= ic)
            break;
          int orig_offset = oc_idx * ic * kh * kw +
                            (ic_idx * IC_PARALLEL + inner) * kernel_hw + k_idx;
          int trans_offset = oc_idx * new_ic * new_hw + ic_idx * new_hw +
                             k_idx * IC_PARALLEL + inner;
          filter_new->at(trans_offset) = filter->at(orig_offset);
        }
      }
    }
  }
  filter = filter_new;
  shape = {1, oc, 1, new_ic * new_hw};
}

template <typename T>
static void reshape_coeff_for_3ic(std::shared_ptr<std::vector<T>> &weight,
                                  std::vector<int64_t> &shape,
                                  int64_t use_3ic_optimize) {
  int64_t oc, ic, kh, kw;
  Module::getNCHW(shape, oc, ic, kh, kw);
  use_3ic_optimize = use_3ic_optimize & 0x3;

  // if merge kw to ic, it need convert (oc, ic, kh, kw) to (oc, ic, kw, kh).
  if (use_3ic_optimize == 2) {
    auto khw = std::make_shared<std::vector<T>>(kh * kw, 0);
    for (uint i = 0; i < oc * ic; ++i) {
      for (uint j = 0; j < kh; ++j) {
        for (uint k = 0; k < kw; ++k) {
          khw->at(k * kh + j) = weight->at(i * kh * kw + j * kw + k);
        }
      }
      for (uint j = 0; j < kh * kw; ++j) {
        weight->at(i * kh * kw + j) = khw->at(j);
      }
    }
  }

  int64_t new_ic, new_kernel;
  switch (use_3ic_optimize) {
  case 1: // merge kh to ic
    new_ic = ic * kh;
    new_kernel = kw;
    break;
  case 2: // merge kw to ic
    new_ic = ic * kw;
    new_kernel = kh;
    break;
  case 3: // merge kh and kw to ic
    new_ic = ic * kh * kw;
    new_kernel = 1;
    break;
  default: // not merge
    new_ic = ic;
    new_kernel = kh * kw;
    break;
  }

  shape = {oc, new_ic, 1, new_kernel};
  filter_reorder(weight, shape);
}

// refer to net_compiler: bool BM1684xCoeffArranger::ConvWeightArr(GraphEdge*
// edge)
void tpu::Conv2DOp::weight_reorder_int8_bm1684x() {
  conv_attr_t attr = {0};
  parseParam(&attr);

  bool merge = true;
  auto out_stype = Module::getStorageType(output());
  if (out_stype.isInteger(32)) {
    merge = false;
  }
  auto op = getOperation();
  OpBuilder builder(getContext());
  // filter
  auto filterOp = filter().getDefiningOp<top::WeightOp>();
  auto filter_i8 = filterOp.read<int8_t>();
  std::vector<int64_t> filter_shape = {attr.oc, attr.ic / attr.groups, attr.kh,
                                       attr.kw};
  int IC_PARALLEL = 64;
  int use_3ic_optimize = 0;
  if (attr.ic * attr.kh * attr.kw <= IC_PARALLEL && attr.kh > 1 &&
      attr.kw > 1) {
    use_3ic_optimize = 3; // merge kh and kw to ic
  } else if (attr.ic * attr.kw <= IC_PARALLEL && attr.kw > 1 &&
             (attr.kh < attr.kw || attr.ic * attr.kh > IC_PARALLEL)) {
    use_3ic_optimize = 2; // merge kw to ic
  } else if (attr.ic * attr.kh <= IC_PARALLEL && attr.kh > 1) {
    use_3ic_optimize = 1; // merge kh to ic
  } else {
    use_3ic_optimize = 0;
  }
  if (use_3ic_optimize) {
    // Now only support broadcast using BDC when it is a local layer.
    use_3ic_optimize |= 0x10;
  }

  if (attr.is_dw == false) {
    reshape_coeff_for_3ic(filter_i8, filter_shape, use_3ic_optimize);
  } else {
    filter_shape = {1, attr.oc, 1, attr.kh * attr.kw};
  }
  op->setAttr("use_3ic_optimize", builder.getI64IntegerAttr(use_3ic_optimize));
  if (merge == false) {
    auto stype = Module::getStorageType(filter());
    auto new_type = RankedTensorType::get(filter_shape, stype);
    auto new_op =
        top::WeightOp::create(op, "filter_reorderd", *filter_i8, new_type);
    op->setOperand(1, new_op);
    if (attr.has_bias) {
      auto elem_type = Module::getStorageType(bias());
      auto bias_type = RankedTensorType::get({1, attr.oc, 1, 1}, elem_type);
      bias().setType(bias_type);
    }
    return;
  }
  reshape_coeff_for_broadcast_channel(filter_i8, filter_shape, false);
  int64_t new_oc = filter_shape[1];
  int64_t filter_w_bytes = filter_shape[3] * sizeof(int8_t);

  // bias
  std::shared_ptr<std::vector<int32_t>> bias_new;
  std::vector<int64_t> bias_shape = {1, attr.oc, 1, 1};
  int64_t bias_w_bytes = 0;
  if (attr.has_bias) {
    auto biasOp = bias().getDefiningOp<top::WeightOp>();
    bias_new = biasOp.read<int32_t>();
    reshape_coeff_for_broadcast_channel(bias_new, bias_shape, false);
    assert(new_oc == bias_shape[1]);
    bias_w_bytes = bias_shape[3] * sizeof(int32_t);
  }
  if (merge == false) {
    return;
  }

  // requant
  auto qtype = Quant::getUniformQuantizedType(output());
  std::vector<int64_t> quant_shape = {1, attr.oc, 1, 3};
  auto quant_data = std::make_shared<std::vector<int32_t>>(attr.oc * 3, 0);
  auto m_data = Module::getI64Array(multiplier(), attr.oc, 1);
  auto r_data = Module::getI64Array(rshift(), attr.oc, 0);
  for (int i = 0; i < attr.oc; i++) {
    quant_data->at(i * 3) = m_data->at(i);
    quant_data->at(i * 3 + 1) = -r_data->at(i);
    quant_data->at(i * 3 + 2) = qtype.getZeroPoint();
  }
  reshape_coeff_for_broadcast_channel(quant_data, quant_shape, true);
  assert(new_oc == quant_shape[1]);
  int64_t quant_w_bytes = quant_shape[3] * sizeof(int32_t);

  // merge
  int64_t quant_offset = 0, bias_offset = 0, filter_offset = 0;
  int64_t filter_align = attr.is_dw ? 1 : BM1684x::EU_BYTES;
  if (attr.has_bias) {
    bias_offset =
        align_up(quant_offset + quant_w_bytes, (int64_t)sizeof(int32_t));
    filter_offset = align_up(bias_offset + bias_w_bytes, filter_align);
  } else {
    filter_offset = align_up(quant_offset + quant_w_bytes, filter_align);
  }
  int64_t merge_w = filter_offset + filter_w_bytes;
  // merge requant/bias/filter
  auto new_coeff = std::make_shared<std::vector<int8_t>>(new_oc * merge_w, 0);
  std::vector<int64_t> coeff_shape = {1, new_oc, 1, merge_w};
  for (int i = 0; i < new_oc; i++) {
    auto coeff_ptr = new_coeff->data() + i * merge_w;
    auto quant_ptr = quant_data->data() + i * quant_shape[3];
    auto bias_ptr =
        attr.has_bias ? (bias_new->data() + i * bias_shape[3]) : nullptr;
    auto filter_ptr = filter_i8->data() + i * filter_shape[3];
    // copy quant
    memcpy(coeff_ptr + quant_offset, quant_ptr, quant_w_bytes);
    if (attr.has_bias) {
      memcpy(coeff_ptr + bias_offset, bias_ptr, bias_w_bytes);
    }
    memcpy(coeff_ptr + filter_offset, filter_ptr, filter_w_bytes);
  }
  if (merge_w > 65535) {
    if (attr.is_dw) {
      coeff_shape[2] = ceiling_func(attr.oc, (int64_t)64);
      coeff_shape[3] /= coeff_shape[2];
    } else {
      coeff_shape[2] = 64;
      coeff_shape[3] /= 64;
    }
  }
  auto elem_type = Module::getStorageType(filter());
  auto coeff_type = RankedTensorType::get(coeff_shape, elem_type);
  auto coeff_op = top::WeightOp::create(op, "merge", *new_coeff, coeff_type);
  op->removeAttr("rshift");
  op->removeAttr("multiplier");
  op->setAttr("coeff_merged", builder.getBoolAttr(true));
  op->setOperand(1, coeff_op);
  auto none = Module::getNoneOp(op);
  op->setOperand(2, none.getResult());
}

void tpu::Conv2DOp::weight_reorder_bf16_bm1684x() {
  conv_attr_t attr = {0};
  parseParam(&attr);
  auto filterOp = filter().getDefiningOp<top::WeightOp>();
  if (attr.is_dw || attr.groups > 1) {
    llvm_unreachable("depthwise should support !!");
  }
  auto filter_u16 = filterOp.read<uint16_t>();
  std::vector<int64_t> filter_shape = {attr.oc, attr.ic, attr.kh, attr.kw};
  filter_reorder(filter_u16, filter_shape);
  reshape_coeff_for_broadcast_channel(filter_u16, filter_shape);
  auto op = getOperation();
  OpBuilder builder(getContext());
  auto filter_type = Module::getStorageType(filter());
  auto new_filter_type = RankedTensorType::get(filter_shape, filter_type);
  auto newFilterOp =
      top::WeightOp::create(op, "_reordered", *filter_u16, new_filter_type);
  op->setOperand(1, newFilterOp);
}

void tpu::Conv2DOp::weight_reorder_f16_bm1684x() {
  weight_reorder_bf16_bm1684x();
}

void tpu::Conv2DOp::weight_reorder_f32_bm1684x() {
  conv_attr_t attr = {0};
  parseParam(&attr);
  auto op = getOperation();
  auto out_type = Module::getStorageType(output());
  // filter reorder
  auto filterOp = filter().getDefiningOp<top::WeightOp>();
  int64_t filter_shape[4];
  if (out_type.isF32()) {
    filter_shape[0] = 1;
    filter_shape[1] = attr.oc;
    filter_shape[2] = attr.ic / attr.groups;
    filter_shape[3] = attr.kh * attr.kw;
    auto new_type = RankedTensorType::get(filter_shape, out_type);
    filter().setType(new_type);
  } else {
    dump();
    llvm_unreachable("op type not support");
  }

  // bias op
  if (attr.has_bias) {
    auto biasOp = bias().getDefiningOp<top::WeightOp>();
    int64_t bias_shape[4] = {1, attr.oc, 1, 1};
    auto new_type = RankedTensorType::get(bias_shape, out_type);
    bias().setType(new_type);
  }
}

// ======================================
// GlobalGenInterface
// ======================================

#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
  uint64_t input_global_addr;
  uint64_t weight_global_addr;
  uint64_t bias_global_addr;
  uint64_t kzp_global_addr;
  uint64_t pad_global_addr;
  uint64_t output_global_addr;
  int batch_num;
  int input_c;
  int input_h;
  int input_w;
  int groups;
  int output_c;
  int kh;
  int kw;
  int stride_h;
  int stride_w;
  int dh;
  int dw;
  int pad_h;
  int pad_h_after;
  int pad_w;
  int pad_w_after;
  int has_bias;
  int if_relu;
  float upper_limit;
  int rshift;
  int idtype;
  int wdtype;
  int bdtype;
  int kdtype;
  int odtype;
  int round_mode;
  /**
   * merge_coeff:
   *    0: Not merge and not reshape weight and bias
   *    1. reshape and merge weight and bias as (bias, weight) align to (4, 1)
   * bytes for depthwise_fix8b or (4, 64) bytes for conv_fix8b
   *    2. reshape and merge weight, bias and requant as has bias-(requant,
   * bias, weight) align to (64, 4, 1) bytes for depthwise_fix8b or (64, 4, 64)
   * bytes for conv_fix8b or no bias-(requant, weight) align to (64, 1) bytes
   * for depthwise_fix8b or (64, 64) bytes for conv_fix8b
   */
  int merge_coeff;
  bool is_asym;
  bool kzp_is_const;
  bool pad_is_const;
  int kzp_val;
  int pad_val;
} conv_global_param_t;

typedef struct conv_common_spec {
  int32_t groups;
  int32_t input_c;
  int32_t output_c;
  int32_t kh;
  int32_t kw;
  int32_t stride_h;
  int32_t stride_w;
  int32_t dh;
  int32_t dw;
  int32_t pad_h_t;
  int32_t pad_h_b;
  int32_t pad_w_l;
  int32_t pad_w_r;
  int32_t has_bias;
  int32_t if_relu;
  float upper_limit;
  int32_t rshift;
  int32_t round_mode;
  int32_t is_asym;
  int32_t kzp_is_const;
  int32_t kzp_value;
  int32_t ipad_is_const;
  int32_t ipad_value;
  int32_t bias_sign; // For merged coeff
  int32_t use_3ic_optimize;
} conv_common_spec_t;

typedef struct conv_global_spec {
  conv_common_spec_t common;
  /**
   * merge_coeff:
   *    0: Not merge and not reshape weight and bias
   *    1. reshape and merge weight and bias as (bias, weight) align to (4, 1)
   * bytes for depthwise_fix8b or (4, 64) bytes for conv_fix8b
   *    2. reshape and merge weight, bias and requant as has bias-(requant,
   * bias, weight) align to (64, 4, 1) bytes for depthwise_fix8b or (64, 4, 64)
   * bytes for conv_fix8b or no bias-(requant, weight) align to (64, 1) bytes
   * for depthwise_fix8b or (64, 64) bytes for conv_fix8b
   */
  int32_t merge_coeff;
  int32_t weight_is_tensor;
} conv_global_spec_t;

typedef struct conv_local_spec {
  conv_common_spec_t common;
  uint32_t buffer_local_addr;
  int32_t result_add;
  int32_t unused_ht_for_input;
  int32_t unused_hb_for_input;
  int32_t unused_wl_for_input;
  int32_t unused_wr_for_input;
  int32_t group_one_conv;
  int32_t with_requant;
  int32_t merge_coeff;

  // For dynamic inference
  uint32_t concat_c;
  int32_t concat_c_idx;
  int32_t reference_id;
} conv_local_spec_t;

typedef struct conv_local_param {
  conv_local_spec_t spec;
} conv_local_param_t;

#ifdef __cplusplus
}
#endif

void tpu::Conv2DOp::codegen_global_bm1684x() {
  conv_attr_t attr = {0};
  parseParam(&attr);
  auto op = getOperation();
  auto input_spec = BM1684x::get_input_spec(op);
  auto output_spec = BM1684x::get_output_spec(op);
  conv_global_spec_t spec;
  memset(&spec, 0, sizeof(spec));
  auto &common = spec.common;
  common.input_c = attr.ic;
  common.output_c = attr.oc;
  common.if_relu = attr.do_relu;
  common.upper_limit = attr.relu_limit;
  common.kh = attr.kh;
  common.kw = attr.kw;
  common.dh = attr.dh;
  common.dw = attr.dw;
  common.stride_h = attr.sh;
  common.stride_w = attr.sw;
  common.groups = attr.groups;
  common.pad_h_t = attr.pht;
  common.pad_h_b = attr.phb;
  common.pad_w_l = attr.pwl;
  common.pad_w_r = attr.pwr;
  common.round_mode = ROUND_UP;
  common.has_bias = attr.has_bias;
  common.bias_sign = true;
  common.ipad_is_const = true;
  common.kzp_is_const = true;
  if (Quant::isUniformQuantized(input())) {
    auto in_qtype = Quant::getUniformQuantizedType(input());
    if (coeff_merged()) {
      spec.merge_coeff = 2;
      auto out_etype = Module::getStorageType(output());
      common.if_relu = out_etype.isUnsignedInteger(8);
    }
    common.is_asym = true;
    common.ipad_value = in_qtype.getZeroPoint();
    common.use_3ic_optimize = use_3ic_optimize();
  }
  BM1684x::instance().call_global_func("backend_api_conv_global", &spec,
                                       sizeof(spec), input_spec->data(),
                                       output_spec->data());
}

// ======================================
// LocalGenInterface
// ======================================

int64_t tpu::Conv2DOp::getBufferSize_bm1684x(
    int64_t in_lmem_bytes, int64_t out_lmem_bytes, int64_t in_nslice,
    int64_t in_hslice, int64_t out_nslice, int64_t out_hslice) {
  conv_attr_t p = {0};
  parseParam(&p);
  int64_t sz = 0;
  auto in_type = BM168x::getDataType(input());
  auto out_type = BM168x::getDataType(output());
  int in_type_len = BM168x::getFmtBytes(in_type);
  int out_type_len = BM168x::getFmtBytes(out_type);
  auto eu_num = BM1684x::instance().get_eu_num(in_type_len);
  auto npu_num = BM1684x::NPU_NUM;
  int oc_per_npu = ceiling_func(p.oc, npu_num);
  int ic_per_npu = ceiling_func(p.ic/p.groups, npu_num);
  int int32_size = out_lmem_bytes * sizeof(int32_t) / out_type_len;
  if (coeff_merged()) {
    sz += int32_size;
  }
  if (p.groups > 1) {
    sz += in_nslice * ic_per_npu * align_up(in_hslice * p.iw, eu_num) * in_type_len;
    sz += ic_per_npu * 2 * in_type_len;
  }

  if (p.is_dw) {
    sz += int32_size;
    sz += oc_per_npu * p.kh * p.kw;
  }
  int use_3ic = (use_3ic_optimize() & 0x3);
  if (use_3ic == 1) { // merge kh to ic
    sz += align_up(out_hslice * p.iw, eu_num) * in_nslice * in_type_len;
    sz += 64 * 2;
    sz += p.kh * in_type_len;
  } else if (use_3ic == 2) { // merge kw to ic
    sz += align_up(in_hslice * p.ow, eu_num) * in_nslice * in_type_len;
    sz += 64 * 2;
    sz += p.kw * in_type_len;
  } else if (use_3ic == 3) { // merge kh and kw to ic
    sz += align_up(out_hslice * p.ow, eu_num) * in_nslice * in_type_len;
    sz += 64 * 2;
    sz += p.kh * p.kw * in_type_len;
  }
  return sz;
}

void tpu::Conv2DOp::codegen_local_bm1684x(int64_t n_step, int64_t h_step) {
  conv_attr_t attr = {0};
  parseParam(&attr);
  auto op = getOperation();
  auto input_spec = BM1684x::get_input_spec(op);
  auto output_spec = BM1684x::get_output_spec(op);
  auto gi = getGroupInfo(n_step, h_step);
  auto in_gi = LocalGenInterface::getGroupInfo(input(), n_step, h_step);
  conv_local_param_t p;
  memset(&p, 0, sizeof(p));
  p.spec.buffer_local_addr = gi.buffer_addr;
  auto &common = p.spec.common;
  common.input_c = attr.ic;
  common.output_c = attr.oc;
  common.if_relu = attr.do_relu;
  common.upper_limit = attr.relu_limit;
  common.kh = attr.kh;
  common.kw = attr.kw;
  common.dh = attr.dh;
  common.dw = attr.dw;
  common.stride_h = attr.sh;
  common.stride_w = attr.sw;
  common.groups = attr.groups;
  common.pad_h_t = (in_gi.h_idx == 0 ? attr.pht : 0);
  common.pad_h_b = (in_gi.h_idx + in_gi.h_slice == attr.ih ? attr.phb : 0);
  common.pad_w_l = attr.pwl;
  common.pad_w_r = attr.pwr;
  common.round_mode = ROUND_UP;
  common.has_bias = attr.has_bias;
  common.bias_sign = true;
  common.ipad_is_const = true;
  common.kzp_is_const = true;
  if (Quant::isUniformQuantized(input())) {
    auto in_qtype = Quant::getUniformQuantizedType(input());
    if (coeff_merged()) {
      p.spec.merge_coeff = 2;
      p.spec.with_requant = 1;
      auto out_etype = Module::getStorageType(output());
      common.if_relu = out_etype.isUnsignedInteger(8);
    }
    common.is_asym = true;
    common.ipad_value = in_qtype.getZeroPoint();
    common.use_3ic_optimize = use_3ic_optimize();
  }
  local_sec_info_t sec_info;
  memset(&sec_info, 0, sizeof(sec_info));
  sec_info.n_slice = in_gi.n_slice;
  sec_info.h_slice = in_gi.h_slice;
  sec_info.h_idx = in_gi.h_idx;
  sec_info.is_h_split = !(in_gi.h_idx == 0 && in_gi.h_slice == attr.ih);
  // to be compatible with nntoolchain
  if (sec_info.is_h_split) {
    sec_info.h_idx = h_step == 0 ? -attr.pht : in_gi.h_idx;
    sec_info.h_slice = sec_info.h_idx < 0 ? sec_info.h_slice - sec_info.h_idx
                                          : sec_info.h_slice;
    sec_info.h_slice = sec_info.h_slice + common.pad_h_b;
  }
  sec_info.w_slice = attr.iw;
  sec_info.out_n_slice = gi.n_slice;
  sec_info.out_h_idx = gi.h_idx;
  sec_info.out_h_slice = gi.h_slice;
  sec_info.out_w_slice = attr.ow;
  BM1684x::instance().call_local_func("backend_api_conv_local", &p, sizeof(p),
                                      &sec_info, input_spec->data(),
                                      output_spec->data());
}