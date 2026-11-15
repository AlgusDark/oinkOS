#include "TabHome.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "UITheme.h"
#include "fontIds.h"

namespace TabHome {
namespace {

constexpr int COVER_RADIUS = 8;
constexpr int COVER_LINE = 3;
constexpr int SELECTION_GAP = 5;
constexpr int SELECTION_LINE = 4;
constexpr int TAB_BAR_HEIGHT = 84;
constexpr int ICON_SIZE = 32;

// Carousel: tallest 2:3 center cover that leaves room for the details above
// the tab bar; side covers at 70% run off the screen edges.
constexpr int CAROUSEL_TOP = 50;
constexpr int CENTER_H = 500;
constexpr int CENTER_W = CENTER_H * 2 / 3;
constexpr int SIDE_H = CENTER_H * 7 / 10;
constexpr int SIDE_W = CENTER_W * 7 / 10;
constexpr int SIDE_HIDDEN = 60;

// Recents: current-book card, then three recent tiles with progress.
constexpr int SIDE_PAD = 24;
constexpr int HERO_TOP = 56;
constexpr int HERO_H = 300;
constexpr int HERO_W = HERO_H * 2 / 3;
constexpr int HERO_INSET = 12;
constexpr int TILES_LABEL_TOP = HERO_TOP + HERO_H + 2 * HERO_INSET + 16;
constexpr int TILE_TOP = TILES_LABEL_TOP + 24;
constexpr int TILE_H = 180;
constexpr int TILE_W = TILE_H * 2 / 3;
constexpr int RECENT_TILES = 3;

Rect centerSlot(const GfxRenderer& renderer) {
  return Rect{(renderer.getScreenWidth() - CENTER_W) / 2, CAROUSEL_TOP, CENTER_W, CENTER_H};
}

Rect sideSlot(const GfxRenderer& renderer, const int dir) {
  const Rect center = centerSlot(renderer);
  const int x = dir < 0 ? center.x + SIDE_HIDDEN - SIDE_W : center.x + center.width - SIDE_HIDDEN;
  return Rect{x, center.y + (CENTER_H - SIDE_H) / 2, SIDE_W, SIDE_H};
}

Rect heroSlot() { return Rect{SIDE_PAD + HERO_INSET, HERO_TOP + HERO_INSET, HERO_W, HERO_H}; }

Rect tileSlot(const GfxRenderer& renderer, const int tile) {
  const int span = renderer.getScreenWidth() - 2 * SIDE_PAD;
  const int gap = (span - RECENT_TILES * TILE_W) / (RECENT_TILES - 1);
  return Rect{SIDE_PAD + tile * (TILE_W + gap), TILE_TOP, TILE_W, TILE_H};
}

void drawCover(const GfxRenderer& renderer, const RecentBook& book, const Rect slot) {
  // Painting the slot white first lets a cover drawn later hide the ones behind it.
  renderer.fillRect(slot.x, slot.y, slot.width, slot.height, false);
  bool drawn = false;
  if (!book.coverBmpPath.empty()) {
    HalFile file;
    if (Storage.openFileForRead("HOME", UITheme::getCoverThumbPath(book.coverBmpPath, slot.height), file)) {
      Bitmap bitmap(file);
      drawn = bitmap.parseHeaders() == BmpReaderError::Ok && BaseTheme::drawCoverThumbFill(renderer, bitmap, slot);
      file.close();
    }
  }
  if (!drawn) {
    const auto lines = renderer.wrappedText(UI_12_FONT_ID, book.title.c_str(), slot.width - 24, 3, EpdFontFamily::BOLD);
    int y = slot.y + 16;
    for (const auto& line : lines) {
      renderer.drawText(UI_12_FONT_ID, slot.x + 12, y, line.c_str(), true, EpdFontFamily::BOLD);
      y += renderer.getLineHeight(UI_12_FONT_ID);
    }
  }
  renderer.drawRoundedRect(slot.x, slot.y, slot.width, slot.height, COVER_LINE, COVER_RADIUS, true);
}

void drawSelection(const GfxRenderer& renderer, const Rect slot) {
  renderer.drawRoundedRect(slot.x - SELECTION_GAP - SELECTION_LINE, slot.y - SELECTION_GAP - SELECTION_LINE,
                           slot.width + 2 * (SELECTION_GAP + SELECTION_LINE),
                           slot.height + 2 * (SELECTION_GAP + SELECTION_LINE), SELECTION_LINE,
                           COVER_RADIUS + SELECTION_GAP, true);
}

// Bar plus percentage; nothing when progress is unknown.
void drawProgress(const GfxRenderer& renderer, const int x, const int y, const int width, const int8_t percent) {
  if (percent < 0) return;
  renderer.drawRect(x, y, width, 6);
  const int fillWidth = (width - 4) * percent / 100;
  if (fillWidth > 0) renderer.fillRect(x + 2, y + 2, fillWidth, 2);
  char text[8];
  snprintf(text, sizeof(text), "%d%%", percent);
  renderer.drawText(SMALL_FONT_ID, x, y + 10, text, true);
}

void drawCentered(const GfxRenderer& renderer, const int fontId, const int y, const char* text, const int maxWidth,
                  const EpdFontFamily::Style style) {
  const std::string fitted = renderer.truncatedText(fontId, text, maxWidth, style);
  const int x = (renderer.getScreenWidth() - renderer.getTextWidth(fontId, fitted.c_str(), style)) / 2;
  renderer.drawText(fontId, x, y, fitted.c_str(), true, style);
}

void drawIconInk(const GfxRenderer& renderer, const uint8_t* icon, const int x, const int y, const bool black) {
  if (black) {
    renderer.drawIcon(icon, x, y, ICON_SIZE);
    return;
  }
  // drawIcon only paints black; same bit layout and Portrait mapping, white ink.
  constexpr int rowBytes = (ICON_SIZE + 7) / 8;
  for (int row = 0; row < ICON_SIZE; row++) {
    for (int col = 0; col < ICON_SIZE; col++) {
      if (((icon[row * rowBytes + (col >> 3)] >> (7 - (col & 7))) & 1) == 0) {
        renderer.drawPixel(x + (ICON_SIZE - 1 - row), y + col, false);
      }
    }
  }
}

}  // namespace

int bookCount(const Layout layout) { return layout == Layout::Carousel ? MAX_BOOKS : 1 + RECENT_TILES; }

int thumbHeights(const Layout layout, const int bookIndex, int out[2]) {
  if (layout == Layout::Carousel) {
    out[0] = CENTER_H;
    out[1] = SIDE_H;
    return 2;
  }
  out[0] = bookIndex == 0 ? HERO_H : TILE_H;
  return 1;
}

Rect coverArea(const GfxRenderer& renderer, const Layout layout) {
  if (layout == Layout::Carousel) return Rect{0, CAROUSEL_TOP, renderer.getScreenWidth(), CENTER_H};
  return Rect{0, HERO_TOP, renderer.getScreenWidth(), TILE_TOP + TILE_H - HERO_TOP};
}

void drawCovers(const GfxRenderer& renderer, const Layout layout, const std::vector<RecentBook>& books,
                const int centerBook) {
  const int count = static_cast<int>(books.size());
  if (count == 0) return;
  if (layout == Layout::Carousel) {
    if (count > 1) {
      drawCover(renderer, books[(centerBook + count - 1) % count], sideSlot(renderer, -1));
      drawCover(renderer, books[(centerBook + 1) % count], sideSlot(renderer, +1));
    }
    drawCover(renderer, books[centerBook], centerSlot(renderer));
    return;
  }
  drawCover(renderer, books[0], heroSlot());
  for (int i = 1; i < count && i <= RECENT_TILES; i++) drawCover(renderer, books[i], tileSlot(renderer, i - 1));
}

void drawBookDetails(const GfxRenderer& renderer, const Layout layout, const std::vector<RecentBook>& books,
                     const int8_t* progress, const int centerBook, const int selectedBook) {
  const int count = static_cast<int>(books.size());
  if (count == 0) return;
  const int screenW = renderer.getScreenWidth();

  if (layout == Layout::Carousel) {
    const Rect center = centerSlot(renderer);
    if (selectedBook >= 0) drawSelection(renderer, center);
    constexpr int DOT = 8;
    constexpr int DOT_GAP = 6;
    int dotX = (screenW - (count * DOT + (count - 1) * DOT_GAP)) / 2;
    const int dotY = center.y + center.height + 12;
    for (int i = 0; i < count; i++, dotX += DOT + DOT_GAP) {
      if (i == centerBook) {
        renderer.fillRect(dotX, dotY, DOT, DOT, true);
      } else {
        renderer.drawRect(dotX, dotY, DOT, DOT, true);
      }
    }
    const RecentBook& book = books[centerBook];
    int y = dotY + DOT + 6;
    if (!book.author.empty()) {
      drawCentered(renderer, UI_10_FONT_ID, y, book.author.c_str(), screenW - 80, EpdFontFamily::REGULAR);
      y += renderer.getLineHeight(UI_10_FONT_ID);
    }
    drawCentered(renderer, UI_12_FONT_ID, y, book.title.c_str(), screenW - 80, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID) + 4;
    drawProgress(renderer, (screenW - 200) / 2, y, 200, progress[centerBook]);
    return;
  }

  const Rect hero = heroSlot();
  if (selectedBook == 0) {
    renderer.drawRoundedRect(SIDE_PAD, HERO_TOP, screenW - 2 * SIDE_PAD, HERO_H + 2 * HERO_INSET, SELECTION_LINE, 14,
                             true);
  }
  const int textX = hero.x + hero.width + 20;
  const int textW = screenW - SIDE_PAD - HERO_INSET - textX;
  const auto titleLines = renderer.wrappedText(UI_12_FONT_ID, books[0].title.c_str(), textW, 3, EpdFontFamily::BOLD);
  int y = hero.y + hero.height / 2 - 50;
  for (const auto& line : titleLines) {
    renderer.drawText(UI_12_FONT_ID, textX, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID);
  }
  if (!books[0].author.empty()) {
    renderer.drawText(UI_10_FONT_ID, textX, y + 4,
                      renderer.truncatedText(UI_10_FONT_ID, books[0].author.c_str(), textW).c_str(), true);
    y += renderer.getLineHeight(UI_10_FONT_ID) + 12;
  }
  drawProgress(renderer, textX, y, std::min(200, textW), progress[0]);

  if (count > 1)
    renderer.drawText(SMALL_FONT_ID, SIDE_PAD, TILES_LABEL_TOP, tr(STR_TAB_RECENT), true, EpdFontFamily::BOLD);
  for (int i = 1; i < count && i <= RECENT_TILES; i++) {
    const Rect tile = tileSlot(renderer, i - 1);
    if (selectedBook == i) drawSelection(renderer, tile);
    drawProgress(renderer, tile.x, tile.y + tile.height + 10, tile.width, progress[i]);
  }
}

void drawTabBar(const GfxRenderer& renderer, const Tab* tabs, const int count, const int selectedTab) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int screenW = renderer.getScreenWidth();
  const int top = renderer.getScreenHeight() - metrics.buttonHintsHeight - TAB_BAR_HEIGHT;
  renderer.fillRect(0, top, screenW, 1, true);
  const int slotW = screenW / count;
  const int labelH = renderer.getLineHeight(SMALL_FONT_ID);
  const int contentY = top + (TAB_BAR_HEIGHT - ICON_SIZE - 4 - labelH) / 2;
  for (int i = 0; i < count; i++) {
    const int slotX = i * slotW;
    const bool selected = i == selectedTab;
    if (selected) renderer.fillRoundedRect(slotX + 3, top + 6, slotW - 6, TAB_BAR_HEIGHT - 12, 14, Color::Black);
    drawIconInk(renderer, tabs[i].icon, slotX + (slotW - ICON_SIZE) / 2, contentY, !selected);
    const std::string label = renderer.truncatedText(SMALL_FONT_ID, tabs[i].label, slotW - 8);
    renderer.drawText(SMALL_FONT_ID, slotX + (slotW - renderer.getTextWidth(SMALL_FONT_ID, label.c_str())) / 2,
                      contentY + ICON_SIZE + 4, label.c_str(), !selected);
  }
}

std::string carouselCachePath(const int centerBook) {
  return "/.crosspoint/tabhome_carousel_" + std::to_string(centerBook) + ".bin";
}

void clearCarouselCache() {
  for (int i = 0; i < MAX_BOOKS; i++) Storage.remove(carouselCachePath(i).c_str());
}

}  // namespace TabHome
