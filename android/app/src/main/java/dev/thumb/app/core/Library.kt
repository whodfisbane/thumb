package dev.thumb.app.core

import android.content.Context
import android.content.pm.ApplicationInfo
import android.graphics.Bitmap
import androidx.core.graphics.drawable.toBitmap
import java.io.File
import java.util.zip.ZipFile

/**
 * The apps THUMB has patched on this phone. There is no separate database:
 * an app belongs here if THUMB installed it and it carries THUMB's runtime,
 * so the list is always what's really installed.
 */
object Library {
    /** UNKNOWN: the app hasn't reported yet (not opened since this THUMB). */
    enum class Obb { UNKNOWN, PENDING, PRESENT, MISSING }

    data class Entry(
        val packageName: String,
        val label: String,
        val versionName: String?,
        val versionCode: Long,
        val icon: Bitmap?,
        /** Base APK first, then splits (readable by THUMB). */
        val apks: List<File>,
        val options: PatchOptions?,
        /** Runtime id the app was patched with (null: patched before ids existed). */
        val runtime: String?,
        /** The app uses an OBB data file. */
        val needsObb: Boolean,
        val obb: Obb,
    )

    fun scan(context: Context): List<Entry> {
        val pm = context.packageManager
        val self = context.packageName
        return pm.getInstalledPackages(0).mapNotNull { pi ->
            val ai: ApplicationInfo = pi.applicationInfo ?: return@mapNotNull null
            val installer = runCatching { pm.getInstallSourceInfo(pi.packageName).installingPackageName }.getOrNull()
            if (installer != self) return@mapNotNull null
            val apks = listOf(File(ai.sourceDir)) + (ai.splitSourceDirs?.map { File(it) } ?: emptyList())
            var options: PatchOptions? = null
            var runtime: String? = null
            var hasThumb = false
            var needsObb = false
            for (apk in apks) runCatching {
                ZipFile(apk).use { zip ->
                    if (!needsObb) needsObb = zip.entries().asSequence().any {
                        it.name.endsWith(".dex") && zip.getInputStream(it).readBytes().containsAscii("getObbDir")
                    }
                    if (zip.getEntry("lib/arm64-v8a/libthumb.so") != null) hasThumb = true
                    zip.getEntry("assets/thumb/options.json")?.let { e ->
                        val json = org.json.JSONObject(zip.getInputStream(e).bufferedReader().readText())
                        options = PatchOptions.fromJson(json)
                        runtime = json.optString("runtime").ifEmpty { null }
                    }
                }
            }
            if (!hasThumb) return@mapNotNull null
            val store = ObbProvider.store(context, pi.packageName)
            val obbName = "main.${pi.longVersionCode}.${pi.packageName}.obb"
            val status = File(store, ObbProvider.STATUS + obbName).takeIf { it.isFile }?.readText()
            val obb = when {
                File(store, obbName).isFile -> Obb.PENDING
                status != null -> if (status.startsWith("1")) Obb.PRESENT else Obb.MISSING
                File(store, ObbProvider.DELIVERED + obbName).isFile -> Obb.PRESENT
                else -> Obb.UNKNOWN
            }
            Entry(
                packageName = pi.packageName,
                label = ai.loadLabel(pm).toString(),
                versionName = pi.versionName,
                versionCode = pi.longVersionCode,
                icon = runCatching { ai.loadIcon(pm).toBitmap(96, 96) }.getOrNull(),
                apks = apks,
                options = options,
                runtime = runtime,
                needsObb = needsObb,
                obb = obb,
            )
        }.sortedBy { it.label.lowercase() }
    }

    private fun ByteArray.containsAscii(needle: String): Boolean {
        val n = needle.toByteArray()
        outer@ for (i in 0..size - n.size) {
            for (j in n.indices) if (this[i + j] != n[j]) continue@outer
            return true
        }
        return false
    }
}
