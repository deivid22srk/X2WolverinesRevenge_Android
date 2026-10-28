# X2: Wolverine's Revenge — Registro de Testes

> **Regra deste arquivo: só entra aqui o que foi realmente executado ou
> medido.** Nada é presumido como funcionando. Se algo não foi testado, está
> escrito que não foi. Quando um número aparece, ele foi medido nesta
> máquina, com o comando indicado.

Máquina de desenvolvimento: Linux x86-64, sem compilador C instalado
(`gcc`/`cc`/`clang` ausentes) e sem acesso de superusuário para instalá-lo.
**Consequência honesta: nenhum código C deste repositório foi compilado
localmente.** A primeira compilação de verdade acontece no GitHub Actions
(`.github/workflows/build.yml`). Toda afirmação sobre "compila" ou "linka"
está, por isso, marcada como não verificada.

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

### 2.8 BLOQUEADOR ABERTO: convenção de chamada da pilha

Este é o achado mais importante desta rodada e **não está corrigido**.

O projeto adota, por decisão documentada em `x2rw_shims.h`, o modelo "sem
endereço de retorno": `x2rw_call(c, va)` chama `fn(c)` direto, sem empilhar
nada, e `ret` só faz `return;`. O primeiro argumento ficaria em `esp` (não em
`esp+4`). O dispatcher de imports segue essa regra (`x2rw_arg(c,i)` lê
`esp + 4*i`; o handler desconta `4*nargs`).

O problema: **o código x86 original foi compilado para o modelo com endereço
de retorno**. Ele acessa argumentos com os deslocamentos reais, e esses
deslocamentos ficam 4 bytes deslocados no nosso modelo. Exemplo do próprio
binário, `_heap_init` em `0x5c40b1`:

```asm
005c40b1  xor  eax, eax
005c40b3  push 0
005c40b5  cmp  dword ptr [esp + 8], eax    ; no x86 real: arg0 == 0
```

No x86 real, no entry `esp` aponta para o endereço de retorno; depois de
`push 0`, `[esp+8]` é `arg0`. No nosso modelo, `esp` já aponta para `arg0`,
então `[esp+8]` é `arg1`. **Toda leitura de argumento na pilha está 4 bytes
deslocada.**

Consequência observada ao executar (`LB_TRACE=1`):

```
[trace] x2rw_call(005c3042)   entrypoint
[trace] x2rw_call(001deeb0)   ← valor de slot de IAT não corrigido
[trace] x2rw_call(005c40b1)   _heap_init
[trace] x2rw_call(005d4200)   HeapCreate (import)
[trace] x2rw_call(005c3f69)
[trace] x2rw_call(005c1770)   _alloca_probe: termina em push/ret
[trace] x2rw_call(005d4158)   GetVersionExA (import)
[trace] x2rw_call(005c40ae)   ← pop ebx; leave; ret  (epílogo, não é função)
[falha] code=3 va=005c40ae msg=call para VA não resolvido
```

Como corrigir (as três partes precisam mudar juntas, senão uma conserta e a
outra quebra):

1. `x2rw_call()` empilha um endereço de retorno sentinela antes de `fn(c)`;
2. `ret` vira `esp += 4 + imm16` (e o salto de cauda passa a desempilhar a
   própria sentinela antes de retornar);
3. `x2rw_arg(c,i)` lê `esp + 4 + 4*i` e o dispatcher desconta `4*(nargs+1)`.

Isso é exatamente o que `tools/difftest.py` existe para validar — e ele
**nunca foi executado** (§3.3). Não fiz essa mudança sob pressa de tempo: ela
troca a ABI de 16.528 funções e dos 226 shims, e sem o oráculo x86 ligado o
risco de "consertar" errado é alto. Fica registrada como bloqueador aberto.

## 3. NÃO testado — nada abaixo foi executado

### 3.1 Compilação

> **Esta seção mudou.** Até a data da última revisão deste documento, nada
> tinha sido compilado. Depois de obter um compilador real nesta máquina, o
> runtime, os 11 shims e **todo** o `gen/` foram compilados para `aarch64` —
> o resultado está em §2.5. O que continua **não** testado aqui:

- **Compilar com o NDK / clang do Android (bionic)** — o compilador usado foi
  `zig cc` (clang 18.1.6) para `aarch64-linux-musl`. É a mesma arquitetura e
  praticamente o mesmo frontend, mas **não** é o NDK nem a libc do Android.
- **Compilar os arquivos Android-específicos** (`x2rw_android_main.c`) — usam
  `jni.h`, `android/log.h` e `AAssetManager`, ausentes aqui. Não compilados.
- **Linkar o binário final do APK** — o link testado foi o do caminho
  *app-only* com as tabelas vazias, usando o `main()` de teste.
- **Emitir o APK** — nunca localmente; a regra do projeto proíbe build local
  do APK. Isso continua exclusivo do CI.

### 3.2 Execução do jogo

- O jogo **não foi executado** em nenhuma forma: nem no `desktop/`, nem em
  emulador, nem em dispositivo.
- O jogo **não chegou ao menu principal**. Isto é o critério de aceitação e
  **não foi atingido**.
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
