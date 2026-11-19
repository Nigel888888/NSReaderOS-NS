#pragma once
#ifdef RICKYOS_PRODUCT
#include <string>
#include <vector>

#include "activities/CatalogActivity.h"
#include "util/NutstoreDav.h"

// Browses a Nutstore (坚果云) WebDAV folder and downloads selected md/txt/epub
// books to the RickyOS library. Reuses CatalogActivity's WiFi gate and
// download lifecycle, exactly like the OPDS browser.
class NutstoreBrowserActivity final : public CatalogActivity {
 public:
  NutstoreBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : CatalogActivity("NutstoreBrowser", renderer, mappedInput) {}

  void onEnter() override;

 private:
  std::vector<nutstore::Entry> entries;
  std::vector<freeink::ui::ListItem> rowItems;
  std::string folder;
  std::string currentPath;  // "" = root of sync folder; else a subfolder path with trailing '/'
  std::vector<std::string> navigationHistory;

  int listCount() const override { return state == State::BROWSING ? static_cast<int>(entries.size()) : 0; }
  bool hasSearch() const override { return false; }
  void performSearch(const std::string&) override {}
  void downloadFinished(bool cancelled) override { (void)cancelled; startBrowse(); }
  void startBrowse() override;
  void activateIndex(int index) override;
  void buildScreen(UiScreen& screen) override;
  void drawFooter() override;
  void onBackButton() override;

  void rebuildRowItems();
  void fetchEntries();
  void downloadEntry(const nutstore::Entry& entry);
  std::string baseUrl() const;
};
#endif
