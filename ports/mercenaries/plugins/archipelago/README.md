# Archipelago plugin

Plays Mercenaries Recompiled as an [Archipelago](https://archipelago.gg) world,
with the client built into the game. It needs a recomp build with plugin
interface 2 (this fork). The world itself is
[mercs-ap](https://github.com/ToxicFrog/mercs-ap) (use the fork with the recomp
changes); generate seeds with its `.apworld` as usual.

## Using it

1. Copy the plugin folder (`archipelago.dll`, `mod.ini`, `archipelago.ini`,
   `cacert.pem`, `licenses/`) to `mods/Archipelago/` beside
   `Mercenaries Recompiled.exe`.
2. Enable **Archipelago** in the mod selector and choose **Launch Modded**.
   Start a new game.
3. Press **F7**. Enter the server (`archipelago.gg:38281` or `host:port`), your
   slot name and the room password, then **Connect**.

The panel shows the connection status, a log of items, hints and chat, and a
chat box (`!hint`, `!release` and other server commands work there). Items,
hints and chat also appear briefly in the top-right corner of the game. Close
the panel with F7, Esc or its close button; the client keeps running.

Tick **Connect when the game starts** to reconnect automatically next time.
The panel saves the server, slot and password in
`plugin-settings/archipelago/archipelago.ini`.

## What it does

- **Checks:** verified cards (killed or captured), completed faction missions,
  and missable number cards released at the end of their chapter. Capturing a
  card alive hints a progression item; completing a hint mission hints its card.
- **Items:** Ace intel, Merchant of Menace unlocks (vanilla unlocks are blocked),
  cash, faction reputation floors and free samples.
- **Goal:** sent when you verify the Ace of the goal chapter.
- **Bounties:** blueprints, treasures, listening posts and monuments, counted
  from the scribble memory key the game writes for each one collected or
  destroyed (so they survive reloads and count both maps). The game has 55
  listening posts and 20 monuments; the apworld's 56th and 21st checks (only
  with `all` or a number) cannot be reached here.

Game changes go through Lua in the game's script VM (the runtime from mercs-ap
`recomp_runtime.py`, embedded in `src/ap_data.inc`).

## Saves

The client links each save to one seed and slot, and keeps its own delivery
record inside the save (a few number keys in the game's scribble memory, which
the game writes into the save file):

- **New game:** the first sync stamps the save with the current seed and slot.
  The panel status line then says "Save linked".
- **Loading an older save:** the client notices (even if the script VM is not
  rebuilt) and re-delivers exactly the cash and free samples that save is
  missing. Checks already sent stay sent.
- **Wrong save:** if the save belongs to another seed or slot, the client warns
  once and changes nothing and sends nothing until you load the right save or
  start a new game.
- **Fallback:** if the save cannot keep the values, the status line says "Save
  not linked" and the server's per-slot record is used, as before.

Use the recomp's **Use separate saves** option in the mod selector to keep AP
saves apart from your normal ones. Vanilla shop unlocks already in an old,
non-AP save stay unlocked. The Python client does not use the save record;
do not alternate between it and the in-game client on one save.

## Settings

`plugin-settings/archipelago/archipelago.ini` (seeded from the shipped copy):

| Section | Key | Default | Meaning |
| --- | --- | --- | --- |
| Client | Server, Slot, Password | | Saved by the panel. |
| Client | AutoConnect | 0 | Connect when the game starts. |
| Client | Hotkey | F7 | Panel key. F8-F11 belong to the recomp. |
| Client | Toasts | 1 | In-frame notices. |
| Client | HudMessages | 0 | Also show messages through the game's HUD, one per delivery. |
| Bridge | Enabled, Port | 1, 28012 | Local socket for the Python client and `tools/recomp_repl.py`. |

The bridge refuses Lua and write requests while the in-game client is active,
so the Python client and the in-game client cannot both deliver items. Reads
still work, so `tools/recomp_repl.py` can scan memory during a session.

## Building

Visual Studio 2022 x64, CMake 3.24+, and OpenSSL for `wss://` (archipelago.gg).
With vcpkg:

~~~bat
vcpkg install openssl:x64-windows-static
cmake -S ports/mercenaries/plugins/archipelago -B build/archipelago -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build/archipelago --config Release
~~~

CMake downloads the pinned header-only dependencies (APClientPP, wswrap,
websocketpp 0.8.2, asio 1.28.2, nlohmann/json 3.12.0, zlib 1.3.1) and curl's CA
bundle. The static triplet links OpenSSL into the DLL, so there are no extra
DLLs to ship. `-DARCHIPELAGO_SSL=OFF` builds without OpenSSL; then only `ws://`
servers work: an address without a scheme is treated as `ws://`, and `wss://`
is refused with a message in the panel.

### Regenerating the data

`src/ap_data.inc` holds the item and location tables and the Lua runtime. After
changing the apworld or its runtime, regenerate it from mercs-ap:

~~~text
python tools/export_recomp_plugin.py PATH/TO/Archipelago \
  PATH/TO/Mercenaries-Recompiled/ports/mercenaries/plugins/archipelago/src/ap_data.inc
~~~

### Tests (Linux or macOS)

`tests/` runs the client logic against a stock Lua 5.0.3 VM with stubbed game
scripts, and the full session against a real Archipelago server:

~~~text
make -C tests check
# live: generate a Mercenaries seed, start MultiServer on it, then
make -C tests build/test_live DEPS=/path/with/apclientpp,wswrap,websocketpp,asio,json
tests/build/test_live tests/game_stubs.lua ws://127.0.0.1:38281 SLOT
~~~

`DEPS` needs the same pins as CMake (websocketpp 0.8.2 with asio 1.28.2) and
zlib development headers.

## Bridge protocol

See the comment at the top of `src/socket_bridge.c`. One client at a time; a
new connection drops anything queued for the old one.
