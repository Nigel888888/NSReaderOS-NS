#include "AiConfigActivity.h"
#ifdef RICKYOS_PRODUCT

#include <I18n.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "util/AiCredentials.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/RickyPageUi.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

const char* AiConfigActivity::headerTitle() const { return tr(STR_AI_CONFIG_TITLE); }

void AiConfigActivity::buildScreen(UiScreen& screen) {
  const auto bounds = pageContentRect();
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(bounds.y), static_cast<int16_t>(renderer.getScreenWidth() - bounds.x - bounds.width),
      static_cast<int16_t>(renderer.getScreenHeight() - bounds.y - bounds.height), static_cast<int16_t>(bounds.x)});
  const auto& theme = screen.theme();
  screen.insetContent(fui::Insets{theme.spaceSm, theme.spaceLg, theme.spaceSm, theme.spaceLg});
  const int gap = std::max<int>(12, theme.spaceSm);

  AiCredentials::Config cfg = AiCredentials::load();
  // maskedKey() returns a temporary; keep it alive through screen.list().
  const std::string masked = AiCredentials::maskedKey();
  rows[0].label = tr(STR_AI_URL);
  rows[0].subtitle = cfg.url.c_str();
  rows[1].label = tr(STR_AI_MODEL);
  rows[1].subtitle = cfg.model.c_str();
  rows[2].label = tr(STR_AI_KEY);
  rows[2].subtitle = masked.c_str();
  rows[3].label = tr(STR_AI_CLEAR);
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

bool AiConfigActivity::handleCustomInput() {
  if (!waitForConfirmRelease) return false;
  if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) waitForConfirmRelease = false;
  return true;
}

void AiConfigActivity::editUrl() {
  AiCredentials::Config cfg = AiCredentials::load();
  startActivityForResultWith<KeyboardEntryActivity>(
      [this](const ActivityResult& result) {
        waitForConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
        if (result.isCancelled) return;
        const auto* entry = std::get_if<KeyboardResult>(&result.data);
        if (!entry || entry->text.empty()) return;
        RenderLock lock(*this);
        AiCredentials::Config cfg = AiCredentials::load();
        cfg.url = entry->text;
        AiCredentials::save(cfg);
        requestUpdate();
      },
      tr(STR_AI_URL), cfg.url, AiCredentials::MAX_URL_LEN);
}

void AiConfigActivity::editModel() {
  AiCredentials::Config cfg = AiCredentials::load();
  startActivityForResultWith<KeyboardEntryActivity>(
      [this](const ActivityResult& result) {
        waitForConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
        if (result.isCancelled) return;
        const auto* entry = std::get_if<KeyboardResult>(&result.data);
        if (!entry || entry->text.empty()) return;
        RenderLock lock(*this);
        AiCredentials::Config cfg = AiCredentials::load();
        cfg.model = entry->text;
        AiCredentials::save(cfg);
        requestUpdate();
      },
      tr(STR_AI_MODEL), cfg.model, AiCredentials::MAX_MODEL_LEN);
}

void AiConfigActivity::editKey() {
  startActivityForResultWith<KeyboardEntryActivity>(
      [this](const ActivityResult& result) {
        waitForConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
        if (result.isCancelled) return;
        const auto* entry = std::get_if<KeyboardResult>(&result.data);
        if (!entry) return;
        RenderLock lock(*this);
        AiCredentials::Config cfg = AiCredentials::load();
        cfg.key = entry->text;  // key is masked in UI and obfuscated on disk
        AiCredentials::save(cfg);
        requestUpdate();
      },
      tr(STR_AI_KEY), std::string(), AiCredentials::MAX_KEY_LEN, InputType::Password);
}

void AiConfigActivity::clearConfig() {
  AiCredentials::Config cfg;
  cfg.url = AiCredentials::DEFAULT_URL;
  cfg.model = AiCredentials::DEFAULT_MODEL;
  cfg.key.clear();
  AiCredentials::save(cfg);
  requestUpdate();
}

void AiConfigActivity::activateIndex(const int index) {
  app.clearTapFlash();
  switch (index) {
    case 0: editUrl(); break;
    case 1: editModel(); break;
    case 2: editKey(); break;
    case 3: clearConfig(); break;
    default: break;
  }
}

void AiConfigActivity::onBackButton() {
  // Opened from Settings, so finishing returns to the settings list.
  finish();
}
#endif
