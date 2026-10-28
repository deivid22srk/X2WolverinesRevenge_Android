#!/usr/bin/env python3
"""Lifter x86-32 -> C para os módulos do jogo.

Estratégia (translate com dispatch explícito):
  * Cada REGISTRADOR x86 vira estado no contexto `x2rw_ctx`
    (runtime/x2rw_runtime.h).
  * Cada FUNÇÃO descoberta por tools/discover.py vira uma função C
    `void x2rw_<mod>_<rva>(x2rw_ctx *c)`.
  * Fluxo intra-função -> rótulos `goto L_<va>`.
  * `call` -> `x2rw_call(c, va)` (resolve por VA na tabela de módulos).
  * `jmp` indireto / tabela de salto -> `switch` sobre TODOS os endereços de
    instrução da função (dispatch correto, sem adivinhar).
  * `ret` -> sincroniza e `return;`.
  * FLAGS -> helpers `x2rw_flags_*` do runtime (semântica x86 exata).
  * x87 -> helpers `x2rw_x87_*` sobre a pilha de 8 doubles do contexto.
  * Instrução NÃO suportada -> `x2rw_unsupported()`: LOGA e marca falha.
    NUNCA falha em silêncio.

O que sai daqui é C DERIVADO do binário do jogo -> vive em gen/, que é
publicado apenas no repositório PRIVADO (X2WR_gen). Nunca no público.

Uso:
    python3 tools/lifter.py --game x2wr --out gen \
        --exe "…/Wolvie.exe" --dll "…/binkw32.dll" --jobs 2
"""

from __future__ import annotations

import argparse
import json
import bisect
import os
import re
import sys
import time

try:
    import capstone
except ImportError:
    sys.exit("capstone não instalado: pip install capstone")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from games import GameConfig, Module, get_game  # noqa: E402

# ---------------------------------------------------------------------------
# Registradores
# ---------------------------------------------------------------------------
R32 = {
    "eax": "c->r.eax", "ecx": "c->r.ecx", "edx": "c->r.edx", "ebx": "c->r.ebx",
    "esp": "c->r.esp", "ebp": "c->r.ebp", "esi": "c->r.esi", "edi": "c->r.edi",
}
R8 = {
    "al": ("eax", 0), "cl": ("ecx", 0), "dl": ("edx", 0), "bl": ("ebx", 0),
    "ah": ("eax", 8), "ch": ("ecx", 8), "dh": ("edx", 8), "bh": ("ebx", 8),
}
R16 = {"ax": "eax", "cx": "ecx", "dx": "edx", "bx": "ebx",
       "sp": "esp", "bp": "ebp", "si": "esi", "di": "edi"}

CC = {
    "o": 0, "no": 1, "b": 2, "c": 2, "nae": 2, "ae": 3, "nb": 3, "nc": 3,
    "e": 4, "z": 4, "ne": 5, "nz": 5, "be": 6, "na": 6, "a": 7, "nbe": 7,
    "s": 8, "ns": 9, "p": 10, "pe": 10, "np": 11, "po": 11,
    "l": 12, "nge": 12, "ge": 13, "nl": 13, "le": 14, "ng": 14,
    "g": 15, "nle": 15,
}

# mnemônicos SIMD (nunca suportados aqui; logados explicitamente)
SIMD_RE = re.compile(
    r"^(p|v|_)?(add|sub|mul|div|and|or|xor|min|max|avg|cmp|com|ucom|mov|"
    r"pack|unpck|shuf|psll|psrl|psra|cvt|rcp|sqrt|rsqrt|maskmov|pmov)"
)


class LiftError(Exception):
    pass


# ---------------------------------------------------------------------------
# Operandos
# ---------------------------------------------------------------------------

def _reg_size(name: str) -> int:
    if name in R32:
        return 4
    if name in R16:
        return 2
    if name in R8:
        return 1
    if re.match(r"^mm\d$", name):
        return 8
    if re.match(r"^[xy]mm\d+$", name):
        return 16
    return 4


def _mem_expr(md, insn, op) -> str:
    m = op.mem
    parts: list[str] = []
    if m.base:
        b = insn.reg_name(m.base)
        parts.append(R32.get(b, "0u"))
    if m.index:
        i = insn.reg_name(m.index)
        ip = R32.get(i, "0u")
        parts.append(f"(({ip}) * {m.scale}u)" if m.scale != 1 else f"({ip})")
    if m.disp or not parts:
        parts.append(f"(uint32_t)({m.disp})" if m.disp < 0 else f"{m.disp}u")
    return "(" + " + ".join(parts) + ")"


def op_size(insn, op, default: int = 4) -> int:
    sz = getattr(op, "size", 0) or 0
    if sz:
        return sz
    if op.type == capstone.x86.X86_OP_REG:
        return _reg_size(insn.reg_name(op.reg))
    return default


def read_op(insn, op) -> tuple[str, int]:
    """(expressão C rvalue, tamanho em bytes)."""
    if op.type == capstone.x86.X86_OP_REG:
        name = insn.reg_name(op.reg)
        if name in R32:
            return R32[name], 4
        if name in R16:
            return f"(({R32[R16[name]]}) & 0xffffu)", 2
        if name in R8:
            base, sh = R8[name]
            e = R32[base]
            return (f"(({e}) & 0xffu)" if sh == 0
                    else f"((({e}) >> 8) & 0xffu)"), 1
        return f"/* reg {name} */ 0u", _reg_size(name)
    if op.type == capstone.x86.X86_OP_IMM:
        return f"{op.imm & 0xFFFFFFFF}u", 4
    if op.type == capstone.x86.X86_OP_MEM:
        addr = _mem_expr(None, insn, op)
        size = op_size(insn, op)
        fn = {1: "read8", 2: "read16", 8: "read64"}.get(size, "read32")
        return f"x2rw_{fn}(c, {addr})", size
    return "0u", 4


def write_stmt(insn, op, value: str) -> str:
    size = op_size(insn, op)
    if op.type == capstone.x86.X86_OP_REG:
        name = insn.reg_name(op.reg)
        if name in R32:
            return f"{R32[name]} = (uint32_t)({value});"
        if name in R16:
            e = R32[R16[name]]
            return (f"{e} = ({e} & 0xffff0000u) | "
                    f"((uint32_t)({value}) & 0xffffu);")
        if name in R8:
            base, sh = R8[name]
            e = R32[base]
            if sh == 0:
                return (f"{e} = ({e} & 0xffffff00u) | "
                        f"((uint32_t)({value}) & 0xffu);")
            return (f"{e} = ({e} & 0xffff00ffu) | "
                    f"(((uint32_t)({value}) & 0xffu) << 8);")
        if name in ("ds", "es", "fs", "gs", "ss", "cs"):
            # Modelo com memória plana: segmento não tem efeito. Registrado
            # como no-op para não poluir o log.
            return "(void)0; /* escrita ignorada em registrador de segmento */"
        raise LiftError(f"escrita em registrador não suportado: {name}")
    if op.type == capstone.x86.X86_OP_MEM:
        addr = _mem_expr(None, insn, op)
        fn = {1: "write8", 2: "write16", 8: "write64"}.get(size, "write32")
        cast = {1: "uint8_t", 2: "uint16_t", 8: "uint64_t"}.get(size, "uint32_t")
        return f"x2rw_{fn}(c, {addr}, ({cast})({value}));"
    raise LiftError("operando de destino inválido")


# ---------------------------------------------------------------------------
# Emissão por instrução
# ---------------------------------------------------------------------------
ARITH = {"add": "+", "sub": "-", "and": "&", "or": "|", "xor": "^",
         "adc": "+", "sbb": "-"}



def _is_branch(mn: str) -> bool:
    return (mn == "jmp" or mn in LOOP_MN
            or (mn.startswith("j") and mn[1:] in CC))


LOOP_MN = ("loop", "loope", "loopne", "loopz", "loopnz", "jecxz", "jrcxz")

# Instruções que encerram um caminho de execução. Se a faixa de uma função
# termina SEM uma destas, o código continua — ver _extent().
TERMINATORS = ("ret", "retf", "retn", "iret", "iretd", "iretf", "jmp", "ljmp",
               "lret", "hlt", "ud2", "int3")


def _branch_targets(md, code: bytes, base: int) -> set[int]:
    """Alvos imediatos de salto no bloco [base, base+len(code))."""
    out: set[int] = set()
    for insn in md.disasm(code, base):
        if not _is_branch(insn.mnemonic):
            continue
        if insn.operands and insn.operands[0].type == capstone.x86.X86_OP_IMM:
            out.add(insn.operands[0].imm & 0xffffffff)
    return out


def _extent(md, image: bytes, rva: int, va: int, size: int,
            max_ext: int = 0x2000, hard_end: int = 0) -> tuple[int, int]:
    """Alcance real do código da função, em endereços de GUEST.

    Duas correções sobre a estimativa de tools/discover.py:

    1) TERMINADOR. A descoberta marca início de função por padrão de prólogo,
       e isso dá FALSO POSITIVO dentro de funções reais. O tamanho de uma
       função é a distância até o próximo "início" descoberto, então um falso
       prólogo parte a função no meio. Aqui, se a faixa termina sem
       `ret`/`jmp`/`int3`, continuamos desmontando até o primeiro terminador.
       Sem isto o entrypoint do jogo (VA 0x005c3042) ficava com 29 bytes — o
       prólogo SEH — e retornava antes de fazer qualquer coisa.

    2) ALVOS DE SALTO PARA FRENTE. Saltos internos podem apontar para fora da
       faixa estimada; nesse caso o lifter os trataria erradamente como salto
       de cauda (chamada de outra função). Estendemos até o maior alvo
       adiante, iterando até estabilizar.

    ATENÇÃO AO INDEXAR `image`: `image` é a imagem mapeada em memória, e é
    indexada por RVA (índice 0 == ImageBase). Passar um VA absoluto aqui
    lê a região ERRADA do arquivo — foi exatamente o bug que produziu C
    desmontado a partir de lixo durante o desenvolvimento. Por isso `rva`
    é parâmetro obrigatório desta função.

    A desmontagem sempre COMEÇA em `va`, então o alinhamento é o correto
    do código real; só o fim da faixa se move.
    """
    lo = va
    hi = va + max(size, 1)
    # Teto da extensão. `max_ext` é DELIBERADAMENTE pequeno (8 KiB): seguir
    # saltos para frente sem limite encadeia de um `jmp`-de-cauda para outro e
    # acaba absorvendo funções inteiras — medido: o entrypoint ia de 201 bytes
    # para 40.855 e o total de instruções não suportadas quadruplicava
    # (2.823 -> 12.017) só com código inalcançável. 8 KiB recupera o corpo
    # cortado por um falso prólogo sem invadir a função seguinte.
    limit = min(va + max(size, 1) + max_ext, va + 0x100000)
    # `hard_end` = início da próxima função CONFIÁVEL (alvo de call/export/
    # thunk de IAT). A função atual não pode continuar depois disso: é código
    # de outra função. Delimita a extensão sem depender de heurística.
    if hard_end > va:
        limit = min(limit, hard_end)

    def _sweep(upto: int) -> tuple[int, str]:
        """Desmonta linearmente de `lo` até `upto`.

        Devolve (endereço final, mnemônico da última instrução decodificada).
        """
        cur = lo
        last = ""
        while cur < upto:
            off = rva + (cur - va)
            chunk = image[off:off + max(1, upto - cur)]
            if not chunk:
                break
            insns = list(md.disasm(chunk, cur))
            if not insns:
                break
            for ins in insns:
                cur = ins.address + ins.size
                last = ins.mnemonic.split()[0]
                if cur >= upto:
                    break
        return cur, last

    # --- 1) estende até um terminador ---------------------------------
    # Só paga o custo quando a faixa realmente termina no meio do corpo.
    for _ in range(64):
        end, last = _sweep(hi)
        if end < hi or last in TERMINATORS or hi >= limit:
            break
        hi = min(hi + 4096, limit)

    # --- 2) estende até os alvos de salto para frente -----------------
    for _ in range(6):
        if hi <= lo:
            break
        off = rva + (lo - va)
        code = image[off:off + (hi - lo)]
        tgts = _branch_targets(md, code, lo)
        # Só alvos para frente E perto (dentro da janela): um `jmp` para longe
        # é salto de cauda para outra função, não código desta.
        fwd = [t for t in tgts if hi <= t <= hi + max_ext]
        if not fwd:
            break
        new_hi = min(max(fwd) + 16, limit)
        if new_hi <= hi:
            break
        hi = new_hi
    return lo, hi


def _in_range(tgt: int, rng: dict) -> bool:
    """O alvo está dentro desta função E tem instrução desmontada lá?"""
    if tgt < rng["lo"] or tgt >= rng["hi"]:
        return False
    return tgt in rng["addrs"]


def _tmp(va: int, tag: str = "") -> str:
    return f"_r{va & 0xffff:x}{tag}"


def lift_insn(insn, out: list[str], stats: dict, rng: dict) -> None:
    """`rng` descreve o que é "dentro desta função":
        {'lo','hi','addrs'} — lo/hi = faixa de endereços; addrs = conjunto
        dos endereços que realmente têm instrução desmontada.

    Salto para FORA da faixa é um salto de cauda para outra função. Nesse
    caso emitimos `x2rw_call` + `return`: a pilha está exatamente como o
    destino espera (nada foi empilhado pelo salto), então a função destino
    executa e o `ret` dela devolve o controle ao NOSSO chamador. É
    equivalente a saltar, e evita duplicar/errar a fronteira das funções.
    """
    mn = insn.mnemonic
    ops = insn.operands
    va = insn.address
    nos = len(ops)
    stats["insns"] = stats.get("insns", 0) + 1

    def need(n: int) -> bool:
        if nos < n:
            out.append(f'x2rw_unsupported(c, {va:#x}u, "operandos {mn}");')
            stats["unsupported"] += 1
            return False
        return True

    # ------------------------------------------------------------ prefixos
    # capstone 5 devolve o prefixo colado ao mnemônico ("rep movsd",
    # "repz ret"). Separa os dois: "rep ret" vira "ret"; "rep movsd" vira
    # "movsd" com rep=0xF3.
    rep_prefix = 0
    parts = mn.split()
    if parts[0] in ("rep", "repe", "repz", "repne", "repnz", "lock"):
        if parts[0] in ("rep", "repe", "repz"):
            rep_prefix = 0xF3
        elif parts[0] in ("repne", "repnz"):
            rep_prefix = 0xF2
        # "lock" não muda a semântica aqui (port single-thread)
        if len(parts) == 1:
            return
        mn = parts[1]
        stats["prefixed"] = stats.get("prefixed", 0) + 1

    if mn in ("nop", "pause", "wait", "fwait", "fnop"):
        return
    if mn == "int3":
        out.append(f'x2rw_fault(c, X2RW_FAULT_ASSERT, {va:#x}u, "int3");')
        return
    if mn in ("hlt", "ud2", "int", "into", "iret", "iretd"):
        out.append(f'x2rw_unsupported(c, {va:#x}u, "{mn}");')
        stats["unsupported"] += 1
        return

    # ------------------------------------------------------------ mov
    if mn == "mov":
        if not need(2):
            return
        val, _ = read_op(insn, ops[1])
        out.append(write_stmt(insn, ops[0], val))
        return

    if mn in ("movzx", "movsx", "movsxd"):
        if not need(2):
            return
        val, vsz = read_op(insn, ops[1])
        if mn == "movzx":
            expr = f"({val})"
        elif vsz == 1:
            expr = f"(uint32_t)(int32_t)(int8_t)({val})"
        elif vsz == 2:
            expr = f"(uint32_t)(int32_t)(int16_t)({val})"
        else:
            expr = f"({val})"
        out.append(write_stmt(insn, ops[0], expr))
        return

    if mn == "lea":
        if not need(2) or ops[1].type != capstone.x86.X86_OP_MEM:
            return
        out.append(write_stmt(insn, ops[0], _mem_expr(None, insn, ops[1])))
        return

    # ------------------------------------------------------------ push/pop
    if mn == "push":
        if not need(1):
            return
        val, _ = read_op(insn, ops[0])
        out.append(f"x2rw_push(c, (uint32_t)({val}));")
        return
    if mn == "pop":
        if not need(1):
            return
        if ops[0].type == capstone.x86.X86_OP_REG and \
                insn.reg_name(ops[0].reg) == "esp":
            out.append("c->r.esp = x2rw_pop(c);")
            return
        out.append(write_stmt(insn, ops[0], "x2rw_pop(c)"))
        return
    if mn in ("pushad", "pushal"):
        for r in ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"):
            out.append(f"x2rw_push(c, {R32[r]});")
        return
    if mn in ("popad", "popal"):
        for r in reversed(("eax", "ecx", "edx", "ebx", "esp", "ebp",
                           "esi", "edi")):
            if r == "esp":
                out.append("(void)x2rw_pop(c); /* esp descartado no popad */")
            else:
                out.append(f"{R32[r]} = x2rw_pop(c);")
        return

    # ------------------------------------------------------------ aritmética
    if mn in ("add", "sub", "and", "or", "xor", "adc", "sbb"):
        if not need(2):
            return
        a, asz = read_op(insn, ops[0])
        b, _ = read_op(insn, ops[1])
        expr = f"({a}) {ARITH[mn]} ({b})"
        if asz == 1:
            expr = f"(uint8_t)({expr})"
        elif asz == 2:
            expr = f"(uint16_t)({expr})"
        t = _tmp(va)
        out.append(f"uint32_t {t} = (uint32_t)({expr});")
        if mn in ("and", "or", "xor"):
            out.append(f"x2rw_flags_logic(c, {t});")
        elif mn in ("add", "adc"):
            out.append(f"x2rw_flags_add(c, (uint32_t)({a}), (uint32_t)({b}), {t});")
        else:
            out.append(f"x2rw_flags_sub(c, (uint32_t)({a}), (uint32_t)({b}), {t});")
        out.append(write_stmt(insn, ops[0], t))
        return

    if mn == "cmp":
        if not need(2):
            return
        a, _ = read_op(insn, ops[0])
        b, _ = read_op(insn, ops[1])
        t = _tmp(va)
        out.append(f"uint32_t {t} = (uint32_t)(({a}) - ({b}));")
        out.append(f"x2rw_flags_sub(c, (uint32_t)({a}), (uint32_t)({b}), {t});")
        return

    if mn == "test":
        if not need(2):
            return
        a, _ = read_op(insn, ops[0])
        b, _ = read_op(insn, ops[1])
        t = _tmp(va)
        out.append(f"uint32_t {t} = (uint32_t)(({a}) & ({b}));")
        out.append(f"x2rw_flags_logic(c, {t});")
        return

    if mn == "inc" or mn == "dec":
        if not need(1):
            return
        a, asz = read_op(insn, ops[0])
        t = _tmp(va)
        op = "+" if mn == "inc" else "-"
        expr = f"({a}) {op} 1u"
        if asz == 1:
            expr = f"(uint8_t)({expr})"
        elif asz == 2:
            expr = f"(uint16_t)({expr})"
        out.append(f"uint32_t {t} = (uint32_t)({expr});")
        out.append(f"x2rw_flags_{mn}(c, (uint32_t)({a}), {t});")
        out.append(write_stmt(insn, ops[0], t))
        return

    if mn == "neg":
        if not need(1):
            return
        a, _ = read_op(insn, ops[0])
        t = _tmp(va)
        out.append(f"uint32_t {t} = (uint32_t)(0u - ({a}));")
        out.append(f"x2rw_flags_sub(c, 0u, (uint32_t)({a}), {t});")
        out.append(write_stmt(insn, ops[0], t))
        return
    if mn == "not":
        if not need(1):
            return
        a, _ = read_op(insn, ops[0])
        out.append(write_stmt(insn, ops[0], f"~({a})"))
        return

    # ------------------------------------------------------------ shifts
    if mn in ("shl", "sal", "shr", "sar", "rol", "ror", "rcl", "rcr"):
        if not need(2):
            return
        a, asz = read_op(insn, ops[0])
        cnt, _ = read_op(insn, ops[1])
        bits = asz * 8
        mask = (1 << bits) - 1 if bits < 32 else 0xFFFFFFFF
        cntv = f"(({cnt}) & 0x1fu)"
        t = _tmp(va)
        if mn in ("shl", "sal"):
            out.append(f"uint32_t {t} = (uint32_t)((({a}) << {cntv}) & {mask}u);")
            out.append(f"x2rw_flags_logic(c, {t});")
            # `32 - cnt` chega a 32 quando cnt==0 -> deslocamento >= largura,
            # que é indefinido em C (-Wshift-count-overflow, medido no CI em
            # gen/Wolvie_part007.c:28543). Mascarar com 0x1f resolve: para
            # cnt==0 vira `>> 0`.
            out.append(f"if ({cntv}) {{ if (((({a}) >> "
                       f"((32u - {cntv}) & 0x1fu)) & 1u)) "
                       f"c->eflags |= X2RW_CF; }}")
        elif mn == "shr":
            out.append(f"uint32_t {t} = (uint32_t)((({a}) & {mask}u) >> {cntv});")
            out.append(f"x2rw_flags_logic(c, {t});")
        elif mn == "sar":
            out.append(f"uint32_t {t} = (uint32_t)(((int32_t)({a})) >> {cntv});")
            out.append(f"x2rw_flags_logic(c, {t});")
        # Rotação: a segunda parcela usa `(32 - cnt) & 0x1f`. Sem a máscara,
        # cnt==0 gera `>> 32` / `<< 32` (indefinido) — era o aviso real
        # -Wshift-count-overflow do CI. Com a máscara, cnt==0 dá `x | x` = x,
        # que é o resultado correto de uma rotação por 0.
        elif mn == "rol":
            out.append(f"uint32_t {t} = (uint32_t)(((({a}) << {cntv}) | "
                       f"((({a}) & {mask}u) >> ((32u - {cntv}) & 0x1fu))) "
                       f"& {mask}u);")
        elif mn == "ror":
            out.append(f"uint32_t {t} = (uint32_t)(((({a}) & {mask}u) >> {cntv}) | "
                       f"((({a}) << ((32u - {cntv}) & 0x1fu)) & {mask}u));")
        elif mn in ("rcl", "rcr"):
            out.append(f'x2rw_unsupported(c, {va:#x}u, "{mn}");')
            stats["unsupported"] += 1
            return
        else:
            out.append(f'x2rw_unsupported(c, {va:#x}u, "{mn}");')
            stats["unsupported"] += 1
            return
        out.append(write_stmt(insn, ops[0], t))
        return

    # ------------------------------------------------------------ multiplicação
    if mn == "imul":
        if nos == 1:
            a, _ = read_op(insn, ops[0])
            out.append("{ int64_t _m = (int64_t)(int32_t)c->r.eax * "
                       f"(int64_t)(int32_t)({a});")
            out.append("  c->r.eax = (uint32_t)_m;")
            out.append("  c->r.edx = (uint32_t)(_m >> 32);")
            out.append("  c->eflags &= ~(X2RW_CF|X2RW_OF);")
            out.append("  if (_m != (int64_t)(int32_t)c->r.eax) "
                       "c->eflags |= X2RW_CF|X2RW_OF; }")
            return
        if nos == 2:
            a, _ = read_op(insn, ops[0])
            b, _ = read_op(insn, ops[1])
            t = _tmp(va)
            out.append(f"uint32_t {t} = (uint32_t)((int32_t)({a}) * (int32_t)({b}));")
            out.append(write_stmt(insn, ops[0], t))
            return
        if nos == 3:
            a, _ = read_op(insn, ops[1])
            b, _ = read_op(insn, ops[2])
            t = _tmp(va)
            out.append(f"uint32_t {t} = (uint32_t)((int32_t)({a}) * (int32_t)({b}));")
            out.append(write_stmt(insn, ops[0], t))
            return
        out.append(f'x2rw_unsupported(c, {va:#x}u, "imul");')
        stats["unsupported"] += 1
        return

    if mn == "mul":
        if not need(1):
            return
        a, _ = read_op(insn, ops[0])
        out.append(f"{{ uint64_t _m = (uint64_t)c->r.eax * (uint64_t)({a});")
        out.append("  c->r.eax = (uint32_t)_m; c->r.edx = (uint32_t)(_m >> 32);")
        out.append("  c->eflags &= ~(X2RW_CF|X2RW_OF);")
        out.append("  if (c->r.edx != 0) c->eflags |= X2RW_CF|X2RW_OF; }")
        return

    if mn in ("div", "idiv"):
        if not need(1):
            return
        a, _ = read_op(insn, ops[0])
        out.append("{ uint32_t _d = (uint32_t)(" + a + ");")
        out.append(f"  if (_d == 0) {{ x2rw_fault(c, X2RW_FAULT_ASSERT, {va:#x}u, "
                   '"divisão por zero"); return; }')
        if mn == "div":
            out.append("  uint64_t _n = ((uint64_t)c->r.edx << 32) | c->r.eax;")
            out.append("  uint64_t _q = _n / _d, _rem = _n % _d;")
            out.append(f"  if (_q > 0xffffffffull) {{ x2rw_fault(c, "
                       f"X2RW_FAULT_ASSERT, {va:#x}u, \"div overflow\"); return; }}")
        else:
            out.append("  int64_t _n = (int64_t)(((uint64_t)c->r.edx << 32) | c->r.eax);")
            out.append("  int64_t _d64 = (int64_t)(int32_t)_d;")
            out.append(f"  if (_d64 == 0) {{ x2rw_fault(c, X2RW_FAULT_ASSERT, "
                       f"{va:#x}u, \"idiv zero\"); return; }}")
            out.append("  int64_t _q = _n / _d64, _rem = _n % _d64;")
        out.append("  c->r.eax = (uint32_t)_q; c->r.edx = (uint32_t)_rem; }")
        return

    if mn in ("cdq",):
        out.append("c->r.edx = (c->r.eax & 0x80000000u) ? 0xffffffffu : 0u;")
        return
    if mn == "cwd":
        out.append("c->r.edx = (c->r.eax & 0x8000u) ? 0xffffu : 0u;")
        return
    if mn == "cbw":
        out.append("c->r.eax = ((uint32_t)(int32_t)(int8_t)(c->r.eax & 0xffu)) & 0xffffu;")
        return
    if mn == "cwde":
        out.append("c->r.eax = (uint32_t)(int32_t)(int16_t)(c->r.eax & 0xffffu);")
        return

    # ------------------------------------------------------------ controle
    if mn == "jmp":
        if not need(1):
            return
        op = ops[0]
        if op.type == capstone.x86.X86_OP_IMM:
            tgt = op.imm & 0xffffffff
            if _in_range(tgt, rng):
                out.append(f"goto L_{tgt:x};")
            else:
                stats["tail_jumps"] = stats.get("tail_jumps", 0) + 1
                out.append(f"/* salto de cauda para {tgt:#010x} */")
                out.append(f"x2rw_call(c, {tgt:#010x}u);")
                out.append("return;")
        else:
            val, _ = read_op(insn, op)
            out.append(f"c->jump_target = (uint32_t)({val}); goto L_dispatch;")
        return

    if mn.startswith("j") and mn[1:] in CC:
        if not need(1):
            return
        if ops[0].type != capstone.x86.X86_OP_IMM:
            out.append(f'x2rw_unsupported(c, {va:#x}u, "jcc indireto");')
            stats["unsupported"] += 1
            return
        tgt = ops[0].imm & 0xffffffff
        cc = CC[mn[1:]]
        if _in_range(tgt, rng):
            out.append(f"if (x2rw_cond(c, {cc})) goto L_{tgt:x};")
        else:
            stats["tail_jumps"] = stats.get("tail_jumps", 0) + 1
            out.append(f"/* salto de cauda condicional para {tgt:#010x} */")
            out.append(f"if (x2rw_cond(c, {cc})) {{ "
                       f"x2rw_call(c, {tgt:#010x}u); return; }}")
        return

    if mn in ("loop", "loope", "loopne", "loopz", "loopnz"):
        if not need(1) or ops[0].type != capstone.x86.X86_OP_IMM:
            return
        tgt = ops[0].imm & 0xffffffff
        dec = "c->r.ecx = (c->r.ecx - 1u) & 0xffffffffu;"
        if mn == "loop":
            cond = "c->r.ecx != 0u"
        elif mn in ("loope", "loopz"):
            cond = "(c->r.ecx != 0u) && (c->eflags & X2RW_ZF)"
        else:
            cond = "(c->r.ecx != 0u) && !(c->eflags & X2RW_ZF)"
        if _in_range(tgt, rng):
            out.append(f"{{ {dec} if ({cond}) goto L_{tgt:x}; }}")
        else:
            stats["tail_jumps"] = stats.get("tail_jumps", 0) + 1
            out.append(f"{{ {dec} if ({cond}) {{ x2rw_call(c, {tgt:#010x}u); "
                       "return; } }")
        return
    if mn == "jecxz":
        if not need(1) or ops[0].type != capstone.x86.X86_OP_IMM:
            return
        tgt = ops[0].imm & 0xffffffff
        if _in_range(tgt, rng):
            out.append(f"if (c->r.ecx == 0u) goto L_{tgt:x};")
        else:
            stats["tail_jumps"] = stats.get("tail_jumps", 0) + 1
            out.append(f"if (c->r.ecx == 0u) {{ x2rw_call(c, {tgt:#010x}u); "
                       "return; }")
        return

    if mn == "call":
        if not need(1):
            return
        op = ops[0]
        if op.type == capstone.x86.X86_OP_IMM:
            out.append(f"x2rw_call(c, {op.imm & 0xffffffff:#x}u);")
        else:
            val, _ = read_op(insn, op)
            out.append(f"x2rw_call(c, (uint32_t)({val}));")
        out.append("if (c->fault) return;")
        return

    if mn in ("ret", "retf"):
        if nos >= 1 and ops[0].type == capstone.x86.X86_OP_IMM:
            out.append(f"c->r.esp += {ops[0].imm & 0xffffffff}u;")
        out.append("return;")
        return
    if mn == "leave":
        out.append("c->r.esp = c->r.ebp;")
        out.append("c->r.ebp = x2rw_pop(c);")
        return
    if mn == "enter":
        if nos >= 2 and ops[0].type == capstone.x86.X86_OP_IMM:
            frame = ops[0].imm & 0xffff
            level = ops[1].imm & 0x1f
            out.append(f"x2rw_enter(c, {frame}u, {level}u);")
        else:
            out.append(f'x2rw_unsupported(c, {va:#x}u, "enter");')
            stats["unsupported"] += 1
        return

    # ------------------------------------------------------------ condicionais
    if mn.startswith("set") and mn[3:] in CC:
        if not need(1):
            return
        out.append(write_stmt(insn, ops[0], f"x2rw_setcc(c, {CC[mn[3:]]})"))
        return
    if mn.startswith("cmov") and mn[4:] in CC:
        if not need(2):
            return
        b, _ = read_op(insn, ops[1])
        out.append(f"if (x2rw_cond(c, {CC[mn[4:]]}))")
        out.append("    " + write_stmt(insn, ops[0], f"({b})"))
        return

    if mn == "xchg":
        if not need(2):
            return
        a, _ = read_op(insn, ops[0])
        b, _ = read_op(insn, ops[1])
        t = _tmp(va, "x")
        out.append(f"uint32_t {t} = (uint32_t)({a});")
        out.append(write_stmt(insn, ops[0], f"({b})"))
        out.append(write_stmt(insn, ops[1], t))
        return

    # ------------------------------------------------------------ flags
    if mn in ("pushfd", "pushf"):
        out.append("x2rw_push(c, c->eflags);")
        return
    if mn in ("popfd", "popf"):
        out.append("c->eflags = x2rw_pop(c);")
        return
    if mn == "lahf":
        out.append("c->r.eax = (c->r.eax & 0xffff00ffu) | ((c->eflags & 0xd5u) << 8);")
        return
    if mn == "sahf":
        out.append("c->eflags = (c->eflags & ~0xd5u) | ((c->r.eax >> 8) & 0xd5u);")
        return
    if mn in ("clc", "stc", "cmc", "cld", "std"):
        op = {"clc": "&= ~X2RW_CF", "stc": "|= X2RW_CF", "cmc": "^= X2RW_CF",
              "cld": "&= ~X2RW_DF", "std": "|= X2RW_DF"}[mn]
        out.append(f"c->eflags {op};")
        return
    if mn in ("sti", "cli"):
        out.append(f"/* {mn}: ignorado (port single-thread) */")
        return

    # ------------------------------------------------------------ strings
    if mn in ("movsb", "movsw", "movsd", "stosb", "stosw", "stosd",
              "lodsb", "lodsw", "lodsd", "scasb", "scasw", "scasd",
              "cmpsb", "cmpsw", "cmpsd"):
        rep = rep_prefix
        if rep == 0:
            # insn.prefix traz os bytes de prefixo (0xf3 = rep/repe,
            # 0xf2 = repne)
            if insn.prefix and 0xF3 in insn.prefix:
                rep = 0xF3
            elif insn.prefix and 0xF2 in insn.prefix:
                rep = 0xF2
        esize = {"b": 1, "w": 2, "d": 4}[mn[-1]]
        family = mn[:-1]
        out.append(f"x2rw_str_{family}(c, {esize}u, {rep:#x}u);")
        return

    # ------------------------------------------------------------ x87
    if mn.startswith("f") and not SIMD_RE.match(mn):
        emit_x87(insn, out, va, stats)
        return

    # ------------------------------------------------------------ SIMD/MMX
    if re.search(r"[xy]mm\d|mm\d", insn.op_str):
        if emit_simd(insn, out, stats):
            return
        out.append(f'x2rw_unsupported(c, {va:#x}u, "SIMD {mn} {insn.op_str}");')
        stats["unsupported"] += 1
        return
    if SIMD_RE.match(mn):
        out.append(f'x2rw_unsupported(c, {va:#x}u, "SIMD {mn} {insn.op_str}");')
        stats["unsupported"] += 1
        return

    # ------------------------------------------------------------ diversos
    if mn == "xlatb":
        out.append("c->r.eax = (c->r.eax & 0xffffff00u) | "
                   "x2rw_read8(c, (c->r.ebx + (c->r.eax & 0xffu)) & 0xffffffffu);")
        return

    if mn == "emms":
        out.append("x2rw_simd_emms(c);")
        return

    if mn == "bswap":
        if not need(1):
            return
        e = R32.get(insn.reg_name(ops[0].reg))
        if e is None:
            raise LiftError("bswap não-reg")
        out.append(f"{e} = __builtin_bswap32({e});")
        return
    if mn == "cpuid":
        out.append("x2rw_cpuid(c);")
        return
    if mn in ("rdtsc", "rdtscp"):
        out.append("x2rw_rdtsc(c);")
        return
    if mn in ("in", "out", "insb", "outsb", "insd", "outsd", "insw", "outsw",
              "invd", "wbinvd", "invlpg", "lgdt", "lidt", "ltr", "lldt",
              "lmsw", "smsw", "clts", "wrmsr", "rdmsr", "sysenter", "sysexit"):
        out.append(f'x2rw_unsupported(c, {va:#x}u, "privilegiado {mn}");')
        stats["unsupported"] += 1
        return

    out.append(f'x2rw_unsupported(c, {va:#x}u, "{mn} {insn.op_str}");')
    stats["unsupported"] += 1


# ---------------------------------------------------------------------------
# Emissão SIMD (MMX/SSE/3DNow!)
# ---------------------------------------------------------------------------

def _simd_reg_index(insn, op) -> int | None:
    if op.type != capstone.x86.X86_OP_REG:
        return None
    name = insn.reg_name(op.reg)
    m = re.match(r"^(?:mm|xmm)(\d)$", name)
    if m:
        return int(m.group(1))
    return None


def _simd_op(insn, op) -> str:
    """Expressão C de um operando SIMD (x2rw_sop)."""
    if op.type == capstone.x86.X86_OP_MEM:
        return f"x2rw_sop_mem({_mem_expr(None, insn, op)})"
    idx = _simd_reg_index(insn, op)
    if idx is None:
        raise LiftError("operando SIMD nao-registrador")
    return f"x2rw_sop_reg({idx}u)"


# nome do helper -> (nº de operandos, tem imediato)
SIMD_MAP = {
    "movq": "movq", "movd": "movd", "movaps": "movaps", "movups": "movups",
    "movss": "movss", "movlps": "movlps", "movhps": "movhps",
    "movlpd": "movlpd", "movhpd": "movhpd",
    "pxor": "pxor", "pand": "pand", "pandn": "pandn", "por": "por",
    "paddb": "paddb", "paddw": "paddw", "paddd": "paddd",
    "psubb": "psubb", "psubw": "psubw", "psubd": "psubd",
    "pmullw": "pmullw", "pmulhw": "pmulhw", "pmuludq": "pmuludq",
    "pmaddwd": "pmaddwd", "pcmpeqb": "pcmpeqb", "pcmpeqw": "pcmpeqw",
    "pcmpeqd": "pcmpeqd", "pcmpgtw": "pcmpgtw", "pcmpgtd": "pcmpgtd",
    "packsswb": "packsswb", "packssdw": "packssdw", "packuswb": "packuswb",
    "punpcklbw": "punpcklbw", "punpcklwd": "punpcklwd",
    "punpckldq": "punpckldq", "punpckhbw": "punpckhbw",
    "punpckhwd": "punpckhwd", "punpckhdq": "punpckhdq",
    "psllw": "psllw", "pslld": "pslld", "psllq": "psllq",
    "psrlw": "psrlw", "psrld": "psrld", "psrlq": "psrlq",
    "psraw": "psraw", "psrad": "psrad",
    "addps": "addps", "subps": "subps", "mulps": "mulps", "divps": "divps",
    "minps": "minps", "maxps": "maxps", "sqrtps": "sqrtps",
    "rcpps": "rcpps", "rsqrtps": "rsqrtps",
    "addss": "addss", "subss": "subss", "mulss": "mulss", "divss": "divss",
    "minss": "minss", "maxss": "maxss", "sqrtss": "sqrtss",
    "addsd": "addsd", "subsd": "subsd", "mulsd": "mulsd", "divsd": "divsd",
    "andps": "andps", "andnps": "andnps", "orps": "orps", "xorps": "xorps",
    "unpcklps": "unpcklps", "unpckhps": "unpckhps",
    "comiss": "comiss", "ucomiss": "ucomiss",
    "comisd": "comisd", "ucomisd": "ucomisd",
    "cvtdq2ps": "cvtdq2ps", "cvtps2dq": "cvtps2dq", "cvttps2dq": "cvttps2dq",
    "pshufd": "pshufd", "pshufw": "pshufw",
    "pshuflw": "pshuflw", "pshufhw": "pshufhw",
    "shufps": "shufps",
    "movapd": "movapd", "movupd": "movupd",
    "rsqrtss": "rsqrtss", "rcpss": "rcpss",
    "psubsw": "psubsw", "psubusb": "psubusb",
    "cvtps2pd": "cvtps2pd", "cvtpd2ps": "cvtpd2ps",
    "unpcklpd": "unpcklpd", "unpckhpd": "unpckhpd",
    "movdqa": "movaps", "movdqu": "movups", "movntps": "movaps",
    "movntdq": "movaps", "movntq": "movq",
    "sqrtpd": "sqrtps", "addpd": "addps", "subpd": "subps",
    "mulpd": "mulps", "divpd": "divps", "andpd": "andps",
    "orpd": "orps", "xorpd": "xorps",
}

# 3DNow!
NOW3D_MAP = {
    "pfadd": "pfadd", "pfsub": "pfsub", "pfsubr": "pfsubr", "pfmul": "pfmul",
    "pfacc": "pfacc", "pfnacc": "pfnacc", "pfpnacc": "pfpnacc",
    "pfrcp": "pfrcp", "pfrsqrt": "pfrsqrt", "pfrcpit1": "pfrcpit1",
    "pfrcpit2": "pfrcpit2", "pfrsqit1": "pfrsqit1",
    "pf2id": "pf2id", "pf2iw": "pf2iw", "pi2fd": "pi2fd", "pi2fw": "pi2fw",
    "pavgusb": "pavgusb", "pmulhrw": "pmulhrw", "pswapd": "pswapd",
    "pfcmpge": "pfcmpge", "pfcmpgt": "pfcmpgt", "pfcmpeq": "pfcmpeq",
}


def emit_simd(insn, out: list[str], stats: dict) -> bool:
    """Emite a chamada SIMD correspondente. False = não suportado."""
    mn = insn.mnemonic
    ops = insn.operands
    va = insn.address

    if mn == "emms":
        out.append("x2rw_simd_emms(c);")
        return True

    # --- movd/movq têm variantes reg-reg, reg-mem, mem-reg, xmm<->r32 ---
    if mn in ("movd", "movq"):
        if len(ops) < 2:
            return False
        dst = ops[0]
        src = ops[1]
        dst_is_mem = dst.type == capstone.x86.X86_OP_MEM
        if dst_is_mem:
            # mem <- mm/xmm  (movd: 32 bits; movq: 64 bits)
            idx = _simd_reg_index(insn, src)
            if idx is None:
                return False
            addr = _mem_expr(None, insn, dst)
            sname = insn.reg_name(src.reg)
            if mn == "movq":
                out.append(f"x2rw_write64(c, {addr}, c->mm[{idx}u]);")
            elif sname.startswith("xmm"):
                out.append(f"{{ uint32_t _v; memcpy(&_v, c->xmm[{idx}u], 4); "
                           f"x2rw_write32(c, {addr}, _v); }}")
            else:
                out.append(f"x2rw_write32(c, {addr}, (uint32_t)c->mm[{idx}u]);")
            return True
        idx = _simd_reg_index(insn, dst)
        if idx is None:
            return False
        is_xmm = insn.reg_name(dst.reg).startswith("xmm")
        # fonte = registrador GPR (movd mm0, eax / movd eax, mm0)
        if src.type == capstone.x86.X86_OP_REG and _simd_reg_index(insn, src) is None:
            gpr = insn.reg_name(src.reg)
            if gpr not in R32:
                return False
            if is_xmm:
                out.append(f"{{ uint32_t _v = {R32[gpr]}; "
                           f"memcpy(c->xmm[{idx}u], &_v, 4); }}")
            else:
                out.append(f"c->mm[{idx}u] = (uint64_t)({R32[gpr]});")
            return True
        s = _simd_op(insn, src)
        if mn == "movd":
            fn = "movd_xmm" if is_xmm else "movd_mm"
            out.append(f"x2rw_simd_{fn}(c, {idx}u, {s});")
        else:
            out.append(f"x2rw_simd_movq(c, {idx}u, {s});")
        return True

    # --- conversões que escrevem em GPR ---
    if mn in ("cvttss2si", "cvtss2si", "cvttsd2si", "cvtsd2si"):
        if len(ops) < 2:
            return False
        gpr = insn.reg_name(ops[0].reg) if ops[0].type == capstone.x86.X86_OP_REG else None
        if gpr not in R32:
            return False
        s = _simd_op(insn, ops[1])
        fn = {"cvttss2si": "cvttss2si", "cvtss2si": "cvtss2si",
              "cvttsd2si": "cvttsd2si", "cvtsd2si": "cvttsd2si"}[mn]
        out.append(f"x2rw_simd_{fn}(c, &{R32[gpr]}, {s});")
        return True
    if mn in ("cvtsi2ss", "cvtsi2sd"):
        if len(ops) < 2:
            return False
        idx = _simd_reg_index(insn, ops[0])
        src = ops[1]
        if idx is None:
            return False
        if src.type == capstone.x86.X86_OP_REG:
            gpr = insn.reg_name(src.reg)
            if gpr not in R32:
                return False
            val = R32[gpr]
        elif src.type == capstone.x86.X86_OP_MEM:
            val = f"x2rw_read32(c, {_mem_expr(None, insn, src)})"
        else:
            return False
        fn = "cvtsi2ss" if mn == "cvtsi2ss" else "cvtsi2sd"
        out.append(f"x2rw_simd_{fn}(c, {idx}u, (uint32_t)({val}));")
        return True
    if mn in ("cvtsd2ss", "cvtss2sd"):
        idx = _simd_reg_index(insn, ops[0])
        if idx is None:
            return False
        s = _simd_op(insn, ops[1])
        out.append(f"x2rw_simd_{mn}(c, {idx}u, {s});")
        return True

    # --- movhlps/movlhps (reg, reg) ---
    if mn in ("movhlps", "movlhps"):
        d = _simd_reg_index(insn, ops[0])
        s = _simd_reg_index(insn, ops[1])
        if d is None or s is None:
            return False
        out.append(f"x2rw_simd_{mn}(c, {d}u, {s}u);")
        return True


    # --- comparações com predicado imediato ---
    if mn in ("cmpps", "cmpss", "cmppd", "cmpsd", "cmpnleps", "cmpleps",
              "cmpltps", "cmpneqps", "cmpeqps", "cmpunordps"):
        alias = {"cmpnleps": (4, "ps"), "cmpleps": (2, "ps"),
                 "cmpltps": (1, "ps"), "cmpneqps": (4, "ps"),
                 "cmpeqps": (0, "ps"), "cmpunordps": (3, "ps")}
        if mn in alias:
            pred, _sfx = alias[mn]
            kind = "cmpps"
        else:
            pred = ops[2].imm & 7 if len(ops) > 2 else 0
            kind = mn
        idx = _simd_reg_index(insn, ops[0])
        if idx is None:
            return False
        out.append(f"x2rw_simd_{kind}(c, {idx}u, {_simd_op(insn, ops[1])}, "
                   f"{pred}u);")
        return True

    # --- pextrw / pinsrw / movmskps / pmovmskb (destino GPR) ---
    if mn in ("pextrw", "movmskps", "pmovmskb"):
        gpr = insn.reg_name(ops[0].reg) if ops[0].type == capstone.x86.X86_OP_REG else None
        idx = _simd_reg_index(insn, ops[1])
        if gpr not in R32 or idx is None:
            return False
        if mn == "pextrw":
            imm = ops[2].imm & 3 if len(ops) > 2 else 0
            out.append(f"x2rw_simd_pextrw(c, &{R32[gpr]}, {idx}u, {imm}u);")
        else:
            out.append(f"x2rw_simd_{mn}(c, &{R32[gpr]}, {idx}u);")
        return True
    if mn == "pinsrw":
        idx = _simd_reg_index(insn, ops[0])
        if idx is None:
            return False
        if ops[1].type == capstone.x86.X86_OP_REG:
            g = insn.reg_name(ops[1].reg)
            if g not in R32:
                return False
            val = R32[g]
        elif ops[1].type == capstone.x86.X86_OP_MEM:
            val = f"x2rw_read32(c, {_mem_expr(None, insn, ops[1])})"
        else:
            return False
        imm = ops[2].imm & 3 if len(ops) > 2 else 0
        out.append(f"x2rw_simd_pinsrw(c, {idx}u, (uint32_t)({val}), {imm}u);")
        return True

    # --- armazenamento SIMD em memória: movaps [mem], xmm ---
    if ops and ops[0].type == capstone.x86.X86_OP_MEM and len(ops) >= 2 and mn in (
            "movaps", "movups", "movdqa", "movdqu", "movss", "movlps",
            "movhps", "movlpd", "movhpd", "movq", "movd"):
        idx = _simd_reg_index(insn, ops[1])
        if idx is None:
            return False
        addr = _mem_expr(None, insn, ops[0])
        is_xmm = insn.reg_name(ops[1].reg).startswith("xmm")
        if mn in ("movaps", "movups", "movdqa", "movdqu"):
            out.append(f"x2rw_write128(c, {addr}, c->xmm[{idx}u]);")
        elif mn == "movss":
            out.append(f"{{ uint32_t _v; memcpy(&_v, c->xmm[{idx}u], 4); "
                       f"x2rw_write32(c, {addr}, _v); }}")
        elif mn in ("movlps", "movlpd"):
            out.append(f"{{ uint64_t _v; memcpy(&_v, c->xmm[{idx}u], 8); "
                       f"x2rw_write64(c, {addr}, _v); }}")
        elif mn in ("movhps", "movhpd"):
            out.append(f"{{ uint64_t _v; memcpy(&_v, c->xmm[{idx}u] + 8, 8); "
                       f"x2rw_write64(c, {addr}, _v); }}")
        elif mn == "movq":
            if is_xmm:
                out.append(f"{{ uint64_t _v; memcpy(&_v, c->xmm[{idx}u], 8); "
                           f"x2rw_write64(c, {addr}, _v); }}")
            else:
                out.append(f"x2rw_write64(c, {addr}, c->mm[{idx}u]);")
        else:  # movd
            if is_xmm:
                out.append(f"{{ uint32_t _v; memcpy(&_v, c->xmm[{idx}u], 4); "
                           f"x2rw_write32(c, {addr}, _v); }}")
            else:
                out.append(f"x2rw_write32(c, {addr}, (uint32_t)c->mm[{idx}u]);")
        return True

    # --- movd GPR <- mm/xmm ---
    if mn == "movd" and ops[0].type == capstone.x86.X86_OP_REG and \
            _simd_reg_index(insn, ops[0]) is None:
        gpr = insn.reg_name(ops[0].reg)
        idx = _simd_reg_index(insn, ops[1])
        if gpr in R32 and idx is not None:
            if insn.reg_name(ops[1].reg).startswith("xmm"):
                out.append(f"{{ uint32_t _v; memcpy(&_v, c->xmm[{idx}u], 4); "
                           f"{R32[gpr]} = _v; }}")
            else:
                out.append(f"{R32[gpr]} = (uint32_t)c->mm[{idx}u];")
            return True

    # --- genéricos: dst reg, src r/m, opcional imediato ---
    table = SIMD_MAP if mn in SIMD_MAP else (NOW3D_MAP if mn in NOW3D_MAP else None)
    if table is None:
        return False
    if len(ops) < 2:
        return False
    idx = _simd_reg_index(insn, ops[0])
    if idx is None:
        return False
    s = _simd_op(insn, ops[1])
    fn = table[mn]
    prefix = "x2rw_3d_" if mn in NOW3D_MAP else "x2rw_simd_"
    # shifts com imediato: o lifter passa o valor como "memória" sintética
    if mn in ("psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq",
              "psraw", "psrad") and ops[1].type == capstone.x86.X86_OP_IMM:
        s = f"x2rw_sop_reg(0u) /* n={ops[1].imm & 63} */"
        out.append(f"{{ x2rw_sop _s = x2rw_sop_reg(0u); c->mm[7] = "
                   f"{ops[1].imm & 63}u; x2rw_simd_{fn}(c, {idx}u, _s); }}")
        return True
    if mn in ("pshufd", "pshufw", "pshuflw", "pshufhw", "shufps"):
        imm = ops[2].imm & 0xff if len(ops) > 2 else 0
        out.append(f"{prefix}{fn}(c, {idx}u, {s}, {imm}u);")
        return True
    out.append(f"{prefix}{fn}(c, {idx}u, {s});")
    return True


def emit_x87(insn, out: list[str], va: int, stats: dict) -> None:
    """x87 sobre a pilha de 8 doubles do contexto (x2rw_x87_*)."""
    mn = insn.mnemonic
    o = insn.op_str
    ops = insn.operands
    mem = ops and ops[0].type == capstone.x86.X86_OP_MEM
    addr = _mem_expr(None, insn, ops[0]) if mem else "0u"
    msz = op_size(insn, ops[0], 4) if mem else 4

    if mn == "fld":
        if mem:
            out.append(f"x2rw_x87_fld_mem(c, {addr}, {msz}u);")
        else:
            m = re.search(r"st\((\d)\)", o)
            out.append(f"x2rw_x87_push(c, {int(m.group(1)) if m else 0}u);")
        return
    if mn in ("fst", "fstp"):
        pop = 1 if mn == "fstp" else 0
        if mem:
            out.append(f"x2rw_x87_fst_mem(c, {addr}, {msz}u, {pop}u);")
        else:
            m = re.search(r"st\((\d)\)", o)
            out.append(f"x2rw_x87_fst(c, {int(m.group(1)) if m else 0}u, {pop}u);")
        return
    if mn in ("fild", "fistp", "fist", "fisttp"):
        if not mem:
            out.append(f'x2rw_unsupported(c, {va:#x}u, "{mn} não-mem");')
            stats["unsupported"] += 1
            return
        pop = 1 if mn in ("fistp", "fisttp") else 0
        kind = "fild" if mn == "fild" else "fist"
        out.append(f"x2rw_x87_{kind}_mem(c, {addr}, {msz}u, {pop}u);")
        return
    if mn in ("fadd", "faddp", "fsub", "fsubp", "fsubr", "fsubrp",
              "fmul", "fmulp", "fdiv", "fdivp", "fdivr", "fdivrp",
              "fiadd", "fisub", "fimul", "fidiv"):
        if mem:
            out.append(f'x2rw_x87_arith_mem(c, {addr}, {msz}u, "{mn}");')
        else:
            m = re.search(r"st\((\d)\)", o)
            out.append(f'x2rw_x87_arith(c, "{mn}", {int(m.group(1)) if m else 0}u);')
        return
    if mn in ("fcom", "fcomp", "fcompp", "fucom", "fucomp", "fucompp",
              "fcomi", "fcomip", "fucomi", "fucomip", "ftst", "fxam"):
        out.append(f'x2rw_x87_cmp(c, "{mn}");')
        return
    if mn in ("fldz", "fld1", "fldpi", "fldl2e", "fldl2t", "fldlg2", "fldln2"):
        out.append(f'x2rw_x87_const(c, "{mn}");')
        return
    if mn in ("fchs", "fabs", "fsqrt", "frndint", "fxtract", "f2xm1", "fyl2x",
              "fyl2xp1", "fptan", "fpatan", "fsin", "fcos", "fsincos",
              "fscale", "fprem", "fprem1", "fdecstp", "fincstp"):
        out.append(f'x2rw_x87_unary(c, "{mn}");')
        return
    if mn == "fxch":
        m = re.search(r"st\((\d)\)", o)
        out.append(f"x2rw_x87_fxch(c, {int(m.group(1)) if m else 1}u);")
        return
    if mn in ("ffree", "ffreep"):
        m = re.search(r"st\((\d)\)", o)
        out.append(f"x2rw_x87_free(c, {int(m.group(1)) if m else 0}u);")
        return
    if mn in ("fnstsw", "fstsw"):
        if "ax" in o:
            out.append("x2rw_x87_status(c); c->r.eax = (c->r.eax & 0xffff0000u) "
                       "| (uint32_t)(c->x87_status & 0xffffu);")
        elif mem:
            out.append(f"x2rw_x87_status(c); x2rw_write16(c, {addr}, "
                       "c->x87_status);")
        else:
            out.append(f'x2rw_unsupported(c, {va:#x}u, "{mn}");')
            stats["unsupported"] += 1
        return
    if mn in ("fnstcw", "fstcw"):
        if mem:
            out.append(f"x2rw_write16(c, {addr}, c->x87_cw);")
        else:
            out.append(f'x2rw_unsupported(c, {va:#x}u, "{mn}");')
            stats["unsupported"] += 1
        return
    if mn == "fldcw":
        if mem:
            out.append(f"c->x87_cw = x2rw_read16(c, {addr});")
        else:
            out.append(f'x2rw_unsupported(c, {va:#x}u, "fldcw");')
            stats["unsupported"] += 1
        return
    if mn in ("fninit", "finit", "fnclex", "fclex"):
        out.append("x2rw_x87_init(c);")
        return
    if mn in ("fxsave", "fxrstor", "fnsave", "frsave", "frstor"):
        out.append(f'x2rw_unsupported(c, {va:#x}u, "{mn}");')
        stats["unsupported"] += 1
        return
    out.append(f'x2rw_unsupported(c, {va:#x}u, "x87 {mn} {o}");')
    stats["unsupported"] += 1


# ---------------------------------------------------------------------------
# Lifting de função e módulo
# ---------------------------------------------------------------------------

# Inícios de função CONFIÁVEIS (alvo de call/jmp, export ou thunk de IAT),
# ordenados. Preenchido uma vez por módulo em lift_module(). Serve para o
# _extent não invadir a função seguinte.
_RELIABLE: list[int] = []


def _next_reliable_after(va: int) -> int:
    """Menor início confiável > va, ou 0 se não houver."""
    i = bisect.bisect_right(_RELIABLE, va)
    return _RELIABLE[i] if i < len(_RELIABLE) else 0


def lift_function(md, m: Module, func: dict, image: bytes, stats: dict):
    rva = func["rva"]
    va = func["va"]
    size = func["size"]
    name = f"x2rw_{m.name.lower()}_{rva:x}"

    # Alcance real (a estimativa de tamanho da descoberta subestima funções
    # longas — ver _extent). `lo` é sempre `va`; só `hi` cresce.
    lo, hi = _extent(md, image, rva, va, size, hard_end=_next_reliable_after(va))
    code = image[rva:rva + (hi - lo)]     # RVA, não VA (ver _extent)
    insns = list(md.disasm(code, lo))

    out: list[str] = []
    if not insns:
        body = f"void {name}(x2rw_ctx *c) {{\n    (void)c;\n    return;\n}}"
        return name, body

    # Coleta os alvos diretos (que viram rótulos) e detecta se a função tem
    # salto indireto (tabela de salto / ponteiro de função local).
    direct_targets: set[int] = set()
    all_addrs: list[int] = []
    has_indirect_jump = False
    # Mnemônicos de salto condicional que NÃO começam com "j" (loop/loope/
    # loopne/jecxz). Esquecer estes fazia o rótulo nunca ser emitido.
    LOOP_MN = ("loop", "loope", "loopne", "loopz", "loopnz", "jecxz", "jrcxz")
    for insn in insns:
        all_addrs.append(insn.address)
        is_branch = (insn.mnemonic == "jmp"
                     or insn.mnemonic in LOOP_MN
                     or (insn.mnemonic.startswith("j")
                         and insn.mnemonic[1:] in CC))
        if is_branch:
            if insn.operands and insn.operands[0].type == capstone.x86.X86_OP_IMM:
                direct_targets.add(insn.operands[0].imm & 0xffffffff)
            elif insn.mnemonic != "jecxz" and insn.mnemonic != "jrcxz":
                # jecxz com operando imediato é salto direto; indireto só
                # em jmp/jcc com registrador/memória.
                has_indirect_jump = True

    # Se há salto indireto, TODA instrução precisa de rótulo: o alvo do
    # `switch` é desconhecido em tempo de lifting. Sem salto indireto, só os
    # alvos diretos viram rótulo (evita milhares de rótulos inúteis).
    dispatchable = has_indirect_jump and len(all_addrs) <= 4096
    addr_set = set(all_addrs)
    rng = {"lo": lo, "hi": hi, "addrs": addr_set}
    # Só viram rótulo os alvos que caem DENTRO da faixa e têm instrução
    # desmontada ali. Alvos fora são tratados como salto de cauda.
    in_range_targets = {t for t in direct_targets if _in_range(t, rng)}
    if dispatchable:
        label_set = addr_set
    else:
        label_set = in_range_targets

    for insn in insns:
        if insn.address in label_set:
            out.append(f"L_{insn.address:x}:;")
        try:
            lift_insn(insn, out, stats, rng)
        except LiftError as e:
            out.append(f'x2rw_unsupported(c, {insn.address:#x}u, "{e}");')
            stats["unsupported"] += 1

    if not out or "return;" not in out[-1]:
        out.append("return;")
    if has_indirect_jump and out and "return;" not in out[-1]:
        out.append("return;")

    # Dispatch para saltos indiretos. Só é emitido quando a função
    # realmente tem um: assim o switch nunca contém `goto` para rótulo
    # inexistente (o que seria erro de compilação) e não há código morto
    # nas funções comuns.
    dispatch: list[str] = []
    if has_indirect_jump:
        dispatch.append("")
        dispatch.append("/* ---- dispatch de salto indireto (tabela de salto) ---- */")
        dispatch.append("L_dispatch:;")
        if dispatchable:
            dispatch.append("    switch (c->jump_target) {")
            for a in all_addrs:
                dispatch.append(f"    case {a:#x}u: goto L_{a:x};")
            dispatch.append("    default:")
            dispatch.append("        x2rw_unsupported(c, c->jump_target, "
                            '"salto indireto fora da função");')
            dispatch.append("        return;")
            dispatch.append("    }")
        else:
            dispatch.append(f"    /* função com {len(all_addrs)} instruções: "
                            "dispatch grande demais */")
            dispatch.append("    x2rw_unsupported(c, c->jump_target, "
                            '"função grande demais para dispatch");')
            dispatch.append("    return;")

    lines = [f"/* {func['name']}  VA={va:#010x} size={size} "
             f"alcance={hi - lo} "
             f"flags={func['flags']} */",
             f"void {name}(x2rw_ctx *c) {{"]
    lines.extend("    " + l for l in out)
    lines.extend("    " + l for l in dispatch)
    lines.append("}")
    return name, "\n".join(lines)


def lift_module(mod_path: str, m: Module, funcs_json: str, out_dir: str,
                jobs: int) -> dict:
    import pefile

    with open(funcs_json, encoding="utf-8") as fh:
        disc = json.load(fh)

    pe = pefile.PE(mod_path)
    image = pe.get_memory_mapped_image()
    pe.close()

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    md.syntax = capstone.CS_OPT_SYNTAX_INTEL

    stats = {"unsupported": 0}
    funcs = disc["functions"]

    # Inícios confiáveis deste módulo (para _extent não invadir a função
    # seguinte). Flags: 1 ENTRY, 2 IAT_THUNK, 4 PROLOGUE, 8 CALL_TARGET,
    # 16 EXPORT. Só as confiáveis entram.
    global _RELIABLE
    _RELIABLE = sorted(f["va"] for f in funcs
                       if f.get("flags", 0) & (1 | 2 | 8 | 16))

    # Lift de TODAS as funções primeiro. A partição precisa ser feita pelo
    # TAMANHO REAL do C gerado, não pelo tamanho estimado da função: a
    # extensão de alcance (ver _extent) faz uma função de 200 bytes virar
    # 50 KB de C, e particionar pelo tamanho nominal desbalanceava os
    # arquivos (6 arquivos de 14 MB em vez de 16 equilibrados), o que
    # estoura a memória do compilador no runner do CI.
    lifted: list[tuple[int, str, str]] = []       # (va, nomes, fonte)
    for f in funcs:
        fname, src = lift_function(md, m, f, image, stats)
        lifted.append((f["va"], fname, src))

    total_chars = sum(len(src) for _, _, src in lifted) or 1
    n_parts = max(1, jobs)
    # Alvo de bytes por arquivo, com teto duro para não gerar unidades de
    # tradução grandes demais para o compilador.
    target = max(total_chars // n_parts, 1)

    parts: list[list[tuple[int, str, str]]] = [[]]
    acc = 0
    for item in lifted:
        if acc + len(item[2]) > target and len(parts) < n_parts and parts[-1]:
            parts.append([])
            acc = 0
        parts[-1].append(item)
        acc += len(item[2])

    table_entries: list[tuple[int, str]] = []
    written: list[str] = []
    for i, part in enumerate(parts):
        if not part:
            continue
        body = [src for _, _, src in part]
        for va, fname, _ in part:
            table_entries.append((va, fname))
        cpath = os.path.join(out_dir, f"{m.name}_part{i:03d}.c")
        with open(cpath, "w", encoding="utf-8") as fh:
            fh.write(f"/* Gerado por tools/lifter.py — módulo {m.name} parte {i} */\n")
            fh.write("/* C DERIVADO DO BINÁRIO DO JOGO — NÃO REDISTRIBUIR. */\n")
            fh.write("/* Ver README.md: este arquivo vive apenas em gen/ privado. */\n")
            # O código emitido chama x2rw_simd_* / x2rw_sop_* (declarados em
            # x2rw_simd.h) e memcpy/memset (string.h). Sem estes dois includes
            # o clang do NDK trata a chamada como declaração implícita — erro
            # em C99+, não aviso. Descoberto compilando o gen com clang 18.
            fh.write('#include <string.h>\n')
            fh.write('#include "x2rw_runtime.h"\n')
            fh.write('#include "x2rw_simd.h"\n')
            fh.write(f'#include "{m.name}_imports.h"\n')
            fh.write(f'#include "{m.name}_funcs.h"\n\n')
            fh.write("\n\n".join(body))
            fh.write("\n")
        sz = os.path.getsize(cpath)
        if sz > 8 * 1024 * 1024:
            print(f"[lifter] AVISO: {os.path.basename(cpath)} tem "
                  f"{sz / 1048576:.1f} MiB — aumente --jobs para o "
                  f"compilador não estourar memória no CI")
        written.append(cpath)

    # Cabeçalho com as DECLARAÇÕES das funções lifted.
    #
    # POR QUE É NECESSÁRIO: a tabela (Wolvie_table.c) referencia as
    # funções por nome (x2rw_wolvie_1000), mas as DEFINIÇÕES moram nos
    # arquivos partNNN.c — em arquivos diferentes. Sem uma declaração
    # visível, o C99/C11 exige que a função seja declarada antes do uso:
    # sem isso o build falha com "implicit declaration" e, pior, o
    # compilador assume `int` como retorno, o que trunca o ponteiro em
    # 64 bits. Este header é incluído pela tabela e por cada part.
    hpath = os.path.join(out_dir, f"{m.name}_funcs.h")
    with open(hpath, "w", encoding="utf-8") as fh:
        fh.write(f"/* Declarações das funções lifted de {m.name}. */\n")
        fh.write("/* Gerado por tools/lifter.py — NÃO EDITAR À MÃO. */\n")
        fh.write("#ifndef X2RW_FUNCS_%s_H\n" % m.name.upper())
        fh.write("#define X2RW_FUNCS_%s_H\n\n" % m.name.upper())
        fh.write('#include "x2rw_runtime.h"\n\n')
        fh.write("#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n")
        for va, fname in sorted(table_entries):
            fh.write(f"void {fname}(x2rw_ctx *c);\n")
        fh.write("\n#ifdef __cplusplus\n}\n#endif\n\n")
        fh.write(f"#endif /* X2RW_FUNCS_%s_H */\n" % m.name.upper())
    written.append(hpath)

    # tabela do módulo (arquivo separado, para o linker)
    tpath = os.path.join(out_dir, f"{m.name}_table.c")
    with open(tpath, "w", encoding="utf-8") as fh:
        fh.write(f"/* Tabela de funções de {m.name}: VA -> ponteiro */\n")
        fh.write('#include "x2rw_runtime.h"\n')
        fh.write(f'#include "{m.name}_funcs.h"\n\n')
        fh.write(f"const x2rw_fn_desc x2rw_funcs_{m.name}[] = {{\n")
        for va, fname in sorted(table_entries):
            fh.write(f"    {{ {va:#010x}u, {fname} }},\n")
        fh.write("};\n")
        fh.write(f"const uint32_t x2rw_n_funcs_{m.name} = "
                 f"{len(table_entries)}u;\n")
        fh.write(f"const uint32_t x2rw_base_{m.name} = {m.imagebase:#010x}u;\n")
        fh.write(f"const uint32_t x2rw_size_{m.name} = "
                 f"{m.size_of_image:#010x}u;\n")
    written.append(tpath)

    return {"module": m.name, "files": written,
            "n_functions": len(table_entries),
            "instructions": stats.get("insns", 0),
            "unsupported": stats["unsupported"]}


def main() -> int:
    ap = argparse.ArgumentParser(description="lifter x86-32 -> C")
    ap.add_argument("--game", required=True)
    ap.add_argument("--out", default="gen")
    ap.add_argument("--exe", default=None)
    ap.add_argument("--dll", action="append", default=[])
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--only", default=None)
    args = ap.parse_args()

    game: GameConfig = get_game(args.game)
    os.makedirs(args.out, exist_ok=True)

    t0 = time.time()
    results = []
    for m in game.modules:
        if args.only and m.name != args.only:
            continue
        path = None
        if m.is_primary and args.exe:
            path = args.exe
        else:
            for d in args.dll:
                if os.path.basename(d).lower() == m.filename.lower():
                    path = d
        if path is None:
            print(f"[lifter] {m.name}: PULADO (informe --exe/--dll)")
            continue
        j = os.path.join(args.out, f"{m.name}_funcs.json")
        if not os.path.isfile(j):
            print(f"[lifter] {m.name}: {j} ausente — rode tools/discover.py antes")
            return 1
        print(f"[lifter] {m.name}: liftando {len(json.load(open(j))['functions'])} "
              f"funções (jobs={args.jobs})...")
        r = lift_module(path, m, j, args.out, args.jobs)
        results.append(r)
        print(f"[lifter] {m.name}: {r['n_functions']} funções, "
              f"{r['unsupported']} instruções não suportadas, "
              f"{len(r['files'])} arquivos")

    summary = {"game": game.id, "elapsed_seconds": round(time.time() - t0, 2),
               "modules": results}
    with open(os.path.join(args.out, "lift_summary.json"), "w",
              encoding="utf-8") as fh:
        json.dump(summary, fh, indent=1)
    total_unsup = sum(r["unsupported"] for r in results)
    total_insns = sum(r.get("instructions", 0) for r in results)
    print(f"[lifter] concluído em {summary['elapsed_seconds']}s; "
          f"instruções: {total_insns}, não-suportadas: {total_unsup} "
          f"({100.0*(total_insns-total_unsup)/max(total_insns,1):.2f}% cobertas)"
          f" -> {args.out}/lift_summary.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
