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
tasks.named("preBuild") { dependsOn(bundleThumbRuntime) }
