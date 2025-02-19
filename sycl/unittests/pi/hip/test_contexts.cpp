//==---- test_contexts.cpp --- PI unit tests -------------------------------==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include <condition_variable>
#include <mutex>
#include <thread>

#include <hip/hip_runtime.h>

#include "HipUtils.hpp"
#include "TestGetPlugin.hpp"
#include <detail/plugin.hpp>
#include <pi_hip.hpp>
#include <sycl/detail/pi.hpp>
#include <sycl/sycl.hpp>

using namespace sycl;

struct HipContextsTest : public ::testing::Test {

protected:
  std::optional<detail::PluginPtr> &plugin =
      pi::initializeAndGet(backend::ext_oneapi_hip);

  pi_platform platform_;
  pi_device device_;

  void SetUp() override {
    // skip the tests if the HIP backend is not available
    if (!plugin.has_value()) {
      GTEST_SKIP();
    }

    pi_uint32 numPlatforms = 0;
    ASSERT_EQ(plugin->hasBackend(backend::ext_oneapi_hip), PI_SUCCESS);

    ASSERT_EQ((plugin->call_nocheck<detail::PiApiKind::piPlatformsGet>(
                  0, nullptr, &numPlatforms)),
              PI_SUCCESS)
        << "piPlatformsGet failed.\n";

    ASSERT_EQ((plugin->call_nocheck<detail::PiApiKind::piPlatformsGet>(
                  numPlatforms, &platform_, nullptr)),
              PI_SUCCESS)
        << "piPlatformsGet failed.\n";

    ASSERT_EQ((plugin->call_nocheck<detail::PiApiKind::piDevicesGet>(
                  platform_, PI_DEVICE_TYPE_GPU, 1, &device_, nullptr)),
              PI_SUCCESS);
  }

  void TearDown() override {}

  HipContextsTest() = default;

  ~HipContextsTest() = default;
};

TEST_F(HipContextsTest, ContextLifetime) {
  // start with no active context
  pi::clearHipContext();

  // create a context
  pi_context context;
  ASSERT_EQ((plugin->call_nocheck<detail::PiApiKind::piContextCreate>(
                nullptr, 1, &device_, nullptr, nullptr, &context)),
            PI_SUCCESS);
  ASSERT_NE(context, nullptr);

  // create a queue from the context, this should use the ScopedContext
  pi_queue queue;
  ASSERT_EQ((plugin->call_nocheck<detail::PiApiKind::piQueueCreate>(
                context, device_, 0, &queue)),
            PI_SUCCESS);
  ASSERT_NE(queue, nullptr);

  // ensure the queue has the correct context
  ASSERT_EQ(context, queue->get_context());

  // check that the context is now the active HIP context
  hipCtx_t hipCtxt = nullptr;
  hipCtxGetCurrent(&hipCtxt);
  ASSERT_EQ(hipCtxt, context->get());

  plugin->call<detail::PiApiKind::piQueueRelease>(queue);
  plugin->call<detail::PiApiKind::piContextRelease>(context);

  // check that the context was cleaned up properly by the destructor
  hipCtxGetCurrent(&hipCtxt);
  ASSERT_EQ(hipCtxt, nullptr);
}

TEST_F(HipContextsTest, ContextLifetimeExisting) {
  // start by setting up a HIP context on the thread
  hipCtx_t original;
  hipCtxCreate(&original, hipDeviceMapHost, device_->get());

  // ensure the HIP context is active
  hipCtx_t current = nullptr;
  hipCtxGetCurrent(&current);
  ASSERT_EQ(original, current);

  // create a PI context
  pi_context context;
  ASSERT_EQ((plugin->call_nocheck<detail::PiApiKind::piContextCreate>(
                nullptr, 1, &device_, nullptr, nullptr, &context)),
            PI_SUCCESS);
  ASSERT_NE(context, nullptr);

  // create a queue from the context, this should use the ScopedContext
  pi_queue queue;
  ASSERT_EQ((plugin->call_nocheck<detail::PiApiKind::piQueueCreate>(
                context, device_, 0, &queue)),
            PI_SUCCESS);
  ASSERT_NE(queue, nullptr);

  // ensure the queue has the correct context
  ASSERT_EQ(context, queue->get_context());

  // check that the context is now the active HIP context
  hipCtxGetCurrent(&current);
  ASSERT_EQ(current, context->get());

  plugin->call<detail::PiApiKind::piQueueRelease>(queue);
  plugin->call<detail::PiApiKind::piContextRelease>(context);

  // check that the context was cleaned up, the old context will be restored
  // automatically by hipCtxDestroy in piContextRelease, as it was pushed on the
  // stack bu hipCtxCreate
  hipCtxGetCurrent(&current);
  ASSERT_EQ(current, original);

  // release original context
  hipCtxDestroy(original);
}

// In some cases (for host_task), the SYCL runtime may call PI API functions
// from threads of the thread pool, this can cause issues because with the HIP
// plugin these functions will set an active HIP context on these threads, but
// never clean it up, as it will only get cleaned up in the main thread.
//
// So the following test aims to reproduce the scenario where there is a
// dangling deleted context in a separate thread and seeing if the PI calls are
// still able to work correctly in that thread.
TEST_F(HipContextsTest, ContextThread) {
  // start with no active context
  pi::clearHipContext();

  // create two PI contexts
  pi_context context1;
  ASSERT_EQ((plugin->call_nocheck<detail::PiApiKind::piContextCreate>(
                nullptr, 1, &device_, nullptr, nullptr, &context1)),
            PI_SUCCESS);
  ASSERT_NE(context1, nullptr);

  pi_context context2;
  ASSERT_EQ((plugin->call_nocheck<detail::PiApiKind::piContextCreate>(
                nullptr, 1, &device_, nullptr, nullptr, &context2)),
            PI_SUCCESS);
  ASSERT_NE(context2, nullptr);

  // setup synchronization variables between the main thread and the testing
  // thread
  std::mutex m;
  std::condition_variable cv;
  bool released = false;
  bool thread_done = false;

  // create a testing thread that will create a queue with the first context,
  // release the queue, then wait for the main thread to release the first
  // context, and then create and release another queue with the second context
  // this time
  auto test_thread = std::thread([&] {
    hipCtx_t current = nullptr;

    // create a queue with the first context
    pi_queue queue;
    ASSERT_EQ((plugin->call_nocheck<detail::PiApiKind::piQueueCreate>(
                  context1, device_, 0, &queue)),
              PI_SUCCESS);
    ASSERT_NE(queue, nullptr);

    // ensure the queue has the correct context
    ASSERT_EQ(context1, queue->get_context());

    // check that the first context is now the active HIP context
    hipCtxGetCurrent(&current);
    ASSERT_EQ(current, context1->get());

    plugin->call<detail::PiApiKind::piQueueRelease>(queue);

    // mark the first set of processing as done and notify the main thread
    std::unique_lock<std::mutex> lock(m);
    thread_done = true;
    lock.unlock();
    cv.notify_one();

    // wait for the main thread to release the first context
    lock.lock();
    cv.wait(lock, [&] { return released; });

    // check that the first context is still active, this is because deleting a
    // context only cleans up the current thread
    hipCtxGetCurrent(&current);
    ASSERT_EQ(current, context1->get());

    // create a queue with the second context
    ASSERT_EQ((plugin->call_nocheck<detail::PiApiKind::piQueueCreate>(
                  context2, device_, 0, &queue)),
              PI_SUCCESS);
    ASSERT_NE(queue, nullptr);

    // ensure the queue has the correct context
    ASSERT_EQ(context2, queue->get_context());

    // check that the second context is now the active HIP context
    hipCtxGetCurrent(&current);
    ASSERT_EQ(current, context2->get());

    plugin->call<detail::PiApiKind::piQueueRelease>(queue);
  });

  // wait for the thread to be done with the first queue to release the first
  // context
  std::unique_lock<std::mutex> lock(m);
  cv.wait(lock, [&] { return thread_done; });
  plugin->call<detail::PiApiKind::piContextRelease>(context1);

  // notify the other thread that the context was released
  released = true;
  lock.unlock();
  cv.notify_one();

  // wait for the thread to finish
  test_thread.join();

  plugin->call<detail::PiApiKind::piContextRelease>(context2);

  // check that there is no context set on the main thread
  hipCtx_t current = nullptr;
  hipCtxGetCurrent(&current);
  ASSERT_EQ(current, nullptr);
}
