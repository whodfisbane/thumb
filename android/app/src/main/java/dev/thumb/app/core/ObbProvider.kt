package dev.thumb.app.core

import android.content.ContentProvider
import android.content.ContentValues
import android.content.Context
import android.content.pm.PackageManager
import android.database.Cursor
import android.net.Uri
import android.os.ParcelFileDescriptor
import java.io.File

/**
 * Hands imported OBB files to the games they belong to.
 *
 * Android doesn't let THUMB write into another app's Android/obb folder, so
 * THUMB keeps the OBB privately and grants the game read access to it. On first
 * launch, THUMB's runtime inside the game copies it into the game's own OBB
 * folder (which the game may write), then deletes THUMB's copy.
 *
 * URI: content://dev.thumb.app.obb/<package>/<file name>
 * Only <package> itself may read or delete it, and only if it was patched by
 * THUMB on this phone (signed with this phone's THUMB key).
 */
class ObbProvider : ContentProvider() {
    companion object {
        const val AUTHORITY = "dev.thumb.app.obb"
        /** Marker left after the game took its OBB (prefix + OBB name). */
        const val DELIVERED = "delivered-"
        /** What the game last reported about its OBB (prefix + OBB name): "1 <size>" or "0". */
        const val STATUS = "status-"

        fun store(context: Context, pkg: String) = File(context.filesDir, "obb/$pkg")
        fun uri(pkg: String, name: String): Uri = Uri.parse("content://$AUTHORITY/$pkg/$name")
    }

    override fun onCreate() = true

    /** Returns the file for [uri] if the caller is the app it belongs to. */
    private fun authorizedFile(uri: Uri): File {
        val ctx = context ?: throw SecurityException("no context")
        val segments = uri.pathSegments
        require(segments.size == 2) { "bad OBB uri $uri" }
        val (pkg, name) = segments
        require(!name.contains('/') && !name.startsWith(".")) { "bad OBB name" }
        val caller = callingPackage
        if (caller != pkg) throw SecurityException("$caller may not read OBBs of $pkg")
        if (!isPatchedByUs(ctx, pkg)) throw SecurityException("$pkg was not patched by THUMB on this phone")
        return File(store(ctx, pkg), name)
    }

    private fun isPatchedByUs(ctx: Context, pkg: String): Boolean {
        val info = runCatching { ctx.packageManager.getPackageInfo(pkg, PackageManager.GET_SIGNING_CERTIFICATES) }.getOrNull() ?: return false
        val ours = Signer.certificateDigest()
        val md = java.security.MessageDigest.getInstance("SHA-256")
        return info.signingInfo?.apkContentsSigners?.any { md.digest(it.toByteArray()).contentEquals(ours) } == true
    }

    override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor {
        val file = authorizedFile(uri)
        if (!file.isFile) throw java.io.FileNotFoundException(uri.toString())
        return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY)
    }

    /** The game deletes THUMB's copy once it has its own. */
    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?): Int {
        val file = authorizedFile(uri)
        if (!file.delete()) return 0
        runCatching { File(file.parentFile, DELIVERED + file.name).createNewFile() }
        return 1
    }

    override fun getType(uri: Uri) = "application/octet-stream"
    override fun query(uri: Uri, p: Array<out String>?, s: String?, a: Array<out String>?, o: String?): Cursor? = null
    override fun insert(uri: Uri, values: ContentValues?): Uri? = null
    /** The game reports whether its OBB is in place (shown in THUMB's Library). */
    override fun update(uri: Uri, values: ContentValues?, s: String?, a: Array<out String>?): Int {
        val file = authorizedFile(uri)
        val present = values?.getAsString("present") == "1"
        val size = values?.getAsString("size")?.toLongOrNull() ?: 0
        file.parentFile?.mkdirs()
        File(file.parentFile, STATUS + file.name).writeText(if (present) "1 $size" else "0")
        return 1
    }
}
