import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
}

// Release signing: keystore.properties (git-ignored; written by the release workflow from secrets)
// holds storeFile, storePassword, keyAlias and keyPassword. Without it the release APK is unsigned.
val keystoreProps = Properties().apply {
    val f = rootProject.file("keystore.properties")
    if (f.exists()) f.inputStream().use { load(it) }
}

android {
    namespace = "com.linuxkernel44.audiovisualizer"
    compileSdk = 36
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = "com.linuxkernel44.audiovisualizer"
        minSdk = 31
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_shared", "-DORT_ROOT=${layout.buildDirectory.get().asFile}/ort")
                cppFlags += "-std=c++20"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../engine/CMakeLists.txt")
            version = "3.31.6"
        }
    }

    signingConfigs {
        if (keystoreProps.containsKey("storeFile")) {
            create("release") {
                storeFile = file(keystoreProps.getProperty("storeFile"))
                storePassword = keystoreProps.getProperty("storePassword")
                keyAlias = keystoreProps.getProperty("keyAlias")
                keyPassword = keystoreProps.getProperty("keyPassword")
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
    testOptions { unitTests.isReturnDefaultValues = true }

    // The ONNX model is opened through AssetFileDescriptor, which needs it stored uncompressed.
    androidResources { noCompress += "onnx" }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

// ONNX Runtime ships headers and the arm64 library inside its AAR (no prefab package), so the AAR
// is unpacked here and handed to CMake; the same AAR is a normal dependency so the .so is packaged.
val ortVersion = "1.30.0"
val ortAar: Configuration by configurations.creating
val extractOrt = tasks.register<Copy>("extractOrt") {
    from(zipTree({ ortAar.singleFile })) { include("headers/**", "jni/arm64-v8a/**") }
    into(layout.buildDirectory.dir("ort"))
}
tasks.configureEach {
    if (name.startsWith("configureCMake") || name.startsWith("buildCMake")) dependsOn(extractOrt)
}

dependencies {
    ortAar("com.microsoft.onnxruntime:onnxruntime-android:$ortVersion@aar")
    implementation("com.microsoft.onnxruntime:onnxruntime-android:$ortVersion")
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.material3)

    androidTestImplementation("androidx.test.ext:junit:1.2.1")
    androidTestImplementation("androidx.test:runner:1.6.2")
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.json:json:20240303")  // the android.jar stubs of org.json do nothing in JVM tests
}
