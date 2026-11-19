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

// Folio-style home: a three-card portrait page (continue / reading time /
// recently read) over a brand header. Geometry mirrors Folio's 684x1216
// layout, scaled to the current content rect, so the page keeps the same
// proportions on any safe area or font scale.
void RickyHomeUi::drawPortrait(UiScreen& screen) {
  const auto& theme = screen.theme();
  auto& target = screen.target();
  const int gap = std::max<int>(12, theme.spaceSm);
  const int titleHeight = target.lineHeight(theme.titleText.font);
  const int smallHeight = target.lineHeight(theme.smallText.font);
  // The content rect already carries the page padding; add only a hairline of air.
  screen.insetContent(fui::Insets{static_cast<int16_t>(gap), 6, static_cast<int16_t>(gap), 6});
  // One CJK weight ships; synthetic bold (held for this whole page) supplies the
  // hierarchy that `style.bold` asks for. Headings nest a stronger scope below.
  RickyPageUi::Bold bold(renderer, 1);
  const auto black = fui::Paint::solid(fui::Color::Black);
  const auto rule = fui::Paint::solid(fui::Color::DarkGray);
  auto small = theme.smallText;
  small.maxLines = 1;
  auto right = small;
  right.align = fui::TextAlign::Right;
  // The small face carries the full CJK set; synthetic bold supplies hierarchy.
  auto strong = small;
  strong.bold = true;
  auto heading = theme.titleText;
  heading.bold = true;
  heading.maxLines = 1;

  // ---- Header: brand + battery on the left, time + date on the right. ----
  std::tm local{};
  const bool dated = halClock.localTime(local);
  char clockText[24];
  if (dated && !TimeUtils::formatCurrentTime(clockText, sizeof(clockText), SETTINGS.clockFormat == 1))
    snprintf(clockText, sizeof(clockText), "%s", "--:--");
  if (!dated) snprintf(clockText, sizeof(clockText), "%s", "--:--");
  char dateTextLocal[48];
  if (dated) {
    static constexpr StrId weekdays[] = {
        StrId::STR_CAL_WEEKDAY_SUN, StrId::STR_CAL_WEEKDAY_MON, StrId::STR_CAL_WEEKDAY_TUE, StrId::STR_CAL_WEEKDAY_WED,
        StrId::STR_CAL_WEEKDAY_THU, StrId::STR_CAL_WEEKDAY_FRI, StrId::STR_CAL_WEEKDAY_SAT};
    snprintf(dateTextLocal, sizeof(dateTextLocal), tr(STR_RICKY_FOLIO_DATE), local.tm_year + 1900,
             local.tm_mon + 1, local.tm_mday, I18N.get(weekdays[std::clamp(local.tm_wday, 0, 6)]));
  } else {
    snprintf(dateTextLocal, sizeof(dateTextLocal), "%s", tr(STR_RICKY_DATE_UNSET));
  }
  char batteryText[32];
  snprintf(batteryText, sizeof(batteryText), tr(STR_RICKY_FOLIO_BATTERY),
           static_cast<unsigned>(powerManager.getBatteryPercentage()));
  const int headerHeight = titleHeight + gap / 2 + smallHeight;
  const auto header = screen.takeTop(headerHeight, gap * 2);
  const int halfHeader = header.width / 2;
  {
    RickyPageUi::Bold brand(renderer, 2);
    target.text(fui::Rect{header.x, header.y, static_cast<int16_t>(halfHeader), static_cast<int16_t>(titleHeight)},
                tr(STR_RICKY_HOME_BRAND), heading);
    auto clockStyle = heading;
    clockStyle.align = fui::TextAlign::Right;
    target.text(fui::Rect{static_cast<int16_t>(header.x + halfHeader), header.y,
                          static_cast<int16_t>(header.width - halfHeader), static_cast<int16_t>(titleHeight)},
                clockText, clockStyle);
  }
  target.text(fui::Rect{header.x, static_cast<int16_t>(header.y + titleHeight + gap / 2),
                        static_cast<int16_t>(halfHeader), static_cast<int16_t>(smallHeight)},
              batteryText, small);
  target.text(fui::Rect{static_cast<int16_t>(header.x + halfHeader),
                        static_cast<int16_t>(header.y + titleHeight + gap / 2),
                        static_cast<int16_t>(header.width - halfHeader), static_cast<int16_t>(smallHeight)},
              dateTextLocal, right);
  screen.frame().hit(header, OPEN_BOOK, STATISTICS, fui::InputTouch);

  if (!books || books->empty()) {
    screen.centeredText(tr(STR_NO_RECENT_BOOKS));
    screen.frame().hit(screen.body(), OPEN_BOOK, LIBRARY, fui::InputTouch);
    return;
  }

  // ---- Card 1: continue reading, pinned to books[0]. ----
  const int coverWidth = std::min<int>(screen.body().width * 26 / 100, 168);
  const int coverHeight = std::max<int>(coverWidth * 4 / 3, smallHeight * 3);
  const int cardPad = gap;
  const int continueHeight = coverHeight + cardPad * 2;
  const auto card = screen.takeTop(continueHeight, gap * 2);
  target.stroke(card, black, 1, 10);
  thumbnailHeight = coverHeight;
  const fui::Rect cover{card.x + cardPad, card.y + cardPad, static_cast<int16_t>(coverWidth),
                        static_cast<int16_t>(coverHeight)};
  paint(cover, 0);
  if (showSelection && selected == 0) target.stroke(cover, black, 3, 4);
  auto info = card;
  info.x = static_cast<int16_t>(cover.right() + cardPad);
  info.width = static_cast<int16_t>(card.right() - cardPad - info.x);
  info.y = static_cast<int16_t>(card.y + cardPad);
  info.height = static_cast<int16_t>(coverHeight);
  target.text(fui::Rect{info.x, info.y, info.width, static_cast<int16_t>(smallHeight)}, tr(STR_CONTINUE_READING), small);
  const auto& book = (*books)[0];
  auto bookTitle = strong;
  bookTitle.maxLines = 2;
  const int bookTitleHeight =
      std::min<int>(smallHeight * 2, fui::measureWrappedText(target, titleOf(book), bookTitle, info.width).height);
  RickyPageUi::wrappedText(target,
                           fui::Rect{info.x, static_cast<int16_t>(info.y + smallHeight + gap / 2), info.width,
                                     static_cast<int16_t>(bookTitleHeight)},
                           titleOf(book), bookTitle);
  const int bottomRowsHeight = smallHeight * 2 + gap + 4;
  const int bottomRowsY = info.bottom() - bottomRowsHeight;
  snprintf(statsText, sizeof(statsText), tr(STR_RICKY_READ_PROGRESS), static_cast<unsigned>(progress));
  target.text(fui::Rect{info.x, static_cast<int16_t>(bottomRowsY), info.width, static_cast<int16_t>(smallHeight)},
              statsText, small);
  RickyPageUi::progressBar(target,
                           fui::Rect{info.x, static_cast<int16_t>(bottomRowsY + smallHeight + gap / 2), info.width, 4},
                           progress);
  // "Read on >" closes the card, matching Folio's primary action.
  auto resume = info;
  resume.y = static_cast<int16_t>(info.bottom() - smallHeight);
  resume.height = static_cast<int16_t>(smallHeight);
  fui::Rect mark{static_cast<int16_t>(resume.right() - 24), resume.y, static_cast<int16_t>(24),
                 static_cast<int16_t>(smallHeight)};
  resume.width = static_cast<int16_t>(mark.x - gap / 2 - resume.x);
  target.text(resume, tr(STR_RICKY_HOME_RESUME), strong);
  RickyPageUi::chevron(target, mark);
  screen.frame().hit(card, OPEN_BOOK, 0, fui::InputTouch);

  // ---- Card 2: reading time with a 28-day (14x2) grayscale heatmap. ----
  const auto& days = READING_STATS.getReadingDays();
  const int32_t today = static_cast<int32_t>(READING_STATS.getTodayDayOrdinal());
  uint64_t todayMs = READING_STATS.getTodayReadingMs();
  uint64_t totalMs = READING_STATS.getTotalReadingMs();
  uint32_t heat[28] = {};
  for (const auto& day : days) {
    const int64_t offset = static_cast<int64_t>(day.dayOrdinal) - (static_cast<int64_t>(today) - 27);
    if (offset >= 0 && offset < 28) heat[offset] = day.readingMs;
  }
  auto formatDuration = [&](uint64_t ms, char* buffer, size_t size) {
    const uint64_t seconds = ms / 1000;
    if (seconds == 0)
      snprintf(buffer, size, tr(STR_RICKY_MINUTES), 0);
    else if (seconds < 3600)
      snprintf(buffer, size, tr(STR_RICKY_MINUTES), static_cast<unsigned>(seconds / 60));
    else
      snprintf(buffer, size, tr(STR_RICKY_HOUR_MIN), static_cast<unsigned>(seconds / 3600),
               static_cast<unsigned>(seconds % 3600 / 60));
  };
  char durationBuffer[48];
  int readingHeight = std::max<int>(smallHeight * 6 + gap * 2, screen.body().height * 22 / 100);
  readingHeight = std::min<int>(readingHeight, screen.body().height - smallHeight * 4 - gap * 4);
  const auto reading = screen.takeTop(readingHeight, gap * 2);
  target.stroke(reading, black, 1, 10);
  auto metricArea = reading;
  metricArea.x = static_cast<int16_t>(metricArea.x + cardPad);
  metricArea.width = static_cast<int16_t>(metricArea.width - cardPad * 2);
  metricArea.y = static_cast<int16_t>(metricArea.y + cardPad);
  metricArea.height = static_cast<int16_t>(metricArea.height - cardPad * 2);
  target.text(fui::Rect{metricArea.x, metricArea.y, static_cast<int16_t>(metricArea.width / 2),
                        static_cast<int16_t>(smallHeight)},
              tr(STR_RICKY_READ_TIME), strong);
  target.text(fui::Rect{static_cast<int16_t>(metricArea.x + metricArea.width / 2), metricArea.y,
                        static_cast<int16_t>(metricArea.width - metricArea.width / 2),
                        static_cast<int16_t>(smallHeight)},
              tr(STR_RICKY_LAST_28_DAYS), right);
  const int valueY = metricArea.y + smallHeight + gap / 2;
  formatDuration(todayMs, durationBuffer, sizeof(durationBuffer));
  target.text(fui::Rect{metricArea.x, static_cast<int16_t>(valueY), static_cast<int16_t>(metricArea.width / 2),
                        static_cast<int16_t>(smallHeight)},
              tr(STR_RICKY_TODAY_READ), small);
  target.text(fui::Rect{metricArea.x, static_cast<int16_t>(valueY + smallHeight),
                        static_cast<int16_t>(metricArea.width / 2),
                        static_cast<int16_t>(target.lineHeight(theme.bodyText.font))},
              durationBuffer, strong);
  formatDuration(totalMs, durationBuffer, sizeof(durationBuffer));
  target.text(fui::Rect{static_cast<int16_t>(metricArea.x + metricArea.width / 2), static_cast<int16_t>(valueY),
                        static_cast<int16_t>(metricArea.width - metricArea.width / 2),
                        static_cast<int16_t>(smallHeight)},
              tr(STR_RICKY_TOTAL_READ), small);
  target.text(fui::Rect{static_cast<int16_t>(metricArea.x + metricArea.width / 2),
                        static_cast<int16_t>(valueY + smallHeight),
                        static_cast<int16_t>(metricArea.width - metricArea.width / 2),
                        static_cast<int16_t>(target.lineHeight(theme.bodyText.font))},
              durationBuffer, strong);
  // Heatmap fills the remaining card space; each cell is a rounded square with
  // four shades: empty / light / dark / inked, exactly like Folio.
  const int heatTop = valueY + smallHeight + target.lineHeight(theme.bodyText.font) + gap / 2;
  const int heatHeight = std::max<int>(0, metricArea.bottom() - heatTop);
  const int cellGap = std::max<int>(4, gap / 3);
  const int cellByWidth = (metricArea.width - cellGap * 13) / 14;
  const int cellByHeight = (heatHeight - cellGap) / 2;
  const int cell = std::max<int>(2, std::min<int>(cellByWidth, cellByHeight));
  const int gridWidth = cell * 14 + cellGap * 13;
  const int gridX = metricArea.x + (metricArea.width - gridWidth) / 2;
  for (int i = 0; i < 28; ++i) {
    const int col = i % 14, row = i / 14;
    const fui::Rect box{static_cast<int16_t>(gridX + col * (cell + cellGap)),
                        static_cast<int16_t>(heatTop + row * (cell + cellGap)), static_cast<int16_t>(cell),
                        static_cast<int16_t>(cell)};
    const uint32_t seconds = heat[i] / 1000;
    if (seconds == 0) {
      target.fill(box, fui::Paint::solid(fui::Color::White), 3);
      target.stroke(box, rule, 1, 3);
    } else {
      const auto shade = seconds < 15 * 60  ? fui::Color::LightGray
                         : seconds < 30 * 60 ? fui::Color::DarkGray
                                             : fui::Color::Black;
      target.fill(box, fui::Paint::solid(shade), 3);
    }
  }
  screen.frame().hit(reading, OPEN_BOOK, STATISTICS, fui::InputTouch);

  // ---- Card 3: recently read, three paged rows independent of card 1. ----
  const auto recentArea = screen.body();
  const auto recentCard = fui::Rect{recentArea.x, recentArea.y, recentArea.width, recentArea.height};
  if (recentCard.height <= smallHeight * 2) return;
  target.stroke(recentCard, black, 1, 10);
  auto list = recentCard;
  list.x = static_cast<int16_t>(list.x + cardPad);
  list.width = static_cast<int16_t>(list.width - cardPad * 2);
  list.y = static_cast<int16_t>(list.y + cardPad);
  list.height = static_cast<int16_t>(list.height - cardPad * 2);
  target.text(fui::Rect{list.x, list.y, static_cast<int16_t>(list.width - smallHeight * 3),
                        static_cast<int16_t>(smallHeight)},
              tr(STR_RICKY_RECENT_READ), strong);
  const int recentCount = std::max(0, static_cast<int>(books->size()) - 1);
  const int pages = std::max(1, (recentCount + RickyHomeLayout::PAGE_BOOKS - 1) / RickyHomeLayout::PAGE_BOOKS);
  if (pages > 1) {
    char pageText[16];
    snprintf(pageText, sizeof(pageText), "%d / %d",
             std::clamp((recentStart - 1) / RickyHomeLayout::PAGE_BOOKS + 1, 1, pages), pages);
    target.text(fui::Rect{static_cast<int16_t>(list.right() - smallHeight * 3), list.y,
                          static_cast<int16_t>(smallHeight * 3), static_cast<int16_t>(smallHeight)},
                pageText, right);
  }
  const int rowTop = list.y + smallHeight + gap / 2;
  const int rowHeight = (list.bottom() - rowTop) / RickyHomeLayout::PAGE_BOOKS;
  const int count = std::min<int>(RickyHomeLayout::PAGE_BOOKS, std::max<int>(0, recentCount - (recentStart - 1)));
  const int thumbHeight = std::max<int>(smallHeight * 2, rowHeight - gap);
  const int thumbWidth = thumbHeight * 3 / 4;
  for (int i = 0; i < count; ++i) {
    const int index = recentStart + i;
    if (index <= 0 || index >= static_cast<int>(books->size())) break;
    const auto& item = (*books)[index];
    const int y = rowTop + i * rowHeight;
    const fui::Rect row{list.x, static_cast<int16_t>(y), list.width, static_cast<int16_t>(rowHeight - 2)};
    const fui::Rect thumb{list.x, static_cast<int16_t>(y), static_cast<int16_t>(thumbWidth),
                          static_cast<int16_t>(thumbHeight)};
    paint(thumb, index);
    if (showSelection && selected == index) target.stroke(thumb, black, 2, 4);
    auto textX = static_cast<int16_t>(thumb.right() + gap);
    const int textWidth = row.right() - gap * 2 - textX;
    target.text(fui::Rect{textX, static_cast<int16_t>(y + gap / 2), static_cast<int16_t>(textWidth),
                          static_cast<int16_t>(smallHeight)},
                titleOf(item), strong);
    const auto* saved = READING_STATS.findMatchingBookForPath(item.path);
    if (saved)
      snprintf(statsText, sizeof(statsText), tr(STR_RICKY_READ_PROGRESS),
               static_cast<unsigned>(saved->lastProgressPercent));
    target.text(fui::Rect{textX, static_cast<int16_t>(y + gap / 2 + smallHeight),
                          static_cast<int16_t>(textWidth), static_cast<int16_t>(smallHeight)},
                saved ? statsText : tr(STR_RICKY_READ_UNREAD), small);
    RickyPageUi::chevron(target,
                         fui::Rect{static_cast<int16_t>(row.right() - 20),
                                   static_cast<int16_t>(y + (thumbHeight - smallHeight) / 2), 20,
                                   static_cast<int16_t>(smallHeight)});
    if (i + 1 < count)
      target.fill(fui::Rect{textX, static_cast<int16_t>(y + rowHeight - 2),
                            static_cast<int16_t>(row.right() - textX), 1}, rule);
    screen.frame().hit(row, OPEN_BOOK, index, fui::InputTouch);
  }
}
#endif
