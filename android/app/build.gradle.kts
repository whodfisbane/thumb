plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "dev.thumb.app"
    compileSdk = 37

    defaultConfig {
        applicationId = "dev.thumb.app"
        minSdk = 29
        targetSdk = 37
        versionCode = 1
        versionName = "0.1.0"
        ndk { abiFilters += "arm64-v8a" }
    }

    // Release signing: keystore path and passwords come from the environment
    // (never committed). Without them, the release APK is built unsigned.
    signingConfigs {
        System.getenv("THUMB_RELEASE_KEYSTORE")?.let { path ->
            create("release") {
                storeFile = file(path)
                storePassword = System.getenv("THUMB_RELEASE_STORE_PASSWORD")
                keyAlias = System.getenv("THUMB_RELEASE_KEY_ALIAS") ?: "thumb-release"
                keyPassword = System.getenv("THUMB_RELEASE_KEY_PASSWORD") ?: System.getenv("THUMB_RELEASE_STORE_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            signingConfigs.findByName("release")?.let { signingConfig = it }
        }
    }

    buildFeatures { compose = true }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        // The runtime ships as an asset (it is copied into patched APKs),
        // so keep it uncompressed and untouched.
        resources.excludes += "META-INF/*.version"
    }
    androidResources { noCompress += listOf("so") }
}

dependencies {
    val composeBom = platform("androidx.compose:compose-bom:2026.09.00")
    implementation(composeBom)
    implementation("androidx.activity:activity-compose:1.13.0")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("com.android.tools.build:apksig:9.4.1")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.11.0")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.11.0")
}

// ---- THUMB runtime: bundled as assets, copied into every patched APK ----
// Built by tools/build-android.sh; Doctor's list of supported functions comes
// from the Linux harness, so the app always matches the runtime it ships.
val repoRoot = rootDir.parentFile
val ndkStrip = File(System.getenv("NDK") ?: "/home/powmy/android-sdk/android-ndk-r30",
    "toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip")
val runtimeAssets = layout.projectDirectory.dir("src/main/assets/runtime")

val bundleThumbRuntime by tasks.registering {
    description = "Strips the THUMB runtime and stub into the app's assets"
    val buildDir = File(repoRoot, "build-android")
    val harness = File(repoRoot, "build/harness")
    inputs.files(File(buildDir, "libthumb.so"), File(buildDir, "libthumb_stub.so"), harness)
    outputs.dir(runtimeAssets)
    doLast {
        val out = runtimeAssets.asFile.apply { mkdirs() }
        for ((src, dst) in listOf("libthumb.so" to "libthumb.so", "libthumb_stub.so" to "libthumb_stub.so")) {
            val input = File(buildDir, src)
            check(input.exists()) { "missing $input: run tools/build-android.sh first" }
            val proc = ProcessBuilder(ndkStrip.path, "--strip-unneeded", "-o", File(out, dst).path, input.path)
                .inheritIO().start()
            check(proc.waitFor() == 0) { "llvm-strip failed for $src" }
        }
        check(harness.exists()) { "missing $harness: build the Linux harness first" }
        val list = ProcessBuilder(harness.path, "--list-thunks").redirectErrorStream(false).start()
        File(out, "supported.txt").writeText(list.inputStream.bufferedReader().readText())
        check(list.waitFor() == 0) { "harness --list-thunks failed" }
    }
}
// The in-game menu (android/overlay) is compiled to a standalone dex that the
// patcher adds to apps. Plain javac + d8 against android.jar: it must not
// depend on anything (the patched app ships its own libraries).
val sdkDir = File(System.getenv("ANDROID_SDK_ROOT") ?: "/home/powmy/android-sdk")
val buildOverlayDex by tasks.registering {
    description = "Compiles the THUMB overlay to assets/runtime/overlay.dex"
    val src = File(rootDir, "overlay/src")
    inputs.dir(src)
    outputs.file(runtimeAssets.file("overlay.dex"))
    doLast {
        val androidJar = File(sdkDir, "platforms/android-37.0/android.jar")
        val d8 = sdkDir.resolve("build-tools").listFiles()!!.sortedDescending().map { File(it, "d8") }.first { it.exists() }
        val classes = layout.buildDirectory.dir("overlay/classes").get().asFile.apply { deleteRecursively(); mkdirs() }
        val dexOut = layout.buildDirectory.dir("overlay/dex").get().asFile.apply { deleteRecursively(); mkdirs() }
        val sources = src.walkTopDown().filter { it.extension == "java" }.map { it.path }.toList()
        val javac = File(System.getProperty("java.home"), "bin/javac").path
        fun run(vararg cmd: String) {
            val p = ProcessBuilder(*cmd).inheritIO().start()
            check(p.waitFor() == 0) { "failed: ${cmd.first()}" }
        }
        run(javac, "--release", "8", "-nowarn", "-cp", androidJar.path,
            "-d", classes.path, *sources.toTypedArray())
        val classFiles = classes.walkTopDown().filter { it.extension == "class" }.map { it.path }.toList()
        run(d8.path, "--min-api", "21", "--release", "--lib", androidJar.path, "--output", dexOut.path, *classFiles.toTypedArray())
        File(dexOut, "classes.dex").copyTo(runtimeAssets.file("overlay.dex").asFile, overwrite = true)
    }
}
bundleThumbRuntime { finalizedBy(buildOverlayDex) }
tasks.named("preBuild") { dependsOn(bundleThumbRuntime, buildOverlayDex) }

// A release must never ship a developer runtime (tools/build-android.sh --dev),
// which has the OBB-over-adb shortcut and debug sampling compiled in.
val checkReleaseRuntime by tasks.registering {
    dependsOn(bundleThumbRuntime)
    doLast {
        val lib = runtimeAssets.file("libthumb.so").asFile
        val marker = "DEV BUILD".toByteArray()
        val bytes = lib.readBytes()
        val dev = (0..bytes.size - marker.size).any { i -> marker.indices.all { bytes[i + it] == marker[it] } }
        check(!dev) { "libthumb.so is a --dev build: rebuild it with tools/build-android.sh (no --dev) before a release" }
    }
}
afterEvaluate { tasks.named("preReleaseBuild") { dependsOn(checkReleaseRuntime) } }
