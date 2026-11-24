#pragma once

#include <algorithm>

#include "Rect.h"
#include "images/NsReaderPowerLogo.h"

// Safe, aspect-preserving geometry for the embedded NSReader artwork and text.
namespace RickyPowerLayout {
struct Layout {
  Rect mark;
  Rect message;
  Rect footer;
};

constexpr Layout fit(const Rect safe, const int textHeight, const int gap) {
  const int padding = std::max(12, gap);
  const int footerHeight = textHeight * 2;
  const int available = std::max(0, safe.height - footerHeight - padding * 3);
  const int maxMarkW = std::max(0, safe.width - padding * 2);
  const int maxMarkH = std::max(0, available - padding * 2 - textHeight * 2);
  int markW = std::min(maxMarkW, maxMarkH * NsReaderPowerLogo::width / NsReaderPowerLogo::height);
  int markH = markW * NsReaderPowerLogo::height / NsReaderPowerLogo::width;
  if (markH > maxMarkH) {
    markH = maxMarkH;
    markW = markH * NsReaderPowerLogo::width / NsReaderPowerLogo::height;
  }
  const int blockHeight = markH + padding * 2 + textHeight * 2;
  const int top = safe.y + std::max(0, (available - blockHeight) / 2);
  return {Rect{safe.x + (safe.width - markW) / 2, top, markW, markH},
          Rect{safe.x, top + markH + padding * 2, safe.width, textHeight * 2},
          Rect{safe.x, safe.y + safe.height - footerHeight - padding, safe.width, footerHeight}};
}
}  // namespace RickyPowerLayout
