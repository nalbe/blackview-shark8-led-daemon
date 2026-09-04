package com.bastet.ledgui

import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Live state from /data/local/tmp/led_status written by the daemon.
 * Format: ts / mode (charge|notify|ring|voip) / band (lower|middle|upper
 * |none) / pkg / color=r,g,b / type (0=off 1=breathing 2=flashing 3=static).
 */
data class LedStatus(
    val ts: Long = 0,
    val mode: String = "",
    val band: String = "",
    val pkg: String = "",
    val color: Triple<Int, Int, Int> = Triple(0, 0, 0),
    val type: Int = 0
) {
    val colorHex: String
        get() = String.format(Locale.US, "#%02X%02X%02X",
            color.first, color.second, color.third)

    val prettyTime: String
        get() = if (ts > 0) {
            SimpleDateFormat("HH:mm:ss", Locale.US).format(Date(ts * 1000))
        } else "-"

    val lightTypeName: String
        get() = when (type) {
            0 -> "off"
            1 -> "breathing"
            2 -> "flashing"
            3 -> "static"
            else -> "type=$type"
        }

    /** True when some owner is actively driving the LED. */
    val isArmed: Boolean
        get() = mode.isNotEmpty() && mode != "!" && (pkg.isNotEmpty() || band.isNotEmpty())
}

object LedStatusReader {

    private const val STATUS_PATH = "/data/local/tmp/led_status"

    /** Parse the daemon's led_status key=value dump (no shell roundtrip). */
    internal fun parseStatus(text: String): LedStatus {
        var ts = 0L
        var mode = ""
        var band = ""
        var pkg = ""
        var color = Triple(0, 0, 0)
        var type = 0

        for (line in text.lineSequence()) {
            val i = line.indexOf('=')
            if (i <= 0) continue
            val k = line.substring(0, i)
            val v = line.substring(i + 1)
            when (k) {
                "ts" -> ts = v.toLongOrNull() ?: 0L
                "mode" -> mode = v
                "band" -> band = v
                "pkg" -> pkg = v
                "color" -> {
                    val parts = v.split(',')
                    if (parts.size == 3) {
                        color = Triple(
                            parts[0].trim().toIntOrNull() ?: 0,
                            parts[1].trim().toIntOrNull() ?: 0,
                            parts[2].trim().toIntOrNull() ?: 0
                        )
                    }
                }
                "type" -> type = v.trim().toIntOrNull() ?: 0
            }
        }
        return LedStatus(ts, mode, band, pkg, color, type)
    }
}