#pragma once

#include <I18n.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/Rect.h"
#include "util/RssStore.h"

// Vocabulary RSS reader, styled after the redesigned phone screen:
//   - a titled header with a star (toggle favorite) action
//   - a three-way segmented tab: Updates / Feeds / Favorites
//   - card-style rows (bold title, subtitle, date pill at the right edge)
//   - a bottom action bar: Update / Add feed / Manage
//
// Three views share one set of rows:
//   Updates   : all cached articles, newest first
//   Feeds     : subscription management (add via keyboard, tap to remove)
//   Favorites : articles starred by the user
//   Article   : paged plain-text reading of one cached article (no tabs/bar)
//
// Networking runs strictly in loop(), off the render task. A Phase state
// machine keeps each fetch on its own loop turn so the render task can paint
// the busy notice and the loop task can feed its watchdog. All drawing is in
// render() while holding RenderLock. This mirrors the weather app's structure.
class RssActivity final : public Activity {
 public:
  RssActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~RssActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class View : uint8_t { Updates, Feeds, Favorites, Article };
  enum class Phase : uint8_t {
    Idle,
    FetchRequested,   // start next feed on the following turn
    Fetching,         // one feed's HTTPS request in progress
  };

  struct Row {
    std::string title;
    std::string subtitle;
    std::string date;  // date pill, empty hides the pill
    bool favorite = false;
    bool read = false;         // opened before; drawn in regular weight
    std::string favoriteGuid;  // article guid, so Favorites rows can be opened
  };

  // One selectable word in the article body. Built once while wrapping the
  // body, so a long-press only has to hit-test a handful of boxes. `x` is
  // relative to listBounds.x and `line` indexes bodyLines, so page turns
  // (which only change bodyLine) do not invalidate the boxes.
  struct BodyWord {
    int x = 0;
    int width = 0;
    int line = 0;
    size_t textOffset = 0;  // byte offset of the token inside bodyText
    uint16_t textLength = 0;
    // No std::string copy: a long article can hold thousands of tokens, and
    // one heap string each starves the dictionary's 32 KB inflate window.
  };

  void loadArticles();
  void loadFeeds();
  void rebuildUpdateRows();
  void rebuildFavoriteRows();
  void rebuildFeedRows();

  void drawArticleList();
  void drawFeedList();
  void drawArticleBody();
  void drawHeaderAndTabs(const char* title, bool showStar);
  void drawTabs();
  void drawBottomBar(bool onFeeds);
  int listTop() const;
  // Rows that fit in the list band, from the last rendered geometry.
  int visibleRows() const;
  // Viewport top row. Kept independent of `selected` so a tap only moves the
  // cursor and never drags the list along with it.
  int listFirstIndex() const;
  void clampScroll();
  // Pull the viewport just enough to bring the cursor back on screen (used by
  // button navigation and restore, never by a tap).
  void ensureCursorVisible();
  void drawCardRow(const Rect& r, const Row& row, bool selected);
  void drawMessage();
  void selectTab(View target);
  // Persist / restore the selected tab and each tab's cursor + scroll offset
  // so leaving the app and coming back lands on the same card.
  void rememberCursor();
  void saveState();
  void loadState();

  void updateAll();
  void beginFetch();
  void queueNextFetch();
  bool networkShouldAbort(unsigned long started) const;

  void addFeed();
  void removeSelectedFeed();
  void toggleCurrentFavorite();
  void deleteCurrentArticle();
  void clearReadArticles();
  void openArticle(int index);
  // Reading-page navigation: open the previous/next cached article relative to
  // the currently open one. No-op at either end of the list.
  void navigateArticle(int delta);
  void scrollBody(int lines);
  void wrapBodyLines();
  // Long-press word lookup in the reading view. `rebuildBodyWords()` is part
  // of wrapping: it records the drawn x position and source offset of every
  // selectable token. The other helpers hit-test a touch, cut the source
  // sentence and run the dictionary lookup off the render task.
  void rebuildBodyWords();
  int bodyWordAt(int x, int y) const;
  std::string bodySentenceFor(int wordIndex) const;
  void lookupBodyWord(int x, int y);

  View view = View::Updates;
  Phase phase = Phase::Idle;
  int selected = 0;
  // Viewport top row, independent of the cursor. A tap moves only `selected`;
  // swiping moves `scrollTop` to page the list.
  int scrollTop = 0;
  int updatesSelected = 0;
  int feedsSelected = 0;
  int favoritesSelected = 0;
  int updatesScroll = 0;
  int feedsScroll = 0;
  int favoritesScroll = 0;
  // Select-then-open, mirroring the vocabulary notebook: -1 = nothing armed.
  int armedIndex = -1;
  // Lines per body page, measured during the last reading-view render.
  int bodyPageLines = 1;

  std::vector<RssStore::Article> articles;
  std::vector<RssStore::Feed> feeds;
  std::vector<Row> rows;  // rows for the current list view

  // Fetch progress: index into feeds, and how many new articles this pass.
  int fetchFeedIndex = -1;
  int fetchedNew = 0;
  int failedFeeds = 0;

  // Currently open article body, wrapped into screen lines.
  std::string bodyText;
  std::vector<std::string> bodyLines;
  // Byte offset in bodyText of each wrapped line's first character, so a
  // selected token can be expanded back to its original sentence.
  std::vector<size_t> bodyLineOffsets;
  std::vector<BodyWord> bodyWords;
  int bodyLine = 0;
  int bodyLineHeight = 0;  // measured by the last reading-view render
  bool dictionaryBusy = false;
  std::string openGuid;  // guid of the article being read, for the star/delete actions
  // Two-step delete: first tap arms, second tap on Delete removes the article.
  bool deleteArmed = false;
  unsigned long deleteArmedUntil = 0;
  // Two-step "clear read": first tap arms, second tap clears all read articles.
  bool clearReadArmed = false;
  unsigned long clearReadArmedUntil = 0;

  Rect tabRects[3]{};
  Rect prevButton{0, 0, 0, 0};  // reading view: previous article
  Rect starButton{0, 0, 0, 0};
  Rect deleteButton{0, 0, 0, 0};
  Rect nextButton{0, 0, 0, 0};  // reading view: next article
  Rect updateButton{0, 0, 0, 0};
  Rect addButton{0, 0, 0, 0};
  Rect manageButton{0, 0, 0, 0};
  Rect listBounds{0, 0, 0, 0};
  int rowHeight = 0;
  int rowGap = 0;

  // Owned copy: callers pass stack buffers (e.g. the formatted result count).
  std::string message;
  unsigned long messageUntil = 0;
  void showMessage(const std::string& text);
};
