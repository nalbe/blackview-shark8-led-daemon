package com.bastet.ledgui

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.text.InputType
import android.util.TypedValue
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.TextView
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlin.math.roundToInt

/**
 * Shared classic-Views widget kit + a common config-page base, so the
 * three settings tabs (Charge / Notification / Call) share every editor
 * and the load/save/reload plumbing instead of copy-pasting it.
 *
 * A ConfPage carries one ScrollView of cards and a pinned Save/Reload
 * footer. Subclasses only declare their fields and wire them onto the
 * shared LedConf in applyTo()/collectFrom().
 */
abstract class ConfPage(context: Context) : LinearLayout(context) {

    protected val mP = android.view.ViewGroup.LayoutParams.MATCH_PARENT
    protected val wP = android.view.ViewGroup.LayoutParams.WRAP_CONTENT

    protected val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    /** Optional status line card. Only pages that want one (Notification)
     *  create it and assign this; the rest never render a status row. */
    protected var msgTv: TextView? = null

    init {
        orientation = VERTICAL
        setBackgroundColor(parse("#FF121212"))
        buildShell()
        // buildBody()/reload() must run as posts: subclass fields that buildBody()
        // touches are only initialized after this super-constructor returns.
        post { buildBody() }
        post { reload() }
    }

    private fun buildShell() {
        val scroll = ScrollView(context)
        scroll.isFillViewport = true
        val content = LinearLayout(context)
        content.orientation = VERTICAL
        content.setPadding(0, dpi(4), 0, dpi(8))
        scroll.addView(content, LayoutParams(mP, wP))
        addView(scroll, LayoutParams(mP, 0, 1f))
        body = content

        val foot = LinearLayout(context)
        foot.orientation = HORIZONTAL
        foot.setPadding(dpi(12), dpi(8), dpi(12), dpi(8))
        foot.addView(row(filledBtn("Save to device") { save() },
            outlinedBtn("Reload") { reload() }))
        addView(foot, LayoutParams(mP, wP))
    }

    protected lateinit var body: LinearLayout

    protected abstract fun buildBody()
    protected abstract fun applyTo(c: LedConf)
    protected abstract fun collectFrom(c: LedConf)

    /** One-line summary shown after a successful load. Pages show it only
     *  if they created a msgTv card (Notification); others return "" and
     *  render no status row at all. */
    protected open fun loadSummary(c: LedConf): String = ""

    private fun reload() {
        scope.launch {
            var attempts = 0
            while (true) {
                val root = withContext(Dispatchers.IO) { Su.run("id") }
                if (root.ok && root.out.contains("uid=0")) break
                attempts++
                if (attempts > 12) {
                    msgTv?.text = "load skipped: no root yet. Grant root / press Request root on Info, then Reload."
                    return@launch
                }
                delay(1000)
            }
            val c = withContext(Dispatchers.IO) { LedConf.load() }
            applyTo(c)
            loadSummary(c).takeIf { it.isNotEmpty() }?.let { msgTv?.text = it }
        }
    }

    private fun save() {
        scope.launch {
            val c = LedConf()
            collectFrom(c)
            val msg = withContext(Dispatchers.IO) {
                val res = c.save()
                if (res.ok) {
                    Su.run("kill -ALRM \$(pidof chgd) 2>/dev/null")
                    "Saved OK. Daemon reloaded (SIGALRM) - applied now."
                } else {
                    "Save FAILED (exit ${res.code}): ${res.out.ifBlank { "no su / no write?" }}"
                }
            }
            msgTv?.text = msg
        }
    }

    // ------------------------------------------------------------ helpers

    protected fun dpf(v: Float) = v * resources.displayMetrics.density
    protected fun dpf(v: Int) = v * resources.displayMetrics.density
    protected fun dpi(v: Float) = (v * resources.displayMetrics.density).roundToInt()
    protected fun dpi(v: Int) = (v * resources.displayMetrics.density).roundToInt()
    protected fun parse(hex: String) = Color.parseColor(hex)

    protected fun card(builder: LinearLayout.() -> Unit): LinearLayout {
        val c = LinearLayout(context)
        c.orientation = VERTICAL
        c.setBackgroundResource(R.drawable.card_bg)
        c.setPadding(dpi(14), dpi(12), dpi(14), dpi(12))
        val lp = LayoutParams(mP, wP)
        lp.setMargins(dpi(12), dpi(6), dpi(12), dpi(6))
        c.layoutParams = lp
        c.builder()
        return c
    }

    protected fun sectionTitle(s: String): TextView {
        val t = text(s, 15f, parse("#FF90CAF9"), bold = true)
        t.setPadding(0, dpi(2), 0, dpi(2))
        return t
    }

    protected fun text(s: String, size: Float = 14f, color: Int = parse("#FFE0E0E0"),
                       bold: Boolean = false, mono: Boolean = false): TextView {
        val t = TextView(context)
        t.text = s
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, size)
        t.setTextColor(color)
        if (bold) t.setTypeface(null, Typeface.BOLD)
        if (mono) t.typeface = Typeface.MONOSPACE
        return t
    }

    protected fun spacer(h: Int): View {
        val v = View(context)
        v.layoutParams = LayoutParams(1, dpi(h))
        return v
    }

    protected fun row(vararg views: View, gravity: Int = Gravity.CENTER_VERTICAL): LinearLayout {
        val l = LinearLayout(context)
        l.orientation = HORIZONTAL
        l.gravity = gravity
        views.forEachIndexed { i, v ->
            if (i < views.size - 1) {
                val lp = v.layoutParams as? LayoutParams
                    ?: LayoutParams(wP, wP)
                lp.marginEnd = dpi(8)
                v.layoutParams = lp
            }
            l.addView(v)
        }
        return l
    }

    protected fun filledBtn(label: String, onClick: () -> Unit): Button {
        val b = Button(context)
        b.text = label
        b.isAllCaps = false
        b.setTextColor(parse("#FF90CAF9"))
        b.textSize = 14f
        val g = GradientDrawable()
        g.shape = GradientDrawable.RECTANGLE
        g.cornerRadius = dpf(8)
        g.setColor(parse("#FF242424"))
        g.setStroke(dpi(1), parse("#FF5C6BC0"))
        b.background = g
        b.setOnClickListener { onClick() }
        return b
    }

    protected fun outlinedBtn(label: String, onClick: () -> Unit): Button {
        val b = Button(context)
        b.text = label
        b.isAllCaps = false
        b.setTextColor(parse("#FF90CAF9"))
        b.textSize = 14f
        val g = GradientDrawable()
        g.shape = GradientDrawable.RECTANGLE
        g.cornerRadius = dpf(8)
        g.setColor(parse("#FF242424"))
        g.setStroke(dpi(1), parse("#FF5C6BC0"))
        b.background = g
        b.setOnClickListener { onClick() }
        return b
    }

    protected fun multiLine(hint: String): EditText {
        val e = EditText(context)
        e.hint = hint
        e.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
        e.setTextColor(parse("#FFE0E0E0"))
        e.setHintTextColor(parse("#FF727272"))
        e.typeface = Typeface.MONOSPACE
        e.setMinLines(4)
        e.gravity = Gravity.TOP or Gravity.START
        val g = GradientDrawable()
        g.shape = GradientDrawable.RECTANGLE
        g.cornerRadius = dpf(8)
        g.setColor(parse("#FF2A2A2A"))
        g.setStroke(dpi(1), parse("#FF333333"))
        e.background = g
        e.setPadding(dpi(8), dpi(6), dpi(8), dpi(6))
        return e
    }

    protected fun softChk(label: String): CheckBox {
        val c = CheckBox(context)
        c.text = label
        c.setTextColor(parse("#FFB0BEC5"))
        c.textSize = 13f
        c.setPadding(0, dpi(2), 0, dpi(2))
        return c
    }

    // ------------------------------------------------------------ range editor
    // One "range" block = light type + custom-breath checkbox + color. Used
    // three times on the Charge tab with zero duplication.
    protected inner class RangeEditor(
        title: String
    ) : LinearLayout(context) {
        lateinit var type: TypePicker
            private set
        lateinit var soft: CheckBox
            private set
        lateinit var color: RgbPicker
            private set

        init {
            orientation = VERTICAL
            addView(card {
                addView(sectionTitle(title))
                addView(spacer(4))
                type = TypePicker("light type") { sync() }
                addView(type)
                soft = softChk("custom breath (full RGB range)")
                soft.setOnCheckedChangeListener { _, _ -> sync() }
                addView(soft)
                addView(spacer(4))
                color = RgbPicker("color")
                addView(color)
            })
        }

        fun sync() = syncSoft(type.getType(), color, soft)

        fun load(t: Int, c: Triple<Int, Int, Int>, s: Boolean) {
            type.setType(t)
            color.setColor(c)
            soft.isChecked = s
            sync()
        }
    }

    // ------------------------------------------------------------ widgets

    protected inner class NumField(label: String, init: String) : LinearLayout(context) {
        private val edit = EditText(context)

        init {
            orientation = HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            val l = text(label, 13f, parse("#FFB0BEC5"))
            val llp = LayoutParams(dpi(180), wP)
            l.layoutParams = llp
            addView(l)
            edit.setText(init)
            edit.inputType = InputType.TYPE_CLASS_NUMBER
            edit.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
            edit.setTextColor(parse("#FFE0E0E0"))
            edit.setSingleLine(true)
            val g = GradientDrawable()
            g.shape = GradientDrawable.RECTANGLE
            g.cornerRadius = dpf(8)
            g.setColor(parse("#FF2A2A2A"))
            g.setStroke(dpi(1), parse("#FF333333"))
            edit.background = g
            edit.setPadding(dpi(8), dpi(4), dpi(8), dpi(4))
            addView(edit, LayoutParams(0, wP, 1f))
            setPadding(0, dpi(3), 0, dpi(3))
        }

        fun setText(v: String) { edit.setText(v) }
        fun getInt(def: Int): Int = edit.text.toString().toIntOrNull() ?: def
        fun getLong(def: Long): Long = edit.text.toString().toLongOrNull() ?: def
    }

    protected fun LinearLayout.numRow(label: String, init: String): NumField {
        val f = NumField(label, init)
        addView(f)
        return f
    }

    protected inner class TypePicker(
        label: String,
        private val onType: (Int) -> Unit
    ) : LinearLayout(context) {
        private val names = listOf("off", "breathing", "flashing", "static")
        private val group = RadioGroup(context)

        init {
            orientation = VERTICAL
            addView(text(label, 13f, parse("#FFB0BEC5")))
            group.orientation = HORIZONTAL
            names.forEachIndexed { i, n ->
                val rb = RadioButton(context)
                rb.id = i
                rb.text = n
                rb.setTextColor(parse("#FFE0E0E0"))
                rb.setPadding(0, 0, dpi(10), 0)
                group.addView(rb)
            }
            group.setOnCheckedChangeListener { _, _ ->
                onType(group.checkedRadioButtonId.coerceIn(0, names.size - 1))
            }
            addView(group)
            setPadding(0, dpi(3), 0, dpi(3))
        }

        fun setType(t: Int) {
            val idx = t.coerceIn(0, names.size - 1)
            if (group.checkedRadioButtonId != idx) group.check(idx)
        }

        fun getType(): Int = group.checkedRadioButtonId.coerceIn(0, names.size - 1)
    }

    protected inner class RgbPicker(label: String) : LinearLayout(context) {
        private var color = Triple(255, 0, 0)
        private var discrete = false
        private val swatch = View(context)
        private val sliders = mutableListOf<SliderView>()
        private val valueTvs = mutableListOf<TextView>()

        init {
            orientation = VERTICAL
            val head = row(
                text(label, 13f, parse("#FFB0BEC5")),
                swatch
            )
            addView(head)
            val sw = GradientDrawable()
            sw.shape = GradientDrawable.RECTANGLE
            sw.cornerRadius = dpf(4)
            swatch.background = sw
            swatch.layoutParams = LayoutParams(dpi(22), dpi(22))
            val channels = listOf("R", "G", "B")
            channels.forEachIndexed { i, name ->
                val vt = text(intComponent(i).toString(), 13f, parse("#FFE0E0E0"), mono = true)
                vt.layoutParams = LayoutParams(dpi(36), wP)
                val sl = SliderView(channelColor(i), intComponent(i)) { v ->
                    if (discrete) {
                        val s = snap(v)
                        sliders[i].progress = s
                        color = withComponent(i, s)
                    } else {
                        color = withComponent(i, v)
                    }
                    valueTvs[i].text = colorOf(i).toString()
                    paintSwatch()
                }
                sl.layoutParams = LayoutParams(0, dpi(50), 1f)
                sliders.add(sl)
                valueTvs.add(vt)
                addView(row(text("$name", 13f, parse("#FF90CAF9"), bold = true), sl, vt))
            }
            setPadding(0, dpi(3), 0, dpi(3))
            paintSwatch()
        }

        private fun snap(v: Int): Int = if (v < 128) 0 else 255

        private fun colorOf(i: Int): Int = when (i) {
            0 -> color.first; 1 -> color.second; else -> color.third
        }

        fun setDiscrete(on: Boolean) {
            discrete = on
            if (on) {
                color = Triple(snap(color.first), snap(color.second), snap(color.third))
                for (i in 0..2) {
                    sliders[i].progress = colorOf(i)
                    valueTvs[i].text = colorOf(i).toString()
                }
                paintSwatch()
            }
        }

        private fun channelColor(i: Int): Int = when (i) {
            0 -> parse("#FFE05A4E")
            1 -> parse("#FF62B86B")
            else -> parse("#FF4E9BE0")
        }

        private fun intComponent(i: Int): Int = when (i) {
            0 -> color.first; 1 -> color.second; else -> color.third
        }

        private fun withComponent(i: Int, v: Int): Triple<Int, Int, Int> = when (i) {
            0 -> Triple(v, color.second, color.third)
            1 -> Triple(color.first, v, color.third)
            else -> Triple(color.first, color.second, v)
        }

        private fun paintSwatch() {
            val g = swatch.background as? GradientDrawable ?: return
            g.setColor(Color.rgb(color.first, color.second, color.third))
        }

        fun setColor(c: Triple<Int, Int, Int>) {
            color = if (discrete) {
                Triple(snap(c.first), snap(c.second), snap(c.third))
            } else c
            for (i in 0..2) {
                sliders[i].progress = colorOf(i)
                valueTvs[i].text = colorOf(i).toString()
            }
            paintSwatch()
        }

        fun getColor(): Triple<Int, Int, Int> = color
    }

    protected fun syncSoft(type: Int, picker: RgbPicker, soft: CheckBox) {
        soft.visibility = if (type == 1) VISIBLE else GONE
        picker.setDiscrete(type == 1 && !soft.isChecked)
    }

    /** Minimal cheap slider: a single View drawn straight in onDraw. */
    protected inner class SliderView(
        private val channelColor: Int,
        initial: Int,
        private val onMove: (Int) -> Unit
    ) : View(context) {
        var progress: Int = initial.coerceIn(0, 255)
            set(v) {
                field = v.coerceIn(0, 255)
                invalidate()
            }

        private val trackPaint = Paint().apply { color = parse("#FF333A46"); isAntiAlias = true }
        private val fillPaint = Paint().apply { color = channelColor; isAntiAlias = true }
        private val thumbPaint = Paint().apply { color = parse("#FFF0F0F0"); isAntiAlias = true }

        init {
            setMinimumHeight(dpi(50))
            setMinimumWidth(dpi(80))
        }

        override fun onDraw(canvas: Canvas) {
            val t = dpi(6).toFloat()
            val cy = height / 2f
            val pad = dpi(14).toFloat()
            val left = pad
            val right = (width - pad).coerceAtLeast(left + 1f)
            val range = right - left
            val thumbX = left + range * (progress / 255f)
            canvas.drawRoundRect(RectF(left, cy - t / 2f, right, cy + t / 2f), t / 2f, t / 2f, trackPaint)
            canvas.drawRoundRect(RectF(left, cy - t / 2f, thumbX, cy + t / 2f), t / 2f, t / 2f, fillPaint)
            canvas.drawCircle(thumbX, cy, dpi(11).toFloat(), thumbPaint)
        }

        override fun onTouchEvent(e: MotionEvent): Boolean {
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    parent?.requestDisallowInterceptTouchEvent(true)
                    updateFromX(e.x)
                    return true
                }
                MotionEvent.ACTION_MOVE -> updateFromX(e.x)
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    parent?.requestDisallowInterceptTouchEvent(false)
                }
            }
            return true
        }

        private fun updateFromX(x: Float) {
            val pad = dpi(14).toFloat()
            val left = pad
            val right = (width - pad).coerceAtLeast(left + 1f)
            val v = ((x - left) / (right - left) * 255f)
                .coerceIn(0f, 255f).toInt()
            if (v != progress) {
                progress = v
                onMove(v)
            }
        }
    }
}
