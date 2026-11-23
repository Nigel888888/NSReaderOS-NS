#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Read-only ICS agenda data layer for TickTick / Dida365 subscription links.
//
// The device never logs into TickTick. The user pastes a read-only ICS
// subscription URL once on the device; this layer stores that URL in a local
// config file (never in source, i18n, scripts or the repository), downloads
// the calendar over HTTPS and caches parsed events on the SD card so the
// agenda can be viewed offline. Nothing here can create or modify tasks.
//
// On disk (under /.crosspoint/agenda):
//   subscription.txt   the ICS URL, one line, entered locally by the user
//   events.json        [{uid,title,desc,loc,day,time,allDay}] cached events
class AgendaStore {
 public:
  // Longest ICS response kept in RAM (bytes). TickTick calendars stay small;
  // this cap keeps a hostile endpoint from exhausting memory.
  static constexpr size_t MAX_ICS_RESPONSE = 384 * 1024;
  static constexpr size_t MAX_EVENTS = 400;
  static constexpr size_t MAX_TITLE_LENGTH = 160;
  static constexpr size_t MAX_DESC_LENGTH = 1024;
  static constexpr size_t MAX_URL_LENGTH = 512;

  struct Event {
    std::string uid;
    std::string title;
    std::string desc;
    std::string loc;
    uint32_t dayOrdinal = 0;  // local day number for Today/This-week grouping
    std::string timeText;     // "09:00-10:00" or "" for all-day
    bool allDay = false;
  };

  // ---- local subscription config ----------------------------------------
  static std::string configPath();
  static bool loadUrl(std::string& url);
  static bool saveUrl(const std::string& url);
  static bool hasUrl();

  // ---- cached events ----------------------------------------------------
  static std::string cachePath();
  static bool loadEvents(std::vector<Event>& events);
  static bool saveEvents(const std::vector<Event>& events);

  // ---- fetching & parsing ----------------------------------------------
  // Downloads `url`, parses VEVENT blocks and REPLACES the cache with the
  // result. Returns the event count, or -1 on a transport/parse failure.
  // `shouldAbort` is polled during network waits (watchdog + deadline).
  static int fetch(const std::string& url, std::vector<Event>& events,
                   const std::function<bool()>& shouldAbort);

  // Parses one ICS document already in memory. Exposed for host tests.
  static std::vector<Event> parseIcs(const std::string& text);

 private:
  static void ensureDir();
};

// Local-day helpers shared with the activity, expressed as free functions so
// they can be unit-tested without the renderer.
namespace agendaTime {
// Monday-based week start (day ordinal) for the given local day ordinal.
uint32_t weekStart(uint32_t dayOrdinal);
// True when `day` falls in the week (Mon..Sun) containing `today`.
bool isThisWeek(uint32_t day, uint32_t today);
// Parse "YYYYMMDD" (and optional "THHMMSS") into a local day ordinal and a
// display time string. Returns false for malformed dates.
bool parseIcsDate(const std::string& value, uint32_t& dayOrdinal, std::string& timeText, bool& allDay);
}  // namespace agendaTime
