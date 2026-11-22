#pragma once

#include <ArduinoJson.h>

#include <cstdint>
#include <memory>
#include <string>

#include "activities/Activity.h"

namespace freeink {
class SecureHttpClient;
}

// Weather app: choose any city by name (geocoded through Open-Meteo), fetch
// current conditions plus a 5-day forecast on demand, and keep the last
// successful result on the SD card for offline viewing. No automatic refresh.
//
// Networking and rendering are strictly separated: the loop task runs the
// fetch with no renderer access; a Phase state machine hands the "updating"
// notice to render(), which runs on the render task holding RenderLock. This
// mirrors AirPageActivity and avoids the rendering-mutex violation that
// crashed earlier networking code. A reusable SecureHttpClient session avoids
// repeated TLS handshakes that fragment the heap.
class WeatherActivity final : public Activity {
 public:
  WeatherActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~WeatherActivity() override;  // out-of-line: complete SecureHttpClient type

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr int kForecastDays = 5;

  struct Location {
    std::string name;
    double latitude = 0.0;
    double longitude = 0.0;
  };

  struct Day {
    char date[6] = {};  // "MM-DD"
    float high = 0.0f;
    float low = 0.0f;
    int code = -1;
  };

  struct Cache {
    bool valid = false;
    std::string city;
    float temperature = 0.0f;
    int weatherCode = -1;
    float windSpeed = 0.0f;
    char observed[32] = {};
    Day days[kForecastDays];
  };

  enum class Phase : uint8_t { Idle, FetchRequested, Fetching };
  enum class PendingAction : uint8_t { None, Update, GeocodeAndUpdate };

  Location location_;
  Cache cache_;
  Phase phase_ = Phase::Idle;
  PendingAction pending_ = PendingAction::None;
  int selected_ = 0;  // 0 = Update, 1 = Change City
  const char* message_ = nullptr;
  unsigned long messageUntil_ = 0;

  std::string pendingCityName_;
  bool resumeAfterWifi_ = false;  // set by Wi-Fi result handler, consumed in loop()

  std::unique_ptr<freeink::SecureHttpClient> session_;  // reused across requests

  void loadLocation();
  void saveLocation() const;
  void loadCache();
  void saveCache() const;

  void showMessage(const char* text);
  bool wifiConnected() const;
  void ensureWifiThen(PendingAction action);
  void queueFetch();
  void doFetch();
  void promptCityName();
  bool geocodeCity(const std::string& name, Location& out);
  void applyForecast(JsonObject current, JsonArray dailyTime, JsonArray dailyMax, JsonArray dailyMin,
                     JsonArray dailyCode);
  // Returns nullptr on OOM; callers must check before passing to request().
  freeink::SecureHttpClient* httpClient();
};

// Default coordinates used until the user picks a city. Open-Meteo's values.
namespace WeatherDefaults {
inline constexpr const char* kCityName = "Xiangtan";
inline constexpr double kLatitude = 27.8290;
inline constexpr double kLongitude = 112.9403;
}  // namespace WeatherDefaults
