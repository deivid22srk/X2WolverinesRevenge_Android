/* X2: Wolverine's Revenge - port Android
 * ============================================================================
 * x2rw_imports.c — despachante de imports e registro de handlers.
 *
 * COMO O JOGO CHAMA UMA API WIN32
 * ------------------------------
 * O código lifted faz, para cada chamada a uma API importada:
 *
 *     x2rw_push(c, argN); ... x2rw_push(c, arg0);
 *     x2rw_call(c, (uint32_t)(x2rw_read32(c, <VA do slot da IAT>)));
 *
 * `x2rw_call` NÃO empilha endereço de retorno (não existe retorno real: é
 * uma máquina virtual). Portanto, quando o despacho acontece:
 *     arg[0] = x2rw_read32(c, c->r.esp)
 *     arg[1] = x2rw_read32(c, c->r.esp + 4)
 *     ...
 * A convenção é stdcall: quem limpa os argumentos é a API chamada, logo
 * depois do handler fazemos c->r.esp += 4 * nargs e deixamos o retorno em
 * c->r.eax. O helper público x2rw_arg(c, i) implementa a mesma leitura.
 *
 * O QUE PODE CHEGAR EM `va`
 * -------------------------
 *   1. O VA do slot da IAT (0x5d4084 etc.) — caso em que o chamador leu um
 *      ponteiro que já aponta para o próprio slot;
 *   2. o VALOR lido da IAT. Se a imagem foi mapeada como no arquivo PE, esse
 *      valor é a RVA de um IMAGE_IMPORT_BY_NAME (hint + nome) — então
 *      recuperamos o NOME do export e resolvemos pelo registro (base+va);
 *   3. o VA de um thunk `jmp dword ptr [IAT]` (MSVC incremental linking) —
 *      detectado pelo opcode FF 25 e resolvido pelo slot apontado;
 *   4. um VA sintético de COM (X2RW_COM_BASE) ou de export dinâmico
 *      (X2RW_EXPORT_BASE), criados por esta camada.
 * Nunca devolvemos 0 em silêncio: se nada resolver, o runtime levanta
 * X2RW_FAULT_UNRESOLVED (via x2rw_call) ou X2RW_FAULT_UNIMPL_API (via
 * shim_unimplemented).
 * ============================================================================
 */
#include "x2rw_runtime.h"
#include "x2rw_shims.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Tabelas de IAT geradas pelo lifter (gen/ no include path). Os dois headers
 * declaram o MESMO typedef anônimo `x2wr_import_t`, e C11 não considera dois
 * structs anônimos como o mesmo tipo — então renomeamos o typedef do segundo
 * header por macro para poder incluir os dois. Os campos têm o mesmo layout. */
#include "Wolvie_imports.h"
#define x2wr_import_t x2wr_import_t_bink
#include "binkw32_imports.h"
#undef x2wr_import_t

/* ------------------------------------------------------------------ */
/* Registro estático (g_shims[])                                      */
/* ------------------------------------------------------------------ */

#define X2RW_ENTRY(dll, name, fn, nargs, level) { dll, name, fn, nargs },
static const x2rw_shim_t g_shims[] = {
    X2RW_SHIMS_ALL(X2RW_ENTRY)
};
#undef X2RW_ENTRY

/* Vetor paralelo com o nível de cada handler, para as contagens do startup.
 * O token do nível vira X2RW_LEVEL_REAL/PARCIAL/STUB por concatenação. */
#define X2RW_LEVEL(dll, name, fn, na, level) (uint8_t)X2RW_LEVEL_##level,
static const uint8_t g_shim_levels[] = {
    X2RW_SHIMS_ALL(X2RW_LEVEL)
};
#undef X2RW_LEVEL

#define X2RW_N_SHIMS ((int)(sizeof(g_shims) / sizeof(g_shims[0])))

/* Cap rígido: falha de compilação se a lista mestra crescer além do teto. */
typedef char x2rw_shim_cap_ok[(X2RW_N_SHIMS <= X2RW_MAX_SHIMS) ? 1 : -1];
typedef char x2rw_shim_levels_match[
    ((int)(sizeof(g_shim_levels) / sizeof(g_shim_levels[0])) == X2RW_N_SHIMS) ? 1 : -1];

/* Handler corrente (para shim_unimplemented conseguir logar DLL+nome). */
static const x2rw_shim_t *g_current_shim = NULL;

int x2rw_import_shim_count(void) { return X2RW_N_SHIMS; }

void x2rw_host_register_shims(void) {
    int real = 0, par = 0, stub = 0, nulls = 0;
    for (int i = 0; i < X2RW_N_SHIMS; i++) {
        switch (g_shim_levels[i]) {
            case X2RW_LEVEL_REAL:    real++; break;
            case X2RW_LEVEL_PARCIAL: par++;  break;
            default:                 stub++; break;
        }
        if (!g_shims[i].fn) nulls++;
    }
    x2rw_log("[imports] %d símbolos registrados (REAL=%d PARCIAL=%d STUB=%d)"
             " [handlers nulos=%d]",
             X2RW_N_SHIMS, real, par, stub, nulls);
    if (nulls) {
        for (int i = 0; i < X2RW_N_SHIMS; i++)
            if (!g_shims[i].fn)
                x2rw_log("[imports] ERRO: handler nulo para %s!%s",
                         g_shims[i].dll, g_shims[i].name);
    }
}

/* ------------------------------------------------------------------ */
/* shim_unimplemented                                                 */
/* ------------------------------------------------------------------ */
/* Handler compartilhado para símbolos deliberadamente não implementados.
 * Loga DLL+nome+argumentos e levanta X2RW_FAULT_UNIMPL_API — NUNCA devolve 0
 * em silêncio. */
uint32_t shim_unimplemented(x2rw_ctx *c, uint32_t *args, int nargs) {
    const char *dll = g_current_shim ? g_current_shim->dll : "?";
    const char *nm  = g_current_shim ? g_current_shim->name : "?";
    char buf[256];
    int off = 0;
    for (int i = 0; i < nargs && i < 12 && off < (int)sizeof(buf) - 16; i++)
        off += snprintf(buf + off, sizeof(buf) - (size_t)off, "%08x ", args[i]);
    buf[off < (int)sizeof(buf) ? off : (int)sizeof(buf) - 1] = 0;
    x2rw_log("[não-implementado] %s!%s args=[%s]", dll, nm, buf);
    x2rw_fault(c, X2RW_FAULT_UNIMPL_API, 0, nm);
    return 0u;
}

/* ------------------------------------------------------------------ */
/* Busca no registro                                                  */
/* ------------------------------------------------------------------ */

/* Compara o nome da DLL sem diferenciar maiúsculas (a IAT usa "KERNEL32.dll",
 * o jogo pode pedir "kernel32"). O nome do export é case-sensitive. */
static const x2rw_shim_t *find_by_dll_name(const char *dll, const char *name) {
    if (!dll || !name) return NULL;
    for (int i = 0; i < X2RW_N_SHIMS; i++) {
        if (strcasecmp(g_shims[i].dll, dll) == 0 &&
            strcmp(g_shims[i].name, name) == 0)
            return &g_shims[i];
    }
    return NULL;
}

/* Procura um nome de export em QUALQUER DLL do registro (usado quando só
 * temos o IMAGE_IMPORT_BY_NAME, sem saber de qual módulo veio). Os 226 nomes
 * são únicos no registro, então não há ambiguidade. */
static const x2rw_shim_t *find_by_name_any(const char *name) {
    if (!name || !*name) return NULL;
    for (int i = 0; i < X2RW_N_SHIMS; i++)
        if (strcmp(g_shims[i].name, name) == 0) return &g_shims[i];
    return NULL;
}

/* Resolve um VA de slot de IAT consultando as duas tabelas geradas. */
static const x2rw_shim_t *resolve_slot(uint32_t va) {
    for (int i = 0; i < X2WR_N_IMPORTS_WOLVIE; i++)
        if (x2wr_imports_Wolvie[i].va == va)
            return find_by_dll_name(x2wr_imports_Wolvie[i].dll,
                                    x2wr_imports_Wolvie[i].name);
    for (int i = 0; i < X2WR_N_IMPORTS_BINKW32; i++)
        if (x2wr_imports_binkw32[i].va == va)
            return find_by_dll_name(x2wr_imports_binkw32[i].dll,
                                    x2wr_imports_binkw32[i].name);
    return NULL;
}

/* Ler um IMAGE_IMPORT_BY_NAME em `addr` (hint de 2 bytes + nome ASCII).
 * Devolve 1 e copia o nome se o conteúdo parecer um nome de export válido. */
static int import_name_at(x2rw_ctx *c, uint32_t addr, char *out, size_t outlen) {
    if (!addr) return 0;
    uint16_t hint = x2rw_read16(c, addr);
    (void)hint;
    size_t i = 0;
    for (; i + 1 < outlen; i++) {
        uint8_t ch = x2rw_read8(c, addr + 2u + (uint32_t)i);
        if (ch == 0) break;
        /* nomes de export são ASCII imprimível ('A'..'z', dígitos, '_', '@') */
        if (ch < 0x20 || ch > 0x7e) return 0;
        out[i] = (char)ch;
    }
    out[i] = 0;
    if (i < 3) return 0;                 /* "ab" não é nome plausível */
    return 1;
}

/* Cadeia completa de resolução (ver comentário do topo do arquivo). */
static const x2rw_shim_t *resolve_va_deep(x2rw_ctx *c, uint32_t va) {
    const x2rw_shim_t *sh = resolve_slot(va);
    if (sh) return sh;

    /* (2) um nível de indireção: o conteúdo de `va` pode ser outro VA/RVA. */
    uint32_t p = x2rw_read32(c, va);
    if (p && p != va) {
        sh = resolve_slot(p);
        if (sh) return sh;
    }

    /* (3) thunk MSVC `jmp dword ptr [IAT]` = FF 25 <disp32>. */
    if (x2rw_read8(c, va) == 0xFF && x2rw_read8(c, va + 1u) == 0x25) {
        uint32_t slot = x2rw_read32(c, va + 2u);
        sh = resolve_slot(slot);
        if (sh) return sh;
        const char *nm3 = NULL;
        char name3[192];
        /* o slot pode estar guardando a RVA do hint/name */
        if (import_name_at(c, 0x00400000u + slot, name3, sizeof(name3))) nm3 = name3;
        if (nm3 && (sh = find_by_name_any(nm3)) != NULL) return sh;
    }

    /* (4) recuperação pelo NOME: se `va` (ou o conteúdo dele) for a RVA de um
     * IMAGE_IMPORT_BY_NAME dentro de um dos módulos carregados, o nome do
     * export está ali. As bases verificadas estão em docs/RECON.md. */
    {
        static const uint32_t bases[3] = { 0u, X2RW_BASE_WOLVIE, X2RW_BASE_BINK };
        char name[192];
        for (int b = 0; b < 3; b++) {
            if (import_name_at(c, bases[b] + va, name, sizeof(name))) {
                sh = find_by_name_any(name);
                if (sh) return sh;
            }
        }
        if (p && p != va) {
            for (int b = 0; b < 3; b++) {
                if (import_name_at(c, bases[b] + p, name, sizeof(name))) {
                    sh = find_by_name_any(name);
                    if (sh) return sh;
                }
            }
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Registro de thunks COM                                             */
/* ------------------------------------------------------------------ */

#define X2RW_MAX_COM 512
typedef struct {
    x2rw_api_t fn;
    int        nargs;
} x2rw_com_thunk;

static x2rw_com_thunk g_com[X2RW_MAX_COM];
static int g_com_n = 0;

uint32_t x2rw_com_register(x2rw_api_t fn, int nargs) {
    if (!fn) return 0;
    for (int i = 0; i < g_com_n; i++) {
        if (g_com[i].fn == fn && g_com[i].nargs == nargs)
            return X2RW_COM_BASE + 4u * (uint32_t)i;   /* reaproveita */
    }
    if (g_com_n >= X2RW_MAX_COM) {
        x2rw_log("[imports] tabela de thunks COM esgotada (%d)", X2RW_MAX_COM);
        return 0;
    }
    g_com[g_com_n].fn = fn;
    g_com[g_com_n].nargs = nargs;
    return X2RW_COM_BASE + 4u * (uint32_t)g_com_n++;
}

uint32_t x2rw_com_object_create(x2rw_ctx *c, const uint32_t *vtbl_vas, uint32_t count) {
    if (!vtbl_vas || count == 0) return 0;
    /* 4 bytes de ponteiro de vtbl + count slots */
    uint32_t obj = x2rw_heap_alloc(c, 4u + 4u * count);
    if (!obj) return 0;
    x2rw_write32(c, obj, obj + 4u);
    for (uint32_t i = 0; i < count; i++)
        x2rw_write32(c, obj + 4u + 4u * i, vtbl_vas[i]);
    return obj;
}

/* ------------------------------------------------------------------ */
/* Invocação                                                          */
/* ------------------------------------------------------------------ */

static uint32_t invoke_from_stack(x2rw_ctx *c, x2rw_api_t fn, int nargs) {
    uint32_t args[X2RW_STACK_MAX_ARGS];
    int n = nargs;
    if (n < 0) n = 0;
    if (n > X2RW_STACK_MAX_ARGS) n = X2RW_STACK_MAX_ARGS;
    /* stdcall: arg0 está EXATAMENTE em esp (sem endereço de retorno). */
    for (int i = 0; i < n; i++)
        args[i] = x2rw_read32(c, c->r.esp + (uint32_t)(4 * i));

    uint32_t r;
    if (fn) r = fn(c, args, nargs);
    else    r = shim_unimplemented(c, args, nargs);

    /* callee limpa a pilha (stdcall) */
    c->r.esp += 4u * (uint32_t)nargs;
    c->r.eax = r;
    return r;
}

int x2rw_import_dispatch(x2rw_ctx *c, uint32_t va) {
    if (c->fault) return 1;

    /* Correção opcional de carga (ver x2rw_imports_patch_iat): só quando o
     * integrador pedir, porque reescrever a IAT é invasivo. */
    {
        static int patched = 0;
        if (!patched) {
            patched = 1;
            const char *p = getenv("X2RW_PATCH_IAT");
            if (p && *p && *p != '0') x2rw_imports_patch_iat(c);
        }
    }

    /* Thunks COM sintéticos (métodos de D3D8/DInput8/DirectSound). */
    if (va >= X2RW_COM_BASE && va < X2RW_COM_LIMIT) {
        uint32_t idx = (va - X2RW_COM_BASE) / 4u;
        if (idx < (uint32_t)g_com_n && g_com[idx].fn) {
            invoke_from_stack(c, g_com[idx].fn, g_com[idx].nargs);
            return 1;
        }
        x2rw_log("[imports] thunk COM inválido va=%08x idx=%u", va, idx);
        x2rw_fault(c, X2RW_FAULT_UNRESOLVED, va, "thunk COM inválido");
        return 1;
    }

    /* Exports resolvidos dinamicamente por GetProcAddress. */
    if (va >= X2RW_EXPORT_BASE && va < X2RW_EXPORT_LIMIT) {
        uint32_t idx = (va - X2RW_EXPORT_BASE) / 4u;
        if (idx < (uint32_t)X2RW_N_SHIMS) {
            const x2rw_shim_t *sh = &g_shims[idx];
            g_current_shim = sh;
            invoke_from_stack(c, sh->fn, sh->nargs);
            g_current_shim = NULL;
            return 1;
        }
        x2rw_fault(c, X2RW_FAULT_UNRESOLVED, va, "thunk de export inválido");
        return 1;
    }

    const x2rw_shim_t *sh = resolve_va_deep(c, va);
    if (!sh) return 0;   /* não é import: o runtime decide (fault) */

    if (sh->fn == shim_unimplemented) {
        /* loga antes para o fault do handler sair com nome certo */
        g_current_shim = sh;
        x2rw_log("[imports] símbolo não implementado chamado: %s!%s va=%08x",
                 sh->dll, sh->name, va);
    }
    g_current_shim = sh;
    invoke_from_stack(c, sh->fn, sh->nargs);
    g_current_shim = NULL;
    return 1;
}

uint32_t x2rw_call_import(x2rw_ctx *c, uint32_t va, uint32_t *args, int nargs) {
    const x2rw_shim_t *sh = resolve_va_deep(c, va);
    if (va >= X2RW_COM_BASE && va < X2RW_COM_LIMIT) {
        uint32_t idx = (va - X2RW_COM_BASE) / 4u;
        if (idx < (uint32_t)g_com_n && g_com[idx].fn)
            return g_com[idx].fn(c, args, g_com[idx].nargs);
        return 0;
    }
    if (va >= X2RW_EXPORT_BASE && va < X2RW_EXPORT_LIMIT) {
        uint32_t idx = (va - X2RW_EXPORT_BASE) / 4u;
        if (idx < (uint32_t)X2RW_N_SHIMS && g_shims[idx].fn)
            return g_shims[idx].fn(c, args, nargs);
        return 0;
    }
    if (!sh) {
        x2rw_log("[imports] x2rw_call_import: va %08x desconhecido", va);
        return 0;
    }
    g_current_shim = sh;
    uint32_t r = sh->fn ? sh->fn(c, args, nargs) : shim_unimplemented(c, args, nargs);
    g_current_shim = NULL;
    return r;
}

/* ------------------------------------------------------------------ */
/* Lookup por nome/ordinal (GetProcAddress)                           */
/* ------------------------------------------------------------------ */

uint32_t x2rw_import_lookup_export(const char *dll, const char *name) {
    const x2rw_shim_t *sh = find_by_dll_name(dll, name);
    if (!sh) return 0;
    /* índice no registro -> VA sintético reconhecido pelo dispatch */
    return X2RW_EXPORT_BASE + 4u * (uint32_t)(sh - g_shims);
}

uint32_t x2rw_import_lookup_ordinal(const char *dll, uint32_t ordinal) {
    /* Só há um import por ordinal na IAT do jogo: DSOUND.dll!ordinal_11, que
     * é DirectSoundCreate (ver docs/RECON.md §12 e o comentário em
     * x2rw_audio.c). */
    if (dll && strcasecmp(dll, "DSOUND.dll") == 0 && ordinal == 11u)
        return x2rw_import_lookup_export("DSOUND.dll", "ordinal_11");
    x2rw_log("[imports] import por ordinal não mapeado: %s!%u",
             dll ? dll : "?", ordinal);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Helper opcional: fixar a IAT em ponteiros próprios                 */
/* ------------------------------------------------------------------ */
/* Não é necessário para o despacho normal (a cadeia resolve_va_deep cobre
 * RVA de hint/name e thunk). Existe para o integrador ligar
 * X2RW_PATCH_IAT=1 e forçar cada slot a apontar para si mesmo, caso algum
 * layout de carga diferente apareça. */
void x2rw_imports_patch_iat(x2rw_ctx *c) {
    for (int i = 0; i < X2WR_N_IMPORTS_WOLVIE; i++)
        x2rw_write32(c, x2wr_imports_Wolvie[i].va, x2wr_imports_Wolvie[i].va);
    for (int i = 0; i < X2WR_N_IMPORTS_BINKW32; i++)
        x2rw_write32(c, x2wr_imports_binkw32[i].va,
                     x2wr_imports_binkw32[i].va);
    x2rw_log("[imports] IAT fixada em auto-referência (%d slots)",
             X2WR_N_IMPORTS_WOLVIE + X2WR_N_IMPORTS_BINKW32);
}
