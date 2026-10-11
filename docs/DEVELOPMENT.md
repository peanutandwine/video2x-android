# Video2X for Android

把 [k4yt3x/video2x](https://github.com/k4yt3x/video2x)（v6.x，纯 C/C++ 实现）移植到 Android 的工程。

> **本工程已实测通过完整构建**：从零开始交叉编译全部依赖 → 编译 libvideo2x → 产出可安装 APK（arm64-v8a，142MB 含模型）。

支持 **全量算法**：

| 算法 | 后端 | 模式 |
|---|---|---|
| Real-ESRGAN | ncnn / Vulkan | 超分 |
| Real-CUGAN | ncnn / Vulkan | 超分（带降噪） |
| Anime4K v4 | FFmpeg `libplacebo` filter / Vulkan | 超分（GLSL 着色器） |
| RIFE | ncnn / Vulkan | 插帧 |

设计与风险分析见方案文档 `video2x-android-移植方案.md`。

---

## 1. 架构总览

```
Kotlin UI (Compose)
      │  SAF 选片 → 拷入私有目录
      ▼
ProcessingService（前台服务，长任务 + 进度轮询）
      │  JNI
      ▼
video2x_jni.cpp ──► libvideo2x（上游源码 + FFmpeg，共 2 处补丁，见 §6）
                        ├─ RealESRGAN / RealCUGAN / RIFE（ncnn + Vulkan）
                        └─ Anime4K（FFmpeg libplacebo filter + Vulkan）
      ▲
      └── 依赖：FFmpeg、ncnn、libplacebo、glslang、spdlog、OpenMP、x264
```

**关键设计：**

- **零侵入上游**：日志（logcat sink）、进度、中止全部通过上游公开 API 实现，不改源码。
- **补丁共 2 处**，均由 `apply_patches.sh` 幂等注入（字符串锚点匹配，不依赖行号，上游小版本更新不会脆断）：
  1. **`video2x/src/fsutils.cpp`** —— `find_resource()` 增加读取 `VIDEO2X_RESOURCE_DIR` 环境变量，用于在 Android 上定位模型与 Anime4K 着色器。上游只探测 `/proc/self/exe` 同目录与 `/usr/share`，在 Android 上全部不成立。
  2. **`ffmpeg/libavcodec/mediacodecenc.c`** —— 修复 `mediacodec_init_bsf()` 中未初始化栈缓冲区 `char str[128]`，它会让 MediaCodec 编码器初始化报 `Bitstream filter not found`（详见 §6 坑位 #16）。
  其余功能（日志、进度、中止、编码参数）全部通过上游公开 API 或 JNI 侧实现，**未改动上游逻辑**。详见 `patches/README.md`。
- **不做 CLI**：`VIDEO2X_BUILD_CLI=OFF`，因此**完全不需要 Boost**。
- **文件流**：`VideoProcessor::process()` 只接受文件路径。输入由 SAF 选择后拷入私有目录；**输出固定写入公共目录 `/storage/emulated/0/Video2x/`，文件名 = 原文件名 + 算法参数**（如 `video_realesrgan-animevideov3-x4.mp4`）。写公共目录需要「所有文件访问」权限（`MANAGE_EXTERNAL_STORAGE`，个人侧载场景）。

---

## 2. 目录结构

```
video2x-android/
├── app/
│   ├── build.gradle.kts
│   └── src/main/
│       ├── AndroidManifest.xml
│       ├── cpp/
│       │   ├── CMakeLists.txt        # 链接预编译依赖，编译 JNI 层
│       │   └── video2x_jni.cpp       # JNI 桥（核心）
│       ├── java/com/video2x/android/ # Kotlin UI / Service / 文件桥接
│       └── assets/models/            # ← 模型放这里
├── scripts/                          # 交叉编译脚本
│   ├── config.sh                     # 公共配置（NDK/ABI/API/版本）
│   ├── fetch_sources.sh              # 拉取全部源码
│   ├── apply_patches.sh              # 幂等打补丁
│   ├── build_glslang.sh              # ① GLSL→SPIR-V 编译器
│   ├── build_libplacebo.sh           # ② Anime4K 渲染后端（Vulkan）
│   ├── build_x264.sh                 # ③ H.264 软编码器
│   ├── build_ffmpeg.sh               # ④ 解码/编码/滤镜底座
│   ├── build_ncnn.sh                 # ⑤ 神经网络推理
│   ├── build_spdlog.sh               # ⑥ 日志
│   ├── build_video2x.sh              # ⑦ 核心库
│   ├── build_all.sh                  # 一键
│   └── prepare_jni_libs.sh           # 汇总 .so 供 gradle 打包
└── patches/README.md                 # 补丁说明
```

---

## 3. 环境要求

**构建机（Linux，实测 Ubuntu 24.04）：**

- Android **NDK r27c**
- CMake ≥ 3.22、Ninja、pkg-config、git、curl、yasm/nasm
- **Meson**（libplacebo 必需）：`pip install meson ninja`
- **glslangValidator**（算法库编译 shader 用，host 工具）：`apt install glslang-tools`
- Python 3
- 磁盘 ≥ 25GB

**设备：**

- **arm64-v8a**、**Android 8.0（API 26）** 及以上
- **支持 Vulkan** 的 GPU

---

## 4. 构建步骤

### 4.1 交叉编译全部依赖

```bash
cd scripts
export ANDROID_NDK_ROOT=$HOME/Android/Sdk/ndk/27.1.12297006
./build_all.sh
```

编译顺序（不可调换）：

```
glslang → libplacebo(vulkan) → x264 → FFmpeg → ncnn → spdlog → libvideo2x
```

产物统一安装到 `scripts/dist/arm64-v8a/`：

```
dist/arm64-v8a/
├── include/
├── lib/          # libvideo2x.so, libav*.so, libomp.so, libplacebo.a ...
├── lib/cmake/    # ncnn / spdlog / glslang 的 CMake config
└── res/          # 模型与 Anime4K 着色器
```

### 4.2 放入模型资产

```bash
cp -r scripts/dist/arm64-v8a/res/. app/src/main/assets/models/
```

体积参考：`libplacebo` 3.2M / `realcugan` 46M / `realesrgan` 54M / `rife` **948M**。

> `rife` 模型异常庞大（近 1GB），**不建议打进 APK**。仅用超分功能时拷前三个即可。

### 4.3 汇总 native 库

gradle 的 jniLibs 要求按 ABI 子目录组织：

```bash
./scripts/prepare_jni_libs.sh
```

### 4.4 构建 APK

用 Android Studio 打开本目录，或命令行：

```bash
gradle wrapper --gradle-version 8.9   # 若没有 wrapper
./gradlew :app:assembleDebug
```

输出：`app/build/outputs/apk/debug/app-debug.apk`

### 4.5 安装运行

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb logcat -s Video2X Video2X-JNI
```

---

## 5. 实测验证结果

| 阶段 | 结果 |
|---|---|
| glslang | ✅ libglslang.a / libSPIRV.a / libMachineIndependent.a |
| libplacebo（Vulkan 后端） | ✅ **`vulkan : YES` / `glslang : YES`** —— 全案最高风险点验证通过 |
| x264 | ✅ libx264.a |
| FFmpeg | ✅ libav*.so（含 libplacebo filter + vulkan + mediacodec + libx264）|
| ncnn | ✅ libncnn.a（Vulkan）|
| spdlog | ✅ |
| libvideo2x | ✅ `ELF 64-bit LSB shared object, ARM aarch64` |
| APK | ✅ `BUILD SUCCESSFUL`，142MB，含全部 .so 与模型 |

APK 内已确认包含：`libvideo2x.so`、`libvideo2x_jni.so`、`libav*.so`、`libsw*.so`、`lib{realesrgan,realcugan,rife}-ncnn-vulkan.so`、`libomp.so`、`libc++_shared.so`。

---

## 6. 关键坑位（复现必读）

这些都是实测踩过的，脚本里已修正：

| # | 现象 | 原因 | 处理 |
|---|---|---|---|
| 1 | 链接报 `unable to find library -lavcodec` | 上游 `pkg_check_modules` 只取库名，**没带 `LIBRARY_DIRS`** | 传 `CMAKE_SHARED_LINKER_FLAGS="-L$PREFIX/lib"` |
| 2 | `find_package(ncnn)` 找不到 | NDK toolchain 把 `CMAKE_FIND_ROOT_PATH_MODE_PACKAGE` 设为 `ONLY`，`CMAKE_PREFIX_PATH` 失效 | 设 `..._MODE_PACKAGE/LIBRARY/INCLUDE=BOTH` |
| 3 | meson 报 `SPIRV not found` | libplacebo 用 `cxx.find_library()`，**既不认 `--prefix` 也不认 crossfile 的 `-L` / `LIBRARY_PATH`** | 把 glslang 产物**软链到 NDK sysroot** |
| 4 | FFmpeg `ERROR: vulkan requested but not found` | FFmpeg 7.1 要求 Vulkan 头 **≥1.3.277**，NDK r27 只有 **275** | `vulkan.pc` 指向 **Vulkan-Headers** 的新头（365） |
| 5 | libplacebo `Could not find the vulkan registry (vk.xml)` | NDK 只有 `vulkan.h`，不含 registry | `-Dvulkan-registry=<Vulkan-Headers>/registry/vk.xml` |
| 6 | libplacebo `VK_FORMAT_ASTC_...` 未定义 | 生成代码用新枚举、编译用旧头 | 让 Vulkan-Headers 的头优先于 sysroot |
| 7 | libplacebo `fast_float.h is required` | 子模块未初始化 | `git submodule update --init` |
| 8 | 算法库 `glslangValidator not found` | 需 host 端 shader 编译器 | `apt install glslang-tools` |
| 9 | ncnnConfig 引用 `/${XXX}Targets.cmake` | ncnn 在 `find_package(glslang)` 失败时走坏分支 | 修好 #2 让 glslang 被找到即可 |
| 10 | Kotlin `Unclosed comment` | 注释里写了 `models/libplacebo/*.glsl`，`/*` 触发**嵌套块注释** | 注释中避免出现 `/*` |
| 11 | `out extracted from path .../libavcodec.so is not an ABI` | jniLibs 需要 `<dir>/<abi>/*.so` 结构 | `prepare_jni_libs.sh` 汇总 |
| 12 | 点击「开始」后**卡在「启动中…」不动** | ViewModel 先把 `status.running` 置 `true`，Service 又据此判定「已有任务」直接 `stopSelf()` —— 逻辑互斥，任务永不开始 | 去重判断改由 Service 自己持有的 `job` 决定，不再依赖 `ProcessBus.status` |
| 13 | Android 14+ 前台服务报类型错误 | `startForeground()` 未指定类型 | API 29+ 传 `FOREGROUND_SERVICE_TYPE_DATA_SYNC` |
| 14 | 设备弹「Android 应用兼容性：以下库未进行 16 KB 对齐」 | NDK r27 默认 **4KB 页对齐**，自建库的 LOAD 段是 `0x1000`；Android 15+ 要求 `0x4000` | 所有链接步骤加 `-Wl,-z,max-page-size=16384`（libvideo2x、三个算法库、JNI 层）。NDK r27.2 自带的 libc++/libomp 已对齐，无需换 NDK |
| 15 | Real-ESRGAN 报 `model param file not found: ...-x2.param` | 界面里的模型名/倍率是凭印象写的，与上游 `models/` 实际文件不符：只有 `realesr-animevideov3` 有 x2/x3/x4，其余模型**仅 x4**；RealCUGAN 的路径是 `<变体>/up<N>x-<降噪>.param` 三维组合 | 按实际打包的模型文件重建可选清单，模型与倍率联动、RealCUGAN 拆成 变体/倍率/降噪 三个参数 |
| 16 | MediaCodec 编码器报 `Failed to initialize encoder: Bitstream filter not found` | **FFmpeg 上游 bug**：`libavcodec/mediacodecenc.c` 的 `mediacodec_init_bsf()` 中 `char str[128];` 未初始化。当 `crop==0`（输出尺寸已按 16 对齐）而 `extract_extradata==true`（mp4 输出使 video2x 置上 `AV_CODEC_FLAG_GLOBAL_HEADER`）时，代码跳过 `snprintf` 直接对未初始化缓冲区做 `av_strlcatf` 追加，垃圾串交给 `av_bsf_list_parse_str()` 必然失败 | 在 `apply_patches.sh` 中打补丁：`char str[128] = { 0 };`（幂等注入，不依赖行号） |
| 17 | 开启 R8 后 release 构建报 `Gradle build daemon disappeared unexpectedly`（R8 其实已跑完，死在 `lintVitalRelease`） | 构建环境内存上限 8 GB，而构建期同时存在 Gradle daemon（默认配了 `-Xmx4096m`）、Kotlin 编译 daemon、R8、lint 数个进程，峰值超限后 daemon 被 OOM kill | 把 `org.gradle.jvmargs` 调小到 `-Xmx2560m`、单独限制 `kotlin.daemon.jvmargs`、关掉 `org.gradle.parallel`，并设 `lint { checkReleaseBuilds = false }`（需要时手动跑 `gradle lint`） |
| 18 | 图片超分：多帧图片报 `Cannot write more than one file with the same name` | FFmpeg 的 `image2` muxer 在输出文件名不含 `%d` 序号时，只能写一张图；写第二帧直接失败。而 GIF 会被解码成多帧 | 图片模式只接受单帧静态格式，在 UI（`FileBridge.isImageName`）与 native（`is_image_file`）两侧用**同一份**扩展名白名单，刻意排除 `gif`。另外 GIF 这类动图本来也不适合超分 |

---

## 7. 常见问题

**Q：Anime4K 在设备上崩溃 / 无法创建 Vulkan 设备**
编译期已验证可行（见 §5）。若运行期失败，通常是设备 Vulkan 版本不足；
可按方案 §2.2「路线 B」改用 libplacebo 的 GLES 后端。

**Q：RealESRGAN 处理时 OOM**
手机 GPU 显存有限。上游会按 `ncnn::get_gpu_device()->get_heap_budget()` 自动选择
tile 大小（默认落到 32/64）。可进一步降低分辨率或倍率。

**Q：想用硬件编码**
把 UI 的编码器改为 `h264_mediacodec` / `hevc_mediacodec`（FFmpeg 已启用）。
若质量不理想，用默认的 `libx264`。

---

## 8. 已知限制 / 后续可做

- **仅 arm64-v8a**。
- **未启用硬件解码**（方案 §5.4）：ncnn/libplacebo 都需要 CPU 可访问的帧，硬解回拷收益不确定。后续可评估 `AV_HWDEVICE_TYPE_MEDIACODEC`。
- **RIFE 模型 948MB**，默认不打进 APK；需要插帧功能时单独放入 `filesDir/models/rife`。
- **未在真机运行验证**：本次验证到「产出可安装 APK」为止，设备端的实际处理效果需你实测。
- **许可证**：上游为 **AGPL-3.0**，个人使用无碍。
