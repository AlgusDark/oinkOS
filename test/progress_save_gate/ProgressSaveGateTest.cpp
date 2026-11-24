#include <gtest/gtest.h>

#include "src/activities/reader/ProgressSaveGate.h"

namespace {
constexpr uint32_t kInterval = ProgressSaveGate::intervalMs;
}

TEST(ProgressSaveGateTest, FirstPositionWritesNow) {
  ProgressSaveGate gate;
  EXPECT_TRUE(gate.note({0, 0, 8}));
  EXPECT_TRUE(gate.hasPending);
}

TEST(ProgressSaveGateTest, SavedPositionIsNotPending) {
  ProgressSaveGate gate;
  gate.note({2, 4, 40});
  gate.written(1000);
  EXPECT_FALSE(gate.note({2, 4, 40}));
  EXPECT_FALSE(gate.hasPending);
  EXPECT_FALSE(gate.due(1000 + kInterval));
}

TEST(ProgressSaveGateTest, SameChapterWaitsForTheInterval) {
  ProgressSaveGate gate;
  gate.note({2, 4, 40});
  gate.written(1000);

  EXPECT_FALSE(gate.note({2, 5, 40}));
  EXPECT_FALSE(gate.due(1000 + kInterval - 1));
  EXPECT_TRUE(gate.due(1000 + kInterval));
}

TEST(ProgressSaveGateTest, NewChapterWritesNow) {
  ProgressSaveGate gate;
  gate.note({2, 9, 40});
  gate.written(1000);
  EXPECT_TRUE(gate.note({3, 0, 12}));
}

TEST(ProgressSaveGateTest, FailedWriteInNewChapterWaitsBeforeRetry) {
  ProgressSaveGate gate;
  gate.note({2, 9, 40});
  gate.written(1000);

  EXPECT_TRUE(gate.note({3, 0, 12}));
  gate.attempted(2000);

  EXPECT_FALSE(gate.note({3, 0, 12}));
  EXPECT_FALSE(gate.note({3, 1, 12}));
  EXPECT_TRUE(gate.hasPending);
  EXPECT_FALSE(gate.due(2000 + kInterval - 1));
  EXPECT_TRUE(gate.due(2000 + kInterval));
}

TEST(ProgressSaveGateTest, ReenteringFailedChapterFromSavedPositionWritesNow) {
  ProgressSaveGate gate;
  gate.note({2, 9, 40});
  gate.written(1000);

  EXPECT_TRUE(gate.note({3, 0, 12}));
  gate.attempted(2000);

  EXPECT_FALSE(gate.note({2, 9, 40}));
  EXPECT_TRUE(gate.note({3, 0, 12}));
}

TEST(ProgressSaveGateTest, ReturningToSavedPositionClearsPending) {
  ProgressSaveGate gate;
  gate.note({1, 3, 9});
  gate.written(0);
  gate.note({1, 4, 9});
  EXPECT_TRUE(gate.hasPending);
  gate.note({1, 3, 9});
  EXPECT_FALSE(gate.hasPending);
}

TEST(ProgressSaveGateTest, ForgetSavedMakesUnchangedPositionPending) {
  ProgressSaveGate gate;
  gate.note({1, 3, 9});
  gate.written(0);
  gate.forgetSaved();
  gate.note({1, 3, 9});
  EXPECT_TRUE(gate.hasPending);
  EXPECT_EQ(gate.pending, (ProgressPosition{1, 3, 9}));
}

TEST(ProgressSaveGateTest, ClockWrapUsesUnsignedElapsed) {
  ProgressSaveGate gate;
  constexpr uint32_t last = 0xFFFFF000u;
  gate.note({1, 2, 3});
  gate.written(last);
  gate.note({1, 3, 3});
  EXPECT_FALSE(gate.due(last + kInterval - 1));
  EXPECT_TRUE(gate.due(last + kInterval));
}
