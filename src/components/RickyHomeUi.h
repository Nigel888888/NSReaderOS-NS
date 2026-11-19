#pragma once

#ifdef RICKYOS_PRODUCT
#include <cstdint>

#include "RecentBooksStore.h"
#include "Rect.h"
#include "UiAppHost.h"
#include "components/media/book-card.h"
#include "components/media/cover-grid.h"

class RickyHomeUi final : public UiAppHost {
 public:
  using CoverPainter = bool (*)(void*, int, const Rect&);
  explicit RickyHomeUi(GfxRenderer& renderer) : UiAppHost(renderer), renderer(renderer) {}
  void begin(const std::vector<RecentBook>& recent, CoverPainter painter, void* owner);
  // recentStart is the first book shown in the "recently opened" strip; the
  // "continue reading" card is pinned to books[0] and never follows paging.
  void configure(Rect content, int selection, bool showSelection, int progress, int recentStart);
  int selectedAction(const MappedInputManager& input);
  int coverHeight() const { return thumbnailHeight; }
  static constexpr int LIBRARY = -3;
  static constexpr int STATISTICS = -4;
  static constexpr int BANNER = -5;  // tap the banner to edit its picture / phrase

 private:
  static void screenFn(UiScreen& screen, void* user);
  static void onAction(const freeink::ui::ActionEvent& event, void* user);
  bool paint(freeink::ui::Rect rect, int index);
  void draw(UiScreen& screen);
  void drawPortrait(UiScreen& screen);
  GfxRenderer& renderer;
  const std::vector<RecentBook>* books = nullptr;
  CoverPainter painter = nullptr;
  void* owner = nullptr;
  Rect content{};
  int selected = 0;
  int start = 0;        // pinned "continue reading" book (always 0)
  int recentStart = 0;  // first book in the paged "recently opened" strip
  int progress = 0;
  int pending = -1;
  int thumbnailHeight = 0;
  bool showSelection = false;
  uint8_t greetingChoice = 0;
  char progressText[8]{};
  char dateText[32]{};
  char greetingText[128]{};
  char statsText[128]{};
  // Host and props are activity-owned, never on the small render-task stack.
  freeink::ui::BookCardProps card;
  freeink::ui::CoverGridProps grid;
};
#endif
