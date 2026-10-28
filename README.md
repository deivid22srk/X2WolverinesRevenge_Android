# X2: Wolverine's Revenge — Port Nativo Android (ARM64, Recomilação Estática)

> [!WARNING]
> **Nada deste repositório contém código ou dados do jogo.** `Wolvie.exe`, `data.pkr`, `wavs.pkr`, `binkw32.dll`, os assets e até o C gerado (`gen/`) **não** estão aqui. Tudo isso vem da sua própria cópia do jogo. Este repositório entrega apenas a infraestrutura do port: discovery, lifter, runtime, app Android e CI.

## 🎮 Sobre este projeto

Port **nativo** de 32 bits → ARM64 do *X2: Wolverine's Revenge* (Activision, 2003) para Android via **recompilação estática**: o binário Windows (x86) é analisado com Capstone/pefile, convertido para C, e compilado com o NDK. **Sem Wine, Box64, FEX ou qualquer emulação x86 em tempo real.**

Arquitetura derivada dos projetos de referência:
- **Principal**: https://github.com/ttom795/LMS_Android (Lego Masters of Magic / adaptado)
- **Secundária**: https://github.com/deivid22srk/Ultimate_Spider_Man_Android (outra Activision, runtime generalizado)
- **Runtime base**: https://github.com/ttom795/Skyrim_Android

Repositório alvo do gerado (`gen/`): **privado** (`deivid22srk/X2WR_gen`) → o `build.yml` baixa o `gen.tar.zst` com `GEN_TOKEN`.

---

## 📊 Status

| Componente | Status | Comentários |
|---|---|---|
| Plataforma confirmada (PE32 x86) | ✅ Feito | docs/RECON.md — evidências pefile |
| DRM verificado | ✅ Feito | Sem DRM; release NoCD fornecida |
| API gráfica confirmada | ✅ Feito | **D3D8** (não D3D9) — medido nos imports |
| Superfície de imports mapeada | ✅ Feito | **226 símbolos** em 9 DLLs — docs/TESTED.md §1.1 |
| Discovery (capstone) | ✅ Feito | **16.069 funções** (exe) + 459 (binkw32) — após corrigir a varredura, docs/TESTED.md §2.7 |
| Lifter x86→C | ✅ Feito | **16.528 funções, 939.059 instruções, 2.349 não suportadas (99,75 % cobertas)** |
| Runtime (memória, flags, x87, dispatch) | ✅ Escrito | `runtime/x2rw_runtime.c` |
| Camada SIMD (MMX/SSE/3DNow!) | ✅ Escrita | `runtime/x2rw_simd.c` |
| Shims Win32 | 🔶 Escritos | 226 símbolos; muitos são sentinela — ver docs/TESTED.md |
| `tools/difftest.py` (correção do lifter) | 🔶 Escrito, **não executado** | Falta o pacote `unicorn`; **correção não medida** |
| `.github/workflows/build.yml` | ✅ Escrito | **entregável obrigatório** — Estratégia C (gen/ privado) |
| App Android (launcher + overlay) | ✅ Escrito | `android/` |
| **Compilar runtime + shims + `gen/`** | ✅ **Feito** | **71/71 arquivos, 0 erros** com clang 18 aarch64 — docs/TESTED.md §2.5 (**não** é o APK) |
| Camada gráfica D3D8→Vulkan | ❌ Não implementado | `Direct3DCreate8` devolve sentinela |
| Áudio (DirectSound + winmm) | ❌ Não implementado | Gancho definido, sem saída real |
| Decodificação de vídeo Bink | ❌ Não implementado | 15 exports existem como máquina de estados |
| PKR3 container | ⚠️ Parcial | Índice lido; mapeamento offset→arquivo pendente |
| APK compilado no CI | ✅ **Feito** | run [40737389260](https://github.com/deivid22srk/X2WolverinesRevenge_Android/actions/runs/40737389260) — `app-debug.apk` 14.077.625 B, `libx2wr.so` 24.647.744 B, ELF AArch64 (`e_machine=183`) |
| Jogo executa o CRT | 🔶 **Parcial** | Entrypoint executa (HeapCreate/GetVersionExA chamados) e para em `VA 0x005c40ae` — bloqueador em docs/TESTED.md §2.8 |
| Jogo chega ao menu principal | ❌ **Não** | Critério de aceitação **não atingido** |
| APK instalado / executado | ❌ Não testado | Não há dispositivo aqui; o artefato foi inspecionado por dentro, não aberto |
| Testado em hardware real | ❌ Não testado | Nenhum dispositivo disponível |

> O README e os docs seguem o princípio da referência: **sem otimismo**. Tudo que está "pendente" não foi construído. Consulte docs/TESTED.md para o que foi realmente executado.

---

## 📁 Estrutura do repositório

```
.
├── android/            # App Gradle: JNI, SDL2, Vulkan, overlay de toque, launcher
├── ci/gen_stub/        # Tabelas vazias → build "app-only" (público, sem código do jogo)
├── desktop/            # CMake: build Linux x86-64 para depuração do lifting
├── docs/
│   ├── RECON.md        # Fase 1 — evidências extraídas dos binários
│   └── TESTED.md       # O que foi realmente compilado/testado (sem otimismo)
├── runtime/            # Shims Win32, hooks do jogo, áudio/vídeo/input sobre SDL2
├── tools/              # discovery, lifter, difftest (unicorn), regeneração
├── .github/workflows/
│   └── build.yml       # Compila o APK no GitHub Actions (única fonte do APK)
├── Makefile            # Orquestra gen/ (local, privado) e builds
├── README.md
└── .gitignore
```

---

## 📋 Pré-requisitos (você precisa ter)

| Item | Detalhes |
|---|---|
| **Cópia do jogo** | `Wolvie.exe` + `data.pkr` + `wavs.pkr` + `binkw32.dll` + `movies/` + `music/` |
| **SHA-1 do exe** | `37f4327c4ea45d2037e9e177ecbac003469ff490` (release NoCD patched fornecida) — o launcher valida |
| **GitHub Token** (para gen/ privado) | `GEN_TOKEN` no repositório público |
| **Sistema de build** | GitHub Actions (nativo) — **não compile o APK localmente** |

### Onde conseguir a cópia do jogo
A release fornecida neste projeto (`X2_Wolverines_Revenge_Win_R_EN/`) é um **Full-Rip NoCD com widescreen fix**, testado no Windows 7/10. Use **exatamente** essa release — o launcher recusa outras versões.

Se você só tiver imagens de console (Xbox/PS2/GC): **pare**, porque a arquitetura (MIPS/PowerPC/ARM) e a API gráfica são diferentes e este pipeline não se aplica. Veja "Alternativas se não tiver a build PC" abaixo.

---

## ⚙️ Como funciona

```
┌───────────────────────┐      ┌──────────────────────────────┐      ┌──────────────────────┐
│  Sua cópia do jogo     │─────▶│  Makefile (local, privado)   │─────▶│  gen/ (C gerado)      │
│  Wolvie.exe / PKR3     │      │  LIFT_GAME=x2wr              │      │  repositório privado  │
└───────────────────────┘      └──────────────────────────────┘      └──────────┬───────────┘
                                                                               │
                          ┌──────────────────────────────────────────────────────┘
                          ▼
        ┌───────────────────────────────────────────────────────────────────────────────────────┐
        │  GitHub Actions  .github/workflows/build.yml  (único local onde o APK nasce)            │
        │  ├─ GEN_REPO definido  →  baixa gen.tar.zst de X2WR_gen (release asset privada)       │
        │  └─ sem GEN_REPO       →  usa ci/gen_stub/  →  build "app-only" (público, sem jogo)   │
        │  └─ assembleRelease/arm64-v8a → APK assinado → release privada em X2WR_gen            │
        └───────────────────────────────────────────────────────────────────────────────────────┘
```

**Estratégia de CI (Estratégia C — a da referência LMS_Android):**
- Repositório **público** + `gen/` em repositório **privado separado** (`deivid22srk/X2WR_gen`)
- Variáveis: `GEN_REPO`, `GEN_TAG`, secret `GEN_TOKEN`
- Com `GEN_REPO`: APK completo, publicado como **release asset privada** no repositório privado
- Sem `GEN_REPO`: build **app-only** (app + runtime + stubs), artefato público — seguro, sem código derivado do jogo

---

## 🚀 Instalação

### 1. Clonar
```bash
git clone https://github.com/deivid22srk/X2WolverinesRevenge_Android.git
cd X2WolverinesRevenge_Android
```

### 2. Colocar a cópia do jogo
```
seu_home/
  game/
    Wolvie.exe        # SHA-1: 37f4327c4ea45d2037e9e177ecbac003469ff490
    data.pkr
    wavs.pkr
    binkw32.dll
    Wolvie.ini
    movies/
    music/
```

### 3. Gerar `gen/` (local, requer ferramentas da referência)
```bash
# Instala pefile + capstone + unicorn (pip)
pip install pefile capstone

# Gera o C a partir dos binários (executado em máquina Windows ou com wine + PE)
make gen LIFT_GAME=x2wr GAME_PATH=/c/seu_home/game
# → cria gen/ com os arquivos .c compilados
tar -czf gen.tar.zst gen/
# fazer upload manual de gen.tar.zst como Release Asset no repositório privado X2WR_gen
```

### 4. Configurar o repositório público
No GitHub: Settings → Secrets and variables → Actions → adicionar `GEN_TOKEN` (token com permissão para publicar releases em `deivid22srk/X2WR_gen`).

### 5. Disparar o build
```bash
gh workflow run build.yml --repo deivid22srk/X2WolverinesRevenge_Android \
  --field gen_tag=v1.0
# depois:
gh run watch --repo deivid22srk/X2WolverinesRevenge_Android
gh run download --repo deivid22srk/X2WolverinesRevenge_Android
```

O APK nasce **apenas** como artefato do workflow. Se falhar: corrija o código, `git push`, dispare de novo. **Não tente compilar o APK localmente.**

---

## 📱 Instalação no Android

1. Instale o APK (download do workflow).
2. Abra o launcher e aponte para a pasta do jogo.
3. O launcher **valida tamanho e SHA-1** de `Wolvie.exe`, `data.pkr`, `wavs.pkr`, `binkw32.dll`.
4. Configure: resolução, proporção, qualidade gráfica, controles de toque.
5. Jogue.

### Controles
- **Físico**: layout Xbox por padrão, opção Nintendo 64
- **Toque**: overlay customizável — mapeamento para Punch/Kick/Jump/Action/Claws/Crouch/Stealth/Senses/Camera
- Sensibilidade ajustável (padrão conforme INI original)

### Armazenamento
Saves e logs ficam em `port/save/`, `port/log.txt`, `port/last_exit.txt` **dentro da pasta do jogo** — fora de `Android/data`, usando access legacy/all-files. Motivo: compatibilidade com as pastas de jogo originais e permissão de leitura/escrita em todas as versões de Android.

---

## 🔧 Flags de diagnóstico (herdadas das referências)

```bash
LB_D3DTRACE=from:to      # registra chamadas D3D de N até M (apk-only)
LB_WINDOW=640x480        # força janela de depuração
LB_LOG=n                 # nível de log (0-5)
LB_GEN_JOBS=2            # jobs de compilação do gen (runner 8GB)
singlecore.txt           # cria-se em port/save/ para forçar single-core
noprof.txt               # cria-se em port/save/ para desabilitar profiling
```

---

## 🛠 Troubleshooting

| Problema | Causa provável | Solução |
|---|---|---|
| Launcher recusa pasta | SHA-1 do exe não confere | Use **exatamente** a release fornecida; hashes em RECON.md |
| "Missing data.pkr" | PKR3 não decodificado | Execute script de extração em docs/ (ou o launcher aceita PKR extraído) |
| Tela preta / crash no inicializar | D3D8→Vulkan falhou | Verifique dx8vk instalado; veja port/log.txt |
| Sem som | DirectSound/Bink não implementado | Funcionalidade pendente (ver TESTED.md) |
| Lento (FPS baixo) | Software rendering | Forçar dx8vk / GPU; verificação de flags LB_ |
| Falha no build do CI | gen/ não baixado | Configure `GEN_REPO` + `GEN_TOKEN`; ou rode build app-only |

---

## ⚠️ Problemas conhecidos

Listagem a ser preenchida com medições reais — seguindo o formato da referência:

- PKR3: mapeamento offset→arquivo ainda não totalmente decodificado (ver docs/RECON.md)
- Bink: codec de vídeo não testado em ARM64 (recompilação pendente)
- ...

Consulte sempre **docs/TESTED.md** — se não está escrito lá, não está testado.

---

## 📄 Aviso legal

*X2: Wolverine's Revenge* é propriedade intelectual da Activision. Este repositório **não distribui** nenhum asset, executável ou dado do jogo. Você precisa possuir uma cópia legal do jogo e fornecer os arquivos. A recompilação estática de um binário que você possui é feita sob as exceções de interoperabilidade/backup da sua jurisdição; consulte um advogado se tiver dúvidas.

---

## 📈 Métricas

Números **medidos** nesta máquina; o resto está marcado como não medido.

| Métrica | Valor |
|---|---|
| Tempo de discovery (Wolvie.exe) | **0,36 s** (16.069 funções, 174 imports) |
| Tempo de lifting total (`--jobs 32`) | **58,42 s** (16.528 funções) |
| Tamanho do `gen/` (C gerado) | **70 MiB** em 58 partes (Wolvie 34, binkw32 26) |
| Instruções desmontadas | **939.059** (Wolvie 912.426 + binkw32 26.633) |
| Instruções não suportadas | **2.349** (**99,75 %** cobertas) |
| `goto` para rótulo inexistente | **0** |
| Chamadas entre funções emitidas | **75.426** (`x2rw_call`) |
| Compilação local dos 71 `.c` (clang 18, aarch64) | **0 erros** em ~14 min a `-O1` |
| Execução do port (desktop aarch64, sem D3D8) | executa o CRT e para em `0x005c40ae` — §2.8 |
| Imports Win32 com handler | **226/226** (102 REAL, 50 PARCIAL, 74 STUB) |
| RAM pico de lifting | **não medido** (o lifter roda em um processo; pico não instrumentado) |
| RAM por arquivo compilado | **não medido** — a partição em 16 arquivos existe para caber; o número real só sai do CI |
| Tempo de build do APK no CI | **não medido** (limite do job: 90 min) |
| FPS medido (mobile) | **não medido** — sem hardware |
| CPU por thread | **não medido** |
| Tempo de carregamento | **não medido** |
| Correção do lifter (difftest) | **não medido** — falta `unicorn` |

---

## ⚠️ Leia isto antes de esperar qualquer coisa

O jogo **não roda ainda**. O que existe é: a análise dos binários, o
recompilador (que gera ~100 MiB de C a partir de 6.698 funções do jogo), o
runtime que dá suporte a esse C, a camada SIMD, os shims Win32, o app
Android e o workflow que constrói o APK.

O que **não** existe: a camada gráfica, o áudio, o decodificador de vídeo
Bink e a decodificação dos assets PKR3. Sem eles o jogo não chega ao menu
principal — o critério de aceitação **não foi atingido**.

Além disso, **nenhuma linha de C deste repositório foi compilada ainda**:
esta máquina de desenvolvimento não tem compilador C. A primeira
compilação real acontece no GitHub Actions. Até ela rodar, "compila" é uma
expectativa, não um fato. Ver [docs/TESTED.md](docs/TESTED.md).

---

*Repositório público `deivid22srk/X2WolverinesRevenge_Android`, com `gen/`
no repositório privado `deivid22srk/X2WR_gen` (Estratégia C).*
