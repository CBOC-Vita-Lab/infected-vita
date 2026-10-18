// lean_neon_shim.h - the subset of <arm_neon.h> that the lean VFPU forms use, as plain C++ for the x86-64 test build
// (-DLEAN_NEON_SHIM).  Each intrinsic does what the ARM instruction does lane by lane in IEEE single precision with the
// default rounding; the ARM Advanced SIMD unit's flush-to-zero and default-NaN behaviour is not modelled (the guest-visible
// differences are denormal inputs / results and NaN payloads, see recomp/README.md).
#pragma once
#include <stdint.h>
#include <string.h>

struct float32x4_t { float v[4]; };
struct uint8x16_t { uint8_t v[16]; };
struct float32x4x4_t { float32x4_t val[4]; };

static inline float32x4_t vld1q_f32(const float* p) { float32x4_t r; memcpy(r.v, p, 16); return r; }
static inline void vst1q_f32(float* p, float32x4_t a) { memcpy(p, a.v, 16); }
static inline uint8x16_t vld1q_u8(const uint8_t* p) { uint8x16_t r; memcpy(r.v, p, 16); return r; }
static inline void vst1q_u8(uint8_t* p, uint8x16_t a) { memcpy(p, a.v, 16); }
static inline float32x4_t vreinterpretq_f32_u8(uint8x16_t a) { float32x4_t r; memcpy(r.v, a.v, 16); return r; }
static inline uint8x16_t vreinterpretq_u8_f32(float32x4_t a) { uint8x16_t r; memcpy(r.v, a.v, 16); return r; }
static inline float32x4_t vaddq_f32(float32x4_t a, float32x4_t b) { float32x4_t r; for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] + b.v[i]; return r; }
static inline float32x4_t vsubq_f32(float32x4_t a, float32x4_t b) { float32x4_t r; for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] - b.v[i]; return r; }
static inline float32x4_t vmulq_f32(float32x4_t a, float32x4_t b) { float32x4_t r; for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] * b.v[i]; return r; }
static inline float32x4_t vmulq_n_f32(float32x4_t a, float b) { float32x4_t r; for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] * b; return r; }
static inline float32x4_t vdupq_n_f32(float a) { float32x4_t r; for (int i = 0; i < 4; ++i) r.v[i] = a; return r; }
#define vgetq_lane_f32(a, lane) ((a).v[lane])
static inline float32x4x4_t vld4q_f32(const float* p) {
    float32x4x4_t r;
    for (int c = 0; c < 4; ++c) for (int b = 0; b < 4; ++b) r.val[c].v[b] = p[b * 4 + c];
    return r;
}
