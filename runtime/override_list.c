/* X2: Wolverine's Revenge - port Android
 * Lista de SUBSTITUIÇÕES POR ENDEREÇO FIXO (host overrides).
 *
 * Por que isto existe: algumas funções do jogo não valem a pena ser
 * executadas como código x86 recompilado — ou porque dependem de hardware
 * que não existe no Android (Direct3D, DirectSound), ou porque o port
 * precisa de uma versão nativa para funcionar (loop principal, leitura de
 * arquivos). Para essas, o port fornece uma implementação C própria,
 * ligada a um endereço fixo verificado no binário.
 *
 * Como funciona: tools/apply_hooks.py lê esta lista e REMOVE da tabela
 * gerada (gen/<mod>_table.c) a entrada correspondente ao VA. O runtime
 * então resolve aquele VA para a função do host em vez da lifted.
 *
 * REGRAS
 *  1. Todo endereço aqui foi lido do binário e conferido (docs/RECON.md).
 *     Se você não conferiu, NÃO adicione.
 *  2. Cada substituição precisa de um comentário dizendo POR QUE.
 *  3. Substituir uma função sem necessidade é perda: ela deixa de ser
 *     código recompilado de verdade e passa a ser código novo do port.
 *     A lista deve permanecer CURTA.
 *  4. Se uma substituição for revertida, o comportamento muda; registre em
 *     docs/TESTED.md.
 *
 * A lista começa deliberadamente VAZIA. Nenhuma substituição foi validada
 * ainda (ver docs/TESTED.md). Preencher só depois de medir.
 */

#ifndef X2WR_OVERRIDE_LIST_C
#define X2WR_OVERRIDE_LIST_C

#include "x2rw_runtime.h"

typedef struct {
    uint32_t    va;      /* VA global (imagebase + RVA) da função do jogo */
    x2rw_fn_t   fn;      /* implementação do host que assume o lugar      */
    const char *module;  /* módulo dono do VA                            */
    const char *why;     /* motivo, obrigatório                          */
} x2rw_override_t;

/* ------------------------------------------------------------------ */
/* Nenhuma substituição validada até agora.                           */
/*                                                                    */
/* Exemplos do que PODERIA entrar aqui, quando medido e justificado:  */
/*                                                                    */
/*   { 0x0050A1B0u, x2wr_host_d3d8_present, "Wolvie",                 */
/*     "ponto de Present do D3D8: no Android quem apresenta é a       */
/*      SurfaceView, não o código do jogo" },                         */
/*                                                                    */
/* NÃO adicione nada sem (a) ter lido o endereço no binário e         */
/* (b) ter um motivo que se sustente.                                 */
/* ------------------------------------------------------------------ */

const x2rw_override_t x2rw_overrides[] = {
    { 0u, (x2rw_fn_t)0, (const char *)0, (const char *)0 }  /* sentinela */
};

const uint32_t x2rw_n_overrides = 0u;

#endif /* X2WR_OVERRIDE_LIST_C */
