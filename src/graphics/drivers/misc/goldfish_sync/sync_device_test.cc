// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/graphics/drivers/misc/goldfish_sync/sync_device.h"

#include <fuchsia/hardware/acpi/cpp/banjo-mock.h>
#include <fuchsia/hardware/goldfish/llcpp/fidl.h>
#include <fuchsia/hardware/goldfish/sync/cpp/banjo.h>
#include <lib/fake-bti/bti.h>
#include <lib/fake_ddk/fake_ddk.h>
#include <lib/fake_ddk/fidl-helper.h>
#include <lib/fzl/vmo-mapper.h>
#include <lib/zx/channel.h>
#include <lib/zx/time.h>
#include <lib/zx/vmar.h>
#include <zircon/errors.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include <zxtest/zxtest.h>

#include "lib/zx/eventpair.h"
#include "src/graphics/drivers/misc/goldfish_sync/sync_common_defs.h"

namespace goldfish {

namespace sync {

namespace {

constexpr uint32_t kGoldfishSyncBtiId = 0x80888099;

// MMIO Registers of goldfish sync device.
// The layout should match the register offsets defined in sync_common_defs.h.
struct __attribute__((__packed__)) Registers {
  uint32_t batch_command;
  uint32_t batch_guestcommand;
  uint64_t batch_command_addr;
  uint64_t batch_guestcommand_addr;
  uint32_t init;

  void DebugPrint() const {
    printf(
        "Registers [ batch_cmd %08x batch_guestcmd: %08x batch_cmd_addr %08lx "
        "batch_guestcmd_addr %08lx init %08x ]\n",
        batch_command, batch_guestcommand, batch_command_addr, batch_guestcommand_addr, init);
  }
};

}  // namespace

// Test device used for fake DDK based tests. Due to limitation of fake ACPI bus
// used in fake DDK tests, only a fixed VMO can be bound to the ACPI MMIO, thus
// we cannot block MMIO reads / writes or have callbacks, thus we can only feed
// one host command to device at a time.
//
// TODO(67846): Allow injection of ddk::MmioBuffers to test devices so that we
// can add hooks to MMIO register read / write operations, which will better
// simulate the real device.
class TestDevice : public SyncDevice {
 public:
  explicit TestDevice(zx_device_t* parent)
      : SyncDevice(parent, /* can_read_multiple_commands= */ false) {}
  ~TestDevice() = default;

  using SyncDevice::RunHostCommand;
};

// Test suite creating fake SyncDevice on a mock ACPI bus.
class SyncDeviceTest : public zxtest::Test {
 public:
  // |zxtest::Test|
  void SetUp() override {
    zx::bti out_bti;
    ASSERT_OK(fake_bti_create(out_bti.reset_and_get_address()));
    ASSERT_OK(out_bti.duplicate(ZX_RIGHT_SAME_RIGHTS, &acpi_bti_));

    constexpr size_t kCtrlSize = 4096u;
    zx::vmo vmo_control;
    ASSERT_OK(zx::vmo::create(kCtrlSize, 0u, &vmo_control));
    ASSERT_OK(vmo_control.duplicate(ZX_RIGHT_SAME_RIGHTS, &vmo_control_));

    zx::interrupt irq;
    ASSERT_OK(zx::interrupt::create(zx::resource(), 0u, ZX_INTERRUPT_VIRTUAL, &irq));
    ASSERT_OK(irq.duplicate(ZX_RIGHT_SAME_RIGHTS, &irq_));

    mock_acpi_.ExpectGetBti(ZX_OK, kGoldfishSyncBtiId, 0, std::move(out_bti))
        .ExpectGetMmio(ZX_OK, 0u, {.offset = 0u, .size = kCtrlSize, .vmo = vmo_control.release()})
        .ExpectMapInterrupt(ZX_OK, 0u, std::move(irq));

    ddk_.SetProtocol(ZX_PROTOCOL_ACPI, mock_acpi_.GetProto());
    dut_ = std::make_unique<TestDevice>(fake_ddk::FakeParent());
  }

  // |zxtest::Test|
  void TearDown() override {}

  fzl::VmoMapper MapControlRegisters() const {
    fzl::VmoMapper mapping;
    mapping.Map(vmo_control_, 0, sizeof(Registers), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE);
    return mapping;
  }

  fzl::VmoMapper MapIoBuffer() {
    if (!io_buffer_.is_valid()) {
      PrepareIoBuffer();
    }
    fzl::VmoMapper mapping;
    mapping.Map(io_buffer_, 0, io_buffer_size_, ZX_VM_PERM_READ | ZX_VM_PERM_WRITE);
    return mapping;
  }

  template <typename T>
  static void Flush(const T* t) {
    zx_cache_flush(t, sizeof(T), ZX_CACHE_FLUSH_DATA | ZX_CACHE_FLUSH_INVALIDATE);
  }

  zx_status_t PrepareIoBuffer() {
    uint64_t num_pinned_vmos = 0u;
    std::vector<fake_bti_pinned_vmo_info_t> pinned_vmo_info;
    zx_status_t status = fake_bti_get_pinned_vmos(acpi_bti_.get(), nullptr, 0, &num_pinned_vmos);
    if (status != ZX_OK) {
      return status;
    }
    if (num_pinned_vmos == 0u) {
      return ZX_ERR_NOT_FOUND;
    }

    pinned_vmo_info.resize(num_pinned_vmos);
    status =
        fake_bti_get_pinned_vmos(acpi_bti_.get(), pinned_vmo_info.data(), num_pinned_vmos, nullptr);
    if (status != ZX_OK) {
      return status;
    }

    io_buffer_ = zx::vmo(pinned_vmo_info.back().vmo);
    pinned_vmo_info.pop_back();
    // close all the unused handles
    for (auto info : pinned_vmo_info) {
      zx_handle_close(info.vmo);
    }

    status = io_buffer_.get_size(&io_buffer_size_);
    return status;
  }

 protected:
  ddk::MockAcpi mock_acpi_;
  fake_ddk::Bind ddk_;
  std::unique_ptr<TestDevice> dut_;

  zx::bti acpi_bti_;
  zx::vmo vmo_control_;
  zx::vmo io_buffer_;
  zx::interrupt irq_;

  size_t io_buffer_size_;
};

// Tests the sync device setup process.
// Checks that the control registers are correctly initialized.
TEST_F(SyncDeviceTest, Bind) {
  {
    auto mapped = MapControlRegisters();
    Registers* ctrl_regs = reinterpret_cast<Registers*>(mapped.start());

    memset(ctrl_regs, 0x0u, sizeof(Registers));
    ctrl_regs->init = 0xffffffffu;
  }

  ASSERT_OK(dut_->Bind());

  {
    auto mapped = MapControlRegisters();
    Registers* ctrl_regs = reinterpret_cast<Registers*>(mapped.start());
    Flush(ctrl_regs);

    ASSERT_NE(ctrl_regs->batch_command_addr, 0u);
    ASSERT_NE(ctrl_regs->batch_guestcommand_addr, 0u);
    ASSERT_EQ(ctrl_regs->init, 0u);
  }

  dut_->DdkAsyncRemove();
  EXPECT_TRUE(ddk_.Ok());
}

// Tests FIDL channel creation and TriggerHostWait() call.
//
// This creates a FIDL channel for banjo clients, so that clients can call
// SyncTimeline.TriggerHostWait() method on the channel to get a waitable event.
TEST_F(SyncDeviceTest, TriggerHostWait) {
  ASSERT_OK(dut_->Bind());
  {
    auto mapped = MapControlRegisters();
    Registers* ctrl_regs = reinterpret_cast<Registers*>(mapped.start());

    memset(ctrl_regs, 0x0u, sizeof(Registers));
    ctrl_regs->batch_guestcommand = 0xffffffffu;
  }

  zx::channel tl_req, tl_client;
  ASSERT_OK(zx::channel::create(0u, &tl_req, &tl_client));
  ASSERT_OK(dut_->GoldfishSyncCreateTimeline(std::move(tl_req)));

  llcpp::fuchsia::hardware::goldfish::SyncTimeline::SyncClient tl(std::move(tl_client));

  uint64_t kGlSyncHandle = 0xabcd'1234'5678'90abUL;
  uint64_t kSyncThreadHandle = 0xdcba'9876'5432'01feUL;

  zx::eventpair event_client, event_server;
  zx_status_t status = zx::eventpair::create(0u, &event_client, &event_server);
  ASSERT_EQ(status, ZX_OK);

  // Make a FIDL TriggerHostWait call.
  auto result = tl.TriggerHostWait(kGlSyncHandle, kSyncThreadHandle, std::move(event_server));
  ASSERT_TRUE(result.ok());

  // Verify the returned eventpair.
  zx_status_t wait_status =
      event_client.wait_one(ZX_EVENTPAIR_SIGNALED, zx::deadline_after(zx::msec(100)), nullptr);
  EXPECT_EQ(wait_status, ZX_ERR_TIMED_OUT);

  // Verify the control registers.
  {
    auto mapped = MapControlRegisters();
    Registers* ctrl_regs = reinterpret_cast<Registers*>(mapped.start());
    ASSERT_EQ(ctrl_regs->batch_guestcommand, 0u);
  }

  // Verify command buffers.
  SyncTimeline* timeline_ptr;
  {
    auto mapped = MapIoBuffer();
    CommandBuffers* cmd_buffers = reinterpret_cast<CommandBuffers*>(mapped.start());
    EXPECT_EQ(cmd_buffers->batch_guestcmd.host_command, CMD_TRIGGER_HOST_WAIT);
    EXPECT_EQ(cmd_buffers->batch_guestcmd.glsync_handle, kGlSyncHandle);
    EXPECT_EQ(cmd_buffers->batch_guestcmd.thread_handle, kSyncThreadHandle);
    EXPECT_NE(cmd_buffers->batch_guestcmd.guest_timeline_handle, 0u);

    timeline_ptr =
        reinterpret_cast<SyncTimeline*>(cmd_buffers->batch_guestcmd.guest_timeline_handle);
  }

  // Verify SyncTimeline pointer.
  EXPECT_TRUE(timeline_ptr->InContainer());
}

// Tests goldfish sync host commands handling.
//
// This tests CMD_CREATE_SYNC_TIMELINE and CMD_DESTROY_SYNC_TIMELINE commands.
TEST_F(SyncDeviceTest, HostCommand_CreateDestroyTimeline) {
  ASSERT_OK(dut_->Bind());
  {
    auto mapped = MapControlRegisters();
    Registers* ctrl_regs = reinterpret_cast<Registers*>(mapped.start());

    memset(ctrl_regs, 0x0u, sizeof(Registers));
    ctrl_regs->batch_command = 0xffffffffu;
    ctrl_regs->batch_guestcommand = 0xffffffffu;
  }

  uint64_t kHostCmdHandle = 0xabcd'1234'5678'abcdUL;
  // Test "CMD_CREATE_SYNC_TIMELINE" command.
  dut_->RunHostCommand({
      .hostcmd_handle = kHostCmdHandle,
      .cmd = CMD_CREATE_SYNC_TIMELINE,
  });

  // Verify the control registers.
  {
    auto mapped = MapControlRegisters();
    Registers* ctrl_regs = reinterpret_cast<Registers*>(mapped.start());
    ASSERT_EQ(ctrl_regs->batch_command, 0u);

    ctrl_regs->batch_command = 0xffffffffu;
  }

  // Verify command buffers.
  fbl::RefPtr<SyncTimeline> timeline_ptr;
  {
    auto mapped = MapIoBuffer();
    CommandBuffers* cmd_buffers = reinterpret_cast<CommandBuffers*>(mapped.start());
    EXPECT_EQ(cmd_buffers->batch_hostcmd.cmd, CMD_CREATE_SYNC_TIMELINE);
    EXPECT_EQ(cmd_buffers->batch_hostcmd.hostcmd_handle, kHostCmdHandle);
    EXPECT_EQ(cmd_buffers->batch_hostcmd.time_arg, 0u);
    EXPECT_NE(cmd_buffers->batch_hostcmd.handle, 0u);

    timeline_ptr = fbl::RefPtr<SyncTimeline>(
        reinterpret_cast<SyncTimeline*>(cmd_buffers->batch_hostcmd.handle));
    memset(cmd_buffers, 0, sizeof(CommandBuffers));
  }

  // Verify timeline.
  EXPECT_TRUE(timeline_ptr->InContainer());

  // Test "CMD_DESTROY_SYNC_TIMELINE" command.
  dut_->RunHostCommand({
      .handle = reinterpret_cast<uint64_t>(timeline_ptr.get()),
      .hostcmd_handle = kHostCmdHandle,
      .cmd = CMD_DESTROY_SYNC_TIMELINE,
  });

  // Verify timeline.
  EXPECT_FALSE(timeline_ptr->InContainer());
}

// Tests goldfish sync host commands handling.
//
// This tests CMD_CREATE_SYNC_FENCE and CMD_SYNC_TIMELINE_INC commands, as well
// as fence signaling logic.
TEST_F(SyncDeviceTest, HostCommand_CreateSignalFences) {
  ASSERT_OK(dut_->Bind());
  {
    auto mapped = MapControlRegisters();
    Registers* ctrl_regs = reinterpret_cast<Registers*>(mapped.start());

    memset(ctrl_regs, 0x0u, sizeof(Registers));
    ctrl_regs->batch_command = 0xffffffffu;
    ctrl_regs->batch_guestcommand = 0xffffffffu;
  }

  // Create timeline.
  dut_->RunHostCommand({
      .hostcmd_handle = 1u,
      .cmd = CMD_CREATE_SYNC_TIMELINE,
  });

  fbl::RefPtr<SyncTimeline> timeline_ptr;
  {
    auto mapped = MapIoBuffer();
    CommandBuffers* cmd_buffers = reinterpret_cast<CommandBuffers*>(mapped.start());
    ASSERT_NE(cmd_buffers->batch_hostcmd.handle, 0u);
    timeline_ptr = fbl::RefPtr<SyncTimeline>(
        reinterpret_cast<SyncTimeline*>(cmd_buffers->batch_hostcmd.handle));
    EXPECT_TRUE(timeline_ptr->InContainer());
  }

  // Reset control registers.
  {
    auto mapped = MapControlRegisters();
    Registers* ctrl_regs = reinterpret_cast<Registers*>(mapped.start());
    ctrl_regs->batch_command = 0xffffffffu;
  }

  // Create fence.
  dut_->RunHostCommand({
      .handle = reinterpret_cast<uint64_t>(timeline_ptr.get()),
      .hostcmd_handle = 2u,
      .cmd = CMD_CREATE_SYNC_FENCE,
      .time_arg = 1u,
  });

  // Verify control registers and command buffers.
  zx::eventpair event_timeline_1 = {};
  {
    auto reg_map = MapControlRegisters();
    Registers* ctrl_regs = reinterpret_cast<Registers*>(reg_map.start());
    EXPECT_EQ(ctrl_regs->batch_command, 0u);

    auto io_map = MapIoBuffer();
    CommandBuffers* cmd_buffers = reinterpret_cast<CommandBuffers*>(io_map.start());
    EXPECT_EQ(cmd_buffers->batch_hostcmd.cmd, CMD_CREATE_SYNC_FENCE);
    EXPECT_EQ(cmd_buffers->batch_hostcmd.hostcmd_handle, 2u);
    ASSERT_NE(cmd_buffers->batch_hostcmd.handle, 0u);

    event_timeline_1.reset(cmd_buffers->batch_hostcmd.handle);
    ASSERT_TRUE(event_timeline_1.is_valid());
  }

  // Create another fence, waiting on the same timeline at timestamp 2.
  dut_->RunHostCommand({
      .handle = reinterpret_cast<uint64_t>(timeline_ptr.get()),
      .hostcmd_handle = 3u,
      .cmd = CMD_CREATE_SYNC_FENCE,
      .time_arg = 2u,
  });

  // Verify control registers and command buffers.
  zx::eventpair event_timeline_2 = {};
  {
    auto reg_map = MapControlRegisters();
    Registers* ctrl_regs = reinterpret_cast<Registers*>(reg_map.start());
    EXPECT_EQ(ctrl_regs->batch_command, 0u);

    auto mapped = MapIoBuffer();
    CommandBuffers* cmd_buffers = reinterpret_cast<CommandBuffers*>(mapped.start());
    EXPECT_EQ(cmd_buffers->batch_hostcmd.cmd, CMD_CREATE_SYNC_FENCE);
    EXPECT_EQ(cmd_buffers->batch_hostcmd.hostcmd_handle, 3u);
    ASSERT_NE(cmd_buffers->batch_hostcmd.handle, 0u);

    event_timeline_2.reset(cmd_buffers->batch_hostcmd.handle);
    ASSERT_TRUE(event_timeline_2.is_valid());
  }

  // At this moment, neither of the events should be signalled.
  EXPECT_EQ(
      event_timeline_1.wait_one(ZX_EVENTPAIR_SIGNALED, zx::deadline_after(zx::msec(100)), nullptr),
      ZX_ERR_TIMED_OUT);
  EXPECT_EQ(
      event_timeline_2.wait_one(ZX_EVENTPAIR_SIGNALED, zx::deadline_after(zx::msec(100)), nullptr),
      ZX_ERR_TIMED_OUT);

  // Now we increase timeline to timestamp 1.
  dut_->RunHostCommand({
      .handle = reinterpret_cast<uint64_t>(timeline_ptr.get()),
      .hostcmd_handle = 4u,
      .cmd = CMD_SYNC_TIMELINE_INC,
      .time_arg = 1u,
  });

  // |event_timeline_1| should be signalled, while |event_timeline_2| is still
  // waiting for the timeline getting to timestamp 2.
  EXPECT_EQ(
      event_timeline_1.wait_one(ZX_EVENTPAIR_SIGNALED, zx::deadline_after(zx::msec(100)), nullptr),
      ZX_OK);
  EXPECT_EQ(
      event_timeline_2.wait_one(ZX_EVENTPAIR_SIGNALED, zx::deadline_after(zx::msec(100)), nullptr),
      ZX_ERR_TIMED_OUT);

  // Now we increase timeline to timestamp 2.
  dut_->RunHostCommand({
      .handle = reinterpret_cast<uint64_t>(timeline_ptr.get()),
      .hostcmd_handle = 5u,
      .cmd = CMD_SYNC_TIMELINE_INC,
      .time_arg = 1u,
  });

  // Now |event_timeline_2| should be signalled as well.
  EXPECT_EQ(
      event_timeline_2.wait_one(ZX_EVENTPAIR_SIGNALED, zx::deadline_after(zx::msec(100)), nullptr),
      ZX_OK);

  // Destroy the timeline.
  dut_->RunHostCommand({
      .handle = reinterpret_cast<uint64_t>(timeline_ptr.get()),
      .hostcmd_handle = 6u,
      .cmd = CMD_DESTROY_SYNC_TIMELINE,
  });

  // Verify timeline.
  EXPECT_FALSE(timeline_ptr->InContainer());
}

// Tests the interrupt handler. Real goldfish sync devices always use interrupts
// to inform system on incoming host commands. This test case simulates the
// interrupt-triggered host command handling logic.
TEST_F(SyncDeviceTest, IrqHandler) {
  {
    auto mapped = MapControlRegisters();
    Registers* ctrl_regs = reinterpret_cast<Registers*>(mapped.start());

    memset(ctrl_regs, 0x0u, sizeof(Registers));
    ctrl_regs->batch_command = 0xffffffffu;
    ctrl_regs->batch_guestcommand = 0xffffffffu;
  }
  ASSERT_OK(dut_->Bind());

  {
    auto mapped = MapIoBuffer();
    CommandBuffers* cmd_buffers = reinterpret_cast<CommandBuffers*>(mapped.start());
    cmd_buffers->batch_hostcmd.cmd = CMD_CREATE_SYNC_TIMELINE;
    cmd_buffers->batch_hostcmd.hostcmd_handle = 1u;
    cmd_buffers->batch_hostcmd.handle = 0u;
  }
  irq_.trigger(0u, zx::time());

  // Irq handler thread handles the interrupt, copying command into staging
  // buffer and post a task on async loop to handle the commands.
  // Async loop thread runs the command and returns the value back to the
  // command buffer.
  // We wait on the test thread until all the tasks above have finished.
  uint32_t wait_msecs = 0u;
  while (true) {
    auto mapped = MapIoBuffer();
    CommandBuffers* cmd_buffers = reinterpret_cast<CommandBuffers*>(mapped.start());
    if (cmd_buffers->batch_hostcmd.handle) {
      EXPECT_TRUE(
          reinterpret_cast<SyncTimeline*>(cmd_buffers->batch_hostcmd.handle)->InContainer());
      break;
    }
    zx::nanosleep(zx::deadline_after(zx::msec(100)));
    wait_msecs += 100u;
    if (wait_msecs >= 15'000) {  // 15s
      FAIL("timeout");
    }
  }
}

// This test case simulates the most common use case of goldfish_sync device.
//
// Clients asks for SyncTimeline and do TriggerHostWait() FIDL calls, waiting on
// returned events.
// Once the wait finishes on goldfish device, devices sends a SYNC_TIMELINE_INC
// command and triggers the interrupt, making driver signal the event to notify
// clients.
TEST_F(SyncDeviceTest, TriggerHostWaitAndSignalFence) {
  ASSERT_OK(dut_->Bind());

  zx::channel tl_req, tl_client;
  ASSERT_OK(zx::channel::create(0u, &tl_req, &tl_client));
  ASSERT_OK(dut_->GoldfishSyncCreateTimeline(std::move(tl_req)));

  llcpp::fuchsia::hardware::goldfish::SyncTimeline::SyncClient tl(std::move(tl_client));

  uint64_t kGlSyncHandle = 0xabcd'1234'5678'90abUL;
  uint64_t kSyncThreadHandle = 0xdcba'9876'5432'01feUL;

  // Make a FIDL TriggerHostWait call.
  zx::eventpair event_client, event_server;
  zx_status_t status = zx::eventpair::create(0u, &event_client, &event_server);
  ASSERT_EQ(status, ZX_OK);

  auto result = tl.TriggerHostWait(kGlSyncHandle, kSyncThreadHandle, std::move(event_server));
  ASSERT_TRUE(result.ok());

  // Verify the returned eventpair.
  zx_status_t wait_status =
      event_client.wait_one(ZX_EVENTPAIR_SIGNALED, zx::deadline_after(zx::msec(100)), nullptr);
  EXPECT_EQ(wait_status, ZX_ERR_TIMED_OUT);

  fbl::RefPtr<SyncTimeline> timeline_ptr;
  {
    auto mapped = MapIoBuffer();
    CommandBuffers* cmd_buffers = reinterpret_cast<CommandBuffers*>(mapped.start());
    ASSERT_NE(cmd_buffers->batch_guestcmd.guest_timeline_handle, 0u);
    timeline_ptr = fbl::RefPtr(
        reinterpret_cast<SyncTimeline*>(cmd_buffers->batch_guestcmd.guest_timeline_handle));
  }

  // Set up host command (CMD_SYNC_TIMELINE_INC) and trigger an interrupt.
  {
    auto mapped = MapIoBuffer();
    CommandBuffers* cmd_buffers = reinterpret_cast<CommandBuffers*>(mapped.start());
    cmd_buffers->batch_hostcmd.cmd = CMD_SYNC_TIMELINE_INC;
    cmd_buffers->batch_hostcmd.hostcmd_handle = 1u;
    cmd_buffers->batch_hostcmd.handle = reinterpret_cast<uint64_t>(timeline_ptr.get());
    cmd_buffers->batch_hostcmd.time_arg = 1u;
  }
  irq_.trigger(0u, zx::time());

  // Event should be signalled once the host command is executed.
  wait_status =
      event_client.wait_one(ZX_EVENTPAIR_SIGNALED, zx::deadline_after(zx::sec(15)), nullptr);
  EXPECT_EQ(wait_status, ZX_OK);
}

}  // namespace sync

}  // namespace goldfish
