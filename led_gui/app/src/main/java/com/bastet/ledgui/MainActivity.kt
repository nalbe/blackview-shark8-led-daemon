package com.bastet.ledgui

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
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
    private lateinit var container: PagerContainer
    private lateinit var tabStatus: Button
    private lateinit var tabConfig: Button
    private var onStatus = true

    // drag state (follow-the-finger tab pager)
    private var dragActive = false
    private var dragOffset = 0f
    private var dragLeft = false          // fixed drag direction (true = toward Config)
    private var downX = 0f
    private var downY = 0f
    private val dragWidth: Float get() = resources.displayMetrics.widthPixels.toFloat()

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
        container = PagerContainer(this)
        container.addView(statusView, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT
        ))
        root.addView(container, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f
        ))

        setContentView(root)
        selectTab(true)
    }

    /** Follow-the-finger pager as a real ViewGroup interceptor. A child view
     *  that claims the touch (our sliders send requestDisallowInterceptTouchEvent
     *  on DOWN) wins over the pager; otherwise a mostly-horizontal drag flips
     *  tabs and swallows the stream so the inner ScrollView can't scroll too. */
    private inner class PagerContainer(ctx: Context) : FrameLayout(ctx) {

        override fun onInterceptTouchEvent(ev: MotionEvent): Boolean {
            when (ev.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    downX = ev.x
                    downY = ev.y
                    dragOffset = 0f
                    dragActive = false
                }
                MotionEvent.ACTION_MOVE -> {
                    if (!dragActive) {
                        val dx = ev.x - downX
                        val dy = ev.y - downY
                        if (Math.abs(dx) > dpi(28f) && Math.abs(dx) > Math.abs(dy)) {
                            dragActive = true
                            dragLeft = dx < 0
                            dragOffset = dx
                            beginDrag()
                            return true
                        }
                    }
                }
            }
            return false
        }

        override fun onTouchEvent(ev: MotionEvent): Boolean {
            if (!dragActive) return false
            when (ev.actionMasked) {
                MotionEvent.ACTION_MOVE -> {
                    dragOffset = ev.x - downX
                    updateDrag(dragOffset)
                    return true
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    finishDrag(commit = ev.actionMasked == MotionEvent.ACTION_UP)
                    dragActive = false
                    return true
                }
            }
            return true
        }
    }

    private fun dpi(v: Float) = (v * resources.displayMetrics.density).roundToInt()

    /** Create the neighbor page on demand and lay both out for a drag. */
    private fun beginDrag() {
        if (configView == null) {
            configView = ConfigView(this).also { v ->
                container.addView(v, FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT,
                    FrameLayout.LayoutParams.MATCH_PARENT
                ))
            }
        }
        val (front, back) = pages(onStatus)
        val backSide = if (dragLeft) dragWidth else -dragWidth
        front.translationX = 0f
        front.alpha = 1f
        back.translationX = backSide
        back.alpha = 1f
        front.visibility = View.VISIBLE
        back.visibility = View.VISIBLE
        updateDrag(dragOffset)
    }

    private fun pages(status: Boolean): Pair<View, View> {
        return if (status) statusView to configView!! else configView!! to statusView
    }

    /** Move both pages exactly with the finger. */
    private fun updateDrag(offset: Float) {
        if (configView == null) return
        val (front, back) = pages(onStatus)
        front.translationX = offset
        // neighbor lives on the opposite side of the fixed drag direction
        back.translationX = (if (dragLeft) dragWidth else -dragWidth) + offset
        // the farther the page slides in, the more it shows
        back.alpha = (kotlin.math.abs(offset) / dragWidth).coerceIn(0f, 1f)
    }

    /** Snap to whichever page we've dragged far enough toward. */
    private fun finishDrag(commit: Boolean) {
        if (configView == null) return
        val (front, back) = pages(onStatus)
        val threshold = dragWidth / 3f
        val doSwitch = commit && kotlin.math.abs(dragOffset) > threshold

        if (doSwitch) {
            // neighbor page becomes the front
            front.animate().translationX(
                if (dragLeft) -dragWidth else dragWidth
            ).alpha(0f).setDuration(180).start()
            back.animate().translationX(0f).alpha(1f).setDuration(180).withEndAction {
                onStatus = !onStatus
                statusView.visibility =
                    if (onStatus) View.VISIBLE else View.GONE
                configView?.visibility =
                    if (onStatus) View.GONE else View.VISIBLE
                front.translationX = 0f
                front.alpha = 1f
                tabsFor(onStatus)
                android.util.Log.d("ledgui", "TAB ${if (onStatus) "no STATUS" else "CONFIG"}")
            }.start()
        } else {
            // spring back to the current page
            front.animate().translationX(0f).alpha(1f).setDuration(180).start()
            back.animate().translationX(
                if (dragLeft) dragWidth else -dragWidth
            ).alpha(0f).setDuration(180).withEndAction {
                back.visibility = View.GONE
                back.translationX = 0f
                back.alpha = 1f
            }.start()
        }
    }

    private fun tabsFor(status: Boolean) {
        tabStatus.isSelected = status
        tabConfig.isSelected = !status
        paintTabs()
    }

    private fun selectTab(status: Boolean) {
        dragActive = false
        onStatus = status
        android.util.Log.d("ledgui", "TAB ${if (status) "config? no STATUS" else "CONFIG"}")
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
        statusView.translationX = 0f
        statusView.alpha = 1f
        configView?.translationX = 0f
        configView?.alpha = 1f
        tabsFor(status)
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