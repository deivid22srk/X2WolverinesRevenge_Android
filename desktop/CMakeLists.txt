# CMake do build de DESKTOP (Linux x86-64).
#
# Para que serve: validar o lifting e os shims ANTES de tocar no Android.
# Compilar o APK só acontece no CI (regra do projeto), então este é o único
# caminho de depuração rápida do código gerado.
#
# Diferenças em relação ao CMake do Android (android/app/src/main/cpp):
#   * não há NDK/STL — usa o compilador do host;
#   * os caminhos padrão são relativos a `desktop/` (../../../ do Android
#     seria errado aqui);
#   * não existe X2WR_ANDROID e não há liblog/libandroid.
cmake_minimum_required(VERSION 3.22.1)
project(x2wr C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    # Debug por padrão: o objetivo deste alvo é depurar o lifting.
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Tipo de build" FORCE)
endif()

# ---------------------------------------------------------------------------
# Diretórios de entrada (sobrescrevíveis)
# ---------------------------------------------------------------------------
set(X2WR_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../gen"
    CACHE PATH "Diretório do código lifted (gen/*.c)")

set(X2WR_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../runtime"
    CACHE PATH "Diretório do runtime (x2rw_runtime.c, x2rw_simd.c)")

set(X2WR_ANDROID_CPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../android/app/src/main/cpp"
    CACHE PATH "Diretório do loader compartilhado (x2rw_loader.inc, x2rw_host.h)")

message(STATUS "X2WR_GEN_DIR     = ${X2WR_GEN_DIR}")
message(STATUS "X2WR_RUNTIME_DIR = ${X2WR_RUNTIME_DIR}")

# ---------------------------------------------------------------------------
# Runtime + shims
# ---------------------------------------------------------------------------
file(GLOB X2WR_RUNTIME_SOURCES CONFIGURE_DEPENDS
    "${X2WR_RUNTIME_DIR}/*.c")

file(GLOB X2WR_SHIM_SOURCES CONFIGURE_DEPENDS
    "${X2WR_RUNTIME_DIR}/shims/*.c")

if(NOT X2WR_RUNTIME_SOURCES)
    message(FATAL_ERROR
        "Nenhum .c em X2WR_RUNTIME_DIR=${X2WR_RUNTIME_DIR}. "
        "Use -DX2WR_RUNTIME_DIR=... apontando para o runtime/ do repositório.")
endif()

list(LENGTH X2WR_SHIM_SOURCES X2WR_SHIM_COUNT)
if(X2WR_SHIM_COUNT EQUAL 0)
    message(STATUS "Shims Win32: nenhum .c em ${X2WR_RUNTIME_DIR}/shims "
                   "(o runtime traz um x2rw_import_dispatch FRACO que "
                   "só registra o fault).")
endif()

# ---------------------------------------------------------------------------
# Código lifted (gen/)
# ---------------------------------------------------------------------------
file(GLOB X2WR_GEN_SOURCES CONFIGURE_DEPENDS
    "${X2WR_GEN_DIR}/*.c")

list(LENGTH X2WR_GEN_SOURCES X2WR_GEN_COUNT)
if(X2WR_GEN_COUNT EQUAL 0)
    message(STATUS
        "gen/: nenhum .c em ${X2WR_GEN_DIR} — build SEM código lifted. "
        "O executável x2wr_run compila e avisa, mas não executa o jogo.")
    set(X2WR_HAVE_GEN OFF)
else()
    message(STATUS "gen/: ${X2WR_GEN_COUNT} arquivos .c — build COM código lifted")
    set(X2WR_HAVE_GEN ON)
endif()

# ---------------------------------------------------------------------------
# Alvo: biblioteca com runtime + shims + (opcional) gen
# ---------------------------------------------------------------------------
add_library(x2wr-desktop STATIC
    ${X2WR_RUNTIME_SOURCES}
    ${X2WR_SHIM_SOURCES}
    ${X2WR_GEN_SOURCES})

target_include_directories(x2wr-desktop PUBLIC
    "${X2WR_RUNTIME_DIR}"
    "${X2WR_GEN_DIR}")

# As mesmas flags semânticas do Android: o código transladado depende da
# aritmética de inteiro do x86 (sem UB por overflow) e faz aliasing de
# propósito dentro do bloco de memória do guest.
#
# _GNU_SOURCE: o código lifted (e futuros shims POSIX) costuma usar
# M_PI/strdup/memmem, que em glibc só aparecem com este macro.
target_compile_definitions(x2wr-desktop PRIVATE _GNU_SOURCE=1)

target_compile_options(x2wr-desktop PRIVATE
    -O2
    -fno-strict-aliasing
    -fwrapv
    -Wno-unused-variable
    -Wno-unused-but-set-variable)

if(X2WR_HAVE_GEN)
    target_compile_definitions(x2wr-desktop PUBLIC X2WR_HAVE_GEN=1)
endif()

target_link_libraries(x2wr-desktop PUBLIC m)

# ---------------------------------------------------------------------------
# Alvo: executável de depuração
# ---------------------------------------------------------------------------
add_executable(x2wr_run
    "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
    )

target_include_directories(x2wr_run PRIVATE
    "${X2WR_RUNTIME_DIR}"
    "${X2WR_GEN_DIR}"
    "${X2WR_ANDROID_CPP_DIR}")

target_link_libraries(x2wr_run PRIVATE x2wr-desktop m pthread)

# Sem --undefined: o runtime sempre define x2rw_import_dispatch (fraco), e
# a biblioteca estática é ligada por inteiro neste executável de depuração.
