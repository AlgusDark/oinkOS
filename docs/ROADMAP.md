# oinkOS roadmap

oinkOS is e-reader firmware for the Xteink X3 (ESP32-C3 rev4, UC8253 display
controller).

The device is USB-locked, so there is no USB flashing or serial log. Settings >
Check for Updates installs the channel chosen in Settings > System > Update
Channel. A local build still goes on through File Transfer. Run
`pio run -e oinkos`, upload `.pio/build/oinkos/firmware.bin` to `/firmware/`,
then install it from Settings > SD Card Firmware Update. Keep an official
CrossPoint image in `/firmware/` too. Going back to stock uses
Settings > SD Card Firmware Update. If oinkOS does not start, hold the side Up
button while pressing Power to open the SD firmware picker.

Development tools come from `flake.nix`. With Nix and direnv installed, run
`direnv allow` once. Entering the folder then provides `clang-format`, Python
and uv, and creates `.venv` with the pinned PlatformIO core on first use.

Status words: **done** (tested on the device), **built** (committed, waiting
for a device test), **open** (not started).

## Measured on the device

From the on-screen timings (Settings > System > Show Timings). Each line
shows the previous screen change. Text anti-aliasing was off for the reader
numbers, so the e-ink column is the `bw` field; with anti-aliasing on, add
the `gray` field.

| Screen | Load | Draw | E-ink refresh |
| --- | --- | --- | --- |
| Tabs + Carousel, cover band from the SD cache | 22 ms | 36 ms | 483 ms |
| Tabs + Recents, cover snapshot | 2 ms | 19 ms | 483 ms |
| Reader page turn | 15 ms | 64 ms | 487 ms |
| Reader, every 15th page (cleanup refresh) | 14 ms | 70 ms | 2,306 ms |

The e-ink refresh is about 86% of a page turn. Firmware work alone cannot make
page turns much faster. The cleanup refresh is the one big cost left.

## Fast page refresh

Research: [x3-fast-refresh.md](research/x3-fast-refresh.md) and
[page-navigation-ram.md](research/page-navigation-ram.md).

| Step | Work | Status |
| --- | --- | --- |
| 0 | Identify the display controller | done (UC8253) |
| 1 | Clear carousel covers when a book changes | built |
| 2 | Measure page turns | done (table above) |
| 3 | Send the InkPointX driver state fixes upstream to FreeInk | open |
| 4 | Run the display SPI bus at 20 MHz | open |
| 5 | Decide on FreeInk SDK commit c27da30 (UC8253 rollback) | open |
| 6 | Single-scan UC8253 cleanup refresh (targets the 2.3 s page) | open |

Order after the device tests: step 6, then steps 3 and 5, then the Home theme
polish, then the list experiment.

Settings that already help, no code needed: Text Anti-Aliasing off, Refresh
Frequency at 30 or Never, and Short Power Button Click set to Refresh Screen
for a cleanup refresh on demand.

Smaller source changes from the research:

- Cache the display probe result, so waking skips at least 110 ms of fixed
  delays. Open.
- Save reading progress less often than every page turn. Exit and sleep must
  save first. Open.

Not worth doing on the X3: keeping the next page as a bitmap in RAM (about
52 KB, more than the free heap while reading), caching page bitmaps on the SD
card, dirty-rectangle refresh for page turns, and an X3-only build without the
X4 drivers (saves about 17 KB of flash and no speed).

## Home themes

- Tabs + Recents and Tabs + Carousel homes: done (tested on the device).
- Refresh covers (hold Confirm on Home): built.
- Open:
  - The theme list on boards without PSRAM. Every theme is listed, and Cover
    Grid falls back to Lyra on the X3.
  - Remove the white space above the tab bar in Recents.
  - Center the carousel's progress percentage.
  - Document both themes and the Refresh covers gesture in `USER_GUIDE.md`.

## About screen

- SD used space is counted only when the user selects SD Used or SD Free:
  built.

## E-ink list experiment

Branch `feat/eink-list-experiment`. Its FreeInk SDK changes are a patch in
`patches/` on that branch.

- Done in the simulator: scroll arrows, fixed two-line Library rows (title and
  author), whole rows only, and a "Hold Open for Actions" hint.
- Open:
  - Device test and the firmware review.
  - The SDK text-style fix (small font id 0 was treated as unset). Worth
    sending to FreeInk on its own.
  - Not decided: series in the Library index, and a type and size line in the
    file browser.

## Build size

- English-only UI, about 420 KB less flash and no RAM change: built.
- No Bluetooth page turner frees about 11 KB of heap and 375 KB of flash. Not
  applied; only worth it if free heap becomes a limit.

## Updates over Wi-Fi

Settings > System > Update Channel has three choices. Check for Updates reads
the file for that choice and installs the build only when its version is
newer. Switching channels does not install an older build. To go backward,
use the SD card firmware picker described at the top.

Every push to `main`, and every pull request whose head repository is this
one, publishes a GitHub pre-release. The tag is `0.1.N`. `N` is
`github.run_number` for `.github/workflows/ci.yml`. Push runs and pull
request runs share that counter, so a higher number is a newer build. The
asset name is `oinkos-0.1.N-x3-x4.bin`. The release is a pre-release, and
GitHub's latest flag is left unset.

A pull request from a fork still uploads `oinkos.bin` as a build artifact.
It does not publish a release.

One rolling pre-release, tagged `channels`, holds `stable.json`, `beta.json`,
and `nightly.json`. Each file is the release JSON the device parser reads,
with `tag_name` before `assets`. The device downloads
`https://github.com/AlgusDark/oinkOS/releases/download/channels/` and then
`stable.json`, `beta.json`, or `nightly.json`.

Nightly tracks every push to `main`. Beta tracks the latest build of a pull
request from this repository. Stable has no file until the first promote.

Promote a published version with this command.

```
gh workflow run promote.yml -f version=0.1.N
```

The workflow writes `stable.json` when that version is newer than the one
already recorded, then marks the numbered release as a full release and as
GitHub's latest. Promoting a version that is not newer fails and changes
nothing. Only this workflow sets latest. Devices still on `0.1.2` read
GitHub's latest release, so promoting a newer build also moves them onto the
channel firmware.

A channel file is left as it is when the recorded `tag_name` is the same
version or a newer one. That keeps an out-of-order run from moving a channel
backward.

A local build reports `0.1.0-dev-<branch>-<shortsha>`. The patch number is
0, so any published `0.1.N` is newer.

Check for Updates no longer offers CrossPoint releases. Going back to
CrossPoint uses the SD firmware picker described at the top.

`N` comes from the run number of `ci.yml`. Renaming that workflow starts
the count again. If that happens, bump `version` under `[oinkos]` so the
next release stays newer than releases already published.

Old pre-releases are not pruned yet.

Open:

- The downloader does not verify HTTPS certificates. `HttpDownloader` calls `setInsecure()`. This is existing CrossPoint behavior and is worth fixing separately.

## Testing in the simulator

`scripts/make_sim_sd.py SD_DIR [THEME]` fills a folder with five books with
covers, a recents list, and optionally a Home theme. Start the simulator with
`CROSSPOINT_SIM_SD=SD_DIR`; the simulator library reads that variable.

## Origin

oinkOS started from CrossPoint Reader. The `upstream` remote points at it.
The shared history is kept on purpose: it credits CrossPoint's authors and lets
each sync be a normal merge.

To sync with CrossPoint:

1. Run `git fetch upstream` and merge `upstream/develop` into a branch made from `main`.
2. Resolve conflicts. A clean merge can still break the build when CrossPoint
   renames something oinkOS uses, so build `oinkos` and run the unit tests.
3. Run `nix develop --command ./bin/clang-format-fix`, then review the result like
   any other firmware change.
4. Test on the X3, then merge into `main`. CI publishes it to Nightly.

## Branches

- `main`: oinkOS.
- `feat/eink-list-experiment`: the list experiment.
