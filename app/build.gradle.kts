import java.net.URI
import java.security.MessageDigest

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
    alias(libs.plugins.kotlin.serialization)
    alias(libs.plugins.ksp)
}

android {
    namespace = "io.github.alecjacobson.besttake"
    compileSdk = 36
    ndkVersion = "27.0.12077973"

    defaultConfig {
        applicationId = "io.github.alecjacobson.besttake"
        // Android 16 is the initial product floor (standard Pixel 10).
        minSdk = 36
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"
        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_static")
                cppFlags += "-O2"
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    buildFeatures { compose = true }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
    androidResources { noCompress += listOf("tflite", "task") }
    sourceSets["main"].assets.srcDir(layout.buildDirectory.dir("models"))
}

kotlin { jvmToolchain(17) }

// Models are packaged so that editing works offline. Each artifact is pinned
// by hash; see docs/models.md for provenance and license notes.
val modelArtifacts = mapOf(
    "face_landmarker.task" to Pair(
        "https://storage.googleapis.com/mediapipe-models/face_landmarker/face_landmarker/float16/1/face_landmarker.task",
        "64184e229b263107bc2b804c6625db1341ff2bb731874b0bcc2fe6544e0bc9ff",
    ),
    "blaze_face_short_range.tflite" to Pair(
        "https://storage.googleapis.com/mediapipe-models/face_detector/blaze_face_short_range/float16/1/blaze_face_short_range.tflite",
        "b4578f35940bf5a1a655214a1cce5cab13eba73c1297cd78e1a04c2380b0152f",
    ),
)

val downloadModels by tasks.registering {
    val outputDir = layout.buildDirectory.dir("models/models")
    outputs.dir(outputDir)
    inputs.property("artifacts", modelArtifacts.mapValues { it.value.second })
    doLast {
        val dir = outputDir.get().asFile.apply { mkdirs() }
        for ((name, source) in modelArtifacts) {
            val (url, expected) = source
            val bytes = URI(url).toURL().openStream().use { it.readBytes() }
            val actual = MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }
            check(actual == expected) { "Model $name has hash $actual, expected $expected" }
            dir.resolve(name).writeBytes(bytes)
        }
    }
}
tasks.named("preBuild") { dependsOn(downloadModels) }

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    implementation(libs.androidx.lifecycle.runtime.compose)
    implementation(libs.androidx.exifinterface)
    implementation(platform(libs.compose.bom))
    implementation(libs.compose.ui)
    implementation(libs.compose.foundation)
    implementation(libs.compose.material3)
    implementation(libs.compose.material.icons)
    implementation(libs.room.runtime)
    implementation(libs.room.ktx)
    ksp(libs.room.compiler)
    implementation(libs.kotlinx.coroutines.android)
    implementation(libs.kotlinx.serialization.json)
    implementation(libs.mediapipe.tasks.vision)
    testImplementation(libs.junit)
}

// Host build of the JNI bridge and a procedural photo set, so JVM unit tests
// exercise the real Kotlin -> JNI -> engine path without a device.
val hostJniDir = layout.buildDirectory.dir("hostJni")
val engineFixtureDir = layout.buildDirectory.dir("engineFixture")
val testJdk = javaToolchains.launcherFor { languageVersion = JavaLanguageVersion.of(17) }
val buildHostJni by tasks.registering {
    val javaHome = testJdk.map { it.metadata.installationPath.asFile.path }
    inputs.property("javaHome", javaHome)
    val sources = fileTree("src/main/cpp") + fileTree("../engine") { exclude("build/**") }
    inputs.files(sources)
    outputs.dir(hostJniDir)
    outputs.dir(engineFixtureDir)
    doLast {
        val build = hostJniDir.get().asFile
        val fixture = engineFixtureDir.get().asFile.apply { mkdirs() }
        val jobs = Runtime.getRuntime().availableProcessors().toString()
        fun run(vararg command: String) = project.exec { commandLine(*command) }
        run(
            "cmake", "-S", file("src/main/cpp").path, "-B", build.path,
            "-DCMAKE_BUILD_TYPE=Release", "-DBT_JAVA_HOME=${javaHome.get()}",
        )
        run("cmake", "--build", build.path, "-j", jobs)
        run(build.resolve("engine/tools/bt_fixture").path, fixture.path)
    }
}
tasks.withType<Test>().configureEach {
    dependsOn(buildHostJni)
    javaLauncher = testJdk
    systemProperty("java.library.path", hostJniDir.get().asFile.path)
    systemProperty("besttake.fixture", engineFixtureDir.get().asFile.path)
}
