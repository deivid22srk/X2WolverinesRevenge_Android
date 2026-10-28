/* X2: Wolverine's Revenge - build de DESKTOP (Linux x86-64)
 *
 * Entrada de depuração: carrega os PE do jogo e executa o entrypoint, com o
 * mesmo loader que o Android usa. Serve para validar o lifting (gen/) e os
 * shims sem passar por APK — a regra do projeto é que o APK só nasce no CI.
 *
 * Uso:
 *     ./x2wr_run <pasta-do-jogo> [teclas]
 *
 * `teclas` é opcional e tem o formato VK:estado separados por vírgula, por
 * exemplo: 75:1,75:0 (aperta e solta o soco). Existe porque ainda não há
 * camada de input no desktop; sem ela, só dá para observar a inicialização.
 *
 * O QUE É STUB (sem otimismo, como manda docs/TESTED.md):
 *   - x2rw_host_register_shims(): os shims Win32 reais não existem. Aqui ela
 *     apenas registra isso no log.
 *   - x2rw_import_dispatch() está no stub de desktop/runtime_override.c.
 *   - não há janela/render/áudio: o port só executa o código do jogo.
 *
 * O loader independente de plataforma está em
 * android/app/src/main/cpp/x2rw_loader.inc — o MESMO arquivo que o Android
 * compila. Isso é de propósito: duas cópias divergiriam em silêncio.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "x2rw_runtime.h"
#include "x2rw_host.h"

/* Implementa o loader uma única vez nesta unidade de tradução (o Android faz
 * o mesmo em x2rw_android_main.c). */
#define X2WR_LOADER_IMPL
#include "x2rw_loader.inc"

/* ------------------------------------------------------------------ */
/* Estado do host                                                     */
/* ------------------------------------------------------------------ */

#define X2WR_KEY_MAP_SIZE 256

static x2rw_ctx *g_ctx = NULL;
static x2rw_pe   g_wolvie;
static x2rw_pe   g_bink;
static int       g_have_wolvie = 0;
static int       g_have_bink = 0;
static int       g_render_w = 0;
static int       g_render_h = 0;
static unsigned char g_keys[X2WR_KEY_MAP_SIZE];

/* ------------------------------------------------------------------ */
/* Leitura de arquivo (filesystem do host)                            */
/* ------------------------------------------------------------------ */

uint8_t *x2rw_read_game_file(const char *game_dir, const char *name,
                             size_t *out_size) {
    if (out_size) *out_size = 0;
    if (!game_dir || !*game_dir) {
        fprintf(stderr, "[x2wr] pasta do jogo não informada\n");
        return NULL;
    }

    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", game_dir, name);

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[x2wr] não abriu %s: ", path);
        perror("");
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return NULL; }
    rewind(f);

    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(buf); return NULL; }
    if (out_size) *out_size = rd;
    fprintf(stderr, "[x2wr] %s: %zu bytes\n", path, rd);
    return buf;
}

void x2rw_mem_free_impl(void *p) {
    free(p);
}

/* ------------------------------------------------------------------ */
/* Host                                                               */
/* ------------------------------------------------------------------ */

int x2rw_host_init(const char *game_dir, int width, int height) {
    g_render_w = width;
    g_render_h = height;

    if (!g_ctx) {
        g_ctx = x2rw_ctx_create();
        if (!g_ctx) {
            fprintf(stderr, "[x2wr] x2rw_ctx_create falhou\n");
            return 1;
        }
        fprintf(stderr, "[x2wr] contexto criado (stack 0x%08x..0x%08x)\n",
                g_ctx->stack_lo, g_ctx->stack_hi);
    }

    if (!g_have_wolvie) {
        if (x2rw_load_pe(game_dir, "Wolvie.exe", X2RW_BASE_WOLVIE, 1,
                         &g_wolvie) != 0) {
            fprintf(stderr, "[x2wr] falha ao carregar Wolvie.exe\n");
            return 1;
        }
        g_have_wolvie = 1;
        fprintf(stderr, "[x2wr] Wolvie.exe em 0x%08x (entry RVA 0x%x)\n",
                g_wolvie.base, g_wolvie.entry_rva);
    }
    if (!g_have_bink) {
        if (x2rw_load_pe(game_dir, "binkw32.dll", X2RW_BASE_BINK, 0,
                         &g_bink) != 0) {
            fprintf(stderr, "[x2wr] falha ao carregar binkw32.dll\n");
            return 1;
        }
        g_have_bink = 1;
        fprintf(stderr, "[x2wr] binkw32.dll em 0x%08x\n", g_bink.base);
    }

#ifdef X2WR_HAVE_GEN
    {
        static const x2rw_module *mods[2];
        mods[0] = &x2rw_mod_wolvie;
        mods[1] = &x2rw_mod_bink;
        x2rw_modules_register(mods, 2);
        fprintf(stderr, "[x2wr] módulos: %u + %u funções\n",
                (unsigned)x2rw_mod_wolvie.n_functions,
                (unsigned)x2rw_mod_bink.n_functions);
    }
#else
    fprintf(stderr, "[x2wr] sem código lifted: módulos não registrados\n");
#endif

    x2rw_host_register_shims();
    return 0;
}

/* FRACA de propósito: os shims reais (runtime/shims/x2rw_*.c) trazem a
 * definição FORTE e vencem no link. Duas definições fortes causariam
 * SÍMBOLO DUPLICADO. */
__attribute__((weak)) void x2rw_host_register_shims(void) {
    /* STUB CONSCIENTE. Os imports passam por x2rw_import_dispatch(); sem
     * shims o runtime registra X2RW_FAULT_UNRESOLVED — falha alta e clara,
     * que é o comportamento desejado. */
    fprintf(stderr, "[x2wr] shims Win32: NÃO implementados nesta árvore "
                    "(runtime/shims/ é de outro engenheiro)\n");
}

int x2rw_host_key_state(int scancode) {
    if (scancode < 0 || scancode >= X2WR_KEY_MAP_SIZE) return 0;
    return g_keys[scancode] ? 1 : 0;
}

void x2rw_host_key_set(int scancode, int down) {
    if (scancode < 0 || scancode >= X2WR_KEY_MAP_SIZE) return;
    g_keys[scancode] = down ? 1u : 0u;
    fprintf(stderr, "[x2wr] tecla VK=%d %s\n", scancode,
            down ? "pressionada" : "solta");
}

void x2rw_host_get_render_size(int *out_w, int *out_h) {
    if (out_w) *out_w = g_render_w;
    if (out_h) *out_h = g_render_h;
}

uint32_t x2rw_host_entry_rva(void) {
    return g_have_wolvie ? g_wolvie.entry_rva : 0u;
}

x2rw_ctx *x2rw_host_ctx(void) {
    return g_ctx;
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

/* Formato: "75:1,75:0" — VK:estado. Devolve o número de teclas aplicadas. */
static int apply_key_spec(const char *spec) {
    int applied = 0;
    const char *p = spec;
    while (p && *p) {
        char *end = NULL;
        long vk = strtol(p, &end, 10);
        if (end == p) break;
        p = end;
        if (*p != ':') break;
        p++;
        long down = strtol(p, &end, 10);
        if (end == p) break;
        p = end;
        x2rw_host_key_set((int)vk, down != 0);
        applied++;
        if (*p == ',') p++;
    }
    return applied;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "uso: %s <pasta-do-jogo> [VK:estado,VK:estado,...]\n"
            "  ex.: %s /caminho/game 75:1,75:0\n"
            "  A pasta deve conter Wolvie.exe e binkw32.dll.\n",
            argv[0], argv[0]);
        return 2;
    }

    const char *game_dir = argv[1];
    fprintf(stderr, "[x2wr] pasta do jogo: %s\n", game_dir);

    /* Sem janela no desktop: usamos um tamanho fixo só para o log. A
     * resolução real é decisão da camada gráfica (inexistente hoje). */
    if (x2rw_host_init(game_dir, 640, 480) != 0) {
        fprintf(stderr, "[x2wr] inicialização do host falhou\n");
        return 1;
    }

    if (argc >= 3) {
        int n = apply_key_spec(argv[2]);
        fprintf(stderr, "[x2wr] %d eventos de tecla aplicados\n", n);
    }

#ifndef X2WR_HAVE_GEN
    fprintf(stderr,
        "[x2wr] build SEM código lifted: nada a executar.\n"
        "       Gere gen/ (make gen) e recompile para rodar o jogo.\n");
    return 2;
#else
    uint32_t entry_rva = x2rw_host_entry_rva();
    if (entry_rva == 0) {
        fprintf(stderr, "[x2wr] entrypoint RVA ausente (PE não mapeado?)\n");
        return 1;
    }
    uint32_t entry_va = X2RW_BASE_WOLVIE + entry_rva;
    fprintf(stderr, "[x2wr] chamando entrypoint VA=0x%08x (RVA=0x%x)\n",
            entry_va, entry_rva);

    x2rw_call(g_ctx, entry_va);

    if (g_ctx->fault) {
        fprintf(stderr, "[x2wr] FAULT code=%u va=0x%08x msg=%s\n",
                (unsigned)g_ctx->fault_code, (unsigned)g_ctx->fault_va,
                g_ctx->fault_msg ? g_ctx->fault_msg : "?");
        fprintf(stderr, "[x2wr] estado: esp=%08x ebp=%08x eip_pc=%08x\n",
                g_ctx->r.esp, g_ctx->r.ebp, g_ctx->pc);
        /* Qualquer falha merece a cadeia de chamadas: sem ela o diagnóstico
         * vira adivinhação (foi assim que o off-by-4 da pilha passou tanto
         * tempo escondido). */
        x2rw_trace_dump();
        return 3;
    }

    fprintf(stderr, "[x2wr] entrypoint retornou normalmente\n");
    fprintf(stderr, "[x2wr] leituras=%llu escritas=%llu páginas=%llu chamadas=%llu\n",
            (unsigned long long)x2rw_stat_reads(),
            (unsigned long long)x2rw_stat_writes(),
            (unsigned long long)x2rw_stat_pages(),
            (unsigned long long)x2rw_stat_calls());
    return 0;
#endif
}
