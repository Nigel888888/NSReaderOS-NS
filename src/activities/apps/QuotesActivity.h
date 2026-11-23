#pragma once

#include <I18n.h>

#include <string>
#include <vector>

#include "activities/UiTabListActivity.h"
#include "components/OptionPopup.h"
#include "components/Rect.h"
#include "util/QuotesFile.h"

// The Quotes notebook: every sentence the reader highlighted, in one app.
// Three tabs mirror the CrossNext Quotes screen -- ALL (every quote, newest
// first), BOOKS (one row per source title) and FAVORITES (starred quotes).
// Tapping a quote opens a dialog with the full sentence and Star/Delete
// actions; tapping a book drills into that book's quotes.
class QuotesActivity final : public UiTabListActivity {
 public:
  explicit QuotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;

 protected:
  enum Tab : int { AllTab, BooksTab, FavoritesTab, TabCount };

  int listCount() const override { return static_cast<int>(rowItems.size()); }
  const char* headerTitle() const override { return tr(STR_QUOTES_TITLE); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  void render(RenderLock&& lock) override;
  void onBackButton() override { activityManager.goToApps(); }

  int tabCount() const override { return TabCount; }
  int activeTab() const override { return tab_; }
  const char* tabLabel(int index) const override;
  void onTabAction(int index) override;
  void stepTab(int direction) override;

 private:
  // Rebuilds rowItems from `quotes` for the current tab/drill-down state.
  void rebuildRows();
  // Re-reads the notebook from disk and rebuilds the visible rows.
  void reload();
  // Opens the full-screen paged viewer on the quote at `index`.
  void openQuote(int index);
  // Left-swipe a quote row to delete it after confirmation.
  bool handleSwipeDelete();
  // Deletes the quote at visible row `index` and persists the notebook.
  void deleteQuoteAt(int index);
  // Exports every quote to a Kindle "My Clippings.txt" file on the SD card
  // and reports the outcome on the export button for a few seconds.
  void exportToClippings();

  enum class RowKind : uint8_t { Quote, Book };

  struct Row {
    RowKind kind = RowKind::Quote;
    int index = -1;  // index into `quotes` for a quote row, else -1
    std::string key;
    std::string label;
    std::string subtitle;
    std::string date;     // YYYY-MM-DD pill drawn at the card's bottom right
    std::string bookKey;  // actual source title for a Book row (may be empty)
    bool favorite = false;
  };

  std::vector<QuotesFile::Quote> quotes;
  std::vector<Row> rows;
  std::vector<freeink::ui::ListItem> rowItems;
  std::vector<std::string> rowSubtitles;
  int tab_ = AllTab;
  // Two-step activation: first tap selects the card, second tap on the same
  // card opens the sentence / drills into the book. -1 = nothing armed.
  int armedIndex = -1;
  // When >= 0, the Books tab is drilled into this source title.
  int bookFilter_ = -1;
  std::string filteredBook;
  OptionPopup dialog{true};
  // List area on the last build, for mapping a left-swipe start y to a row.
  Rect listRect{0, 0, 0, 0};
  int swipeRowHeight = 0;
  int swipeRowGap = 0;
  // Export button in the band under the tabs, and its transient result label.
  Rect exportButton{0, 0, 0, 0};
  std::string exportMessage;
  unsigned long exportMessageUntil = 0;
};
