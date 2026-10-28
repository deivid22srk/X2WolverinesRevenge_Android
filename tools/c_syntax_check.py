"""Checa SINTAXE de C sem compilador, usando pycparser.

POR QUE ESTA FERRAMENTA EXISTE:
na maquina de desenvolvimento deste port nao ha gcc/clang/NDK. O unico
compilador real e o do CI, e cada rodada do CI custa minutos. Esta checagem
roda em segundos e pega a classe de erro que mais aparece primeiro num build:
sintaxe (chave/parêntese/declaracao invalida/comentario aberto).

O QUE ELA **NAO** FAZ (leia antes de confiar):
  * nao expande macros de verdade — invocacoes de macro sao trocadas por
    `(void)0`, entao o CORPO das macros nao e validado aqui (o linker/lint
    de macros cobre a contagem de argumentos);
  * nao resolve tipos nem simbolos — isso e o tools/x2wr_lint.py;
  * nao substitui compilar. Um "OK" aqui significa "provavelmente compila",
    nunca "compila".

As declaracoes de jni.h/android/log.h sao opacas (apagadas) e os tipos que o
codigo usa vem do preambulo PRELUDE.

Uso:
    python3 tools/c_syntax_check.py runtime/*.c runtime/shims/*.c ...
    python3 tools/c_syntax_check.py --all

A numeracao de linhas e preservada (comentarios e diretivas viram espaco em
branco), entao o erro reportado aponta a linha real do arquivo.
"""
import re
import sys
import os

PRELUDE = r'''
typedef unsigned char uint8_t;
typedef signed char int8_t;
typedef unsigned short uint16_t;
typedef signed short int16_t;
typedef unsigned int uint32_t;
typedef signed int int32_t;
typedef unsigned long long uint64_t;
typedef signed long long int64_t;
typedef unsigned long size_t;
typedef long ssize_t;
typedef unsigned long uintptr_t;
typedef long intptr_t;
typedef int pid_t;
typedef long time_t;
typedef long off_t;
typedef long suseconds_t;
typedef unsigned int mode_t;
typedef int clockid_t;
typedef long clock_t;
typedef struct _FILE FILE;
typedef struct _DIR DIR;
typedef struct _pthread_mutex_t pthread_mutex_t;
typedef struct _pthread_attr_t pthread_attr_t;
typedef struct _pthread_mutexattr_t pthread_mutexattr_t;

typedef int jint;
typedef unsigned char jboolean;
typedef long long jlong;
typedef float jfloat;
typedef double jdouble;
typedef unsigned short jchar;
typedef signed char jbyte;
typedef short jshort;
typedef void *jobject;
typedef void *jclass;
typedef void *jstring;
typedef void *jarray;
typedef void *jthrowable;
typedef void *jmethodID;
typedef void *jfieldID;
typedef struct _AAssetManager AAssetManager;
typedef struct _AAsset AAsset;
typedef struct _AAssetDir AAssetDir;
typedef struct _ANativeWindow ANativeWindow;
typedef struct _JavaVM JavaVM;
typedef struct _JNIEnv JNIEnv;
struct _JNIEnv {
    void *reserved0; void *reserved1; void *reserved2; void *reserved3;
    void *GetVersion;
    void *DefineClass; void *FindClass;
    void *GetSuperclass; void *IsAssignableFrom;
    void *Throw; void *ThrowNew; void *ExceptionOccurred;
    void *ExceptionDescribe; void *ExceptionClear; void *FatalError;
    void *PushLocalFrame; void *PopLocalFrame;
    void *NewGlobalRef; void *DeleteGlobalRef;
    void *DeleteLocalRef; void *IsSameObject;
    void *NewLocalRef; void *EnsureLocalCapacity;
    void *AllocObject; void *NewObject;
    void *GetObjectClass; void *IsInstanceOf;
    void *GetMethodID; void *CallObjectMethod;
    void *CallBooleanMethod; void *CallIntMethod; void *CallVoidMethod;
    void *GetFieldID; void *GetObjectField; void *GetIntField;
    void *SetObjectField; void *SetIntField;
    void *GetStaticMethodID; void *CallStaticObjectMethod;
    void *GetStaticFieldID; void *GetStaticObjectField;
    void *NewString; void *GetStringLength; void *GetStringChars;
    void *ReleaseStringChars; void *NewStringUTF;
    void *GetStringUTFLength; void *GetStringUTFChars;
    void *ReleaseStringUTFChars;
    void *GetArrayLength; void *NewObjectArray;
    void *GetObjectArrayElement; void *SetObjectArrayElement;
    void *ExceptionCheck;
    void *GetJavaVM;
    void *GetStringRegion; void *GetStringUTFRegion;
    void *GetPrimitiveArrayCritical; void *ReleasePrimitiveArrayCritical;
};
typedef unsigned long pthread_t;
typedef unsigned int pthread_key_t;
typedef struct _va_list va_list;
struct timespec { time_t tv_sec; long tv_nsec; };
struct timeval { time_t tv_sec; suseconds_t tv_usec; };
struct tm { int tm_sec; int tm_min; int tm_hour; int tm_mday; int tm_mon;
            int tm_year; int tm_wday; int tm_yday; int tm_isdst; };
struct stat { unsigned long long st_dev; unsigned long long st_ino;
              unsigned int st_mode; unsigned int st_nlink; unsigned int st_uid;
              unsigned int st_gid; unsigned long long st_rdev;
              long long st_size; long st_blksize; long long st_blocks; };
struct statvfs { unsigned long f_bsize; unsigned long f_frsize;
                 unsigned long long f_blocks; unsigned long long f_bfree;
                 unsigned long long f_bavail; unsigned long long f_files;
                 unsigned long long f_ffree; unsigned long long f_favail;
                 unsigned long f_flag; unsigned long f_namemax; };
struct dirent { unsigned long long d_ino; long long d_off;
                unsigned short d_reclen; unsigned char d_type;
                char d_name[256]; };

extern FILE *fopen(const char *, const char *);
extern FILE *fdopen(int, const char *);
extern int fclose(FILE *);
extern size_t fread(void *, size_t, size_t, FILE *);
extern size_t fwrite(const void *, size_t, size_t, FILE *);
extern int fseek(FILE *, long, int);
extern long ftell(FILE *);
extern int fflush(FILE *);
extern int feof(FILE *);
extern int ferror(FILE *);
extern char *fgets(char *, int, FILE *);
extern int fputs(const char *, FILE *);
extern int fputc(int, FILE *);
extern int fgetc(FILE *);
extern int fprintf(FILE *, const char *, ...);
extern int printf(const char *, ...);
extern int snprintf(char *, size_t, const char *, ...);
extern int sprintf(char *, const char *, ...);
extern int vfprintf(FILE *, const char *, va_list);
extern int vsnprintf(char *, size_t, const char *, va_list);
extern int sscanf(const char *, const char *, ...);
extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);
extern void *memmove(void *, const void *, size_t);
extern int memcmp(const void *, const void *, size_t);
extern size_t strlen(const char *);
extern size_t strnlen(const char *, size_t);
extern int strcmp(const char *, const char *);
extern int strncmp(const char *, const char *, size_t);
extern int strcasecmp(const char *, const char *);
extern int strncasecmp(const char *, const char *, size_t);
extern char *strcpy(char *, const char *);
extern char *strncpy(char *, const char *, size_t);
extern char *strcat(char *, const char *);
extern char *strncat(char *, const char *, size_t);
extern char *strchr(const char *, int);
extern char *strrchr(const char *, int);
extern char *strstr(const char *, const char *);
extern char *strdup(const char *);
extern char *strerror(int);
extern long strtol(const char *, char **, int);
extern unsigned long strtoul(const char *, char **, int);
extern int atoi(const char *);
extern void *malloc(size_t);
extern void *calloc(size_t, size_t);
extern void *realloc(void *, size_t);
extern void free(void *);
extern int remove(const char *);
extern int rename(const char *, const char *);
extern int mkdir(const char *, mode_t);
extern int rmdir(const char *);
extern int stat(const char *, struct stat *);
extern int statvfs(const char *, struct statvfs *);
extern DIR *opendir(const char *);
extern struct dirent *readdir(DIR *);
extern int closedir(DIR *);
extern int clock_gettime(clockid_t, struct timespec *);
extern int usleep(unsigned int);
extern unsigned int sleep(unsigned int);
extern time_t time(time_t *);
extern struct tm *localtime(const time_t *);
extern struct tm *gmtime(const time_t *);
extern size_t strftime(char *, size_t, const char *, const struct tm *);
extern int pthread_mutex_init(pthread_mutex_t *, const void *);
extern int pthread_mutex_destroy(pthread_mutex_t *);
extern int pthread_mutex_lock(pthread_mutex_t *);
extern int pthread_mutex_unlock(pthread_mutex_t *);
extern int pthread_mutex_trylock(pthread_mutex_t *);
extern pthread_t pthread_self(void);
extern int open(const char *, int, ...);
extern int close(int);
extern ssize_t read(int, void *, size_t);
extern ssize_t write(int, const void *, size_t);
extern off_t lseek(int, off_t, int);
extern int ftruncate(int, off_t);
extern int access(const char *, int);
extern char *getcwd(char *, size_t);
extern int chdir(const char *);
extern char *getenv(const char *);
extern int setenv(const char *, const char *, int);
extern int unsetenv(const char *);
extern int errno;
extern int fprintf(FILE *, const char *, ...);
'''
PRELUDE_N = PRELUDE.count('\n')


INCLUDE_DIRS = [
    '.', 'runtime', 'runtime/shims', 'gen',
    'ci/gen_stub/imports', 'android/app/src/main/cpp',
]


def inline_local_includes(path, text, seen, depth=0):
    """Substitui `#include "x.h"` pelo conteúdo do arquivo (recursivo).

    Headers do sistema (`<...>`) ficam para o PRELUDE. Sem isto o pycparser
    vê tipos do projeto (x2rw_ctx) como identificadores soltos e reporta erro
    onde o C está correto.
    """
    if depth > 16:
        return text
    base = os.path.dirname(os.path.abspath(path))
    out = []
    for line in text.split('\n'):
        ls = line.lstrip()
        m = re.match(r'^#\s*include\s+"([^"]+)"', ls)
        if not m:
            out.append(line)
            continue
        name = m.group(1)
        cands = [os.path.join(base, name)] + [
            os.path.join(d, name) for d in INCLUDE_DIRS]
        hit = None
        for c in cands:
            if os.path.isfile(c):
                hit = os.path.abspath(c)
                break
        if not hit or hit in seen:
            out.append('')       # não resolvido: tenta o PRELUDE
            continue
        seen = seen | {hit}
        sub = open(hit, encoding='utf-8', errors='replace').read()
        sub = inline_local_includes(hit, sub, seen, depth + 1)
        out.append('')
        out.append(sub)
        out.append('')
    return '\n'.join(out)


def strip_comments_keep_lines(src):
    """Remove comentários preservando as quebras de linha e o comprimento."""
    out = []
    i, n = 0, len(src)
    state = 'code'
    while i < n:
        c = src[i]
        if state == 'code':
            if c == '/' and i + 1 < n and src[i + 1] == '*':
                state = 'block'
                out.append('  ')
                i += 2
                continue
            if c == '/' and i + 1 < n and src[i + 1] == '/':
                state = 'line'
                out.append('  ')
                i += 2
                continue
            if c == '"':
                state = 'str'
                out.append(c)
                i += 1
                continue
            if c == "'":
                state = 'chr'
                out.append(c)
                i += 1
                continue
            out.append(c)
            i += 1
        elif state == 'block':
            if c == '*' and i + 1 < n and src[i + 1] == '/':
                state = 'code'
                out.append('  ')
                i += 2
                continue
            out.append('\n' if c == '\n' else ' ')
            i += 1
        elif state == 'line':
            if c == '\n':
                state = 'code'
                out.append('\n')
            else:
                out.append(' ')
            i += 1
        elif state == 'str':
            out.append(c)
            if c == '\\' and i + 1 < n:
                out.append(src[i + 1])
                i += 2
                continue
            if c == '"':
                state = 'code'
            i += 1
        elif state == 'chr':
            out.append(c)
            if c == '\\' and i + 1 < n:
                out.append(src[i + 1])
                i += 2
                continue
            if c == "'":
                state = 'code'
            i += 1
    if state in ('block', 'str', 'chr'):
        raise ValueError('arquivo termina com %s aberto' % state)
    return ''.join(out)


def blank_directives(text):
    """Apaga linhas de diretiva (mantendo a numeração)."""
    out = []
    skipping = False
    for line in text.split('\n'):
        if skipping:
            skipping = line.rstrip().endswith('\\')
            out.append('')
            continue
        if line.lstrip().startswith('#'):
            skipping = line.rstrip().endswith('\\')
            out.append('')
            continue
        out.append(line)
    return '\n'.join(out)


CPP_GUARD_OPEN = re.compile(
    r'#[ \t]*ifdef[ \t]+__cplusplus[ \t]*\n[ \t]*extern[ \t]*"C"[ \t]*\{[ \t]*\n'
    r'[ \t]*#[ \t]*endif')
CPP_GUARD_CLOSE = re.compile(
    r'#[ \t]*ifdef[ \t]+__cplusplus[ \t]*\n[ \t]*\}[ \t]*\n'
    r'[ \t]*#[ \t]*endif')


def _blank(m):
    """Troca o trecho por linhas vazias, preservando a numeração."""
    return '\n' * m.group(0).count('\n')


def strip_attribute(t):
    """Remove `__attribute__((...))` com parênteses balanceados.

    Substituir por um identificador (`__attr_x`) fazia uma declaração de topo
    parecer chamada de função, e o pycparser acusava erro onde o C é válido.
    """
    while True:
        m = re.search(r'__attribute__\s*\(', t)
        if not m:
            return t
        i = m.end() - 1
        depth = 0
        j = i
        while j < len(t):
            if t[j] == '(':
                depth += 1
            elif t[j] == ')':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        filler = ''.join('\n' if ch == '\n' else ' ' for ch in t[m.start():j + 1])
        t = t[:m.start()] + filler + t[j + 1:]


def blank_macro_invocations(t, names):
    """Troca `NAME(args)` por `(void)0` para o pycparser conseguir parsear.

    O corpo da macro é checado à parte (check_macro_bodies). Sem isto, uma
    invocação como PADD(uint8_t, dst, src, +) — onde `+` é argumento de macro
    — parece expressão inválida e o arquivo inteiro é rejeitado.
    """
    for name in sorted(names, key=len, reverse=True):
        pat = re.compile(r'\b' + re.escape(name) + r'\s*\(')
        while True:
            m = pat.search(t)
            if not m:
                break
            i = m.end() - 1
            depth = 0
            j = i
            while j < len(t):
                if t[j] == '(':
                    depth += 1
                elif t[j] == ')':
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            if j >= len(t):
                break
            filler = ''.join('\n' if ch == '\n' else ' ' for ch in t[m.start():j + 1])
            t = t[:m.start()] + '(void)0' + filler[6:] + t[j + 1:]
    return t


def macro_names(text):
    return set(re.findall(r'(?m)^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)[ \t]*\(', text))


def transform(src):
    # Os blocos `extern "C" { ... }` existem para o C++ e são C inválido:
    # removidos ANTES de apagar as diretivas, senão sobrariam chaves soltas.
    src = CPP_GUARD_OPEN.sub(_blank, src)
    src = CPP_GUARD_CLOSE.sub(_blank, src)
    t = strip_comments_keep_lines(src)
    t = blank_directives(t)
    # construtos que o pycparser não conhece
    t = strip_attribute(t)
    t = re.sub(r'\b__extension__\b', ' ', t)
    t = re.sub(r'\b__restrict\b', ' ', t)
    t = re.sub(r'\b__inline\b', ' ', t)
    t = re.sub(r'\brestrict\b', ' ', t)
    t = re.sub(r'\binline\b', ' ', t)
    t = re.sub(r'\b__volatile__\b', 'volatile', t)
    t = re.sub(r'\b__asm__\b[^;]*;', lambda m: ';' + ' ' * (len(m.group(0)) - 1), t)
    # Invocações de macro no escopo de arquivo (ex.: X2RW_SHIMS_ALL(X2RW_DECLARE))
    # viram declarações só depois de expandidas; o pycparser não expande.
    t = re.sub(r'(?m)^[ \t]*X2RW_SHIMS_ALL\s*\(.*$', '', t)
    # Macros de jni.h (header de sistema, apagado): sem eles JNIEXPORT/JNICALL
    # sobrariam como identificadores soltos e a declaração viraria inválida.
    t = blank_macro_invocations(t, macro_names(src))
    t = re.sub(r'\bJNIEXPORT\b', ' ', t)
    t = re.sub(r'\bJNICALL\b', ' ', t)
    return t


def check(path, show=2):
    from pycparser import c_parser
    src = open(path, encoding='utf-8', errors='replace').read()
    src = inline_local_includes(path, src, frozenset([os.path.abspath(path)]))
    body = transform(src)
    full = PRELUDE + body
    try:
        c_parser.CParser().parse(full, filename=os.path.basename(path))
        return None
    except Exception as e:
        msg = str(e)
        m = re.search(r':(\d+):(\d+):\s*(.*)', msg)
        if not m:
            coord = getattr(e, 'coord', None)
            if coord:
                line, col = coord[1], coord[2]
                srclines = body.split('\n')
                ctx = []
                for k in range(max(0, line - show - 1), min(len(srclines), line + show)):
                    mark = '>>' if k == line - 1 else '  '
                    ctx.append('%s %5d | %s' % (mark, k + 1, srclines[k][:150]))
                return 'linha %d col %d: %s\n%s' % (
                    line - PRELUDE_N, col, msg.split('\n')[0][:160], '\n'.join(ctx))
            return msg.split('\n')[0][:220]
        line = int(m.group(1)) - PRELUDE_N
        col = int(m.group(2))
        why = m.group(3)[:160]
        srclines = body.split('\n')
        ctx = []
        for k in range(max(0, line - show - 1), min(len(srclines), line + show)):
            mark = '>>' if k == line - 1 else '  '
            ctx.append('%s %5d | %s' % (mark, k + 1, srclines[k][:150]))
        return 'linha %d col %d: %s\n%s' % (line, col, why, '\n'.join(ctx))


DEFAULT_GLOBS = [
    'runtime/*.c', 'runtime/shims/*.c',
    'android/app/src/main/cpp/x2rw_android_main.c', 'desktop/main.c',
]


def _expand(args):
    import glob
    if args == ['--all'] or not args:
        out = []
        for g in DEFAULT_GLOBS:
            out += sorted(glob.glob(g))
        return out
    return args


if __name__ == '__main__':
    files = _expand(sys.argv[1:])
    bad = 0
    for f in files:
        err = check(f)
        if err is None:
            print('OK    %s' % f)
        else:
            bad += 1
            print('FALHA %s\n      %s' % (f, err))
    print('\n%d/%d passaram' % (len(files) - bad, len(files)))
    sys.exit(1 if bad else 0)