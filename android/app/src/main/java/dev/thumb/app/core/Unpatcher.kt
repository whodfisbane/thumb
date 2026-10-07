package dev.thumb.app.core

import java.io.File
import java.util.zip.ZipEntry
import java.util.zip.ZipFile

/**
 * Rebuilds the original 32-bit APK from one THUMB patched, so an installed app
 * can be patched again (new THUMB version, other options) without its APK:
 *   lib/arm64-v8a/libX_arm32.so  -> lib/armeabi-v7a/libX.so
 *   stubs, libthumb.so, assets/thumb/ and the in-game menu dex are dropped.
 * The manifest stays as patched (already updated for modern Android). Unsigned.
 */
object Unpatcher {
    private val OVERLAY_CLASS = "Ldev/thumb/overlay/ThumbOverlay;".toByteArray()

    /** Returns true if [input] was patched by THUMB. */
    fun unpatch(input: File, output: File): Boolean {
        var patched = false
        ZipFile(input).use { src ->
            output.outputStream().buffered(1 shl 16).use { os ->
                AlignedZipWriter(os).use { out ->
                    for (e in src.entries()) {
                        val name = e.name
                        when {
                            e.isDirectory -> Unit
                            name.startsWith("META-INF/") && Patcher.isSignatureFile(name) -> Unit
                            name.startsWith("assets/thumb/") -> patched = true
                            name.startsWith("lib/arm64-v8a/") -> {
                                patched = true
                                val lib = name.substringAfterLast('/')
                                if (lib.endsWith("_arm32.so"))
                                    out.addDeflated("lib/armeabi-v7a/" + lib.removeSuffix("_arm32.so") + ".so", src.getInputStream(e).readBytes(), e.time)
                            }
                            else -> {
                                val data = src.getInputStream(e).readBytes()
                                if (name.endsWith(".dex") && data.contains(OVERLAY_CLASS)) { patched = true; continue }
                                if (e.method == ZipEntry.STORED) out.addStored(name, data, e.time) else out.addDeflated(name, data, e.time)
                            }
                        }
                    }
                }
            }
        }
        return patched
    }

    private fun ByteArray.contains(needle: ByteArray): Boolean {
        outer@ for (i in 0..size - needle.size) {
            for (j in needle.indices) if (this[i + j] != needle[j]) continue@outer
            return true
        }
        return false
    }
}
