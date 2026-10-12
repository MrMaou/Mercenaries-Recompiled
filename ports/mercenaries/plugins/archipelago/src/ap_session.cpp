#include "ap_session.hpp"

#include <apclient.hpp>
#include <apuuid.hpp>

#include <algorithm>
#include <list>

namespace ap {
namespace {

constexpr int ITEMS_HANDLING = 0b111;  // all items come from the server
const APClient::Version CLIENT_VERSION{0, 6, 2};
using json = nlohmann::json;

}  // namespace

Session::Session(GameLogic &logic, Message message) : logic_(logic), message_(std::move(message)) {}

Session::~Session() = default;

void Session::connect(const SessionConfig &config) {
  disconnect();
  config_ = config;
  if (config_.server.empty() || config_.slot.empty()) {
    message_(MessageKind::Error, "Enter a server and a slot name first.");
    return;
  }
  try {
    std::string uuid = ap_get_uuid(config_.uuid_file, config_.server);
    client_ = std::make_unique<APClient>(uuid, "Mercenaries", config_.server, config_.cert_store);
  } catch (const std::exception &e) {
    client_.reset();
    state_text_ = std::string("Cannot connect: ") + e.what();
    message_(MessageKind::Error, state_text_);
    return;
  }
  install_handlers();
  state_text_ = "Connecting to " + config_.server + "...";
  message_(MessageKind::Log, state_text_);
  connect_started_ = -1;  // set on the next poll
  warned_slow_connect_ = false;
}

void Session::disconnect() {
  if (client_) message_(MessageKind::Log, "Disconnected.");
  client_.reset();
  slot_connected_ = false;
  sent_loaded_ = false;
  goal_sent_ = false;
  drop_client_ = false;
  received_.clear();
  sent_.clear();
  capture_hints_.clear();
  hintables_.clear();
  state_text_ = "Not connected";
}

bool Session::say(const std::string &text) {
  return client_ && slot_connected_ && !text.empty() && client_->Say(text);
}

std::string Session::sent_key() const {
  // Data storage is shared by the whole room; name this slot. The Python client
  // (mercs-ap MercenariesClient.py) uses the same key.
  return "mercs_sent_items_" + std::to_string(client_->get_team_number()) + "_" +
         std::to_string(client_->get_player_number());
}

std::string Session::status() const { return state_text_; }

std::pair<size_t, size_t> Session::location_progress() const {
  if (!client_ || !slot_connected_) return {0, 0};
  size_t checked = client_->get_checked_locations().size();
  return {checked, checked + client_->get_missing_locations().size()};
}

void Session::install_handlers() {
  APClient &client = *client_;

  client.set_socket_connected_handler([this] { message_(MessageKind::Log, "Socket connected."); });
  client.set_socket_error_handler([this](const std::string &error) {
    if (!slot_connected_) state_text_ = "Connecting to " + config_.server + "... (" + error + ")";
  });
  client.set_socket_disconnected_handler([this] {
    if (slot_connected_) message_(MessageKind::Error, "Lost connection to the server; reconnecting.");
    slot_connected_ = false;
    sent_loaded_ = false;
    state_text_ = "Reconnecting to " + config_.server + "...";
  });
  client.set_room_info_handler([this] {
    client_->ConnectSlot(config_.slot, config_.password, ITEMS_HANDLING, {"AP"}, CLIENT_VERSION);
  });
  client.set_slot_refused_handler([this](const std::list<std::string> &errors) {
    std::string text = "Connection refused:";
    for (const auto &error : errors) text += " " + error;
    state_text_ = text;
    message_(MessageKind::Error, text);
    drop_client_ = true;  // stop reconnecting; poll() drops the client
  });
  client.set_slot_connected_handler([this](const json &slot_data) {
    SlotOptions options;
    options.goal = slot_data.value("goal", 4);
    options.vanilla_intel = slot_data.value("vanilla_intel", false);
    options.vanilla_intel_target = slot_data.value("vanilla_intel_target", 80);
    options.intel_target = slot_data.value("intel_target", 10);
    options.progressive_intel = slot_data.value("progressive_intel", false);
    if (slot_data.contains("hints_from_cards") && slot_data["hints_from_cards"].is_array()) {
      for (const auto &pair : slot_data["hints_from_cards"])
        if (pair.is_array() && pair.size() == 2)
          options.hints_from_cards.emplace_back(pair[0].get<int64_t>(), pair[1].get<int>());
    }
    handle_slot_connected(std::move(options));
  });
  client.set_items_received_handler([this](const std::list<APClient::NetworkItem> &items) {
    for (const auto &item : items) {
      if (item.index < 0) continue;
      size_t index = size_t(item.index);
      if (index < received_.size()) {
        received_[index] = item.item;  // resync after a reconnect
      } else {
        received_.resize(index);
        received_.push_back(item.item);
      }
    }
    next_sync_ = 0;  // deliver promptly
  });
  client.set_set_reply_handler([this](const std::string &key, const json &value, const json &) {
    if (!client_ || key != sent_key()) return;
    Counter sent;
    if (value.is_object()) {
      for (const auto &[id, count] : value.items()) {
        try {
          if (count.is_number_integer()) sent[std::stoll(id)] = count.get<int>();
        } catch (const std::exception &) {
        }
      }
    }
    handle_sent(std::move(sent));
  });
  client.set_print_handler([this](const std::string &text) { handle_print(text, true, text); });
  client.set_print_json_handler([this](const APClient::PrintJSONArgs &args) {
    const int me = client_->get_player_number();
    std::string text = client_->render_json(args.data, APClient::RenderFormat::TEXT);
    bool toast = false;
    std::string hud;
    if (args.type == "ItemSend" || args.type == "ItemCheat") {
      if (args.item && args.receiving) {
        int source = args.item->player, receiving = *args.receiving;
        std::string name = client_->get_item_name(args.item->item, client_->get_player_game(receiving));
        if (receiving == me && source == me) {
          hud = "Found " + name;
        } else if (receiving == me) {
          hud = client_->get_player_alias(source) + " >> " + name;
        } else if (source == me) {
          hud = client_->get_player_alias(receiving) + " << " + name;
        }
        toast = !hud.empty();
      }
    } else if (args.type == "Hint") {
      toast = (args.receiving && *args.receiving == me) || (args.item && args.item->player == me);
    } else if (args.type == "Chat") {
      std::string sender = args.slot ? client_->get_player_alias(*args.slot) : "?";
      hud = "<" + sender + "> " + (args.message ? *args.message : text);
      toast = true;
    } else if (args.type == "ServerChat") {
      hud = "<*> " + (args.message ? *args.message : text);
      toast = true;
    } else if (args.type == "Countdown" || args.type == "CommandResult" || args.type == "AdminCommandResult" ||
               args.type == "Goal" || args.type == "Release" || args.type == "Collect") {
      toast = true;
    }
    handle_print(text, toast, hud);
  });
}

void Session::handle_slot_connected(SlotOptions options) {
  options_ = std::move(options);
  slot_connected_ = true;
  sent_loaded_ = false;
  goal_sent_ = false;
  capture_hints_.clear();
  hintables_.clear();
  state_text_ = "Connected as " + config_.slot + ".";
  message_(MessageKind::Notice, "Connected to Archipelago as " + config_.slot + ".");
  // Ask for this slot's record of one-shot items already delivered to the game.
  client_->Set(sent_key(), json::object(), true, {{"default", json::object()}});
}

void Session::handle_sent(Counter sent) {
  sent_ = std::move(sent);
  if (!sent_loaded_) next_sync_ = 0;
  sent_loaded_ = true;
}

void Session::handle_print(const std::string &text, bool toast, const std::string &hud_text) {
  if (text.empty()) return;
  message_(toast ? MessageKind::Notice : MessageKind::Log, text);
  if (!hud_text.empty()) logic_.queue_message(hud_text);
}

void Session::poll(double now) {
  if (!client_) return;
  if (connect_started_ < 0) connect_started_ = now;
  client_->poll();
  if (drop_client_) {
    std::string reason = state_text_;
    disconnect();
    state_text_ = reason;
    return;
  }
  if (!slot_connected_ && !warned_slow_connect_ && now - connect_started_ > 10) {
    message_(MessageKind::Error, "Still connecting to " + config_.server + "; check the address and port.");
    warned_slow_connect_ = true;
  }
  if (!slot_connected_ || !sent_loaded_ || now < next_sync_) return;
  try {
    sync();
    next_sync_ = now + 1.0;
  } catch (const GameError &e) {
    state_text_ = "Connected as " + config_.slot + ". Waiting for the game: " + e.what();
    next_sync_ = now + 4.0;
  }
}

void Session::sync() {
  // Send new items to the game.
  Counter sent = logic_.send_items(received_, sent_, options_);
  if (sent != sent_) {
    json value = json::object();
    for (const auto &[id, count] : sent) value[std::to_string(id)] = count;
    client_->Set(sent_key(), json::object(), true, {{"replace", value}});
    sent_ = sent;  // the reply confirms it; never deliver twice while it is in flight
  }

  // Report checks, including missable cards released at the end of a chapter.
  std::set<int64_t> missing = client_->get_missing_locations();
  CheckResult checks = logic_.get_checks_and_hints(missing, options_);
  if (!checks.found.empty()) client_->LocationChecks(std::list<int64_t>(checks.found.begin(), checks.found.end()));

  // Capturing a card alive hints a progression item, maybe in another world.
  for (const auto &hint : checks.capture_hints) {
    if (capture_hints_.insert(hint).second) client_->CreateHints({hint.first}, hint.second);
  }

  // Cards hinted by completed missions.
  std::set<int64_t> hintables = logic_.get_hintable_checks(client_->get_checked_locations(), client_->get_missing_locations());
  if (hintables != hintables_) {
    hintables_ = hintables;
    if (!hintables.empty()) client_->CreateHints(std::list<int64_t>(hintables.begin(), hintables.end()));
  }

  if (!goal_sent_ && logic_.current_chapter() > options_.goal) {
    client_->StatusUpdate(APClient::ClientStatus::GOAL);
    goal_sent_ = true;
    message_(MessageKind::Notice, "Goal complete!");
  }

  auto [checked, total] = location_progress();
  state_text_ = "Connected as " + config_.slot + ". " + std::to_string(checked) + "/" + std::to_string(total) +
                " checks, chapter " + std::to_string(std::min(logic_.current_chapter(), 4)) + ".";
}

}  // namespace ap
