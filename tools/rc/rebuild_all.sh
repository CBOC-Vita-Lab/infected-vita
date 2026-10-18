#!/bin/bash
# desktop incremental build, settings/overlay ctest subset, then the Vita build (VPK + ELF into dist/)
cd /home/claude/ws/all
{ nice -n 10 cmake --build build/desktop -j2 2>&1 | tail -5; } > build/desktop_build6.log 2>&1
echo "desktop build rc=$?" >> build/desktop_build6.log
nice -n 10 proj/vita/app/build.sh --portablekit /home/claude/ws/all/PortableKit --lean-lib /home/claude/ws/integ/private/vita/liblean_vita.latest.a --shader-cache /home/claude/ws/all/private/shader_cache_new/shader_cache > build/vita_build7.log 2>&1
echo "vita build rc=$?" >> build/vita_build7.log
