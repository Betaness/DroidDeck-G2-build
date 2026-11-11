package com.droiddeck.launcher.input

import android.app.Activity
import android.content.Context
import android.os.Build
import android.text.InputType
import android.view.KeyCharacterMap
import android.view.KeyEvent
import android.view.View
import android.view.WindowInsets
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager
import com.droiddeck.launcher.wayland.CompositorHost
import com.droiddeck.launcher.wayland.WaylandCompositor

class KeyboardHost(
    context: Context,
    private val sendKeyEvent: (KeyEvent) -> Boolean,
) : View(context) {
    constructor(activity: Activity) : this(activity, { activity.dispatchKeyEvent(it) })

    private val imm = context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
    private val map: KeyCharacterMap = KeyCharacterMap.load(KeyCharacterMap.VIRTUAL_KEYBOARD)
    var shown = false
        private set
    var imeShowing = false
        private set
    var onImeChanged: ((Boolean) -> Unit)? = null

    init {
        isFocusable = false
        isFocusableInTouchMode = false
        visibility = VISIBLE
        layoutParams = android.widget.FrameLayout.LayoutParams(1, 1)
    }

    override fun onCheckIsTextEditor(): Boolean = shown

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection? {
        if (!shown) return null
        // A password field keeps IMEs on a Latin layout that types key by key; pinyin, kana and
        // hangul need a plain text field, where they compose before committing.
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS or
            (if (fullIme()) 0 else InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD)
        outAttrs.imeOptions = EditorInfo.IME_ACTION_NONE or EditorInfo.IME_FLAG_NO_FULLSCREEN or
            EditorInfo.IME_FLAG_NO_EXTRACT_UI
        return KeyConnection()
    }

    @Suppress("DEPRECATION")
    private fun fullIme(): Boolean {
        val locales = resources.configuration.locales
        val languages = (0 until locales.size()).mapTo(mutableListOf<String>()) { locales[it].language }
        imm.currentInputMethodSubtype?.let {
            languages += it.languageTag.ifEmpty { it.locale }.substringBefore('-').substringBefore('_')
        }
        return languages.any { it in COMPOSING_LANGUAGES }
    }

    override fun onApplyWindowInsets(insets: WindowInsets): WindowInsets {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val visible = insets.isVisible(WindowInsets.Type.ime())
            if (visible != imeShowing) {
                imeShowing = visible
                onImeChanged?.invoke(visible)
            }
        }
        return super.onApplyWindowInsets(insets)
    }

    override fun onKeyPreIme(keyCode: Int, event: KeyEvent): Boolean {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R || keyCode != KeyEvent.KEYCODE_BACK || !shown) {
            return super.onKeyPreIme(keyCode, event)
        }
        if (event.action == KeyEvent.ACTION_UP && !event.isCanceled) {
            hide()
            onImeChanged?.invoke(false)
        }
        return true
    }

    fun toggle() = if (shown && imeVisible()) hide() else show()

    private fun imeVisible() = Build.VERSION.SDK_INT < Build.VERSION_CODES.R ||
        rootWindowInsets?.isVisible(WindowInsets.Type.ime()) != false

    fun show() {
        shown = true
        isFocusable = true
        isFocusableInTouchMode = true
        requestFocus()
        imm.restartInput(this)
        imm.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT)
    }

    fun hide() {
        shown = false
        imm.hideSoftInputFromWindow(windowToken, 0)
        isFocusable = false
        isFocusableInTouchMode = false
        clearFocus()
    }

    // Whole, so ASCII in it keeps its place: the compositor types it into X programs too.
    fun type(text: CharSequence?) {
        if (text.isNullOrEmpty()) return
        val events = map.getEvents(text.toString().toCharArray())
        if (events != null) {
            for (event in events) sendKeyEvent(event)
        } else if (CompositorHost.isStarted) {
            WaylandCompositor.textInputCommit(text.toString())
        }
    }

    fun tap(keyCode: Int) {
        val t = System.currentTimeMillis()
        sendKeyEvent(KeyEvent(t, t, KeyEvent.ACTION_DOWN, keyCode, 0))
        sendKeyEvent(KeyEvent(t, t, KeyEvent.ACTION_UP, keyCode, 0))
    }

    private inner class KeyConnection : BaseInputConnection(this, false) {
        // Composing text stays in the IME's candidate bar; programs get it once committed.
        private var composing = ""

        override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean {
            composing = ""
            type(text)
            return true
        }

        override fun setComposingText(text: CharSequence?, newCursorPosition: Int): Boolean {
            composing = text?.toString().orEmpty()
            return true
        }

        override fun finishComposingText(): Boolean {
            val text = composing
            composing = ""
            type(text)
            return true
        }

        override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
            repeat(beforeLength) { tap(KeyEvent.KEYCODE_DEL) }
            repeat(afterLength) { tap(KeyEvent.KEYCODE_FORWARD_DEL) }
            return true
        }

        override fun sendKeyEvent(event: KeyEvent): Boolean = this@KeyboardHost.sendKeyEvent(event)

        override fun performEditorAction(actionCode: Int): Boolean {
            tap(KeyEvent.KEYCODE_ENTER)
            return true
        }

        override fun getTextBeforeCursor(n: Int, flags: Int): CharSequence = ""
        override fun getTextAfterCursor(n: Int, flags: Int): CharSequence = ""
        override fun getSelectedText(flags: Int): CharSequence? = null
    }

    private companion object {
        val COMPOSING_LANGUAGES = setOf("zh", "ja", "ko")
    }
}
