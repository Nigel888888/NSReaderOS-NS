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
  // Shows a quote with its full text and Star/Delete actions.
  void openQuote(int index);
  // Star/unstar the quote behind the open dialog.
  void toggleFavorite(int index);
  // Ask, then delete, the quote behind the open dialog.
  void confirmDelete(int index);

  enum class RowKind : uint8_t { Quote, Book };

  struct Row {
    RowKind kind = RowKind::Quote;
    int index = -1;  // index into `quotes` for a quote row, else -1
    std::string key;
    std::string label;
    std::string subtitle;
    std::string bookKey;  // actual source title for a Book row (may be empty)
    bool favorite = false;
  };

  std::vector<QuotesFile::Quote> quotes;
  std::vector<Row> rows;
  std::vector<freeink::ui::ListItem> rowItems;
  std::vector<std::string> rowSubtitles;
  int tab_ = AllTab;
  // When >= 0, the Books tab is drilled into this source title.
  int bookFilter_ = -1;
  std::string filteredBook;
  // The count line drawn under the tab band.
  std::string countLine;
  // Row the currently open quote dialog describes (-1 when none).
  int activeQuoteIndex = -1;
  OptionPopup dialog{true};
};
