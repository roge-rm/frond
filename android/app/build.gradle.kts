import java.net.URI
import java.security.MessageDigest
import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
}

// Release signing reads frond-keystore.properties from -Pfrond.signingProperties, else
// ~/Projects/Keys, else ../Keys beside the projects, so any checkout or worktree finds it.
// Without one, a release build stops unless -Pfrond.allowUnsigned=true.
val signingPropertiesFile: File? = listOfNotNull(
    providers.gradleProperty("frond.signingProperties").orNull?.let { File(it) },
    File(System.getProperty("user.home"), "Projects/Keys/frond-keystore.properties"),
    rootProject.file("../../../AndroidStudioProjects/Keys/frond-keystore.properties"),
).firstOrNull { it.exists() }
val signingProperties: Properties? = signingPropertiesFile
    ?.let { f -> Properties().apply { f.inputStream().use { load(it) } } }
val allowUnsigned = providers.gradleProperty("frond.allowUnsigned").orNull == "true"
val releaseStoreFile = signingProperties?.getProperty("storeFile")

// The repo root, one above android/. The core's source is in core/ there.
val repoRoot: File = rootProject.projectDir.parentFile
// frond.coreSource builds from a copy of core/ in that folder instead, made by tools/copy-core.py.
val coreSourceOverride = providers.gradleProperty("frond.coreSource").orNull
val coreSource: File = coreSourceOverride?.let { File(it).let { path -> if (path.isAbsolute) path else File(repoRoot, it) } }
    ?: File(repoRoot, "core")

// The native code compiles through ccache when it's on PATH, unless FROND_CCACHE=0.
// prefix_command= keeps icecream out, as its nodes have no NDK compiler.
val ccache: String? = if (providers.environmentVariable("FROND_CCACHE").orNull == "0") null else
    providers.environmentVariable("PATH").orNull?.split(File.pathSeparator)
        ?.map { File(it, "ccache") }?.firstOrNull { it.isFile && it.canExecute() }?.path

// SDL2 and SDL2_image match the desktop build (Debian's libsdl2 2.32.4 and
// libsdl2-image 2.8.8). The tarballs are the GitHub releases, checked against these
// hashes; their signatures from Sam Lantinga's key were verified once when pinned.
val sdlVersion = "2.32.4"
val sdlSha256 = "f15b478253e1ff6dac62257ded225ff4e7d0c5230204ac3450f1144ee806f934"
val sdlUrl = "https://github.com/libsdl-org/SDL/releases/download/release-$sdlVersion/SDL2-$sdlVersion.tar.gz"
val sdlRoot: File = rootProject.layout.buildDirectory.dir("sdl").get().asFile
val sdlSource = File(sdlRoot, "SDL2-$sdlVersion")

val sdlImageVersion = "2.8.8"
val sdlImageSha256 = "2213b56fdaff2220d0e38c8e420cbe1a83c87374190cba8c70af2156097ce30a"
val sdlImageUrl =
    "https://github.com/libsdl-org/SDL_image/releases/download/release-$sdlImageVersion/SDL2_image-$sdlImageVersion.tar.gz"
val sdlImageSource = File(sdlRoot, "SDL2_image-$sdlImageVersion")

// Self-test build: -Pfrond.selfTest=true builds the M1 CPU self-test as libmain.so
// instead of the emulator.
val selfTest = providers.gradleProperty("frond.selfTest").map { it.toBoolean() }.getOrElse(false)
// -Pfrond.jitDifferential=true also builds tests/jit-differential.cpp and tools/benchmark-replay.cpp
// as programs (arm64-v8a and x86_64), to push to /data/local/tmp and run with adb shell.
val jitDifferential = providers.gradleProperty("frond.jitDifferential").map { it.toBoolean() }.getOrElse(false)

val prepareCore = tasks.register<Exec>("prepareCore") {
    group = "build"
    description = "Copies core/ to frond.coreSource when that is set"
    onlyIf { coreSourceOverride != null }
    workingDir(repoRoot)
    commandLine("python3", "tools/copy-core.py", coreSource.absolutePath)
}

// The Fast engine's Dynarmic source, with Frond's changes, in vendor/dynarmic. Dynarmic also
// needs Boost's headers; frond.boostInclude is the folder holding boost/, /usr/include by default.
val dynarmicSource: File = File(repoRoot, "vendor/dynarmic")
val boostInclude = providers.gradleProperty("frond.boostInclude").getOrElse("/usr/include")

// The licence notices shown by About: the repo's licences/ folder plus the GPL text from LICENSE.
// The committed FrondBench.prc goes in the same assets folder, for Install and Save.
val licencesAssets = layout.buildDirectory.dir("generated/licences-assets")
val copyLicences = tasks.register<Sync>("copyLicences") {
    from(File(repoRoot, "licences")) { into("licences") }
    from(File(repoRoot, "LICENSE")) { into("licences"); rename { "GPL-3.0.txt" } }
    from(File(repoRoot, "tools/frondbench/FrondBench.prc")) { into("frondbench") }
    into(licencesAssets)
}
tasks.configureEach {
    if ((name.startsWith("merge") && name.endsWith("Assets")) || name.lowercase().contains("lint")) dependsOn(copyLicences)
}

val checkDeviceData = tasks.register<Exec>("checkDeviceData") {
    dependsOn(prepareCore)
    workingDir(repoRoot)
    commandLine("python3", "tools/generate-device-data.py", "--check", "--source", coreSource.absolutePath,
        "--build-dir", layout.buildDirectory.dir("device-data").get().asFile.absolutePath)
}
tasks.named("preBuild") { dependsOn(checkDeviceData) }

// Downloads a release tarball into sdlRoot, checks its SHA-256 and unpacks it.
fun registerFetch(name: String, url: String, sha256: String, source: File) = tasks.register(name) {
    group = "build"
    description = "Downloads ${source.name}, checks its SHA-256 and unpacks it"
    val archive = File(sdlRoot, source.name + ".tar.gz")
    val stamp = File(source, ".frond-sha256")
    outputs.file(stamp)
    doLast {
        fun hash(file: File): String = MessageDigest.getInstance("SHA-256")
            .digest(file.readBytes()).joinToString("") { "%02x".format(it) }
        if (stamp.isFile && stamp.readText().trim() == sha256) return@doLast
        sdlRoot.mkdirs()
        if (!archive.isFile || hash(archive) != sha256) {
            val partial = File(sdlRoot, archive.name + ".part")
            URI(url).toURL().openStream().use { input -> partial.outputStream().use { input.copyTo(it) } }
            val actual = hash(partial)
            if (actual != sha256) {
                partial.delete()
                throw GradleException("${archive.name} download has SHA-256 $actual, expected $sha256")
            }
            partial.renameTo(archive)
        }
        source.deleteRecursively()
        project.providers.exec { commandLine("tar", "-xzf", archive.absolutePath, "-C", sdlRoot.absolutePath) }.result.get()
        stamp.writeText(sha256 + "\n")
    }
}

val fetchSdl = registerFetch("fetchSdl", sdlUrl, sdlSha256, sdlSource)
val fetchSdlImage = registerFetch("fetchSdlImage", sdlImageUrl, sdlImageSha256, sdlImageSource)

val networkRoot = rootProject.layout.buildDirectory.dir("net").get().asFile
val networkTasks = listOf("arm64-v8a", "x86_64").map { abi ->
    tasks.register<Exec>("buildNetwork${abi.replace("-", "").replace("_", "")}") {
        group = "build"
        inputs.file(rootProject.file("build-network.py"))
        inputs.property("ndkVersion", "28.2.13676358")
        inputs.file(File(androidComponents.sdkComponents.sdkDirectory.get().asFile,
            "ndk/28.2.13676358/source.properties"))
        outputs.files(listOf(".built", "install/lib/libslirp.a", "install/lib/libglib-2.0.a",
            "install/lib/libpcre2-8.a", "install/lib/libiconv.a", "install/lib/libintl.a", "install/include/slirp/libslirp.h", "install/include/slirp/libslirp-version.h",
            "install/include/glib-2.0/glib.h", "install/include/glib-2.0/glibconfig.h",
            "install/lib/pkgconfig/glib-2.0.pc", "install/lib/pkgconfig/slirp.pc")
            .map { File(networkRoot, "$abi/$it") })
        // meson wraps the NDK compiler in ccache by itself; keep icecream out of it.
        environment("CCACHE_PREFIX", "")
        commandLine("python3", rootProject.file("build-network.py"),
            "--ndk", File(androidComponents.sdkComponents.sdkDirectory.get().asFile, "ndk/28.2.13676358"),
            "--abi", abi, "--root", networkRoot)
    }
}
networkTasks[1].configure { mustRunAfter(networkTasks[0]) }
tasks.configureEach {
    if (name.startsWith("configureCMake") || name.startsWith("buildCMake")) {
        dependsOn(networkTasks)
    }
}

val bluetoothRoot = rootProject.layout.buildDirectory.dir("bt").get().asFile
val bluetoothTasks = listOf("arm64-v8a", "x86_64").map { abi ->
    tasks.register<Exec>("buildBluetooth${abi.replace("-", "").replace("_", "")}") {
        group = "build"
        inputs.file(rootProject.file("build-bluetooth.py"))
        inputs.file(File(repoRoot, "tools/bluez-bdaddr.patch"))
        inputs.file(File(repoRoot, "tools/bluez-bthost-config.patch"))
        inputs.file(File(repoRoot, "tools/bluez-bthost-stream.patch"))
        inputs.file(File(androidComponents.sdkComponents.sdkDirectory.get().asFile,
            "ndk/28.2.13676358/source.properties"))
        outputs.file(File(bluetoothRoot, "$abi/.built"))
        outputs.file(File(bluetoothRoot, "jniLibs/$abi/libbtvirt.so"))
        outputs.dir(File(bluetoothRoot, "$abi/source").absoluteFile)
        outputs.dir(File(bluetoothRoot, "$abi/phone"))
        commandLine("python3", rootProject.file("build-bluetooth.py"),
            "--ndk", File(androidComponents.sdkComponents.sdkDirectory.get().asFile, "ndk/28.2.13676358"),
            "--abi", abi, "--root", bluetoothRoot)
    }
}
bluetoothTasks[1].configure { mustRunAfter(bluetoothTasks[0]) }
tasks.configureEach {
    if (name.startsWith("merge") && name.endsWith("JniLibFolders")) {
        dependsOn(bluetoothTasks)
    }
    if (name.startsWith("configureCMake") || name.startsWith("buildCMake")) {
        dependsOn(bluetoothTasks)
    }
}

android {
    namespace = "com.rm.frond"
    compileSdk {
        version = release(37)
    }

    defaultConfig {
        applicationId = "com.rm.frond"
        minSdk = 26
        targetSdk = 37
        testInstrumentationRunner = "com.rm.frond.MachineSettingsInstrumentation"
        versionCode = 21
        versionName = "0.20.0"
        ndk { abiFilters += listOf("arm64-v8a", "x86_64") }
        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DFROND_CORE=${coreSource.absolutePath}",
                    "-DFROND_NETWORK_ROOT=${networkRoot.absolutePath}",
                    "-DFROND_BLUETOOTH_ROOT=${bluetoothRoot.absolutePath}",
                    "-DSDL2_SOURCE=${sdlSource.absolutePath}",
                    "-DSDL2_IMAGE_SOURCE=${sdlImageSource.absolutePath}",
                    "-DFROND_SELF_TEST=${if (selfTest) "ON" else "OFF"}",
                    "-DFROND_JIT_DIFFERENTIAL=${if (jitDifferential) "ON" else "OFF"}",
                    "-DFROND_DYNARMIC=${dynarmicSource.absolutePath}",
                    "-DFROND_BOOST_INCLUDE=$boostInclude",
                )
                if (ccache != null) arguments += listOf(
                    "-DCMAKE_C_COMPILER_LAUNCHER=$ccache;prefix_command=",
                    "-DCMAKE_CXX_COMPILER_LAUNCHER=$ccache;prefix_command=",
                )
                targets += listOf("main", "main68k", "frondcard", "frondlink", "frondsync", "frond-bluetooth-phone")
                if (jitDifferential) targets += listOf("jit-differential", "benchmark-replay")
            }
        }
    }
    ndkVersion = "28.2.13676358"
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "4.1.2"
        }
    }

    signingConfigs {
        if (releaseStoreFile != null) {
            create("release") {
                storeFile = file(releaseStoreFile)
                storePassword = signingProperties?.getProperty("storePassword")
                keyAlias = signingProperties?.getProperty("keyAlias")
                keyPassword = signingProperties?.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        debug {
            applicationIdSuffix = ".debug"
            externalNativeBuild.cmake.arguments += "-DFROND_PHONE_OUTPUT=${File(bluetoothRoot, "debug").absolutePath}"
        }
        release {
            externalNativeBuild.cmake.arguments += "-DFROND_PHONE_OUTPUT=${File(bluetoothRoot, "release").absolutePath}"
            if (releaseStoreFile != null) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }
    packaging {
        jniLibs.useLegacyPackaging = true
    }
    sourceSets {
        getByName("debug").jniLibs.directories.add(File(bluetoothRoot, "debug").absolutePath)
        getByName("release").jniLibs.directories.add(File(bluetoothRoot, "release").absolutePath)
        getByName("main") {
            jniLibs.directories.add(File(bluetoothRoot, "jniLibs").absolutePath)
            assets.directories.add(licencesAssets.get().asFile.absolutePath)
            // SDL's Java glue, SDLActivity and friends, straight from the pinned tarball.
            java.directories.add(File(sdlSource, "android-project/app/src/main/java").absolutePath)
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    buildFeatures {
        compose = true
    }
}

// The phone is a PIE executable, packaged alongside btvirt so Android extracts both.
tasks.configureEach {
    if (name == "mergeDebugJniLibFolders") dependsOn("externalNativeBuildDebug")
    if (name == "mergeReleaseJniLibFolders") dependsOn("externalNativeBuildRelease")
}

dependencies {
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.core.ktx)
    implementation(libs.jb.compose.runtime)
    implementation(libs.jb.compose.foundation)
    implementation(libs.jb.compose.ui)
    implementation(libs.jb.compose.material3)
    implementation(libs.compose.material.icons)
    implementation(libs.kotlinx.coroutines.android)
    testImplementation(libs.junit)
    // The JUnit 4 tests run on the JUnit Platform through the vintage engine, beside Jazzer's @FuzzTest ones.
    testImplementation(platform(libs.junit.bom))
    testImplementation(libs.junit.jupiter)
    testImplementation(libs.jazzer.junit)
    testRuntimeOnly(libs.junit.vintage)
    testRuntimeOnly(libs.junit.launcher)
    testImplementation("org.json:json:20250517")
}

tasks.named("preBuild") { dependsOn(prepareCore, fetchSdl, fetchSdlImage) }

// Unit tests load a host build of the storage library's JNI (CardStorageJni.cpp and ZaurusCfJni.cpp
// with miniz) as libfrondcard.so, so the CardFile, FatImage and ZaurusCf tests run the same C++ as the
// app. Needs the host's cc and c++, and the JDK's jni.h.
val hostStorage: File = layout.buildDirectory.dir("host-storage").get().asFile
val buildHostStorage = tasks.register<Exec>("buildHostStorage") {
    group = "build"
    description = "Builds the storage library's JNI for the host, for unit tests"
    dependsOn(prepareCore)
    val jni = File(projectDir, "src/main/cpp/CardStorageJni.cpp")
    val zaurusJni = File(projectDir, "src/main/cpp/ZaurusCfJni.cpp")
    val common = File(coreSource, "src/common")
    val zaurusSources = listOf("TarStream", "Ext3Writer", "Fat32Writer", "ZaurusCf")
        .joinToString(" ") { "\"$common/storage/$it.cpp\"" }
    val jdk = File(System.getProperty("java.home"))
    inputs.file(jni)
    inputs.file(zaurusJni)
    inputs.dir(File(common, "storage"))
    inputs.file(File(common, "zip/miniz.c"))
    outputs.file(File(hostStorage, "libfrondcard.so"))
    doFirst { hostStorage.mkdirs() }
    commandLine("sh", "-c",
        "cc -std=gnu99 -O1 -fPIC -I\"$common\" -c \"$common/zip/miniz.c\" -o \"$hostStorage/miniz.o\" && " +
        "c++ -std=c++17 -O1 -g -fPIC -shared -Wall -Wextra -Wno-unused-parameter " +
        "-I\"$jdk/include\" -I\"$jdk/include/linux\" -I\"$common\" \"$jni\" \"$zaurusJni\" $zaurusSources " +
        "\"$hostStorage/miniz.o\" " +
        "-o \"$hostStorage/libfrondcard.so\"")
}
// -Pfrond.fuzz=CLASS fuzzes that @FuzzTest class for frond.fuzzTime (default 5m) instead of only
// replaying its seeds, with JAZZER_FUZZ as Jazzer reads it. Jazzer fuzzes one test per run, so
// -Pfrond.fuzz also takes CLASS.method. Findings go in build/fuzzing-f6/findings/ of the repo.
val fuzzTarget = providers.gradleProperty("frond.fuzz").orNull
val fuzzTime = providers.gradleProperty("frond.fuzzTime").getOrElse("5m")
tasks.withType<Test>().configureEach {
    dependsOn(buildHostStorage, copyLicences)
    systemProperty("java.library.path", hostStorage.absolutePath)
    useJUnitPlatform()
    // Jazzer attaches its agent at run time. ZaurusCfTest hands the JNI raw file descriptors.
    jvmArgs("-XX:+EnableDynamicAgentLoading", "--add-opens=java.base/java.io=ALL-UNNAMED")
    if (fuzzTarget != null) {
        environment("JAZZER_FUZZ", "1")
        systemProperty("jazzer.max_duration", fuzzTime)
        systemProperty("jazzer.internal.basedir", File(repoRoot, "build/fuzzing-f6").absolutePath)
        filter.includeTestsMatching("com.rm.frond.$fuzzTarget")
        outputs.upToDateWhen { false }
    }
}

// A release APK without the release key can't update installed copies, so fail instead.
val releaseSigned = releaseStoreFile != null && File(releaseStoreFile).exists()
tasks.matching { it.name == "preReleaseBuild" }.configureEach {
    doFirst {
        if (!releaseSigned && !allowUnsigned) {
            throw GradleException("No release key: frond-keystore.properties not found or its storeFile is missing. " +
                "Pass -Pfrond.signingProperties=PATH, or -Pfrond.allowUnsigned=true for an unsigned build.")
        }
    }
}
