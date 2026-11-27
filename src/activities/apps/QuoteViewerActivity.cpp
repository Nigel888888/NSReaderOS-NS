#include "QuoteViewerActivity.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <SecureHttpClient.h>

#include <WiFi.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <variant>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "util/AiCredentials.h"
#include "util/AiExplainCache.h"
#include "util/AiExplainClient.h"
#include "util/TaskWatchdog.h"

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

// AI explanation window: sits below the sentence, framed, paginated. The
// sentence keeps roughly the top 40% of the body; the box takes the rest.
constexpr int EXPLAIN_GAP = 12;
// Gap between the "AI 解释" title row and the first body line. The title band
// is the font's real line height plus this gap; a fixed small height made the
// 18pt title overlap the first answer line.
constexpr int EXPLAIN_TITLE_GAP = 10;
constexpr int CARD_INSET_EXPLAIN = 10;
// Longest measurable/drawable span. Wrapped lines stay under the screen width;
// only a pathological unbreakable token is split at this cap.
constexpr size_t MAX_EXPLAIN_LINE_BYTES = 191;

// Returns true when (tx, ty) is inside `r`.
inline bool inRect(int tx, int ty, const Rect& r) {
  return tx >= r.x && tx < r.x + r.width && ty >= r.y && ty < r.y + r.height;
}

// How long an AI request may run before its watchdog abort fires.
constexpr unsigned long AI_REQUEST_DEADLINE_MS = 60000;

}  // namespace

QuoteViewerActivity::QuoteViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         std::vector<QuotesFile::Quote> quotes, int index)
    : Activity("QuoteViewer", renderer, mappedInput),
      quotes(std::move(quotes)),
      index(index) {}

QuoteViewerActivity::~QuoteViewerActivity() = default;

void QuoteViewerActivity::onEnter() {
  Activity::onEnter();
  clampIndex();
  reloadExplanation();
  requestUpdate();
}

void QuoteViewerActivity::onExit() {
  aiSession_.reset();  // release wolfSSL buffers before leaving
  Activity::onExit();
}

void QuoteViewerActivity::clampIndex() {
  if (quotes.empty()) {
    index = 0;
    return;
  }
  if (index < 0) index = 0;
  if (index >= static_cast<int>(quotes.size())) index = static_cast<int>(quotes.size()) - 1;
}

// The framed AI window under the sentence. Shared by layout and render so
// wrapping and painting use exactly the same box.
Rect QuoteViewerActivity::explainRect() const {
  // No cached answer for this sentence: no window at all. A zero rect makes
  // drawExplanation() a no-op and keeps the tap handler from eating page turns.
  if (!hasExplanation()) return Rect{0, 0, 0, 0};
  const auto& metrics = UITheme::getInstance().getMetrics();
  const bool isInverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int contentY = isInverted ? metrics.buttonHintsHeight : 0;
  const int bodyTop = contentY + metrics.topPadding + metrics.headerHeight + NAV_TOP + NAV_HEIGHT + NAV_GAP;
  const int bodyBottom = starButtonRect().y - EXPLAIN_GAP;
  const int sentenceBand = (bodyBottom - bodyTop) * 2 / 5;  // sentence ~40%, explain ~60%
  const int top = bodyTop + sentenceBand + EXPLAIN_GAP;
  const int height = bodyBottom - top;
  return Rect{SIDE_PADDING, top, renderer.getScreenWidth() - 2 * SIDE_PADDING, std::max(0, height)};
}

// Loads the cached explanation for the current sentence. Keyed by quote key so
// re-visiting the same line reuses the stored answer (no request, no cost).
void QuoteViewerActivity::reloadExplanation() {
  explanation.clear();
  explainLines.clear();
  explainPage = 0;
  explainPages = 1;
  loadedExplainKey.clear();
  if (quotes.empty()) return;
  loadedExplainKey = quotes[index].key;
  // The cache is keyed by sentence; AI answers are stored under the sentence
  // when the quote is saved from the dictionary card.
  AiExplainCache::get(quotes[index].sentence, explanation);
  layoutExplanation();
}

// Measures a byte span of `explanation` through a bounded NUL-terminated
// stack buffer — the renderer's text APIs take C strings, not (ptr,len).
int QuoteViewerActivity::measureExplainSpan(int fontId, const char* text, size_t len) const {
  char buf[MAX_EXPLAIN_LINE_BYTES + 1];
  len = std::min(len, MAX_EXPLAIN_LINE_BYTES);
  memcpy(buf, text, len);
  buf[len] = '\0';
  return renderer.getTextAdvanceX(fontId, buf, EpdFontFamily::REGULAR);
}

// Wraps the explanation to the window width and computes pages. Uses the same
// token-based, UTF-8-safe greedy wrap as the dictionary card so CJK answers
// (the Chinese translation DeepSeek appends) lay out correctly.
void QuoteViewerActivity::layoutExplanation() {
  explainLines.clear();
  explainPage = 0;
  explainPages = 1;
  explainLinesPerPage = 1;
  const Rect box = explainRect();
  if (box.height <= 0 || explanation.empty()) return;

  // The explanation is an AI answer carrying Chinese text. The reader font can
  // be a Latin-only SD face with no CJK fallback (tofu boxes), so lay it out in
  // the CJK-capable built-in face. drawExplanation() uses the same font id.
  const int fontId = NOTOSANS_18_FONT_ID;
  renderer.ensureSdCardFontReady(fontId, explanation.c_str(), 0x01 /* REGULAR */);
  const int maxWidth = box.width - 2 * CARD_INSET_EXPLAIN;
  const int lineHeight = renderer.getLineHeight(fontId);
  const int titleH = lineHeight + EXPLAIN_TITLE_GAP;
  const int bodyH = box.height - 2 * CARD_INSET_EXPLAIN - titleH;
  explainLinesPerPage = std::max(1, bodyH / lineHeight);
  const int spaceWidth = renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR);

  const char* text = explanation.c_str();
  const uint32_t n = static_cast<uint32_t>(explanation.size());
  uint32_t lineStart = 0;
  uint32_t lineEnd = 0;  // one past the last token byte on the current line
  int lineWidth = 0;

  const auto flushLine = [&](uint32_t nextStart) {
    explainLines.push_back({lineStart, static_cast<uint16_t>(lineEnd - lineStart)});
    lineStart = nextStart;
    lineEnd = nextStart;
    lineWidth = 0;
  };

  uint32_t i = 0;
  while (i < n) {
    const char c = text[i];
    if (c == '\n' || c == '\0') {
      flushLine(i + 1);
      ++i;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      ++i;
      continue;
    }
    // Token: run of non-whitespace bytes, capped at the measure buffer.
    const uint32_t tokenStart = i;
    while (i < n && text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n' && text[i] != '\0' &&
           i - tokenStart < MAX_EXPLAIN_LINE_BYTES) {
      ++i;
    }
    // Back off a byte cap that cut mid-UTF-8-sequence.
    while (i - tokenStart > 1 && (text[i] & 0xC0) == 0x80) --i;
    const uint32_t tokenLen = i - tokenStart;
    const int tokenWidth = measureExplainSpan(fontId, text + tokenStart, tokenLen);

    if (lineEnd == lineStart) {
      lineStart = tokenStart;
      lineEnd = tokenStart + tokenLen;
      lineWidth = tokenWidth;
    } else if (lineWidth + spaceWidth + tokenWidth <= maxWidth &&
               tokenStart + tokenLen - lineStart <= UINT16_MAX) {
      lineEnd = tokenStart + tokenLen;
      lineWidth += spaceWidth + tokenWidth;
    } else {
      flushLine(tokenStart);
      lineEnd = tokenStart + tokenLen;
      lineWidth = tokenWidth;
    }

    // An unbreakable token wider than the screen: split on a UTF-8 boundary.
    while (lineWidth > maxWidth && lineEnd - lineStart > 1) {
      const uint32_t len = lineEnd - lineStart;
      uint32_t lastFit = 0;
      for (uint32_t f = 1; f <= len; ++f) {
        if (f == len || (text[lineStart + f] & 0xC0) != 0x80) {
          if (measureExplainSpan(fontId, text + lineStart, f) > maxWidth) break;
          lastFit = f;
        }
      }
      if (lastFit == 0) {
        lastFit = 1;
        while (lastFit < len && (text[lineStart + lastFit] & 0xC0) == 0x80) ++lastFit;
      }
      const uint32_t rest = lineStart + lastFit;
      lineEnd = rest;
      flushLine(rest);
      lineEnd = rest + (len - lastFit);
      lineWidth = measureExplainSpan(fontId, text + lineStart, lineEnd - lineStart);
    }
  }
  if (lineEnd > lineStart) flushLine(lineEnd);
  explainPages = std::max(1, (static_cast<int>(explainLines.size()) + explainLinesPerPage - 1) / explainLinesPerPage);
}

// Paints the framed AI window: rounded card, a small title row, the current
// page of wrapped lines and (when paginated) a "p / n" counter.
void QuoteViewerActivity::drawExplanation(const Rect& box) const {
  if (box.width <= 0 || box.height <= 0) return;
  renderer.drawRoundedRect(box.x, box.y, box.width, box.height, 1, 6, true);

  // Match layoutExplanation(): CJK-capable face for the AI answer text.
  const int fontId = NOTOSANS_18_FONT_ID;
  const int lineHeight = renderer.getLineHeight(fontId);
  const int textX = box.x + CARD_INSET_EXPLAIN;
  int textY = box.y + CARD_INSET_EXPLAIN;

  // Title row.
  renderer.drawText(fontId, textX, textY, tr(STR_AI_EXPLAIN), true, EpdFontFamily::BOLD);
  if (explainPages > 1) {
    char pager[24];
    std::snprintf(pager, sizeof(pager), "%d / %d", explainPage + 1, explainPages);
    const int pw = renderer.getTextAdvanceX(fontId, pager, EpdFontFamily::REGULAR);
    renderer.drawText(fontId, box.x + box.width - CARD_INSET_EXPLAIN - pw, textY, pager);
  }
  textY += renderer.getLineHeight(fontId) + EXPLAIN_TITLE_GAP;

  if (explanation.empty() || explainLines.empty()) {
    renderer.drawText(fontId, textX, textY, tr(STR_AI_EXPLAIN_EMPTY));
    return;
  }

  const int first = explainPage * explainLinesPerPage;
  const int last = std::min(first + explainLinesPerPage, static_cast<int>(explainLines.size()));
  char buf[MAX_EXPLAIN_LINE_BYTES + 1];
  for (int li = first; li < last; ++li) {
    const ExplainLine& line = explainLines[li];
    const size_t len = std::min<size_t>(line.len, MAX_EXPLAIN_LINE_BYTES);
    memcpy(buf, explanation.c_str() + line.start, len);
    buf[len] = '\0';
    renderer.drawText(fontId, textX, textY, buf);
    textY += lineHeight;
  }
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
  reloadExplanation();
  message = tr(STR_QUOTES_DELETE);
  messageUntil = millis() + MESSAGE_MS;
  requestUpdate();
}

// The "AI 解释" chip occupies the band the explanation window would take, but
// only when this sentence has no cached answer. It keeps the same footprint as
// the star/delete buttons so the layout does not jump between states.
Rect QuoteViewerActivity::aiButtonRect() const {
  if (quotes.empty() || hasExplanation()) return Rect{0, 0, 0, 0};
  const Rect star = starButtonRect();
  const int w = std::min(ACTION_MAX_WIDTH / 2, renderer.getScreenWidth() - 2 * SIDE_PADDING);
  const int x = star.x + (star.width - w) / 2;
  return Rect{x, star.y - ACTION_HEIGHT - EXPLAIN_GAP, w, ACTION_HEIGHT};
}

void QuoteViewerActivity::drawAiButton() const {
  const Rect rect = aiButtonRect();
  if (rect.width <= 0) return;
  const char* label = aiMessage ? aiMessage : tr(STR_AI_EXPLAIN);
  GUI.drawActionButton(renderer, rect, label, aiMessage != nullptr);
}

void QuoteViewerActivity::ensureAiSession() {
  if (!aiSession_) aiSession_ = makeUniqueNoThrow<freeink::SecureHttpClient>();
}

void QuoteViewerActivity::expireAiMessage() {
  if (aiMessage && static_cast<long>(millis() - aiMessageUntil) >= 0) {
    aiMessage = nullptr;
    requestUpdate();
  }
}

// Tap handler: no cache for this sentence, so go straight to Wi-Fi (auto)
// then queue the network turn for the next loop so the busy popup paints.
void QuoteViewerActivity::startAiExplain() {
  if (aiPhase != AiPhase::Idle || quotes.empty()) return;
  if (!AiCredentials::hasKey()) {
    aiMessage = tr(STR_AI_NO_KEY);
    aiMessageUntil = millis() + MESSAGE_MS;
    requestUpdate();
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    ensureWifiThenAi();
    return;
  }
  aiPhase = AiPhase::Requested;
  requestUpdate();
}

void QuoteViewerActivity::ensureWifiThenAi() {
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput, /*autoConnect=*/true);
  if (!wifi) {
    aiMessage = tr(STR_AI_NO_WIFI);
    aiMessageUntil = millis() + MESSAGE_MS;
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(wifi), [this](const ActivityResult& result) {
    const auto* wifiResult = std::get_if<WifiResult>(&result.data);
    const bool connected = !result.isCancelled && wifiResult && wifiResult->connected;
    if (connected) {
      aiPhase = AiPhase::Requested;
    } else {
      aiMessage = tr(STR_AI_NO_WIFI);
      aiMessageUntil = millis() + MESSAGE_MS;
    }
    requestUpdate();
  });
}

void QuoteViewerActivity::performAiExplain() {
  resetTaskWatchdogIfSubscribed();
  ensureAiSession();
  const unsigned long started = millis();
  const auto keepAlive = [started]() {
    resetTaskWatchdogIfSubscribed();
    return millis() - started >= AI_REQUEST_DEADLINE_MS;
  };
  const std::string sentence = quotes[index].sentence;
  AiExplainClient::Result result = AiExplainClient::explainSentence(sentence, aiSession_.get(), keepAlive);
  resetTaskWatchdogIfSubscribed();
  LOG_INF("AIC", "sentence status=%s http=%d ms=%lu", AiExplainClient::Result::label(result.status),
          result.httpStatus, result.elapsedMs);

  aiPhase = AiPhase::Idle;
  aiSession_.reset();  // free wolfSSL buffers; stacked over the quotes list

  if (result.status == AiExplainClient::Result::Status::Ok) {
    AiExplainCache::put(sentence, result.text);
    reloadExplanation();
    requestUpdate();
    return;
  }
  switch (result.status) {
    case AiExplainClient::Result::Status::NoWifi:
      ensureWifiThenAi();
      return;
    case AiExplainClient::Result::Status::HttpError:
      aiMessage = tr(STR_AI_HTTP_ERROR);
      break;
    default:
      aiMessage = tr(STR_AI_FAILED);
      break;
  }
  aiMessageUntil = millis() + MESSAGE_MS;
  requestUpdate();
}

void QuoteViewerActivity::loop() {
  if (message) {
    if (static_cast<long>(millis() - messageUntil) >= 0) {
      message = nullptr;
      requestUpdate();
    }
  }
  expireAiMessage();

  // AI request state machine: one network turn after the busy popup paints.
  if (aiPhase == AiPhase::Requested) {
    aiPhase = AiPhase::Requesting;
    requestUpdate();
    return;
  }
  if (aiPhase == AiPhase::Requesting) {
    performAiExplain();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // Move to a sentence and re-load its cached explanation (or clear it).
  const auto step = [&](int delta) {
    const int next = index + delta;
    if (next < 0 || next >= static_cast<int>(quotes.size())) return;
    index = next;
    reloadExplanation();
    requestUpdate();
  };
  // Up/Down swipe walks the explanation pages; when it has only one page the
  // gesture falls through to sentence paging below.
  const auto pageExplain = [&](int delta) {
    if (explainPages <= 1) return false;
    const int next = explainPage + delta;
    if (next < 0 || next >= explainPages) return true;  // consumed at the end
    explainPage = next;
    requestUpdate();
    return true;
  };

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    if (pageExplain(1)) return;
  } else if (swipe == MappedInputManager::SwipeDir::Down) {
    if (pageExplain(-1)) return;
  } else if (swipe == MappedInputManager::SwipeDir::Left) {
    step(1);
    return;
  } else if (swipe == MappedInputManager::SwipeDir::Right) {
    step(-1);
    return;
  }

  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    const Rect prev = prevButtonRect();
    if (inRect(tx, ty, prev)) {
      step(-1);
      return;
    }
    const Rect next = nextButtonRect();
    if (inRect(tx, ty, next)) {
      step(1);
      return;
    }
    const Rect star = starButtonRect();
    if (inRect(tx, ty, star)) {
      toggleFavorite();
      return;
    }
    const Rect del = deleteButtonRect();
    if (inRect(tx, ty, del)) {
      deleteCurrent();
      return;
    }
    // "AI 解释" chip for a sentence with no cached answer.
    const Rect aiBtn = aiButtonRect();
    if (inRect(tx, ty, aiBtn)) {
      startAiExplain();
      return;
    }
    // Tapping the explanation window pages it (top half back, bottom half
    // forward), mirroring the swipe gesture.
    const Rect box = explainRect();
    if (inRect(tx, ty, box)) {
      pageExplain(ty < box.y + box.height / 2 ? -1 : 1);
      return;
    }
    // Tapping the left/right margins pages like the reader.
    if (tx < renderer.getScreenWidth() / 3) {
      step(-1);
    } else if (tx >= renderer.getScreenWidth() * 2 / 3) {
      step(1);
    }
    return;
  }

  // Physical Prev/Next step through the sentences (no swipe this frame).
  if (mappedInput.wasPressed(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    step(-1);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Right) ||
             mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    step(1);
  }
}

void QuoteViewerActivity::render(RenderLock&&) {
  renderer.clearScreen();

  // Busy popup while the network turn runs (network never runs on render).
  if (aiPhase == AiPhase::Requesting) {
    GUI.drawPopup(renderer, tr(STR_AI_EXPLAINING));
    renderer.displayBuffer();
    return;
  }

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

  // With an answer the sentence sits in the top band above the window; with no
  // answer it uses the body down to the "AI 解释" chip (which offers to make one).
  const Rect explainBox = explainRect();
  const Rect aiBtn = aiButtonRect();
  const int bodyTop = prev.y + NAV_HEIGHT + NAV_GAP + 8;
  const int sentenceBottom = hasExplanation() ? (explainBox.y - EXPLAIN_GAP)
                                              : (aiBtn.width > 0 ? aiBtn.y - EXPLAIN_GAP
                                                                 : starButtonRect().y - EXPLAIN_GAP);
  if (sentenceBottom > bodyTop) {
    if (!q.book.empty()) {
      UITheme::drawCenteredText(renderer, Rect{contentX, 0, contentWidth, renderer.getScreenHeight()},
                                UI_10_FONT_ID, bodyTop, q.book.c_str());
    }
    const int sentenceTop = bodyTop + renderer.getLineHeight(UI_10_FONT_ID) + 8;
    UITheme::drawCenteredWrappedText(
        renderer, Rect{contentX + SIDE_PADDING, sentenceTop, contentWidth - 2 * SIDE_PADDING,
                       sentenceBottom - sentenceTop},
        NOTOSANS_16_FONT_ID, q.sentence.c_str(), 6, true, EpdFontFamily::REGULAR);
  }
  drawExplanation(explainBox);
  drawAiButton();

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
