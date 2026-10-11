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

    external fun getProcessedFrames(): Long

    external fun getTotalFrames(): Long

    /** 返回上游 VideoProcessorState；-1 表示当前无任务 */
    external fun getState(): Int

    external fun abort()
}
