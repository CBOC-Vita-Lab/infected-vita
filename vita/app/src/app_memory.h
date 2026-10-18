// app_memory.h - the process memory budget and the guest arena provider.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace app {

// psprecomp::ArenaProvider callbacks: the 66 MiB masked guest arena comes from one plat::mem_alloc_block (cached USER_RW, 1 MiB aligned).
uint8_t* arena_allocate(size_t bytes);
void arena_release(uint8_t* arena, size_t bytes);
size_t arena_bytes();

// One log line of the memory state: free bytes of the three system pools, newlib heap use and its peak, vitaGL pools.
std::string memory_line();
// Highest malloc arena size seen by memory_line()/memory_sample() (bytes the heap has taken from its block).
size_t heap_peak();
void memory_sample();   // updates the peak; cheap, called from the status line

}  // namespace app
