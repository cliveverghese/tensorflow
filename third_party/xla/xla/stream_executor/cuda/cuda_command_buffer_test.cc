/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

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

#include <algorithm>
#include <cstdint>
#include <vector>

#include "absl/log/check.h"
#include "third_party/gpus/cuda/include/cuda.h"
#include "xla/stream_executor/command_buffer.h"
#include "xla/stream_executor/cuda/cuda_test_kernels.h"
#include "xla/stream_executor/kernel.h"
#include "xla/stream_executor/launch_dim.h"
#include "xla/stream_executor/multi_platform_manager.h"
#include "xla/stream_executor/platform.h"
#include "xla/stream_executor/stream.h"
#include "xla/stream_executor/stream_executor.h"
#include "tsl/lib/core/status_test_util.h"
#include "tsl/platform/status.h"
#include "tsl/platform/test.h"
#include "tsl/platform/test_benchmark.h"

namespace stream_executor::cuda {

using AddI32Kernel = TypedKernel<DeviceMemory<int32_t>, DeviceMemory<int32_t>,
                                 DeviceMemory<int32_t>>;

using AddI32Ptrs3 = TypedKernel<internal::Ptrs3<int32_t>>;

static constexpr auto nested = CommandBuffer::Mode::kNested;    // NOLINT
static constexpr auto primary = CommandBuffer::Mode::kPrimary;  // NOLINT

TEST(CudaCommandBufferTest, LaunchSingleKernel) {
  Platform* platform = MultiPlatformManager::PlatformWithName("CUDA").value();
  StreamExecutor* executor = platform->ExecutorForDevice(0).value();

  Stream stream(executor);
  stream.Init();
  ASSERT_TRUE(stream.ok());

  MultiKernelLoaderSpec spec(/*arity=*/3);
  spec.AddInProcessSymbol(internal::GetAddI32CudaKernel(), "add");

  AddI32Kernel add(executor);
  TF_ASSERT_OK(executor->GetKernel(spec, &add));

  int64_t length = 4;
  int64_t byte_length = sizeof(int32_t) * length;

  // Prepare arguments: a=1, b=2, c=0
  DeviceMemory<int32_t> a = executor->AllocateArray<int32_t>(length, 0);
  DeviceMemory<int32_t> b = executor->AllocateArray<int32_t>(length, 0);
  DeviceMemory<int32_t> c = executor->AllocateArray<int32_t>(length, 0);

  stream.ThenMemset32(&a, 1, byte_length);
  stream.ThenMemset32(&b, 2, byte_length);
  stream.ThenMemZero(&c, byte_length);

  // Create a command buffer with a single kernel launch.
  auto cmd_buffer = CommandBuffer::Create(executor).value();
  TF_ASSERT_OK(cmd_buffer.Launch(add, ThreadDim(), BlockDim(4), a, b, c));
  TF_ASSERT_OK(cmd_buffer.Finalize());

  TF_ASSERT_OK(executor->Submit(&stream, cmd_buffer));

  // Copy `c` data back to host.
  std::vector<int32_t> dst(4, 42);
  stream.ThenMemcpy(dst.data(), c, byte_length);

  std::vector<int32_t> expected = {3, 3, 3, 3};
  ASSERT_EQ(dst, expected);

  // Prepare argument for graph update: d = 0
  DeviceMemory<int32_t> d = executor->AllocateArray<int32_t>(length, 0);
  stream.ThenMemZero(&d, byte_length);

  // Update command buffer to write into `d` buffer.
  TF_ASSERT_OK(cmd_buffer.Update());
  TF_ASSERT_OK(cmd_buffer.Launch(add, ThreadDim(), BlockDim(4), a, b, d));
  TF_ASSERT_OK(cmd_buffer.Finalize());

  TF_ASSERT_OK(executor->Submit(&stream, cmd_buffer));

  // Copy `d` data back to host.
  std::fill(dst.begin(), dst.end(), 42);
  stream.ThenMemcpy(dst.data(), d, byte_length);
  ASSERT_EQ(dst, expected);
}

TEST(CudaCommandBufferTest, TraceSingleKernel) {
  Platform* platform = MultiPlatformManager::PlatformWithName("CUDA").value();
  StreamExecutor* executor = platform->ExecutorForDevice(0).value();

  Stream stream(executor);
  stream.Init();
  ASSERT_TRUE(stream.ok());

  AddI32Ptrs3 add(executor);

  // Register a kernel with a custom arguments packing function that packs
  // device memory arguments into a struct with pointers.
  MultiKernelLoaderSpec spec(/*arity=*/1, [&](const KernelArgs& args) {
    auto bufs = Cast<KernelArgsDeviceMemoryArray>(&args)->device_memory_args();
    auto cast = [](auto m) { return reinterpret_cast<int32_t*>(m.opaque()); };
    return PackKernelArgs(add, internal::Ptrs3<int32_t>{
                                   cast(bufs[0]),
                                   cast(bufs[1]),
                                   cast(bufs[2]),
                               });
  });
  spec.AddInProcessSymbol(internal::GetAddI32Ptrs3CudaKernel(), "add");

  TF_ASSERT_OK(executor->GetKernel(spec, &add));

  int64_t length = 4;
  int64_t byte_length = sizeof(int32_t) * length;

  // Prepare arguments: a=1, b=2, c=0
  DeviceMemory<int32_t> a = executor->AllocateArray<int32_t>(length, 0);
  DeviceMemory<int32_t> b = executor->AllocateArray<int32_t>(length, 0);
  DeviceMemory<int32_t> c = executor->AllocateArray<int32_t>(length, 0);

  stream.ThenMemset32(&a, 1, byte_length);
  stream.ThenMemset32(&b, 2, byte_length);
  stream.ThenMemZero(&c, byte_length);

  // Use an array of device memory base pointers as argument to test packing.
  KernelArgsDeviceMemoryArray args({a, b, c}, 0);

  // Create a command buffer by tracing kernel launch operations.
  auto cmd_buffer = CommandBuffer::Trace(executor, [&](Stream* stream) {
    return executor->Launch(stream, ThreadDim(), BlockDim(4), add, args);
  });

  TF_ASSERT_OK(cmd_buffer.status());
  TF_ASSERT_OK(executor->Submit(&stream, *cmd_buffer));

  // Copy data back to host.
  std::vector<int32_t> dst(4, 42);
  stream.ThenMemcpy(dst.data(), c, byte_length);

  std::vector<int32_t> expected = {3, 3, 3, 3};
  ASSERT_EQ(dst, expected);
}

TEST(CudaCommandBufferTest, LaunchNestedCommandBuffer) {
  Platform* platform = MultiPlatformManager::PlatformWithName("CUDA").value();
  StreamExecutor* executor = platform->ExecutorForDevice(0).value();

  Stream stream(executor);
  stream.Init();
  ASSERT_TRUE(stream.ok());

  MultiKernelLoaderSpec spec(/*arity=*/3);
  spec.AddCudaPtxInMemory(internal::kAddI32Kernel, "add");

  AddI32Kernel add(executor);
  TF_ASSERT_OK(executor->GetKernel(spec, &add));

  int64_t length = 4;
  int64_t byte_length = sizeof(int32_t) * length;

  // Prepare arguments: a=1, b=2, c=0
  DeviceMemory<int32_t> a = executor->AllocateArray<int32_t>(length, 0);
  DeviceMemory<int32_t> b = executor->AllocateArray<int32_t>(length, 0);
  DeviceMemory<int32_t> c = executor->AllocateArray<int32_t>(length, 0);

  stream.ThenMemset32(&a, 1, byte_length);
  stream.ThenMemset32(&b, 2, byte_length);
  stream.ThenMemZero(&c, byte_length);

  // Create a command buffer with a single kernel launch.
  auto primary_cmd = CommandBuffer::Create(executor).value();
  auto nested_cmd = CommandBuffer::Create(executor, nested).value();
  TF_ASSERT_OK(nested_cmd.Launch(add, ThreadDim(), BlockDim(4), a, b, c));
  TF_ASSERT_OK(primary_cmd.AddNestedCommandBuffer(nested_cmd));
  TF_ASSERT_OK(primary_cmd.Finalize());

  TF_ASSERT_OK(executor->Submit(&stream, primary_cmd));

  // Copy `c` data back to host.
  std::vector<int32_t> dst(4, 42);
  stream.ThenMemcpy(dst.data(), c, byte_length);

  std::vector<int32_t> expected = {3, 3, 3, 3};
  ASSERT_EQ(dst, expected);

  // Prepare argument for graph update: d = 0
  DeviceMemory<int32_t> d = executor->AllocateArray<int32_t>(length, 0);
  stream.ThenMemZero(&d, byte_length);

  // Update command buffer to write into `d` buffer by creating a new nested
  // command buffer.
  nested_cmd = CommandBuffer::Create(executor, nested).value();
  TF_ASSERT_OK(nested_cmd.Launch(add, ThreadDim(), BlockDim(4), a, b, d));
  TF_ASSERT_OK(primary_cmd.Update());
  TF_ASSERT_OK(primary_cmd.AddNestedCommandBuffer(nested_cmd));
  TF_ASSERT_OK(primary_cmd.Finalize());

  TF_ASSERT_OK(executor->Submit(&stream, primary_cmd));

  // Copy `d` data back to host.
  std::fill(dst.begin(), dst.end(), 42);
  stream.ThenMemcpy(dst.data(), d, byte_length);
  ASSERT_EQ(dst, expected);
}

TEST(CudaCommandBufferTest, ConditionalIf) {
#if CUDA_VERSION < 12030
  GTEST_SKIP() << "CUDA graph conditionals are not supported";
#endif

  Platform* platform = MultiPlatformManager::PlatformWithName("CUDA").value();
  StreamExecutor* executor = platform->ExecutorForDevice(0).value();

  Stream stream(executor);
  stream.Init();
  ASSERT_TRUE(stream.ok());

  AddI32Kernel add(executor);

  {  // Load addition kernel.
    MultiKernelLoaderSpec spec(/*arity=*/3);
    spec.AddInProcessSymbol(internal::GetAddI32CudaKernel(), "add");
    TF_ASSERT_OK(executor->GetKernel(spec, &add));
  }

  int64_t length = 4;
  int64_t byte_length = sizeof(int32_t) * length;

  // Prepare arguments: a=1, b=2, c=0, pred=true
  DeviceMemory<bool> pred = executor->AllocateArray<bool>(1, 0);
  DeviceMemory<int32_t> a = executor->AllocateArray<int32_t>(length, 0);
  DeviceMemory<int32_t> b = executor->AllocateArray<int32_t>(length, 0);
  DeviceMemory<int32_t> c = executor->AllocateArray<int32_t>(length, 0);

  constexpr bool kTrue = true;
  stream.ThenMemcpy(&pred, &kTrue, 1);
  stream.ThenMemset32(&a, 1, byte_length);
  stream.ThenMemset32(&b, 2, byte_length);
  stream.ThenMemZero(&c, byte_length);

  // if (pred == true) c = a + b
  CommandBuffer::Builder then_builder = [&](CommandBuffer* then_cmd) {
    return then_cmd->Launch(add, ThreadDim(), BlockDim(4), a, b, c);
  };

  // Create a command buffer with a single conditional operation.
  auto cmd_buffer = CommandBuffer::Create(executor).value();
  TF_ASSERT_OK(cmd_buffer.If(pred, then_builder));
  TF_ASSERT_OK(cmd_buffer.Finalize());

  TF_ASSERT_OK(executor->Submit(&stream, cmd_buffer));

  // Copy `c` data back to host.
  std::vector<int32_t> dst(4, 42);
  stream.ThenMemcpy(dst.data(), c, byte_length);

  std::vector<int32_t> expected = {3, 3, 3, 3};
  ASSERT_EQ(dst, expected);

  // Reset predicate to false and clear output buffer.
  constexpr bool kFalse = false;
  stream.ThenMemcpy(&pred, &kFalse, 1);
  stream.ThenMemZero(&c, byte_length);

  // Submit the same command buffer, but this time it should not execute
  // conditional branch as conditional handle should be updated to false.
  TF_ASSERT_OK(executor->Submit(&stream, cmd_buffer));

  stream.ThenMemcpy(dst.data(), c, byte_length);
  std::vector<int32_t> zeroes = {0, 0, 0, 0};
  ASSERT_EQ(dst, zeroes);

  // TODO(ezhulenev): Test conditional command buffer updates.
}

//===----------------------------------------------------------------------===//
// Performance benchmarks below
//===----------------------------------------------------------------------===//

#define BENCHMARK_SIZES(NAME) \
  BENCHMARK(NAME)->Arg(8)->Arg(32)->Arg(128)->Arg(512)->Arg(1024);

// In benchmarks we construct command buffers in nested mode when we
// do not want to measure graph executable instantiation overhead.
static void BM_CreateCommandBuffer(benchmark::State& state) {
  Platform* platform = MultiPlatformManager::PlatformWithName("CUDA").value();
  StreamExecutor* executor = platform->ExecutorForDevice(0).value();

  MultiKernelLoaderSpec spec(/*arity=*/3);
  spec.AddCudaPtxInMemory(internal::kAddI32Kernel, "add");

  AddI32Kernel add(executor);
  CHECK_OK(executor->GetKernel(spec, &add));

  DeviceMemory<int32_t> b = executor->AllocateArray<int32_t>(1, 0);

  for (auto s : state) {
    auto cmd_buffer = CommandBuffer::Create(executor, nested).value();
    for (int i = 1; i < state.range(0); ++i) {
      CHECK_OK(cmd_buffer.Launch(add, ThreadDim(), BlockDim(4), b, b, b));
    }
    CHECK_OK(cmd_buffer.Finalize());
  }
}

BENCHMARK_SIZES(BM_CreateCommandBuffer);

static void BM_TraceCommandBuffer(benchmark::State& state) {
  Platform* platform = MultiPlatformManager::PlatformWithName("CUDA").value();
  StreamExecutor* executor = platform->ExecutorForDevice(0).value();

  Stream stream(executor);
  stream.Init();
  CHECK(stream.ok());

  MultiKernelLoaderSpec spec(/*arity=*/3);
  spec.AddCudaPtxInMemory(internal::kAddI32Kernel, "add");

  AddI32Kernel add(executor);
  CHECK_OK(executor->GetKernel(spec, &add));

  DeviceMemory<int32_t> b = executor->AllocateArray<int32_t>(1, 0);

  for (auto s : state) {
    auto launch_kernels = [&](Stream* stream) {
      for (int i = 1; i < state.range(0); ++i) {
        CHECK_OK(stream->ThenLaunch(ThreadDim(), BlockDim(4), add, b, b, b));
      }
      return tsl::OkStatus();
    };

    CHECK_OK(CommandBuffer::Trace(executor, launch_kernels, nested));
  }
}

BENCHMARK_SIZES(BM_TraceCommandBuffer);

static void BM_UpdateCommandBuffer(benchmark::State& state) {
  Platform* platform = MultiPlatformManager::PlatformWithName("CUDA").value();
  StreamExecutor* executor = platform->ExecutorForDevice(0).value();

  MultiKernelLoaderSpec spec(/*arity=*/3);
  spec.AddCudaPtxInMemory(internal::kAddI32Kernel, "add");

  AddI32Kernel add(executor);
  CHECK_OK(executor->GetKernel(spec, &add));

  DeviceMemory<int32_t> b = executor->AllocateArray<int32_t>(1, 0);

  auto cmd_buffer = CommandBuffer::Create(executor, primary).value();
  for (int i = 1; i < state.range(0); ++i) {
    CHECK_OK(cmd_buffer.Launch(add, ThreadDim(), BlockDim(4), b, b, b));
  }
  CHECK_OK(cmd_buffer.Finalize());

  for (auto s : state) {
    CHECK_OK(cmd_buffer.Update());
    for (int i = 1; i < state.range(0); ++i) {
      CHECK_OK(cmd_buffer.Launch(add, ThreadDim(), BlockDim(4), b, b, b));
    }
    CHECK_OK(cmd_buffer.Finalize());
  }
}

BENCHMARK_SIZES(BM_UpdateCommandBuffer);

}  // namespace stream_executor::cuda
