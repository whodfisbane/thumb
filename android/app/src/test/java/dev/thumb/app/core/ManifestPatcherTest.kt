package dev.thumb.app.core

import java.io.File
import java.util.zip.ZipFile
import java.util.zip.ZipOutputStream
import java.util.zip.ZipEntry
import org.junit.Assume.assumeTrue
import org.junit.Test

/**
 * Runs the manifest edits against a real APK (set THUMB_TEST_APK) and writes
 * a copy with the edited manifest so `aapt dump` can check it.
 */
class ManifestPatcherTest {
    @Test
    fun removePermissionsAndPatch() {
        val path = System.getenv("THUMB_TEST_APK")
        assumeTrue("THUMB_TEST_APK not set", path != null)
        val apk = File(path!!)
        val out = File(System.getenv("THUMB_TEST_OUT") ?: "build/manifest-test.apk")
        ZipFile(apk).use { zip ->
            val manifest = zip.getInputStream(zip.getEntry("AndroidManifest.xml")).readBytes()
            val (stripped, removed) = ManifestPatcher.removePermissions(manifest, PatchOptions.SENSITIVE_PERMISSIONS + "android.permission.INTERNET")
            println("removed: $removed")
            val renamed = ManifestPatcher.setLabel(stripped, "Wörms 3 (THUMB) 👍") ?: error("setLabel failed")
            val patched = ManifestPatcher.patch(renamed, Patcher.MIN_TARGET_SDK).bytes
            ZipOutputStream(out.outputStream()).use { zo ->
                for (e in zip.entries()) {
                    zo.putNextEntry(ZipEntry(e.name))
                    zo.write(if (e.name == "AndroidManifest.xml") patched else zip.getInputStream(e).readBytes())
                    zo.closeEntry()
                }
            }
        }
    }
}
