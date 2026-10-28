# CMake do código nativo do APK: produz libx2wr.so (arm64-v8a).
#
# POR QUE AS FONTES DO RUNTIME E DO gen/ SÃO GLOBAIS:
# o código lifted (gen/) é publicado como um diretório com N arquivos .c
# numerados (Wolvie_part000.c ...), cuja contagem muda a cada geração. Uma
# lista fixa de fontes quebraria o build toda vez que o lifter mudasse a
# partição. O glob com CONFIGURE_DEPENDS mantém o build incremental correto
# (o CMake re-executa quando um arquivo aparece/some).
#
# POR QUE EXISTE O CAMINHO "app-only":
# o repositório é público e o gen/ é DERIVADO DO BINÁRIO DO JOGO — ele não
# pode ser versionado aqui. Sem gen/, o build ainda precisa produzir um APK
# instalável (o launcher valida a pasta do jogo e avisa que falta o código).
# Nesse caso compilamos `ci_stub/gen_stub.c` e NÃO definimos X2WR_HAVE_GEN,
# o que faz o código de entrada retornar 2 em vez de chamar x2rw_call().
cmake_minimum_required(VERSION 3.22.1)
project(x2wr C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Diretórios de entrada (todos sobrescrevíveis pela linha de comando)
# ---------------------------------------------------------------------------
# ATENÇÃO À GEOMETRIA: este CMakeLists está em
#     <repo>/android/app/src/main/cpp/
# e o runtime/ e o gen/ vivem na RAIZ do repositório. A raiz está CINCO
# níveis acima daqui (cpp -> main -> src -> app -> android -> repo).
#
# O Gradle SEMPRE passa X2WR_GEN_DIR absoluto (ver app/build.gradle.kts), de
# modo que esta geometria só importa para quem invoca o CMake à mão. Ainda
# assim ela é validada abaixo: um default errado tem que falhar com mensagem
# clara, e não virar um build "app-only" silencioso que só falha no aparelho.
set(X2WR_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../..")

set(X2WR_GEN_DIR "${X2WR_REPO_ROOT}/gen"
    CACHE PATH "Diretório do código lifted (gen/*.c)")

set(X2WR_RUNTIME_DIR "${X2WR_REPO_ROOT}/runtime"
    CACHE PATH "Diretório do runtime (x2rw_runtime.c, x2rw_simd.c)")

# Número de partições do código lifted. Hoje é apenas informativo: o CI
# controla o paralelismo real (LB_GEN_JOBS) e o lifter decide as partições.
set(X2WR_GEN_JOBS "4" CACHE STRING "Partições do código lifted (informativo)")

message(STATUS "X2WR_GEN_DIR     = ${X2WR_GEN_DIR}")
message(STATUS "X2WR_RUNTIME_DIR = ${X2WR_RUNTIME_DIR}")
message(STATUS "X2WR_GEN_JOBS    = ${X2WR_GEN_JOBS}")

# ---------------------------------------------------------------------------
# Runtime do port (C puro, sem STL)
# ---------------------------------------------------------------------------
file(GLOB X2WR_RUNTIME_SOURCES CONFIGURE_DEPENDS
    "${X2WR_RUNTIME_DIR}/*.c")

# Shims Win32. Podem não existir ainda (é o trabalho do outro engenheiro):
# o glob de um diretório ausente simplesmente devolve lista vazia.
file(GLOB X2WR_SHIM_SOURCES CONFIGURE_DEPENDS
    "${X2WR_RUNTIME_DIR}/shims/*.c")

if(NOT X2WR_RUNTIME_SOURCES)
    message(FATAL_ERROR
        "Nenhum .c encontrado em X2WR_RUNTIME_DIR=${X2WR_RUNTIME_DIR}. "
        "Passe -DX2WR_RUNTIME_DIR=<repo>/runtime (o runtime é a base do port).")
endif()

list(LENGTH X2WR_SHIM_SOURCES X2WR_SHIM_COUNT)
if(X2WR_SHIM_COUNT EQUAL 0)
    message(STATUS "Shims Win32: nenhum .c em ${X2WR_RUNTIME_DIR}/shims "
                   "(esperado nesta fase — imports ainda não resolvem).")
endif()

# ---------------------------------------------------------------------------
# Código lifted do jogo (gen/) — opcional
# ---------------------------------------------------------------------------
file(GLOB X2WR_GEN_SOURCES CONFIGURE_DEPENDS
    "${X2WR_GEN_DIR}/*.c")

list(LENGTH X2WR_GEN_SOURCES X2WR_GEN_COUNT)
if(X2WR_GEN_COUNT EQUAL 0)
    # Build app-only. NÃO é erro: é o caminho suportado do repositório
    # público. O stub é um símbolo fraco só para o alvo ter ao menos uma
    # unidade de tradução própria.
    if(NOT EXISTS "${X2WR_GEN_DIR}")
        message(STATUS
            "gen/: diretório ${X2WR_GEN_DIR} não existe — BUILD APP-ONLY "
            "(sem código lifted do jogo; a entrada retornará 2).")
    else()
        message(STATUS
            "gen/: nenhum .c em ${X2WR_GEN_DIR} — BUILD APP-ONLY "
            "(sem código lifted do jogo; a entrada retornará 2).")
    endif()
    # Fontes e headers do stub público. Ficam em ci/gen_stub/ na RAIZ do
    # repositório (não aqui em cpp/) para que `make stubs` e o CI gerem
    # sempre o mesmo conjunto.
    #
    # POR QUE OS HEADERS DE IMPORTS SÃO OBRIGATÓRIOS AQUI:
    # runtime/shims/x2rw_imports.c faz `#include "Wolvie_imports.h"` e
    # `#include "binkw32_imports.h"`. Sem eles o build app-only não compila —
    # os arquivos de tabela sozinhos não bastam. Os headers de stub definem
    # as mesmas macros com valor 0, de modo que os laços de patch de IAT não
    # executam nenhuma iteração.
    set(X2WR_STUB_ROOT "${X2WR_REPO_ROOT}/ci/gen_stub")

    file(GLOB X2WR_STUB_TABLES CONFIGURE_DEPENDS
        "${X2WR_STUB_ROOT}/tables/*.c")
    if(NOT X2WR_STUB_TABLES)
        message(FATAL_ERROR
            "Build app-only pede as tabelas de stub em "
            "${X2WR_STUB_ROOT}/tables/*.c e não achei nenhuma. "
            "Rode: python3 tools/gen_stub.py --game x2wr --out ci/gen_stub/tables")
    endif()

    foreach(_h Wolvie_imports.h binkw32_imports.h)
        if(NOT EXISTS "${X2WR_STUB_ROOT}/imports/${_h}")
            message(FATAL_ERROR
                "Falta ${X2WR_STUB_ROOT}/imports/${_h}. "
                "Rode: python3 tools/gen_stub.py --game x2wr --out ci/gen_stub/tables")
        endif()
    endforeach()

    message(STATUS "app-only: tabelas de stub = ${X2WR_STUB_TABLES}")
    set(X2WR_GEN_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/ci_stub/gen_stub.c"
        ${X2WR_STUB_TABLES})
    set(X2WR_HAVE_GEN OFF)
else()
    message(STATUS "gen/: ${X2WR_GEN_COUNT} arquivos .c — BUILD FULL "
                   "(código lifted incluído)")
    set(X2WR_HAVE_GEN ON)
endif()

# ---------------------------------------------------------------------------
# Alvo: libx2wr.so
# ---------------------------------------------------------------------------
add_library(x2wr SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/x2rw_android_main.c
    ${X2WR_RUNTIME_SOURCES}
    ${X2WR_SHIM_SOURCES}
    ${X2WR_GEN_SOURCES})

# GEN_DIR em build full (headers reais de imports e de funções);
# STUB_ROOT/imports em build app-only. Nunca os dois: incluir os dois
# deixaria a ordem de resolução decidir em silêncio qual cabeçalho vence.
if(X2WR_HAVE_GEN)
    target_include_directories(x2wr PRIVATE
        "${X2WR_RUNTIME_DIR}"
        "${X2WR_GEN_DIR}")
else()
    target_include_directories(x2wr PRIVATE
        "${X2WR_RUNTIME_DIR}"
        "${X2WR_STUB_ROOT}/imports")
endif()

# Flags herdadas do port: o código lifted assume semântica de inteiro do
# x86 (sem UB com overflow) e aponta ponteiros para dentro do bloco de
# memória do guest (aliasing agressivo). Sem -fwrapv/-fno-strict-aliasing o
# clang otimizaria com premissas que o código transladado viola de propósito.
#
# OBS: nenhuma flag de STL — runtime e código lifted são C puro. O STL
# default do NDK (c++_shared) não é linkado de propósito: linká-lo inflaria
# o APK sem uso.
target_compile_options(x2wr PRIVATE
    -O2
    -fno-strict-aliasing
    -fwrapv
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -DX2WR_ANDROID=1)

if(X2WR_HAVE_GEN)
    # É ESTA definição que habilita x2rw_call()/x2rw_resolve() no código de
    # entrada (x2rw_android_main.c). O desktop define o mesmo macro a partir
    # do próprio glob em desktop/CMakeLists.txt.
    target_compile_definitions(x2wr PRIVATE X2WR_HAVE_GEN=1)
endif()

target_link_libraries(x2wr
    android
    log
    m)
