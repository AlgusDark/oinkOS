# Fast page refresh on the Xteink X3: what InkPointX and CrossInk do, and what we can take

Scope: EPUB page turns and Home cover navigation on the Xteink X3 (ESP32-C3, no
PSRAM, 792x528 panel, UC8253 or UC8279d controller). Our tree is oinkOS
`main` with the FreeInk SDK at `fb7e926`.

Sources:

- InkPointX app at `d272143`, its SDK fork `yokki-vans/community-sdk` at
  `01e429fe`.
- CrossInk app at `9914146e`, its SDK `Free-Ink/freeink-sdk` at `699370183`.
  That commit is an ancestor of our `fb7e926`.
- Our repository and SDK, a download-time EPUB optimizer (section 7), and
  Android e-ink prior art from the `mihon-eink` and `arco` projects.

Nothing here was built, flashed, or timed for this note. Millisecond figures
come from code comments, commit messages, and release notes, and each one names
its source. "Estimate" marks arithmetic from cited code.

Related note: [page-navigation-ram.md](page-navigation-ram.md) (RAM is not the main page-turn
lever). This note agrees with it and adds the controller side.

---

## 1. TL;DR

- Neither fork has faster waveforms than ours. The X3 LUT bytes that are loaded
  are identical in all three SDKs. CrossInk's `Uc8253X3Luts.h`,
  `Uc8279X3Luts.h`, and `UltraChipDirectGrayLuts.h` match ours byte for byte.
  InkPointX's loaded banks match too.
- A fast X3 page turn is a full-frame differential update. The controller keeps
  the previous frame in its own RAM (DTM1 / OLD), the host writes the new frame
  to DTM2, and the short LUT drives only pixels that changed. No second host
  framebuffer is involved.
- What made InkPointX feel fast was keeping the driver on that short LUT. Its
  "blink and lag" bug turned every key press into a full refresh of about
  3.1 s. After the fix a fast UC8253 frame took about 200 to 400 ms in one field
  trace (`InkPointX/docs/releases/v2.3.6.md:23-26`).
- Our SDK is missing several of those state fixes, and it also rolled back the
  UC8253 async split and the one-boot-clear change that CrossInk ships
  (`freeink-sdk` commit `c27da30`). The rollback message says boot and refresh
  latency will increase and that no hardware validation was done.
- A next-page bitmap in RAM does not fit while reading. One X3 frame is
  52,272 bytes, and the reading heap is about 43 to 50 KB free
  (`page-navigation-ram.md` section 3.2). Neither fork does it.
- An SD bitmap cache of rendered pages does not remove the costs that matter.
  Those are the waveform, the grayscale pass, and the periodic cleanup chain.
  Neither fork does it either. Both cache layout on SD, which we already do.
- The new Home carousel file cache can show stale covers after a book is
  replaced at the same path. Its key hashes thumbnail path strings, not file
  identity (`src/activities/home/HomeActivity.cpp:552-562`).

---

## 2. How an X3 page turn works today (all three trees)

Two controllers share the same 792x528 glass. The SDK picks one at boot
(`FreeInkDisplay::selectDriver()`). The framebuffer is 99 x 528 = 52,272 bytes
(`X3_BUFFER_SIZE`, `freeink-sdk/libs/display/FreeInkDisplay/include/FreeInkDisplay.h:86`).
`platformio.ini:42` builds single-buffer mode.

| Step | UC8253 (original X3) | UC8279d (newer X3, blank MTP) |
| --- | --- | --- |
| Fast turn | `_fast` bank ("papyrix turbo", community LUT, `Uc8253X3Luts.h:60-63`), write DTM2, refresh, copy frame to DTM1 | `BW_DU` bank from stock `update.bin`, same plane flow |
| Full or forced resync | Fill DTM1 white, `_full` OEM bank, `delay(200)`, one `_normal` condition pass, a no-op `_fast` settle (`Uc8253X3Driver.cpp:173-288`) | One `BW_GC` refresh, no extra passes (`Uc8279Driver.cpp:120-233`) |
| Boot | `_initialFullSyncsRemaining = 2` (`Uc8253X3Driver.cpp:162`) | `_initialFullsRemaining = 2` (`Uc8279Driver.cpp:109`) |

Reader policy (`src/activities/reader/ReaderUtils.h:180-191`) uses FAST on most
turns and HALF every N pages (default 15, `CrossPointSettings.h:335`). On X3,
`HalDisplay::displayBuffer(HALF)` calls `requestResync(1)` first
(`lib/hal/HalDisplay.cpp:62-65`). On UC8253 that turns the cleanup page into the
whole full-sync chain above, so it is several waveforms long.

Text anti-aliasing is on by default (`CrossPointSettings.h:299`). An AA page is
a grayscale base waveform, then 7 strips x 2 planes rendered into a
7,920-byte scratch (`EpubReaderActivity.cpp:2214-2318`), then a gray waveform.
Both X3 drivers report `asyncBase = false`, so the two-plane 104 KB overlap
buffer is never allocated on X3.

SPI wire time, estimate: one plane at 10 MHz is 52,272 x 8 / 10e6 = 41.8 ms. A
warm fast turn writes two planes (DTM2, then the DTM1 sync). An AA turn writes
about six.

Historical timings, measured on hardware before our rollback: warm FAST 436 ms,
and the same paint promoted to a full sync 2989 ms (SDK commit `b17beee`). The
UC8253 waveform window is described as about 130 to 770 ms (`61aa2aa`).

---

## 3. What InkPointX changed (v2.3.5 and v2.3.6)

Commits: app `f56fa33` and `9888f7c`, SDK `57aa571` and `01e429f`.

**The blink loop.** The driver re-sent power-on (PON, `0x04`) on every full
sync. On field units, PON while the charge pump is already on never toggles
BUSY, so `waitBusy(" X3_PON")` hit its 1000 ms ceiling every frame. The facade
then called `begin()`, which reset the boot-full counter to 2. One success only
drained it to 1, so the next frame was full again, forever. Fix: send PON only
when the pump is off.

**Start-edge handling.** UC8279 polled BUSY for only 50 ms after the refresh
command, then carried on as if the waveform had started. A slow start looked
like a failure, the next frame re-initialised the controller, and the retry was
a strong waveform. Fix: wait up to 3000 ms for the start edge
(`X3RefreshPolicy.h:16`, `EpdBus.cpp:153-177`). On a timeout, keep the fast
baseline, retry the same frame once, and fully recover only after that
(`FreeInkDisplay.cpp:572-663`).

**State fixes underneath (v2.3.5).** Rewrite the full 792x528 partial window
before every UC8279 B/W frame, because a grayscale strip window survives
PTOUT. Seed an unknown OLD plane with the inverted target under GC, not white,
because white OLD ghosts black pixels that should turn white. Do not mark a
frame valid after a failed grayscale write. Do not downgrade an explicit
UC8253 Full on power-on.

InkPointX also sets X3 SPI to 20 MHz (`BoardConfig.h:739-743`). Its comment
says 16 MHz was about 25% slower on plane writes.

---

## 4. Gaps in our SDK, verified against `fb7e926`

Each row was read in our tree.

| Gap | Our code | InkPointX | Effect named in their notes |
| --- | --- | --- | --- |
| Any frame becomes Half when the screen is off, including an explicit Full | `Uc8253X3Driver.cpp:184-186` | Fast to Half only if the pump is off and OLD RAM is unsynced (`Uc8253X3Driver.cpp:180-188`) | Wrong waveform on wake |
| Seamless wake marks controller RAM as a valid baseline | `Uc8253X3Driver.cpp:555-557`, `Uc8279Driver.cpp:242-244` | Clears the boot counter only. Reset RAM is not a baseline | Fast diff against unknown RAM, so ghosting |
| UC8279 waits 50 ms for the waveform to start, then continues | `Uc8279Driver.cpp:193-197` | 3000 ms start wait with a sticky timeout flag | False failure, re-init, strong waveform |
| UC8279 seeds unknown OLD with white | `Uc8279Driver.cpp:167-173` | Inverted target under GC (`Uc8279Driver.cpp:165-170`) | Ghosted black splash |
| UC8279 trusts the window set at init | `Uc8279Driver.cpp:165` | Full PTL before every B/W frame | Narrow strip window can stick |
| `waitBusy` result ignored, no retry | `EpdBus.cpp:363-385`, `FreeInkDisplay.cpp:679-715` | Fail closed, soft retry, then recover | Committing DTM1 for a frame that never ran |
| UC8253 async split, ISR wait, one boot clear | Rolled back in `c27da30` | CrossInk keeps all three. InkPointX keeps the split and uses two boot clears | Latency, per the rollback message |

We already send PON only when the screen is off (`Uc8253X3Driver.cpp:225-231`),
so the v2.3.6 loop itself is not present. The table rows are the remaining
differences.

These are SDK changes. The submodule tracks `Free-Ink/freeink-sdk`, so they
belong upstream, not as a local patch.

---

## 5. Feasibility of the four ideas

| Idea | RAM cost on X3 | What it would save | Verdict |
| --- | --- | --- | --- |
| Next-page bitmap prefetch in RAM | 52,272 bytes more, while the reading heap is about 43 to 50 KB free | Render time of one page. This is not the waveform, which is the main cost | Does not fit. Neither fork does it. mihon-eink's prepared frames work because Android has the RAM |
| Rendered-page bitmap cache on SD | Small RAM, about 52 KB SD write per page (about 157 KB with gray planes) | B/W render only. The AA pass still re-renders 14 strips, and the waveform is unchanged | Not worth it. It also needs invalidation on every font or layout setting |
| Dirty-rectangle partial refresh for page turns | None | Little. A text page changes almost every row, and the differential LUT already skips unchanged pixels | No for page turns. Yes for UI regions such as Home tab highlights and menus, which already use windows |
| SD data caching | Already in use | Layout per chapter (`sections/<spine>.bin`), covers, `.pxc` image pixels | Keep. Fix invalidation (section 6) |

Ideas that do target the real cost:

1. **Measure per controller.** `EpdBus.cpp:387-388` already logs
   `Wait complete: X3_DRF (N ms)` over serial. Record FAST, AA, and cleanup
   turns on both a UC8253 and a UC8279 unit before changing anything.
2. **Port the InkPointX state fixes upstream** (section 4). No RAM. Expected
   effect is fewer surprise full refreshes and less ghosting, not a faster
   steady-state turn.
3. **Decide the `c27da30` rollback with its author.** It was a diagnostic for
   "persistent residue". Re-enabling the split restores CPU and waveform
   overlap with no shadow buffer: the framebuffer must stay untouched until
   `displayFinish`, which re-reads it to sync DTM1 (CrossInk
   `Uc8253X3Driver.h:122-126`).
4. **Try SPI at 16 or 20 MHz.** Estimate: about 21 ms saved per plane at
   20 MHz, so about 40 ms on a fast turn and about 125 ms on an AA turn. The SD
   card shares the bus, so this needs a stress test on real cards.
5. **Make the UC8253 cleanup page one full scan.** Today it is a full,
   `delay(200)`, a condition pass, and a settle. With AA on it also adds
   `preconditionGrayscale` and the gray waveform. The Android lesson below says
   the cleanup should be a single full scan of the new page. This is a panel
   behaviour change and needs hardware ghosting checks.
6. **Anti-aliasing is a user-facing speed setting on X3.** With AA off a turn is
   one FAST waveform. With AA on it is a base waveform plus a gray waveform.

Do not take:

- A second 52 KB shadow buffer so the reader can draw during the waveform.
  InkPointX sets `needsFrameForFinish` to prevent exactly that.
- InkPointX `factoryP1`/`factoryP2` banks. They need the grayscale rail
  settings and wash the panel gray on B/W rails (`Uc8253X3Luts.h:130-139`).
- An automatic clean every 8 UI frames. InkPointX turned it off because it
  flashed the panel every ninth menu action (`ActivityManager.cpp:312-317`).
- The X4 SSD1677 `0x5A` "90 C" temperature byte. It is an X4 command. X3 LUTs
  have no temperature table.

---

## 6. Cover cache and invalidation

### What we have

- The book cache folder is `/.crosspoint/epub_<std::hash(path)>`
  (`lib/Epub/Epub.cpp:28-29`). Thumbnails are `thumb_<height>.bmp` inside it,
  and they are generated only when the file is absent (`Epub.cpp:943-961`).
- `clearBookCache` deletes that folder (`src/util/BookCacheUtils.cpp:24-32`).
  Web upload, WebDAV PUT, HTTP rename, WebDAV MOVE, web delete, WebDAV DELETE,
  and the file browser delete all reach it. File browser rename moves the
  folder (`FileBrowserActivity.cpp:32-42`).
- `book.bin` stores no source size or mtime (`BookMetadataCache.cpp:201-219`).
  TXT does compare size (`lib/Txt/Txt.cpp:121-164`).

### Problems

1. **Carousel file cache, same-path replace.** `tabhome_carousel_<i>.bin` is
   keyed by FNV-1a of the three `coverBmpPath` strings, the book count, the band
   height, and inversion (`HomeActivity.cpp:552-562`). After an upload replaces
   a book, `clearBookCache` deletes and rebuilds the thumbnail, but the path is
   unchanged, so the key matches and Home restores the old band. Nothing deletes
   `tabhome_carousel_*.bin`. A delete is mostly safe, because the recent-books
   list changes and the key misses.
2. **Replacement outside the firmware.** Copying a new EPUB over the old path on
   a computer bypasses `clearBookCache`. Layout, `book.bin`, and thumbnails stay
   stale until a version or settings mismatch. CrossInk has the same gap for its
   render cache (`CrossInk/docs/data-cache.md:66-81`).

### Prior art to copy

CrossInk's library metadata cache keys on path FNV-1a plus file size plus packed
FAT mtime, and never stores `mtime == 0` (`CrossInk/lib/LibraryIndex/LibraryMetadataCache.cpp:232-251`).
A size or mtime mismatch is a miss.

Smallest fixes, in order:

- Fold each shown book's EPUB size and mtime (or its thumbnail's) into
  `carouselCacheKey`. A replace then misses even though the path is the same.
- Optionally have `clearBookCache` remove `tabhome_carousel_*.bin`. With the
  key fix this is only cleanup, because the slots are indexed by position and
  get overwritten.
- For problem 2, store source size and mtime in `book.bin` and treat a mismatch
  as `clearBookCache`. That is a cache format change and needs a
  `BOOK_CACHE_VERSION` bump (`.agents/rules/generated-files-cache.md`).

How to verify: open Home with three recents, upload a different EPUB with the
same file name over the centre book, return to Home. Today the old cover band
appears. With the fix the new cover appears after one rebuild.

---

## 7. Download-time EPUB preprocessing

An OPDS proxy can optimize books as the reader downloads them. The one studied
here handles `GET /api/v1/opds/<id>/download` requests whose User-Agent starts
with `CrossPoint`. For each JPEG or PNG
that is progressive, larger than 528x792, or not grayscale, it composites onto
white, converts to grayscale, downsizes to fit 528x792, and re-encodes as a
baseline JPEG at quality 85 or a 16-level PNG. Text, CSS, and fonts are left
alone. Zip timestamps are fixed so the KOReader partial MD5 stays stable. It
falls back to the original on any error, Range request, or book over 64 MiB.

Why it matters on device: our JPEG decoder uses DC coefficients only for
progressive images, and 1184 of 2714 EPUBs (44%) in one sampled library
contain one. This step already removes the biggest image-decode cost before the book
reaches the card. It does not affect text page turns.

Possible next step for the proxy, not yet built: pre-dither images to the panel's
real levels (1-bit, or the 4 gray levels of the AA LUT) with an ordered dither,
so the device does not need to dither at render time. Arco dithers to 1-bit
(`arco/.../ImageTransformPipeline.kt:220-255`). Ordered dither stays stable
under partial updates, while Floyd-Steinberg changes pixels away from an edit
(`mihon-eink/docs/eink/research-image-quality.md:108-122`).

---

## 8. Lessons from Android e-ink work that transfer

Device for these numbers is a Boox Note3. Android `UpdateMode` maps to a choice
of LUT and partial or full scan on our controller.

- **One panel update per turn, with the image ready first.** Mihon went from
  3.00 updates per turn to 1.00, with the first update 23 to 90 ms after finger
  up (`mihon-eink/docs/eink/traces/fork-prepared-frames/summary.txt`,
  `fork-acceptance-tap-latency/summary.txt`). On X3 the same rule means: finish
  the B/W render, start one waveform, and never start a scan from a half-drawn
  buffer.
- **Full refresh every N, folded into that turn.** mihon-eink PR #100 replaced a
  black flash plus a page scan (2.00 updates per turn) with one GC scan of the
  new page on every Nth turn, and the GC turn did not start later
  (`mihon-eink/docs/eink/page-turn-measurement.md:198-216`). This is the
  argument for idea 5 in section 5.
- **No animation.** Every intermediate frame is another LUT scan
  (`page-turn-measurement.md:128-152`, `arco/.../ArcoNavHost.kt:28-31`).
- **Fast LUT only while moving.** A2 or DU during a gesture, then one normal
  update at rest. On X3 that applies to scrolling lists, not page turns.
- **Does not transfer.** REAGL (no such waveform on a 1-bit UC8253 set, and it
  showed no HAL change on the Note3), 16-level dithering (a Carta feature), and
  ARGB page caches (about 10 MB per Note3 frame).
- **Measurement gap.** None of these repos measured ghosting itself. A
  framebuffer dump cannot show it. The missing tool is a camera, or a known
  previous-image and next-image pair plus BUSY timing.

---

## 9. Open questions

- Steady-state timings on the current blocking UC8253 driver. The 436 ms and
  2989 ms figures predate `c27da30`.
- Which controller each test unit has. Cleanup cost differs a lot between
  UC8253 (multi-pass) and UC8279 (one GC).
- What residue `c27da30` was diagnosing. Re-enabling the split needs that
  context.
- Whether 16 or 20 MHz SPI is reliable on the shared display and SD bus.
- UC8279 `XTF_AA` is still marked pending hardware validation in the LUT header
  in both trees.
- No ghosting-versus-N data exists for any device here.
