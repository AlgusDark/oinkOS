#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/themes/BaseTheme.h"

class GfxRenderer;

namespace TabHome {

enum class Layout : uint8_t { Recents, Carousel };

constexpr int MAX_BOOKS = 5;
constexpr int MAX_TABS = 5;

struct Tab {
  const char* label;
  const uint8_t* icon;
};

int bookCount(Layout layout);
// Thumb heights a book needs: covers are drawn 1:1, so every slot size it can
// occupy needs its own cached BMP.
int thumbHeights(Layout layout, int bookIndex, int out[2]);
Rect coverArea(const GfxRenderer& renderer, Layout layout);

// Reads cover BMPs from the SD card; the result is snapshotted by the caller.
void drawCovers(const GfxRenderer& renderer, Layout layout, const std::vector<RecentBook>& books, int centerBook);
// Cheap per-press layer drawn over the restored cover snapshot.
void drawBookDetails(const GfxRenderer& renderer, Layout layout, const std::vector<RecentBook>& books,
                     const int8_t* progress, int centerBook, int selectedBook);
void drawTabBar(const GfxRenderer& renderer, const Tab* tabs, int count, int selectedTab);

// Bump when drawCovers changes for the carousel, so old bands are not shown.
constexpr int32_t CAROUSEL_CACHE_VERSION = 2;
std::string carouselCachePath(int centerBook);
// The band spans three books, so any book cache clear drops every slot.
void clearCarouselCache();

}  // namespace TabHome
