// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/graphics/drivers/misc/goldfish_control/control_device.h"

#include <fuchsia/hardware/goldfish/llcpp/fidl.h>
#include <zircon/syscalls.h>

#include <memory>

#include <ddk/debug.h>
#include <ddk/platform-defs.h>
#include <ddk/trace/event.h>
#include <ddktl/fidl.h>
#include <fbl/auto_call.h>
#include <fbl/auto_lock.h>

#include "src/graphics/drivers/misc/goldfish_control/device_local_heap.h"
#include "src/graphics/drivers/misc/goldfish_control/goldfish_control_composite-bind.h"
#include "src/graphics/drivers/misc/goldfish_control/host_visible_heap.h"
#include "src/graphics/drivers/misc/goldfish_control/render_control_commands.h"

namespace goldfish {
namespace {

const char* kTag = "goldfish-control";

const char* kPipeName = "pipe:opengles";

constexpr uint32_t kClientFlags = 0;

constexpr uint32_t VULKAN_ONLY = 1;

constexpr uint32_t kInvalidBufferHandle = 0U;

zx_koid_t GetKoidForVmo(const zx::vmo& vmo) {
  zx_info_handle_basic_t info;
  zx_status_t status =
      zx_object_get_info(vmo.get(), ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: zx_object_get_info() failed - status: %d", kTag, status);
    return ZX_KOID_INVALID;
  }
  return info.koid;
}

}  // namespace

// static
zx_status_t Control::Create(void* ctx, zx_device_t* device) {
  auto control = std::make_unique<Control>(device);

  zx_status_t status = control->Bind();
  if (status == ZX_OK) {
    // devmgr now owns device.
    __UNUSED auto* dev = control.release();
  }
  return status;
}

Control::Control(zx_device_t* parent) : ControlType(parent) {
  // Initialize parent protocols.
  Init();

  goldfish_control_protocol_t self{&goldfish_control_protocol_ops_, this};
  control_ = ddk::GoldfishControlProtocolClient(&self);
}

Control::~Control() {
  if (id_) {
    fbl::AutoLock lock(&lock_);
    if (cmd_buffer_.is_valid()) {
      for (auto& buffer : buffer_handles_) {
        CloseBufferOrColorBufferLocked(buffer.second);
      }
      auto buffer = static_cast<pipe_cmd_buffer_t*>(cmd_buffer_.virt());
      buffer->id = id_;
      buffer->cmd = PIPE_CMD_CODE_CLOSE;
      buffer->status = PIPE_ERROR_INVAL;

      pipe_.Exec(id_);
      ZX_DEBUG_ASSERT(!buffer->status);
    }
    pipe_.Destroy(id_);
  }
}

zx_status_t Control::Init() {
  zx_status_t status =
      ddk::GoldfishPipeProtocolClient::CreateFromDevice(parent(), "goldfish-pipe", &pipe_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: goldfish pipe fragment is invalid", kTag);
    return status;
  }

  status = ddk::GoldfishAddressSpaceProtocolClient::CreateFromDevice(
      parent(), "goldfish-address-space", &address_space_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: goldfish address space fragment is invalid", kTag);
    return status;
  }

  status = ddk::GoldfishSyncProtocolClient::CreateFromDevice(parent(), "goldfish-sync", &sync_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: goldfish sync fragment is invalid", kTag);
    return status;
  }

  return ZX_OK;
}

zx_status_t Control::InitPipeDeviceLocked() {
  if (!pipe_.is_valid()) {
    zxlogf(ERROR, "%s: no pipe protocol", kTag);
    return ZX_ERR_NOT_SUPPORTED;
  }

  zx_status_t status = pipe_.GetBti(&bti_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: GetBti failed: %d", kTag, status);
    return status;
  }

  status = io_buffer_.Init(bti_.get(), PAGE_SIZE, IO_BUFFER_RW | IO_BUFFER_CONTIG);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: io_buffer_init failed: %d", kTag, status);
    return status;
  }

  ZX_DEBUG_ASSERT(!pipe_event_.is_valid());
  status = zx::event::create(0u, &pipe_event_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: zx_event_create failed: %d", kTag, status);
    return status;
  }

  zx::event pipe_event_dup;
  status = pipe_event_.duplicate(ZX_RIGHT_SAME_RIGHTS, &pipe_event_dup);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: zx_handle_duplicate failed: %d", kTag, status);
    return status;
  }

  zx::vmo vmo;
  status = pipe_.Create(&id_, &vmo);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: pipe Create failed: %d", kTag, status);
    return status;
  }
  status = pipe_.SetEvent(id_, std::move(pipe_event_dup));
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: pipe SetEvent failed: %d", kTag, status);
    return status;
  }

  status = cmd_buffer_.InitVmo(bti_.get(), vmo.get(), 0, IO_BUFFER_RW);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: io_buffer_init_vmo failed: %d", kTag, status);
    return status;
  }

  auto release_buffer =
      fbl::MakeAutoCall([this]() TA_NO_THREAD_SAFETY_ANALYSIS { cmd_buffer_.release(); });

  auto buffer = static_cast<pipe_cmd_buffer_t*>(cmd_buffer_.virt());
  buffer->id = id_;
  buffer->cmd = PIPE_CMD_CODE_OPEN;
  buffer->status = PIPE_ERROR_INVAL;

  pipe_.Open(id_);
  if (buffer->status) {
    zxlogf(ERROR, "%s: Open failed: %d", kTag, buffer->status);
    return ZX_ERR_INTERNAL;
  }

  // Keep buffer after successful execution of OPEN command. This way
  // we'll send CLOSE later.
  release_buffer.cancel();

  size_t length = strlen(kPipeName) + 1;
  memcpy(io_buffer_.virt(), kPipeName, length);
  int32_t consumed_size = 0;
  int32_t result = WriteLocked(static_cast<uint32_t>(length), &consumed_size);
  if (result < 0) {
    zxlogf(ERROR, "%s: failed connecting to '%s' pipe: %d", kTag, kPipeName, result);
    return ZX_ERR_INTERNAL;
  }
  ZX_DEBUG_ASSERT(consumed_size == static_cast<int32_t>(length));

  memcpy(io_buffer_.virt(), &kClientFlags, sizeof(kClientFlags));
  WriteLocked(sizeof(kClientFlags));
  return ZX_OK;
}

zx_status_t Control::InitAddressSpaceDeviceLocked() {
  if (!address_space_.is_valid()) {
    zxlogf(ERROR, "%s: no address space protocol", kTag);
    return ZX_ERR_NOT_SUPPORTED;
  }

  // Initialize address space device.
  zx::channel address_space_child_client, address_space_child_req;
  zx_status_t status =
      zx::channel::create(0u, &address_space_child_client, &address_space_child_req);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: zx_channel_create failed: %d", kTag, status);
    return status;
  }

  status = address_space_.OpenChildDriver(ADDRESS_SPACE_CHILD_DRIVER_TYPE_DEFAULT,
                                          std::move(address_space_child_req));
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: AddressSpaceDevice::OpenChildDriver failed: %d", kTag, status);
    return status;
  }

  address_space_child_ =
      std::make_unique<llcpp::fuchsia::hardware::goldfish::AddressSpaceChildDriver::SyncClient>(
          std::move(address_space_child_client));

  return ZX_OK;
}

zx_status_t Control::InitSyncDeviceLocked() {
  if (!sync_.is_valid()) {
    zxlogf(ERROR, "%s: no sync protocol", kTag);
    return ZX_ERR_NOT_SUPPORTED;
  }

  // Initialize sync timeline client.
  zx::channel timeline_client, timeline_req;
  zx_status_t status = zx::channel::create(0u, &timeline_client, &timeline_req);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: zx_channel_create failed: %d", kTag, status);
    return status;
  }

  status = sync_.CreateTimeline(std::move(timeline_req));
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: SyncDevice::CreateTimeline failed: %d", kTag, status);
    return status;
  }

  sync_timeline_ = std::make_unique<llcpp::fuchsia::hardware::goldfish::SyncTimeline::SyncClient>(
      std::move(timeline_client));
  return ZX_OK;
}

zx_status_t Control::RegisterAndBindHeap(llcpp::fuchsia::sysmem2::HeapType heap_type, Heap* heap) {
  zx::channel heap_request, heap_connection;
  zx_status_t status = zx::channel::create(0, &heap_request, &heap_connection);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: zx::channel:create() failed: %d", kTag, status);
    return status;
  }
  status = pipe_.RegisterSysmemHeap(static_cast<uint64_t>(heap_type), std::move(heap_connection));
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: failed to register heap: %d", kTag, status);
    return status;
  }
  heap->Bind(std::move(heap_request));
  return ZX_OK;
}

zx_status_t Control::Bind() {
  fbl::AutoLock lock(&lock_);

  zx_status_t status = InitPipeDeviceLocked();
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: InitPipeDeviceLocked() failed: %d", kTag, status);
    return status;
  }

  status = InitAddressSpaceDeviceLocked();
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: InitAddressSpaceDeviceLocked() failed: %d", kTag, status);
    return status;
  }

  status = InitSyncDeviceLocked();
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: InitSyncDeviceLocked() failed: %d", kTag, status);
    return status;
  }

  // Serve goldfish device-local heap allocations.
  std::unique_ptr<DeviceLocalHeap> device_local_heap = DeviceLocalHeap::Create(this);
  DeviceLocalHeap* device_local_heap_ptr = device_local_heap.get();
  heaps_.push_back(std::move(device_local_heap));
  RegisterAndBindHeap(llcpp::fuchsia::sysmem2::HeapType::GOLDFISH_DEVICE_LOCAL,
                      device_local_heap_ptr);

  // Serve goldfish host-visible heap allocations.
  std::unique_ptr<HostVisibleHeap> host_visible_heap = HostVisibleHeap::Create(this);
  HostVisibleHeap* host_visible_heap_ptr = host_visible_heap.get();
  heaps_.push_back(std::move(host_visible_heap));
  RegisterAndBindHeap(llcpp::fuchsia::sysmem2::HeapType::GOLDFISH_HOST_VISIBLE,
                      host_visible_heap_ptr);

  return DdkAdd(ddk::DeviceAddArgs("goldfish-control").set_proto_id(ZX_PROTOCOL_GOLDFISH_CONTROL));
}

uint64_t Control::RegisterBufferHandle(const zx::vmo& vmo) {
  zx_koid_t koid = GetKoidForVmo(vmo);
  if (koid == ZX_KOID_INVALID) {
    return static_cast<uint64_t>(ZX_KOID_INVALID);
  }

  fbl::AutoLock lock(&lock_);
  buffer_handles_[koid] = kInvalidBufferHandle;
  return static_cast<uint64_t>(koid);
}

void Control::FreeBufferHandle(uint64_t id) {
  fbl::AutoLock lock(&lock_);

  auto it = buffer_handles_.find(static_cast<zx_koid_t>(id));
  if (it == buffer_handles_.end()) {
    zxlogf(ERROR, "%s: invalid key", kTag);
    return;
  }

  if (it->second) {
    CloseBufferOrColorBufferLocked(it->second);
  }
  buffer_handle_info_.erase(it->second);
  buffer_handles_.erase(it);
}

Control::CreateColorBuffer2Result Control::CreateColorBuffer2(
    zx::vmo vmo, llcpp::fuchsia::hardware::goldfish::CreateColorBuffer2Params create_params) {
  using llcpp::fuchsia::hardware::goldfish::ControlDevice;

  // Check argument validity.
  if (!create_params.has_width() || !create_params.has_height() || !create_params.has_format() ||
      !create_params.has_memory_property()) {
    zxlogf(ERROR, "%s: invalid arguments: width? %d height? %d format? %d memory property? %d\n",
           kTag, create_params.has_width(), create_params.has_height(), create_params.has_format(),
           create_params.has_memory_property());
    return fit::ok(ControlDevice::CreateColorBuffer2Response(ZX_ERR_INVALID_ARGS, -1));
  }
  if ((create_params.memory_property() &
       llcpp::fuchsia::hardware::goldfish::MEMORY_PROPERTY_HOST_VISIBLE) &&
      !create_params.has_physical_address()) {
    zxlogf(ERROR, "%s: invalid arguments: memory_property %d, no physical address\n", kTag,
           create_params.memory_property());
    return fit::ok(ControlDevice::CreateColorBuffer2Response(ZX_ERR_INVALID_ARGS, -1));
  }

  TRACE_DURATION("gfx", "Control::CreateColorBuffer2", "width", create_params.width(), "height",
                 create_params.height(), "format", static_cast<uint32_t>(create_params.format()),
                 "memory_property", create_params.memory_property());

  zx_koid_t koid = GetKoidForVmo(vmo);
  if (koid == ZX_KOID_INVALID) {
    zxlogf(ERROR, "%s: koid of VMO handle %u is invalid", kTag, vmo.get());
    return fit::error(ZX_ERR_INVALID_ARGS);
  }

  fbl::AutoLock lock(&lock_);

  auto it = buffer_handles_.find(koid);
  if (it == buffer_handles_.end()) {
    return fit::ok(ControlDevice::CreateColorBuffer2Response(ZX_ERR_INVALID_ARGS, -1));
  }

  if (it->second != kInvalidBufferHandle) {
    return fit::ok(ControlDevice::CreateColorBuffer2Response(ZX_ERR_ALREADY_EXISTS, -1));
  }

  uint32_t id;
  zx_status_t status = CreateColorBufferLocked(create_params.width(), create_params.height(),
                                               static_cast<uint32_t>(create_params.format()), &id);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: failed to create color buffer: %d", kTag, status);
    return fit::error(status);
  }

  auto close_color_buffer =
      fbl::MakeAutoCall([this, id]() TA_NO_THREAD_SAFETY_ANALYSIS { CloseColorBufferLocked(id); });

  uint32_t result = 0;
  status =
      SetColorBufferVulkanMode2Locked(id, VULKAN_ONLY, create_params.memory_property(), &result);
  if (status != ZX_OK || result) {
    zxlogf(ERROR, "%s: failed to set vulkan mode: %d %d", kTag, status, result);
    return fit::error(status);
  }

  int32_t hw_address_page_offset = -1;
  if (create_params.memory_property() &
      llcpp::fuchsia::hardware::goldfish::MEMORY_PROPERTY_HOST_VISIBLE) {
    uint64_t vmo_size;
    status = vmo.get_size(&vmo_size);
    if (status != ZX_OK) {
      zxlogf(ERROR, "%s: zx_vmo_get_size error: %d", kTag, status);
      return fit::error(status);
    }
    uint32_t map_result = 0;
    status =
        MapGpaToBufferHandleLocked(id, create_params.physical_address(), vmo_size, &map_result);
    if (status != ZX_OK || map_result < 0) {
      zxlogf(ERROR, "%s: failed to map gpa to color buffer: %d %d", kTag, status, map_result);
      return fit::error(status);
    }

    hw_address_page_offset = map_result;
  }

  close_color_buffer.cancel();
  it->second = id;
  buffer_handle_info_[id] = {
      .type = llcpp::fuchsia::hardware::goldfish::BufferHandleType::COLOR_BUFFER,
      .memory_property = create_params.memory_property()};

  return fit::ok(ControlDevice::CreateColorBuffer2Response(ZX_OK, hw_address_page_offset));
}

void Control::CreateColorBuffer2(
    zx::vmo vmo, llcpp::fuchsia::hardware::goldfish::CreateColorBuffer2Params create_params,
    CreateColorBuffer2Completer::Sync& completer) {
  auto result = CreateColorBuffer2(std::move(vmo), std::move(create_params));
  if (result.is_ok()) {
    completer.Reply(result.value().res, result.value().hw_address_page_offset);
  } else {
    completer.Close(result.error());
  }
}

Control::CreateBuffer2Result Control::CreateBuffer2(
    zx::vmo vmo, llcpp::fuchsia::hardware::goldfish::CreateBuffer2Params create_params) {
  using llcpp::fuchsia::hardware::goldfish::ControlDevice;
  using llcpp::fuchsia::hardware::goldfish::ControlDevice_CreateBuffer2_Response;
  using llcpp::fuchsia::hardware::goldfish::ControlDevice_CreateBuffer2_Result;

  // Check argument validity.
  if (!create_params.has_size() || !create_params.has_memory_property()) {
    zxlogf(ERROR, "%s: invalid arguments: size? %d memory property? %d\n", kTag,
           create_params.has_size(), create_params.has_memory_property());
    return fit::ok(ControlDevice_CreateBuffer2_Result::WithErr(
        std::make_unique<zx_status_t>(ZX_ERR_INVALID_ARGS)));
  }
  if ((create_params.memory_property() &
       llcpp::fuchsia::hardware::goldfish::MEMORY_PROPERTY_HOST_VISIBLE) &&
      !create_params.has_physical_address()) {
    zxlogf(ERROR, "%s: invalid arguments: memory_property %d, no physical address\n", kTag,
           create_params.memory_property());
    return fit::ok(ControlDevice_CreateBuffer2_Result::WithErr(
        std::make_unique<zx_status_t>(ZX_ERR_INVALID_ARGS)));
  }

  TRACE_DURATION("gfx", "Control::CreateBuffer2", "size", create_params.size(), "memory_property",
                 create_params.memory_property());

  zx_koid_t koid = GetKoidForVmo(vmo);
  if (koid == ZX_KOID_INVALID) {
    zxlogf(ERROR, "%s: koid of VMO handle %u is invalid", kTag, vmo.get());
    return fit::error(ZX_ERR_INVALID_ARGS);
  }

  fbl::AutoLock lock(&lock_);

  auto it = buffer_handles_.find(koid);
  if (it == buffer_handles_.end()) {
    return fit::ok(ControlDevice_CreateBuffer2_Result::WithErr(
        std::make_unique<zx_status_t>(ZX_ERR_INVALID_ARGS)));
  }

  if (it->second != kInvalidBufferHandle) {
    return fit::ok(ControlDevice_CreateBuffer2_Result::WithErr(
        std::make_unique<zx_status_t>(ZX_ERR_ALREADY_EXISTS)));
  }

  uint32_t id;
  zx_status_t status =
      CreateBuffer2Locked(create_params.size(), create_params.memory_property(), &id);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: failed to create buffer: %d", kTag, status);
    return fit::error(status);
  }

  auto close_buffer =
      fbl::MakeAutoCall([this, id]() TA_NO_THREAD_SAFETY_ANALYSIS { CloseBufferLocked(id); });

  int32_t hw_address_page_offset = -1;
  if (create_params.memory_property() &
      llcpp::fuchsia::hardware::goldfish::MEMORY_PROPERTY_HOST_VISIBLE) {
    uint64_t vmo_size;
    status = vmo.get_size(&vmo_size);
    if (status != ZX_OK) {
      zxlogf(ERROR, "%s: zx_vmo_get_size error: %d", kTag, status);
      return fit::error(status);
    }
    uint32_t map_result = 0;
    status =
        MapGpaToBufferHandleLocked(id, create_params.physical_address(), vmo_size, &map_result);
    if (status != ZX_OK || map_result < 0) {
      zxlogf(ERROR, "%s: failed to map gpa to buffer: %d %d", kTag, status, map_result);
      return fit::error(status);
    }

    hw_address_page_offset = map_result;
  }

  close_buffer.cancel();
  it->second = id;
  buffer_handle_info_[id] = {.type = llcpp::fuchsia::hardware::goldfish::BufferHandleType::BUFFER,
                             .memory_property = create_params.memory_property()};

  return fit::ok(ControlDevice_CreateBuffer2_Result::WithResponse(
      std::make_unique<ControlDevice_CreateBuffer2_Response>(
          ControlDevice_CreateBuffer2_Response{.hw_address_page_offset = hw_address_page_offset})));
}

void Control::CreateBuffer2(zx::vmo vmo,
                            llcpp::fuchsia::hardware::goldfish::CreateBuffer2Params create_params,
                            CreateBuffer2Completer::Sync& completer) {
  auto result = CreateBuffer2(std::move(vmo), std::move(create_params));
  if (result.is_ok()) {
    completer.Reply(result.take_value());
  } else {
    completer.Close(result.error());
  }
}

void Control::CreateSyncFence(zx::eventpair event, CreateSyncFenceCompleter::Sync& completer) {
  zx_status_t status = GoldfishControlCreateSyncFence(std::move(event));
  if (status != ZX_OK) {
    completer.ReplyError(status);
  } else {
    completer.ReplySuccess();
  }
}

void Control::GetBufferHandle(zx::vmo vmo, GetBufferHandleCompleter::Sync& completer) {
  TRACE_DURATION("gfx", "Control::FidlGetBufferHandle");

  zx_koid_t koid = GetKoidForVmo(vmo);
  if (koid == ZX_KOID_INVALID) {
    completer.Close(ZX_ERR_INVALID_ARGS);
    return;
  }

  uint32_t handle = kInvalidBufferHandle;
  auto handle_type = llcpp::fuchsia::hardware::goldfish::BufferHandleType::INVALID;

  fbl::AutoLock lock(&lock_);

  auto it = buffer_handles_.find(koid);
  if (it == buffer_handles_.end()) {
    completer.Reply(ZX_ERR_INVALID_ARGS, handle, handle_type);
    return;
  }

  handle = it->second;
  if (handle == kInvalidBufferHandle) {
    // Color buffer not created yet.
    completer.Reply(ZX_ERR_NOT_FOUND, handle, handle_type);
    return;
  }

  auto it_types = buffer_handle_info_.find(handle);
  if (it_types == buffer_handle_info_.end()) {
    // Color buffer type not registered yet.
    completer.Reply(ZX_ERR_NOT_FOUND, handle, handle_type);
    return;
  }

  handle_type = it_types->second.type;
  completer.Reply(ZX_OK, handle, handle_type);
}

void Control::GetBufferHandleInfo(zx::vmo vmo, GetBufferHandleInfoCompleter::Sync& completer) {
  using llcpp::fuchsia::hardware::goldfish::BufferHandleType;
  using llcpp::fuchsia::hardware::goldfish::ControlDevice_GetBufferHandleInfo_Response;
  using llcpp::fuchsia::hardware::goldfish::ControlDevice_GetBufferHandleInfo_Result;

  TRACE_DURATION("gfx", "Control::FidlGetBufferHandleInfo");

  zx_koid_t koid = GetKoidForVmo(vmo);
  if (koid == ZX_KOID_INVALID) {
    completer.Close(ZX_ERR_INVALID_ARGS);
    return;
  }

  uint32_t handle = kInvalidBufferHandle;
  fbl::AutoLock lock(&lock_);

  auto it = buffer_handles_.find(koid);
  if (it == buffer_handles_.end()) {
    completer.ReplyError(ZX_ERR_INVALID_ARGS);
    return;
  }

  handle = it->second;
  if (handle == kInvalidBufferHandle) {
    // Color buffer not created yet.
    completer.ReplyError(ZX_ERR_NOT_FOUND);
    return;
  }

  auto it_types = buffer_handle_info_.find(handle);
  if (it_types == buffer_handle_info_.end()) {
    // Color buffer type not registered yet.
    completer.ReplyError(ZX_ERR_NOT_FOUND);
    return;
  }

  ControlDevice_GetBufferHandleInfo_Response response;
  auto builder =
      llcpp::fuchsia::hardware::goldfish::BufferHandleInfo::Builder(
          std::make_unique<llcpp::fuchsia::hardware::goldfish::BufferHandleInfo::Frame>())
          .set_id(std::make_unique<uint32_t>(handle))
          .set_memory_property(std::make_unique<uint32_t>(it_types->second.memory_property))
          .set_type(std::make_unique<BufferHandleType>(it_types->second.type));

  completer.Reply(ControlDevice_GetBufferHandleInfo_Result::WithResponse(
      std::make_unique<ControlDevice_GetBufferHandleInfo_Response>(
          ControlDevice_GetBufferHandleInfo_Response{
              .info = builder.build(),
          })));
}

void Control::DdkUnbind(ddk::UnbindTxn txn) { txn.Reply(); }

void Control::DdkRelease() { delete this; }

zx_status_t Control::DdkMessage(fidl_incoming_msg_t* msg, fidl_txn_t* txn) {
  DdkTransaction transaction(txn);
  llcpp::fuchsia::hardware::goldfish::ControlDevice::Dispatch(this, msg, &transaction);
  return transaction.Status();
}

zx_status_t Control::DdkGetProtocol(uint32_t proto_id, void* out_protocol) {
  fbl::AutoLock lock(&lock_);

  switch (proto_id) {
    case ZX_PROTOCOL_GOLDFISH_PIPE: {
      pipe_.GetProto(static_cast<goldfish_pipe_protocol_t*>(out_protocol));
      return ZX_OK;
    }
    case ZX_PROTOCOL_GOLDFISH_CONTROL: {
      control_.GetProto(static_cast<goldfish_control_protocol_t*>(out_protocol));
      return ZX_OK;
    }
    default:
      return ZX_ERR_NOT_SUPPORTED;
  }
}

zx_status_t Control::GoldfishControlGetColorBuffer(zx::vmo vmo, uint32_t* out_id) {
  zx_koid_t koid = GetKoidForVmo(vmo);
  if (koid == ZX_KOID_INVALID) {
    return ZX_ERR_INVALID_ARGS;
  }

  fbl::AutoLock lock(&lock_);

  auto it = buffer_handles_.find(koid);
  if (it == buffer_handles_.end()) {
    return ZX_ERR_INVALID_ARGS;
  }

  *out_id = it->second;
  return ZX_OK;
}

zx_status_t Control::GoldfishControlCreateSyncFence(zx::eventpair event) {
  fbl::AutoLock lock(&lock_);
  uint64_t glsync = 0;
  uint64_t syncthread = 0;
  zx_status_t status = CreateSyncKHRLocked(&glsync, &syncthread);
  if (status != ZX_OK) {
    zxlogf(ERROR, "CreateSyncFence: cannot call rcCreateSyncKHR, status=%d", status);
    return ZX_ERR_INTERNAL;
  }

  auto result = sync_timeline_->TriggerHostWait(glsync, syncthread, std::move(event));
  if (!result.ok()) {
    zxlogf(ERROR, "TriggerHostWait: FIDL call failed, status=%d", result.status());
    return ZX_ERR_INTERNAL;
  }
  return ZX_OK;
}

int32_t Control::WriteLocked(uint32_t cmd_size, int32_t* consumed_size) {
  TRACE_DURATION("gfx", "Control::Write", "cmd_size", cmd_size);

  auto buffer = static_cast<pipe_cmd_buffer_t*>(cmd_buffer_.virt());
  buffer->id = id_;
  buffer->cmd = PIPE_CMD_CODE_WRITE;
  buffer->status = PIPE_ERROR_INVAL;
  buffer->rw_params.ptrs[0] = io_buffer_.phys();
  buffer->rw_params.sizes[0] = cmd_size;
  buffer->rw_params.buffers_count = 1;
  buffer->rw_params.consumed_size = 0;
  pipe_.Exec(id_);
  *consumed_size = buffer->rw_params.consumed_size;
  return buffer->status;
}

void Control::WriteLocked(uint32_t cmd_size) {
  int32_t consumed_size;
  int32_t result = WriteLocked(cmd_size, &consumed_size);
  ZX_DEBUG_ASSERT(result >= 0);
  ZX_DEBUG_ASSERT(consumed_size == static_cast<int32_t>(cmd_size));
}

zx_status_t Control::ReadResultLocked(void* result, size_t size) {
  TRACE_DURATION("gfx", "Control::ReadResult");

  while (true) {
    auto buffer = static_cast<pipe_cmd_buffer_t*>(cmd_buffer_.virt());
    buffer->id = id_;
    buffer->cmd = PIPE_CMD_CODE_READ;
    buffer->status = PIPE_ERROR_INVAL;
    buffer->rw_params.ptrs[0] = io_buffer_.phys();
    buffer->rw_params.sizes[0] = static_cast<uint32_t>(size);
    buffer->rw_params.buffers_count = 1;
    buffer->rw_params.consumed_size = 0;
    pipe_.Exec(id_);

    // Positive consumed size always indicate a successful transfer.
    if (buffer->rw_params.consumed_size) {
      ZX_DEBUG_ASSERT(buffer->rw_params.consumed_size == static_cast<int32_t>(size));
      memcpy(result, io_buffer_.virt(), size);
      return ZX_OK;
    }

    // Early out if error is not because of back-pressure.
    if (buffer->status != PIPE_ERROR_AGAIN) {
      zxlogf(ERROR, "%s: reading result failed: %d", kTag, buffer->status);
      return ZX_ERR_INTERNAL;
    }

    buffer->id = id_;
    buffer->cmd = PIPE_CMD_CODE_WAKE_ON_READ;
    buffer->status = PIPE_ERROR_INVAL;
    pipe_.Exec(id_);
    ZX_DEBUG_ASSERT(!buffer->status);

    // Wait for pipe to become readable.
    zx_status_t status =
        pipe_event_.wait_one(llcpp::fuchsia::hardware::goldfish::SIGNAL_HANGUP |
                                 llcpp::fuchsia::hardware::goldfish::SIGNAL_READABLE,
                             zx::time::infinite(), nullptr);
    if (status != ZX_OK) {
      if (status != ZX_ERR_CANCELED) {
        zxlogf(ERROR, "%s: zx_object_wait_one failed: %d", kTag, status);
      }
      return status;
    }
  }
}

zx_status_t Control::ExecuteCommandLocked(uint32_t cmd_size, uint32_t* result) {
  TRACE_DURATION("gfx", "Control::ExecuteCommand", "cnd_size", cmd_size);

  WriteLocked(cmd_size);
  return ReadResultLocked(result);
}

zx_status_t Control::CreateBuffer2Locked(uint64_t size, uint32_t memory_property, uint32_t* id) {
  TRACE_DURATION("gfx", "Control::CreateBuffer2", "size", size, "memory_property", memory_property);

  auto cmd = static_cast<CreateBuffer2Cmd*>(io_buffer_.virt());
  cmd->op = kOP_rcCreateBuffer2;
  cmd->size = kSize_rcCreateBuffer2;
  cmd->buffer_size = size;
  cmd->memory_property = memory_property;

  return ExecuteCommandLocked(kSize_rcCreateBuffer2, id);
}

zx_status_t Control::CreateColorBufferLocked(uint32_t width, uint32_t height, uint32_t format,
                                             uint32_t* id) {
  TRACE_DURATION("gfx", "Control::CreateColorBuffer", "width", width, "height", height);

  auto cmd = static_cast<CreateColorBufferCmd*>(io_buffer_.virt());
  cmd->op = kOP_rcCreateColorBuffer;
  cmd->size = kSize_rcCreateColorBuffer;
  cmd->width = width;
  cmd->height = height;
  cmd->internalformat = format;

  return ExecuteCommandLocked(kSize_rcCreateColorBuffer, id);
}

void Control::CloseBufferOrColorBufferLocked(uint32_t id) {
  ZX_DEBUG_ASSERT(buffer_handle_info_.find(id) != buffer_handle_info_.end());
  auto buffer_type = buffer_handle_info_.at(id).type;
  switch (buffer_type) {
    case llcpp::fuchsia::hardware::goldfish::BufferHandleType::BUFFER:
      CloseBufferLocked(id);
      break;
    case llcpp::fuchsia::hardware::goldfish::BufferHandleType::COLOR_BUFFER:
      CloseColorBufferLocked(id);
      break;
    default:
      // Otherwise buffer/colorBuffer was not created. We don't need to do
      // anything.
      break;
  }
}

void Control::CloseColorBufferLocked(uint32_t id) {
  TRACE_DURATION("gfx", "Control::CloseColorBuffer", "id", id);

  auto cmd = static_cast<CloseColorBufferCmd*>(io_buffer_.virt());
  cmd->op = kOP_rcCloseColorBuffer;
  cmd->size = kSize_rcCloseColorBuffer;
  cmd->id = id;

  WriteLocked(kSize_rcCloseColorBuffer);
}

void Control::CloseBufferLocked(uint32_t id) {
  TRACE_DURATION("gfx", "Control::CloseBuffer", "id", id);

  auto cmd = static_cast<CloseBufferCmd*>(io_buffer_.virt());
  cmd->op = kOP_rcCloseBuffer;
  cmd->size = kSize_rcCloseBuffer;
  cmd->id = id;

  WriteLocked(kSize_rcCloseBuffer);
}

zx_status_t Control::SetColorBufferVulkanMode2Locked(uint32_t id, uint32_t mode,
                                                     uint32_t memory_property, uint32_t* result) {
  TRACE_DURATION("gfx", "Control::SetColorBufferVulkanMode2Locked", "id", id, "mode", mode,
                 "memory_property", memory_property);

  auto cmd = static_cast<SetColorBufferVulkanMode2Cmd*>(io_buffer_.virt());
  cmd->op = kOP_rcSetColorBufferVulkanMode2;
  cmd->size = kSize_rcSetColorBufferVulkanMode2;
  cmd->id = id;
  cmd->mode = mode;
  cmd->memory_property = memory_property;

  return ExecuteCommandLocked(kSize_rcSetColorBufferVulkanMode2, result);
}

zx_status_t Control::MapGpaToBufferHandleLocked(uint32_t id, uint64_t gpa, uint64_t size,
                                                uint32_t* result) {
  TRACE_DURATION("gfx", "Control::MapGpaToBufferHandleLocked", "id", id, "gpa", gpa, "size", size);

  auto cmd = static_cast<MapGpaToBufferHandle2Cmd*>(io_buffer_.virt());
  cmd->op = kOP_rcMapGpaToBufferHandle2;
  cmd->size = kSize_rcMapGpaToBufferHandle2;
  cmd->id = id;
  cmd->gpa = gpa;
  cmd->map_size = size;

  return ExecuteCommandLocked(kSize_rcMapGpaToBufferHandle2, result);
}

zx_status_t Control::CreateSyncKHRLocked(uint64_t* glsync_out, uint64_t* syncthread_out) {
  TRACE_DURATION("gfx", "Control::CreateSyncKHRLocked");

  constexpr size_t kAttribSize = 2u;

  struct {
    CreateSyncKHRCmdHeader header;
    int32_t attribs[kAttribSize];
    CreateSyncKHRCmdFooter footer;
  } cmd = {
      .header =
          {
              .op = kOP_rcCreateSyncKHR,
              .size = kSize_rcCreateSyncKHRCmd + kAttribSize * sizeof(int32_t),
              .type = EGL_SYNC_NATIVE_FENCE_ANDROID,
              .attribs_size = kAttribSize * sizeof(int32_t),
          },
      .attribs =
          {
              EGL_SYNC_NATIVE_FENCE_FD_ANDROID,
              EGL_NO_NATIVE_FENCE_FD_ANDROID,
          },
      .footer =
          {
              .attribs_size = kAttribSize * sizeof(int32_t),
              .destroy_when_signaled = 1,
              .size_glsync_out = kSize_GlSyncOut,
              .size_syncthread_out = kSize_SyncThreadOut,
          },
  };

  auto cmd_buffer = static_cast<uint8_t*>(io_buffer_.virt());
  memcpy(cmd_buffer, &cmd, sizeof(cmd));

  WriteLocked(static_cast<uint32_t>(sizeof(cmd)));

  struct {
    uint64_t glsync;
    uint64_t syncthread;
  } result;
  zx_status_t status = ReadResultLocked(&result, kSize_GlSyncOut + kSize_SyncThreadOut);
  if (status != ZX_OK) {
    return status;
  }
  *glsync_out = result.glsync;
  *syncthread_out = result.syncthread;
  return ZX_OK;
}

void Control::RemoveHeap(Heap* heap) {
  fbl::AutoLock lock(&lock_);
  // The async loop of heap is still running when calling this method, so that
  // we cannot remove it directly from |heaps_| (otherwise async loop needs to
  // wait for this to end before shutting down the loop, causing an infinite
  // loop), instead we move it into a staging area for future deletion.
  removed_heaps_.push_back(heaps_.erase(*heap));
}

}  // namespace goldfish

static constexpr zx_driver_ops_t goldfish_control_driver_ops = []() -> zx_driver_ops_t {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = goldfish::Control::Create;
  return ops;
}();

ZIRCON_DRIVER(goldfish_control_composite, goldfish_control_driver_ops, "zircon", "0.1");

// clang-format on
