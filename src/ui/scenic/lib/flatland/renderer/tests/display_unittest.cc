// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-testing/test_loop.h>
#include <lib/async/cpp/executor.h>
#include <lib/async/cpp/wait.h>
#include <lib/async/default.h>
#include <lib/fdio/directory.h>

#include <thread>

#include "src/lib/fsl/handles/object_info.h"
#include "src/lib/testing/loop_fixture/real_loop_fixture.h"
#include "src/ui/lib/display/get_hardware_display_controller.h"
#include "src/ui/lib/escher/vk/vulkan_device_queues.h"
#include "src/ui/scenic/lib/display/display_manager.h"
#include "src/ui/scenic/lib/display/util.h"
#include "src/ui/scenic/lib/flatland/renderer/null_renderer.h"
#include "src/ui/scenic/lib/flatland/renderer/tests/common.h"
#include "src/ui/scenic/lib/flatland/renderer/vk_renderer.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtx/matrix_transform_2d.hpp>

using namespace scenic_impl;
using namespace display;

class DisplayTest : public gtest::RealLoopFixture {
 protected:
  void SetUp() override {
    if (VK_TESTS_SUPPRESSED()) {
      return;
    }
    gtest::RealLoopFixture::SetUp();

    // Create the SysmemAllocator.
    zx_status_t status = fdio_service_connect(
        "/svc/fuchsia.sysmem.Allocator", sysmem_allocator_.NewRequest().TakeChannel().release());
    sysmem_allocator_->SetDebugClientInfo(fsl::GetCurrentProcessName(),
                                          fsl::GetCurrentProcessKoid());

    async_set_default_dispatcher(dispatcher());
    executor_ = std::make_unique<async::Executor>(dispatcher());

    display_manager_ = std::make_unique<display::DisplayManager>([]() {});

    auto hdc_promise = ui_display::GetHardwareDisplayController();
    executor_->schedule_task(
        hdc_promise.then([this](fit::result<ui_display::DisplayControllerHandles>& handles) {
          display_manager_->BindDefaultDisplayController(std::move(handles.value().controller),
                                                         std::move(handles.value().dc_device));
        }));

    RunLoopUntil([this] { return display_manager_->default_display() != nullptr; });
  }

  void TearDown() override {
    if (VK_TESTS_SUPPRESSED()) {
      return;
    }
    executor_.reset();
    display_manager_.reset();
    sysmem_allocator_ = nullptr;
    gtest::RealLoopFixture::TearDown();
  }

  uint64_t InitializeDisplayLayer(fuchsia::hardware::display::ControllerSyncPtr& display_controller,
                                  Display* display) {
    uint64_t layer_id;
    zx_status_t create_layer_status;
    zx_status_t transport_status = display_controller->CreateLayer(&create_layer_status, &layer_id);
    if (create_layer_status != ZX_OK || transport_status != ZX_OK) {
      FX_LOGS(ERROR) << "Failed to create layer, " << create_layer_status;
      return 0;
    }

    zx_status_t status = display_controller->SetDisplayLayers(display->display_id(), {layer_id});
    if (status != ZX_OK) {
      FX_LOGS(ERROR) << "Failed to configure display layers. Error code: " << status;
      return 0;
    }

    return layer_id;
  }

  std::unique_ptr<async::Executor> executor_;
  std::unique_ptr<display::DisplayManager> display_manager_;
  fuchsia::sysmem::AllocatorSyncPtr sysmem_allocator_;
};

// Create a buffer collection and set constraints on the display, the vulkan renderer
// and the client, and make sure that the collection is still properly allocated.
VK_TEST_F(DisplayTest, SetAllConstraintsTest) {
  const uint64_t kWidth = 32;
  const uint64_t kHeight = 64;

  // Grab the display controller.
  auto display_controller = display_manager_->default_display_controller();
  EXPECT_TRUE(display_controller);

  // Create the VK renderer.
  auto env = escher::test::EscherEnvironment::GetGlobalTestEnvironment();
  auto unique_escher = std::make_unique<escher::Escher>(
      env->GetVulkanDevice(), env->GetFilesystem(), /*gpu_allocator*/ nullptr);
  flatland::VkRenderer renderer(unique_escher->GetWeakPtr());

  // First create the pair of sysmem tokens, one for the client, one for the renderer.
  auto tokens = flatland::SysmemTokens::Create(sysmem_allocator_.get());

  fuchsia::sysmem::BufferCollectionTokenSyncPtr display_token;
  zx_status_t status = tokens.local_token->Duplicate(std::numeric_limits<uint32_t>::max(),
                                                     display_token.NewRequest());
  FX_DCHECK(status == ZX_OK);

  // Register the collection with the renderer, which sets the vk constraints.
  auto renderer_collection_id = sysmem_util::GenerateUniqueBufferCollectionId();
  auto result = renderer.ImportBufferCollection(renderer_collection_id, sysmem_allocator_.get(),
                                                std::move(tokens.dup_token));
  EXPECT_TRUE(result);

  // Importing an image should fail at this point because we've only set the renderer constraints.
  auto import_result = renderer.ImportImage(
      {.collection_id = renderer_collection_id, .vmo_index = 0, .width = kWidth, .height = kHeight});
  EXPECT_FALSE(import_result);

  // Set the display constraints on the display controller.
  fuchsia::hardware::display::ImageConfig display_constraints = {
      .pixel_format = ZX_PIXEL_FORMAT_RGB_x888,
  };
  bool res = scenic_impl::ImportBufferCollection(renderer_collection_id, *display_controller.get(),
                                                 std::move(display_token), display_constraints);
  ASSERT_TRUE(res);

  // Importing should fail again, because we've only set 2 of the 3 constraints.
  import_result = renderer.ImportImage(
      {.collection_id = renderer_collection_id, .vmo_index = 0, .width = kWidth, .height = kHeight});
  EXPECT_FALSE(import_result);

  // Create a client-side handle to the buffer collection and set the client constraints.
  auto client_collection = flatland::CreateClientPointerWithConstraints(
      sysmem_allocator_.get(), std::move(tokens.local_token),
      /*image_count*/ 1,
      /*width*/ kWidth,
      /*height*/ kHeight);

  // Have the client wait for buffers allocated so it can populate its information
  // struct with the vmo data.
  fuchsia::sysmem::BufferCollectionInfo_2 client_collection_info = {};
  {
    zx_status_t allocation_status = ZX_OK;
    auto status =
        client_collection->WaitForBuffersAllocated(&allocation_status, &client_collection_info);
    EXPECT_EQ(status, ZX_OK);
    EXPECT_EQ(allocation_status, ZX_OK);
  }

  // Now that the renderer, client, and the display have set their constraints, we import one last
  // time and this time it should return true.
  import_result = renderer.ImportImage(
      {.collection_id = renderer_collection_id, .vmo_index = 0, .width = kWidth, .height = kHeight});
  EXPECT_TRUE(import_result);

  // We should now be able to also import an image to the display controller, using the
  // display-specific buffer collection id. If it returns OK, then we know that the renderer
  // did fully set the DC constraints.
  uint64_t image_id;
  fuchsia::hardware::display::ImageConfig image_config = {
      .width = kWidth,
      .height = kHeight,
      .pixel_format = ZX_PIXEL_FORMAT_RGB_x888,
  };

  zx_status_t import_image_status = ZX_OK;
  (*display_controller.get())
      ->ImportImage(image_config, renderer_collection_id, /*vmo_index*/ 0, &import_image_status,
                    &image_id);
  EXPECT_EQ(import_image_status, ZX_OK);
  EXPECT_NE(image_id, 0u);
}

// Test out event signaling on the Display Controller by importing a buffer collection and its 2
// images, setting the first image to a display layer with a signal event, and
// then setting the second image on the layer which has a wait event. When the wait event is
// signaled, this will cause the second layer image to go up, which in turn will cause the first
// layer image's event to be signaled.
// TODO(fxbug.dev/55167): Check to see if there is a more appropriate place to test display
// controller events and/or if there already exist adequate tests that cover all of the use cases
// being covered by this test.
VK_TEST_F(DisplayTest, SetDisplayImageTest) {
  // Grab the display controller.
  auto display_controller = display_manager_->default_display_controller();
  ASSERT_TRUE(display_controller);

  auto display = display_manager_->default_display();
  ASSERT_TRUE(display);

  auto layer_id = InitializeDisplayLayer(*display_controller.get(), display);
  ASSERT_NE(layer_id, 0U);

  const uint32_t kWidth = display->width_in_px();
  const uint32_t kHeight = display->height_in_px();
  const uint32_t kNumVmos = 2;

  // First create the pair of sysmem tokens, one for the client, one for the display.
  auto tokens = flatland::SysmemTokens::Create(sysmem_allocator_.get());

  // Set the display constraints on the display controller.
  fuchsia::hardware::display::ImageConfig image_config = {
      .width = kWidth,
      .height = kHeight,
      .pixel_format = ZX_PIXEL_FORMAT_RGB_x888,
  };
  auto display_collection_id = sysmem_util::GenerateUniqueBufferCollectionId();
  ASSERT_NE(display_collection_id, 0U);

  bool res = scenic_impl::ImportBufferCollection(display_collection_id, *display_controller.get(),
                                                 std::move(tokens.dup_token), image_config);
  ASSERT_TRUE(res);

  flatland::SetClientConstraintsAndWaitForAllocated(
      sysmem_allocator_.get(), std::move(tokens.local_token), kNumVmos, kWidth, kHeight);

  // Import the images to the display.
  uint64_t image_ids[kNumVmos];
  for (uint64_t i = 0; i < kNumVmos; i++) {
    zx_status_t import_image_status = ZX_OK;
    auto transport_status = (*display_controller.get())
                                ->ImportImage(image_config, display_collection_id, i,
                                              &import_image_status, &image_ids[i]);
    ASSERT_EQ(transport_status, ZX_OK);
    ASSERT_EQ(import_image_status, ZX_OK);
    ASSERT_NE(image_ids[i], fuchsia::hardware::display::INVALID_DISP_ID);
  }

  // Create the events used by the display.
  zx::event display_wait_fence, display_signal_fence;
  auto status = zx::event::create(0, &display_wait_fence);
  status |= zx::event::create(0, &display_signal_fence);
  EXPECT_EQ(status, ZX_OK);

  // Import the above events to the display.
  auto display_wait_event_id =
      scenic_impl::ImportEvent(*display_controller.get(), display_wait_fence);
  auto display_signal_event_id =
      scenic_impl::ImportEvent(*display_controller.get(), display_signal_fence);
  EXPECT_NE(display_wait_event_id, fuchsia::hardware::display::INVALID_DISP_ID);
  EXPECT_NE(display_signal_event_id, fuchsia::hardware::display::INVALID_DISP_ID);
  EXPECT_NE(display_wait_event_id, display_signal_event_id);

  // Set the layer image and apply the config.
  (*display_controller.get())->SetLayerPrimaryConfig(layer_id, image_config);

  status = (*display_controller.get())
               ->SetLayerImage(layer_id, image_ids[0], 0, display_signal_event_id);
  EXPECT_EQ(status, ZX_OK);

  // Apply the config.
  fuchsia::hardware::display::ConfigResult result;
  std::vector<fuchsia::hardware::display::ClientCompositionOp> ops;
  (*display_controller.get())->CheckConfig(/*discard=*/false, &result, &ops);
  EXPECT_EQ(result, fuchsia::hardware::display::ConfigResult::OK);
  status = (*display_controller.get())->ApplyConfig();
  EXPECT_EQ(status, ZX_OK);

  // Attempt to wait here...this should time out because the event has not yet been signaled.
  status =
      display_signal_fence.wait_one(ZX_EVENT_SIGNALED, zx::deadline_after(zx::msec(3000)), nullptr);
  EXPECT_EQ(status, ZX_ERR_TIMED_OUT);

  // Set the layer image again, to the second image, so that our first call to SetLayerImage()
  // above will signal.
  status =
      (*display_controller.get())->SetLayerImage(layer_id, image_ids[1], display_wait_event_id, 0);
  EXPECT_EQ(status, ZX_OK);

  // Apply the config to display the second image.
  (*display_controller.get())->CheckConfig(/*discard=*/false, &result, &ops);
  EXPECT_EQ(result, fuchsia::hardware::display::ConfigResult::OK);
  status = (*display_controller.get())->ApplyConfig();
  EXPECT_EQ(status, ZX_OK);

  // Attempt to wait again, this should also time out because we haven't signaled our wait fence.
  status =
      display_signal_fence.wait_one(ZX_EVENT_SIGNALED, zx::deadline_after(zx::msec(3000)), nullptr);
  EXPECT_EQ(status, ZX_ERR_TIMED_OUT);

  // Now we signal wait on the second layer.
  display_wait_fence.signal(0, ZX_EVENT_SIGNALED);

  // Now we wait for the display to signal again, and this time it should go through.
  status =
      display_signal_fence.wait_one(ZX_EVENT_SIGNALED, zx::deadline_after(zx::msec(3000)), nullptr);
  EXPECT_EQ(status, ZX_OK);
}
