#!/bin/bash
# minimap_record.sh LEVEL FIRST-LAST [BIN] : plays the scripted flow on the desktop lean build with LEVEL (e.g. Mission_5A) seen through the
# mission-1 slot of a COPY of the ISO, and records the GE flips FIRST..LAST (consecutive) into private/mm/ge_LEVEL, plus the texture harvest
# (private/mm/tex_LEVEL) so the textures can be named. Only the copy of the ISO is modified. Delete private/mm/ge_* when done (50 MB or so).
set -u
cd /home/claude/ws/all
L=$1; R=$2; BIN=${3:-build/desktop/bin/InfectedNative}
M=private/mm; S=/home/claude/ws/integ/private
ISO=$M/swap.iso
[ -f $ISO ] || cp /home/claude/infected/input/Infected.iso $ISO
LV=PSP_GAME/USRDIR/IMAGE/levels
python3 -I /home/claude/proj/tools/texpack/iso_swap.py $ISO --restore > /dev/null
[ $L != Mission_1A ] && python3 -I /home/claude/proj/tools/texpack/iso_swap.py $ISO --redirect $LV/Mission_1A.stream.zpm $LV/$L.stream.zpm > /dev/null
D=$M/data_$L; rm -rf $D; mkdir -p $D $M/ge_$L $M/tex_$L
cp $S/data/EBOOT.ELF $D/; sed "s|^disc_image=.*|disc_image=$PWD/$ISO|" $S/data/settings.ini > $D/settings.ini
cp -a $S/ms0.pristine $D/ms0
export EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1
env INFECTED_DATA_DIR=$PWD/$D INFECTED_NO_AUDIO=1 INFECTED_LEAN_ONLY=1 INFECTED_LIBC_HOOKS=1 INFECTED_CLOCK=virtual INFECTED_NO_BUSY_CLOCK=1 \
    INFECTED_SPIN_INSNS=0 INFECTED_FRAME_LIMIT_US=16683 INFECTED_UNTHROTTLED=1 INFECTED_PAD_SCRIPT=@$PWD/proj/tools/tune/stab.script \
    INFECTED_GL_DISPLAY_SCALE=1 INFECTED_TEX_DUMP=$PWD/$M/tex_$L INFECTED_GE_DUMP=$PWD/$M/ge_$L INFECTED_GE_DUMP_FLIPS=$R \
    INFECTED_MAX_FRAMES=$(( ${R##*-} + 5 )) nice -n 10 timeout -s KILL 1500 stdbuf -oL $BIN > $M/rec_$L.out 2> $M/rec_$L.err
echo "exit=$?"
python3 -I /home/claude/proj/tools/texpack/iso_swap.py $ISO --restore > /dev/null
rm -rf $D
du -sh $M/ge_$L $M/tex_$L
