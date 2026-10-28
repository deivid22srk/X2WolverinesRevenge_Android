#!/usr/bin/env python3
"""Autoteste do lifter: código recompilado (C) vs execução x86 real (unicorn).

O ponto deste programa é NÃO confiar no lifter. Ele pega funções reais do
jogo, executa cada uma de duas formas independentes:

  A) no unicorn, que emula a CPU x86-32 de verdade — este é o ORÁCULO;
  B) no C gerado por tools/lifter.py, compilado no host.

E compara o estado final (registradores, EFLAGS e uma janela de memória).
Divergência = bug no lifter (ou no runtime). O resultado é uma TAXA MEDIDA,
não uma afirmação.

Limitação honesta: só dá para testar funções que (1) não chamam imports do
Windows, (2) não dependem de dados externos e (3) terminam sozinhas. Ou
seja, a taxa medida cobre um subconjunto do jogo — ela diz "entre as funções
testáveis, X% batem", e NÃO "o jogo funciona".

Uso:
    python3 tools/difftest.py --game x2wr --exe "…/Wolvie.exe" --count 200
    python3 tools/difftest.py --game x2wr --exe "…/Wolvie.exe" --seed 1
"""

from __future__ import annotations

import argparse
import json
import os
import random
import subprocess
import sys
import textwrap
import time

try:
    import capstone
except ImportError:
    sys.exit("capstone não instalado: pip install capstone")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from games import GameConfig, get_game  # noqa: E402

try:
    import unicorn
except ImportError:
    unicorn = None

# Endereços de trabalho no espaço do teste (não colidem com o módulo)
STACK_BASE = 0x00100000
STACK_TOP = 0x00200000
SCRATCH = 0x00300000
SCRATCH_SIZE = 0x1000

# Instruções que tornam a função não testável de forma isolada
BAIL_MNEMONICS = {
    "call", "int", "int3", "sysenter", "cpuid", "rdtsc", "rdtscp",
    "in", "out", "hlt", "iret", "iretd", "retf", "enter", "leave",
}
# Nomes de import: qualquer `call` já basta para descartar, mas listamos
# para clareza do relatório.


def load_image(exe: str) -> bytes:
    import pefile
    pe = pefile.PE(exe)
    img = pe.get_memory_mapped_image()
    pe.close()
    return img


def pick_functions(funcs: list[dict], image: bytes, md, count: int,
                   rng: random.Random) -> list[dict]:
    """Escolhe funções 'fechadas': sem call, sem instrução privilegiada."""
    ok = []
    for f in funcs:
        size = f["size"]
        if size < 8 or size > 512:
            continue
        code = image[f["rva"]:f["rva"] + size]
        good = True
        for insn in md.disasm(code, f["va"]):
            if insn.mnemonic in BAIL_MNEMONICS:
                good = False
                break
            if insn.mnemonic.startswith("j") and insn.operands and \
                    insn.operands[0].type == capstone.x86.X86_OP_IMM:
                tgt = insn.operands[0].imm & 0xffffffff
                if not (f["va"] <= tgt < f["va"] + size):
                    good = False
                    break
            if insn.mnemonic == "jmp" and insn.operands and \
                    insn.operands[0].type != capstone.x86.X86_OP_IMM:
                good = False          # salto indireto: pula
                break
        if good:
            ok.append(f)
    rng.shuffle(ok)
    return ok[:count]


# ---------------------------------------------------------------------------
# Lado A: oráculo unicorn
# ---------------------------------------------------------------------------

def run_unicorn(image: bytes, imagebase: int, f: dict, init_regs: dict,
                init_mem: bytes) -> dict | None:
    """Executa a função no unicorn e devolve o estado final."""
    if unicorn is None:
        return None
    try:
        from unicorn import (Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE,
                             UcError)
        from unicorn.x86_const import (
            UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
            UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI,
            UC_X86_REG_EFLAGS,
        )
    except ImportError:
        return None

    mu = Uc(UC_ARCH_X86, UC_MODE_32)
    MAP = 0x100000
    # Mapeia o módulo inteiro, arredondado para 1 MiB
    msize = ((len(image) + MAP - 1) // MAP) * MAP
    mu.mem_map(imagebase, msize)
    mu.mem_write(imagebase, image)

    # Pilha e área de rascunho
    mu.mem_map(STACK_BASE, STACK_TOP - STACK_BASE)
    mu.mem_map(SCRATCH, MAP)
    # Dados iniciais determinísticos
    mu.mem_write(SCRATCH, init_mem)
    # Pilha com um "retorno" falso: o unicorn para quando alcança o
    # sentinela (endereço inválido) OU quando o contador de instruções
    # estoura. Usamos um endereço não mapeado para provocar parada.
    ret_sentinel = 0x0BADF000
    esp = STACK_TOP - 0x100
    mu.mem_write(esp, ret_sentinel.to_bytes(4, "little"))

    regs = dict(init_regs)
    regs["esp"] = esp
    regmap = {
        "eax": UC_X86_REG_EAX, "ecx": UC_X86_REG_ECX, "edx": UC_X86_REG_EDX,
        "ebx": UC_X86_REG_EBX, "esp": UC_X86_REG_ESP, "ebp": UC_X86_REG_EBP,
        "esi": UC_X86_REG_ESI, "edi": UC_X86_REG_EDI,
    }
    for k, v in regs.items():
        mu.reg_write(regmap[k], v)
    mu.reg_write(UC_X86_REG_EFLAGS, init_regs.get("eflags", 0x202))

    steps = [0]
    done = [False]

    def on_code(uc, addr, size, user):
        steps[0] += 1
        if addr == ret_sentinel or steps[0] > 20000:
            done[0] = True
            uc.emu_stop()

    mu.hook_add(UC_HOOK_CODE, on_code)
    try:
        mu.emu_start(f["va"], ret_sentinel, count=20000)
    except UcError:
        pass
    if steps[0] > 20000:
        return None                    # provavelmente laço infinito: ignora

    out = {k: mu.reg_read(regmap[k]) for k in regmap}
    out["eflags"] = mu.reg_read(UC_X86_REG_EFLAGS)
    out["mem"] = mu.mem_read(SCRATCH, SCRATCH_SIZE)
    return out


# ---------------------------------------------------------------------------
# Lado B: harness C com o código recompilado
# ---------------------------------------------------------------------------

HARNESS_HEAD = r'''
/* Gerado por tools/difftest.py — compara C recompilado vs unicorn. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "x2rw_runtime.h"
#include "x2rw_simd.h"

extern const x2rw_module x2rw_mod_wolvie;

#define SCRATCH 0x00300000u
#define SCRATCH_SIZE 0x1000u

typedef struct {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi, eflags;
    uint8_t  mem[SCRATCH_SIZE];
} state_t;

static void snapshot(x2rw_ctx *c, state_t *s) {
    s->eax = c->r.eax; s->ecx = c->r.ecx; s->edx = c->r.edx;
    s->ebx = c->r.ebx; s->esp = c->r.esp; s->ebp = c->r.ebp;
    s->esi = c->r.esi; s->edi = c->r.edi; s->eflags = c->eflags;
    for (uint32_t i = 0; i < SCRATCH_SIZE; i++)
        s->mem[i] = x2rw_read8(c, SCRATCH + i);
}

int main(void) {
    uint8_t init[SCRATCH_SIZE];
    for (int i = 0; i < (int)sizeof(init); i++) init[i] = (uint8_t)(i * 7 + 3);
    int pass = 0, fail = 0, skip = 0;
    printf("va,status,detalhe\n");
'''

HARNESS_TAIL = r'''
    printf("# resumo,%d,%d,%d\n", pass, fail, skip);
    return fail == 0 ? 0 : 1;
}
'''


def gen_harness(cases: list[dict]) -> str:
    """Gera um caso de teste em C por função."""
    parts = [HARNESS_HEAD]
    for i, cs in enumerate(cases):
        f = cs["func"]
        regs = cs["regs"]
        name = f"x2rw_wolvie_{f['rva']:x}"
        parts.append(textwrap.dedent(f'''
        /* ---- caso {i}: {f['name']} VA={f['va']:#x} ---- */
        {{
            x2rw_ctx *c = x2rw_ctx_create();
            x2rw_modules_register((const x2rw_module *[]){{ &x2rw_mod_wolvie }}, 1);
            for (uint32_t i = 0; i < SCRATCH_SIZE; i++)
                x2rw_write8(c, SCRATCH + i, init[i]);
            c->r.eax = {regs['eax']:#x}u; c->r.ecx = {regs['ecx']:#x}u;
            c->r.edx = {regs['edx']:#x}u; c->r.ebx = {regs['ebx']:#x}u;
            c->r.ebp = {regs['ebp']:#x}u; c->r.esi = {regs['esi']:#x}u;
            c->r.edi = {regs['edi']:#x}u; c->eflags = {regs['eflags']:#x}u;
            /* O esp do harness começa no topo da pilha do runtime; o vetor
             * de teste do unicorn usou o mesmo layout, então repomos. */
            c->r.esp = c->stack_hi - 0x100u;
            x2rw_write32(c, c->r.esp, 0x0BADF000u);

            {name}(c);

            state_t s; snapshot(c, &s);
            /* Compara com o esperado calculado pelo unicorn */
            int bad = 0;
            char det[512]; det[0] = 0;
        ''')
        )
        # comparações
        for reg in ("eax", "ecx", "edx", "ebx", "ebp", "esi", "edi", "eflags"):
            parts.append(textwrap.dedent(f'''
            if (s.{reg} != {regs.get(reg + '_out', 0):#x}u) {{
                bad = 1;
                snprintf(det + strlen(det), sizeof(det) - strlen(det),
                         "{reg}:%08x!={regs.get(reg + '_out', 0):08x} ", s.{reg});
            }}
            '''))
        parts.append(textwrap.dedent('''
            /* memória */
            for (uint32_t j = 0; j < SCRATCH_SIZE; j++) {
                if (s.mem[j] != exp_mem[i][j]) {
                    bad = 1;
                    snprintf(det + strlen(det), sizeof(det) - strlen(det),
                             "mem[%u]:%02x!=%02x ", j, s.mem[j], exp_mem[i][j]);
                    break;
                }
            }
            if (bad) { fail++; printf("%08x,FALHA,\\"%s\\"\\n", 0x%xu, det); }
            else     { pass++; }
            x2rw_ctx_destroy(c);
        }
        ''' % (f['va'], f['va'])))
    parts.append(HARNESS_TAIL)
    return "".join(parts)


def main() -> int:
    ap = argparse.ArgumentParser(description="autoteste do lifter")
    ap.add_argument("--game", required=True)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--dll", action="append", default=[])
    ap.add_argument("--count", type=int, default=200)
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--out", default="build/difftest")
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = ap.parse_args()

    game: GameConfig = get_game(args.game)
    m = game.primary if hasattr(game, "primary") else game.modules[0]

    if unicorn is None:
        print("[difftest] unicorn AUSENTE. Este teste precisa do unicorn como")
        print("[difftest] oráculo x86 real. Instale com:")
        print("[difftest]   python3 -m pip install --break-system-packages unicorn")
        print("[difftest] Sem ele, NÃO há como afirmar que o lifter está correto.")
        print("[difftest] Resultado: NÃO MEDIDO (ver docs/TESTED.md).")
        return 2

    image = load_image(args.exe)
    funcs = json.load(open(f"gen/{m.name}_funcs.json"))["functions"]

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    rng = random.Random(args.seed)
    picked = pick_functions(funcs, image, md, args.count, rng)
    print(f"[difftest] {len(picked)} funções testáveis de "
          f"{len(funcs)} (sem call/privilégio/salto externo)")

    init_mem = bytes(((i * 7 + 3) & 0xFF) for i in range(SCRATCH_SIZE))
    cases = []
    t0 = time.time()
    for f in picked:
        regs = {
            "eax": rng.randrange(1 << 32), "ecx": rng.randrange(1 << 32),
            "edx": rng.randrange(1 << 32), "ebx": rng.randrange(1 << 32),
            "ebp": rng.randrange(1 << 32), "esi": SCRATCH,
            "edi": SCRATCH + 0x100,
            "eflags": rng.choice([0x202, 0x246, 0x292, 0x0]),
        }
        exp = run_unicorn(image, m.imagebase, f, regs, init_mem)
        if exp is None:
            continue
        for k in ("eax", "ecx", "edx", "ebx", "ebp", "esi", "edi", "eflags"):
            regs[k + "_out"] = exp[k]
        cases.append({"func": f, "regs": regs, "mem": bytes(exp["mem"])})
    print(f"[difftest] oráculo unicorn: {len(cases)} casos em "
          f"{time.time() - t0:.1f}s")

    if not cases:
        print("[difftest] nenhum caso utilizável")
        return 2

    os.makedirs(args.out, exist_ok=True)
    # Memória esperada como tabela C
    mem_tbl = ["static const uint8_t exp_mem[%d][%d] = {" % (len(cases), SCRATCH_SIZE)]
    for cs in cases:
        mem_tbl.append("{" + ",".join(str(b) for b in cs["mem"]) + "},")
    mem_tbl.append("};")
    src = gen_harness(cases).replace(
        "int main(void) {", "\n".join(mem_tbl) + "\nint main(void) {")

    cpath = os.path.join(args.out, "harness.c")
    open(cpath, "w", encoding="utf-8").write(src)
    print(f"[difftest] harness: {cpath}")

    gen_c = sorted(
        os.path.join("gen", f) for f in os.listdir("gen") if f.endswith(".c"))
    exe = os.path.join(args.out, "harness")
    cmd = [args.cc, "-O1", "-g", "-fwrapv", "-fno-strict-aliasing",
           "-I", "runtime", "-I", "gen", "-o", exe, cpath,
           "runtime/x2rw_runtime.c", "runtime/x2rw_simd.c",
           "runtime/override_list.c"] + gen_c + ["-lm"]
    # shims, se existirem
    shims = os.path.join("runtime", "shims")
    if os.path.isdir(shims):
        cmd += sorted(os.path.join(shims, f) for f in os.listdir(shims)
                      if f.endswith(".c"))
    print(f"[difftest] compilando ({len(gen_c)} arquivos de gen/)...")
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("[difftest] FALHA ao compilar o harness:")
        print(r.stderr[-4000:])
        return 1

    print("[difftest] executando...")
    r = subprocess.run([exe], capture_output=True, text=True)
    lines = r.stdout.strip().split("\n")
    summary = [l for l in lines if l.startswith("# resumo")]
    fails = [l for l in lines if ",FALHA," in l]
    print(r.stdout[:200] if not summary else "")
    for l in fails[:25]:
        print("  " + l)
    if summary:
        _, p, fa, sk = summary[0].split(",")
        total = int(p) + int(fa)
        taxa = 100.0 * int(p) / total if total else 0.0
        print(f"[difftest] PASSARAM {p} / {total}  ({taxa:.1f}%)")
        if fails:
            print(f"[difftest] {len(fails)} falhas — ver acima")
    log = os.path.join("analysis", "difftest.log")
    os.makedirs("analysis", exist_ok=True)
    open(log, "w", encoding="utf-8").write(r.stdout)
    print(f"[difftest] log completo em {log}")
    return 0 if not fails else 1


if __name__ == "__main__":
    raise SystemExit(main())
