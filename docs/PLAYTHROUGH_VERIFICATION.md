# Play-through verification of the native port (bot runs)

Scope: ULUS10054 v1.02, the owner's dump, lean (statically recompiled) code on the re-implemented system layer, deterministic virtual clock
(`INFECTED_CLOCK=virtual INFECTED_NO_BUSY_CLOCK=1 INFECTED_SPIN_INSNS=0 INFECTED_FRAME_LIMIT_US=16683 INFECTED_UNTHROTTLED=1`).
Tools and how to run them: `tools/bot/README.md`. A flip is one `sceDisplaySetFrameBuf`; 60 flips are about one emulated second.

## Result

| Question | Answer |
| --- | --- |
| Does mission 1 "NEW MEAT" play end to end on lean code? | Yes, 4 of 4 bot runs (menu timing phases 0/17/53/81): complete at flip 9289 / 9755 / 9245 / 10661, 39 kills, score 20750..21500, bronze or silver medal. |
| Mission-complete sequence | last viral shot, "MISSION SUCCESS" banner (about 20 flips later), fade to black, MISSION STATS screen (counts up) 180 flips after the last kill, ACCEPT on CAMPAIGN, "SAVING", title/menu load, main menu with the new score, mission selection with MISSION 2 "WALK IN PARK" unlocked, cutscene, briefing, mission 2. |
| Save | `ms0/PSP/SAVEDATA/ULUS100540000/INFECTED.BIN` (65520 B) is rewritten at CAMPAIGN: mission 1 score at u16 0x3283, hazard pay u16 0x329a, mission 2 record at 0x33c5/0x33dc, totals at 0x5305/0x5317. A relaunch from that memory stick autoloads it, shows the score on the main menu and continues at mission 2. |
| Mission 2 | Completes (type "KILL 'EM ALL", 34 kills, silver). Mission 3 "UPTOWN HURL" needs both zones cleared through the Subway; the bot clears zone 1, finds the manhole by random walking, reaches zone 2 and dies there repeatedly, so it is not completed (bot skill). |
| Stuck corpses (tuning kill harness) | Harness artefact. Its lethal blow takes an infected to hp 0 with state bit 0x100: the red (weakened) state, which only a viral-gun hit finishes. See below. |
| Lean vs interpreter | Mission 1 from the title to the save and the mission-2 briefing: player table, every state transition and the save file are identical between lean and `INFECTED_INTERP_ONLY=all`. No generator bug was found, nothing changed in the generator. |
| Soak | 31 min emulated retry loop and 31 min campaign flow: no crash, hang, stall or `pk_fatal`; heap headroom flat; no audio gap during play. |
| Enemy density 150 / 200 | Mission 1 and 2 complete at both, counters correct (39 / 34 kills), no pool or heap overflow. |

## Facts worth keeping (relocated image, base 0x08804000)

* Controls: CROSS pistol (pulse it), SQUARE viral gun, R lock-on and strafe, TRIANGLE dismisses tutorial dialogs, CROSS dismisses the briefing, L map, CIRCLE speed boost.
* Pistol hit = 17 damage (`Npc::TakeDamage` 0x0881F6FC, vtable +0xF8, damage in $f12); infected hp 100; six hits give hp 0 and state byte +0x59 bit 0 (word 0x0502 / 0x1502) = red. A viral hit (a3 = 1, about 100 damage) kills red infected near the aim point; the NPC is removed 4..9 flips later.
* The cell cap is 4: four red infected keep the cap full, no new ones spawn, so a player that never lands a viral shot stalls the mission (this is the "stuck corpse" state).
* Engine mode word 0x08AACAFC: 1 running, 2 modal (tutorial dialog, briefing, results).
* Score board: `*(u32*)0x08ACE588` (vtable word 0x08ACE95C at +0x1C), statistics block at +0x128: score, +0xC kills, +0x10 ground combos, +0x14 best combo, +0x20 viral shots, +0x24 mission seconds, +0x28 hazard pay (0 until the end), +0x2C medal (0xFFFFFFFF while running). Mission 3 uses a different layout.
* Death: "YOU ARE DEAD", ghost, revive after about 300 flips; after the lives are used the mission rebuilds from the start.

## Stuck-corpse analysis

* Tuning harness `INFECTED_TUNE_KILL` calls the game's hit path 0x088A54F0 with a lethal damage and a3 = 1. The state it produces (0x0502 / 0x1502, hp 0) is the same the pistol produces, not death.
* Idle player plus harness (run e1): all four infected turn red and stay. Viral-only bot plus harness (e3): 13 of 13 red infected are removed 4..9 flips after the blow and mission 1 completes at flip 12365 with the harness still running.
* The tuning agent's scripted player (stab.script, run e4) lands some viral shots; the last removal is at flip 8367 and from then on its aim no longer reaches the four red infected: the reported "first ~14 removed, then stuck".
* Normal bot play (39 infected lives, state every 10 flips): every life ends removed, red to removed takes 10..110 flips (median 50), no infected remains at the end, counters advance to 39, the mission ends, the death animation (bodies thrown up, COMBO banner) plays.

## Not verified

* Civilian objectives (touch civilians, bring them to a save point, "Civilians Need Saving / Instantly Saved / Saves Required" records), the Subway objective beyond entering zone 2, the shotgun and upgrades, multiplayer. The bot has no navigation for them.
* Vita: the pad log and config keys are in the Vita format, but the replay was not run on a Vita; it is exact only if the ARM build is bit-identical to x86 under the virtual clock.
