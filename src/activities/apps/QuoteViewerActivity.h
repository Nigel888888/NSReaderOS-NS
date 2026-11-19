#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/Rect.h"
#include "util/QuotesFile.h"

// Full-screen, paged reader for the highlighted sentences. Opened from the
// Quotes notebook: one sentence fills the screen, Prev/Next step through the
// whole list (touch buttons, left/right screen zones or the physical buttons),
// and Star/Delete act on the sentence in place. Back returns to the notebook.
class QuoteViewerActivity final : public Activity {
 public:
  explicit QuoteViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                               std::vector<QuotesFile::Quote> quotes, int index)
      : Activity("QuoteViewer", renderer, mappedInput),
        quotes(std::move(quotes)),
        index(index) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void clampIndex();
  Rect prevButtonRect() const;
  Rect nextButtonRect() const;
  Rect starButtonRect() const;
  Rect deleteButtonRect() const;
  void toggleFavorite();
  void deleteCurrent();

  std::vector<QuotesFile::Quote> quotes;
  int index = 0;
  const char* message = nullptr;
  unsigned long messageUntil = 0;
};
