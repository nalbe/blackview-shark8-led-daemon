package com.bastet.ledgui

import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.view.Gravity
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import androidx.activity.ComponentActivity
import kotlin.math.roundToInt

/** Classic-Views shell (Compose built no smooth 120Hz scrolling on this
 * firmware; the plain View framework does). Two tabs, Status + Config. */
class MainActivity : ComponentActivity() {

    private lateinit var statusView: StatusView
    private var configView: ConfigView? = null
    private lateinit var container: FrameLayout
    private lateinit var tabStatus: Button
    private lateinit var tabConfig: Button

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        statusView = StatusView(this)

        val root = LinearLayout(this)
        root.orientation = LinearLayout.VERTICAL
        root.setBackgroundColor(Color.parseColor("#FF121212"))

        // top tab strip
        val strip = LinearLayout(this)
        strip.orientation = LinearLayout.HORIZONTAL
        strip.setPadding(dpi(12f), dpi(8f), dpi(12f), dpi(4f))
        tabStatus = tabButton("Status") { selectTab(true) }
        tabConfig = tabButton("Config") { selectTab(false) }
        strip.addView(tabStatus)
        strip.addView(tabConfig)
        root.addView(strip)

        // content area
        container = FrameLayout(this)
        container.addView(statusView, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT
        ))
        root.addView(container, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f
        ))

        setContentView(root)
        selectTab(true)
    }

    private fun dpi(v: Float) = (v * resources.displayMetrics.density).roundToInt()

    private fun selectTab(status: Boolean) {
        val cw = configView
        statusView.visibility =
            if (status) android.view.View.VISIBLE else android.view.View.GONE
        if (!status && cw == null) {
            configView = ConfigView(this).also { v ->
                container.addView(v, FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT,
                    FrameLayout.LayoutParams.MATCH_PARENT
                ))
            }
        }
        configView?.visibility =
            if (status) android.view.View.GONE else android.view.View.VISIBLE
        tabStatus.isSelected = status
        tabConfig.isSelected = !status
        paintTabs()
    }

    private fun paintTabs() {
        styleTab(tabStatus, tabStatus.isSelected)
        styleTab(tabConfig, tabConfig.isSelected)
    }

    private fun styleTab(b: Button, selected: Boolean) {
        val g = GradientDrawable()
        g.shape = GradientDrawable.RECTANGLE
        g.cornerRadius = resources.displayMetrics.density * 8f
        if (selected) {
            g.setColor(Color.parseColor("#FF1565C0"))
            b.setTextColor(Color.WHITE)
        } else {
            g.setColor(Color.parseColor("#FF2A2A2A"))
            b.setTextColor(Color.parseColor("#FFB0BEC5"))
        }
        b.background = g
    }

    private fun tabButton(label: String, onClick: () -> Unit): Button {
        val b = Button(this)
        b.text = label
        b.isAllCaps = false
        b.textSize = 15f
        b.gravity = Gravity.CENTER
        b.setPadding(0, 0, 0, 0)
        b.setMinHeight(0)
        b.minHeight = 0
        b.minimumHeight = 0
        val lp = LinearLayout.LayoutParams(0, dpi(44f), 1f)
        lp.setMargins(0, 0, dpi(8f), 0)
        b.layoutParams = lp
        b.setOnClickListener { onClick() }
        return b
    }
}