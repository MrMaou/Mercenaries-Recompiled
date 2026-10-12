// Archipelago plugin for Mercenaries Recompiled (plugin interface 2).
//
// - In-game client: APClientPP session + game logic (ap_session, game_logic).
// - F7 panel (panel) and in-frame notices (toasts).
// - Optional local socket bridge for the Python client and the console tool
//   (socket_bridge); it refuses changes while the in-game client is active.
#define MERC_PLUGIN_IMPLEMENTATION
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../../../sdk/mercenaries_plugin.h"
#include "ap_session.hpp"
#include "game_logic.hpp"
#include "panel.hpp"
#include "socket_bridge.h"
#include "text.hpp"
#include "toasts.hpp"

namespace {

constexpr size_t LUA_RESULT_CAPACITY = 256 * 1024;

class HostGameIO : public ap::GameIO {
 public:
  explicit HostGameIO(const MercPluginHost2 *host) : host_(host), buffer_(LUA_RESULT_CAPACITY) {}

  ap::LuaResult lua(const std::string &source) override {
    size_t length = 0;
    int status = host_->run_lua(host_->v1.context, source.data(), source.size(), buffer_.data(), buffer_.size(), &length);
    if (length >= buffer_.size()) length = buffer_.size() - 1;
    return {ap::LuaStatus(status), std::string(buffer_.data(), length)};
  }

  bool read(uint32_t address, void *out, size_t size) override {
    return host_->v1.read_guest(host_->v1.context, address, out, size) != 0;
  }

 private:
  const MercPluginHost2 *host_;
  std::vector<char> buffer_;
};

struct Settings {
  std::wstring path;
  std::string server, slot, password;
  bool auto_connect = false;
  bool hud_messages = false;
  bool toasts = true;
  UINT hotkey = VK_F7;
  bool bridge = true;
  unsigned port = 28012, budget_ms = 4, per_update = 64;
  uint32_t bounty[4] = {};
  unsigned bounty_size = 4;

  std::wstring get(const wchar_t *section, const wchar_t *key, const wchar_t *fallback = L"") const {
    wchar_t value[1024];
    GetPrivateProfileStringW(section, key, fallback, value, 1024, path.c_str());
    return value;
  }
  unsigned get_int(const wchar_t *section, const wchar_t *key, unsigned fallback) const {
    return GetPrivateProfileIntW(section, key, int(fallback), path.c_str());
  }
  uint32_t get_address(const wchar_t *section, const wchar_t *key) const {
    std::wstring value = get(section, key);
    return value.empty() ? 0 : uint32_t(wcstoul(value.c_str(), nullptr, 0));
  }

  void load(const std::string &directory) {
    path = widen(directory) + L"\\archipelago.ini";
    server = narrow(get(L"Client", L"Server", L"archipelago.gg:38281"));
    slot = narrow(get(L"Client", L"Slot"));
    password = narrow(get(L"Client", L"Password"));
    auto_connect = get_int(L"Client", L"AutoConnect", 0) != 0;
    hud_messages = get_int(L"Client", L"HudMessages", 0) != 0;
    toasts = get_int(L"Client", L"Toasts", 1) != 0;
    std::wstring key = get(L"Client", L"Hotkey", L"F7");
    if ((key[0] == L'F' || key[0] == L'f') && key.size() > 1) {
      unsigned n = unsigned(_wtoi(key.c_str() + 1));
      if (n >= 1 && n <= 24) hotkey = VK_F1 + n - 1;
    } else if (!key.empty()) {
      hotkey = UINT(wcstoul(key.c_str(), nullptr, 0));
    }
    bridge = get_int(L"Bridge", L"Enabled", 1) != 0;
    port = get_int(L"Bridge", L"Port", 28012);
    budget_ms = get_int(L"Bridge", L"UpdateBudgetMs", 4);
    per_update = get_int(L"Bridge", L"MaxRequestsPerUpdate", 64);
    const wchar_t *bounty_keys[4] = {L"ListeningPost", L"Blueprint", L"Monument", L"Treasure"};
    for (int i = 0; i < 4; ++i) bounty[i] = get_address(L"Bounties", bounty_keys[i]);
    bounty_size = get_int(L"Bounties", L"Size", 4);
  }

  void save_connection() const {
    WritePrivateProfileStringW(L"Client", L"Server", widen(server).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Client", L"Slot", widen(slot).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Client", L"Password", widen(password).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Client", L"AutoConnect", auto_connect ? L"1" : L"0", path.c_str());
  }
};

std::string clock_prefix() {
  std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &now);
  char text[16];
  std::strftime(text, sizeof(text), "[%H:%M] ", &local);
  return text;
}

double seconds_now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct Plugin {
  explicit Plugin(const MercPluginHost2 *plugin_host)
      : host(plugin_host),
        io(plugin_host),
        logic(io, [this](const std::string &text) { message(ap::MessageKind::Log, text); }),
        session(logic, [this](ap::MessageKind kind, const std::string &text) { message(kind, text); }) {}

  void message(ap::MessageKind kind, const std::string &text) {
    if (panel) panel->append_log(clock_prefix() + text);
    if (kind != ap::MessageKind::Log && settings.toasts) toasts.add(text, kind == ap::MessageKind::Error);
    if (kind == ap::MessageKind::Error) host->v1.log(host->v1.context, text.c_str());
  }

  void connect() {
    ap::SessionConfig config;
    config.server = settings.server;
    config.slot = settings.slot;
    config.password = settings.password;
    config.uuid_file = std::string(host->v1.settings_directory) + "\\uuid";
    std::wstring certs = widen(host->v1.plugin_directory) + L"\\cacert.pem";
    if (GetFileAttributesW(certs.c_str()) != INVALID_FILE_ATTRIBUTES) config.cert_store = narrow(certs);
    session.connect(config);
  }

  void update() {
    auto actions = std::move(pending);
    pending.clear();
    for (auto &action : actions) action();
    if (first_update) {
      first_update = false;
      if (settings.auto_connect && !settings.server.empty() && !settings.slot.empty()) connect();
    }
    session.poll(seconds_now());
    if (settings.bridge) archipelago_bridge_update(!session.active());
    if (panel) panel->set_connection(session.active(), session.status());
  }

  const MercPluginHost2 *host;
  Settings settings;
  HostGameIO io;
  ap::GameLogic logic;
  ap::Session session;
  Toasts toasts;
  std::unique_ptr<Panel> panel;
  std::vector<std::function<void()>> pending;  // panel actions, run in on_update
  bool first_update = true;
};

Plugin *plugin_state;

void report(const std::exception &e, const char *where) {
  if (!plugin_state) return;
  std::string text = std::string("Archipelago ") + where + " error: " + e.what();
  plugin_state->host->v1.log(plugin_state->host->v1.context, text.c_str());
}

int MERC_PLUGIN_CALL on_update(void *, const MercPluginUpdate *) {
  try {
    plugin_state->update();
  } catch (const std::exception &e) {
    report(e, "update");
    try {
      plugin_state->session.disconnect();
    } catch (...) {
    }
  } catch (...) {
    plugin_state->host->v1.log(plugin_state->host->v1.context, "Archipelago update: unknown error.");
  }
  return 1;
}

void MERC_PLUGIN_CALL on_hotkey(void *) {
  try {
    if (plugin_state && plugin_state->panel) plugin_state->panel->toggle();
  } catch (const std::exception &e) {
    report(e, "panel");
  }
}

void MERC_PLUGIN_CALL on_paint(void *) {
  try {
    if (plugin_state) plugin_state->toasts.paint(plugin_state->host);
  } catch (const std::exception &e) {
    report(e, "overlay");
  }
}

void MERC_PLUGIN_CALL on_shutdown(void *) {
  if (!plugin_state) return;
  try {
    plugin_state->session.disconnect();
  } catch (...) {
  }
  if (plugin_state->settings.bridge) archipelago_bridge_stop();
  delete plugin_state;
  plugin_state = nullptr;
}

}  // namespace

MERC_PLUGIN_EXPORT int MERC_PLUGIN_CALL MercenariesPlugin_Init(const MercPluginHost *base, MercPlugin *plugin,
                                                              char *error, uint32_t capacity) {
  auto fail = [&](const char *text) {
    if (error && capacity) std::snprintf(error, capacity, "%s", text);
    return 0;
  };
  if (!base || base->api_version != MERCENARIES_PLUGIN_API_VERSION_2 || base->struct_size < sizeof(MercPluginHost2) ||
      !plugin || plugin->struct_size < sizeof(MercPlugin))
    return fail("This plugin needs a Mercenaries Recompiled build with plugin interface 2.");
  const auto *host = reinterpret_cast<const MercPluginHost2 *>(base);
  if (!host->run_lua || !host->window || !host->register_hotkey || !host->set_overlay || !host->draw_overlay)
    return fail("The host does not provide the plugin interface 2 UI functions.");

  try {
    plugin_state = new Plugin(host);
    Plugin &state = *plugin_state;
    plugin->userdata = nullptr;
    plugin->on_update = on_update;
    plugin->on_shutdown = on_shutdown;

    state.settings.load(host->v1.settings_directory);
    state.logic.hud_messages = state.settings.hud_messages;
    for (int i = 0; i < 4; ++i)
      if (state.settings.bounty[i]) state.logic.set_bounty_address(i, state.settings.bounty[i], state.settings.bounty_size);

    Panel::Callbacks callbacks;
    callbacks.connect = [&state](const std::string &server, const std::string &slot, const std::string &password,
                                 bool auto_connect) {
      state.pending.push_back([&state, server, slot, password, auto_connect] {
        state.settings.server = server;
        state.settings.slot = slot;
        state.settings.password = password;
        state.settings.auto_connect = auto_connect;
        state.settings.save_connection();
        state.connect();
      });
    };
    callbacks.disconnect = [&state] { state.pending.push_back([&state] { state.session.disconnect(); }); };
    callbacks.say = [&state](const std::string &text) {
      state.pending.push_back([&state, text] {
        if (!state.session.say(text)) state.message(ap::MessageKind::Error, "Chat is only available while connected.");
      });
    };
    state.panel = std::make_unique<Panel>(static_cast<HWND>(host->window), state.settings.hotkey, callbacks);
    state.panel->set_fields(state.settings.server, state.settings.slot, state.settings.password,
                            state.settings.auto_connect);

    if (!host->register_hotkey(host->v1.context, state.settings.hotkey, on_hotkey, nullptr))
      host->v1.log(host->v1.context, "Could not register the panel hotkey; change Hotkey in archipelago.ini.");
    host->set_overlay(host->v1.context, on_paint, nullptr);

    if (state.settings.bridge) {
      char bridge_error[256] = {};
      if (!archipelago_bridge_start(host, static_cast<unsigned short>(state.settings.port), state.settings.budget_ms,
                                    state.settings.per_update, bridge_error, sizeof(bridge_error))) {
        host->v1.log(host->v1.context, bridge_error);
        state.settings.bridge = false;  // the in-game client still works
      }
    }
    char text[96];
    std::snprintf(text, sizeof(text), "Archipelago ready; press F%u for the panel.",
                  state.settings.hotkey >= VK_F1 && state.settings.hotkey <= VK_F24 ? state.settings.hotkey - VK_F1 + 1 : 0);
    host->v1.log(host->v1.context, text);
    state.message(ap::MessageKind::Log, text);
    return 1;
  } catch (const std::exception &e) {
    return fail(e.what());
  }
}
