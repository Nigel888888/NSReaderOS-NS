#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace freeink {
class SecureHttpClient;
}

// Offline RSS reader data layer.
//
// Feeds are fetched over HTTPS while online, parsed with the expat SAX parser
// already shipped for EPUB/WebDAV, and cached on the SD card as plain text so
// articles can be read with no connection. Nothing here touches the renderer.
//
// On disk (under /.crosspoint/rss):
//   feeds.json               array of {"url","name"} subscriptions
//   articles.json            index: title/date/source/body file per article
//   bodies/<hash>.txt        the plain-text body of one article
//
// The index is small enough to hold in RAM; bodies are read one at a time when
// an article is opened, mirroring the vocabulary notebook's split storage.
class RssStore {
 public:
  // Longest article body kept on disk (bytes). Keeps RAM and SD use bounded.
  static constexpr size_t MAX_BODY_LENGTH = 8192;
  // Longest title kept (bytes).
  static constexpr size_t MAX_TITLE_LENGTH = 160;
  // Maximum articles retained in the index across all feeds. Lowered from 200
  // to 100: the whole index is parsed into RAM on entry, so every extra row
  // costs heap and slows the first paint on this device.
  static constexpr size_t MAX_ARTICLES = 100;
  // Maximum number of subscriptions.
  static constexpr size_t MAX_FEEDS = 24;

  struct Feed {
    std::string url;
    std::string name;  // channel title, may be empty until first update
  };

  struct Article {
    std::string guid;    // stable id from <guid> or <link>
    std::string title;
    std::string date;    // human-readable date, already formatted
    std::string feed;    // source feed name
    std::string bodyFile;  // file name under bodies/
    uint32_t timestamp = 0;  // for newest-first sorting
    int feedIndex = -1;      // index into the feeds array, filled on load
    bool favorite = false;   // starred by the user, shown under the Favorites tab
    bool read = false;       // opened at least once; drawn in regular weight
    // In-memory only: the parsed body text, consumed by fetchFeed when writing
    // the body file. Never serialized into articles.json.
    std::string bodyText;
  };

  // ---- subscriptions ----------------------------------------------------
  static bool loadFeeds(std::vector<Feed>& feeds);
  static bool saveFeeds(const std::vector<Feed>& feeds);
  static bool addFeed(std::vector<Feed>& feeds, const std::string& url);
  static bool removeFeed(std::vector<Feed>& feeds, size_t index);
  // Extracts a readable name from a feed URL, used before the first update.
  static std::string nameFromUrl(const std::string& url);

  // ---- article index ----------------------------------------------------
  static bool loadArticles(std::vector<Article>& articles);
  static bool saveArticles(const std::vector<Article>& articles);
  static bool loadBody(const Article& article, std::string& body);
  // Removes cached articles that belong to a removed feed index.
  static size_t dropArticlesForFeed(std::vector<Article>& articles, int feedIndex);
  // Loads the guid dismissal list (one guid per line) into `out`.
  static void loadDismissed(std::vector<std::string>& out);
  // Toggles the star flag on one article (matched by guid) and persists the
  // index. Returns true when a matching article was found and saved.
  static bool toggleFavorite(std::vector<Article>& articles, const std::string& guid);
  // Marks one article as read (matched by guid) and persists the index.
  static bool markRead(std::vector<Article>& articles, const std::string& guid);
  // Deletes one article by guid: removes its body file, drops it from the
  // index and persists. Returns true when a matching article was removed.
  static bool deleteArticle(std::vector<Article>& articles, const std::string& guid);
  // Clears every read article: removes its body file, drops it from the index
  // and adds its guid to a persistent dismissal list so a later refresh no
  // longer re-downloads it. Returns the number of articles cleared.
  static size_t clearReadArticles(std::vector<Article>& articles);
  // How many days back a refresh is allowed to import. Older feed items are
  // skipped so the cache stays current and bounded.
  static constexpr uint32_t REFRESH_WINDOW_DAYS = 7;

  // ---- fetching & parsing ----------------------------------------------
  // Downloads `url`, parses the RSS/Atom feed and appends new articles to
  // `articles`, writing each body to disk and the updated index at the end.
  // Returns the number of new articles, or -1 on a transport/parse failure.
  // The `shouldAbort` callback is polled during network waits (watchdog feed +
  // deadline); pass a lambda that returns true to cancel.
  // `session` is an optional caller-owned reusable HTTPS client. Passing one
  // avoids a fresh wolfSSL CTX/connection per feed (each handshake allocates
  // ~tens of KB and fragments the heap on this device); nullptr keeps the old
  // per-call client behaviour.
  static int fetchFeed(const Feed& feed, int feedIndex, std::vector<Article>& articles,
                       const std::function<bool()>& shouldAbort, freeink::SecureHttpClient* session = nullptr);

  // Turns feed HTML (e.g. <br/>, <a href=...>) into readable plain text.
  static std::string htmlToText(const std::string& html);

 private:
  static std::string bodiesDir();
  static std::string bodyPath(const std::string& bodyFile);
  static std::string bodyFileName(const std::string& guid);
  static std::string dismissedPath();
};
