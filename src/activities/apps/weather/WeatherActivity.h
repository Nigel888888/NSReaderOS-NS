#pragma once

#include <string>

#include "activities/Activity.h"

// Weather app: choose any city by name (geocoded through Open-Meteo), fetch
// current conditions on demand, and keep the last successful result on the SD
// card for offline viewing. No automatic refresh, matching the whiteos manual.
// When the device is offline the app routes to the Wi-Fi screen and resumes
// the pending action once a connection is established.
class WeatherActivity final : public Activity {
 public:
  WeatherActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Weather", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct Location {
    std::string name;
    double latitude = 0.0;
    double longitude = 0.0;
  };

  struct Cache {
    bool valid = false;
    std::string city;
    float temperature = 0.0f;
    int weatherCode = -1;
    float windSpeed = 0.0f;
    char observed[32] = {};
  };

  enum class PendingAction : uint8_t { None, Update, GeocodeAndUpdate };

  Location location_;
  Cache cache_;
  int selected_ = 0;  // 0 = Update, 1 = Change City
  const char* message_ = nullptr;
  unsigned long messageUntil_ = 0;

  PendingAction pending_ = PendingAction::None;
  std::string pendingCityName_;
  bool resumeAfterWifi_ = false;   // set by Wi-Fi result handler, consumed in loop()
  bool awaitingCityName_ = false;  // keyboard result expected

  void loadLocation();
  void saveLocation() const;
  void loadCache();
  void saveCache() const;

  void showMessage(const char* text);
  bool wifiConnected() const;
  void ensureWifiThen(PendingAction action);
  void runPending();
  void promptCityName();
  bool geocodeCity(const std::string& name, Location& out);
  void updateWeather();
};

// Default coordinates used until the user picks a city. Open-Meteo's values.
namespace WeatherDefaults {
inline constexpr const char* kCityName = "Xiangtan";
inline constexpr double kLatitude = 27.8290;
inline constexpr double kLongitude = 112.9403;
}  // namespace WeatherDefaults
