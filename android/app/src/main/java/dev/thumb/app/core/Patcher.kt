package dev.thumb.app.core

import java.io.File
import java.util.zip.ZipEntry
import java.util.zip.ZipFile

/**
 * Builds an arm64-compatible copy of a 32-bit-only APK (the on-device twin of
 * tools/repack.py). For every lib/armeabi-v7a/libX.so:
 *   lib/arm64-v8a/libX.so        THUMB stub
 *   lib/arm64-v8a/libX_arm32.so  the original library
 * plus lib/arm64-v8a/libthumb.so (the translator). The result is unsigned.
 */
class Patcher(private val runtime: ByteArray, private val stub: ByteArray) {
    data class Result(val libs: List<String>, val oldTargetSdk: Int?, val newTargetSdk: Int?)

    companion object {
        /** Android 14+ blocks installs below 23; 24 is the floor on newer releases. */
        const val MIN_TARGET_SDK = 24
    }

    fun patch(input: File, output: File, log: (String) -> Unit = {}): Result {
        val started = System.nanoTime()
        var copied = 0
        var copiedBytes = 0L
        val libs = ArrayList<String>()
        var targetOld: Int? = null
        var targetNew: Int? = null
        ZipFile(input).use { src ->
            output.outputStream().buffered(1 shl 16).use { os ->
                AlignedZipWriter(os).use { out ->
                    val armLibs = ArrayList<ZipEntry>()
                    for (e in src.entries()) {
                        val name = e.name
                        when {
                            e.isDirectory -> Unit
                            name.startsWith("lib/") -> if (name.startsWith("lib/armeabi-v7a/") && name.endsWith(".so")) armLibs += e
                            name.startsWith("META-INF/") && isSignatureFile(name) -> Unit // old signature
                            name == "AndroidManifest.xml" -> {
                                val r = ManifestPatcher.patch(src.getInputStream(e).readBytes(), MIN_TARGET_SDK)
                                targetOld = r.oldTarget
                                targetNew = r.newTarget
                                r.newTarget?.let { log("targetSdkVersion ${r.oldTarget} -> $it") }
                                if (r.forcedExtractNativeLibs) log("extractNativeLibs false -> true")
                                out.addDeflated(name, r.bytes, e.time)
                            }
                            else -> {
                                val data = src.getInputStream(e).readBytes()
                                if (e.method == ZipEntry.STORED) out.addStored(name, data, e.time) else out.addDeflated(name, data, e.time)
                                copied++
                                copiedBytes += data.size
                            }
                        }
                    }
                    if (armLibs.isEmpty()) {
                        // A split without native code (or the base of a bundle): just re-signed.
                        log("copied $copied files unchanged (${copiedBytes / 1024} KB), no 32-bit libraries here")
                        return@use
                    }
                    log("copied $copied files unchanged (${copiedBytes / 1024} KB): code, resources, assets")
                    val now = System.currentTimeMillis()
                    for (e in armLibs) {
                        val lib = e.name.substringAfterLast('/')
                        val original = lib.removeSuffix(".so") + "_arm32.so"
                        out.addDeflated("lib/arm64-v8a/$lib", stub, now)
                        out.addDeflated("lib/arm64-v8a/$original", src.getInputStream(e).readBytes(), e.time)
                        libs += lib
                        log("$lib: stub + $original")
                    }
                    out.addDeflated("lib/arm64-v8a/libthumb.so", runtime, now)
                    log("added libthumb.so (translator runtime, ${runtime.size / 1024} KB)")
                }
            }
        }
        log("patched ${input.name} in ${(System.nanoTime() - started) / 1_000_000} ms -> ${output.length() / 1024} KB")
        return Result(libs, targetOld, targetNew)
    }

    private fun isSignatureFile(name: String): Boolean {
        val upper = name.uppercase()
        return listOf(".SF", ".RSA", ".DSA", ".EC", ".MF").any { upper.endsWith(it) }
    }
}
