#pragma once
#ifdef RICKYOS_PRODUCT

#include <cstdint>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"

// RickyOS reading statistics: one page, no menu. Today against the daily goal with
// the logo's dog reacting to it, this week's days, three totals and one small fact
// that changes daily, all from ReadingStatsStore. A small card at the bottom shows
// a random sentence from the quotes notebook; tapping its star draws another one.
class RickyReadingStatsActivity final : public UiListActivity {
 public:
  RickyReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& input)
      : UiListActivity("RickyReadingStats", renderer, input) {}

  void onEnter() override;

 protected:
  // This page is opened straight from the Apps grid via replaceActivityWith();
  // finishing would drop back to Home. Return to the Apps grid instead, like
  // the other product apps (Quotes, Vocabulary, RSS, ...).
  void onBackButton() override { activityManager.goToApps(); }
  static constexpr freeink::ui::ActionId ACTION_QUOTE_STAR = ACTION_USER;
  int listCount() const override { return 0; }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int) override {}

 private:
  void loadQuotes();
  void nextQuote();
  static void onQuoteStar(const freeink::ui::ActionEvent&, void* user);

  std::vector<std::string> quotes_;
  size_t quoteIndex_ = 0;
  Rect starRect_{};
};

#endif
