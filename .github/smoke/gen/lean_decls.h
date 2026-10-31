// Synthetic stand-in for the generated lean_decls.h: declares the entry points of the two synthetic units.
// Nothing here comes from a game; it only gives the kit's compile, archive, link and packaging steps something to chew on.
#pragma once
#include "lean_abi.h"
extern "C" {
void smoke_entry_a();
void smoke_entry_b();
}
