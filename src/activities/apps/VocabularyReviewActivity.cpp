#include "VocabularyReviewActivity.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "fontIds.h"

namespace {

// The font the word itself is drawn in.
constexpr int kWordFontId = NOTOSANS_18_FONT_ID;

// The rating grid is a 2x2 set of bordered cards, matching the CrossNext
// flashcard screen: each card carries an icon, the action name and the exact
// scheduling interval for that choice.
constexpr int kRatingCount = 4;
constexpr int kRatingRows = 2;
constexpr int kRatingCols = 2;
constexpr int kCardSideMargin = 14;
// Finger-sized rating cards for the high-DPI Read Pico panel: the old 72 px
// rows with 8 px gaps were cramped. Two 92 px rows + a 14 px gap = 198 px.
constexpr int kCardGap = 14;
constexpr int kCardHeight = 92;

// The label of one rating button.
const char* ratingLabel(const int index) {
  switch (index) {
    case 0:
      return tr(STR_REVIEW_AGAIN);
    case 1:
      return tr(STR_REVIEW_HARD);
    case 2:
      return tr(STR_REVIEW_GOOD);
    default:
      return tr(STR_REVIEW_EASY);
  }
}

}  // namespace

VocabularyReviewActivity::VocabularyReviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("VocabularyReview", renderer, mappedInput) {}

void VocabularyReviewActivity::onEnter() {
  Activity::onEnter();
  buildQueue();
  requestUpdate();
}

// Builds the queue of due words. New cards come first, then the ones whose
// learning step expires soonest.
void VocabularyReviewActivity::buildQueue() {
  entries.clear();
  queue.clear();
  VocabularyFile::load(entries);
  returns.assign(entries.size(), 0);

  const int today = VocabularyFile::currentDayNumber();
  const int minute = VocabularyFile::currentMinuteOfDay();
  const int nowMinute = today * 1440 + minute;

  std::vector<int> due;
  VocabularyFile::collectDue(entries, due, today, minute);
  for (const int index : due) {
    const auto& entryCard = entries[index].card;
    int dueMinute = entryCard.dueDay * 1440 + entryCard.dueMinute;
    // A word that has never been reviewed carries no day of its own; it is
    // due now.
    if (dueMinute < nowMinute) {
      dueMinute = nowMinute;
    }
    queue.push_back(QueuedCard{index, dueMinute});
  }

  position = 0;
  reviewed = 0;
  revealed = false;
  finished = queue.empty();
  if (!finished) {
    loadCurrentCard();
  }
}

// Puts the current card's word and scheduling state on screen.
void VocabularyReviewActivity::loadCurrentCard() {
  const QueuedCard& queued = queue[position];
  const auto& entry = entries[queued.index];
  card = entry.card;
  headword = entry.headword;
  sentence.clear();
  definition.clear();
  revealed = false;
  // The question face carries the original sentence, so load the long text
  // up front instead of waiting for the flip.
  loadCardText();
}

// Reads the current card's sentence and definition from disk. The index only
// holds an excerpt, so the long text is fetched when the card is turned over.
void VocabularyReviewActivity::loadCardText() {
  sentence.clear();
  definition.clear();
  if (headword.empty()) {
    return;
  }
  VocabularyFile::loadText(headword, sentence, definition);
  if (definition.empty() && position < static_cast<int>(queue.size())) {
    // The long text is gone or unreadable: fall back to the excerpt the
    // index still holds.
    definition = entries[queue[position].index].summary;
  }
}

void VocabularyReviewActivity::reveal() {
  if (finished || revealed) {
    return;
  }
  // Text was loaded with the card; flipping only changes what is shown.
  revealed = true;
  requestUpdate();
}

// Applies a rating, writes the new scheduling state back and moves on.
void VocabularyReviewActivity::answer(const study::Rating rating) {
  if (finished || !revealed || position >= static_cast<int>(queue.size())) {
    return;
  }
  const int today = VocabularyFile::currentDayNumber();
  const int minute = VocabularyFile::currentMinuteOfDay();
  const int nowMinute = today * 1440 + minute;

  const study::Fsrs fsrs;
  const study::Scheduler scheduler(fsrs, study::Steps::defaults());
  const study::Outcome outcome = scheduler.answer(card, rating, today, minute);
  card = outcome.card;

  // Which saved word this was, so the file and the in-memory copy stay in step.
  const int entryIndex = queue[position].index;
  VocabularyFile::saveCard(headword, card, static_cast<uint8_t>(rating));
  entries[entryIndex].card = card;
  ++reviewed;

  // The answered card leaves the queue.
  queue.erase(queue.begin() + position);

  // A card that goes back into a learning step comes back in this session
  // once the step expires, the way Anki works through a deck's learning cards
  // before it lets you stop. A longer step is left for the next session, and
  // a word comes back at most once, so a session always ends.
  const bool learning = card.state == static_cast<uint8_t>(study::State::Learning) ||
                        card.state == static_cast<uint8_t>(study::State::Relearning);
  if (learning && outcome.delayMinutes > 0 && outcome.delayMinutes <= 30 &&
      entryIndex < static_cast<int>(returns.size()) && returns[entryIndex] == 0) {
    returns[entryIndex] = 1;
    queue.push_back(QueuedCard{entryIndex, nowMinute + outcome.delayMinutes});
  }

  // The next card is the first one that is due now. A card whose step has not
  // expired yet waits: the session ends and it is picked up next time.
  const auto dueNow = std::find_if(queue.begin(), queue.end(),
                                   [nowMinute](const QueuedCard& queued) { return queued.dueMinute <= nowMinute; });
  if (dueNow == queue.end()) {
    finished = true;
    card = study::CardState();
    headword.clear();
    requestUpdate();
    return;
  }
  position = static_cast<int>(dueNow - queue.begin());
  loadCurrentCard();
  requestUpdate();
}

Rect VocabularyReviewActivity::ratingButtonRect(const int index) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool isLandscape = orientation == GfxRenderer::Orientation::LandscapeClockwise ||
                           orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = isLandscape ? metrics.sideButtonHintsWidth : 0;
  const int contentX = orientation == GfxRenderer::Orientation::LandscapeClockwise ? hintGutterWidth : 0;
  const int contentWidth = renderer.getScreenWidth() - hintGutterWidth;

  const int gridHeight = kRatingRows * kCardHeight + (kRatingRows - 1) * kCardGap;
  const int gridTop =
      isInverted ? metrics.buttonHintsHeight + metrics.verticalSpacing
                 : renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing - gridHeight;
  const int cardWidth = (contentWidth - 2 * kCardSideMargin - (kRatingCols - 1) * kCardGap) / kRatingCols;
  const int col = index % kRatingCols;
  const int row = index / kRatingCols;
  return Rect{contentX + kCardSideMargin + col * (cardWidth + kCardGap), gridTop + row * (kCardHeight + kCardGap),
              cardWidth, kCardHeight};
}

// One CrossNext-style rating card: a hairline box, the action name on top and
// the resulting interval below, separated by a rule.  `preview` is the delay
// string the scheduler computed for this choice (e.g. "1 min" / "6 days").
void VocabularyReviewActivity::drawRatingCard(const Rect& r, const char* label, const char* preview) const {
  if (r.width <= 0 || r.height <= 0) return;
  renderer.clearClipRect();
  renderer.drawRoundedRect(r.x, r.y, r.width, r.height, 2, 4, true);

  const int nameFont = UI_12_FONT_ID;
  const int lineH = renderer.getLineHeight(nameFont);
  const int ruleY = r.y + r.height * 3 / 5;
  // Action name, centred in the upper band.
  const int nameWidth = renderer.getTextWidth(nameFont, label, EpdFontFamily::BOLD);
  renderer.drawText(nameFont, r.x + (r.width - nameWidth) / 2, r.y + (ruleY - r.y - lineH) / 2, label, true,
                    EpdFontFamily::BOLD);
  // Dashed rule splits the two halves. A solid hairline read as a hard
  // boundary; the dotted separator keeps the action and interval visually
  // related but distinct.
  const int ruleLeft = r.x + 6;
  const int ruleRight = r.x + r.width - 6;
  for (int x = ruleLeft; x < ruleRight; x += 8) {
    renderer.drawLine(x, ruleY, std::min(x + 4, ruleRight), ruleY, 1, true);
  }
  // Interval, centred in the lower band.
  const int previewWidth = renderer.getTextWidth(UI_10_FONT_ID, preview);
  renderer.drawText(UI_10_FONT_ID, r.x + (r.width - previewWidth) / 2,
                    ruleY + (r.y + r.height - ruleY - renderer.getLineHeight(UI_10_FONT_ID)) / 2, preview);
}

void VocabularyReviewActivity::drawRatingButtons() const {
  const int today = VocabularyFile::currentDayNumber();
  const int minute = VocabularyFile::currentMinuteOfDay();
  const study::Fsrs fsrs;
  const study::Scheduler scheduler(fsrs, study::Steps::defaults());
  study::Outcome outcomes[kRatingCount];
  scheduler.preview(card, today, minute, outcomes);

  for (int i = 0; i < kRatingCount; ++i) {
    char delay[16];
    study::formatDelay(outcomes[i].delayMinutes, outcomes[i].intervalDays, delay, sizeof(delay));
    drawRatingCard(ratingButtonRect(i), ratingLabel(i), delay);
  }
}

void VocabularyReviewActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (finished) {
    return;
  }

  // The rating buttons are the only thing on the answer face that acts.
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    if (revealed) {
      for (int i = 0; i < kRatingCount; ++i) {
        const Rect button = ratingButtonRect(i);
        if (button.width > 0 && button.height > 0 && tx >= button.x && tx < button.x + button.width && ty >= button.y &&
            ty < button.y + button.height) {
          answer(static_cast<study::Rating>(i + 1));
          return;
        }
      }
    }
    // Anywhere else turns the card over.
    reveal();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    reveal();
  }
}

void VocabularyReviewActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? metrics.sideButtonHintsWidth : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = renderer.getScreenWidth() - hintGutterWidth;
  const int contentY = isInverted ? metrics.buttonHintsHeight : 0;

  const int top = contentY + metrics.topPadding + metrics.headerHeight;

  // The standard header, like every other screen: title, clock and battery.
  GUI.drawHeader(renderer, Rect{0, contentY + metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_REVIEW_TITLE));

  if (finished) {
    // The session is over. Say how much was done, and that the rest can wait.
    const char* headline = reviewed > 0 ? tr(STR_REVIEW_DONE) : tr(STR_REVIEW_EMPTY);
    UITheme::drawCenteredText(renderer, Rect{contentX, 0, contentWidth, renderer.getScreenHeight()}, UI_12_FONT_ID,
                              top + 40, headline);
    if (reviewed > 0) {
      char summary[64];
      std::snprintf(summary, sizeof(summary), "%s %d", tr(STR_REVIEW_REVIEWED), reviewed);
      UITheme::drawCenteredText(renderer, Rect{contentX, 0, contentWidth, renderer.getScreenHeight()}, UI_12_FONT_ID,
                                top + 80, summary);
      // Words still inside a learning step are due later, not never.
      if (!queue.empty()) {
        UITheme::drawCenteredText(renderer, Rect{contentX, 0, contentWidth, renderer.getScreenHeight()}, UI_10_FONT_ID,
                                  top + 120, tr(STR_REVIEW_LATER));
      }
    }
  } else {
    // How many cards are still to come, under the header on the right.
    char left[48];
    std::snprintf(left, sizeof(left), "%s %d", tr(STR_REVIEW_LEFT), static_cast<int>(queue.size()));
    const int leftWidth = renderer.getTextWidth(UI_10_FONT_ID, left);
    renderer.drawText(UI_10_FONT_ID, contentX + contentWidth - 20 - leftWidth, top + 4, left);

    // The word, large, in the middle of the upper half.
    const int wordTop = top + 34;
    const int wordHeight = 120;
    if (card.isNew()) {
      // A word that has never been reviewed says so: the first answer sets
      // its memory, and nothing before that does.
      UITheme::drawCenteredText(renderer, Rect{contentX, 0, contentWidth, renderer.getScreenHeight()}, UI_10_FONT_ID,
                                wordTop - 22, tr(STR_REVIEW_NEW));
    }
    UITheme::drawCenteredWrappedText(renderer, Rect{contentX + 20, wordTop, contentWidth - 40, wordHeight}, kWordFontId,
                                     headword.c_str(), 3, true, EpdFontFamily::BOLD);

    const int bodyTop = wordTop + wordHeight + 12;
    const int bodyBottom = ratingButtonRect(0).y - 16;
    if (!revealed) {
      // Question face: the word and the sentence it was collected from.
      if (!sentence.empty() && bodyBottom - bodyTop > 24) {
        UITheme::drawCenteredWrappedText(
            renderer, Rect{contentX + 20, bodyTop, contentWidth - 40, bodyBottom - bodyTop - 24}, UI_12_FONT_ID,
            sentence.c_str(), 6);
      }
      UITheme::drawCenteredText(renderer, Rect{contentX, 0, contentWidth, renderer.getScreenHeight()}, UI_10_FONT_ID,
                                bodyBottom - 20, tr(STR_REVIEW_TAP_TO_ANSWER));
    } else {
      // Answer face: the definition only; the original sentence stays on the
      // front so the two sides have one job each.
      if (!definition.empty() && bodyBottom > bodyTop) {
        UITheme::drawCenteredWrappedText(renderer, Rect{contentX + 20, bodyTop, contentWidth - 40, bodyBottom - bodyTop},
                                         UI_12_FONT_ID, definition.c_str(), 10);
      }
      drawRatingButtons();
    }
  }

  // The key hints stay unchanged: no key adds a word, the button does.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), revealed ? "" : tr(STR_REVIEW_SHOW), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
