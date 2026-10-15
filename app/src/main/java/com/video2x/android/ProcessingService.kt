package com.video2x.android

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.BitmapFactory
import android.media.MediaExtractor
import android.media.MediaFormat
import android.media.MediaMetadataRetriever
import android.media.MediaScannerConnection
import android.net.Uri
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import java.io.File
import java.util.Locale

/** 一次处理请求的全部参数 */
data class ProcessRequest(
    /** 输入文件的内容 URI（由 SAF 选择） */
    val inUri: String,
    /** 输出文件的**绝对路径**（如 /storage/emulated/0/Video2x/xxx.mp4） */
    val outPath: String,
    /** 本次任务的日志文件路径（应用侧也会追加少量诊断信息） */
    val logPath: String,
    val algo: Algo,
    val model: String,
    val scale: Int,
    /** Real-CUGAN 的降噪等级（-1=保守，0=无降噪，1..3=降噪Nx）；其余算法传 -1 */
    val noiseLevel: Int,
    val width: Int,
    val height: Int,
    val frameMultiplier: Int,
    val shader: String,
    val codec: String,
    val bitRate: Long,
    /**
     * 旋转 / 翻转位掩码（仅视频）：0 = 不旋转（走超分）；非 0 走纯软件旋转管线。
     * 1=顺时针90°  2=逆时针90°  4=水平翻转  8=垂直翻转，可自由组合。
     */
    val transform: Int = 0,
    /**
     * 输入是否为单帧静态图片。
     * 图片与视频走同一条 native 管线（FFmpeg 把图片解复用为只有 1 帧的视频流），
     * 但对目标尺寸的探测方式、以及是否需要码率/硬件编码预检都不同。
     */
    val isImage: Boolean = false,
)

/** 供 UI 观察的运行状态 */
data class ProcessStatus(
    val running: Boolean = false,
    val progress: Float = 0f,
    val processed: Long = 0L,
    val total: Long = 0L,
    val message: String = "空闲",
    val done: Boolean = false,
    val success: Boolean = false,
)

/** 进程内共享的状态总线（个人使用场景，单任务模型） */
object ProcessBus {
    val status = MutableStateFlow(ProcessStatus())

    @Volatile
    var request: ProcessRequest? = null
}

class ProcessingService : Service() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    /**
     * 去重判断由 Service 自己持有的 job 决定。
     * 注意：不能用 ProcessBus.status.running —— 那会被 ViewModel 提前置位，
     * 导致服务误判为「已有任务」而直接退出（曾导致卡在「启动中」）。
     */
    private var job: Job? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        createChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_ABORT) {
            Video2XNative.abort()
            return START_NOT_STICKY
        }

        startForegroundCompat("准备中…")

        if (job?.isActive == true) {
            // 已有任务在执行，忽略本次启动
            return START_NOT_STICKY
        }

        val req = ProcessBus.request
        if (req == null) {
            stopSelf()
            return START_NOT_STICKY
        }

        job = scope.launch { runTask(req) }
        return START_NOT_STICKY
    }

    private suspend fun runTask(req: ProcessRequest) = coroutineScope {
        val ctx: Context = applicationContext

        fun publish(s: ProcessStatus) {
            ProcessBus.status.value = s
            updateNotification(s.message)
        }

        try {
            publish(
                ProcessStatus(
                    running = true,
                    message = if (req.isImage) "导入输入图片…" else "导入输入视频…",
                )
            )
            val inFile = FileBridge.importInput(ctx, Uri.parse(req.inUri))

            // --- 输入尺寸探测 -------------------------------------------------
            //  · 视频：MediaExtractor / MediaMetadataRetriever
            //  · 图片：BitmapFactory 只读文件头（inJustDecodeBounds=true），不解码像素
            //
            // 分辨率预检（下方）只对硬件编码器有意义：MediaCodec 对分辨率存在硬上限
            //（常见 4096），超出后 configure 直接失败并返回 -10000。图片走 PNG
            // 编码器，没有这个限制，所以探到尺寸后不做拦截。
            val videoInfo = if (req.isImage) null else probeVideoInfo(inFile)

            val inW: Int
            val inH: Int
            if (req.isImage) {
                val size = probeImageSize(inFile)
                inW = size.first
                inH = size.second
            } else {
                inW = videoInfo!!.width
                inH = videoInfo!!.height
            }
            val srcBitRate = videoInfo?.bitRate ?: 0L

            // 解析目标尺寸：
            //  · 手动值优先 —— 需宽高同时 > 0 才生效
            //  · 否则按「输入分辨率 × 倍率」自动推算
            //  · 视频编码要求偶数尺寸（NV12 / YUV420P），向下对齐到偶数；
            //    图片走 RGB24 无此限制，但一并对齐，保持行为一致
            val scaleFactor = if (req.scale > 0) req.scale else 1
            val manualOk = req.width > 0 && req.height > 0
            val outW = if (manualOk) req.width and -2 else (inW * scaleFactor) and -2
            val outH = if (manualOk) req.height and -2 else (inH * scaleFactor) and -2

            // ncnn 系列的目标尺寸由模型倍率决定，无需显式传宽高；
            // libplacebo（Anime4K）则必须给出明确的目标尺寸。
            val nativeW = if (req.algo.needsDimensions) outW else 0
            val nativeH = if (req.algo.needsDimensions) outH else 0

            // ---- 码率解析 ----
            // 图片输出是 PNG（无损），没有码率概念；视频则：
            //  · 用户填 > 0           → 用用户值
            //  · 填 0 + 硬件编码      → 按「输入码率 × 输出/输入像素比」推算。
            //    MediaCodec 必须拿到正码率，无法像 libx264 那样走"自动"。
            //  · 填 0 + 软编码        → 传 0，交给 libx264 的默认 CRF（质量优先）
            val autoBitRate =
                if (req.isImage) 0L
                else autoBitRateFor(srcBitRate, inW, inH, outW, outH)
            val effectiveBitRate = when {
                req.isImage -> 0L
                req.bitRate > 0 -> req.bitRate
                req.codec.contains("mediacodec") -> autoBitRate
                else -> 0L
            }

            appendLog(
                req.logPath,
                "[app] mode=${if (req.isImage) "image" else "video"} " +
                    "input=${inW}x$inH output=${outW}x$outH manual=$manualOk " +
                    "codec=${if (req.isImage) "png" else req.codec} scale=$scaleFactor " +
                    "src_bitrate=$srcBitRate out_bitrate=$effectiveBitRate",
            )

            if (!req.isImage &&
                req.codec.contains("mediacodec") && outW > HW_MAX_DIM && outH > 0
            ) {
                val msg = "输出 ${outW}x$outH 超出硬件编码器上限(${HW_MAX_DIM})，" +
                    "请改用 libx264 或降低倍率"
                appendLog(req.logPath, "[app] aborted: $msg")
                publish(
                    ProcessStatus(running = false, done = true, success = false, message = msg)
                )
                return@coroutineScope
            }

            val outFile = File(req.outPath)
            outFile.parentFile?.mkdirs()
            if (outFile.exists()) outFile.delete()

            publish(ProcessStatus(running = true, message = "初始化模型…"))

            val worker = async(Dispatchers.IO) {
                if (req.transform != 0) {
                    // 旋转 / 翻转：纯软件编码（libx264），规避 GPU 卡死
                    Video2XNative.rotateVideo(
                        inFile.absolutePath,
                        outFile.absolutePath,
                        req.transform,
                        "libx264",
                        if (req.bitRate > 0) req.bitRate else 8_000_000L,
                    )
                } else {
                    Video2XNative.process(
                        inFile.absolutePath,
                        outFile.absolutePath,
                        req.algo.id,
                        req.model,
                        req.scale,
                        req.noiseLevel,
                        nativeW,
                        nativeH,
                        req.frameMultiplier,
                        req.shader,
                        req.codec,
                        effectiveBitRate,
                    )
                }
            }

            // 处理期间周期性落盘：每 10 秒一行，含瞬时帧率与 ETA。
            // Anime4K 走 libplacebo，没有 native 侧计时，靠这里评估速度。
            val startAt = System.currentTimeMillis()
            var lastLogAt = startAt
            var lastFrames = 0L
            // native 侧在任务结束后会清空计数器，故在这里留存最后一次读到的值，
            // 供 finished 行使用（否则收尾时读到的永远是 0）。
            var finalFrames = 0L
            var finalTotal = 0L

            while (worker.isActive) {
                val p = Video2XNative.getProcessedFrames()
                val t = Video2XNative.getTotalFrames()
                if (p > 0) finalFrames = p
                if (t > 0) finalTotal = t
                val frac = if (t > 0) p.toFloat() / t.toFloat() else 0f
                publish(
                    ProcessStatus(
                        running = true,
                        progress = frac.coerceIn(0f, 1f),
                        processed = p,
                        total = t,
                        message = if (t > 0) {
                            "处理中 $p / $t 帧（${(frac * 100).toInt()}%）"
                        } else {
                            "处理中…"
                        },
                    )
                )

                val now = System.currentTimeMillis()
                if (now - lastLogAt >= 10_000L) {
                    val deltaSec = (now - lastLogAt) / 1000.0
                    val elapsedSec = (now - startAt) / 1000.0
                    val instFps = if (deltaSec > 0) (p - lastFrames) / deltaSec else 0.0
                    val avgFps = if (elapsedSec > 0) p / elapsedSec else 0.0
                    val etaSec = if (avgFps > 0 && t > p) (t - p) / avgFps else 0.0
                    appendLog(
                        req.logPath,
                        "[app] progress $p/$t (${(frac * 100).toInt()}%) " +
                            "elapsed=" + fmt1(elapsedSec) + "s " +
                            "fps=" + fmt2(instFps) + " avg=" + fmt2(avgFps) +
                            " eta=" + fmt1(etaSec) + "s",
                    )
                    lastLogAt = now
                    lastFrames = p
                }
                delay(500)
            }

            val ret = worker.await()

            val frameInfo = if (finalTotal > 0) "$finalFrames/$finalTotal" else "$finalFrames"
            appendLog(
                req.logPath,
                "[app] finished ret=$ret total=" +
                    fmt1((System.currentTimeMillis() - startAt) / 1000.0) +
                    "s frames=$frameInfo",
            )
            if (ret == 0) {
                // 通知媒体库刷新：输出目录是公共目录，但只写入文件并不会让
                // 相册/图库立刻收录它，必须主动触发 MediaScanner。
                notifyMediaScanner(outFile)
                publish(
                    ProcessStatus(
                        running = false, done = true, success = true, progress = 1f,
                        message = "完成：${outFile.name}",
                    )
                )
            } else {
                publish(
                    ProcessStatus(
                        running = false, done = true, success = false,
                        progress = ProcessBus.status.value.progress,
                        message = "失败（错误码 $ret），详情见日志文件",
                    )
                )
            }
        } catch (t: Throwable) {
            publish(
                ProcessStatus(
                    running = false, done = true, success = false,
                    message = "异常：${t.javaClass.simpleName}: ${t.message}",
                )
            )
        } finally {
            ProcessBus.request = null
            delay(1200)
            stopForegroundCompat()
            stopSelf()
        }
    }

    // ---------------------------------------------------------------- 通知

    private fun createChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val mgr = getSystemService(NotificationManager::class.java)
            if (mgr.getNotificationChannel(CHANNEL_ID) == null) {
                val ch = NotificationChannel(
                    CHANNEL_ID,
                    getString(R.string.channel_name),
                    NotificationManager.IMPORTANCE_LOW,
                ).apply { description = getString(R.string.channel_desc) }
                mgr.createNotificationChannel(ch)
            }
        }
    }

    private fun buildNotification(text: String): Notification {
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_sys_upload)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .build()
    }

    private fun updateNotification(text: String) {
        val mgr = getSystemService(NotificationManager::class.java)
        mgr.notify(NOTIF_ID, buildNotification(text))
    }

    /** API 29+ 起需显式声明前台服务类型（Android 14 起更严格） */
    private fun startForegroundCompat(text: String) {
        val n = buildNotification(text)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
        } else {
            startForeground(NOTIF_ID, n)
        }
    }

    private fun stopForegroundCompat() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            stopForeground(STOP_FOREGROUND_REMOVE)
        } else {
            @Suppress("DEPRECATION")
            stopForeground(true)
        }
    }

    // ------------------------------------------------------------ 辅助

    /** 输入视频探测结果 */
    private data class VideoInfo(val width: Int, val height: Int, val bitRate: Long)

    /**
     * 读取输入视频的宽高与码率，任一失败取 0。
     *
     * 码率优先用 MediaExtractor 读视频轨自身的 KEY_BIT_RATE（MKV/WebM 通常有）；
     * MP4 容器一般不写该字段，此时退化为 METADATA_KEY_BITRATE ——
     * 那是 Android 按「文件大小 / 时长」估算的整体码率（含音频），用于推算足够。
     */
    /**
     * 读取图片尺寸。
     *
     * 用 BitmapFactory 只解析文件头（inJustDecodeBounds = true），不会真正解码像素，
     * 对几千像素的大图也是毫秒级。失败返回 (0, 0)。
     */
    private fun probeImageSize(file: File): Pair<Int, Int> {
        return try {
            val opts = BitmapFactory.Options().apply { inJustDecodeBounds = true }
            BitmapFactory.decodeFile(file.absolutePath, opts)
            if (opts.outWidth > 0 && opts.outHeight > 0) {
                opts.outWidth to opts.outHeight
            } else {
                0 to 0
            }
        } catch (_: Throwable) {
            0 to 0
        }
    }

    private fun probeVideoInfo(file: File): VideoInfo {
        var w = 0
        var h = 0
        var br = 0L

        try {
            val ex = MediaExtractor()
            ex.setDataSource(file.absolutePath)
            for (i in 0 until ex.trackCount) {
                val f = ex.getTrackFormat(i)
                val mime = f.getString(MediaFormat.KEY_MIME) ?: continue
                if (!mime.startsWith("video/")) continue
                if (f.containsKey(MediaFormat.KEY_WIDTH)) w = f.getInteger(MediaFormat.KEY_WIDTH)
                if (f.containsKey(MediaFormat.KEY_HEIGHT)) h = f.getInteger(MediaFormat.KEY_HEIGHT)
                if (f.containsKey(MediaFormat.KEY_BIT_RATE)) {
                    br = f.getInteger(MediaFormat.KEY_BIT_RATE).toLong()
                }
                break
            }
            ex.release()
        } catch (_: Throwable) {
            // 忽略，交给下面的回退
        }

        if (w <= 0 || h <= 0 || br <= 0) {
            try {
                val mmr = MediaMetadataRetriever()
                mmr.setDataSource(file.absolutePath)
                if (w <= 0) {
                    w = mmr.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH)
                        ?.toIntOrNull() ?: 0
                }
                if (h <= 0) {
                    h = mmr.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT)
                        ?.toIntOrNull() ?: 0
                }
                if (br <= 0) {
                    br = mmr.extractMetadata(MediaMetadataRetriever.METADATA_KEY_BITRATE)
                        ?.toLongOrNull() ?: 0L
                }
                mmr.release()
            } catch (_: Exception) {
                // 保持 0
            }
        }

        return VideoInfo(w, h, br)
    }

    /**
     * 推算输出码率。
     *
     * 首选「输入码率 × 输出/输入像素比」—— 超分后像素数按倍率平方增长，
     * 码率同比例放大才能维持单位像素质量；而 MediaCodec 又必须拿到正码率。
     * 读不到输入码率时退化为按输出像素的经验值：约 0.08 bit/像素/帧
     * （1080p30 ≈ 5 Mbps，2880x3840@30 ≈ 26 Mbps）。
     */
    private fun autoBitRateFor(srcBr: Long, inW: Int, inH: Int, outW: Int, outH: Int): Long {
        val outPx = outW.toLong() * outH
        if (outPx <= 0) return 0L

        // 用「每像素每帧比特数」给输出封顶。
        // 只按像素比外推的话，输入本身是高码率时结果会离谱 ——
        // 实测一例：输入 28.8 Mbps(1440x1920)，x2 按 4 倍算得 115 Mbps，
        // 远超手机编码器的合理区间，文件也大到不可用。
        // 0.15 bit/px/frame 大致对应 H.265 的高质量档。
        val capByBpp = (outPx * ASSUMED_FPS * MAX_BITS_PER_PIXEL).toLong()

        val est = if (srcBr > 0 && inW > 0 && inH > 0) {
            val ratio = outPx.toDouble() / (inW.toDouble() * inH.toDouble())
            minOf((srcBr * ratio).toLong(), capByBpp)
        } else {
            (outPx * ASSUMED_FPS * TARGET_BITS_PER_PIXEL).toLong()
        }
        return est.coerceIn(MIN_AUTO_BITRATE, MAX_AUTO_BITRATE)
    }

    /** 固定用 US locale 格式化，避免某些系统用逗号作小数分隔符 */
    private fun fmt1(v: Double): String = String.format(Locale.US, "%.1f", v)

    private fun fmt2(v: Double): String = String.format(Locale.US, "%.2f", v)

    /**
     * 通知系统媒体库刷新，让新产出的视频立刻出现在相册 / 图库里。
     * 失败不影响主流程。
     */
    private fun notifyMediaScanner(file: File) {
        try {
            val mime = when (file.extension.lowercase(Locale.US)) {
                "mp4", "m4v" -> "video/mp4"
                "mkv" -> "video/x-matroska"
                "webm" -> "video/webm"
                "mov" -> "video/quicktime"
                "avi" -> "video/x-msvideo"
                // 图片超分的产物（统一 PNG，见 FileBridge.buildOutputPath）
                "png" -> "image/png"
                "jpg", "jpeg" -> "image/jpeg"
                "webp" -> "image/webp"
                "bmp" -> "image/bmp"
                "tif", "tiff" -> "image/tiff"
                else -> "video/*"
            }
            MediaScannerConnection.scanFile(
                applicationContext,
                arrayOf(file.absolutePath),
                arrayOf(mime),
                null,
            )
        } catch (_: Throwable) {
            // 忽略
        }
    }

    /** 向日志文件追加一行应用侧诊断信息 */
    private fun appendLog(path: String, msg: String) {
        try {
            File(path).appendText(msg + "\n")
        } catch (_: Exception) {
            // 日志写入失败不影响主流程
        }
    }

    companion object {
        const val ACTION_ABORT = "com.video2x.android.action.ABORT"
        private const val CHANNEL_ID = "video2x_process"
        private const val NOTIF_ID = 1001

        /** 硬件编码器的常见分辨率上限；超出后 MediaCodec 会 configure 失败(-10000) */
        private const val HW_MAX_DIM = 4096

        /** 自动码率的下限与绝对上限（绝对上限兜住异常输入） */
        private const val MIN_AUTO_BITRATE = 2_000_000L
        private const val MAX_AUTO_BITRATE = 60_000_000L

        /** 无输入码率时的经验目标值，以及按比例外推时的封顶值（单位 bit/像素/帧） */
        private const val ASSUMED_FPS = 30.0
        private const val TARGET_BITS_PER_PIXEL = 0.08
        private const val MAX_BITS_PER_PIXEL = 0.15
    }
}
