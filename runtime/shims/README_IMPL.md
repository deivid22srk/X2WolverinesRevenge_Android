# Implementação dos handlers de shim — contrato

Este arquivo é o contrato para quem implementa os handlers de API Win32
da camada de shims. Ele substitui a lista do header por um conjunto de
regras verificáveis.

## 1. Onde as coisas estão

| Arquivo | Conteúdo |
|---|---|
| `runtime/shims/x2rw_shims.h` | **A lista mestra.** As macros `X2RW_SHIMS_KERNEL32(X)`, `X2RW_SHIMS_USER32(X)` etc. listam cada import como `X("DLL.dll", "Nome", handler, nargs, NIVEL)`. As declarações de todos os handlers são geradas dela. **Não edite essa lista** para adicionar handlers — ela é gerada a partir da superfície de imports medida nos binários. |
| `runtime/shims/x2rw_imports.c` | O despachante (`x2rw_import_dispatch`, `x2rw_call_import`), o registro `g_shims[]`, a ponte de COM sintético. **Já escrito — não mexa.** |
| `runtime/shims/x2rw_core.c` | Estado global, last error, strings do guest, CP-1252, caminhos Win32→POSIX, hooks padrão. **Já escrito — não mexa.** |
| `runtime/shims/x2rw_<dll>.c` | **O que você vai escrever.** Um arquivo por DLL. |

## 2. A assinatura do handler

```c
uint32_t nome_do_handler(x2rw_ctx *c, uint32_t *args, int nargs);
```

- `args[i]` é o i-ésimo argumento (32 bits), já lido da pilha do guest.
- `nargs` é a quantidade declarada na lista mestra. **Não confie em
  `nargs` para validar**: use-o, mas trate `args[i]` com `i < nargs`.
- O retorno vai para `c->r.eax`.
- **A API é `stdcall`**: quem limpa os argumentos é o handler. O
  despachante (`invoke_from_stack` em `x2rw_imports.c`) já faz
  `c->r.esp += 4 * nargs` depois de chamar. **NÃO altere `c->r.esp`.**

## 3. Acesso à memória do guest

A memória do jogo vive num espaço de endereços de 32 bits próprio, paginado.
**Nunca use ponteiros do host para dados do guest.**

```c
uint8_t  v8  = x2rw_read8 (c, va);
uint32_t v32 = x2rw_read32(c, va);
x2rw_write32(c, va, valor);
x2rw_mem_read(va, buf, n);   /* bloco */
x2rw_mem_write(va, buf, n);  /* bloco */
```

Strings: o ponteiro é um VA do guest.

```c
uint32_t len = x2rw_guest_strlen(c, va);
/* copia string do HOST para o guest: */
x2rw_guest_strcpy(c, dst, "texto", cap);
/* aloca no heap do guest e copia: */
uint32_t p = x2rw_guest_alloc_str(c, "texto");
```

Para ler uma string do guest para um buffer do host:

```c
char buf[256];
uint32_t n = x2rw_guest_strlen(c, va);
if (n >= sizeof(buf)) n = sizeof(buf) - 1;
x2rw_mem_read(va, buf, n);
buf[n] = 0;
```

## 4. Last error

```c
X2RW_OK();                    /* GetLastError() passa a devolver 0      */
X2RW_FAIL(c, X2RW_ERROR_FILE_NOT_FOUND);  /* devolve 0 e seta o erro */
```

Os códigos estão em `x2rw_shims.h` (`X2RW_ERROR_*`).

## 5. A REGRA DE OURO — nunca falhe em silêncio

> Um handler **não** devolve um valor inventado sem registrar o que fez.

Handler que não sabe implementar:

```c
uint32_t k32_Alguem(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c; (void)args; (void)nargs;
    X2RW_FAIL(c, X2RW_ERROR_CALL_NOT_IMPLEMENTED);
    return 0u;
}
```

Se a API é fundamental (ex.: `CreateFileA`), implemente de verdade.
Se é cosmética (ex.: `SetWindowTextA`), o stub acima está bom — o jogo
sobrevive. Mas **deixe um comentário dizendo por quê**.

Se uma API **precisa** devolver algo não-zero para o jogo seguir (ex.:
um handle de arquivo), devolva um handle **sintético** gerenciado por
um contador do próprio módulo, e registre em log. Nunca devolva um
ponteiro inventado para memória do host.

## 6. Níveis (a coluna `NIVEL` da lista mestra)

| Nível | Significado |
|---|---|
| `REAL` | Semântica correta dentro do modelo de memória do guest. |
| `PARCIAL` | Funciona para o caminho principal, mas não para todos os casos. |
| `STUB` | Valor sintético benigno, logado. |

**A coluna `NIVEL` já está preenchida na lista mestra. Não mude.** O
seu código deve ser coerente com o nível declarado: se o nível é `REAL`,
implemente de verdade; se é `STUB`, um stub honesto serve.

## 7. Regras de compilação

- C11 puro. **Sem STL, sem headers do Windows.**
- `#include "x2rw_runtime.h"` e `#include "x2rw_shims.h"`.
- Sem `malloc` sem free; sem estado global que não seja justified.
- Sem `printf` — use `x2rw_log(...)`.
- Nomes de arquivo `x2rw_<dll>.c` em minúsculas.
- **Não modifique `x2rw_shims.h`, `x2rw_imports.c` nem `x2rw_core.c`.**
  Se precisar de um helper novo, escreva um `static` no seu arquivo.

## 8. Como verificar que você terminou

Depois de escrever o arquivo, rode:

```bash
cd /root/X-Men
python3 - << 'EOF'
import re
h=open('runtime/shims/x2rw_shims.h',encoding='utf-8').read()
c=open('runtime/shims/x2rw_core.c',encoding='utf-8').read()
import glob
mine=''.join(open(f,encoding='utf-8').read() for f in glob.glob('runtime/shims/x2rw_*.c'))
handlers=set()
for m in re.finditer(r'#define (X2RW_SHIMS_\w+)\(X\)(.*?)(?=\n#define |\n#endif)', h, re.S):
    for d,n,f in re.findall(r'X\(\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*([A-Za-z_]\w*)', m.group(2)):
        handlers.add(f)
defs=set(re.findall(r'^\s*(?:static\s+)?uint32_t\s+([A-Za-z_]\w*)\s*\(', mine, re.M))
miss=sorted(handlers-defs)
print("handlers referenciados:", len(handlers))
print("sem definicao:", len(miss))
for x in miss: print("   ", x)
EOF
```

A lista de "sem definição" deve conter **apenas** os handlers dos outros
arquivos (os que você não implementou). Se contiver algum handler seu,
o seu arquivo está errado.
