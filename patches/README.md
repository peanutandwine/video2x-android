# 上游补丁说明

本移植**尽量不改动**上游源码。目前共 **2 处**必要改动：

1. [k4yt3x/video2x](https://github.com/k4yt3x/video2x) —— `src/fsutils.cpp` 的资源查找
2. [FFmpeg](https://ffmpeg.org/) —— `libavcodec/mediacodecenc.c` 的未初始化栈缓冲区

两处都由 `scripts/apply_patches.sh` **幂等注入**（字符串锚点匹配，不依赖行号），可重复执行。

---

## 改动 1：`video2x/src/fsutils.cpp` 的 `find_resource()`

**问题：** 上游 `find_resource()` 依次探测以下位置的模型/着色器文件：

1. 传入的路径原样
2. `$APPDIR/usr/share/video2x/...`（AppImage）
3. `/usr/local/share/video2x/...`
4. `/usr/share/video2x/...`
5. `/proc/self/exe` 所在目录

在 Android 上，2/3/4 不存在，5 解析为 `/system/bin`（无意义），1 是相对路径（进程 CWD 为 `/`，不可读）。结果是 **RealESRGAN / RealCUGAN / RIFE 模型与 Anime4K 着色器全部「找不到文件」**。

**解决：** 在候选列表中插入一项——由环境变量 `VIDEO2X_RESOURCE_DIR` 指定的目录。JNI 层（`video2x_jni.cpp` 的 `setResourceDir`）会在启动时把它设为 App 私有目录的**上一级**（`filesDir`），使拼接结果为 `filesDir/models/realesrgan/...`。

> 注意：`setResourceDir` 必须传 `filesDir` 而不是 `filesDir/models`，因为上游传入的 `resource` 已经带 `models/` 前缀。多一层会拼成 `models/models/...`。

差分如下：

```diff
     // 1. The resource's path as provided
     candidates.push_back(resource);
 
+#ifndef _WIN32
+    // Android: explicit resource root supplied by the embedding application.
+    // The JNI layer sets VIDEO2X_RESOURCE_DIR to the app's private models dir.
+    if (const char* resdir = std::getenv("VIDEO2X_RESOURCE_DIR")) {
+        candidates.push_back(std::filesystem::path(resdir) / resource);
+    }
+#endif
+
 #ifndef _WIN32
     // 2. AppImage's mounted directory
```

---

## 改动 2：`ffmpeg/libavcodec/mediacodecenc.c` 的 `mediacodec_init_bsf()`

**问题：** 该函数中 `char str[128];` **未初始化**：

```c
static int mediacodec_init_bsf(AVCodecContext *avctx)
{
    MediaCodecEncContext *s = avctx->priv_data;
    char str[128];                     // ← 未初始化
    int ret;
    int crop_right  = s->width  - avctx->width;
    int crop_bottom = s->height - avctx->height;

    if (!crop_right && !crop_bottom && !s->extract_extradata)
        return 0;

    ret = 0;
    if (crop_right || crop_bottom) {
        snprintf(str, sizeof(str), "h264_metadata=crop_right=%d:crop_bottom=%d", ...);
        // 只有走这个分支时 str 才被初始化
    }

    if (s->extract_extradata) {
        av_strlcatf(str, sizeof(str), "%sextract_extradata", ret ? "," : "");
        // ← crop==0 时对未初始化缓冲区做追加，str 变成垃圾串
    }

    ret = av_bsf_list_parse_str(str, &s->bsf);   // ← 解析失败，返回 AVERROR_BSF_NOT_FOUND
```

触发条件正好是 Android 上的常见组合：

| 条件 | 来源 | 实际取值 |
|---|---|---|
| `extract_extradata == true` | mp4 输出的 muxer 带 `AVFMT_GLOBALHEADER`，video2x 据此置上 `AV_CODEC_FLAG_GLOBAL_HEADER`（`encoder.cpp:169-171`） | 真 |
| `crop_right == 0 && crop_bottom == 0` | 输出尺寸已是 16 的倍数（如 2880×3840） | 真 |

两者叠加 → 走「跳过 `snprintf`、直接对未初始化缓冲区追加」的路径 → 编码器初始化失败，报 **`Bitstream filter not found`**（即 `AVERROR_BSF_NOT_FOUND`）。

**解决：** 显式零初始化：

```diff
     MediaCodecEncContext *s = avctx->priv_data;
-    char str[128];
+    char str[128] = { 0 };
     int ret;
```

此时 `str` 为合法空串，`av_strlcatf` 得到 `"extract_extradata"`，解析正常。**不影响 `crop != 0` 的既有路径。**

> 该 bug 依赖栈垃圾内容，属于「间歇性」故障；`str` 恰好为 0 时不会触发，这也是它容易被忽略的原因。

---

## 为什么其他功能都不需要改上游

| 功能 | 实现方式 | 是否需要改上游 |
|---|---|---|
| 日志写文件 | JNI 层自研 `spdlog` sink（`base_sink` 纯头模板 + `ofstream`），通过公开 API `logger_manager::LoggerManager::reconfigure_logger(sinks)` 挂载，输出到 `/storage/emulated/0/Video2x/log/` | 否 |
| FFmpeg 日志 | `hook_ffmpeg_logging()`（同样是公开 API），再经上面的 sink 落盘 | 否 |
| 进度 / 中止 | `get_processed_frames()` / `get_total_frames()` / `abort()`（公开 API） | 否 |
| 算法选择 / 参数 | 构造 `ProcessorConfig` / `EncoderConfig` | 否 |
| 编码器像素格式 | `EncoderConfig::pix_fmt` 显式指定（MediaCodec → `NV12`，软编 → `YUV420P`） | 否 |
| 文件输入输出 | `process(in_path, out_path)` 文件级 API + JNI 侧目录拷贝 | 否 |

> 升级上游后若锚点结构变化，`apply_patches.sh` 会报「未找到锚点」并以非 0 退出，届时按上面的 diff 手工处理即可。
