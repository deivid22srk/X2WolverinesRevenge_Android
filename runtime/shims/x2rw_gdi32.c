/* X2: Wolverine's Revenge - port Android
 * ============================================================================
 * x2rw_gdi32.c — shims de GDI32.dll (13 símbolos da lista mestra).
 *
 * PAPEL DESTE ARQUIVO
 * -------------------
 * O jogo desenha por Direct3D 8. O GDI aqui cobre o pouco que o jogo usa
 * fora do D3D8 (o binkw32.dll também importa GDI32 para uns poucos objetos):
 * DCs de memória, superfícies de bitmap na MEMÓRIA DO GUEST, objetos de
 * estoque (brush/pen/font/palette), consulta de capacidades do dispositivo
 * e um blit de software entre as superfícies que ESTE módulo criou.
 *
 * SUPERFÍCIES
 * -----------
 * CreateDIBSection/CreateCompatibleBitmap alocam os pixels com
 * x2rw_heap_alloc() e o ponteiro devolvido em *ppvBits é um VA do guest —
 * nunca um ponteiro do host. Por isso GetPixel/SetPixel/StretchBlt/GetDIBits
 * conseguem operar de verdade sobre essas superfícies com
 * x2rw_read32/x2rw_write32. O que o jogo desenha no back buffer do D3D8 não
 * passa por aqui.
 *
 * LIMITES HONESTOS (o nível declarado é STUB/PARCIAL)
 * ---------------------------------------------------
 *   - CreateCompatibleBitmap não copia o conteúdo do DC de origem (não há
 *     framebuffer GDI): aloca a superfície zerada e registra;
 *   - formatos com paleta (1/4/8 bpp) existem como memória, mas GetPixel/
 *     SetPixel/StretchBlt só cobrem 32 bpp (o resto devolve CLR_INVALID ou
 *     não copia, sempre com log);
 *   - StretchBlt cobre SRCCOPY 32 bpp com escala nearest-neighbor; outros
 *     ROPs/formato registram "nada copiado" e devolvem 1 (o jogo não checa
 *     o retorno e a renderização real é do D3D8);
 *   - GetDeviceCaps devolve um conjunto fixo e plausível de capacidades
 *     (96 dpi, 32 bpp, 1 plano), coerente com a superfície do port.
 *
 * Handles são sintéticos e estáveis: bitmaps/DC a partir de 0x4000 (tabela
 * estática) e objetos de estoque em 0x5000+índice, como manda a orientação
 * do port. O contador não recicla handles liberados.
 * ============================================================================
 */
#include "x2rw_runtime.h"
#include "x2rw_shims.h"

/* ------------------------------------------------------------------ */
/* Constantes GDI (o port não inclui windows.h)                        */
/* ------------------------------------------------------------------ */

#define GDI_SRCCOPY           0x00CC0020u
#define GDI_CLR_INVALID       0xFFFFFFFFu
#define GDI_MAX_SURFACE_BYTES (64u * 1024u * 1024u)

#define GDI_MAX_OBJ      64
#define GDI_MAX_DCSEL    32
#define GDI_FIRST_HANDLE 0x4000u
#define GDI_STOCK_BASE   0x5000u
#define GDI_STOCK_MAX    20       /* índices 0..19 (DC_BRUSH=18, DC_PEN=19) */

/* Leitura segura do i-ésimo argumento (args[i] só vale para i < nargs). */
#define GDI_A(i) (((i) < nargs) ? args[i] : 0u)

/* ------------------------------------------------------------------ */
/* Estado                                                             */
/* ------------------------------------------------------------------ */

enum {
    GDI_KIND_FREE = 0,
    GDI_KIND_DC,        /* HDC de memória                            */
    GDI_KIND_BITMAP     /* HBITMAP com superfície (ou não) no guest  */
};

/* Classes de objeto GDI, para SelectObject devolver o objeto ANTERIOR do
 * mesmo tipo (o GDI real tem um slot por tipo em cada DC). */
enum {
    GDI_T_UNKNOWN = 0,
    GDI_T_BITMAP,
    GDI_T_BRUSH,
    GDI_T_PEN,
    GDI_T_FONT,
    GDI_T_PALETTE,
    GDI_T_OTHER,
    GDI_T_DC
};

typedef struct {
    int      used;
    uint32_t kind;
    uint32_t handle;

    /* bitmap */
    int32_t  w, h;
    int32_t  bpp;        /* 8, 16, 24 ou 32                          */
    int32_t  stride;     /* bytes por linha (alinhado em 4)          */
    uint32_t pixels;     /* VA no heap do guest (0 = sem superfície) */
    int32_t  top_down;   /* 1 = linha 0 é o topo (biHeight < 0)      */
    int32_t  dib;        /* 1 = criado por CreateDIBSection          */
} gdi_obj;

typedef struct {
    int      used;
    uint32_t hdc;
    uint32_t bitmap, brush, pen, font, palette, other;
    int32_t  stretch_mode;
} gdi_dcsel;

static gdi_obj   g_obj[GDI_MAX_OBJ];
static gdi_dcsel g_sel[GDI_MAX_DCSEL];
static uint32_t  g_next_handle = GDI_FIRST_HANDLE;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static gdi_obj *gdi_alloc_obj(uint32_t kind) {
    for (int i = 0; i < GDI_MAX_OBJ; i++) {
        gdi_obj *o = &g_obj[i];
        if (o->used) continue;
        memset(o, 0, sizeof(*o));
        o->used = 1;
        o->kind = kind;
        o->handle = g_next_handle++;
        return o;
    }
    x2rw_log("[gdi32] tabela de objetos cheia (%d): handle não criado "
             "(kind=%u)", GDI_MAX_OBJ, kind);
    return NULL;
}

static gdi_obj *gdi_find(uint32_t h) {
    if (h == 0u) return NULL;
    for (int i = 0; i < GDI_MAX_OBJ; i++)
        if (g_obj[i].used && g_obj[i].handle == h)
            return &g_obj[i];
    return NULL;
}

static gdi_obj *gdi_find_kind(uint32_t h, uint32_t kind) {
    gdi_obj *o = gdi_find(h);
    return (o && o->kind == kind) ? o : NULL;
}

/* Entrada de "objetos selecionados" de um DC. `create` cria na primeira
 * SelectObject/GetPixel (qualquer HDC, inclusive os de x2rw_user32.c). */
static gdi_dcsel *gdi_sel_get(uint32_t hdc, int create) {
    for (int i = 0; i < GDI_MAX_DCSEL; i++)
        if (g_sel[i].used && g_sel[i].hdc == hdc)
            return &g_sel[i];
    if (!create) return NULL;
    for (int i = 0; i < GDI_MAX_DCSEL; i++) {
        if (g_sel[i].used) continue;
        memset(&g_sel[i], 0, sizeof(g_sel[i]));
        g_sel[i].used = 1;
        g_sel[i].hdc = hdc;
        g_sel[i].stretch_mode = 1;   /* COLORONCOLOR, default do Windows */
        return &g_sel[i];
    }
    x2rw_log("[gdi32] tabela de seleção por DC cheia (%d)", GDI_MAX_DCSEL);
    return NULL;
}

/* Bitmap selecionado no DC (0 se nenhum). */
static gdi_obj *gdi_selected_bitmap(uint32_t hdc) {
    gdi_dcsel *s = gdi_sel_get(hdc, 0);
    if (!s || s->bitmap == 0u) return NULL;
    return gdi_find_kind(s->bitmap, GDI_KIND_BITMAP);
}

/* Classifica um handle para SelectObject. Objetos de estoque são
 * identificados pela faixa 0x5000+índice (ver GetStockObject). */
static uint32_t gdi_type_of(uint32_t h) {
    if (h >= GDI_STOCK_BASE && h < GDI_STOCK_BASE + GDI_STOCK_MAX) {
        uint32_t i = h - GDI_STOCK_BASE;
        if (i <= 5u) return GDI_T_BRUSH;                  /* 0..5  brushes */
        if (i <= 8u) return GDI_T_PEN;                    /* 6..8  pens    */
        if (i == 15u) return GDI_T_PALETTE;               /* 15    palette */
        if (i == 18u) return GDI_T_BRUSH;                 /* DC_BRUSH      */
        if (i == 19u) return GDI_T_PEN;                   /* DC_PEN        */
        if (i >= 10u && i <= 17u) return GDI_T_FONT;      /* 10..17 fontes */
        return GDI_T_OTHER;
    }
    gdi_obj *o = gdi_find(h);
    if (o) return (o->kind == GDI_KIND_DC) ? GDI_T_DC : GDI_T_BITMAP;
    return GDI_T_UNKNOWN;
}

/* VA da linha FÍSICA correspondente à linha lógica y (0 = topo), levando em
 * conta a orientação DIB da superfície. */
static uint32_t gdi_row_va(const gdi_obj *o, int32_t y) {
    int32_t row = o->top_down ? y : (o->h - 1 - y);
    return o->pixels + (uint32_t)(row * o->stride);
}

/* Cópia guest->guest em blocos (não existe memcpy entre dois VAs). */
static void gdi_copy_guest(uint32_t dst, uint32_t src, uint32_t n) {
    uint8_t buf[4096];
    while (n > 0) {
        uint32_t k = n > (uint32_t)sizeof(buf) ? (uint32_t)sizeof(buf) : n;
        x2rw_mem_read(src, buf, k);
        x2rw_mem_write(dst, buf, k);
        src += k;
        dst += k;
        n -= k;
    }
}

/* Aloca a superfície de um bitmap no heap do guest. Devolve 0 em falha. */
static uint32_t gdi_alloc_surface(x2rw_ctx *c, int32_t w, int32_t h,
                                  int32_t bpp, int32_t *out_stride) {
    int64_t stride = (((int64_t)w * bpp + 31) / 32) * 4;
    int64_t bytes  = stride * h;
    if (stride <= 0 || bytes <= 0 || bytes > (int64_t)GDI_MAX_SURFACE_BYTES) {
        x2rw_log("[gdi32] superfície %dx%d @ %d bpp fora do limite "
                 "(stride=%d bytes=%d, teto=%u)", (int)w, (int)h, (int)bpp,
                 (int)stride, (int)bytes, (unsigned)GDI_MAX_SURFACE_BYTES);
        return 0u;
    }
    uint32_t px = x2rw_heap_alloc(c, (uint32_t)bytes);
    if (!px) {
        x2rw_log("[gdi32] heap do guest esgotado ao alocar %dx%d @ %d bpp",
                 (int)w, (int)h, (int)bpp);
        return 0u;
    }
    if (out_stride) *out_stride = (int32_t)stride;
    return px;
}

/* ------------------------------------------------------------------ */
/* DCs                                                                */
/* ------------------------------------------------------------------ */

uint32_t gdi_CreateCompatibleDC(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    uint32_t src = GDI_A(0);
    gdi_obj *o = gdi_alloc_obj(GDI_KIND_DC);
    if (!o)
        X2RW_FAIL(c, X2RW_ERROR_NOT_ENOUGH_MEMORY);
    x2rw_log("[gdi32] CreateCompatibleDC(%08x) -> HDC %08x (DC de memória; "
             "a superfície de pixel vem de CreateDIBSection/"
             "CreateCompatibleBitmap)", src, o->handle);
    X2RW_OK();
    return o->handle;
}

uint32_t gdi_DeleteDC(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    uint32_t hdc = GDI_A(0);
    gdi_obj *o = gdi_find_kind(hdc, GDI_KIND_DC);
    if (!o) {
        x2rw_log("[gdi32] DeleteDC(%08x): DC desconhecido", hdc);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
    }
    gdi_dcsel *s = gdi_sel_get(hdc, 0);
    if (s) s->used = 0;          /* esquece a seleção deste DC */
    o->used = 0;
    x2rw_log("[gdi32] DeleteDC(%08x) — DC liberado", hdc);
    X2RW_OK();
    return 1u;
}

/* ------------------------------------------------------------------ */
/* Bitmaps                                                            */
/* ------------------------------------------------------------------ */

uint32_t gdi_CreateCompatibleBitmap(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t hdc = GDI_A(0);
    int32_t  w   = (int32_t)GDI_A(1);
    int32_t  h   = (int32_t)GDI_A(2);

    if (w <= 0 || h <= 0) {
        x2rw_log("[gdi32] CreateCompatibleBitmap(%08x, %d, %d): dimensões "
                 "inválidas", hdc, (int)w, (int)h);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    gdi_obj *b = gdi_alloc_obj(GDI_KIND_BITMAP);
    if (!b)
        X2RW_FAIL(c, X2RW_ERROR_NOT_ENOUGH_MEMORY);

    b->w = w;
    b->h = h;
    b->bpp = 32;
    b->top_down = 1;                     /* nosso padrão: linha 0 = topo */
    b->dib = 0;
    b->pixels = gdi_alloc_surface(c, w, h, 32, &b->stride);
    if (!b->pixels) {
        b->used = 0;
        X2RW_FAIL(c, X2RW_ERROR_NOT_ENOUGH_MEMORY);
    }
    /* Não existe framebuffer GDI para copiar do DC de origem: a superfície
     * começa zerada (é o comportamento honesto — nenhum conteúdo é
     * inventado a partir do back buffer do D3D8). */
    x2rw_log("[gdi32] CreateCompatibleBitmap(%08x, %dx%d) -> HBITMAP %08x "
             "(32 bpp, %u bytes no heap do guest, zerado)", hdc, (int)w,
             (int)h, b->handle, (unsigned)((uint32_t)b->stride *
             (uint32_t)b->h));
    X2RW_OK();
    return b->handle;
}

uint32_t gdi_CreateDIBSection(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t hdc     = GDI_A(0);
    uint32_t pbmi    = GDI_A(1);
    uint32_t usage   = GDI_A(2);
    uint32_t ppvbits = GDI_A(3);
    uint32_t hsection = GDI_A(4);
    uint32_t offset  = GDI_A(5);

    if (!pbmi || !ppvbits) {
        x2rw_log("[gdi32] CreateDIBSection: pbmi=%08x ppvBits=%08x "
                 "(ponteiro de saída é obrigatório)", pbmi, ppvbits);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    /* BITMAPINFOHEADER: biSize(0) biWidth(4) biHeight(8) biPlanes(12)
     * biBitCount(14) biCompression(16) biSizeImage(20) ... */
    int32_t bih_size = (int32_t)x2rw_read32(c, pbmi + 0u);
    int32_t w        = (int32_t)x2rw_read32(c, pbmi + 4u);
    int32_t hraw     = (int32_t)x2rw_read32(c, pbmi + 8u);
    int32_t bpp      = (int32_t)x2rw_read16(c, pbmi + 14u);

    if (bih_size < 40 || w <= 0 || hraw == 0) {
        x2rw_log("[gdi32] CreateDIBSection: cabeçalho inválido "
                 "(biSize=%d, %dx%d) -> falha", (int)bih_size, (int)w,
                 (int)hraw);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    if (bpp == 0) bpp = 32;      /* "como a tela": o port é sempre 32 bpp */
    if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) {
        x2rw_log("[gdi32] CreateDIBSection: %d bpp não modelado (só 8/16/"
                 "24/32) -> falha", (int)bpp);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    if (hsection != 0u)
        x2rw_log("[gdi32] CreateDIBSection: hSection=%08x ignorado (sem "
                 "arquivo mapeado; alocando no heap do guest)", hsection);
    if (offset != 0u)
        x2rw_log("[gdi32] CreateDIBSection: dwOffset=%u ignorado",
                 (unsigned)offset);

    int32_t h = (int32_t)(hraw < 0 ? -(int64_t)hraw : hraw);
    int32_t top_down = hraw < 0;

    gdi_obj *b = gdi_alloc_obj(GDI_KIND_BITMAP);
    if (!b)
        X2RW_FAIL(c, X2RW_ERROR_NOT_ENOUGH_MEMORY);
    b->w = w;
    b->h = h;
    b->bpp = bpp;
    b->top_down = top_down;
    b->dib = 1;
    b->pixels = gdi_alloc_surface(c, w, h, bpp, &b->stride);
    if (!b->pixels) {
        b->used = 0;
        X2RW_FAIL(c, X2RW_ERROR_NOT_ENOUGH_MEMORY);
    }
    /* *ppvBits recebe um VA DO GUEST — é isso que o jogo escreve direto. */
    x2rw_write32(c, ppvbits, b->pixels);

    x2rw_log("[gdi32] CreateDIBSection(hdc=%08x, usage=%08x) %dx%d @ %d bpp "
             "%s -> HBITMAP %08x, bits em %08x (stride %d)", hdc, usage,
             (int)w, (int)h, (int)bpp, top_down ? "top-down" : "bottom-up",
             b->handle, b->pixels, (int)b->stride);
    X2RW_OK();
    return b->handle;
}

uint32_t gdi_DeleteObject(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t h = GDI_A(0);

    if (h >= GDI_STOCK_BASE && h < GDI_STOCK_BASE + GDI_STOCK_MAX) {
        /* O Windows ignora a liberação de um objeto de estoque (é do
         * sistema); devolvemos sucesso sem liberar nada. */
        x2rw_log("[gdi32] DeleteObject(%08x): objeto de estoque — não é "
                 "liberado (pertence ao sistema)", h);
        X2RW_OK();
        return 1u;
    }
    gdi_obj *o = gdi_find(h);
    if (!o) {
        x2rw_log("[gdi32] DeleteObject(%08x): objeto desconhecido", h);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
    }
    if (o->kind == GDI_KIND_DC) {
        x2rw_log("[gdi32] DeleteObject(%08x): é um HDC — use DeleteDC", h);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
    }
    if (o->pixels) {
        x2rw_heap_free(c, o->pixels);      /* marca livre (bump allocator) */
        o->pixels = 0u;
    }
    for (int i = 0; i < GDI_MAX_DCSEL; i++)
        if (g_sel[i].used && g_sel[i].bitmap == h)
            g_sel[i].bitmap = 0u;
    x2rw_log("[gdi32] DeleteObject(%08x) — HBITMAP liberado", h);
    o->used = 0;
    X2RW_OK();
    return 1u;
}

uint32_t gdi_GetDIBits(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t hdc    = GDI_A(0);
    uint32_t hbm    = GDI_A(1);
    int32_t  start  = (int32_t)GDI_A(2);
    int32_t  lines  = (int32_t)GDI_A(3);
    uint32_t lpv    = GDI_A(4);
    uint32_t lpbmi  = GDI_A(5);
    uint32_t usage  = GDI_A(6);

    gdi_obj *b = gdi_find_kind(hbm, GDI_KIND_BITMAP);
    if (!b || !b->pixels) {
        x2rw_log("[gdi32] GetDIBits(hdc=%08x, hbm=%08x): bitmap desconhecido "
                 "ou sem superfície", hdc, hbm);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
    }
    if (!lpbmi) {
        x2rw_log("[gdi32] GetDIBits: lpbi nulo");
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    if (start < 0) start = 0;

    /* Preenche o BITMAPINFOHEADER (40 bytes) com o formato REAL da
     * superfície; a orientação (sinal de biHeight) é a da superfície, para
     * que o chamador interprete as linhas copiadas corretamente. */
    x2rw_write32(c, lpbmi + 0u, 40u);
    x2rw_write32(c, lpbmi + 4u, (uint32_t)b->w);
    x2rw_write32(c, lpbmi + 8u, (uint32_t)(b->top_down ? -b->h : b->h));
    x2rw_write16(c, lpbmi + 12u, 1u);                        /* biPlanes   */
    x2rw_write16(c, lpbmi + 14u, (uint16_t)b->bpp);          /* biBitCount */
    x2rw_write32(c, lpbmi + 16u, 0u);                        /* BI_RGB     */
    x2rw_write32(c, lpbmi + 20u, (uint32_t)(b->stride * b->h));
    x2rw_write32(c, lpbmi + 24u, 0u);                        /* X pels/m   */
    x2rw_write32(c, lpbmi + 28u, 0u);                        /* Y pels/m   */
    x2rw_write32(c, lpbmi + 32u, 0u);                        /* clrUsed    */
    x2rw_write32(c, lpbmi + 36u, 0u);                        /* clrImport. */

    if (!lpv) {
        /* Consulta de formato: o retorno é o número de linhas da imagem. */
        x2rw_log("[gdi32] GetDIBits(hbm=%08x): consulta de formato -> "
                 "%dx%d @ %d bpp, %d linha(s)", hbm, (int)b->w, (int)b->h,
                 (int)b->bpp, (int)b->h);
        X2RW_OK();
        return (uint32_t)b->h;
    }
    if (lines <= 0) {
        x2rw_log("[gdi32] GetDIBits(hbm=%08x): cScanLines=%d <= 0 com "
                 "lpvBits=%08x — nada copiado", hbm, (int)lines, lpv);
        X2RW_OK();
        return 0u;
    }
    int32_t total = b->h - start;
    if (total <= 0) {
        x2rw_log("[gdi32] GetDIBits(hbm=%08x): uStartScan=%d além da altura "
                 "%d -> 0 linhas", hbm, (int)start, (int)b->h);
        X2RW_OK();
        return 0u;
    }
    int32_t n = lines > total ? total : lines;
    uint32_t dst = lpv;
    for (int32_t j = 0; j < n; j++) {
        uint32_t src = b->pixels + (uint32_t)((start + j) * b->stride);
        gdi_copy_guest(dst + (uint32_t)(j * b->stride), src,
                       (uint32_t)b->stride);
    }
    x2rw_log("[gdi32] GetDIBits(hbm=%08x, start=%d, lines=%d, usage=%08x): "
             "copiadas %d linha(s) de %d bytes para %08x", hbm, (int)start,
             (int)lines, usage, (int)n, (int)b->stride, lpv);
    X2RW_OK();
    return (uint32_t)n;
}

/* ------------------------------------------------------------------ */
/* Objetos de estoque e seleção                                       */
/* ------------------------------------------------------------------ */

uint32_t gdi_GetStockObject(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    int32_t idx = (int32_t)GDI_A(0);
    if (idx < 0 || idx >= GDI_STOCK_MAX) {
        x2rw_log("[gdi32] GetStockObject(%d): fora da tabela (0..%d) -> 0",
                 (int)idx, GDI_STOCK_MAX - 1);
        return 0u;
    }
    /* Handles FIXOS por tipo, fora da faixa dos objetos criados: o jogo
     * compara esses valores entre chamadas (ex.: hbrBackground da classe).
     * WHITE_BRUSH=0 ... DEFAULT_GUI_FONT=17 (índices do Win32). */
    uint32_t h = GDI_STOCK_BASE + (uint32_t)idx;
    x2rw_log("[gdi32] GetStockObject(%d) -> %08x (handle fixo de estoque)",
             (int)idx, h);
    X2RW_OK();
    return h;
}

uint32_t gdi_SelectObject(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    uint32_t hdc = GDI_A(0);
    uint32_t h   = GDI_A(1);
    gdi_dcsel *s = gdi_sel_get(hdc, 1);
    if (!s)
        X2RW_FAIL(c, X2RW_ERROR_NOT_ENOUGH_MEMORY);

    uint32_t t = gdi_type_of(h);
    uint32_t prev;
    switch (t) {
    case GDI_T_BITMAP:
        prev = s->bitmap ? s->bitmap : 0u;
        s->bitmap = h;
        break;
    case GDI_T_BRUSH:
        /* Default de um DC novo: pincel preto (como no GDI real). */
        prev = s->brush ? s->brush : (GDI_STOCK_BASE + 4u);
        s->brush = h;
        break;
    case GDI_T_PEN:
        prev = s->pen ? s->pen : (GDI_STOCK_BASE + 7u);
        s->pen = h;
        break;
    case GDI_T_FONT:
        prev = s->font ? s->font : (GDI_STOCK_BASE + 17u);
        s->font = h;
        break;
    case GDI_T_PALETTE:
        prev = s->palette ? s->palette : (GDI_STOCK_BASE + 15u);
        s->palette = h;
        break;
    case GDI_T_OTHER:
        prev = s->other;
        s->other = h;
        break;
    case GDI_T_DC:
        x2rw_log("[gdi32] SelectObject(%08x, %08x): HDC não é selecionável",
                 hdc, h);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    default:
        x2rw_log("[gdi32] SelectObject(%08x, %08x): handle desconhecido",
                 hdc, h);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
    }
    x2rw_log("[gdi32] SelectObject(%08x, %08x) tipo=%u -> anterior %08x",
             hdc, h, t, prev);
    X2RW_OK();
    return prev;
}

/* ------------------------------------------------------------------ */
/* Capacidades do dispositivo                                         */
/* ------------------------------------------------------------------ */

uint32_t gdi_GetDeviceCaps(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    uint32_t hdc = GDI_A(0);
    int32_t  idx = (int32_t)GDI_A(1);
    int32_t  w = g_render_w > 0 ? g_render_w : 1280;
    int32_t  h = g_render_h > 0 ? g_render_h : 720;
    int32_t  v;

    /* Conjunto coerente com a superfície do port (o GDI aqui nunca é o
     * caminho de render). 96 dpi / 32 bpp / 1 plano. */
    switch (idx) {
    case 88:  v = 96;   break;   /* LOGPIXELSX      */
    case 90:  v = 96;   break;   /* LOGPIXELSY      */
    case 8:   v = w;    break;   /* HORZRES         */
    case 10:  v = h;    break;   /* VERTRES         */
    case 12:  v = 32;   break;   /* BITSPIXEL       */
    case 14:  v = 1;    break;   /* PLANES          */
    case 24:  v = -1;   break;   /* NUMCOLORS: -1 = "mais que 256" */
    case 104: v = 0;    break;   /* SIZEPALETTE     */
    case 4:   v = 320;  break;   /* HORZSIZE (mm, aproximado)        */
    case 6:   v = 240;  break;   /* VERTSIZE (mm, aproximado)        */
    default:
        x2rw_log("[gdi32] GetDeviceCaps(hdc=%08x, %d): capacidade não "
                 "modelada -> 0", hdc, (int)idx);
        return 0u;
    }
    X2RW_OK();
    return (uint32_t)v;
}

/* ------------------------------------------------------------------ */
/* Pixels                                                             */
/* ------------------------------------------------------------------ */

/* Devolve o bitmap selecionado com superfície 32 bpp, ou NULL (com log).
 * `api` só entra na mensagem. */
static gdi_obj *gdi_surface32(x2rw_ctx *c, uint32_t hdc, const char *api,
                              int32_t *x, int32_t *y) {
    (void)c;
    static uint32_t fails = 0;   /* log espaçado: SetPixel em laço pode ser quente */
    gdi_obj *b = gdi_selected_bitmap(hdc);
    if (!b || !b->pixels) {
        fails++;
        if (fails <= 4u || (fails % 1024u) == 0u)
            x2rw_log("[gdi32] %s(hdc=%08x): nenhum HBITMAP com superfície "
                     "selecionado (falha %u; a renderização real é do D3D8)",
                     api, hdc, fails);
        x2rw_last_error_set(X2RW_ERROR_INVALID_HANDLE);
        return NULL;
    }
    if (b->bpp != 32) {
        x2rw_log("[gdi32] %s(hdc=%08x): bitmap de %d bpp — paletas não são "
                 "modeladas (só 32 bpp)", api, hdc, (int)b->bpp);
        x2rw_last_error_set(X2RW_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (*x < 0 || *y < 0 || *x >= b->w || *y >= b->h) {
        x2rw_log("[gdi32] %s(hdc=%08x): (%d,%d) fora de %dx%d", api, hdc,
                 (int)*x, (int)*y, (int)b->w, (int)b->h);
        return NULL;
    }
    return b;
}

uint32_t gdi_GetPixel(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t hdc = GDI_A(0);
    int32_t  x   = (int32_t)GDI_A(1);
    int32_t  y   = (int32_t)GDI_A(2);
    gdi_obj *b = gdi_surface32(c, hdc, "GetPixel", &x, &y);
    if (!b) return GDI_CLR_INVALID;      /* CLR_INVALID */
    uint32_t px = x2rw_read32(c, gdi_row_va(b, y) + 4u * (uint32_t)x);
    X2RW_OK();
    return px & 0x00FFFFFFu;
}

uint32_t gdi_SetPixel(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t hdc   = GDI_A(0);
    int32_t  x     = (int32_t)GDI_A(1);
    int32_t  y     = (int32_t)GDI_A(2);
    uint32_t color = GDI_A(3);
    gdi_obj *b = gdi_surface32(c, hdc, "SetPixel", &x, &y);
    if (!b) return GDI_CLR_INVALID;
    x2rw_write32(c, gdi_row_va(b, y) + 4u * (uint32_t)x,
                 color & 0x00FFFFFFu);
    X2RW_OK();
    return color & 0x00FFFFFFu;          /* Windows devolve a cor aplicada */
}

uint32_t gdi_SetStretchBltMode(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    uint32_t hdc  = GDI_A(0);
    int32_t  mode = (int32_t)GDI_A(1);
    if (mode < 1 || mode > 4) {          /* BLACKONWHITE..HALFTONE */
        x2rw_log("[gdi32] SetStretchBltMode(%08x, %d): modo inválido",
                 hdc, (int)mode);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    gdi_dcsel *s = gdi_sel_get(hdc, 1);
    if (!s)
        X2RW_FAIL(c, X2RW_ERROR_NOT_ENOUGH_MEMORY);
    int32_t prev = s->stretch_mode;
    s->stretch_mode = mode;
    x2rw_log("[gdi32] SetStretchBltMode(%08x, %d) -> anterior %d",
             hdc, (int)mode, (int)prev);
    X2RW_OK();
    return (uint32_t)prev;
}

/* ------------------------------------------------------------------ */
/* Blit de software entre superfícies do guest                        */
/* ------------------------------------------------------------------ */

uint32_t gdi_StretchBlt(x2rw_ctx *c, uint32_t *args, int nargs) {
    if (nargs < 11) {
        x2rw_log("[gdi32] StretchBlt: nargs=%d (esperado 11)", nargs);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    uint32_t hdc_dst = args[0];
    int32_t  xd = (int32_t)args[1], yd = (int32_t)args[2];
    int32_t  wd = (int32_t)args[3], hd = (int32_t)args[4];
    uint32_t hdc_src = args[5];
    int32_t  xs = (int32_t)args[6], ys = (int32_t)args[7];
    int32_t  ws = (int32_t)args[8], hs = (int32_t)args[9];
    uint32_t rop = args[10];

    if (wd <= 0 || hd <= 0 || ws <= 0 || hs <= 0) {
        x2rw_log("[gdi32] StretchBlt: dimensões inválidas dst=%dx%d src=%dx%d",
                 (int)wd, (int)hd, (int)ws, (int)hs);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    gdi_obj *d = gdi_selected_bitmap(hdc_dst);
    gdi_obj *s = gdi_selected_bitmap(hdc_src);
    if (!d || !d->pixels || !s || !s->pixels) {
        x2rw_log("[gdi32] StretchBlt(dst=%08x, src=%08x, rop=%08x): destino "
                 "ou origem sem superfície GDI selecionada — NADA copiado "
                 "(o D3D8 é quem desenha; este caminho cobre só os bitmaps "
                 "criados por CreateDIBSection/CreateCompatibleBitmap)",
                 hdc_dst, hdc_src, rop);
        X2RW_OK();
        return 1u;
    }
    if (rop != GDI_SRCCOPY) {
        x2rw_log("[gdi32] StretchBlt(dst=%08x, src=%08x): rop=%08x não "
                 "modelado (só SRCCOPY) — nada copiado", hdc_dst, hdc_src,
                 rop);
        X2RW_OK();
        return 1u;
    }
    if (d->bpp != 32 || s->bpp != 32) {
        x2rw_log("[gdi32] StretchBlt: %d->%d bpp não modelado (só 32->32) — "
                 "nada copiado", (int)s->bpp, (int)d->bpp);
        X2RW_OK();
        return 1u;
    }

    /* Caminho rápido: cópia 1:1 na mesma posição (o caso de
     * SetDIBits/blit de tela inteira). */
    if (xd == xs && yd == ys && wd == ws && hd == hs &&
        d->top_down == s->top_down) {
        int32_t rows = 0;
        for (int32_t j = 0; j < hd; j++) {
            int64_t y_d = (int64_t)yd + j;
            int64_t y_s = (int64_t)ys + j;
            if (y_d < 0 || y_d >= d->h || y_s < 0 || y_s >= s->h) continue;
            int64_t x_d = xd;
            int64_t x_s = xs;
            int64_t n = wd;
            if (x_d < 0) { n += x_d; x_s -= x_d; x_d = 0; }
            if (x_s < 0) { n += x_s; x_d -= x_s; x_s = 0; }
            if (n > d->w - x_d) n = d->w - x_d;
            if (n > s->w - x_s) n = s->w - x_s;
            if (n <= 0) continue;
            gdi_copy_guest(gdi_row_va(d, (int32_t)y_d) + 4u * (uint32_t)x_d,
                           gdi_row_va(s, (int32_t)y_s) + 4u * (uint32_t)x_s,
                           (uint32_t)n * 4u);
            rows++;
        }
        x2rw_log("[gdi32] StretchBlt 1:1 %dx%d de %08x para %08x (%d linha(s))",
                 (int)wd, (int)hd, hdc_src, hdc_dst, (int)rows);
        X2RW_OK();
        return 1u;
    }

    /* Escala nearest-neighbor, pixel a pixel (cobre também offsets). */
    int32_t copied = 0;
    for (int32_t dy = 0; dy < hd; dy++) {
        int64_t y_d = (int64_t)yd + dy;
        if (y_d < 0 || y_d >= d->h) continue;
        int64_t y_s = (int64_t)ys + ((int64_t)dy * hs) / hd;
        if (y_s < 0 || y_s >= s->h) continue;
        uint32_t drow = gdi_row_va(d, (int32_t)y_d);
        uint32_t srow = gdi_row_va(s, (int32_t)y_s);
        for (int32_t dx = 0; dx < wd; dx++) {
            int64_t x_d = (int64_t)xd + dx;
            if (x_d < 0 || x_d >= d->w) continue;
            int64_t x_s = (int64_t)xs + ((int64_t)dx * ws) / wd;
            if (x_s < 0 || x_s >= s->w) continue;
            x2rw_write32(c, drow + 4u * (uint32_t)x_d,
                         x2rw_read32(c, srow + 4u * (uint32_t)x_s));
            copied++;
        }
    }
    x2rw_log("[gdi32] StretchBlt %dx%d -> %dx%d de %08x para %08x: %d "
             "pixel(s) copiados", (int)ws, (int)hs, (int)wd, (int)hd,
             hdc_src, hdc_dst, (int)copied);
    X2RW_OK();
    return 1u;
}
