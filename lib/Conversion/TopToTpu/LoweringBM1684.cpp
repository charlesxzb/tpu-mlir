//===----------------------------------------------------------------------===//
//
// Copyright (C) 2022 Sophgo Technologies Inc.  All rights reserved.
//
// TPU-MLIR is licensed under the 2-Clause BSD License except for the
// third-party components.
//
//===----------------------------------------------------------------------===//

#include "tpu_mlir/Conversion/TopToTpu/LoweringBM1684.h"

namespace tpu_mlir {
namespace bm1684 {

void populateTopToTpuConversionPatterns(RewritePatternSet *patterns) {
  patterns->add<
      // clang-format off
      AbsLowering,
      AddLowering,
      ArgLowering,
      AvgPoolLowering,
      ClipLowering,
      ConcatLowering,
      ConvLowering,
      CustomLowering,
      Depth2SpaceLowering,
      FloorLowering,
      GatherLowering,
      GatherNDLowering,
      GridSamplerLowering,
      GroupNormLowering,
      MatMulLowering,
      MaxPoolLowering,
      MulLowering,
      LogLowering,
      NmsLowering,
      PadLowering,
      PermuteLowering,
      ReciprocalLowering,
      ReduceLowering,
      ReluLowering,
      ReshapeLowering,
      ScatterNDLowering,
      SliceLowering,
      SoftmaxLowering,
      SigmoidLowering,
      SiLULowering,
      SqrtLowering,
      TanhLowering,
      TileLowering,
      TopKLowering,
      UpsampleLowering,
      InterpLowering,
      ReduceLowering,
      HardSigmoidLowering,
      HardSwishLowering,
      AddConstLowering,
      MulConstLowering,
      SubLowering,
      LayerNormLowering,
      SwapDimInnerLowering,
      LRNLowering,
      MinLowering,
      MaxLowering,
      DeconvLowering,
      ExpLowering,
      PReluLowering,
      LSTMLowering,
      LeakyReluLowering,
      GELULowering,
      PowLowering,
      DivLowering,
      CompareLowering
      // clang-format on
      >(patterns->getContext());
}
} // namespace bm1684
} // namespace tpu_mlir
