#ifdef RICKYOS_PRODUCT
#include "LibraryCollageFace.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

#include "CrossPointSettings.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "StandbyCoverPath.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Geometry ported from kiikoread's book_lock_collage.c, scaled to the logical
// screen so it works at any panel size. kiikoread designed for 684x1216.
constexpr float kDesignH = 1216.f;
constexpr float kCos15 = 0.9659258263f;
constexpr float kSin15 = 0.2588190451f;
constexpr float kGapRatio = 18.f / 1216.f;
constexpr float kCoverHeightRatio = 334.f / 1216.f;
constexpr float kRadiusRatio = 14.f / 1216.f;

// 4x4 Bayer matrix, identical to kiikoread's ui_image_dither_gray, mapping an
// 8-bit gray to a 16-level (17*k) value at the final screen coordinate.
uint8_t ditherGray(unsigned gray, int x, int y) {
  static const uint8_t bayer4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  const unsigned base = gray / 17u;
  if (base >= 15u) return 255;
  const unsigned remainder = gray - base * 17u;
  const unsigned threshold = bayer4[y & 3][x & 3];
  const unsigned bump = remainder * 16u > threshold * 17u ? 1u : 0u;
  return static_cast<uint8_t>((base + bump) * 17u);
}

struct Placement {
  float x = 0, y = 0, w = 0;  // x,y relative to screen centre; y from top-left of unrotated cover
  int index = 0;
};

struct CoverGray {
  std::unique_ptr<uint8_t[]> data;
  int width = 0;
  int height = 0;
  explicit operator bool() const { return data != nullptr && width > 0 && height > 0; }
};

// Load a book cover BMP as an 8-bit gray buffer (0=black .. 255=white), undithered.
CoverGray loadCoverGray(const std::string& path, const int targetH) {
  CoverGray out;
  if (path.empty()) return out;
  HalFile file;
  if (!Storage.openFileForRead("COLLAGE", path, file)) return out;
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return out;
  const int srcW = bitmap.getWidth();
  const int srcH = bitmap.getHeight();
  if (srcW <= 0 || srcH <= 0 || srcW > 4096 || srcH > 4096) return out;
  // Downscale to at most a cover-height sample to bound PSRAM use (15 covers are
  // loaded one at a time, but a single huge scan would still be wasteful).
  const int scale = std::max(1, (srcH + targetH * 2 - 1) / (targetH * 2));
  const int w = std::max(1, srcW / scale);
  const int h = std::max(1, srcH / scale);
  auto gray = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[static_cast<size_t>(w) * h]);
  if (!gray) return out;
  const size_t rowBytes = static_cast<size_t>(bitmap.getRowBytes());
  auto row = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[rowBytes + static_cast<size_t>(bitmap.getWidth())]);
  if (!row) return out;
  uint8_t* source = row.get() + rowBytes;
  bitmap.rewindToData();
  // readNextRow consumes one BMP row at a time. Walk all source rows in order,
  // keeping only the sampled ones (every `scale` rows) to bound memory.
  for (int srcY = 0; srcY < srcH; ++srcY) {
    const BmpReaderError err = bitmap.readNextRow(row.get(), source, nullptr, Bitmap::RowOutput::Gray8);
    if (err != BmpReaderError::Ok) return out;
    if (srcY % scale != 0) continue;
    const int destRow = srcY / scale;
    if (destRow >= h) break;
    const int destY = bitmap.isTopDown() ? destRow : h - 1 - destRow;
    for (int x = 0; x < w; ++x) {
      const int srcX = std::min(bitmap.getWidth() - 1, x * scale);
      gray[static_cast<size_t>(destY) * w + x] = source[srcX];
    }
  }
  out.data = std::move(gray);
  out.width = w;
  out.height = h;
  return out;
}

inline bool insideCover(float x, float y, float w, float h, float radius) {
  if (x < 0 || y < 0 || x >= w || y >= h) return false;
  const float r = std::min(radius, std::min(w, h) / 2.f);
  const float cx = std::max(r - x, x - (w - r));
  const float cy = std::max(r - y, y - (h - r));
  return cx <= 0 || cy <= 0 || cx * cx + cy * cy <= r * r;
}

uint8_t sampleCover(const CoverGray& cover, float sx, float sy) {
  sx = std::clamp(sx, 0.f, cover.width - 1.f);
  sy = std::clamp(sy, 0.f, cover.height - 1.f);
  const unsigned x = static_cast<unsigned>(sx), y = static_cast<unsigned>(sy);
  const unsigned nx = x + 1 < static_cast<unsigned>(cover.width) ? x + 1 : x;
  const unsigned ny = y + 1 < static_cast<unsigned>(cover.height) ? y + 1 : y;
  const float fx = sx - x, fy = sy - y;
  const uint8_t* d = cover.data.get();
  const float a = d[static_cast<size_t>(y) * cover.width + x] * (1.f - fx) +
                  d[static_cast<size_t>(y) * cover.width + nx] * fx;
  const float b = d[static_cast<size_t>(ny) * cover.width + x] * (1.f - fx) +
                  d[static_cast<size_t>(ny) * cover.width + nx] * fx;
  return static_cast<uint8_t>(a * (1.f - fy) + b * fy + .5f);
}

bool paleCover(const CoverGray& cover) {
  unsigned sum = 0;
  for (unsigned i = 0; i < 32; ++i) {
    const unsigned x = i * (cover.width - 1) / 31, y = i * (cover.height - 1) / 31;
    sum += cover.data[static_cast<size_t>(x)] + cover.data[static_cast<size_t>((cover.height - 1)) * cover.width + x] +
           cover.data[static_cast<size_t>(y) * cover.width] +
           cover.data[static_cast<size_t>(y) * cover.width + cover.width - 1];
  }
  return sum >= 128u * 200u;
}

inline void ditherPixel(GfxRenderer& renderer, int x, int y, unsigned gray) {
  renderer.drawGrayscale16Pixel(x, y, ditherGray(gray, x, y));
}
}  // namespace

void LibraryCollageFace::onEnter() {
  books_.clear();
  totalCount_ = 0;
  // Gather real covers from the reading-stats shelf, most-recently-read first,
  // capped at 15 like kiikoread. Use the multi-gray cover.bmp (2-bit/4-gray),
  // never the 1-bit noise-dithered thumb, which turns to gray mush when sampled.
  const auto& stats = READING_STATS.getBooks();
  for (auto it = stats.rbegin(); it != stats.rend() && books_.size() < 15; ++it) {
    if (it->coverBmpPath.empty() && it->path.empty()) continue;
    const std::string resolved = standbyCover::coverBmpForBook(it->path, it->coverBmpPath);
    if (resolved.empty() || !Storage.exists(resolved.c_str())) continue;
    CollageBook b;
    b.title = it->title;
    b.author = it->author;
    b.coverPath = resolved;
    books_.push_back(std::move(b));
  }
  // Reading stats may be empty while the recent list still has covers.
  if (books_.empty()) {
    for (const auto& rb : RECENT_BOOKS.getBooks()) {
      if (rb.coverBmpPath.empty() && rb.path.empty()) continue;
      const std::string resolved = standbyCover::coverBmpForBook(rb.path, rb.coverBmpPath);
      if (resolved.empty() || !Storage.exists(resolved.c_str())) continue;
      CollageBook b;
      b.title = rb.title;
      b.author = rb.author;
      b.coverPath = resolved;
      books_.push_back(std::move(b));
      if (books_.size() >= 15) break;
    }
  }
  totalCount_ = static_cast<int>(std::max(stats.size(), RECENT_BOOKS.getBooks().size()));
}

StandbyFace::TickResult LibraryCollageFace::tick() { return TickResult::None; }

bool LibraryCollageFace::renderNative(GfxRenderer& renderer, const Rect& /*viewport*/) {
  if (renderer.getGrayscaleLevels() != 16 || books_.empty()) return false;
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const float cx = screenW / 2.f;
  const float cy = screenH / 2.f;
  const float coverH = screenH * kCoverHeightRatio;
  const float gap = screenH * kGapRatio;
  const float radius = screenH * kRadiusRatio;

  // Probe cover aspect ratios from BMP headers first (cheap), so layout matches
  // kiikoread's two-pass approach without decoding every cover twice.
  std::vector<float> aspect(books_.size(), 0.7f);
  for (size_t i = 0; i < books_.size(); ++i) {
    HalFile file;
    if (!Storage.openFileForRead("COLLAGE", books_[i].coverPath, file)) continue;
    Bitmap bmp(file);
    if (bmp.parseHeaders() == BmpReaderError::Ok && bmp.getHeight() > 0) {
      aspect[i] = static_cast<float>(bmp.getWidth()) / static_cast<float>(bmp.getHeight());
    }
  }

  // 6 rows x 5 cols, 15 books cycled (matches kiikoread's make_placements).
  Placement placements[30];
  unsigned count = 0;
  static const unsigned starts[6] = {10, 5, 0, 10, 5, 0};
  const unsigned used = static_cast<unsigned>(books_.size());
  for (unsigned row = 0; row < 6; ++row) {
    float width = gap * 4.f;
    for (unsigned col = 0; col < 5; ++col) {
      const unsigned i = (starts[row] + col) % used;
      width += coverH * aspect[i];
    }
    float x = -width / 2.f;
    // Slight per-row stagger, same as the source.
    x += (row == 1 ? 35.f : row == 2 ? -18.f : row == 3 ? 36.f : 0.f) * (screenH / kDesignH);
    for (unsigned col = 0; col < 5; ++col) {
      const unsigned i = (starts[row] + col) % used;
      const float w = coverH * aspect[i];
      placements[count++] = {x, -coverH * 2.38f + row * (coverH + gap), w, static_cast<int>(i)};
      x += w + gap;
    }
  }

  if (!renderer.beginGrayscale16()) return false;
  // White background.
  renderer.clearScreen();

  // Soft shadows first (all of them), then covers in book order (later writes win).
  const float offsetX = 3.f * (screenH / kDesignH), offsetY = 6.f * (screenH / kDesignH);
  const float spread = 11.f * (screenH / kDesignH);
  const auto drawShadow = [&](const Placement& p) {
    const float x0 = cx + kCos15 * p.x - kSin15 * p.y + offsetX;
    const float y0 = cy + kSin15 * p.x + kCos15 * p.y + offsetY;
    const int left = static_cast<int>(std::clamp(std::floor(x0 - kSin15 * coverH - spread), 0.f, float(screenW)));
    const int right = static_cast<int>(std::clamp(std::ceil(x0 + kCos15 * p.w + spread), 0.f, float(screenW)));
    const int top = static_cast<int>(std::clamp(std::floor(y0 - spread), 0.f, float(screenH)));
    const int bottom = static_cast<int>(
        std::clamp(std::ceil(y0 + kSin15 * p.w + kCos15 * coverH + spread), 0.f, float(screenH)));
    const float r = std::min(radius, p.w / 2.f);
    for (int y = top; y < bottom; ++y) {
      for (int x = left; x < right; ++x) {
        const float dx = x + .5f - x0, dy = y + .5f - y0;
        const float u = kCos15 * dx + kSin15 * dy, v = -kSin15 * dx + kCos15 * dy;
        const float qx = std::fabs(u - p.w / 2.f) - (p.w / 2.f - r);
        const float qy = std::fabs(v - coverH / 2.f) - (coverH / 2.f - r);
        const float ax = qx > 0.f ? qx : 0.f, ay = qy > 0.f ? qy : 0.f;
        const float outer = qx > qy ? qx : qy, inner = outer < 0.f ? outer : 0.f;
        const float distance = std::sqrt(ax * ax + ay * ay) + inner - r;
        if (distance >= spread) continue;
        const float fade = distance <= 0.f ? 1.f : (spread - distance) / spread;
        const unsigned tone = static_cast<unsigned>(255.f - 46.f * fade * fade);
        ditherPixel(renderer, x, y, tone);
      }
    }
  };
  for (unsigned p = 0; p < count; ++p) drawShadow(placements[p]);

  // Draw covers, one decoded cover shared by all its placements.
  for (size_t bi = 0; bi < books_.size(); ++bi) {
    bool visible = false;
    for (unsigned p = 0; p < count; ++p) {
      if (placements[p].index != static_cast<int>(bi)) continue;
      const Placement& q = placements[p];
      const float x0 = cx + kCos15 * q.x - kSin15 * q.y;
      const float y0 = cy + kSin15 * q.x + kCos15 * q.y;
      const int left = static_cast<int>(std::clamp(std::floor(x0 - kSin15 * coverH), 0.f, float(screenW)));
      const int right = static_cast<int>(std::clamp(std::ceil(x0 + kCos15 * q.w), 0.f, float(screenW)));
      const int top = static_cast<int>(std::clamp(std::floor(y0), 0.f, float(screenH)));
      const int bottom = static_cast<int>(
          std::clamp(std::ceil(y0 + kSin15 * q.w + kCos15 * coverH), 0.f, float(screenH)));
      if (left < right && top < bottom) { visible = true; break; }
    }
    if (!visible) continue;

    const CoverGray cover = loadCoverGray(books_[bi].coverPath, static_cast<int>(coverH));
    if (!cover) continue;
    const bool outline = paleCover(cover);
    for (unsigned p = 0; p < count; ++p) {
      if (placements[p].index != static_cast<int>(bi)) continue;
      const Placement& q = placements[p];
      const float x0 = cx + kCos15 * q.x - kSin15 * q.y;
      const float y0 = cy + kSin15 * q.x + kCos15 * q.y;
      const int left = static_cast<int>(std::clamp(std::floor(x0 - kSin15 * coverH), 0.f, float(screenW)));
      const int right = static_cast<int>(std::clamp(std::ceil(x0 + kCos15 * q.w), 0.f, float(screenW)));
      const int top = static_cast<int>(std::clamp(std::floor(y0), 0.f, float(screenH)));
      const int bottom = static_cast<int>(
          std::clamp(std::ceil(y0 + kSin15 * q.w + kCos15 * coverH), 0.f, float(screenH)));
      for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
          const float dx = x + .5f - x0, dy = y + .5f - y0;
          const float u = kCos15 * dx + kSin15 * dy, v = -kSin15 * dx + kCos15 * dy;
          if (!insideCover(u, v, q.w, coverH, radius)) continue;
          unsigned tone = sampleCover(cover, u * cover.width / q.w - .5f,
                                      v * cover.height / coverH - .5f);
          if (outline) {
            const float inset = 1.3f * (screenH / kDesignH);
            const float r = std::min(radius, q.w / 2.f);
            const float cxr = std::max(r - u, u - (q.w - r));
            const float cyr = std::max(r - v, v - (coverH - r));
            if (u < inset || v < inset || u >= q.w - inset || v >= coverH - inset ||
                (cxr > 0 && cyr > 0 &&
                 cxr * cxr + cyr * cyr >= (r - inset) * (r - inset))) {
              tone = 112;
            }
          }
          ditherPixel(renderer, x, y, tone);
        }
      }
    }
  }

  // Paper sticker title (top-left) and count (bottom-right), drawn on the B/W
  // proxy then copied into the 16-gray frame as a solid white badge + black ink.
  const int titleFont = NOTOSANS_18_FONT_ID;
  const int labelFont = UI_10_FONT_ID;
  const int pad = 16;
  {
    const char* title = tr(STR_RICKY_COLLAGE_TITLE);
    const int tw = renderer.getTextWidth(titleFont, title);
    const int th = renderer.getLineHeight(titleFont);
    const int x = 24, y = 28;
    renderer.fillRoundedRect(x, y, tw + 2 * pad, th + 2 * pad, radius, Color::White);
    renderer.drawText(titleFont, x + pad, y + pad, title, true);
    renderer.copyBwToGrayscale16(x, y, tw + 2 * pad, th + 2 * pad, static_cast<int>(radius));
  }
  {
    char count[24];
    snprintf(count, sizeof(count), "%d", totalCount_);
    const int cw = renderer.getTextWidth(titleFont, count);
    const int unitW = renderer.getTextWidth(labelFont, tr(STR_RICKY_COLLAGE_UNIT));
    const int ch = renderer.getLineHeight(titleFont);
    const int w = cw + unitW + 3 * pad;
    const int h = ch + 2 * pad;
    const int x = screenW - 24 - w, y = screenH - 28 - h;
    renderer.fillRoundedRect(x, y, w, h, radius, Color::White);
    renderer.drawText(titleFont, x + pad, y + pad, count, true);
    renderer.drawText(labelFont, x + pad + cw + pad, y + pad + (ch - renderer.getLineHeight(labelFont)),
                      tr(STR_RICKY_COLLAGE_UNIT), true);
    renderer.copyBwToGrayscale16(x, y, w, h, static_cast<int>(radius));
  }

  renderer.commitGrayscale16();
  renderer.cancelGrayscale16();
  return true;
}

void LibraryCollageFace::render(GfxRenderer& renderer, const Rect& viewport) {
  // B/W fallback on panels without native 16-gray: a simple labelled rest screen.
  const int font = UI_10_FONT_ID;
  UITheme::drawCenteredWrappedText(renderer, viewport, font, tr(STR_RICKY_COLLAGE_TITLE), 2);
}

StrId LibraryCollageFace::titleId() const { return StrId::STR_RICKY_STANDBY_STYLE_LIBRARY_COLLAGE; }

uint32_t LibraryCollageFace::secondsUntilNextWake() const { return 3600u; }
#endif
