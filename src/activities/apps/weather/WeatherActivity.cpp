#include "WeatherActivity.h"

#include <SecureHttpClient.h>
#include <WiFi.h>

#include <I18n.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

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
// block far longer than that, so every wait poll feeds it. SecureHttpClient
// already applies its own 15 s per-read timeout plus one reconnect retry, which
// is what actually bounds a stalled request. This callback's abort deadline is
// only a last-resort backstop for a transfer stuck outside those timed reads
// (e.g. deep inside a TLS handshake on a weak link). It MUST stay comfortably
// above the library timeout: a shorter value aborts healthy slow requests in
// the handshake -> status -1 -> a spurious "update failed" (regression of the
// 10 s value). 60 s leaves the library in control while still preventing a
// permanent hang.
constexpr unsigned long kNetworkRequestTimeoutMs = 60000;
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

// Keep the full ISO date so the weekday can be derived correctly even when
// the cache is a day old. Legacy "MM-DD" values still render without a row
// label; they simply fall back to the device's current weekday.
void copyIsoDate(const char* iso, char* out, size_t outSize) {
  if (!iso || outSize == 0) return;
  snprintf(out, outSize, "%s", iso);
}

int weekdayFromIso(const char* iso) {
  if (!iso || std::strlen(iso) < 10) return -1;
  char buf[16] = {};
  snprintf(buf, sizeof(buf), "%.4s", iso);
  const int year = std::atoi(buf);
  snprintf(buf, sizeof(buf), "%.2s", iso + 5);
  const int month = std::atoi(buf);
  snprintf(buf, sizeof(buf), "%.2s", iso + 8);
  const int day = std::atoi(buf);
  if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31) return -1;
  struct tm t = {};
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = day;
  t.tm_hour = 12;  // noon avoids DST-day rollover surprises
  mktime(&t);
  return t.tm_wday;  // 0 = Sunday
}

const char* weekdayName(int wday) {
  switch (wday) {
    case 0: return tr(STR_WEATHER_SUN);
    case 1: return tr(STR_WEATHER_MON);
    case 2: return tr(STR_WEATHER_TUE);
    case 3: return tr(STR_WEATHER_WED);
    case 4: return tr(STR_WEATHER_THU);
    case 5: return tr(STR_WEATHER_FRI);
    case 6: return tr(STR_WEATHER_SAT);
    default: return "";
  }
}

const char* uvLevelName(float uv) {
  if (uv < 3.0f) return tr(STR_WEATHER_UV_LOW);
  if (uv < 6.0f) return tr(STR_WEATHER_UV_MODERATE);
  if (uv < 8.0f) return tr(STR_WEATHER_UV_HIGH);
  return tr(STR_WEATHER_UV_VERY_HIGH);
}

bool isWetCode(int code) {
  return code == 51 || code == 53 || code == 55 || code == 56 || code == 57 || code == 61 || code == 63 ||
         code == 65 || code == 66 || code == 67 || code == 80 || code == 81 || code == 82 || code == 95 ||
         code == 96 || code == 99;
}

const char* adviceFor(int code, float precipitation, float uv, float feelsLike) {
  if (isWetCode(code) || precipitation >= 60.0f) return tr(STR_WEATHER_ADVICE_UMBRELLA);
  if (uv >= 6.0f) return tr(STR_WEATHER_ADVICE_SUNSCREEN);
  if (feelsLike <= 5.0f) return tr(STR_WEATHER_ADVICE_WARM);
  return tr(STR_WEATHER_ADVICE_OUTDOOR);
}

// Fit text into a fixed width by trimming whole UTF-8 codepoints. Used by the
// five-day columns, where a long localized condition must not spill over.
void drawFittedText(const GfxRenderer& renderer, int fontId, int x, int y, int maxWidth, const char* text,
                    bool black = true, EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  if (!text || maxWidth <= 0) return;
  std::string fitted = text;
  while (!fitted.empty() && renderer.getTextWidth(fontId, fitted.c_str(), style) > maxWidth) {
    size_t n = fitted.size() - 1;
    while (n > 0 && (static_cast<unsigned char>(fitted[n]) & 0xC0) == 0x80) --n;
    fitted.erase(n);
  }
  renderer.drawText(fontId, x, y, fitted.c_str(), black, style);
}

void drawCenteredFitted(const GfxRenderer& renderer, int fontId, int centerX, int y, int maxWidth, const char* text,
                        bool black = true, EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const int width = renderer.getTextWidth(fontId, text, style);
  drawFittedText(renderer, fontId, centerX - std::min(width, maxWidth) / 2, y, maxWidth, text, black, style);
}

// --- Small line-art weather icons, built from the renderer's primitives so
// no new bitmap assets are needed on the device. ---------------------------
void drawSun(const GfxRenderer& renderer, int cx, int cy, int radius) {
  const int r = std::max(6, radius);
  for (int q = 0; q < 4; ++q) {
    const int xDir = (q == 0 || q == 3) ? -1 : 1;
    const int yDir = (q < 2) ? -1 : 1;
    renderer.drawArc(r / 2, cx, cy, xDir, yDir, 1, true);
  }
  const int inner = r / 2 + 3;
  const int outer = r + 3;
  for (int i = 0; i < 8; ++i) {
    const float angle = static_cast<float>(i) * 3.14159265f / 4.0f;
    const int x1 = cx + static_cast<int>(std::cos(angle) * inner);
    const int y1 = cy + static_cast<int>(std::sin(angle) * inner);
    const int x2 = cx + static_cast<int>(std::cos(angle) * outer);
    const int y2 = cy + static_cast<int>(std::sin(angle) * outer);
    renderer.drawLine(x1, y1, x2, y2, true);
  }
}

void drawCloud(const GfxRenderer& renderer, int x, int y, int width, int height) {
  const int h = std::max(8, height);
  const int radius = std::min(h / 2, width / 4);
  // Flat base with two rounded humps reads as a cloud at e-paper resolution.
  renderer.drawRoundedRect(x, y + h / 3, width, h - h / 3, 1, radius, true, true, false, false, true);
  renderer.drawArc(radius, x + width / 3, y + h / 2 + radius / 2, -1, -1, 1, true);
  renderer.drawArc(radius, x + 2 * width / 3, y + h / 2 + radius / 2, 1, -1, 1, true);
  renderer.drawLine(x + 2, y + h, x + width - 2, y + h, true);
}

void drawRainCloud(const GfxRenderer& renderer, int x, int y, int width, int height) {
  drawCloud(renderer, x, y, width, height / 2);
  const int baseY = y + height / 2 + 4;
  for (int i = 0; i < 3; ++i) {
    const int dx = x + width * (i + 1) / 4;
    renderer.drawLine(dx, baseY, dx - 2, baseY + height / 3, true);
  }
}

void drawWeatherIcon(const GfxRenderer& renderer, int code, int x, int y, int size) {
  if (code == 0 || code == 1) {
    drawSun(renderer, x + size / 2, y + size / 2, size / 4);
  } else if (code == 2) {
    drawSun(renderer, x + size * 2 / 3, y + size / 3, size / 6);
    drawCloud(renderer, x + 2, y + size / 3, size - 4, size * 2 / 3);
  } else if (code == 3 || code == 45 || code == 48) {
    drawCloud(renderer, x + 2, y + size / 4, size - 4, size * 3 / 4);
  } else {
    drawRainCloud(renderer, x + 2, y + 2, size - 4, size - 4);
  }
}

void drawThermometerIcon(const GfxRenderer& renderer, int x, int y, int size) {
  const int cx = x + size / 3;
  const int top = y + 2;
  const int bottom = y + size * 2 / 3;
  renderer.drawRoundedRect(cx - 2, top, 4, bottom - top, 1, 2, true);
  renderer.drawArc(size / 5, cx, bottom, -1, -1, 1, true);
  renderer.drawArc(size / 5, cx, bottom, 1, -1, 1, true);
  renderer.drawArc(size / 5, cx, bottom, -1, 1, 1, true);
  renderer.drawArc(size / 5, cx, bottom, 1, 1, 1, true);
  renderer.drawLine(cx + 3, top + 4, x + size - 2, top + 4, true);
  renderer.drawLine(cx + 3, top + 10, x + size - 4, top + 10, true);
}

void drawDropIcon(const GfxRenderer& renderer, int x, int y, int size) {
  const int cx = x + size / 2;
  const int r = size / 3;
  for (int q = 0; q < 4; ++q) {
    const int xDir = (q == 0 || q == 3) ? -1 : 1;
    const int yDir = (q < 2) ? -1 : 1;
    renderer.drawArc(r, cx, y + r + 2, xDir, yDir, 1, true);
  }
  renderer.drawLine(cx - r, y + r + 2, cx, y + size - 2, true);
  renderer.drawLine(cx + r, y + r + 2, cx, y + size - 2, true);
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
  cache_.feelsLike = static_cast<float>(doc["feelsLike"] | cache_.temperature);
  cache_.precipitation = static_cast<float>(doc["precipitation"] | 0.0);
  cache_.uvIndex = static_cast<float>(doc["uv"] | 0.0);
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
      copyIsoDate(d["date"] | "", cache_.days[i].date, sizeof(cache_.days[i].date));
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
  doc["feelsLike"] = cache_.feelsLike;
  doc["precipitation"] = cache_.precipitation;
  doc["uv"] = cache_.uvIndex;
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

void WeatherActivity::applyForecast(JsonObject current, JsonObject daily) {
  cache_.valid = true;
  cache_.city = location_.name;
  if (!current.isNull()) {
    cache_.temperature = static_cast<float>(current["temperature_2m"] | 0.0);
    cache_.feelsLike = static_cast<float>(current["apparent_temperature"] | cache_.temperature);
    cache_.weatherCode = static_cast<int>(current["weather_code"] | -1);
    cache_.windSpeed = static_cast<float>(current["wind_speed_10m"] | 0.0);
    const char* observed = current["time"] | "";
    snprintf(cache_.observed, sizeof(cache_.observed), "%s", observed);
  }
  if (!daily.isNull()) {
    const JsonArray dailyTime = daily["time"].as<JsonArray>();
    const JsonArray dailyMax = daily["temperature_2m_max"].as<JsonArray>();
    const JsonArray dailyMin = daily["temperature_2m_min"].as<JsonArray>();
    const JsonArray dailyCode = daily["weather_code"].as<JsonArray>();
    const JsonArray dailyRain = daily["precipitation_probability_max"].as<JsonArray>();
    const JsonArray dailyUv = daily["uv_index_max"].as<JsonArray>();
    if (!dailyTime.isNull()) {
      size_t i = 0;
      for (; i < kForecastDays && i < dailyTime.size(); ++i) {
        copyIsoDate(dailyTime[i] | "", cache_.days[i].date, sizeof(cache_.days[i].date));
        cache_.days[i].high = dailyMax.isNull() ? 0.0f : static_cast<float>(dailyMax[i] | 0.0);
        cache_.days[i].low = dailyMin.isNull() ? 0.0f : static_cast<float>(dailyMin[i] | 0.0);
        cache_.days[i].code = dailyCode.isNull() ? -1 : static_cast<int>(dailyCode[i] | -1);
      }
      for (; i < kForecastDays; ++i) cache_.days[i] = Day{};
    }
    // "Today" metrics are the first day's daily maxima; Open-Meteo does not
    // expose them as `current` variables.
    if (!dailyRain.isNull() && dailyRain.size() > 0) {
      cache_.precipitation = static_cast<float>(dailyRain[0] | 0.0);
    }
    if (!dailyUv.isNull() && dailyUv.size() > 0) {
      cache_.uvIndex = static_cast<float>(dailyUv[0] | 0.0);
    }
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
           "&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,uv_index_max"
           "&forecast_days=%d&timezone=auto",
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

  applyForecast(current, daily);
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

  // Layout follows the supplied reference: a titled "today" block, a row of
  // four metrics, then a five-column week strip -- all separated by hairlines.
  const int pad = std::max(16, metrics.contentSidePadding / 2);
  const int lineGap = std::max(6, metrics.verticalSpacing / 2);
  const int bigFont = NOTOSANS_18_FONT_ID;
  const int smallFont = NOTOSANS_12_FONT_ID;
  const int bigLine = renderer.getLineHeight(bigFont);
  const int smallLine = renderer.getLineHeight(smallFont);
  const int innerW = card.width - 2 * pad;
  const auto divider = [&](int yy) { renderer.fillRect(card.x + pad, yy, innerW, 1, true); };
  int y = card.y + pad;

  renderer.drawText(bigFont, card.x + pad, y, tr(STR_WEATHER_TODAY), true, EpdFontFamily::BOLD);
  drawSun(renderer, card.x + card.width - pad - 20, y + bigLine / 2, 18);
  y += bigLine + lineGap;
  divider(y);
  y += lineGap + 2;

  if (cache_.valid && cache_.city == location_.name) {
    // Today: condition + high/low on the left, a large weather icon on right.
    const int iconSize = 64;
    drawWeatherIcon(renderer, cache_.weatherCode, card.x + card.width - pad - iconSize, y, iconSize);
    renderer.drawText(bigFont, card.x + pad, y, weatherDescription(cache_.weatherCode), true, EpdFontFamily::BOLD);
    char range[48];
    snprintf(range, sizeof(range), "%.0f°/%.0f°", cache_.days[0].high, cache_.days[0].low);
    renderer.drawText(bigFont, card.x + pad, y + bigLine + 4, range, true, EpdFontFamily::BOLD);
    if (cache_.observed[0]) {
      char observedLine[96];
      snprintf(observedLine, sizeof(observedLine), "%s %s", tr(STR_WEATHER_OBSERVED), cache_.observed);
      drawFittedText(renderer, smallFont, card.x + pad, y + 2 * (bigLine + 4), innerW - iconSize - 12, observedLine);
    }
    y += std::max(iconSize, 2 * bigLine + 8 + smallLine) + lineGap;
    divider(y);
    y += lineGap + 2;

    // Four metric cells: chance of rain, feels-like, UV level and advice.
    const int colW = innerW / 4;
    const char* labels[4] = {tr(STR_WEATHER_PRECIP), tr(STR_WEATHER_FEELS_LIKE), tr(STR_WEATHER_UV),
                             tr(STR_WEATHER_ADVICE)};
    char precipText[24];
    char feelsText[24];
    snprintf(precipText, sizeof(precipText), "%.0f%%", cache_.precipitation);
    snprintf(feelsText, sizeof(feelsText), "%.0f°", cache_.feelsLike);
    const char* values[4] = {precipText, feelsText, uvLevelName(cache_.uvIndex),
                             adviceFor(cache_.weatherCode, cache_.precipitation, cache_.uvIndex, cache_.feelsLike)};
    const int iconBox = 36;
    const int iconTop = y + smallLine + 4;
    for (int i = 0; i < 4; ++i) {
      const int left = card.x + pad + i * colW;
      const int center = left + colW / 2;
      drawCenteredFitted(renderer, smallFont, center, y, colW - 8, labels[i]);
      switch (i) {
        case 0: drawDropIcon(renderer, center - iconBox / 2, iconTop, iconBox); break;
        case 1: drawThermometerIcon(renderer, center - iconBox / 2, iconTop, iconBox); break;
        case 2: drawSun(renderer, center, iconTop + iconBox / 2, iconBox / 3); break;
        default: drawCloud(renderer, center - iconBox / 2, iconTop + 6, iconBox, iconBox - 8); break;
      }
      if (i == 3) {
        // Advice wraps inside its cell instead of being truncated.
        std::string advice = values[i];
        int line = 0;
        size_t pos = 0;
        while (pos < advice.size() && line < 2) {
          size_t take = pos;
          while (take < advice.size()) {
            size_t step = 1;
            const unsigned char c = static_cast<unsigned char>(advice[take]);
            if ((c & 0xE0) == 0xC0) step = 2;
            else if ((c & 0xF0) == 0xE0) step = 3;
            else if ((c & 0xF8) == 0xF0) step = 4;
            if (take + step > advice.size()) step = 1;
            if (renderer.getTextWidth(smallFont, advice.substr(pos, take + step - pos).c_str()) > colW - 10) break;
            take += step;
          }
          if (take == pos) take = std::min(advice.size(), pos + 1);
          drawFittedText(renderer, smallFont, left + 5, iconTop + iconBox + 4 + line * (smallLine + 2),
                         colW - 10, advice.substr(pos, take - pos).c_str());
          pos = take;
          ++line;
        }
      } else {
        drawCenteredFitted(renderer, bigFont, center, iconTop + iconBox + 4, colW - 8, values[i], true,
                           EpdFontFamily::BOLD);
      }
      if (i > 0) renderer.fillRect(left, y, 1, iconBox + smallLine + bigLine + lineGap, true);
    }
    y += smallLine + 4 + iconBox + 4 + 2 * bigLine + lineGap;
    divider(y);
    y += lineGap + 2;

    // Multi-day strip: weekday / line-art icon / condition / high-low.
    renderer.drawText(bigFont, card.x + pad, y, tr(STR_WEATHER_FORECAST), true, EpdFontFamily::BOLD);
    y += bigLine + lineGap;
    divider(y);
    y += lineGap + 2;

    // Ten days in two rows of five: weekday / icon / high-low / condition.
    // A generous row gap keeps the two forecast rows visually separated.
    constexpr int kCols = 5;
    const int dayW = innerW / kCols;
    const int dayIcon = 36;
    const int conditionLine = renderer.getLineHeight(smallFont);
    // Roomier separation between the two five-day rows, plus a closing rule
    // below the last row so the future-weather block reads as complete.
    const int rowGap = std::max(40, smallLine);
    const int rowStep = bigLine + 4 + dayIcon + 4 + smallLine + 2 + conditionLine + rowGap;
    int todayWday = weekdayFromIso(cache_.days[0].date);
    if (todayWday < 0) {
      const time_t now = time(nullptr);
      const struct tm* local = localtime(&now);
      todayWday = local ? local->tm_wday : 0;
    }
    int forecastDays = 0;
    for (int i = 0; i < kForecastDays && cache_.days[i].date[0]; ++i) {
      forecastDays = i + 1;
      const int row = i / kCols;
      const int col = i % kCols;
      const int left = card.x + pad + col * dayW;
      const int center = left + dayW / 2;
      const int rowY = y + row * rowStep;
      int wday = weekdayFromIso(cache_.days[i].date);
      if (wday < 0) wday = (todayWday + i) % 7;
      drawCenteredFitted(renderer, bigFont, center, rowY, dayW - 4, weekdayName(wday), true, EpdFontFamily::BOLD);
      drawWeatherIcon(renderer, cache_.days[i].code, center - dayIcon / 2, rowY + bigLine + 4, dayIcon);
      char dayRange[32];
      snprintf(dayRange, sizeof(dayRange), "%.0f°/%.0f°", cache_.days[i].high, cache_.days[i].low);
      const int tempY = rowY + bigLine + 4 + dayIcon + 4;
      drawCenteredFitted(renderer, smallFont, center, tempY, dayW - 2, dayRange);
      // Condition (e.g. 小雨 / 晴 / 阴) on its own line below the temperature.
      drawCenteredFitted(renderer, smallFont, center, tempY + smallLine + 2, dayW - 2,
                         weatherDescription(cache_.days[i].code));
      if (col > 0) renderer.fillRect(left, rowY, 1, rowStep - rowGap / 2, true);
    }
    // Closing rule below the last forecast row.
    if (forecastDays > 0) {
      const int forecastRows = (forecastDays + kCols - 1) / kCols;
      const int lastContentH = bigLine + 4 + dayIcon + 4 + smallLine + 2 + conditionLine;
      divider(y + (forecastRows - 1) * rowStep + lastContentH + lineGap);
    }
  } else {
    const char* noData = tr(STR_WEATHER_NO_DATA);
    const int textWidth = renderer.getTextWidth(bigFont, noData);
    renderer.drawText(bigFont, card.x + (card.width - textWidth) / 2, y + bigLine, noData);
  }

  if (message_ && millis() < messageUntil_) {
    const int textWidth = renderer.getTextWidth(smallFont, message_);
    renderer.drawText(smallFont, card.x + (card.width - textWidth) / 2,
                      card.y + card.height - pad - smallLine, message_);
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
