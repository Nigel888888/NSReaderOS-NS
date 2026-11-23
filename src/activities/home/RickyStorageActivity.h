#pragma once
#ifdef RICKYOS_PRODUCT
#include <array>

#include "activities/UiListActivity.h"

class RickyStorageActivity final : public UiListActivity {
 public:
  // fromSettings: opened from the Settings grid instead of the removed tab;
  // Back then returns to Settings rather than Home.
  RickyStorageActivity(GfxRenderer& renderer, MappedInputManager& input, bool fromSettings = false)
      : UiListActivity("RickyStorage", renderer, input), fromSettings_(fromSettings) {}
  // No longer a bottom navigation tab; it is a Settings sub-page.
  MainTab mainTab() const override { return MainTab::None; }
  void selectMainTabContentEdge(MainTabContentEdge edge) override {
    nav.requestSelection(MainTabs::contentEdgeIndex(edge, kCardCount));  // Refresh is never an edge
  }

 protected:
  bool fromSettings_ = false;
  void onBackButton() override;
  // Four folder cards and the SD summary (0..4), then the header's Refresh.
  static constexpr int kCardCount = 5;
  static constexpr int kRefreshAction = kCardCount;
  int listCount() const override { return kCardCount + 1; }
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void drawChrome() override;
  void drawFooter() override;

 private:
  enum class Space : uint8_t { Measuring, Ready, Unavailable };
  // Folder behind a content card; the first existing candidate wins.
  static const char* folderFor(int index);
  bool folderMissing = false;
  Space space = Space::Measuring;
  uint64_t sdTotalBytes = 0;
  uint64_t sdFreeBytes = 0;
  std::array<int, 4> counts{-1, -1, -1, -1};  // books, fonts, images, downloads; -1 = unknown
  uint32_t seenScans = 0;                     // background results already applied to this page
  bool counting = false;                      // a background pass is running

 public:
  // Something changed the card (a delete, an import): count again on the next visit.
  static void invalidateScan();

 private:
  void applyLastScan();
  void refresh();
};
#endif
