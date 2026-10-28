/* X2: Wolverine's Revenge - port Android
 * ============================================================================
 * x2rw_strings.c — handlers de STRING/LOCALE do CRT (KERNEL32.dll).
 *
 * Por que este arquivo existe
 * ---------------------------
 * O CRT do MSVC (o que veio linkado em Wolvie.exe, build americana de
 * 04/04/2003) decide o "locale" do programa chamando GetACP/GetOEMCP/
 * GetUserDefaultLCID na inicialização e convertendo as strings de
 * ambiente/arquivo com MultiByteToWideChar. Se qualquer uma dessas
 * devolver um valor incoerente (ex.: codepage 0), o CRT aborta antes de
 * o jogo chegar ao main.
 *
 * O alvo: code page 1252 (Windows-1252 / ANSI americano), OEM 437 e
 * LCID 0x0409 (en-US). Isso está verificado no binário: o jogo carrega a
 * tabela de idiomas com as colunas (LCID, LNG, CTRY, OEMCP, ACP) e a
 * entrada final de inglês é "0409 ENU USA 437 1252" — ver docs/RECON.md.
 *
 * O que é de verdade e o que não é
 * --------------------------------
 *   MultiByteToWideChar / WideCharToMultiByte : REAL (conversão via
 *       x2rw_cp1252_to_utf16 / x2rw_utf16_to_cp1252, do x2rw_core.c);
 *   GetStringTypeA/W                          : REAL (CT_CTYPE1 completo,
 *       por caractere);
 *   CompareStringA/W, LCMapStringA/W          : PARCIAL — respeitam os
 *       flags pedidos, mas o "locale" é sempre o en-US e o mapeamento de
 *       caixa só cobre ASCII (é o que o jogo usa: textos em inglês);
 *   GetLocaleInfoA/W                          : PARCIAL — devolve os
 *       valores do en-US para os LCType que o CRT pergunta; o resto
 *       devolve 0 + ERROR_INVALID_PARAMETER (nunca um valor inventado);
 *   EnumSystemLocalesA                        : NÃO enumera — ver o
 *       comentário em destaque no handler.
 * ============================================================================
 */
#include "x2rw_runtime.h"
#include "x2rw_shims.h"

#include <string.h>
#include <strings.h>    /* strcasecmp (CompareStringA com NORM_IGNORECASE) */

/* ------------------------------------------------------------------ */
/* Constantes Win32 usadas aqui                                       */
/* ------------------------------------------------------------------ */

/* O jogo é build americana: ver docs/RECON.md §12 e a tabela de idiomas
 * dentro de Wolvie.exe ("0409 ENU USA 437 1252"). */
#define X2RW_ACP      1252u     /* ANSI code page do en-US           */
#define X2RW_OEMCP    437u      /* OEM code page do en-US            */
#define X2RW_LCID_EN_US  0x0409u
#define X2RW_LCID_EN_GB  0x0809u

/* Só estas cinco: as que o CRT pergunta. UTF-8 (65001) é aceito porque o
 * jogo/launcher pode pedir; ver o comentário no handler. */
#define X2RW_CP_UTF8  65001u

/* CompareString */
#define X2RW_CSTR_LESS_THAN     1u
#define X2RW_CSTR_EQUAL         2u
#define X2RW_CSTR_GREATER_THAN  3u
#define X2RW_NORM_IGNORECASE    0x00000001u

/* LCMapString */
#define X2RW_LCMAP_LOWERCASE    0x00000100u
#define X2RW_LCMAP_UPPERCASE    0x00000200u
#define X2RW_LCMAP_SORTKEY      0x00000400u

/* GetLocaleInfo — LOCALE_* (a referência é o winnls.h do SDK) */
#define X2RW_LOCALE_ILANGUAGE               0x00000001u
#define X2RW_LOCALE_SDECIMAL                0x0000000Eu
#define X2RW_LOCALE_STHOUSAND               0x0000000Fu
#define X2RW_LOCALE_SDATE                   0x0000001Du
#define X2RW_LOCALE_STIMEFORMAT             0x00001003u
#define X2RW_LOCALE_IDEFAULTANSICODEPAGE    0x00001004u
#define X2RW_LOCALE_SENGLANGUAGE            0x00001001u
#define X2RW_LOCALE_SENGCOUNTRY             0x00001002u
#define X2RW_LOCALE_SABBREVLANGNAME         0x00000003u
#define X2RW_LOCALE_SABBREVCTRYNAME         0x00000007u
#define X2RW_LOCALE_SISO639LANGNAME         0x00000059u
#define X2RW_LOCALE_SISO3166CTRYNAME        0x0000005Au

/* GetStringType — CT_CTYPE1 */
#define X2RW_CT_CTYPE1    0x0001u
#define X2RW_CT_CTYPE2    0x0002u
#define X2RW_CT_CTYPE3    0x0004u

#define X2RW_C1_UPPER     0x0001u
#define X2RW_C1_LOWER     0x0002u
#define X2RW_C1_DIGIT     0x0004u
#define X2RW_C1_SPACE     0x0008u
#define X2RW_C1_PUNCT     0x0010u
#define X2RW_C1_CNTRL     0x0020u
#define X2RW_C1_BLANK     0x0040u
#define X2RW_C1_XDIGIT    0x0080u
#define X2RW_C1_ALPHA     0x0100u

/* Teto defensivo para as conversões: nenhuma string do jogo chega perto
 * disto, e um ponteiro corrompido não deve fazer o port alocar o heap
 * inteiro. 64 KiB é o mesmo teto de x2rw_guest_strlen(). */
#define X2RW_STR_MAX  0x10000

/* ------------------------------------------------------------------ */
/* Identidade de locale / codepage                                    */
/* ------------------------------------------------------------------ */

/* O CRT pergunta isto para saber se pode converter. */
uint32_t str_GetACP(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c; (void)args; (void)nargs;
    X2RW_OK();
    return X2RW_ACP;
}

uint32_t str_GetOEMCP(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c; (void)args; (void)nargs;
    X2RW_OK();
    return X2RW_OEMCP;
}

uint32_t str_GetUserDefaultLCID(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c; (void)args; (void)nargs;
    X2RW_OK();
    return X2RW_LCID_EN_US;
}

/* IsValidCodePage(cp): 0 é o "ANSI" (o que o GetACP devolveu) e o Windows
 * o considera válido. 65001 (UTF-8) entra porque está instalado em
 * qualquer Windows 2000+; o jogo não o usa, mas o CRT às vezes sonda. */
uint32_t str_IsValidCodePage(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t cp = (nargs > 0) ? args[0] : 0u;
    (void)c;    /* só mexe no last error, não lê guest */
    X2RW_OK();
    switch (cp) {
        case 0u:
        case X2RW_ACP:
        case X2RW_OEMCP:
        case X2RW_CP_UTF8:
            return 1u;
        default:
            return 0u;
    }
}

/* IsValidLocale(lcid, flags): só as duas variantes de inglês que o jogo
 * conhece. Qualquer outra devolve 0 — e é honesto: o port não tem NLS. */
uint32_t str_IsValidLocale(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t lcid = (nargs > 0) ? args[0] : 0u;
    uint32_t flags = (nargs > 1) ? args[1] : 0u;
    (void)c;        /* só mexe no last error, não lê guest */
    (void)flags;    /* LCID_INSTALLED / LCID_SUPPORTED não mudam a resposta */
    X2RW_OK();
    return (lcid == X2RW_LCID_EN_US || lcid == X2RW_LCID_EN_GB) ? 1u : 0u;
}

/* ------------------------------------------------------------------ */
/* MultiByteToWideChar / WideCharToMultiByte                          */
/* ------------------------------------------------------------------ */

/* Aceita as codepages acima (0 = "a ANSI", que é 1252 aqui) e também as
 * combinações que o Windows aceita por compatibilidade. Devolve 0 e loga
 * quando não sabe — o CRT trata 0 como erro de conversão. */
static int str_cp_supported(uint32_t cp) {
    return cp == 0u || cp == X2RW_ACP || cp == X2RW_OEMCP || cp == X2RW_CP_UTF8;
}

/* Comprimento de uma string UTF-16 no guest, em UNIDADES Wide. Não dá
 * para usar x2rw_guest_strlen aqui: ele conta BYTES até o primeiro zero, e
 * "abc" em UTF-16 é 61 00 62 00 63 00 — o primeiro zero está no byte 1.
 * (Foi exatamente esse bug que o teste do port pegou: CompareStringW via
 * "abc" como 1 unidade.) */
static uint32_t str_guest_wstrlen(x2rw_ctx *c, uint32_t va) {
    uint32_t n = 0;
    while (n < X2RW_STR_MAX && x2rw_read16(c, va + 2u * n) != 0) n++;
    return n;
}

/* MultiByteToWideChar(CodePage, dwFlags, lpMultiByteStr, cbMultiByte,
 *                     lpWideCharStr, cchWideChar)
 *
 * Real (mas só nas codepages de 1 byte suportadas): lê a origem para um
 * buffer do host (nunca usa ponteiro do host para memória do guest),
 * converte com x2rw_cp1252_to_utf16 e escreve o destino com x2rw_write16.
 *
 * cbMultiByte == -1 (0xFFFFFFFF): a origem é NUL-terminada. Nesse caso o
 * Windows conta e escreve o NUL final também (cchWideChar==0 devolve
 * len+1; com destino, o retorno é len+1).
 *
 * Só suporta cp 1252/437/0/65001: o jogo é uma build americana de um
 * único idioma. Qualquer outra devolve 0 com ERROR_INVALID_PARAMETER em
 * vez de "quase converter" (que corromperia os menus). */
uint32_t str_MultiByteToWideChar(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t cp    = (nargs > 0) ? args[0] : 0u;
    uint32_t flags = (nargs > 1) ? args[1] : 0u;
    uint32_t src   = (nargs > 2) ? args[2] : 0u;
    uint32_t cch_s = (nargs > 3) ? args[3] : 0u;
    uint32_t dst   = (nargs > 4) ? args[4] : 0u;
    uint32_t cch_d = (nargs > 5) ? args[5] : 0u;
    (void)flags;    /* MB_PRECOMPOSED etc.: irrelevante em CP-1252 */

    if (!src) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }
    if (!str_cp_supported(cp)) {
        x2rw_log("[strings] MultiByteToWideChar: codepage %u não suportada "
                 "(só 1252/437/0/65001)", cp);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }

    /* Monta no host a string de origem + o comprimento REAL em bytes. */
    char srcbuf[X2RW_STR_MAX + 2];
    uint32_t nbytes;        /* unidades de entrada, SEM o NUL */
    int want_nul = 0;

    if (cch_s == 0xFFFFFFFFu) {
        nbytes = x2rw_guest_strlen(c, src);
        want_nul = 1;
    } else {
        nbytes = cch_s;
    }
    if (nbytes > X2RW_STR_MAX) nbytes = X2RW_STR_MAX;
    if (nbytes) x2rw_mem_read(src, srcbuf, nbytes);
    srcbuf[nbytes] = '\0';

    if (cch_d == 0u) {
        /* Só contar: o Windows devolve quantas unidades Wide são
         * necessárias, incluindo o NUL quando a origem é -1. */
        X2RW_OK();
        return nbytes + (uint32_t)want_nul;
    }
    if (!dst) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }

    uint16_t wbuf[X2RW_STR_MAX + 2];
    int conv = x2rw_cp1252_to_utf16(srcbuf, (int)nbytes, wbuf,
                                    (int)(X2RW_STR_MAX + 1));
    int written = conv;
    if (want_nul) {
        /* Inclui o terminador no que é escrito e no retorno, como o
         * Windows faz com cbMultiByte == -1. O helper já sintetizou um
         * NUL em wbuf[conv]; ele é copiado junto. */
        if ((uint32_t)(conv + 1) > cch_d) {
            X2RW_FAIL(c, X2RW_ERROR_INSUFFICIENT_BUFFER);
            return 0u;
        }
        written = conv + 1;
    } else {
        if ((uint32_t)conv > cch_d) {
            X2RW_FAIL(c, X2RW_ERROR_INSUFFICIENT_BUFFER);
            return 0u;
        }
    }
    x2rw_guest_write_utf16(c, dst, wbuf, written);   /* + NUL final */
    X2RW_OK();
    return (uint32_t)written;
}

/* WideCharToMultiByte(CodePage, dwFlags, lpWideCharStr, cchWideChar,
 *                     lpMultiByteStr, cbMultiByte, lpDefaultChar,
 *                     lpUsedDefaultChar)
 *
 * Real, com as mesmas regras do lado A. cchWideChar == -1: a origem é
 * NUL-terminada e o NUL conta. A conversão é feita por
 * x2rw_utf16_to_cp1252: o que não existir em CP-1252 vira '?' (o próprio
 * helper sinaliza isso uma vez por log). */
uint32_t str_WideCharToMultiByte(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t cp     = (nargs > 0) ? args[0] : 0u;
    uint32_t flags  = (nargs > 1) ? args[1] : 0u;
    uint32_t src    = (nargs > 2) ? args[2] : 0u;
    uint32_t cch_s  = (nargs > 3) ? args[3] : 0u;
    uint32_t dst    = (nargs > 4) ? args[4] : 0u;
    uint32_t cb_d   = (nargs > 5) ? args[5] : 0u;
    uint32_t defch  = (nargs > 6) ? args[6] : 0u;
    uint32_t usedch = (nargs > 7) ? args[7] : 0u;
    (void)flags; (void)defch;

    if (!src) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }
    if (!str_cp_supported(cp)) {
        x2rw_log("[strings] WideCharToMultiByte: codepage %u não suportada "
                 "(só 1252/437/0/65001)", cp);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }
    if (usedch) x2rw_write8(c, usedch, 0);   /* nada virou default char */

    /* Lê a origem UTF-16 (por unidade, com x2rw_read16). */
    uint16_t wbuf[X2RW_STR_MAX + 2];
    uint32_t nunits;        /* unidades de entrada, SEM o NUL */
    int want_nul = 0;

    if (cch_s == 0xFFFFFFFFu) {
        nunits = str_guest_wstrlen(c, src);
        want_nul = 1;
    } else {
        nunits = cch_s;
        if (nunits > X2RW_STR_MAX) nunits = X2RW_STR_MAX;
    }
    for (uint32_t i = 0; i < nunits; i++)
        wbuf[i] = x2rw_read16(c, src + 2u * i);
    wbuf[nunits] = 0;

    if (cb_d == 0u) {
        X2RW_OK();
        return nunits + (uint32_t)want_nul;
    }
    if (!dst) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }

    char bbuf[X2RW_STR_MAX + 2];
    int conv = x2rw_utf16_to_cp1252(wbuf, (int)nunits, bbuf,
                                    (int)sizeof(bbuf));
    uint32_t written = (uint32_t)conv;
    if (want_nul) {
        /* A origem é NUL-terminada: o Windows escreve e conta o NUL. */
        if (written + 1u > cb_d) {
            X2RW_FAIL(c, X2RW_ERROR_INSUFFICIENT_BUFFER);
            return 0u;
        }
        bbuf[written] = '\0';
        written += 1u;
    } else {
        /* Sem NUL de origem não escrevemos terminador algum: o buffer do
         * chamador pode ser exatamente do tamanho pedido. */
        if (written > cb_d) {
            X2RW_FAIL(c, X2RW_ERROR_INSUFFICIENT_BUFFER);
            return 0u;
        }
    }
    if (written) x2rw_mem_write(dst, bbuf, (size_t)written);
    X2RW_OK();
    return written;
}

/* ------------------------------------------------------------------ */
/* CompareStringA / CompareStringW                                    */
/* ------------------------------------------------------------------ */

/* Lê `cch` bytes (ou até o NUL, se cch == -1) da string A do guest para
 * um buffer do host. Devolve o comprimento efetivo. */
static uint32_t str_read_a(x2rw_ctx *c, uint32_t va, uint32_t cch,
                           char *buf, size_t cap) {
    uint32_t n = (cch == 0xFFFFFFFFu) ? x2rw_guest_strlen(c, va) : cch;
    if (n > cap - 1u) n = (uint32_t)(cap - 1u);
    if (n) x2rw_mem_read(va, buf, n);
    buf[n] = '\0';
    return n;
}

/* CompareStringA(Locale, dwCmpFlags, lpString1, cchCount1, lpString2,
 *                cchCount2)
 *
 * PARCIAL: o locale é sempre en-US e a comparação é byte a byte em
 * CP-1252 (que para o alfabeto inglês e para os acentos do CP-1252 dá a
 * mesma ordem do Windows). NORM_IGNORECASE é honrado com strcasecmp.
 * Devolve CSTR_* (1/2/3) ou 0 em erro — nunca compara sem respeitar cch. */
uint32_t str_CompareStringA(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t locale = (nargs > 0) ? args[0] : 0u;
    uint32_t flags  = (nargs > 1) ? args[1] : 0u;
    uint32_t s1     = (nargs > 2) ? args[2] : 0u;
    uint32_t cch1   = (nargs > 3) ? args[3] : 0u;
    uint32_t s2     = (nargs > 4) ? args[4] : 0u;
    uint32_t cch2   = (nargs > 5) ? args[5] : 0u;

    if (!s1 || !s2) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }
    (void)locale;   /* sempre en-US: único locale deste port (ver topo) */

    char b1[X2RW_STR_MAX + 2];
    char b2[X2RW_STR_MAX + 2];
    uint32_t n1 = str_read_a(c, s1, cch1, b1, sizeof(b1));
    uint32_t n2 = str_read_a(c, s2, cch2, b2, sizeof(b2));

    /* Compara no máximo o menor comprimento, como o Windows faz com
     * cchCount explícito; se um for prefixo do outro, o mais curto é
     * "menor". */
    uint32_t n = (n1 < n2) ? n1 : n2;
    int r;
    if (flags & X2RW_NORM_IGNORECASE)
        r = strncasecmp(b1, b2, n);
    else
        r = strncmp(b1, b2, n);
    if (r == 0) {
        if (n1 == n2) r = 0;
        else          r = (n1 < n2) ? -1 : 1;
    }
    X2RW_OK();
    if (r < 0) return X2RW_CSTR_LESS_THAN;
    if (r > 0) return X2RW_CSTR_GREATER_THAN;
    return X2RW_CSTR_EQUAL;
}

/* CompareStringW: mesma coisa em UTF-16, comparando por code point. */
uint32_t str_CompareStringW(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t locale = (nargs > 0) ? args[0] : 0u;
    uint32_t flags  = (nargs > 1) ? args[1] : 0u;
    uint32_t s1     = (nargs > 2) ? args[2] : 0u;
    uint32_t cch1   = (nargs > 3) ? args[3] : 0u;
    uint32_t s2     = (nargs > 4) ? args[4] : 0u;
    uint32_t cch2   = (nargs > 5) ? args[5] : 0u;
    (void)locale;

    if (!s1 || !s2) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }

    uint32_t n1 = (cch1 == 0xFFFFFFFFu) ? str_guest_wstrlen(c, s1) : cch1;
    uint32_t n2 = (cch2 == 0xFFFFFFFFu) ? str_guest_wstrlen(c, s2) : cch2;
    if (n1 > X2RW_STR_MAX) n1 = X2RW_STR_MAX;
    if (n2 > X2RW_STR_MAX) n2 = X2RW_STR_MAX;

    uint32_t n = (n1 < n2) ? n1 : n2;
    int r = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t u1 = x2rw_read16(c, s1 + 2u * i);
        uint32_t u2 = x2rw_read16(c, s2 + 2u * i);
        if (flags & X2RW_NORM_IGNORECASE) {
            /* Só ASCII — o mesmo limite do LCMapStringW (ver comentário). */
            if (u1 >= 'a' && u1 <= 'z') u1 -= 0x20u;
            if (u2 >= 'a' && u2 <= 'z') u2 -= 0x20u;
        }
        if (u1 != u2) { r = (u1 < u2) ? -1 : 1; break; }
    }
    if (r == 0 && n1 != n2) r = (n1 < n2) ? -1 : 1;

    X2RW_OK();
    if (r < 0) return X2RW_CSTR_LESS_THAN;
    if (r > 0) return X2RW_CSTR_GREATER_THAN;
    return X2RW_CSTR_EQUAL;
}

/* ------------------------------------------------------------------ */
/* LCMapStringA / LCMapStringW                                        */
/* ------------------------------------------------------------------ */

/* LCMapStringA(Locale, dwMapFlags, lpSrcStr, cchSrc, lpDestStr, cchDest)
 *
 * PARCIAL: só LCMAP_LOWERCASE / LCMAP_UPPERCASE, e só para ASCII —
 * o mapeamento de caixa fora de ASCII exigiria as tabelas do NLS do
 * Windows, que o port não tem. Para o texto inglês do jogo isso é
 * exato; para um byte acentuado o resultado é o próprio byte (não um
 * '?' — preservar é melhor que corromper).
 *
 * Cuidado de sinal: LCMAP_LOWERCASE é 0x100 e LCMAP_UPPERCASE é 0x200;
 * eles NUNCA aparecem juntos, e usamos comparação de igualdade (não
 * teste de bit) justamente para não confundir os dois.
 *
 * LCMAP_SORTKEY não é suportado: a chave de ordenação do Windows tem um
 * formato binário próprio (pesos + NULs internos). Devolver um "sort key"
 * caseiro faria o jogo ordenar errado em silêncio, então devolvemos 0
 * com erro e logamos.
 *
 * Devolve o número de bytes escritos (sem o NUL); com cchDest == 0,
 * apenas conta. */
uint32_t str_LCMapStringA(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t locale = (nargs > 0) ? args[0] : 0u;
    uint32_t flags  = (nargs > 1) ? args[1] : 0u;
    uint32_t src    = (nargs > 2) ? args[2] : 0u;
    uint32_t cch_s  = (nargs > 3) ? args[3] : 0u;
    uint32_t dst    = (nargs > 4) ? args[4] : 0u;
    uint32_t cch_d  = (nargs > 5) ? args[5] : 0u;
    (void)locale;

    if (flags & X2RW_LCMAP_SORTKEY) {
        x2rw_log("[strings] LCMapStringA: LCMAP_SORTKEY não implementado "
                 "(o formato da chave de ordenação do Windows não é "
                 "reproduzido aqui)");
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }
    if (!src) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }
    if (!(flags & (X2RW_LCMAP_LOWERCASE | X2RW_LCMAP_UPPERCASE))) {
        /* Sem flag de mapeamento o Windows devolve o comprimento da
         * string (locale "fold" — usado só para medir). */
        uint32_t n = (cch_s == 0xFFFFFFFFu) ? x2rw_guest_strlen(c, src) : cch_s;
        if (cch_d == 0u) { X2RW_OK(); return n; }
        if (dst) x2rw_guest_strcpy(c, dst, "", 1);
        X2RW_OK();
        return n;
    }

    char buf[X2RW_STR_MAX + 2];
    uint32_t n = str_read_a(c, src, cch_s, buf, sizeof(buf));
    int lower = (flags & X2RW_LCMAP_LOWERCASE) != 0u;

    for (uint32_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)buf[i];
        if (ch >= 'A' && ch <= 'Z' && lower) buf[i] = (char)(ch + 0x20u);
        else if (ch >= 'a' && ch <= 'z' && !lower) buf[i] = (char)(ch - 0x20u);
    }

    if (cch_d == 0u) { X2RW_OK(); return n; }   /* só contar */
    if (!dst) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }
    if (n + 1u > cch_d) {
        X2RW_FAIL(c, X2RW_ERROR_INSUFFICIENT_BUFFER);
        return 0u;
    }
    if (n) x2rw_mem_write(dst, buf, n);
    x2rw_write8(c, dst + n, 0);
    X2RW_OK();
    return n;
}

/* LCMapStringW: mesma semântica, em UTF-16 (só ASCII). */
uint32_t str_LCMapStringW(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t locale = (nargs > 0) ? args[0] : 0u;
    uint32_t flags  = (nargs > 1) ? args[1] : 0u;
    uint32_t src    = (nargs > 2) ? args[2] : 0u;
    uint32_t cch_s  = (nargs > 3) ? args[3] : 0u;
    uint32_t dst    = (nargs > 4) ? args[4] : 0u;
    uint32_t cch_d  = (nargs > 5) ? args[5] : 0u;
    (void)locale;

    if (flags & X2RW_LCMAP_SORTKEY) {
        x2rw_log("[strings] LCMapStringW: LCMAP_SORTKEY não implementado "
                 "(ver LCMapStringA)");
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }
    if (!src) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }

    uint32_t n = (cch_s == 0xFFFFFFFFu)
                     ? str_guest_wstrlen(c, src)
                     : ((cch_s > X2RW_STR_MAX) ? X2RW_STR_MAX : cch_s);

    if (!(flags & (X2RW_LCMAP_LOWERCASE | X2RW_LCMAP_UPPERCASE))) {
        if (cch_d == 0u) { X2RW_OK(); return n; }
        if (dst) x2rw_write16(c, dst, 0);
        X2RW_OK();
        return n;
    }

    uint16_t wbuf[X2RW_STR_MAX + 1];
    int lower = (flags & X2RW_LCMAP_LOWERCASE) != 0u;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t u = x2rw_read16(c, src + 2u * i);
        if (u >= 'A' && u <= 'Z' && lower) u += 0x20u;
        else if (u >= 'a' && u <= 'z' && !lower) u -= 0x20u;
        wbuf[i] = (uint16_t)u;
    }

    if (cch_d == 0u) { X2RW_OK(); return n; }
    if (!dst) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }
    if (n + 1u > cch_d) {
        X2RW_FAIL(c, X2RW_ERROR_INSUFFICIENT_BUFFER);
        return 0u;
    }
    x2rw_guest_write_utf16(c, dst, wbuf, (int)n);
    X2RW_OK();
    return n;
}

/* ------------------------------------------------------------------ */
/* GetCPInfo                                                          */
/* ------------------------------------------------------------------ */

/* GetCPInfo(CodePage, lpCPInfo) -> BOOL
 *
 * A CPINFO é { UINT MaxCharSize; BYTE DefaultChar[2]; BYTE LeadByte[12]; }
 * = 4 + 2 + 12 = 18 bytes (sem padding: MaxCharSize é UINT de 4 bytes e o
 * resto são bytes). Para 1252/437/65001 o MaxCharSize é 1 (o 65001 tem
 * multibyte de verdade, mas o CRT só usa isto para decidir o buffer de
 * conversão; anunciar 1 é correto para 1252/437 e conservador para UTF-8,
 * que o jogo não usa). LeadByte fica zerado = "nenhum lead byte".
 * Devolve 1 (sucesso). */
uint32_t str_GetCPInfo(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t cp   = (nargs > 0) ? args[0] : 0u;
    uint32_t info = (nargs > 1) ? args[1] : 0u;

    if (!str_cp_supported(cp)) {
        x2rw_log("[strings] GetCPInfo: codepage %u não suportada", cp);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }
    if (!info) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }
    x2rw_write32(c, info, 1u);            /* MaxCharSize = 1            */
    x2rw_write8(c, info + 4u, '?');       /* DefaultChar[0] = '?'       */
    x2rw_write8(c, info + 5u, 0u);        /* DefaultChar[1] = 0         */
    for (uint32_t i = 0; i < 12u; i++)    /* LeadByte[12] = zeros       */
        x2rw_write8(c, info + 6u + i, 0u);
    X2RW_OK();
    return 1u;
}

/* ------------------------------------------------------------------ */
/* GetLocaleInfoA / GetLocaleInfoW                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t    type;
    const char *value;      /* string ASCII (também serve para o lado W) */
} x2rw_locale_entry;

/* Valores do en-US. Nada aqui é inventado: são os valores documentados
 * do locale 0x0409 no Windows (o jogo é build americana e o LCID default
 * é 0x0409 — ver str_GetUserDefaultLCID). O que o CRT do MSVC pergunta
 * está todo coberto; o resto cai no default e devolve erro. */
static const x2rw_locale_entry g_locale_en_us[] = {
    { X2RW_LOCALE_ILANGUAGE,             "0409"        },
    { X2RW_LOCALE_SABBREVLANGNAME,       "ENU"         },
    { X2RW_LOCALE_SISO639LANGNAME,       "en"          },
    { X2RW_LOCALE_SENGLANGUAGE,          "en-US"       },  /* o CRT usa    */
    { X2RW_LOCALE_SENGCOUNTRY,           "United States"},
    { X2RW_LOCALE_SABBREVCTRYNAME,       "USA"         },
    { X2RW_LOCALE_SISO3166CTRYNAME,      "US"          },
    { X2RW_LOCALE_IDEFAULTANSICODEPAGE,  "1252"        },  /* o CRT usa    */
    { X2RW_LOCALE_SDECIMAL,              "."           },
    { X2RW_LOCALE_STHOUSAND,             ","           },
    { X2RW_LOCALE_SDATE,                 "MM/dd/yyyy"  },
    /* O formato de hora do en-US: "h:mm:ss tt" (a forma curta do painel
     * de controle). É a forma que o CRT devolve numa instalação en-US. */
    { X2RW_LOCALE_STIMEFORMAT,           "h:mm:ss tt"  }
};

static const char *str_locale_lookup(uint32_t lcid, uint32_t type) {
    if (lcid != 0u && lcid != X2RW_LCID_EN_US && lcid != X2RW_LCID_EN_GB)
        return NULL;    /* não temos NLS para nenhum outro locale */
    for (size_t i = 0; i < sizeof(g_locale_en_us) / sizeof(g_locale_en_us[0]); i++)
        if (g_locale_en_us[i].type == type) return g_locale_en_us[i].value;
    return NULL;
}

/* GetLocaleInfoA(Locale, LCType, lpLCData, cchData)
 *
 * Devolve o número de BYTES escritos, incluindo o NUL; com cchData == 0,
 * devolve o tamanho necessário (em bytes, incluindo o NUL) sem escrever.
 * LCType desconhecido -> 0 + ERROR_INVALID_PARAMETER (comentado acima:
 * nunca inventamos um valor de locale). */
uint32_t str_GetLocaleInfoA(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t lcid  = (nargs > 0) ? args[0] : 0u;
    uint32_t type  = (nargs > 1) ? args[1] : 0u;
    uint32_t data  = (nargs > 2) ? args[2] : 0u;
    uint32_t cch   = (nargs > 3) ? args[3] : 0u;

    const char *v = str_locale_lookup(lcid, type);
    if (!v) {
        x2rw_log("[strings] GetLocaleInfoA: LCType 0x%08x (lcid 0x%04x) "
                 "não suportado", type, lcid);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }

    uint32_t need = (uint32_t)strlen(v) + 1u;   /* inclui o NUL */
    if (cch == 0u) { X2RW_OK(); return need; }
    if (!data) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }
    if (cch < need) {
        X2RW_FAIL(c, X2RW_ERROR_INSUFFICIENT_BUFFER);
        return 0u;
    }
    x2rw_guest_strcpy(c, data, v, (size_t)cch);
    X2RW_OK();
    return need;
}

/* GetLocaleInfoW: mesma tabela, escrita em UTF-16 |
 * o retorno é o número de unidades Wide, incluindo o NUL. */
uint32_t str_GetLocaleInfoW(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t lcid  = (nargs > 0) ? args[0] : 0u;
    uint32_t type  = (nargs > 1) ? args[1] : 0u;
    uint32_t data  = (nargs > 2) ? args[2] : 0u;
    uint32_t cch   = (nargs > 3) ? args[3] : 0u;

    const char *v = str_locale_lookup(lcid, type);
    if (!v) {
        x2rw_log("[strings] GetLocaleInfoW: LCType 0x%08x (lcid 0x%04x) "
                 "não suportado", type, lcid);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }

    /* A tabela é ASCII puro: cada byte vira uma unidade Wide. */
    uint16_t w[64];
    uint32_t n = (uint32_t)strlen(v);
    if (n > 63u) n = 63u;
    for (uint32_t i = 0; i < n; i++) w[i] = (uint16_t)(unsigned char)v[i];

    uint32_t need = n + 1u;     /* inclui o NUL */
    if (cch == 0u) { X2RW_OK(); return need; }
    if (!data) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }
    if (cch < need) {
        X2RW_FAIL(c, X2RW_ERROR_INSUFFICIENT_BUFFER);
        return 0u;
    }
    x2rw_guest_write_utf16(c, data, w, (int)n);
    X2RW_OK();
    return need;
}

/* ------------------------------------------------------------------ */
/* GetStringTypeA / GetStringTypeW                                    */
/* ------------------------------------------------------------------ */

/* Tabela CT_CTYPE1 para um byte CP-1252. Cobre ASCII e os acentuados
 * imprimíveis do CP-1252 (0xA0-0xFF): em CP-1252 as letras acentuadas
 * ocupam exatamente 0xC0-0xFF (maiúsculas), 0xE0-0xFF (minúsculas) e
 * 0xDF (ß) — os mesmos intervalos do Latin-1, que é onde o CP-1252
 * coincide com o Unicode. Os bytes 0x80-0x9F são os caracteres
 * "tipográficos" (aspas curvas, travessão...) e contam como pontuação. */
static uint16_t str_ctype1_byte(uint8_t b) {
    uint16_t f = 0;

    if (b < 0x80u) {
        if (b >= 'A' && b <= 'Z') f |= X2RW_C1_UPPER | X2RW_C1_ALPHA;
        if (b >= 'a' && b <= 'z') f |= X2RW_C1_LOWER | X2RW_C1_ALPHA;
        if (b >= '0' && b <= '9') f |= X2RW_C1_DIGIT | X2RW_C1_XDIGIT;
        if ((b >= 'A' && b <= 'F') || (b >= 'a' && b <= 'f'))
            f |= X2RW_C1_XDIGIT;
        if (b == ' ' || b == '\t') f |= X2RW_C1_SPACE | X2RW_C1_BLANK;
        if (b == '\n' || b == '\r' || b == '\v' || b == '\f')
            f |= X2RW_C1_SPACE;
        if (b < 0x20u || b == 0x7Fu) f |= X2RW_C1_CNTRL;
        if (b >= 0x21u && b <= 0x7Eu &&
            !(f & (X2RW_C1_ALPHA | X2RW_C1_DIGIT)))
            f |= X2RW_C1_PUNCT;
        return f;
    }

    if (b >= 0xC0u && b <= 0xDEu && b != 0xD7u) f |= X2RW_C1_UPPER | X2RW_C1_ALPHA;
    if (b >= 0xDFu && b <= 0xFFu && b != 0xF7u) f |= X2RW_C1_LOWER | X2RW_C1_ALPHA;
    if (b == 0xAAu || b == 0xBAu) f |= X2RW_C1_LOWER | X2RW_C1_ALPHA;
    if (b >= 0x80u && b <= 0xA0u) f |= X2RW_C1_PUNCT;  /* inclui NBSP */
    if (b >= 0xA1u && b <= 0xBFu && !(f & X2RW_C1_ALPHA)) f |= X2RW_C1_PUNCT;
    if (b == 0xD7u || b == 0xF7u) f |= X2RW_C1_PUNCT;  /* × e ÷ */
    return f;
}

/* GetStringTypeA(Locale, dwInfoType, lpSrcStr, cchSrc, lpCharType)
 *
 * Real para CT_CTYPE1: escreve uma WORD por caractere no guest. CT_CTYPE2
 * (direção) e CT_CTYPE3 (Kana/CJK) não são suportados — devolvem 0 com
 * erro em vez de preencher uma tabela de zeros que o jogo leria como
 * "isto não é letra nenhuma". (O jogo só usa CT_CTYPE1 na classificação
 * de letras/dígitos do parser.) */
uint32_t str_GetStringTypeA(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t locale = (nargs > 0) ? args[0] : 0u;
    uint32_t type   = (nargs > 1) ? args[1] : 0u;
    uint32_t src    = (nargs > 2) ? args[2] : 0u;
    uint32_t cch    = (nargs > 3) ? args[3] : 0u;
    uint32_t out    = (nargs > 4) ? args[4] : 0u;
    (void)locale;

    if (type != X2RW_CT_CTYPE1) {
        x2rw_log("[strings] GetStringTypeA: dwInfoType 0x%04x não suportado "
                 "(só CT_CTYPE1)", type);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }
    if (!src || !out) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }

    uint32_t n = (cch == 0xFFFFFFFFu) ? x2rw_guest_strlen(c, src) : cch;
    if (n > X2RW_STR_MAX) n = X2RW_STR_MAX;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t b = x2rw_read8(c, src + i);
        x2rw_write16(c, out + 2u * i, str_ctype1_byte(b));
    }
    X2RW_OK();
    return 1u;
}

/* GetStringTypeW(Locale, dwInfoType, lpSrcStr, cchSrc, lpCharType)
 *
 * LIMITAÇÃO REAL (e é assim que o Windows funciona): o buffer do chamador
 * tem 65536 entradas, mas o Windows preenche UMA entrada por caractere de
 * lpSrcStr. Este handler escreve exatamente `cchSrc` (= n) entradas
 * uint16 no guest, nunca a tabela inteira.
 *
 * Para UTF-16 só classificamos o que está em ASCII mais os mesmos
 * intervalos de acento do CP-1252 (U+00C0..U+00FF), que é o que as
 * strings do jogo contêm. O resto fica 0. */
uint32_t str_GetStringTypeW(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t locale = (nargs > 0) ? args[0] : 0u;
    uint32_t type   = (nargs > 1) ? args[1] : 0u;
    uint32_t src    = (nargs > 2) ? args[2] : 0u;
    uint32_t cch    = (nargs > 3) ? args[3] : 0u;
    uint32_t out    = (nargs > 4) ? args[4] : 0u;
    (void)locale;

    if (type != X2RW_CT_CTYPE1) {
        x2rw_log("[strings] GetStringTypeW: dwInfoType 0x%04x não suportado "
                 "(só CT_CTYPE1)", type);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }
    if (!src || !out) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }

    uint32_t n = (cch == 0xFFFFFFFFu)
                     ? str_guest_wstrlen(c, src)
                     : ((cch > X2RW_STR_MAX) ? X2RW_STR_MAX : cch);

    for (uint32_t i = 0; i < n; i++) {
        uint32_t u = x2rw_read16(c, src + 2u * i);
        uint16_t f;
        if (u <= 0xFFu)
            f = str_ctype1_byte((uint8_t)u);
        else
            f = 0;      /* fora do Latin-1: zero, não inventamos categoria */
        x2rw_write16(c, out + 2u * i, f);
    }
    X2RW_OK();
    return 1u;
}

/* ------------------------------------------------------------------ */
/* EnumSystemLocalesA                                                 */
/* ------------------------------------------------------------------ */

/* ====================================================================
 * LIMITAÇÃO EM DESTAQUE — EnumSystemLocalesA NÃO ENUMERA NADA
 * ====================================================================
 * O primeiro argumento é um ponteiro para CÓDIGO x86 do guest
 * (LOCALE_ENUMPROCA). Chamá-lo daqui exigiria reentrar no runtime do
 * guest a partir do C do host, o que esta camada de shim não faz: os
 * handlers só tocam a memória do guest e devolvem valores, nunca
 * transferem controle para código lifted.
 *
 * Consequência: o jogo NÃO enumera locales. Ele não precisa — a build é
 * americana e o locale é fixo em 0x0409 —, e o resultado é honesto:
 * devolvemos 1 (sucesso) porque a "enumeração" de um único locale, o
 * en-US, está completa no que o shim oferece, e deixamos um log bem
 * visível para que ninguém conclua que o callback rodou.
 *
 * Se algum dia for necessário enumerar de verdade, o certo é registrar
 * uma função lifted no runtime que chame o callback com os LCIDs da
 * tabela de idiomas do próprio binário — não inventar isso aqui.
 * ==================================================================== */
uint32_t str_EnumSystemLocalesA(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t proc  = (nargs > 0) ? args[0] : 0u;
    uint32_t flags = (nargs > 1) ? args[1] : 0u;
    (void)c;
    x2rw_log("[strings] EnumSystemLocalesA: callback do guest %08x NÃO é "
             "chamado (não reentramos no código x86 lifted); devolvendo "
             "sucesso sem enumerar. Locale fixo: 0x0409 (flags=%08x)",
             proc, flags);
    X2RW_OK();
    return 1u;
}
