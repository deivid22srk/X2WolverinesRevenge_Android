package br.com.x2wr

import android.content.Context
import android.content.SharedPreferences
import android.net.Uri
import android.provider.DocumentsContract
import androidx.documentfile.provider.DocumentFile
import java.io.File
import java.io.InputStream
import java.security.MessageDigest

/**
 * Validação da pasta do jogo e memória da última pasta aceita.
 *
 * Decisão sobre persistência: usar SharedPreferences, NÃO DataStore. O
 * requisito é guardar UM caminho e um booleano. O DataStore traz corrotinas,
 * fluxos e um artefato a mais para um caso que não precisa de nenhum dos
 * dois; SharedPreferences.apply() é suficiente e não adiciona dependência.
 *
 * Decisão sobre o SHA-1: divergência de hash é AVISO, nunca recusa. O
 * projeto assume a release NoCD fornecida (37f4327c...), mas o usuário pode
 * ter uma cópia legítima diferente; travar aqui impediria alguém de jogar a
 * própria cópia. Os arquivos obrigatórios (Wolvie.exe, binkw32.dll) é que
 * definem se a pasta serve.
 */
object GameFolder {

    /** SHA-1 da release verificada (docs/RECON.md / tools/games.py). */
    const val WOLVIE_SHA1 = "37f4327c4ea45d2037e9e177ecbac003469ff490"

    /** Presentes em qualquer cópia jogável: sem eles não há o que executar. */
    private val REQUIRED = listOf("Wolvie.exe", "binkw32.dll")

    /** Containers de asset. Ausentes, o jogo abre mas não carrega conteúdo. */
    private val OPTIONAL = listOf("data.pkr", "wavs.pkr")

    /** Nomes dos binários embutidos em assets/game/, copiados para o
     *  armazenamento privado quando o usuário NÃO aponta uma pasta real. */
    private val BUNDLED = listOf("Wolvie.exe", "binkw32.dll")

    private const val PREFS = "x2wr_prefs"
    private const val KEY_TREE = "game_tree_uri"
    private const val KEY_PATH = "game_fs_path"

    // ------------------------------------------------------------------
    // Modelo de resultado
    // ------------------------------------------------------------------

    /**
     * Pasta pronta para uso.
     *
     * @param nativePath caminho de FILESYSTEM que a camada nativa (C) pode
     *   abrir com fopen(). Nunca é um content:// — ver [toFilesystemPath].
     * @param wolvieSha1 SHA-1 calculado, ou null se não foi possível ler.
     * @param hashMatches true/false quando [wolvieSha1] existe, null quando
     *   o hash não pôde ser calculado.
     */
    data class Ok(
        val displayName: String,
        val nativePath: String,
        val treeUri: Uri?,
        val presentOptional: List<String>,
        val missingOptional: List<String>,
        val wolvieSha1: String?,
        val hashMatches: Boolean?
    )

    /** Pasta recusada, com mensagem pronta para exibição (pt-BR). */
    data class Failure(val message: String)

    /** Resultado da validação + do cálculo de hash (feito fora da UI). */
    data class Outcome(val ok: Ok?, val failure: Failure?) {
        val isOk: Boolean get() = ok != null
    }

    // ------------------------------------------------------------------
    // Memória da pasta
    // ------------------------------------------------------------------

    private fun prefs(ctx: Context): SharedPreferences =
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    fun rememberTreeUri(ctx: Context, uri: Uri?) {
        prefs(ctx).edit().apply {
            if (uri == null) remove(KEY_TREE) else putString(KEY_TREE, uri.toString())
            apply()
        }
    }

    fun rememberFilesystemPath(ctx: Context, path: String?) {
        prefs(ctx).edit().apply {
            if (path.isNullOrBlank()) remove(KEY_PATH) else putString(KEY_PATH, path)
            apply()
        }
    }

    fun rememberedTreeUri(ctx: Context): Uri? =
        prefs(ctx).getString(KEY_TREE, null)?.let(Uri::parse)

    fun rememberedFilesystemPath(ctx: Context): String? =
        prefs(ctx).getString(KEY_PATH, null)

    /** Caminho lembrado, na forma mais utilizável para o launcher. */
    fun rememberedDisplay(ctx: Context): String? =
        rememberedFilesystemPath(ctx) ?: rememberedTreeUri(ctx)?.toString()

    // ------------------------------------------------------------------
    // Validação
    // ------------------------------------------------------------------

    /**
     * Valida uma pasta escolhida pelo usuário via Storage Access Framework.
     * Exige permissão persistente já tomada por quem chamou (o
     * MainActivity chama takePersistableUriPermission).
     *
     * IMPORTANTE: a validação por SAF funciona para QUALQUER provedor
     * (inclusive nuvem). Só o caminho devolvido para o C pode ficar nulo —
     * nesse caso a camada nativa não consegue abrir o PE e o launcher
     * avisa. DocumentFile é usado aqui porque ele é a única API correta
     * para um tree URI.
     */
    fun validateTree(ctx: Context, treeUri: Uri): Outcome {
        val root = try {
            DocumentFile.fromTreeUri(ctx, treeUri)
        } catch (e: Exception) {
            null
        } ?: return Outcome(null, Failure("Não foi possível abrir a pasta escolhida."))

        if (!root.isDirectory) {
            return Outcome(null, Failure("O item escolhido não é uma pasta."))
        }

        val missingRequired = REQUIRED.filter { findChild(root, it) == null }
        if (missingRequired.isNotEmpty()) {
            return Outcome(
                null,
                Failure(
                    "Pasta do jogo inválida: falta(m) " +
                        missingRequired.joinToString(", ") +
                        ". Escolha a pasta que contém Wolvie.exe e binkw32.dll."
                )
            )
        }

        val presentOptional = OPTIONAL.filter { findChild(root, it) != null }
        // Ausência de PKR é AVISO, não erro: o PKR é conteúdo; sem ele o
        // jogo abre mas não carrega fase (ver docs/RECON.md, PKR3). A
        // mensagem viaja em Ok.missingOptional e o launcher a exibe.
        val missingOptional = OPTIONAL - presentOptional.toSet()

        val wolvie = findChild(root, "Wolvie.exe")!!
        val sha1 = try {
            hashDocument(ctx, wolvie.uri)
        } catch (e: Exception) {
            null
        }

        return Outcome(
            Ok(
                displayName = root.name ?: treeUri.lastPathSegment ?: treeUri.toString(),
                nativePath = toFilesystemPath(ctx, treeUri) ?: "",
                treeUri = treeUri,
                presentOptional = presentOptional,
                missingOptional = missingOptional,
                wolvieSha1 = sha1,
                hashMatches = sha1?.equals(WOLVIE_SHA1, ignoreCase = true)
            ),
            null
        )
    }

    /** Valida um caminho plano de filesystem (ex.: /sdcard/X2). */
    fun validatePath(path: String): Outcome {
        val dir = File(path)
        if (!dir.isDirectory) {
            return Outcome(null, Failure("A pasta \"$path\" não existe ou não é acessível."))
        }
        val missingRequired = REQUIRED.filter { !File(dir, it).isFile }
        if (missingRequired.isNotEmpty()) {
            return Outcome(
                null,
                Failure(
                    "Pasta do jogo inválida: falta(m) " +
                        missingRequired.joinToString(", ") +
                        ". Escolha a pasta que contém Wolvie.exe e binkw32.dll."
                )
            )
        }
        val presentOptional = OPTIONAL.filter { File(dir, it).isFile }
        val missingOptional = OPTIONAL - presentOptional.toSet()
        val wolvie = File(dir, "Wolvie.exe")
        val sha1 = try {
            hashFile(wolvie)
        } catch (e: Exception) {
            null
        }
        return Outcome(
            Ok(
                displayName = dir.name,
                nativePath = dir.absolutePath,
                treeUri = null,
                presentOptional = presentOptional,
                missingOptional = missingOptional,
                wolvieSha1 = sha1,
                hashMatches = sha1?.equals(WOLVIE_SHA1, ignoreCase = true)
            ),
            null
        )
    }

    // ------------------------------------------------------------------
    // Caminho para a camada nativa
    // ------------------------------------------------------------------

    /**
     * Traduz um tree URI para um caminho que o C consegue abrir com fopen().
     *
     * Por que isso não é trivia: o C não fala SAF. A conversão só é possível
     * quando o provedor é o armazenamento externo local
     * ("com.android.externalstorage.documents"), caso em que o documentId
     * carrega o caminho (ex.: "primary:X2"). Para qualquer outro provedor
     * (Downloads, Google Drive, SD via provider de terceiros) devolvemos
     * null: o correto é o launcher dizer ao usuário para escolher a pasta
     * pelo armazenamento, ou copiar os binários para o espaço do app — ver
     * [copyBundledBinaries], que é o caminho alternativo.
     */
    fun toFilesystemPath(ctx: Context, treeUri: Uri): String? {
        if (treeUri.scheme == "file") return treeUri.path
        if (treeUri.authority != "com.android.externalstorage.documents") {
            // Ainda tentamos o caminho "cru", que alguns provedores publicam;
            // se não existir, quem chama trata.
            return treeUri.path?.takeIf { File(it).isDirectory }
        }
        val docId = try {
            DocumentsContract.getTreeDocumentId(treeUri)
        } catch (e: Exception) {
            return null
        } ?: return null

        val parts = docId.split(':')
        if (parts.size != 2) return null
        val volume = parts[0]
        val relative = parts[1]
        val base = when (volume) {
            "primary" -> "/storage/emulated/0"
            else -> "/storage/$volume"
        }
        val full = if (relative.isEmpty()) base else "$base/$relative"
        return if (File(full).isDirectory) full else null
    }

    // ------------------------------------------------------------------
    // Binários embutidos (assets/game/)
    // ------------------------------------------------------------------

    /**
     * Copia Wolvie.exe e binkw32.dll de assets/game/ para
     * filesDir/port/game/, devolvendo o diretório — ou null se os assets não
     * existem neste APK.
     *
     * Por que copiar e não ler do AssetManager: assets dentro do APK são
     * comprimidos e NÃO são mapeáveis por mmap; o loader precisa do arquivo
     * PE inteiro em memória e o C do port já lê de um caminho de arquivo.
     * Copiar uma vez, na primeira execução, é mais simples e mantém o C
     * independente de JNI/AAsset.
     *
     * Escopo: apenas os dois binários executáveis (alguns MB). Os PKR NÃO
     * são copiados — são centenas de MB e o usuário deve apontar a pasta
     * real para jogar de verdade.
     */
    fun copyBundledBinaries(ctx: Context): String? {
        val dest = File(ctx.filesDir, "port/game")
        if (!dest.isDirectory && !dest.mkdirs()) return null

        var copied = 0
        for (name in BUNDLED) {
            val target = File(dest, name)
            if (target.isFile && target.length() > 0) {
                copied++
                continue
            }
            val ok = try {
                ctx.assets.open("game/$name").use { input ->
                    target.outputStream().use { output -> input.copyTo(output) }
                }
                true
            } catch (e: Exception) {
                false
            }
            if (!ok) {
                // Limpa um arquivo parcial: um PE truncado é pior que ausente.
                target.delete()
            } else {
                copied++
            }
        }
        return if (copied == BUNDLED.size) dest.absolutePath else null
    }

    // ------------------------------------------------------------------
    // Hash
    // ------------------------------------------------------------------

    fun hashFile(file: File): String = file.inputStream().use { hashStream(it) }

    private fun hashDocument(ctx: Context, uri: Uri): String =
        ctx.contentResolver.openInputStream(uri).use { input ->
            if (input == null) "" else hashStream(input)
        }

    /** SHA-1 em blocos: os binários têm alguns MB, mas ler tudo de uma vez
     *  duplicaria a memória à toa. Chamar SEMPRE fora da thread de UI. */
    private fun hashStream(input: InputStream): String {
        val md = MessageDigest.getInstance("SHA-1")
        val buf = ByteArray(1 shl 16)
        while (true) {
            val n = input.read(buf)
            if (n <= 0) break
            md.update(buf, 0, n)
        }
        return md.digest().joinToString("") { "%02x".format(it) }
    }

    // ------------------------------------------------------------------
    // Utilitários
    // ------------------------------------------------------------------

    /** Busca um filho por nome sem diferenciar maiúsculas (o PKR original é
     *  minúsculo, o EXE é capitalizado — e alguns provedores normalizam). */
    private fun findChild(dir: DocumentFile, name: String): DocumentFile? {
        dir.findFile(name)?.let { return it }
        // findFile é case-sensitive em alguns provedores; varredura é o
        // fallback. Poucos arquivos na raiz, custo irrelevante.
        return dir.listFiles().firstOrNull { it.name.equals(name, ignoreCase = true) }
    }

    /** Nome de exibição curto para uma árvore escolhida. */
    fun displayNameOf(ctx: Context, uri: Uri): String =
        try {
            DocumentFile.fromTreeUri(ctx, uri)?.name ?: uri.lastPathSegment ?: uri.toString()
        } catch (e: Exception) {
            uri.toString()
        }
}
