package com.video2x.android

import android.app.Application
import android.util.Log

/** native 库加载状态，供 UI 显示诊断信息 */
object NativeState {
    @Volatile
    var ready: Boolean = false

    @Volatile
    var error: String? = null
}

class Video2XApp : Application() {

    override fun onCreate() {
        super.onCreate()
        try {
            // 挂载 logcat 日志。之后 native 侧与 FFmpeg 的日志都会出现在 logcat 中。
            Video2XNative.initLogger("Video2X")
            Video2XNative.setLogLevel("info")
            NativeState.ready = true
        } catch (t: Throwable) {
            // 例如设备缺少 Vulkan，或 native 库加载失败
            NativeState.error = "${t.javaClass.simpleName}: ${t.message}"
            Log.e(TAG, "native 初始化失败", t)
        }
    }

    private companion object {
        const val TAG = "Video2X"
    }
}
