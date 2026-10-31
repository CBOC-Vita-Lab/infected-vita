# Infected for the PS Vita: the application (title INFC00001).
#   Normal use: vita/app/build.sh      (LiveArea assets -> configure -> build -> infected.vpk -> dist/)
#   Manual:     source vita/app/env.sh && cmake -S vita/app -B build/vita -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
#
# What it links, in one line: the PortableKit runtime (pk_runtime, from PORTABLEKIT_DIR) + the GLES2 renderer on
# vitaGL + the Infected profile + the lean recompiled game (liblean_vita.a, one cache entry to swap) + FFmpeg
# decoders (libavcodec/libavutil) + the platform layer in vita/platform/vita + this directory's application code.
cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VITASDK})
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
endif()

project(infected_vita C CXX ASM)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

get_filename_component(PROJ_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(VITA_TREE "${PROJ_ROOT}/vita")

# ---------------------------------------------------------------------------------------------------- cache options
if(EXISTS "${PROJ_ROOT}/third_party/PortableKit")
  set(_pk_default "${PROJ_ROOT}/third_party/PortableKit")
else()
  set(_pk_default "${PROJ_ROOT}/../PortableKit")
endif()
set(PORTABLEKIT_DIR "${_pk_default}" CACHE PATH "PortableKit checkout with patches/portablekit applied (scripts/get-portablekit.sh makes one in third_party/PortableKit)")
if(DEFINED ENV{VITA_SYSROOT})
  set(_sysroot_default "$ENV{VITA_SYSROOT}")
else()
  set(_sysroot_default "${VITA_TREE}/sysroot")
endif()
set(VITA_SYSROOT "${_sysroot_default}" CACHE PATH "sysroot with vitaGL, FFmpeg, vitaShaRK, SceShaccCgExt, taiHEN stubs, math-neon (vita/scripts)")
set(VITAGL_VARIANT "release" CACHE STRING "vitaGL library variant: release|debug|fast|store")
set(FFMPEG_VARIANT "nopt" CACHE STRING "FFmpeg variant: nopt | pt")
set(LEAN_LIB "" CACHE FILEPATH "the lean recompiled game library made from your own disc (recomp/build_lib.sh vita; build.sh --lean-lib FILE)")
if(NOT LEAN_LIB)
  message(FATAL_ERROR "LEAN_LIB is not set. The recompiled game library is made from your own copy of the game: see docs/BUILDING.md (build.sh --lean-lib FILE).")
endif()
set(VITA_ATTRIBUTE2 "12" CACHE STRING "param.sfo ATTRIBUTE2 (extended memory mode); 0 = retail memory")
set(VITA_TITLEID "INFC00001" CACHE STRING "title id")
set(VITA_APP_NAME "Infected" CACHE STRING "bubble name")
set(VITA_VERSION "01.00" CACHE STRING "version ##.##")
set(LIVEAREA_DIR "${CMAKE_BINARY_DIR}/livearea" CACHE PATH "generated LiveArea tree (contains sce_sys/)")
set(RETROFLOW_DIR "${CMAKE_BINARY_DIR}/retroflow" CACHE PATH "RetroFlow pictures (COVERS/Homebrew/<id>.png, BACKGROUNDS/Homebrew/<id>.png); the app copies them to ux0:data/RetroFlow/ on its first start when RetroFlow is installed")
set(NEWLIB_HEAP_MIB "176" CACHE STRING "_newlib_heap_size_user in MiB (see src/app_memory.cpp for the budget)")
set(INFECTED_TEXT_PAD 0 CACHE STRING "bytes of padding in .text (build.sh sets it so vita-elf-create finds room after the code segment)")
set(INFECTED_SHADER_CACHE_DIR "" CACHE PATH "a vitaGL shader cache (the shader_cache folder of a run: v2/v/XX/*.gxp, v2/f/XX/*.gxp) to ship in the VPK; it is copied to ux0:data/infected/shader_cache on the first start")
set(INFECTED_NO_GL OFF CACHE BOOL "build without the GL renderer (headless logging renderer only)")

if(VITAGL_VARIANT STREQUAL "release")
  set(VGL_SUFFIX "")
elseif(VITAGL_VARIANT MATCHES "^(debug|fast|store|profile)$")
  set(VGL_SUFFIX "_${VITAGL_VARIANT}")
else()
  message(FATAL_ERROR "VITAGL_VARIANT must be release|debug|fast|store|profile")
endif()
set(FFMPEG_ROOT "${VITA_SYSROOT}/ffmpeg-${FFMPEG_VARIANT}/ffmpeg-vita")
foreach(f "${VITA_SYSROOT}/lib/libvitaGL${VGL_SUFFIX}.a" "${FFMPEG_ROOT}/lib/libavcodec.a" "${FFMPEG_ROOT}/lib/libavutil.a" "${LEAN_LIB}")
  if(NOT EXISTS "${f}")
    message(FATAL_ERROR "missing ${f}")
  endif()
endforeach()

# ---------------------------------------------------------------------------------------------------- compile flags
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# -fshort-enums is the vitasdk default and the whole runtime is built with it; FFmpeg headers are only seen by the two decoder TUs
# (atrac_decoder.cpp, avc_decoder.cpp), which are compiled with -fno-short-enums (see below).
set(ARCH_FLAGS "-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${ARCH_FLAGS} -O2 -g")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${ARCH_FLAGS} -O2 -g -fno-math-errno")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -mfpu=neon -mfloat-abi=hard")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-q -Wl,--no-enum-size-warning")

# ---------------------------------------------------------------------------------------------------- PortableKit
set(PORTABLEKIT_BUILD_RUNTIME_LIBRARY ON CACHE BOOL "" FORCE)
set(PSPRECOMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PSPRECOMP_CCACHE OFF CACHE BOOL "" FORCE)
set(PSPRECOMP_BUILD_LOCK OFF CACHE BOOL "" FORCE)
# newlib hides the POSIX 2008 names (strnlen, ...) under -std=c++20 unless _GNU_SOURCE is set; g++ on Linux defines it itself.
add_compile_definitions(_GNU_SOURCE)
add_subdirectory("${PORTABLEKIT_DIR}" portablekit_fw EXCLUDE_FROM_ALL)
set(PK "${PORTABLEKIT_DIR}")

# pk_runtime holds hle_media.cpp / the decoders, which choose their backend by macro: the GLES2 renderer and FFmpeg.
set(PK_DEFS PORTABLEKIT_HAS_FFMPEG=1)
if(NOT INFECTED_NO_GL)
  list(APPEND PK_DEFS PORTABLEKIT_HAS_GL_RENDERER=1)
endif()
target_compile_definitions(pk_runtime PRIVATE ${PK_DEFS})
target_include_directories(pk_runtime PRIVATE "${FFMPEG_ROOT}/include" "${VITA_SYSROOT}/include")
# Source file properties are directory scoped: they must be attached to the directory pk_runtime lives in.
set_source_files_properties("${PK}/host/audio/atrac_decoder.cpp" "${PK}/host/movie/avc_decoder.cpp"
                            TARGET_DIRECTORY pk_runtime PROPERTIES COMPILE_OPTIONS "-fno-short-enums")
# The GE dump reader needs uncompress(); the SDK has no zlib, so a small inflate (src/tiny_zlib.cpp) stands behind a zlib.h shim.
set_source_files_properties("${PK}/host/gpu/ge_dump.cpp" TARGET_DIRECTORY pk_runtime PROPERTIES
                            COMPILE_DEFINITIONS "PORTABLEKIT_HAS_ZLIB=1" INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src/zlib_shim")
# psprecomp_core is a different target; its own warnings are not ours to fix.
target_compile_options(psprecomp_core PRIVATE -Wno-conversion -Wno-shadow -Wno-pedantic)

# ---------------------------------------------------------------------------------------------------- embedded NID table
set(NIDS_INC "${CMAKE_BINARY_DIR}/generated_nids/nid_table.inc")
set(NIDS_CSV "${PK}/configs/nids.csv")
if(EXISTS "${PROJ_ROOT}/port/config/nids.csv")
  list(APPEND NIDS_CSV "${PROJ_ROOT}/port/config/nids.csv")
endif()
string(REPLACE ";" "|" NIDS_INPUT "${NIDS_CSV}")
add_custom_command(OUTPUT "${NIDS_INC}"
  COMMAND ${CMAKE_COMMAND} "-DINPUT=${NIDS_INPUT}" "-DOUTPUT=${NIDS_INC}" -P "${PK}/tools/embed_nids.cmake"
  DEPENDS ${NIDS_CSV} "${PK}/tools/embed_nids.cmake" COMMENT "Embedding the NID table")

# ---------------------------------------------------------------------------------------------------- the application
set(APP_SOURCES
  src/app_main.cpp
  src/app_config.cpp
  src/app_log.cpp
  src/app_memory.cpp
  src/app_ui.cpp
  src/app_status.cpp
  src/app_pad_script.cpp
  src/app_fatal.cpp
  src/app_platform_hooks.cpp
  src/app_settings.cpp
  src/app_hd_movie.cpp
  src/app_iso.cpp
  src/app_install.cpp
  src/app_bench.cpp
  src/sha256_stream.cpp
  src/tiny_zlib.cpp
  src/app_glue.cpp
  src/text_pad.cpp)

set(GL_SOURCES)
if(NOT INFECTED_NO_GL)
  # The framework's list of the renderer (gl_renderer, shaders, overlay panel, mip chains, PNG, HD textures), minus its EGL platform file.
  set(_pk_gl ${PORTABLEKIT_GL_RENDERER_SOURCES})
  if(NOT _pk_gl)
    message(FATAL_ERROR "PORTABLEKIT_GL_RENDERER_SOURCES is empty: ${PK} is not a PortableKit with the GL renderer")
  endif()
  list(FILTER _pk_gl EXCLUDE REGEX "gl_platform_egl")
  foreach(f ${_pk_gl})
    list(APPEND GL_SOURCES "${PK}/${f}")
  endforeach()
  list(APPEND GL_SOURCES
    "${PK}/host/gpu/renderer.cpp"
    src/gl_platform_vita.cpp)
endif()

# renderer.cpp's default_window_title() reads this generated header (portablekit_add_game makes it for the desktop port)
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/generated_version/portablekit_build_label.hpp" CONTENT
"#pragma once
namespace portablekit { inline constexpr const char *kBuildLabel = \"vita\"; }
")

set_source_files_properties(src/text_pad.cpp PROPERTIES COMPILE_DEFINITIONS "INFECTED_TEXT_PAD=${INFECTED_TEXT_PAD}")   # only this file rebuilds when the pad changes

add_executable(infected
  ${APP_SOURCES}
  ${GL_SOURCES}
  "${PROJ_ROOT}/port/host/infected_profile.cpp"
  "${PROJ_ROOT}/port/host/infected_tuning.cpp"      # enemy density and view distance (GameProfile::frame_hook); the profile refers to it
  "${PROJ_ROOT}/port/host/infected_bot.cpp"         # the play-through bot (config.txt `bot=1`, bench=play); the tuning module installs and ticks it
  "${PK}/host/generated_stub.cpp"
  "${PK}/host/crypto_keys_builtin.cpp"
  "${NIDS_INC}")
target_compile_options(infected PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(infected PRIVATE ${PK_DEFS}
  NEWLIB_HEAP_MIB=${NEWLIB_HEAP_MIB} INFECTED_VITA_TITLEID="${VITA_TITLEID}"
  # Our vitaGL has patch 0003 (scripts/patches): the GL renderer's vgl_skip_redundant setting can switch it on.
  PORTABLEKIT_VITAGL_SKIP_REDUNDANT=1
  # and patch 0004: the HD textures' compressed mip chains are reserved at level 0 (no wait for the GPU between levels).
  PORTABLEKIT_VITAGL_RESERVE_MIPS=1)
if(VITAGL_VARIANT STREQUAL "debug")
  target_compile_definitions(infected PRIVATE INFECTED_VGL_DEBUG=1)
endif()
target_include_directories(infected PRIVATE
  "${PK}/host" "${PK}/third_party" "${PK}/include" "${PROJ_ROOT}/port/host"
  "${VITA_TREE}/platform" "${VITA_SYSROOT}/include"
  "${CMAKE_BINARY_DIR}/generated_nids" "${CMAKE_BINARY_DIR}/generated_version" src)

# the Vita platform layer (vitaGL, pad, audio pump, clocks, memory, files, log)
add_library(infected_platform_vita STATIC
  "${VITA_TREE}/platform/vita/platform_vita.cpp"
  "${VITA_TREE}/platform/vita/audio_vita.cpp"
  "${VITA_TREE}/platform/vita/gl_vita.cpp")
target_include_directories(infected_platform_vita PUBLIC "${VITA_TREE}/platform" "${VITA_SYSROOT}/include")
target_compile_options(infected_platform_vita PRIVATE -Wall -fno-exceptions -fno-rtti -Wno-unused-parameter)

target_link_directories(infected PRIVATE "${VITA_SYSROOT}/lib" "${FFMPEG_ROOT}/lib")
set(FFMPEG_EXTRA_LIBS "")
if(FFMPEG_VARIANT STREQUAL "pt")
  set(FFMPEG_EXTRA_LIBS pthread)
endif()
# The lean library: g_lean_entry is only a WEAK reference of the runtime (exec.cpp), which does not pull an archive member by itself,
# so the table is forced in with -u, and --whole-archive is not needed (every thunk is reached through the table).
target_link_options(infected PRIVATE "LINKER:-u,g_lean_entry" "LINKER:-u,g_lean_entry_count")
# INFECTED_LEAN_LTO: LEAN_LIB is the link-time-optimisation archive (build_lib.sh with LEAN_LTO=1). The link then compiles the
# game's code as one program, with the lean flags (no fused multiply-add: the guest has none) and two jobs. Only the lean
# objects carry GIMPLE; the rest of the application is linked as before.
option(INFECTED_LEAN_LTO "LEAN_LIB holds LTO objects" OFF)
if(INFECTED_LEAN_LTO)
  # -frandom-seed: without a fixed seed GCC seeds the LTO partitions from the clock/temporary names, and two links of the same
  # objects differ in a few hundred bytes of code (builder/kit proves the kit's link is byte-identical to this one).
  target_link_options(infected PRIVATE -flto=2 -flto-partition=balanced -frandom-seed=infected -O2 -fno-math-errno -ffp-contract=off
                      -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb)
endif()
# libgcc and libstdc++ decide whether the program has threads by a WEAK reference to pthread_cancel (gthr-posix.h,
# __gthread_active_p). Nothing references it strongly, so in a static link it stayed 0 and the C++ runtime ran
# single-threaded: std::mutex locked nothing, shared_ptr counts were not atomic, and thread_local (emulated TLS) was one
# variable for every thread, which made the split renderer run every GL call on its walker. -u pulls it in.
# The other pthread functions the C++ runtime calls are weak references too, and a call to an undefined weak function
# links as a call to nothing: with pthread_once left out, the first guarded static (__cxa_guard_acquire) dereferenced a
# mutex that was never made. Every one of them is pulled in, and the build fails if one is still weak and undefined.
target_link_options(infected PRIVATE "LINKER:-u,pthread_cancel" "LINKER:-u,pthread_once" "LINKER:-u,pthread_cond_wait"
  "LINKER:-u,pthread_cond_broadcast" "LINKER:-u,pthread_cond_signal" "LINKER:-u,pthread_cond_timedwait"
  "LINKER:-u,pthread_getspecific" "LINKER:-u,pthread_setspecific" "LINKER:-u,pthread_key_create"
  "LINKER:-u,pthread_key_delete" "LINKER:-u,pthread_mutex_init" "LINKER:-u,pthread_mutex_destroy"
  "LINKER:-u,pthread_mutex_lock" "LINKER:-u,pthread_mutex_trylock" "LINKER:-u,pthread_mutex_unlock"
  "LINKER:-u,pthread_cond_init" "LINKER:-u,pthread_cond_destroy" "LINKER:-u,pthread_create" "LINKER:-u,pthread_join"
  "LINKER:-u,pthread_detach" "LINKER:-u,pthread_equal" "LINKER:-u,pthread_self" "LINKER:-u,sched_yield"
  "LINKER:-u,pthread_mutexattr_init" "LINKER:-u,pthread_mutexattr_settype" "LINKER:-u,pthread_mutexattr_destroy")
add_custom_command(TARGET infected POST_BUILD
  COMMAND sh -c "if ${CMAKE_NM} \"$1\" | grep -E '^ +w (pthread_|sched_)'; then echo 'error: pthread functions above are weak and undefined: add them to the -u list (vita/app/CMakeLists.txt)'; exit 1; fi" sh "$<TARGET_FILE:infected>"
  VERBATIM)
target_link_libraries(infected PRIVATE
  pk_runtime
  infected_platform_vita
  "${LEAN_LIB}"
  pk_runtime            # the runtime and the lean library reference each other (g_cpu, pk_hle_call <-> thunks)
  psprecomp_core
  vitaGL${VGL_SUFFIX}
  vitashark
  SceShaccCgExt
  taihen_stub
  mathneon
  avcodec
  avutil
  ${FFMPEG_EXTRA_LIBS}
  pthread
  SceGxm_stub
  SceShaccCg_stub
  SceDisplay_stub
  SceCtrl_stub
  SceTouch_stub
  SceAudio_stub
  ScePower_stub
  SceSysmodule_stub
  SceVideodec_stub
  SceCommonDialog_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceKernelModulemgr_stub
  SceKernelDmacMgr_stub
  SceIofilemgr_stub
  SceRtc_stub
  SceProcessmgr_stub
  m)

# ---------------------------------------------------------------------------------------------------- package
set(sfo_flags "-d PARENTAL_LEVEL=1 -d ATTRIBUTE2=${VITA_ATTRIBUTE2}")
set(VITA_MKSFOEX_FLAGS "${sfo_flags}" CACHE STRING "vita-mksfoex flags" FORCE)
set(SHADER_CACHE_FILES "")
if(INFECTED_SHADER_CACHE_DIR AND IS_DIRECTORY "${INFECTED_SHADER_CACHE_DIR}")
  file(GLOB_RECURSE _gxp RELATIVE "${INFECTED_SHADER_CACHE_DIR}" "${INFECTED_SHADER_CACHE_DIR}/*.gxp")
  foreach(f ${_gxp})
    list(APPEND SHADER_CACHE_FILES FILE "${INFECTED_SHADER_CACHE_DIR}/${f}" "shader_cache/${f}")
  endforeach()
  list(LENGTH _gxp _gxp_count)
  message(STATUS "shader cache: ${_gxp_count} compiled shaders from ${INFECTED_SHADER_CACHE_DIR} go into the VPK")
endif()
# The list of shader programs the game draws with (renderer flags, one per line; gl_renderer's prepare_variants()). Shipped as
# shader_cache/variants.txt, it lets a first start compile and link every program while the game boots instead of at the
# first draw that needs it, which was a 40-90 ms stall each, a dozen of them in the first seconds of a mission.
set(INFECTED_SHADER_VARIANTS "${CMAKE_CURRENT_SOURCE_DIR}/shader_variants.txt" CACHE FILEPATH "the shader program list to ship (empty: none)")
if(INFECTED_SHADER_VARIANTS AND EXISTS "${INFECTED_SHADER_VARIANTS}")
  list(APPEND SHADER_CACHE_FILES FILE "${INFECTED_SHADER_VARIANTS}" "shader_cache/variants.txt")
  message(STATUS "shader variants: ${INFECTED_SHADER_VARIANTS} goes into the VPK as shader_cache/variants.txt")
endif()
set(RETROFLOW_FILES "")
foreach(kind COVERS BACKGROUNDS)
  string(TOLOWER "${kind}" _k)
  if(EXISTS "${RETROFLOW_DIR}/${kind}/Homebrew/${VITA_TITLEID}.png")
    list(APPEND RETROFLOW_FILES FILE "${RETROFLOW_DIR}/${kind}/Homebrew/${VITA_TITLEID}.png" "retroflow/${_k}.png")
  endif()
endforeach()
vita_create_self(infected.self infected UNSAFE)
# The package's own kernel module that lets helper threads use the fourth CPU core (vita/core3; loaded by the application itself).
set(CORE3_MODULE "${CMAKE_CURRENT_BINARY_DIR}/module/infected_core3.skprx")
add_custom_command(OUTPUT "${CORE3_MODULE}"
  COMMAND ${CMAKE_COMMAND} -E env "VITASDK=$ENV{VITASDK}" "VITA_SYSROOT=${VITA_SYSROOT}" "${VITA_TREE}/core3/build.sh" "${CMAKE_CURRENT_BINARY_DIR}/module"
  DEPENDS "${VITA_TREE}/core3/core3.c" "${VITA_TREE}/core3/exports.yml" "${VITA_TREE}/core3/build.sh"
  COMMENT "infected_core3.skprx")
add_custom_target(core3_module DEPENDS "${CORE3_MODULE}")
vita_create_vpk(infected.vpk ${VITA_TITLEID} infected.self
  FILE ${CORE3_MODULE} module/infected_core3.skprx
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${LIVEAREA_DIR}/sce_sys/icon0.png sce_sys/icon0.png
  FILE ${LIVEAREA_DIR}/sce_sys/pic0.png sce_sys/pic0.png
  FILE ${LIVEAREA_DIR}/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE ${LIVEAREA_DIR}/sce_sys/livearea/contents/logo0.png sce_sys/livearea/contents/logo0.png
  FILE ${LIVEAREA_DIR}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE ${LIVEAREA_DIR}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  ${RETROFLOW_FILES}
  ${SHADER_CACHE_FILES})
add_dependencies(infected.vpk-vpk core3_module)
