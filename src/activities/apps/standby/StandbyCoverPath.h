#pragma once
#ifdef RICKYOS_PRODUCT

#include <string>

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Xtc.h>

#include "components/UITheme.h"

namespace standbyCover {

// Return the multi-gray cover BMP for a book, NOT the 1-bit dithered thumb.
// cover.bmp is 2-bit/4-gray (XTC) or Gray2 (EPUB); thumb_[HEIGHT].bmp is 1-bit
// noise-dithered and looks like gray mush once sampled/rotated for a collage.
inline std::string coverBmpForBook(const std::string& bookPath, const std::string& storedCoverPath) {
  if (!bookPath.empty()) {
    if (FsHelpers::hasXtcExtension(bookPath)) {
      const Xtc xtc(bookPath, "/.crosspoint");
      const std::string p = xtc.getCoverBmpPath();
      if (!p.empty() && Storage.exists(p.c_str())) return p;
    } else if (FsHelpers::hasReflowableBookExtension(bookPath)) {
      const Epub epub(bookPath, "/.crosspoint");
      const std::string p = epub.getCoverBmpPath();
      if (!p.empty() && Storage.exists(p.c_str())) return p;
    }
  }
  // Fall back to the stored path. If it is a thumb placeholder, resolve it, but
  // prefer stripping to a full cover name when recognizable.
  std::string resolved = UITheme::getCoverThumbPath(storedCoverPath, 344);
  const size_t thumb = resolved.find("/thumb_");
  if (thumb != std::string::npos) {
    std::string candidate = resolved.substr(0, thumb) + "/cover_legacy_v2.bmp";
    if (Storage.exists(candidate.c_str())) return candidate;
    candidate = resolved.substr(0, thumb) + "/cover.bmp";
    if (Storage.exists(candidate.c_str())) return candidate;
  }
  return resolved;
}

}  // namespace standbyCover

#endif
