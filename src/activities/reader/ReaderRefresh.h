#pragma once
#include <CrossPointSettings.h>

#include <algorithm>
#include <GfxRenderer.h>

namespace ReaderUtils {
inline HalDisplay::RefreshMode consumeRefreshMode(int& pagesUntilFullRefresh, bool deferClean = false) {
  // The reader's clean cadence is now driven ONLY by the explicit "refresh
  // frequency" setting (1/5/10/15/30/never). Previously the four display presets
  // silently clamped it with std::min (HD -> 5, FAST -> 10), so choosing 15 or
  // "never" appeared to do nothing while the default preset is FAST. That was
  // the reported bug: a 10-page full clean regardless of the setting.
  // Now the chosen page count is authoritative:
  //   - 1/5/10/15/30 -> a full GC16 clean at exactly that interval (user still
  //     has every "full refresh" option and can pick one any time).
  //   - "never" (INT_MAX) -> the counter never reaches the threshold, so no
  //     full clean ever fires; every turn stays on the flash-free differential
  //     waveform, giving the same self-cleaning, no-black-flash behaviour the
  //     navigation bar now has. Ghost residue is light and tolerated.
  // The four display presets still drive every non-reader screen; the reader
  // deliberately decouples so the explicit page-count setting always wins.
  int cadence = SETTINGS.getRefreshFrequency();
  if (cadence < 1) cadence = 1;

  // The clean is FULL on Read Pico (GL16 never pre-blackens, so it cannot reset
  // the accumulated gray floor; GC16 does). Other panels keep HALF for a lighter
  // clean. Deferred cleans are capped at ONE turn so a long transition sequence
  // can never postpone the reset indefinitely -- the main ghosting fix for FAST.
  if (deferClean && pagesUntilFullRefresh <= 2) {
    pagesUntilFullRefresh = 2;  // allow at most one more deferred turn, then clean
    return HalDisplay::FAST_REFRESH;
  }
#if FREEINK_DEVICE_READPICO
  const auto cleanMode = HalDisplay::FULL_REFRESH;
#else
  using Mode = CrossPointSettings::REFRESH_MODE;
  const auto cleanMode = SETTINGS.getRefreshMode() == Mode::REFRESH_MODE_FASTEST
                             ? HalDisplay::HALF_REFRESH
                             : HalDisplay::FULL_REFRESH;
#endif
  const auto mode = (pagesUntilFullRefresh <= 1) ? cleanMode : HalDisplay::FAST_REFRESH;
  if (pagesUntilFullRefresh <= 1) {
    pagesUntilFullRefresh = cadence;
  } else {
    pagesUntilFullRefresh--;
  }
  return mode;
}

// One helper, blocking or deferred: the async form starts the refresh and
// returns so the caller can overlap CPU work with the panel's refresh time.
// Async callers must not touch the framebuffer until
// renderer.waitRefreshComplete() and must rebuild the differential baseline
// before the next page turn (the tiled grayscale cleanup does).
inline void displayWithRefreshCycle(const GfxRenderer& renderer, int& pagesUntilFullRefresh, bool async = false) {
  const auto mode = consumeRefreshMode(pagesUntilFullRefresh);
  if (async) {
    renderer.displayBufferAsync(mode, DisplayRefreshContext::ContinuousReading);
  } else {
    renderer.displayBuffer(mode, DisplayRefreshContext::ContinuousReading);
  }
}

// Display the B/W base of a page whose grayscale pass follows. Panels that
// combine the base (Paper Mono) defer the activation so base + gray planes go
// out as one waveform — displaying the base separately makes the gray pass
// re-drive the whole text body (a visible flash). Other panels display
// normally. Same refresh-cadence bookkeeping as displayWithRefreshCycle.
inline void displayBaseWithRefreshCycle(const GfxRenderer& renderer, int& pagesUntilFullRefresh, bool manualRefresh) {
  if (!renderer.supportsTextOnlyCombinedBase()) {
    displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
    return;
  }
  if (renderer.supportsReaderTransitions()) renderer.waitRefreshComplete();
  const bool transition = !manualRefresh && renderer.canUseTextTransition();
  HalDisplay::RefreshMode mode;
  if (transition && pagesUntilFullRefresh == 0) {
    // A trusted entry starts the cycle; a real due counter (1) keeps its debt.
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    mode = HalDisplay::FAST_REFRESH;
  } else {
    mode = consumeRefreshMode(pagesUntilFullRefresh, transition);
  }
#if FREEINK_DEVICE_READPICO
  // Read Pico's anti-aliased text turn is GL16 (37 phases, ~415 ms of scan): it
  // is the lightest differential profile that still carries text mid-tones -- DU
  // at 20 phases leaves them flat -- and it is what the ~600 ms cached turn is
  // built on. Only the ordinary turn is promoted; a due clean keeps whatever the
  // cadence chose, so a real reset still happens on schedule.
  if (mode == HalDisplay::FAST_REFRESH) mode = HalDisplay::HALF_REFRESH;
#endif
  renderer.displayGrayscaleBase(mode, DisplayRefreshContext::TextOnlyAntiAliasing);
}
}  // namespace ReaderUtils
