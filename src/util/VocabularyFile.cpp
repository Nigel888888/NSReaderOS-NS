#include "VocabularyFile.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PersistableStore.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "util/TimeUtils.h"

namespace {

// On-disk format version. Bump only when the shape changes in a way an older
// build cannot read.
constexpr int VOCABULARY_FILE_VERSION = 2;

// The index lives in the CrossPoint data directory next to the other per-user
// stores (bookmarks, settings).
constexpr const char* VOCABULARY_FILE_PATH = "/.crosspoint/vocabulary.json";

// The long text of every word lives in this directory, one file per word.
constexpr const char* VOCABULARY_TEXT_DIR = "/.crosspoint/vocabulary-defs";

// A notebook index can grow past 50 KB (roughly 200-320 words at ~160-256 B
// each).  PersistableStoreBase::readDocFromFile goes through
// HalStorage::readFile(), which silently caps a read at 50 KB -- the JSON then
// fails to parse and, worse, add() rebuilds the array from an empty document
// and drops every previously saved word.  Read the index with the explicit-cap
// helper instead; it has no 50 KB ceiling and probes the heap before growing.
//
// The cap is deliberately generous: at 1 MiB this holds several thousand
// words. A corrupted/failed read returns false without touching `doc`, so
// callers never write a partial index back over a good one.
bool readIndexDoc(const char* path, JsonDocument& doc) {
  if (!Storage.exists(path)) {
    return false;
  }
  std::string json;
  constexpr size_t INDEX_CAP = 1024 * 1024;
  if (!Storage.readFileToString("VOC", path, INDEX_CAP, json) || json.empty()) {
    LOG_ERR("VOC", "Cannot read vocabulary index %s", path);
    return false;
  }
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("VOC", "Vocabulary index parse error: %s", error.c_str());
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
  // Walk back over continuation bytes (0b10xxxxxx) so the cut lands on a
  // codepoint boundary and the stored text stays valid UTF-8.
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
    --end;
  }
  return text.substr(0, end);
}

// A short hash of the headword, so two headwords that sanitize to the same
// file name still get separate files.
uint32_t hashHeadword(const std::string& headword) {
  uint32_t hash = 2166136261u;
  for (const char c : headword) {
    // Case-fold: "Apple" and "apple" are the same word.
    unsigned char lower = static_cast<unsigned char>(c);
    if (lower >= 'A' && lower <= 'Z') {
      lower = static_cast<unsigned char>(lower - 'A' + 'a');
    }
    hash = (hash ^ lower) * 16777619u;
  }
  return hash;
}

}  // namespace

// One word's long text: a file name that is safe on the SD card and unique
// for the word.
std::string VocabularyFile::textFileName(const std::string& headword) {
  std::string name;
  name.reserve(headword.size() + 12);
  for (const char c : headword) {
    const unsigned char u = static_cast<unsigned char>(c);
    const bool plain =
        (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '-' || u == '_';
    name.push_back(plain ? c : '_');
  }
  if (name.empty()) {
    name = "word";
  }
  // The hash is appended so that "don't" and "dont" cannot collide.
  char suffix[12];
  std::snprintf(suffix, sizeof(suffix), "-%08x", hashHeadword(headword));
  name += suffix;
  name += ".txt";
  return name;
}

std::string VocabularyFile::textFilePath(const std::string& headword) {
  return std::string(VOCABULARY_TEXT_DIR) + "/" + textFileName(headword);
}

// One line, whitespace collapsed: the list row shows this excerpt next to the
// headword, the full text stays on disk.
std::string VocabularyFile::makeSummary(const std::string& definition) {
  std::string flat;
  flat.reserve(MAX_SUMMARY_LENGTH);
  bool pendingSpace = false;
  for (const char c : definition) {
    if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
      pendingSpace = !flat.empty();
      continue;
    }
    if (pendingSpace) {
      flat.push_back(' ');
      pendingSpace = false;
    }
    flat.push_back(c);
  }
  if (flat.size() > MAX_SUMMARY_LENGTH) {
    flat = truncateUtf8(flat, MAX_SUMMARY_LENGTH);
    // The cut may have landed right after a space; drop it.
    while (!flat.empty() && flat.back() == ' ') {
      flat.pop_back();
    }
  }
  return flat;
}

bool VocabularyFile::load(std::vector<VocabularyFile::Entry>& entries) {
  entries.clear();

  JsonDocument doc;
  if (!readIndexDoc(VOCABULARY_FILE_PATH, doc)) {
    return false;
  }

  JsonArray items = doc["items"].as<JsonArray>();
  entries.reserve(items.size());
  for (JsonObject obj : items) {
    VocabularyFile::Entry entry;
    entry.headword = obj["w"] | "";
    if (entry.headword.empty()) {
      continue;
    }
    entry.summary = obj["s"] | "";
    entry.book = obj["b"] | "";
    entry.timestamp = obj["t"] | static_cast<uint32_t>(0);
    entry.card.state = obj["st"] | static_cast<uint8_t>(0);
    entry.card.stepIndex = obj["si"] | static_cast<uint8_t>(0);
    entry.card.stability = obj["sta"] | 0.0f;
    entry.card.difficulty = obj["dif"] | 0.0f;
    entry.card.dueDay = obj["dd"] | static_cast<int32_t>(0);
    entry.card.lastReviewDay = obj["lr"] | static_cast<int32_t>(-1);
    entry.card.reps = obj["rp"] | static_cast<uint16_t>(0);
    entry.card.lapses = obj["lp"] | static_cast<uint16_t>(0);
    entry.card.dueMinute = obj["dm"] | static_cast<uint16_t>(0);
    entry.lastRating = obj["ra"] | static_cast<uint8_t>(0);
    entries.push_back(std::move(entry));
  }

  LOG_DBG("VOC", "Loaded %zu vocabulary entries", entries.size());
  return true;
}

VocabularyFile::AddResult VocabularyFile::add(const std::string& headword, const std::string& definition,
                                              const std::string& sentence, const std::string& book,
                                              const uint32_t timestamp) {
  const std::string word = truncateUtf8(headword, MAX_HEADWORD_LENGTH);
  if (word.empty()) {
    return AddResult::Failed;
  }

  JsonDocument doc;
  // A missing file is not an error here: it is simply the first saved word.
  // But a file that exists yet fails to read/parse must abort the add -- writing
  // an empty rebuilt array over it would silently destroy every saved word.
  if (Storage.exists(VOCABULARY_FILE_PATH) && !readIndexDoc(VOCABULARY_FILE_PATH, doc)) {
    LOG_ERR("VOC", "Refusing to add '%s': index unreadable", word.c_str());
    return AddResult::Failed;
  }
  // (A missing file leaves `doc` empty, which is the correct starting point.)
  doc["v"] = VOCABULARY_FILE_VERSION;

  JsonArray items = doc["items"].to<JsonArray>();
  for (JsonObject obj : items) {
    if ((obj["w"] | "") == word) {
      return AddResult::AlreadyPresent;
    }
  }

  // The long text goes to its own file first: an index entry that points at a
  // text file that was never written would show an empty card later.
  Storage.mkdir(VOCABULARY_TEXT_DIR);
  const std::string text =
      truncateUtf8(sentence, MAX_SENTENCE_LENGTH) + "\n" + truncateUtf8(definition, MAX_DEFINITION_LENGTH);
  if (!Storage.writeFile(textFilePath(word).c_str(), String(text.c_str()))) {
    LOG_ERR("VOC", "Failed to write vocabulary text for '%s'", word.c_str());
    return AddResult::Failed;
  }

  JsonObject obj = items.add<JsonObject>();
  obj["w"] = word;
  obj["s"] = makeSummary(definition);
  obj["b"] = truncateUtf8(book, MAX_HEADWORD_LENGTH);
  obj["t"] = timestamp;
  // A word that has never been reviewed is a new card: due right away.
  obj["st"] = static_cast<uint8_t>(study::State::New);
  obj["si"] = static_cast<uint8_t>(0);
  obj["sta"] = 0.0f;
  obj["dif"] = 0.0f;
  obj["dd"] = static_cast<int32_t>(0);
  obj["lr"] = static_cast<int32_t>(-1);
  obj["rp"] = static_cast<uint16_t>(0);
  obj["lp"] = static_cast<uint16_t>(0);
  obj["dm"] = static_cast<uint16_t>(0);
  obj["ra"] = static_cast<uint8_t>(0);

  if (!PersistableStoreBase::writeDocToFile(VOCABULARY_FILE_PATH, doc)) {
    LOG_ERR("VOC", "Failed to save vocabulary entry '%s'", word.c_str());
    return AddResult::Failed;
  }
  LOG_DBG("VOC", "Saved vocabulary entry '%s'", word.c_str());
  return AddResult::Added;
}

bool VocabularyFile::remove(const std::string& headword) {
  JsonDocument doc;
  if (!readIndexDoc(VOCABULARY_FILE_PATH, doc)) {
    return false;
  }

  JsonArray items = doc["items"].as<JsonArray>();
  bool found = false;
  for (JsonObject obj : items) {
    if ((obj["w"] | "") == headword) {
      found = true;
      break;
    }
  }
  if (!found) {
    return false;
  }

  // Rebuild the array without the removed word: an ArduinoJson array cannot
  // drop one element in place, and every later index would shift.
  JsonDocument out;
  out["v"] = VOCABULARY_FILE_VERSION;
  JsonArray kept = out["items"].to<JsonArray>();
  for (JsonObject obj : items) {
    if ((obj["w"] | "") == headword) {
      continue;
    }
    JsonObject copy = kept.add<JsonObject>();
    copy["w"] = obj["w"] | "";
    copy["s"] = obj["s"] | "";
    copy["b"] = obj["b"] | "";
    copy["t"] = obj["t"] | static_cast<uint32_t>(0);
    copy["st"] = obj["st"] | static_cast<uint8_t>(0);
    copy["si"] = obj["si"] | static_cast<uint8_t>(0);
    copy["sta"] = obj["sta"] | 0.0f;
    copy["dif"] = obj["dif"] | 0.0f;
    copy["dd"] = obj["dd"] | static_cast<int32_t>(0);
    copy["lr"] = obj["lr"] | static_cast<int32_t>(-1);
    copy["rp"] = obj["rp"] | static_cast<uint16_t>(0);
    copy["lp"] = obj["lp"] | static_cast<uint16_t>(0);
    copy["dm"] = obj["dm"] | static_cast<uint16_t>(0);
    copy["ra"] = obj["ra"] | static_cast<uint8_t>(0);
  }

  if (!PersistableStoreBase::writeDocToFile(VOCABULARY_FILE_PATH, out)) {
    LOG_ERR("VOC", "Failed to save vocabulary after removing '%s'", headword.c_str());
    return false;
  }
  // The long text goes last: an index entry that still exists is a word the
  // user can still open, while a stray text file is only wasted space.
  Storage.remove(textFilePath(headword).c_str());
  LOG_DBG("VOC", "Removed vocabulary entry '%s'", headword.c_str());
  return true;
}

int VocabularyFile::exportAnki(const std::string& path) {
  std::vector<Entry> entries;
  if (!load(entries)) {
    return -1;  // no notebook yet
  }

  const std::string temp = path + ".tmp";
  HalFile out;
  if (!Storage.openFileForWrite("VOC", temp, out)) {
    LOG_ERR("VOC", "Anki export: cannot open %s", temp.c_str());
    return -1;
  }

  // Anki's text import format. `#html:false` keeps the plain-text fields as
  //-is; `#columns` names them so an import profile can map them 1:1.
  const char header[] = "#separator:tab\n#html:false\n#columns:Word\tMeaning\tSentence\tBook\n";
  if (out.write(header, sizeof(header) - 1) != sizeof(header) - 1) {
    out.close();
    Storage.remove(temp.c_str());
    return -1;
  }

  // Flatten control characters that would break the one-card-per-line format.
  const auto flat = [](const std::string& s) {
    std::string r = s;
    for (char& c : r) {
      if (c == '\t' || c == '\r' || c == '\n') c = ' ';
    }
    return r;
  };

  int written = 0;
  bool ok = true;
  for (const auto& entry : entries) {
    std::string sentence;
    std::string definition;
    loadText(entry.headword, sentence, definition);
    // The index excerpt is the fallback when the long text is gone.
    if (definition.empty()) definition = entry.summary;
    const std::string row =
        flat(entry.headword) + "\t" + flat(definition) + "\t" + flat(sentence) + "\t" + flat(entry.book) + "\n";
    if (out.write(row.data(), row.size()) != row.size()) {
      ok = false;
      break;
    }
    ++written;
  }
  out.flush();
  out.close();

  if (!ok || written == 0) {
    Storage.remove(temp.c_str());
    return ok ? 0 : -1;
  }
  // Publish atomically: a half-written deck is never left at `path`.
  if (Storage.exists(path.c_str()) && !Storage.remove(path.c_str())) {
    Storage.remove(temp.c_str());
    return -1;
  }
  if (!Storage.rename(temp.c_str(), path.c_str())) {
    Storage.remove(temp.c_str());
    return -1;
  }
  LOG_DBG("VOC", "Anki export wrote %d cards to %s", written, path.c_str());
  return written;
}

bool VocabularyFile::contains(const std::string& headword) {
  JsonDocument doc;
  if (!readIndexDoc(VOCABULARY_FILE_PATH, doc)) {
    return false;
  }
  JsonArray items = doc["items"].as<JsonArray>();
  for (JsonObject obj : items) {
    if ((obj["w"] | "") == headword) {
      return true;
    }
  }
  return false;
}

bool VocabularyFile::loadText(const std::string& headword, std::string& sentence, std::string& definition) {
  sentence.clear();
  definition.clear();

  std::string text;
  // The cap is a hard limit: a file larger than this is refused outright.
  constexpr size_t TEXT_CAP = MAX_SENTENCE_LENGTH + MAX_DEFINITION_LENGTH + 2;
  if (!Storage.readFileToString("VOC", textFilePath(headword), TEXT_CAP, text)) {
    return false;
  }
  // The file holds the sentence, a newline, then the definition.
  const size_t split = text.find('\n');
  if (split == std::string::npos) {
    definition = text;
    return true;
  }
  sentence = text.substr(0, split);
  definition = text.substr(split + 1);
  return true;
}

bool VocabularyFile::saveCard(const std::string& headword, const study::CardState& card, const uint8_t lastRating) {
  JsonDocument doc;
  if (!readIndexDoc(VOCABULARY_FILE_PATH, doc)) {
    return false;
  }
  JsonArray items = doc["items"].as<JsonArray>();
  bool found = false;
  for (JsonObject obj : items) {
    if ((obj["w"] | "") != headword) {
      continue;
    }
    obj["st"] = card.state;
    obj["si"] = card.stepIndex;
    obj["sta"] = card.stability;
    obj["dif"] = card.difficulty;
    obj["dd"] = card.dueDay;
    obj["lr"] = card.lastReviewDay;
    obj["rp"] = card.reps;
    obj["lp"] = card.lapses;
    obj["dm"] = card.dueMinute;
    obj["ra"] = lastRating;
    found = true;
    break;
  }
  if (!found) {
    return false;
  }
  return PersistableStoreBase::writeDocToFile(VOCABULARY_FILE_PATH, doc);
}

int VocabularyFile::countDue(const std::vector<VocabularyFile::Entry>& entries, const int today, const int nowMinute) {
  int count = 0;
  for (const auto& entry : entries) {
    if (study::Scheduler::isDue(entry.card, today, nowMinute)) {
      ++count;
    }
  }
  return count;
}

void VocabularyFile::collectDue(const std::vector<VocabularyFile::Entry>& entries, std::vector<int>& indices,
                                const int today, const int nowMinute) {
  indices.clear();
  // New cards first, then the cards whose learning step expires soonest: a
  // word that comes back in ten minutes is more urgent than one that comes
  // back tomorrow.
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto& card = entries[i].card;
    if (!study::Scheduler::isDue(card, today, nowMinute)) {
      continue;
    }
    const bool learning = card.state == static_cast<uint8_t>(study::State::Learning) ||
                          card.state == static_cast<uint8_t>(study::State::Relearning);
    const bool isNew = card.state == static_cast<uint8_t>(study::State::New);
    const int64_t dueAt = static_cast<int64_t>(card.dueDay) * 1440 + card.dueMinute;
    const int64_t now = static_cast<int64_t>(today) * 1440 + nowMinute;

    size_t at = indices.size();
    for (size_t n = 0; n < indices.size(); ++n) {
      const auto& other = entries[indices[n]].card;
      const bool otherLearning = other.state == static_cast<uint8_t>(study::State::Learning) ||
                                 other.state == static_cast<uint8_t>(study::State::Relearning);
      const bool otherNew = other.state == static_cast<uint8_t>(study::State::New);
      const int64_t otherDue = static_cast<int64_t>(other.dueDay) * 1440 + other.dueMinute;
      // New before learning before review; then the earlier due instant.
      const bool before = (isNew && !otherNew) || (isNew == otherNew && learning && !otherLearning) ||
                          (isNew == otherNew && learning == otherLearning && dueAt < otherDue);
      if (before) {
        at = n;
        break;
      }
    }
    indices.insert(indices.begin() + at, static_cast<int>(i));
  }
}

int VocabularyFile::currentDayNumber() {
  const uint32_t now = TimeUtils::getCurrentValidTimestamp();
  if (now == 0) {
    return 0;
  }
  return static_cast<int>(TimeUtils::getLocalDayOrdinal(now));
}

int VocabularyFile::currentMinuteOfDay() {
  const uint32_t now = TimeUtils::getCurrentValidTimestamp();
  if (now == 0) {
    return 0;
  }
  std::tm local{};
  if (!TimeUtils::getLocalDateTime(now, local)) {
    return 0;
  }
  return local.tm_hour * 60 + local.tm_min;
}
