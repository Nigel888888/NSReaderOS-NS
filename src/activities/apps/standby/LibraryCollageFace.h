#pragma once
#ifdef RICKYOS_PRODUCT

#include <cstdint>
#include <string>
#include <vector>

#include "StandbyFace.h"

// A full-screen collage of the reader's real book covers, tilted 15 degrees and
// overlapped, with a rounded-corner + soft-shadow treatment and a paper sticker
// title ("My library") / count ("N books"). Ported from the kiikoread collage
// lock screen; rendered through GfxRenderer's native 16-level gray frame so the
// cover shading survives. Degrades to a plain B/W grid when 16-gray is absent.
class LibraryCollageFace final : public StandbyFace {
 public:
  void onEnter() override;
  TickResult tick() override;
  void render(GfxRenderer& renderer, const Rect& viewport) override;
  bool renderNative(GfxRenderer& renderer, const Rect& viewport) override;
  StrId titleId() const override;
  uint32_t secondsUntilNextWake() const override;
  bool wantsClock() const override { return false; }

 private:
  struct CollageBook {
    std::string title;
    std::string author;
    std::string coverPath;
    int srcW = 0;
    int srcH = 0;
  };

  std::vector<CollageBook> books_;
  int totalCount_ = 0;  // total books on the shelf, shown on the sticker
};

#endif
