/* X2: Wolverine's Revenge - port Android
 * Runtime central: espaço de endereços do guest, despacho de chamadas,
 * flags, x87, strings, CPUID/TSC e log.
 *
 * O espaço de endereços de 32 bits é paginado (64 KiB por página, tabela
 * esparsa). Cada módulo lifted é mapeado na sua imagebase verificada
 * (docs/RECON.md). Heap e pilha do guest vivem no mesmo espaço, porque o
 * jogo tira ponteiros de pilha e os repassa entre funções.
 */
#include "x2rw_runtime.h"
#include "x2rw_simd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Log                                                                */
/* ------------------------------------------------------------------ */

static FILE *g_log = NULL;
static int   g_log_count = 0;
static int   g_log_limit = 4000;   /* LB_LOG=<n> ajusta */
static int   g_log_inited = 0;

static void log_init(void) {
    if (g_log_inited) return;
    g_log_inited = 1;
    const char *n = getenv("LB_LOG");
    if (n && *n) g_log_limit = atoi(n);
    const char *path = getenv("LB_LOGPATH");
    g_log = path && *path ? fopen(path, "a") : stderr;
    if (!g_log) g_log = stderr;
}

void x2rw_log(const char *fmt, ...) {
    log_init();
    if (g_log_count++ > g_log_limit) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

void x2rw_fault(x2rw_ctx *c, int code, uint32_t va, const char *msg) {
    if (c->fault) return;      /* preserva a primeira falha */
    c->fault = code;
    c->fault_code = (uint32_t)code;
    c->fault_va = va;
    c->fault_msg = msg;
    x2rw_log("[falha] code=%d va=%08x msg=%s", code, va, msg ? msg : "?");
}

void x2rw_unsupported(x2rw_ctx *c, uint32_t va, const char *insn) {
    x2rw_log("[não-suportado] va=%08x insn=%s", va, insn ? insn : "?");
    if (!c->fault) {
        c->fault = X2RW_FAULT_UNSUPPORTED;
        c->fault_code = X2RW_FAULT_UNSUPPORTED;
        c->fault_va = va;
        c->fault_msg = insn;
    }
}

/* ------------------------------------------------------------------ */
/* Espaço de endereços paginado                                       */
/* ------------------------------------------------------------------ */

#define PAGE_BITS 16
/* O NDK 27 traz `PAGE_SIZE 4096` em <bits/page_size.h>. O nosso é 64 KiB
 * (o do espaço de endereços do guest), então descarta o do sistema antes —
 * sem o #undef o clang emite -Wmacro-redefined e a ordem de inclusão vira
 * sorte. Ver docs/TESTED.md §2.5. */
#undef PAGE_SIZE
#define PAGE_SIZE (1u << PAGE_BITS)              /* 64 KiB */
#define PAGE_COUNT (1u << (32 - PAGE_BITS))      /* 65536 páginas = 4 GiB */

static uint8_t **g_pages = NULL;

/* Estatísticas para o log de encerramento */
static uint64_t g_pages_live = 0;
static uint64_t g_reads = 0, g_writes = 0;

static uint8_t *page_get(uint32_t va, int create) {
    uint32_t idx = va >> PAGE_BITS;
    uint8_t *p = g_pages[idx];
    if (!p && create) {
        p = (uint8_t *)calloc(1, PAGE_SIZE);
        if (!p) return NULL;
        g_pages[idx] = p;
        g_pages_live++;
    }
    return p;
}

int x2rw_mem_init(void) {
    if (g_pages) return 0;
    g_pages = (uint8_t **)calloc(PAGE_COUNT, sizeof(uint8_t *));
    return g_pages ? 0 : -1;
}

/* Mapeia um blob (imagem de módulo) a partir de `base`. */
int x2rw_mem_map(uint32_t base, const void *data, size_t size) {
    if (x2rw_mem_init() != 0) return -1;
    const uint8_t *src = (const uint8_t *)data;
    size_t off = 0;
    while (off < size) {
        uint32_t va = base + (uint32_t)off;
        uint8_t *pg = page_get(va, 1);
        if (!pg) return -1;
        size_t in_page = PAGE_SIZE - (va & (PAGE_SIZE - 1));
        size_t n = size - off < in_page ? size - off : in_page;
        memcpy(pg + (va & (PAGE_SIZE - 1)), src + off, n);
        off += n;
    }
    return 0;
}

/* Escreve um bloco direto (sem passar por x2rw_write8 por byte). */
void x2rw_mem_write(uint32_t va, const void *data, size_t size) {
    const uint8_t *src = (const uint8_t *)data;
    size_t off = 0;
    while (off < size) {
        uint32_t a = va + (uint32_t)off;
        uint8_t *pg = page_get(a, 1);
        if (!pg) return;
        size_t in_page = PAGE_SIZE - (a & (PAGE_SIZE - 1));
        size_t n = size - off < in_page ? size - off : in_page;
        memcpy(pg + (a & (PAGE_SIZE - 1)), src + off, n);
        off += n;
    }
}

void x2rw_mem_read(uint32_t va, void *dst, size_t size) {
    uint8_t *d = (uint8_t *)dst;
    size_t off = 0;
    while (off < size) {
        uint32_t a = va + (uint32_t)off;
        uint8_t *pg = page_get(a, 0);
        size_t in_page = PAGE_SIZE - (a & (PAGE_SIZE - 1));
        size_t n = size - off < in_page ? size - off : in_page;
        if (!pg) {
            memset(d + off, 0, n);
        } else {
            memcpy(d + off, pg + (a & (PAGE_SIZE - 1)), n);
        }
        off += n;
    }
}

/* ------------------------------------------------------------------ */
/* Leitura / escrita do guest                                         */
/* ------------------------------------------------------------------ */

#define RD(T, suffix)                                                     \
    T x2rw_read##suffix(x2rw_ctx *c, uint32_t va) {                       \
        g_reads++;                                                        \
        if (va < PAGE_SIZE) {                                             \
            x2rw_log("[aviso] leitura de endereço baixo %08x", va);       \
        }                                                                 \
        uint8_t *pg = page_get(va, 0);                                    \
        if (!pg) {                                                        \
            /* leitura de página nunca tocada devolve zero: memória       \
             * alocada por malloc do CRT é zero-preenchida, e falhar      \
             * aqui quebraria código de inicialização legítimo. */        \
            return (T)0;                                                  \
        }                                                                 \
        T v;                                                              \
        uint32_t off = va & (PAGE_SIZE - 1);                              \
        if (off + sizeof(T) <= PAGE_SIZE) {                               \
            memcpy(&v, pg + off, sizeof(T));                              \
        } else {                                                          \
            uint8_t tmp[sizeof(T)];                                       \
            x2rw_mem_read(va, tmp, sizeof(T));                            \
            memcpy(&v, tmp, sizeof(T));                                   \
        }                                                                 \
        return v;                                                         \
    }

RD(uint8_t, 8)
RD(uint16_t, 16)
RD(uint32_t, 32)
RD(uint64_t, 64)

#define WR(T, suffix)                                                     \
    void x2rw_write##suffix(x2rw_ctx *c, uint32_t va, T v) {              \
        g_writes++;                                                       \
        uint8_t *pg = page_get(va, 1);                                    \
        if (!pg) {                                                        \
            x2rw_fault(c, X2RW_FAULT_BAD_ADDR, va, "write: sem memória"); \
            return;                                                       \
        }                                                                 \
        uint32_t off = va & (PAGE_SIZE - 1);                              \
        if (off + sizeof(T) <= PAGE_SIZE) {                               \
            memcpy(pg + off, &v, sizeof(T));                              \
        } else {                                                          \
            x2rw_mem_write(va, &v, sizeof(T));                            \
        }                                                                 \
    }

WR(uint8_t, 8)
WR(uint16_t, 16)
WR(uint32_t, 32)
WR(uint64_t, 64)

char *x2rw_read_cstr(x2rw_ctx *c, uint32_t va, char *buf, size_t buflen) {
    if (buflen == 0) return buf;
    size_t i = 0;
    while (i + 1 < buflen) {
        uint8_t ch = x2rw_read8(c, va + (uint32_t)i);
        buf[i++] = (char)ch;
        if (ch == 0) return buf;
    }
    buf[buflen - 1] = 0;
    return buf;
}

uint32_t x2rw_strlen(x2rw_ctx *c, uint32_t va) {
    uint32_t n = 0;
    while (n < (1u << 24) && x2rw_read8(c, va + n) != 0) n++;
    return n;
}

/* ------------------------------------------------------------------ */
/* Heap do guest (bump allocator com lista livre simples)             */
/* ------------------------------------------------------------------ */

typedef struct block_hdr {
    uint32_t size;
    uint32_t free;
    uint32_t next;
    uint32_t prev;
} block_hdr;

static uint32_t g_heap_start = 0;
static uint32_t g_heap_end = 0;

uint32_t x2rw_heap_alloc(x2rw_ctx *c, uint32_t size) {
    if (g_heap_start == 0) {
        g_heap_start = X2RW_HEAP_BASE;
        g_heap_end = X2RW_HEAP_BASE;
    }
    if (size == 0) size = 16;
    size = (size + 15u) & ~15u;              /* alinhamento 16 como o CRT */
    uint32_t p = g_heap_end;
    if (p + size + 16u > X2RW_HEAP_LIMIT) {
        x2rw_fault(c, X2RW_FAULT_BAD_ADDR, 0, "heap do guest esgotado");
        return 0;
    }
    g_heap_end = p + size + 16u;
    block_hdr h = { size, 0, 0, 0 };
    x2rw_mem_write(p, &h, sizeof(h));
    return p + 16u;
}

uint32_t x2rw_heap_free(x2rw_ctx *c, uint32_t ptr) {
    (void)c;
    if (ptr == 0) return 0;
    /* Marca como livre; o bump allocator não reutiliza (suficiente para
     * o volume de alocações do jogo, que é pequeno por frame). */
    block_hdr h;
    x2rw_mem_read(ptr - 16u, &h, sizeof(h));
    h.free = 1;
    x2rw_mem_write(ptr - 16u, &h, sizeof(h));
    return 0;
}

uint32_t x2rw_heap_used(void) {
    return g_heap_end > g_heap_start ? g_heap_end - g_heap_start : 0;
}

/* ------------------------------------------------------------------ */
/* ENTER / LEAVE                                                      */
/* ------------------------------------------------------------------ */

void x2rw_enter(x2rw_ctx *c, uint32_t frame_size, uint32_t nesting) {
    x2rw_push(c, c->r.ebp);
    uint32_t frame = c->r.esp;
    if (nesting > 0) {
        for (uint32_t i = 1; i < nesting; i++) {
            uint32_t ebp = c->r.ebp;
            x2rw_push(c, x2rw_read32(c, ebp - (int32_t)(i * 4u)));
        }
        x2rw_push(c, frame);
    }
    c->r.ebp = frame;
    c->r.esp -= frame_size;
}

/* ------------------------------------------------------------------ */
/* Operações de string                                                */
/* ------------------------------------------------------------------ */

static void str_step(x2rw_ctx *c, uint32_t elem) {
    if (c->eflags & X2RW_DF) {
        c->r.esi -= elem;
        c->r.edi -= elem;
    } else {
        c->r.esi += elem;
        c->r.edi += elem;
    }
}

void x2rw_str_movs(x2rw_ctx *c, uint32_t elem, uint32_t rep) {
    if (rep == 0) {
        if (elem == 1) x2rw_write8(c, c->r.edi, x2rw_read8(c, c->r.esi));
        else if (elem == 2) x2rw_write16(c, c->r.edi, x2rw_read16(c, c->r.esi));
        else x2rw_write32(c, c->r.edi, x2rw_read32(c, c->r.esi));
        str_step(c, elem);
        return;
    }
    while (c->r.ecx != 0) {
        if (elem == 1) x2rw_write8(c, c->r.edi, x2rw_read8(c, c->r.esi));
        else if (elem == 2) x2rw_write16(c, c->r.edi, x2rw_read16(c, c->r.esi));
        else x2rw_write32(c, c->r.edi, x2rw_read32(c, c->r.esi));
        str_step(c, elem);
        c->r.ecx--;
    }
}

void x2rw_str_stos(x2rw_ctx *c, uint32_t elem, uint32_t rep) {
    uint32_t n = rep ? c->r.ecx : 1u;
    while (n--) {
        if (elem == 1) x2rw_write8(c, c->r.edi, (uint8_t)c->r.eax);
        else if (elem == 2) x2rw_write16(c, c->r.edi, (uint16_t)c->r.eax);
        else x2rw_write32(c, c->r.edi, c->r.eax);
        if (c->eflags & X2RW_DF) c->r.edi -= elem; else c->r.edi += elem;
    }
    if (rep) c->r.ecx = 0;
}

void x2rw_str_lods(x2rw_ctx *c, uint32_t elem, uint32_t rep) {
    uint32_t n = rep ? c->r.ecx : 1u;
    while (n--) {
        if (elem == 1) c->r.eax = (c->r.eax & 0xffffff00u) | x2rw_read8(c, c->r.esi);
        else if (elem == 2) c->r.eax = (c->r.eax & 0xffff0000u) | x2rw_read16(c, c->r.esi);
        else c->r.eax = x2rw_read32(c, c->r.esi);
        if (c->eflags & X2RW_DF) c->r.esi -= elem; else c->r.esi += elem;
    }
    if (rep) c->r.ecx = 0;
}

void x2rw_str_scas(x2rw_ctx *c, uint32_t elem, uint32_t rep) {
    for (;;) {
        uint32_t a = c->r.eax & (elem == 1 ? 0xffu : elem == 2 ? 0xffffu : ~0u);
        uint32_t b = elem == 1 ? x2rw_read8(c, c->r.edi)
                   : elem == 2 ? x2rw_read16(c, c->r.edi)
                               : x2rw_read32(c, c->r.edi);
        uint32_t res = a - b;
        x2rw_flags_sub(c, a, b, res);
        if (c->eflags & X2RW_DF) c->r.edi -= elem; else c->r.edi += elem;
        if (rep == 0) return;
        c->r.ecx--;
        if (c->r.ecx == 0) return;
        if (rep == 0xF3 && !(c->eflags & X2RW_ZF)) return;   /* repe  */
        if (rep == 0xF2 && (c->eflags & X2RW_ZF)) return;    /* repne */
    }
}

void x2rw_str_cmps(x2rw_ctx *c, uint32_t elem, uint32_t rep) {
    for (;;) {
        uint32_t a = elem == 1 ? x2rw_read8(c, c->r.esi)
                   : elem == 2 ? x2rw_read16(c, c->r.esi)
                               : x2rw_read32(c, c->r.esi);
        uint32_t b = elem == 1 ? x2rw_read8(c, c->r.edi)
                   : elem == 2 ? x2rw_read16(c, c->r.edi)
                               : x2rw_read32(c, c->r.edi);
        uint32_t res = a - b;
        x2rw_flags_sub(c, a, b, res);
        str_step(c, elem);
        if (rep == 0) return;
        c->r.ecx--;
        if (c->r.ecx == 0) return;
        if (rep == 0xF3 && !(c->eflags & X2RW_ZF)) return;
        if (rep == 0xF2 && (c->eflags & X2RW_ZF)) return;
    }
}

/* ------------------------------------------------------------------ */
/* CPUID / RDTSC                                                      */
/* ------------------------------------------------------------------ */

void x2rw_cpuid(x2rw_ctx *c) {
    uint32_t leaf = c->r.eax;
    switch (leaf) {
        case 0:
            c->r.eax = 1;
            c->r.ebx = 0x756e6547u;   /* "Genu" */
            c->r.edx = 0x49656e69u;   /* "ineI" */
            c->r.ecx = 0x6c65746eu;   /* "ntel" */
            break;
        case 1:
            /* Família 6, modelo 15 (Pentium 4 / compatível 2003).
             * Sem MMX/SSE reportado? NÃO: o jogo usa MMX/SSE/3DNow!, então
             * reportamos exatamente o que o runtime realmente implementa. */
            c->r.eax = 0x00000F24u;   /* family 15, model 2, stepping 4 */
            c->r.ebx = 0x00000000u;
            c->r.edx = X2RW_CPUID_FEAT_EDX;
            c->r.ecx = X2RW_CPUID_FEAT_ECX;
            break;
        case 0x80000000u:
            c->r.eax = 0x80000008u;
            break;
        case 0x80000001u:
            /* 3DNow! é AMD: o jogo tem caminho 3DNow! e caminho SSE. */
            c->r.eax = 0x00000F24u;
            c->r.edx = X2RW_CPUID_EXT_EDX;
            c->r.ecx = 0;
            break;
        default:
            c->r.eax = c->r.ebx = c->r.ecx = c->r.edx = 0;
            break;
    }
}

void x2rw_rdtsc(x2rw_ctx *c) {
    static uint64_t base = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    if (base == 0) base = now;
    /* ~1 GHz aparente, suficiente para os temporizadores do jogo */
    uint64_t t = now - base;
    c->r.eax = (uint32_t)t;
    c->r.edx = (uint32_t)(t >> 32);
}

/* ------------------------------------------------------------------ */
/* x87                                                                */
/* ------------------------------------------------------------------ */

#define X87_ST(i) c->x87[(c->x87_top + (i)) & 7]

void x2rw_x87_init(x2rw_ctx *c) {
    memset(c->x87, 0, sizeof(c->x87));
    c->x87_top = 0;
    c->x87_status = 0;
    c->x87_cw = 0x037Fu;
}

void x2rw_x87_push(x2rw_ctx *c, uint32_t st_index) {
    c->x87_top = (c->x87_top - 1) & 7;
    X87_ST(0) = X87_ST((int)st_index + 1);
}


void x2rw_x87_fld_mem(x2rw_ctx *c, uint32_t va, uint32_t size) {
    double v;
    if (size == 4) {
        float f; uint32_t bits = x2rw_read32(c, va);
        memcpy(&f, &bits, 4); v = (double)f;
    } else if (size == 8) {
        uint64_t bits = x2rw_read64(c, va);
        memcpy(&v, &bits, 8);
    } else if (size == 10) {
        /* 80 bits: lê 64 de mantissa + 16 de expoente; converte via long
         * double quando disponível. */
        uint8_t raw[10];
        x2rw_mem_read(va, raw, 10);
        long double ld;
        memcpy(&ld, raw, 10);
        v = (double)ld;
    } else {
        v = 0.0;
    }
    c->x87_top = (c->x87_top - 1) & 7;
    X87_ST(0) = v;
}

void x2rw_x87_fst(x2rw_ctx *c, uint32_t st_index, uint32_t pop) {
    if (st_index != 0) X87_ST((int)st_index) = X87_ST(0);
    if (pop) c->x87_top = (c->x87_top + 1) & 7;
}

void x2rw_x87_fst_mem(x2rw_ctx *c, uint32_t va, uint32_t size, uint32_t pop) {
    double v = X87_ST(0);
    if (size == 4) {
        float f = (float)v; uint32_t bits; memcpy(&bits, &f, 4);
        x2rw_write32(c, va, bits);
    } else if (size == 8) {
        uint64_t bits; memcpy(&bits, &v, 8);
        x2rw_write64(c, va, bits);
    } else if (size == 10) {
        long double ld = (long double)v;
        uint8_t raw[10]; memcpy(raw, &ld, 10);
        x2rw_mem_write(va, raw, 10);
    }
    if (pop) c->x87_top = (c->x87_top + 1) & 7;
}

void x2rw_x87_fild_mem(x2rw_ctx *c, uint32_t va, uint32_t size, uint32_t pop) {
    double v;
    if (size == 2) v = (double)(int16_t)x2rw_read16(c, va);
    else if (size == 4) v = (double)(int32_t)x2rw_read32(c, va);
    else if (size == 8) v = (double)(int64_t)x2rw_read64(c, va);
    else v = 0.0;
    c->x87_top = (c->x87_top - 1) & 7;
    X87_ST(0) = v;
    if (pop) c->x87_top = (c->x87_top + 1) & 7;
}

void x2rw_x87_fist_mem(x2rw_ctx *c, uint32_t va, uint32_t size, uint32_t pop) {
    double v = X87_ST(0);
    if (size == 2) x2rw_write16(c, va, (uint16_t)(int16_t)v);
    else if (size == 4) x2rw_write32(c, va, (uint32_t)(int32_t)v);
    else if (size == 8) x2rw_write64(c, va, (uint64_t)(int64_t)v);
    if (pop) c->x87_top = (c->x87_top + 1) & 7;
}

void x2rw_x87_arith(x2rw_ctx *c, const char *op, uint32_t st_index) {
    double a = X87_ST(0), b = X87_ST((int)st_index);
    double r;
    if (strcmp(op, "fadd") == 0 || strcmp(op, "faddp") == 0) r = a + b;
    else if (strcmp(op, "fsub") == 0 || strcmp(op, "fsubp") == 0) r = a - b;
    else if (strcmp(op, "fsubr") == 0 || strcmp(op, "fsubrp") == 0) r = b - a;
    else if (strcmp(op, "fmul") == 0 || strcmp(op, "fmulp") == 0) r = a * b;
    else if (strcmp(op, "fdiv") == 0 || strcmp(op, "fdivp") == 0) r = a / b;
    else if (strcmp(op, "fdivr") == 0 || strcmp(op, "fdivrp") == 0) r = b / a;
    else r = a;
    X87_ST(0) = r;
    if (op[2] == 'p' || op[3] == 'p') c->x87_top = (c->x87_top + 1) & 7;
}

void x2rw_x87_arith_mem(x2rw_ctx *c, uint32_t va, uint32_t size, const char *op) {
    double b;
    if (size == 4) {
        float f; uint32_t bits = x2rw_read32(c, va);
        memcpy(&f, &bits, 4); b = (double)f;
    } else if (size == 8) {
        uint64_t bits = x2rw_read64(c, va);
        memcpy(&b, &bits, 8);
    } else if (size == 2) {
        b = (double)(int16_t)x2rw_read16(c, va);
    } else if (size == 4) {
        b = (double)(int32_t)x2rw_read32(c, va);
    } else {
        b = 0.0;
    }
    double a = X87_ST(0), r;
    if (strncmp(op, "fadd", 4) == 0 || strncmp(op, "fiadd", 5) == 0) r = a + b;
    else if (strncmp(op, "fsubr", 5) == 0 || strncmp(op, "fisubr", 6) == 0) r = b - a;
    else if (strncmp(op, "fsub", 4) == 0 || strncmp(op, "fisub", 5) == 0) r = a - b;
    else if (strncmp(op, "fmul", 4) == 0 || strncmp(op, "fimul", 5) == 0) r = a * b;
    else if (strncmp(op, "fdivr", 5) == 0 || strncmp(op, "fidivr", 6) == 0) r = b / a;
    else if (strncmp(op, "fdiv", 4) == 0 || strncmp(op, "fidiv", 5) == 0) r = a / b;
    else r = a;
    X87_ST(0) = r;
    if (op[3] == 'p') c->x87_top = (c->x87_top + 1) & 7;
}

void x2rw_x87_cmp(x2rw_ctx *c, const char *op) {
    double a = X87_ST(0), b;
    if (strstr(op, "pp")) b = a;                    /* fcompp/fucompp */
    else b = X87_ST(1);
    c->x87_status &= ~0x4700u;
    if (a != a || b != b) {
        c->x87_status |= 0x4500u;                   /* não-ordenado */
    } else if (a > b) {
        c->x87_status |= 0x0000u;
    } else if (a < b) {
        c->x87_status |= 0x0100u;
    } else {
        c->x87_status |= 0x4000u;
    }
    if (strstr(op, "comp")) {
        c->x87_top = (c->x87_top + 1) & 7;
        if (strstr(op, "pp")) c->x87_top = (c->x87_top + 1) & 7;
    }
    /* fcomi/fucomi afetam EFLAGS */
    if (strstr(op, "comi")) {
        c->eflags &= ~(X2RW_ZF | X2RW_PF | X2RW_CF);
        if (c->x87_status & 0x4000u) c->eflags |= X2RW_ZF;
        if (c->x87_status & 0x0100u) c->eflags |= X2RW_CF;
        if (c->x87_status & 0x4500u) c->eflags |= X2RW_ZF | X2RW_PF | X2RW_CF;
    }
}

void x2rw_x87_const(x2rw_ctx *c, const char *name) {
    double v = 0.0;
    if (strcmp(name, "fld1") == 0) v = 1.0;
    else if (strcmp(name, "fldz") == 0) v = 0.0;
    else if (strcmp(name, "fldpi") == 0) v = 3.14159265358979323846;
    else if (strcmp(name, "fldl2e") == 0) v = 1.44269504088896340736;
    else if (strcmp(name, "fldl2t") == 0) v = 3.32192809488736234787;
    else if (strcmp(name, "fldlg2") == 0) v = 0.30102999566398119521;
    else if (strcmp(name, "fldln2") == 0) v = 0.69314718055994530942;
    c->x87_top = (c->x87_top - 1) & 7;
    X87_ST(0) = v;
}

void x2rw_x87_unary(x2rw_ctx *c, const char *name) {
    double a = X87_ST(0);
    if (strcmp(name, "fchs") == 0) a = -a;
    else if (strcmp(name, "fabs") == 0) a = a < 0 ? -a : a;
    else if (strcmp(name, "fsqrt") == 0) a = __builtin_sqrt(a);
    else if (strcmp(name, "frndint") == 0) a = (double)(int64_t)(a < 0 ? a - 0.5 : a + 0.5);
    else if (strcmp(name, "fsin") == 0) a = __builtin_sin(a);
    else if (strcmp(name, "fcos") == 0) a = __builtin_cos(a);
    else if (strcmp(name, "fsincos") == 0) {
        double s = __builtin_sin(a), co = __builtin_cos(a);
        c->x87_top = (c->x87_top - 1) & 7;
        X87_ST(0) = co;
        c->x87_top = (c->x87_top - 1) & 7;
        X87_ST(0) = s;
        return;
    }
    else if (strcmp(name, "f2xm1") == 0) a = __builtin_pow(2.0, a) - 1.0;
    else if (strcmp(name, "fyl2x") == 0) {
        double x = X87_ST(1);
        a = x * __builtin_log2(a);
        c->x87_top = (c->x87_top + 1) & 7;
    }
    else if (strcmp(name, "fyl2xp1") == 0) {
        double x = X87_ST(1);
        a = x * __builtin_log2(a + 1.0);
        c->x87_top = (c->x87_top + 1) & 7;
    }
    else if (strcmp(name, "fpatan") == 0) {
        double x = X87_ST(1);
        a = __builtin_atan2(x, a);
        c->x87_top = (c->x87_top + 1) & 7;
    }
    else if (strcmp(name, "fptan") == 0) {
        double t = __builtin_tan(a);
        c->x87_top = (c->x87_top - 1) & 7;
        X87_ST(0) = 1.0;
        a = t;
    }
    else if (strcmp(name, "fscale") == 0) {
        a = a * __builtin_pow(2.0, (double)(int)X87_ST(1));
    }
    else if (strcmp(name, "fprem") == 0 || strcmp(name, "fprem1") == 0) {
        double b = X87_ST(1);
        a = b != 0 ? __builtin_fmod(a, b) : a;
    }
    else if (strcmp(name, "fxtract") == 0) {
        int e;
        double m = __builtin_frexp(a, &e);
        X87_ST(0) = (double)e;
        c->x87_top = (c->x87_top - 1) & 7;
        a = m;
    }
    else if (strcmp(name, "fincstp") == 0) { c->x87_top = (c->x87_top + 1) & 7; return; }
    else if (strcmp(name, "fdecstp") == 0) { c->x87_top = (c->x87_top - 1) & 7; return; }
    X87_ST(0) = a;
}

void x2rw_x87_fxch(x2rw_ctx *c, uint32_t st_index) {
    double t = X87_ST(0);
    X87_ST(0) = X87_ST((int)st_index);
    X87_ST((int)st_index) = t;
}

void x2rw_x87_free(x2rw_ctx *c, uint32_t st_index) {
    X87_ST((int)st_index) = 0.0;
}

void x2rw_x87_status(x2rw_ctx *c) {
    uint32_t top = (uint32_t)c->x87_top;
    c->x87_status = (uint16_t)((c->x87_status & 0x38FFu) | ((top & 7u) << 11));
}

/* ------------------------------------------------------------------ */
/* Tabela de módulos e despacho                                       */
/* ------------------------------------------------------------------ */

extern const x2rw_fn_desc x2rw_funcs_Wolvie[];
extern const uint32_t x2rw_n_funcs_Wolvie;
extern const x2rw_fn_desc x2rw_funcs_binkw32[];
extern const uint32_t x2rw_n_funcs_binkw32;

/* `n_functions` é preenchido em x2rw_modules_register() com o valor de
 * x2rw_n_funcs_<modulo>: 6525/173 quando gen/ está presente, 0 no build
 * app-only (ci/gen_stub). Com 0 a busca binária não encontra nada — que é o
 * correto quando não há código lifted para chamar.
 *
 * NÃO dá para pôr o valor aqui no inicializador: `x2rw_n_funcs_Wolvie` é uma
 * variável `const`, e um inicializador de duração estática só aceita
 * expressão constante. Deixar `0` no inicializador e esquecer de atribuir
 * depois foi exatamente o bug que impedia o jogo de chamar qualquer função
 * (toda x2rw_call caía em "VA não resolvido"). */
x2rw_module x2rw_mod_wolvie = {
    "Wolvie", X2RW_BASE_WOLVIE, X2RW_SIZE_WOLVIE,
    x2rw_funcs_Wolvie, 0
};
x2rw_module x2rw_mod_bink = {
    "binkw32", X2RW_BASE_BINK, X2RW_SIZE_BINK,
    x2rw_funcs_binkw32, 0
};

static const x2rw_module *g_modules[4];
static int g_n_modules = 0;
static uint32_t g_fixup_count = 0;

void x2rw_modules_register(const x2rw_module **mods, int n) {
    /* Contagens reais das tabelas geradas (gen/ ou ci/gen_stub/). */
    x2rw_mod_wolvie.n_functions = x2rw_n_funcs_Wolvie;
    x2rw_mod_bink.n_functions   = x2rw_n_funcs_binkw32;
    g_n_modules = 0;
    for (int i = 0; i < n && i < 4; i++) g_modules[g_n_modules++] = mods[i];
}

x2rw_fn_t x2rw_resolve(uint32_t va) {
    for (int i = 0; i < g_n_modules; i++) {
        const x2rw_module *m = g_modules[i];
        if (va < m->imagebase || va >= m->imagebase + m->size_of_image) continue;
        /* busca binária na tabela ordenada por VA */
        uint32_t lo = 0, hi = m->n_functions;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (m->functions[mid].va == va) return m->functions[mid].fn;
            if (m->functions[mid].va < va) lo = mid + 1; else hi = mid;
        }
    }
    return NULL;
}

/* Fallback FRACO do despacho de imports.
 *
 * POR QUE EXISTE: a camada de shims (os arquivos runtime/shims/x2rw_*.c) define
 * `x2rw_import_dispatch` com ligação forte. Quando ela ainda não existe —
 * ou quando alguém compila o runtime isolado — este símbolo fraco evita
 * erro de link. O linker prefere a definição forte quando ela está
 * presente, então não há conflito.
 *
 * SEMÂNTICA: devolve 0 = "não tratei". O chamador registra
 * X2RW_FAULT_UNRESOLVED. É o comportamento honesto: falhar no primeiro
 * import é melhor que devolver um handle inventado e deixar o jogo seguir
 * com estado inválido, longe da causa. */
__attribute__((weak)) int x2rw_import_dispatch(x2rw_ctx *c, uint32_t va) {
    (void)c;
    (void)va;
    return 0;
}

/* Rastro circular das últimas chamadas. Quando o jogo para num VA que não
 * resolve, a pergunta útil não é "qual VA" (o log já diz) e sim "por onde ele
 * chegou aqui". Ligado com LB_TRACE=1; desligado não custa nada além de um
 * store. Ver docs/TESTED.md §2.7. */
#define X2RW_TRACE_N 64
static uint32_t g_trace_va[X2RW_TRACE_N];
static int g_trace_pos = 0;
static int g_trace_on = -1;

int x2rw_trace_on(void) {
    if (g_trace_on < 0) {
        const char *e = getenv("LB_TRACE");
        g_trace_on = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return g_trace_on;
}

void x2rw_trace_dump(void) {
    if (!x2rw_trace_on()) return;
    x2rw_log("[trace] ultimas %d chamadas (mais antiga primeiro):",
             X2RW_TRACE_N);
    for (int k = 0; k < X2RW_TRACE_N; k++) {
        uint32_t v = g_trace_va[(g_trace_pos + k) % X2RW_TRACE_N];
        if (v) x2rw_log("[trace]   x2rw_call(%08x)", v);
    }
}

void x2rw_call(x2rw_ctx *c, uint32_t va) {
    if (c->fault) return;
    if (x2rw_trace_on()) {
        g_trace_va[g_trace_pos] = va;
        g_trace_pos = (g_trace_pos + 1) % X2RW_TRACE_N;
    }
    x2rw_fn_t fn = x2rw_resolve(va);
    if (!fn) {
        /* Pode ser um thunk/import: tenta a camada de imports.
         *
         * A SENTINELA PRECISA ESTAR NA PILHA ANTES DAQUI. O dispatcher lê os
         * argumentos em esp+4+4*i e desconta 4*(nargs+1): ele pressupõe o
         * endereço de retorno. Empilhar depois fazia o import receber os
         * argumentos deslocados — medido:
         *   HeapCreate(00001000, 00000000, fffffff0)  [correto: 1, 1000, 0]
         *   InitializeCriticalSection(fffffff0)       [recebeu a sentinela]
         */
        x2rw_push(c, X2RW_RET_SENTINEL);
        if (x2rw_import_dispatch(c, va)) return;
        c->r.esp += 4u;                 /* desfaz: vamos falhar mesmo */
        x2rw_fault(c, X2RW_FAULT_UNRESOLVED, va, "call para VA não resolvido");
        x2rw_trace_dump();
        return;
    }
    g_fixup_count++;
    /* Empilha um endereço de retorno sintético.
     *
     * O código x86 original foi compilado assumindo que, ao entrar numa
     * função, [esp] é o endereço de retorno e os argumentos começam em
     * [esp+4]. Sem esta sentinela, TODA leitura de argumento na pilha saía
     * deslocada em 4 bytes (ver docs/TESTED.md §2.8: `_heap_init` lê
     * `[esp+8]` esperando arg0 e recebia arg1, e o port morria num `call`
     * para um epílogo).
     *
     * O valor é irrelevante: o retorno acontece no C. `ret` no código
     * liftado desconta 4+imm8 e o dispatch de imports desconta
     * 4*(nargs+1), então a pilha volta ao lugar. */
    x2rw_push(c, X2RW_RET_SENTINEL);
    fn(c);
}

/* Salto de cauda (jmp) para outra função, com a pilha INTACTA.
 *
 * Diferença crucial em relação a x2rw_call: aqui não se empilha nada. O
 * `jmp` do x86 não empilha, então a sentinela de retorno que já está na
 * pilha continua sendo a única — é ela que o `ret` da função de destino
 * vai consumir, devolvendo o controle direto para quem chamou.
 *
 * Por que não usar x2rw_call: em 0x5c3fa7 o jogo faz `jmp 0x5c40ae`, que é
 * o `pop ebx; leave; ret` (epílogo compartilhado do MSVC). Com uma sentinela
 * a mais, o `pop ebx` do epílogo consumia a sentinela e o `ret` seguinte
 * desalinhava a pilha em 4 bytes. */
void x2rw_tail(x2rw_ctx *c, uint32_t va) {
    if (c->fault) return;
    if (x2rw_trace_on()) {
        g_trace_va[g_trace_pos] = va;
        g_trace_pos = (g_trace_pos + 1) % X2RW_TRACE_N;
    }
    x2rw_fn_t fn = x2rw_resolve(va);
    if (!fn) {
        if (x2rw_import_dispatch(c, va)) return;
        x2rw_fault(c, X2RW_FAULT_UNRESOLVED, va,
                   "salto de cauda para VA não resolvido");
        x2rw_trace_dump();
        return;
    }
    g_fixup_count++;
    fn(c);
}

/* ------------------------------------------------------------------ */
/* Inicialização                                                      */
/* ------------------------------------------------------------------ */

x2rw_ctx *x2rw_ctx_create(void) {
    x2rw_ctx *c = (x2rw_ctx *)calloc(1, sizeof(x2rw_ctx));
    if (!c) return NULL;
    if (x2rw_mem_init() != 0) { free(c); return NULL; }
    c->stack_lo = X2RW_STACK_LO;
    c->stack_hi = X2RW_STACK_HI;
    c->r.esp = X2RW_STACK_HI - 64u;
    c->r.ebp = c->r.esp;
    c->eflags = X2RW_IF;
    x2rw_x87_init(c);
    return c;
}

void x2rw_ctx_destroy(x2rw_ctx *c) {
    free(c);
}

uint64_t x2rw_stat_reads(void)  { return g_reads; }
uint64_t x2rw_stat_writes(void) { return g_writes; }
uint64_t x2rw_stat_pages(void)  { return g_pages_live; }
uint64_t x2rw_stat_calls(void)  { return g_fixup_count; }
