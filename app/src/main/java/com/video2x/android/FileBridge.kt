package com.video2x.android

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.OpenableColumns
import android.provider.Settings
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * 文件路径处理。
 *
 * 输出策略：固定写到公共目录 /storage/emulated/0/Video2x/，
 * 文件名 = 原文件名 + 算法参数（需要 MANAGE_EXTERNAL_STORAGE）。
 * 输入仍由 SAF 选择，先拷贝到应用私有目录再交给 native。
 */
object FileBridge {

    /** 默认输出目录（用户可见的公共目录） */
    const val OUTPUT_DIR = "/storage/emulated/0/Video2x"

    /** 日志目录 */
    const val LOG_DIR = "$OUTPUT_DIR/log"

    private const val TMP_DIR = "tmp"

    fun workDir(context: Context): File =
        File(context.filesDir, TMP_DIR).apply { mkdirs() }

    fun outputDir(): File = File(OUTPUT_DIR).apply { mkdirs() }

    fun logDir(): File = File(LOG_DIR).apply { mkdirs() }

    /** 生成带时间戳的日志文件路径：/storage/emulated/0/Video2x/log/video2x_<时间>.log */
    fun newLogFilePath(): String {
        logDir()  // 确保目录存在，否则 native 侧打开文件会失败
        val ts = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
        return "$LOG_DIR/video2x_$ts.log"
    }

    // ------------------------------------------------------------ 权限

    /** 是否已获得「所有文件访问」权限（Android 11+ 需要） */
    fun hasOutputPermission(): Boolean {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            Environment.isExternalStorageManager()
        } else {
            true
        }
    }

    /** 跳转到「所有文件访问」授权页 */
    fun permissionIntent(context: Context): Intent? {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return null
        return Intent(
            Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
            Uri.parse("package:${context.packageName}"),
        )
    }

    // ------------------------------------------------------------ 输入

    /** 读取用户选择输入文件的显示名 */
    fun displayName(context: Context, uri: Uri): String? {
        return try {
            context.contentResolver.query(uri, null, null, null, null)?.use { c ->
                val idx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (idx >= 0 && c.moveToFirst()) c.getString(idx) else null
            }
        } catch (_: Exception) {
            null
        }
    }

    /** 将输入 URI 拷贝到私有工作目录，返回本地文件 */
    fun importInput(context: Context, uri: Uri): File {
        val name = displayName(context, uri) ?: "input.mp4"
        var ext = name.substringAfterLast('.', "").lowercase(Locale.US)

        // 某些 provider 的显示名不带扩展名（或就是个 UUID）—— 此时用 MIME 兜底。
        // native 侧依赖扩展名区分「图片 / 视频」并据此选择编码器，
        // 扩展名错了会被当成视频，用 mp4 编码器去处理单帧图片而失败。
        if (ext.isEmpty()) {
            ext = extFromMime(context.contentResolver.getType(uri))
        }
        if (ext.isEmpty()) {
            ext = "mp4"
        }

        val dst = File(workDir(context), "input.$ext")
        if (dst.exists()) dst.delete()

        context.contentResolver.openInputStream(uri).use { input ->
            requireNotNull(input) { "无法打开输入文件" }
            dst.outputStream().use { output -> input.copyTo(output, bufferSize = 1 shl 16) }
        }
        return dst
    }

    /** MIME → 扩展名。只覆盖 native 侧需要区分的类型。 */
    private fun extFromMime(mime: String?): String = when (mime) {
        "image/png" -> "png"
        "image/jpeg" -> "jpg"
        "image/webp" -> "webp"
        "image/bmp", "image/x-ms-bmp" -> "bmp"
        "image/tiff" -> "tiff"
        "video/mp4" -> "mp4"
        "video/x-matroska" -> "mkv"
        "video/webm" -> "webm"
        "video/quicktime" -> "mov"
        else -> ""
    }

    /**
     * 文件名是否为单帧静态图片。
     * 与 native 侧 video2x_jni.cpp 的 is_image_file() 保持一致的集合 ——
     * 刻意排除 gif：多帧输入会让 image2 muxer 在写第二帧时直接失败。
     */
    fun isImageName(name: String?): Boolean {
        val ext = name?.substringAfterLast('.', "")?.lowercase(Locale.US) ?: return false
        return ext in setOf("png", "jpg", "jpeg", "webp", "bmp", "tif", "tiff")
    }

    // ------------------------------------------------------------ 输出

    /**
     * 生成默认输出绝对路径：原文件名 + 算法参数。
     *
     * 例如 video.mp4 + RealESRGAN(animevideov3, x4)
     *   → /storage/emulated/0/Video2x/video_realesrgan-animevideov3-x4.mp4
     *
     * 图片输入则统一输出 `.png`（无损，且与 native 侧选用的 png 编码器一致）——
     * 若沿用输入扩展名（如 .jpg），会出现「文件名是 jpg、内容实际是 PNG」的错配。
     */
    @Suppress("LongParameterList")
    fun buildOutputPath(
        inputName: String,
        algo: Algo,
        model: String,
        scale: Int,
        width: Int,
        height: Int,
        frameMultiplier: Int,
        shader: String,
        isImage: Boolean = false,
    ): String {
        val base = inputName.substringBeforeLast('.', inputName)
        val ext = if (isImage) {
            "png"
        } else {
            inputName.substringAfterLast('.', "mp4").ifBlank { "mp4" }
        }

        val suffix = when (algo) {
            Algo.REALESRGAN -> "realesrgan-$model-x$scale"
            Algo.REALCUGAN -> "realcugan-$model-x$scale"
            // Anime4K 的目标尺寸通常由倍率自动推算（此时 width/height 为 0），
            // 文件名里就退化为倍率标识；若用户手填了尺寸则照实写入。
            Algo.ANIME4K -> if (width > 0 && height > 0) {
                "anime4k-$shader-${width}x${height}"
            } else {
                "anime4k-$shader-x$scale"
            }
            Algo.RIFE -> "rife-$model-${frameMultiplier}x"
        }

        return "$OUTPUT_DIR/${base}_$suffix.$ext"
    }

    /** 清理临时文件 */
    fun cleanup(context: Context) {
        workDir(context).listFiles()?.forEach { it.delete() }
    }
}
