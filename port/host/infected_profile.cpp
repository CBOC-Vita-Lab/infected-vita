// Infected (Planet Moon / Majesco, 2005), ULUS10054 v1.02, RenderWare 3.7 based.
//
// The disc carries PSP_GAME/SYSDIR/BOOT.BIN as an unencrypted PRX next to the
// encrypted EBOOT.BIN, so this profile points the installer at BOOT.BIN and no
// decryption key is involved.

#include "profile.hpp"
#include "infected_tuning.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <vector>

// Provided by the lean corpus (recomp/lean_gen.py's lean_table.cpp) when one is linked in: reads LEAN_NO_HOOKS
// ("memcpy,memset,strcmp" or "all") to switch the corpus's own native replacements of those functions off.
extern "C" void lean_init_hooks_from_env(void) __attribute__((weak));

namespace portablekit {
namespace {

// The game's C library copies and fills memory one byte at a time (a lb/sb loop
// at 16 instructions a byte), and under the interpreter that is almost half of
// everything it runs: a decoded movie frame alone costs about 4 million
// instructions to move. These replace the three hottest of them with the same
// result. The addresses are those of ULUS10054 v1.02's BOOT.BIN.
//   0x08A2F228  void *memcpy(void *dst, const void *src, size_t n)  forward byte copy, returns dst
//   0x08A2F2FC  void *memset(void *dst, int value, size_t n)        low byte of value, returns dst
// The instructions the loops would have run are added to the interpreter's
// count, so profiles and the CPU time model see what they saw before.
// Off by default; INFECTED_LIBC_HOOKS=1 turns them on (see patch_image).
constexpr std::uint32_t kMemcpy = 0x08A2F228u;
constexpr std::uint32_t kMemset = 0x08A2F2FCu;

void guest_memcpy(psprecomp::CorpusRuntime &corpus, psprecomp::AllegrexContext &ctx) {
    auto &rt = static_cast<psprecomp::Runtime &>(corpus);
    auto &memory = rt.memory();
    const std::uint32_t destination = ctx.gpr[4], source = ctx.gpr[5], length = ctx.gpr[6];
    // A forward byte copy whose destination lies inside its own source repeats a
    // pattern, which a block copy would not; those go byte by byte.
    const bool repeats = destination > source && destination - source < length;
    if (length != 0u && !repeats && memory.contains(source, length) && memory.contains(destination, length)) {
        std::vector<std::uint8_t> block(length);
        memory.copy_out(source, block);
        memory.copy_in(destination, block);
    } else {
        for (std::uint32_t i = 0; i < length; ++i) memory.store8(destination + i, memory.load8(source + i));
    }
    // The guest's loop runs 8 instructions a byte and 8 around it.
    psprecomp::interpreter_account_native(kMemcpy, 8u * static_cast<std::uint64_t>(length) + 8u);
    ctx.gpr[2] = destination;
    ctx.pc = ctx.gpr[31];
}

void guest_memset(psprecomp::CorpusRuntime &corpus, psprecomp::AllegrexContext &ctx) {
    auto &rt = static_cast<psprecomp::Runtime &>(corpus);
    auto &memory = rt.memory();
    const std::uint32_t destination = ctx.gpr[4], length = ctx.gpr[6];
    const auto value = static_cast<std::uint8_t>(ctx.gpr[5]);
    if (length != 0u && memory.contains(destination, length)) {
        const std::vector<std::uint8_t> block(length, value);
        memory.copy_in(destination, block);
    } else {
        for (std::uint32_t i = 0; i < length; ++i) memory.store8(destination + i, value);
    }
    // 6 instructions a byte and 7 around it.
    psprecomp::interpreter_account_native(kMemset, 6u * static_cast<std::uint64_t>(length) + 7u);
    ctx.gpr[2] = destination;
    ctx.pc = ctx.gpr[31];
}

void patch_image(psprecomp::Runtime &runtime, const psprecomp::Elf32Image &) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress"
    if (lean_init_hooks_from_env != nullptr) lean_init_hooks_from_env();
#pragma GCC diagnostic pop
    // Off unless INFECTED_LIBC_HOOKS is 1 (both), "memcpy" or "memset". They only
    // make the interpreter faster. With both on the end of the intro movie used
    // to hang: the movie player's drain loop waited for a free slot in its
    // two-deep frame ring, and the library handed the movie's last ring packets
    // back before the player had fetched the last audio blocks (the framework
    // now holds them until their units are taken; INFECTED_MPEG_EAGER_RELEASE=1
    // restores the old behaviour). It was a timing dependent hang the faster
    // loops happened to expose, not a fault of the hooks.
    const char *on = portablekit::env("LIBC_HOOKS");
    const std::string_view which = on != nullptr ? on : "";
    if (which == "1" || which == "memcpy") runtime.register_function(kMemcpy, &guest_memcpy, "infected_memcpy");
    if (which == "1" || which == "memset") runtime.register_function(kMemset, &guest_memset, "infected_memset");
    infected::tuning_install(runtime);
}

constexpr SaveFolder kSaveFolders[] = {
    {"ULUS10054", "Game data", true},
};

} // namespace

const GameProfile &game() {
    static const GameProfile profile{
        .app_name = "InfectedNative",
        .project_name = "Infected",
        .env_prefix = "INFECTED",
        .data_organization = "InfectedPort",
        .data_application = "infected",
        .disc_id = "ULUS10054",
        .disc_id_display = "ULUS-10054",
        .game_title = "Infected",
        .executable_path_on_disc = "PSP_GAME/SYSDIR/BOOT.BIN",
        .param_sfo_path_on_disc = "PSP_GAME/PARAM.SFO",
        .release_name = "the North American release, version 1.02",
        .encrypted_executable_sha256 = "",
        // SHA-256 of PSP_GAME/SYSDIR/BOOT.BIN, which is already the plain executable.
        .executable_sha256 = "952d4bb2c08209f4cbc0020d95c4cc601e9944cad5c7ade4c1d060fe8daab1bc",
        .decryption_tag = 0u,
        .decryption_key = {},
        .load_base = 0x08804000u,
        .guest_ram_bytes = 32u * 1024u * 1024u,
        .boot_path = "disc0:/PSP_GAME/SYSDIR/BOOT.BIN",
        .overlay_slots = {},
        .save_game_name = "ULUS10054",
        .save_folders = kSaveFolders,
        .adhoc_product_code = "ULUS10054",
        .patch_loaded_image = &patch_image,
        .frame_hook = &infected::tuning_frame,
        .frame_vblanks = 2,
        .system_language = 1u,
        .confirm_button = 1u,
    };
    return profile;
}

} // namespace portablekit
