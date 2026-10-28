/* X2: Wolverine's Revenge - port Android
 * Interface entre o LAUNCHER (Kotlin/JNI) e o HOST do port.
 *
 * Quem implementa o quê (para não haver dúvida ao integrar):
 *
 *   loader  (x2rw_loader.inc):
 *       mapeia os PE do jogo (Wolvie.exe, binkw32.dll) no espaço de
 *       endereços do guest e registra os módulos. Implementado.
 *
 *   shims Win32  (runtime/shims/x2rw_*.c) — OUTRO ENGENHEIRO:
 *       precisa definir x2rw_import_dispatch() e x2rw_call_import(). Aqui
 *       são apenas DECLARADOS no header do runtime (x2rw_runtime.h).
 *
 *   entrada (x2rw_android_main.c / desktop/main.c):
 *       cria o contexto, chama o loader e dispara o entrypoint do jogo.
 *
 * A assinatura de x2rw_host_init é o contrato central do port:
 *     int x2rw_host_init(const char *game_dir, int width, int height);
 * Retorna 0 em sucesso e != 0 em falha ANTES de executar código do jogo
 * (arquivo ausente, PE inválido, memória insuficiente).
 */
#ifndef X2WR_HOST_H
#define X2WR_HOST_H

#include <stddef.h>
#include <stdint.h>

#include "x2rw_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Contrato do host                                                   */
/* ------------------------------------------------------------------ */

/* Prepara o guest para executar o jogo: mapeia os PE e registra os
 * módulos. NÃO executa código lifted (isso é responsabilidade de quem
 * chama, atrás de X2WR_HAVE_GEN).
 *
 * game_dir: pasta do jogo escolhida pelo usuário (pode ser NULL/vazia —
 *           nesse caso o Android tenta `assets/game/`).
 * width/height: tamanho do SurfaceView; guardado para a camada gráfica.
 * Retorna 0 em sucesso; 1 = contexto/mapeamento falhou. */
int x2rw_host_init(const char *game_dir, int width, int height);

/* Instala os shims Win32 (imports das DLLs). É definida UMA vez por alvo
 * (Android/desktop). Nesta árvore ela é um stub logado — a implementação
 * real dos shims vive em runtime/shims/x2rw_*.c (outro engenheiro). */
void x2rw_host_register_shims(void);

/* Estado de uma tecla, por código de tecla virtual do Windows. É o que os
 * shims de DirectInput/GetAsyncKeyState consultam.
 * Retorna 1 se pressionada, 0 caso contrário. */
int x2rw_host_key_state(int scancode);

/* Seta um código (mesmo formato do nativeSetKey) no mapa do host. Existe
 * para o desktop poder injetar teclas por linha de comando/teste. */
void x2rw_host_key_set(int scancode, int down);

/* Tamanho pedido pela camada Java (SurfaceView). Devolve 0/0 se ainda não
 * houve chamada a nativeSetRenderSize. */
void x2rw_host_get_render_size(int *out_w, int *out_h);

/* RVA do entrypoint lido do PE mapeado (`AddressOfEntryPoint`).
 * Devolve 0 se nenhum módulo primário foi mapeado. NUNCA é hardcoded: o
 * valor 0x1c3042 que aparece em docs/RECON.md é lido do binário em runtime,
 * porque uma release diferente pode ter outro entrypoint. */
uint32_t x2rw_host_entry_rva(void);

/* Contexto do guest criado por x2rw_host_init. NULL antes da init. */
x2rw_ctx *x2rw_host_ctx(void);

/* ------------------------------------------------------------------ */
/* Loader de PE                                                       */
/* ------------------------------------------------------------------ */

/* Resultado do carregamento de um módulo. */
typedef struct {
    uint8_t *image;         /* buffer mapeado (libere com x2rw_free_image) */
    size_t   image_size;    /* tamanho mapeado (SizeOfImage, >= file_size) */
    size_t   file_size;     /* bytes realmente lidos do arquivo            */
    uint32_t base;          /* imagebase usado no guest                    */
    uint32_t entry_rva;     /* AddressOfEntryPoint                         */
    int      primary;       /* 1 = Wolvie.exe (executável)                 */
} x2rw_pe;

/* Lê um arquivo do jogo inteiro. Devolve buffer alocado com malloc (libere
 * com x2rw_mem_free_impl) ou NULL. No Android tenta `assets/game/<name>`
 * primeiro (quando game_dir é NULL/vazio) e depois o filesystem. */
uint8_t *x2rw_read_game_file(const char *game_dir, const char *name,
                             size_t *out_size);

/* Carrega um PE (exe se primary=1, DLL caso contrário) e o mapeia no guest
 * via x2rw_mem_map. Devolve 0 em sucesso. */
int x2rw_load_pe(const char *game_dir, const char *name, uint32_t base,
                 int primary, x2rw_pe *out);

void x2rw_free_image(x2rw_pe *pe);

/* Libera um buffer do loader. Ponto único de liberação: se um dia o Android
 * passar a alocar estes buffers em memória compartilhada/ashmem, só esta
 * função muda (o desktop continua com free()). */
void x2rw_mem_free_impl(void *p);

#ifdef __cplusplus
}
#endif

#endif /* X2WR_HOST_H */
