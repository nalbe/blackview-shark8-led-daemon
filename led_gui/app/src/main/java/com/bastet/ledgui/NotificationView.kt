package com.bastet.ledgui

import android.content.Context

/** Notification tab: shared notify light behavior (type / custom breath /
 *  default color / timing), the pending screen-off window, the apps that
 *  never light the LED, and per-app colour rules. */
class NotificationView(context: Context) : ConfPage(context) {

    private lateinit var suppressEt: android.widget.EditText
    private lateinit var rulesEt: android.widget.EditText
    private lateinit var notifyType: TypePicker
    private lateinit var notifySoft: android.widget.CheckBox
    private lateinit var notifyColor: RgbPicker
    private lateinit var nRise: NumField
    private lateinit var nHold: NumField
    private lateinit var nFall: NumField
    private lateinit var nOfft: NumField
    private lateinit var nMaxSec: NumField
    private lateinit var nPending: NumField
    private lateinit var nSoftCycle: NumField

    override fun buildBody() {
        body.addView(card {
            msgTv = text("", 12f, parse("#FF90CAF9"), mono = true)
            addView(msgTv!!)
        })
        body.addView(card {
            addView(sectionTitle(
                "Notify (type/breath/duration apply to ALL apps; default color only for apps without a rule)"
            ))
            addView(spacer(4))
            notifyType = TypePicker("light type") { syncNotify() }
            addView(notifyType)
            notifySoft = softChk("custom breath (full RGB range)")
            notifySoft.setOnCheckedChangeListener { _, _ -> syncNotify() }
            addView(notifySoft)
            addView(spacer(4))
            notifyColor = RgbPicker("default color")
            addView(notifyColor)
            addView(spacer(4))
            addView(sectionTitle("Notify timing (ms)"))
            addView(spacer(4))
            nRise = numRow("rise", "500")
            nHold = numRow("hold", "100")
            nFall = numRow("fall", "500")
            nOfft = numRow("off", "1200")
            nMaxSec = numRow("notif_max_sec (0 = unlimited)", "0")
            nPending = numRow("pending window (ms, screen-off flash)", "60000")
            nSoftCycle = numRow("soft breath cycle (ms)", "25500")
        })

        body.addView(card {
            addView(sectionTitle("Suppressed packages (one per line; never light the LED)"))
            addView(spacer(6))
            suppressEt = multiLine("")
            addView(suppressEt)
        })

        body.addView(card {
            addView(sectionTitle("Per-app rules (pkg=r,g,b)"))
            addView(spacer(6))
            rulesEt = multiLine("com.whatsapp=0,255,0")
            addView(rulesEt)
        })
    }

    private fun syncNotify() = syncSoft(notifyType.getType(), notifyColor, notifySoft)

    override fun loadSummary(c: LedConf): String =
        "loaded: ${c.suppress.size} suppressed, ${c.rules.size} rules"

    override fun applyTo(c: LedConf) {
        notifyType.setType(c.notifyType)
        notifyColor.setColor(c.notifyColor)
        notifySoft.isChecked = c.notifySoftBreath
        syncNotify()
        nRise.setText(c.notifyRise.toString())
        nHold.setText(c.notifyHold.toString())
        nFall.setText(c.notifyFall.toString())
        nOfft.setText(c.notifyOfft.toString())
        nMaxSec.setText(c.notifMaxSec.toString())
        nPending.setText(c.notifyScreenDelayMs.toString())
        nSoftCycle.setText(c.notifySoftCycleMs.toString())
        suppressEt.setText(c.suppress.joinToString("\n"))
        rulesEt.setText(c.rules.joinToString("\n") { "${it.pkg}=${it.r},${it.g},${it.b}" })
    }

    override fun collectFrom(c: LedConf) {
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
        c.suppress.addAll(suppressEt.text.toString().lines().filter { it.isNotBlank() })
        c.notifyType = notifyType.getType()
        c.notifyRise = nRise.getInt(500)
        c.notifyHold = nHold.getInt(100)
        c.notifyFall = nFall.getInt(500)
        c.notifyOfft = nOfft.getInt(1200)
        c.notifMaxSec = nMaxSec.getLong(0L)
        c.notifyScreenDelayMs = nPending.getLong(60000L)
        c.notifySoftCycleMs = nSoftCycle.getLong(25500L)
        c.notifyColor = notifyColor.getColor()
        c.notifySoftBreath = notifySoft.isChecked
        if (bad.isNotEmpty()) msgTv?.text = bad.trimEnd()
    }
}
