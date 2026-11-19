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
  const auto touch = routeTouch(input);
  return touch.snap.touchReleased ? pending : -1;
}

bool RickyHomeUi::paint(fui::Rect rect, int index) {
  if (painter && painter(owner, index, Rect{rect.x, rect.y, rect.width, rect.height})) {
    // Real artwork gets the same hairline edge as generated covers; pale covers
    // would otherwise dissolve into the page.
    uiTarget.stroke(rect, fui::Paint::solid(fui::Color::Black), 1);
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
    screen.target().text(screen.takeBottom(labelHeight * 2, gap * 2), RickyProfile::homePhrase(), phrase);
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
  grid.selectedIndex = showSelection && selected >= recentStart && selected < recentStart + grid.count
                           ? selected - recentStart
                           : -1;
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
  const int titleHeight = target.lineHeight(theme.titleText.font);
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
  char clockText[24];
  if (dated && !TimeUtils::formatCurrentTime(clockText, sizeof(clockText), SETTINGS.clockFormat == 1))
    snprintf(clockText, sizeof(clockText), "%s", "--:--");
  if (!dated) snprintf(clockText, sizeof(clockText), "%s", "--:--");
  char batteryText[32];
  snprintf(batteryText, sizeof(batteryText), tr(STR_RICKY_FOLIO_BATTERY),
           static_cast<unsigned>(powerManager.getBatteryPercentage()));

  // ---- Status row: time on the left, battery on the right; no date / app name. ----
  const auto status = screen.takeTop(titleHeight, gap);
  {
    RickyPageUi::Bold clock(renderer, 2);
    target.text(fui::Rect{status.x, status.y, static_cast<int16_t>(status.width / 2),
                          static_cast<int16_t>(titleHeight)},
                clockText, heading);
    auto batteryStyle = heading;
    batteryStyle.align = fui::TextAlign::Right;
    target.text(fui::Rect{static_cast<int16_t>(status.x + status.width / 2), status.y,
                          static_cast<int16_t>(status.width - status.width / 2),
                          static_cast<int16_t>(titleHeight)},
                batteryText, batteryStyle);
  }
  screen.frame().hit(status, OPEN_BOOK, STATISTICS, fui::InputTouch);

  // ---- Banner: custom picture + large overlaid phrase (tap to configure). ----
  int bannerHeight = totalHeight * 25 / 100;
  bannerHeight = std::max<int>(smallHeight * 7, std::min<int>(bannerHeight, totalHeight * 32 / 100));
  const auto banner = screen.takeTop(bannerHeight, gap);
  bool pictured = RickyProfile::drawHomeBanner(renderer, Rect{banner.x, banner.y, banner.width, banner.height});
  if (!pictured) {
    target.fill(banner, fui::Paint::dither(fui::Color::LightGray), 6);
    target.stroke(banner, black, 1, 6);
  }
  // A pale text panel keeps the phrase legible over any imported picture.
  const int panelX = banner.x + banner.width * 44 / 100;
  const fui::Rect panel{static_cast<int16_t>(panelX), static_cast<int16_t>(banner.y + gap / 2),
                        static_cast<int16_t>(banner.right() - panelX - gap / 2),
                        static_cast<int16_t>(banner.height - gap)};
  target.fill(panel, fui::Paint::solid(fui::Color::White), 4);
  {
    RickyPageUi::Bold kicker(renderer, 2);
    target.text(fui::Rect{panel.x, panel.y, panel.width, static_cast<int16_t>(smallHeight)},
                tr(STR_RICKY_HOME_BRAND), small);
  }
  auto phraseStyle = heading;
  phraseStyle.align = fui::TextAlign::Right;
  phraseStyle.maxLines = 4;
  {
    RickyPageUi::Bold quote(renderer, 3);
    RickyPageUi::wrappedText(
        target,
        fui::Rect{panel.x, static_cast<int16_t>(panel.y + smallHeight + gap / 2), panel.width,
                  static_cast<int16_t>(panel.bottom() - panel.y - smallHeight - gap / 2)},
        RickyProfile::homePhrase(), phraseStyle);
  }
  target.fill(fui::Rect{banner.x, static_cast<int16_t>(banner.bottom() - 1), banner.width, 1}, black);
  screen.frame().hit(banner, OPEN_BOOK, BANNER, fui::InputTouch);

  // ---- Reading record: this-week total + weekday-aligned 4-week heatmap. ----
  const auto& days = READING_STATS.getReadingDays();
  const int32_t todayOrdinal = static_cast<int32_t>(READING_STATS.getTodayDayOrdinal());
  uint32_t heat[28] = {};  // index 0 = 27 days ago, 27 = today
  for (const auto& day : days) {
    const int64_t offset = static_cast<int64_t>(day.dayOrdinal) - (static_cast<int64_t>(todayOrdinal) - 27);
    if (offset >= 0 && offset < 28) heat[offset] = day.readingMs;
  }
  const int mondayCol = dated ? (local.tm_wday + 6) % 7 : 0;  // 0 = Monday
  uint64_t weekMs = 0;
  for (int d = 0; d <= mondayCol; ++d) weekMs += heat[27 - d];
  auto formatDuration = [&](uint64_t ms, char* buffer, size_t size) {
    const uint64_t seconds = ms / 1000;
    if (seconds < 3600)
      snprintf(buffer, size, tr(STR_RICKY_MINUTES), static_cast<unsigned>(seconds / 60));
    else
      snprintf(buffer, size, tr(STR_RICKY_HOUR_MIN), static_cast<unsigned>(seconds / 3600),
               static_cast<unsigned>(seconds % 3600 / 60));
  };
  char durationBuffer[48];
  formatDuration(weekMs, durationBuffer, sizeof(durationBuffer));
  snprintf(statsText, sizeof(statsText), tr(STR_RICKY_WEEK_READ), durationBuffer);

  int recordHeight = totalHeight * 29 / 100;
  recordHeight = std::max<int>(smallHeight * 7, std::min<int>(recordHeight, totalHeight * 34 / 100));
  const auto record = screen.takeTop(recordHeight, gap);
  target.text(fui::Rect{record.x, record.y, static_cast<int16_t>(record.width / 2),
                        static_cast<int16_t>(smallHeight)},
              tr(STR_RICKY_READING_RECORD), strong);
  auto weekStyle = right;
  target.text(fui::Rect{static_cast<int16_t>(record.x + record.width / 2), record.y,
                        static_cast<int16_t>(record.width - record.width / 2),
                        static_cast<int16_t>(smallHeight)},
              statsText, weekStyle);
  target.fill(fui::Rect{record.x, static_cast<int16_t>(record.y + smallHeight + gap / 4), record.width, 1}, rule);
  // Weekday header row (MON..SUN), then the grid cells below it.
  static constexpr const char* kWeekHeads[] = {"MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"};
  const int headsTop = record.y + smallHeight + gap / 2;
  const int colWidth = record.width / 7;
  for (int c = 0; c < 7; ++c) {
    auto head = small;
    head.align = fui::TextAlign::Center;
    target.text(fui::Rect{static_cast<int16_t>(record.x + c * colWidth), static_cast<int16_t>(headsTop),
                          static_cast<int16_t>(colWidth), static_cast<int16_t>(smallHeight)},
                kWeekHeads[c], head);
  }
  const int gridTop = headsTop + smallHeight + gap / 4;
  const int gridHeight = std::max<int>(0, record.bottom() - gridTop);
  const int cellGap = std::max<int>(3, gap / 3);
  const int cell = std::max<int>(3, std::min<int>((colWidth - cellGap * 2) / 2, (gridHeight - cellGap * 3) / 4));
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 7; ++col) {
      const int daysAgo = mondayCol - col + (3 - row) * 7;
      if (daysAgo < 0 || daysAgo >= 28) continue;
      const fui::Rect box{static_cast<int16_t>(record.x + col * colWidth + (colWidth - cell) / 2),
                          static_cast<int16_t>(gridTop + row * (cell + cellGap)), static_cast<int16_t>(cell),
                          static_cast<int16_t>(cell)};
      const uint32_t seconds = heat[27 - daysAgo] / 1000;
      if (seconds == 0) {
        target.fill(box, fui::Paint::solid(fui::Color::White), 2);
        target.stroke(box, rule, 1, 2);
      } else {
        const auto shade = seconds < 15 * 60  ? fui::Color::LightGray
                           : seconds < 30 * 60 ? fui::Color::DarkGray
                                               : fui::Color::Black;
        target.fill(box, fui::Paint::solid(shade), 2);
      }
    }
  }
  screen.frame().hit(record, OPEN_BOOK, STATISTICS, fui::InputTouch);

  // ---- Footer: left "now reading" (books[0]) | right paged "recently read". ----
  const auto footer = screen.body();
  if (footer.height <= smallHeight * 2) return;
  const int dividerX = footer.x + footer.width * 38 / 100;
  target.fill(fui::Rect{static_cast<int16_t>(dividerX - 1), footer.y, 1, static_cast<int16_t>(footer.height)}, rule);
  target.text(fui::Rect{footer.x, footer.y, static_cast<int16_t>(dividerX - footer.x - gap),
                        static_cast<int16_t>(smallHeight)},
              tr(STR_RICKY_NOW_READING), strong);
  target.text(fui::Rect{static_cast<int16_t>(dividerX + gap), footer.y,
                        static_cast<int16_t>(footer.right() - dividerX - gap), static_cast<int16_t>(smallHeight)},
              tr(STR_RICKY_RECENT_READ), strong);
  const int listTop = footer.y + smallHeight + gap / 2;
  const int listHeight = footer.bottom() - listTop;

  // Left column: the pinned current book, cover + title + progress.
  if (!books || books->empty()) {
    target.text(fui::Rect{footer.x, static_cast<int16_t>(listTop + listHeight / 2),
                          static_cast<int16_t>(dividerX - footer.x - gap), static_cast<int16_t>(smallHeight)},
                tr(STR_NO_RECENT_BOOKS), small);
    screen.frame().hit(fui::Rect{footer.x, footer.y, static_cast<int16_t>(dividerX - footer.x),
                                 static_cast<int16_t>(footer.height)}, OPEN_BOOK, LIBRARY, fui::InputTouch);
  } else {
    const int leftWidth = dividerX - footer.x - gap * 2;
    const int coverHeight = std::max<int>(smallHeight * 3, std::min<int>(listHeight - smallHeight * 2, leftWidth * 4 / 3));
    const int coverWidth = coverHeight * 3 / 4;
    const fui::Rect cover{static_cast<int16_t>(footer.x + gap / 2), static_cast<int16_t>(listTop + gap / 2),
                          static_cast<int16_t>(coverWidth), static_cast<int16_t>(coverHeight)};
    paint(cover, 0);
    thumbnailHeight = coverHeight;
    if (showSelection && selected == 0) target.stroke(cover, black, 3, 4);
    const int textX = cover.right() + gap / 2;
    const int textWidth = dividerX - textX - gap;
    const auto& book = (*books)[0];
    auto bookTitle = strong;
    bookTitle.maxLines = 3;
    RickyPageUi::wrappedText(target,
                             fui::Rect{static_cast<int16_t>(textX), static_cast<int16_t>(listTop + gap / 2),
                                       static_cast<int16_t>(textWidth),
                                       static_cast<int16_t>(std::min<int>(coverHeight - smallHeight, smallHeight * 3))},
                             titleOf(book), bookTitle);
    snprintf(statsText, sizeof(statsText), tr(STR_RICKY_READ_PROGRESS), static_cast<unsigned>(progress));
    target.text(fui::Rect{static_cast<int16_t>(textX), static_cast<int16_t>(cover.bottom() - smallHeight),
                          static_cast<int16_t>(textWidth), static_cast<int16_t>(smallHeight)},
                statsText, small);
    RickyPageUi::progressBar(target,
                             fui::Rect{static_cast<int16_t>(textX), static_cast<int16_t>(cover.bottom()),
                                       static_cast<int16_t>(textWidth), 4}, progress);
    screen.frame().hit(fui::Rect{footer.x, static_cast<int16_t>(listTop), static_cast<int16_t>(dividerX - footer.x),
                                 static_cast<int16_t>(listHeight)}, OPEN_BOOK, 0, fui::InputTouch);
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
    const fui::Rect row{static_cast<int16_t>(dividerX + gap), static_cast<int16_t>(y),
                        static_cast<int16_t>(footer.right() - dividerX - gap), static_cast<int16_t>(rowHeight - 2)};
    target.text(fui::Rect{row.x, row.y, static_cast<int16_t>(row.width - smallHeight * 2),
                          static_cast<int16_t>(smallHeight)}, titleOf(item), strong);
    const auto* saved = READING_STATS.findMatchingBookForPath(item.path);
    if (saved)
      snprintf(statsText, sizeof(statsText), tr(STR_RICKY_READ_PROGRESS),
               static_cast<unsigned>(saved->lastProgressPercent));
    target.text(fui::Rect{row.x, static_cast<int16_t>(row.y + smallHeight),
                          static_cast<int16_t>(row.width - smallHeight * 2), static_cast<int16_t>(smallHeight)},
                saved ? statsText : tr(STR_RICKY_READ_UNREAD), small);
    RickyPageUi::chevron(target, fui::Rect{static_cast<int16_t>(row.right() - 20),
                                           static_cast<int16_t>(row.y + smallHeight / 2), 20,
                                           static_cast<int16_t>(smallHeight)});
    if (i + 1 < count)
      target.fill(fui::Rect{row.x, static_cast<int16_t>(row.bottom()), row.width, 1}, rule);
    screen.frame().hit(row, OPEN_BOOK, index, fui::InputTouch);
  }
}
#endif
