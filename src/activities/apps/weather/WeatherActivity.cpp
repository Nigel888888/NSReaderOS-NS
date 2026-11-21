#include "WeatherActivity.h"

#include <WiFi.h>

#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "components/UITheme.h"
#include "fontIds.h"
#include "util/PluginHttp.h"

namespace {

constexpr const char* kCityPath = "/weather_city.txt";
constexpr const char* kCachePath = "/weather_cache.json";
constexpr size_t kMaxForecastResponse = 16 * 1024;
constexpr unsigned long kMessageDurationMs = 2600;

struct City {
  const char* name;
  double latitude;
  double longitude;
};

// Small preset list keeps the first release offline-friendly and avoids a
// second geocoding request on every city change. Coordinates are Open-Meteo's.
constexpr City kCities[] = {
    {"Beijing", 39.9042, 116.4074}, {"Shanghai", 31.2304, 121.4737}, {"Guangzhou", 23.1291, 113.2644},
    {"Shenzhen", 22.5431, 114.0579}, {"Changsha", 28.2282, 112.9388}, {"Xiangtan", 27.8290, 112.9403},
    {"Chengdu", 30.5728, 104.0668}, {"Hangzhou", 30.2741, 120.1551},
};
constexpr int kCityCount = static_cast<int>(sizeof(kCities) / sizeof(kCities[0]));

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

}  // namespace

void WeatherActivity::onEnter() {
  Activity::onEnter();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  loadCityIndex();
  loadCache();
  selected_ = 0;
  message_ = nullptr;
  messageUntil_ = 0;
  requestUpdate();
}

void WeatherActivity::loadCityIndex() {
  std::string text;
  if (!Storage.readFileToString("WEATHER", kCityPath, 16, text)) return;
  const int value = std::atoi(text.c_str());
  if (value >= 0 && value < kCityCount) cityIndex_ = value;
}

void WeatherActivity::loadCache() {
  std::string json;
  if (!Storage.readFileToString("WEATHER", kCachePath, 4096, json)) return;
  JsonDocument doc;
  if (deserializeJson(doc, json.c_str())) return;
  cache_.valid = true;
  cache_.cityIndex = static_cast<uint8_t>(doc["cityIndex"] | 0);
  cache_.temperature = static_cast<float>(doc["temperature"] | 0.0);
  cache_.weatherCode = static_cast<int>(doc["code"] | -1);
  cache_.windSpeed = static_cast<float>(doc["wind"] | 0.0);
  const char* observed = doc["observed"] | "";
  snprintf(cache_.observed, sizeof(cache_.observed), "%s", observed);
}

void WeatherActivity::saveCache() const {
  JsonDocument doc;
  doc["cityIndex"] = cache_.cityIndex;
  doc["temperature"] = cache_.temperature;
  doc["code"] = cache_.weatherCode;
  doc["wind"] = cache_.windSpeed;
  doc["observed"] = cache_.observed;
  String out;
  serializeJson(doc, out);
  Storage.writeFile(kCachePath, out);
}

bool WeatherActivity::cacheMatchesCity() const {
  return cache_.valid && cache_.cityIndex == static_cast<uint8_t>(cityIndex_);
}

void WeatherActivity::showMessage(const char* text) {
  message_ = text;
  messageUntil_ = millis() + kMessageDurationMs;
  requestUpdate();
}

void WeatherActivity::cycleCity() {
  cityIndex_ = (cityIndex_ + 1) % kCityCount;
  Storage.writeFile(kCityPath, String(cityIndex_));
  showMessage(tr(STR_WEATHER_CITY_CHANGED));
}

void WeatherActivity::updateWeather() {
  if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
    showMessage(tr(STR_WEATHER_NO_WIFI));
    return;
  }

  GUI.drawPopup(renderer, tr(STR_WEATHER_UPDATING));
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);

  const City& city = kCities[cityIndex_];
  char url[256];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,weather_code,wind_speed_10m&timezone=auto",
           city.latitude, city.longitude);

  String body;
  const int status = pluginhttp::request(nullptr, url, "GET", "", {}, body, kMaxForecastResponse);
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
  if (current.isNull()) {
    showMessage(tr(STR_WEATHER_UPDATE_FAILED));
    return;
  }

  cache_.valid = true;
  cache_.cityIndex = static_cast<uint8_t>(cityIndex_);
  cache_.temperature = static_cast<float>(current["temperature_2m"] | 0.0);
  cache_.weatherCode = static_cast<int>(current["weather_code"] | -1);
  cache_.windSpeed = static_cast<float>(current["wind_speed_10m"] | 0.0);
  const char* observed = current["time"] | "";
  snprintf(cache_.observed, sizeof(cache_.observed), "%s", observed);
  saveCache();
  showMessage(tr(STR_WEATHER_UPDATED));
}

void WeatherActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToApps();
    return;
  }

  const WeatherLayout layout = layoutFor(renderer);
  int touchX, touchY;
  if (mappedInput.wasScreenTapped(touchX, touchY)) {
    const int touched = buttonFromPoint(layout, touchX, touchY);
    if (touched >= 0) {
      RenderLock lock(*this);
      selected_ = touched;
      lock.unlock();
      if (touched == 0) {
        updateWeather();
      } else {
        cycleCity();
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
      updateWeather();
    } else {
      cycleCity();
    }
    return;
  }
  if (next != selected_) {
    RenderLock lock(*this);
    selected_ = next;
    lock.unlock();
    requestUpdate();
  }
}

void WeatherActivity::render(RenderLock&&) {
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const WeatherLayout layout = layoutFor(renderer);
  GUI.drawHeader(renderer, layout.header, tr(STR_WEATHER_TITLE));

  const Rect& card = layout.card;
  const int radius = std::min(metrics.optionPopupSelectionRadius, std::min(card.width, card.height) / 2);
  renderer.drawRoundedRect(card.x, card.y, card.width, card.height, 1, radius, true);

  const int pad = std::max(12, metrics.contentSidePadding / 2);
  const int lineGap = std::max(8, metrics.verticalSpacing / 2);
  int y = card.y + pad;

  const char* cityName = kCities[cityIndex_].name;
  int textWidth = renderer.getTextWidth(NOTOSANS_18_FONT_ID, cityName, EpdFontFamily::BOLD);
  renderer.drawText(NOTOSANS_18_FONT_ID, card.x + (card.width - textWidth) / 2, y, cityName, true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(NOTOSANS_18_FONT_ID) + lineGap;

  if (cacheMatchesCity()) {
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
