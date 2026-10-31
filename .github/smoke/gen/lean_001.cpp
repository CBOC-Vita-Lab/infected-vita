// Synthetic unit 1 of the kit smoke test (no game data).
#include "lean_decls.h"

extern "C" {

uint32_t smoke_mix(uint32_t x);

void smoke_entry_b() {
    volatile uint32_t* p = reinterpret_cast<volatile uint32_t*>(g_ram + ((LEAN_BASE + 0x100u) & LEAN_MASK));
    uint32_t acc = 0;
    for (int i = 0; i < 32; i++) acc += smoke_mix(p[i]);
    p[0] = acc;
    smoke_entry_a();
}

}
