#include "QuoteViewerActivity.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace {

// Body text inset, matching the definition card.
constexpr int SIDE_PADDING = 20;

// Prev/next word row, sized as real touch targets.
constexpr int NAV_HEIGHT = 56;
constexpr int NAV_WIDTH = 150;
constexpr int NAV_TOP = 8;
constexpr int NAV_GAP = 12;

// Star/delete row under the body.
constexpr int ACTION_HEIGHT = 60;
constexpr int ACTION_GAP = 12;
constexpr int ACTION_MAX_WIDTH = 560;

// How long an action result stays on screen.
constexpr unsigned long MESSAGE_MS = 2500;

}  // namespace

void QuoteViewerActivity::onEnter() {
  Activity::onEnter();
  clampIndex();
  requestUpdate();
}

void QuoteViewerActivity::clampIndex() {
  if (quotes.empty()) {
    index = 0;
    return;
  }
  if (index < 0) index = 0;
  if (index >= static_cast<int>(quotes.size())) index = static_cast<int>(quotes.size()) - 1;
}

Rect QuoteViewerActivity::prevButtonRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const bool isInverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int contentY = isInverted ? metrics.buttonHintsHeight : 0;
  const int y = contentY + metrics.topPadding + metrics.headerHeight + NAV_TOP;
  return Rect{SIDE_PADDING, y, NAV_WIDTH, NAV_HEIGHT};
}

Rect QuoteViewerActivity::nextButtonRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool isLandscape = orientation == GfxRenderer::Orientation::LandscapeClockwise ||
                           orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutter = isLandscape ? metrics.sideButtonHintsWidth : 0;
  const int contentWidth = renderer.getScreenWidth() - hintGutter;
  const int contentY = isInverted ? metrics.buttonHintsHeight : 0;
  const int y = contentY + metrics.topPadding + metrics.headerHeight + NAV_TOP;
  return Rect{contentWidth - SIDE_PADDING - NAV_WIDTH, y, NAV_WIDTH, NAV_HEIGHT};
}

Rect QuoteViewerActivity::starButtonRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool isLandscape = orientation == GfxRenderer::Orientation::LandscapeClockwise ||
                           orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutter = isLandscape ? metrics.sideButtonHintsWidth : 0;
  const int contentX = orientation == GfxRenderer::Orientation::LandscapeClockwise ? hintGutter : 0;
  const int contentWidth = renderer.getScreenWidth() - hintGutter;
  int width = contentWidth - 2 * SIDE_PADDING;
  if (width > ACTION_MAX_WIDTH) width = ACTION_MAX_WIDTH;
  const int half = (width - ACTION_GAP) / 2;
  const int y = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing - ACTION_HEIGHT;
  return Rect{contentX + (contentWidth - width) / 2, y, half, ACTION_HEIGHT};
}

Rect QuoteViewerActivity::deleteButtonRect() const {
  const Rect star = starButtonRect();
  return Rect{star.x + star.width + ACTION_GAP, star.y, star.width, ACTION_HEIGHT};
}

void QuoteViewerActivity::toggleFavorite() {
  if (quotes.empty()) return;
  const auto& q = quotes[index];
  const bool next = !q.favorite;
  if (!QuotesFile::setFavorite(q.key, next)) {
    message = tr(STR_QUOTES_SAVE_FAILED);
  } else {
    quotes[index].favorite = next;
    message = next ? tr(STR_QUOTES_FAVORITE) : tr(STR_QUOTES_UNFAVORITE);
  }
  messageUntil = millis() + MESSAGE_MS;
  requestUpdate();
}

void QuoteViewerActivity::deleteCurrent() {
  if (quotes.empty()) return;
  const std::string key = quotes[index].key;
  if (!QuotesFile::remove(key)) {
    message = tr(STR_QUOTES_SAVE_FAILED);
    messageUntil = millis() + MESSAGE_MS;
    requestUpdate();
    return;
  }
  quotes.erase(quotes.begin() + index);
  if (quotes.empty()) {
    finish();
    return;
  }
  clampIndex();
  message = tr(STR_QUOTES_DELETE);
  messageUntil = millis() + MESSAGE_MS;
  requestUpdate();
}

void QuoteViewerActivity::loop() {
  if (message) {
    if (static_cast<long>(millis() - messageUntil) >= 0) {
      message = nullptr;
      requestUpdate();
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    const Rect prev = prevButtonRect();
    if (tx >= prev.x && tx < prev.x + prev.width && ty >= prev.y && ty < prev.y + prev.height) {
      if (index > 0) {
        --index;
        requestUpdate();
      }
      return;
    }
    const Rect next = nextButtonRect();
    if (tx >= next.x && tx < next.x + next.width && ty >= next.y && ty < next.y + next.height) {
      if (index + 1 < static_cast<int>(quotes.size())) {
        ++index;
        requestUpdate();
      }
      return;
    }
    const Rect star = starButtonRect();
    if (tx >= star.x && tx < star.x + star.width && ty >= star.y && ty < star.y + star.height) {
      toggleFavorite();
      return;
    }
    const Rect del = deleteButtonRect();
    if (tx >= del.x && tx < del.x + del.width && ty >= del.y && ty < del.y + del.height) {
      deleteCurrent();
      return;
    }
    // Tapping the left/right margins pages like the reader.
    if (tx < renderer.getScreenWidth() / 3) {
      if (index > 0) {
        --index;
        requestUpdate();
      }
    } else if (tx >= renderer.getScreenWidth() * 2 / 3) {
      if (index + 1 < static_cast<int>(quotes.size())) {
        ++index;
        requestUpdate();
      }
    }
    return;
  }

  // Physical Prev/Next step through the sentences.
  if (mappedInput.wasPressed(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (index > 0) {
      --index;
      requestUpdate();
    }
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Right) ||
             mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (index + 1 < static_cast<int>(quotes.size())) {
      ++index;
      requestUpdate();
    }
  }
}

void QuoteViewerActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool isLandscape = orientation == GfxRenderer::Orientation::LandscapeClockwise ||
                           orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutter = isLandscape ? metrics.sideButtonHintsWidth : 0;
  const int contentX = orientation == GfxRenderer::Orientation::LandscapeClockwise ? hintGutter : 0;
  const int contentWidth = renderer.getScreenWidth() - hintGutter;
  const int contentY = isInverted ? metrics.buttonHintsHeight : 0;

  const int headerY = contentY + metrics.topPadding;
  GUI.drawHeader(renderer, Rect{contentX, headerY, contentWidth, metrics.headerHeight}, tr(STR_QUOTES_TITLE));

  if (quotes.empty()) {
    UITheme::drawCenteredText(renderer, Rect{contentX, 0, contentWidth, renderer.getScreenHeight()},
                              UI_12_FONT_ID, headerY + metrics.headerHeight + 40, tr(STR_QUOTES_EMPTY));
    GUI.drawButtonHints(renderer, tr(STR_BACK), "", "", "");
    renderer.displayBuffer();
    return;
  }

  const auto& q = quotes[index];

  // Prev/next row and the "n / N" counter between them.
  const Rect prev = prevButtonRect();
  char prevLabel[48];
  std::snprintf(prevLabel, sizeof(prevLabel), "< %s", tr(STR_QUOTES_PREV));
  GUI.drawActionButton(renderer, prev, prevLabel, index > 0);
  const Rect next = nextButtonRect();
  char nextLabel[48];
  std::snprintf(nextLabel, sizeof(nextLabel), "%s >", tr(STR_QUOTES_NEXT));
  GUI.drawActionButton(renderer, next, nextLabel, index + 1 < static_cast<int>(quotes.size()));
  char counter[24];
  std::snprintf(counter, sizeof(counter), "%d / %d", index + 1, static_cast<int>(quotes.size()));
  const int counterWidth = renderer.getTextWidth(UI_10_FONT_ID, counter);
  const int counterX = (prev.x + prev.width + next.x - counterWidth) / 2;
  const int counterY = prev.y + (NAV_HEIGHT - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
  renderer.drawText(UI_10_FONT_ID, counterX, counterY, counter);

  // The sentence itself, large, centred vertically in the space between the
  // nav row and the action row. The source book sits just above it.
  const int bodyTop = prev.y + NAV_HEIGHT + NAV_GAP + 8;
  const int bodyBottom = starButtonRect().y - 16;
  const int bodyHeight = bodyBottom - bodyTop;
  if (bodyHeight > 0) {
    if (!q.book.empty()) {
      UITheme::drawCenteredText(renderer, Rect{contentX, 0, contentWidth, renderer.getScreenHeight()},
                                UI_10_FONT_ID, bodyTop, q.book.c_str());
    }
    const int sentenceTop = bodyTop + renderer.getLineHeight(UI_10_FONT_ID) + 8;
    UITheme::drawCenteredWrappedText(
        renderer, Rect{contentX + SIDE_PADDING, sentenceTop, contentWidth - 2 * SIDE_PADDING,
                       bodyBottom - sentenceTop},
        NOTOSANS_16_FONT_ID, q.sentence.c_str(), 9, true, EpdFontFamily::REGULAR);
  }

  // Star/delete row. The label takes over with the last action's result.
  const Rect star = starButtonRect();
  const char* starLabel = message ? message
                                  : (q.favorite ? tr(STR_QUOTES_UNFAVORITE) : tr(STR_QUOTES_FAVORITE));
  GUI.drawActionButton(renderer, star, starLabel, message != nullptr);
  GUI.drawActionButton(renderer, deleteButtonRect(), tr(STR_QUOTES_DELETE), false);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_QUOTES_PREV), tr(STR_QUOTES_NEXT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
