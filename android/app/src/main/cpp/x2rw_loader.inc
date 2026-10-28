/* X2: Wolverine's Revenge - port Android
 * Loader dos PE do jogo (parte independente de plataforma).
 *
 * POR QUE ESTE ARQUIVO EXISTE COMO `.inc`:
 * o build Android e o build desktop precisam mapear os MESMOS binários no
 * MESMO espaço de endereços do guest. Duplicar esta lógica criaria duas
 * versões que divergem em silêncio (endereço errado = bug silencioso, o
 * pior tipo neste projeto). Então a parte independente de plataforma vive
 * aqui e é compilada UMA vez por alvo — pelo `x2rw_android_main.c` e pelo
 * `desktop/main.c`. O que muda entre alvos é só `x2rw_read_game_file` /
 * `x2rw_mem_free_impl` (por isso ficam de fora deste arquivo).
 *
 * O que este loader NÃO faz (e é honesto dizer):
 *   - não aplica relocations do PE: o jogo é carregado na imagebase com que
 *     foi linkado (0x00400000 / 0x03000000), logo não há rebase;
 *   - não resolve a IAT: quem resolve imports é a camada de shims
 *     (`x2rw_import_dispatch`, em runtime/shims/).
 */
#ifdef X2WR_LOADER_IMPL

#include <stdlib.h>
#include <string.h>

/* O header tem guard: incluí-lo aqui é seguro mesmo que quem inclui o .inc
 * já o tenha incluído antes. */
#include "x2rw_host.h"

/* Offsets do PE (PE32). Ver docs/RECON.md: Wolvie.exe é PE32 i386, 5 seções. */
#define X2WR_PE_E_LFANEW          0x3Cu  /* deslocamento do cabeçalho PE       */
#define X2WR_PE_OPT_MAGIC         0x18u  /* PE32=0x10b, PE32+=0x20b            */
/* AddressOfEntryPoint fica em 0x10 no optional header do PE32. Uma versão
 * anterior deste arquivo usava 0x28, que é Major/MinorOperatingSystemVersion
 * — o valor lido era 4, e o jogo começaria a executar em ImageBase+4, isto é,
 * lixo. Fica registrado para não voltar. */
#define X2WR_PE_ENTRY_RVA         0x10u  /* AddressOfEntryPoint                */
#define X2WR_PE_IMAGE_BASE        0x1Cu  /* ImageBase (precisa bater com base) */
#define X2WR_PE_SIZE_OF_IMAGE     0x38u  /* SizeOfImage                        */
#define X2WR_PE_SIZE_OF_HEADERS   0x3Cu  /* SizeOfHeaders                      */
#define X2WR_PE_MAGIC32           0x10bu

static uint32_t x2rw_pe_u16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t x2rw_pe_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Offsets do cabeçalho COFF (que começa 4 bytes depois de "PE\0\0"). */
#define X2WR_PE_NUM_SECTIONS      0x06u  /* +lfanew: NumberOfSections          */
#define X2WR_PE_OPT_SIZE          0x14u  /* +lfanew: SizeOfOptionalHeader      */

/* Layout de um section header (40 bytes). */
#define X2WR_SECT_VIRT_SIZE       0x08u
#define X2WR_SECT_VIRT_ADDR       0x0Cu
#define X2WR_SECT_RAW_SIZE        0x10u
#define X2WR_SECT_RAW_PTR         0x14u
#define X2WR_SECT_HEADER_SIZE     40u

typedef struct {
    size_t   size_of_image;
    uint32_t entry_rva;
    uint32_t image_base;
    uint32_t size_of_headers;
    uint32_t num_sections;
    uint32_t opt_size;
    uint32_t section_table;   /* offset do primeiro section header no arquivo */
} x2rw_pe_info;

/* Lê e valida o cabeçalho do PE32. Devolve 0 se o arquivo é utilizável. */
static int x2rw_pe_probe(const uint8_t *img, size_t file_size, x2rw_pe_info *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (file_size < 0x40) return -1;
    if (img[0] != 'M' || img[1] != 'Z') return -1;

    uint32_t lfanew = x2rw_pe_u32(img + X2WR_PE_E_LFANEW);
    if (lfanew == 0 || (size_t)lfanew + 0x40u > file_size) return -1;
    if (img[lfanew + 0] != 'P' || img[lfanew + 1] != 'E' ||
        img[lfanew + 2] != 0   || img[lfanew + 3] != 0) {
        return -1;
    }

    /* Optional header começa em lfanew + 4 (assinatura) + 20 (COFF). */
    const uint8_t *opt = img + lfanew + 24u;
    if ((size_t)(opt - img) + 0x40u > file_size) return -1;
    if (x2rw_pe_u16(opt) != X2WR_PE_MAGIC32) return -1; /* não é PE32 */

    out->entry_rva       = x2rw_pe_u32(opt + X2WR_PE_ENTRY_RVA);
    out->image_base      = x2rw_pe_u32(opt + X2WR_PE_IMAGE_BASE);
    out->size_of_image   = x2rw_pe_u32(opt + X2WR_PE_SIZE_OF_IMAGE);
    out->size_of_headers = x2rw_pe_u32(opt + X2WR_PE_SIZE_OF_HEADERS);
    out->num_sections    = x2rw_pe_u16(img + lfanew + X2WR_PE_NUM_SECTIONS);
    out->opt_size        = x2rw_pe_u16(img + lfanew + X2WR_PE_OPT_SIZE);
    out->section_table   = lfanew + 24u + out->opt_size;

    if (out->size_of_image == 0 || out->num_sections == 0 ||
        out->num_sections > 96) {
        return -1;
    }
    if ((size_t)out->section_table +
        (size_t)out->num_sections * X2WR_SECT_HEADER_SIZE > file_size) {
        return -1;
    }
    return 0;
}

/* Monta a IMAGEM DE MEMÓRIA do PE em `image` (já zerada, tamanho
 * size_of_image).
 *
 * POR QUE ISTO É OBRIGATÓRIO E NÃO UM DETALHE:
 * num arquivo PE, o conteúdo de cada seção está num deslocamento de ARQUIVO
 * (PointerToRawData) que NÃO é igual ao endereço VIRTUAL onde a seção é
 * carregada (VirtualAddress). Mapear os bytes crus do arquivo no endereço
 * base entregaria ao jogo dados nos endereços errados — todo ponteiro
 * absoluto embutido no binário apontaria para o lugar errado, e o
 * resultado seria uma falha sem causa óbvia. É o mesmo tipo de erro que já
 * apareceu no lifter (VA x RVA), então fica documentado aqui.
 *
 * Regra do Windows (que seguimos): copia-se min(VirtualSize, SizeOfRawData)
 * de PointerToRawData para VirtualAddress; o resto da seção fica ZERO
 * (é o "BSS" do PE). Os cabeçalhos (SizeOfHeaders bytes) vão para o RVA 0.
 */
static int x2rw_build_image(const uint8_t *file, size_t file_size,
                            const x2rw_pe_info *info, uint8_t *image) {
    size_t n_head = info->size_of_headers;
    if (n_head > info->size_of_image) n_head = info->size_of_image;
    if (n_head > file_size) n_head = file_size;
    memcpy(image, file, n_head);

    for (uint32_t i = 0; i < info->num_sections; i++) {
        const uint8_t *sh = file + info->section_table +
                            (size_t)i * X2WR_SECT_HEADER_SIZE;
        uint32_t vsize = x2rw_pe_u32(sh + X2WR_SECT_VIRT_SIZE);
        uint32_t vaddr = x2rw_pe_u32(sh + X2WR_SECT_VIRT_ADDR);
        uint32_t rsize = x2rw_pe_u32(sh + X2WR_SECT_RAW_SIZE);
        uint32_t rptr  = x2rw_pe_u32(sh + X2WR_SECT_RAW_PTR);

        /* Seção sem dados crus (só BSS): nada a copiar; fica zero. */
        if (rptr == 0 || rsize == 0) continue;

        /* Copia SizeOfRawData inteiro, não min(VirtualSize, SizeOfRawData).
         *
         * POR QUÊ: quando VirtualSize < SizeOfRawData (o normal quando a
         * seção é alinhada no arquivo), o Windows mapeia VirtualSize
         * arredondado para SectionAlignment e preenche do arquivo — ou seja,
         * os bytes de alinhamento ENTRE VirtualSize e SizeOfRawData também
         * ficam visíveis. Copiar só VirtualSize deixava 216 bytes zerados
         * onde o binário tem dados. Verificado comparando byte a byte com
         * pefile.get_memory_mapped_image(): com SizeOfRawData a imagem sai
         * IDÊNTICA.
         *
         * `vsize` fica sem uso aqui de propósito; o limite é o tamanho da
         * imagem. */
        (void)vsize;
        size_t n = (size_t)rsize;
        if (vaddr >= info->size_of_image) continue;
        if (n > info->size_of_image - vaddr)
            n = info->size_of_image - vaddr;
        if ((size_t)rptr >= file_size) continue;
        if (n > file_size - rptr) n = file_size - rptr;

        memcpy(image + vaddr, file + rptr, n);
    }
    return 0;
}

int x2rw_load_pe(const char *game_dir, const char *name, uint32_t base,
                 int primary, x2rw_pe *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->base = base;
    out->primary = primary;

    size_t got = 0;
    uint8_t *file = x2rw_read_game_file(game_dir, name, &got);
    if (!file || got == 0) {
        if (file) x2rw_mem_free_impl(file);
        return -1;
    }

    x2rw_pe_info info;
    if (x2rw_pe_probe(file, got, &info) != 0) {
        x2rw_mem_free_impl(file);
        return -1;
    }

    /* O PE é carregado na ImageBase com que foi linkado — não aplicamos
     * relocations. Se a ImageBase do arquivo não for `base`, todos os
     * ponteiros absolutos embutidos apontariam para o lugar errado. Falhar
     * aqui é muito melhor que carregar e quebrar longe da causa. */
    if (info.image_base != base) {
        x2rw_mem_free_impl(file);
        return -2;
    }

    /* A imagem tem o tamanho de SizeOfImage (não o do arquivo) e começa
     * ZERADA: o trecho entre o fim dos dados crus de cada seção e o fim da
     * seção é "BSS" e precisa ser zero. malloc não zera. */
    uint8_t *image = (uint8_t *)calloc(1, info.size_of_image);
    if (!image) {
        x2rw_mem_free_impl(file);
        return -1;
    }
    x2rw_build_image(file, got, &info, image);
    x2rw_mem_free_impl(file);          /* a imagem é o que fica */

    if (x2rw_mem_map(base, image, (size_t)info.size_of_image) != 0) {
        x2rw_mem_free_impl(image);
        return -1;
    }

    out->image = image;
    out->image_size = info.size_of_image;
    out->file_size = got;
    out->entry_rva = info.entry_rva;
    return 0;
}

void x2rw_free_image(x2rw_pe *pe) {
    if (!pe || !pe->image) return;
    x2rw_mem_free_impl(pe->image);
    pe->image = NULL;
    pe->image_size = 0;
}

#else
typedef int x2rw_loader_inc_sem_impl_t; /* evita unidade de tradução vazia */
#endif /* X2WR_LOADER_IMPL */
