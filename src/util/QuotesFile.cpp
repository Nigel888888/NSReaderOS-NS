#include "QuotesFile.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PersistableStore.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "util/StringUtils.h"
#include "util/TimeUtils.h"

namespace {

constexpr const char* QUOTES_FILE_PATH = "/.crosspoint/quotes.json";
constexpr int QUOTES_FILE_VERSION = 1;

// Read the JSON index with an explicit cap. HalStorage::readFile() silently
// truncates at 50 KB, which would drop quotes and, on the next write, wipe the
// whole notebook. readFileToString() has no such ceiling.
bool readDoc(const char* path, JsonDocument& doc) {
  if (!Storage.exists(path)) {
    return false;
  }
  std::string json;
  constexpr size_t CAP = 1024 * 1024;
  if (!Storage.readFileToString("QUOTES", path, CAP, json) || json.empty()) {
    LOG_ERR("QUOTES", "Cannot read quotes index %s", path);
    return false;
  }
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("QUOTES", "Quotes index parse error: %s", error.c_str());
    return false;
  }
  return true;
}

// Cuts `text` to at most `limit` bytes without splitting a UTF-8 sequence.
std::string truncateUtf8(const std::string& text, const size_t limit) {
  if (text.size() <= limit) {
    return text;
  }
  size_t end = limit;
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
    --end;
  }
  return text.substr(0, end);
}

}  // namespace

std::string QuotesFile::makeKey(const std::string& sentence, const std::string& book) {
  // FNV-1a over both fields with a separator that cannot occur inside either
  // once the sentence has been flattened by the caller.
  uint64_t hash = 1469598103934665603ull;
  const auto feed = [&hash](const char* s) {
    for (; *s; ++s) {
      hash = (hash ^ static_cast<uint8_t>(*s)) * 1099511628211ull;
    }
  };
  feed(book.c_str());
  hash = (hash ^ '\n') * 1099511628211ull;
  feed(sentence.c_str());
  char hex[17];
  std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
  return hex;
}

bool QuotesFile::load(std::vector<QuotesFile::Quote>& quotes) {
  quotes.clear();
  JsonDocument doc;
  if (!readDoc(QUOTES_FILE_PATH, doc)) {
    return false;
  }
  JsonArray items = doc["items"].as<JsonArray>();
  quotes.reserve(items.size());
  for (JsonObject obj : items) {
    Quote q;
    q.key = obj["k"] | "";
    if (q.key.empty()) {
      continue;
    }
    q.sentence = obj["s"] | "";
    q.book = obj["b"] | "";
    q.timestamp = obj["t"] | static_cast<uint32_t>(0);
    q.page = obj["p"] | static_cast<uint16_t>(0);
    q.favorite = (obj["f"] | static_cast<uint8_t>(0)) != 0;
    quotes.push_back(std::move(q));
  }
  LOG_DBG("QUOTES", "Loaded %zu quotes", quotes.size());
  return true;
}

QuotesFile::AddResult QuotesFile::add(const std::string& sentenceIn, const std::string& bookIn, const uint16_t page,
                                      const uint32_t timestamp) {
  const std::string sentence = truncateUtf8(sentenceIn, MAX_QUOTE_LENGTH);
  const std::string book = truncateUtf8(bookIn, MAX_BOOK_LENGTH);
  if (sentence.empty()) {
    return AddResult::Failed;
  }
  const std::string key = makeKey(sentence, book);

  JsonDocument doc;
  // A missing file is the first quote; an unreadable one must never be
  // rebuilt from empty, or every earlier quote is silently lost.
  const bool exists = Storage.exists(QUOTES_FILE_PATH);
  if (exists && !readDoc(QUOTES_FILE_PATH, doc)) {
    LOG_ERR("QUOTES", "Refusing to add quote: index unreadable");
    return AddResult::Failed;
  }
  JsonArray items = doc["items"].as<JsonArray>();
  if (items.isNull()) {
    items = doc["items"].to<JsonArray>();
  }
  for (JsonObject obj : items) {
    if ((obj["k"] | "") == key) {
      return AddResult::AlreadyPresent;
    }
  }

  JsonObject obj = items.add<JsonObject>();
  obj["k"] = key;
  obj["s"] = sentence;
  obj["b"] = book;
  obj["t"] = timestamp;
  obj["p"] = page;
  obj["f"] = static_cast<uint8_t>(0);
  doc["v"] = QUOTES_FILE_VERSION;

  if (!PersistableStoreBase::writeDocToFile(QUOTES_FILE_PATH, doc)) {
    LOG_ERR("QUOTES", "Failed to write quotes index");
    return AddResult::Failed;
  }
  LOG_DBG("QUOTES", "Saved quote %s", key.c_str());
  return AddResult::Added;
}

bool QuotesFile::setFavorite(const std::string& key, const bool favorite) {
  JsonDocument doc;
  if (!readDoc(QUOTES_FILE_PATH, doc)) {
    return false;
  }
  JsonArray items = doc["items"].as<JsonArray>();
  bool found = false;
  for (JsonObject obj : items) {
    if ((obj["k"] | "") == key) {
      obj["f"] = static_cast<uint8_t>(favorite ? 1 : 0);
      found = true;
      break;
    }
  }
  if (!found) {
    return false;
  }
  return PersistableStoreBase::writeDocToFile(QUOTES_FILE_PATH, doc);
}

bool QuotesFile::remove(const std::string& key) {
  JsonDocument doc;
  if (!readDoc(QUOTES_FILE_PATH, doc)) {
    return false;
  }
  JsonArray items = doc["items"].as<JsonArray>();
  int index = -1;
  for (size_t i = 0; i < items.size(); ++i) {
    if ((items[i]["k"] | "") == key) {
      index = static_cast<int>(i);
      break;
    }
  }
  if (index < 0) {
    return false;
  }
  items.remove(index);
  return PersistableStoreBase::writeDocToFile(QUOTES_FILE_PATH, doc);
}

namespace {

// English weekday/month names: clippings are consumed by Kindle/Readwise
// tooling, whose parsers expect the fixed English "Added on" line regardless
// of the device UI locale.
const char* const kWeekdays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const char* const kMonths[] = {"January", "February", "March", "April", "May", "June",
                               "July", "August", "September", "October", "November", "December"};

// Collapse any line break / carriage return to a space so one highlight is
// always one physical line in the exported block.
std::string flattenForClippings(const std::string& text) {
  std::string out = text;
  for (char& c : out) {
    if (c == '\r' || c == '\n') {
      c = ' ';
    }
  }
  return out;
}

// "Monday, January 1, 2024 10:00:00 AM" -- the exact form Kindle writes and
// Readwise parses. Returns an empty string when the timestamp is unusable.
std::string kindleDateTime(const uint32_t epochSeconds) {
  std::tm local{};
  if (epochSeconds == 0 || !TimeUtils::getLocalDateTime(epochSeconds, local)) {
    return std::string();
  }
  int hour12 = local.tm_hour % 12;
  if (hour12 == 0) {
    hour12 = 12;
  }
  const char* ampm = local.tm_hour < 12 ? "AM" : "PM";
  char buf[96];
  const int wday = (local.tm_wday >= 0 && local.tm_wday < 7) ? local.tm_wday : 0;
  std::snprintf(buf, sizeof(buf), "%s, %s %d, %d %d:%02d:%02d %s", kWeekdays[wday],
                kMonths[(local.tm_mon >= 0 && local.tm_mon < 12) ? local.tm_mon : 0], local.tm_mday,
                local.tm_year + 1900, hour12, local.tm_min, local.tm_sec, ampm);
  return std::string(buf);
}

}  // namespace

namespace {

// Writes one Kindle clippings file atomically via a .tmp sibling. `quotes`
// are the highlights for this file; `titleOverride` is non-empty when every
// entry shares one book title (the per-book export) and should be printed on
// each divider block instead of the per-quote book field.
// Returns highlights written, 0 when none had text, -1 on I/O failure.
int writeClippingsFile(const std::string& path, const std::vector<QuotesFile::Quote>& quotes,
                       const std::string& titleOverride) {
  const std::string temp = path + ".tmp";
  HalFile out;
  if (!Storage.openFileForWrite("QUOTES", temp.c_str(), out)) {
    LOG_ERR("QUOTES", "Clippings export: cannot open %s", temp.c_str());
    return -1;
  }

  bool ok = true;
  int written = 0;
  const std::string divider = "==========\n";
  for (const QuotesFile::Quote& q : quotes) {
    if (q.sentence.empty()) {
      continue;
    }
    // No author field in our model -> omit the " (Author)" suffix.
    std::string title = titleOverride.empty() ? q.book : titleOverride;
    if (title.empty()) {
      title = "Unknown Book";
    }
    title = flattenForClippings(title);

    std::string meta = "- Your Highlight";
    if (q.page > 0) {
      char pageBuf[32];
      std::snprintf(pageBuf, sizeof(pageBuf), " on page %u", static_cast<unsigned>(q.page));
      meta += pageBuf;
    }
    const std::string when = kindleDateTime(q.timestamp);
    if (!when.empty()) {
      meta += " | Added on " + when;
    }

    const std::string block = divider + title + "\n" + meta + "\n\n" + flattenForClippings(q.sentence) + "\n";
    if (out.write(block.data(), block.size()) != block.size()) {
      ok = false;
      break;
    }
    ++written;
  }
  if (ok && written > 0 && out.write(divider.data(), divider.size()) != divider.size()) {
    ok = false;
  }
  out.flush();
  out.close();

  if (!ok || written == 0) {
    Storage.remove(temp.c_str());
    return ok ? 0 : -1;
  }
  if (Storage.exists(path.c_str()) && !Storage.remove(path.c_str())) {
    Storage.remove(temp.c_str());
    LOG_ERR("QUOTES", "Clippings export: cannot replace %s", path.c_str());
    return -1;
  }
  if (!Storage.rename(temp.c_str(), path.c_str())) {
    Storage.remove(temp.c_str());
    LOG_ERR("QUOTES", "Clippings export: cannot rename to %s", path.c_str());
    return -1;
  }
  LOG_DBG("QUOTES", "Clippings export wrote %d highlights to %s", written, path.c_str());
  return written;
}

}  // namespace

int QuotesFile::exportClippings(const std::string& path) {
  std::vector<Quote> quotes;
  if (!load(quotes)) {
    return -1;  // unreadable index
  }
  if (quotes.empty()) {
    return 0;  // nothing collected yet
  }
  return writeClippingsFile(path, quotes, /*titleOverride=*/std::string());
}

int QuotesFile::exportClippingsByBook(const std::string& dir) {
  std::vector<Quote> quotes;
  if (!load(quotes)) {
    return -1;
  }
  if (quotes.empty()) {
    return 0;
  }

  if (!Storage.exists(dir.c_str()) && !Storage.mkdir(dir.c_str())) {
    LOG_ERR("QUOTES", "Clippings export: cannot create dir %s", dir.c_str());
    return -1;
  }

  // Group by source title, preserving first-seen order for stable filenames.
  std::vector<std::string> books;
  std::vector<std::vector<Quote>> groups;
  for (const Quote& q : quotes) {
    const std::string book = q.book.empty() ? std::string("Unknown Book") : q.book;
    size_t found = books.size();
    for (size_t i = 0; i < books.size(); ++i) {
      if (books[i] == book) {
        found = i;
        break;
      }
    }
    if (found == books.size()) {
      books.push_back(book);
      groups.emplace_back();
    }
    groups[found].push_back(q);
  }

  // De-duplicate sanitized filenames (two titles can sanitize to the same
  // name) by appending a numeric suffix before the extension.
  std::vector<std::string> used;
  int filesWritten = 0;
  for (size_t i = 0; i < books.size(); ++i) {
    const std::string stem = StringUtils::sanitizeFilename(books[i], 80);
    std::string base = stem;
    int suffix = 2;
    while (std::find(used.begin(), used.end(), base) != used.end()) {
      char n[24];
      std::snprintf(n, sizeof(n), "%s_%d", stem.c_str(), suffix++);
      base = n;
    }
    used.push_back(base);

    const std::string path = dir + "/" + base + ".txt";
    const int result = writeClippingsFile(path, groups[i], /*titleOverride=*/books[i]);
    if (result < 0) {
      return -1;  // a real I/O failure aborts the batch
    }
    if (result > 0) {
      ++filesWritten;
    }
  }
  LOG_DBG("QUOTES", "Clippings export wrote %d book files to %s", filesWritten, dir.c_str());
  return filesWritten;
}
