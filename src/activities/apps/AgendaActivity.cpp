#include "AgendaActivity.h"

#include <Arduino.h>
#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <variant>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TaskWatchdog.h"
#include "util/TimeUtils.h"

namespace {

constexpr int SIDE_PAD = 16;
constexpr int TAB_H = 56;
constexpr int TAB_TOP = 6;
constexpr int TAB_GAP = 8;
constexpr int BAR_H = 56;
constexpr int BAR_GAP = 12;
constexpr int BAR_TOP = 8;
constexpr int CARD_GAP = 10;
constexpr int CARD_RADIUS = 8;
constexpr unsigned long MESSAGE_MS = 3000;
constexpr unsigned long NETWORK_DEADLINE_MS = 60000;
constexpr const char* UI_STATE_PATH = "/.crosspoint/agenda/ui-state.txt";

// Process-wide cache: the event index is small but re-reading it on every
// entry still added noticeable latency. Keep it for the firmware lifetime.
std::vector<AgendaStore::Event> gEventsCache;
bool gEventsCached = false;

bool hit(const Rect& r, int x, int y) {
  return r.width > 0 && r.height > 0 && x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

Rect rowRectFor(int index, const Rect& bounds, int rowHeight, int rowGap) {
  return Rect{bounds.x, bounds.y + index * (rowHeight + rowGap), bounds.width, rowHeight};
}

}  // namespace

std::string agendaDateLabel(uint32_t dayOrdinal) {
  int y = 0;
  unsigned m = 0, d = 0;
  if (!TimeUtils::getDateFromDayOrdinal(dayOrdinal, y, m, d)) return std::string();
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02u-%02u", m, d);
  return std::string(buf);
}

AgendaActivity::AgendaActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Agenda", renderer, mappedInput) {}

AgendaActivity::~AgendaActivity() = default;

void AgendaActivity::loadConfig() {
  subscriptionUrl.clear();
  hasSubscription = AgendaStore::loadUrl(subscriptionUrl);
}

void AgendaActivity::rememberCursor() {
  if (view == View::Detail) return;
  if (view == View::Today) { todaySelected = selected; todayScroll = scrollTop; }
  else if (view == View::Week) { weekSelected = selected; weekScroll = scrollTop; }
  else { allSelected = selected; allScroll = scrollTop; }
}

void AgendaActivity::saveState() {
  rememberCursor();
  if (!Storage.exists("/.crosspoint/agenda")) Storage.mkdir("/.crosspoint/agenda");
  const View persisted = view == View::Detail ? View::Today : view;
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "view=%d\ntoday=%d\ntodayScroll=%d\nweek=%d\nweekScroll=%d\nall=%d\nallScroll=%d\n",
                static_cast<int>(persisted), todaySelected, todayScroll, weekSelected, weekScroll, allSelected,
                allScroll);
  Storage.writeFile(UI_STATE_PATH, String(buf));
}

void AgendaActivity::loadState() {
  std::string text;
  if (!Storage.readFileToString("AGENDA", UI_STATE_PATH, 1024, text)) return;
  const auto readInt = [&text](const char* key, const int fallback) {
    const char* p = std::strstr(text.c_str(), key);
    return p ? std::atoi(p + std::strlen(key)) : fallback;
  };
  const int v = std::clamp(readInt("view=", 0), 0, static_cast<int>(View::All));
  view = static_cast<View>(v);
  todaySelected = std::max(0, readInt("today=", 0));
  todayScroll = std::max(0, readInt("todayScroll=", 0));
  weekSelected = std::max(0, readInt("week=", 0));
  weekScroll = std::max(0, readInt("weekScroll=", 0));
  allSelected = std::max(0, readInt("all=", 0));
  allScroll = std::max(0, readInt("allScroll=", 0));
}

void AgendaActivity::onEnter() {
  Activity::onEnter();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  phase = Phase::Idle;
  message.clear();
  messageUntil = 0;
  armedIndex = -1;
  detailIndex = -1;
  detailLine = 0;
  loadState();
  loadConfig();
  if (!gEventsCached) {
    AgendaStore::loadEvents(gEventsCache);
    gEventsCached = true;
  }
  events = gEventsCache;
  const uint32_t now = TimeUtils::getCurrentValidTimestamp();
  todayOrdinal = TimeUtils::getLocalDayOrdinal(now);
  if (view == View::Today) { selected = todaySelected; scrollTop = todayScroll; }
  else if (view == View::Week) { selected = weekSelected; scrollTop = weekScroll; }
  else { selected = allSelected; scrollTop = allScroll; }
  rebuildRows();
  if (selected >= static_cast<int>(rows.size())) selected = rows.empty() ? 0 : static_cast<int>(rows.size()) - 1;
  requestUpdate();
}

void AgendaActivity::onExit() {
  saveState();
  Activity::onExit();
}

void AgendaActivity::showMessage(const std::string& text) {
  message = text;
  messageUntil = millis() + MESSAGE_MS;
  requestUpdate();
}

void AgendaActivity::rebuildRows() {
  rows.clear();
  rows.reserve(events.size());
  for (size_t i = 0; i < events.size(); ++i) {
    const AgendaStore::Event& e = events[i];
    const bool today = e.dayOrdinal == todayOrdinal;
    const bool week = agendaTime::isThisWeek(e.dayOrdinal, todayOrdinal);
    if (view == View::Today && !today) continue;
    if (view == View::Week && !week) continue;
    Row r;
    r.title = e.title;
    r.subtitle = e.allDay ? std::string(tr(STR_AGENDA_ALL_DAY)) : e.timeText;
    if (!e.loc.empty()) r.subtitle += "  ·  " + e.loc;
    r.date = agendaDateLabel(e.dayOrdinal);
    r.eventIndex = static_cast<int>(i);
    rows.push_back(std::move(r));
  }
}

void AgendaActivity::selectTab(View target) {
  if (view == target) return;
  rememberCursor();
  view = target;
  if (view == View::Today) { selected = todaySelected; scrollTop = todayScroll; }
  else if (view == View::Week) { selected = weekSelected; scrollTop = weekScroll; }
  else { selected = allSelected; scrollTop = allScroll; }
  rebuildRows();
  clampScroll();
  ensureCursorVisible();
  armedIndex = -1;
  saveState();
  requestUpdate();
}

bool AgendaActivity::networkShouldAbort(const unsigned long started) const {
  resetTaskWatchdogIfSubscribed();
  return millis() - started >= NETWORK_DEADLINE_MS;
}

void AgendaActivity::refresh() {
  if (!hasSubscription || subscriptionUrl.empty()) {
    showMessage(tr(STR_AGENDA_NO_SUBSCRIPTION));
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    showMessage(tr(STR_AGENDA_NO_WIFI));
    startActivityForResultWith<WifiSelectionActivity>(
        [this](const ActivityResult& result) {
          const auto* wifiResult = std::get_if<WifiResult>(&result.data);
          if (!result.isCancelled && wifiResult && wifiResult->connected) beginFetch();
        },
        true);
    return;
  }
  beginFetch();
}

void AgendaActivity::beginFetch() {
  phase = Phase::Fetching;
  requestUpdate();
}

void AgendaActivity::setSubscription() {
  startActivityForResultWith<KeyboardEntryActivity>(
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        const auto* entry = std::get_if<KeyboardResult>(&result.data);
        if (!entry || entry->text.empty()) return;
        // Accept only http(s) ICS endpoints. The value is stored solely in a
        // local SD-card config file; never logged, never sent anywhere else.
        const std::string url = entry->text;
        if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0) {
          showMessage(tr(STR_AGENDA_URL_INVALID));
          return;
        }
        if (!AgendaStore::saveUrl(url)) {
          showMessage(tr(STR_AGENDA_URL_INVALID));
          return;
        }
        subscriptionUrl = url;
        hasSubscription = true;
        showMessage(tr(STR_AGENDA_URL_SAVED));
      },
      tr(STR_AGENDA_URL_PROMPT), std::string("https://"), AgendaStore::MAX_URL_LENGTH);
}

void AgendaActivity::openDetail(int index) {
  if (index < 0 || index >= static_cast<int>(rows.size())) return;
  const int eventIndex = rows[index].eventIndex;
  if (eventIndex < 0 || eventIndex >= static_cast<int>(events.size())) return;
  detailIndex = eventIndex;
  const AgendaStore::Event& e = events[eventIndex];

  std::string text = e.title;
  text += "\n";
  text += agendaDateLabel(e.dayOrdinal) + "  ";
  text += e.allDay ? std::string(tr(STR_AGENDA_ALL_DAY)) : e.timeText;
  if (!e.loc.empty()) text += "\n" + e.loc;
  if (!e.desc.empty()) text += "\n\n" + e.desc;

  // Greedy UTF-8-aware wrap using the renderer, bounded to keep RAM modest.
  detailLines.clear();
  const int font = NOTOSANS_18_FONT_ID;
  const int maxWidth = renderer.getScreenWidth() - 2 * SIDE_PAD;
  constexpr size_t MAX_LINES = 400;
  size_t pos = 0;
  while (pos < text.size() && detailLines.size() < MAX_LINES) {
    const size_t nl = text.find('\n', pos);
    const std::string segment = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    std::string line;
    size_t i = 0;
    while (i < segment.size() && detailLines.size() < MAX_LINES) {
      size_t next = i;
      while (next < segment.size()) {
        size_t step = 1;
        const unsigned char c = static_cast<unsigned char>(segment[next]);
        if ((c & 0xE0) == 0xC0) step = 2;
        else if ((c & 0xF0) == 0xE0) step = 3;
        else if ((c & 0xF8) == 0xF0) step = 4;
        if (next + step > segment.size()) step = 1;
        const std::string candidate = line + segment.substr(next, step);
        if (!line.empty() && renderer.getTextWidth(font, candidate.c_str()) > maxWidth) break;
        line = candidate;
        next += step;
      }
      if (next == i) next = i + 1;
      detailLines.push_back(line);
      line.clear();
      i = next;
    }
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
  if (detailLines.empty()) detailLines.push_back("");
  detailLine = 0;
  view = View::Detail;
  requestUpdate();
}

void AgendaActivity::loop() {
  if (!message.empty() && static_cast<long>(millis() - messageUntil) >= 0) {
    message.clear();
    requestUpdate();
  }

  if (phase == Phase::Fetching) {
    resetTaskWatchdogIfSubscribed();
    const unsigned long started = millis();
    const auto keepAlive = [this, started]() { return networkShouldAbort(started); };
    std::vector<AgendaStore::Event> fetched;
    const int count = AgendaStore::fetch(subscriptionUrl, fetched, keepAlive);
    resetTaskWatchdogIfSubscribed();
    if (count >= 0) {
      events = std::move(fetched);
      gEventsCache = events;
      gEventsCached = true;
      rebuildRows();
      clampScroll();
      ensureCursorVisible();
      char msg[64];
      std::snprintf(msg, sizeof(msg), tr(STR_AGENDA_UPDATE_DONE), count);
      showMessage(msg);
    } else {
      showMessage(tr(STR_AGENDA_UPDATE_FAILED));
    }
    phase = Phase::Idle;
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (view == View::Detail) {
      view = View::Today;
      rebuildRows();
      selected = todaySelected;
      scrollTop = todayScroll;
      clampScroll();
      ensureCursorVisible();
      requestUpdate();
    } else {
      activityManager.goToApps();
    }
    return;
  }

  int tx = 0, ty = 0;
  const bool tapped = mappedInput.wasScreenTapped(tx, ty);
  if (phase == Phase::Idle && tapped) {
    if (view != View::Detail) {
      if (hit(tabRects[0], tx, ty)) { selectTab(View::Today); return; }
      if (hit(tabRects[1], tx, ty)) { selectTab(View::Week); return; }
      if (hit(tabRects[2], tx, ty)) { selectTab(View::All); return; }
      if (hit(refreshButton, tx, ty)) { refresh(); return; }
      if (hit(configButton, tx, ty)) { setSubscription(); return; }
    }

    if (view == View::Detail) {
      if (hit(backButton, tx, ty)) {
        view = View::Today;
        rebuildRows();
        selected = todaySelected;
        scrollTop = todayScroll;
        clampScroll();
        ensureCursorVisible();
        requestUpdate();
        return;
      }
      const int mid = listBounds.y + listBounds.height / 2;
      const int step = std::max(1, detailPageLines - 1);
      int next = detailLine + (ty >= mid ? step : -step);
      next = std::clamp(next, 0, static_cast<int>(detailLines.size()) - 1);
      if (next != detailLine) { detailLine = next; requestUpdate(); }
      return;
    }

    if (hit(listBounds, tx, ty) && rowHeight + rowGap > 0) {
      int idx = listFirstIndex() + (ty - listBounds.y) / (rowHeight + rowGap);
      if (idx >= 0 && idx < static_cast<int>(rows.size())) {
        selected = idx;
        rememberCursor();
        if (armedIndex != idx) {
          armedIndex = idx;
          saveState();
          requestUpdate();
          return;
        }
        armedIndex = -1;
        saveState();
        openDetail(idx);
      }
      return;
    }
  }

  if (phase == Phase::Idle && view != View::Detail && rowHeight + rowGap > 0) {
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
      const int perScreen = visibleRows();
      const int maxScroll = std::max(0, static_cast<int>(rows.size()) - perScreen);
      int next = scrollTop + (swipe == MappedInputManager::SwipeDir::Up ? perScreen : -perScreen);
      next = std::clamp(next, 0, maxScroll);
      if (next != scrollTop) {
        scrollTop = next;
        selected = scrollTop;
        armedIndex = -1;
        rememberCursor();
        saveState();
        requestUpdate();
      }
      return;
    }
  }

  if (phase == Phase::Idle && view != View::Detail) {
    int next = selected;
    if (mappedInput.wasReleased(MappedInputManager::Button::ScreenDown)) next = selected + 1;
    else if (mappedInput.wasReleased(MappedInputManager::Button::ScreenUp)) next = selected - 1;
    else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selected >= 0 && selected < static_cast<int>(rows.size())) {
        if (armedIndex == selected) { armedIndex = -1; openDetail(selected); }
        else { armedIndex = selected; requestUpdate(); }
      }
      return;
    }
    const int count = static_cast<int>(rows.size());
    next = std::clamp(next, 0, count - 1);
    if (next != selected && count > 0) {
      selected = next;
      ensureCursorVisible();
      armedIndex = -1;
      rememberCursor();
      saveState();
      requestUpdate();
    }
  }
}

int AgendaActivity::listTop() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return metrics.topPadding + metrics.headerHeight + TAB_TOP + TAB_H + metrics.verticalSpacing;
}

int AgendaActivity::visibleRows() const {
  if (rowHeight + rowGap <= 0) return 1;
  return std::max(1, listBounds.height / (rowHeight + rowGap));
}

void AgendaActivity::clampScroll() {
  const int maxScroll = std::max(0, static_cast<int>(rows.size()) - visibleRows());
  scrollTop = std::clamp(scrollTop, 0, maxScroll);
}

void AgendaActivity::ensureCursorVisible() {
  clampScroll();
  if (rows.empty()) { selected = 0; scrollTop = 0; return; }
  if (selected < scrollTop) scrollTop = selected;
  else if (selected >= scrollTop + visibleRows()) scrollTop = selected - visibleRows() + 1;
  clampScroll();
}

int AgendaActivity::listFirstIndex() const {
  if (rows.empty()) return 0;
  const int maxScroll = std::max(0, static_cast<int>(rows.size()) - visibleRows());
  return std::clamp(scrollTop, 0, maxScroll);
}

void AgendaActivity::drawCardRow(const Rect& r, const Row& row, bool selectedRow) {
  if (selectedRow) {
    renderer.fillRoundedRect(r.x, r.y, r.width, r.height, CARD_RADIUS, Color::LightGray);
    renderer.fillRect(r.x, r.y + 6, 8, r.height - 12);
  } else {
    renderer.fillRoundedRect(r.x, r.y, r.width, r.height, CARD_RADIUS, Color::White);
  }
  renderer.drawRoundedRect(r.x, r.y, r.width, r.height, 1, CARD_RADIUS, true);
  const int titleLine = renderer.getLineHeight(NOTOSANS_18_FONT_ID);
  const int subLine = renderer.getLineHeight(NOTOSANS_16_FONT_ID);
  const int innerX = r.x + 14;
  const int dateW = row.date.empty() ? 0 : renderer.getTextWidth(UI_10_FONT_ID, row.date.c_str()) + 16;
  const int maxTitleW = r.width - 24 - dateW - 8;
  std::string title = row.title;
  while (!title.empty() && renderer.getTextWidth(NOTOSANS_18_FONT_ID, title.c_str(), EpdFontFamily::BOLD) > maxTitleW) {
    size_t n = title.size() - 1;
    while (n > 0 && (static_cast<unsigned char>(title[n]) & 0xC0) == 0x80) --n;
    title.erase(n);
  }
  const int textTop = r.y + (r.height - titleLine - subLine) / 2;
  renderer.drawText(NOTOSANS_18_FONT_ID, innerX, textTop, title.c_str(), true, EpdFontFamily::BOLD);
  renderer.drawText(NOTOSANS_16_FONT_ID, innerX, textTop + titleLine, row.subtitle.c_str(), true);
  if (!row.date.empty() && dateW > 0) {
    const Rect pill{r.x + r.width - dateW - 10, textTop + 2, dateW, subLine + 4};
    renderer.fillRoundedRect(pill.x, pill.y, pill.width, pill.height, pill.height / 2,
                             selectedRow ? Color::White : Color::LightGray);
    renderer.drawText(UI_10_FONT_ID, pill.x + 8, pill.y + 2, row.date.c_str(), true);
  }
}

void AgendaActivity::drawHeaderAndTabs() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{SIDE_PAD, metrics.topPadding, renderer.getScreenWidth() - 2 * SIDE_PAD,
                                metrics.headerHeight},
                 tr(STR_AGENDA_TITLE));
}

void AgendaActivity::drawTabs() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int y = metrics.topPadding + metrics.headerHeight + TAB_TOP;
  const int width = renderer.getScreenWidth() - 2 * SIDE_PAD;
  const int segW = (width - TAB_GAP * 2) / 3;
  const char* labels[3] = {tr(STR_AGENDA_TODAY), tr(STR_AGENDA_WEEK), tr(STR_AGENDA_ALL)};
  const View views[3] = {View::Today, View::Week, View::All};
  renderer.fillRoundedRect(SIDE_PAD, y, width, TAB_H, TAB_H / 2, Color::LightGray);
  for (int i = 0; i < 3; ++i) {
    tabRects[i] = Rect{SIDE_PAD + i * (segW + TAB_GAP), y, segW, TAB_H};
    const bool active = view == views[i];
    if (active) {
      renderer.fillRoundedRect(tabRects[i].x, tabRects[i].y, tabRects[i].width, tabRects[i].height, TAB_H / 2,
                               Color::DarkGray);
    }
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, labels[i], EpdFontFamily::BOLD);
    const int textX = tabRects[i].x + (segW - textWidth) / 2;
    const int textY = tabRects[i].y + (TAB_H - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawText(UI_10_FONT_ID, textX, textY, labels[i], !active, EpdFontFamily::BOLD);
  }
}

void AgendaActivity::drawBottomBar() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int barY = renderer.getScreenHeight() - metrics.buttonHintsHeight - BAR_H;
  const int width = renderer.getScreenWidth() - 2 * SIDE_PAD;
  const int btnW = (width - BAR_GAP) / 2;
  refreshButton = Rect{SIDE_PAD, barY, btnW, BAR_H};
  configButton = Rect{SIDE_PAD + btnW + BAR_GAP, barY, btnW, BAR_H};
  GUI.drawActionButton(renderer, refreshButton, tr(STR_AGENDA_REFRESH), false);
  GUI.drawActionButton(renderer, configButton, tr(STR_AGENDA_SUBSCRIBE), false);
}

void AgendaActivity::drawEventList() {
  drawHeaderAndTabs();
  drawTabs();
  drawBottomBar();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int top = listTop();
  const int barY = renderer.getScreenHeight() - metrics.buttonHintsHeight - BAR_H;
  listBounds = Rect{SIDE_PAD, top, renderer.getScreenWidth() - 2 * SIDE_PAD, barY - CARD_GAP - top};
  rowHeight = renderer.getLineHeight(NOTOSANS_18_FONT_ID) + renderer.getLineHeight(NOTOSANS_16_FONT_ID) + 28;
  rowGap = CARD_GAP;

  const char* empty = nullptr;
  if (!hasSubscription) empty = tr(STR_AGENDA_NO_SUBSCRIPTION);
  else if (rows.empty()) {
    empty = view == View::Today ? tr(STR_AGENDA_NO_TODAY)
           : view == View::Week ? tr(STR_AGENDA_NO_WEEK)
           : tr(STR_AGENDA_NO_EVENTS);
  }
  if (empty) {
    renderer.drawText(NOTOSANS_16_FONT_ID, SIDE_PAD, listBounds.y + 20, empty);
    return;
  }
  const int first = listFirstIndex();
  const int visible = std::max(1, listBounds.height / (rowHeight + rowGap));
  const int last = std::min(static_cast<int>(rows.size()), first + visible + 1);
  for (int i = first; i < last; ++i) {
    drawCardRow(rowRectFor(i - first, listBounds, rowHeight, rowGap), rows[i], i == selected);
  }
}

void AgendaActivity::drawDetail() {
  drawHeaderAndTabs();
  const auto& metrics = UITheme::getInstance().getMetrics();
  backButton = Rect{SIDE_PAD, metrics.topPadding + 4, 120, metrics.headerHeight - 8};
  GUI.drawActionButton(renderer, backButton, tr(STR_AGENDA_BACK), false);
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  listBounds = Rect{SIDE_PAD, top, renderer.getScreenWidth() - 2 * SIDE_PAD,
                    renderer.getScreenHeight() - top - metrics.buttonHintsHeight - metrics.verticalSpacing};
  const int lineHeight = renderer.getLineHeight(NOTOSANS_18_FONT_ID);
  const int linesOnScreen = std::max(1, listBounds.height / lineHeight);
  detailPageLines = linesOnScreen;
  for (int i = 0; i < linesOnScreen && detailLine + i < static_cast<int>(detailLines.size()); ++i) {
    renderer.drawText(NOTOSANS_18_FONT_ID, listBounds.x, listBounds.y + i * lineHeight,
                      detailLines[detailLine + i].c_str());
  }
}

void AgendaActivity::drawMessage() {
  if (message.empty() || static_cast<long>(millis() - messageUntil) >= 0) return;
  renderer.drawText(NOTOSANS_16_FONT_ID, SIDE_PAD, renderer.getScreenHeight() - 40, message.c_str());
}

void AgendaActivity::render(RenderLock&&) {
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  renderer.clearScreen();

  if (phase != Phase::Idle) {
    drawHeaderAndTabs();
    GUI.drawPopup(renderer, tr(STR_AGENDA_UPDATING));
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  if (view == View::Detail) drawDetail();
  else drawEventList();
  drawMessage();

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
