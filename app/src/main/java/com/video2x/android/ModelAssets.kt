package com.video2x.android

import android.content.Context
import java.io.File

/**
 * 把打包在 assets/models 下的模型与着色器解压到应用私有目录。
 *
 * 上游 find_resource() 期望的目录结构（相对资源根）：
 *   models/realesrgan/<name>.param|.bin
 *   models/realcugan/<variant>/<name>.param|.bin
 *   models/libplacebo/<shader>.glsl
 *   models/rife/<name>.param|.bin
 *
 * 解压完成后通过 Video2XNative.setResourceDir(filesDir/models) 告知 native 层。
 */
object ModelAssets {

    private const val ASSET_ROOT = "models"

    /** 资源版本号：模型集发生变化时递增，会触发重新解压 */
    // 2：内置 RIFE v4 系列补帧模型（此前仅内置超分模型，RIFE 需手动导入）
    private const val VERSION = "2"

    fun modelsDir(context: Context): File = File(context.filesDir, "models")

    /**
     * 确保模型已解压，返回资源根目录。
     * 应在后台线程调用（涉及大量 IO）。
     */
    fun ensureExtracted(context: Context): File {
        val dest = modelsDir(context)
        val stamp = File(dest, ".version")

        if (stamp.isFile && stamp.readText().trim() == VERSION) {
            return dest
        }

        dest.mkdirs()
        copyAssetDir(context, ASSET_ROOT, dest)
        stamp.writeText(VERSION)
        return dest
    }

    /** assets 是否包含模型（未拷贝资产时给出友好提示） */
    fun hasBundledModels(context: Context): Boolean {
        return try {
            context.assets.list(ASSET_ROOT)?.isNotEmpty() == true
        } catch (_: Exception) {
            false
        }
    }

    private fun copyAssetDir(context: Context, assetPath: String, dest: File) {
        val assets = context.assets
        val children = assets.list(assetPath)

        if (children != null && children.isNotEmpty()) {
            // 目录
            dest.mkdirs()
            for (child in children) {
                copyAssetDir(context, "$assetPath/$child", File(dest, child))
            }
        } else {
            // 文件
            dest.parentFile?.mkdirs()
            assets.open(assetPath).use { input ->
                dest.outputStream().use { output ->
                    input.copyTo(output, bufferSize = 1 shl 16)
                }
            }
        }
    }
}
