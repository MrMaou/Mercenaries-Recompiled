// GameLogic against a stock Lua 5.0.3 VM with stubbed game scripts.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../src/game_data.hpp"
#include "../src/game_logic.hpp"
#include "../src/ap_data.inc"
#include "lua_game_io.hpp"

namespace {
int failures = 0;
void check(bool ok, const std::string &what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++failures;
}

const apdata::Item *find_item(apdata::ItemKind kind, const char *shop_template = nullptr, int intel_suit = -2) {
  for (const auto &item : apdata::ITEMS) {
    if (item.kind != kind) continue;
    if (shop_template && std::string(item.shop_template) != shop_template) continue;
    if (intel_suit != -2 && item.intel_suit != intel_suit) continue;
    return &item;
  }
  return nullptr;
}

int64_t location_id(const char *short_name) {
  for (const auto &location : apdata::LOCATIONS)
    if (std::string(location.short_name) == short_name) return location.id;
  return 0;
}

void enter_gameplay(LuaGameIO &io, uint32_t vm) {
  io.poke32(0x00413F68, 0xC2CBD863);
  io.poke32(0x00413F6C, 0x4249D707);
  io.poke32(0x00403970, 0x4A5220AF);
  io.poke32(0x003717BC, vm);
}
}  // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: test_logic game_stubs.lua\n");
    return 2;
  }
  LuaGameIO io(argv[1]);
  std::vector<std::string> log;
  ap::GameLogic logic(io, [&](const std::string &line) { log.push_back(line); });
  ap::SlotOptions options;
  options.intel_target = 10;
  for (int i = 0; i < 52; ++i) options.hints_from_cards.emplace_back(i, 1);

  try {
    logic.validate();
    check(false, "validate refuses outside gameplay");
  } catch (const ap::GameError &) {
    check(true, "validate refuses outside gameplay");
  }

  enter_gameplay(io, 0x00123450);
  logic.validate();
  check(io.eval("AP_Version()") == apdata::RUNTIME_VERSION, "runtime installed");
  int calls = io.lua_calls;
  logic.validate();
  check(io.lua_calls == calls + 1, "second validate only checks the version");

  const auto *money = find_item(apdata::ItemKind::Money);
  const auto *shop1 = find_item(apdata::ItemKind::ShopUnlock, "deliverH2");
  const auto *shop2 = find_item(apdata::ItemKind::ShopUnlock, "deliverH3");
  const auto *intel = find_item(apdata::ItemKind::Intel, nullptr, -1);
  const auto *coupon = find_item(apdata::ItemKind::Coupon);
  const apdata::Item *rep = find_item(apdata::ItemKind::Reputation);
  for (const auto &item : apdata::ITEMS)
    if (item.kind == apdata::ItemKind::Coupon && (item.groups & shop1->groups)) coupon = &item;
  std::vector<int64_t> received = {money->id, rep->id, rep->id, shop1->id, shop2->id, intel->id, coupon->id};

  logic.queue_message("Hello from AP: \"quotes\" \\ and \xc3\xbcnic\xc3\xb6" "de");
  logic.get_checks_and_hints({}, options);  // sets the chapter for intel
  ap::Counter sent = logic.send_items(received, {}, options);
  check(io.eval("TEST_money") == std::to_string(1000 + money->money), "money delivered");
  check(sent[money->id] == 1 && sent[coupon->id] == 1, "one-shot items recorded as sent");
  check(io.eval("TEST_hud[1]") == "Hello from AP: \"quotes\" \\ and \xc3\xbcnic\xc3\xb6" "de", "HUD message survives quoting");
  check(io.eval("table.getn(TEST_support)") == "1", "coupon delivered once");
  std::string unlocked = io.eval("TEST_dump(TEST_unlocked)");
  check(unlocked == "1=template_support_deliverH2,2=template_support_deliverH3", "shop unlocks: " + unlocked);
  check(std::stod(io.eval("AP.intel")) == intel->intel * 80.0 / 10, "intel scaled to 80");
  check(io.eval("TEST_floors." + std::string(rep->faction)) == "0", "two reputation items set the floor to 0");
  check(io.eval("TEST_ace") != "0", "tick ran the ace unlock check");

  ap::Counter sent2 = logic.send_items(received, sent, options);
  check(sent2 == sent, "sent counter stable");
  check(io.eval("TEST_money") == std::to_string(1000 + money->money), "money not re-sent");
  check(io.eval("table.getn(TEST_unlocked)") == "2", "shop not re-unlocked");
  check(io.eval("table.getn(TEST_support)") == "1", "coupon not re-sent");

  // A second copy of the money item arrives later.
  received.push_back(money->id);
  ap::Counter sent3 = logic.send_items(received, sent2, options);
  check(sent3[money->id] == 2, "second money copy recorded");
  check(io.eval("TEST_money") == std::to_string(1000 + 2 * money->money), "second money copy paid");

  // Checks from stubbed scribble memory: 2C killed, AC captured, 5D captured,
  // allies mission 3 and china mission 1 current.
  std::set<int64_t> missing;
  for (const auto &location : apdata::LOCATIONS) missing.insert(location.id);
  ap::CheckResult checks = logic.get_checks_and_hints(missing, options);
  check(checks.found.count(location_id("2C")) == 1, "killed 2 of clubs checked");
  check(checks.found.count(location_id("1C")) == 1, "captured ace of clubs checked");
  check(checks.found.count(location_id("5D")) == 1, "captured 5 of diamonds checked");
  check(checks.found.count(location_id("A1")) && checks.found.count(location_id("A2")) &&
            !checks.found.count(location_id("A3")),
        "allies missions 1-2 complete, 3 not");
  check(!checks.found.count(location_id("C1")), "china mission 1 not complete");
  check(logic.current_chapter() == 2, "chapter 2 after the ace of clubs");
  check(checks.found.count(location_id("3C")) == 1, "unchecked clubs number card released in chapter 2");
  check(!checks.found.count(location_id("3D")), "diamonds number card not released yet");
  check(!checks.found.count(location_id("blueprint1")), "no bounty checks without addresses");
  check(checks.capture_hints.count({0, 1}) && checks.capture_hints.count({17, 1}) && checks.capture_hints.size() == 2,
        "capture hints for the ace of clubs and 5 of diamonds");

  // Hintable cards: the 3 of clubs is hinted by completing C1.
  std::set<int64_t> hintable = logic.get_hintable_checks({location_id("C1")}, {location_id("3C"), location_id("4C")});
  check(hintable == std::set<int64_t>{location_id("3C")}, "3C hintable once C1 is checked");

  // Bounty address configured: count comes from guest memory.
  logic.set_bounty_address(1, 0x00500000, 4);
  io.poke32(0x00500000, 5);
  checks = logic.get_checks_and_hints(missing, options);
  check(checks.found.count(location_id("blueprint5")) && !checks.found.count(location_id("blueprint10")),
        "blueprint bounty checks from configured address");

  // VM rebuilt by a save load: runtime and idempotent state come back.
  io.lua("AP = nil; AP_Version = nil");
  io.poke32(0x003717BC, 0x00654320);
  logic.send_items(received, sent3, options);
  check(io.eval("AP_Version()") == apdata::RUNTIME_VERSION, "runtime reinstalled in the new VM");
  check(std::stod(io.eval("AP.intel")) == intel->intel * 80.0 / 10, "intel re-sent after reinstall");

  // Not ready: nothing is marked as sent.
  io.ready = false;
  received.push_back(money->id);
  ap::Counter sent4 = logic.send_items(received, sent3, options);
  check(sent4 == sent3, "nothing marked sent while the VM is not ready");
  io.ready = true;
  ap::Counter sent5 = logic.send_items(received, sent3, options);
  check(sent5[money->id] == 3, "delivered once ready again");
  check(io.eval("TEST_money") == std::to_string(1000 + 3 * money->money), "third money copy paid exactly once");

  std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL OK", failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
