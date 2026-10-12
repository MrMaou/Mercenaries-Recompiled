#include "toasts.hpp"

#include <algorithm>
#include <cstring>

#include "../../../sdk/mercenaries_plugin.h"
#include "text.hpp"

namespace {
// Fixed canvas: the host shares one overlay texture and resizes it to each
// image, so a constant size avoids resizing on our side.
constexpr int WIDTH = 420;
constexpr int ROW = 28;
constexpr int GAP = 4;
constexpr int MAX_VISIBLE = 5;
constexpr int HEIGHT = MAX_VISIBLE * (ROW + GAP);
constexpr int X = 960 - WIDTH - 12;
constexpr int Y = 12;
constexpr ULONGLONG LIFE_MS = 7000;
constexpr ULONGLONG FADE_MS = 800;
constexpr size_t MAX_QUEUED = 24;

struct Rgb {
  float r, g, b;
};
constexpr Rgb BACKGROUND{18, 22, 26};
constexpr float BACKGROUND_ALPHA = 0.82f;
constexpr Rgb NOTICE_ACCENT{60, 200, 200};
constexpr Rgb ERROR_ACCENT{240, 120, 60};
constexpr Rgb NOTICE_TEXT{236, 240, 242};
constexpr Rgb ERROR_TEXT{255, 214, 190};

uint32_t premultiplied(Rgb color, float alpha) {
  auto channel = [&](float value) { return uint32_t(std::clamp(value * alpha, 0.0f, 255.0f) + 0.5f); };
  return channel(color.b) | channel(color.g) << 8 | channel(color.r) << 16 |
         uint32_t(std::clamp(alpha * 255.0f, 0.0f, 255.0f) + 0.5f) << 24;
}
}  // namespace

Toasts::~Toasts() {
  if (dc_) DeleteDC(dc_);
  if (bitmap_) DeleteObject(bitmap_);
  if (font_) DeleteObject(font_);
}

void Toasts::add(const std::string &utf8_text, bool error) {
  std::wstring text = widen(utf8_text);
  for (auto &c : text)
    if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
  if (text.empty()) return;
  toasts_.push_back({std::move(text), error, 0});
  while (toasts_.size() > MAX_QUEUED) toasts_.pop_front();  // a burst: keep the newest
  dirty_ = true;
}

bool Toasts::ensure_canvas() {
  if (dc_) return pixels_ != nullptr;
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = WIDTH;
  info.bmiHeader.biHeight = -HEIGHT;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  dc_ = CreateCompatibleDC(nullptr);
  void *bits = nullptr;
  bitmap_ = dc_ ? CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
  if (!bitmap_ || !bits) return false;
  pixels_ = static_cast<uint32_t *>(bits);
  SelectObject(dc_, bitmap_);
  font_ = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                      ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  SelectObject(dc_, font_);
  SetBkMode(dc_, TRANSPARENT);
  SetTextColor(dc_, RGB(255, 255, 255));
  composed_.assign(size_t(WIDTH) * HEIGHT, 0);
  return true;
}

void Toasts::render(ULONGLONG now) {
  // GDI draws white text on black; its coverage becomes the text alpha.
  std::memset(pixels_, 0, size_t(WIDTH) * HEIGHT * 4);
  size_t visible = std::min<size_t>(toasts_.size(), MAX_VISIBLE);
  for (size_t i = 0; i < visible; ++i) {
    RECT rect{14, LONG(i * (ROW + GAP)), WIDTH - 8, LONG(i * (ROW + GAP) + ROW)};
    DrawTextW(dc_, toasts_[i].text.c_str(), int(toasts_[i].text.size()), &rect,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
  }
  GdiFlush();
  std::fill(composed_.begin(), composed_.end(), 0u);
  for (size_t i = 0; i < visible; ++i) {
    const Toast &toast = toasts_[i];
    ULONGLONG age = now - toast.shown;
    float fade = age + FADE_MS > LIFE_MS ? float(LIFE_MS - std::min(age, LIFE_MS)) / FADE_MS : 1.0f;
    Rgb text = toast.error ? ERROR_TEXT : NOTICE_TEXT;
    uint32_t accent = premultiplied(toast.error ? ERROR_ACCENT : NOTICE_ACCENT, fade);
    float base_alpha = BACKGROUND_ALPHA * fade;
    for (int y = int(i) * (ROW + GAP); y < int(i) * (ROW + GAP) + ROW; ++y) {
      for (int x = 0; x < WIDTH; ++x) {
        size_t index = size_t(y) * WIDTH + size_t(x);
        if (x < 4) {
          composed_[index] = accent;
          continue;
        }
        uint32_t gdi = pixels_[index] & 0xFFFFFF;
        float coverage = float(std::max({gdi & 0xFF, (gdi >> 8) & 0xFF, gdi >> 16})) / 255.0f * fade;
        // Text over the translucent background, premultiplied.
        float alpha = coverage + base_alpha * (1.0f - coverage);
        float r = text.r * coverage + BACKGROUND.r * base_alpha * (1.0f - coverage);
        float g = text.g * coverage + BACKGROUND.g * base_alpha * (1.0f - coverage);
        float b = text.b * coverage + BACKGROUND.b * base_alpha * (1.0f - coverage);
        composed_[index] = uint32_t(b + 0.5f) | uint32_t(g + 0.5f) << 8 | uint32_t(r + 0.5f) << 16 |
                           uint32_t(alpha * 255.0f + 0.5f) << 24;
      }
    }
  }
  rendered_ = now;
  dirty_ = false;
}

void Toasts::paint(const MercPluginHost2 *host) {
  if (toasts_.empty()) return;
  ULONGLONG now = GetTickCount64();
  while (!toasts_.empty() && toasts_.front().shown && now - toasts_.front().shown >= LIFE_MS) {
    toasts_.pop_front();
    dirty_ = true;
  }
  if (toasts_.empty()) return;  // nothing visible: skip the draw entirely
  bool fading = false;
  size_t visible = std::min<size_t>(toasts_.size(), MAX_VISIBLE);
  for (size_t i = 0; i < visible; ++i) {
    if (!toasts_[i].shown) {
      toasts_[i].shown = now;
      dirty_ = true;
    }
    if (now - toasts_[i].shown + FADE_MS > LIFE_MS) fading = true;
  }
  if (!ensure_canvas()) return;
  if (dirty_ || (fading && now - rendered_ >= 33)) render(now);
  host->draw_overlay(host->v1.context, composed_.data(), WIDTH, HEIGHT, X, Y);
}
