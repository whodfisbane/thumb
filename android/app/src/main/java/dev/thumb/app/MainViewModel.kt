package dev.thumb.app

import android.app.Application
import android.content.Intent
import android.content.pm.PackageInfo
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.drawable.Drawable
import android.net.Uri
import androidx.core.graphics.drawable.toBitmap
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import dev.thumb.app.core.Doctor
import dev.thumb.app.core.Installer
import dev.thumb.app.core.ObbFinder
import dev.thumb.app.core.Patcher
import dev.thumb.app.core.Signer
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.zip.ZipFile
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class MainViewModel(app: Application) : AndroidViewModel(app) {
    data class AppInfo(
        val label: String,
        val packageName: String,
        val versionName: String?,
        val versionCode: Long,
        val targetSdk: Int,
        val icon: Bitmap?,
        val needsObb: Boolean,
        /** An app with this package is installed with a different signature. */
        val conflictingInstall: Boolean,
    ) {
        val obbName get() = "main.$versionCode.$packageName.obb"
    }

    sealed class State {
        data object Idle : State()
        data class Working(val message: String) : State()
        data class Analyzed(val file: File, val info: AppInfo, val report: Doctor.Report) : State()
        /** Installed, but the game's OBB data file isn't in place yet. */
        data class ObbNeeded(val info: AppInfo, val status: String? = null, val busy: Boolean = false) : State()
        data class Ready(val info: AppInfo, val obbNote: String?) : State()
        data class Failed(val message: String) : State()
    }

    private val _state = MutableStateFlow<State>(State.Idle)
    val state: StateFlow<State> = _state

    /** Friendly summary of what THUMB did (shown in the card). */
    private val _steps = MutableStateFlow<List<String>>(emptyList())
    val steps: StateFlow<List<String>> = _steps

    /** Raw, timestamped log of everything (the console). */
    private val _console = MutableStateFlow<List<String>>(emptyList())
    val console: StateFlow<List<String>> = _console

    private val context get() = getApplication<Application>()
    private val time = SimpleDateFormat("HH:mm:ss.SSS", Locale.ROOT)

    private val supported: Set<String> by lazy {
        context.assets.open("runtime/supported.txt").bufferedReader().readLines().filter { it.isNotBlank() }.toSet()
    }

    private fun raw(line: String) {
        _console.value = _console.value + "[${time.format(Date())}] $line"
    }

    private fun step(friendly: String) {
        _steps.value = _steps.value + friendly
        raw("== $friendly")
    }

    fun reset() {
        _state.value = State.Idle
        _steps.value = emptyList()
        raw("---- new session ----")
    }

    fun analyze(uri: Uri) = viewModelScope.launch {
        _steps.value = emptyList()
        _state.value = State.Working("Reading app…")
        raw("analyze $uri")
        try {
            val (file, info, report) = withContext(Dispatchers.IO) {
                val file = File(context.cacheDir, "input.apk")
                context.contentResolver.openInputStream(uri).use { input ->
                    requireNotNull(input) { "Cannot open the selected file" }
                    file.outputStream().use { input.copyTo(it) }
                }
                raw("copied to ${file.path} (${file.length() / 1024} KB)")
                val info = readInfo(file)
                raw("package ${info.packageName} ${info.versionName} (code ${info.versionCode}), targetSdk ${info.targetSdk}, needsObb=${info.needsObb}")
                val report = Doctor.check(file, supported)
                for (lib in report.libs) {
                    raw("doctor: ${lib.name} ${lib.handled}/${lib.total}" + (lib.error?.let { " ERROR $it" } ?: ""))
                    for ((cat, syms) in lib.missing) raw("doctor:   missing $cat: ${syms.joinToString()}")
                }
                Triple(file, info, report)
            }
            step("Checked ${info.label}: ${report.percent}% compatible")
            _state.value = State.Analyzed(file, info, report)
        } catch (e: Exception) {
            fail(e)
        }
    }

    fun patchAndInstall(analyzed: State.Analyzed) = viewModelScope.launch {
        val info = analyzed.info
        try {
            val signed = withContext(Dispatchers.IO) {
                _state.value = State.Working("Adding the translator…")
                val runtime = context.assets.open("runtime/libthumb.so").readBytes()
                val stub = context.assets.open("runtime/libthumb_stub.so").readBytes()
                val unsigned = File(context.cacheDir, "patched-unsigned.apk")
                val result = Patcher(runtime, stub).patch(analyzed.file, unsigned) { raw("patch: $it") }
                step("Added the THUMB translator to ${result.libs.size} 32-bit libraries")
                if (result.newTargetSdk != null) step("Updated it for modern Android (target ${result.oldTargetSdk} → ${result.newTargetSdk})")
                _state.value = State.Working("Signing…")
                val signed = File(context.cacheDir, "patched.apk")
                val t0 = System.nanoTime()
                Signer.sign(unsigned, signed)
                raw("signed in ${(System.nanoTime() - t0) / 1_000_000} ms -> ${signed.length() / 1024} KB")
                unsigned.delete()
                step("Signed with this phone's THUMB key")
                signed
            }
            _state.value = State.Working("Installing… confirm in the dialog")
            raw("install session started")
            val outcome = Installer.install(context, signed)
            withContext(Dispatchers.IO) { signed.delete() }
            when (outcome) {
                is Installer.Outcome.Success -> {
                    step("Installed ${info.label}")
                    afterInstall(info)
                }
                is Installer.Outcome.Failure -> {
                    raw("install failed: status ${outcome.status} ${outcome.message}")
                    _state.value = State.Failed("Install failed: ${outcome.message}")
                }
            }
        } catch (e: Exception) {
            fail(e)
        }
    }

    private suspend fun afterInstall(info: AppInfo) {
        if (!info.needsObb) {
            _state.value = State.Ready(info, null)
            return
        }
        val present = withContext(Dispatchers.IO) { ObbFinder.installed(info.packageName, info.versionCode) }
        if (present != null) {
            raw("obb already in place: ${present.path}")
            _state.value = State.Ready(info, "Data file (OBB) already in place")
        } else {
            _state.value = State.ObbNeeded(info)
        }
    }

    /** Searches where THUMB may look; returns false if it may look nowhere yet. */
    fun autoSearchObb(s: State.ObbNeeded): Boolean {
        if (!ObbFinder.canSearch(context)) return false
        searchAndImport(s)
        return true
    }

    /** Called when the user comes back from granting full file access. */
    fun retrySearch(s: State.ObbNeeded) {
        if (ObbFinder.canSearch(context)) searchAndImport(s)
    }

    /** A folder the user allowed THUMB to search (read-only grant, remembered). */
    fun addSearchFolder(s: State.ObbNeeded, tree: Uri) {
        context.contentResolver.takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        raw("search folder added: $tree")
        searchAndImport(s)
    }

    private fun searchAndImport(s: State.ObbNeeded) = viewModelScope.launch {
        _state.value = s.copy(status = "Searching…", busy = true)
        val found = withContext(Dispatchers.IO) { ObbFinder.search(context, s.info.packageName, s.info.versionCode) }
        if (found == null) {
            raw("obb search: nothing matching ${s.info.obbName}")
            _state.value = s.copy(status = "Couldn't find it in your search folder. Pick the file manually, or download it there and search again.", busy = false)
        } else {
            raw("obb search: found ${found.name} (${found.size} bytes) at ${found.uri}")
            importObb(s, found.uri)
        }
    }

    /**
     * Copies an OBB into the app's Android/obb folder under the name Android
     * expects. Apps allowed to install packages (like THUMB) may write other
     * apps' OBB folders, the way app stores deliver expansion files.
     */
    fun importObb(s: State.ObbNeeded, uri: Uri) = viewModelScope.launch {
        _state.value = s.copy(status = "Copying the data file…", busy = true)
        try {
            val target = withContext(Dispatchers.IO) {
                val dir = ObbFinder.obbDir(s.info.packageName)
                if (!dir.exists() && !dir.mkdirs()) throw java.io.IOException("cannot create $dir")
                val target = File(dir, s.info.obbName)
                val tmp = File(dir, "${s.info.obbName}.part")
                val t0 = System.nanoTime()
                context.contentResolver.openInputStream(uri).use { input ->
                    requireNotNull(input) { "Cannot open the selected file" }
                    tmp.outputStream().use { input.copyTo(it, 1 shl 20) }
                }
                if (!tmp.renameTo(target)) throw java.io.IOException("cannot rename to $target")
                raw("obb copied to ${target.path} (${target.length()} bytes in ${(System.nanoTime() - t0) / 1_000_000} ms)")
                target
            }
            step("Added the data file (OBB)")
            _state.value = State.Ready(s.info, "Data file (OBB) added: ${target.name}")
        } catch (e: Exception) {
            android.util.Log.e("THUMB", "OBB import failed", e)
            raw("obb import failed: ${describe(e)}")
            _state.value = s.copy(status = "Couldn't copy the data file: ${e.message}", busy = false)
        }
    }

    fun skipObb(s: State.ObbNeeded) {
        raw("obb skipped by user")
        _state.value = State.Ready(s.info, "No data file added: the app may not start without it")
    }

    private fun fail(e: Exception) {
        android.util.Log.e("THUMB", "failed", e)
        raw("ERROR ${describe(e)}")
        raw(android.util.Log.getStackTraceString(e))
        _state.value = State.Failed(describe(e))
    }

    /** Message plus the chain of causes, so errors are diagnosable from a screenshot. */
    private fun describe(e: Throwable): String = generateSequence(e) { it.cause }.take(4)
        .joinToString("\n→ ") { "${it.javaClass.simpleName}: ${it.message}" }

    private fun readInfo(file: File): AppInfo {
        val pm = context.packageManager
        val pi: PackageInfo = pm.getPackageArchiveInfo(file.path, 0)
            ?: throw IllegalArgumentException("Not a valid APK (bundles like .xapk/.apks come in a later version)")
        val ai = pi.applicationInfo!!.apply {
            sourceDir = file.path
            publicSourceDir = file.path
        }
        val icon: Drawable? = runCatching { ai.loadIcon(pm) }.getOrNull()
        val needsObb = ZipFile(file).use { zip ->
            zip.entries().asSequence().any { it.name.endsWith(".dex") && zip.getInputStream(it).readBytes().containsAscii("getObbDir") }
        }
        return AppInfo(
            label = ai.loadLabel(pm).toString(),
            packageName = pi.packageName,
            versionName = pi.versionName,
            versionCode = pi.longVersionCode,
            targetSdk = ai.targetSdkVersion,
            icon = icon?.toBitmap(128, 128),
            needsObb = needsObb,
            conflictingInstall = hasConflictingInstall(pi.packageName),
        )
    }

    /** True if [pkg] is installed but not signed with our key (it must be uninstalled first). */
    private fun hasConflictingInstall(pkg: String): Boolean {
        val installed = try {
            context.packageManager.getPackageInfo(pkg, PackageManager.GET_SIGNING_CERTIFICATES)
        } catch (_: PackageManager.NameNotFoundException) {
            return false
        }
        val ours = Signer.certificateDigest()
        val signers = installed.signingInfo?.apkContentsSigners ?: return true
        val md = java.security.MessageDigest.getInstance("SHA-256")
        return signers.none { md.digest(it.toByteArray()).contentEquals(ours) }
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
