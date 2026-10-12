// Short Archipelago notices drawn into the game frame (top-right corner).
#pragma once
#include <windows.h>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

struct MercPluginHost2;

class Toasts {
 public:
  ~Toasts();
  void add(const std::string &utf8_text, bool error);
  void clear() { toasts_.clear(); }
  // From the host's overlay paint callback.
  void paint(const MercPluginHost2 *host);

 private:
  struct Toast {
    std::wstring text;
    bool error;
    ULONGLONG shown;  // 0 until first painted
  };
  bool ensure_canvas();
  void render(ULONGLONG now);

  std::deque<Toast> toasts_;
  HDC dc_ = nullptr;
  HBITMAP bitmap_ = nullptr;
  HFONT font_ = nullptr;
  uint32_t *pixels_ = nullptr;
  std::vector<uint32_t> composed_;
  ULONGLONG rendered_ = 0;
  bool dirty_ = true;
};
