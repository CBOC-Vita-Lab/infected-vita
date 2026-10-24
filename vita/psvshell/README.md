# PSVshellPlus profile for Infected (INFC00001): CPU 500 MHz, GPU 222, bus 222, crossbar 166

`INFC00001` is the 28-byte profile that runs the CPU at 500 MHz and the GPU at 222 MHz for this game. It is byte for byte the profile
that was measured on the test Vita for another title (496 MHz measured, about 10 % less CPU time per frame than at 444); with this profile the game itself
measured 494 MHz. `gen_profile.py` writes it; `gen_profile.py --check FILE` reads one back. The game does not install it: you copy it.

```
50 53 56 53 30 33 30 30   "PSVS0300"
0b 00 00 00               lock: CPU + GPU + crossbar. The game's own scePowerSet*ClockFrequency calls are ignored.
f4 01 00 00               CPU       500 MHz
de 00 00 00               GPU       222 MHz
de 00 00 00               bus       222 MHz (read by the shell, not applied)
a6 00 00 00               crossbar  166 MHz
```

## Why a profile
A game may ask for at most 444 MHz. PSVshellPlus (a kernel plug-in plus a shell part, which you install yourself) can run one
title at 500 MHz: it applies the profile when that title comes to the front and the lock keeps the game's own requests from undoing it.
The game's own 500 MHz request (`perf.cpu_clock`, default 500) is therefore not what makes the clock 500 on that console; the profile is.

## Install
1. Copy `INFC00001` to `ur0:data/PSVshell/profiles/INFC00001`: with VitaShell or over FTP (keep a copy of any profile that is already there). No reboot.
2. Start the game from LiveArea.
3. Read `ux0:data/infected/log.txt`: the line `clocks: the ARM core measures NNN MHz (the system reports 444)` must read about 496-500. The
   system's own answer stays 444 under the plug-in; the measured number (a loop of dependent decrements, one cycle each) is the truth.
   A second line `settings: the ARM clock runs at NNN MHz (measured ...)` comes from the settings applier.

Remove: delete the file; the clock is back at 444 the next time the game starts. Heat and battery: 500 MHz is outside
Sony's specification, so it is the owner's choice. In-game, the menu's "CPU clock" row sets only the game's own request, which the lock ignores.

## Not verified
Nothing here has run on a console: the file's bytes are checked against the known-good profile, the measuring loop and the log lines build
into the Vita package, and the install command only exists in the test script.
