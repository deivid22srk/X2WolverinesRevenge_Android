/* X2: Wolverine's Revenge - port Android
 * ============================================================================
 * x2rw_d3d8.c — d3d8.dll: a FÁBRICA Direct3DCreate8 e a vtable do
 *                IDirect3D8.
 *
 * O jogo importa UM único símbolo de d3d8.dll — Direct3DCreate8 (lista
 * mestra: 1 arg, o SDKVersion = 0x220). Todo o resto do Direct3D é COM:
 * o jogo faz `vtbl = *obj; call vtbl[4*i]`. Como as vtables vivem na
 * memória do guest, cada slot recebe um VA sintético de
 * x2rw_com_register() e o despachante marshalla os argumentos.
 *
 * Layout da vtable — IDirect3D8 do SDK 8.1, 16 slots (0..15), CONFERIDO
 * contra o d3d8.h do SDK e contra o próprio binário:
 *
 *   0  QueryInterface            8  GetAdapterDisplayMode
 *   1  AddRef                    9  CheckDeviceType
 *   2  Release                  10  CheckDeviceFormat
 *   3  RegisterSoftwareDevice   11  CheckDeviceMultiSampleType
 *   4  GetAdapterCount          12  CheckDepthStencilMatch
 *   5  GetAdapterIdentifier     13  GetDeviceCaps
 *   6  GetAdapterModeCount      14  GetAdapterMonitor
 *   7  EnumAdapterModes         15  CreateDevice
 *
 * Três provas tiradas do binário (não de memória):
 *   - 0x00560184 `call [ecx+0x20]`  = índice 8  = GetAdapterDisplayMode
 *     (this, Adapter, pMode) com 3 pushes — casa com o SDK;
 *   - 0x0056013c `call [ecx+0x34]`  = índice 13 = GetDeviceCaps
 *     (this, Adapter, DeviceType, pCaps) com 4 pushes — casa com o SDK;
 *   - 0x00560624 `call [edx+0x3c]`  = índice 15 = CreateDevice, com os 7
 *     argumentos (this, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
 *     pPresentationParameters, ppDevice) — casa com o SDK e NÃO com um
 *     slot 16.
 * O identificador de 1608 bytes também foi confirmado no binário: o jogo
 * lê a Description em [renderer+0x228], e o renderer guarda o identificador
 * em +0x28 → 0x228-0x28 = 0x200 = 512, exatamente o offset do campo.
 *
 * ATENÇÃO (corrigido na revisão): a primeira versão desta tabela tinha
 * uma ordem "quase D3D9" — GetAdapterModeCount e EnumAdapterModes
 * trocados, GetAdapterModeCount em 8, CheckDeviceFormatConversion (que é
 * API do D3D9, não existe no D3D8) em 14 e CreateDevice em 16. Como
 * invoke_from_stack() usa o nargs REGISTRADO para ler os argumentos e
 * avançar `esp`, cada contagem errada corrompia a pilha do guest. A ordem
 * e as contagens abaixo são as do SDK, uma a uma, e batem com os três
 * call sites acima.
 *
 * Este arquivo NÃO renderiza nada. Ele mantém o objeto/vtable/estado e
 * delega todo trabalho de GPU aos hooks `x2rw_d3d8_hooks` (x2rw_core.c
 * guarda o ponteiro; outro engenheiro instala o renderizador Vulkan/GLES
 * com x2rw_d3d8_set_hooks()).
 *
 * TUDO O QUE ESTE ARQUIVO RELATA AO JOGO SOBRE O HARDWARE É SINTÉTICO:
 * adaptador, modos de vídeo e D3DCAPS8 são um conjunto plausível e
 * mínimo, escolhido para o jogo passar das suas verificações de
 * capacidade e chegar ao CreateDevice. Não é o hardware real, e não
 * pretende ser: o renderizador real é quem sabe o que a GPU suporta.
 * O que NÃO é sintético é a honestidade do CreateDevice: sem hook
 * instalado devolvemos D3DERR_NOTAVAILABLE e logamos — devolver um
 * device falso faria o jogo seguir e quebrar muito depois.
 * ============================================================================
 */
#include "x2rw_runtime.h"
#include "x2rw_shims.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* HRESULTs e enums do D3D8 (não incluímos d3d8.h: sem headers Win32)  */
/* ------------------------------------------------------------------ */

#define X2RW_D3D_OK                    0x00000000u
#define X2RW_E_NOINTERFACE             0x80004002u
#define X2RW_D3DERR_INVALIDCALL        0x8876086Cu
#define X2RW_D3DERR_NOTAVAILABLE       0x8876086Au
#define X2RW_D3DERR_INVALIDDEVICE      0x8876086Bu

/* HMONITOR sintético: handle opaco, nunca é um monitor de verdade. */
#define X2RW_D3D_MONITOR_PSEUDO        0x00010001u

#define X2RW_D3DADAPTER_DEFAULT        0u
#define X2RW_D3DDEVTYPE_HAL            1u
#define X2RW_D3DDEVTYPE_REF            3u

/* D3DFORMATs usados (os valores são os do SDK 8.1) */
#define X2RW_D3DFMT_R8G8B8             20u
#define X2RW_D3DFMT_A8R8G8B8           21u
#define X2RW_D3DFMT_X8R8G8B8           22u   /* o formato do back buffer */
#define X2RW_D3DFMT_R5G6B5             23u
#define X2RW_D3DFMT_X1R5G5B5           24u
#define X2RW_D3DFMT_A1R5G5B5           25u
#define X2RW_D3DFMT_A4R4G4B4           26u
#define X2RW_D3DFMT_R3G3B2             27u
#define X2RW_D3DFMT_A8                 28u
#define X2RW_D3DFMT_A8R3G3B2           29u
#define X2RW_D3DFMT_X4R4G4B4           30u
#define X2RW_D3DFMT_A2B10G10R10        31u
#define X2RW_D3DFMT_A8B8G8R8           32u
#define X2RW_D3DFMT_X8B8G8R8           33u
#define X2RW_D3DFMT_A2R10G10B10        35u
#define X2RW_D3DFMT_A8P8               40u
#define X2RW_D3DFMT_DXT1               0x31545844u
#define X2RW_D3DFMT_DXT2               0x32545844u
#define X2RW_D3DFMT_DXT3               0x33545844u
#define X2RW_D3DFMT_DXT4               0x34545844u
#define X2RW_D3DFMT_DXT5               0x35545844u
#define X2RW_D3DFMT_D16_LOCKABLE       70u
#define X2RW_D3DFMT_D32                71u
#define X2RW_D3DFMT_D15S1              73u
#define X2RW_D3DFMT_D24S8              75u
#define X2RW_D3DFMT_D24X8              77u
#define X2RW_D3DFMT_D24X4S4            79u
#define X2RW_D3DFMT_D16                80u
#define X2RW_D3DFMT_VERTEXDATA         100u
#define X2RW_D3DFMT_INDEX16            101u
#define X2RW_D3DFMT_INDEX32            102u

/* ------------------------------------------------------------------ */
/* IIDs                                                               */
/* ------------------------------------------------------------------ */

/* IID_IDirect3D8 e IID_IUnknown, como estão no d3d8.h/d3d8guid do SDK.
 * QueryInterface precisa reconhecer os dois: o jogo pede IUnknown em
 * alguns caminhos (e o agregador do QI do Windows devolve o próprio
 * ponteiro para os dois). */
/* VA do objeto IDirect3D8 corrente (0 = nenhum). O despachante chama os
 * handlers sem `this` próprio, então o objeto é estado do módulo — só
 * existe um por processo, como no Direct3D de verdade. */
static uint32_t g_d3d8_obj = 0u;

/* Diagnóstico: o VA do objeto IDirect3D8 vivo, ou 0. Não é API de jogo —
 * existe para o teste do port e para o log de encerramento. */
uint32_t d3d8_current_object(void) { return g_d3d8_obj; }

/* IID_IDirect3D8 — o GUID de 128 bits do SDK 8.1. */
static const uint8_t x2rw_iid_idirect3d8[16] = {
    0x1D, 0xD7, 0x70, 0x1B, 0x79, 0xC4, 0xD1, 0x11,
    0x89, 0xD8, 0x00, 0xC0, 0x4F, 0xB6, 0xE1, 0x15
};
static const uint8_t x2rw_iid_iunknown[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46
};

static int x2rw_guid_eq(x2rw_ctx *c, uint32_t va, const uint8_t *guid) {
    for (int i = 0; i < 16; i++)
        if (x2rw_read8(c, va + (uint32_t)i) != guid[i]) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Formatos de exibição aceitos pelo adaptador sintético              */
/* ------------------------------------------------------------------ */

static int x2rw_is_display_format(uint32_t fmt) {
    switch (fmt) {
        case X2RW_D3DFMT_R8G8B8:
        case X2RW_D3DFMT_A8R8G8B8:
        case X2RW_D3DFMT_X8R8G8B8:
        case X2RW_D3DFMT_R5G6B5:
        case X2RW_D3DFMT_X1R5G5B5:
        case X2RW_D3DFMT_A1R5G5B5:
        case X2RW_D3DFMT_A4R4G4B4:
        case X2RW_D3DFMT_R3G3B2:
        case X2RW_D3DFMT_A8:
        case X2RW_D3DFMT_A8R3G3B2:
        case X2RW_D3DFMT_X4R4G4B4:
        case X2RW_D3DFMT_A2B10G10R10:
        case X2RW_D3DFMT_A8B8G8R8:
        case X2RW_D3DFMT_X8B8G8R8:
        case X2RW_D3DFMT_A2R10G10B10:
            return 1;
        default:
            return 0;
    }
}

/* Modos que EnumAdapterModes oferece. O último é a resolução corrente do
 * port (g_render_w x g_render_h), que o launcher Android ajusta. */
typedef struct { uint32_t w, h; } x2rw_display_mode;

static const x2rw_display_mode x2rw_modes[] = {
    {  640u,  480u },
    {  800u,  600u },
    { 1024u,  768u },
    { 1280u,  720u },
    {    0u,    0u }    /* sentinela: preenchido com g_render_* */
};

#define X2RW_N_MODES ((int)(sizeof(x2rw_modes) / sizeof(x2rw_modes[0])))

/* Escreve uma D3DDISPLAYMODE (16 bytes: UINT Width, Height, RefreshRate,
 * D3DFORMAT Format) no guest. RefreshRate = 0 em modo janela e 60 nos
 * modos dedicados — o jogo só compara o formato. */
static void x2rw_write_display_mode(x2rw_ctx *c, uint32_t va,
                                    uint32_t w, uint32_t h, uint32_t refresh) {
    x2rw_write32(c, va + 0u,  w);
    x2rw_write32(c, va + 4u,  h);
    x2rw_write32(c, va + 8u,  refresh);
    x2rw_write32(c, va + 12u, X2RW_D3DFMT_X8R8G8B8);
}

/* Dimensões de um modo, resolvendo a sentinela para a resolução corrente. */
static void x2rw_mode_dims(int i, uint32_t *w, uint32_t *h) {
    if (i < 0 || i >= X2RW_N_MODES) { *w = 0u; *h = 0u; return; }
    if (x2rw_modes[i].w == 0u) {
        *w = (uint32_t)((g_render_w > 0) ? g_render_w : 1280);
        *h = (uint32_t)((g_render_h > 0) ? g_render_h : 720);
    } else {
        *w = x2rw_modes[i].w;
        *h = x2rw_modes[i].h;
    }
}

/* ------------------------------------------------------------------ */
/* D3DCAPS8 — offsets verificados byte a byte                         */
/* ------------------------------------------------------------------ */
/* A D3DCAPS8 é 4-byte packed (pshpack4.h no SDK para i386) e tem 212
 * BYTES — medido, não estimado, em duas fontes independentes:
 *   1. a definição do d3d8caps.h do SDK 8.1 (soma dos campos, pack(4));
 *   2. o PRÓPRIO BINÁRIO: em 0x005600d9 o jogo reserva a struct em
 *      ebp+0x454 e o membro seguinte é acessado em ebp+0x528 — diferença
 *      de 0xD4 = 212 bytes. Zerar mais que isso invadiria o campo
 *      seguinte do objeto do jogo (foi um bug pego na revisão: a
 *      primeira versão zerava 252 por analogia com a D3DCAPS9, que é
 *      maior).
 *
 * Abaixo, o offset de CADA campo usado. Os offsets batem byte a byte com
 * o d3d8caps.h (verificado campo a campo, 53 campos):
 *
 *   0   DeviceType (D3DDEVTYPE)      4   AdapterOrdinal (UINT)
 *   8   Caps                        12   Caps2
 *   16  Caps3                       20   PresentationIntervals
 *   24  CursorCaps                  28   DevCaps
 *   32  PrimitiveMiscCaps           36   RasterCaps
 *   40  ZCmpCaps                    44   SrcBlendCaps
 *   48  DestBlendCaps               52   AlphaCmpCaps
 *   56  ShadeCaps                   60   TextureCaps
 *   64  TextureFilterCaps           68   CubeTextureFilterCaps
 *   72  VolumeTextureFilterCaps     76   TextureAddressCaps
 *   80  VolumeTextureAddressCaps    84   LineCaps
 *   88  MaxTextureWidth             92   MaxTextureHeight
 *   96  MaxVolumeExtent            100   MaxTextureRepeat
 *   104 MaxTextureAspectRatio      108   MaxAnisotropy
 *   112 MaxVertexW (float)         116   GuardBandLeft (float)
 *   ...
 *   132 ExtentsAdjust (float)      136   StencilCaps
 *   140 FVFCaps                    144   TextureOpCaps
 *   148 MaxTextureBlendStages      152   MaxSimultaneousTextures
 *   156 VertexProcessingCaps       160   MaxActiveLights
 *   164 MaxUserClipPlanes          168   MaxVertexBlendMatrices
 *   172 MaxVertexBlendMatrixIndex  176   MaxPointSize (float)
 *   180 MaxPrimitiveCount          184   MaxVertexIndex
 *   188 MaxStreams                 192   MaxStreamStride
 *   196 VertexShaderVersion         200   MaxVertexShaderConst
 *   204 PixelShaderVersion         208   MaxPixelShaderValue (float)
 *
 * QUAIS CAMPOS O JOGO REALMENTE LÊ (varredura das 607.901 instruções do
 * .text à procura de acessos a [reg+0x454+offset], que é onde este objeto
 * guarda a D3DCAPS8): DeviceType, AdapterOrdinal, DevCaps, RasterCaps,
 * SrcBlendCaps, DestBlendCaps, AlphaCmpCaps, ShadeCaps, TextureCaps,
 * CubeTextureFilterCaps, VolumeTextureAddressCaps, LineCaps,
 * MaxTextureWidth/Height, MaxTextureRepeat, MaxTextureAspectRatio,
 * MaxUserClipPlanes, MaxVertexBlendMatrices, MaxPointSize,
 * MaxPrimitiveCount, MaxVertexIndex, VertexShaderVersion,
 * MaxVertexShaderConst, PixelShaderVersion, MaxPixelShaderValue.
 * Foi por isso que AlphaCmpCaps, LineCaps, MaxTextureRepeat,
 * MaxTextureAspectRatio e MaxPointSize deixaram de ficar zerados: zero ali
 * não é "neutro", é a afirmação "este device não faz alpha test / não
 * repete textura / desenha ponto de tamanho 0".
 *
 * O que preenchemos (e o porquê):
 *   DeviceType=HAL, AdapterOrdinal=0  -> prefere o caminho de hardware;
 *   Caps2 = CANRENDERWINDOWED|FULLSCREENGAMMA;
 *   PresentationIntervals = IMMEDIATE|ONE (o jogo pede um destes em
 *             D3DPRESENT_PARAMETERS; um 0 aqui pode reprovar o device);
 *   CursorCaps = COLOR;
 *   DevCaps = HWTRANSFORMANDLIGHT|HWRASTERIZATION|DRAWPRIMITIVES2
 *             |DRAWPRIMTLVERTEX|TLVERTEXSYSTEMMEMORY|EXECUTESYSTEMMEMORY;
 *   PrimitiveMiscCaps = MASKZ|CULLNONE|CULLCW|CULLCCW|COLORWRITEENABLE
 *             |TSSARGTEMP|BLENDOP;
 *   RasterCaps = ZBIAS|ZTEST|FOGVERTEX|FOGRANGE|FOGTABLE|MIPMAPLODBIAS
 *             |WFOG|COLORPERSPECTIVE|ANISOTROPY|DITHER|DEPTHBIAS
 *             |ZFOG|TEXTUREPERSPECTIVE (base para fog e depth bias);
 *   ZCmpCaps/AlphaCmpCaps = os 8 comparadores (NEVER..ALWAYS);
 *   SrcBlendCaps/DestBlendCaps = conjunto completo menos BOTHSRCALPHA/
 *             BOTHINVSRCALPHA (o jogo usa blend alfa e deixa esses dois
 *             por último na negociação);
 *   ShadeCaps = COLORGOURAUD|ALPHAGOURAUD|FOGGOURAUD|SPECULARGOURAUD;
 *   TextureCaps = PERSPECTIVE|ALPHA|ALPHAPALETTE|NONPOW2CONDITIONAL|POW2;
 *   TextureFilterCaps e as versões Cube/Volume = MIN/MAG/MIP dos filtros;
 *   TextureAddressCaps e VolumeTextureAddressCaps =
 *             WRAP|MIRROR|CLAMP|BORDER|MIRRORONCE;
 *   LineCaps = TEXTURE|ZTEST|BLEND|ALPHACMP|FOG;
 *   StencilCaps = as 8 operações KEEP..DECR;
 *   FVFCaps = 8 coordenadas de textura | PSIZE;
 *   MaxTextureWidth/Height = 2048 (o jogo limita as texturas a isto);
 *   MaxVolumeExtent = 256, MaxTextureRepeat = 8192,
 *   MaxTextureAspectRatio = 2048, MaxAnisotropy = 16;
 *   MaxTextureBlendStages/SimultaneousTextures = 8;
 *   MaxActiveLights = 8, MaxUserClipPlanes = 1, MaxVertexBlendMatrices = 4,
 *   MaxVertexBlendMatrixIndex = 0 (índice por stream, como num vs_1_1),
 *   MaxPointSize = 64.0f, MaxPrimitiveCount = 0xFFFFF,
 *   MaxVertexIndex = 0xFFFF, MaxStreams = 16, MaxStreamStride = 255,
 *   VertexProcessingCaps = TEXGEN|MATERIALSOURCE7|DIRECTIONALLIGHTS|
 *             POSITIONALLIGHTS|LOCALVIEWER,
 *   VertexShaderVersion = 0xFFFE0101 (vs_1_1),
 *   MaxVertexShaderConst = 96,
 *   PixelShaderVersion = 0xFFFF0104 (ps_1_4),
 *   MaxPixelShaderValue = 1.0f.
 *
 * Fica ZERADO de propósito (o valor depende do renderizador e o jogo não
 * lê nenhum destes na varredura acima):
 *   Caps, Caps3, TextureOpCaps, MaxVertexW, GuardBandLeft/Top/Right/Bottom,
 *   ExtentsAdjust.
 * O renderizador real pode sobrescrever tudo isto chamando GetDeviceCaps
 * no seu próprio hook (a struct está na memória do guest e o hook tem
 * acesso a ela). */

/* Flags que usamos (d3d8caps.h) */
#define X2RW_D3DCAPS2_CANRENDERWINDOWED   0x00020000u
#define X2RW_D3DCAPS2_FULLSCREENGAMMA     0x00040000u
#define X2RW_D3DDEVCAPS_EXECUTESYSTEMMEMORY 0x0000010u
#define X2RW_D3DDEVCAPS_TLVERTEXSYSTEMMEMORY 0x0000040u
#define X2RW_D3DDEVCAPS_DRAWPRIMTLVERTEX    0x0000400u
#define X2RW_D3DDEVCAPS_DRAWPRIMITIVES2     0x0002000u
#define X2RW_D3DDEVCAPS_HWTRANSFORMANDLIGHT 0x0010000u
#define X2RW_D3DDEVCAPS_HWRASTERIZATION     0x0080000u
#define X2RW_D3DPRASTERCAPS_DITHER          0x0000001u
#define X2RW_D3DPRASTERCAPS_ZTEST           0x0000010u
#define X2RW_D3DPRASTERCAPS_FOGVERTEX       0x0000080u
#define X2RW_D3DPRASTERCAPS_FOGTABLE        0x0000100u
#define X2RW_D3DPRASTERCAPS_MIPMAPLODBIAS   0x0002000u
#define X2RW_D3DPRASTERCAPS_ZBIAS           0x0004000u
#define X2RW_D3DPRASTERCAPS_ZBUFFERLESSHSR  0x0008000u
#define X2RW_D3DPRASTERCAPS_FOGRANGE        0x0010000u
#define X2RW_D3DPRASTERCAPS_ANISOTROPY      0x0020000u
#define X2RW_D3DPRASTERCAPS_WFOG            0x0040000u
#define X2RW_D3DPRASTERCAPS_ZFOG            0x0200000u
#define X2RW_D3DPRASTERCAPS_COLORPERSPECTIVE 0x0400000u
#define X2RW_D3DPRASTERCAPS_DEPTHBIAS       0x4000000u
#define X2RW_D3DPRASTERCAPS_TEXTUREPERSPECTIVE 0x0000004u
#define X2RW_D3DPSHADECAPS_COLORGOURAUDRGB   0x0000008u
#define X2RW_D3DPSHADECAPS_SPECULARGOURAUDRGB 0x0000200u
#define X2RW_D3DPSHADECAPS_ALPHAGOURAUDBLEND 0x0000400u
#define X2RW_D3DPSHADECAPS_FOGGOURAUD        0x0000800u
#define X2RW_D3DPBLENDCAPS_ZERO            0x00000001u
#define X2RW_D3DPBLENDCAPS_ONE             0x00000002u
#define X2RW_D3DPBLENDCAPS_SRCCOLOR        0x00000004u
#define X2RW_D3DPBLENDCAPS_INVSRCCOLOR     0x00000008u
#define X2RW_D3DPBLENDCAPS_SRCALPHA        0x00000010u
#define X2RW_D3DPBLENDCAPS_INVSRCALPHA     0x00000020u
#define X2RW_D3DPBLENDCAPS_DESTALPHA       0x00000040u
#define X2RW_D3DPBLENDCAPS_INVDESTALPHA    0x00000080u
#define X2RW_D3DPBLENDCAPS_DESTCOLOR       0x00000100u
#define X2RW_D3DPBLENDCAPS_INVDESTCOLOR    0x00000200u
#define X2RW_D3DPBLENDCAPS_SRCALPHASAT     0x00000400u
#define X2RW_D3DPTEXTURECAPS_PERSPECTIVE   0x00000001u
#define X2RW_D3DPTEXTURECAPS_POW2          0x00000002u
#define X2RW_D3DPTEXTURECAPS_ALPHA         0x00000004u
#define X2RW_D3DPTEXTURECAPS_ALPHAPALETTE  0x00000080u
#define X2RW_D3DPTEXTURECAPS_NONPOW2CONDITIONAL 0x00000100u
#define X2RW_D3DPTFILTERCAPS_NEAREST       0x00000001u
#define X2RW_D3DPTFILTERCAPS_LINEAR        0x00000002u
#define X2RW_D3DPTFILTERCAPS_MIPNEAREST    0x00000004u
#define X2RW_D3DPTFILTERCAPS_MIPLINEAR     0x00000008u
#define X2RW_D3DPTFILTERCAPS_LINEARMIPNEAREST 0x00000010u
#define X2RW_D3DPTFILTERCAPS_LINEARMIPLINEAR  0x00000020u
/* Comparação (ZCmpCaps/AlphaCmpCaps): os 8 comparadores do D3DCMP_*.
 * Valores de d3d8caps.h (D3DPCMPCAPS_NEVER 0x01 ... D3DPCMPCAPS_ALWAYS 0x80). */
#define X2RW_CMP_CAPS_ALL                  0x000000FFu
/* Stencil: as 8 operações (D3DSTENCILCAPS_KEEP 0x01 ... DECR 0x80). */
#define X2RW_STENCIL_CAPS_ALL              0x000000FFu
/* Linhas: TEXTURE 0x01 | ZTEST 0x02 | BLEND 0x04 | ALPHACMP 0x08 | FOG 0x10. */
#define X2RW_LINECAPS_ALL                  0x0000001Fu
/* FVF: 8 conjuntos de coordenadas de textura + PSIZE (0x100000).
 * d3d8caps.h: TEXCOORDCOUNTMASK 0x00FFFF, PSIZE 0x100000. */
#define X2RW_FVF_CAPS                      (0x00010000u | 0x00100000u)
/* VertexProcessingCaps: TEXGEN 0x01 | MATERIALSOURCE7 0x02 |
 * DIRECTIONALLIGHTS 0x08 | POSITIONALLIGHTS 0x10 | LOCALVIEWER 0x20. */
#define X2RW_VTXPCAPS                      0x0000003Bu
/* PrimitiveMiscCaps: MASKZ 0x02 | CULLNONE 0x10 | CULLCW 0x20 |
 * CULLCCW 0x40 | COLORWRITEENABLE 0x80 | TSSARGTEMP 0x400 | BLENDOP 0x800. */
#define X2RW_MISCCAPS                      0x00000CF2u
/* PresentationIntervals: IMMEDIATE 0x80000000 | ONE 0x1. */
#define X2RW_PRESENT_INTERVALS             0x80000001u
#define X2RW_D3DPTADDRESSCAPS_WRAP         0x00000001u
#define X2RW_D3DPTADDRESSCAPS_MIRROR       0x00000002u
#define X2RW_D3DPTADDRESSCAPS_CLAMP        0x00000004u
#define X2RW_D3DPTADDRESSCAPS_BORDER       0x00000008u
#define X2RW_D3DPTADDRESSCAPS_INDEPENDENTUV 0x00000010u
#define X2RW_D3DPTADDRESSCAPS_MIRRORONCE   0x00000020u

/* sizeof(D3DCAPS8) no SDK 8.1 — ver a explicação acima (0xD4). */
#define X2RW_CAPS8_SIZE  212u

static const uint32_t X2RW_BLEND_CAPS_ALL =
    X2RW_D3DPBLENDCAPS_ZERO | X2RW_D3DPBLENDCAPS_ONE |
    X2RW_D3DPBLENDCAPS_SRCCOLOR | X2RW_D3DPBLENDCAPS_INVSRCCOLOR |
    X2RW_D3DPBLENDCAPS_SRCALPHA | X2RW_D3DPBLENDCAPS_INVSRCALPHA |
    X2RW_D3DPBLENDCAPS_DESTALPHA | X2RW_D3DPBLENDCAPS_INVDESTALPHA |
    X2RW_D3DPBLENDCAPS_DESTCOLOR | X2RW_D3DPBLENDCAPS_INVDESTCOLOR |
    X2RW_D3DPBLENDCAPS_SRCALPHASAT;

static const uint32_t X2RW_TEXFILTER_CAPS =
    X2RW_D3DPTFILTERCAPS_NEAREST | X2RW_D3DPTFILTERCAPS_LINEAR |
    X2RW_D3DPTFILTERCAPS_MIPNEAREST | X2RW_D3DPTFILTERCAPS_MIPLINEAR |
    X2RW_D3DPTFILTERCAPS_LINEARMIPNEAREST |
    X2RW_D3DPTFILTERCAPS_LINEARMIPLINEAR;

/* Zera a D3DCAPS8 no guest e preenche os campos da tabela de offsets. */
static void x2rw_fill_caps8(x2rw_ctx *c, uint32_t caps) {
    for (uint32_t i = 0; i < X2RW_CAPS8_SIZE; i += 4u)
        x2rw_write32(c, caps + i, 0u);

    x2rw_write32(c, caps + 0u,  X2RW_D3DDEVTYPE_HAL);   /* DeviceType     */
    x2rw_write32(c, caps + 4u,  0u);                    /* AdapterOrdinal */
    /* Caps: nada declarado — os bits aqui são "recursos especiais" que o
     * renderizador real deve anunciar, não nós. */
    x2rw_write32(c, caps + 12u, X2RW_D3DCAPS2_CANRENDERWINDOWED |
                                X2RW_D3DCAPS2_FULLSCREENGAMMA);   /* Caps2 */
    x2rw_write32(c, caps + 20u, X2RW_PRESENT_INTERVALS);/* PresentationIntervals */
    x2rw_write32(c, caps + 24u, 0x00000001u);           /* CursorCaps: COLOR */
    x2rw_write32(c, caps + 28u, X2RW_D3DDEVCAPS_HWTRANSFORMANDLIGHT |
                                X2RW_D3DDEVCAPS_HWRASTERIZATION |
                                X2RW_D3DDEVCAPS_DRAWPRIMITIVES2 |
                                X2RW_D3DDEVCAPS_DRAWPRIMTLVERTEX |
                                X2RW_D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
                                X2RW_D3DDEVCAPS_EXECUTESYSTEMMEMORY);

    x2rw_write32(c, caps + 32u, X2RW_MISCCAPS);         /* PrimitiveMiscCaps */
    x2rw_write32(c, caps + 36u, X2RW_D3DPRASTERCAPS_DITHER |
                                X2RW_D3DPRASTERCAPS_ZTEST |
                                X2RW_D3DPRASTERCAPS_FOGVERTEX |
                                X2RW_D3DPRASTERCAPS_FOGTABLE |
                                X2RW_D3DPRASTERCAPS_MIPMAPLODBIAS |
                                X2RW_D3DPRASTERCAPS_ZBIAS |
                                X2RW_D3DPRASTERCAPS_FOGRANGE |
                                X2RW_D3DPRASTERCAPS_ANISOTROPY |
                                X2RW_D3DPRASTERCAPS_WFOG |
                                X2RW_D3DPRASTERCAPS_ZFOG |
                                X2RW_D3DPRASTERCAPS_COLORPERSPECTIVE |
                                X2RW_D3DPRASTERCAPS_DEPTHBIAS |
                                X2RW_D3DPRASTERCAPS_TEXTUREPERSPECTIVE);
    x2rw_write32(c, caps + 40u, X2RW_CMP_CAPS_ALL);     /* ZCmpCaps      */
    x2rw_write32(c, caps + 44u, X2RW_BLEND_CAPS_ALL);   /* SrcBlendCaps  */
    x2rw_write32(c, caps + 48u, X2RW_BLEND_CAPS_ALL);   /* DestBlendCaps */
    x2rw_write32(c, caps + 52u, X2RW_CMP_CAPS_ALL);     /* AlphaCmpCaps  */
    x2rw_write32(c, caps + 56u, X2RW_D3DPSHADECAPS_COLORGOURAUDRGB |
                                X2RW_D3DPSHADECAPS_ALPHAGOURAUDBLEND |
                                X2RW_D3DPSHADECAPS_FOGGOURAUD |
                                X2RW_D3DPSHADECAPS_SPECULARGOURAUDRGB);
    x2rw_write32(c, caps + 60u, X2RW_D3DPTEXTURECAPS_PERSPECTIVE |
                                X2RW_D3DPTEXTURECAPS_POW2 |
                                X2RW_D3DPTEXTURECAPS_ALPHA |
                                X2RW_D3DPTEXTURECAPS_ALPHAPALETTE |
                                X2RW_D3DPTEXTURECAPS_NONPOW2CONDITIONAL);
    x2rw_write32(c, caps + 64u, X2RW_TEXFILTER_CAPS);   /* TextureFilterCaps */
    x2rw_write32(c, caps + 68u, X2RW_TEXFILTER_CAPS);   /* CubeTextureFilterCaps */
    x2rw_write32(c, caps + 72u, X2RW_TEXFILTER_CAPS);   /* VolumeTextureFilterCaps */
    x2rw_write32(c, caps + 76u, X2RW_D3DPTADDRESSCAPS_WRAP |
                                X2RW_D3DPTADDRESSCAPS_MIRROR |
                                X2RW_D3DPTADDRESSCAPS_CLAMP |
                                X2RW_D3DPTADDRESSCAPS_BORDER |
                                X2RW_D3DPTADDRESSCAPS_MIRRORONCE);
    x2rw_write32(c, caps + 80u, X2RW_D3DPTADDRESSCAPS_WRAP |
                                X2RW_D3DPTADDRESSCAPS_MIRROR |
                                X2RW_D3DPTADDRESSCAPS_CLAMP |
                                X2RW_D3DPTADDRESSCAPS_BORDER |
                                X2RW_D3DPTADDRESSCAPS_MIRRORONCE);
    x2rw_write32(c, caps + 84u, X2RW_LINECAPS_ALL);     /* LineCaps      */
    x2rw_write32(c, caps + 88u, 2048u);                 /* MaxTextureWidth  */
    x2rw_write32(c, caps + 92u, 2048u);                 /* MaxTextureHeight */
    x2rw_write32(c, caps + 96u, 256u);                  /* MaxVolumeExtent  */
    x2rw_write32(c, caps + 100u, 8192u);                /* MaxTextureRepeat */
    x2rw_write32(c, caps + 104u, 2048u);                /* MaxTextureAspectRatio */
    x2rw_write32(c, caps + 108u, 16u);                  /* MaxAnisotropy    */
    x2rw_write32(c, caps + 136u, X2RW_STENCIL_CAPS_ALL);/* StencilCaps      */
    x2rw_write32(c, caps + 140u, X2RW_FVF_CAPS);        /* FVFCaps          */
    x2rw_write32(c, caps + 144u, 0u);                   /* TextureOpCaps    */
    x2rw_write32(c, caps + 148u, 8u);                   /* MaxTextureBlendStages */
    x2rw_write32(c, caps + 152u, 8u);                   /* MaxSimultaneousTextures */
    x2rw_write32(c, caps + 156u, X2RW_VTXPCAPS);        /* VertexProcessingCaps */
    x2rw_write32(c, caps + 160u, 8u);                   /* MaxActiveLights  */
    x2rw_write32(c, caps + 164u, 1u);                   /* MaxUserClipPlanes */
    x2rw_write32(c, caps + 168u, 4u);                   /* MaxVertexBlendMatrices */
    x2rw_write32(c, caps + 172u, 0u);                   /* MaxVertexBlendMatrixIndex
                                                        * (0 = o índice da matriz
                                                        *  vai por stream, como
                                                        *  num VS 1.1 típico)   */
    x2rw_write32(c, caps + 176u, 0x42800000u);          /* MaxPointSize = 64.0f */
    x2rw_write32(c, caps + 180u, 0xFFFFFu);             /* MaxPrimitiveCount */
    x2rw_write32(c, caps + 184u, 0xFFFFu);              /* MaxVertexIndex   */
    x2rw_write32(c, caps + 188u, 16u);                  /* MaxStreams       */
    x2rw_write32(c, caps + 192u, 255u);                 /* MaxStreamStride  */
    x2rw_write32(c, caps + 196u, 0xFFFE0101u);          /* VertexShaderVersion  */
    x2rw_write32(c, caps + 200u, 96u);                  /* MaxVertexShaderConst */
    x2rw_write32(c, caps + 204u, 0xFFFF0104u);          /* PixelShaderVersion   */
    x2rw_write32(c, caps + 208u, 0x3F800000u);          /* MaxPixelShaderValue = 1.0f */
}

/* ------------------------------------------------------------------ */
/* D3DADAPTER_IDENTIFIER8                                             */
/* ------------------------------------------------------------------ */
/* Layout (4-byte packed, 1608 bytes):
 *      0  Driver[512]
 *    512  Description[512]
 *   1024  DriverVersion (LARGE_INTEGER, 8 bytes)
 *   1032  VendorId   1036  DeviceId   1040  SubSysId   1044  Revision
 *   1048  DeviceIdentifier (GUID, 16 bytes)
 *   1064  WHQLLevel
 * O driver/descrição abaixo são do ADAPTADOR SINTÉTICO do port — não são
 * o hardware real. VendorId/DeviceId são do intervalo "Microsoft
 * software device" (0x1414/0x0001), que é o nome honesto para "não é um
 * adaptador de hardware". */

static void x2rw_fill_adapter_identifier(x2rw_ctx *c, uint32_t ai) {
    const char *driver = "X2WR-Vulkan/GLES";
    const char *desc   = "X2WR synthetic adapter (Android port)";

    for (uint32_t i = 0; i < 512u; i++) {
        char ch = (i < strlen(driver)) ? driver[i] : '\0';
        x2rw_write8(c, ai + i, (uint8_t)ch);
    }
    for (uint32_t i = 0; i < 512u; i++) {
        char ch = (i < strlen(desc)) ? desc[i] : '\0';
        x2rw_write8(c, ai + 512u + i, (uint8_t)ch);
    }

    x2rw_write32(c, ai + 1024u, 0x00010000u);   /* DriverVersion LowPart  */
    x2rw_write32(c, ai + 1028u, 0x00000001u);   /* DriverVersion HighPart */
    x2rw_write32(c, ai + 1032u, 0x00001414u);   /* VendorId               */
    x2rw_write32(c, ai + 1036u, 0x00000001u);   /* DeviceId               */
    x2rw_write32(c, ai + 1040u, 0x00000000u);   /* SubSysId               */
    x2rw_write32(c, ai + 1044u, 0x00000000u);   /* Revision               */
    /* DeviceIdentifier (GUID): zeros = "sem GUID de driver de hardware". */
    for (uint32_t i = 0; i < 16u; i++) x2rw_write8(c, ai + 1048u + i, 0u);
    x2rw_write32(c, ai + 1064u, 0u);            /* WHQLLevel              */
}

/* ------------------------------------------------------------------ */
/* Métodos do IDirect3D8 (args[0] é o `this`, como em todo COM)       */
/* ------------------------------------------------------------------ */

static uint32_t d3d_QueryInterface(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t riid = (nargs > 1) ? args[1] : 0u;
    uint32_t ppv  = (nargs > 2) ? args[2] : 0u;

    if (!ppv) {
        x2rw_log("[d3d8] IDirect3D8::QueryInterface: ppvObject nulo");
        return X2RW_E_NOINTERFACE;
    }
    if (riid && (x2rw_guid_eq(c, riid, x2rw_iid_idirect3d8) ||
                 x2rw_guid_eq(c, riid, x2rw_iid_iunknown))) {
        x2rw_write32(c, ppv, args[0]);
        return X2RW_D3D_OK;
    }
    x2rw_log("[d3d8] IDirect3D8::QueryInterface: IID não suportado");
    x2rw_write32(c, ppv, 0u);
    return X2RW_E_NOINTERFACE;
}

static uint32_t d3d_AddRef(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c; (void)args; (void)nargs;
    return 1u;      /* refcount sintético */
}

static uint32_t d3d_Release(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)args; (void)nargs;
    /* O objeto é do heap do guest; a vtable é estática (registrada uma vez
     * em g_com[] do despachante), então liberamos só o objeto. */
    if (g_d3d8_obj) {
        x2rw_heap_free(c, g_d3d8_obj);
        x2rw_log("[d3d8] IDirect3D8::Release: objeto %08x liberado",
                 g_d3d8_obj);
        g_d3d8_obj = 0u;
    }
    return 0u;
}

/* RegisterSoftwareDevice(THIS, pfnInitialize): o adaptador de software
 * (o rasterizador RGB do D3D8) não existe neste port. Aceitar o registro
 * e não fazer nada é o stub benigno: o jogo não usa esse adaptador (cria
 * HAL) e a chamada não tem efeito colateral. */
static uint32_t d3d_RegisterSoftwareDevice(x2rw_ctx *c, uint32_t *args,
                                           int nargs) {
    (void)c;
    x2rw_log("[d3d8] IDirect3D8::RegisterSoftwareDevice(%08x) ignorado "
             "(stub benigno: não há adaptador de software no port)",
             (nargs > 1) ? args[1] : 0u);
    return X2RW_D3D_OK;
}

static uint32_t d3d_GetAdapterCount(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c; (void)args; (void)nargs;
    return 1u;      /* um adaptador sintético (o único que existe) */
}

static uint32_t d3d_GetAdapterIdentifier(x2rw_ctx *c, uint32_t *args,
                                         int nargs) {
    uint32_t adapter = (nargs > 1) ? args[1] : 0u;
    uint32_t ai      = (nargs > 2) ? args[2] : 0u;

    if (adapter != X2RW_D3DADAPTER_DEFAULT || !ai) {
        x2rw_log("[d3d8] GetAdapterIdentifier: adapter %u / out %08x "
                 "inválidos", adapter, ai);
        return X2RW_D3DERR_INVALIDCALL;
    }
    x2rw_fill_adapter_identifier(c, ai);
    return X2RW_D3D_OK;
}

static uint32_t d3d_EnumAdapterModes(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t adapter = (nargs > 1) ? args[1] : 0u;
    uint32_t mode    = (nargs > 2) ? args[2] : 0u;
    uint32_t pMode   = (nargs > 3) ? args[3] : 0u;

    if (adapter != X2RW_D3DADAPTER_DEFAULT || !pMode) {
        return X2RW_D3DERR_INVALIDCALL;
    }
    if (mode >= (uint32_t)X2RW_N_MODES) {
        /* Fim da enumeração — o jogo espera exatamente este erro para
         * parar de listar modos. */
        return X2RW_D3DERR_INVALIDCALL;
    }
    uint32_t w, h;
    x2rw_mode_dims((int)mode, &w, &h);
    x2rw_write_display_mode(c, pMode, w, h, 60u);
    return X2RW_D3D_OK;
}

static uint32_t d3d_GetAdapterDisplayMode(x2rw_ctx *c, uint32_t *args,
                                          int nargs) {
    uint32_t adapter = (nargs > 1) ? args[1] : 0u;
    uint32_t pMode   = (nargs > 2) ? args[2] : 0u;

    if (adapter != X2RW_D3DADAPTER_DEFAULT || !pMode)
        return X2RW_D3DERR_INVALIDCALL;

    /* O modo corrente é a resolução de render do port (o launcher Android
     * a escreve em g_render_w/g_render_h). RefreshRate = 0 (janela). */
    uint32_t w = (uint32_t)((g_render_w > 0) ? g_render_w : 1280);
    uint32_t h = (uint32_t)((g_render_h > 0) ? g_render_h : 720);
    x2rw_write_display_mode(c, pMode, w, h, 0u);
    return X2RW_D3D_OK;
}

static uint32_t d3d_GetAdapterModeCount(x2rw_ctx *c, uint32_t *args,
                                        int nargs) {
    uint32_t adapter = (nargs > 1) ? args[1] : 0u;
    (void)c;
    if (adapter != X2RW_D3DADAPTER_DEFAULT) return 0u;
    return (uint32_t)X2RW_N_MODES;      /* mesma lista de EnumAdapterModes */
}

static uint32_t d3d_CheckDeviceType(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t adapter   = (nargs > 1) ? args[1] : 0u;
    uint32_t devtype   = (nargs > 2) ? args[2] : 0u;
    uint32_t adformat  = (nargs > 3) ? args[3] : 0u;
    uint32_t backfmt   = (nargs > 4) ? args[4] : 0u;
    (void)c;

    if (adapter != X2RW_D3DADAPTER_DEFAULT) return X2RW_D3DERR_NOTAVAILABLE;
    /* HAL e REF: o adaptador sintético aceita os dois (o REF é o caminho
     * de fallback que o jogo tenta quando o HAL falha). SW não existe. */
    if (devtype != X2RW_D3DDEVTYPE_HAL && devtype != X2RW_D3DDEVTYPE_REF)
        return X2RW_D3DERR_NOTAVAILABLE;
    if (!x2rw_is_display_format(adformat))
        return X2RW_D3DERR_NOTAVAILABLE;
    /* O back buffer tem de ser um formato de exibição com alfa ou X. */
    if (!x2rw_is_display_format(backfmt))
        return X2RW_D3DERR_NOTAVAILABLE;
    return X2RW_D3D_OK;
}

/* CheckDeviceFormat(THIS, Adapter, DeviceType, AdapterFormat, Usage,
 *                   RType, CheckFormat)
 *
 * Aceitamos qualquer formato: quem decide de verdade o que a GPU suporta
 * é o renderizador instalado, não esta tabela. O jogo usa esta resposta
 * para montar a matriz de formatos de textura/render target e vem aqui
 * com D3DUSAGE_* variados; recusar por não conhecer o formato faria o
 * jogo desabilitar Efeitos e sombras sem necessidade. */
static uint32_t d3d_CheckDeviceFormat(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c; (void)args; (void)nargs;
    return X2RW_D3D_OK;
}

static uint32_t d3d_CheckDeviceMultiSampleType(x2rw_ctx *c, uint32_t *args,
                                               int nargs) {
    (void)c;
    /* Só o modo sem multisample (D3DMULTISAMPLE_NONE = 0) é aceito: o
     * adaptador sintético não faz AA. O jogo pergunta e usa isto para
     * desabilitar AA — resposta honesta. */
    for (int i = 0; i < nargs; i++) {
        if (i == 5) {   /* MultiSampleType */
            if (args[i] != 0u) return X2RW_D3DERR_NOTAVAILABLE;
        }
    }
    return X2RW_D3D_OK;
}

static uint32_t d3d_CheckDepthStencilMatch(x2rw_ctx *c, uint32_t *args,
                                           int nargs) {
    (void)c; (void)args; (void)nargs;
    /* Pares de formato de depth/stencil combinam com os alvos do port:
     * D16/D24X8/D24S8/D32 estão todos na lista de formatos do render. */
    return X2RW_D3D_OK;
}

static uint32_t d3d_GetDeviceCaps(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t adapter = (nargs > 1) ? args[1] : 0u;
    uint32_t devtype = (nargs > 2) ? args[2] : 0u;
    uint32_t pCaps   = (nargs > 3) ? args[3] : 0u;

    if (!pCaps) return X2RW_D3DERR_INVALIDCALL;
    if (adapter != X2RW_D3DADAPTER_DEFAULT) return X2RW_D3DERR_NOTAVAILABLE;
    if (devtype != X2RW_D3DDEVTYPE_HAL && devtype != X2RW_D3DDEVTYPE_REF)
        return X2RW_D3DERR_INVALIDDEVICE;
    x2rw_fill_caps8(c, pCaps);
    return X2RW_D3D_OK;
}

/* GetAdapterMonitor(THIS, Adapter) -> HMONITOR.
 *
 * O jogo usa o monitor só como handle opaco (nunca chama GetMonitorInfo
 * através dele em D3D8). Devolvemos um pseudo-handle não nulo, estável
 * por adaptador; 0 seria "falhou" e faria o jogo desistir do adaptador. */
static uint32_t d3d_GetAdapterMonitor(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c;
    uint32_t adapter = (nargs > 1) ? args[1] : 0u;
    if (adapter != X2RW_D3DADAPTER_DEFAULT) return 0u;
    return X2RW_D3D_MONITOR_PSEUDO;
}

/* CreateDevice(THIS, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
 *              pPresentationParameters, ppReturnedDeviceInterface)
 *
 * A única coisa deste arquivo que NÃO é sintética. Sem hook de
 * renderizador instalado devolvemos D3DERR_NOTAVAILABLE e logamos; um
 * device falso faria o jogo seguir e quebrar lá adiante, longe da causa.
 * Com hook, o hook cria o objeto do device (VA do guest) e devolve em
 * *out_device; nós o escrevemos em ppReturnedDeviceInterface. */
static uint32_t d3d_CreateDevice(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t adapter   = (nargs > 1) ? args[1] : 0u;
    uint32_t devtype   = (nargs > 2) ? args[2] : 0u;
    uint32_t hwnd      = (nargs > 3) ? args[3] : 0u;
    uint32_t flags     = (nargs > 4) ? args[4] : 0u;
    uint32_t present   = (nargs > 5) ? args[5] : 0u;   /* VA do guest */
    uint32_t out_dev   = (nargs > 6) ? args[6] : 0u;

    if (!out_dev) return X2RW_D3DERR_INVALIDCALL;
    if (adapter != X2RW_D3DADAPTER_DEFAULT) return X2RW_D3DERR_INVALIDCALL;
    if (!present) {
        x2rw_log("[d3d8] CreateDevice: pPresentationParameters nulo");
        return X2RW_D3DERR_INVALIDCALL;
    }

    const x2rw_d3d8_hooks *h = x2rw_d3d8_get_hooks();
    if (!h || !h->create_device) {
        /* SEM RENDERIZADOR INSTALADO: não há como devolver um device. */
        x2rw_log("[d3d8] CreateDevice: NENHUM RENDERIZADOR INSTALADO "
                 "(x2rw_d3d8_set_hooks não foi chamado) — devolvendo "
                 "D3DERR_NOTAVAILABLE em vez de um device falso "
                 "(adapter=%u type=%u hwnd=%08x flags=%08x present=%08x)",
                 adapter, devtype, hwnd, flags, present);
        return X2RW_D3DERR_NOTAVAILABLE;
    }

    uint32_t dev = 0u;
    int rc = h->create_device(c, args[0], adapter, devtype, hwnd, flags,
                              present, &dev);
    if (rc != 0 || dev == 0u) {
        x2rw_log("[d3d8] CreateDevice: hook falhou (rc=%d, device=%08x)",
                 rc, dev);
        return X2RW_D3DERR_NOTAVAILABLE;
    }
    x2rw_write32(c, out_dev, dev);
    x2rw_log("[d3d8] CreateDevice: device do renderizador em %08x", dev);
    return X2RW_D3D_OK;
}

/* ------------------------------------------------------------------ */
/* Vtable e criação do objeto                                         */
/* ------------------------------------------------------------------ */

/* Ordem === ordem do SDK (ver o cabeçalho do arquivo). Um handler por
 * slot, com o número TOTAL de argumentos de 4 bytes incluindo o `this`
 * em args[0] — é o que x2rw_com_register() precisa (ver x2rw_audio.c). */
typedef struct {
    x2rw_api_t fn;
    int        nargs;
} x2rw_d3d_slot;

static const x2rw_d3d_slot x2rw_d3d_vtable[] = {
    { d3d_QueryInterface,             3 },   /* 0  */
    { d3d_AddRef,                     1 },   /* 1  */
    { d3d_Release,                    1 },   /* 2  */
    { d3d_RegisterSoftwareDevice,     2 },   /* 3  */
    { d3d_GetAdapterCount,            1 },   /* 4  */
    { d3d_GetAdapterIdentifier,       4 },   /* 5  */
    { d3d_GetAdapterModeCount,        2 },   /* 6  */
    { d3d_EnumAdapterModes,           4 },   /* 7  */
    { d3d_GetAdapterDisplayMode,      3 },   /* 8  */
    { d3d_CheckDeviceType,            6 },   /* 9  */
    { d3d_CheckDeviceFormat,          7 },   /* 10 */
    { d3d_CheckDeviceMultiSampleType, 6 },   /* 11 */
    { d3d_CheckDepthStencilMatch,     6 },   /* 12 */
    { d3d_GetDeviceCaps,              4 },   /* 13 */
    { d3d_GetAdapterMonitor,          2 },   /* 14 */
    { d3d_CreateDevice,               7 }    /* 15 */
};

#define X2RW_D3D_VTBL_N \
    ((int)(sizeof(x2rw_d3d_vtable) / sizeof(x2rw_d3d_vtable[0])))

/* (o objeto corrente está declarado no topo como static g_d3d8_obj) */

/* Registra os thunks uma única vez (x2rw_com_register reaproveita por
 * função+nargs de qualquer forma) e cria um objeto novo no heap do guest
 * com a vtable apontando para eles. */
static uint32_t d3d_object_create(x2rw_ctx *c) {
    static uint32_t vtbl_vas[X2RW_D3D_VTBL_N];
    static int ready = 0;

    if (!ready) {
        for (int i = 0; i < X2RW_D3D_VTBL_N; i++) {
            vtbl_vas[i] = x2rw_com_register(x2rw_d3d_vtable[i].fn,
                                            x2rw_d3d_vtable[i].nargs);
            if (vtbl_vas[i] == 0u) {
                x2rw_log("[d3d8] ERRO: thunk COM nulo no slot %d de "
                         "IDirect3D8 (tabela de thunks esgotada?)", i);
                return 0u;
            }
        }
        ready = 1;
    }

    uint32_t obj = x2rw_com_object_create(c, vtbl_vas,
                                          (uint32_t)X2RW_D3D_VTBL_N);
    if (!obj) {
        x2rw_log("[d3d8] ERRO: x2rw_com_object_create falhou "
                 "(heap do guest sem memória?)");
        return 0u;
    }
    return obj;
}

/* ------------------------------------------------------------------ */
/* Direct3DCreate8 — o único export de d3d8.dll                           */
/* ------------------------------------------------------------------ */

/* Direct3DCreate8(SDKVersion) -> IDirect3D8*
 *
 * SDKVersion esperado: 0x220 (D3D_SDK_VERSION do D3D8.1). Não validamos o
 * valor: o jogo é a única origem da chamada e o valor correto está no
 * binário; recusar por versão só criaria um modo de falha a mais.
 *
 * Devolve o VA do objeto no guest (ou 0 em falha de memória). O objeto
 * corrente é liberado antes de criar o novo — Direct3DCreate8 chamado
 * duas vezes não deve vazar heap do guest. */
uint32_t d3d_Direct3DCreate8(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t sdk = (nargs > 0) ? args[0] : 0u;

    if (g_d3d8_obj) {
        x2rw_log("[d3d8] Direct3DCreate8 chamado de novo; liberando o "
                 "objeto anterior %08x", g_d3d8_obj);
        x2rw_heap_free(c, g_d3d8_obj);
        g_d3d8_obj = 0u;
    }

    uint32_t obj = d3d_object_create(c);
    if (!obj) {
        x2rw_log("[d3d8] Direct3DCreate8(sdk=%08x): FALHOU ao criar o "
                 "objeto IDirect3D8", sdk);
        X2RW_FAIL(c, X2RW_ERROR_OUTOFMEMORY);
        return 0u;
    }
    g_d3d8_obj = obj;

    x2rw_log("[d3d8] Direct3DCreate8(sdk=%08x) -> IDirect3D8 em %08x "
             "(vtable com %d métodos, adaptador sintético)",
             sdk, obj, X2RW_D3D_VTBL_N);

    /* Aviso de instalação: o objeto existe, mas sem hook nenhum device
     * poderá ser criado. É melhor dizer isto agora do que deixar o jogo
     * descobrir no CreateDevice. */
    if (!x2rw_d3d8_get_hooks())
        x2rw_log("[d3d8] AVISO: nenhum renderizador instalado "
                 "(x2rw_d3d8_set_hooks) — CreateDevice devolverá "
                 "D3DERR_NOTAVAILABLE");

    const x2rw_d3d8_hooks *h = x2rw_d3d8_get_hooks();
    if (h && h->init) {
        if (h->init(c, obj) != 0)
            x2rw_log("[d3d8] hook init devolveu erro para o objeto %08x",
                     obj);
    }

    X2RW_OK();
    return obj;
}
