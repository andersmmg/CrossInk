#pragma once

#include <GfxRenderer.h>
#include <MappedInputManager.h>

#include <string>

#include "activities/Activity.h"
#include "util/LuaManager.h"

/**
 * LuaActivity — Hosts a Lua plugin script as a CrossInk Activity.
 *
 * Lifecycle:
 *   - onEnter(): spin up LuaManager with platform refs, load + run the
 *                script's global `init()` function (if defined). Any Lua
 *                runtime error during `init()` is logged and surfaces as a
 *                rendered error banner on the framebuffer so the user sees
 *                something instead of a black screen. Schedules an
 *                `requestUpdate()` at the end so the render task picks up
 *                whatever `gui.refresh(mode)` was queued.
 *   - loop(): once per frame. Calls Lua's global `draw()` (if defined).
 *             Polls no input directly — `input.wasPressed(name)` from Lua
 *             is forwarded to MappedInputManager::wasPressed which already
 *             tracks edges for the current frame.
 *   - render(RenderLock&&): consumes LuaManager::takePendingRefresh(); if
 *             a refresh was queued (via gui.refresh(mode) inside draw()),
 *             calls renderer.displayBuffer() under the lock. Without
 *             this, the framebuffer change made by draw() never reaches
 *             the e-ink panel / SDL window. If `init()` failed (ready_ is
 *             false and lastError_ is set), the error banner is drawn and
 *             committed with a FULL_REFRESH so the user sees it.
 *   - onExit(): close the LuaManager (releases lua_State). Exit codes
 *              captured from `sys.exit(rc)` / `os.exit(rc)` are LOG'd for
 *              debug but NOT propagated to the host process — LuaActivity
 *              integrates with the activity stack, and pressing Back (or
 *              any Lua exit call) must pop back to the previous activity
 *              (PluginList → Home), never terminate the simulator.
 *
 * Host integration:
 *   - Launched by PluginListActivity::launchSelected() with the resolved
 *     /plugins/<name>/main.lua path. Loops stay alive for redraws; no env
 *     vars or CLI flags gate it. Pressing Back from the Plugins list
 *     already returns to HomeActivity; pressing Back or calling sys.exit
 *     inside Lua does the same.
 *

 * docs/plugins.md covers the surfaced entry path — PluginListActivity pushes
 * a LuaActivity for /plugins/<name>/main.lua, this activity runs the script's
 * `init()` on enter and `draw()` once per frame, and Back / sys.exit pop
 * back to PluginListActivity → Home.
 */
class LuaActivity : public Activity {
 public:
  LuaActivity(GfxRenderer& renderer, MappedInputManager& input, const std::string& scriptPath);
  ~LuaActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  // `renderer` and `mappedInput` are inherited from `Activity` — do not
  // shadow them with refs; the base stores them by reference and re-uses
  // them across our render / loop calls. Keeping member refs here would
  // double the indirection without buying anything.
  std::string scriptPath_;
  LuaManager mgr_;
  // True after the script ran init() successfully — guards against
  // re-entering draw() before Lua is ready.
  bool ready_ = false;
  // Last fatal-message used to seed the rendered error banner. Cleared
  // when the user finishes the activity.
  std::string lastError_;

  void runLuaGlobal(const char* name);
  void renderErrorBanner();
  void renderLoadingOverlay();
};
