package dev.thumb.app.core

/**
 * Per-app choices made in THUMB before patching. Stored in the patched APK as
 * assets/thumb/options.json and read by the runtime and the in-game menu.
 */
data class PatchOptions(
    // ---- Build options (apply with or without the overlay) ----
    /** Custom app name shown in the launcher (null or blank: keep the app's own). */
    val label: String? = null,
    /** Block calls to known ad SDKs from the start. */
    val adblock: Boolean = true,
    /** FPS from the start: "compat" (60, what old games were built for) or "default" (screen decides). */
    val fpsMode: String = "compat",
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
    /** The in-game menu, with every mod (each starts neutral: the app's own behaviour). */
    val overlay: Boolean = true,
) {
    /** [runtime]: id of the THUMB runtime the app is patched with (Library shows updates). */
    fun toJson(runtime: String? = null): String = org.json.JSONObject().apply {
        put("version", 1)
        runtime?.let { put("runtime", it) }
        put("adblock", adblock)
        put("fps_mode", fpsMode)
        put("legacy_fs", legacyFs)
        put("overlay", overlay)
        label?.takeIf { it.isNotBlank() }?.let { put("label", it) }
        put("sandbox", sandbox)
        put("block_internet", blockInternet)
        // The menu always has every mod; "mods" stays so the runtime format can grow per-mod switches again.
        put("mods", org.json.JSONObject().apply {
            for (m in listOf("fps_counter", "speed", "fps_unlock", "adblock", "keep_screen_on")) put(m, overlay)
        })
    }.toString(2)

    /** Permissions the sandbox removes from the manifest. */
    fun removedPermissions(): Set<String> = buildSet {
        if (sandbox) addAll(SENSITIVE_PERMISSIONS)
        if (sandbox && blockInternet) add("android.permission.INTERNET")
    }

    companion object {
        /** The options an installed app was patched with (for re-patching). */
        fun fromJson(json: org.json.JSONObject) = PatchOptions(
            adblock = json.optBoolean("adblock", true),
            fpsMode = json.optString("fps_mode", "compat").let { if (it == "default") it else "compat" },
            legacyFs = json.optBoolean("legacy_fs", true),
            sandbox = json.optBoolean("sandbox", false),
            blockInternet = json.optBoolean("block_internet", false),
            overlay = json.optBoolean("overlay", true),
            label = json.optString("label").ifBlank { null },
        )

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

        const val INFO_OVERLAY = "A floating THUMB button inside the app opens a menu with:\n" +
            "• FPS counter (in the menu or on screen)\n" +
            "• Speed slider: slow motion or fast-forward\n" +
            "• FPS unlock: 30 FPS up to your screen's maximum\n" +
            "• Ad-block on/off\n" +
            "• Keep screen on\n" +
            "• Force restart / kill (for frozen apps)\n" +
            "• Hide the button (three-finger double tap brings it back)\n\n" +
            "Everything starts as the app normally behaves; nothing changes until you use it. " +
            "The button may cover part of the screen: drag it anywhere, or hide it with a three-finger double tap."
        const val INFO_LEGACY_FS = "Recommended. Old apps expect things modern Android changed: browsing from \"/\", " +
            "asset files positioned for them, and so on. THUMB quietly provides the old behaviour."
        const val WARN_SPEED = "Changing game speed can break timing-sensitive games, make audio stutter, or cause desyncs in online play."
        const val WARN_FPS_HIGH = "Most legacy games were built for 60 FPS. Running them faster can break physics, animations or game speed (for example characters moving or jumping too fast)."
        const val WARN_ADBLOCK = "Blocks calls to known ad SDKs. Some apps may refuse features or crash if their ads can't load."
        const val WARN_SANDBOX = "Removes access to contacts, accounts, location, phone, SMS, camera, microphone, calendar and sensors. " +
            "Old apps often assume they have these and may crash or lose features (e.g. friend lists, maps, photos)."
        const val WARN_BLOCK_INTERNET = "The app can't go online at all: no online play, leaderboards, cloud saves or downloads. " +
            "Apps that require a connection to start may refuse to run."
    }
}
