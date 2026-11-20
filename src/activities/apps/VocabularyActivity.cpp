#include "VocabularyActivity.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "VocabularyReviewActivity.h"
#include "activities/reader/DictionaryDefinitionActivity.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "components/icons/book24.h"
#include "components/icons/bookmark.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {

// Hold time on Confirm that opens the delete confirmation.
constexpr unsigned long ENTER_ACTIONS_MODE_MS = 700;

// The dashboard above the list: two compact solid cards. Each puts the label
// and its number on one row (label first, count after) in white on black.
constexpr int STATS_CARD_HEIGHT = 64;
constexpr int STATS_CARD_GAP = 12;
constexpr int STATS_CARD_RADIUS = 6;
constexpr int STATS_ICON_SIZE = 24;
constexpr int STATS_ICON_PAD = 10;
// Corner radius of the counter cards.
// Baseline offset of the single "label + number" text row inside the card.
constexpr int STATS_CARD_TEXT_TOP = 20;
// The review button under the counters. Enlarged for the high-DPI Read Pico
// panel: the old 44/40 px bars were hard to see and to hit.
constexpr int REVIEW_BUTTON_HEIGHT = 64;
constexpr int REVIEW_BUTTON_GAP = 12;
// The Anki export button, beneath the review button.
constexpr int EXPORT_BUTTON_HEIGHT = 60;
constexpr int EXPORT_BUTTON_GAP = 10;
// How long the export result stays on the button.
constexpr unsigned long EXPORT_MESSAGE_MS = 3000;
// The font the counters are drawn in.
constexpr int STATS_FONT_ID = NOTOSANS_18_FONT_ID;
// Section label above the list ("我的生词"), with the total written right
// after the text (not right-aligned) and generous space above/below.
constexpr int SECTION_LABEL_HEIGHT = 30;
constexpr int SECTION_LABEL_GAP = 24;

}  // namespace

VocabularyActivity::VocabularyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("Vocabulary", renderer, mappedInput, /*wantsTouchLongPress=*/true) {}

void VocabularyActivity::onEnter() {
  UiListActivity::onEnter();
  reload();
}

// Reads the notebook and the counters back from disk.
void VocabularyActivity::reload() {
  VocabularyFile::load(entries);
  sortEntries();
  rebuildRowItems();
  dueCount =
      VocabularyFile::countDue(entries, VocabularyFile::currentDayNumber(), VocabularyFile::currentMinuteOfDay());
  // The button carries the count, so its label is rebuilt with it.
  reviewLabel = std::string(tr(STR_VOCABULARY_START_REVIEW)) + " (" + std::to_string(dueCount) + ")";
}

// Sorts by the timestamp saved with each word. Equal timestamps keep their
// on-disk order. 0 = newest first, 1 = oldest first.
void VocabularyActivity::sortEntries() {
  const bool newestFirst = (sortOrder == 0);
  std::stable_sort(entries.begin(), entries.end(), [newestFirst](const VocabularyFile::Entry& a,
                                                                 const VocabularyFile::Entry& b) {
    return newestFirst ? a.timestamp > b.timestamp : a.timestamp < b.timestamp;
  });
}

void VocabularyActivity::toggleSort() {
  sortOrder = static_cast<uint8_t>(sortOrder == 0 ? 1 : 0);
  sortEntries();
  rebuildRowItems();
  requestUpdate();
}

// Derives rowSubtitles/rowItems from `entries`. Called whenever `entries`
// changes (onEnter() load, post-delete) so buildScreen() reuses the cached
// rows on every repaint.
void VocabularyActivity::rebuildRowItems() {
  rowSubtitles.clear();
  rowItems.clear();
  // Reserve up front: the ListItem strings point into rowSubtitles, so the
  // vector must not reallocate while the items are being built.
  rowSubtitles.reserve(entries.size());
  rowItems.reserve(entries.size());
  if (manageMode) selectedFlags.assign(entries.size(), false);
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto& entry = entries[i];
    // Row subtitle: the definition excerpt, then the book it came from.
    std::string subtitle = entry.summary;
    if (!entry.book.empty()) {
      if (!subtitle.empty()) {
        subtitle += " - ";
      }
      subtitle += entry.book;
    }
    rowSubtitles.push_back(std::move(subtitle));

    fui::ListItem item;
    item.label = entry.headword.c_str();
    item.subtitle = rowSubtitles.back().c_str();
    if (manageMode) {
      // A checkbox per word; tapping the row toggles selection instead of
      // opening the card.
      item.icon = GUI.checkboxIcon(i < selectedFlags.size() && selectedFlags[i]);
      item.value = nullptr;
    } else {
      item.icon = listIconFor(UIIcon::Bookmark, 32);  // subtitle rows carry the larger icon
      item.value = ">";
    }
    item.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(item);
  }
}

void VocabularyActivity::activateIndex(const int index) {
  if (confirmPopup.isActive()) {
    return;
  }
  // The interaction table can deliver a row index captured before a delete
  // shrank the list; the next render re-registers the rows.
  if (index < 0 || index >= listCount()) {
    return;
  }
  // The tapped row leaves this screen; a lingering flash would gray an
  // unrelated row on the next render.
  app.clearTapFlash();
  nav.selected = index;

  if (manageMode) {
    if (index >= 0 && index < static_cast<int>(selectedFlags.size())) {
      selectedFlags[index] = !selectedFlags[index];
      rebuildRowItems();
      requestUpdate();
    }
    return;
  }

  const auto& entry = entries.at(index);
  // The row only holds an excerpt; the full definition stays on disk.
  std::string sentence;
  std::string definition;
  VocabularyFile::loadText(entry.headword, sentence, definition);
  // Hand the card the whole ordered word list plus this position so Prev/Next
  // can step through the notebook without returning here.
  std::vector<std::string> navWords;
  navWords.reserve(entries.size());
  for (const auto& e : entries) {
    navWords.push_back(e.headword);
  }
  // Re-reading a saved word: the sentence is already attached to the card, so
  // do not offer the "save as quote" action here (it belongs to the reader).
  startActivityForResultWith<DictionaryDefinitionActivity>(
      [this](const ActivityResult&) { requestUpdate(); }, entry.headword, definition, false, entry.book, sentence,
      /*showQuoteButton=*/false, std::move(navWords), index);
}

void VocabularyActivity::onRowLongPress(const int index) {
  if (confirmPopup.isActive()) {
    return;
  }
  if (index < 0 || index >= listCount()) {
    return;
  }
  app.clearTapFlash();
  nav.selected = index;
  if (manageMode) {
    if (index >= 0 && index < static_cast<int>(selectedFlags.size())) {
      selectedFlags[index] = !selectedFlags[index];
      rebuildRowItems();
      requestUpdate();
    }
    return;
  }
  showDeleteConfirmation();
}

bool VocabularyActivity::handleCustomInput() {
  // Delete confirmation popup.
  if (confirmPopup.handleInput(mappedInput, [this] { requestUpdate(); })) {
    return true;
  }
  if (confirmingDelete) {
    // Popup dismissed without a selection (Back button or tap outside): cancel
    // the delete.
    confirmingDelete = false;
    requestUpdate();
    return true;
  }
  // The review button sits above the list, so the list never sees the tap.
  // The tap query does not consume the event, so a tap that misses the button
  // still reaches the list below.
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    const Rect button = reviewButton;
    if (button.width > 0 && button.height > 0 && tx >= button.x && tx < button.x + button.width && ty >= button.y &&
        ty < button.y + button.height) {
      startReview();
      return true;
    }
    const Rect exp = exportButton;
    if (exp.width > 0 && exp.height > 0 && tx >= exp.x && tx < exp.x + exp.width && ty >= exp.y &&
        ty < exp.y + exp.height) {
      exportToAnki();
      return true;
    }
    const auto hit = [tx, ty](const Rect& r) {
      return r.width > 0 && r.height > 0 && tx >= r.x && tx < r.x + r.width && ty >= r.y && ty < r.y + r.height;
    };
    if (hit(sortButton)) {
      toggleSort();
      return true;
    }
    if (hit(manageButton)) {
      toggleManageMode();
      return true;
    }
    if (manageMode && hit(selectAllButton)) {
      const bool allSelected = !selectedFlags.empty() &&
                               std::all_of(selectedFlags.begin(), selectedFlags.end(), [](bool v) { return v; });
      std::fill(selectedFlags.begin(), selectedFlags.end(), !allSelected);
      rebuildRowItems();
      requestUpdate();
      return true;
    }
    if (manageMode && hit(deleteBar)) {
      deleteSelectedWords();
      return true;
    }
  }
  return false;
}

// Writes the deck to the SD card and reports the outcome on the button.
void VocabularyActivity::exportToAnki() {
  app.clearTapFlash();
  constexpr const char* kPath = "/vocabulary.tsv";
  const int count = VocabularyFile::exportAnki(kPath);
  if (count > 0) {
    char msg[64];
    std::snprintf(msg, sizeof(msg), tr(STR_VOCABULARY_EXPORT_DONE), count);
    exportMessage = msg;
  } else if (count == 0) {
    exportMessage = tr(STR_VOCABULARY_EXPORT_EMPTY);
  } else {
    exportMessage = tr(STR_VOCABULARY_EXPORT_FAIL);
  }
  exportMessageUntil = millis() + EXPORT_MESSAGE_MS;
  requestUpdate();
}

bool VocabularyActivity::handleButtons() {
  if (!manageMode && mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, ENTER_ACTIONS_MODE_MS)) {
    showDeleteConfirmation();
    return true;
  }
  return UiListActivity::handleButtons();
}

void VocabularyActivity::showDeleteConfirmation() {
  if (entries.empty() || confirmPopup.isActive()) {
    return;
  }
  confirmingDelete = true;
  const char* options[] = {tr(STR_CANCEL), tr(STR_DELETE)};
  confirmPopup.show(tr(STR_VOCABULARY_DELETE_CONFIRM), options, 2, 0, [this](int idx) {
    confirmingDelete = false;
    if (idx == 1) {
      deleteSelected();
    }
    requestUpdate();
  });
  requestUpdate();
}

void VocabularyActivity::deleteSelected() {
  if (nav.selected < 0 || nav.selected >= listCount()) {
    return;
  }
  const std::string headword = entries.at(nav.selected).headword;
  if (!VocabularyFile::remove(headword)) {
    LOG_ERR("VOC", "Failed to remove vocabulary entry '%s'", headword.c_str());
    return;
  }
  entries.erase(entries.begin() + nav.selected);
  // Deleting shifts every later word's index, so the cached subtitles and
  // actionValues must be re-derived, not just trimmed.
  rebuildRowItems();

  // Move the selector up if the last word was deleted.
  if (nav.selected >= static_cast<int>(entries.size()) && nav.selected > 0) {
    nav.selected--;
  }
  nav.follow(listCount());
  // The deleted word was one of the due ones.
  dueCount =
      VocabularyFile::countDue(entries, VocabularyFile::currentDayNumber(), VocabularyFile::currentMinuteOfDay());
  reviewLabel = std::string(tr(STR_VOCABULARY_START_REVIEW)) + " (" + std::to_string(dueCount) + ")";
  requestUpdate(true);
}

void VocabularyActivity::toggleManageMode() {
  app.clearTapFlash();
  manageMode = !manageMode;
  if (manageMode) {
    selectedFlags.assign(entries.size(), false);
  }
  rebuildRowItems();
  requestUpdate();
}

void VocabularyActivity::deleteSelectedWords() {
  if (!manageMode) return;
  int removed = 0;
  // Erase from the back so earlier indices stay valid. `entries` is sorted,
  // so build the descending list of checked indices first.
  std::vector<int> toRemove;
  for (size_t i = 0; i < selectedFlags.size() && i < entries.size(); ++i) {
    if (selectedFlags[i]) toRemove.push_back(static_cast<int>(i));
  }
  if (toRemove.empty()) return;
  for (auto it = toRemove.rbegin(); it != toRemove.rend(); ++it) {
    const std::string headword = entries.at(*it).headword;
    if (VocabularyFile::remove(headword)) {
      entries.erase(entries.begin() + *it);
      ++removed;
    } else {
      LOG_ERR("VOC", "Failed to remove vocabulary entry '%s'", headword.c_str());
    }
  }
  manageMode = false;
  selectedFlags.clear();
  sortEntries();
  rebuildRowItems();
  dueCount =
      VocabularyFile::countDue(entries, VocabularyFile::currentDayNumber(), VocabularyFile::currentMinuteOfDay());
  reviewLabel = std::string(tr(STR_VOCABULARY_START_REVIEW)) + " (" + std::to_string(dueCount) + ")";
  nav.follow(listCount());
  requestUpdate(true);
}

// Starts the review session. It is pushed on the stack, so Back comes back
// to this screen with the notebook rescheduled.
void VocabularyActivity::startReview() {
  app.clearTapFlash();
  startActivityForResultWith<VocabularyReviewActivity>([this](const ActivityResult&) {
    // The session rescheduled the words it touched: read the notebook again
    // so the counters show the new state.
    reload();
    requestUpdate();
  });
}

// Draws the two counters and the review button above the list.
void VocabularyActivity::drawStatsBand(const Rect& band) {
  // The band is drawn straight to the renderer, before the list is built, so
  // any clip a previous screen left behind has to go: otherwise the counters
  // would silently be drawn outside it.
  renderer.clearClipRect();
  const int cardWidth = (band.width - STATS_CARD_GAP) / 2;
  if (cardWidth <= 0 || band.height <= 0) {
    return;
  }
  // Left: how many words the notebook holds.
  drawCounterCard(Rect{band.x, band.y, cardWidth, STATS_CARD_HEIGHT}, static_cast<int>(entries.size()),
                  tr(STR_VOCABULARY_SAVED), Book24Icon);
  // Right: how many of them are due for review.
  drawCounterCard(Rect{band.x + cardWidth + STATS_CARD_GAP, band.y, cardWidth, STATS_CARD_HEIGHT}, dueCount,
                  tr(STR_VOCABULARY_TO_REVIEW), BookmarkIcon);

  // The button under the counters starts the session. Its box is remembered
  // so the tap can be hit-tested outside the list.
  const Rect button{band.x, band.y + STATS_CARD_HEIGHT + REVIEW_BUTTON_GAP, band.width, REVIEW_BUTTON_HEIGHT};
  reviewButton = button;
  // Active style: a filled black bar, the primary action on this screen.
  GUI.drawActionButton(renderer, button, reviewLabel.c_str(), true);

  // Export to Anki: outlined (secondary), with the last result shown in place
  // of the label for a few seconds.
  const Rect exportRect{band.x, button.y + REVIEW_BUTTON_HEIGHT + EXPORT_BUTTON_GAP, band.width, EXPORT_BUTTON_HEIGHT};
  exportButton = exportRect;
  const char* exportText = tr(STR_VOCABULARY_EXPORT_ANKI);
  if (!exportMessage.empty() && static_cast<long>(millis() - exportMessageUntil) >= 0) {
    exportMessage.clear();
  }
  if (!exportMessage.empty()) {
    exportText = exportMessage.c_str();
  }
  GUI.drawActionButton(renderer, exportRect, exportText, false);
}

// One compact counter card: solid black background, white icon, and the label
// followed by its count on a single row -- label first, number after.
void VocabularyActivity::drawCounterCard(const Rect& rect, const int value, const char* label, const uint8_t* icon) {
  if (rect.width <= 0 || rect.height <= 0 || !label) {
    return;
  }
  renderer.clearClipRect();
  renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 2, STATS_CARD_RADIUS, true);

  int textX = rect.x + STATS_ICON_PAD;
  const int baseline = rect.y + STATS_CARD_TEXT_TOP;
  // White icon on the black fill.
  if (icon) {
    renderer.drawIconInverted(icon, textX, rect.y + (rect.height - STATS_ICON_SIZE) / 2, STATS_ICON_SIZE);
    textX += STATS_ICON_SIZE + STATS_ICON_PAD / 2;
  }
  // Label first, in white.
  renderer.drawText(STATS_FONT_ID, textX, baseline, label, /*black=*/false, EpdFontFamily::BOLD);
  textX += renderer.getTextWidth(STATS_FONT_ID, label, EpdFontFamily::BOLD) + STATS_ICON_PAD / 2;
  // Count right after the label, also white.
  char number[16];
  std::snprintf(number, sizeof(number), "%d", value);
  renderer.drawText(STATS_FONT_ID, textX, baseline, number, /*black=*/false, EpdFontFamily::BOLD);
}

// Section label: the caption with the total written immediately after it in
// brackets (not right-aligned), underlined by a hairline rule.
void VocabularyActivity::drawSectionLabel(const Rect& band, const char* label, const int count) {
  renderer.clearClipRect();
  if (band.width <= 0 || band.height <= 0) return;
  const int baseline = band.y + (band.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
  renderer.drawText(UI_10_FONT_ID, band.x, baseline, label, true, EpdFontFamily::BOLD);
  const int labelWidth = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD);
  char number[32];
  std::snprintf(number, sizeof(number), "  (%d)", count);
  renderer.drawText(UI_10_FONT_ID, band.x + labelWidth, baseline, number);
  renderer.drawLine(band.x, band.y + band.height - 1, band.x + band.width, band.y + band.height - 1);
}

void VocabularyActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = uiThemeMetrics(usesUpstreamStyle());
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the title band render() paints.
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});

  // The counters and the two action buttons come first: they take the top of
  // the content area, the list takes what is left.
  const int bandHeight =
      STATS_CARD_HEIGHT + REVIEW_BUTTON_GAP + REVIEW_BUTTON_HEIGHT + EXPORT_BUTTON_GAP + EXPORT_BUTTON_HEIGHT;
  const fui::Rect band = screen.takeTop(static_cast<int16_t>(bandHeight));
  drawStatsBand(Rect{band.x, band.y, band.width, band.height});

  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (entries.empty()) {
    screen.centeredText(tr(STR_VOCABULARY_EMPTY), screen.theme().bodyText);
    return;
  }

  // Section caption with the total right after the text. Two small buttons on
  // the row's right edge cycle sort order and toggle multi-select management.
  const fui::Rect labelBand = screen.takeTop(static_cast<int16_t>(SECTION_LABEL_HEIGHT));
  drawSectionLabel(Rect{labelBand.x, labelBand.y, labelBand.width, labelBand.height}, tr(STR_VOC_SECTION_WORDS),
                   static_cast<int>(entries.size()));
  const char* sortText = sortOrder == 0 ? tr(STR_VOCAB_SORT_NEWEST) : tr(STR_VOCAB_SORT_OLDEST);
  const int sortW = renderer.getTextWidth(SMALL_FONT_ID, sortText) + 16;
  const int manageW = renderer.getTextWidth(SMALL_FONT_ID, manageMode ? tr(STR_VOCABULARY_DONE)
                                                                     : tr(STR_VOCABULARY_MANAGE)) + 16;
  sortButton = Rect{labelBand.x + labelBand.width - manageW - sortW - 8, labelBand.y, sortW, labelBand.height};
  manageButton = Rect{labelBand.x + labelBand.width - manageW, labelBand.y, manageW, labelBand.height};
  GUI.drawActionButton(renderer, sortButton, sortText, false);
  GUI.drawActionButton(renderer, manageButton, manageMode ? tr(STR_VOCABULARY_DONE) : tr(STR_VOCABULARY_MANAGE),
                       manageMode);
  screen.spacer(static_cast<int16_t>(SECTION_LABEL_GAP));

  if (manageMode) {
    // Management bar pinned to the bottom: select-all + delete-selected.
    const int barHeight = renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing * 2;
    const fui::Rect helpBand = screen.takeBottom(static_cast<int16_t>(barHeight));
    deleteBar = Rect{helpBand.x, helpBand.y, helpBand.width, helpBand.height};
    const int half = helpBand.width / 2 - 4;
    selectAllButton = Rect{helpBand.x, helpBand.y, half, helpBand.height};
    const Rect delRect{helpBand.x + helpBand.width / 2 + 4, helpBand.y, half, helpBand.height};
    GUI.drawActionButton(renderer, selectAllButton, tr(STR_VOCABULARY_SELECT_ALL), false);
    GUI.drawActionButton(renderer, delRect, tr(STR_VOCABULARY_DELETE_SELECTED), true);
    deleteBar = delRect;  // tap target is the delete button only
  } else {
    deleteBar = Rect{0, 0, 0, 0};
    selectAllButton = Rect{0, 0, 0, 0};
    // The old "long-press a word to delete it" hint is intentionally gone:
    // deletion now happens in the explicit management mode above.
  }

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  // Tap opens the word; long-press deletes it.
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.valueInset = 8;  // air between the trailing chevron and the row edge
  syncListViewport(screen, props);
  screen.list(props);
}
