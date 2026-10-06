package dev.thumb.app.core

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import java.io.File

/**
 * Finds an app's OBB: already in place, or in folders the user allowed THUMB
 * to search (Storage Access Framework tree grants; THUMB can see nothing else).
 */
object ObbFinder {
    data class Candidate(val uri: Uri, val name: String, val size: Long)

    @Suppress("DEPRECATION")
    fun obbDir(pkg: String) = File(android.os.Environment.getExternalStorageDirectory(), "Android/obb/$pkg")

    /** The OBB already sitting in the app's OBB folder, if any. */
    fun installed(pkg: String, versionCode: Long): File? {
        val dir = obbDir(pkg)
        val exact = File(dir, "main.$versionCode.$pkg.obb")
        if (exact.isFile) return exact
        return dir.listFiles()?.firstOrNull { it.isFile && it.name.startsWith("main.") && it.name.endsWith(".obb") }
    }

    /** True if THUMB may search somewhere: full file access, or a chosen folder. */
    fun canSearch(context: Context) =
        android.os.Environment.isExternalStorageManager() || context.contentResolver.persistedUriPermissions.any { it.isReadPermission }

    /** Best match in the places THUMB may search: exact name first, then any .obb naming the package. */
    fun search(context: Context, pkg: String, versionCode: Long): Candidate? {
        val all = context.contentResolver.persistedUriPermissions.filter { it.isReadPermission }.flatMap { list(context, it.uri, depth = 2) } +
            (if (android.os.Environment.isExternalStorageManager()) listCommonFolders() else emptyList())
        val exact = "main.$versionCode.$pkg.obb"
        return all.firstOrNull { it.name == exact }
            ?: all.filter { it.name.endsWith(".obb") && it.name.contains(pkg) }.maxByOrNull { it.size }
    }

    /** With full file access: the usual places people keep downloads. */
    @Suppress("DEPRECATION")
    private fun listCommonFolders(): List<Candidate> {
        val root = android.os.Environment.getExternalStorageDirectory()
        val dirs = listOf("Download", "Downloads", "Documents", "THUMB").map { File(root, it) } + root
        val out = ArrayList<Candidate>()
        fun walk(dir: File, level: Int) {
            for (f in dir.listFiles() ?: return) {
                if (f.isDirectory && level < 2 && !f.name.startsWith(".") && f.name != "Android") walk(f, level + 1)
                else if (f.isFile && f.name.endsWith(".obb", ignoreCase = true)) out += Candidate(Uri.fromFile(f), f.name, f.length())
            }
        }
        for (d in dirs) walk(d, if (d == root) 2 else 0) // root: only its direct files
        return out.distinctBy { it.uri }
    }

    private fun list(context: Context, tree: Uri, depth: Int): List<Candidate> {
        val out = ArrayList<Candidate>()
        fun walk(docId: String, level: Int) {
            val children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId)
            val cols = arrayOf(
                DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE,
            )
            context.contentResolver.query(children, cols, null, null, null)?.use { c ->
                while (c.moveToNext()) {
                    val id = c.getString(0)
                    val name = c.getString(1) ?: continue
                    if (c.getString(2) == DocumentsContract.Document.MIME_TYPE_DIR) {
                        if (level < depth) walk(id, level + 1)
                    } else if (name.endsWith(".obb", ignoreCase = true)) {
                        out += Candidate(DocumentsContract.buildDocumentUriUsingTree(tree, id), name, c.getLong(3))
                    }
                }
            }
        }
        runCatching { walk(DocumentsContract.getTreeDocumentId(tree), 0) }
        return out
    }
}
