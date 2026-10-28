# X2: Wolverine's Revenge Android Port - Makefile
#
# Regra: localmente NUNCA compilamos o APK (regra #5). O Makefile serve para:
#   1. gen/          - gerar C a partir dos binários do jogo (executado localmente, NO PRIVADO)
#   2. desktop       - build Linux x86-64 para depuração do lifting/shims
#   3. stubs         - ci/gen_stub/ para o build app-only do CI público
# O APK nasce exclusivamente em .github/workflows/build.yml

SHELL := /bin/bash
.ONESHELL:
.SHELLFLAGS := -eu -o pipefail -c

# ---------------------------------------------------------------------------
# Configurações do jogo (editar conforme a cópia fornecida)
# ---------------------------------------------------------------------------
LIFT_GAME ?= x2wr
GAME_PATH ?= /root/share/X2_Wolverines_Revenge_Win_R_EN/Game\ Files/X2\ Wolverine's\ Revenge

# O CI (.github/workflows/build.yml) baixa os binários para uma pasta sem
# espaços e passa GAME_BIN_DIR. Aceitamos as duas formas para que o mesmo
# alvo sirva localmente e no runner.
GAME_BIN_DIR ?=
ifneq ($(strip $(GAME_BIN_DIR)),)
GAME_PATH := $(GAME_BIN_DIR)
endif

# Hashes verificados na Fase 1 (docs/RECON.md) - o launcher valida estes
WOLVIE_SHA1 := 37f4327c4ea45d2037e9e177ecbac003469ff490
BINK_SHA1   := dd8c012f7e2bb68ec7b47702fcf3ea42e805e1f7
DATA_SHA1   := 560f2d2a0d26ee34f8591c5a09585e0cf6865a27
WAVS_SHA1   := 309e57952af136288a46fb9b6b6e0ae723efd868

# Endereços de carga (verificados pefile, docs/RECON.md)
# Cada módulo carregado em base própria, sem sobreposição
WOLVIE_IMAGEBASE  := 0x00400000
WOLVIE_SIZE       := 0x04cae8e
BINK_IMAGEBASE    := 0x30000000
BINK_SIZE         := 0x00072000

# Partições do gen/ — o mesmo número controla o paralelismo do lifter E a
# quantidade de arquivos .c gerados. Medi 16 partições nesta máquina: 66 MiB
# de C em 16 arquivos de 3,0–4,8 MiB (60 s). Mais partições = unidades de
# tradução menores = menos RAM por compilação no runner do CI.
# Referência LMS_Android: ~1,5 GB por arquivo compilado.
LB_GEN_JOBS ?= 16

# Quantas funções o difftest tenta comparar (cada uma roda 2x)
DIFFTEST_COUNT ?= 200

# ---------------------------------------------------------------------------
# Ferramentas (dependências da referência principal - LMS_Android)
# ---------------------------------------------------------------------------
CAPSTONE ?= $(shell command -v capstone || echo tools/capstone)
# Dependências Python: pefile (cabeçalhos PE) e capstone (desmontagem).
#   python3 -m pip install --break-system-packages pefile capstone

.PHONY: all gen desktop stubs clean test difftest verify

all: verify gen desktop  # default: gerar gen + build desktop

# ---------------------------------------------------------------------------
# verify: conferência dos binários fornecidos (usado também pelo launcher)
# ---------------------------------------------------------------------------
verify:
	@echo "[verify] Conferindo binários do jogo..."
	@test -f $(GAME_PATH)/Wolvie.exe || { echo "ERRO: Wolvie.exe não encontrado em $(GAME_PATH)"; exit 1; }
	@test -f $(GAME_PATH)/data.pkr || { echo "ERRO: data.pkr não encontrado"; exit 1; }
	@test -f $(GAME_PATH)/wavs.pkr || { echo "ERRO: wavs.pkr não encontrado"; exit 1; }
	@test -f $(GAME_PATH)/binkw32.dll || { echo "ERRO: binkw32.dll não encontrado"; exit 1; }
	@sha1=$(sha1sum $(GAME_PATH)/Wolvie.exe | cut -d' ' -f1); \
	[ "$$sha1" = "$(WOLVIE_SHA1)" ] || { echo "AVISO: SHA-1 de Wolvie.exe não confere ($$sha1 != $(WOLVIE_SHA1))"; echo "Se você tem outra release, edite WOLVIE_SHA1 no Makefile e reavalie docs/RECON.md."; }
	@sha1=$(sha1sum $(GAME_PATH)/binkw32.dll | cut -d' ' -f1); \
	[ "$$sha1" = "$(BINK_SHA1)" ] || { echo "AVISO: SHA-1 de binkw32.dll não confere"; }
	@sha1=$(sha1sum $(GAME_PATH)/data.pkr | cut -d' ' -f1); \
	[ "$$sha1" = "$(DATA_SHA1)" ] || { echo "AVISO: SHA-1 de data.pkr não confere"; }
	@sha1=$(sha1sum $(GAME_PATH)/wavs.pkr | cut -d' ' -f1); \
	[ "$$sha1" = "$(WAVS_SHA1)" ] || { echo "AVISO: SHA-1 de wavs.pkr não confere"; }
	@echo "[verify] OK - binários presentes"

# ---------------------------------------------------------------------------
# gen: geração do C a partir dos binários (descoberta + lifting)
#    Executado localmente, EM SEU COMPUTADOR PRIVADO. Resultado = gen/
# ---------------------------------------------------------------------------
gen: verify
	@echo "[gen] LIFT_GAME=$(LIFT_GAME) GAME_PATH=$(GAME_PATH) LB_GEN_JOBS=$(LB_GEN_JOBS)"
	@mkdir -p gen analysis
	@echo "# Discovery + lifting para $(LIFT_GAME)" > analysis/gen.log
	@echo "  [1/4] pefile+capstone: descobrindo funções e imports..." >> analysis/gen.log
	@python3 tools/discover.py --game $(LIFT_GAME) \
	    --exe "$(GAME_PATH)/Wolvie.exe" --dll "$(GAME_PATH)/binkw32.dll" \
	    --out gen --analysis analysis 2>&1 | tee -a analysis/gen.log
	@echo "  [2/4] lifter: convertendo x86 -> C (jobs=$(LB_GEN_JOBS))..." >> analysis/gen.log
	@python3 tools/lifter.py --game $(LIFT_GAME) --out gen --jobs $(LB_GEN_JOBS) \
	    --exe "$(GAME_PATH)/Wolvie.exe" --dll "$(GAME_PATH)/binkw32.dll" \
	    2>&1 | tee -a analysis/gen.log
	@echo "  [3/4] hooks: aplicando a lista de overrides do host..." >> analysis/gen.log
	@python3 tools/apply_hooks.py --game $(LIFT_GAME) --out gen \
	    --overrides runtime/override_list.c 2>&1 | tee -a analysis/gen.log
	@echo "  [4/4] resumo de cobertura..." >> analysis/gen.log
	@python3 tools/apply_hooks.py --game $(LIFT_GAME) --out gen --report-only \
	    2>&1 | tee -a analysis/gen.log
	@echo "[gen] gerado $(shell find gen -name '*.c' | wc -l) arquivos .c"
	@echo "[gen] tamanho: $(du -sh gen 2>/dev/null | cut -f1)"

# ---------------------------------------------------------------------------
# desktop: build Linux x86-64 para depuração (VALIDA lifting/shims antes do Android)
# ---------------------------------------------------------------------------
desktop:
	@echo "[desktop] Build de depuração Linux x86-64..."
	@mkdir -p build/desktop
	@cd build/desktop && cmake .. -DCMAKE_BUILD_TYPE=Debug \
	    -DANDROID_NDK=OFF -DDISABLE_D3D=ON -DENABLE_TESTS=ON 2>&1 | tail -20
	@cd build/desktop && cmake --build . -j"$(nproc)" 2>&1 | tail -20
	@echo "[desktop] pronto"

# ---------------------------------------------------------------------------
# stubs: ci/gen_stub/ - tabelas vazias para o build app-only do CI público
#    (sem código derivado do jogo)
# ---------------------------------------------------------------------------
stubs:
	@echo "[stubs] Gerando tabelas VAZIAS de gen/ para o build app-only..."
	@python3 tools/gen_stub.py --game $(LIFT_GAME) --out ci/gen_stub/tables
	@echo "[stubs] Estas tabelas não contêm nada do jogo: são só para o"
	@echo "[stubs] linker resolver x2rw_funcs_* quando gen/ não existe."

# ---------------------------------------------------------------------------
# difftest: autoteste do lifter (execução lifted vs unicorn emulada)
# ---------------------------------------------------------------------------
difftest:
	@echo "[difftest] Autoteste: C recompilado vs x86 real (unicorn)..."
	@echo "[difftest] Requer: python3 -m pip install --break-system-packages unicorn"
	@mkdir -p analysis build/difftest
	@python3 tools/difftest.py --game $(LIFT_GAME) \
	    --exe "$(GAME_PATH)/Wolvie.exe" --count $(DIFFTEST_COUNT) \
	    2>&1 | tee analysis/difftest.log
	@echo "[difftest] resultados em analysis/difftest.log"
	@echo "[difftest] ATENÇÃO: a taxa mede só funções isoláveis; ela NÃO prova"
	@echo "[difftest] que o jogo roda. Registre o número em docs/TESTED.md."

# ---------------------------------------------------------------------------
# clean
# ---------------------------------------------------------------------------
clean:
	rm -rf gen analysis build/desktop build/difftest ci/gen_stub/gen

# ---------------------------------------------------------------------------
# help
# ---------------------------------------------------------------------------
help:
	@echo "X2: Wolverine's Revenge - Android Port (Makefile)"
	@echo "Uso: make <alvo>"
	@echo ""
	@echo "  all        - verify + gen + desktop (default)"
	@echo "  verify     - conferência de SHA-1 dos binários do jogo"
	@echo "  gen        - gerar gen/ (C a partir dos binários, local/privado)"
	@echo "  desktop    - build de depuração Linux x86-64"
	@echo "  stubs      - gerar ci/gen_stub/ (build app-only para CI público)"
	@echo "  difftest   - autoteste lifted vs unicorn"
	@echo "  clean      - limpar gen/ analysis/ build/"
	@echo ""
	@echo "Ambiente:"
	@echo "  LIFT_GAME=$(LIFT_GAME)"
	@echo "  GAME_PATH=$(GAME_PATH)"
	@echo "  LB_GEN_JOBS=$(LB_GEN_JOBS)  (runner GitHub: 8GB RAM)"
