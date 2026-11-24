#include "QuotesActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "MappedInputManager.h"
#include "QuoteViewerActivity.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "components/icons/book24.h"
#include "components/icons/bookmark.h"
#include "fontIds.h"
#include "util/TimeUtils.h"

namespace fui = freeink::ui;

namespace {
// Short YYYY-MM-DD stamp shown as a pill on each saved sentence, as in the redesign.
std::string quoteDate(const uint32_t epochSeconds) {
  std::tm local{};
  if (epochSeconds == 0 || !TimeUtils::getLocalDateTime(epochSeconds, local)) return std::string();
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
  return std::string(buf);
}
// Export-to-Clippings button directly under the tabs. High-DPI friendly size,
// matching the secondary button treatment in VocabularyActivity. The former
// counter band (book name + count) was a mostly empty row the reader asked to
// remove, so the export button now sits flush under the tab bar.
constexpr int EXPORT_BUTTON_HEIGHT = 60;
constexpr int EXPORT_BAND_EXTRA = EXPORT_BUTTON_HEIGHT;
// How long the export result stays on the button.
constexpr unsigned long EXPORT_MESSAGE_MS = 3000;
}  // namespace

QuotesActivity::QuotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiTabListActivity("Quotes", renderer, mappedInput, /*wantsTouchLongPress=*/false) {}

void QuotesActivity::onEnter() {
  reload();
  UiTabListActivity::onEnter();
}

void QuotesActivity::reload() {
  armedIndex = -1;
  QuotesFile::load(quotes);
  // Newest first: the last saved sentence is the one on the reader's mind.
  std::reverse(quotes.begin(), quotes.end());
  rebuildRows();
}

const char* QuotesActivity::tabLabel(const int index) const {
  switch (index) {
    case BooksTab:
      return tr(STR_QUOTES_BOOKS);
    case FavoritesTab:
      return tr(STR_QUOTES_FAVORITES);
    default:
      return tr(STR_QUOTES_ALL);
  }
}

void QuotesActivity::onTabAction(const int index) {
  if (dialog.isActive() || index < 0 || index >= TabCount) {
    return;
  }
  if (tab_ != index) {
    armedIndex = -1;
    tab_ = index;
    bookFilter_ = -1;  // switching tabs leaves a drilled-in book
    rebuildRows();
    auto& n = activeNav();
    n.selected = 0;
    n.followOnBuild = true;
    requestUpdate();
  }
  app.clearTapFlash();
}

void QuotesActivity::stepTab(const int direction) {
  onTabAction((tab_ + (direction < 0 ? TabCount - 1 : 1)) % TabCount);
}

void QuotesActivity::rebuildRows() {
  rows.clear();
  rowSubtitles.clear();
  rowItems.clear();

  const int total = static_cast<int>(quotes.size());
  if (tab_ == BooksTab) {
    // One row per distinct source title, newest quote's order preserved.
    std::vector<std::string> seen;
    for (int i = 0; i < total; ++i) {
      const std::string& book = quotes[i].book;
      if (std::find(seen.begin(), seen.end(), book) != seen.end()) {
        continue;
      }
      seen.push_back(book);
      int count = 0;
      int favs = 0;
      for (const auto& q : quotes) {
        if (q.book == book) {
          ++count;
          if (q.favorite) ++favs;
        }
      }
      Row row;
      row.kind = RowKind::Book;
      row.bookKey = book;
      row.label = book.empty() ? tr(STR_QUOTES_ALL) : book;
      char sub[80];
      if (favs > 0) {
        std::snprintf(sub, sizeof(sub), "%d / %d %s", count, favs, tr(STR_QUOTES_FAVORITES));
      } else {
        std::snprintf(sub, sizeof(sub), tr(STR_QUOTES_BOOK_COUNT), count);
      }
      row.subtitle = sub;
      rows.push_back(std::move(row));
    }
  } else {
    for (int i = 0; i < total; ++i) {
      const auto& q = quotes[i];
      if (tab_ == FavoritesTab && !q.favorite) {
        continue;
      }
      if (tab_ == AllTab && bookFilter_ >= 0 && q.book != filteredBook) {
        continue;
      }
      Row row;
      row.kind = RowKind::Quote;
      row.index = i;
      row.key = q.key;
      row.favorite = q.favorite;
      row.label = q.sentence;
      // Source sits in the gray subtitle; the save date is painted as a
      // separate light-gray pill at the card's bottom right (see buildScreen).
      row.subtitle = q.book;
      row.date = quoteDate(q.timestamp);
      rows.push_back(std::move(row));
    }
  }

  rowSubtitles.reserve(rows.size());
  rowItems.reserve(rows.size());
  for (const auto& row : rows) {
    rowSubtitles.push_back(row.subtitle);
    fui::ListItem item;
    item.label = row.label.c_str();
    item.subtitle = rowSubtitles.back().c_str();
    // The reference cards have no leading icon and no chevron; the date pill
    // (quotes) or book count (books) supplies the secondary information.
    item.icon = {};
    item.value = nullptr;
    item.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(item);
  }
}

void QuotesActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = uiThemeMetrics(usesUpstreamStyle());
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});

  buildTabBar(screen);

  // Export button flush under the tabs. The old counter band (book name +
  // count on an otherwise empty row) has been removed at the reader's request;
  // the reclaimed height goes into more generous sentence row spacing below.
  renderer.clearClipRect();
  const fui::Rect band = screen.takeTop(static_cast<int16_t>(EXPORT_BAND_EXTRA));

  // Export button: outlined secondary action. Its label is replaced by the
  // transient result message for a few seconds after a tap.
  const Rect exportRect{band.x, band.y, band.width, EXPORT_BUTTON_HEIGHT};
  exportButton = exportRect;
  const char* exportText = tab_ == BooksTab ? tr(STR_QUOTES_EXPORT_BY_BOOK) : tr(STR_QUOTES_EXPORT_CLIPPINGS);
  if (!exportMessage.empty() && static_cast<long>(millis() - exportMessageUntil) >= 0) {
    exportMessage.clear();
  }
  if (!exportMessage.empty()) {
    exportText = exportMessage.c_str();
  }
  GUI.drawActionButton(renderer, exportRect, exportText, false);

  if (rows.empty()) {
    const char* msg = tab_ == FavoritesTab ? tr(STR_QUOTES_EMPTY_FAV) : tr(STR_QUOTES_EMPTY);
    screen.centeredText(msg, screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  // Taller rows with more breathing room between saved sentences: the source
  // sits in the gray subtitle under the bold sentence, with the date painted
  // as a separate capsule at the card's bottom right. The removed counter band
  // is given back here as extra line spacing, as the reader requested.
  props.rowHeight = static_cast<int16_t>(std::max<int>(84, renderer.getLineHeight(UI_12_FONT_ID) * 3 + 12));
  props.rowGap = static_cast<int16_t>(std::max<int>(12, metrics.verticalSpacing + 4));
  // Same minimal card treatment as the vocabulary screen: white rounded card,
  // bold sentence, gray source and a dark bar on the pressed card's left.
  applyCardListStyle(props, screen.theme());
  // A saved sentence is the headline of this screen: one step up from the
  // default body font so it reads comfortably on e-paper.
  props.labelText.font = UI_12_FONT_ID;
  syncTabListViewport(screen, props);
  const fui::Rect body = screen.body();
  listRect = Rect{body.x, body.y, body.width, body.height};
  swipeRowHeight = props.rowHeight;
  swipeRowGap = props.rowGap > 0 ? props.rowGap : 0;
  screen.list(props);

  // The SDK's list has no per-row badge slot, so paint the light-gray date
  // capsules over each card after the list has drawn. Row geometry matches
  // the fixed rowHeight/rowGap handed to props above.
  const int rowStep = props.rowHeight + swipeRowGap;
  if (rowStep > 0) {
    const int first = std::max(0, activeNav().top);
    const int visible = body.height / rowStep + 1;
    const int last = std::min<int>(static_cast<int>(rows.size()), first + visible);
    // 8px UI font: the date is a secondary cue, not a competing headline.
    const int pillHeight = renderer.getLineHeight(SMALL_FONT_ID) + 4;
    for (int i = first; i < last; ++i) {
      if (rows[i].date.empty()) continue;
      const int dateW = renderer.getTextWidth(SMALL_FONT_ID, rows[i].date.c_str()) + 12;
      const Rect pill{body.x + body.width - dateW - 10,
                      body.y + (i - first) * rowStep + props.rowHeight - pillHeight - 8, dateW, pillHeight};
      renderer.fillRoundedRect(pill.x, pill.y, pill.width, pill.height, pill.height / 2, Color::LightGray);
      renderer.drawText(SMALL_FONT_ID, pill.x + 6, pill.y + 2, rows[i].date.c_str(), true);
    }
  }
}

void QuotesActivity::activateIndex(const int index) {
  if (dialog.isActive() || index < 0 || index >= static_cast<int>(rows.size())) {
    return;
  }
  app.clearTapFlash();
  // Select first, open on the second tap of the same card. A single stray
  // touch should not throw the reader straight into a sentence.
  if (armedIndex != index) {
    armedIndex = index;
    requestUpdate();
    return;
  }
  armedIndex = -1;
  const Row& row = rows[index];
  if (row.kind == RowKind::Book) {
    // Drill into this book's quotes via the ALL tab.
    filteredBook = row.bookKey;
    bookFilter_ = 0;
    tab_ = AllTab;
    rebuildRows();
    auto& n = activeNav();
    n.selected = 0;
    n.followOnBuild = true;
    requestUpdate();
    return;
  }
  openQuote(index);
}

void QuotesActivity::onRowLongPress(const int index) {
  if (dialog.isActive() || index < 0 || index >= static_cast<int>(rows.size())) {
    return;
  }
  armedIndex = -1;
  if (rows[index].kind == RowKind::Quote) {
    openQuote(index);
  }
}

void QuotesActivity::openQuote(int index) {
  const Row& row = rows[index];
  int quoteIndex = row.index;
  if (quoteIndex < 0 || quoteIndex >= static_cast<int>(quotes.size())) {
    return;
  }
  // Build the visible quote list in row order so Prev/Next in the viewer
  // follows the current tab and book drill-down, then find this row's place.
  std::vector<QuotesFile::Quote> visible;
  visible.reserve(rows.size());
  int viewerIndex = 0;
  for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
    if (rows[i].kind != RowKind::Quote) continue;
    const auto& q = quotes[rows[i].index];
    if (i == index) viewerIndex = static_cast<int>(visible.size());
    visible.push_back(q);
  }
  app.clearTapFlash();
  startActivityForResultWith<QuoteViewerActivity>([this](const ActivityResult&) { reload(); }, std::move(visible),
                                                  viewerIndex);
}

void QuotesActivity::deleteQuoteAt(const int index) {
  armedIndex = -1;
  if (index < 0 || index >= static_cast<int>(rows.size())) return;
  const Row& row = rows[index];
  if (row.kind != RowKind::Quote || row.index < 0 || row.index >= static_cast<int>(quotes.size())) return;
  const std::string key = quotes[row.index].key;
  if (!QuotesFile::remove(key)) return;
  app.clearTapFlash();
  reload();
  auto& n = activeNav();
  n.selected = std::clamp(index, 0, std::max(0, static_cast<int>(rows.size()) - 1));
  requestUpdate();
}

bool QuotesActivity::handleSwipeDelete() {
  if (dialog.isActive() || rows.empty()) return false;
  int sx = 0, sy = 0, ex = 0, ey = 0;
  if (!mappedInput.decodeSwipe(sx, sy, ex, ey)) return false;
  const int dx = ex - sx, dy = ey - sy;
  if (dx >= 0 || std::abs(dx) < std::abs(dy)) return false;  // leftward, horizontal-dominant
  if (listRect.height <= 0 || swipeRowHeight <= 0 || sy < listRect.y || sy >= listRect.y + listRect.height) return false;
  auto& n = activeNav();
  const int rowStep = swipeRowHeight + swipeRowGap;
  if (rowStep <= 0) return false;
  const int index = n.top + (sy - listRect.y) / rowStep;
  if (index < 0 || index >= static_cast<int>(rows.size()) || rows[index].kind != RowKind::Quote) return false;
  armedIndex = -1;
  const char* options[] = {tr(STR_CANCEL), tr(STR_QUOTES_DELETE)};
  dialog.show(tr(STR_QUOTES_DELETE_CONFIRM), options, 2, 0, [this, index](int choice) {
    if (choice == 1) deleteQuoteAt(index);
    requestUpdate();
  });
  requestUpdate();
  return true;
}

void QuotesActivity::exportToClippings() {
  app.clearTapFlash();
  // In the BOOKS tab the button exports one clippings file per book; every
  // other tab writes the whole notebook into a single file.
  const bool byBook = (tab_ == BooksTab);
  constexpr const char* kPath = "/My Clippings.txt";
  constexpr const char* kDir = "/Clippings";
  const int count = byBook ? QuotesFile::exportClippingsByBook(kDir) : QuotesFile::exportClippings(kPath);
  if (count > 0) {
    char msg[64];
    const char* doneFmt = byBook ? tr(STR_QUOTES_EXPORT_BOOKS_DONE) : tr(STR_QUOTES_EXPORT_DONE);
    std::snprintf(msg, sizeof(msg), doneFmt, count);
    exportMessage = msg;
  } else if (count == 0) {
    exportMessage = tr(STR_QUOTES_EXPORT_EMPTY);
  } else {
    exportMessage = tr(STR_QUOTES_EXPORT_FAIL);
  }
  exportMessageUntil = millis() + EXPORT_MESSAGE_MS;
  requestUpdate();
}

bool QuotesActivity::handleCustomInput() {
  if (dialog.handleInput(mappedInput, [this] { requestUpdate(); })) {
    return true;
  }
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty) && exportButton.width > 0 && exportButton.height > 0 &&
      tx >= exportButton.x && tx < exportButton.x + exportButton.width && ty >= exportButton.y &&
      ty < exportButton.y + exportButton.height) {
    exportToClippings();
    return true;
  }
  if (handleSwipeDelete()) {
    return true;
  }
  return false;
}

bool QuotesActivity::handleButtons() {
  if (dialog.isActive()) {
    return false;  // the popup owns the buttons
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Back from a drilled-in book returns to the BOOKS tab.
    if (tab_ == AllTab && bookFilter_ >= 0) {
      bookFilter_ = -1;
      filteredBook.clear();
      tab_ = BooksTab;
      rebuildRows();
      auto& n = activeNav();
      n.selected = 0;
      n.followOnBuild = true;
      requestUpdate();
      return true;
    }
    activityManager.goToApps();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ringPos() == 0) {
      stepTab(1);
    } else {
      activateIndex(ringPos() - 1);
    }
    return true;
  }
  return false;
}

void QuotesActivity::render(RenderLock&& lock) {
  if (dialog.processRender(renderer, mappedInput)) {
    return;
  }
  UiTabListActivity::render(std::move(lock));
}
