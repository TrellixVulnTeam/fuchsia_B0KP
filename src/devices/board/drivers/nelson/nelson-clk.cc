// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/hardware/platform/bus/c/banjo.h>

#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/metadata.h>
#include <ddk/metadata/clock.h>
#include <ddk/platform-defs.h>
#include <soc/aml-meson/sm1-clk.h>
#include <soc/aml-s905d3/s905d3-hw.h>

#include "nelson.h"

namespace nelson {

constexpr pbus_mmio_t clk_mmios[] = {
    // CLK Registers
    {
        .base = S905D3_HIU_BASE,
        .length = S905D3_HIU_LENGTH,
    },
    {
        .base = S905D3_DOS_BASE,
        .length = S905D3_DOS_LENGTH,
    },
    // CLK MSR block
    {
        .base = S905D3_MSR_CLK_BASE,
        .length = S905D3_MSR_CLK_LENGTH,
    },
};

constexpr clock_id_t clock_ids[] = {
    {sm1_clk::CLK_RESET},  // PLACEHOLDER.

    // For audio driver.
    {sm1_clk::CLK_HIFI_PLL},
    {sm1_clk::CLK_SYS_PLL_DIV16},
    {sm1_clk::CLK_SYS_CPU_CLK_DIV16},

    // For video decoder
    {sm1_clk::CLK_DOS_GCLK_VDEC},
    {sm1_clk::CLK_DOS},
};

const pbus_metadata_t clock_metadata[] = {
    {
        .type = DEVICE_METADATA_CLOCK_IDS,
        .data_buffer = reinterpret_cast<const uint8_t*>(&clock_ids),
        .data_size = sizeof(clock_ids),
    },
};

static const pbus_dev_t clk_dev = []() {
  pbus_dev_t dev = {};
  dev.name = "nelson-clk";
  dev.vid = PDEV_VID_AMLOGIC;
  dev.pid = PDEV_PID_AMLOGIC_S905D3;
  dev.did = PDEV_DID_AMLOGIC_SM1_CLK;
  dev.mmio_list = clk_mmios;
  dev.mmio_count = countof(clk_mmios);
  dev.metadata_list = clock_metadata;
  dev.metadata_count = countof(clock_metadata);
  return dev;
}();

zx_status_t Nelson::ClkInit() {
  zx_status_t status = pbus_.ProtocolDeviceAdd(ZX_PROTOCOL_CLOCK_IMPL, &clk_dev);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: ProtocolDeviceAdd failed, st = %d", __func__, status);
    return status;
  }

  clk_impl_ = ddk::ClockImplProtocolClient(parent());
  if (!clk_impl_.is_valid()) {
    zxlogf(ERROR, "%s: ClockImplProtocolClient failed", __func__);
    return ZX_ERR_INTERNAL;
  }

  return ZX_OK;
}

}  // namespace nelson
