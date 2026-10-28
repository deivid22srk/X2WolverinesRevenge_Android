/* X2: Wolverine's Revenge - port Android
 * ============================================================================
 * x2rw_bink.c — shims de VÍDEO: binkw32.dll (Bink Video 2002).
 *
 * O CASO ESPECIAL DO BINK
 * -----------------------
 * binkw32.dll é um binário SEPARADO do jogo e TAMBÉM foi recompilado: os
 * exports vivem em gen/binkw32_*.c como funções lifted (nome montado com o
 * prefixo do módulo + RVA), com base de carga 0x03000000. O jogo importa 15
 * dos 85 exports (ver docs/RECON.md e gen/Wolvie_imports.h); estes 15 são
 * exatamente os da lista mestra X2RW_SHIMS_BINK(X).
 *
 * POR QUE NÃO CHAMAMOS OS SÍMBOLOS DE gen/ DIRETAMENTE
 * ----------------------------------------------------
 * gen/ é DERIVADO DO BINÁRIO DO JOGO e não existe no build app-only (o
 * repositório público compila com ci/gen_stub/). Uma referência direta aos
 * símbolos lifted de binkw32 quebraria o link nesse build. Então cada
 * handler:
 *   1. resolve o VA do export com `x2rw_resolve(va)` (função do runtime,
 *      que devolve NULL quando o módulo não está registrado);
 *   2. se resolveu, PREPARA A PILHA como o lifted espera (o lifted lê os
 *      argumentos em esp+4.. porque quem o chama empilha um endereço de
 *      retorno) e FORWARDa a chamada — a decodificação real, quando
 *      existir, acontece lá;
 *   3. se não resolveu (app-only), mantém um estado sintético próprio e
 *      devolve o valor documentado abaixo.
 *
 * FORWARDING E stdcall (a parte delicada)
 * ---------------------------------------
 * O despachante já ajusta c->r.esp += 4*nargs, então o shim NÃO mexe em
 * esp. Para chamar o lifted:
 *     esp_guardado = esp;
 *     x2rw_push(dummy_ret);         // lifted passa a ver o arg0 em esp+4
 *     fn(c);                        // o `ret N` do lifted desfaz o dummy
 *     esp = esp_guardado;           // garante o esp canônico para o
 *                                   // despachante limpar os nargs
 * O restauro é o mesmo padrão usado pelos chamadores gerados em gen/ (ver
 * `c->r.esp = _r1053` depois de um x2rw_call indireto).
 *
 * O QUE NÃO ESTÁ IMPLEMENTADO DE VERDADE (registrado, não escondido)
 * ------------------------------------------------------------------
 *  - Não há decodificador de vídeo nesta fase: _BinkDoFrame só registra e
 *    devolve 0; _BinkCopyToBuffer devolve 0 e NENHUM PIXEL é copiado (o
 *    buffer do jogo fica como estava). Sem o lifting do binkw32.dll em
 *    gen/, o vídeo não aparece na tela.
 *  - _BinkWait devolve 0 SEMPRE e nunca bloqueia: sem timer de Bink o jogo
 *    não pode ficar preso esperando o "próximo frame".
 *  - O sistema de som do Bink (_BinkSetSoundSystem/_BinkOpenDirectSound)
 *    não está ligado a nenhuma saída real: só guardamos ponteiros/funções.
 *  - Os handles sintéticos da tabela g_bnk[] só existem no caminho
 *    app-only; em build full o handle é o do próprio Bink.
 * ============================================================================
 */
#include "x2rw_runtime.h"
#include "x2rw_shims.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Endereços (VA) dos 15 exports em binkw32.dll (base 0x03000000)      */
/* ------------------------------------------------------------------ */
/* Lidos da tabela de exports do PE (gen/binkw32_table.c usa as mesmas
 * RVAs). São CONSTANTES de dado — nenhum símbolo de gen/ é referenciado,
 * só passamos o VA para x2rw_resolve(). */
#define BNK_VA_DX8_SURFACE_TYPE 0x03011a70u  /* _BinkDX8SurfaceType@4  */
#define BNK_VA_GET_ERROR        0x03011b90u  /* _BinkGetError@0        */
#define BNK_VA_SET_SOUND_SYSTEM 0x03011ba0u  /* _BinkSetSoundSystem@8  */
#define BNK_VA_SET_SOUND_TRACK  0x03012080u  /* _BinkSetSoundTrack@8   */
#define BNK_VA_OPEN             0x030122c0u  /* _BinkOpen@8            */
#define BNK_VA_COPY_TO_BUFFER   0x030131b0u  /* _BinkCopyToBuffer@28   */
#define BNK_VA_DO_FRAME         0x03013f00u  /* _BinkDoFrame@4         */
#define BNK_VA_NEXT_FRAME       0x03014520u  /* _BinkNextFrame@4       */
#define BNK_VA_CLOSE            0x03014990u  /* _BinkClose@4           */
#define BNK_VA_WAIT             0x03014be0u  /* _BinkWait@4            */
#define BNK_VA_PAUSE            0x03014e00u  /* _BinkPause@8           */
#define BNK_VA_SET_VOLUME       0x030158d0u  /* _BinkSetVolume@12      */
#define BNK_VA_LOGO_ADDRESS     0x03015a10u  /* _BinkLogoAddress@0     */
#define BNK_VA_SET_VIDEO_ON_OFF 0x03015cf0u  /* _BinkSetVideoOnOff@8   */
#define BNK_VA_OPEN_DSOUND      0x030172e0u  /* _BinkOpenDirectSound@4 */

/* D3DFMT_X8R8G8B8 (d3d8types.h) — formato que o Bink costuma pedir para a
 * superfície do DX8 no jogo. */
#define BNK_D3DFMT_X8R8G8B8 21u

/* ------------------------------------------------------------------ */
/* Estado sintético (caminho app-only)                                */
/* ------------------------------------------------------------------ */
/* Handle sintético = BNK_HANDLE_BASE + índice (1-based). É um VA fora de
 * qualquer região usada (heap do guest 0x20000000+, módulos 0x00400000 e
 * 0x03000000), então um handle "vaza" no log de forma inequívoca. */
#define BNK_HANDLE_BASE 0x50000001u
#define BNK_MAX_HANDLES 16

typedef struct {
    uint32_t in_use;
    char     name[128];      /* caminho do .bik lido do guest            */
    uint32_t flags;          /* dwFlags do _BinkOpen                     */
    int32_t  pause;          /* _BinkPause(value)                        */
    int32_t  volume;         /* _BinkSetVolume(-1 desligado .. 32768)    */
    int32_t  track;          /* faixa usada no último _BinkSetVolume     */
    int32_t  video_on;       /* _BinkSetVideoOnOff                       */
    uint32_t do_frame_calls;
    uint32_t next_frame_calls;
    uint32_t wait_calls;
    uint32_t copy_calls;
} bnk_handle;

static bnk_handle g_bnk[BNK_MAX_HANDLES];

/* Estado global do Bink (não pertence a um handle): faixa de som, sistema
 * de som instalado e objeto DirectSound devolvido por _BinkOpenDirectSound.
 * Nada disto está ligado a uma saída real — é só o que o jogo guardou. */
static int32_t  g_bnk_sound_track  = -1;
static uint32_t g_bnk_sound_system = 0u;
static uint32_t g_bnk_dsound       = 0u;

static bnk_handle *bnk_lookup(uint32_t h) {
    if (h < BNK_HANDLE_BASE) return NULL;
    uint32_t idx = h - BNK_HANDLE_BASE;
    if (idx >= BNK_MAX_HANDLES) return NULL;
    return g_bnk[idx].in_use ? &g_bnk[idx] : NULL;
}

static uint32_t bnk_alloc(void) {
    for (uint32_t i = 0; i < BNK_MAX_HANDLES; i++) {
        if (!g_bnk[i].in_use) {
            memset(&g_bnk[i], 0, sizeof(g_bnk[i]));
            g_bnk[i].in_use = 1u;
            g_bnk[i].volume = 32768;   /* volume máximo por padrão do Bink */
            g_bnk[i].video_on = 1;
            return BNK_HANDLE_BASE + i;
        }
    }
    return 0u;
}

static void bnk_free(uint32_t h) {
    bnk_handle *b = bnk_lookup(h);
    if (b) b->in_use = 0u;
}

/* ------------------------------------------------------------------ */
/* Forwarding para o código lifted (gen/binkw32_*.c), se carregado     */
/* ------------------------------------------------------------------ */

/* Devolve o ponteiro da função lifted do VA, ou NULL (build app-only). */
static x2rw_fn_t bnk_lifted(uint32_t va) {
    return x2rw_resolve(va);
}

/* Chama o lifted com a pilha preparada (ver comentário do topo) e devolve
 * o EAX dele. Devolve `fallback` se não houver código lifted. */
static uint32_t bnk_forward(x2rw_ctx *c, uint32_t va, uint32_t fallback) {
    x2rw_fn_t fn = bnk_lifted(va);
    if (!fn) return fallback;

    /* Endereço de retorno FICTÍCIO: existe só para o lifted ler seus
     * argumentos em esp+4.. (o `ret N` o descarta da pilha virtual, como
     * fazem os chamadores gerados em gen/). Escolhemos um valor que
     * x2rw_resolve() NÃO resolve, para que ele nunca seja executado por
     * engano — um VA fixo poderia cair dentro de um módulo mapeado. O
     * estado de falha do contexto é preservado: isto é só uma sondagem. */
    int saved_fault = c->fault;
    uint32_t dummy = 0xDEADBEEFu;
    if (x2rw_resolve(dummy) != NULL) {
        dummy = 0x7FFFFFFFu;
        if (x2rw_resolve(dummy) != NULL) dummy = 0x6FFFFFFFu;
    }
    c->fault = saved_fault;

    uint32_t saved = c->r.esp;
    x2rw_push(c, dummy);
    if (c->fault) { c->r.esp = saved; return fallback; }
    fn(c);
    /* O `ret N` do lifted já tirou o dummy; o esp guardado é o que o
     * despachante espera para fazer esp += 4*nargs. */
    c->r.esp = saved;
    return c->r.eax;
}

/* ------------------------------------------------------------------ */
/* Handles de vídeo: _BinkOpen / _BinkClose                           */
/* ------------------------------------------------------------------ */

/* HBINK _BinkOpen(const char *name, DWORD flags) — 2 args. */
uint32_t bnk_BinkOpen(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t name_va = (nargs > 0) ? args[0] : 0u;
    uint32_t flags   = (nargs > 1) ? args[1] : 0u;

    if (name_va == 0u) {
        /* Contrato: nome NULL => falha (devolve 0). */
        x2rw_log("[bink] _BinkOpen(NULL) -> 0 (sem nome de arquivo)");
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }

    char win[256];
    uint32_t n = x2rw_guest_strlen(c, name_va);
    if (n >= sizeof(win)) n = sizeof(win) - 1u;
    if (n) x2rw_mem_read(name_va, win, n);
    win[n] = 0;

    /* Caminho convertido só para o log (o arquivo .bik é aberto pelo
     * próprio Bink lifted, via mmio/CreateFileA dos shims). */
    char host[256];
    x2rw_win32_path_to_host(win, host, sizeof(host));

    /* Com o binkw32 recompilado, o handle REAL é o dele; a tabela sintética
     * só é usada quando o módulo lifted não está carregado, para o estado do
     * filme ter onde morar. */
    x2rw_fn_t lifted_fn = bnk_lifted(BNK_VA_OPEN);
    uint32_t h = 0u;
    if (!lifted_fn) {
        h = bnk_alloc();
        if (h) {
            bnk_handle *b = bnk_lookup(h);
            if (b) {
                size_t cn = strlen(win);
                if (cn >= sizeof(b->name)) cn = sizeof(b->name) - 1u;
                memcpy(b->name, win, cn);
                b->name[cn] = 0;
                b->flags = flags;
            }
        }
    }

    uint32_t lifted = bnk_forward(c, BNK_VA_OPEN, 0u);
    uint32_t result = lifted ? lifted : h;
    x2rw_log("[bink] _BinkOpen('%s' -> host '%s', flags=%08x) -> handle %08x "
             "(%s)", win, host, flags, result,
             lifted ? "handle do Bink lifted" : "handle sintético (app-only)");
    X2RW_OK();
    return result;
}

/* void _BinkClose(HBINK bnk) — 1 arg. */
uint32_t bnk_BinkClose(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t h = (nargs > 0) ? args[0] : 0u;
    bnk_handle *b = bnk_lookup(h);
    if (b) x2rw_log("[bink] _BinkClose(%08x): '%s' — frames=DoFrame:%u "
                    "NextFrame:%u Wait:%u CopyToBuffer:%u",
                    h, b->name, b->do_frame_calls, b->next_frame_calls,
                    b->wait_calls, b->copy_calls);
    else
        x2rw_log("[bink] _BinkClose(%08x)", h);
    (void)bnk_forward(c, BNK_VA_CLOSE, 0u);
    bnk_free(h);
    X2RW_OK();
    return 0u;
}

/* ------------------------------------------------------------------ */
/* Decodificação (NÃO implementada nesta fase)                        */
/* ------------------------------------------------------------------ */

/* void _BinkDoFrame(HBINK bnk) — 1 arg.
 * NÃO existe decodificador aqui: não fingimos que decodificamos. Só
 * registramos a chamada (o lifting do binkw32 em gen/ é quem decodifica,
 * quando presente) e devolvemos 0. */
uint32_t bnk_BinkDoFrame(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t h = (nargs > 0) ? args[0] : 0u;
    bnk_handle *b = bnk_lookup(h);
    if (b) b->do_frame_calls++;
    uint32_t r = bnk_forward(c, BNK_VA_DO_FRAME, 0u);
    if (!b || b->do_frame_calls <= 2u)
        x2rw_log("[bink] _BinkDoFrame(%08x) -> %u: NENHUM frame foi "
                 "decodificado neste módulo%s", h, r,
                 bnk_lifted(BNK_VA_DO_FRAME) ? "" :
                 " (binkw32 não está lifted: build app-only)");
    X2RW_OK();
    return r;
}

/* int _BinkWait(HBINK bnk) — 1 arg.
 * Devolve 0 IMEDIATAMENTE e NUNCA bloqueia: sem timer de Bink, esperar pelo
 * próximo frame travaria o jogo. */
uint32_t bnk_BinkWait(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t h = (nargs > 0) ? args[0] : 0u;
    bnk_handle *b = bnk_lookup(h);
    if (b) b->wait_calls++;
    uint32_t r = bnk_forward(c, BNK_VA_WAIT, 0u);
    if (b && b->wait_calls == 1u)
        x2rw_log("[bink] _BinkWait(%08x) -> %u (nunca bloqueia)", h, r);
    X2RW_OK();
    return r;
}

/* void _BinkNextFrame(HBINK bnk) — 1 arg. */
uint32_t bnk_BinkNextFrame(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t h = (nargs > 0) ? args[0] : 0u;
    bnk_handle *b = bnk_lookup(h);
    if (b) b->next_frame_calls++;
    uint32_t r = bnk_forward(c, BNK_VA_NEXT_FRAME, 0u);
    if (!b || b->next_frame_calls <= 2u)
        x2rw_log("[bink] _BinkNextFrame(%08x) -> %u (nenhum frame avançou "
                 "neste módulo)", h, r);
    X2RW_OK();
    return r;
}

/* void _BinkCopyToBuffer(HBINK bnk, void *dest, int destpitch,
 *                        UINT destheight, UINT destx, UINT desty, DWORD flags)
 * — 7 args. NENHUM pixel é copiado: o buffer do jogo fica como estava.
 * Devolvemos 0 e registramos (a cópia real depende do decodificador). */
uint32_t bnk_BinkCopyToBuffer(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t h      = (nargs > 0) ? args[0] : 0u;
    uint32_t dest   = (nargs > 1) ? args[1] : 0u;
    uint32_t pitch  = (nargs > 2) ? args[2] : 0u;
    uint32_t height = (nargs > 3) ? args[3] : 0u;
    uint32_t x      = (nargs > 4) ? args[4] : 0u;
    uint32_t y      = (nargs > 5) ? args[5] : 0u;
    uint32_t flags  = (nargs > 6) ? args[6] : 0u;
    bnk_handle *b = bnk_lookup(h);
    if (b) b->copy_calls++;
    uint32_t r = bnk_forward(c, BNK_VA_COPY_TO_BUFFER, 0u);
    if (!b || b->copy_calls <= 2u)
        x2rw_log("[bink] _BinkCopyToBuffer(bnk=%08x dest=%08x pitch=%u h=%u "
                 "x=%u y=%u flags=%08x) -> %u: NENHUM pixel foi copiado — "
                 "sem decodificador de Bink nesta fase", h, dest, pitch, height,
                 x, y, flags, r);
    X2RW_OK();
    return r;
}

/* ------------------------------------------------------------------ */
/* Estado do handle                                                   */
/* ------------------------------------------------------------------ */

/* int _BinkPause(HBINK bnk, int pause) — 2 args. */
uint32_t bnk_BinkPause(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t h = (nargs > 0) ? args[0] : 0u;
    int32_t  p = (nargs > 1) ? (int32_t)args[1] : 0;
    bnk_handle *b = bnk_lookup(h);
    if (b) b->pause = p;
    x2rw_log("[bink] _BinkPause(%08x, %d) — estado guardado; nenhum quadro "
             "está sendo reproduzido neste módulo", h, p);
    (void)bnk_forward(c, BNK_VA_PAUSE, 0u);
    X2RW_OK();
    return 0u;
}

/* void _BinkSetSoundTrack(UINT trackID, ...) — 2 args (a lista de tracks
 * termina em -1; só o primeiro valor é guardado). */
uint32_t bnk_BinkSetSoundTrack(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t a0 = (nargs > 0) ? args[0] : 0u;
    uint32_t a1 = (nargs > 1) ? args[1] : 0u;
    /* Sem handle no parâmetro: a faixa é global ao Bink. */
    g_bnk_sound_track = (int32_t)a0;
    x2rw_log("[bink] _BinkSetSoundTrack(%d, %d) — faixas guardadas; o sistema "
             "de som do Bink não está ligado a nenhuma saída real",
             (int32_t)a0, (int32_t)a1);
    (void)bnk_forward(c, BNK_VA_SET_SOUND_TRACK, 0u);
    X2RW_OK();
    return 0u;
}

/* void _BinkSetVideoOnOff(HBINK bnk, int onoff) — 2 args. */
uint32_t bnk_BinkSetVideoOnOff(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t h   = (nargs > 0) ? args[0] : 0u;
    int32_t  on  = (nargs > 1) ? (int32_t)args[1] : 0;
    bnk_handle *b = bnk_lookup(h);
    if (b) b->video_on = on;
    x2rw_log("[bink] _BinkSetVideoOnOff(%08x, %d) — guardado (nada é "
             "desenhado sem decodificador)", h, on);
    (void)bnk_forward(c, BNK_VA_SET_VIDEO_ON_OFF, 0u);
    X2RW_OK();
    return 0u;
}

/* void _BinkSetVolume(HBINK bnk, UINT trackID, int volume) — 3 args. */
uint32_t bnk_BinkSetVolume(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t h     = (nargs > 0) ? args[0] : 0u;
    int32_t  track = (nargs > 1) ? (int32_t)args[1] : 0;
    int32_t  vol   = (nargs > 2) ? (int32_t)args[2] : 0;
    bnk_handle *b = bnk_lookup(h);
    if (b) { b->volume = vol; b->track = track; }
    x2rw_log("[bink] _BinkSetVolume(%08x, track=%d, %d) — guardado; o áudio "
             "do filme não sai por nenhuma saída real", h, track, vol);
    (void)bnk_forward(c, BNK_VA_SET_VOLUME, 0u);
    X2RW_OK();
    return 0u;
}

/* ------------------------------------------------------------------ */
/* Sistema de som do Bink (desligado)                                 */
/* ------------------------------------------------------------------ */

/* void _BinkSetSoundSystem(void *open_func, UINT param) — 2 args.
 * Guardamos o ponteiro da função de abertura; NADA é ligado a uma saída de
 * áudio real (nem ao DirectSound sintético deste port). */
uint32_t bnk_BinkSetSoundSystem(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t fn_va = (nargs > 0) ? args[0] : 0u;
    uint32_t param = (nargs > 1) ? args[1] : 0u;
    g_bnk_sound_system = fn_va;
    x2rw_log("[bink] _BinkSetSoundSystem(open_func=%08x, param=%08x) — "
             "guardado; o sistema de som do Bink NÃO está ligado a nenhuma "
             "saída real", fn_va, param);
    (void)bnk_forward(c, BNK_VA_SET_SOUND_SYSTEM, 0u);
    X2RW_OK();
    return 0u;
}

/* void *_BinkOpenDirectSound(UINT uDeviceID) — 1 arg.
 * Devolve o ponteiro guardado (0 na primeira chamada): não há objeto
 * DirectSound do Bink. */
uint32_t bnk_BinkOpenDirectSound(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t dev = (nargs > 0) ? args[0] : 0u;
    uint32_t r = bnk_forward(c, BNK_VA_OPEN_DSOUND, g_bnk_dsound);
    g_bnk_dsound = r;
    x2rw_log("[bink] _BinkOpenDirectSound(dev=%u) -> %08x — ponteiro "
             "guardado; nenhuma saída DirectSound do Bink existe neste port",
             dev, r);
    X2RW_OK();
    return r;
}

/* ------------------------------------------------------------------ */
/* Utilitários                                                        */
/* ------------------------------------------------------------------ */

/* U32 _BinkDX8SurfaceType(void) — sem argumentos (o @4 é só a decoração
 * stdcall; o export não recebe parâmetro nenhum). Devolve um formato D3D8
 * plausível para a superfície: 21 = D3DFMT_X8R8G8B8. */
uint32_t bnk_BinkDX8SurfaceType(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)args; (void)nargs;
    uint32_t r = bnk_forward(c, BNK_VA_DX8_SURFACE_TYPE, BNK_D3DFMT_X8R8G8B8);
    if (r == 0u) r = BNK_D3DFMT_X8R8G8B8;
    x2rw_log("[bink] _BinkDX8SurfaceType() -> %u (D3DFMT_X8R8G8B8)", r);
    X2RW_OK();
    return r;
}

/* U32 _BinkGetError(void) — sem argumentos (decorado @0). */
uint32_t bnk_BinkGetError(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)args; (void)nargs;
    uint32_t r = bnk_forward(c, BNK_VA_GET_ERROR, 0u);
    x2rw_log("[bink] _BinkGetError() -> %u (%s)", r,
             r ? "erro reportado pelo Bink" : "sem erro");
    X2RW_OK();
    return r;
}

/* void *_BinkLogoAddress(void) — sem argumentos (decorado @0).
 * Sem logotipo do Bink neste port: devolve NULL. */
uint32_t bnk_BinkLogoAddress(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)args; (void)nargs;
    uint32_t r = bnk_forward(c, BNK_VA_LOGO_ADDRESS, 0u);
    x2rw_log("[bink] _BinkLogoAddress() -> %08x (sem logotipo do Bink)", r);
    X2RW_OK();
    return r;
}
