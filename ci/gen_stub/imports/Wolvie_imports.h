/* Gerado por tools/gen_stub.py — CABEÇALHO DE IMPORTS VAZIO (app-only).
 *
 * NÃO contém dado do jogo: a tabela real de IAT é derivada do binário e vive
 * no repositório privado (gen/). Aqui existe só o suficiente para
 * `runtime/shims/x2rw_imports.c` compilar no repositório público.
 *
 * Com X2WR_N_IMPORTS_WOLVIE == 0, os laços de `x2rw_imports_patch_iat` e de resolução por VA
 * não executam nenhuma iteração: nada é patchado, nenhum import é resolvido.
 * É o comportamento correto para um build sem código lifted.
 *
 * A entrada-sentinela existe porque C não aceita array de tamanho zero; ela
 * nunca é lida.
 */
#ifndef X2WR_IMPORTS_WOLVIE_H
#define X2WR_IMPORTS_WOLVIE_H

#include <stdint.h>

typedef struct {
    uint32_t    va;      /* VA absoluto da entrada na IAT */
    const char *dll;
    const char *name;
} x2wr_import_t;

/* Sentinela; NÃO representa um import. O laço usa a macro abaixo (0). */
static const x2wr_import_t x2wr_imports_Wolvie[] = {
    { 0u, "stub", "stub" }
};

#define X2WR_N_IMPORTS_WOLVIE 0

#endif /* X2WR_IMPORTS_WOLVIE_H */
