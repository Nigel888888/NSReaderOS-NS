#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/Dictionary.h"

// Word selection over the current reader page: Left/Right step through words
// in reading order, Up/Down jump rows, Confirm looks the word up and opens
// DictionaryDefinitionActivity, Back returns to the reader. On touch devices a
// touch-down moves the highlight and a tap on a word looks it up directly.
class DictionaryWordSelectActivity final : public Activity {
 public:
  // `preselectX/preselectY` are screen coordinates of a long-press in the
  // reader; when set (>= 0) the word under the finger is highlighted on entry
  // instead of the default middle-row word.
  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<Page> page, int marginLeft, int marginTop,
                                        std::string bookTitle = "", int preselectX = -1, int preselectY = -1)
      : Activity("DictionaryWordSelect", renderer, mappedInput),
        page(std::move(page)),
        marginLeft(marginLeft),
        marginTop(marginTop),
        bookTitle(std::move(bookTitle)),
        preselectX(preselectX),
        preselectY(preselectY) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Screen box of one selectable word. `text` points into the owned Page's
  // TextBlock arena (NUL-terminated), valid for this activity's lifetime.
  struct WordBox {
    int16_t x;
    int16_t y;
    int16_t width;
    uint16_t row;
    const char* text;
    EpdFontFamily::Style style;
    uint32_t textOffset = 0;  // byte offset of `text` inside pageText
  };

  enum class Popup : uint8_t { None, Busy, NotFound, Error };

  void extractWords();
  // The text of one page row, used as the sentence a collected word is
  // reviewed in. Empty when the row is unknown.
  const std::string& rowText(uint16_t row) const;
  // The complete source sentence containing `wordIndex`: the page's text is
  // joined in reading order and cut at sentence punctuation on both sides.
  // Falls back to the row text when the page text is unavailable.
  std::string sentenceForWord(int wordIndex) const;
  int closestInRow(uint16_t row, int centerX) const;
  int wordAt(int x, int y) const;
  void moveVertical(int direction);
  void performLookup();
  bool drawHighlightWithSnapshot();
  void drawHints() const;

  std::unique_ptr<Page> page;
  const int marginLeft;
  const int marginTop;
  // Title of the book the word came from; passed on to the definition card so
  // a collected word can record where it was found.
  const std::string bookTitle;
  // Optional reader long-press point; the word beneath it is preselected.
  const int preselectX;
  const int preselectY;
  int fontId = 0;
  int lineHeight = 0;

  std::vector<WordBox> words;
  // The full text of every page row that holds a selectable word, indexed the
  // same way as WordBox::row. A collected word is reviewed in the sentence it
  // came from, so the line has to be kept whole -- including the tokens that
  // are not selectable on their own.
  std::vector<std::string> rowTexts;
  // Full page text in reading order (all tokens, not just selectable ones).
  // Used to cut out the complete sentence a collected word came from.
  std::string pageText;
  int selected = 0;
  uint16_t rowCount = 0;
  unsigned long lastHorizontalMoveTime = 0;

  Dictionary dict;
  bool dictOpenAttempted = false;
  bool dictOpenOk = false;
  bool dictNeedsIndex = false;

  Popup popup = Popup::None;
  StrId popupMsg = StrId::STR_DICT_NOT_FOUND;
  unsigned long popupTime = 0;

  // Differential highlight repaint: the pixels under the current highlight
  // box, so a cursor move restores them and repaints only the two affected
  // boxes instead of re-running the full two-pass page render (which also
  // reloads every SD-font glyph on the page). snapshotIdx is the word whose
  // under-pixels are saved; -1 means the framebuffer no longer holds a clean
  // page (popup drawn, sub-activity shown) and the next render must be full.
  static constexpr size_t SNAPSHOT_CAPACITY = 4096;
  std::unique_ptr<uint8_t[]> snapshot;
  int16_t snapshotX = 0;
  int16_t snapshotY = 0;
  int16_t snapshotW = 0;
  int16_t snapshotH = 0;
  int snapshotIdx = -1;
};
