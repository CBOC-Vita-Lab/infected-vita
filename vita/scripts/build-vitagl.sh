#!/usr/bin/env bash
# Build one vitaGL variant into $VITA_SYSROOT/lib/libvitaGL<suffix>.a
#   usage: build-vitagl.sh release|debug|fast|store
# Source tree: third_party/vitaGL (vitaGL at dca4b9d plus vita/scripts/patches/vitagl-*.patch; fetch-third-party.sh makes it). Each variant is built in its own copy.
# NOTE: GCC 15 defaults to -std=gnu23; vitaGL (C) needs -std=gnu17 (e.g. K&R-style () prototypes). Passed through CC.
set -euo pipefail
source "$(dirname "$0")/env.sh"
variant=${1:-release}

# --- flag sets (documented in VITAGL_NOTES.md section "Build flags") ---------------------------------------------
COMMON="NO_SPLASHSCREEN=1 HAVE_SHADER_CACHE=1 HAVE_VERTEX_LAYOUT_CACHE=1 NO_TEX_COMBINER=1"
# The two vitaGL speed options the port's own GL calls were checked against (VITAGL_NOTES.md section 1, "Speed options"): in
# every shipping-style variant (release, store, profile). INDICES_SPEEDHACK: the vertex streams keep their 16-bit index source
# (the port draws 16-bit indices only, never instanced). SAMPLERS_SPEEDHACK: no null check per texture unit (every program has
# one sampler, unit 0, or none). Not in debug (bring-up keeps the checks) and not in fast (measurement build, its own list).
SPEED="INDICES_SPEEDHACK=1 SAMPLERS_SPEEDHACK=1"
case "$variant" in
  release) FLAGS="$COMMON NO_DEBUG=1 $SPEED"; SUFFIX="";;
  debug)   FLAGS="$COMMON LOG_ERRORS=1 HAVE_SHARK_LOG=1 HAVE_PROFILING=1 DEBUG_THREAD_SAFENESS=1 DEBUG_GLSL_TRANSLATOR=1"; SUFFIX="_debug";;
  # release + unsafe draw hacks: no client-array copy, no vertex-layout cache, no samplers null checks, in-place buffer
  # updates, and scratch (circular-pool) memory for GL_STREAM_DRAW/GL_DYNAMIC_DRAW buffers (see VITAGL_NOTES.md)
  fast)    FLAGS="$COMMON NO_DEBUG=1 DRAW_SPEEDHACK=1 SAMPLERS_SPEEDHACK=1 BUFFERS_SPEEDHACK=1 USE_SCRATCH_MEMORY=1"; SUFFIX="_fast";;
  # release + depth/stencil kept between GXM scenes (needed when the port goes default-FB -> FBO -> default-FB and keeps
  # depth-testing / scissoring / stencilling afterwards). Costs one depth+stencil load/store per scene.
  store)   FLAGS="$COMMON NO_DEBUG=1 $SPEED STORE_DEPTH_STENCIL=1"; SUFFIX="_store";;
  # release + vitaGL's own per-30-frame profile (time in draw calls, time waiting for the GPU, vertex-program cache hits),
  # printed through vgl_hook_printf into log.txt. For measuring on the console only.
  profile) FLAGS="$COMMON NO_DEBUG=1 $SPEED HAVE_PROFILING=1 LOG_ERRORS=1"; SUFFIX="_profile";;
  *) echo "unknown variant $variant"; exit 2;;
esac
# debug: route sceClibPrintf (vgl_log + shark log) into our file logger (symbol vgl_hook_printf is in the platform layer)
CCX="arm-vita-eabi-gcc -std=gnu17 -I$VITA_SYSROOT/include"
CXXX="arm-vita-eabi-g++ -I$VITA_SYSROOT/include"
if [ "$variant" = debug ] || [ "$variant" = profile ]; then CCX="$CCX -DsceClibPrintf=vgl_hook_printf"; CXXX="$CXXX -DsceClibPrintf=vgl_hook_printf"; fi

dir=$VITA_WS/build/vitagl-$variant
rm -rf "$dir"; cp -r "$VITA_WS/third_party/vitaGL" "$dir"
cd "$dir"
nice -n 10 make -j${VGL_JOBS:-2} CC="$CCX" CXX="$CXXX" $FLAGS 2>&1 | tail -25
cp libvitaGL.a "$VITA_SYSROOT/lib/libvitaGL$SUFFIX.a"
cp source/vitaGL.h "$VITA_SYSROOT/include/vitaGL.h"
ls -la "$VITA_SYSROOT/lib/libvitaGL$SUFFIX.a"
echo "$variant: $FLAGS" > "$VITA_SYSROOT/lib/libvitaGL$SUFFIX.flags"
