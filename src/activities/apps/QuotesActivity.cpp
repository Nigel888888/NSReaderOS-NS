#include "QuotesActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "QuoteViewerActivity.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "components/icons/book24.h"
#include "components/icons/bookmark.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr int ROW_ICON_SIZE = 32;
constexpr int COUNTER_HEIGHT = 34;
}  // namespace

QuotesActivity::QuotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiTabListActivity("Quotes", renderer, mappedInput, /*wantsTouchLongPress=*/false) {}

void QuotesActivity::onEnter() {
  reload();
  UiTabListActivity::onEnter();
}

void QuotesActivity::reload() {
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

  // Counters are computed over the whole notebook, not the current tab.
  const int total = static_cast<int>(quotes.size());
  int bookCount = 0;
  const int favCount = static_cast<int>(
      std::count_if(quotes.begin(), quotes.end(), [](const QuotesFile::Quote& q) { return q.favorite; }));
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
    bookCount = static_cast<int>(rows.size());
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
      row.subtitle = q.book;
      rows.push_back(std::move(row));
    }
    bookCount = total;
  }
  (void)bookCount;

  // The count line is rendered above the list in buildScreen(); keep it here
  // so the values stay in one place.
  char counter[96];
  std::snprintf(counter, sizeof(counter), tr(STR_QUOTES_COUNT), total,
                static_cast<int>(std::count_if(quotes.begin(), quotes.end(),
                                               [](const QuotesFile::Quote& q) { return !q.book.empty(); })),
                favCount);
  countLine = counter;

  rowSubtitles.reserve(rows.size());
  rowItems.reserve(rows.size());
  for (const auto& row : rows) {
    rowSubtitles.push_back(row.subtitle);
    fui::ListItem item;
    item.label = row.label.c_str();
    item.subtitle = rowSubtitles.back().c_str();
    item.icon = row.kind == RowKind::Book ? listIconFor(UIIcon::Book, ROW_ICON_SIZE)
                                          : listIconFor(UIIcon::Bookmark, ROW_ICON_SIZE);
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

  // Count line under the tabs (or the drilled-in book title).
  renderer.clearClipRect();
  const fui::Rect band = screen.takeTop(static_cast<int16_t>(COUNTER_HEIGHT));
  const char* line = (tab_ == AllTab && bookFilter_ >= 0) ? filteredBook.c_str() : countLine.c_str();
  const int tw = renderer.getTextWidth(UI_10_FONT_ID, line);
  renderer.drawText(UI_10_FONT_ID, band.x + (band.width - tw) / 2,
                    band.y + (band.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2, line);

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
  syncTabListViewport(screen, props);
  screen.list(props);
}

void QuotesActivity::activateIndex(const int index) {
  if (dialog.isActive() || index < 0 || index >= static_cast<int>(rows.size())) {
    return;
  }
  app.clearTapFlash();
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

bool QuotesActivity::handleCustomInput() {
  if (dialog.handleInput(mappedInput, [this] { requestUpdate(); })) {
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
