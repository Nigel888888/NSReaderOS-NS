#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/Rect.h"
#include "util/QuotesFile.h"

namespace freeink {
class SecureHttpClient;
}

// Full-screen, paged reader for the highlighted sentences. Opened from the
// Quotes notebook: one sentence fills the screen, Prev/Next step through the
// whole list (touch buttons, left/right screen zones or the physical buttons),
// and Star/Delete act on the sentence in place. Back returns to the notebook.
class QuoteViewerActivity final : public Activity {
 public:
  // Out-of-line ctor/dtor: the class holds a unique_ptr<SecureHttpClient> and
  // both need that forward-declared type to be complete at instantiation.
  explicit QuoteViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                               std::vector<QuotesFile::Quote> quotes, int index);
  ~QuoteViewerActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Paging a sentence and the in-place AI-explain request burst must not trip
  // the global clean cadence into a full flash mid-action.
  bool skipsFullRefreshCadence() const override { return true; }

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
  // True only when this sentence has a cached AI answer. Sentences that were
  // never explained show no window at all -- the sentence fills the body.
  bool hasExplanation() const { return !explanation.empty(); }
  Rect explainRect() const;
  void reloadExplanation();
  void layoutExplanation();
  // Measures a byte span of `explanation` without a per-line heap copy.
  int measureExplainSpan(int fontId, const char* text, size_t len) const;
  void drawExplanation(const Rect& box) const;
  void toggleFavorite();
  void deleteCurrent();

  // "AI 解释" button shown only for sentences with no cached answer. Lets the
  // user collect a sentence first (e.g. a whole RSS line no word was picked
  // from) and generate its explanation here, offline-cacheable, then reload.
  Rect aiButtonRect() const;
  void drawAiButton() const;
  void startAiExplain();
  void performAiExplain();
  void ensureWifiThenAi();
  void ensureAiSession();
  void expireAiMessage();

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

  // AI-explain request state (mirrors the dictionary card). Network runs in
  // loop(); the render task only paints a busy popup.
  enum class AiPhase : uint8_t { Idle, Requested, Requesting };
  AiPhase aiPhase = AiPhase::Idle;
  const char* aiMessage = nullptr;
  unsigned long aiMessageUntil = 0;
  std::unique_ptr<freeink::SecureHttpClient> aiSession_;
};
