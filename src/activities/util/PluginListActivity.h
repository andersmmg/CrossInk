#pragma once

#include <GfxRenderer.h>
#include <MappedInputManager.h>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * PluginListActivity — Discovers and lists Lua plugins dropped under
 * `/plugins/<PluginName>/main.lua` on the SD card. Each plugin is loaded
 * into the existing LuaActivity when the user confirms a row.
 *
 * docs/plugins.md Phase 3 ("discovery") + Phase 5 ("wiring") cover this
 * design:
 *   - scan happens exactly once in onEnter(); the result is mirrored
 *     onto a fixed-capacity array so the loop() / render() hot paths
 *     only navigate the in-memory selector (no FS traffic per frame).
 *   - the user-facing row label is the directory name; the description
 *     next to it is parsed from a `-- DESCRIPTION: ...` magic comment
 *     inside the first 256 bytes of main.lua (see docs/...
 *     part 1 "Description metadata"). A missing comment falls back to
 *     STR_NO_PLUGIN_DESCRIPTION (= "Lua Script Extension").
 *
 * The activity is named `"PluginList"` so ActivityManager::goHome() can
 * route the user back to the Plugins home row when they press Back.
 */
class PluginListActivity : public Activity {
 public:
  explicit PluginListActivity(GfxRenderer& renderer, MappedInputManager& input);
  ~PluginListActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  // Fixed-capacity so onEnter has zero heap churn. Stack use is bounded
  // by sizeof(PluginEntry[]) which is well under 256 bytes per AGENTS.md
  // heap-discipline. Each entry holds a directory name, a parsed
  // description, and the full SD-card path to the entry-point script.
  struct PluginEntry {
    // Soft caps kept for documentation; std::string enforces no bound
    // by itself. Real-world plugin names fit well under kNameMax and
    // descriptions under kDescMax; kPathMax guards against absurdly
    // deep /plugins/<name>/main.lua paths.
    static constexpr size_t kNameMax = 31;
    static constexpr size_t kDescMax = 95;
    static constexpr size_t kPathMax = 63;

    // std::string (not char[]) so the row getter lambdas in
    // renderPluginList() can do `return plugins_[index].name;` — the
    // implicit move + RVO elide the heap allocation that
    // `std::string(plugins_[index].name)` would trigger on EVERY redraw.
    // Short strings stay zero-alloc under SSO (size is implementation-
    // defined; newlib-nano uses a smaller buffer than libstdc++). Long
    // strings are moved not copied (cheap pointer transfer). Each entry's
    // std::string is allocated once during scanPlugins() at onEnter()
    // and reused across all subsequent draws + launches.
    std::string name;
    std::string description;
    std::string path;
  };

  // 24 entries × ~192 bytes/entry ≈ 4.6 KB static. Comfortably within the
  // 380 KB no-PSRAM RAM budget. Beyond this we silently drop the rest with
  // a LOG_WRN; users rarely install more plugins than this on a single
  // device and the row count is bounded by what the e-ink panel can show.
  static constexpr int kMaxPlugins = 24;

  PluginEntry plugins_[kMaxPlugins]{};
  int pluginCount_ = 0;
  int selectorIndex_ = 0;

  ButtonNavigator buttonNavigator;

  // Performs the SD scan. Populates `plugins_` and writes the count to
  // `pluginCount_`. Always closes every handle it opens so a failed scan
  // can't leak a FatFs entry to the LuaActivity that will run on top of
  // this activity (the SDK's SdFat on real hardware allows only one
  // file open at a time — see AGENTS.md "Single reader" rule).
  int scanPlugins();

  // Reads up to `kDescProbeBytes` from the head of `main.lua` and copies
  // the first `-- DESCRIPTION: <text>` line into `out`. Returns true on
  // success. Closes the file handle before returning.
  std::string readFirstLineDescription(const char* absPath);

  void renderHeader();
  void renderPluginList();
  void renderEmptyState();
  void launchSelected();
};
