package dev.thumb.app.core

import java.io.ByteArrayInputStream
import java.io.File
import java.util.zip.ZipFile
import java.util.zip.ZipInputStream

/**
 * THUMB Doctor: checks the imports of an app's 32-bit libraries against the
 * functions this THUMB build implements. Port of tools/thumb-doctor.py.
 */
object Doctor {
    data class LibReport(
        val name: String,
        val handled: Int,
        val total: Int,
        val systemLibs: List<String>,
        val missing: Map<String, List<String>>, // category -> symbols
        val error: String? = null,
    ) {
        /** System libraries it needs that apps may not load on modern Android (Android 7+). */
        val privateLibs get() = systemLibs.filter { it !in PUBLIC_LIBS }
        /** Such a library can't load even on a real 32-bit phone today; apps treat it as optional. */
        val skipped get() = privateLibs.isNotEmpty()
    }

    enum class Level { GOOD, PARTIAL, BAD }

    /** NDK libraries every app may load (the rest are private to the system since Android 7). */
    val PUBLIC_LIBS = setOf(
        "libc.so", "libm.so", "libdl.so", "liblog.so", "libz.so", "libstdc++.so", "libandroid.so", "libjnigraphics.so",
        "libEGL.so", "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so", "libOpenSLES.so", "libOpenMAXAL.so",
        "libaaudio.so", "libvulkan.so", "libmediandk.so", "libcamera2ndk.so", "libnativewindow.so", "libsync.so",
        "libamidi.so", "libbinder_ndk.so", "libneuralnetworks.so", "libicu.so",
    )

    private val CRITICAL = setOf("OpenGL ES 2/3", "EGL", "NDK window / assets / input")

    data class Report(val libs: List<LibReport>) {
        val counted get() = libs.filter { !it.skipped }
        val skipped get() = libs.filter { it.skipped }
        val handled get() = counted.sumOf { it.handled }
        val total get() = counted.sumOf { it.total }
        val percent get() = if (total == 0) 100 else handled * 100 / total
        val needsThumb get() = libs.isNotEmpty()
        /** Missing one of these means no picture or no window at all, whatever the percentage. */
        val critical get() = counted.any { lib -> lib.missing.keys.any { it in CRITICAL } }
        val level get() = when {
            percent == 100 -> Level.GOOD
            percent >= 90 && !critical -> Level.PARTIAL
            else -> Level.BAD
        }
        val verdict get() = when {
            !needsThumb -> "No 32-bit libraries: this app doesn't need THUMB"
            level == Level.GOOD -> "Should work"
            level == Level.PARTIAL -> "Might partly work"
            else -> "Missing too much"
        }
        val detail get() = when (level) {
            Level.GOOD -> "Everything this app needs from the system is supported."
            Level.PARTIAL -> "A few functions aren't supported yet. If the app uses them, parts of it may not work."
            Level.BAD -> if (critical) "It needs graphics or system features THUMB doesn't support yet. You can still try."
                else "Many functions it needs aren't supported yet. You can still try."
        }
    }

    private val categories: List<Pair<String, (String) -> Boolean>> = listOf(
        "OpenGL ES 2/3" to { s -> s.startsWith("gl") && s.getOrNull(2)?.isUpperCase() == true },
        "EGL" to { s -> s.startsWith("egl") },
        "OpenSL ES (audio)" to { s -> s.startsWith("SL") || s.startsWith("slCreate") },
        "AAudio (audio)" to { s -> s.startsWith("AAudio") },
        "NDK window / assets / input" to { s ->
            listOf("ANativeWindow", "AAsset", "AInput", "ALooper", "AConfiguration", "ANativeActivity", "ASensor", "AKey", "AMotion")
                .any { s.startsWith(it) }
        },
        "OpenAL (audio)" to { s -> (s.startsWith("al") || s.startsWith("alc")) && s.getOrNull(2)?.isUpperCase() == true },
        "Dynamic loading" to { s -> s in setOf("dlopen", "dlsym", "dlclose", "dlerror", "android_dlopen_ext") },
        "C++ runtime" to { s -> s.startsWith("_Z") },
        "Threads" to { s -> s.startsWith("pthread_") || s.startsWith("sem_") },
        "Networking" to { s ->
            s in setOf("socket", "bind", "listen", "accept", "connect", "send", "recv", "sendto", "recvfrom", "getsockopt",
                "setsockopt", "getaddrinfo", "gethostbyname", "inet_addr", "inet_pton", "inet_ntop", "shutdown")
        },
    )

    fun category(sym: String) = categories.firstOrNull { it.second(sym) }?.first ?: "libc / other"

    /** All armeabi-v7a libraries in an APK, or inside the APKs of a bundle (.xapk/.apks/.apkm). */
    fun armLibs(file: File): Map<String, ByteArray> {
        val out = LinkedHashMap<String, ByteArray>()
        ZipFile(file).use { zip ->
            for (e in zip.entries()) {
                if (e.name.startsWith("lib/armeabi-v7a/") && e.name.endsWith(".so")) {
                    out[e.name.substringAfterLast('/')] = zip.getInputStream(e).readBytes()
                } else if (e.name.endsWith(".apk")) {
                    ZipInputStream(ByteArrayInputStream(zip.getInputStream(e).readBytes())).use { inner ->
                        while (true) {
                            val ie = inner.nextEntry ?: break
                            if (ie.name.startsWith("lib/armeabi-v7a/") && ie.name.endsWith(".so"))
                                out[ie.name.substringAfterLast('/')] = inner.readBytes()
                        }
                    }
                }
            }
        }
        return out
    }

    fun check(file: File, supported: Set<String>): Report = check(listOf(file), supported)

    /** Checks the libraries of several APKs together (a base APK and its splits). */
    fun check(files: List<File>, supported: Set<String>): Report {
        val parsed = LinkedHashMap<String, Elf32.Info>()
        val errors = LinkedHashMap<String, String>()
        for ((name, bytes) in files.flatMap { armLibs(it).entries }.associate { it.key to it.value }) {
            try {
                parsed[name] = Elf32(bytes).parse()
            } catch (e: Exception) {
                errors[name] = e.message ?: e.toString()
            }
        }
        val appExports = parsed.values.flatMap { it.exports }.toSet()
        val reports = parsed.map { (name, info) ->
            val missing = info.imports.filter { (sym, weak) -> !weak && sym !in supported && sym !in appExports }.keys.sorted()
            LibReport(
                name = name,
                handled = info.imports.size - missing.size,
                total = info.imports.size,
                systemLibs = info.needed.filter { it !in parsed },
                missing = missing.groupBy { category(it) },
            )
        } + errors.map { (name, err) -> LibReport(name, 0, 0, emptyList(), emptyMap(), err) }
        return Report(reports)
    }
}
