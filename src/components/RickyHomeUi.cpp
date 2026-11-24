#include "RickyHomeUi.h"

#ifdef RICKYOS_PRODUCT
#include <FsHelpers.h>
#include <HalClock.h>
#include <HalPowerManager.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "ReadingStatsStore.h"
#include "RickyHomeLayout.h"
#include "RickyPageUi.h"
#include "RickyProfile.h"
#include "UITheme.h"
#include "fontIds.h"
#include "util/TimeUtils.h"

namespace fui = freeink::ui;
namespace {
constexpr fui::ActionId OPEN_BOOK = 1;
constexpr StrId greetingKeys[] = {StrId::STR_RICKY_HOME_GREETING, StrId::STR_RICKY_HOME_GREETING_REST,
                                  StrId::STR_RICKY_HOME_GREETING_SLOW, StrId::STR_RICKY_HOME_GREETING_TIME,
                                  StrId::STR_RICKY_HOME_GREETING_STORY};
static_assert(sizeof(greetingKeys) / sizeof(greetingKeys[0]) == RickyHomeLayout::GREETING_COUNT);
const char* titleOf(const RecentBook& book) { return book.title.empty() ? book.path.c_str() : book.title.c_str(); }
const char* formatOf(const std::string& path) {
  return FsHelpers::hasEpubExtension(path)       ? "EPUB"
         : FsHelpers::hasTxtExtension(path)      ? "TXT"
         : FsHelpers::hasMarkdownExtension(path) ? "MD"
         : FsHelpers::hasXtcExtension(path)      ? "XTC"
                                                 : nullptr;
}
}  // namespace

void RickyHomeUi::begin(const std::vector<RecentBook>& recent, CoverPainter coverPainter, void* activity) {
  books = &recent;
  painter = coverPainter;
  owner = activity;
  // Non-security variety, chosen once per visit. Render/cache refreshes must
  // never change the greeting or cause a timer-driven e-ink refresh.
  static uint8_t previousGreeting = RickyHomeLayout::GREETING_COUNT;
  static uint32_t visit = 0;
  greetingChoice = RickyHomeLayout::chooseGreeting(static_cast<uint32_t>(halClock.nowUtc()) ^ (++visit * 0x9e3779b9U),
                                                   previousGreeting);
  previousGreeting = greetingChoice;
  resetUi();
  app.on(OPEN_BOOK, &RickyHomeUi::onAction, this);
  app.setScreen(&RickyHomeUi::screenFn, this);
}

void RickyHomeUi::configure(Rect bounds, int selection, bool show, int percent, int recent) {
  content = bounds;
  selected = selection;
  // "Continue reading" is pinned to the most recent book; paging only moves
  // the "recently opened" strip below it, so the two blocks are independent.
  start = 0;
  recentStart = recent;
  showSelection = show;
  progress = std::clamp(percent, 0, 100);
  snprintf(progressText, sizeof(progressText), "%d%%", progress);
}

void RickyHomeUi::onAction(const fui::ActionEvent& event, void* user) {
  auto& self = *static_cast<RickyHomeUi*>(user);
  self.pending = event.value;
  self.app.clearTapFlash();
}

int RickyHomeUi::selectedAction(const MappedInputManager& input) {
  pending = -1;
  // Banner editing and the reading calendar are long-press only (see the
  // InputLongPress hits below) so a stray tap on the home page can never open
  // an editor. Forward long-press snapshots to the interaction table; normal
  // taps still route through InputTouch exactly as before.
  const auto touch = routeTouch(input, /*withLongPress=*/true);
  return touch.snap.touchReleased ? pending : -1;
}

bool RickyHomeUi::paint(fui::Rect rect, int index, bool frame) {
  if (painter && painter(owner, index, Rect{rect.x, rect.y, rect.width, rect.height})) {
    // Optional hairline edge: generated covers need it to avoid dissolving into
    // the page, but the portrait "now reading" cover wants artwork only.
    if (frame) uiTarget.stroke(rect, fui::Paint::solid(fui::Color::Black), 1);
    return true;
  }
  if (!books || index < 0 || index >= static_cast<int>(books->size())) return false;
  const auto& book = (*books)[index];
  auto text = grid.titleText;
  text.font = fui::GfxRendererTarget::FONT_SMALL;  // Full common-character coverage for any title.
  if (rect.width > rect.height * 2) {
    RickyPageUi::bookTile(uiTarget, rect, titleOf(book), text);
    return true;
  }
  RickyPageUi::generatedCover(uiTarget, rect, titleOf(book), book.author.c_str(), formatOf(book.path), text);
  return true;
}

void RickyHomeUi::screenFn(UiScreen& screen, void* user) { static_cast<RickyHomeUi*>(user)->draw(screen); }

void RickyHomeUi::draw(UiScreen& screen) {
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(content.y), static_cast<int16_t>(renderer.getScreenWidth() - content.x - content.width),
      static_cast<int16_t>(renderer.getScreenHeight() - content.y - content.height), static_cast<int16_t>(content.x)});
  if (content.height > content.width) {
    drawPortrait(screen);
    return;
  }
  const auto& theme = screen.theme();
  const int gap = std::max(UiHighDpiProfile::controlGap, static_cast<int>(theme.spaceSm));
  const int16_t padding = gap;
  auto heading = theme.bodyText;
  heading.bold = true;
  const int headingHeight = screen.target().lineHeight(heading.font);
  const int labelHeight = screen.target().lineHeight(theme.smallText.font);
  const bool quietPortrait = content.height > content.width;
  const int profileHeight = headingHeight + labelHeight + gap;
  const auto profileRect = screen.takeTop(profileHeight + (quietPortrait ? labelHeight + gap * 2 : 0), gap * 2);
  std::tm local{};
  if (halClock.localTime(local)) {
    snprintf(dateText, sizeof(dateText), "%04d.%02d.%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
  } else {
    snprintf(dateText, sizeof(dateText), "%s", tr(STR_RICKY_DATE_UNSET));
  }
  auto dateStyle = theme.smallText;
  const int avatarSize = std::min<int>(profileHeight, profileRect.width / 4);
  RickyProfile::drawAvatar(renderer, Rect{profileRect.right() - avatarSize, profileRect.y, avatarSize, avatarSize});
  auto textRect = profileRect;
  textRect.width -= avatarSize + gap * 2;
  textRect.height = labelHeight;
  screen.target().text(textRect, dateText, dateStyle);
  textRect.y += labelHeight + gap;
  textRect.height = std::max(0, profileRect.bottom() - textRect.y);
  snprintf(greetingText, sizeof(greetingText), I18N.get(greetingKeys[greetingChoice]), RickyProfile::nickname());
  auto greeting = heading;
  greeting.maxLines = quietPortrait ? 2 : 1;
  screen.target().text(textRect, greetingText, greeting);
  screen.frame().hit(profileRect, OPEN_BOOK, STATISTICS, fui::InputTouch);
  if (quietPortrait) {
    auto phrase = theme.smallText;
    phrase.align = fui::TextAlign::Center;
    phrase.maxLines = 2;
    screen.target().text(screen.takeBottom(labelHeight * 2, gap * 2), SETTINGS.rickyHomePhrase, phrase);
  }
  const auto statsRect = screen.takeBottom(labelHeight + gap * 2, gap * 2);
  snprintf(statsText, sizeof(statsText), tr(STR_RICKY_HOME_STATS),
           static_cast<unsigned>(READING_STATS.getTodayReadingMs() / 60000),
           static_cast<unsigned>(READING_STATS.getCurrentStreakDays()));
  auto statsStyle = theme.smallText;
  statsStyle.align = fui::TextAlign::Center;
  screen.target().text(statsRect, statsText, statsStyle);
  screen.frame().hit(statsRect, OPEN_BOOK, STATISTICS, fui::InputTouch);
  if (!books || books->empty()) {
    auto message = theme.bodyText;
    message.align = fui::TextAlign::Center;
    message.maxLines = 2;
    auto rect = screen.body();
    rect.y += std::max(0, (rect.height - headingHeight * 2) / 2);
    rect.height = headingHeight * 2;
    screen.target().text(rect, tr(STR_NO_RECENT_BOOKS), message);
    screen.frame().hit(rect, OPEN_BOOK, LIBRARY, fui::InputTouch);
    return;
  }
  const auto body = screen.body();
  const auto layout = RickyHomeLayout::fit(body.width, body.height, headingHeight, labelHeight, gap, padding);
  const auto textFit = RickyHomeLayout::fitText(layout.coverHeight, headingHeight, labelHeight, gap, layout.rows == 1);
  thumbnailHeight = layout.coverHeight;
  screen.target().text(screen.takeTop(headingHeight, gap), tr(STR_CONTINUE_READING), heading);
  const auto& book = (*books)[0];
  card = {};
  card.title = book.title.empty() ? book.path.c_str() : book.title.c_str();
  card.author = book.author.empty() || !textFit.author ? nullptr : book.author.c_str();
  card.titleText = theme.bodyText;
  card.titleText.maxLines = textFit.titleLines;
  card.authorText = theme.smallText;
  card.progressText = theme.smallText;
  card.progress = progress;
  card.progressMax = textFit.progress ? 100 : 0;
  card.progressLabel = textFit.progress ? progressText : nullptr;
  card.coverSize =
      fui::Size{static_cast<int16_t>(layout.coverHeight * 2 / 3), static_cast<int16_t>(layout.coverHeight)};
  card.padding = fui::Insets{padding, padding, padding, padding};
  card.gap = gap * 2;
  card.textGap = gap;
  card.centerTextOnCover = true;
  card.progressHeight = 6;
  card.action = OPEN_BOOK;
  card.value = 0;
  card.state = showSelection && selected == 0 ? fui::StateSelected : fui::StateNormal;
  card.styles = theme.listRow;
  card.selectionIndicator = fui::BookCardSelectionIndicator::CoverFrame;
  card.coverPainterUserData = this;
  card.coverPainter = [](fui::DrawTarget&, fui::Rect rect, const fui::BookCardProps&, void* user) {
    auto& self = *static_cast<RickyHomeUi*>(user);
    return self.paint(rect, 0);
  };
  fui::bookCard(screen.frame(), screen.takeTop(layout.rowHeight, gap), card);
  auto recentHeading = screen.takeTop(headingHeight, gap);
  screen.frame().hit(recentHeading, OPEN_BOOK, LIBRARY, fui::InputTouch);
  const int recentCount = std::max(0, static_cast<int>(books->size()) - 1);
  const int pages = std::max(1, (recentCount + RickyHomeLayout::PAGE_BOOKS - 1) / RickyHomeLayout::PAGE_BOOKS);
  if (pages > 1) {
    char pageText[16];
    snprintf(pageText, sizeof(pageText), "%d / %d",
             std::clamp((recentStart - 1) / RickyHomeLayout::PAGE_BOOKS + 1, 1, pages), pages);
    auto pageStyle = theme.smallText;
    pageStyle.align = fui::TextAlign::Right;
    auto pageRect = recentHeading;
    pageRect.width = std::min<int>(recentHeading.width / 3, headingHeight * 3);
    pageRect.x += recentHeading.width - pageRect.width;
    screen.target().text(pageRect, pageText, pageStyle);
    recentHeading.width -= pageRect.width + gap;
  }
  screen.target().text(recentHeading, tr(STR_MENU_RECENT_BOOKS), heading);
  grid = {};
  grid.count = std::min<int>(RickyHomeLayout::PAGE_BOOKS, recentCount - (recentStart - 1));
  grid.columns = RickyHomeLayout::PAGE_BOOKS;
  grid.rowHeight = layout.rowHeight;
  grid.gap = grid.rowGap = gap;
  // Smaller thumbnails so three sit in the strip without dwarfing the card.
  grid.coverSize = fui::Size{static_cast<int16_t>(card.coverSize.width * 2 / 3),
                             static_cast<int16_t>(card.coverSize.height * 2 / 3)};
  grid.cellInset = card.padding;
  grid.labelHeight = labelHeight;
  grid.labelGap = 0;
  grid.titleText = theme.smallText;
  grid.titleText.maxLines = 1;
  grid.cellStyles = theme.listRow;
  grid.minTouchSize = 0;  // Keep hit regions inside drawn cells, never expand into gaps.
  grid.action = OPEN_BOOK;
  grid.inputMask = fui::InputTouch;
  grid.selectedIndex =
      showSelection && selected >= recentStart && selected < recentStart + grid.count ? selected - recentStart : -1;
  grid.selectionIndicator = fui::CoverGridSelectionIndicator::CoverFrame;
  grid.scrollIndicator = false;
  grid.itemProviderUserData = this;
  grid.itemProvider = [](uint16_t index, void* user) {
    auto& self = *static_cast<RickyHomeUi*>(user);
    const int bookIndex = self.recentStart + static_cast<int>(index);
    const auto& item = (*self.books)[bookIndex];
    return fui::coverGridItem(item.title.empty() ? item.path.c_str() : item.title.c_str(), bookIndex);
  };
  grid.coverPainterUserData = this;
  grid.coverPainter = [](fui::DrawTarget&, fui::Rect rect, const fui::CoverGridItem&, uint16_t index, void* user) {
    auto& self = *static_cast<RickyHomeUi*>(user);
    return self.paint(rect, self.recentStart + static_cast<int>(index));
  };
  fui::coverGrid(screen.frame(), screen.body(), grid);
}

// ifink-style home: a custom picture+phrase banner, a weekly reading heatmap,
// and a two-column footer ("now reading" / "recently read"). Geometry scales to
// the current content rect so the page keeps its proportions on any safe area.
void RickyHomeUi::drawPortrait(UiScreen& screen) {
  const auto& theme = screen.theme();
  auto& target = screen.target();
  const int gap = std::max<int>(10, theme.spaceSm);
  const int smallHeight = target.lineHeight(theme.smallText.font);
  screen.insetContent(fui::Insets{static_cast<int16_t>(gap), 6, static_cast<int16_t>(gap), 6});
  // One CJK weight ships; synthetic bold (held for this whole page) supplies hierarchy.
  RickyPageUi::Bold bold(renderer, 1);
  const auto black = fui::Paint::solid(fui::Color::Black);
  const auto rule = fui::Paint::solid(fui::Color::DarkGray);
  auto small = theme.smallText;
  small.maxLines = 1;
  auto right = small;
  right.align = fui::TextAlign::Right;
  auto strong = small;
  strong.bold = true;
  auto heading = theme.titleText;
  heading.bold = true;
  heading.maxLines = 1;

  const int totalHeight = screen.body().height;
  std::tm local{};
  const bool dated = halClock.localTime(local);
  // No status row: the top of the page starts directly with the banner.
  // Statistics remain reachable from the reading-record calendar below.

  // ---- Banner: custom picture + configurable phrase (tap to configure). ----
  // The text panel shares the exact image bounds inset by one uniform margin,
  // so picture and text frames line up regardless of imported artwork.
  int bannerHeight = totalHeight * 25 / 100;
  bannerHeight = std::max<int>(smallHeight * 7, std::min<int>(bannerHeight, totalHeight * 32 / 100));
  const auto banner = screen.takeTop(bannerHeight, gap);
  bool pictured = RickyProfile::drawHomeBanner(renderer, Rect{banner.x, banner.y, banner.width, banner.height});
  if (!pictured) {
    target.fill(banner, fui::Paint::dither(fui::Color::LightGray), 6);
    target.stroke(banner, black, 1, 6);
  }
  // Text panel sits on the right with the SAME uniform inset on every side as
  // the image frame, so its top/bottom/right edges line up with the picture.
  const int bannerInset = gap;
  const int panelX = banner.x + banner.width * 42 / 100;
  const fui::Rect panel{static_cast<int16_t>(panelX), static_cast<int16_t>(banner.y + bannerInset),
                        static_cast<int16_t>(banner.right() - bannerInset - panelX),
                        static_cast<int16_t>(banner.height - bannerInset * 2)};
  // No filled panel behind the phrase: it sits directly on the banner artwork
  // (or on the plain banner background), with no visible box.
  // Phrase typography comes from the persisted banner settings. TextStyle.font
  // is a FONT SLOT (0..3), not a real GfxRenderer font id: writing a raw id here
  // silently falls back to FONT_BODY, which is why the old picker changed
  // nothing. Bind the chosen real font to the LABEL slot for this one draw,
  // then restore the slot so the rest of the page keeps its normal chrome.
  auto* gfxTarget = static_cast<fui::GfxRendererTarget*>(&target);
  const int bannerFontId = SETTINGS.rickyBannerFontId(SETTINGS.rickyBannerFont, SETTINGS.rickyBannerFontSize);
  gfxTarget->setFont(fui::GfxRendererTarget::FONT_LABEL, bannerFontId);
  auto phraseStyle = heading;
  phraseStyle.font = fui::GfxRendererTarget::FONT_LABEL;
  // Right-aligned: the phrase hugs the right edge of the banner panel. Use the
  // SDK's right-aligned wrapper (justifiedWrappedText forces Left + stretching).
  phraseStyle.align = fui::TextAlign::Right;
  phraseStyle.maxLines = 4;
  {
    RickyPageUi::Bold quote(renderer, 2);
    // Draw the raw persisted phrase so an intentionally empty value stays
    // blank; homePhrase() would substitute the localized default instead.
    RickyPageUi::wrappedText(target, panel, SETTINGS.rickyHomePhrase, phraseStyle);
  }
  gfxTarget->setFont(fui::GfxRendererTarget::FONT_LABEL, SMALL_FONT_ID);
  target.fill(fui::Rect{banner.x, static_cast<int16_t>(banner.bottom() - 1), banner.width, 1}, black);
  // Long-press only: a single tap on the banner must not open the editor.
  screen.frame().hit(banner, OPEN_BOOK, BANNER, fui::InputLongPress);

  // ---- Reading record: a real month calendar that follows the current date. ----
  const int calYear = dated ? local.tm_year + 1900 : 0;
  const unsigned calMonth = dated ? static_cast<unsigned>(local.tm_mon + 1) : 0;
  const unsigned todayDom = dated ? static_cast<unsigned>(local.tm_mday) : 0;
  // Days in month + Monday-based offset of day 1, from the same ordinal math
  // the Reading Analytics calendar uses.
  static constexpr unsigned kDaysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  unsigned daysInMonth = 31;
  int firstWeekday = 0;  // 0 = Monday
  uint64_t monthMs = 0;
  if (dated && calMonth >= 1 && calMonth <= 12) {
    const bool leap = (calYear % 4 == 0) && (calYear % 100 != 0 || calYear % 400 == 0);
    daysInMonth = kDaysInMonth[calMonth - 1] + (calMonth == 2 && leap ? 1 : 0);
    const uint32_t firstOrdinal = TimeUtils::getDayOrdinalForDate(calYear, calMonth, 1);
    firstWeekday = static_cast<int>((firstOrdinal + 3U) % 7U);
    const uint32_t monthStart = firstOrdinal;
    const uint32_t monthEnd = TimeUtils::getDayOrdinalForDate(calYear, calMonth, daysInMonth);
    for (const auto& day : READING_STATS.getReadingDays()) {
      if (day.dayOrdinal >= monthStart && day.dayOrdinal <= monthEnd) monthMs += day.readingMs;
    }
  }
  auto formatDuration = [&](uint64_t ms, char* buffer, size_t size) {
    const uint64_t seconds = ms / 1000;
    if (seconds < 3600)
      snprintf(buffer, size, tr(STR_RICKY_MINUTES), static_cast<unsigned>(seconds / 60));
    else
      snprintf(buffer, size, tr(STR_RICKY_HOUR_MIN), static_cast<unsigned>(seconds / 3600),
               static_cast<unsigned>(seconds % 3600 / 60));
  };
  char durationBuffer[48];
  formatDuration(monthMs, durationBuffer, sizeof(durationBuffer));
  snprintf(statsText, sizeof(statsText), tr(STR_RICKY_MONTH_READ), durationBuffer);

  int recordHeight = totalHeight * 29 / 100;
  recordHeight = std::max<int>(smallHeight * 8, std::min<int>(recordHeight, totalHeight * 34 / 100));
  const auto record = screen.takeTop(recordHeight, gap);
  char monthTitle[32];
  if (dated)
    snprintf(monthTitle, sizeof(monthTitle), "%04d.%02d", calYear, calMonth);
  else
    snprintf(monthTitle, sizeof(monthTitle), "%s", tr(STR_RICKY_DATE_UNSET));
  target.text(fui::Rect{record.x, record.y, static_cast<int16_t>(record.width / 2), static_cast<int16_t>(smallHeight)},
              monthTitle, strong);
  auto monthStyle = right;
  target.text(fui::Rect{static_cast<int16_t>(record.x + record.width / 2), record.y,
                        static_cast<int16_t>(record.width - record.width / 2), static_cast<int16_t>(smallHeight)},
              statsText, monthStyle);
  target.fill(fui::Rect{record.x, static_cast<int16_t>(record.y + smallHeight + gap / 4), record.width, 1}, rule);
  // Weekday header row (M..S), then the day-number grid. Each cell is shaded
  // by that day's reading time. ASCII initials avoid font/locale glyph issues.
  static constexpr const char* kWeekHeads[] = {"M", "T", "W", "T", "F", "S", "S"};
  const int headsTop = record.y + smallHeight + gap / 2;
  const int colWidth = record.width / 7;
  auto headStyle = small;
  headStyle.align = fui::TextAlign::Center;
  for (int c = 0; c < 7; ++c) {
    target.text(fui::Rect{static_cast<int16_t>(record.x + c * colWidth), static_cast<int16_t>(headsTop),
                          static_cast<int16_t>(colWidth), static_cast<int16_t>(smallHeight)},
                kWeekHeads[c], headStyle);
  }
  const int gridTop = headsTop + smallHeight + gap / 4;
  const int gridHeight = std::max<int>(0, record.bottom() - gridTop);
  const int rows = 6;
  const int cellH = std::max<int>(smallHeight + 4, gridHeight / rows);
  const uint32_t monthStartOrdinal = dated ? TimeUtils::getDayOrdinalForDate(calYear, calMonth, 1) : 0;
  for (int row = 0; row < rows; ++row) {
    for (int col = 0; col < 7; ++col) {
      const int dom = (row * 7 + col) - firstWeekday + 1;
      if (dom < 1 || static_cast<unsigned>(dom) > daysInMonth) continue;
      const fui::Rect box{static_cast<int16_t>(record.x + col * colWidth + 2),
                          static_cast<int16_t>(gridTop + row * cellH), static_cast<int16_t>(colWidth - 4),
                          static_cast<int16_t>(cellH - 2)};
      uint64_t dayMs = 0;
      if (dated) {
        const uint32_t ordinal = monthStartOrdinal + static_cast<uint32_t>(dom - 1);
        for (const auto& day : READING_STATS.getReadingDays())
          if (day.dayOrdinal == ordinal) {
            dayMs = day.readingMs;
            break;
          }
      }
      const uint32_t seconds = static_cast<uint32_t>(dayMs / 1000);
      const auto shade = seconds == 0        ? fui::Color::White
                         : seconds < 15 * 60 ? fui::Color::LightGray
                         : seconds < 30 * 60 ? fui::Color::DarkGray
                                             : fui::Color::Black;
      target.fill(box, fui::Paint::solid(shade), 2);
      if (seconds == 0) target.stroke(box, rule, 1, 2);
      // Day number; invert text over dark cells for legibility.
      auto dayStyle = small;
      dayStyle.align = fui::TextAlign::Center;
      dayStyle.inverted = seconds >= 30 * 60;
      if (static_cast<unsigned>(dom) == todayDom) dayStyle.bold = true;
      char dayText[4];
      snprintf(dayText, sizeof(dayText), "%u", static_cast<unsigned>(dom));
      target.text(fui::Rect{box.x, box.y, box.width, static_cast<int16_t>(smallHeight)}, dayText, dayStyle);
    }
  }
  // Long-press only: opening the calendar/stats is deliberate, never a stray tap.
  screen.frame().hit(record, OPEN_BOOK, STATISTICS, fui::InputLongPress);

  // ---- Footer: left "now reading" (books[0]) | right paged "recently read". ----
  // The calendar's last row can sit visually tight against the section label.
  // Add one full gap on top of takeTop()'s own gap before starting the footer.
  const auto footerRect = screen.body();
  const int footerGap = gap;
  if (footerRect.height <= footerGap + smallHeight * 2) return;
  const fui::Rect footer{footerRect.x, static_cast<int16_t>(footerRect.y + footerGap), footerRect.width,
                         static_cast<int16_t>(footerRect.height - footerGap)};
  const int dividerX = footer.x + footer.width * 38 / 100;
  constexpr int kDividerWidth = 3;
  const int headerRuleY = footer.y + smallHeight + gap / 4;
  // The vertical rule now lives only below the header rule: the short segment
  // between the two column headings ("now reading" / "recently read") has been
  // removed. What remains separates the cover from the recent-book list, drawn
  // in a lighter gray and stopping short of the very bottom.
  const int ruleBreakHalf = 5;
  const int dividerBottom = footer.bottom() - gap;
  if (dividerBottom > headerRuleY + ruleBreakHalf) {
    target.fill(
        fui::Rect{static_cast<int16_t>(dividerX - kDividerWidth / 2), static_cast<int16_t>(headerRuleY + ruleBreakHalf),
                  kDividerWidth, static_cast<int16_t>(dividerBottom - headerRuleY - ruleBreakHalf)},
        rule);
  }
  // Give the right column extra breathing room from the divider so the two
  // sections never look crowded against the rule.
  const int recentInset = gap + std::max<int>(4, gap / 2);
  target.text(
      fui::Rect{footer.x, footer.y, static_cast<int16_t>(dividerX - footer.x - gap), static_cast<int16_t>(smallHeight)},
      tr(STR_RICKY_NOW_READING), strong);
  target.text(
      fui::Rect{static_cast<int16_t>(dividerX + recentInset), footer.y,
                static_cast<int16_t>(footer.right() - dividerX - recentInset), static_cast<int16_t>(smallHeight)},
      tr(STR_RICKY_RECENT_READ), strong);
  // One continuous light-gray rule under both headers, matching the calendar
  // divider. The vertical rule above is broken here, so they never intersect.
  target.fill(fui::Rect{footer.x, static_cast<int16_t>(headerRuleY), footer.width, 1}, rule);
  // Push the content block (cover/title/progress on the left, book list on the
  // right) further below the header rule so it does not hug the gray line.
  const int listTop = footer.y + smallHeight + gap + gap / 2;
  const int listHeight = footer.bottom() - listTop;

  // Left column: an enlarged cover, the book title, then a progress bar and
  // percentage on one row. Nothing is drawn over the cover artwork.
  if (!books || books->empty()) {
    target.text(fui::Rect{footer.x, static_cast<int16_t>(listTop + listHeight / 2),
                          static_cast<int16_t>(dividerX - footer.x - gap), static_cast<int16_t>(smallHeight)},
                tr(STR_NO_RECENT_BOOKS), small);
    screen.frame().hit(
        fui::Rect{footer.x, footer.y, static_cast<int16_t>(dividerX - footer.x), static_cast<int16_t>(footer.height)},
        OPEN_BOOK, LIBRARY, fui::InputTouch);
  } else {
    const int boxW = dividerX - footer.x - gap;
    // iFink-style "now reading" card: a large cover, then a bold centred
    // title, then one row holding a slim progress bar and its percentage.
    // Nothing is drawn over the cover artwork.
    const int recentRows = std::max<int>(1, RickyHomeLayout::PAGE_BOOKS);
    const int recentRowHeight = listHeight / recentRows;
    const int recentBarH = std::max<int>(4, smallHeight / 4);
    // Bottom edge of the last recent row's progress bar (row index recentRows-1).
    const int lastRowY = listTop + (recentRows - 1) * recentRowHeight;
    const int lastBarBottom = lastRowY + smallHeight + (smallHeight - recentBarH) / 2 + recentBarH;
    const int blockBottom = std::min<int>(lastBarBottom, footer.bottom());
    const int textH = smallHeight;
    const int barH = std::max<int>(4, smallHeight / 4);  // slim track, like the recent list
    const int titleGap = std::max(3, gap / 3);
    const int lineGap = std::max(2, gap / 6);
    const int coverTop = listTop + std::max(4, gap / 3);
    // Reserve one row for the title and one for the bar + percentage pair.
    const int reserved = titleGap + textH + lineGap + textH;
    // Reference geometry (the previous centred 3:4 cover) pins the LEFT edge,
    // so enlarging the cover only grows it to the right and downward.
    const int refAvailH = std::max<int>(0, blockBottom - coverTop - reserved);
    const int maxW = std::max<int>(0, boxW - gap * 2);
    int refW = std::min<int>(maxW, refAvailH * 3 / 4);
    int refH = refW * 4 / 3;
    if (refH > refAvailH) {
      refH = refAvailH;
      refW = refH * 3 / 4;
    }
    const int coverLeft = footer.x + gap + (maxW - refW) / 2;
    // Grow the frame a little to the right and use a lower bottom budget, so
    // the title and progress row follow the taller cover downward.
    const int growW = std::max<int>(4, gap * 2);
    int coverW = std::min<int>(dividerX - gap - coverLeft, refW + growW);
    const int coverAvailH = std::max<int>(0, footer.bottom() - coverTop - reserved);
    int coverH = std::min<int>(coverAvailH, coverW * 4 / 3);
    if (coverH > 0 && coverW > 0) {
      const fui::Rect cover{static_cast<int16_t>(coverLeft), static_cast<int16_t>(coverTop),
                            static_cast<int16_t>(coverW), static_cast<int16_t>(coverH)};
      // Bind the real cover height BEFORE painting. paint() -> painter lambda
      // calls activity.setThumbnailHeight(rickyHome->coverHeight()); a freshly
      // constructed host still holds 0 here, which reset the activity height
      // to 0 and made drawBookCover() skip the artwork (empty placeholder)
      // until a later redraw (swipe) happened to re-bind the height.
      thumbnailHeight = coverH;
      paint(cover, 0, false);  // no hairline frame — artwork only

      const auto& currentBook = (*books)[0];
      auto titleStyle = strong;
      titleStyle.align = fui::TextAlign::Center;
      const int titleY = cover.bottom() + titleGap;
      target.text(fui::Rect{cover.x, static_cast<int16_t>(titleY), cover.width, static_cast<int16_t>(textH)},
                  titleOf(currentBook), titleStyle);

      // One row below the title: a slim progress bar plus the percentage.
      // Bind a small real font to the LABEL slot for the muted percentage,
      // then restore the theme default so the rest of the page keeps its chrome.
      char progressLabel[8];
      snprintf(progressLabel, sizeof(progressLabel), "%u%%", static_cast<unsigned>(progress));
      auto progressStyle = small;
      progressStyle.font = fui::GfxRendererTarget::FONT_LABEL;
      progressStyle.align = fui::TextAlign::Right;
      progressStyle.color = fui::Color::DarkGray;  // muted percentage, not eye-catching
      auto* coverTarget = static_cast<fui::GfxRendererTarget*>(&target);
      coverTarget->setFont(fui::GfxRendererTarget::FONT_LABEL, UI_10_FONT_ID);
      const int pctWidth =
          coverTarget->measureText(fui::GfxRendererTarget::FONT_LABEL, progressLabel, progressStyle).width;
      const int rowY = titleY + textH + lineGap;
      const int barWidth = std::max<int>(8, cover.width - pctWidth - gap / 2);
      const int barY = rowY + (textH - barH) / 2;
      RickyPageUi::progressBar(
          target,
          fui::Rect{cover.x, static_cast<int16_t>(barY), static_cast<int16_t>(barWidth), static_cast<int16_t>(barH)},
          progress);
      target.text(fui::Rect{static_cast<int16_t>(cover.right() - pctWidth), rowY, static_cast<int16_t>(pctWidth),
                            static_cast<int16_t>(textH)},
                  progressLabel, progressStyle);
      coverTarget->setFont(fui::GfxRendererTarget::FONT_LABEL, SMALL_FONT_ID);
    }
    screen.frame().hit(fui::Rect{footer.x, static_cast<int16_t>(listTop), static_cast<int16_t>(dividerX - footer.x),
                                 static_cast<int16_t>(listHeight)},
                       OPEN_BOOK, 0, fui::InputTouch);
  }

  // Right column: paged recent books, independent of the pinned current book.
  const int recentCount = books ? std::max(0, static_cast<int>(books->size()) - 1) : 0;
  const int pages = std::max(1, (recentCount + RickyHomeLayout::PAGE_BOOKS - 1) / RickyHomeLayout::PAGE_BOOKS);
  if (pages > 1) {
    char pageText[16];
    snprintf(pageText, sizeof(pageText), "%d / %d",
             std::clamp((recentStart - 1) / RickyHomeLayout::PAGE_BOOKS + 1, 1, pages), pages);
    target.text(fui::Rect{static_cast<int16_t>(footer.right() - smallHeight * 3), footer.y,
                          static_cast<int16_t>(smallHeight * 3), static_cast<int16_t>(smallHeight)},
                pageText, right);
  }
  const int rowHeight = listHeight / RickyHomeLayout::PAGE_BOOKS;
  const int count = std::min<int>(RickyHomeLayout::PAGE_BOOKS, std::max<int>(0, recentCount - (recentStart - 1)));
  for (int i = 0; i < count; ++i) {
    const int index = recentStart + i;
    if (index <= 0 || !books || index >= static_cast<int>(books->size())) break;
    const auto& item = (*books)[index];
    const int y = listTop + i * rowHeight;
    const fui::Rect row{static_cast<int16_t>(dividerX + recentInset), static_cast<int16_t>(y),
                        static_cast<int16_t>(footer.right() - dividerX - recentInset),
                        static_cast<int16_t>(rowHeight - 2)};
    target.text(
        fui::Rect{row.x, row.y, static_cast<int16_t>(row.width - smallHeight * 2), static_cast<int16_t>(smallHeight)},
        titleOf(item), strong);
    // A slim progress bar replaces the "read xx% / unread" caption: one quiet
    // line under each title reads cleaner and keeps the column minimal.
    const auto* saved = READING_STATS.findMatchingBookForPath(item.path);
    const int percent = saved ? static_cast<int>(saved->lastProgressPercent) : 0;
    const int barH = std::max<int>(4, smallHeight / 4);
    const int barY = row.y + smallHeight + (smallHeight - barH) / 2;
    const int barW = std::max<int>(24, row.width - smallHeight * 2);
    RickyPageUi::progressBar(
        target, fui::Rect{row.x, static_cast<int16_t>(barY), static_cast<int16_t>(barW), static_cast<int16_t>(barH)},
        percent);
    RickyPageUi::chevron(
        target, fui::Rect{static_cast<int16_t>(row.right() - 20), static_cast<int16_t>(row.y + smallHeight / 2), 20,
                          static_cast<int16_t>(smallHeight)});
    if (i + 1 < count) target.fill(fui::Rect{row.x, static_cast<int16_t>(row.bottom()), row.width, 1}, rule);
    screen.frame().hit(row, OPEN_BOOK, index, fui::InputTouch);
  }
}
#endif
