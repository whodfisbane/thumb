package dev.thumb.app.core

import android.content.Context
import java.io.File
import java.util.zip.ZipFile

/**
 * An app as THUMB receives it: a single .apk, or a bundle (.xapk / .apks /
 * .apkm) holding a base APK, split APKs and sometimes OBB files.
 */
class Bundle(val apks: List<File>, val obbs: List<File>) {
    /** The base APK (has the app's code and main manifest). */
    val base: File get() = apks.first()

    companion object {
        /** Unpacks [file] into [workDir] if it is a bundle; a plain APK is used as is. */
        fun open(file: File, workDir: File): Bundle {
            workDir.deleteRecursively()
            workDir.mkdirs()
            val entries = ZipFile(file).use { zip -> zip.entries().asSequence().map { it.name }.toList() }
            if ("AndroidManifest.xml" in entries) return Bundle(listOf(file), emptyList()) // plain APK
            val apks = ArrayList<File>()
            val obbs = ArrayList<File>()
            ZipFile(file).use { zip ->
                for (e in zip.entries()) {
                    if (e.isDirectory) continue
                    val name = e.name.substringAfterLast('/')
                    val target = when {
                        e.name.endsWith(".apk") -> File(workDir, name).also { apks += it }
                        e.name.endsWith(".obb") -> File(workDir, name).also { obbs += it }
                        else -> continue
                    }
                    zip.getInputStream(e).use { input -> target.outputStream().use { input.copyTo(it, 1 shl 20) } }
                }
            }
            require(apks.isNotEmpty()) { "No APK inside this file" }
            // Base first: "base.apk", else the one that isn't a config/split, else the biggest.
            apks.sortWith(compareBy<File>(
                { if (it.name == "base.apk") 0 else 1 },
                { if (it.name.startsWith("config.") || it.name.startsWith("split_")) 1 else 0 },
                { -it.length() },
            ))
            return Bundle(apks, obbs)
        }
    }
}
