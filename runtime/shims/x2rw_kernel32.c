/* X2: Wolverine's Revenge - port Android
 * ============================================================================
 * x2rw_kernel32.c — os 119 handlers de KERNEL32.dll (núcleo).
 *
 * A lista mestra (x2rw_shims.h, macro X2RW_SHIMS_KERNEL32) é a fonte de
 * verdade dos NOMES e das ARIDADES; este arquivo é a implementação.
 *
 * MODELO
 * ------
 * O jogo é x86-32 recompilado; a memória do guest é um espaço paginado de
 * 32 bits, NUNCA um ponteiro do host. Todo acesso a dados do jogo passa por
 * x2rw_read*, x2rw_write* e x2rw_mem_*; strings do jogo são VAs.
 *
 * Handles: o Windows devolve HANDLEs opacos. Aqui eles são sintéticos e
 * vivem em tabelas `static` deste módulo:
 *
 *   0x00000001..03      stdin/stdout/stderr (GetStdHandle)
 *   0x00000100..        arquivos abertos (FILE * do host)
 *   0x00000200..        buscas FindFirstFileA/FindNextFileA (DIR *)
 *   0x00000400..        objetos de sincronização e mapeamentos
 *   0x00A00000          "heap" do processo (o heap do guest é um só)
 *   0x70000000..        módulos na GetModuleHandleA/LoadLibraryA
 *
 * Um handle desconhecido NUNCA é aceito em silêncio: devolve erro
 * (INVALID_HANDLE) e loga. Ver README_IMPL.md §5.
 *
 * O QUE NÃO É REAL (e por quê)
 * ----------------------------
 *   - Threads: o port é single-thread nesta fase. CreateThread devolve um
 *     handle sintético e loga que o corpo NÃO roda; ExitThread/x2rw_fault.
 *   - Recursos de PE (FindResourceA/LoadResource/...): as seções de recurso
 *     do Wolvie.exe não existem no build Android (só o código lifted), então
 *     a API falha ALTO (erro + log) em vez de devolver ponteiro inventado.
 *   - GetProcAddress: funciona de verdade para os 226 símbolos do registro
 *     (o despachante reconhece X2RW_EXPORT_BASE); para qualquer outro export
 *     devolve 0 com ERROR_PROC_NOT_FOUND e loga.
 *
 * `stdcall`: o despachante já faz `esp += 4*nargs`; NENHUM handler toca em
 * c->r.esp.
 * ============================================================================
 */
#include "x2rw_runtime.h"
#include "x2rw_shims.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>

/* ==================================================================== */
/* Constantes                                                           */
/* ==================================================================== */

#define K32_INVALID_HANDLE    0xFFFFFFFFu
#define K32_STD_IN            0x00000001u
#define K32_STD_OUT           0x00000002u
#define K32_STD_ERR           0x00000003u
#define K32_HANDLE_FILE_BASE  0x00000100u
#define K32_HANDLE_FIND_BASE  0x00000200u
#define K32_HANDLE_OBJ_BASE   0x00000400u
#define K32_HEAP_HANDLE       0x00A00000u
#define K32_PROCESS_HANDLE    0xFFFFFFFFu   /* GetCurrentProcess: (HANDLE)-1 */
#define K32_THREAD_HANDLE     0xFFFFFFFEu   /* GetCurrentThread:  (HANDLE)-2 */
#define K32_MAIN_THREAD_ID    0x00000001u

#define K32_MAX_FILES   64
#define K32_MAX_FINDS   16
#define K32_MAX_OBJECTS 64
#define K32_MAX_CS      64
#define K32_MAX_LOADS   16
#define K32_TLS_SLOTS   64
#define K32_MAX_ENV     32
#define K32_ENV_NAME    64
#define K32_ENV_VALUE   192
#define K32_INI_MAX     65536
#define K32_IO_CHUNK    16384

/* Acessos/atributos do Win32 que usamos (nomes locais, sem windows.h). */
#define K32_GENERIC_READ    0x80000000u
#define K32_GENERIC_WRITE   0x40000000u
#define K32_GENERIC_ALL     0x10000000u
#define K32_CREATE_NEW      1u
#define K32_CREATE_ALWAYS   2u
#define K32_OPEN_EXISTING   3u
#define K32_OPEN_ALWAYS     4u
#define K32_TRUNCATE_EXIST  5u
#define K32_HEAP_ZERO_MEMORY 0x00000008u

#define K32_FILE_ATTRIBUTE_READONLY  0x00000001u
#define K32_FILE_ATTRIBUTE_DIRECTORY 0x00000010u
#define K32_FILE_ATTRIBUTE_ARCHIVE   0x00000020u

#define K32_FILE_TYPE_DISK    1u
#define K32_FILE_TYPE_CHAR    2u
#define K32_FILE_TYPE_UNKNOWN 0u

#define K32_MEM_COMMIT  0x00001000u
#define K32_MEM_FREE    0x00010000u
#define K32_MEM_PRIVATE 0x00020000u
#define K32_PAGE_NOACCESS    0x01u
#define K32_PAGE_READWRITE   0x04u

/* Segundos entre 1601-01-01 e 1970-01-01 (para FILETIME). */
#define K32_FT_UNIX_DELTA 11644473600LL

enum {
    K32_OBJ_MUTEX = 1,
    K32_OBJ_EVENT,
    K32_OBJ_MAPPING,
    K32_OBJ_THREAD
};

/* Tipo de retorno dos helpers INTERNOS deste arquivo (prefixo `k32i_`).
 * O verificador de handlers reconhece `uint32_t nome(` — a lista de handlers
 * deste arquivo tem de ser EXATAMENTE os 119 nomes de X2RW_SHIMS_KERNEL32,
 * então os helpers internos usam este alias e não se confundem com handlers.
 * Helpers internos: k32i_errno_to_win, k32i_create_file_impl,
 * k32i_copy_fixed, k32i_env_build_a, k32i_env_build_w, k32i_int_add. */
typedef uint32_t k32_word;

/* ==================================================================== */
/* Helpers de string / cópia                                           */
/* ==================================================================== */

static char k32_lower(char ch) {
    return (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
}

/* Compara strings sem diferenciar maiúsculas (ASCII). */
static int k32_ci_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (k32_lower(*a) != k32_lower(*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* Compara `a[0..an)` com a string NUL-terminada `b`, sem diferenciar caixa. */
static int k32_ci_eq_n(const char *a, size_t an, const char *b) {
    size_t i = 0;
    for (; i < an; i++) {
        if (!b[i]) return 0;
        if (k32_lower(a[i]) != k32_lower(b[i])) return 0;
    }
    return b[an] == 0;
}

static void k32_copy(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    size_t i = 0;
    for (; src && src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* Lê uma string ANSI (CP-1252) do guest para o buffer do host. */
static void k32_str(x2rw_ctx *c, uint32_t va, char *buf, size_t cap) {
    if (!buf || cap == 0) return;
    buf[0] = 0;
    if (!va) return;
    uint32_t n = x2rw_guest_strlen(c, va);
    if (n > cap - 1) n = (uint32_t)(cap - 1);
    if (n) x2rw_mem_read(va, buf, n);
    buf[n] = 0;
}

/* Lê uma string UTF-16 do guest e converte para CP-1252 no host. */
static void k32_wstr(x2rw_ctx *c, uint32_t va, char *out, size_t cap) {
    uint16_t w[512];
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!va) return;
    uint32_t i = 0;
    for (; i + 1 < (uint32_t)(sizeof(w) / sizeof(w[0])); i++) {
        uint16_t ch = x2rw_read16(c, va + 2u * i);
        if (!ch) break;
        w[i] = ch;
    }
    w[i] = 0;
    x2rw_utf16_to_cp1252(w, (int)i, out, (int)cap);
}

/* ==================================================================== */
/* Bytes no guest (zeragem e cópia em bloco)                            */
/* ==================================================================== */

/* Buffer de E/S compartilhado. O port é single-thread (ver o cabeçalho);
 * usá-lo evita malloc/free por chamada de ReadFile/WriteFile. */
static uint8_t g_io_buf[K32_IO_CHUNK];

static void k32_zero(x2rw_ctx *c, uint32_t va, uint32_t size) {
    static const uint8_t z[256];
    uint32_t off = 0;
    (void)c;
    while (off < size) {
        uint32_t n = size - off;
        if (n > (uint32_t)sizeof(z)) n = (uint32_t)sizeof(z);
        x2rw_mem_write(va + off, z, n);
        off += n;
    }
}

static void k32_guest_copy(uint32_t dst, uint32_t src, uint32_t n) {
    while (n) {
        uint32_t c = n > K32_IO_CHUNK ? K32_IO_CHUNK : n;
        x2rw_mem_read(src, g_io_buf, c);
        x2rw_mem_write(dst, g_io_buf, c);
        src += c; dst += c; n -= c;
    }
}

/* Escrita little-endian explícita (nenhuma premissa de endianness do host). */
static void k32_put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void k32_put_u64(uint8_t *p, uint64_t v) {
    k32_put_u32(p, (uint32_t)v);
    k32_put_u32(p + 4, (uint32_t)(v >> 32));
}

/* ==================================================================== */
/* Tempo: base monotônica e conversões FILETIME/SYSTEMTIME              */
/* ==================================================================== */

static uint64_t g_mono_base_ns = 0;

static uint64_t k32_mono_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Nanossegundos desde a primeira chamada. GetTickCount e
 * QueryPerformanceCounter usam ESTA base: ambos precisam ser monotônicos e
 * crescentes (o jogo trava se o tempo "volta"). */
static uint64_t k32_mono_rel(void) {
    uint64_t n = k32_mono_ns();
    if (g_mono_base_ns == 0) g_mono_base_ns = n;
    return n >= g_mono_base_ns ? n - g_mono_base_ns : 0;   /* nunca negativo */
}

static uint64_t k32_unix_to_ft(int64_t secs) {
    return (uint64_t)((secs + K32_FT_UNIX_DELTA) * 10000000LL);
}

static uint64_t k32_now_filetime(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return k32_unix_to_ft((int64_t)ts.tv_sec) + (uint64_t)(ts.tv_nsec / 100);
}

static uint64_t k32_read_ft(x2rw_ctx *c, uint32_t va) {
    uint64_t lo = (uint64_t)x2rw_read32(c, va);
    uint64_t hi = (uint64_t)x2rw_read32(c, va + 4u);
    return lo | (hi << 32);
}

static void k32_write_ft(x2rw_ctx *c, uint32_t va, uint64_t ft) {
    x2rw_write32(c, va, (uint32_t)ft);
    x2rw_write32(c, va + 4u, (uint32_t)(ft >> 32));
}

/* SYSTEMTIME (16 bytes, 8 WORDs) a partir de um struct tm. */
static void k32_write_systime(x2rw_ctx *c, uint32_t va, const struct tm *t, int ms) {
    x2rw_write16(c, va + 0u,  (uint16_t)(t->tm_year + 1900));
    x2rw_write16(c, va + 2u,  (uint16_t)(t->tm_mon + 1));
    x2rw_write16(c, va + 4u,  (uint16_t)t->tm_wday);
    x2rw_write16(c, va + 6u,  (uint16_t)t->tm_mday);
    x2rw_write16(c, va + 8u,  (uint16_t)t->tm_hour);
    x2rw_write16(c, va + 10u, (uint16_t)t->tm_min);
    x2rw_write16(c, va + 12u, (uint16_t)t->tm_sec);
    x2rw_write16(c, va + 14u, (uint16_t)ms);
}

/* FILETIME (UTC) -> tm + milissegundos. */
static int k32_ft_to_parts(uint64_t ft, struct tm *out, int *out_ms) {
    int64_t secs = (int64_t)(ft / 10000000ull) - K32_FT_UNIX_DELTA;
    if (secs < 0) secs = 0;                 /* antes de 1601: satura em 1601 */
    *out_ms = (int)((ft % 10000000ull) / 10000ull);
    time_t t = (time_t)secs;
    return gmtime_r(&t, out) != NULL;
}

/* Diferença em segundos entre a hora local e UTC (local = UTC + offset).
 * Evita timegm(): usa a propriedade de que mktime() interpreta o tm como
 * hora local, então mktime(gmtime(T)) - T = offset. */
static int64_t k32_tz_offset(void) {
    time_t now = time(NULL);
    struct tm g;
    if (!gmtime_r(&now, &g)) return 0;
    time_t as_local = mktime(&g);
    if (as_local == (time_t)-1) return 0;
    return (int64_t)as_local - (int64_t)now;
}

/* ==================================================================== */
/* Tabela de arquivos                                                   */
/* ==================================================================== */

typedef struct {
    int       used;
    uint32_t  handle;
    FILE     *fp;
} k32_file_t;

static k32_file_t g_files[K32_MAX_FILES];
static uint32_t   g_next_file = K32_HANDLE_FILE_BASE;

static k32_file_t *k32_file_find(uint32_t h) {
    if (h < K32_HANDLE_FILE_BASE) return NULL;
    for (int i = 0; i < K32_MAX_FILES; i++)
        if (g_files[i].used && g_files[i].handle == h) return &g_files[i];
    return NULL;
}

static k32_file_t *k32_file_alloc(FILE *fp) {
    for (int i = 0; i < K32_MAX_FILES; i++) {
        if (!g_files[i].used) {
            g_files[i].used = 1;
            g_files[i].fp = fp;
            g_files[i].handle = g_next_file++;
            return &g_files[i];
        }
    }
    return NULL;
}

static void k32_file_close(k32_file_t *f) {
    if (!f) return;
    if (f->fp) fclose(f->fp);
    f->fp = NULL;
    f->used = 0;
}

static int k32_host_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int k32_host_isdir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static k32_word k32i_errno_to_win(int e) {
    switch (e) {
        case ENOENT:    return X2RW_ERROR_FILE_NOT_FOUND;
        case EACCES:
        case EPERM:     return X2RW_ERROR_ACCESS_DENIED;
        case EEXIST:    return X2RW_ERROR_ALREADY_EXISTS;
        case ENOMEM:    return X2RW_ERROR_NOT_ENOUGH_MEMORY;
        case ENOTDIR:
        case EISDIR:    return X2RW_ERROR_PATH_NOT_FOUND;
        default:        return X2RW_ERROR_FILE_NOT_FOUND;
    }
}

/* Abre `host` seguindo dwDesiredAccess/dwCreationDisposition do Win32.
 * Devolve NULL e o código de erro Win32 em *out_err. */
static FILE *k32_fopen_win32(const char *host, uint32_t access, uint32_t disp,
                             uint32_t *out_err) {
    int want_write = ((access & K32_GENERIC_WRITE) != 0) ||
                     ((access & K32_GENERIC_ALL) != 0);
    FILE *fp = NULL;
    errno = 0;
    switch (disp) {
        case K32_CREATE_NEW:                       /* CREATE_NEW */
            if (k32_host_exists(host)) {
                *out_err = X2RW_ERROR_ALREADY_EXISTS;
                return NULL;
            }
            fp = fopen(host, "w+b");
            break;
        case K32_CREATE_ALWAYS:                    /* CREATE_ALWAYS */
            fp = fopen(host, "w+b");
            break;
        case K32_OPEN_ALWAYS:                      /* OPEN_ALWAYS */
            fp = fopen(host, want_write ? "r+b" : "rb");
            if (!fp) {
                errno = 0;
                fp = fopen(host, "w+b");
            }
            break;
        case K32_TRUNCATE_EXIST:                   /* TRUNCATE_EXISTING */
            if (!k32_host_exists(host)) {
                *out_err = X2RW_ERROR_FILE_NOT_FOUND;
                return NULL;
            }
            fp = fopen(host, "w+b");
            break;
        case K32_OPEN_EXISTING:                    /* OPEN_EXISTING */
        default:
            fp = fopen(host, want_write ? "r+b" : "rb");
            break;
    }
    if (!fp) *out_err = k32i_errno_to_win(errno ? errno : ENOENT);
    return fp;
}

/* Corpo comum de CreateFileA/CreateFileW (o caminho já vem em CP-1252). */
static k32_word k32i_create_file_impl(x2rw_ctx *c, const char *win,
                                     uint32_t access, uint32_t disp) {
    if (!win || !win[0]) {
        x2rw_log("[k32] CreateFile: nome de arquivo vazio");
        x2rw_last_error_set(X2RW_ERROR_INVALID_PARAMETER);
        return K32_INVALID_HANDLE;
    }
    /* Nomes de dispositivo: o port não tem console Win32, mas o jogo pode
     * usá-los para o log de depuração. */
    if (k32_ci_eq(win, "CONOUT$") || k32_ci_eq(win, "CON")) {
        X2RW_OK();
        return K32_STD_OUT;
    }
    if (k32_ci_eq(win, "CONIN$")) {
        X2RW_OK();
        return K32_STD_IN;
    }

    char host[1024];
    if (k32_ci_eq(win, "NUL")) {
        k32_copy(host, sizeof(host), "/dev/null");   /* escrito, descartado */
    } else {
        x2rw_win32_path_to_host(win, host, sizeof(host));
    }
    if (!host[0]) k32_copy(host, sizeof(host), ".");

    /* O Win32 falha ao abrir um diretório sem FILE_FLAG_BACKUP_SEMANTICS. */
    if (k32_host_isdir(host)) {
        x2rw_log("[k32] CreateFile('%s'): é um diretório", win);
        x2rw_last_error_set(X2RW_ERROR_ACCESS_DENIED);
        return K32_INVALID_HANDLE;
    }

    uint32_t err = X2RW_ERROR_FILE_NOT_FOUND;
    FILE *fp = k32_fopen_win32(host, access, disp, &err);
    if (!fp) {
        x2rw_log("[k32] CreateFile('%s' -> '%s') falhou (erro %u)",
                 win, host, err);
        x2rw_last_error_set(err);
        return K32_INVALID_HANDLE;
    }
    k32_file_t *f = k32_file_alloc(fp);
    if (!f) {
        fclose(fp);
        x2rw_log("[k32] CreateFile: tabela de handles cheia (%d)", K32_MAX_FILES);
        x2rw_last_error_set(X2RW_ERROR_NOT_ENOUGH_MEMORY);
        return K32_INVALID_HANDLE;
    }
    x2rw_log("[k32] CreateFile('%s') = handle %08x", win, f->handle);
    (void)c;
    X2RW_OK();
    return f->handle;
}

/* ==================================================================== */
/* Tabela de buscas (FindFirstFileA/FindNextFileA)                      */
/* ==================================================================== */

typedef struct {
    int       used;
    uint32_t  handle;
    DIR      *d;
    char      pattern[260];
    char      dir[900];
} k32_find_t;

static k32_find_t g_finds[K32_MAX_FINDS];
static uint32_t   g_next_find = K32_HANDLE_FIND_BASE;

static k32_find_t *k32_find_find(uint32_t h) {
    if (h < K32_HANDLE_FIND_BASE) return NULL;
    for (int i = 0; i < K32_MAX_FINDS; i++)
        if (g_finds[i].used && g_finds[i].handle == h) return &g_finds[i];
    return NULL;
}

static k32_find_t *k32_find_alloc(void) {
    for (int i = 0; i < K32_MAX_FINDS; i++) {
        if (!g_finds[i].used) {
            memset(&g_finds[i], 0, sizeof(g_finds[i]));
            g_finds[i].used = 1;
            g_finds[i].handle = g_next_find++;
            return &g_finds[i];
        }
    }
    return NULL;
}

static void k32_find_release(k32_find_t *f) {
    if (!f) return;
    if (f->d) closedir(f->d);
    f->d = NULL;
    f->used = 0;
}

static void k32_join_path(char *dst, size_t cap, const char *dir, const char *name) {
    size_t n = strlen(dir);
    if (n + 1 + strlen(name) + 1 > cap) { dst[0] = 0; return; }
    memcpy(dst, dir, n);
    if (n && dst[n - 1] != '/') dst[n++] = '/';
    strcpy(dst + n, name);
}

/* Casamento de curinga do Win32: '*' e '?', sem diferenciar maiúsculas.
 * Não implementa o casamento por nome curto 8.3 (documentado). */
static int k32_wild_match(const char *pat, const char *name) {
    /* Quirk documentado do Win32: "*.*" casa com TODOS os arquivos, mesmo os
     * que não têm ponto no nome. Sem isto, uma enumeração do jogo com "*.*"
     * perderia arquivos. */
    if (pat[0] == '*' && pat[1] == '.' && pat[2] == '*' && pat[3] == 0)
        return 1;
    while (*pat) {
        if (*pat == '*') {
            while (*pat == '*') pat++;
            if (!*pat) return 1;
            for (; *name; name++)
                if (k32_wild_match(pat, name)) return 1;
            return k32_wild_match(pat, name);   /* '*' casando com o fim */
        }
        if (*pat == '?') {
            if (!*name) return 0;
            pat++; name++;
            continue;
        }
        if (!*name) return 0;
        if (k32_lower(*pat) != k32_lower(*name)) return 0;
        pat++; name++;
    }
    return *name == 0;
}

/* Preenche WIN32_FIND_DATAA (320 bytes) no guest. Devolve 0 se o arquivo
 * não puder ser consultado (nesse caso o chamador pula a entrada). */
static int k32_find_fill(x2rw_ctx *c, const k32_find_t *fd, const char *name,
                         uint32_t out_va) {
    char full[1200];
    struct stat st;
    uint8_t rec[320];

    k32_join_path(full, sizeof(full), fd->dir, name);
    if (!full[0]) return 0;
    if (stat(full, &st) != 0) return 0;

    memset(rec, 0, sizeof(rec));
    uint32_t attr = S_ISDIR(st.st_mode) ? K32_FILE_ATTRIBUTE_DIRECTORY
                                        : K32_FILE_ATTRIBUTE_ARCHIVE;
    if (!(st.st_mode & S_IWUSR)) attr |= K32_FILE_ATTRIBUTE_READONLY;
    k32_put_u32(rec + 0, attr);
    k32_put_u64(rec + 4, k32_unix_to_ft((int64_t)st.st_mtime));    /* criação  */
    k32_put_u64(rec + 12, k32_unix_to_ft((int64_t)st.st_atime));   /* acesso   */
    k32_put_u64(rec + 20, k32_unix_to_ft((int64_t)st.st_mtime));   /* escrita  */
    k32_put_u32(rec + 28, (uint32_t)((uint64_t)st.st_size >> 32));
    k32_put_u32(rec + 32, (uint32_t)st.st_size);
    /* 36/40: reservados; 304..317: nome curto 8.3 (não geramos) */
    strncpy((char *)rec + 44, name, 259);

    x2rw_mem_write(out_va, rec, sizeof(rec));
    return 1;
}

/* Avança até a próxima entrada que casa com o padrão. */
static int k32_find_step(x2rw_ctx *c, k32_find_t *fd, uint32_t out_va,
                         uint32_t *out_err) {
    struct dirent *de;
    while ((de = readdir(fd->d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (!k32_wild_match(fd->pattern, de->d_name)) continue;
        if (!k32_find_fill(c, fd, de->d_name, out_va)) continue;
        return 1;
    }
    *out_err = X2RW_ERROR_NO_MORE_FILES;
    return 0;
}

/* ==================================================================== */
/* Objetos sintéticos: mutex, evento, mapeamento, thread                */
/* ==================================================================== */

typedef struct {
    int      used;
    int      kind;
    uint32_t handle;
    /* mapeamento de arquivo */
    uint32_t file_handle;      /* 0 = anônimo, senão handle de arquivo      */
    uint32_t max_size;         /* tamanho máximo em bytes                   */
    uint32_t view;             /* último view mapeado (ver MapViewOfFile)   */
    uint32_t view_size;
    /* evento */
    int      manual_reset;
    int      signaled;
} k32_obj_t;

static k32_obj_t g_objs[K32_MAX_OBJECTS];
static uint32_t  g_next_obj = K32_HANDLE_OBJ_BASE;

static k32_obj_t *k32_obj_new(int kind) {
    for (int i = 0; i < K32_MAX_OBJECTS; i++) {
        if (!g_objs[i].used) {
            memset(&g_objs[i], 0, sizeof(g_objs[i]));
            g_objs[i].used = 1;
            g_objs[i].kind = kind;
            g_objs[i].handle = g_next_obj++;
            return &g_objs[i];
        }
    }
    return NULL;
}

static k32_obj_t *k32_obj_find(uint32_t h, int kind) {
    if (h < K32_HANDLE_OBJ_BASE) return NULL;
    for (int i = 0; i < K32_MAX_OBJECTS; i++)
        if (g_objs[i].used && g_objs[i].handle == h &&
            (kind == 0 || g_objs[i].kind == kind))
            return &g_objs[i];
    return NULL;
}

static void k32_obj_close(k32_obj_t *o) {
    if (!o) return;
    if (o->view) x2rw_heap_free(NULL, o->view);   /* x2rw_heap_free ignora c */
    memset(o, 0, sizeof(*o));
}

/* ==================================================================== */
/* Seções críticas: um pthread_mutex_t do host por VA do guest          */
/* ==================================================================== */

typedef struct {
    int             used;
    uint32_t        va;
    pthread_mutex_t m;
} k32_cs_t;

static k32_cs_t g_cs[K32_MAX_CS];

static k32_cs_t *k32_cs_find(uint32_t va, int create) {
    for (int i = 0; i < K32_MAX_CS; i++)
        if (g_cs[i].used && g_cs[i].va == va) return &g_cs[i];
    if (!create) return NULL;
    for (int i = 0; i < K32_MAX_CS; i++) {
        if (!g_cs[i].used) {
            pthread_mutexattr_t attr;
            pthread_mutexattr_init(&attr);
            /* CRITICAL_SECTION é recursiva por contrato (o dono pode entrar
             * de novo); sem isso o jogo trava no primeiro Enter aninhado. */
            pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
            if (pthread_mutex_init(&g_cs[i].m, &attr) != 0) {
                pthread_mutexattr_destroy(&attr);
                return NULL;
            }
            pthread_mutexattr_destroy(&attr);
            g_cs[i].used = 1;
            g_cs[i].va = va;
            return &g_cs[i];
        }
    }
    return NULL;
}

/* ==================================================================== */
/* TLS                                                                  */
/* ==================================================================== */

static uint8_t  g_tls_used[K32_TLS_SLOTS];
static uint32_t g_tls_value[K32_TLS_SLOTS];

/* ==================================================================== */
/* Variáveis de ambiente (bloco ANSI/UTF-16 montado no guest)           */
/* ==================================================================== */

typedef struct {
    int  used;
    char name[K32_ENV_NAME];
    char value[K32_ENV_VALUE];
} k32_env_t;

static k32_env_t g_envs[K32_MAX_ENV];
static int       g_env_ready = 0;
static uint32_t  g_env_block_a = 0;    /* cache dos blocos: liberados só
                                        * pelo bump allocator do guest      */
static uint32_t  g_env_block_w = 0;

static int k32_env_find(const char *name) {
    for (int i = 0; i < K32_MAX_ENV; i++)
        if (g_envs[i].used && k32_ci_eq(g_envs[i].name, name)) return i;
    return -1;
}

static int k32_env_set(const char *name, const char *value) {
    int idx = k32_env_find(name);
    if (!value) {
        if (idx >= 0) g_envs[idx].used = 0;
        return 1;
    }
    if (idx < 0) {
        for (int i = 0; i < K32_MAX_ENV; i++) {
            if (!g_envs[i].used) { idx = i; g_envs[i].used = 1; break; }
        }
    }
    if (idx < 0) return 0;
    k32_copy(g_envs[idx].name, sizeof(g_envs[idx].name), name);
    k32_copy(g_envs[idx].value, sizeof(g_envs[idx].value), value);
    return 1;
}

static void k32_env_defaults(void) {
    if (g_env_ready) return;
    g_env_ready = 1;
    k32_env_set("PATH", ".");
    k32_env_set("TEMP", ".");
    k32_env_set("TMP", ".");
    k32_env_set("SystemRoot", "C:\\WINDOWS");
    k32_env_set("windir", "C:\\WINDOWS");
    k32_env_set("OS", "Windows_NT");
    k32_env_set("PROCESSOR_ARCHITECTURE", "x86");
    k32_env_set("COMPUTERNAME", "X2RW");
    k32_env_set("USERNAME", "player");
}

static void k32_env_invalidate(void) {
    /* O heap do guest é um bump allocator: o bloco antigo fica como lixo
     * (poucos bytes) até o fim do processo. SetEnvironmentVariable é raro. */
    g_env_block_a = 0;
    g_env_block_w = 0;
}

/* Bloco ANSI: "NOME=VALOR\0"... + "\0" final. */
static k32_word k32i_env_build_a(x2rw_ctx *c) {
    if (g_env_block_a) return g_env_block_a;
    k32_env_defaults();
    uint32_t total = 1;
    for (int i = 0; i < K32_MAX_ENV; i++)
        if (g_envs[i].used)
            total += (uint32_t)strlen(g_envs[i].name) + 1u +
                     (uint32_t)strlen(g_envs[i].value) + 1u;
    uint32_t p = x2rw_heap_alloc(c, total);
    if (!p) return 0;
    uint32_t at = p;
    for (int i = 0; i < K32_MAX_ENV; i++) {
        if (!g_envs[i].used) continue;
        uint32_t n = (uint32_t)strlen(g_envs[i].name);
        x2rw_mem_write(at, g_envs[i].name, n); at += n;
        x2rw_write8(c, at++, '=');
        n = (uint32_t)strlen(g_envs[i].value);
        x2rw_mem_write(at, g_envs[i].value, n); at += n;
        x2rw_write8(c, at++, 0);
    }
    x2rw_write8(c, at, 0);
    g_env_block_a = p;
    return p;
}

/* Bloco UTF-16 (mesmo formato, unidades de 16 bits). */
static k32_word k32i_env_build_w(x2rw_ctx *c) {
    if (g_env_block_w) return g_env_block_w;
    k32_env_defaults();
    uint32_t units = 1;
    for (int i = 0; i < K32_MAX_ENV; i++)
        if (g_envs[i].used)
            units += (uint32_t)strlen(g_envs[i].name) +
                     (uint32_t)strlen(g_envs[i].value) + 2u;
    uint32_t p = x2rw_heap_alloc(c, units * 2u);
    if (!p) return 0;
    uint32_t at = p;
    for (int i = 0; i < K32_MAX_ENV; i++) {
        if (!g_envs[i].used) continue;
        for (const char *s = g_envs[i].name; *s; s++, at += 2u)
            x2rw_write16(c, at, (uint16_t)(uint8_t)*s);
        x2rw_write16(c, at, (uint16_t)'='); at += 2u;
        for (const char *s = g_envs[i].value; *s; s++, at += 2u)
            x2rw_write16(c, at, (uint16_t)(uint8_t)*s);
        x2rw_write16(c, at, 0); at += 2u;
    }
    x2rw_write16(c, at, 0);
    g_env_block_w = p;
    return p;
}

/* ==================================================================== */
/* INI (GetPrivateProfile*, WritePrivateProfileStringA)                 */
/* ==================================================================== */

static char g_ini_in[K32_INI_MAX];
static char g_ini_out[K32_INI_MAX];

/* Definido mais abaixo; usado por k32_ini_key(). */
static int k32_line_key_eq(const char *p, size_t n, const char *key);

/* Lê o arquivo INI inteiro para o buffer do host. Devolve o tamanho lido
 * (0 se o arquivo não existe ou não pôde ser lido). */
static size_t k32_ini_load(const char *win_path, char *buf, size_t cap) {
    char host[1024];
    if (!win_path || !win_path[0] || cap == 0) return 0;
    x2rw_win32_path_to_host(win_path, host, sizeof(host));
    if (!host[0]) return 0;
    FILE *fp = fopen(host, "rb");
    if (!fp) return 0;
    size_t n = fread(buf, 1, cap - 1, fp);
    fclose(fp);
    buf[n] = 0;
    return n;
}

/* Devolve o intervalo [begin,end) do corpo da seção, sem o cabeçalho. */
static int k32_ini_section(const char *buf, size_t len, const char *section,
                           size_t *out_begin, size_t *out_end) {
    size_t i = 0;
    while (i < len) {
        size_t ls = i;
        while (i < len && buf[i] != '\n') i++;
        size_t le = i;
        if (i < len) i++;
        const char *p = buf + ls;
        size_t n = le - ls;
        if (n && p[n - 1] == '\r') n--;              /* CRLF */
        while (n && (*p == ' ' || *p == '\t')) { p++; n--; }
        if (n >= 2 && p[0] == '[' && p[n - 1] == ']' &&
            k32_ci_eq_n(p + 1, n - 2, section)) {
            *out_begin = i;
            size_t j = i;
            while (j < len) {
                size_t ks = j;
                while (j < len && buf[j] != '\n') j++;
                if (j < len) j++;
                const char *q = buf + ks;
                size_t m = j - ks;
                while (m && (*q == ' ' || *q == '\t')) { q++; m--; }
                if (m && q[0] == '[') { j = ks; break; }
            }
            *out_end = j;
            return 1;
        }
    }
    return 0;
}

/* Procura "chave=valor" no intervalo e devolve o ponteiro do valor. */
static int k32_ini_key(const char *buf, size_t begin, size_t end,
                       const char *key, const char **out_val, size_t *out_len) {
    size_t i = begin;
    while (i < end) {
        size_t ls = i;
        while (i < end && buf[i] != '\n') i++;
        size_t le = i;
        if (i < end) i++;
        const char *p = buf + ls;
        size_t n = le - ls;
        if (n && p[n - 1] == '\r') n--;
        while (n && (*p == ' ' || *p == '\t')) { p++; n--; }
        if (!n || p[0] == ';' || p[0] == '#') continue;
        if (!k32_line_key_eq(p, n, key)) continue;
        size_t eq = 0;
        while (eq < n && p[eq] != '=') eq++;
        const char *v = p + eq + 1;
        size_t vn = n - eq - 1;
        while (vn && (*v == ' ' || *v == '\t')) { v++; vn--; }
        while (vn && (v[vn - 1] == ' ' || v[vn - 1] == '\t')) vn--;
        *out_val = v;
        *out_len = vn;
        return 1;
    }
    return 0;
}

/* Compara o nome da chave de uma linha "chave=valor" já sem espaços à
 * esquerda. Devolve 1 se a linha define `key`. */
static int k32_line_key_eq(const char *p, size_t n, const char *key) {
    size_t eq = 0;
    while (eq < n && p[eq] != '=') eq++;
    if (eq == 0 || eq >= n) return 0;
    size_t kn = eq;
    while (kn && (p[kn - 1] == ' ' || p[kn - 1] == '\t')) kn--;
    return k32_ci_eq_n(p, kn, key);
}

static size_t k32_app(char *out, size_t cap, size_t at, const char *s, size_t n) {
    if (at > cap || n > cap - at) return (size_t)-1;
    memcpy(out + at, s, n);
    return at + n;
}

static size_t k32_app_kv(char *out, size_t cap, size_t at, const char *key,
                         const char *value) {
    at = k32_app(out, cap, at, key, strlen(key));
    if (at == (size_t)-1) return at;
    at = k32_app(out, cap, at, "=", 1);
    if (at == (size_t)-1) return at;
    at = k32_app(out, cap, at, value, strlen(value));
    if (at == (size_t)-1) return at;
    return k32_app(out, cap, at, "\r\n", 2);
}

/* Reescreve o INI aplicando uma alteração. key==NULL apaga a seção inteira,
 * value==NULL apaga só a chave. Devolve o novo tamanho, ou -1 se não couber
 * no buffer de saída. */
static int k32_ini_rewrite(const char *in, size_t inlen, const char *section,
                           const char *key, const char *value,
                           char *out, size_t outcap) {
    size_t o = 0;
    size_t i = 0;
    int in_sec = 0, sec_seen = 0, done = 0;
    int del_sec = (key == NULL);
    int del_key = !del_sec && (value == NULL);
    int adding = !del_sec && !del_key;      /* só há 'value' neste caso */

    while (i < inlen) {
        size_t ls = i;
        while (i < inlen && in[i] != '\n') i++;
        size_t le = i;
        if (i < inlen) i++;
        const char *line = in + ls;
        size_t ln = i - ls;
        const char *p = line;
        size_t n = le - ls;
        if (n && p[n - 1] == '\r') n--;
        while (n && (*p == ' ' || *p == '\t')) { p++; n--; }
        int is_sec = (n >= 2 && p[0] == '[' && p[n - 1] == ']');

        if (is_sec) {
            int ours = k32_ci_eq_n(p + 1, n - 2, section);
            if (in_sec && !done && adding) {            /* fim da nossa seção */
                o = k32_app_kv(out, outcap, o, key, value);
                if (o == (size_t)-1) return -1;
                done = 1;
            }
            in_sec = ours;
            if (ours) {
                sec_seen = 1;
                if (del_sec) {                          /* remove a seção toda */
                    done = 1;
                    continue;
                }
            }
            o = k32_app(out, outcap, o, line, ln);
            if (o == (size_t)-1) return -1;
            continue;
        }
        if (in_sec) {
            if (del_sec) continue;                      /* corpo da seção fora */
            if (!done && k32_line_key_eq(p, n, key)) {
                if (!del_key) {
                    o = k32_app_kv(out, outcap, o, key, value);
                    if (o == (size_t)-1) return -1;
                }
                done = 1;
                continue;                               /* substitui/remove */
            }
        }
        o = k32_app(out, outcap, o, line, ln);
        if (o == (size_t)-1) return -1;
    }
    if (!done && adding) {
        if (!sec_seen) {
            o = k32_app(out, outcap, o, "[", 1);
            if (o == (size_t)-1) return -1;
            o = k32_app(out, outcap, o, section, strlen(section));
            if (o == (size_t)-1) return -1;
            o = k32_app(out, outcap, o, "]\r\n", 3);
            if (o == (size_t)-1) return -1;
        }
        o = k32_app_kv(out, outcap, o, key, value);
        if (o == (size_t)-1) return -1;
    }
    return (int)o;
}

/* ==================================================================== */
/* Módulos: nome <-> handle sintético                                   */
/* ==================================================================== */

typedef struct {
    const char *name;
    uint32_t    handle;
} k32_mod_t;

static const k32_mod_t g_mods[] = {
    { "wolvie.exe",   X2RW_BASE_WOLVIE },
    { "kernel32.dll", 0x70000000u },
    { "user32.dll",   0x70001000u },
    { "gdi32.dll",    0x70002000u },
    { "advapi32.dll", 0x70003000u },
    { "dsound.dll",   0x70004000u },
    { "winmm.dll",    0x70005000u },
    { "dinput8.dll",  0x70006000u },
    { "d3d8.dll",     0x70007000u },
    { "binkw32.dll",  X2RW_BASE_BINK }
};

#define K32_N_MODS ((int)(sizeof(g_mods) / sizeof(g_mods[0])))

typedef struct {
    int      used;
    uint32_t handle;
    char     name[64];
} k32_load_t;

static k32_load_t g_loads[K32_MAX_LOADS];
static uint32_t   g_next_load = 0x70010000u;

/* Aceita "kernel32", "KERNEL32.dll" e "C:\...\kernel32.dll". */
static int k32_module_match(const char *cand, const char *query) {
    const char *q = query;
    for (const char *p = query; *p; p++)
        if (*p == '\\' || *p == '/') q = p + 1;
    if (!*q) return 0;
    const char *cname = cand;
    for (;;) {
        char a = k32_lower(*cname), b = k32_lower(*q);
        if (b == 0) return a == 0 || a == '.';
        if (a != b) return 0;
        cname++; q++;
    }
}

static int k32_module_handle(const char *name, uint32_t *out) {
    for (int i = 0; i < K32_N_MODS; i++)
        if (k32_module_match(g_mods[i].name, name)) {
            *out = g_mods[i].handle;
            return 1;
        }
    for (int i = 0; i < K32_MAX_LOADS; i++)
        if (g_loads[i].used && k32_module_match(g_loads[i].name, name)) {
            *out = g_loads[i].handle;
            return 1;
        }
    return 0;
}

static int k32_module_name(uint32_t handle, char *out, size_t cap) {
    for (int i = 0; i < K32_N_MODS; i++)
        if (g_mods[i].handle == handle) {
            k32_copy(out, cap, g_mods[i].name);
            return 1;
        }
    for (int i = 0; i < K32_MAX_LOADS; i++)
        if (g_loads[i].used && g_loads[i].handle == handle) {
            k32_copy(out, cap, g_loads[i].name);
            return 1;
        }
    return 0;
}

/* ==================================================================== */
/* HANDLERS — na MESMA ORDEM da lista mestra X2RW_SHIMS_KERNEL32        */
/* ==================================================================== */

/* Nota geral dos handlers abaixo: as APIs que o Win32 declara `void`
 * devolvem 0 (o jogo não lê eax). Todas as que devolvem BOOL devolvem 1/0 e
 * nunca devolvem um valor inventado sem antes logar e/ou setar o last error. */

uint32_t k32_CloseHandle(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    uint32_t h = args[0];

    k32_file_t *f = k32_file_find(h);
    if (f) {
        k32_file_close(f);
        X2RW_OK();
        return 1u;
    }
    k32_obj_t *o = k32_obj_find(h, 0);
    if (o) {
        k32_obj_close(o);
        X2RW_OK();
        return 1u;
    }
    k32_find_t *fd = k32_find_find(h);
    if (fd) {
        k32_find_release(fd);
        X2RW_OK();
        return 1u;
    }
    /* Pseudo-handles do processo/thread corrente e os três handles padrão
     * não são fecháveis de verdade, mas o Win32 os aceita sem erro. */
    if (h == K32_STD_IN || h == K32_STD_OUT || h == K32_STD_ERR ||
        h == K32_PROCESS_HANDLE || h == K32_THREAD_HANDLE) {
        X2RW_OK();
        return 1u;
    }
    x2rw_log("[k32] CloseHandle: handle desconhecido %08x", h);
    X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
}

uint32_t k32_CreateDirectoryA(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    char win[1024], host[1024];
    k32_str(c, args[0], win, sizeof(win));
    if (!win[0]) X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    x2rw_win32_path_to_host(win, host, sizeof(host));
    if (!host[0]) X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    if (mkdir(host, 0777) != 0) {
        uint32_t e = (errno == EEXIST) ? X2RW_ERROR_ALREADY_EXISTS
                                       : k32i_errno_to_win(errno);
        x2rw_log("[k32] CreateDirectoryA('%s') falhou (erro %u)", win, e);
        x2rw_last_error_set(e);
        return 0u;
    }
    x2rw_log("[k32] CreateDirectoryA('%s')", win);
    X2RW_OK();
    return 1u;
}

uint32_t k32_CreateEventA(x2rw_ctx *c, uint32_t *args, int nargs) {
    k32_obj_t *o = k32_obj_new(K32_OBJ_EVENT);
    if (!o) X2RW_FAIL(c, X2RW_ERROR_NOT_ENOUGH_MEMORY);
    o->manual_reset = args[1] ? 1 : 0;
    o->signaled = args[2] ? 1 : 0;
    char name[128] = "";
    if (nargs >= 4 && args[3]) k32_str(c, args[3], name, sizeof(name));
    x2rw_log("[k32] CreateEventA(manual=%u inicial=%u nome='%s') = %08x",
             (unsigned)o->manual_reset, (unsigned)o->signaled, name, o->handle);
    X2RW_OK();
    return o->handle;
}

uint32_t k32_CreateFileA(x2rw_ctx *c, uint32_t *args, int nargs) {
    char win[1024];
    if (nargs < 7) X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    k32_str(c, args[0], win, sizeof(win));
    return k32i_create_file_impl(c, win, args[1], args[4]);
}

uint32_t k32_CreateFileMappingA(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    if (args[3] != 0) {
        x2rw_log("[k32] CreateFileMappingA: tamanho > 4 GiB não suportado");
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    uint32_t size = args[4];
    uint32_t file_handle = 0;

    if (args[0] != K32_INVALID_HANDLE) {
        k32_file_t *f = k32_file_find(args[0]);
        if (!f) {
            x2rw_log("[k32] CreateFileMappingA: arquivo %08x inválido", args[0]);
            X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
        }
        file_handle = args[0];
        if (size == 0) {                    /* 0 = tamanho do arquivo */
            long cur = ftell(f->fp);
            if (cur >= 0 && fseek(f->fp, 0, SEEK_END) == 0) {
                long end = ftell(f->fp);
                if (end > 0) size = (uint32_t)end;
                fseek(f->fp, cur, SEEK_SET);
            }
        }
    }
    if (size == 0) size = 0x1000;           /* anônimo de tamanho 0: 1 página */

    k32_obj_t *o = k32_obj_new(K32_OBJ_MAPPING);
    if (!o) X2RW_FAIL(c, X2RW_ERROR_NOT_ENOUGH_MEMORY);
    o->file_handle = file_handle;
    o->max_size = size;
    o->view = 0;
    o->view_size = 0;
    char name[128] = "";
    if (args[5]) k32_str(c, args[5], name, sizeof(name));
    x2rw_log("[k32] CreateFileMappingA(arquivo=%08x %u bytes nome='%s') = %08x",
             file_handle, size, name, o->handle);
    X2RW_OK();
    return o->handle;
}

uint32_t k32_CreateFileW(x2rw_ctx *c, uint32_t *args, int nargs) {
    char win[1024];
    if (nargs < 7) X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    k32_wstr(c, args[0], win, sizeof(win));
    return k32i_create_file_impl(c, win, args[1], args[4]);
}

uint32_t k32_CreateMutexA(x2rw_ctx *c, uint32_t *args, int nargs) {
    k32_obj_t *o = k32_obj_new(K32_OBJ_MUTEX);
    if (!o) X2RW_FAIL(c, X2RW_ERROR_NOT_ENOUGH_MEMORY);
    char name[128] = "";
    if (nargs >= 3 && args[2]) k32_str(c, args[2], name, sizeof(name));
    x2rw_log("[k32] CreateMutexA(dono inicial=%u nome='%s') = %08x",
             (unsigned)args[1], name, o->handle);
    X2RW_OK();
    return o->handle;
}

uint32_t k32_CreateThread(x2rw_ctx *c, uint32_t *args, int nargs) {
    k32_obj_t *o = k32_obj_new(K32_OBJ_THREAD);
    if (!o) X2RW_FAIL(c, X2RW_ERROR_NOT_ENOUGH_MEMORY);
    /* STUB honesto: nesta fase o port roda o jogo em uma única thread. O
     * handle existe para o jogo não desreferenciar NULL, mas o corpo em
     * lpStartAddress NÃO é executado — está registrado no log. */
    x2rw_log("[k32] CreateThread: port single-thread — o corpo em %08x NÃO "
             "será executado (handle %08x devolvido)",
             (nargs >= 3 ? args[2] : 0u), o->handle);
    if (nargs >= 6 && args[5]) x2rw_write32(c, args[5], K32_MAIN_THREAD_ID);
    X2RW_OK();
    return o->handle;
}

uint32_t k32_DeleteCriticalSection(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    k32_cs_t *cs = k32_cs_find(args[0], 0);
    if (!cs) {
        x2rw_log("[k32] DeleteCriticalSection(%08x): seção nunca inicializada",
                 args[0]);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
    }
    pthread_mutex_destroy(&cs->m);
    cs->used = 0;
    X2RW_OK();
    return 0u;
}

uint32_t k32_DeleteFileA(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    char win[1024], host[1024];
    k32_str(c, args[0], win, sizeof(win));
    if (!win[0]) X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    x2rw_win32_path_to_host(win, host, sizeof(host));
    if (unlink(host) != 0) {
        uint32_t e = k32i_errno_to_win(errno);
        x2rw_log("[k32] DeleteFileA('%s') falhou (erro %u)", win, e);
        x2rw_last_error_set(e);
        return 0u;
    }
    X2RW_OK();
    return 1u;
}

uint32_t k32_EnterCriticalSection(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    if (!k32_cs_find(args[0], 0)) {
        /* O contrato do Win32 exige InitializeCriticalSection antes; o jogo
         * pode ter perdido a chamada (ou ela veio de outro módulo). Criar sob
         * demanda mantém a semântica de exclusão mútua — e fica registrado. */
        x2rw_log("[k32] EnterCriticalSection(%08x): sem "
                 "InitializeCriticalSection — criando mutex sob demanda",
                 args[0]);
    }
    k32_cs_t *cs = k32_cs_find(args[0], 1);
    if (!cs) {
        x2rw_log("[k32] EnterCriticalSection(%08x): tabela cheia", args[0]);
        X2RW_FAIL(c, X2RW_ERROR_NOT_ENOUGH_MEMORY);
    }
    pthread_mutex_lock(&cs->m);
    X2RW_OK();
    return 0u;
}

uint32_t k32_ExitProcess(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    /* NÃO chamamos exit(): o host é quem encerra, ao ver o fault. */
    x2rw_log("[k32] ExitProcess(%u)", (unsigned)args[0]);
    x2rw_fault(c, X2RW_FAULT_UNIMPL_API, 0, "ExitProcess");
    return 0u;
}

uint32_t k32_ExitThread(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    /* O port não tem threads reais (ver CreateThread): não existe "esta
     * thread" para encerrar, então falhamos ALTO em vez de seguir com o
     * estado errado. */
    x2rw_log("[k32] ExitThread(%u): sem threads no port — fault",
             (unsigned)args[0]);
    x2rw_fault(c, X2RW_FAULT_UNIMPL_API, 0, "ExitThread");
    return 0u;
}

uint32_t k32_FatalAppExitA(x2rw_ctx *c, uint32_t *args, int nargs) {
    char msg[512];
    (void)nargs;
    k32_str(c, args[1], msg, sizeof(msg));
    x2rw_log("[k32] FatalAppExitA(%u, '%s')", (unsigned)args[0], msg);
    x2rw_fault(c, X2RW_FAULT_UNIMPL_API, 0, "FatalAppExitA");
    return 0u;
}

uint32_t k32_FileTimeToLocalFileTime(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    uint64_t ft = k32_read_ft(c, args[0]);
    int64_t off = k32_tz_offset();
    uint64_t local = (uint64_t)((int64_t)ft + off * 10000000LL);
    k32_write_ft(c, args[1], local);
    X2RW_OK();
    return 1u;
}

uint32_t k32_FileTimeToSystemTime(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    uint64_t ft = k32_read_ft(c, args[0]);
    struct tm t;
    int ms = 0;
    if (!k32_ft_to_parts(ft, &t, &ms)) {
        x2rw_log("[k32] FileTimeToSystemTime: FILETIME fora de faixa (%016llx)",
                 (unsigned long long)ft);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    k32_write_systime(c, args[1], &t, ms);
    X2RW_OK();
    return 1u;
}

uint32_t k32_FindClose(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    k32_find_t *fd = k32_find_find(args[0]);
    if (!fd) {
        x2rw_log("[k32] FindClose: handle inválido %08x", args[0]);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
    }
    k32_find_release(fd);
    X2RW_OK();
    return 1u;
}

uint32_t k32_FindFirstFileA(x2rw_ctx *c, uint32_t *args, int nargs) {
    char win[1024], dirwin[1024], hostdir[1024];
    (void)nargs;
    k32_str(c, args[0], win, sizeof(win));
    if (!win[0] || !args[1]) {
        x2rw_log("[k32] FindFirstFileA: padrão '%s' ou buffer nulo", win);
        x2rw_last_error_set(X2RW_ERROR_INVALID_PARAMETER);
        return K32_INVALID_HANDLE;
    }
    /* Separa diretório e padrão no último separador. */
    const char *pat = win;
    for (const char *p = win; *p; p++)
        if (*p == '\\' || *p == '/') pat = p + 1;
    size_t dn = (size_t)(pat - win);
    if (dn >= sizeof(dirwin)) dn = sizeof(dirwin) - 1;
    memcpy(dirwin, win, dn);
    dirwin[dn] = 0;
    while (dn > 0 && (dirwin[dn - 1] == '\\' || dirwin[dn - 1] == '/'))
        dirwin[--dn] = 0;

    if (dn == 0) {
        k32_copy(hostdir, sizeof(hostdir), ".");
    } else {
        x2rw_win32_path_to_host(dirwin, hostdir, sizeof(hostdir));
        if (!hostdir[0]) k32_copy(hostdir, sizeof(hostdir), ".");
    }

    DIR *d = opendir(hostdir);
    if (!d) {
        uint32_t e = k32i_errno_to_win(errno);
        x2rw_log("[k32] FindFirstFileA('%s'): opendir('%s') falhou (erro %u)",
                 win, hostdir, e);
        x2rw_last_error_set(e);
        return K32_INVALID_HANDLE;
    }
    k32_find_t *fd = k32_find_alloc();
    if (!fd) {
        closedir(d);
        x2rw_log("[k32] FindFirstFileA: tabela de buscas cheia");
        x2rw_last_error_set(X2RW_ERROR_NOT_ENOUGH_MEMORY);
        return K32_INVALID_HANDLE;
    }
    fd->d = d;
    k32_copy(fd->dir, sizeof(fd->dir), hostdir);
    k32_copy(fd->pattern, sizeof(fd->pattern), pat);

    uint32_t err = X2RW_ERROR_NO_MORE_FILES;
    if (!k32_find_step(c, fd, args[1], &err)) {
        k32_find_release(fd);
        x2rw_log("[k32] FindFirstFileA('%s'): nenhum arquivo", win);
        x2rw_last_error_set(err);
        return K32_INVALID_HANDLE;
    }
    X2RW_OK();
    return fd->handle;
}

uint32_t k32_FindNextFileA(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    k32_find_t *fd = k32_find_find(args[0]);
    if (!fd || !args[1]) {
        x2rw_log("[k32] FindNextFileA: handle %08x inválido", args[0]);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
    }
    uint32_t err = X2RW_ERROR_NO_MORE_FILES;
    if (!k32_find_step(c, fd, args[1], &err)) {
        x2rw_last_error_set(err);
        return 0u;
    }
    X2RW_OK();
    return 1u;
}

/* FindResourceA/W — PARCIAL: as seções de recurso do Wolvie.exe não estão
 * disponíveis no build Android (só o código lifted é embarcado), então não há
 * o que devolver. Falhamos ALTO com erro explícito em vez de inventar um
 * HRSRC que o LockResource depois desreferenciaria. */
uint32_t k32_FindResourceA(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)args; (void)nargs;
    x2rw_log("[k32] FindResourceA: recursos de PE não existem neste port");
    X2RW_FAIL(c, X2RW_ERROR_CALL_NOT_IMPLEMENTED);
}

uint32_t k32_FindResourceW(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)args; (void)nargs;
    x2rw_log("[k32] FindResourceW: recursos de PE não existem neste port");
    X2RW_FAIL(c, X2RW_ERROR_CALL_NOT_IMPLEMENTED);
}

uint32_t k32_FlushConsoleInputBuffer(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)args; (void)nargs;
    /* PARCIAL: não há console Win32 no port; a chamada é benigna. */
    static int logged = 0;
    if (!logged++) x2rw_log("[k32] FlushConsoleInputBuffer: sem console — no-op");
    X2RW_OK();
    return 1u;
}

uint32_t k32_FlushFileBuffers(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    k32_file_t *f = k32_file_find(args[0]);
    if (!f) {
        if (args[0] == K32_STD_OUT || args[0] == K32_STD_ERR ||
            args[0] == K32_STD_IN) {
            X2RW_OK();
            return 1u;
        }
        x2rw_log("[k32] FlushFileBuffers: handle inválido %08x", args[0]);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
    }
    if (fflush(f->fp) != 0) {
        x2rw_log("[k32] FlushFileBuffers(handle %08x): fflush falhou", args[0]);
        X2RW_FAIL(c, X2RW_ERROR_ACCESS_DENIED);
    }
    X2RW_OK();
    return 1u;
}

uint32_t k32_FreeEnvironmentStringsA(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    /* O bloco veio de GetEnvironmentStrings, que o aloca no heap do guest. */
    if (args[0] && args[0] == g_env_block_a) {
        x2rw_heap_free(c, args[0]);
        g_env_block_a = 0;
    }
    X2RW_OK();
    return 1u;
}

uint32_t k32_FreeEnvironmentStringsW(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    if (args[0] && args[0] == g_env_block_w) {
        x2rw_heap_free(c, args[0]);
        g_env_block_w = 0;
    }
    X2RW_OK();
    return 1u;
}

uint32_t k32_FreeLibrary(x2rw_ctx *c, uint32_t *args, int nargs) {
    char name[64];
    (void)nargs;
    /* REAL no sentido possível: nenhuma DLL do host foi carregada (só os
     * shims), então liberar é só largar o handle sintético. */
    if (k32_module_name(args[0], name, sizeof(name)))
        x2rw_log("[k32] FreeLibrary('%s' handle %08x): nenhuma DLL do host "
                 "foi carregada", name, args[0]);
    else
        x2rw_log("[k32] FreeLibrary(handle desconhecido %08x)", args[0]);
    X2RW_OK();
    return 1u;
}

uint32_t k32_GetCommandLineA(x2rw_ctx *c, uint32_t *args, int nargs) {
    static uint32_t cmdline = 0;
    (void)args; (void)nargs;
    if (!cmdline) cmdline = x2rw_guest_alloc_str(c, "Wolvie.exe");
    if (!cmdline) X2RW_FAIL(c, X2RW_ERROR_NOT_ENOUGH_MEMORY);
    X2RW_OK();
    return cmdline;
}

uint32_t k32_GetConsoleMode(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    if (!args[1]) X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    /* PARCIAL: modos plausíveis de um console texto (entrada com eco/linha,
     * saída processada). Nada é lido de verdade. */
    x2rw_write32(c, args[1], (args[0] == K32_STD_IN) ? 7u : 3u);
    X2RW_OK();
    return 1u;
}

uint32_t k32_GetConsoleScreenBufferInfo(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    if (!args[1]) X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    /* CONSOLE_SCREEN_BUFFER_INFO (22 bytes). A geometria é derivada da
     * superfície de render: não há console real. */
    int cols = (int)(g_render_w / 8);
    int rows = (int)(g_render_h / 16);
    if (cols < 1) cols = 80;
    if (rows < 1) rows = 25;
    uint32_t p = args[1];
    x2rw_write16(c, p + 0u,  (uint16_t)cols);
    x2rw_write16(c, p + 2u,  (uint16_t)rows);
    x2rw_write16(c, p + 4u,  0u);            /* cursor X */
    x2rw_write16(c, p + 6u,  0u);            /* cursor Y */
    x2rw_write16(c, p + 8u,  0x07u);         /* atributo: cinza sobre preto */
    x2rw_write16(c, p + 10u, 0);             /* srWindow.Left   */
    x2rw_write16(c, p + 12u, 0);             /* srWindow.Top    */
    x2rw_write16(c, p + 14u, (uint16_t)(cols - 1));
    x2rw_write16(c, p + 16u, (uint16_t)(rows - 1));
    x2rw_write16(c, p + 18u, (uint16_t)cols);
    x2rw_write16(c, p + 20u, (uint16_t)rows);
    X2RW_OK();
    return 1u;
}

uint32_t k32_GetCurrentDirectoryA(x2rw_ctx *c, uint32_t *args, int nargs) {
    char cwd[1024];
    (void)nargs;
    if (!getcwd(cwd, sizeof(cwd))) {
        x2rw_log("[k32] GetCurrentDirectoryA: getcwd falhou (errno=%d)", errno);
        X2RW_FAIL(c, X2RW_ERROR_PATH_NOT_FOUND);
    }
    size_t n = strlen(cwd);
    if (!args[1] || n + 1 > args[0]) {
        x2rw_log("[k32] GetCurrentDirectoryA: buffer pequeno (%u < %u)",
                 (unsigned)args[0], (unsigned)(n + 1));
        x2rw_last_error_set(X2RW_ERROR_INSUFFICIENT_BUFFER);
        return 0u;
    }
    x2rw_mem_write(args[1], cwd, n);
    x2rw_write8(c, args[1] + (uint32_t)n, 0);
    X2RW_OK();
    return (uint32_t)n;
}

uint32_t k32_GetCurrentProcess(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)args; (void)nargs;
    X2RW_OK();
    return K32_PROCESS_HANDLE;      /* (HANDLE)-1, como no Win32 */
}

uint32_t k32_GetCurrentProcessId(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)args; (void)nargs;
    (void)c;
    return (uint32_t)getpid();
}

uint32_t k32_GetCurrentThread(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)args; (void)nargs;
    X2RW_OK();
    return K32_THREAD_HANDLE;       /* (HANDLE)-2, como no Win32 */
}

uint32_t k32_GetCurrentThreadId(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)args; (void)nargs; (void)c;
    /* Uma thread só (ver CreateThread): id fixo e estável. */
    return K32_MAIN_THREAD_ID;
}

uint32_t k32_GetDiskFreeSpaceExA(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    struct statvfs st;
    if (statvfs(".", &st) != 0) {
        x2rw_log("[k32] GetDiskFreeSpaceExA: statvfs falhou (errno=%d)", errno);
        X2RW_FAIL(c, X2RW_ERROR_PATH_NOT_FOUND);
    }
    uint64_t bs = st.f_frsize ? (uint64_t)st.f_frsize : (uint64_t)st.f_bsize;
    if (!bs) bs = 4096u;
    uint64_t avail = (uint64_t)st.f_bavail * bs;
    uint64_t total = (uint64_t)st.f_blocks * bs;
    uint64_t total_free = (uint64_t)st.f_bfree * bs;
    if (args[1]) x2rw_write64(c, args[1], avail);
    if (args[2]) x2rw_write64(c, args[2], total);
    if (args[3]) x2rw_write64(c, args[3], total_free);
    X2RW_OK();
    return 1u;
}

uint32_t k32_GetEnvironmentStrings(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)args; (void)nargs;
    uint32_t p = k32i_env_build_a(c);
    if (!p) X2RW_FAIL(c, X2RW_ERROR_NOT_ENOUGH_MEMORY);
    X2RW_OK();
    return p;
}

uint32_t k32_GetEnvironmentStringsW(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)args; (void)nargs;
    uint32_t p = k32i_env_build_w(c);
    if (!p) X2RW_FAIL(c, X2RW_ERROR_NOT_ENOUGH_MEMORY);
    X2RW_OK();
    return p;
}

uint32_t k32_GetEnvironmentVariableA(x2rw_ctx *c, uint32_t *args, int nargs) {
    char name[K32_ENV_NAME];
    (void)nargs;
    k32_str(c, args[0], name, sizeof(name));
    k32_env_defaults();
    int idx = k32_env_find(name);
    if (idx < 0) {
        x2rw_log("[k32] GetEnvironmentVariableA('%s'): não existe", name);
        X2RW_FAIL(c, X2RW_ERROR_ENVVAR_NOT_FOUND);
    }
    size_t vlen = strlen(g_envs[idx].value);
    if (!args[1] || args[2] == 0) {
        X2RW_OK();
        return (uint32_t)(vlen + 1);      /* consulta de tamanho */
    }
    if (vlen + 1 > args[2]) {             /* como o Win32: devolve o preciso */
        X2RW_OK();
        return (uint32_t)(vlen + 1);
    }
    x2rw_mem_write(args[1], g_envs[idx].value, vlen);
    x2rw_write8(c, args[1] + (uint32_t)vlen, 0);
    X2RW_OK();
    return (uint32_t)vlen;
}

uint32_t k32_GetFileSize(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    k32_file_t *f = k32_file_find(args[0]);
    if (!f) {
        x2rw_log("[k32] GetFileSize: handle inválido %08x", args[0]);
        x2rw_last_error_set(X2RW_ERROR_INVALID_HANDLE);
        return K32_INVALID_HANDLE;
    }
    long cur = ftell(f->fp);
    if (cur < 0 || fseek(f->fp, 0, SEEK_END) != 0) {
        x2rw_last_error_set(X2RW_ERROR_ACCESS_DENIED);
        return K32_INVALID_HANDLE;
    }
    long size = ftell(f->fp);
    fseek(f->fp, cur, SEEK_SET);          /* GetFileSize não move o ponteiro */
    if (size < 0) {
        x2rw_last_error_set(X2RW_ERROR_ACCESS_DENIED);
        return K32_INVALID_HANDLE;
    }
    if (args[1]) x2rw_write32(c, args[1], (uint32_t)((uint64_t)size >> 32));
    X2RW_OK();
    return (uint32_t)size;
}

uint32_t k32_GetFileType(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    if (args[0] == K32_STD_IN || args[0] == K32_STD_OUT || args[0] == K32_STD_ERR) {
        X2RW_OK();
        return K32_FILE_TYPE_CHAR;
    }
    if (k32_file_find(args[0])) {
        X2RW_OK();
        return K32_FILE_TYPE_DISK;
    }
    x2rw_log("[k32] GetFileType: handle inválido %08x", args[0]);
    x2rw_last_error_set(X2RW_ERROR_INVALID_HANDLE);
    return K32_FILE_TYPE_UNKNOWN;
}

uint32_t k32_GetLastError(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c; (void)args; (void)nargs;
    return x2rw_last_error_get();         /* não altera o erro (é o getter) */
}

uint32_t k32_GetLocalTime(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    struct timespec ts;
    struct tm t;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || !localtime_r(&ts.tv_sec, &t)) {
        x2rw_log("[k32] GetLocalTime: relógio indisponível");
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    k32_write_systime(c, args[0], &t, (int)(ts.tv_nsec / 1000000));
    X2RW_OK();
    return 0u;
}

uint32_t k32_GetModuleFileNameA(x2rw_ctx *c, uint32_t *args, int nargs) {
    static const char *exe = "Wolvie.exe";
    (void)nargs;
    size_t n = strlen(exe);
    if (!args[1] || args[2] == 0) {
        x2rw_log("[k32] GetModuleFileNameA: buffer nulo/0");
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    if (n + 1 > args[2]) {                /* trunca com NUL, como o Win32 */
        x2rw_mem_write(args[1], exe, args[2] - 1);
        x2rw_write8(c, args[1] + args[2] - 1, 0);
        x2rw_log("[k32] GetModuleFileNameA: buffer de %u truncou o nome",
                 (unsigned)args[2]);
        x2rw_last_error_set(X2RW_ERROR_INSUFFICIENT_BUFFER);
        return args[2];
    }
    x2rw_mem_write(args[1], exe, n);
    x2rw_write8(c, args[1] + (uint32_t)n, 0);
    X2RW_OK();
    return (uint32_t)n;
}

uint32_t k32_GetModuleHandleA(x2rw_ctx *c, uint32_t *args, int nargs) {
    char name[256];
    uint32_t handle = 0;
    (void)nargs;
    if (args[0] == 0) {                   /* NULL = o próprio executável */
        X2RW_OK();
        return X2RW_BASE_WOLVIE;
    }
    k32_str(c, args[0], name, sizeof(name));
    if (k32_module_handle(name, &handle)) {
        X2RW_OK();
        return handle;
    }
    x2rw_log("[k32] GetModuleHandleA('%s'): módulo não está carregado", name);
    X2RW_FAIL(c, X2RW_ERROR_MOD_NOT_FOUND);
}

uint32_t k32_GetNumberOfConsoleInputEvents(x2rw_ctx *c, uint32_t *args,
                                           int nargs) {
    (void)nargs;
    /* PARCIAL: sem console de entrada; o jogo vê "nenhum evento". */
    if (!args[1]) X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    x2rw_write32(c, args[1], 0u);
    X2RW_OK();
    return 1u;
}

uint32_t k32_GetPrivateProfileIntA(x2rw_ctx *c, uint32_t *args, int nargs) {
    char sec[128], key[128], path[1024], tmp[64];
    (void)c; (void)nargs;
    k32_str(c, args[0], sec, sizeof(sec));
    k32_str(c, args[1], key, sizeof(key));
    k32_str(c, args[3], path, sizeof(path));
    if (!sec[0] || !key[0]) return args[2];

    size_t n = k32_ini_load(path, g_ini_in, sizeof(g_ini_in));
    if (!n) return args[2];
    size_t b = 0, e = 0;
    if (!k32_ini_section(g_ini_in, n, sec, &b, &e)) return args[2];
    const char *v = NULL;
    size_t vn = 0;
    if (!k32_ini_key(g_ini_in, b, e, key, &v, &vn)) return args[2];
    size_t m = vn < sizeof(tmp) - 1 ? vn : sizeof(tmp) - 1;
    memcpy(tmp, v, m);
    tmp[m] = 0;
    return (uint32_t)(int32_t)strtol(tmp, NULL, 10);
}

uint32_t k32_GetPrivateProfileStringA(x2rw_ctx *c, uint32_t *args, int nargs) {
    char sec[128], key[128], def[512], path[1024];
    (void)nargs;
    k32_str(c, args[0], sec, sizeof(sec));
    k32_str(c, args[1], key, sizeof(key));
    k32_str(c, args[2], def, sizeof(def));
    k32_str(c, args[5], path, sizeof(path));

    const char *val = def;
    size_t vlen = strlen(def);
    if (sec[0] && key[0] && path[0]) {
        size_t n = k32_ini_load(path, g_ini_in, sizeof(g_ini_in));
        if (n) {
            size_t b = 0, e = 0;
            if (k32_ini_section(g_ini_in, n, sec, &b, &e)) {
                const char *v = NULL;
                size_t vn = 0;
                if (k32_ini_key(g_ini_in, b, e, key, &v, &vn)) {
                    val = v;
                    vlen = vn;
                }
            }
        }
    }
    if (!args[3] || args[4] == 0) {
        X2RW_OK();
        return (uint32_t)(vlen + 1);      /* consulta de tamanho */
    }
    if (vlen + 1 > args[4]) vlen = args[4] - 1;
    if (vlen) x2rw_mem_write(args[3], val, vlen);
    x2rw_write8(c, args[3] + (uint32_t)vlen, 0);
    X2RW_OK();
    return (uint32_t)vlen;
}

uint32_t k32_GetProcAddress(x2rw_ctx *c, uint32_t *args, int nargs) {
    char dll[64];
    char name[192];
    (void)nargs;
    name[0] = 0;
    if (!k32_module_name(args[0], dll, sizeof(dll))) {
        x2rw_log("[k32] GetProcAddress: módulo %08x desconhecido", args[0]);
        x2rw_last_error_set(X2RW_ERROR_INVALID_HANDLE);
        return 0u;
    }
    uint32_t proc = args[1];
    uint32_t out = 0;
    if (proc < 0x10000u) {
        /* O Win32 trata um "nome" com a palavra alta zerada como ordinal. */
        x2rw_log("[k32] GetProcAddress(%s, #%u): import por ordinal", dll, proc);
        out = x2rw_import_lookup_ordinal(dll, proc);
    } else {
        k32_str(c, proc, name, sizeof(name));
        if (!name[0]) {
            x2rw_log("[k32] GetProcAddress(%s, nome vazio)", dll);
            x2rw_last_error_set(X2RW_ERROR_INVALID_PARAMETER);
            return 0u;
        }
        /* O VA devolvido é um thunk de X2RW_EXPORT_BASE que o despachante
         * reconhece (x2rw_imports.c). Exports fora do registro falham. */
        out = x2rw_import_lookup_export(dll, name);
    }
    if (!out) {
        x2rw_log("[k32] GetProcAddress(%s!%s): export fora do registro de "
                 "shims (0x%08x)", dll, (proc < 0x10000u ? "?" : name), proc);
        x2rw_last_error_set(X2RW_ERROR_PROC_NOT_FOUND);
        return 0u;
    }
    x2rw_log("[k32] GetProcAddress(%s) = %08x", dll, out);
    X2RW_OK();
    return out;
}

uint32_t k32_GetStartupInfoA(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    if (!args[0]) X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    /* STARTUPINFO zerado com cb preenchido: o jogo não usa console/janela. */
    k32_zero(c, args[0], 68);
    x2rw_write32(c, args[0], 68u);
    X2RW_OK();
    return 0u;
}

uint32_t k32_GetStdHandle(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    switch (args[0]) {
        case 0xFFFFFFF6u: X2RW_OK(); return K32_STD_IN;    /* STD_INPUT_HANDLE  */
        case 0xFFFFFFF5u: X2RW_OK(); return K32_STD_OUT;   /* STD_OUTPUT_HANDLE */
        case 0xFFFFFFF4u: X2RW_OK(); return K32_STD_ERR;   /* STD_ERROR_HANDLE  */
        default:
            x2rw_log("[k32] GetStdHandle: seletor inválido %08x", args[0]);
            x2rw_last_error_set(X2RW_ERROR_INVALID_PARAMETER);
            return K32_INVALID_HANDLE;
    }
}

/* Copia uma string fixa para o buffer do guest respeitando o teto. Devolve o
 * comprimento copiado, ou (uint32_t)-1 se o buffer for nulo. */
static k32_word k32i_copy_fixed(x2rw_ctx *c, uint32_t dst, uint32_t cap,
                               const char *s) {
    size_t n = strlen(s);
    if (!dst || cap == 0) return (uint32_t)-1;
    if (n + 1 > cap) {
        if (cap > 1) x2rw_mem_write(dst, s, cap - 1);
        x2rw_write8(c, dst + cap - 1, 0);
        x2rw_last_error_set(X2RW_ERROR_INSUFFICIENT_BUFFER);
        return (uint32_t)(n + 1);         /* o Win32 devolve o tamanho preciso */
    }
    x2rw_mem_write(dst, s, n);
    x2rw_write8(c, dst + (uint32_t)n, 0);
    return (uint32_t)n;
}

uint32_t k32_GetSystemDirectoryA(x2rw_ctx *c, uint32_t *args, int nargs) {
    static const char *dir = "C:\\WINDOWS\\SYSTEM32";
    (void)nargs;
    uint32_t n = k32i_copy_fixed(c, args[1], args[0], dir);
    if (n == (uint32_t)-1) X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    X2RW_OK();
    return n;
}

uint32_t k32_GetSystemInfo(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    if (!args[0]) X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    /* SYSTEM_INFO de um único processador x86 (o guest É x86-32). */
    uint32_t p = args[0];
    x2rw_write32(c, p + 0u,  0u);            /* dwOemId                      */
    x2rw_write32(c, p + 4u,  4096u);         /* dwPageSize                   */
    x2rw_write32(c, p + 8u,  X2RW_STACK_LO); /* lpMinimumApplicationAddress  */
    x2rw_write32(c, p + 12u, 0x7FFEFFFFu);   /* lpMaximumApplicationAddress  */
    x2rw_write32(c, p + 16u, 1u);            /* dwActiveProcessorMask        */
    x2rw_write32(c, p + 20u, 1u);            /* dwNumberOfProcessors         */
    x2rw_write32(c, p + 24u, 586u);          /* PROCESSOR_INTEL_PENTIUM      */
    x2rw_write32(c, p + 28u, 65536u);        /* dwAllocationGranularity      */
    x2rw_write16(c, p + 32u, 6u);            /* wProcessorLevel              */
    x2rw_write16(c, p + 34u, 0x0100u);       /* wProcessorRevision           */
    X2RW_OK();
    return 0u;
}

uint32_t k32_GetSystemTime(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    struct timespec ts;
    struct tm t;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || !gmtime_r(&ts.tv_sec, &t)) {
        x2rw_log("[k32] GetSystemTime: relógio indisponível");
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    k32_write_systime(c, args[0], &t, (int)(ts.tv_nsec / 1000000));
    X2RW_OK();
    return 0u;
}

uint32_t k32_GetSystemTimeAsFileTime(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    k32_write_ft(c, args[0], k32_now_filetime());
    X2RW_OK();
    return 0u;
}

uint32_t k32_GetTickCount(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c; (void)args; (void)nargs;
    /* Monotônico desde a primeira chamada; NÃO loga (é chamado por frame).
     * Envolve em 2^32 ms como no Win32 (49 dias). */
    return (uint32_t)(k32_mono_rel() / 1000000ull);
}

uint32_t k32_GetTimeZoneInformation(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t p = args[0];
    (void)nargs;
    if (!p) X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    static const char *std_name = "Standard Time";
    static const char *dst_name = "Daylight Time";
    int64_t off = k32_tz_offset();

    k32_zero(c, p, 172);                      /* TIME_ZONE_INFORMATION */
    x2rw_write32(c, p + 0u, (uint32_t)(int32_t)(-off / 60));   /* Bias (min) */
    for (int i = 0; std_name[i]; i++)
        x2rw_write16(c, p + 4u + 2u * (uint32_t)i, (uint16_t)std_name[i]);
    for (int i = 0; dst_name[i]; i++)
        x2rw_write16(c, p + 88u + 2u * (uint32_t)i, (uint16_t)dst_name[i]);
    /* StandardDate/DaylightDate ficam zeradas: não modelamos as regras de
     * horário de verão, e por isso a resposta abaixo é TIME_ZONE_ID_UNKNOWN. */
    X2RW_OK();
    return 0u;
}

uint32_t k32_GetVersion(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c; (void)args; (void)nargs;
    /* Windows XP: 5.1 build 2600 -> low word 0x0105, high word 0x0A28. */
    return 0x0A280105u;
}

uint32_t k32_GetVersionExA(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t p = args[0];
    (void)nargs;
    if (!p) X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    uint32_t cb = x2rw_read32(c, p);
    if (cb < 20u) {
        x2rw_log("[k32] GetVersionExA: dwOSVersionInfoSize=%u inválido", cb);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    x2rw_write32(c, p + 4u,  5u);         /* dwMajorVersion */
    x2rw_write32(c, p + 8u,  1u);         /* dwMinorVersion */
    x2rw_write32(c, p + 12u, 2600u);      /* dwBuildNumber  */
    x2rw_write32(c, p + 16u, 2u);         /* VER_PLATFORM_WIN32_NT */
    if (cb > 20u) {
        uint32_t room = cb - 20u;
        if (room > 128u) room = 128u;
        k32_zero(c, p + 20u, room);       /* szCSDVersion = "" */
    }
    X2RW_OK();
    return 1u;
}

uint32_t k32_GetWindowsDirectoryA(x2rw_ctx *c, uint32_t *args, int nargs) {
    static const char *dir = "C:\\WINDOWS";
    (void)nargs;
    uint32_t n = k32i_copy_fixed(c, args[1], args[0], dir);
    if (n == (uint32_t)-1) X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    X2RW_OK();
    return n;
}

uint32_t k32_HeapAlloc(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    /* Só existe UM heap no guest (x2rw_heap_alloc), então hHeap é decorativo;
     * logamos um aviso se vier um handle que não seja o nosso. */
    if (args[0] != K32_HEAP_HANDLE && args[0] != K32_PROCESS_HANDLE) {
        static uint32_t logged = 0;
        if (logged++ < 4)
            x2rw_log("[k32] HeapAlloc: hHeap=%08x desconhecido — usando o heap "
                     "único do guest", args[0]);
    }
    uint32_t p = x2rw_heap_alloc(c, args[2]);
    if (!p) {
        x2rw_log("[k32] HeapAlloc(%u bytes) sem memória", (unsigned)args[2]);
        X2RW_FAIL(c, X2RW_ERROR_NOT_ENOUGH_MEMORY);
    }
    if (args[1] & K32_HEAP_ZERO_MEMORY) k32_zero(c, p, args[2] ? args[2] : 16u);
    X2RW_OK();
    return p;
}

uint32_t k32_HeapCreate(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)args; (void)nargs;
    /* REAL no modelo possível: existe um único heap do guest; devolvemos um
     * handle fixo. HeapDestroy é no-op e o espaço não é recuperado. */
    x2rw_log("[k32] HeapCreate: heap único do guest (handle %08x)",
             K32_HEAP_HANDLE);
    X2RW_OK();
    return K32_HEAP_HANDLE;
}

uint32_t k32_HeapDestroy(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    x2rw_log("[k32] HeapDestroy(%08x): no-op (heap único do guest)", args[0]);
    X2RW_OK();
    return 1u;
}

uint32_t k32_HeapFree(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    x2rw_heap_free(c, args[2]);
    X2RW_OK();
    return 1u;
}

uint32_t k32_HeapReAlloc(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    if (!args[2]) return k32_HeapAlloc(c, args, nargs);
    /* O tamanho antigo está no cabeçalho do bloco do heap do guest (ver
     * block_hdr em x2rw_runtime.c: uint32 size no offset -16). */
    uint32_t old = x2rw_read32(c, args[2] - 16u);
    uint32_t p = x2rw_heap_alloc(c, args[3]);
    if (!p) {
        x2rw_log("[k32] HeapReAlloc(%u bytes) sem memória", (unsigned)args[3]);
        X2RW_FAIL(c, X2RW_ERROR_NOT_ENOUGH_MEMORY);
    }
    uint32_t n = old < args[3] ? old : args[3];
    if (n) k32_guest_copy(p, args[2], n);
    x2rw_heap_free(c, args[2]);
    X2RW_OK();
    return p;
}

uint32_t k32_HeapSize(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    if (!args[2]) {
        x2rw_log("[k32] HeapSize(NULL)");
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    /* Tamanho do bloco do heap do guest (offset -16, ver x2rw_runtime.c). */
    X2RW_OK();
    return x2rw_read32(c, args[2] - 16u);
}

uint32_t k32_InitializeCriticalSection(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    if (!args[0]) X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    /* O jogo pode ler os campos da CRITICAL_SECTION: zeramos os 24 bytes. */
    k32_zero(c, args[0], 24);
    if (!k32_cs_find(args[0], 1)) {
        x2rw_log("[k32] InitializeCriticalSection(%08x): tabela cheia (%d)",
                 args[0], K32_MAX_CS);
        X2RW_FAIL(c, X2RW_ERROR_NOT_ENOUGH_MEMORY);
    }
    X2RW_OK();
    return 0u;
}

/* RMW atômico sobre a memória do guest. Não existe ponteiro do host para o
 * VA, então a atomicidade é garantida por um lock estático (o runtime também
 * é paginado — ver x2rw_runtime.c §espaço de endereços). */
static volatile char g_int_lock = 0;

static k32_word k32i_int_add(x2rw_ctx *c, uint32_t va, int32_t delta) {
    while (__atomic_test_and_set(&g_int_lock, __ATOMIC_ACQUIRE)) { }
    uint32_t v = x2rw_read32(c, va);
    v = (uint32_t)((int32_t)v + delta);
    x2rw_write32(c, va, v);
    __atomic_clear(&g_int_lock, __ATOMIC_RELEASE);
    return v;
}

uint32_t k32_InterlockedDecrement(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    /* REAL: o jogo usa muito; sem atomicidade os contadores corrompem. */
    return k32i_int_add(c, args[0], -1);
}

uint32_t k32_InterlockedIncrement(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    return k32i_int_add(c, args[0], 1);
}

/* IsBad*Ptr: no guest TODO endereço do espaço de 32 bits é legível/escrito
 * (as páginas ausentes devolvem zero e são criadas na escrita), então a
 * resposta correta é FALSE = "não é ruim". */
uint32_t k32_IsBadCodePtr(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c; (void)args; (void)nargs;
    return 0u;
}

uint32_t k32_IsBadReadPtr(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c; (void)args; (void)nargs;
    return 0u;
}

uint32_t k32_IsBadWritePtr(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c; (void)args; (void)nargs;
    return 0u;
}

uint32_t k32_IsProcessorFeaturePresent(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c; (void)nargs;
    switch (args[0]) {
        case 0u:  return 0u;   /* PF_FLOATING_POINT_EMULATED: há FPU (x87) */
        case 3u:  return 1u;   /* PF_MMX_INSTRUCTIONS_AVAILABLE (x2rw_simd) */
        case 6u:  return 1u;   /* PF_SSE_INSTRUCTIONS_AVAILABLE (x2rw_simd) */
        case 10u: return 1u;   /* PF_SSE2_INSTRUCTIONS_AVAILABLE             */
        default:
            x2rw_log("[k32] IsProcessorFeaturePresent(%u): desconhecida — 0",
                     (unsigned)args[0]);
            return 0u;
    }
}

uint32_t k32_LeaveCriticalSection(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    k32_cs_t *cs = k32_cs_find(args[0], 0);
    if (!cs) {
        x2rw_log("[k32] LeaveCriticalSection(%08x): seção nunca inicializada",
                 args[0]);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
    }
    pthread_mutex_unlock(&cs->m);
    X2RW_OK();
    return 0u;
}

uint32_t k32_LoadLibraryA(x2rw_ctx *c, uint32_t *args, int nargs) {
    char name[256];
    uint32_t handle = 0;
    (void)nargs;
    k32_str(c, args[0], name, sizeof(name));
    if (!name[0]) {
        x2rw_log("[k32] LoadLibraryA: nome vazio");
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    if (k32_module_handle(name, &handle)) {
        x2rw_log("[k32] LoadLibraryA('%s') = %08x (já registrado)", name, handle);
        X2RW_OK();
        return handle;
    }
    for (int i = 0; i < K32_MAX_LOADS; i++) {
        if (!g_loads[i].used) {
            uint32_t h = g_next_load;
            g_loads[i].used = 1;
            g_loads[i].handle = h;
            k32_copy(g_loads[i].name, sizeof(g_loads[i].name), name);
            g_next_load += 0x1000u;
            /* Nenhuma DLL do host é carregada: os símbolos dela só existem
             * se estiverem no registro de shims (GetProcAddress resolve). */
            x2rw_log("[k32] LoadLibraryA('%s') = %08x: DLL do host NÃO foi "
                     "carregada (apenas shims)", name, h);
            X2RW_OK();
            return h;
        }
    }
    x2rw_log("[k32] LoadLibraryA('%s'): tabela de módulos carregados cheia", name);
    X2RW_FAIL(c, X2RW_ERROR_NOT_ENOUGH_MEMORY);
}

/* Recursos de PE: inexistentes neste build (ver FindResourceA). Devolver um
 * HRSRC/HGLOBAL falso faria o jogo desreferenciar lixo. */
uint32_t k32_LoadResource(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)args; (void)nargs;
    x2rw_log("[k32] LoadResource: recursos de PE não existem neste port");
    X2RW_FAIL(c, X2RW_ERROR_CALL_NOT_IMPLEMENTED);
}

uint32_t k32_LockResource(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)args; (void)nargs;
    x2rw_log("[k32] LockResource: recursos de PE não existem neste port");
    X2RW_FAIL(c, X2RW_ERROR_CALL_NOT_IMPLEMENTED);
}

uint32_t k32_MapViewOfFile(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    k32_obj_t *o = k32_obj_find(args[0], K32_OBJ_MAPPING);
    if (!o) {
        x2rw_log("[k32] MapViewOfFile: mapeamento %08x inválido", args[0]);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
    }
    if (args[2] != 0) {
        x2rw_log("[k32] MapViewOfFile: offset > 4 GiB não suportado");
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    uint32_t off = args[3];
    uint32_t size = args[4];
    if (size == 0) size = (o->max_size > off) ? o->max_size - off : 0u;
    if (size == 0) {
        x2rw_log("[k32] MapViewOfFile: view de 0 bytes (offset %u)", off);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    uint32_t va = x2rw_heap_alloc(c, size);
    if (!va) {
        x2rw_log("[k32] MapViewOfFile(%u bytes): sem memória", size);
        X2RW_FAIL(c, X2RW_ERROR_NOT_ENOUGH_MEMORY);
    }
    k32_zero(c, va, size);                /* zeros nas partes sem arquivo */

    if (o->file_handle) {
        k32_file_t *f = k32_file_find(o->file_handle);
        if (!f) {
            x2rw_heap_free(c, va);
            x2rw_log("[k32] MapViewOfFile: arquivo %08x já fechado", o->file_handle);
            X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
        }
        if (fseek(f->fp, (long)off, SEEK_SET) != 0) {
            x2rw_heap_free(c, va);
            x2rw_log("[k32] MapViewOfFile: seek para %u falhou", off);
            X2RW_FAIL(c, X2RW_ERROR_ACCESS_DENIED);
        }
        uint32_t got = 0;
        while (got < size) {
            uint32_t n = size - got;
            if (n > K32_IO_CHUNK) n = K32_IO_CHUNK;
            size_t r = fread(g_io_buf, 1, n, f->fp);
            if (r) {
                x2rw_mem_write(va + got, g_io_buf, r);
                got += (uint32_t)r;
            }
            if (r < n) break;             /* fim do arquivo */
        }
        x2rw_log("[k32] MapViewOfFile: %u/%u bytes do arquivo %08x em %08x",
                 got, size, o->file_handle, va);
    } else {
        x2rw_log("[k32] MapViewOfFile: mapeamento anônimo de %u bytes em %08x",
                 size, va);
    }
    /* PARCIAL: um view por mapeamento (o último vence). Vários views
     * simultâneos do mesmo arquivo não são suportados nesta fase. */
    o->view = va;
    o->view_size = size;
    X2RW_OK();
    return va;
}

uint32_t k32_MoveFileA(x2rw_ctx *c, uint32_t *args, int nargs) {
    char from[1024], to[1024], hfrom[1024], hto[1024];
    (void)nargs;
    k32_str(c, args[0], from, sizeof(from));
    k32_str(c, args[1], to, sizeof(to));
    if (!from[0] || !to[0]) X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    x2rw_win32_path_to_host(from, hfrom, sizeof(hfrom));
    x2rw_win32_path_to_host(to, hto, sizeof(hto));
    if (rename(hfrom, hto) != 0) {
        uint32_t e = k32i_errno_to_win(errno);
        x2rw_log("[k32] MoveFileA('%s' -> '%s') falhou (erro %u)", from, to, e);
        x2rw_last_error_set(e);
        return 0u;
    }
    x2rw_log("[k32] MoveFileA('%s' -> '%s')", from, to);
    X2RW_OK();
    return 1u;
}

uint32_t k32_OutputDebugStringA(x2rw_ctx *c, uint32_t *args, int nargs) {
    char txt[1024];
    (void)nargs;
    k32_str(c, args[0], txt, sizeof(txt));
    x2rw_log("[ODS] %s", txt);
    X2RW_OK();
    return 0u;
}

uint32_t k32_PeekConsoleInputA(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    /* PARCIAL: sem console de entrada no port; devolve "nenhum evento".
     * args[3] é lpNumberOfEventsRead (args[2] é o tamanho do buffer). */
    if (!args[3]) X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    x2rw_write32(c, args[3], 0u);
    X2RW_OK();
    return 1u;
}

uint32_t k32_QueryPerformanceCounter(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    /* Monotônico e crescente (base comum com GetTickCount); não loga. */
    x2rw_write64(c, args[0], k32_mono_rel());
    X2RW_OK();
    return 1u;
}

uint32_t k32_QueryPerformanceFrequency(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    /* O contador está em nanossegundos (clock_gettime CLOCK_MONOTONIC). */
    x2rw_write64(c, args[0], 1000000000ull);
    X2RW_OK();
    return 1u;
}

uint32_t k32_RaiseException(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    /* Sem SEH real no port: a exceção do jogo vira um fault do host. */
    x2rw_log("[k32] RaiseException(code=%08x flags=%u args=%u)",
             args[0], (unsigned)args[1], (unsigned)args[2]);
    x2rw_fault(c, X2RW_FAULT_UNIMPL_API, args[0], "RaiseException");
    return 0u;
}

uint32_t k32_ReadConsoleInputA(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    /* PARCIAL: sem console de entrada; devolve "nenhum evento lido".
     * args[3] é lpNumberOfEventsRead (args[2] é o tamanho do buffer). */
    if (!args[3]) X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    x2rw_write32(c, args[3], 0u);
    X2RW_OK();
    return 1u;
}

uint32_t k32_ReadFile(x2rw_ctx *c, uint32_t *args, int nargs) {
    if (nargs < 5) X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    uint32_t h = args[0], buf = args[1], want = args[2], pgot = args[3];

    if (h == K32_STD_IN) {
        /* Sem console de entrada: declara explicitamente 0 bytes lidos. */
        if (pgot) x2rw_write32(c, pgot, 0u);
        x2rw_log("[k32] ReadFile(stdin): sem console no port — 0 bytes");
        X2RW_OK();
        return 1u;
    }
    k32_file_t *f = k32_file_find(h);
    if (!f) {
        x2rw_log("[k32] ReadFile: handle inválido %08x", h);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
    }
    if (!buf && want) {
        x2rw_log("[k32] ReadFile: buffer nulo para %u bytes", (unsigned)want);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    uint32_t got = 0;
    int failed = 0;
    while (got < want) {
        uint32_t n = want - got;
        if (n > K32_IO_CHUNK) n = K32_IO_CHUNK;
        size_t r = fread(g_io_buf, 1, n, f->fp);
        if (r) {
            x2rw_mem_write(buf + got, g_io_buf, r);
            got += (uint32_t)r;
        }
        if (r < n) {                       /* fim de arquivo ou erro */
            if (ferror(f->fp)) failed = 1;
            break;
        }
    }
    if (pgot) x2rw_write32(c, pgot, got);
    if (failed) {
        x2rw_log("[k32] ReadFile(handle %08x): erro de E/S após %u bytes",
                 h, got);
        X2RW_FAIL(c, X2RW_ERROR_ACCESS_DENIED);
    }
    X2RW_OK();
    return 1u;
}

uint32_t k32_ReleaseMutex(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    k32_obj_t *o = k32_obj_find(args[0], K32_OBJ_MUTEX);
    if (!o) {
        x2rw_log("[k32] ReleaseMutex: handle %08x não é mutex", args[0]);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
    }
    /* Sem posse real: WaitForSingleObject devolve imediatamente (ver lá), o
     * que é coerente com um port single-thread em que nada disputa o mutex. */
    o->signaled = 1;
    X2RW_OK();
    return 1u;
}

uint32_t k32_ResumeThread(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    /* PARCIAL: os threads criados não suspendem nem retomam (não rodam). */
    x2rw_log("[k32] ResumeThread(%08x): nenhum thread real suspenso — 0",
             args[0]);
    X2RW_OK();
    return 0u;                             /* contagem de suspensão anterior */
}

/* RtlUnwind (lista mestra, STUB) aponta para shim_unimplemented. A definição
 * FORTE vive em x2rw_imports.c — ela tem acesso ao shim corrente e loga
 * DLL+nome. Esta aqui é FRACA e serve para este arquivo poder ser compilado e
 * ligado sozinho (testes do shim); o linker sempre prefere a forte. */
#if defined(__GNUC__)
__attribute__((weak))
#endif
uint32_t shim_unimplemented(x2rw_ctx *c, uint32_t *args, int nargs) {
    x2rw_log("[k32] shim_unimplemented: chamado sem o despachante (args[0]=%08x)",
             nargs > 0 ? args[0] : 0u);
    x2rw_fault(c, X2RW_FAULT_UNIMPL_API, 0, "shim_unimplemented");
    return 0u;
}

uint32_t k32_SetConsoleCtrlHandler(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    /* PARCIAL: nenhum sinal do host é traduzido em Ctrl+C/Ctrl+Break. */
    x2rw_log("[k32] SetConsoleCtrlHandler(%08x, %u): sem console — aceito",
             args[0], (unsigned)args[1]);
    X2RW_OK();
    return 1u;
}

uint32_t k32_SetConsoleMode(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    /* PARCIAL: não há modo de console para guardar; aceita e loga. */
    static uint32_t logged = 0;
    if (logged++ < 4)
        x2rw_log("[k32] SetConsoleMode(%08x, %08x): sem console — no-op",
                 args[0], args[1]);
    X2RW_OK();
    return 1u;
}

uint32_t k32_SetEndOfFile(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    k32_file_t *f = k32_file_find(args[0]);
    if (!f) {
        x2rw_log("[k32] SetEndOfFile: handle inválido %08x", args[0]);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
    }
    if (fflush(f->fp) != 0) X2RW_FAIL(c, X2RW_ERROR_ACCESS_DENIED);
    long pos = ftell(f->fp);
    if (pos < 0) X2RW_FAIL(c, X2RW_ERROR_ACCESS_DENIED);
    if (ftruncate(fileno(f->fp), (off_t)pos) != 0) {
        x2rw_log("[k32] SetEndOfFile(handle %08x): ftruncate falhou (errno=%d)",
                 args[0], errno);
        X2RW_FAIL(c, X2RW_ERROR_ACCESS_DENIED);
    }
    X2RW_OK();
    return 1u;
}

uint32_t k32_SetEnvironmentVariableA(x2rw_ctx *c, uint32_t *args, int nargs) {
    char name[K32_ENV_NAME], value[K32_ENV_VALUE];
    (void)nargs;
    k32_str(c, args[0], name, sizeof(name));
    if (!name[0]) X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    k32_env_defaults();
    if (args[1] == 0) {                   /* valor NULL = apaga a variável */
        k32_env_set(name, NULL);
        x2rw_log("[k32] SetEnvironmentVariableA('%s', NULL): removida", name);
    } else {
        k32_str(c, args[1], value, sizeof(value));
        if (!k32_env_set(name, value)) {
            x2rw_log("[k32] SetEnvironmentVariableA: tabela de ambiente cheia");
            X2RW_FAIL(c, X2RW_ERROR_NOT_ENOUGH_MEMORY);
        }
    }
    k32_env_invalidate();
    X2RW_OK();
    return 1u;
}

uint32_t k32_SetErrorMode(x2rw_ctx *c, uint32_t *args, int nargs) {
    static uint32_t mode = 0;
    uint32_t old = mode;
    (void)nargs;
    mode = args[0];
    x2rw_log("[k32] SetErrorMode(%08x) — anterior %08x", args[0], old);
    X2RW_OK();
    return old;                           /* devolve o modo anterior */
}

uint32_t k32_SetEvent(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    k32_obj_t *o = k32_obj_find(args[0], K32_OBJ_EVENT);
    if (!o) {
        x2rw_log("[k32] SetEvent: handle %08x não é evento", args[0]);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
    }
    o->signaled = 1;
    X2RW_OK();
    return 1u;
}

uint32_t k32_SetFilePointer(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    k32_file_t *f = k32_file_find(args[0]);
    if (!f) {
        x2rw_log("[k32] SetFilePointer: handle inválido %08x", args[0]);
        x2rw_last_error_set(X2RW_ERROR_INVALID_HANDLE);
        return K32_INVALID_HANDLE;
    }
    /* Distância de 64 bits: parte baixa em args[1], parte alta opcional no
     * ponteiro args[2]. `long` tem 64 bits nos alvos LP64 (arm64-v8a). */
    int64_t dist = (int64_t)(int32_t)args[1];
    if (args[2]) dist |= ((int64_t)(int32_t)x2rw_read32(c, args[2])) << 32;

    int whence = SEEK_SET;
    if (args[3] == 1u) whence = SEEK_CUR;
    else if (args[3] == 2u) whence = SEEK_END;
    else if (args[3] != 0u) {
        x2rw_log("[k32] SetFilePointer: método %u inválido", args[3]);
        x2rw_last_error_set(X2RW_ERROR_INVALID_PARAMETER);
        return K32_INVALID_HANDLE;
    }
    if (fseek(f->fp, (long)dist, whence) != 0) {
        x2rw_log("[k32] SetFilePointer(handle %08x, %lld): fseek falhou",
                 args[0], (long long)dist);
        x2rw_last_error_set(X2RW_ERROR_ACCESS_DENIED);
        return K32_INVALID_HANDLE;
    }
    long pos = ftell(f->fp);
    if (pos < 0) {
        x2rw_last_error_set(X2RW_ERROR_ACCESS_DENIED);
        return K32_INVALID_HANDLE;
    }
    if (args[2]) x2rw_write32(c, args[2], (uint32_t)((uint64_t)pos >> 32));
    X2RW_OK();
    return (uint32_t)pos;
}

uint32_t k32_SetHandleCount(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    /* STUB: o port não tem a tabela de handles do DOS/CRT; aceitamos o valor
     * e devolvemos o mesmo (o jogo só usa o retorno para saber o limite). */
    x2rw_log("[k32] SetHandleCount(%u): stub — devolve o mesmo valor",
             (unsigned)args[0]);
    X2RW_OK();
    return args[0];
}

uint32_t k32_SetLastError(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c; (void)nargs;
    x2rw_last_error_set(args[0]);
    return 0u;
}

uint32_t k32_SetStdHandle(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c; (void)nargs;
    /* STUB: o port não tem console; os handles padrão são fixos (ver
     * GetStdHandle). Registramos a tentativa para não falhar em silêncio. */
    x2rw_log("[k32] SetStdHandle(%08x, %08x): sem console — ignorado",
             args[0], args[1]);
    return 1u;
}

uint32_t k32_SetThreadPriority(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    /* PARCIAL: o port não tem escalonador de threads do jogo. */
    x2rw_log("[k32] SetThreadPriority(%08x, %d): sem threads reais — aceito",
             args[0], (int)args[1]);
    X2RW_OK();
    return 1u;
}

uint32_t k32_SetUnhandledExceptionFilter(x2rw_ctx *c, uint32_t *args,
                                         int nargs) {
    static uint32_t filter = 0;
    uint32_t old = filter;
    (void)nargs;
    filter = args[0];
    /* REAL no que é possível: guardamos o filtro e o devolvemos na troca,
     * mas o port não tem SEH para invocá-lo (ver RaiseException). */
    x2rw_log("[k32] SetUnhandledExceptionFilter(%08x) — anterior %08x",
             args[0], old);
    X2RW_OK();
    return old;
}

uint32_t k32_SizeofResource(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)args; (void)nargs;
    x2rw_log("[k32] SizeofResource: recursos de PE não existem neste port");
    X2RW_FAIL(c, X2RW_ERROR_CALL_NOT_IMPLEMENTED);
}

uint32_t k32_Sleep(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t ms = args[0];
    (void)nargs;
    /* NUNCA dormir de verdade: o port é single-thread e o jogo espera
     * progresso. Só um atraso curto (<= 50 ms) é honrado para não transformar
     * um laço de espera em busy-loop. */
    if (ms > 0u && ms <= 50u) {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = (long)ms * 1000000L;
        nanosleep(&ts, NULL);
    } else if (ms > 50u) {
        static uint32_t logged = 0;
        if (logged++ < 8)
            x2rw_log("[k32] Sleep(%u): não dorme (port single-thread)",
                     (unsigned)ms);
    }
    X2RW_OK();
    return 0u;
}

uint32_t k32_TerminateProcess(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    /* Não chamamos exit(): o host encerra ao ver o fault. */
    x2rw_log("[k32] TerminateProcess(%08x, %u)", args[0], (unsigned)args[1]);
    x2rw_fault(c, X2RW_FAULT_UNIMPL_API, 0, "TerminateProcess");
    return 0u;
}

uint32_t k32_TerminateThread(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    /* PARCIAL: sem threads reais não há o que terminar; falha explícita em
     * vez de dizer que terminou. */
    x2rw_log("[k32] TerminateThread(%08x, %u): sem threads no port",
             args[0], (unsigned)args[1]);
    X2RW_FAIL(c, X2RW_ERROR_CALL_NOT_IMPLEMENTED);
}

uint32_t k32_TlsAlloc(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c; (void)args; (void)nargs;
    for (uint32_t i = 0; i < K32_TLS_SLOTS; i++) {
        if (!g_tls_used[i]) {
            g_tls_used[i] = 1;
            g_tls_value[i] = 0;
            X2RW_OK();
            return i;
        }
    }
    x2rw_log("[k32] TlsAlloc: %d slots esgotados", K32_TLS_SLOTS);
    x2rw_last_error_set(X2RW_ERROR_NOT_ENOUGH_MEMORY);
    return K32_INVALID_HANDLE;            /* TLS_OUT_OF_INDEXES */
}

uint32_t k32_TlsFree(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    uint32_t idx = args[0];
    if (idx >= K32_TLS_SLOTS || !g_tls_used[idx]) {
        x2rw_log("[k32] TlsFree(%u): índice inválido", (unsigned)idx);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    g_tls_used[idx] = 0;
    g_tls_value[idx] = 0;
    X2RW_OK();
    return 1u;
}

uint32_t k32_TlsGetValue(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    uint32_t idx = args[0];
    if (idx >= K32_TLS_SLOTS || !g_tls_used[idx]) {
        x2rw_log("[k32] TlsGetValue(%u): índice inválido", (unsigned)idx);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    /* O Win32 zera o last error neste getter. */
    X2RW_OK();
    return g_tls_value[idx];
}

uint32_t k32_TlsSetValue(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    uint32_t idx = args[0];
    if (idx >= K32_TLS_SLOTS || !g_tls_used[idx]) {
        x2rw_log("[k32] TlsSetValue(%u): índice inválido", (unsigned)idx);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    g_tls_value[idx] = args[1];
    X2RW_OK();
    return 1u;
}

uint32_t k32_UnhandledExceptionFilter(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    /* Stub honesto: não há SEH no port e não vamos encerrar o processo por
     * conta própria; devolvemos EXCEPTION_EXECUTE_HANDLER (1) e logamos. */
    x2rw_log("[k32] UnhandledExceptionFilter(%08x): sem SEH — handler padrão",
             args[0]);
    X2RW_OK();
    return 1u;
}

uint32_t k32_UnmapViewOfFile(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    uint32_t va = args[0];
    int found = 0;
    for (int i = 0; i < K32_MAX_OBJECTS; i++) {
        if (g_objs[i].used && g_objs[i].kind == K32_OBJ_MAPPING &&
            g_objs[i].view == va) {
            g_objs[i].view = 0;
            g_objs[i].view_size = 0;
            found = 1;
            break;
        }
    }
    if (!found) {
        x2rw_log("[k32] UnmapViewOfFile(%08x): view não conhecida", va);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    x2rw_heap_free(c, va);
    X2RW_OK();
    return 1u;
}

uint32_t k32_VirtualAlloc(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    uint32_t size = args[1];
    if (size == 0u) {
        x2rw_log("[k32] VirtualAlloc(size=0)");
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    /* Alocação no heap do guest; o endereço pedido em args[0] é uma dica que
     * não podemos honrar (o heap é um bump allocator). */
    uint32_t p = x2rw_heap_alloc(c, size);
    if (!p) {
        x2rw_log("[k32] VirtualAlloc(%u bytes): sem memória", (unsigned)size);
        X2RW_FAIL(c, X2RW_ERROR_OUTOFMEMORY);
    }
    k32_zero(c, p, size);                 /* o Win32 entrega páginas zeradas */
    if (args[0] && args[0] != p) {
        static uint32_t logged = 0;
        if (logged++ < 8)
            x2rw_log("[k32] VirtualAlloc: endereço pedido %08x ignorado "
                     "(devolvido %08x)", args[0], p);
    }
    X2RW_OK();
    return p;
}

uint32_t k32_VirtualFree(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    if (!args[0]) {
        x2rw_log("[k32] VirtualFree(NULL)");
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    /* O bump allocator do guest não devolve espaço; marca o bloco como livre
     * (ver x2rw_heap_free). */
    x2rw_heap_free(c, args[0]);
    X2RW_OK();
    return 1u;
}

uint32_t k32_VirtualProtect(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    /* O port não tem proteção de página no guest: toda a memória é
     * leitura/escrita. Devolvemos PAGE_READWRITE como proteção anterior. */
    static uint32_t logged = 0;
    if (logged++ < 8)
        x2rw_log("[k32] VirtualProtect(%08x, %u, %08x): sem proteção real no "
                 "guest — no-op", args[0], (unsigned)args[1], args[2]);
    if (args[3]) x2rw_write32(c, args[3], K32_PAGE_READWRITE);
    X2RW_OK();
    return 1u;
}

uint32_t k32_VirtualQuery(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    uint32_t va = args[0], out = args[1], len = args[2];
    if (!out || len < 28u) {
        x2rw_log("[k32] VirtualQuery: buffer %08x/%u pequeno", out,
                 (unsigned)len);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    uint32_t base, region, state, protect;
    if (va < X2RW_STACK_LO) {
        base = 0u;
        region = X2RW_STACK_LO - va;      /* do início da página 0 até a pilha */
        state = K32_MEM_FREE;
        protect = K32_PAGE_NOACCESS;
    } else {
        base = va & 0xFFFF0000u;
        region = 0x00010000u;             /* granularidade da tabela do guest */
        state = K32_MEM_COMMIT;
        protect = K32_PAGE_READWRITE;
    }
    x2rw_write32(c, out + 0u,  va);          /* BaseAddress       */
    x2rw_write32(c, out + 4u,  base);        /* AllocationBase    */
    x2rw_write32(c, out + 8u,  K32_PAGE_READWRITE); /* AllocationProtect */
    x2rw_write32(c, out + 12u, region);      /* RegionSize        */
    x2rw_write32(c, out + 16u, state);       /* State             */
    x2rw_write32(c, out + 20u, protect);     /* Protect           */
    x2rw_write32(c, out + 24u, K32_MEM_PRIVATE); /* Type          */
    X2RW_OK();
    return 28u;                          /* bytes escritos */
}

uint32_t k32_WaitForMultipleObjects(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t count = args[0];
    (void)nargs;
    if (count == 0u || count > 64u) {
        x2rw_log("[k32] WaitForMultipleObjects: nCount=%u inválido",
                 (unsigned)count);
        x2rw_last_error_set(X2RW_ERROR_INVALID_PARAMETER);
        return K32_INVALID_HANDLE;        /* WAIT_FAILED */
    }
    /* REAL no modelo: NUNCA bloqueia. Devolve WAIT_OBJECT_0 (o primeiro
     * objeto), que é o que um port single-thread pode garantir sem travar. */
    static uint32_t logged = 0;
    if (logged++ < 8)
        x2rw_log("[k32] WaitForMultipleObjects(n=%u, all=%u): devolve "
                 "WAIT_OBJECT_0 imediatamente", (unsigned)count,
                 (unsigned)args[2]);
    X2RW_OK();
    return 0u;                            /* WAIT_OBJECT_0 */
}

uint32_t k32_WaitForSingleObject(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)nargs;
    if (args[0] == 0u) {
        x2rw_log("[k32] WaitForSingleObject(NULL)");
        x2rw_last_error_set(X2RW_ERROR_INVALID_HANDLE);
        return K32_INVALID_HANDLE;        /* WAIT_FAILED */
    }
    /* NUNCA bloqueie: o jogo travaria (não há outra thread para sinalizar).
     * WAIT_OBJECT_0 imediato é a única resposta que mantém o progresso. */
    static uint32_t logged = 0;
    if (logged++ < 8)
        x2rw_log("[k32] WaitForSingleObject(%08x, %u): devolve WAIT_OBJECT_0 "
                 "imediatamente (sem espera real)", args[0], (unsigned)args[1]);
    X2RW_OK();
    return 0u;                            /* WAIT_OBJECT_0 */
}

uint32_t k32_WriteConsoleA(x2rw_ctx *c, uint32_t *args, int nargs) {
    char txt[512];
    uint32_t n = args[2];
    (void)nargs;
    uint32_t m = n < sizeof(txt) - 1 ? n : (uint32_t)(sizeof(txt) - 1);
    if (args[1] && m) x2rw_mem_read(args[1], txt, m);
    txt[m] = 0;
    /* PARCIAL: não há console Win32; o texto vai para o log do port. */
    static uint32_t logged = 0;
    if (logged++ < 32) x2rw_log("[console] %s", txt);
    if (args[3]) x2rw_write32(c, args[3], n);
    X2RW_OK();
    return 1u;
}

uint32_t k32_WriteFile(x2rw_ctx *c, uint32_t *args, int nargs) {
    if (nargs < 5) X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    uint32_t h = args[0], buf = args[1], want = args[2], pput = args[3];

    if (h == K32_STD_OUT || h == K32_STD_ERR) {
        char txt[512];
        uint32_t m = want < sizeof(txt) - 1 ? want : (uint32_t)(sizeof(txt) - 1);
        if (buf && m) x2rw_mem_read(buf, txt, m);
        txt[m] = 0;
        static uint32_t logged = 0;
        if (logged++ < 32) x2rw_log("[stdout] %s", txt);
        if (pput) x2rw_write32(c, pput, want);
        X2RW_OK();
        return 1u;
    }
    if (h == K32_STD_IN) {
        x2rw_log("[k32] WriteFile(stdin): handle não é de escrita");
        X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
    }
    k32_file_t *f = k32_file_find(h);
    if (!f) {
        x2rw_log("[k32] WriteFile: handle inválido %08x", h);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
    }
    if (!buf && want) {
        x2rw_log("[k32] WriteFile: buffer nulo para %u bytes", (unsigned)want);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }
    uint32_t put = 0;
    int failed = 0;
    while (put < want) {
        uint32_t n = want - put;
        if (n > K32_IO_CHUNK) n = K32_IO_CHUNK;
        x2rw_mem_read(buf + put, g_io_buf, n);
        size_t w = fwrite(g_io_buf, 1, n, f->fp);
        if (w) put += (uint32_t)w;
        if (w < n) {
            failed = 1;
            break;
        }
    }
    if (pput) x2rw_write32(c, pput, put);
    if (failed) {
        x2rw_log("[k32] WriteFile(handle %08x): erro de E/S após %u bytes",
                 h, put);
        X2RW_FAIL(c, X2RW_ERROR_ACCESS_DENIED);
    }
    X2RW_OK();
    return 1u;
}

uint32_t k32_WritePrivateProfileStringA(x2rw_ctx *c, uint32_t *args,
                                        int nargs) {
    char sec[128], key[256], value[512], path[1024];
    (void)nargs;
    k32_str(c, args[0], sec, sizeof(sec));       /* seção  */
    if (args[1]) k32_str(c, args[1], key, sizeof(key));
    else key[0] = 0;                             /* key NULL = apaga seção */
    if (args[2]) k32_str(c, args[2], value, sizeof(value));
    else value[0] = 0;                           /* valor NULL = apaga chave */
    k32_str(c, args[3], path, sizeof(path));

    if (!sec[0] || !path[0] || (args[1] && !key[0])) {
        x2rw_log("[k32] WritePrivateProfileStringA: parâmetros inválidos "
                 "(seção='%s' caminho='%s')", sec, path);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
    }

    size_t inlen = k32_ini_load(path, g_ini_in, sizeof(g_ini_in));
    const char *nkey = (args[1] == 0) ? NULL : key;
    const char *nval = (args[2] == 0) ? NULL : value;

    /* Apagar de um arquivo inexistente não cria arquivo (como no Win32). */
    if (inlen == 0 && nkey == NULL) {
        X2RW_OK();
        return 1u;
    }
    int n = k32_ini_rewrite(g_ini_in, inlen, sec, nkey, nval,
                            g_ini_out, sizeof(g_ini_out) - 1);
    if (n < 0) {
        x2rw_log("[k32] WritePrivateProfileStringA('%s'): INI maior que %d "
                 "bytes", path, K32_INI_MAX);
        X2RW_FAIL(c, X2RW_ERROR_INSUFFICIENT_BUFFER);
    }

    char host[1024];
    x2rw_win32_path_to_host(path, host, sizeof(host));
    FILE *fp = fopen(host, "wb");
    if (!fp) {
        uint32_t e = k32i_errno_to_win(errno);
        x2rw_log("[k32] WritePrivateProfileStringA('%s' -> '%s'): abrir para "
                 "escrita falhou (erro %u)", path, host, e);
        x2rw_last_error_set(e);
        return 0u;
    }
    size_t wrote = n > 0 ? fwrite(g_ini_out, 1, (size_t)n, fp) : 0;
    fclose(fp);
    if (wrote != (size_t)n) {
        x2rw_log("[k32] WritePrivateProfileStringA('%s'): escrita parcial "
                 "(%u de %d)", path, (unsigned)wrote, n);
        X2RW_FAIL(c, X2RW_ERROR_ACCESS_DENIED);
    }
    x2rw_log("[k32] WritePrivateProfileStringA('[%s] %s') em '%s'",
             sec, nkey ? nkey : "(seção removida)", path);
    X2RW_OK();
    return 1u;
}
