/* X2: Wolverine's Revenge - port Android
 * Ponto de entrada NATIVO (JNI) do launcher.
 *
 * Papel deste arquivo:
 *   1. receber do Kotlin a pasta do jogo, o tamanho do SurfaceView e o
 *      estado das teclas do overlay de toque;
 *   2. carregar os PE do jogo com o loader compartilhado (x2rw_loader.inc);
 *   3. executar o código lifted, quando ele existir.
 *
 * O QUE É STUB AQUI (sem otimismo, como manda docs/TESTED.md):
 *   - x2rw_host_register_shims(): os shims Win32 reais (D3D8, DirectSound,
 *     DirectInput, winmm...) NÃO existem nesta árvore. A função registra
 *     isso no log e não faz mais nada. Sem shims, mesmo um build FULL vai
 *     falhar no primeiro import — isso é esperado e está documentado.
 *   - não há camada gráfica nem de áudio: o SurfaceView do Kotlin existe
 *     para receber o tamanho da tela, mas nada é desenhado por este código.
 *
 * Dependência externa (declarada em x2rw_runtime.h, implementada em
 * runtime/shims/): x2rw_import_dispatch(). Enquanto os shims não existirem,
 * o link de libx2wr.so falha com símbolo indefinido — falha honesta e
 * visível, melhor do que um stub que finge funcionar.
 */
#include <jni.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "x2rw_runtime.h"
#include "x2rw_host.h"

/* Implementa o loader UMA vez nesta unidade de tradução (o desktop faz o
 * mesmo no seu main.c). */
#define X2WR_LOADER_IMPL
#include "x2rw_loader.inc"

#define X2WR_LOG_TAG "X2WR"
#define X2WR_LOG(...) __android_log_print(ANDROID_LOG_INFO, X2WR_LOG_TAG, __VA_ARGS__)
#define X2WR_ERR(...) __android_log_print(ANDROID_LOG_ERROR, X2WR_LOG_TAG, __VA_ARGS__)

/* ------------------------------------------------------------------ */
/* Estado global                                                      */
/* ------------------------------------------------------------------ */

/* Mapa de teclas: índice = código de tecla virtual do Windows (0..255),
 * valor = pressionada. NÃO é a scancode do hardware: o jogo (via
 * DirectInput/GetAsyncKeyState) raciocina em VK, e o overlay do Kotlin já
 * converte para VK (Wolvie.INI: Punch=75, Jump=57, ...). */
#define X2WR_KEY_MAP_SIZE 256
static volatile unsigned char g_keys[X2WR_KEY_MAP_SIZE];

/* Contexto do guest + imagens mapeadas. Globais porque o jogo roda até o
 * fim do processo: liberar no meio de código lifted seria pior. */
static x2rw_ctx *g_ctx = NULL;
static x2rw_pe   g_wolvie;
static x2rw_pe   g_bink;
static int       g_have_wolvie = 0;
static int       g_have_bink = 0;

static int g_render_w = 0;
static int g_render_h = 0;

/* Cópia da pasta do jogo: o Kotlin só garante a validade da jstring
 * durante a chamada JNI atual. */
static char g_game_dir[1024];

/* JavaVM para anexar threads nativas ao JNI (usado para abrir o
 * AssetManager a partir de qualquer thread). */
static JavaVM *g_vm = NULL;

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    g_vm = vm;
    return JNI_VERSION_1_6;
}

/* ------------------------------------------------------------------ */
/* Acesso ao AssetManager (assets/game/...)                           */
/* ------------------------------------------------------------------ */

/* Obtém o AssetManager da aplicação. Anexa a thread atual se necessário:
 * o AssetManager é guardado apenas durante a chamada, nunca entre threads.
 *
 * O PushLocalFrame/PopLocalFrame não é enfeite: esta função roda na thread
 * do jogo, que fica viva por horas. Sem o frame, cada referência local
 * criada aqui (ActivityThread, Application, AssetManager) ficaria na tabela
 * JNI até a thread morrer, e a tabela estouraria no meio de uma partida. */
static AAssetManager *x2wr_asset_manager(void) {
    if (!g_vm) return NULL;
    JNIEnv *env = NULL;
    int attached = 0;
    jint rc = (*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) return NULL;
        attached = 1;
    } else if (rc != JNI_OK) {
        return NULL;
    }

    AAssetManager *mgr = NULL;
    if ((*env)->PushLocalFrame(env, 8) == JNI_OK) {
        jclass at = (*env)->FindClass(env, "android/app/ActivityThread");
        if (at) {
            jmethodID cur = (*env)->GetStaticMethodID(
                env, at, "currentApplication", "()Landroid/app/Application;");
            if (cur) {
                jobject app = (*env)->CallStaticObjectMethod(env, at, cur);
                if (app) {
                    jmethodID get = (*env)->GetMethodID(
                        env, (*env)->GetObjectClass(env, app), "getAssets",
                        "()Landroid/content/res/AssetManager;");
                    if (get) {
                        jobject am = (*env)->CallObjectMethod(env, app, get);
                        if (am) mgr = AAssetManager_fromJava(env, am);
                    }
                }
            }
        }
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            mgr = NULL;
        }
        (*env)->PopLocalFrame(env, NULL);
    }

    if (attached) (*g_vm)->DetachCurrentThread(g_vm);
    return mgr;
}

/* ------------------------------------------------------------------ */
/* Leitura dos arquivos do jogo                                       */
/* ------------------------------------------------------------------ */

/* Lê o arquivo inteiro para um buffer malloc'ado.
 *
 * Ordem de busca: (1) assets/game/<name> — usado no APK "tudo-em-um";
 * (2) filesystem/<game_dir>/<name> — usado quando o usuário aponta a pasta
 * real do jogo. O AssetManager é preferido apenas quando não há pasta
 * escolhida: assim uma pasta do usuário sempre vence o asset embutido. */
uint8_t *x2rw_read_game_file(const char *game_dir, const char *name,
                             size_t *out_size) {
    if (out_size) *out_size = 0;

    AAssetManager *mgr = x2wr_asset_manager();
    if (!game_dir || !*game_dir) {
        if (mgr) {
            char path[256];
            snprintf(path, sizeof(path), "game/%s", name);
            AAsset *a = AAssetManager_open(mgr, path, AASSET_MODE_BUFFER);
            if (a) {
                size_t n = (size_t)AAsset_getLength(a);
                uint8_t *buf = (uint8_t *)malloc(n ? n : 1);
                if (buf) {
                    int got = AAsset_read(a, buf, n);
                    AAsset_close(a);
                    if (got == (int)n) {
                        if (out_size) *out_size = n;
                        X2WR_LOG("asset game/%s: %lu bytes", name, (unsigned long)n);
                        return buf;
                    }
                    free(buf);
                } else {
                    AAsset_close(a);
                }
                X2WR_ERR("asset game/%s: falha ao ler", name);
            }
        }
        return NULL;
    }

    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", game_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        X2WR_ERR("abrir %s: %s", path, strerror(errno));
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
    X2WR_LOG("%s: %lu bytes", path, (unsigned long)rd);
    return buf;
}

void x2rw_mem_free_impl(void *p) {
    free(p);
}

/* ------------------------------------------------------------------ */
/* Host: init, modules, teclas                                        */
/* ------------------------------------------------------------------ */

int x2rw_host_init(const char *game_dir, int width, int height) {
    g_render_w = width;
    g_render_h = height;

    if (!g_ctx) {
        g_ctx = x2rw_ctx_create();
        if (!g_ctx) {
            X2WR_ERR("x2rw_ctx_create falhou");
            return 1;
        }
        X2WR_LOG("contexto criado: stack 0x%08x..0x%08x",
                 g_ctx->stack_lo, g_ctx->stack_hi);
    }

    /* Cada módulo vai para a imagebase verificada (docs/RECON.md). Se um
     * módulo já foi mapeado antes (segunda chamada de nativeRun), não
     * remapeamos: o guest acumularia duas cópias e os ponteiros antigos
     * apontariam para a primeira. */
    if (!g_have_wolvie) {
        if (x2rw_load_pe(game_dir, "Wolvie.exe", X2RW_BASE_WOLVIE, 1,
                         &g_wolvie) != 0) {
            X2WR_ERR("Wolvie.exe não pôde ser carregado (pasta=%s)",
                     game_dir && *game_dir ? game_dir : "<assets>");
            return 1;
        }
        g_have_wolvie = 1;
        X2WR_LOG("Wolvie.exe mapeado em 0x%08x (entry RVA 0x%x, %lu bytes)",
                 g_wolvie.base, g_wolvie.entry_rva,
                 (unsigned long)g_wolvie.file_size);
    }
    if (!g_have_bink) {
        if (x2rw_load_pe(game_dir, "binkw32.dll", X2RW_BASE_BINK, 0,
                         &g_bink) != 0) {
            X2WR_ERR("binkw32.dll não pôde ser carregado (pasta=%s)",
                     game_dir && *game_dir ? game_dir : "<assets>");
            return 1;
        }
        g_have_bink = 1;
        X2WR_LOG("binkw32.dll mapeado em 0x%08x (%lu bytes)",
                 g_bink.base, (unsigned long)g_bink.file_size);
    }

    /* Só registra módulos se houver código lifted: sem gen/, as tabelas
     * x2rw_funcs_* não existem e o link nem chegaria aqui. */
#ifdef X2WR_HAVE_GEN
    {
        static const x2rw_module *mods[2];
        mods[0] = &x2rw_mod_wolvie;
        mods[1] = &x2rw_mod_bink;
        x2rw_modules_register(mods, 2);
        X2WR_LOG("módulos registrados: %u + %u funções",
                 (unsigned)x2rw_mod_wolvie.n_functions,
                 (unsigned)x2rw_mod_bink.n_functions);
    }
#else
    X2WR_LOG("sem código lifted (build app-only): módulos não registrados");
#endif

    x2rw_host_register_shims();
    return 0;
}

/* FRACA de propósito: runtime/shims/x2rw_imports.c traz a definição FORTE
 * e vence no link. Sem os shims, esta versão fica e apenas registra que
 * nada foi instalado — em vez de fingir sucesso. Ter as duas com ligação
 * forte causaria SÍMBOLO DUPLICADO (foi o que aconteceu na integração). */
__attribute__((weak)) void x2rw_host_register_shims(void) {
    /* STUB CONSCIENTE. O contrato real é este:
     *   - runtime/shims/x2rw_*.c define x2rw_import_dispatch(c, va), que o
     *     runtime chama quando um VA não resolve para função lifted;
     *   - é ali que D3D8/DirectSound/DirectInput/winmm/KERNEL32 viram
     *     implementações reais (dx8vk/Vulkan, SDL2, ...).
     * Não implementamos nada disso aqui de propósito: um stub que devolve
     * "sucesso" faria o jogo seguir com estado inválido e o bug apareceria
     * longe da causa. Sem shims, o primeiro import falha alto. */
    X2WR_LOG("shims Win32: NÃO implementados nesta árvore "
             "(runtime/shims/ é de outro engenheiro)");
}

int x2rw_host_key_state(int scancode) {
    if (scancode < 0 || scancode >= X2WR_KEY_MAP_SIZE) return 0;
    return g_keys[scancode] ? 1 : 0;
}

void x2rw_host_key_set(int scancode, int down) {
    if (scancode < 0 || scancode >= X2WR_KEY_MAP_SIZE) return;
    g_keys[scancode] = down ? 1u : 0u;
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
/* JNI: chamado pelo MainActivity.kt                                  */
/* ------------------------------------------------------------------ */

/* Copia a jstring para g_game_dir. Aceita NULL (limpa a pasta). */
JNIEXPORT void JNICALL
Java_br_com_x2wr_MainActivity_nativeSetGameDir(JNIEnv *env, jobject thiz,
                                               jstring path) {
    (void)thiz;
    if (!path) {
        g_game_dir[0] = '\0';
        X2WR_LOG("pasta do jogo: <nenhuma> (usa assets/game/)");
        return;
    }
    const char *utf = (*env)->GetStringUTFChars(env, path, NULL);
    if (!utf) return;
    snprintf(g_game_dir, sizeof(g_game_dir), "%s", utf);
    (*env)->ReleaseStringUTFChars(env, path, utf);
    X2WR_LOG("pasta do jogo: %s", g_game_dir);
}

/* Guarda o tamanho do SurfaceView. Não redimensiona nada ainda: a camada
 * gráfica (D3D8 -> Vulkan) é trabalho futuro e lerá estes valores. */
JNIEXPORT void JNICALL
Java_br_com_x2wr_MainActivity_nativeSetRenderSize(JNIEnv *env, jobject thiz,
                                                  jint w, jint h) {
    (void)env;
    (void)thiz;
    g_render_w = w;
    g_render_h = h;
    X2WR_LOG("render size: %dx%d", w, h);
}

/* Teclas do overlay de toque. scancode aqui é o código VIRTUAL do Windows
 * (o mesmo do Wolvie.INI), não a scancode de hardware. */
JNIEXPORT void JNICALL
Java_br_com_x2wr_MainActivity_nativeSetKey(JNIEnv *env, jobject thiz,
                                           jint scancode, jboolean down) {
    (void)env;
    (void)thiz;
    x2rw_host_key_set((int)scancode, down ? 1 : 0);
}

/* Informa ao Kotlin se este build contém o código lifted. Existe porque o
 * launcher precisa dizer ao usuário o que está acontecendo ANTES de
 * disparar o jogo, e não depois de um código de erro cru. */
JNIEXPORT jboolean JNICALL
Java_br_com_x2wr_MainActivity_nativeHasGameCode(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
#ifdef X2WR_HAVE_GEN
    return JNI_TRUE;
#else
    return JNI_FALSE;
#endif
}

/* Executa o jogo. Bloqueia até o código lifted retornar — quem chama é uma
 * thread própria (ver MainActivity.kt), NUNCA a thread de UI.
 *
 * Códigos de retorno (contrato com o Kotlin):
 *   0 = o entrypoint retornou sem fault e sem exceção
 *   1 = falha de preparação (PE ausente/ inválido, contexto, memória)
 *   2 = build app-only: não há código lifted para executar
 *   3 = o guest parou com fault (c->fault != 0) — ver port/log.txt */
JNIEXPORT jint JNICALL
Java_br_com_x2wr_MainActivity_nativeRun(JNIEnv *env, jobject thiz) {
    (void)thiz;

    X2WR_LOG("nativeRun: início (pasta=%s, %dx%d)",
             g_game_dir[0] ? g_game_dir : "<assets>",
             (int)g_render_w, (int)g_render_h);

    const char *dir = g_game_dir[0] ? g_game_dir : NULL;
    if (x2rw_host_init(dir, g_render_w, g_render_h) != 0) {
        X2WR_ERR("nativeRun: x2rw_host_init falhou");
        return 1;
    }

#ifndef X2WR_HAVE_GEN
    /* Sem gen/ não existe x2rw_resolve: chamar x2rw_call só produziria
     * X2RW_FAULT_UNRESOLVED. Retornamos 2 com log explícito — o launcher
     * mostra uma mensagem específica para este caso. */
    X2WR_ERR("nativeRun: build SEM código lifted (app-only); "
             "nada a executar. Compile com gen/ para rodar o jogo.");
    return 2;
#else
    uint32_t entry_rva = x2rw_host_entry_rva();
    if (entry_rva == 0) {
        X2WR_ERR("nativeRun: entrypoint RVA ausente (PE não mapeado?)");
        return 1;
    }
    uint32_t entry_va = X2RW_BASE_WOLVIE + entry_rva;
    X2WR_LOG("nativeRun: chamando entrypoint VA=0x%08x (RVA=0x%x lido do PE)",
             entry_va, entry_rva);

    /* O runtime loga via x2rw_log() (stderr). No Android, stderr do
     * processo vai para o logcat com a tag "x2wr"; o Kotlin também espelha
     * o resultado em port/log.txt. */
    x2rw_call(g_ctx, entry_va);

    if (g_ctx->fault) {
        X2WR_ERR("nativeRun: fault code=%u va=0x%08x msg=%s",
                 (unsigned)g_ctx->fault_code, (unsigned)g_ctx->fault_va,
                 g_ctx->fault_msg ? g_ctx->fault_msg : "?");
        return 3;
    }
    X2WR_LOG("nativeRun: entrypoint retornou normalmente");
    return 0;
#endif
}
