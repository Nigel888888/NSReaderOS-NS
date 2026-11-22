#include "WeatherActivity.h"

#include <SecureHttpClient.h>
#include <WiFi.h>

#include <I18n.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Memory.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/PluginHttp.h"
#include "util/TaskWatchdog.h"

namespace {

constexpr const char* kLocationPath = "/weather_location.json";
constexpr const char* kCachePath = "/weather_cache.json";
// A 5-day forecast with current + daily arrays is well under 6 KB in practice;
// keeping the caps tight limits the peak String reserve on the first fetch.
constexpr size_t kMaxForecastResponse = 8 * 1024;
constexpr size_t kMaxGeocodeResponse = 4 * 1024;
// Arduino's loop task is subscribed to the task watchdog. TLS connect/read can
// block far longer than that, so every wait poll feeds it and a request is
// aborted after this deadline rather than rebooting or pinning a black screen.
constexpr unsigned long kNetworkRequestTimeoutMs = 10000;
constexpr unsigned long kMessageDurationMs = 3200;

struct WeatherLayout {
  Rect header;
  Rect card;
  Rect buttons[2];
};

WeatherLayout layoutFor(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  int viewTop, viewRight, viewBottom, viewLeft;
  renderer.getOrientedViewableTRBL(&viewTop, &viewRight, &viewBottom, &viewLeft);

  const int left = safe.x + std::max(metrics.contentSidePadding, viewLeft);
  const int top = safe.y + std::max(metrics.topPadding, viewTop);
  const int right = safe.x + safe.width - std::max(metrics.contentSidePadding, viewRight);
  const int bottom = safe.y + safe.height - std::max(metrics.verticalSpacing, viewBottom);
  const int width = std::max(0, right - left);
  const int gap = std::max(12, metrics.verticalSpacing);
  const Rect header{left, top, width, metrics.headerHeight};

  const int buttonHeight = std::max(48, metrics.keyboardKeyHeight);
  const int buttonGap = std::max(8, metrics.keyboardKeySpacing);
  const int buttonWidth = std::max(1, (width - buttonGap) / 2);
  const int buttonsTop = std::max(header.y + header.height + gap, bottom - buttonHeight);
  const Rect buttons[] = {
      Rect{left, buttonsTop, buttonWidth, buttonHeight},
      Rect{left + buttonWidth + buttonGap, buttonsTop, width - buttonWidth - buttonGap, buttonHeight},
  };
  const Rect card{left, header.y + header.height + gap, width,
                  std::max(0, buttonsTop - gap - (header.y + header.height + gap))};
  return WeatherLayout{header, card, {buttons[0], buttons[1]}};
}

int buttonFromPoint(const WeatherLayout& layout, const int x, const int y) {
  for (int i = 0; i < 2; ++i) {
    const Rect& b = layout.buttons[i];
    if (x >= b.x && x < b.x + b.width && y >= b.y && y < b.y + b.height) return i;
  }
  return -1;
}

const char* weatherDescription(const int code) {
  switch (code) {
    case 0:
      return tr(STR_WEATHER_CLEAR);
    case 1:
      return tr(STR_WEATHER_MAINLY_CLEAR);
    case 2:
      return tr(STR_WEATHER_PARTLY_CLOUDY);
    case 3:
      return tr(STR_WEATHER_OVERCAST);
    case 45:
    case 48:
      return tr(STR_WEATHER_FOG);
    case 51:
    case 53:
    case 55:
    case 56:
    case 57:
      return tr(STR_WEATHER_DRIZZLE);
    case 61:
    case 63:
    case 65:
    case 66:
    case 67:
      return tr(STR_WEATHER_RAIN);
    case 71:
    case 73:
    case 75:
    case 77:
      return tr(STR_WEATHER_SNOW);
    case 80:
    case 81:
    case 82:
      return tr(STR_WEATHER_SHOWERS);
    case 95:
    case 96:
    case 99:
      return tr(STR_WEATHER_THUNDERSTORM);
    default:
      return tr(STR_WEATHER_UNKNOWN);
  }
}

void drawButton(const GfxRenderer& renderer, const Rect button, const char* label, const bool selected,
                const int radius) {
  bool foregroundBlack = true;
  if (selected) {
    foregroundBlack = GUI.drawSelectionBackground(renderer, button);
  } else {
    renderer.drawRoundedRect(button.x, button.y, button.width, button.height, 1, radius, true);
  }
  const int textWidth = renderer.getTextWidth(NOTOSANS_18_FONT_ID, label);
  const int textHeight = renderer.getTextHeight(NOTOSANS_18_FONT_ID);
  renderer.drawText(NOTOSANS_18_FONT_ID, button.x + (button.width - textWidth) / 2,
                    button.y + (button.height - textHeight) / 2, label, foregroundBlack);
}

// "2026-11-22" -> "11-22" (safe on a short, fixed date string).
void shortDate(const char* iso, char* out, size_t outSize) {
  if (!iso || std::strlen(iso) < 10) {
    snprintf(out, outSize, "--");
    return;
  }
  snprintf(out, outSize, "%c%c-%c%c", iso[5], iso[6], iso[8], iso[9]);
}

}  // namespace

WeatherActivity::WeatherActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Weather", renderer, mappedInput) {}

WeatherActivity::~WeatherActivity() = default;

freeink::SecureHttpClient* WeatherActivity::httpClient() {
  if (!session_) session_ = makeUniqueNoThrow<freeink::SecureHttpClient>();
  return session_.get();  // may be nullptr on OOM; callers must check
}

bool WeatherActivity::networkShouldAbort(const unsigned long started) const {
  // pluginhttp documents that this callback is polled in every wait loop.
  resetTaskWatchdogIfSubscribed();
  return millis() - started >= kNetworkRequestTimeoutMs;
}

void WeatherActivity::onEnter() {
  Activity::onEnter();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  loadLocation();
  loadCache();
  selected_ = 0;
  message_ = nullptr;
  messageUntil_ = 0;
  pending_ = PendingAction::None;
  pendingCityName_.clear();
  resumeAfterWifi_ = false;
  phase_ = Phase::Idle;
  requestUpdate();
}

void WeatherActivity::loadLocation() {
  location_.name = WeatherDefaults::kCityName;
  location_.latitude = WeatherDefaults::kLatitude;
  location_.longitude = WeatherDefaults::kLongitude;
  std::string json;
  if (!Storage.readFileToString("WEATHER", kLocationPath, 1024, json)) return;
  JsonDocument doc;
  if (deserializeJson(doc, json.c_str())) return;
  const char* name = doc["name"] | "";
  const double lat = doc["lat"] | WeatherDefaults::kLatitude;
  const double lon = doc["lon"] | WeatherDefaults::kLongitude;
  if (name[0] && lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0) {
    location_.name = name;
    location_.latitude = lat;
    location_.longitude = lon;
  }
}

void WeatherActivity::saveLocation() const {
  JsonDocument doc;
  doc["name"] = location_.name;
  doc["lat"] = location_.latitude;
  doc["lon"] = location_.longitude;
  String out;
  serializeJson(doc, out);
  Storage.writeFile(kLocationPath, out);
}

void WeatherActivity::loadCache() {
  std::string json;
  if (!Storage.readFileToString("WEATHER", kCachePath, 8192, json)) return;
  JsonDocument doc;
  if (deserializeJson(doc, json.c_str())) return;
  cache_.valid = true;
  cache_.city = doc["city"] | "";
  cache_.temperature = static_cast<float>(doc["temperature"] | 0.0);
  cache_.weatherCode = static_cast<int>(doc["code"] | -1);
  cache_.windSpeed = static_cast<float>(doc["wind"] | 0.0);
  const char* observed = doc["observed"] | "";
  snprintf(cache_.observed, sizeof(cache_.observed), "%s", observed);
  JsonArray days = doc["days"].as<JsonArray>();
  if (!days.isNull()) {
    size_t i = 0;
    for (JsonVariant v : days) {
      if (i >= kForecastDays) break;
      JsonObject d = v.as<JsonObject>();
      if (d.isNull()) {
        ++i;
        continue;
      }
      shortDate(d["date"] | "", cache_.days[i].date, sizeof(cache_.days[i].date));
      cache_.days[i].high = static_cast<float>(d["high"] | 0.0);
      cache_.days[i].low = static_cast<float>(d["low"] | 0.0);
      cache_.days[i].code = static_cast<int>(d["code"] | -1);
      ++i;
    }
  }
}

void WeatherActivity::saveCache() const {
  JsonDocument doc;
  doc["city"] = cache_.city;
  doc["temperature"] = cache_.temperature;
  doc["code"] = cache_.weatherCode;
  doc["wind"] = cache_.windSpeed;
  doc["observed"] = cache_.observed;
  JsonArray days = doc["days"].to<JsonArray>();
  for (int i = 0; i < kForecastDays; ++i) {
    JsonObject d = days.add<JsonObject>();
    d["date"] = cache_.days[i].date;
    d["high"] = cache_.days[i].high;
    d["low"] = cache_.days[i].low;
    d["code"] = cache_.days[i].code;
  }
  String out;
  serializeJson(doc, out);
  Storage.writeFile(kCachePath, out);
}

void WeatherActivity::showMessage(const char* text) {
  message_ = text;
  messageUntil_ = millis() + kMessageDurationMs;
  requestUpdate();
}

bool WeatherActivity::wifiConnected() const {
  return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

void WeatherActivity::ensureWifiThen(PendingAction action) {
  pending_ = action;
  if (wifiConnected()) {
    queueFetch();
    return;
  }
  showMessage(tr(STR_WEATHER_NO_WIFI));
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput, true);
  if (!wifi) {
    pending_ = PendingAction::None;
    showMessage(tr(STR_WEATHER_UPDATE_FAILED));
    return;
  }
  startActivityForResult(std::move(wifi), [this](const ActivityResult& result) {
    const auto* wifiResult = std::get_if<WifiResult>(&result.data);
    const bool connected = !result.isCancelled && wifiResult && wifiResult->connected;
    resumeAfterWifi_ = connected;
    if (!connected) pending_ = PendingAction::None;
  });
}

void WeatherActivity::queueFetch() {
  if (phase_ != Phase::Idle) return;
  phase_ = pending_ == PendingAction::GeocodeAndUpdate ? Phase::GeocodeRequested : Phase::ForecastRequested;
  requestUpdate();
}

void WeatherActivity::promptCityName() {
  startActivityForResultWith<KeyboardEntryActivity>(
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        const auto* entry = std::get_if<KeyboardResult>(&result.data);
        if (!entry || entry->text.empty()) return;
        pendingCityName_ = entry->text;
        ensureWifiThen(PendingAction::GeocodeAndUpdate);
      },
      tr(STR_WEATHER_CITY_PROMPT), location_.name, 48);
}

bool WeatherActivity::geocodeCity(const std::string& name, Location& out, const unsigned long started) {
  resetTaskWatchdogIfSubscribed();
  const std::string encoded = pluginhttp::urlEncodeQuery(name);
  char url[384];
  snprintf(url, sizeof(url),
           "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=en&format=json",
           encoded.c_str());

  auto* client = httpClient();
  if (!client) return false;  // OOM: never dereference a null session
  String body;
  const auto keepAlive = [this, started]() { return networkShouldAbort(started); };
  const int status = pluginhttp::request(client, url, "GET", "", {}, body, kMaxGeocodeResponse, nullptr, keepAlive);
  resetTaskWatchdogIfSubscribed();
  if (status != 200) return false;

  JsonDocument doc;
  if (deserializeJson(doc, body.c_str())) return false;
  JsonArray results = doc["results"].as<JsonArray>();
  if (results.isNull() || results.size() == 0) return false;
  JsonObject first = results[0].as<JsonObject>();
  if (first.isNull()) return false;
  const char* found = first["name"] | "";
  const double lat = first["latitude"] | NAN;
  const double lon = first["longitude"] | NAN;
  if (!found[0] || std::isnan(lat) || std::isnan(lon)) return false;
  out.name = found;
  out.latitude = lat;
  out.longitude = lon;
  return true;
}

void WeatherActivity::applyForecast(JsonObject current, JsonArray dailyTime, JsonArray dailyMax,
                                    JsonArray dailyMin, JsonArray dailyCode) {
  cache_.valid = true;
  cache_.city = location_.name;
  if (!current.isNull()) {
    cache_.temperature = static_cast<float>(current["temperature_2m"] | 0.0);
    cache_.weatherCode = static_cast<int>(current["weather_code"] | -1);
    cache_.windSpeed = static_cast<float>(current["wind_speed_10m"] | 0.0);
    const char* observed = current["time"] | "";
    snprintf(cache_.observed, sizeof(cache_.observed), "%s", observed);
  }
  if (!dailyTime.isNull()) {
    size_t i = 0;
    for (; i < kForecastDays && i < dailyTime.size(); ++i) {
      shortDate(dailyTime[i] | "", cache_.days[i].date, sizeof(cache_.days[i].date));
      cache_.days[i].high = dailyMax.isNull() ? 0.0f : static_cast<float>(dailyMax[i] | 0.0);
      cache_.days[i].low = dailyMin.isNull() ? 0.0f : static_cast<float>(dailyMin[i] | 0.0);
      cache_.days[i].code = dailyCode.isNull() ? -1 : static_cast<int>(dailyCode[i] | -1);
    }
    for (; i < kForecastDays; ++i) cache_.days[i] = Day{};
  }
  saveCache();
}

void WeatherActivity::fetchForecast(const bool cityChanged) {
  resetTaskWatchdogIfSubscribed();
  if (!wifiConnected()) {
    showMessage(tr(STR_WEATHER_NO_WIFI));
    return;
  }

  char url[320];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,weather_code,wind_speed_10m"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min&forecast_days=%d&timezone=auto",
           location_.latitude, location_.longitude, kForecastDays);

  auto* client = httpClient();
  if (!client) {
    showMessage(tr(STR_WEATHER_UPDATE_FAILED));
    return;
  }
  String body;
  const unsigned long started = millis();
  const auto keepAlive = [this, started]() { return networkShouldAbort(started); };
  const int status = pluginhttp::request(client, url, "GET", "", {}, body, kMaxForecastResponse, nullptr, keepAlive);
  resetTaskWatchdogIfSubscribed();
  if (status != 200) {
    showMessage(tr(STR_WEATHER_UPDATE_FAILED));
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body.c_str())) {
    showMessage(tr(STR_WEATHER_UPDATE_FAILED));
    return;
  }
  const JsonObject current = doc["current"].as<JsonObject>();
  JsonObject daily = doc["daily"].as<JsonObject>();
  if (current.isNull() && daily.isNull()) {
    showMessage(tr(STR_WEATHER_UPDATE_FAILED));
    return;
  }

  applyForecast(current, daily["time"].as<JsonArray>(), daily["temperature_2m_max"].as<JsonArray>(),
                daily["temperature_2m_min"].as<JsonArray>(), daily["weather_code"].as<JsonArray>());
  showMessage(cityChanged ? tr(STR_WEATHER_CITY_UPDATED) : tr(STR_WEATHER_UPDATED));
}

void WeatherActivity::loop() {
  if (resumeAfterWifi_) {
    resumeAfterWifi_ = false;
    if (wifiConnected() && pending_ != PendingAction::None) queueFetch();
  }

  // Network I/O runs here, off the render task. No renderer access in these
  // phases. Each request gets its own loop turn so the render task can paint
  // the busy popup between geocoding and the forecast, and the loop task can
  // feed its watchdog instead of disappearing into two TLS calls at once.
  if (phase_ == Phase::GeocodeRequested) {
    phase_ = Phase::Geocoding;
    requestUpdate();
    return;
  }
  if (phase_ == Phase::Geocoding) {
    resetTaskWatchdogIfSubscribed();
    Location located;
    if (!geocodeCity(pendingCityName_, located, millis())) {
      pending_ = PendingAction::None;
      pendingCityName_.clear();
      phase_ = Phase::Idle;
      showMessage(tr(STR_WEATHER_CITY_NOT_FOUND));
      return;
    }
    location_ = located;
    saveLocation();
    pendingCityName_.clear();
    phase_ = Phase::ForecastRequested;
    requestUpdate();
    return;
  }
  if (phase_ == Phase::ForecastRequested) {
    phase_ = Phase::Forecasting;
    requestUpdate();
    return;
  }
  if (phase_ == Phase::Forecasting) {
    const bool cityChanged = pending_ == PendingAction::GeocodeAndUpdate;
    fetchForecast(cityChanged);
    pending_ = PendingAction::None;
    phase_ = Phase::Idle;
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToApps();
    return;
  }

  const WeatherLayout layout = layoutFor(renderer);
  int touchX, touchY;
  if (mappedInput.wasScreenTapped(touchX, touchY)) {
    const int touched = buttonFromPoint(layout, touchX, touchY);
    if (touched >= 0) {
      {
        RenderLock lock(*this);
        selected_ = touched;
      }
      if (touched == 0) {
        ensureWifiThen(PendingAction::Update);
      } else {
        promptCityName();
      }
      return;
    }
  }

  int next = selected_;
  if (mappedInput.wasReleased(MappedInputManager::Button::ScreenLeft)) {
    next = 0;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::ScreenRight)) {
    next = 1;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selected_ == 0) {
      ensureWifiThen(PendingAction::Update);
    } else {
      promptCityName();
    }
    return;
  }
  if (next != selected_) {
    RenderLock lock(*this);
    selected_ = next;
    requestUpdate();
  }
}

void WeatherActivity::render(RenderLock&&) {
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const WeatherLayout layout = layoutFor(renderer);
  GUI.drawHeader(renderer, layout.header, tr(STR_WEATHER_TITLE));

  // The updating notice is painted on the render task (holding RenderLock),
  // never from loop(), which is what crashed the previous implementation.
  if (phase_ != Phase::Idle) {
    GUI.drawPopup(renderer, tr(STR_WEATHER_UPDATING));
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  const Rect& card = layout.card;
  const int radius = std::min(metrics.optionPopupSelectionRadius, std::min(card.width, card.height) / 2);
  renderer.drawRoundedRect(card.x, card.y, card.width, card.height, 1, radius, true);

  const int pad = std::max(12, metrics.contentSidePadding / 2);
  const int lineGap = std::max(4, metrics.verticalSpacing / 3);
  int y = card.y + pad;

  const char* cityName = location_.name.c_str();
  int textWidth = renderer.getTextWidth(NOTOSANS_18_FONT_ID, cityName, EpdFontFamily::BOLD);
  renderer.drawText(NOTOSANS_18_FONT_ID, card.x + (card.width - textWidth) / 2, y, cityName, true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(NOTOSANS_18_FONT_ID) + lineGap;

  if (cache_.valid && cache_.city == location_.name) {
    char buffer[96];
    snprintf(buffer, sizeof(buffer), tr(STR_WEATHER_TEMP_FORMAT), cache_.temperature);
    textWidth = renderer.getTextWidth(NOTOSANS_18_FONT_ID, buffer, EpdFontFamily::BOLD);
    renderer.drawText(NOTOSANS_18_FONT_ID, card.x + (card.width - textWidth) / 2, y, buffer, true,
                      EpdFontFamily::BOLD);
    y += renderer.getLineHeight(NOTOSANS_18_FONT_ID) + lineGap;

    const char* description = weatherDescription(cache_.weatherCode);
    textWidth = renderer.getTextWidth(NOTOSANS_18_FONT_ID, description);
    renderer.drawText(NOTOSANS_18_FONT_ID, card.x + (card.width - textWidth) / 2, y, description);
    y += renderer.getLineHeight(NOTOSANS_18_FONT_ID) + lineGap;

    if (cache_.observed[0]) {
      snprintf(buffer, sizeof(buffer), "%s %s", tr(STR_WEATHER_OBSERVED), cache_.observed);
      renderer.drawText(NOTOSANS_18_FONT_ID, card.x + pad, y, buffer);
      y += renderer.getLineHeight(NOTOSANS_18_FONT_ID) + lineGap;
    }
    snprintf(buffer, sizeof(buffer), tr(STR_WEATHER_WIND_FORMAT), cache_.windSpeed);
    char windLine[120];
    snprintf(windLine, sizeof(windLine), "%s %s", tr(STR_WEATHER_WIND), buffer);
    renderer.drawText(NOTOSANS_18_FONT_ID, card.x + pad, y, windLine);
    y += renderer.getLineHeight(NOTOSANS_18_FONT_ID) + lineGap;

    // Divider between current conditions and the forecast block.
    y += lineGap / 2;
    renderer.fillRect(card.x + pad, y, card.width - pad * 2, 1, true);
    y += lineGap / 2 + 2;

    // 5-day forecast, three aligned columns: date | condition | high/low.
    // Condition reuses weatherDescription() so both languages are covered;
    // the temperature range is universal symbols, no translation needed.
    const int rowFont = NOTOSANS_16_FONT_ID;
    const int rowHeight = renderer.getLineHeight(rowFont);
    const int dateX = card.x + pad;
    const int condX = dateX + renderer.getTextWidth(rowFont, "00-00") + pad;
    const int tempRight = card.x + card.width - pad;
    for (int i = 0; i < kForecastDays && cache_.days[i].date[0]; ++i) {
      renderer.drawText(rowFont, dateX, y, cache_.days[i].date);
      renderer.drawText(rowFont, condX, y, weatherDescription(cache_.days[i].code));
      char range[32];
      snprintf(range, sizeof(range), "%.0f° / %.0f°", cache_.days[i].high, cache_.days[i].low);
      renderer.drawText(rowFont, tempRight - renderer.getTextWidth(rowFont, range), y, range);
      y += rowHeight + lineGap;
    }
  } else {
    const char* noData = tr(STR_WEATHER_NO_DATA);
    textWidth = renderer.getTextWidth(NOTOSANS_18_FONT_ID, noData);
    renderer.drawText(NOTOSANS_18_FONT_ID, card.x + (card.width - textWidth) / 2,
                      y + renderer.getLineHeight(NOTOSANS_18_FONT_ID), noData);
  }

  if (message_ && millis() < messageUntil_) {
    textWidth = renderer.getTextWidth(NOTOSANS_18_FONT_ID, message_);
    renderer.drawText(NOTOSANS_18_FONT_ID, card.x + (card.width - textWidth) / 2,
                      card.y + card.height - pad - renderer.getLineHeight(NOTOSANS_18_FONT_ID), message_);
  }

  const int buttonRadius = std::min(metrics.optionPopupSelectionRadius,
                                    std::min(layout.buttons[0].height, layout.buttons[0].width) / 2);
  drawButton(renderer, layout.buttons[0], tr(STR_WEATHER_UPDATE), selected_ == 0, buttonRadius);
  drawButton(renderer, layout.buttons[1], tr(STR_WEATHER_CHANGE_CITY), selected_ == 1, buttonRadius);

  const auto labels = mappedInput.mapDirectionalLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT),
                                                       tr(STR_DIR_RIGHT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
