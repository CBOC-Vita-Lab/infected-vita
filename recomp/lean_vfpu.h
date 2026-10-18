// lean_vfpu.h - generic VFPU lowering helpers (step 1: correctness first).
//
// Every helper executes ONE guest VFPU instruction `w` on g_cpu.vfpu / g_cpu.vfpu_ctrl with the interpreter's exact
// semantics: the bodies below are PortableKit's src/interpreter.cpp (execute_simple) and src/extra_instructions.cpp
// (execute_extra_instruction) cases, ported unchanged except that `ctx` is g_cpu and guest memory is the flat arena.
// `w` is always a compile-time constant at the call sites, so with LEAN_INLINE the field extraction folds away.
//
// Included from lean_abi.h; do not include directly.
#pragma once

#include <bit>
#include <algorithm>
#include <cmath>

namespace lean_vf {

using psprecomp::AllegrexContext;

static LEAN_INLINE AllegrexContext& cx() { return g_cpu; }

static LEAN_INLINE uint32_t length_of(uint32_t w) { return (((w >> 7u) & 1u) | (((w >> 15u) & 1u) << 1u)) + 1u; }
static LEAN_INLINE uint32_t vd_of(uint32_t w) { return w & 0x7Fu; }
static LEAN_INLINE uint32_t vs_of(uint32_t w) { return (w >> 8u) & 0x7Fu; }
static LEAN_INLINE uint32_t vt_of(uint32_t w) { return (w >> 16u) & 0x7Fu; }
static LEAN_INLINE uint32_t bits(float v) { return std::bit_cast<uint32_t>(v); }
static LEAN_INLINE float from_bits(uint32_t v) { return std::bit_cast<float>(v); }

// VFIM binary16 literal expansion (interpreter.cpp expand_vfim_half)
static inline uint32_t expand_vfim_half(uint32_t half) {
    const uint32_t sign = (half & 0x8000u) << 16u;
    const uint32_t exponent = (half >> 10u) & 0x1Fu;
    uint32_t mantissa = half & 0x03FFu;
    if (exponent == 0u) {
        if (mantissa == 0u) return sign;
        uint32_t shift = 0u;
        while ((mantissa & 0x0400u) == 0u) { mantissa <<= 1u; ++shift; }
        mantissa &= 0x03FFu;
        return sign | ((113u - shift) << 23u) | (mantissa << 13u);
    }
    if (exponent == 31u) return sign | 0x7F800000u | (mantissa << 13u);
    return sign | ((exponent + 112u) << 23u) | (mantissa << 13u);
}

static inline float unary_value(uint32_t operation, float value) {
    constexpr float half_pi = 1.57079632679489661923f;
    constexpr float two_over_pi = 0.63661977236758134308f;
    switch (operation) {
    case 0u: return value;                                              // vmov
    case 1u: return std::fabs(value);                                   // vabs
    case 2u: return -value;                                             // vneg
    case 4u: return value <= 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);  // vsat0
    case 5u: return value < -1.0f ? -1.0f : (value > 1.0f ? 1.0f : value); // vsat1
    case 16u: return 1.0f / value;                                      // vrcp
    case 17u: return 1.0f / std::sqrt(value);                           // vrsq
    case 18u: return std::sin(value * half_pi);                         // vsin
    case 19u: return std::cos(value * half_pi);                         // vcos
    case 20u: return std::exp2(value);                                  // vexp2
    case 21u: return std::log2(value);                                  // vlog2
    case 22u: return std::fabs(std::sqrt(value));                       // vsqrt
    case 23u: return std::asin(value) * two_over_pi;                    // vasin
    case 24u: return -1.0f / value;                                     // vnrcp
    case 26u: return -std::sin(value * half_pi);                        // vnsin
    default: return 1.0f / std::exp2(value);                            // vrexp2
    }
}

// ------------------------------------------------------------------------------------------- prefix / flush / immediates
static LEAN_INLINE void vflush(uint32_t w) { if ((w & 0xFFFF0000u) != 0xFFFF0000u) cx().eat_vfpu_prefixes(); }
static LEAN_INLINE void vpfx(uint32_t w) {
    const uint32_t control = (w >> 24u) & 3u;
    uint32_t data = w & 0x000FFFFFu;
    if (control == 2u) data &= 0x00000FFFu;
    cx().vfpu_ctrl[control] = data;
}
static LEAN_INLINE void viim(uint32_t w) {
    const float value[1]{static_cast<float>(static_cast<int16_t>(w & 0xFFFFu))};
    cx().write_vfpu_vector_with_destination_prefix(value, (w >> 16u) & 0x7Fu, 1u);
}
static LEAN_INLINE void vfim(uint32_t w) {
    const float value[1]{from_bits(expand_vfim_half(w & 0xFFFFu))};
    cx().write_vfpu_vector_with_destination_prefix(value, (w >> 16u) & 0x7Fu, 1u);
}
static LEAN_INLINE void mtv(uint32_t w, uint32_t v) { cx().set_vfpu_scalar_bits(w & 0xFFu, v); }
static LEAN_INLINE uint32_t mfv(uint32_t w) { return cx().vfpu_scalar_bits(w & 0xFFu); }

// ------------------------------------------------------------------------------------------- converts
static LEAN_INLINE void vf2h(uint32_t w) { cx().execute_vfpu_vf2h(vd_of(w), vs_of(w), length_of(w)); }
static LEAN_INLINE void vh2f(uint32_t w) { cx().execute_vfpu_vh2f(vd_of(w), vs_of(w), length_of(w)); }
static LEAN_INLINE void vi2x(uint32_t w) {
    if (((w >> 16u) & 31u) == 28u) cx().execute_vfpu_vi2uc(vd_of(w), vs_of(w), length_of(w));
    else cx().execute_vfpu_vi2s(vd_of(w), vs_of(w), length_of(w));
}
static LEAN_INLINE void vt5650(uint32_t w) { cx().execute_vfpu_vt5650(vd_of(w), vs_of(w), length_of(w)); }
static LEAN_INLINE void vrnd(uint32_t w) { cx().execute_vfpu_vrnd(vd_of(w), length_of(w), (w >> 16u) & 31u); }
static LEAN_INLINE void vx2i(uint32_t w) { cx().execute_vfpu_vx2i(vd_of(w), vs_of(w), length_of(w), (w >> 16u) & 3u); }

static inline void vf2i(uint32_t w) {
    AllegrexContext& ctx = cx();
    const uint32_t length = length_of(w);
    const uint32_t destination = vd_of(w);
    const uint32_t scale_exponent = (w >> 16u) & 31u;
    const uint32_t mode = (w >> 21u) & 31u;
    float source[4]{};
    int32_t result[4]{};
    ctx.read_vfpu_vector(source, vs_of(w), length);
    ctx.apply_vfpu_source_prefix(source, length, 0u);
    const float scale = std::ldexp(1.0f, static_cast<int>(scale_exponent));
    for (uint32_t lane = 0u; lane < length; ++lane) {
        if (std::isnan(source[lane])) { result[lane] = std::numeric_limits<int32_t>::max(); continue; }
        const auto scaled = static_cast<double>(source[lane] * scale);
        if (scaled > static_cast<double>(std::numeric_limits<int32_t>::max())) {
            result[lane] = std::numeric_limits<int32_t>::max();
        } else if (scaled <= static_cast<double>(std::numeric_limits<int32_t>::min())) {
            result[lane] = std::numeric_limits<int32_t>::min();
        } else {
            double rounded = 0.0;
            switch (mode) {
            case 16u: rounded = AllegrexContext::round_ties_to_even(scaled); break;
            case 17u: rounded = std::trunc(scaled); break;
            case 18u: rounded = std::ceil(scaled); break;
            default: rounded = std::floor(scaled); break;
            }
            result[lane] = static_cast<int32_t>(rounded);
        }
    }
    const uint32_t destination_prefix = ctx.vfpu_ctrl[2];
    for (uint32_t lane = 0u; lane < length; ++lane) {
        if (((destination_prefix >> (8u + lane)) & 1u) != 0u) continue;
        ctx.vfpu[AllegrexContext::vfpu_vector_lane_index(destination, length, lane)] =
            std::bit_cast<float>(static_cast<uint32_t>(result[lane]));
    }
    ctx.eat_vfpu_prefixes();
}
static inline void vi2f(uint32_t w) {
    AllegrexContext& ctx = cx();
    const uint32_t length = length_of(w);
    float source[4]{};
    float result[4]{};
    ctx.read_vfpu_vector(source, vs_of(w), length);
    ctx.apply_vfpu_source_prefix(source, length, 0u);
    const float scale = std::ldexp(1.0f, -static_cast<int>((w >> 16u) & 31u));
    for (uint32_t lane = 0u; lane < length; ++lane) {
        const auto integer = static_cast<int32_t>(std::bit_cast<uint32_t>(source[lane]));
        result[lane] = static_cast<float>(integer) * scale;
    }
    ctx.write_vfpu_vector_with_destination_prefix(result, vd_of(w), length);
}

// ------------------------------------------------------------------------------------------- matrices
static LEAN_INLINE void vmidt(uint32_t w) { cx().execute_vfpu_matrix_init(vd_of(w), length_of(w), 3u); }
static LEAN_INLINE void vmatrix_init(uint32_t w) { cx().execute_vfpu_matrix_init(vd_of(w), length_of(w), (w >> 16u) & 15u); }
static LEAN_INLINE void vmmov(uint32_t w) { cx().execute_vfpu_vmmov(vd_of(w), vs_of(w), length_of(w)); }
static inline void vmmul(uint32_t w) {
    AllegrexContext& ctx = cx();
    const uint32_t side = length_of(w);
    float source[16]{};
    float target[16]{};
    float result[16]{};
    ctx.read_vfpu_matrix(source, vs_of(w), side);
    ctx.read_vfpu_matrix(target, vt_of(w), side);
    for (uint32_t a = 0u; a < side; ++a) {
        for (uint32_t b = 0u; b < side; ++b) {
            float sum = 0.0f;
            for (uint32_t c = 0u; c < side; ++c) sum += source[b * 4u + c] * target[a * 4u + c];
            result[a * 4u + b] = sum;
        }
    }
    ctx.write_vfpu_matrix(result, vd_of(w), side);
    ctx.eat_vfpu_prefixes();
}
static LEAN_INLINE void vmscl(uint32_t w) { cx().execute_vfpu_vmscl(vd_of(w), vs_of(w), vt_of(w), length_of(w)); }
static inline void vtfm(uint32_t w) {
    AllegrexContext& ctx = cx();
    const uint32_t side = ((w >> 23u) & 3u) + 1u;
    const uint32_t input_length = length_of(w);
    float matrix[16]{};
    float target_raw[4]{};
    float target[4]{};
    float result[4]{};
    ctx.read_vfpu_matrix(matrix, vs_of(w), side);
    ctx.read_vfpu_vector(target_raw, vt_of(w), side);
    for (uint32_t lane = 0u; lane < 4u; ++lane) target[lane] = lane < input_length ? target_raw[lane] : 0.0f;
    // VTFM/VHTFM implies a homogeneous 1 in the lane the encoded input omits.
    if (side - 1u >= input_length) target[side - 1u] = 1.0f;
    for (uint32_t row = 0u; row + 1u < side; ++row) {
        float sum = 0.0f;
        for (uint32_t column = 0u; column < side; ++column) sum += matrix[row * 4u + column] * target[column];
        result[row] = sum;
    }
    float final_row[4]{matrix[(side - 1u) * 4u + 0u], matrix[(side - 1u) * 4u + 1u],
                       matrix[(side - 1u) * 4u + 2u], matrix[(side - 1u) * 4u + 3u]};
    ctx.apply_vfpu_source_prefix(final_row, 4u, 0u);
    ctx.apply_vfpu_source_prefix(target, 4u, 1u);
    for (uint32_t column = 0u; column < 4u; ++column) result[side - 1u] += final_row[column] * target[column];
    const uint32_t destination_prefix = ctx.vfpu_ctrl[2];
    const uint32_t last_lane = side - 1u;
    ctx.vfpu_ctrl[2] = ((destination_prefix & (1u << 8u)) << last_lane) | ((destination_prefix & 3u) << (last_lane * 2u));
    ctx.write_vfpu_vector_with_destination_prefix(result, vd_of(w), side);
}

// ------------------------------------------------------------------------------------------- vector forms
static inline void vidt(uint32_t w) {
    const uint32_t length = length_of(w);
    const uint32_t destination = vd_of(w);
    float value[4]{};
    value[destination & (length >= 3u ? 3u : 1u)] = 1.0f;
    cx().write_vfpu_vector_with_destination_prefix(value, destination, length);
}
static inline void vvector_init(uint32_t w) {
    const bool one = ((w >> 16u) & 31u) == 7u;
    const float fill = one ? 1.0f : 0.0f;
    const float value[4]{fill, fill, fill, fill};
    cx().write_vfpu_vector_with_destination_prefix(value, vd_of(w), length_of(w));
}
static inline void vcst(uint32_t w) {
    static constexpr uint32_t constant_bits[32] = {
        0x00000000u, 0x7F7FFFFFu, 0x3FB504F3u, 0x3F3504F3u, 0x3F906EBAu, 0x3F22F983u, 0x3EA2F983u, 0x3F490FDBu,
        0x3FC90FDBu, 0x40490FDBu, 0x402DF854u, 0x3FB8AA3Bu, 0x3EDE5BD9u, 0x3F317218u, 0x40135D8Eu, 0x40C90FDBu,
        0x3F060A92u, 0x3E9A209Bu, 0x40549A78u, 0x3F5DB3D7u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
    };
    const float constant = from_bits(constant_bits[(w >> 16u) & 31u]);
    const float value[4]{constant, constant, constant, constant};
    cx().write_vfpu_vector_with_destination_prefix(value, vd_of(w), length_of(w));
}
static LEAN_INLINE void vocp(uint32_t w) { cx().execute_vfpu_vocp(vd_of(w), vs_of(w), length_of(w)); }
static LEAN_INLINE void vhorizontal(uint32_t w) {
    cx().execute_vfpu_horizontal(vd_of(w), vs_of(w), length_of(w), ((w >> 16u) & 31u) == 7u);
}
static LEAN_INLINE void vrot(uint32_t w) { cx().execute_vfpu_vrot(vd_of(w), vs_of(w), length_of(w), (w >> 16u) & 31u); }
static inline void vvec3(uint32_t w) {
    AllegrexContext& ctx = cx();
    const uint32_t length = length_of(w);
    // VMUL shares opcode 0x19 with the dot/scale group, so it does not use the sub-operation field.
    const uint32_t operation = (w >> 26u) == 0x19u ? 2u : ((w >> 23u) & 7u);
    float source[4]{};
    float target[4]{};
    float result[4]{};
    ctx.read_vfpu_vector_with_source_prefix(source, vs_of(w), length, 0u);
    ctx.read_vfpu_vector_with_source_prefix(target, vt_of(w), length, 1u);
    for (uint32_t lane = 0u; lane < length; ++lane) {
        result[lane] = operation == 0u ? source[lane] + target[lane]
                     : operation == 1u ? source[lane] - target[lane]
                     : operation == 2u ? source[lane] * target[lane]
                                       : source[lane] / target[lane];
    }
    ctx.write_vfpu_vector_with_destination_prefix(result, vd_of(w), length);
}
static LEAN_INLINE void vscl(uint32_t w) { cx().execute_vfpu_vscl(vd_of(w), vs_of(w), vt_of(w), length_of(w)); }
static LEAN_INLINE void vdot(uint32_t w) { cx().execute_vfpu_vdot(vd_of(w), vs_of(w), vt_of(w), length_of(w)); }
static LEAN_INLINE void vhdp(uint32_t w) { cx().execute_vfpu_vhdp(vd_of(w), vs_of(w), vt_of(w), length_of(w)); }
static inline void vcrs(uint32_t w) {
    AllegrexContext& ctx = cx();
    float source[4]{};
    float target[4]{};
    ctx.read_vfpu_vector_with_source_prefix(source, vs_of(w), 3u, 0u);
    ctx.read_vfpu_vector_with_source_prefix(target, vt_of(w), 3u, 1u);
    const float result[4]{source[1] * target[2], source[2] * target[0], source[0] * target[1], 0.0f};
    ctx.write_vfpu_vector_with_destination_prefix(result, vd_of(w), 3u);
}
static LEAN_INLINE void vcross_quat(uint32_t w) { cx().execute_vfpu_cross_quat(vd_of(w), vs_of(w), vt_of(w), length_of(w)); }
static LEAN_INLINE void vminmax(uint32_t w) {
    cx().execute_vfpu_vminmax(vd_of(w), vs_of(w), vt_of(w), length_of(w), ((w >> 23u) & 7u) == 3u);
}
static LEAN_INLINE void vcompare3(uint32_t w) {
    cx().execute_vfpu_compare3(vd_of(w), vs_of(w), vt_of(w), length_of(w), (w >> 23u) & 7u);
}
static LEAN_INLINE void vcmp(uint32_t w) { cx().execute_vfpu_vcmp(vs_of(w), vt_of(w), length_of(w), w & 15u); }
static LEAN_INLINE void vcmov(uint32_t w) {
    cx().execute_vfpu_vcmov(vd_of(w), vs_of(w), length_of(w), (w >> 16u) & 7u, ((w >> 19u) & 1u) != 0u);
}
static inline void vunary(uint32_t w) {
    AllegrexContext& ctx = cx();
    const uint32_t length = length_of(w);
    const uint32_t operation = (w >> 16u) & 31u;
    float source[4]{};
    float result[4]{};
    ctx.read_vfpu_vector_with_source_prefix(source, vs_of(w), length, 0u);
    for (uint32_t lane = 0u; lane < length; ++lane) result[lane] = unary_value(operation, source[lane]);
    ctx.write_vfpu_vector_with_destination_prefix(result, vd_of(w), length);
}
static inline void vsgn(uint32_t w) {
    AllegrexContext& ctx = cx();
    const uint32_t length = length_of(w);
    float source[4]{};
    float result[4]{};
    ctx.read_vfpu_vector_with_source_prefix(source, vs_of(w), length, 0u);
    for (uint32_t lane = 0u; lane < length; ++lane)
        result[lane] = source[lane] > 0.0f ? 1.0f : (source[lane] < 0.0f ? -1.0f : 0.0f);
    ctx.write_vfpu_vector_with_destination_prefix(result, vd_of(w), length);
}
static inline void vbfy1(uint32_t w, uint32_t pc) {
    AllegrexContext& ctx = cx();
    const uint32_t length = length_of(w);
    if (length != 2u && length != 4u) pk_fatal("vbfy1 with single/triple source", pc);
    float source[4]{};
    float result[4]{};
    ctx.read_vfpu_vector_with_source_prefix(source, vs_of(w), length, 0u);
    for (uint32_t lane = 0u; lane < length; lane += 2u) {
        result[lane] = source[lane] + source[lane + 1u];
        result[lane + 1u] = source[lane] - source[lane + 1u];
    }
    ctx.write_vfpu_vector_with_destination_prefix(result, vd_of(w), length);
}
static inline void vsocp(uint32_t w, uint32_t pc) {
    AllegrexContext& ctx = cx();
    const uint32_t length = length_of(w);
    if (length > 2u) pk_fatal("vsocp with triple/quad source", pc);
    float source[4]{};
    float result[4]{};
    ctx.read_vfpu_vector_with_source_prefix(source, vs_of(w), length, 0u);
    for (uint32_t lane = 0u; lane < length; ++lane) {
        const float inverse = 1.0f - source[lane];
        result[lane * 2u] = inverse < 0.0f ? 0.0f : (inverse > 1.0f ? 1.0f : inverse);
        result[lane * 2u + 1u] = source[lane] < 0.0f ? 0.0f : (source[lane] > 1.0f ? 1.0f : source[lane]);
    }
    ctx.write_vfpu_vector_with_destination_prefix(result, vd_of(w), length * 2u);
}

// ------------------------------------------------------------------------------------------- memory forms
// (base + offset has been added by the caller: `a` is the guest address)
static LEAN_INLINE uint32_t scalar_of_mem(uint32_t w) { return ((w >> 16u) & 0x1Fu) | ((w & 3u) << 5u); }
static LEAN_INLINE uint32_t vector_of_mem(uint32_t w) { return ((w >> 16u) & 0x1Fu) | ((w & 1u) << 5u); }
static LEAN_INLINE void lvs(uint32_t w, uint32_t a) { cx().set_vfpu_scalar_bits(scalar_of_mem(w), lean_lw(a)); }
static LEAN_INLINE void svs(uint32_t w, uint32_t a) { lean_sw(a, cx().vfpu_scalar_bits(scalar_of_mem(w))); }
static LEAN_INLINE void lvq(uint32_t w, uint32_t a) {
    const float value[4]{from_bits(lean_lw(a)), from_bits(lean_lw(a + 4u)), from_bits(lean_lw(a + 8u)), from_bits(lean_lw(a + 12u))};
    cx().write_vfpu_vector(value, vector_of_mem(w), 4u);
}
static LEAN_INLINE void svq(uint32_t w, uint32_t a) {
    float value[4]{};
    cx().read_vfpu_vector(value, vector_of_mem(w), 4u);
    for (uint32_t lane = 0u; lane < 4u; ++lane) lean_sw(a + lane * 4u, bits(value[lane]));
}

// ------------------------------------------------------------------------------------------- extra instructions
// (extra_instructions.cpp; the "first use" diagnostic is dropped)
static inline void write_first_lane_result(uint32_t w, float* source, float first) {
    source[0] = first;
    cx().write_vfpu_vector_with_destination_prefix(source, vd_of(w), length_of(w));
}
static inline uint32_t pack_colour(uint32_t colour, bool is_4444) {
    const uint32_t r = colour & 0xFFu, g = (colour >> 8u) & 0xFFu, b = (colour >> 16u) & 0xFFu, a = (colour >> 24u) & 0xFFu;
    if (is_4444) return ((a >> 4u) << 12u) | ((b >> 4u) << 8u) | ((g >> 4u) << 4u) | (r >> 4u);
    return ((a >> 7u) << 15u) | ((b >> 3u) << 10u) | ((g >> 3u) << 5u) | (r >> 3u);
}
static LEAN_INLINE float lane_min(float a, float b) { return std::fmin(a, b); }
static LEAN_INLINE float lane_max(float a, float b) { return std::fmax(a, b); }

// lvl.q / lvr.q / svl.q / svr.q; `a` = rs + (offset & 0xFFFC)
static inline void quad_unaligned(uint32_t w, uint32_t a, int which /*0 lvl 1 lvr 2 svl 3 svr*/) {
    AllegrexContext& ctx = cx();
    const uint32_t vector = vector_of_mem(w);
    const uint32_t offset = (a >> 2u) & 3u;
    float lanes[4]{};
    ctx.read_vfpu_vector(lanes, vector, 4u);
    if (which == 0) {
        for (uint32_t i = 0u; i <= offset; ++i) lanes[3u - i] = from_bits(lean_lw(a - 4u * i));
        ctx.write_vfpu_vector(lanes, vector, 4u);
    } else if (which == 1) {
        for (uint32_t i = 0u; i <= 3u - offset; ++i) lanes[i] = from_bits(lean_lw(a + 4u * i));
        ctx.write_vfpu_vector(lanes, vector, 4u);
    } else if (which == 2) {
        for (uint32_t i = 0u; i <= offset; ++i) lean_sw(a - 4u * i, bits(lanes[3u - i]));
    } else {
        for (uint32_t i = 0u; i <= 3u - offset; ++i) lean_sw(a + 4u * i, bits(lanes[i]));
    }
}

static inline void vsbn(uint32_t w) {
    AllegrexContext& ctx = cx();
    const uint32_t length = length_of(w);
    float source[4]{}, target[4]{};
    ctx.read_vfpu_vector_with_source_prefix(source, vs_of(w), length, 0u);
    ctx.read_vfpu_vector_with_source_prefix(target, vt_of(w), length, 1u);
    const uint32_t s = bits(source[0]);
    const uint32_t exponent = (127u + bits(target[0])) & 0xFFu;
    const uint32_t old_exponent = s & 0x7F800000u;
    const uint32_t result = old_exponent != 0u && old_exponent != 0x7F800000u ? (s & ~0x7F800000u) | (exponent << 23u) : s;
    write_first_lane_result(w, source, from_bits(result));
}
static inline void vdet(uint32_t w) {
    AllegrexContext& ctx = cx();
    const uint32_t length = length_of(w);
    float source[4]{}, target[4]{};
    ctx.read_vfpu_vector_with_source_prefix(source, vs_of(w), length, 0u);
    ctx.read_vfpu_vector_with_source_prefix(target, vt_of(w), length, 1u);
    const float result[4]{source[0] * target[1] - source[1] * target[0], 0.0f, 0.0f, 0.0f};
    ctx.write_vfpu_vector_with_destination_prefix(result, vd_of(w), 1u);
}
static inline void vwbn(uint32_t w) {
    AllegrexContext& ctx = cx();
    const uint32_t length = length_of(w);
    float source[4]{};
    ctx.read_vfpu_vector_with_source_prefix(source, vs_of(w), length, 0u);
    const uint32_t exponent = (w >> 16u) & 0xFFu;
    const uint32_t s = bits(source[0]);
    const uint32_t old_exponent = (s >> 23u) & 0xFFu;
    uint32_t result = s | (exponent << 23u);
    if (old_exponent != 0u && old_exponent != 0xFFu) {
        uint32_t mantissa = (s & 0x007FFFFFu) | 0x00800000u;
        if (exponent > old_exponent) mantissa >>= (exponent - old_exponent) & 0xFu;
        else mantissa <<= (old_exponent - exponent) & 0xFu;
        result = (s & 0x80000000u) | (mantissa & 0x007FFFFFu) | (exponent << 23u);
    }
    write_first_lane_result(w, source, from_bits(result));
}
static inline void vrnds(uint32_t w) {
    AllegrexContext& ctx = cx();
    float seed[4]{};
    ctx.read_vfpu_vector_with_source_prefix(seed, vd_of(w), 1u, 0u);
    const uint32_t value = bits(seed[0]);
    ctx.vfpu_ctrl[8] = value ^ 0x9E3779B9u;
    ctx.vfpu_ctrl[9] = value * 0x85EBCA6Bu + 0x243F6A88u;
    ctx.vfpu_ctrl[10] = ~value;
    ctx.vfpu_ctrl[11] = value + 0xB7E15162u;
    ctx.eat_vfpu_prefixes();
}
static inline void vsbz(uint32_t w) {
    AllegrexContext& ctx = cx();
    const uint32_t length = length_of(w);
    float source[4]{};
    ctx.read_vfpu_vector_with_source_prefix(source, vs_of(w), length, 0u);
    const uint32_t s = bits(source[0]);
    const bool passes = std::isnan(source[0]) || (s & 0x7F800000u) == 0u;
    write_first_lane_result(w, source, passes ? source[0] : from_bits((127u << 23u) | (s & 0x007FFFFFu)));
}
static inline void vlgb(uint32_t w) {
    AllegrexContext& ctx = cx();
    const uint32_t length = length_of(w);
    float source[4]{};
    ctx.read_vfpu_vector_with_source_prefix(source, vs_of(w), length, 0u);
    const uint32_t exponent = (bits(source[0]) >> 23u) & 0xFFu;
    const float result = exponent == 0xFFu ? source[0]
                       : exponent == 0u   ? -INFINITY
                                          : static_cast<float>(static_cast<int>(exponent) - 127);
    write_first_lane_result(w, source, result);
}
static inline void vi2c_vi2us(uint32_t w, bool is_vi2c) {
    AllegrexContext& ctx = cx();
    const uint32_t length = length_of(w);
    float source[4]{};
    ctx.read_vfpu_vector_with_source_prefix(source, vs_of(w), length, 0u);
    float result[4]{};
    uint32_t result_length = 1u;
    if (is_vi2c) {
        uint32_t packed = 0u;
        for (uint32_t lane = 0u; lane < length; ++lane) packed |= (bits(source[lane]) >> 24u) << (lane * 8u);
        result[0] = from_bits(packed);
    } else {
        result_length = (length + 1u) / 2u;
        for (uint32_t lane = 0u; lane < result_length; ++lane) {
            const auto low = std::max(0, std::bit_cast<int32_t>(source[lane * 2u]));
            const auto high = lane * 2u + 1u < length ? std::max(0, std::bit_cast<int32_t>(source[lane * 2u + 1u])) : 0;
            result[lane] = from_bits((static_cast<uint32_t>(low) >> 15u) | ((static_cast<uint32_t>(high) >> 15u) << 16u));
        }
    }
    ctx.write_vfpu_vector_with_destination_prefix(result, vd_of(w), result_length);
}
// kind: 0 vsrt1, 1 vsrt2, 2 vsrt3, 3 vsrt4, 4 vbfy2
static inline void vsort(uint32_t w, int kind) {
    AllegrexContext& ctx = cx();
    const uint32_t length = length_of(w);
    float s[4]{};
    ctx.read_vfpu_vector_with_source_prefix(s, vs_of(w), length, 0u);
    float d[4]{};
    switch (kind) {
    case 0: d[0] = lane_min(s[0], s[1]); d[1] = lane_max(s[0], s[1]); d[2] = lane_min(s[2], s[3]); d[3] = lane_max(s[2], s[3]); break;
    case 1: d[0] = lane_min(s[0], s[3]); d[1] = lane_min(s[1], s[2]); d[2] = lane_max(s[1], s[2]); d[3] = lane_max(s[0], s[3]); break;
    case 2: d[0] = lane_max(s[0], s[1]); d[1] = lane_min(s[0], s[1]); d[2] = lane_max(s[2], s[3]); d[3] = lane_min(s[2], s[3]); break;
    case 3: d[0] = lane_max(s[0], s[3]); d[1] = lane_max(s[1], s[2]); d[2] = lane_min(s[1], s[2]); d[3] = lane_min(s[0], s[3]); break;
    default: d[0] = s[0] + s[2]; d[1] = s[1] + s[3]; d[2] = s[0] - s[2]; d[3] = s[1] - s[3]; break;
    }
    ctx.write_vfpu_vector_with_destination_prefix(d, vd_of(w), length);
}
static inline void vmfvc(uint32_t w) {
    AllegrexContext& ctx = cx();
    const uint32_t control = (w >> 8u) & 0x7Fu;
    ctx.set_vfpu_scalar_bits(vd_of(w), control < 16u ? ctx.vfpu_ctrl[control] : 0u);
}
static inline void vmtvc(uint32_t w) {
    AllegrexContext& ctx = cx();
    const uint32_t control = w & 0x7Fu;
    if (control < 16u) ctx.set_vfpu_scalar_bits(128u + control, ctx.vfpu_scalar_bits(vs_of(w)));
}
static inline void vt4444_5551(uint32_t w, bool is_4444) {
    AllegrexContext& ctx = cx();
    const uint32_t length = length_of(w);
    float source[4]{};
    ctx.read_vfpu_vector_with_source_prefix(source, vs_of(w), length, 0u);
    const uint32_t result_length = (length + 1u) / 2u;
    float result[4]{};
    for (uint32_t lane = 0u; lane < result_length; ++lane) {
        const uint32_t low = pack_colour(bits(source[lane * 2u]), is_4444);
        const uint32_t high = lane * 2u + 1u < length ? pack_colour(bits(source[lane * 2u + 1u]), is_4444) : 0u;
        result[lane] = from_bits(low | (high << 16u));
    }
    ctx.write_vfpu_vector_with_destination_prefix(result, vd_of(w), result_length);
}

// bvf / bvt condition (VFPU condition code bit of vfpu_ctrl[3])
static LEAN_INLINE uint32_t cc_bit(uint32_t w) { return (cx().vfpu_ctrl[3] >> ((w >> 18u) & 7u)) & 1u; }

} // namespace lean_vf
