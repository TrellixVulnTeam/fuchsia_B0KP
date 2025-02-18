// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-testing/test_loop.h>
#include <lib/async/cpp/wait.h>
#include <lib/async/default.h>
#include <lib/fdio/directory.h>

#include <cstdint>
#include <thread>

#include "src/lib/fsl/handles/object_info.h"
#include "src/ui/scenic/lib/flatland/buffers/util.h"
#include "src/ui/scenic/lib/flatland/renderer/null_renderer.h"
#include "src/ui/scenic/lib/flatland/renderer/tests/common.h"
#include "src/ui/scenic/lib/flatland/renderer/vk_renderer.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtx/matrix_transform_2d.hpp>

namespace flatland {

using NullRendererTest = RendererTest;
using VulkanRendererTest = RendererTest;

namespace {
static constexpr float kDegreesToRadians = glm::pi<float>() / 180.f;

glm::ivec4 GetPixel(uint8_t* vmo_host, uint32_t width, uint32_t x, uint32_t y) {
  uint32_t r = vmo_host[y * width * 4 + x * 4];
  uint32_t g = vmo_host[y * width * 4 + x * 4 + 1];
  uint32_t b = vmo_host[y * width * 4 + x * 4 + 2];
  uint32_t a = vmo_host[y * width * 4 + x * 4 + 3];
  return glm::ivec4(r, g, b, a);
};

}  // anonymous namespace

// Make sure a valid token can be used to register a buffer collection.
void RegisterCollectionTest(Renderer* renderer, fuchsia::sysmem::Allocator_Sync* sysmem_allocator) {
  auto tokens = SysmemTokens::Create(sysmem_allocator);
  auto tokens2 = SysmemTokens::Create(sysmem_allocator);

  // First id should be valid.
  auto bcid = sysmem_util::GenerateUniqueBufferCollectionId();
  auto result = renderer->RegisterRenderTargetCollection(bcid, sysmem_allocator,
                                                         std::move(tokens.local_token));
  EXPECT_TRUE(result);
}

// Multiple clients may need to reference the same buffer collection in the renderer
// (for example if they both need access to a global camera feed). In this case, both
// clients will be passing their own duped tokens to the same collection to the renderer,
// and will each get back a different ID. The collection itself (which is just a pointer)
// will be in the renderer's map twice. So if all tokens are set, both server-side
// registered collections should be allocated (since they are just pointers that refer
// to the same collection).
void SameTokenTwiceTest(Renderer* renderer, fuchsia::sysmem::Allocator_Sync* sysmem_allocator) {
  auto tokens = flatland::SysmemTokens::Create(sysmem_allocator);

  // Create a client token to represent a single client.
  fuchsia::sysmem::BufferCollectionTokenSyncPtr client_token;
  auto status = tokens.local_token->Duplicate(std::numeric_limits<uint32_t>::max(),
                                              client_token.NewRequest());
  EXPECT_EQ(status, ZX_OK);

  // First id should be valid.
  auto bcid = sysmem_util::GenerateUniqueBufferCollectionId();
  auto result = renderer->RegisterRenderTargetCollection(bcid, sysmem_allocator,
                                                         std::move(tokens.local_token));
  EXPECT_TRUE(result);

  // Second id should be valid.
  auto bcid2 = sysmem_util::GenerateUniqueBufferCollectionId();
  result = renderer->RegisterRenderTargetCollection(bcid2, sysmem_allocator,
                                                    std::move(tokens.dup_token));
  EXPECT_TRUE(result);

  // Set the client constraints.
  std::vector<uint64_t> additional_format_modifiers;
  if (escher::VulkanIsSupported() && escher::test::GlobalEscherUsesVirtualGpu()) {
    additional_format_modifiers.push_back(fuchsia::sysmem::FORMAT_MODIFIER_GOOGLE_GOLDFISH_OPTIMAL);
  }
  SetClientConstraintsAndWaitForAllocated(sysmem_allocator, std::move(client_token),
                                          /* image_count */ 1, /* width */ 64, /* height */ 32,
                                          kNoneUsage, additional_format_modifiers);

  // Now check that both server ids are allocated.
  bool res_1 = renderer->ImportImage({.collection_id = bcid,
                                      .identifier = sysmem_util::GenerateUniqueImageId(),
                                      .vmo_index = 0,
                                      .width = 1,
                                      .height = 1});
  bool res_2 = renderer->ImportImage({.collection_id = bcid2,
                                      .identifier = sysmem_util::GenerateUniqueImageId(),
                                      .vmo_index = 0,
                                      .width = 1,
                                      .height = 1});
  EXPECT_TRUE(res_1);
  EXPECT_TRUE(res_2);
}

// Make sure a bad token returns Renderer::sysmem_util::kInvalidId. A "bad token" here can
// either be a null token, or a token that's a valid channel but just not a
// valid buffer collection token.
void BadTokenTest(Renderer* renderer, fuchsia::sysmem::Allocator_Sync* sysmem_allocator) {
  // Null token should fail.
  auto bcid = sysmem_util::GenerateUniqueBufferCollectionId();
  auto result = renderer->RegisterRenderTargetCollection(bcid, sysmem_allocator, nullptr);
  EXPECT_FALSE(result);

  // A valid channel that isn't a buffer collection should also fail.
  zx::channel local_endpoint;
  zx::channel remote_endpoint;
  zx::channel::create(0, &local_endpoint, &remote_endpoint);
  flatland::BufferCollectionHandle handle{std::move(remote_endpoint)};
  ASSERT_TRUE(handle.is_valid());

  bcid = sysmem_util::GenerateUniqueBufferCollectionId();
  result = renderer->RegisterRenderTargetCollection(bcid, sysmem_allocator, std::move(handle));
  EXPECT_FALSE(result);
}

// Test the ImportImage() function. First call ImportImage() without setting the client
// constraints, which should return false, and then set the client constraints which
// should cause it to return true.
void ImportImageTest(Renderer* renderer, fuchsia::sysmem::Allocator_Sync* sysmem_allocator) {
  auto tokens = SysmemTokens::Create(sysmem_allocator);

  auto bcid = sysmem_util::GenerateUniqueBufferCollectionId();
  auto result =
      renderer->RegisterRenderTargetCollection(bcid, sysmem_allocator, std::move(tokens.dup_token));
  EXPECT_TRUE(result);

  // The buffer collection should not be valid here.
  auto image_id = sysmem_util::GenerateUniqueImageId();
  EXPECT_FALSE(renderer->ImportImage(
      {.collection_id = bcid, .identifier = image_id, .vmo_index = 0, .width = 1, .height = 1}));

  std::vector<uint64_t> additional_format_modifiers;
  if (escher::VulkanIsSupported() && escher::test::GlobalEscherUsesVirtualGpu()) {
    additional_format_modifiers.push_back(fuchsia::sysmem::FORMAT_MODIFIER_GOOGLE_GOLDFISH_OPTIMAL);
  }
  SetClientConstraintsAndWaitForAllocated(sysmem_allocator, std::move(tokens.local_token),
                                          /* image_count */ 1, /* width */ 64, /* height */ 32,
                                          kNoneUsage, additional_format_modifiers);

  // The buffer collection *should* be valid here.
  auto res = renderer->ImportImage(
      {.collection_id = bcid, .identifier = image_id, .vmo_index = 0, .width = 1, .height = 1});
  EXPECT_TRUE(res);
}

// Simple deregistration test that calls ReleaseBufferCollection() directly without
// any zx::events just to make sure that the method's functionality itself is
// working as intented.
void DeregistrationTest(Renderer* renderer, fuchsia::sysmem::Allocator_Sync* sysmem_allocator) {
  auto tokens = SysmemTokens::Create(sysmem_allocator);

  auto bcid = sysmem_util::GenerateUniqueBufferCollectionId();
  auto result =
      renderer->RegisterRenderTargetCollection(bcid, sysmem_allocator, std::move(tokens.dup_token));
  EXPECT_TRUE(result);

  // The buffer collection should not be valid here.
  auto image_id = sysmem_util::GenerateUniqueImageId();
  EXPECT_FALSE(renderer->ImportImage(
      {.collection_id = bcid, .identifier = image_id, .vmo_index = 0, .width = 1, .height = 1}));

  std::vector<uint64_t> additional_format_modifiers;
  if (escher::VulkanIsSupported() && escher::test::GlobalEscherUsesVirtualGpu()) {
    additional_format_modifiers.push_back(fuchsia::sysmem::FORMAT_MODIFIER_GOOGLE_GOLDFISH_OPTIMAL);
  }
  SetClientConstraintsAndWaitForAllocated(sysmem_allocator, std::move(tokens.local_token),
                                          /* image_count */ 1, /* width */ 64, /* height */ 32,
                                          kNoneUsage, additional_format_modifiers);

  // The buffer collection *should* be valid here.
  auto import_result = renderer->ImportImage(
      {.collection_id = bcid, .identifier = image_id, .vmo_index = 0, .width = 1, .height = 1});
  EXPECT_TRUE(import_result);

  // Now deregister the collection.
  renderer->DeregisterRenderTargetCollection(bcid);

  // After deregistration, calling ImportImage() should return false.
  import_result = renderer->ImportImage(
      {.collection_id = bcid, .identifier = image_id, .vmo_index = 0, .width = 1, .height = 1});
  EXPECT_FALSE(import_result);
}

// Test to make sure we can call the functions RegisterTextureCollection(),
// RegisterRenderTargetCollection() and ImportIMage() simultaneously from
// multiple threads and have it work.
void MultithreadingTest(Renderer* renderer) {
  const uint32_t kNumThreads = 50;

  std::set<sysmem_util::GlobalBufferCollectionId> bcid_set;
  std::mutex lock;

  auto register_and_import_function = [&renderer, &bcid_set, &lock]() {
    // Make a test loop.
    async::TestLoop loop;

    // Make an extra sysmem allocator for tokens.
    fuchsia::sysmem::AllocatorSyncPtr sysmem_allocator;
    zx_status_t status = fdio_service_connect(
        "/svc/fuchsia.sysmem.Allocator", sysmem_allocator.NewRequest().TakeChannel().release());
    sysmem_allocator->SetDebugClientInfo(fsl::GetCurrentProcessName(),
                                         fsl::GetCurrentProcessKoid());

    auto tokens = SysmemTokens::Create(sysmem_allocator.get());
    auto bcid = sysmem_util::GenerateUniqueBufferCollectionId();
    auto image_id = sysmem_util::GenerateUniqueImageId();
    bool result = renderer->RegisterRenderTargetCollection(bcid, sysmem_allocator.get(),
                                                           std::move(tokens.local_token));
    EXPECT_TRUE(result);

    std::vector<uint64_t> additional_format_modifiers;
    if (escher::VulkanIsSupported() && escher::test::GlobalEscherUsesVirtualGpu()) {
      additional_format_modifiers.push_back(
          fuchsia::sysmem::FORMAT_MODIFIER_GOOGLE_GOLDFISH_OPTIMAL);
    }
    SetClientConstraintsAndWaitForAllocated(sysmem_allocator.get(), std::move(tokens.local_token),
                                            /* image_count */ 1, /* width */ 64, /* height */ 32,
                                            kNoneUsage, additional_format_modifiers);

    // Add the bcid to the global vector in a thread-safe manner.
    {
      std::unique_lock<std::mutex> unique_lock(lock);
      bcid_set.insert(bcid);
    }

    // The buffer collection *should* be valid here.
    auto import_result = renderer->ImportImage(
        {.collection_id = bcid, .identifier = image_id, .vmo_index = 0, .width = 1, .height = 1});
    EXPECT_TRUE(import_result);
    loop.RunUntilIdle();
  };

  // Run a bunch of threads, alternating between threads that register texture collections
  // and threads that register render target collections.
  std::vector<std::thread> threads;
  for (uint32_t i = 0; i < kNumThreads; i++) {
    threads.push_back(std::thread(register_and_import_function));
  }

  for (auto&& thread : threads) {
    thread.join();
  }

  // Import the ids here one more time to make sure the renderer's internal
  // state hasn't been corrupted. We use the values gathered in the bcid_vec
  // to test with.
  EXPECT_EQ(bcid_set.size(), kNumThreads);
  for (const auto& bcid : bcid_set) {
    // The buffer collection *should* be valid here.
    auto result = renderer->ImportImage({.collection_id = bcid,
                                         .identifier = sysmem_util::GenerateUniqueImageId(),
                                         .vmo_index = 0,
                                         .width = 1,
                                         .height = 1});
    EXPECT_TRUE(result);
  }
}

// This test checks to make sure that the Render() function properly signals
// a zx::event which can be used by an async::Wait object to asynchronously
// call a custom function.
void AsyncEventSignalTest(Renderer* renderer, fuchsia::sysmem::Allocator_Sync* sysmem_allocator,
                          bool use_vulkan) {
  // First create a pairs of sysmem tokens for the render target.
  auto target_tokens = SysmemTokens::Create(sysmem_allocator);

  // Register the render target with the renderer.
  fuchsia::sysmem::BufferCollectionInfo_2 target_info = {};

  auto target_id = sysmem_util::GenerateUniqueBufferCollectionId();

  auto result = renderer->RegisterRenderTargetCollection(target_id, sysmem_allocator,
                                                         std::move(target_tokens.dup_token));
  EXPECT_TRUE(result);

  // Create a client-side handle to the buffer collection and set the client constraints.
  const uint32_t kWidth = 64, kHeight = 32;
  auto client_target_collection =
      CreateClientPointerWithConstraints(sysmem_allocator, std::move(target_tokens.local_token),
                                         /*image_count*/ 1, kWidth, kHeight);
  auto allocation_status = ZX_OK;
  auto status = client_target_collection->WaitForBuffersAllocated(&allocation_status, &target_info);
  EXPECT_EQ(status, ZX_OK);
  EXPECT_EQ(allocation_status, ZX_OK);

  // Now that the renderer and client have set their contraints, we can import the render target.
  // Create the render_target image meta_data.
  ImageMetadata render_target = {.collection_id = target_id,
                                 .identifier = sysmem_util::GenerateUniqueImageId(),
                                 .vmo_index = 0,
                                 .width = kWidth,
                                 .height = kHeight,
                                 .is_render_target = true};
  auto target_import = renderer->ImportImage(render_target);
  EXPECT_TRUE(target_import);

  // Create the release fence that will be passed along to the Render()
  // function and be used to signal when we should deregister the collection.
  zx::event release_fence;
  status = zx::event::create(0, &release_fence);
  EXPECT_EQ(status, ZX_OK);

  // Set up the async::Wait object to wait until the release_fence signals
  // ZX_EVENT_SIGNALED. We make use of a test loop to access an async dispatcher.
  async::TestLoop loop;
  bool signaled = false;
  auto dispatcher = loop.dispatcher();
  auto wait = std::make_unique<async::Wait>(release_fence.get(), ZX_EVENT_SIGNALED);
  wait->set_handler([&signaled](async_dispatcher_t*, async::Wait*, zx_status_t /*status*/,
                                const zx_packet_signal_t* /*signal*/) mutable { signaled = true; });
  wait->Begin(dispatcher);

  // The call to Render() will signal the release fence, triggering the wait object to
  // call its handler function.
  std::vector<zx::event> fences;
  fences.push_back(std::move(release_fence));
  renderer->Render(render_target, {}, {}, fences);

  if (use_vulkan) {
    auto vk_renderer = static_cast<VkRenderer*>(renderer);
    vk_renderer->WaitIdle();
  }

  // Close the test loop and test that our handler was called.
  loop.RunUntilIdle();
  EXPECT_TRUE(signaled);
}

TEST_F(NullRendererTest, RegisterCollectionTest) {
  NullRenderer renderer;
  RegisterCollectionTest(&renderer, sysmem_allocator_.get());
}

TEST_F(NullRendererTest, SameTokenTwiceTest) {
  NullRenderer renderer;
  SameTokenTwiceTest(&renderer, sysmem_allocator_.get());
}

TEST_F(NullRendererTest, BadTokenTest) {
  NullRenderer renderer;
  BadTokenTest(&renderer, sysmem_allocator_.get());
}

TEST_F(NullRendererTest, ImportImageTest) {
  NullRenderer renderer;
  ImportImageTest(&renderer, sysmem_allocator_.get());
}

TEST_F(NullRendererTest, DeregistrationTest) {
  NullRenderer renderer;
  DeregistrationTest(&renderer, sysmem_allocator_.get());
}

TEST_F(NullRendererTest, DISABLED_MultithreadingTest) {
  NullRenderer renderer;
  MultithreadingTest(&renderer);
}

TEST_F(NullRendererTest, AsyncEventSignalTest) {
  NullRenderer renderer;
  AsyncEventSignalTest(&renderer, sysmem_allocator_.get(), /*use_vulkan*/ false);
}

VK_TEST_F(VulkanRendererTest, RegisterCollectionTest) {
  auto env = escher::test::EscherEnvironment::GetGlobalTestEnvironment();
  auto unique_escher = std::make_unique<escher::Escher>(
      env->GetVulkanDevice(), env->GetFilesystem(), /*gpu_allocator*/ nullptr);
  VkRenderer renderer(unique_escher->GetWeakPtr());
  RegisterCollectionTest(&renderer, sysmem_allocator_.get());
}

VK_TEST_F(VulkanRendererTest, SameTokenTwiceTest) {
  auto env = escher::test::EscherEnvironment::GetGlobalTestEnvironment();
  auto unique_escher = std::make_unique<escher::Escher>(
      env->GetVulkanDevice(), env->GetFilesystem(), /*gpu_allocator*/ nullptr);
  VkRenderer renderer(unique_escher->GetWeakPtr());
  SameTokenTwiceTest(&renderer, sysmem_allocator_.get());
}

VK_TEST_F(VulkanRendererTest, BadTokenTest) {
  auto env = escher::test::EscherEnvironment::GetGlobalTestEnvironment();
  auto unique_escher = std::make_unique<escher::Escher>(
      env->GetVulkanDevice(), env->GetFilesystem(), /*gpu_allocator*/ nullptr);
  VkRenderer renderer(unique_escher->GetWeakPtr());
  BadTokenTest(&renderer, sysmem_allocator_.get());
}

VK_TEST_F(VulkanRendererTest, ImportImageTest) {
  auto env = escher::test::EscherEnvironment::GetGlobalTestEnvironment();
  auto unique_escher = std::make_unique<escher::Escher>(
      env->GetVulkanDevice(), env->GetFilesystem(), /*gpu_allocator*/ nullptr);
  VkRenderer renderer(unique_escher->GetWeakPtr());
  ImportImageTest(&renderer, sysmem_allocator_.get());
}

VK_TEST_F(VulkanRendererTest, DeregistrationTest) {
  auto env = escher::test::EscherEnvironment::GetGlobalTestEnvironment();
  auto unique_escher = std::make_unique<escher::Escher>(
      env->GetVulkanDevice(), env->GetFilesystem(), /*gpu_allocator*/ nullptr);
  VkRenderer renderer(unique_escher->GetWeakPtr());
  DeregistrationTest(&renderer, sysmem_allocator_.get());
}

VK_TEST_F(VulkanRendererTest, DISABLED_MultithreadingTest) {
  auto env = escher::test::EscherEnvironment::GetGlobalTestEnvironment();
  auto unique_escher = std::make_unique<escher::Escher>(
      env->GetVulkanDevice(), env->GetFilesystem(), /*gpu_allocator*/ nullptr);
  VkRenderer renderer(unique_escher->GetWeakPtr());
  MultithreadingTest(&renderer);
}

VK_TEST_F(VulkanRendererTest, AsyncEventSignalTest) {
  SKIP_TEST_IF_ESCHER_USES_DEVICE(VirtualGpu);
  auto env = escher::test::EscherEnvironment::GetGlobalTestEnvironment();
  auto unique_escher = std::make_unique<escher::Escher>(
      env->GetVulkanDevice(), env->GetFilesystem(), /*gpu_allocator*/ nullptr);
  VkRenderer renderer(unique_escher->GetWeakPtr());
  AsyncEventSignalTest(&renderer, sysmem_allocator_.get(), /*use_vulkan*/ true);
}

// This test actually renders a rectangle using the VKRenderer. We create a single rectangle,
// with a half-red, half-green texture, translate and scale it. The render target is 16x8
// and the rectangle is 4x2. So in the end the result should look like this:
//
// ----------------
// ----------------
// ----------------
// ------RRGG------
// ------RRGG------
// ----------------
// ----------------
// ----------------
VK_TEST_F(VulkanRendererTest, RenderTest) {
  SKIP_TEST_IF_ESCHER_USES_DEVICE(VirtualGpu);
  auto env = escher::test::EscherEnvironment::GetGlobalTestEnvironment();
  auto unique_escher = std::make_unique<escher::Escher>(
      env->GetVulkanDevice(), env->GetFilesystem(), /*gpu_allocator*/ nullptr);
  VkRenderer renderer(unique_escher->GetWeakPtr());

  // First create the pair of sysmem tokens, one for the client, one for the renderer.
  auto tokens = flatland::SysmemTokens::Create(sysmem_allocator_.get());

  auto target_tokens = flatland::SysmemTokens::Create(sysmem_allocator_.get());

  // Register the collection with the renderer.
  auto collection_id = sysmem_util::GenerateUniqueBufferCollectionId();

  auto result = renderer.ImportBufferCollection(collection_id, sysmem_allocator_.get(),
                                                std::move(tokens.dup_token));
  EXPECT_TRUE(result);

  // Create a client-side handle to the buffer collection and set the client constraints.
  auto [buffer_usage, memory_constraints] = GetUsageAndMemoryConstraintsForCpuWriteOften();
  auto client_collection = CreateClientPointerWithConstraints(
      sysmem_allocator_.get(), std::move(tokens.local_token),
      /*image_count*/ 1,
      /*width*/ 60,
      /*height*/ 40, buffer_usage, std::make_optional(memory_constraints));

  auto target_id = sysmem_util::GenerateUniqueBufferCollectionId();

  result = renderer.RegisterRenderTargetCollection(target_id, sysmem_allocator_.get(),
                                                   std::move(target_tokens.dup_token));
  EXPECT_TRUE(result);

  // Create a client-side handle to the buffer collection and set the client constraints.
  auto client_target = CreateClientPointerWithConstraints(
      sysmem_allocator_.get(), std::move(target_tokens.local_token),
      /*image_count*/ 1,
      /*width*/ 60,
      /*height*/ 40, buffer_usage, std::make_optional(memory_constraints));

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

  // Have the client wait for buffers allocated so it can populate its information
  // struct with the vmo data.
  fuchsia::sysmem::BufferCollectionInfo_2 client_target_info = {};
  {
    zx_status_t allocation_status = ZX_OK;
    auto status = client_target->WaitForBuffersAllocated(&allocation_status, &client_target_info);
    EXPECT_EQ(status, ZX_OK);
    EXPECT_EQ(allocation_status, ZX_OK);
  }

  const uint32_t kTargetWidth = 16;
  const uint32_t kTargetHeight = 8;

  // Create the render_target image meta_data.
  ImageMetadata render_target = {
      .collection_id = target_id,
      .identifier = sysmem_util::GenerateUniqueImageId(),
      .vmo_index = 0,
      .width = kTargetWidth,
      .height = kTargetHeight,
      .is_render_target = true,
  };

  // Create the image meta data for the renderable.
  ImageMetadata renderable_texture = {.collection_id = collection_id,
                                      .identifier = sysmem_util::GenerateUniqueImageId(),
                                      .vmo_index = 0,
                                      .width = 2,
                                      .height = 1};

  auto import_res = renderer.ImportImage(render_target);
  EXPECT_TRUE(import_res);

  import_res = renderer.ImportImage(renderable_texture);
  EXPECT_TRUE(import_res);

  // Create a renderable where the upper-left hand corner should be at position (6,3)
  // with a width/height of (4,2).
  const uint32_t kRenderableWidth = 4;
  const uint32_t kRenderableHeight = 2;
  Rectangle2D renderable(glm::vec2(6, 3), glm::vec2(kRenderableWidth, kRenderableHeight));

  // Have the client write pixel values to the renderable's texture.
  MapHostPointer(client_collection_info, renderable_texture.vmo_index,
                 [&](uint8_t* vmo_host, uint32_t num_bytes) mutable {
                   // The texture only has 2 pixels, so it needs 8 write values for 4 channels. We
                   // set the first pixel to red and the second pixel to green.
                   const uint8_t kNumWrites = 8;
                   const uint8_t kWriteValues[] = {/*red*/ 255U, 0,    0, 255U,
                                                   /*green*/ 0,  255U, 0, 255U};
                   memcpy(vmo_host, kWriteValues, sizeof(kWriteValues));

                   // Flush the cache after writing to host VMO.
                   EXPECT_EQ(ZX_OK,
                             zx_cache_flush(vmo_host, kNumWrites,
                                            ZX_CACHE_FLUSH_DATA | ZX_CACHE_FLUSH_INVALIDATE));
                 });

  // Render the renderable to the render target.
  renderer.Render(render_target, {renderable}, {renderable_texture});
  renderer.WaitIdle();

  // Get a raw pointer from the client collection's vmo that represents the render target
  // and read its values. This should show that the renderable was rendered to the center
  // of the render target, with its associated texture.
  MapHostPointer(client_target_info, render_target.vmo_index,
                 [&](uint8_t* vmo_host, uint32_t num_bytes) mutable {
                   // Flush the cache before reading back target image.
                   EXPECT_EQ(ZX_OK,
                             zx_cache_flush(vmo_host, kTargetWidth * kTargetHeight * 4,
                                            ZX_CACHE_FLUSH_DATA | ZX_CACHE_FLUSH_INVALIDATE));

                   // Make sure the pixels are in the right order give that we rotated
                   // the rectangle.
                   EXPECT_EQ(GetPixel(vmo_host, kTargetWidth, 6, 3), glm::ivec4(255, 0, 0, 255));
                   EXPECT_EQ(GetPixel(vmo_host, kTargetWidth, 7, 3), glm::ivec4(255, 0, 0, 255));
                   EXPECT_EQ(GetPixel(vmo_host, kTargetWidth, 8, 3), glm::ivec4(0, 255, 0, 255));
                   EXPECT_EQ(GetPixel(vmo_host, kTargetWidth, 9, 3), glm::ivec4(0, 255, 0, 255));
                   EXPECT_EQ(GetPixel(vmo_host, kTargetWidth, 6, 4), glm::ivec4(255, 0, 0, 255));
                   EXPECT_EQ(GetPixel(vmo_host, kTargetWidth, 7, 4), glm::ivec4(255, 0, 0, 255));
                   EXPECT_EQ(GetPixel(vmo_host, kTargetWidth, 8, 4), glm::ivec4(0, 255, 0, 255));
                   EXPECT_EQ(GetPixel(vmo_host, kTargetWidth, 9, 4), glm::ivec4(0, 255, 0, 255));

                   // Make sure the remaining pixels are black.
                   uint32_t black_pixels = 0;
                   for (uint32_t y = 0; y < kTargetHeight; y++) {
                     for (uint32_t x = 0; x < kTargetWidth; x++) {
                       auto col = GetPixel(vmo_host, kTargetWidth, x, y);
                       if (col == glm::ivec4(0, 0, 0, 0))
                         black_pixels++;
                     }
                   }
                   EXPECT_EQ(black_pixels,
                             kTargetWidth * kTargetHeight - kRenderableWidth * kRenderableHeight);
                 });
}

// Tests transparency. Render two overlapping rectangles, a red opaque one covered slightly by
// a green transparent one with an alpha of 0.5. The result should look like this:
//
// ----------------
// ----------------
// ----------------
// ------RYYYG----
// ------RYYYG----
// ----------------
// ----------------
// ----------------
// TODO(fxbug.dev/52632): Transparency is currently hardcoded in the renderer to be on. This test
// will break if that is changed to be hardcoded to false before we expose it in the API.
VK_TEST_F(VulkanRendererTest, TransparencyTest) {
  SKIP_TEST_IF_ESCHER_USES_DEVICE(VirtualGpu);
  auto env = escher::test::EscherEnvironment::GetGlobalTestEnvironment();
  auto unique_escher = std::make_unique<escher::Escher>(
      env->GetVulkanDevice(), env->GetFilesystem(), /*gpu_allocator*/ nullptr);
  VkRenderer renderer(unique_escher->GetWeakPtr());

  // First create the pair of sysmem tokens, one for the client, one for the renderer.
  auto tokens = flatland::SysmemTokens::Create(sysmem_allocator_.get());

  auto target_tokens = flatland::SysmemTokens::Create(sysmem_allocator_.get());

  // Register and the collection with the renderer.
  auto collection_id = sysmem_util::GenerateUniqueBufferCollectionId();

  auto result = renderer.ImportBufferCollection(collection_id, sysmem_allocator_.get(),
                                                std::move(tokens.dup_token));
  EXPECT_TRUE(result);

  // Create a client-side handle to the buffer collection and set the client constraints.
  auto [buffer_usage, memory_constraints] = GetUsageAndMemoryConstraintsForCpuWriteOften();
  auto client_collection = CreateClientPointerWithConstraints(
      sysmem_allocator_.get(), std::move(tokens.local_token),
      /*image_count*/ 2,
      /*width*/ 60,
      /*height*/ 40, buffer_usage, std::make_optional(memory_constraints));

  auto target_id = sysmem_util::GenerateUniqueBufferCollectionId();
  result = renderer.RegisterRenderTargetCollection(target_id, sysmem_allocator_.get(),
                                                   std::move(target_tokens.dup_token));
  EXPECT_TRUE(result);

  // Create a client-side handle to the buffer collection and set the client constraints.
  auto client_target = CreateClientPointerWithConstraints(
      sysmem_allocator_.get(), std::move(target_tokens.local_token),
      /*image_count*/ 1,
      /*width*/ 60,
      /*height*/ 40, buffer_usage, std::make_optional(memory_constraints));

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

  fuchsia::sysmem::BufferCollectionInfo_2 client_target_info = {};
  {
    zx_status_t allocation_status = ZX_OK;
    auto status = client_target->WaitForBuffersAllocated(&allocation_status, &client_target_info);
    EXPECT_EQ(status, ZX_OK);
    EXPECT_EQ(allocation_status, ZX_OK);
  }

  const uint32_t kTargetWidth = 16;
  const uint32_t kTargetHeight = 8;

  // Create the render_target image meta_data.
  ImageMetadata render_target = {
      .collection_id = target_id,
      .identifier = sysmem_util::GenerateUniqueImageId(),
      .vmo_index = 0,
      .width = kTargetWidth,
      .height = kTargetHeight,
      .is_render_target = true,
  };

  // Create the image meta data for the renderable.
  ImageMetadata renderable_texture = {.collection_id = collection_id,
                                      .identifier = sysmem_util::GenerateUniqueImageId(),
                                      .vmo_index = 0,
                                      .width = 1,
                                      .height = 1};

  // Create the texture that will go on the transparent renderable.
  ImageMetadata transparent_texture = {.collection_id = collection_id,
                                       .identifier = sysmem_util::GenerateUniqueImageId(),
                                       .vmo_index = 1,
                                       .width = 1,
                                       .height = 1,
                                       .has_transparency = true};

  // Import all the images.
  renderer.ImportImage(render_target);
  renderer.ImportImage(renderable_texture);
  renderer.ImportImage(transparent_texture);

  // Create the two renderables.
  const uint32_t kRenderableWidth = 4;
  const uint32_t kRenderableHeight = 2;
  Rectangle2D renderable(glm::vec2(6, 3), glm::vec2(kRenderableWidth, kRenderableHeight));
  Rectangle2D transparent_renderable(glm::vec2(7, 3),
                                     glm::vec2(kRenderableWidth, kRenderableHeight));

  // Have the client write pixel values to the renderable's texture.
  MapHostPointer(client_collection_info, renderable_texture.vmo_index,
                 [&](uint8_t* vmo_host, uint32_t num_bytes) mutable {
                   // Create a red opaque pixel.
                   const uint8_t kNumWrites = 4;
                   const uint8_t kWriteValues[] = {/*red*/ 255U, 0, 0, 255U};
                   memcpy(vmo_host, kWriteValues, sizeof(kWriteValues));

                   // Flush the cache after writing to host VMO.
                   EXPECT_EQ(ZX_OK,
                             zx_cache_flush(vmo_host, kNumWrites,
                                            ZX_CACHE_FLUSH_DATA | ZX_CACHE_FLUSH_INVALIDATE));
                 });

  MapHostPointer(client_collection_info, transparent_texture.vmo_index,
                 [&](uint8_t* vmo_host, uint32_t num_bytes) mutable {
                   // Create a green pixel with an alpha of 0.5.
                   const uint8_t kNumWrites = 4;
                   const uint8_t kWriteValues[] = {/*red*/ 0, 255, 0, 128U};
                   memcpy(vmo_host, kWriteValues, sizeof(kWriteValues));

                   // Flush the cache after writing to host VMO.
                   EXPECT_EQ(ZX_OK,
                             zx_cache_flush(vmo_host, kNumWrites,
                                            ZX_CACHE_FLUSH_DATA | ZX_CACHE_FLUSH_INVALIDATE));
                 });

  // Render the renderable to the render target.
  renderer.Render(render_target, {renderable, transparent_renderable},
                  {renderable_texture, transparent_texture});
  renderer.WaitIdle();

  // Get a raw pointer from the client collection's vmo that represents the render target
  // and read its values. This should show that the renderable was rendered to the center
  // of the render target, with its associated texture.
  MapHostPointer(client_target_info, render_target.vmo_index,
                 [&](uint8_t* vmo_host, uint32_t num_bytes) mutable {
                   // Flush the cache before reading back target image.
                   EXPECT_EQ(ZX_OK,
                             zx_cache_flush(vmo_host, kTargetWidth * kTargetHeight * 4,
                                            ZX_CACHE_FLUSH_DATA | ZX_CACHE_FLUSH_INVALIDATE));

                   // Make sure the pixels are in the right order give that we rotated
                   // the rectangle.
                   EXPECT_EQ(GetPixel(vmo_host, kTargetWidth, 6, 3), glm::ivec4(255, 0, 0, 255));
                   EXPECT_EQ(GetPixel(vmo_host, kTargetWidth, 6, 4), glm::ivec4(255, 0, 0, 255));
                   EXPECT_EQ(GetPixel(vmo_host, kTargetWidth, 7, 3), glm::ivec4(127, 255, 0, 255));
                   EXPECT_EQ(GetPixel(vmo_host, kTargetWidth, 7, 4), glm::ivec4(127, 255, 0, 255));
                   EXPECT_EQ(GetPixel(vmo_host, kTargetWidth, 8, 3), glm::ivec4(127, 255, 0, 255));
                   EXPECT_EQ(GetPixel(vmo_host, kTargetWidth, 8, 4), glm::ivec4(127, 255, 0, 255));
                   EXPECT_EQ(GetPixel(vmo_host, kTargetWidth, 9, 3), glm::ivec4(127, 255, 0, 255));
                   EXPECT_EQ(GetPixel(vmo_host, kTargetWidth, 9, 4), glm::ivec4(127, 255, 0, 255));
                   EXPECT_EQ(GetPixel(vmo_host, kTargetWidth, 10, 3), glm::ivec4(0, 255, 0, 128));
                   EXPECT_EQ(GetPixel(vmo_host, kTargetWidth, 10, 4), glm::ivec4(0, 255, 0, 128));

                   // Make sure the remaining pixels are black.
                   uint32_t black_pixels = 0;
                   for (uint32_t y = 0; y < kTargetHeight; y++) {
                     for (uint32_t x = 0; x < kTargetWidth; x++) {
                       auto col = GetPixel(vmo_host, kTargetWidth, x, y);
                       if (col == glm::ivec4(0, 0, 0, 0))
                         black_pixels++;
                     }
                   }
                   EXPECT_EQ(black_pixels, kTargetWidth * kTargetHeight - 10U);
                 });
}

}  // namespace flatland
