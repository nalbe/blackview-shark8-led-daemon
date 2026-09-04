package com.bastet.ledgui

import android.content.Context

/** Call tab: previously had no GUI at all. Three knobs straight off the
 *  mods in the daemon:
 *    [ring] test_sec  - how long the incoming-call test rainbow holds
 *    [voip] max_sec   - silence grace (sec) before a voice call disarms
 *    [voip] packages  - messenger package list for VoIP call detection */
class CallView(context: Context) : ConfPage(context) {

    private lateinit var testSec: NumField
    private lateinit var maxSec: NumField
    private lateinit var packages: android.widget.EditText

    override fun buildBody() {
        body.addView(card {
            addView(sectionTitle("Incoming call (rainbow)"))
            addView(spacer(4))
            addView(text(
                "[ring] test_sec: how long the fake-dialer/test rainbow holds before disarm.",
                13f, parse("#FF909090")
            ))
            addView(spacer(4))
            testSec = numRow("test rainbow hold (sec)", "30")
            addView(spacer(8))
            addView(sectionTitle("Messenger / VoIP calls"))
            addView(spacer(4))
            addView(text(
                "[voip] max_sec: grace in seconds while the voice channel must stay silent " +
                    "before the LED disarms. [voip] packages: comma list of messenger apps " +
                    "triggering the VoIP call rainbow (empty = daemon default).",
                13f, parse("#FF909090")
            ))
            addView(spacer(4))
            maxSec = numRow("voip silence grace (sec)", "300")
        })
        body.addView(card {
            addView(sectionTitle("VoIP packages (comma separated)"))
            addView(spacer(6))
            packages = multiLine("com.whatsapp,com.telegram.messenger")
            addView(packages)
        })
    }

    override fun applyTo(c: LedConf) {
        testSec.setText(c.ringTestSec.toString())
        maxSec.setText(c.voipMaxSec.toString())
        packages.setText(c.voipPackages)
    }

    override fun collectFrom(c: LedConf) {
        c.ringTestSec = testSec.getLong(30L)
        c.voipMaxSec = maxSec.getLong(300L)
        c.voipPackages = packages.text.toString().trim()
            .split(',').filter { it.isNotBlank() }.joinToString(",")
    }
}
