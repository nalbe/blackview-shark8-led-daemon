package com.bastet.ledgui

import android.content.Context
import android.content.Intent
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.CheckBox
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.util.Locale
import kotlin.math.roundToInt

/** Status tab rebuilt on classic Views (Compose cannot render smooth 120Hz
 * on this firmware; plain View framework does). All logic is identical to
 * the old composable StatusScreen: no Compose anywhere in the render path.
 *
 * Polling: collectAll() every 3s (one persistent-su-shell roundtrip),
 * live LED brightness read 10x/sec, tickers auto-start/stop with the view.
 */
class StatusView(context: Context) : LinearLayout(context) {

    private val mP = android.view.ViewGroup.LayoutParams.MATCH_PARENT
    private val wP = android.view.ViewGroup.LayoutParams.WRAP_CONTENT

    private enum class RootState { CHECKING, PENDING, GRANTED, DENIED }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var tickerJob: Job? = null

    // state
    private var rootState = RootState.CHECKING
    private var pid = ""
    private var keepaliveOn = false
    private var keepalive = ""
    private var led: LedStatus = LedStatus()
    private var logText = ""
    private var live = Triple(-1, -1, -1)
    private var loggingOn = true
    private var loggingBusy = false
    private lateinit var loggingListener: android.widget.CompoundButton.OnCheckedChangeListener

    // widgets
    private lateinit var rootTv: TextView
    private lateinit var rootHintTv: TextView
    private lateinit var requestBtn: Button
    private lateinit var pidTv: TextView
    private lateinit var keepaliveCb: CheckBox
    private lateinit var keepaliveTv: TextView
    private lateinit var ledSwatch: View
    private lateinit var ledModeTv: TextView
    private lateinit var ledColorTv: TextView
    private lateinit var ledLightBandTv: TextView
    private lateinit var ledPkgTv: TextView
    private lateinit var ledSinceTv: TextView
    private lateinit var logTv: TextView
    private lateinit var loggingCb: CheckBox

init {
        orientation = VERTICAL
        setBackgroundColor(parse("#FF121212"))
        buildUi()
        render()
        post {
            android.util.Log.d("ledgui", "StatusView: $width x $height children=$childCount")
            val sv = getChildAt(0)
            if (sv is ViewGroup) {
                android.util.Log.d("ledgui", "  root-child h=${sv.height} w=${sv.width} children=${sv.childCount} lp=${sv.layoutParams}")
                for (i in 0 until sv.childCount) {
                    val c = sv.getChildAt(i)
                    android.util.Log.d("ledgui", "    [$i] ${c.javaClass.simpleName} h=${c.height} w=${c.width} vis=${c.visibility}")
                }
            }
            requestLayout()
        }
    }

override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        tickerJob = scope.launch {
            launch { refreshLoop() }
            launch { liveLoop() }
        }
    }

    override fun onDetachedFromWindow() {
        super.onDetachedFromWindow()
        tickerJob?.cancel()
        tickerJob = null
    }

    // ---------------------------------------------------------------- loops

    private suspend fun refreshLoop() {
        while (true) {
            val snap = withContext(Dispatchers.IO) { collectAll() }
            applySnap(snap)
            delay(3000)
        }
    }

private suspend fun liveLoop() {
        while (true) {
            live = withContext(Dispatchers.IO) { readBrightness() }
            updateLedLive()
            delay(250)
        }
    }

    // ---------------------------------------------------------------- render

    private fun applySnap(s: Snapshot) {
        android.util.Log.d("ledgui", "snap rootOk=${s.rootOk} daemon='${s.daemon}' ka='${s.ka}' status.mode='${s.status.mode}' logLen=${s.log.length}")
        rootState = if (s.rootOk) RootState.GRANTED else RootState.DENIED
        pid = s.daemon
        keepalive = s.ka
        keepaliveOn = keepalive.isNotBlank()
        led = s.status
        logText = s.log
        if (!loggingBusy && loggingOn != s.logging) {
            loggingOn = s.logging
            loggingCb.setOnCheckedChangeListener(null)
            loggingCb.isChecked = s.logging
            loggingCb.setOnCheckedChangeListener(loggingListener)
        }
        render()
    }

    private fun render() {
        renderRoot()
        renderDaemon()
        renderLed()
        logTv.text = logText.ifBlank { "(empty)" }
    }

    private fun renderRoot() {
        when (rootState) {
            RootState.CHECKING -> {
                rootTv.text = "checking..."
                rootTv.setTextColor(parse("#FFE0E0E0"))
                rootHintTv.text = ""
            }
            RootState.PENDING -> {
                rootTv.text = "Root request sent. Confirm the KernelSU dialog on screen."
                rootTv.setTextColor(parse("#FFEF6C00"))
                rootHintTv.text = ""
            }
            RootState.GRANTED -> {
                rootTv.text = "root granted (uid=0)"
                rootTv.setTextColor(parse("#FF4CAF50"))
                rootHintTv.text = ""
            }
            RootState.DENIED -> {
                rootTv.text = "no root yet"
                rootTv.setTextColor(parse("#FFF44336"))
                rootHintTv.text =
                    "Open KernelSU Manager, tap Superuser, add LED GUI and set Allow."
            }
        }
requestBtn.isEnabled = rootState != RootState.GRANTED
        requestBtn.alpha = if (requestBtn.isEnabled) 1f else 0.45f
    }

    private fun renderDaemon() {
        pidTv.text = "chgd pid: ${if (pid.isBlank()) "down" else pid}"
        keepaliveCb.isChecked = keepaliveOn
        keepaliveTv.text = "keepalive: ${if (keepalive.isBlank()) "off" else "pid $keepalive"}"
    }

    private fun renderLed() {
        if (led.mode.isEmpty()) {
            ledModeTv.text = "no /data/local/tmp/led_status (old daemon binary?)"
            ledColorTv.visibility = GONE
            ledLightBandTv.visibility = GONE
            ledPkgTv.visibility = GONE
            ledSinceTv.visibility = GONE
            return
        }
        ledColorTv.visibility = VISIBLE
        ledLightBandTv.visibility = VISIBLE
        ledPkgTv.visibility = VISIBLE
        ledSinceTv.visibility = VISIBLE
        ledModeTv.text = "mode: ${led.mode}  ${if (led.isArmed) "(armed)" else "(off)"}"
        ledLightBandTv.text = "light: ${led.lightTypeName}  band: ${led.band.ifBlank { "-" }}"
        ledPkgTv.text = "pkg: ${led.pkg.ifBlank { "-" }}"
        ledSinceTv.text = "since: ${led.prettyTime}"
        updateLedLive()
    }

    private fun updateLedLive() {
        if (led.mode.isEmpty()) return
        val sw = ledSwatch.background as? GradientDrawable ?: return
        val liveOn = live.first >= 0
        val shown = when {
            liveOn -> live
            led.isArmed -> led.color
            else -> Triple(-1, -1, -1)
        }
sw.setColor(
            if (shown.first >= 0) Color.rgb(shown.first, shown.second, shown.third)
            else Color.DKGRAY
        )
        val liveHex = String.format(Locale.US, "#%02X%02X%02X",
            shown.first.coerceIn(0, 255), shown.second.coerceIn(0, 255), shown.third.coerceIn(0, 255))
        val newText = "color: $liveHex  (${shown.first},${shown.second},${shown.third})" +
            if (liveOn && shown.first >= 0) "  [live]" else ""
        if (ledColorTv.text.toString() != newText) ledColorTv.text = newText
    }

    // ---------------------------------------------------------------- actions

    private fun refreshNow() {
        scope.launch {
            val snap = withContext(Dispatchers.IO) { collectAll() }
            applySnap(snap)
        }
    }

    private fun toggleKeepalive(on: Boolean) {
        scope.launch {
            val snap = withContext(Dispatchers.IO) {
                if (on) keepaliveStart() else keepaliveKill()
                collectAll()
            }
            applySnap(snap)
        }
    }

    private fun sendHook(sig: String) {
        scope.launch { withContext(Dispatchers.IO) { hook(sig) } }
    }

    private fun clearLog() {
        scope.launch {
            withContext(Dispatchers.IO) {
                hook("CONT")
                logText = logTail(25)
            }
            logTv.text = logText.ifBlank { "(empty)" }
        }
    }

    /** Flip the daemon's on-disk logging live. Touches ONLY the [led]
     *  logging= line in led.conf (no full-file rewrite, so unsaved edits
     *  in the Config tab survive) and pokes the daemon with SIGALRM to
     *  hot-reload it. */
    private fun toggleLogging(on: Boolean) {
        if (loggingBusy) return
        loggingBusy = true
        loggingCb.isChecked = on
        loggingOn = on
        scope.launch {
            withContext(Dispatchers.IO) {
                val valStr = if (on) "1" else "0"
                Su.run(
                    "CFG=/data/adb/modules/led_hal_root/led.conf;" +
                        " if grep -q '^logging=.\$' \$CFG; then " +
                        "  sed -i 's/^logging=./logging=$valStr/' \$CFG; " +
                        "else " +
                        "  (printf '\\n[led]\\nlogging=$valStr\\n' >> \$CFG); " +
                        "fi; kill -ALRM \$(pidof chgd) 2>/dev/null"
                )
            }
            loggingBusy = false
        }
    }


    private fun requestRoot() {
        scope.launch {
            rootState = withContext(Dispatchers.IO) { probeRoot() }
            render()
        }
    }

    // ---------------------------------------------------------------- UI build

private fun buildUi() {
        val scroll = ScrollView(context)
        scroll.isFillViewport = true
        val content = LinearLayout(context)
        content.orientation = VERTICAL
        content.setPadding(0, dpi(4), 0, dpi(8))
        scroll.addView(content, LayoutParams(mP, wP))
        addView(scroll, LayoutParams(mP, mP))

        // --- Root card
        content.addView(card {
            addView(sectionTitle("Root"))
            addView(spacer(6))
            rootTv = text("checking...")
            addView(rootTv)
            rootHintTv = text("", 13f, parse("#FF909090"))
            addView(rootHintTv)
            addView(spacer(4))
            requestBtn = filledBtn("Request root") { requestRoot() }
            addView(row(requestBtn, outlinedBtn("Open KernelSU Manager") {
                openRootManager(context)
            }))
        })

        // --- Daemon card
        content.addView(card {
            addView(sectionTitle("Daemon"))
            addView(spacer(6))
            pidTv = text("")
            addView(pidTv)
            addView(spacer(6))
            keepaliveCb = CheckBox(context)
            keepaliveCb.setOnCheckedChangeListener { _, on -> toggleKeepalive(on) }
            keepaliveTv = text("")
            addView(row(keepaliveCb, keepaliveTv))
            addView(spacer(8))
            addView(row(
                filledBtn("Refresh") { refreshNow() },
                outlinedBtn("Kill chgd") {
                    scope.launch {
                        val snap = withContext(Dispatchers.IO) {
                            Su.run("kill -9 \$(pidof chgd) 2>/dev/null")
                            collectAll()
                        }
                        applySnap(snap)
                    }
                }
            ))
            addView(spacer(4))
            addView(text(
                "Keepalive restarts chgd after a kill. Uncheck to stop it.",
                13f, parse("#FF909090")
            ))
        })

        // --- LED + test hooks card
        content.addView(card {
            addView(sectionTitle("LED state"))
            addView(spacer(6))
            val sw = GradientDrawable()
            sw.shape = GradientDrawable.RECTANGLE
            sw.cornerRadius = dpf(6)
            sw.setColor(Color.DKGRAY)
            ledSwatch = View(context)
            ledSwatch.background = sw
            ledSwatch.layoutParams = LayoutParams(dpi(30), dpi(30))
            val liveCol = LinearLayout(context)
            liveCol.orientation = VERTICAL
            ledModeTv = text("")
            ledColorTv = text("", 13f, parse("#FFB0BEC5"), mono = true)
            ledLightBandTv = text("")
            ledPkgTv = text("")
            ledSinceTv = text("")
            liveCol.addView(ledModeTv)
            liveCol.addView(ledColorTv)
            liveCol.addView(ledLightBandTv)
            liveCol.addView(ledPkgTv)
            liveCol.addView(ledSinceTv)
            val ledRow = row(ledSwatch, liveCol)
            for (i in 0 until ledRow.childCount) {
                val c = ledRow.getChildAt(i)
                if (i > 0) {
                    val lp = c.layoutParams as LayoutParams
                    lp.marginStart = dpi(10)
                    c.layoutParams = lp
                }
            }
            addView(ledRow)
            addView(spacer(8))
            addView(sectionTitle("Test hooks"))
            addView(spacer(6))
            addView(row(
                filledBtn("Fake Telegram") { sendHook("USR1") },
                outlinedBtn("Disarm") { sendHook("USR2") }
            ))
            addView(spacer(4))
            addView(row(
                filledBtn("Fake Dialer") { sendHook("HUP") },
                outlinedBtn("Rainbow") { sendHook("WINCH") }
            ))
            addView(spacer(4))
            addView(row(filledBtn("Charge Test") { sendHook("QUIT") }))
        })

        // --- log card
        content.addView(card {
            val titleLp = LayoutParams(0, wP, 1f)
            val title = sectionTitle("ledd.log (tail)")
            title.layoutParams = titleLp
            addView(row(title, outlinedBtn("Clear Log") { clearLog() }))
            addView(spacer(6))
            loggingCb = CheckBox(context)
            loggingCb.text = "Logging (daemon writes ledd.log)"
            loggingCb.setTextColor(parse("#FFB0BEC5"))
            loggingCb.textSize = 13f
            loggingCb.isChecked = loggingOn
            loggingListener = android.widget.CompoundButton.OnCheckedChangeListener { _, on -> toggleLogging(on) }
            loggingCb.setOnCheckedChangeListener(loggingListener)
            addView(loggingCb)
            addView(spacer(6))
            logTv = text("", 12f, parse("#FFB0BEC5"), mono = true)
            addView(logTv)
        })
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

    // ---------------------------------------------------------------- shell glue

    private fun daemonPid(): String = Su.run("pidof chgd 2>/dev/null").out

    private fun keepaliveKill(): String =
        Su.run("kill -9 \$(cat /data/local/tmp/led_keepalive.pid 2>/dev/null) 2>/dev/null; " +
            "rm -f /data/local/tmp/led_keepalive.pid").out

    private fun keepaliveStart(): String =
        Su.run("setsid sh /data/adb/modules/led_hal_root/keepalive.sh </dev/null >/dev/null 2>&1 &").out

    /** One snapshot of everything the Status screen shows in ONE su call. */
    private data class Snapshot(
        val rootOk: Boolean,
        val daemon: String,
        val ka: String,
        val status: LedStatus,
        val log: String,
        val logging: Boolean
    )

    private fun collectAll(): Snapshot {
        val out = Su.run(
            "echo 'U='\$(id -u);" +
                " echo 'D='\$(pidof chgd);" +
                " KP=\$(cat /data/local/tmp/led_keepalive.pid 2>/dev/null);" +
                " if [ -n \"\$KP\" ] && kill -0 \$KP 2>/dev/null; then echo 'K='\$KP; " +
                "else echo 'K='; fi;" +
                " echo 'S<<'; cat /data/local/tmp/led_status 2>/dev/null; echo 'S>>';" +
                " echo 'L<<'; tail -n 25 /data/local/tmp/ledd.log 2>/dev/null; echo 'L>>';" +
                " echo 'G='\$(grep -o '^logging=[01]' /data/adb/modules/led_hal_root/led.conf 2>/dev/null | head -1)"
        ).out

        var rootOk = false
        var daemon = ""
        var ka = ""
        var status = LedStatus()
        var log = ""
        var logging = true
        var section = ""
        val sb = StringBuilder()
        for (line in out.lineSequence()) {
            when {
                line == "U=0" -> rootOk = true
                line.startsWith("D=") -> daemon = line.substring(2)
                line.startsWith("K=") -> ka = line.substring(2)
                line.startsWith("G=logging=0") -> logging = false
                line.startsWith("G=logging=1") -> logging = true
                line == "S<<" -> { section = "S"; sb.clear() }
                line == "S>>" -> { status = LedStatusReader.parseStatus(sb.toString()); section = "" }
                line == "L<<" -> { section = "L"; sb.clear() }
                line == "L>>" -> { log = sb.toString(); section = "" }
                section == "S" -> sb.appendLine(line)
                section == "L" -> sb.appendLine(line)
            }
        }
        return Snapshot(rootOk, daemon, ka, status, log, logging)
    }

/** Live LED color through the persistent root shell (SELinux blocks
     * untrusted reads of /sys/class/leds). (-1,-1,-1) = n/a.
     * Uses the dedicated live shell so sysfs stalls during animations
     * (rainbow/charge) never block the status polls on the main shell. */
    private fun readBrightness(): Triple<Int, Int, Int> {
        val out = try {
            SuShell.live.exec(
                "cat /sys/class/leds/red/brightness /sys/class/leds/green/brightness " +
                    "/sys/class/leds/blue/brightness 2>/dev/null",
                700
            )
        } catch (_: IllegalStateException) {
            ""
        }.lines()
        if (out.size < 3) return Triple(-1, -1, -1)
        val r = out[0].trim().toIntOrNull()
        val g = out[1].trim().toIntOrNull()
        val b = out[2].trim().toIntOrNull()
        return if (r != null && g != null && b != null) {
            Triple(r.coerceIn(0, 255), g.coerceIn(0, 255), b.coerceIn(0, 255))
        } else Triple(-1, -1, -1)
    }

    private fun logTail(n: Int): String =
        Su.run("tail -n $n /data/local/tmp/ledd.log 2>/dev/null").out

    private fun hook(sig: String) {
        Su.run("kill -$sig \$(pidof chgd) 2>/dev/null")
    }

    private fun probeRoot(): RootState {
        val r = Su.run("id", timeoutSec = 8)
        return when {
            r.pending -> RootState.PENDING
            r.out.contains("uid=0") -> RootState.GRANTED
            else -> RootState.DENIED
        }
    }

    /** Find and launch the KernelSU manager (any installed superuser app). */
    private fun openRootManager(ctx: Context) {
        val pm = ctx.packageManager
        val launcher = Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
        val candidates = listOf("kernelsu", "ksu", "magisk", "superuser")
        var target: String? = null
        for (qi in pm.queryIntentActivities(launcher, 0)) {
            val pkg = qi.activityInfo.packageName
            if (candidates.any { pkg.contains(it, ignoreCase = true) }) { target = pkg; break }
        }
        if (target != null) {
            pm.getLaunchIntentForPackage(target)?.let {
                it.flags = Intent.FLAG_ACTIVITY_NEW_TASK
                ctx.startActivity(it)
            }
        }
    }
}

