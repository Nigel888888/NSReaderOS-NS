#pragma once

#include <I18n.h>

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
    std::string favoriteGuid;  // article guid, so Favorites rows can be opened
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
  void drawCardRow(const Rect& r, const Row& row, bool selected);
  void drawMessage();
  void selectTab(View target);

  void updateAll();
  void beginFetch();
  void queueNextFetch();
  bool networkShouldAbort(unsigned long started) const;

  void addFeed();
  void removeSelectedFeed();
  void toggleCurrentFavorite();
  void openArticle(int index);
  void scrollBody(int lines);
  void wrapBodyLines();

  View view = View::Updates;
  Phase phase = Phase::Idle;
  int selected = 0;
  int updatesSelected = 0;
  int feedsSelected = 0;
  int favoritesSelected = 0;

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
  int bodyLine = 0;
  std::string openGuid;  // guid of the article being read, for the star action

  Rect tabRects[3]{};
  Rect starButton{0, 0, 0, 0};
  Rect updateButton{0, 0, 0, 0};
  Rect addButton{0, 0, 0, 0};
  Rect manageButton{0, 0, 0, 0};
  Rect backButton{0, 0, 0, 0};
  Rect listBounds{0, 0, 0, 0};
  int rowHeight = 0;
  int rowGap = 0;

  // Owned copy: callers pass stack buffers (e.g. the formatted result count).
  std::string message;
  unsigned long messageUntil = 0;
  void showMessage(const std::string& text);
};
