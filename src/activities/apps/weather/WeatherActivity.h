#pragma once

#include "activities/Activity.h"

// Minimal weather app: pick from a small preset city list, update from
// Open-Meteo on demand, and keep the last successful result on the SD card
// for offline viewing. No automatic refresh, matching the whiteos manual.
class WeatherActivity final : public Activity {
 public:
  WeatherActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Weather", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct Cache {
    bool valid = false;
    uint8_t cityIndex = 0;
    float temperature = 0.0f;
    int weatherCode = -1;
    float windSpeed = 0.0f;
    char observed[32] = {};
  };

  int cityIndex_ = 0;
  int selected_ = 0;  // 0 = Update, 1 = Change City
  Cache cache_;
  const char* message_ = nullptr;
  unsigned long messageUntil_ = 0;

  void loadCityIndex();
  void loadCache();
  void saveCache() const;
  void cycleCity();
  void updateWeather();
  void showMessage(const char* text);
  bool cacheMatchesCity() const;
};
