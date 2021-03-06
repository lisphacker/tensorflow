/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// This file contains legalizations common to mapping both TensorFlow and
// TensorFlow Lite to TOSA. It operates generically on ops and does not have
// a hard reference on either dialect.
//
// Conversion functions return llvm::None on a legalization failure or a
// legalized value on success.  Callers must check for presence of an
// llvm::Optional value after each call.

#include "tensorflow/compiler/mlir/tosa/transforms/legalize_common.h"

#include <climits>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <numeric>

#include "llvm/Support/FormatVariadic.h"
#include "mlir/Dialect/Quant/QuantTypes.h"  // from @llvm-project
#include "mlir/Dialect/Tosa/IR/TosaOps.h"  // from @llvm-project
#include "mlir/IR/Matchers.h"  // from @llvm-project
#include "mlir/IR/PatternMatch.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/tosa/transforms/legalize_utils.h"

namespace mlir {
namespace tosa {

// Copied Nudge implementation from
// tensorflow/core/kernels/fake_quant_ops_functor.h.
// Suggested approach to avoid significant TensorFlow
// build dependency.
void tensorflow_nudge(const float min, const float max, const int quant_min,
                      const int quant_max, float* nudged_min, float* nudged_max,
                      float* scale) {
  const float quant_min_float = static_cast<float>(quant_min);
  const float quant_max_float = static_cast<float>(quant_max);
  *scale = (max - min) / (quant_max_float - quant_min_float);
  const float zero_point_from_min = quant_min_float - min / *scale;
  const uint16_t nudged_zero_point = [zero_point_from_min, quant_min,
                                      quant_min_float, quant_max,
                                      quant_max_float] {
    if (zero_point_from_min < quant_min_float) {
      return static_cast<uint16_t>(quant_min);
    }
    if (zero_point_from_min > quant_max_float) {
      return static_cast<uint16_t>(quant_max);
    }
    return static_cast<uint16_t>(std::round(zero_point_from_min));
  }();
  *nudged_min = (quant_min_float - nudged_zero_point) * (*scale);
  *nudged_max = (quant_max_float - nudged_zero_point) * (*scale);
}

// Lowers the Unpack operator to TOSA
llvm::Optional<ValueRange> convertUnpackOp(PatternRewriter& rewriter,
                                           Operation* op, Value input_value,
                                           int32_t axis) {
  RankedTensorType input_type =
      input_value.getType().dyn_cast<RankedTensorType>();
  if (!input_type) return llvm::None;

  auto input_shape = input_type.getShape();
  int64_t input_rank = input_shape.size();

  SmallVector<Value, 4> results_vec;

  // Negative axis allowed as long as it's within [-input_rank, input_rank).
  if (axis < 0) axis += input_rank;

  assert(axis >= 0 && axis < input_shape.size());

  // A list of the output types for each slice op
  SmallVector<Type, 4> outs_type_vec;

  // Step 1: transpose 'axis' to leftmost dimension.
  Value transposed_input_value;
  if (axis != 0) {
    SmallVector<int32_t, 8> perm_vec;
    SmallVector<int64_t, 2> a1_transpose_shape(input_rank);

    perm_vec.push_back(axis);
    for (int i = 0; i < input_rank; i++) {
      if (i == axis) continue;
      perm_vec.push_back(i);
    }

    Value a1_transpose_perm =
        get1DConstTensor<tosa::ConstOp, int32_t>(rewriter, op, perm_vec);

    for (int i = 0; i < input_rank; i++) {
      a1_transpose_shape[i] = input_shape[perm_vec[i]];
    }

    auto a1_transpose_op = rewriter.create<tosa::TransposeOp>(
        op->getLoc(),
        RankedTensorType::get(ArrayRef<int64_t>(a1_transpose_shape),
                              input_type.getElementType()),
        input_value, a1_transpose_perm);

    transposed_input_value = a1_transpose_op.getResult();
  } else {
    // Do nothing if axis is already at leftmost dimension.
    transposed_input_value = input_value;
  }

  // Step 2: slice [N, A, B, C] into N [A, B, C].
  RankedTensorType transposed_input_type =
      transposed_input_value.getType().dyn_cast<RankedTensorType>();
  if (!transposed_input_type) return llvm::None;

  auto transposed_input_shape = transposed_input_type.getShape();
  int64_t transposed_input_rank = transposed_input_shape.size();

  for (int i = 0; i < transposed_input_shape[0]; i++) {
    SmallVector<int64_t, 4> begin_vals, size_vals, shape_vals;

    for (int j = 0; j < transposed_input_rank; j++) {
      if (j == 0) {
        begin_vals.push_back(i);
        size_vals.push_back(1);
      } else {
        begin_vals.push_back(0);
        size_vals.push_back(transposed_input_shape[j]);
        shape_vals.push_back(transposed_input_shape[j]);
      }
    }

    ArrayAttr begin = rewriter.getI64ArrayAttr(begin_vals);
    ArrayAttr size = rewriter.getI64ArrayAttr(size_vals);

    auto a2_slice_op = rewriter.create<tosa::SliceOp>(
        op->getLoc(),
        RankedTensorType::get(ArrayRef<int64_t>(size_vals),
                              transposed_input_type.getElementType()),
        transposed_input_value, begin, size);

    auto a3_reshape_op = rewriter.create<tosa::ReshapeOp>(
        op->getLoc(),
        RankedTensorType::get(ArrayRef<int64_t>(shape_vals),
                              transposed_input_type.getElementType()),
        a2_slice_op.getResult(), rewriter.getI64ArrayAttr(shape_vals));

    outs_type_vec.push_back(RankedTensorType::get(
        ArrayRef<int64_t>(shape_vals), transposed_input_type.getElementType()));

    results_vec.push_back(a3_reshape_op.getResult());
  }

  // Combine the sequence of tosa.slice() ops into a list
  // using the IdentityN operator.
  return rewriter
      .create<tosa::IdentityNOp>(op->getLoc(), ArrayRef<Type>(outs_type_vec),
                                 results_vec)
      .getResults();
}

// Lowers the Select operator to TOSA.
llvm::Optional<Value> convertSelectOp(PatternRewriter& rewriter, Operation* op,
                                      Value result_value, Value condition_value,
                                      Value x_value, Value y_value) {
  RankedTensorType result_type =
      result_value.getType().dyn_cast<RankedTensorType>();
  RankedTensorType condition_type =
      condition_value.getType().dyn_cast<RankedTensorType>();
  RankedTensorType x_type = x_value.getType().dyn_cast<RankedTensorType>();
  RankedTensorType y_type = y_value.getType().dyn_cast<RankedTensorType>();

  if (!result_type || !condition_type || !x_type || !y_type) {
    op->emitOpError("Select: failed ranked tensor type check");
    return llvm::None;
  }

  // First check whether we need to reshape the condition to match
  // the same rank as the then/else clauses.
  if (result_type.getRank() == condition_type.getRank()) {
    // Nothing to reshape.
    return rewriter
        .create<tosa::SelectOp>(op->getLoc(), result_type, condition_value,
                                x_value, y_value)
        .getResult();
  }

  // Need to reshape the condition.
  SmallVector<int64_t, 8> new_cond_dims(
      result_type.getRank() - condition_type.getRank(), 1);

  for (int i = 0; i < condition_type.getRank(); i++) {
    new_cond_dims.push_back(condition_type.getShape()[i]);
  }

  auto reshape_op = rewriter.create<tosa::ReshapeOp>(
      op->getLoc(),
      RankedTensorType::get(ArrayRef<int64_t>(new_cond_dims),
                            condition_type.getElementType()),
      condition_value, rewriter.getI64ArrayAttr(new_cond_dims));

  return rewriter
      .create<tosa::SelectOp>(op->getLoc(), result_type, reshape_op, x_value,
                              y_value)
      .getResult();
}

// Lowers the ZerosLike operator to TOSA by creating a constant
// of the desired type and shape.
llvm::Optional<Value> convertZerosLikeOp(PatternRewriter& rewriter,
                                         Operation* op, Value result,
                                         Value input) {
  RankedTensorType result_type = result.getType().dyn_cast<RankedTensorType>();
  if (!result_type) {
    op->emitOpError("Zeroslike: result not ranked tensor type");
    return llvm::None;
  }

  RankedTensorType input_type = input.getType().dyn_cast<RankedTensorType>();
  if (!input_type) {
    op->emitOpError("Zeroslike: input not ranked tensor type");
    return llvm::None;
  }

  auto input_shape = input_type.getShape();

  ShapedType zero_type =
      RankedTensorType::get(input_shape, input_type.getElementType());
  Attribute zero_attr = rewriter.getZeroAttr(zero_type);

  return rewriter
      .create<tosa::ConstOp>(op->getLoc(), zero_type,
                             zero_attr.cast<ElementsAttr>())
      .getResult();
}

// Lowers the Mul operator to TOSA.  For quantized types, this requires
// inserting rescale operators before and after the operation.
llvm::Optional<Value> convertMultiplyOp(PatternRewriter& rewriter,
                                        Operation* op, Value output_val,
                                        Value input_lhs_val,
                                        Value input_rhs_val) {
  RankedTensorType input_lhs_type =
      input_lhs_val.getType().dyn_cast<RankedTensorType>();
  RankedTensorType input_rhs_type =
      input_rhs_val.getType().dyn_cast<RankedTensorType>();
  RankedTensorType output_type =
      output_val.getType().dyn_cast<RankedTensorType>();
  // Not a ranked tensor output
  if (!input_lhs_type || !input_rhs_type || !output_type) return llvm::None;

  bool input_lhs_is_qtype =
      input_lhs_type.getElementType().isa<mlir::quant::UniformQuantizedType>();
  bool input_rhs_is_qtype =
      input_rhs_type.getElementType().isa<mlir::quant::UniformQuantizedType>();
  bool output_is_qtype =
      output_type.getElementType().isa<mlir::quant::UniformQuantizedType>();

  if (input_lhs_is_qtype != output_is_qtype ||
      input_rhs_is_qtype != output_is_qtype) {
    op->emitOpError(
        "ConvertMultiplyOp: input/output tensor should "
        "be all quantized or all floating-point");
    return llvm::None;
  }

  Value output;
  if (output_is_qtype) {
    RankedTensorType rescale_type =
        RankedTensorType::get(output_type.getShape(), rewriter.getI32Type());
    auto input_lhs_qtype = input_lhs_type.getElementType()
                               .cast<mlir::quant::UniformQuantizedType>();
    auto input_rhs_qtype = input_rhs_type.getElementType()
                               .cast<mlir::quant::UniformQuantizedType>();
    auto output_qtype =
        output_type.getElementType().cast<mlir::quant::UniformQuantizedType>();
    double in_lhs_scale = input_lhs_qtype.getScale();
    double in_rhs_scale = input_rhs_qtype.getScale();
    double output_scale = output_qtype.getScale();

    double output_rescale_scale = in_lhs_scale * in_rhs_scale / output_scale;

    Value op1_rescale_lhs = buildRescaleToInt32(
        rewriter, op, input_lhs_val, 1.0f, input_lhs_qtype.getZeroPoint());
    Value op2_rescale_rhs = buildRescaleToInt32(
        rewriter, op, input_rhs_val, 1.0f, input_rhs_qtype.getZeroPoint());
    auto op3_mul_op1_op2 = rewriter.create<tosa::MulOp>(
        op->getLoc(), rescale_type, op1_rescale_lhs, op2_rescale_rhs, 0);
    return buildRescaleFromInt32(
        rewriter, op, output_type, op3_mul_op1_op2.getResult(),
        output_rescale_scale, output_qtype.getZeroPoint());
  }

  return rewriter
      .create<tosa::MulOp>(op->getLoc(), output_type, input_lhs_val,
                           input_rhs_val, 0)
      .getResult();
}

// Lowers the SquaredDifference operator to TOSA.
llvm::Optional<Value> convertSquaredDifferenceOp(PatternRewriter& rewriter,
                                                 Operation* op, Value result,
                                                 Value x, Value y) {
  // Squared-difference is (x-y)*(x-y).
  // This lowering calculates the difference and multiplies.
  RankedTensorType result_type = result.getType().dyn_cast<RankedTensorType>();
  if (!result_type) {
    op->emitOpError("SquaredDifference: result not ranked tensor type");
    return llvm::None;
  }

  RankedTensorType x_type = x.getType().dyn_cast<RankedTensorType>();
  RankedTensorType y_type = y.getType().dyn_cast<RankedTensorType>();
  if (!x_type || !y_type) {
    op->emitOpError("SquaredDifference: inputs not ranked tensor type");
    return llvm::None;
  }

  auto sub_op = rewriter.create<tosa::SubOp>(op->getLoc(), result_type, x, y);
  return rewriter
      .create<tosa::MulOp>(op->getLoc(), result_type, sub_op.getResult(),
                           sub_op.getResult(), 0)
      .getResult();
}

// Lowers the Round operator to TOSA.
llvm::Optional<Value> convertRoundOp(PatternRewriter& rewriter, Operation* op,
                                     Value result, Value input) {
  // Implements banker's rounding by calculating floor(input + 0.5).
  RankedTensorType result_type = result.getType().dyn_cast<RankedTensorType>();
  if (!result_type) {
    op->emitOpError("Round: result not ranked tensor type");
    return llvm::None;
  }

  RankedTensorType input_type = input.getType().dyn_cast<RankedTensorType>();
  if (!input_type) {
    op->emitOpError("Round: input not ranked tensor type");
    return llvm::None;
  }

  auto add_op = rewriter.create<tosa::AddOp>(
      op->getLoc(), result_type, input,
      getTosaConstTensorSingleF32(rewriter, op, 0.5));

  return rewriter
      .create<tosa::FloorOp>(op->getLoc(), result_type, add_op.getResult())
      .getResult();
}

// Lowers SpaceToBatchND to TOSA.
llvm::Optional<Value> convertSpaceToBatchNDOp(PatternRewriter& rewriter,
                                              Operation* op, Value result_value,
                                              Value input_value,
                                              Value block_shape_value,
                                              Value paddings_value) {
  /////////////////////////////////////////////////
  // Operator: output = SpaceToBatchND(input, block_shape, paddings)
  // Lowering:
  //
  // SpaceToBatch input tensors are broken into three pieces:
  //   (a) batch dimension (N in NHWC)
  //   (b) input being transformed to batch dimension (typically H, W in NHWC)
  //   (c) remainder of input (typically C in NHWC)
  //
  // Step 0. Generate padding constant for the first reshape.
  //   No padding on the batch dimension
  //   The input paddings array is addressed as [input_rank][2]
  //   No padding on the remaining dimensions
  //
  //  a0_pad_const = tosa.const(input=Tensor<input_rank, 2>)
  //
  // Step 1. Pad the input tensor
  //
  //  a1_pad_input_op = tosa.pad(input=input, shape=a0_pad_const_op)
  //
  // Step 2. Reshape the padded structure of shape padded_shape to
  // [batch + padded_shape[1] / block_shape[0], block_shape[0], ...
  //    padded_shape[M] / block_shape[M-1], block_shape[M-1]] +
  //    remaining_shape
  //
  // block_rank = M (number of elements in block_shape)
  // New rank: input_rank + block_rank
  //
  //  a2_reshape_a1_op = tosa.reshape(input=a1_pad_input_op, shape=a2_shape)
  //
  // Step 3. Transpose dimensions to:
  //  block-shape +
  //  [batch] +
  //  [padded_shape[1] / block_shape[0],
  // ...
  //  [padded_shape[M] / block_shape[M-1]] +
  //  remaining_shape
  //
  // a3_transpose_a2_op = tosa.tranpose(input=a2_reshape_a1_op,
  // perms=a3_perm)
  //
  // Step 4. Reshape the transposed tensor to flatten block_shape stuff
  // into the batch dimension with the following shape:
  // [ batch * prod(block_shape)] +
  // [ padded_shape[1] / block_shape[0],
  //   ...,
  // padded_shape[M] / block_shape[M-1]] +
  // remaining_shape
  //
  //  a4_reshape_a3_op = tosa.reshape(input=a3_tranpose_a2_op,
  //  shape=a3_shape)
  //

  RankedTensorType result_type =
      result_value.getType().dyn_cast<RankedTensorType>();
  RankedTensorType input_type =
      input_value.getType().dyn_cast<RankedTensorType>();
  RankedTensorType block_shape_type =
      block_shape_value.getType().dyn_cast<RankedTensorType>();
  RankedTensorType paddings_type =
      paddings_value.getType().dyn_cast<RankedTensorType>();

  // Not a ranked tensor output.
  if (!result_type) {
    op->emitOpError("SpaceToBatchND: result type not ranked tensor");
    return llvm::None;
  }
  if (!input_type) {
    op->emitOpError("SpaceToBatchND: input type not ranked tensor");
    return llvm::None;
  }
  if (!block_shape_type) {
    op->emitOpError("SpaceToBatchND: block shape type not ranked tensor");
    return llvm::None;
  }
  if (!paddings_type) {
    op->emitOpError("SpaceToBatchND: paddings type not ranked tensor");
    return llvm::None;
  }

  // Follow implementation in
  // tensorflow/compiler/tf2xla/kernels/spacetobatch_op.cc

  // So, to figure out the spatial_shape, remove the batch dimension and
  // then use the next block_rank dimensions.  The remaining dimensions are
  // remaining_shape.

  auto block_shape = block_shape_type.getShape();
  auto input_shape = input_type.getShape();

  int block_rank = block_shape[0];
  int batch_size = input_shape[0];
  int input_rank = input_type.getRank();
  int remaining_shape_rank = input_rank - block_rank - 1;
  int block_num_elems = 1;
  int padding_sum = 0;

  ElementsAttr block_shape_elems;
  ElementsAttr paddings_elems;

  if (!matchPattern(block_shape_value, m_Constant(&block_shape_elems)))
    return llvm::None;

  if (!matchPattern(paddings_value, m_Constant(&paddings_elems)))
    return llvm::None;

  SmallVector<int32_t, 2> a0_pad_const(2 * (input_rank));
  SmallVector<int64_t, 2> padded_shape(input_rank);

  // 1. Pad based on paddings operand.  No padding on the batch dimension.
  // The a0_pad_const array is addressed as [input_rank][2], but
  // it is flattened to a 1D array because LLVM appears to only accept 1D.
  //
  // padded_shape[] is the shape of the padded output of step a1.
  // The name is retained for consistency with the TF reference code.
  padded_shape[0] = input_shape[0];

  // Batch dimension padding
  a0_pad_const[0] = 0;
  a0_pad_const[1] = 0;

  // This iterator seems to be the only reliable way to get
  // int values out of a multi-dimensional ElementsAttr.
  int idx = 0;

  for (auto i : paddings_elems.getValues<IntegerAttr>()) {
    a0_pad_const[idx + 2] = i.getInt();
    padding_sum += i.getInt();
    idx++;
  }

  // Insert padding on the spatial shape dimensions
  for (int i = 0; i < block_rank; i++) {
    int32_t lo_pad = a0_pad_const[2 * (i + 1) + 0];
    int32_t hi_pad = a0_pad_const[2 * (i + 1) + 1];

    padded_shape[i + 1] = input_shape[i + 1] + lo_pad + hi_pad;
  }

  // No padding on the remaining_shape dimensions
  for (int i = 0; i < remaining_shape_rank; i++) {
    a0_pad_const[2 * (i + block_rank + 1) + 0] = 0;
    a0_pad_const[2 * (i + block_rank + 1) + 1] = 0;
    padded_shape[i + block_rank + 1] = input_shape[i + block_rank + 1];
  }

  RankedTensorType a0_pad_const_attr_type =
      RankedTensorType::get({(input_rank), 2}, rewriter.getIntegerType(32));

  // Create a const op to generate the tensor type for the input padding array
  auto a0_pad_const_op = rewriter.create<tosa::ConstOp>(
      op->getLoc(), a0_pad_const_attr_type,
      DenseElementsAttr::get(a0_pad_const_attr_type,
                             llvm::makeArrayRef<int32_t>(a0_pad_const)));

  auto a1_pad_input_op = rewriter.create<tosa::PadOp>(
      op->getLoc(),
      RankedTensorType::get(ArrayRef<int64_t>(padded_shape),
                            result_type.getElementType()),
      input_value, a0_pad_const_op.getResult());

  // 2. Reshape the padded structure of shape padded_shape to
  // [batch + padded_shape[1] / block_shape[0], block_shape[0], ...
  //    padded_shape[M] / block_shape[M-1], block_shape[M-1]] +
  //    remaining_shape

  // block_rank = M (number of elements in block_shape)
  // New rank: input_rank + block_rank
  SmallVector<int64_t, 2> a2_shape(1 + block_rank * 2 + remaining_shape_rank);

  // First dimension is batch.
  a2_shape[0] = input_type.getShape()[0];
  for (int i = 0; i < block_rank; i++) {
    int32_t block_shape_val =
        rewriter
            .getI32IntegerAttr(
                block_shape_elems.getValue<IntegerAttr>(i).getInt())
            .getInt();
    a2_shape[1 + i * 2 + 0] = padded_shape[1 + i] / block_shape_val;
    a2_shape[1 + i * 2 + 1] = block_shape_val;
    block_num_elems *= block_shape_val;
  }

  // Copy in the remaining block shape.
  for (int i = 0; i < remaining_shape_rank; i++) {
    a2_shape[1 + block_rank * 2 + i] = input_shape[1 + block_rank + i];
  }

  auto a2_reshape_a1_op = rewriter.create<tosa::ReshapeOp>(
      op->getLoc(),
      RankedTensorType::get(ArrayRef<int64_t>(a2_shape),
                            result_type.getElementType()),
      a1_pad_input_op.getResult(), rewriter.getI64ArrayAttr(a2_shape));

  // 3. Transpose dimensions to:
  //  block-shape +
  //  [batch] +
  //  [padded_shape[1] / block_shape[0],
  // ...
  //  [padded_shape[M] / block_shape[M-1]] +
  //  remaining_shape
  int32_t a2_reshape_a1_rank =
      a2_reshape_a1_op.getResult().getType().cast<RankedTensorType>().getRank();
  SmallVector<int32_t, 8> a3_perm(a2_reshape_a1_rank);
  SmallVector<int64_t, 2> a3_transpose_shape(a2_reshape_a1_rank);

  for (int i = 0; i < block_rank; i++) {
    a3_perm[i] = 1 + 2 * i + 1;
    a3_perm[block_rank + 1 + i] = 1 + 2 * i;
  }
  a3_perm[block_rank] = 0;
  for (int i = 1 + block_rank * 2; i < a2_reshape_a1_rank; i++) {
    a3_perm[i] = i;
  }

  for (int i = 0; i < a3_transpose_shape.size(); i++) {
    a3_transpose_shape[i] = a2_shape[a3_perm[i]];
  }

  Value a3_transpose_const =
      get1DConstTensor<tosa::ConstOp, int32_t>(rewriter, op, a3_perm);

  auto a3_transpose_a2_op = rewriter.create<tosa::TransposeOp>(
      op->getLoc(),
      RankedTensorType::get(ArrayRef<int64_t>(a3_transpose_shape),
                            result_type.getElementType()),
      a2_reshape_a1_op.getResult(), a3_transpose_const);

  // 4. Reshape the transposed tensor to flatten block_shape
  // into the batch dimension with the following shape:
  // [ batch * prod(block_shape)] +
  // [ padded_shape[1] / block_shape[0],
  //   ...,
  // padded_shape[M] / block_shape[M-1]] +
  // remaining_shape
  SmallVector<int64_t, 2> a4_reshape_shape(input_rank);

  // Batch
  a4_reshape_shape[0] = batch_size * block_num_elems;

  // padded shape / block_shape.
  for (int i = 0; i < block_rank; i++) {
    int32_t block_shape_val =
        rewriter
            .getI32IntegerAttr(
                block_shape_elems.getValue<IntegerAttr>(i).getInt())
            .getInt();
    a4_reshape_shape[i + 1] = padded_shape[i + 1] / block_shape_val;
  }

  // Copy in remainder shape.
  for (int i = 0; i < remaining_shape_rank; i++) {
    a4_reshape_shape[1 + block_rank + i] = input_shape[1 + block_rank + i];
  }

  return rewriter
      .create<tosa::ReshapeOp>(op->getLoc(), result_type,
                               a3_transpose_a2_op.getResult(),
                               rewriter.getI64ArrayAttr(a4_reshape_shape))
      .getResult();
}

// Lowers BatchToSpaceND to TOSA.
llvm::Optional<Value> convertBatchToSpaceNDOp(PatternRewriter& rewriter,
                                              Operation* op, Value result_value,
                                              Value input_value,
                                              Value block_shape_value,
                                              Value crops_value) {
  /////////////////////////////////////////////////
  // Operator: output = BatchToSpaceND(input, block_shape, clips)
  // Lowering:
  //
  // BatchToSpace input tensors are broken into three pieces:
  //   (a) batch dimension (N in NHWC)
  //   (b) input being transformed from batch dimension (typically H, W in
  //   NHWC)
  //   (c) remainder of input (typically C in NHWC)
  //
  // Step 1. Reshape input to:
  // [block_shape[0],
  // ...
  // [block_shape[M-1],
  // [batch / prod(block_shape)]
  // [input_shape[1],
  // ...
  // [input_shape[N-1]
  //
  // a1_reshape_input_op = tosa.reshape(input=input, shape=a1_shape)
  //
  // Step 2. Permute to shape
  // [ batch / prod(block_shape) ],
  // [ input_shape[1] ], [ block_shape[1] ]
  //  ...
  // [ input_shape[M] ], [ block_shape[M-1]
  // + remaining_input_shapes input_shape[M .. N-1]
  //
  // a2_transpose_a1 = tosa.transpose(input=a1_reshape_input_op,
  // shape=a2_shape)
  //
  // Step 3. Reshape to:
  // [ batch / prod(block_shape) ],
  // [input_shape[1] * block_shape[0] ],
  //    ..
  // [input_shape[M * block_shape[M-1],
  // + remaining input shapes [input_shape[M+1.. N-1]]
  //
  // a3_reshape_a2 = tosa.reshape(input=a2_transpose_a1, shape=a3_shape)
  //
  // Step 4. Crop the start/end dimensions according to crops of the
  // a3_reshape_a2 shape
  //
  // a4_slice_a3 = tosa.slice(input=a3_reshape_a2, start=a4_start,
  // size=a4_size)

  RankedTensorType result_type =
      result_value.getType().dyn_cast<RankedTensorType>();
  RankedTensorType input_type =
      input_value.getType().dyn_cast<RankedTensorType>();
  RankedTensorType block_shape_type =
      block_shape_value.getType().dyn_cast<RankedTensorType>();
  RankedTensorType crops_type =
      crops_value.getType().dyn_cast<RankedTensorType>();

  if (!result_type) {
    op->emitOpError("BatchToSpaceND: result type not ranked tensor");
    return llvm::None;
  }
  if (!input_type) {
    op->emitOpError("BatchToSpaceND: input type not ranked tensor");
    return llvm::None;
  }
  if (!block_shape_type) {
    op->emitOpError("BatchToSpaceND: block shape type not ranked tensor");
    return llvm::None;
  }
  if (!crops_type) {
    op->emitOpError("BatchToSpaceND: crops type not ranked tensor");
    return llvm::None;
  }

  // Another 4-step process
  int block_rank = block_shape_type.getShape()[0];
  int input_rank = input_type.getRank();
  int crops_dims = crops_type.getShape()[0];
  int remaining_shape_rank = input_rank - block_rank - 1;
  auto input_shape = input_type.getShape();

  ElementsAttr block_shape_elems;
  ElementsAttr crops_elems;

  if (!matchPattern(block_shape_value, m_Constant(&block_shape_elems))) {
    op->emitOpError("BatchToSpaceND: block_shape not a constant");
    return llvm::None;
  }

  if (!matchPattern(crops_value, m_Constant(&crops_elems))) {
    op->emitOpError("BatchToSpaceND: crops not a constant");
    return llvm::None;
  }

  SmallVector<int64_t, 4> block_shape(block_rank);
  SmallVector<std::pair<int64_t, int64_t>, 4> crops(crops_dims);

  // Extract values for block_shape and crops now.
  int block_num_elems = 1;
  for (int i = 0; i < block_rank; i++) {
    int block_shape_val =
        rewriter
            .getI32IntegerAttr(
                block_shape_elems.getValue<IntegerAttr>(i).getInt())
            .getInt();
    block_num_elems *= block_shape_val;
    block_shape[i] = block_shape_val;
  }

  // This iterator seems to be the only reliable way to get
  // int values out of a multi-dimensional ElementsAttr
  SmallVector<int32_t, 2> crops_const(2 * (crops_dims));
  int idx = 0;
  for (auto i : crops_elems.getValues<IntegerAttr>()) {
    crops_const[idx++] = i.getInt();
  }

  for (int i = 0; i < crops_dims; i++) {
    int crops_lo = crops_const[i * crops_dims + 0];
    int crops_hi = crops_const[i * crops_dims + 1];
    crops[i] = std::make_pair(crops_lo, crops_hi);
  }

  // Step 1. Reshape input to:
  // [block_shape[0],
  // ...
  // [block_shape[M-1],
  // [batch / prod(block_shape)]
  // [input_shape[1],
  // ...
  // [input_shape[N-1]
  SmallVector<int64_t, 2> a1_shape(block_rank + input_rank);

  for (int i = 0; i < block_rank; i++) a1_shape[i] = block_shape[i];

  a1_shape[block_rank] = input_shape[0] / block_num_elems;

  for (int i = 0; i < input_rank - 1; i++)
    a1_shape[i + block_rank + 1] = input_shape[i + 1];

  auto a1_reshape_input_op = rewriter.create<tosa::ReshapeOp>(
      op->getLoc(),
      RankedTensorType::get(ArrayRef<int64_t>(a1_shape),
                            result_type.getElementType()),
      input_value, rewriter.getI64ArrayAttr(a1_shape));

  // 2. Permute to shape
  // [ batch / prod(block_shape) ],
  // [ input_shape[1] ], [ block_shape[0] ]
  //  ...
  // [ input_shape[M] ], [ block_shape[M-1]
  // + remaining_input_shapes input_shape[M+1 .. N-1]

  // 2a. calculate the permutation
  SmallVector<int32_t, 8> a2_perm(block_rank + input_rank);
  SmallVector<int64_t, 2> a2_transpose_shape(block_rank + input_rank);

  a2_perm[0] = block_rank;
  for (int i = 0; i < block_rank; i++) {
    a2_perm[1 + i * 2 + 0] = block_rank + 1 + i;
    a2_perm[1 + i * 2 + 1] = i;
  }

  for (int i = 0; i < remaining_shape_rank; i++) {
    a2_perm[1 + 2 * block_rank + i] = 1 + 2 * block_rank + i;
  }

  // 2b. calculate the a2_permuted shape
  for (int i = 0; i < (block_rank + input_rank); i++) {
    a2_transpose_shape[i] = a1_shape[a2_perm[i]];
  }

  Value a2_transpose_perm =
      get1DConstTensor<tosa::ConstOp, int32_t>(rewriter, op, a2_perm);
  auto a2_transpose_a1_op = rewriter.create<tosa::TransposeOp>(
      op->getLoc(),
      RankedTensorType::get(ArrayRef<int64_t>(a2_transpose_shape),
                            result_type.getElementType()),
      a1_reshape_input_op.getResult(), a2_transpose_perm);

  // Step 3. Reshape to:
  // [ batch / prod(block_shape) ],
  // [input_shape[1] * block_shape[0] ],
  //    ..
  // [input_shape[M * block_shape[M-1],
  // + remaining input shapes [input_shape[M+1.. N-1]]
  SmallVector<int64_t, 2> a4_shape(input_rank);

  a4_shape[0] = input_shape[0] / block_num_elems;
  for (int i = 0; i < block_rank; i++) {
    a4_shape[1 + i] = input_shape[i + 1] * block_shape[i];
  }
  for (int i = 0; i < remaining_shape_rank; i++) {
    a4_shape[1 + block_rank + i] = input_shape[block_rank + 1 + i];
  }

  auto a3_reshape_a2 = rewriter.create<tosa::ReshapeOp>(
      op->getLoc(),
      RankedTensorType::get(ArrayRef<int64_t>(a4_shape),
                            result_type.getElementType()),
      a2_transpose_a1_op.getResult(), rewriter.getI64ArrayAttr(a4_shape));

  // 4. Crop the start/end dimensions on 'spatial dimension' according to
  // crops
  // Use a slice operator to do the cropping.
  //
  // Calculate a beginning point and a size:
  // - Begin is the origin, offset by the lo crop amount in each dimension
  // - Size is the reshaped tensor size, minus the quantity (lo + hi) for each
  // dimension
  SmallVector<int64_t, 4> a4_begin_vals(input_rank), a4_size_vals(input_rank);

  for (int i = 0; i < input_rank; i++) {
    // Batch dimension and remaining dimensions.
    if (i == 0 || i > crops_dims) {
      a4_begin_vals[i] = 0;
      a4_size_vals[i] = result_type.getShape()[i];
    } else {
      // Spatial dimension.
      assert(i - 1 >= 0 && i - 1 < crops_dims);
      a4_begin_vals[i] = crops[i - 1].first;
      a4_size_vals[i] = a4_shape[i] - crops[i - 1].first - crops[i - 1].second;
    }
  }

  return rewriter
      .create<tosa::SliceOp>(
          op->getLoc(),
          RankedTensorType::get(ArrayRef<int64_t>(a4_size_vals),
                                result_type.getElementType()),
          a3_reshape_a2.getResult(), rewriter.getI64ArrayAttr(a4_begin_vals),
          rewriter.getI64ArrayAttr(a4_size_vals))
      .getResult();
}

// Lowers ExpandDims to TOSA.
llvm::Optional<Value> convertExpandDimsOp(PatternRewriter& rewriter,
                                          Operation* op, Value result_value,
                                          Value input_value, Value dim_value) {
  // Lowers to a reshape op with 1's inserted in the appropriate dimensions.
  RankedTensorType output_type =
      result_value.getType().dyn_cast<RankedTensorType>();
  // Not a ranked tensor output
  if (!output_type) {
    op->emitOpError("ExpandDims: output type not ranked tensor");
    return llvm::None;
  }

  RankedTensorType input_type =
      input_value.getType().dyn_cast<RankedTensorType>();
  if (!input_type) {
    op->emitOpError("ExpandDims: input type not ranked tensor");
    return llvm::None;
  }

  auto input_shape = input_type.getShape();

  ElementsAttr dim_elem;
  if (!matchPattern(dim_value, m_Constant(&dim_elem))) return llvm::None;

  assert(dim_elem.getType().getRank() == 0 && "expected scalar tensor");
  int32_t dim = dim_elem.getValue<IntegerAttr>({}).getInt();

  SmallVector<int64_t, 4> reshape_dims;
  if (dim < 0 || dim >= input_shape.size()) {  // add dim at end of tensor
    dim = input_shape.size();
    for (int i = 0; i < input_shape.size(); i++) {
      reshape_dims.emplace_back(input_shape[i]);
    }
    reshape_dims.emplace_back(1);
  } else {
    for (int i = 0; i < input_shape.size(); i++) {
      if (i == dim) {
        reshape_dims.emplace_back(1);
      }
      reshape_dims.emplace_back(input_shape[i]);
    }
  }

  ArrayAttr shape_attr = rewriter.getI64ArrayAttr(reshape_dims);

  return rewriter
      .create<tosa::ReshapeOp>(op->getLoc(), output_type, input_value,
                               shape_attr)
      .getResult();
}

// Lowers Squeeze to TOSA.
llvm::Optional<Value> convertSqueezeOp(PatternRewriter& rewriter, Operation* op,
                                       Value result_value, Value input_value,
                                       SmallVector<int32_t, 8>& squeeze_dims) {
  // Lowers to a reshape op where dimensions in squeeze_dims with size=1
  // are removed.
  RankedTensorType output_type =
      result_value.getType().dyn_cast<RankedTensorType>();
  // Not a ranked tensor output
  if (!output_type) {
    op->emitOpError("Squeeze: output type not ranked tensor");
    return llvm::None;
  }

  RankedTensorType input_type =
      input_value.getType().dyn_cast<RankedTensorType>();
  if (!input_type) {
    op->emitOpError("Squeeze: input type not ranked tensor");
    return llvm::None;
  }

  auto input_shape = input_type.getShape();

  SmallVector<int64_t, 8> reshape_dims;

  if (squeeze_dims.empty()) {  // remove all 1-dims
    for (int i = 0; i < input_shape.size(); i++) {
      if (input_shape[i] != 1) {
        reshape_dims.emplace_back(input_shape[i]);
      }
    }
  } else {
    // Remove only specified dims.
    // First sort the array so they can be picked off in sequence.
    std::sort(squeeze_dims.begin(), squeeze_dims.end(),
              [](const int32_t a, const int32_t b) { return a < b; });

    int pos = 0;
    auto dim = squeeze_dims[pos];
    for (int i = 0; i < input_shape.size(); i++) {
      if (i == dim) {
        pos = pos + 1;
        if (pos < squeeze_dims.size())
          dim = squeeze_dims[pos];
        else
          dim = -1;  // Invalid
      } else {
        reshape_dims.emplace_back(input_shape[i]);
      }
    }
  }

  ArrayAttr shape_attr = rewriter.getI64ArrayAttr(reshape_dims);

  return rewriter
      .create<tosa::ReshapeOp>(op->getLoc(), output_type, input_value,
                               shape_attr)
      .getResult();
}

// Lowers ELU to a sequence of TOSA ops.
llvm::Optional<Value> convertEluOp(PatternRewriter& rewriter, Operation* op,
                                   Value result_value, Value features_value) {
  // Lowers Elu using the following formula:
  // elu(x) = x < 0 ? (exp(x) - 1) : x
  // one = const({1});
  // zero = const({0});
  // one_bcast = reshape(one, [1, ..., rank(x) - 1])
  // zero_bcast = reshape(zero, [1, ..., rank(x) - 1])
  // a1 = exp(x);
  // a2 = sub(a1, one_bcast)
  // a3 = ge(x, zero_bcast)
  // a4 = select(a3, x, a2)
  RankedTensorType output_type =
      result_value.getType().dyn_cast<RankedTensorType>();
  // Not a ranked tensor output
  if (!output_type) {
    op->emitOpError("Elu: output type not ranked tensor");
    return llvm::None;
  }

  int32_t input_rank = output_type.getShape().size();
  SmallVector<int64_t, 4> bcast_shape(input_rank, 1);

  // Can't directly create size=1, rank=rank(input) tensor because
  // it will be optimized out.  Instead, create rank0 tensor and reshape later.
  Value one_const_op = getTosaConstTensorSingleF32(rewriter, op, 1.0);

  Value zero_const_op = getTosaConstTensorSingleF32(rewriter, op, 0.0);

  auto a1_exp_in_op =
      rewriter.create<tosa::ExpOp>(op->getLoc(), output_type, features_value);

  auto a2_sub_a1_one_op = rewriter.create<tosa::SubOp>(
      op->getLoc(), output_type, a1_exp_in_op.getResult(), one_const_op);

  auto a3_ge_in_zero_op = rewriter.create<tosa::GreaterEqualOp>(
      op->getLoc(),
      RankedTensorType::get(output_type.getShape(), rewriter.getIntegerType(1)),
      features_value, zero_const_op);

  return rewriter
      .create<tosa::SelectOp>(op->getLoc(), output_type,
                              a3_ge_in_zero_op.getResult(), features_value,
                              a2_sub_a1_one_op.getResult())
      .getResult();
}

// Lowers Softmax to a sequence of TOSA ops.
llvm::Optional<Value> convertSoftmaxOp(PatternRewriter& rewriter, Operation* op,
                                       Value result_value, Value logits_value) {
  // softmax = exp(logits) / reduce_sum(exp(logits), -1)
  //
  // or equivalently multiply exp(-max(logits)) to both numerator and
  // denominator we get:
  //
  // softmax = exp(logits - max(logits)) / reduce_sum(exp(logits -
  // max(logits)), -1)
  //
  // We'll use first version for direct fp lowering, and second version for
  // quantized lowering since second one we can restrict input to exp() be
  // negative, and thus LUT can always be within [0.0, 1.0].
  RankedTensorType output_type =
      result_value.getType().dyn_cast<RankedTensorType>();
  RankedTensorType input_type =
      logits_value.getType().dyn_cast<RankedTensorType>();

  // Not a ranked tensor input/output
  if (!output_type || !input_type) {
    op->emitOpError("Softmax: input and result not ranked tensors");
    return llvm::None;
  }

  // reduce_sum on last dimension
  int32_t input_rank = input_type.getShape().size();
  ArrayRef<int64_t> logits_shape = output_type.getShape();

  if (input_type.getElementType().isa<mlir::quant::QuantizedType>() &&
      output_type.getElementType().isa<mlir::quant::QuantizedType>()) {
    SmallVector<int64_t, 4> rsum_shape_v(input_type.getShape().begin(),
                                         input_type.getShape().end() - 1);
    rsum_shape_v.push_back(1);
    ArrayRef<int64_t> rsum_shape(rsum_shape_v);
    // The if condition already checks if these are UQTs
    mlir::quant::UniformQuantizedType in_quant_type =
        input_type.getElementType().cast<mlir::quant::UniformQuantizedType>();
    mlir::quant::UniformQuantizedType out_quant_type =
        output_type.getElementType().cast<mlir::quant::UniformQuantizedType>();

    auto int16_element_qtype = mlir::quant::UniformQuantizedType::get(
        true, rewriter.getIntegerType(16), rewriter.getF32Type(), 1.0f, 0,
        -32768, 32767);
    RankedTensorType int16_logits_type =
        RankedTensorType::get(logits_shape, int16_element_qtype);
    RankedTensorType int32_logits_type =
        RankedTensorType::get(logits_shape, rewriter.getIntegerType(32));
    RankedTensorType int16_rsum_type =
        RankedTensorType::get(rsum_shape, int16_element_qtype);
    RankedTensorType int32_rsum_type =
        RankedTensorType::get(rsum_shape, rewriter.getIntegerType(32));

    // Step 1. get x - max(x)
    Value op1_rescale_in =
        buildRescale(rewriter, op, int32_logits_type, logits_value, 1.0f,
                     in_quant_type.getZeroPoint(), 0);

    auto op2_reducemax_op1 = rewriter.create<tosa::ReduceMaxOp>(
        op->getLoc(), int32_rsum_type, op1_rescale_in,
        rewriter.getI64IntegerAttr(input_rank - 1));

    auto op3_sub_op1_op2 = rewriter.create<tosa::SubOp>(
        op->getLoc(), int32_logits_type, op1_rescale_in,
        op2_reducemax_op1.getResult());

    // Table input range from -16.0 to 16.0, input below -16.0 treated as
    // exp(-16.0), which is 0 in 0.16
    const double exp_sample_grain = 1.0 / 16.0;
    auto exp_func = [exp_sample_grain](int32_t x) -> int32_t {
      double v = static_cast<double>(x) * exp_sample_grain;
      v = v < 0.0 ? std::exp(v) : 1.0;
      return std::lround(32768.0 * v);
    };

    Value exp_table_const = getTosa1DConstTensorTable(rewriter, op, exp_func);

    // Step 2. rescale input
    Value op4_rescale_op3 = buildRescale(
        rewriter, op, int16_logits_type, op3_sub_op1_op2.getResult(),
        in_quant_type.getScale() * 128.0 / exp_sample_grain, 0, 0);

    // Step 3. get exp() result
    // Since we already make sure input x < 0 in step 1,
    // we can utilize full output 0.16 range.

    // Output is 0.23
    auto op5_table_op4 = rewriter.create<tosa::TableOp>(
        op->getLoc(), int32_logits_type, op4_rescale_op3, exp_table_const);

    // Right shift 3 bits. output 0.20
    auto op6_rshift_op5 = rewriter.create<tosa::ArithmeticRightShiftOp>(
        op->getLoc(), int32_logits_type, op5_table_op4.getResult(),
        getTosaConstTensorSingleI32(rewriter, op, 3), true);

    // Step 4. get sum(exp()). output 12.20
    auto op7_reducesum_op6 = rewriter.create<tosa::ReduceSumOp>(
        op->getLoc(), int32_rsum_type, op6_rshift_op5.getResult(),
        rewriter.getI64IntegerAttr(input_rank - 1));

    // Step 5. calculate reciprocal(sum(exp()))
    auto op8_clz_op7 = rewriter.create<tosa::ClzOp>(
        op->getLoc(), int32_rsum_type, op7_reducesum_op6.getResult());

    // rshift amount of reciprocal(sum(exp()))
    // 12 from the integer bits of 12.20 accumulator
    // 30 from output of multiply 0.15 x 0.15
    // -8 to keep additional 8 bits before output rescaling
    auto op9_sub_op8 = rewriter.create<tosa::SubOp>(
        op->getLoc(), int32_rsum_type,
        getTosaConstTensorSingleI32(rewriter, op, 12 + 30 - 8),
        op8_clz_op7.getResult());

    // Left shift to get  1.31 format
    auto op10_lshift_op7_op8 = rewriter.create<tosa::LogicalLeftShiftOp>(
        op->getLoc(), int32_rsum_type, op7_reducesum_op6.getResult(),
        op8_clz_op7.getResult());

    // Subtract (1 << 31) to make 0 <= x <= 1
    auto op11_sub_op10 = rewriter.create<tosa::SubOp>(
        op->getLoc(), int32_rsum_type, op10_lshift_op7_op8.getResult(),
        getTosaConstTensorSingleI32(rewriter, op, (1u << 31)));

    // Right shift 16 bits to get 16 bits index
    auto op12_rshift_op11 = rewriter.create<tosa::ArithmeticRightShiftOp>(
        op->getLoc(), int32_rsum_type, op11_sub_op10.getResult(),
        getTosaConstTensorSingleI32(rewriter, op, 16), true);

    // cast to 16 bits to index TABLE op
    auto op13_cast_op12 = rewriter.create<tosa::CastOp>(
        op->getLoc(), int16_rsum_type, op12_rshift_op11.getResult());

    // Generate table for 1 / (1 + x), for 0 <= x <= 1
    const double one_over_one_plus_x_sample_grain = 1.0 / 256.0;
    auto one_over_one_plus_x_func =
        [one_over_one_plus_x_sample_grain](int32_t x) -> int32_t {
      double v = static_cast<double>(x) * one_over_one_plus_x_sample_grain;
      v = v < 0 ? 1.0 : 1.0 / (1.0 + v);
      return std::lround(32768.0 * v);
    };

    Value one_over_one_plus_x_table_const =
        getTosa1DConstTensorTable(rewriter, op, one_over_one_plus_x_func);

    auto op14_table_op13 = rewriter.create<tosa::TableOp>(
        op->getLoc(), int32_rsum_type, op13_cast_op12.getResult(),
        one_over_one_plus_x_table_const);

    // Rescale sum(exp(x)) from 0.23 back to 0.16
    Value op15_rescale_op14 = buildRescale(rewriter, op, int32_rsum_type,
                                           op14_table_op13, 1.0 / 128.0, 0, 0);

    // Rescale exp(x) from 0.23 back to 0.16
    Value op16_rescale_op5 =
        buildRescale(rewriter, op, int32_logits_type, op5_table_op4.getResult(),
                     1.0 / 128.0, 0, 0);

    // Step 6. apply the scales we just get explicitly in i32 space
    // lhs: 0.16, rhs: 0.16, output: 0.32
    auto op17_mul_op15_op16 =
        rewriter.create<tosa::MulOp>(op->getLoc(), int32_logits_type,
                                     op15_rescale_op14, op16_rescale_op5, 0);

    // Apply right shift from clz
    auto op18_rshift_op17_op9 = rewriter.create<tosa::ArithmeticRightShiftOp>(
        op->getLoc(), int32_logits_type, op17_mul_op15_op16.getResult(),
        op9_sub_op8.getResult(), true);

    // Step 7. output scaling, extra 1.0 / 256.0 since we keep extra 8 bits
    // in op9_sub_op8
    return buildRescale(rewriter, op, output_type,
                        op18_rshift_op17_op9.getResult(),
                        1.0 / (out_quant_type.getScale() * 256.0), 0,
                        out_quant_type.getZeroPoint());

  } else {
    SmallVector<int64_t, 4> rsum_shape_v(input_type.getShape().begin(),
                                         input_type.getShape().end());
    rsum_shape_v[input_rank - 1] = 1;
    ArrayRef<int64_t> rsum_shape(rsum_shape_v);

    // Floating-point loewring is more direct:
    //
    // op1 = exp(logits)
    // op2 = reduce_sum(op1, -1)
    // op3 = reciprocal(op2)
    // op4 = mul(op1, op3)
    auto op1_exp_in =
        rewriter.create<tosa::ExpOp>(op->getLoc(), output_type, logits_value);
    RankedTensorType rsum_type =
        RankedTensorType::get(rsum_shape, output_type.getElementType());

    // Keep dims so we don't need to reshape later
    auto op2_reducesum_op1 = rewriter.create<tosa::ReduceSumOp>(
        op->getLoc(), rsum_type, op1_exp_in.getResult(),
        rewriter.getI64IntegerAttr(input_rank - 1));
    auto op3_reciprocal_op2 = rewriter.create<tosa::ReciprocalOp>(
        op->getLoc(), rsum_type, op2_reducesum_op1.getResult());

    return rewriter
        .create<tosa::MulOp>(op->getLoc(), output_type, op1_exp_in.getResult(),
                             op3_reciprocal_op2.getResult(), 0)
        .getResult();
  }
}

// Lowers LogSoftmax to a sequence of TOSA ops.
llvm::Optional<Value> convertLogSoftmaxOp(PatternRewriter& rewriter,
                                          Operation* op, Value result_value,
                                          Value logits_value) {
  // log_softmax = log(exp(logits) / reduce_sum(exp(logits), -1))
  // op1 = exp(logits)
  // op2 = reduce_sum(op1, -1)
  // op3 = reciprocal(op2)
  // op4 = mul(op1, op3)
  // op5 = log(op4)

  RankedTensorType output_type =
      result_value.getType().dyn_cast<RankedTensorType>();
  // Not a ranked tensor output
  if (!output_type) {
    op->emitOpError("LogSoftmax: output type not ranked tensor.");
    return llvm::None;
  }

  RankedTensorType input_type =
      op->getOperand(0).getType().dyn_cast<RankedTensorType>();
  if (!input_type) {
    op->emitOpError("LogSoftmax: input type not ranked tensor.");
    return llvm::None;
  }

  mlir::quant::UniformQuantizedType in_quant_type =
      input_type.getElementType()
          .dyn_cast_or_null<mlir::quant::UniformQuantizedType>();
  mlir::quant::UniformQuantizedType out_quant_type =
      output_type.getElementType()
          .dyn_cast_or_null<mlir::quant::UniformQuantizedType>();
  if (in_quant_type || out_quant_type) {
    op->emitOpError("Quantized log_softmax lowering not implemented yet");
    return llvm::None;
  }

  auto op1_exp_in =
      rewriter.create<tosa::ExpOp>(op->getLoc(), output_type, logits_value);

  // reduce_sum on last dimension
  int32_t input_rank = input_type.getShape().size();
  SmallVector<int64_t, 4> rsum_shape(output_type.getShape().begin(),
                                     output_type.getShape().end());
  rsum_shape[input_rank - 1] = 1;
  RankedTensorType rsum_type = RankedTensorType::get(
      ArrayRef<int64_t>(rsum_shape), output_type.getElementType());
  // Keep dims so we don't need to reshape later
  auto op2_reducesum_op1 = rewriter.create<tosa::ReduceSumOp>(
      op->getLoc(), rsum_type, op1_exp_in.getResult(),
      rewriter.getI64IntegerAttr(input_rank - 1));
  auto op3_reciprocal_op2 = rewriter.create<tosa::ReciprocalOp>(
      op->getLoc(), rsum_type, op2_reducesum_op1.getResult());

  auto op4_mul_op1_op3 = rewriter.create<tosa::MulOp>(
      op->getLoc(), output_type, op1_exp_in.getResult(),
      op3_reciprocal_op2.getResult(), 0);

  return rewriter
      .create<tosa::LogOp>(op->getLoc(), output_type,
                           op4_mul_op1_op3.getResult())
      .getResult();
}

// Lowers SpaceToDepth to a sequence of TOSA ops.  Supports NHWC.
llvm::Optional<Value> convertSpaceToDepthOp(PatternRewriter& rewriter,
                                            Operation* op, Value result_value,
                                            Value input_value,
                                            IntegerAttr block_size_attr,
                                            StringAttr data_format) {
  // NHWC lowering version:
  // a2 = tf.reshape(a, [orig_shape[0], orig_shape[1]//b, b, orig_shape[2]//b,
  // b, orig_shape[3]])
  // a3 = tf.transpose(a2, [0, 1, 3, 2, 4, 5])
  // a4 = tf.reshape(a3, [orig_shape[0], orig_shape[1]//b, orig_shape[2]//b,
  // orig_shape[3]*b*b])
  // return a4
  RankedTensorType output_type =
      result_value.getType().dyn_cast<RankedTensorType>();

  // Not a ranked tensor output.
  if (!output_type) {
    op->emitOpError("SpaceToDepth: output type not ranked tensor.");
    return llvm::None;
  }

  RankedTensorType input_type =
      input_value.getType().dyn_cast<RankedTensorType>();
  if (!input_type) {
    op->emitOpError("SpaceToDepth: input type not ranked tensor.");
    return llvm::None;
  }

  if (input_type.getRank() != 4) {
    op->emitOpError("SpaceToDepth: input rank not 4.");
    return llvm::None;
  }

  auto input_shape = input_type.getShape();

  if (!block_size_attr) {  // This is a required parameter
    op->emitOpError("SpaceToDepth: block size attribute not set.");
    return llvm::None;
  }

  SmallVector<int64_t, 2> block_size;
  block_size.assign(2, block_size_attr.getInt());

  if (!data_format) data_format = rewriter.getStringAttr("NHWC");

  if (data_format.getValue().str() != "NHWC") {
    op->emitOpError("SpaceToDepth: data format not NHWC.");
    return llvm::None;
  }

  assert(block_size[0] * block_size[1] != 0);

  SmallVector<int64_t, 4> a_reshape_dims;
  a_reshape_dims.push_back(input_shape[0]);
  a_reshape_dims.push_back(input_shape[1] / block_size[0]);
  a_reshape_dims.push_back(block_size[0]);
  a_reshape_dims.push_back(input_shape[2] / block_size[1]);
  a_reshape_dims.push_back(block_size[1]);
  a_reshape_dims.push_back(input_shape[3]);

  RankedTensorType a_reshape_output_type = RankedTensorType::get(
      ArrayRef<int64_t>(a_reshape_dims), output_type.getElementType());
  auto a2_reshape_a_op = rewriter.create<tosa::ReshapeOp>(
      op->getLoc(), a_reshape_output_type, input_value,
      rewriter.getI64ArrayAttr(a_reshape_dims));

  Value a3_transpose_perm = get1DConstTensor<tosa::ConstOp, int32_t>(
      rewriter, op, {0, 1, 3, 2, 4, 5});

  auto a3_transpose_a2_op = rewriter.create<tosa::TransposeOp>(
      op->getLoc(), a_reshape_output_type, a2_reshape_a_op.getResult(),
      a3_transpose_perm);

  SmallVector<int64_t, 4> a3_reshape_dims;
  a3_reshape_dims.push_back(input_shape[0]);
  a3_reshape_dims.push_back(input_shape[1] / block_size[0]);
  a3_reshape_dims.push_back(input_shape[2] / block_size[1]);
  a3_reshape_dims.push_back(input_shape[3] * block_size[0] * block_size[1]);

  RankedTensorType a3_reshape_output_type = RankedTensorType::get(
      ArrayRef<int64_t>(a3_reshape_dims), output_type.getElementType());
  return rewriter
      .create<tosa::ReshapeOp>(op->getLoc(), a3_reshape_output_type,
                               a3_transpose_a2_op.getResult(),
                               rewriter.getI64ArrayAttr(a3_reshape_dims))
      .getResult();
}

// Lowers DepthToSpace to a sequence of TOSA ops.  Supports NHWC.
llvm::Optional<Value> convertDepthToSpaceOp(PatternRewriter& rewriter,
                                            Operation* op, Value result_value,
                                            Value input_value,
                                            IntegerAttr block_size_attr,
                                            StringAttr data_format) {
  // NHWC version
  // a2 = tf.reshape(a, [orig_shape[0], orig_shape[1], orig_shape[2], b, b,
  // orig_shape[3] // (b*b)])
  // a3 = tf.transpose(a2, [0, 1, 3, 2, 4, 5])
  // a4 = tf.reshape(a3, [orig_shape[0], orig_shape[1] * b, orig_shape[2] * b,
  // orig_shape[3] // (b*b)])
  // return a4

  RankedTensorType output_type =
      result_value.getType().dyn_cast<RankedTensorType>();

  // Not a ranked tensor output
  if (!output_type) {
    op->emitOpError("DepthToSpace: output type not ranked tensor.");
    return llvm::None;
  }

  RankedTensorType input_type =
      input_value.getType().dyn_cast<RankedTensorType>();
  if (!input_type) {
    op->emitOpError("DepthToSpace: input type not ranked tensor.");
    return llvm::None;
  }

  if (input_type.getRank() != 4) return llvm::None;
  auto input_shape = input_type.getShape();

  if (!block_size_attr) {  // This is a required parameter
    op->emitOpError("DepthToSpace: block size attribute not set.");
    return llvm::None;
  }

  SmallVector<int64_t, 2> block_size;
  block_size.assign(2, block_size_attr.getInt());

  if (!data_format) data_format = rewriter.getStringAttr("NHWC");
  if (data_format.getValue().str() != "NHWC") {
    op->emitOpError("DepthToSpace: data format not NHWC.");
    return llvm::None;
  }

  assert(block_size[0] * block_size[1] != 0);

  SmallVector<int64_t, 4> a_reshape_dims;
  a_reshape_dims.push_back(input_shape[0]);
  a_reshape_dims.push_back(input_shape[1]);
  a_reshape_dims.push_back(input_shape[2]);
  a_reshape_dims.push_back(block_size[0]);
  a_reshape_dims.push_back(block_size[1]);
  a_reshape_dims.push_back(input_shape[3] / (block_size[0] * block_size[1]));

  RankedTensorType a_reshape_output_type = RankedTensorType::get(
      ArrayRef<int64_t>(a_reshape_dims), output_type.getElementType());
  auto a2_reshape_a_op = rewriter.create<tosa::ReshapeOp>(
      op->getLoc(), a_reshape_output_type, input_value,
      rewriter.getI64ArrayAttr(a_reshape_dims));

  Value a3_transpose_perm = get1DConstTensor<tosa::ConstOp, int32_t>(
      rewriter, op, {0, 1, 3, 2, 4, 5});

  auto a3_transpose_a2_op = rewriter.create<tosa::TransposeOp>(
      op->getLoc(), a_reshape_output_type, a2_reshape_a_op.getResult(),
      a3_transpose_perm);

  SmallVector<int64_t, 4> a3_reshape_dims;
  a3_reshape_dims.push_back(input_shape[0]);
  a3_reshape_dims.push_back(input_shape[1] * block_size[0]);
  a3_reshape_dims.push_back(input_shape[2] * block_size[1]);
  a3_reshape_dims.push_back(input_shape[3] / (block_size[0] * block_size[1]));

  RankedTensorType a3_reshape_output_type = RankedTensorType::get(
      ArrayRef<int64_t>(a3_reshape_dims), output_type.getElementType());
  return rewriter
      .create<tosa::ReshapeOp>(op->getLoc(), a3_reshape_output_type,
                               a3_transpose_a2_op.getResult(),
                               rewriter.getI64ArrayAttr(a3_reshape_dims))
      .getResult();
}

// Lowers Split to a sequence of TOSA ops.
llvm::Optional<ValueRange> convertSplitOp(PatternRewriter& rewriter,
                                          Operation* op, Value result_value,
                                          Value input_value, int32_t num_split,
                                          int32_t axis) {
  // This lowering creates num_split slice ops and ties them together
  // with IdentityN to get from an array of Operations to a single Operation
  // with a list of result tensors.
  RankedTensorType result_type =
      result_value.getType().dyn_cast<RankedTensorType>();
  // Not a ranked tensor output
  if (!result_type) {
    op->emitOpError("Split: output type not ranked tensor.");
    return llvm::None;
  }

  RankedTensorType input_type =
      input_value.getType().dyn_cast<RankedTensorType>();
  if (!input_type) {
    op->emitOpError("Split: input type not ranked tensor.");
    return llvm::None;
  }

  auto input_shape = input_type.getShape();

  SmallVector<Value, 4> results_vec;

  assert(axis > 0 && axis < input_shape.size());
  assert((input_shape[axis] % num_split) == 0);
  assert(num_split > 0);

  int64_t slice_size = input_shape[axis] / num_split;

  SmallVector<Type, 4>
      outs_type_vec;  // A list of the output types for each slice op

  for (int i = 0; i < num_split; i++) {
    // Each slice has a different begining point.
    // The slice size is actually the same each op.
    SmallVector<int64_t, 4> begin_vals, size_vals;

    for (int j = 0; j < input_shape.size(); j++) {
      if (j == axis) {
        begin_vals.push_back(slice_size * i);
        size_vals.push_back(slice_size);
      } else {
        begin_vals.push_back(0);
        size_vals.push_back(input_shape[j]);
      }
    }

    ArrayAttr begin = rewriter.getI64ArrayAttr(begin_vals);
    ArrayAttr size = rewriter.getI64ArrayAttr(size_vals);

    outs_type_vec.push_back(RankedTensorType::get(
        ArrayRef<int64_t>(size_vals), result_type.getElementType()));

    auto slice_op = rewriter.create<tosa::SliceOp>(
        op->getLoc(),
        RankedTensorType::get(ArrayRef<int64_t>(size_vals),
                              result_type.getElementType()),
        input_value, begin, size);

    results_vec.push_back(slice_op.getResult());
  }

  // Combine the sequence of tosa.slice() ops into a list
  // using the IdentityN operator
  return rewriter
      .create<tosa::IdentityNOp>(op->getLoc(), ArrayRef<Type>(outs_type_vec),
                                 results_vec)
      .getResults();
}

// Lowers SplitV to a sequence of TOSA ops.
llvm::Optional<ValueRange> convertSplitVOp(PatternRewriter& rewriter,
                                           Operation* op, Value result_value,
                                           Value input_value,
                                           SmallVector<int32_t, 4>& size_split,
                                           int32_t axis) {
  // This lowering creates num_split slice ops and ties them together
  // with IdentityN to get from an array of Operations to a single Operation
  // with a list of result tensors.
  RankedTensorType result_type =
      result_value.getType().dyn_cast<RankedTensorType>();
  // Not a ranked tensor output
  if (!result_type) {
    op->emitOpError("SplitV: output type not ranked tensor.");
    return llvm::None;
  }

  RankedTensorType input_type =
      input_value.getType().dyn_cast<RankedTensorType>();
  if (!input_type) {
    op->emitOpError("SplitV: input type not ranked tensor.");
    return llvm::None;
  }

  auto input_shape = input_type.getShape();

  SmallVector<Value, 4> results_vec;

  assert(axis > 0 && axis < input_shape.size());
  int32_t size_split_sum = 0;
  for (int i = 0; i < size_split.size(); i++) {
    size_split_sum += size_split[i];
  }

  // The split sizes must sum up to the size of the axis being split
  assert(size_split_sum == input_shape[axis]);

  // Create num_split slice ops:
  SmallVector<Type, 4>
      outs_type_vec;  // A list of the output types for each slice op

  int32_t curr_split_start = 0;
  for (int i = 0; i < size_split.size(); i++) {
    // Each slice has a different begining point.
    // The slice size is different for each op.
    SmallVector<int64_t, 4> begin_vals, size_vals;

    for (int j = 0; j < input_shape.size(); j++) {
      if (j == axis) {
        begin_vals.push_back(curr_split_start);
        size_vals.push_back(size_split[i]);
      } else {
        begin_vals.push_back(0);
        size_vals.push_back(input_shape[j]);
      }
    }

    ArrayAttr begin = rewriter.getI64ArrayAttr(begin_vals);
    ArrayAttr size = rewriter.getI64ArrayAttr(size_vals);

    outs_type_vec.push_back(RankedTensorType::get(
        ArrayRef<int64_t>(size_vals), result_type.getElementType()));

    auto slice_op = rewriter.create<tosa::SliceOp>(
        op->getLoc(),
        RankedTensorType::get(ArrayRef<int64_t>(size_vals),
                              result_type.getElementType()),
        input_value, begin, size);

    results_vec.push_back(slice_op.getResult());

    // Next start position
    curr_split_start += size_split[i];
  }

  // Combine the sequence of tosa.slice() ops into a list
  // using the IdentityN operator
  return rewriter
      .create<tosa::IdentityNOp>(op->getLoc(), ArrayRef<Type>(outs_type_vec),
                                 results_vec)
      .getResults();
}

// Lowers StridedSlice to a sequence of TOSA ops.
llvm::Optional<Value> convertStridedSliceOp(
    PatternRewriter& rewriter, Operation* op, Value result_value,
    Value input_value, Value begin_value, Value end_value, Value strides_value,
    int32_t begin_mask, int32_t end_mask, int32_t ellipsis_mask,
    int32_t new_axis_mask, int32_t shrink_axis_mask) {
  // The mask arguments are bitmasks where bit [i] applies to
  // dimension [i] of the input tensor.
  //
  // The rough algorithm for lowering strided slice is as follows:
  //
  // 0. Process begin/end masks, since they are basically syntactic sugar
  // on top of the begin_value/end_value arrays
  //
  // 1. Slice1: Ignoring stride, slice the interesting range from the input
  // tensor
  //
  // 2. Reshape2: Reshape the tensor from (1) such that each dimension with
  // stride is split into two dimensions of size_i/stride_i, stride_i.   A naive
  // implementation doubles the input tensor rank, but only dimensions being
  // strided actually need to be doubled.
  //
  // 3. Slice3: Slice the tensor from (2) such that we select index [0] from
  // each of the stride_i dimensions in (2)
  //
  // 4. Reshape4: Reshape the tensor to eliminate the stride_i dimensions, add
  // any dimensions in new_axis_mask and remove any dimensions in the
  // shrink_axis_mask

  // Limitations:
  // This implementation only supports ellipsis_mask=0 for now
  // This implementation does not support reverse stride yet.  Will need
  // to insert tosa.Reverse operators for this.
  assert(ellipsis_mask == 0);

  RankedTensorType input_type =
      input_value.getType().dyn_cast<RankedTensorType>();
  RankedTensorType result_type =
      result_value.getType().dyn_cast<RankedTensorType>();

  if (!result_type) {
    op->emitOpError("StridedSlice: output type not ranked tensor.");
    return llvm::None;
  }

  if (!input_type) {
    op->emitOpError("StridedSlice: input type not ranked tensor.");
    return llvm::None;
  }

  int32_t input_rank = input_type.getRank();
  auto input_shape = input_type.getShape();

  // Extract the begin/end/stride tensors
  SmallVector<int32_t, 4> begin, end, strides;

  if (getVectorFromValue32(begin_value, begin) != input_rank) {
    op->emitOpError("StridedSlice: begin doesn't match input_rank.");
    return llvm::None;
  }
  if (getVectorFromValue32(end_value, end) != input_rank) {
    op->emitOpError("StridedSlice: end doesn't match input_rank.");
    return llvm::None;
  }
  if (getVectorFromValue32(strides_value, strides) != input_rank) {
    op->emitOpError("StridedSlice: strides doesn't match input_rank.");
    return llvm::None;
  }

  SmallVector<int64_t, 2> a1_begin(input_rank), a1_size(input_rank);
  SmallVector<int64_t, 2> a2_shape(input_rank * 2);
  SmallVector<int64_t, 2> a3_begin(input_rank * 2), a3_size(input_rank * 2);
  SmallVector<int64_t, 2> a4_shape;

  // Step 0: Process the begin/end masks and build the begin/sizes for the
  // first slice
  int residual = 1;
  (void)residual;
  for (int i = 0; i < input_rank; i++) {
    if (begin_mask & (1 << i)) begin[i] = 0;

    if (end_mask & (1 << i)) end[i] = input_shape[i];

    // Wrap around index if begin and end is negative
    if (begin[i] < 0) begin[i] += input_shape[i];

    if (end[i] < 0) end[i] += input_shape[i];

    // TODO(suderman): support reverse stride
    a1_begin[i] = begin[i];
    a1_size[i] = end[i] - begin[i];

    a2_shape[i * 2 + 0] = a1_size[i] / strides[i];
    a2_shape[i * 2 + 1] = strides[i];

    a3_begin[i * 2 + 0] = 0;
    a3_begin[i * 2 + 1] = 0;

    if (shrink_axis_mask & (1 << i)) {
      a3_size[i * 2 + 0] = 1;
    } else {
      a3_size[i * 2 + 0] = a1_size[i] / strides[i];
    }
    a3_size[i * 2 + 1] = 1;

    if (!(shrink_axis_mask & (1 << i))) {
      if (new_axis_mask & (1 << i)) a4_shape.push_back(1);
      a4_shape.push_back((a1_size[i] / strides[i]));
    }
  }

  // Make sure we didn't lose any dimensions from the shrink_axis_mask
  assert(residual == 1);

  // Step 1: Slice the input array
  auto a1_slice_op = rewriter.create<tosa::SliceOp>(
      op->getLoc(),
      RankedTensorType::get(ArrayRef<int64_t>(a1_size),
                            input_type.getElementType()),
      input_value, rewriter.getI64ArrayAttr(a1_begin),
      rewriter.getI64ArrayAttr(a1_size));

  // Step 2: reshape the sliced array
  auto a2_reshape_op = rewriter.create<tosa::ReshapeOp>(
      op->getLoc(),
      RankedTensorType::get(ArrayRef<int64_t>(a2_shape),
                            input_type.getElementType()),
      a1_slice_op.getResult(), rewriter.getI64ArrayAttr(a2_shape));

  // Step 3: take a slice along the strides
  auto a3_slice_op = rewriter.create<tosa::SliceOp>(
      op->getLoc(),
      RankedTensorType::get(ArrayRef<int64_t>(a3_size),
                            input_type.getElementType()),
      a2_reshape_op.getResult(), rewriter.getI64ArrayAttr(a3_begin),
      rewriter.getI64ArrayAttr(a3_size));

  // Step 4: reshape the now-strided tensor
  return rewriter
      .create<tosa::ReshapeOp>(
          op->getLoc(),
          RankedTensorType::get(ArrayRef<int64_t>(a4_shape),
                                input_type.getElementType()),
          a3_slice_op.getResult(), rewriter.getI64ArrayAttr(a4_shape))
      .getResult();
}

// Lowers FloorDiv to a sequence of TOSA operators.
llvm::Optional<Value> convertFloorDivOp(PatternRewriter& rewriter,
                                        Operation* op, Value result_value,
                                        Value lhs_value, Value rhs_value) {
  // FloorDiv lowering:
  // floor(1/rhs * lhs)
  //
  // a1 = reciprocal(rhs);
  // a2 = mul(lhs, a1);
  // a3 = floor(a2);
  // return a3;
  RankedTensorType output_type =
      result_value.getType().dyn_cast<RankedTensorType>();
  // Not a ranked tensor output
  if (!output_type) return llvm::None;

  auto a1_reciprocal_rhs_op =
      rewriter.create<tosa::ReciprocalOp>(op->getLoc(), output_type, rhs_value);
  auto a2_mul_lhs_a1_op =
      rewriter.create<tosa::MulOp>(op->getLoc(), output_type, lhs_value,
                                   a1_reciprocal_rhs_op.getResult(), 0);
  return rewriter
      .create<tosa::FloorOp>(op->getLoc(), output_type,
                             a2_mul_lhs_a1_op.getResult())
      .getResult();
}

// Lowers FloorMod to a sequence of TOSA operators.
llvm::Optional<Value> convertFloorModOp(PatternRewriter& rewriter,
                                        Operation* op, Value result_value,
                                        Value lhs_value, Value rhs_value) {
  // FloorMod lowering:
  // (1/rhs * lhs) - floor(1/rhs * lhs)
  // a1 = reciprocal(rhs);
  // a2 = mul(lhs, a1);
  // a3 = floor(a2);
  // a4 = sub(a2, a3);
  // return a4;

  RankedTensorType output_type =
      result_value.getType().dyn_cast<RankedTensorType>();
  // Not a ranked tensor output
  if (!output_type) return llvm::None;

  auto a1_reciprocal_rhs_op =
      rewriter.create<tosa::ReciprocalOp>(op->getLoc(), output_type, rhs_value);
  auto a2_mul_lhs_a1_op =
      rewriter.create<tosa::MulOp>(op->getLoc(), output_type, lhs_value,
                                   a1_reciprocal_rhs_op.getResult(), 0);
  auto a3_floor_a2_op = rewriter.create<tosa::FloorOp>(
      op->getLoc(), output_type, a2_mul_lhs_a1_op.getResult());
  return rewriter
      .create<tosa::SubOp>(op->getLoc(), output_type,
                           a2_mul_lhs_a1_op.getResult(),
                           a3_floor_a2_op.getResult())
      .getResult();
}

// Lowers FusedActivation to a sequence of TOSA ops.
llvm::Optional<Value> convertFusedActivation(PatternRewriter& rewriter,
                                             Operation* op, Value input_value,
                                             StringAttr fused_activation_fn) {
  RankedTensorType input_type =
      input_value.getType().dyn_cast<RankedTensorType>();
  if (!input_type) return llvm::None;

  bool input_is_qtype =
      input_type.getElementType().isa<mlir::quant::UniformQuantizedType>();

  if (input_is_qtype) {
    // We can always make output/input tensor's scale/zp always be the same when
    // legalizing fused_activation_function, as it's generated during
    // legalization.
    auto input_qtype =
        input_type.getElementType().cast<mlir::quant::UniformQuantizedType>();

    if (fused_activation_fn.getValue() == "NONE") {
      return input_value;
    } else if (fused_activation_fn.getValue() == "RELU") {
      int32_t quantized_0 = input_qtype.getZeroPoint();
      int32_t quantized_max = input_qtype.getStorageTypeMax();

      auto clamp_op = rewriter.create<tosa::ClampOp>(
          op->getLoc(), input_type, input_value,
          rewriter.getI64IntegerAttr(quantized_0),
          rewriter.getI64IntegerAttr(quantized_max),
          rewriter.getF32FloatAttr(0), rewriter.getF32FloatAttr(0));

      return clamp_op.getResult();
    } else if (fused_activation_fn.getValue() == "RELU6") {
      int32_t quantized_0 = input_qtype.getZeroPoint();
      int32_t quantized_6 = std::llround((6.0f / input_qtype.getScale()) +
                                         input_qtype.getZeroPoint());

      auto clamp_op = rewriter.create<tosa::ClampOp>(
          op->getLoc(), input_type, input_value,
          rewriter.getI64IntegerAttr(quantized_0),
          rewriter.getI64IntegerAttr(quantized_6), rewriter.getF32FloatAttr(0),
          rewriter.getF32FloatAttr(0));

      return clamp_op.getResult();
    } else if (fused_activation_fn.getValue() == "RELU_N1_TO_1") {
      int32_t quantized_n1 = std::llround((-1.0f / input_qtype.getScale()) +
                                          input_qtype.getZeroPoint());
      int32_t quantized_1 = std::llround((1.0f / input_qtype.getScale()) +
                                         input_qtype.getZeroPoint());

      auto clamp_op = rewriter.create<tosa::ClampOp>(
          op->getLoc(), input_type, input_value,
          rewriter.getI64IntegerAttr(quantized_n1),
          rewriter.getI64IntegerAttr(quantized_1), rewriter.getF32FloatAttr(0),
          rewriter.getF32FloatAttr(0));

      return clamp_op.getResult();
    } else {
      op->emitWarning("convertFusedActivation: Not implemented yet");
      return llvm::None;
    }
  } else {
    if (fused_activation_fn.getValue() == "NONE") {
      return input_value;
    } else {
      // For non-quantized type, only support F32.
      if (!input_type.getElementType().isF32()) {
        op->emitOpError("ConvertTFLeakyReluOp: only support F32");
        return llvm::None;
      }

      if (fused_activation_fn.getValue() == "RELU") {
        return rewriter
            .create<tosa::ReluNOp>(
                op->getLoc(), input_type, input_value,
                rewriter.getI64IntegerAttr(std::numeric_limits<int32_t>::max()),
                rewriter.getF32FloatAttr(std::numeric_limits<float>::max()))
            .getResult();
      } else if (fused_activation_fn.getValue() == "RELU6") {
        return rewriter
            .create<tosa::ReluNOp>(op->getLoc(), input_type, input_value,
                                   rewriter.getI64IntegerAttr(6),
                                   rewriter.getF32FloatAttr(6.0))
            .getResult();
      } else if (fused_activation_fn.getValue() == "RELU_N1_TO_1") {
        return rewriter
            .create<tosa::ClampOp>(
                op->getLoc(), input_type, input_value,
                rewriter.getI64IntegerAttr(-1), rewriter.getI64IntegerAttr(1),
                rewriter.getF32FloatAttr(-1.0), rewriter.getF32FloatAttr(1.0))
            .getResult();
      } else if (fused_activation_fn.getValue() == "TANH") {
        return rewriter
            .create<tosa::TanhOp>(op->getLoc(), input_type, input_value)
            .getResult();
      } else {
        // Unsupported activation type. Bail out.
        return llvm::None;
      }
    }
  }

  return llvm::None;
}

// Common function for lowering reduce operations to TOSA ops.
template <typename T>
llvm::Optional<Value> convertReduceOpCommon(
    PatternRewriter& rewriter, Operation* op, RankedTensorType output_type,
    Value input_value, ElementsAttr axes_elems, bool keep_dims,
    Type reduce_element_type, bool is_quantized, double input_scale,
    int64_t input_zp, double output_scale, int64_t output_zp) {
  RankedTensorType input_type =
      input_value.getType().dyn_cast<RankedTensorType>();
  if (!input_type) return llvm::None;

  ArrayRef<int64_t> input_shape = input_type.getShape();
  ArrayRef<int64_t> output_shape = output_type.getShape();
  auto input_rank = input_shape.size();
  Value val = input_value;

  if (axes_elems.getNumElements() == 0) {
    // No axes means return the original tensor.
    auto identity_op =
        rewriter.create<tosa::IdentityOp>(op->getLoc(), output_type, val);
    val = identity_op.getResult();
  } else {
    // Reduce along each axis
    SmallVector<int64_t, 4> shape_vec(input_shape.begin(), input_shape.end());

    if (is_quantized) {
      val = buildRescaleToInt32(rewriter, op, val, input_scale, input_zp);
    }

    for (int i = 0; i < axes_elems.getNumElements(); i++) {
      int64_t axis_val = axes_elems.getValue<IntegerAttr>(i).getInt();
      if (axis_val < 0) axis_val += input_rank;
      auto axis_attr = rewriter.getI64IntegerAttr(axis_val);

      shape_vec[axis_val] = 1;
      RankedTensorType reduce_type = RankedTensorType::get(
          llvm::makeArrayRef<int64_t>(shape_vec), reduce_element_type);

      auto reduce_op =
          rewriter.create<T>(op->getLoc(), reduce_type, val, axis_attr);

      val = reduce_op.getResult();
    }

    if (is_quantized) {
      RankedTensorType output_rescale_type = RankedTensorType::get(
          llvm::makeArrayRef<int64_t>(shape_vec), output_type.getElementType());
      val = buildRescale(rewriter, op, output_rescale_type, val, output_scale,
                         0, output_zp, false);
    }

    // Optionally squeeze out the reduced axes.
    if (!keep_dims) {
      auto reshape_op = rewriter.create<tosa::ReshapeOp>(
          op->getLoc(), output_type, val,
          rewriter.getI64ArrayAttr(output_shape));
      val = reshape_op.getResult();
    }
  }

  return val;
}

// Lowers ReduceAll to a sequence of TOSA ops.
llvm::Optional<Value> convertReduceAllOp(
    PatternRewriter& rewriter, Operation* op, RankedTensorType output_type,
    Value input_value, ElementsAttr axes_elems, bool keep_dims) {
  RankedTensorType input_type =
      input_value.getType().dyn_cast<RankedTensorType>();
  if (!input_type) return llvm::None;

  return convertReduceOpCommon<tosa::ReduceAllOp>(
      rewriter, op, output_type, input_value, axes_elems, keep_dims,
      output_type.getElementType(), false, 1.0f, 0, 1.0f, 0);
}

// Lowers ReduceAny to a sequence of TOSA ops.
llvm::Optional<Value> convertReduceAnyOp(
    PatternRewriter& rewriter, Operation* op, RankedTensorType output_type,
    Value input_value, ElementsAttr axes_elems, bool keep_dims) {
  RankedTensorType input_type =
      input_value.getType().dyn_cast<RankedTensorType>();
  if (!input_type) return llvm::None;

  return convertReduceOpCommon<tosa::ReduceAnyOp>(
      rewriter, op, output_type, input_value, axes_elems, keep_dims,
      output_type.getElementType(), false, 1.0f, 0, 1.0f, 0);
}

// Lowers ReduceMin to a sequence of TOSA ops.
llvm::Optional<Value> convertReduceMinOp(
    PatternRewriter& rewriter, Operation* op, RankedTensorType output_type,
    Value input_value, ElementsAttr axes_elems, bool keep_dims) {
  RankedTensorType input_type =
      input_value.getType().dyn_cast<RankedTensorType>();
  if (!input_type) return llvm::None;

  return convertReduceOpCommon<tosa::ReduceMinOp>(
      rewriter, op, output_type, input_value, axes_elems, keep_dims,
      output_type.getElementType(), false, 1.0f, 0, 1.0f, 0);
}

// Lowers ReduceMax to a sequence of TOSA ops.
llvm::Optional<Value> convertReduceMaxOp(
    PatternRewriter& rewriter, Operation* op, RankedTensorType output_type,
    Value input_value, ElementsAttr axes_elems, bool keep_dims) {
  RankedTensorType input_type =
      input_value.getType().dyn_cast<RankedTensorType>();
  if (!input_type) return llvm::None;

  return convertReduceOpCommon<tosa::ReduceMaxOp>(
      rewriter, op, output_type, input_value, axes_elems, keep_dims,
      output_type.getElementType(), false, 1.0f, 0, 1.0f, 0);
}

// Lowers ReduceProd to a sequence of TOSA ops.
llvm::Optional<Value> convertReduceProdOp(
    PatternRewriter& rewriter, Operation* op, RankedTensorType output_type,
    Value input_value, ElementsAttr axes_elems, bool keep_dims) {
  RankedTensorType input_type =
      input_value.getType().dyn_cast<RankedTensorType>();
  if (!input_type) return llvm::None;

  bool input_is_qtype =
      input_type.getElementType().isa<mlir::quant::UniformQuantizedType>();
  bool output_is_qtype =
      output_type.getElementType().isa<mlir::quant::UniformQuantizedType>();

  if (input_is_qtype || output_is_qtype) {
    op->emitOpError(
        "ConvertReduceProdOp: input/output tensor should "
        "be all floating-point.");
    return llvm::None;
  }

  return convertReduceOpCommon<tosa::ReduceProdOp>(
      rewriter, op, output_type, input_value, axes_elems, keep_dims,
      output_type.getElementType(), false, 1.0f, 0, 1.0f, 0);
}

// Lowers ReduceSum to a sequence of TOSA ops.
llvm::Optional<Value> convertReduceSumOp(
    PatternRewriter& rewriter, Operation* op, RankedTensorType output_type,
    Value input_value, ElementsAttr axes_elems, bool keep_dims) {
  RankedTensorType input_type =
      input_value.getType().dyn_cast<RankedTensorType>();
  if (!input_type) return llvm::None;

  bool input_is_qtype =
      input_type.getElementType().isa<mlir::quant::UniformQuantizedType>();
  bool output_is_qtype =
      output_type.getElementType().isa<mlir::quant::UniformQuantizedType>();

  if (input_is_qtype != output_is_qtype) {
    op->emitOpError(
        "ConvertReduceSumOp: input/output tensor should "
        "be all quantized or all floating-point.");
    return llvm::None;
  }

  double input_scale = 1.0f;
  double output_scale = 1.0f;
  int64_t input_zp = 0;
  int64_t output_zp = 0;
  Type reduce_element_type = input_type.getElementType();

  if (input_is_qtype) {
    auto input_qtype =
        input_type.getElementType().cast<mlir::quant::UniformQuantizedType>();
    auto output_qtype =
        output_type.getElementType().cast<mlir::quant::UniformQuantizedType>();

    int32_t input_shift = 20;

    input_scale =
        static_cast<double>(1 << input_shift) * input_qtype.getScale();
    output_scale =
        1.0 / (output_qtype.getScale() * static_cast<double>(1 << input_shift));

    input_zp = input_qtype.getZeroPoint();
    output_zp = output_qtype.getZeroPoint();
    reduce_element_type = rewriter.getI32Type();
  }

  return convertReduceOpCommon<tosa::ReduceSumOp>(
      rewriter, op, output_type, input_value, axes_elems, keep_dims,
      reduce_element_type, input_is_qtype, input_scale, input_zp, output_scale,
      output_zp);
}

// Lowers ReduceMean to a sequence of TOSA ops.
llvm::Optional<Value> convertReduceMeanOp(
    PatternRewriter& rewriter, Operation* op, RankedTensorType output_type,
    Value input_value, ElementsAttr axes_elems, bool keep_dims) {
  // reduce_mean is lowered as followed:
  // op1 = reduce_sum(input)
  // op2 = mul(op1, 1.0 / num_elements_on_reduced_axis)

  RankedTensorType input_type =
      input_value.getType().dyn_cast<RankedTensorType>();
  if (!input_type) return llvm::None;

  bool input_is_qtype =
      input_type.getElementType().isa<mlir::quant::UniformQuantizedType>();
  bool output_is_qtype =
      output_type.getElementType().isa<mlir::quant::UniformQuantizedType>();

  if (input_is_qtype != output_is_qtype) {
    op->emitOpError(
        "ConvertReduceSumOp: input/output tensor should "
        "be all quantized or all floating-point.");
    return llvm::None;
  }

  // Only supports float type mean() if it's non-quantized
  if (!input_is_qtype && !output_type.getElementType().isa<mlir::FloatType>()) {
    op->emitWarning(
        "Failed convertReduceMean: input unquantized type but output element "
        "not FloatType!");
    return llvm::None;
  }

  int64_t input_rank = input_type.getRank();
  int64_t num_elems_on_reduced_axis = 1;
  for (int i = 0; i < axes_elems.getNumElements(); i++) {
    int64_t axis_val = axes_elems.getValue<IntegerAttr>(i).getInt();
    if (axis_val < 0) axis_val += input_rank;
    num_elems_on_reduced_axis *= input_type.getShape()[axis_val];
  }
  double div_scale = 1.0 / static_cast<double>(num_elems_on_reduced_axis);

  double input_scale = 1.0f;
  double output_scale = 1.0f;
  int64_t input_zp = 0;
  int64_t output_zp = 0;
  Type reduce_element_type = input_type.getElementType();

  if (input_is_qtype) {
    auto input_qtype =
        input_type.getElementType().cast<mlir::quant::UniformQuantizedType>();
    auto output_qtype =
        output_type.getElementType().cast<mlir::quant::UniformQuantizedType>();

    // Combine 'div_scale' as part of output rescale
    output_scale = div_scale * input_qtype.getScale() / output_qtype.getScale();

    input_zp = input_qtype.getZeroPoint();
    output_zp = output_qtype.getZeroPoint();
    reduce_element_type = rewriter.getI32Type();
  }

  auto val = convertReduceOpCommon<tosa::ReduceSumOp>(
      rewriter, op, output_type, input_value, axes_elems, keep_dims,
      reduce_element_type, input_is_qtype, input_scale, input_zp, output_scale,
      output_zp);

  if (!val.hasValue()) return llvm::None;

  if (!input_is_qtype) {
    Value div_const = getTosaConstTensorSingleF32(rewriter, op, div_scale);
    return rewriter
        .create<tosa::MulOp>(op->getLoc(), output_type, val.getValue(),
                             div_const, 0)
        .getResult();
  }

  return val;
}

// Lowers ResizeBilinear and ResizeNearestNeighbor to TOSA resize.
llvm::Optional<Value> convertResizeOp(PatternRewriter& rewriter, Operation* op,
                                      RankedTensorType output_type,
                                      Value input_value, StringRef mode,
                                      bool align_corners,
                                      bool half_pixel_centers) {
  RankedTensorType input_type =
      input_value.getType().dyn_cast<RankedTensorType>();
  if (!input_type) return llvm::None;

  if (input_type.getRank() != 4 || output_type.getRank() != 4) {
    op->emitOpError("convertResizeOp: input/output must be rank 4");
    return llvm::None;
  }

  bool input_is_qtype =
      input_type.getElementType().isa<mlir::quant::UniformQuantizedType>();
  bool output_is_qtype =
      output_type.getElementType().isa<mlir::quant::UniformQuantizedType>();

  if (input_is_qtype != output_is_qtype) {
    op->emitOpError(
        "ConvertResizeOp: input/output tensor should "
        "be all quantized or all floating-point.");
    return llvm::None;
  }

  if (!input_is_qtype) {
    if (!input_type.getElementType().isa<mlir::FloatType>()) {
      op->emitOpError(
          "ConvertResizeOp: only quantized or float types supported.");
      return llvm::None;
    }
  }

  auto input_shape = input_type.getShape();
  auto output_shape = output_type.getShape();

  size_t input_height = input_shape[1];
  size_t input_width = input_shape[2];
  size_t output_height = output_shape[1];
  size_t output_width = output_shape[2];

  double fp_stride_y =
      static_cast<double>(input_height) / static_cast<double>(output_height);
  double fp_stride_x =
      static_cast<double>(input_width) / static_cast<double>(output_width);
  if (align_corners && output_height > 1) {
    fp_stride_y = static_cast<double>(input_height - 1) /
                  static_cast<double>(output_height - 1);
  }
  if (align_corners && output_width > 1) {
    fp_stride_x = static_cast<double>(input_width - 1) /
                  static_cast<double>(output_width - 1);
  }

  double fp_offset_y, fp_offset_x;
  if (half_pixel_centers) {
    fp_offset_y = fp_stride_y * 0.5f - 0.5f;
    fp_offset_x = fp_stride_x * 0.5f - 0.5f;
  } else {
    fp_offset_y = 0.0f;
    fp_offset_x = 0.0f;
  }

  // oh * fp_stride_y + fp_offset_y = ix

  ArrayAttr output_size =
      rewriter.getI64ArrayAttr({static_cast<int64_t>(output_height),
                                static_cast<int64_t>(output_width)});
  StringAttr resize_mode = rewriter.getStringAttr(mode);

  if (input_is_qtype) {
    // Magic shift number TFLite resize bilinear use
    // reference: tensorflow/lite/kernels/internal/reference/reference_ops.h
    int32_t shift = 10;

    // 1.0 is equivalent to (1 << shift) in quantized space.
    // Here we noted as unit = (1 << shift).
    double unit = static_cast<double>(1 << shift);

    // Stride and Offset is int16.
    int32_t stride_y = std::lround(fp_stride_y * unit);
    int32_t stride_x = std::lround(fp_stride_x * unit);
    int32_t offset_y = std::lround(fp_offset_y * unit);
    int32_t offset_x = std::lround(fp_offset_x * unit);

    // Numerically we can decrement shift to let these number fits within 16
    // bits but that's not commonly seen and won't match TFLite reference
    if (stride_y > std::numeric_limits<int16_t>::max() ||
        stride_x > std::numeric_limits<int16_t>::max() ||
        stride_y < std::numeric_limits<int16_t>::min() ||
        stride_x < std::numeric_limits<int16_t>::min() ||
        offset_y > std::numeric_limits<int16_t>::max() ||
        offset_x > std::numeric_limits<int16_t>::max() ||
        offset_y < std::numeric_limits<int16_t>::min() ||
        offset_x < std::numeric_limits<int16_t>::min()) {
      op->emitOpError("OpResize: stride or offset out of 16 bits");
      return llvm::None;
    }

    ArrayAttr stride = rewriter.getI64ArrayAttr({stride_y, stride_x});
    ArrayAttr offset = rewriter.getI64ArrayAttr({offset_y, offset_x});
    IntegerAttr shift_attr = rewriter.getI32IntegerAttr(shift);

    // If quantized bilinear mode, need to lower to RESIZE + RESCALE pair.
    if (mode == "BILINEAR") {
      RankedTensorType output_acc_type;
      auto input_element_qtype =
          input_type.getElementType().cast<mlir::quant::UniformQuantizedType>();

      // TOSA RESIZE: 16 bit input -> 48 bit output, or 8 bit input -> 32 bit
      // output.
      if (input_element_qtype.getStorageTypeIntegralWidth() == 16) {
        output_acc_type = RankedTensorType::get(output_type.getShape(),
                                                rewriter.getIntegerType(48));
      } else if (input_element_qtype.getStorageTypeIntegralWidth() == 8) {
        output_acc_type = RankedTensorType::get(output_type.getShape(),
                                                rewriter.getI32Type());
      } else {
        op->emitOpError("OpResize: support 16-bit and 8-bit quantized input");
        return llvm::None;
      }

      RankedTensorType output_bool_type =
          RankedTensorType::get(output_type.getShape(), rewriter.getI1Type());

      auto resize_op = rewriter.create<tosa::ResizeOp>(
          op->getLoc(), output_acc_type, input_value, output_size, stride,
          offset, shift_attr, rewriter.getF32ArrayAttr({0.0, 0.0}),
          rewriter.getF32ArrayAttr({0.0, 0.0}), resize_mode);

      // TFLite resize_bilinear always assume input and output tensors have same
      // scale That means we only need to arithmetic right shift with (2 *
      // shift)
      // TODO(suderman): Align TFLite rounding behavior
      // TFLite also uses symmetric rounding by doing 'x / (1 << 20)'
      // TOSA arithmetic right shift is doing standard rounding.
      // Right now it's legalized using GreaterEqualOp + SelectOp to conform to
      // TFLite reference. But this eventually should be fixed in TFLite
      // reference
      Value cst_zero = getTosaConstTensorSingleI32(rewriter, op, 0);
      Value cst_twenty = getTosaConstTensorSingleI32(rewriter, op, 20);

      auto ge_op = rewriter.create<tosa::GreaterEqualOp>(
          op->getLoc(), output_bool_type, resize_op.getResult(), cst_zero);

      auto abs_op = rewriter.create<tosa::AbsOp>(op->getLoc(), output_acc_type,
                                                 resize_op.getResult());

      auto rshift_op = rewriter.create<tosa::ArithmeticRightShiftOp>(
          op->getLoc(), output_acc_type, abs_op.getResult(), cst_twenty, true);

      auto negate_op = rewriter.create<tosa::NegateOp>(
          op->getLoc(), output_acc_type, rshift_op.getResult());

      auto select_op = rewriter.create<tosa::SelectOp>(
          op->getLoc(), output_acc_type, ge_op.getResult(),
          rshift_op.getResult(), negate_op.getResult());

      auto cast_op = rewriter.create<tosa::CastOp>(op->getLoc(), output_type,
                                                   select_op.getResult());

      return cast_op.getResult();
    } else if (mode == "NEAREST_NEIGHBOR") {
      auto resize_op = rewriter.create<tosa::ResizeOp>(
          op->getLoc(), output_type, input_value, output_size, stride, offset,
          shift_attr, rewriter.getF32ArrayAttr({0.0, 0.0}),
          rewriter.getF32ArrayAttr({0.0, 0.0}), resize_mode);
      return resize_op.getResult();
    } else {
      op->emitOpError(
          "OpResize: only support BILINEAR or NEAREST_NEIGHBOR mode");
      return llvm::None;
    }
  } else {
    auto resize_op = rewriter.create<tosa::ResizeOp>(
        op->getLoc(), output_type, input_value, output_size,
        rewriter.getI64ArrayAttr({0, 0}), rewriter.getI64ArrayAttr({0, 0}),
        rewriter.getI32IntegerAttr(0),
        rewriter.getF32ArrayAttr(
            {static_cast<float>(fp_stride_y), static_cast<float>(fp_stride_x)}),
        rewriter.getF32ArrayAttr(
            {static_cast<float>(fp_offset_y), static_cast<float>(fp_offset_x)}),
        resize_mode);
    return resize_op.getResult();
  }
}

// Lowers Quantize to a sequence of TOSA quantization ops.
llvm::Optional<Value> convertQuantizeOp(PatternRewriter& rewriter,
                                        Operation* op,
                                        RankedTensorType output_type,
                                        Value input_value, double scale,
                                        int64_t zeropoint) {
  RankedTensorType input_type =
      input_value.getType().dyn_cast<RankedTensorType>();
  if (!input_type) return llvm::None;

  auto output_shape = output_type.getShape();
  auto output_element_type = output_type.getElementType();

  // output element type could only be quantized integer
  if (!output_element_type.isa<mlir::quant::QuantizedType>()) {
    op->emitWarning(
        "Lowering quantizeOp but output element type not quantized!");
    return llvm::None;
  }

  RankedTensorType output_fp_type =
      RankedTensorType::get(output_shape, rewriter.getF32Type());

  Value zp_val =
      getTosaConstTensorSingleF32(rewriter, op, static_cast<float>(zeropoint));

  auto op1_mul_in = rewriter.create<tosa::MulOp>(
      op->getLoc(), output_fp_type, input_value,
      getTosaConstTensorSingleF32(rewriter, op, static_cast<float>(scale)), 0);

  auto op2_add_op1 = rewriter.create<tosa::AddOp>(
      op->getLoc(), output_fp_type, op1_mul_in.getResult(), zp_val);

  auto op3_cast_op2 = rewriter.create<tosa::CastOp>(op->getLoc(), output_type,
                                                    op2_add_op1.getResult());

  return op3_cast_op2.getResult();
}

// Lowers Dequantize to a sequence of TOSA dequantization ops.
llvm::Optional<Value> convertDequantizeOp(PatternRewriter& rewriter,
                                          Operation* op,
                                          RankedTensorType output_type,
                                          Value input_value, double scale,
                                          int64_t zeropoint) {
  RankedTensorType input_type =
      input_value.getType().dyn_cast<RankedTensorType>();
  if (!input_type) return llvm::None;

  // input element type could only be quantized integer
  if (!input_type.getElementType().isa<mlir::quant::QuantizedType>())
    return llvm::None;

  Value zp_val =
      getTosaConstTensorSingleF32(rewriter, op, static_cast<float>(zeropoint));

  auto op1_cast_in =
      rewriter.create<tosa::CastOp>(op->getLoc(), output_type, input_value);

  auto op2_sub_op1 = rewriter.create<tosa::SubOp>(
      op->getLoc(), output_type, op1_cast_in.getResult(), zp_val);

  return rewriter
      .create<tosa::MulOp>(
          op->getLoc(), output_type, op2_sub_op1.getResult(),
          getTosaConstTensorSingleF32(rewriter, op, static_cast<float>(scale)),
          0)
      .getResult();
}

// Lowers FakeQuant to a sequence of TOSA quantization ops.
llvm::Optional<Value> convertFakeQuantOp(PatternRewriter& rewriter,
                                         Operation* op,
                                         RankedTensorType output_type,
                                         Value input_value, double min,
                                         double max, int64_t num_bits,
                                         bool narrow_range) {
  // FakeQuant is lowered as follow:
  // op1 = quantize(input)
  // op2 = dequantize(op1)

  RankedTensorType input_type =
      input_value.getType().dyn_cast<RankedTensorType>();
  if (!input_type) return llvm::None;

  // quantized as INT<num_bits>, where num_bits can only be 8, 16
  if (num_bits != 8 && num_bits != 16) {
    op->emitWarning("FakeQuantOp lowering handles only 8 and 16 for num_bits!");
    return llvm::None;
  }

  // This code originates from tensorflow/core/kernels/fake_quant_ops_functor.h.
  int32_t qmax = (1 << (num_bits)) - 1;
  int32_t qmin = narrow_range ? 1 : 0;

  float nudged_min, nudged_max, nudged_scale;
  tensorflow_nudge(min, max, qmin, qmax, &nudged_min, &nudged_max,
                   &nudged_scale);

  Value cst_min = getTosaConstTensorSingleF32(rewriter, op, nudged_min);
  Value cst_max = getTosaConstTensorSingleF32(rewriter, op, nudged_max);
  Value cst_scale = getTosaConstTensorSingleF32(rewriter, op, nudged_scale);
  Value cst_inv_scale =
      getTosaConstTensorSingleF32(rewriter, op, 1.0f / nudged_scale);
  Value cst_half = getTosaConstTensorSingleF32(rewriter, op, 0.5f);

  // This code originates from tensorflow/core/kernels/fake_quant_ops_functor.h.
  auto op1_min_in = rewriter.create<tosa::MinimumOp>(op->getLoc(), output_type,
                                                     input_value, cst_max);

  auto op2_max_op1 = rewriter.create<tosa::MaximumOp>(
      op->getLoc(), output_type, op1_min_in.getResult(), cst_min);

  auto op3_sub_op2 = rewriter.create<tosa::SubOp>(
      op->getLoc(), output_type, op2_max_op1.getResult(), cst_min);

  auto op4_mul_op3 = rewriter.create<tosa::MulOp>(
      op->getLoc(), output_type, op3_sub_op2.getResult(), cst_inv_scale, 0);

  auto op5_add_op4 = rewriter.create<tosa::AddOp>(
      op->getLoc(), output_type, op4_mul_op3.getResult(), cst_half);

  auto op6_floor_op5 = rewriter.create<tosa::FloorOp>(op->getLoc(), output_type,
                                                      op5_add_op4.getResult());

  auto op7_mul_op6 = rewriter.create<tosa::MulOp>(
      op->getLoc(), output_type, op6_floor_op5.getResult(), cst_scale, 0);

  return rewriter
      .create<tosa::AddOp>(op->getLoc(), output_type, op7_mul_op6.getResult(),
                           cst_min)
      .getResult();
}

llvm::Optional<Value> convertTFConv2DCommon(
    PatternRewriter& rewriter, Operation* op, RankedTensorType output_type,
    Value input, Value filter, Value bias, ArrayAttr strides_attr,
    ArrayAttr dilations_attr, ArrayAttr explicit_padding_attr,
    StringRef padding_ref, StringRef data_format_ref) {
  RankedTensorType input_type = input.getType().dyn_cast<RankedTensorType>();
  RankedTensorType filter_type = filter.getType().dyn_cast<RankedTensorType>();
  // Not a ranked tensor output
  if (!input_type) return llvm::None;
  if (!filter_type) return llvm::None;

  // Transpose [H, W, I, O] to [O, H, W, I]
  auto filter_shape = filter_type.getShape();
  SmallVector<int64_t, 4> a1_transpose_dims;
  a1_transpose_dims.push_back(filter_shape[3]);
  a1_transpose_dims.push_back(filter_shape[0]);
  a1_transpose_dims.push_back(filter_shape[1]);
  a1_transpose_dims.push_back(filter_shape[2]);
  Value a1_filter_transpose_perm =
      get1DConstTensor<tosa::ConstOp, int32_t>(rewriter, op, {3, 0, 1, 2});
  auto a1_filter_transpose_op = rewriter.create<tosa::TransposeOp>(
      op->getLoc(),
      RankedTensorType::get(ArrayRef<int64_t>(a1_transpose_dims),
                            filter_type.getElementType()),
      filter, a1_filter_transpose_perm);

  // Only support NHWC now.
  if (data_format_ref.str() != "NHWC") {
    op->emitWarning("convertTDConv2DCommon only supports NHWC!");
    return llvm::None;
  }

  ArrayAttr stride;
  ArrayAttr dilation;
  ArrayAttr pad;
  {
    if (!strides_attr) {
      stride = rewriter.getI64ArrayAttr({1, 1});
    } else {
      // Note: hardcoded to NHWC for now
      int64_t stride_h = strides_attr[1].cast<IntegerAttr>().getInt();
      int64_t stride_w = strides_attr[2].cast<IntegerAttr>().getInt();
      stride = rewriter.getI64ArrayAttr({stride_h, stride_w});
    }
  }
  {
    if (!dilations_attr) {
      dilation = rewriter.getI64ArrayAttr({1, 1});
    } else {
      // Note: hardcoded to NHWC for now
      int64_t dilation_h = dilations_attr[1].cast<IntegerAttr>().getInt();
      int64_t dilation_w = dilations_attr[2].cast<IntegerAttr>().getInt();
      dilation = rewriter.getI64ArrayAttr({dilation_h, dilation_w});
    }
  }
  {
    tensorflow::Padding tf_pad;
    if (!GetPaddingFromString(padding_ref.str(), &tf_pad).ok()) {
      op->emitWarning("Could not get padding data from padding string term!");
      return llvm::None;
    }

    tensorflow::TensorFormat data_format_tf;
    if (!FormatFromString(data_format_ref.str(), &data_format_tf))
      return llvm::None;

    if (tf_pad == tensorflow::Padding::EXPLICIT) {
      pad = getPaddingValuesFromExplicitPadAttr(explicit_padding_attr,
                                                data_format_tf, rewriter);
    } else {
      if (!getPaddingValuesFromPadType(tf_pad, data_format_tf,
                                       0,  // tensorflow::FORMAT_HWIO
                                       input_type, filter_type, stride,
                                       dilation, rewriter, pad))
        return llvm::None;
    }
  }

  return rewriter
      .create<tosa::Conv2DOp>(op->getLoc(), output_type, input,
                              a1_filter_transpose_op.getResult(), bias, pad,
                              stride, dilation)
      .getResult();
}

// Lowers Gather operators to a sequence of TOSA ops.
llvm::Optional<Value> convertGatherOp(PatternRewriter& rewriter, Operation* op,
                                      Value result_value, Value params_value,
                                      Value indices_value, int32_t batch_dims,
                                      int32_t axis) {
  auto result_type = result_value.getType().dyn_cast<RankedTensorType>();
  auto params_type = params_value.getType().dyn_cast<RankedTensorType>();
  auto indices_type = indices_value.getType().dyn_cast<RankedTensorType>();

  if (!result_type || !params_type || !indices_type) return llvm::None;

  // batch_dims indicates the number of batch dimensions in params and
  // indices axis indicates the axis at which the gather indexing is
  // applied.  axis must be >= batch_dims.  When axis is equal to
  // batch_dims, the right-most batch dimension disappears.
  //
  // N: number of batches
  // Computed as product of params.shape[0:batch_dims-1]
  //
  // W: number of indices in each batch
  // Computed as product of indices.shape[batch_dims:]
  //
  // K: range of each index
  // Computed as  params.shape[axis:axis+rank(indices)-1]
  //
  // C: number of channels for each index
  // Computed as:  LeftChannels * RightChannels:
  // product(params.shape[batch_dims:axis]) * product(params.shape[axis+1:])
  //
  // The params tensor needs to be transposed, then reshaped to move the
  // dimensions into [N, K, C] order.
  //
  // The dimensions of the input params[] tensor are grouped in the following
  // order to begin with:
  //
  //  [Batch, LeftChannels, Indices, RightChannels]
  //  |-----||------------||-------||-------------|
  //     N         C_l         K          C_r
  //
  // Where Batch (N), Indices (K) can be one or more dimensions in size,
  // while LeftChannels and RightChannels represent the group of data channels
  // (C) to the left and right (C_l, C_r) of the indices; the sum of these two
  // is one or more dimensions in size, but either one may be zero depending on
  // how axis was specified by the caller.
  //
  // The resulting tensor will look like:
  //
  //  [Batch, Indices, LeftChannels, RightChannels]
  //  |-----||-------||---------------------------|
  //     N       K                 C
  //
  // The indices tensor simply needs a reshape to flatten all of the
  // batch dimensions (N) together and flatten all of the indices (W)
  // together.
  //
  // Then do the tosa.GATHER
  //
  // output[N,W,C] = tosa.GATHER(values[N,K,C], indices[N,W])
  //
  // Finally, the resulting tensor will have shape [N, W, C], where C is a
  // flattened version of [LeftChannels, RightChannels].  We need to reshape
  // to unflatten to:
  //
  //  [N, W, LeftChannels, RightChannels]
  //
  // and finally transpose back to the output shape
  //
  //  [Batch, LeftChannels, Non-Batch-Indices, RightChannels]

  int N = 1, W = 1, K = 1, C = 1;

  int params_rank = params_type.getShape().size();
  int indices_rank = indices_type.getShape().size();

  if (!(batch_dims <= indices_rank)) {
    op->emitOpError("Batch_dims must be <= indices_rank for a valid gather op");
    return llvm::None;
  }

  if (!(axis >= batch_dims)) {
    op->emitOpError("axis must be >= batch_dims for a valid gather op");
    return llvm::None;
  }

  // Sizes for each of these fields.
  SmallVector<int64_t, 2> params_batch, params_indices, params_left_channels,
      params_right_channels;

  // Dimension indices for each of these fields.
  SmallVector<int64_t, 2> params_idx_batch, params_idx_indices,
      params_idx_left_channels, params_idx_right_channels;

  // Read through the params tensor dimensions left-to-right and extract the
  // different fields.
  for (int i = 0; i < params_rank; i++) {
    // When batch_dims == axis, the batch dimension gets replaced.
    if (i < batch_dims && i < axis) {
      params_batch.push_back(params_type.getShape()[i]);
      params_idx_batch.push_back(i);
    } else if (i < axis) {
      params_left_channels.push_back(params_type.getShape()[i]);
      params_idx_left_channels.push_back(i);
    } else if (i < (axis + 1)) {
      params_indices.push_back(params_type.getShape()[i]);
      params_idx_indices.push_back(i);
    } else {
      params_right_channels.push_back(params_type.getShape()[i]);
      params_idx_right_channels.push_back(i);
    }
  }

  // Calculate N, K, W, C
  for (int i = 0; i < batch_dims; i++) N *= params_type.getShape()[i];

  for (int i = batch_dims; i < indices_rank; i++)
    W *= indices_type.getShape()[i];

  K = params_type.getShape()[axis];

  for (int i = batch_dims; i < axis; i++) C *= params_type.getShape()[i];
  for (int i = (axis + 1); i < params_rank; i++) C *= params_type.getShape()[i];

  // Check for obviously invalid values before doing a divide.
  if (N <= 0 || K <= 0 || W <= 0 || C <= 0) {
    op->emitOpError(
        "N, K, W, or C was calculated as <= zero.  Invalid dimensions for "
        "Gather");
    return llvm::None;
  }

  /////////////////////////////////////////////
  // Build up the params transpose operator
  SmallVector<int32_t, 8> params_transpose_perm;
  SmallVector<int64_t, 2> params_transpose_shape;

  // Batch
  for (int i = 0; i < params_batch.size(); i++) {
    params_transpose_perm.push_back(params_idx_batch[i]);
    params_transpose_shape.push_back(params_batch[i]);
  }

  // Indices
  for (int i = 0; i < params_indices.size(); i++) {
    params_transpose_perm.push_back(params_idx_indices[i]);
    params_transpose_shape.push_back(params_indices[i]);
  }

  // LeftChannels
  for (int i = 0; i < params_left_channels.size(); i++) {
    params_transpose_perm.push_back(params_idx_left_channels[i]);
    params_transpose_shape.push_back(params_left_channels[i]);
  }

  // RightChannels
  for (int i = 0; i < params_right_channels.size(); i++) {
    params_transpose_perm.push_back(params_idx_right_channels[i]);
    params_transpose_shape.push_back(params_right_channels[i]);
  }

  /////////////////////////////////////////////
  // Build up the result reshape, in prepration for transpose
  // [N, W, C] -> [ Batch, Indices, LeftChannels, RightChannels ]
  SmallVector<int64_t, 2> result_reshape_shape;

  // Indices
  for (int i = 0; i < indices_type.getShape().size(); i++) {
    result_reshape_shape.push_back(indices_type.getShape()[i]);
  }

  // Left channels
  for (int i = 0; i < params_left_channels.size(); i++) {
    result_reshape_shape.push_back(params_left_channels[i]);
  }

  // Right channels.  But remove the axis dimension.
  for (int i = 0; i < params_right_channels.size(); i++) {
    result_reshape_shape.push_back(params_right_channels[i]);
  }

  /////////////////////////////////////////////
  // Build up the result transpose operator.
  SmallVector<int32_t, 8> result_transpose_perm;

  // Batch dimensions
  for (int i = 0; i < batch_dims; i++) {
    result_transpose_perm.push_back(i);
  }

  // LeftChannels
  for (int i = 0; i < params_left_channels.size(); i++) {
    result_transpose_perm.push_back(i + indices_type.getShape().size());
  }

  // Indices (remainder of dimensions after batch).
  for (int i = batch_dims; i < (indices_type.getShape().size()); i++) {
    result_transpose_perm.push_back(i);
  }

  // RightChannels, coming from after both the Indices and LeftChannels.
  for (int i = 0; i < params_right_channels.size(); i++) {
    result_transpose_perm.push_back(i + indices_type.getShape().size() +
                                    params_left_channels.size());
  }

  SmallVector<int64_t, 3> tosa_values_shape = {N, K, C};
  SmallVector<int64_t, 2> tosa_indices_shape = {N, W};
  SmallVector<int64_t, 3> tosa_gather_result_shape = {N, W, C};

  auto params_transpose_op = rewriter.create<tosa::TransposeOp>(
      op->getLoc(),
      RankedTensorType::get(ArrayRef<int64_t>(params_transpose_shape),
                            params_type.getElementType()),
      params_value,
      get1DConstTensor<tosa::ConstOp, int32_t>(rewriter, op,
                                               params_transpose_perm));

  auto tosa_values_reshape_op = rewriter.create<tosa::ReshapeOp>(
      op->getLoc(),
      RankedTensorType::get(ArrayRef<int64_t>(tosa_values_shape),
                            params_type.getElementType()),
      params_transpose_op.getResult(),
      rewriter.getI64ArrayAttr(tosa_values_shape));

  auto tosa_indices_reshape_op = rewriter.create<tosa::ReshapeOp>(
      op->getLoc(),
      RankedTensorType::get(ArrayRef<int64_t>(tosa_indices_shape),
                            indices_type.getElementType()),
      indices_value, rewriter.getI64ArrayAttr(tosa_indices_shape));

  auto tosa_gather_op = rewriter.create<tosa::GatherOp>(
      op->getLoc(),
      RankedTensorType::get(ArrayRef<int64_t>(tosa_gather_result_shape),
                            result_type.getElementType()),
      tosa_values_reshape_op.getResult(), tosa_indices_reshape_op.getResult());

  auto tosa_result_reshape_op = rewriter.create<tosa::ReshapeOp>(
      op->getLoc(),
      RankedTensorType::get(ArrayRef<int64_t>(result_reshape_shape),
                            params_type.getElementType()),
      tosa_gather_op.getResult(),
      rewriter.getI64ArrayAttr(result_reshape_shape));

  return rewriter
      .create<tosa::TransposeOp>(op->getLoc(), result_type,
                                 tosa_result_reshape_op.getResult(),
                                 get1DConstTensor<tosa::ConstOp, int32_t>(
                                     rewriter, op, result_transpose_perm))
      .getResult();
}

// Lowers Gather operators to a sequence of TOSA ops.
llvm::Optional<Value> convertGatherNdOp(PatternRewriter& rewriter,
                                        Operation* op, Value result_value,
                                        Value params_value, Value indices_value)

{
  auto result_type = result_value.getType().dyn_cast<RankedTensorType>();
  auto params_type = params_value.getType().dyn_cast<RankedTensorType>();
  auto indices_type = indices_value.getType().dyn_cast<RankedTensorType>();

  if (!result_type || !params_type || !indices_type) return llvm::None;

  // N: number of batches
  // Always 1 for GatherND
  //
  // Because TOSA's GATHER operator already uses the symbol 'N' for
  // the number of batches, we will use the symbol 'ND' to specify the
  // number of dimensions that are sliced from params instead of'N' in
  // the TF MLIR documentation.
  //
  // ND: indices.shape[-1]
  //
  // W: number of indices in each batch
  // Computed as:
  // product(indices.shape[0:-1]) (all but the last dimension)
  //
  // K: range of each index
  // Computed as:
  // product(params.shape[0:ND-1])
  //
  // C: number of channels for each index
  // Computed as:
  // product(params.shape[ND:])
  //
  // The params tensor needs to be reshaped, but not transposed, to move the
  // dimensions into [N, K, C] order.
  //
  // The dimensions of the input params[] tensor are grouped in the following
  // order to begin with:
  //
  //  [ParamIndices, ParamChannels]
  //  |------------||-------------|
  //         K              C
  //
  // The reshape simply flattens the params tensor into a 2D [K, C] shape.
  //
  // Indices needs to be put in the form of [N, W], but a simple flattening
  // will not suffice, because the indices need to index into a [W]-shape
  // vector instead of the params.shape[0:ND-1] tensor that we had before.
  //
  // To flatten the coordinates, first reshape indices to a [W, ND] matrix,
  // where the matrix now represents W ND-dimensional coordinates into the
  // params tensor.
  //
  // From here, we take each of the ND dimensions and multiply it with
  // the the size of the next params dimension (or 1 for the last
  // dimension), then sum all these together with a reduce_sum
  // operator.  This is exactly the same mathematics as one would use
  // flatten the indicies of an N-dimensional row-major array into a
  // 1-D array in C.
  //
  // More precisely, do an element-wise multiply with [params.shape[1
  // .. ND], 1] in axis 1, then reduce_sum in axis 1 to flatten to a
  // [W]-shaped tensor, then trivially reshape to [N=1, W] to be
  // compatible with the GATHER operator's shape.
  //
  // Then perform the tosa.GATHER() operation.
  //
  // Now we have result = [N, K, C].
  //
  // Reshape with a single, simple reshape to the final output shape of:
  //  [Indices, ParamChannels]
  //
  // Where, Indices is indices.shape[0:ND-1]

  int N = 1, W = 1, K = 1, C = 1, ND = 1;

  int params_rank = params_type.getShape().size();
  int indices_rank = indices_type.getShape().size();

  ND = indices_type.getShape()[indices_rank - 1];

  if (ND >= params_rank) {
    op->emitOpError("Size of last dimension on indices must be < params rank");
    return llvm::None;
  }

  // Calculate N, K, W, C.  (N is always 1)
  for (int i = 0; i < (indices_rank - 1); i++) {
    W *= indices_type.getShape()[i];
  }

  for (int i = 0; i < ND; i++) {
    K *= params_type.getShape()[i];
  }

  for (int i = ND; i < params_rank; i++) {
    C *= params_type.getShape()[i];
  }

  SmallVector<int64_t, 3> tosa_values_shape = {N, K, C};
  SmallVector<int64_t, 2> tosa_indices_shape = {N, W};
  SmallVector<int64_t, 2> indices_matrix_shape = {W, ND};
  SmallVector<int64_t, 3> tosa_gather_result_shape = {N, W, C};

  auto tosa_values_reshape_op = rewriter.create<tosa::ReshapeOp>(
      op->getLoc(),
      RankedTensorType::get(ArrayRef<int64_t>(tosa_values_shape),
                            params_type.getElementType()),
      params_value, rewriter.getI64ArrayAttr(tosa_values_shape));

  // Flatten the input indices tensor to an [W, ND] matrix.
  auto indices_matrix_reshape_op = rewriter.create<tosa::ReshapeOp>(
      op->getLoc(),
      RankedTensorType::get(ArrayRef<int64_t>(indices_matrix_shape),
                            indices_type.getElementType()),
      indices_value, rewriter.getI64ArrayAttr(indices_matrix_shape));

  SmallVector<int32_t, 8> flattened_coeff_vec;
  for (int i = 1; i < ND; i++) {
    flattened_coeff_vec.push_back(indices_type.getShape()[i]);
  }
  flattened_coeff_vec.push_back(1);

  Value flattened_coeff_value = get1DConstTensor<tosa::ConstOp, int32_t>(
      rewriter, op, flattened_coeff_vec);

  // Multiply the coefficients by the coordinates
  auto flattened_indices_mul_op = rewriter.create<tosa::MulOp>(
      op->getLoc(),
      RankedTensorType::get(ArrayRef<int64_t>(indices_matrix_shape),
                            indices_type.getElementType()),
      indices_matrix_reshape_op.getResult(), flattened_coeff_value, 0);

  // Sum up the products of the coefficients and coordinates
  auto flattened_indices_reduce_op = rewriter.create<tosa::ReduceSumOp>(
      op->getLoc(),
      RankedTensorType::get(ArrayRef<int64_t>(tosa_indices_shape),
                            indices_type.getElementType()),
      flattened_indices_mul_op.getResult(), rewriter.getI64IntegerAttr(1));

  // And reshape to [N, W]
  auto tosa_indices_reshape_op = rewriter.create<tosa::ReshapeOp>(
      op->getLoc(),
      RankedTensorType::get(ArrayRef<int64_t>(tosa_indices_shape),
                            indices_type.getElementType()),
      flattened_indices_reduce_op.getResult(),
      rewriter.getI64ArrayAttr(tosa_indices_shape));

  // Now the gather op itself
  auto tosa_gather_op = rewriter.create<tosa::GatherOp>(
      op->getLoc(),
      RankedTensorType::get(ArrayRef<int64_t>(tosa_gather_result_shape),
                            result_type.getElementType()),
      tosa_values_reshape_op.getResult(), tosa_indices_reshape_op.getResult());

  // Finally, reshape back to the original output shape of [Indices,
  // ParamChannels].
  return rewriter
      .create<tosa::ReshapeOp>(op->getLoc(), result_type,
                               tosa_gather_op.getResult(),
                               rewriter.getI64ArrayAttr(result_type.getShape()))
      .getResult();
}

// Lowers OneHot operator to a sequence of TOSA ops.
llvm::Optional<Value> convertOneHotOp(PatternRewriter& rewriter, Operation* op,
                                      Value result_value, Value indices_value,
                                      Value on_value, Value off_value,
                                      int32_t depth, int32_t axis) {
  auto result_type = result_value.getType().dyn_cast<RankedTensorType>();
  auto indices_type = indices_value.getType().dyn_cast<RankedTensorType>();
  auto on_value_type = on_value.getType().dyn_cast<RankedTensorType>();
  auto off_value_type = off_value.getType().dyn_cast<RankedTensorType>();

  if (!result_type || !indices_type || !on_value_type || !off_value_type)
    return llvm::None;

  // OneHot operator creates a new tensor with shape indices.shape[:axis] +
  // [depth] + indices.shape[axis:] For each index in 'indices', it needs to be
  // within range of [0, depth - 1] and the [..., k, ...] = on_value (if k =
  // index), or [..., k, ...] = off_value (if k != index)
  //
  // The lowering below assumes depth is always known at compile time.
  // TBD for depth resolved in run time.
  //
  // OneHot can be lowered as TOSA Scatter, where off_value being mapped to
  // 'values_in', on_value being mapped to 'input', and indices naturally mapped
  // to 'indices'. Also the dimensions of TOSA scatter (N, W, K, C) need to be
  // picked.
  //
  // N: number of elements of input indices
  // Computed as:
  // product(indices.shape[:])
  //
  // K: newly added dimension
  // K = depth
  //
  // W, C: dummy dimension now
  // W = C = 1
  //
  // High level description of lowering looks like:
  // 1. off_value is reshaped/tiled into [N, K, C]
  // 2. on_value is reshaped/tiled into [N, W, C]
  // 3. indices is reshaped into [N, W]
  // 4. scatter into [N, K, C]
  // 5. reshaped into [LeftDims, RightDims, K]
  // 6. transpose into [LeftDims, K, RightDims]
  // 7. reshaped to result.shape

  if (on_value_type.getRank() != 0 || off_value_type.getRank() != 0) {
    op->emitOpError("OneHotOp: on_value/off_value needs to be scalar");
    return llvm::None;
  }

  if (axis < -1 || axis > indices_type.getRank()) {
    op->emitOpError("OneHotOp: axis out of valie range [-1, indices.rank]");
    return llvm::None;
  }

  // axis = -1 is equivalent to axis = indices.rank
  if (axis == -1) {
    axis = indices_type.getRank();
  }

  int N = 1, W = 1, C = 1;
  int K = depth;
  int left_dim = 1, right_dim = 1;

  for (int32_t i = 0; i < indices_type.getRank(); i++) {
    int32_t dim = indices_type.getShape()[i];
    N *= dim;
    if (i >= axis) {
      right_dim *= dim;
    } else {
      left_dim *= dim;
    }
  }

  // Reshape on_value to [1, 1, 1]
  auto op1_reshape_on_value = rewriter.create<tosa::ReshapeOp>(
      op->getLoc(),
      RankedTensorType::get({1, 1, 1}, on_value_type.getElementType()),
      on_value, rewriter.getI64ArrayAttr({1, 1, 1}));

  // And tile to [N, W, C]
  auto op2_tile_op1 = rewriter.create<tosa::TileOp>(
      op->getLoc(),
      RankedTensorType::get({N, W, C}, on_value_type.getElementType()),
      op1_reshape_on_value.getResult(), rewriter.getI64ArrayAttr({N, W, C}));

  // Reshape off_value to [1, 1, 1]
  auto op3_reshape_off_value = rewriter.create<tosa::ReshapeOp>(
      op->getLoc(),
      RankedTensorType::get({1, 1, 1}, off_value_type.getElementType()),
      off_value, rewriter.getI64ArrayAttr({1, 1, 1}));

  // And tile to [N, K, C]
  auto op4_tile_op3 = rewriter.create<tosa::TileOp>(
      op->getLoc(),
      RankedTensorType::get({N, K, C}, on_value_type.getElementType()),
      op3_reshape_off_value.getResult(), rewriter.getI64ArrayAttr({N, K, C}));

  // Reshape indices to [N, W]
  auto op5_reshape_indices = rewriter.create<tosa::ReshapeOp>(
      op->getLoc(),
      RankedTensorType::get({N, W}, indices_type.getElementType()),
      indices_value, rewriter.getI64ArrayAttr({N, W}));

  // Scatter to [N, K, C]
  auto op6_scatter_op4_op5_op2 = rewriter.create<tosa::ScatterOp>(
      op->getLoc(),
      RankedTensorType::get({N, K, C}, result_type.getElementType()),
      op4_tile_op3.getResult(), op5_reshape_indices.getResult(),
      op2_tile_op1.getResult());

  // Reshaped to [LeftDims, RightDims, K]. C being squeezed out since it's 1.
  auto op7_reshape_op6 = rewriter.create<tosa::ReshapeOp>(
      op->getLoc(),
      RankedTensorType::get({left_dim, right_dim, K},
                            result_type.getElementType()),
      op6_scatter_op4_op5_op2.getResult(),
      rewriter.getI64ArrayAttr({left_dim, right_dim, K}));

  // Transposed to [LeftDims, K, RightDims].
  Value perm_const =
      get1DConstTensor<tosa::ConstOp, int32_t>(rewriter, op, {0, 2, 1});
  auto op8_transpose_op7 = rewriter.create<tosa::TransposeOp>(
      op->getLoc(),
      RankedTensorType::get({left_dim, K, right_dim},
                            result_type.getElementType()),
      op7_reshape_op6.getResult(), perm_const);

  // Reshaped to result.shape.
  return rewriter
      .create<tosa::ReshapeOp>(op->getLoc(), result_type,
                               op8_transpose_op7.getResult(),
                               rewriter.getI64ArrayAttr(result_type.getShape()))
      .getResult();
}

};  // namespace tosa
};  // namespace mlir
