// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/a11y/lib/gesture_manager/recognizers/m_finger_n_tap_drag_recognizer.h"

#include <fuchsia/ui/input/accessibility/cpp/fidl.h>

#include <memory>

#include <gtest/gtest.h>

#include "src/lib/testing/loop_fixture/test_loop_fixture.h"
#include "src/ui/a11y/lib/gesture_manager/arena/tests/mocks/mock_contest_member.h"
#include "src/ui/a11y/lib/gesture_manager/gesture_util/util.h"
#include "src/ui/a11y/lib/testing/input.h"

namespace accessibility_test {

namespace {

using Phase = fuchsia::ui::input::PointerEventPhase;

class MFingerNTapDragRecognizerTest : public gtest::TestLoopFixture {
 public:
  MFingerNTapDragRecognizerTest() = default;

  void SendPointerEvents(const std::vector<PointerParams>& events) const {
    for (const auto& event : events) {
      SendPointerEvent(event);
    }
  }

  // Constraints to keep in mind when simulating |GestureArena| behavior:
  // * Only send pointer events while a contest member is held.
  void SendPointerEvent(const PointerParams& event) const {
    if (member_.is_held()) {
      recognizer_->HandleEvent(ToPointerEvent(event, 0));
    }
  }

  void CreateGestureRecognizer(uint32_t number_of_fingers, uint32_t number_of_taps) {
    recognizer_ = std::make_unique<a11y::MFingerNTapDragRecognizer>(
        [this](a11y::GestureContext context) {
          gesture_won_ = true;
          gesture_context_ = context;
        },
        [this](a11y::GestureContext context) { gesture_updates_.push_back(context); },
        [this](a11y::GestureContext context) { gesture_complete_called_ = true; },
        number_of_fingers, number_of_taps);
  }

  MockContestMember member_;
  std::unique_ptr<a11y::MFingerNTapDragRecognizer> recognizer_;
  bool gesture_won_ = false;
  bool gesture_complete_called_ = false;
  a11y::GestureContext gesture_context_;
  std::vector<a11y::GestureContext> gesture_updates_;
};

// Tests successfulthree-finger double-tap with hold detection.
TEST_F(MFingerNTapDragRecognizerTest, ThreeFingerDoubleTapWithHoldDetected) {
  CreateGestureRecognizer(3 /*number of fingers*/, 2 /*number of fingers*/);
  recognizer_->OnContestStarted(member_.TakeInterface());

  // Send events for first tap.
  SendPointerEvents((DownEvents(1, {}) + DownEvents(2, {}) + DownEvents(3, {}) + UpEvents(1, {}) +
                     UpEvents(2, {}) + UpEvents(3, {})));

  // Send events for second tap.
  SendPointerEvents((DownEvents(1, {}) + DownEvents(2, {}) + DownEvents(3, {})));

  RunLoopFor(a11y::MFingerNTapDragRecognizer::kMinTapHoldDuration);

  EXPECT_EQ(member_.status(), a11y::ContestMember::Status::kAccepted);
  recognizer_->OnWin();

  EXPECT_TRUE(gesture_won_);
  EXPECT_FALSE(gesture_complete_called_);

  SendPointerEvents(MoveEvents(1, {}, {0, .5f}));
  EXPECT_EQ(gesture_updates_.size(), 10u);
  EXPECT_EQ(gesture_updates_[9].current_pointer_locations[1].ndc_point.x, 0);
  EXPECT_GT(gesture_updates_[9].current_pointer_locations[1].ndc_point.y, .49f);
  EXPECT_LT(gesture_updates_[9].current_pointer_locations[1].ndc_point.y, .51f);

  // We should call on_complete_ after the first UP event received after the
  // gesture was accepted.
  SendPointerEvents(UpEvents(1, {}));

  EXPECT_TRUE(gesture_complete_called_);
}

// Tests rejection of drag that doesn't last long enough.
TEST_F(MFingerNTapDragRecognizerTest, ThreeFingerDoubleTapWithDragRejected) {
  CreateGestureRecognizer(3 /*number of fingers*/, 2 /*number of fingers*/);
  recognizer_->OnContestStarted(member_.TakeInterface());

  // Send events for first tap.
  SendPointerEvents((DownEvents(1, {}) + DownEvents(2, {}) + DownEvents(3, {}) + UpEvents(1, {}) +
                     UpEvents(2, {}) + UpEvents(3, {})));

  // Send events for second tap.
  SendPointerEvents((DownEvents(1, {}) + DownEvents(2, {}) + DownEvents(3, {}) + UpEvents(1, {}) +
                     UpEvents(2, {}) + UpEvents(3, {})));

  EXPECT_EQ(member_.status(), a11y::ContestMember::Status::kRejected);
  EXPECT_FALSE(gesture_won_);
  EXPECT_TRUE(gesture_updates_.empty());
  EXPECT_FALSE(gesture_complete_called_);
}

// Tests successful one-finger triple-tap with drag detection.
TEST_F(MFingerNTapDragRecognizerTest, OneFingerTripleTapWithDragDetected) {
  CreateGestureRecognizer(1 /*number of fingers*/, 3 /*number of fingers*/);
  recognizer_->OnContestStarted(member_.TakeInterface());

  SendPointerEvents((DownEvents(1, {}) + UpEvents(1, {}) + DownEvents(1, {}) + UpEvents(1, {}) +
                     DownEvents(1, {}) + MoveEvents(1, {}, {})));

  RunLoopFor(a11y::MFingerNTapDragRecognizer::kMinTapHoldDuration);

  EXPECT_EQ(member_.status(), a11y::ContestMember::Status::kAccepted);
  recognizer_->OnWin();

  EXPECT_TRUE(gesture_won_);
  EXPECT_FALSE(gesture_complete_called_);
  // We should NOT have received any updates during the MOVE events prior to
  // accepting.
  EXPECT_TRUE(gesture_updates_.empty());

  SendPointerEvents(MoveEvents(1, {}, {0, .5f}));
  EXPECT_EQ(gesture_updates_.size(), 10u);

  EXPECT_FALSE(gesture_complete_called_);

  SendPointerEvents(UpEvents(1, {}));

  EXPECT_TRUE(gesture_complete_called_);
}

// Tests the case in which a drag is detected, but then an extra finger is
// placed on screen.
TEST_F(MFingerNTapDragRecognizerTest, ThreeFingerDoubleTapWithDragDetectedExtraFinger) {
  CreateGestureRecognizer(1 /*number of fingers*/, 3 /*number of fingers*/);
  recognizer_->OnContestStarted(member_.TakeInterface());

  SendPointerEvents((DownEvents(1, {}) + UpEvents(1, {}) + DownEvents(1, {}) + UpEvents(1, {}) +
                     DownEvents(1, {}) + MoveEvents(1, {}, {})));

  RunLoopFor(a11y::MFingerNTapDragRecognizer::kMinTapHoldDuration);

  EXPECT_EQ(member_.status(), a11y::ContestMember::Status::kAccepted);
  recognizer_->OnWin();

  EXPECT_TRUE(gesture_won_);
  EXPECT_FALSE(gesture_complete_called_);
  // We should NOT have received any updates during the MOVE events prior to
  // accepting.
  EXPECT_TRUE(gesture_updates_.empty());

  SendPointerEvents(MoveEvents(1, {}, {0, .5f}));
  EXPECT_EQ(gesture_updates_.size(), 10u);

  EXPECT_FALSE(gesture_complete_called_);

  SendPointerEvents(DownEvents(2, {}));

  EXPECT_TRUE(gesture_complete_called_);
}

// Tests the case in which the finger moves too far from its starting location
// during one of the non-drag taps.
TEST_F(MFingerNTapDragRecognizerTest, ThreeFingerDoubleTapWithDragRejectedInvalidTap) {
  CreateGestureRecognizer(1 /*number of fingers*/, 3 /*number of fingers*/);
  recognizer_->OnContestStarted(member_.TakeInterface());

  SendPointerEvents((DownEvents(1, {}) + MoveEvents(1, {}, {1, 1})));

  EXPECT_EQ(member_.status(), a11y::ContestMember::Status::kRejected);
  EXPECT_FALSE(gesture_won_);
  EXPECT_FALSE(gesture_complete_called_);
  EXPECT_TRUE(gesture_updates_.empty());
}

// Tests the case in which the gesture is accepted after the finger moves far from its starting
// position on the last tap.
TEST_F(MFingerNTapDragRecognizerTest, ThreeFingerDoubleTapWithDragAggressiveAccept) {
  CreateGestureRecognizer(1 /*number of fingers*/, 3 /*number of fingers*/);
  recognizer_->OnContestStarted(member_.TakeInterface());

  SendPointerEvents((DownEvents(1, {}) + UpEvents(1, {}) + DownEvents(1, {}) + UpEvents(1, {}) +
                     DownEvents(1, {}) + MoveEvents(1, {}, {0, 0.6})));

  // Once the finger has a displacement of more than .1f from its initial
  // location during the third tap, we should accept.
  EXPECT_EQ(member_.status(), a11y::ContestMember::Status::kAccepted);
}

}  // namespace
}  // namespace accessibility_test
