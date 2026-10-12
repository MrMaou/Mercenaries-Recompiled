#include "panel.hpp"

#include <commctrl.h>

#include "text.hpp"

namespace {
enum : int { ID_SERVER = 101, ID_SLOT, ID_PASSWORD, ID_AUTO, ID_CONNECT, ID_STATUS, ID_LOG, ID_CHAT, ID_SEND };
constexpr wchar_t CLASS_NAME[] = L"MercenariesArchipelagoPanel";
constexpr COLORREF BACKGROUND = RGB(20, 24, 28);
constexpr COLORREF FIELD = RGB(30, 36, 42);
constexpr COLORREF TEXT = RGB(230, 236, 238);
constexpr COLORREF MUTED = RGB(150, 166, 172);
constexpr COLORREF ACCENT = RGB(60, 200, 200);
constexpr int CLIENT_WIDTH = 600;
constexpr int CLIENT_HEIGHT = 452;
constexpr size_t MAX_LOG_CHARS = 60000;
constexpr size_t MAX_PENDING_LINES = 400;
}  // namespace

Panel::Panel(HWND owner, UINT hotkey, Callbacks callbacks)
    : owner_(owner), hotkey_(hotkey), callbacks_(std::move(callbacks)) {}

Panel::~Panel() {
  if (window_) DestroyWindow(window_);
  if (font_) DeleteObject(font_);
  if (background_) DeleteObject(background_);
  if (field_background_) DeleteObject(field_background_);
}

bool Panel::visible() const { return window_ && IsWindowVisible(window_); }

void Panel::toggle() {
  if (visible()) hide();
  else show();
}

bool Panel::create() {
  if (window_) return true;
  HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW cls{};
  cls.cbSize = sizeof(cls);
  cls.lpfnWndProc = window_proc;
  cls.hInstance = instance;
  cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
  cls.lpszClassName = CLASS_NAME;
  RegisterClassExW(&cls);  // fails harmlessly if already registered

  dpi_ = int(GetDpiForWindow(owner_));
  if (dpi_ <= 0) dpi_ = 96;
  background_ = CreateSolidBrush(BACKGROUND);
  field_background_ = CreateSolidBrush(FIELD);
  font_ = CreateFontW(-MulDiv(10, dpi_, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                      CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

  DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
  RECT rect{0, 0, scale(CLIENT_WIDTH), scale(CLIENT_HEIGHT)};
  AdjustWindowRectExForDpi(&rect, style, FALSE, WS_EX_TOOLWINDOW, UINT(dpi_));
  window_ = CreateWindowExW(WS_EX_TOOLWINDOW, CLASS_NAME, L"Archipelago", style, CW_USEDEFAULT, CW_USEDEFAULT,
                            rect.right - rect.left, rect.bottom - rect.top, owner_, nullptr, instance, this);
  if (!window_) return false;

  auto make = [&](const wchar_t *cls_name, const wchar_t *text, DWORD extra, int id, DWORD ex = 0) {
    HWND control = CreateWindowExW(ex, cls_name, text, WS_CHILD | WS_VISIBLE | extra, 0, 0, 1, 1, window_,
                                   HMENU(INT_PTR(id)), instance, nullptr);
    SendMessageW(control, WM_SETFONT, WPARAM(font_), TRUE);
    return control;
  };
  make(L"STATIC", L"Server", SS_LEFT, 0);
  make(L"STATIC", L"Slot", SS_LEFT, 0);
  make(L"STATIC", L"Password", SS_LEFT, 0);
  server_ = make(L"EDIT", server_text_.c_str(), WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER, ID_SERVER);
  slot_ = make(L"EDIT", slot_text_.c_str(), WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER, ID_SLOT);
  password_ = make(L"EDIT", password_text_.c_str(), WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD | WS_BORDER, ID_PASSWORD);
  auto_connect_ = make(L"BUTTON", L"Connect when the game starts", WS_TABSTOP | BS_AUTOCHECKBOX, ID_AUTO);
  connect_ = make(L"BUTTON", L"Connect", WS_TABSTOP | BS_PUSHBUTTON, ID_CONNECT);
  status_ = make(L"STATIC", status_text_.c_str(), SS_LEFT | SS_ENDELLIPSIS, ID_STATUS);
  log_ = make(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER, ID_LOG);
  chat_ = make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER, ID_CHAT);
  send_ = make(L"BUTTON", L"Send", WS_TABSTOP | BS_PUSHBUTTON, ID_SEND);
  SendMessageW(log_, EM_SETLIMITTEXT, 0, 0);
  SendMessageW(auto_connect_, BM_SETCHECK, auto_connect_value_ ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessageW(chat_, EM_SETCUEBANNER, TRUE, LPARAM(L"Chat or !command"));
  for (HWND field : {server_, slot_, password_, chat_, auto_connect_, connect_, send_})
    SetWindowSubclass(field, field_proc, 1, DWORD_PTR(this));
  layout();
  set_connection(active_, narrow(status_text_));
  for (const auto &line : pending_log_) append_log(narrow(line));
  pending_log_.clear();
  return true;
}

void Panel::layout() {
  HWND labels[3] = {};
  int count = 0;
  for (HWND child = GetWindow(window_, GW_CHILD); child && count < 3; child = GetWindow(child, GW_HWNDNEXT))
    if (GetDlgCtrlID(child) == 0) labels[count++] = child;
  auto place = [&](HWND control, int x, int y, int w, int h) {
    MoveWindow(control, scale(x), scale(y), scale(w), scale(h), FALSE);
  };
  place(labels[0], 14, 17, 70, 20);
  place(server_, 86, 14, 300, 24);
  place(labels[1], 400, 17, 40, 20);
  place(slot_, 440, 14, 146, 24);
  place(labels[2], 14, 49, 70, 20);
  place(password_, 86, 46, 180, 24);
  place(auto_connect_, 280, 48, 200, 22);
  place(connect_, 486, 45, 100, 26);
  place(status_, 14, 82, 572, 20);
  place(log_, 14, 108, 572, 298);
  place(chat_, 14, 416, 486, 24);
  place(send_, 506, 415, 80, 26);
}

void Panel::show() {
  if (!create()) return;
  RECT owner_rect{}, rect{};
  GetWindowRect(owner_, &owner_rect);
  GetWindowRect(window_, &rect);
  int w = rect.right - rect.left, h = rect.bottom - rect.top;
  SetWindowPos(window_, HWND_TOP, owner_rect.left + (owner_rect.right - owner_rect.left - w) / 2,
               owner_rect.top + (owner_rect.bottom - owner_rect.top - h) / 2, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
  SetForegroundWindow(window_);
  HWND focus = GetWindowTextLengthW(server_) == 0 ? server_ : GetWindowTextLengthW(slot_) == 0 ? slot_ : chat_;
  if (!active_ && focus == chat_) focus = connect_;
  SetFocus(focus);
}

void Panel::hide() {
  if (!visible()) return;
  ShowWindow(window_, SW_HIDE);
  SetForegroundWindow(owner_);
}

void Panel::set_fields(const std::string &server, const std::string &slot, const std::string &password,
                       bool auto_connect) {
  server_text_ = widen(server);
  slot_text_ = widen(slot);
  password_text_ = widen(password);
  auto_connect_value_ = auto_connect;
  if (!window_) return;
  SetWindowTextW(server_, server_text_.c_str());
  SetWindowTextW(slot_, slot_text_.c_str());
  SetWindowTextW(password_, password_text_.c_str());
  SendMessageW(auto_connect_, BM_SETCHECK, auto_connect ? BST_CHECKED : BST_UNCHECKED, 0);
}

void Panel::set_connection(bool active, const std::string &status) {
  std::wstring text = widen(status);
  bool changed = active != active_ || text != status_text_;
  active_ = active;
  status_text_ = std::move(text);
  if (!window_ || !changed) return;
  SetWindowTextW(status_, status_text_.c_str());
  SetWindowTextW(connect_, active_ ? L"Disconnect" : L"Connect");
  for (HWND field : {server_, slot_, password_}) EnableWindow(field, !active_);
  EnableWindow(chat_, active_);
  EnableWindow(send_, active_);
}

void Panel::append_log(const std::string &line) {
  std::wstring text = widen(line);
  if (!window_) {
    pending_log_.push_back(std::move(text));
    while (pending_log_.size() > MAX_PENDING_LINES) pending_log_.pop_front();
    return;
  }
  int length = GetWindowTextLengthW(log_);
  if (size_t(length) > MAX_LOG_CHARS) {
    // Drop the oldest third, at a line boundary.
    int cut = int(SendMessageW(log_, EM_LINEINDEX, SendMessageW(log_, EM_LINEFROMCHAR, length / 3, 0) + 1, 0));
    SendMessageW(log_, EM_SETSEL, 0, cut > 0 ? cut : length / 3);
    SendMessageW(log_, EM_REPLACESEL, FALSE, LPARAM(L""));
    length = GetWindowTextLengthW(log_);
  }
  text = (length ? L"\r\n" : L"") + text;
  SendMessageW(log_, EM_SETSEL, length, length);
  SendMessageW(log_, EM_REPLACESEL, FALSE, LPARAM(text.c_str()));
  SendMessageW(log_, EM_SCROLLCARET, 0, 0);
}

std::wstring Panel::text_of(HWND control) const {
  int length = GetWindowTextLengthW(control);
  std::wstring text(size_t(length) + 1, L'\0');
  GetWindowTextW(control, text.data(), length + 1);
  text.resize(size_t(length));
  return text;
}

void Panel::on_connect_button() {
  if (active_) {
    if (callbacks_.disconnect) callbacks_.disconnect();
    return;
  }
  bool auto_connect = SendMessageW(auto_connect_, BM_GETCHECK, 0, 0) == BST_CHECKED;
  server_text_ = text_of(server_);
  slot_text_ = text_of(slot_);
  password_text_ = text_of(password_);
  auto_connect_value_ = auto_connect;
  if (callbacks_.connect) callbacks_.connect(narrow(server_text_), narrow(slot_text_), narrow(password_text_), auto_connect);
}

void Panel::on_send() {
  std::wstring text = text_of(chat_);
  if (text.empty() || !active_) return;
  SetWindowTextW(chat_, L"");
  if (callbacks_.say) callbacks_.say(narrow(text));
}

LRESULT CALLBACK Panel::field_proc(HWND control, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR,
                                   DWORD_PTR data) {
  auto *self = reinterpret_cast<Panel *>(data);
  if (message == WM_KEYDOWN) {
    if (wparam == VK_RETURN) {
      if (control == self->chat_) self->on_send();
      else if (control != self->auto_connect_) self->on_connect_button();
      return 0;
    }
    if (wparam == VK_TAB) {
      bool back = GetKeyState(VK_SHIFT) < 0;
      if (HWND next = GetNextDlgTabItem(self->window_, control, back)) SetFocus(next);
      return 0;
    }
    if (wparam == VK_ESCAPE || wparam == self->hotkey_) {
      self->hide();
      return 0;
    }
  }
  if (message == WM_CHAR && (wparam == L'\r' || wparam == L'\t' || wparam == 27)) return 0;  // no beep
  return DefSubclassProc(control, message, wparam, lparam);
}

LRESULT CALLBACK Panel::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto *self = reinterpret_cast<Panel *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  switch (message) {
    case WM_NCCREATE: {
      auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
      SetWindowLongPtrW(window, GWLP_USERDATA, LONG_PTR(create->lpCreateParams));
      break;
    }
    case WM_COMMAND:
      if (!self) break;
      if (LOWORD(wparam) == ID_CONNECT && HIWORD(wparam) == BN_CLICKED) self->on_connect_button();
      if (LOWORD(wparam) == ID_SEND && HIWORD(wparam) == BN_CLICKED) self->on_send();
      return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN: {
      if (!self) break;
      HDC dc = HDC(wparam);
      HWND control = HWND(lparam);
      bool field = message == WM_CTLCOLOREDIT || control == self->log_;
      SetTextColor(dc, control == self->status_ ? ACCENT : IsWindowEnabled(control) ? TEXT : MUTED);
      SetBkColor(dc, field ? FIELD : BACKGROUND);
      return LRESULT(field ? self->field_background_ : self->background_);
    }
    case WM_ERASEBKGND:
      if (!self) break;
      {
        RECT rect;
        GetClientRect(window, &rect);
        FillRect(HDC(wparam), &rect, self->background_);
      }
      return 1;
    case WM_KEYDOWN:
      if (self && (wparam == VK_ESCAPE || wparam == self->hotkey_)) {
        self->hide();
        return 0;
      }
      break;
    case WM_CLOSE:
      if (self) self->hide();
      return 0;
    case WM_DPICHANGED:
      if (self) {
        self->dpi_ = HIWORD(wparam);
        auto *suggested = reinterpret_cast<RECT *>(lparam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        self->layout();
        InvalidateRect(window, nullptr, TRUE);
      }
      return 0;
    default:
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}
