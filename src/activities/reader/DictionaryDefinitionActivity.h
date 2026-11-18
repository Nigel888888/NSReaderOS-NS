#pragma once

#include <Epub/Page.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/Rect.h"
#include "util/ButtonNavigator.h"

// Paged viewer for one dictionary definition. HTML definitions are laid out
// through the EPUB chapter parser into styled Pages; anything else (plain
// text, or HTML too damaged to parse) is word-wrapped once on entry and each
// page renders spans of the original string, so no per-line copies are held.
class DictionaryDefinitionActivity final : public Activity {
 public:
  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string headword,
                                        std::string definition, bool htmlDefinition = false, std::string book = "",
                                        std::string sentence = "")
      : Activity("DictionaryDefinition", renderer, mappedInput),
        headword(std::move(headword)),
        definition(std::move(definition)),
        htmlDefinition(htmlDefinition),
        book(std::move(book)),
        sentence(std::move(sentence)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

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
  bool layoutHtmlPages();
  void wrapText();
  int measureSpan(int fontId, const char* text, size_t len) const;
  void drawBody(int fontId, int x, int startY) const;

  // Screen box of the "add to vocabulary" button. Collecting a word is a tap
  // on this button, never a button press, so the box has to be hit-testable.
  Rect vocabularyButtonRect() const;
  // Screen box of the "save as quote" button, beside the vocabulary one. Only
  // shown when the word came from a line of text.
  Rect quoteButtonRect() const;
  void drawVocabularyButton() const;
  void addToVocabulary();
  void saveAsQuote();
  void expireVocabularyMessage();
  void expireQuoteMessage();

  const std::string headword;
  // Not const: onEnter() normalizes embedded NULs (StarDict multi-type
  // separators) to newlines so C-string APIs see the whole text.
  std::string definition;
  const bool htmlDefinition;
  // Where the word was collected from (the book title). Empty when it did not
  // come from a book.
  const std::string book;
  // The line the word was picked from. It is the context the word is reviewed
  // in later on; empty when the word did not come from a page of text.
  const std::string sentence;
  // Plain-text copy of the definition, kept for the vocabulary notebook: the
  // styled path below consumes `definition` (the HTML source) and frees it.
  std::string plainDefinition;
  // Result of the last add attempt, shown on the button until it expires.
  const char* vocabularyMessage = nullptr;
  unsigned long vocabularyMessageUntil = 0;
  // Result of the last save-as-quote attempt.
  const char* quoteMessage = nullptr;
  unsigned long quoteMessageUntil = 0;
  // Styled path: reader-identical Pages laid out from the HTML definition.
  // Empty means the plain-text span path below is active.
  std::vector<std::unique_ptr<Page>> pages;
  std::vector<Line> lines;
  int currentPage = 0;
  int totalPages = 1;
  int linesPerPage = 1;
  ButtonNavigator buttonNavigator;
};
