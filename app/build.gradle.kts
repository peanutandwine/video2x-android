plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

// 预编译依赖（FFmpeg / ncnn / libplacebo / glslang / libvideo2x）统一安装在此前缀下，
// 由 scripts/build_all.sh 产出。gradle 只做「链接 + 打包」，不负责编译这些大件。
val depsPrefix = file("${rootProject.projectDir}/scripts/dist/arm64-v8a")

android {
    namespace = "com.video2x.android"
    compileSdk = 35
    // 与 scripts/config.sh 中的 NDK 版本保持一致
    ndkVersion = "27.1.12297006"

    defaultConfig {
        applicationId = "com.video2x.android"
        minSdk = 26
        targetSdk = 35
        versionCode = 7
        versionName = "0.3.1"

        // 只做 arm64-v8a：v7a 无实用价值，x86_64 仅模拟器需要
        ndk {
            abiFilters += "arm64-v8a"
        }

        externalNativeBuild {
            cmake {
                // 依赖全部以 c++_shared 构建，保持一致
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DVIDEO2X_PREFIX=$depsPrefix"
                )
                cppFlags += listOf("-std=c++17", "-fexceptions")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    sourceSets {
        getByName("main") {
            // 注意：jniLibs 必须按 ABI 组织，即 <dir>/arm64-v8a/*.so。
            // 依赖的 .so 由 scripts/prepare_jni_libs.sh 从 prefix/lib 汇总到此处。
            jniLibs.srcDirs("src/main/jniLibs")
        }
    }

    buildTypes {
        // R8（压缩 + 混淆）。
        //
        // 注意收益的边界：APK 体积的大头是模型（155MB，noCompress 不压缩）与
        // native 库（61MB，压缩后约 33MB），R8 只能作用于 dex 那一部分
        //（未压缩 44MB → 压缩后 12MB）。所以它只能省个位数 MB，
        // 真正的体积决策在于「模型是否内置」。
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            // 个人使用场景：release 直接复用 debug 签名 —— 免于管理密钥库，
            // 且能与此前安装的 debug 包直接互相覆盖。
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        compose = true
    }

    packaging {
        // 使用传统的 .so 打包方式，便于调试与解压查看
        jniLibs {
            useLegacyPackaging = true
        }
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
    }

    // 模型体积较大时关闭压缩，避免构建期无谓开销
    androidResources {
        noCompress += listOf("bin", "param", "glsl")
    }

    lint {
        // release 构建默认会执行 lintVital（强制致命问题检查），这个任务内存开销
        // 很大 —— 在 8GB 内存上限的环境里会把 Gradle daemon 拖死
        //（"Gradle build daemon disappeared unexpectedly"）。
        // 需要时手动执行 `gradle lint` 即可。
        checkReleaseBuilds = false
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.6")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.6")
    implementation("androidx.activity:activity-compose:1.9.2")

    implementation(platform("androidx.compose:compose-bom:2024.09.03"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended")

    // SAF 目录/文件操作
    implementation("androidx.documentfile:documentfile:1.0.1")

    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")

    debugImplementation("androidx.compose.ui:ui-tooling")
}
