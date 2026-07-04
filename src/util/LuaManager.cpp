#include "LuaManager.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#ifdef SIMULATOR
#include <WiFi.h>
#endif
#include <Logging.h>
#include <MappedInputManager.h>
#include <fontIds.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

#include "network/NetClient.h"
#include "util/UrlUtils.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace {

std::atomic<int> g_pendingExit{-1};
std::atomic<int> g_pendingRefresh{-1};

struct ButtonNameToEnum {
  const char* name;
  MappedInputManager::Button button;
};
constexpr ButtonNameToEnum kButtonTable[] = {
    {"confirm", MappedInputManager::Button::Confirm},
    {"back", MappedInputManager::Button::Back},
    {"up", MappedInputManager::Button::Up},
    {"down", MappedInputManager::Button::Down},
    {"left", MappedInputManager::Button::Left},
    {"right", MappedInputManager::Button::Right},
    {"page_back", MappedInputManager::Button::PageBack},
    {"page_forward", MappedInputManager::Button::PageForward},
};

const MappedInputManager::Button* lookupButton(const char* name) {
  const auto end = std::end(kButtonTable);
  const auto it = std::find_if(std::begin(kButtonTable), end,
                               [name](const ButtonNameToEnum& entry) { return std::strcmp(entry.name, name) == 0; });
  return it != end ? &it->button : nullptr;
}

GfxRenderer* getRenderer(lua_State* L) {
  lua_getfield(L, LUA_REGISTRYINDEX, "renderer_ptr");
  GfxRenderer* r = static_cast<GfxRenderer*>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  return r;
}

MappedInputManager* getInput(lua_State* L) {
  lua_getfield(L, LUA_REGISTRYINDEX, "input_ptr");
  MappedInputManager* i = static_cast<MappedInputManager*>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  return i;
}

// =========================================================================
//                                   net
// =========================================================================

#ifdef SIMULATOR
int lua_net_wifiConnect(lua_State* L) {
  const char* ssid = luaL_checkstring(L, 1);
  const char* pass = luaL_optstring(L, 2, "");
  const int status = WiFi.begin(ssid, pass);
  lua_pushboolean(L, status == WL_CONNECTED);
  return 1;
}

int lua_net_wifiStatus(lua_State* L) {
  lua_pushinteger(L, static_cast<lua_Integer>(WiFi.status()));
  return 1;
}

int lua_net_wifiDisconnect(lua_State* L) {
  (void)L;
  WiFi.disconnect();
  return 0;
}
#endif

int lua_net_urlencode(lua_State* L) {
  const char* s = luaL_checkstring(L, 1);
  const std::string out = UrlUtils::urlencode(s);
  lua_pushlstring(L, out.data(), out.size());
  return 1;
}

int lua_net_get(lua_State* L) {
  const char* url = luaL_checkstring(L, 1);
  std::string body;
  if (!NetClient::get(url, body)) {
    lua_pushnil(L);
    const auto& err = NetClient::lastError();
    lua_pushlstring(L, err.data(), err.size());
    return 2;
  }
  lua_pushlstring(L, body.data(), body.size());
  return 1;
}

int lua_net_wifiConnectUnavailable(lua_State* L) {
  return luaL_error(L, "net.wifiConnect is not implemented on firmware yet");
}

int lua_net_wifiStatusUnavailable(lua_State* L) {
  return luaL_error(L, "net.wifiStatus is not implemented on firmware yet");
}

int lua_net_wifiDisconnectUnavailable(lua_State* L) {
  return luaL_error(L, "net.wifiDisconnect is not implemented on firmware yet");
}

const luaL_Reg kNetFuncs[] = {
    {"urlencode", lua_net_urlencode},
    {"get", lua_net_get},
#ifdef SIMULATOR
    {"wifiConnect", lua_net_wifiConnect},
    {"wifiStatus", lua_net_wifiStatus},
    {"wifiDisconnect", lua_net_wifiDisconnect},
#else
    {"wifiConnect", lua_net_wifiConnectUnavailable},
    {"wifiStatus", lua_net_wifiStatusUnavailable},
    {"wifiDisconnect", lua_net_wifiDisconnectUnavailable},
#endif
    {nullptr, nullptr},
};

void registerNetModule(lua_State* L) {
  luaL_newlib(L, kNetFuncs);
  lua_setglobal(L, "net");
}

static EpdFontFamily::Style luaStyle(lua_State* L, int arg, EpdFontFamily::Style fallback) {
  const int v = static_cast<int>(luaL_optinteger(L, arg, static_cast<lua_Integer>(fallback)));
  if (v < 0 || v > 3) {
    LOG_ERR("LUA", "style %d out of range, using REGULAR", v);
    return EpdFontFamily::REGULAR;
  }
  return static_cast<EpdFontFamily::Style>(v);
}

static int luaColorToBlack(lua_State* L, int arg) {
  // 0 (default) → true (black ink). 1 → false (white ink). Other → true.
  const int v = static_cast<int>(luaL_optinteger(L, arg, 0));
  return v != 1;
}

int lua_gui_width(lua_State* L) {
  lua_getfield(L, LUA_REGISTRYINDEX, "renderer_ptr");
  const GfxRenderer* const r = static_cast<GfxRenderer*>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  if (!r) return luaL_error(L, "renderer not initialized for gui.* bindings");
  lua_pushinteger(L, static_cast<lua_Integer>(r->getScreenWidth()));
  return 1;
}

int lua_gui_height(lua_State* L) {
  lua_getfield(L, LUA_REGISTRYINDEX, "renderer_ptr");
  const GfxRenderer* const r = static_cast<GfxRenderer*>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  if (!r) return luaL_error(L, "renderer not initialized for gui.* bindings");
  lua_pushinteger(L, static_cast<lua_Integer>(r->getScreenHeight()));
  return 1;
}

int lua_gui_clearScreen(lua_State* L) {
  const GfxRenderer* const r = getRenderer(L);
  if (!r) return luaL_error(L, "renderer not initialized for gui.* bindings");
  r->clearScreen(0xFF);
  return 0;
}

int lua_gui_drawText(lua_State* L) {
  const GfxRenderer* const r = getRenderer(L);
  if (!r) return luaL_error(L, "renderer not initialized for gui.* bindings");
  const int fontId = static_cast<int>(luaL_checkinteger(L, 1));
  const int x = static_cast<int>(luaL_checkinteger(L, 2));
  const int y = static_cast<int>(luaL_checkinteger(L, 3));
  const char* text = luaL_checkstring(L, 4);
  const bool black = luaColorToBlack(L, 5) != 0;
  const EpdFontFamily::Style style = luaStyle(L, 6, EpdFontFamily::REGULAR);
  r->drawText(fontId, x, y, text, black, style);
  return 0;
}

int lua_gui_drawCenteredText(lua_State* L) {
  const GfxRenderer* const r = getRenderer(L);
  if (!r) return luaL_error(L, "renderer not initialized for gui.* bindings");
  const int fontId = static_cast<int>(luaL_checkinteger(L, 1));
  const int y = static_cast<int>(luaL_checkinteger(L, 2));
  const char* text = luaL_checkstring(L, 3);
  const bool black = luaColorToBlack(L, 4) != 0;
  const EpdFontFamily::Style style = luaStyle(L, 5, EpdFontFamily::REGULAR);
  r->drawCenteredText(fontId, y, text, black, style);
  return 0;
}

int lua_gui_getTextWidth(lua_State* L) {
  const GfxRenderer* const r = getRenderer(L);
  if (!r) return luaL_error(L, "renderer not initialized for gui.* bindings");
  const int fontId = static_cast<int>(luaL_checkinteger(L, 1));
  const char* text = luaL_checkstring(L, 2);
  const EpdFontFamily::Style style = luaStyle(L, 3, EpdFontFamily::REGULAR);
  lua_pushinteger(L, static_cast<lua_Integer>(r->getTextWidth(fontId, text, style)));
  return 1;
}

int lua_gui_drawRect(lua_State* L) {
  const GfxRenderer* const r = getRenderer(L);
  if (!r) return luaL_error(L, "renderer not initialized for gui.* bindings");
  const int x = static_cast<int>(luaL_checkinteger(L, 1));
  const int y = static_cast<int>(luaL_checkinteger(L, 2));
  const int w = static_cast<int>(luaL_checkinteger(L, 3));
  const int h = static_cast<int>(luaL_checkinteger(L, 4));
  const bool black = luaColorToBlack(L, 5) != 0;
  r->drawRect(x, y, w, h, black);
  return 0;
}

int lua_gui_fillRect(lua_State* L) {
  const GfxRenderer* const r = getRenderer(L);
  if (!r) return luaL_error(L, "renderer not initialized for gui.* bindings");
  const int x = static_cast<int>(luaL_checkinteger(L, 1));
  const int y = static_cast<int>(luaL_checkinteger(L, 2));
  const int w = static_cast<int>(luaL_checkinteger(L, 3));
  const int h = static_cast<int>(luaL_checkinteger(L, 4));
  const bool black = luaColorToBlack(L, 5) != 0;
  r->fillRect(x, y, w, h, black);
  return 0;
}

int lua_gui_refresh(lua_State* L) {
  const int mode = static_cast<int>(luaL_optinteger(L, 1, 2));
  if (mode < 0 || mode > 2) {
    LOG_ERR("LUA", "refresh mode %d out of range, using FAST", mode);
  }
  g_pendingRefresh.store(mode, std::memory_order_release);
  return 0;
}

const luaL_Reg kGuiFuncs[] = {
    {"width", lua_gui_width},
    {"height", lua_gui_height},
    {"clear", lua_gui_clearScreen},
    {"drawText", lua_gui_drawText},
    {"drawCenteredText", lua_gui_drawCenteredText},
    {"getTextWidth", lua_gui_getTextWidth},
    {"drawRect", lua_gui_drawRect},
    {"fillRect", lua_gui_fillRect},
    {"refresh", lua_gui_refresh},
    {nullptr, nullptr},
};

void registerGuiModule(lua_State* L) {
  luaL_newlib(L, kGuiFuncs);
  lua_setglobal(L, "gui");
}

// =========================================================================
//                                  input
// =========================================================================

int lua_input_wasPressed(lua_State* L) {
  const MappedInputManager* const i = getInput(L);
  if (!i) return luaL_error(L, "input not initialized for input.* bindings");
  const char* name = luaL_checkstring(L, 1);
  const auto* btn = lookupButton(name);
  if (!btn) return luaL_error(L, "unknown input button: %s", name);
  lua_pushboolean(L, i->wasPressed(*btn));
  return 1;
}

int lua_input_wasReleased(lua_State* L) {
  const MappedInputManager* const i = getInput(L);
  if (!i) return luaL_error(L, "input not initialized for input.* bindings");
  const char* name = luaL_checkstring(L, 1);
  const auto* btn = lookupButton(name);
  if (!btn) return luaL_error(L, "unknown input button: %s", name);
  lua_pushboolean(L, i->wasReleased(*btn));
  return 1;
}

int lua_input_isPressed(lua_State* L) {
  const MappedInputManager* const i = getInput(L);
  if (!i) return luaL_error(L, "input not initialized for input.* bindings");
  const char* name = luaL_checkstring(L, 1);
  const auto* btn = lookupButton(name);
  if (!btn) return luaL_error(L, "unknown input button: %s", name);
  lua_pushboolean(L, i->isPressed(*btn));
  return 1;
}

const luaL_Reg kInputFuncs[] = {
    {"wasPressed", lua_input_wasPressed},
    {"wasReleased", lua_input_wasReleased},
    {"isPressed", lua_input_isPressed},
    {nullptr, nullptr},
};

void registerInputModule(lua_State* L) {
  luaL_newlib(L, kInputFuncs);
  lua_setglobal(L, "input");
}

// =========================================================================
//                                   sys
// =========================================================================

int lua_sys_millis(lua_State* L) {
  lua_pushinteger(L, static_cast<lua_Integer>(::millis()));
  return 1;
}

int lua_sys_delay(lua_State* L) {
  const int ms = static_cast<int>(luaL_checkinteger(L, 1));
  if (ms < 0) return 0;
  ::delay(static_cast<unsigned long>(ms));
  return 0;
}

int lua_sys_exit(lua_State* L) {
  const int rc = static_cast<int>(luaL_optinteger(L, 1, 0));
  g_pendingExit.store(rc, std::memory_order_release);
  return 0;
}

const luaL_Reg kSysFuncs[] = {
    {"millis", lua_sys_millis},
    {"delay", lua_sys_delay},
    {"exit", lua_sys_exit},
    {nullptr, nullptr},
};

void registerSysModule(lua_State* L) {
  luaL_newlib(L, kSysFuncs);
  lua_setglobal(L, "sys");
}

// =========================================================================
//                          Constants & os.exit override
// =========================================================================

void registerConstants(lua_State* L) {
  lua_pushinteger(L, 0);
  lua_setglobal(L, "REFRESH_FULL");
  lua_pushinteger(L, 1);
  lua_setglobal(L, "REFRESH_HALF");
  lua_pushinteger(L, 2);
  lua_setglobal(L, "REFRESH_FAST");
  lua_pushinteger(L, 0);
  lua_setglobal(L, "COLOR_BLACK");
  lua_pushinteger(L, 1);
  lua_setglobal(L, "COLOR_WHITE");
  lua_pushinteger(L, 0);
  lua_setglobal(L, "STYLE_REGULAR");
  lua_pushinteger(L, 1);
  lua_setglobal(L, "STYLE_BOLD");
  lua_pushinteger(L, 2);
  lua_setglobal(L, "STYLE_ITALIC");
  lua_pushinteger(L, 3);
  lua_setglobal(L, "STYLE_BOLD_ITALIC");

  lua_pushinteger(L, SMALL_FONT_ID);
  lua_setglobal(L, "FONT_SMALL");
  lua_pushinteger(L, UI_10_FONT_ID);
  lua_setglobal(L, "FONT_UI_10");
  lua_pushinteger(L, UI_12_FONT_ID);
  lua_setglobal(L, "FONT_UI_12");
  lua_pushinteger(L, CHAREINK_8_FONT_ID);
  lua_setglobal(L, "FONT_CHAREINK_8");
  lua_pushinteger(L, CHAREINK_9_FONT_ID);
  lua_setglobal(L, "FONT_CHAREINK_9");
  lua_pushinteger(L, CHAREINK_10_FONT_ID);
  lua_setglobal(L, "FONT_CHAREINK_10");
  lua_pushinteger(L, CHAREINK_12_FONT_ID);
  lua_setglobal(L, "FONT_CHAREINK_12");
  lua_pushinteger(L, CHAREINK_14_FONT_ID);
  lua_setglobal(L, "FONT_CHAREINK_14");
  lua_pushinteger(L, CHAREINK_16_FONT_ID);
  lua_setglobal(L, "FONT_CHAREINK_16");
  lua_pushinteger(L, CHAREINK_18_FONT_ID);
  lua_setglobal(L, "FONT_CHAREINK_18");
  lua_pushinteger(L, CHAREINK_20_FONT_ID);
  lua_setglobal(L, "FONT_CHAREINK_20");
  lua_pushinteger(L, LEXENDDECA_8_FONT_ID);
  lua_setglobal(L, "FONT_LEXENDDECA_8");
  lua_pushinteger(L, LEXENDDECA_9_FONT_ID);
  lua_setglobal(L, "FONT_LEXENDDECA_9");
  lua_pushinteger(L, LEXENDDECA_10_FONT_ID);
  lua_setglobal(L, "FONT_LEXENDDECA_10");
  lua_pushinteger(L, LEXENDDECA_12_FONT_ID);
  lua_setglobal(L, "FONT_LEXENDDECA_12");
  lua_pushinteger(L, LEXENDDECA_14_FONT_ID);
  lua_setglobal(L, "FONT_LEXENDDECA_14");
  lua_pushinteger(L, LEXENDDECA_16_FONT_ID);
  lua_setglobal(L, "FONT_LEXENDDECA_16");
  lua_pushinteger(L, LEXENDDECA_18_FONT_ID);
  lua_setglobal(L, "FONT_LEXENDDECA_18");
  lua_pushinteger(L, LEXENDDECA_20_FONT_ID);
  lua_setglobal(L, "FONT_LEXENDDECA_20");
  lua_pushinteger(L, BITTER_8_FONT_ID);
  lua_setglobal(L, "FONT_BITTER_8");
  lua_pushinteger(L, BITTER_9_FONT_ID);
  lua_setglobal(L, "FONT_BITTER_9");
  lua_pushinteger(L, BITTER_10_FONT_ID);
  lua_setglobal(L, "FONT_BITTER_10");
  lua_pushinteger(L, BITTER_12_FONT_ID);
  lua_setglobal(L, "FONT_BITTER_12");
  lua_pushinteger(L, BITTER_14_FONT_ID);
  lua_setglobal(L, "FONT_BITTER_14");
  lua_pushinteger(L, BITTER_16_FONT_ID);
  lua_setglobal(L, "FONT_BITTER_16");
  lua_pushinteger(L, BITTER_18_FONT_ID);
  lua_setglobal(L, "FONT_BITTER_18");
  lua_pushinteger(L, BITTER_20_FONT_ID);
  lua_setglobal(L, "FONT_BITTER_20");
}

int rebindOsExitToSysExit(lua_State* L) {
  const int loadRc = luaL_loadstring(L, "os.exit = sys.exit");
  if (loadRc != LUA_OK) {
    LOG_ERR("LUA", "rebindOsExitToSysExit: compile failed: %s", lua_tostring(L, -1));
    return loadRc;
  }
  const int callRc = lua_pcall(L, 0, 0, 0);
  if (callRc != LUA_OK) {
    LOG_ERR("LUA", "rebindOsExitToSysExit: pcall failed: %s", lua_tostring(L, -1));
    return callRc;
  }
  return 0;
}

// =========================================================================
//                            LuaManager lifecycle
// =========================================================================

}  // namespace

namespace {

constexpr size_t kMaxLuaScriptSize = 64 * 1024;

}  // namespace

int LuaManager::loadLuaFromStorage(lua_State* L, const std::string& path) {
  HalFile file = Storage.open(path.c_str());
  if (!file) {
    lua_pushfstring(L, "cannot open %s: No such file or directory", path.c_str());
    return LUA_ERRFILE;
  }

  const size_t size = file.size();
  if (size > kMaxLuaScriptSize) {
    file.close();
    lua_pushfstring(L, "cannot load %s: file too large (%u > %u limit)", path.c_str(), static_cast<unsigned>(size),
                    static_cast<unsigned>(kMaxLuaScriptSize));
    return LUA_ERRMEM;
  }

  std::unique_ptr<char[]> buffer(new (std::nothrow) char[size]());
  if (!buffer) {
    file.close();
    lua_pushfstring(L, "out of memory loading %s (%u bytes)", path.c_str(), static_cast<unsigned>(size));
    return LUA_ERRMEM;
  }

  if (size > 0) {
    const int bytesRead = file.read(buffer.get(), size);
    if (bytesRead < 0 || static_cast<size_t>(bytesRead) != size) {
      file.close();
      lua_pushfstring(L, "cannot read %s: io error", path.c_str());
      return LUA_ERRFILE;
    }
  }
  file.close();

  std::string chunkName = std::string("@") + path;
  return luaL_loadbufferx(L, buffer.get(), size, chunkName.c_str(), nullptr);
}

LuaManager::LuaManager() = default;

LuaManager::~LuaManager() { close(); }

bool LuaManager::init(GfxRenderer& renderer, MappedInputManager& input) {
  close();
  L_ = luaL_newstate();
  if (L_ == nullptr) {
    LOG_ERR("LUA", "luaL_newstate failed (likely OOM)");
    return false;
  }
  g_pendingExit.store(-1, std::memory_order_release);
  g_pendingRefresh.store(-1, std::memory_order_release);
  luaL_openlibs(L_);
  registerNetModule(L_);
  registerGuiModule(L_);
  registerInputModule(L_);
  registerSysModule(L_);
  registerConstants(L_);
  if (const int rc = rebindOsExitToSysExit(L_); rc != 0) {
    LOG_ERR("LUA", "init: os.exit rebind failed (%d); tearing down lua_State", rc);
    lua_pop(L_, 1);  // pop leftover error string
    lua_close(L_);
    L_ = nullptr;
    return false;
  }
  // stash pointers into registry for static C bindings to find
  lua_pushlightuserdata(L_, &renderer);
  lua_setfield(L_, LUA_REGISTRYINDEX, "renderer_ptr");
  lua_pushlightuserdata(L_, &input);
  lua_setfield(L_, LUA_REGISTRYINDEX, "input_ptr");
  LOG_DBG("LUA", "Lua 5.4 state ready (full platform bindings)");
  return true;
}

void LuaManager::close() {
  if (L_ != nullptr) {
    lua_close(L_);
    L_ = nullptr;
  }
}

int LuaManager::takePendingExit() {
  const int rc = g_pendingExit.exchange(-1, std::memory_order_acq_rel);
  return rc;
}

int LuaManager::takePendingRefresh() {
  const int rc = g_pendingRefresh.exchange(-1, std::memory_order_acq_rel);
  return rc;
}
