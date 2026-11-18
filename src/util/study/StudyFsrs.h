#pragma once

// FSRS-5, the scheduler Anki runs when "FSRS" is enabled.
//
// Ported from CrossNext (https://github.com/manuelx900/CrossNext), MIT License,
// Copyright (c) 2026 Mario Ruiz (CrossPlay). The algorithm body is kept
// verbatim so the device produces the same intervals as the desktop.
//
// Freestanding C++17: no Arduino, no renderer, no heap.

#include <cstdint>

namespace study {

// Anki's four buttons. The numeric values are Anki's revlog `ease` column.
enum class Rating : uint8_t { Again = 1, Hard = 2, Good = 3, Easy = 4 };

// FSRS-5 takes 19 weights. Anki stores them per deck-preset; this build uses
// the published defaults, since the notebook has no imported Anki deck.
inline constexpr int kNumParams = 19;

// Anki's defaults, used when no optimized parameters are supplied.
extern const float kDefaultParams[kNumParams];

// What FSRS remembers about a card. Mirrors the `{"s":..,"d":..}` blob Anki
// keeps in `cards.data`.
struct Memory {
  float stability = 0.0f;   // days for retrievability to fall to 90%
  float difficulty = 0.0f;  // 1..10
  bool learned = false;     // false until the first review

  bool operator==(const Memory& o) const {
    return learned == o.learned && stability == o.stability && difficulty == o.difficulty;
  }
};

class Fsrs {
 public:
  // `params` must point at kNumParams floats that outlive this object;
  // passing nullptr selects kDefaultParams.
  explicit Fsrs(const float* params = nullptr, float desiredRetention = 0.9f);

  // Probability the card is still recallable `elapsedDays` after its last
  // review. This is the forgetting curve; 1.0 for a card never studied.
  float retrievability(const Memory& m, float elapsedDays) const;

  // Apply a review and return the new memory state.
  //
  // `elapsedDays` is whole days between the previous review's day and this
  // one's -- 0 for a same-day review. It is ignored for a card's first review.
  Memory review(const Memory& m, Rating rating, int elapsedDays) const;

  // Days until the card next falls to the desired retention. This is the
  // number shown on a rating button.
  int intervalDays(const Memory& m) const;

  // Interval each button would produce, for the four labels under a card.
  // `out` receives Again, Hard, Good, Easy in that order.
  void previewIntervals(const Memory& m, int elapsedDays, int out[4]) const;

  float desiredRetention() const { return desiredRetention_; }
  void setDesiredRetention(float r) { desiredRetention_ = r; }

  // Longest interval the scheduler may hand out, in days. Anki's default is
  // 36500 (a century).
  void setMaximumInterval(int days) { maximumInterval_ = days; }

 private:
  float initialStability(Rating r) const;
  float initialDifficulty(Rating r) const;
  float nextDifficulty(float d, Rating r) const;
  float shortTermStability(float s, Rating r) const;
  float recallStability(float d, float s, float retr, Rating r) const;
  float forgetStability(float d, float s, float retr) const;

  const float* w_;
  float desiredRetention_;
  int maximumInterval_ = 36500;
};

}  // namespace study
