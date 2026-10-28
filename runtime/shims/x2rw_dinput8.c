/* X2: Wolverine's Revenge - port Android
 * ============================================================================
 * x2rw_dinput8.c — shim de INPUT: DINPUT8.dll (DirectInput 8).
 *
 * O JOGO CHAMA ASSIM
 * ------------------
 *   DirectInput8Create(hinst, 0x0800, IID_IDirectInput8A, &pDI, NULL);
 *   pDI->CreateDevice(SysKeyboard / SysMouse, &pDevice, NULL);   vtbl[3]
 *   pDevice->SetDataFormat(&c_dfDIKeyboard);                     vtbl[13]
 *   pDevice->SetCooperativeLevel(hwnd, DISCL_*);                 vtbl[14]
 *   pDevice->Acquire();                                          vtbl[7]
 *   loop: pDevice->GetDeviceState(256, buf);                     vtbl[9]
 *
 * Como não existe dinput8.dll no Android, criamos objetos COM SINTÉTICOS
 * na memória do guest (x2rw_com_object_create) cujos slots apontam para
 * thunks registrados por x2rw_com_register(). O jogo lê `vtbl = *obj` e
 * `call vtbl[4*i]` e o despachante (x2rw_imports.c) reconhece o VA sintético.
 *
 * O ESTADO VEM DO OVERLAY DE TOQUE
 * --------------------------------
 * Quem escreve o estado de entrada é o overlay Android (a camada que traduz
 * toque/teclado virtual): x2rw_key_state[256] (formato DirectInput: bit 0x80
 * = pressionada), x2rw_mouse_dx/dy e x2rw_mouse_buttons (declarados em
 * x2rw_shims.h e definidos em x2rw_core.c). Este arquivo é só o LEITOR.
 *
 * O QUE NÃO É POSSÍVEL FAZER AQUI (registrado, não escondido)
 * ----------------------------------------------------------
 *  - EnumDevices/EnumObjects: o callback é CÓDIGO x86 DO GUEST. Chamá-lo
 *    exigiria um mini-invocador x86 (montar args + transferir controle) e
 *    o retorno do callback; o lifter não expõe esse caminho para o host.
 *    Devolvemos DI_OK com o enumeração VAZIA. Se o jogo depender da
 *    enumeração (não depende: ele pede SysKeyboard/SysMouse direto pelo
 *    GUID), nenhum dispositivo apareceria.
 *  - Effect/force feedback: sem hardware no Android; slots devolvem
 *    DI_OK/DIERR e ficam no log.
 *  - GetDeviceState do MOUSE devolve lX/lY do overlay (movimento relativo),
 *    lZ = 0 e 4 botões. NÃO há leitura de dispositivo físico aqui.
 *
 * CONVENÇÃO: stdcall. NENHUM handler toca em c->r.esp (o despachante já
 * faz c->r.esp += 4*nargs). Para métodos COM, nargs inclui o `this` em
 * args[0] — ver invoke_from_stack().
 * ============================================================================
 */
#include "x2rw_runtime.h"
#include "x2rw_shims.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Constantes de DirectInput                                          */
/* ------------------------------------------------------------------ */

#define DIN_OK             0u          /* DI_OK == S_OK           */
#define DIN_VERSION        0x0800u     /* DIRECTINPUT_VERSION     */

/* DIMOUSESTATE (20 bytes): lX @0, lY @4, lZ @8, rgbButtons[4] @0xC */
#define DIN_MOUSE_BYTES    20u
#define DIN_KEY_BYTES      256u

/* Identificadores das classes de dispositivo. */
#define DIN_DEVTYPE_MOUSE    0u   /* DIMOUSESTATE (20 bytes)          */
#define DIN_DEVTYPE_KEYBOARD 1u   /* teclado (256 bytes)              */
#define DIN_DEVTYPE_UNKNOWN  2u

/* ------------------------------------------------------------------ */
/* Registro dos dispositivos criados                                  */
/* ------------------------------------------------------------------ */
/* A vtable é a mesma para todos os dispositivos da mesma classe, mas o
 * TIPO (teclado/mouse) é por dispositivo: precisamos guardá-lo para saber
 * como responder GetDeviceState/GetDeviceData (um teclado pode pedir
 * 256 bytes; o mouse pede 20). O objeto vive no heap do guest e o registro
 * aponta para ele. */

typedef struct {
    uint32_t obj;      /* VA do objeto COM no guest (0 = slot livre) */
    uint32_t guid[4];  /* GUID pedido em CreateDevice (como está no guest) */
    int      type;     /* DIN_DEVTYPE_* */
    int      acquired;
    uint32_t calls;    /* GetDeviceState atendidos (resumo no log) */
} din_device;

#define DIN_MAX_DEVICES 8
static din_device g_din_devices[DIN_MAX_DEVICES];

static din_device *din_find(uint32_t obj) {
    for (int i = 0; i < DIN_MAX_DEVICES; i++)
        if (g_din_devices[i].obj == obj && obj != 0u)
            return &g_din_devices[i];
    return NULL;
}

/* Compara o GUID do guest com o GUID canônico do sistema, em BYTES.
 *
 * O guest guarda o GUID exatamente como o Windows o tem em memória:
 * Data1/Data2/Data3 em little-endian e Data4 em bytes. Os quatro uint32 do
 * struct acima reproduzem esse layout, então a comparação é feita byte a
 * byte contra a forma de memória canônica (evita armadilha de endianness). */
static int din_guid_is_bytes(const uint32_t g[4], uint32_t d1, uint16_t d2,
                             uint16_t d3, const uint8_t d4[8]) {
    uint8_t b[16];
    memcpy(b + 0, &d1, 4);
    memcpy(b + 4, &d2, 2);
    memcpy(b + 6, &d3, 2);
    memcpy(b + 8, d4, 8);
    const uint8_t *got = (const uint8_t *)g;
    for (int i = 0; i < 16; i++)
        if (got[i] != b[i]) return 0;
    return 1;
}

/* GUIDs do sistema (dinput.h): {6F1D2B6x-D5A0-11CF-BFC7-444553540000} */
static const uint8_t DIN_SYS_D4[8] = { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54,
                                       0x00, 0x00 };

static int din_is_system_mouse(const uint32_t g[4]) {
    return din_guid_is_bytes(g, 0x6F1D2B60u, 0xD5A0u, 0x11CFu, DIN_SYS_D4);
}

static int din_is_system_keyboard(const uint32_t g[4]) {
    return din_guid_is_bytes(g, 0x6F1D2B61u, 0xD5A0u, 0x11CFu, DIN_SYS_D4);
}

/* Classificação rápida por Data1 (o jogo usa os GUIDs do sistema). */
static int din_classify(const uint32_t g[4]) {
    if (g[0] == 0x6F1D2B70u) return DIN_DEVTYPE_MOUSE;      /* ...2B70 joy?  */
    if (g[0] == 0x6F1D2B61u) return DIN_DEVTYPE_KEYBOARD;   /* SysKeyboard   */
    if (g[0] == 0x6F1D2B60u) return DIN_DEVTYPE_MOUSE;      /* SysMouse      */
    return DIN_DEVTYPE_UNKNOWN;
}

/* ------------------------------------------------------------------ */
/* Vtables                                                            */
/* ------------------------------------------------------------------ */
/* A vtable do IDirectInputDevice8 é usada por CreateDevice, definida mais
 * abaixo junto com a do IDirectInput8. */
static int din_device_vtable(x2rw_ctx *c, uint32_t *out);
static int din_di8_vtable(x2rw_ctx *c, uint32_t *out);

/* ------------------------------------------------------------------ */
/* IUnknown                                                           */
/* ------------------------------------------------------------------ */

static uint32_t din_com_query_interface(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t ppv = (nargs > 3) ? args[3] : 0u;
    if (ppv) x2rw_write32(c, ppv, args[0]);
    return DIN_OK;
}

static uint32_t din_com_add_ref(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c; (void)args; (void)nargs;
    return 1u;
}

static uint32_t din_com_release(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c; (void)args; (void)nargs;
    return 0u;
}

/* Handler genérico: métodos sem uso no caminho do port (propriedades,
 * efeitos, EnumObjects, RunControlPanel...) devolvem DI_OK e ficam no log,
 * para que nenhum slot da vtable aponte para 0x0. */
static uint32_t din_com_unused(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c; (void)args; (void)nargs;
    return DIN_OK;
}

/* ------------------------------------------------------------------ */
/* IDirectInput8                                                      */
/* ------------------------------------------------------------------ */

/* HRESULT CreateDevice(REFGUID rguid, LPDIRECTINPUTDEVICE *lplpDevice,
 *                      LPUNKNOWN pUnkOuter)  == 4 args com o this. */
static uint32_t din_create_device(x2rw_ctx *c, uint32_t *args, int nargs) {
    /* args[0] = this; rguid = args[1], lplpDevice = args[2],
     * args[3] = pUnkOuter. */
    uint32_t guid_va = (nargs > 1) ? args[1] : 0u;
    uint32_t out_pp  = (nargs > 2) ? args[2] : 0u;

    if (!out_pp) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }

    uint32_t guid[4] = { 0u, 0u, 0u, 0u };
    if (guid_va) x2rw_mem_read(guid_va, guid, sizeof(guid));

    int type = DIN_DEVTYPE_UNKNOWN;
    if (guid_va) {
        /* 1) classificação pelos GUIDs do sistema (o caminho normal);
         * 2) fallback byte a byte, caso o jogo tenha copiado o GUID para
         *    outro lugar sem alterar o layout. */
        type = din_classify(guid);
        if (type == DIN_DEVTYPE_UNKNOWN && din_is_system_mouse(guid))
            type = DIN_DEVTYPE_MOUSE;
        if (type == DIN_DEVTYPE_UNKNOWN && din_is_system_keyboard(guid))
            type = DIN_DEVTYPE_KEYBOARD;
    }

    uint32_t vtbl[24];
    if (!din_device_vtable(c, vtbl)) {
        X2RW_FAIL(c, X2RW_ERROR_OUTOFMEMORY);
        return 0u;
    }
    uint32_t obj = x2rw_com_object_create(c, vtbl, 24);
    if (!obj) {
        X2RW_FAIL(c, X2RW_ERROR_OUTOFMEMORY);
        return 0u;
    }

    int slot = -1;
    for (int i = 0; i < DIN_MAX_DEVICES; i++)
        if (g_din_devices[i].obj == 0u) { slot = i; break; }
    if (slot < 0) {
        x2rw_log("[input] CreateDevice: tabela de dispositivos cheia (%d)",
                 DIN_MAX_DEVICES);
        X2RW_FAIL(c, X2RW_ERROR_NOT_ENOUGH_MEMORY);
        return 0u;
    }
    g_din_devices[slot].obj = obj;
    memcpy(g_din_devices[slot].guid, guid, sizeof(guid));
    g_din_devices[slot].type = type;
    g_din_devices[slot].acquired = 0;
    g_din_devices[slot].calls = 0u;

    x2rw_write32(c, out_pp, obj);
    x2rw_log("[input] IDirectInput8::CreateDevice(guid=%08x %08x %08x %08x) -> "
             "%08x (%s; vtable de 24 slots na memória do guest)",
             guid[0], guid[1], guid[2], guid[3], obj,
             type == DIN_DEVTYPE_MOUSE ? "mouse" :
             type == DIN_DEVTYPE_KEYBOARD ? "teclado" : "classe desconhecida");
    X2RW_OK();
    return DIN_OK;
}

/* EnumDevices(dwDevType, LPDIENUMDEVICESCALLBACK, pvRef, dwFlags) == 5 args.
 *
 * NÃO chamamos o callback: ele é código x86 DO JOGO e este host não tem um
 * invocador genérico para ele (ver nota no topo do arquivo). Devolvemos
 * DI_OK com a enumeração VAZIA e registramos a limitação. O jogo obtém os
 * dispositivos por CreateDevice(SysKeyboard/SysMouse), que funciona. */
static uint32_t din_enum_devices(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t cb = (nargs > 1) ? args[1] : 0u;   /* LPDIENUMDEVICESCALLBACK */
    x2rw_log("[input] IDirectInput8::EnumDevices(cb=%08x) devolve DI_OK com a "
             "enumeração VAZIA — o callback do jogo NÃO é chamado (código x86 "
             "do guest não é invocável do host)", cb);
    X2RW_OK();
    return DIN_OK;
}

/* ------------------------------------------------------------------ */
/* IDirectInputDevice8                                                */
/* ------------------------------------------------------------------ */

static uint32_t din_dev_get_capabilities(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t out = (nargs > 1) ? args[1] : 0u;    /* LPDIDEVCAPS (depois do this) */
    din_device *d = din_find(args[0]);
    if (!out) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }
    /* DIDEVCAPS: dwSize @0, dwFlags @4, dwDevType @8, dwAxes @0xC,
     * dwButtons @0x10, dwPOVs @0x14 — 44 bytes no total. */
    uint32_t sz = x2rw_read32(c, out);
    if (sz == 0u || sz > 128u) sz = 44u;
    int type = d ? d->type : DIN_DEVTYPE_UNKNOWN;
    uint32_t devtype = (type == DIN_DEVTYPE_MOUSE)    ? 0x00000012u :  /* DI8DEVTYPE_MOUSE */
                       (type == DIN_DEVTYPE_KEYBOARD) ? 0x00000011u :  /* DI8DEVTYPE_KEYBOARD */
                                                        0x00000000u;
    x2rw_write32(c, out + 0u, sz);
    x2rw_write32(c, out + 4u, 0x00000001u);   /* DIDC_ATTACHED */
    x2rw_write32(c, out + 8u, devtype);
    x2rw_write32(c, out + 12u, (type == DIN_DEVTYPE_MOUSE) ? 3u : 0u); /* eixos  */
    x2rw_write32(c, out + 16u, (type == DIN_DEVTYPE_MOUSE) ? 4u : 256u);/* botões */
    x2rw_write32(c, out + 20u, 0u);           /* POVs */
    x2rw_log("[input] IDirectInputDevice8::GetCapabilities -> tipo=%s em %08x",
             type == DIN_DEVTYPE_MOUSE ? "mouse" :
             type == DIN_DEVTYPE_KEYBOARD ? "teclado" : "desconhecido", out);
    X2RW_OK();
    return DIN_OK;
}

static uint32_t din_dev_acquire(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c; (void)nargs;
    din_device *d = din_find(args[0]);
    if (d) d->acquired = 1;
    return DIN_OK;
}

static uint32_t din_dev_unacquire(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c; (void)nargs;
    din_device *d = din_find(args[0]);
    if (d) d->acquired = 0;
    return DIN_OK;
}

/* HRESULT GetDeviceState(DWORD cbData, LPVOID lpvData) == 3 args com this.
 * Teclado: copia os 256 bytes de x2rw_key_state (formato DirectInput).
 * Mouse:   escreve DIMOUSESTATE (20 bytes) a partir de x2rw_mouse_dx/dy,
 *          x2rw_mouse_buttons (lZ = 0: sem scroll neste port). */
static uint32_t din_dev_get_device_state(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t self = args[0];
    uint32_t cb   = (nargs > 1) ? args[1] : 0u;    /* cbData */
    uint32_t data = (nargs > 2) ? args[2] : 0u;    /* lpvData */

    if (!data) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }

    din_device *d = din_find(self);
    int type = d ? d->type : DIN_DEVTYPE_UNKNOWN;

    if (type == DIN_DEVTYPE_MOUSE ||
        (type == DIN_DEVTYPE_UNKNOWN && cb > 0u && cb < DIN_KEY_BYTES)) {
        /* --- mouse: movimento relativo acumulado pelo overlay de toque --- */
        if (cb < DIN_MOUSE_BYTES) {
            x2rw_log("[input] GetDeviceState(mouse): cbData=%u < %u (DIMOUSESTATE)",
                     cb, DIN_MOUSE_BYTES);
            if (cb) {
                uint8_t zero[DIN_MOUSE_BYTES];
                memset(zero, 0, sizeof(zero));
                x2rw_mem_write(data, zero, cb);
            }
            X2RW_OK();
            return DIN_OK;
        }
        uint8_t ms[DIN_MOUSE_BYTES];
        memset(ms, 0, sizeof(ms));
        int32_t dx = x2rw_mouse_dx;
        int32_t dy = x2rw_mouse_dy;
        int32_t btns = x2rw_mouse_buttons;
        memcpy(ms + 0, &dx, 4);
        memcpy(ms + 4, &dy, 4);
        /* lZ @8 fica 0 (sem roda/scroll) */
        for (int i = 0; i < 4; i++)
            ms[12 + i] = (uint8_t)((btns & (1 << i)) ? 0x80 : 0x00);
        x2rw_mem_write(data, ms, DIN_MOUSE_BYTES);

        /* O movimento relativo é consumido na leitura, como no DirectInput. */
        x2rw_mouse_dx = 0;
        x2rw_mouse_dy = 0;

        if (d) d->calls++;
        x2rw_log("[input] GetDeviceState(mouse): DIMOUSESTATE lX=%d lY=%d "
                 "lZ=0 buttons=%02x (deltas consumidos)", dx, dy, btns & 0x0f);
        X2RW_OK();
        return DIN_OK;
    }

    /* --- teclado: x2rw_key_state é exatamente o formato pedido --- */
    if (cb > DIN_KEY_BYTES) {
        /* O jogo pediu mais que 256: copiamos os 256 e zeramos o resto. */
        x2rw_mem_write(data, (const void *)x2rw_key_state, DIN_KEY_BYTES);
        uint8_t zero[64];
        memset(zero, 0, sizeof(zero));
        uint32_t rest = cb - DIN_KEY_BYTES;
        while (rest) {
            uint32_t n = rest > sizeof(zero) ? (uint32_t)sizeof(zero) : rest;
            x2rw_mem_write(data + DIN_KEY_BYTES + (cb - DIN_KEY_BYTES - rest),
                           zero, n);
            rest -= n;
        }
    } else {
        x2rw_mem_write(data, (const void *)x2rw_key_state, cb);
    }
    if (d) d->calls++;
    /* Não logamos a cada leitura (é o caminho mais quente do input); o
     * resumo sai no primeiro GetDeviceState de cada dispositivo. */
    if (d && d->calls == 1u)
        x2rw_log("[input] GetDeviceState(teclado): %u bytes de x2rw_key_state "
                 "copiados para %08x (formato DirectInput, bit 0x80)", cb, data);
    X2RW_OK();
    return DIN_OK;
}

/* HRESULT GetDeviceData(DWORD cbObjectData, LPDIDEVICEOBJECTDATA rgdod,
 *                       LPDWORD pdwInOut, DWORD dwFlags) == 5 args (com this):
 *   args[1] cbObjectData, args[2] rgdod, args[3] pdwInOut, args[4] dwFlags.
 * Sem fila de eventos: nada a reportar, e *pdwInOut = 0 diz isso ao jogo
 * (ele então usa GetDeviceState). */
static uint32_t din_dev_get_device_data(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t inout = (nargs > 3) ? args[3] : 0u;   /* LPDWORD pdwInOut */
    if (inout) x2rw_write32(c, inout, 0u);
    return DIN_OK;
}

/* ------------------------------------------------------------------ */
/* Vtables (definições; as declarações estão no topo do arquivo)       */
/* ------------------------------------------------------------------ */

/* IDirectInput8 — 11 slots:
 *   0 QueryInterface 1 AddRef 2 Release 3 CreateDevice 4 EnumDevices
 *   5 GetDeviceStatus 6 RunControlPanel 7 Initialize 8 FindDevice
 *   9 EnumDevicesBySemantics 10 ConfigureDevices                       */
static int din_di8_vtable(x2rw_ctx *c, uint32_t *out) {
    static uint32_t v[11];
    static int ready = 0;
    (void)c;
    if (ready) {
        for (int i = 0; i < 11; i++) out[i] = v[i];
        return 1;
    }
    v[0]  = x2rw_com_register(din_com_query_interface, 4);
    v[1]  = x2rw_com_register(din_com_add_ref, 1);
    v[2]  = x2rw_com_register(din_com_release, 1);
    v[3]  = x2rw_com_register(din_create_device, 4);
    v[4]  = x2rw_com_register(din_enum_devices, 5);
    v[5]  = x2rw_com_register(din_com_unused, 2);   /* GetDeviceStatus      */
    v[6]  = x2rw_com_register(din_com_unused, 2);   /* RunControlPanel      */
    v[7]  = x2rw_com_register(din_com_unused, 3);   /* Initialize           */
    v[8]  = x2rw_com_register(din_com_unused, 4);   /* FindDevice           */
    v[9]  = x2rw_com_register(din_com_unused, 6);   /* EnumDevicesBySemantics */
    v[10] = x2rw_com_register(din_com_unused, 5);   /* ConfigureDevices     */
    for (int i = 0; i < 11; i++) {
        if (v[i] == 0u) {
            x2rw_log("[input] ERRO: thunk COM nulo no slot %d de IDirectInput8", i);
            return 0;
        }
    }
    ready = 1;
    for (int i = 0; i < 11; i++) out[i] = v[i];
    return 1;
}

/* IDirectInputDevice8 — 24 slots. Os índices 3/9/10 são os usados no
 * caminho principal:
 *   3 GetCapabilities  7 Acquire  8 Unacquire  9 GetDeviceState
 *   10 GetDeviceData
 * O resto (EnumObjects/GetProperty/SetProperty/GetObjectInfo e os métodos
 * de force feedback) recebe o handler genérico: nenhum slot fica nulo. */
static int din_device_vtable(x2rw_ctx *c, uint32_t *out) {
    static uint32_t v[24];
    static int ready = 0;
    (void)c;
    if (ready) {
        for (int i = 0; i < 24; i++) out[i] = v[i];
        return 1;
    }
    v[0]  = x2rw_com_register(din_com_query_interface, 4);
    v[1]  = x2rw_com_register(din_com_add_ref, 1);
    v[2]  = x2rw_com_register(din_com_release, 1);
    v[3]  = x2rw_com_register(din_dev_get_capabilities, 3);
    v[4]  = x2rw_com_register(din_com_unused, 4);   /* EnumObjects          */
    v[5]  = x2rw_com_register(din_com_unused, 4);   /* GetProperty          */
    v[6]  = x2rw_com_register(din_com_unused, 5);   /* SetProperty          */
    v[7]  = x2rw_com_register(din_dev_acquire, 2);
    v[8]  = x2rw_com_register(din_dev_unacquire, 2);
    v[9]  = x2rw_com_register(din_dev_get_device_state, 3);
    v[10] = x2rw_com_register(din_dev_get_device_data, 5);
    v[11] = x2rw_com_register(din_com_unused, 4);   /* GetObjectInfo        */
    v[12] = x2rw_com_register(din_com_unused, 3);   /* GetDeviceInfo        */
    v[13] = x2rw_com_register(din_com_unused, 3);   /* GetDeviceInstance... */
    v[14] = x2rw_com_register(din_com_unused, 3);   /* SetDataFormat        */
    v[15] = x2rw_com_register(din_com_unused, 4);   /* SetEventNotification */
    v[16] = x2rw_com_register(din_com_unused, 4);   /* SetCooperativeLevel  */
    v[17] = x2rw_com_register(din_com_unused, 3);   /* GetObjectProperties  */
    v[18] = x2rw_com_register(din_com_unused, 4);   /* GetEffectInfo        */
    v[19] = x2rw_com_register(din_com_unused, 3);   /* GetForceFeedbackState*/
    v[20] = x2rw_com_register(din_com_unused, 4);   /* SendForceFeedbackCmd */
    v[21] = x2rw_com_register(din_com_unused, 4);   /* EnumCreatedEffectObjects */
    v[22] = x2rw_com_register(din_com_unused, 4);   /* Escape               */
    v[23] = x2rw_com_register(din_com_unused, 4);   /* Poll                 */
    for (int i = 0; i < 24; i++) {
        if (v[i] == 0u) {
            x2rw_log("[input] ERRO: thunk COM nulo no slot %d de "
                     "IDirectInputDevice8", i);
            return 0;
        }
    }
    ready = 1;
    for (int i = 0; i < 24; i++) out[i] = v[i];
    return 1;
}

/* ------------------------------------------------------------------ */
/* Ponto de entrada: DirectInput8Create                              */
/* ------------------------------------------------------------------ */
/* HRESULT DirectInput8Create(HINSTANCE hinst, DWORD dwVersion,
 *                            REFIID riidltf, LPVOID *ppvOut,
 *                            LPUNKNOWN punkOuter)  — 5 args. */
uint32_t inp_DirectInput8Create(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t hinst   = (nargs > 0) ? args[0] : 0u;
    uint32_t version = (nargs > 1) ? args[1] : 0u;
    uint32_t riid    = (nargs > 2) ? args[2] : 0u;
    uint32_t ppv     = (nargs > 3) ? args[3] : 0u;   /* LPVOID *ppvOut */
    uint32_t outer   = (nargs > 4) ? args[4] : 0u;
    (void)hinst; (void)riid; (void)outer;

    if (!ppv) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }

    /* A versão pedida é 0x0800 (DirectInput 8). Qualquer valor é aceito —
     * o objeto que devolvemos é o mesmo —, mas registramos a diferença. */
    if (version != DIN_VERSION) {
        x2rw_log("[input] DirectInput8Create: versão pedida %04x (esperada "
                 "%04x) — aceitando e registrando", version, DIN_VERSION);
    }

    uint32_t vtbl[11];
    if (!din_di8_vtable(c, vtbl)) {
        X2RW_FAIL(c, X2RW_ERROR_OUTOFMEMORY);
        return 0u;
    }
    uint32_t obj = x2rw_com_object_create(c, vtbl, 11);
    if (!obj) {
        X2RW_FAIL(c, X2RW_ERROR_OUTOFMEMORY);
        return 0u;
    }
    x2rw_write32(c, ppv, obj);

    x2rw_log("[input] DirectInput8Create(ver=%04x) -> IDirectInput8 em %08x "
             "(vtable de 11 slots; estado de entrada vem de x2rw_key_state/"
             "x2rw_mouse_dx/dy)", version, obj);
    X2RW_OK();
    return DIN_OK;
}
