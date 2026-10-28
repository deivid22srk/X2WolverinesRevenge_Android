/* X2: Wolverine's Revenge - port Android
 * Contexto de execução compartilhado entre os módulos recompilados.
 *
 * O contexto é o "registro de máquina" virtual: o lifter traduz x86-32 para
 * C usando este estado. Todas as chamadas entre funções do jogo e todos os
 * shims Win32 operam sobre este mesmo contexto.
 *
 * Este header NÃO contém código do jogo — apenas a estrutura de dados que
 * o port precisa para representar a máquina x86 virtual.
 */
#ifndef X2RW_RUNTIME_H
#define X2RW_RUNTIME_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Limites                                                            */
/* ------------------------------------------------------------------ */

#define X2RW_STACK_SLOTS   (1u << 20)   /* 4 MiB de pilha x86        */
#define X2RW_STACK_MAX_ARGS 64

/* Módulos carregados (endereços de carga verificados — docs/RECON.md) */
/* Endereço de retorno sintético empilhado por x2rw_call(). O valor não é
 * usado como endereço (o retorno acontece no C); ele só ocupa o lugar que o
 * `call` do x86 ocuparia, mantendo `[esp+4]` como o primeiro argumento
 * dentro do código liftado. Ver docs/TESTED.md §2.8. */
#define X2RW_RET_SENTINEL 0xFFFFFFF0u

#define X2RW_BASE_WOLVIE   0x00400000u
#define X2RW_BASE_BINK     0x30000000u

#define X2RW_SIZE_WOLVIE   0x004CAE8Eu
#define X2RW_SIZE_BINK     0x00072000u

/* ------------------------------------------------------------------ */
/* Registros x86-32                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
} x2rw_regs;

/* Flags de EFLAGS relevantes */
#define X2RW_CF  0x00000001u
#define X2RW_PF  0x00000004u
#define X2RW_AF  0x00000010u
#define X2RW_ZF  0x00000040u
#define X2RW_SF  0x00000080u
#define X2RW_OF  0x00000800u
#define X2RW_DF  0x00000400u
#define X2RW_IF  0x00000200u
#define X2RW_TF  0x00000100u

/* ------------------------------------------------------------------ */
/* Contexto global                                                    */
/* ------------------------------------------------------------------ */

typedef struct x2rw_ctx {
    x2rw_regs r;
    uint32_t eflags;

    /* Pilha x86: REGIÃO DE MEMÓRIA do guest, com endereços reais.
     * O jogo faz "lea eax,[esp+4]" e passa esse ponteiro adiante, então a
     * pilha precisa estar na mesma memória endereçável que o resto.
     * esp cresce para baixo: começa em stack_hi e desce até stack_lo. */
    uint32_t stack_lo;
    uint32_t stack_hi;

    /* Base do bloco de memória "flat" do processo (heap linear). */
    uint8_t *mem;
    size_t   mem_size;

    /* Alinhamento do heap (x86 aloca 8/16 bytes) */
    uint32_t mem_brk;

    /* ID do call-site em retorno (usado pelo tradutor para 'ret') */
    uint32_t ret_id;

    /* Alvo de um salto indireto (tabela de salto do jogo) */
    uint32_t jump_target;

    /* Último endereço de ORIGEM de chamada visto (o "callsite"), escrito
     * pelo código liftado antes de cada call/salto de cauda.
     *
     * Existe por necessidade de diagnóstico: quando o guest trava, saber
     * "qual função" morreu não basta — foi assim que o off-by-4 da pilha
     * ficou escondido. Com o callsite, a falha aponta para a instrução
     * exata. Custa um store por chamada. */
    uint32_t pc;

    /* Registradores SIMD: MMX (64 bits) e SSE (128 bits) */
    uint64_t mm[8];
    uint8_t  xmm[8][16];

    /* Estado x87 (pilha de 8 doubles) */
    double   x87[8];
    int      x87_top;      /* índice do ST(0) */
    uint16_t x87_status;
    uint16_t x87_cw;

    /* Ponteiro para a thread que está executando (para shims de sync) */
    void *tls_self;

    /* Marcador de erro: != 0 aborta o módulo com log */
    int      fault;
    uint32_t fault_code;
    uint32_t fault_va;
    const char *fault_msg;
} x2rw_ctx;

/* Códigos de falha */
#define X2RW_FAULT_NONE        0
#define X2RW_FAULT_UNSUPPORTED 1  /* opcode não liftingado            */
#define X2RW_FAULT_BAD_ADDR    2  /* acesso fora da memória do guest  */
#define X2RW_FAULT_UNRESOLVED  3  /* call para endereço não resolvido */
#define X2RW_FAULT_UNIMPL_API  4  /* API Win32 sem implementação      */
#define X2RW_FAULT_ASSERT      5  /* assert do jogo                   */

/* ------------------------------------------------------------------ */
/* Funções de memória do guest                                        */
/* ------------------------------------------------------------------ */

/* Endereços x86 acima de 0x10000 são "flat" no bloco mem.
 * Abaixo disso: stack, sem dados. */
static inline int x2rw_addr_is_heap(uint32_t va) {
    return va >= 0x00010000u;
}

uint8_t  x2rw_read8 (x2rw_ctx *c, uint32_t va);
uint16_t x2rw_read16(x2rw_ctx *c, uint32_t va);
uint32_t x2rw_read32(x2rw_ctx *c, uint32_t va);
uint64_t x2rw_read64(x2rw_ctx *c, uint32_t va);
void     x2rw_write8 (x2rw_ctx *c, uint32_t va, uint8_t v);
void     x2rw_write16(x2rw_ctx *c, uint32_t va, uint16_t v);
void     x2rw_write32(x2rw_ctx *c, uint32_t va, uint32_t v);
void     x2rw_write64(x2rw_ctx *c, uint32_t va, uint64_t v);

/* Leitura/escrita de string (x86 convention, NUL-terminated) */
char    *x2rw_read_cstr(x2rw_ctx *c, uint32_t va, char *buf, size_t buflen);
uint32_t x2rw_strlen(x2rw_ctx *c, uint32_t va);

/* ENTER/LEAVE: monta o frame (usado por código de runtime do MSVC) */
void x2rw_enter(x2rw_ctx *c, uint32_t frame_size, uint32_t nesting);

/* Mapa de endereços do guest (definidos em x2rw_runtime.c) */
#define X2RW_STACK_LO   0x00010000u
#define X2RW_STACK_HI   0x00400000u
#define X2RW_HEAP_BASE  0x20000000u
#define X2RW_HEAP_LIMIT 0x30000000u

/* Alocação no heap do guest (equivale a malloc do processo do jogo) */
uint32_t x2rw_heap_alloc(x2rw_ctx *c, uint32_t size);
uint32_t x2rw_heap_free(x2rw_ctx *c, uint32_t ptr);

/* Bytes de heap do guest em uso agora (para o log de encerramento). */
uint32_t x2rw_heap_used(void);

/* ------------------------------------------------------------------ */
/* Pilha                                                               */
/* ------------------------------------------------------------------ */

static inline void x2rw_push(x2rw_ctx *c, uint32_t v) {
    uint32_t esp = c->r.esp - 4u;
    if (esp < c->stack_lo) {
        c->fault = X2RW_FAULT_BAD_ADDR;
        c->fault_msg = "stack overflow";
        return;
    }
    c->r.esp = esp;
    x2rw_write32(c, esp, v);
}

static inline uint32_t x2rw_pop(x2rw_ctx *c) {
    uint32_t esp = c->r.esp;
    if (esp >= c->stack_hi) {
        c->fault = X2RW_FAULT_BAD_ADDR;
        c->fault_msg = "stack underflow";
        return 0;
    }
    c->r.esp = esp + 4u;
    return x2rw_read32(c, esp);
}

/* ------------------------------------------------------------------ */
/* Cálculo de flags (helpers emitidos pelo lifter)                     */
/* ------------------------------------------------------------------ */

static inline void x2rw_flags_logic(x2rw_ctx *c, uint32_t res) {
    c->eflags &= ~(X2RW_CF | X2RW_OF | X2RW_AF | X2RW_SF | X2RW_ZF | X2RW_PF);
    if (res == 0) c->eflags |= X2RW_ZF;
    if (res & 0x80000000u) c->eflags |= X2RW_SF;
    if (__builtin_parity(res & 0xffu)) c->eflags |= X2RW_PF;
}

static inline void x2rw_flags_add(x2rw_ctx *c, uint32_t a, uint32_t b, uint32_t res) {
    uint64_t full = (uint64_t)a + (uint64_t)b;
    c->eflags &= ~(X2RW_CF | X2RW_OF | X2RW_AF | X2RW_SF | X2RW_ZF | X2RW_PF);
    if (full > 0xffffffffu) c->eflags |= X2RW_CF;
    if ((a ^ res) & (b ^ res) & 0x80000000u) c->eflags |= X2RW_OF;
    if (((a ^ b ^ res) & 0x10u)) c->eflags |= X2RW_AF;
    if (res == 0) c->eflags |= X2RW_ZF;
    if (res & 0x80000000u) c->eflags |= X2RW_SF;
    if (__builtin_parity(res & 0xffu)) c->eflags |= X2RW_PF;
}

static inline void x2rw_flags_sub(x2rw_ctx *c, uint32_t a, uint32_t b, uint32_t res) {
    uint64_t full = (uint64_t)a - (uint64_t)b;
    c->eflags &= ~(X2RW_CF | X2RW_OF | X2RW_AF | X2RW_SF | X2RW_ZF | X2RW_PF);
    if (full & 0x100000000ull) c->eflags |= X2RW_CF;
    if ((a ^ b) & (a ^ res) & 0x80000000u) c->eflags |= X2RW_OF;
    if (((a ^ b ^ res) & 0x10u)) c->eflags |= X2RW_AF;
    if (res == 0) c->eflags |= X2RW_ZF;
    if (res & 0x80000000u) c->eflags |= X2RW_SF;
    if (__builtin_parity(res & 0xffu)) c->eflags |= X2RW_PF;
}

static inline void x2rw_flags_inc(x2rw_ctx *c, uint32_t a, uint32_t res) {
    uint32_t old = c->eflags;
    x2rw_flags_add(c, a, 1u, res);
    c->eflags = (c->eflags & ~X2RW_CF) | (old & X2RW_CF); /* INC preserva CF */
}

static inline void x2rw_flags_dec(x2rw_ctx *c, uint32_t a, uint32_t res) {
    uint32_t old = c->eflags;
    x2rw_flags_sub(c, a, 1u, res);
    c->eflags = (c->eflags & ~X2RW_CF) | (old & X2RW_CF); /* DEC preserva CF */
}

static inline uint32_t x2rw_cond(x2rw_ctx *c, int cc) {
    uint32_t f = c->eflags;
    switch (cc) {
        case 0:  return (f & X2RW_OF) ? 1u : 0u;                 /* jo  */
        case 1:  return (f & X2RW_OF) ? 0u : 1u;                 /* jno */
        case 2:  return (f & X2RW_CF) ? 1u : 0u;                 /* jb  */
        case 3:  return (f & X2RW_CF) ? 0u : 1u;                 /* jae */
        case 4:  return (f & X2RW_ZF) ? 1u : 0u;                 /* je  */
        case 5:  return (f & X2RW_ZF) ? 0u : 1u;                 /* jne */
        case 6:  return (f & X2RW_CF) || (f & X2RW_ZF) ? 1u : 0u;/* jbe */
        case 7:  return (f & X2RW_CF) || (f & X2RW_ZF) ? 0u : 1u;/* ja  */
        case 8:  return (f & X2RW_SF) ? 1u : 0u;                 /* js  */
        case 9:  return (f & X2RW_SF) ? 0u : 1u;                 /* jns */
        case 10: return (f & X2RW_PF) ? 1u : 0u;                 /* jp  */
        case 11: return (f & X2RW_PF) ? 0u : 1u;                 /* jnp */
        case 12: return ((f & X2RW_SF) != (f & X2RW_OF)) ? 1u : 0u; /* jl  */
        case 13: return ((f & X2RW_SF) != (f & X2RW_OF)) ? 0u : 1u; /* jge */
        case 14: return ((f & X2RW_ZF) ||
                          ((f & X2RW_SF) != (f & X2RW_OF))) ? 1u : 0u; /* jle */
        default: return ((f & X2RW_ZF) ||
                          ((f & X2RW_SF) != (f & X2RW_OF))) ? 0u : 1u; /* jg  */
    }
}

/* Converte a condição de "setcc" (cc codificado 0..15) */
static inline uint32_t x2rw_setcc(x2rw_ctx *c, int cc) {
    return x2rw_cond(c, cc);
}

/* ------------------------------------------------------------------ */
/* Diagnóstico                                                        */
/* ------------------------------------------------------------------ */

void x2rw_log(const char *fmt, ...);
void x2rw_fault(x2rw_ctx *c, int code, uint32_t va, const char *msg);
void x2rw_unsupported(x2rw_ctx *c, uint32_t va, const char *insn);

/* ------------------------------------------------------------------ */
/* Pontos de entrada / dispatch                                       */
/* ------------------------------------------------------------------ */

/* Cada módulo lifted expõe uma tabela de funções por RVA. */
typedef void (*x2rw_fn_t)(x2rw_ctx *c);

typedef struct {
    uint32_t  va;
    x2rw_fn_t fn;
} x2rw_fn_desc;

typedef struct {
    const char   *module;
    uint32_t      imagebase;
    uint32_t      size_of_image;
    const x2rw_fn_desc *functions;   /* tabela VA -> ponteiro */
    uint32_t      n_functions;
} x2rw_module;

/* Procura uma função por VA global entre os módulos carregados. */
x2rw_fn_t x2rw_resolve(uint32_t va);

/* Executa um VA (entrypoint do jogo). */
void x2rw_call(x2rw_ctx *c, uint32_t va);

/* Salto de cauda (jmp) para outra função: transfere o controle SEM empilhar
 * endereço de retorno — é o que o `jmp` do x86 faz. Ver x2rw_tail() em
 * x2rw_runtime.c para o motivo (epílogos compartilhados do MSVC). */
void x2rw_tail(x2rw_ctx *c, uint32_t va);

/* Despeja o rastro circular de chamadas (LB_TRACE=1). */
void x2rw_trace_dump(void);

/* Verdadeiro quando LB_TRACE está ligado (ver x2rw_runtime.c). Usado pelos
 * shims para não pagar o custo do log de rastreio quando ele está desligado. */
int x2rw_trace_on(void);

/* Chamadas a imports: o shim é implementado pelo módulo de runtime. */
typedef uint32_t (*x2rw_api_t)(x2rw_ctx *c, uint32_t *args, int nargs);

/* Chamada a uma API Win32 importada. */
uint32_t x2rw_call_import(x2rw_ctx *c, uint32_t va, uint32_t *args, int nargs);

/* Despacho de um VA de import. Devolve 1 se tratou, 0 se não conhece.
 *
 * O runtime traz uma definição FRACA (que devolve 0 e leva a
 * X2RW_FAULT_UNRESOLVED). A camada de shims (os arquivos runtime/shims/x2rw_*.c) traz a
 * definição FORTE, que vence no link. Assim o runtime liga sozinho e
 * também com os shims presentes, sem símbolo duplicado. */
int x2rw_import_dispatch(x2rw_ctx *c, uint32_t va);

/* Escrita de 16 bytes (movaps/movups/movdqa com destino em memória). */
static inline void x2rw_write128(x2rw_ctx *c, uint32_t addr, const uint8_t v[16]) {
    for (int _i = 0; _i < 16; _i++)
        x2rw_write8(c, addr + (uint32_t)_i, v[_i]);
}

/* ------------------------------------------------------------------ */
/* Operações de string (rep movs/stos/lods/scas/cmps)                 */
/* ------------------------------------------------------------------ */

/* rep: 0 = sem prefixo, 0xF3 = rep/repe, 0xF2 = repne */
void x2rw_str_movs(x2rw_ctx *c, uint32_t elem, uint32_t rep);
void x2rw_str_stos(x2rw_ctx *c, uint32_t elem, uint32_t rep);
void x2rw_str_lods(x2rw_ctx *c, uint32_t elem, uint32_t rep);
void x2rw_str_scas(x2rw_ctx *c, uint32_t elem, uint32_t rep);
void x2rw_str_cmps(x2rw_ctx *c, uint32_t elem, uint32_t rep);

/* ------------------------------------------------------------------ */
/* CPU / TSC                                                          */
/* ------------------------------------------------------------------ */

void x2rw_cpuid(x2rw_ctx *c);
void x2rw_rdtsc(x2rw_ctx *c);

/* ------------------------------------------------------------------ */
/* x87 (pilha de 8 doubles)                                           */
/* ------------------------------------------------------------------ */

void x2rw_x87_init(x2rw_ctx *c);
void x2rw_x87_push(x2rw_ctx *c, uint32_t st_index);
void x2rw_x87_fld_mem(x2rw_ctx *c, uint32_t va, uint32_t size);
void x2rw_x87_fst(x2rw_ctx *c, uint32_t st_index, uint32_t pop);
void x2rw_x87_fst_mem(x2rw_ctx *c, uint32_t va, uint32_t size, uint32_t pop);
void x2rw_x87_fild_mem(x2rw_ctx *c, uint32_t va, uint32_t size, uint32_t pop);
void x2rw_x87_fist_mem(x2rw_ctx *c, uint32_t va, uint32_t size, uint32_t pop);
void x2rw_x87_arith(x2rw_ctx *c, const char *op, uint32_t st_index);
void x2rw_x87_arith_mem(x2rw_ctx *c, uint32_t va, uint32_t size, const char *op);
void x2rw_x87_cmp(x2rw_ctx *c, const char *op);
void x2rw_x87_const(x2rw_ctx *c, const char *name);
void x2rw_x87_unary(x2rw_ctx *c, const char *name);
void x2rw_x87_fxch(x2rw_ctx *c, uint32_t st_index);
void x2rw_x87_free(x2rw_ctx *c, uint32_t st_index);
void x2rw_x87_status(x2rw_ctx *c);

/* ------------------------------------------------------------------ */
/* Contexto: criação e destruição                                     */
/* ------------------------------------------------------------------ */

/* Cria um contexto. A memória do guest é UM espaço global paginado
 * (64 KiB por página, 4 GiB de espaço de endereçamento), criado sob
 * demanda por x2rw_mem_init(): não há mapeamento por contexto.
 *
 * A pilha fica em X2RW_STACK_LO..X2RW_STACK_HI e as páginas dela são
 * criadas na primeira escrita; leitura de página nunca escrita devolve
 * zero (mesma semântica de um calloc).
 *
 * Devolve NULL se a memória não puder ser obtida. */
x2rw_ctx *x2rw_ctx_create(void);

/* Libera o contexto. Aceita NULL. NÃO libera as páginas do guest (elas
 * são globais e servem a um processo de jogo). */
void x2rw_ctx_destroy(x2rw_ctx *c);

/* ------------------------------------------------------------------ */
/* Memória do guest (espaço global, não por contexto)                 */
/* ------------------------------------------------------------------ */

/* Aloca a tabela de páginas. Idempotente. Devolve 0 em sucesso, -1 em
 * falha de alocação. */
int x2rw_mem_init(void);

/* Copia um blob (imagem de módulo já mapeada no formato de memória do
 * PE) para o endereço de guest `base`. Devolve 0 em sucesso. */
int x2rw_mem_map(uint32_t base, const void *data, size_t size);

/* Escrita/leitura em bloco (mais rápido que byte a byte). A leitura de
 * página ausente devolve zeros. */
void x2rw_mem_write(uint32_t va, const void *data, size_t size);
void x2rw_mem_read(uint32_t va, void *dst, size_t size);

/* ------------------------------------------------------------------ */
/* Estatísticas (medidas; não alteram a execução)                     */
/* ------------------------------------------------------------------ */

uint64_t x2rw_stat_reads(void);    /* leituras de memória do guest */
uint64_t x2rw_stat_writes(void);   /* escritas de memória do guest */
uint64_t x2rw_stat_pages(void);    /* páginas de 64 KiB vivas */
uint64_t x2rw_stat_calls(void);    /* chamadas resolvidas por VA */

/* ------------------------------------------------------------------ */
/* Módulos                                                            */
/* ------------------------------------------------------------------ */

/* NÃO são `const`: `n_functions` é preenchido em x2rw_modules_register() a
 * partir de x2rw_n_funcs_<modulo> (símbolo gerado pelo lifter/gen_stub). Um
 * inicializador de objeto com duração estática não pode ler outra variável
 * `const`, então o valor real não cabe no inicializador e tem de ser
 * atribuído em tempo de execução. Ver docs/TESTED.md §2.6 — este era um bug
 * que deixava a tabela de funções vazia e fazia TODA x2rw_call falhar. */
extern x2rw_module x2rw_mod_wolvie;
extern x2rw_module x2rw_mod_bink;

/* Registra a lista de módulos consultada por x2rw_resolve(). Pode ser
 * chamada mais de uma vez (a última vale). */
void x2rw_modules_register(const x2rw_module **mods, int n);

/* Feições de CPUID devolvidas por x2rw_cpuid(). O jogo não checa quase
 * nada disto, mas deixar os bits zerados faria o código tomado por
 * caminhos de compatibilidade que não existem. */
#define X2RW_CPUID_FEAT_EDX  0x00000001u  /* EDX: FPU presente */
#define X2RW_CPUID_FEAT_ECX  0x00000000u  /* ECX: nenhuma extensão */
#define X2RW_CPUID_EXT_EDX   0x00000000u  /* EDX estendido: nenhuma */

#ifdef __cplusplus
}
#endif

#endif /* X2RW_RUNTIME_H */
