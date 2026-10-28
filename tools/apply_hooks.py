#!/usr/bin/env python3
"""Aplica a lista de substituições por endereço fixo (runtime/override_list.c).

Lê os VAs declarados em `x2rw_overrides[]` e REMOVE das tabelas geradas
(`gen/<mod>_table.c`) as entradas correspondentes. Com a entrada fora da
tabela, `x2rw_resolve()` não encontra a versão lifted e a chamada é
interceptada pela camada de host (o port conecta o VA à função nativa).

Por que remover em vez de marcar: a tabela é consultada por busca binária,
e o runtime exige que esteja ordenada por VA. Remover mantém a ordenação e
deixa explícito no arquivo gerado que aquela função não é reexecutada.

Uso:
    python3 tools/apply_hooks.py --game x2wr --out gen \\
        --overrides runtime/override_list.c
    python3 tools/apply_hooks.py --game x2wr --out gen --report-only
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from games import get_game  # noqa: E402

ENTRY_RE = re.compile(
    r"\{\s*(0x[0-9a-fA-F]+)u?\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,"
    r"\s*(?:\"([^\"]*)\"|\(const char \*\)0)\s*,"
    r"\s*(?:\"([^\"]*)\"|\(const char \*\)0)\s*\}",
)


def parse_overrides(path: str) -> list[dict]:
    """Extrai as entradas {va, fn, module, why} de override_list.c."""
    if not os.path.isfile(path):
        print(f"[hooks] {path} não existe — nada a aplicar")
        return []
    txt = open(path, encoding="utf-8").read()
    out = []
    for m in ENTRY_RE.finditer(txt):
        va = int(m.group(1), 16)
        if va == 0:
            continue                       # sentinela
        out.append({"va": va, "fn": m.group(2),
                    "module": m.group(3) or "", "why": m.group(4) or ""})
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="aplica overrides de host")
    ap.add_argument("--game", required=True)
    ap.add_argument("--out", default="gen")
    ap.add_argument("--overrides", default="runtime/override_list.c")
    ap.add_argument("--report-only", action="store_true",
                    help="só relata a cobertura, não altera nada")
    args = ap.parse_args()

    game = get_game(args.game)

    # Cobertura: lê o resumo do lifter, se houver.
    summary_path = os.path.join(args.out, "lift_summary.json")
    if os.path.isfile(summary_path):
        s = json.load(open(summary_path, encoding="utf-8"))
        total_f = sum(m["n_functions"] for m in s.get("modules", []))
        total_u = sum(m["unsupported"] for m in s.get("modules", []))
        print(f"[hooks] lifter: {total_f} funções, "
              f"{total_u} instruções não suportadas")
        for m in s.get("modules", []):
            print(f"[hooks]   {m['module']}: {m['n_functions']} funções, "
                  f"{m['unsupported']} não suportadas")

    if args.report_only:
        return 0

    ov = parse_overrides(args.overrides)
    print(f"[hooks] {len(ov)} substituição(ões) declarada(s) em {args.overrides}")

    applied = 0
    for entry in ov:
        if not entry["why"]:
            print(f"[hooks] AVISO: override em {entry['va']:#x} sem motivo "
                  f"declarado — ignorado (regra 2 da override_list.c)")
            continue
        # Qual módulo é dono desse VA?
        owner = None
        for m in game.modules:
            if m.imagebase <= entry["va"] < m.imagebase + m.size_of_image:
                owner = m
                break
        if owner is None:
            print(f"[hooks] AVISO: {entry['va']:#x} fora de qualquer módulo "
                  f"conhecido — ignorado")
            continue
        if entry["module"] and entry["module"] != owner.name:
            print(f"[hooks] AVISO: {entry['va']:#x} pertence a {owner.name}, "
                  f"mas a lista diz {entry['module']} — usando {owner.name}")

        tpath = os.path.join(args.out, f"{owner.name}_table.c")
        if not os.path.isfile(tpath):
            print(f"[hooks] AVISO: {tpath} ausente — pulando {entry['va']:#x}")
            continue
        txt = open(tpath, encoding="utf-8").read()
        pat = re.compile(
            r"^\s*\{\s*" + f"{entry['va']:#010x}" + r"u\s*,\s*[A-Za-z0-9_]+\s*\},\s*\n",
            re.MULTILINE)
        new, n = pat.subn("", txt)
        if n == 0:
            print(f"[hooks] AVISO: {entry['va']:#x} não estava na tabela de "
                  f"{owner.name} (já removido?)")
            continue
        # Corrige a contagem declarada no fim do arquivo.
        new = re.sub(
            r"(const uint32_t x2rw_n_funcs_" + owner.name + r" = )(\d+)(u;)",
            lambda mm: mm.group(1) + str(int(mm.group(2)) - n) + mm.group(3),
            new)
        new = new.replace(
            f"/* Tabela de funções de {owner.name}: VA -> ponteiro */",
            f"/* Tabela de funções de {owner.name}: VA -> ponteiro.\n"
            f" * tools/apply_hooks.py removeu {n} entrada(s) substituída(s) "
            f"pelo host:\n"
            + "".join(f" *   {e['va']:#010x} -> {e['fn']}  ({e['why']})\n"
                      for e in ov if e is entry)
            + " */")
        open(tpath, "w", encoding="utf-8").write(new)
        applied += n
        print(f"[hooks] {entry['va']:#010x} -> {entry['fn']} "
              f"(removido da tabela de {owner.name})")

    print(f"[hooks] {applied} entrada(s) removida(s) das tabelas")
    print("[hooks] lembre: cada override tira uma função do recompilado real; "
          "registre o motivo em docs/TESTED.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
