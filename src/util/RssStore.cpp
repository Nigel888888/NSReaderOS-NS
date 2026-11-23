#include "RssStore.h"

#ifdef RICKYOS_PRODUCT

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <expat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "PluginHttp.h"

namespace {

constexpr const char* RSS_DIR = "/.crosspoint/rss";
constexpr const char* FEEDS_PATH = "/.crosspoint/rss/feeds.json";
constexpr const char* ARTICLES_PATH = "/.crosspoint/rss/articles.json";
// A generous cap; feeds stay a few KB in practice. Keeps a corrupt feed from
// driving an unbounded allocation on this memory-constrained device.
constexpr size_t MAX_FEED_RESPONSE = 256 * 1024;
constexpr size_t INDEX_READ_CAP = 512 * 1024;

// writeFile() does not create parent directories. Do it explicitly so the
// first update after a factory reset can persist anything.
void ensureRssDir() {
  if (!Storage.exists(RSS_DIR)) Storage.mkdir(RSS_DIR);
}

uint32_t hashGuid(const std::string& guid) {
  uint32_t hash = 2166136261u;
  for (const char c : guid) hash = (hash ^ static_cast<unsigned char>(c)) * 16777619u;
  return hash;
}

// Truncates to `limit` bytes without splitting a UTF-8 codepoint.
std::string truncateUtf8(const std::string& text, const size_t limit) {
  if (text.size() <= limit) return text;
  size_t end = limit;
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
  return text.substr(0, end);
}

std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

// "Sun, 22 Nov 2026 05:00:00 GMT" -> "2026-11-22" when possible; otherwise the
// original string is kept (trimmed and shortened) so a non-RFC822 feed still
// shows something meaningful.
std::string formatRfc822(const std::string& raw) {
  const std::string s = trim(raw);
  static const char* MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  // Find "DD Mon YYYY".
  for (size_t i = 0; i + 10 < s.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(s[i]))) continue;
    int mon = -1;
    for (int m = 0; m < 12; ++m) {
      if (s.compare(i + 3, 3, MONTHS[m]) == 0) { mon = m + 1; break; }
    }
    if (mon < 0) continue;
    if (!std::isdigit(static_cast<unsigned char>(s[i + 7])) || !std::isdigit(static_cast<unsigned char>(s[i + 8])) ||
        !std::isdigit(static_cast<unsigned char>(s[i + 9])) || !std::isdigit(static_cast<unsigned char>(s[i + 10]))) {
      continue;
    }
    char out[16];
    std::snprintf(out, sizeof(out), "%c%c%c%c-%02d-%c%c", s[i + 7], s[i + 8], s[i + 9], s[i + 10], mon, s[i],
                  s[i + 1]);
    return std::string(out);
  }
  return truncateUtf8(s, 12);
}

// Timestamp from RFC822 for sorting; 0 when it cannot be parsed.
uint32_t rfc822Timestamp(const std::string& raw) {
  const std::string formatted = formatRfc822(raw);
  if (formatted.size() < 10) return 0;
  int y = 0, m = 0, d = 0;
  if (std::sscanf(formatted.c_str(), "%4d-%2d-%2d", &y, &m, &d) != 3) return 0;
  // Days since a fixed epoch, good enough for ordering.
  long days = 0;
  for (int year = 2000; year < y; ++year) days += ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0) ? 366 : 365;
  static const int MD[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  days += MD[std::max(0, std::min(11, m - 1))] + d;
  if (m > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) ++days;
  return static_cast<uint32_t>(std::max(0L, days));
}

// ---- SAX feed parser -----------------------------------------------------
struct FeedParser {
  XML_Parser xml = nullptr;
  int depth = 0;
  int itemDepth = 0;
  bool inItem = false;
  const char* capture = nullptr;  // which field is being collected
  std::string text;
  std::string title, link, guid, date, description;
  std::vector<RssStore::Article> parsed;
  bool failed = false;
};

const char* localName(const char* name) {
  const char* sep = std::strchr(name, ':');
  return sep ? sep + 1 : name;
}

void XMLCALL onStart(void* data, const char* name, const char** /*attrs*/) {
  auto* p = static_cast<FeedParser*>(data);
  if (++p->depth > 16) { p->failed = true; XML_StopParser(p->xml, XML_FALSE); return; }
  const char* tag = localName(name);
  if (!std::strcmp(tag, "item") || !std::strcmp(tag, "entry")) {
    p->inItem = true;
    p->itemDepth = p->depth;
    p->title.clear(); p->link.clear(); p->guid.clear(); p->date.clear(); p->description.clear();
    p->capture = nullptr;
    return;
  }
  if (!p->inItem) return;
  // Atom uses <published>/<updated> and <summary>/<content>; RSS uses pubDate.
  if (!std::strcmp(tag, "title")) p->capture = "title";
  else if (!std::strcmp(tag, "guid") || !std::strcmp(tag, "id")) p->capture = "guid";
  else if (!std::strcmp(tag, "pubDate") || !std::strcmp(tag, "published") || !std::strcmp(tag, "updated"))
    p->capture = "date";
  else if (!std::strcmp(tag, "description") || !std::strcmp(tag, "summary") || !std::strcmp(tag, "content"))
    p->capture = "description";
  else if (!std::strcmp(tag, "link")) p->capture = "link";
  if (p->capture) p->text.clear();
}

void XMLCALL onText(void* data, const char* value, int len) {
  auto* p = static_cast<FeedParser*>(data);
  if (!p->inItem || !p->capture || len <= 0) return;
  // Hard per-field cap keeps a malicious feed from exhausting RAM.
  if (p->text.size() < RssStore::MAX_BODY_LENGTH) p->text.append(value, static_cast<size_t>(len));
}

void XMLCALL onEnd(void* data, const char* name) {
  auto* p = static_cast<FeedParser*>(data);
  const char* tag = localName(name);
  if (p->inItem && p->capture) {
    if ((!std::strcmp(p->capture, "title") && !std::strcmp(tag, "title")) ||
        (!std::strcmp(p->capture, "guid") && (!std::strcmp(tag, "guid") || !std::strcmp(tag, "id"))) ||
        (!std::strcmp(p->capture, "date") && (!std::strcmp(tag, "pubDate") || !std::strcmp(tag, "published") ||
                                              !std::strcmp(tag, "updated"))) ||
        (!std::strcmp(p->capture, "description") && (!std::strcmp(tag, "description") ||
                                                     !std::strcmp(tag, "summary") || !std::strcmp(tag, "content"))) ||
        (!std::strcmp(p->capture, "link") && !std::strcmp(tag, "link"))) {
      const std::string value = trim(p->text);
      if (!std::strcmp(p->capture, "title")) p->title = value;
      else if (!std::strcmp(p->capture, "guid")) p->guid = value;
      else if (!std::strcmp(p->capture, "date")) p->date = value;
      else if (!std::strcmp(p->capture, "description")) p->description += p->text;
      else if (!std::strcmp(p->capture, "link")) {
        // Atom links put the URL in href; with no character text, keep whatever
        // text we got (RSS puts the URL inline).
        if (p->link.empty()) p->link = value;
      }
      p->capture = nullptr;
      p->text.clear();
    }
  }
  if (p->inItem && (!std::strcmp(tag, "item") || !std::strcmp(tag, "entry")) && p->itemDepth == p->depth) {
    p->inItem = false;
    RssStore::Article a;
    a.title = truncateUtf8(trim(p->title), RssStore::MAX_TITLE_LENGTH);
    if (a.title.empty()) return;
    if (p->guid.empty()) a.guid = p->link.empty() ? a.title : p->link;
    else a.guid = p->guid;
    a.date = formatRfc822(p->date);
    a.timestamp = rfc822Timestamp(p->date);
    a.bodyText = p->description;  // consumed by fetchFeed, never serialized
    p->parsed.push_back(std::move(a));
  }
  --p->depth;
}

}  // namespace

// Placeholder definitions are provided below; the real parse routine lives in
// fetchFeed where it can also write bodies to disk.

std::string RssStore::bodiesDir() { return std::string(RSS_DIR) + "/bodies"; }

std::string RssStore::bodyPath(const std::string& bodyFile) { return bodiesDir() + "/" + bodyFile; }

std::string RssStore::bodyFileName(const std::string& guid) {
  char name[32];
  std::snprintf(name, sizeof(name), "%08x.txt", hashGuid(guid));
  return std::string(name);
}

std::string RssStore::nameFromUrl(const std::string& url) {
  // Host name minus www., e.g. www.thefreedictionary.com -> thefreedictionary.com
  size_t scheme = url.find("://");
  size_t start = scheme == std::string::npos ? 0 : scheme + 3;
  size_t slash = url.find('/', start);
  std::string host = url.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
  if (host.compare(0, 4, "www.") == 0) host = host.substr(4);
  return truncateUtf8(host, 40);
}

// CDATA is passed through by expat verbatim, so HTML entities inside it need
// decoding here. expat has already decoded entities outside CDATA.
std::string decodeEntity(const std::string& e) {
  if (e == "&amp;") return "&";
  if (e == "&lt;") return "<";
  if (e == "&gt;") return ">";
  if (e == "&quot;") return "\"";
  if (e == "&#39;" || e == "&apos;") return "'";
  if (e == "&nbsp;") return " ";
  if (e.size() > 3 && e[1] == '#') {
    int cp = 0;
    if (e[2] == 'x' || e[2] == 'X') {
      if (std::sscanf(e.c_str() + 3, "%x", &cp) != 1) return e;
    } else if (std::sscanf(e.c_str() + 2, "%d", &cp) != 1) {
      return e;
    }
    // Keep the ASCII range only; non-ASCII codepoints would need re-encoding
    // to UTF-8, and leaving them verbatim is safer than emitting broken bytes.
    if (cp >= 32 && cp <= 126) return std::string(1, static_cast<char>(cp));
  }
  return e;
}

std::string RssStore::htmlToText(const std::string& html) {
  std::string out;
  out.reserve(html.size());
  bool inTag = false;
  bool pendingBreak = false;
  for (size_t i = 0; i < html.size(); ++i) {
    const char c = html[i];
    if (inTag) {
      if (c == '>') inTag = false;
      continue;
    }
    if (c == '<') {
      // <br ...> becomes a newline; other tags are dropped. <a> keeps its text.
      size_t j = i + 1;
      while (j < html.size() && std::isspace(static_cast<unsigned char>(html[j]))) ++j;
      if (j < html.size() && (html[j] == 'b' || html[j] == 'B')) {
        if (html.compare(j, 2, "br") == 0 || html.compare(j, 2, "BR") == 0) pendingBreak = true;
      }
      inTag = true;
      continue;
    }
    if (c == '&') {
      const size_t semi = html.find(';', i + 1);
      if (semi != std::string::npos && semi - i <= 10) {
        out += decodeEntity(html.substr(i, semi - i + 1));
        i = semi;
        pendingBreak = false;
        continue;
      }
    }
    if (pendingBreak) {
      if (!out.empty() && out.back() != '\n') out.push_back('\n');
      pendingBreak = false;
    }
    out.push_back(c);
  }
  // Trim each line (CDATA is indented in real feeds) and collapse blank runs.
  std::string clean;
  clean.reserve(out.size());
  std::string line;
  int blank = 0;
  for (const char c : out) {
    if (c != '\n' && c != '\r') {
      line.push_back(c);
      continue;
    }
    const std::string t = trim(line);
    if (!t.empty()) {
      clean += t;
      clean.push_back('\n');
      blank = 0;
    } else if (blank == 0 && !clean.empty()) {
      clean.push_back('\n');
      blank = 1;
    }
    line.clear();
  }
  const std::string tail = trim(line);
  if (!tail.empty()) clean += tail;
  while (!clean.empty() && clean.back() == '\n') clean.pop_back();
  return truncateUtf8(clean, MAX_BODY_LENGTH);
}

bool RssStore::loadFeeds(std::vector<Feed>& feeds) {
  feeds.clear();
  ensureRssDir();
  std::string json;
  if (!Storage.readFileToString("RSS", FEEDS_PATH, INDEX_READ_CAP, json) || json.empty()) {
    // First run: seed a useful default subscription so the reader works
    // immediately after flashing. The user can remove or add feeds later.
    Feed def;
    def.url = "https://www.thefreedictionary.com/_/WoD/rss.aspx";
    def.name = "Word of the Day";
    feeds.push_back(def);
    saveFeeds(feeds);
    return true;
  }
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  JsonArray arr = doc.as<JsonArray>();
  if (arr.isNull()) return false;
  for (JsonVariant v : arr) {
    const char* url = v["url"] | "";
    if (!url[0]) continue;
    Feed f;
    f.url = url;
    f.name = v["name"] | "";
    feeds.push_back(std::move(f));
    if (feeds.size() >= MAX_FEEDS) break;
  }
  return true;
}

bool RssStore::saveFeeds(const std::vector<Feed>& feeds) {
  ensureRssDir();
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (const Feed& f : feeds) {
    JsonObject o = arr.add<JsonObject>();
    o["url"] = f.url;
    o["name"] = f.name;
  }
  String out;
  serializeJson(doc, out);
  return Storage.writeFile(FEEDS_PATH, out);
}

bool RssStore::addFeed(std::vector<Feed>& feeds, const std::string& url) {
  std::string u = trim(url);
  if (u.empty() || (u.compare(0, 7, "http://") != 0 && u.compare(0, 8, "https://") != 0)) return false;
  if (feeds.size() >= MAX_FEEDS) return false;
  for (const Feed& f : feeds) if (f.url == u) return false;
  Feed f;
  f.url = u;
  f.name = nameFromUrl(u);
  feeds.push_back(std::move(f));
  return saveFeeds(feeds);
}

bool RssStore::removeFeed(std::vector<Feed>& feeds, size_t index) {
  if (index >= feeds.size()) return false;
  feeds.erase(feeds.begin() + static_cast<long>(index));
  return saveFeeds(feeds);
}

bool RssStore::loadArticles(std::vector<Article>& articles) {
  articles.clear();
  std::string json;
  if (!Storage.readFileToString("RSS", ARTICLES_PATH, INDEX_READ_CAP, json) || json.empty()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  JsonArray arr = doc.as<JsonArray>();
  if (arr.isNull()) return false;
  for (JsonVariant v : arr) {
    Article a;
    a.guid = v["guid"] | "";
    a.title = v["title"] | "";
    a.date = v["date"] | "";
    a.feed = v["feed"] | "";
    a.bodyFile = v["file"] | "";
    a.timestamp = static_cast<uint32_t>(v["ts"] | 0);
    a.feedIndex = static_cast<int>(v["fi"] | -1);
    a.favorite = static_cast<bool>(v["fav"] | false);
    if (a.guid.empty() || a.bodyFile.empty()) continue;
    articles.push_back(std::move(a));
    if (articles.size() >= MAX_ARTICLES) break;
  }
  return true;
}

bool RssStore::saveArticles(const std::vector<Article>& articles) {
  ensureRssDir();
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (const Article& a : articles) {
    JsonObject o = arr.add<JsonObject>();
    o["guid"] = a.guid;
    o["title"] = a.title;
    o["date"] = a.date;
    o["feed"] = a.feed;
    o["file"] = a.bodyFile;
    o["ts"] = a.timestamp;
    o["fi"] = a.feedIndex;
    o["fav"] = a.favorite;
  }
  String out;
  serializeJson(doc, out);
  return Storage.writeFile(ARTICLES_PATH, out);
}

bool RssStore::toggleFavorite(std::vector<Article>& articles, const std::string& guid) {
  for (Article& a : articles) {
    if (a.guid == guid) {
      a.favorite = !a.favorite;
      return saveArticles(articles);
    }
  }
  return false;
}

bool RssStore::loadBody(const Article& article, std::string& body) {
  body.clear();
  return Storage.readFileToString("RSS", bodyPath(article.bodyFile).c_str(), MAX_BODY_LENGTH + 1, body);
}

size_t RssStore::dropArticlesForFeed(std::vector<Article>& articles, int feedIndex) {
  size_t removed = 0;
  for (size_t i = 0; i < articles.size();) {
    if (articles[i].feedIndex == feedIndex) {
      Storage.remove(bodyPath(articles[i].bodyFile).c_str());
      articles.erase(articles.begin() + static_cast<long>(i));
      ++removed;
    } else {
      if (articles[i].feedIndex > feedIndex) --articles[i].feedIndex;
      ++i;
    }
  }
  return removed;
}

int RssStore::fetchFeed(const Feed& feed, int feedIndex, std::vector<Article>& articles,
                        const std::function<bool()>& shouldAbort) {
  // Passing a null session makes PluginHttp build a per-call stack client.
  // One request per fetch keeps it simple and avoids holding TLS state.
  String body;
  const int status = pluginhttp::request(nullptr, feed.url, "GET", "", {}, body, MAX_FEED_RESPONSE, nullptr,
                                         shouldAbort);
  if (status != 200) return -1;

  FeedParser parser;
  parser.xml = XML_ParserCreate("UTF-8");
  if (!parser.xml) return -1;
  XML_SetUserData(parser.xml, &parser);
  XML_SetElementHandler(parser.xml, onStart, onEnd);
  XML_SetCharacterDataHandler(parser.xml, onText);
  const bool ok = XML_Parse(parser.xml, body.c_str(), static_cast<int>(body.length()), 1) == XML_STATUS_OK;
  XML_ParserFree(parser.xml);
  if (!ok || parser.failed || parser.parsed.empty()) return -1;

  // Ensure the bodies directory exists once.
  if (!Storage.exists(bodiesDir().c_str())) Storage.mkdir(bodiesDir().c_str());

  int added = 0;
  for (auto& a : parser.parsed) {
    if (shouldAbort && shouldAbort()) break;
    // De-duplicate against the existing index by guid.
    bool exists = false;
    for (const Article& old : articles) {
      if (old.guid == a.guid) { exists = true; break; }
    }
    if (exists) continue;
    a.feedIndex = feedIndex;
    a.feed = feed.name.empty() ? nameFromUrl(feed.url) : feed.name;
    a.bodyFile = bodyFileName(a.guid);
    // Store the body as a separate plain-text file.
    const std::string text = RssStore::htmlToText(a.bodyText);
    HalFile file;
    if (!Storage.openFileForWrite("RSS", bodyPath(a.bodyFile).c_str(), file)) continue;
    file.write(text.data(), text.size());
    file.flush();
    file.close();
    a.bodyText.clear();  // do not retain large text in the index-carrying vector
    articles.push_back(std::move(a));
    ++added;
    if (articles.size() >= MAX_ARTICLES) break;
  }
  if (added > 0) {
    // Trim to the newest N by timestamp so the index stays bounded.
    if (articles.size() > MAX_ARTICLES) {
      std::sort(articles.begin(), articles.end(),
                [](const Article& x, const Article& y) { return x.timestamp > y.timestamp; });
      articles.resize(MAX_ARTICLES);
    }
    saveArticles(articles);
  }
  return added;
}

#endif  // RICKYOS_PRODUCT
