// Archipelago server session for the in-game client: APClientPP plus the sync
// loop of mercs-ap MercenariesClient.py. Runs on the game thread; call poll()
// from every on_update.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "game_logic.hpp"

class APClient;

namespace ap {

struct SessionConfig {
  std::string server;     // host:port, ws://..., or wss://...
  std::string slot;
  std::string password;
  std::string uuid_file;  // persistent client UUID
  std::string cert_store; // CA bundle for wss; empty uses the system store
};

enum class MessageKind {
  Log,    // panel log only
  Notice, // panel log and an on-screen toast
  Error,  // panel log and an on-screen toast, highlighted
};

class Session {
 public:
  using Message = std::function<void(MessageKind, const std::string &)>;

  Session(GameLogic &logic, Message message);
  ~Session();
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;

  void connect(const SessionConfig &config);
  void disconnect();
  void poll(double now_seconds);
  bool say(const std::string &text);

  bool active() const { return client_ != nullptr; }
  bool slot_connected() const { return slot_connected_; }
  std::string status() const;
  std::pair<size_t, size_t> location_progress() const;  // checked, total

 private:
  void install_handlers();
  void handle_slot_connected(SlotOptions options);
  void handle_sent(Counter sent);
  void handle_print(const std::string &text, bool toast, const std::string &hud_text);
  void sync();
  std::string sent_key() const;

  GameLogic &logic_;
  Message message_;
  std::unique_ptr<APClient> client_;
  SessionConfig config_;
  SlotOptions options_;
  std::vector<int64_t> received_;
  Counter sent_;
  bool sent_loaded_ = false;
  bool slot_connected_ = false;
  bool goal_sent_ = false;
  bool drop_client_ = false;
  std::string state_text_ = "Not connected";
  std::set<std::pair<int64_t, int>> capture_hints_;
  std::set<int64_t> hintables_;
  double next_sync_ = 0;
  double connect_started_ = 0;
  bool warned_slow_connect_ = false;
};

}  // namespace ap
