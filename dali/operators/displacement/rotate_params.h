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

#ifndef DALI_OPERATORS_DISPLACEMENT_ROTATE_PARAMS_H_
#define DALI_OPERATORS_DISPLACEMENT_ROTATE_PARAMS_H_

#include <sstream>
#include <string>
#include <vector>
#include "dali/pipeline/operator/operator.h"
#include "dali/kernels/imgproc/warp/affine.h"
#include "dali/kernels/imgproc/warp/mapping_traits.h"
#include "dali/kernels/imgproc/roi.h"
#include "dali/operators/displacement/warp_param_provider.h"
#include "dali/core/tensor_shape_print.h"
#include "dali/core/format.h"

namespace dali {

template <typename Backend, int spatial_ndim, typename BorderType>
class RotateParamProvider;

template <int spatial_ndim>
using RotateParams = kernels::AffineMapping<spatial_ndim>;

inline TensorShape<2> RotatedCanvasSize(TensorShape<2> input_size, double angle) {
  TensorShape<2> out_size;
  double eps = 1e-2;
  double abs_cos = std::abs(std::cos(angle));
  double abs_sin = std::abs(std::sin(angle));
  int w = input_size[1];
  int h = input_size[0];
  int w_out = std::ceil(abs_cos * w + abs_sin * h - eps);
  int h_out = std::ceil(abs_cos * h + abs_sin * w - eps);
  if (abs_sin <= abs_cos) {
    // if rotated by less than 45deg, maintain size parity to reduce blur
    if (w_out % 2 != w % 2)
      w_out++;
    if (h_out % 2 != h % 2)
      h_out++;
  } else {
    // if rotated by more than 45deg, swap size parity to reduce blur
    if (h_out % 2 != w % 2)
      h_out++;
    if (w_out % 2 != h % 2)
      w_out++;
  }
  out_size = { h_out, w_out };
  return out_size;
}

template <typename Backend, typename BorderType>
class RotateParamProvider<Backend, 2, BorderType>
: public WarpParamProvider<Backend, 2, RotateParams<2>, BorderType> {
 protected:
  static constexpr int spatial_ndim = 2;
  using MappingParams = RotateParams<spatial_ndim>;
  using Base = WarpParamProvider<Backend, spatial_ndim, MappingParams, BorderType>;
  using Workspace = typename Base::Workspace;
  using Base::ws_;
  using Base::spec_;
  using Base::params_gpu_;
  using Base::params_cpu_;
  using Base::num_samples_;
  using Base::out_sizes_;

  void SetParams() override {
    input_shape_ = convert_dim<spatial_ndim + 1>(ws_->template InputRef<Backend>(0).shape());
    Collect(angles_, "angle", true);
  }

  template <typename T>
  void CopyIgnoreShape(vector<T> &out, const TensorListView<StorageCPU, const T> &TL) {
    int64_t n = TL.num_elements();
    out.resize(n);
    if (!n)
      return;
    int64_t sample_size = TL.shape[0].num_elements();
    int s = 0;  // sample index
    int64_t ofs = 0;  // offset within sample
    for (int64_t i = 0; i < n; i++) {
      out[i] = TL.data[s][ofs++];
      if (ofs == sample_size) {
        ofs = 0;
        sample_size = TL.shape[++s].num_elements();
      }
    }
  }

  template <typename T, int N>
  void CopyIgnoreShape(vector<vec<N, T>> &out, const TensorListView<StorageCPU, const T> &TL) {
    int64_t n = TL.num_elements();
    out.resize(n);
    if (!n)
      return;
    int64_t sample_size = TL.shape[0].num_elements();
    int s = 0;  // sample index
    int64_t ofs = 0;  // offset within sample
    for (int64_t i = 0; i < n; i++) {
      for (int j = 0; j < N; j++) {
        out[i][j] = TL.data[s][ofs++];
        if (ofs == sample_size) {
          ofs = 0;
          sample_size = TL.shape[++s].num_elements();
        }
      }
    }
  }

  template <typename T>
  void Collect(std::vector<T> &v, const std::string &name, bool required) {
    if (spec_->HasTensorArgument(name)) {
      auto arg_view = dali::view<const T>(ws_->ArgumentInput(name));
      int n = arg_view.num_elements();
      // TODO(michalz): handle TensorListView when #1390 is merged
      DALI_ENFORCE(n == num_samples_, make_string(
        "Unexpected number of elements in argument `", name, "`: ", n,
        "; expected: ", num_samples_));
      CopyIgnoreShape(v, arg_view);
    } else {
      T scalar;
      v.clear();

      if (required)
        scalar = spec_->template GetArgument<T>(name);

      if (required || spec_->TryGetArgument(scalar, name))
        v.resize(num_samples_, scalar);
    }
  }


  void AdjustParams() override {
    using kernels::shape2vec;
    using kernels::skip_dim;
    assert(input_shape_.num_samples() == num_samples_);
    assert(static_cast<int>(out_sizes_.size()) == num_samples_);

    auto *params = this->AllocParams(kernels::AllocType::Host);
    for (int i = 0; i < num_samples_; i++) {
      ivec2 in_size = shape2vec(skip_dim<2>(input_shape_[i]));
      ivec2 out_size = shape2vec(out_sizes_[i]);

      float a = deg2rad(angles_[i]);
      mat3 M = translation(in_size*0.5f) * rotation2D(a) * translation(-out_size*0.5f);
      params[i] = sub<2, 3>(M);
    }
  }

  void InferSize() override {
    assert(static_cast<int>(out_sizes_.size()) == num_samples_);
    for (int i = 0; i < num_samples_; i++) {
      auto in_shape = kernels::skip_dim<2>(input_shape_[i]);
      out_sizes_[i] = RotatedCanvasSize(in_shape, deg2rad(angles_[i]));
    }
  }

  bool ShouldInferSize() const override {
    return !this->HasExplicitSize() && !this->KeepOriginalSize();
  }

  bool KeepOriginalSize() const override {
    return spec_->template GetArgument<bool>("keep_size");
  }

  std::vector<float> angles_;
  TensorListShape<spatial_ndim + 1> input_shape_;
};

}  // namespace dali

#endif  // DALI_OPERATORS_DISPLACEMENT_ROTATE_PARAMS_H_
