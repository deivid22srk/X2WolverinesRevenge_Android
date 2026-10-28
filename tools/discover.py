#!/usr/bin/env python3
"""Discovery de funções e imports para os módulos PE de um jogo.

Pipeline (Fase 2):
  1. Lê o PE com pefile: seções, imports, IAT, entrypoint.
  2. Varre o .text com capstone procurando:
       - prologues clássicos (push ebp; mov ebp,esp) / (push ebx; push esi; ...)
       - alvos de CALL/JMP relativos dentro do módulo
       - thunks da IAT (endereços de imports)
  3. Consolida candidatos, filtra por tamanho mínimo plausível e grava:
       - gen/<modulo>_funcs.json   (lista de funções: rva, tamanho, flags)
       - gen/<modulo>_imports.h    (tabela de imports com nomes)
       - analysis/<modulo>_map.txt (mapa de endereços, para documentação)

Nada aqui é código do jogo: são tabelas de ENDEREÇOS e NOMES de API.
Os endereços vêm dos binários que o usuário possui; o resultado é
considerado "derivado" e por isso vive em gen/ (repositório privado).

Uso:
    python3 tools/discover.py --game x2wr --exe <Wolvie.exe> --dll <binkw32.dll> --out gen
    python3 tools/discover.py --game x2wr --exe <Wolvie.exe> --lift --out gen
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time

try:
    import pefile
except ImportError:
    sys.exit("pefile não instalado: pip install pefile")

try:
    import capstone
except ImportError:
    sys.exit("capstone não instalado: pip install capstone")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from games import GameConfig, Module, get_game  # noqa: E402

# --------------------------------------------------------------------------
# Assinaturas de prologue x86 de 32 bits.
# --------------------------------------------------------------------------
PROLOGUE_PATTERNS = (
    b"\x55\x8b\xec",              # push ebp; mov ebp, esp  (frame padrão)
    b"\x55\x89\xe5",              # push ebp; mov ebp, esp  (AT&T/GCC)
    b"\x55\x8b\xec\x83\xec",      # push ebp; mov ebp,esp; sub esp, imm
    b"\x53\x56\x57",              # push ebx; push esi; push edi (GCC callee-saved)
    b"\x53\x83\xec",              # push ebx; sub esp, imm
    b"\x56\x57",                  # push esi; push edi
    b"\x51\x52",                  # push ecx; push edx
    b"\x83\xec",                  # sub esp, imm8
    b"\x8b\xff\x55",              # mov edi,edi; push ebp
    b"\x6a\x00\x68",              # push 0; push imm32 (WinMainCRTStartup)
)

# prologues de kernel: bypass (usado pelo shellcode, raro em jogo)
KERNEL_PROLOGUE = b"\x8b\xff"

MIN_FUNC_BYTES = 8
MAX_FUNC_BYTES = 0x40000


def _module_candidates(pe: pefile.PE, m: Module) -> tuple[dict[int, str], list[int]]:
    """Retorna (mapa de IAT slot->nome da API, alvos de chamada internos)."""
    iat: dict[int, str] = {}
    if hasattr(pe, "DIRECTORY_ENTRY_IMPORT"):
        for dll in pe.DIRECTORY_ENTRY_IMPORT:
            dllname = (dll.dll or b"").decode("latin-1", "replace")
            for imp in dll.imports:
                name = (imp.name or b"").decode("latin-1", "replace")
                if not name:
                    name = f"ordinal_{imp.ordinal}"
                # imp.address é o VA absoluto da entrada na IAT
                iat[imp.address] = f"{dllname}!{name}"
    return iat, []


def discover_module(path: str, m: Module, lift: bool = False) -> dict:
    """Analisa um PE e devolve a tabela de funções/imports."""
    t0 = time.time()
    # parse completo: precisamos de imports + exports para montar a IAT
    pe = pefile.PE(path)

    iat, _ = _module_candidates(pe, m)
    image = pe.get_memory_mapped_image()
    ep = pe.OPTIONAL_HEADER.AddressOfEntryPoint

    # limites do .text
    text = None
    for s in pe.sections:
        if s.Name.startswith(b".text"):
            text = s
            break
    if text is None:
        text = pe.sections[0]

    text_va = m.imagebase + text.VirtualAddress
    text_start = text.VirtualAddress
    text_end = text.VirtualAddress + max(text.Misc_VirtualSize, text.SizeOfRawData)
    code = bytes(text.get_data())

    funcs: dict[int, int] = {}  # rva -> flags
    flags = {
        "ENTRY": 1,
        "IAT_THUNK": 2,
        "PROLOGUE": 4,
        "CALL_TARGET": 8,
        "EXPORT": 16,
    }

    def mark(va: int, flag: int) -> None:
        rva = va - m.imagebase
        if text_start <= rva < text_end:
            funcs[rva] = funcs.get(rva, 0) | flag

    mark(m.imagebase + ep, flags["ENTRY"])

    # thunks da IAT (jmp dword ptr [iat])
    for va in iat:
        mark(va, flags["IAT_THUNK"])

    # exports (binkw32 exporta símbolos)
    if hasattr(pe, "DIRECTORY_ENTRY_EXPORT") and pe.DIRECTORY_ENTRY_EXPORT:
        base = pe.DIRECTORY_ENTRY_EXPORT
        for e in base.symbols:
            if e.address:
                mark(m.imagebase + e.address, flags["EXPORT"])

    # prologues
    for pat in PROLOGUE_PATTERNS:
        start = 0
        while True:
            idx = code.find(pat, start)
            if idx == -1:
                break
            mark(text_va + idx, flags["PROLOGUE"])
            start = idx + 1

    # alvos de call/jmp relativos
    #
    # A varredura linear precisa ser RESILIENTE: `md.disasm` para no primeiro
    # byte que não decodifica, e `.text` tem dados/padding no meio. Sem
    # ressincronizar, a varredura morria cedo e o conjunto de alvos de `call`
    # saía praticamente vazio — medido neste binário: apenas 8 funções de
    # 6.525 marcadas como CALL. Como o tamanho de cada função é a distância
    # até o próximo início descoberto, todo o resto passava a ser delimitado
    # por "prólogos" achados por busca cega de bytes (falsos positivos dentro
    # de funções reais) — e as funções ficavam truncadas no meio do corpo.
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = False
    off = 0
    n_code = len(code)
    while off < n_code:
        insns = list(md.disasm(code[off:off + 4096], text_va + off))
        if not insns:
            off += 1              # ressincroniza no byte seguinte
            continue
        for insn in insns:
            if insn.mnemonic in ("call", "jmp"):
                op = insn.op_str
                if op.startswith("0x"):
                    try:
                        target = int(op, 16)
                    except ValueError:
                        continue
                    # call rel32 para dentro do próprio módulo
                    if m.imagebase + text_start <= target < m.imagebase + text_end:
                        mark(target, flags["CALL_TARGET"])
        off = (insns[-1].address + insns[-1].size) - text_va

    # ---- funde inícios próximos antes de calcular tamanhos ----
    #
    # BUG MEDIDO: o filtro de tamanho mínimo descartava a função REAL quando
    # um prólogo FALSO caía poucos bytes depois dela. Caso concreto neste
    # binário: `call 0x5cbddc` (prólogo legítimo: push ebp; mov ebp,esp;
    # sub esp,0x48; push ebx/esi/edi) e um falso início em 0x5cbde3, 7 bytes
    # adiante, no MEIO de `push ebx`. Distância 7 < MIN_FUNC_BYTES -> a função
    # de verdade era removida da tabela e o código liftado chamava um VA que
    # não existia em lugar nenhum ("call para VA não resolvido").
    #
    # Regra: quem manda é a evidência. CALL_TARGET/ENTRY/EXPORT/IAT_THUNK são
    # fortes; PROLOGUE é palpite por busca de bytes. Se só um dos dois for
    # forte, o fraco sai — e o forte herda os bytes.
    FORTE = (flags["ENTRY"] | flags["IAT_THUNK"] | flags["CALL_TARGET"]
             | flags["EXPORT"])
    limpos: list[int] = []
    for rva in sorted(funcs):
        if limpos and rva - limpos[-1] < MIN_FUNC_BYTES:
            ant = limpos[-1]
            ant_forte = bool(funcs[ant] & FORTE)
            novo_forte = bool(funcs[rva] & FORTE)
            if ant_forte and not novo_forte:
                continue                    # o novo é prólogo falso
            if novo_forte and not ant_forte:
                limpos[-1] = rva            # o anterior é que era falso
                continue
        limpos.append(rva)

    # ---- monta a tabela final com tamanhos ----
    rvas = limpos
    out_funcs = []
    for idx, rva in enumerate(rvas):
        nxt = rvas[idx + 1] if idx + 1 < len(rvas) else text_end
        size = nxt - rva
        if size < MIN_FUNC_BYTES or size > MAX_FUNC_BYTES:
            # Pequena demais (share de tail-call/padding) ou grande demais.
            # Só descarta palpite fraco: alvo de call/export NUNCA pode ser
            # jogado fora — foi isso que apagou 0x5cbddc da tabela.
            if not (funcs[rva] & FORTE):
                continue
            size = min(max(size, MIN_FUNC_BYTES), MAX_FUNC_BYTES)
        out_funcs.append(
            {
                "rva": rva,
                "va": m.imagebase + rva,
                "size": size,
                "flags": funcs[rva],
                "name": _guess_name(rva, funcs[rva], iat, flags),
            }
        )

    elapsed = time.time() - t0
    pe.close()

    return {
        "module": m.name,
        "filename": m.filename,
        "imagebase": m.imagebase,
        "size_of_image": m.size_of_image,
        "entrypoint_rva": ep,
        "imports": {hex(k): v for k, v in sorted(iat.items())},
        "functions": out_funcs,
        "stats": {
            "discover_seconds": round(elapsed, 3),
            "n_functions": len(out_funcs),
            "n_imports": len(iat),
            "text_size": text_end - text_start,
        },
    }


def _guess_name(rva: int, fl: int, iat: dict[int, str], flags: dict[str, int]) -> str:
    if fl & flags["ENTRY"]:
        return "entry_point"
    if fl & flags["EXPORT"]:
        return f"export_{rva:x}"
    if fl & flags["IAT_THUNK"]:
        return f"iat_{rva:x}"
    return f"sub_{rva:x}"


def write_imports_header(result: dict, out_dir: str) -> str:
    """Gera um .h com a tabela de imports (endereço -> nome de API)."""
    mod = result["module"]
    path = os.path.join(out_dir, f"{mod}_imports.h")
    lines = [
        f"/* Gerado por tools/discover.py - NÃO EDITAR À MÃO. */",
        f"/* Módulo: {result['filename']}  base={result['imagebase']:#010x} */",
        "#ifndef X2WR_IMPORTS_%s_H" % mod.upper(),
        "#define X2WR_IMPORTS_%s_H" % mod.upper(),
        "",
        "#include <stdint.h>",
        "",
        "typedef struct {",
        "    uint32_t    va;      /* VA absoluto da entrada na IAT */",
        "    const char *dll;",
        "    const char *name;",
        "} x2wr_import_t;",
        "",
        f"/* {result['stats']['n_imports']} imports */",
        "static const x2wr_import_t x2wr_imports_%s[] = {" % mod,
    ]
    for va_hex, full in result["imports"].items():
        va = int(va_hex, 16)
        dll, _, name = full.partition("!")
        lines.append(f'    {{ {va:#010x}u, "{dll}", "{name}" }},')
    lines += [
        "};",
        "",
        f"#define X2WR_N_IMPORTS_{mod.upper()} {result['stats']['n_imports']}",
        "",
        "#endif /* X2WR_IMPORTS_%s_H */" % mod.upper(),
        "",
    ]
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    return path


def write_map(result: dict, analysis_dir: str) -> str:
    """Grava o mapa de endereços (documentação de endereçamento)."""
    os.makedirs(analysis_dir, exist_ok=True)
    path = os.path.join(analysis_dir, f"{result['module']}_map.txt")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(f"# Mapa de endereços — {result['filename']}\n")
        fh.write(f"# base = {result['imagebase']:#010x}  end = "
                 f"{result['imagebase'] + result['size_of_image']:#010x}\n")
        fh.write(f"# entrypoint rva = {result['entrypoint_rva']:#x}\n")
        fh.write(f"# funções descobertas = {result['stats']['n_functions']}\n")
        fh.write(f"# imports = {result['stats']['n_imports']}\n")
        fh.write(f"# tempo = {result['stats']['discover_seconds']}s\n\n")
        fh.write("rva        va          size   flags  nome\n")
        for f in result["functions"]:
            fh.write(
                f"{f['rva']:#010x}  {f['va']:#010x}  {f['size']:6d}  "
                f"{f['flags']:5d}  {f['name']}\n"
            )
    return path


def main() -> int:
    ap = argparse.ArgumentParser(description="discovery de funções PE")
    ap.add_argument("--game", required=True, help="id do jogo (x2wr)")
    ap.add_argument("--exe", help="caminho do executável principal")
    ap.add_argument("--dll", action="append", default=[], help="DLLs adicionais")
    ap.add_argument("--out", default="gen", help="pasta de saída (gen/)")
    ap.add_argument("--analysis", default="analysis", help="pasta de análise")
    ap.add_argument("--lift", action="store_true",
                    help="alias: executa a varredura completa (o lifting em si "
                         "é feito por tools/lifter.py)")
    args = ap.parse_args()

    game: GameConfig = get_game(args.game)
    os.makedirs(args.out, exist_ok=True)

    paths: list[tuple[str, Module]] = []
    if args.exe:
        mod = game.module("Wolvie")
        paths.append((args.exe, mod))
    for d in args.dll:
        base = os.path.basename(d).lower()
        mod = next((m for m in game.modules if m.filename.lower() == base), None)
        if mod is None:
            print(f"[discover] AVISO: DLL não mapeada em games.py: {d}")
            continue
        paths.append((d, mod))

    if not paths:
        print("[discover] nenhum módulo informado (--exe/--dll)")
        return 1

    for path, mod in paths:
        if not os.path.isfile(path):
            print(f"[discover] ERRO: não encontrado: {path}")
            return 1
        print(f"[discover] {mod.filename} (base {mod.imagebase:#010x})...")
        res = discover_module(path, mod, lift=args.lift)
        json_path = os.path.join(args.out, f"{mod.name}_funcs.json")
        with open(json_path, "w", encoding="utf-8") as fh:
            json.dump(res, fh, indent=1)
        hdr = write_imports_header(res, args.out)
        mp = write_map(res, args.analysis)
        st = res["stats"]
        print(
            f"           funções={st['n_functions']} imports={st['n_imports']} "
            f"tempo={st['discover_seconds']}s"
        )
        print(f"           -> {json_path}")
        print(f"           -> {hdr}")
        print(f"           -> {mp}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
