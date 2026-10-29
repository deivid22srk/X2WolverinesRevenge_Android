# X2: Wolverine's Revenge — Registro de Testes

> **Regra deste arquivo: só entra aqui o que foi realmente executado ou
> medido.** Nada é presumido como funcionando. Se algo não foi testado, está
> escrito que não foi. Quando um número aparece, ele foi medido nesta
> máquina, com o comando indicado.

Máquina de desenvolvimento: Linux **aarch64**, 8 núcleos, ~1 GB de RAM livre.
Existe um compilador C real disponível: `zig cc` 0.13.0 (clang 18.1.6),
usado com `-target aarch64-linux-musl` — os binários rodam nativos nesta
máquina.

**Correção de registro:** uma versão anterior deste arquivo dizia que nada
tinha sido compilado localmente. Isso deixou de ser verdade: runtime, shims e
todo o `gen/` são compilados aqui e executados no harness `desktop/`
(`x2wr_desktop`), que é o oráculo usado para depurar. O que **nunca** é
compilado aqui é o APK: APK só sai do `build.yml`, por regra do projeto.

Desde a rodada em que o `build.yml` ganhou o job `desktop`, a compilação
pesada (1,2 milhão de instruções liftadas) também pode rodar no runner e sair
como artefato; o runner **não executa** o jogo, porque os arquivos do jogo não
podem entrar no repositório.

---

## 1. Verificado por execução direta (Fase 1 — análise dos binários)

| Verificação | Ferramenta | Resultado |
|---|---|---|
| Plataforma do executável | `file`, `pefile` | PE32 i386, GUI, 5 seções ✅ |
| SHA-1 dos binários | `sha1sum` | Wolvie.exe `37f4327c4ea45d2037e9e177ecbac003469ff490`; binkw32.dll `dd8c012f7e2bb68ec7b47702fcf3ea42e805e1f7`; data.pkr `560f2d2a0d26ee34f8591c5a09585e0cf6865a27`; wavs.pkr `309e57952af136288a46fb9b6b6e0ae723efd868` ✅ |
| Linker / data | `pefile` | VC++ 6.0, TimeDateStamp 2003-04-05 ✅ |
| Cabeçalhos PE | `pefile` | ImageBase `0x400000`, SizeOfImage `0x4CAE8E`, EntryPoint `0x1C3042`, `DllCharacteristics=0` (sem SEH/NX/ASLR) ✅ |
| DRM / packer | busca de assinaturas nas seções | Nenhum encontrado ✅ |
| API gráfica | imports + `d3d.log` do rip | **Direct3D 8** (`Direct3DCreate8` de d3d8.dll) — **não é D3D9** ✅ |
| API de áudio | imports | DirectSound (`ordinal_11`) + winmm `waveOut*` + `mmio*` ✅ |
| Vídeo | `file` nos `.bik` | Bink Video rev.i; 31 cutscenes em `movies/`, 109 trilhas em `music/` ✅ |
| Rede | busca de imports Winsock | **Ausente** — jogo single-player ✅ |
| Controles | `Wolvie.INI` | Punch=75, Kick=72, Jump=57, Action=77, Claws=71, Crouch=82, Stealth=42, Senses=156, Camera=76 ✅ |
| Containers de asset | análise binária | PKR3: cabeçalho + BZip2 + índice de 62.128 bytes (`MODE`, tabelas `.all`/`.mdl`/`.pso`) ✅ |

### 1.1 Superfície de import verificada (medida)

Extraída de `gen/*_imports.h`, contagem por DLL:

| DLL | Símbolos únicos |
|---|---|
| KERNEL32.dll | 136 |
| USER32.dll | 39 |
| WINMM.dll | 16 |
| binkw32.dll | 15 |
| GDI32.dll | 13 |
| ADVAPI32.dll | 4 |
| DINPUT8.dll | 1 |
| DSOUND.dll | 1 (`ordinal_11`) |
| d3d8.dll | 1 (`Direct3DCreate8`) |
| **Total** | **226** |

---

## 2. Verificado por execução — ferramentas do port

Estes comandos foram executados nesta máquina e produziram resultado.

| Item | Comando | Resultado medido |
|---|---|---|
| Descoberta de funções | `python3 tools/discover.py --game x2wr --exe … --dll …` | Wolvie.exe: **6.525 funções**, 174 imports, 0,33 s. binkw32.dll: **173 funções**, 114 imports ✅ |
| Lifting x86→C | `python3 tools/lifter.py --game x2wr --out gen --jobs 16 …` | **6.698 funções** liftadas, **882.687 instruções** desmontadas, **2.823 não suportadas** (**99,68 % de cobertura**), **69,85 s**, **65,6 MiB** de C em **32 arquivos** (Wolvie: 16 × 3,0–4,85 MiB; binkw32: 16 × 3 KiB–1,27 MiB) ✅ |
| Integridade dos rótulos | script de conferência sobre `gen/` | **0** `goto` sem rótulo (eram 2.456 antes das correções) ✅ |
| Chamadas entre funções | `grep -c x2rw_call gen/*.c` | **46.803** chamadas emitidas ✅ |
| Saltos de cauda | script sobre `gen/` | **1.856** — saltos para fora do alcance estimado da função, emitidos como chamada + retorno ✅ |
| Balanceamento das partições | `ls -la gen/*_part*.c` | Wolvie 16 partes de 3,0–4,85 MiB; binkw32 16 partes de 3 KiB–1,27 MiB ✅ |
| Instruções desmontadas (medida independente) | script que chama `lifter._extent` + capstone sobre as 6.698 funções | Wolvie **817.782** + binkw32 **64.905** = **882.687** ✅ |
| Shims Win32 | contagem das listas X-macro | **226/226** símbolos com handler registrado: 102 REAL, 50 PARCIAL, 74 STUB ✅ |
| Gerador de stubs | `python3 tools/gen_stub.py --game x2wr --out /tmp/gs` | 3 arquivos, nenhum derivado do jogo ✅ |
| Aplicador de overrides | `python3 tools/apply_hooks.py --game x2wr --out gen --overrides runtime/override_list.c` | 0 overrides (lista vazia por decisão — nada validado ainda) ✅ |
| Validação do workflow | `python3 -c "import yaml; yaml.safe_load(…)"` | YAML válido; jobs `guard`, `apk`, `gen`; entradas `build_type`, `gen_tag`, `gen_jobs` ✅ |
| Sintaxe dos scripts | `python3 -m py_compile tools/*.py` | OK ✅ |

### 2.1 O que o lifting cobre — e o que NÃO cobre

As 2.823 instruções não suportadas (de 882.687 — **0,32 % do total**),
medidas extraindo o mnemônico das próprias chamadas `x2rw_unsupported()` que o
lifter emitiu no C gerado:

| Mnemônico / categoria | Ocorrências | Situação |
|---|---:|---|
| `das` | 814 | Aritmética BCD decimal — legado do CRT de 16 bits |
| `operando` (forma de operando não coberta) | 481 | Emitido quando o mnemônico existe mas a combinação de operandos não |
| `x87` (casos exóticos) | 446 | `fxsave`/`frstor` e derivados |
| `SIMD` (forma não coberta) | 323 | MMX/SSE/3DNow! com forma de operando ainda não implementada |
| `privilegiado` (`in`/`out`/`wrmsr`/`lgdt`…) | 211 | Nunca executadas no caminho normal |
| `les` | 179 | Carregamento de ponteiro distante (segmentação de 16 bits) |
| `int` | 69 | Interrupções de software |
| `rcr` | 48 | Rotação com carry (direita) |
| `shld` | 22 | Deslocamento duplo à esquerda |
| `salc` | 19 | Instrução não documentada |
| `fnsave` / `frstor` | 36 | Salvar/restaurar estado x87 |
| `int1` / `into` | 28 | Depuração / overflow |
| `hlt` | 15 | Parada de CPU |
| `daa` | 14 | Ajuste decimal BCD |
| `aas` / `aad` / `aaa` | 33 | Ajuste ASCII |
| `lcall` | 9 | Chamada distante |
| demais | 76 | `bound`, `iretd`, `rcl`, `jcc indireto`, `enter` |
| **total** | **2.823** | |

Todas as 2.823 **são registradas em log e abortam com
`X2RW_FAULT_UNSUPPORTED`** — nenhuma falha em silêncio. Nenhuma delas foi
observada em execução real (o jogo não roda ainda), então **não se sabe se
estão no caminho crítico**. Uma taxa de cobertura de 99,68 % das instruções
NÃO significa 99,68 % de chance de o jogo funcionar: o que importa é se as
0,32 % restantes estão no caminho de inicialização.

### 2.2 Correções de lifter feitas por medição (não por suposição)

Cada item abaixo foi encontrado porque o resultado estava errado:

1. **`goto` para rótulo inexistente** — o `switch` de dispatch de salto
   indireto emitia `case`/`goto` para endereços sem rótulo (2.456 funções).
   Erro de compilação garantido. Corrigido: só emite dispatch quando a
   função realmente tem salto indireto, e aí rotula toda instrução.
2. **`loop`/`loope`/`loopne`/`jecxz` fora da coleta de alvos** — o coletor
   só olhava mnemônicos começando com `j`, então os rótulos desses saltos
   nunca eram emitidos.
3. **Tamanho de função subestimado** — a descoberta estima o tamanho pelo
   prólogo e truncava funções longas; 933 de 962 saltos amostrados caíam
   "fora" da própria função. Corrigido com `_extent()`, que estende o
   alcance até cobrir os alvos de salto (limite de 1 MiB).
4. **Mnemônico `rep` colado no capstone 5** — `rep movsd` chega como um
   único mnemônico `"rep movsd"`, e não como prefixo + `movsd`. Todas as
   instruções de string com `rep` eram tratadas como não suportadas.
5. **Pilha em vetor separado** — o jogo faz `lea eax,[esp+4]` e passa esse
   ponteiro adiante; com a pilha fora do espaço de endereços do guest, esses
   ponteiros seriam inválidos. A pilha virou região de memória real.
6. **Cobertura SIMD** — implementados MMX, SSE, SSE2 e 3DNow! (o caminho
   AMD, que o jogo usa). Restam 323 formas de operando não cobertas.
7. **Indexação RVA vs VA (o bug mais grave)** — a extensão de alcance
   (`_extent`) passou a indexar a imagem do PE com o endereço VIRTUAL
   absoluto em vez do RVA. Isso desmontou a região ERRADA do arquivo para
   todas as 6.698 funções, produzindo C a partir de lixo — e o número de
   "não suportadas" caiu para 228, o que *parecia* uma melhoria. Só foi
   detectado ao conferir a primeira instrução de uma função conhecida
   (`0x401030` deveria começar com `push ecx`; começava com `add [eax],al`).
   Os números honestos estão na seção 2. Lição: cobertura alta é suspeita.

---

### 2.3 Correções no carregador de PE feitas por medição

O carregador (`android/app/src/main/cpp/x2rw_loader.inc`, compartilhado com o
build desktop) foi escrito e depois **verificado contra o `pefile`**, que é a
referência. Três erros apareceram — todos silenciosos, do tipo que só se
manifesta como "o jogo trava e não se sabe por quê":

| # | Erro | Como foi descoberto | Correção |
|---|---|---|---|
| 1 | `AddressOfEntryPoint` lido do offset `0x28` do optional header | `u32(raw, opt+0x28)` devolvia **4**; o valor real é **0x1C3042**. `0x28` é `Major/MinorOperatingSystemVersion` | offset **`0x10`** |
| 2 | Imagem montada a partir dos bytes CRUS do arquivo | Seções de um PE têm `PointerToRawData` ≠ `VirtualAddress`. Mapear o arquivo direto põe todo o conteúdo no endereço errado | monta a imagem de memória: cabeçalhos em RVA 0, depois cada seção de `PointerToRawData` → `VirtualAddress` |
| 3 | Cada seção copiada com `min(VirtualSize, SizeOfRawData)` | Comparação byte a byte com `pefile.get_memory_mapped_image()` acusou **216 bytes diferentes** (`.text` e `.rdata`) | copia `SizeOfRawData` inteiro; resultado fica **idêntico** ao `pefile` |

Também foi adicionada uma checagem que **recusa carregar** o módulo se a
`ImageBase` do arquivo não for o endereço em que ele será mapeado (não há
aplicação de relocations; carregar em base diferente quebraria todo ponteiro
absoluto).

**Resultado verificado:** para `Wolvie.exe` e `binkw32.dll`, a imagem gerada
pelo algoritmo do carregador é **byte a byte igual** à do `pefile`
(`get_memory_mapped_image()`), com a única diferença de que o `pefile` arredonda
`SizeOfImage` para `SectionAlignment` no final.

### 2.4 ImageBase da binkw32.dll estava errada

O valor usado em todo o projeto era `0x03000000`. Lendo o cabeçalho PE:

```
binkw32.dll: ImageBase = 0x30000000   (não 0x03000000)
```

É um fator 10 de diferença, não um erro de digitação irrelevante: todas as 173
funções da binkw32 tinham sido liftadas com VAs começando em `0x03001000`, e
uma `x2rw_call(c, 0x03001000u)` nunca resolveria para o módulo mapeado em
`0x30000000`. Corrigido em `tools/games.py`, `Makefile`,
`runtime/x2rw_runtime.h` e `docs/RECON.md`, e o `gen/` foi **regerado**.

### 2.5 Compilação de verdade (runtime + shims + todo o `gen/`)

Até aqui nada tinha sido compilado. Obtendo um compilador real nesta máquina
(`zig cc` = clang 18.1.6, alvo `aarch64-linux-musl`), os fontes foram
compilados **de verdade**. Resultado medido, não estimado:

| Conjunto | Arquivos | Erros |
|---|---|---|
| `runtime/*.c` (2) | 2 | 0 |
| `runtime/shims/*.c` | 11 | 0 |
| `gen/*.c` (Wolvie 34 + binkw32 26) | 58 | 0 |
| **Total** | **71** | **0** |

Isso **não** é o build do APK (proibido localmente): é compilação `-c` dos
mesmos `.c` que o CI compila, sem Gradle, sem NDK, sem SDK, sem empacotar.
Arquitetura idêntica (`aarch64`), frontend praticamente idêntico, **libc
diferente** (musl aqui, bionic no Android) — por isso continua valendo a regra
de que a palavra final é do CI.

Três erros reais apareceram e foram corrigidos por causa desta compilação:

1. **`gen/*.c` chamava `x2rw_simd_*` e `x2rw_sop_*` sem declaração.** O
   preâmbulo do lifter só incluía `x2rw_runtime.h`; as declarações estão em
   `x2rw_simd.h`. Em C99+ isso é **erro** no clang do NDK, não aviso. O mesmo
   valia para `memcpy` (falta de `<string.h>`). Corrigido no preâmbulo de
   `tools/lifter.py` e o `gen/` foi regerado.
2. **`x2rw_mod_wolvie.n_functions` estava fixo em `0`** (ver §2.6).
3. Comentários continham `runtime/shims/*.c` dentro de bloco `/* */`, gerando
   `-Wcomment`. Corrigido nos 4 arquivos.

### 2.6 A tabela de funções estava VAZIA na execução

O runtime declara `x2rw_mod_wolvie` com `n_functions` no inicializador:

```c
const x2rw_module x2rw_mod_wolvie = {
    "Wolvie", X2RW_BASE_WOLVIE, X2RW_SIZE_WOLVIE, x2rw_funcs_Wolvie, 0
};                                              /* ^ sempre 0, ignorando  */
                                                /*   x2rw_n_funcs_Wolvie  */
```

O comentário dizia "vem do gen/", mas o valor nunca era atribuído. Em C não dá
para inicializar um objeto de duração estática com outra variável `const`, e
a correção anterior parou aí. Consequência medida, rodando o port:

```
[x2wr] módulos: 0 + 0 funções
[falha] code=3 va=005c3042 msg=call para VA não resolvido   ← o próprio entrypoint
```

**Toda** `x2rw_call` falhava. Corrigido atribuindo as contagens em
`x2rw_modules_register()` a partir de `x2rw_n_funcs_<modulo>` (o mesmo código
serve o build app-only, onde o valor é 0 de propósito). Depois:

```
[x2wr] módulos: 16069 + 459 funções
```

### 2.7 A descoberta de funções estava descartando 3/4 do binário

`tools/discover.py` varria `.text` com `md.disasm(code, text_va)` de uma vez
só. `md.disasm` **para no primeiro byte que não decodifica**, e `.text` tem
dados e padding no meio. Medido: a varredura morria cedo e só **8** funções
de 6.525 eram marcadas como alvo de `call`; o resto vinha do heurístico de
prólogo por busca cega de bytes, que acha falsos positivos *dentro* de funções
reais. Como o tamanho de cada função era a distância até o próximo "início",
as funções ficavam **truncadas no meio do corpo** — 79% delas terminavam sem
`ret`/`jmp`.

Prova mais direta: o entrypoint (`VA 0x005c3042`, `_WinMainCRTStartup`) foi
liftado com **29 bytes** — só o prólogo SEH — e "retornava normalmente" sem
executar nada:

```
[x2wr] entrypoint retornou normalmente
[x2wr] leituras=1 escritas=6 páginas=87 chamadas=1
```

Correções:

- varredura **ressincronizante** (avança 1 byte quando não decodifica);
- `_extent()` estende a faixa até o primeiro terminador (`ret`/`jmp`/`int3`)
  quando a faixa termina no meio de uma sequência;
- `_extent()` nunca passa do próximo início **confiável** (alvo de `call`,
  export ou thunk de IAT): `jmp` para longe é salto de cauda, não código desta
  função.

Sem essa última delimitação a extensão encadeava de salto em salto e
absorvia funções inteiras: medido **+48 MiB** de x86 reliftado e 10.388
instruções não suportadas. Com ela: **+1,1 MiB**, mediana de 136 bytes.

Antes → depois (medido):

| Métrica | Antes | Depois |
|---|---|---|
| Funções | 6.698 | **16.528** |
| Instruções não suportadas | 2.823 | **2.349** |
| Tamanho do `gen/` | 65,6 MiB | **70 MiB** |
| Tempo de lifting (`--jobs 32`) | 69,85 s | **81,27 s** |
| Entrypoint liftado | 29 bytes | **206 bytes** |

### 2.8 CORRIGIDO: convenção de chamada da pilha (off-by-4 em todo argumento)

Este era o bloqueador principal. **Está corrigido**, e a correção foi
verificada duas vezes: no desmontar do próprio binário e num aparelho real.

O projeto adotava, por decisão documentada em `x2rw_shims.h`, o modelo "sem
endereço de retorno": `x2rw_call(c, va)` chamava `fn(c)` direto, sem empilhar
nada, e `ret` só fazia `return;`. O primeiro argumento ficava em `esp`.

O código x86 original, porém, foi compilado para o modelo **com** endereço de
retorno, e acessa os argumentos pelos deslocamentos reais. Prova tirada do
próprio binário — `_heap_init` em `0x5c40b1`:

```asm
005c40b1  xor   eax, eax
005c40b3  push  0
005c40b5  cmp   dword ptr [esp + 8], eax   ; lê o ARG0 depois de 1 push
```

No entry de `_heap_init`, `arg0` está em `[esp+4]` — depois do `push 0`, o
mesmo valor cai em `[esp+8]`. Isso só fecha com o endereço de retorno em
`[esp]`. No modelo antigo `arg0` estava em `[esp]`, então `[esp+8]` era
`arg1`: **toda leitura de argumento na pilha estava 4 bytes deslocada.**

Segunda confirmação, agora em `_alloca_probe` (`0x5c1770`), que existe para
provar que o valor empilhado é lido de volta do lugar certo:

```asm
005c1792  mov   eax, esp
005c1798  mov   ecx, [eax]        ; salva o que estava em [esp]
005c179a  mov   eax, [eax + 4]    ; <-- lê o ENDEREÇO DE RETORNO
005c179d  push  eax
005c179e  ret                     ; "retorna" usando o valor relido
```

Ou seja: o binário tem código cuja função é recuperar `[esp+4]` como endereço
de retorno. Sem a sentinela, ele releria `arg0`.

O que mudou (as três partes juntas, como registrado antes):

1. `x2rw_call()` empilha `X2RW_RET_SENTINEL` (`0xFFFFFFF0`) antes de `fn(c)`;
2. `ret` no código liftado virou `c->r.esp += 4 + imm16; return;`;
3. `x2rw_arg(c,i)` lê `esp + 4 + 4*i` e o dispatcher de imports desconta
   `4*(nargs+1)`.

O valor da sentinela é irrelevante: o retorno acontece no C. O que importa é
o **espaço** que ela ocupa, que é o que mantém `[esp+4]` sendo o primeiro
argumento.

Efeito medido no desktop (aarch64, `LB_TRACE=1`), antes e depois:

```
antes:  [k32] HeapCreate
        [trace] ... x2rw_call(005d4158)  GetVersionExA
        [trace] x2rw_call(005c40ae)  <- falha
depois: [k32] HeapCreate
        [k32] GetEnvironmentVariableA(''): não existe   <- argumento read coerente
        [trace] ... x2rw_call(001deeee)
        [trace] x2rw_call(005c409b)  <- falha (outra causa, ver 2.10)
```

O ponto importante do "depois" não é chegar mais longe: é que
`GetEnvironmentVariableA` passou a receber uma string vazia plausível em vez
de lixo — sinal de que a leitura de argumento passou a apontar para o lugar
certo.

### 2.9 CI: o APK sai do `build.yml`

Quatro execuções no GitHub Actions, cada uma derrubando um defeito diferente.
Nenhuma linha de C foi "achada no olho": os quatro problemas abaixo só
apareceram porque o CI compilou de verdade.

| Run | Modo | Onde parou | Causa medida |
|---|---|---|---|
| 40732101341 | push (app-only) | passo "Android SDK" | `android-actions/setup-android@v3` roda `sdkmanager tools`, pacote que **não existe mais**: `Warning: Failed to find package 'tools'` |
| 40733271314 | full | `:app:processDebugResources` | `AAPT: error: resource style/X2WR not found` |
| 40735670541 | full | `:app:compileDebugKotlin` | `'return' is not allowed here` + 2× `Uri?` vs `Uri` |
| **40737389260** | **full** | **—** | **sucesso** |

O que cada um ensinou:

1. **`setup-android@v3` está quebrado** para o repositório atual do Google. O
   runner `ubuntu-24.04` já traz `ANDROID_SDK_ROOT` e `cmdline-tools`; o passo
   agora só aponta o `PATH` e instala plataforma/build-tools/NDK/cmake.
2. **`X2WR.Status` sem `parent`**. O AAPT trata a notação de ponto como
   herança: `X2WR.Status` herda de `X2WR`, que não existe. `parent=""` desliga
   isso. (Só apareceu porque o `.xml` nunca tinha sido linkado.)
3. **`return` puro dentro do lambda de `registerForActivityResult`**. Além de
   ilegal, o ramo de saída deixa de ser terminante e `uri` continua `Uri?` no
   smart cast — daí os três erros de uma vez. `return@registerForActivityResult`
   resolve os três.
4. **Deslocamento indefinido na rotação.** O CI emitiu
   `-Wshift-count-overflow` em `gen/Wolvie_part007.c:28543` (e 014/029): o
   lifter gerava `x >> (32 - cnt)`; com `cnt == 0` isso é `>> 32`, que é
   **comportamento indefinido** em C. Corrigido para `(32 - cnt) & 0x1f` (com
   `cnt == 0` a rotação vira `x | x == x`, que é o resultado certo) — e o `gen/`
   foi regerado. Este era um bug de *correção*, não só um aviso.

Resultado do run que passou (`40737389260`), medido:

```
toolchain:      NDK 27.0.12077973, Gradle 8.7, JDK 17, ubuntu-24.04
artefato:       x2wr-apk-debug-full-8   13.056.270 bytes
app-debug.apk:  14.077.625 bytes
libx2wr.so:     24.647.744 bytes  (lib/arm64-v8a/)
ELF:            e_type=3 (ET_DYN), e_machine=183 (AArch64), 64 bits, LE
passos:         1..18 todos success (inclui "Verificar que o APK contém a arm64-v8a")
```

O `libx2wr.so` acima é o executável real do jogo recompilado: runtime + 11
shims + as 16.528 funções liftadas, tudo em ARM64 nativo.

**O que isso prova e o que não prova.** Prova que o pipeline de build funciona
de ponta a ponta e que o código compila com o NDK para arm64. Não prova que o
APK instala (não há dispositivo aqui), nem que o jogo chega ao menu — o
bloqueador de §2.8 e a ausência de camada gráfica (§3.4) continuam valendo.

### 2.10 CORRIGIDO: jmp para epílogo compartilhado (empilhava sentinela a mais)

Depois da correção de ABI, o jogo passou a morrer em `0x005c409b`, ainda
"call para VA não resolvido". A causa é outra, e é uma armadilha clássica do
código gerado pelo MSVC: **epílogo compartilhado**.

Em `0x5c3f69`:

```asm
005c3fa4  push   1
005c3fa6  pop    eax
005c3fa7  jmp    0x5c40ae        ; não é call: é um salto
```

`0x5c40ae` não inicia função nenhuma. É o fim (`pop ebx; leave; ret`) que
várias funções reusam:

```asm
005c40ae  pop    ebx
005c40af  leave
005c40b0  ret
```

A descoberta nunca marcou esse endereço como função — ele não tem prólogo nem
`call` apontando para ele. O lifter, vendo um `jmp` para fora da faixa,
transformava em `x2rw_call(c, alvo)`. E `x2rw_call` **empilha a sentinela**.
O `pop ebx` do epílogo então consumia a sentinela em vez do `ebx` salvo, e o
`ret` seguinte desalinhava a pilha em 4 bytes. Foi esse desalinhamento que
produziu o `call` para endereço inválido no aparelho.

Correção em duas partes:

1. `x2rw_tail(c, va)` — transfere o controle **sem empilhar nada**, que é o
   que o `jmp` do x86 faz. A sentinela que já está na pilha continua sendo a
   única, e é ela que o `ret` do destino consome. Todo `jmp`/`jcc`/`loop`
   para fora da faixa passou a usar isso em vez de `x2rw_call`.
2. Fechamento dos alvos: o alvo de um salto de cauda é liftado como entrada
   própria (com janela curta), senão `x2rw_resolve()` não teria para onde
   transferir. Medido: **+32.002 entradas no Wolvie, +1.488 na binkw32**.

A primeira tentativa do item 2 deu errado e está registrada porque o erro é
instrutivo: deixei `_extent` correr até 8 KiB por alvo. O total de instruções
foi de 939 mil para **4,1 milhões** e a cobertura **caiu** de 99,75% para
94,28% — os alvos são epílogos de 3 bytes, e a janela larga desmontava código
inalcançável (e dados) como se fosse função. Com janela de 64 bytes:
1.295.543 instruções, 99,55% cobertas.

### 2.11 CORRIGIDO no aparelho: "Falha ao iniciar (código 1)" era permissão

Primeiro teste em aparelho real (motorola edge 30 fusion, Android 13/API 34),
com o APK do CI instalado. O log do aparelho:

```
I X2WR : pasta do jogo: /storage/emulated/0/Download/X2 Wolverine/X2 Wolverine's Revenge
E X2WR : abrir .../Wolvie.exe: Permission denied
E X2WR : nativeRun: x2rw_host_init falhou        -> "Falha ao iniciar (código 1)"
```

Não era a pasta. No Android 11+ o `stat` em `/storage/emulated/0` é liberado
mesmo sem permissão, mas o `open()` não é. A validação do launcher usava
`File.isDirectory`/`isFile` — que são `stat` — e passava; quem falhava era o
`open()` do C, 4 chamadas depois.

O SAF (`OpenDocumentTree`) **não** resolve sozinho: ele dá acesso por
`content://` URI, e o jogo abre arquivos por caminho (`data.pkr`,
`wavs.pkr`, `movies/*.bik`, `Wolvie.ini`). Migrar para SAF seria reescrever
todo o I/O do jogo.

O que foi feito:

* `MANAGE_EXTERNAL_STORAGE` (`Todos os arquivos`) no manifesto, mais
  `requestLegacyExternalStorage` para a API 29 e `READ_EXTERNAL_STORAGE`
  pedido em tempo de execução até a API 29;
* `GameFolder.isReadable()` passou a provar a leitura abrindo os arquivos
  obrigatórios e lendo 1 byte — nada de `isFile`;
* o botão JOGAR, se não conseguir ler, abre a tela de permissão, explica o
  motivo e continua sozinho quando o usuário volta;
* a mensagem de erro do código 1 deixou de culpar a pasta.

**Resultado medido no aparelho depois da correção** (segundo log):

```
I X2WR : /storage/.../Wolvie.exe: 2916352 bytes
I X2WR : Wolvie.exe mapeado em 0x00400000 (entry RVA 0x1c3042, 2916352 bytes)
I X2WR : binkw32.dll mapeado em 0x30000000 (377856 bytes)
I X2WR : módulos registrados: 16069 + 459 funções
I X2WR : nativeRun: chamando entrypoint VA=0x005c3042 (RVA=0x1c3042 lido do PE)
E X2WR : nativeRun: fault code=3 va=0x005c40ae msg=call para VA não resolvido
```

Ou seja: o port **abre os arquivos, carrega os dois PE, registra os módulos e
executa código lifted dentro do aparelho**. O que falta ali é justamente o
`0x5c40ae` do item 2.10 — o mesmo defeito reproduzido no desktop, o que
confirma que o harness de desktop é um oráculo válido para o aparelho.

## 3. NÃO testado — o que ainda falta

> Leia esta seção junto com §2.9: compilar com o NDK e gerar o APK saíram
> desta lista. O que resta aqui é o que realmente **não** foi executado.

### 3.1 Compilação

> **Esta seção mudou duas vezes.** Primeiro quando o runtime, os 11 shims e
> todo o `gen/` foram compilados com `zig cc` (§2.5); depois quando o CI
> compilou tudo com o **NDK de verdade** e emitiu o APK (§2.9). Portanto
> *compilar para Android* e *gerar o APK* **saíram** desta lista.

- ~~Compilar com o NDK / clang do Android (bionic)~~ — **feito no CI** (§2.9).
- ~~Compilar os arquivos Android-específicos (`x2rw_android_main.c`)~~ —
  **feito no CI** (§2.9).
- **Instalar o APK** num aparelho ou emulador — não há dispositivo aqui. O
  artefato existe e foi inspecionado por dentro (`lib/arm64-v8a/libx2wr.so`,
  ELF AArch64), mas **ninguém o abriu**.
- **Rodar o APK** — depende de instalar.
- **Build local do APK** — continua proibido por regra do projeto; o APK só
  nasce no CI.

### 3.2 Execução do jogo

- O jogo **foi executado** — mas só o binário **desktop aarch64** construído
  aqui a partir dos mesmos fontes (`desktop/main.c` + runtime + shims + `gen/`),
  não o APK. Resultado em §2.8: executa o CRT, chama `HeapCreate` e
  `GetVersionExA`, e para num `call` para `VA 0x005c40ae` por causa do
  deslocamento de 4 bytes na pilha.
- O jogo **não chegou ao menu principal**. Critério de aceitação **não
  atingido** — e mesmo com o bloqueador de §2.8 resolvido ele não chegaria:
  `Direct3DCreate8` devolve `D3DERR_NOTAVAILABLE` enquanto não existir backend
  gráfico (§3.4).
- Nenhuma cena, nenhum som, nenhum frame foi renderizado.

### 3.3 Validação de correção do lifter

- `tools/difftest.py` está escrito, mas **não foi executado**: ele precisa do
  `unicorn` como oráculo x86 real e o pacote não está instalado aqui.
  Portanto: **a correção do lifter não está medida.** O que se sabe é apenas
  que ele *gera* C; não que o C *faça a mesma coisa* que o x86.
- `difftest` mede só funções isoláveis (sem `call`, sem privilégio, que
  terminam sozinhas). Mesmo quando rodar, a taxa cobre um subconjunto — ela
  diz "entre as funções testáveis, X% batem" e **não** diz "o jogo funciona".

### 3.4 Camadas do port

| Camada | Situação |
|---|---|
| Gráficos D3D8 → tela Android | **Não implementado.** O `Direct3DCreate8` devolve um objeto sentinela. |
| Áudio (DirectSound/winmm) | **Não implementado** (gancho definido, sem saída real). |
| Decodificação de vídeo Bink | **Não implementado.** Os 15 exports existem como máquina de estados; nenhum frame é decodificado. |
| Entrada (DirectInput8) | Estrutura pronta (teclado/mouse lidos de um array), **não testada em dispositivo**. |
| PKR3 (assets) | **Não decodificado.** O mapeamento deslocamento→arquivo continua parcial. |
| Overlay de toque | Escrito, **nunca desenhado numa tela**. |
| Launcher (validação da pasta) | Escrito, **nunca executado**. |

### 3.5 Hardware e desempenho

- **Sem hardware Android disponível.** Nenhum teste em dispositivo.
- FPS, uso de CPU, consumo de RAM, tempo de carregamento: **não medidos**.
- Instalação do APK: **não testada**.
- Tempo de compilação no CI e memória por unidade de tradução: **estimativa**
  (~1,5 GB por arquivo, herdada da referência LMS_Android). A partição em
  16 arquivos de ~6 MiB existe justamente para caber, mas **o número real
  não foi medido**.

---

## 4. Hipóteses declaradas (NÃO são fatos)

1. Os shims Win32 escritos bastam para a inicialização do jogo — **não
   verificado**.
2. O objeto D3D8 sentinela permite que a inicialização prossiga até o menu
   — **não verificado**; pode falhar antes.
3. O código MMX/SSE/3DNow! implementado é suficiente para a matemática do
   jogo — **99,68 % das instruções estão cobertas**, mas a semântica de
   cada uma **não foi comparada** com o hardware real.
4. `pfrcp`/`pfrsqrt` (3DNow!) divergem do hardware AMD, que devolve
   aproximações de ~14 bits; aqui devolvemos o valor exato em `float`. Isso
   **muda o resultado** de qualquer código que dependa do erro da
   aproximação. Documentado em `runtime/x2rw_simd.c`.
5. Um build completo cabe no tempo do runner — **não medido**.

---

### 2.12 CORRIGIDO: argumentos de import deslocados + 26 mil alvos falsos

Quatro defeitos medidos, todos com sintoma visível no harness:

**(a) `x2rw_call` despachava import ANTES de empilhar a sentinela.** O leitor de
argumentos (`esp+4+4*i`) e o desconto de `4*(nargs+1)` pressupõem o endereço de
retorno na pilha. Medido com o log de imports (`LB_TRACE=1`):

```
HeapCreate(00001000, 00000000, fffffff0)   <- correto: (0, 0x1000, 0)
InitializeCriticalSection(fffffff0)        <- recebeu a SENTINELA
```

Todo import chamado por `call [IAT]` recebia argumentos deslocados. Depois da
correção: `HeapCreate(0, 0x1000, 0)`, `HeapAlloc(00a00000, 8, 0x80)`,
`TlsSetValue(0, 20000010)`.

**(b) Salto indireto para fora da função era tratado como erro.** Medido:
`jmp dword ptr [0x5d4144]` em `0x5cccf2` salta para `GetACP`. É salto de cauda.
Agora cai em `x2rw_tail()`.

**(c) A descoberta marcava alvo de `call` FALSO dentro do corpo de uma função.**
A varredura linear ressincroniza byte a byte e decodifica `call rel32` em região
de dados. Medido: `0x5ccb2b` (`push ebp; mov ebp,esp; sub esp,0x18`) com alvo
falso em `0x5ccb52` (`xor esi,esi`, corpo de um `if`), 39 bytes adiante. Como o
tamanho da função era a distância até o próximo início, a função real era
**partida** e a segunda metade liftada a partir do meio. Corrigido: alvo sem
prólogo que cai dentro do corpo de outra função é descartado (263 no Wolvie,
11 na binkw32).

**(d) O fechamento de saltos de cauda criava entrada em QUALQUER alvo.** Medido:
`0x5c8cf3` cai no MEIO do `call _alloca_probe` (`0x5c8cf2`, 5 bytes). O corpo
gerado era lixo; o lixo continha outro salto de cauda, que virava outra entrada,
que gerava outro salto — o programa entrava em ciclo e estourava a pilha
(`esp` chegou a `0x00010000`, o fundo da pilha). Agora o alvo só vira entrada se
uma decodificação **alinhada**, partindo do início da função que o contém,
topar com ele como início de instrução.

Resultado medido em (d): 26.953 alvos falsos descartados no Wolvie, 6.143
entradas legítimas mantidas. Instruções caíram de 1.259.953 para 978.612 e a
**cobertura subiu** de 99,54 % para 99,76 % — ou seja, a maior parte do que
estava sendo liftado era lixo.

Duas tentativas de regra foram medidas e **descartadas** (registro honesto):

- Exigir que o alvo terminasse em `ret`/`jmp` em até 32 bytes: recusava alvo de
  desvio legítimo no meio da função (medido: `0x5c3371`, dentro de `0x5c3368`).
- Parar a decodificação no primeiro terminador: recusava epílogo compartilhado,
  que vive logo depois de um `ret` (medido: `0x5c410a` é `push 1; pop eax; ret`,
  precedido por um `ret` em `0x5c4109`).

**(e) O lifter não apagava as partições antigas.** O número de partes muda a
cada regeração (32 arquivos numa execução, 24 na seguinte). Partição velha
esquecida no diretório é recompilada — o build pega `gen/*.c` — e o link falha
com `duplicate symbol` (medido: `x2rw_binkw32_d6c0` em `binkw32_part023.c` e
`binkw32_part024.c`). Pior que falhar: uma partição velha com a **mesma** função
mas código de antes ligaria em silêncio e rodaria código errado.

**(f) Harness de diagnóstico no CI.** Job `desktop` do `build.yml`: baixa `gen/`
do repositório privado, compila em paralelo com `zig cc`, publica
`x2wr-desktop-aarch64`. Executado com sucesso no run 40872543379.

### 2.13 ONDE A EXECUÇÃO PARA AGORA (honesto)

O harness chega ao seguinte, nesta ordem, e para:

```
módulos registrados: 22660 + 827 funções
HeapCreate(0, 0x1000, 0)   InitializeCriticalSection x4   TlsAlloc
TlsSetValue(0, 20000010)   HeapAlloc(00a00000, 8, 0x80)
GetCommandLineA   GetEnvironmentStringsW   GetACP   GetCPInfo(1252)
GetStringTypeA/W  LCMapStringW  MultiByteToWideChar  GetCurrentThreadId
GetStartupInfoA
FAULT code=3 va=0x005cbf2c msg=salto de cauda para VA não resolvido
estado: esp=003ffee8 ebp=003fff3c eip_pc=005cbe3a
```

`0x5cbf2c` cai **dentro** de `lea esi,[ecx+eax*4]` (que começa em `0x5cbf2b`):
é alvo falso de um **salto indireto calculado**, não um stub faltando. `esp` e
`ebp` estão alinhados — não há mais corrupção de pilha; agora é valor.

Ou seja: a inicialização do CRT vai longe (heap, TLS, seções críticas, ambiente,
tabelas de ctype/locale), mas **o jogo ainda NÃO chega ao menu principal**, e a
camada gráfica ainda não foi exercitada nem uma vez.

### 2.14 Superfície Direct3D 8 REALMENTE usada (medido no binário)

Varredura de `call [reg+disp32]` no `.text` de `Wolvie.exe`, casando o
deslocamento com a vtable de `IDirect3DDevice8`:

| Slot | Método | Chamadas |
|---|---|---|
| 63 | `SetTextureStageState` | 106 |
| 51 | `GetRenderState` | 76 |
| 50 | `SetRenderState` | 73 |
| 37 | `SetTransform` | 7 |
| 72 | `DrawPrimitiveUP` | 7 |
| 42 | `SetMaterial` | 4 |
| 46 | `LightEnable` | 3 |
| 61 | `SetTexture` | 3 |
| 70 | `DrawPrimitive` | 3 |
| 83 | `SetStreamSource` | 3 |
| 34/35 | `BeginScene` / `EndScene` | 2 / 7 |
| 36 | `Clear` | 2 |

**Limite honesto desta medição:** ela só enxerga chamadas *diretas* pela vtable
(`call [reg+disp32]`). O jogo pode carregar o ponteiro do método num registrador
antes de chamar (`mov edx,[eax+0x3C]; call edx`), o que esta varredura NÃO vê —
por isso `Present` não aparece na tabela. É um **piso**, não a lista completa. A
lista completa só sai por instrumentação em tempo de execução, quando o jogo
criar o device.

O que a tabela já sustenta: é um jogo de **pipeline fixo** (fixed-function), com
uso intenso de render states e texture stage states; os shaders aparecem, mas
pouco (`CreateVertexShader`/`CreatePixelShader` com poucas chamadas).

---

## 5. O que precisa ser feito antes de afirmar qualquer coisa

1. Rodar o workflow e **ver se compila**. É o primeiro teste real de tudo.
2. Instalar o APK e ver o launcher validar a pasta do jogo.
3. Rodar `difftest` com `unicorn` e **registrar a taxa aqui**, com o número.
4. Implementar a camada gráfica (D3D8) e só então falar em "chegar ao menu".
5. Testar em dispositivo real.

Cada item, quando concluído, muda de seção neste arquivo — com o número
medido, não com uma promessa.

---

*Última atualização: após o lifting completo e as correções de dispatch,
alcance e SIMD. Nenhuma linha de C foi compilada. O jogo não foi executado.*
