#pragma once

#include <cstdint>

namespace appVisibility {

// Persisted bit positions: never reorder or reuse IDs, including uncompiled apps.
enum class AppId : uint8_t {
  ReadingStats = 0,
  WeRead = 1,
  Sudoku = 2,
  // Gomoku was retired to reclaim Flash; the bit stays reserved so older saved
  // hiddenAppsMask values keep pointing at the same slot.
  Gomoku = 3,
  ChineseChess = 4,
  Minesweeper = 5,
  Game2048 = 6,
  UglyAvatar = 7,
  Standby = 8,
  AirPage = 9,
  Buddy = 10,
  Sokoban = 11,
  PixelSwitch = 12,
  FileTransfer = 13,
  OpdsBrowser = 14,
  Calculator = 15,
  Woodfish = 16,
  // Vocabulary notebook: words collected from the dictionary card.
  Vocabulary = 17,
  // Quotes notebook: sentences highlighted while reading.
  Quotes = 18,
  // Nutstore (坚果云) WebDAV cloud sync. Slot 19 was the old built-in WebDAV
  // file service; repurposed — that function lives under FileTransfer.
  Nutstore = 19,
  WebDav = Nutstore,  // legacy alias, do not use in new code
  // On-demand weather (Open-Meteo), cached on SD for offline viewing.
  Weather = 20,
  // Offline RSS reader: fetch feeds online, read cached articles offline.
  Rss = 21,
  // Read-only ICS agenda (TickTick / Dida365 subscription URL, entered locally).
  Agenda = 22,
  Count = 23,
};

constexpr uint32_t appBit(const AppId id) { return uint32_t{1} << static_cast<uint8_t>(id); }

constexpr uint32_t DEFAULT_HIDDEN_APPS_MASK =
    appBit(AppId::ChineseChess) | appBit(AppId::Minesweeper) | appBit(AppId::Game2048) | appBit(AppId::UglyAvatar) |
    appBit(AppId::Buddy) | appBit(AppId::Sokoban) | appBit(AppId::PixelSwitch) | appBit(AppId::Woodfish);

static_assert(static_cast<uint8_t>(AppId::Count) <= 32, "app IDs must fit the persisted mask");

}  // namespace appVisibility
