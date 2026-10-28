#!/usr/bin/env python3
"""Gera o esqueleto de gen/ para o build APP-ONLY do CI público.

O build app-only existe para que o workflow público produza um APK
instalável sem precisar do código lifted (que é derivado do binário do jogo
e por isso vive apenas no repositório privado). Esse APK:

  * instala;
  * abre;
  * valida a pasta do jogo escolhida pelo usuário;
  * NÃO executa o jogo (as tabelas estão vazias e o app diz isso na tela).

Nada aqui é derivado do jogo: são apenas tabelas vazias com os mesmos
símbolos que o runtime espera, para o linker não reclamar.

Uso:
    python3 tools/gen_stub.py --game x2wr --out ci/gen_stub/gen
"""

from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from games import get_game  # noqa: E402

TEMPLATE = """/* Gerado por tools/gen_stub.py — TABELA VAZIA do build app-only.
 *
 * Este arquivo NÃO contém código do jogo. Ele existe apenas para que o
 * linker resolva os símbolos que o runtime declara, permitindo compilar e
 * instalar o APK sem o código lifted (que é privado).
 *
 * Em build full, gen/ contém as tabelas REAIS e este arquivo não é usado.
 */
#include "x2rw_runtime.h"

const x2rw_fn_desc x2rw_funcs_{mod}[] = {{
    {{ 0u, (x2rw_fn_t)0 }}   /* sentinela: nenhuma função */
}};

const uint32_t x2rw_n_funcs_{mod} = 0u;
const uint32_t x2rw_base_{mod} = {base:#010x}u;
const uint32_t x2rw_size_{mod} = {size:#010x}u;
"""

IMPORTS = """/* Gerado por tools/gen_stub.py — CABEÇALHO DE IMPORTS VAZIO (app-only).
 *
 * NÃO contém dado do jogo: a tabela real de IAT é derivada do binário e vive
 * no repositório privado (gen/). Aqui existe só o suficiente para
 * `runtime/shims/x2rw_imports.c` compilar no repositório público.
 *
 * Com {n} == 0, os laços de `x2rw_imports_patch_iat` e de resolução por VA
 * não executam nenhuma iteração: nada é patchado, nenhum import é resolvido.
 * É o comportamento correto para um build sem código lifted.
 *
 * A entrada-sentinela existe porque C não aceita array de tamanho zero; ela
 * nunca é lida.
 */
#ifndef {guard}
#define {guard}

#include <stdint.h>

typedef struct {{
    uint32_t    va;      /* VA absoluto da entrada na IAT */
    const char *dll;
    const char *name;
}} x2wr_import_t;

/* Sentinela; NÃO representa um import. O laço usa a macro abaixo (0). */
static const x2wr_import_t {arr}[] = {{
    {{ 0u, "stub", "stub" }}
}};

#define {n} 0

#endif /* {guard} */
"""

RUNNER = """/* Gerado por tools/gen_stub.py — ponto de entrada do build app-only. */
#include "x2rw_runtime.h"
#include <stdio.h>

int x2rw_stub_run(void) {{
    /* O app-only não executa o jogo: não há código lifted para chamar.
     * Devolvemos um código distinto para o launcher exibir a mensagem
     * correta em vez de fingir que rodou. */
    fprintf(stderr,
            "[x2wr] build APP-ONLY: sem código lifted do jogo.\\n"
            "[x2wr] Use o build 'full' (com gen/ privado) para executar.\\n");
    return {code};
}}
"""


def main() -> int:
    ap = argparse.ArgumentParser(description="gera stubs de gen/ (app-only)")
    ap.add_argument("--game", required=True)
    ap.add_argument("--out", default="ci/gen_stub/tables")
    ap.add_argument("--imports-out", default=None,
                    help="diretório dos headers de imports vazios "
                         "(default: tables/../imports)")
    args = ap.parse_args()

    game = get_game(args.game)
    os.makedirs(args.out, exist_ok=True)

    imports_dir = args.imports_out or os.path.join(
        os.path.dirname(os.path.abspath(args.out)), "imports")
    os.makedirs(imports_dir, exist_ok=True)

    written = []
    for m in game.modules:
        path = os.path.join(args.out, f"{m.name}_table.c")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(TEMPLATE.format(mod=m.name, base=m.imagebase,
                                     size=m.size_of_image))
        written.append(path)

        # Header de imports vazio: x2rw_imports.c faz `#include
        # "Wolvie_imports.h"` e `#include "binkw32_imports.h"`. Sem estes
        # arquivos o build app-only NÃO COMPILA — foi um erro real, pego ao
        # conferir os includes antes do primeiro build.
        n = m.name.upper()
        ipath = os.path.join(imports_dir, f"{m.name}_imports.h")
        with open(ipath, "w", encoding="utf-8") as fh:
            fh.write(IMPORTS.format(
                guard=f"X2WR_IMPORTS_{n}_H",
                arr=f"x2wr_imports_{m.name}",
                n=f"X2WR_N_IMPORTS_{n}"))
        written.append(ipath)

    for w in written:
        print(f"[gen_stub] {w}")
    print(f"[gen_stub] {len(written)} arquivos de stub gerados "
          f"(nenhum derivado do jogo)")
    print("[gen_stub] Estas tabelas VAZIAS existem para o build app-only "
          "ligar sem gen/. Elas não executam nada.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
