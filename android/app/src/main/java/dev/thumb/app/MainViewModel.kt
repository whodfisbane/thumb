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
        /** [confirm]: Android's install confirmation screen, if one is pending. */
        data class Working(val message: String, val confirm: Intent? = null) : State()
        data class Analyzed(val file: File, val bundle: dev.thumb.app.core.Bundle, val info: AppInfo, val report: Doctor.Report) : State()
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
            val analyzed = withContext(Dispatchers.IO) {
                val file = File(context.cacheDir, "input.apk")
                context.contentResolver.openInputStream(uri).use { input ->
                    requireNotNull(input) { "Cannot open the selected file" }
                    file.outputStream().use { input.copyTo(it) }
                }
                raw("copied to ${file.path} (${file.length() / 1024} KB)")
                val bundle = dev.thumb.app.core.Bundle.open(file, File(context.cacheDir, "bundle"))
                if (bundle.apks.size > 1 || bundle.obbs.isNotEmpty())
                    raw("bundle: ${bundle.apks.joinToString { it.name }}; obb: ${bundle.obbs.joinToString { it.name }.ifEmpty { "none" }}")
                val info = readInfo(bundle.base).let { if (bundle.obbs.isNotEmpty()) it.copy(needsObb = true) else it }
                raw("package ${info.packageName} ${info.versionName} (code ${info.versionCode}), targetSdk ${info.targetSdk}, needsObb=${info.needsObb}")
                val report = Doctor.check(file, supported)
                for (lib in report.libs) {
                    raw("doctor: ${lib.name} ${lib.handled}/${lib.total}" + (lib.error?.let { " ERROR $it" } ?: ""))
                    for ((cat, syms) in lib.missing) raw("doctor:   missing $cat: ${syms.joinToString()}")
                }
                State.Analyzed(file, bundle, info, report)
            }
            val parts = analyzed.bundle.apks.size
            step("Checked ${analyzed.info.label}: ${analyzed.report.percent}% compatible" + if (parts > 1) " ($parts-part bundle)" else "")
            _state.value = analyzed
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
                val outDir = File(context.cacheDir, "patched").apply { deleteRecursively(); mkdirs() }
                var libCount = 0
                var target: Pair<Int?, Int?>? = null
                val unsigned = analyzed.bundle.apks.mapIndexed { i, apk ->
                    val out = File(outDir, "$i-unsigned.apk")
                    val result = Patcher(runtime, stub).patch(apk, out) { raw("patch: $it") }
                    libCount += result.libs.size
                    if (result.newTargetSdk != null) target = result.oldTargetSdk to result.newTargetSdk
                    out
                }
                require(libCount > 0) { "No 32-bit (armeabi-v7a) libraries found: THUMB is not needed for this app" }
                step("Added the THUMB translator to $libCount 32-bit libraries")
                target?.let { (old, new) -> step("Updated it for modern Android (target $old → $new)") }
                _state.value = State.Working("Signing…")
                val t0 = System.nanoTime()
                val signed = unsigned.mapIndexed { i, u ->
                    File(outDir, "$i.apk").also { Signer.sign(u, it); u.delete() }
                }
                raw("signed ${signed.size} APK(s) in ${(System.nanoTime() - t0) / 1_000_000} ms")
                step("Signed with this phone's THUMB key")
                signed
            }
            _state.value = State.Working("Installing… confirm in the dialog")
            raw("install session started (${signed.size} APK(s))")
            val outcome = Installer.install(context, signed) { confirm ->
                _state.value = State.Working("Installing… confirm in the dialog", confirm)
            }
            withContext(Dispatchers.IO) { signed.forEach { it.delete() } }
            when (outcome) {
                is Installer.Outcome.Success -> {
                    step("Installed ${info.label}")
                    afterInstall(info, analyzed.bundle)
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

    private suspend fun afterInstall(info: AppInfo, bundle: dev.thumb.app.core.Bundle) {
        if (!info.needsObb) {
            _state.value = State.Ready(info, null)
            return
        }
        // Bundles (.xapk) often carry the OBB: put it in place automatically.
        bundle.obbs.firstOrNull()?.let { obb ->
            importObb(State.ObbNeeded(info), android.net.Uri.fromFile(obb))
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
     * Gets an OBB to the game. Android doesn't let THUMB write other apps' OBB
     * folders, so THUMB stores it privately and grants the game read access;
     * the THUMB runtime inside the game copies it over on first launch.
     */
    fun importObb(s: State.ObbNeeded, uri: Uri) = viewModelScope.launch {
        _state.value = s.copy(status = "Copying the data file…", busy = true)
        try {
            withContext(Dispatchers.IO) {
                val pkg = s.info.packageName
                val dir = dev.thumb.app.core.ObbProvider.store(context, pkg).apply { mkdirs() }
                val target = File(dir, s.info.obbName)
                val tmp = File(dir, "${s.info.obbName}.part")
                val t0 = System.nanoTime()
                context.contentResolver.openInputStream(uri).use { input ->
                    requireNotNull(input) { "Cannot open the selected file" }
                    tmp.outputStream().use { input.copyTo(it, 1 shl 20) }
                }
                if (!tmp.renameTo(target)) throw java.io.IOException("cannot rename to $target")
                raw("obb stored at ${target.path} (${target.length()} bytes in ${(System.nanoTime() - t0) / 1_000_000} ms)")
                val shared = dev.thumb.app.core.ObbProvider.uri(pkg, target.name)
                context.grantUriPermission(pkg, shared, Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
                raw("granted $pkg access to $shared")
            }
            step("Prepared the data file (OBB)")
            _state.value = State.Ready(s.info, "The data file will be moved into ${s.info.label} the first time you open it (takes a few seconds).")
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
