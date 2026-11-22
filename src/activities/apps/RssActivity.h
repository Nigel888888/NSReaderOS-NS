#pragma once

#include <I18n.h>

#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/Rect.h"
#include "util/RssStore.h"

// Offline RSS reader. One activity hosts three views:
//   Articles : the cached article list (newest first), with Update / Feeds buttons.
//   Feeds    : subscription management (add via keyboard, remove, per-feed update).
//   Article  : paged plain-text reading of one cached article.
//
// Networking runs strictly in loop(), off the render task. A Phase state machine
// keeps each fetch on its own loop turn so the render task can paint the busy
// notice and the loop task can feed its watchdog. All drawing is in render()
// while holding RenderLock. This mirrors the weather app's proven structure.
class RssActivity final : public Activity {
 public:
  RssActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~RssActivity() override;

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class View : uint8_t { Articles, Feeds, Article };
  enum class Phase : uint8_t {
    Idle,
    FetchRequested,   // start next feed on the following turn
    Fetching,         // one feed's HTTPS request in progress
  };

  struct Row {
    std::string title;
    std::string subtitle;
  };

  void loadArticles();
  void loadFeeds();
  void rebuildArticleRows();
  void rebuildFeedRows();

  void drawArticleList();
  void drawFeedList();
  void drawArticleBody();
  void drawHeaderAndButtons(const char* title);
  void drawMessage();

  void updateAll();
  void beginFetch();
  void queueNextFetch();
  bool networkShouldAbort(unsigned long started) const;

  void addFeed();
  void removeSelectedFeed();
  void openArticle(int index);
  void scrollBody(int lines);
  void wrapBodyLines();

  View view = View::Articles;
  Phase phase = Phase::Idle;
  int selected = 0;
  int articlesSelected = 0;
  int feedsSelected = 0;

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

  Rect updateButton{0, 0, 0, 0};
  Rect feedsButton{0, 0, 0, 0};
  Rect addButton{0, 0, 0, 0};
  Rect backButton{0, 0, 0, 0};
  Rect listBounds{0, 0, 0, 0};
  int rowHeight = 0;
  int rowGap = 0;

  // Owned copy: callers pass stack buffers (e.g. the formatted result count).
  std::string message;
  unsigned long messageUntil = 0;
  void showMessage(const std::string& text);
};
