// What the client logic needs from the game. The plugin implements it with the
// host's run_lua/read_guest; tests implement it with a stock Lua 5.0 VM.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace ap {

// Status codes match MERC_LUA_* in mercenaries_plugin.h.
enum class LuaStatus { Ok = 0, Error = 1, NotReady = 2, Rejected = 3, NoMemory = 4, BadResult = 5 };

struct LuaResult {
  LuaStatus status;
  std::string text;  // first return value, or the error message
};

class GameIO {
 public:
  virtual ~GameIO() = default;
  virtual LuaResult lua(const std::string &source) = 0;
  virtual bool read(uint32_t address, void *out, size_t size) = 0;
};

// The game cannot be changed right now (menus, loading, cutscenes, a script
// error). The caller retries later; nothing is lost.
class GameError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Lua 5.0 string literal for arbitrary bytes.
inline std::string lua_quote(const std::string &text) {
  std::string out = "\"";
  for (unsigned char c : text) {
    if (c == '\\' || c == '"') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c >= 32 && c < 127) {
      out += static_cast<char>(c);
    } else {
      char escape[5];
      std::snprintf(escape, sizeof(escape), "\\%03u", c);  // padded so a following digit is not absorbed
      out += escape;
    }
  }
  out += '"';
  return out;
}

inline std::string lua_number(double value) {
  char text[40];
  std::snprintf(text, sizeof(text), "%.17g", value);
  return text;
}

}  // namespace ap
