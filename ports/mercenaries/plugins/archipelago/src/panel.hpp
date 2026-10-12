// The in-game Archipelago panel: connection fields, status, log and chat.
// A Win32 popup owned by the game window, opened with the plugin hotkey.
#pragma once
#include <windows.h>

#include <deque>
#include <functional>
#include <string>

class Panel {
 public:
  struct Callbacks {
    std::function<void(const std::string &server, const std::string &slot, const std::string &password,
                       bool auto_connect)>
        connect;
    std::function<void()> disconnect;
    std::function<void(const std::string &text)> say;
  };

  Panel(HWND owner, UINT hotkey, Callbacks callbacks);
  ~Panel();
  Panel(const Panel &) = delete;
  Panel &operator=(const Panel &) = delete;

  void toggle();
  void show();
  void hide();
  bool visible() const;

  void set_fields(const std::string &server, const std::string &slot, const std::string &password, bool auto_connect);
  void set_connection(bool active, const std::string &status);
  void append_log(const std::string &line);

 private:
  bool create();
  void layout();
  void on_connect_button();
  void on_send();
  int scale(int value) const { return MulDiv(value, dpi_, 96); }
  std::wstring text_of(HWND control) const;
  static LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK field_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

  HWND owner_;
  UINT hotkey_;
  Callbacks callbacks_;
  HWND window_ = nullptr;
  HWND server_ = nullptr, slot_ = nullptr, password_ = nullptr, auto_connect_ = nullptr;
  HWND connect_ = nullptr, status_ = nullptr, log_ = nullptr, chat_ = nullptr, send_ = nullptr;
  HFONT font_ = nullptr;
  HBRUSH background_ = nullptr, field_background_ = nullptr;
  int dpi_ = 96;
  bool active_ = false;
  std::wstring status_text_ = L"Not connected";
  std::wstring server_text_, slot_text_, password_text_;
  bool auto_connect_value_ = false;
  std::deque<std::wstring> pending_log_;
};
