#include "NutstoreConfigActivity.h"
#ifdef RICKYOS_PRODUCT

#include <I18n.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "activities/apps/nutstore/NutstoreBrowserActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/RickyPageUi.h"
#include "components/UITheme.h"
#include "util/NutstoreDav.h"

namespace fui = freeink::ui;

const char* NutstoreConfigActivity::headerTitle() const { return tr(STR_RICKY_NUTSTORE_TITLE); }

void NutstoreConfigActivity::buildScreen(UiScreen& screen) {
  const auto bounds = pageContentRect();
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(bounds.y), static_cast<int16_t>(renderer.getScreenWidth() - bounds.x - bounds.width),
      static_cast<int16_t>(renderer.getScreenHeight() - bounds.y - bounds.height), static_cast<int16_t>(bounds.x)});
  const auto& theme = screen.theme();
  screen.insetContent(fui::Insets{theme.spaceSm, theme.spaceLg, theme.spaceSm, theme.spaceLg});
  const int gap = std::max<int>(12, theme.spaceSm);

  rows[0].label = tr(STR_RICKY_NUTSTORE_FOLDER);
  rows[0].subtitle = SETTINGS.rickyNutstoreFolder;
  rows[1].label = tr(STR_RICKY_NUTSTORE_ACCOUNT);
  rows[1].subtitle = SETTINGS.rickyNutstoreAccount[0] ? SETTINGS.rickyNutstoreAccount : tr(STR_RICKY_NUTSTORE_NOT_SET);
  rows[2].label = tr(STR_RICKY_NUTSTORE_PASSWORD);
  rows[2].subtitle = SETTINGS.rickyNutstorePassword[0] ? "••••••••" : tr(STR_RICKY_NUTSTORE_NOT_SET);
  rows[3].label = tr(STR_RICKY_NUTSTORE_CONNECT);
  rows[4].label = tr(STR_RICKY_NUTSTORE_CLEAR);
  rows[5].label = tr(STR_RICKY_NUTSTORE_GUIDE_1);
  rows[6].label = tr(STR_RICKY_NUTSTORE_GUIDE_3);
  for (int i = 0; i < ROWS; ++i) rows[i].actionValue = i;

  fui::ListProps props;
  props.items = rows.data();
  props.count = rows.size();
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.rowGap = std::max<int16_t>(6, theme.listRowGap);
  syncListViewport(screen, props);
  screen.list(props);
}

bool NutstoreConfigActivity::handleCustomInput() {
  if (!waitForConfirmRelease) return false;
  if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) waitForConfirmRelease = false;
  return true;
}

void NutstoreConfigActivity::editFolder() {
  startActivityForResultWith<KeyboardEntryActivity>(
      [this](const ActivityResult& result) {
        waitForConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
        if (result.isCancelled) return;
        const auto* entry = std::get_if<KeyboardResult>(&result.data);
        if (!entry) return;
        RenderLock lock(*this);
        std::string value = entry->text;
        while (!value.empty() && value.front() == '/') value.erase(value.begin());
        while (!value.empty() && value.back() == '/') value.pop_back();
        snprintf(SETTINGS.rickyNutstoreFolder, sizeof(SETTINGS.rickyNutstoreFolder), "%s", value.c_str());
        SETTINGS.saveToFile();
        requestUpdate();
      },
      tr(STR_RICKY_NUTSTORE_FOLDER), std::string(SETTINGS.rickyNutstoreFolder),
      sizeof(SETTINGS.rickyNutstoreFolder) - 1);
}

void NutstoreConfigActivity::editAccount() {
  startActivityForResultWith<KeyboardEntryActivity>(
      [this](const ActivityResult& result) {
        waitForConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
        if (result.isCancelled) return;
        const auto* entry = std::get_if<KeyboardResult>(&result.data);
        if (!entry) return;
        RenderLock lock(*this);
        snprintf(SETTINGS.rickyNutstoreAccount, sizeof(SETTINGS.rickyNutstoreAccount), "%s", entry->text.c_str());
        SETTINGS.saveToFile();
        requestUpdate();
      },
      tr(STR_RICKY_NUTSTORE_ACCOUNT), std::string(SETTINGS.rickyNutstoreAccount),
      sizeof(SETTINGS.rickyNutstoreAccount) - 1);
}

void NutstoreConfigActivity::editPassword() {
  startActivityForResultWith<KeyboardEntryActivity>(
      [this](const ActivityResult& result) {
        waitForConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
        if (result.isCancelled) return;
        const auto* entry = std::get_if<KeyboardResult>(&result.data);
        if (!entry) return;
        RenderLock lock(*this);
        snprintf(SETTINGS.rickyNutstorePassword, sizeof(SETTINGS.rickyNutstorePassword), "%s", entry->text.c_str());
        SETTINGS.saveToFile();
        requestUpdate();
      },
      tr(STR_RICKY_NUTSTORE_PASSWORD), std::string(SETTINGS.rickyNutstorePassword),
      sizeof(SETTINGS.rickyNutstorePassword) - 1, InputType::Password);
}

void NutstoreConfigActivity::activateIndex(const int index) {
  app.clearTapFlash();
  switch (index) {
    case 0:
      editFolder();
      break;
    case 1:
      editAccount();
      break;
    case 2:
      editPassword();
      break;
    case 3: {
      // Test connection: run a synchronous, bounded PROPFIND under a popup.
      if (!SETTINGS.rickyNutstoreAccount[0] || !SETTINGS.rickyNutstorePassword[0]) {
        rows[3].subtitle = tr(STR_RICKY_NUTSTORE_FAIL);
        requestUpdate();
        break;
      }
      GUI.drawPopup(renderer, tr(STR_RICKY_NUTSTORE_CONNECTING));
      std::string message;
      const nutstore::Result r = nutstore::verify(SETTINGS.rickyNutstoreFolder, SETTINGS.rickyNutstoreAccount,
                                                  SETTINGS.rickyNutstorePassword, message);
      rows[3].subtitle = (r == nutstore::Result::Ok) ? tr(STR_RICKY_NUTSTORE_OK) : tr(STR_RICKY_NUTSTORE_FAIL);
      requestUpdate();
      if (r == nutstore::Result::Ok) {
        startActivityForResultWith<NutstoreBrowserActivity>([](const ActivityResult&) {});
      }
      break;
    }
    case 4: {
      RenderLock lock(*this);
      SETTINGS.rickyNutstoreAccount[0] = '\0';
      SETTINGS.rickyNutstorePassword[0] = '\0';
      SETTINGS.saveToFile();
      rows[3].subtitle = nullptr;
      requestUpdate();
      break;
    }
    default:
      break;
  }
}
#endif
