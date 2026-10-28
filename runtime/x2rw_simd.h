/* X2: Wolverine's Revenge - port Android
 * Camada SIMD: MMX, SSE (float/double) e 3DNow!.
 *
 * O código de 2003 usa bastante MMX/3DNow! (caminho AMD) e SSE. O lifter
 * emite chamadas a estas funções; cada uma opera sobre o banco de registros
 * do contexto (mm[8], xmm[8]).
 *
 * Precisão: operações de ponto flutuante usam float/double do host, o que
 * difere ligeiramente da semântica x87/SSE original em arredondamento e em
 * instruções de aproximação (pfrcp/pfrsqrt). Isso é MEDIDO pelo difftest
 * (docs/TESTED.md), não presumido.
 */
#ifndef X2RW_SIMD_H
#define X2RW_SIMD_H

#include <stdint.h>
#include <string.h>
#include "x2rw_runtime.h"

/* Operando SIMD: registrador direto ou memória. */
typedef struct {
    uint8_t  is_mem;
    uint8_t  reg;
    uint16_t _pad;
    uint32_t addr;
} x2rw_sop;

static inline x2rw_sop x2rw_sop_reg(int r) {
    x2rw_sop o = {0, (uint8_t)r, 0, 0};
    return o;
}
static inline x2rw_sop x2rw_sop_mem(uint32_t a) {
    x2rw_sop o = {1, 0, 0, a};
    return o;
}

/* ---------------------------------------------------------------- */
/* Acesso a operandos                                                */
/* ---------------------------------------------------------------- */

uint64_t x2rw_sop_get64(x2rw_ctx *c, x2rw_sop o);
void     x2rw_sop_set64(x2rw_ctx *c, int reg, uint64_t v);
void     x2rw_sop_get128(x2rw_ctx *c, x2rw_sop o, uint8_t out[16]);
void     x2rw_sop_set128(x2rw_ctx *c, int reg, const uint8_t in[16]);
uint32_t x2rw_sop_get32(x2rw_ctx *c, x2rw_sop o);
void     x2rw_sop_set32(x2rw_ctx *c, int reg, uint32_t v);
float    x2rw_sop_getf32(x2rw_ctx *c, x2rw_sop o);
void     x2rw_sop_setf32(x2rw_ctx *c, int reg, float v);
double   x2rw_sop_getf64(x2rw_ctx *c, x2rw_sop o);
void     x2rw_sop_setf64(x2rw_ctx *c, int reg, double v);

/* ---------------------------------------------------------------- */
/* MMX / SSE inteiro                                                 */
/* ---------------------------------------------------------------- */

void x2rw_simd_emms(x2rw_ctx *c);
void x2rw_simd_movq(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_movd_mm(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_movd_xmm(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_pxor(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_pand(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_pandn(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_por(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_paddb(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_paddw(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_paddd(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_psubb(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_psubw(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_psubd(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_pmullw(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_pmulhw(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_pmuludq(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_pmaddwd(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_pcmpeqb(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_pcmpeqw(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_pcmpeqd(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_pcmpgtw(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_pcmpgtd(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_packsswb(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_packssdw(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_packuswb(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_punpcklbw(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_punpcklwd(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_punpckldq(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_punpckhbw(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_punpckhwd(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_punpckhdq(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_psllw(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_pslld(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_psllq(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_psrlw(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_psrld(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_psrlq(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_psraw(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_psrad(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_pshufd(x2rw_ctx *c, int dst, x2rw_sop src, uint8_t imm);
void x2rw_simd_pshufw(x2rw_ctx *c, int dst, x2rw_sop src, uint8_t imm);
void x2rw_simd_pshuflw(x2rw_ctx *c, int dst, x2rw_sop src, uint8_t imm);
void x2rw_simd_pshufhw(x2rw_ctx *c, int dst, x2rw_sop src, uint8_t imm);

/* ---------------------------------------------------------------- */
/* SSE / SSE2 ponto flutuante                                        */
/* ---------------------------------------------------------------- */

void x2rw_simd_movaps(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_movups(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_movss(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_movlps(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_movhps(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_movlpd(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_movhpd(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_movhlps(x2rw_ctx *c, int dst, int src);
void x2rw_simd_movlhps(x2rw_ctx *c, int dst, int src);
void x2rw_simd_addps(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_subps(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_mulps(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_divps(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_minps(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_maxps(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_sqrtps(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_rcpps(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_rsqrtps(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_addss(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_subss(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_mulss(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_divss(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_minss(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_maxss(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_sqrtss(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_addsd(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_subsd(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_mulsd(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_divsd(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_andps(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_andnps(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_orps(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_xorps(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_shufps(x2rw_ctx *c, int dst, x2rw_sop src, uint8_t imm);
void x2rw_simd_unpcklps(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_unpckhps(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_comiss(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_ucomiss(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_comisd(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_ucomisd(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_cvtsi2ss(x2rw_ctx *c, int dst, uint32_t v);
void x2rw_simd_cvtsi2sd(x2rw_ctx *c, int dst, uint32_t v);
void x2rw_simd_cvttss2si(x2rw_ctx *c, uint32_t *dst, x2rw_sop src);
void x2rw_simd_cvtss2si(x2rw_ctx *c, uint32_t *dst, x2rw_sop src);
void x2rw_simd_cvttsd2si(x2rw_ctx *c, uint32_t *dst, x2rw_sop src);
void x2rw_simd_cvtsd2ss(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_cvtss2sd(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_cvtdq2ps(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_cvtps2dq(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_cvttps2dq(x2rw_ctx *c, int dst, x2rw_sop src);

/* ---------------------------------------------------------------- */
/* 3DNow! (caminho AMD) — aproximações; divergem do hardware AMD      */
/* ---------------------------------------------------------------- */

void x2rw_3d_pfadd(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_3d_pfsub(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_3d_pfsubr(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_3d_pfmul(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_3d_pfacc(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_3d_pfrcp(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_3d_pfrsqrt(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_3d_pfrcpit1(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_3d_pfrcpit2(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_3d_pfrsqit1(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_3d_pf2id(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_3d_pf2iw(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_3d_pi2fd(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_3d_pi2fw(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_3d_pfnacc(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_3d_pfpnacc(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_3d_pavgusb(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_3d_pmulhrw(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_3d_pswapd(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_3d_pfcmpge(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_3d_pfcmpgt(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_3d_pfcmpeq(x2rw_ctx *c, int dst, x2rw_sop src);

/* SSE diversos */
void x2rw_simd_movapd(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_movupd(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_rsqrtss(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_rcpss(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_movmskps(x2rw_ctx *c, uint32_t *dst, int src);
void x2rw_simd_pmovmskb(x2rw_ctx *c, uint32_t *dst, int src);
void x2rw_simd_pextrw(x2rw_ctx *c, uint32_t *dst, int src, uint8_t imm);
void x2rw_simd_pinsrw(x2rw_ctx *c, int dst, uint32_t val, uint8_t imm);
void x2rw_simd_psubsw(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_psubusb(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_cvtps2pd(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_cvtpd2ps(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_unpcklpd(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_unpckhpd(x2rw_ctx *c, int dst, x2rw_sop src);
void x2rw_simd_cmpps(x2rw_ctx *c, int dst, x2rw_sop src, uint8_t imm);
void x2rw_simd_cmpss(x2rw_ctx *c, int dst, x2rw_sop src, uint8_t imm);
void x2rw_simd_cmppd(x2rw_ctx *c, int dst, x2rw_sop src, uint8_t imm);
void x2rw_simd_cmpsd(x2rw_ctx *c, int dst, x2rw_sop src, uint8_t imm);

#endif /* X2RW_SIMD_H */
