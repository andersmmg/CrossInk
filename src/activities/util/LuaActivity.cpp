#include "LuaActivity.h"

#include <EpdFontFamily.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>
#include <fontIds.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "util/LuaManager.h"
#include "util/UrlUtils.h"

// Activity base ctor requires a std::string name; we pass "LuaActivity" so the
// ActivityManager logs/scrrenshots can identify this activity without
// inspecting its LuaActivity scriptPath field.
LuaActivity::LuaActivity(GfxRenderer& renderer, MappedInputManager& input, const std::string& scriptPath)
    : Activity("LuaActivity", renderer, input), scriptPath_(scriptPath) {}

LuaActivity::~LuaActivity() { mgr_.close(); }

void LuaActivity::onEnter() {
  LOG_DBG("LUA", "LuaActivity::onEnter scriptPath=%s", scriptPath_.c_str());
  // PluginListActivity consumed the press edge of Confirm when it called
  // launchSelected(), but the matching release edge stays queued in
  // HalGPIO until the user lifts the button. On the frame the release
  // fires, LuaActivity's draw() -> input.wasReleased("confirm") would
  // see it and the script would interpret it as an in-plugin event
  // (e.g. a rigid "release == accept" binding or a held-press toggle).
  // Phase-2 wiring checklist item in docs/plugins.md: set the flag
  // before the first callFunction("draw") so the menu's launch-press
  // can't leak into the plugin. The flag is one-shot — the next
  // wasReleased(Confirm) call clears it — and `wasPressed` /
  // `isPressed` are unaffected, so genuine in-plugin Confirm gestures
  // behave normally. Harmless on the init-failure path: the rendered
  // error banner's footer reads "press Back to exit", so Confirm-
  // release absorption doesn't change what the user can do there.
  mappedInput.suppressNextConfirmRelease();
  if (!mgr_.init(renderer, mappedInput)) {
    lastError_ = "LuaManager init failed (out of memory?)";
    LOG_ERR("LUA", "%s", lastError_.c_str());
    // Still kick the render task so the error banner reaches the panel —
    // see render()'s !ready_ branch below.
    requestUpdate();
    return;
  }
  // Load the user script. A load failure here means syntax error or missing
  // file — record it so the render pass surfaces it on the e-ink. Routed
  // through loadLuaFromStorage so the simulator's virtual-SD-root path
  // resolution matches what PluginListActivity::scanPlugins used to
  // discover the same file (see util/LuaManager.cpp for the long version).
  if (LuaManager::loadLuaFromStorage(mgr_.L(), scriptPath_) != LUA_OK) {
    lastError_ = std::string("Lua load error: ") + lua_tostring(mgr_.L(), -1);
    LOG_ERR("LUA", "%s", lastError_.c_str());
    lua_pop(mgr_.L(), 1);
    requestUpdate();
    return;
  }
  // Run the chunk — exposes `init`, `draw`, etc. globals onto the stack.
  if (lua_pcall(mgr_.L(), 0, 0, 0) != LUA_OK) {
    lastError_ = std::string("Lua runtime error: ") + lua_tostring(mgr_.L(), -1);
    LOG_ERR("LUA", "%s", lastError_.c_str());
    lua_pop(mgr_.L(), 1);
    requestUpdate();
    return;
  }
  ready_ = true;
  runLuaGlobal("init");
  // Schedule the first paint. Without requestUpdate, the render task is never
  // notified, so the framebuffer contents from init()'s gui.drawText /
  // gui.fillRect / gui.refresh(REFRESH_FULL) call sit there invisible —
  // a black screen on the e-ink panel even though everything "worked".
  // PluginListActivity::onEnter() ends with the same call for the same
  // reason; this is the missing one-liner that produces the apparent freeze.
  //
  // Race note: init()'s gui.* calls mutate the framebuffer on the main task
  // without holding the RenderLock; the render task later reads it under
  // the lock to commit the display. Safe today because plugin scripts
  // (HelloNet, the smoke test, the example plugin) don't draw from draw() —
  // they only draw once in init(). A future plugin that draws per frame
  // from draw() will be racy and needs a redesign so all framebuffer
  // writes happen inside render(RenderLock&&).
  requestUpdate();
}

void LuaActivity::onExit() {
  LOG_DBG("LUA", "LuaActivity::onExit");
  mgr_.close();
}

void LuaActivity::loop() {
  if (!ready_) {
    return;
  }
  // If `sys.exit` was called during a prior draw(), bail out to the activity
  // manager. Sys.exit is requested by the script calling `sys.exit(rc)`;
  // we honor it here rather than mid-`lua_pcall` so the Lua state is torn
  // down cleanly through onExit() below. The rc is purely informational —
  // nothing in this Activity propagates it to the host process; the
  // surfaced plugin path must NOT kill the simulator.
  const int rc = mgr_.takePendingExit();
  if (rc >= 0) {
    LOG_DBG("LUA", "script requested sys.exit(%d); finishing activity", rc);
    finish();
    return;
  }
  // Run the script's per-tick body. draw() may or may not write to the
  // framebuffer / queue a gui.refresh — both are valid. Either way, the
  // script doesn't notify the render task on its own: that's our job.
  // Calling requestUpdate() every tick is cheap (ActivityManager's
  // deferred path coalesces many calls into one render-task wake-up), and
  // render() bails cleanly if takePendingRefresh() returns -1 (no frame
  // was queued this tick). Without this call the script's framebuffer
  // changes never reach the e-ink / SDL panel — `draw()` could run for
  // minutes doing real work and the user would still see the previous
  // activity's frame.
  //
  // HelloNet works by accident: its init() (not draw()) calls
  // gui.refresh, and onEnter()'s requestUpdate() fires the render task
  // exactly once at startup. StopWatch's init() doesn't draw, so this
  // loop()-side requestUpdate() is what makes per-tick redraws reach the
  // panel at all.
  runLuaGlobal("draw");
  requestUpdate();
}

void LuaActivity::render(RenderLock&& /*lock*/) {
  const int mode = mgr_.takePendingRefresh();
  if (!ready_) {
    // !ready_ covers two non-running states:
    //
    //   1. lastError_ set → init/load/parse failed: render the error
    //      banner so the user can see what went wrong.
    //   2. lastError_ empty → init hasn't finished on the main task yet.
    //      Today onEnter() runs the whole synchronous load before any
    //      render-task firing, so this branch is unreachable. Keeping
    //      the layout in place so a future async loader lands on a
    //      centered "Loading plugin..." overlay instead of a stale
    //      frame from the previous activity.
    if (!lastError_.empty()) {
      renderErrorBanner();
      renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    } else {
      renderLoadingOverlay();
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    return;
  }
  if (mode < 0) {
    return;  // No refresh requested this frame.
  }
  // Translate Lua's 0/1/2 integers back into the HalDisplay enum value.
  HalDisplay::RefreshMode rm = HalDisplay::FAST_REFRESH;
  switch (mode) {
    case 0:
      rm = HalDisplay::FULL_REFRESH;
      break;
    case 1:
      rm = HalDisplay::HALF_REFRESH;
      break;
    case 2:
      rm = HalDisplay::FAST_REFRESH;
      break;
    default:
      rm = HalDisplay::FAST_REFRESH;
      break;
  }
  LOG_DBG("LUA", "render: committing refresh mode=%d at millis=%lu", static_cast<int>(rm), ::millis());
  renderer.displayBuffer(rm);
}

void LuaActivity::runLuaGlobal(const char* name) {
  lua_State* L = mgr_.L();
  if (L == nullptr) return;
  lua_getglobal(L, name);
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 1);
    return;
  }
  if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
    const std::string err = std::string(name) + "() error: " + lua_tostring(L, -1);
    LOG_ERR("LUA", "%s", err.c_str());
    lastError_ = err;
    lua_pop(L, 1);
    renderer.clearScreen(0xFF);
    renderErrorBanner();
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  }
}

void LuaActivity::renderErrorBanner() {
  if (lastError_.empty()) return;
  renderer.clearScreen(0xFF);
  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  // Frame the error pane so it's clearly demarcated from the rest of the
  // screen (e.g. over a previous frame's stale contents). drawRect on its
  // own leaves just an outline; that's what we want here so the dialog reads
  // like a structured error card against the cleared background.
  renderer.drawRect(8, 8, w - 16, h - 16, /*black=*/true);

  const int yTitle = 28;
  const int yMsg = 60;
  // STR_PLUGIN_LOAD_FAILED is the user-facing header for any plugin
  // failure (mgr_.init OOM, loadLuaFromStorage parse/read error, runtime
  // error from init()'s pcall, runtime error from draw()'s runLuaGlobal).
  // Was hard-coded "Lua plugin failed:" before the i18n wiring landed.
  renderer.drawText(UI_10_FONT_ID, 16, yTitle, tr(STR_PLUGIN_LOAD_FAILED), /*black=*/true);

  // Wrap-ish: chunk the message into lines by space.
  const char* msg = lastError_.c_str();
  const size_t maxLineChars = 96;
  std::string line;
  int y = yMsg;
  for (size_t i = 0; msg[i] != '\0' && y < h - 24; ++i) {
    line.push_back(msg[i]);
    if (line.size() >= maxLineChars || msg[i + 1] == '\0' || (msg[i] == ' ' && line.size() > 40)) {
      renderer.drawText(SMALL_FONT_ID, 16, y, line.c_str(), /*black=*/true);
      y += 18;
      line.clear();
    }
  }
  if (!line.empty() && y < h - 24) {
    renderer.drawText(SMALL_FONT_ID, 16, y, line.c_str(), /*black=*/true);
  }
  // Footer hint
  char footer[64];
  std::snprintf(footer, sizeof(footer), "press Back to exit (width=%d)", w);
  renderer.drawCenteredText(UI_10_FONT_ID, h - 28, footer, /*black=*/true);
}

void LuaActivity::renderLoadingOverlay() {
  // Defensive state: render() fires with !ready_ and no error. Today this
  // branch is unreachable — onEnter() runs the whole sync init/load/parse
  // on the main task, so by the time the render task first runs, ready_ is
  // either true or lastError_ is set. Keeping the code path in place so a
  // future async loader (e.g. one that defers the SD read or precompiled
  // cache lookup to a worker task) doesn't need to invent the layout
  // on the spot. Loader commits via HALF_REFRESH so a single flash on the
  // e-ink tells the user something is happening without burning a full
  // refresh for a text label.
  renderer.clearScreen(0xFF);
  const int h = renderer.getScreenHeight();
  renderer.drawCenteredText(UI_10_FONT_ID, h / 2, tr(STR_PLUGIN_LOADING), /*black=*/true);
}
