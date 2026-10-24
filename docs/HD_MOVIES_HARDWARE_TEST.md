# HD movies on a real Vita: the test, step by step

Written for the owner, not for a programmer. Nothing about the HD movies has run on a real console yet: the emulator (Vita3K) shows that the pieces fit, not that the console agrees.
This sheet answers three questions, in this order:

1. **Does the console's video decoder start?** (The emulator needed a workaround for this one, the console should not.)
2. **Is the picture right?** Colours like the original movie, upright, no green or purple tint, black is black.
3. **Does it keep up?** The movies run at 29.97 pictures per second; the console has to produce every one of them in time while the game is running.

Time: about 25 minutes for the first run, 5 minutes for each extra run. Vita on AC power or a full battery, no other application running.
If you only have ten minutes, do **Run 1** and send back its log: it already answers all three questions.

## What you need

1. The port installed as in `INSTALL.md` (libshacccg, the ISO in `ux0:data/infected/`). Use a build from the same source tree as this sheet.
2. The movies: the folder **`movies_hd`** (18 movies and a file `index.txt`, 413 MB in all) copied to **`ux0:data/infected/movies_hd/`**. It is made by `tools/hdmovies/build_movies.sh` (the finished set
   is in `private/hdbuild/movies_hd/`; `MANIFEST.txt` next to it lists every file with its size and checksum, so a damaged copy can be found). You need about 420 MB free on `ux0:`.
   Copy the **whole folder**: the movies play only if `index.txt` and the movie files are both there.
3. A text file `ux0:data/infected/config.txt`. Each run below tells you what to put in it. The easiest way: write the lines on your computer, call the file `config.txt`, copy it to the Vita with VitaShell.
   Copying a new file over the old one is how you change from one run to the next. **No `settings.txt` from earlier runs** (delete it, so that everything else is at the defaults).
4. The log is **rewritten at every start of the game**. After each run, copy `ux0:data/infected/log.txt` to `log_<name>.txt` with VitaShell before you start the game again.
5. The menu: hold **SELECT + START** for half a second. UP/DOWN chooses the row, LEFT/RIGHT changes it, CIRCLE closes. Leave the game with **START + SELECT + L + R** held for a second.

## Run 1: everything at its default (log_m1_default.txt)

`config.txt` for this run (two lines):

```
gfx.hd_movies=on
hd_movie_probe=1
```

(`hd_movie_probe=1` makes the log say a little more about the picture, in case it looks wrong. It costs nothing you could notice.)

Start the game. **Do not press anything.** The three logos play (about 6 seconds each), then the intro (67 seconds), then the title screen. When the title screen has been there for 10 seconds, leave the game
and copy the log.

### Question 2, on the screen

Look at the logos and the intro. In the intro the old man in the grey suit with white hair is on the screen at about 18 to 28 seconds.

| What you see | What it means |
| --- | --- |
| Looks like the movies always did, only a bit cleaner and sharper | **good**: the default is right |
| Skin looks blue or grey, the orange jacket looks blue-green (red and blue are swapped) | try **Run 2** (`hd_movie_chroma=vu`) |
| Colours are the right colours but look flat and washed out, or too strong and too contrasty, black is grey | try **Run 3** (`hd_movie_csc=1`) |
| Mostly green or pink, with the shape of the picture in it, or colour smeared over the picture | try **Run 4** (`hd_movie_layout=planar`) |
| The picture is upside down, mirrored, shifted, striped or flickers | no setting for this: send the log and a photo of the screen |
| Same as the old movies: not sharper, not different | the HD movie was not used: look in the log (below) |
| Black screen during the movies, or the game stops | send the log, say when it happened |

Also listen: the sound must stay in step with the picture from the first second to the last.

### Question 1 and 3, in the log

Open `log_m1_default.txt` in any text editor (or VitaShell) and search for `hdmovie`. These are the lines, in the order they appear:

| The line | What it tells you |
| --- | --- |
| `hdmovie: hardware source registered` | the port is ready (always there when HD movies are on) |
| `[hdmovie] 18 replacement(s) in ux0:data/infected/movies_hd` | the movies were found (the number is how many files were found; fewer than 18 = the copy is incomplete; **no such line at all = the folder `movies_hd` or its `index.txt` is not where the game looks**) |
| `hdmovie: memory before the first picture, decoder not set up: phycont_free=... KiB cdram_free=... KiB user_free=... KiB, 0 phycont blocks of the decoder` | the memory numbers before the first movie |
| **`hdmovie: hardware decoder ready: decoder ... KiB + stream 1024 KiB + 12 frames of 768 KiB in phycont; pixel type packed, chroma UV, GXM format YVU420P2_CSC0`** (and a `memory with the decoder set up` line after it) | **Question 1 answered yes**: the console's decoder started. Please write down the number after "decoder". |
| `hdmovie: hardware decoder unavailable: sceSysmoduleLoadModule(AVCDEC) failed (0x...)` (or another reason after the colon) | **Question 1 answered no.** The movies play from the disc as before, nothing is damaged. Send the log, with the number in brackets. |
| `hdmovie: picture N could not be decoded; the original shows pictures N to M` | one picture could not be decoded by the console; the original movie shows it and the pictures that depend on it, then the HD movie goes on. Write down how many such lines there are. |
| **`[hdmovie] session 4 ended: intro: 2010 replacement pictures, 0 original (67.07 s, 29.97 pictures/s, longest wait 41 ms), 2010 decoded, 0 seek(s), decode ... ms CPU a picture`** | **Question 3**: see below |
| `hdmovie: memory after the last movie, decoder given back: phycont_free=... KiB ..., 0 phycont blocks of the decoder` and `[hdmovie] frame N: the replacement pictures are released` (only with `trace_hdmovie=1`) | the decoder was given back: the first number must be the same as `phycont_free` in the line "before the first picture", and the block count is 0 |

**Question 3: does it keep up?** On the "ended" line of every movie:

* **`N replacement pictures, 0 original`** (the numbers before the bracket) where N is the number of pictures of the movie (184, 185, 180 for the logos, 2010 for the intro): every picture was the HD one. Some "original" pictures
  are not a failure, the movie just fell back for them (the log says why), but write the numbers down.
* **`pictures/s` in the bracket**: **29.9x** is good (the movie's own rate is 29.97). **Below 29**: the console is too slow for the intro, the movie lasts longer than its sound, or the sound runs ahead of the picture. Send the log.
* **`longest wait`** in the bracket: the longest time between two pictures. Up to about 70 ms is fine (a picture is 33 ms; a pause in the menu would show up here as a long time, so do not touch the console during this run).
  Over 150 ms is a stutter you could have seen.
* **`decode ... ms CPU a picture`**: for information (the emulator decodes in software and shows about 3 ms; the console does the work in hardware and should show less).

### Question 1, the memory

Two lines say `phycont_free=`: before the first movie and after the last one. They must be **the same number** (give or take 4 KiB). If the number after is smaller by about 13 MiB (13312 KiB), the decoder was not given back:
send the log. (The decoder, its 12 picture slots and its stream buffer take 13 MiB of the special memory that only the video decoder uses. The game's textures do not live there.)

## If the picture was wrong: Runs 2, 3 and 4 (one setting each)

Put these lines in `config.txt` (replacing the previous ones), start the game, watch the logos and the first 30 seconds of the intro, leave, copy the log under the name shown.

| Run | Log name | `config.txt` |
| --- | --- | --- |
| 2: red and blue swapped | `log_m2_chroma_vu.txt` | `gfx.hd_movies=on` `hd_movie_probe=1` `hd_movie_chroma=vu` (three lines) |
| 3: washed out or too contrasty | `log_m3_csc1.txt` | `gfx.hd_movies=on` `hd_movie_probe=1` `hd_movie_csc=1` (three lines) |
| 4: green, pink or smeared | `log_m4_planar.txt` | `gfx.hd_movies=on` `hd_movie_probe=1` `hd_movie_layout=planar` (three lines) |

One line each, as in Run 1. If one of them makes the picture right, **that is the answer**: write down which, and send the logs of the wrong and the right run.
If none does, send all the logs and a photo of the screen of each wrong picture. Do not combine them on your own unless Run 2, 3 or 4 got you close: the combination `hd_movie_layout=planar` and
`hd_movie_chroma=vu` is the one extra worth trying after Run 4.

## Run 5: with the game running (log_m5_play.txt)

The first runs show the movies on their own. This one shows them next to everything else the game does. `config.txt` has the line that worked (`gfx.hd_movies=on` and any of the lines from Runs 2 to 4 that you needed), plus
(only if you have the 2x pack installed):

```
gfx.hd_textures=2x
```

Start the game, let the logos and the intro play, **start a new game and play the first mission for 10 minutes** (or until the first tutorial movie appears and ends). Any movie the game plays during that has
its own "ended" line in the log. Look at:

* The tutorial and news movies: the same three checks as before (colours, sound in step, `pictures/s` on the "ended" line).
* After a movie, the game must go on as before: the same frame rate (menu, `Show FPS` On), the textures as sharp as before the movie. The log has a line `[gl] textures: ... | hd 2x: resident ... KiB ... over budget N`
  every 30 seconds: **`over budget`** must not grow after a movie.
* The line `hdmovie: memory after the last movie, decoder given back` appears after each movie (about 2 to 3 seconds later), and `phycont_free` in it is again the number of "before".

## Run 6: what happens when you interrupt a movie (log_m6_interrupt.txt, 5 minutes)

Use the `config.txt` that worked, **but take the line `gfx.hd_movies=on` out of it** (a line in `config.txt` locks the menu row: the game then does not let you change it). Instead, switch it on once in the menu: start the game, wait for the title screen, open the menu, Graphics page, last row **HD movies**, **On**, close, leave the game; the game remembers it,
so the next start plays the movies in HD. Each of these must go on **without a freeze, a black screen or a crash**, and at the end of each the picture is simply the movie (HD or the original) again.

1. Start the game, press **CROSS** during the first logo: the next logo or the intro follows at once.
2. Next start: during the intro (after 10 seconds) press **CROSS**: the title screen follows.
3. Next start: during the intro open the menu (**SELECT + START**), wait 5 seconds, close it with CIRCLE: the picture goes on where it was, sound as well.
4. Next start: during the intro open the menu, set **HD movies** (Graphics page, last row) to **Off**, close the menu: the movie goes on with its old picture (not black). Open the menu again, set **HD movies** back to **On**, close: it goes on in HD (it may stop for a moment first: the decoder has to catch up from the last full picture, up to about 2 seconds of movie; that is normal, but write down if it was longer than a second).
5. Leave the game. In `log.txt` there must be no line with `fault` or `crash`; the lines `[hdmovie] session N ended` show for each of the movies you interrupted.

## What to send back

* All `log_m*.txt`, and `ux0:data/infected/config.txt` of the last run.
* A few lines: for Question 1 the word yes or no and the number after "decoder"; for Question 2 which run gave the right picture (or "none"); for Question 3 the `pictures/s` and `longest wait` of the intro; anything that looked or sounded wrong.
* If a screen looked wrong, a photo of it with a phone.

## What the settings are, in one line each

| Line in `config.txt` | What it is for |
| --- | --- |
| `gfx.hd_movies=on` | uses the HD movies (the same as the menu row Graphics, HD movies; delete the line or write `off` to go back) |
| `hd_movie_probe=1` | the log gets a few lines with numbers about the picture, to tell what is wrong with it without seeing it |
| `hd_movie_chroma=vu` | the decoder writes the colour bytes in the other order (swaps red and blue) |
| `hd_movie_csc=1` | the second colour conversion table of the graphics chip instead of the first |
| `hd_movie_layout=planar` | the decoder writes three separate planes instead of luma plus one interleaved colour plane |
| `hd_movie_ignore_load_error=1` | **do not use on the console.** Only for the emulator, whose video module cannot be loaded. |
