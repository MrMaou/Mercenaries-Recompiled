// Session + GameLogic against a real Archipelago server and a Lua-stubbed game.
//   test_live game_stubs.lua ws://localhost:38281 SLOT
// Needs a running MultiServer with a Mercenaries seed for SLOT; see README.md.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "../src/ap_session.hpp"
#include "../src/game_logic.hpp"
#include "lua_game_io.hpp"

namespace {
int failures = 0;
void check(bool ok, const std::string &what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++failures;
}

double now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

template <typename Done>
bool run(ap::Session &session, double seconds, Done done) {
  double end = now() + seconds;
  while (now() < end) {
    session.poll(now());
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}
}  // namespace

int main(int argc, char **argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: test_live game_stubs.lua SERVER SLOT\n");
    return 2;
  }
  LuaGameIO io(argv[1]);
  io.poke32(0x00413F68, 0xC2CBD863);
  io.poke32(0x00413F6C, 0x4249D707);
  io.poke32(0x00403970, 0x4A5220AF);
  io.poke32(0x003717BC, 0x00123450);

  std::vector<std::string> notices;
  ap::GameLogic logic(io, [](const std::string &line) { std::printf("  [game] %s\n", line.c_str()); });
  auto on_message = [&](ap::MessageKind kind, const std::string &text) {
    std::printf("  [%s] %s\n", kind == ap::MessageKind::Log ? "log" : kind == ap::MessageKind::Notice ? "notice" : "error",
                text.c_str());
    if (kind != ap::MessageKind::Log) notices.push_back(text);
  };

  ap::SessionConfig config{argv[2], argv[3], "", "/tmp/mercs-ap-test-uuid", ""};
  {
    ap::Session session(logic, on_message);
    session.connect(config);
    check(run(session, 20, [&] { return session.slot_connected(); }), "slot connected");
    // Stubbed scribble memory has 2C killed, AC captured, 5D captured and
    // allies missions 1-2 done: those checks go out and their items come back.
    run(session, 20, [&] { return session.location_progress().first >= 6; });
    auto [checked, total] = session.location_progress();
    check(checked >= 6, "checks reported to the server (" + std::to_string(checked) + "/" + std::to_string(total) + ")");
    run(session, 3, [] { return false; });  // let item deliveries settle
    std::printf("  status: %s\n", session.status().c_str());
    check(session.status().find("checks") != std::string::npos, "status shows progress");
  }
  std::string money_after_first = io.eval("TEST_money");
  std::string support_after_first = io.eval("table.getn(TEST_support)");
  std::printf("  money=%s support=%s unlocked=%s floors=%s intel=%s\n", money_after_first.c_str(),
              support_after_first.c_str(), io.eval("table.getn(TEST_unlocked)").c_str(),
              io.eval("TEST_dump(TEST_floors)").c_str(), io.eval("AP.intel").c_str());

  // Reconnect with a fresh session: the sent record comes back from data
  // storage, so nothing one-shot is delivered twice.
  {
    ap::Session session(logic, on_message);
    session.connect(config);
    check(run(session, 20, [&] { return session.slot_connected(); }), "reconnected");
    run(session, 5, [] { return false; });
    check(io.eval("TEST_money") == money_after_first, "money not paid twice after reconnect");
    check(io.eval("table.getn(TEST_support)") == support_after_first, "free samples not repeated after reconnect");
    check(session.say("hello from the test client"), "chat message sent");
    run(session, 2, [] { return false; });
  }

  // A bad slot name is refused and the session stops trying.
  {
    ap::Session session(logic, on_message);
    ap::SessionConfig bad = config;
    bad.slot = "NoSuchSlot";
    session.connect(bad);
    run(session, 10, [&] { return !session.active(); });
    check(!session.active() && session.status().find("refused") != std::string::npos,
          "bad slot refused: " + session.status());
  }

  std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL OK", failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
