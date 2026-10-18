# Infected PS Vita port workstream - top level CMake (vitasdk).
#   Normal use: vita/build.sh   (generates LiveArea assets, configures, builds, packages build/selftest/infected_selftest.vpk)
#   Manual:     source scripts/env.sh && cmake -S . -B build/selftest -DVITAGL_VARIANT=release && cmake --build build/selftest -j2
#
# Cache options
#   VITAGL_VARIANT   release | debug | fast | store   -> $VITA_SYSROOT/lib/libvitaGL[_debug|_fast|_store].a  (scripts/build-vitagl.sh)
#   FFMPEG_VARIANT   nopt | pt                         -> $VITA_SYSROOT/ffmpeg-<variant>/ffmpeg-vita          (scripts/build-ffmpeg.sh)
#   VITA_ATTRIBUTE2  integer for param.sfo ATTRIBUTE2: 0 = retail memory, 4/8/12 = extended memory +29/+77/+109 MiB (default 12)
#   LIVEAREA_DIR     directory with the generated sce_sys tree (vita/assets/gen_livearea.py writes it)
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK (source scripts/env.sh)")
  endif()
endif()

project(infected_selftest C CXX ASM)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_SYSROOT "${CMAKE_CURRENT_SOURCE_DIR}/sysroot" CACHE PATH "private sysroot with vitaGL / FFmpeg / ShaccCgExt (never the shared SDK)")
set(VITAGL_VARIANT "release" CACHE STRING "vitaGL library variant: release|debug|fast|store")
set(FFMPEG_VARIANT "nopt" CACHE STRING "FFmpeg variant: nopt (no threads) | pt (pthreads)")
set(VITA_ATTRIBUTE2 "12" CACHE STRING "param.sfo ATTRIBUTE2 (extended memory mode bits); 0 = retail memory, written explicitly")
set(VITA_TITLEID "INFT00001" CACHE STRING "title id")
set(VITA_APP_NAME "Infected Port Self-Test" CACHE STRING "bubble name")
set(VITA_VERSION "01.00" CACHE STRING "version ##.##")
set(LIVEAREA_DIR "${CMAKE_BINARY_DIR}/livearea" CACHE PATH "generated LiveArea tree (contains sce_sys/)")

if(VITAGL_VARIANT STREQUAL "release")
  set(VGL_SUFFIX "")
elseif(VITAGL_VARIANT MATCHES "^(debug|fast|store)$")
  set(VGL_SUFFIX "_${VITAGL_VARIANT}")
else()
  message(FATAL_ERROR "VITAGL_VARIANT must be release|debug|fast|store")
endif()
set(FFMPEG_ROOT "${VITA_SYSROOT}/ffmpeg-${FFMPEG_VARIANT}/ffmpeg-vita")
foreach(f "${VITA_SYSROOT}/lib/libvitaGL${VGL_SUFFIX}.a" "${FFMPEG_ROOT}/lib/libavcodec.a" "${FFMPEG_ROOT}/lib/libavutil.a")
  if(NOT EXISTS "${f}")
    message(FATAL_ERROR "missing ${f} - build it with scripts/build-vitagl.sh ${VITAGL_VARIANT} / scripts/build-ffmpeg.sh ${FFMPEG_VARIANT}")
  endif()
endforeach()

# GCC 15 defaults to gnu23 for C; the platform is C++20 without exceptions/RTTI (platform.h rules).
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -g -Wall -mfpu=neon -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -g -Wall -Wextra -Wno-unused-parameter -fno-exceptions -fno-rtti -mfpu=neon -mfloat-abi=hard")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -mfpu=neon -mfloat-abi=hard")
# --no-enum-size-warning: FFmpeg objects are 32-bit-enum, everything else short-enum; the two never share enum-typed data (t_ffmpeg.cpp is the only bridge)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-q -Wl,--no-enum-size-warning")

# ---------------------------------------------------------------------------------------------------------------- platform
add_library(infected_platform_vita STATIC
  platform/vita/platform_vita.cpp
  platform/vita/audio_vita.cpp
  platform/vita/gl_vita.cpp)
target_include_directories(infected_platform_vita PUBLIC platform "${VITA_SYSROOT}/include")

# ---------------------------------------------------------------------------------------------------------------- selftest
add_executable(infected_selftest
  selftest/st_harness.cpp
  selftest/st_console_vita.cpp
  selftest/t_sys.cpp
  selftest/t_audio.cpp
  selftest/t_coro.cpp
  selftest/coro_core.cpp
  selftest/coro_arm.S
  selftest/t_gl.cpp
  selftest/t_ffmpeg.cpp)
target_include_directories(infected_selftest PRIVATE selftest platform "${VITA_SYSROOT}/include" "${FFMPEG_ROOT}/include")
target_compile_definitions(infected_selftest PRIVATE ST_VARIANT="${VITAGL_VARIANT}")
target_compile_definitions(infected_selftest PRIVATE ST_FFMPEG_VARIANT="${FFMPEG_VARIANT}" ST_ATTR2=${VITA_ATTRIBUTE2})   # echoed on the T0 build line
if(VITAGL_VARIANT STREQUAL "fast")
  target_compile_definitions(infected_selftest PRIVATE ST_VGL_FAST=1)
endif()
if(FFMPEG_VARIANT STREQUAL "pt")
  target_compile_definitions(infected_selftest PRIVATE ST_FFMPEG_PT=1)
endif()
# FFmpeg headers are built for 32-bit enums; the vitasdk toolchain defaults to -fshort-enums. Only this TU sees FFmpeg types.
set_source_files_properties(selftest/t_ffmpeg.cpp PROPERTIES COMPILE_OPTIONS "-fno-short-enums")

target_link_directories(infected_selftest PRIVATE "${VITA_SYSROOT}/lib" "${FFMPEG_ROOT}/lib")
set(FFMPEG_EXTRA_LIBS "")
if(FFMPEG_VARIANT STREQUAL "pt")
  set(FFMPEG_EXTRA_LIBS pthread)
endif()
target_link_libraries(infected_selftest
  infected_platform_vita
  vitaGL${VGL_SUFFIX}
  vitashark
  SceShaccCgExt
  taihen_stub
  mathneon
  avcodec
  avutil
  ${FFMPEG_EXTRA_LIBS}
  SceGxm_stub
  SceShaccCg_stub
  SceDisplay_stub
  SceCtrl_stub
  SceAudio_stub
  ScePower_stub
  SceSysmodule_stub
  SceCommonDialog_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceKernelModulemgr_stub
  SceKernelDmacMgr_stub
  m)

# ---------------------------------------------------------------------------------------------------------------- package
# param.sfo: ATTRIBUTE2 selects the extended memory mode (UNVERIFIED on hardware, see vita/README.md). Flags are plain vita-mksfoex ones.
# ATTRIBUTE2 is ALWAYS written explicitly: vita-mksfoex's own default is ATTRIBUTE2=12 (0xC) (checked with vita-mksfoex + a SFO parser),
# so "omitting" the flag would silently give the extended-memory package even for --attr2 0.
set(sfo_flags "-d PARENTAL_LEVEL=1 -d ATTRIBUTE2=${VITA_ATTRIBUTE2}")
set(VITA_MKSFOEX_FLAGS "${sfo_flags}" CACHE STRING "vita-mksfoex flags" FORCE)

vita_create_self(infected_selftest.self infected_selftest UNSAFE)
vita_create_vpk(infected_selftest.vpk ${VITA_TITLEID} infected_selftest.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${LIVEAREA_DIR}/sce_sys/icon0.png sce_sys/icon0.png
  FILE ${LIVEAREA_DIR}/sce_sys/pic0.png sce_sys/pic0.png
  FILE ${LIVEAREA_DIR}/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE ${LIVEAREA_DIR}/sce_sys/livearea/contents/logo0.png sce_sys/livearea/contents/logo0.png
  FILE ${LIVEAREA_DIR}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE ${LIVEAREA_DIR}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml)
