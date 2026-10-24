#!/bin/bash
# g1_live.sh : desktop (lean code) check of the two gameplay rows. The stab.script flow plays to mission 1 with the Vita's default settings (packs as installed on the Vita:
# texpack_2x.bin + the mixed pack as texpack_4x.bin), the settings menu opens at flip 6000 (pad chord), and
#   control : the menu only opens and closes (same pad script, same pictures flow)      -> private/live/shots/g1_control
#   change  : Performance page, Enemy density 200 %, View distance 200 %, closed         -> private/live/shots/g1_change
#   density : null-draw run to flip 17000 with the cell/pool probes, change at flip 6000 -> private/live/logs/g1_density.err
cd /work/ws/all
P=private/g1; PK=/work/ws/all/private/packs_vita
FL=5990,6025,6032,6034,6040,6070,6150,6300
case "$1" in
  control) PACKS=$PK PAD=$PWD/$P/pad_open6000.script OVR=$PWD/$P/menu_control.overlay FLIPS=$FL proj/tools/all_live.sh g1_control 3000 INFECTED_MAX_FRAMES=6320 INFECTED_TUNE_LOG=1 ;;
  change)  PACKS=$PK PAD=$PWD/$P/pad_open6000.script OVR=$PWD/$P/menu_change.overlay  FLIPS=$FL proj/tools/all_live.sh g1_change 3000 INFECTED_MAX_FRAMES=6320 INFECTED_TUNE_LOG=1 ;;
  density) PACKS=$PK PAD=$PWD/$P/pad_open6000_long.script OVR=$PWD/$P/menu_change.overlay  FLIPS=6025 proj/tools/all_live.sh g1_density 3300 INFECTED_MAX_FRAMES=${DFLIPS:-17000} INFECTED_GL_NULL_DRAW=1 INFECTED_TUNE_LOG=1 INFECTED_TUNE_CELLS=300 INFECTED_TUNE_POOLS=300 ;;
esac
