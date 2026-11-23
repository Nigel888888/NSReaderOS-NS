#pragma once

#include <I18n.h>

#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"
#include "components/Rect.h"
#include "util/VocabularyFile.h"

// The vocabulary notebook: every word collected from the dictionary card.
//
// The screen opens on a band of two counters -- how many words are saved and
// how many of them are due -- and a button that starts the review session.
// Below the band sits the notebook itself: a row shows the headword, a
// one-line excerpt of its definition and the book it came from. Tapping a row
// opens the full definition (the notebook keeps the whole text on disk, the
// row only holds an excerpt); long-pressing a row asks for confirmation and
// deletes the word.
class VocabularyActivity final : public UiListActivity {
 public:
  explicit VocabularyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;

 private:
  // Visible row count: equals entries.size() in "collected" mode, or only the
  // due words in "to review" mode.
  int listCount() const override { return static_cast<int>(rowItems.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  // Popup handling runs before everything else each pass.
  bool handleCustomInput() override;
  // Back cancels with a result; Confirm opens on release and a hold deletes.
  bool handleButtons() override;
  // Back returns to the Apps menu, like every other app (this app replaces the
  // previous activity, so there is no stack entry to pop).
  void onBackButton() override { activityManager.goToApps(); }
  const char* headerTitle() const override { return tr(STR_VOCABULARY_TITLE); }

  // Derives the row subtitles/items from `entries`. Rebuilt whenever the
  // notebook changes (onEnter() load, post-delete) so buildScreen() reuses
  // the cached rows instead of re-composing them on every repaint.
  void rebuildRowItems();

  // Opens the Cancel/Delete confirmation for the selected word.
  void showDeleteConfirmation();
  // Deletes the currently selected word and persists the notebook.
  void deleteSelected();

  // Re-reads the notebook and the counters from disk.
  void reload();

  // Sorts `entries` by the saved timestamp per the current sort order.
  void sortEntries();
  // Toggles newest-first / oldest-first and rebuilds the rows.
  void toggleSort();
  // Switches between the "collected" (all words) and "to review" (due words)
  // segments of the counter capsule.
  void setDueOnly(bool dueOnly);
  // Enters/leaves multi-select management mode.
  void toggleManageMode();
  // Deletes every checked word and persists the notebook.
  void deleteSelectedWords();
  // Left-swipe on a row opens the delete confirmation for that word.
  bool handleSwipeDelete();

  // The counter band above the list: the two cards and the review button.
  void drawStatsBand(const Rect& band);
  void drawCounterCard(const Rect& rect, int value, const char* label, const uint8_t* icon, bool whiteText = true);
  // Screen box of the "start review" button, remembered so the tap can be
  // hit-tested outside the list.
  Rect reviewButtonRect() const { return reviewButton; }
  // Starts the review session.
  void startReview();
  // Exports the notebook to an Anki TSV deck on the SD card.
  void exportToAnki();

  std::vector<VocabularyFile::Entry> entries;
  // Row buffers derived from `entries`; kept alive so the ListItem string
  // pointers stay valid between renders.
  std::vector<std::string> rowSubtitles;
  std::vector<freeink::ui::ListItem> rowItems;
  // Visible row -> position in `entries`, so filter/tap/delete stay aligned.
  std::vector<int> rowEntryIndex;
  // Counter capsule segment: false = collected (all words), true = to review
  // (only due words). Both halves are tappable.
  bool dueOnly = false;
  // Two-step activation: the first tap only selects the visible card, the
  // second tap on that same card opens the full definition. -1 = nothing armed.
  int armedIndex = -1;
  Rect collectedSegment{0, 0, 0, 0};
  Rect dueSegment{0, 0, 0, 0};
  // How many words are due for review right now.
  int dueCount = 0;
  // The label of the review button, rebuilt whenever the due count changes.
  std::string reviewLabel;
  // Where the review and export buttons were drawn last time.
  Rect reviewButton{0, 0, 0, 0};
  Rect exportButton{0, 0, 0, 0};
  // Last export result, shown on the export button until it expires.
  std::string exportMessage;
  unsigned long exportMessageUntil = 0;
  bool confirmingDelete = false;
  OptionPopup confirmPopup{true};

  // Multi-select management mode: each row carries a checkbox, tapping a row
  // checks it instead of opening the card, and a bottom bar deletes all checked
  // words at once.
  bool manageMode = false;
  std::vector<bool> selectedFlags;
  // 0 = newest first, 1 = oldest first.
  uint8_t sortOrder = 0;
  Rect sortButton{0, 0, 0, 0};
  Rect manageButton{0, 0, 0, 0};
  Rect deleteBar{0, 0, 0, 0};
  Rect selectAllButton{0, 0, 0, 0};
  // Screen rect the list occupied on the last build, used to map a left-swipe's
  // start y to a row index.
  Rect listRect{0, 0, 0, 0};
  // Exact row geometry from the last build. The list is given a fixed rowHeight
  // so a swipe's start y maps to a row precisely (same scheme as Quotes); these
  // mirror the values handed to ListProps.
  int swipeRowHeight = 0;
  int swipeRowGap = 0;
};
