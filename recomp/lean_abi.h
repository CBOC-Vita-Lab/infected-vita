// lean_abi.h - runtime ABI shared by the lean generator's output, the native hooks and the runtime.
//
// Everything the generated code needs from outside is declared here (the "contract"), plus the inline helpers whose
// semantics must equal PortableKit's interpreter exactly (arithmetic, FPU, unaligned access).  VFPU helpers are in
// lean_vfpu.h (included at the end).  Nothing in this file is game specific.
//
// Compile with:  -std=c++20 -fno-exceptions -fno-rtti -fno-math-errno -ffp-contract=off  (and -fwrapv is NOT required:
// the helpers never rely on signed overflow).
#pragma once

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <limits.h>

#include "psprecomp/allegrex_context.hpp"

// NEON: real intrinsics on ARM; on x86-64 the tests can build the same code against tests/lean_neon_shim.h (-DLEAN_NEON_SHIM);
// otherwise the scalar fallbacks are used.
#if defined(__ARM_NEON)
#include <arm_neon.h>
#define LEAN_HAVE_NEON 1
#elif defined(LEAN_NEON_SHIM)
#include "lean_neon_shim.h"
#define LEAN_HAVE_NEON 1
#else
#define LEAN_HAVE_NEON 0
#endif

#define LEAN_INLINE inline __attribute__((always_inline))
#define LEAN_NOINLINE __attribute__((noinline))
#define LEAN_LIKELY(x) __builtin_expect(!!(x), 1)
#define LEAN_UNLIKELY(x) __builtin_expect(!!(x), 0)

// call tracing for the differential tests' bisection mode (-DLEAN_TRACE): function entry address, mask of valid a0..a3, a0..a3
#ifdef LEAN_TRACE
extern "C" void lean_trace(uint32_t pc, uint32_t mask, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
#define LEAN_TRC(pc, mask, a0, a1, a2, a3) lean_trace(pc, mask, a0, a1, a2, a3)
#else
#define LEAN_TRC(...) ((void)0)
#endif

// ------------------------------------------------------------------------------------------------ the contract
//
// The runtime provides these symbols (and only these) to the generated corpus; the corpus provides
// g_lean_entry / g_lean_entry_count (liblean_*.a).
extern psprecomp::AllegrexContext g_cpu;            // the one live register file (VFPU state, import/regfile marshalling)
extern "C" {
extern uint8_t* const g_ram;                         // flat guest arena, 0x04200000 bytes, see LEAN_H
void pk_guest_call(uint32_t pc);                     // run the guest function at pc to its return, regfile convention on g_cpu
void pk_hle_call(uint32_t stub_pc);                  // HLE import; arguments / results in g_cpu (a0..a7, sp, f12..f19 -> v0, v1, f0)
[[noreturn]] void pk_fatal(const char* what, uint32_t pc);
void pk_yield(void);                                 // only referenced when the profile's yield list is non-empty
extern void (*const g_lean_entry[])();               // index (pc - 0x08804000) >> 2, nullptr = not a function start
extern const uint32_t g_lean_entry_count;
}
#define PK_YIELD() pk_yield()

#define LEAN_BASE 0x08804000u

// ------------------------------------------------------------------------------------------------ bits and floats
typedef uint32_t u32;
typedef int32_t s32;

static LEAN_INLINE float lean_u2f(uint32_t x) { return __builtin_bit_cast(float, x); }
static LEAN_INLINE uint32_t lean_f2u(float x) { return __builtin_bit_cast(uint32_t, x); }
static LEAN_INLINE uint64_t lean_pk2(uint32_t lo, uint32_t hi) { return (uint64_t)lo | ((uint64_t)hi << 32); }
static LEAN_INLINE uint32_t lean_lo(uint64_t x) { return (uint32_t)x; }
static LEAN_INLINE uint32_t lean_hi(uint64_t x) { return (uint32_t)(x >> 32); }

// three return values (v0, v1, f0): AAPCS returns composites > 4 bytes in memory, which is fine for this rare form
struct lean_r3 { uint64_t a; uint32_t b; };

// ------------------------------------------------------------------------------------------------ guest memory
//
//   host(a) = g_ram + (a & 0x05FFFFFF)            (ARCHITECTURE.md section 1; one `bic` on ARM)
//
// Accesses are unchecked.  Integer accesses may be unaligned (ARMv7 ldr/str handle that; the helpers never let the
// compiler merge them into ldrd/ldm); float accesses assume word alignment (vldr/vstr), as the PSP faults otherwise.
#define LEAN_MASK 0x05FFFFFFu
typedef uint32_t __attribute__((may_alias, aligned(1))) lean_u32u;
typedef uint16_t __attribute__((may_alias, aligned(1))) lean_u16u;
typedef int16_t __attribute__((may_alias, aligned(1))) lean_s16u;
typedef int8_t __attribute__((may_alias)) lean_s8a;
typedef uint8_t __attribute__((may_alias)) lean_u8a;
typedef float __attribute__((may_alias, aligned(1))) lean_f32u;
typedef float __attribute__((may_alias, aligned(4))) lean_f32a;

static LEAN_INLINE uint8_t* lean_h(uint32_t a) { return g_ram + (a & LEAN_MASK); }

// by guest address (masked)
static LEAN_INLINE uint32_t lean_lw(uint32_t a) { return *(const lean_u32u*)lean_h(a); }
static LEAN_INLINE uint32_t lean_lhu(uint32_t a) { return *(const lean_u16u*)lean_h(a); }
static LEAN_INLINE uint32_t lean_lh(uint32_t a) { return (uint32_t)(int32_t)*(const lean_s16u*)lean_h(a); }
static LEAN_INLINE uint32_t lean_lbu(uint32_t a) { return *(const lean_u8a*)lean_h(a); }
static LEAN_INLINE uint32_t lean_lb(uint32_t a) { return (uint32_t)(int32_t)*(const lean_s8a*)lean_h(a); }
static LEAN_INLINE float lean_lwc1(uint32_t a) { return *(const lean_f32a*)lean_h(a); }
static LEAN_INLINE void lean_sw(uint32_t a, uint32_t v) { *(lean_u32u*)lean_h(a) = v; }
static LEAN_INLINE void lean_sh(uint32_t a, uint32_t v) { *(lean_u16u*)lean_h(a) = (uint16_t)v; }
static LEAN_INLINE void lean_sb(uint32_t a, uint32_t v) { *(lean_u8a*)lean_h(a) = (uint8_t)v; }
static LEAN_INLINE void lean_swc1(uint32_t a, float v) { *(lean_f32a*)lean_h(a) = v; }

// by host pointer (stack-relative accesses: the mask was applied once to sp)
static LEAN_INLINE uint32_t lean_lw_p(const uint8_t* p) { return *(const lean_u32u*)p; }
static LEAN_INLINE uint32_t lean_lhu_p(const uint8_t* p) { return *(const lean_u16u*)p; }
static LEAN_INLINE uint32_t lean_lh_p(const uint8_t* p) { return (uint32_t)(int32_t)*(const lean_s16u*)p; }
static LEAN_INLINE uint32_t lean_lbu_p(const uint8_t* p) { return *(const lean_u8a*)p; }
static LEAN_INLINE uint32_t lean_lb_p(const uint8_t* p) { return (uint32_t)(int32_t)*(const lean_s8a*)p; }
static LEAN_INLINE float lean_lwc1_p(const uint8_t* p) { return *(const lean_f32a*)p; }
static LEAN_INLINE void lean_sw_p(uint8_t* p, uint32_t v) { *(lean_u32u*)p = v; }
static LEAN_INLINE void lean_sh_p(uint8_t* p, uint32_t v) { *(lean_u16u*)p = (uint16_t)v; }
static LEAN_INLINE void lean_sb_p(uint8_t* p, uint32_t v) { *(lean_u8a*)p = (uint8_t)v; }
static LEAN_INLINE void lean_swc1_p(uint8_t* p, float v) { *(lean_f32a*)p = v; }

// lwl / lwr / swl / swr, exactly the interpreter's (GuestMemory::aot_load_word_left ...) formulas on aligned words
static LEAN_INLINE uint32_t lean_lwl(uint32_t a, uint32_t existing) {
    const uint32_t shift = (a & 3u) * 8u;
    const uint32_t w = lean_lw(a & ~3u);
    return (existing & (0x00FFFFFFu >> shift)) | (w << (24u - shift));
}
static LEAN_INLINE uint32_t lean_lwr(uint32_t a, uint32_t existing) {
    const uint32_t shift = (a & 3u) * 8u;
    const uint32_t w = lean_lw(a & ~3u);
    return (existing & (0xFFFFFF00u << (24u - shift))) | (w >> shift);
}
static LEAN_INLINE void lean_swl(uint32_t a, uint32_t v) {
    const uint32_t shift = (a & 3u) * 8u;
    const uint32_t w = lean_lw(a & ~3u);
    lean_sw(a & ~3u, (v >> (24u - shift)) | (w & (0xFFFFFF00u << shift)));
}
static LEAN_INLINE void lean_swr(uint32_t a, uint32_t v) {
    const uint32_t shift = (a & 3u) * 8u;
    const uint32_t w = lean_lw(a & ~3u);
    lean_sw(a & ~3u, (v << shift) | (w & (0x00FFFFFFu >> (24u - shift))));
}
// fused lwr/lwl (swr/swl) pair: one unaligned word access at `a` = the lwr address
static LEAN_INLINE uint32_t lean_ulw(uint32_t a) { return lean_lw(a); }
static LEAN_INLINE void lean_usw(uint32_t a, uint32_t v) { lean_sw(a, v); }

// ------------------------------------------------------------------------------------------------ integer helpers
static LEAN_INLINE uint32_t lean_rotr(uint32_t x, uint32_t n) { n &= 31u; return (x >> n) | (x << ((32u - n) & 31u)); }
static LEAN_INLINE uint32_t lean_clz(uint32_t x) { return x ? (uint32_t)__builtin_clz(x) : 32u; }
static LEAN_INLINE uint32_t lean_clo(uint32_t x) { return lean_clz(~x); }
static LEAN_INLINE uint32_t lean_bitrev(uint32_t v) {
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
    v = ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8);
    return (v >> 16) | (v << 16);
}
static LEAN_INLINE uint32_t lean_wsbh(uint32_t v) { return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8); }
static LEAN_INLINE uint32_t lean_wsbw(uint32_t v) { return __builtin_bswap32(v); }
static LEAN_INLINE uint32_t lean_seb(uint32_t v) { return (uint32_t)(int32_t)(int8_t)(uint8_t)v; }
static LEAN_INLINE uint32_t lean_seh(uint32_t v) { return (uint32_t)(int32_t)(int16_t)(uint16_t)v; }
static LEAN_INLINE uint32_t lean_sra(uint32_t v, uint32_t n) { return (uint32_t)((int32_t)v >> (n & 31u)); }
static LEAN_INLINE uint32_t lean_slt(uint32_t a, uint32_t b) { return (int32_t)a < (int32_t)b; }
static LEAN_INLINE uint32_t lean_max(uint32_t a, uint32_t b) { return (int32_t)a > (int32_t)b ? a : b; }
static LEAN_INLINE uint32_t lean_min(uint32_t a, uint32_t b) { return (int32_t)a < (int32_t)b ? a : b; }
static LEAN_INLINE uint32_t lean_ext(uint32_t rs, uint32_t pos, uint32_t size) {
    const uint32_t mask = size >= 32u ? 0xFFFFFFFFu : ((1u << size) - 1u);
    return (rs >> pos) & mask;
}
static LEAN_INLINE uint32_t lean_ins(uint32_t rt, uint32_t rs, uint32_t pos, uint32_t msb) {
    const uint32_t size = msb >= pos ? msb - pos + 1u : 0u;
    const uint32_t smask = size >= 32u ? 0xFFFFFFFFu : (size == 0u ? 0u : ((1u << size) - 1u));
    const uint32_t dmask = smask << pos;
    return (rt & ~dmask) | ((rs & smask) << pos);
}
// multiply / divide: hi and lo by reference
static LEAN_INLINE void lean_mult(uint32_t a, uint32_t b, uint32_t& hi, uint32_t& lo) {
    const int64_t p = (int64_t)(int32_t)a * (int64_t)(int32_t)b;
    lo = (uint32_t)(uint64_t)p; hi = (uint32_t)((uint64_t)p >> 32);
}
static LEAN_INLINE void lean_multu(uint32_t a, uint32_t b, uint32_t& hi, uint32_t& lo) {
    const uint64_t p = (uint64_t)a * (uint64_t)b;
    lo = (uint32_t)p; hi = (uint32_t)(p >> 32);
}
static LEAN_INLINE void lean_madd(uint32_t a, uint32_t b, uint32_t& hi, uint32_t& lo) {
    const int64_t p = (int64_t)(int32_t)a * (int64_t)(int32_t)b;
    const uint64_t r = (((uint64_t)hi << 32) | lo) + (uint64_t)p;
    lo = (uint32_t)r; hi = (uint32_t)(r >> 32);
}
static LEAN_INLINE void lean_msub(uint32_t a, uint32_t b, uint32_t& hi, uint32_t& lo) {
    const int64_t p = (int64_t)(int32_t)a * (int64_t)(int32_t)b;
    const uint64_t r = (((uint64_t)hi << 32) | lo) - (uint64_t)p;
    lo = (uint32_t)r; hi = (uint32_t)(r >> 32);
}
static LEAN_INLINE void lean_maddu(uint32_t a, uint32_t b, uint32_t& hi, uint32_t& lo) {
    const uint64_t r = (((uint64_t)hi << 32) | lo) + (uint64_t)a * (uint64_t)b;
    lo = (uint32_t)r; hi = (uint32_t)(r >> 32);
}
static LEAN_INLINE void lean_msubu(uint32_t a, uint32_t b, uint32_t& hi, uint32_t& lo) {
    const uint64_t r = (((uint64_t)hi << 32) | lo) - (uint64_t)a * (uint64_t)b;
    lo = (uint32_t)r; hi = (uint32_t)(r >> 32);
}
static LEAN_INLINE void lean_div(uint32_t a, uint32_t b, uint32_t& hi, uint32_t& lo) {
    const int32_t x = (int32_t)a, y = (int32_t)b;
    if (y == 0) { lo = x >= 0 ? 0xFFFFFFFFu : 1u; hi = (uint32_t)x; }
    else if (x == INT32_MIN && y == -1) { lo = 0x80000000u; hi = 0u; }
    else { lo = (uint32_t)(x / y); hi = (uint32_t)(x % y); }
}
static LEAN_INLINE void lean_divu(uint32_t a, uint32_t b, uint32_t& hi, uint32_t& lo) {
    if (b == 0u) { lo = 0xFFFFFFFFu; hi = a; }
    else { lo = a / b; hi = a % b; }
}
// add / sub trap on signed overflow in the interpreter (arithmetic_overflow -> stop)
static LEAN_INLINE uint32_t lean_add_trap(uint32_t a, uint32_t b, uint32_t pc) {
    int32_t r;
    if (__builtin_add_overflow((int32_t)a, (int32_t)b, &r)) pk_fatal("arithmetic overflow (add)", pc);
    return (uint32_t)r;
}
static LEAN_INLINE uint32_t lean_sub_trap(uint32_t a, uint32_t b, uint32_t pc) {
    int32_t r;
    if (__builtin_sub_overflow((int32_t)a, (int32_t)b, &r)) pk_fatal("arithmetic overflow (sub)", pc);
    return (uint32_t)r;
}

// ------------------------------------------------------------------------------------------------ FPU
// mul.s: the interpreter produces a quiet NaN for inf * 0
static LEAN_INLINE float lean_mul_s(float a, float b) {
    if ((__builtin_isinf(a) && b == 0.0f) || (__builtin_isinf(b) && a == 0.0f)) return lean_u2f(0x7FC00000u);
    return a * b;
}
static LEAN_INLINE float lean_abs_s(float a) { return lean_u2f(lean_f2u(a) & 0x7FFFFFFFu); }
static LEAN_INLINE float lean_neg_s(float a) { return lean_u2f(lean_f2u(a) ^ 0x80000000u); }
static LEAN_INLINE float lean_sqrt_s(float a) { return __builtin_sqrtf(a); }

// c.cond.s -> condition flag.  cond = low four bits of the instruction word (the interpreter's evaluate_fpu_compare)
template <uint32_t Cond>
static LEAN_INLINE uint32_t lean_fcmp(float fs, float ft) {
    // every form maps onto one VFP condition (NaN makes <, <=, >, >=, == false): no explicit unordered test
    switch (Cond & 0xFu) {
    case 0: case 8: return 0u;
    case 1: case 9: return __builtin_isunordered(fs, ft);
    case 2: case 10: return fs == ft;
    case 3: case 11: return !(fs < ft || fs > ft);          // unordered or equal
    case 4: case 12: return fs < ft;
    case 5: case 13: return !(fs >= ft);                    // unordered or less
    case 6: case 14: return fs <= ft;
    default: return !(fs > ft);                             // unordered or less-or-equal
    }
}

// float -> word conversions (AllegrexContext::fpu_float_to_word): modes 0 round(floor(x+.5)) 1 trunc 2 ceil 3 floor 4 cvt (fcr31)
static LEAN_INLINE uint32_t lean_clamp_word(double v) {
    constexpr double lo = -2147483648.0, hi = 2147483647.0;
    if (v <= lo) return 0x80000000u;
    if (v >= hi) return 0x7FFFFFFFu;
    return (uint32_t)(int32_t)v;
}
static LEAN_INLINE uint32_t lean_nonfinite_word(float v) {
    return (__builtin_isinf(v) && __builtin_signbit(v)) ? 0x80000000u : 0x7FFFFFFFu;
}
static LEAN_INLINE double lean_round_even(double v) {
    const double lower = __builtin_floor(v);
    const double fraction = v - lower;
    if (fraction < 0.5) return lower;
    if (fraction > 0.5) return lower + 1.0;
    const int64_t integral = (int64_t)lower;
    return (integral & 1) == 0 ? lower : lower + 1.0;
}
static LEAN_INLINE uint32_t lean_trunc_w(float v) {
    if (!__builtin_isfinite(v)) return lean_nonfinite_word(v);
    if (v >= 2147483648.0f) return 0x7FFFFFFFu;
    if (v <= -2147483648.0f) return 0x80000000u;
    return (uint32_t)(int32_t)v;
}
static LEAN_INLINE uint32_t lean_round_w(float v) {
    if (!__builtin_isfinite(v)) return lean_nonfinite_word(v);
    return lean_clamp_word(__builtin_floor((double)v + 0.5));
}
static LEAN_INLINE uint32_t lean_ceil_w(float v) {
    if (!__builtin_isfinite(v)) return lean_nonfinite_word(v);
    return lean_clamp_word(__builtin_ceil((double)v));
}
static LEAN_INLINE uint32_t lean_floor_w(float v) {
    if (!__builtin_isfinite(v)) return lean_nonfinite_word(v);
    return lean_clamp_word(__builtin_floor((double)v));
}
static LEAN_INLINE uint32_t lean_cvt_w(float v) {
    if (!__builtin_isfinite(v)) return lean_nonfinite_word(v);
    double c;
    switch (g_cpu.fcr31 & 3u) {
    case 0: c = lean_round_even((double)v); break;
    case 1: c = __builtin_trunc((double)v); break;
    case 2: c = __builtin_ceil((double)v); break;
    default: c = __builtin_floor((double)v); break;
    }
    return lean_clamp_word(c);
}
static LEAN_INLINE float lean_cvt_s_w(uint32_t bits) { return (float)(int32_t)bits; }

// cfc1 / ctc1 (only $31 exists).  The condition flag lives in a local (`cc`); the rest of fcr31 in g_cpu.fcr31.
static LEAN_INLINE uint32_t lean_cfc1(uint32_t cc) { return (g_cpu.fcr31 & ~(1u << 23)) | ((cc & 1u) << 23); }
static LEAN_INLINE uint32_t lean_ctc1(uint32_t v) { g_cpu.fcr31 = v & 0x0181FFFFu; return (g_cpu.fcr31 >> 23) & 1u; }

// ------------------------------------------------------------------------------------------------ native hooks
// Replacements of guest library functions, verified against their disassembly (see recomp/README.md).  Each hook is
// switched off per function with g_lean_hook_off (bit set = hook disabled; lean_init_hooks_from_env() sets it from the
// environment variable LEAN_NO_HOOKS="memcpy,memset,strcmp" or "all").
extern "C" uint32_t g_lean_hook_off;
extern "C" void lean_init_hooks_from_env(void);
enum : uint32_t { LEAN_HOOK_MEMCPY = 1u, LEAN_HOOK_MEMSET = 2u, LEAN_HOOK_STRCMP = 4u };

// ranges are handled natively only when they stay inside one 16 MiB window (no mask wrap, no 32-bit overflow)
static LEAN_INLINE bool lean_range_ok(uint32_t a, uint32_t n) { return n != 0u && (a + n - 1u) >= a && ((a ^ (a + n - 1u)) >> 24) == 0u; }

// 0x08A2F228: byte loop, forward, `n` iterations; returns dst.  When dst lies above src inside the copied range the
// guest loop re-reads its own output (pattern fill), so that case stays a byte loop.
static inline uint32_t lean_hook_memcpy(uint32_t dst, uint32_t src, uint32_t n) {
    if (n == 0u) return dst;
    if (lean_range_ok(dst, n) && lean_range_ok(src, n)) {
        uint8_t* d = lean_h(dst); const uint8_t* s = lean_h(src);
        if (d > s && (uintptr_t)(d - s) < n) { for (uint32_t i = 0; i < n; ++i) d[i] = s[i]; }
        else memmove(d, s, n);
        return dst;
    }
    for (uint32_t i = 0; i < n; ++i) lean_sb(dst + i, lean_lbu(src + i));
    return dst;
}
// 0x08A2F2FC: stores the low byte of a1, n times; returns dst
static inline uint32_t lean_hook_memset(uint32_t dst, uint32_t c, uint32_t n) {
    if (n == 0u) return dst;
    if (lean_range_ok(dst, n)) { memset(lean_h(dst), (int)(c & 0xFFu), n); return dst; }
    for (uint32_t i = 0; i < n; ++i) lean_sb(dst + i, c);
    return dst;
}
// 0x08A2F47C: compares signed bytes, stops at a differing byte or a NUL in s1; returns lbu(s1) - lbu(s2) at that point
static inline uint32_t lean_hook_strcmp(uint32_t a, uint32_t b) {
    for (;;) {
        const int32_t x = (int32_t)(int8_t)lean_lbu(a);
        if (x == 0) break;
        const int32_t y = (int32_t)(int8_t)lean_lbu(b);
        if (x != y) break;
        ++a; ++b;
    }
    return lean_lbu(a) - lean_lbu(b);
}

// function-entry check of the VFPU prefix assumption (lean_vfpu_fast.h): only in the test build (-DLEAN_CHECK_VFPU_ENTRY)
#ifdef LEAN_CHECK_VFPU_ENTRY
#define LEAN_VFPU_ENTRY_CHECK(pc) do { if (g_cpu.vfpu_ctrl[0] != 0xE4u || g_cpu.vfpu_ctrl[1] != 0xE4u || g_cpu.vfpu_ctrl[2] != 0u) pk_fatal("VFPU prefix pending at function entry", (pc)); } while (0)
#else
#define LEAN_VFPU_ENTRY_CHECK(pc) ((void)0)
#endif

#include "lean_vfpu.h"
#include "lean_vfpu_fast.h"
