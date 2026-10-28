# Regras do ProGuard/R8 para o :app.
#
# O build não habilita minificação (isMinifyEnabled = false nos dois tipos),
# então este arquivo é propositalmente quase vazio. Ele existe porque o
# template do AGP o referencia e porque, no dia em que a minificação for
# ligada, os pontos abaixo precisam sobreviver — R8 renomeia/remove classes
# e os nomes JNI passariam a não casar com os símbolos de libx2wr.so.
#
# Se isMinifyEnabled virar true, preserve a classe que declara os `external`:
-keep class br.com.x2wr.MainActivity { *; }
