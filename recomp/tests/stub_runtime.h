// shared between stub_runtime.cpp and the harnesses
#pragma once
#include <csetjmp>
#include <cstdint>

struct FakeHle { uint32_t v0, v1; float f0; };
FakeHle fake_hle(uint32_t stub, const uint32_t* a, const float* fa, uint32_t sp, uint32_t counter, int ngpr, int nfpr);

// out-parameters: every hashed argument that points into the test arena receives 12 deterministic bytes (the real HLE
// would fill such buffers; leaving them stale makes the oracle and lean read different dead-stack junk)
extern uint32_t g_hle_stack_lo, g_hle_stack_hi, g_hle_dead_hi;
void fake_hle_writes(uint8_t* ram, uint32_t stub, const uint32_t* a, int ngpr, uint32_t counter);

// import arity table (generated into lean_test_meta.inc by lean_gen.py)
int stub_arity_gpr(uint32_t stub);
int stub_arity_fpr(uint32_t stub);

struct StubState {
    sigjmp_buf jmp;
    const char* abort_reason = nullptr;
    uint32_t abort_pc = 0;
    bool fatal = false;
    uint32_t hle_counter = 0;
    uint64_t hle_calls = 0, guest_calls = 0;
    int depth = 0;
};
extern StubState g_stub;
