#include "AgendaStore.h"

#ifdef RICKYOS_PRODUCT

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "PluginHttp.h"
#include "TimeUtils.h"

namespace {

constexpr const char* AGENDA_DIR = "/.crosspoint/agenda";

void ensureAgendaDir() {
  if (!Storage.exists(AGENDA_DIR)) Storage.mkdir(AGENDA_DIR);
}

std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

std::string truncateUtf8(const std::string& text, const size_t limit) {
  if (text.size() <= limit) return text;
  size_t end = limit;
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
  return text.substr(0, end);
}

// Days since 1970-01-01 from a civil date (Howard Hinnant's algorithm).
uint32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= (m <= 2) ? 1 : 0;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned mp = (m > 2) ? (m - 3) : (m + 9);  // Mar=0 .. Feb=11
  const unsigned doy = (153 * mp + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<uint32_t>(era * 146097 + static_cast<int>(doe) - 719468);
}

// Unescape ICS TEXT values: \n / \N -> newline, \, \; \\ -> literal.
std::string unescapeIcs(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '\\' && i + 1 < in.size()) {
      const char n = in[++i];
      if (n == 'n' || n == 'N') out.push_back('\n');
      else out.push_back(n);
    } else {
      out.push_back(in[i]);
    }
  }
  return out;
}

// "20261124" or "20261124T093000" or "20261124T093000Z" -> parts.
bool splitIcsDateTime(const std::string& raw, int& y, unsigned& mo, unsigned& d, unsigned& h, unsigned& mi,
                      bool& hasTime, bool& utc) {
  const std::string s = trim(raw);
  if (s.size() < 8) return false;
  auto digits = [&s](size_t at, size_t n) -> int {
    int v = 0;
    for (size_t i = 0; i < n; ++i) {
      if (at + i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[at + i]))) return -1;
      v = v * 10 + (s[at + i] - '0');
    }
    return v;
  };
  y = digits(0, 4);
  mo = static_cast<unsigned>(digits(4, 2));
  d = static_cast<unsigned>(digits(6, 2));
  if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31) return false;
  hasTime = s.size() >= 15 && s[8] == 'T';
  h = hasTime ? static_cast<unsigned>(digits(9, 2)) : 0;
  mi = hasTime ? static_cast<unsigned>(digits(11, 2)) : 0;
  if (hasTime && (h > 23 || mi > 59)) return false;
  utc = !s.empty() && s.back() == 'Z';
  return true;
}

std::string twoDigits(unsigned v) {
  char b[4];
  std::snprintf(b, sizeof(b), "%02u", v);
  return b;
}

}  // namespace

namespace agendaTime {

bool parseIcsDate(const std::string& value, uint32_t& dayOrdinal, std::string& timeText, bool& allDay) {
  int y = 0;
  unsigned mo = 0, d = 0, h = 0, mi = 0;
  bool hasTime = false, utc = false;
  if (!splitIcsDateTime(value, y, mo, d, h, mi, hasTime, utc)) return false;
  allDay = !hasTime;
  if (utc && hasTime) {
    // UTC wall time -> device-local day/time.
    const uint32_t epoch = daysFromCivil(y, mo, d) * 86400UL + h * 3600UL + mi * 60UL;
    dayOrdinal = TimeUtils::getLocalDayOrdinal(epoch);
    std::tm local{};
    if (TimeUtils::getLocalDateTime(epoch, local)) {
      h = static_cast<unsigned>(local.tm_hour);
      mi = static_cast<unsigned>(local.tm_min);
    }
  } else {
    // TZID / floating wall time: the ICS already carries the user's local
    // clock (TickTick emits TZID=Asia/Shanghai for China accounts), so treat
    // the civil date as a local day directly.
    dayOrdinal = TimeUtils::getDayOrdinalForDate(y, static_cast<int>(mo), d);
  }
  timeText = hasTime ? (twoDigits(h) + ":" + twoDigits(mi)) : std::string();
  return dayOrdinal != 0;
}

uint32_t weekStart(uint32_t dayOrdinal) {
  // Day 0 (1970-01-01) was a Thursday. (day + 3) % 7: Mon=0 .. Sun=6.
  const uint32_t weekday = (dayOrdinal + 3) % 7;
  return dayOrdinal - weekday;
}

bool isThisWeek(uint32_t day, uint32_t today) {
  if (day == 0 || today == 0) return false;
  const uint32_t start = weekStart(today);
  return day >= start && day < start + 7;
}

}  // namespace agendaTime

// ---- local subscription config -----------------------------------------

std::string AgendaStore::configPath() { return std::string(AGENDA_DIR) + "/subscription.txt"; }
std::string AgendaStore::cachePath() { return std::string(AGENDA_DIR) + "/events.json"; }

void AgendaStore::ensureDir() { ensureAgendaDir(); }

bool AgendaStore::loadUrl(std::string& url) {
  url.clear();
  std::string text;
  if (!Storage.readFileToString("AGENDA", configPath().c_str(), MAX_URL_LENGTH + 1, text)) return false;
  url = trim(text);
  return !url.empty();
}

bool AgendaStore::saveUrl(const std::string& url) {
  ensureDir();
  if (url.size() > MAX_URL_LENGTH) return false;
  return Storage.writeFile(configPath().c_str(), String(url.c_str()));
}

bool AgendaStore::hasUrl() {
  std::string url;
  return loadUrl(url);
}

// ---- cached events ------------------------------------------------------

bool AgendaStore::loadEvents(std::vector<Event>& events) {
  events.clear();
  std::string json;
  if (!Storage.readFileToString("AGENDA", cachePath().c_str(), 512 * 1024, json) || json.empty()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  JsonArray arr = doc.as<JsonArray>();
  if (arr.isNull()) return false;
  for (JsonVariant v : arr) {
    Event e;
    e.uid = v["uid"] | "";
    e.title = v["title"] | "";
    e.desc = v["desc"] | "";
    e.loc = v["loc"] | "";
    e.dayOrdinal = static_cast<uint32_t>(v["day"] | 0);
    e.timeText = v["time"] | "";
    e.allDay = static_cast<bool>(v["allDay"] | false);
    if (e.title.empty() || e.dayOrdinal == 0) continue;
    events.push_back(std::move(e));
    if (events.size() >= MAX_EVENTS) break;
  }
  return true;
}

bool AgendaStore::saveEvents(const std::vector<Event>& events) {
  ensureDir();
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (const Event& e : events) {
    JsonObject o = arr.add<JsonObject>();
    o["uid"] = e.uid;
    o["title"] = e.title;
    o["desc"] = e.desc;
    o["loc"] = e.loc;
    o["day"] = e.dayOrdinal;
    o["time"] = e.timeText;
    o["allDay"] = e.allDay;
  }
  String out;
  serializeJson(doc, out);
  return Storage.writeFile(cachePath().c_str(), out);
}

// ---- ICS parsing --------------------------------------------------------

std::vector<AgendaStore::Event> AgendaStore::parseIcs(const std::string& text) {
  // Unfold folded lines (continuation lines begin with a space or tab).
  std::vector<std::string> lines;
  std::string current;
  for (size_t i = 0; i < text.size();) {
    const size_t nl = text.find('\n', i);
    std::string line = text.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!lines.empty() && !line.empty() && (line[0] == ' ' || line[0] == '\t')) {
      lines.back() += line.substr(1);
    } else {
      lines.push_back(std::move(line));
    }
    if (nl == std::string::npos) break;
    i = nl + 1;
  }

  std::vector<Event> events;
  bool inEvent = false;
  bool cancelled = false;
  std::string uid, summary, desc, loc, dtstart, dtend;

  auto reset = [&]() {
    inEvent = false;
    cancelled = false;
    uid.clear(); summary.clear(); desc.clear(); loc.clear(); dtstart.clear(); dtend.clear();
  };

  auto emit = [&]() {
    if (cancelled || summary.empty()) return;
    Event e;
    uint32_t startDay = 0, endDay = 0;
    std::string startTime, endTime;
    bool allDay = false, ignored = false;
    if (!agendaTime::parseIcsDate(dtstart, startDay, startTime, allDay) || startDay == 0) return;
    e.dayOrdinal = startDay;
    e.allDay = allDay;
    e.title = truncateUtf8(unescapeIcs(summary), MAX_TITLE_LENGTH);
    e.desc = truncateUtf8(unescapeIcs(desc), MAX_DESC_LENGTH);
    e.loc = truncateUtf8(unescapeIcs(loc), 120);
    e.uid = truncateUtf8(uid.empty() ? (summary + "|" + dtstart) : uid, 200);
    if (!allDay && !dtend.empty() &&
        agendaTime::parseIcsDate(dtend, endDay, endTime, ignored) && endDay == startDay && !endTime.empty()) {
      e.timeText = startTime + "-" + endTime;
    } else {
      e.timeText = startTime;
    }
    // Collapse whitespace in one-line fields for tidy cards.
    for (std::string* field : {&e.title, &e.loc}) {
      std::string out;
      bool space = false;
      for (const char c : *field) {
        if (c == '\r' || c == '\n') { space = true; continue; }
        if (std::isspace(static_cast<unsigned char>(c))) { space = true; continue; }
        if (space && !out.empty()) out.push_back(' ');
        space = false;
        out.push_back(c);
      }
      *field = std::move(out);
    }
    events.push_back(std::move(e));
  };

  for (const std::string& line : lines) {
    if (line.compare(0, 13, "BEGIN:VEVENT") == 0) { reset(); inEvent = true; continue; }
    if (line.compare(0, 11, "END:VEVENT") == 0) { if (inEvent) emit(); reset(); continue; }
    if (!inEvent) continue;
    const size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string prop = line.substr(0, colon);
    const std::string value = line.substr(colon + 1);
    const size_t semi = prop.find(';');
    if (semi != std::string::npos) prop = prop.substr(0, semi);
    if (prop == "UID") uid = value;
    else if (prop == "SUMMARY") summary = value;
    else if (prop == "DESCRIPTION") desc = value;
    else if (prop == "LOCATION") loc = value;
    else if (prop == "DTSTART") dtstart = value;
    else if (prop == "DTEND") dtend = value;
    else if (prop == "STATUS" && value == "CANCELLED") cancelled = true;
  }

  // De-duplicate by uid, keep insertion order, then sort by day + time.
  std::vector<Event> unique;
  unique.reserve(events.size());
  size_t seen = 0;
  for (Event& e : events) {
    bool dup = false;
    for (const Event& u : unique) if (u.uid == e.uid) { dup = true; break; }
    if (!dup && seen++ < MAX_EVENTS) unique.push_back(std::move(e));
  }
  std::sort(unique.begin(), unique.end(), [](const Event& a, const Event& b) {
    if (a.dayOrdinal != b.dayOrdinal) return a.dayOrdinal < b.dayOrdinal;
    if (a.allDay != b.allDay) return a.allDay;  // all-day first
    return a.timeText < b.timeText;
  });
  return unique;
}

// ---- fetch --------------------------------------------------------------

int AgendaStore::fetch(const std::string& url, std::vector<Event>& events,
                       const std::function<bool()>& shouldAbort) {
  String body;
  const int status = pluginhttp::request(nullptr, url, "GET", "", {}, body, MAX_ICS_RESPONSE, nullptr, shouldAbort);
  if (status != 200) return -1;
  const std::string text(body.c_str(), body.length());
  if (text.find("BEGIN:VCALENDAR") == std::string::npos) return -1;
  std::vector<Event> parsed = parseIcs(text);
  if (parsed.empty()) return -1;
  if (!saveEvents(parsed)) return -1;
  events = std::move(parsed);
  return static_cast<int>(events.size());
}

#endif  // RICKYOS_PRODUCT
