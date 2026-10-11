package com.video2x.android

import android.content.Context
import android.net.Uri
import java.io.BufferedInputStream
import java.io.File
import java.util.Locale
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream

/**
 * 补帧（RIFE）模型管理。
 *
 * 超分模型（realesrgan / realcugan / libplacebo）仍随 APK 打包，由 [ModelAssets]
 * 解压；RIFE 模型全部共约 474MB、使用频率低，改为按需导入，避免把 APK 撑到 600MB+。
 *
 * 上游 interpolator_rife.cpp 查找的是**目录**：
 *     models/rife/<model_name>/
 * 目录内需有 flownet.param + flownet.bin；v3 及更早版本还需要
 * contextnet / fusionnet 各自的一对 param/bin。
 *
 * 导入包为 zip，两种层级都接受：
 *     rife/rife-v4.6/flownet.param      （本项目导出的格式）
 *     rife-v4.6/flownet.param           （直接把模型目录放进来）
 */
object RifeModels {

    /** 只有包含 flownet.param 的目录才被认作一个可用模型 */
    private const val REQUIRED_FILE = "flownet.param"

    /** RIFE 模型根目录：filesDir/models/rife */
    fun rifeDir(context: Context): File = File(ModelAssets.modelsDir(context), "rife")

    /**
     * 扫描已安装的 RIFE 模型，返回模型名（即目录名），按名称排序。
     * 目录扫描很快，可直接在 UI 线程调用。
     */
    fun installed(context: Context): List<String> {
        val dir = rifeDir(context)
        if (!dir.isDirectory) return emptyList()
        return dir.listFiles()
            ?.asSequence()
            ?.filter { it.isDirectory && File(it, REQUIRED_FILE).isFile }
            ?.map { it.name }
            ?.sorted()
            ?.toList()
            ?: emptyList()
    }

    /** 单个模型占用的字节数 */
    fun sizeOf(context: Context, model: String): Long {
        val dir = File(rifeDir(context), model)
        if (!dir.isDirectory) return 0L
        return dir.walkBottomUp().filter { it.isFile }.sumOf { it.length() }
    }

    /** 全部已安装模型的总体积 */
    fun totalSize(context: Context): Long =
        installed(context).sumOf { sizeOf(context, it) }

    /**
     * 从 zip 导入模型，返回被写入的模型名列表。
     *
     * 必须在后台线程调用（数据量可能到数百 MB）。
     * 逐条流式解压，不做整体缓存，避免大包撑爆内存。
     */
    fun importFromZip(context: Context, uri: Uri): List<String> {
        val destRoot = rifeDir(context)
        destRoot.mkdirs()
        val imported = linkedSetOf<String>()

        val raw = context.contentResolver.openInputStream(uri)
            ?: throw IllegalStateException("无法打开所选文件")

        raw.use { input ->
            ZipInputStream(BufferedInputStream(input, 1 shl 16)).use { zis ->
                while (true) {
                    val entry = zis.nextEntry ?: break
                    try {
                        handleEntry(zis, entry, destRoot, imported)
                    } finally {
                        zis.closeEntry()
                    }
                }
            }
        }
        return imported.toList()
    }

    /**
     * 处理单个 zip 条目，只接受 `<模型名>/<xxx>.{param,bin}` 两层结构。
     * 其余（目录项、macOS 元数据、隐藏文件、更深层级）一律跳过。
     */
    private fun handleEntry(
        zis: ZipInputStream,
        entry: ZipEntry,
        destRoot: File,
        imported: MutableSet<String>,
    ) {
        if (entry.isDirectory) return

        val path = entry.name.replace('\\', '/')
        // macOS 打包残留与隐藏文件
        if (path.startsWith("__MACOSX/")) return
        val leaf = path.substringAfterLast('/')
        if (leaf.isEmpty() || leaf.startsWith(".")) return

        // 允许可选的 "rife/" 前缀，便于直接使用上游仓库里的 models/ 目录打包
        val rel = if (path.startsWith("rife/")) path.removePrefix("rife/") else path
        val parts = rel.split('/')
        if (parts.size != 2) return

        val modelName = parts[0]
        val fileName = parts[1]

        // zip slip 防护
        if (modelName.isEmpty() || modelName == "." || modelName == "..") return
        if (modelName.contains("..") || fileName.contains("..")) return
        if (!fileName.endsWith(".param") && !fileName.endsWith(".bin")) return

        val outFile = File(File(destRoot, modelName), fileName)
        outFile.parentFile?.mkdirs()
        outFile.outputStream().use { zis.copyTo(it, 1 shl 16) }
        imported += modelName
    }

    /** 删除一个已安装的模型 */
    fun remove(context: Context, model: String): Boolean {
        val dir = File(rifeDir(context), model)
        if (!dir.isDirectory) return false
        return dir.deleteRecursively()
    }

    /** 人类可读的体积 */
    fun formatSize(bytes: Long): String = when {
        bytes >= 1L shl 30 -> String.format(Locale.US, "%.2f GB", bytes.toDouble() / (1L shl 30))
        bytes >= 1L shl 20 -> String.format(Locale.US, "%.1f MB", bytes.toDouble() / (1L shl 20))
        bytes >= 1L shl 10 -> String.format(Locale.US, "%.0f KB", bytes.toDouble() / (1L shl 10))
        else -> "$bytes B"
    }
}
