package br.com.x2wr

import android.Manifest
import android.content.Intent
import android.content.pm.ActivityInfo
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.Settings
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.lifecycle.lifecycleScope
import br.com.x2wr.databinding.ActivityMainBinding
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Launcher do port.
 *
 * Fluxo: escolher/lembrar a pasta do jogo -> validar (Wolvie.exe +
 * binkw32.dll, SHA-1 como AVISO) -> JOGAR -> `nativeRun` numa thread própria.
 *
 * O que este launcher NÃO faz (honestidade obrigatória, ver docs/TESTED.md):
 *  - não há camada gráfica: o SurfaceView existe para receber/reportar o
 *    tamanho da tela e para dar lugar ao overlay; nada é desenhado nele
 *    ainda. Por isso ele começa INVISÍVEL/útil apenas quando o jogo roda.
 *  - não há áudio nem gamepad físico;
 *  - num APK "app-only" (sem gen/) `nativeRun` devolve 2 e o launcher diz
 *    exatamente isso ao usuário em vez de fingir que jogou.
 */
class MainActivity : AppCompatActivity() {

    companion object {
        init {
            // Carrega libx2wr.so. A falha é deixada propagar: um APK sem a
            // .so não tem como funcionar, e engolir o erro só adiaria o
            // diagnóstico. O nome precisa casar com o alvo CMake `x2wr`.
            System.loadLibrary("x2wr")
        }

        /** Pasta do jogo, o tamanho da tela e as teclas do overlay. */
        @JvmStatic external fun nativeSetGameDir(path: String?)
        @JvmStatic external fun nativeSetRenderSize(w: Int, h: Int)
        @JvmStatic external fun nativeSetKey(scancode: Int, down: Boolean)

        /** true se este APK contém o código lifted (gen/). */
        @JvmStatic external fun nativeHasGameCode(): Boolean

        /**
         * Executa o jogo. BLOQUEIA até o entrypoint retornar — chame sempre
         * numa thread própria.
         * Retorno: 0 = ok, 1 = falha de preparação, 2 = build sem código
         * lifted, 3 = fault do guest (ver port/log.txt).
         */
        @JvmStatic external fun nativeRun(): Int
    }

    private lateinit var binding: ActivityMainBinding
    private lateinit var overlay: TouchOverlay

    private val ui = Handler(Looper.getMainLooper())

    /** Pasta validada e pronta para uso (null = nada escolhido ainda). */
    private var ready: GameFolder.Ok? = null

    /** Impede dois `nativeRun` simultâneos (o contexto do guest não é
     *  reentrante e o jogo reivindica o processo). */
    @Volatile private var running = false

    // ------------------------------------------------------------------
    // Ciclo de vida
    // ------------------------------------------------------------------

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        // O jogo é 3D de 2003: fixar paisagem evita reprojetar a tela no
        // meio da execução. `fullSensor` no manifesto permite as duas, e a
        // Activity restringe só para escolher a inicial.
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE

        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        overlay = TouchOverlay(this)
        binding.overlayHost.addView(
            overlay,
            ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            )
        )

        binding.surfaceView.holder.addCallback(object : android.view.SurfaceHolder.Callback {
            override fun surfaceCreated(holder: android.view.SurfaceHolder) = reportSurfaceSize()
            override fun surfaceChanged(
                holder: android.view.SurfaceHolder, format: Int, width: Int, height: Int
            ) = reportSurfaceSize()
            override fun surfaceDestroyed(holder: android.view.SurfaceHolder) = Unit
        })

        binding.buttonPick.setOnClickListener { pickFolder() }
        binding.buttonSave.setOnClickListener { saveCurrent() }
        binding.buttonPlay.setOnClickListener { play() }

        // Aviso sobre o build: melhor dizer ANTES do que devolver erro 2.
        if (!nativeHasGameCode()) {
            binding.textBuildMode.text = getString(R.string.build_app_only)
            binding.textBuildMode.visibility = View.VISIBLE
        }

        restoreRememberedFolder()
    }

    override fun onResume() {
        super.onResume()
        goImmersive()
        // Voltando da tela de permissão: se o usuário concedeu, continua o
        // que ele pediu (JOGAR) em vez de obrigá-lo a tocar de novo.
        if (jogarPendente) tentarJogarPendente()
        // O overlay só deve capturar toque quando o jogo está rodando; na
        // tela do launcher, os botões da UI é que importam.
        overlay.controlsEnabled = running
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) goImmersive()
    }

    /** Esconde barras do sistema. O jogo ocupa a tela inteira e qualquer
     *  barra visível muda o tamanho do SurfaceView no meio da execução. */
    private fun goImmersive() {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior =
                WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    private fun reportSurfaceSize() {
        val v = binding.surfaceView
        if (v.width > 0 && v.height > 0) {
            nativeSetRenderSize(v.width, v.height)
        }
    }

    // ------------------------------------------------------------------
    // Pasta do jogo
    // ------------------------------------------------------------------

    private val openTree = registerForActivityResult(
        ActivityResultContracts.OpenDocumentTree()
    ) { uri: Uri? ->
        if (uri == null) {
            setStatus(getString(R.string.status_pick_cancelled))
            // `return` puro nao e permitido dentro do lambda de
            // registerForActivityResult; sem o rotulo o compilador tambem
            // nao considera o ramo terminante e `uri` fica Uri? (erros
            // 168/174/183 medidos no run 40735670541).
            return@registerForActivityResult
        }
        // Sem permissão persistente o acesso morre quando o processo é
        // recriado — e o jogo roda por muito tempo.
        try {
            contentResolver.takePersistableUriPermission(
                uri, Intent.FLAG_GRANT_READ_URI_PERMISSION
            )
        } catch (e: SecurityException) {
            // Alguns provedores não oferecem permissão persistente; segue
            // com a permissão temporária e avisa no status.
            Toast.makeText(this, R.string.warn_no_persist, Toast.LENGTH_LONG).show()
        }
        GameFolder.rememberTreeUri(this, uri)
        GameFolder.rememberFilesystemPath(this, null)
        validateTreeAsync(uri)
    }

    private fun pickFolder() {
        // Sugere a última pasta, quando o provedor aceita um hint.
        val remembered = GameFolder.rememberedTreeUri(this)
        try {
            openTree.launch(remembered)
        } catch (e: Exception) {
            openTree.launch(null)
        }
    }

    /** Ao abrir: se a pasta lembrada ainda valida, mostra JOGAR direto. */
    private fun restoreRememberedFolder() {
        val path = GameFolder.rememberedFilesystemPath(this)
        val uri = GameFolder.rememberedTreeUri(this)
        if (path != null) {
            validatePathAsync(path)
        } else if (uri != null) {
            validateTreeAsync(uri)
        } else {
            setStatus(getString(R.string.status_pick_folder))
            updateReadyUi()
        }
    }

    private fun validateTreeAsync(uri: Uri) {
        setStatus(getString(R.string.status_validating))
        lifecycleScope.launch {
            // Hash de alguns MB: nunca na thread de UI.
            val outcome = withContext(Dispatchers.IO) {
                GameFolder.validateTree(this@MainActivity, uri)
            }
            applyOutcome(outcome)
        }
    }

    private fun validatePathAsync(path: String) {
        setStatus(getString(R.string.status_validating))
        lifecycleScope.launch {
            val outcome = withContext(Dispatchers.IO) {
                GameFolder.validatePath(path)
            }
            applyOutcome(outcome)
        }
    }

    private fun applyOutcome(outcome: GameFolder.Outcome) {
        val failure = outcome.failure
        if (failure != null) {
            ready = null
            setStatus(failure.message)
            updateReadyUi()
            return
        }

        var ok = outcome.ok!!
        // Pasta válida mas sem caminho de filesystem (provedor sem
        // documentId de armazenamento): a camada nativa não consegue abrir
        // o PE. Tentamos os binários embutidos em assets/game/, mas NÃO
        // silenciosamente: o usuário precisa saber que só os binários do
        // APK serão usados (os PKR não são copiados).
        if (ok.nativePath.isBlank()) {
            val extracted = GameFolder.copyBundledBinaries(this)
            if (extracted != null) {
                val bundled = GameFolder.validatePath(extracted)
                if (bundled.isOk) {
                    ok = bundled.ok!!.copy(displayName = ok.displayName)
                    setStatus(getString(R.string.status_bundled_binaries))
                } else {
                    ready = null
                    setStatus(getString(R.string.status_no_native_path))
                    updateReadyUi()
                    return
                }
            } else {
                ready = null
                setStatus(getString(R.string.status_no_native_path))
                updateReadyUi()
                return
            }
        }

        ready = ok
        reportSha(ok)
        reportOptionals(ok)
        // Persiste o caminho já resolvido: é ele que o launcher reusa.
        if (ok.nativePath.isNotBlank()) {
            GameFolder.rememberFilesystemPath(this, ok.nativePath)
        }
        updateReadyUi()
        setStatus(getString(R.string.status_ready, ok.displayName))
    }

    private fun reportSha(ok: GameFolder.Ok) {
        when {
            ok.wolvieSha1.isNullOrBlank() -> {
                binding.textSha.text = getString(R.string.sha_unavailable)
                binding.textSha.setTextColor(0xFFFFA000.toInt())
            }
            ok.hashMatches == true -> {
                binding.textSha.text = getString(R.string.sha_ok, ok.wolvieSha1)
                binding.textSha.setTextColor(0xFF2E7D32.toInt())
            }
            else -> {
                // AVISO, não erro: cópia legítima diferente da release
                // verificada continua jogável (ver GameFolder).
                binding.textSha.text = getString(R.string.sha_mismatch, ok.wolvieSha1)
                binding.textSha.setTextColor(0xFFC62828.toInt())
            }
        }
    }

    private fun reportOptionals(ok: GameFolder.Ok) {
        binding.textAssets.text = if (ok.missingOptional.isEmpty()) {
            getString(R.string.assets_complete, ok.presentOptional.joinToString(", "))
        } else {
            getString(
                R.string.assets_missing,
                ok.presentOptional.joinToString(", ").ifBlank { "—" },
                ok.missingOptional.joinToString(", ")
            )
        }
    }

    private fun updateReadyUi() {
        val ok = ready
        binding.buttonPlay.isEnabled = ok != null && !running
        binding.buttonSave.isEnabled = ok != null
        binding.textFolder.text = ok?.let {
            getString(R.string.folder_value, it.nativePath.ifBlank { it.displayName })
        } ?: getString(R.string.folder_none)

        if (ok == null) {
            binding.textSha.text = getString(R.string.sha_unavailable)
            binding.textAssets.text = ""
        }
    }

    /** "SALVAR": grava o caminho no nativo e nas preferências. */
    private fun saveCurrent() {
        val ok = ready ?: return
        nativeSetGameDir(ok.nativePath)
        GameFolder.rememberFilesystemPath(this, ok.nativePath)
        Toast.makeText(this, getString(R.string.toast_saved), Toast.LENGTH_SHORT).show()
        setStatus(getString(R.string.status_saved, ok.nativePath))
    }

    // ------------------------------------------------------------------
    // Permissão de armazenamento
    // ------------------------------------------------------------------

    /**
     * Por que isto existe (bug real, medido em aparelho):
     *
     *   I X2WR : pasta do jogo: /storage/emulated/0/Download/...
     *   E X2WR : abrir .../Wolvie.exe: Permission denied
     *   E X2WR : nativeRun: x2rw_host_init falhou   ->  "código 1"
     *
     * No Android 11+ o `stat` em /storage/emulated/0 é liberado mesmo sem
     * permissão, então validar a pasta com isDirectory/isFile dá falso
     * positivo — quem falha é o open(). A correção é: (1) pedir
     * MANAGE_EXTERNAL_STORAGE (API 30+) para manter o I/O por caminho que o
     * jogo usa; (2) antes de lançar, provar a leitura abrindo os arquivos.
     */
    private var jogarPendente = false

    private val pedirLeitura = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { concedida: Boolean ->
        if (concedida) {
            tentarJogarPendente()
        } else {
            setStatus(getString(R.string.err_sem_permissao))
        }
    }

    private fun temAcessoAArquivos(): Boolean {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            Environment.isExternalStorageManager()
        } else {
            checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) ==
                PackageManager.PERMISSION_GRANTED
        }
    }

    private fun pedirAcessoAArquivos() {
        setStatus(getString(R.string.msg_pedir_acesso))
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            // Tela "Todos os arquivos" direto no app; sem a variante por
            // pacote em alguns aparelhos, cai na lista geral.
            val porPacote = Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION)
            porPacote.data = Uri.parse("package:$packageName")
            try {
                startActivity(porPacote)
            } catch (e: Exception) {
                try {
                    startActivity(Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION))
                } catch (e2: Exception) {
                    setStatus(getString(R.string.err_sem_permissao))
                }
            }
        } else {
            pedirLeitura.launch(Manifest.permission.READ_EXTERNAL_STORAGE)
        }
    }

    /** Chamado ao voltar da tela de permissão. */
    private fun tentarJogarPendente() {
        if (!jogarPendente) return
        val ok = ready ?: return
        if (!GameFolder.isReadable(ok.nativePath)) {
            jogarPendente = false
            setStatus(getString(R.string.err_sem_permissao))
            return
        }
        jogarPendente = false
        play()
    }

    // ------------------------------------------------------------------
    // Execução
    // ------------------------------------------------------------------

    private fun play() {
        val ok = ready ?: return
        if (running) return
        // Prova a leitura antes de acordar o código nativo: sem isso a falha
        // aparece como "código 1" genérico, que manda o usuário conferir a
        // pasta — e a pasta estava certa.
        if (!GameFolder.isReadable(ok.nativePath)) {
            jogarPendente = true
            if (!temAcessoAArquivos()) {
                pedirAcessoAArquivos()
            } else {
                jogarPendente = false
                setStatus(getString(R.string.err_sem_permissao))
            }
            return
        }
        running = true
        binding.buttonPlay.isEnabled = false

        nativeSetGameDir(ok.nativePath)
        reportSurfaceSize()

        setStatus(getString(R.string.status_running))
        // Esconde o launcher e liga o overlay: a partir daqui a tela é do
        // jogo (ou do que existir dele, num build app-only).
        binding.launcherPanel.visibility = View.GONE
        overlay.controlsEnabled = true
        binding.surfaceView.visibility = View.VISIBLE

        // `nativeRun` bloqueia; a thread própria evita ANR. Uma IntentService
        // seria pior aqui: o jogo precisa do processo vivo e de acesso ao
        // mesmo contexto nativo, não de trabalho em background do sistema.
        val gameDirForLog = ok.nativePath
        Thread({
            val startedAt = System.currentTimeMillis()
            val rc = try {
                nativeRun()
            } catch (t: Throwable) {
                1
            }
            val elapsed = System.currentTimeMillis() - startedAt
            writeLog(gameDirForLog, rc, elapsed)
            ui.post { onRunFinished(rc, elapsed) }
        }, "x2wr-game").start()
    }

    private fun onRunFinished(rc: Int, elapsedMs: Long) {
        running = false
        overlay.controlsEnabled = false
        binding.launcherPanel.visibility = View.VISIBLE
        binding.surfaceView.visibility = View.GONE
        binding.buttonPlay.isEnabled = ready != null

        val message = when (rc) {
            0 -> getString(R.string.run_ok, elapsedMs / 1000)
            2 -> getString(R.string.run_no_gen)
            3 -> getString(R.string.run_fault)
            else -> getString(R.string.run_fail, rc)
        }
        setStatus(message)
        Toast.makeText(this, message, Toast.LENGTH_LONG).show()
    }

    /**
     * Espelha em filesDir/port/log.txt o que o launcher sabe: escolha da
     * pasta, código de retorno e duração. O log do RUNTIME (x2rw_log) sai
     * no stderr do processo e portanto no logcat com a tag "x2wr" — ele
     * não passa por aqui.
     *
     * TODO(port): quando o host souber escrever em port/log.txt dentro da
     * pasta do jogo (regra do README), este arquivo passa a ser o fallback
     * de armazenamento privado.
     */
    private fun writeLog(gameDir: String, rc: Int, elapsedMs: Long) {
        try {
            val dir = File(filesDir, "port")
            if (!dir.isDirectory && !dir.mkdirs()) return
            val stamp = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date())
            File(dir, "log.txt").appendText(
                buildString {
                    append("[").append(stamp).append("] ")
                    append("pasta=").append(gameDir.ifBlank { "(nenhuma)" })
                    append(" retorno=").append(rc)
                    append(" duracao_ms=").append(elapsedMs)
                    append('\n')
                }
            )
        } catch (e: Exception) {
            // Log é diagnóstico: falhar em escrevê-lo não pode derrubar o app.
        }
    }

    private fun setStatus(text: String) {
        binding.textStatus.text = text
    }
}
