cmake_minimum_required(VERSION 3.20)
project(Infected VERSION 0.1.0 LANGUAGES CXX)

# PortableKit checkout (a submodule in a real port repository).
set(INFECTED_PORTABLEKIT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/PortableKit" CACHE PATH "PortableKit checkout with patches/portablekit applied (scripts/get-portablekit.sh)")
add_subdirectory("${INFECTED_PORTABLEKIT_DIR}" portablekit_fw)

portablekit_add_game(InfectedNative
    PROFILE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    SOURCES host/infected_profile.cpp host/infected_tuning.cpp host/infected_bot.cpp host/lean_count.cpp)

# The lean recompiled corpus (recomp/lean_gen.py + recomp/build_lib.sh x86 -> liblean_x86.a). Linked in
# whole: the runtime refers to g_lean_entry only weakly (it also links without a corpus), which would not
# pull the archive member out, so the two table symbols are made undefined on the link line.
# Without INFECTED_LEAN_LIB the binary is the interpreter-only one.
set(INFECTED_LEAN_LIB "" CACHE FILEPATH "liblean_x86.a, the lean corpus built by recomp/build_lib.sh")
if(INFECTED_LEAN_LIB)
    message(STATUS "InfectedNative: lean corpus ${INFECTED_LEAN_LIB}")
    target_link_libraries(InfectedNative PRIVATE
        "-Wl,-u,g_lean_entry" "-Wl,-u,g_lean_entry_count" "${INFECTED_LEAN_LIB}")
    target_compile_definitions(InfectedNative PRIVATE INFECTED_HAS_LEAN=1)
    set_property(TARGET InfectedNative APPEND PROPERTY LINK_DEPENDS "${INFECTED_LEAN_LIB}")
endif()
