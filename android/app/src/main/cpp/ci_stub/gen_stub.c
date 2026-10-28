/* X2: Wolverine's Revenge - port Android
 * Placeholder do build APP-ONLY.
 *
 * POR QUE ISTO EXISTE:
 * o CMake deste projeto aceita dois modos. No modo FULL ele compila o
 * código lifted (gen/*.c, derivado do binário do jogo, mantido no
 * repositório PRIVADO). No modo APP-ONLY — o único possível no repositório
 * público — gen/ não existe, e mesmo assim o alvo precisa de pelo menos uma
 * unidade de tradução própria para ser um alvo válido e para o linker ter
 * onde ancorar o símbolo.
 *
 * O QUE ESTE ARQUIVO NÃO É:
 * não é um stub do jogo. Ele não contém tabela de funções, não contém
 * entrypoint e não finge executar nada. Quando ele é compilado, o CMake NÃO
 * define X2WR_HAVE_GEN, e por isso `nativeRun` retorna 2 (build sem código
 * lifted) sem nunca chamar x2rw_call(). Ver x2rw_android_main.c.
 *
 * O símbolo é `weak` de propósito: se um dia o gen/ trouxer um símbolo com
 * este nome (não deve), o do gen/ vence sem erro de link duplicado.
 */
void x2wr_ci_stub_placeholder(void) __attribute__((weak));

void x2wr_ci_stub_placeholder(void) {
    /* Intencionalmente vazio: este arquivo existe só para satisfazer o
     * build app-only. Nada deve chamá-lo. */
}
