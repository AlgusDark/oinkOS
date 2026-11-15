# Xteink X3 firmware ecosystem: what other projects do, and what oinkOS can take

Scope: other firmware for the Xteink X3 (ESP32-C3 rev4, no PSRAM, 792x528
panel, UC8253 or UC8279d controller) and close relatives. The question is which
ideas or code would improve oinkOS on the X3. This is a snapshot of oinkOS
`main` at `6bf013d2`, with the FreeInk SDK pinned at `fb7e926`. Line numbers
are from that tree. The CrossPoint sync that followed moved the SDK pin to
`a8acebf`; see R9. Display refresh and LUT work by InkPointX and CrossInk is already in
[x3-fast-refresh.md](x3-fast-refresh.md). RAM on the page-turn path is in
[page-navigation-ram.md](page-navigation-ram.md). This note does not repeat
them. It covers power, sleep, storage, reader, and the SDK.

Sources (each cloned shallowly and read at this commit):

- marigold-os, Rust, `Jon-Vii/marigold-os` at
  `66727610640717fe7a03671b4e1e2ac5dd9db3df` (2026-11-10).
- YACP, C++ CrossInk fork, `Sichroteph/YACP` at
  `23568ad3580740068cd3466f400b6900f08a1a15` (2026-09-25, 1.8.0 beta).
- pulp-os, Rust, `hansmrtn/pulp-os` at
  `445755fead6c0815116180e12e3dce38cca38cb7` (2026-03-05).
- CrossInk, `uxjulia/CrossInk` at `9914146eeae7b46b300f475a16c32426fc02ec1f`
  (2026-10-04). This is the same commit as the fast-refresh note. GitHub shows
  pushes after it, on other branches.
- FreeInk SDK upstream, `Free-Ink/freeink-sdk` at
  `3f3e4c1126e2cb9a278c2c1076fb4a3334ffdc30` (2026-11-13). Our pin `fb7e926`
  is an ancestor, 59 commits behind.
- PapyriX, `bigbag/papyrix-reader` at
  `7f87247dabe4d16ca5c7a0cd18dc1d5e5574452b` (2026-11-13).
- Upstream CrossPoint pull requests
  [#2525](https://github.com/crosspoint-reader/crosspoint-reader/pull/2525)
  (merge `70faa29d`),
  [#3077](https://github.com/crosspoint-reader/crosspoint-reader/pull/3077)
  (merge `81e19270`), and the open
  [#3082](https://github.com/crosspoint-reader/crosspoint-reader/pull/3082),
  read with `gh` and through our local `upstream` history.
- ESP-IDF 5.5.5 sources installed by PlatformIO (`framework-espidf`), and the
  C3 sdkconfig in `framework-arduinoespressif32-libs/esp32c3/sdkconfig`.
- Briefly examined: `rgushchin/vvbook` `e9ef791`, `ib0es/bookorbit-firmware`
  `343e2cd`, `hthoene/paperpaw` `2e9f4be`, `gdaniil/wren-firmware` `bfc7b3f`,
  `7box-studio/pagepet-reader` `2fcb988`, `anki630/crossmosa` `fe1d166`,
  `mohitagw15856/inkhome` `735e74d`, `hannah-nula/crosspoint-x3-ble-page-turner`
  `c16af17`.

Nothing here was built, flashed, or timed for this note. Numbers come from code,
commit messages, pull request text, or release notes, and each one names its
source. "Estimate" marks arithmetic from cited code. "Claimed" marks a number or
result that only a README, changelog, or PR text states.

---

## 1. TL;DR

- **The 2.3 s cleanup page has two ready-made single-pass replacements.** YACP
  replaces the periodic cleanup on X3 text pages with the "AA-pre-BW(mid)"
  differential waveform. Our SDK already has it, as `displayGrayscaleBase()`.
  marigold uses one pass of the `_half` scrub bank, with bytes identical to ours.
  Our driver already has that single-scan path too. What forces the multi-pass
  chain is one X3 rule in `lib/hal/HalDisplay.cpp:62-64`. Both options are small
  app or HAL changes with no RAM cost. Ghosting has to be checked on hardware.
- **Debounced progress writes are solved elsewhere.** YACP has a 55-line,
  allocation-free `ReaderProgressSaveDebouncer` (MIT). marigold writes at most
  once per 15 s and always writes before sleep. We still write on every page
  (`EpubReaderActivity.cpp:1979-1986`).
- **Light sleep is the biggest battery lever, and the riskiest.** Upstream
  CrossPoint measured an X3 at 9.68 mA idle before and 2.78 mA after with
  timer-paced light sleep (claimed, PPK2, PR #2525). Upstream reverted it within
  4 days (#3077). YACP tried and reverted the same idea in one day. The failure
  mechanism is in ESP-IDF: with `CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND=y`
  (set in our sdkconfig), every GPIO floats during light sleep, including GPIO13,
  which the X3 uses to keep power on.
- **Two low-risk power fixes need no light sleep.** Our pinned SDK has busy-wait
  hooks that can drop the CPU to 10 MHz during the 0.4 to 2 s BUSY wait. We
  never install them; YACP does. And on X3 every `gpio.update()` reads the
  fuel gauge over I2C to detect USB (`HalGPIO.cpp:128`). YACP and #2525 rate
  limited that read.
- **Next-page staging in controller RAM costs no host RAM on X3.** marigold
  draws the next page while the reader reads and writes it to DTM2 ahead of the
  press. Our driver already keeps DTM1 equal to the screen, so the one
  framebuffer is free. It hides draw and SPI time (about 100 ms, estimate). It
  needs a new SDK API and has not run on X3 hardware in marigold either.
- **The FreeInk SDK has nothing new for the UC8253** since our pin. Worth
  taking on a bump: the SD flush before sleep (`07567ed`), on X3 the SPI-card
  `shutdown()` is an empty function today, and HTTP hardening. One commit
  (`d210ec2`) quietly changes UC8279 X3 image LUT bytes.
- **The display SPI clock evidence conflicts.** PapyriX says 20 MHz caused pixel
  damage on a UC8253 X3. marigold picked 16 MHz for that reason. InkPointX runs
  20 MHz. Roadmap step 4 should try 16 MHz first.

---

## 2. What oinkOS already has (do not port again)

Several items that other forks advertise are already in our tree. They are
listed so nobody re-imports them.

| Feature in another fork | Where we already have it |
| --- | --- |
| Quick Resume, saved sleep frame, no splash on wake (YACP README) | `src/main.cpp:348-361`, `:747-783` (`BootResume::SplashlessWake`) |
| X3 RAM re-seed on wake without a flash (YACP `main.cpp:1029-1033`) | `src/main.cpp:774-778` (`cleanupGrayscaleWithFrameBuffer`) |
| Short wake-button check instead of a 500 ms settle (YACP `main.cpp:890-893`) | `lib/hal/HalGPIO.cpp:220-238` (10 ms) |
| No 250 ms USB CDC wait on release builds (YACP) | `src/main.cpp:568-574`, only with `CROSSPOINT_WAIT_FOR_USB_SERIAL` |
| Background chapter layout while reading (YACP, CrossInk, marigold) | `EpubReaderActivity.h:135-151` |
| Total-fit line breaking (marigold `ARCHITECTURE.md:597-617`) | `lib/Epub/Epub/ParsedText.cpp:1121` (DP over badness) |
| Glyphs drawn per glyph, not per pixel (marigold `3d723c3`) | `lib/GfxRenderer/GfxRenderer.cpp:588-619` |
| Parsed CSS cached on SD (PapyriX `styles.bin`) | `lib/Epub/Epub/css/CssParser.h:61`, `Epub.cpp:499` |
| Saved Wi-Fi tried in RSSI order with scan channel and BSSID (YACP) | `WifiSelectionActivity.cpp:229-256`, `:412-431`, `:545` |
| BQ27220 capacity set to 650 mAh (CrossInk changelog line 102) | `src/main.cpp:896`, `BoardConfig.h:1090` |

---

## 3. marigold-os

**What it is.** A bare-metal Rust reader OS for the X4 and X3 (Embassy on
esp-rtos, no Arduino, no Wi-Fi stack unless a sync session runs). MIT. 268
commits in the shallow history, releases v0.5.0 to v0.8.0 in October 2026,
34 stars. Active: last merge 2026-11-10. It ships separate X3 and X4 images
(`docs/releases/v0.8.0.md`). The code is Rust, so only ideas transfer, not
code. MIT is compatible with our MIT sources.

**Hardware maturity on X3.** Lower than on X4. The staging commit says "No X3
has run it yet" (`032ce64`). The light-sleep commit measured only an X4
(`2242e7a`). The button-latency change excludes the X3 because its ladders
"have not been traced" (`7088a9d`).

### 3.1 Next page staged in DTM2 (verified in code; not run on X3)

Mechanism (`docs/ARCHITECTURE.md:410-478`, `display/src/epd/uc8253.rs:239-357`):

1. After a page settles, the app predicts what a Next press would render
   (`ReaderState::next_page_request`).
2. The display task draws that page into the framebuffer and writes it to DTM2,
   ending with `DATA_STOP` (`STAGE_NEW_STEPS`, `uc8253.rs:351-357`). DTM1 still
   holds the page on screen.
3. On the press, if the request matches, the turn is only a LUT load and
   `DISPLAY_REFRESH` (`FAST_BOTH_STAGED_STEPS`, `uc8253.rs:241-244`).
4. A 64-bit digest of the reader store, taken at stage time and at press time,
   decides "same frame". Any change is a miss, and the normal path runs.
5. Staging yields to input: it checks for a press before each line and each
   SPI band.

marigold uses two framebuffers (`fw/src/tasks/display.rs:67-71`), but staging
does not depend on that. In our driver the host framebuffer is free after a
fast turn: `Uc8253X3Driver.cpp` rewrites DTM1 from the framebuffer after each
refresh, so controller RAM holds the screen. That makes staging a zero-host-RAM
idea on our tree.

What it would save on a warm fast turn, estimate: the reader draw (64 ms in the
ROADMAP table) plus one DTM2 plane write (52,272 x 8 / 10e6 = 41.8 ms at our
10 MHz). The waveform itself (the rest of the ~487 ms e-ink time) is unchanged.

What it would touch in our tree:

- SDK: a new `FreeInkDisplay` call pair, "stage frame" and "refresh staged",
  next to `displayBuffer()`. This is a new SDK contract. It belongs upstream.
- `src/activities/reader/EpubReaderActivity.cpp:2156-2203`: the page-turn path,
  plus a "frame key" for page, layout settings, and status bar text. The status
  bar shows battery and time, so a minute change must be a miss.
- Anything that draws over the current framebuffer (popups, menus) must first
  re-render the current page, because the framebuffer now holds the next one.

Conflicts and limits: with text anti-aliasing on, a turn is a base plus a gray
waveform; staging only hides the base part. It overlaps with the open
`c27da30` async-split question (roadmap step 5). Image pages and SD fonts would
not stage (marigold skips both too).

### 3.2 Light sleep between page turns (verified in code; measured on X4 only)

`fw/src/light_sleep.rs` runs as the esp-rtos idle hook. It sleeps until the
next timer, then moves the system timer forward by the RTC time
(`2242e7a`). Tasks with hardware work in flight hold the chip awake
(`light_sleep.rs:31-57`). Wake pins are GPIO1, 2, 3 and USB D+
(`light_sleep.rs:98`). Other ladder buttons are found by polling every 15 ms for
10 s after a press, then every 40 ms (`ARCHITECTURE.md:236-239`).

Claimed results (commit `2242e7a`, X4): asleep 97% of the time on a page; battery
voltage fell 7.5 mV/h with light sleep against 52 and 20 mV/h without it; a
side Down press reaches the input task in 3.4 ms instead of 10.5 ms. Waking
adds about 3.6 ms because the flash cache is cleared.

This is a different runtime, so the code does not transfer. Section 8 covers
light sleep for our Arduino build.

### 3.3 Progress journal (verified in docs and code layout)

"Same-book page turns write at most one alternating STATEA/STATEB generation
per 15 s." The pending position is written when the window ends, before display
sleep, before Wi-Fi, before a firmware stage, and on a book switch. A battery
pull loses at most 15 s (`ARCHITECTURE.md:261-267`). See recommendation R2.

### 3.4 Cleanup waveform on X3 (verified in code)

marigold's X3 "FastClean" is one `_half` bank refresh with CDI `0xA9`, then a
200 ms delay (`CLEAN_POWERED_STEPS`, `uc8253.rs:273-281`). The `_half` bytes
are identical to our `lut_x3_*_half` (`Uc8253X3Luts.h:39-58` against
`uc8253.rs:433-452`). Its "clean every ten" policy uses this instead of a full
sync (`ARCHITECTURE.md:357-370`). See recommendation R1, option B.

### 3.5 Other items

- Display SPI at 16 MHz on X3, "papyrix reports pixel corruption above
  ~20 MHz" (`uc8253.rs:60-64`). See R8.
- Built-in fonts stored in half the flash (`14e7be1`, claimed in
  `docs/releases/v0.8.0.md`). Their format is Rust-specific. Not studied further.
- A host emulator models the UC8253 command stream and rejects a refresh
  without a complete DTM2 write (`032ce64`). This is a good test idea for our
  SDK host tests, not firmware.

---

## 4. YACP

**What it is.** "Yet Another CrossPoint", a personal fork of CrossInk 1.4.0 for
X3 and X4 (README "Origin"). C++, same architecture and storage layout as
ours. MIT (`LICENSE`, Dave Allie copyright, as ours). 80 stars. Releases up to
`v1.8.1-yacp.9` (2026-09-30, pre-release); our clone of the default branch is
1.8.0 beta. The README states that it has no issues, discussions, or support
channel. Its stated goal is battery and incidental-work reduction on the X3.
Code can be borrowed with attribution.

### 4.1 No-flash periodic cleanup on X3 (verified in code; quality claimed)

On X3 text pages, when the periodic refresh is due, YACP calls
`displayGrayscaleBase(FAST_REFRESH)` instead of `displayBuffer(HALF_REFRESH)`:

- `src/activities/reader/ReaderUtils.h:163-190` (`shouldUseNoFlashReinforcement`
  and `displayWithRefreshCycle`).
- `src/activities/reader/EpubReaderActivity.cpp:4761-4785` for AA text pages.
  Pages with images, forced cleanups, wake, and popups still use the full path.
- Introduced in release `v1.4.0-yacp.155` (`CHANGELOG.md:661-662`, code in
  `20af8ae`).

The waveform is the OEM "AA-pre-BW(mid)" bank. Changed pixels get strong drive;
unchanged pixels get a gentle reinforcement pulse (our SDK's comment,
`Uc8253X3Driver.cpp:310-319`). Our ordinary AA turn already uses this exact
call (`EpubReaderActivity.cpp:2197-2198`). Only the "due" branch differs
(`:2189-2194`).

The ghosting result is shown only in a release video. YACP's own preset uses
it on every page, with anti-aliasing off (`CHANGELOG.md:674-675`). See R1,
option A.

### 4.2 Debounced progress writes (verified in code)

`src/activities/reader/ReaderProgressSaveDebouncer.h:9-55`: a plain class with
five scalar fields. It saves after 10 page changes or 5 minutes, whichever comes
first. `EpubReaderActivity.cpp:4530-4567` queues saves; `onExit()` flushes
(`:1888`). It never allocates. See R2.

### 4.3 CPU downclock during BUSY waits (verified in code)

`src/main.cpp:782` installs `display.setBusyWaitHooks(begin, end)`. The hooks
call `powerManager.beginDisplayBusyWait()` and `endDisplayBusyWait()`
(`main.cpp:234-235`), which lower the CPU clock while the controller runs the
waveform. Our pinned SDK has the same hook (`FreeInkDisplay.h:313-314`). Our
firmware never calls it. See R3.

### 4.4 Light sleep tried and reverted (verified in history)

`6d1f10f` added `HalPowerManager::sleepForInputPoll()`: a timer-only
`esp_light_sleep_start()` between 50 ms polls. The next commit on the same day,
`8e2f67f`, removed it: "ESP light sleep ... can drop the board's power latch and
prevent page buttons from waking the device" (`CHANGELOG.md:1301-1302`). See
section 8.

### 4.5 Rejected from YACP

Reading statistics, achievements, finished-books history, sleep galleries, and
the custom Home theme. SCOPE section 2 defers stats to forks, and themes are
frozen (SCOPE section 3).

---

## 5. Upstream CrossPoint light-sleep attempt (#2525, #3077, #3082)

Not a separate project, but the most direct X3 evidence, so it gets its own
section.

- **#2525**, merged 2026-08-13 as `70faa29d`. Three parts: light sleep in 50 ms
  timer slices after 1 s idle; downclock at 500 ms; and a busy-wait downclock
  through a new SDK hook. It also throttled the X3 I2C USB check to 1 Hz.
  Claimed X3 PPK2 results from the PR text: static page 9.68 mA before and
  2.78 mA after; a fast page turn about 100 mC before and 28.98 mC after;
  deep sleep unchanged at 12.8 µA.
- The patch held GPIO13 high with `gpio_hold_en` during each slice. Its comment
  says GPIO13 is the flash SPIWP pad wired to the battery-latch MOSFET, and that
  "the IDF flash-leakage workaround would drop it mid-slice, hard-powering the
  device off" (`git show 70faa29d -- lib/hal/HalPowerManager.cpp`).
- **#3077**, merged 2026-08-17 as `81e19270`, reverted it. Reasons in the PR
  thread: the X4 froze when USB was plugged in, and a USB-locked user could
  not use nightly builds without USB attached. The author confirmed the revert
  restores 9.79 mA idle on his X3.
- **#3082**, still open, proposes the fix: release the GPIO13 hold right after
  each slice instead of holding it across the whole idle period, and add USB
  detect as a wake source on X4. The author states he has no X4 to test it.

Our SDK still carries the slice-hook API from this work
(`freeink-sdk` `c8ed8d5`, `EpdBus.cpp:397-410`). The comment there also says
the slice hook costs about 9% more per refresh than the ISR wait path when
installed ("measured ~29 mC vs ~26.5 mC on X3", claimed).

---

## 6. CrossInk (beyond display)

**What it is.** A personal CrossPoint fork for X3, X4, X4 Pro, X4 Classic, and
Sticky (README). MIT. 1,898 stars, last push 2026-11-12. The
display work is in [x3-fast-refresh.md](x3-fast-refresh.md). YACP and wren-firmware
build on it or on CrossPoint. Its code is C++ and compatible with our tree.

Items checked against our tree:

- **EPUB render-mode fallback for low memory (verified in code).** When a
  section build runs out of memory, CrossInk retries in "Balanced" (no complex
  CSS lookups or tables), then "Light", then a "safe" profile with embedded
  styles off (`src/activities/reader/EpubReaderActivity.cpp:253-303`,
  `docs/epub-render-modes.md`). The mode is saved per book, and section caches
  get a mode suffix (`:205-211`). Our reader shows an error instead
  (`EpubReaderActivity.cpp:1740-1745`). See R6.
- **Grayscale strip buffer reused per section (claimed in changelog line 267,
  and in YACP `EpubReaderActivity.cpp:1883`).** We allocate the strip scratch on
  each AA page (`EpubReaderActivity.cpp:2271`), so a page turn allocates and
  frees `gwBytes * STRIP_ROWS` bytes (7,920 per the fast-refresh note). See R7.
- **Library metadata keyed by size and mtime.** Already recommended in
  [x3-fast-refresh.md section 6](x3-fast-refresh.md#6-cover-cache-and-invalidation).
- **Fonts (Lexend Deca and Bitter).** CrossInk picks them for even strokes that
  hold up under AA on e-ink (README "Reader Fonts", claimed). Changing built-in
  fonts changes flash size and layout caches. That is a product choice for the
  human, not a measured improvement, so it is only noted here.
- **Scalable TTF fonts** are S3-only in CrossInk (`docs/scalable-fonts.md`).
  Not for the X3.
- Reading stats, stats sync, nearby position sync, and themes: out of scope
  (SCOPE sections 2, 3, and "Active Connectivity").

---

## 7. FreeInk SDK: upstream `3f3e4c1` against our pin `fb7e926`

`fb7e926` is an ancestor of upstream `main`. There are 59 commits in
`fb7e926..3f3e4c1`, touching 73 files. Most are for the Picco, EEGO A4, X4 Pro,
Sticky, and Paper Mono. Relevant to the X3:

| Commit | What | X3 effect |
| --- | --- | --- |
| none | `Uc8253X3Driver.*`, `Uc8253X3Luts.h`, `EpdBus.*` | Unchanged. The `c27da30` rollback and the InkPointX gaps listed in the fast-refresh note are still there upstream. |
| `07567ed` | SD: `shutdown()` and USB hand-off first sync the filesystem and report failure | On the SPI card path used by X3, our `shutdown()` is an empty function (`SDCardManager.h:109`) and `HalStorage::prepareForDeepSleep()` calls it (`lib/hal/HalStorage.cpp:48-51`). Low impact when every writer closes its file. It protects against one that does not. |
| `d210ec2` | "Extend reader waveform to first refresh after book page" | Also changes `kUc8279X3_Xth4` (UC8279d X3 absolute image waveform) from 29 to 35 bytes per row, ending `0x01, 0x01`. That is a waveform change inside a "reformat" commit. Needs a UC8279d X3 image-page check. |
| `55cd23c`, `d2600e9`, `6a208c0` | HTTP: streamed uploads from bounded readers; 8 KiB and 64-field response header limits | Hardens OTA and OPDS. `55cd23c` lets an upload stream through a 256-byte buffer instead of building the body in RAM. |
| `c9bba53` | OPDS: authenticate downloads after a 401 | Fix to an existing OPDS feature, in scope. |
| `d2e6f09` | TTF: GDEF-aware GPOS kerning | TTF is S3-only in practice. No X3 effect found. |

Not found upstream: any X3 light-sleep support, new X3 drivers, or UC8253 LUTs.

Recommendation: bump the submodule only together with the `c27da30` decision
(roadmap step 5), because a bump moves both. See R9.

---

## 8. Light sleep on our X3 build: the mechanism

This section joins sections 3.2, 4.4, and 5 with ESP-IDF source.

**Today.** The main loop drops to 10 MHz after 3 s idle and polls in 10 ms
`delay()` slices (`src/main.cpp:1109-1122`). Automatic light sleep is off:
`# CONFIG_PM_ENABLE is not set` (C3 sdkconfig line 2004). The SDK already
creates `ESP_PM_NO_LIGHT_SLEEP` locks around display SPI when PM is enabled
(`EpdBus.cpp:279-292`, `FreeInkDisplay.cpp:80-95`), so it is partly ready.

**Why the pin drops (verified in ESP-IDF 5.5.5).**

- Our sdkconfig has `CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND=y` (line 1873).
- With that option, `esp_sleep_startup_init` runs at boot
  (`components/esp_hw_support/sleep_gpio.c:222-231`). It calls
  `esp_sleep_config_gpio_isolate()`, which gives every GPIO a sleep
  configuration of "disabled, floating" (`sleep_gpio.c:57-66`), and
  `esp_sleep_enable_gpio_switch(true)`, which applies it during each light sleep.
- So during light sleep GPIO13 stops driving high. On X3 GPIO13 keeps power on:
  the SDK calls it the SD-rail switch (`BoardConfig.h:1071-1078`), #2525 calls
  it the battery latch, and our HAL says both (`HalPowerManager.cpp:28`).
  Either way, a floating GPIO13 can power things off. This matches YACP's
  "drop the board's power latch".
- `gpio_hold_en()` keeps the pad, but the hold also survives resets. #3082
  argues that a hold left on across idle caused the X4 USB freeze.

**A narrower fix (my proposal; no project has tried it).** ESP-IDF provides
`gpio_sleep_sel_dis(pin)`, which excludes one pin from the sleep switch
(`components/esp_pm/Kconfig:81-90` help text). Calling it once at boot for
GPIO13 (and for any other output that must hold, such as display CS) would keep
the normal output level through light sleep without a hold that outlives a
reset. This needs reading of `gpio_sleep_sel_dis` behavior on C3 and a hardware
test before anyone relies on it.

**Recovery if it goes wrong.** A dropped latch powers the X3 off on battery. It
does not brick it: the SD firmware picker (hold side Up and press Power,
ROADMAP) still works. Test on the nightly channel only.

---

## 9. PapyriX

**What it is.** C++ firmware for X3, X4, X4 Pro, X4 Classic, Sticky, and
Murphy M4. Not a CrossPoint fork; it has its own libraries (`lib/`). MIT.
514 stars, active (2026-11-13). It is the source of the "papyrix turbo" fast
LUT that our SDK already loads (fast-refresh note, section 2).

Findings:

- **SPI clock (claimed).** "The UC8253 X3 uses 10 MHz SPI (20 MHz caused pixel
  damage in testing). The UC8279d X3 uses 20 MHz" (`docs/x3-lut-waveforms.md:133`).
  The UC8279d is rated to 20 MHz (our `BoardConfig.h:1097-1100` comment). See R8.
- **LUT timing reference (claimed, derived from LUT bytes).** One frame group is
  about 18.2 ms at PLL `0x09`. `_turbo` is 19 groups (about 382 ms), `_full` 26
  groups (about 472 ms) (`docs/x3-lut-waveforms.md:30-95`). By the same rule
  the `_half` bank is 19 + 6 = 25 groups, about 455 ms (estimate). That is
  the cost of one R1 option B scan, plus marigold's 200 ms delay.
- **LUT reload skipped when unchanged** (`lib/EInkDisplay/src/Display.cpp:1430-1446`).
  Five 42-byte tables are about 210 bytes, about 0.2 ms at 10 MHz (estimate).
  Not worth porting.
- **Input in a background task** with a 32-entry timestamped queue, so presses
  during a blocking render are kept (`docs/architecture.md:47-59`). Not
  compared in depth with our input path. Listed as an open question.
- Games and mini-apps (Sokoban, Lights Out, Go): out of scope.

---

## 10. pulp-os

**What it is.** Bare-metal Rust OS for the X4 only (SSD1677), no framebuffer:
it renders through a 4 KB strip buffer (`README.txt`). MIT. 134 stars. Last
push 2026-03-06, so inactive.

Ideas checked:

- **No framebuffer.** It saves RAM on the SSD1677, which can take strips. On
  our UC8253 path the driver re-sends DTM1 from the framebuffer after each
  refresh. Without a framebuffer, every page would be rendered twice. Rejected.
- **Session state in RTC memory** to skip SD reads on wake (`2f7b352`). It keeps
  RTC fast memory powered in deep sleep ("~1-2µA extra", claimed). Our wake
  reads settings and state from SD. The saving was not measured anywhere. Low
  priority.
- **Adaptive input polling**, 10 ms active and 50 ms idle (`6f32525`). We already
  do this (`src/main.cpp:1109-1122`).

---

## 11. Other projects examined

| Project | Commit | What | Verdict |
| --- | --- | --- | --- |
| `rgushchin/vvbook` | `e9ef791` | Rust X3 OS, Apache-2.0, two commits, includes chess | Too new to learn from. Games are out of scope. |
| `ib0es/bookorbit-firmware` | `343e2cd` | From-scratch C++, many boards, X3 "Alpha", MIT | Light sleep only on S3 (`src/services/power_service.h:51`). Nothing X3-specific to take. |
| `hthoene/paperpaw` | `2e9f4be` | Rust, plugin-based, X4 and X4 Pro, Apache-2.0 | Not X3. |
| `gdaniil/wren-firmware` | `bfc7b3f` | CrossPoint fork, UI rework started 2026-11-14 | Too new. |
| `7box-studio/pagepet-reader` | `2fcb988` | X3 firmware, early, MIT | Its README says hardware validation is pending. Nothing to take yet. |
| `anki630/crossmosa` | `fe1d166` | X3/X4 fork for Chinese: vertical text, Zhuyin, MIT | Relevant to SCOPE's language call to action, not to X3 speed or power. Worth a separate look if oinkOS adds CJK work. |
| `mohitagw15856/inkhome` | `735e74d` | X3-first CrossPoint fork with on-device apps | Not verified on hardware (its README). Apps are out of scope. |
| `hannah-nula/crosspoint-x3-ble-page-turner` | `c16af17` | X3 BLE reconnect fork, May 2026 | We already have the page turner. Not compared in detail. |
| UC8253 drivers (`Pearlthecatgirl/UC8253-Driver`, `andriydread/UC8253C_epaper.driver`, Adafruit CircuitPython) | not cloned | Other panels (WeAct 3.7", T-Deck Pro) | Different glass and LUTs. Not useful for the X3. |

Searches with no useful result: "esp32-c3 epub reader", "esp32 epub e-ink
reader" (GitHub repository search, 2026-11-15).

---

## 12. Recommended improvements, ranked for the X3

Impact is for X3 reading. Effort and risk are rough labels. RAM means DRAM
heap. Every item needs the firmware handoff and a hardware test by the human.

| # | Change | Impact | Effort | Risk | RAM / flash |
| --- | --- | --- | --- | --- | --- |
| R1 | Single-pass periodic cleanup on text pages | High: the 2.3 s page | Small | Medium (ghosting) | 0 / about 0 |
| R2 | Debounce progress writes, flush on exit and sleep | Medium | Small | Low | about 16 B per reader |
| R3 | Busy-wait CPU downclock and rate-limited X3 USB check | Low to medium (battery) | Small | Low | 0 / small |
| R4 | X3 light sleep between polls | High (battery, claimed 3.5x idle) | Medium | High | 0 / small |
| R5 | Stage the next page in DTM2 | Medium (about 100 ms per turn, estimate) | Large (SDK API) | Medium | 0 heap / small |
| R6 | Render-mode fallback on section-build OOM | Medium for heavy books | Medium | Low | 0 / some |
| R7 | Keep the AA strip scratch per section | Low (fragmentation) | Small | Low | holds about 7.9 KB while reading |
| R8 | Display SPI 16 MHz on UC8253, 20 MHz on UC8279d | Low (about 31 ms per fast turn, estimate) | Small | Medium | 0 |
| R9 | SDK bump for `07567ed` and HTTP fixes | Low (safety) | Small | Low to medium | none found |

### R1. Single-pass periodic cleanup (roadmap step 6)

- **Where now.** `src/activities/reader/EpubReaderActivity.cpp:2189-2194` (AA
  text) and `ReaderUtils.h:179-191` (B/W text) request `HALF_REFRESH`.
  `lib/hal/HalDisplay.cpp:62-64` turns any X3 `HALF_REFRESH` into
  `requestResync(1)`, so the driver runs the full chain
  (`Uc8253X3Driver.cpp:192-214` and the passes after it). That rule came from `0a57c0a5` ("Fix X3
  display ghosting on cover screen transitions"), which is about covers, not
  text.
- **Option A, from YACP.** On X3 text-only pages, call
  `renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH)` for the due page
  (YACP `ReaderUtils.h:163-190`, `EpubReaderActivity.cpp:4761-4785`). One
  differential waveform, the same bank our AA turns already use. No new SDK
  code.
- **Option B, from marigold.** Let reader text pages reach the driver's
  existing single `_half` scan (`Uc8253X3Driver.cpp:214-217`) without the forced
  resync. For example, pass a flag so `HalDisplay` keeps `requestResync` for
  covers and screen changes only. Cost about 455 ms plus a 200 ms delay
  (estimate, section 9), against 2,306 ms measured today (ROADMAP).
- **Mechanism.** Both remove the white DTM1 fill, the `_full` pass, the
  `_normal` condition pass, and the settle pass. A uses a differential bank, so
  it does not flash. B drives every pixel to its target, so it flashes once.
- **Cost.** No RAM. Flash: a few instructions.
- **Risk.** Ghosting builds up over many pages; no repo measured it with a
  camera (fast-refresh note section 8). Keep the full chain for images, wake,
  manual refresh, and UC8279d units until each is checked.
- **Verify.** Settings > System > Show Timings: read the cleanup page time on a
  UC8253 unit. Then read 100 pages at refresh frequency 15 and photograph the
  panel at pages 15, 45, and 100 with each option.

### R2. Debounced progress writes (ROADMAP "Smaller source changes")

- **Source.** YACP `ReaderProgressSaveDebouncer.h:9-55` (MIT, copy with
  attribution) and its use in `EpubReaderActivity.cpp:4530-4567`. Policy from
  marigold `ARCHITECTURE.md:261-267`: save at most once per 15 s, and always on
  chapter change, book exit, sleep, and Wi-Fi start.
- **Where in our tree.** `EpubReaderActivity.cpp:1979-1986` saves on every page.
  Txt and Xtc readers would follow the same pattern.
- **Mechanism.** Removes the SD create, write, flush, and rename from most page
  turns (page-navigation-ram, TL;DR item 3). It also reduces SD wear.
- **Cost.** A few scalars per reader activity, no heap.
- **Risk.** A crash or battery pull loses up to the window. Check that every
  sleep path goes through the reader's `onExit()` or an explicit flush.
- **Verify.** Turn 20 pages, pull power within 10 s, then reopen. The book
  should open at most 15 s of reading behind. With Show Timings on, the Load
  figure should drop on pages that do not save.

### R3. Busy-wait downclock and X3 USB-check rate limit

- **Source.** YACP `main.cpp:234-235` and `:782`; #2525's 1 Hz throttle.
- **Where in our tree.** Hook: `FreeInkDisplay.h:313-314`, installed from
  `setupDisplayAndFonts()` in `src/main.cpp`. The hook must take the same
  frequency path as `HalPowerManager::setPowerSaving()`
  (`HalPowerManager.h:28-39`), respecting the existing `Lock` (`:62`). USB check:
  `lib/hal/HalGPIO.cpp:128` calls `isUsbConnected()` on every `update()`. On X3
  this reads the BQ27220 over I2C (`HalGPIO.cpp:244-257`,
  `BatteryMonitor.cpp:945-958`; no charger IC on X3, `BoardConfig.h:1090`).
  Cache the result for about 1 s on X3.
- **Mechanism.** The CPU runs at full speed for 0.4 to 2 s of each refresh only
  to poll BUSY. The I2C read runs on every loop pass.
- **Cost.** No RAM. A few hundred bytes of flash (estimate).
- **Risk.** Low. The SDK fires the hooks only after a wait has lasted 20 ms
  (`EpdBus.h:216`). A late USB detect (up to 1 s) only delays the USB screen.
- **Verify.** No PPK2 here, so use battery drop per hour across a fixed page
  turn script, before and after. Confirm USB plug-in still shows the USB state.

### R4. X3 light sleep between polls

- **Source.** #2525 (`70faa29d`), #3082, marigold `light_sleep.rs`. Failure
  history in section 5 and YACP `8e2f67f`.
- **Where in our tree.** `src/main.cpp:1109-1122` (idle loop),
  `lib/hal/HalPowerManager.cpp` (new sleep helper next to the existing
  `esp_light_sleep_start()` use at `:188-191` for the Picco). X3 only, behind
  `gpio.deviceIsX3()`. This build also runs on X4, so the X4 path must stay as
  it is.
- **Mechanism.** Between 50 ms polls the C3 sleeps instead of running at
  10 MHz. Timer wake keeps ADC ladder polling as today; GPIO wake on the power
  button. Section 8 gives the GPIO13 mechanism and a narrower fix.
- **Cost.** No RAM. About 1 to 2 KB of flash (estimate from #2525's diff size).
- **Risk.** High. Two projects reverted it. A wrong pin state powers the device
  off. Never sleep with USB attached, Wi-Fi on, a render lock held, or the
  Bluetooth page turner running.
- **Verify.** Nightly channel only. Leave a page open for 1 hour on battery and
  then press each button; each must wake the reader. Plug and unplug USB while
  idle. Compare battery drop per hour with R3 alone.

### R5. Stage the next page in DTM2

- **Source.** marigold `032ce64`, `uc8253.rs:239-357`, `ARCHITECTURE.md:410-478`.
- **Where in our tree.** SDK `FreeInkDisplay` and `Uc8253X3Driver` (new stage
  and refresh-staged calls; upstream first). Reader:
  `EpubReaderActivity.cpp:2156-2203` and the idle part of its loop.
- **Mechanism.** Section 3.1. The press then waits only for the waveform.
- **Cost.** No extra heap. It uses the one framebuffer and the controller's
  own RAM. Flash: a few KB (estimate).
- **Risk.** Medium. Wrong frame keys show a stale page. Overlays must redraw
  the current page first. SD traffic happens between the DTM2 write and the
  refresh, on the shared SPI bus.
- **Verify.** Show Timings: Draw should read near 0 on staged turns. Alternate
  Next and Previous presses and open the menu right after a turn, to catch
  stale frames.
- **Order.** After R1 and after the `c27da30` decision, because both change the
  same driver flow.

### R6. Render-mode fallback on section-build OOM

- **Source.** CrossInk `EpubReaderActivity.cpp:253-303`, `docs/epub-render-modes.md`.
- **Where in our tree.** `EpubReaderActivity.cpp:1740-1745` (build failure shows
  `showBuildError()`), and the section cache key in `lib/Epub/Epub/Section.*`.
- **Mechanism.** Retry the build with complex CSS and tables off, then with
  embedded styles off, instead of failing.
- **Cost.** No new heap. Flash for the simplified parser paths. A per-book
  saved mode is new persistent state, and the section cache needs a mode in
  its key, which means a cache version bump
  (`.agents/rules/generated-files-cache.md`).
- **Scope.** In scope (EPUB rendering). A new per-book setting needs the human's
  agreement under `SCOPE.md`.
- **Verify.** Open a known heavy EPUB that fails today. It should open in a
  simpler layout, and the reader should say which mode it used.

### R7. Keep the AA strip scratch for the section

- **Source.** CrossInk changelog line 267; YACP `releaseGrayscaleStripScratch()`
  in `onExit()` (`EpubReaderActivity.cpp:1883`).
- **Where in our tree.** `EpubReaderActivity.cpp:2271`.
- **Trade-off.** It removes one allocation and free per AA page (less
  fragmentation). But it holds about 7.9 KB, which lowers free heap against the
  32 KB background-build gate (`EpubReaderActivity.h:138`). Release it before
  any chapter build, as YACP does. Use `heap-discipline` before deciding.
- **Verify.** Log free heap and largest free block after 200 AA pages, before and
  after.

### R8. Display SPI clock (refines roadmap step 4)

- **Evidence.** PapyriX: 20 MHz damaged pixels on a UC8253 X3
  (`docs/x3-lut-waveforms.md:133`). marigold: 16 MHz for that reason
  (`uc8253.rs:60-64`). InkPointX: 20 MHz (fast-refresh note). The UC8279d is
  rated to 20 MHz (`BoardConfig.h:1097-1100`).
- **Where.** `XTEINK_DISPLAY_SPI_HZ` (`BoardConfig.h:1021`), used for both X3
  controllers (`:1076`, `:1115`). An SDK change, per controller.
- **Saving, estimate.** At 16 MHz one plane is 26.1 ms instead of 41.8 ms, so
  about 31 ms per fast turn (two planes).
- **Verify.** Fill the screen with a fine checkerboard and a page of small text
  on a UC8253 unit; look for wrong pixels. Repeat during SD reads.

### R9. SDK bump

- Take `07567ed` (SD flush before sleep and USB hand-off) and the HTTP commits.
  The SDK pin `a8acebf` from the CrossPoint sync already has the HTTP commits
  and `d210ec2`, but not `07567ed`.
- Check `d210ec2` on a UC8279d X3 image page.
- Do it together with roadmap step 5, since the bump moves the whole SDK.
- **Verify.** Build all firmware environments; on device, sleep right after
  saving a setting and check that it survives.

---

## 13. Rejected or out of scope

- Reading statistics, achievements, finished-books lists, stats or position sync
  between devices (YACP, CrossInk): SCOPE section 2 defers these to forks.
- New Home themes and the YACP Home: themes are frozen (SCOPE section 3).
- Games and apps: PapyriX Sokoban, Lights Out, Go; vvbook chess; inkhome apps
  (SCOPE "Interactive Apps"). Gamebooks are a separate case: oinkOS wants
  EPUB gamebooks such as Project Aon's Lone Wolf, with an action chart,
  inventory, and saved games. That is not settled yet: it conflicts with
  `SCOPE.md:56` as written, and it needs its own research note.
- A second 52 KB framebuffer, as marigold uses on X3 (`display.rs:67-71`). It
  does not fit the reading heap (page-navigation-ram section 3.2). Staging (R5)
  does not need it.
- Rendering without a framebuffer (pulp-os): breaks the UC8253 DTM1 sync.
- marigold's shortened X4 fast LUT and temperature read: SSD1677 only.
- marigold's 2 ms press confirmation: measured on X4 ladders only. Our ADC
  debounce is already 5 ms (`InputManager.h:527`).
- LUT reload skipping (PapyriX): about 0.2 ms saved (estimate).
- TTF runtime fonts (CrossInk, SDK `d2e6f09`): S3 only in practice.
- New network connectors (Book Orbit sync, LocalSend, printers in PapyriX):
  closed area (SCOPE section 3).
- Automatic ESP-IDF light sleep (`CONFIG_PM_ENABLE` with tickless idle) as a
  first step: it changes timing for every task, Wi-Fi, and Bluetooth at once.
  The manual slice approach in R4 is easier to keep X3-only.

---

## 14. Open questions

- Does `gpio_sleep_sel_dis(GPIO_NUM_13)` keep the X3 latch through a light-sleep
  slice without the reset-surviving behavior of `gpio_hold_en`? No project has
  tried it.
- What does GPIO13 actually switch on the X3: the SD rail (SDK comment, factory
  firmware reverse engineering) or the battery latch (#2525)? It may be both.
- Cleanup ghosting after R1, per controller. No repo has camera data.
- Does our input path keep presses that land during a blocking refresh, as
  PapyriX's queued background sampler does? Not compared in this note.
- Did YACP's later `v1.8.1-yacp.9` change any of the cited code? Our clone is
  the default branch at 1.8.0 beta.
- CrossInk's newest work is on `development` and `release/v2.0.0`
  (2026-11-11, tag `v2.0.0-rc2`). This note read only `main` at `9914146`.
  Refresh local reference clones of these projects before relying on this.
