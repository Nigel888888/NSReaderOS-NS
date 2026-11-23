#include "VocabularyActivity.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

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

// The segmented capsule splits the counter band into two equal halves.
constexpr int STATS_CARD_GAP = 12;
constexpr int STATS_ICON_SIZE = 24;
constexpr int STATS_ICON_PAD = 10;
// How long the export result stays on the button.
constexpr unsigned long EXPORT_MESSAGE_MS = 3000;
// The font the counters are drawn in. One step smaller so the label + number
// sit comfortably inside the capsule and read as a compact statistic.
constexpr int STATS_FONT_ID = NOTOSANS_16_FONT_ID;
// Extra breathing room between the button panel and the first word card.
constexpr int BAND_TO_SECTION_GAP = 16;
// Minimal-style dashboard: a segmented counter capsule plus a 2x2 action grid
// (Start review / Export Anki / Manage words / Sort order). Management and
// sorting sit right beside Review and Export, as requested.
constexpr int CAPSULE_HEIGHT = 56;
constexpr int CAPSULE_GAP = 14;
constexpr int ACTION_BUTTON_HEIGHT = 58;
constexpr int ACTION_BUTTON_GAP = 12;

}  // namespace

VocabularyActivity::VocabularyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    // Touch long-press is OFF on purpose: wasScreenLongPress() calls
    // suppressTouchContact(), which sets touchSuppressed and makes wasSwipe()
    // return false for the rest of that contact. On the slow e-ink loop a
    // left-swipe dwells past TOUCH_LONG_PRESS_MS before crossing the tap slop,
    // so long-press fired mid-swipe and silently killed the swipe delete.
    // Quotes has this flag off too and its swipe delete works. Deletion here
    // is via left-swipe; the long-press path is intentionally disabled.
    : UiListActivity("Vocabulary", renderer, mappedInput, /*wantsTouchLongPress=*/false) {}

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

// Switches the counter capsule between every saved word and only the words
// the scheduler considers due. Both halves are tappable; the list and the
// review navigation are rebuilt from the same visible-row map.
void VocabularyActivity::setDueOnly(const bool next) {
  if (dueOnly == next) return;
  dueOnly = next;
  nav.selected = 0;
  nav.followOnBuild = true;
  rebuildRowItems();
  requestUpdate();
}

// Derives rowSubtitles/rowItems from `entries`. Called whenever `entries`
// changes (onEnter() load, post-delete) so buildScreen() reuses the cached
// rows on every repaint.
void VocabularyActivity::rebuildRowItems() {
  rowSubtitles.clear();
  rowItems.clear();
  rowEntryIndex.clear();
  // Reserve up front: the ListItem strings point into rowSubtitles, so the
  // vector must not reallocate while the items are being built.
  rowSubtitles.reserve(entries.size());
  rowItems.reserve(entries.size());
  rowEntryIndex.reserve(entries.size());

  // "To review" segment shows only the words the scheduler considers due now;
  // "Collected" shows every word. rowEntryIndex maps each visible row back to
  // its position in `entries` so taps/deletes still hit the right word.
  const int today = VocabularyFile::currentDayNumber();
  const int nowMinute = VocabularyFile::currentMinuteOfDay();
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto& entry = entries[i];
    if (dueOnly && !study::Scheduler::isDue(entry.card, today, nowMinute)) continue;
    rowEntryIndex.push_back(static_cast<int>(i));

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
    // No leading badge icon in the minimal card design. Manage mode still
    // needs its checkbox so batch deletion stays usable.
    if (manageMode) {
      item.icon = GUI.checkboxIcon(i < selectedFlags.size() && selectedFlags[i]);
      item.value = nullptr;
    } else {
      item.icon = {};
      item.value = ">";
    }
    item.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(item);
  }

  // Keep the checkbox flags sized to the full notebook (indexed by entries
  // position); they are only initialised once on entering manage mode.
  if (manageMode && selectedFlags.size() != entries.size()) {
    selectedFlags.assign(entries.size(), false);
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
    // Checkbox flags are indexed by the full notebook, not by visible rows.
    if (index >= 0 && index < static_cast<int>(rowEntryIndex.size())) {
      const int entryIndex = rowEntryIndex.at(index);
      if (entryIndex >= 0 && entryIndex < static_cast<int>(selectedFlags.size())) {
        selectedFlags[entryIndex] = !selectedFlags[entryIndex];
        rebuildRowItems();
        requestUpdate();
      }
    }
    return;
  }

  // Map the visible row to its word, and navigate within the visible list.
  const int entryIndex = rowEntryIndex.at(index);
  const auto& entry = entries.at(entryIndex);
  // The row only holds an excerpt; the full definition stays on disk.
  std::string sentence;
  std::string definition;
  VocabularyFile::loadText(entry.headword, sentence, definition);
  // Hand the card the currently visible word list plus this position so
  // Prev/Next steps through what the user is looking at (all words, or only
  // the due ones) without returning here.
  std::vector<std::string> navWords;
  navWords.reserve(rowEntryIndex.size());
  for (const int ei : rowEntryIndex) {
    navWords.push_back(entries.at(ei).headword);
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
    if (index >= 0 && index < static_cast<int>(rowEntryIndex.size())) {
      const int entryIndex = rowEntryIndex.at(index);
      if (entryIndex >= 0 && entryIndex < static_cast<int>(selectedFlags.size())) {
        selectedFlags[entryIndex] = !selectedFlags[entryIndex];
        rebuildRowItems();
        requestUpdate();
      }
    }
    return;
  }
  showDeleteConfirmation();
}

// Left-swipe any row to open the delete confirmation for that word. The swipe
// is horizontal, so it never collides with the up/down scroll gesture or the
// left-edge back gesture (which is right-swipe anchored at the edge).
bool VocabularyActivity::handleSwipeDelete() {
  if (confirmPopup.isActive() || manageMode || entries.empty()) return false;
  int sx = 0, sy = 0, ex = 0, ey = 0;
  if (!mappedInput.decodeSwipe(sx, sy, ex, ey)) return false;
  const int dx = ex - sx, dy = ey - sy;
  if (dx >= 0 || std::abs(dx) < std::abs(dy)) return false;  // leftward, horizontal-dominant
  // Map the finger's start y to the row it began on using the exact fixed row
  // geometry recorded at build time (same as Quotes). The old estimate
  // (listRect.height / visibleRows) drifted off the real rows and silently
  // missed, which is why a left-swipe appeared to do nothing.
  if (listRect.height <= 0 || swipeRowHeight <= 0 || sy < listRect.y ||
      sy >= listRect.y + listRect.height) {
    return false;
  }
  auto& n = activeNav();
  const int rowStep = swipeRowHeight + swipeRowGap;
  if (rowStep <= 0) return false;
  const int index = n.top + (sy - listRect.y) / rowStep;
  if (index < 0 || index >= listCount()) return false;
  app.clearTapFlash();
  n.selected = index;
  showDeleteConfirmation();
  return true;
}

bool VocabularyActivity::handleCustomInput() {
  // Delete confirmation popup.
  if (confirmPopup.handleInput(mappedInput, [this] { requestUpdate(); })) {
    return true;
  }
  if (handleSwipeDelete()) {
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
    // Both halves of the counter capsule are real tabs now: tapping "to
    // review" filters the list to due words and moves the dark pill.
    if (hit(collectedSegment)) {
      setDueOnly(false);
      return true;
    }
    if (hit(dueSegment)) {
      setDueOnly(true);
      return true;
    }
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
  const int entryIndex = rowEntryIndex.at(nav.selected);
  const std::string headword = entries.at(entryIndex).headword;
  if (!VocabularyFile::remove(headword)) {
    LOG_ERR("VOC", "Failed to remove vocabulary entry '%s'", headword.c_str());
    return;
  }
  entries.erase(entries.begin() + entryIndex);
  // Deleting shifts every later word's index, so the cached subtitles and the
  // row->entry map must be re-derived, not just trimmed.
  rebuildRowItems();

  // Move the selector up if the last visible row was deleted.
  if (nav.selected >= listCount() && nav.selected > 0) {
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

// Draws the segmented counter capsule and the 2x2 action grid above the list.
// The layout mirrors the redesigned phone mock-up: a single rounded capsule
// split into "collected / due" halves, then four side-by-side actions where
// Manage words and Sort order sit next to Start review and Export Anki.
void VocabularyActivity::drawStatsBand(const Rect& band) {
  renderer.clearClipRect();
  const int cardWidth = (band.width - STATS_CARD_GAP) / 2;
  if (cardWidth <= 0 || band.height <= 0) {
    return;
  }

  // Segmented control from the redesign: one light-gray rounded bar with a
  // dark rounded capsule over the active half. Both halves are tappable and
  // drive the list filter (see handleCustomInput()).
  const int capsuleRadius = CAPSULE_HEIGHT / 2;
  renderer.fillRoundedRect(band.x, band.y, band.width, CAPSULE_HEIGHT, capsuleRadius, Color::LightGray);
  collectedSegment = Rect{band.x, band.y, cardWidth, CAPSULE_HEIGHT};
  dueSegment = Rect{band.x + cardWidth + STATS_CARD_GAP, band.y, cardWidth, CAPSULE_HEIGHT};
  const Rect& active = dueOnly ? dueSegment : collectedSegment;
  renderer.fillRoundedRect(active.x, active.y, active.width, active.height, capsuleRadius, Color::DarkGray);
  // The active half carries white text; the idle half stays dark-on-gray.
  drawCounterCard(collectedSegment, static_cast<int>(entries.size()), tr(STR_VOCABULARY_SAVED), nullptr,
                  /*whiteText=*/!dueOnly);
  drawCounterCard(dueSegment, dueCount, tr(STR_VOCABULARY_TO_REVIEW), nullptr, /*whiteText=*/dueOnly);

  // 2x2 action grid. Row 1: Start review (primary, filled) + Export Anki
  // (secondary, outlined). Row 2: Manage words + Sort order (both outlined;
  // Manage shows filled while its mode is active).
  const int gridY = band.y + CAPSULE_HEIGHT + CAPSULE_GAP;
  const int halfW = (band.width - ACTION_BUTTON_GAP) / 2;
  const int row2Y = gridY + ACTION_BUTTON_HEIGHT + ACTION_BUTTON_GAP;
  reviewButton = Rect{band.x, gridY, halfW, ACTION_BUTTON_HEIGHT};
  exportButton = Rect{band.x + halfW + ACTION_BUTTON_GAP, gridY, halfW, ACTION_BUTTON_HEIGHT};
  manageButton = Rect{band.x, row2Y, halfW, ACTION_BUTTON_HEIGHT};
  sortButton = Rect{band.x + halfW + ACTION_BUTTON_GAP, row2Y, halfW, ACTION_BUTTON_HEIGHT};

  GUI.drawActionButton(renderer, reviewButton, reviewLabel.c_str(), true);

  const char* exportText = tr(STR_VOCABULARY_EXPORT_ANKI);
  if (!exportMessage.empty() && static_cast<long>(millis() - exportMessageUntil) >= 0) {
    exportMessage.clear();
  }
  if (!exportMessage.empty()) {
    exportText = exportMessage.c_str();
  }
  GUI.drawActionButton(renderer, exportButton, exportText, false);

  const char* manageText = manageMode ? tr(STR_VOCABULARY_DONE) : tr(STR_VOCABULARY_MANAGE);
  GUI.drawActionButton(renderer, manageButton, manageText, manageMode);
  const char* sortText = sortOrder == 0 ? tr(STR_VOCAB_SORT_NEWEST) : tr(STR_VOCAB_SORT_OLDEST);
  GUI.drawActionButton(renderer, sortButton, sortText, false);
}

// One counter half inside the capsule: label then count, centred as one row.
// `whiteText` selects white-on-black for the filled half, black-on-white for
// the empty half. `icon` is unused in the capsule layout but kept optional.
void VocabularyActivity::drawCounterCard(const Rect& rect, const int value, const char* label, const uint8_t* icon,
                                         const bool whiteText) {
  if (rect.width <= 0 || rect.height <= 0 || !label) {
    return;
  }
  renderer.clearClipRect();

  // Compose "LABEL count" so the pair can be centred as a unit.
  char number[16];
  std::snprintf(number, sizeof(number), "%d", value);
  const int pad = icon ? STATS_ICON_PAD : 0;
  const int iconW = icon ? STATS_ICON_SIZE + STATS_ICON_PAD / 2 : 0;
  const int labelW = renderer.getTextWidth(STATS_FONT_ID, label, EpdFontFamily::BOLD);
  const int numberW = renderer.getTextWidth(STATS_FONT_ID, number, EpdFontFamily::BOLD);
  const int gap = STATS_ICON_PAD / 2;
  const int textW = iconW + labelW + gap + numberW;
  int textX = rect.x + std::max<int>(pad, (rect.width - textW) / 2);

  const int lineHeight = renderer.getLineHeight(STATS_FONT_ID);
  const int baseline = rect.y + (rect.height - lineHeight) / 2;
  if (icon) {
    if (whiteText) {
      renderer.drawIconInverted(icon, textX, rect.y + (rect.height - STATS_ICON_SIZE) / 2, STATS_ICON_SIZE);
    }
    textX += iconW;
  }
  renderer.drawText(STATS_FONT_ID, textX, baseline, label, !whiteText, EpdFontFamily::BOLD);
  textX += labelW + gap;
  renderer.drawText(STATS_FONT_ID, textX, baseline, number, !whiteText, EpdFontFamily::BOLD);
}

void VocabularyActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = uiThemeMetrics(usesUpstreamStyle());
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the title band render() paints.
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});

  // The counter capsule and the 2x2 action grid come first: they take the top
  // of the content area, the list takes what is left.
  const int bandHeight = CAPSULE_HEIGHT + CAPSULE_GAP + ACTION_BUTTON_HEIGHT * 2 + ACTION_BUTTON_GAP;
  const fui::Rect band = screen.takeTop(static_cast<int16_t>(bandHeight));
  drawStatsBand(Rect{band.x, band.y, band.width, band.height});

  // Breathing room between the action panel and the first word card. The old
  // "我的生词" section caption is intentionally gone in the minimal redesign.
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing + BAND_TO_SECTION_GAP));

  if (rowItems.empty()) {
    screen.centeredText(tr(STR_VOCABULARY_EMPTY), screen.theme().bodyText);
    return;
  }

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
    // Deletion is now a left-swipe on the row (reveal a delete button); the
    // old long-press / management-mode hints are intentionally gone.
  }

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  // Tap opens the word; long-press deletes it.
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.valueInset = 8;  // air between the trailing chevron and the row edge
  // White rounded cards, gray subtitles, and a dark bar on the left of the
  // pressed card (shared with Quotes so both notebooks look the same).
  applyCardListStyle(props, screen.theme());
  // Give every row a fixed height, exactly like the Quotes screen. Without it
  // the framework grows rows to fit wrapped subtitles, so rows are unequal and
  // a swipe's start y cannot be mapped to a row by an even-height estimate —
  // the left-swipe delete then silently misses. A fixed height keeps rows
  // uniform, so start-y -> row index is exact. It is a minimum: the headword +
  // one subtitle line still fit comfortably.
  props.rowHeight = static_cast<int16_t>(std::max<int>(72, renderer.getLineHeight(UI_12_FONT_ID) * 3));
  props.rowGap = static_cast<int16_t>(std::max<int>(8, metrics.verticalSpacing));
  syncListViewport(screen, props);
  // Remember the exact area and row geometry the list is about to occupy, so a
  // left-swipe's start y maps to the row the finger began on.
  const fui::Rect body = screen.body();
  listRect = Rect{body.x, body.y, body.width, body.height};
  swipeRowHeight = props.rowHeight;
  swipeRowGap = props.rowGap > 0 ? props.rowGap : 0;
  screen.list(props);
}
