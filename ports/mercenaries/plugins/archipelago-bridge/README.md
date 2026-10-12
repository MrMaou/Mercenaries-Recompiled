# Archipelago bridge plugin

Connects the Mercenaries Archipelago client to Mercenaries Recompiled. It needs a
recomp build with plugin interface 2 (`run_lua`).

## Build

From a Visual Studio 2022 x64 developer prompt, at the repository root:

~~~bat
cmake -S ports/mercenaries/plugins/archipelago-bridge -B build/archipelago-bridge -A x64
cmake --build build/archipelago-bridge --config Release
~~~

Copy the printed output folder (`archipelago_bridge.dll`, `mod.ini`,
`bridge.ini`) to `mods/Archipelago Bridge/`, enable it in the mod selector, and
launch modded.

## Settings

`plugin-settings/archipelago-bridge/bridge.ini`:

| Key | Default | Meaning |
| --- | --- | --- |
| Port | 28012 | Local TCP port. The bridge listens on 127.0.0.1 only. |
| UpdateBudgetMs | 4 | Most milliseconds of client work per input update (1-16). |
| MaxRequestsPerUpdate | 64 | Most requests per input update (1-1024). |

## Protocol

See the comment at the top of `bridge.c`. One client at a time; a new
connection drops anything queued for the old one.
