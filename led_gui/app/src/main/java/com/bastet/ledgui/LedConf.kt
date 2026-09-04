package com.bastet.ledgui

/**
 * led.conf model + text parser/renderer.
 *
 * The daemon hot-reloads the file by mtime on the next event, so a save
 * here takes effect without a restart. The file must stay ASCII-only
 * (the daemon parser and the shell toolchain are byte-oriented).
 */
data class Rule(val pkg: String, val r: Int, val g: Int, val b: Int)

data class LedConf(
    val suppress: MutableList<String> = mutableListOf(),
    val rules: MutableList<Rule> = mutableListOf(),
    var firstThreshold: Int = 90,
    var secondThreshold: Int = 95,
    var chargeRise: Int = 700,
    var chargeHold: Int = 100,
    var chargeFall: Int = 700,
    var chargeOfft: Int = 900,
    var softCycleMs: Int = 25500,
    var lowerType: Int = 1,
    var middleType: Int = 1,
    var upperType: Int = 3,
    var lowerColor: Triple<Int, Int, Int> = Triple(255, 0, 0),
    var middleColor: Triple<Int, Int, Int> = Triple(96, 255, 0),
    var upperColor: Triple<Int, Int, Int> = Triple(0, 255, 0),
    var lowerSoftBreath: Boolean = false,
    var middleSoftBreath: Boolean = false,
    var upperSoftBreath: Boolean = false,
    var notifyType: Int = 1,
    var notifyRise: Int = 500,
    var notifyHold: Int = 100,
    var notifyFall: Int = 500,
    var notifyOfft: Int = 1200,
    var notifMaxSec: Long = 0,
    var notifyScreenDelayMs: Long = 60000,
    var notifyColor: Triple<Int, Int, Int> = Triple(255, 150, 150),
    var notifySoftBreath: Boolean = false,
    var notifySoftCycleMs: Long = 25500,
    var ringTestSec: Long = 30,
    var voipMaxSec: Long = 300,
    var voipPackages: String = "",
    var logging: Boolean = true
) {

    companion object {
        const val CONF_PATH = "/data/adb/modules/led_hal_root/led.conf"

        fun load(): LedConf {
            val conf = LedConf()
            val r = Su.run("cat $CONF_PATH 2>/dev/null")
            if (r.ok) conf.parse(r.out)
            return conf
        }

        private fun clampColor(v: Int) = v.coerceIn(0, 255)
    }

    fun parse(text: String) {
        var section = ""
        for (rawLine in text.lineSequence()) {
            val line = rawLine.trim()
            if (line.isEmpty() || line.startsWith("#") || line.startsWith(";")) continue
            if (line.startsWith("[")) {
                val end = line.indexOf(']')
                section = if (end > 1) line.substring(1, end) else ""
                continue
            }
            when (section) {
                "suppress" -> {
                    if (line.isNotBlank() && !suppress.contains(line)) suppress.add(line)
                }
                "rules" -> {
                    val i = line.indexOf('=')
                    if (i <= 0) continue
                    val pkg = line.substring(0, i).trim()
                    val rgb = parseRgb(line.substring(i + 1)) ?: continue
                    rules.removeAll { it.pkg == pkg }
                    rules.add(Rule(pkg, rgb.first, rgb.second, rgb.third))
                }
                "charge" -> {
                    val i = line.indexOf('=')
                    if (i <= 0) continue
                    val k = line.substring(0, i).trim()
                    val v = line.substring(i + 1).trim()
                    when (k) {
                        "first_threshold" -> v.toIntOrNull()?.let { firstThreshold = it }
                        "second_threshold" -> v.toIntOrNull()?.let { secondThreshold = it }
                        "rise" -> v.toIntOrNull()?.let { chargeRise = it }
                        "hold" -> v.toIntOrNull()?.let { chargeHold = it }
                        "fall" -> v.toIntOrNull()?.let { chargeFall = it }
                        "offt" -> v.toIntOrNull()?.let { chargeOfft = it }
                        "soft_cycle_ms" -> v.toIntOrNull()?.let { softCycleMs = it }
                        "lower_range_light_type" -> v.toIntOrNull()?.let { lowerType = it }
                        "middle_range_light_type" -> v.toIntOrNull()?.let { middleType = it }
                        "upper_range_light_type" -> v.toIntOrNull()?.let { upperType = it }
                        "lower_range_color" -> parseRgb(v)?.let { lowerColor = it }
                        "middle_range_color" -> parseRgb(v)?.let { middleColor = it }
                        "upper_range_color" -> parseRgb(v)?.let { upperColor = it }
                        "lower_soft_breath" -> lowerSoftBreath = v.trim() == "1"
                        "middle_soft_breath" -> middleSoftBreath = v.trim() == "1"
                        "upper_soft_breath" -> upperSoftBreath = v.trim() == "1"
                    }
                }
                "notify" -> {
                    val i = line.indexOf('=')
                    if (i <= 0) continue
                    val k = line.substring(0, i).trim()
                    val v = line.substring(i + 1).trim()
                    when (k) {
                        "notify_light_type" -> v.toIntOrNull()?.let { notifyType = it }
                        "rise" -> v.toIntOrNull()?.let { notifyRise = it }
                        "hold" -> v.toIntOrNull()?.let { notifyHold = it }
                        "fall" -> v.toIntOrNull()?.let { notifyFall = it }
                        "offt" -> v.toIntOrNull()?.let { notifyOfft = it }
                        "notif_max_sec" -> v.toLongOrNull()?.let { notifMaxSec = it }
                        "notify_screen_delay_ms" -> v.toLongOrNull()?.let { notifyScreenDelayMs = it }
                        "default_color" -> parseRgb(v)?.let { notifyColor = it }
                        "notify_soft_breath" -> notifySoftBreath = v.trim() == "1"
                        "notify_soft_cycle_ms" -> v.toLongOrNull()?.let { notifySoftCycleMs = it }
                    }
                }
                "ring" -> {
                    val i = line.indexOf('=')
                    if (i <= 0) continue
                    val k = line.substring(0, i).trim()
                    val v = line.substring(i + 1).trim()
                    if (k == "test_sec") v.toLongOrNull()?.let { ringTestSec = it }
                }
                "voip" -> {
                    val i = line.indexOf('=')
                    if (i <= 0) continue
                    val k = line.substring(0, i).trim()
                    val v = line.substring(i + 1).trim()
                    when (k) {
                        "max_sec" -> v.toLongOrNull()?.let { voipMaxSec = it }
                        "packages" -> voipPackages = v
                    }
                }
                "led" -> {
                    val i = line.indexOf('=')
                    if (i <= 0) continue
                    val k = line.substring(0, i).trim()
                    val v = line.substring(i + 1).trim()
                    if (k == "logging") {
                        val flag = v.toIntOrNull()
                        if (flag != null) logging = flag != 0
                    }
                }
            }
        }
        // order-free thresholds: keep first <= second
        if (firstThreshold > secondThreshold) {
            val t = firstThreshold; firstThreshold = secondThreshold; secondThreshold = t
        }
    }

    fun render(): String {
        val sb = StringBuilder()
        sb.append("# led_hal_root runtime config - generated by LED GUI (v1)\n")
        sb.append("# ASCII only. Save applies on the next daemon event (mtime reload).\n")
        sb.append("#\n")
        sb.append("# [suppress] one package per line - never lights the LED\n")
        sb.append("# [rules]    pkg=r,g,b  (0-255 per channel)\n")
        sb.append("# [charge]   thresholds (percent, order-free), light types per range,\n")
        sb.append("#            colors r,g,b, breath timings rise/hold/fall/offt (ms)\n")
        sb.append("# [notify]   SHARED light behavior for every app (type, breath timings,\n")
        sb.append("#            notif_max_sec); default_color r,g,b is used only for apps\n")
        sb.append("#            without a [rules] entry. Per-rule colors: [rules] pkg=r,g,b\n")
        sb.append("#\n")
        sb.append("# Light types: 0=off 1=breathing 2=flashing 3=static\n")
        sb.append("\n[suppress]\n")
        for (p in suppress) sb.append(p).append('\n')
        sb.append("\n[rules]\n")
        for (rule in rules) {
            sb.append(rule.pkg).append('=').append(rule.r).append(',')
                .append(rule.g).append(',').append(rule.b).append('\n')
        }
        sb.append("\n[charge]\n")
        sb.append("first_threshold=").append(firstThreshold).append('\n')
        sb.append("second_threshold=").append(secondThreshold).append('\n')
        sb.append("rise=").append(chargeRise).append('\n')
        sb.append("hold=").append(chargeHold).append('\n')
        sb.append("fall=").append(chargeFall).append('\n')
        sb.append("offt=").append(chargeOfft).append('\n')
        sb.append("soft_cycle_ms=").append(softCycleMs).append('\n')
        sb.append("lower_range_light_type=").append(lowerType).append('\n')
        sb.append("middle_range_light_type=").append(middleType).append('\n')
        sb.append("upper_range_light_type=").append(upperType).append('\n')
        sb.append("lower_range_color=").append(rgb(lowerColor)).append('\n')
        sb.append("middle_range_color=").append(rgb(middleColor)).append('\n')
        sb.append("upper_range_color=").append(rgb(upperColor)).append('\n')
        sb.append("lower_soft_breath=").append(if (lowerSoftBreath) 1 else 0).append('\n')
        sb.append("middle_soft_breath=").append(if (middleSoftBreath) 1 else 0).append('\n')
        sb.append("upper_soft_breath=").append(if (upperSoftBreath) 1 else 0).append('\n')
        sb.append("\n[notify]\n")
        sb.append("notify_light_type=").append(notifyType).append('\n')
        sb.append("rise=").append(notifyRise).append('\n')
        sb.append("hold=").append(notifyHold).append('\n')
        sb.append("fall=").append(notifyFall).append('\n')
        sb.append("offt=").append(notifyOfft).append('\n')
        sb.append("notif_max_sec=").append(notifMaxSec).append('\n')
        sb.append("notify_screen_delay_ms=").append(notifyScreenDelayMs).append('\n')
        sb.append("default_color=").append(rgb(notifyColor)).append('\n')
        sb.append("notify_soft_breath=").append(if (notifySoftBreath) 1 else 0).append('\n')
        sb.append("notify_soft_cycle_ms=").append(notifySoftCycleMs).append('\n')
        sb.append("\n[ring]\n")
        sb.append("test_sec=").append(ringTestSec).append('\n')
        sb.append("\n[voip]\n")
        sb.append("max_sec=").append(voipMaxSec).append('\n')
        if (voipPackages.isNotBlank()) sb.append("packages=").append(voipPackages).append('\n')
        sb.append("\n[led]\n")
        sb.append("logging=").append(if (logging) 1 else 0).append('\n')
        return sb.toString()
    }

    fun save(): Su.Result = Su.writeFile(CONF_PATH, render())

    private fun rgb(c: Triple<Int, Int, Int>): String =
        "${clampColor(c.first)},${clampColor(c.second)},${clampColor(c.third)}"

    private fun parseRgb(v: String): Triple<Int, Int, Int>? {
        val parts = v.split(',')
        if (parts.size != 3) return null
        val r = parts[0].trim().toIntOrNull() ?: return null
        val g = parts[1].trim().toIntOrNull() ?: return null
        val b = parts[2].trim().toIntOrNull() ?: return null
        if (r !in 0..255 || g !in 0..255 || b !in 0..255) return null
        return Triple(r, g, b)
    }
}