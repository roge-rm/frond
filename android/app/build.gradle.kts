import java.net.URI
import java.security.MessageDigest
import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
}

// Release signing comes from ../Keys/frond-keystore.properties, beside the
// projects rather than in one. Without that file the release build is unsigned.
val signingProperties: Properties? = rootProject.file("../../../AndroidStudioProjects/Keys/frond-keystore.properties")
    .takeIf { it.exists() }
    ?.let { f -> Properties().apply { f.inputStream().use { load(it) } } }
val releaseStoreFile = signingProperties?.getProperty("storeFile")

// The repo root, one above android/. The core is reconstructed there in build/source.
val repoRoot: File = rootProject.projectDir.parentFile
val coreSource: File = File(repoRoot, "build/source")

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

val prepareCore = tasks.register<Exec>("prepareCore") {
    group = "build"
    description = "Reconstructs the patched core in build/source with tools/prepare.py"
    workingDir(repoRoot)
    commandLine("python3", "tools/prepare.py")
}

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
        inputs.file(File(androidComponents.sdkComponents.sdkDirectory.get().asFile,
            "ndk/28.2.13676358/source.properties"))
        outputs.file(File(bluetoothRoot, "$abi/.built"))
        outputs.file(File(bluetoothRoot, "jniLibs/$abi/libbtvirt.so"))
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
        versionCode = 1
        versionName = "0.4.0"
        ndk { abiFilters += listOf("arm64-v8a", "x86_64") }
        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DFROND_CORE=${coreSource.absolutePath}",
                    "-DFROND_NETWORK_ROOT=${networkRoot.absolutePath}",
                    "-DSDL2_SOURCE=${sdlSource.absolutePath}",
                    "-DSDL2_IMAGE_SOURCE=${sdlImageSource.absolutePath}",
                    "-DFROND_SELF_TEST=${if (selfTest) "ON" else "OFF"}",
                )
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
        }
        release {
            if (releaseStoreFile != null) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }
    packaging {
        jniLibs.useLegacyPackaging = true
    }
    sourceSets {
        getByName("main") {
            jniLibs.directories.add(File(bluetoothRoot, "jniLibs").absolutePath)
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
}

tasks.named("preBuild") { dependsOn(prepareCore, fetchSdl, fetchSdlImage) }
