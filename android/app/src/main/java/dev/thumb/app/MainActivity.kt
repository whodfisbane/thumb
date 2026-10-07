package dev.thumb.app

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.provider.Settings
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.lifecycle.viewmodel.compose.viewModel
import dev.thumb.app.MainViewModel.State

private val Mint = Color(0xFF42FFC3)
private val Ink = Color(0xFF000000)
private val Panel = Color(0xFF121614)
private val Muted = Color(0xFF9AA8A2)
private val Warn = Color(0xFFFFC857)
private val Bad = Color(0xFFFF6B6B)

private val ThumbColors = darkColorScheme(
    primary = Mint, onPrimary = Ink, background = Ink, onBackground = Color.White,
    surface = Panel, onSurface = Color.White, secondary = Mint, surfaceContainer = Panel,
)

// Easter egg: Holo, the Android 4.x look (2011-2014), the era of the apps THUMB revives.
private val HoloBlue = Color(0xFF33B5E5)
private val HoloColors = darkColorScheme(
    primary = HoloBlue, onPrimary = Color.White, background = Ink, onBackground = Color.White,
    surface = Color(0xFF282828), onSurface = Color.White, secondary = HoloBlue, surfaceContainer = Color(0xFF282828),
)
private val HoloShapes = androidx.compose.material3.Shapes(
    extraSmall = RoundedCornerShape(1.dp), small = RoundedCornerShape(2.dp), medium = RoundedCornerShape(2.dp),
    large = RoundedCornerShape(2.dp), extraLarge = RoundedCornerShape(2.dp),
)

/** Holo mode on/off, shared by the whole UI. */
private val LocalHolo = androidx.compose.runtime.staticCompositionLocalOf { false }

/** Brand colour for the current theme (mint, or Holo blue). */
@Composable
private fun accent() = if (LocalHolo.current) HoloBlue else Mint

/** The THUMB logo; tinted Holo blue in Holo mode. */
@Composable
private fun ThumbLogo(modifier: Modifier, description: String? = null) = Image(
    painterResource(R.drawable.thumb_logo), description, modifier,
    colorFilter = if (LocalHolo.current) androidx.compose.ui.graphics.ColorFilter.tint(HoloBlue) else null,
)

/**
 * Text with highlighted parts marked by double asterisks: bold white by
 * default, or just coloured (normal weight) when [highlight] is given.
 */
private fun rich(text: String, highlight: Color? = null) = androidx.compose.ui.text.buildAnnotatedString {
    val style = if (highlight != null) androidx.compose.ui.text.SpanStyle(color = highlight)
    else androidx.compose.ui.text.SpanStyle(fontWeight = FontWeight.Bold, color = Color.White)
    for ((i, part) in text.split("**").withIndex()) {
        if (i % 2 == 1) withStyle(style) { append(part) } else append(part)
    }
}

/** Button labels are ALL CAPS in Holo mode, like Android 4.x. */
@Composable
private fun label(text: String) = if (LocalHolo.current) text.uppercase() else text

class MainActivity : ComponentActivity() {
    private val vm by lazy { androidx.lifecycle.ViewModelProvider(this)[MainViewModel::class.java] }

    /** Coming back (e.g. after uninstalling an app): refresh the Library. */
    override fun onResume() {
        super.onResume()
        vm.refreshLibrary()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        val prefs = getSharedPreferences("thumb", MODE_PRIVATE)
        setContent {
            var holo by remember { mutableStateOf(prefs.getBoolean("holo", false)) }
            androidx.compose.runtime.CompositionLocalProvider(LocalHolo provides holo) {
                MaterialTheme(
                    colorScheme = if (holo) HoloColors else ThumbColors,
                    shapes = if (holo) HoloShapes else MaterialTheme.shapes,
                ) {
                    Surface(Modifier.fillMaxSize(), color = Ink) {
                        ThumbScreen(onToggleHolo = {
                            holo = !holo
                            prefs.edit().putBoolean("holo", holo).apply()
                        })
                    }
                }
            }
        }
    }
}

private fun Context.versionName(): String =
    runCatching { packageManager.getPackageInfo(packageName, 0).versionName }.getOrNull() ?: "?"

@Composable
private fun ThumbScreen(onToggleHolo: () -> Unit, vm: MainViewModel = viewModel()) {
    val state by vm.state.collectAsState()
    val steps by vm.steps.collectAsState()
    val console by vm.console.collectAsState()
    var consoleOpen by remember { mutableStateOf(false) }
    var aboutOpen by remember { mutableStateOf(false) }
    val ctx = LocalContext.current
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        if (uri != null) vm.analyze(uri)
    }

    Onboarding()

    val holo = LocalHolo.current
    Column(Modifier.fillMaxSize().safeDrawingPadding()) {
    if (holo) HoloActionBar(onAbout = { aboutOpen = true })
    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(if (holo) 12.dp else 20.dp),
        verticalArrangement = Arrangement.spacedBy(if (holo) 12.dp else 16.dp),
    ) {
        // Back: from any screen except while working (patching/installing).
        val canGoBack = state !is State.Idle && state !is State.Working
        androidx.activity.compose.BackHandler(enabled = canGoBack) { vm.reset() }
        if (!holo && state is State.Idle) Header(onAbout = { aboutOpen = true })
        when (val s = state) {
            is State.Idle -> {
                IdleCard { picker.launch(arrayOf("application/vnd.android.package-archive", "application/octet-stream", "*/*")) }
                val apps by vm.library.collectAsState()
                if (apps.isNotEmpty()) LibraryCard(apps, vm)
            }
            is State.Working -> WorkingCard(s, steps)
            is State.Analyzed -> ReportCard(s, onPatch = { options -> vm.patchAndInstall(s, options) }, onCancel = vm::reset)
            is State.ObbNeeded -> ObbCard(s, vm)
            is State.Ready -> ReadyCard(s, steps, onAgain = vm::reset)
            is State.Failed -> FailedCard(s, onBack = vm::reset)
        }
        if (console.isNotEmpty()) {
            TTextButton(onClick = { consoleOpen = true }) { Text(label(">_ Console"), fontFamily = FontFamily.Monospace, color = Muted) }
        }
        Text(
            "THUMB v${ctx.versionName()} · free software (GPL-3.0)",
            color = Muted, fontSize = 11.sp, modifier = Modifier.clickable { aboutOpen = true },
        )
    }
    }

    if (consoleOpen) ConsoleDialog(console) { consoleOpen = false }
    if (aboutOpen) AboutDialog(onToggleHolo = onToggleHolo) { aboutOpen = false }
}

@Composable
private fun Header(onAbout: () -> Unit) {
    val holo = LocalHolo.current
    Row(verticalAlignment = Alignment.CenterVertically) {
        ThumbLogo(Modifier.size(56.dp).clickable(onClick = onAbout), "About THUMB")
        Spacer(Modifier.width(14.dp))
        Column {
            Text("THUMB", color = accent(), fontSize = 30.sp, fontWeight = if (holo) FontWeight.Light else FontWeight.Black, letterSpacing = 2.sp)
            Text("THUMB Helps Unsupported Mobile Binaries", color = Muted, fontSize = 12.sp)
        }
    }
}

@Composable
private fun PanelCard(content: @Composable () -> Unit) {
    if (LocalHolo.current) {
        // Holo: no cards, flat content between thin dividers.
        Column(Modifier.fillMaxWidth()) {
            androidx.compose.material3.HorizontalDivider(color = Color(0xFF3A3A3A))
            Column(Modifier.padding(vertical = 14.dp, horizontal = 4.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) { content() }
            androidx.compose.material3.HorizontalDivider(color = Color(0xFF3A3A3A))
        }
        return
    }
    Card(Modifier.fillMaxWidth(), colors = CardDefaults.cardColors(containerColor = Panel)) {
        Column(Modifier.padding(18.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) { content() }
    }
}

// ---- Themed buttons: modern THUMB, or flat rectangular Holo buttons ----

@Composable
private fun HoloButton(onClick: () -> Unit, modifier: Modifier, enabled: Boolean, strong: Boolean, content: @Composable androidx.compose.foundation.layout.RowScope.() -> Unit) {
    Surface(
        onClick = onClick, enabled = enabled, modifier = modifier.height(48.dp),
        shape = androidx.compose.ui.graphics.RectangleShape,
        color = if (strong) Color(0xFF3C3C3C) else Color(0xFF2A2A2A),
        border = androidx.compose.foundation.BorderStroke(1.dp, if (strong) HoloBlue else Color(0xFF4A4A4A)),
        contentColor = if (enabled) Color.White else Color(0xFF777777),
    ) {
        Row(Modifier.padding(horizontal = 16.dp), horizontalArrangement = Arrangement.Center, verticalAlignment = Alignment.CenterVertically, content = content)
    }
}

@Composable
private fun TButton(onClick: () -> Unit, modifier: Modifier = Modifier, enabled: Boolean = true, content: @Composable androidx.compose.foundation.layout.RowScope.() -> Unit) =
    if (LocalHolo.current) HoloButton(onClick, modifier, enabled, strong = true, content)
    else Button(onClick = onClick, modifier = modifier, enabled = enabled, content = content)

@Composable
private fun TOutlinedButton(onClick: () -> Unit, modifier: Modifier = Modifier, enabled: Boolean = true, content: @Composable androidx.compose.foundation.layout.RowScope.() -> Unit) =
    if (LocalHolo.current) HoloButton(onClick, modifier, enabled, strong = false, content)
    else OutlinedButton(onClick = onClick, modifier = modifier, enabled = enabled, content = content)

@Composable
private fun TTextButton(onClick: () -> Unit, modifier: Modifier = Modifier, content: @Composable androidx.compose.foundation.layout.RowScope.() -> Unit) =
    TextButton(onClick = onClick, modifier = modifier, shape = if (LocalHolo.current) androidx.compose.ui.graphics.RectangleShape else ButtonDefaults.textShape, content = content)

/** Holo action bar: dark bar, small icon, Roboto Light title, 2dp blue line. */
@Composable
private fun HoloActionBar(onAbout: () -> Unit) {
    Column(Modifier.fillMaxWidth()) {
        Row(
            Modifier.fillMaxWidth().background(Color(0xFF1F1F1F)).height(56.dp).padding(horizontal = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            ThumbLogo(Modifier.size(32.dp).clickable(onClick = onAbout), "About THUMB")
            Spacer(Modifier.width(12.dp))
            Text("THUMB", color = Color.White, fontSize = 20.sp, fontWeight = FontWeight.Light)
        }
        Spacer(Modifier.fillMaxWidth().height(2.dp).background(HoloBlue))
    }
}

@Composable
private fun AppTitle(info: MainViewModel.AppInfo) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        info.icon?.let { Image(it.asImageBitmap(), null, Modifier.size(52.dp)) }
        Spacer(Modifier.width(12.dp))
        Column {
            Text(info.label, fontWeight = FontWeight.Bold, fontSize = 18.sp)
            Text("${info.packageName} · ${info.versionName ?: "?"}", color = Muted, fontSize = 12.sp)
        }
    }
}

@Composable
private fun Steps(steps: List<String>) {
    for (s in steps) Text("✓ $s", color = Muted, fontSize = 13.sp)
}

@Composable
private fun IdleCard(onPick: () -> Unit) = PanelCard {
    // Before picking: a reminder to patch only apps you own (until "Don't show again").
    val ctx = LocalContext.current
    val prefs = remember { ctx.getSharedPreferences("thumb", Context.MODE_PRIVATE) }
    var ask by remember { mutableStateOf(false) }
    if (ask) {
        var dontShow by remember { mutableStateOf(false) }
        AlertDialog(
            onDismissRequest = { ask = false },
            containerColor = MaterialTheme.colorScheme.surfaceContainer,
            title = { Text("Before you add an app") },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(rich("⚠️ **Patch only apps you own**, and **never share patched APKs** or game data.", Warn), fontSize = 14.sp)
                    Text("THUMB contains no code or data from any app or game; you supply your own copy.", color = Muted, fontSize = 13.sp)
                    Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.clickable { dontShow = !dontShow }) {
                        androidx.compose.material3.Checkbox(checked = dontShow, onCheckedChange = { dontShow = it })
                        Text("Don't show again", fontSize = 13.sp)
                    }
                }
            },
            confirmButton = {
                TTextButton(onClick = {
                    if (dontShow) prefs.edit().putBoolean("own_apps_ok", true).apply()
                    ask = false
                    onPick()
                }) { Text(label("Continue")) }
            },
            dismissButton = { TTextButton(onClick = { ask = false }) { Text(label("Cancel")) } },
        )
    }
    Text("Run old 32-bit apps and games on this phone", fontWeight = FontWeight.Bold, fontSize = 18.sp)
    Text(rich("Pick an APK. THUMB **checks** it, adds its **ARM32 translator**, signs it with **this phone's own key** and **installs** it.", accent()), color = Muted)
    TButton(onClick = { if (prefs.getBoolean("own_apps_ok", false)) onPick() else ask = true }, modifier = Modifier.fillMaxWidth()) { Text(label("Add app"), fontWeight = FontWeight.Bold) }
}

/** "Your apps": everything THUMB patched on this phone. Tap one for its actions. */
@Composable
private fun LibraryCard(apps: List<dev.thumb.app.core.Library.Entry>, vm: MainViewModel) = PanelCard {
    val ctx = LocalContext.current
    var open by remember { mutableStateOf<String?>(null) }
    var uninstall by remember { mutableStateOf<dev.thumb.app.core.Library.Entry?>(null) }
    Text("Your apps", fontWeight = FontWeight.Bold, fontSize = 18.sp)
    for (app in apps) {
        val outdated = app.runtime != vm.runtimeId
        val expanded = open == app.packageName
        Row(
            Modifier.fillMaxWidth().clickable { open = if (expanded) null else app.packageName }.padding(vertical = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            app.icon?.let { Image(it.asImageBitmap(), null, Modifier.size(44.dp)) } ?: Spacer(Modifier.size(44.dp))
            Spacer(Modifier.width(12.dp))
            Column(Modifier.weight(1f)) {
                Text(app.label, fontWeight = FontWeight.Bold, fontSize = 16.sp)
                Text("v${app.versionName ?: "?"}", color = Muted, fontSize = 12.sp)
                if (outdated) Text("Update available", color = accent(), fontSize = 12.sp, fontWeight = FontWeight.Bold)
                if (app.needsObb) when (app.obb) {
                    dev.thumb.app.core.Library.Obb.PENDING -> Text("Data file moves in when you open it", color = Muted, fontSize = 12.sp)
                    dev.thumb.app.core.Library.Obb.PRESENT -> Text("✓ Data file", color = Muted, fontSize = 12.sp)
                    dev.thumb.app.core.Library.Obb.MISSING -> Text("⚠️ Data file missing", color = Warn, fontSize = 12.sp, fontWeight = FontWeight.Bold)
                    dev.thumb.app.core.Library.Obb.UNKNOWN -> Unit
                }
            }
            Text(if (expanded) "▴" else "▾", color = Muted)
        }
        if (expanded) {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                ctx.packageManager.getLaunchIntentForPackage(app.packageName)?.let { launch ->
                    TButton(onClick = { ctx.startActivity(launch) }, modifier = Modifier.weight(1f)) { Text(label("Open"), fontWeight = FontWeight.Bold) }
                }
                TOutlinedButton(onClick = { vm.repatch(app) }, modifier = Modifier.weight(1f)) { Text(label(if (outdated) "Update" else "Options")) }
            }
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                if (app.needsObb) TTextButton(onClick = { vm.addObb(app) }) {
                    Text(label(if (app.obb == dev.thumb.app.core.Library.Obb.MISSING) "Add data file" else "Data file"))
                }
                Spacer(Modifier.weight(1f))
                TTextButton(onClick = { uninstall = app }) { Text(label("Uninstall"), color = Bad) }
            }
        }
    }
    uninstall?.let { app ->
        AlertDialog(
            onDismissRequest = { uninstall = null },
            containerColor = MaterialTheme.colorScheme.surfaceContainer,
            title = { Text("Uninstall ${app.label}?") },
            text = { Text("Its saves and settings on this phone are deleted too. To reinstall, add the app again.", color = Muted) },
            confirmButton = {
                TTextButton(onClick = {
                    uninstall = null
                    ctx.startActivity(Intent(Intent.ACTION_DELETE, Uri.parse("package:${app.packageName}")))
                }) { Text(label("Uninstall"), color = Bad) }
            },
            dismissButton = { TTextButton(onClick = { uninstall = null }) { Text(label("Cancel")) } },
        )
    }
}

@Composable
private fun WorkingCard(s: State.Working, steps: List<String>) = PanelCard {
    Steps(steps)
    Row(verticalAlignment = Alignment.CenterVertically) {
        CircularProgressIndicator(Modifier.size(22.dp), color = accent(), strokeWidth = 3.dp)
        Spacer(Modifier.width(12.dp))
        Text(s.message, fontWeight = FontWeight.Bold)
    }
    // If Android's install dialog didn't show up, offer to reopen it after a moment.
    val confirm = s.confirm ?: return@PanelCard
    var late by remember(confirm) { mutableStateOf(false) }
    androidx.compose.runtime.LaunchedEffect(confirm) {
        kotlinx.coroutines.delay(4000)
        late = true
    }
    if (late) {
        val ctx = LocalContext.current
        TTextButton(onClick = { ctx.startActivity(confirm) }, modifier = Modifier.fillMaxWidth()) {
            Text(label("Dialog didn't appear? Tap here"))
        }
    }
}

/** One option: title, description, switch; its warning shows while it's on. */
@Composable
private fun OptionRow(
    title: String, description: String, checked: Boolean, onChange: (Boolean) -> Unit,
    warning: String? = null, enabled: Boolean = true, indent: Boolean = false, info: String? = null,
) {
    Column(Modifier.fillMaxWidth().padding(start = if (indent) 18.dp else 0.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(title, fontWeight = FontWeight.Bold, fontSize = 15.sp, color = if (enabled) Color.White else Muted)
                Text(description, color = Muted, fontSize = 12.sp)
            }
            Spacer(Modifier.width(12.dp))
            androidx.compose.material3.Switch(checked = checked && enabled, onCheckedChange = onChange, enabled = enabled)
        }
        if (warning != null && checked && enabled) {
            // Collapsed by default: "⚠️ Warning ▾", tap to read it.
            var open by remember { mutableStateOf(false) }
            Text(
                if (open) "⚠️ Warning ▴" else "⚠️ Warning ▾", color = Warn, fontSize = 12.sp, fontWeight = FontWeight.Bold,
                modifier = Modifier.clickable { open = !open }.padding(vertical = 2.dp),
            )
            if (open) Text(warning, color = Warn, fontSize = 12.sp)
        }
        if (info != null && checked && enabled) {
            var open by remember { mutableStateOf(false) }
            Text(
                if (open) "ℹ️ Info ▴" else "ℹ️ Info ▾", color = Muted, fontSize = 12.sp, fontWeight = FontWeight.Bold,
                modifier = Modifier.clickable { open = !open }.padding(vertical = 2.dp),
            )
            if (open) Text(info, color = Muted, fontSize = 12.sp)
        }
    }
}

/** A short centred line that separates sections (not edge to edge). */
@Composable
private fun SectionDivider() {
    androidx.compose.foundation.layout.Box(Modifier.fillMaxWidth().padding(vertical = 10.dp), contentAlignment = Alignment.Center) {
        androidx.compose.material3.HorizontalDivider(Modifier.fillMaxWidth(0.4f), thickness = 1.dp, color = Muted.copy(alpha = 0.5f))
    }
}

/** Build option: FPS mode, Compat (60) or Default (the screen decides). */
@Composable
private fun FpsRow(mode: String, onChange: (String) -> Unit) {
    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Text("FPS", fontWeight = FontWeight.Bold, fontSize = 15.sp)
        Text("Compat: 60 FPS, what old games were built for · Default: the screen decides", color = Muted, fontSize = 12.sp)
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            for ((id, name) in listOf("compat" to "Compat (60 FPS)", "default" to "Default")) {
                androidx.compose.material3.FilterChip(selected = mode == id, onClick = { onChange(id) }, label = { Text(label(name)) })
            }
        }
        if (mode == "default") {
            var open by remember { mutableStateOf(false) }
            Text(
                if (open) "⚠️ Warning ▴" else "⚠️ Warning ▾", color = Warn, fontSize = 12.sp, fontWeight = FontWeight.Bold,
                modifier = Modifier.clickable { open = !open }.padding(vertical = 2.dp),
            )
            if (open) Text(dev.thumb.app.core.PatchOptions.WARN_FPS_HIGH, color = Warn, fontSize = 12.sp)
        }
    }
}

@Composable
private fun OptionsSection(options: dev.thumb.app.core.PatchOptions, onChange: (dev.thumb.app.core.PatchOptions) -> Unit) {
    val P = dev.thumb.app.core.PatchOptions

    SectionDivider()
    Text("Build options", fontWeight = FontWeight.Bold, fontSize = 16.sp, color = accent())
    OptionRow("Block ads", "Stops calls to known ad SDKs from the start", options.adblock, { onChange(options.copy(adblock = it)) }, P.WARN_ADBLOCK)
    FpsRow(options.fpsMode) { onChange(options.copy(fpsMode = it)) }
    OptionRow("Sandbox", "Remove access to contacts, location, camera…", options.sandbox, { onChange(options.copy(sandbox = it)) },
        info = P.WARN_SANDBOX)
    OptionRow("Block internet", "The app can't go online at all", options.blockInternet, { onChange(options.copy(blockInternet = it)) },
        enabled = options.sandbox, indent = true, info = P.WARN_BLOCK_INTERNET)

    SectionDivider()
    Text("THUMB overlay", fontWeight = FontWeight.Bold, fontSize = 16.sp, color = accent())
    OptionRow("In-game menu", "FPS counter, speed, FPS unlock, ad-block and more, while playing", options.overlay,
        { onChange(options.copy(overlay = it)) }, info = P.INFO_OVERLAY)
}

@Composable
private fun ReportCard(s: State.Analyzed, onPatch: (dev.thumb.app.core.PatchOptions) -> Unit, onCancel: () -> Unit) = PanelCard {
    var options by remember(s.info.packageName, s.update) { mutableStateOf(s.initial) }
    val r = s.report
    AppTitle(s.info)
    if (!r.needsThumb) {
        Text(r.verdict, color = Muted)
        TOutlinedButton(onClick = onCancel, modifier = Modifier.fillMaxWidth()) { Text(label("Back")) }
        return@PanelCard
    }
    val color = when {
        r.percent >= 90 -> accent()
        r.percent >= 75 -> Warn
        else -> Bad
    }
    Text("THUMB Doctor: ${r.percent}%", color = color, fontWeight = FontWeight.Black, fontSize = 22.sp)
    LinearProgressIndicator(progress = { r.percent / 100f }, modifier = Modifier.fillMaxWidth(), color = color, trackColor = Ink)
    Text(rich("**${r.verdict}** · ${r.handled}/${r.total} system calls handled"), color = Muted, fontSize = 13.sp)
    val missing = r.libs.flatMap { it.missing.entries }.groupBy({ it.key }, { it.value.size }).mapValues { it.value.sum() }
    if (missing.isNotEmpty()) {
        Text(rich("**Not supported yet:** " + missing.entries.sortedByDescending { it.value }.joinToString { "${it.key} (${it.value})" }), color = Muted, fontSize = 13.sp)
    }
    if (s.info.needsObb) Text(rich("📦 Uses an **extra data file (OBB)**: you can add it right after installing."), color = Muted, fontSize = 13.sp)
    if (s.info.conflictingInstall) {
        Text(rich("⚠️ ${s.info.label} is already installed with a **different signature**. Uninstall it first (**its data will be lost**)."), color = Warn, fontSize = 13.sp)
        val ctx = LocalContext.current
        TOutlinedButton(onClick = {
            ctx.startActivity(Intent(Intent.ACTION_DELETE, Uri.parse("package:${s.info.packageName}")))
        }, modifier = Modifier.fillMaxWidth()) { Text("Uninstall existing app") }
    }
    OptionsSection(options) { options = it }
    TButton(onClick = { onPatch(options) }, modifier = Modifier.fillMaxWidth(), enabled = !s.info.conflictingInstall) {
        Text(if (s.update) "Update & install" else "Patch & install", fontWeight = FontWeight.Bold)
    }
    TOutlinedButton(onClick = onCancel, modifier = Modifier.fillMaxWidth()) { Text(label("Cancel")) }
}

@Composable
private fun ObbCard(s: State.ObbNeeded, vm: MainViewModel) = PanelCard {
    val folderPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri: Uri? ->
        if (uri != null) vm.addSearchFolder(s, uri)
    }
    val filePicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        if (uri != null) vm.importObb(s, uri)
    }
    val ctx = LocalContext.current
    var chooser by remember { mutableStateOf(false) }
    val fullAccess = rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) { vm.retrySearch(s) }
    if (chooser) SearchAccessDialog(
        onFolder = { chooser = false; folderPicker.launch(null) },
        onFullAccess = { chooser = false; fullAccess.launch(fullAccessIntent(ctx)) },
        onDismiss = { chooser = false },
    )
    AppTitle(s.info)
    if (s.fromLibrary) {
        Text("📦 Data file (OBB)", fontWeight = FontWeight.Bold, fontSize = 17.sp)
        Text(
            rich("Only needed if ${s.info.label} is **missing its data file** or you want to **replace it**. " +
                "If the app already works, just go back. It's usually named **${s.info.obbName}**."),
            color = Muted, fontSize = 13.sp,
        )
    } else {
        Text("📦 ${s.info.label} needs its data file (OBB)", fontWeight = FontWeight.Bold, fontSize = 17.sp)
        Text(
            rich("If you have it, THUMB will **look for it** in a folder you choose. If THUMB can't find it, **pick the file yourself**. " +
                "It's usually named **${s.info.obbName}**."),
            color = Muted, fontSize = 13.sp,
        )
    }
    s.status?.let { Text(it, color = if (s.busy) accent() else Warn, fontSize = 13.sp) }
    if (s.busy) {
        LinearProgressIndicator(Modifier.fillMaxWidth(), color = accent(), trackColor = Ink)
        return@PanelCard
    }
    TButton(onClick = { if (!vm.autoSearchObb(s)) chooser = true }, modifier = Modifier.fillMaxWidth()) {
        Text(label("Find automatically"), fontWeight = FontWeight.Bold)
    }
    TOutlinedButton(onClick = { filePicker.launch(arrayOf("*/*")) }, modifier = Modifier.fillMaxWidth()) { Text(label("Select manually")) }
    if (s.fromLibrary) TTextButton(onClick = { vm.reset() }, modifier = Modifier.fillMaxWidth()) { Text(label("Cancel"), color = Muted) }
    else TTextButton(onClick = { vm.skipObb(s) }, modifier = Modifier.fillMaxWidth()) { Text(label("Skip"), color = Muted) }
}

@Composable
private fun ReadyCard(s: State.Ready, steps: List<String>, onAgain: () -> Unit) = PanelCard {
    val ctx = LocalContext.current
    AppTitle(s.info)
    Text("✅ ${s.info.label} is ready", color = accent(), fontWeight = FontWeight.Black, fontSize = 20.sp)
    Steps(steps)
    s.obbNote?.let { Text(it, color = if (it.startsWith("No data")) Warn else Muted, fontSize = 13.sp) }
    // Side by side: start the app right away, or go patch another one.
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
        ctx.packageManager.getLaunchIntentForPackage(s.info.packageName)?.let { launch ->
            TButton(onClick = { ctx.startActivity(launch) }, modifier = Modifier.weight(1f)) { Text(label("Start app"), fontWeight = FontWeight.Bold) }
        }
        TOutlinedButton(onClick = onAgain, modifier = Modifier.weight(1f)) { Text(label("Add another app")) }
    }
}

@Composable
private fun FailedCard(s: State.Failed, onBack: () -> Unit) = PanelCard {
    Text("Something went wrong", color = Bad, fontWeight = FontWeight.Bold, fontSize = 18.sp)
    Text(s.message, fontSize = 13.sp)
    Text("Open the console below for the full log.", color = Muted, fontSize = 12.sp)
    TOutlinedButton(onClick = onBack, modifier = Modifier.fillMaxWidth()) { Text(label("Back")) }
}

/**
 * How THUMB may look for game data: a chosen folder (private, but Android
 * won't let apps pick Download itself) or full file access (simplest).
 */
@Composable
private fun SearchAccessDialog(onFolder: () -> Unit, onFullAccess: () -> Unit, onDismiss: () -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = MaterialTheme.colorScheme.surfaceContainer,
        title = { Text("How should THUMB find game data?", fontWeight = if (LocalHolo.current) FontWeight.Light else FontWeight.Bold, color = if (LocalHolo.current) HoloBlue else Color.Unspecified) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text("Choose a folder", color = accent(), fontWeight = FontWeight.Bold)
                Text(rich("THUMB can **only see that folder**. Android doesn't allow picking Download itself, so use a folder inside it (e.g. **Download/THUMB**)."), color = Muted, fontSize = 13.sp)
                Text("Allow full file access", color = accent(), fontWeight = FontWeight.Bold)
                Text(rich("THUMB searches **Download** and similar folders by itself. THUMB has **no internet permission**, so nothing it reads can leave your phone."), color = Muted, fontSize = 13.sp)
            }
        },
        confirmButton = {
            Column(horizontalAlignment = Alignment.End) {
                TTextButton(onClick = onFolder) { Text(label("Choose a folder")) }
                TTextButton(onClick = onFullAccess) { Text(label("Allow full file access")) }
                TTextButton(onClick = onDismiss) { Text(label("Not now"), color = Muted) }
            }
        },
    )
}

private fun fullAccessIntent(ctx: Context) =
    Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:${ctx.packageName}"))

/** Full-screen raw log for nerds and bug reports. */
@Composable
private fun ConsoleDialog(lines: List<String>, onClose: () -> Unit) {
    val ctx = LocalContext.current
    val holo = LocalHolo.current
    // Holo: a plain grey-on-black terminal under a Holo bar; otherwise a mint terminal.
    val textColor = if (holo) Color(0xFFD0D0D0) else Mint
    val panel = if (holo) Ink else Color(0xFF07100C)
    val shape = if (holo) androidx.compose.ui.graphics.RectangleShape else RoundedCornerShape(8.dp)
    Dialog(onDismissRequest = onClose, properties = DialogProperties(usePlatformDefaultWidth = false)) {
        Surface(Modifier.fillMaxSize(), color = Ink) {
            Column(Modifier.fillMaxSize().safeDrawingPadding()) {
                Row(
                    Modifier.fillMaxWidth().background(if (holo) Color(0xFF1F1F1F) else Ink).padding(horizontal = 12.dp, vertical = 4.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        if (holo) "Console" else ">_ THUMB console", color = if (holo) Color.White else Mint,
                        fontFamily = if (holo) FontFamily.Default else FontFamily.Monospace,
                        fontWeight = if (holo) FontWeight.Light else FontWeight.Bold,
                        fontSize = if (holo) 20.sp else 14.sp, modifier = Modifier.weight(1f),
                    )
                    TTextButton(onClick = {
                        val cm = ctx.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                        cm.setPrimaryClip(ClipData.newPlainText("THUMB log", lines.joinToString("\n")))
                    }) { Text(label("Copy")) }
                    TTextButton(onClick = onClose) { Text(label("Close")) }
                }
                if (holo) Spacer(Modifier.fillMaxWidth().height(2.dp).background(HoloBlue))
                Column(
                    Modifier.fillMaxSize().padding(if (holo) 0.dp else 12.dp).background(panel, shape).padding(10.dp)
                        .verticalScroll(rememberScrollState()),
                ) {
                    for (l in lines) Text(l, color = textColor, fontFamily = FontFamily.Monospace, fontSize = 11.sp)
                }
            }
        }
    }
}

@Composable
private fun AboutDialog(onToggleHolo: () -> Unit, onClose: () -> Unit) {
    val ctx = LocalContext.current
    val holo = LocalHolo.current
    // Easter egg, like tapping "Build number" in Android's About phone.
    var taps by remember { mutableStateOf(0) }
    var lastTap by remember { mutableStateOf(0L) }
    var toast by remember { mutableStateOf<android.widget.Toast?>(null) }
    fun say(msg: String) {
        toast?.cancel()
        toast = android.widget.Toast.makeText(ctx, msg, android.widget.Toast.LENGTH_SHORT).also { it.show() }
    }
    fun tap() {
        val now = System.currentTimeMillis()
        taps = if (now - lastTap > 1500) 1 else taps + 1
        lastTap = now
        val left = 7 - taps
        when {
            left <= 0 -> {
                taps = 0
                onToggleHolo()
                say(if (holo) "Back to THUMB mint 👍" else "🎉 Holo mode! Welcome back to 2013")
            }
            taps >= 2 -> say("You are $left ${if (left == 1) "tap" else "taps"} away from ${if (holo) "the future" else "a blast from the past"}")
        }
    }
    AlertDialog(
        onDismissRequest = onClose,
        containerColor = MaterialTheme.colorScheme.surfaceContainer,
        icon = {
            ThumbLogo(Modifier.size(64.dp).clickable(interactionSource = remember { androidx.compose.foundation.interaction.MutableInteractionSource() }, indication = null) { tap() })
        },
        title = { Text("THUMB v${ctx.versionName()}", fontWeight = if (holo) FontWeight.Light else FontWeight.Bold, color = if (holo) HoloBlue else Color.Unspecified) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("THUMB Helps Unsupported Mobile Binaries: runs old 32-bit Android apps and games on 64-bit-only phones.", color = Color.White, fontSize = 14.sp)
                Text(rich("**Free software** under the **GNU GPL v3.0**. You can use, study, share and change it."), color = Muted, fontSize = 13.sp)
                Text("Built with Claude Opus 5.5 (Anthropic) as a pair programmer.", color = Muted, fontSize = 13.sp)
                Text("Source code: GitHub (link coming soon)", color = Muted, fontSize = 13.sp)
                Text("Includes: dynarmic (0BSD), TLSF (BSD), libffi (MIT), apksig and jni.h (Apache-2.0), AndroidX/Compose (Apache-2.0).", color = Muted, fontSize = 12.sp)
                Text(rich("THUMB contains no code or data from any app or game. **Patch only apps you own** and **never share patched APKs**."), color = Muted, fontSize = 12.sp)
            }
        },
        confirmButton = { TTextButton(onClick = onClose) { Text(label("Close")) } },
    )
}

/**
 * First launch: explain what THUMB needs, then ask for it in order:
 * install permission (required), a folder to search for OBBs (recommended).
 */
@Composable
private fun Onboarding() {
    val ctx = LocalContext.current
    val prefs = remember { ctx.getSharedPreferences("thumb", Context.MODE_PRIVATE) }
    var show by remember { mutableStateOf(!prefs.getBoolean("onboarded", false)) }
    var chooser by remember { mutableStateOf(false) }
    val folderPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri: Uri? ->
        if (uri != null) ctx.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
    }
    val fullAccess = rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) {}
    val installSettings = rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) { chooser = true }
    if (chooser) SearchAccessDialog(
        onFolder = { chooser = false; folderPicker.launch(null) },
        onFullAccess = { chooser = false; fullAccess.launch(fullAccessIntent(ctx)) },
        onDismiss = { chooser = false },
    )
    if (!show) return
    AlertDialog(
        onDismissRequest = {},
        containerColor = MaterialTheme.colorScheme.surfaceContainer,
        title = { Text("Welcome to THUMB 👍", fontWeight = if (LocalHolo.current) FontWeight.Light else FontWeight.Bold, color = if (LocalHolo.current) HoloBlue else Color.Unspecified) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text("THUMB needs very little:", color = Color.White)
                Text("Required", color = accent(), fontWeight = FontWeight.Bold)
                Text(rich("• **Install unknown apps**: to install the apps THUMB patches for you."), color = Muted, fontSize = 14.sp)
                Text("Recommended", color = accent(), fontWeight = FontWeight.Bold)
                Text(rich("• **Access to find game data (OBB files)**: one folder you choose, or full file access."), color = Muted, fontSize = 14.sp)
                Text(rich("THUMB has **no internet permission**: nothing leaves your phone."), color = Muted, fontSize = 12.sp)
            }
        },
        confirmButton = {
            TButton(onClick = {
                prefs.edit().putBoolean("onboarded", true).apply()
                show = false
                if (!ctx.packageManager.canRequestPackageInstalls()) {
                    installSettings.launch(Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:${ctx.packageName}")))
                } else {
                    chooser = true
                }
            }) { Text(label("OK")) }
        },
        dismissButton = {
            TTextButton(onClick = {
                prefs.edit().putBoolean("onboarded", true).apply()
                show = false
            }) { Text(label("Later"), color = Muted) }
        },
    )
}
