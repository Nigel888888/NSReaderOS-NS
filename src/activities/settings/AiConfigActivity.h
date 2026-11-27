#pragma once
#ifdef RICKYOS_PRODUCT
#include <array>
#include <string>

#include "activities/UiListActivity.h"

// AI explain configuration screen: DeepSeek-compatible endpoint URL, model
// name and API key. All values are entered on-device and stored obfuscated on
// the SD card via AiCredentials — never in source, logs or OTA dumps.
class AiConfigActivity final : public UiListActivity {
 public:
  AiConfigActivity(GfxRenderer& renderer, MappedInputManager& input)
      : UiListActivity("AiConfig", renderer, input) {}

 protected:
  static constexpr int ROWS = 4;
  int listCount() const override { return ROWS; }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  void onBackButton() override;

 private:
  std::array<freeink::ui::ListItem, ROWS> rows{};
  bool waitForConfirmRelease = false;

  void editUrl();
  void editModel();
  void editKey();
  void clearConfig();
};
#endif
