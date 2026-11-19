#pragma once
#ifdef RICKYOS_PRODUCT
#include <array>

#include "activities/UiListActivity.h"

// Nutstore (坚果云) login/config screen: sync folder, account, app password,
// test connection, clear. Mirrors the ifink "设置 → 坚果云同步" page.
class NutstoreConfigActivity final : public UiListActivity {
 public:
  NutstoreConfigActivity(GfxRenderer& renderer, MappedInputManager& input)
      : UiListActivity("NutstoreConfig", renderer, input) {}

 protected:
  static constexpr int ROWS = 7;
  int listCount() const override { return ROWS; }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;

 private:
  std::array<freeink::ui::ListItem, ROWS> rows{};
  bool waitForConfirmRelease = false;

  void editFolder();
  void editAccount();
  void editPassword();
};
#endif
