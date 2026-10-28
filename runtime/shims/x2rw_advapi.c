/* X2: Wolverine's Revenge - port Android
 * ============================================================================
 * x2rw_advapi.c — ADVAPI32.dll: o registro (RegOpenKeyA/RegOpenKeyExA/
 *                 RegQueryValueExA/RegCloseKey).
 *
 * O que o jogo faz com o registro
 * -------------------------------
 * Há UM único uso de registro em Wolvie.exe. EVIDÊNCIA (medida no binário
 * fornecido, não suposta):
 *
 *   1. a .rdata contém "Software\Activision\Wolverine's Revenge" em
 *      0x006ae2c4 e "Language" em 0x006ae2b8;
 *   2. logo acima ficam as mensagens de erro e a tabela de tags:
 *        0x006ae224 "Language value not found: defaulting to English"
 *        0x006ae258 "Language value unknown: defaulting to English"
 *        0x006ae2a0 "Language Value: %s"
 *        0x006ae28c "SP", 0x006ae290 "GM", 0x006ae294 "FR",
 *        0x006ae298 "UK", 0x006ae29c "US"   (tags de 4 bytes, passo 4)
 *   3. a função resolvedora começa em 0x005579c0 e faz, nesta ordem:
 *        0x005579e3  RegOpenKeyExA(HKLM, chave, 0, 0x000F003F
 *                                  (KEY_ALL_ACCESS), &hKey)
 *        0x00557a07  RegQueryValueExA(hKey, "Language", NULL,
 *                                     lpType = NULL, lpData = buf,
 *                                     lpcbData = &cb)  com cb = 16
 *        0x00557a2e  compara o buffer com "US"/"UK"/"FR"/"GM"/"SP"
 *        0x00557b35  RegCloseKey(hKey)
 *   4. o valor de retorno da API é TESTADO (`test eax,eax`), não só o
 *      last error — por isso este módulo devolve o código Win32 de fato
 *      (ERROR_*) em vez de 0/1. Ver X2RW_FAIL mais abaixo.
 *
 * O QUE ESTE ARQUIVO **NÃO** FAZ (de propósito)
 * ---------------------------------------------
 * NÃO devolve nenhum valor para "Language". Qualquer string que
 * inventássemos ("US", "ENU", ...) seria um chute sobre um dado que não
 * foi medido — o run real que gerou o d3d.log desta instalação registrou
 * "Language Value: UK", ou seja, o valor depende de como o jogo foi
 * instalado. A política da camada é explícita: um handler nunca devolve um
 * valor inventado em silêncio.
 *
 * Por isso RegQueryValueExA(..., "Language", ...) devolve
 * ERROR_FILE_NOT_FOUND, que é o que o Windows devolve quando o valor não
 * existe — e um dos caminhos que o jogo trata explicitamente.
 *
 * O que o jogo faz nesse caminho (0x00557a0f `jne 0x557b19`) foi lido no
 * binário, e é o ponto que torna esta escolha segura: ele loga "Language
 * value not found", cai em 0x00557b26 `mov dword ptr [0x6b4f58], 1` e
 * devolve esi = 0 — EXATAMENTE o mesmo estado (idioma 0 = inglês, flag 1)
 * que o caminho de sucesso de "US" (0x00557a3e `je 0x557b26`). Ou seja:
 * para esta build, "valor ausente" e "US" produzem o mesmo resultado
 * interno. Não é um palpite com efeito colateral; é o mesmo destino.
 *
 * O registro virtual existe para o que o jogo exige de verdade: um
 * HKLM\Software\Activision\... que ABRE e FECHA. RegOpenKeyExA devolve
 * handle para a chave; o handle é sintético, pequeno (0x6001+) e este
 * módulo o gerencia.
 * ============================================================================
 */
#include "x2rw_runtime.h"
#include "x2rw_shims.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Handle de registro sintético                                       */
/* ------------------------------------------------------------------ */

/* Handles 0x6001..0x60FF: pequenos, fora da faixa das pseudo-handles do
 * KERNEL32 (heap, arquivo, processo), para não colidirem em nenhum log. O
 * contador é do módulo e nunca é reciclado: o jogo abre a chave uma vez
 * na inicialização e a fecha no shutdown, então o consumo é 1. */
#define X2RW_REG_HANDLE_BASE  0x6000u
#define X2RW_REG_HANDLE_MAX   0x60FFu
#define X2RW_REG_HANDLE_COUNT (X2RW_REG_HANDLE_MAX - X2RW_REG_HANDLE_BASE)

/* ------------------------------------------------------------------ */
/* O "registro virtual": só as chaves que o jogo procura              */
/* ------------------------------------------------------------------ */
/* A tabela é de CHAVES, não de valores: nenhuma chave tem valor
 * cadastrado (ver o comentário do topo). Isso é deliberado — o tipo
 * `value` fica NULL e RegQueryValueExA responde FILE_NOT_FOUND para
 * qualquer nome, que é o caminho "defaulting to English" do jogo. */

typedef struct {
    uint32_t    handle;             /* 0 enquanto não aberta             */
    const char *key;                /* caminho completo, sem HKEY_*      */
    /* Reservado para o dia em que um valor medido aparecer: (nome, tipo,
     * dados, tamanho). Hoje sempre NULL/0 — não inventamos valores. */
    const char *value_name;
    uint32_t    value_type;
    const void *value_data;
    uint32_t    value_size;
} x2rw_reg_key;

static x2rw_reg_key g_reg[] = {
    /* HKLM\Software\Activision\Wolverine's Revenge — a única chave que o
     * jogo abre (evidência: string em 0x006ae2c8). O subkey chega como
     * "Software\Activision\Wolverine's Revenge" com o hKey = HKEY_LOCAL_MACHINE
     * (0x80000002). */
    { 0u, "Software\\Activision\\Wolverine's Revenge", NULL, 0u, NULL, 0u }
};

#define X2RW_N_REG_KEYS ((int)(sizeof(g_reg) / sizeof(g_reg[0])))

/* HKEY_LOCAL_MACHINE, do WinReg.h. O jogo abre a chave na raiz HKLM. */
#define X2RW_HKEY_LOCAL_MACHINE 0x80000002u

/* Próximo handle livre. Estático e monotônico (ver comentário acima). */
static uint32_t g_reg_next = X2RW_REG_HANDLE_BASE + 1u;

/* Compara o subkey ignorando maiúsculas e as duas formas de separador
 * (o jogo pode usar '/'). Devolve 1 se casar. */
static int reg_key_match(const char *subkey, const char *entry) {
    if (!subkey) return 0;
    while (*subkey && *entry) {
        char a = (*subkey == '/') ? '\\' : *subkey;
        char b = (*entry  == '/') ? '\\' : *entry;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
        subkey++; entry++;
    }
    return *subkey == '\0' && *entry == '\0';
}

/* Lê o subkey do guest para um buffer do host. 512 bytes cobrem com
 * folga o caminho mais longo que o jogo usa (37 bytes). */
static int reg_read_subkey(x2rw_ctx *c, uint32_t va, char *out, size_t cap) {
    if (!va) return 0;
    uint32_t n = x2rw_guest_strlen(c, va);
    if (n >= cap) {
        x2rw_log("[advapi] subkey do guest com %u bytes (teto %u)",
                 n, (unsigned)cap - 1u);
        return 0;
    }
    if (n) x2rw_mem_read(va, out, n);
    out[n] = '\0';
    return 1;
}

/* Procura o handle devolvido por RegOpenKey*, para o RegQueryValueExA e o
 * RegCloseKey saberem qual chave é. Devolve índice em g_reg ou -1. */
static int reg_find_handle(uint32_t h) {
    for (int i = 0; i < X2RW_N_REG_KEYS; i++)
        if (g_reg[i].handle == h && h != 0u) return i;
    return -1;
}

/* ------------------------------------------------------------------ */
/* RegOpenKeyExA / RegOpenKeyA                                        */
/* ------------------------------------------------------------------ */

/* Abre a chave: se o caminho casar com uma entrada da tabela, devolve um
 * handle sintético e escreve-o em phkResult. Devolve ERROR_SUCCESS (0).
 * Se o caminho não for conhecido, ERROR_FILE_NOT_FOUND (2) — nunca um
 * handle para uma chave que não existe. */
uint32_t adv_RegOpenKeyExA(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t hkey   = (nargs > 0) ? args[0] : 0u;
    uint32_t subkey = (nargs > 1) ? args[1] : 0u;
    uint32_t opts   = (nargs > 2) ? args[2] : 0u;
    uint32_t sam    = (nargs > 3) ? args[3] : 0u;
    uint32_t out    = (nargs > 4) ? args[4] : 0u;

    if (!out) {
        x2rw_last_error_set(X2RW_ERROR_INVALID_PARAMETER);
        return X2RW_ERROR_INVALID_PARAMETER;
    }

    char path[512];
    if (!reg_read_subkey(c, subkey, path, sizeof(path))) {
        x2rw_last_error_set(X2RW_ERROR_INVALID_PARAMETER);
        return X2RW_ERROR_INVALID_PARAMETER;
    }

    /* HKLM é a única raiz com chaves neste port. HKEY_CURRENT_USER/
     * HKEY_CLASSES_ROOT apareceriam aqui se alguma string do binário
     * apontasse para elas — nenhuma aponta. */
    if (hkey != X2RW_HKEY_LOCAL_MACHINE) {
        x2rw_log("[advapi] RegOpenKeyExA: hKey %08x não é HKLM (o jogo "
                 "só usa HKLM neste port)", hkey);
        x2rw_last_error_set(X2RW_ERROR_FILE_NOT_FOUND);
        return X2RW_ERROR_FILE_NOT_FOUND;
    }

    for (int i = 0; i < X2RW_N_REG_KEYS; i++) {
        if (!reg_key_match(path, g_reg[i].key)) continue;

        if (g_reg[i].handle == 0u) {
            if (g_reg_next > X2RW_REG_HANDLE_MAX) {
                x2rw_log("[advapi] handles de registro esgotados");
                x2rw_last_error_set(X2RW_ERROR_OUTOFMEMORY);
                return X2RW_ERROR_OUTOFMEMORY;
            }
            g_reg[i].handle = g_reg_next++;
        }
        x2rw_write32(c, out, g_reg[i].handle);
        x2rw_log("[advapi] RegOpenKeyExA(\"%s\") -> handle %08x "
                 "(samDesired=%08x, options=%08x)", path, g_reg[i].handle,
                 sam, opts);
        X2RW_OK();
        return X2RW_ERROR_SUCCESS;
    }

    x2rw_log("[advapi] RegOpenKeyExA(\"%s\"): chave não está no registro "
             "virtual do port", path);
    x2rw_last_error_set(X2RW_ERROR_FILE_NOT_FOUND);
    return X2RW_ERROR_FILE_NOT_FOUND;
}

/* RegOpenKeyA(hKey, lpSubKey, phkResult): a variante sem samDesired/
 * options. Mesma semântica do Ex — inclusive o handle sintético. */
uint32_t adv_RegOpenKeyA(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t hkey   = (nargs > 0) ? args[0] : 0u;
    uint32_t subkey = (nargs > 1) ? args[1] : 0u;
    uint32_t out    = (nargs > 2) ? args[2] : 0u;

    if (!out) {
        x2rw_last_error_set(X2RW_ERROR_INVALID_PARAMETER);
        return X2RW_ERROR_INVALID_PARAMETER;
    }

    char path[512];
    if (!reg_read_subkey(c, subkey, path, sizeof(path))) {
        x2rw_last_error_set(X2RW_ERROR_INVALID_PARAMETER);
        return X2RW_ERROR_INVALID_PARAMETER;
    }
    if (hkey != X2RW_HKEY_LOCAL_MACHINE) {
        x2rw_log("[advapi] RegOpenKeyA: hKey %08x não é HKLM", hkey);
        x2rw_last_error_set(X2RW_ERROR_FILE_NOT_FOUND);
        return X2RW_ERROR_FILE_NOT_FOUND;
    }

    for (int i = 0; i < X2RW_N_REG_KEYS; i++) {
        if (!reg_key_match(path, g_reg[i].key)) continue;
        if (g_reg[i].handle == 0u) {
            if (g_reg_next > X2RW_REG_HANDLE_MAX) {
                x2rw_log("[advapi] handles de registro esgotados");
                x2rw_last_error_set(X2RW_ERROR_OUTOFMEMORY);
                return X2RW_ERROR_OUTOFMEMORY;
            }
            g_reg[i].handle = g_reg_next++;
        }
        x2rw_write32(c, out, g_reg[i].handle);
        x2rw_log("[advapi] RegOpenKeyA(\"%s\") -> handle %08x",
                 path, g_reg[i].handle);
        X2RW_OK();
        return X2RW_ERROR_SUCCESS;
    }

    x2rw_log("[advapi] RegOpenKeyA(\"%s\"): chave desconhecida", path);
    x2rw_last_error_set(X2RW_ERROR_FILE_NOT_FOUND);
    return X2RW_ERROR_FILE_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* RegQueryValueExA                                                   */
/* ------------------------------------------------------------------ */

/* RegQueryValueExA(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData)
 *
 * Semântica (a do Win32, implementada de verdade):
 *   - handle inválido            -> ERROR_INVALID_HANDLE (6);
 *   - valor inexistente na chave -> ERROR_FILE_NOT_FOUND (2);
 *   - buffer pequeno             -> ERROR_INSUFFICIENT_BUFFER (122), com o
 *                                   tamanho necessário escrito em lpcbData;
 *   - sucesso                    -> ERROR_SUCCESS (0), com o tipo em
 *                                   lpType e o tamanho em lpcbData.
 *
 * Duas restrições medidas no call site (0x00557a07) que qualquer
 * implementação futura PRECISA respeitar:
 *   - lpType vem NULL (o jogo não pede o tipo) — escrever em *lpType sem
 *     checar seria escrita em VA 0;
 *   - lpData aponta para um buffer de 16 BYTES (lpcbData = 0x10). Devolver
 *     uma string maior sem passar por ERROR_INSUFFICIENT_BUFFER estouraria
 *     esse buffer do jogo.
 *
 * No registro virtual atual nenhuma chave tem valor cadastrado, então a
 * resposta é sempre FILE_NOT_FOUND. Isso é a resposta HONESTA: o nome do
 * valor e o dado não foram medidos no binário (só a chave e o nome
 * "Language" foram), e o jogo trata esse erro caindo para o inglês.
 * Quando um valor medido for acrescentado à tabela, o caminho de
 * sucesso/tipo/cópia abaixo já está pronto e passa a valer. */
uint32_t adv_RegQueryValueExA(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t hkey    = (nargs > 0) ? args[0] : 0u;
    uint32_t name_va = (nargs > 1) ? args[1] : 0u;
    uint32_t reserved= (nargs > 2) ? args[2] : 0u;
    uint32_t ptype   = (nargs > 3) ? args[3] : 0u;
    uint32_t pdata   = (nargs > 4) ? args[4] : 0u;
    uint32_t pcb     = (nargs > 5) ? args[5] : 0u;
    (void)reserved;

    if (!pcb) {
        /* Sem lpcbData não há como informar o tamanho: o Windows também
         * falha aqui. */
        x2rw_last_error_set(X2RW_ERROR_INVALID_PARAMETER);
        return X2RW_ERROR_INVALID_PARAMETER;
    }

    int idx = reg_find_handle(hkey);
    if (idx < 0) {
        x2rw_log("[advapi] RegQueryValueExA: handle %08x desconhecido", hkey);
        x2rw_last_error_set(X2RW_ERROR_INVALID_HANDLE);
        return X2RW_ERROR_INVALID_HANDLE;
    }

    char name[256];
    if (!reg_read_subkey(c, name_va, name, sizeof(name))) {
        x2rw_log("[advapi] RegQueryValueExA: lpValueName inválido (%08x)",
                 name_va);
        x2rw_last_error_set(X2RW_ERROR_INVALID_PARAMETER);
        return X2RW_ERROR_INVALID_PARAMETER;
    }

    const x2rw_reg_key *k = &g_reg[idx];
    if (k->value_name && strcmp(name, k->value_name) == 0 && k->value_data) {
        /* Caminho de sucesso — hoje inalcançável (nenhuma entrada tem
         * value_data), pronto para quando um valor medido aparecer. */
        uint32_t cb = x2rw_read32(c, pcb);
        if (ptype) x2rw_write32(c, ptype, k->value_type);
        x2rw_write32(c, pcb, k->value_size);   /* tamanho real, sempre */
        if (pdata == 0u) {
            /* Consulta de tamanho sem buffer: sucesso, nada copiado. */
            X2RW_OK();
            return X2RW_ERROR_SUCCESS;
        }
        if (cb < k->value_size) {
            x2rw_last_error_set(X2RW_ERROR_INSUFFICIENT_BUFFER);
            return X2RW_ERROR_INSUFFICIENT_BUFFER;
        }
        x2rw_mem_write(pdata, k->value_data, k->value_size);
        X2RW_OK();
        return X2RW_ERROR_SUCCESS;
    }

    x2rw_log("[advapi] RegQueryValueExA(\"%s\"): valor não cadastrado no "
             "registro virtual (não inventamos \"Language\") — o jogo "
             "assume o inglês, que é o idioma desta build", name);
    /* ATENÇÃO ao contrato do Win32 aqui: RegQueryValueExA devolve o
     * CÓDIGO DE ERRO (não FALSE/0) em caso de falha, e o jogo testa esse
     * valor. Por isso NÃO usamos X2RW_FAIL neste handler — o macro do
     * contrato sempre devolve 0u, o que faria esta API parecer sucesso.
     * Setamos o last error e devolvemos o código explicitamente. */
    x2rw_last_error_set(X2RW_ERROR_FILE_NOT_FOUND);
    return X2RW_ERROR_FILE_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* RegCloseKey                                                        */
/* ------------------------------------------------------------------ */

/* Libera o handle: marca a entrada como fechada (handle volta a 0 e pode
 * ser reaberta com um novo handle) e devolve ERROR_SUCCESS. Handle
 * desconhecido -> ERROR_INVALID_HANDLE. */
uint32_t adv_RegCloseKey(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    uint32_t hkey = (nargs > 0) ? args[0] : 0u;
    int idx = reg_find_handle(hkey);
    if (idx < 0) {
        x2rw_log("[advapi] RegCloseKey: handle %08x desconhecido", hkey);
        x2rw_last_error_set(X2RW_ERROR_INVALID_HANDLE);
        return X2RW_ERROR_INVALID_HANDLE;
    }
    x2rw_log("[advapi] RegCloseKey(%08x) — \"%s\"", hkey, g_reg[idx].key);
    g_reg[idx].handle = 0u;
    X2RW_OK();
    return X2RW_ERROR_SUCCESS;
}
