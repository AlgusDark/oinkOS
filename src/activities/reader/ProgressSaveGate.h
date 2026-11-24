#pragma once

#include <cstdint>

struct ProgressPosition {
  int spineIndex = -1;
  int page = -1;
  int pageCount = -1;

  constexpr bool operator==(const ProgressPosition& other) const {
    return spineIndex == other.spineIndex && page == other.page && pageCount == other.pageCount;
  }
};

// What progress.bin holds and what is waiting to be written. A position in a
// chapter not yet attempted writes at once; anything else waits intervalMs
// after the last attempt, so a failing card is not retried on every render.
struct ProgressSaveGate {
  static constexpr uint32_t intervalMs = 15000;

  ProgressPosition saved;
  ProgressPosition pending;
  bool hasPending = false;
  int attemptedSpineIndex = -1;
  uint32_t lastAttemptMs = 0;

  // Returns true when the position should be written now.
  constexpr bool note(const ProgressPosition& position) {
    hasPending = !(position == saved);
    if (!hasPending) {
      attemptedSpineIndex = saved.spineIndex;
      return false;
    }
    pending = position;
    return position.spineIndex != attemptedSpineIndex;
  }

  constexpr bool due(const uint32_t nowMs) const {
    return hasPending && static_cast<uint32_t>(nowMs - lastAttemptMs) >= intervalMs;
  }

  constexpr void attempted(const uint32_t nowMs) {
    attemptedSpineIndex = pending.spineIndex;
    lastAttemptMs = nowMs;
  }

  constexpr void written(const uint32_t nowMs) {
    attempted(nowMs);
    saved = pending;
    hasPending = false;
  }

  // The card lost progress.bin, so the next note() must write even an unchanged position.
  constexpr void forgetSaved() { saved = {}; }
};
