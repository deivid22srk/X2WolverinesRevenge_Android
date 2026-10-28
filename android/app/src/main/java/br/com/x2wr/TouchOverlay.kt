package br.com.x2wr

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.view.MotionEvent
import android.view.View
import kotlin.math.hypot
import kotlin.math.min

/**
 * Overlay de controles de toque desenhado sobre o SurfaceView.
 *
 * DECISÕES (e o porquê):
 *
 * 1. Os códigos enviados ao nativo são CÓDIGOS VIRTUAIS DO WINDOWS, não
 *    scancodes de hardware. O mapeamento abaixo foi verificado no
 *    Wolvie.INI da cópia fornecida; é o que o DirectInput do jogo consulta,
 *    então é o que o host precisa enxergar. Não converta para outra coisa.
 *
 * 2. Multi-toque por ID de ponteiro. Um `Set` de códigos não serve: com dois
 *    dedos no mesmo botão, o primeiro UP liberaria a tecla com o segundo
 *    ainda pressionado. Guardamos ponteiro -> código e só liberamos quando
 *    não sobra nenhum ponteiro com aquele código. O direcional é o único
 *    caso em que um ponteiro segura DUAS teclas (diagonal), então ele vive
 *    num mapa separado ([dpadHeld]).
 *
 * 3. O analógico esquerdo é um "stick virtual": ele NÃO envia eixos (não há
 *    caminho de gamepad nos shims). Ele converte o arrasto em teclas de
 *    seta com zona morta — utilizável hoje, sem depender de shim novo.
 *
 * 4. O desenho usa Canvas puro, sem assets: qualquer bitmap exigiria arte
 *    licenciada que este repositório não tem.
 *
 * Este arquivo NÃO implementa lógica do jogo: só traduz toque em teclas.
 * É a mesma interface que um gamepad físico usará quando existir.
 */
class TouchOverlay(context: Context) : View(context) {

    private companion object {
        // Mapeamento verificado no Wolvie.INI (códigos virtuais do Windows).
        const val K_PUNCH = 75      // 'K'
        const val K_KICK = 72       // 'H'
        const val K_JUMP = 57       // '9'
        const val K_ACTION = 77     // 'M'
        const val K_CLAWS = 71      // 'G'
        const val K_CROUCH = 82     // 'R'
        const val K_STEALTH = 42    // PrintScreen
        const val K_SENSES = 156    // VK_OEM_102
        const val K_CAMERA = 76     // 'L'

        // Setas.
        const val K_LEFT = 37
        const val K_UP = 38
        const val K_RIGHT = 39
        const val K_DOWN = 40

        const val STICK_DEADZONE = 0.22f
    }

    /** Botão circular: posição relativa (0..1 da tela), rótulo e código VK. */
    private data class Button(
        val label: String,
        val code: Int,
        val rx: Float,
        val ry: Float,
        val secondary: Boolean = false
    )

    private val buttons: List<Button> = listOf(
        // Lado direito: as quatro ações principais, em losango.
        Button("PUNHO", K_PUNCH, 0.885f, 0.52f),
        Button("CHUTE", K_KICK, 0.775f, 0.40f),
        Button("PULO", K_JUMP, 0.775f, 0.64f),
        Button("AÇÃO", K_ACTION, 0.665f, 0.52f),
        // Lado esquerdo, acima do direcional: Garras/Frente/Furtivo.
        Button("GARRAS", K_CLAWS, 0.10f, 0.17f),
        Button("FRENTE", K_CROUCH, 0.22f, 0.115f),
        Button("FURTIVO", K_STEALTH, 0.34f, 0.17f),
        Button("SENTIDOS", K_SENSES, 0.10f, 0.315f),
        Button("CÂMERA", K_CAMERA, 0.22f, 0.375f),
        // Canto superior direito: funções menos usadas, laranja (secondary).
        Button("SENTIDOS", K_SENSES, 0.90f, 0.13f, secondary = true),
        Button("FURTIVO", K_STEALTH, 0.80f, 0.095f, secondary = true)
    )

    /** D-pad: um controle em cruz, separado da lista acima. */
    private val dpadRx = 0.22f
    private val dpadRy = 0.78f

    // ------------------------------------------------------------------
    // Estado de toque
    // ------------------------------------------------------------------

    private val pointerToCode = HashMap<Int, Int>()
    private val dpadHeld = HashMap<Int, List<Int>>()

    private var stickPointer = -1
    private var stickOriginX = 0f
    private var stickOriginY = 0f
    private var stickX = 0f
    private var stickY = 0f
    private var stickHeld: List<Int> = emptyList()

    // ------------------------------------------------------------------
    // Tinta (criada uma vez: onDraw roda a cada frame)
    // ------------------------------------------------------------------

    private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = Color.argb(70, 0, 0, 0)
    }
    private val fillActive = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = Color.argb(130, 30, 140, 200)
    }
    private val fillSecondary = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = Color.argb(55, 200, 170, 30)
    }
    private val fillArrow = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = Color.argb(120, 255, 255, 255)
    }
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        color = Color.argb(150, 255, 255, 255)
    }
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        textAlign = Paint.Align.CENTER
        isFakeBoldText = true
    }

    /** Habilita/desabilita o overlay. Enquanto o launcher está na tela, os
     *  toques pertencem aos botões da UI, não ao jogo. */
    var controlsEnabled: Boolean = false
        set(value) {
            field = value
            if (!value) releaseAll()
            visibility = if (value) VISIBLE else GONE
            invalidate()
        }

    init {
        isFocusable = false
        isClickable = false
        setBackgroundColor(Color.TRANSPARENT)
    }

    // ------------------------------------------------------------------
    // Geometria
    // ------------------------------------------------------------------

    private fun buttonRadius(): Float = min(width, height) * 0.078f

    private fun centerX(b: Button) = width * b.rx
    private fun centerY(b: Button) = height * b.ry

    private fun dpadRadius(): Float = min(width, height) * 0.115f
    private fun dpadCenterX() = width * dpadRx
    private fun dpadCenterY() = height * dpadRy

    private fun stickRadius(): Float = min(width, height) * 0.13f
    private fun stickCenterX() = width * 0.20f
    private fun stickCenterY() = height * 0.62f

    /** Botão de ação sob o ponto, ou null. Varre de trás para frente para
     *  que quem é desenhado por cima ganhe o toque. */
    private fun hitButton(x: Float, y: Float): Int? {
        val r = buttonRadius()
        for (b in buttons.asReversed()) {
            if (hypot(x - centerX(b), y - centerY(b)) <= r) return b.code
        }
        return null
    }

    /** Direções do D-pad sob o ponto. Diagonais devolvem duas setas, que é
     *  exatamente como o teclado se comporta. */
    private fun hitDpad(x: Float, y: Float, fromCenterOnly: Boolean = false): List<Int> {
        val dx = x - dpadCenterX()
        val dy = y - dpadCenterY()
        val r = dpadRadius()
        val limit = if (fromCenterOnly) r * 1.4f else r
        if (hypot(dx, dy) > limit) return emptyList()
        val out = ArrayList<Int>(2)
        val dz = r * 0.30f
        if (dx < -dz) out.add(K_LEFT)
        if (dx > dz) out.add(K_RIGHT)
        if (dy < -dz) out.add(K_UP)
        if (dy > dz) out.add(K_DOWN)
        return out
    }

    private fun inStickZone(x: Float, y: Float): Boolean =
        hypot(x - stickCenterX(), y - stickCenterY()) <= stickRadius() * 1.35f

    // ------------------------------------------------------------------
    // Teclas
    // ------------------------------------------------------------------

    private fun pressCode(code: Int) = MainActivity.nativeSetKey(code, true)
    private fun releaseCode(code: Int) = MainActivity.nativeSetKey(code, false)

    private fun releaseAll() {
        for (code in pointerToCode.values.toSet()) releaseCode(code)
        pointerToCode.clear()
        for (codes in dpadHeld.values) for (c in codes) releaseCode(c)
        dpadHeld.clear()
        releaseStickDirections()
        stickPointer = -1
        invalidate()
    }

    private fun applyDpad(pointerId: Int, codes: List<Int>) {
        val old = dpadHeld[pointerId] ?: emptyList()
        for (c in old) {
            if (c in codes) continue
            val stillHeld = dpadHeld.filterKeys { it != pointerId }.values.any { c in it }
            if (!stillHeld) releaseCode(c)
        }
        for (c in codes) if (c !in old) pressCode(c)
        dpadHeld[pointerId] = codes
    }

    private fun clearDpad(pointerId: Int) {
        val old = dpadHeld.remove(pointerId) ?: return
        for (c in old) {
            val stillHeld = dpadHeld.values.any { c in it }
            if (!stillHeld) releaseCode(c)
        }
    }

    // ------------------------------------------------------------------
    // Toque
    // ------------------------------------------------------------------

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (!controlsEnabled) return false

        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val i = event.actionIndex
                handleDown(event.getPointerId(i), event.getX(i), event.getY(i))
            }

            MotionEvent.ACTION_MOVE -> {
                for (i in 0 until event.pointerCount) {
                    handleMove(event.getPointerId(i), event.getX(i), event.getY(i))
                }
            }

            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                handleUp(event.getPointerId(event.actionIndex))
            }

            MotionEvent.ACTION_CANCEL -> releaseAll()
        }
        invalidate()
        return true
    }

    private fun handleDown(id: Int, x: Float, y: Float) {
        // Ordem de prioridade: botão > direcional > analógico > nada.
        hitButton(x, y)?.let { code ->
            pointerToCode[id] = code
            pressCode(code)
            return
        }
        val dirs = hitDpad(x, y)
        if (dirs.isNotEmpty()) {
            applyDpad(id, dirs)
            return
        }
        if (stickPointer == -1 && inStickZone(x, y)) {
            stickPointer = id
            stickOriginX = x
            stickOriginY = y
            stickX = x
            stickY = y
        }
        // Toque solto é ignorado de propósito: virar tecla aleatória seria
        // pior que não fazer nada.
    }

    private fun handleMove(id: Int, x: Float, y: Float) {
        if (id == stickPointer) {
            stickX = x
            stickY = y
            updateStickDirections()
            return
        }
        if (dpadHeld.containsKey(id)) {
            applyDpad(id, hitDpad(x, y, fromCenterOnly = true))
            return
        }
        // Dedo que saiu do botão continua pressionando: soltar ao sair
        // deixaria a ação "presa" no meio de um combo.
    }

    private fun handleUp(id: Int) {
        if (id == stickPointer) {
            stickPointer = -1
            releaseStickDirections()
            return
        }
        if (dpadHeld.containsKey(id)) {
            clearDpad(id)
            return
        }
        val code = pointerToCode.remove(id) ?: return
        if (!pointerToCode.containsValue(code)) releaseCode(code)
    }

    // ------------------------------------------------------------------
    // Analógico -> setas
    // ------------------------------------------------------------------

    private fun updateStickDirections() {
        val r = stickRadius()
        val dx = (stickX - stickOriginX) / r
        val dy = (stickY - stickOriginY) / r
        val out = ArrayList<Int>(2)
        if (dx < -STICK_DEADZONE) out.add(K_LEFT)
        if (dx > STICK_DEADZONE) out.add(K_RIGHT)
        if (dy < -STICK_DEADZONE) out.add(K_UP)
        if (dy > STICK_DEADZONE) out.add(K_DOWN)
        for (c in stickHeld) if (c !in out) releaseCode(c)
        for (c in out) if (c !in stickHeld) pressCode(c)
        stickHeld = out
    }

    private fun releaseStickDirections() {
        for (c in stickHeld) releaseCode(c)
        stickHeld = emptyList()
    }

    // ------------------------------------------------------------------
    // Desenho
    // ------------------------------------------------------------------

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        if (!controlsEnabled) return

        val r = buttonRadius()
        stroke.strokeWidth = r * 0.10f
        text.textSize = r * 0.42f

        for (b in buttons) {
            val cx = centerX(b)
            val cy = centerY(b)
            val active = pointerToCode.containsValue(b.code)
            canvas.drawCircle(
                cx, cy, r,
                if (active) fillActive else if (b.secondary) fillSecondary else fill
            )
            canvas.drawCircle(cx, cy, r, stroke)
            canvas.drawText(b.label, cx, cy + text.textSize * 0.35f, text)
        }

        drawDpad(canvas)
        drawStick(canvas)
    }

    private fun drawDpad(canvas: Canvas) {
        val cx = dpadCenterX()
        val cy = dpadCenterY()
        val r = dpadRadius()
        stroke.strokeWidth = r * 0.10f

        // Cruz: barra horizontal + vertical, cantos arredondados.
        val arm = r * 0.36f
        canvas.drawRoundRect(RectF(cx - r, cy - arm, cx + r, cy + arm), arm, arm, fill)
        canvas.drawRoundRect(RectF(cx - arm, cy - r, cx + arm, cy + r), arm, arm, fill)

        val held = dpadHeld.values.flatten().toSet()
        val s = r * 0.20f
        arrow(canvas, cx, cy - r * 0.66f, 0f, K_UP in held, s)
        arrow(canvas, cx, cy + r * 0.66f, 180f, K_DOWN in held, s)
        arrow(canvas, cx - r * 0.66f, cy, 270f, K_LEFT in held, s)
        arrow(canvas, cx + r * 0.66f, cy, 90f, K_RIGHT in held, s)
    }

    /** Triângulo apontando para `angle` em graus (0 = para cima). */
    private fun arrow(canvas: Canvas, x: Float, y: Float, angle: Float,
                      active: Boolean, size: Float) {
        val p = Path()
        p.moveTo(0f, -size)
        p.lineTo(size * 0.85f, size * 0.75f)
        p.lineTo(-size * 0.85f, size * 0.75f)
        p.close()
        canvas.save()
        canvas.translate(x, y)
        canvas.rotate(angle)
        canvas.drawPath(p, if (active) fillActive else fillArrow)
        canvas.restore()
    }

    private fun drawStick(canvas: Canvas) {
        val cx = stickCenterX()
        val cy = stickCenterY()
        val r = stickRadius()
        stroke.strokeWidth = r * 0.07f
        canvas.drawCircle(cx, cy, r, stroke)
        canvas.drawCircle(cx, cy, r * 0.16f, stroke)

        if (stickPointer != -1) {
            var dx = stickX - stickOriginX
            var dy = stickY - stickOriginY
            val d = hypot(dx, dy)
            if (d > r) {
                dx = dx / d * r
                dy = dy / d * r
            }
            canvas.drawCircle(stickOriginX + dx, stickOriginY + dy, r * 0.32f, fillActive)
        }
    }
}
