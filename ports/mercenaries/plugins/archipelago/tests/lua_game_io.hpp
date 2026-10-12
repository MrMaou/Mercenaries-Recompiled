// GameIO backed by a stock Lua 5.0.3 VM and a fake guest memory map, for tests
// that run on a desktop OS without the game.
#pragma once
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include "../src/game_io.hpp"

class LuaGameIO : public ap::GameIO {
 public:
  explicit LuaGameIO(const std::string &stubs_path) : L(lua_open()) {
    luaopen_base(L);
    luaopen_table(L);
    luaopen_string(L);
    luaopen_math(L);
    if (luaL_loadfile(L, stubs_path.c_str()) || lua_pcall(L, 0, 0, 0))
      throw std::runtime_error(std::string("stubs: ") + lua_tostring(L, -1));
    lua_settop(L, 0);
  }
  ~LuaGameIO() override { lua_close(L); }

  bool ready = true;                     // false: Lua requests answer NotReady
  std::map<uint32_t, uint8_t> memory;    // sparse guest RAM, zero elsewhere
  int lua_calls = 0;

  void poke32(uint32_t address, uint32_t value) {
    for (int i = 0; i < 4; ++i) memory[address + i] = uint8_t(value >> (8 * i));
  }

  ap::LuaResult lua(const std::string &source) override {
    ++lua_calls;
    if (!ready) return {ap::LuaStatus::NotReady, ""};
    int top = lua_gettop(L);
    int status = luaL_loadbuffer(L, source.data(), source.size(), "plugin");
    if (!status) status = lua_pcall(L, 0, 1, 0);
    ap::LuaResult result{status ? ap::LuaStatus::Error : ap::LuaStatus::Ok, ""};
    if (lua_gettop(L) > top) {
      switch (lua_type(L, -1)) {
        case LUA_TNIL: break;
        case LUA_TBOOLEAN: result.text = lua_toboolean(L, -1) ? "true" : "false"; break;
        case LUA_TNUMBER: result.text = ap::lua_number(lua_tonumber(L, -1)); break;
        case LUA_TSTRING: result.text.assign(lua_tostring(L, -1), lua_strlen(L, -1)); break;
        default: result.status = ap::LuaStatus::BadResult;
      }
    }
    lua_settop(L, top);
    return result;
  }

  bool read(uint32_t address, void *out, size_t size) override {
    auto *bytes = static_cast<uint8_t *>(out);
    for (size_t i = 0; i < size; ++i) {
      auto found = memory.find(address + uint32_t(i));
      bytes[i] = found == memory.end() ? 0 : found->second;
    }
    return true;
  }

  // Convenience for assertions.
  std::string eval(const std::string &expression) { return lua("return " + expression).text; }

 private:
  lua_State *L;
};
