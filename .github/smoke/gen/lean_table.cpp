// Synthetic corpus table of the kit smoke test: what the app expects of a game library (g_lean_entry, g_lean_entry_count).
#include "lean_decls.h"

extern "C" {
const uint32_t g_lean_entry_count = 2u;
void (*const g_lean_entry[2])() = { smoke_entry_a, smoke_entry_b };
}
