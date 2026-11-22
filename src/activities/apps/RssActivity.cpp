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

constexpr int SIDE_PAD = 20;
constexpr int HEADER_H = 56;
constexpr int BUTTON_H = 56;
constexpr int BUTTON_GAP = 12;
constexpr int BUTTON_TOP = 8;
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
  view = View::Articles;
  phase = Phase::Idle;
  selected = 0;
  articlesSelected = 0;
  feedsSelected = 0;
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
  rebuildArticleRows();
}

void RssActivity::loadFeeds() {
  RssStore::loadFeeds(feeds);
  rebuildFeedRows();
}

void RssActivity::rebuildArticleRows() {
  rows.clear();
  rows.reserve(articles.size());
  for (const auto& a : articles) {
    Row r;
    r.title = a.title;
    r.subtitle = a.date.empty() ? a.feed : (a.date + "  |  " + a.feed);
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
    return;
  }
  phase = Phase::Fetching;
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
      view = View::Articles;
      selected = articlesSelected;
      rebuildArticleRows();
      requestUpdate();
    } else if (view == View::Feeds) {
      loadArticles();
      view = View::Articles;
      selected = articlesSelected;
      requestUpdate();
    } else {
      activityManager.goToApps();
    }
    return;
  }

  int tx = 0, ty = 0;
  const bool tapped = mappedInput.wasScreenTapped(tx, ty);
  if (phase == Phase::Idle && tapped) {
    if (view == View::Articles) {
      if (hit(updateButton, tx, ty)) { updateAll(); return; }
      if (hit(feedsButton, tx, ty)) {
        view = View::Feeds;
        selected = feedsSelected;
        rebuildFeedRows();
        requestUpdate();
        return;
      }
      if (hit(rowRectFor(selected, listBounds, rowHeight, rowGap), tx, ty) || hit(listBounds, tx, ty)) {
        int idx = (ty - listBounds.y) / (rowHeight + rowGap);
        if (idx >= 0 && idx < static_cast<int>(articles.size())) openArticle(idx);
        return;
      }
    } else if (view == View::Feeds) {
      if (hit(backButton, tx, ty)) {
        loadArticles();
        view = View::Articles;
        selected = articlesSelected;
        requestUpdate();
        return;
      }
      if (hit(addButton, tx, ty)) { addFeed(); return; }
      if (hit(listBounds, tx, ty)) {
        int idx = (ty - listBounds.y) / (rowHeight + rowGap);
        if (idx >= 0 && idx < static_cast<int>(feeds.size())) {
          feedsSelected = idx;
          selected = idx;
          removeSelectedFeed();
        }
        return;
      }
    } else if (view == View::Article) {
      if (hit(backButton, tx, ty)) {
        view = View::Articles;
        selected = articlesSelected;
        rebuildArticleRows();
        requestUpdate();
        return;
      }
      // Tapping lower half advances a page; upper half goes back a page.
      const int mid = listBounds.y + listBounds.height / 2;
      scrollBody(ty >= mid ? 1 : -1);
      return;
    }
  }

  // Button / touch navigation of the current list.
  if (phase == Phase::Idle && (view == View::Articles || view == View::Feeds)) {
    int next = selected;
    if (mappedInput.wasReleased(MappedInputManager::Button::ScreenDown)) next = selected + 1;
    else if (mappedInput.wasReleased(MappedInputManager::Button::ScreenUp)) next = selected - 1;
    else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (view == View::Articles && selected < static_cast<int>(articles.size())) openArticle(selected);
      return;
    }
    const int count = static_cast<int>(rows.size());
    if (next < 0) next = 0;
    if (next >= count) next = count - 1;
    if (next != selected && count > 0) {
      selected = next;
      if (view == View::Articles) articlesSelected = selected; else feedsSelected = selected;
      requestUpdate();
    }
  }
}

void RssActivity::openArticle(int index) {
  if (index < 0 || index >= static_cast<int>(articles.size())) return;
  articlesSelected = index;
  if (!RssStore::loadBody(articles[index], bodyText)) {
    bodyText = tr(STR_RSS_NO_BODY);
  }
  view = View::Article;
  bodyLine = 0;
  wrapBodyLines();
  requestUpdate();
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
      tr(STR_RSS_FEED_PROMPT), std::string("https://"), 48);
}

void RssActivity::removeSelectedFeed() {
  if (feedsSelected < 0 || feedsSelected >= static_cast<int>(feeds.size())) return;
  RssStore::dropArticlesForFeed(articles, feedsSelected);
  RssStore::saveArticles(articles);
  RssStore::removeFeed(feeds, static_cast<size_t>(feedsSelected));
  loadFeeds();
  loadArticles();
  if (feedsSelected >= static_cast<int>(feeds.size())) feedsSelected = static_cast<int>(feeds.size()) - 1;
  selected = feedsSelected;
  showMessage(tr(STR_RSS_REMOVED));
}

void RssActivity::drawHeaderAndButtons(const char* title) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{SIDE_PAD, metrics.topPadding, renderer.getScreenWidth() - 2 * SIDE_PAD,
                                metrics.headerHeight},
                 title);
  const int y = metrics.topPadding + metrics.headerHeight + BUTTON_TOP;
  const int width = renderer.getScreenWidth() - 2 * SIDE_PAD;
  const int half = (width - BUTTON_GAP) / 2;
  updateButton = Rect{SIDE_PAD, y, half, BUTTON_H};
  feedsButton = Rect{SIDE_PAD + half + BUTTON_GAP, y, half, BUTTON_H};
  backButton = Rect{SIDE_PAD, y, 120, BUTTON_H};
  addButton = Rect{SIDE_PAD + width - 120, y, 120, BUTTON_H};
  return;
}

void RssActivity::drawArticleList() {
  drawHeaderAndButtons(tr(STR_RSS_TITLE));
  GUI.drawActionButton(renderer, updateButton, tr(STR_RSS_UPDATE), false);
  GUI.drawActionButton(renderer, feedsButton, tr(STR_RSS_FEEDS), false);

  const auto& metrics = UITheme::getInstance().getMetrics();
  int top = metrics.topPadding + metrics.headerHeight + BUTTON_TOP + BUTTON_H + metrics.verticalSpacing;
  listBounds = Rect{SIDE_PAD, top, renderer.getScreenWidth() - 2 * SIDE_PAD,
                    renderer.getScreenHeight() - top - metrics.buttonHintsHeight - metrics.verticalSpacing};
  rowHeight = 72;
  rowGap = 8;
  if (rows.empty()) {
    renderer.drawText(NOTOSANS_18_FONT_ID, SIDE_PAD, listBounds.y + 20, tr(STR_RSS_NO_ARTICLES));
    return;
  }
  const int first = std::max(0, std::min(selected - 2, static_cast<int>(rows.size()) - 1));
  const int visible = std::max(1, listBounds.height / (rowHeight + rowGap));
  const int last = std::min(static_cast<int>(rows.size()), first + visible + 1);
  for (int i = first; i < last; ++i) {
    const Rect r = rowRectFor(i - first, listBounds, rowHeight, rowGap);
    const bool sel = (i == selected);
    bool black = true;
    if (sel) black = GUI.drawSelectionBackground(renderer, r);
    renderer.drawText(NOTOSANS_18_FONT_ID, r.x + 8, r.y + 4, rows[i].title.c_str(), black, EpdFontFamily::BOLD);
    renderer.drawText(NOTOSANS_16_FONT_ID, r.x + 8, r.y + 4 + renderer.getLineHeight(NOTOSANS_18_FONT_ID),
                      rows[i].subtitle.c_str(), black);
  }
}

void RssActivity::drawFeedList() {
  drawHeaderAndButtons(tr(STR_RSS_FEEDS));
  GUI.drawActionButton(renderer, backButton, tr(STR_RSS_BACK), false);
  GUI.drawActionButton(renderer, addButton, tr(STR_RSS_ADD_FEED), false);

  const auto& metrics = UITheme::getInstance().getMetrics();
  int top = metrics.topPadding + metrics.headerHeight + BUTTON_TOP + BUTTON_H + metrics.verticalSpacing;
  listBounds = Rect{SIDE_PAD, top, renderer.getScreenWidth() - 2 * SIDE_PAD,
                    renderer.getScreenHeight() - top - metrics.buttonHintsHeight - metrics.verticalSpacing};
  rowHeight = 72;
  rowGap = 8;
  if (rows.empty()) {
    renderer.drawText(NOTOSANS_18_FONT_ID, SIDE_PAD, listBounds.y + 20, tr(STR_RSS_NO_FEEDS));
    return;
  }
  const int first = std::max(0, std::min(selected - 2, static_cast<int>(rows.size()) - 1));
  const int visible = std::max(1, listBounds.height / (rowHeight + rowGap));
  const int last = std::min(static_cast<int>(rows.size()), first + visible + 1);
  for (int i = first; i < last; ++i) {
    const Rect r = rowRectFor(i - first, listBounds, rowHeight, rowGap);
    const bool sel = (i == selected);
    bool black = true;
    if (sel) black = GUI.drawSelectionBackground(renderer, r);
    renderer.drawText(NOTOSANS_18_FONT_ID, r.x + 8, r.y + 4, rows[i].title.c_str(), black, EpdFontFamily::BOLD);
    renderer.drawText(NOTOSANS_16_FONT_ID, r.x + 8, r.y + 4 + renderer.getLineHeight(NOTOSANS_18_FONT_ID),
                      rows[i].subtitle.c_str(), black);
  }
  renderer.drawText(NOTOSANS_16_FONT_ID, SIDE_PAD,
                    renderer.getScreenHeight() - metrics.buttonHintsHeight - 4, tr(STR_RSS_TAP_REMOVE));
}

void RssActivity::drawArticleBody() {
  drawHeaderAndButtons(tr(STR_RSS_READING));
  GUI.drawActionButton(renderer, backButton, tr(STR_RSS_BACK), false);

  const auto& metrics = UITheme::getInstance().getMetrics();
  int top = metrics.topPadding + metrics.headerHeight + BUTTON_TOP + BUTTON_H + metrics.verticalSpacing;
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

  if (view == View::Articles) drawArticleList();
  else if (view == View::Feeds) drawFeedList();
  else drawArticleBody();
  drawMessage();

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
