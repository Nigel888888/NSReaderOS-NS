#include "DictionaryDefinitionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <SecureHttpClient.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>

#include "AiCredentials.h"
#include "AiExplainClient.h"
#include "CrossPointSettings.h"
#include "Memory.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TaskWatchdog.h"
#include "util/DictHtmlPages.h"
#include "util/HtmlToPlainText.h"
#include "util/QuotesFile.h"
#include "util/VocabularyFile.h"

namespace {

// Longest measurable/drawable span. Wrapped lines stay under the screen width
// (far below this); only pathological unbreakable tokens are split at this cap.
constexpr size_t MAX_LINE_BYTES = 191;

// Body text left/right inset, matching the reader's default feel.
constexpr int SIDE_PADDING = 20;

// Styled-path ceiling: the laid-out Pages keep the whole definition resident
// (TextBlock arenas ≈ text + ~7 bytes/word plus per-line objects), roughly
// doubling the string's footprint while this activity is stacked over the
// reader and word-select. Bigger definitions take the span-based plain-text
// path, which holds no per-page copies.
constexpr size_t MAX_STYLED_HTML_BYTES = 16 * 1024;

// Gap between the word row and the framed definition card below it.
constexpr int VOCABULARY_BUTTON_GAP = 12;

// In the word row the action sits as a compact, centered chip; the headword
// is drawn to its left and the page counter to its right.
constexpr int HEADER_ACTION_WIDTH = 176;
constexpr int HEADER_ACTION_HEIGHT = 52;

// How long the result of an add stays on the button.
constexpr unsigned long VOCABULARY_MESSAGE_MS = 2500;

// AI explain: the top row can hold up to three chips (save, quote, AI). They
// share the available width so three never overflow the word row.
constexpr int AI_CHIP_WIDTH = 108;
constexpr int AI_ACTION_GAP = 10;
// Backstop for one AI request; comfortably above the library read timeout.
constexpr unsigned long AI_REQUEST_DEADLINE_MS = 60000;

// Prev/next-word buttons anchored at the bottom of the card while browsing the
// vocabulary notebook. Sized as full touch targets, not tiny glyphs.
constexpr int WORD_NAV_HEIGHT = 48;
constexpr int WORD_NAV_WIDTH = 130;
constexpr int WORD_NAV_GAP = 12;

// CrossNext card chrome: hairline separators and rounded card radius.
constexpr int CARD_RADIUS = 6;
constexpr int CARD_INSET = 10;

// Draws a thin horizontal rule (a CrossNext "hairline") across the content
// width, inset to match the rest of the card.
void drawHairline(GfxRenderer& renderer, int x, int y, int width) {
  renderer.drawLine(x, y, x + width - 1, y, 1, true);
}

}  // namespace

void DictionaryDefinitionActivity::onEnter() {
  Activity::onEnter();
  layoutCurrentWord();
  requestUpdate();
}

// Lays out whatever word/text is currently held. Shared by onEnter() and
// showWordAt() so navigating to another saved word reuses the same pipeline.
void DictionaryDefinitionActivity::layoutCurrentWord() {
  pages.clear();
  lines.clear();
  currentPage = 0;
  totalPages = 1;
  wordSaved = VocabularyFile::contains(headword);
  // Normalize StarDict multi-type separators so the wrap loop and the
  // C-string font APIs below both see the whole definition.
  std::replace(definition.begin(), definition.end(), '\0', '\n');
  // Plain-text copy for the vocabulary notebook. The styled path below
  // consumes `definition` (the HTML source) and frees it, so the readable
  // text has to be captured before that happens.
  std::string plain = htmlDefinition ? htmlToPlainText(definition) : std::string();
  if (!(htmlDefinition && definition.size() <= MAX_STYLED_HTML_BYTES && layoutHtmlPages())) {
    definition = htmlToPlainText(definition);
    // What ends up on screen is what the notebook stores.
    plain = definition;
    wrapText();
  }
  plainDefinition = std::move(plain);
}

void DictionaryDefinitionActivity::onExit() {
  aiSession_.reset();  // release wolfSSL buffers before leaving
  Activity::onExit();
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->releaseSdFontCaches();
  }
}

namespace {
// Gap between the two action chips when both are shown.
constexpr int ACTION_BUTTON_GAP = 10;
}  // namespace

int DictionaryDefinitionActivity::wordRowY() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const bool isInverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  return (isInverted ? metrics.buttonHintsHeight : 0) + metrics.topPadding;
}

int DictionaryDefinitionActivity::wordRowHeight() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Tall enough to hold the centered action chip with a little air.
  return std::max(metrics.headerHeight, HEADER_ACTION_HEIGHT + 8);
}

DictionaryDefinitionActivity::BodyArea DictionaryDefinitionActivity::bodyArea() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool isLandscape = orientation == GfxRenderer::Orientation::LandscapeClockwise ||
                           orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const int hintGutterWidth = isLandscape ? metrics.sideButtonHintsWidth : 0;
  const int bottomArea = metrics.buttonHintsHeight + metrics.verticalSpacing;
  // The word row holds the headword, the centered save chip and the page
  // counter; the framed definition sits below it and paging is anchored at
  // the bottom. Only the bottom pager is reserved from the body height.
  const int topArea = wordRowY() + wordRowHeight();
  const int navReserve = hasWordNavigation() ? (WORD_NAV_HEIGHT + WORD_NAV_GAP) : 0;
  // Reader lookups reserve the bottom row for the "AI explain" button.
  const int aiReserve = (!hasWordNavigation()) ? (WORD_NAV_HEIGHT + WORD_NAV_GAP) : 0;
  const int bodyHeight =
      renderer.getScreenHeight() - topArea - bottomArea - navReserve - aiReserve - VOCABULARY_BUTTON_GAP;
  return {renderer.getScreenWidth() - hintGutterWidth - 2 * SIDE_PADDING, bodyHeight};
}

// The AI explain button occupies the bottom row for reader lookups, mirroring
// the notebook's prev/next band. Zero-width in the notebook (no AI there).
Rect DictionaryDefinitionActivity::aiButtonRect() const {
  if (hasWordNavigation()) return Rect{0, 0, 0, 0};
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = std::min(240, renderer.getScreenWidth() - 2 * SIDE_PADDING);
  const int x = (renderer.getScreenWidth() - width) / 2;
  return Rect{x, wordNavY(), width, WORD_NAV_HEIGHT};
}

// The save / un-favorite chip is centered in the word row, between the
// headword (left) and the page counter (right).
Rect DictionaryDefinitionActivity::vocabularyButtonRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? metrics.sideButtonHintsWidth : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = renderer.getScreenWidth() - hintGutterWidth;
  const int chipY = wordRowY() + (wordRowHeight() - HEADER_ACTION_HEIGHT) / 2;
  const bool split = showQuoteButton && !sentence.empty();
  const int groupWidth = split ? 2 * HEADER_ACTION_WIDTH + ACTION_BUTTON_GAP : HEADER_ACTION_WIDTH;
  const int groupX = contentX + (contentWidth - groupWidth) / 2;
  return Rect{groupX, chipY, HEADER_ACTION_WIDTH, HEADER_ACTION_HEIGHT};
}

Rect DictionaryDefinitionActivity::quoteButtonRect() const {
  const Rect vocab = vocabularyButtonRect();
  const bool split = showQuoteButton && !sentence.empty();
  if (!split) return Rect{0, 0, 0, 0};
  return Rect{vocab.x + vocab.width + ACTION_BUTTON_GAP, vocab.y, vocab.width, vocab.height};
}

// Prev/next-word paging is anchored at the bottom of the card, above the key
// hints -- the CrossNext pager pattern. Both buttons share one baseline.
int DictionaryDefinitionActivity::wordNavY() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing - WORD_NAV_HEIGHT;
}

Rect DictionaryDefinitionActivity::prevWordButtonRect() const {
  if (!hasWordNavigation()) return Rect{0, 0, 0, 0};
  return Rect{SIDE_PADDING, wordNavY(), WORD_NAV_WIDTH, WORD_NAV_HEIGHT};
}

Rect DictionaryDefinitionActivity::nextWordButtonRect() const {
  if (!hasWordNavigation()) return Rect{0, 0, 0, 0};
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? metrics.sideButtonHintsWidth : 0;
  const int contentWidth = renderer.getScreenWidth() - hintGutterWidth;
  return Rect{contentWidth - SIDE_PADDING - WORD_NAV_WIDTH, wordNavY(), WORD_NAV_WIDTH, WORD_NAV_HEIGHT};
}

// Draws the prev/next-word row and the "index / total" counter between them.
// Only shown while browsing the vocabulary notebook; a plain reader lookup
// has no word list and draws nothing here.
void DictionaryDefinitionActivity::drawWordNavigation() const {
  if (!hasWordNavigation()) return;
  const Rect prev = prevWordButtonRect();
  const Rect next = nextWordButtonRect();
  const int total = static_cast<int>(navWords.size());

  // Just the arrows: the word labels ("previous/next word") made the pager
  // noisy. The boxes keep their size and the counter still shows position.
  GUI.drawActionButton(renderer, prev, "<", navIndex > 0);
  GUI.drawActionButton(renderer, next, ">", navIndex + 1 < total);

  char counter[24];
  std::snprintf(counter, sizeof(counter), "%d / %d", navIndex + 1, total);
  const int counterWidth = renderer.getTextWidth(UI_10_FONT_ID, counter);
  const int counterX = (prev.x + prev.width + next.x - counterWidth) / 2;
  const int counterY = prev.y + (WORD_NAV_HEIGHT - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
  renderer.drawText(UI_10_FONT_ID, counterX, counterY, counter);
}

// Loads the saved word at `index` from disk and re-lays the card out, so the
// reader can step through the whole notebook without returning to the list.
void DictionaryDefinitionActivity::showWordAt(int index) {
  if (!hasWordNavigation() || index < 0 || index >= static_cast<int>(navWords.size())) {
    return;
  }
  navIndex = index;
  headword = navWords[index];
  std::string loadedSentence;
  std::string loadedDefinition;
  if (VocabularyFile::loadText(headword, loadedSentence, loadedDefinition)) {
    sentence = std::move(loadedSentence);
    definition = std::move(loadedDefinition);
  } else {
    // Long text missing: still show the headword with an empty definition.
    sentence.clear();
    definition.clear();
  }
  book.clear();
  layoutCurrentWord();
  requestUpdate();
}

// Styled path: lay the HTML definition out through the EPUB chapter parser
// into reader-identical Pages. Frees `definition` on success (the page arenas
// own the text); any failure leaves state untouched for the plain-text path.
bool DictionaryDefinitionActivity::layoutHtmlPages() {
  const BodyArea body = bodyArea();
  if (body.width <= 0 || body.height <= 0) return false;
  if (!buildDictionaryHtmlPages(renderer, definition, static_cast<uint16_t>(body.width),
                                static_cast<uint16_t>(body.height), pages)) {
    return false;
  }
  definition.clear();
  definition.shrink_to_fit();
  totalPages = static_cast<int>(pages.size());
  currentPage = 0;
  return true;
}

int DictionaryDefinitionActivity::measureSpan(const int fontId, const char* text, size_t len) const {
  char buf[MAX_LINE_BYTES + 1];
  len = std::min(len, MAX_LINE_BYTES);
  memcpy(buf, text, len);
  buf[len] = '\0';
  return renderer.getTextAdvanceX(fontId, buf, EpdFontFamily::REGULAR);
}

// Greedy word-wrap of `definition` into byte spans. '\n' breaks lines (blank
// lines survive as paragraph spacing; NULs from multi-type StarDict entries
// were normalized to newlines in onEnter); '\r' is dropped by treating it as
// a space at a token edge.
void DictionaryDefinitionActivity::wrapText() {
  lines.clear();
  lines.reserve(definition.size() / 32 + 8);

  const int fontId = SETTINGS.getReaderFontId();
  // SD-card fonts: merge every definition codepoint into the persistent
  // advance table up front. Otherwise each unseen codepoint measured below
  // falls back to an on-demand glyph load from SD (8-slot overflow ring).
  renderer.ensureSdCardFontReady(fontId, definition.c_str(), 0x01 /* REGULAR */);

  const BodyArea body = bodyArea();
  const int maxWidth = body.width;
  const int spaceWidth = renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR);
  const int lineHeight = renderer.getLineHeight(fontId);
  linesPerPage = std::max(1, body.height / lineHeight);

  const char* text = definition.c_str();
  const uint32_t n = static_cast<uint32_t>(definition.size());
  uint32_t lineStart = 0;
  uint32_t lineEnd = 0;  // one past the last token byte on the current line
  int lineWidth = 0;

  const auto flushLine = [&](uint32_t nextStart) {
    lines.push_back({lineStart, static_cast<uint16_t>(lineEnd - lineStart)});
    lineStart = nextStart;
    lineEnd = nextStart;
    lineWidth = 0;
  };

  uint32_t i = 0;
  while (i < n) {
    const char c = text[i];
    if (c == '\n' || c == '\0') {
      flushLine(i + 1);
      i++;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      i++;
      continue;
    }

    // Token: run of non-whitespace bytes, capped at the measure buffer.
    const uint32_t tokenStart = i;
    while (i < n && text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n' && text[i] != '\0' &&
           i - tokenStart < MAX_LINE_BYTES) {
      i++;
    }
    // If the byte cap cut the token mid-UTF-8-sequence, back off to the last
    // complete codepoint so measure/draw never see a partial sequence. A
    // natural stop lands on whitespace or the terminating NUL, never on a
    // continuation byte, so this is a no-op there.
    while (i - tokenStart > 1 && (text[i] & 0xC0) == 0x80) i--;
    const uint32_t tokenLen = i - tokenStart;
    const int tokenWidth = measureSpan(fontId, text + tokenStart, tokenLen);

    if (lineEnd == lineStart) {
      lineStart = tokenStart;
      lineEnd = tokenStart + tokenLen;
      lineWidth = tokenWidth;
    } else if (lineWidth + spaceWidth + tokenWidth <= maxWidth &&
               tokenStart + tokenLen - lineStart <= UINT16_MAX) {  // span len must fit Line::len
      lineEnd = tokenStart + tokenLen;
      lineWidth += spaceWidth + tokenWidth;
    } else {
      flushLine(tokenStart);
      lineEnd = tokenStart + tokenLen;
      lineWidth = tokenWidth;
    }

    // An unbreakable token wider than the screen is now alone on the line
    // (any previous content was flushed above): split it at the widest
    // fitting UTF-8 boundary and carry the remainder forward.
    while (lineWidth > maxWidth && lineEnd - lineStart > 1) {
      const uint32_t len = lineEnd - lineStart;
      uint32_t lastFit = 0;
      for (uint32_t f = 1; f <= len; f++) {
        if (f == len || (text[lineStart + f] & 0xC0) != 0x80) {  // codepoint boundary
          if (measureSpan(fontId, text + lineStart, f) > maxWidth) break;
          lastFit = f;
        }
      }
      if (lastFit == 0) {
        // Even a single over-wide glyph must make progress; consume its whole
        // UTF-8 sequence rather than splitting it into invalid fragments.
        lastFit = 1;
        while (lastFit < len && (text[lineStart + lastFit] & 0xC0) == 0x80) lastFit++;
      }
      const uint32_t rest = lineStart + lastFit;
      lineEnd = rest;
      flushLine(rest);
      lineEnd = rest + (len - lastFit);
      lineWidth = measureSpan(fontId, text + lineStart, lineEnd - lineStart);
    }
  }
  if (lineEnd > lineStart) flushLine(n);

  // Trim trailing blank lines so the last page is not empty padding.
  while (!lines.empty() && lines.back().len == 0) lines.pop_back();

  totalPages = std::max(1, (static_cast<int>(lines.size()) + linesPerPage - 1) / linesPerPage);
  currentPage = 0;
}

void DictionaryDefinitionActivity::loop() {
  expireVocabularyMessage();
  expireQuoteMessage();
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

  // Same tap zones as the reader page turns: left third = previous page,
  // the rest = next. Back is the usual left-edge swipe. The vocabulary
  // button is checked first: collecting a word is a tap on it, never a
  // button press.
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    const Rect button = vocabularyButtonRect();
    if (button.width > 0 && button.height > 0 && tx >= button.x && tx < button.x + button.width && ty >= button.y &&
        ty < button.y + button.height) {
      toggleVocabulary();
      return;
    }
    const Rect qbutton = quoteButtonRect();
    if (qbutton.width > 0 && qbutton.height > 0 && tx >= qbutton.x && tx < qbutton.x + qbutton.width &&
        ty >= qbutton.y && ty < qbutton.y + qbutton.height) {
      saveAsQuote();
      return;
    }
    const Rect aibutton = aiButtonRect();
    if (aibutton.width > 0 && ty >= aibutton.y && ty < aibutton.y + aibutton.height &&
        tx >= aibutton.x && tx < aibutton.x + aibutton.width) {
      startAiExplain();
      return;
    }
    // The prev/next-word row (vocabulary browsing only) takes taps before the
    // generic left/right page-turn zones.
    const Rect prev = prevWordButtonRect();
    if (prev.width > 0 && tx >= prev.x && tx < prev.x + prev.width && ty >= prev.y && ty < prev.y + prev.height) {
      if (navIndex > 0) showWordAt(navIndex - 1);
      return;
    }
    const Rect next = nextWordButtonRect();
    if (next.width > 0 && tx >= next.x && tx < next.x + next.width && ty >= next.y && ty < next.y + next.height) {
      if (navIndex + 1 < static_cast<int>(navWords.size())) showWordAt(navIndex + 1);
      return;
    }
    if (tx < renderer.getScreenWidth() / 3) {
      if (currentPage > 0) {
        currentPage--;
        requestUpdate();
      }
    } else if (currentPage + 1 < totalPages) {
      currentPage++;
      requestUpdate();
    }
    return;
  }

  // While browsing the notebook the physical Prev/Next buttons step through
  // the saved words; for a reader lookup they page through the definition.
  if (hasWordNavigation()) {
    buttonNavigator.onNext([this] {
      if (navIndex + 1 < static_cast<int>(navWords.size())) showWordAt(navIndex + 1);
    });
    buttonNavigator.onPrevious([this] {
      if (navIndex > 0) showWordAt(navIndex - 1);
    });
    return;
  }

  buttonNavigator.onNext([this] {
    if (currentPage + 1 < totalPages) {
      currentPage++;
      requestUpdate();
    }
  });

  buttonNavigator.onPrevious([this] {
    if (currentPage > 0) {
      currentPage--;
      requestUpdate();
    }
  });
}

// Collects the word into the vocabulary notebook and reports the outcome on
// the button itself.
void DictionaryDefinitionActivity::addToVocabulary() {
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  const VocabularyFile::AddResult result = VocabularyFile::add(headword, plainDefinition, sentence, book, now);
  switch (result) {
    case VocabularyFile::AddResult::Added:
      vocabularyMessage = tr(STR_VOCABULARY_ADDED);
      // The word is now stored, so the button must offer "remove" next.
      wordSaved = true;
      break;
    case VocabularyFile::AddResult::AlreadyPresent:
      vocabularyMessage = tr(STR_VOCABULARY_ALREADY_SAVED);
      // It is already in the notebook -- still a saved word.
      wordSaved = true;
      break;
    default:
      vocabularyMessage = tr(STR_VOCABULARY_SAVE_FAILED);
      break;
  }
  vocabularyMessageUntil = millis() + VOCABULARY_MESSAGE_MS;
  requestUpdate();
}

// Toggles the word in the notebook: add when unsaved, remove when saved.
void DictionaryDefinitionActivity::toggleVocabulary() {
  if (wordSaved) {
    if (VocabularyFile::remove(headword)) {
      wordSaved = false;
      vocabularyMessage = tr(STR_VOCABULARY_REMOVED);
    } else {
      vocabularyMessage = tr(STR_VOCABULARY_SAVE_FAILED);
    }
    vocabularyMessageUntil = millis() + VOCABULARY_MESSAGE_MS;
    requestUpdate();
  } else {
    // addToVocabulary() owns the message, the saved flag and the repaint.
    addToVocabulary();
  }
}

// Saves the line the word was picked from as a quote in the Quotes notebook.
void DictionaryDefinitionActivity::saveAsQuote() {
  if (sentence.empty()) {
    return;
  }
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  const QuotesFile::AddResult result = QuotesFile::add(sentence, book, /*page=*/0, now);
  switch (result) {
    case QuotesFile::AddResult::Added:
      quoteMessage = tr(STR_QUOTES_SAVED);
      break;
    case QuotesFile::AddResult::AlreadyPresent:
      quoteMessage = tr(STR_QUOTES_ALREADY_SAVED);
      break;
    default:
      quoteMessage = tr(STR_QUOTES_SAVE_FAILED);
      break;
  }
  quoteMessageUntil = millis() + VOCABULARY_MESSAGE_MS;
  requestUpdate();
}

// Drops the add-result message once it has been on screen long enough.
void DictionaryDefinitionActivity::expireVocabularyMessage() {
  if (!vocabularyMessage) {
    return;
  }
  if (static_cast<long>(millis() - vocabularyMessageUntil) < 0) {
    return;
  }
  vocabularyMessage = nullptr;
  requestUpdate();
}

// Drops the quote-result message once it has been on screen long enough.
void DictionaryDefinitionActivity::expireQuoteMessage() {
  if (!quoteMessage) {
    return;
  }
  if (static_cast<long>(millis() - quoteMessageUntil) < 0) {
    return;
  }
  quoteMessage = nullptr;
  requestUpdate();
}

// Draws the current page: a styled Page when the HTML layout succeeded,
// otherwise the wrapped line spans (copied into a stack buffer for NUL
// termination). Called twice per render: once in font-cache scan mode, once
// for the real paint.
void DictionaryDefinitionActivity::drawBody(const int fontId, const int x, const int startY) const {
  if (!pages.empty()) {
    pages[currentPage]->render(renderer, fontId, x, startY);
    return;
  }
  const int lineHeight = renderer.getLineHeight(fontId);
  char buf[MAX_LINE_BYTES + 1];
  const int firstLine = currentPage * linesPerPage;
  const int lastLine = std::min(firstLine + linesPerPage, static_cast<int>(lines.size()));
  for (int i = firstLine; i < lastLine; i++) {
    if (lines[i].len == 0) continue;
    const size_t len = std::min(static_cast<size_t>(lines[i].len), MAX_LINE_BYTES);
    memcpy(buf, definition.c_str() + lines[i].start, len);
    buf[len] = '\0';
    renderer.drawText(fontId, x, startY + (i - firstLine) * lineHeight, buf);
  }
}

void DictionaryDefinitionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  // While an AI request is in flight, paint a busy popup instead of the card
  // (network never runs on the render task).
  if (aiPhase == AiPhase::Requesting) {
    GUI.drawPopup(renderer, tr(STR_AI_EXPLAINING));
    renderer.displayBuffer();
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? metrics.sideButtonHintsWidth : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = renderer.getScreenWidth() - hintGutterWidth;
  const int rowTop = wordRowY();
  const int rowHeight = wordRowHeight();
  const int rowMid = rowTop + rowHeight / 2;

  // Word row: headword on the left, page counter on the right, and the
  // save / un-favorite chip centered between them.
  const Rect saveChip = vocabularyButtonRect();
  const int headwordBaseline = rowMid - renderer.getLineHeight(UI_12_FONT_ID) / 2;
  const int headwordRoom = saveChip.x - SIDE_PADDING - ACTION_BUTTON_GAP - (contentX + SIDE_PADDING);
  const std::string headwordLabel = renderer.truncatedText(UI_12_FONT_ID, headword.c_str(),
                                                           std::max(16, headwordRoom), EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, contentX + SIDE_PADDING, headwordBaseline, headwordLabel.c_str(), true,
                    EpdFontFamily::BOLD);
  if (totalPages > 1) {
    char counter[16];
    snprintf(counter, sizeof(counter), "%d/%d", currentPage + 1, totalPages);
    const int counterWidth = renderer.getTextWidth(UI_10_FONT_ID, counter);
    renderer.drawText(UI_10_FONT_ID, contentX + contentWidth - SIDE_PADDING - counterWidth,
                      rowMid - renderer.getLineHeight(UI_10_FONT_ID) / 2, counter);
  }
  drawVocabularyButton();
  drawHairline(renderer, contentX + CARD_INSET, rowTop + rowHeight + VOCABULARY_BUTTON_GAP / 2 - 2,
               contentWidth - 2 * CARD_INSET);

  // Body: a framed card holding the definition. Two-pass draw inside a
  // prewarm scope (same pattern as the reader's renderContents) so SD-card
  // font glyphs load from SD in one batch instead of one on-demand overflow
  // read per character on every page turn.
  const int fontId = SETTINGS.getReaderFontId();
  const int bodyStartY = rowTop + rowHeight + VOCABULARY_BUTTON_GAP;
  const BodyArea body = bodyArea();
  renderer.drawRoundedRect(contentX + SIDE_PADDING - CARD_INSET, bodyStartY - 6, body.width + 2 * CARD_INSET,
                           body.height + 12, 1, CARD_RADIUS, true);
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  drawBody(fontId, contentX + SIDE_PADDING, bodyStartY);  // scan pass: records codepoints only
  scope.endScanAndPrewarm();
  drawBody(fontId, contentX + SIDE_PADDING, bodyStartY);

  // Bottom pager: prev / n of N / next (notebook) or the AI explain button
  // (reader lookup). Both share the same bottom band.
  drawWordNavigation();
  drawAiButton();

  // The key hints stay unchanged: no key adds a word, the button does.
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), "", (currentPage > 0 ? "<" : ""), (currentPage + 1 < totalPages ? ">" : ""));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

// Draws the "add to vocabulary" button and, when a line is available, the
// "save as quote" button beside it. A result takes over the label so the
// answer to the tap stays where the eye already is.
void DictionaryDefinitionActivity::drawVocabularyButton() const {
  const char* idle = wordSaved ? tr(STR_VOCABULARY_REMOVE) : tr(STR_VOCABULARY_ADD);
  const char* label = vocabularyMessage ? vocabularyMessage : idle;
  GUI.drawActionButton(renderer, vocabularyButtonRect(), label, vocabularyMessage != nullptr);

  const Rect qbutton = quoteButtonRect();
  if (qbutton.width > 0) {
    const char* qlabel = quoteMessage ? quoteMessage : tr(STR_QUOTES_SAVE);
    GUI.drawActionButton(renderer, qbutton, qlabel, quoteMessage != nullptr);
  }
}

// Draws the bottom "AI explain" button for reader lookups. A transient result
// message (e.g. "no API key") takes over the label so the answer stays where
// the eye is.
void DictionaryDefinitionActivity::drawAiButton() const {
  const Rect rect = aiButtonRect();
  if (rect.width <= 0) return;
  const char* label = aiMessage ? aiMessage : tr(STR_AI_EXPLAIN);
  GUI.drawActionButton(renderer, rect, label, aiMessage != nullptr);
}

void DictionaryDefinitionActivity::ensureAiSession() {
  if (!aiSession_) aiSession_ = makeUniqueNoThrow<freeink::SecureHttpClient>();
}

void DictionaryDefinitionActivity::expireAiMessage() {
  if (aiMessage && static_cast<long>(millis() - aiMessageUntil) >= 0) {
    aiMessage = nullptr;
    requestUpdate();
  }
}

// Tap handler: queue the request for the next loop turn so the busy popup can
// be painted first and the loop task can feed its watchdog (same pattern as
// the weather app — no network on the render task).
void DictionaryDefinitionActivity::startAiExplain() {
  if (aiPhase != AiPhase::Idle) return;
  if (!AiCredentials::hasKey()) {
    aiMessage = tr(STR_AI_NO_KEY);
    aiMessageUntil = millis() + VOCABULARY_MESSAGE_MS;
    requestUpdate();
    return;
  }
  aiPhase = AiPhase::Requested;
  requestUpdate();
}

// Performs one AI request off the render task. Reuses the definition card to
// display the result: open a fresh card with the answer as its definition and
// the original word as headword, quote button hidden (the AI text is not a
// sentence to collect).
void DictionaryDefinitionActivity::performAiExplain() {
  resetTaskWatchdogIfSubscribed();
  ensureAiSession();
  const unsigned long started = millis();
  const auto keepAlive = [started]() {
    resetTaskWatchdogIfSubscribed();
    return millis() - started >= AI_REQUEST_DEADLINE_MS;
  };
  AiExplainClient::Result result =
      AiExplainClient::explain(headword, sentence, aiSession_.get(), keepAlive);
  resetTaskWatchdogIfSubscribed();
  LOG_INF("AIC", "explain status=%s http=%d ms=%lu", AiExplainClient::Result::label(result.status),
          result.httpStatus, result.elapsedMs);

  aiPhase = AiPhase::Idle;
  aiSession_.reset();  // free wolfSSL buffers; this card is stacked over a reader

  if (result.status == AiExplainClient::Result::Status::Ok) {
    std::string answer = std::move(result.text);
    std::string word = headword;
    // showQuoteButton=false: an AI answer is not a line to add to the notebook.
    startActivityForResultWith<DictionaryDefinitionActivity>(
        [this](const ActivityResult&) { requestUpdate(); }, std::move(word), std::move(answer),
        /*htmlDefinition=*/false, book, sentence, /*showQuoteButton=*/false);
    return;
  }

  switch (result.status) {
    case AiExplainClient::Result::Status::NoWifi:
      aiMessage = tr(STR_AI_NO_WIFI);
      break;
    case AiExplainClient::Result::Status::HttpError:
      aiMessage = tr(STR_AI_HTTP_ERROR);
      break;
    default:
      aiMessage = tr(STR_AI_FAILED);
      break;
  }
  aiMessageUntil = millis() + VOCABULARY_MESSAGE_MS;
  requestUpdate();
}
