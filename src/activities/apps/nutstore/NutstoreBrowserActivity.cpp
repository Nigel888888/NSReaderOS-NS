#include "NutstoreBrowserActivity.h"
#ifdef RICKYOS_PRODUCT

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/CatalogScreens.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/NutstoreDav.h"
#include "util/RickyStorageLayout.h"

namespace fui = freeink::ui;
namespace {

bool isTextDocument(const std::string& name) {
  const auto dot = name.rfind('.');
  if (dot == std::string::npos) return false;
  std::string ext = name.substr(dot + 1);
  for (char& c : ext)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return ext == "md" || ext == "txt";
}

std::string nutstoreErrorText(const nutstore::Result result, const std::string& detail) {
  switch (result) {
    case nutstore::Result::AuthFailure:
      return tr(STR_RICKY_NUTSTORE_FAIL);
    case nutstore::Result::MissingFolder:
      return tr(STR_RICKY_NUTSTORE_ERR_FOLDER);
    case nutstore::Result::ParseFailure:
      return tr(STR_RICKY_NUTSTORE_ERR_PARSE);
    case nutstore::Result::TooLarge:
      return tr(STR_RICKY_NUTSTORE_ERR_SIZE);
    case nutstore::Result::TransportFailure: {
      char buffer[160];
      snprintf(buffer, sizeof(buffer), tr(STR_RICKY_NUTSTORE_ERR_HTTP),
               detail.empty() ? "unknown" : detail.c_str());
      return buffer;
    }
    case nutstore::Result::Ok:
      break;
  }
  return tr(STR_RICKY_NUTSTORE_FAIL);
}

std::string humanSize(uint64_t bytes) {
  char buf[32];
  if (bytes >= 1024 * 1024)
    snprintf(buf, sizeof(buf), "%u MB", static_cast<unsigned>(bytes / (1024 * 1024)));
  else if (bytes >= 1024)
    snprintf(buf, sizeof(buf), "%u KB", static_cast<unsigned>(bytes / 1024));
  else
    snprintf(buf, sizeof(buf), "%u B", static_cast<unsigned>(bytes));
  return buf;
}

}  // namespace

void NutstoreBrowserActivity::onEnter() {
  CatalogActivity::onEnter();
  folder = SETTINGS.rickyNutstoreFolder;
  currentPath.clear();
  navigationHistory.clear();
  state = State::CHECK_WIFI;
  statusMessage = tr(STR_CHECKING_WIFI);
  checkAndConnectWifi();
}

std::string NutstoreBrowserActivity::baseUrl() const {
  std::string url = nutstore::folderUrl(folder);
  if (!currentPath.empty()) url += currentPath;
  return url;
}

void NutstoreBrowserActivity::startBrowse() {
  nav.reset();
  beginLoading();
  fetchEntries();
}

void NutstoreBrowserActivity::fetchEntries() {
  std::vector<nutstore::Entry> all;
  std::string message;
  const nutstore::Result r = nutstore::list(folder, SETTINGS.rickyNutstoreAccount, SETTINGS.rickyNutstorePassword, all,
                                            message, currentPath);
  if (r != nutstore::Result::Ok) {
    LOG_ERR("NUTS", "list failed: result=%d detail=%s folder=%s path=%s", static_cast<int>(r), message.c_str(),
            SETTINGS.rickyNutstoreFolder, currentPath.c_str());
    state = State::ERROR;
    errorMessage = nutstoreErrorText(r, message);
    requestUpdate();
    return;
  }

  // The parser already returned only direct children of the current folder.
  entries = std::move(all);
  std::sort(entries.begin(), entries.end(), [](const nutstore::Entry& a, const nutstore::Entry& b) {
    if (a.isDirectory != b.isDirectory) return a.isDirectory;
    return a.name < b.name;
  });
  rebuildRowItems();
  state = entries.empty() ? State::ERROR : State::BROWSING;
  if (entries.empty()) errorMessage = tr(STR_RICKY_NUTSTORE_EMPTY);
  requestUpdate();
}

void NutstoreBrowserActivity::rebuildRowItems() {
  rowItems.clear();
  rowItems.reserve(entries.size());
  for (const auto& e : entries) {
    fui::ListItem item;
    item.label = e.name.c_str();
    if (e.isDirectory) {
      item.value = ">";
    } else {
      item.subtitle = humanSize(e.bytes).c_str();
    }
    item.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(std::move(item));
  }
}

void NutstoreBrowserActivity::buildScreen(UiScreen& screen) {
  screenHeader(screen, tr(STR_RICKY_NUTSTORE_TITLE));
  if (!buildStatusScreen(screen, /*boldError=*/false, /*showDownloadTotal=*/true)) {
    if (entries.empty()) {
      screen.centeredText(tr(STR_RICKY_NUTSTORE_EMPTY), screen.theme().bodyText);
      return;
    }
    fui::ListProps props;
    props.items = rowItems.data();
    props.count = static_cast<uint16_t>(rowItems.size());
    props.action = ACTION_ROW;
    props.inputMask = fui::InputTouch;
    props.valueInset = 8;
    syncListViewport(screen, props);
    screen.list(props);
  }
}

void NutstoreBrowserActivity::drawFooter() {
  MappedInputManager::Labels labels;
  switch (state) {
    case State::BROWSING: {
      const char* confirm = (!entries.empty() && entries[nav.selected].isDirectory) ? tr(STR_OPEN) : tr(STR_FETCH);
      labels = mappedInput.mapLabels(tr(STR_BACK), confirm, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      break;
    }
    case State::DOWNLOADING:
      labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
      break;
    case State::ERROR:
      labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
      break;
    default:
      labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      break;
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void NutstoreBrowserActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  const auto& entry = entries[index];
  if (entry.isDirectory) {
    navigationHistory.push_back(currentPath);
    currentPath += entry.href + "/";
    startBrowse();
  } else {
    downloadEntry(entry);
  }
}

void NutstoreBrowserActivity::onBackButton() {
  if (state == State::CHECK_WIFI || navigationHistory.empty()) {
    onGoHome();
  } else {
    currentPath = navigationHistory.back();
    navigationHistory.pop_back();
    startBrowse();
  }
}

void NutstoreBrowserActivity::downloadEntry(const nutstore::Entry& entry) {
  beginDownload(entry.name);

  const std::string url = nutstore::childUrl(baseUrl(), entry.href);
  // md/txt go to /markdown if present; epub goes to the normal books folder.
  const char* destDir = RickyStorageLayout::BOOKS;
  if (isTextDocument(entry.name) && Storage.exists("/markdown")) destDir = "/markdown";
  if (!Storage.exists(destDir)) Storage.mkdir(destDir);

  char dest[256];
  snprintf(dest, sizeof(dest), "%s/%s", destDir, entry.name.c_str());

  const auto err = downloadFile(url, dest, SETTINGS.rickyNutstoreAccount, SETTINGS.rickyNutstorePassword);
  finishDownload(err);
}
#endif
