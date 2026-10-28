# ci/gen_stub/ - Tabelas vazias para o build "app-only"

Este diretório contém **apenas estruturas de dados vazias/stub** — nada de código do jogo, nada de `gen/` real. Serve para que o workflow do GitHub Actions (`.github/workflows/build.yml`) consiga **compilar o APK "app-only"** (launcher + runtime + SDL2) **sem qualquer código derivado do jogo**, mantendo o repositório público limpo.

## Como funciona

### Build app-only (público)
Quando `GEN_REPO` **não** está definido no workflow:
```yaml
- name: Build app-only
  run: ./gradlew assembleRelease -DLB_GEN_JOBS=$(LB_GEN_JOBS)
```
O compilador encontra `ci/gen_stub/gen/*.c` — módulos C vazios/placeholder que **nunca são ligados** (são apenas para que o linker não reclame). O APK gerado contém:
- O launcher (validação de pasta/SHA-1)
- Os shims Win32, SDL2, camada gráfica, áudio, input
- Stubs para APIs do jogo (ex: `Wolvie_main_stub`) que exibem "jogo não encontrado"

✅ Seguro de publicar — zero bytes derivados do jogo.

### Build completo (privado)
Quando `GEN_REPO` **está** definido:
```yaml
- name: Baixar gen/ privado
  run: |
    wget -O gen.tar.zst "https://github.com/deivid22srk/X2WR_gen/releases/download/${{ inputs.gen_tag }}/gen.tar.zst"
    mkdir -p gen && tar -xf gen.tar.zst -C gen
```
O APK é compilado contra o `gen/` real (C gerado dos binários do jogo) e publicado como **release asset privada** em `deivid22srk/X2WR_gen`.

## Conteúdo deste diretório

```
ci/gen_stub/
├── README.md            # Este arquivo
├── gen/                 # Placeholder C (gerado por make stubs)
│   ├── stub_Wolvie_main.c
│   ├── stub_Wolvie_init.c
│   └── stub_headers.c
└── stub_list.txt        # Lista de stubs esperados pelo Makefile/CI
```

**Nota:** os arquivos abaixo são *placeholder* — não representam comportamento algum do jogo. Para um build funcional, o gen/ real vem do repositório privado `X2WR_gen`.
