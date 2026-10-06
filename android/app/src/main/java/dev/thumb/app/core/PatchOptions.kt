package dev.thumb.app.core

/**
 * Per-app choices made in THUMB before patching. Stored in the patched APK as
 * assets/thumb/options.json and read by the runtime and the in-game menu.
 */
data class PatchOptions(
    /** Build option: block calls to known ad SDKs from the start. */
    val adblock: Boolean = true,
    /** Add the THUMB in-game menu (floating button). */
    val overlay: Boolean = true,
    // Mods available in the menu (only when [overlay] is on).
    val fpsCounter: Boolean = true,
    val speed: Boolean = true,
    val fpsUnlock: Boolean = false,
    val adblockToggle: Boolean = false,
) {
    fun toJson(): String = org.json.JSONObject().apply {
        put("version", 1)
        put("adblock", adblock)
        put("overlay", overlay)
        put("mods", org.json.JSONObject().apply {
            put("fps_counter", overlay && fpsCounter)
            put("speed", overlay && speed)
            put("fps_unlock", overlay && fpsUnlock)
            put("adblock", overlay && adblockToggle)
        })
    }.toString(2)

    companion object {
        const val WARN_OVERLAY = "Adds a floating THUMB button inside the app. It may cover part of the screen; " +
            "drag it anywhere, or hide it with a three-finger double tap."
        const val WARN_SPEED = "Changing game speed can break timing-sensitive games, make audio stutter, or cause desyncs in online play."
        const val WARN_FPS_UNLOCK = "Many older games tie their game speed to the frame rate. Unlocking FPS may make the game run too fast, " +
            "break physics or animations, or drain more battery. If the game speeds up, use the speed slider to bring it back to 1×."
        const val WARN_ADBLOCK = "Blocks calls to known ad SDKs. Some apps may refuse features or crash if their ads can't load."
    }
}
