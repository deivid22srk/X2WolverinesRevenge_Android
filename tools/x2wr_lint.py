"""Checa estaticamente as duas classes de erro que quebram o build (link):
 1. nome chamado que nao esta declarado em lugar nenhum;
 2. funcao nao-static definida em mais de um .c.
Nao substitui compilador.
"""
import glob
import os
import re
import sys

SRC_GLOBS = ['runtime/*.c', 'runtime/shims/*.c',
             'android/app/src/main/cpp/x2rw_android_main.c',
             'desktop/main.c']
HEADERS = ['runtime/x2rw_runtime.h', 'runtime/x2rw_simd.h',
           'runtime/shims/x2rw_shims.h',
           'android/app/src/main/cpp/x2rw_host.h']

STDLIB = set('''
memcpy memset memmove memcmp strlen strnlen strcmp strncmp strcasecmp
strncasecmp strcpy strncpy strcat strncat strchr strrchr strstr strdup
strerror strtol strtoul atoi atof malloc calloc realloc free remove rename
mkdir rmdir stat statvfs opendir readdir closedir clock_gettime clock
usleep sleep time localtime gmtime strftime mktime
pthread_mutex_init pthread_mutex_destroy pthread_mutex_lock
pthread_mutex_unlock pthread_mutex_trylock pthread_mutexattr_init
pthread_mutexattr_settype pthread_mutexattr_destroy pthread_self
pthread_key_create pthread_key_delete pthread_getspecific pthread_setspecific
pthread_create pthread_join pthread_detach
open close read write lseek ftruncate access getcwd chdir getenv setenv
unsetenv fopen fdopen fclose fread fwrite fseek ftell fflush feof ferror
fgets fputs fputc fgetc fprintf printf snprintf sprintf vfprintf vsnprintf
sscanf abort qsort bsearch toupper tolower isdigit isalpha isspace isalnum
isprint isxdigit getpid gettimeofday
fileno fdopen fseeko ftello rewind clearerr setvbuf unlink perror
localtime_r gmtime_r nanosleep clock_getres sqrtf sqrt powf pow lrintf
lrint fabsf fabs floorf floor ceilf ceil atan2f atan2 log2f log2 sinf sin
cosf cos tanf tan fmodf fmod frexpf frexp
'''.split())

# Builtins do compilador e palavras-chave: nao precisam de declaracao.
BUILTINS = set('''
__attribute__ __restrict __inline __extension__ __volatile__ __int128
__builtin_atan2 __builtin_cos __builtin_fmod __builtin_frexp __builtin_log2
__builtin_pow __builtin_sin __builtin_sqrt __builtin_tan __builtin_alloca
__builtin_expect __builtin_trap __builtin_popcount __builtin_clz __builtin_ctz
__atomic_clear __atomic_test_and_set __atomic_exchange_n
__atomic_compare_exchange_n __atomic_load_n __atomic_store_n
__atomic_add_fetch __atomic_sub_fetch __atomic_and_fetch __atomic_or_fetch
__sync_fetch_and_add __sync_bool_compare_and_swap __sync_synchronize
va_start va_end va_copy va_arg
'''.split())

# Definicoes que vivem em alvos DIFERENTES (Android e desktop) e por isso
# podem repetir sem conflito de link.
PER_TARGET = set('''
x2rw_host_ctx x2rw_host_entry_rva x2rw_host_get_render_size
x2rw_host_register_shims x2rw_read_game_file x2rw_mem_free_impl
x2rw_host_init x2rw_host_shutdown main
x2rw_host_key_set x2rw_host_key_state x2rw_host_key_clear
'''.split())

# Definicoes com __attribute__((weak)) num arquivo e forte em outro: o linker
# escolhe a forte, nao ha conflito. Listadas aqui para o lint nao gritar.
WEAK_OK = set('''
shim_unimplemented x2rw_import_dispatch x2wr_ci_stub_placeholder
'''.split())

# Macros do sistema que pycparser/regex nao veem (headers de sistema).
SYSTEM_MACROS = set('''
S_ISDIR S_ISREG S_ISLNK S_ISCHR S_ISBLK S_ISFIFO S_ISSOCK
O_RDONLY O_WRONLY O_RDWR O_CREAT O_TRUNC O_APPEND O_BINARY
'''.split())

ANDROID = set('''
AAssetManager_fromJava AAssetManager_open AAsset_close AAsset_getLength
AAsset_read AAsset_seek ANativeWindow_fromSurface
AttachCurrentThread DetachCurrentThread GetEnv
__android_log_print __android_log_vprint JNI_OnLoad JNI_OnUnload
'''.split())

KEYWORDS = set('''if while for switch return sizeof defined do else case goto
break continue void int char unsigned signed long short float double const
volatile static extern struct union enum typedef'''.split())


def strip(text):
    t = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    t = re.sub(r'//[^\n]*', ' ', t)
    t = re.sub(r'"(?:\\.|[^"\\])*"', '""', t)
    t = re.sub(r"'(?:\\.|[^'\\])*'", "''", t)
    return t


def declared(text):
    out = set()
    for m in re.finditer(
            r'(?m)^[ \t]*(?:extern[ \t]+)?(?:static[ \t]+)?(?:inline[ \t]+)?'
            r'[A-Za-z_][A-Za-z0-9_ \t\*]*?[\* \t]([A-Za-z_]\w*)[ \t]*\(',
            text):
        out.add(m.group(1))
    return out


def defined(text):
    out = []
    # atributos (`__attribute__((weak))`, `__attribute__((noreturn))`) podem
    # vir ANTES da declaracao e nao mudam a assinatura
    t2 = re.sub(r'__attribute__\s*\(\([^()]*\)\)', ' ', text)
    for m in re.finditer(
            r'(?m)^[ \t]*(static[ \t]+)?(?:inline[ \t]+)?'
            r'[A-Za-z_][A-Za-z0-9_ \t\*]*?[\* \t]([A-Za-z_]\w*)[ \t]*'
            r'\([^;{]*\)[ \t\n]*\{', t2):
        out.append((m.group(2), bool(m.group(1))))
    return out


def called(text):
    out = set()
    for m in re.finditer(r'([A-Za-z_]\w*)\s*\(', text):
        pre = text[max(0, m.start() - 2):m.start()]
        if pre.endswith('->') or pre.endswith('.'):
            continue
        # `algo##suffix(` e colagem de token dentro de #define, nao chamada
        if text[max(0, m.start() - 2):m.start()] == '##':
            continue
        out.add(m.group(1))
    return out


def macros(text):
    joined = re.sub(r'\\\n', ' ', text)
    return set(re.findall(r'(?m)^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)',
                          joined))


def jni_exports(text):
    return set(re.findall(r'Java_[A-Za-z0-9_]+\s*\(', text))


def main():
    paths = []
    for g in SRC_GLOBS:
        paths += sorted(glob.glob(g))

    raw = {}
    for p in paths:
        raw[p] = open(p, encoding='utf-8', errors='replace').read()
    text = {p: strip(t) for p, t in raw.items()}

    known = set()
    for h in HEADERS:
        if os.path.isfile(h):
            known |= declared(strip(open(h, encoding='utf-8').read()))
    mac = set()
    for h in HEADERS:
        if os.path.isfile(h):
            mac |= macros(open(h, encoding='utf-8', errors='replace').read())
    for t in text.values():
        known |= declared(t)
        mac |= macros(t)
    # nomes de export JNI sao definidos com JNIEXPORT/JNICALL na frente, o que
    # engana o regex de declaracao
    mac |= set(x[:-1].strip() for x in
               re.findall(r'Java_[A-Za-z0-9_]+', ' '.join(text.values())))

    undefined = {}
    for p, t in text.items():
        for c in called(t):
            if c in known or c in STDLIB or c in ANDROID or c in mac:
                continue
            if c in BUILTINS or c in SYSTEM_MACROS:
                continue
            if c.startswith('Java_'):
                continue
            if c in KEYWORDS:
                continue
            undefined.setdefault(c, set()).add(p)

    print('=== 1) chamadas sem declaracao ===')
    if undefined:
        for c, w in sorted(undefined.items()):
            print('  %-30s %s' % (c, ', '.join(sorted(w))))
    else:
        print('  nenhuma')

    seen = {}
    for p, t in text.items():
        for name, is_static in defined(t):
            if is_static or name in PER_TARGET or name in WEAK_OK:
                continue
            if name in KEYWORDS:
                continue
            if '#' in name:
                continue
            seen.setdefault(name, set()).add(p)
    dups = {k: v for k, v in seen.items() if len(v) > 1}
    print('\n=== 2) definicoes nao-static duplicadas ===')
    if dups:
        for k, v in sorted(dups.items()):
            print('  %-30s %s' % (k, ', '.join(sorted(v))))
    else:
        print('  nenhuma')

    print('\n%d arquivos; %d indefinidos; %d duplicados'
          % (len(paths), len(undefined), len(dups)))
    return 0 if not undefined and not dups else 1


if __name__ == '__main__':
    sys.exit(main())