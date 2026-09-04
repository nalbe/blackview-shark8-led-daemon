package com.bastet.ledgui

import android.content.Context
import android.widget.LinearLayout

/** Charge tab: thresholds + charge breath timing + the three charge bands
 *  (lower / middle / upper), each with its own light type, custom breath
 *  toggle and color. All shared widgets live in UiKit. */
class ChargeView(context: Context) : ConfPage(context) {

    private val first = NumHolder(context, "first", "90")
    private val second = NumHolder(context, "second", "95")
    private val rise = NumHolder(context, "rise", "700")
    private val hold = NumHolder(context, "hold", "100")
    private val fall = NumHolder(context, "fall", "700")
    private val offt = NumHolder(context, "off", "900")
    private val softCycle = NumHolder(context, "breath phase (ms)", "25500")
    private val lower by lazy { RangeEditor("Lower range (below first threshold)") }
    private val middle by lazy { RangeEditor("Middle range (between thresholds)") }
    private val upper by lazy { RangeEditor("Upper range (>= second threshold / Full)") }

    /** Small holder so a NumField can be declared once and still be
     *  attached to the card only when the body is built. */
    protected inner class NumHolder(private val buildCtx: Context, private val label: String, private val def: String) {
        lateinit var field: NumField
            private set
        fun build(into: LinearLayout) {
            field = into.numRow(label, def)
        }
    }

    override fun buildBody() {
        body.addView(card {
            addView(sectionTitle("Charge thresholds (percent, order-free)"))
            addView(spacer(4))
            first.build(this)
            second.build(this)
            addView(spacer(4))
            addView(sectionTitle("Charge timing (ms)"))
            addView(spacer(4))
            rise.build(this)
            hold.build(this)
            fall.build(this)
            offt.build(this)
            addView(spacer(4))
            addView(sectionTitle("Custom breathing timing (ms)"))
            addView(spacer(4))
            softCycle.build(this)
        })
        body.addView(lower)
        body.addView(middle)
        body.addView(upper)
    }

    override fun applyTo(c: LedConf) {
        first.field.setText(c.firstThreshold.toString())
        second.field.setText(c.secondThreshold.toString())
        rise.field.setText(c.chargeRise.toString())
        hold.field.setText(c.chargeHold.toString())
        fall.field.setText(c.chargeFall.toString())
        offt.field.setText(c.chargeOfft.toString())
        softCycle.field.setText(c.softCycleMs.toString())
        lower.load(c.lowerType, c.lowerColor, c.lowerSoftBreath)
        middle.load(c.middleType, c.middleColor, c.middleSoftBreath)
        upper.load(c.upperType, c.upperColor, c.upperSoftBreath)
    }

    override fun collectFrom(c: LedConf) {
        c.firstThreshold = first.field.getInt(90)
        c.secondThreshold = second.field.getInt(95)
        c.chargeRise = rise.field.getInt(700)
        c.chargeHold = hold.field.getInt(100)
        c.chargeFall = fall.field.getInt(700)
        c.chargeOfft = offt.field.getInt(900)
        c.softCycleMs = softCycle.field.getInt(25500)
        c.lowerType = lower.type.getType()
        c.middleType = middle.type.getType()
        c.upperType = upper.type.getType()
        c.lowerColor = lower.color.getColor()
        c.middleColor = middle.color.getColor()
        c.upperColor = upper.color.getColor()
        c.lowerSoftBreath = lower.soft.isChecked
        c.middleSoftBreath = middle.soft.isChecked
        c.upperSoftBreath = upper.soft.isChecked
    }
}
