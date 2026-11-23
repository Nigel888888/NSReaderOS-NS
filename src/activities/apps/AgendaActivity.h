#pragma once

#include <I18n.h>

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/Rect.h"
#include "util/AgendaStore.h"

// Read-only TickTick / Dida365 agenda, backed by a user-supplied ICS
// subscription URL that lives ONLY in a local config file on the device.
// The app never stores a credential in source, logs or the repository and
// never writes back to the calendar.
//
//   Today    : events whose local day == today
//   This week: events in the Monday..Sunday week containing today
//   All      : every cached event, oldest first
//   Detail   : one event's full text (no tabs / bar)
//
// Networking lives in loop() with a Phase state machine so the render task can
// paint the busy state and the loop task can feed its watchdog; draws happen in
// render() under RenderLock. The layout mirrors the RSS reader for consistency.
class AgendaActivity final : public Activity {
 public:
  AgendaActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~AgendaActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class View : uint8_t { Today, Week, All, Detail };
  enum class Phase : uint8_t { Idle, FetchRequested, Fetching };

  struct Row {
    std::string title;
    std::string subtitle;
    std::string date;
    int eventIndex = -1;  // index into `events`
  };

  void loadConfig();
  void rebuildRows();
  void drawHeaderAndTabs();
  void drawTabs();
  void drawBottomBar();
  void drawEventList();
  void drawDetail();
  void drawMessage();

  int listTop() const;
  int visibleRows() const;
  int listFirstIndex() const;
  void clampScroll();
  void ensureCursorVisible();
  void drawCardRow(const Rect& r, const Row& row, bool selected);

  void selectTab(View target);
  void openDetail(int index);
  void rememberCursor();
  void saveState();
  void loadState();
  void showMessage(const std::string& text);

  void refresh();
  void beginFetch();
  bool networkShouldAbort(unsigned long started) const;
  void setSubscription();

  View view = View::Today;
  Phase phase = Phase::Idle;
  int selected = 0;
  int scrollTop = 0;
  int todaySelected = 0, todayScroll = 0;
  int weekSelected = 0, weekScroll = 0;
  int allSelected = 0, allScroll = 0;
  int armedIndex = -1;

  bool hasSubscription = false;
  std::string subscriptionUrl;  // in-memory only, never logged or serialized to source
  uint32_t todayOrdinal = 0;

  std::vector<AgendaStore::Event> events;
  std::vector<Row> rows;

  int detailIndex = -1;
  std::vector<std::string> detailLines;
  int detailLine = 0;
  int detailPageLines = 1;

  Rect tabRects[3]{};
  Rect refreshButton{0, 0, 0, 0};
  Rect configButton{0, 0, 0, 0};
  Rect backButton{0, 0, 0, 0};
  Rect listBounds{0, 0, 0, 0};
  int rowHeight = 0;
  int rowGap = 0;

  std::string message;
  unsigned long messageUntil = 0;
};

// Formats a local day ordinal as "MM-DD" for the date pill.
std::string agendaDateLabel(uint32_t dayOrdinal);
