#include "game_logic.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <unordered_map>

#include "ap_data.inc"

namespace ap {
namespace {

// Xbox guest state used to decide whether it is safe to change the game. From
// the recomp's own title code; tied to the one supported retail XBE.
constexpr uint32_t VM_POINTER = 0x003717BC;
constexpr uint32_t TRANSITION_PENDING = 0x003717D6;
constexpr uint32_t MAP_HASH = 0x00403970;
constexpr uint32_t MAP_SOUTH = 0x4A5220AF;
constexpr uint32_t MAP_NORTH = 0x4A364A32;
constexpr uint32_t GAMEPLAY_SENTINELS = 0x00413F68;
constexpr uint32_t SENTINEL_0 = 0xC2CBD863;
constexpr uint32_t SENTINEL_1 = 0x4249D707;

constexpr const char *SUITS[] = {"clubs", "diamonds", "hearts", "spades"};
constexpr const char *FACTIONS[] = {"allies", "china", "mafia", "sk"};

const apdata::Item *item_by_id(int64_t id) {
  static const auto table = [] {
    std::unordered_map<int64_t, const apdata::Item *> map;
    for (const auto &item : apdata::ITEMS) map[item.id] = &item;
    return map;
  }();
  auto found = table.find(id);
  return found == table.end() ? nullptr : found->second;
}

const apdata::Location *location_by_id(int64_t id) {
  static const auto table = [] {
    std::unordered_map<int64_t, const apdata::Location *> map;
    for (const auto &location : apdata::LOCATIONS) map[location.id] = &location;
    return map;
  }();
  auto found = table.find(id);
  return found == table.end() ? nullptr : found->second;
}

int suit_from_lua(const std::string &name) {
  std::string lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  for (int i = 0; i < 4; ++i) {
    std::string stem(SUITS[i]);
    stem.pop_back();  // club, diamond, heart, spade
    if (lower.find(stem) != std::string::npos) return i;
  }
  return -1;
}

int to_int(const std::string &text) {
  try {
    return static_cast<int>(std::lround(std::stod(text)));
  } catch (const std::exception &) {
    return 0;
  }
}

std::string random_prefix(std::mt19937 &random) {
  char text[9];
  std::snprintf(text, sizeof(text), "%08x", static_cast<unsigned>(random()));
  return text;
}

}  // namespace

GameLogic::GameLogic(GameIO &io, Log log) : io_(io), log_(std::move(log)), random_(std::random_device{}()) {
  delivery_prefix_ = random_prefix(random_);
  for (auto &suit : cards_) suit.fill(1);
}

void GameLogic::set_bounty_address(int type, uint32_t address, unsigned size) {
  if (type < 0 || type >= 4) return;
  bounty_address_[type] = address;
  bounty_size_[type] = size == 1 || size == 2 || size == 4 ? size : 4;
}

//// Game access ////

std::string GameLogic::lua(const std::string &source) {
  LuaResult result = io_.lua(source);
  switch (result.status) {
    case LuaStatus::Ok:
      return result.text;
    case LuaStatus::NotReady:
      throw GameError("The game script VM is not ready.");
    case LuaStatus::Error:
      throw GameError("Lua error: " + result.text);
    default:
      throw GameError("Lua request failed (status " + std::to_string(int(result.status)) + ")");
  }
}

std::string GameLogic::call(const std::string &function, const std::string &args) {
  return lua("return " + function + "(" + args + ")");
}

uint32_t GameLogic::read_u32(uint32_t address) {
  uint32_t value = 0;
  if (!io_.read(address, &value, sizeof(value))) throw GameError("Guest memory read was rejected.");
  return value;
}

bool GameLogic::in_gameplay() {
  uint32_t sentinels[2] = {};
  if (!io_.read(GAMEPLAY_SENTINELS, sentinels, sizeof(sentinels))) throw GameError("Guest memory read was rejected.");
  uint32_t map = read_u32(MAP_HASH);
  uint8_t transition = 1;
  if (!io_.read(TRANSITION_PENDING, &transition, 1)) throw GameError("Guest memory read was rejected.");
  return sentinels[0] == SENTINEL_0 && sentinels[1] == SENTINEL_1 && (map == MAP_SOUTH || map == MAP_NORTH) &&
         transition == 0;
}

void GameLogic::validate() {
  if (!in_gameplay()) throw GameError("Game is not in normal gameplay.");
  uint32_t vm = read_u32(VM_POINTER);
  std::string version = lua("return AP_Version and AP_Version() or \"\"");
  if (version != apdata::RUNTIME_VERSION || !installed_vm_ || *installed_vm_ != vm) inject(vm);
  if (expected_stamp_) check_save();
}

//// Save binding ////

uint32_t GameLogic::save_stamp(const std::string &seed, int team, int slot) {
  // FNV-1a, folded to 24 bits so it survives a float in scribble memory.
  std::string key = seed + "|" + std::to_string(team) + "|" + std::to_string(slot);
  uint32_t hash = 2166136261u;
  for (unsigned char c : key) hash = (hash ^ c) * 16777619u;
  hash = (hash ^ (hash >> 24)) & 0xFFFFFFu;
  return hash ? hash : 1u;  // 0 means "unbound"
}

void GameLogic::bind_save(uint32_t stamp) {
  expected_stamp_ = stamp;
  save_state_ = SaveState::Unbound;
  save_sent_.clear();
  tick_written_ = false;
}

void GameLogic::unbind_save() {
  expected_stamp_.reset();
  save_state_ = SaveState::Unbound;
  save_sent_.clear();
  tick_written_ = false;
}

void GameLogic::forget_applied() {
  intel_value_.reset();
  floors_.clear();
  shop_names_.reset();
  shop_txn_ = 0;
  call("AP_ForgetApplied");
}

void GameLogic::check_save() {
  static const std::string ids = [] {
    std::string list = "{";
    for (const auto &item : apdata::ITEMS)
      if (item.kind == apdata::ItemKind::Money || item.kind == apdata::ItemKind::Coupon)
        list += std::to_string(item.id) + ",";
    return list + "}";
  }();
  uint32_t next_tick = tick_ % 0xFFFFFFu + 1u;  // 1..2^24-1, exact in a float
  std::string text = call("AP_SyncSave", ids + ", " + std::to_string(next_tick));
  if (text.empty()) {
    save_state_ = SaveState::Unsupported;
    return;
  }
  uint32_t stamp = 0, tick = 0;
  Counter counts;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = line.substr(0, eq);
    int value = to_int(line.substr(eq + 1));
    if (key == "stamp") stamp = uint32_t(value);
    else if (key == "tick") tick = uint32_t(value);
    else counts[std::stoll(key)] = value;
  }
  // The tick read back differs from the one we wrote last: the save data was
  // replaced (a save was loaded), even if the script VM was not rebuilt.
  bool reloaded = tick_written_ && tick != tick_;
  tick_ = next_tick;
  tick_written_ = true;

  if (stamp == 0) {
    if (call("AP_StampSave", std::to_string(*expected_stamp_)) != "ok") {
      if (save_state_ != SaveState::Unsupported && log_)
        log_("This save cannot store Archipelago data; using the server's record instead.");
      save_state_ = SaveState::Unsupported;
      return;
    }
    if (log_) log_("Linked this save to the current seed and slot.");
    save_state_ = SaveState::Linked;
    save_sent_.clear();
  } else if (stamp == *expected_stamp_) {
    if (save_state_ != SaveState::Linked && log_) log_("This save belongs to the current seed and slot.");
    save_state_ = SaveState::Linked;
    save_sent_ = std::move(counts);
  } else {
    save_state_ = SaveState::Mismatch;
    throw SaveMismatch("This save belongs to a different seed or slot.");
  }
  if (reloaded) {
    if (log_) log_("A save was loaded; re-applying items to it.");
    forget_applied();
  }
}

void GameLogic::inject(uint32_t vm) {
  char text[80];
  std::snprintf(text, sizeof(text), "Installing Archipelago runtime in script VM $%08X.", vm);
  if (log_) log_(text);
  std::string version = lua(apdata::RUNTIME);
  if (version != apdata::RUNTIME_VERSION) throw GameError("Runtime install returned '" + version + "'.");
  // A new VM has none of our state; force everything idempotent to be re-sent.
  installed_vm_ = vm;
  intel_value_.reset();
  floors_.clear();
  shop_names_.reset();
  shop_txn_ = 0;
}

std::string GameLogic::shop_table() { return call("AP_ShopTable"); }

GameLogic::Poll GameLogic::parse_poll(const std::string &text) const {
  Poll poll;
  for (auto &suit : poll.cards) suit.fill(1);
  std::string quadrant;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = line.substr(0, eq), value = line.substr(eq + 1);
    if (key.rfind("card.", 0) == 0) {
      auto dot = key.rfind('.');
      if (dot <= 5) continue;
      int suit = suit_from_lua(key.substr(5, dot - 5));
      int rank = to_int(key.substr(dot + 1));
      if (rank == 14) rank = 1;  // the engine ranks aces above kings
      if (suit >= 0 && rank >= 1 && rank <= 13) poll.cards[suit][rank - 1] = to_int(value);
    } else if (key.rfind("mission.", 0) == 0) {
      poll.missions[key.substr(8)] = to_int(value);
    } else if (key == "quadrant") {
      quadrant = value;
    } else if (key == "blocked_shop") {
      poll.blocked_shop = to_int(value);
    }
  }
  std::transform(quadrant.begin(), quadrant.end(), quadrant.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  for (const char *faction : FACTIONS) poll.missions[faction] += quadrant == "nw" ? 6 : 0;
  return poll;
}

std::array<int, 4> GameLogic::read_bounties() {
  std::array<int, 4> counts{};
  bool any = false;
  for (int type = 0; type < 4; ++type) {
    if (!bounty_address_[type]) continue;
    any = true;
    uint32_t value = 0;
    if (!io_.read(bounty_address_[type], &value, bounty_size_[type])) throw GameError("Bounty counter read was rejected.");
    counts[type] = static_cast<int>(value);
  }
  if (!any && !warned_bounties_ && log_) {
    log_("Bounty counter addresses are not set; bounty checks are disabled.");
    warned_bounties_ = true;
  }
  return counts;
}

int GameLogic::chapter_from_cards() const {
  if (is_card_verified(3, 1)) return 5;  // victory
  if (is_card_verified(2, 1)) return 4;
  if (is_card_verified(1, 1)) return 3;
  if (is_card_verified(0, 1)) return 2;
  return 1;
}

//// Locations ////

bool GameLogic::is_checked(const apdata::Location &location) const {
  switch (location.kind) {
    case apdata::LocationKind::Card:
      return is_card_verified(location.suit, location.rank);
    case apdata::LocationKind::Mission: {
      auto found = missions_.find(location.faction);
      return found != missions_.end() && location.rank < found->second;
    }
    case apdata::LocationKind::Bounty:
      return bounties_[location.bounty_type] >= location.bounty_count;
  }
  return false;
}

bool GameLogic::is_missed(const apdata::Location &location) const {
  // Missions are auto-cleared at the end of their chapter by the game; bounties
  // can always be collected later. Missable number cards are released once
  // their chapter is over.
  if (location.kind != apdata::LocationKind::Card || location.never_missable) return false;
  return !is_checked(location) && latest_chapter_ > location.min_chapter;
}

CheckResult GameLogic::get_checks_and_hints(const std::set<int64_t> &missing, const SlotOptions &options) {
  validate();
  Poll poll = parse_poll(call("AP_Poll"));
  cards_ = poll.cards;
  missions_ = poll.missions;
  blocked_shop_ = poll.blocked_shop;
  bounties_ = read_bounties();
  latest_chapter_ = chapter_from_cards();

  CheckResult result;
  for (int64_t id : missing) {
    const apdata::Location *location = location_by_id(id);
    if (!location) continue;
    if (is_checked(*location) || is_missed(*location)) result.found.insert(id);
  }
  // Capturing a card alive hints a progression item (clubs to hearts only).
  for (int suit = 0; suit < 3; ++suit) {
    for (int rank = 1; rank <= 13; ++rank) {
      size_t index = size_t(suit) * 13 + size_t(rank) - 1;
      if (is_card_captured(suit, rank) && index < options.hints_from_cards.size())
        result.capture_hints.insert(options.hints_from_cards[index]);
    }
  }
  return result;
}

std::set<int64_t> GameLogic::get_hintable_checks(const std::set<int64_t> &checked,
                                                 const std::set<int64_t> &missing) const {
  std::set<int64_t> hintable;
  for (int64_t id : missing) {
    const apdata::Location *location = location_by_id(id);
    if (location && location->kind == apdata::LocationKind::Card && location->hint_mission &&
        checked.count(location->hint_mission))
      hintable.insert(id);
  }
  return hintable;
}

//// Items ////

Counter GameLogic::send_items(const std::vector<int64_t> &received, const Counter &old_sent,
                              const SlotOptions &options) {
  std::vector<const apdata::Item *> items;
  items.reserve(received.size());
  for (int64_t id : received)
    if (const apdata::Item *item = item_by_id(id)) items.push_back(item);

  try {
    validate();

    // Each kind of item on its own: one the game refuses (say, a shop unlock
    // before the shop exists) must not hold up the others, cash above all.
    apply_step("shop unlocks", [&] {
      std::vector<const apdata::Item *> unlocks;
      for (const auto *item : items)
        if (item->kind == apdata::ItemKind::ShopUnlock &&
            std::find(unlocks.begin(), unlocks.end(), item) == unlocks.end())
          unlocks.push_back(item);
      std::sort(unlocks.begin(), unlocks.end(), [](auto *a, auto *b) { return a->tag < b->tag; });
      std::vector<std::string> names;
      for (const auto *item : unlocks) names.emplace_back(item->shop_template);
      set_unlocked_shop_items(names, received.size());
    });

    // Intel: total for the current chapter, scaled to the game's target of 80.
    apply_step("intel", [&] {
      int chapter = current_chapter();
      if (chapter < 1) return;
      int suit = std::min(chapter, 4) - 1;
      double total = 0;
      for (const auto *item : items)
        if (item->kind == apdata::ItemKind::Intel && (item->intel_suit < 0 || item->intel_suit == suit))
          total += item->intel;
      double target = options.vanilla_intel ? options.vanilla_intel_target : options.intel_target;
      if (options.progressive_intel) total = std::max(0.0, total - target * (chapter - 1));
      set_intel(total, target);
    });

    // Reputation floors: -50, then 0, then diminishing steps towards 100.
    apply_step("reputation", [&] {
      std::map<std::string, int> counts;
      for (const auto *item : items)
        if (item->kind == apdata::ItemKind::Reputation) ++counts[item->faction];
      std::map<std::string, double> floors;
      for (const auto &[faction, count] : counts)
        floors[faction] = count <= 2 ? -100.0 + 50.0 * count : 100.0 - 100.0 * std::pow(0.9, count - 2);
      set_reputation_floors(floors);
    });

    // One-shot deliveries: all unsent money, one coupon, one message. A linked
    // save carries its own record; otherwise the caller's (server) record.
    const bool linked = save_state_ == SaveState::Linked;
    const Counter &base = linked ? save_sent_ : old_sent;
    Counter new_sent = base;
    Counter all_money;
    for (const auto *item : items)
      if (item->kind == apdata::ItemKind::Money) ++all_money[item->id];
    int64_t money_total = 0;
    for (const auto &[id, count] : all_money) {
      auto previous = base.find(id);
      int unsent = count - (previous == base.end() ? 0 : previous->second);
      if (unsent > 0) money_total += item_by_id(id)->money * unsent;
      new_sent[id] = std::max(new_sent[id], count);
    }

    // Only coupons not delivered yet, in the order they were received.
    std::string support_item;
    Counter seen;
    std::vector<const apdata::Item *> shop_received;
    for (const auto *item : items)
      if (item->kind == apdata::ItemKind::ShopUnlock) shop_received.push_back(item);
    for (const auto *coupon : items) {
      if (coupon->kind != apdata::ItemKind::Coupon) continue;
      auto previous = base.find(coupon->id);
      if (++seen[coupon->id] <= (previous == base.end() ? 0 : previous->second)) continue;
      std::vector<const apdata::Item *> matches;
      for (const auto *unlock : shop_received)
        if (unlock->groups & coupon->groups) matches.push_back(unlock);
      if (matches.empty()) continue;
      support_item = matches[std::uniform_int_distribution<size_t>(0, matches.size() - 1)(random_)]->shop_template;
      ++new_sent[coupon->id];
      break;
    }

    std::string save_counts;  // written to the save together with the delivery
    if (linked) {
      save_counts = "{";
      for (const auto &[id, count] : new_sent) {
        auto previous = base.find(id);
        if (previous == base.end() || previous->second != count)
          save_counts += "[" + std::to_string(id) + "] = " + std::to_string(count) + ", ";
      }
      save_counts += "}";
    }
    std::string message = messages_.empty() ? std::string() : messages_.front();
    bool delivered = false;
    apply_step("cash and free samples", [&] {
      delivered = send_once(money_total, message, support_item, save_counts);
    });
    if (delivered) {
      if (!message.empty()) messages_.pop_front();
      if (linked) save_sent_ = new_sent;
      return new_sent;
    }
    return base;
  } catch (const GameError &) {
    return old_sent;
  }
}

template <typename Step>
bool GameLogic::apply_step(const char *what, Step step) {
  try {
    step();
    step_errors_.erase(what);
    return true;
  } catch (const SaveMismatch &) {
    throw;  // handled by the caller: nothing at all may change
  } catch (const GameError &e) {
    std::string &last = step_errors_[what];
    if (last != e.what() && log_) log_(std::string("Could not apply ") + what + " (will retry): " + e.what());
    last = e.what();
    return false;
  }
}

bool GameLogic::send_once(int64_t money, const std::string &message, const std::string &support_item,
                          const std::string &counts) {
  if (!money && message.empty() && support_item.empty()) return false;
  auto payload = std::make_tuple(money, message, support_item, counts);
  if (!pending_delivery_ || pending_delivery_->second != payload)
    pending_delivery_ = std::make_pair(delivery_prefix_ + "-" + std::to_string(next_delivery_++), payload);
  call("AP_Deliver", lua_quote(pending_delivery_->first) + ", " + std::to_string(money) + ", " + lua_quote(message) +
                         ", " + lua_quote(support_item) + (counts.empty() ? "" : ", " + counts));
  if (log_ && (money || !support_item.empty())) {
    std::string text = "Delivered";
    if (money) {
      std::string digits = std::to_string(money);
      for (int i = int(digits.size()) - 3; i > 0; i -= 3) digits.insert(size_t(i), ",");
      text += " $" + digits;
    }
    if (!support_item.empty()) {
      std::string name = support_item;  // the shop item's display name, if known
      for (const auto &item : apdata::ITEMS)
        if (item.kind == apdata::ItemKind::ShopUnlock && support_item == item.shop_template) name = item.name;
      text += (money ? " and a free " : " a free ") + name;
    }
    log_(text + ".");
  }
  pending_delivery_.reset();
  return true;
}

void GameLogic::set_unlocked_shop_items(const std::vector<std::string> &names, size_t txn) {
  if (shop_names_ && *shop_names_ == names && shop_txn_ == txn) return;
  std::string list = "{";
  for (size_t i = 0; i < names.size(); ++i) list += (i ? ", " : "") + lua_quote(names[i]);
  list += "}";
  std::string result = call("AP_SetShop", list);
  if (result == "nohook") throw GameError("Shop_UnlockItem is not available yet.");
  auto bar = result.find('|');
  std::string applied = result.substr(0, bar);
  if (!applied.empty() && applied != "0" && log_) log_("Unlocked " + applied + " Merchant of Menace item(s).");
  // Refused unlocks are retried on the next sync; the rest are already applied.
  if (bar != std::string::npos) {
    std::string failed = result.substr(bar + 1);  // "template: error; template: error; ..."
    size_t count = 1, at = 0;
    while ((at = failed.find("; ", at)) != std::string::npos) ++count, at += 2;
    std::string first = failed.substr(0, failed.find("; "));
    throw GameError("the game refused " + std::to_string(count) + " unlock(s), first " + first);
  }
  shop_names_ = names;
  shop_txn_ = txn;
}

void GameLogic::set_intel(double amount, double target) {
  double value = target > 0 ? amount / target * 80.0 : 0.0;
  if (!intel_value_ || *intel_value_ != value) {
    call("AP_SetIntel", lua_number(value));
    intel_value_ = value;
  }
  call("AP_Tick");
}

void GameLogic::set_reputation_floors(const std::map<std::string, double> &floors) {
  bool changed = false;
  for (const auto &[faction, floor] : floors) {
    auto found = floors_.find(faction);
    if (found == floors_.end() || found->second != floor) {
      floors_[faction] = floor;
      changed = true;
    }
  }
  if (!changed) return;
  std::string table = "{";
  for (const auto &[faction, floor] : floors_) table += "[" + lua_quote(faction) + "] = " + lua_number(floor) + ", ";
  table += "}";
  call("AP_SetFloors", table);
}

}  // namespace ap
