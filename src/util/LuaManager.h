#pragma once

#include <GfxRenderer.h>
#include <MappedInputManager.h>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <cstdint>
#include <string>

class LuaManager {
 public:
  LuaManager();
  ~LuaManager();

  LuaManager(const LuaManager&) = delete;
  LuaManager& operator=(const LuaManager&) = delete;

  bool init(GfxRenderer& renderer, MappedInputManager& input);

  void close();
  bool isInitialized() const { return L_ != nullptr; }

  int takePendingExit();

  int takePendingRefresh();

  lua_State* L() const { return L_; }

  static int loadLuaFromStorage(lua_State* L, const std::string& path);

 private:
  lua_State* L_ = nullptr;
};
