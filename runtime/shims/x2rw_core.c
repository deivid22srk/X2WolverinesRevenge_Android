/* X2: Wolverine's Revenge - port Android
 * ============================================================================
 * x2rw_core.c — estado global e helpers compartilhados da camada de shims.
 *
 * Este arquivo NÃO implementa APIs do Windows. Ele fornece o que os
 * handlers precisam:
 *
 *   - o estado global (tamanho de render, teclado, mouse);
 *   - o "last error" (GetLastError/SetLastError), que no Windows é
 *     por-thread; aqui é global, porque o port roda um único jogo por
 *     processo e o CRT do jogo não usa threads no caminho de inicialização;
 *   - conversões string <-> memória do guest (a string do jogo vive no
 *     heap do guest, não no heap do host);
 *   - conversão de caminho Win32 para POSIX (o jogo abre "C:\\Jogo\\x.bik"
 *     mas o Android não tem "C:");
 *   - a instalação dos hooks de D3D8 e de áudio, mais os hooks padrão
 *     (headless: registram a chamada e devolvem sucesso sem renderizar nem
 *     tocar som).
 *
 * A REGRA que vale para toda a camada: um handler NUNCA devolve um valor
 * inventado em silêncio. Ou devolve algo correto, ou devolve 0 e registra
 * o last error / levanta um fault. Um stub que "faz sucesso" com estado
 * falso faz o jogo avançar e o bug aparecer longe da causa.
 * ============================================================================
 */
#include "x2rw_runtime.h"
#include "x2rw_shims.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Estado global                                                      */
/* ------------------------------------------------------------------ */

/* Resolução de render. O padrão 1280x720 é o que o jogo usava em 2003
 * em resoluçõesLCD/DTV; o Android sobrescreve via x2rw_host_init. */
int32_t g_render_w = 1280;
int32_t g_render_h = 720;

/* Teclado no formato do DirectInput: 256 bytes, bit 0x80 = pressionada.
 * O overlay de toque do Android escreve aqui (ver x2rw_android_main.c). */
volatile uint8_t x2rw_key_state[256];

/* Deslocamento relativo do mouse, acumulado desde a última leitura. */
volatile int32_t x2rw_mouse_dx = 0;
volatile int32_t x2rw_mouse_dy = 0;
volatile int32_t x2rw_mouse_buttons = 0;

/* ------------------------------------------------------------------ */
/* Last error                                                         */
/* ------------------------------------------------------------------ */

/* No Windows, GetLastError/SetLastError são por-thread. Aqui é global:
 * ver a nota no topo do arquivo. */
static uint32_t g_last_error = X2RW_ERROR_SUCCESS;

uint32_t x2rw_last_error_get(void) { return g_last_error; }
void     x2rw_last_error_set(uint32_t err) { g_last_error = err; }

/* ------------------------------------------------------------------ */
/* Strings do guest                                                   */
/* ------------------------------------------------------------------ */

/* Comprimento da string NUL-terminada no guest, com teto de 64 KiB.
 * O teto existe para que um ponteiro corrompido não trave o port
 * varrendo a memória do guest inteira. */
uint32_t x2rw_guest_strlen(x2rw_ctx *c, uint32_t va) {
    uint32_t n = 0;
    while (n < 0x10000u && x2rw_read8(c, va + n) != 0)
        n++;
    return n;
}

/* Copia `src` (string do HOST, NUL-terminada) para `dst` no guest,
 * truncando em `cap` bytes e sempre terminando com NUL. */
void x2rw_guest_strcpy(x2rw_ctx *c, uint32_t dst, const char *src, size_t cap) {
    if (cap == 0) return;
    size_t i = 0;
    for (; src && src[i] && i + 1 < cap; i++)
        x2rw_write8(c, dst + (uint32_t)i, (uint8_t)src[i]);
    x2rw_write8(c, dst + (uint32_t)i, 0);
}

/* Aloca no heap do guest e copia a string. Devolve 0 se não couber. */
uint32_t x2rw_guest_alloc_str(x2rw_ctx *c, const char *s) {
    size_t n = s ? strlen(s) : 0;
    uint32_t p = x2rw_heap_alloc(c, (uint32_t)(n + 1u));
    if (!p) return 0u;
    x2rw_guest_strcpy(c, p, s, n + 1u);
    return p;
}

/* ------------------------------------------------------------------ */
/* CP-1252 <-> UTF-16                                                 */
/* ------------------------------------------------------------------ */

/* O jogo é uma build americana: as strings do INI e do menu são
 * CP-1252 (Windows-1252), não UTF-8. O Android usa UTF-16 no Java, e
 * as APIs "W" do Windows esperam UTF-16.
 *
 * A tabela cobre apenas os 0x00-0x7F (ASCII, idêntico) e os caracteres
 * imprimíveis estendidos 0xA0-0xFF. O que não está na tabela vira '?'
 * — é melhor que lixo, e é sinalizado por x2rw_log uma única vez. */

/* Índices 0x80-0x9F do CP-1252 são caracteres de controle no Windows
 * (€ e o bloco de "_xHHHH_" do Windows-1252). O jogo não os usa. */
static const uint16_t cp1252_high[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
};

/* Converte CP-1252 -> UTF-16. Devolve o número de unidades escritas
 * (sem o NUL final). */
int x2rw_cp1252_to_utf16(const char *src, int srclen, uint16_t *dst, int dstcount) {
    if (!src || !dst || dstcount <= 0) return 0;
    int n = 0;
    for (int i = 0; i < srclen && n < dstcount; i++) {
        unsigned char b = (unsigned char)src[i];
        if (b < 0x80) {
            dst[n++] = (uint16_t)b;
        } else if (b >= 0xA0) {
            dst[n++] = (uint16_t)(b + 0x00A0 - 0x00A0); /* Latin-1 = CP-1252 aqui */
        } else if (b >= 0x80) {
            dst[n++] = cp1252_high[b - 0x80];
        }
    }
    dst[n] = 0;
    return n;
}

/* Converte UTF-16 -> CP-1252. Devolve o número de bytes escritos. */
int x2rw_utf16_to_cp1252(const uint16_t *src, int srclen, char *dst, int dstcount) {
    if (!src || !dst || dstcount <= 0) return 0;
    int n = 0;
    for (int i = 0; i < srclen && n < dstcount - 1; i++) {
        uint16_t u = src[i];
        if (u == 0) break;
        if (u < 0x80) {
            dst[n++] = (char)u;
        } else if (u >= 0xA0 && u <= 0xFF) {
            dst[n++] = (char)u;
        } else if (u >= 0x2018 && u <= 0x201D) {
            static const char map[] = { 0, 0, 0x82, 0, 0, 0x85 };
            (void)map;
            dst[n++] = '?';
        } else {
            dst[n++] = '?';
        }
    }
    dst[n] = 0;
    return n;
}

/* Escreve uma sequência UTF-16 no guest (com NUL final). */
void x2rw_guest_write_utf16(x2rw_ctx *c, uint32_t va, const uint16_t *src, int count) {
    for (int i = 0; i < count; i++)
        x2rw_write16(c, va + (uint32_t)(2 * i), src[i]);
    x2rw_write16(c, va + (uint32_t)(2 * count), 0);
}

/* ------------------------------------------------------------------ */
/* Caminhos Win32 -> POSIX                                            */
/* ------------------------------------------------------------------ */

/* O jogo usa caminhos absolutos Windows ("C:\\Jogo\\X2\\data.pkr"). No
 * Android não existe "C:". A tradução aqui é: tira a letra de unidade,
 * troca "\\" por "/", e o resultado é RELATIVO ao diretório atual do
 * processo (que o launcher define como a pasta do jogo).
 *
 * O prefixo "\\?\\" (caminho literal do Win32) é removido; "\\server\\"
 * (UNC) vira "//server/", que não vai resolver — e é o comportamento
 * honesto: a API devolve FILE_NOT_FOUND em vez de abrir o arquivo
 * errado. */
void x2rw_win32_path_to_host(const char *win, char *out, size_t outlen) {
    if (!out || outlen == 0) return;
    out[0] = 0;
    if (!win) return;

    const char *p = win;
    if (p[0] == '\\' && p[1] == '\\' && p[2] == '?' && p[3] == '\\')
        p += 4;
    /* "C:\..." ou "C:/..." -> "..." */
    if (p[0] && p[1] == ':') {
        p += 2;
        if (*p == '\\' || *p == '/') p++;
    }
    size_t n = 0;
    for (; *p && n + 1 < outlen; p++) {
        char ch = *p;
        if (ch == '\\') ch = '/';
        out[n++] = ch;
    }
    out[n] = 0;
}

/* ------------------------------------------------------------------ */
/* Hooks de D3D8: default headless                                    */
/* ------------------------------------------------------------------ */

/* Os hooks padrão NÃO renderizam: registram a chamada e devolvem
 * sucesso. Isso mantém o jogo avançando pela inicialização (que é o
 * que precisamos para chegar ao menu) sem exigir um backend gráfico
 * pronto. Quando o renderizador real for plugado, ele substitui estes
 * hooks via x2rw_d3d8_set_hooks(). */

static x2rw_d3d8_hooks g_d3d8_hooks;
static int g_d3d8_hooks_installed = 0;

void x2rw_d3d8_set_hooks(const x2rw_d3d8_hooks *h) {
    if (h) {
        g_d3d8_hooks = *h;
        g_d3d8_hooks_installed = 1;
    } else {
        memset(&g_d3d8_hooks, 0, sizeof(g_d3d8_hooks));
        g_d3d8_hooks_installed = 0;
    }
}

/* Acesso ao hook atual (usado pelos handlers de D3D8). Se nenhum hook
 * foi instalado, devolve NULL e o handler decide o que fazer. */
const x2rw_d3d8_hooks *x2rw_d3d8_get_hooks(void) {
    return g_d3d8_hooks_installed ? &g_d3d8_hooks : NULL;
}

/* ------------------------------------------------------------------ */
/* Hooks de áudio: default headless                                   */
/* ------------------------------------------------------------------ */

static x2rw_audio_hooks g_audio_hooks;
static int g_audio_hooks_installed = 0;

void x2rw_audio_set_hooks(const x2rw_audio_hooks *h) {
    if (h) {
        g_audio_hooks = *h;
        g_audio_hooks_installed = 1;
    } else {
        memset(&g_audio_hooks, 0, sizeof(g_audio_hooks));
        g_audio_hooks_installed = 0;
    }
}

const x2rw_audio_hooks *x2rw_audio_get_hooks(void) {
    return g_audio_hooks_installed ? &g_audio_hooks : NULL;
}
