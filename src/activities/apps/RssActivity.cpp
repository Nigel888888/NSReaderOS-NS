#include "RssActivity.h"

#include <Arduino.h>
#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <variant>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "util/TaskWatchdog.h"

namespace fui = freeink::ui;

namespace {

constexpr int SIDE_PAD = 16;
constexpr int TAB_H = 56;
constexpr int TAB_TOP = 6;
constexpr int TAB_GAP = 8;
constexpr int BAR_H = 56;
constexpr int BAR_GAP = 12;
constexpr int BAR_TOP = 8;
constexpr int CARD_GAP = 10;
constexpr int CARD_RADIUS = 8;
constexpr unsigned long MESSAGE_MS = 3000;
// A single feed request is bounded well above the HTTP library's own 15 s
// per-read timeout; this is only a last-resort backstop (see weather app).
constexpr unsigned long NETWORK_DEADLINE_MS = 60000;

bool hit(const Rect& r, int x, int y) {
  return r.width > 0 && r.height > 0 && x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

Rect rowRectFor(int index, const Rect& bounds, int rowHeight, int rowGap) {
  return Rect{bounds.x, bounds.y + index * (rowHeight + rowGap), bounds.width, rowHeight};
}

}  // namespace

RssActivity::RssActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Rss", renderer, mappedInput) {}

RssActivity::~RssActivity() = default;

void RssActivity::onEnter() {
  Activity::onEnter();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  view = View::Updates;
  phase = Phase::Idle;
  selected = 0;
  updatesSelected = 0;
  feedsSelected = 0;
  favoritesSelected = 0;
  message.clear();
  messageUntil = 0;
  fetchFeedIndex = -1;
  bodyLine = 0;
  loadFeeds();
  loadArticles();
  requestUpdate();
}

void RssActivity::showMessage(const std::string& text) {
  message = text;
  messageUntil = millis() + MESSAGE_MS;
  requestUpdate();
}

void RssActivity::loadArticles() {
  RssStore::loadArticles(articles);
  std::sort(articles.begin(), articles.end(),
            [](const RssStore::Article& a, const RssStore::Article& b) { return a.timestamp > b.timestamp; });
}

void RssActivity::loadFeeds() {
  RssStore::loadFeeds(feeds);
  rebuildFeedRows();
}

void RssActivity::rebuildUpdateRows() {
  rows.clear();
  rows.reserve(articles.size());
  for (const auto& a : articles) {
    Row r;
    r.title = a.title;
    r.subtitle = a.feed;
    r.date = a.date;
    r.favorite = a.favorite;
    r.favoriteGuid = a.guid;
    rows.push_back(std::move(r));
  }
}

void RssActivity::rebuildFavoriteRows() {
  rows.clear();
  for (const auto& a : articles) {
    if (!a.favorite) continue;
    Row r;
    r.title = a.title;
    r.subtitle = a.feed;
    r.date = a.date;
    r.favorite = a.favorite;
    r.favoriteGuid = a.guid;
    rows.push_back(std::move(r));
  }
}

void RssActivity::rebuildFeedRows() {
  rows.clear();
  rows.reserve(feeds.size());
  for (const auto& f : feeds) {
    Row r;
    r.title = f.name.empty() ? RssStore::nameFromUrl(f.url) : f.name;
    r.subtitle = f.url;
    rows.push_back(std::move(r));
  }
}

bool RssActivity::networkShouldAbort(const unsigned long started) const {
  resetTaskWatchdogIfSubscribed();
  return millis() - started >= NETWORK_DEADLINE_MS;
}

void RssActivity::updateAll() {
  if (feeds.empty()) {
    showMessage(tr(STR_RSS_NO_FEEDS));
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    showMessage(tr(STR_RSS_NO_WIFI));
    // Take the user straight to the same Wi-Fi picker the weather app uses;
    // on success the update resumes automatically.
    startActivityForResultWith<WifiSelectionActivity>(
        [this](const ActivityResult& result) {
          const auto* wifiResult = std::get_if<WifiResult>(&result.data);
          if (!result.isCancelled && wifiResult && wifiResult->connected) beginFetch();
        },
        true);
    return;
  }
  beginFetch();
}

void RssActivity::beginFetch() {
  fetchFeedIndex = -1;
  fetchedNew = 0;
  failedFeeds = 0;
  queueNextFetch();
}

void RssActivity::queueNextFetch() {
  // Advance to the next feed. Each request gets its own loop turn so the
  // render task can repaint and the loop task can feed its watchdog.
  fetchFeedIndex++;
  if (fetchFeedIndex >= static_cast<int>(feeds.size())) {
    phase = Phase::Idle;
    if (failedFeeds > 0 && fetchedNew == 0) {
      showMessage(tr(STR_RSS_UPDATE_FAILED));
    } else {
      char msg[64];
      std::snprintf(msg, sizeof(msg), tr(STR_RSS_UPDATE_DONE), fetchedNew);
      showMessage(msg);
    }
    loadArticles();
    if (view == View::Updates) rebuildUpdateRows();
    else if (view == View::Favorites) rebuildFavoriteRows();
    if (selected >= static_cast<int>(rows.size())) selected = rows.empty() ? 0 : static_cast<int>(rows.size()) - 1;
    return;
  }
  phase = Phase::Fetching;
  requestUpdate();
}

// Switch to one of the three list tabs and restore that tab's cursor.
void RssActivity::selectTab(View target) {
  if (view == target) return;
  if (view == View::Updates) updatesSelected = selected;
  else if (view == View::Feeds) feedsSelected = selected;
  else if (view == View::Favorites) favoritesSelected = selected;
  view = target;
  if (target == View::Updates) { rebuildUpdateRows(); selected = updatesSelected; }
  else if (target == View::Feeds) { rebuildFeedRows(); selected = feedsSelected; }
  else if (target == View::Favorites) { rebuildFavoriteRows(); selected = favoritesSelected; }
  if (selected >= static_cast<int>(rows.size())) selected = rows.empty() ? 0 : static_cast<int>(rows.size()) - 1;
  requestUpdate();
}

void RssActivity::loop() {
  if (!message.empty() && static_cast<long>(millis() - messageUntil) >= 0) {
    message.clear();
    requestUpdate();
  }

  // Network phase: one feed per loop turn, no renderer access here.
  if (phase == Phase::Fetching) {
    resetTaskWatchdogIfSubscribed();
    const unsigned long started = millis();
    const auto keepAlive = [this, started]() { return networkShouldAbort(started); };
    const int added = RssStore::fetchFeed(feeds[fetchFeedIndex], fetchFeedIndex, articles, keepAlive);
    if (added < 0) ++failedFeeds;
    else if (added > 0) fetchedNew += added;
    resetTaskWatchdogIfSubscribed();
    queueNextFetch();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (view == View::Article) {
      view = View::Updates;
      rebuildUpdateRows();
      selected = updatesSelected;
      requestUpdate();
    } else {
      activityManager.goToApps();
    }
    return;
  }

  int tx = 0, ty = 0;
  const bool tapped = mappedInput.wasScreenTapped(tx, ty);
  if (phase == Phase::Idle && tapped) {
    if (view != View::Article) {
      // Segmented tabs at the top.
      if (hit(tabRects[0], tx, ty)) { selectTab(View::Updates); return; }
      if (hit(tabRects[1], tx, ty)) { selectTab(View::Feeds); return; }
      if (hit(tabRects[2], tx, ty)) { selectTab(View::Favorites); return; }
      // Bottom action bar.
      if (hit(updateButton, tx, ty)) { updateAll(); return; }
      if (hit(addButton, tx, ty)) { addFeed(); return; }
      if (hit(manageButton, tx, ty)) { selectTab(View::Feeds); return; }
    }

    if (view == View::Updates || view == View::Favorites) {
      if (hit(listBounds, tx, ty) && rowHeight + rowGap > 0) {
        // The list draws from listFirstIndex(), so taps must use the same
        // viewport offset or a scrolled list opens the wrong article.
        int idx = listFirstIndex() + (ty - listBounds.y) / (rowHeight + rowGap);
        if (idx >= 0 && idx < static_cast<int>(rows.size())) {
          // Updates/Favorites rows map 1:1 to the sorted articles array only
          // for Updates; Favorites is a filtered subset, so resolve by guid.
          if (view == View::Updates) {
            openArticle(idx);
          } else {
            const std::string guid = rows[idx].title.empty() ? std::string() : rows[idx].favoriteGuid;
            int articleIndex = -1;
            for (size_t i = 0; i < articles.size(); ++i) {
              if (articles[i].favorite && articles[i].guid == guid) { articleIndex = static_cast<int>(i); break; }
            }
            if (articleIndex >= 0) openArticle(articleIndex);
          }
        }
        return;
      }
    } else if (view == View::Feeds) {
      if (hit(listBounds, tx, ty) && rowHeight + rowGap > 0) {
        int idx = listFirstIndex() + (ty - listBounds.y) / (rowHeight + rowGap);
        if (idx >= 0 && idx < static_cast<int>(feeds.size())) {
          feedsSelected = idx;
          selected = idx;
          removeSelectedFeed();
        }
        return;
      }
    } else if (view == View::Article) {
      if (hit(starButton, tx, ty)) { toggleCurrentFavorite(); return; }
      if (hit(backButton, tx, ty)) {
        view = View::Updates;
        rebuildUpdateRows();
        selected = updatesSelected;
        requestUpdate();
        return;
      }
      // Tapping lower half advances a page; upper half goes back a page.
      const int mid = listBounds.y + listBounds.height / 2;
      scrollBody(ty >= mid ? 1 : -1);
      return;
    }
  }

  // Swipe-scroll the card list. RSS used to handle only Up/Down *buttons*,
  // so after a refresh added new articles the longer list could not be
  // scrolled by touch at all. One swipe moves about a screenful and keeps
  // the per-tab cursor in sync.
  if (phase == Phase::Idle && view != View::Article && rowHeight + rowGap > 0) {
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
      const int perScreen = std::max(1, listBounds.height / (rowHeight + rowGap) - 1);
      int next = selected + (swipe == MappedInputManager::SwipeDir::Up ? perScreen : -perScreen);
      next = std::clamp(next, 0, std::max(0, static_cast<int>(rows.size()) - 1));
      if (next != selected) {
        selected = next;
        if (view == View::Updates) updatesSelected = selected;
        else if (view == View::Feeds) feedsSelected = selected;
        else if (view == View::Favorites) favoritesSelected = selected;
        requestUpdate();
      }
      return;
    }
  }

  // Button navigation of the current list.
  if (phase == Phase::Idle && view != View::Article) {
    int next = selected;
    if (mappedInput.wasReleased(MappedInputManager::Button::ScreenDown)) next = selected + 1;
    else if (mappedInput.wasReleased(MappedInputManager::Button::ScreenUp)) next = selected - 1;
    else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (view == View::Feeds) { if (selected >= 0 && selected < static_cast<int>(feeds.size())) { feedsSelected = selected; removeSelectedFeed(); } }
      else if (view == View::Updates) { if (selected >= 0 && selected < static_cast<int>(articles.size())) openArticle(selected); }
      else if (view == View::Favorites) { if (selected >= 0 && selected < static_cast<int>(rows.size())) { const std::string guid = rows[selected].favoriteGuid; for (size_t i = 0; i < articles.size(); ++i) { if (articles[i].favorite && articles[i].guid == guid) { openArticle(static_cast<int>(i)); break; } } } }
      return;
    }
    const int count = static_cast<int>(rows.size());
    if (next < 0) next = 0;
    if (next >= count) next = count - 1;
    if (next != selected && count > 0) {
      selected = next;
      if (view == View::Updates) updatesSelected = selected;
      else if (view == View::Feeds) feedsSelected = selected;
      else if (view == View::Favorites) favoritesSelected = selected;
      requestUpdate();
    }
  }
}

void RssActivity::openArticle(int index) {
  if (index < 0 || index >= static_cast<int>(articles.size())) return;
  openGuid = articles[index].guid;
  if (!RssStore::loadBody(articles[index], bodyText)) {
    bodyText = tr(STR_RSS_NO_BODY);
  }
  view = View::Article;
  bodyLine = 0;
  wrapBodyLines();
  requestUpdate();
}

void RssActivity::toggleCurrentFavorite() {
  if (openGuid.empty()) return;
  if (RssStore::toggleFavorite(articles, openGuid)) {
    if (view == View::Favorites) rebuildFavoriteRows();
    else if (view == View::Updates) rebuildUpdateRows();
    showMessage(tr(STR_RSS_FAV_TOGGLED));
  }
}

void RssActivity::wrapBodyLines() {
  bodyLines.clear();
  // Simple greedy wrap by measuring with the renderer, splitting on newlines
  // first. Bound the number of lines to keep memory modest.
  const int font = NOTOSANS_18_FONT_ID;
  const int maxWidth = renderer.getScreenWidth() - 2 * SIDE_PAD;
  constexpr size_t MAX_LINES = 600;
  size_t pos = 0;
  while (pos < bodyText.size() && bodyLines.size() < MAX_LINES) {
    const size_t nl = bodyText.find('\n', pos);
    const std::string segment = bodyText.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    std::string line;
    size_t i = 0;
    while (i < segment.size() && bodyLines.size() < MAX_LINES) {
      // Advance by UTF-8 codepoints until the line no longer fits.
      size_t next = i;
      while (next < segment.size()) {
        size_t step = 1;
        const unsigned char c = static_cast<unsigned char>(segment[next]);
        if ((c & 0xE0) == 0xC0) step = 2;
        else if ((c & 0xF0) == 0xE0) step = 3;
        else if ((c & 0xF8) == 0xF0) step = 4;
        if (next + step > segment.size()) step = 1;
        const std::string candidate = line + segment.substr(next, step);
        if (!line.empty() && renderer.getTextWidth(font, candidate.c_str()) > maxWidth) break;
        line = candidate;
        next += step;
      }
      if (next == i) next = i + 1;  // never stall
      bodyLines.push_back(line);
      line.clear();
      i = next;
    }
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
  if (bodyLines.empty()) bodyLines.push_back("");
  if (bodyLine < 0) bodyLine = 0;
  if (bodyLine >= static_cast<int>(bodyLines.size())) bodyLine = static_cast<int>(bodyLines.size()) - 1;
}

void RssActivity::scrollBody(int lines) {
  const int maxLine = static_cast<int>(bodyLines.size()) - 1;
  int next = bodyLine + lines;
  if (next < 0) next = 0;
  if (next > maxLine) next = maxLine;
  if (next != bodyLine) {
    bodyLine = next;
    requestUpdate();
  }
}

void RssActivity::addFeed() {
  startActivityForResultWith<KeyboardEntryActivity>(
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        const auto* entry = std::get_if<KeyboardResult>(&result.data);
        if (!entry || entry->text.empty()) return;
        if (RssStore::addFeed(feeds, entry->text)) {
          loadFeeds();
          showMessage(tr(STR_RSS_ADDED));
        } else {
          showMessage(tr(STR_RSS_ADD_FAILED));
        }
      },
      tr(STR_RSS_FEED_PROMPT), std::string("https://"), 256);
}

void RssActivity::removeSelectedFeed() {
  if (feedsSelected < 0 || feedsSelected >= static_cast<int>(feeds.size())) return;
  RssStore::dropArticlesForFeed(articles, feedsSelected);
  RssStore::saveArticles(articles);
  RssStore::removeFeed(feeds, static_cast<size_t>(feedsSelected));
  loadFeeds();
  loadArticles();
  if (view == View::Updates) rebuildUpdateRows();
  else if (view == View::Favorites) rebuildFavoriteRows();
  if (feedsSelected >= static_cast<int>(feeds.size())) feedsSelected = static_cast<int>(feeds.size()) - 1;
  selected = feedsSelected;
  showMessage(tr(STR_RSS_REMOVED));
}

// Header plus the three-way segmented tab bar. When `showStar` is set (the
// reading view) a small star button occupies the right of the title band.
void RssActivity::drawHeaderAndTabs(const char* title, bool showStar) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{SIDE_PAD, metrics.topPadding, renderer.getScreenWidth() - 2 * SIDE_PAD,
                                metrics.headerHeight},
                 title);
  if (showStar) {
    starButton = Rect{renderer.getScreenWidth() - SIDE_PAD - 120, metrics.topPadding + 4, 120,
                      metrics.headerHeight - 8};
    const bool starred = [this]() {
      for (const auto& a : articles) if (a.guid == openGuid) return a.favorite;
      return false;
    }();
    GUI.drawActionButton(renderer, starButton,
                         starred ? tr(STR_RSS_STARRED) : tr(STR_RSS_STAR), starred);
  }
}

// Draws the three segmented tabs (Updates / Feeds / Favorites) as one
// light-gray rounded bar with a dark capsule over the active segment, exactly
// like the reference screen. Hit rects stay the full thirds of the bar.
void RssActivity::drawTabs() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int y = metrics.topPadding + metrics.headerHeight + TAB_TOP;
  const int width = renderer.getScreenWidth() - 2 * SIDE_PAD;
  const int segW = (width - TAB_GAP * 2) / 3;
  const char* labels[3] = {tr(STR_RSS_UPDATE), tr(STR_RSS_FEEDS), tr(STR_RSS_FAVORITES)};
  const View views[3] = {View::Updates, View::Feeds, View::Favorites};
  renderer.fillRoundedRect(SIDE_PAD, y, width, TAB_H, TAB_H / 2, Color::LightGray);
  for (int i = 0; i < 3; ++i) {
    tabRects[i] = Rect{SIDE_PAD + i * (segW + TAB_GAP), y, segW, TAB_H};
    const bool active = view == views[i];
    if (active) {
      renderer.fillRoundedRect(tabRects[i].x, tabRects[i].y, tabRects[i].width, tabRects[i].height, TAB_H / 2,
                               Color::DarkGray);
    }
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, labels[i], EpdFontFamily::BOLD);
    const int textX = tabRects[i].x + (segW - textWidth) / 2;
    const int textY = tabRects[i].y + (TAB_H - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawText(UI_10_FONT_ID, textX, textY, labels[i], !active, EpdFontFamily::BOLD);
  }
}

// Bottom action bar: Update / Add feed / Manage, three equal buttons.
void RssActivity::drawBottomBar(bool onFeeds) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int barY = renderer.getScreenHeight() - metrics.buttonHintsHeight - BAR_H;
  const int width = renderer.getScreenWidth() - 2 * SIDE_PAD;
  const int btnW = (width - BAR_GAP * 2) / 3;
  updateButton = Rect{SIDE_PAD, barY, btnW, BAR_H};
  addButton = Rect{SIDE_PAD + btnW + BAR_GAP, barY, btnW, BAR_H};
  manageButton = Rect{SIDE_PAD + 2 * (btnW + BAR_GAP), barY, btnW, BAR_H};
  GUI.drawActionButton(renderer, updateButton, tr(STR_RSS_UPDATE), false);
  GUI.drawActionButton(renderer, addButton, tr(STR_RSS_ADD_FEED), false);
  GUI.drawActionButton(renderer, manageButton, tr(STR_RSS_MANAGE), onFeeds);
}

// Top y of the card list, sitting below the tabs and above the bottom bar.
int RssActivity::listTop() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return metrics.topPadding + metrics.headerHeight + TAB_TOP + TAB_H + metrics.verticalSpacing;
}

int RssActivity::listFirstIndex() const {
  if (rows.empty()) return 0;
  return std::max(0, std::min(selected - 2, static_cast<int>(rows.size()) - 1));
}

// Draws one card-style row: white rounded card, bold title, subtle subtitle
// and a light-gray date pill at the bottom right. The selected card gets a
// pale gray wash and a dark vertical bar along its left edge (the pressed
// shadow from the reference screen); text stays dark in both states.
void RssActivity::drawCardRow(const Rect& r, const Row& row, bool selected) {
  if (selected) {
    renderer.fillRoundedRect(r.x, r.y, r.width, r.height, CARD_RADIUS, Color::LightGray);
    renderer.fillRect(r.x, r.y + 6, 4, r.height - 12);
  } else {
    renderer.fillRoundedRect(r.x, r.y, r.width, r.height, CARD_RADIUS, Color::White);
  }
  renderer.drawRoundedRect(r.x, r.y, r.width, r.height, 1, CARD_RADIUS, true);
  const int titleLine = renderer.getLineHeight(NOTOSANS_18_FONT_ID);
  const int subLine = renderer.getLineHeight(NOTOSANS_16_FONT_ID);
  const int innerX = r.x + 14;
  const int dateW = row.date.empty() ? 0 : renderer.getTextWidth(UI_10_FONT_ID, row.date.c_str()) + 16;
  const int maxTitleW = r.width - 24 - dateW - 8;
  // Clip/measure the title to the space left of the date pill.
  std::string title = row.title;
  while (!title.empty() && renderer.getTextWidth(NOTOSANS_18_FONT_ID, title.c_str(), EpdFontFamily::BOLD) > maxTitleW) {
    size_t n = title.size() - 1;
    while (n > 0 && (static_cast<unsigned char>(title[n]) & 0xC0) == 0x80) --n;
    title.erase(n);
  }
  const int textTop = r.y + (r.height - titleLine - subLine) / 2;
  renderer.drawText(NOTOSANS_18_FONT_ID, innerX, textTop, title.c_str(), true, EpdFontFamily::BOLD);
  renderer.drawText(NOTOSANS_16_FONT_ID, innerX, textTop + titleLine, row.subtitle.c_str(), true);
  if (!row.date.empty() && dateW > 0) {
    // Date capsule: pale gray on a white card, white on the selected wash --
    // always clearly separated from the card surface, dark text either way.
    const Rect pill{r.x + r.width - dateW - 10, textTop + 2, dateW, subLine + 4};
    renderer.fillRoundedRect(pill.x, pill.y, pill.width, pill.height, pill.height / 2,
                             selected ? Color::White : Color::LightGray);
    renderer.drawText(UI_10_FONT_ID, pill.x + 8, pill.y + 2, row.date.c_str(), true);
  }
}

void RssActivity::drawArticleList() {
  drawHeaderAndTabs(tr(STR_RSS_TITLE), false);
  drawTabs();
  drawBottomBar(false);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int top = listTop();
  const int barY = renderer.getScreenHeight() - metrics.buttonHintsHeight - BAR_H;
  listBounds = Rect{SIDE_PAD, top, renderer.getScreenWidth() - 2 * SIDE_PAD, barY - CARD_GAP - top};
  rowHeight = renderer.getLineHeight(NOTOSANS_18_FONT_ID) + renderer.getLineHeight(NOTOSANS_16_FONT_ID) + 28;
  rowGap = CARD_GAP;
  if (rows.empty()) {
    renderer.drawText(NOTOSANS_18_FONT_ID, SIDE_PAD, listBounds.y + 20,
                      view == View::Favorites ? tr(STR_RSS_NO_FAVORITES) : tr(STR_RSS_NO_ARTICLES));
    return;
  }
  const int first = listFirstIndex();
  const int visible = std::max(1, listBounds.height / (rowHeight + rowGap));
  const int last = std::min(static_cast<int>(rows.size()), first + visible + 1);
  for (int i = first; i < last; ++i) {
    drawCardRow(rowRectFor(i - first, listBounds, rowHeight, rowGap), rows[i], i == selected);
  }
}

void RssActivity::drawFeedList() {
  drawHeaderAndTabs(tr(STR_RSS_TITLE), false);
  drawTabs();
  drawBottomBar(true);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int top = listTop();
  const int barY = renderer.getScreenHeight() - metrics.buttonHintsHeight - BAR_H;
  // Reserve a band of its own for the "tap to remove" hint: drawing it at
  // barY - 2 used to sit on top of the bottom action buttons / tab labels.
  const int hintLine = renderer.getLineHeight(NOTOSANS_16_FONT_ID);
  const int hintBand = hintLine + 6;
  listBounds = Rect{SIDE_PAD, top, renderer.getScreenWidth() - 2 * SIDE_PAD, barY - CARD_GAP - hintBand - top};
  rowHeight = renderer.getLineHeight(NOTOSANS_18_FONT_ID) + renderer.getLineHeight(NOTOSANS_16_FONT_ID) + 28;
  rowGap = CARD_GAP;
  if (rows.empty()) {
    renderer.drawText(NOTOSANS_18_FONT_ID, SIDE_PAD, listBounds.y + 20, tr(STR_RSS_NO_FEEDS));
  } else {
    const int first = listFirstIndex();
    const int visible = std::max(1, listBounds.height / (rowHeight + rowGap));
    const int last = std::min(static_cast<int>(rows.size()), first + visible + 1);
    for (int i = first; i < last; ++i) {
      Row unnamed = rows[i];
      drawCardRow(rowRectFor(i - first, listBounds, rowHeight, rowGap), unnamed, i == selected);
    }
  }
  renderer.drawText(NOTOSANS_16_FONT_ID, SIDE_PAD, barY - hintBand + 2, tr(STR_RSS_TAP_REMOVE));
}

void RssActivity::drawArticleBody() {
  drawHeaderAndTabs(tr(STR_RSS_READING), true);
  const auto& metrics = UITheme::getInstance().getMetrics();
  backButton = Rect{SIDE_PAD, metrics.topPadding + 4, 120, metrics.headerHeight - 8};
  GUI.drawActionButton(renderer, backButton, tr(STR_RSS_BACK), false);
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  listBounds = Rect{SIDE_PAD, top, renderer.getScreenWidth() - 2 * SIDE_PAD,
                    renderer.getScreenHeight() - top - metrics.buttonHintsHeight - metrics.verticalSpacing};
  const int lineHeight = renderer.getLineHeight(NOTOSANS_18_FONT_ID);
  const int linesOnScreen = std::max(1, listBounds.height / lineHeight);
  for (int i = 0; i < linesOnScreen && bodyLine + i < static_cast<int>(bodyLines.size()); ++i) {
    renderer.drawText(NOTOSANS_18_FONT_ID, listBounds.x, listBounds.y + i * lineHeight,
                      bodyLines[bodyLine + i].c_str());
  }
}

void RssActivity::drawMessage() {
  if (message.empty() || static_cast<long>(millis() - messageUntil) >= 0) return;
  renderer.drawText(NOTOSANS_16_FONT_ID, SIDE_PAD, renderer.getScreenHeight() - 40, message.c_str());
}

void RssActivity::render(RenderLock&&) {
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  renderer.clearScreen();

  if (phase != Phase::Idle) {
    GUI.drawHeader(renderer, Rect{SIDE_PAD, UITheme::getInstance().getMetrics().topPadding,
                                  renderer.getScreenWidth() - 2 * SIDE_PAD,
                                  UITheme::getInstance().getMetrics().headerHeight},
                   tr(STR_RSS_TITLE));
    GUI.drawPopup(renderer, tr(STR_RSS_UPDATING));
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  if (view == View::Updates || view == View::Favorites) drawArticleList();
  else if (view == View::Feeds) drawFeedList();
  else drawArticleBody();
  drawMessage();

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
