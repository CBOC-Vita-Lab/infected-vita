// Synthetic unit 0 of the kit smoke test (no game data): a few helpers from lean_abi.h so that the real headers, flags and
// NEON paths are compiled, and a cross-unit call so that the link-time optimiser has something to inline.
#include "lean_decls.h"

extern "C" {

uint32_t smoke_mix(uint32_t x) {
    float f = lean_u2f(x | 0x3f800000u);
    f = f * 1.5f + 0.25f;
    return lean_f2u(f) ^ lean_lo(lean_pk2(x, x * 3u));
}

void smoke_entry_a() {
    volatile uint32_t* p = reinterpret_cast<volatile uint32_t*>(g_ram + (LEAN_BASE & LEAN_MASK));
    for (int i = 0; i < 16; i++) p[i] = smoke_mix(p[i] + (uint32_t)i);
}

}
