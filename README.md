# Video2X for Android

> 把 [video2x](https://github.com/k4yt3x/video2x) 搬进 Android 的原生应用 —— 视频超分辨率与插帧，编解码走硬件（MediaCodec），推理走 Vulkan。

[![License](https://img.shields.io/badge/license-AGPL--3.0-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Android%208.0%2B%20arm64--v8a-green.svg)](#环境要求)
[![NDK](https://img.shields.io/badge/NDK-r27c-orange.svg)](#构建)
[![Version](https://img.shields.io/badge/version-0.2.0-blueviolet.svg)](../../releases)

---

## 这是什么

[video2x](https://github.com/k4yt3x/video2x) 是一个把「放大 / 插帧」做得相当完整的桌面工具。本项目把它整套搬到 Android：

| 层次 | 说明 |
|---|---|
| **算法内核** | 直接使用上游的 `libvideo2x`（C/C++）。核心逻辑未改动，仅注入 1 处必要补丁 |
| **依赖链** | FFmpeg、ncnn、libplacebo、glslang、x264 全部交叉编译为 `arm64-v8a` |
| **App 层** | JNI 桥 + 前台服务 + Jetpack Compose UI，与 Video2X 桌面版无耦合 |

它不是命令行工具的壳，而是一个完整的 Android 应用：选片、选算法、看进度、导出的流程都在 App 里完成。

## 功能

**处理算法**

| 算法 | 后端 | 特点 |
|---|---|---|
| **Anime4K** | libplacebo + GLSL（Vulkan） | 着色器增强，**超分算法中速度最快**，适合动画 |
| **Real-ESRGAN** | ncnn（Vulkan） | 神经网络超分，细节最强，速度最慢 |
| **Real-CUGAN** | ncnn（Vulkan） | 神经网络超分，降噪表现好 |
| **RIFE** | ncnn（Vulkan） | 插帧，支持 2× / 4× / 8× |

**三种超分算法同时支持视频与单张图片**；RIFE 仅用于视频（插帧依赖帧间关系，对单张图无意义）。

图片不需要单独的代码通路 —— 上游只提供文件级的 `VideoProcessor`，而 FFmpeg 会把单张图片解复用成
「只有 1 帧的视频流」，因此图片天然能走进同一条管线，只是编码侧换成 PNG。输出统一为 **`.png`（无损）**，
输入支持 png / jpg / webp / bmp / tiff。

**其他**

- **硬件编解码**：MediaCodec（H.264 / H.265），自动协商像素格式与分辨率上限；不适合时可切回软编（libx264 / MPEG-4）
- **动态码率**：按「输入码率 × 输出/输入像素比」推算，并以每像素比特数封顶，避免输入码率高时输出爆掉
- **音频 / 字幕流直拷**：不重编码，无损且零耗时
- **前台服务**：长任务不被系统回收，通知栏显示进度，处理中途退出 App 也能继续
- **日志落盘**：每次任务一个文本日志，含分阶段耗时与帧率采样
- **输出自动进媒体库**：产出后立即出现在相册 / 图库
- **文件选择**：可选相册、第三方文件管理器（如 MT 管理器）

## 性能实测

在 **一加 Ace 5（标准版）**（骁龙 8 Gen 3 · 16 GB RAM）上，竖屏 1440 宽素材的实测结果。数据来自 App 自身落盘的 `[app] progress` / `[app] finished` 日志：

| 算法 | 参数 | 输出分辨率 | 输出帧数 / 总耗时 | 速度 |
|---|---|---|---|---|
| **RIFE** | 插帧 | 1440×2560（同输入） | 1838 帧 / 34.9 s | **~53 fps** |
| **Anime4K** | 超分 x2 | 2880×3840 | 1272 帧 / 156.7 s | ~8.1 fps |
| **Real-ESRGAN** | 超分 x2 | 2880×3840 | 约 5.5 s / 帧 | ~0.18 fps |
| **Real-ESRGAN（图片）** | 超分 x2 | 2072×2712 | **4.5 s / 张** | 其中推理 2.75 s |

几点结论：

- **RIFE 插帧速度最好**：每秒产出 50+ 个输出帧（含插帧帧）
- **Anime4K x2 输出 4 倍像素**，仍有 8 fps，属于可用区间
- **Real-ESRGAN 慢是算法本身决定的**：它跑的是真正的卷积神经网络推理，输出 2880×3840 时单帧 5.5 s 属于手机 GPU 的正常水平，不是实现问题
- **图片超分不需要担心耗时**：同样走 Real-ESRGAN，但只处理一帧（1036×1356 → 2072×2712 约 4.5 秒，含首次模型加载与 PNG 编码）
- 长时间满载会触发 **SoC 热节流**（实测 Anime4K x2 时帧率从 9.5 fps 衰减到 6.1 fps），这是物理限制

> 速度口径为「每秒写入编码器的**输出**帧数」，插帧任务下即插帧后的产出速率。
> 上表是单台设备的实测值，仅供量级参考 —— 换设备后差异可能很大。


## 使用

1. 安装 APK，首次启动会引导授予「**所有文件访问**」权限（Android 11+ 必需，否则无法写公共目录）
2. 选择输入视频 —— 点「选择」会弹出应用列表，相册或第三方文件管理器都可以
3. 选择算法与参数（倍率 / 模型 / 编码器 / 码率）
4. 点「开始」

输出固定写到：

```
/storage/emulated/0/Video2x/
├── 原文件名_算法参数.mp4      ← 成品
└── log/
    └── video2x_20261011_185747.log
```

### 模型：全部内置，开箱即用

**不需要任何手动导入**，APK 里已经包含全部模型：

| 类别 | 内容 | 体积 |
|---|---|---|
| 超分 | Real-ESRGAN（4 组）、Real-CUGAN（18 个组合）、Anime4K 着色器（7 个） | 约 102 MB |
| 插帧 | RIFE v4 系列 5 个模型（`v4` / `v4.25` / `v4.25-lite` / `v4.26` / `v4.6`） | 约 53 MB |

首次启动时会把 assets 中的模型解压到应用私有目录（`filesDir/models`），约需十几秒。

**如果想用上游其他版本的 RIFE 模型**（全部 14 个版本合计约 474 MB，其中 v2 / v3 系列与 HD / UHD 属旧版），
仍可在「**设置 → 补帧模型**」里按需导入 zip，包内结构：

```
rife/<模型名>/flownet.param
rife/<模型名>/flownet.bin
```

导入的模型会与内置模型共存，在主界面的模型下拉里一并列出，也可以在那里删除。

## 体积构成

APK 约 **178 MB**，其中 **约 87% 是模型**：

| 组成 | 大小 | 说明 |
|---|---|---|
| 模型（`assets/models/`） | 154 MB | **不压缩** —— ncnn 权重（`.bin`）本身已接近熵极限，deflate 徒增构建时间 |
| native 库（`lib/arm64-v8a/`） | 62 MB | 压缩存储，APK 内实际约占 33 MB |
| dex（Kotlin/Java 代码） | 1.7 MB | release 构建经 R8 处理后 |

> **关于 R8**：release 构建开启了 R8（压缩 + 混淆 + 资源裁剪），dex 从 53 MiB 降到 **1.7 MiB**，
> 加上排除 debug-only 依赖，相对同内容体积的 debug 包共省下约 **17 MB**。
> 但这对总体积的意义有限 —— 因为大头是模型。要让 APK 显著变小，只能从「是否内置模型」入手。

## 架构

```
┌─────────────────────────────────────────────────────┐
│  Jetpack Compose UI (MainActivity / SettingsScreen)  │
├─────────────────────────────────────────────────────┤
│  ProcessingService  前台服务 · 进度轮询 · 日志落盘    │
├─────────────────────────────────────────────────────┤
│  Video2XNative  ────────────  JNI 桥                 │
│   ├─ 挂 spdlog sink（写文件）                        │
│   ├─ setResourceDir()（告诉上游模型在哪）             │
│   └─ process() / getProcessedFrames() / abort()      │
├─────────────────────────────────────────────────────┤
│  libvideo2x.so（上游核心，1 处补丁）                  │
│   ├─ FFmpeg     解码 / 编码 / 封装 / libplacebo 滤镜  │
│   ├─ ncnn       Real-ESRGAN · Real-CUGAN · RIFE      │
│   └─ libplacebo Anime4K（GLSL，走 FFmpeg avfilter）   │
└─────────────────────────────────────────────────────┘
```

### 对上游代码的改动

`video2x/src/fsutils.cpp` 的 `find_resource()`：上游只探测 `/proc/self/exe` 同目录与 `/usr/share`，在 Android 上全部不成立，会导致所有模型「找不到文件」。补丁增加一个由环境变量 `VIDEO2X_RESOURCE_DIR` 指定的候选路径。

日志、进度、中止、编码参数等全部通过上游公开 API 或 JNI 侧完成，**未改动上游逻辑**。

另外每个依赖库编译时带有若干针对性补丁（含一个 FFmpeg 上游 bug 修复），详见 [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md)。

## 环境要求

| 项 | 要求 |
|---|---|
| 系统 | Android 8.0（API 26）及以上 |
| ABI | **arm64-v8a**（仅此一个） |
| **Vulkan** | **1.1 及以上，必需**。ncnn 与 libplacebo 都依赖它，不支持的设备无法运行 |
| 存储 | 安装约 178 MB；首次启动解压模型后约 333 MB |

## 构建

需要 Linux 环境、Android NDK r27c、CMake、Gradle 8.9、以及 `glslang-tools`。

```bash
# 1. 交叉编译全部依赖（glslang → libplacebo → x264 → FFmpeg → ncnn → spdlog → libvideo2x）
cd scripts
./fetch_sources.sh      # 拉取源码
./build_all.sh          # 编译（耗时较长）

# 2. 放入模型资产
cp -r dist/arm64-v8a/res/. ../app/src/main/assets/models/

# 3. 汇总 native 库到 jniLibs
./prepare_jni_libs.sh

# 4. 构建 APK
cd ..
./gradlew :app:assembleRelease     # 开启 R8（推荐）
# 或 ./gradlew :app:assembleDebug  # 不混淆，便于调试
```

> **构建环境的内存要求**：release 构建时 R8 与 lint 都很吃内存。项目已在
> `gradle.properties` 里把 Gradle daemon 与 Kotlin daemon 的堆调小、并关闭了
> `lintVitalRelease` —— 这是为了适配 8 GB 内存上限的环境。如果你的机器内存充裕，
> 可以把 `org.gradle.jvmargs` 的 `-Xmx` 调回 4g 并重新打开 lint 检查。

**逐步的过程、以及过程中踩到的 18 个坑（每个都有现象 / 原因 / 解决），都在 [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md)。** 这份文档对想自己编译的人价值最大，建议先读它再动手。

## 已知限制

- **测试覆盖非常有限** ⚠️：全部功能（四种算法、图片超分、MediaCodec 硬件编码、模型导入）**仅在一加 Ace 5（标准版）· 骁龙 8 Gen 3 · 16 GB RAM 上完成验证**，未做多机型、大规模的兼容性与稳定性测试。其他设备 —— 尤其是不同 SoC 的 Vulkan 驱动与 MediaCodec 实现 —— 完全可能遇到问题。请把它视为「在单台设备上跑通的移植」，而非成熟产品
- **图片模式不支持多帧格式**：只接受单帧静态图（png / jpg / webp / bmp / tiff）。GIF 这类多帧图片会被拒绝 —— FFmpeg 的 image2 muxer 写第二帧时会直接失败（`Cannot write more than one file with the same name`）
- **图片输出固定为 PNG**：无色深/位深选项，带 alpha 的输入会丢掉透明通道（超分算法本身不处理 alpha）
- **仅 arm64-v8a**，未提供 32 位或 x86
- **强依赖 Vulkan 1.1**：模拟器与部分老旧设备无法运行
- **Real-ESRGAN / Real-CUGAN 速度受限**：这是神经网络推理的固有成本，不是可优化项
- **MediaCodec 兼容性因 SoC 而异**：不同厂商对分辨率上限、像素格式要求不同（已在 App 侧做预检与格式协商，但无法覆盖全部机型）。遇到问题时用 `libx264` 最稳
- **长时间高负载会热节流**，建议避开边充边跑
- 输出目录固定为 `/storage/emulated/0/Video2x/`，暂不可自定义

## 许可证

**GNU Affero General Public License v3.0（AGPL-3.0）** —— 见 [LICENSE](LICENSE)。

本项目链接了 video2x 的 `libvideo2x`，而上游以 AGPL-3.0 发布，因此**本项目整体同样以 AGPL-3.0 分发**。这意味着：

- 你可以自由使用、修改、分发
- 分发时必须**提供完整源码**并保留版权声明
- 若将其作为网络服务提供，同样需要向使用者提供源码

### 第三方组件

| 组件 | 许可证 | 用途 |
|---|---|---|
| [k4yt3x/video2x](https://github.com/k4yt3x/video2x) | AGPL-3.0 | 核心算法库 `libvideo2x` |
| [FFmpeg](https://ffmpeg.org/) | GPL-2.0+（构建时启用了 `--enable-gpl --enable-libx264`） | 解码 / 编码 / 封装 / 滤镜 |
| [x264](https://www.videolan.org/developers/x264.html) | GPL-2.0+ | H.264 软编码 |
| [ncnn](https://github.com/Tencent/ncnn) | BSD-3-Clause | 神经网络推理 |
| [libplacebo](https://code.videolan.org/videolan/libplacebo) | LGPL-2.1+ | Anime4K 着色器渲染 |
| [glslang](https://github.com/KhronosGroup/glslang) | Apache-2.0 / BSD | GLSL 编译 |
| [spdlog](https://github.com/gabime/spdlog) | MIT | 日志 |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | Apache-2.0 / MIT | Vulkan 头文件 |
| AndroidX / Jetpack Compose | Apache-2.0 | UI 框架 |

各算法模型（Real-ESRGAN / Real-CUGAN / RIFE / Anime4K）版权归各自作者所有，遵循其原始许可。

## 致谢

- [video2x](https://github.com/k4yt3x/video2x) —— 本项目的一切都建立在它之上
- [ncnn](https://github.com/Tencent/ncnn)、[libplacebo](https://code.videolan.org/videolan/libplacebo)、[FFmpeg](https://ffmpeg.org/) 及其社区

## 免责声明

本项目仅供学习与技术研究使用。请遵守你所在地区的法律法规，不要用于处理你无权处理的素材。
