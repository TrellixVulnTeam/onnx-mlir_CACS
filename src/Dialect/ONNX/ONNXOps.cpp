/*
 * SPDX-License-Identifier: Apache-2.0
 */

//===------------------ ONNXOps.cpp - ONNX Operations ---------------------===//
//
// Copyright 2019-2022 The IBM Research Authors.
//
// =============================================================================
//
// This file provides definition of ONNX dialect operations.
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/Traits.h"
#include "mlir/IR/Block.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/DialectImplementation.h"
#include "mlir/IR/IntegerSet.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/PatternMatch.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/FormatVariadic.h"

#include "src/Dialect/ONNX/ONNXEinsumOpHelper.hpp"
#include "src/Dialect/ONNX/ONNXOps.hpp"
#include "src/Dialect/ONNX/ONNXOpsHelper.hpp"
#include "src/Dialect/ONNX/ShapeInference/ONNXShapeHelper.hpp"
#include "src/Support/Diagnostic.hpp"
#include "src/Support/TypeUtilities.hpp"

#include <algorithm>
#include <string>

using namespace mlir;
using namespace mlir::OpTrait::util;
using namespace onnx_mlir;

//===----------------------------------------------------------------------===//
// Tablegen Type Definitions
//===----------------------------------------------------------------------===//
// Explanation: the type implementation is used in dialect initialization.
// If ONNXTypes.cpp.inc is included in ONNXTypes.cpp, compilation error occurs.
#define GET_TYPEDEF_CLASSES
#include "src/Dialect/ONNX/ONNXTypes.cpp.inc"

//===----------------------------------------------------------------------===//
// ONNXDialect initialization
//===----------------------------------------------------------------------===//

/// Dialect creation, the instance will be owned by the context. This is the
/// point of registration of custom types and operations for the dialect.
void ONNXDialect::initialize() {
  addOperations<
#define GET_OP_LIST
#include "src/Dialect/ONNX/ONNXOps.cpp.inc"
      >();

  addTypes<
#define GET_TYPEDEF_LIST
#include "src/Dialect/ONNX/ONNXTypes.cpp.inc"
      >();
}

//===----------------------------------------------------------------------===//
// ONNX Helper functions for shape helpers
//===----------------------------------------------------------------------===//

// Get reduction type.
static RankedTensorType getReductionOutputType(
    ShapedType operandTy, Optional<ArrayAttr> axesAttrs, uint64_t keepdims) {
  int64_t rank = operandTy.getRank();

  SmallVector<int64_t, 4> axes;
  if (axesAttrs != llvm::None)
    for (auto axisAttr : axesAttrs.value()) {
      int64_t axis = axisAttr.cast<IntegerAttr>().getInt();
      axis = axis >= 0 ? axis : (rank + axis);
      assert(axis >= -rank && axis <= rank - 1);
      if (std::find(axes.begin(), axes.end(), axis) == axes.end())
        axes.emplace_back(axis);
    }
  else
    for (decltype(rank) i = 0; i < rank; ++i)
      axes.emplace_back(i);

  // Mark reduction axes.
  SmallVector<bool, 4> isReductionAxis;
  for (decltype(rank) i = 0; i < rank; ++i)
    isReductionAxis.emplace_back(
        (std::find(axes.begin(), axes.end(), i) != axes.end()) ? true : false);

  // KeepDims
  bool isKeepdims = (keepdims == 1) ? true : false;

  SmallVector<int64_t, 4> dims;
  for (decltype(rank) i = 0; i < rank; ++i) {
    if (isReductionAxis[i]) {
      if (isKeepdims)
        dims.emplace_back(1); // reduction dimension
    } else
      dims.emplace_back(operandTy.getShape()[i]);
  }

  return RankedTensorType::get(dims, operandTy.getElementType());
}

// Reduction with axes is from ConstantOp.
// Only ReduceSum call this function now.
static RankedTensorType getReductionOutputType(ShapedType operandTy,
    DenseElementsAttr axesAttrs, uint64_t keepdims,
    uint64_t noop_with_empty_axes) {
  int64_t rank = operandTy.getRank();

  SmallVector<int64_t, 4> axes;
  if (axesAttrs)
    for (auto element : axesAttrs.getValues<IntegerAttr>()) {
      int64_t axis = element.getInt();
      if (axis < -rank || axis > rank - 1)
        return RankedTensorType();

      axis = axis >= 0 ? axis : (rank + axis);
      if (std::find(axes.begin(), axes.end(), axis) == axes.end())
        axes.emplace_back(axis);
    }

  if (axes.size() == 0 && !noop_with_empty_axes)
    for (decltype(rank) i = 0; i < rank; ++i)
      axes.emplace_back(i);

  // Mark reduction axes.
  SmallVector<bool, 4> isReductionAxis;
  for (decltype(rank) i = 0; i < rank; ++i)
    isReductionAxis.emplace_back(
        (std::find(axes.begin(), axes.end(), i) != axes.end()) ? true : false);

  // KeepDims
  bool isKeepdims = (keepdims == 1) ? true : false;

  SmallVector<int64_t, 4> dims;
  for (decltype(rank) i = 0; i < rank; ++i) {
    if (isReductionAxis[i]) {
      if (isKeepdims)
        dims.emplace_back(1); // reduction dimension
    } else
      dims.emplace_back(operandTy.getShape()[i]);
  }

  return RankedTensorType::get(dims, operandTy.getElementType());
}

// Handle shapes for operations with a single output.
template <class SHAPE_HELPER, class OP, class ADAPTOR>
static LogicalResult shapeHelperInferShapes(OP &op, Type elementType) {
  SHAPE_HELPER shapeHelper(&op);
  ADAPTOR operandAdaptor(op);
  if (failed(shapeHelper.computeShape(operandAdaptor)))
    return op.emitError("Failed to scan " + OP::getOperationName() +
                        " parameters successfully");

  SmallVector<int64_t, 4> outputDims;
  IndexExpr::getShape(shapeHelper.dimsForOutput(), outputDims);

  updateType(op.getResult(), outputDims, elementType);
  return success();
}

// Handle shapes for operations with multiple outputs.
template <class SHAPE_HELPER, class OP, class ADAPTOR>
static LogicalResult shapeHelperInferMultipleShapes(
    OP &op, TypeRange elementTypes) {
  assert(elementTypes.size() == op.getNumResults() &&
         "Incorrect elementTypes size");

  SHAPE_HELPER shapeHelper(&op);
  ADAPTOR operandAdaptor(op);
  if (failed(shapeHelper.computeShape(operandAdaptor)))
    return op.emitError("Failed to scan " + OP::getOperationName() +
                        " parameters successfully");

  for (unsigned i = 0; i < op.getNumResults(); ++i) {
    SmallVector<int64_t, 4> outputDims;
    IndexExpr::getShape(shapeHelper.dimsForOutput(i), outputDims);
    updateType(op.getResults()[i], outputDims, elementTypes[i]);
  }
  return success();
}

// Handle shape inference for numpy style broadcasting operators.
template <class OP, class ADAPTOR>
static LogicalResult inferShapeForBroadcastingOps(
    OP &op, Type elementType = nullptr) {
  ADAPTOR operandAdaptor(op);
  if (llvm::any_of(operandAdaptor.getOperands(),
          [](const Value &op) { return !hasShapeAndRank(op); }))
    return success(); // cannot infer when the operands shape is not yet known.

  auto resultTy = op.getOperand(0).getType().template cast<ShapedType>();
  for (unsigned i = 1; i < op->getNumOperands(); ++i) {
    auto nextTy = op.getOperand(i).getType().template cast<ShapedType>();
    resultTy = getBroadcastedType(resultTy, nextTy, elementType);
  }

  updateType(op.getResult(), getShape(resultTy), resultTy.getElementType());
  return success();
}

// Handle shape inference for reduction like operators.
template <class OP, class ADAPTOR>
static LogicalResult inferShapeForReductionOps(OP &op) {
  ADAPTOR operandAdaptor(op);
  if (llvm::any_of(operandAdaptor.getOperands(),
          [](const Value &op) { return !hasShapeAndRank(op); }))
    return success(); // cannot infer when the operands shape is not yet known.

  auto operandTy = op.getOperand().getType().template cast<ShapedType>();
  auto resultTy = getReductionOutputType(operandTy, op.axes(), op.keepdims());

  updateType(op.getResult(), getShape(resultTy), resultTy.getElementType());
  return success();
}

#define NOT_IMPLEMENTED_MESSAGE                                                \
  (getOperationName() +                                                        \
      ": is not supported at this time. Please open an issue on "              \
      "https://github.com/onnx/onnx-mlir and/or consider contribute code. "    \
      "Error encountered in shape inference.")

//===----------------------------------------------------------------------===//
// ONNX Helper functions
//===----------------------------------------------------------------------===//

// This method substitutes any uses of dimensions and symbols (e.g.
// dim#0 with dimReplacements[0]) in an affine map, simplifies the modified
// affine map, and returns an integer constant.
static int64_t AffineMapIntConstant(Builder &builder, AffineMap map,
    ArrayRef<int64_t> dimReplacements, ArrayRef<int64_t> symReplacements,
    unsigned numResultDims, unsigned numResultSyms) {
  // Prepare affine expressions.
  SmallVector<AffineExpr, 4> dimExprs, symExprs;
  for (int64_t dim : dimReplacements) {
    AffineExpr exp = builder.getAffineConstantExpr(dim);
    dimExprs.emplace_back(exp);
  }
  for (int64_t sym : symReplacements) {
    AffineExpr exp = builder.getAffineConstantExpr(sym);
    symExprs.emplace_back(exp);
  }
  // Replace all the affine map's arguments with real values and evaluate the
  // map.
  AffineMap replacedDimMap = map.replaceDimsAndSymbols(
      dimExprs, symExprs, numResultDims, numResultSyms);
  AffineMap simplifiedMap = simplifyAffineMap(replacedDimMap);
  return simplifiedMap.getSingleConstantResult();
}

//===----------------------------------------------------------------------===//
// Support function that computes default values for dilations.
//===----------------------------------------------------------------------===//
template <class T>
static LogicalResult processConvDilationParam(
    T *op, Optional<ArrayAttr> kernelShape) {
  auto builder = mlir::Builder(op->getContext());
  auto kernelRank = ArrayAttrSize(kernelShape);

  auto dilationsOpt = op->dilations();
  if (dilationsOpt.has_value()) {
    if (ArrayAttrSize(dilationsOpt) != kernelRank) {
      return op->emitError("dilation rank is not the same as the spatial rank");
    }
    // Test values to be greater than 0.
    for (decltype(kernelRank) i = 0; i < kernelRank; ++i) {
      if (ArrayAttrIntVal(dilationsOpt, i) < 1) {
        return op->emitError("dilation value must be nonzero positive");
      }
    }
  } else {
    // Default dilatation is needed, all dimensions init with 1.
    SmallVector<int64_t, 4> defaultVals(kernelRank, 1);
    // Convert to ArrayRef, then build attribute, then store attribute.
    ArrayRef<int64_t> defaultRefs(defaultVals);
    op->dilationsAttr(builder.getI64ArrayAttr(defaultRefs));
  }
  return success();
}

//===----------------------------------------------------------------------===//
// Support function that computes default values for strides.
//===----------------------------------------------------------------------===//
template <class T>
static LogicalResult processConvStrideParam(
    T *op, Optional<ArrayAttr> kernelShape) {
  auto builder = mlir::Builder(op->getContext());
  auto kernelRank = ArrayAttrSize(kernelShape);

  auto stridesOpt = op->strides();
  if (stridesOpt.has_value()) {
    if (ArrayAttrSize(stridesOpt) != kernelRank)
      return op->emitError("strides rank is not the same as the spatial rank");
    // Check values to be greater than 0.
    for (decltype(kernelRank) i = 0; i < kernelRank; ++i) {
      if (ArrayAttrIntVal(stridesOpt, i) < 1)
        return op->emitError("strides value must be nonzero positive");
    }
  } else {
    // Default stride is needed, all dimensions init with 1.
    SmallVector<int64_t, 4> defaultVals(kernelRank, 1);
    // Convert to ArrayRef, then build attribute, then store attribute.
    ArrayRef<int64_t> defaultRefs(defaultVals);
    op->stridesAttr(builder.getI64ArrayAttr(defaultRefs));
  }
  return success();
}

//===----------------------------------------------------------------------===//
// Support function that computes default values for pads.
//===----------------------------------------------------------------------===//
template <class T>
static LogicalResult processConvPadParam(T *op, ArrayRef<int64_t> inputShape,
    Optional<ArrayAttr> kernelShape, Optional<ArrayAttr> stridesOpt,
    Optional<ArrayAttr> dilationsOpt = llvm::None) {
  auto builder = mlir::Builder(op->getContext());

  auto inputRank = inputShape.size();
  auto kernelRank = ArrayAttrSize(kernelShape);
  auto kernelOffset = inputRank - kernelRank;

  // Try to find padding, getting auto_pad attribute first.
  auto autoPad = op->auto_pad();
  // And then investigate the various different cases. Prefill pad values with
  // zeros, the most common case.
  SmallVector<int64_t, 4> actualPads(2 * kernelRank, 0);
  bool updatedPad = false;
  if (autoPad == "NOTSET") {
    auto padsOpt = op->pads();
    if (padsOpt.has_value()) {
      // Only option where pads are not updated. Pads consists of two entries
      // for each spatial axis.
      if (ArrayAttrSize(padsOpt) != 2 * kernelRank) {
        return op->emitError("pads rank is not twice the spatial rank");
      }
      // Check values, pads cannot be negative.
      for (decltype(kernelRank) i = 0; i < 2 * kernelRank; ++i) {
        if (ArrayAttrIntVal(padsOpt, i) < 0) {
          return op->emitError("pads value must be nonnegative");
        }
      }
    } else {
      // We have notset with no pads, they are assumed to be all zero.
      updatedPad = true;
    }
  } else if (autoPad == "SAME_UPPER" || autoPad == "SAME_LOWER") {
    // Reload dilation and strides as they may have gotten default values.
    updatedPad = true;
    int64_t dilationVal = 1;
    for (decltype(kernelRank) i = 0; i < kernelRank; ++i) {
      auto inputSize = inputShape[kernelOffset + i];
      if (inputSize < 0)
        return op->emitError("Conv Pads defined as SAME_UPPER or SAME_LOWER "
                             "requires compile time X sizes");
      auto kernelSize = ArrayAttrIntVal(kernelShape, i);
      if (dilationsOpt.has_value())
        dilationVal = ArrayAttrIntVal(dilationsOpt, i);
      auto strideVal = ArrayAttrIntVal(stridesOpt, i);
      // Output size is input size divided by stride. When stride is 1, then
      // input and output are the same size, which is the usual case. When
      // stride is greater than 1, take the ceil to be sure to have each input
      // value used, as padding will be used to fill the gaps.
      int64_t outputSize = ceil((1.0 * inputSize) / (1.0 * strideVal));
      // Formula is from ONNX MaxPool, and can be explained as follows. Pads
      // is the difference between the needed values for the computations,
      // minus the input values. The needed values for the computation is the
      // effective side of the kernel plus the number of times we jump to the
      // next kernel. Number of time we jump is (outputSize - 1). That number
      // is multiplied with the size of the jump, namely strideVal. Now for
      // the effective kernel size. It is the kernelSize + the number of times
      // we have dilation holes time the dilation. The number of dilation
      // holes is (kernelSize -1). Thus the effective size is "kernelSize +
      // (kernelSize-1)*dilation". This simplifies to "(kernelSize
      // -1)*dilation + 1".
      auto sumOfPad = (outputSize - 1) * strideVal +
                      ((kernelSize - 1) * dilationVal + 1) - inputSize;
      // Pad values are assumed equal on both size, at half the total value.
      actualPads[i] = actualPads[kernelRank + i] = sumOfPad / 2;
      // But if the total pad value is odd, we add 1 to begining or end
      // depending on autoPad value.
      if (sumOfPad % 2 != 0) {
        if (autoPad == "SAME_UPPER") {
          actualPads[kernelRank + i] += 1;
        } else {
          actualPads[i] += 1;
        }
      }
    }
  } else if (autoPad == "VALID") {
    // No pad, default value was set to zero, we are all set.
    updatedPad = true;
  } else {
    return op->emitError("auto_pad of unknown / unsupported value");
  }
  // Set pads values in attributes, if it is needed.
  if (updatedPad) {
    ArrayRef<int64_t> defaultRefs(actualPads);
    op->padsAttr(builder.getI64ArrayAttr(defaultRefs));
  }
  // In all cases now, the actual pad values are found in the pads attribute.
  op->auto_padAttr(builder.getStringAttr("NOTSET"));
  return success();
}

//===----------------------------------------------------------------------===//
// Support function computing default values for dilations, strides, and pads.
//===----------------------------------------------------------------------===//
template <class T>
static LogicalResult processConvTypeParams(T *op, Value inputOperand) {
  // 1) Get shape of input. Shape is not guaranteed to be compile time constant.
  auto inputShape = inputOperand.getType().cast<RankedTensorType>().getShape();

  // 2) Get kernel_shape attribute. They were previously computed. At this time,
  // they are guranteed to be compile time constant.
  auto kernelShape = op->kernel_shape();

  // Dilation. It is compile time constants (filled to default 1 value if not
  // explicitely given as input).
  LogicalResult res = processConvDilationParam<T>(op, kernelShape);
  if (failed(res))
    return res;
  auto dilationsOpt = op->dilations();

  // Strides. It is compile time constants (filled to default 1 value if not
  // explicitely given as input).
  res = processConvStrideParam<T>(op, kernelShape);
  if (failed(res))
    return res;
  auto stridesOpt = op->strides();

  // Pads.
  return processConvPadParam<T>(
      op, inputShape, kernelShape, stridesOpt, dilationsOpt);
}

//===----------------------------------------------------------------------===//
// Compute spatial dimensions given dilations, strides, pads, and ceil mode.
//===----------------------------------------------------------------------===//
static void insertConvSpatialDim(SmallVector<int64_t, 4> *outputDims,
    Builder &builder, ArrayRef<int64_t> xShape, Optional<ArrayAttr> kernelShape,
    Optional<ArrayAttr> padsOpt, Optional<ArrayAttr> stridesOpt,
    Optional<ArrayAttr> dilationsOpt = llvm::None, bool ceilMode = false) {
  auto spatialRank = ArrayAttrSize(kernelShape);
  auto spatialOffset = xShape.size() - spatialRank;

  // Get an affine map to compute the output dimension.
  AffineMap dimMap = getConvDimMap(builder, ceilMode);
  for (unsigned int i = 0; i < spatialRank; ++i) {
    int64_t res = -1;
    if (xShape[spatialOffset + i] != -1) {
      auto inputSize = xShape[spatialOffset + i];
      auto kernelSize = ArrayAttrIntVal(kernelShape, i);
      auto sumOfPads = ArrayAttrIntVal(padsOpt, i) +
                       ArrayAttrIntVal(padsOpt, spatialRank + i);
      auto strideVal = ArrayAttrIntVal(stridesOpt, i);
      int64_t dilationVal = 1;
      if (dilationsOpt.has_value())
        dilationVal = ArrayAttrIntVal(dilationsOpt, i);
      res = AffineMapIntConstant(builder, dimMap, {inputSize},
          {kernelSize, sumOfPads, strideVal, dilationVal}, 1, 4);
    }
    outputDims->emplace_back(res);
  }
}

//===----------------------------------------------------------------------===//
// Support function that infers shape for RNN operations.
//===----------------------------------------------------------------------===//
template <typename T>
static LogicalResult RNNShapeInference(T *op, int gates) {
  bool batchwiseLayout = op->layout() == 1;

  Value X = op->X();
  Value W = op->W();
  Value R = op->R();

  if (!X.getType().isa<RankedTensorType>() ||
      !W.getType().isa<RankedTensorType>() ||
      !R.getType().isa<RankedTensorType>()) {
    return success();
  }

  auto xTy = X.getType().cast<RankedTensorType>();
  auto elementType = xTy.getElementType();

  // xShape :: [batch_size, seq_length, input_size] if batchwiseLayout
  // xShape :: [seq_length, batch_size, input_size] otherwise
  auto xShape = xTy.getShape();
  // wShape :: [num_dir, gates*hidden_size, input_size]
  auto wShape = W.getType().cast<RankedTensorType>().getShape();
  // rShape :: [num_dir, gates*hidden_size, hidden_size]
  auto rShape = R.getType().cast<RankedTensorType>().getShape();

  if (xShape.size() != 3) {
    return op->emitError("The first input tensor must have rank 3");
  }
  if (wShape.size() != 3) {
    return op->emitError("The second input tensor must have rank 3");
  }
  if (rShape.size() != 3) {
    return op->emitError("The third input tensor must have rank 3");
  }

  // Get sequence length, batch size.
  int64_t seqLength = batchwiseLayout ? xShape[1] : xShape[0];
  int64_t batchSize = batchwiseLayout ? xShape[0] : xShape[1];

  // Get hidden size from hidden_size attribute.
  int64_t hiddenSize = -1;
  if (op->hidden_size().has_value()) {
    hiddenSize = op->hidden_size().value();
  } else {
    // Infer hidden_size from wShape and rShape if possible.
    if (rShape[2] != -1)
      hiddenSize = rShape[2];
    else if (rShape[1] != -1)
      hiddenSize = rShape[1] / gates;
    else if (wShape[1] != -1)
      hiddenSize = wShape[1] / gates;
    // Update hidden_size attribute.
    if (hiddenSize != -1) {
      auto builder = mlir::Builder(op->getContext());
      auto hiddenSizeAttr =
          IntegerAttr::get(builder.getIntegerType(64, /*isSigned=*/true),
              APInt(64, /*value=*/hiddenSize, /*isSigned=*/true));
      op->hidden_sizeAttr(hiddenSizeAttr);
    }
  }

  // Get direction.
  int64_t numDir;
  if ((op->direction() == "forward") || (op->direction() == "reverse"))
    numDir = 1;
  else if (op->direction() == "bidirectional")
    numDir = 2;
  else
    return op->emitError(
        "direction attribute must be one of the strings: forward, "
        "reverse, and bidirectional");

  // Set result types. There are always 2 (RNN, GRU) or 3 results
  // but they are sometimes optional in which case they have NoneType.
  assert((op->getNumResults() == 2 || op->getNumResults() == 3) &&
         "RNN, GRU have 2 results, LSTM has 3");
  // Y :: [batch_size, seq_length, num_dir, hidden_size] if batchwiseLayout
  // Y :: [seq_length, num_dir, batch_size, hidden_size] otherwise
  Type yTy = op->getResult(0).getType();
  if (!yTy.isa<NoneType>()) {
    if (batchwiseLayout) {
      yTy = RankedTensorType::get(
          {batchSize, seqLength, numDir, hiddenSize}, elementType);
    } else {
      yTy = RankedTensorType::get(
          {seqLength, numDir, batchSize, hiddenSize}, elementType);
    }
    op->getResult(0).setType(yTy);
  }
  // Y_h :: [batch_size, num_dir, hidden_size] if batchwiseLayout
  // Y_h :: [num_dir, batch_size, hidden_size] otherwise
  Type yhTy = op->getResult(1).getType();
  if (!yhTy.isa<NoneType>()) {
    if (batchwiseLayout) {
      yhTy =
          RankedTensorType::get({batchSize, numDir, hiddenSize}, elementType);
    } else {
      yhTy =
          RankedTensorType::get({numDir, batchSize, hiddenSize}, elementType);
    }
    op->getResult(1).setType(yhTy);
  }
  if (op->getNumResults() == 3) {
    // Y_c :: [batch_size, num_dir, hidden_size] if batchwiseLayout
    // Y_c :: [num_dir, batch_size, hidden_size] otherwise
    Type ycTy = op->getResult(2).getType();
    if (!ycTy.isa<NoneType>()) {
      if (batchwiseLayout) {
        ycTy =
            RankedTensorType::get({batchSize, numDir, hiddenSize}, elementType);
      } else {
        ycTy =
            RankedTensorType::get({numDir, batchSize, hiddenSize}, elementType);
      }
      op->getResult(2).setType(ycTy);
    }
  }

  return success();
}

static void insertConvTransposeSpatialDim(SmallVectorImpl<int64_t> &outputDims,
    ArrayRef<int64_t> xShape, Optional<ArrayAttr> kernelShape,
    Optional<ArrayAttr> padsOpt, Optional<ArrayAttr> stridesOpt,
    Optional<ArrayAttr> outputPadsOpt, Optional<ArrayAttr> outputShapeOpt,
    Optional<ArrayAttr> dilationsOpt = llvm::None, bool ceilMode = false) {
  auto xRank = xShape.size();
  auto spatialRank = ArrayAttrSize(kernelShape);
  auto spatialOffset = xRank - spatialRank;

  int64_t dilationVal = 1;
  int64_t outputPadsVal = 0;
  // output_shape[i] = stride[i] * (input_size[i] - 1) + output_padding[i] +
  // ((kernel_shape[i] - 1) * dilations[i] + 1) - pads[start_i] - pads[end_i]
  for (unsigned int i = 0; i < spatialRank; ++i) {
    auto inputSize = xShape[spatialOffset + i];
    auto sumOfPads =
        ArrayAttrIntVal(padsOpt, i) + ArrayAttrIntVal(padsOpt, spatialRank + i);
    auto kernelSize = ArrayAttrIntVal(kernelShape, i);
    if (dilationsOpt.has_value())
      dilationVal = ArrayAttrIntVal(dilationsOpt, i);
    auto strideVal = ArrayAttrIntVal(stridesOpt, i);
    if (outputPadsOpt.has_value())
      outputPadsVal = ArrayAttrIntVal(outputPadsOpt, i);
    // Number of useful values: input plus pad - effective size of kernel (see
    // processConvTypeParams comments to see how this value is derived).
    int64_t res = strideVal * (inputSize - 1) + outputPadsVal +
                  ((kernelSize - 1) * dilationVal + 1) - sumOfPads;
    outputDims.emplace_back(res);
  }
}

//===----------------------------------------------------------------------===//
// ONNXArgMaxOp
//===----------------------------------------------------------------------===//

LogicalResult ONNXArgMaxOp::verify() {
  ONNXArgMaxOpAdaptor operandAdaptor(*this);
  if (llvm::any_of(operandAdaptor.getOperands(),
          [](const Value &op) { return !hasShapeAndRank(op); }))
    return success(); // Won't be able to do any checking at this stage.

  int64_t rank = data().getType().cast<ShapedType>().getRank();
  int64_t axisIndex = axis();

  // axis value must be in the range [-rank, rank-1].
  if (axisIndex < -rank || axisIndex >= rank)
    return onnx_mlir::Diagnostic::emitAttributeOutOfRangeError(
        *this->getOperation(), "axis", axisIndex,
        onnx_mlir::Diagnostic::Range<int64_t>(-rank, rank - 1));

  return success();
}

LogicalResult ONNXArgMaxOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  if (!hasShapeAndRank(data()))
    return success();

  // ONNX spec specifies the reduced type as an int64
  auto elementType = IntegerType::get(getContext(), 64);
  return shapeHelperInferShapes<ONNXArgMaxOpShapeHelper, ONNXArgMaxOp,
      ONNXArgMaxOpAdaptor>(*this, elementType);
}

//===----------------------------------------------------------------------===//
// ONNXArgMinOp
//===----------------------------------------------------------------------===//

LogicalResult ONNXArgMinOp::verify() {
  ONNXArgMinOpAdaptor operandAdaptor(*this);
  if (llvm::any_of(operandAdaptor.getOperands(),
          [](const Value &op) { return !hasShapeAndRank(op); }))
    return success(); // Won't be able to do any checking at this stage.

  int64_t rank = data().getType().cast<ShapedType>().getRank();
  int64_t axisIndex = axis();

  // axis value must be in the range [-rank, rank-1].
  if (axisIndex < -rank || axisIndex >= rank)
    return onnx_mlir::Diagnostic::emitAttributeOutOfRangeError(
        *this->getOperation(), "axis", axisIndex,
        onnx_mlir::Diagnostic::Range<int64_t>(-rank, rank - 1));

  return success();
}

LogicalResult ONNXArgMinOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  if (!hasShapeAndRank(data()))
    return success();

  // ONNX spec specifies the reduced type as an int64
  auto elementType = IntegerType::get(getContext(), 64);
  return shapeHelperInferShapes<ONNXArgMinOpShapeHelper, ONNXArgMinOp,
      ONNXArgMinOpAdaptor>(*this, elementType);
}

//===----------------------------------------------------------------------===//
// ONNXEntryPointOp
//===----------------------------------------------------------------------===//

void ONNXEntryPointOp::build(mlir::OpBuilder &builder,
    mlir::OperationState &state, mlir::func::FuncOp function) {
  state.addAttribute(ONNXEntryPointOp::getEntryPointFuncAttrName(),
      SymbolRefAttr::get(function));
}

ONNXEntryPointOp ONNXEntryPointOp::create(
    mlir::Location location, mlir::func::FuncOp &func) {
  mlir::OperationState state(location, "onnx.EntryPoint");
  OpBuilder builder(location->getContext());
  mlir::ONNXEntryPointOp::build(builder, state, func);
  Operation *op = mlir::Operation::create(state);
  auto onnxEntryOp = llvm::cast<mlir::ONNXEntryPointOp>(op);
  return onnxEntryOp;
}

//===----------------------------------------------------------------------===//
// ONNXNoneOp
//===----------------------------------------------------------------------===//

OpFoldResult ONNXNoneOp::fold(ArrayRef<Attribute> operands) {
  return valueAttr();
}

//===----------------------------------------------------------------------===//
// ONNX Operations
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Exp
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXExpOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXExpOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// Atan
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXAtanOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXAtanOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// Tan
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXTanOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXTanOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// Tanh
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXTanhOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXTanhOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// Sin
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXSinOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXSinOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// Sinh
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXSinhOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXSinhOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// Cosh
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXCoshOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXCoshOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// Cos
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXCosOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXCosOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// Acos
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXAcosOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXAcosOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// Acosh
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXAcoshOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXAcoshOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// Asin
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXAsinOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXAsinOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// Asinh
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXAsinhOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXAsinhOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// Atanh
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXAtanhOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXAtanhOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// Log
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXLogOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXLogOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// HardSigmoid
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXHardSigmoidOp. This method is required by
/// the shape inference interface.
LogicalResult ONNXHardSigmoidOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// Sigmoid
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXSigmoidOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXSigmoidOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// Celu
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXCeluOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXCeluOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// Elu
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXEluOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXEluOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// Relu
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXReluOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXReluOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// LeakyRelu
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXLeakyReluOp. This method is required by
/// the shape inference interface.
LogicalResult ONNXLeakyReluOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// Selu
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXSeluOp. This method is required by
/// the shape inference interface.
LogicalResult ONNXSeluOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

// Sequence related operations
// The general form for seq is seq<tensor<*xT>>
// Tensors will be add to or removed from a seq dynamically.
// The tensor type in a seq should be a summary of all the tensor type in
// the seq.
// It is possible seq<tensor<*xT>> can be refined into seq<RankedTensor>,
// or even seq<StaticShapedTensor> if all the tensors have common shape info
// It is important to refine the type for seq in onnx-mlir because static
// type is used. If seq of unranked tensor remains, onnx-mlir can not handle
// the unranked tensor retrieved from the seq.
// Here is the rules for shape inferences of seq-related ops:
// * A seq is started empty as the result of SequenceEmpty. We can track this
//   property with a tag in seq type or along dataflow.
// * When the an element is added, we can merge its shape with that in seq.
// * when an element is removed from seq, the seq becomes empty if it is the
//   last tenor in the seq (known statically).
// Since the seq is usually used as a parameter of a graph (e.g. for LoopOp),
// shape inference for region may need improvement.

namespace {
// Helper function used in Sequence ops shape inference
ShapedType sequenceAddType(
    ShapedType accumulatedType, ShapedType additionalType) {
  Type elementType = accumulatedType.getElementType();
  assert(elementType == additionalType.getElementType() &&
         "types to merge must have the same data type");
  // Pick the weaker attr: known dim > unknown dim > unranked
  if (!accumulatedType.hasRank())
    return accumulatedType;
  if (!additionalType.hasRank())
    return additionalType;
  int64_t rank = accumulatedType.getRank();
  if (rank != additionalType.getRank())
    return UnrankedTensorType::get(elementType);
  ArrayRef<int64_t> acc = accumulatedType.getShape();
  ArrayRef<int64_t> add = additionalType.getShape();
  SmallVector<int64_t, 4> dims;
  for (int64_t i = 0; i < rank; i++) {
    dims.push_back(acc[i] != add[i] ? -1 : add[i]);
  }
  return RankedTensorType::get(dims, elementType);
}
} // namespace

//===----------------------------------------------------------------------===//
// SequenceInsertOp
//===----------------------------------------------------------------------===//

LogicalResult ONNXSequenceInsertOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Merge the tensor type for the seq and the inserted tensor
  SeqType seqType = input_sequence().getType().cast<mlir::SeqType>();
  ShapedType tensorType = tensor().getType().cast<ShapedType>();
  int64_t length = seqType.getLength();
  if (length == 0) {
    // When the input seq is empty, inherit the tensor type
    getResult().setType(SeqType::get(tensorType, 1));
  } else {
    int64_t newLength = length == -1 ? -1 : length + 1;
    ShapedType seqTensorType = seqType.getElementType().cast<ShapedType>();
    seqTensorType = sequenceAddType(seqTensorType, tensorType);
    getResult().setType(SeqType::get(seqTensorType, newLength));
  }
  return success();
}

LogicalResult ONNXSequenceInsertOp::verify() {
  ONNXSequenceInsertOpAdaptor operandAdaptor =
      ONNXSequenceInsertOpAdaptor(*this);

  // These cast should be guaranteed by default verifier
  Type seqElementType = operandAdaptor.input_sequence()
                            .getType()
                            .dyn_cast<mlir::SeqType>()
                            .getElementType();
  Type elementType1 = seqElementType.dyn_cast<ShapedType>().getElementType();
  ShapedType insertType =
      operandAdaptor.tensor().getType().dyn_cast<ShapedType>();
  Type elementType2 = insertType.getElementType();

  if (elementType1 != elementType2) {
    return emitError("Element types of the tensor in seqence and input "
                     "have to be the same");
  }
  return success();
}

//===----------------------------------------------------------------------===//
// SequenceAtOp
//===----------------------------------------------------------------------===//

LogicalResult ONNXSequenceAtOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  auto outputType = getResult().getType();
  auto inputElementType =
      input_sequence().getType().cast<SeqType>().getElementType();
  if (!inputElementType.isa<UnrankedTensorType>() &&
      outputType.isa<UnrankedTensorType>()) {
    getResult().setType(inputElementType);
  }
  return success();
}

//===----------------------------------------------------------------------===//
// SequenceConstructOp
//===----------------------------------------------------------------------===//

LogicalResult ONNXSequenceConstructOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  auto types = inputs().getTypes();
  ShapedType seqTensorType = types[0].cast<ShapedType>();
  for (size_t i = 1; i < types.size(); ++i) {
    seqTensorType = sequenceAddType(seqTensorType, types[i].cast<ShapedType>());
  }
  getResult().setType(SeqType::get(seqTensorType, types.size()));
  return success();
}

//===----------------------------------------------------------------------===//
// SequenceEmptyOp
//===----------------------------------------------------------------------===//

LogicalResult ONNXSequenceEmptyOp::verify() {
  // For the Optional dtypeAttr, the default type is F32
  auto builder = mlir::OpBuilder(getContext());
  Type elementType;
  if (dtypeAttr()) {
    elementType = convertONNXTypeToMLIRType(builder,
        (onnx::TensorProto_DataType)dtypeAttr().getValue().getSExtValue());
  } else {
    elementType = builder.getF32Type();
  }

  // Get element type for seq from the output
  ShapedType outputSeqElementType =
      getResult().getType().cast<SeqType>().getElementType();
  if (outputSeqElementType.getElementType() != elementType)
    return emitError("SequenceEmpty dtype() does not match the output type");
  return success();
}

LogicalResult ONNXSequenceEmptyOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  auto originTy = getResult().getType().cast<SeqType>();
  auto elementTy = originTy.getElementType();
  auto returnTy = SeqType::get(elementTy, 0);
  getResult().setType(returnTy);
  return success();
}

//===----------------------------------------------------------------------===//
// SequenceEraseOp
//===----------------------------------------------------------------------===//

LogicalResult ONNXSequenceEraseOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  auto inputTy = input_sequence().getType().cast<SeqType>();
  int64_t length = inputTy.getLength();

  if (length == 0)
    return emitError("SequenceErase from an empty seq");
  getResult().setType(
      SeqType::get(inputTy.getElementType(), length == -1 ? -1 : length - 1));
  return success();
}

//===----------------------------------------------------------------------===//
// SequenceLengthOp
//===----------------------------------------------------------------------===//

LogicalResult ONNXSequenceLengthOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  Type outputTy = getResult().getType();
  if (!outputTy.isa<RankedTensorType>() ||
      outputTy.cast<RankedTensorType>().getRank() != 0) {
    SmallVector<int64_t, 1> dims;
    auto builder = mlir::Builder(getContext());
    Type scalarTy =
        mlir::RankedTensorType::get(dims, builder.getIntegerType(64));
    getResult().setType(scalarTy);
  }
  // ElementType of I64 will be checked by verifier
  return success();
}

//===----------------------------------------------------------------------===//
// PRelu
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXPReluOp. This method is required by
/// the shape inference interface.
LogicalResult ONNXPReluOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  ONNXPReluOpAdaptor operandAdaptor(*this);
  if (llvm::any_of(operandAdaptor.getOperands(),
          [](const Value &op) { return !hasShapeAndRank(op); }))
    return success();

  auto xShape = X().getType().cast<ShapedType>().getShape();
  auto slopeShape = slope().getType().cast<ShapedType>().getShape();

  // To do unidirectional broadcasting, we first apply bidirectional
  // broadcasting. Then, fine-tune by getting constant dimensions from X.
  SmallVector<int64_t, 4> shape;
  // Bidirectional broadcasting rules.
  getBroadcastedShape(xShape, slopeShape, shape);
  // Fine-tune.
  for (unsigned int i = 0; i < shape.size(); ++i)
    if (xShape[i] != -1)
      shape[i] = xShape[i];

  getResult().setType(RankedTensorType::get(
      shape, X().getType().cast<ShapedType>().getElementType()));
  return success();
}

LogicalResult ONNXPReluOp::verify() {
  ArrayRef<int64_t> xShape = X().getType().cast<ShapedType>().getShape();
  ArrayRef<int64_t> slopeShape =
      slope().getType().cast<ShapedType>().getShape();

  // PRelu supports unidirectional broadcasting, that is slope should be
  // unidirectional broadcastable to input X.
  if (slopeShape.size() > xShape.size())
    return emitError("Slope tensor has a wrong shape");
  return success();
}

//===----------------------------------------------------------------------===//
// ReciprocalOp
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXReciprocalOp. This method is required by
/// the shape inference interface.
LogicalResult ONNXReciprocalOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// SoftmaxOp
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXSoftmaxOp. This method is required by
/// the shape inference interface.
LogicalResult ONNXSoftmaxOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// SoftplusOp
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXSoftplusOp. This method is required by
/// the shape inference interface.
LogicalResult ONNXSoftplusOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// SoftsignOp
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXSoftsignOp. This method is required by
/// the shape inference interface.
LogicalResult ONNXSoftsignOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// SqrtOp
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXSqrtOp. This method is required by
/// the shape inference interface.
LogicalResult ONNXSqrtOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// SignOp
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXSignOp. This method is required by
/// the shape inference interface.
LogicalResult ONNXSignOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// AbsOp
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXAbsOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXAbsOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// ErfOp
//===----------------------------------------------------------------------===//

LogicalResult ONNXErfOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// PowOp
//===----------------------------------------------------------------------===//

LogicalResult ONNXPowOp::verify() {
  ShapedType lhsTy = X().getType().cast<ShapedType>();
  ShapedType rhsTy = Y().getType().cast<ShapedType>();
  Type rhsETy = rhsTy.getElementType();
  Type lhsETy = lhsTy.getElementType();
  if (rhsETy != lhsETy)
    return emitOpError("Pow with different input type not implemented yet");
  if (lhsETy.isa<IntegerType>() || lhsETy.isa<IntegerType>())
    return emitOpError("Integer power not implemented yet");
  return success();
}
LogicalResult ONNXPowOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForBroadcastingOps<ONNXPowOp, ONNXPowOpAdaptor>(*this);
}

//===----------------------------------------------------------------------===//
// AddOp
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXAddOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXAddOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForBroadcastingOps<ONNXAddOp, ONNXAddOpAdaptor>(*this);
}

//===----------------------------------------------------------------------===//
// MulOp
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXMulOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXMulOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForBroadcastingOps<ONNXMulOp, ONNXMulOpAdaptor>(*this);
}

//===----------------------------------------------------------------------===//
// DivOp
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXDivOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXDivOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForBroadcastingOps<ONNXDivOp, ONNXDivOpAdaptor>(*this);
}

//===----------------------------------------------------------------------===//
// SubOp
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXSubOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXSubOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForBroadcastingOps<ONNXSubOp, ONNXSubOpAdaptor>(*this);
}

//===----------------------------------------------------------------------===//
// AndOp
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXAndOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXAndOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForBroadcastingOps<ONNXAndOp, ONNXAndOpAdaptor>(*this);
}

//===----------------------------------------------------------------------===//
// OrOp
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXOrOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXOrOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForBroadcastingOps<ONNXOrOp, ONNXOrOpAdaptor>(*this);
}

//===----------------------------------------------------------------------===//
// XorOp
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXXorOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXXorOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForBroadcastingOps<ONNXXorOp, ONNXXorOpAdaptor>(*this);
}

//===----------------------------------------------------------------------===//
// SumOp
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXSumOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXSumOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForBroadcastingOps<ONNXSumOp, ONNXSumOpAdaptor>(*this);
}

//===----------------------------------------------------------------------===//
// MaxOp
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXMaxOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXMaxOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForBroadcastingOps<ONNXMaxOp, ONNXMaxOpAdaptor>(*this);
}

//===----------------------------------------------------------------------===//
// MinOp
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXMinOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXMinOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForBroadcastingOps<ONNXMinOp, ONNXMinOpAdaptor>(*this);
}

//===----------------------------------------------------------------------===//
// NegOp
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXNegOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXNegOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// IdentityOp
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXIdentityOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXIdentityOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  getResult().setType(getOperand().getType());
  return success();
}

//===----------------------------------------------------------------------===//
// MatMulOp
//===----------------------------------------------------------------------===//

LogicalResult ONNXMatMulOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Cannot infer shape if no shape exists.
  if (!A().getType().isa<RankedTensorType>() ||
      !B().getType().isa<RankedTensorType>())
    return success();

  auto elementType = A().getType().cast<ShapedType>().getElementType();
  return shapeHelperInferShapes<ONNXMatMulOpShapeHelper, ONNXMatMulOp,
      ONNXMatMulOpAdaptor>(*this, elementType);
}

//===----------------------------------------------------------------------===//
// QLinearMatMulOp
//===----------------------------------------------------------------------===//

LogicalResult ONNXQLinearMatMulOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  if (!a().getType().isa<RankedTensorType>() ||
      !b().getType().isa<RankedTensorType>())
    return success();

  auto elementType = getResult().getType().cast<ShapedType>().getElementType();
  return shapeHelperInferShapes<ONNXQLinearMatMulOpShapeHelper,
      ONNXQLinearMatMulOp, ONNXQLinearMatMulOpAdaptor>(*this, elementType);
}

//===----------------------------------------------------------------------===//
// MatMulIntegerOp
//===----------------------------------------------------------------------===//

LogicalResult ONNXMatMulIntegerOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Cannot infer shape if no shape exists.
  if (!A().getType().isa<RankedTensorType>() ||
      !B().getType().isa<RankedTensorType>())
    return success();

  auto elementType = getResult().getType().cast<ShapedType>().getElementType();
  return shapeHelperInferShapes<ONNXMatMulIntegerOpShapeHelper,
      ONNXMatMulIntegerOp, ONNXMatMulIntegerOpAdaptor>(*this, elementType);
}

// GemmOp
LogicalResult ONNXGemmOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  bool hasBias = !C().getType().isa<NoneType>();
  // Cannot infer shape if no shape exists.
  if (!A().getType().isa<RankedTensorType>() ||
      !B().getType().isa<RankedTensorType>() ||
      (hasBias && !C().getType().isa<RankedTensorType>()))
    return success();

  auto elementType = A().getType().cast<ShapedType>().getElementType();
  return shapeHelperInferShapes<ONNXGemmOpShapeHelper, ONNXGemmOp,
      ONNXGemmOpAdaptor>(*this, elementType);
}

/// BatchNormalizationInferenceModeOp
LogicalResult ONNXBatchNormalizationInferenceModeOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Cannot infer shape if no shape exists.
  if (!X().getType().isa<RankedTensorType>() ||
      !scale().getType().isa<RankedTensorType>() ||
      !B().getType().isa<RankedTensorType>() ||
      !mean().getType().isa<RankedTensorType>() ||
      !var().getType().isa<RankedTensorType>())
    return success();

  auto inputTensorTy = X().getType().cast<RankedTensorType>();
  auto scaleTensorTy = scale().getType().cast<RankedTensorType>();
  auto biasTensorTy = B().getType().cast<RankedTensorType>();
  auto meanTensorTy = mean().getType().cast<RankedTensorType>();
  auto varianceTensorTy = var().getType().cast<RankedTensorType>();

  // Check whether the shapes of scale, bias, mean and variance are valid.
  // Operand's dimensions can be in the form of NxCxD1xD2x...xDn or N.
  // In case of N, C is assumed to be 1.
  // 2-D tensors are assumed to be of shape NxC
  // Shapes of scale, bias, mean and variance must be C.
  int64_t c = -1;
  if (inputTensorTy.getShape().size() == 1) {
    c = 1;
  } else if (inputTensorTy.getShape().size() >= 2) {
    c = (inputTensorTy.getShape()[1] != -1) ? inputTensorTy.getShape()[1] : -1;
  }

  if (c != -1) {
    auto s = scaleTensorTy.getShape();
    auto b = biasTensorTy.getShape();
    auto m = meanTensorTy.getShape();
    auto v = varianceTensorTy.getShape();

    if ((s.size() != 1) || (s[0] != -1 && s[0] != c))
      return emitError("Wrong rank for the scale");
    if ((b.size() != 1) || (b[0] != -1 && b[0] != c))
      return emitError("Wrong rank for the bias");
    if ((m.size() != 1) || (m[0] != -1 && m[0] != c))
      return emitError("Wrong rank for the mean");
    if ((v.size() != 1) || (v[0] != -1 && v[0] != c))
      return emitError("Wrong rank for the variance");
  }

  // The output tensor of the same shape as the input.
  getResult().setType(X().getType());
  return success();
}

// TODO:
//   Verify that matrix sizes are valid for multiplication and addition.
//   Take into account the dimensionality of the matrix.

//===----------------------------------------------------------------------===//
// Reshape
//===----------------------------------------------------------------------===//

LogicalResult ONNXReshapeOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Cannot infer shape if no shape tensor is specified.
  if (!data().getType().isa<RankedTensorType>())
    return success();

  if (!shape().getType().isa<RankedTensorType>())
    return success();

  // Only rank 1 shape tensors are supported.
  auto shapeTensorTy = shape().getType().cast<RankedTensorType>();
  if (shapeTensorTy.getShape().size() != 1)
    return emitError("Shape tensor must have rank one");

  // Shape tensor must have constant shape.
  int64_t outputRank = shapeTensorTy.getShape()[0];
  if (outputRank < 0)
    return emitError("Shape tensor must have constant shape");

  auto elementType = data().getType().cast<ShapedType>().getElementType();
  return shapeHelperInferShapes<ONNXReshapeOpShapeHelper, ONNXReshapeOp,
      ONNXReshapeOpAdaptor>(*this, elementType);
}

//===----------------------------------------------------------------------===//
// Transpose
//===----------------------------------------------------------------------===//

LogicalResult ONNXTransposeOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Cannot infer shape if no shape exists.
  if (!data().getType().isa<RankedTensorType>())
    return success();

  auto elementType = data().getType().cast<ShapedType>().getElementType();
  return shapeHelperInferShapes<ONNXTransposeOpShapeHelper, ONNXTransposeOp,
      ONNXTransposeOpAdaptor>(*this, elementType);
}

//===----------------------------------------------------------------------===//
// ONNXTriluOp
//===----------------------------------------------------------------------===//

LogicalResult ONNXTriluOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// ReduceMax
//===----------------------------------------------------------------------===//

LogicalResult ONNXReduceMaxOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForReductionOps<ONNXReduceMaxOp, ONNXReduceMaxOpAdaptor>(
      *this);
}

//===----------------------------------------------------------------------===//
// ReduceMean
//===----------------------------------------------------------------------===//

LogicalResult ONNXReduceMeanOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForReductionOps<ONNXReduceMeanOp, ONNXReduceMeanOpAdaptor>(
      *this);
}

//===----------------------------------------------------------------------===//
// ReduceMin
//===----------------------------------------------------------------------===//

LogicalResult ONNXReduceMinOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForReductionOps<ONNXReduceMinOp, ONNXReduceMinOpAdaptor>(
      *this);
}

//===----------------------------------------------------------------------===//
// ReduceProd
//===----------------------------------------------------------------------===//

LogicalResult ONNXReduceProdOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForReductionOps<ONNXReduceProdOp, ONNXReduceProdOpAdaptor>(
      *this);
}

//===----------------------------------------------------------------------===//
// ReduceL1
//===----------------------------------------------------------------------===//

LogicalResult ONNXReduceL1Op::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForReductionOps<ONNXReduceL1Op, ONNXReduceL1OpAdaptor>(
      *this);
}

//===----------------------------------------------------------------------===//
// ReduceL2
//===----------------------------------------------------------------------===//

LogicalResult ONNXReduceL2Op::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForReductionOps<ONNXReduceL2Op, ONNXReduceL2OpAdaptor>(
      *this);
}

//===----------------------------------------------------------------------===//
// ReduceLogSum
//===----------------------------------------------------------------------===//

LogicalResult ONNXReduceLogSumOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForReductionOps<ONNXReduceLogSumOp,
      ONNXReduceLogSumOpAdaptor>(*this);
}

//===----------------------------------------------------------------------===//
// ReduceLogSumExp
//===----------------------------------------------------------------------===//

LogicalResult ONNXReduceLogSumExpOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForReductionOps<ONNXReduceLogSumExpOp,
      ONNXReduceLogSumExpOpAdaptor>(*this);
}

//===----------------------------------------------------------------------===//
// ReduceSumSquare
//===----------------------------------------------------------------------===//

LogicalResult ONNXReduceSumSquareOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForReductionOps<ONNXReduceSumSquareOp,
      ONNXReduceSumSquareOpAdaptor>(*this);
}

//===----------------------------------------------------------------------===//
// ReduceSum
//===----------------------------------------------------------------------===//

LogicalResult ONNXReduceSumOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  if (!data().getType().isa<RankedTensorType>())
    return success();

  auto operandTy = data().getType().cast<RankedTensorType>();
  /**
   *    In OpSet 13, axes of ReduceSum is an input, not an attribute.
   *    If the axes is not a constant, the output shape is unknown.
   *    So far, only constant input for axes is handled.
   *    Since other reduction ops still have axes as attributes,
   *    interface of getReductionOutputType is kept.
   *    An array attribute is generated from the constant input
   **/
  DenseElementsAttr constAxes;
  if (isFromNone(axes())) {
    // constAxes should just be NULL
    // Default value will be given in getReductionOutputType
  } else if (getONNXConstantOp(axes())) {
    constAxes = getONNXConstantOp(axes())
                    .valueAttr()
                    .dyn_cast_or_null<mlir::DenseElementsAttr>();
    if (!constAxes) {
      return emitError("ReduceSum: expect dense value for axes ");
    }
  } else {
    // When the axis is dynamic, try to infer the rank of output tensor

    // Can not infer when keepdims is false
    if (!keepdims())
      return success();

    if (getResult().getType().isa<RankedTensorType>())
      // Can not improve further
      return success();

    // Output tensor should have the same rank as the input
    // But size of dims is unknown
    auto outputNumDim = operandTy.getShape().size();
    SmallVector<int64_t, 4> dims(outputNumDim, -1);
    getResult().setType(
        RankedTensorType::get(dims, operandTy.getElementType()));
    return success();
  }

  RankedTensorType type = getReductionOutputType(
      operandTy, constAxes, keepdims(), noop_with_empty_axes());
  if (!type)
    return emitError("unknown shape");
  getResult().setType(type);
  return success();
}

LogicalResult ONNXReduceSumV11Op::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForReductionOps<ONNXReduceSumV11Op,
      ONNXReduceSumV11OpAdaptor>(*this);
}

//===----------------------------------------------------------------------===//
// Conv
//===----------------------------------------------------------------------===//

// For ops without filter, pass nullptr in filterOperand.
template <class T>
static LogicalResult verifyKernelShape(T *op, Value filterOperand,
    Optional<ArrayAttr> kernelShapeOpt, int64_t spatialRank) {
  if (filterOperand && !hasShapeAndRank(filterOperand)) {
    // Won't be able to do any checking at this stage.
    return success();
  }
  // 1) Get shape of filter. Shape is not guaranteed to be compile time
  // constant.
  ArrayRef<int64_t> filterShape =
      filterOperand ? filterOperand.getType().cast<ShapedType>().getShape()
                    : ArrayRef<int64_t>();
  // 2) Get kernel_shape attribute
  if (!kernelShapeOpt.has_value()) {
    assert(
        filterOperand && "ops without filter have mandatory kernel_shape arg");
    // Don't have a kernel shape explicitly, still make sure that the filter
    // shape are fine if known. If size is negative, ok since this is runtime.
    // If positive, ok since it must be strictly positive. If zero, that is
    // bad.
    for (int i = 0; i < spatialRank; ++i)
      if (filterShape[2 + i] == 0)
        return op->emitError("Bad spatial filter size: cannot be zero");
    return success();
  }
  // 3) Verify that we have the right number.
  if ((int64_t)ArrayAttrSize(kernelShapeOpt) != spatialRank)
    return op->emitError(
        "kernel_shape length incompatible with spatial dimensions");
  // 4) Verify that they are all positive.
  for (int i = 0; i < spatialRank; ++i) {
    auto attrSize = ArrayAttrIntVal(kernelShapeOpt, i);
    if (attrSize < 1)
      return op->emitError("Bad kernel_shape value: must be strictly positive");
    if (filterOperand) {
      // Has a shape from filter, make sure its consistent.
      auto filterSize = filterShape[2 + i];
      if (filterSize >= 0 && filterSize != attrSize)
        return op->emitError(
            "Bad kernel_shape value: does not match filter sizes");
    }
  }
  return success();
}

template <class T>
static LogicalResult verifyStrides(T *op, int64_t spatialRank) {
  // 1) Get strides attribute.
  auto strides = op->strides();
  if (!strides.has_value())
    return success();
  // 2) Verify that we have the right number.
  if ((int64_t)ArrayAttrSize(strides) != spatialRank)
    return op->emitError("strides length incompatible with spatial dimensions");
  // 3) Verify that they are all positive.
  for (int i = 0; i < spatialRank; ++i) {
    auto attrSize = ArrayAttrIntVal(strides, i);
    if (attrSize < 1)
      return op->emitError("Bad stride value: must be strictly positive");
  }
  return success();
}

template <class T>
static LogicalResult verifyDilations(T *op, int64_t spatialRank) {
  // 1) Get dilation attribute.
  auto dilations = op->dilations();
  if (!dilations.has_value())
    return success();
  // 2) Verify that we have the right number.
  if ((int64_t)ArrayAttrSize(dilations) != spatialRank)
    return op->emitError(
        "dilations length incompatible with spatial dimensions");
  // 3) Verify that they are all positive.
  for (int i = 0; i < spatialRank; ++i) {
    auto attrSize = ArrayAttrIntVal(dilations, i);
    if (attrSize < 1)
      return op->emitError("Bad dilation value: must be strictly positive");
  }
  return success();
}

template <class T>
static LogicalResult verifyPadding(T *op, int64_t spatialRank) {
  // Verify auto pad field.
  auto autoPad = op->auto_pad();
  if (autoPad == "SAME_UPPER" || autoPad == "SAME_LOWER" ||
      autoPad == "VALID" || autoPad == "NOTSET") {
    // Ok, known auto pad value.
  } else {
    return op->emitError("Unknown auto pad option");
  }
  // Verify pad values, if defined.
  auto pads = op->pads();
  if (!pads.has_value())
    return success();
  // Verify that we have the right number of pad values.
  if ((int32_t)ArrayAttrSize(pads) != 2 * spatialRank)
    return op->emitError("pads length incompatible with spatial dimensions");
  // Verify the values of the pads.
  if (autoPad == "NOTSET") {
    for (int i = 0; i < 2 * spatialRank; ++i)
      if (ArrayAttrIntVal(pads, i) < 0)
        return op->emitError("Bad pad value: must be nonnegative");
  } else {
    for (int i = 0; i < 2 * spatialRank; ++i)
      if (ArrayAttrIntVal(pads, i) != 0)
        return op->emitError("Bad pad value: nonzero pad value only allowed "
                             "with NOTSET option");
  }
  return success();
}

LogicalResult ONNXConvOp::verify() {
  ONNXConvOpAdaptor operandAdaptor = ONNXConvOpAdaptor(*this);
  // Get operands.
  auto X = operandAdaptor.X();
  auto W = operandAdaptor.W();
  auto B = operandAdaptor.B();
  bool hasBias = !B.getType().isa<NoneType>();
  int64_t g = group();
  if (g < 1)
    return emitOpError("group must be strictly positive");
  // Get spatial rank.
  if (!hasShapeAndRank(W)) {
    // Won't be able to do any checking at this stage.
    return success();
  }
  auto wShape = W.getType().cast<ShapedType>().getShape();
  int64_t spatialRank = wShape.size() - 2;
  // If ranked, verify ranks of inputs.
  if (spatialRank < 1)
    return emitOpError("Spatial rank must be strictly positive");

  if (wShape[0] >= 0 && wShape[0] % g != 0) {
    // This rule is not enforced in the spec but is present in Keras,
    // Pytorch, and simplifies the code.
    // Note: Pytorch requires both channel in (CI) and channel out (CO) to be
    // multiple of group number (G).
    // https://pytorch.org/docs/stable/generated/torch.nn.Conv2d.html
    // ONNX clearly states that C (channel in or CI here) is a multiple of group
    // number (G).
    // https://github.com/onnx/onnx/blob/main/docs/Operators.md#Conv
    // Quote: X.shape[1] == (W.shape[1] * group) == C
    // Keras also specifies it: Input channels and filters must both be
    // divisible by groups.
    // https://www.tensorflow.org/api_docs/python/tf/keras/layers/Conv2D
    return emitOpError(
        "Channel Out (M) must be a multiple of the number of groups");
  }
  if (hasShapeAndRank(X)) {
    auto xShape = X.getType().cast<ShapedType>().getShape();
    if ((int64_t)xShape.size() - 2 != spatialRank)
      return emitOpError("Input and filter rank mismatch");
    if (xShape[1] >= 0 && xShape[1] % g != 0)
      return emitOpError(
          "Channel In (C) must be a multiple of the number of groups");
    if (xShape[1] >= 0 && wShape[1] >= 0 && xShape[1] != wShape[1] * g) {
      return emitOpError("Channel In (C) of input must be equal 2nd dim "
                         "of weights times g");
    }
  }
  if (hasBias && hasShapeAndRank(B)) {
    auto bShape = B.getType().cast<ShapedType>().getShape();
    if (bShape.size() != 1)
      return emitOpError("Bias should have a rank of one");
    if (bShape[0] >= 0 && wShape[0] >= 0 && wShape[0] != bShape[0])
      return emitOpError(
          "Bias should have same dimension as first dimension of weights");
  }
  // Verify parameters.
  if (failed(
          verifyKernelShape<ONNXConvOp>(this, W, kernel_shape(), spatialRank)))
    return failure();
  if (failed(verifyStrides<ONNXConvOp>(this, spatialRank)))
    return failure();
  if (failed(verifyDilations<ONNXConvOp>(this, spatialRank)))
    return failure();
  if (failed(verifyPadding<ONNXConvOp>(this, spatialRank)))
    return failure();
  return success();
}

// For this operation, we define the attributes once in the original Conv
// operation class. There is no need to redefine the attribute names for the
// other classes based on Conv.
// Conv attributes output: no changes to the op but the output.
// ShapeHelper get
//   -  dilations, strides: set to 1 if not defined by user;
//   -  kernelShape: inferred from weight matrix if not defined by user;
//   -  pads: set to proper value

LogicalResult ONNXConvOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Generic shape for data input X, weight tensor W, and optional bias B
  // X: (N x C x D1 x D2 ... x Dn)
  // W: (M x C/group x k1 x k2 x ... x kn)
  // B: (M) Optional

  // Cannot infer shape if no shape exists.
  bool hasBias = !B().getType().isa<NoneType>();
  if (!X().getType().isa<RankedTensorType>() ||
      !W().getType().isa<RankedTensorType>() ||
      (hasBias && !B().getType().isa<RankedTensorType>()))
    return success();

  auto elementType = X().getType().cast<ShapedType>().getElementType();
  return shapeHelperInferShapes<ONNXConvOpShapeHelper, ONNXConvOp,
      ONNXConvOpAdaptor>(*this, elementType);
}

//===----------------------------------------------------------------------===//
// ConvTranspose
//===----------------------------------------------------------------------===//

// For this operation, we define the attributes once in the original Conv
// operation class. There is no need to redefine the attribute names for the
// other classes based on Conv.
// Conv attributes output:
//   -  auto_pad set to NOTSET;
//   -  dilations, strides: set to 1 if not defined by user;
//   -  kernelShape: inferred from weight matrix if not defined by user;
//   -  pads: set to proper value, 0 if not defined by user.

LogicalResult ONNXConvTransposeOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Generic shape for data input X, weight tensor W, and optional bias B
  // X: (N x C x D1 x D2 ... x Dn)
  // W: (C x M/group x k1 x k2 x ... x kn)
  // B: (M) Optional

  bool hasBias = !B().getType().isa<NoneType>();

  // Cannot infer shape if no shape exists.
  if (!X().getType().isa<RankedTensorType>() ||
      !W().getType().isa<RankedTensorType>() ||
      (hasBias && !B().getType().isa<RankedTensorType>())) {
    return success();
  }

  auto xTy = X().getType().cast<RankedTensorType>();
  auto xShape = xTy.getShape();
  auto weightTy = W().getType().cast<RankedTensorType>();
  auto weightShape = weightTy.getShape();
  auto builder = mlir::Builder(this->getContext());

  // Lowest supported convolution is a one dimensional convolution.
  if (xShape.size() < 3) {
    return emitError("Data input shape must be at least (NxCxD1)");
  }

  // Check that shape of weight and data have same length.
  if (xShape.size() != weightShape.size()) {
    return emitError("Weight size not compatible with data size");
  }

  // Group is a required attribute and should have default value of 1.
  int64_t group = ONNXConvTransposeOp::group();

  // Check if the attribute actually exists. If it does not then add it.
  if (!groupAttr())
    groupAttr(IntegerAttr::get(builder.getIntegerType(64, /*isSigned=*/true),
        APInt(64, group, /*isSigned=*/true)));

  int64_t inChannels = weightShape[0];
  int64_t outChannels = weightShape[1] * group;

  // Check that the X.shape[1] == W.shape[0] == C && X.shape[1] % group == 0
  // condition holds.
  if (xShape[1] != -1 && inChannels != -1 && xShape[1] != inChannels &&
      xShape[1] % group != 0) {
    return emitOpError("Channel dimension mismatch")
           << xTy << " " << weightTy << " " << group;
  }

  // Check the size of bias.
  if (hasBias) {
    auto bTx = B().getType().cast<RankedTensorType>();
    auto bShape = bTx.getShape();
    if (bShape.size() != 1) {
      return emitError("bias should be one dimensional");
    }
    if (bShape[0] != outChannels) {
      return emitError(
          "bias should have same dimensions as number of output channels");
    }
  }

  // Note: the value of the group attribute only impacts the way the
  // computation is carried out and not the actual output size.

  // Number of spatial dimensions.
  auto spatialOffset = 2;
  int32_t spatialRank = xShape.size() - spatialOffset;

  // Use kernel_shape attribute if present otherwise use size from weight
  // argument.
  auto kernelShape = kernel_shape();
  if (kernelShape.has_value()) {
    if ((int32_t)ArrayAttrSize(kernelShape) != spatialRank) {
      return emitError(
          "kernel_shape length incompatible with spatial dimensions");
    }
    // Have the right number of values, check them.
    for (int i = 0; i < spatialRank; ++i)
      if (ArrayAttrIntVal(kernelShape, i) < 1) {
        return emitError("bad kernel_shape value");
      }
  } else {
    // Deduce shape from weight input.
    SmallVector<int64_t, 2> defaultVals;
    for (int i = 0; i < spatialRank; ++i)
      defaultVals.emplace_back(weightShape[spatialOffset + i]);
    // Convert to ArrayRef, then build attribute, then store attribute.
    ArrayRef<int64_t> defaultRefs(defaultVals);
    auto builder = mlir::Builder(getContext());
    kernel_shapeAttr(builder.getI64ArrayAttr(defaultRefs));
    kernelShape = kernel_shape();
  }

  // Process strides, dilations, and pads.
  LogicalResult res = processConvTypeParams<>(this, X());
  assert(succeeded(res));
  auto dilationsOpt = dilations();
  auto stridesOpt = strides();
  auto padsOpt = pads();
  auto outputPads = output_padding();
  auto outputShape = output_shape();
  // TODO: handle the spatial dimension computation if output shape is
  // specified
  assert(!outputShape.has_value() && "unhandled option in ConvTranspose");

  // First two output dimensions consist of the number of batches and the
  // number of kernels being applied.
  SmallVector<int64_t, 4> outputDims;
  // Insert batch size.
  outputDims.emplace_back(xShape[0]);
  // Insert number of filters being applied (number of output channels *
  // groups).
  outputDims.emplace_back(outChannels);
  // Compute and insert spatial dims.
  insertConvTransposeSpatialDim(outputDims, xShape, kernelShape, padsOpt,
      stridesOpt, outputPads, outputShape, dilationsOpt);

  // Set the output shape if it's not already set
  if (!outputShape.has_value()) {
    output_shapeAttr(builder.getI64ArrayAttr(outputDims));
  }

  getResult().setType(RankedTensorType::get(outputDims, xTy.getElementType()));
  return success();
}

//===----------------------------------------------------------------------===//
// QLinearConv
//===----------------------------------------------------------------------===//

LogicalResult ONNXQLinearConvOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Generic shape for data input X, weight tensor W, and optional bias B
  // X: (N x C x D1 x D2 ... x Dn)
  // W: (M x C/group x k1 x k2 x ... x kn)
  // B: (M) Optional

  bool hasBias = !B().getType().isa<NoneType>();

  // Cannot infer shape if no shape exists.
  if (!x().getType().isa<RankedTensorType>() ||
      !w().getType().isa<RankedTensorType>() ||
      (hasBias && !B().getType().isa<RankedTensorType>()))
    return success();

  auto xTy = x().getType().cast<RankedTensorType>();
  auto xShape = xTy.getShape();
  auto weightTy = w().getType().cast<RankedTensorType>();
  auto weightShape = weightTy.getShape();
  auto builder = mlir::Builder(this->getContext());

  // Lowest supported convolution is a one dimensional convolution.
  if (xShape.size() < 3)
    return emitError("Data input shape must be at least (NxCxD1)");

  // Check that shape of weight and data have same length.
  if (xShape.size() != weightShape.size())
    return emitError("Weight size not compatible with data size");

  // Group is a required attribute and should have default value of 1.
  int64_t group = ONNXQLinearConvOp::group();

  // Check if the attribute actually exists. If it does not then add it.
  if (!groupAttr())
    groupAttr(builder.getI64IntegerAttr(group));

  // Check that the X.shape[1] == (W.shape[1] * group) == C condition holds.
  if (xShape[1] != -1 && weightShape[1] != -1 &&
      xShape[1] != (weightShape[1] * group))
    return emitError("Channel dimension mismatch");

  // Check the size of bias.
  if (hasBias) {
    auto bTx = B().getType().cast<RankedTensorType>();
    auto bShape = bTx.getShape();
    if (bShape.size() != 1)
      return emitError("bias should be one dimensional");
    if (bShape[0] != weightShape[0])
      return emitError("bias should have same dimensions "
                       "as weight's first dimension");
  }

  // Note: the value of the group attribute only impacts the way the
  // computation is carried out and not the actual output size.

  // Number of spatial dimensions.
  auto spatialOffset = 2;
  int32_t spatialRank = xShape.size() - spatialOffset;

  // Use kernel_shape attribute if present otherwise use size from weight
  // argument.
  auto kernelShape = kernel_shape();
  if (kernelShape.has_value()) {
    if ((int32_t)ArrayAttrSize(kernelShape) != spatialRank)
      return emitError(
          "kernel_shape length incompatible with spatial dimensions");
    // Have the right number of values, check them.
    for (int i = 0; i < spatialRank; ++i)
      if (ArrayAttrIntVal(kernelShape, i) < 1)
        return emitError("bad kernel_shape value");
  } else {
    // Deduce shape from weight input.
    SmallVector<int64_t, 2> defaultVals;
    for (int i = 0; i < spatialRank; ++i)
      defaultVals.emplace_back(weightShape[spatialOffset + i]);
    // Convert to ArrayRef, then build attribute, then store attribute.
    ArrayRef<int64_t> defaultRefs(defaultVals);
    auto builder = mlir::Builder(getContext());
    kernel_shapeAttr(builder.getI64ArrayAttr(defaultRefs));
    kernelShape = kernel_shape();
  }

  // Process strides, dilations, and pads.
  LogicalResult res = processConvTypeParams<>(this, x());
  assert(succeeded(res));
  auto dilationsOpt = dilations();
  auto stridesOpt = strides();
  auto padsOpt = pads();

  // First two output dimensions consist of the number of batches and the
  // number of kernels being applied.
  SmallVector<int64_t, 4> outputDims;
  // Insert batch size.
  outputDims.emplace_back(xShape[0]);
  // Insert number of filters being applied (number of output channels).
  outputDims.emplace_back(weightShape[0]);
  // Compute and insert spatial dims.
  insertConvSpatialDim(&outputDims, builder, xShape, kernelShape, padsOpt,
      stridesOpt, dilationsOpt);

  getResult().setType(RankedTensorType::get(outputDims, xTy.getElementType()));
  return success();
}

//===----------------------------------------------------------------------===//
// AveragePool
//===----------------------------------------------------------------------===//

LogicalResult ONNXAveragePoolOp::verify() {
  ONNXAveragePoolOpAdaptor operandAdaptor = ONNXAveragePoolOpAdaptor(*this);

  // Mandatory and unsupported parameters.
  if (!kernel_shape())
    return emitOpError("kernel_shape is a mandatory attribute");
  // Get spatial rank from mandatory kernel_shape parameter.
  int64_t spatialRank = kernel_shape().size();
  if (spatialRank < 1)
    return emitOpError("Spatial rank must be strictly positive");

  // Get operands.
  auto X = operandAdaptor.X();
  if (hasShapeAndRank(X)) {
    auto xShape = X.getType().cast<ShapedType>().getShape();
    if ((int64_t)xShape.size() - 2 != spatialRank)
      return emitOpError("Input and kernel shape rank mismatch");
  }

  // Verify parameters.
  if (failed(verifyKernelShape<ONNXAveragePoolOp>(
          this, nullptr, kernel_shape(), spatialRank)))
    return failure();
  if (failed(verifyStrides<ONNXAveragePoolOp>(this, spatialRank)))
    return failure();
  if (failed(verifyPadding<ONNXAveragePoolOp>(this, spatialRank)))
    return failure();
  return success();
}

LogicalResult ONNXAveragePoolOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Cannot infer shape if no shape exists.
  if (!X().getType().isa<RankedTensorType>())
    return success();

  auto elementType = X().getType().cast<ShapedType>().getElementType();
  return shapeHelperInferShapes<ONNXAveragePoolOpShapeHelper, ONNXAveragePoolOp,
      ONNXAveragePoolOpAdaptor>(*this, elementType);
}

//===----------------------------------------------------------------------===//
// MaxPoolSingleOut
//===----------------------------------------------------------------------===//

LogicalResult ONNXMaxPoolSingleOutOp::verify() {
  ONNXMaxPoolSingleOutOpAdaptor operandAdaptor =
      ONNXMaxPoolSingleOutOpAdaptor(*this);

  // Mandatory and unsupported parameters.
  if (!kernel_shape())
    return emitOpError("kernel_shape is a mandatory attribute");
  // Get spatial rank from mandatory kernel_shape parameter.
  int64_t spatialRank = kernel_shape().size();
  if (spatialRank < 1)
    return emitOpError("Spatial rank must be strictly positive");
  // Not supported for storage order in column major mode.
  if (storage_order() != 0)
    return emitOpError("Column major storage order not implemented yet");

  // Get operands.
  auto X = operandAdaptor.X();
  if (hasShapeAndRank(X)) {
    auto xShape = X.getType().cast<ShapedType>().getShape();
    if (static_cast<int64_t>(xShape.size()) - 2 != spatialRank)
      return emitOpError("Input and kernel shape rank mismatch");
  }

  // Verify parameters.
  if (failed(verifyKernelShape<ONNXMaxPoolSingleOutOp>(
          this, nullptr, kernel_shape(), spatialRank)))
    return failure();
  if (failed(verifyStrides<ONNXMaxPoolSingleOutOp>(this, spatialRank)))
    return failure();
  if (failed(verifyDilations<ONNXMaxPoolSingleOutOp>(this, spatialRank)))
    return failure();
  if (failed(verifyPadding<ONNXMaxPoolSingleOutOp>(this, spatialRank)))
    return failure();
  return success();
}

LogicalResult ONNXMaxPoolSingleOutOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Cannot infer shape if no shape exists.
  if (!X().getType().isa<RankedTensorType>())
    return success();

  // Verify parameters: mandatory for kernel shape.
  auto kernelShape = kernel_shape();
  assert(kernelShape && "verified that we had kernel shape");

  auto elementType = X().getType().cast<ShapedType>().getElementType();
  return shapeHelperInferShapes<ONNXMaxPoolSingleOutOpShapeHelper,
      ONNXMaxPoolSingleOutOp, ONNXMaxPoolSingleOutOpAdaptor>(
      *this, elementType);
}

// Helper function to infer shapes of global pool operations.
template <typename PoolingOp>
static LogicalResult inferShapesGlobalPool(PoolingOp *op) {
  // Cannot infer shape if no shape exists.
  if (!op->X().getType().template isa<RankedTensorType>())
    return success();

  auto xTy = op->X().getType().template cast<RankedTensorType>();
  auto xShape = xTy.getShape();
  xTy.getRank();

  if (xShape.size() < 3) {
    return op->emitError("Data input shape must be at least (NxCxD1)");
  }

  SmallVector<int64_t, 4> outputDims;
  outputDims.emplace_back(xShape[0]);
  outputDims.emplace_back(xShape[1]);
  // Spatial dimensions are reduced to 1.
  outputDims.insert(outputDims.end(), xTy.getRank() - 2, 1);

  op->getResult().setType(
      RankedTensorType::get(outputDims, xTy.getElementType()));
  return success();
}

//===----------------------------------------------------------------------===//
// GlobalAveragePool
//===----------------------------------------------------------------------===//

LogicalResult ONNXGlobalAveragePoolOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapesGlobalPool(this);
}

//===----------------------------------------------------------------------===//
// GlobalLpPool
//===----------------------------------------------------------------------===//

LogicalResult ONNXGlobalLpPoolOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapesGlobalPool(this);
}

//===----------------------------------------------------------------------===//
// GlobalMaxPool
//===----------------------------------------------------------------------===//

LogicalResult ONNXGlobalMaxPoolOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapesGlobalPool(this);
}

//===----------------------------------------------------------------------===//
// Pad
//===----------------------------------------------------------------------===//

LogicalResult ONNXPadOp::verify() {
  ShapedType dataTy = data().getType().cast<ShapedType>();
  Type constTy = constant_value().getType();

  if (!constTy.isa<NoneType>()) {
    // Check that the constant has the same element type as the input
    ShapedType shapedConstTy = constTy.cast<ShapedType>();
    if (dataTy.getElementType() != shapedConstTy.getElementType()) {
      return emitOpError("Pad with constant_value that doesn't match the "
                         "element type of the input.");
    }
  }
  return success();
}

LogicalResult ONNXPadOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Cannot infer shape if no shape exists.
  if (!data().getType().isa<RankedTensorType>() ||
      !pads().getType().isa<RankedTensorType>())
    return success();

  auto elementType = data().getType().cast<ShapedType>().getElementType();
  return shapeHelperInferShapes<ONNXPadOpShapeHelper, ONNXPadOp,
      ONNXPadOpAdaptor>(*this, elementType);
}

//===----------------------------------------------------------------------===//
// Unsqueeze
//===----------------------------------------------------------------------===//

// Update axes attribute so that it contains only positive values.
// Helper functions for both Unsqueeze and Squeeze Ops
template <typename Op>
void updateNegativeAxis(Op *op, ArrayRef<int64_t> axes) {
  OpBuilder builder(op->getContext());
  if (auto axesConstOp = getONNXConstantOp(op->axes())) {
    auto tensorType = axesConstOp.getType().template cast<RankedTensorType>();
    auto constDenseAttr = mlir::DenseElementsAttr::get(tensorType, axes);
    builder.setInsertionPoint(*op);
    auto constOp = builder.create<mlir::ONNXConstantOp>(
        op->getLoc(), mlir::Attribute(), constDenseAttr);
    mlir::Value constRes = constOp.output();
    op->setOperand(1, constRes);
  } else {
    llvm_unreachable("cannot update axes for non-constant Op");
  }
}

template <typename Op>
void updateNegativeAxisV11(Op *op, ArrayRef<int64_t> axes) {
  auto builder = mlir::Builder(op->getContext());
  ArrayRef<int64_t> defaultRefs(axes);
  op->axesAttr(builder.getI64ArrayAttr(defaultRefs));
}

void updateUnsqueezeOpNegativeAxis(
    ONNXUnsqueezeOp *op, ArrayRef<int64_t> axes) {
  updateNegativeAxis(op, axes);
}

void updateUnsqueezeOpNegativeAxis(
    ONNXUnsqueezeV11Op *op, ArrayRef<int64_t> axes) {
  updateNegativeAxisV11(op, axes);
}

template <typename Op, typename Adaptor, typename ShapeHelper>
LogicalResult ONNXUnsqueezeOpInferShapesCommon(Op *op,
    llvm::Optional<ArrayAttr> axisAttrs,
    std::function<void(mlir::Region &)> doShapeInference) {
  if (!op->data().getType().template isa<RankedTensorType>())
    return success();

  auto operandTy = op->data().getType().template cast<RankedTensorType>();
  auto elementType =
      op->data().getType().template cast<ShapedType>().getElementType();
  int64_t inRank = operandTy.getRank();

  if (!axisAttrs)
    return op->emitError("Axes attribute is required");

  SmallVector<int64_t, 4> axes;
  bool hasNegativeAxis = false;
  int64_t outRank = inRank + axisAttrs.value().size();
  for (auto axisAttr : axisAttrs.value()) {
    int64_t axis = axisAttr.cast<IntegerAttr>().getInt();
    if (axis < -outRank || axis >= outRank)
      return op->emitError("Invalid axis value");
    if (axis < 0) {
      axis = outRank + axis;
      hasNegativeAxis = true;
    }
    if (std::find(axes.begin(), axes.end(), axis) == axes.end())
      axes.emplace_back(axis);
    else
      return op->emitError("Duplicated axes");
  }

  if (hasNegativeAxis) {
    updateUnsqueezeOpNegativeAxis(op, axes);
  }

  return shapeHelperInferShapes<ShapeHelper, Op, Adaptor>(*op, elementType);
}

LogicalResult ONNXUnsqueezeOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  auto builder = mlir::Builder(getContext());
  llvm::Optional<ArrayAttr> optionalAttr;
  if (auto axesConstOp = getONNXConstantOp(axes())) {
    auto axesAttr = createArrayAttrFromConstantOp(builder, axesConstOp);
    optionalAttr.emplace(axesAttr);
  } else if (!axes().getType().isa<NoneType>()) {
    // Cannot handle Non-constant axes
    // Hope further transformation may creat constant axes
    return success();
  }
  return ONNXUnsqueezeOpInferShapesCommon<ONNXUnsqueezeOp,
      ONNXUnsqueezeOpAdaptor, ONNXUnsqueezeOpShapeHelper>(
      this, optionalAttr, doShapeInference);
}

LogicalResult ONNXUnsqueezeV11Op::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return ONNXUnsqueezeOpInferShapesCommon<ONNXUnsqueezeV11Op,
      ONNXUnsqueezeV11OpAdaptor, ONNXUnsqueezeV11OpShapeHelper>(
      this, axes(), doShapeInference);
}

//===----------------------------------------------------------------------===//
// Squeeze
//===----------------------------------------------------------------------===//

// Update axes attribute so that it contains only positive values.
void updateSqueezeOpNegativeAxis(ONNXSqueezeOp *op, ArrayRef<int64_t> axes) {
  updateNegativeAxis(op, axes);
}

void updateSqueezeOpNegativeAxis(ONNXSqueezeV11Op *op, ArrayRef<int64_t> axes) {
  updateNegativeAxisV11(op, axes);
}

template <typename Op, typename Adaptor, typename ShapeHelper>
LogicalResult ONNXSqueezeOpInferShapesCommon(Op *op,
    llvm::Optional<ArrayAttr> axisAttrs,
    std::function<void(mlir::Region &)> doShapeInference) {
  auto operandTy = op->data().getType().template cast<RankedTensorType>();
  auto elementType =
      op->data().getType().template cast<ShapedType>().getElementType();
  int64_t inRank = operandTy.getRank();

  SmallVector<int64_t, 4> axes;
  bool hasNegativeAxis = false;
  for (auto axisAttr : axisAttrs.value()) {
    int64_t axis = axisAttr.cast<IntegerAttr>().getInt();
    if (axis < -inRank || axis >= inRank)
      return op->emitError("Invalid axis value");
    if (axis < 0) {
      axis = inRank + axis;
      hasNegativeAxis = true;
    }
    if (std::find(axes.begin(), axes.end(), axis) != axes.end())
      return op->emitError("Duplicated axes");
    axes.emplace_back(axis);
  }

  if (hasNegativeAxis) {
    updateSqueezeOpNegativeAxis(op, axes);
  }

  return shapeHelperInferShapes<ShapeHelper, Op, Adaptor>(*op, elementType);
}

// Helper function to return an ArrayAttr from an input shape
// All single dimensions will be returned
ArrayAttr getSqueezeOpAxesFromShape(
    OpBuilder builder, ArrayRef<int64_t> shape) {
  SmallVector<int64_t, 4> axes;
  for (unsigned int i = 0; i < shape.size(); ++i) {
    if (shape[i] == 1) {
      axes.emplace_back(i);
    } else if (shape[i] == -1) {
      llvm_unreachable(
          "only static input shape currently supported with empty axes");
    }
  }
  return builder.getI64ArrayAttr(axes);
}

LogicalResult ONNXSqueezeOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  auto dataType = data().getType().dyn_cast<RankedTensorType>();
  if (!dataType)
    return success();

  OpBuilder builder(getContext());
  llvm::Optional<ArrayAttr> optionalAttr;

  if (isFromNone(axes())) {
    auto axesAttr = getSqueezeOpAxesFromShape(builder, dataType.getShape());
    optionalAttr.emplace(axesAttr);

    // Create a ConstantOp associated with this Squeeze Op
    auto tensorType =
        RankedTensorType::get(ArrayAttrSize(axesAttr), builder.getI64Type());
    SmallVector<int64_t, 4> values;
    for (auto attr : axesAttr.getValue()) {
      values.emplace_back(attr.cast<IntegerAttr>().getInt());
    }
    auto constDenseAttr =
        DenseElementsAttr::get(tensorType, llvm::makeArrayRef(values));
    builder.setInsertionPoint(*this);
    auto constOp = builder.create<mlir::ONNXConstantOp>(
        getLoc(), mlir::Attribute(), constDenseAttr);
    mlir::Value constRes = constOp.output();
    setOperand(1, constRes);
  } else if (auto axesConstOp = getONNXConstantOp(axes())) {
    auto axesAttr = createArrayAttrFromConstantOp(builder, axesConstOp);
    optionalAttr.emplace(axesAttr);
  } else {
    llvm_unreachable("dynamic axes not yet supported");
  }

  return ONNXSqueezeOpInferShapesCommon<ONNXSqueezeOp, ONNXSqueezeOpAdaptor,
      ONNXSqueezeOpShapeHelper>(this, optionalAttr, doShapeInference);
}

LogicalResult ONNXSqueezeV11Op::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  auto dataType = data().getType().dyn_cast<RankedTensorType>();
  if (!dataType)
    return success();

  if (!axes()) {
    OpBuilder builder(getContext());

    auto newAxesAttr = getSqueezeOpAxesFromShape(builder, dataType.getShape());

    // Update the axes attribute
    axesAttr(newAxesAttr);
  }

  return ONNXSqueezeOpInferShapesCommon<ONNXSqueezeV11Op,
      ONNXSqueezeV11OpAdaptor, ONNXSqueezeV11OpShapeHelper>(
      this, axes(), doShapeInference);
}

//===----------------------------------------------------------------------===//
// Cast
//===----------------------------------------------------------------------===//

LogicalResult ONNXCastOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  ShapedType inputType = input().getType().dyn_cast<RankedTensorType>();
  if (!inputType) {
    return success();
  }

  auto getOutputType = [&inputType](Type elementType) -> Type {
    if (inputType.hasRank()) {
      return RankedTensorType::get(inputType.getShape(), elementType);
    }
    return UnrankedTensorType::get(elementType);
  };

  mlir::Type targetType =
      (*this)->getAttr("to").cast<::mlir::TypeAttr>().getValue();
  OpBuilder builder(getContext());
  getResult().setType(getOutputType(targetType));
  return success();
}

//===----------------------------------------------------------------------===//
// CastLike
//===----------------------------------------------------------------------===//

LogicalResult ONNXCastLikeOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  ShapedType inputType = input().getType().dyn_cast<RankedTensorType>();
  if (!inputType) {
    return success();
  }

  TensorType targetType = target_type().getType().dyn_cast<TensorType>();
  if (!inputType) {
    return success();
  }
  auto targetElementType = targetType.getElementType();

  auto getOutputType = [&inputType](Type elementType) -> Type {
    if (inputType.hasRank()) {
      return RankedTensorType::get(inputType.getShape(), elementType);
    }
    return UnrankedTensorType::get(elementType);
  };

  getResult().setType(getOutputType(targetElementType));
  return success();
}

//===----------------------------------------------------------------------===//
// Scaler
//===----------------------------------------------------------------------===//

LogicalResult ONNXScalerOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  auto inputType = X().getType().dyn_cast<RankedTensorType>();

  if (!inputType)
    return success();

  updateType(
      getResult(), inputType.getShape(), FloatType::getF32(getContext()));
  return success();
}

//===----------------------------------------------------------------------===//
// Constant
//===----------------------------------------------------------------------===//

LogicalResult ONNXConstantOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  if ((sparse_value().has_value() && value().has_value()) ||
      (!sparse_value().has_value() && !value().has_value()))
    return emitError("Require exactly one of the two attributes, "
                     "either value or sparse_value");
  ElementsAttr valAttr;
  if (sparse_value().has_value())
    valAttr = sparse_valueAttr().cast<SparseElementsAttr>();
  else
    valAttr = valueAttr().cast<DenseElementsAttr>();
  getResult().setType(valAttr.getType());
  return success();
}

//===----------------------------------------------------------------------===//
// Concat
//===----------------------------------------------------------------------===//

LogicalResult ONNXConcatOp::verify() {
  // Cannot verify semantics if the operands do not have a known shape yet.
  ONNXConcatOpAdaptor operandAdaptor(*this);
  if (llvm::any_of(operandAdaptor.getOperands(),
          [](const Value &op) { return !hasShapeAndRank(op); }))
    return success(); // Won't be able to do any checking at this stage.

  auto commonType =
      operandAdaptor.getOperands().front().getType().cast<ShapedType>();
  ArrayRef<int64_t> commonShape = commonType.getShape();
  int64_t commonRank = commonShape.size();
  int64_t axisIndex = axis();

  // axis attribute must be in the range [-r,r-1], where r = rank(inputs).
  if (axisIndex < -commonRank || axisIndex >= commonRank)
    return onnx_mlir::Diagnostic::emitAttributeOutOfRangeError(
        *this->getOperation(), "axis", axisIndex,
        onnx_mlir::Diagnostic::Range<int64_t>(-commonRank, commonRank - 1));

  if (axisIndex < 0)
    axisIndex += commonRank;

  // All input tensors must have the same shape, except for the dimension size
  // of the axis to concatenate on.
  for (Value operand : operandAdaptor.getOperands()) {
    ArrayRef<int64_t> operandShape =
        operand.getType().cast<ShapedType>().getShape();
    int64_t operandRank = operandShape.size();
    if (operandRank != commonRank)
      return onnx_mlir::Diagnostic::emitOperandHasUnexpectedRankError(
          *this->getOperation(), operand, operandRank,
          std::to_string(commonRank));

    for (int64_t dim = 0; dim < commonRank; ++dim) {
      if (dim == axisIndex)
        continue;
      if (operandShape[dim] != -1 && commonShape[dim] != -1 &&
          operandShape[dim] != commonShape[dim])
        return onnx_mlir::Diagnostic::emitDimensionHasUnexpectedValueError(
            *this->getOperation(), operand, dim, operandShape[dim],
            std::to_string(commonShape[dim]));
    }
  }

  return success();
}

LogicalResult ONNXConcatOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // The check of constraints is kept
  // However, current check handles dynamic dim only for the concat dim
  int inputNum = getNumOperands();
  for (int i = 0; i < inputNum; ++i) {
    if (!getOperand(i).getType().isa<RankedTensorType>())
      return success();
  }
  // Checking value of axis parameter.
  auto commonType = getOperand(0).getType().cast<RankedTensorType>();
  auto commonShape = commonType.getShape();
  int64_t commonRank = commonShape.size();
  int64_t axisIndex = axis();
  // Negative axis means values are counted from the opposite side.
  if (axisIndex < 0) {
    axisIndex = commonRank + axisIndex;
    // Tong Chen:
    // TOFIX: attribute modification should be into canonicalization
    // I did not move the code into ShapeHelper
    auto builder = mlir::Builder(getContext());
    axisAttr(IntegerAttr::get(builder.getIntegerType(64, /*isSigned=*/true),
        APInt(64, /*value=*/axisIndex, /*isSigned=*/true)));
  }

  return shapeHelperInferShapes<ONNXConcatOpShapeHelper, ONNXConcatOp,
      ONNXConcatOpAdaptor>(*this, commonType.getElementType());
}

//===----------------------------------------------------------------------===//
// ConcatFromSequence
//===----------------------------------------------------------------------===//

LogicalResult ONNXConcatFromSequenceOp::verify() {
  ONNXConcatFromSequenceOpAdaptor operandAdaptor(*this);
  if (!hasShapeAndRank(operandAdaptor.input_sequence()))
    return success(); // Won't be able to do any checking at this stage.

  Value inputSequence = operandAdaptor.input_sequence();
  assert(inputSequence.getType().isa<SeqType>() &&
         "Incorrect type for a sequence");
  auto seqType = inputSequence.getType().cast<SeqType>();
  auto elemType = seqType.getElementType().cast<ShapedType>();
  int64_t rank = elemType.getShape().size();
  int64_t axisIndex = axis();
  int64_t newAxisIndex = new_axis();

  // axis attribute must be in the range [-r,r-1], where r = rank(inputs).
  // When `new_axis` is 1, accepted range is [-r-1,r].
  if (newAxisIndex == 1) {
    if (axisIndex < (-rank - 1) || axisIndex > rank)
      return onnx_mlir::Diagnostic::emitAttributeOutOfRangeError(
          *this->getOperation(), "axis", axisIndex,
          onnx_mlir::Diagnostic::Range<int64_t>(-rank - 1, rank));
  } else {
    if (axisIndex < -rank || axisIndex >= rank)
      return onnx_mlir::Diagnostic::emitAttributeOutOfRangeError(
          *this->getOperation(), "axis", axisIndex,
          onnx_mlir::Diagnostic::Range<int64_t>(-rank, rank - 1));
  }

  return success();
}

LogicalResult ONNXConcatFromSequenceOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return emitError(NOT_IMPLEMENTED_MESSAGE);
}

//===----------------------------------------------------------------------===//
// RNN
//===----------------------------------------------------------------------===//

LogicalResult ONNXRNNOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  int gates = 1;
  return RNNShapeInference(this, gates);
}

//===----------------------------------------------------------------------===//
// LSTM
//===----------------------------------------------------------------------===//

LogicalResult ONNXLSTMOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  int gates = 4;
  return RNNShapeInference(this, gates);
}

//===----------------------------------------------------------------------===//
// GRU
//===----------------------------------------------------------------------===//

LogicalResult ONNXGRUOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  int gates = 3;
  return RNNShapeInference(this, gates);
}

//===----------------------------------------------------------------------===//
// Split
//===----------------------------------------------------------------------===//

LogicalResult ONNXSplitOp::verify() {
  ONNXSplitOpAdaptor operandAdaptor(*this);
  Value input = operandAdaptor.input();
  if (!hasShapeAndRank(input))
    return success(); // Won't be able to do any checking at this stage.

  auto inputType = input.getType().cast<ShapedType>();
  int64_t inputRank = inputType.getShape().size();
  int64_t axisIndex = axis();

  // axis attribute must be in the range [-r,r-1], where r = rank(input).
  if (axisIndex < -inputRank || axisIndex >= inputRank)
    return onnx_mlir::Diagnostic::emitAttributeOutOfRangeError(
        *this->getOperation(), "axis", axisIndex,
        onnx_mlir::Diagnostic::Range<int64_t>(-inputRank, inputRank - 1));

  return success();
}

LogicalResult ONNXSplitOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Cannot infer the output shape if the input shape isn't known yet.
  if (!hasShapeAndRank(input()))
    return success();

  auto inputType = input().getType().cast<ShapedType>();
  Type elementType = inputType.getElementType();
  SmallVector<Type> elementTypes(getNumResults(), elementType);

  return shapeHelperInferMultipleShapes<ONNXSplitOpShapeHelper, ONNXSplitOp,
      ONNXSplitOpAdaptor>(*this, elementTypes);
}

LogicalResult ONNXSplitV11Op::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Cannot infer the output shape if the input shape isn't known yet.
  if (!hasShapeAndRank(input()))
    return success();

  auto inputType = input().getType().cast<ShapedType>();
  Type elementType = inputType.getElementType();
  SmallVector<Type> elementTypes(getNumResults(), elementType);

  return shapeHelperInferMultipleShapes<ONNXSplitV11OpShapeHelper,
      ONNXSplitV11Op, ONNXSplitV11OpAdaptor>(*this, elementTypes);
}

//===----------------------------------------------------------------------===//
// SplitToSequence
//===----------------------------------------------------------------------===//

LogicalResult ONNXSplitToSequenceOp::verify() {
  Value inputValue = input();
  if (!hasShapeAndRank(inputValue))
    return success(); // Won't be able to do any checking at this stage.

  auto inputType = inputValue.getType().cast<ShapedType>();
  ArrayRef<int64_t> inputShape = inputType.getShape();
  int64_t inputRank = inputShape.size();

  int64_t axisIndex = axis();
  // axis attribute must be in the range [-r,r-1], where r = rank(input).
  if (axisIndex < -inputRank || axisIndex >= inputRank)
    return onnx_mlir::Diagnostic::emitAttributeOutOfRangeError(
        *this->getOperation(), "axis", axisIndex,
        onnx_mlir::Diagnostic::Range<int64_t>(-inputRank, inputRank - 1));
  if (axisIndex < 0)
    axisIndex += inputRank;

  Value splitValue = split();
  if (isFromNone(splitValue)) {
    // since split is not specified, check the keepdims attribute
    int64_t keep = keepdims();
    // keepdims must be 0 or 1
    if (keep < 0 || keep > 1)
      return onnx_mlir::Diagnostic::emitAttributeOutOfRangeError(
          *this->getOperation(), "keepdims", keep,
          onnx_mlir::Diagnostic::Range<int64_t>(0, 1));
    return success();
  }
  auto splitType = splitValue.getType().cast<ShapedType>();
  ArrayRef<int64_t> splitShape = splitType.getShape();
  int64_t splitRank = splitShape.size();
  if (splitRank > 1)
    return emitOpError() << ": split has rank " << splitRank << " > 1";
  if (DenseElementsAttr entries =
          getDenseElementAttributeFromONNXValue(splitValue)) {
    if (splitRank == 0) {
      auto scalar = getScalarValue<int64_t>(entries, splitType);
      if (scalar <= 0)
        return emitOpError() << ": split scalar " << scalar << " <= 0";
    } else {
      int64_t sum = 0;
      for (auto entry : entries.getValues<IntegerAttr>()) {
        int64_t i = entry.getInt();
        if (i < 0)
          return emitOpError() << ": split tensor has entry " << i << " < 0";
        sum += i;
      }
      int64_t dimSize = inputShape[axisIndex];
      if (dimSize != -1 && dimSize != sum)
        return emitOpError() << ": split tensor entries sum to " << sum
                             << " != axis dimension size " << dimSize;
    }
  }

  return success();
}

LogicalResult ONNXSplitToSequenceOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  Value inputValue = input();
  if (!hasShapeAndRank(inputValue))
    return success(); // Cannot infer output shape if input shape isn't known.

  // NOTE: all the asserts below are conditions checked in verify()

  auto inputType = inputValue.getType().cast<ShapedType>();
  ArrayRef<int64_t> shape = inputType.getShape();
  int64_t rank = shape.size();
  int64_t axisIndex = axis();
  assert((-rank <= axisIndex && axisIndex < rank) && "axis out of range");
  if (axisIndex < 0)
    axisIndex += rank;
  int64_t dimSize = shape[axisIndex];

  // start with length unknown and dims == shape with unknown dimension size
  // for axis (-1 is ShapedType::kDynamicSize), and edit it as needed below
  int64_t length = -1;
  SmallVector<int64_t, 4> dims(shape.begin(), shape.end());
  dims[axisIndex] = -1;

  Value splitValue = split();
  if (isFromNone(splitValue)) {
    // since split is not specified, check the keepdims attribute
    int64_t keep = keepdims();
    assert(0 <= keep && keep <= 1 && "keepdims out of range");
    length = dimSize;
    if (keep == 1) {
      // if dimSize is zero we can choose any value here, 1 is fine
      dims[axisIndex] = 1;
    } else {
      dims.erase(dims.begin() + axisIndex);
    }
  } else {
    auto splitType = splitValue.getType().cast<ShapedType>();
    ArrayRef<int64_t> splitShape = splitType.getShape();
    int64_t splitRank = splitShape.size();
    assert(splitRank <= 1 && "invalid split tensor rank");
    if (DenseElementsAttr entries =
            getDenseElementAttributeFromONNXValue(splitValue)) {
      if (splitRank == 0) {
        auto scalar = getScalarValue<int64_t>(entries, splitType);
        assert(scalar > 0 && "invalid split scalar");
        if (dimSize != -1) {
          length = dimSize / scalar;
          if ((dimSize % scalar) == 0)
            dims[axisIndex] = scalar;
        }
      } else {
        auto values = entries.getValues<IntegerAttr>();
        length = values.size();
        if (length > 0) {
          // in the (unlikely?) case that all entries are the same, we infer
          // that's the dimension size for axis
          int64_t first = values[0].getInt();
          assert(first >= 0 && "invalid split tensor entry");
          if (llvm::all_of(values, [first](IntegerAttr value) {
                return value.getInt() == first;
              }))
            dims[axisIndex] = first;
        }
      }
    } else if (splitRank == 1 && splitShape[0] != -1) {
      length = splitShape[0];
      // corner case: if the input dimension size for axis is zero, any tensors
      // in the output sequence must also be zero if the sequence is non-empty
      if (length > 0 && dimSize == 0)
        dims[axisIndex] = 0;
      // if length and dimSize are both zero, we can choose any value,
      // leaving it be -1 is fine
    }
  }
  getResult().setType(SeqType::get(
      RankedTensorType::get(dims, inputType.getElementType()), length));
  return success();
}

//===----------------------------------------------------------------------===//
// Flatten
//===----------------------------------------------------------------------===//

LogicalResult ONNXFlattenOp::verify() {
  // Cannot verify constraints if the input shape is not yet known.
  if (!hasShapeAndRank(input()))
    return success();

  auto inputType = input().getType().cast<ShapedType>();
  ArrayRef<int64_t> inputShape = inputType.getShape();
  int64_t inputRank = inputShape.size();
  int64_t axisValue = axis();

  // axis attribute must be in the range [-r,r], where r = rank(input).
  if (axisValue < -inputRank || axisValue > inputRank)
    return onnx_mlir::Diagnostic::emitAttributeOutOfRangeError(
        *this->getOperation(), "axis", axisValue,
        onnx_mlir::Diagnostic::Range<int64_t>(-inputRank, inputRank));

  return success();
}

LogicalResult ONNXFlattenOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Cannot infer the output shape if the input shape is not yet known.
  if (!hasShapeAndRank(input()))
    return success();

  auto elementType = input().getType().cast<ShapedType>().getElementType();
  return shapeHelperInferShapes<ONNXFlattenOpShapeHelper, ONNXFlattenOp,
      ONNXFlattenOpAdaptor>(*this, elementType);
}

//===----------------------------------------------------------------------===//
// Resize
//===----------------------------------------------------------------------===//

LogicalResult ONNXResizeOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  if (!X().getType().isa<RankedTensorType>()) {
    return success();
  }
  auto inputTy = X().getType().cast<RankedTensorType>();

  // Output should at least has the same rank as X input
  if (!getResult().getType().isa<RankedTensorType>()) {
    SmallVector<int64_t, 4> dims(inputTy.getRank(), -1);
    getResult().setType(RankedTensorType::get(dims, inputTy.getElementType()));
  }

  if (isFromNone(scales()) == isFromNone(sizes())) {
    if (isFromNone(scales()))
      return emitError("scales() and sizes() can not be both None");
    else
      return emitError("scales() and sizes() can not be both defined");
  }

  // Current implementation handles constant scales only
  if (!isFromNone(scales())) {
    DenseElementsAttr scalesAttrs =
        getDenseElementAttributeFromONNXValue(scales());
    if (!scalesAttrs) {
      return success();
    }

    SmallVector<float, 4> scalesConstant;
    for (auto scaleAttr : scalesAttrs.getValues<FloatAttr>()) {
      scalesConstant.emplace_back(scaleAttr.getValueAsDouble());
    }

    SmallVector<int64_t, 4> dims;
    for (int i = 0; i < inputTy.getRank(); i++) {
      int newDim;
      if (inputTy.getShape()[i] == -1)
        newDim = -1;
      else
        newDim = inputTy.getShape()[i] * scalesConstant[i];
      dims.emplace_back(newDim);
    }

    updateType(getResult(), dims, inputTy.getElementType());
  } else {
    DenseElementsAttr sizesAttrs =
        getDenseElementAttributeFromONNXValue(sizes());
    if (!sizesAttrs) {
      return success();
    }

    SmallVector<int64_t, 4> sizesConstant;
    for (auto sizeAttr : sizesAttrs.getValues<IntegerAttr>()) {
      sizesConstant.emplace_back(sizeAttr.getInt());
    }

    updateType(getResult(), sizesConstant, inputTy.getElementType());
  }
  return success();
}

//===----------------------------------------------------------------------===//
// DynamicQuantizeLinear
//===----------------------------------------------------------------------===//

LogicalResult ONNXDynamicQuantizeLinearOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  auto inTy = x().getType().dyn_cast<RankedTensorType>();
  if (!inTy) {
    return success();
  }

  auto yTy = y().getType().cast<ShapedType>();
  auto yScaleTy = y_scale().getType().cast<ShapedType>();
  auto yZPTy = y_zero_point().getType().cast<ShapedType>();

  IntegerType ui8Type =
      IntegerType::get(getContext(), 8, IntegerType::Unsigned);
  FloatType f32Type = FloatType::getF32(getContext());

  RankedTensorType scalarType = RankedTensorType::get({}, f32Type);
  RankedTensorType y_zero_point_type = RankedTensorType::get({}, ui8Type);

  // Set the types for the scalars
  if (!yScaleTy.hasStaticShape()) {
    y_scale().setType(scalarType);
  }

  if (!yZPTy.hasStaticShape()) {
    y_zero_point().setType(y_zero_point_type);
  }

  if (!yTy.hasStaticShape()) {
    RankedTensorType outType = RankedTensorType::get(inTy.getShape(), ui8Type);
    y().setType(outType);
  }

  return success();
}

//===----------------------------------------------------------------------===//
// QuantizeLinear
//===----------------------------------------------------------------------===//

LogicalResult ONNXQuantizeLinearOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  auto inTy = x().getType().dyn_cast<RankedTensorType>();
  if (!inTy) {
    return success();
  }

  auto yTy = y().getType().cast<ShapedType>();

  if (!yTy.hasStaticShape()) {
    // TODO: Unfortunately, we can't tell if this should be signed or
    // unsigned here...
    IntegerType i8Type = IntegerType::get(getContext(), 8);
    RankedTensorType outType = RankedTensorType::get(inTy.getShape(), i8Type);
    y().setType(outType);
  }

  return success();
}

//===----------------------------------------------------------------------===//
// DequantizeLinear
//===----------------------------------------------------------------------===//

namespace {
// Returns known length if ty is a non-scalar 1-D vector, otherwise -1.
int64_t nonScalar1DLen(ShapedType ty) {
  if (!ty.hasRank() || ty.getRank() != 1 || ty.isDynamicDim(0))
    return -1;
  int64_t d = ty.getDimSize(0);
  return d == 1 ? -1 : d; // If dim size is 1 then it's considered a scalar.
}
} // namespace

LogicalResult ONNXDequantizeLinearOp::verify() {
  // Is tensor known to be a scalar (rank 0 or rank 1 with 1 element)?
  auto isScalar = [](RankedTensorType t) -> bool {
    return t.getRank() == 0 || (t.getRank() == 1 && t.getDimSize(0) == 1);
  };

  Value scale = x_scale();
  auto scaleTy = scale.getType().cast<ShapedType>();
  if (scaleTy.hasRank() && scaleTy.getRank() > 1)
    return emitOpError("x_scale must be a scalar or 1-D tensor");
  int64_t scaleLen = nonScalar1DLen(scaleTy);

  Value zero = x_zero_point();
  int64_t zeroLen = -1;
  if (!isFromNone(zero)) {
    if (auto zeroTy = zero.getType().dyn_cast<RankedTensorType>()) {
      if (zeroTy.getRank() > 1)
        return emitOpError("x_zero_point must be a scalar or 1-D tensor");
      zeroLen = nonScalar1DLen(zeroTy);
      if (auto scaleTy = scale.getType().dyn_cast<RankedTensorType>()) {
        if ((isScalar(scaleTy) && scaleLen != -1) ||
            (zeroLen != -1 && isScalar(zeroTy)) ||
            (zeroLen != -1 && scaleLen != -1 && zeroLen != scaleLen))
          return emitOpError(
              "x_scale and x_zero_point must have the same shape");
      }
    }

    // TODO: Figure out whether to introduce a variant of this check from the
    // spec ("'x_zero_point' and 'x' must have same type"). It is violated in
    // in the resnet50-v1-12-qdq model where x, x_zero_point are i8, ui8.
    //
    // if (getElementType(x().getType()) != getElementType(zero.getType()))
    //   return emitOpError("x and x_zero_point must have the same data type");

    if (getElementType(zero.getType()).isInteger(32) && zeroLen != 0)
      if (auto values = getDenseElementAttributeFromONNXValue(zero))
        if (!values.isSplat() || !values.getSplatValue<APInt>().isZero())
          return emitOpError("x_zero_point must be 0 for data type int32");
  }

  if (scaleLen == -1 && zeroLen == -1) {
    // Either x_scale or x_zero_point is scalar, so quantization is per-tensor /
    // per layer and axis is ignored and there is nothing more to verify, or
    // their 1-D rank is unknown and we cannot verify more until they are known.
  } else {
    // If x_scale or x_zero_point is a non-scalar 1-D tensor then quantization
    // is per-axis.
    int64_t d = scaleLen != -1 ? scaleLen : zeroLen;
    if (auto xTy = x().getType().dyn_cast<RankedTensorType>()) {
      int64_t r = xTy.getRank();
      // axis attribute must be in the range [-r,r-1].
      int64_t a = axis();
      if (a < -r || a >= r)
        return onnx_mlir::Diagnostic::emitAttributeOutOfRangeError(
            *this->getOperation(), "axis", a,
            onnx_mlir::Diagnostic::Range<int64_t>(-r, r - 1));
      if (a < 0)
        a += r;
      if (!xTy.isDynamicDim(a) && xTy.getDimSize(a) != d)
        return emitOpError("x_scale and x_zero_point 1-D tensor length must "
                           "match the input axis dim size");
    } else {
      // Cannot verify more until x rank is known.
    }
  }

  return success();
}

LogicalResult ONNXDequantizeLinearOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {

  if (auto xTy = x().getType().dyn_cast<RankedTensorType>()) {
    auto xShape = xTy.getShape();
    SmallVector<int64_t, 4> yShape(xShape.begin(), xShape.end());
    int64_t d = nonScalar1DLen(x_scale().getType().cast<ShapedType>());
    if (d == -1 && !isFromNone(x_zero_point())) {
      d = nonScalar1DLen(x_zero_point().getType().cast<ShapedType>());
    }
    if (d != -1) {
      int64_t r = xTy.getRank();
      int64_t a = axis();
      // Checked in verify:
      assert(-r <= a && a < r && "axis out of range");
      if (a < 0)
        a += r;
      if (yShape[a] == -1) {
        yShape[a] = d;
      } else {
        // Checked in verify:
        assert(yShape[a] == d && "x_scale and x_zero_point 1-D tensor length "
                                 "must match the input axis dim size");
      }
    }
    updateType(y(), yShape);
  }

  return success();
}

//===----------------------------------------------------------------------===//
// ConvInteger - copied almost exactly from Conv (X -> x, W -> w, no bias)
//===----------------------------------------------------------------------===//

LogicalResult ONNXConvIntegerOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Generic shape for data input X, weight tensor W
  // X: (N x C x D1 x D2 ... x Dn)
  // W: (M x C/group x k1 x k2 x ... x kn)

  // Cannot infer shape if no shape exists.
  if (!x().getType().isa<RankedTensorType>() ||
      !w().getType().isa<RankedTensorType>()) {
    return success();
  }

  auto xTy = x().getType().cast<RankedTensorType>();
  if (!xTy.getElementType().isInteger(8)) {
    return emitOpError("Invalid input type");
  }
  auto xShape = xTy.getShape();
  auto weightTy = w().getType().cast<RankedTensorType>();
  if (!weightTy.getElementType().isInteger(8)) {
    return emitOpError("Invalid input type");
  }
  auto weightShape = weightTy.getShape();
  auto builder = mlir::Builder(this->getContext());

  // Lowest supported convolution is a one dimensional convolution.
  if (xShape.size() < 3) {
    return emitOpError("Data input shape must be at least (NxCxD1)");
  }

  // Check that shape of weight and data have same length.
  if (xShape.size() != weightShape.size()) {
    return emitError("Weight size not compatible with data size");
  }

  // Group is a required attribute and should have default value of 1.
  int64_t group = ONNXConvIntegerOp::group();

  // Check if the attribute actually exists. If it does not then add it.
  if (!groupAttr())
    groupAttr(IntegerAttr::get(builder.getIntegerType(64, /*isSigned=*/true),
        APInt(64, 1, /*isSigned=*/true)));

  // Check that the X.shape[1] == (W.shape[1] * group) == C condition holds.
  if (xShape[1] != -1 && weightShape[1] != -1 &&
      xShape[1] != (weightShape[1] * group)) {
    return emitOpError("Channel dimension mismatch");
  }

  // Note: the value of the group attribute only impacts the way the
  // computation is carried out and not the actual output size.

  // Number of spatial dimensions.
  auto spatialOffset = 2;
  int32_t spatialRank = xShape.size() - spatialOffset;

  // Use kernel_shape attribute if present otherwise use size from weight
  // argument.
  auto kernelShape = kernel_shape();
  if (kernelShape.has_value()) {
    if ((int32_t)ArrayAttrSize(kernelShape) != spatialRank) {
      return emitOpError(
          "kernel_shape length incompatible with spatial dimensions");
    }
    // Have the right number of values, check them.
    for (int i = 0; i < spatialRank; ++i)
      if (ArrayAttrIntVal(kernelShape, i) < 1) {
        return emitError("bad kernel_shape value");
      }
  } else {
    // Deduce shape from weight input.
    SmallVector<int64_t, 2> defaultVals;
    for (int i = 0; i < spatialRank; ++i)
      defaultVals.emplace_back(weightShape[spatialOffset + i]);
    // Convert to ArrayRef, then build attribute, then store attribute.
    ArrayRef<int64_t> defaultRefs(defaultVals);
    auto builder = mlir::Builder(getContext());
    kernel_shapeAttr(builder.getI64ArrayAttr(defaultRefs));
    kernelShape = kernel_shape();
  }

  // Process strides, dilations, and pads.
  LogicalResult res = processConvTypeParams<>(this, x());
  assert(succeeded(res));
  auto dilationsOpt = dilations();
  auto stridesOpt = strides();
  auto padsOpt = pads();

  // First two output dimensions consist of the number of batches and the
  // number of kernels being applied.
  SmallVector<int64_t, 4> outputDims;
  // Insert batch size.
  outputDims.emplace_back(xShape[0]);
  // Insert number of filters being applied (number of output channels).
  outputDims.emplace_back(weightShape[0]);
  // Compute and insert spatial dims.
  insertConvSpatialDim(&outputDims, builder, xShape, kernelShape, padsOpt,
      stridesOpt, dilationsOpt);

  // ONNX spec specifies the output type as an int32
  Type outputElementType = IntegerType::get(getContext(), 32);
  updateType(getResult(), outputDims, outputElementType);
  return success();
}

//===----------------------------------------------------------------------===//
// Shape
//===----------------------------------------------------------------------===//

LogicalResult ONNXShapeOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Cannot infer shape if no shape exists.
  if (!data().getType().isa<RankedTensorType>())
    return success();

  // Output is an 1D int64 tensor containing the shape of the input tensor.
  auto elementType = IntegerType::get(getContext(), 64);
  return shapeHelperInferShapes<ONNXShapeOpShapeHelper, ONNXShapeOp,
      ONNXShapeOpAdaptor>(*this, elementType);
}

LogicalResult ONNXShapeOp::verify() {
  if (!data().getType().isa<RankedTensorType>())
    return success();
  ONNXShapeOpAdaptor operandAdaptor(*this);
  int64_t start;
  int64_t end;
  std::tie(start, end) = getDataShapeBounds(operandAdaptor);
  if (start > end)
    return emitOpError() << "Start: " << start << " is after End: " << end;
  return success();
}

//===----------------------------------------------------------------------===//
// Size
//===----------------------------------------------------------------------===//

LogicalResult ONNXSizeOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Output is scalar of int64 containing the size of the input tensor.
  SmallVector<int64_t, 1> outDims;
  getResult().setType(
      RankedTensorType::get(outDims, IntegerType::get(getContext(), 64)));
  return success();
}

//===----------------------------------------------------------------------===//
// Tile
//===----------------------------------------------------------------------===//

LogicalResult ONNXTileOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Cannot infer shape if no shape exists.
  if (!input().getType().isa<RankedTensorType>())
    return success();

  // Read 'repeats' value.
  if (!repeats().getType().isa<RankedTensorType>())
    return success();

  // 'repeats' tensor is an 1D tensor.
  auto repeatsTensorTy = repeats().getType().cast<RankedTensorType>();
  if (repeatsTensorTy.getShape().size() != 1)
    return emitError("Repeats tensor must have rank one");

  auto elementType = input().getType().cast<ShapedType>().getElementType();
  return shapeHelperInferShapes<ONNXTileOpShapeHelper, ONNXTileOp,
      ONNXTileOpAdaptor>(*this, elementType);
}

//===----------------------------------------------------------------------===//
// Gather
//===----------------------------------------------------------------------===//

LogicalResult ONNXGatherOp::verify() {
  ONNXGatherOpAdaptor operandAdaptor(*this);
  if (llvm::any_of(operandAdaptor.getOperands(),
          [](const Value &op) { return !hasShapeAndRank(op); }))
    return success(); // Won't be able to do any checking at this stage.

  auto dataType = operandAdaptor.data().getType().cast<RankedTensorType>();
  ArrayRef<int64_t> dataShape = dataType.getShape();
  int64_t dataRank = dataShape.size();
  int64_t axisValue = axis();

  // axis attribute must be in the range [-r,r-1], where r = rank(data).
  if (axisValue < -dataRank || axisValue >= dataRank)
    return onnx_mlir::Diagnostic::emitAttributeOutOfRangeError(
        *this->getOperation(), "axis", axisValue,
        onnx_mlir::Diagnostic::Range<int64_t>(-dataRank, dataRank - 1));

  return success();
}

LogicalResult ONNXGatherOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  if (llvm::any_of(this->getOperands(),
          [](const Value &op) { return !hasShapeAndRank(op); }))
    return success();

  auto elementType = data().getType().cast<ShapedType>().getElementType();
  return shapeHelperInferShapes<ONNXGatherOpShapeHelper, ONNXGatherOp,
      ONNXGatherOpAdaptor>(*this, elementType);
}

//===----------------------------------------------------------------------===//
// GatherElements
//===----------------------------------------------------------------------===//

LogicalResult ONNXGatherElementsOp::verify() {
  ONNXGatherElementsOpAdaptor operandAdaptor(*this);
  if (llvm::any_of(operandAdaptor.getOperands(),
          [](const Value &op) { return !hasShapeAndRank(op); }))
    return success(); // Won't be able to do any checking at this stage.

  // Get operands and attributes.
  Value data = operandAdaptor.data();
  Value indices = operandAdaptor.indices();
  auto dataType = data.getType().cast<ShapedType>();
  auto indicesType = indices.getType().cast<ShapedType>();
  int64_t dataRank = dataType.getRank();
  int64_t indicesRank = indicesType.getRank();
  int64_t axis = this->axis();

  // All inputs must have the same rank, and the rank must be strictly greater
  // than zero.
  if (dataRank < 1)
    return onnx_mlir::Diagnostic::emitOperandHasUnexpectedRankError(
        *this->getOperation(), data, dataRank, "> 0");
  if (indicesRank != dataRank)
    return onnx_mlir::Diagnostic::emitOperandHasUnexpectedRankError(
        *this->getOperation(), indices, indicesRank, std::to_string(dataRank));

  // axis attribute must be in the range [-r,r-1], where r = rank(data).
  if (axis < -dataRank || axis >= dataRank)
    return onnx_mlir::Diagnostic::emitAttributeOutOfRangeError(
        *this->getOperation(), "axis", axis,
        onnx_mlir::Diagnostic::Range<int64_t>(-dataRank, dataRank - 1));
  if (axis < 0)
    axis += dataRank;

  // All index values in 'indices' are expected to be within bounds [-s, s-1]
  // along axis of size s.
  ArrayRef<int64_t> dataShape = dataType.getShape();
  const int64_t dataDimAtAxis = dataShape[axis];
  if (dataDimAtAxis >= 0)
    if (DenseElementsAttr valueAttribute =
            getDenseElementAttributeFromONNXValue(indices))
      for (IntegerAttr value : valueAttribute.getValues<IntegerAttr>()) {
        int64_t index = value.getInt();
        if (index >= -dataDimAtAxis && index < dataDimAtAxis)
          continue;

        return onnx_mlir::Diagnostic::emitAttributeOutOfRangeError(
            *this->getOperation(), "indices", index,
            onnx_mlir::Diagnostic::Range<int64_t>(
                -dataDimAtAxis, dataDimAtAxis - 1));
      }

  return success();
}

LogicalResult ONNXGatherElementsOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Cannot infer the output shape if the operands shape is not yet known.
  if (llvm::any_of(this->getOperands(),
          [](const Value &op) { return !hasShapeAndRank(op); }))
    return success();

  auto elementType = data().getType().cast<ShapedType>().getElementType();
  return shapeHelperInferShapes<ONNXGatherElementsOpShapeHelper,
      ONNXGatherElementsOp, ONNXGatherElementsOpAdaptor>(*this, elementType);
}

//===----------------------------------------------------------------------===//
// GatherND
//===----------------------------------------------------------------------===//

LogicalResult ONNXGatherNDOp::verify() {
  ONNXGatherNDOpAdaptor operandAdaptor(*this);
  if (llvm::any_of(operandAdaptor.getOperands(),
          [](const Value &op) { return !hasShapeAndRank(op); }))
    return success(); // Won't be able to do any checking at this stage.

  // Get operands and attributes.
  Value data = operandAdaptor.data();
  Value indices = operandAdaptor.indices();
  auto dataType = data.getType().cast<ShapedType>();
  auto indicesType = indices.getType().cast<ShapedType>();
  int64_t dataRank = dataType.getRank();
  int64_t indicesRank = indicesType.getRank();
  int64_t b = batch_dims();

  // 'data' and 'indices' must have rank strictly greater than zero.
  if (dataRank < 1)
    return onnx_mlir::Diagnostic::emitOperandHasUnexpectedRankError(
        *this->getOperation(), data, dataRank, "> 0");
  if (indicesRank < 1)
    return onnx_mlir::Diagnostic::emitOperandHasUnexpectedRankError(
        *this->getOperation(), indices, indicesRank, "> 0");

  ArrayRef<int64_t> dataShape = dataType.getShape();
  ArrayRef<int64_t> indicesShape = indicesType.getShape();
  int64_t indicesLastDim = indicesShape[indicesRank - 1];

  // b must be smaller than min(rank(data), rank(indices).
  int64_t minDataAndIndicesRank = std::min(dataRank, indicesRank);
  if (b >= minDataAndIndicesRank)
    return onnx_mlir::Diagnostic::emitAttributeOutOfRangeError(
        *this->getOperation(), "batch_dims", b,
        onnx_mlir::Diagnostic::Range<int64_t>(0, minDataAndIndicesRank - 1));

  // The first b dimensions of the shape of 'indices' and 'data' must be equal.
  for (int64_t i = 0; i < b; ++i) {
    int64_t dataDim = dataShape[i];
    int64_t indicesDim = indicesShape[i];
    if (indicesDim < 0 || dataDim < 0)
      continue;
    if (indicesDim != dataDim)
      return onnx_mlir::Diagnostic::emitDimensionHasUnexpectedValueError(
          *this->getOperation(), indices, i, indicesShape[i],
          std::to_string(dataShape[i]));
  }

  // Let r = rank(data), indices.shape[-1] must be in the range [1, r-b].
  if (indicesLastDim == 0)
    return onnx_mlir::Diagnostic::emitDimensionHasUnexpectedValueError(
        *this->getOperation(), indices, indicesRank - 1, indicesLastDim,
        ">= 1");
  if (indicesLastDim > dataRank - b)
    return onnx_mlir::Diagnostic::emitDimensionHasUnexpectedValueError(
        *this->getOperation(), indices, indicesRank - 1, indicesLastDim,
        "<= " + std::to_string(dataRank - b));

  // All values in 'indices' are expected to satisfy the inequality:
  //   -data.shape[b + i] <= indices[...,i] <= (data.shape[b + i]-1)].
  if (DenseElementsAttr valueAttribute =
          getDenseElementAttributeFromONNXValue(indices)) {
    int flatIndex = 0;
    for (IntegerAttr value : valueAttribute.getValues<IntegerAttr>()) {
      int64_t indexValue = value.getInt();
      int64_t gatherAxis = b + (flatIndex % indicesLastDim);
      int64_t dataDimAtAxis = dataShape[gatherAxis];
      if (dataDimAtAxis >= 0) {
        if (indexValue < -dataDimAtAxis || indexValue > dataDimAtAxis - 1)
          return onnx_mlir::Diagnostic::emitAttributeOutOfRangeError(
              *this->getOperation(),
              "indices[" + std::to_string(flatIndex) + "]", indexValue,
              onnx_mlir::Diagnostic::Range<int64_t>(
                  -dataDimAtAxis, dataDimAtAxis - 1));
      }
      flatIndex++;
    }
  }

  return success();
}

LogicalResult ONNXGatherNDOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Cannot infer the shape of the output if the inputs shape is not yet known.
  if (llvm::any_of(
          this->getOperands(), [](Value op) { return !hasShapeAndRank(op); }))
    return success();

  // The output rank is given by:
  //   rank(output) = rank(indices) + rank(data) - indices_shape[-1] - 1 - b.
  // Therefore 'indices.shape[-1]' must be known in order to compute the output
  // shape.
  ArrayRef<int64_t> indicesShape =
      indices().getType().cast<ShapedType>().getShape();
  int64_t indicesRank = indicesShape.size();
  if (indicesShape[indicesRank - 1] < 0)
    return success(); // cannot infer the oputput shape yet.

  auto elementType = data().getType().cast<ShapedType>().getElementType();
  return shapeHelperInferShapes<ONNXGatherNDOpShapeHelper, ONNXGatherNDOp,
      ONNXGatherNDOpAdaptor>(*this, elementType);
  return success();
}

//===----------------------------------------------------------------------===//
// ConstantOfShape
//===----------------------------------------------------------------------===//

LogicalResult ONNXConstantOfShapeOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {

  Type elementType;

  // 'value' attribute is a one-element tensor whose value and datatype are
  // used to set the output tensor value and datatype.
  if (value().has_value()) {
    elementType =
        valueAttr().cast<DenseElementsAttr>().getType().getElementType();
  } else {
    // If 'value' attribute is not specified, it defaults to a tensor of
    // value 0 and datatype float32.
    elementType = FloatType::getF32(getContext());

    llvm::SmallVector<int64_t, 2> dims(1, 1);
    auto tensorType = mlir::RankedTensorType::get(dims, elementType);

    llvm::SmallVector<float, 1> values(1, 0.);
    valueAttr(
        mlir::DenseElementsAttr::get(tensorType, llvm::makeArrayRef(values)));
  }

  // 'input' must be a 1D tensor.
  auto inputShape = input().getType().cast<RankedTensorType>().getShape();
  if (inputShape[0] == 0) {
    // If 'input' is an empty tensor, the output would be a scalar.
    getResult().setType(RankedTensorType::get({}, elementType));
    return success();
  }

  // Calculate output dimensions.
  SmallVector<int64_t, 4> outputDims(inputShape[0], -1);
  // If 'input' is a constant, check whether its values are valid or not.
  // If the values are valid, it is possible to infer shape.
  if (auto constantOp = getONNXConstantOp(input())) {
    DenseElementsAttr valueAttribute =
        constantOp.valueAttr().dyn_cast<DenseElementsAttr>();
    // Get repeat values from valueAttribute.
    auto valueIt = valueAttribute.getValues<IntegerAttr>().begin();
    for (int i = 0; i < inputShape[0]; ++i) {
      auto dim = (*valueIt++).cast<IntegerAttr>().getInt();
      outputDims[i] = dim;
    }
  }

  updateType(getResult(), outputDims, elementType);
  return success();
}

LogicalResult ONNXConstantOfShapeOp::verify() {
  ONNXConstantOfShapeOpAdaptor operandAdaptor(*this);
  auto input = operandAdaptor.input();
  if (!hasShapeAndRank(input))
    return success();

  auto inputShape = input.getType().cast<RankedTensorType>().getShape();
  if (inputShape.size() != 1)
    return emitOpError("Input tensor must be a 1D tensor");
  if (inputShape[0] == -1)
    return emitOpError("Input tensor must have static shape");

  // Calculate output dimensions.
  SmallVector<int64_t, 4> outputDims(inputShape[0], -1);
  // If 'input' is a constant, check whether its values are valid or not.
  // If the values are valid, it is possible to infer shape.
  if (auto constantOp = getONNXConstantOp(input)) {
    DenseElementsAttr valueAttribute =
        constantOp.valueAttr().dyn_cast<DenseElementsAttr>();
    // Get repeat values from valueAttribute.
    auto valueIt = valueAttribute.getValues<IntegerAttr>().begin();
    for (int i = 0; i < inputShape[0]; ++i) {
      auto dim = (*valueIt++).cast<IntegerAttr>().getInt();
      if (dim < 0)
        return emitOpError("All values of the input tensor must be >=0");
    }
    // Unreachable error: Type error will trigger before this occurs
    // No test needed for this error -----
    if (valueIt != valueAttribute.getValues<IntegerAttr>().end())
      return emitOpError(
          "Constant value must have same length as output's rank");
  }
  return success();
}

//===----------------------------------------------------------------------===//
// Slice
//===----------------------------------------------------------------------===//

LogicalResult ONNXSliceOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Cannot infer shape if no shape exists.
  if (!data().getType().isa<RankedTensorType>())
    return success();

  const auto startsType =
      this->getOperand(1).getType().dyn_cast<RankedTensorType>();
  assert(startsType != nullptr && "starts type is not a RankedTensorType");
  auto startsDim = startsType.getShape()[0];
  {
    OpBuilder builder(this->getContext());
    const auto elementType = builder.getIntegerType(64);
    const auto tensorType =
        mlir::RankedTensorType::get({startsDim}, elementType);

    // If axes is not specified, default to [0, ..., ndim-1]
    if (this->getOperand(3).getType().isa<NoneType>()) {
      SmallVector<int64_t, 1> vals = {};
      for (size_t s = 0; s < (size_t)startsDim; ++s)
        vals.emplace_back(s);
      auto constantDenseAttribute =
          mlir::DenseElementsAttr::get(tensorType, llvm::makeArrayRef(vals));
      builder.setInsertionPoint(*this);
      auto constantOp = builder.create<mlir::ONNXConstantOp>(
          this->getLoc(), mlir::Attribute(), constantDenseAttribute);
      mlir::Value constantResult = constantOp.output();
      this->setOperand(3, constantResult);
    }

    // If steps is not specified, default to [1, ..., 1]
    if (this->getOperand(4).getType().isa<NoneType>()) {
      SmallVector<int64_t, 1> vals(startsDim, 1);
      auto constantDenseAttribute =
          mlir::DenseElementsAttr::get(tensorType, llvm::makeArrayRef(vals));
      builder.setInsertionPoint(*this);
      auto constantOp = builder.create<mlir::ONNXConstantOp>(
          this->getLoc(), mlir::Attribute(), constantDenseAttribute);
      mlir::Value constantResult = constantOp.output();
      this->setOperand(4, constantResult);
    }
  }

  auto elementType = data().getType().cast<ShapedType>().getElementType();
  return shapeHelperInferShapes<ONNXSliceOpShapeHelper, ONNXSliceOp,
      ONNXSliceOpAdaptor>(*this, elementType);
}

//===----------------------------------------------------------------------===//
// Expand
//===----------------------------------------------------------------------===//

LogicalResult ONNXExpandOp::verify() {
  ONNXExpandOpAdaptor operandAdaptor = ONNXExpandOpAdaptor(*this);
  // Get operands.
  auto shape = operandAdaptor.shape();
  // Check input.
  auto shapeType = shape.getType().dyn_cast_or_null<ShapedType>();
  if (shapeType && shapeType.hasRank()) {
    if (shapeType.getRank() != 1)
      return emitOpError("Shape has a rank of 1");
  }
  return success();
}

LogicalResult ONNXExpandOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  if (!input().getType().isa<RankedTensorType>())
    return success();
  if (!shape().getType().isa<RankedTensorType>())
    return success();

  auto elementType = input().getType().cast<ShapedType>().getElementType();
  return shapeHelperInferShapes<ONNXExpandOpShapeHelper, ONNXExpandOp,
      ONNXExpandOpAdaptor>(*this, elementType);
}

//===----------------------------------------------------------------------===//
// Dropout
//===----------------------------------------------------------------------===//

LogicalResult ONNXDropoutOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  if (!data().getType().isa<RankedTensorType>())
    return success();

  getResult(0).setType(data().getType());

  IntegerType i1Type = IntegerType::get(getContext(), 1, IntegerType::Signless);
  updateType(getResult(1), getShape(data().getType()), i1Type);
  return success();
}

//===----------------------------------------------------------------------===//
// OneHotEncoder
//===----------------------------------------------------------------------===//

LogicalResult ONNXOneHotEncoderOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  ShapedType inputType = X().getType().dyn_cast<RankedTensorType>();
  if (!inputType)
    return success();
  auto shape = inputType.getShape();
  int64_t outDim = 0;

  // If the input is a tensor of float, int32, or double,
  // the data will be cast to integers and
  // the cats_int64s category list will be used for the lookups.
  if (inputType.getElementType().isIntOrFloat()) {
    outDim = ArrayAttrSize(cats_int64s());
  } else {
    outDim = ArrayAttrSize(cats_strings());
  }

  // Encoded output data, having one more dimension than X
  // total category count will determine the size of the extra dimension
  SmallVector<int64_t, 2> dims;
  for (unsigned int i = 0; i != shape.size(); ++i)
    dims.emplace_back(shape[i]);
  dims.emplace_back(outDim);

  updateType(getResult(), dims, FloatType::getF32(getContext()));
  return success();
}

LogicalResult ONNXOneHotEncoderOp::verify() {
  ONNXOneHotEncoderOpAdaptor operandAdaptor = ONNXOneHotEncoderOpAdaptor(*this);

  // get operands
  auto input = operandAdaptor.X();
  if (!hasShapeAndRank(input))
    return success();

  auto inputType = input.getType().cast<ShapedType>();
  if (!inputType)
    return success();

  // If the input is a tensor of float, int32, or double,
  // the data will be cast to integers and
  // the cats_int64s category list will be used for the lookups.
  if (inputType.getElementType().isIntOrFloat()) {
    if (!operandAdaptor.cats_int64s()) {
      return emitOpError("input is a tensor of float, int32, or double, "
                         "but no cats_int64s attribute");
    }
  } else {
    if (!operandAdaptor.cats_strings()) {
      return emitOpError("input is not a tensor of float, int32, or double, "
                         "but no cats_strings attribute");
    }
  }
  return success();
}

//===----------------------------------------------------------------------===//
// Less
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXLessOp. This method is required by the
/// shape inference interface.
LogicalResult ONNXLessOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  for (unsigned int i = 0; i < getNumOperands(); ++i) {
    if (!getOperand(i).getType().cast<RankedTensorType>())
      return success();
  }
  Type lhsTy = getOperand(0).getType().cast<RankedTensorType>();
  Type rhsTy = getOperand(1).getType().cast<RankedTensorType>();
  ArrayRef<int64_t> dims =
      getBroadcastedType(lhsTy, rhsTy).cast<RankedTensorType>().getShape();

  updateType(getResult(), dims, IntegerType::get(getContext(), /*width=*/1));
  return success();
}

LogicalResult ONNXLessOrEqualOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  Builder b(getContext());
  return inferShapeForBroadcastingOps<ONNXLessOrEqualOp,
      ONNXLessOrEqualOpAdaptor>(*this, b.getI1Type());
}

// Operations for which shape inference has not been implemented yet
// If you add the implementation for one op, move it out of this section
// Also please add test case in test/mlir/onnx/onnx_shape_inference.mlir
// Followed by the implementation of lowering to Krnl and
// Enable the corresponding node test in check-onnx-backend

LogicalResult ONNXBatchNormalizationOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return emitError(NOT_IMPLEMENTED_MESSAGE);
}

LogicalResult ONNXBitShiftOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForBroadcastingOps<ONNXBitShiftOp, ONNXBitShiftOpAdaptor>(
      *this);
}

LogicalResult ONNXBernoulliOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  auto builder = mlir::OpBuilder(getContext());
  if (!hasShapeAndRank(input())) {
    return success();
  }
  RankedTensorType inputType = input().getType().cast<RankedTensorType>();
  Type elementType;
  if (dtypeAttr()) {
    elementType = convertONNXTypeToMLIRType(builder,
        (onnx::TensorProto_DataType)dtypeAttr().getValue().getSExtValue());
  } else {
    elementType = inputType.getElementType();
  }
  getResult().setType(RankedTensorType::get(inputType.getShape(), elementType));
  return success();
}

LogicalResult ONNXCeilOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

LogicalResult ONNXClipOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Look at input.
  if (!input().getType().isa<RankedTensorType>())
    return success();
  RankedTensorType inputTy = input().getType().cast<RankedTensorType>();
  Type elementType = inputTy.getElementType();
  // Look at optional min.
  if (!min().getType().isa<NoneType>()) {
    // Has a min, make sure its of the right type.
    if (!min().getType().isa<RankedTensorType>())
      return success();
    // And size.
    RankedTensorType minTy = min().getType().cast<RankedTensorType>();
    if (minTy.getElementType() != elementType)
      return emitError("Element type mismatch between input and min tensors");
    if (minTy.getShape().size() != 0)
      return emitError("Min tensor ranked with nonzero size");
  }
  // Look at optional max
  if (!max().getType().isa<NoneType>()) {
    // Has a max, make sure its of the right type.
    if (!max().getType().isa<RankedTensorType>())
      return success();
    // And size.
    RankedTensorType maxTy = max().getType().cast<RankedTensorType>();
    if (maxTy.getElementType() != elementType)
      return emitError("Element type mismatch between input and max tensors");
    if (maxTy.getShape().size() != 0)
      return emitError("Min tensor ranked with nonzero size");
  }

  updateType(getResult(), inputTy.getShape(), elementType);
  return success();
}

LogicalResult ONNXInstanceNormalizationOp::verify() {
  ONNXInstanceNormalizationOpAdaptor operandAdaptor =
      ONNXInstanceNormalizationOpAdaptor(*this);
  // Get operands.
  auto input = operandAdaptor.input();
  auto scale = operandAdaptor.scale();
  auto B = operandAdaptor.B();

  // Check input.
  if (!hasShapeAndRank(input)) {
    // Won't be able to do any checking at this stage.
    return success();
  }
  auto inputType = input.getType().cast<ShapedType>();
  auto inputShape = inputType.getShape();
  auto inputElementType = inputType.getElementType();
  int64_t spatialRank = inputShape.size() - 2;
  // If ranked, verify ranks of inputs.
  if (spatialRank < 1)
    return emitOpError("Spatial rank must be strictly positive");

  // Check bias B.
  if (hasShapeAndRank(B)) {
    // Can check at this stage.
    auto bType = B.getType().cast<ShapedType>();
    auto bShape = bType.getShape();
    if (bShape.size() != 1)
      return emitOpError("Bias should have a rank of one");
    if (bShape[0] >= 0 && inputShape[1] >= 0 && bShape[0] != inputShape[1])
      return emitOpError(
          "Bias should have same dimension as the second dimension of input");
    if (bType.getElementType() != inputElementType)
      return emitOpError("Bias should have same element type as input");
  }

  // Check scale.
  if (hasShapeAndRank(scale)) {
    // Can check at this stage.
    auto scaleType = scale.getType().cast<ShapedType>();
    auto scaleShape = scaleType.getShape();
    if (scaleShape.size() != 1)
      return emitOpError("Scale should have a rank of one");
    if (scaleShape[0] >= 0 && inputShape[1] >= 0 &&
        scaleShape[0] != inputShape[1])
      return emitOpError(
          "Scale should have same dimension as the second dimension of input");
    if (scaleType.getElementType() != inputElementType)
      return emitOpError("Scale should have same element type as input");
  }

  return success();
}

LogicalResult ONNXInstanceNormalizationOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// ONNXCompressOp
//===----------------------------------------------------------------------===//

LogicalResult ONNXCompressOp::verify() {
  // Cannot check constraints if the shape of the inputs is not yet knwon.
  ONNXCompressOpAdaptor operandAdaptor(*this);
  if (llvm::any_of(operandAdaptor.getOperands(),
          [](const Value &op) { return !hasShapeAndRank(op); }))
    return success(); // Won't be able to do any checking at this stage.

  int64_t inputRank = input().getType().cast<ShapedType>().getRank();
  Optional<int64_t> optionalAxis = axis();

  if (optionalAxis.has_value()) {
    // axis attribute must be in the range [-r,r-1], where r = rank(input).
    int64_t axis = optionalAxis.value();
    if (axis < -inputRank || axis >= inputRank)
      return onnx_mlir::Diagnostic::emitAttributeOutOfRangeError(
          *this->getOperation(), "axis", axis,
          onnx_mlir::Diagnostic::Range<int64_t>(-inputRank, inputRank - 1));
  }

  int64_t condRank = condition().getType().cast<ShapedType>().getRank();
  if (condRank != 1)
    return onnx_mlir::Diagnostic::emitAttributeOutOfRangeError(
        *this->getOperation(), "condition", condRank,
        onnx_mlir::Diagnostic::Range<int64_t>(1, 1));

  return success();
}

LogicalResult ONNXCompressOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Cannot infer the output shape if the input shape is not yet knwon.
  if (!hasShapeAndRank(input()))
    return success();

  auto elementType = input().getType().cast<ShapedType>().getElementType();
  return shapeHelperInferShapes<ONNXCompressOpShapeHelper, ONNXCompressOp,
      ONNXCompressOpAdaptor>(*this, elementType);
}

LogicalResult ONNXCumSumOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

LogicalResult ONNXDepthToSpaceOp::verify() {
  ONNXDepthToSpaceOpAdaptor operandAdaptor(*this);

  // Check input.
  Value input = operandAdaptor.input();
  if (!hasShapeAndRank(input)) {
    // Won't be able to do any checking at this stage.
    return success();
  }
  auto inputType = input.getType().cast<ShapedType>();
  auto inputShape = inputType.getShape();
  if (inputShape.size() != 4)
    return emitOpError("Input should have a rank of four");

  // Check blocksize.
  int64_t blocksize = operandAdaptor.blocksize();
  if (blocksize < 0)
    return emitOpError("Blocksize should be non negative");

  int64_t C = inputShape[1];
  if (C != -1 && C % (blocksize * blocksize) != 0)
    return emitOpError("The input tensor depth must be divisible by the "
                       "(blocksize * blocksize)");

  // Check mode.
  StringRef mode = operandAdaptor.mode();
  if (mode != "DCR" && mode != "CRD")
    return emitOpError("Mode must be DCR or CRD");

  return success();
}

LogicalResult ONNXDepthToSpaceOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Cannot infer shape if no input shape exists.
  if (!input().getType().isa<RankedTensorType>())
    return success();

  auto elementType = input().getType().cast<ShapedType>().getElementType();
  return shapeHelperInferShapes<ONNXDepthToSpaceOpShapeHelper,
      ONNXDepthToSpaceOp, ONNXDepthToSpaceOpAdaptor>(*this, elementType);
}

LogicalResult ONNXDetOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return emitError(NOT_IMPLEMENTED_MESSAGE);
}

LogicalResult ONNXEinsumOp::verify() {
  einsum::ErrorFn errorFn = [this]() -> mlir::InFlightDiagnostic {
    return this->emitOpError() << "equation '" << this->equation() << "': ";
  };

  ONNXEinsumOpAdaptor operandAdaptor(*this);
  ValueRange inputs = operandAdaptor.Inputs();

  if (failed(einsum::verifyEquation(equation(), inputs.size(), errorFn))) {
    return failure();
  }

  Type firstElementType =
      inputs[0].getType().cast<ShapedType>().getElementType();
  for (Value input : inputs) {
    ShapedType type = input.getType().cast<ShapedType>();
    if (type.getElementType() != firstElementType) {
      return emitOpError() << "different input element types";
    }
  }
  if (!llvm::all_of(inputs, hasShapeAndRank))
    return success(); // Can only infer once operand shapes are known.
  return einsum::verifyShapes(operandAdaptor, errorFn);
}

LogicalResult ONNXEinsumOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  ONNXEinsumOpAdaptor operandAdaptor(*this);
  if (!llvm::all_of(operandAdaptor.Inputs(), hasShapeAndRank))
    return success(); // Can only infer once operand shapes are known.

  einsum::ErrorFn errorFn = [this]() {
    return this->emitOpError() << "equation '" << this->equation() << "': ";
  };
  FailureOr<einsum::Shape> shape =
      einsum::inferOutputShape(operandAdaptor, errorFn);
  assert(succeeded(shape) && "any failure should be caught in verify()");
  Type elementType =
      getOperand(0).getType().cast<ShapedType>().getElementType();

  updateType(getResult(), *shape, elementType);
  return success();
}

LogicalResult ONNXEqualOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  Builder b(getContext());
  return inferShapeForBroadcastingOps<ONNXEqualOp, ONNXEqualOpAdaptor>(
      *this, b.getI1Type());
}

LogicalResult ONNXEyeLikeOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  auto builder = mlir::OpBuilder(getContext());
  if (!hasShapeAndRank(input())) {
    return success();
  }
  RankedTensorType inputType = input().getType().cast<RankedTensorType>();
  Type elementType;
  if (dtypeAttr()) {
    elementType = convertONNXTypeToMLIRType(builder,
        (onnx::TensorProto_DataType)dtypeAttr().getValue().getSExtValue());
  } else {
    elementType = inputType.getElementType();
  }

  updateType(getResult(), inputType.getShape(), elementType);
  return success();
}

LogicalResult ONNXFloorOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

LogicalResult ONNXGreaterOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  Builder b(getContext());
  return inferShapeForBroadcastingOps<ONNXGreaterOp, ONNXGreaterOpAdaptor>(
      *this, b.getI1Type());
}

LogicalResult ONNXGreaterOrEqualOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  Builder b(getContext());
  return inferShapeForBroadcastingOps<ONNXGreaterOrEqualOp,
      ONNXGreaterOrEqualOpAdaptor>(*this, b.getI1Type());
}

//===----------------------------------------------------------------------===//
// ONNXHardmaxOp
//===----------------------------------------------------------------------===//

LogicalResult ONNXHardmaxOp::verify() {
  ONNXHardmaxOpAdaptor operandAdaptor(*this);
  Value input = operandAdaptor.input();
  if (!hasShapeAndRank(input))
    return success(); // Won't be able to do any checking at this stage.

  // axis attribute must be in the range [-r,r-1], where r = rank(input).
  int64_t axisValue = axis();
  int64_t inputRank = input.getType().cast<ShapedType>().getRank();
  if (axisValue < -inputRank || axisValue >= inputRank)
    return onnx_mlir::Diagnostic::emitAttributeOutOfRangeError(
        *this->getOperation(), "axis", axisValue,
        onnx_mlir::Diagnostic::Range<int64_t>(-inputRank, inputRank - 1));

  return success();
}

LogicalResult ONNXHardmaxOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  if (!hasShapeAndRank(input()))
    return success();

  auto inputType = input().getType().cast<ShapedType>();
  int64_t inputRank = inputType.getRank();
  int64_t axisValue = axis();

  // axis attribute must be in the range [-r,r], where r = rank(input).
  if (axisValue < -inputRank || axisValue > inputRank)
    return onnx_mlir::Diagnostic::emitAttributeOutOfRangeError(
        *this->getOperation(), "axis", axisValue,
        onnx_mlir::Diagnostic::Range<int64_t>(-inputRank, inputRank - 1));

  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// ONNXHardSwishOp
//===----------------------------------------------------------------------===//

LogicalResult ONNXHardSwishOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// ONNXIfOp
//===----------------------------------------------------------------------===//

namespace {
bool areCompatibleIfTypes(Type ifResultType, Type branchResultType) {
  // ifResultType must be tensor/seq/opt type because that's checked in
  // ONNXIfOp::verifyInvariantsImpl()
  if (ShapedType ifShapedType = ifResultType.dyn_cast<ShapedType>()) {
    if (ShapedType branchShapedType = branchResultType.dyn_cast<ShapedType>()) {
      return ifShapedType.getElementType() == branchShapedType.getElementType();
    } else {
      return false;
    }
  }
  if (SeqType ifSeqType = ifResultType.dyn_cast<SeqType>()) {
    if (SeqType branchSeqType = branchResultType.dyn_cast<SeqType>()) {
      return areCompatibleIfTypes(
          ifSeqType.getElementType(), branchSeqType.getElementType());
    } else {
      return false;
    }
  }
  if (OptType ifOptType = ifResultType.dyn_cast<OptType>()) {
    if (OptType branchOptType = branchResultType.dyn_cast<OptType>()) {
      return areCompatibleIfTypes(
          ifOptType.getElementType(), branchOptType.getElementType());
    } else {
      return false;
    }
  }
  llvm_unreachable("areCompatibleIfTypes called with non tensor/seq/opt type");
}

// Pre-condition: areCompatibleIfTypes(ifTy, lhs) && areCompatibleIfTypes(ifTy,
// rhs)
Type unionOfIfTypes(Type lhs, Type rhs) {
  // All asserts below are checked in areCompatibleIfTypes().
  if (ShapedType lhsShapedType = lhs.dyn_cast<ShapedType>()) {
    ShapedType rhsShapedType = rhs.cast<ShapedType>();
    Type elementType = lhsShapedType.getElementType();
    assert(elementType == rhsShapedType.getElementType() &&
           "tensor element types mismatch");
    if (lhsShapedType.hasRank() && rhsShapedType.hasRank() &&
        lhsShapedType.getRank() == rhsShapedType.getRank()) {
      int64_t rank = lhsShapedType.getRank();
      auto lhsShape = lhsShapedType.getShape();
      auto rhsShape = rhsShapedType.getShape();
      SmallVector<int64_t, 4> shape;
      for (int64_t i = 0; i < rank; ++i) {
        shape.push_back(lhsShape[i] == rhsShape[i] ? lhsShape[i] : -1);
      }
      return RankedTensorType::get(shape, elementType);
    } else {
      return UnrankedTensorType::get(elementType);
    }
  }
  if (SeqType lhsSeqType = lhs.dyn_cast<SeqType>()) {
    SeqType rhsSeqType = rhs.cast<SeqType>();
    int64_t length = lhsSeqType.getLength() == rhsSeqType.getLength()
                         ? lhsSeqType.getLength()
                         : -1;
    return SeqType::get(unionOfIfTypes(lhsSeqType.getElementType(),
                            rhsSeqType.getElementType()),
        length);
  }
  if (OptType lhsOptType = lhs.dyn_cast<OptType>()) {
    OptType rhsOptType = rhs.cast<OptType>();
    return OptType::get(unionOfIfTypes(
        lhsOptType.getElementType(), rhsOptType.getElementType()));
  }
  llvm_unreachable("unionOfIfTypes called with non tensor/seq/opt type");
}
} // namespace

LogicalResult ONNXIfOp::verify() {
  size_t ifNumResults = getNumResults();
  assert(ifNumResults == outputs().size() && "outputs() != all results");
  auto thenResults = then_branch().back().getTerminator()->getOperands();
  if (ifNumResults != thenResults.size())
    return emitOpError() << "then branch #results=" << thenResults.size()
                         << " differ from if #results=" << ifNumResults;
  auto elseResults = else_branch().back().getTerminator()->getOperands();
  if (ifNumResults != elseResults.size())
    return emitOpError() << "else branch #results=" << elseResults.size()
                         << " differ from if #results=" << ifNumResults;
  auto thenResultTypes = thenResults.getTypes();
  auto elseResultTypes = elseResults.getTypes();
  for (size_t i = 0; i < ifNumResults; ++i) {
    Type ifResultType = getResultTypes()[i];
    if (!areCompatibleIfTypes(ifResultType, thenResultTypes[i]))
      emitOpError() << "then branch disagrees on result type #" << (i + 1)
                    << " of " << ifNumResults;
    if (!areCompatibleIfTypes(ifResultType, elseResultTypes[i]))
      emitOpError() << "else branch disagrees on result type #" << (i + 1)
                    << " of " << ifNumResults;
  }
  return success();
}

LogicalResult ONNXIfOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  doShapeInference(then_branch());
  doShapeInference(else_branch());
  size_t ifNumResults = getNumResults();
  auto thenResultTypes =
      then_branch().back().getTerminator()->getOperandTypes();
  auto elseResultTypes =
      else_branch().back().getTerminator()->getOperandTypes();
  // assert is checked in verify()
  assert(ifNumResults == thenResultTypes.size() &&
         ifNumResults == elseResultTypes.size() &&
         "if #results and branches #results differ");
  for (size_t i = 0; i < ifNumResults; ++i) {
    getResult(i).setType(
        unionOfIfTypes(thenResultTypes[i], elseResultTypes[i]));
  }
  return success();
}

//===------------------------------------------------------------------------===//
// IsInfOp
//===------------------------------------------------------------------------===//

LogicalResult ONNXIsInfOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return emitError(NOT_IMPLEMENTED_MESSAGE);
}

//===------------------------------------------------------------------------===//
// IsNaNOp
//===------------------------------------------------------------------------===//

LogicalResult ONNXIsNaNOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  ONNXIsNaNOpAdaptor operandAdaptor(*this);
  if (!hasShapeAndRank(operandAdaptor.X()))
    return success();

  IntegerType i1Type = IntegerType::get(getContext(), 1, IntegerType::Signless);
  updateType(getResult(), getShape(X().getType()), i1Type);
  return success();
}

//===------------------------------------------------------------------------===//
// LRNOp
//===------------------------------------------------------------------------===//

LogicalResult ONNXLRNOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  auto elementType = X().getType().cast<ShapedType>().getElementType();
  return shapeHelperInferShapes<ONNXLRNOpShapeHelper, ONNXLRNOp,
      ONNXLRNOpAdaptor>(*this, elementType);
}

//===----------------------------------------------------------------------===//
// ONNXLogSoftmax
//===----------------------------------------------------------------------===//

LogicalResult ONNXLogSoftmaxOp::verify() {
  ONNXLogSoftmaxOpAdaptor operandAdaptor(*this);
  if (!hasShapeAndRank(operandAdaptor.input()))
    return success(); // Won't be able to do any checking at this stage.

  int64_t inputRank =
      operandAdaptor.input().getType().cast<ShapedType>().getRank();
  int64_t axisIndex = axis();

  // axis attribute must be in the range [-r,r-1], where r = rank(input).
  if (axisIndex < -inputRank || axisIndex >= inputRank)
    return onnx_mlir::Diagnostic::emitAttributeOutOfRangeError(
        *this->getOperation(), "axis", axisIndex,
        onnx_mlir::Diagnostic::Range<int64_t>(-inputRank, inputRank - 1));

  return success();
}

LogicalResult ONNXLogSoftmaxOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

LogicalResult ONNXLpNormalizationOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

LogicalResult ONNXLpPoolOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return emitError(NOT_IMPLEMENTED_MESSAGE);
}

LogicalResult ONNXMaxPoolOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return emitError(NOT_IMPLEMENTED_MESSAGE);
}

LogicalResult ONNXMaxRoiPoolOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  if (!X().getType().isa<RankedTensorType>())
    return success();

  if (!rois().getType().isa<RankedTensorType>())
    return success();

  auto x_type = X().getType().cast<RankedTensorType>();
  auto x_shape = x_type.getShape();
  auto rois_rank = rois().getType().cast<RankedTensorType>().getRank();
  if (rois_rank != 2)
    return success();

  // 2d tensor: (num_rois, 5)
  auto roi_shape = rois().getType().cast<RankedTensorType>().getShape();
  int64_t num_rois = roi_shape[0];
  SmallVector<int64_t, 2> pooled_dims;

  auto pooled_shape_array_attr = pooled_shape();
  for (auto pooled_shape_attr : pooled_shape_array_attr) {
    auto pooled_shape_int_attr = pooled_shape_attr.dyn_cast<IntegerAttr>();
    if (!pooled_shape_int_attr)
      return success();
    pooled_dims.push_back(pooled_shape_int_attr.getInt());
  }

  // 4-D tensor : (num_rois, channels, pooled_shape[0], pooled_shape[1]).
  SmallVector<int64_t, 2> outputDims;
  outputDims.push_back(num_rois);
  outputDims.push_back(x_shape[1]); // channel
  outputDims.push_back(pooled_dims[0]);
  outputDims.push_back(pooled_dims[1]);

  updateType(getResult(), outputDims, x_type.getElementType());
  return success();
}

LogicalResult ONNXMaxUnpoolOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return emitError(NOT_IMPLEMENTED_MESSAGE);
}

LogicalResult ONNXMeanOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForBroadcastingOps<ONNXMeanOp, ONNXMeanOpAdaptor>(*this);
}

LogicalResult ONNXMeanVarianceNormalizationOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

LogicalResult ONNXModOp::verify() {
  Type elementType;
  if (A().getType().isa<ShapedType>())
    elementType = A().getType().cast<ShapedType>().getElementType();
  else
    return emitOpError("Input type must be TensorType or MemRefType");

  // Verify that when the input type is floating point, then `fmod` attribute
  // must be set to 1.
  if (elementType.isa<FloatType>() && (fmod() != 1))
    return emitOpError("fmod must be 1 when the input type is floating point");

  return success();
}

LogicalResult ONNXModOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForBroadcastingOps<ONNXModOp, ONNXModOpAdaptor>(*this);
}

LogicalResult ONNXMultinomialOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return emitError(NOT_IMPLEMENTED_MESSAGE);
}

LogicalResult ONNXNonMaxSuppressionOp::verify() {
  ONNXNonMaxSuppressionOpAdaptor operandAdaptor =
      ONNXNonMaxSuppressionOpAdaptor(*this);
  // Get operands.
  auto boxes = operandAdaptor.boxes();
  auto scores = operandAdaptor.scores();
  auto MOPC = operandAdaptor.max_output_boxes_per_class();
  auto scoreThreshold = operandAdaptor.score_threshold();
  auto iouThreshold = operandAdaptor.iou_threshold();

  // Check operands.
  if (hasShapeAndRank(boxes)) {
    auto shape = boxes.getType().cast<ShapedType>().getShape();
    if (shape.size() != 3)
      return emitOpError("boxes should have a rank of three");
    if (shape[2] != -1 && shape[2] != 4)
      return emitOpError("The last dim of Boxes should be four");
  }

  if (hasShapeAndRank(scores))
    if (scores.getType().cast<ShapedType>().getRank() != 3)
      return emitOpError("scores should have a rank of three");

  if (hasShapeAndRank(MOPC))
    if (MOPC.getType().cast<ShapedType>().getRank() > 1)
      return emitOpError(
          "max_output_boxex_per_class should have a rank of zero or one");

  if (hasShapeAndRank(scoreThreshold))
    if (scoreThreshold.getType().cast<ShapedType>().getRank() > 1)
      return emitOpError("score_threshold should have a rank of zero or one");

  if (hasShapeAndRank(iouThreshold))
    if (iouThreshold.getType().cast<ShapedType>().getRank() > 1)
      return emitOpError("iou_threshold should have a rank of zero or one");

  return success();
}

LogicalResult ONNXNonMaxSuppressionOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  auto b = mlir::Builder(getContext());
  getResult().setType(RankedTensorType::get({-1, 3}, b.getI64Type()));
  return success();
}

LogicalResult ONNXNonZeroOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  auto builder = mlir::Builder(getContext());
  Type inputType = getOperand().getType();
  if (!inputType.isa<RankedTensorType>())
    return success();
  SmallVector<int64_t, 2> dims;
  // The first dimension size is the rank of the input.
  dims.emplace_back(inputType.cast<RankedTensorType>().getRank());
  // The second dimension size is the number of nonzero values in the input.
  // So this dimension size is always unknown at compile time.
  dims.emplace_back(-1);
  getResult().setType(RankedTensorType::get(dims, builder.getI64Type()));
  return success();
}

LogicalResult ONNXNotOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

LogicalResult ONNXOneHotOp::verify() {
  ONNXOneHotOpAdaptor operandAdaptor = ONNXOneHotOpAdaptor(*this);
  // Check indices.
  Value indices = operandAdaptor.indices();
  if (hasShapeAndRank(indices)) {
    // Get rank.
    int64_t indicesRank = indices.getType().cast<ShapedType>().getRank();
    // Verify axis.
    int64_t axisValue = axis();
    // Unusually, with a rank of 3, acceptable values are 0 (before first) to 3
    // (after last).
    if (axisValue < 0)
      axisValue += indicesRank + 1;
    if (!(axisValue >= 0 && axisValue <= indicesRank))
      return emitOpError("OneHot axis value is out of range");
  }
  // Check that values is a rank 2 with 2 elements
  Value values = operandAdaptor.values();
  if (hasShapeAndRank(values)) {
    ShapedType valuesShape = values.getType().cast<ShapedType>();
    if (valuesShape.getRank() != 1)
      return emitOpError("OneHot values must be 1D tensor");
    int64_t dim = valuesShape.getDimSize(0);
    if (dim >= 0 && dim != 2)
      return emitOpError("OneHot values must be 1D tensor with 2 elements");
  }
  // Depth is a scalar, check when its a tensor of rank 0 or 1.
  Value depth = operandAdaptor.depth();
  if (hasShapeAndRank(depth)) {
    ShapedType depthShape = depth.getType().cast<ShapedType>();
    if (depthShape.getRank() == 1) {
      int64_t dim = depthShape.getDimSize(0);
      if (dim >= 0 && dim != 1)
        return emitOpError("OneHot depth can be 1D tensor with 1 elements");
    } else {
      if (depthShape.getRank() > 1)
        return emitOpError("OneHot depth must be 0 or 1D tensor");
    }
  }
  return success();
}

LogicalResult ONNXOneHotOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Cannot infer shape if no shape exists.
  if (!indices().getType().isa<RankedTensorType>())
    return success();

  auto elementType = values().getType().cast<ShapedType>().getElementType();
  return shapeHelperInferShapes<ONNXOneHotOpShapeHelper, ONNXOneHotOp,
      ONNXOneHotOpAdaptor>(*this, elementType);
}

LogicalResult ONNXOptionalOp::verify() {
  if (type().has_value() != input().getType().isa<NoneType>())
    return emitError(
        "Optional should have either type attribute or input value");
  return success();
}

LogicalResult ONNXOptionalOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  Type ty;
  if (auto typeAttr = type()) {
    ty = typeAttr.value();
  } else {
    ty = input().getType();
    // checked in verify()
    assert(!ty.isa<NoneType>() && "type attribute or input value needed");
  }
  getResult().setType(OptType::get(ty));
  return success();
}

LogicalResult ONNXOptionalGetElementOp::verify() {
  if (!input().getType().isa<OptType>())
    return emitError("OptionalGetElement input should have optional type");
  return success();
}

LogicalResult ONNXOptionalGetElementOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  Type elementType = input().getType().cast<OptType>().getElementType();
  getResult().setType(elementType);
  return success();
}

LogicalResult ONNXOptionalHasElementOp::verify() {
  if (!input().getType().isa<OptType>())
    return emitError("OptionalHasElement input should have optional type");
  return success();
}

LogicalResult ONNXOptionalHasElementOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  Builder builder(getContext());
  Type scalarBoolType = RankedTensorType::get({}, builder.getI1Type());
  getResult().setType(scalarBoolType);
  return success();
}

LogicalResult ONNXRandomNormalOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  auto outputShape = shape();
  auto elementTypeID = dtype();

  SmallVector<int64_t, 4> outputDims;
  auto spatialRank = ArrayAttrSize(outputShape);
  for (unsigned long i = 0; i < spatialRank; ++i) {
    int64_t dimension = ArrayAttrIntVal(outputShape, i);
    if (dimension < 0)
      return emitError("Random normal tensor has dynamic dimension.");
    outputDims.emplace_back(dimension);
  }

  RankedTensorType outputTensorType =
      RankedTensorType::get(outputDims, FloatType::getF32(getContext()));
  if (elementTypeID == 0)
    outputTensorType =
        RankedTensorType::get(outputDims, FloatType::getF16(getContext()));
  else if (elementTypeID == 2)
    outputTensorType =
        RankedTensorType::get(outputDims, FloatType::getF64(getContext()));

  getResult().setType(outputTensorType);
  return success();
}

LogicalResult ONNXRandomNormalLikeOp::verify() {
  ONNXRandomNormalLikeOpAdaptor operandAdaptor(*this);
  mlir::Value input = operandAdaptor.input();
  if (!hasShapeAndRank(input))
    return success();
  mlir::Value output = this->output();
  if (!hasShapeAndRank(output))
    return success();

  auto inputType = input.getType().cast<RankedTensorType>().getElementType();
  auto outputType = output.getType().cast<RankedTensorType>().getElementType();

  auto elementTypeIDDType = operandAdaptor.dtype();
  if (elementTypeIDDType) {
    int64_t elementTypeID = elementTypeIDDType.value();
    if (elementTypeID < 0 || elementTypeID > 2) {
      return emitOpError("dtype not 0, 1 or 2.");
    }
    if (elementTypeID == 0 && outputType != FloatType::getF16(getContext()))
      return emitOpError("output tensor does match 0 dtype.");
    else if (elementTypeID == 1 &&
             outputType != FloatType::getF32(getContext()))
      return emitOpError("output tensor does match 1 dtype.");
    else if (elementTypeID == 2 &&
             outputType != FloatType::getF64(getContext()))
      return emitOpError("output tensor does match 2 dtype.");
  } else if (inputType != outputType) {
    return emitOpError("output and input element types do not match.");
  }

  return success();
}

LogicalResult ONNXRandomNormalLikeOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  if (!input().getType().isa<RankedTensorType>())
    return success();
  auto inputType = input().getType().cast<RankedTensorType>();
  auto elementTypeIDDType = dtype();

  // Default output tensor type in all cases is the input tensor type.
  Type elementType;
  if (!elementTypeIDDType) {
    elementType = inputType.getElementType();
  } else {
    int64_t elementTypeID = elementTypeIDDType.value();
    if (elementTypeID == 0)
      elementType = FloatType::getF16(getContext());
    else if (elementTypeID == 1)
      elementType = FloatType::getF32(getContext());
    else if (elementTypeID == 2)
      elementType = FloatType::getF64(getContext());
    else
      return emitError("dtype attribute is invalid (use: 0, 1 or 2)");
  }
  updateType(getResult(), inputType.getShape(), elementType);
  return success();
}

LogicalResult ONNXRandomUniformOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return emitError(NOT_IMPLEMENTED_MESSAGE);
}

LogicalResult ONNXRandomUniformLikeOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return emitError(NOT_IMPLEMENTED_MESSAGE);
}

LogicalResult ONNXRangeOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // All inputs must be valid ranked tensors.
  if (!start().getType().isa<RankedTensorType>())
    return success();

  if (!limit().getType().isa<RankedTensorType>())
    return success();

  if (!delta().getType().isa<RankedTensorType>())
    return success();

  auto startTensorTy = start().getType().cast<RankedTensorType>();
  auto limitTensorTy = limit().getType().cast<RankedTensorType>();
  auto deltaTensorTy = delta().getType().cast<RankedTensorType>();

  // Only rank 0 or 1 input tensors are supported.
  if (startTensorTy.getShape().size() > 1)
    return emitError("start tensor must have rank zero or one");
  if (limitTensorTy.getShape().size() > 1)
    return emitError("limit tensor must have rank zero or one");
  if (deltaTensorTy.getShape().size() > 1)
    return emitError("delta tensor must have rank zero or one");

  // If tensor is rank 1 then the dimension has to be 1.
  if (startTensorTy.getShape().size() == 1 && startTensorTy.getShape()[0] > 1)
    return emitError("start tensor of rank one must have size one");
  if (limitTensorTy.getShape().size() == 1 && limitTensorTy.getShape()[0] > 1)
    return emitError("limit tensor of rank one must have size one");
  if (deltaTensorTy.getShape().size() == 1 && deltaTensorTy.getShape()[0] > 1)
    return emitError("delta tensor of rank one must have size one");

  // Only int or float input types are supported:
  // tensor(float), tensor(double), tensor(int16), tensor(int32),
  // tensor(int64)
  if (!startTensorTy.getElementType().isIntOrFloat())
    return emitError("start tensor type is not int or float");
  if (!limitTensorTy.getElementType().isIntOrFloat())
    return emitError("limit tensor type is not int or float");
  if (!deltaTensorTy.getElementType().isIntOrFloat())
    return emitError("delta tensor type is not int or float");

  // Additional condition for simplicity, enforce that all inputs have the
  // exact same element type:
  if (startTensorTy.getElementType() != limitTensorTy.getElementType() ||
      startTensorTy.getElementType() != deltaTensorTy.getElementType())
    return emitError("all inputs must have the exact same input type");

  // Number of elements, default is unknown so -1:
  int64_t number_of_elements = -1;

  // Check if input is constant. All inputs must be
  // constant for this path to be used.
  auto constantStart = getONNXConstantOp(start());
  auto constantLimit = getONNXConstantOp(limit());
  auto constantDelta = getONNXConstantOp(delta());
  if (constantStart && constantLimit && constantDelta) {
    // Get all inputs:
    double start = getScalarValue<double>(constantStart, startTensorTy);
    double limit = getScalarValue<double>(constantLimit, limitTensorTy);
    double delta = getScalarValue<double>(constantDelta, deltaTensorTy);

    // Compute size:
    number_of_elements = (int64_t)ceil((limit - start) / delta);

    // When no elements are present create a dynamic tensor.
    // TODO: represent an empty tensor for this case.
    if (number_of_elements <= 0)
      number_of_elements = -1;
  }

  SmallVector<int64_t, 1> dims(1, number_of_elements);
  updateType(getResult(), dims, startTensorTy.getElementType());
  return success();
}

LogicalResult ONNXReverseSequenceOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  if (!input().getType().isa<RankedTensorType>())
    return success();

  auto elementType = input().getType().cast<ShapedType>().getElementType();
  return shapeHelperInferShapes<ONNXReverseSequenceOpShapeHelper,
      ONNXReverseSequenceOp, ONNXReverseSequenceOpAdaptor>(*this, elementType);
}

LogicalResult ONNXReverseSequenceOp::verify() {
  ONNXReverseSequenceOpAdaptor operandAdaptor =
      ONNXReverseSequenceOpAdaptor(*this);

  auto sequence_lensTy =
      operandAdaptor.sequence_lens().getType().dyn_cast<RankedTensorType>();
  auto inputTy = operandAdaptor.input().getType().dyn_cast<RankedTensorType>();

  // sequence_lens should be 1D tensor
  if (sequence_lensTy) {
    if (sequence_lensTy.getRank() != 1)
      return emitOpError("sequence_lens of ReverseSequnce should be 1D tensor");
  }

  if (inputTy) {
    if (inputTy.getRank() < 2)
      return emitOpError(
          "input of Reversesequence should be 2D or higher rank tensor");
  }

  if (sequence_lensTy && inputTy) {
    int64_t batchAxis = batch_axis();
    if (sequence_lensTy.getShape()[0] != -1 &&
        inputTy.getShape()[batchAxis] != -1) {
      if (sequence_lensTy.getShape()[0] != inputTy.getShape()[batchAxis]) {
        return emitOpError("Length of sequence_lens should match the sizeof  "
                           "batch axis of the input");
      }
    }
  }

  return success();
}

LogicalResult ONNXRoiAlignOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Cannot infer shape if no shape exists.
  if (!X().getType().isa<RankedTensorType>() ||
      !batch_indices().getType().isa<RankedTensorType>())
    return success();

  auto elementType = X().getType().cast<ShapedType>().getElementType();
  return shapeHelperInferShapes<ONNXRoiAlignOpShapeHelper, ONNXRoiAlignOp,
      ONNXRoiAlignOpAdaptor>(*this, elementType);
}

LogicalResult ONNXRoiAlignOp::verify() {
  ONNXRoiAlignOpAdaptor operandAdaptor = ONNXRoiAlignOpAdaptor(*this);
  // get input info.
  mlir::Value X = operandAdaptor.X();
  mlir::Value batch_indices = operandAdaptor.batch_indices();

  if (!hasShapeAndRank(X) || !hasShapeAndRank(batch_indices))
    return success();

  int64_t x_rank = X.getType().cast<mlir::ShapedType>().getRank();
  int64_t batch_indices_rank =
      batch_indices.getType().cast<mlir::ShapedType>().getRank();

  // Test ranks.
  if (x_rank != 4)
    return emitOpError("RoiAlign with X should be a 4D tensor");
  if (batch_indices_rank != 1)
    return emitOpError("RoiAlign with batch_indices should be a 1D tensor");

  return success();
}

LogicalResult ONNXRoundOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

LogicalResult ONNXScanOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  auto &loopBody = getRegion();
  assert(!scan_input_axes().has_value());

  // We proceed to set types for loop body function inputs.
  // Set types for loop carried dependencies (i.e., set these loop carried
  // dependencies that appear in the body function input signature to have
  // the same type as their counterpart in LoopOp inputs).
  auto bodyInputs = loopBody.getArguments();
  auto bodyVRange = llvm::make_range(bodyInputs.begin(), bodyInputs.end());
  for (auto opVToBodyVTy : llvm::zip(v_initial(), bodyVRange)) {
    auto opVTy = std::get<0>(opVToBodyVTy).getType();
    std::get<1>(opVToBodyVTy).setType(opVTy);
  }

  auto bodyScanInputs = llvm::make_range(
      bodyInputs.begin() + v_initial().size(), bodyInputs.end());
  for (auto vScanOutputValToTy : llvm::zip(scan_inputs(), bodyScanInputs)) {
    auto rankedScanTy =
        std::get<0>(vScanOutputValToTy).getType().cast<RankedTensorType>();
    auto shape = rankedScanTy.getShape();
    SmallVector<int64_t, 4> squeezedShape(shape.begin() + 1, shape.end());

    // Note that we may know the extent of the scan output leading
    // dimension, which is very likely just the trip count specified as an
    // input to Loop operation, but we need to eliminate the possibility of
    // early termination to be sure.
    updateType(std::get<1>(vScanOutputValToTy), squeezedShape,
        rankedScanTy.getElementType(), /*encoding=*/nullptr,
        /*refineShape=*/false);
  }

  // Now we have modified loop body function input signatures according to
  // the knowledge we have on the inputs we pass to this function. Dispatch
  // shape inference to obtain body function output types.
  doShapeInference(loopBody);

  // Output loop variables should have the same type as their input
  // counterparts.
  auto bodyResultTys = loopBody.back().getTerminator()->getOperandTypes();
  // Compute the type range corresponding to the final values of
  // loop-carried dependencies/scan outputs in the body function output
  // types.
  auto scanStartItr = std::next(bodyResultTys.begin(), v_initial().size());
  auto bodyResVFinalTys = llvm::make_range(bodyResultTys.begin(), scanStartItr);
  auto bodyResScanTys = llvm::make_range(scanStartItr, bodyResultTys.end());

  // Set shape for loop operation outputs corresponding to the final
  // values of loop-carried dependencies to be shape of their counterparts
  // in the body function output.
  for (auto vFinalValToTy : llvm::zip(v_final(), bodyResVFinalTys)) {
    std::get<0>(vFinalValToTy).setType(std::get<1>(vFinalValToTy));
  }

  // For scan outputs, we set their shape to be the shape of the return
  // values of the loop body function corresponding to scan outputs, but
  // with an extra leading dimension.
  for (auto vScanOutputValToTy : llvm::zip(scan_outputs(), bodyResScanTys)) {
    auto rankedScanTy =
        std::get<1>(vScanOutputValToTy).cast<RankedTensorType>();
    auto shape = rankedScanTy.getShape();
    SmallVector<int64_t, 4> unsqueezedShape(shape.begin(), shape.end());
    // Note that we may know the extent of the scan output leading
    // dimension, which is very likely just the trip count specified as an
    // input to Loop operation, but we need to eliminate the possibility of
    // early termination to be sure.
    auto scanExtent =
        scan_inputs().front().getType().cast<ShapedType>().getDimSize(0);
    unsqueezedShape.insert(unsqueezedShape.begin(), scanExtent);
    updateType(std::get<0>(vScanOutputValToTy), unsqueezedShape,
        rankedScanTy.getElementType(), /*encoding=*/nullptr,
        /*refineShape=*/false);
  }

  return success();
}

mlir::Operation::operand_range ONNXScanOp::v_initial() {
  auto numVInit = initial_state_and_scan_inputs().size() - num_scan_inputs();
  auto operands = getOperands();
  return llvm::make_range(operands.begin(), operands.begin() + numVInit);
}

mlir::Operation::operand_range ONNXScanOp::scan_inputs() {
  auto numVInit = initial_state_and_scan_inputs().size() - num_scan_inputs();
  auto operands = getOperands();
  return llvm::make_range(operands.begin() + numVInit, operands.end());
}

// Helper function to obtain subset of op results corresponding to the final
// value of loop carried dependencies.
mlir::Operation::result_range ONNXScanOp::v_final() {
  auto results = getResults();
  return llvm::make_range(
      results.begin(), results.begin() + v_initial().size());
}

// Helper function to obtain subset of op results corresponding to the scan
// outputs.
mlir::Operation::result_range ONNXScanOp::scan_outputs() {
  auto results = getResults();
  return llvm::make_range(results.begin() + v_initial().size(), results.end());
}

//===----------------------------------------------------------------------===//
// ONNXScatterElements
//===----------------------------------------------------------------------===//

LogicalResult ONNXScatterElementsOp::verify() {
  ONNXScatterElementsOpAdaptor operandAdaptor(*this);
  if (llvm::any_of(operandAdaptor.getOperands(),
          [](const Value &op) { return !hasShapeAndRank(op); }))
    return success(); // Won't be able to do any checking at this stage.

  // Get operands and attributes.
  Value data = operandAdaptor.data();
  Value indices = operandAdaptor.indices();
  Value updates = operandAdaptor.updates();
  auto dataType = data.getType().cast<ShapedType>();
  auto indicesType = indices.getType().cast<ShapedType>();
  auto updatesType = updates.getType().cast<ShapedType>();
  int64_t dataRank = dataType.getRank();
  int64_t indicesRank = indicesType.getRank();
  int64_t updatesRank = updatesType.getRank();
  int64_t axis = this->axis();

  // All inputs must have the same rank, and the rank must be strictly greater
  // than zero.
  if (dataRank < 1)
    return onnx_mlir::Diagnostic::emitOperandHasUnexpectedRankError(
        *this->getOperation(), data, dataRank, "> 0");
  if (indicesRank != dataRank)
    return onnx_mlir::Diagnostic::emitOperandHasUnexpectedRankError(
        *this->getOperation(), indices, indicesRank, std::to_string(dataRank));
  if (updatesRank != dataRank)
    return onnx_mlir::Diagnostic::emitOperandHasUnexpectedRankError(
        *this->getOperation(), updates, updatesRank, std::to_string(dataRank));

  // axis attribute must be in the range [-r,r-1], where r = rank(data).
  if (axis < -dataRank || axis >= dataRank)
    return onnx_mlir::Diagnostic::emitAttributeOutOfRangeError(
        *this->getOperation(), "axis", axis,
        onnx_mlir::Diagnostic::Range<int64_t>(-dataRank, dataRank - 1));

  if (axis < 0)
    axis += dataRank;

  // All index values in 'indices' are expected to be within bounds [-s, s-1]
  // along axis of size s.
  ArrayRef<int64_t> dataShape = dataType.getShape();
  const int64_t dataDimAtAxis = dataShape[axis];
  if (dataDimAtAxis >= 0) {
    if (DenseElementsAttr valueAttribute =
            getDenseElementAttributeFromONNXValue(indices)) {
      for (IntegerAttr value : valueAttribute.getValues<IntegerAttr>()) {
        int64_t index = value.getInt();
        if (index >= -dataDimAtAxis && index < dataDimAtAxis)
          continue;

        return onnx_mlir::Diagnostic::emitAttributeOutOfRangeError(
            *this->getOperation(), "indices", index,
            onnx_mlir::Diagnostic::Range<int64_t>(
                -dataDimAtAxis, dataDimAtAxis - 1));
      }
    }
  }

  return success();
}

//===----------------------------------------------------------------------===//
// ONNXScatter
//===----------------------------------------------------------------------===//

LogicalResult ONNXScatterOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// ONNXScatterElements
//===----------------------------------------------------------------------===//

LogicalResult ONNXScatterElementsOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// ONNXScatterND
//===----------------------------------------------------------------------===//

LogicalResult ONNXScatterNDOp::verify() {
  ONNXScatterNDOpAdaptor operandAdaptor(*this);
  if (llvm::any_of(operandAdaptor.getOperands(),
          [](const Value &op) { return !hasShapeAndRank(op); }))
    return success(); // Won't be able to do any checking at this stage.

  // Get operands and attributes.
  Value data = operandAdaptor.data();
  Value indices = operandAdaptor.indices();
  Value updates = operandAdaptor.updates();
  auto dataType = data.getType().cast<ShapedType>();
  auto indicesType = indices.getType().cast<ShapedType>();
  auto updatesType = updates.getType().cast<ShapedType>();
  int64_t dataRank = dataType.getRank();
  int64_t indicesRank = indicesType.getRank();
  int64_t updatesRank = updatesType.getRank();

  // 'data' and 'indices' must have rank strictly greater than zero.
  if (dataRank < 1)
    return onnx_mlir::Diagnostic::emitOperandHasUnexpectedRankError(
        *this->getOperation(), data, dataRank, "> 0");
  if (indicesRank < 1)
    return onnx_mlir::Diagnostic::emitOperandHasUnexpectedRankError(
        *this->getOperation(), indices, indicesRank, "> 0");

  ArrayRef<int64_t> dataShape = dataType.getShape();
  ArrayRef<int64_t> indicesShape = indicesType.getShape();
  ArrayRef<int64_t> updatesShape = updatesType.getShape();
  int64_t indicesLastDim = indicesShape[indicesRank - 1];

  // The rank of 'updates' must be equal to:
  //    rank(data) + rank(indices) - indices.shape[-1] - 1.
  if (indicesLastDim > 0) {
    int64_t expectedUpdatesRank = dataRank + indicesRank - indicesLastDim - 1;
    if (updatesRank != expectedUpdatesRank)
      return onnx_mlir::Diagnostic::emitOperandHasUnexpectedRankError(
          *this->getOperation(), updates, updatesRank,
          std::to_string(expectedUpdatesRank));

    // The last dimension of the 'indices' shape can be at most equal to the
    // rank of 'data'.
    if (indicesLastDim > dataRank)
      return onnx_mlir::Diagnostic::emitDimensionHasUnexpectedValueError(
          *this->getOperation(), indices, indicesRank - 1, indicesLastDim,
          "<= " + std::to_string(dataRank));
  }

  // The constraints check following this point requires the input tensors shape
  // dimensions to be known, if they aren't delay the checks.
  if (llvm::any_of(indicesShape, [](int64_t idx) { return (idx < 0); }))
    return success();
  if (llvm::any_of(updatesShape, [](int64_t idx) { return (idx < 0); }))
    return success();

  // Let q = rank(indices). The first (q-1) dimensions of the 'updates' shape
  // must match the first (q-1) dimensions of the 'indices' shape.
  for (int64_t i = 0; i < indicesRank - 1; ++i) {
    assert(i < updatesRank && "i is out of bounds");
    if (updatesShape[i] != indicesShape[i])
      return onnx_mlir::Diagnostic::emitDimensionHasUnexpectedValueError(
          *this->getOperation(), updates, i, updatesShape[i],
          std::to_string(indicesShape[i]));
  }

  if (llvm::any_of(dataShape, [](int64_t idx) { return (idx < 0); }))
    return success();

  // Let k = indices.shape[-1], r = rank(data), q = rank(indices). Check that
  // updates.shape[q:] matches data.shape[k:r-1].
  for (int64_t i = indicesLastDim, j = indicesRank - 1; i < dataRank;
       ++i, ++j) {
    assert(j < updatesRank && "j is out of bounds");
    if (updatesShape[j] != dataShape[i])
      return onnx_mlir::Diagnostic::emitDimensionHasUnexpectedValueError(
          *this->getOperation(), updates, j, updatesShape[j],
          std::to_string(dataShape[i]));
  }

  return success();
}

LogicalResult ONNXScatterNDOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// ONNXShrink
//===----------------------------------------------------------------------===//

LogicalResult ONNXShrinkOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// ONNXSpaceToDepth
//===----------------------------------------------------------------------===//

LogicalResult ONNXSpaceToDepthOp::verify() {
  ONNXSpaceToDepthOpAdaptor operandAdaptor(*this);

  // Check input.
  Value input = operandAdaptor.input();
  if (!hasShapeAndRank(input)) {
    // Won't be able to do any checking at this stage.
    return success();
  }
  auto inputType = input.getType().cast<ShapedType>();
  auto inputShape = inputType.getShape();
  if (inputShape.size() != 4)
    return emitOpError("Input should have a rank of four");

  // Check blocksize.
  int64_t blocksize = operandAdaptor.blocksize();
  if (blocksize < 0)
    return emitOpError("Blocksize should be non negative");

  int64_t H = inputShape[2];
  int64_t W = inputShape[3];

  if (H != -1 && H % blocksize != 0)
    return emitOpError(
        "The input tensor height must be divisible by the block size");
  if (W != -1 && W % blocksize != 0)
    return emitOpError(
        "The input tensor width must be divisible by the block size");

  return success();
}

LogicalResult ONNXSpaceToDepthOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Cannot infer shape if no input shape exists.
  if (!input().getType().isa<RankedTensorType>())
    return success();

  auto elementType = input().getType().cast<ShapedType>().getElementType();
  return shapeHelperInferShapes<ONNXSpaceToDepthOpShapeHelper,
      ONNXSpaceToDepthOp, ONNXSpaceToDepthOpAdaptor>(*this, elementType);
}

LogicalResult ONNXStringNormalizerOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return emitError(NOT_IMPLEMENTED_MESSAGE);
}

LogicalResult ONNXTfIdfVectorizerOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return emitError(NOT_IMPLEMENTED_MESSAGE);
}

LogicalResult ONNXThresholdedReluOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return inferShapeForUnaryElementwiseOps(this->getOperation());
}

//===----------------------------------------------------------------------===//
// TopK
//===----------------------------------------------------------------------===//

LogicalResult ONNXTopKOp::verify() {
  ONNXTopKOpAdaptor operandAdaptor(*this);

  Value K = operandAdaptor.K();
  if (hasShapeAndRank(K)) {
    // K's rank must be zero or one.
    int64_t KRank = K.getType().cast<ShapedType>().getRank();
    if (KRank > 1)
      return onnx_mlir::Diagnostic::emitOperandHasUnexpectedRankError(
          *this->getOperation(), K, KRank, "< 2");
  }

  // axis attribute must be in the range [-r,r-1], where r = rank(X).
  Value X = operandAdaptor.X();
  if (hasShapeAndRank(X)) {
    int64_t Xrank = X.getType().cast<ShapedType>().getRank();
    int64_t axis = this->axis();

    if (axis < -Xrank || axis >= Xrank)
      return onnx_mlir::Diagnostic::emitAttributeOutOfRangeError(
          *this->getOperation(), "axis", axis,
          onnx_mlir::Diagnostic::Range<int64_t>(-Xrank, Xrank - 1));
  }

  return success();
}

LogicalResult ONNXTopKOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Cannot infer the output shape if the operands shape isn't known yet.
  if (llvm::any_of(this->getOperands(),
          [](const Value &op) { return !hasShapeAndRank(op); }))
    return success();

  Builder b(getContext());
  auto elementType = X().getType().cast<ShapedType>().getElementType();
  return shapeHelperInferMultipleShapes<ONNXTopKOpShapeHelper, ONNXTopKOp,
      ONNXTopKOpAdaptor>(*this, {elementType, b.getI64Type()});
}

//===----------------------------------------------------------------------===//
// Unique
//===----------------------------------------------------------------------===//

LogicalResult ONNXUniqueOp::verify() {
  Optional<int64_t> optionalSorted = sorted();
  if (optionalSorted.has_value()) {
    // optional sorted attribute must be zero or one.
    int64_t sorted = optionalSorted.value();
    if (sorted < 0 || sorted > 1)
      return onnx_mlir::Diagnostic::emitAttributeOutOfRangeError(
          *this->getOperation(), "sorted", sorted,
          onnx_mlir::Diagnostic::Range<int64_t>(0, 1));
  }

  ONNXUniqueOpAdaptor operandAdaptor(*this);
  Value X = operandAdaptor.X();
  if (!hasShapeAndRank(X))
    return success(); // Too early to verify.

  int64_t XRank = X.getType().cast<ShapedType>().getRank();
  Optional<int64_t> optionalAxis = axis();

  if (optionalAxis.has_value()) {
    // axis attribute must be in the range [-r,r-1], where r = rank(X).
    int64_t axis = optionalAxis.value();
    if (axis < -XRank || axis >= XRank)
      return onnx_mlir::Diagnostic::emitAttributeOutOfRangeError(
          *this->getOperation(), "axis", axis,
          onnx_mlir::Diagnostic::Range<int64_t>(-XRank, XRank - 1));
  }

  return success();
}

LogicalResult ONNXUniqueOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return emitError(NOT_IMPLEMENTED_MESSAGE);
}

LogicalResult ONNXUpsampleOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  if (!X().getType().isa<RankedTensorType>()) {
    return success();
  }
  if (!scales().getType().isa<RankedTensorType>()) {
    return success();
  }

  auto inputTy = X().getType().cast<RankedTensorType>();
  int32_t inputRank = inputTy.getShape().size();

  SmallVector<int64_t, 4> outputDims(inputRank, -1);

  // Extract the scale values
  auto scalesConstOp = getONNXConstantOp(scales());
  if (!scalesConstOp) {
    return success();
  }
  auto valueAttr = scalesConstOp.valueAttr().dyn_cast<DenseElementsAttr>();
  if (!valueAttr) {
    return emitError("Scales constant is not a DenseElementsAttr");
  }
  int scaleIdx = 0;
  // Why are the scale values float's?
  for (auto it = valueAttr.getValues<FloatAttr>().begin();
       it != valueAttr.getValues<FloatAttr>().end(); ++it) {
    outputDims[scaleIdx++] = (int)((*it).getValueAsDouble());
  }

  // Compute and set the output shape
  for (int i = 0; i < inputRank; ++i) {
    outputDims[i] *= inputTy.getShape()[i];
  }
  getResult().setType(
      RankedTensorType::get(outputDims, inputTy.getElementType()));

  return success();
}

LogicalResult ONNXUpsampleOp::verify() {
  if (!X().getType().isa<RankedTensorType>()) {
    return success();
  }
  if (!scales().getType().isa<RankedTensorType>()) {
    return success();
  }

  auto inputTy = X().getType().cast<RankedTensorType>();
  int32_t inputRank = inputTy.getShape().size();

  // Sanity checks on scale argument
  auto scalesTy = scales().getType().cast<RankedTensorType>();
  if (scalesTy.getShape().size() != 1) {
    return emitError("Scales tensor must be rank-1");
  }
  if (scalesTy.getShape()[0] != inputRank) {
    return emitError("Input tensor rank doesn't match scales tensor shape");
  }

  // Extract the scale values
  auto scalesConstOp = getONNXConstantOp(scales());
  if (!scalesConstOp) {
    return success();
  }
  auto valueAttr = scalesConstOp.valueAttr().dyn_cast<DenseElementsAttr>();
  if (!valueAttr) {
    return emitError("Scales constant is not a DenseElementsAttr");
  }

  int scaleIdx = 0;
  for (auto it = valueAttr.getValues<FloatAttr>().begin();
       it != valueAttr.getValues<FloatAttr>().end(); ++it) {
    if (scaleIdx >= inputRank) {
      return emitError("Scales tensor shape doesn't match # of scale values");
    }
    scaleIdx++;
  }
  if (scaleIdx != inputRank) {
    return emitError("Scales tensor shape doesn't match # of scale values");
  }
  return success();
}

LogicalResult ONNXWhereOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  if (!hasShapeAndRank(X()))
    return success();

  Type resultElementType = X().getType().cast<ShapedType>().getElementType();
  return inferShapeForBroadcastingOps<ONNXWhereOp, ONNXWhereOpAdaptor>(
      *this, resultElementType);
}

LogicalResult ONNXArrayFeatureExtractorOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return emitError(NOT_IMPLEMENTED_MESSAGE);
}

LogicalResult ONNXBinarizerOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return emitError(NOT_IMPLEMENTED_MESSAGE);
}

LogicalResult ONNXCastMapOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return emitError(NOT_IMPLEMENTED_MESSAGE);
}

LogicalResult ONNXCategoryMapperOp::verify() {
  ONNXCategoryMapperOpAdaptor operandAdaptor(*this);

  // Check input.
  const Value X = operandAdaptor.X();
  if (!hasShapeAndRank(X)) {
    // Won't be able to do any checking at this stage.
    return success();
  }

  ShapedType inputType = X.getType().cast<ShapedType>();
  Type elementType = inputType.getElementType();
  if (!elementType.isInteger(64) && !elementType.isa<ONNXStringType>())
    return emitOpError("input must be a tensor of int64 or string");

  // Check attributes.
  if (!cats_int64s())
    return emitOpError("cats_int64 attribute must be present");
  if (!cats_strings())
    return emitOpError("cats_strings attribute must be present");
  if (ArrayAttrSize(cats_int64s()) != ArrayAttrSize(cats_strings()))
    return emitOpError("cats_int64 and cats_strings should have the same size");

  if (elementType.isInteger(64) && !default_stringAttr())
    return emitOpError("'default_string' attribute is missing.");
  if (elementType.isa<ONNXStringType>() && !default_int64Attr())
    return emitOpError("'default_int64' attribute is missing.");

  return success();
}

LogicalResult ONNXCategoryMapperOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // Cannot infer shape if no shape exists.
  if (!X().getType().isa<RankedTensorType>())
    return success();

  Type inputElementType = X().getType().cast<ShapedType>().getElementType();
  assert((inputElementType.isInteger(64) ||
             inputElementType.isa<ONNXStringType>()) &&
         "Input tensor must have int64 or string element type.");

  Type outputElementType;
  if (inputElementType.isInteger(64))
    outputElementType = ONNXStringType::get(getContext());
  else
    outputElementType = IntegerType::get(getContext(), /*width=*/64);

  return shapeHelperInferShapes<ONNXCategoryMapperOpShapeHelper,
      ONNXCategoryMapperOp, ONNXCategoryMapperOpAdaptor>(
      *this, outputElementType);
}

LogicalResult ONNXDictVectorizerOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return emitError(NOT_IMPLEMENTED_MESSAGE);
}

LogicalResult ONNXFeatureVectorizerOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return emitError(NOT_IMPLEMENTED_MESSAGE);
}

LogicalResult ONNXImputerOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return emitError(NOT_IMPLEMENTED_MESSAGE);
}

LogicalResult ONNXLabelEncoderOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return emitError(NOT_IMPLEMENTED_MESSAGE);
}

LogicalResult ONNXLinearClassifierOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return emitError(NOT_IMPLEMENTED_MESSAGE);
}

LogicalResult ONNXLinearRegressorOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return emitError(NOT_IMPLEMENTED_MESSAGE);
}

LogicalResult ONNXNormalizerOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return emitError(NOT_IMPLEMENTED_MESSAGE);
}

LogicalResult ONNXSVMClassifierOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return emitError(NOT_IMPLEMENTED_MESSAGE);
}

LogicalResult ONNXSVMRegressorOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return emitError(NOT_IMPLEMENTED_MESSAGE);
}

LogicalResult ONNXTreeEnsembleClassifierOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return emitError(NOT_IMPLEMENTED_MESSAGE);
}

LogicalResult ONNXTreeEnsembleRegressorOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return emitError(NOT_IMPLEMENTED_MESSAGE);
}

LogicalResult ONNXZipMapOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return emitError(NOT_IMPLEMENTED_MESSAGE);
}

LogicalResult ONNXGridSampleOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  return emitError(NOT_IMPLEMENTED_MESSAGE);
}

#define NOT_IMPLEMENTED_INFERSHAPE(T)                                          \
  LogicalResult T::inferShapes(                                                \
      std::function<void(mlir::Region &)> doShapeInference) {                  \
    return emitError(NOT_IMPLEMENTED_MESSAGE);                                 \
  }

NOT_IMPLEMENTED_INFERSHAPE(ONNXAdagradOp)
NOT_IMPLEMENTED_INFERSHAPE(ONNXAdamOp)
NOT_IMPLEMENTED_INFERSHAPE(ONNXClipV6Op)
NOT_IMPLEMENTED_INFERSHAPE(ONNXClipV11Op)
NOT_IMPLEMENTED_INFERSHAPE(ONNXClipV12Op)
NOT_IMPLEMENTED_INFERSHAPE(ONNXGradientOp)
NOT_IMPLEMENTED_INFERSHAPE(ONNXMomentumOp)
NOT_IMPLEMENTED_INFERSHAPE(ONNXNegativeLogLikelihoodLossOp)
NOT_IMPLEMENTED_INFERSHAPE(ONNXPadV2Op)
NOT_IMPLEMENTED_INFERSHAPE(ONNXPadV11Op)
NOT_IMPLEMENTED_INFERSHAPE(ONNXResizeV11Op)
NOT_IMPLEMENTED_INFERSHAPE(ONNXResizeV10Op)
NOT_IMPLEMENTED_INFERSHAPE(ONNXSoftmaxCrossEntropyLossOp)
NOT_IMPLEMENTED_INFERSHAPE(ONNXUpsampleV9Op)
NOT_IMPLEMENTED_INFERSHAPE(ONNXUpsampleV7Op)

//===----------------------------------------------------------------------===//
// Loop
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXLoopOp.
LogicalResult ONNXLoopOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  auto builder = mlir::Builder(getContext());
  auto &loopBody = getRegion();
  assert(loopBody.getNumArguments() >= 2 &&
         "Loop body must take at least 2 inputs.");

  // We proceed to set types for loop body function inputs.
  // Set type for iteration number (trip count):
  loopBody.getArgument(0).setType(
      RankedTensorType::get({}, builder.getI64Type()));
  // Set type for termination condition:
  loopBody.getArgument(1).setType(
      RankedTensorType::get({}, builder.getI1Type()));

  // Set types for loop carried dependencies (i.e., set these loop carried
  // depdencies that appear in the body function input signature to have the
  // same type as their counterpart in LoopOp inputs).
  auto bodyInputs = loopBody.getArguments();
  auto bodyVRange = llvm::make_range(bodyInputs.begin() + 2, bodyInputs.end());
  for (auto opVToBodyVTy : llvm::zip(v_initial(), bodyVRange)) {
    auto opVTy = std::get<0>(opVToBodyVTy).getType();
    std::get<1>(opVToBodyVTy).setType(opVTy);
  }

  // Now we have modified loop body function input signatures according to
  // the knowledge we have on the inputs we pass to this function. Dispatch
  // shape inference to obtain body function output types.
  doShapeInference(loopBody);

  // Output loop variables should have the same type as their input
  // counterparts.
  auto bodyResultTys = loopBody.back().getTerminator()->getOperandTypes();
  // Compute the type range corresponding to the final values of
  // loop-carried dependencies/scan outputs in the body function output
  // types.
  auto scanStartItr = std::next(bodyResultTys.begin(), 1 + v_initial().size());
  auto bodyResVFinalTys =
      llvm::make_range(std::next(bodyResultTys.begin(), 1), scanStartItr);
  auto bodyResScanTys = llvm::make_range(scanStartItr, bodyResultTys.end());

  // Set shape for loop operation outputs corresponding to the final
  // values of loop-carried dependencies to be shape of their counterparts
  // in the body function output.
  for (auto vFinalValToTy : llvm::zip(v_final(), bodyResVFinalTys)) {
    std::get<0>(vFinalValToTy).setType(std::get<1>(vFinalValToTy));
  }

  // For scan outputs, we set their shape to be the shape of the return
  // values of the loop body function corresponding to scan outputs, but
  // with an extra leading dimension.
  for (auto vScanOutputValToTy : llvm::zip(scan_outputs(), bodyResScanTys)) {
    auto rankedScanTy =
        std::get<1>(vScanOutputValToTy).cast<RankedTensorType>();
    auto shape = rankedScanTy.getShape();
    SmallVector<int64_t, 4> unsqueezedShape(shape.begin(), shape.end());
    // Note that we may know the extent of the scan output leading
    // dimension, which is very likely just the trip count specified as an
    // input to Loop operation, but we need to eliminate the possibility of
    // early termination to be sure.
    unsqueezedShape.insert(unsqueezedShape.begin(), -1);
    updateType(std::get<0>(vScanOutputValToTy), unsqueezedShape,
        rankedScanTy.getElementType(), /*encoding=*/nullptr,
        /*refineShape=*/false);
  }

  return success();
}

// Helper function to obtain subset of op results corresponding to the final
// value of loop carried dependencies.
mlir::Operation::result_range ONNXLoopOp::v_final() {
  auto results = getResults();
  return llvm::make_range(
      results.begin(), results.begin() + v_initial().size());
}

// Helper function to obtain subset of op results corresponding to the scan
// outputs.
mlir::Operation::result_range ONNXLoopOp::scan_outputs() {
  auto results = getResults();
  return llvm::make_range(results.begin() + v_initial().size(), results.end());
}

//===----------------------------------------------------------------------===//
// CustomOp
//===----------------------------------------------------------------------===//
/// Infer the output shape of the ONNXCustomOp. This method is required by
/// the shape inference interface.
LogicalResult ONNXCustomOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  // getResult().setType(getOperand().getType());
  return success();
}

LogicalResult ONNXCallOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  // Check that the callee attribute was specified.
  auto fnAttr = (*this)->getAttrOfType<FlatSymbolRefAttr>("callee");
  if (!fnAttr)
    return emitOpError("requires a 'callee' symbol reference attribute");
  func::FuncOp fn =
      symbolTable.lookupNearestSymbolFrom<func::FuncOp>(*this, fnAttr);
  if (!fn)
    return emitOpError() << "'" << fnAttr.getValue()
                         << "' does not reference a valid function";

  // Verify that the operand and result types match the callee.
  auto fnType = fn.getFunctionType();
  if (fnType.getNumInputs() != getNumOperands())
    return emitOpError("incorrect number of operands for callee");

  for (unsigned i = 0, e = fnType.getNumInputs(); i != e; ++i)
    if (getOperand(i).getType() != fnType.getInput(i))
      return emitOpError("operand type mismatch: expected operand type ")
             << fnType.getInput(i) << ", but provided "
             << getOperand(i).getType() << " for operand number " << i;

  if (fnType.getNumResults() != getNumResults())
    return emitOpError("incorrect number of results for callee");

  for (unsigned i = 0, e = fnType.getNumResults(); i != e; ++i)
    if (getResult(i).getType() != fnType.getResult(i)) {
      auto diag = emitOpError("result type mismatch at index ") << i;
      diag.attachNote() << "      op result types: " << getResultTypes();
      diag.attachNote() << "function result types: " << fnType.getResults();
      return diag;
    }

  return success();
}

FunctionType ONNXCallOp::getCalleeType() {
  return FunctionType::get(getContext(), getOperandTypes(), getResultTypes());
}

//===----------------------------------------------------------------------===//
// SeqType
//===---------------------------------------------------------------------===//

mlir::Type SeqType::parse(mlir::AsmParser &parser) {
  Type elementType;
  if (parser.parseLess() || parser.parseType(elementType) ||
      parser.parseGreater()) {
    parser.emitError(parser.getCurrentLocation())
        << "failed to parse !onnx.Seq type";
    return Type();
  }

  return get(elementType, -1);
}

void SeqType::print(mlir::AsmPrinter &printer) const {
  // Previous implementation did not print/parse the length field
  // May add the field in future
  printer << "<" << getElementType() << ">";
}

//===----------------------------------------------------------------------===//
// DimOp
//===---------------------------------------------------------------------===//

LogicalResult ONNXDimOp::verify() {
  // Input data must be ranked.
  if (!hasShapeAndRank(this->data()))
    return failure();
  // Axis must be in [0, rank -1].
  int64_t axis = this->axis();
  return failure((axis < 0) || (axis >= getRank(this->data().getType())));
}

LogicalResult ONNXDimOp::inferShapes(
    std::function<void(mlir::Region &)> doShapeInference) {
  OpBuilder b(getContext());
  getResult().setType(RankedTensorType::get({1}, b.getI64Type()));
  return success();
}

//===----------------------------------------------------------------------===//
// TableGen'd op method definitions
//===----------------------------------------------------------------------===//

#define GET_OP_CLASSES
#include "src/Dialect/ONNX/ONNXOps.cpp.inc"
