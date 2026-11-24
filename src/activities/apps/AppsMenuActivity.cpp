#include "AppsMenuActivity.h"

#include <I18n.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include "CrossPointSettings.h"
#include "InxItemLayout.h"
#include "OpdsServerStore.h"
#include "components/RickyPageUi.h"
#include "components/SubpageLayout.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/icons/inx_apps.h"
#ifdef RICKYOS_PRODUCT
#include "components/RickyAaIcons.h"
#include "components/icons/rickyAppIcons.h"
#endif
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {

using appVisibility::appBit;
using appVisibility::AppId;

struct AppEntry {
  AppId id;
  StrId titleId;
  UIIcon icon;
  void (ActivityManager::*open)();
};

constexpr AppEntry kAppEntries[] = {
    {AppId::FileTransfer, StrId::STR_FILE_TRANSFER, UIIcon::Transfer, &ActivityManager::goToFileTransfer},
    {AppId::Nutstore, StrId::STR_RICKY_NUTSTORE_TITLE, UIIcon::Nutstore, &ActivityManager::goToWebDav},
    {AppId::OpdsBrowser, StrId::STR_OPDS_BROWSER, UIIcon::Opds, &ActivityManager::goToBrowser},
#ifdef ENABLE_CHINESE_VERSION
    {AppId::WeRead, StrId::STR_WEREAD_TITLE, UIIcon::WeRead, &ActivityManager::goToWeRead},
#endif
    {AppId::AirPage, StrId::STR_AIRPAGE_TITLE, UIIcon::AirPage, &ActivityManager::goToAirPage},
#ifdef RICKYOS_PRODUCT
    {AppId::ReadingStats, StrId::STR_READING_STATS, UIIcon::ReadingStats, &ActivityManager::goToRickyReadingStats},
#else
    {AppId::ReadingStats, StrId::STR_READING_STATS, UIIcon::ReadingStats, &ActivityManager::goToReadingStatsMenu},
#endif
    {AppId::Vocabulary, StrId::STR_VOCABULARY_TITLE, UIIcon::Vocabulary, &ActivityManager::goToVocabulary},
    {AppId::Quotes, StrId::STR_QUOTES_TITLE, UIIcon::Quotes, &ActivityManager::goToQuotes},
    {AppId::Weather, StrId::STR_WEATHER_TITLE, UIIcon::Weather, &ActivityManager::goToWeather},
    {AppId::Rss, StrId::STR_RSS_TITLE, UIIcon::Rss, &ActivityManager::goToRss},
    {AppId::Agenda, StrId::STR_AGENDA_TITLE, UIIcon::Agenda, &ActivityManager::goToAgenda},
#ifndef RICKYOS_PRODUCT
    {AppId::Sudoku, StrId::STR_SUDOKU_TITLE, UIIcon::Sudoku, &ActivityManager::goToSudoku},
#endif
#ifndef RICKYOS_PRODUCT
    {AppId::Sokoban, StrId::STR_SOKOBAN_TITLE, UIIcon::Sokoban, &ActivityManager::goToSokoban},
#ifdef ENABLE_CHINESE_VERSION
    {AppId::ChineseChess, StrId::STR_CHINESE_CHESS_TITLE, UIIcon::ChineseChess, &ActivityManager::goToChineseChess},
#endif
    {AppId::Minesweeper, StrId::STR_MINESWEEPER_TITLE, UIIcon::Minesweeper, &ActivityManager::goToMinesweeper},
    {AppId::Game2048, StrId::STR_2048_TITLE, UIIcon::Game2048, &ActivityManager::goToGame2048},
    {AppId::UglyAvatar, StrId::STR_UGLY_AVATAR, UIIcon::Avatar, &ActivityManager::goToUglyAvatar},
    {AppId::Buddy, StrId::STR_BUDDY_TITLE, UIIcon::Buddy, &ActivityManager::goToBuddy},
    {AppId::PixelSwitch, StrId::STR_PIXEL_SWITCH_TITLE, UIIcon::PixelSwitch, &ActivityManager::goToPixelSwitch},
#endif
    {AppId::Calculator, StrId::STR_CALCULATOR_TITLE, UIIcon::Calculator, &ActivityManager::goToCalculator},
#ifndef RICKYOS_PRODUCT
    {AppId::Woodfish, StrId::STR_WOODFISH_TITLE, UIIcon::Woodfish, &ActivityManager::goToWoodfish},
#endif
#ifdef RICKYOS_PRODUCT
    // The one entry for Standby & power off (not repeated in Settings).
    {AppId::Standby, StrId::STR_RICKY_POWER_PAGE_TITLE, UIIcon::Standby, &ActivityManager::goToStandbySettings},
#else
    {AppId::Standby, StrId::STR_STANDBY_TITLE, UIIcon::Standby, &ActivityManager::goToStandby},
#endif
};

constexpr int kAppCount = static_cast<int>(sizeof(kAppEntries) / sizeof(kAppEntries[0]));

constexpr int visibleAppCount(const uint32_t hiddenMask) {
  int count = 0;
  for (const auto& app : kAppEntries) {
    if ((hiddenMask & appBit(app.id)) == 0) {
      // cppcheck-suppress useStlAlgorithm
      ++count;
    }
  }
  return count;
}

constexpr int appIndexForVisibleIndex(const uint32_t hiddenMask, const int visibleIndex) {
  int visible = 0;
  for (int appIndex = 0; appIndex < kAppCount; ++appIndex) {
    if ((hiddenMask & appBit(kAppEntries[appIndex].id)) != 0) continue;
    if (visible++ == visibleIndex) return appIndex;
  }
  return -1;
}

constexpr uint32_t effectiveHiddenMask(const uint32_t hiddenMask, const bool hasOpdsServers) {
  uint32_t effective = hiddenMask;
  if (!hasOpdsServers) effective |= appBit(AppId::OpdsBrowser);
  return effective;
}

constexpr bool appIdsAreUnique() {
  for (int i = 0; i < kAppCount; ++i) {
    for (int j = i + 1; j < kAppCount; ++j) {
      if (kAppEntries[i].id == kAppEntries[j].id) return false;
    }
  }
  return true;
}

static_assert(kAppCount <= 32, "the app catalog must fit hiddenAppsMask");
static_assert(static_cast<uint8_t>(AppId::Count) <= 32, "hiddenAppsMask supports at most 32 stable app IDs");
static_assert(appIdsAreUnique(), "stable app IDs must not be reused");
static_assert(visibleAppCount(0) == kAppCount, "a zero mask must show every compiled app");
static_assert(visibleAppCount(UINT32_MAX) == 0, "a full mask must hide every compiled app");
// Gomoku was retired to reclaim Flash; its persisted bit stays reserved in AppId.
static_assert(visibleAppCount(appBit(AppId::Gomoku)) == kAppCount, "Gomoku must stay out of the compiled catalog");
static_assert(visibleAppCount(appBit(AppId::Calculator)) == kAppCount - 1, "the mask must hide a compiled app");
static_assert(visibleAppCount(effectiveHiddenMask(0, false)) == kAppCount - 1,
              "OPDS must be hidden when no server is configured");
static_assert(visibleAppCount(effectiveHiddenMask(0, true)) == kAppCount,
              "all apps must be available regardless of language");
static_assert(appIndexForVisibleIndex(appBit(kAppEntries[1].id), 1) == 2,
              "visible indices must skip a hidden middle app");

// The apps menu is recreated on every return (goToApps replaces the activity),
// and UiListActivity::onEnter() resets the cursor to 0. Remember the last tile
// the user actually opened so returning selects that app instead of always
// landing on index 0 ("File transfer").
int gLastOpenedVisibleApp = -1;

}  // namespace

#ifdef RICKYOS_PRODUCT
namespace {
// The product grid draws its own 56 px line icons (same system as the tab bar)
// at native size; the stock 32 px pixel icons are only doubled, which looks coarse.
const uint8_t* rickyAppIcon(const UIIcon icon) {
  switch (icon) {
    case UIIcon::Transfer:
      return ricky_app_transfer_56;
    case UIIcon::Nutstore:
      return ricky_app_nutstore_56;
    case UIIcon::Opds:
      return ricky_app_opds_56;
    case UIIcon::WeRead:
      return ricky_app_weread_56;
    case UIIcon::ReadingStats:
      return ricky_app_stats_56;
    case UIIcon::Bookmark:
      return ricky_app_bookmark_56;
    case UIIcon::Calculator:
      return ricky_app_calculator_56;
    case UIIcon::Standby:
      return ricky_app_standby_56;
    case UIIcon::Weather:
      return ricky_app_weather_56;
    case UIIcon::AirPage:
      return ricky_app_airpage_56;
    case UIIcon::Vocabulary:
      return ricky_app_vocabulary_56;
    case UIIcon::Quotes:
      return ricky_app_quotes_56;
    case UIIcon::Rss:
      return ricky_app_rss_56;
    case UIIcon::Agenda:
      return ricky_app_agenda_56;
    default:
      return nullptr;
  }
}

// Reference style: a clean 3x4 ruled grid (thin gray dividers), each cell a
// bare black line icon over its label — no rounded-square tile, no shadow/halo
// and no selection marker behind the last opened app.
void drawRickyAppIcon(const GfxRenderer& renderer, const uint8_t* icon, const int x, const int y,
                      const bool /*selected*/) {
  // Icons are pure black line art, with no backing shape in any state.
  if (RickyAaIcons::draw(renderer, icon, kRickyAppIconSize, kRickyAppIconSize, x, y, true)) return;
  constexpr int rowBytes = (kRickyAppIconSize + 7) / 8;
  for (int row = 0; row < kRickyAppIconSize; ++row) {
    for (int column = 0; column < kRickyAppIconSize; ++column) {
      if ((icon[row * rowBytes + column / 8] & (0x80U >> (column % 8))) == 0)
        renderer.drawPixel(x + column, y + row, true);
    }
  }
}

// Draw the ruled 3x4 grid: one thin outer frame plus internal dividers,
// matching the reference's plain table (no gaps, no rounded tiles). A 1px ink
// line is the cleanest divider on e-paper and reads like the reference's hairline.
void drawAppGridRules(fui::DrawTarget& target, const Rect& rect) {
  const auto faint = fui::Paint::dither(fui::Color::DarkGray);
  const auto line = [&](const Rect& r) {
    target.fill(fui::Rect{static_cast<int16_t>(r.x), static_cast<int16_t>(r.y),
                          static_cast<int16_t>(r.width), static_cast<int16_t>(r.height)},
                faint, 0);
  };
  const int right = rect.x + rect.width;
  const int bottom = rect.y + rect.height;
  line(Rect{rect.x, rect.y, rect.width, 1});
  line(Rect{rect.x, bottom - 1, rect.width, 1});
  line(Rect{rect.x, rect.y, 1, rect.height});
  line(Rect{right - 1, rect.y, 1, rect.height});
  for (int c = 1; c < InxGridGeometry::columns; ++c) {
    const int x = rect.x + c * rect.width / InxGridGeometry::columns;
    line(Rect{x, rect.y, 1, rect.height});
  }
  const int rowsDrawn = std::min(InxGridGeometry::rows, 4);
  for (int r = 1; r < rowsDrawn; ++r) {
    const int y = rect.y + r * rect.height / rowsDrawn;
    line(Rect{rect.x, y, rect.width, 1});
  }
}
}  // namespace
#endif

int AppsMenuActivity::getAppCount() { return kAppCount; }

StrId AppsMenuActivity::getAppTitleId(const int appIndex) {
  return appIndex >= 0 && appIndex < kAppCount ? kAppEntries[appIndex].titleId : StrId::STR_NONE_OPT;
}

bool AppsMenuActivity::isAppVisible(const int appIndex) {
  if (appIndex < 0 || appIndex >= kAppCount) return false;
  return (SETTINGS.hiddenAppsMask & appBit(kAppEntries[appIndex].id)) == 0;
}

bool AppsMenuActivity::setAppVisible(const int appIndex, const bool visible) {
  if (appIndex < 0 || appIndex >= kAppCount) return false;

  const uint32_t bit = appBit(kAppEntries[appIndex].id);
  const uint32_t updatedMask = visible ? SETTINGS.hiddenAppsMask & ~bit : SETTINGS.hiddenAppsMask | bit;
  if (updatedMask == SETTINGS.hiddenAppsMask) return false;

  SETTINGS.hiddenAppsMask = updatedMask;
  return true;
}

int AppsMenuActivity::getVisibleAppCount() {
  return visibleAppCount(effectiveHiddenMask(SETTINGS.hiddenAppsMask, OPDS_STORE.hasServers()));
}

void AppsMenuActivity::selectMainTabContentEdge(const MainTabContentEdge edge) {
  nav.selected = MainTabs::contentEdgeIndex(edge, getVisibleAppCount());
  nav.follow(getVisibleAppCount());
}

int AppsMenuActivity::getAppIndexForVisibleIndex(const int visibleIndex) {
  return appIndexForVisibleIndex(effectiveHiddenMask(SETTINGS.hiddenAppsMask, OPDS_STORE.hasServers()), visibleIndex);
}

void AppsMenuActivity::onEnter() {
  UiListActivity::onEnter();
  rebuildRowItems();
  // UiListActivity::onEnter() reset the cursor to 0; restore the app the user
  // last opened so the grid no longer highlights "File transfer" every time.
  const int count = getVisibleAppCount();
  if (count <= 0) return;
  if (gLastOpenedVisibleApp >= 0 && gLastOpenedVisibleApp < count) {
    nav.selected = gLastOpenedVisibleApp;
    nav.follow(count);  // pull the viewport to the restored page
  } else {
    gLastOpenedVisibleApp = nav.selected;
  }
}

void AppsMenuActivity::rebuildRowItems() {
  rowItems.clear();
  rowItems.reserve(static_cast<size_t>(getVisibleAppCount()));
  for (int visibleIndex = 0; visibleIndex < getVisibleAppCount(); ++visibleIndex) {
    const int appIndex = getAppIndexForVisibleIndex(visibleIndex);
    if (appIndex < 0) continue;
    fui::ListItem item;
    item.label = I18N.get(kAppEntries[appIndex].titleId);
    item.icon = listIconFor(kAppEntries[appIndex].icon, 32);
    item.actionValue = static_cast<int16_t>(visibleIndex);
    rowItems.push_back(item);
  }
}

bool AppsMenuActivity::usesIconLayout() const {
  return UITheme::getInstance().hasMainTabs() &&
         InxGridGeometry::layoutFrom(SETTINGS.inxAppsLayout) == InxItemLayout::Icons;
}

Rect AppsMenuActivity::appContentRect() const {
  const int spacing = UITheme::getInstance().getMetrics().verticalSpacing;
  Rect content = pageContentRect();
  content.y += spacing;
  // Only the top inset is kept: the grid's bottom edge now sits directly on
  // the navigation-bar rule instead of floating above it with a blank strip.
  content.height -= spacing;
  return content;
}

int AppsMenuActivity::iconIndexFromPoint(const int x, const int y) const {
  const Rect content = appContentRect();
  return InxGridGeometry::indexFromPoint(x - content.x, y - content.y, content.width, content.height,
                                         InxGridGeometry::pageStart(nav.selected, getVisibleAppCount()),
                                         getVisibleAppCount());
}

void AppsMenuActivity::openSelected() {
  const int appIndex = getAppIndexForVisibleIndex(nav.selected);
  if (appIndex >= 0) gLastOpenedVisibleApp = nav.selected;
  if (appIndex >= 0) (activityManager.*kAppEntries[appIndex].open)();
}

void AppsMenuActivity::activateIndex(const int index) {
  app.clearTapFlash();
  nav.selected = index;
  openSelected();
}

bool AppsMenuActivity::handleCustomInput() {
  if (!usesIconLayout()) return false;

  const int visibleCount = getVisibleAppCount();
  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTouchDown(x, y)) {
    const int touched = iconIndexFromPoint(x, y);
    if (touched >= 0 && touched != nav.selected) {
      nav.selected = touched;
      requestUpdate();
    }
    return true;
  }
  if (mappedInput.wasScreenTapped(x, y)) {
    const int touched = iconIndexFromPoint(x, y);
    if (touched >= 0) {
      nav.selected = touched;
      openSelected();
    }
    return true;
  }
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    nav.selected = ButtonNavigator::nextPageIndex(nav.selected, visibleCount, InxGridGeometry::itemsPerPage);
    requestUpdate();
    return true;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    nav.selected = ButtonNavigator::previousPageIndex(nav.selected, visibleCount, InxGridGeometry::itemsPerPage);
    requestUpdate();
    return true;
  }
  return false;
}

void AppsMenuActivity::drawIconGrid(const Rect& rect, const int visibleCount, const bool showSelection,
                                    fui::DrawTarget& target) const {
  const int start = InxGridGeometry::pageStart(nav.selected, visibleCount);
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
#ifdef RICKYOS_PRODUCT
  // Reference style: a plain ruled 3x4 table drawn behind the icons.
  drawAppGridRules(target, rect);
#endif

  for (int slot = 0; slot < InxGridGeometry::itemsPerPage && start + slot < visibleCount; ++slot) {
    const int visibleIndex = start + slot;
    const int appIndex = getAppIndexForVisibleIndex(visibleIndex);
    if (appIndex < 0) continue;
    const auto bounds = InxGridGeometry::cellBounds(slot, rect.width, rect.height);
    const Rect cell{rect.x + bounds.x, rect.y + bounds.y, bounds.width, bounds.height};
    const bool isSelected = showSelection && visibleIndex == nav.selected;
    const std::string label =
        renderer.truncatedText(UI_10_FONT_ID, I18N.get(kAppEntries[appIndex].titleId), std::max(1, cell.width - 8));
    const int labelX = cell.x + (cell.width - renderer.getTextWidth(UI_10_FONT_ID, label.c_str())) / 2;
#ifdef RICKYOS_PRODUCT
    if (const uint8_t* icon = rickyAppIcon(kAppEntries[appIndex].icon)) {
      // Center the icon + label as one block inside the cell so a short grid
      // never pushes the label past the row divider. Icons are plain black line
      // art with no selection halo behind them.
      constexpr int labelGap = 6;
      const int blockHeight = kRickyAppIconSize + labelGap + lineHeight;
      const int blockTop = cell.y + std::max(2, (cell.height - blockHeight) / 2);
      const int iconX = cell.x + (cell.width - kRickyAppIconSize) / 2;
      drawRickyAppIcon(renderer, icon, iconX, blockTop, isSelected);
      renderer.drawText(UI_10_FONT_ID, labelX, blockTop + kRickyAppIconSize + labelGap, label.c_str(), true);
      continue;
    }
#endif
    const int iconScale = cell.height >= InxAppIcons::size * 2 + lineHeight + 18 ? 2 : 1;
    const int iconSize = InxAppIcons::size * iconScale;
    if (isSelected) renderer.fillRect(cell.x, cell.y, cell.width, cell.height, true);

    const int iconX = cell.x + (cell.width - iconSize) / 2;
    const int iconY = cell.y + std::max(5, (cell.height - iconSize - lineHeight - 8) / 2);
    InxAppIcons::draw(renderer, kAppEntries[appIndex].icon, iconX, iconY, iconScale, isSelected);
    renderer.drawText(UI_10_FONT_ID, labelX, iconY + iconSize + 8, label.c_str(), !isSelected);
  }

  GUI.drawSideScrollBar(renderer, rect, visibleCount, start, InxGridGeometry::itemsPerPage);
}

void AppsMenuActivity::buildScreen(UiScreen& screen) {
  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  const Rect content = appContentRect();
  const int visibleCount = getVisibleAppCount();
  const bool showSelection = showMainTabContentSelection();

  if (visibleCount == 0) {
    UITheme::drawCenteredWrappedText(renderer, content, UI_12_FONT_ID, tr(STR_NO_APPS_ENABLED), 2);
  } else if (usesIconLayout()) {
    drawIconGrid(content, visibleCount, showSelection, screen.target());
  } else {
    screen.setContentMarginFromScreen(
        fui::Insets{static_cast<int16_t>(content.y), static_cast<int16_t>(sw - content.x - content.width),
                    static_cast<int16_t>(sh - content.y - content.height), static_cast<int16_t>(content.x)});
    fui::ListProps props;
    props.items = rowItems.data();
    props.count = static_cast<uint16_t>(rowItems.size());
    props.action = ACTION_ROW;
    props.inputMask = fui::InputTouch;
    syncListViewport(screen, props);
    screen.list(props);
  }
}

void AppsMenuActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  drawPageHeader(Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, tr(STR_APPS_TITLE));
}

void AppsMenuActivity::drawFooter() {
  const int visibleCount = getVisibleAppCount();
  const auto labels = mainTabButtonLabels(tr(STR_BACK), tr(STR_SELECT), visibleCount > 1);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void AppsMenuActivity::onBackButton() { activityManager.goHome(); }
