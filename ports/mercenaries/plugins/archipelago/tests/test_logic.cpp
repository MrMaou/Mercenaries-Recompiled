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

  // ---- Save binding ---------------------------------------------------
  {
    LuaGameIO save_io(argv[1]);
    std::vector<std::string> save_log;
    ap::GameLogic linked(save_io, [&](const std::string &line) { save_log.push_back(line); });
    enter_gameplay(save_io, 0x00200000);
    const std::string cash = std::to_string(money->money);
    auto money_is = [&](int copies) {
      return save_io.eval("TEST_money") == std::to_string(1000 + copies * money->money);
    };
    linked.bind_save(1234);
    std::vector<int64_t> got = {money->id};
    linked.send_items(got, {}, options);
    check(linked.save_state() == ap::SaveState::Linked, "fresh save linked");
    check(save_io.eval("TEST_scribble.ap_seed") == "1234", "save stamped with the seed");
    check(money_is(1) && save_io.eval("TEST_scribble.ap_n" + std::to_string(money->id)) == "1",
          "cash delivered and recorded in the save");
    save_io.lua("SAVE1 = TEST_save()");

    got.push_back(money->id);
    ap::Counter server_record = {{money->id, 99}};  // ignored while the save is linked
    linked.send_items(got, server_record, options);
    check(money_is(2), "second cash copy delivered from the save record, not the server's");

    save_io.lua("TEST_load(SAVE1)");  // load an older save; the VM stays
    check(money_is(1), "older save loaded (cash back to one copy)");
    linked.send_items(got, {}, options);
    check(money_is(2), "load detected without a VM rebuild; missing cash re-delivered once");
    bool saw_load = false;
    for (const auto &line : save_log) saw_load |= line.find("A save was loaded") != std::string::npos;
    check(saw_load, "load reported in the log");
    linked.send_items(got, {}, options);
    check(money_is(2), "no further delivery after catching up");

    save_io.lua("TEST_load(SAVE1); AP = nil; AP_Version = nil");  // load with a VM rebuild
    save_io.poke32(0x003717BC, 0x00300000);
    linked.send_items(got, {}, options);
    check(money_is(2), "load with VM rebuild: missing cash re-delivered once");

    ap::GameLogic other(save_io);
    other.bind_save(9999);
    other.send_items({money->id, money->id, money->id}, {}, options);
    check(other.save_state() == ap::SaveState::Mismatch && money_is(2), "save of another seed: nothing delivered");
    bool refused = false;
    try {
      other.get_checks_and_hints(missing, options);
    } catch (const ap::SaveMismatch &) {
      refused = true;
    }
    check(refused, "save of another seed: no checks read");
    check(save_io.eval("TEST_scribble.ap_seed") == "1234", "foreign save's stamp left alone");
  }
  {
    LuaGameIO plain_io(argv[1]);
    plain_io.lua("TEST_scribble_writable = false");
    ap::GameLogic fallback(plain_io);
    enter_gameplay(plain_io, 0x00200000);
    fallback.bind_save(1234);
    ap::Counter record = fallback.send_items({money->id}, {}, options);
    check(fallback.save_state() == ap::SaveState::Unsupported, "save that cannot hold data: falls back");
    check(record[money->id] == 1 && plain_io.eval("TEST_money") == std::to_string(1000 + money->money),
          "fallback uses the caller's record");
    fallback.send_items({money->id}, record, options);
    check(plain_io.eval("TEST_money") == std::to_string(1000 + money->money), "fallback does not pay twice");
  }
  // ---- One failing item kind must not block the others (tutorial case) --
  {
    LuaGameIO tut_io(argv[1]);
    // Before the shop exists: the game refuses unlocks. Intel's ace check throws too.
    tut_io.lua("function Shop_UnlockItem(t) error('shop is not available yet') end");
    tut_io.lua("function gameflow_AttemptAceMissionUnlock() error('no chapter yet') end");
    std::vector<std::string> tut_log;
    ap::GameLogic tut(tut_io, [&](const std::string &line) { tut_log.push_back(line); });
    enter_gameplay(tut_io, 0x00200000);
    tut.bind_save(4321);
    tut.get_checks_and_hints({}, options);  // chapter known, so intel runs
    std::vector<int64_t> got = {shop1->id, money->id};
    tut.send_items(got, {}, options);
    check(tut_io.eval("TEST_money") == std::to_string(1000 + money->money), "cash delivered while shop unlocks fail");
    auto logged = [&](const char *text) {
      int n = 0;
      for (const auto &line : tut_log) n += line.find(text) != std::string::npos;
      return n;
    };
    check(logged("Could not apply shop unlocks") == 1 && logged("shop is not available yet") == 1,
          "shop failure logged with the game's reason");
    check(logged("Could not apply intel") == 1, "intel failure logged");
    tut.send_items(got, {}, options);
    tut.send_items(got, {}, options);
    check(logged("Could not apply shop unlocks") == 1, "same failure not logged again on retries");
    check(tut_io.eval("TEST_money") == std::to_string(1000 + money->money), "cash still paid only once");
    tut_io.lua("function Shop_UnlockItem(t) table.insert(TEST_unlocked, t) end");  // the shop opens
    tut.send_items(got, {}, options);
    check(tut_io.eval("TEST_dump(TEST_unlocked)") == "1=template_support_deliverH2", "refused unlock retried once the shop opens");
  }
  // ---- Shop names are matched in the client, ignoring case --------------
  {
    LuaGameIO case_io(argv[1]);
    case_io.lua("tMafiaShop = { { template = 'TEMPLATE_SUPPORT_DELIVERH2' } }");
    ap::GameLogic shop(case_io);
    enter_gameplay(case_io, 0x00200000);
    shop.send_items({shop1->id, shop2->id}, {}, options);
    check(case_io.eval("TEST_dump(TEST_unlocked)") ==
              "1=TEMPLATE_SUPPORT_DELIVERH2,2=template_support_deliverH3",
          "exact template from the shop table, prefix fallback otherwise");
  }
  // ---- A full script heap is collected, and reported once ---------------
  {
    LuaGameIO mem_io(argv[1]);
    mem_io.lua("function gameflow_AttemptAceMissionUnlock() error('not enough memory') end");
    std::vector<std::string> mem_log;
    ap::GameLogic mem(mem_io, [&](const std::string &line) { mem_log.push_back(line); });
    enter_gameplay(mem_io, 0x00200000);
    mem.get_checks_and_hints({}, options);
    ap::Counter record = mem.send_items({money->id, intel->id}, {}, options);
    mem.send_items({money->id, intel->id}, record, options);  // no linked save: pass the record back
    int collected = 0;
    for (const auto &line : mem_log) collected += line.find("collected garbage") != std::string::npos;
    check(collected == 1, "full script heap: garbage collected, reported once");
    check(mem_io.eval("TEST_money") == std::to_string(1000 + money->money), "cash still delivered once");
  }
  check(ap::GameLogic::save_stamp("12345", 0, 1) == ap::GameLogic::save_stamp("12345", 0, 1) &&
            ap::GameLogic::save_stamp("12345", 0, 1) != ap::GameLogic::save_stamp("12345", 0, 2) &&
            ap::GameLogic::save_stamp("12345", 0, 1) < (1u << 24) && ap::GameLogic::save_stamp("12345", 0, 1) != 0,
        "stamps are stable, per slot, 24-bit and non-zero");

  std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL OK", failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
