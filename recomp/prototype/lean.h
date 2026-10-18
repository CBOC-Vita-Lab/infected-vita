// Runtime header for the lean-generator experiment (Cortex-A9, Thumb-2, GCC 15).
// Memory model: unchecked bias-pointer access into a flat guest RAM image; guest address 0x08000000 maps to g_ram[0].
#pragma once
#include <stdint.h>
#include <string.h>
typedef uint32_t u32; typedef int32_t s32; typedef uint16_t u16; typedef int16_t s16;
typedef uint8_t u8; typedef int8_t s8; typedef uint64_t u64; typedef int64_t s64;
typedef u32 __attribute__((may_alias)) u32a; typedef u16 __attribute__((may_alias)) u16a; typedef u8 __attribute__((may_alias)) u8a;
typedef s16 __attribute__((may_alias)) s16a; typedef s8 __attribute__((may_alias)) s8a; typedef float __attribute__((may_alias)) floata;
extern "C" u8 g_ram[];
extern "C" void* const g_fn_tbl[];
#if defined(LEAN_MASK25)
// recommended design point: arena starts at guest 0x08000000, no bias term; folds mirrors and the scratchpad (0x00010000 -> arena+0x10000)
#define MEMA(a) ((uintptr_t)g_ram + ((u32)(a) & 0x01FFFFFFu))
#elif defined(LEAN_MASK)
#define MEMA(a) ((uintptr_t)g_ram - 0x08000000u + ((u32)(a) & 0x1FFFFFFFu))
#else
#define MEMA(a) ((uintptr_t)g_ram - 0x08000000u + (u32)(a))
#endif
#define LW(a)  (*(const u32a*)MEMA(a))
#define LHU(a) ((u32)*(const u16a*)MEMA(a))
#define LH(a)  ((s32)*(const s16a*)MEMA(a))
#define LBU(a) ((u32)*(const u8a*)MEMA(a))
#define LB(a)  ((s32)*(const s8a*)MEMA(a))
#define LWF(a) (*(const floata*)MEMA(a))
#define SW(a,v) (*(u32a*)MEMA(a) = (u32)(v))
#define SH(a,v) (*(u16a*)MEMA(a) = (u16)(v))
#define SB(a,v) (*(u8a*)MEMA(a) = (u8)(v))
#define SWF(a,v) (*(floata*)MEMA(a) = (v))
static inline float BC_F(u32 x) { return __builtin_bit_cast(float, x); }
static inline u32 BC_U(float x) { return __builtin_bit_cast(u32, x); }
static inline s32 MAXS(s32 a, s32 b) { return a > b ? a : b; }
static inline s32 MINS(s32 a, s32 b) { return a < b ? a : b; }
static inline float SQRTF(float x) { return __builtin_sqrtf(x); }
static inline float FABSF(float x) { return __builtin_fabsf(x); }
static inline u32 TRUNC_W(float x) { if (x != x) return 0x7FFFFFFFu; if (x >= 2147483648.0f) return 0x7FFFFFFFu; if (x < -2147483648.0f) return 0x80000000u; return (u32)(s32)x; }
static inline u32 CVT_W(float x) { return TRUNC_W(__builtin_rintf(x)); }
static inline void DIV_S(u32 a, u32 b, u32& lo, u32& hi) { s32 x = (s32)a, y = (s32)b; if (y == 0) { lo = x < 0 ? 1u : 0xFFFFFFFFu; hi = a; } else if (y == -1 && x == INT32_MIN) { lo = 0x80000000u; hi = 0; } else { lo = (u32)(x / y); hi = (u32)(x % y); } }
static inline void DIV_U(u32 a, u32 b, u32& lo, u32& hi) { if (b == 0) { lo = 0xFFFFFFFFu; hi = a; } else { lo = a / b; hi = a % b; } }
typedef u64 (*ind_fn)(u32, u32, u32, u32, u32);
#define CALL_IND(t, sp, a0, a1, a2, a3) (((ind_fn)g_fn_tbl[((u32)(t) - 0x08804000u) >> 2])((sp), (a0), (a1), (a2), (a3)))
