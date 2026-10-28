/* X2: Wolverine's Revenge - port Android
 * ============================================================================
 * x2rw_audio.c — shims de ÁUDIO: DSOUND.dll + WINMM.dll.
 *
 * O QUE ESTE ARQUIVO FAZ
 * ----------------------
 *   1. DirectSoundCreate (import por ORDINAL 11 de DSOUND.dll — ver
 *      x2rw_import_lookup_ordinal() em x2rw_imports.c e docs/RECON.md §12).
 *      Cria um objeto COM SINTÉTICO cuja vtable vive na memória do guest e
 *      cujos slots apontam para thunks de x2rw_com_register(); o jogo chama
 *      IDirectSound::CreateSoundBuffer => IDirectSoundBuffer::Lock/Play/... e
 *      nunca precisa saber que não existe dsound.dll.
 *   2. waveOut* — o loop de áudio do jogo (e do Bink, que importa waveOutOpen/
 *      waveOutWrite de WINMM) avança porque marcamos WHDR_DONE na WAVEHDR.
 *   3. mmio* — abertura/leitura de WAV dentro dos containers (.pkr) por
 *      caminho convertido para POSIX e FILE* do host, guardado numa tabela
 *      indexada por handle sintético.
 *   4. timeGetTime — relógio monotônico em ms.
 *
 * REGRA DE OURO (headless)
 * ------------------------
 * Nenhum som sai deste módulo. Quem toca é o hook `x2rw_audio_hooks`
 * (SDL2/AAudio), instalado por outro módulo. Se `x2rw_audio_get_hooks()`
 * devolver NULL o modo é HEADLESS: DirectSoundCreate/waveOutOpen continuam
 * devolvendo SUCESSO e handles sintéticos (o jogo PRECISA avançar), os
 * buffers de Lock são alocados no heap do guest (o jogo escreve as amostras
 * neles — o buffer é real, o que falta é o destino), e waveOutWrite marca a
 * WAVEHDR como concluída para o loop não travar esperando o hardware.
 *
 * Cada handler registra o que fez. Um handler que devolvesse sucesso sem
 * preencher a struct pedida faria o jogo ler lixo — por isso o valor
 * sintético é sempre escrito explicitamente.
 *
 * CONVENÇÃO stdcall (não mexer)
 * -----------------------------
 * O despachante (invoke_from_stack) já faz c->r.esp += 4*nargs. NENHUM
 * handler deste arquivo toca em c->r.esp.
 *
 * NOTA SOBRE A LISTA MESTRA
 * -------------------------
 * A lista `X2RW_SHIMS_AUDIO(X)` em x2rw_shims.h tem 17 símbolos: é a
 * superfície REALMENTE importada pelos dois binários do jogo (medida em
 * gen/Wolvie_imports.h e gen/binkw32_imports.h). APIs de áudio citadas no
 * contrato mas ausentes da lista (waveOutGetNumDevs, waveOutGetDevCapsA,
 * waveOutGetPosition, mmioWrite, mmioSeek, mmioGetInfo, mmioSetInfo) NÃO
 * estão registradas aqui porque o jogo não as importa — um handler sem
 * chamador é código morto que não passa pelo verificador do contrato.
 * ============================================================================
 */
/* clock_gettime/CLOCK_MONOTONIC são POSIX: pedir explicitamente em vez de
 * depender da flag do build (_GNU_SOURCE já vem do CMake, mas o arquivo
 * precisa compilar também com -std=c11 puro). Precisa vir ANTES de todo
 * include. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "x2rw_runtime.h"
#include "x2rw_shims.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Constantes de áudio                                                */
/* ------------------------------------------------------------------ */

/* HRESULT/MMSYSERR de sucesso. DS_OK == S_OK == MMSYSERR_NOERROR == 0. */
#define AUD_OK 0u

/* WHDR_* (mmsystem.h) */
#define AUD_WHDR_DONE       0x00000001u
#define AUD_WHDR_PREPARED   0x00000002u
#define AUD_WHDR_BEGINLOOP  0x00000004u
#define AUD_WHDR_ENDLOOP    0x00000008u
#define AUD_WHDR_INQUEUE    0x00000010u

/* WAVEHDR: offsets dentro da struct do guest (32 bits). */
#define AUD_WAVEHDR_FLAGS   0x10u   /* DWORD dwFlags */

/* WAVEFORMATEX: 18 bytes. */
#define AUD_WF_SIZE         18u

/* Tag de formato PCM — o único que o jogo usa nos .wav originais. */
#define AUD_WAVE_FORMAT_PCM 1u

/* DSCAPS.dwSize é o primeiro campo; não o validamos. */
#define AUD_WAVE_MAPPER     0xFFFFFFFFu

/* Limites da tabela de arquivos do mmio (handle sintético = índice+1). */
#define AUD_MAX_FILES 32

/* ------------------------------------------------------------------ */
/* Registro de log                                                    */
/* ------------------------------------------------------------------ */

/* Recursos do "dispositivo" observados no startup. Ficam em static para o
 * resumo final; nenhum handler depende deles para decidir o que devolver. */
static int      g_aud_ds_created = 0;      /* objetos IDirectSound criados   */
static int      g_aud_buffers    = 0;      /* buffers criados via COM        */
static uint32_t g_aud_out_rate   = 0;      /* nSamplesPerSec do waveOutOpen  */
static int      g_aud_out_ch     = 0;      /* nChannels do waveOutOpen       */
static int      g_aud_writes     = 0;      /* waveOutWrite chamado           */

/* ------------------------------------------------------------------ */
/* 1. Tabela de arquivos do mmio                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    FILE    *fp;
    uint32_t size;   /* tamanho em bytes (posição inicial 0) */
} aud_mmio_file;

/* Handle sintético = índice + 1 (nunca devolvemos 0 = NULL). */
static aud_mmio_file g_aud_files[AUD_MAX_FILES];

static aud_mmio_file *aud_file_get(uint32_t handle) {
    if (handle == 0 || handle > AUD_MAX_FILES) return NULL;
    aud_mmio_file *f = &g_aud_files[handle - 1u];
    return f->fp ? f : NULL;
}

static uint32_t aud_file_new(FILE *fp, uint32_t size) {
    for (uint32_t i = 0; i < AUD_MAX_FILES; i++) {
        if (g_aud_files[i].fp == NULL) {
            g_aud_files[i].fp = fp;
            g_aud_files[i].size = size;
            return i + 1u;
        }
    }
    return 0u;
}

/* ------------------------------------------------------------------ */
/* 2. Vtables COM (uma por classe, reusadas por todas as instâncias)   */
/* ------------------------------------------------------------------ */

/* IDirectSound  — 11 slots:
 *   0 QueryInterface 1 AddRef 2 Release 3 CreateSoundBuffer 4 GetCaps
 *   5 DuplicateSoundBuffer 6 SetCooperativeLevel 7 Compact
 *   8 GetSpeakerConfig 9 SetSpeakerConfig 10 Initialize
 *
 * IDirectSoundBuffer — 21 slots:
 *   0 QueryInterface 1 AddRef 2 Release 3 GetCaps 4 GetCurrentPosition
 *   5 GetFormat 6 GetVolume 7 GetPan 8 GetFrequency 9 GetStatus
 *   10 Initialize 11 Lock 12 Play 13 SetCurrentPosition 14 SetFormat
 *   15 SetVolume 16 SetPan 17 SetFrequency 18 Stop 19 Unlock 20 Restore
 *
 * `x2rw_com_register(fn, nargs)` recebe o número TOTAL de argumentos de 4
 * bytes do método, incluindo o `this` em args[0] — ver invoke_from_stack():
 * ele marshalla a partir de `c->r.esp`, que no stdcall aponta para o `this`.
 * Os parâmetros da API ficam em args[1], args[2], ...
 * Ex.: CreateSoundBuffer(this, desc, out, punkOuter) => nargs = 4 e
 * desc = args[1], out = args[2].
 */

/* --- IUnknown (compartilhado pelas duas classes) ------------------- */

static uint32_t aud_com_query_interface(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c; (void)nargs;
    uint32_t ppv = (nargs > 3) ? args[3] : 0u;
    if (ppv) x2rw_write32(c, ppv, args[0]);   /* devolve o mesmo objeto */
    return AUD_OK;
}

static uint32_t aud_com_add_ref(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c; (void)args; (void)nargs;
    return 1u;      /* refcount sintético: nunca morre antes do jogo sair */
}

static uint32_t aud_com_release(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c; (void)args; (void)nargs;
    /* Não liberamos: os objetos vivem no heap do guest até o processo sair.
     * Devolver 0 é o "refcount depois de soltar" (o jogo não usa o valor). */
    return 0u;
}

/* Registra os 21 slots de IDirectSoundBuffer uma única vez e devolve os VAs
 * dos thunks em `out`. Devolve 0 (e loga) se a tabela COM do despachante
 * esgotar. Declarada aqui porque CreateSoundBuffer/DuplicateSoundBuffer a
 * usam antes da definição. */
static int aud_ds_buffer_vtable(x2rw_ctx *c, uint32_t *out);

/* ------------------------------------------------------------------ */
/* Estado de um IDirectSoundBuffer                                    */
/* ------------------------------------------------------------------ */
/* x2rw_com_object_create() aloca EXATAMENTE 4 + 4*count bytes (o ponteiro
 * de vtable + os slots). Qualquer campo extra do shim tem de morar em OUTRO
 * bloco do heap do guest — escrever depois do último slot corromperia o
 * cabeçalho do bloco seguinte.
 *
 * Layout: 7 words no heap do guest, VA guardado no slot 21 do objeto
 * (self + 84), que fica depois dos 21 slots da vtable do IDirectSoundBuffer.
 *   0 dono (IDirectSound)      1 lpAudio (VA da região)   2 bytes
 *   3 posição do play          4 volume                    5 frequência
 *   6 pan                                                             */
#define AUD_BUF_STATE_WORDS 7u
#define AUD_BUF_STATE_SLOT  21u
#define AUD_BUF_STATE_OFF   4u

enum {
    AUD_ST_OWNER = 0,
    AUD_ST_AUDIO,
    AUD_ST_BYTES,
    AUD_ST_POS,
    AUD_ST_VOL,
    AUD_ST_FREQ,
    AUD_ST_PAN
};

static uint32_t aud_buf_state_slot(uint32_t self) {
    return self + AUD_BUF_STATE_OFF + 4u * AUD_BUF_STATE_SLOT;
}

static uint32_t aud_buf_state(x2rw_ctx *c, uint32_t self) {
    return x2rw_read32(c, aud_buf_state_slot(self));
}

/* Cria (se preciso) e devolve o bloco de estado do buffer, ou 0. */
static uint32_t aud_buf_state_ensure(x2rw_ctx *c, uint32_t self,
                                     uint32_t owner) {
    uint32_t st = aud_buf_state(c, self);
    if (st) return st;
    st = x2rw_heap_alloc(c, 4u * AUD_BUF_STATE_WORDS);
    if (!st) return 0u;
    x2rw_write32(c, st + 4u * AUD_ST_AUDIO, 0u);
    x2rw_write32(c, st + 4u * AUD_ST_BYTES, 0u);
    x2rw_write32(c, st + 4u * AUD_ST_POS,   0u);
    x2rw_write32(c, st + 4u * AUD_ST_VOL,   100u);   /* -10000..0 (0 = máx.) */
    x2rw_write32(c, st + 4u * AUD_ST_FREQ,  44100u);
    x2rw_write32(c, st + 4u * AUD_ST_PAN,   0u);
    x2rw_write32(c, st + 4u * AUD_ST_OWNER, owner);
    x2rw_write32(c, aud_buf_state_slot(self), st);
    return st;
}

static uint32_t aud_buf_get(x2rw_ctx *c, uint32_t self, int field) {
    uint32_t st = aud_buf_state(c, self);
    return st ? x2rw_read32(c, st + 4u * (uint32_t)field) : 0u;
}

static void aud_buf_set(x2rw_ctx *c, uint32_t self, int field, uint32_t v) {
    uint32_t st = aud_buf_state(c, self);
    if (st) x2rw_write32(c, st + 4u * (uint32_t)field, v);
}

/* --- IDirectSound -------------------------------------------------- */

static uint32_t aud_ds_create_sound_buffer(x2rw_ctx *c, uint32_t *args, int nargs) {
    /* args[0] = this. Os argumentos da API vêm DEPOIS dele:
     * desc = args[1] (LPCDSBUFFERDESC), out = args[2] (LPDIRECTSOUNDBUFFER*),
     * args[3] = punkOuter (agregação COM não usada pelo jogo). */
    uint32_t desc = (nargs > 1) ? args[1] : 0u;
    uint32_t out  = (nargs > 2) ? args[2] : 0u;

    if (!out) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }

    uint32_t vtbl[21];
    if (!aud_ds_buffer_vtable(c, vtbl)) {
        X2RW_FAIL(c, X2RW_ERROR_OUTOFMEMORY);
        return 0u;
    }

    const x2rw_audio_hooks *h = x2rw_audio_get_hooks();
    uint32_t buf = 0;
    if (h && h->ds_create_buffer) {
        if (h->ds_create_buffer(c, args[0], desc, &buf) != 0) {
            x2rw_log("[audio] ds_create_buffer: hook falhou (desc=%08x)", desc);
            X2RW_FAIL(c, X2RW_ERROR_OUTOFMEMORY);
            return 0u;
        }
    } else if (h) {
        x2rw_log("[audio] ds_create_buffer: hook presente sem ds_create_buffer"
                 " — objeto sintético criado");
    }

    if (buf == 0) {
        /* Objeto + estado do shim num bloco só (8 words: vtbl + campos). */
        buf = x2rw_com_object_create(c, vtbl, 21);
        if (!buf) {
            X2RW_FAIL(c, X2RW_ERROR_OUTOFMEMORY);
            return 0u;
        }
        /* Estado do shim em bloco PRÓPRIO (não cabe nos slots da vtable). */
        if (!aud_buf_state_ensure(c, buf, args[0])) {
            x2rw_log("[audio] CreateSoundBuffer: sem heap para o estado do "
                     "buffer %08x", buf);
            X2RW_FAIL(c, X2RW_ERROR_OUTOFMEMORY);
            return 0u;
        }
    }

    x2rw_write32(c, out, buf);
    g_aud_buffers++;
    x2rw_log("[audio] IDirectSound::CreateSoundBuffer(this=%08x desc=%08x) -> "
             "buffer=%08x (objeto COM sintético, vtable na memória do guest)",
             args[0], desc, buf);
    X2RW_OK();
    return AUD_OK;
}

static uint32_t aud_ds_get_caps(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t caps = (nargs > 1) ? args[1] : 0u;   /* LPDSCAPS (depois do this) */
    if (!caps) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }
    /* DSCAPS de 96 bytes. Só escrevemos os campos que o jogo pode consultar:
     * dwSize @0, dwFlags @4 (DSCAPS_PRIMARYMONO|PRIMARYSTEREO = 0x3),
     * dwMinSecondarySampleRate @8, dwMaxSecondarySampleRate @0xC. */
    uint32_t sz = x2rw_read32(c, caps);
    if (sz == 0u || sz > 256u) sz = 96u;
    x2rw_write32(c, caps + 0u, sz);
    x2rw_write32(c, caps + 4u, 0x00000003u);
    x2rw_write32(c, caps + 8u, 22050u);
    x2rw_write32(c, caps + 12u, 48000u);
    x2rw_log("[audio] IDirectSound::GetCaps -> DSCAPS sintético em %08x "
             "(3D/EAX não anunciados de propósito)", caps);
    X2RW_OK();
    return AUD_OK;
}

static uint32_t aud_ds_duplicate_buffer(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t src = (nargs > 1) ? args[1] : 0u;   /* LPDIRECTSOUNDBUFFER  */
    uint32_t out = (nargs > 2) ? args[2] : 0u;   /* LPDIRECTSOUNDBUFFER* */
    /* Sem cópia real do conteúdo: devolvemos um segundo objeto com a MESMA
     * vtable. O jogo usa DuplicateSoundBuffer para sons simultâneos; cada
     * cópia tem o seu próprio estado (Lock aloca região própria). */
    uint32_t vtbl[21];
    if (!out || !aud_ds_buffer_vtable(c, vtbl)) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }
    uint32_t buf = x2rw_com_object_create(c, vtbl, 21);
    if (!buf) {
        X2RW_FAIL(c, X2RW_ERROR_OUTOFMEMORY);
        return 0u;
    }
    if (!aud_buf_state_ensure(c, buf, args[0])) {
        X2RW_FAIL(c, X2RW_ERROR_OUTOFMEMORY);
        return 0u;
    }
    x2rw_write32(c, out, buf);
    x2rw_log("[audio] IDirectSound::DuplicateSoundBuffer(%08x) -> %08x "
             "(objeto novo; os dados NÃO foram copiados)", src, buf);
    X2RW_OK();
    return AUD_OK;
}

/* --- IDirectSoundBuffer -------------------------------------------- */

/* Lock(this, dwOffset, dwBytes, ppv1, pbytes1, ppv2, pbytes2, dwFlags) */
static uint32_t aud_ds_buf_lock(x2rw_ctx *c, uint32_t *args, int nargs) {
    /* args[0] = this; os 7 parâmetros da API vêm depois dele. */
    uint32_t self   = args[0];
    uint32_t offset = (nargs > 1) ? args[1] : 0u;   /* dwOffset       */
    uint32_t bytes  = (nargs > 2) ? args[2] : 0u;   /* dwBytes        */
    uint32_t ppv1   = (nargs > 3) ? args[3] : 0u;   /* ppvAudioPtr1   */
    uint32_t pb1    = (nargs > 4) ? args[4] : 0u;   /* pdwAudioBytes1 */
    uint32_t ppv2   = (nargs > 5) ? args[5] : 0u;   /* ppvAudioPtr2   */
    uint32_t pb2    = (nargs > 6) ? args[6] : 0u;   /* pdwAudioBytes2 */
    uint32_t flags  = (nargs > 7) ? args[7] : 0u;   /* dwFlags        */

    if (!ppv1 || !pb1) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }
    if (bytes == 0u) bytes = 8192u;    /* DSBLOCK_ENTIREBUFFER sem tamanho */

    /* Hook primeiro: o backend de áudio pode devolver memória própria. */
    const x2rw_audio_hooks *h = x2rw_audio_get_hooks();
    if (h && h->ds_buffer_lock) {
        uint32_t a = 0, an = 0, b = 0, bn = 0;
        if (h->ds_buffer_lock(c, self, offset, bytes, &a, &an, &b, &bn,
                              flags) == 0 && a) {
            x2rw_write32(c, ppv1, a);
            x2rw_write32(c, pb1, an);
            if (ppv2) x2rw_write32(c, ppv2, b);
            if (pb2)  x2rw_write32(c, pb2, bn);
            x2rw_log("[audio] IDirectSoundBuffer::Lock(hook) off=%u bytes=%u -> "
                     "%08x/%u + %08x/%u", offset, bytes, a, an, b, bn);
            X2RW_OK();
            return AUD_OK;
        }
        x2rw_log("[audio] ds_buffer_lock: hook recusou (off=%u bytes=%u) — "
                 "caindo no bloco do heap do guest", offset, bytes);
    }

    /* Região de áudio do guest: alocada uma vez por buffer e reaproveitada
     * em todos os Lock. O jogo ESCREVE as amostras aqui; sem região real o
     * mixer do jogo escreveria em lixo. */
    uint32_t lp = aud_buf_get(c, self, AUD_ST_AUDIO);
    uint32_t cap = aud_buf_get(c, self, AUD_ST_BYTES);
    if (lp == 0u) {
        uint32_t want = bytes;
        if (want < 65536u) want = 65536u;        /* evita realocar a cada Lock */
        if (want > 0x1000000u) want = 0x1000000u;/* teto de 16 MiB por buffer  */
        lp = x2rw_heap_alloc(c, want);
        if (!lp) {
            X2RW_FAIL(c, X2RW_ERROR_OUTOFMEMORY);
            return 0u;
        }
        cap = want;
        aud_buf_set(c, self, AUD_ST_AUDIO, lp);
        aud_buf_set(c, self, AUD_ST_BYTES, cap);
        x2rw_log("[audio] Lock: região de áudio do guest alocada: %u bytes "
                 "em VA %08x", cap, lp);
    }
    if (offset >= cap) {
        x2rw_log("[audio] Lock: offset %u fora da região (%u bytes)", offset, cap);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }
    uint32_t n1 = bytes;
    if (n1 > cap - offset) n1 = cap - offset;
    x2rw_write32(c, ppv1, lp + offset);
    x2rw_write32(c, pb1, n1);
    if (ppv2) x2rw_write32(c, ppv2, 0u);   /* segunda região sempre vazia */
    if (pb2)  x2rw_write32(c, pb2, 0u);

    x2rw_log("[audio] IDirectSoundBuffer::Lock(this=%08x off=%u bytes=%u) -> "
             "ptr1=%08x n1=%u ptr2=0 n2=0 (região no heap do guest)",
             self, offset, bytes, lp + offset, n1);
    X2RW_OK();
    return AUD_OK;
}

static uint32_t aud_ds_buf_unlock(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t self = args[0];
    uint32_t p1 = (nargs > 1) ? args[1] : 0u;
    uint32_t n1 = (nargs > 2) ? args[2] : 0u;
    const x2rw_audio_hooks *h = x2rw_audio_get_hooks();
    if (h && h->ds_buffer_unlock) h->ds_buffer_unlock(c, self);
    /* Nada a fazer: o jogo escreveu no heap do guest, que é o buffer. */
    x2rw_log("[audio] IDirectSoundBuffer::Unlock(this=%08x ptr=%08x n=%u)",
             self, p1, n1);
    X2RW_OK();
    return AUD_OK;
}

/* Play(this, dwReserved1, dwPriority, dwFlags) == 4 args */
static uint32_t aud_ds_buf_play(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t self = args[0];
    uint32_t r1   = (nargs > 1) ? args[1] : 0u;   /* dwReserved1 */
    uint32_t prio = (nargs > 2) ? args[2] : 0u;   /* dwPriority  */
    uint32_t fl   = (nargs > 3) ? args[3] : 0u;   /* dwFlags     */
    const x2rw_audio_hooks *h = x2rw_audio_get_hooks();
    if (h && h->ds_buffer_play) h->ds_buffer_play(c, self, fl, r1, 0u, prio);
    else x2rw_log("[audio] Play headless: buffer %08x toca no vazio "
                  "(nenhum hook instalado)", self);
    X2RW_OK();
    return AUD_OK;
}

static uint32_t aud_ds_buf_stop(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t self = args[0];
    const x2rw_audio_hooks *h = x2rw_audio_get_hooks();
    if (h && h->ds_buffer_stop) h->ds_buffer_stop(c, self);
    x2rw_log("[audio] IDirectSoundBuffer::Stop(this=%08x)", self);
    X2RW_OK();
    return AUD_OK;
}

/* SetVolume(this, LONG lVolume) — guarda o valor no objeto e repassa ao hook */
static uint32_t aud_ds_buf_set_volume(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t self = args[0];
    int32_t vol = (nargs > 2) ? (int32_t)args[2] : 0;
    aud_buf_set(c, self, AUD_ST_VOL, (uint32_t)vol);
    const x2rw_audio_hooks *h = x2rw_audio_get_hooks();
    if (h && h->ds_buffer_set_volume) h->ds_buffer_set_volume(c, self, vol);
    x2rw_log("[audio] IDirectSoundBuffer::SetVolume(this=%08x vol=%d)", self, vol);
    X2RW_OK();
    return AUD_OK;
}

static uint32_t aud_ds_buf_get_volume(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t self = args[0];
    uint32_t out = (nargs > 1) ? args[1] : 0u;   /* LPLONG */
    int32_t vol = (int32_t)aud_buf_get(c, self, AUD_ST_VOL);
    if (!out) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }
    x2rw_write32(c, out, (uint32_t)vol);
    return AUD_OK;
}

static uint32_t aud_ds_buf_get_frequency(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t self = args[0];
    uint32_t out = (nargs > 1) ? args[1] : 0u;
    uint32_t f = aud_buf_get(c, self, AUD_ST_FREQ);
    if (!out) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }
    x2rw_write32(c, out, f);
    return AUD_OK;
}

static uint32_t aud_ds_buf_get_pan(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t self = args[0];
    uint32_t out = (nargs > 1) ? args[1] : 0u;
    uint32_t p = aud_buf_get(c, self, AUD_ST_PAN);
    if (!out) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }
    x2rw_write32(c, out, p);
    return AUD_OK;
}

/* GetCurrentPosition(this, pdwPlayCursor, pdwWriteCursor) */
static uint32_t aud_ds_buf_get_current_position(x2rw_ctx *c, uint32_t *args,
                                                int nargs) {
    uint32_t self = args[0];
    uint32_t pplay  = (nargs > 1) ? args[1] : 0u;
    uint32_t pwrite = (nargs > 2) ? args[2] : 0u;
    uint32_t pos = aud_buf_get(c, self, AUD_ST_POS);
    if (pplay)  x2rw_write32(c, pplay, pos);
    if (pwrite) x2rw_write32(c, pwrite, pos);
    /* Cursor estático: o jogo só usa isto para saber se o som terminou. */
    return AUD_OK;
}

/* GetStatus(this, LPDWORD pdwStatus) — PLAYING/CURSOR não são anunciados. */
static uint32_t aud_ds_buf_get_status(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t out = (nargs > 1) ? args[1] : 0u;
    if (out) x2rw_write32(c, out, 0u);
    return AUD_OK;
}

/* Handler genérico dos métodos de IDirectSound/IDirectSoundBuffer que o
 * jogo não usa no caminho principal (GetFormat/SetFormat/Initialize/
 * Compact/GetSpeakerConfig/SetSpeakerConfig/Restore/SetCurrentPosition...).
 * Registrado para que NENHUM slot da vtable fique nulo: uma chamada inesperada
 * devolve sucesso sintético e fica no log, em vez de pular para 0x0. */
static uint32_t aud_com_unused(x2rw_ctx *c, uint32_t *args, int nargs) {
    char buf[128];
    int off = 0;
    for (int i = 0; i < nargs && i < 8 && off < (int)sizeof(buf) - 12; i++)
        off += snprintf(buf + off, sizeof(buf) - (size_t)off, "%08x ", args[i]);
    buf[off < (int)sizeof(buf) ? off : (int)sizeof(buf) - 1] = 0;
    x2rw_log("[audio] método COM sem uso no port devolveu DS_OK "
             "(args=[%s]) — nenhum estado foi alterado", buf);
    X2RW_OK();
    return AUD_OK;
}

/* ------------------------------------------------------------------ */
/* 2b. Construção das vtables COM                                     */
/* ------------------------------------------------------------------ */

/* Registra os 21 slots de IDirectSoundBuffer uma única vez e devolve os VAs
 * dos thunks em `out`. Devolve 0 (e loga) se a tabela COM do despachante
 * esgotar. */
static int aud_ds_buffer_vtable(x2rw_ctx *c, uint32_t *out) {
    static uint32_t v[21];
    static int ready = 0;
    (void)c;
    if (ready) {
        for (int i = 0; i < 21; i++) out[i] = v[i];
        return 1;
    }
    v[0]  = x2rw_com_register(aud_com_query_interface, 4);
    v[1]  = x2rw_com_register(aud_com_add_ref, 1);
    v[2]  = x2rw_com_register(aud_com_release, 1);
    v[3]  = x2rw_com_register(aud_com_unused, 2);        /* GetCaps          */
    v[4]  = x2rw_com_register(aud_ds_buf_get_current_position, 4);
    v[5]  = x2rw_com_register(aud_com_unused, 3);        /* GetFormat        */
    v[6]  = x2rw_com_register(aud_ds_buf_get_volume, 3);
    v[7]  = x2rw_com_register(aud_ds_buf_get_pan, 3);
    v[8]  = x2rw_com_register(aud_ds_buf_get_frequency, 3);
    v[9]  = x2rw_com_register(aud_ds_buf_get_status, 3);
    v[10] = x2rw_com_register(aud_com_unused, 2);        /* Initialize       */
    v[11] = x2rw_com_register(aud_ds_buf_lock, 8);
    v[12] = x2rw_com_register(aud_ds_buf_play, 4);
    v[13] = x2rw_com_register(aud_com_unused, 3);        /* SetCurrentPos.   */
    v[14] = x2rw_com_register(aud_com_unused, 3);        /* SetFormat        */
    v[15] = x2rw_com_register(aud_ds_buf_set_volume, 3);
    v[16] = x2rw_com_register(aud_com_unused, 3);        /* SetPan           */
    v[17] = x2rw_com_register(aud_com_unused, 3);        /* SetFrequency     */
    v[18] = x2rw_com_register(aud_ds_buf_stop, 2);
    v[19] = x2rw_com_register(aud_ds_buf_unlock, 4);
    v[20] = x2rw_com_register(aud_com_unused, 2);        /* Restore          */
    for (int i = 0; i < 21; i++) {
        if (v[i] == 0u) {
            x2rw_log("[audio] ERRO: thunk COM nulo no slot %d de "
                     "IDirectSoundBuffer", i);
            return 0;
        }
    }
    ready = 1;
    for (int i = 0; i < 21; i++) out[i] = v[i];
    return 1;
}

/* Idem para os 11 slots de IDirectSound. */
static int aud_ds_vtable(x2rw_ctx *c, uint32_t *out) {
    static uint32_t v[11];
    static int ready = 0;
    (void)c;
    if (ready) {
        for (int i = 0; i < 11; i++) out[i] = v[i];
        return 1;
    }
    v[0]  = x2rw_com_register(aud_com_query_interface, 4);
    v[1]  = x2rw_com_register(aud_com_add_ref, 1);
    v[2]  = x2rw_com_register(aud_com_release, 1);
    v[3]  = x2rw_com_register(aud_ds_create_sound_buffer, 4);
    v[4]  = x2rw_com_register(aud_ds_get_caps, 3);
    v[5]  = x2rw_com_register(aud_ds_duplicate_buffer, 4);
    v[6]  = x2rw_com_register(aud_com_unused, 4);   /* SetCooperativeLevel  */
    v[7]  = x2rw_com_register(aud_com_unused, 2);   /* Compact              */
    v[8]  = x2rw_com_register(aud_com_unused, 3);   /* GetSpeakerConfig     */
    v[9]  = x2rw_com_register(aud_com_unused, 3);   /* SetSpeakerConfig     */
    v[10] = x2rw_com_register(aud_com_unused, 3);   /* Initialize           */
    for (int i = 0; i < 11; i++) {
        if (v[i] == 0u) {
            x2rw_log("[audio] ERRO: thunk COM nulo no slot %d de IDirectSound", i);
            return 0;
        }
    }
    ready = 1;
    for (int i = 0; i < 11; i++) out[i] = v[i];
    return 1;
}

/* ------------------------------------------------------------------ */
/* 3. Handler: DirectSoundCreate (DSOUND.dll!ordinal_11)              */
/* ------------------------------------------------------------------ */
/* HRESULT DirectSoundCreate(LPCGUID pcGuidDevice, LPDIRECTSOUND *ppDS,
 *                          LPUNKNOWN pUnkOuter)  — stdcall, 3 args.
 * É a MESMA função que DirectSoundCreate8 no meio do .dll; o nome no
 * registro é "ordinal_11" porque é assim que a IAT do jogo a importa
 * (x2rw_import_lookup_ordinal trata o caso). */
uint32_t aud_DirectSoundCreate(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t guid = (nargs > 0) ? args[0] : 0u;   /* LPCGUID (NULL = padrão) */
    uint32_t ppds = (nargs > 1) ? args[1] : 0u;   /* LPDIRECTSOUND*          */
    (void)guid;

    if (!ppds) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }

    uint32_t vtbl[11];
    if (!aud_ds_vtable(c, vtbl)) {
        X2RW_FAIL(c, X2RW_ERROR_OUTOFMEMORY);
        return 0u;
    }

    const x2rw_audio_hooks *h = x2rw_audio_get_hooks();
    uint32_t ds = 0;
    if (h && h->ds_create) {
        if (h->ds_create(c, guid, &ds) != 0) {
            x2rw_log("[audio] ds_create: hook falhou (guid=%08x)", guid);
            X2RW_FAIL(c, X2RW_ERROR_OUTOFMEMORY);
            return 0u;
        }
    }
    if (ds == 0u) {
        ds = x2rw_com_object_create(c, vtbl, 11);
        if (!ds) {
            X2RW_FAIL(c, X2RW_ERROR_OUTOFMEMORY);
            return 0u;
        }
    }

    x2rw_write32(c, ppds, ds);
    g_aud_ds_created++;

    /* O hook de init é idempotente por contrato; chamamos só na primeira
     * criação para não reabrir o dispositivo a cada nível do jogo. */
    if (h && h->init && g_aud_ds_created == 1) h->init(c);

    x2rw_log("[audio] DirectSoundCreate(guid=%08x) -> IDirectSound em %08x "
             "(%s; vtable de 11 slots na memória do guest)",
             guid, ds, h ? "hook de áudio instalado" : "HEADLESS: sem hook");
    X2RW_OK();
    return AUD_OK;
}

/* ------------------------------------------------------------------ */
/* 4. Handlers: waveOut (WINMM.dll)                                   */
/* ------------------------------------------------------------------ */

/* MMRESULT waveOutOpen(LPHWAVEOUT phwo, UINT uDeviceID, LPCWAVEFORMATEX pwfx,
 *                      DWORD_PTR dwCallback, DWORD_PTR dwInstance, DWORD fdwOpen)
 * Devolve um handle sintético e registra o formato pedido. */
uint32_t aud_waveOutOpen(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t phwo  = (nargs > 0) ? args[0] : 0u;
    uint32_t devid = (nargs > 1) ? args[1] : 0u;
    uint32_t pwfx  = (nargs > 2) ? args[2] : 0u;
    uint32_t cbfn  = (nargs > 3) ? args[3] : 0u;
    uint32_t inst  = (nargs > 4) ? args[4] : 0u;
    uint32_t flags = (nargs > 5) ? args[5] : 0u;

    if (!phwo || !pwfx) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }

    /* WAVEFORMATEX (18 bytes) lido da memória do guest:
     *  @0 wFormatTag, @2 nChannels, @4 nSamplesPerSec, @8 nAvgBytesPerSec,
     *  @12 nBlockAlign, @14 wBitsPerSample, @16 cbSize. */
    uint8_t wf[AUD_WF_SIZE];
    x2rw_mem_read(pwfx, wf, sizeof(wf));
    uint16_t tag   = (uint16_t)(wf[0] | (wf[1] << 8));
    uint16_t ch    = (uint16_t)(wf[2] | (wf[3] << 8));
    uint32_t rate  = (uint32_t)wf[4] | ((uint32_t)wf[5] << 8) |
                     ((uint32_t)wf[6] << 16) | ((uint32_t)wf[7] << 24);
    uint16_t align = (uint16_t)(wf[12] | (wf[13] << 8));
    uint16_t bits  = (uint16_t)(wf[14] | (wf[15] << 8));

    g_aud_out_rate = rate;
    g_aud_out_ch = (int)ch;

    uint32_t handle = 0;
    const x2rw_audio_hooks *h = x2rw_audio_get_hooks();
    if (h && h->wave_out_open) {
        /* O hook pode abrir o dispositivo real (AAudio) e devolver o VA
         * dele; se devolver 0, usamos o handle sintético abaixo. */
        h->wave_out_open(c, &handle, devid, pwfx, cbfn, inst, flags);
    }
    if (handle == 0u) {
        /* Handle sintético no espaço 0x40000000+ (fora da faixa dos handles
         * de arquivo do kernel, 1..N, para não confundir o log de quem lê). */
        handle = 0x40000001u;
    }
    x2rw_write32(c, phwo, handle);
    x2rw_log("[audio] waveOutOpen(dev=%u) -> hwo=%08x  wfx: tag=%u ch=%u "
             "rate=%u align=%u bits=%u cbSize=%u %s",
             devid, handle, tag, ch, rate, align, bits,
             (uint32_t)(wf[16] | (wf[17] << 8)),
             h ? "(hook instalado)" : "(HEADLESS: nenhum som sai)");
    X2RW_OK();
    return AUD_OK;
}

/* MMRESULT waveOutWrite(HWAVEOUT hwo, LPWAVEHDR pwh, UINT cbwh)
 *
 * É AQUI que o loop de áudio do jogo avança: marcamos WHDR_DONE na WAVEHDR
 * (dwFlags @0x10) e limpamos WHDR_INQUEUE. Sem isso o mixer do jogo (e o
 * Bink) fica esperando para sempre por um buffer que "o hardware" nunca
 * devolve. */
uint32_t aud_waveOutWrite(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t hwo = (nargs > 0) ? args[0] : 0u;
    uint32_t pwh = (nargs > 1) ? args[1] : 0u;
    uint32_t cb  = (nargs > 2) ? args[2] : 0u;

    const x2rw_audio_hooks *h = x2rw_audio_get_hooks();
    if (h && h->wave_out_write) h->wave_out_write(c, hwo, pwh);

    if (pwh) {
        /* WAVEHDR: lpData @0, dwBufferLength @4, dwBytesRecorded @8,
         * dwUser @0xC, dwFlags @0x10, dwLoops @0x14. */
        uint32_t fl = x2rw_read32(c, pwh + AUD_WAVEHDR_FLAGS);
        fl |= AUD_WHDR_DONE;
        fl &= ~AUD_WHDR_INQUEUE;
        x2rw_write32(c, pwh + AUD_WAVEHDR_FLAGS, fl);
    }

    if (g_aud_writes++ < 4) {
        x2rw_log("[audio] waveOutWrite(hwo=%08x hdr=%08x cb=%u): "
                 "WHDR_DONE marcado em hdr+0x10 (flags=%08x) — o loop de "
                 "áudio do jogo pode reaproveitar o buffer",
                 hwo, pwh, cb, pwh ? x2rw_read32(c, pwh + AUD_WAVEHDR_FLAGS) : 0u);
    }
    X2RW_OK();
    return AUD_OK;
}

uint32_t aud_waveOutPrepareHeader(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t hwo = (nargs > 0) ? args[0] : 0u;
    uint32_t pwh = (nargs > 1) ? args[1] : 0u;
    uint32_t cb  = (nargs > 2) ? args[2] : 0u;
    if (pwh) {
        uint32_t fl = x2rw_read32(c, pwh + AUD_WAVEHDR_FLAGS);
        fl |= AUD_WHDR_PREPARED;
        fl &= ~AUD_WHDR_INQUEUE;
        x2rw_write32(c, pwh + AUD_WAVEHDR_FLAGS, fl);
    }
    x2rw_log("[audio] waveOutPrepareHeader(hwo=%08x hdr=%08x cb=%u) -> "
             "WHDR_PREPARED", hwo, pwh, cb);
    X2RW_OK();
    return AUD_OK;
}

uint32_t aud_waveOutUnprepareHeader(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t hwo = (nargs > 0) ? args[0] : 0u;
    uint32_t pwh = (nargs > 1) ? args[1] : 0u;
    uint32_t cb  = (nargs > 2) ? args[2] : 0u;
    if (pwh)
        x2rw_write32(c, pwh + AUD_WAVEHDR_FLAGS, x2rw_read32(c, pwh +
                    AUD_WAVEHDR_FLAGS) & ~AUD_WHDR_PREPARED);
    x2rw_log("[audio] waveOutUnprepareHeader(hwo=%08x hdr=%08x cb=%u)",
             hwo, pwh, cb);
    X2RW_OK();
    return AUD_OK;
}

uint32_t aud_waveOutReset(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t hwo = (nargs > 0) ? args[0] : 0u;
    const x2rw_audio_hooks *h = x2rw_audio_get_hooks();
    if (h && h->wave_out_reset) h->wave_out_reset(c, hwo);
    x2rw_log("[audio] waveOutReset(hwo=%08x) -> MMSYSERR_NOERROR", hwo);
    X2RW_OK();
    return AUD_OK;
}

uint32_t aud_waveOutClose(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t hwo = (nargs > 0) ? args[0] : 0u;
    x2rw_log("[audio] waveOutClose(hwo=%08x)", hwo);
    X2RW_OK();
    return AUD_OK;
}

uint32_t aud_waveOutRestart(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t hwo = (nargs > 0) ? args[0] : 0u;
    x2rw_log("[audio] waveOutRestart(hwo=%08x)", hwo);
    X2RW_OK();
    return AUD_OK;
}

uint32_t aud_waveOutPause(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t hwo = (nargs > 0) ? args[0] : 0u;
    x2rw_log("[audio] waveOutPause(hwo=%08x)", hwo);
    X2RW_OK();
    return AUD_OK;
}

uint32_t aud_waveOutSetVolume(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t dev  = (nargs > 0) ? args[0] : 0u;
    uint32_t vol  = (nargs > 1) ? args[1] : 0u;
    /* dwVolume: LOWORD = canal esquerdo, HIWORD = direito. Só registramos:
     * o volume real quem aplica é o hook de áudio (que aqui não existe). */
    x2rw_log("[audio] waveOutSetVolume(dev=%08x vol=%08x) — registrado; "
             "nenhum dispositivo de saída existe neste módulo", dev, vol);
    X2RW_OK();
    return AUD_OK;
}

/* ------------------------------------------------------------------ */
/* 5. Handlers: mmio (WINMM.dll) — leitura real dos WAV              */
/* ------------------------------------------------------------------ */

/* Abre o arquivo apontado pelo caminho Win32 do guest e devolve um handle
 * sintético (índice+1 na tabela g_aud_files). */
uint32_t aud_mmioOpenA(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t name_va = (nargs > 0) ? args[0] : 0u;   /* LPSTR szFileName */
    uint32_t info    = (nargs > 1) ? args[1] : 0u;   /* LPMMIOINFO        */
    uint32_t flags   = (nargs > 2) ? args[2] : 0u;   /* DWORD dwOpenFlags */
    (void)info;

    if (!name_va) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }

    char win[512];
    uint32_t n = x2rw_guest_strlen(c, name_va);
    if (n >= sizeof(win)) n = sizeof(win) - 1u;
    x2rw_mem_read(name_va, win, n);
    win[n] = 0;

    char path[512];
    x2rw_win32_path_to_host(win, path, sizeof(path));

    /* MMIO_READ=0, MMIO_WRITE=1, MMIO_READWRITE=2 (os outros bits de
     * dwOpenFlags são de criação/compartilhamento e não mudam o modo). */
    uint32_t mode = flags & 3u;
    const char *m = (mode == 1u) ? "wb" : (mode == 2u) ? "r+b" : "rb";
    FILE *fp = fopen(path, m);
    if (!fp && mode == 2u) fp = fopen(path, "wb");   /* arquivo novo */
    if (!fp) {
        x2rw_log("[audio] mmioOpenA: não abriu '%s' (host='%s' mode=%u)",
                 win, path, mode);
        X2RW_FAIL(c, X2RW_ERROR_FILE_NOT_FOUND);
        return 0u;
    }

    uint32_t size = 0;
    if (fseek(fp, 0, SEEK_END) == 0) {
        long end = ftell(fp);
        if (end > 0) size = (uint32_t)end;
        fseek(fp, 0, SEEK_SET);
    }

    uint32_t h = aud_file_new(fp, size);
    if (h == 0u) {
        fclose(fp);
        x2rw_log("[audio] mmioOpenA: tabela de arquivos cheia (%d)", AUD_MAX_FILES);
        X2RW_FAIL(c, X2RW_ERROR_NOT_ENOUGH_MEMORY);
        return 0u;
    }
    x2rw_log("[audio] mmioOpenA('%s') -> handle %u (host='%s', %u bytes)",
             win, h, path, size);
    X2RW_OK();
    return h;
}

uint32_t aud_mmioClose(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t h = (nargs > 0) ? args[0] : 0u;
    uint32_t flags = (nargs > 1) ? args[1] : 0u;
    (void)flags;
    aud_mmio_file *f = aud_file_get(h);
    if (!f) {
        x2rw_log("[audio] mmioClose: handle %u inválido", h);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
        return 0u;
    }
    fclose(f->fp);
    f->fp = NULL;
    f->size = 0u;
    x2rw_log("[audio] mmioClose(handle %u)", h);
    X2RW_OK();
    return AUD_OK;
}

uint32_t aud_mmioRead(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t h   = (nargs > 0) ? args[0] : 0u;
    uint32_t dst = (nargs > 1) ? args[1] : 0u;   /* HPSTR pch */
    uint32_t n   = (nargs > 2) ? args[2] : 0u;   /* LONG cch  */
    aud_mmio_file *f = aud_file_get(h);
    if (!f) {
        x2rw_log("[audio] mmioRead: handle %u inválido", h);
        X2RW_FAIL(c, X2RW_ERROR_INVALID_HANDLE);
        return (uint32_t)-1;
    }
    if (n == 0u) return 0u;
    if (!dst) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return (uint32_t)-1;
    }

    /* Lê em blocos de 32 KiB para não estourar a pilha do host com `n`
     * arbitrariamente grande vindo do guest. */
    uint8_t chunk[32768];
    uint32_t done = 0;
    while (done < n) {
        uint32_t want = n - done;
        if (want > sizeof(chunk)) want = (uint32_t)sizeof(chunk);
        size_t got = fread(chunk, 1, want, f->fp);
        if (got) x2rw_mem_write(dst + done, chunk, got);
        done += (uint32_t)got;
        if (got < (size_t)want) break;   /* EOF */
    }
    x2rw_log("[audio] mmioRead(handle %u, %u bytes pedidos) -> %u lidos",
             h, n, done);
    X2RW_OK();
    return done;
}

/* NOTA SOBRE `mmioSeek`: NÃO está implementado aqui porque o jogo não o
 * importa. A lista de imports (gen/Wolvie_imports.h) traz apenas mmioAscend,
 * mmioRead, mmioDescend, mmioClose, mmioStringToFOURCCA e mmioOpenA — um
 * handler para mmioSeek seria código permanentemente inalcançável, e
 * código inalcançável que parece funcionar é pior que código ausente.
 * Se algum dia o import aparecer, a lista mestra (X2RW_SHIMS_AUDIO) é que
 * precisa ganhar a entrada, e só então a implementação. */

/* FOURCC mmioStringToFOURCCA(LPCSTR sz, UINT uFlags)
 * FOURCC = s[0] | s[1]<<8 | s[2]<<16 | s[3]<<24 (strings curtas valem 0). */
uint32_t aud_mmioStringToFOURCCA(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t s_va = (nargs > 0) ? args[0] : 0u;
    uint32_t flags = (nargs > 1) ? args[1] : 0u;
    (void)flags;
    uint8_t s[4] = { 0, 0, 0, 0 };
    if (s_va) {
        uint32_t n = x2rw_guest_strlen(c, s_va);
        if (n > 4u) n = 4u;
        if (n) x2rw_mem_read(s_va, s, n);
    }
    uint32_t fourcc = (uint32_t)s[0] | ((uint32_t)s[1] << 8) |
                      ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24);
    x2rw_log("[audio] mmioStringToFOURCCA('%c%c%c%c') -> %08x",
             s[0] ? s[0] : '.', s[1] ? s[1] : '.', s[2] ? s[2] : '.',
             s[3] ? s[3] : '.', fourcc);
    X2RW_OK();
    return fourcc;
}

/* MMRESULT mmioDescend(HMMIO hmmio, LPMMCKINFO pmmcki, const MMCKINFO *pmmckiParent,
 *                     UINT fuDescend)
 *
 * MMCKINFO (32 bits): ckid @0, ckSize @4, fccType @8, dwDataOffset @0xC,
 * dwFlags @0x10. Lemos a assinatura (4 bytes) no arquivo e preenchemos a
 * struct do guest. Só o caminho "descer para o próximo chunk" (fuDescend==0,
 * sem busca por ckid) é resolvido de verdade; procurar por um ckid/fccType
 * específico exigiria varrer o arquivo, e o jogo não o faz no caminho de
 * leitura dos .wav. */
uint32_t aud_mmioDescend(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t h    = (nargs > 0) ? args[0] : 0u;
    uint32_t cki  = (nargs > 1) ? args[1] : 0u;   /* LPMMCKINFO        */
    uint32_t par  = (nargs > 2) ? args[2] : 0u;   /* LPMMCKINFO pai    */
    uint32_t how  = (nargs > 3) ? args[3] : 0u;   /* fuDescend         */
    (void)par;

    aud_mmio_file *f = aud_file_get(h);
    if (!f || !cki) {
        X2RW_FAIL(c, f ? X2RW_ERROR_INVALID_PARAMETER : X2RW_ERROR_INVALID_HANDLE);
        return 0u;
    }

    uint8_t hdr[8];
    size_t got = fread(hdr, 1, sizeof(hdr), f->fp);
    if (got < sizeof(hdr)) {
        x2rw_log("[audio] mmioDescend: fim do arquivo (handle %u)", h);
        X2RW_FAIL(c, X2RW_ERROR_NO_MORE_FILES);
        return 0u;
    }
    uint32_t ckid = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                    ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
    uint32_t sz   = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
                    ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
    long datapos = ftell(f->fp);

    /* Um chunk RIFF/LIST/RIFX embute o tamanho do cabeçalho; o "conteúdo"
     * útil começa depois do fccType. */
    int container = (ckid == 0x46464952u /*RIFF*/ ||
                     ckid == 0x5453494Cu /*LIST*/ ||
                     ckid == 0x58464952u /*RIFX*/);
    uint32_t content = sz;
    if (container) {
        uint8_t type[4];
        if (fread(type, 1, sizeof(type), f->fp) == sizeof(type))
            content = (sz >= 4u) ? sz - 4u : 0u;
        x2rw_write32(c, cki + 8u, (uint32_t)type[0] | ((uint32_t)type[1] << 8) |
                                      ((uint32_t)type[2] << 16) |
                                      ((uint32_t)type[3] << 24));
    } else {
        x2rw_write32(c, cki + 8u, 0u);
    }
    x2rw_write32(c, cki + 0u, ckid);
    x2rw_write32(c, cki + 4u, content);
    x2rw_write32(c, cki + 12u, (uint32_t)datapos);
    x2rw_write32(c, cki + 16u, 0u);   /* dwFlags: chunk normal, sem padding */

    x2rw_log("[audio] mmioDescend(handle %u): ckid='%c%c%c%c' ckSize=%u "
             "(conteúdo=%u) dataOffset=%ld%s",
             h, (char)(ckid & 0xff), (char)((ckid >> 8) & 0xff),
             (char)((ckid >> 16) & 0xff), (char)((ckid >> 24) & 0xff),
             sz, content, datapos, container ? " [container]" : "");
    if (how & 0x10u) {
        x2rw_log("[audio] mmioDescend: busca por ckid/fccType específico "
                 "(fuDescend=%08x) NÃO é resolvida — o arquivo não é varrido", how);
        X2RW_FAIL(c, X2RW_ERROR_CALL_NOT_IMPLEMENTED);
        return 0u;
    }
    X2RW_OK();
    return AUD_OK;
}

/* MMRESULT mmioAscend(HMMIO hmmio, LPMMCKINFO pmmcki, UINT fuAscend)
 * Pula para o fim do chunk corrente (ckSize + padding para paridade). */
uint32_t aud_mmioAscend(x2rw_ctx *c, uint32_t *args, int nargs) {
    uint32_t h   = (nargs > 0) ? args[0] : 0u;
    uint32_t cki = (nargs > 1) ? args[1] : 0u;
    uint32_t fu  = (nargs > 2) ? args[2] : 0u;
    (void)fu;
    aud_mmio_file *f = aud_file_get(h);
    if (!f || !cki) {
        X2RW_FAIL(c, f ? X2RW_ERROR_INVALID_PARAMETER : X2RW_ERROR_INVALID_HANDLE);
        return 0u;
    }
    uint32_t off  = x2rw_read32(c, cki + 12u);   /* dwDataOffset */
    uint32_t size = x2rw_read32(c, cki + 4u);    /* ckSize       */
    uint32_t end  = off + size;
    if (end & 1u) end++;                          /* padding RIFF  */
    if (fseek(f->fp, (long)end, SEEK_SET) != 0) {
        X2RW_FAIL(c, X2RW_ERROR_INVALID_PARAMETER);
        return 0u;
    }
    x2rw_log("[audio] mmioAscend(handle %u): salta para %u", h, end);
    X2RW_OK();
    return AUD_OK;
}

/* ------------------------------------------------------------------ */
/* 6. Handler: timeGetTime (WINMM.dll)                                */
/* ------------------------------------------------------------------ */
/* DWORD timeGetTime(void) — ms desde o boot, monotônico.
 * O jogo usa isto para temporização de animação/áudio: precisa ser
 * monotônico e NÃO pode andar para trás (usamos CLOCK_MONOTONIC). */
uint32_t aud_timeGetTime(x2rw_ctx *c, uint32_t *args, int nargs) {
    (void)c; (void)args; (void)nargs;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        X2RW_FAIL(c, X2RW_ERROR_CALL_NOT_IMPLEMENTED);
        return 0u;
    }
    /* 32 bits de ms = ~49 dias; o wrap é o mesmo do Windows. */
    uint32_t ms = (uint32_t)((uint64_t)ts.tv_sec * 1000ull +
                             (uint64_t)(ts.tv_nsec / 1000000));
    X2RW_OK();
    return ms;
}
