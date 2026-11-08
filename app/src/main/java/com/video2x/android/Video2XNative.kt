package com.video2x.android

/**
 * JNI 绑定层。
 *
 * native 实现见 app/src/main/cpp/video2x_jni.cpp。
 * 方法名、参数顺序、类型必须与 C++ 侧的 JNIEXPORT 声明严格一致。
 */
object Video2XNative {

    init {
        System.loadLibrary("video2x_jni")
    }

    // 与 C++ 侧 map_algo() 保持一致
    const val ALGO_ANIME4K = 1
    const val ALGO_REALESRGAN = 2
    const val ALGO_REALCUGAN = 3
    const val ALGO_RIFE = 4

    // 与上游 video2x::VideoProcessorState 保持一致
    const val STATE_IDLE = 0
    const val STATE_RUNNING = 1
    const val STATE_PAUSED = 2
    const val STATE_FAILED = 3
    const val STATE_ABORTED = 4
    const val STATE_COMPLETED = 5

    /** 挂载 logcat 日志 sink（走上游公开 API，零侵入） */
    external fun initLogger(tag: String)

    external fun setLogLevel(level: String)

    /**
     * 把日志输出切换到文本文件（父目录需已存在且可写）。
     * @return 是否切换成功
     */
    external fun setLogFile(path: String): Boolean

    /** 设定模型 / 着色器根目录（绝对路径），供上游 find_resource 使用 */
    external fun setResourceDir(dir: String)

    /**
     * 探测本机 Vulkan 是否满足 Anime4K（libplacebo）的要求。
     *
     * ffmpeg 7.x 的 Vulkan 硬件上下文把 timeline semaphore 当作硬性要求，
     * 部分老设备（如骁龙865 的早期 Adreno 驱动）没有该扩展，libplacebo
     * 滤镜必然初始化失败。结果在 native 侧按进程缓存。
     *
     * @return 0 表示可用；负数表示不可用（FFmpeg AVERROR）
     */
    external fun probeVulkan(): Int

    /**
     * Anime4K 直连管线（Vulkan 1.1，不经过 ffmpeg 的 libplacebo 滤镜）。
     *
     * 用于 Vulkan 1.2 特性缺失的设备（如骁龙 865 的 Adreno 650 驱动）：
     * libplacebo 硬性要求 timelineSemaphore / hostQueryReset，这类机型上必然
     * 初始化失败，所以改由 App 自己执行 Anime4K 的 shader 链。
     *
     * @param shaderPath 已解压的 .glsl 绝对路径
     * @return 0 成功；-2 用户中止；负数失败
     */
    external fun processAnime4K(
        inPath: String,
        outPath: String,
        shaderPath: String,
        outW: Int,
        outH: Int,
        codec: String,
        bitRate: Long,
        isImage: Boolean,
    ): Int

    /**
     * 执行处理。阻塞调用，必须放在工作线程。
     *
     * @return 0 成功；负数错误码（FFmpeg AVERROR 或 -1 表示参数/异常错误）
     */
    external fun process(
        inPath: String,
        outPath: String,
        algo: Int,
        model: String,
        scale: Int,
        noiseLevel: Int,
        width: Int,
        height: Int,
        frmRateMul: Int,
        shader: String,
        codec: String,
        bitRate: Long,
    ): Int

    /**
     * 视频旋转 / 翻转（纯软件路径：libavfilter + libx264，不经过 libplacebo / Vulkan，
     * 因此不会触发横屏超分场景下的 GPU 卡死）。
     *
     * @param transform 位掩码：1=顺时针90°  2=逆时针90°  4=水平翻转  8=垂直翻转，可组合
     * @param codec     视频编码器，旋转场景固定使用 libx264（软件编码）
     * @param bitRate   视频码率（bit/s），<=0 时 native 侧回退到 8Mbps 默认值
     * @return 0 成功；-2 用户中止；负数对应 FFmpeg AVERROR 或 -1 表示参数/异常错误
     */
    external fun rotateVideo(
        inPath: String,
        outPath: String,
        transform: Int,
        codec: String,
        bitRate: Long,
    ): Int

    external fun getProcessedFrames(): Long

    external fun getTotalFrames(): Long

    /** 返回上游 VideoProcessorState；-1 表示当前无任务 */
    external fun getState(): Int

    external fun abort()
}
