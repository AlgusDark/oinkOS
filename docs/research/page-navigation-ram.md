# Page navigation on the Xteink X3: is RAM the bottleneck?

Scope: EPUB page turns on an Xteink X3 (ESP32-C3, no PSRAM, 792x528 panel,
UC8253 or UC8279 controller). Branch `develop`. FreeInk SDK at the pinned
revision `fb7e926` (`git submodule status` shows no local drift).

Sources are this repository, the pinned SDK (code and git history), the
ESP-IDF 5.5.5 sources installed by PlatformIO (package
`framework-espidf`), the effective rebuilt sdkconfig (package
`framework-arduinoespressif32-libs`, `esp32c3/sdkconfig`),
docs.espressif.com, and linker maps from a baseline `gh_release` build and a
build without Bluetooth. "Measured" marks numbers from those two builds.
Everything marked
"estimate" is arithmetic from the cited code, not a device measurement.

Note on the sdkconfig: the inspected file was regenerated at 17:30 by the
no-Bluetooth experiment (`CONFIG_BT_ENABLED` is not set in it). The other
options cited from it come from `platformio.ini` or framework defaults, so they
should match `gh_release`. Treat that as an assumption.

---

## 1. TL;DR

**RAM is not the main lever for a normal text page turn on the X3.** On a
cached chapter with the built-in font, the time goes to e-ink waveforms, SPI
transfers at 10 MHz, the extra grayscale (anti-aliasing) passes, the periodic
"half" refresh (which the X3 UC8253 path turns into a full sync), and an SD
progress write after every page. None of these get faster with more free heap.

RAM matters in specific situations, through fixed heap thresholds in the code:

- SD-card fonts keep glyph data between pages only while free heap is at least
  40 KB (`lib/EpdFont/SdCardFont.cpp:119`, `:183-189`). Below that, every turn
  re-reads glyphs from SD.
- Image pages keep the decoded image in RAM across the ~13 render passes only
  if free heap is at least the image size plus 24 KB
  (`lib/Epub/Epub/blocks/ImageBlock.cpp:100-104`, `:147`). Otherwise every
  pass re-reads the image from SD.
- Background chapter layout pauses below 32 KB free or a 16 KB largest block
  (`src/activities/reader/EpubReaderActivity.h:135-136`). Then a turn near the
  end of the built pages runs layout synchronously.
- A Bluetooth remote takes about 49 KB while the radio runs
  (`lib/BlePageTurner/src/RadioPolicy.h:19-23`), and the radio is stopped for
  every chapter build (`EpubReaderActivity.cpp:380-404`).

**Top 5 levers, ranked by expected effect on page turns:**

1. **Turn text anti-aliasing off** (runtime setting). On X3 it removes one of
   two waveforms per page, about 4 of 6 full-plane SPI transfers (about 167 ms
   at 10 MHz, estimate), and 14 extra page renders.
2. **Lower the full-refresh frequency** (runtime setting, 30 pages or Never).
   On the UC8253 X3, each scheduled "half" refresh becomes a full sync with 3
   waveforms plus a fixed 200 ms delay. The SDK author measured about 3 s for a
   promoted full sync, compared with about 436 ms for a warm FAST refresh.
3. **Debounce the per-page progress save** (source change). Today every page
   does create + write + flush + remove + rename on the SD card while the
   render lock is held. The repository's own rule asks for this to be
   debounced (`.agents/rules/hardware-resources.md:42`).
4. **Cull whole lines per grayscale strip** (source change, anti-aliasing on
   only). Each of the 14 strip passes walks every glyph on the page before
   rejecting it (`lib/GfxRenderer/GfxRenderer.cpp:548-563`, `lib/Epub/Epub/Page.cpp:11-19`).
5. **Raise free heap above the reader's thresholds** (personal build config).
   Move FreeRTOS, heap, ringbuf, RMT and libc-lock code from IRAM to flash
   (about 20 KB, estimate). Drop Bluetooth if unused (−10,992 B static,
   measured). Disable light heap poisoning (12 B per live
   allocation). This helps only for SD fonts, image pages, chapter boundaries
   and Bluetooth users.

---

## 2. The page-turn path and where time goes

### 2.1 Input edge

- The main loop polls input. It sleeps 10 ms per pass
  (`src/main.cpp:1122`). After 3 s without input it drops the CPU to 10 MHz on
  C3 (`lib/hal/HalPowerManager.h:28-33`, `src/main.cpp:1109-1118`). It polls in
  10 ms slices and breaks early when a contact closes. A press restores full
  speed (`src/main.cpp:936`). The render task holds a power lock while it
  renders (`src/activities/ActivityManager.cpp:85`,
  `lib/hal/HalPowerManager.cpp:229-243`).
- The SDK debounce is 5 ms
  (`freeink-sdk/libs/hardware/InputManager/include/InputManager.h:527`).
- Page buttons fire on **press** only when "long-press behavior" is Off. Any
  other value fires on **release**, so the hold time adds to latency
  (`src/activities/reader/ReaderUtils.h:66-76`). The default is Off
  (`src/CrossPointSettings.h:351`).
- While a render is running, or within 200 ms of the last turn, a new press is
  stored as one pending turn. Later presses overwrite it. Turns do not queue up
  (`EpubReaderActivity.cpp:1004-1018`, `:1064-1068`).
- No log line records input-edge-to-render-start latency.

### 2.2 Page lookup (no layout on a cached chapter)

- `pageTurn()` only changes `currentPage`. At a chapter edge it resets the
  section (`EpubReaderActivity.cpp:1467-1508`).
- `renderBook()` opens the chapter's `.bin` cache. A complete cache skips
  layout (`EpubReaderActivity.cpp:1702-1826`).
- Each turn reads the page from SD: open the file, two LUT seeks, then
  `Page::deserialize` (`lib/Epub/Epub/Section.cpp:877-926`). There is no page
  object cache in RAM. Each line costs about 3 heap allocations: `PageLine`,
  `TextBlock` and its arena (`lib/Epub/Epub/Page.cpp:358-377`,
  `blocks/TextBlock.cpp:537-549`).
- Layout runs only when the chapter is not fully built. A background build
  runs 2 pages per idle loop frame, but only with ≥32 KB free and a ≥16 KB
  largest block (`EpubReaderActivity.cpp:406-454`, `.h:133-136`). A turn near
  the built edge, or onto an unbuilt page, builds synchronously in chunks of 8
  pages (`EpubReaderActivity.cpp:1864-1900`).

### 2.3 Glyph rendering and font caches

- Built-in reader fonts are Noto Sans 12/14/16/18, compressed in flash
  (`src/CrossPointSettings.cpp:395-420`). Other families are SD-card `.cpfont`
  files.
- Each render opens a `PrewarmScope`. It clears the glyph cache on entry and
  on exit (`lib/GfxRenderer/FontCacheManager.cpp:173-181`, `:221-226`). It
  scans the page, then decompresses exactly the needed glyphs
  (`EpubReaderActivity.cpp:2104-2111`).
- Built-in fonts (`FontDecompressor`): per page it allocates one buffer per
  font style. Per needed group it `malloc`s a temporary buffer of the group's
  uncompressed size, inflates it, copies the glyphs out, and frees it
  (`lib/EpdFont/FontDecompressor.cpp:366-375`, `:466-490`). Noto Sans 14
  groups are 7,056 / 8,689 / 13,305 B and up to 27,313 B
  (`lib/EpdFont/builtinFonts/notosans_14_regular.h:2905-2908`). `clearCache()`
  frees everything (`FontDecompressor.cpp:27-30`). **No glyph data survives to
  the next page**, so the idle prewarm of page N+1
  (`EpubReaderActivity.cpp:715-736`) is thrown away for built-in fonts.
- If a page-buffer allocation fails, rendering falls back to decompressing a
  whole group per cache miss into a single "hot group" buffer
  (`FontDecompressor.cpp:182-227`). This is slow, and with anti-aliasing it
  repeats over 15 render passes. This is a real low-heap slowdown, but it needs
  heap to fall below roughly 10–30 KB contiguous.
- SD fonts (`SdCardFont`): the glyph data survives the scope only while free
  heap is ≥40 KB. In that case "the idle prewarm of page N+1 serves the actual
  page turn with zero SD reads" (`lib/EpdFont/SdCardFont.cpp:118-122`,
  `:178-190`). The page glyph budget also shrinks with free heap
  (`SdCardFont.cpp:980-987`). Other point-size variants (headings) load only
  with a ≥48 KB largest block (`src/SdCardFontSystem.cpp:370-375`).

### 2.4 Grayscale (text anti-aliasing) on X3

Both X3 drivers report `stripUploads=true, asyncBase=false`
(`freeink-sdk/libs/display/FreeInkDisplay/src/driver/Uc8253X3Driver.h:64-74`,
`Uc8279Driver.h:62-72`). The reader therefore takes the tiled, non-overlapped
path (`EpubReaderActivity.cpp:2134-2141`, `:2185-2197`, `:2258-2307`):

1. A B/W render (after the prewarm scan render).
2. `displayGrayscaleBase(FAST)`. On UC8253 this writes DTM2, runs the
   "pre-BW mid" waveform, and writes DTM1
   (`Uc8253X3Driver.cpp:307-347`).
3. 2 planes × 7 strips of 80 rows. **Each strip re-renders the whole page**
   into a 7,920 B scratch buffer (99 × 80) and uploads it
   (`EpubReaderActivity.cpp:2204`, `:2259`, `:2273-2292`).
4. The gray waveform (`Uc8253X3Driver.cpp:445-495`).
5. Cleanup writes both planes again (`Uc8253X3Driver.cpp:497-527`).

Dark mode disables grayscale entirely
(`freeink-sdk/.../FreeInkDisplay.cpp:660-666`).

### 2.5 Display transfer and waveform

- The X3 display SPI clock is 10 MHz (`XTEINK_DISPLAY_SPI_HZ`,
  `freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h:1015`, used by
  the X3 profile at `:1070`). One full plane is 52,272 B, so it takes at least
  41.8 ms on the wire (52,272 × 8 / 10 MHz, estimate). Plane writes are one CS
  burst of row writes (`freeink-sdk/.../bus/EpdBus.cpp:477-483`).
- Plane transfers per page (estimate from the code paths above):
  - Text, anti-aliasing off: 2 planes (≈84 ms) + 1 FAST waveform
    (`Uc8253X3Driver.cpp:215-223`, `:263-265`).
  - Text, anti-aliasing on: 6 planes (≈251 ms) + 2 waveforms + 14 strip
    renders.
- Waveform time: the SDK commit `b17beee` (measured on X3 hardware) says "a
  FAST refresh that costs 436 ms once warm cost 2989 ms" when promoted to a
  full sync. Commit `61aa2aa` describes the X3 waveform as "~130-770 ms". The
  BUSY wait polls every 1 ms (`EpdBus.cpp:363-381`).
- **The X3 overlap was rolled back.** SDK commit `c27da30` (2 Nov 2026) is a
  "diagnostic rollback" of the X3 refresh split and the interrupt-driven BUSY
  wait. Its message says: "Boot and refresh latency will increase." So
  `freeink-sdk/docs/deferred-refresh-migration.md:97-99` ("the identical
  host-side flow overlaps on X3") is stale for the pinned revision. On X4 that
  overlap cut a page turn from 1,274 ms to 822 ms (same doc).
- **Periodic refresh on UC8253.** `HalDisplay::displayBuffer(HALF)` calls
  `requestResync(1)` on X3 (`lib/hal/HalDisplay.cpp:62-68`). The driver then
  runs a full OEM waveform from a white baseline, a fixed `delay(200)`, one
  conditioning pass, and a fast settle pass (`Uc8253X3Driver.cpp:194-281`,
  `:240`, `:547-553`). With anti-aliasing on, that page also runs
  `preconditionGrayscale` and the gray pass (`EpubReaderActivity.cpp:2186-2191`).
  That is about 5 waveforms + 200 ms + about 10 plane transfers. The default
  cadence is every 15 pages (`src/CrossPointSettings.h:332`,
  `CrossPointSettings.cpp:368-386`). An "Indexing" popup forces the next page
  onto this path (`EpubReaderActivity.cpp:413-418`, `:1730`, `:1771`).
- On the UC8279 X3, HALF is one GC waveform with no extra passes or delay
  (`Uc8279Driver.cpp:143-144`, `:235-239`).
- "Sunlight fading fix" powers the panel off after each page
  (`lib/GfxRenderer/GfxRenderer.cpp:1759-1775`). The next page then waits for
  power-on (`Uc8253X3Driver.cpp:226-231`). It is off by default
  (`src/CrossPointSettings.h:358`).

### 2.6 After the refresh

- `saveProgress()` runs after `renderContents()` returns, still under the
  render lock (`EpubReaderActivity.cpp:1976-1982`). It writes
  `progress.bin.tmp`, flushes, closes, removes `progress.bin`, and renames
  (`src/activities/reader/ProgressFile.h:32-62`). That is several FAT
  directory and table sector writes per page. The new page is already visible,
  but the next turn waits (section 2.1).
- Exit and sleep do not save progress separately
  (`src/activities/reader/ReaderActivity.cpp:97-129`). Correctness relies on
  the per-page save.

### 2.7 Image pages

- First view: a placeholder page is shown with a FAST refresh, then the image
  is decoded to a `.pxc` cache (`EpubReaderActivity.cpp:2154-2158`,
  `ImageBlock.cpp:312-416`). JPEG needs ≥36 KB free
  (`JpegToFramebufferConverter.cpp:104-105`). PNG needs ≥60 KB free
  (`PngToFramebufferConverter.cpp:86-87`) plus **one contiguous block of about
  59.5 KB** (estimate). That block is PNGdec's state: a 32 KB zlib window, an
  ~7.1 KB `inflate_state`, a 1 KB palette, the 16,416 B scanline buffer set by
  `-DPNG_MAX_BUFFERED_PIXELS=16416` (`platformio.ini:69`), and a 2 KB file
  buffer (`.pio/libdeps/gh_release/PNGdec/src/PNGdec.h:50`, `:157-163`;
  `inflate.h:125-127`; `inftrees.h:51-53`). The code budgets only 44 KB for it
  (`PngToFramebufferConverter.cpp:86`). With 43–50 KB free while reading,
  first-view PNG decode probably fails and shows a placeholder.
- Later views: the first pass loads the `.pxc` payload into RAM in 16 KB
  chunks (at most 96 KB). This works only if free heap ≥ remaining bytes +
  24 KB and the largest block ≥ chunk + 8 KB
  (`ImageBlock.cpp:100-104`, `:135-162`). Otherwise each pass that touches the
  image re-reads the whole file from SD, because the row stride is smaller
  than a sector (`ImageBlock.cpp:88-99`, `:224-283`). The code comment gives
  "~100 ms for a full-page image" per pass, over "~13 passes".
- A full-height X3 image is 198 B × 528 rows = 104,544 B. That is over the
  96 KB cap, so it always streams. A 400×300 image is about 30 KB. It needs
  about 54 KB free to stay in RAM, which is above typical reading heap
  (estimate).

### 2.8 Bluetooth page turner

- The radio starts only with the feature on, a bonded remote, and a book open
  (`lib/BlePageTurner/src/RadioPolicy.h:88-113`). It holds about 49,152 B
  (`RadioPolicy.h:19-23`).
- Every chapter build stops the radio first. The build waits up to 5 s more
  and is refused if the radio does not let go (`EpubReaderActivity.cpp:380-404`,
  `.h:153`). This is a stall at chapter boundaries, not on every page.

### 2.9 Summary: where RAM does and does not matter

| Cost | Fixed by more RAM? | Evidence |
| --- | --- | --- |
| Waveform time (1–2 per page, more on scheduled refresh) | No | §2.5 |
| SPI plane transfers at 10 MHz | No | §2.5 |
| 14 strip re-renders with anti-aliasing | Partly: bigger strips mean fewer passes | §2.4 |
| SD progress write per page | No | §2.6 |
| Built-in font decompression per page | Only with a code change to retain glyphs | §2.3 |
| SD-font glyph reads per page | Yes, at ≥40 KB free | §2.3 |
| Image re-reads per pass | Yes, at image size + 24 KB free | §2.7 |
| Synchronous layout near the built edge | Yes, at ≥32 KB free and a 16 KB block | §2.2 |
| Bluetooth radio stop/start at chapter builds | Yes (≈49 KB) | §2.8 |

---

## 3. RAM budget

### 3.1 Static (from the map, `gh_release` baseline)

From `esp_idf_size` on the baseline map:

| Region | Bytes |
| --- | --- |
| DRAM total (shared IRAM+DRAM) | 321,296 |
| IRAM code (`.text` in DRAM) | 69,244 |
| `.bss` | 44,944 |
| `.data` | 21,517 |
| Used | 135,705 |
| Remaining for heap | 185,591 |
| RTC: `.rtc_noinit` of 8,192 | 5,532 |

- IRAM ends at `ALIGN(0x200)` (`dual.map:71674`). **IRAM savings reach the
  heap in 512-byte steps.** `.dram0.dummy` reserves 0x11000 (69,632 B) of DRAM
  for IRAM (`dual.map:71703`). The heap starts at `0x3fca13a0`
  (`dual.map:134706`).
- On C3, unused IRAM becomes DRAM heap
  ([Memory Types, ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32c3/api-guides/memory-types.html)).

Largest IRAM users (per-archive DRAM minus `.bss` minus `.data`):

| Component | IRAM B | Notes |
| --- | --- | --- |
| FreeRTOS (`tasks.c` 6,236, `queue.c` 4,106, others) | 14,044 | `CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH` is off (sdkconfig:1870) |
| spi_flash | ~9,300 | Must stay in IRAM (runs with cache disabled) |
| heap (`tlsf.c` 5,196, `heap_caps_base` 928, `multi_heap_poisoning` 644, `multi_heap` 512, `heap_caps` 462) | 7,742 | `CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH` is off (sdkconfig:1920) |
| hal (IDF) | ~6,970 | Several knobs; see the HAL assertion item |
| esp_hw_support | ~6,930 | Mostly required |
| ringbuf | 4,812 | `CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH` is off (sdkconfig:1549) |
| phy | ~3,630 | `ESP_PHY_IRAM_OPT` "only affect esp32c2" (`esp_phy/Kconfig:199-205`) |
| RMT (`rmt_tx` 1,674, `rmt_rx` 672, encoders 360) | 2,706 | Linked only via Arduino `rgbLedWrite` (`dual.map:198933-198953`) |
| spi_flash chip drivers | 1,496 | Needs the X3 flash vendor to trim |
| esp_timer | 1,192 | `CONFIG_ESP_TIMER_IN_IRAM=y` |
| libc locks | 796 | `CONFIG_LIBC_LOCKS_PLACE_IN_IRAM=y` (sdkconfig:2362) |

Largest `.bss`/`.data` items: IDF FreeRTOS 3.1 KB, net80211 7.7 KB, lwip
4.4 KB, wl_cnx 3.9 KB, mdns 2.0 KB. App objects total about 26.6 KB. The
biggest are `refreshSharedUiThemeTokens` pool 3,448 B (two `ThemeTokens`,
intentional; `src/components/UiAppHelpers.h:20-54`), `BleKeyboardHost`
2,152 B, `fontCacheManager` 2,132 B, `fontDecompressor` 1,504 B, and
`SDCardManager` 1,200 B. None is a large lever.

RTC: a 4,096 B log ring `logMessages` (`lib/Logging/Logging.cpp:9-13`,
`dual.map:69706`) and 1,408 B of panic capture (`dual.map:69708-69713`). RTC
fast memory left free joins the heap (`CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP=y`,
sdkconfig:1580).

### 3.2 Known runtime allocations while reading (X3)

| Item | Bytes | Lifetime | Evidence |
| --- | --- | --- | --- |
| Framebuffer | 52,272 | Permanent | `FreeInkDisplay.cpp:285`; `hardware-resources.md` |
| Render task stack | 8,192 | Permanent | `ActivityManager.cpp:61` |
| Arduino loop task stack | 8,192 | Permanent | sdkconfig:852; the 24 KB override is vector-font boards only (`src/main.cpp:56-63`) |
| esp_timer task | 4,096 | Permanent | `platformio.ini:205` |
| FreeRTOS timer task | 2,560 | Permanent | `platformio.ini:206` |
| Idle task | 2,304 | Permanent | sdkconfig:1825 |
| Grayscale strip scratch | 7,920 | Per AA page | `EpubReaderActivity.cpp:2259` |
| Font page slots + group temp | ~5–30 K | Per page | §2.3 |
| Page object (~3 allocations per line) | a few KB | Per page | §2.2 |
| Image `.pxc` slot | ≤96 K | Per image page, if it fits | §2.7 |
| Toolbar overlay snapshot | 52,272 in 8 KB chunks | While the overlay is open | `GfxRenderer.cpp:2485-2511`, `.h:48` |
| Bluetooth stack | ≈49,152 | While the radio is up | `RadioPolicy.h:19-23` |
| Layout build: inflate ring | 4 × 8 KB | During builds | `lib/InflateReader/InflateReader.h:53-60` |

Observed free heap while reading: about 50 KB (`platformio.ini:105`) and
"~43 KB free" (SDK commit `08073b3`). The heap at boot is about 185 KB. The
listed permanent items take about 78 KB. That leaves roughly 55–60 KB of
book state, font tables, settings and SdFat buffers that this report could
not break down without a device heap census (see §7).

---

## 4. Ranked opportunities

Kinds: **(a)** user-visible runtime setting, **(b)** build
configuration in the `x3fork` environment only, **(c)** source change.
"Upstream?" says whether the change suits the shared project, or only a
personal X3 build.

| # | Lever | Kind | Helps when | Estimated gain | RAM effect | Risk | Upstream? |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | Text anti-aliasing off | a | Every page | −1 waveform, −~167 ms SPI, −14 renders per page | −7.9 KB transient | Rougher text edges | Setting exists |
| 2 | Refresh frequency 30 or Never | a | Every Nth page (UC8253) | Avoids a ~3 s page every 15 | none | More ghosting | Setting exists |
| 3 | Debounce progress save, flush on exit/sleep | c | Every page (next-turn readiness) | Unmeasured; several SD sector writes | none | Progress loss on crash | Yes (rule 8) |
| 4 | Line-level culling in strip passes | c | AA pages | Unmeasured; most of 14 full walks | none | Low | Yes |
| 5 | IRAM → flash (FreeRTOS, heap, ringbuf, RMT, locks) | b | SD fonts, images, builds, BLE | — | +~20 KB heap (estimate) | Small CPU cost; ISR rules | Maybe (per board) |
| 6 | No Bluetooth (or no bonded remote) | b / a | Chapter boundaries; heap gates | Removes radio stop/start stalls | +10,992 B static (measured); +≈49 KB at runtime if a remote is used | Feature loss | Personal |
| 7 | Heap poisoning Light → Basic | b | Heap gates | — | +12 B per live block, +644 B IRAM | Lose corruption detection | Personal |
| 8 | Restore X3 refresh split / declare `asyncBase` | c (SDK) | Every AA page | X4 saw 1,274 → 822 ms | +up to 2 × 52 KB plane buffers if buffered | Needs hardware validation; rollback was diagnostic | SDK owner's call |
| 9 | Larger AA strips when heap allows | c | AA pages | Fewer re-renders (14 → 4 at 264 rows) | +~18 KB transient | Heap gate needed | Maybe |
| 10 | Smaller `PNG_MAX_BUFFERED_PIXELS` | b | First view of PNG pages | Decoder may then fit | −13,854 B from a 59.5 KB block (estimate) | Narrower PNG limit | Personal |
| 11 | Reuse one group buffer in `FontDecompressor` | c | Fragmentation over a session | Keeps the largest block stable | none | Low | Yes |
| 12 | Keep built-in glyphs across pages | c | Every page (built-in font) | Saves the measured `prewarm=` time | +page slot size (KB) | More heap held | Maybe |
| 13 | Sunlight fading fix off | a | Every page, if on | Removes PON/POF waits | none | Sunlight fading | Setting exists |
| 14 | X3 display SPI 10 → 16 MHz | c (SDK) | Every page | −37.5% of SPI time (≈−94 ms AA, ≈−31 ms non-AA) | none | Signal integrity unknown | SDK owner's call |
| 15 | Smaller RTC log ring | c | Heap gates | — | +up to 4 KB (RTC heap) | Fewer crash breadcrumbs | Personal |
| 16 | Trim flash chip drivers, HAL asserts, IRAM event post | b | Heap gates | — | +~1–2 KB | Boot failure if the vendor is wrong | Personal |

### Item details

**1. Text anti-aliasing off (a).**
Mechanism: with `textAntiAliasing` on (default 1, `CrossPointSettings.h:298`),
the X3 path runs a second waveform, 4 extra plane transfers and 14 strip
renders (§2.4). Off, it is one `displayBuffer(FAST)`
(`EpubReaderActivity.cpp:2198-2199`).
Estimate: (6 − 2) planes × 41.8 ms = 167 ms SPI, plus one waveform. The gray
nudge waveform time is not measured; FAST is ~436 ms (SDK `b17beee`).
Trade-off: lower text quality. Image pages still use grayscale unless the image
mode is B/W (`:2116-2128`).

**2. Refresh frequency (a).**
Mechanism: §2.5. Every Nth page on UC8253 runs full + condition + settle
waveforms + 200 ms (`Uc8253X3Driver.cpp:209-281`). The SDK measured 2,989 ms
for a promoted full sync (`b17beee`). At 15 pages that is about 170 ms per
page averaged (estimate: (2,989 − 436) / 15). At 30 pages it is about half
that. "Never" removes it (`CrossPointSettings.cpp:381-384`). The manual
refresh button still works.
Trade-off: ghosting builds up. On UC8279 the gain is smaller (one GC
waveform).

**3. Debounce progress saves (c).**
Mechanism: §2.6. Five file-system operations per page under the render lock.
Proposal: save at most every N pages or T seconds, and always on exit,
sleep, auto-sleep and chapter change. This needs a new flush point, because
`ReaderActivity::onExit`/`prepareForSleep` do not save progress today
(`ReaderActivity.cpp:97-129`).
Trade-off: a crash or battery cut loses up to N pages. No RAM change. This
matches `.agents/rules/hardware-resources.md:42` ("debounce progress saves").

**4. Line-level culling in strip passes (c).**
Mechanism: `renderFilteredPageElements` renders every element
(`Page.cpp:11-19`). `PageLine::render` → `TextBlock::render` walks every word.
Glyphs are culled only after `getGlyphMetrics`
(`GfxRenderer.cpp:534-563`). Skipping lines whose y-range misses the active
strip (`glyphIntersectsStrip`, `GfxRenderer.cpp:1732-1745`) would cut most
of the 14 passes. Images already do this (`ImageBlock.cpp:333-341`).
Verify with the `gray_lsb=`/`gray_msb=` fields of the "Page render (tiled)"
log. Rotated orientations need care: strip rows are physical rows.

**5. IRAM → flash (b).**
Mechanism: on C3, freed IRAM becomes heap (§3.1). ESP-IDF lists these as
ways to reduce IRAM
([Minimizing RAM Usage, v5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32c3/api-guides/performance/ram-usage.html)):

- `CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH=y`: "safe to enable in all
  configurations" if functions are not misused from ISRs. Kconfig: "saves up to
  8KB" (`freertos/Kconfig:557-562`). ISR and critical-section functions stay
  in IRAM (`freertos/linker.lf:1-40`).
- `CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH=y`: safe "provided that
  CONFIG_SPI_MASTER_ISR_IN_IRAM is not enabled" (it is not, sdkconfig:1223)
  and no IRAM ISR calls heap functions (`heap/Kconfig:134-141`). Up to
  7,742 B.
- `CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH=y`: non-ISR ringbuf functions
  (`esp_ringbuf/Kconfig:4-9`). Part of 4,812 B. The ISR variant is also
  possible because `CONFIG_UART_ISR_IN_IRAM` is off (sdkconfig:1246) and
  Arduino's USB-CDC interrupt is not IRAM (flags 0,
  `framework-arduinoespressif32/cores/esp32/HWCDC.cpp:462`). It is riskier.
- `CONFIG_RMT_TX_ISR_HANDLER_IN_IRAM=n`, `CONFIG_RMT_RX_ISR_HANDLER_IN_IRAM=n`,
  `CONFIG_RMT_ENCODER_FUNC_IN_IRAM=n` (`esp_driver_rmt/Kconfig:4-22`). Up to
  2,706 B. RMT is linked only by Arduino's RGB-LED path.
- `CONFIG_LIBC_LOCKS_PLACE_IN_IRAM=n` (`newlib/Kconfig:48-53`). 796 B.

Estimate: 8 + 7.7 + ~2.5 + 2.7 + 0.8 ≈ 21 KB upper bound, minus 512 B
rounding. Trade-off: "Moving frequently-called functions from IRAM to flash
may increase their execution time" (same ESP-IDF page). Flash writes (NVS,
OTA, coredump) disable the cache, but these functions are only called from
tasks then, which block anyway.
Benefit for page turns is indirect. It helps only when it lifts free heap
over the 40 KB, 32 KB or image thresholds.

Personal-build sketch (not applied; preserve the shared flags as
`.agents/rules/environment.md:22-37` requires):

```ini
[env:x3_personal]
extends = env:gh_release
custom_sdkconfig =
  ${firmware_tuned_c3.custom_sdkconfig}
  CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH=y
  CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH=y
  CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH=y
  CONFIG_RMT_TX_ISR_HANDLER_IN_IRAM=n
  CONFIG_RMT_RX_ISR_HANDLER_IN_IRAM=n
  CONFIG_RMT_ENCODER_FUNC_IN_IRAM=n
  CONFIG_LIBC_LOCKS_PLACE_IN_IRAM=n
```

**6. No Bluetooth (b), or no bonded remote (a).**
Static −10,992 B DRAM and −383,946 B flash (measured). At
runtime the radio is not started without a bonded remote (`RadioPolicy.h:103-104`),
so a user without a remote already avoids the 49 KB. With a remote, every
chapter build stops the radio (§2.8). The radio also stops after 30 s with no
link (`RadioPolicy.h:25-27`).

**7. Heap poisoning off (b).**
`CONFIG_HEAP_POISONING_LIGHT=y` (sdkconfig:1912). Each allocation carries an
8-byte head (canary + size) and a 4-byte tail
(`heap/multi_heap_poisoning.c:52-61`). Basic mode does not compile that file
(`heap/CMakeLists.txt:32-34`), so it also frees 644 B of IRAM. The total
depends on the live block count, which is unknown (§7). For example, 1,000
live blocks would be 12 KB.
Trade-off: heap corruption is no longer caught at `free()`. This weakens
crash diagnosis.

**8. X3 overlap (c, SDK).**
The reader overlaps gray-plane rendering with the B/W waveform only when
`asyncBase` is true (`EpubReaderActivity.cpp:2141`, `:2226-2257`). The pinned
X3 drivers declare it false, and UC8253 uses blocking display (§2.5). The
rollback commit `c27da30` is labeled diagnostic, so restoring it is the SDK
owner's call. The buffered variant also needs two 52,272 B plane buffers with
60 KB headroom (`EpubReaderActivity.cpp:2220-2227`). That is not available on
the X3, so the X3 would use the strip variant.

**9. Larger strips (c).**
`STRIP_ROWS = 80` is fixed (`EpubReaderActivity.cpp:2204`). With 264 rows
(26,136 B) there would be 2 strips per plane instead of 7. Gate it on free
heap and fall back to 80. Only worth doing if item 4 does not already remove
most of the per-pass cost.

**10. PNG decoder size (b).**
`-DPNG_MAX_BUFFERED_PIXELS=16416` (`platformio.ini:67-69`) supports 2,048 px
wide images. The library default is `(320*4+1)*2 = 2,562`
(`PNGdec.h:55`). Lowering it shrinks the single `new PNG()` block
(`PngToFramebufferConverter.cpp:350-351`) by 13,854 B. Also correct
`PNG_DECODER_APPROX_SIZE` (44 KB, `:86`) to the real size so the gate is
honest. First view only; later views use the `.pxc` cache.

**11. Group buffer reuse (c).**
`prewarmCache` `malloc`s and frees a 7–27 KB buffer per group per page
(`FontDecompressor.cpp:466-490`). This goes against the "allocate once, reuse"
rule (`.agents/rules/hardware-resources.md:36`). A grow-only buffer like
`hotGroup` (`FontDecompressor.cpp:52-60`) avoids the churn. Benefit: a more
stable `MaxAlloc` over long sessions. Small speed effect.

**12. Built-in glyph retention (c).**
Built-in glyphs are decompressed on every page, and the idle prewarm result is
discarded (§2.3). SD fonts already use "keep-if-fits" retention with a heap
floor (`SdCardFont.cpp:124-130`, `:178-190`). The same pattern for
`FontDecompressor` would let the idle prewarm serve the next turn. Only do it
if the `prewarm=` field of the render log shows material time. Inflating
~30 KB from flash is probably a few ms, but that is not measured.

**13. Fading fix (a).** §2.5. Check the setting is off unless needed.

**14. SPI clock (c, SDK).** The UC8253 driver default is 16 MHz
(`Uc8253X3Driver.cpp:68-71`), but the X3 profile sets 10 MHz
(`BoardConfig.h:1015`, `:1070`). There is no evidence here of whether 16 MHz
is reliable on X3 hardware.

**15–16. Small items.** The RTC log ring is always compiled
(`Logging.cpp:9-13`). `CONFIG_ESP_EVENT_POST_FROM_IRAM_ISR=y` (sdkconfig:1281),
`CONFIG_HAL_DEFAULT_ASSERTION_LEVEL=2` (sdkconfig:1905), and six flash vendor
drivers (sdkconfig:2498-2503) are listed as IRAM savers in the ESP-IDF RAM
guide. Trimming vendors needs the X3 flash ID (`esptool.py flash_id`).

### Not worth it (measured)

- X3-only build: −17,658 B flash, −152 B RAM.
- English-only UI: −429,540 B flash, 0 RAM.
- Content-protection crypto: cannot be removed by flags
  (`lib/Epub/ContentProtection.cpp`), ~120 B RAM.
- The boot-time X3 controller probe affects wake time, not page turns.

---

## 5. Already done (do not re-propose)

- `custom_sdkconfig` heap reclamation (`platformio.ini:192-244`, `:290-305`):
  esp_timer stack 4,096, FreeRTOS timer stack 2,560, WiFi IRAM and RX IRAM
  off, Arduino selective compilation, unused components removed, BT
  controller run from flash, ROM libc (misaligned access off), IPC stack
  1,024, GCMP off.
- NimBLE sized for one remote (`platformio.ini:264-278`).
- wolfSSL single-precision ECC, which avoids MP_MEM at ~50 KB free
  (`platformio.ini:102-109`, `scripts/patch_wolfssl.py:15`).
- Expat `XML_CONTEXT_BYTES=0` and `XML_GE=0`; WebSocket client slots 2;
  `USE_SPI_ARRAY_TRANSFER=1` for ~3× SD reads (`platformio.ini:44-66`).
- Single framebuffer (`-DEINK_DISPLAY_SINGLE_BUFFER_MODE=1`), lent in place to
  builds (`GfxRenderer.cpp:219-240`).
- Inflate ring in four 8 KB segments (`InflateReader.h:53-71`).
- Render task 8 KB on C3; 16 KB and the 24 KB loop stack only on vector-font
  boards (`ActivityManager.cpp:53-62`, `src/main.cpp:56-63`).
- SD-font keep-if-fits buffers and idle prewarm (`SdCardFont.cpp:124-190`,
  `EpubReaderActivity.cpp:715-736`).
- Tiled grayscale with glyph and image strip culling (`GfxRenderer.cpp:548-563`,
  `ImageBlock.cpp:333-341`).
- Per-page `.pxc` RAM slot (`ImageBlock.cpp:88-162`).
- Chunked 8 KB B/W snapshot (`GfxRenderer.h:48`).
- Bluetooth radio released for chapter builds (`EpubReaderActivity.cpp:380-404`).
- Font caches registered as evictable sinks (`GfxRenderer.cpp:181-216`).

---

## 6. How to verify on the device

Log levels: `LOG_DBG` lines need `LOG_LEVEL=2`. `gh_release` uses 1
(`platformio.ini:329`). Use `env:default` (`:309-318`) or a local override.
Lines carry a millisecond timestamp (`lib/Logging/Logging.cpp:46`).
The SDK's `Wait complete: <tag> (N ms)` lines print whenever Serial is
connected, at any log level (`EpdBus.cpp:387-389`). Tags on UC8253:
`X3_PON`, `X3_DRF`, `X3_POF`. On UC8279: `8279_PON`, `8279_DRF`, `8279_POF`.
Settings → About shows the live controller name
(`src/activities/settings/AboutActivity.cpp:143`).

| What | Log line (source) | Method |
| --- | --- | --- |
| Page render breakdown | `ERS Page render (tiled): prewarm= bw_render= display= gray_lsb= gray_msb= gray_display= cleanup= total=` (`EpubReaderActivity.cpp:2303-2307`); non-AA `Page render: prewarm= bw_render= display= total=` (`:2345`) | 20 turns with AA on, 20 off; compare `total` and the `X3_DRF` waits |
| Whole render | `ERS Rendered page in Nms` (`:1971`) | Same |
| Scheduled refresh cost | `X3_DRF` waits on the 15th page, plus `total` | Compare refresh frequency 15 vs 30 vs Never |
| Progress save | Timestamp gap from `Rendered page` to `Progress saved:` (`EpubReaderUtils.h:40`) | Before and after item 3 |
| Free heap | `[MEM] Free: … Min Free: … MaxAlloc:` every 10 s (`src/main.cpp:904`), INF level | After the change, read 10 min in one book |
| Built-in font cost | `FDC Prewarm: N glyphs in B bytes from G groups (M missed)` (`FontDecompressor.cpp:508`) | `missed > 0` means low-heap fallback |
| SD-font retention | `ERS Idle prewarm: page N in Mms` (`EpubReaderActivity.cpp:732`); then the next turn's `prewarm=` | Near 0 ms means retained |
| Image slot | `IMG Cache render complete (payload now in RAM)` vs `IMG Cache render complete` (`ImageBlock.cpp:219`, `:282`) | The second means streaming from SD |
| PNG decode | `PNG Not enough heap for PNG decoder` / `Failed to allocate PNG decoder` (`PngToFramebufferConverter.cpp:346-353`), ERR level | Open a book with PNG images |
| Background build pauses | `ERS Heap before section build: N (max block M)` (`EpubReaderActivity.cpp:1791`) | Chapter boundaries |
| Bluetooth stall | `ERS Bluetooth radio still holds the heap after … ms` (`:401`) | With a remote |
| Loop stalls | `LOOP New max loop duration` (`src/main.cpp:1087`) | During background builds |
| Static RAM | `esp_idf_size` on the new map: DRAM used | For item 5, 7 and the `[MEM]` free value at idle |

---

## 7. Open questions (need on-device measurement)

1. Which controller does this X3 have (About screen)? The UC8253 scheduled
   refresh is far more expensive than UC8279.
2. Real waveform times on the pinned SDK: FAST, pre-BW mid, gray nudge, and
   the full-sync chain. The SDK numbers (436 ms, 2,989 ms) were measured with
   the split driver that is now rolled back.
3. Actual SPI throughput at 10 MHz, including per-row call overhead.
4. Progress-save time on the user's SD card, and its variance.
5. Per-strip render time (`gray_lsb`/`gray_msb`), to size item 4 against
   item 9.
6. Free heap and `MaxAlloc` during reading on this device, with and without a
   bonded remote. Is it above or below 40 KB (SD fonts) and 32 KB (builds)?
7. The live heap block count, to size item 7. `heap_caps_get_info()` reports
   `allocated_blocks`; no existing code logs it.
8. The ~55–60 KB of runtime heap not broken down in §3.2. A one-off
   `heap_caps_print_heap_info(MALLOC_CAP_INTERNAL)` would show it; no code
   does this today.
9. Whether 16 MHz display SPI is reliable on X3 hardware (item 14).
10. Whether the user reads with a built-in font or an SD font. This decides
    whether the 40 KB threshold matters at all.
11. Input-to-render-start latency, especially after 3 s idle at 10 MHz. No
    log line measures it today.
