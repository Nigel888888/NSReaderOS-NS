#include "RickyHomePhraseActivity.h"
#ifdef RICKYOS_PRODUCT
#include <Bitmap.h>
#include <I18n.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "activities/home/FileBrowserActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/RickyPageUi.h"
#include "components/RickyProfile.h"
#include "components/UITheme.h"
#include "util/RickyStorageLayout.h"

const char* RickyHomePhraseActivity::headerTitle() const { return tr(STR_RICKY_PHRASE_TITLE); }

void RickyHomePhraseActivity::buildScreen(UiScreen& screen) {
  namespace fui = freeink::ui;
  const auto bounds = pageContentRect();
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(bounds.y), static_cast<int16_t>(renderer.getScreenWidth() - bounds.x - bounds.width),
      static_cast<int16_t>(renderer.getScreenHeight() - bounds.y - bounds.height), static_cast<int16_t>(bounds.x)});
  const auto& theme = screen.theme();
  screen.insetContent(fui::Insets{theme.spaceSm, theme.spaceLg, theme.spaceSm, theme.spaceLg});
  const int gap = std::max<int>(12, theme.spaceSm);
  const int smallHeight = screen.target().lineHeight(theme.smallText.font);

  // Live banner preview: imported picture + the phrase that will be overlaid.
  int previewHeight = screen.body().height / 3;
  previewHeight = std::clamp(previewHeight, smallHeight * 5, smallHeight * 11);
  const auto preview = screen.takeTop(previewHeight, gap);
  bool pictured = RickyProfile::drawHomeBanner(renderer, Rect{preview.x, preview.y, preview.width, preview.height});
  if (!pictured) {
    screen.target().fill(preview, fui::Paint::dither(fui::Color::LightGray), 6);
    screen.target().stroke(preview, fui::Paint::solid(fui::Color::Black), 1, 6);
  }
  const int panelX = preview.x + preview.width * 42 / 100;
  const fui::Rect panel{static_cast<int16_t>(panelX), static_cast<int16_t>(preview.y + gap),
                        static_cast<int16_t>(preview.right() - gap - panelX),
                        static_cast<int16_t>(preview.height - gap * 2)};
  screen.target().fill(panel, fui::Paint::solid(fui::Color::White), 4);
  auto phrase = theme.titleText;
  phrase.font = SETTINGS.rickyBannerFontId(SETTINGS.rickyBannerFont, SETTINGS.rickyBannerFontSize);
  phrase.bold = true;
  phrase.align = fui::TextAlign::Center;
  phrase.maxLines = 4;
  {
    RickyPageUi::Bold bold(renderer, 2);
    RickyPageUi::wrappedText(
        screen.target(),
        fui::Rect{panel.x, panel.y, panel.width, static_cast<int16_t>(panel.height)}, RickyProfile::homePhrase(),
        phrase);
  }

  auto hint = theme.smallText;
  hint.maxLines = 2;
  screen.target().text(screen.takeTop(screen.target().lineHeight(hint.font) * 2, theme.spaceLg),
                       tr(STR_RICKY_PHRASE_HINT), hint);
  if (failed)
    screen.target().text(screen.takeTop(screen.target().lineHeight(hint.font), theme.spaceSm),
                         tr(STR_RICKY_PROFILE_FAILED), hint);
  static const char* const kSizeLabels[] = {tr(STR_RICKY_BANNER_SIZE_12), tr(STR_RICKY_BANNER_SIZE_14),
                                            tr(STR_RICKY_BANNER_SIZE_16)};
  rows[0].label = tr(STR_RICKY_PHRASE_EDIT);
  rows[1].label = tr(STR_RICKY_BANNER_CHOOSE);
  rows[2].label = tr(STR_RICKY_BANNER_CLEAR);
  rows[3].label = tr(STR_RICKY_BANNER_FONT);
  rows[3].subtitle = SETTINGS.rickyBannerFont == 0 ? tr(STR_RICKY_BANNER_SANS) : tr(STR_RICKY_BANNER_SERIF);
  rows[4].label = tr(STR_RICKY_BANNER_SIZE);
  rows[4].subtitle = kSizeLabels[SETTINGS.rickyBannerFontSize < 3 ? SETTINGS.rickyBannerFontSize : 1];
  rows[5].label = tr(STR_RICKY_PHRASE_RESET);
  for (int i = 0; i < listCount(); ++i) rows[i].actionValue = i;
  fui::ListProps props;
  props.items = rows.data();
  props.count = rows.size();
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.rowGap = std::max<int16_t>(6, theme.listRowGap);
  syncListViewport(screen, props);
  screen.list(props);
}

bool RickyHomePhraseActivity::handleCustomInput() {
  if (!waitForConfirmRelease) return false;
  if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) waitForConfirmRelease = false;
  return true;
}

void RickyHomePhraseActivity::activateIndex(int index) {
  app.clearTapFlash();
  if (index == 0) {
    startActivityForResultWith<KeyboardEntryActivity>(
        [this](const ActivityResult& result) {
          waitForConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
          if (result.isCancelled) return;
          const auto* entry = std::get_if<KeyboardResult>(&result.data);
          if (!entry) return;
          RenderLock lock(*this);
          failed = !RickyProfile::setHomePhrase(entry->text);
          requestUpdate();
        },
        tr(STR_RICKY_PHRASE_TITLE), std::string(RickyProfile::homePhrase()), sizeof(SETTINGS.rickyHomePhrase) - 1);
  } else if (index == 1) {
    // Pick an image from /images; the file browser filters to PNG/JPG/BMP.
    startActivityForResultWith<FileBrowserActivity>(
        [this](const ActivityResult& result) {
          waitForConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
          if (result.isCancelled) return;
          const auto* entry = std::get_if<FilePathResult>(&result.data);
          if (!entry) return;
          RenderLock lock(*this);
          closeRouting();
          GUI.drawPopup(renderer, tr(STR_RICKY_BANNER_IMPORTING));
          failed = !RickyProfile::importHomeBanner(entry->path);
          requestUpdate();
        },
        RickyStorageLayout::IMAGES, FileBrowserActivity::Mode::PickAvatar);
  } else if (index == 2) {
    RenderLock lock(*this);
    failed = !RickyProfile::setHomeBanner("");
    requestUpdate();
  } else if (index == 3) {
    RenderLock lock(*this);
    SETTINGS.rickyBannerFont = (SETTINGS.rickyBannerFont + 1) % CrossPointSettings::RICKY_BANNER_FONT_COUNT;
    SETTINGS.saveToFile();
    requestUpdate();
  } else if (index == 4) {
    RenderLock lock(*this);
    SETTINGS.rickyBannerFontSize = (SETTINGS.rickyBannerFontSize + 1) % CrossPointSettings::RICKY_BANNER_SIZE_COUNT;
    SETTINGS.saveToFile();
    requestUpdate();
  } else if (index == 5) {
    RenderLock lock(*this);
    failed = !RickyProfile::setHomePhrase("");
    requestUpdate();
  }
}
#endif
