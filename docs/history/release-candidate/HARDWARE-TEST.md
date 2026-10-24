# First run on a real Vita: the checklist

**Nothing in this build has run on a real Vita yet.** This list finds out whether it runs, how fast, and which options are safe.
The first two parts need about 20 minutes. The rest is optional, in the order that matters most.
Stop whenever something is clearly too slow or broken. Send what you have and say where you stopped.

## Before you start

1. Everything from `README.md` is installed: the VPK, `Infected.iso`, the two packs and the `dumps` folder in `ux0:data/infected/`.
2. The Vita is on its charger or fully charged. Screen brightness at the default. No other app open.
3. There is **no `settings.txt` and no `config.txt`** in `ux0:data/infected/` (delete them: that gives the defaults).
4. **The log is rewritten every time you start the game.** After each run, copy `ux0:data/infected/log.txt` to a new name
   (for example `log_1_default.txt`, in VitaShell: Triangle menu, Copy, then Paste and Rename) **before you start the game again.**
5. Leave the game with **START + SELECT + L + R** held for about one second.

## Part A. First run, default settings (about 5 minutes)

1. Start **Infected**. Skip the movies with CROSS. The first time, the game asks for an officer name: enter one and accept it (a new profile is made).
2. Choose Campaign, then **mission 1**. Play for **about one minute** by hand.
3. Open the settings menu once (hold SELECT + START for half a second) and close it with CIRCLE. Check that the game goes on.
4. Open it again, set **HD textures** (first row) to **Off**, close it, and play **30 seconds**. Then open it, set HD textures back to **2x pack**, close it, play 30 seconds more.
   (This is the one live change that crashed the Vita3K emulator twice. See `KNOWN-ISSUES.md`. If the game freezes or the Vita restarts here, write down which step it was.)
5. Leave the game. Copy the log to **`log_1_default.txt`**.

Write down, in a few words: Did it start? Did it look right (no black or white boxes, no flicker)? Was it smooth or stuttering?
Was the sound clean? Did the touch-screen corner tap open the menu (try it once)?

## Part B. The speed test with the bot (about 4 minutes per run)

The bot plays mission 1 for you, so every run is the same and you do not have to hold the Vita. It presses the buttons itself.
**Your own buttons do nothing while the bot plays, except the way out (START + SELECT + L + R).**

**Get a clean profile first.** The bot needs a profile and starts mission 1 from it. Do this once:

1. In VitaShell rename the folder `ux0:data/infected/ms0` to `ms0_mine` (that is your own save, so it is safe).
2. Copy the folder `bench_profile_ms0` from this package's `ux0_data_infected` into `ux0:data/infected/` and rename it to `ms0`.
   (If there is no `bench_profile_ms0` in your copy, skip this: make a new profile by hand, as in part A, and do not finish mission 1.)

Now make a text file called **`config.txt`** in `ux0:data/infected/` with this one line:

```
bench=play
```

(Easiest: edit `config.txt.example` on your PC, remove the `#` in front of `bench=play`, save it as `config.txt`, copy it over.)

Start the game. Do nothing. It plays the intro, starts mission 1, plays it, and **exits by itself** when the mission is over (or after 7 minutes of play).
The log gets one line every 10 seconds and one **FINAL** line. They look like this:

```
[play] t=  20s flips=598 fps=59.8 low1%=41.2 | guest=8.9ms ge=63% swap=3.1ms | draws/f=640 | infected=4(avg 3.1 max 4) deaths=0 | HD pack in use=2x; settings: all defaults
```

* **fps**: pictures per second on average. 60 is full speed.
* **low1%**: the speed of the slowest 1 percent of pictures. Low numbers mean stutter.
* **guest**: milliseconds per picture spent in the game's own code. **ge**: how busy the drawing core is (over 100% means the graphics are the limit). **swap**: milliseconds waiting for the screen.
* **draws/f**: drawing commands per picture. **infected**: enemies alive now (average and most).

Copy the log to **`log_2_bench_default.txt`**.

### The costly options, one at a time

For each row below: put the **two lines** into `config.txt` (replace what was there), start the game, let it finish, copy the log to the name shown.
A `config.txt` line decides for the whole run and is never saved. So `settings.txt` stays clean.

| Run | Lines in `config.txt` | Log name |
| --- | --- | --- |
| 3 | `bench=play` and `gfx.hd_textures=off` | `log_3_hd_off.txt` |
| 4 | `bench=play` and `gfx.hd_textures=4x` | `log_4_hd_4x.txt` |
| 5 | `bench=play` and `gfx.pixel_lighting=on` | `log_5_pixel_light.txt` |
| 6 | `bench=play` and `gfx.antialiasing=fxaa` | `log_6_fxaa.txt` |
| 7 | `bench=play` and `gfx.antialiasing=msaa4` | `log_7_msaa4.txt` |
| 8 | `bench=play` and `perf.enemy_density=200` | `log_8_density200.txt` |
| 9 | `bench=play` and `perf.view_distance=200` | `log_9_view200.txt` |
| 10 | `bench=play` and `gfx.effects_scale=2` | `log_10_effects2.txt` |
| 11 | `bench=play` and `perf.threaded_renderer=off` | `log_11_one_core.txt` |

The `[play]` line says which settings were in effect, so you can check that the right one was used.

When you are done, **put your own save back**: delete `ux0:data/infected/ms0`, rename `ms0_mine` back to `ms0`.

## Part C. The graphics-only speed test (about 3 minutes)

This replays 100 recorded pictures of mission 1 through the graphics code only, with no game logic. It shows what the graphics chip can do.
Make `config.txt` with these three lines:

```
bench=ge_replay
bench_flips=300
bench_report=50
```

Start the game. It shows the recorded pictures and stops after 300. Copy the log to **`log_12_ge_replay.txt`**.
The lines to look at start with `[bench] ge_replay`. The first window includes shader building, so read the later ones.

## Part D. Whole-game timing, if you want it (about 3 minutes)

`config.txt`:

```
bench=cpu
bench_flips=1800
```

The game runs normally for 1800 pictures and prints a table of where the time went. Copy the log to **`log_13_cpu.txt`**.

## What to send back

* **All the `log_*.txt` files.** That is the main thing.
* `ux0:data/infected/settings.txt`, if it exists.
* A few lines from you: for part A, the answers to the questions above. For each run in part B, anything you saw: stutter, wrong colours, a freeze, a crash, an error screen (and its last lines).
* If the game does not start at all: the log, and what the screen showed.

If a run crashes or the Vita restarts, still send the log: it is written about once a second, so it shows how far the game got.
