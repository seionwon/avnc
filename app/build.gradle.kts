import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.plugin.serialization")
    id("org.jetbrains.kotlin.plugin.parcelize")
    id("com.android.legacy-kapt")
}

android {
    namespace = "com.gaurav.avnc"

    compileSdk = 36
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = "com.gaurav.avnc"
        minSdk = 21
        targetSdk = 36
        versionCode = 62
        versionName = "3.3.10-seionwon"

        javaCompileOptions {
            annotationProcessorOptions {
                arguments += "room.schemaLocation" to "$projectDir/roomSchema/"
            }
        }

        externalNativeBuild {
            cmake {
                arguments(
                        "-DANDROID_STL=none", // We are not using STL
                        "-DCMAKE_TOOLCHAIN_FILE=${layout.settingsDirectory}/extern/vcpkg/scripts/buildsystems/vcpkg.cmake",
                        "-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=\${ndk.moduleNdkDir}/build/cmake/android.toolchain.cmake",
                        "-DVCPKG_MANIFEST_MODE=ON"
                )

                targets("native-vnc", "vncclient")
            }
        }

        // On Apple Silicon macOS without Rosetta, NDK's x86_64 yasm cannot assemble x86/x86_64 libjpeg-turbo SIMD.
        // Build ARM ABIs (arm64-v8a + armeabi-v7a) cleanly into app/build/outputs/apk/ unless overridden.
        val isAppleSiliconMac = System.getProperty("os.name").startsWith("Mac", ignoreCase = true) &&
                System.getProperty("os.arch").equals("aarch64", ignoreCase = true)
        if (isAppleSiliconMac && !project.hasProperty("avncAllAbis")) {
            ndk {
                abiFilters += listOf("arm64-v8a", "armeabi-v7a")
            }
        }
    }

    sourceSets {
        getByName("androidTest").assets.directories += "$projectDir/roomSchema"
    }

    buildTypes {

        debug {
            applicationIdSuffix = ".debug"
            versionNameSuffix = " (debug)"

            // This variant is the installer people actually run, so it must not carry debug costs:
            // an unoptimized decoder, debug crypto/JPEG libraries and ART's debuggable mode.
            // Pass -PavncDebuggable to get the debugger, LeakCanary and inline mocking back.
            isDebuggable = project.hasProperty("avncDebuggable")
            if (!isDebuggable) {
                externalNativeBuild {
                    cmake {
                        arguments += "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
                    }
                }
            }
        }

        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }

        create("ci") {
            initWith(getByName("release"))
            applicationIdSuffix = ".ci"
            versionNameSuffix = " (CI)"
        }
    }

    buildFeatures {
        dataBinding = true
        buildConfig = true
    }

    externalNativeBuild {
        cmake {
            version = "3.22.1"
            path("CMakeLists.txt")
        }
    }

    bundle {
        density {
            enableSplit = false
        }
        language {
            enableSplit = false
        }
    }

    packaging {
        resources.excludes += "META-INF/LICENSE.md"
        resources.excludes += "META-INF/LICENSE-notice.md"
        resources.excludes += "META-INF/DEPENDENCIES"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_1_8
        targetCompatibility = JavaVersion.VERSION_1_8
    }
}

val avncDebugApkName = "AVNC-v${android.defaultConfig.versionName}.apk"

// Android Studio/CodeMemo가 보여 주는 기본 디버그 산출물도 앱 이름과 버전 번호로 생성한다.
androidComponents {
    onVariants(selector().withBuildType("debug")) { variant ->
        variant.outputs.forEach { output -> output.outputFileName.set(avncDebugApkName) }
    }
}

kotlin {
    compilerOptions {
        jvmTarget = JvmTarget.JVM_1_8
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.16.0")
    implementation("androidx.activity:activity-ktx:1.10.1")
    implementation("androidx.fragment:fragment-ktx:1.8.9")
    implementation("androidx.appcompat:appcompat:1.7.1")
    implementation("androidx.dynamicanimation:dynamicanimation-ktx:1.1.0")
    implementation("androidx.biometric:biometric-ktx:1.2.0-alpha05")
    implementation("androidx.preference:preference-ktx:1.2.1")
    implementation("androidx.lifecycle:lifecycle-viewmodel-ktx:2.9.3")
    implementation("androidx.lifecycle:lifecycle-livedata-ktx:2.9.3")
    implementation("androidx.recyclerview:recyclerview:1.4.0")
    implementation("androidx.constraintlayout:constraintlayout:2.1.4")
    implementation("androidx.drawerlayout:drawerlayout:1.2.0")

    val roomVersion = "2.7.2"
    implementation("androidx.room:room-runtime:$roomVersion")
    implementation("androidx.room:room-ktx:$roomVersion")
    kapt("androidx.room:room-compiler:$roomVersion")

    implementation("com.google.android.material:material:1.7.0")
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.9.0")
    implementation("org.connectbot:sshlib:2.2.36")
    implementation("com.journeyapps:zxing-android-embedded:4.3.0")

    // LeakCanary writes heap dumps holding decrypted secrets and exports an activity,
    // so only the opt-in developer build packages it.
    val leakCanary = "com.squareup.leakcanary:leakcanary-android:2.14"
    if (project.hasProperty("avncDebuggable")) debugImplementation(leakCanary) else debugCompileOnly(leakCanary)

    testImplementation("junit:junit:4.13.2")
    androidTestImplementation("androidx.test:runner:1.7.0")
    androidTestImplementation("androidx.test.espresso:espresso-core:3.7.0")
    androidTestImplementation("androidx.test.espresso:espresso-contrib:3.7.0")
    androidTestImplementation("androidx.test.espresso:espresso-intents:3.7.0")
    androidTestImplementation("androidx.test:rules:1.7.0")
    androidTestImplementation("androidx.test.ext:junit-ktx:1.3.0")
    androidTestImplementation("androidx.room:room-testing:$roomVersion")
    androidTestImplementation("io.mockk:mockk-android:1.14.5")
    androidTestImplementation("org.apache.sshd:sshd-core:2.16.0")
}