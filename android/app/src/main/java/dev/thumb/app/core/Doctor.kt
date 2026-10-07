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
    )

    data class Report(val libs: List<LibReport>) {
        val handled get() = libs.sumOf { it.handled }
        val total get() = libs.sumOf { it.total }
        val percent get() = if (total == 0) 100 else handled * 100 / total
        val needsThumb get() = libs.isNotEmpty()
        val verdict get() = when {
            !needsThumb -> "No 32-bit libraries: this app doesn't need THUMB"
            percent == 100 -> "Ready to try"
            percent >= 90 -> "Worth a try (missing calls are stubbed and may not matter)"
            else -> "Needs work"
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
