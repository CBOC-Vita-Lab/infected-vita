#!/bin/bash
# live_pair.sh : the scripted mission-1 run with the settings menu opened three times, threaded first, then not threaded.
# (With the virtual clock the threaded run's emulated time follows the renderer's real time while the guest waits for it, so its
# timeline varies from run to run: the flow presses CROSS often enough to find the game wherever it is.)
cd /work/ws/all
for m in 1 0; do
  THREADED=$m PAD=/work/ws/all/proj/tools/all_live_flow3.script FLIPS=4280,4400,4480,4600,4680,4780,4850 \
    proj/tools/all_live.sh live4_t$m 1800 INFECTED_MAX_FRAMES=4860 > private/live4_t$m.rc 2>&1
done
