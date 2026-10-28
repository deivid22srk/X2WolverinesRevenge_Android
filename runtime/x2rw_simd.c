/* X2: Wolverine's Revenge - port Android
 * Implementação da camada SIMD (MMX / SSE / SSE2 / 3DNow!).
 * Ver runtime/x2rw_simd.h para o contrato.
 */
#include "x2rw_simd.h"

#include <math.h>

/* ================================================================= */
/* Acesso a operandos                                                */
/* ================================================================= */

uint64_t x2rw_sop_get64(x2rw_ctx *c, x2rw_sop o) {
    return o.is_mem ? x2rw_read64(c, o.addr) : c->mm[o.reg & 7];
}
void x2rw_sop_set64(x2rw_ctx *c, int reg, uint64_t v) {
    c->mm[reg & 7] = v;
}
void x2rw_sop_get128(x2rw_ctx *c, x2rw_sop o, uint8_t out[16]) {
    if (o.is_mem) {
        for (int i = 0; i < 16; i++) out[i] = x2rw_read8(c, o.addr + (uint32_t)i);
    } else {
        memcpy(out, c->xmm[o.reg & 7], 16);
    }
}
void x2rw_sop_set128(x2rw_ctx *c, int reg, const uint8_t in[16]) {
    memcpy(c->xmm[reg & 7], in, 16);
}
uint32_t x2rw_sop_get32(x2rw_ctx *c, x2rw_sop o) {
    return o.is_mem ? x2rw_read32(c, o.addr) : (uint32_t)c->mm[o.reg & 7];
}
void x2rw_sop_set32(x2rw_ctx *c, int reg, uint32_t v) {
    c->mm[reg & 7] = (uint64_t)v;
}
float x2rw_sop_getf32(x2rw_ctx *c, x2rw_sop o) {
    uint32_t bits;
    if (o.is_mem) {
        bits = x2rw_read32(c, o.addr);
    } else {
        uint8_t tmp[16];
        memcpy(tmp, c->xmm[o.reg & 7], 16);
        memcpy(&bits, tmp, 4);
    }
    float f;
    memcpy(&f, &bits, 4);
    return f;
}
void x2rw_sop_setf32(x2rw_ctx *c, int reg, float v) {
    memcpy(&c->xmm[reg & 7][0], &v, 4);
}
double x2rw_sop_getf64(x2rw_ctx *c, x2rw_sop o) {
    uint64_t bits;
    if (o.is_mem) {
        bits = x2rw_read64(c, o.addr);
    } else {
        memcpy(&bits, c->xmm[o.reg & 7], 8);
    }
    double d;
    memcpy(&d, &bits, 8);
    return d;
}
void x2rw_sop_setf64(x2rw_ctx *c, int reg, double v) {
    memcpy(&c->xmm[reg & 7][0], &v, 8);
}

void x2rw_simd_emms(x2rw_ctx *c) {
    memset(c->mm, 0, sizeof(c->mm));
}

/* ================================================================= */
/* MMX / inteiro                                                     */
/* ================================================================= */

#define MM_DST c->mm[dst & 7]
#define MM_SRC x2rw_sop_get64(c, src)

void x2rw_simd_movq(x2rw_ctx *c, int dst, x2rw_sop src) {
    MM_DST = MM_SRC;
}
void x2rw_simd_movd_mm(x2rw_ctx *c, int dst, x2rw_sop src) {
    MM_DST = (uint64_t)x2rw_sop_get32(c, src);
}
void x2rw_simd_movd_xmm(x2rw_ctx *c, int dst, x2rw_sop src) {
    uint32_t v = x2rw_sop_get32(c, src);
    memcpy(c->xmm[dst & 7], &v, 4);
}
void x2rw_simd_pxor(x2rw_ctx *c, int dst, x2rw_sop src) { MM_DST ^= MM_SRC; }
void x2rw_simd_pand(x2rw_ctx *c, int dst, x2rw_sop src) { MM_DST &= MM_SRC; }
void x2rw_simd_pandn(x2rw_ctx *c, int dst, x2rw_sop src) { MM_DST = (~MM_DST) & MM_SRC; }
void x2rw_simd_por(x2rw_ctx *c, int dst, x2rw_sop src) { MM_DST |= MM_SRC; }

#define PADD(T, dst, src, op) do {                       \
    uint64_t a = MM_DST, b = MM_SRC, r = 0;              \
    for (int i = 0; i < 8 / (int)sizeof(T); i++) {       \
        T x, y, z;                                        \
        memcpy(&x, (uint8_t *)&a + i * sizeof(T), sizeof(T)); \
        memcpy(&y, (uint8_t *)&b + i * sizeof(T), sizeof(T)); \
        z = (T)(x op y);                                  \
        memcpy((uint8_t *)&r + i * sizeof(T), &z, sizeof(T)); \
    }                                                     \
    MM_DST = r;                                           \
} while (0)

void x2rw_simd_paddb(x2rw_ctx *c, int dst, x2rw_sop src) { PADD(uint8_t, dst, src, +); }
void x2rw_simd_paddw(x2rw_ctx *c, int dst, x2rw_sop src) { PADD(uint16_t, dst, src, +); }
void x2rw_simd_paddd(x2rw_ctx *c, int dst, x2rw_sop src) { PADD(uint32_t, dst, src, +); }
void x2rw_simd_psubb(x2rw_ctx *c, int dst, x2rw_sop src) { PADD(uint8_t, dst, src, -); }
void x2rw_simd_psubw(x2rw_ctx *c, int dst, x2rw_sop src) { PADD(uint16_t, dst, src, -); }
void x2rw_simd_psubd(x2rw_ctx *c, int dst, x2rw_sop src) { PADD(uint32_t, dst, src, -); }

void x2rw_simd_pmullw(x2rw_ctx *c, int dst, x2rw_sop src) {
    uint64_t a = MM_DST, b = MM_SRC, r = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t x, y, z;
        memcpy(&x, (uint8_t *)&a + i * 2, 2);
        memcpy(&y, (uint8_t *)&b + i * 2, 2);
        z = (uint16_t)((uint32_t)x * (uint32_t)y);
        memcpy((uint8_t *)&r + i * 2, &z, 2);
    }
    MM_DST = r;
}
void x2rw_simd_pmulhw(x2rw_ctx *c, int dst, x2rw_sop src) {
    uint64_t a = MM_DST, b = MM_SRC, r = 0;
    for (int i = 0; i < 4; i++) {
        int16_t x, y;
        uint16_t z;
        memcpy(&x, (uint8_t *)&a + i * 2, 2);
        memcpy(&y, (uint8_t *)&b + i * 2, 2);
        z = (uint16_t)(((int32_t)x * (int32_t)y) >> 16);
        memcpy((uint8_t *)&r + i * 2, &z, 2);
    }
    MM_DST = r;
}
void x2rw_simd_pmuludq(x2rw_ctx *c, int dst, x2rw_sop src) {
    uint32_t a = (uint32_t)MM_DST, b = (uint32_t)MM_SRC;
    MM_DST = (uint64_t)a * (uint64_t)b;
}
void x2rw_simd_pmaddwd(x2rw_ctx *c, int dst, x2rw_sop src) {
    int16_t a[4], b[4];
    int32_t r[2];
    memcpy(a, &(uint64_t){MM_DST}, 8);
    uint64_t s = MM_SRC;
    memcpy(b, &s, 8);
    for (int i = 0; i < 2; i++) {
        r[i] = (int32_t)a[i * 2] * (int32_t)b[i * 2]
             + (int32_t)a[i * 2 + 1] * (int32_t)b[i * 2 + 1];
    }
    uint64_t out;
    memcpy(&out, r, 8);
    MM_DST = out;
}

#define PCMP(T, dst, src, expr) do {                     \
    uint64_t a = MM_DST, b = MM_SRC, r = 0;              \
    for (int i = 0; i < 8 / (int)sizeof(T); i++) {       \
        T x, y, z;                                        \
        memcpy(&x, (uint8_t *)&a + i * sizeof(T), sizeof(T)); \
        memcpy(&y, (uint8_t *)&b + i * sizeof(T), sizeof(T)); \
        z = (expr) ? (T)~0 : (T)0;                        \
        memcpy((uint8_t *)&r + i * sizeof(T), &z, sizeof(T)); \
    }                                                     \
    MM_DST = r;                                           \
} while (0)

void x2rw_simd_pcmpeqb(x2rw_ctx *c, int dst, x2rw_sop src) { PCMP(uint8_t, d, s, x == y); }
void x2rw_simd_pcmpeqw(x2rw_ctx *c, int dst, x2rw_sop src) { PCMP(uint16_t, d, s, x == y); }
void x2rw_simd_pcmpeqd(x2rw_ctx *c, int dst, x2rw_sop src) { PCMP(uint32_t, d, s, x == y); }
void x2rw_simd_pcmpgtw(x2rw_ctx *c, int dst, x2rw_sop src) { PCMP(int16_t, d, s, x > y); }
void x2rw_simd_pcmpgtd(x2rw_ctx *c, int dst, x2rw_sop src) { PCMP(int32_t, d, s, x > y); }

void x2rw_simd_packsswb(x2rw_ctx *c, int dst, x2rw_sop src) {
    int16_t a[4], b[4];
    uint64_t s = MM_SRC;
    memcpy(a, &(uint64_t){MM_DST}, 8);
    memcpy(b, &s, 8);
    int8_t out[8];
    for (int i = 0; i < 4; i++) {
        int v = a[i];
        out[i] = (int8_t)(v > 127 ? 127 : (v < -128 ? -128 : v));
        v = b[i];
        out[i + 4] = (int8_t)(v > 127 ? 127 : (v < -128 ? -128 : v));
    }
    uint64_t r;
    memcpy(&r, out, 8);
    MM_DST = r;
}
void x2rw_simd_packssdw(x2rw_ctx *c, int dst, x2rw_sop src) {
    int32_t a[2], b[2];
    uint64_t s = MM_SRC;
    memcpy(a, &(uint64_t){MM_DST}, 8);
    memcpy(b, &s, 8);
    int16_t out[4];
    for (int i = 0; i < 2; i++) {
        int32_t v = a[i];
        out[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        v = b[i];
        out[i + 2] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
    }
    uint64_t r;
    memcpy(&r, out, 8);
    MM_DST = r;
}
void x2rw_simd_packuswb(x2rw_ctx *c, int dst, x2rw_sop src) {
    int16_t a[4], b[4];
    uint64_t s = MM_SRC;
    memcpy(a, &(uint64_t){MM_DST}, 8);
    memcpy(b, &s, 8);
    uint8_t out[8];
    for (int i = 0; i < 4; i++) {
        int v = a[i];
        out[i] = (uint8_t)(v > 255 ? 255 : (v < 0 ? 0 : v));
        v = b[i];
        out[i + 4] = (uint8_t)(v > 255 ? 255 : (v < 0 ? 0 : v));
    }
    uint64_t r;
    memcpy(&r, out, 8);
    MM_DST = r;
}

#define PUNPCK(name, T, lo) do {                                 \
    T a[8 / sizeof(T)], b[8 / sizeof(T)], o[8 / sizeof(T)];      \
    uint64_t s = MM_SRC;                                          \
    memcpy(a, &(uint64_t){MM_DST}, 8);                            \
    memcpy(b, &s, 8);                                             \
    int n = 8 / (int)sizeof(T);                                   \
    for (int i = 0; i < n; i++) {                                 \
        o[i * 2]     = lo ? a[i] : a[n / 2 + i];                  \
        o[i * 2 + 1] = lo ? b[i] : b[n / 2 + i];                  \
    }                                                             \
    uint64_t r; memcpy(&r, o, 8); MM_DST = r;                     \
} while (0)

void x2rw_simd_punpcklbw(x2rw_ctx *c, int dst, x2rw_sop src) { PUNPCK(lbw, uint8_t, 1); }
void x2rw_simd_punpcklwd(x2rw_ctx *c, int dst, x2rw_sop src) { PUNPCK(lwd, uint16_t, 1); }
void x2rw_simd_punpckldq(x2rw_ctx *c, int dst, x2rw_sop src) { PUNPCK(ldq, uint32_t, 1); }
void x2rw_simd_punpckhbw(x2rw_ctx *c, int dst, x2rw_sop src) { PUNPCK(hbw, uint8_t, 0); }
void x2rw_simd_punpckhwd(x2rw_ctx *c, int dst, x2rw_sop src) { PUNPCK(hwd, uint16_t, 0); }
void x2rw_simd_punpckhdq(x2rw_ctx *c, int dst, x2rw_sop src) { PUNPCK(hdq, uint32_t, 0); }

/* shifts: o operando pode ser imediato (em `src.addr` como valor) ou mm/xmm */
static uint32_t shift_count(x2rw_ctx *c, x2rw_sop src) {
    if (src.is_mem) return x2rw_read32(c, src.addr) & 63u;
    return (uint32_t)(c->mm[src.reg & 7]) & 63u;
}

void x2rw_simd_psllw(x2rw_ctx *c, int dst, x2rw_sop src) {
    uint32_t n = shift_count(c, src);
    uint64_t a = c->mm[dst & 7], r = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t v; memcpy(&v, (uint8_t *)&a + i * 2, 2);
        v = (uint16_t)(n > 15 ? 0 : (uint32_t)v << n);
        memcpy((uint8_t *)&r + i * 2, &v, 2);
    }
    c->mm[dst & 7] = r;
}
void x2rw_simd_pslld(x2rw_ctx *c, int dst, x2rw_sop src) {
    uint32_t n = shift_count(c, src);
    uint64_t a = c->mm[dst & 7], r = 0;
    for (int i = 0; i < 2; i++) {
        uint32_t v; memcpy(&v, (uint8_t *)&a + i * 4, 4);
        v = (n > 31) ? 0u : (v << n);
        memcpy((uint8_t *)&r + i * 4, &v, 4);
    }
    c->mm[dst & 7] = r;
}
void x2rw_simd_psllq(x2rw_ctx *c, int dst, x2rw_sop src) {
    uint32_t n = shift_count(c, src);
    c->mm[dst & 7] = (n > 63) ? 0 : (c->mm[dst & 7] << n);
}
void x2rw_simd_psrlw(x2rw_ctx *c, int dst, x2rw_sop src) {
    uint32_t n = shift_count(c, src);
    uint64_t a = c->mm[dst & 7], r = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t v; memcpy(&v, (uint8_t *)&a + i * 2, 2);
        v = (uint16_t)(n > 15 ? 0 : (uint32_t)v >> n);
        memcpy((uint8_t *)&r + i * 2, &v, 2);
    }
    c->mm[dst & 7] = r;
}
void x2rw_simd_psrld(x2rw_ctx *c, int dst, x2rw_sop src) {
    uint32_t n = shift_count(c, src);
    uint64_t a = c->mm[dst & 7], r = 0;
    for (int i = 0; i < 2; i++) {
        uint32_t v; memcpy(&v, (uint8_t *)&a + i * 4, 4);
        v = (n > 31) ? 0u : (v >> n);
        memcpy((uint8_t *)&r + i * 4, &v, 4);
    }
    c->mm[dst & 7] = r;
}
void x2rw_simd_psrlq(x2rw_ctx *c, int dst, x2rw_sop src) {
    uint32_t n = shift_count(c, src);
    c->mm[dst & 7] = (n > 63) ? 0 : (c->mm[dst & 7] >> n);
}
void x2rw_simd_psraw(x2rw_ctx *c, int dst, x2rw_sop src) {
    uint32_t n = shift_count(c, src);
    uint64_t a = c->mm[dst & 7], r = 0;
    for (int i = 0; i < 4; i++) {
        int16_t v; memcpy(&v, (uint8_t *)&a + i * 2, 2);
        int32_t sv = v;
        v = (int16_t)(n > 15 ? (sv < 0 ? -1 : 0) : (sv >> n));
        memcpy((uint8_t *)&r + i * 2, &v, 2);
    }
    c->mm[dst & 7] = r;
}
void x2rw_simd_psrad(x2rw_ctx *c, int dst, x2rw_sop src) {
    uint32_t n = shift_count(c, src);
    uint64_t a = c->mm[dst & 7], r = 0;
    for (int i = 0; i < 2; i++) {
        int32_t v; memcpy(&v, (uint8_t *)&a + i * 4, 4);
        v = (n > 31) ? (v < 0 ? -1 : 0) : (v >> n);
        memcpy((uint8_t *)&r + i * 4, &v, 4);
    }
    c->mm[dst & 7] = r;
}

/* pshuf* operam em 128 bits (xmm) */
void x2rw_simd_pshufd(x2rw_ctx *c, int dst, x2rw_sop src, uint8_t imm) {
    uint8_t in[16], out[16];
    x2rw_sop_get128(c, src, in);
    for (int i = 0; i < 4; i++) {
        int sel = (imm >> (i * 2)) & 3;
        memcpy(out + i * 4, in + sel * 4, 4);
    }
    memcpy(c->xmm[dst & 7], out, 16);
}
void x2rw_simd_pshufw(x2rw_ctx *c, int dst, x2rw_sop src, uint8_t imm) {
    uint64_t a = x2rw_sop_get64(c, src), r = 0;
    for (int i = 0; i < 4; i++) {
        int sel = (imm >> (i * 2)) & 3;
        uint16_t v; memcpy(&v, (uint8_t *)&a + sel * 2, 2);
        memcpy((uint8_t *)&r + i * 2, &v, 2);
    }
    c->mm[dst & 7] = r;
}
void x2rw_simd_pshuflw(x2rw_ctx *c, int dst, x2rw_sop src, uint8_t imm) {
    uint8_t in[16], out[16];
    x2rw_sop_get128(c, src, in);
    memcpy(out, in, 16);
    for (int i = 0; i < 4; i++) {
        int sel = (imm >> (i * 2)) & 3;
        memcpy(out + i * 2, in + sel * 2, 2);
    }
    memcpy(c->xmm[dst & 7], out, 16);
}
void x2rw_simd_pshufhw(x2rw_ctx *c, int dst, x2rw_sop src, uint8_t imm) {
    uint8_t in[16], out[16];
    x2rw_sop_get128(c, src, in);
    memcpy(out, in, 16);
    for (int i = 0; i < 4; i++) {
        int sel = (imm >> (i * 2)) & 3;
        memcpy(out + 8 + i * 2, in + 8 + sel * 2, 2);
    }
    memcpy(c->xmm[dst & 7], out, 16);
}

/* ================================================================= */
/* SSE ponto flutuante                                               */
/* ================================================================= */

#define XMM_DST c->xmm[dst & 7]

void x2rw_simd_movaps(x2rw_ctx *c, int dst, x2rw_sop src) {
    x2rw_sop_get128(c, src, XMM_DST);
}
void x2rw_simd_movups(x2rw_ctx *c, int dst, x2rw_sop src) {
    x2rw_sop_get128(c, src, XMM_DST);
}
void x2rw_simd_movss(x2rw_ctx *c, int dst, x2rw_sop src) {
    uint32_t v = (uint32_t)(src.is_mem ? x2rw_read32(c, src.addr)
                                       : *(uint32_t *)c->xmm[src.reg & 7]);
    memcpy(XMM_DST, &v, 4);
}
void x2rw_simd_movlps(x2rw_ctx *c, int dst, x2rw_sop src) {
    uint64_t v = x2rw_sop_get64(c, src);
    memcpy(XMM_DST, &v, 8);
}
void x2rw_simd_movhps(x2rw_ctx *c, int dst, x2rw_sop src) {
    uint64_t v = x2rw_sop_get64(c, src);
    memcpy(XMM_DST + 8, &v, 8);
}
void x2rw_simd_movlpd(x2rw_ctx *c, int dst, x2rw_sop src) {
    uint64_t v = x2rw_sop_get64(c, src);
    memcpy(XMM_DST, &v, 8);
}
void x2rw_simd_movhpd(x2rw_ctx *c, int dst, x2rw_sop src) {
    uint64_t v = src.is_mem ? x2rw_read64(c, src.addr)
                            : *(uint64_t *)c->xmm[src.reg & 7];
    memcpy(XMM_DST + 8, &v, 8);
}
void x2rw_simd_movhlps(x2rw_ctx *c, int dst, int src) {
    memcpy(XMM_DST, c->xmm[src & 7] + 8, 8);
}
void x2rw_simd_movlhps(x2rw_ctx *c, int dst, int src) {
    memcpy(XMM_DST + 8, c->xmm[src & 7], 8);
}

#define PS_OP(name, expr) do {                                     \
    float a[4], b[4], r[4];                                        \
    memcpy(a, c->xmm[dst & 7], 16);                                \
    x2rw_sop_get128(c, src, (uint8_t *)b);                         \
    for (int i = 0; i < 4; i++) { r[i] = (expr); }                 \
    memcpy(c->xmm[dst & 7], r, 16);                                \
} while (0)

void x2rw_simd_addps(x2rw_ctx *c, int dst, x2rw_sop src) { PS_OP(a, a[i] + b[i]); }
void x2rw_simd_subps(x2rw_ctx *c, int dst, x2rw_sop src) { PS_OP(a, a[i] - b[i]); }
void x2rw_simd_mulps(x2rw_ctx *c, int dst, x2rw_sop src) { PS_OP(a, a[i] * b[i]); }
void x2rw_simd_divps(x2rw_ctx *c, int dst, x2rw_sop src) { PS_OP(a, a[i] / b[i]); }
void x2rw_simd_minps(x2rw_ctx *c, int dst, x2rw_sop src) { PS_OP(a, a[i] < b[i] ? a[i] : b[i]); }
void x2rw_simd_maxps(x2rw_ctx *c, int dst, x2rw_sop src) { PS_OP(a, a[i] > b[i] ? a[i] : b[i]); }
void x2rw_simd_sqrtps(x2rw_ctx *c, int dst, x2rw_sop src) { PS_OP(a, sqrtf(b[i])); }
void x2rw_simd_rcpps(x2rw_ctx *c, int dst, x2rw_sop src) { PS_OP(a, 1.0f / b[i]); }
void x2rw_simd_rsqrtps(x2rw_ctx *c, int dst, x2rw_sop src) { PS_OP(a, 1.0f / sqrtf(b[i])); }

#define SS_OP(name, expr) do {                                     \
    float a, b, r;                                                 \
    memcpy(&a, c->xmm[dst & 7], 4);                                \
    b = x2rw_sop_getf32(c, src);                                   \
    (void)a; (void)b;                                              \
    r = (expr);                                                    \
    memcpy(c->xmm[dst & 7], &r, 4);                                \
} while (0)

void x2rw_simd_addss(x2rw_ctx *c, int dst, x2rw_sop src) { SS_OP(a, a + b); }
void x2rw_simd_subss(x2rw_ctx *c, int dst, x2rw_sop src) { SS_OP(a, a - b); }
void x2rw_simd_mulss(x2rw_ctx *c, int dst, x2rw_sop src) { SS_OP(a, a * b); }
void x2rw_simd_divss(x2rw_ctx *c, int dst, x2rw_sop src) { SS_OP(a, a / b); }
void x2rw_simd_minss(x2rw_ctx *c, int dst, x2rw_sop src) { SS_OP(a, a < b ? a : b); }
void x2rw_simd_maxss(x2rw_ctx *c, int dst, x2rw_sop src) { SS_OP(a, a > b ? a : b); }
void x2rw_simd_sqrtss(x2rw_ctx *c, int dst, x2rw_sop src) { SS_OP(a, sqrtf(b)); }

#define SD_OP(name, expr) do {                                     \
    double a, b, r;                                                \
    memcpy(&a, c->xmm[dst & 7], 8);                                \
    b = x2rw_sop_getf64(c, src);                                   \
    (void)a; (void)b;                                              \
    r = (expr);                                                    \
    memcpy(c->xmm[dst & 7], &r, 8);                                \
} while (0)

void x2rw_simd_addsd(x2rw_ctx *c, int dst, x2rw_sop src) { SD_OP(a, a + b); }
void x2rw_simd_subsd(x2rw_ctx *c, int dst, x2rw_sop src) { SD_OP(a, a - b); }
void x2rw_simd_mulsd(x2rw_ctx *c, int dst, x2rw_sop src) { SD_OP(a, a * b); }
void x2rw_simd_divsd(x2rw_ctx *c, int dst, x2rw_sop src) { SD_OP(a, a / b); }

#define BIT_OP(name, op) do {                                      \
    uint8_t b[16];                                                 \
    x2rw_sop_get128(c, src, b);                                    \
    for (int i = 0; i < 16; i++) c->xmm[dst & 7][i] =              \
        (uint8_t)(c->xmm[dst & 7][i] op b[i]);                     \
} while (0)

void x2rw_simd_andps(x2rw_ctx *c, int dst, x2rw_sop src) { BIT_OP(and, &); }
void x2rw_simd_orps(x2rw_ctx *c, int dst, x2rw_sop src) { BIT_OP(or, |); }
void x2rw_simd_xorps(x2rw_ctx *c, int dst, x2rw_sop src) { BIT_OP(xor, ^); }
void x2rw_simd_andnps(x2rw_ctx *c, int dst, x2rw_sop src) {
    uint8_t b[16];
    x2rw_sop_get128(c, src, b);
    for (int i = 0; i < 16; i++)
        c->xmm[dst & 7][i] = (uint8_t)((~c->xmm[dst & 7][i]) & b[i]);
}

void x2rw_simd_shufps(x2rw_ctx *c, int dst, x2rw_sop src, uint8_t imm) {
    float a[4], b[4], r[4];
    memcpy(a, c->xmm[dst & 7], 16);
    x2rw_sop_get128(c, src, (uint8_t *)b);
    r[0] = a[imm & 3];
    r[1] = a[(imm >> 2) & 3];
    r[2] = b[(imm >> 4) & 3];
    r[3] = b[(imm >> 6) & 3];
    memcpy(c->xmm[dst & 7], r, 16);
}
void x2rw_simd_unpcklps(x2rw_ctx *c, int dst, x2rw_sop src) {
    float a[4], b[4];
    memcpy(a, c->xmm[dst & 7], 16);
    x2rw_sop_get128(c, src, (uint8_t *)b);
    float r[4] = {a[0], b[0], a[1], b[1]};
    memcpy(c->xmm[dst & 7], r, 16);
}
void x2rw_simd_unpckhps(x2rw_ctx *c, int dst, x2rw_sop src) {
    float a[4], b[4];
    memcpy(a, c->xmm[dst & 7], 16);
    x2rw_sop_get128(c, src, (uint8_t *)b);
    float r[4] = {a[2], b[2], a[3], b[3]};
    memcpy(c->xmm[dst & 7], r, 16);
}

static void set_cmp_flags(x2rw_ctx *c, int unordered, int less, int equal) {
    c->eflags &= ~(X2RW_CF | X2RW_ZF | X2RW_PF | X2RW_OF | X2RW_SF | X2RW_AF);
    if (unordered) { c->eflags |= X2RW_CF | X2RW_ZF | X2RW_PF; return; }
    if (less) c->eflags |= X2RW_CF;
    if (equal) c->eflags |= X2RW_ZF;
}

void x2rw_simd_comiss(x2rw_ctx *c, int dst, x2rw_sop src) {
    float a, b;
    memcpy(&a, c->xmm[dst & 7], 4);
    b = x2rw_sop_getf32(c, src);
    int u = (a != a) || (b != b);
    set_cmp_flags(c, u, !u && a < b, !u && a == b);
}
void x2rw_simd_ucomiss(x2rw_ctx *c, int dst, x2rw_sop src) {
    x2rw_simd_comiss(c, dst, src);
}
void x2rw_simd_comisd(x2rw_ctx *c, int dst, x2rw_sop src) {
    double a, b;
    memcpy(&a, c->xmm[dst & 7], 8);
    b = x2rw_sop_getf64(c, src);
    int u = (a != a) || (b != b);
    set_cmp_flags(c, u, !u && a < b, !u && a == b);
}
void x2rw_simd_ucomisd(x2rw_ctx *c, int dst, x2rw_sop src) {
    x2rw_simd_comisd(c, dst, src);
}

void x2rw_simd_cvtsi2ss(x2rw_ctx *c, int dst, uint32_t v) {
    float f = (float)(int32_t)v;
    memcpy(c->xmm[dst & 7], &f, 4);
}
void x2rw_simd_cvtsi2sd(x2rw_ctx *c, int dst, uint32_t v) {
    double d = (double)(int32_t)v;
    memcpy(c->xmm[dst & 7], &d, 8);
}
void x2rw_simd_cvttss2si(x2rw_ctx *c, uint32_t *dst, x2rw_sop src) {
    float f = x2rw_sop_getf32(c, src);
    *dst = (uint32_t)(int32_t)f;
}
void x2rw_simd_cvtss2si(x2rw_ctx *c, uint32_t *dst, x2rw_sop src) {
    float f = x2rw_sop_getf32(c, src);
    *dst = (uint32_t)(int32_t)lrintf(f);
}
void x2rw_simd_cvttsd2si(x2rw_ctx *c, uint32_t *dst, x2rw_sop src) {
    double d = x2rw_sop_getf64(c, src);
    *dst = (uint32_t)(int32_t)d;
}
void x2rw_simd_cvtsd2ss(x2rw_ctx *c, int dst, x2rw_sop src) {
    double d = x2rw_sop_getf64(c, src);
    float f = (float)d;
    memcpy(c->xmm[dst & 7], &f, 4);
}
void x2rw_simd_cvtss2sd(x2rw_ctx *c, int dst, x2rw_sop src) {
    float f = x2rw_sop_getf32(c, src);
    double d = (double)f;
    memcpy(c->xmm[dst & 7], &d, 8);
}
void x2rw_simd_cvtdq2ps(x2rw_ctx *c, int dst, x2rw_sop src) {
    int32_t iv[4];
    float r[4];
    x2rw_sop_get128(c, src, (uint8_t *)iv);
    for (int i = 0; i < 4; i++) r[i] = (float)iv[i];
    memcpy(c->xmm[dst & 7], r, 16);
}
void x2rw_simd_cvtps2dq(x2rw_ctx *c, int dst, x2rw_sop src) {
    float fv[4];
    int32_t r[4];
    x2rw_sop_get128(c, src, (uint8_t *)fv);
    for (int i = 0; i < 4; i++) r[i] = (int32_t)lrintf(fv[i]);
    memcpy(c->xmm[dst & 7], r, 16);
}
void x2rw_simd_cvttps2dq(x2rw_ctx *c, int dst, x2rw_sop src) {
    float fv[4];
    int32_t r[4];
    x2rw_sop_get128(c, src, (uint8_t *)fv);
    for (int i = 0; i < 4; i++) r[i] = (int32_t)fv[i];
    memcpy(c->xmm[dst & 7], r, 16);
}

/* ================================================================= */
/* 3DNow! — opera sobre 2 floats empacotados em mm                  */
/* ================================================================= */

#define PF2(a, b) do {                       \
    float x[2], y[2];                        \
    uint64_t _a = c->mm[dst & 7];            \
    uint64_t _b = x2rw_sop_get64(c, src);    \
    memcpy(x, &_a, 8); memcpy(y, &_b, 8);    \
    (void)x; (void)y;                        \
    a;                                       \
    uint64_t _r; memcpy(&_r, x, 8);          \
    c->mm[dst & 7] = _r;                     \
} while (0)

void x2rw_3d_pfadd(x2rw_ctx *c, int dst, x2rw_sop src) { PF2(x[0]+=y[0]; x[1]+=y[1], 0); }
void x2rw_3d_pfsub(x2rw_ctx *c, int dst, x2rw_sop src) { PF2(x[0]-=y[0]; x[1]-=y[1], 0); }
void x2rw_3d_pfsubr(x2rw_ctx *c, int dst, x2rw_sop src) { PF2(x[0]=y[0]-x[0]; x[1]=y[1]-x[1], 0); }
void x2rw_3d_pfmul(x2rw_ctx *c, int dst, x2rw_sop src) { PF2(x[0]*=y[0]; x[1]*=y[1], 0); }
void x2rw_3d_pfacc(x2rw_ctx *c, int dst, x2rw_sop src) {
    float x[2], y[2];
    uint64_t a = c->mm[dst & 7], b = x2rw_sop_get64(c, src);
    memcpy(x, &a, 8); memcpy(y, &b, 8);
    float s = x[0] + x[1];
    float r[2] = {s, s};
    uint64_t rr; memcpy(&rr, r, 8);
    c->mm[dst & 7] = rr;
}
void x2rw_3d_pfnacc(x2rw_ctx *c, int dst, x2rw_sop src) {
    float x[2], y[2];
    uint64_t a = c->mm[dst & 7], b = x2rw_sop_get64(c, src);
    memcpy(x, &a, 8); memcpy(y, &b, 8);
    float s = x[0] - x[1];
    float r[2] = {s, s};
    uint64_t rr; memcpy(&rr, r, 8);
    c->mm[dst & 7] = rr;
}
void x2rw_3d_pfpnacc(x2rw_ctx *c, int dst, x2rw_sop src) {
    float x[2], y[2];
    uint64_t a = c->mm[dst & 7], b = x2rw_sop_get64(c, src);
    memcpy(x, &a, 8); memcpy(y, &b, 8);
    float s = x[1] - x[0];
    float r[2] = {s, s};
    uint64_t rr; memcpy(&rr, r, 8);
    c->mm[dst & 7] = rr;
}
/* pfrcp: aproximação de 1/x. O hardware AMD devolve ~14 bits; aqui devolvemos
 * o valor exato em float — DIVERGÊNCIA conhecida, medida pelo difftest. */
void x2rw_3d_pfrcp(x2rw_ctx *c, int dst, x2rw_sop src) { PF2(x[0]=1.0f/y[0]; x[1]=1.0f/y[1], 0); }
void x2rw_3d_pfrsqrt(x2rw_ctx *c, int dst, x2rw_sop src) {
    PF2(x[0]=1.0f/sqrtf(y[0]); x[1]=1.0f/sqrtf(y[1]), 0);
}
/* pfrcpit1/2 e pfrsqit1 são passos de Newton-Raphson; aproximamos pela
 * operação combinada que o par realiza (divisão exata). */
void x2rw_3d_pfrcpit1(x2rw_ctx *c, int dst, x2rw_sop src) {
    PF2(x[0]=x[0]*(2.0f-x[0]*y[0]); x[1]=x[1]*(2.0f-x[1]*y[1]), 0);
}
void x2rw_3d_pfrcpit2(x2rw_ctx *c, int dst, x2rw_sop src) {
    PF2(x[0]=x[0]*(2.0f-x[0]*y[0]); x[1]=x[1]*(2.0f-x[1]*y[1]), 0);
}
void x2rw_3d_pfrsqit1(x2rw_ctx *c, int dst, x2rw_sop src) {
    PF2(x[0]=x[0]*(1.5f-0.5f*y[0]*x[0]*x[0]);
        x[1]=x[1]*(1.5f-0.5f*y[1]*x[1]*x[1]), 0);
}
void x2rw_3d_pf2id(x2rw_ctx *c, int dst, x2rw_sop src) {
    float y[2];
    uint64_t b = x2rw_sop_get64(c, src);
    memcpy(y, &b, 8);
    int32_t r[2] = {(int32_t)y[0], (int32_t)y[1]};
    uint64_t rr; memcpy(&rr, r, 8);
    c->mm[dst & 7] = rr;
}
void x2rw_3d_pf2iw(x2rw_ctx *c, int dst, x2rw_sop src) {
    float y[2];
    uint64_t b = x2rw_sop_get64(c, src);
    memcpy(y, &b, 8);
    int32_t r[2] = {(int32_t)y[0], (int32_t)y[1]};
    r[0] = (int32_t)((uint32_t)r[0] << 16) >> 16;
    r[1] = (int32_t)((uint32_t)r[1] << 16) >> 16;
    uint64_t rr; memcpy(&rr, r, 8);
    c->mm[dst & 7] = rr;
}
void x2rw_3d_pi2fd(x2rw_ctx *c, int dst, x2rw_sop src) {
    int32_t y[2];
    uint64_t b = x2rw_sop_get64(c, src);
    memcpy(y, &b, 8);
    float r[2] = {(float)y[0], (float)y[1]};
    uint64_t rr; memcpy(&rr, r, 8);
    c->mm[dst & 7] = rr;
}
void x2rw_3d_pi2fw(x2rw_ctx *c, int dst, x2rw_sop src) {
    int32_t y[2];
    uint64_t b = x2rw_sop_get64(c, src);
    memcpy(y, &b, 8);
    y[0] = (int16_t)y[0];
    y[1] = (int16_t)y[1];
    float r[2] = {(float)y[0], (float)y[1]};
    uint64_t rr; memcpy(&rr, r, 8);
    c->mm[dst & 7] = rr;
}
void x2rw_3d_pavgusb(x2rw_ctx *c, int dst, x2rw_sop src) {
    uint8_t a[8], b[8], r[8];
    uint64_t x = c->mm[dst & 7], y = x2rw_sop_get64(c, src);
    memcpy(a, &x, 8); memcpy(b, &y, 8);
    for (int i = 0; i < 8; i++) r[i] = (uint8_t)((a[i] + b[i] + 1) >> 1);
    uint64_t rr; memcpy(&rr, r, 8);
    c->mm[dst & 7] = rr;
}
void x2rw_3d_pmulhrw(x2rw_ctx *c, int dst, x2rw_sop src) {
    int16_t a[4], b[4], r[4];
    uint64_t x = c->mm[dst & 7], y = x2rw_sop_get64(c, src);
    memcpy(a, &x, 8); memcpy(b, &y, 8);
    for (int i = 0; i < 4; i++)
        r[i] = (int16_t)(((int32_t)a[i] * (int32_t)b[i] + 0x8000) >> 16);
    uint64_t rr; memcpy(&rr, r, 8);
    c->mm[dst & 7] = rr;
}

/* ================================================================= */
/* 3DNow! complementos                                               */
/* ================================================================= */

void x2rw_3d_pswapd(x2rw_ctx *c, int dst, x2rw_sop src) {
    uint64_t v = x2rw_sop_get64(c, src);
    c->mm[dst & 7] = (v << 32) | (v >> 32);
}
static void pf_cmp(x2rw_ctx *c, int dst, x2rw_sop src, int mode) {
    float x[2], y[2];
    uint64_t a = c->mm[dst & 7], b = x2rw_sop_get64(c, src);
    memcpy(x, &a, 8); memcpy(y, &b, 8);
    uint32_t r[2];
    for (int i = 0; i < 2; i++) {
        int t = (mode == 0) ? (x[i] >= y[i]) : (mode == 1) ? (x[i] > y[i])
                                                          : (x[i] == y[i]);
        r[i] = t ? 0xffffffffu : 0u;
    }
    uint64_t rr; memcpy(&rr, r, 8);
    c->mm[dst & 7] = rr;
}
void x2rw_3d_pfcmpge(x2rw_ctx *c, int dst, x2rw_sop src) { pf_cmp(c, dst, src, 0); }
void x2rw_3d_pfcmpgt(x2rw_ctx *c, int dst, x2rw_sop src) { pf_cmp(c, dst, src, 1); }
void x2rw_3d_pfcmpeq(x2rw_ctx *c, int dst, x2rw_sop src) { pf_cmp(c, dst, src, 2); }

/* ================================================================= */
/* SSE complementos                                                  */
/* ================================================================= */

void x2rw_simd_movapd(x2rw_ctx *c, int dst, x2rw_sop src) { x2rw_sop_get128(c, src, c->xmm[dst & 7]); }
void x2rw_simd_movupd(x2rw_ctx *c, int dst, x2rw_sop src) { x2rw_sop_get128(c, src, c->xmm[dst & 7]); }
void x2rw_simd_rsqrtss(x2rw_ctx *c, int dst, x2rw_sop src) {
    float b = x2rw_sop_getf32(c, src), r = 1.0f / sqrtf(b);
    memcpy(c->xmm[dst & 7], &r, 4);
}
void x2rw_simd_rcpss(x2rw_ctx *c, int dst, x2rw_sop src) {
    float b = x2rw_sop_getf32(c, src), r = 1.0f / b;
    memcpy(c->xmm[dst & 7], &r, 4);
}
void x2rw_simd_movmskps(x2rw_ctx *c, uint32_t *dst, int src) {
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) {
        float f; memcpy(&f, c->xmm[src & 7] + i * 4, 4);
        uint32_t b; memcpy(&b, &f, 4);
        if (b & 0x80000000u) r |= (1u << i);
    }
    *dst = r;
}
void x2rw_simd_pmovmskb(x2rw_ctx *c, uint32_t *dst, int src) {
    uint32_t r = 0;
    for (int i = 0; i < 8; i++)
        if (c->mm[src & 7] & (1ull << (i * 8 + 7))) r |= (1u << i);
    *dst = r;
}
void x2rw_simd_pextrw(x2rw_ctx *c, uint32_t *dst, int src, uint8_t imm) {
    *dst = (uint32_t)((c->mm[src & 7] >> ((imm & 3) * 16)) & 0xffffu);
}
void x2rw_simd_pinsrw(x2rw_ctx *c, int dst, uint32_t val, uint8_t imm) {
    int sh = (imm & 3) * 16;
    c->mm[dst & 7] = (c->mm[dst & 7] & ~(0xffffull << sh))
                   | ((uint64_t)(val & 0xffffu) << sh);
}
void x2rw_simd_psubsw(x2rw_ctx *c, int dst, x2rw_sop src) {
    int16_t a[4], b[4], r[4];
    uint64_t x = c->mm[dst & 7], y = x2rw_sop_get64(c, src);
    memcpy(a, &x, 8); memcpy(b, &y, 8);
    for (int i = 0; i < 4; i++) {
        int32_t v = (int32_t)a[i] - (int32_t)b[i];
        r[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
    }
    uint64_t rr; memcpy(&rr, r, 8);
    c->mm[dst & 7] = rr;
}
void x2rw_simd_psubusb(x2rw_ctx *c, int dst, x2rw_sop src) {
    uint8_t a[8], b[8], r[8];
    uint64_t x = c->mm[dst & 7], y = x2rw_sop_get64(c, src);
    memcpy(a, &x, 8); memcpy(b, &y, 8);
    for (int i = 0; i < 8; i++) r[i] = (uint8_t)(a[i] > b[i] ? a[i] - b[i] : 0);
    uint64_t rr; memcpy(&rr, r, 8);
    c->mm[dst & 7] = rr;
}
void x2rw_simd_cvtps2pd(x2rw_ctx *c, int dst, x2rw_sop src) {
    float f[2]; double d[2];
    x2rw_sop_get128(c, src, (uint8_t *)f);
    d[0] = (double)f[0]; d[1] = (double)f[1];
    memcpy(c->xmm[dst & 7], d, 16);
}
void x2rw_simd_cvtpd2ps(x2rw_ctx *c, int dst, x2rw_sop src) {
    double d[2]; float f[2];
    x2rw_sop_get128(c, src, (uint8_t *)d);
    f[0] = (float)d[0]; f[1] = (float)d[1];
    memcpy(c->xmm[dst & 7], f, 8);
}
void x2rw_simd_unpcklpd(x2rw_ctx *c, int dst, x2rw_sop src) {
    double a[2], b[2];
    memcpy(a, c->xmm[dst & 7], 16);
    x2rw_sop_get128(c, src, (uint8_t *)b);
    double r[2] = {a[0], b[0]};
    memcpy(c->xmm[dst & 7], r, 16);
}
void x2rw_simd_unpckhpd(x2rw_ctx *c, int dst, x2rw_sop src) {
    double a[2], b[2];
    memcpy(a, c->xmm[dst & 7], 16);
    x2rw_sop_get128(c, src, (uint8_t *)b);
    double r[2] = {a[1], b[1]};
    memcpy(c->xmm[dst & 7], r, 16);
}
/* cmpps/cmpss/cmppd/cmpsd: imm é o predicado (0=eq,1=lt,2=le,3=unord,
 * 4=neq,5=nlt,6=nle,7=ord). Devolve máscara de todos-os-bits. */
static int cmp_pred(int p, double a, double b) {
    int u = (a != a) || (b != b);
    switch (p & 7) {
        case 0: return !u && a == b;
        case 1: return !u && a < b;
        case 2: return !u && a <= b;
        case 3: return u;
        case 4: return u || a != b;
        case 5: return u || !(a < b);
        case 6: return u || !(a <= b);
        default: return !u;
    }
}
void x2rw_simd_cmpps(x2rw_ctx *c, int dst, x2rw_sop src, uint8_t imm) {
    float a[4], b[4];
    memcpy(a, c->xmm[dst & 7], 16);
    x2rw_sop_get128(c, src, (uint8_t *)b);
    uint32_t r[4];
    for (int i = 0; i < 4; i++)
        r[i] = cmp_pred(imm, a[i], b[i]) ? 0xffffffffu : 0u;
    memcpy(c->xmm[dst & 7], r, 16);
}
void x2rw_simd_cmpss(x2rw_ctx *c, int dst, x2rw_sop src, uint8_t imm) {
    float a, b;
    memcpy(&a, c->xmm[dst & 7], 4);
    b = x2rw_sop_getf32(c, src);
    uint32_t r = cmp_pred(imm, a, b) ? 0xffffffffu : 0u;
    memcpy(c->xmm[dst & 7], &r, 4);
}
void x2rw_simd_cmppd(x2rw_ctx *c, int dst, x2rw_sop src, uint8_t imm) {
    double a[2], b[2];
    memcpy(a, c->xmm[dst & 7], 16);
    x2rw_sop_get128(c, src, (uint8_t *)b);
    uint64_t r[2];
    for (int i = 0; i < 2; i++)
        r[i] = cmp_pred(imm, a[i], b[i]) ? ~0ull : 0ull;
    memcpy(c->xmm[dst & 7], r, 16);
}
void x2rw_simd_cmpsd(x2rw_ctx *c, int dst, x2rw_sop src, uint8_t imm) {
    double a, b;
    memcpy(&a, c->xmm[dst & 7], 8);
    b = x2rw_sop_getf64(c, src);
    uint64_t r = cmp_pred(imm, a, b) ? ~0ull : 0ull;
    memcpy(c->xmm[dst & 7], &r, 8);
}
