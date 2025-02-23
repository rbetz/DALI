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

#ifndef DALI_KERNELS_IMGPROC_RESAMPLE_RESAMPLING_SETUP_H_
#define DALI_KERNELS_IMGPROC_RESAMPLE_RESAMPLING_SETUP_H_

#include <cuda_runtime.h>
#include <memory>
#include <vector>
#include "dali/kernels/imgproc/resample/params.h"
#include "dali/kernels/imgproc/resample/resampling_filters.cuh"
#include "dali/core/dev_array.h"

namespace dali {
namespace kernels {

/**
 * @brief Maps a block (by blockIdx) to a sample.
 */
struct SampleBlockInfo {
  int sample, block_in_sample;
};

ResamplingFilter GetResamplingFilter(const ResamplingFilters *filters, const FilterDesc &params);

/**
 * @brief Builds and maintains resampling setup
 */
class SeparableResamplingSetup {
 public:
  enum ProcessingOrder : int {
    HorzVert,
    VertHorz
  };

  /**
   * Number of blocks per pass may differ depending on
   * the image aspect ratio and block aspect ratio.
   */
  struct BlockCount {
    int pass[2];
  };

  struct SampleDesc {
    using DevShape = DeviceArray<int, 2>;

    DeviceArray<uintptr_t, 3> pointers;
    DeviceArray<ptrdiff_t, 3> offsets;
    DeviceArray<int, 3>       strides;
    DeviceArray<DevShape, 3>  shapes;

    template <typename Input, typename Tmp, typename Output>
    __host__ __device__ void set_base_pointers(Input *in, Tmp *tmp, Output *out) {
      in_ptr()  = reinterpret_cast<uintptr_t>(in  + in_offset());
      tmp_ptr() = reinterpret_cast<uintptr_t>(tmp + tmp_offset());
      out_ptr() = reinterpret_cast<uintptr_t>(out + out_offset());
    }

    __host__ __device__ DevShape &in_shape() { return shapes[0]; }
    __host__ __device__ const DevShape &in_shape() const { return shapes[0]; }
    __host__ __device__ DevShape &tmp_shape() { return shapes[1]; }
    __host__ __device__ const DevShape &tmp_shape() const { return shapes[1]; }
    __host__ __device__ DevShape &out_shape() { return shapes[2]; }
    __host__ __device__ const DevShape &out_shape() const { return shapes[2]; }

    __host__ __device__ int  &in_stride()       { return strides[0]; }
    __host__ __device__ int   in_stride() const { return strides[0]; }
    __host__ __device__ int &tmp_stride()       { return strides[1]; }
    __host__ __device__ int  tmp_stride() const { return strides[1]; }
    __host__ __device__ int &out_stride()       { return strides[2]; }
    __host__ __device__ int  out_stride() const { return strides[2]; }

    __host__ __device__ uintptr_t  &in_ptr()       { return pointers[0]; }
    __host__ __device__ uintptr_t   in_ptr() const { return pointers[0]; }
    __host__ __device__ uintptr_t &tmp_ptr()       { return pointers[1]; }
    __host__ __device__ uintptr_t  tmp_ptr() const { return pointers[1]; }
    __host__ __device__ uintptr_t &out_ptr()       { return pointers[2]; }
    __host__ __device__ uintptr_t  out_ptr() const { return pointers[2]; }

    __host__ __device__ ptrdiff_t  &in_offset()       { return offsets[0]; }
    __host__ __device__ ptrdiff_t   in_offset() const { return offsets[0]; }
    __host__ __device__ ptrdiff_t &tmp_offset()       { return offsets[1]; }
    __host__ __device__ ptrdiff_t  tmp_offset() const { return offsets[1]; }
    __host__ __device__ ptrdiff_t &out_offset()       { return offsets[2]; }
    __host__ __device__ ptrdiff_t  out_offset() const { return offsets[2]; }

    template <typename T>
    __host__ __device__ const T *in_ptr() const { return reinterpret_cast<const T*>(pointers[0]); }
    template <typename T>
    __host__ __device__ T *tmp_ptr() const { return reinterpret_cast<T*>(pointers[1]); }
    template <typename T>
    __host__ __device__ T *out_ptr() const { return reinterpret_cast<T*>(pointers[2]); }

    DeviceArray<float, 2> origin, scale;

    ProcessingOrder order;
    int channels;
    ResamplingFilterType filter_type[2];
    ResamplingFilter filter[2];

    BlockCount block_count;
  };

  DLL_PUBLIC void SetupSample(SampleDesc &desc,
                              const TensorShape<3> &in_shape,
                              const ResamplingParams2D &params);

  void Initialize() {
    filters = GetResamplingFilters();
  }
  void InitializeCPU() {
    filters = GetResamplingFiltersCPU();
  }

  int2 block_size = { 32, 24 };

 protected:
  struct ROI {
    int lo[2], hi[2];
    int size(int dim) const { return hi[dim] - lo[dim]; }
  };

  void SetFilters(SampleDesc &desc, const ResamplingParams2D &params);
  ROI ComputeScaleAndROI(SampleDesc &desc, const ResamplingParams2D &params);

  std::shared_ptr<ResamplingFilters> filters;

  static_assert(std::is_pod<SampleDesc>::value,
    "Internal error! Something crept into SampleDesc and made it non-POD");
};

class BatchResamplingSetup : public SeparableResamplingSetup {
 public:
  using Params = span<ResamplingParams2D>;

  std::vector<SampleDesc> sample_descs;
  TensorListShape<3> output_shape, intermediate_shape;
  size_t intermediate_size;
  BlockCount total_blocks;

  DLL_PUBLIC void SetupBatch(const TensorListShape<3> &in, const Params &params);

  template <typename Collection>
  void SetupBatch(const TensorListShape<3> &in, const Collection &params) {
    SetupBatch(in, make_span(params));
  }
  DLL_PUBLIC void InitializeSampleLookup(const OutTensorCPU<SampleBlockInfo, 1> &sample_lookup);
};

}  // namespace kernels
}  // namespace dali

#endif  // DALI_KERNELS_IMGPROC_RESAMPLE_RESAMPLING_SETUP_H_
