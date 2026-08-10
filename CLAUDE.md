# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this is

Firmware for a live ADS-B radar scope: an **ESP32-C3 Super Mini** driving a
**1.28" round GC9A01 display (240×240, SPI)**. It joins Wi-Fi through a captive
setup portal, polls [adsb.fi](https://opendata.adsb.fi) for nearby traffic, and
renders a PPI-style radar picture with an animated sweep, airport runways and
per-aircraft data tags.

Single board, single target. There is no HAL and no second display — resist
adding board `#ifdef`s unless a second device is actually being supported.

## Build

```bash
pio run -e supermini          # build
pio run -t upload -e supermini
pio run -t monitor -e supermini
pio run -t merge -e supermini # single flashable image (scripts/merge_firmware.py)
```

**Sandboxed sessions usually cannot build.** PlatformIO's registry
(`api.registry.nm1.platformio.org`, `dl.registry.platformio.org`) and the
Espressif/Arduino download hosts are commonly blocked by the agent proxy, so the
ESP32 toolchain cannot be installed. GitHub hosts *are* reachable. When you
cannot compile:

1. Run the host tests below — they cover the logic that does not touch LovyanGFX.
2. Re-read the diff for API misuse rather than assuming it compiles.
3. Say plainly in the PR that the firmware was not compiled. Do not imply it was.

Note that **GitHub Actions may be disabled on this fork**, in which case
`.github/workflows/build.yml` will not run on a PR and there is no CI safety net
at all. Check with `list_workflows` before relying on it.

## Host tests

`test/` holds off-device tests for the header-only logic — dead reckoning, the
sweep/animation policy, tag-rect geometry, and adsbdb route parsing. They are
**not** part of the firmware build (PlatformIO ignores `test/` for `pio run`).

```bash
git clone --depth 1 --branch v7.4.2 https://github.com/bblanchon/ArduinoJson.git /tmp/aj
g++ -std=gnu++11 -Wall -Wextra -I include -I /tmp/aj/src -o /tmp/t test/test_route_parse.cpp && /tmp/t
```

**Build these as `gnu++11`, not `gnu++17`** — see the standard-version trap below.
Testing at gnu++17 once hid a constexpr error that only the device build caught.

Logic worth testing belongs in a header that depends only on the standard
library (or ArduinoJson), so it can be reached from here — that is why
`route_parse.h` is split out of `route_fetcher.cpp`.

## Layout

```
include/  headers, mirroring src/ (config.h at the root)
src/
  main.cpp            Arduino setup()/loop(): Wi-Fi state machine, poll cadence, animation tick
  hardware/           LovyanGFX panel config, display init, embedded VLW font
  services/           adsb_client, route_fetcher, radar_location, wifi_setup, aircraft_motion
  ui/                 radar_display (renderer), runway_overlay, radar_range, radar_projection,
                      radar_theme (all layout/colour constants), status_screens
  data/               generated airport dataset — do not hand-edit
scripts/build_airports.py   regenerates the dataset from OurAirports
test/                 host-only tests
```

## Things that will bite you

**Everything renders into one off-screen sprite.** `radar_display.cpp` composites
the grid, sweep, runways and traffic into a 240×240×16bpp `LGFX_Sprite`
(~115 KB) and blits it in a single `pushSprite`. That is what keeps labels from
flickering. If the allocation fails the code falls back to drawing straight to
the panel and `radarDisplayCanAnimate()` returns false so the caller stops
animating. Anything that eats a large contiguous heap block can break this.

**Drawing runs inside the HTTP read loop.** `services::adsb::setPollFn()` is
called from `performGetWithPoll`/`readResponseBodyWithPoll`, and this project's
hook redraws the frame so the sweep keeps turning during the fetch. Two
consequences:

- The Arduino loop task stack is 8 KB and the TLS session is already on it, so
  **do not add large stack arrays to the draw path** — `drawAircraft()`'s
  per-frame arrays are deliberately `static`.
- New ADS-B fixes are parsed into scratch (`s_incoming`) and committed to
  `s_tracks` only after all polling is done, so a redraw never sees a
  half-updated aircraft list. Keep that ordering.

**One TLS session at a time.** Two concurrent mbedTLS contexts plus the frame
sprite will not fit in RAM, so both HTTP clients go through the mutex in
`services/net_session.h`. The route task (background) waits with
`acquireSession()`; the ADS-B poll uses `trySession()` and gives up, because it
runs on the task that also redraws — blocking there would freeze the sweep.
A skipped poll is retried on the next loop iteration rather than waiting out the
full 3 s interval. Any new network client must take the same lock.

**Longitude is not 111 km/deg.** Use `ui::radar::` projection helpers
(`offsetKmFromCenter`, `latLonToScreen`, `distSqKmFromCenter`) rather than
rolling a local conversion; they apply cos(centre latitude). Skipping it stretches
the picture east-west by 1/cos(lat) and skews runway headings.

**The airport dataset is generated.** `src/data/airports_data.cpp` and
`include/data/airports.h` come from `scripts/build_airports.py` (OurAirports:
large airports, plus medium airports with a runway ≥ 1200 m). Change the script
and regenerate; never edit the output. The runway table is grouped by
`airport_idx` and sorted longest-first, and `runway_overlay.cpp` relies on both
to skip whole airports with one range test.

**The project really compiles as gnu++11, whatever platformio.ini says.**
`build_flags` asks for `-std=gnu++17`, but the Arduino ESP32 framework appends
its own `-std=gnu++11` later on the command line and the last `-std` wins. The
flag has been inert since before this fork. Consequences:

- A `constexpr` function body must be **exactly one return statement**. Split
  helpers out rather than naming locals (see `smoothstep`/`smoothstepUnit`).
- C++14/17 library and language features are unavailable. Some C++17 syntax
  compiles anyway as a GNU extension — nested namespace definitions
  (`namespace services::adsb {`) are used throughout and only survive because
  GCC downgrades that to a pedantic warning. Do not read their presence as
  evidence that C++17 is on.
- Making gnu++17 real needs `build_unflags = -std=gnu++11` in platformio.ini.
  That re-standardises LovyanGFX and every other library too, so treat it as its
  own change with a CI run to back it, not a drive-by.

**Layout and colour constants live in `ui/radar_theme.h`**, not inline at the
call site.

**Persisted settings** use the `planeradar` Preferences namespace
(`radar_range.cpp`). A new user-facing toggle needs: the pref, a getter, a
`save*FromPortal()`, a `WiFiManagerParameter` in `wifi_setup.cpp` (declaration,
`refreshPortalParamDefaults`, `onPortalParamsSaved`, `attachPortalParams`), and a
`unitsReset()` entry.

## Style

Follows Google C++ style, as the existing code does:

- 80 columns, 2-space indent, `.cpp`/`.h`
- `kConstantName`, `s_file_static`, `g_global`, `functionName()`, `TypeName`
- File-local helpers go in an anonymous namespace
- Namespaces match directories: `services::`, `ui::`, `ui::radar`, `data::airports`
- Comments explain *why*, not *what*. The existing code is unusually well
  commented for embedded work — match that.

## Upstream

Forked from [MatixYo/ESP32-Plane-Radar](https://github.com/MatixYo/ESP32-Plane-Radar).
When porting an upstream PR, port the *feature*, not the whole branch: upstream
PRs often carry board retargeting (different pins, different `platformio.ini`)
that would break this build. Say in the commit message what you deliberately left
behind.
