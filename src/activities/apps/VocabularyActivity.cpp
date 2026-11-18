#include "VocabularyActivity.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "activities/reader/DictionaryDefinitionActivity.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

#include "VocabularyReviewActivity.h"

namespace fui = freeink::ui;

namespace {

// Hold time on Confirm that opens the delete confirmation.
constexpr unsigned long ENTER_ACTIONS_MODE_MS = 700;

// The counter band above the list: two cards side by side.
constexpr int STATS_CARD_HEIGHT = 92;
constexpr int STATS_CARD_GAP = 12;
// Corner radius of the counter cards.
constexpr int STATS_CARD_RADIUS = 6;
// Where the big number sits inside the card, and how much room the small
// label keeps at the bottom.
constexpr int STATS_CARD_NUMBER_TOP = 14;
constexpr int STATS_CARD_LABEL_BOTTOM = 12;
// The review button under the counters.
constexpr int REVIEW_BUTTON_HEIGHT = 44;
constexpr int REVIEW_BUTTON_GAP = 10;
// The font the counters are drawn in.
constexpr int STATS_FONT_ID = NOTOSANS_18_FONT_ID;

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
  rebuildRowItems();
  dueCount = VocabularyFile::countDue(entries, VocabularyFile::currentDayNumber(), VocabularyFile::currentMinuteOfDay());
  // The button carries the count, so its label is rebuilt with it.
  reviewLabel = std::string(tr(STR_VOCABULARY_START_REVIEW)) + " (" + std::to_string(dueCount) + ")";
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
  for (const auto& entry : entries) {
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
    item.icon = listIconFor(UIIcon::Bookmark, 32);  // subtitle rows carry the larger icon
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

  const auto& entry = entries.at(index);
  // The row only holds an excerpt; the full definition stays on disk.
  std::string sentence;
  std::string definition;
  VocabularyFile::loadText(entry.headword, sentence, definition);
  startActivityForResultWith<DictionaryDefinitionActivity>([this](const ActivityResult&) { requestUpdate(); },
                                                           entry.headword, definition, false, entry.book, sentence);
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
    if (button.width > 0 && button.height > 0 && tx >= button.x && tx < button.x + button.width &&
        ty >= button.y && ty < button.y + button.height) {
      startReview();
      return true;
    }
  }
  return false;
}

bool VocabularyActivity::handleButtons() {
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, ENTER_ACTIONS_MODE_MS)) {
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
  dueCount = VocabularyFile::countDue(entries, VocabularyFile::currentDayNumber(), VocabularyFile::currentMinuteOfDay());
  reviewLabel = std::string(tr(STR_VOCABULARY_START_REVIEW)) + " (" + std::to_string(dueCount) + ")";
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
                  tr(STR_VOCABULARY_SAVED));
  // Right: how many of them are due for review.
  drawCounterCard(Rect{band.x + cardWidth + STATS_CARD_GAP, band.y, cardWidth, STATS_CARD_HEIGHT}, dueCount,
                  tr(STR_VOCABULARY_TO_REVIEW));

  // The button under the counters starts the session. Its box is remembered
  // so the tap can be hit-tested outside the list.
  const Rect button{band.x, band.y + STATS_CARD_HEIGHT + REVIEW_BUTTON_GAP, band.width, REVIEW_BUTTON_HEIGHT};
  reviewButton = button;
  GUI.drawActionButton(renderer, button, reviewLabel.c_str(), false);
}

// One counter card: the number, and the small label under it.
void VocabularyActivity::drawCounterCard(const Rect& rect, const int value, const char* label) {
  if (rect.width <= 0 || rect.height <= 0 || !label) {
    return;
  }
  renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 1, STATS_CARD_RADIUS, true);

  char number[16];
  std::snprintf(number, sizeof(number), "%d", value);
  const int numberWidth = renderer.getTextWidth(STATS_FONT_ID, number, EpdFontFamily::BOLD);
  renderer.drawText(STATS_FONT_ID, rect.x + (rect.width - numberWidth) / 2, rect.y + STATS_CARD_NUMBER_TOP, number,
                    true, EpdFontFamily::BOLD);

  const int labelWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
  renderer.drawText(UI_10_FONT_ID, rect.x + (rect.width - labelWidth) / 2,
                    rect.y + rect.height - STATS_CARD_LABEL_BOTTOM - renderer.getLineHeight(UI_10_FONT_ID), label);
}

void VocabularyActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = uiThemeMetrics(usesUpstreamStyle());
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the title band render() paints.
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});

  // The counters and the review button come first: they take the top of the
  // content area, the list takes what is left.
  const int bandHeight = STATS_CARD_HEIGHT + REVIEW_BUTTON_GAP + REVIEW_BUTTON_HEIGHT;
  const fui::Rect band = screen.takeTop(static_cast<int16_t>(bandHeight));
  drawStatsBand(Rect{band.x, band.y, band.width, band.height});

  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (entries.empty()) {
    screen.centeredText(tr(STR_VOCABULARY_EMPTY), screen.theme().bodyText);
    return;
  }

  // On a touch board the long press is the only way to delete, so say so.
  if (mappedInput.hasTouch()) {
    const int helpLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const fui::Rect helpBand = screen.takeBottom(static_cast<int16_t>(helpLineHeight + metrics.verticalSpacing));
    GUI.drawHelpText(renderer, Rect{helpBand.x, helpBand.y + metrics.verticalSpacing, helpBand.width, helpLineHeight},
                     tr(STR_VOCABULARY_LONG_PRESS_DELETE));
  }

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  // Tap opens the word; long-press deletes it.
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  syncListViewport(screen, props);
  screen.list(props);
}
