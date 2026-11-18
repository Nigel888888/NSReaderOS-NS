#include "QuotesFile.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PersistableStore.h>

#include <algorithm>
#include <cstdio>

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
