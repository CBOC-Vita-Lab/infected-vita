# Console test plan for infected_clock.skprx (500 MHz)

Not yet run on a console. Firmware 3.60-3.74 only; everything else must show the fallback.

1. Install the VPK. Start the game with the settings menu CPU clock at 500 MHz (the default). No PSVshell profile for INFC00001.
2. `ux0:data/infected/log.txt` must hold, in this order:
   - `clock: loading ux0:app/.../module/infected_clock.skprx (arm=500 pid=N stale=0)`
   - `clock module (start): clock: register reads multiplier 0x0000000E (or 0x8), divisor 0x00000000` (the 444 MHz step)
   - `clock module (start): clock: multiplier 14 -> 15, reads 15` (or `8 -> 9, reads 9`)
   - `clock: the 500 MHz module is loaded (id 0x...)`
   - `settings: the ARM clock runs at 49x MHz (measured; the system reports 444; raised by the package's clock module)`
   The settings menu (corner tap) reads `500 MHz` on the CPU clock row.
3. Restore, clean: in the menu set CPU clock to 444 MHz.
   - `clock module (stop): clock: module stop: multiplier 15 -> 14 (restored), reads 14`
   - `settings: the ARM clock is back at 44x MHz (measured ...)`
   Set it to 500 again: the module loads again. Quit the game (PS button, close): the same stop lines appear in the log, and `clock500.pending` is not in `ux0:data/infected/`.
4. Restore, crash: with 500 on, kill the game from the PS-button task list (or force a crash). There is no log line from the game, but
   `ux0:data/infected/clock_module.txt` ends with `owner process killed` or `owner process exited: multiplier 15 -> 14 (restored)`.
   Start the game again: the log shows `clock: clock500.pending is there ... the last module log follows`, then the module tells what it found. Whatever the
   result, `scePowerGetArmClockFrequency` and the 444 MHz measurement after choosing 444 must agree.
5. Coexistence: with the PSVshellPlus INFC00001 profile active (clock locked at 500 before the game asks): the module log says
   `the register already reads 500 MHz (another plugin or profile): not touching it`, nothing is restored at quit.
6. Refusal: `config.txt` `clock_module=0` -> `clock: config.txt clock_module=0 ...`; the row reads `444 MHz (kept)` and the description says the system kept 444.
7. Home screen: with 500 running, press the PS button and come back. Within about 10 s the log may show
   `the ARM core measures NNN MHz, 500 was set: the system took it back; loading the module again`.

What to send back if it fails: log.txt, clock_module.txt, whether the console hung (a hang right after `loading` means the register map or write is wrong:
then delete `clock500.pending` and set `clock_module=0`).
