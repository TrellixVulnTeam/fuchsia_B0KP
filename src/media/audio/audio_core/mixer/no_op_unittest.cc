// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/mixer/no_op.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/media/audio/audio_core/mixer/constants.h"

using testing::FloatEq;
using testing::Pointwise;

namespace media::audio {
namespace {

// Does NoOp mixer behave as expected? (not update offsets, nor touch buffers)
TEST(NoOpMixer, PassThru) {
  auto no_op_mixer = std::make_unique<mixer::NoOp>();
  EXPECT_NE(nullptr, no_op_mixer);

  int16_t source[] = {0x7FFF, -0x8000};
  float accum[] = {-1, 42};
  float expect[] = {-1, 42};

  uint32_t dest_offset = 0;
  int32_t frac_source_offset = 0;

  bool mix_result =
      no_op_mixer->Mix(accum, std::size(accum), &dest_offset, source,
                       std::size(source) << kPtsFractionalBits, &frac_source_offset, false);

  EXPECT_FALSE(mix_result);
  EXPECT_EQ(dest_offset, 0u);
  EXPECT_EQ(frac_source_offset, 0);
  EXPECT_THAT(accum, Pointwise(FloatEq(), expect));
}

}  // namespace
}  // namespace media::audio
