// Game side of the in-game Archipelago client: a C++ port of the mercs-ap
// recomp backend (RecompIPC.py) and connector (MercenariesConnector.py).
//
// Everything here runs on the game thread from the plugin's on_update. All
// game access goes through GameIO; failures surface as GameError and are
// retried on a later update.
#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "game_data.hpp"
#include "game_io.hpp"

namespace ap {

using Counter = std::map<int64_t, int>;  // item id -> copies

struct SlotOptions {
  int goal = 4;
  bool vanilla_intel = false;
  int vanilla_intel_target = 80;
  int intel_target = 10;
  bool progressive_intel = false;
  // hints_from_cards[suit * 13 + rank - 1] = (location, player)
  std::vector<std::pair<int64_t, int>> hints_from_cards;
};

// How one-shot items (cash, free samples) are tracked for the loaded save.
enum class SaveState {
  Unbound,      // no session: the caller's record is used
  Linked,       // this save carries its own delivery record (scribble memory)
  Unsupported,  // scribble memory did not keep our values: the caller's record is used
  Mismatch,     // the save belongs to another seed/slot: nothing is changed
};

struct CheckResult {
  std::set<int64_t> found;                         // checked or auto-released
  std::set<std::pair<int64_t, int>> capture_hints;  // (location, player)
};

class GameLogic {
 public:
  using Log = std::function<void(const std::string &)>;

  explicit GameLogic(GameIO &io, Log log = {});

  // Bounty counters have no Lua API; their addresses come from settings.
  void set_bounty_address(int type, uint32_t address, unsigned size);

  // Connector API (MercenariesConnector.py).
  int current_chapter() const { return latest_chapter_; }
  CheckResult get_checks_and_hints(const std::set<int64_t> &missing, const SlotOptions &options);
  std::set<int64_t> get_hintable_checks(const std::set<int64_t> &checked, const std::set<int64_t> &missing) const;
  // received: every item id received, in order. Returns the new sent counter.
  Counter send_items(const std::vector<int64_t> &received, const Counter &old_sent, const SlotOptions &options);
  // Messages shown through the game's own HUD, one per delivery. Ignored when
  // hud_messages is off (the plugin's overlay shows them instead).
  void queue_message(std::string message) {
    if (hud_messages) messages_.push_back(std::move(message));
  }
  bool hud_messages = true;

  // Game state (RecompIPC.py).
  bool in_gameplay();
  void validate();
  std::string shop_table();
  int poll_blocked_shop() const { return blocked_shop_; }

  // Save binding. bind_save() on slot connect; the first gameplay sync then
  // stamps an unbound save, or checks the stamp of a bound one.
  static uint32_t save_stamp(const std::string &seed, int team, int slot);
  void bind_save(uint32_t stamp);
  void unbind_save();
  SaveState save_state() const { return save_state_; }

 private:
  struct Poll {
    std::array<std::array<int, 13>, 4> cards{};
    std::map<std::string, int> missions;
    int blocked_shop = 0;
  };

  std::string lua(const std::string &source);
  std::string call(const std::string &function, const std::string &args = "");
  uint32_t read_u32(uint32_t address);
  void inject(uint32_t vm);
  Poll parse_poll(const std::string &text) const;
  std::array<int, 4> read_bounties();
  int chapter_from_cards() const;

  bool is_checked(const apdata::Location &location) const;
  bool is_missed(const apdata::Location &location) const;
  bool is_card_verified(int suit, int rank) const { return cards_[suit][rank - 1] > 1; }
  bool is_card_captured(int suit, int rank) const { return cards_[suit][rank - 1] > 2; }

  bool send_once(int64_t money, const std::string &message, const std::string &support_item,
                 const std::string &counts);
  void check_save();
  void forget_applied();
  // Runs one kind of item delivery; a GameError is logged (once per distinct
  // message) and does not stop the other kinds.
  template <typename Step> bool apply_step(const char *what, Step step);
  void set_unlocked_shop_items(const std::vector<std::string> &names, size_t txn);
  void set_intel(double amount, double target);
  void set_reputation_floors(const std::map<std::string, double> &floors);

  GameIO &io_;
  Log log_;
  std::mt19937 random_;

  std::optional<uint32_t> installed_vm_;
  std::optional<double> intel_value_;
  std::map<std::string, double> floors_;
  std::optional<std::vector<std::string>> shop_names_;
  size_t shop_txn_ = 0;
  std::string delivery_prefix_;
  uint64_t next_delivery_ = 1;
  std::optional<std::pair<std::string, std::tuple<int64_t, std::string, std::string, std::string>>> pending_delivery_;
  std::optional<uint32_t> expected_stamp_;
  SaveState save_state_ = SaveState::Unbound;
  Counter save_sent_;
  uint32_t tick_ = 0;
  bool tick_written_ = false;
  std::deque<std::string> messages_;

  std::array<std::array<int, 13>, 4> cards_{};
  std::map<std::string, int> missions_;
  std::array<int, 4> bounties_{};
  std::array<uint32_t, 4> bounty_address_{};
  std::array<unsigned, 4> bounty_size_{};
  bool warned_bounties_ = false;
  std::map<std::string, std::string> step_errors_;  // last error logged per step
  int latest_chapter_ = 0;
  int blocked_shop_ = 0;
};

}  // namespace ap
