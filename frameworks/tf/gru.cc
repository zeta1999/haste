// Copyright 2020 LMNT, Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// ==============================================================================

#include <cuda_runtime_api.h>
#include <vector>

#include "haste.h"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/shape_inference.h"
#include "tensorflow/core/util/stream_executor_util.h"
#include "tensorflow/stream_executor/stream.h"

using namespace tensorflow;

using haste::v0::gru::BackwardPass;
using haste::v0::gru::ForwardPass;
using tensorflow::se::Stream;
using tensorflow::shape_inference::DimensionHandle;
using tensorflow::shape_inference::InferenceContext;
using tensorflow::shape_inference::ShapeHandle;

#define REGISTER_GPU_KERNEL(NAME, T)                 \
  REGISTER_KERNEL_BUILDER(Name(#NAME)                \
                            .Device(DEVICE_GPU)      \
                            .TypeConstraint<T>("R"), \
                          NAME##Op<T>)

// LOL.
struct CublasHandleContainer {
  CublasHandleContainer() {
    int count;
    int current_device;
    cudaGetDevice(&current_device);
    cudaGetDeviceCount(&count);
    for (int i = 0; i < count; ++i) {
      cublasHandle_t handle;
      cudaSetDevice(i);
      cublasCreate(&handle);
      handles.push_back(handle);
    }
    cudaSetDevice(current_device);
  }

  ~CublasHandleContainer() {
    for (auto h : handles)
      cublasDestroy(h);
  }

  std::vector<cublasHandle_t> handles;
};

static cublasHandle_t GetCublasHandle() {
  static CublasHandleContainer all_handles;
  int device;
  cudaGetDevice(&device);
  return all_handles.handles[device];
}

// Define the interface and shape function for the op.
REGISTER_OP("HasteGru")
    .Attr("R: {float, double}")         // Some real number type.
    .Attr("training: bool")
    .Attr("zoneout_prob: float")
    .Input("x: R")                      // [T,N,C]
    .Input("kernel: R")                 // [C,H*3]
    .Input("recurrent_kernel: R")       // [H,H*3]
    .Input("bias: R")                   // [H*3]
    .Input("recurrent_bias: R")         // [H*3]
    .Input("zoneout_mask: R")           // [T,N,H]
    .Output("h: R")                     // [T,N,H]
    .Output("v: R")                     // [T,N,H*4]
    .SetShapeFn([](InferenceContext* c) {
      ShapeHandle input_shape;
      ShapeHandle kernel_shape;
      ShapeHandle recurrent_shape;
      ShapeHandle bias_shape;
      ShapeHandle recurrent_bias_shape;
      ShapeHandle zoneout_mask_shape;

      TF_RETURN_IF_ERROR(c->WithRank(c->input(0), 3, &input_shape));
      TF_RETURN_IF_ERROR(c->WithRank(c->input(1), 2, &kernel_shape));
      TF_RETURN_IF_ERROR(c->WithRank(c->input(2), 2, &recurrent_shape));
      TF_RETURN_IF_ERROR(c->WithRank(c->input(3), 1, &bias_shape));
      TF_RETURN_IF_ERROR(c->WithRank(c->input(4), 1, &recurrent_bias_shape));
      TF_RETURN_IF_ERROR(c->WithRank(c->input(5), 3, &zoneout_mask_shape));

      const DimensionHandle time_steps = c->Dim(input_shape, 0);
      const DimensionHandle batch_size = c->Dim(input_shape, 1);
      const DimensionHandle hidden_size = c->Dim(recurrent_shape, 0);
      DimensionHandle hidden_size_4;

      TF_RETURN_IF_ERROR(c->Multiply(hidden_size, 4, &hidden_size_4));

      c->set_output(0, c->MakeShape({ time_steps, batch_size, hidden_size }));
      c->set_output(1, c->MakeShape({ time_steps, batch_size, hidden_size_4 }));
      return Status::OK();
    });

template<typename T>
struct HasteGruOp : public OpKernel {
  explicit HasteGruOp(OpKernelConstruction* context) : OpKernel(context) {
    OP_REQUIRES_OK(context, context->GetAttr("training", &training_));
    OP_REQUIRES_OK(context, context->GetAttr("zoneout_prob", &zoneout_prob_));
  }

  // When running on GPU, TF backs all inputs and outputs with device memory
  // and not host memory. We don't need to do explicit memory copies or allocations
  // for the inputs and outputs.
  void Compute(OpKernelContext* context) override {
    const Tensor& input = context->input(0);
    const Tensor& kernel = context->input(1);
    const Tensor& recurrent_kernel = context->input(2);
    const Tensor& bias = context->input(3);
    const Tensor& recurrent_bias = context->input(4);
    const Tensor& zoneout_mask = context->input(5);

    const auto time_steps = input.shape().dim_size(0);
    const auto batch_size = input.shape().dim_size(1);
    const auto input_size = input.shape().dim_size(2);
    const auto hidden_size = recurrent_kernel.shape().dim_size(0);
    const bool has_zoneout = zoneout_prob_ && zoneout_mask.NumElements();
    const auto data_type = DataTypeToEnum<T>::value;

    OP_REQUIRES(context, input_size == kernel.shape().dim_size(0),
        errors::InvalidArgument("input[2] and kernel[0] dimensions must match. Found ",
            input_size, " and ", kernel.shape().dim_size(0)));

    const TensorShape output_shape = { time_steps, batch_size, hidden_size };
    const TensorShape v_out_shape = { time_steps, batch_size, training_ ? hidden_size * 4 : 0 };

    Tensor* output = nullptr;
    OP_REQUIRES_OK(context, context->allocate_output(0, output_shape, &output));

    Tensor* v_out = nullptr;
    OP_REQUIRES_OK(context, context->allocate_output(1, v_out_shape, &v_out));

    Tensor tmp_Wx;
    const TensorShape tmp_Wx_shape = { time_steps, batch_size, hidden_size * 3};
    OP_REQUIRES_OK(context, context->allocate_temp(data_type, tmp_Wx_shape, &tmp_Wx));

    Tensor tmp_Rh;
    const TensorShape tmp_Rh_shape = { batch_size, hidden_size * 3 };
    OP_REQUIRES_OK(context, context->allocate_temp(data_type, tmp_Rh_shape, &tmp_Rh));

    cudaMemset(output->flat<T>().data(), 0, output->AllocatedBytes());

    ForwardPass<T> forward = ForwardPass<T>(
        training_,
        batch_size,
        input_size,
        hidden_size,
        GetCublasHandle());

    Tensor h = output->SubSlice(0);
    for (int64 i = 0; i < time_steps; ++i) {
      Tensor x = input.SubSlice(i);
      Tensor new_h = output->SubSlice(i);
      Tensor v = v_out->SubSlice(i);
      Tensor tmp_Wx_cur = tmp_Wx.SubSlice(i);

      forward.Iterate(
          kernel.flat<T>().data(),
          recurrent_kernel.flat<T>().data(),
          bias.flat<T>().data(),
          recurrent_bias.flat<T>().data(),
          x.unaligned_flat<T>().data(),
          h.unaligned_flat<T>().data(),
          new_h.unaligned_flat<T>().data(),
          training_ ? v.unaligned_flat<T>().data() : nullptr,
          tmp_Wx_cur.unaligned_flat<T>().data(),
          tmp_Rh.flat<T>().data(),
          has_zoneout ? zoneout_prob_ : 0.0f,
          has_zoneout ? zoneout_mask.SubSlice(i).unaligned_flat<T>().data() : nullptr);
      h = new_h;
    }
  }

  private:
    bool training_;
    float zoneout_prob_;
};

REGISTER_GPU_KERNEL(HasteGru, float);
REGISTER_GPU_KERNEL(HasteGru, double);

REGISTER_OP("HasteGruGrad")
    .Attr("R: {float, double}")
    .Input("x_t: R")                   // [T,C,N]
    .Input("kernel_t: R")              // [H*3,C]
    .Input("recurrent_kernel_t: R")    // [H*3,H]
    .Input("bias: R")                  // [H*3]
    .Input("recurrent_bias: R")        // [H*3]
    .Input("h_t: R")                   // [T,H,N]
    .Input("v: R")                     // [T,N,H*4]
    .Input("dh_new: R")                // [T,N,H]
    .Input("zoneout_mask: R")          // [T,N,H]
    .Output("dx: R")                   // [T,N,C]
    .Output("dw: R")                   // [C,H*3]
    .Output("dr: R")                   // [H,H*3]
    .Output("dbx: R")                  // [H*3]
    .Output("dbr: R")                  // [H*3]
    .SetShapeFn([](InferenceContext* c) {
      ShapeHandle x_shape;
      ShapeHandle kernel_shape;
      ShapeHandle recurrent_kernel_shape;
      ShapeHandle bias_shape;
      ShapeHandle recurrent_bias_shape;
      ShapeHandle h_shape;
      ShapeHandle v_shape;
      ShapeHandle dh_new_shape;
      ShapeHandle zoneout_mask_shape;

      TF_RETURN_IF_ERROR(c->WithRank(c->input(0), 3, &x_shape));
      TF_RETURN_IF_ERROR(c->WithRank(c->input(1), 2, &kernel_shape));
      TF_RETURN_IF_ERROR(c->WithRank(c->input(2), 2, &recurrent_kernel_shape));
      TF_RETURN_IF_ERROR(c->WithRank(c->input(3), 1, &bias_shape));
      TF_RETURN_IF_ERROR(c->WithRank(c->input(4), 1, &recurrent_bias_shape));
      TF_RETURN_IF_ERROR(c->WithRank(c->input(5), 3, &h_shape));
      TF_RETURN_IF_ERROR(c->WithRank(c->input(6), 3, &v_shape));
      TF_RETURN_IF_ERROR(c->WithRank(c->input(7), 3, &dh_new_shape));
      TF_RETURN_IF_ERROR(c->WithRank(c->input(8), 3, &zoneout_mask_shape));

      DimensionHandle time_steps = c->Dim(x_shape, 0);
      DimensionHandle input_size = c->Dim(x_shape, 1);
      DimensionHandle batch_size = c->Dim(x_shape, 2);
      DimensionHandle hidden_size = c->Dim(recurrent_kernel_shape, 1);

      c->set_output(0, c->MakeShape({ time_steps, batch_size, input_size }));
      c->set_output(1, c->MakeShape({ input_size, c->Value(hidden_size) * 3 }));
      c->set_output(2, c->MakeShape({ hidden_size, c->Value(hidden_size) * 3 }));
      c->set_output(3, bias_shape);
      c->set_output(4, recurrent_bias_shape);
      return Status::OK();
    });

template<typename T>
struct HasteGruGradOp : public OpKernel {
  explicit HasteGruGradOp(OpKernelConstruction* context) : OpKernel(context) {}

  void Compute(OpKernelContext* context) override {
    const Tensor& input = context->input(0);
    const Tensor& kernel = context->input(1);
    const Tensor& recurrent_kernel = context->input(2);
    const Tensor& bias = context->input(3);
    const Tensor& recurrent_bias = context->input(4);
    const Tensor& h_vector = context->input(5);
    const Tensor& v_vector = context->input(6);
    const Tensor& dh_new = context->input(7);
    const Tensor& zoneout_mask = context->input(8);

    const auto time_steps = input.shape().dim_size(0);
    const auto input_size = input.shape().dim_size(1);
    const auto batch_size = input.shape().dim_size(2);
    const auto hidden_size = recurrent_kernel.shape().dim_size(1);
    const bool has_zoneout = !!zoneout_mask.NumElements();
    const auto data_type = DataTypeToEnum<T>::value;

    // Can be uninitialized. Output only, no accumulation.
    const TensorShape dx_shape = { time_steps, batch_size, input_size };
    Tensor* dx = nullptr;
    OP_REQUIRES_OK(context, context->allocate_output(0, dx_shape, &dx));

    // Needs to be initialized to 0.
    const TensorShape dW_shape = { input_size, hidden_size * 3 };
    Tensor* dW = nullptr;
    OP_REQUIRES_OK(context, context->allocate_output(1, dW_shape, &dW));

    // Needs to be initialized to 0.
    const TensorShape dR_shape = { hidden_size, hidden_size * 3 };
    Tensor* dR = nullptr;
    OP_REQUIRES_OK(context, context->allocate_output(2, dR_shape, &dR));

    // Needs to be initialized to 0.
    const TensorShape dbx_shape = { hidden_size * 3 };
    Tensor* dbx = nullptr;
    OP_REQUIRES_OK(context, context->allocate_output(3, dbx_shape, &dbx));

    // Needs to be initialized to 0.
    const TensorShape dbr_shape = { hidden_size * 3 };
    Tensor* dbr = nullptr;
    OP_REQUIRES_OK(context, context->allocate_output(4, dbr_shape, &dbr));

    // Needs to be initialized to 0.
    const TensorShape dh_shape = { batch_size, hidden_size };
    Tensor dh;
    OP_REQUIRES_OK(context, context->allocate_temp(data_type, dh_shape, &dh));

    // Can be uninitialized. Output only, no accumulation.
    const TensorShape dp_shape = { time_steps, batch_size, hidden_size * 3 };
    Tensor dp;
    OP_REQUIRES_OK(context, context->allocate_temp(data_type, dp_shape, &dp));

    // Can be uninitialized. Output only, no accumulation.
    const TensorShape dq_shape = { time_steps, batch_size, hidden_size * 3 };
    Tensor dq;
    OP_REQUIRES_OK(context, context->allocate_temp(data_type, dq_shape, &dq));

    // Needs to be initialized to 0.
    const TensorShape zero_vector_shape = { batch_size, hidden_size };
    Tensor zero_vector;
    OP_REQUIRES_OK(context, context->allocate_temp(data_type, zero_vector_shape, &zero_vector));

    cudaMemset(dW->flat<T>().data(), 0, dW->AllocatedBytes());
    cudaMemset(dR->flat<T>().data(), 0, dR->AllocatedBytes());
    cudaMemset(dbx->flat<T>().data(), 0, dbx->AllocatedBytes());
    cudaMemset(dbr->flat<T>().data(), 0, dbr->AllocatedBytes());
    cudaMemset(dh.flat<T>().data(), 0, dh.AllocatedBytes());
    cudaMemset(zero_vector.flat<T>().data(), 0, zero_vector.AllocatedBytes());

    BackwardPass<T> backward = BackwardPass<T>(
        batch_size,
        input_size,
        hidden_size,
        GetCublasHandle());

    for (int64 i = time_steps - 1; i >= 0; --i) {
      Tensor x = input.SubSlice(i);

      // These are slices of cell outputs so we use (i - 1) to get the
      // cell inputs (i.e., h_t is the output of the t'th LSTM cell
      // which is also the input to the t+1'th cell).
      Tensor h = i != 0 ? h_vector.SubSlice(i - 1) : zero_vector;
      Tensor v = v_vector.SubSlice(i);

      Tensor dh_new_cur = dh_new.SubSlice(i);
      Tensor dx_cur = dx->SubSlice(i);
      Tensor dp_cur = dp.SubSlice(i);
      Tensor dq_cur = dq.SubSlice(i);

      backward.Iterate(
          kernel.flat<T>().data(),
          recurrent_kernel.flat<T>().data(),
          bias.flat<T>().data(),
          recurrent_bias.flat<T>().data(),
          x.unaligned_flat<T>().data(),
          h.unaligned_flat<T>().data(),
          v.unaligned_flat<T>().data(),
          dh_new_cur.unaligned_flat<T>().data(),
          dx_cur.unaligned_flat<T>().data(),
          dW->flat<T>().data(),
          dR->flat<T>().data(),
          dbx->flat<T>().data(),
          dbr->flat<T>().data(),
          dh.flat<T>().data(),
          dp_cur.unaligned_flat<T>().data(),
          dq_cur.unaligned_flat<T>().data(),
          has_zoneout ? zoneout_mask.SubSlice(i).unaligned_flat<T>().data() : nullptr);
    }
  }
};

REGISTER_GPU_KERNEL(HasteGruGrad, float);
REGISTER_GPU_KERNEL(HasteGruGrad, double);
