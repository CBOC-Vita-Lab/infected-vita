# tools/bot - play-through bot, observer, pad recorder and the end-to-end regression replay

The bot is a pad provider inside the port (`port/host/infected_bot.cpp`, installed from `infected_tuning.cpp`). Every flip it reads guest memory and
sets the pad (analog + buttons). It never writes guest memory and calls no game function; `INFECTED_TUNE_GOD` and the other tuning probes are
diagnostics and are not used by the verification runs.

## Run it

```
tools/bot/run.sh NAME SECONDS [VAR=val ...]     # lean-only, virtual clock, null draw, fresh copy of private/ms0.pristine
INFECTED_BOT=1 INFECTED_BOT_PARAMS=phase=0           # the bot (defaults: approach=140 lock=1 lock2=1 pulse2=3 retry=0)
INTERP=1 ...                                         # INFECTED_INTERP_ONLY=all instead of lean
GL=real ... INFECTED_SCREENSHOT_DIR=d INFECTED_SCREENSHOT_FLIPS=a,b,c    # PNGs (llvmpipe, about 25 flips/s)
MS0=data_X/ms0 ...                                   # start from the memory stick another run saved (relaunch from a save)
```

Outputs: `private/logs/NAME.{out,err}`, data dir `private/data_NAME` (the save is `ms0/PSP/SAVEDATA/ULUS100540000/INFECTED.BIN`).

Bot parameters (`INFECTED_BOT_PARAMS=k=v,...`): `approach` (walk towards the target while farther, default 140), `range`, `cone`, `lock`/`lock2`
(hold R while weakening / finishing), `prim` (default CROSS = pistol), `sec` (SQUARE = viral gun), `pulse`, `pulse2`, `back`, `phase`
(shifts the menu-press timing: differently timed, equivalent runs), `retry` (results screen: 1 = RETRY, 0 = CAMPAIGN), `verbose`.

What it does: menus / briefing / tutorial dialogs (engine mode word 0x08AACAFC != 1: TRIANGLE then CROSS), nearest infected by body position,
turns with the left stick, walks to `approach`, pistol pulses until the infected is red (hp 0, state bit 0x100), then SQUARE with R held
(one viral shot kills every red infected near the aim point); with nothing to shoot it walks +x and, when blocked, turns randomly (crude explorer).
It cannot: path around obstacles, rescue civilians, pick up items, buy upgrades, use the shotgun, plan combos, or navigate zone changes on purpose.

## Observer (read-only, `INFECTED_BOT_OBSERVE=1` or `INFECTED_BOT=1`)

`[bot] event mission_complete flip N n K score S kills K ground_combos G best_combo B viral_shots V seconds T hazard H medal M deaths D`,
`event results_screen`, `event player_dead / player_revived`, `event stats_object` and every 600 flips
`stats flip F emu_us U score .. kills .. shots .. seconds .. medal .. heap_top <free bytes of the dlmalloc top chunk> npc_live N npc_cap C`.
Score board object: `*(u32*)0x08ACE588` (vtable word 0x08ACE95C at +0x1C), statistics block at +0x128 (score, +0xC kills, +0x10 ground combos,
+0x14 best combo, +0x20 viral shots, +0x24 seconds, +0x28 hazard pay, +0x2C medal, 0xFFFFFFFF while running).

Other probes: `INFECTED_BOT_TABLE=N` (player and every infected every N flips), `INFECTED_BOT_PROBE=flip,..` (+`_PTRS=1`), `INFECTED_BOT_SNAP=flip,..` with
`INFECTED_BOT_SNAP_DIR` (24 MB RAM dump), `INFECTED_BOT_HOOKS=hexaddr,..` (log calls through the function table, e.g. 0881F6FC = Npc::TakeDamage).

## Regression asset

`mission1_win.pad` is the pad log of one complete mission-1 win (flip-indexed steps, written by `INFECTED_BOT_RECORD=file` while the bot played with
`phase=0`): the mission is won at flip 9289, the results screen is accepted on CAMPAIGN and the save is written by flip 9700. `mission1_win.expect` holds the outcome.

```
tools/bot/replay_check.sh [PADFILE [EXPECTFILE]]     # replays without the bot, checks the outcome, exit status 0 = OK
```

It checks: mission complete at flip 9289, score 21500, 39 kills, 34 viral shots, 100 s, hazard pay 1875, silver medal, no deaths, results screen at flip 9469,
and the save file (mission 1 score u16 at 0x3283 = 21500, hazard pay u16 at 0x329a = 1875). A pad log with every ninth step removed fails it.

Vita: the file is already in the format of the Vita app (`pad_script_file=pad.script`; its loader drops `#` lines and joins the rest with commas, checked to give the
same outcome as the desktop file reader). Copy it to the app data directory as `pad.script` and use `vita-config.txt` (virtual clock keys). The run only
reproduces exactly if the build is bit-exact with the x86 one; otherwise compare the outcome (score 21500 in the save, `STRICT=0` for the desktop script).
The game is time based, so the real-time clock (the Vita default) will not reproduce the flip-exact run.

Other scripts: `m1_runs.sh` (mission 1 for several phases), `sweep.sh` (parameter sweeps), `soak_report.py` (soak summary), `sheet.py` (contact sheets),
`funcs.py`/`d.sh`/`dumpdiff.py`/`tab.py`/`gdiff.py` (disassembly and memory-diff helpers used for the reverse engineering).
