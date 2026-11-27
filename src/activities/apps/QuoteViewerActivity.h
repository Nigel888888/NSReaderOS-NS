#pragma once

#include <cstddef>
#include <cstdint>
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
  // AI explanation window (bottom half). The sentence keeps the top; the
  // explanation lives in its own framed, paginated box below it.
  struct ExplainLine {
    uint32_t start;
    uint16_t len;
  };

  void clampIndex();
  Rect prevButtonRect() const;
  Rect nextButtonRect() const;
  Rect starButtonRect() const;
  Rect deleteButtonRect() const;
  Rect explainRect() const;
  void reloadExplanation();
  void layoutExplanation();
  // Measures a byte span of `explanation` without a per-line heap copy.
  int measureExplainSpan(int fontId, const char* text, size_t len) const;
  void drawExplanation(const Rect& box) const;
  void toggleFavorite();
  void deleteCurrent();

  std::vector<QuotesFile::Quote> quotes;
  int index = 0;
  const char* message = nullptr;
  unsigned long messageUntil = 0;

  // AI explanation of the current sentence, loaded from the on-device cache.
  std::string explanation;                  // raw answer (spans point into this)
  std::vector<ExplainLine> explainLines;    // wrapped lines
  int explainPage = 0;
  int explainPages = 1;
  int explainLinesPerPage = 1;
  std::string loadedExplainKey;             // quote key the cache was loaded for
};
