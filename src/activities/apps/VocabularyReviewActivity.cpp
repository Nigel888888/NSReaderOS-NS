#include "VocabularyReviewActivity.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "fontIds.h"

namespace {

// The font the word itself is drawn in.
constexpr int kWordFontId = NOTOSANS_18_FONT_ID;

// Gap between the rating buttons and the screen edge.
constexpr int kButtonSideMargin = 20;
constexpr int kButtonGap = 8;
constexpr int kButtonHeight = 58;

// The four buttons, in the order Anki puts them in.
constexpr int kRatingCount = 4;

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
    const auto& card = entries[index].card;
    int dueMinute = card.dueDay * 1440 + card.dueMinute;
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
  loadCardText();
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
  if (learning && outcome.delayMinutes > 0 && outcome.delayMinutes <= 30 && entryIndex < static_cast<int>(returns.size()) &&
      returns[entryIndex] == 0) {
    returns[entryIndex] = 1;
    queue.push_back(QueuedCard{entryIndex, nowMinute + outcome.delayMinutes});
  }

  // The next card is the first one that is due now. A card whose step has not
  // expired yet waits: the session ends and it is picked up next time.
  int next = -1;
  for (size_t i = 0; i < queue.size(); ++i) {
    if (queue[i].dueMinute <= nowMinute) {
      next = static_cast<int>(i);
      break;
    }
  }
  if (next < 0) {
    finished = true;
    card = study::CardState();
    headword.clear();
    requestUpdate();
    return;
  }
  position = next;
  loadCurrentCard();
  requestUpdate();
}

Rect VocabularyReviewActivity::ratingButtonRect(const int index) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? metrics.sideButtonHintsWidth : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = renderer.getScreenWidth() - hintGutterWidth;
  const int buttonWidth = (contentWidth - 2 * kButtonSideMargin - (kRatingCount - 1) * kButtonGap) / kRatingCount;
  // The button hints sit on the bottom edge, except upside down, where they sit
  // on the top edge and the buttons follow them.
  const int buttonTop = isInverted
                            ? metrics.buttonHintsHeight + metrics.verticalSpacing
                            : renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing -
                                  kButtonHeight;
  return Rect{contentX + kButtonSideMargin + index * (buttonWidth + kButtonGap), buttonTop, buttonWidth, kButtonHeight};
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
    // The button carries the rating and what it does to the schedule: the
    // interval is what makes the four choices different.
    std::string label = ratingLabel(i);
    label += " ";
    label += delay;
    GUI.drawActionButton(renderer, ratingButtonRect(i), label.c_str(), false);
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
        if (button.width > 0 && button.height > 0 && tx >= button.x && tx < button.x + button.width &&
            ty >= button.y && ty < button.y + button.height) {
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

    if (!revealed) {
      // The question face: the word on its own. Tap anywhere to turn it over.
      UITheme::drawCenteredText(renderer, Rect{contentX, 0, contentWidth, renderer.getScreenHeight()}, UI_10_FONT_ID,
                                wordTop + wordHeight + 40, tr(STR_REVIEW_TAP_TO_ANSWER));
    } else {
      // The answer face: the definition, the line it was collected from, and
      // the four buttons that schedule the next time the word comes back.
      const int definitionTop = wordTop + wordHeight;
      const int definitionBottom = ratingButtonRect(0).y - 16;
      const int definitionHeight = definitionBottom - definitionTop;
      if (definitionHeight > 0) {
        // The definition takes the upper part, the line it came from the rest.
        const int textHeight = definitionHeight * 3 / 5;
        if (!definition.empty()) {
          UITheme::drawCenteredWrappedText(renderer, Rect{contentX + 20, definitionTop, contentWidth - 40, textHeight},
                                           UI_12_FONT_ID, definition.c_str(), 10);
        }
        if (!sentence.empty()) {
          UITheme::drawCenteredWrappedText(renderer,
                                           Rect{contentX + 20, definitionTop + textHeight, contentWidth - 40,
                                                definitionHeight - textHeight},
                                           UI_10_FONT_ID, sentence.c_str(), 5);
        }
      }
      drawRatingButtons();
    }
  }

  // The key hints stay unchanged: no key adds a word, the button does.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), revealed ? "" : tr(STR_REVIEW_SHOW), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
