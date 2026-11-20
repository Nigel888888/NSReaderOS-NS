#pragma once
#ifdef RICKYOS_PRODUCT

#include <cstdint>

#include "StandbyFace.h"

// A torn-paper "reading receipt" lock screen, ported from kiikoread's
// book_ticket.c. Shows the current book (title / author / cover / progress),
// today's and lifetime reading time, and a closing slogan on a jagged-edged
// paper slip. Rendered in B/W (dithered cover shade) via StandbyFace::render().
class ReadingReceiptFace final : public StandbyFace {
 public:
  void onEnter() override;
  TickResult tick() override;
  void render(GfxRenderer& renderer, const Rect& viewport) override;
  StrId titleId() const override;
  uint32_t secondsUntilNextWake() const override;
  bool wantsClock() const override { return false; }

 private:
  int32_t lastDay_ = -1;
};

#endif
