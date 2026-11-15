#include "BookCacheUtils.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <Logging.h>
#include <Xtc.h>

#include <algorithm>

#include "ClippingStore.h"
#include "RecentBooksStore.h"
#include "components/TabHome.h"

bool isBookCacheDirectoryName(const char* name) {
  if (!name) {
    return false;
  }

  constexpr char EPUB_PREFIX[] = "epub_";
  constexpr char TXT_PREFIX[] = "txt_";
  constexpr char XTC_PREFIX[] = "xtc_";

  return strncmp(name, EPUB_PREFIX, std::size(EPUB_PREFIX) - 1) == 0 ||
         strncmp(name, TXT_PREFIX, std::size(TXT_PREFIX) - 1) == 0 ||
         strncmp(name, XTC_PREFIX, std::size(XTC_PREFIX) - 1) == 0;
}

void clearBookCache(const std::string& path) {
  if (FsHelpers::hasReflowableBookExtension(path)) {
    Epub(path, "/.crosspoint").clearCache();
  } else if (FsHelpers::hasXtcExtension(path)) {
    Xtc(path, "/.crosspoint").clearCache();
  } else {
    return;
  }
  LOG_DBG("BookCache", "Done checking metadata cache for: %s", path.c_str());
  // Home skips missing files when filling the carousel, so a shown book can sit anywhere in the recents list.
  const auto& recents = RECENT_BOOKS.getBooks();
  if (std::any_of(recents.begin(), recents.end(), [&](const RecentBook& book) { return book.path == path; })) {
    TabHome::clearCarouselCache();
  }
}

bool removeBookFile(const std::string& path) {
  if (!Storage.remove(path.c_str())) return false;
  clearBookCache(path);
  return ClippingStore::deleteForFilePath(path, "epub");
}
