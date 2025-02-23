// Copyright (c) 2019, NVIDIA CORPORATION. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef DALI_OPERATORS_EXPRESSIONS_ARITHMETIC_H_
#define DALI_OPERATORS_EXPRESSIONS_ARITHMETIC_H_

#include <limits>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "dali/core/format.h"
#include "dali/core/small_vector.h"
#include "dali/core/static_switch.h"
#include "dali/core/tensor_shape.h"
#include "dali/core/tensor_shape_print.h"
#include "dali/kernels/type_tag.h"
#include "dali/operators/expressions/arithmetic_meta.h"
#include "dali/operators/expressions/expression_impl_factory.h"
#include "dali/pipeline/operator/operator.h"

namespace dali {

using TileCover = std::tuple<std::vector<TileDesc>, std::vector<TileRange>>;

/**
 * @brief Divide the shape into groups of linear tiles
 */
inline TileCover GetTiledCover(const TensorListShape<> &shape, int tile_size,
                               int num_tiles_in_task) {
  Index total_elements = shape.num_elements();
  std::vector<TileDesc> descs;
  for (int sample_idx = 0; sample_idx < shape.num_samples(); sample_idx++) {
    int extent_idx = 0;
    Index sample_elements = shape[sample_idx].num_elements();
    for (Index covered = 0; covered < sample_elements; covered += tile_size, extent_idx++) {
      auto actual_tile_size =
          std::min(static_cast<Index>(tile_size), shape[sample_idx].num_elements() - covered);
      descs.push_back({sample_idx, extent_idx, static_cast<int>(actual_tile_size), tile_size});
    }
  }
  Index num_tasks = (descs.size() + num_tiles_in_task - 1) / num_tiles_in_task;
  std::vector<TileRange> ranges;
  ranges.reserve(num_tasks);
  for (int task = 0, tiles_used = 0; task < num_tasks; task++) {
    auto tiles_end = std::min(tiles_used + num_tiles_in_task, static_cast<int>(descs.size()));
    ranges.push_back({tiles_used, tiles_end});
    tiles_used = tiles_end;
  }
  return std::make_tuple(descs, ranges);
}

/**
 * @brief Recurse over expression tree and return the only matching layout
 */
template <typename Backend>
DLL_PUBLIC TensorLayout GetCommonLayout(ExprNode &expr, const workspace_t<Backend> &ws) {
  if (expr.GetNodeType() == NodeType::Constant) {
    return "";
  }
  if (expr.GetNodeType() == NodeType::Tensor) {
    auto &e = dynamic_cast<ExprTensor &>(expr);
    return ws.template InputRef<Backend>(e.GetInputIndex()).GetLayout();
  }
  if (expr.GetSubexpressionCount() == 0) {
    return "";
  }
  auto &func = dynamic_cast<ExprFunc &>(expr);
  auto result_layout = GetCommonLayout<Backend>(func[0], ws);
  for (int i = 1; i < expr.GetSubexpressionCount(); i++) {
    auto next_layout = GetCommonLayout<Backend>(func[i], ws);
    if (result_layout.empty()) {
      result_layout = next_layout;
      continue;
    }
    if (next_layout.empty()) {
      continue;
    }
    DALI_ENFORCE(
        result_layout == next_layout,
        make_string("Layouts of subexpressions", i - 1, "and", i, "for atihmetic operation",
                    func.GetFuncName(), "do not match. Expected", result_layout.c_str(), "got",
                    next_layout.c_str(), "."));
  }
  return result_layout;
}

/**
 * @brief Recurse over expression tree, fill the missing types of TensorInputs
 */
template <typename Backend>
DLL_PUBLIC DALIDataType PropagateTypes(ExprNode &expr, const workspace_t<Backend> &ws) {
  if (expr.GetNodeType() == NodeType::Constant) {
    return expr.GetTypeId();
  }
  if (expr.GetNodeType() == NodeType::Tensor) {
    auto &e = dynamic_cast<ExprTensor &>(expr);
    expr.SetTypeId(ws.template InputRef<Backend>(e.GetInputIndex()).type().id());
    return expr.GetTypeId();
  }
  auto &func = dynamic_cast<ExprFunc &>(expr);
  int subexpression_count = func.GetSubexpressionCount();
  DALI_ENFORCE(subexpression_count == 1 || subexpression_count == 2,
               "Only unary and binary expressions are supported");

  SmallVector<DALIDataType, kMaxArity> types;
  types.resize(subexpression_count);
  for (int i = 0; i < subexpression_count; i++) {
    types[i] = PropagateTypes<Backend>(func[i], ws);
  }
  expr.SetTypeId(TypePromotion(NameToOp(func.GetFuncName()), make_span(types)));
  return expr.GetTypeId();
}

template <typename Backend>
inline void CreateExecutionTasks(std::vector<ExprImplTask> &order, const ExprNode &expr,
                                 ExprImplCache &cache, cudaStream_t stream) {
  if (expr.GetNodeType() != NodeType::Function) {
    return;
  }
  auto &func = dynamic_cast<const ExprFunc &>(expr);
  for (int i = 0; i < expr.GetSubexpressionCount(); i++) {
    CreateExecutionTasks<Backend>(order, func[i], cache, stream);
  }
  order.push_back({cache.GetExprImpl<Backend>(func), {stream, &func}});
}

template <typename Backend>
inline std::vector<ExprImplTask> CreateExecutionTasks(const ExprNode &expr, ExprImplCache &cache,
                                                      cudaStream_t stream) {
  std::vector<ExprImplTask> result;
  CreateExecutionTasks<Backend>(result, expr, cache, stream);
  return result;
}

inline TensorListShape<> ShapePromotion(std::string op, span<const TensorListShape<> *> shapes) {
  const TensorListShape<> *out_shape = nullptr;
  for (int i = 0; i < shapes.size(); i++) {
    if (IsScalarLike(*shapes[i]))
      continue;
    if (!out_shape) {
      out_shape = shapes[i];
    } else {
      DALI_ENFORCE(*out_shape == *shapes[i],
                   make_string_delim("", "Input shapes of elemenetwise arithemtic operator \"", op,
                                     "\" do not match. Expected equal shapes, got: ", op, "(",
                                     *out_shape, ", ", *shapes[i], ")."));
    }
  }
  return out_shape ? *out_shape : TensorListShape<>{{1}};
}

template <typename Backend>
DLL_PUBLIC inline const TensorListShape<> &PropagateShapes(ExprNode &expr,
                                                    const workspace_t<Backend> &ws) {
  if (expr.GetNodeType() == NodeType::Constant) {
    expr.SetShape(TensorListShape<>{{1}});
    return expr.GetShape();
  }
  if (expr.GetNodeType() == NodeType::Tensor) {
    auto &e = dynamic_cast<ExprTensor &>(expr);
    expr.SetShape(ws.template InputRef<Backend>(e.GetInputIndex()).shape());
    return expr.GetShape();
  }
  auto &func = dynamic_cast<ExprFunc &>(expr);
  int subexpression_count = expr.GetSubexpressionCount();
  DALI_ENFORCE(subexpression_count == 1 || subexpression_count == 2,
               "Only unary and binary expressions are supported");

  SmallVector<const TensorListShape<> *, kMaxArity> shapes;
  shapes.resize(subexpression_count);
  for (int i = 0; i < subexpression_count; i++) {
    shapes[i] = &PropagateShapes<Backend>(func[i], ws);
  }
  func.SetShape(ShapePromotion(func.GetFuncName(), make_span(shapes)));
  return func.GetShape();
}

inline void GetConstantNodes(ExprNode &expr, std::vector<ExprConstant *> &nodes) {
  if (expr.GetNodeType() == NodeType::Constant) {
    nodes.push_back(dynamic_cast<ExprConstant *>(&expr));
    return;
  }
  if (expr.GetNodeType() == NodeType::Tensor) {
    return;
  }
  auto &func = dynamic_cast<ExprFunc &>(expr);
  for (int i = 0; i < func.GetSubexpressionCount(); i++) {
    GetConstantNodes(func[i], nodes);
  }
}

/**
 * @brief Arithmetic operator capable of executing expression tree of element-wise
 *        arithmetic operations.
 *
 * Only expressions consisting of one function node with tensor inputs are now supported.
 *
 * There are 3 levels for unit of work.
 * - Thread (CPUBackend) or CUDA kernel invokation (GPUBackend)
 * - Task - group of tiles to process by thread or CUDA kernel
 * - Tile - describes a portion of linear buffer, we try to split the amount of work
 *          evenly into tasks.
 *
 * For CPUBackend we have fixed number of threads that get to process a number of tasks,
 * so the work is evenly distributed. For GPUBackend we pack all tiles into 1 task, to limit
 * the number of CUDA calls.
 */
template <typename Backend>
class ArithmeticGenericOp : public Operator<Backend> {
 public:
  inline explicit ArithmeticGenericOp(const OpSpec &spec) : Operator<Backend>(spec) {
    expr_ = ParseExpressionString(spec.GetArgument<std::string>("expression_desc"));
  }

 protected:
  bool CanInferOutputs() const override {
    return true;
  }

  bool SetupImpl(std::vector<OutputDesc> &output_desc, const workspace_t<Backend> &ws) override {
    output_desc.resize(1);

    if (!types_layout_inferred_) {
      result_type_id_ = PropagateTypes<Backend>(*expr_, ws);
      result_layout_ = GetCommonLayout<Backend>(*expr_, ws);
      std::vector<ExprConstant *> constant_nodes;
      GetConstantNodes(*expr_, constant_nodes);
      constant_storage_.Initialize(spec_, ws.has_stream() ? ws.stream() : 0, constant_nodes);
      types_layout_inferred_ = true;
    }

    result_shape_ = PropagateShapes<Backend>(*expr_, ws);
    AllocateIntermediateNodes();
    exec_order_ = CreateExecutionTasks<Backend>(*expr_, cache_, ws.has_stream() ? ws.stream() : 0);

    output_desc[0] = {result_shape_, TypeTable::GetTypeInfo(result_type_id_)};
    std::tie(tile_cover_, tile_range_) = GetTiledCover(result_shape_, kTileSize, kTaskSize);
    return true;
  }

  using Operator<Backend>::RunImpl;
  void RunImpl(workspace_t<Backend> &ws) override;

 private:
  void AllocateIntermediateNodes() {
    auto &expr = *expr_;
    bool is_simple_expression = expr.GetNodeType() == NodeType::Function &&
                                expr.GetSubexpressionCount() > 0 &&
                                expr.GetSubexpressionCount() <= 2;
    auto &func = dynamic_cast<ExprFunc &>(expr);
    for (int i = 0; i < func.GetSubexpressionCount(); i++) {
      is_simple_expression = is_simple_expression && func[i].GetNodeType() != NodeType::Function;
    }

    DALI_ENFORCE(is_simple_expression,
                 "Complex expression trees are not yet supported. Only expressions containing one "
                 "function node with one or two inputs are supported.");
    // TODO(klecki): allocate memory for intermediate results and point the threads to them
  }

  std::unique_ptr<ExprNode> expr_;
  TensorListShape<> result_shape_;
  bool types_layout_inferred_ = false;
  DALIDataType result_type_id_ = DALIDataType::DALI_NO_TYPE;
  TensorLayout result_layout_;
  std::vector<TileDesc> tile_cover_;
  std::vector<TileRange> tile_range_;
  std::vector<ExprImplTask> exec_order_;
  std::vector<std::vector<ExtendedTileDesc>> tiles_per_task_;
  ConstantStorage<Backend> constant_storage_;
  ExprImplCache cache_;
  // For CPU we limit the tile size to limit the sizes of intermediate buffers
  // For GPU it's better to execute more at one time.
  static constexpr int kTileSize = std::is_same<Backend, CPUBackend>::value ? 4096 : 16384;
  // CPU packs up to 64 tiles in one task, GPU porcesses all of them in one task
  static constexpr int kTaskSize =
      std::is_same<Backend, CPUBackend>::value ? 64 : std::numeric_limits<int>::max();
  USE_OPERATOR_MEMBERS();
};

}  // namespace dali

#endif  // DALI_OPERATORS_EXPRESSIONS_ARITHMETIC_H_
