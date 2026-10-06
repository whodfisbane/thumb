package dev.thumb.app.core

/**
 * Per-app choices made in THUMB before patching. Stored in the patched APK as
 * assets/thumb/options.json and read by the runtime and the in-game menu.
 */
data class PatchOptions(
    // ---- Build options (apply with or without the overlay) ----
    /** Block calls to known ad SDKs from the start. */
    val adblock: Boolean = true,
    /**
     * Compat shims for behaviour modern Android changed (virtual /, positioned
     * asset files). Always on in the UI: they only act where the app would
     * otherwise fail. Kept as a switch for troubleshooting.
     */
    val legacyFs: Boolean = true,
    /** Remove sensitive permissions (contacts, location, camera, ...). */
    val sandbox: Boolean = false,
    /** With [sandbox]: remove internet access too. */
    val blockInternet: Boolean = false,

    // ---- THUMB overlay (in-game menu) and its mods ----
    val overlay: Boolean = true,
    val fpsCounter: Boolean = true,
    val speed: Boolean = false,
    val fpsUnlock: Boolean = false,
    val adblockToggle: Boolean = false,
    val keepScreenOn: Boolean = true,
    val rotation: Boolean = true,
    val fullscreen: Boolean = true,
) {
    /** The overlay dex is only needed for the menu. */
    val needsOverlayCode get() = overlay

    fun toJson(): String = org.json.JSONObject().apply {
        put("version", 1)
        put("adblock", adblock)
        put("legacy_fs", legacyFs)
        put("overlay", overlay)
        put("mods", org.json.JSONObject().apply {
            put("fps_counter", overlay && fpsCounter)
            put("speed", overlay && speed)
            put("fps_unlock", overlay && fpsUnlock)
            put("adblock", overlay && adblockToggle)
            put("keep_screen_on", overlay && keepScreenOn)
            put("rotation", overlay && rotation)
            put("fullscreen", overlay && fullscreen)
        })
    }.toString(2)

    /** Permissions the sandbox removes from the manifest. */
    fun removedPermissions(): Set<String> = buildSet {
        if (sandbox) addAll(SENSITIVE_PERMISSIONS)
        if (sandbox && blockInternet) add("android.permission.INTERNET")
    }

    companion object {
        val SENSITIVE_PERMISSIONS = setOf(
            "android.permission.READ_CONTACTS", "android.permission.WRITE_CONTACTS", "android.permission.GET_ACCOUNTS",
            "android.permission.READ_PROFILE", "android.permission.ACCESS_FINE_LOCATION", "android.permission.ACCESS_COARSE_LOCATION",
            "android.permission.ACCESS_BACKGROUND_LOCATION", "android.permission.READ_PHONE_STATE", "android.permission.READ_PHONE_NUMBERS",
            "android.permission.CALL_PHONE", "android.permission.READ_CALL_LOG", "android.permission.WRITE_CALL_LOG",
            "android.permission.SEND_SMS", "android.permission.RECEIVE_SMS", "android.permission.READ_SMS",
            "android.permission.CAMERA", "android.permission.RECORD_AUDIO", "android.permission.READ_CALENDAR",
            "android.permission.WRITE_CALENDAR", "android.permission.BODY_SENSORS", "android.permission.ACTIVITY_RECOGNITION",
            "android.permission.USE_CREDENTIALS", "android.permission.MANAGE_ACCOUNTS", "android.permission.AUTHENTICATE_ACCOUNTS",
        )

        const val INFO_OVERLAY = "Adds a floating THUMB button inside the app. It may cover part of the screen; " +
            "drag it anywhere, or hide it with a three-finger double tap."
        const val INFO_LEGACY_FS = "Recommended. Old apps expect things modern Android changed: browsing from \"/\", " +
            "asset files positioned for them, and so on. THUMB quietly provides the old behaviour."
        const val WARN_SPEED = "Changing game speed can break timing-sensitive games, make audio stutter, or cause desyncs in online play."
        const val WARN_FPS_UNLOCK = "Many older games tie their game speed to the frame rate. Unlocking FPS may make the game run too fast, " +
            "break physics or animations, or drain more battery. If the game speeds up, use the speed slider to bring it back to 1×."
        const val WARN_ADBLOCK = "Blocks calls to known ad SDKs. Some apps may refuse features or crash if their ads can't load."
        const val WARN_SANDBOX = "Removes access to contacts, accounts, location, phone, SMS, camera, microphone, calendar and sensors. " +
            "Old apps often assume they have these and may crash or lose features (e.g. friend lists, maps, photos)."
        const val WARN_BLOCK_INTERNET = "The app can't go online at all: no online play, leaderboards, cloud saves or downloads. " +
            "Apps that require a connection to start may refuse to run."
        const val WARN_ROTATION = "Forcing an orientation an app wasn't designed for can stretch or cut off its screen."
        const val WARN_FULLSCREEN = "Some old apps draw their own buttons at the screen edges; hiding the system bars can make them hard to reach."
    }
}
