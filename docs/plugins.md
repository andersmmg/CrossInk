---
title: Lua Plugin Support
---

# Lua Plugin Support

CrossInk ships a lightweight Lua plugin system so users can drop small
interactive apps onto the SD card under `/plugins/<PluginName>/main.lua` and
launch them from the home screen.

This document covers two things:

1. The plugin **author API** — what `/plugins/*/main.lua` files can do, and how
   they are loaded.
2. The **port plan** that brought this stack over from the upstream
   `crosspoint-reader-lua` project, with per-phase "verify first" notes for
   reviewers cross-checking each step against the CrossInk codebase.

If you only want to write a plugin, the first section is what you need.
If you are doing a port review or porting the same stack to another Crosspoint
fork, read both.

---

## Part 1 — Plugin Author Guide

### Directory layout

```
/plugins/<PluginName>/      ← one folder per plugin
    main.lua                ← required entry point
```

`<PluginName>` is the directory name shown in the Plugins menu. Plugin
folders without a `main.lua` are silently ignored.

### Description metadata

The first 256 bytes of `main.lua` are scanned for a magic comment:

```lua
-- DESCRIPTION: A short one-line summary shown under the name.
```

If the comment is missing, the row says "Lua Script Extension". Keep your
description short — it shares the row with the title.

### Lifecycle

Every plugin must declare two global functions:

| Function | Called                                  | Returns       |
|----------|-----------------------------------------|---------------|
| `init()` | once, right before the first `draw()`   | anything      |
| `draw()` | every loop tick (input + render)        | anything      |

The `draw()` function runs in the **same task** that reads button state, so
calling `gui.refresh(...)` from `draw()` is the correct way to commit a frame.

A typical skeleton:

```lua
local state = 0

function init()
  math.randomseed(os.time() % 2^31)
end

function draw()
  if input.wasPressed("confirm") then
    state = (state + 1) % 2
  end
  if input.wasPressed("back") then
    sys.exit()
  end

  gui.clear()
  if state == 0 then
    gui.drawText(FONT_UI_10, 20, 20, "Hello world", COLOR_BLACK)
  else
    gui.drawCenteredText(FONT_UI_10, 80, "Goodbye")
  end
  gui.refresh(REFRESH_FAST)
end
```

### Lua globals

| Global | Description |
|--------|-------------|
| `gui`   | Drawing primitives (see below) |
| `input` | Button polling |
| `fs`    | *(planned — not yet bound)* |
| `sys`   | Time, delay, exit |
| `net`   | WiFi + HTTP GET |
| `FONT_*`| Integer font ids (see Constants) |
| `REFRESH_*` | Integer refresh-mode ids |
| `COLOR_*`, `STYLE_*` | Drawing style ids |

### `gui` — drawing primitives

Round 2 binds the following names. Names not listed aren't registered yet;
calling them returns `attempt to call a nil value`. Bindings planned for
later rounds (rounded rects, circles, polygons, BMP blit, button hints) are
listed in `docs/plugins.md` Phase 12 of the port plan.

| Lua call | Notes |
|----------|-------|
| `gui.clear()` | Clear the framebuffer. |
| `gui.refresh(mode)` | `mode` is one of `REFRESH_FULL`, `REFRESH_HALF`, `REFRESH_FAST`. |
| `gui.width()` / `gui.height()` | Current oriented screen size in pixels. |
| `gui.drawText(fontId, x, y, text, [color], [style])` | Single-line text. |
| `gui.drawCenteredText(fontId, y, text, [color], [style])` | Centered across the screen. |
| `gui.getTextWidth(fontId, text, [style])` | Pixel width. |
| `gui.drawRect(x, y, w, h, [color])` | 1-pixel border. |
| `gui.fillRect(x, y, w, h, [color])` | Solid rectangle. |

`color` is `COLOR_BLACK` (default for dark mode) or `COLOR_WHITE`. `style` is one
of `STYLE_REGULAR`, `STYLE_BOLD`, `STYLE_ITALIC`, `STYLE_BOLD_ITALIC`.

Names NOT registered in Round 2 (calling them returns `attempt to call a nil value`):
`drawRoundedRect`, `fillRoundedRect`, `drawLine`, `drawPixel`, `drawCircle`,
`fillCircle`, `fillPolygon`, `drawBmp`, `drawButtonHints`, `gui.setOrientation`.

### `input` — button polling

| Lua call | Returns |
|----------|---------|
| `input.wasPressed("name")` | true once on the press transition frame |
| `input.wasReleased("name")` | true once on the release transition frame |
| `input.isPressed("name")` | true while held |
| `input.isAnyPressed()` | true if any logical button is held |

Button names: `"confirm"`, `"back"`, `"up"`, `"down"`, `"left"`, `"right"`,
`"page_back"`, `"page_forward"`.

### `fs` — SD card *(planned, not yet bound in Round 2)*

Plugin authors: do NOT call `fs.*` functions in Round 2 plugins. Doing so
returns `attempt to call a nil value`. Bindings land in a future round; the
intended surface is:

| Lua call | Notes |
|----------|-------|
| `fs.exists(path)` | |
| `fs.listFiles(path, [maxFiles])` | Returns array of names. Directories end with `/`. |
| `fs.listDirs(path, [maxFiles])` | Returns directory names only. |
| `fs.readFile(path)` | Returns string. Empty string on failure. |
| `fs.writeFile(path, content)` | Overwrites. Returns boolean. |

### `sys` — system

| Lua call | Notes |
|----------|-------|
| `sys.millis()` | Up-time ms |
| `sys.delay(ms)` | Blocks the loop for `ms` ms |
| `sys.exit()` | Request graceful exit back to the Plugins menu |

### `net` — WiFi + HTTP

| Lua call | Notes |
|----------|-------|
| `net.wifiConnect(ssid, password)` | |
| `net.wifiStatus()` | |
| `net.wifiDisconnect()` | |
| `net.urlencode(str)` | |
| `net.get(url)` | Synchronous HTTP GET. Returns the body as a string. |

### Constants exposed to Lua

| Group   | Names |
|---------|-------|
| Fonts   | `FONT_SMALL`, `FONT_UI_10`, `FONT_UI_12`, `FONT_MEDIUM`, `FONT_12`, `FONT_14`, `FONT_16`, `FONT_18`, `FONT_20`, plus all built-in (Lexend Deca / Bitter / ChareInk) variant ids |
| Refresh | `REFRESH_FULL`, `REFRESH_HALF`, `REFRESH_FAST` |
| Style   | `STYLE_REGULAR`, `STYLE_BOLD`, `STYLE_ITALIC`, `STYLE_BOLD_ITALIC` |
| Color   | `COLOR_BLACK`, `COLOR_WHITE` |

The exact numeric values are exposed to Lua as `integer` constants; do **not**
hard-code them in your plugin — always reference the names.

### Targets

Round 2 of the Lua bridge targets the host simulator (`pio run -e simulator`)
for fast iteration. On firmware builds (`pio run -e default`), the `net`
module's `wifiConnect`/`wifiStatus`/`wifiDisconnect` calls fail with a clear
`net.wifi* not available on firmware yet (SIMULATOR-only)` Lua error rather
than an opaque `attempt to call a nil value`. The `net.get(url)` HTTP GET path
is wired through `HttpDownloader::fetchUrl` on firmware and through `libcurl`
on the simulator, so plugins that only fetch URLs work on both targets. Real
firmware WiFi support (ESP-IDF-backed bindings) is a future round.

### Resource limits

* ESP32-C3 has ~380 KB usable DRAM and no PSRAM. Even a single small script
  can drop free heap by 30–40 KB. Avoid large `string` buffers in long-lived
  tables.
* `fs.readFile()` reads the whole file into RAM. Prefer streaming reads for
  anything over a few KB.
* Plugins share the main loop. Avoid `sys.delay(> 1000)`.

### Built-in example: StopWatch

`/plugins/StopWatch/main.lua` is shipped as a worked example. It demonstrates
two-tier input handling (press vs. continuous), a refresh-mode toggle, and a
clean exit path through `sys.exit()`.

---

## Part 2 — Port Plan (Maintainer Reference)

This section is the crossink-internal port checklist adapted from the
`crosspoint-reader-lua` reference implementation. Following it as an ordered
reviewable checklist keeps the diff small enough for a focused PR.

### Phase 0 — Source prep & vendoring

- [ ] Copy `crosspoint-reader-lua/lib/Lua/` into `CrossInk/lib/Lua/`. The vendored library is the standard PUC-Rio Lua 5.x source distribution, unmodified.
- [ ] Add `-DLUA_32BITS` to the `[base]` `build_flags` block in `platformio.ini`. The Lua project's only Lua-specific flag.
  - **Verify first:** confirm the vendored `luaconf.h` honors `#define LUA_32BITS` to switch `LUA_NUMBER` to `float`; otherwise use `-DLUA_USE_FLOAT` instead.
- [ ] Decide whether Lua is firmware-only for `simulator` v1 or built for both. **Verify first:** CrossInk's `[env:simulator]` already has `lib_ignore = hal, WebSockets` — add `Lua` to that list if simulator support is out of scope for the first PR.
- [ ] Decide variants. Lua adds compiled bytes regardless of fonts; verify with `scripts/firmware_size_history.py` after Phase 4 before enabling on `[env:teensy]`.

### Phase 1 — C++ bridge (`src/util/LuaManager.{h,cpp}`)

- [ ] Copy the file from crosspoint-reader-lua unchanged.
- [ ] Find / replace for CrossInk:
  - `SdMan` → `Storage`
  - Direct `SdFat` headers → `<HalStorage.h>` (which already aliases `FsFile = HalFile`)
  - Direct `EInkDisplay` includes → `<HalDisplay.h>` and use `HalDisplay::RefreshMode`
- [ ] Pass `Spirit::L = luaL_newstate()`, then `luaL_openlibs(L)`, then register the `gui`, `input`, `fs`, `sys`, `net` modules.
- [ ] Store `renderer`, `input`, `lua_manager` pointers in `LUA_REGISTRYINDEX` as `lightuserdata` under keys `renderer_ptr`, `input_ptr`, `lua_manager_ptr` so C wrappers can recover them.
- [ ] Naming tweaks:
  - `gui.refresh(mode)` parameter — **`HalDisplay::RefreshMode`** is `FULL_REFRESH=0, HALF_REFRESH=1, FAST_REFRESH=2`. The Lua project uses 0/1/2 in the **opposite** order. Pick CrossInk's order and define `REFRESH_*` Lua constants accordingly.
  - `gui.drawCenteredText` — CrossInk has `UITheme::drawCenteredText(renderer, screenRect, fontId, y, text, [color], [style])`. Bind to that with a safe-area rect; do not manually compute `x = (width - textWidth)/2`.
  - `gui.setOrientation(deg)` — driven by `GfxRenderer::Orientation`. Tolerant of `Portrait / LandscapeClockwise / PortraitInverted / LandscapeCounterClockwise`.
  - `gui.drawBmp(path)` — **Verify first** that CrossInk's BMP-load path is still accessible; if only PNGdec is wired, the binding drops or routes through `HalDisplay::drawImage` after a manual SD-card load.
  - `gui.fillPolygon(...)` — drop silently if `GfxRenderer` does not expose a polygon primitive.
  - `gui.drawButtonHints({back=…, confirm=…, prev=…, next=…})` — bind via `MappedInputManager::Labels` and the current theme's hint bar helper.
- [ ] Confirm constants. The Lua project's `FONT_*` ids match CrossInk's `fontIds.h` ones for the variants CrossInk exposes. Cross-Ink has additional font ids for Lexend Deca / Bitter / ChareInk size variants; expose an appropriate subset.
- [ ] `input.*` mappings — direct one-to-one match against `MappedInputManager::Button::*`. No code change.
- [ ] `fs.*`:
  - `listDirs` — **Verify first** that CrossInk's `Storage.listFiles()` returns files **and** directories. If directories aren't reported, add a one-line extension in `HalStorage.cpp` to append `/` to directory entries, then filter in Lua.
  - `exists` / `readFile` / `writeFile` — direct mapping onto `Storage`. `readFile` returns `String`; standardize to `std::string`.
- [ ] `net.*`:
  - `net.get(url)` — **Verify first** that CrossInk's HTTP client (likely `HttpDownloader` or `CrossPointWebServer`) exposes a synchronous GET. If only async, add a thin sync wrapper that polls the existing callback.
  - `wifiConnect` / `wifiDisconnect` / `wifiStatus` — map onto CrossInk's WiFi helper (likely under `CrossPointWebServer`).

### Phase 2 — Runner (`src/activities/util/LuaActivity.{h,cpp}`)

- [ ] Copy from crosspoint-reader-lua.
- [ ] Fix include paths: `activities/Activity.h`, `util/LuaManager.h`, `MappedInputManager.h`, `HalStorage.h`, `GfxRenderer.h`.
- [ ] Wire `mappedInput.suppressNextConfirmRelease()` before the first `callFunction("draw")` so the menu's Confirm press does not leak into the plugin.
- [ ] Decide render-task behaviour. Recommended: **`render()` on the dedicated render task is a no-op; `draw()` runs from `loop()` in the main task**. Mirror the Lua project; document with a code comment explaining why.
- [ ] Wire `showError()` to either:
  - `FullScreenMessageActivity` (preferred — consistent with existing error UX); or
  - the `GUI.drawPopup(renderer, "Plugin failed to load: ...")` path used by `silentRestart()`.
- [ ] Verify `MappedInputManager::Button::Back` returns the user to PluginList cleanly. Use `Activity::finish()` so the activity manager pops back; **don't** call `ESP.restart()`.

### Phase 3 — Discovery (`src/activities/util/PluginListActivity.{h,cpp}`)

- [ ] Copy from crosspoint-reader-lua.
- [ ] Fix include paths.
- [ ] `onEnter()` scan:
  - Call `Storage.listFiles("/plugins", 64)`.
  - For each entry ending with `/`, attempt `Storage.openFileForRead("LUA", entry + "main.lua", file)` — keep on success.
  - For each successful entry, open `main.lua`, read up to 256 bytes into a stack buffer, scan for the literal token `-- DESCRIPTION:`; capture the rest of the line for the subtitle.
  - Filename without `/` is treated as a script file the user might drop directly, not a plugin — **drop silently for v1**.
- [ ] Navigation: `ButtonNavigator` for Up/Down, `Confirm` to launch, `Back` to leave.
- [ ] Empty-state copy: bind to a new `StrId::STR_NO_PLUGINS` in `lib/I18n/index.yaml`.

### Phase 4 — Build configuration

- [ ] `platformio.ini` `[base]` `build_flags`: append `-DLUA_32BITS`.
- [ ] Run `pio run -e default` (compile-only) **before** moving on to Phase 5.
- [ ] If Lua is firmware-only for v1, add `Lua` to `[env:simulator]` `lib_ignore` and skip simulator validation this round.

### Phase 5 — Wiring (`src/main.cpp`)

- [ ] Includes next to KOReaderCredentialStore include:
  - `util/LuaManager.h`
  - `activities/util/LuaActivity.h`
  - `activities/util/PluginListActivity.h`
- [ ] Define `LuaManager luaManager;` at file top next to `activityManager`.
- [ ] Add `onGoToLuaPlugins()`:
  ```cpp
  void onGoToLuaPlugins() {
    activityManager.pushActivity(std::make_unique<PluginListActivity>(
        renderer, mappedInput,
        /*onGoBack*/ []() { activityManager.goHome(); },
        /*onLaunchPlugin*/ [](const std::string& name) {
          activityManager.replaceActivity(std::make_unique<LuaActivity>(
              renderer, mappedInput, name,
              /*onGoBack*/ []() { onGoToLuaPlugins(); }));
        }));
  }
  ```
- [ ] Add a Plugins entry to `HomeActivity`'s menu — see UX decision below.
- [ ] Extend `ActivityManager::goHome()`'s `if/else if` chain so returning from a `"LuaActivity"` lands on the Plugins menu (keeps the user's focus where they came from).

### Phase 6 — UX decision: home entry

Two candidates:

1. **Top-level home menu row** (matches the Lua project's UX):
   - [ ] Add `HomeMenuItem::PLUGINS` in `src/activities/ActivityManager.h` `enum class HomeMenuItem { ... PLUGINS };`.
   - [ ] Append a row in `src/activities/home/HomeActivity.{h,cpp}`'s menu list.
2. **Settings → Device submenu** (keeps the home screen uncluttered):
   - [ ] Add a Plugins submenu under `SettingsActivity` → `System → Device`.

**Recommend (a)** since `crosspoint-reader-lua` exposes it at the top level, keeping parity with reference plugins.

### Phase 7 — i18n

- [x] Add `STR_PLUGINS`, `STR_NO_PLUGINS`, `STR_PLUGIN_LOADING`, `STR_PLUGIN_LOAD_FAILED` to `lib/I18n/index.yaml`.
- [x] `scripts/gen_i18n.py` regenerates headers automatically on next `pio run`.

### Phase 8 — Seed example

- [ ] Copy `crosspoint-reader-lua/sdcard/plugins/StopWatch/main.lua` into `CrossInk/sdcard/plugins/StopWatch/main.lua` as the worked example.
- [ ] Optionally include `Sudoku` or `Snake` as richer references — but they generate larger flash images, so prefer `StopWatch` for the default seed.

### Phase 9 — Tests

- [ ] No host unit tests for v1. Lua bindings are best validated in the simulator (Phase 10) and on a hardware X4.
- [ ] If you want a host test from scratch:
  - Add `test/lua_integration/CMakeLists.txt` mirroring the existing `test/*/CMakeLists.txt` pattern.
  - Build `lib/Lua/` subset with the chunk_reader exercised against in-memory strings.
  - Aim for ~200 LoC, no filesystem fixtures required.

### Phase 10 — Validation

Run these in order; each is non-destructive.

- [x] `pio check -e default --fail-on-defect low --fail-on-defect medium --fail-on-defect high` — static analysis. Fix all hits.
- [x] `find src lib -name "*.cpp" -o -name "*.h" | xargs clang-format -i` — but only on touched files, to keep the diff tight.
- [x] `pio run -e default` — default firmware build.
- [x] `pio run -e teensy` — minimum firmware; confirm Lua still fits.
- [x] `pio run -e xlarge` — maximum firmware; confirm the Lua-vs-no-Lua delta is in the expected 80-200 KB range.
- [x] `pio run -e simulator` — host build (only if not in `lib_ignore`).
- [ ] On a hardware X4 (optional, recommended):
  - Drop a `main.lua` test plugin in `/plugins/StopWatch/`.
  - Boot → Home → Plugins → StopWatch shows with the DESCRIPTION subtitle.
  - Launch. Confirm `init()` runs once, `draw()` updates per press, no flicker.
  - Press Back twice — return to PluginList, then to Home, with no crash or heap leak.
  - Tail serial logs: free heap should drop ~15–25 KB on plugin start, return to baseline on exit.
  - Force an error: rename `main.lua` to `Main.lua` and confirm `STR_PLUGIN_LOAD_FAILED` displays.

### Phase 11 — Changelog & version

- [ ] Add a `feat:` entry under `## [Unreleased]` → `### Added` in `CHANGELOG.md`.
- [ ] Bump `crossink_version` in `platformio.ini`'s `[crosspoint]` block if any user-facing surface changed (e.g. home menu row). Otherwise no bump.

### Phase 12 — Out of scope for v1

Park these with explicit `// TODO: crossink-future` markers.

- Always-on / boot-time Lua hooks (would complicate Back semantics).
- Per-plugin memory quota.
- Plugin-side i18n (auto-creating translations from Lua strings).
- Async `net.http.get` callbacks.
- `Lua`-side activity push/pop (only `init` + `draw` lifecycle for now).
- A standalone Lua REPL activity.

---

## Things to verify on the very first compile pass

1. `luaL_newstate()` succeeds — log free heap before/after. Expect ~30–40 KB allocation on ESP32-C3.
2. `luaL_openlibs()` succeeds without OOM.
3. The `chunk_reader` actually streams: open a 32 KB plugin, log heap during each read; expect a delta of ~2 KB of buffer state, not 32 KB.
4. Returning from a Lua plugin via Back lands cleanly on PluginList; the `MappedInputManager` `inputReady` debounce does not accidentally swallow the Back release.
5. `sys.exit()` lets `LuaManager::end()` release the VM cleanly — free heap returns to within ~6-8 KB of baseline (Lua internals hold some residual state).
6. No `LOG_ERR` from `l_sys_delay` — `sys.delay()` blocks the main loop including button polling. Plugins are expected to use it sparingly; document rather than fix.

---

## Related documentation

- `USER_GUIDE.md` — End-user orientation.
- `docs/controls.md` — How the host device's button mapping translates to plugin-visible input events.
- `CHANGELOG.md` — Release notes.
- `lib/I18n/index.yaml` — String catalogue maintained by translators.
- `scripts/gen_i18n.py` — Generates `lib/I18n/*.generated.h` from the catalogue.
