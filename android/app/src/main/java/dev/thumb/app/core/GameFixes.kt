package dev.thumb.app.core

/**
 * Per-game fixes (like Proton's): small tweaks for one app, applied inside it
 * by THUMB's helper (overlay GameFixes.java). Offered only for their app.
 */
object GameFixes {
    data class Fix(val id: String, val title: String, val description: String, val default: Boolean = true)

    private val BY_PACKAGE = mapOf(
        "com.worms3.app" to listOf(
            Fix(
                "worms3-audio-latency", "Lower audio delay",
                "Worms 3 buffers about a third of a second of sound on modern phones, so explosions are heard late. " +
                    "This halves the buffer. Turn it off if the sound crackles.",
            ),
        ),
    )

    fun forPackage(pkg: String): List<Fix> = BY_PACKAGE[pkg].orEmpty()

    /** Fixes on by default for [pkg]. */
    fun defaults(pkg: String): Set<String> = forPackage(pkg).filter { it.default }.map { it.id }.toSet()
}
