// UTF-8 <-> UTF-16 for Win32 calls. The rest of the plugin uses UTF-8.
#pragma once
#include <windows.h>

#include <string>

inline std::wstring widen(const std::string &text) {
  if (text.empty()) return {};
  int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
  std::wstring out(size_t(length > 0 ? length : 0), L'\0');
  if (length > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), length);
  return out;
}

inline std::string narrow(const std::wstring &text) {
  if (text.empty()) return {};
  int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size_t(length > 0 ? length : 0), '\0');
  if (length > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), length, nullptr, nullptr);
  return out;
}
