/* X2: Wolverine's Revenge - port Android
 * ============================================================================
 * x2rw_user32.c — shims de USER32.dll (39 símbolos da lista mestra).
 *
 * PAPEL DESTE ARQUIVO
 * -------------------
 * Quem desenha é o Direct3D 8 (d3d8.dll + x2rw_d3d8_hooks). USER32 aqui não
 * renderiza nada: existe para
 *   (a) criar a janela e os DCs,
 *   (b) responder as métricas de tela consultadas na inicialização,
 *   (c) rodar o loop de mensagens SEM BLOQUEAR (um GetMessage/modal real
 *       travaria o APK: não há fila nem outro thread para postar),
 *   (d) manter handles ESTÁVEIS — o jogo guarda o HWND e o HDC em variáveis
 *       globais durante toda a execução.
 *
 * MODELO DE HANDLES
 * -----------------
 * HWND/HDC/HCURSOR/HICON são handles SINTÉTICOS de um contador estático a
 * partir de 0x2000, guardados em `g_obj[]` com o estado do host (título,
 * retângulo, style, USERDATA...). O contador só cresce: um handle liberado
 * nunca é reemitido, então um uso-após-liberar aparece como "handle
 * desconhecido" no log em vez de acertar outro objeto por acidente.
 *
 * POLÍTICA DE HANDLE DESCONHECIDO (nunca falhar em silêncio)
 * ----------------------------------------------------------
 *   - APIs de CONSULTA (GetClientRect, GetWindowRect, GetWindowLongA,
 *     GetClassLongA, IsWindowVisible, ClientToScreen/ScreenToClient)
 *     registram e falham com last error X2RW_ERROR_INVALID_HANDLE;
 *   - APIs de AÇÃO (ShowWindow, MoveWindow, SetFocus, UpdateWindow,
 *     SetForegroundWindow, BeginPaint/EndPaint, ReleaseDC) registram o
 *     aviso, deixam o last error em X2RW_ERROR_INVALID_HANDLE e seguem com
 *     o valor benigno, para não travar a inicialização por um handle que o
 *     port não conhece. O aviso fica no log para quem for depurar.
 *
 * MENSAGENS
 * ---------
 * PeekMessageA devolve sempre 0 (não há fila de mensagens). O loop do jogo
 * roda o corpo de render sem eventos Win32; a entrada de teclado/mouse vem
 * do DirectInput8 (x2rw_input.c), que o jogo polla. Nada aqui bloqueia.
 *
 * O QUE NÃO É IMPLEMENTADO (e por quê)
 * ------------------------------------
 * As APIs citadas na orientação geral do port mas que NÃO estão na lista
 * mestra medida (GetMessageA, SetWindowLongA/GWLP_USERDATA, GetAsyncKeyState,
 * GetKeyboardState/MapVirtualKeyA, wsprintfA/CharUpperA, menus, hotkeys,
 * hooks, ExitWindowsEx) não são importadas por Wolvie.exe nem por
 * binkw32.dll (conferido em gen/*_imports.h: 39 nomes de USER32 e 13 de
 * GDI32). O despachante só resolve nomes registrados em x2rw_shims.h, logo
 * implementá-las aqui seria código morto. As 39 funções abaixo são
 * exatamente a superfície importada; a entrada do jogo é o DirectInput8.
 * ============================================================================
 */
#include "x2rw_runtime.h"
#include "x2rw_shims.h"

/* ------------------------------------------------------------------ */
/* Constantes Win32 usadas aqui (o port não inclui windows.h)          */
/* ------------------------------------------------------------------ */

#define U32_WS_VISIBLE      0x10000000u
#define U32_WS_BORDER       0x00800000u
#define U32_WS_CAPTION      0x00C00000u   /* WS_BORDER | WS_DLGFRAME */
#define U32_WS_THICKFRAME   0x00040000u

/* GetWindowLong/SetWindowLong (índices negativos em relação a 0) */
#define U32_GWL_WNDPROC     (-4)
#define U32_GWL_HINSTANCE   (-6)
#define U32_GWL_HWNDPARENT  (-8)
#define U32_GWL_ID          (-12)
#define U32_GWL_STYLE       (-16)
#define U32_GWL_EXSTYLE     (-20)
#define U32_GWL_USERDATA    (-21)

/* GetClassLong */
#define U32_GCL_MENUNAME      (-8)
#define U32_GCL_HBRBACKGROUND (-10)
#define U32_GCL_HCURSOR       (-12)
#define U32_GCL_HICON         (-14)
#define U32_GCL_HMODULE       (-16)
#define U32_GCL_CBWNDEXTRA    (-18)
#define U32_GCL_CBCLSEXTRA    (-20)
#define U32_GCL_WNDPROC       (-24)
#define U32_GCL_STYLE         (-26)

/* GetWindow */
#define U32_GW_HWNDFIRST   0u
#define U32_GW_HWNDLAST    1u
#define U32_GW_HWNDNEXT    2u
#define U32_GW_HWNDPREV    3u
#define U32_GW_OWNER       4u
#define U32_GW_CHILD       5u

/* Tamanho de PAINTSTRUCT (32 bits) preenchido por BeginPaint. */
#define U32_PAINTSTRUCT_SIZE 64

/* Identidade de processo/thread: quem manda é o shim de KERNEL32 (getpid()
 * e uma thread de jogo só). GetWindowThreadProcessId reusa os MESMOS
 * handlers para que comparações do jogo — ex.:
 * GetWindowThreadProcessId(hwnd,&pid) == GetCurrentProcessId() — continuem
 * verdadeiras neste port. Ver o comentário no handler. */

/* ------------------------------------------------------------------ */
/* Tabelas de estado                                                  */
/* ------------------------------------------------------------------ */

enum {
    U32_KIND_FREE = 0,
    U32_KIND_WND,      /* janela           */
    U32_KIND_DC,       /* device context   */
    U32_KIND_CURSOR,   /* LoadCursorA      */
    U32_KIND_ICON      /* LoadIconA        */
};

#define U32_MAX_OBJ      96
#define U32_MAX_CLASSES  16
#define U32_MAX_RESOURCE 16
#define U32_FIRST_HANDLE 0x2000u

typedef struct {
    int      used;
    uint32_t handle;
    uint32_t kind;

    /* --- janela --- */
    uint32_t parent;        /* hWndParent / owner (0 = top-level)          */
    uint32_t class_atom;    /* ATOM devolvido por RegisterClassA (0 = n/d) */
    uint32_t style;
    uint32_t exstyle;
    uint32_t wndproc;       /* lpfnWndProc da classe                      */
    uint32_t hinstance;
    uint32_t hmenu;
    uint32_t create_lparam; /* lpParam de CreateWindowExA                 */
    uint32_t userdata;      /* GWLP_USERDATA (o jogo não tem SetWindowLong) */
    int32_t  x, y, w, h;    /* retângulo da janela (não do cliente)       */
    int32_t  visible;

    /* --- DC --- */
    uint32_t owner;         /* HWND dona do DC (0 = tela)                 */

    /* --- cursor/ícone --- */
    uint32_t resource_key;  /* id de recurso (MAKEINTRESOURCE) ou VA      */
} u32_obj;

typedef struct {
    int      used;
    uint32_t atom;
    uint32_t style, wndproc, hinstance, hicon, hcursor, hbrbackground;
    uint32_t name_va;
    char     name[64];
} u32_class;

typedef struct {
    int      used;
    uint32_t kind;
    uint32_t key;           /* id de recurso ou VA da string              */
    uint32_t handle;
} u32_resource;

static u32_obj      g_obj[U32_MAX_OBJ];
static u32_class    g_class[U32_MAX_CLASSES];
static u32_resource g_res[U32_MAX_RESOURCE];

static uint32_t g_next_handle = U32_FIRST_HANDLE;

static uint32_t g_main_hwnd   = 0u;   /* 1a janela top-level criada */
static uint32_t g_focus_hwnd  = 0u;   /* SetFocus/GetFocus          */
static uint32_t g_active_hwnd = 0u;   /* SetActiveWindow            */
static uint32_t g_cursor      = 0u;   /* cursor corrente (SetCursor)*/
static int32_t  g_cursor_show = 0;    /* contador de ShowCursor     */

/* Leitura segura do i-ésimo argumento (args[i] só vale para i < nargs). */
#define U32_A(i) (((i) < nargs) ? args[i] : 0u)

/* ------------------------------------------------------------------ */
/* Helpers de objeto                                                  */
/* ------------------------------------------------------------------ */

static u32_obj *u32_alloc_obj(uint32_t kind) {
    for (int i = 0; i < U32_MAX_OBJ; i++) {
        u32_obj *o = &g_obj[i];
        if (o->used) continue;
        memset(o, 0, sizeof(*o));
        o->used = 1;
        o->kind = kind;
        o->handle = g_next_handle++;
        return o;
    }
    x2rw_log("[user32] tabela de objetos cheia (%d): handle não criado "
             "(kind=%u)", U32_MAX_OBJ, kind);
    return NULL;
}

static u32_obj *u32_find(uint32_t h) {
    if (h == 0u) return NULL;
    for (int i = 0; i < U32_MAX_OBJ; i++)
        if (g_obj[i].used && g_obj[i].handle == h)
            return &g_obj[i];
    return NULL;
}

static u32_obj *u32_find_kind(uint32_t h, uint32_t kind) {
    u32_obj *o = u32_find(h);
    return (o && o->kind == kind) ? o : NULL;
}

/* Busca uma janela validando o handle. Devolve NULL e deixa o aviso + o
 * last error quando o handle não é uma janela criada por nós. */
static u32_obj *u32_wnd_or_log(uint32_t hwnd, const char *api) {
    u32_obj *o = u32_find_kind(hwnd, U32_KIND_WND);
    if (!o) {
        x2rw_log("[user32] %s: HWND %08x não é uma janela criada por "
                 "CreateWindowExA", api, hwnd);
        x2rw_last_error_set(X2RW_ERROR_INVALID_HANDLE);
    }
    return o;
}

/* DC de janela: um por HWND, reaproveitado entre GetDC/ReleaseDC (o
 * Windows também cacheia DCs; recriar a cada frame esgotaria a tabela). */
static u32_obj *u32_window_dc(uint32_t hwnd) {
    for (int i = 0; i < U32_MAX_OBJ; i++) {
        u32_obj *o = &g_obj[i];
        if (o->used && o->kind == U32_KIND_DC && o->owner == hwnd)
            return o;
    }
    u32_obj *dc = u32_alloc_obj(U32_KIND_DC);
    if (dc) {
        dc->owner = hwnd;
        x2rw_log("[user32] DC sintético %08x criado para HWND %08x",
                 dc->handle, hwnd);
    }
    return dc;
}

/* ------------------------------------------------------------------ */
/* Helpers de RECT/POINT (VAs do guest, nunca ponteiros do host)       */
/* ------------------------------------------------------------------ */

static int u32_read_rect(x2rw_ctx *c, uint32_t va, int32_t r[4]) {
    if (!va) return 0;
    r[0] = (int32_t)x2rw_read32(c, va + 0u);
    r[1] = (int32_t)x2rw_read32(c, va + 4u);
    r[2] = (int32_t)x2rw_read32(c, va + 8u);
    r[3] = (int32_t)x2rw_read32(c, va + 12u);
    return 1;
}

static void u32_write_rect(x2rw_ctx *c, uint32_t va,
                           int32_t l, int32_t t, int32_t r, int32_t b) {
    if (!va) return;
    x2rw_write32(c, va + 0u, (uint32_t)l);
    x2rw_write32(c, va + 4u, (uint32_t)t);
    x2rw_write32(c, va + 8u, (uint32_t)r);
    x2rw_write32(c, va + 12u, (uint32_t)b);
}

static int u32_read_point(x2rw_ctx *c, uint32_t va, int32_t p[2]) {
    if (!va) return 0;
    p[0] = (int32_t)x2rw_read32(c, va + 0u);
    p[1] = (int32_t)x2rw_read32(c, va + 4u);
    return 1;
}

static void u32_write_point(x2rw_ctx *c, uint32_t va, int32_t x, int32_t y) {
    if (!va) return;
    x2rw_write32(c, va + 0u, (uint32_t)x);
    x2rw_write32(c, va + 4u, (uint32_t)y);
}

/* Dimensões correntes da superfície de render (o D3D8/host são donos deste
 * valor; USER32 apenas REPORTA — CreateWindowExA não sobrescreve, porque a
 * janela é criada antes do device e o device é quem manda no back buffer). */
static int32_t u32_screen_w(void) { return g_render_w > 0 ? g_render_w : 1280; }
static int32_t u32_screen_h(void) { return g_render_h > 0 ? g_render_h : 720; }

/* ------------------------------------------------------------------ */
/* Helpers de classe e de recurso                                     */
/* ------------------------------------------------------------------ */

static u32_class *u32_find_class_name(const char *name) {
    for (int i = 0; i < U32_MAX_CLASSES; i++)
        if (g_class[i].used && strcmp(g_class[i].name, name) == 0)
            return &g_class[i];
    return NULL;
}

static u32_class *u32_find_class_atom(uint32_t atom) {
    if (atom == 0u || atom > U32_MAX_CLASSES) return NULL;
    u32_class *cl = &g_class[atom - 1u];
    return cl->used ? cl : NULL;
}

/* Handle estável para um cursor/ícone pedido por id de recurso (ou por
 * string): a mesma chave devolve SEMPRE o mesmo handle, porque o jogo
 * compara e guarda esses handles. */
static uint32_t u32_resource_handle(uint32_t kind, uint32_t key) {
    for (int i = 0; i < U32_MAX_RESOURCE; i++) {
        u32_resource *r = &g_res[i];
        if (r->used && r->kind == kind && r->key == key)
            return r->handle;
    }
    u32_obj *o = u32_alloc_obj(kind);
    if (!o) return 0u;
    o->resource_key = key;
    for (int i = 0; i < U32_MAX_RESOURCE; i++) {
        if (!g_res[i].used) {
            g_res[i].used = 1;
            g_res[i].kind = kind;
            g_res[i].key = key;
            g_res[i].handle = o->handle;
            return o->handle;          /* handle estável para esta chave */
        }
    }
    /* Sem slot para memorizar a chave: devolvemos o handle novo, mas ele
     * NÃO será reaproveitado na próxima chamada — precisa ficar visível. */
    x2rw_log("[user32] tabela de recursos cheia (%d): handle %08x "
             "(kind=%u, chave=%08x) não será estável nas próximas chamadas",
             U32_MAX_RESOURCE, o->handle, kind, key);
    return o->handle;
}

/* Lista as janelas com um dado `parent`, na ordem de criação (ordem dos
 * slots) — é o que GetWindow/GetTopWindow usam. */
static int u32_collect_windows(uint32_t parent, uint32_t *out, int cap) {
    int n = 0;
    for (int i = 0; i < U32_MAX_OBJ && n < cap; i++) {
        u32_obj *o = &g_obj[i];
        if (o->used && o->kind == U32_KIND_WND && o->parent == parent)
            out[n++] = o->handle;
    }
    return n;
}

/* x2rw_read_cstr tratado para VA 0 (título/classe/caption nulos são
 * válidos no Win32 e não devem gerar o aviso de "endereço baixo" do
 * runtime de memória). */
static void u32_read_str(x2rw_ctx *c, uint32_t va, char *buf, size_t cap) {
    if (!buf || cap == 0) return;
    if (va == 0u) { buf[0] = 0; return; }
    x2rw_read_cstr(c, va, buf, cap);
}

/* ------------------------------------------------------------------ */
/* Janela                                                             */
/* ------------------------------------------------------------------ */

uint32_t u32_RegisterClassA(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t lp = U32_A(0);
    if (!lp) {
        x2rw_log("[user32] RegisterClassA(NULL) — parâmetro inválido");
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }

    /* WNDCLASSA em 32 bits: style, lpfnWndProc, cbClsExtra, cbWndExtra,
     * hInstance, hIcon, hCursor, hbrBackground, lpszMenuName,
     * lpszClassName (10 × 4 bytes). */
    uint32_t style    = x2rw_read32(c, lp + 0u);
    uint32_t wndproc  = x2rw_read32(c, lp + 4u);
    uint32_t hinst    = x2rw_read32(c, lp + 16u);
    uint32_t hicon    = x2rw_read32(c, lp + 20u);
    uint32_t hcursor  = x2rw_read32(c, lp + 24u);
    uint32_t hbr      = x2rw_read32(c, lp + 28u);
    uint32_t name_va  = x2rw_read32(c, lp + 36u);

    char name[64];
    u32_read_str(c, name_va, name, sizeof(name));
    if (name[0] == 0) {
        x2rw_log("[user32] RegisterClassA: lpszClassName vazio/ilegível "
                 "(va=%08x)", name_va);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }

    u32_class *cl = u32_find_class_name(name);
    if (cl) {
        /* Windows devolve o mesmo ATOM e deixa ERROR_CLASS_ALREADY_EXISTS. */
        x2rw_log("[user32] RegisterClassA(\"%s\"): classe já registrada, "
                 "devolvendo ATOM %u", name, cl->atom);
        x2rw_last_error_set(X2RW_ERROR_ALREADY_EXISTS);
        return cl->atom;
    }

    for (int i = 0; i < U32_MAX_CLASSES; i++) {
        if (g_class[i].used) continue;
        cl = &g_class[i];
        memset(cl, 0, sizeof(*cl));
        cl->used = 1;
        cl->atom = (uint32_t)i + 1u;          /* ATOM não-zero (exigido) */
        cl->style = style;
        cl->wndproc = wndproc;
        cl->hinstance = hinst;
        cl->hicon = hicon;
        cl->hcursor = hcursor;
        cl->hbrbackground = hbr;
        cl->name_va = name_va;
        /* copia o nome para o host (64 bytes, truncado e NUL-terminado) */
        x2rw_read_cstr(c, name_va, cl->name, sizeof(cl->name));
        x2rw_log("[user32] RegisterClassA(\"%s\") wndproc=%08x style=%08x "
                 "-> ATOM %u", cl->name, wndproc, style, cl->atom);
        X2RW_OK();
        return cl->atom;
    }

    x2rw_log("[user32] RegisterClassA: tabela de classes cheia (%d)",
             U32_MAX_CLASSES);
    X2RW_FAIL(c, X2RW_ERROR_NOT_ENOUGH_MEMORY);
}

uint32_t u32_UnregisterClassA(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t name_va = U32_A(0);
    u32_class *cl = NULL;

    /* Aceita o nome (string) ou MAKEINTATOM(atom): nesse caso o valor tem
     * o word alto zerado, como as demais APIs que recebem recurso. */
    if (name_va != 0u && (name_va & 0xFFFF0000u) == 0u) {
        cl = u32_find_class_atom(name_va);
    } else {
        char name[64];
        u32_read_str(c, name_va, name, sizeof(name));
        cl = u32_find_class_name(name);
    }

    if (!cl) {
        x2rw_log("[user32] UnregisterClassA(%08x): classe não registrada",
                 name_va);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    x2rw_log("[user32] UnregisterClassA(\"%s\") — ATOM %u liberado",
             cl->name, cl->atom);
    cl->used = 0;
    X2RW_OK();
    return 1u;
}

uint32_t u32_CreateWindowExA(x2rw_ctx *c, uint32_t *args, int nargs) {
    if (nargs < 12) {
        x2rw_log("[user32] CreateWindowExA: nargs=%d (esperado 12)", nargs);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    uint32_t exstyle   = args[0];
    uint32_t classname = args[1];
    uint32_t title_va  = args[2];
    uint32_t style     = args[3];
    int32_t  x         = (int32_t)args[4];
    int32_t  y         = (int32_t)args[5];
    int32_t  w         = (int32_t)args[6];
    int32_t  h         = (int32_t)args[7];
    uint32_t parent    = args[8];
    uint32_t hmenu     = args[9];
    uint32_t hinst     = args[10];
    uint32_t lparam    = args[11];

    char cls[64]   = { 0 };
    char title[160] = { 0 };
    uint32_t atom = 0u;
    u32_class *cl = NULL;

    if (classname != 0u && (classname & 0xFFFF0000u) == 0u) {
        /* classe já registrada, passada por ATOM */
        atom = classname;
        cl = u32_find_class_atom(atom);
        if (!cl)
            x2rw_log("[user32] CreateWindowExA: ATOM %u não registrado",
                     atom);
    } else {
        u32_read_str(c, classname, cls, sizeof(cls));
        cl = u32_find_class_name(cls);
        if (cl) atom = cl->atom;
        else
            x2rw_log("[user32] CreateWindowExA: classe \"%s\" não registrada "
                     "(wndproc não será conhecido)", cls);
    }
    u32_read_str(c, title_va, title, sizeof(title));

    u32_obj *o = u32_alloc_obj(U32_KIND_WND);
    if (!o)
        X2RW_FAIL(c, X2RW_ERROR_NOT_ENOUGH_MEMORY);

    o->parent = parent;
    o->class_atom = atom;
    o->style = style;
    o->exstyle = exstyle;
    o->wndproc = cl ? cl->wndproc : 0u;
    o->hinstance = hinst;
    o->hmenu = hmenu;
    o->create_lparam = lparam;
    o->userdata = 0u;                 /* Windows: GWLP_USERDATA começa 0 */
    o->x = x; o->y = y; o->w = w; o->h = h;
    o->visible = (style & U32_WS_VISIBLE) ? 1 : 0;

    if (parent == 0u && g_main_hwnd == 0u)
        g_main_hwnd = o->handle;

    x2rw_log("[user32] CreateWindowExA classe=\"%s\" (ATOM %u) título=\"%s\" "
             "%dx%d em (%d,%d) style=%08x exstyle=%08x parent=%08x "
             "-> HWND %08x", cls[0] ? cls : "<atom>", atom, title,
             (int)w, (int)h, (int)x, (int)y, style, exstyle, parent,
             o->handle);
    X2RW_OK();
    return o->handle;
}

uint32_t u32_DestroyWindow(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    uint32_t hwnd = U32_A(0);
    u32_obj *o = u32_find_kind(hwnd, U32_KIND_WND);
    if (!o) {
        x2rw_log("[user32] DestroyWindow(%08x): janela desconhecida", hwnd);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
    }
    /* libera os DCs da janela */
    for (int i = 0; i < U32_MAX_OBJ; i++) {
        u32_obj *dc = &g_obj[i];
        if (dc->used && dc->kind == U32_KIND_DC && dc->owner == hwnd)
            dc->used = 0;
    }
    x2rw_log("[user32] DestroyWindow(%08x) — janela e DCs liberados", hwnd);
    o->used = 0;
    if (g_main_hwnd == hwnd)   g_main_hwnd = 0u;
    if (g_focus_hwnd == hwnd)  g_focus_hwnd = 0u;
    if (g_active_hwnd == hwnd) g_active_hwnd = 0u;
    X2RW_OK();
    return 1u;
}

uint32_t u32_ShowWindow(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    uint32_t hwnd = U32_A(0);
    uint32_t cmd  = U32_A(1);          /* SW_HIDE = 0 */
    u32_obj *o = u32_wnd_or_log(hwnd, "ShowWindow");
    int32_t prev = o ? o->visible : 1;
    if (o) o->visible = (cmd != 0u) ? 1 : 0;
    x2rw_log("[user32] ShowWindow(%08x, %u) — visível %d -> %d "
             "(nenhuma janela real é mapeada; a tela é do D3D8)",
             hwnd, cmd, (int)prev, o ? (int)o->visible : -1);
    if (o) X2RW_OK();
    return (uint32_t)prev;   /* Windows devolve a visibilidade anterior */
}

uint32_t u32_UpdateWindow(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    uint32_t hwnd = U32_A(0);
    u32_obj *o = u32_wnd_or_log(hwnd, "UpdateWindow");
    x2rw_log("[user32] UpdateWindow(%08x) — nada a repintar via GDI "
             "(o D3D8 é quem apresenta)", hwnd);
    if (!o) return 1u;       /* ação: segue com o valor benigno */
    X2RW_OK();
    return 1u;
}

uint32_t u32_MoveWindow(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    uint32_t hwnd = U32_A(0);
    int32_t x = (int32_t)U32_A(1), y = (int32_t)U32_A(2);
    int32_t w = (int32_t)U32_A(3), h = (int32_t)U32_A(4);
    uint32_t repaint = U32_A(5);
    u32_obj *o = u32_wnd_or_log(hwnd, "MoveWindow");
    if (!o) return 1u;       /* ação: segue com o valor benigno */
    x2rw_log("[user32] MoveWindow(%08x, %d, %d, %dx%d, repaint=%u): "
             "(%d,%d,%dx%d) -> (%d,%d,%dx%d)", hwnd, (int)x, (int)y,
             (int)w, (int)h, repaint, (int)o->x, (int)o->y, (int)o->w,
             (int)o->h, (int)x, (int)y, (int)w, (int)h);
    o->x = x; o->y = y; o->w = w; o->h = h;
    X2RW_OK();
    return 1u;
}

uint32_t u32_AdjustWindowRect(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t lp    = U32_A(0);
    uint32_t style = U32_A(1);
    int32_t  menu  = (U32_A(2) != 0u);
    int32_t r[4];

    if (!u32_read_rect(c, lp, r)) {
        x2rw_log("[user32] AdjustWindowRect: LPRECT nulo");
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    /* Métricas do tema clássico do Windows (o jogo é de 2003 e não usa
     * temas). O RECT recebido é a ÁREA CLIENTE; devolvemos a janela
     * inteira, como a API real. */
    int32_t frame = (style & U32_WS_THICKFRAME) ? 4
                  : ((style & U32_WS_BORDER) ? 1 : 0);
    int32_t caption = ((style & U32_WS_CAPTION) == U32_WS_CAPTION) ? 19 : 0;
    int32_t menubar = menu ? 19 : 0;

    x2rw_log("[user32] AdjustWindowRect(style=%08x menu=%d) cliente "
             "(%d,%d,%d,%d) + frame=%d caption=%d menu=%d", style,
             (int)menu, (int)r[0], (int)r[1], (int)r[2], (int)r[3],
             (int)frame, (int)caption, (int)menubar);
    r[0] -= frame;
    r[1] -= frame + caption + menubar;
    r[2] += frame;
    r[3] += frame;
    u32_write_rect(c, lp, r[0], r[1], r[2], r[3]);
    X2RW_OK();
    return 1u;
}

uint32_t u32_GetClientRect(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t hwnd = U32_A(0);
    uint32_t lp   = U32_A(1);
    if (!lp) {
        x2rw_log("[user32] GetClientRect: LPRECT nulo");
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    /* A área cliente é a superfície de render corrente (g_render_w/h), que
     * o host/D3D8 mantêm. hwnd 0 é tratado como a tela (tolerância a
     * código que consulta o "desktop"). */
    if (hwnd != 0u && !u32_wnd_or_log(hwnd, "GetClientRect"))
        X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
    u32_write_rect(c, lp, 0, 0, u32_screen_w(), u32_screen_h());
    X2RW_OK();
    return 1u;
}

uint32_t u32_GetWindowRect(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t hwnd = U32_A(0);
    uint32_t lp   = U32_A(1);
    if (!lp) {
        x2rw_log("[user32] GetWindowRect: LPRECT nulo");
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    if (hwnd == 0u) {
        /* sem janela: reporta a tela (mesma tolerância de GetClientRect) */
        u32_write_rect(c, lp, 0, 0, u32_screen_w(), u32_screen_h());
        X2RW_OK();
        return 1u;
    }
    u32_obj *o = u32_wnd_or_log(hwnd, "GetWindowRect");
    if (!o) X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
    u32_write_rect(c, lp, o->x, o->y, o->x + o->w, o->y + o->h);
    X2RW_OK();
    return 1u;
}

uint32_t u32_IsWindowVisible(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    uint32_t hwnd = U32_A(0);
    u32_obj *o = u32_find_kind(hwnd, U32_KIND_WND);
    if (!o) {
        x2rw_log("[user32] IsWindowVisible(%08x): janela desconhecida",
                 hwnd);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
    }
    X2RW_OK();
    return o->visible ? 1u : 0u;
}

uint32_t u32_GetWindowLongA(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    uint32_t hwnd = U32_A(0);
    int32_t  idx  = (int32_t)U32_A(1);
    u32_obj *o = u32_wnd_or_log(hwnd, "GetWindowLongA");
    if (!o) X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);

    /* Nada de SetWindowLongA na superfície importada: GWLP_USERDATA
     * permanece 0 (o valor inicial real do Windows) e é reportado com
     * log para quem for depurar. */
    switch (idx) {
    case U32_GWL_WNDPROC:    return o->wndproc;
    case U32_GWL_STYLE:      return o->style;
    case U32_GWL_EXSTYLE:    return o->exstyle;
    case U32_GWL_HWNDPARENT: return o->parent;
    case U32_GWL_HINSTANCE:  return o->hinstance;
    case U32_GWL_ID:         return 0u;          /* sem SetWindowLongA */
    case U32_GWL_USERDATA:
        x2rw_log("[user32] GetWindowLongA(%08x, GWLP_USERDATA) -> %08x "
                 "(o jogo não importa SetWindowLongA)", hwnd, o->userdata);
        return o->userdata;
    default:
        x2rw_log("[user32] GetWindowLongA(%08x, %d): índice não modelado "
                 "-> 0", hwnd, (int)idx);
        return 0u;
    }
}

uint32_t u32_GetClassLongA(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    uint32_t hwnd = U32_A(0);
    int32_t  idx  = (int32_t)U32_A(1);
    u32_obj *o = u32_wnd_or_log(hwnd, "GetClassLongA");
    if (!o) X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);

    u32_class *cl = u32_find_class_atom(o->class_atom);
    if (!cl) {
        x2rw_log("[user32] GetClassLongA(%08x, %d): janela sem classe "
                 "registrada -> 0", hwnd, (int)idx);
        x2rw_last_error_set(X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }
    switch (idx) {
    case U32_GCL_STYLE:         return cl->style;
    case U32_GCL_WNDPROC:       return cl->wndproc;
    case U32_GCL_HMODULE:       return cl->hinstance;
    case U32_GCL_HICON:         return cl->hicon;
    case U32_GCL_HCURSOR:       return cl->hcursor;
    case U32_GCL_HBRBACKGROUND: return cl->hbrbackground;
    case U32_GCL_CBCLSEXTRA:    return 0u;
    case U32_GCL_CBWNDEXTRA:    return 0u;
    case U32_GCL_MENUNAME:      return 0u;
    default:
        x2rw_log("[user32] GetClassLongA(%08x, %d): índice não modelado "
                 "-> 0", hwnd, (int)idx);
        return 0u;
    }
}

uint32_t u32_GetTopWindow(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    uint32_t hwnd = U32_A(0);
    uint32_t list[U32_MAX_OBJ];
    if (hwnd != 0u && !u32_wnd_or_log(hwnd, "GetTopWindow")) {
        x2rw_last_error_set(X2RW_ERROR_INVALID_HANDLE);
        return 0u;
    }
    int n = u32_collect_windows(hwnd, list, U32_MAX_OBJ);
    return n > 0 ? list[0] : 0u;
}

uint32_t u32_GetWindow(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    uint32_t hwnd = U32_A(0);
    uint32_t cmd  = U32_A(1);
    uint32_t list[U32_MAX_OBJ];
    u32_obj *o = (hwnd != 0u) ? u32_wnd_or_log(hwnd, "GetWindow") : NULL;
    if (hwnd != 0u && !o) {
        x2rw_last_error_set(X2RW_ERROR_INVALID_HANDLE);
        return 0u;
    }

    switch (cmd) {
    case U32_GW_HWNDFIRST: {
        int n = u32_collect_windows(o ? o->parent : 0u, list, U32_MAX_OBJ);
        return n > 0 ? list[0] : 0u;
    }
    case U32_GW_HWNDLAST: {
        int n = u32_collect_windows(o ? o->parent : 0u, list, U32_MAX_OBJ);
        return n > 0 ? list[n - 1] : 0u;
    }
    case U32_GW_HWNDNEXT:
    case U32_GW_HWNDPREV: {
        if (!o) {
            x2rw_log("[user32] GetWindow(0, uCmd=%u): precisa de um HWND",
                     cmd);
            return 0u;
        }
        int n = u32_collect_windows(o->parent, list, U32_MAX_OBJ);
        for (int i = 0; i < n; i++) {
            if (list[i] != hwnd) continue;
            int j = (cmd == U32_GW_HWNDNEXT) ? i + 1 : i - 1;
            return (j >= 0 && j < n) ? list[j] : 0u;
        }
        return 0u;
    }
    case U32_GW_OWNER:
        if (!o) {
            x2rw_log("[user32] GetWindow(0, GW_OWNER): precisa de um HWND");
            return 0u;
        }
        return o->parent;
    case U32_GW_CHILD: {
        int n = u32_collect_windows(hwnd, list, U32_MAX_OBJ);
        return n > 0 ? list[0] : 0u;
    }
    default:
        x2rw_log("[user32] GetWindow(%08x, uCmd=%u): comando não modelado "
                 "-> 0", hwnd, cmd);
        return 0u;
    }
}

uint32_t u32_GetActiveWindow(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c; (void)args; (void)nargs;
    uint32_t h = g_active_hwnd ? g_active_hwnd : g_main_hwnd;
    return h;
}

uint32_t u32_GetFocus(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c; (void)args; (void)nargs;
    /* Devolve o HWND sintético (o jogo usa isto para saber se a janela tem
     * foco antes de ler o DirectInput). */
    return g_focus_hwnd ? g_focus_hwnd : g_main_hwnd;
}

uint32_t u32_SetFocus(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    uint32_t hwnd = U32_A(0);
    uint32_t prev = g_focus_hwnd;
    u32_obj *o = (hwnd != 0u) ? u32_wnd_or_log(hwnd, "SetFocus") : NULL;
    /* SetFocus(NULL) é válido no Windows (remove o foco). */
    g_focus_hwnd = hwnd;
    x2rw_log("[user32] SetFocus(%08x) — foco anterior %08x (sem janela "
             "real: o jogo lê o DirectInput sem esperar WM_SETFOCUS)",
             hwnd, prev);
    if (hwnd == 0u || o) X2RW_OK();
    return prev;
}

uint32_t u32_SetActiveWindow(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    uint32_t hwnd = U32_A(0);
    uint32_t prev = g_active_hwnd;
    u32_obj *o = (hwnd != 0u) ? u32_wnd_or_log(hwnd, "SetActiveWindow") : NULL;
    g_active_hwnd = hwnd;
    x2rw_log("[user32] SetActiveWindow(%08x) — ativa anterior %08x",
             hwnd, prev);
    if (hwnd == 0u || o) X2RW_OK();
    return prev;
}

uint32_t u32_SetForegroundWindow(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    uint32_t hwnd = U32_A(0);
    u32_obj *o = (hwnd != 0u) ? u32_wnd_or_log(hwnd, "SetForegroundWindow")
                              : NULL;
    /* Sem janela real no Android não há como "trazer para a frente"; o
     * jogo ignora o retorno, então seguimos com sucesso depois do log. */
    x2rw_log("[user32] SetForegroundWindow(%08x) — não há janela real "
             "para trazer à frente (Activity do Android é quem manda)",
             hwnd);
    if (hwnd == 0u || o) X2RW_OK();
    return 1u;
}

uint32_t u32_GetWindowThreadProcessId(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t hwnd = U32_A(0);
    uint32_t lpdw = U32_A(1);
    if (hwnd != 0u && !u32_wnd_or_log(hwnd, "GetWindowThreadProcessId"))
        X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
    /* Um processo e uma thread de jogo: a identidade "da janela" é a do
     * próprio port. Reusar os handlers de KERNEL32 (em vez de um número
     * inventado aqui) garante que o que o jogo lê da janela é exatamente o
     * que GetCurrentProcessId/GetCurrentThreadId devolvem. */
    if (lpdw) x2rw_write32(c, lpdw, k32_GetCurrentProcessId(c, NULL, 0));
    X2RW_OK();
    return k32_GetCurrentThreadId(c, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* DC e pintura                                                       */
/* ------------------------------------------------------------------ */

uint32_t u32_GetDC(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    uint32_t hwnd = U32_A(0);
    u32_obj *dc = u32_window_dc(hwnd);
    if (!dc)
        X2RW_FAIL(c, X2RW_ERROR_NOT_ENOUGH_MEMORY);
    X2RW_OK();
    return dc->handle;
}

uint32_t u32_ReleaseDC(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    uint32_t hwnd = U32_A(0);
    uint32_t hdc  = U32_A(1);
    u32_obj *dc = u32_find_kind(hdc, U32_KIND_DC);
    if (!dc || dc->owner != hwnd) {
        x2rw_log("[user32] ReleaseDC(%08x, %08x): DC desconhecido ou de "
                 "outra janela (owner=%08x)", hwnd, hdc,
                 dc ? dc->owner : 0u);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
    }
    /* O objeto é MANTIDO (cache): o jogo costuma pedir GetDC/ReleaseDC a
     * cada frame e recriar o handle esgotaria a tabela. O Windows também
     * devolve o mesmo DC físico nessas chamadas. */
    x2rw_log("[user32] ReleaseDC(%08x, %08x) — DC mantido em cache para "
             "o próximo GetDC", hwnd, hdc);
    X2RW_OK();
    return 1u;
}

uint32_t u32_BeginPaint(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t hwnd = U32_A(0);
    uint32_t lp   = U32_A(1);
    if (!lp) {
        x2rw_log("[user32] BeginPaint: LPPAINTSTRUCT nulo");
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    if (hwnd != 0u && !u32_wnd_or_log(hwnd, "BeginPaint")) {
        x2rw_last_error_set(X2RW_ERROR_INVALID_HANDLE);
        return 0u;
    }
    u32_obj *dc = u32_window_dc(hwnd);
    if (!dc)
        X2RW_FAIL(c, X2RW_ERROR_NOT_ENOUGH_MEMORY);

    /* PAINTSTRUCT (32 bits): hdc(4) fErase(4) rcPaint(16) fRestore(4)
     * fIncUpdate(4) rgbReserved[32] = 64 bytes. */
    uint8_t ps[U32_PAINTSTRUCT_SIZE];
    memset(ps, 0, sizeof(ps));
    x2rw_mem_write(lp, ps, sizeof(ps));
    x2rw_write32(c, lp + 0u, dc->handle);
    x2rw_write32(c, lp + 4u, 1u);            /* fErase = TRUE */
    u32_write_rect(c, lp + 8u, 0, 0, u32_screen_w(), u32_screen_h());

    x2rw_log("[user32] BeginPaint(%08x) -> HDC %08x, rcPaint=(0,0,%d,%d)",
             hwnd, dc->handle, (int)u32_screen_w(), (int)u32_screen_h());
    X2RW_OK();
    return dc->handle;
}

uint32_t u32_EndPaint(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    uint32_t hwnd = U32_A(0);
    uint32_t lp   = U32_A(1);
    x2rw_log("[user32] EndPaint(%08x, %08x) — nada a validar (sem "
             "invalidação real de região)", hwnd, lp);
    X2RW_OK();
    return 1u;
}

/* ------------------------------------------------------------------ */
/* Cursor / ícone / mensagens                                         */
/* ------------------------------------------------------------------ */

uint32_t u32_LoadCursorA(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    uint32_t hinst = U32_A(0);
    uint32_t name  = U32_A(1);
    uint32_t h = u32_resource_handle(U32_KIND_CURSOR, name);
    if (!h)
        X2RW_FAIL(c, X2RW_ERROR_NOT_ENOUGH_MEMORY);
    if ((name & 0xFFFF0000u) == 0u)
        x2rw_log("[user32] LoadCursorA(hinst=%08x, id=%u) -> HCURSOR %08x "
                 "(handle sintético)", hinst, name, h);
    else
        x2rw_log("[user32] LoadCursorA(hinst=%08x, nome=%08x) -> HCURSOR "
                 "%08x", hinst, name, h);
    X2RW_OK();
    return h;
}

uint32_t u32_LoadIconA(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    uint32_t hinst = U32_A(0);
    uint32_t name  = U32_A(1);
    uint32_t h = u32_resource_handle(U32_KIND_ICON, name);
    if (!h)
        X2RW_FAIL(c, X2RW_ERROR_NOT_ENOUGH_MEMORY);
    if ((name & 0xFFFF0000u) == 0u)
        x2rw_log("[user32] LoadIconA(hinst=%08x, id=%u) -> HICON %08x "
                 "(handle sintético)", hinst, name, h);
    else
        x2rw_log("[user32] LoadIconA(hinst=%08x, nome=%08x) -> HICON %08x",
                 hinst, name, h);
    X2RW_OK();
    return h;
}

uint32_t u32_SetCursor(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    uint32_t hcur = U32_A(0);
    uint32_t prev = g_cursor;
    g_cursor = hcur;
    x2rw_log("[user32] SetCursor(%08x) — cursor anterior %08x "
             "(nenhum cursor real é desenhado; o overlay do Android "
             "controla o ponteiro)", hcur, prev);
    X2RW_OK();
    return prev;
}

uint32_t u32_ShowCursor(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    uint32_t show = U32_A(0);
    int32_t prev = g_cursor_show;
    g_cursor_show += show ? 1 : -1;
    if ((prev < 0) != (g_cursor_show < 0))
        x2rw_log("[user32] ShowCursor(%u): visibilidade do cursor mudou "
                 "(%d -> %d) — sem efeito no Android", show, (int)prev,
                 (int)g_cursor_show);
    X2RW_OK();
    return (uint32_t)g_cursor_show;   /* Windows devolve o contador */
}

uint32_t u32_MessageBoxA(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t hwnd = U32_A(0);
    uint32_t text_va = U32_A(1);
    uint32_t cap_va  = U32_A(2);
    uint32_t type    = U32_A(3);
    char text[256];
    char caption[128];
    u32_read_str(c, text_va, text, sizeof(text));
    u32_read_str(c, cap_va, caption, sizeof(caption));

    /* NUNCA abre diálogo: um MessageBox modal no Android trava o app
     * (não há loop de mensagens para fechá-lo, e a janela do jogo não
     * existe). Registra o que o jogo queria mostrar e devolve IDOK. */
    x2rw_log("[user32] MessageBoxA(hwnd=%08x, type=%08x): título=\"%s\" "
             "texto=\"%s\" — nenhum diálogo aberto (modal travaria o "
             "APK); devolvendo IDOK", hwnd, type, caption, text);
    X2RW_OK();
    return 1u;   /* IDOK */
}

uint32_t u32_PeekMessageA(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    (void)args; (void)nargs;
    static uint32_t calls = 0;
    calls++;
    /* Não há fila de mensagens: nem o host nem o jogo postam WM_*, e o
     * DirectInput8 é pollado diretamente. Devolver 0 é o contrato de
     * "nenhuma mensagem"; o jogo segue o corpo do loop (render + input)
     * sem bloquear. Log espaçado para não inundar a saída a 60 fps. */
    if (calls == 1u || (calls % 100000u) == 0u)
        x2rw_log("[user32] PeekMessageA: sem fila de mensagens "
                 "(chamada %u) — loop não bloqueante, entrada via "
                 "DirectInput8", calls);
    X2RW_OK();
    return 0u;
}

uint32_t u32_TranslateMessage(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    (void)args; (void)nargs;
    static uint32_t calls = 0;
    calls++;
    if (calls == 1u || (calls % 100000u) == 0u)
        x2rw_log("[user32] TranslateMessage: sem mensagens para traduzir "
                 "(chamada %u)", calls);
    X2RW_OK();
    return 0u;   /* nenhum WM_CHAR foi postado */
}

uint32_t u32_DispatchMessageA(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    (void)args; (void)nargs;
    static uint32_t calls = 0;
    calls++;
    /* Sem fila, nada é despachado para o WndProc. Chamar o wndproc do jogo
     * exigiria montar uma MSG e a ABI de retorno — não é preciso: o jogo
     * nunca recebe WM_PAINT/WM_SIZE/WM_CLOSE neste port. */
    if (calls == 1u || (calls % 100000u) == 0u)
        x2rw_log("[user32] DispatchMessageA: nenhuma mensagem para "
                 "despachar (chamada %u) -> 0", calls);
    X2RW_OK();
    return 0u;
}

uint32_t u32_DefWindowProcA(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    uint32_t hwnd = U32_A(0);
    uint32_t msg  = U32_A(1);
    x2rw_log("[user32] DefWindowProcA(%08x, msg=%04x, wparam=%08x, "
             "lparam=%08x) -> 0 (tratamento padrão mínimo)", hwnd, msg,
             U32_A(2), U32_A(3));
    X2RW_OK();
    return 0u;
}

/* ------------------------------------------------------------------ */
/* Conversão de coordenadas                                           */
/* ------------------------------------------------------------------ */

/* A janela é modelada sem bordas: a origem do cliente coincide com a
 * origem da janela. O offset devolvido é, portanto, (x,y) da janela. */
uint32_t u32_ClientToScreen(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t hwnd = U32_A(0);
    uint32_t lp   = U32_A(1);
    int32_t p[2];
    if (!u32_read_point(c, lp, p)) {
        x2rw_log("[user32] ClientToScreen: LPPOINT nulo");
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    int32_t ox = 0, oy = 0;
    if (hwnd != 0u) {
        u32_obj *o = u32_wnd_or_log(hwnd, "ClientToScreen");
        if (!o) X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
        ox = o->x; oy = o->y;
    }
    u32_write_point(c, lp, p[0] + ox, p[1] + oy);
    X2RW_OK();
    return 1u;
}

uint32_t u32_ScreenToClient(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t hwnd = U32_A(0);
    uint32_t lp   = U32_A(1);
    int32_t p[2];
    if (!u32_read_point(c, lp, p)) {
        x2rw_log("[user32] ScreenToClient: LPPOINT nulo");
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    int32_t ox = 0, oy = 0;
    if (hwnd != 0u) {
        u32_obj *o = u32_wnd_or_log(hwnd, "ScreenToClient");
        if (!o) X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
        ox = o->x; oy = o->y;
    }
    u32_write_point(c, lp, p[0] - ox, p[1] - oy);
    X2RW_OK();
    return 1u;
}

uint32_t u32_GetCursorPos(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t lp = U32_A(0);
    static int warned = 0;
    if (!lp) {
        x2rw_log("[user32] GetCursorPos: LPPOINT nulo");
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    /* O port não rastreia posição ABSOLUTA de ponteiro: o overlay do
     * Android entrega deslocamento relativo (x2rw_mouse_dx/dy) para o
     * DirectInput, e consumi-lo aqui roubaria o movimento do jogo.
     * Reportamos o centro da superfície, o que é plausível para a
     * inicialização (o jogo só usa GetCursorPos para posicionar menus). */
    int32_t cx = u32_screen_w() / 2;
    int32_t cy = u32_screen_h() / 2;
    u32_write_point(c, lp, cx, cy);
    if (!warned) {
        warned = 1;
        x2rw_log("[user32] GetCursorPos -> centro (%d,%d): posição "
                 "absoluta do ponteiro não é rastreada neste port",
                 (int)cx, (int)cy);
    }
    X2RW_OK();
    return 1u;
}

/* ------------------------------------------------------------------ */
/* Métricas de tela                                                   */
/* ------------------------------------------------------------------ */

uint32_t u32_GetSystemMetrics(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    int32_t idx = (int32_t)U32_A(0);
    int32_t w = u32_screen_w();
    int32_t h = u32_screen_h();
    int32_t v = 0;

    /* Mapeamento usado pelo port (o jogo de 2003 consulta só o tamanho da
     * tela/cliente). Alguns índices NÃO seguem o nome clássico do Win32 —
     * 4/5 devolve o cliente (e não SM_CYCAPTION/SM_CYBORDER) e 78/79
     * devolve o ícone (e não SM_CXSMSIZE/SM_CYSMSIZE) porque é assim que
     * esta tabela foi definida para o port. */
    switch (idx) {
    case 0:  case 4:  case 16:              /* tela / cliente / tela cheia   */
        v = w;
        break;
    case 1:  case 5:  case 17:
        v = h;
        break;
    case 2:  case 3:                        /* borda                         */
    case 32: case 33:                       /* quadro redimensionável mín.   */
        v = 4;
        break;
    case 78: case 79:                       /* ícone                         */
        v = 32;
        break;
    default:
        x2rw_log("[user32] GetSystemMetrics(%d): métrica não modelada -> 0",
                 (int)idx);
        return 0u;
    }
    X2RW_OK();
    return (uint32_t)v;
}

/* ------------------------------------------------------------------ */
/* Exibição                                                           */
/* ------------------------------------------------------------------ */

uint32_t u32_ChangeDisplaySettingsA(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    uint32_t devmode = U32_A(0);
    uint32_t flags   = U32_A(1);
    /* A janela é sempre a superfície do Android: não mudamos nenhuma
     * resolução de sistema (nem poderíamos). 0 = DISP_CHANGE_SUCCESSFUL,
     * ou seja, SUCESSO — não confundir com falha ao ler o retorno. */
    x2rw_log("[user32] ChangeDisplaySettingsA(devmode=%08x, flags=%08x) -> "
             "DISP_CHANGE_SUCCESSFUL (modo não alterado: a superfície é a "
             "do Android, %dx%d)", devmode, flags, (int)u32_screen_w(),
             (int)u32_screen_h());
    X2RW_OK();
    return 0u;
}
