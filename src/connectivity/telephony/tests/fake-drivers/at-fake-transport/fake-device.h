// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef SRC_CONNECTIVITY_TELEPHONY_TESTS_FAKE_DRIVERS_AT_FAKE_TRANSPORT_FAKE_DEVICE_H_
#define SRC_CONNECTIVITY_TELEPHONY_TESTS_FAKE_DRIVERS_AT_FAKE_TRANSPORT_FAKE_DEVICE_H_
#include <fuchsia/hardware/test/c/banjo.h>
#include <zircon/compiler.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>

#define _ALL_SOURCE
#include <fuchsia/hardware/telephony/transport/llcpp/fidl.h>
#include <fuchsia/telephony/snoop/llcpp/fidl.h>
#include <threads.h>

#include <src/connectivity/telephony/tests/fake-drivers/fake-transport-base/fake-transport-base.h>

namespace at_fake {
class AtDevice : public tel_fake::Device {
 public:
  AtDevice(zx_device_t* device);

  zx_status_t Bind() override;

  void ReplyCtrlMsg(uint8_t* req, uint32_t req_size, uint8_t* resp, uint32_t resp_size) override;
  void SnoopCtrlMsg(uint8_t* snoop_data, uint32_t snoop_data_len,
                    ::llcpp::fuchsia::telephony::snoop::Direction direction) override;
};

}  // namespace at_fake

#endif  // SRC_CONNECTIVITY_TELEPHONY_TESTS_FAKE_DRIVERS_AT_FAKE_TRANSPORT_FAKE_DEVICE_H_
