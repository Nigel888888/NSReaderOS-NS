#ifdef RICKYOS_PRODUCT
#include "ReadingReceiptFace.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "ReadingStatsStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Design coordinates are 684x1216 (portrait). Scale everything to the logical
// screen so the receipt stays proportional on any panel.
constexpr float kDesignW = 684.f;
constexpr float kDesignH = 1216.f;

// Torn-paper notch: parabolic bite removed from the top and bottom edges.
// Input/output are in design coordinates (684-wide space).
int notchDepth(int x) {
  const int radius = 18, first = 80, step = 64;
  int dx = (x - first) % step;
  if (dx < 0) dx += step;
  if (dx > step / 2) dx -= step;
  if (dx < -radius || dx > radius) return 0;
  return radius - dx * dx / radius;
}

void formatDuration(uint64_t ms, char* out, size_t n) {
  const uint64_t totalMin = ms / 60000u;
  const uint64_t h = totalMin / 60u;
  const uint64_t m = totalMin % 60u;
  if (h > 0) {
    snprintf(out, n, "%llu:%02llu", static_cast<unsigned long long>(h), static_cast<unsigned long long>(m));
  } else {
    snprintf(out, n, "%llu min", static_cast<unsigned long long>(m));
  }
}

// Draw a wrapped title left-aligned, UTF-8 safe, bounded to maxLines rows.
// Returns the number of lines actually drawn.
int drawTitle(GfxRenderer& renderer, int fontId, int x, int top, int maxWidth, int maxLines, int lineHeight,
              const char* text) {
  auto lines = renderer.wrappedText(fontId, text, maxWidth, maxLines);
  int y = top;
  for (const auto& line : lines) {
    renderer.drawText(fontId, x, y, line.c_str(), true);
    y += lineHeight;
  }
  return static_cast<int>(lines.size());
}
}  // namespace

void ReadingReceiptFace::onEnter() { lastDay_ = -1; }

StandbyFace::TickResult ReadingReceiptFace::tick() {
  // Refresh once a day so "today's reading" stays current during a long standby.
  std::tm local{};
  if (halClock.localTime(local) && local.tm_yday != lastDay_) return TickResult::Redraw;
  return TickResult::None;
}

void ReadingReceiptFace::render(GfxRenderer& renderer, const Rect& viewport) {
  const float sx = viewport.width / kDesignW;
  const float sy = viewport.height / kDesignH;
  const auto X = [&](float v) { return viewport.x + static_cast<int>(std::lround(v * sx)); };
  const auto Y = [&](float v) { return viewport.y + static_cast<int>(std::lround(v * sy)); };
  const auto W = [&](float v) { return std::max(1, static_cast<int>(std::lround(v * sx))); };
  const auto H = [&](float v) { return std::max(1, static_cast<int>(std::lround(v * sy))); };

  // ---- data ----
  const std::string path = APP_STATE.openEpubPath;
  const ReadingBookStats* stats = path.empty() ? nullptr : READING_STATS.findMatchingBookForPath(path);
  std::string title = stats ? stats->title : std::string();
  std::string author = stats ? stats->author : std::string();
  if (title.empty() && !path.empty()) {
    // Fall back to the file name without extension.
    const size_t slash = path.find_last_of('/');
    title = path.substr(slash == std::string::npos ? 0 : slash + 1);
    const size_t dot = title.find_last_of('.');
    if (dot != std::string::npos) title.erase(dot);
  }
  if (title.empty()) title = tr(STR_RICKY_TICKET_UNTITLED);
  const int percent = stats ? stats->lastProgressPercent : 0;
  const std::string chapter = stats ? stats->chapterTitle : std::string();
  const uint64_t todayMs = READING_STATS.getTodayReadingMs();
  const uint64_t totalMs = stats ? stats->totalReadingMs : READING_STATS.getTotalReadingMs();

  std::tm local{};
  const bool clockValid = halClock.localTime(local);
  if (clockValid) lastDay_ = local.tm_yday;

  // ---- background: dithered cover shade + shadow + paper ----
  // A faint gray wash suggests the deep background of the original without a
  // costly full grayscale frame; use a dithered mid tone.
  renderer.fillRectDither(viewport.x, viewport.y, viewport.width, viewport.height, Color::DarkGray);

  const int left = X(48), right = X(636), top = Y(58), bottom = Y(1158);
  // Shadow (offset down-right, light gray dither).
  for (int x = left; x < right; ++x) {
    const int cut = static_cast<int>(std::lround(notchDepth(static_cast<int>(std::lround((x - left) / sx))) * sy));
    const int y0 = top + cut + H(8), y1 = bottom - cut + H(8);
    if (y1 > y0) renderer.fillRectDither(x + W(5), y0, 1, y1 - y0, Color::LightGray);
  }
  // Paper slip: white column per x between the jagged top/bottom cuts, with a
  // single-pixel black tear edge.
  for (int x = left; x < right; ++x) {
    const int cut = static_cast<int>(std::lround(notchDepth(static_cast<int>(std::lround((x - left) / sx))) * sy));
    const int y0 = top + cut, y1 = bottom - cut;
    if (y1 <= y0) continue;
    renderer.fillRect(x, y0, 1, y1 - y0, false);  // white paper
    renderer.drawPixel(x, y0);
    renderer.drawPixel(x, y1 - 1);
  }
  // Side rules.
  renderer.fillRect(left, top + H(20), 1, (bottom - top) - H(40), true);
  renderer.fillRect(right - 1, top + H(20), 1, (bottom - top) - H(40), true);

  // ---- header + rules ----
  const int contentX = X(82);
  const int contentW = W(520);
  const int fontTitle = NOTOSANS_18_FONT_ID;
  const int fontBody = UI_12_FONT_ID;
  const int fontSmall = UI_10_FONT_ID;
  const int titleLH = renderer.getLineHeight(fontTitle);
  const int smallLH = renderer.getLineHeight(fontSmall);

  renderer.drawText(fontSmall, contentX, Y(98), tr(STR_RICKY_TICKET_HEADER), true);
  renderer.drawLine(contentX, Y(156), contentX + contentW, Y(156), 1, true);

  // ---- title / author ----
  const int usedLines = std::clamp(drawTitle(renderer, fontTitle, contentX, Y(202), contentW, 2, titleLH,
                                             title.c_str()),
                                   1, 2);
  if (!author.empty()) {
    const int authorY = (usedLines > 1) ? Y(326) : Y(274);
    renderer.drawText(fontSmall, contentX, authorY, renderer.truncatedText(fontSmall, author.c_str(), contentW).c_str(),
                      true);
  }

  // ---- cover box ----
  {
    const int boxX = X(216), boxY = Y(378), boxW = W(252), boxH = H(344);
    bool drawn = false;
    if (!stats || !stats->coverBmpPath.empty()) {
      const std::string coverPath = stats ? UITheme::getCoverThumbPath(stats->coverBmpPath, 344) : std::string();
      if (!coverPath.empty()) {
        HalFile file;
        if (Storage.openFileForRead("RECEIPT", coverPath, file)) {
          Bitmap bitmap(file, true);
          if (bitmap.parseHeaders() == BmpReaderError::Ok) {
            renderer.drawBitmap(bitmap, boxX, boxY, boxW, boxH);
            drawn = true;
          }
        }
      }
    }
    if (!drawn) {
      renderer.fillRect(boxX, boxY, boxW, boxH, false);
      renderer.drawText(fontSmall, boxX + W(10), boxY + boxH / 2 - smallLH / 2,
                        renderer.truncatedText(fontSmall, title.c_str(), boxW - W(20)).c_str(), true);
    }
    renderer.drawRoundedRect(boxX, boxY, boxW, boxH, 2, 0, true);
  }

  renderer.drawLine(contentX, Y(768), contentX + contentW, Y(768), 1, true);

  // ---- progress ----
  {
    char line[200];
    if (stats) {
      if (!chapter.empty())
        snprintf(line, sizeof(line), tr(STR_RICKY_TICKET_PROGRESS_CHAPTER), percent, chapter.c_str());
      else
        snprintf(line, sizeof(line), tr(STR_RICKY_TICKET_PROGRESS), percent);
    } else {
      snprintf(line, sizeof(line), "%s", tr(STR_RICKY_TICKET_PROGRESS_NONE));
    }
    renderer.drawText(fontBody, contentX, Y(802), renderer.truncatedText(fontBody, line, contentW).c_str(), true);

    const int trackY = Y(868), trackH = H(10);
    renderer.fillRectDither(contentX, trackY, contentW, trackH, Color::LightGray);
    const int filled = stats ? contentW * percent / 100 : 0;
    if (filled > 0) renderer.fillRect(contentX, trackY, filled, trackH, true);
  }

  // ---- today / lifetime ----
  {
    char timeStr[64];
    char line[200];
    if (clockValid) {
      formatDuration(todayMs, timeStr, sizeof(timeStr));
      snprintf(line, sizeof(line), tr(STR_RICKY_TICKET_TODAY), timeStr);
    } else {
      snprintf(line, sizeof(line), "%s", tr(STR_RICKY_TICKET_DATE_UNSET));
    }
    renderer.drawText(fontSmall, contentX, Y(914), renderer.truncatedText(fontSmall, line, contentW).c_str(), true);

    formatDuration(totalMs, timeStr, sizeof(timeStr));
    snprintf(line, sizeof(line), tr(STR_RICKY_TICKET_TOTAL), timeStr);
    renderer.drawText(fontSmall, contentX, Y(969), renderer.truncatedText(fontSmall, line, contentW).c_str(), true);
  }

  renderer.drawLine(contentX, Y(1050), contentX + contentW, Y(1050), 1, true);
  renderer.drawText(fontSmall, contentX, Y(1080), tr(STR_RICKY_TICKET_SLOGAN), true);
}

StrId ReadingReceiptFace::titleId() const { return StrId::STR_RICKY_STANDBY_STYLE_RECEIPT; }

uint32_t ReadingReceiptFace::secondsUntilNextWake() const { return 3600u; }
#endif
