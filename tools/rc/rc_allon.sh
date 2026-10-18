#!/bin/bash
# the bot plays mission 1 with EVERYTHING on (real GL software renderer, display scale 2 = the Vita screen), screenshots every 500 flips.
# Low priority: the box is shared. Result: private/logs/allon.err (events), private/shots_rc/allon/*.png, then sheet.py.
cd /home/claude/ws/all
export BOT_WS=/home/claude/ws/all BIN=/home/claude/ws/all/build/desktop/bin/InfectedNative
export INFECTED_SCREENSHOT_DIR=/home/claude/ws/all/private/shots_rc/allon
export INFECTED_SCREENSHOT_FLIPS=300,900,1500,2100,2700,3600,4100,4600,5100,5600,6100,6600,7100,7600,8100,8600,9100,9600,10100,10600
S='gfx.hd_textures=4x\ngfx.texture_filter=smooth\ngfx.mipmaps=on\ngfx.antialiasing=fxaa\ngfx.pixel_lighting=on\ngfx.effects_scale=2\ngfx.hd_movies=on\nperf.view_distance=200\nperf.enemy_density=200\nperf.threaded_renderer=on'
s=$(date +%s)
GL=real SCALE=2 PACKS=/home/claude/ws/all/private/packs_vita HDMOVIES=/home/claude/ws/hdmov/private/hdbuild/movies_hd SETTINGS="$S" \
  nice -n 9 proj/tools/bot/run.sh allon 9000 INFECTED_BOT=1 INFECTED_MAX_FRAMES=12000
echo "wall $(( $(date +%s) - s )) s"
