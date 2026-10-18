// lean_vfpu_fast.h - VFPU lowering, step 2: instruction forms specialised on a statically known prefix state.
//
// The generator tracks the S / T / D prefix registers (vfpu_ctrl[0..2]) through the control flow graph of every function
// (leangen/vfpu.py).  Where all three are known at an instruction, it calls the form below instead of the generic
// lean_vf:: helper:
//
//     lean_vk::vdot<W, S, T, D>();        W = the instruction word, S / T / D = the prefix values in force
//
// Everything that depends on W, S, T or D is a constant expression, so the lane mapping, the prefix swizzle / abs / negate /
// constants, the saturation and the write mask all fold away, lanes are read from and written to g_cpu.vfpu at constant
// offsets, and no prefix register is loaded.  The only memory traffic left is the one the guest instruction performs.
// Contiguous column quads (lv.q, sv.q, vadd / vsub / vmul, vdot, vscl on C-registers, vmmul on M-registers) use NEON when
// __ARM_NEON is defined (and the x86 test shim when LEAN_NEON_SHIM is defined); everywhere else the scalar code is used.
//
// The invariant that makes this valid: g_cpu.vfpu_ctrl always holds the architectural prefix values.  A prefix is stored
// when it is set (vpfx), and an instruction that consumes prefixes stores the defaults back unless the static state already
// is the default (Eat<>).  The tracked state at a function entry and after a call is the default state (the guest compiler
// never leaves a prefix pending across a call); the x86 test build checks that (LEAN_VFPU_ENTRY_CHECK in lean_abi.h).
//
// Every form is the same computation as the corresponding lean_vf:: helper (which is PortableKit's interpreter code), in the
// same floating-point evaluation order; tests/vfpu_test.cpp checks them against each other and against the interpreter.
//
// Included from lean_abi.h; do not include directly.
#pragma once

#include <math.h>
#include <type_traits>

namespace lean_vk {

using psprecomp::AllegrexContext;

static constexpr uint32_t kS0 = 0xE4u, kT0 = 0xE4u, kD0 = 0u;      // prefix registers after reset ("eat")
static constexpr float kPfxConst[8] = {0.0f, 1.0f, 2.0f, 0.5f, 3.0f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f};

// ---------------------------------------------------------------------------------------------------- field decoding
constexpr uint32_t Len(uint32_t w) { return (((w >> 7u) & 1u) | (((w >> 15u) & 1u) << 1u)) + 1u; }
constexpr uint32_t Vd(uint32_t w) { return w & 0x7Fu; }
constexpr uint32_t Vs(uint32_t w) { return (w >> 8u) & 0x7Fu; }
constexpr uint32_t Vt(uint32_t w) { return (w >> 16u) & 0x7Fu; }
// index of lane `lane` of vector register `reg` of length `len` in g_cpu.vfpu
constexpr uint32_t Ix(uint32_t reg, uint32_t len, uint32_t lane) {
    return static_cast<uint32_t>(AllegrexContext::vfpu_vector_lane_index(reg, len, lane));
}
constexpr bool Contig4(uint32_t reg) {
    return Ix(reg, 4, 1) == Ix(reg, 4, 0) + 1 && Ix(reg, 4, 2) == Ix(reg, 4, 0) + 2 && Ix(reg, 4, 3) == Ix(reg, 4, 0) + 3;
}
// index of element (j, i) of matrix register `reg` of side `side` (AllegrexContext::read_vfpu_matrix)
constexpr uint32_t MIx(uint32_t reg, uint32_t side, uint32_t j, uint32_t i) {
    const uint32_t matrix = (reg >> 2u) & 7u, column = reg & 3u;
    bool transpose = ((reg >> 5u) & 1u) != 0u;
    uint32_t row = 0u;
    if (side == 1u) { transpose = false; row = (reg >> 5u) & 3u; }
    else if (side == 2u || side == 4u) row = (reg >> 5u) & 2u;
    else if (side == 3u) row = (reg >> 6u) & 1u;
    const uint32_t base = matrix * 16u;
    return transpose ? base + (((row + i) & 3u) * 4u + ((column + j) & 3u)) : base + (((column + j) & 3u) * 4u + ((row + i) & 3u));
}
// a side-4 matrix register that is stored as 16 consecutive floats with element (j, i) at 4 j + i
constexpr bool ContigM4(uint32_t reg) {
    for (uint32_t j = 0; j < 4; ++j)
        for (uint32_t i = 0; i < 4; ++i)
            if (MIx(reg, 4, j, i) != MIx(reg, 4, 0, 0) + j * 4u + i) return false;
    return true;
}

static LEAN_INLINE float& V(uint32_t i) { return g_cpu.vfpu[i]; }
static LEAN_INLINE float F(uint32_t bits) { return __builtin_bit_cast(float, bits); }
static LEAN_INLINE uint32_t B(float x) { return __builtin_bit_cast(uint32_t, x); }

// ---------------------------------------------------------------------------------------------------- lane access
template <uint32_t R, uint32_t L> static LEAN_INLINE void Rd(float (&v)[4]) {
    v[0] = V(Ix(R, L, 0));
    if constexpr (L >= 2) v[1] = V(Ix(R, L, 1));
    if constexpr (L >= 3) v[2] = V(Ix(R, L, 2));
    if constexpr (L >= 4) v[3] = V(Ix(R, L, 3));
}

// source prefix P applied to the L lanes of v (AllegrexContext::apply_vfpu_source_prefix; lanes >= L read as 0)
template <uint32_t P, uint32_t L, uint32_t I> static LEAN_INLINE float PfxLane(const float (&o)[4]) {
    constexpr uint32_t sw = (P >> (I * 2u)) & 3u;
    constexpr bool ab = ((P >> (8u + I)) & 1u) != 0u;
    constexpr bool cs = ((P >> (12u + I)) & 1u) != 0u;
    constexpr bool ng = ((P >> (16u + I)) & 1u) != 0u;
    float out;
    if constexpr (cs) {
        out = kPfxConst[sw + (ab ? 4u : 0u)];
    } else {
        if constexpr (sw < L) out = o[sw]; else out = 0.0f;
        if constexpr (ab) out = __builtin_fabsf(out);
    }
    if constexpr (ng) out = F(B(out) ^ 0x80000000u);
    return out;
}
template <uint32_t P, uint32_t L> static LEAN_INLINE void Spfx(float (&v)[4]) {
    if constexpr (P != 0xE4u) {
        const float o[4] = {v[0], v[1], v[2], v[3]};
        v[0] = PfxLane<P, L, 0>(o);
        if constexpr (L >= 2) v[1] = PfxLane<P, L, 1>(o);
        if constexpr (L >= 3) v[2] = PfxLane<P, L, 2>(o);
        if constexpr (L >= 4) v[3] = PfxLane<P, L, 3>(o);
    }
}

// destination prefix D: saturation, then the masked store (AllegrexContext::write_vfpu_vector_with_destination_prefix
// without the final reset of the prefixes, which Eat<> does)
template <uint32_t R, uint32_t L, uint32_t D, uint32_t I> static LEAN_INLINE void WrLane(float x) {
    if constexpr (((D >> (8u + I)) & 1u) == 0u) {
        constexpr uint32_t sat = (D >> (I * 2u)) & 3u;
        if constexpr (sat == 1u) x = fminf(1.0f, fmaxf(0.0f, x));
        else if constexpr (sat == 3u) x = fminf(1.0f, fmaxf(-1.0f, x));
        V(Ix(R, L, I)) = x;
    }
}
template <uint32_t R, uint32_t L, uint32_t D> static LEAN_INLINE void Wr(const float (&v)[4]) {
    WrLane<R, L, D, 0>(v[0]);
    if constexpr (L >= 2) WrLane<R, L, D, 1>(v[1]);
    if constexpr (L >= 3) WrLane<R, L, D, 2>(v[2]);
    if constexpr (L >= 4) WrLane<R, L, D, 3>(v[3]);
}

// the prefix registers go back to their defaults after an instruction that consumed them
template <uint32_t S, uint32_t T, uint32_t D> static LEAN_INLINE void Eat() {
    if constexpr (S != kS0) g_cpu.vfpu_ctrl[0] = kS0;
    if constexpr (T != kT0) g_cpu.vfpu_ctrl[1] = kT0;
    if constexpr (D != kD0) g_cpu.vfpu_ctrl[2] = kD0;
}

// ---------------------------------------------------------------------------------------------------- prefix setters
template <uint32_t W> static LEAN_INLINE void vpfx() {
    constexpr uint32_t control = (W >> 24u) & 3u;
    constexpr uint32_t data = control == 2u ? (W & 0x000FFFFFu) & 0x00000FFFu : (W & 0x000FFFFFu);
    g_cpu.vfpu_ctrl[control] = data;
}

// ---------------------------------------------------------------------------------------------------- memory forms
constexpr uint32_t ScalarOfMem(uint32_t w) { return ((w >> 16u) & 0x1Fu) | ((w & 3u) << 5u); }
constexpr uint32_t VectorOfMem(uint32_t w) { return ((w >> 16u) & 0x1Fu) | ((w & 1u) << 5u); }

template <uint32_t W> static LEAN_INLINE void lvs(uint32_t a) {
    V(AllegrexContext::vfpu_scalar_index(ScalarOfMem(W))) = F(lean_lw(a));
}
template <uint32_t W> static LEAN_INLINE void svs(uint32_t a) {
    lean_sw(a, B(V(AllegrexContext::vfpu_scalar_index(ScalarOfMem(W)))));
}
template <uint32_t W> static LEAN_INLINE void lvq(uint32_t a) {
    constexpr uint32_t R = VectorOfMem(W);
#if LEAN_HAVE_NEON
    if constexpr (Contig4(R)) {
        vst1q_f32(&V(Ix(R, 4, 0)), vreinterpretq_f32_u8(vld1q_u8(lean_h(a))));
        return;
    }
#endif
    const uint32_t x0 = lean_lw(a), x1 = lean_lw(a + 4u), x2 = lean_lw(a + 8u), x3 = lean_lw(a + 12u);
    V(Ix(R, 4, 0)) = F(x0); V(Ix(R, 4, 1)) = F(x1); V(Ix(R, 4, 2)) = F(x2); V(Ix(R, 4, 3)) = F(x3);
}
template <uint32_t W> static LEAN_INLINE void svq(uint32_t a) {
    constexpr uint32_t R = VectorOfMem(W);
#if LEAN_HAVE_NEON
    if constexpr (Contig4(R)) {
        vst1q_u8(lean_h(a), vreinterpretq_u8_f32(vld1q_f32(&V(Ix(R, 4, 0)))));
        return;
    }
#endif
    const uint32_t x0 = B(V(Ix(R, 4, 0))), x1 = B(V(Ix(R, 4, 1))), x2 = B(V(Ix(R, 4, 2))), x3 = B(V(Ix(R, 4, 3)));
    lean_sw(a, x0); lean_sw(a + 4u, x1); lean_sw(a + 8u, x2); lean_sw(a + 12u, x3);
}

// mtv / mfv: the scalar register number may name a control register (128..143)
template <uint32_t W> static LEAN_INLINE void mtv(uint32_t v) {
    constexpr uint32_t reg = W & 0xFFu;
    if constexpr (reg < 128u) V(AllegrexContext::vfpu_scalar_index(reg)) = F(v);
    else if constexpr (reg < 144u) g_cpu.vfpu_ctrl[reg - 128u] = v;
}
template <uint32_t W> static LEAN_INLINE uint32_t mfv() {
    constexpr uint32_t reg = W & 0xFFu;
    if constexpr (reg < 128u) return B(V(AllegrexContext::vfpu_scalar_index(reg)));
    else if constexpr (reg < 144u) return g_cpu.vfpu_ctrl[reg - 128u];
    else return 0u;
}

// ---------------------------------------------------------------------------------------------------- vadd / vsub / vmul / vdiv
template <uint32_t Op> static LEAN_INLINE float VecOp(float a, float b) {
    if constexpr (Op == 0u) return a + b;
    else if constexpr (Op == 1u) return a - b;
    else if constexpr (Op == 2u) return a * b;
    else return a / b;
}
template <uint32_t W, uint32_t S, uint32_t T, uint32_t D> static LEAN_INLINE void vvec3() {
    constexpr uint32_t L = Len(W), vd = Vd(W), vs = Vs(W), vt = Vt(W);
    constexpr uint32_t op = (W >> 26u) == 0x19u ? 2u : ((W >> 23u) & 7u);
    constexpr uint32_t o = op <= 2u ? op : 3u;
#if LEAN_HAVE_NEON
    if constexpr (L == 4u && S == kS0 && T == kT0 && D == kD0 && o <= 2u && Contig4(vs) && Contig4(vt) && Contig4(vd)) {
        const float32x4_t a = vld1q_f32(&V(Ix(vs, 4, 0))), b = vld1q_f32(&V(Ix(vt, 4, 0)));
        vst1q_f32(&V(Ix(vd, 4, 0)), o == 0u ? vaddq_f32(a, b) : (o == 1u ? vsubq_f32(a, b) : vmulq_f32(a, b)));
        return;
    }
#endif
    float s[4] = {0.0f, 0.0f, 0.0f, 0.0f}, t[4] = {0.0f, 0.0f, 0.0f, 0.0f}, r[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    Rd<vs, L>(s); Spfx<S, L>(s);
    Rd<vt, L>(t); Spfx<T, L>(t);
    r[0] = VecOp<o>(s[0], t[0]);
    if constexpr (L >= 2) r[1] = VecOp<o>(s[1], t[1]);
    if constexpr (L >= 3) r[2] = VecOp<o>(s[2], t[2]);
    if constexpr (L >= 4) r[3] = VecOp<o>(s[3], t[3]);
    Wr<vd, L, D>(r);
    Eat<S, T, D>();
}

// ---------------------------------------------------------------------------------------------------- vdot
// result = (((0 + s0 t0) + s1 t1) + s2 t2) + s3 t3 over a four-lane view (lanes >= L start as 0 before the prefixes)
template <uint32_t W, uint32_t S, uint32_t T, uint32_t D> static LEAN_INLINE void vdot() {
    constexpr uint32_t L = Len(W), vd = Vd(W), vs = Vs(W), vt = Vt(W);
    float r = 0.0f;
    if constexpr (S == kS0 && T == kT0) {
#if LEAN_HAVE_NEON
        if constexpr (L == 4u && Contig4(vs) && Contig4(vt)) {
            const float32x4_t p = vmulq_f32(vld1q_f32(&V(Ix(vs, 4, 0))), vld1q_f32(&V(Ix(vt, 4, 0))));
            r += vgetq_lane_f32(p, 0); r += vgetq_lane_f32(p, 1); r += vgetq_lane_f32(p, 2); r += vgetq_lane_f32(p, 3);
        } else
#endif
        {
            // lanes >= L are +0 on both sides, their products +0, and r (which starts at +0) is never -0: skipping them is exact
            float s[4] = {0.0f, 0.0f, 0.0f, 0.0f}, t[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            Rd<vs, L>(s); Rd<vt, L>(t);
            r += s[0] * t[0];
            if constexpr (L >= 2) r += s[1] * t[1];
            if constexpr (L >= 3) r += s[2] * t[2];
            if constexpr (L >= 4) r += s[3] * t[3];
        }
    } else {
        float s[4] = {0.0f, 0.0f, 0.0f, 0.0f}, t[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        Rd<vs, L>(s); Rd<vt, L>(t);
        Spfx<S, 4>(s); Spfx<T, 4>(t);
        r += s[0] * t[0]; r += s[1] * t[1]; r += s[2] * t[2]; r += s[3] * t[3];
    }
    const float res[4] = {r, 0.0f, 0.0f, 0.0f};
    Wr<vd, 1, D>(res);
    Eat<S, T, D>();
}

// ---------------------------------------------------------------------------------------------------- vscl
template <uint32_t W, uint32_t S, uint32_t T, uint32_t D> static LEAN_INLINE void vscl() {
    constexpr uint32_t L = Len(W), vd = Vd(W), vs = Vs(W), vt = Vt(W);
    constexpr uint32_t Tm = T & ~0xFFu;                 // every output lane selects lane 0 of the scalar
#if LEAN_HAVE_NEON
    if constexpr (L == 4u && S == kS0 && Tm == 0u && D == kD0 && Contig4(vs) && Contig4(vd)) {
        const float k = V(Ix(vt, 1, 0));
        vst1q_f32(&V(Ix(vd, 4, 0)), vmulq_n_f32(vld1q_f32(&V(Ix(vs, 4, 0))), k));
        Eat<S, T, D>();                                  // T may carry a pending prefix whose swizzle selects lane 0 anyway
        return;
    }
#endif
    float s[4] = {0.0f, 0.0f, 0.0f, 0.0f}, r[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    Rd<vs, L>(s); Spfx<S, L>(s);
    float t[4] = {V(Ix(vt, 1, 0)), 0.0f, 0.0f, 0.0f};
    Spfx<Tm, L>(t);
    r[0] = s[0] * t[0];
    if constexpr (L >= 2) r[1] = s[1] * t[1];
    if constexpr (L >= 3) r[2] = s[2] * t[2];
    if constexpr (L >= 4) r[3] = s[3] * t[3];
    Wr<vd, L, D>(r);
    Eat<S, T, D>();
}

// ---------------------------------------------------------------------------------------------------- vfad / vavg
template <uint32_t W, uint32_t S, uint32_t T, uint32_t D> static LEAN_INLINE void vhoriz() {
    constexpr uint32_t L = Len(W), vd = Vd(W), vs = Vs(W);
    constexpr bool avg = ((W >> 16u) & 31u) == 7u;
    constexpr uint32_t avg_prefix[4] = {0x0000F000u, 0x0000F0FFu, 0x0000FF55u, 0x0000FFAAu};
    constexpr uint32_t Tm = !avg ? ((T & ~0x000000FFu) | 0x0000F055u) : ((T & ~0x00000FFFu) | avg_prefix[L - 1u]);
    float s[4] = {0.0f, 0.0f, 0.0f, 0.0f}, w[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    Rd<vs, L>(s); Spfx<S, 4>(s);
    Spfx<Tm, 4>(w);
    float r = 0.0f;
    r += s[0] * w[0]; r += s[1] * w[1]; r += s[2] * w[2]; r += s[3] * w[3];
    const float res[4] = {r, 0.0f, 0.0f, 0.0f};
    Wr<vd, 1, D>(res);
    Eat<S, T, D>();
}

// ---------------------------------------------------------------------------------------------------- vscmp / vsge / vslt
template <uint32_t W, uint32_t S, uint32_t T, uint32_t D> static LEAN_INLINE void vcmp3() {
    constexpr uint32_t L = Len(W), vd = Vd(W), vs = Vs(W), vt = Vt(W);
    constexpr uint32_t op = (W >> 23u) & 7u;            // 5 vscmp, 6 vsge, 7 vslt
    static_assert(op >= 5u && op <= 7u, "vcmp3 needs vscmp / vsge / vslt");
    float s[4] = {0.0f, 0.0f, 0.0f, 0.0f}, t[4] = {0.0f, 0.0f, 0.0f, 0.0f}, r[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    Rd<vs, L>(s); Rd<vt, L>(t);
    Spfx<S, L>(s); Spfx<T, L>(t);
    auto lane = [&](auto idx) {
        constexpr uint32_t i = decltype(idx)::value;
        constexpr uint32_t ssw = (S >> (i * 2u)) & 3u, tsw = (T >> (i * 2u)) & 3u;
        constexpr bool sc = ((S >> (12u + i)) & 1u) != 0u, tc = ((T >> (12u + i)) & 1u) != 0u;
        if constexpr ((!sc && ssw >= L) || (!tc && tsw >= L)) {
            r[i] = 0.0f;                                 // an invalid swizzle is wired to an exact +0
        } else if constexpr (op == 5u) {
            const float difference = s[i] - t[i];
            if (__builtin_isnan(difference)) {
                const uint32_t sb = B(s[i]), tb = B(t[i]);
                const int64_t sm = static_cast<int64_t>(sb & 0x7FFFFFFFu), tm = static_cast<int64_t>(tb & 0x7FFFFFFFu);
                const int64_t os = (sb & 0x80000000u) != 0u ? -sm : sm, ot = (tb & 0x80000000u) != 0u ? -tm : tm;
                r[i] = os > ot ? 1.0f : os < ot ? -1.0f : 0.0f;
            } else {
                r[i] = difference > 0.0f ? 1.0f : difference < 0.0f ? -1.0f : 0.0f;
            }
        } else if constexpr (op == 6u) {
            r[i] = (!__builtin_isnan(s[i]) && !__builtin_isnan(t[i]) && s[i] >= t[i]) ? 1.0f : 0.0f;
        } else {
            r[i] = (!__builtin_isnan(s[i]) && !__builtin_isnan(t[i]) && s[i] < t[i]) ? 1.0f : 0.0f;
        }
    };
    lane(std::integral_constant<uint32_t, 0>{});
    if constexpr (L >= 2) lane(std::integral_constant<uint32_t, 1>{});
    if constexpr (L >= 3) lane(std::integral_constant<uint32_t, 2>{});
    if constexpr (L >= 4) lane(std::integral_constant<uint32_t, 3>{});
    Wr<vd, L, D>(r);
    Eat<S, T, D>();
}

// ---------------------------------------------------------------------------------------------------- unary group (vmov, vabs, vneg, vsat, vrcp, vrsq, ...)
template <uint32_t W, uint32_t S, uint32_t T, uint32_t D> static LEAN_INLINE void vunary() {
    constexpr uint32_t L = Len(W), vd = Vd(W), vs = Vs(W);
    constexpr uint32_t op = (W >> 16u) & 31u;
    float s[4] = {0.0f, 0.0f, 0.0f, 0.0f}, r[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    Rd<vs, L>(s); Spfx<S, L>(s);
    r[0] = lean_vf::unary_value(op, s[0]);
    if constexpr (L >= 2) r[1] = lean_vf::unary_value(op, s[1]);
    if constexpr (L >= 3) r[2] = lean_vf::unary_value(op, s[2]);
    if constexpr (L >= 4) r[3] = lean_vf::unary_value(op, s[3]);
    Wr<vd, L, D>(r);
    Eat<S, T, D>();
}

// vidt, vzero, vone
template <uint32_t W, uint32_t S, uint32_t T, uint32_t D> static LEAN_INLINE void vidt() {
    constexpr uint32_t L = Len(W), vd = Vd(W);
    float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    v[vd & (L >= 3u ? 3u : 1u)] = 1.0f;
    Wr<vd, L, D>(v);
    Eat<S, T, D>();
}
template <uint32_t W, uint32_t S, uint32_t T, uint32_t D> static LEAN_INLINE void vinit() {
    constexpr uint32_t L = Len(W), vd = Vd(W);
    constexpr float fill = ((W >> 16u) & 31u) == 7u ? 1.0f : 0.0f;
    const float v[4] = {fill, fill, fill, fill};
    Wr<vd, L, D>(v);
    Eat<S, T, D>();
}

// viim.s
template <uint32_t W, uint32_t S, uint32_t T, uint32_t D> static LEAN_INLINE void viim() {
    constexpr uint32_t vd = (W >> 16u) & 0x7Fu;
    const float v[4] = {static_cast<float>(static_cast<int16_t>(W & 0xFFFFu)), 0.0f, 0.0f, 0.0f};
    Wr<vd, 1, D>(v);
    Eat<S, T, D>();
}

// ---------------------------------------------------------------------------------------------------- float <-> integer
// vf2in / vf2iz / vf2iu / vf2id.  The rounding of the guest (round half even, trunc, ceil, floor) on a value already known to
// lie inside the int32 range: done with an int32 conversion and a correction instead of a libm call.
template <uint32_t Mode> static LEAN_INLINE int32_t F2iRound(double scaled) {
    if constexpr (Mode == 17u) {
        return static_cast<int32_t>(scaled);                                             // trunc
    } else if constexpr (Mode == 18u) {                                                  // ceil
        const int32_t i = static_cast<int32_t>(scaled);
        return static_cast<double>(i) < scaled ? i + 1 : i;
    } else if constexpr (Mode == 16u) {                                                  // round half to even
        const int32_t i = static_cast<int32_t>(scaled);                                  // toward zero
        const double frac = scaled - static_cast<double>(i);                             // exact, |frac| < 1, same sign as scaled
        if (frac > 0.5) return i + 1;
        if (frac < -0.5) return i - 1;
        if (frac == 0.5) return (i & 1) == 0 ? i : i + 1;
        if (frac == -0.5) return (i & 1) == 0 ? i : i - 1;
        return i;
    } else {                                                                             // floor
        const int32_t i = static_cast<int32_t>(scaled);
        return static_cast<double>(i) > scaled ? i - 1 : i;
    }
}
template <uint32_t Mode> static LEAN_INLINE uint32_t F2iLane(float x, float scale) {
    if (__builtin_isnan(x)) return 0x7FFFFFFFu;
    const double scaled = static_cast<double>(x * scale);
    if (scaled > 2147483647.0) return 0x7FFFFFFFu;
    if (scaled <= -2147483648.0) return 0x80000000u;
    return static_cast<uint32_t>(F2iRound<Mode>(scaled));
}
template <uint32_t W, uint32_t S, uint32_t T, uint32_t D> static LEAN_INLINE void vf2i() {
    constexpr uint32_t L = Len(W), vd = Vd(W), vs = Vs(W);
    constexpr uint32_t sexp = (W >> 16u) & 31u, mode = (W >> 21u) & 31u;
    constexpr uint32_t m = mode == 16u ? 16u : mode == 17u ? 17u : mode == 18u ? 18u : 19u;
    constexpr float scale = static_cast<float>(1u << sexp);          // 2^sexp (sexp <= 31): exact
    float s[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    Rd<vs, L>(s); Spfx<S, L>(s);
    uint32_t r[4] = {0u, 0u, 0u, 0u};
    r[0] = F2iLane<m>(s[0], scale);
    if constexpr (L >= 2) r[1] = F2iLane<m>(s[1], scale);
    if constexpr (L >= 3) r[2] = F2iLane<m>(s[2], scale);
    if constexpr (L >= 4) r[3] = F2iLane<m>(s[3], scale);
    // only the write mask applies here (no saturation)
    if constexpr (((D >> 8u) & 1u) == 0u) V(Ix(vd, L, 0)) = F(r[0]);
    if constexpr (L >= 2 && ((D >> 9u) & 1u) == 0u) V(Ix(vd, L, 1)) = F(r[1]);
    if constexpr (L >= 3 && ((D >> 10u) & 1u) == 0u) V(Ix(vd, L, 2)) = F(r[2]);
    if constexpr (L >= 4 && ((D >> 11u) & 1u) == 0u) V(Ix(vd, L, 3)) = F(r[3]);
    Eat<S, T, D>();
}
template <uint32_t W, uint32_t S, uint32_t T, uint32_t D> static LEAN_INLINE void vi2f() {
    constexpr uint32_t L = Len(W), vd = Vd(W), vs = Vs(W);
    constexpr uint32_t sh = (W >> 16u) & 31u;
    constexpr float scale = 1.0f / static_cast<float>(1u << sh);     // 2^-sh: exact
    float s[4] = {0.0f, 0.0f, 0.0f, 0.0f}, r[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    Rd<vs, L>(s); Spfx<S, L>(s);
    r[0] = static_cast<float>(static_cast<int32_t>(B(s[0]))) * scale;
    if constexpr (L >= 2) r[1] = static_cast<float>(static_cast<int32_t>(B(s[1]))) * scale;
    if constexpr (L >= 3) r[2] = static_cast<float>(static_cast<int32_t>(B(s[2]))) * scale;
    if constexpr (L >= 4) r[3] = static_cast<float>(static_cast<int32_t>(B(s[3]))) * scale;
    Wr<vd, L, D>(r);
    Eat<S, T, D>();
}
// vi2uc (Vi2x with sub-op 28): four signed integers -> one word of four bytes
template <uint32_t W, uint32_t S, uint32_t T, uint32_t D> static LEAN_INLINE void vi2uc() {
    constexpr uint32_t L = Len(W), vd = Vd(W), vs = Vs(W);
    float s[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    Rd<vs, L>(s); Spfx<S, L>(s);
    uint32_t packed = 0u;
    auto lane = [&](uint32_t i) {
        const int32_t value = static_cast<int32_t>(B(s[i]));
        const uint32_t byte = value < 0 ? 0u : (static_cast<uint32_t>(value) >> 23u) & 0xFFu;
        packed |= byte << (i * 8u);
    };
    lane(0);
    if constexpr (L >= 2) lane(1);
    if constexpr (L >= 3) lane(2);
    if constexpr (L >= 4) lane(3);
    const float res[4] = {F(packed), 0.0f, 0.0f, 0.0f};
    Wr<vd, 1, D>(res);
    Eat<S, T, D>();
}
// vi2c (Extra): the top byte of each integer
template <uint32_t W, uint32_t S, uint32_t T, uint32_t D> static LEAN_INLINE void vi2c() {
    constexpr uint32_t L = Len(W), vd = Vd(W), vs = Vs(W);
    float s[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    Rd<vs, L>(s); Spfx<S, L>(s);
    uint32_t packed = (B(s[0]) >> 24u);
    if constexpr (L >= 2) packed |= (B(s[1]) >> 24u) << 8u;
    if constexpr (L >= 3) packed |= (B(s[2]) >> 24u) << 16u;
    if constexpr (L >= 4) packed |= (B(s[3]) >> 24u) << 24u;
    const float res[4] = {F(packed), 0.0f, 0.0f, 0.0f};
    Wr<vd, 1, D>(res);
    Eat<S, T, D>();
}

// ---------------------------------------------------------------------------------------------------- matrices
template <uint32_t W, uint32_t S, uint32_t T, uint32_t D> static LEAN_INLINE void vmmul() {
    constexpr uint32_t side = Len(W), vd = Vd(W), vs = Vs(W), vt = Vt(W);
#if LEAN_HAVE_NEON
    if constexpr (side == 4u && ContigM4(vs) && ContigM4(vt) && ContigM4(vd)) {
        // result column a = sum over c (in order, starting from +0) of source column-vector c * target[a][c]
        const float32x4x4_t sm = vld4q_f32(&V(MIx(vs, 4, 0, 0)));
        const float32x4_t zero = vdupq_n_f32(0.0f);
        float32x4_t out[4];
        for (uint32_t a = 0; a < 4; ++a) {
            const float32x4_t tq = vld1q_f32(&V(MIx(vt, 4, a, 0)));
            float32x4_t acc = vaddq_f32(zero, vmulq_n_f32(sm.val[0], vgetq_lane_f32(tq, 0)));
            acc = vaddq_f32(acc, vmulq_n_f32(sm.val[1], vgetq_lane_f32(tq, 1)));
            acc = vaddq_f32(acc, vmulq_n_f32(sm.val[2], vgetq_lane_f32(tq, 2)));
            acc = vaddq_f32(acc, vmulq_n_f32(sm.val[3], vgetq_lane_f32(tq, 3)));
            out[a] = acc;
        }
        for (uint32_t a = 0; a < 4; ++a) vst1q_f32(&V(MIx(vd, 4, a, 0)), out[a]);
        Eat<S, T, D>();
        return;
    }
#endif
    float s[16] = {}, t[16] = {}, r[16] = {};
    for (uint32_t j = 0; j < side; ++j)
        for (uint32_t i = 0; i < side; ++i) { s[j * 4u + i] = V(MIx(vs, side, j, i)); t[j * 4u + i] = V(MIx(vt, side, j, i)); }
    for (uint32_t a = 0; a < side; ++a)
        for (uint32_t b = 0; b < side; ++b) {
            float sum = 0.0f;
            for (uint32_t c = 0; c < side; ++c) sum += s[b * 4u + c] * t[a * 4u + c];
            r[a * 4u + b] = sum;
        }
    for (uint32_t j = 0; j < side; ++j)
        for (uint32_t i = 0; i < side; ++i) V(MIx(vd, side, j, i)) = r[j * 4u + i];
    Eat<S, T, D>();
}

template <uint32_t W, uint32_t S, uint32_t T, uint32_t D> static LEAN_INLINE void vtfm() {
    constexpr uint32_t side = ((W >> 23u) & 3u) + 1u, input_length = Len(W), vd = Vd(W), vs = Vs(W), vt = Vt(W);
    float matrix[16] = {}, target_raw[4] = {0.0f, 0.0f, 0.0f, 0.0f}, target[4] = {0.0f, 0.0f, 0.0f, 0.0f}, result[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (uint32_t j = 0; j < side; ++j)
        for (uint32_t i = 0; i < side; ++i) matrix[j * 4u + i] = V(MIx(vs, side, j, i));
    for (uint32_t lane = 0; lane < side; ++lane) target_raw[lane] = V(Ix(vt, side, lane));
    for (uint32_t lane = 0; lane < 4u; ++lane) target[lane] = lane < input_length ? target_raw[lane] : 0.0f;
    if (side - 1u >= input_length) target[side - 1u] = 1.0f;
    for (uint32_t row = 0; row + 1u < side; ++row) {
        float sum = 0.0f;
        for (uint32_t column = 0; column < side; ++column) sum += matrix[row * 4u + column] * target[column];
        result[row] = sum;
    }
    float final_row[4] = {matrix[(side - 1u) * 4u + 0u], matrix[(side - 1u) * 4u + 1u], matrix[(side - 1u) * 4u + 2u], matrix[(side - 1u) * 4u + 3u]};
    Spfx<S, 4>(final_row);
    Spfx<T, 4>(target);
    for (uint32_t column = 0; column < 4u; ++column) result[side - 1u] += final_row[column] * target[column];
    constexpr uint32_t last_lane = side - 1u;
    constexpr uint32_t Dm = ((D & (1u << 8u)) << last_lane) | ((D & 3u) << (last_lane * 2u));
    Wr<vd, side, Dm>(result);
    Eat<S, T, D>();
}

template <uint32_t W, uint32_t S, uint32_t T, uint32_t D> static LEAN_INLINE void vmscl() {
    constexpr uint32_t side = Len(W), vd = Vd(W), vs = Vs(W), vt = Vt(W);
    float source[16] = {}, target[4] = {0.0f, 0.0f, 0.0f, 0.0f}, result[16] = {}, previous[16] = {};
    for (uint32_t j = 0; j < side; ++j)
        for (uint32_t i = 0; i < side; ++i) { source[j * 4u + i] = V(MIx(vs, side, j, i)); previous[j * 4u + i] = V(MIx(vd, side, j, i)); }
    target[0] = V(Ix(vt, 1, 0));
    const float scalar = target[0];
    for (uint32_t row = 0; row + 1u < side; ++row)
        for (uint32_t column = 0; column < side; ++column) result[row * 4u + column] = source[row * 4u + column] * scalar;
    constexpr uint32_t last_row = side - 1u;
    float final_row[4] = {source[last_row * 4u + 0u], source[last_row * 4u + 1u], source[last_row * 4u + 2u], source[last_row * 4u + 3u]};
    Spfx<S, 4>(final_row);
    constexpr uint32_t target_lane = (vt >> 5u) & 3u;
    target[target_lane] = scalar;
    constexpr uint32_t Tm = (T & ~0xFFu) | (target_lane * 0x55u);
    Spfx<Tm, 4>(target);
    for (uint32_t column = 0; column < side; ++column) {
        float value = final_row[column] * target[column];
        if (((D >> (8u + column)) & 1u) != 0u) value = previous[last_row * 4u + column];
        else {
            const uint32_t saturation = (D >> (column * 2u)) & 3u;
            if (saturation == 1u) value = fminf(1.0f, fmaxf(0.0f, value));
            else if (saturation == 3u) value = fminf(1.0f, fmaxf(-1.0f, value));
        }
        result[last_row * 4u + column] = value;
    }
    for (uint32_t j = 0; j < side; ++j)
        for (uint32_t i = 0; i < side; ++i) V(MIx(vd, side, j, i)) = result[j * 4u + i];
    Eat<S, T, D>();
}

} // namespace lean_vk
