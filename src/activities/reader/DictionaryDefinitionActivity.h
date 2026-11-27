#pragma once

#include <Epub/Page.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/Rect.h"
#include "util/ButtonNavigator.h"

namespace freeink {
class SecureHttpClient;
}

// Paged viewer for one dictionary definition. HTML definitions are laid out
// through the EPUB chapter parser into styled Pages; anything else (plain
// text, or HTML too damaged to parse) is word-wrapped once on entry and each
// page renders spans of the original string, so no per-line copies are held.
class DictionaryDefinitionActivity final : public Activity {
 public:
  // `navWords` + `navIndex` are supplied by the vocabulary notebook so the
  // card can page directly to the previous/next saved word. Empty when the
  // card is opened from the reader (a single lookup).
  // Defined out-of-line: the class holds a unique_ptr<SecureHttpClient> and
  // ctor/dtor both need that forward-declared type to be complete at their
  // point of instantiation (same pattern as WeatherActivity).
  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string headword,
                                        std::string definition, bool htmlDefinition = false, std::string book = "",
                                        std::string sentence = "", bool showQuoteButton = true,
                                        std::vector<std::string> navWords = {}, int navIndex = 0,
                                        bool aiResultCard = false);
  ~DictionaryDefinitionActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Looking up a word and the AI-explain request/popup/result burst are short
  // multi-frame actions; don't let the global cadence flash mid-action.
  bool skipsFullRefreshCadence() const override { return true; }

 private:
  // One wrapped display line: a byte span of `definition`. Wrapping keeps
  // lines under the screen width, so uint16_t length is ample.
  struct Line {
    uint32_t start;
    uint16_t len;
  };

  // Usable body-text area between the header and the button hints.
  struct BodyArea {
    int width;
    int height;
  };

  BodyArea bodyArea() const;
  // Lays out the current headword/definition (shared by entry and navigation).
  void layoutCurrentWord();
  bool layoutHtmlPages();
  void wrapText();
  int measureSpan(int fontId, const char* text, size_t len) const;
  void drawBody(int fontId, int x, int startY) const;
  // AI answers carry Chinese text (DeepSeek appends a translation). The reader
  // font can be a Latin-only SD font with no CJK fallback, so those cards must
  // render in the CJK-capable built-in face. Both wrap and draw go through this
  // helper so measurement and painting always use the same font.
  int bodyFontId() const;

  // Screen box of the "add to vocabulary" button. Collecting a word is a tap
  // on this button, never a button press, so the box has to be hit-testable.
  Rect vocabularyButtonRect() const;
  // Screen box of the "save as quote" button, beside the vocabulary one. Only
  // shown when the word came from a line of text.
  Rect quoteButtonRect() const;
  // Screen boxes of the prev/next-word buttons shown when navigating the
  // vocabulary notebook. Zero-width when there is no word list.
  Rect prevWordButtonRect() const;
  Rect nextWordButtonRect() const;
  // Shared baseline for the bottom prev/next pager (CrossNext-style).
  int wordNavY() const;
  // The top band that holds the headword, the centered save action and the
  // page counter. Shared by render() and the action-button rects.
  int wordRowY() const;
  int wordRowHeight() const;
  void drawVocabularyButton() const;
  void drawWordNavigation() const;
  // Screen box of the "AI explain" button on the bottom row (only for reader
  // lookups; the vocabulary notebook uses that row for prev/next navigation).
  Rect aiButtonRect() const;
  void drawAiButton() const;
  // Kicks off / drives an AI explanation request. Network I/O runs in loop(),
  // never on the render task; a Phase state machine paints a busy popup.
  void startAiExplain();
  void performAiExplain();
  // Opens a fresh definition card showing `text` as the AI explanation.
  // Shared by cache hits (no network) and successful requests.
  void showAiResult(const std::string& text);
  // If Wi-Fi is down, push the Wi-Fi picker (auto-connect from saved creds)
  // and resume the AI request on a successful result.
  void ensureWifiThenAi();
  void expireAiMessage();
  void addToVocabulary();
  // Adds the word when unsaved, removes it when already saved.
  void toggleVocabulary();
  void saveAsQuote();
  // Loads `navWords[navIndex]` from disk and re-lays the card out.
  void showWordAt(int index);
  // True while a word list is attached (not a plain reader lookup).
  bool hasWordNavigation() const { return !navWords.empty(); }
  void expireVocabularyMessage();
  void expireQuoteMessage();

  // Mutable because word navigation can swap in the next/previous saved word.
  // onEnter() also normalizes embedded NULs (StarDict multi-type separators)
  // to newlines so C-string APIs see the whole text.
  std::string headword;
  std::string definition;
  const bool htmlDefinition;
  // Where the word was collected from (the book title). Empty when it did not
  // come from a book.
  std::string book;
  // The full sentence the word came from, used as its review context.
  std::string sentence;
  // The reader offers "save as quote" next to "add to vocabulary". The
  // vocabulary notebook reuses this card to re-read a saved word, and there a
  // quote action makes no sense -- the sentence is already attached to the
  // saved word. Hide the quote button in that case.
  const bool showQuoteButton;
  // Vocabulary browsing: the ordered headwords and the current position.
  // Empty for a plain reader lookup.
  std::vector<std::string> navWords;
  int navIndex = 0;
  // True for the card that shows an AI answer. Its body is a CJK-bearing
  // answer (not an English dictionary definition), so it renders in the
  // CJK-capable built-in face, and its primary action saves the sentence to
  // the quotes notebook instead of adding a vocabulary word.
  const bool aiResultCard;
  // Plain-text copy of the definition, kept for the vocabulary notebook: the
  // styled path below consumes `definition` (the HTML source) and frees it.
  std::string plainDefinition;
  // Whether the word was already in the notebook on entry; the button
  // toggles between add and remove.
  bool wordSaved = false;
  // Result of the last add/remove attempt, shown on the button until it expires.
  const char* vocabularyMessage = nullptr;
  unsigned long vocabularyMessageUntil = 0;
  // Result of the last save-as-quote attempt.
  const char* quoteMessage = nullptr;
  unsigned long quoteMessageUntil = 0;
  // AI explain state machine: Idle -> Requested -> Requesting -> Idle (result
  // opens a new definition card) or a transient error message on this card.
  enum class AiPhase : uint8_t { Idle, Requested, Requesting };
  AiPhase aiPhase = AiPhase::Idle;
  const char* aiMessage = nullptr;
  unsigned long aiMessageUntil = 0;
  std::unique_ptr<freeink::SecureHttpClient> aiSession_;
  void ensureAiSession();
  // Styled path: reader-identical Pages laid out from the HTML definition.
  // Empty means the plain-text span path below is active.
  std::vector<std::unique_ptr<Page>> pages;
  std::vector<Line> lines;
  int currentPage = 0;
  int totalPages = 1;
  int linesPerPage = 1;
  ButtonNavigator buttonNavigator;
};
