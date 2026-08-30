package com.bastet.ledgui

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.LayerDrawable
import android.text.InputType
import android.util.TypedValue
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.TextView
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlin.math.roundToInt

/** Config tab rebuilt on classic Views (same data model + save/load logic
 * as the old composable ConfigScreen, zero Compose in the render path).
 * The footer with Save/Reload is pinned below the scrollable content. */
class ConfigView(context: Context) : LinearLayout(context) {

    private val mP = android.view.ViewGroup.LayoutParams.MATCH_PARENT
    private val wP = android.view.ViewGroup.LayoutParams.WRAP_CONTENT

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

private lateinit var suppressEt: EditText
    private lateinit var rulesEt: EditText
    private val charge = mutableListOf<NumField>()
    private lateinit var lowerType: TypePicker
    private lateinit var middleType: TypePicker
    private lateinit var upperType: TypePicker
    private lateinit var notifyType: TypePicker
    private lateinit var lowerColor: RgbPicker
    private lateinit var middleColor: RgbPicker
    private lateinit var upperColor: RgbPicker
    private lateinit var notifyColor: RgbPicker
    private lateinit var lowerSoft: CheckBox
    private lateinit var middleSoft: CheckBox
    private lateinit var upperSoft: CheckBox
    private lateinit var notifySoft: CheckBox
    private lateinit var softCycle: NumField
    private val notifyFields = mutableListOf<NumField>()
    private lateinit var messageTv: TextView

init {
        orientation = VERTICAL
        setBackgroundColor(parse("#FF121212"))
        buildUi()
        post { reload() }
        post {
            android.util.Log.d("ledgui", "ConfigView: $width x $height children=$childCount")
            val sv = getChildAt(0)
            if (sv is ScrollView) {
                android.util.Log.d("ledgui", "  scroll h=${sv.height} children=${sv.childCount}")
                val cont = sv.getChildAt(0)
                if (cont is ViewGroup) {
                    android.util.Log.d("ledgui", "  content h=${cont.height} children=${cont.childCount}")
                }
            }
        }
    }

    // ---------------------------------------------------------------- actions

private fun reload() {
        scope.launch {
            var attempts = 0
            while (true) {
                val root = withContext(Dispatchers.IO) { Su.run("id") }
                if (root.ok && root.out.contains("uid=0")) break
                attempts++
                if (attempts > 12) {
                    messageTv.text = "load skipped: no root yet. Grant root / press Request root on Status, then Reload."
                    return@launch
                }
                delay(1000)
            }
            val c = withContext(Dispatchers.IO) { LedConf.load() }
            applyConf(c)
            messageTv.text = "loaded: ${c.suppress.size} suppressed, ${c.rules.size} rules"
            android.util.Log.d("ledgui", "reload done: ${c.suppress.size} suppressed, ${c.rules.size} rules")
        }
    }

    private fun save() {
        scope.launch {
            // collect all UI state on the main thread, only su calls go to IO
            val c = LedConf()
            var bad = ""
            for (line in rulesEt.text.toString().lines()) {
                if (line.isBlank()) continue
                val i = line.indexOf('=')
                if (i <= 0) { bad += "bad rule: $line\n"; continue }
                val pkg = line.substring(0, i).trim()
                val parts = line.substring(i + 1).split(',')
                if (parts.size != 3) { bad += "bad rule: $line\n"; continue }
                val r = parts[0].trim().toIntOrNull() ?: -1
                val g = parts[1].trim().toIntOrNull() ?: -1
                val b = parts[2].trim().toIntOrNull() ?: -1
                if (r !in 0..255 || g !in 0..255 || b !in 0..255) {
                    bad += "bad rule: $line\n"
                    continue
                }
                c.rules.add(Rule(pkg, r, g, b))
            }
            c.suppress.addAll(
                suppressEt.text.toString().lines().filter { it.isNotBlank() }
            )
            c.firstThreshold = charge[0].getInt(90)
            c.secondThreshold = charge[1].getInt(95)
            c.chargeRise = charge[2].getInt(700)
            c.chargeHold = charge[3].getInt(100)
            c.chargeFall = charge[4].getInt(700)
            c.chargeOfft = charge[5].getInt(900)
            c.softCycleMs = softCycle.getInt(25500)
            c.lowerType = lowerType.getType()
            c.middleType = middleType.getType()
            c.upperType = upperType.getType()
            c.lowerColor = lowerColor.getColor()
            c.middleColor = middleColor.getColor()
            c.upperColor = upperColor.getColor()
            c.lowerSoftBreath = lowerSoft.isChecked
            c.middleSoftBreath = middleSoft.isChecked
            c.upperSoftBreath = upperSoft.isChecked
            c.notifyType = notifyType.getType()
            c.notifyRise = notifyFields[0].getInt(500)
            c.notifyHold = notifyFields[1].getInt(100)
            c.notifyFall = notifyFields[2].getInt(500)
            c.notifyOfft = notifyFields[3].getInt(1200)
            c.notifMaxSec = notifyFields[4].getLong(0L)
            c.notifyScreenDelayMs = notifyFields[5].getLong(60000L)
            c.notifyColor = notifyColor.getColor()
            c.notifySoftBreath = notifySoft.isChecked

            val msg = withContext(Dispatchers.IO) {
                val res = c.save()
                if (res.ok) {
                    Su.run("kill -ALRM \$(pidof chgd) 2>/dev/null")
                    "Saved OK. Daemon reloaded (SIGALRM) - applied now."
                } else {
                    "Save FAILED (exit ${res.code}): ${res.out.ifBlank { "no su / no write?" }}"
                }
            }
            messageTv.text = if (bad.isNotEmpty()) "$msg\n${bad.trimEnd()}" else msg
        }
    }

    private fun syncSoft(type: Int, picker: RgbPicker, soft: CheckBox) {
        soft.visibility = if (type == 1) VISIBLE else GONE   /* custom breath only for breathing */
        /* full RGB range with custom breath, 0/255 snap on plain hardware breath */
        picker.setDiscrete(type == 1 && !soft.isChecked)
    }

    private fun applyConf(c: LedConf) {
        suppressEt.setText(c.suppress.joinToString("\n"))
        rulesEt.setText(c.rules.joinToString("\n") { "${it.pkg}=${it.r},${it.g},${it.b}" })
        charge[0].setText(c.firstThreshold.toString())
        charge[1].setText(c.secondThreshold.toString())
        charge[2].setText(c.chargeRise.toString())
        charge[3].setText(c.chargeHold.toString())
        charge[4].setText(c.chargeFall.toString())
        charge[5].setText(c.chargeOfft.toString())
        softCycle.setText(c.softCycleMs.toString())
lowerType.setType(c.lowerType)
        middleType.setType(c.middleType)
        upperType.setType(c.upperType)
        lowerColor.setColor(c.lowerColor)
        middleColor.setColor(c.middleColor)
        upperColor.setColor(c.upperColor)
        lowerSoft.isChecked = c.lowerSoftBreath
        middleSoft.isChecked = c.middleSoftBreath
        upperSoft.isChecked = c.upperSoftBreath
        notifyType.setType(c.notifyType)
        notifyColor.setColor(c.notifyColor)
        notifySoft.isChecked = c.notifySoftBreath
        syncSoft(c.lowerType, lowerColor, lowerSoft)
        syncSoft(c.middleType, middleColor, middleSoft)
        syncSoft(c.upperType, upperColor, upperSoft)
        syncSoft(c.notifyType, notifyColor, notifySoft)
        notifyFields[0].setText(c.notifyRise.toString())
        notifyFields[1].setText(c.notifyHold.toString())
        notifyFields[2].setText(c.notifyFall.toString())
        notifyFields[3].setText(c.notifyOfft.toString())
        notifyFields[4].setText(c.notifMaxSec.toString())
        notifyFields[5].setText(c.notifyScreenDelayMs.toString())
    }

// ---------------------------------------------------------------- UI build

    private fun buildUi() {
        val scroll = ScrollView(context)
        scroll.isFillViewport = true
        val content = LinearLayout(context)
        content.orientation = VERTICAL
        content.setPadding(0, dpi(4), 0, dpi(8))
        scroll.addView(content, LayoutParams(mP, wP))
        addView(scroll, LayoutParams(mP, 0, 1f))

        // --- suppress
        content.addView(card {
            addView(sectionTitle("Suppressed packages (one per line; never light the LED)"))
            addView(spacer(6))
            suppressEt = multiLine("")
            addView(suppressEt)
        })

        // --- rules
        content.addView(card {
            addView(sectionTitle("Per-app rules (pkg=r,g,b)"))
            addView(spacer(6))
            rulesEt = multiLine("com.whatsapp=0,255,0")
            addView(rulesEt)
        })

        // --- charge thresholds
        content.addView(card {
            addView(sectionTitle("Charge thresholds (percent, order-free)"))
            addView(spacer(4))
            addNum("first", "90", charge)
            addNum("second", "95", charge)
            addView(spacer(4))
            addView(sectionTitle("Charge timing (ms)"))
            addView(spacer(4))
            addNum("rise", "700", charge)
            addNum("hold", "100", charge)
addNum("fall", "700", charge)
            addNum("off", "900", charge)
            addView(spacer(4))
            addView(sectionTitle("Custom breathing timing (ms)"))
            addView(spacer(4))
            softCycle = addNumL("breath phase (ms)", "25500")
        })

        // --- ranges
        content.addView(card {
addView(sectionTitle("Lower range (below first threshold)"))
            addView(spacer(4))
            lowerType = TypePicker("light type") { t -> syncSoft(t, lowerColor, lowerSoft) }
            addView(lowerType)
            lowerSoft = softChk("custom breath (full RGB range)")
            lowerSoft.setOnCheckedChangeListener { _, _ -> syncSoft(lowerType.getType(), lowerColor, lowerSoft) }
            addView(lowerSoft)
            addView(spacer(4))
            lowerColor = RgbPicker("color")
            addView(lowerColor)
        })
        content.addView(card {
addView(sectionTitle("Middle range (between thresholds)"))
            addView(spacer(4))
            middleType = TypePicker("light type") { t -> syncSoft(t, middleColor, middleSoft) }
            addView(middleType)
            middleSoft = softChk("custom breath (full RGB range)")
            middleSoft.setOnCheckedChangeListener { _, _ -> syncSoft(middleType.getType(), middleColor, middleSoft) }
            addView(middleSoft)
            addView(spacer(4))
            middleColor = RgbPicker("color")
            addView(middleColor)
        })
        content.addView(card {
addView(sectionTitle("Upper range (>= second threshold / Full)"))
            addView(spacer(4))
            upperType = TypePicker("light type") { t -> syncSoft(t, upperColor, upperSoft) }
            addView(upperType)
            upperSoft = softChk("custom breath (full RGB range)")
            upperSoft.setOnCheckedChangeListener { _, _ -> syncSoft(upperType.getType(), upperColor, upperSoft) }
            addView(upperSoft)
            addView(spacer(4))
            upperColor = RgbPicker("color")
            addView(upperColor)
        })

        // --- notify
        content.addView(card {
            addView(sectionTitle(
                "Notify (type/breath/duration apply to ALL apps; default color only for apps without a rule)"
            ))
addView(spacer(4))
            notifyType = TypePicker("light type") { t -> syncSoft(t, notifyColor, notifySoft) }
            addView(notifyType)
            notifySoft = softChk("custom breath (full RGB range)")
            notifySoft.setOnCheckedChangeListener { _, _ -> syncSoft(notifyType.getType(), notifyColor, notifySoft) }
            addView(notifySoft)
            addView(spacer(4))
            notifyColor = RgbPicker("default color")
            addView(notifyColor)
            addView(spacer(4))
            addView(sectionTitle("Notify timing (ms)"))
            addView(spacer(4))
            addNum("rise", "500", notifyFields)
            addNum("hold", "100", notifyFields)
            addNum("fall", "500", notifyFields)
            addNum("off", "1200", notifyFields)
            addNum("notif_max_sec (0 = unlimited)", "0", notifyFields)
            addNum("pending window (ms, screen-off flash)", "60000", notifyFields)
        })

        content.addView(card {
            messageTv = text("", 12f, parse("#FF90CAF9"), mono = true)
            addView(messageTv)
        })

        // --- pinned footer
        val foot = LinearLayout(context)
        foot.orientation = HORIZONTAL
        foot.setPadding(dpi(12), dpi(8), dpi(12), dpi(8))
        foot.addView(row(filledBtn("Save to device") { save() },
            outlinedBtn("Reload") { reload() }))
        addView(foot, LayoutParams(mP, wP))
    }

private fun LinearLayout.addNum(label: String, init: String, into: MutableList<NumField>) {
        val f = NumField(label, init)
        into.add(f)
        addView(f)
    }

    private fun LinearLayout.addNumL(label: String, init: String): NumField {
        val f = NumField(label, init)
        addView(f)
        return f
    }

    // ---------------------------------------------------------------- widgets

    private inner class NumField(label: String, init: String) : LinearLayout(context) {
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

private inner class TypePicker(
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

private inner class RgbPicker(label: String) : LinearLayout(context) {
        private var color = Triple(255, 0, 0)
        private var discrete = false          // breathing: 0 or 255 only
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

        /** Map a raw 0-255 channel onto the 0/255 grid the breathing
         *  effect of the aw2033 chip can actually display. */
        private fun snap(v: Int): Int = if (v < 128) 0 else 255

        private fun colorOf(i: Int): Int = when (i) {
            0 -> color.first; 1 -> color.second; else -> color.third
        }

        /** Breathing light type: channels can only be 0 or 255. */
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

    /** Minimal cheap slider: a single View drawn straight in onDraw (a
     *  rounded track + fill + thumb). Far lighter than a SeekBar's stacked
     *  drawables, so the Config page stays smooth at 120Hz. */
    private inner class SliderView(
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
        private var downProgress = -1

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
            // track bg
            canvas.drawRoundRect(RectF(left, cy - t / 2f, right, cy + t / 2f), t / 2f, t / 2f, trackPaint)
            // fill up to thumb
            canvas.drawRoundRect(RectF(left, cy - t / 2f, thumbX, cy + t / 2f), t / 2f, t / 2f, fillPaint)
            // thumb
            canvas.drawCircle(thumbX, cy, dpi(11).toFloat(), thumbPaint)
        }

        override fun onTouchEvent(e: MotionEvent): Boolean {
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    parent?.requestDisallowInterceptTouchEvent(true)
                    downProgress = progress
                    updateFromX(e.x)
                    return true
                }
                MotionEvent.ACTION_MOVE -> updateFromX(e.x)
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    downProgress = -1
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

// ---------------------------------------------------------------- helpers

    private fun dpf(v: Float) = v * resources.displayMetrics.density

    private fun dpf(v: Int) = v * resources.displayMetrics.density

    private fun dpi(v: Float) = (v * resources.displayMetrics.density).roundToInt()

    private fun dpi(v: Int) = (v * resources.displayMetrics.density).roundToInt()

    private fun parse(hex: String) = Color.parseColor(hex)

    private fun card(builder: LinearLayout.() -> Unit): LinearLayout {
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

    private fun sectionTitle(s: String): TextView {
        val t = text(s, 15f, parse("#FF90CAF9"), bold = true)
        t.setPadding(0, dpi(2), 0, dpi(2))
        return t
    }

    private fun text(s: String, size: Float = 14f, color: Int = parse("#FFE0E0E0"),
                     bold: Boolean = false, mono: Boolean = false): TextView {
        val t = TextView(context)
        t.text = s
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, size)
        t.setTextColor(color)
        if (bold) t.setTypeface(null, Typeface.BOLD)
        if (mono) t.typeface = Typeface.MONOSPACE
        return t
    }

    private fun multiLine(hint: String): EditText {
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

    private fun softChk(label: String): CheckBox {
        val c = CheckBox(context)
        c.text = label
        c.setTextColor(parse("#FFB0BEC5"))
        c.textSize = 13f
        c.setPadding(0, dpi(2), 0, dpi(2))
        return c
    }

    private fun spacer(h: Int): View {
        val v = View(context)
        v.layoutParams = LayoutParams(1, dpi(h))
        return v
    }

    private fun row(vararg views: View, gravity: Int = Gravity.CENTER_VERTICAL): LinearLayout {
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

private fun filledBtn(label: String, onClick: () -> Unit): Button {
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

    private fun outlinedBtn(label: String, onClick: () -> Unit): Button {
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
}

