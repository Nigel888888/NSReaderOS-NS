#pragma once

// Learning steps: the part of Anki that is not FSRS.
//
// Ported from CrossNext (https://github.com/manuelx900/CrossNext), MIT License,
// Copyright (c) 2026 Mario Ruiz (CrossPlay). The scheduling rules are kept
// verbatim so the device produces the same intervals as the desktop.
//
// FSRS answers "how many days until this should come back". It does not answer
// "what happens in the next ten minutes", and that is a separate machine Anki
// runs on top: a new card walks a list of short steps (1 minute then 10
// minutes) before it graduates to day-scale intervals, and a lapsed card walks
// a shorter list (10 minutes) before it goes back.
//
// Without this the Again button on a mature card offered *fifteen days*, which
// is the interval the card gets after it has been relearned -- not when you
// will actually see it again.
//
// Freestanding C++17, like StudyFsrs, so nothing here depends on the device.

#include <cstdint>

#include "StudyFsrs.h"

namespace study {

// Anki allows an arbitrary number of steps; six is well past what anyone uses
// and keeps Steps a fixed-size struct.
inline constexpr int kMaxSteps = 6;

// The four card states, matching Anki's own numbering and cards.dat's `state`.
enum class State : uint8_t {
  New = 0,
  Learning = 1,
  Review = 2,
  Relearning = 3,
  // Ours, with no Anki equivalent: a card Anki has suspended or buried. Kept
  // in the notebook rather than dropped so nothing shifts underneath the
  // saved list, and never shown.
  Suspended = 4,
};

struct Steps {
  float learn[kMaxSteps] = {};    // minutes
  float relearn[kMaxSteps] = {};  // minutes
  uint8_t learnCount = 0;
  uint8_t relearnCount = 0;

  // Anki's defaults, for a notebook that ships none.
  static Steps defaults();
};

// One card's scheduling state. Mirrors the 32-byte record in Anki's cards.dat;
// the ankiCardId is carried for format compatibility and is unused here.
struct CardState {
  int64_t ankiCardId = 0;
  float stability = 0.0f;
  float difficulty = 0.0f;
  int32_t dueDay = 0;
  int32_t lastReviewDay = -1;
  uint16_t reps = 0;
  uint16_t lapses = 0;
  uint8_t state = 0;      // 0 new, 1 learning, 2 review, 3 relearning
  uint8_t stepIndex = 0;  // position in the learning or relearning step list
  // Minutes since dueDay began. Only meaningful while the card is inside a step
  // list, where "come back in ten minutes" cannot be said in day numbering.
  uint16_t dueMinute = 0;

  bool isNew() const { return state == 0; }
  Memory memory() const;
  void setMemory(const Memory& m);
};

// What one answer produces.
struct Outcome {
  CardState card;            // the card's new state, ready to store
  int32_t delayMinutes = 0;  // >0: comes back this session, in this many minutes
  int32_t intervalDays = 0;  // >0: leaves for a future day
  bool graduated = false;    // crossed from a step list into day-scale review
};

class Scheduler {
 public:
  Scheduler(const Fsrs& fsrs, const Steps& steps) : fsrs_(&fsrs), steps_(steps) {}

  // Apply a rating. `today` is the notebook's day number and `nowMinute` is
  // minutes since that day began, which together give a learning card a due
  // instant finer than a day without needing a second clock.
  Outcome answer(const CardState& card, Rating rating, int today, int nowMinute) const;

  // What each of the four buttons would do, for the labels under them.
  void preview(const CardState& card, int today, int nowMinute, Outcome out[4]) const;

  // Is this card due at this instant?
  static bool isDue(const CardState& card, int today, int nowMinute);

 private:
  const Fsrs* fsrs_;
  Steps steps_;
};

// Render a delay as Anki renders it on a button: minutes and hours below a day,
// then days, months, years. `out` must have room for at least 12 bytes.
void formatDelay(int minutes, int days, char* out, size_t size);

}  // namespace study
