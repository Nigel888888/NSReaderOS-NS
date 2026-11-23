#include "RssActivity.h"

#include <Arduino.h>
#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <variant>

#include <CrossPointSettings.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/DictionaryDefinitionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "util/Dictionary.h"
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
constexpr const char* UI_STATE_PATH = "/.crosspoint/rss/ui-state.txt";
// Re-entering the app used to re-read and re-parse the whole article index
// from SD (the main reason the entry felt slow). The RSS data is already
// process-wide, so keep a cache for the lifetime of the firmware.
std::vector<RssStore::Feed> gFeedsCache;
std::vector<RssStore::Article> gArticlesCache;
bool gFeedsCached = false;
bool gArticlesCached = false;
// A single feed request is bounded well above the HTTP library's own 15 s
// per-read timeout; this is only a last-resort backstop (see weather app).
constexpr unsigned long NETWORK_DEADLINE_MS = 60000;

bool hit(const Rect& r, int x, int y) {
  return r.width > 0 && r.height > 0 && x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

Rect rowRectFor(int index, const Rect& bounds, int rowHeight, int rowGap) {
  return Rect{bounds.x, bounds.y + index * (rowHeight + rowGap), bounds.width, rowHeight};
}

// A dictionary index build streams the whole .idx file; yield to FreeRTOS so
// the render and watchdog tasks keep running (same contract as the reader).
void rssIndexBuildYield(void*) { vTaskDelay(1); }

// Word boundaries for RSS prose. Apostrophes and hyphens stay inside a token
// ("don't", "long-press") because Dictionary::cleanWord() knows what to do
// with them; quotes and sentence marks do not. CJK has no spaces, so a run of
// non-ASCII bytes is kept together as one token.
bool isWordSeparator(const unsigned char c) {
  if (c < 0x80) {
    return !std::isalnum(c) && c != '\'' && c != '-';
  }
  return false;
}

bool isSelectableBodyToken(const std::string& token) {
  for (size_t i = 0; i < token.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(token[i]);
    if (c < 0x80) {
      if (std::isalnum(c)) return true;
    } else {
      return true;  // CJK / other non-ASCII run: let the dictionary clean it
    }
  }
  return false;
}

// Byte length of a sentence-ending mark at `pos`, including CJK full stops.
// Mirrors DictWordSelect::sentenceEndLength so a word collected from an RSS
// article is reviewed in the same kind of context as one from a book.
size_t rssSentenceEndLength(const std::string& text, const size_t pos) {
  if (pos >= text.size()) return 0;
  const unsigned char c = static_cast<unsigned char>(text[pos]);
  if (c == '.' || c == '!' || c == '?' || c == ';') return 1;
  if (pos + 2 < text.size() && c == 0xE3 && static_cast<unsigned char>(text[pos + 1]) == 0x80 &&
      static_cast<unsigned char>(text[pos + 2]) == 0x82) {
    return 3;  // 。 U+3002
  }
  if (pos + 2 < text.size() && c == 0xEF && static_cast<unsigned char>(text[pos + 1]) == 0xBC) {
    const unsigned char c2 = static_cast<unsigned char>(text[pos + 2]);
    if (c2 == 0x81 || c2 == 0x9F || c2 == 0x89) return 3;  // ！ ？ ；
  }
  return 0;
}

}  // namespace

RssActivity::RssActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Rss", renderer, mappedInput) {}

RssActivity::~RssActivity() = default;

void RssActivity::rememberCursor() {
  // The article view owns no list cursor; keep the last list tab's values.
  if (view == View::Article) return;
  if (view == View::Feeds) {
    feedsSelected = selected;
    feedsScroll = scrollTop;
  } else if (view == View::Favorites) {
    favoritesSelected = selected;
    favoritesScroll = scrollTop;
  } else {
    updatesSelected = selected;
    updatesScroll = scrollTop;
  }
}

void RssActivity::saveState() {
  // Persist the live cursor into whichever tab is showing (the article view
  // keeps the Updates cursor), so a leave/enter cycle restores the same card.
  rememberCursor();
  if (!Storage.exists("/.crosspoint/rss")) Storage.mkdir("/.crosspoint/rss");
  const View persisted = view == View::Article ? View::Updates : view;
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "view=%d\nupdates=%d\nupdatesScroll=%d\nfeeds=%d\nfeedsScroll=%d\nfavorites=%d\nfavoritesScroll=%d\n",
                static_cast<int>(persisted), updatesSelected, updatesScroll, feedsSelected, feedsScroll,
                favoritesSelected, favoritesScroll);
  Storage.writeFile(UI_STATE_PATH, String(buf));
}

void RssActivity::loadState() {
  std::string text;
  if (!Storage.readFileToString("RSS", UI_STATE_PATH, 1024, text)) return;
  const auto readInt = [&text](const char* key, const int fallback) {
    const char* p = std::strstr(text.c_str(), key);
    return p ? std::atoi(p + std::strlen(key)) : fallback;
  };
  const int v = std::clamp(readInt("view=", 0), 0, static_cast<int>(View::Favorites));
  view = static_cast<View>(v);
  updatesSelected = std::max(0, readInt("updates=", 0));
  updatesScroll = std::max(0, readInt("updatesScroll=", 0));
  feedsSelected = std::max(0, readInt("feeds=", 0));
  feedsScroll = std::max(0, readInt("feedsScroll=", 0));
  favoritesSelected = std::max(0, readInt("favorites=", 0));
  favoritesScroll = std::max(0, readInt("favoritesScroll=", 0));
}

void RssActivity::onEnter() {
  Activity::onEnter();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  phase = Phase::Idle;
  message.clear();
  messageUntil = 0;
  fetchFeedIndex = -1;
  bodyLine = 0;
  armedIndex = -1;
  loadState();
  // The article index is large; re-reading it on every entry made the app feel
  // sluggish. Reuse the process-wide cache unless this is the first entry.
  if (gFeedsCached) {
    feeds = gFeedsCache;
    rebuildFeedRows();
  } else {
    loadFeeds();
  }
  if (gArticlesCached) {
    articles = gArticlesCache;
  } else {
    loadArticles();
  }
  // Rebuild exactly the view we restored so tab and rows can never disagree.
  if (view == View::Feeds) {
    selected = feedsSelected;
    scrollTop = feedsScroll;
  } else if (view == View::Favorites) {
    rebuildFavoriteRows();
    selected = favoritesSelected;
    scrollTop = favoritesScroll;
  } else {
    view = View::Updates;
    rebuildUpdateRows();
    selected = updatesSelected;
    scrollTop = updatesScroll;
  }
  // Geometry is not measured until the first render; the draw functions clamp
  // the viewport once rowHeight/listBounds are valid. Here only bound the
  // cursor so a shrunken list cannot leave it pointing past the end.
  if (selected >= static_cast<int>(rows.size())) selected = rows.empty() ? 0 : static_cast<int>(rows.size()) - 1;
  requestUpdate();
}

void RssActivity::onExit() {
  saveState();
  Activity::onExit();
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
  gArticlesCache = articles;
  gArticlesCached = true;
}

void RssActivity::loadFeeds() {
  RssStore::loadFeeds(feeds);
  gFeedsCache = feeds;
  gFeedsCached = true;
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
    clampScroll();
    ensureCursorVisible();
    return;
  }
  phase = Phase::Fetching;
  requestUpdate();
}

// Switch to one of the three list tabs and restore that tab's cursor.
void RssActivity::selectTab(View target) {
  if (view == target) return;
  rememberCursor();
  view = target;
  if (target == View::Updates) { rebuildUpdateRows(); selected = updatesSelected; scrollTop = updatesScroll; }
  else if (target == View::Feeds) { rebuildFeedRows(); selected = feedsSelected; scrollTop = feedsScroll; }
  else if (target == View::Favorites) { rebuildFavoriteRows(); selected = favoritesSelected; scrollTop = favoritesScroll; }
  clampScroll();
  ensureCursorVisible();
  armedIndex = -1;
  saveState();
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
      scrollTop = updatesScroll;
      clampScroll();
      ensureCursorVisible();
      requestUpdate();
    } else {
      activityManager.goToApps();
    }
    return;
  }

  // A stationary long-press on article text selects the word under the
  // finger and opens the same definition card the book reader uses. It
  // consumes the contact, so releasing cannot also turn the page.
  if (phase == Phase::Idle && view == View::Article && !dictionaryBusy) {
    int lpX = 0;
    int lpY = 0;
    if (mappedInput.wasScreenLongPress(lpX, lpY)) {
      lookupBodyWord(lpX, lpY);
      return;
    }
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
          // A tap only moves the cursor. The viewport (`scrollTop`) is left
          // untouched, so the list never drags itself to keep two cards on top.
          selected = idx;
          rememberCursor();
          // Same two-step rule as the vocabulary notebook: first touch only
          // selects the card, the second touch on it opens the article.
          if (armedIndex != idx) {
            armedIndex = idx;
            saveState();
            requestUpdate();
            return;
          }
          armedIndex = -1;
          saveState();
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
          selected = idx;  // tap moves cursor only; viewport stays put
          rememberCursor();
          if (armedIndex != idx) {
            armedIndex = idx;  // first tap selects; a second tap removes
            saveState();
            requestUpdate();
          } else {
            armedIndex = -1;
            removeSelectedFeed();
          }
        }
        return;
      }
    } else if (view == View::Article) {
      if (hit(starButton, tx, ty)) { toggleCurrentFavorite(); return; }
      if (hit(backButton, tx, ty)) {
        view = View::Updates;
        rebuildUpdateRows();
        selected = updatesSelected;
        scrollTop = updatesScroll;
        clampScroll();
        ensureCursorVisible();
        requestUpdate();
        return;
      }
      // Tapping lower half advances a whole screen of text; upper half goes
      // back one screen (with one line of overlap for continuity).
      const int mid = listBounds.y + listBounds.height / 2;
      const int step = std::max(1, bodyPageLines - 1);
      scrollBody(ty >= mid ? step : -step);
      return;
    }
  }

  // A swipe pages the viewport. The cursor follows to the first row of the
  // new page so the highlight stays on screen; the cursor is never the thing
  // that drives scrolling (a tap moves only the cursor).
  if (phase == Phase::Idle && view != View::Article && rowHeight + rowGap > 0) {
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
      const int perScreen = visibleRows();
      const int maxScroll = std::max(0, static_cast<int>(rows.size()) - perScreen);
      int next = scrollTop + (swipe == MappedInputManager::SwipeDir::Up ? perScreen : -perScreen);
      next = std::clamp(next, 0, maxScroll);
      if (next != scrollTop) {
        scrollTop = next;
        selected = scrollTop;  // keep the cursor visible at the top of the page
        armedIndex = -1;
        rememberCursor();
        saveState();
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
      ensureCursorVisible();  // drag the viewport only if the cursor left the band
      armedIndex = -1;
      rememberCursor();
      saveState();
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
    // Keep the process-wide cache coherent so the star state survives a
    // leave/enter cycle without re-reading the index from SD.
    gArticlesCache = articles;
    if (view == View::Favorites) rebuildFavoriteRows();
    else if (view == View::Updates) rebuildUpdateRows();
    showMessage(tr(STR_RSS_FAV_TOGGLED));
  }
}

void RssActivity::wrapBodyLines() {
  bodyLines.clear();
  bodyLineOffsets.clear();
  bodyWords.clear();
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
      // Remember where this drawn line starts in the source text: a word
      // selected on it can then be expanded to its full original sentence.
      bodyLineOffsets.push_back(pos + i);
      line.clear();
      i = next;
    }
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
  if (bodyLines.empty()) {
    bodyLines.push_back("");
    bodyLineOffsets.push_back(0);
  }
  rebuildBodyWords();
  if (bodyLine < 0) bodyLine = 0;
  if (bodyLine >= static_cast<int>(bodyLines.size())) bodyLine = static_cast<int>(bodyLines.size()) - 1;
}

// Measure the drawn x of every selectable token once, at wrap time. The
// reading view renders these same lines with the same font from listBounds.x,
// so a long-press can map a screen point straight back to the token.
void RssActivity::rebuildBodyWords() {
  bodyWords.clear();
  const int font = NOTOSANS_18_FONT_ID;
  constexpr size_t MAX_WORDS = 4000;
  for (size_t li = 0; li < bodyLines.size() && bodyWords.size() < MAX_WORDS; ++li) {
    const std::string& line = bodyLines[li];
    const size_t lineStart = li < bodyLineOffsets.size() ? bodyLineOffsets[li] : 0;
    size_t t = 0;
    while (t < line.size() && bodyWords.size() < MAX_WORDS) {
      while (t < line.size() && isWordSeparator(static_cast<unsigned char>(line[t]))) ++t;
      const size_t start = t;
      while (t < line.size() && !isWordSeparator(static_cast<unsigned char>(line[t]))) ++t;
      if (t <= start) continue;
      std::string token = line.substr(start, t - start);
      if (!isSelectableBodyToken(token)) continue;
      BodyWord word;
      word.x = renderer.getTextWidth(font, line.substr(0, start).c_str());
      word.width = renderer.getTextWidth(font, token.c_str());
      word.line = static_cast<int>(li);
      word.textOffset = lineStart + start;
      word.text = std::move(token);
      bodyWords.push_back(std::move(word));
    }
  }
}

int RssActivity::bodyWordAt(const int x, const int y) const {
  if (bodyLineHeight <= 0 || bodyWords.empty()) return -1;
  if (y < listBounds.y || y >= listBounds.y + listBounds.height) return -1;
  const int row = (y - listBounds.y) / bodyLineHeight + bodyLine;
  constexpr int SLOP = 4;  // finger error, same allowance as the book reader
  for (int i = 0; i < static_cast<int>(bodyWords.size()); ++i) {
    const BodyWord& word = bodyWords[i];
    if (word.line != row) continue;
    const int wordX = listBounds.x + word.x;
    if (x >= wordX - SLOP && x < wordX + word.width + SLOP) return i;
  }
  return -1;
}

// Full source sentence containing the selected token. bodyLineOffsets keep
// the drawn line tied to its place in bodyText, so wrapping never truncates
// the context the word is reviewed in.
std::string RssActivity::bodySentenceFor(const int wordIndex) const {
  if (wordIndex < 0 || wordIndex >= static_cast<int>(bodyWords.size())) return std::string();
  const BodyWord& word = bodyWords[wordIndex];
  if (bodyText.empty() || word.textOffset >= bodyText.size()) {
    return bodyLines[word.line];
  }
  const size_t wordStart = word.textOffset;
  const size_t wordEnd = wordStart + word.text.size();

  size_t start = 0;
  for (size_t i = wordStart; i > 0; --i) {
    const size_t n = rssSentenceEndLength(bodyText, i - 1);
    if (n > 0) {
      start = i - 1 + n;
      break;
    }
  }
  size_t end = bodyText.size();
  for (size_t i = wordEnd; i < bodyText.size(); ++i) {
    if ((static_cast<unsigned char>(bodyText[i]) & 0xC0) == 0x80) continue;
    const size_t n = rssSentenceEndLength(bodyText, i);
    if (n > 0) {
      end = i + n;
      break;
    }
  }
  if (end <= start || start >= bodyText.size()) return bodyLines[word.line];

  std::string sentence = bodyText.substr(start, end - start);
  std::string flat;
  flat.reserve(sentence.size());
  bool pendingSpace = false;
  for (const char ch : sentence) {
    if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
      pendingSpace = !flat.empty();
      continue;
    }
    if (pendingSpace) {
      flat.push_back(' ');
      pendingSpace = false;
    }
    flat.push_back(ch);
  }
  // VocabularyFile::MAX_SENTENCE_LENGTH is 400; stay just below it.
  constexpr size_t kMaxSentenceBytes = 380;
  if (flat.size() > kMaxSentenceBytes) {
    size_t cut = kMaxSentenceBytes;
    while (cut > 0 && (static_cast<unsigned char>(flat[cut]) & 0xC0) == 0x80) --cut;
    flat = flat.substr(0, cut);
  }
  return flat;
}

void RssActivity::lookupBodyWord(const int x, const int y) {
  const int idx = bodyWordAt(x, y);
  if (idx < 0) return;
  if (SETTINGS.dictionaryName[0] == '\0') {
    showMessage(tr(STR_DICT_NO_DICT_SET));
    return;
  }

  const std::string token = bodyWords[idx].text;
  const std::string context = bodySentenceFor(idx);
  dictionaryBusy = true;
  requestUpdateAndWait();

  Dictionary dict;
  bool ok = dict.open(SETTINGS.dictionaryName);
  bool needsIndex = ok && dict.needsIndex();
  Dictionary::IndexResult indexResult = Dictionary::IndexResult::Ok;
  if (ok && needsIndex) {
    ok = dict.buildIndex(&rssIndexBuildYield, nullptr, &indexResult);
  }
  std::string definition;
  std::string headword;
  Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
  const bool found = ok && dict.lookup(token.c_str(), definition, headword, &result);
  dictionaryBusy = false;

  if (found) {
    std::string book = tr(STR_RSS_TITLE);
    for (const auto& article : articles) {
      if (article.guid == openGuid) {
        if (!article.title.empty()) book = article.title;
        break;
      }
    }
    requestUpdate();
    // The definition card already carries "add to vocabulary" and "save as
    // quote"; reusing it keeps RSS word collection identical to book reading.
    startActivityForResultWith<DictionaryDefinitionActivity>(
        [this](const ActivityResult&) { requestUpdate(); }, std::move(headword), std::move(definition),
        dict.definitionsAreHtml(), std::move(book), std::move(context), /*showQuoteButton=*/true);
    return;
  }

  if (!ok) {
    switch (indexResult) {
      case Dictionary::IndexResult::LowMemory:
        showMessage(tr(STR_DICT_LOW_MEMORY));
        break;
      case Dictionary::IndexResult::ReadError:
        showMessage(tr(STR_DICT_READ_FAILED));
        break;
      case Dictionary::IndexResult::Ok:
      default:
        showMessage(tr(STR_DICT_ERROR));
        break;
    }
    return;
  }
  switch (result) {
    case Dictionary::LookupResult::LowMemory:
      showMessage(tr(STR_DICT_LOW_MEMORY));
      break;
    case Dictionary::LookupResult::Decompress:
      showMessage(tr(STR_DICT_DECOMPRESS_ERROR));
      break;
    case Dictionary::LookupResult::ReadError:
      showMessage(tr(STR_DICT_READ_FAILED));
      break;
    case Dictionary::LookupResult::NotFound:
    case Dictionary::LookupResult::Found:
    default:
      showMessage(tr(STR_DICT_NOT_FOUND));
      break;
  }
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
  // loadFeeds() rebuilt rows for the Feeds tab; re-clamp the viewport to it.
  clampScroll();
  ensureCursorVisible();
  rememberCursor();
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

int RssActivity::visibleRows() const {
  if (rowHeight + rowGap <= 0) return 1;
  return std::max(1, listBounds.height / (rowHeight + rowGap));
}

void RssActivity::clampScroll() {
  const int maxScroll = std::max(0, static_cast<int>(rows.size()) - visibleRows());
  scrollTop = std::clamp(scrollTop, 0, maxScroll);
}

void RssActivity::ensureCursorVisible() {
  clampScroll();
  if (rows.empty()) {
    selected = 0;
    scrollTop = 0;
    return;
  }
  if (selected < scrollTop) {
    scrollTop = selected;
  } else if (selected >= scrollTop + visibleRows()) {
    scrollTop = selected - visibleRows() + 1;
  }
  clampScroll();
}

// Viewport top row. Independent of `selected`: tapping a card moves only the
// cursor, while swiping / arrow paging moves `scrollTop`. Previously this was
// `selected - 2`, which made every tap yank the list so two cards stayed on top.
// Clamp here (read-only) so a restored offset can never show a blank screen.
int RssActivity::listFirstIndex() const {
  if (rows.empty()) return 0;
  const int maxScroll = std::max(0, static_cast<int>(rows.size()) - visibleRows());
  return std::clamp(scrollTop, 0, maxScroll);
}

// Draws one card-style row: white rounded card, bold title, subtle subtitle
// and a light-gray date pill at the bottom right. The selected card gets a
// pale gray wash and a dark vertical bar along its left edge (the pressed
// shadow from the reference screen); text stays dark in both states.
void RssActivity::drawCardRow(const Rect& r, const Row& row, bool selected) {
  if (selected) {
    renderer.fillRoundedRect(r.x, r.y, r.width, r.height, CARD_RADIUS, Color::LightGray);
    renderer.fillRect(r.x, r.y + 6, 8, r.height - 12);
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
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int top = listTop();
  const int barY = renderer.getScreenHeight() - metrics.buttonHintsHeight - BAR_H;
  listBounds = Rect{SIDE_PAD, top, renderer.getScreenWidth() - 2 * SIDE_PAD, barY - CARD_GAP - top};
  rowHeight = renderer.getLineHeight(NOTOSANS_18_FONT_ID) + renderer.getLineHeight(NOTOSANS_16_FONT_ID) + 28;
  rowGap = CARD_GAP;
  if (rows.empty()) {
    renderer.drawText(NOTOSANS_18_FONT_ID, SIDE_PAD, listBounds.y + 20,
                      view == View::Favorites ? tr(STR_RSS_NO_FAVORITES) : tr(STR_RSS_NO_ARTICLES));
  } else {
    const int first = listFirstIndex();
    const int visible = std::max(1, listBounds.height / (rowHeight + rowGap));
    const int last = std::min(static_cast<int>(rows.size()), first + visible + 1);
    // Clamp the trailing "peek" card to the list band. Without this its lower
    // edge can paint over the action bar, hiding the refresh button until the
    // very last page (where rows.size() clips it) -- exactly the reported bug.
    const int listBottom = listBounds.y + listBounds.height;
    for (int i = first; i < last; ++i) {
      Rect r = rowRectFor(i - first, listBounds, rowHeight, rowGap);
      if (r.y + r.height > listBottom) r.height = listBottom - r.y;
      if (r.height > 8) drawCardRow(r, rows[i], i == selected);
    }
  }
  // Paint the fixed action bar last so it can never be covered by a card.
  drawBottomBar(false);
}

void RssActivity::drawFeedList() {
  drawHeaderAndTabs(tr(STR_RSS_TITLE), false);
  drawTabs();
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
    const int listBottom = listBounds.y + listBounds.height;
    for (int i = first; i < last; ++i) {
      Rect r = rowRectFor(i - first, listBounds, rowHeight, rowGap);
      if (r.y + r.height > listBottom) r.height = listBottom - r.y;
      if (r.height > 8) drawCardRow(r, rows[i], i == selected);
    }
  }
  renderer.drawText(NOTOSANS_16_FONT_ID, SIDE_PAD, barY - hintBand + 2, tr(STR_RSS_TAP_REMOVE));
  // Fixed action bar painted last, above both the cards and the hint.
  drawBottomBar(true);
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
  // loop() uses these to advance roughly one screen per tap and to map a
  // long-press back to the drawn word. Both must come from the real rendered
  // geometry, not a guessed constant.
  bodyPageLines = linesOnScreen;
  bodyLineHeight = lineHeight;
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
  if (dictionaryBusy) GUI.drawPopup(renderer, tr(STR_DICT_LOOKING_UP));

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
