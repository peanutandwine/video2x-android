package com.video2x.android

import android.content.Context
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL

/** 一次「发现新版本」的检查结果 */
data class UpdateInfo(
    /** 去掉 v 前缀的版本号，例如 0.4.0 */
    val version: String,
    /** 原始 tag，例如 v0.4.0 */
    val tag: String,
    /** Release 页面地址，用于跳转浏览器下载 */
    val pageUrl: String,
)

/** 检查更新的三种结果 —— 手动检查需要区分「已最新」与「失败」，不能都吞成 null */
sealed interface UpdateCheckResult {

    /** 发现新版本 */
    data class Available(val info: UpdateInfo) : UpdateCheckResult

    /** 已是最新（本地版本 >= 远程最新版本） */
    data class UpToDate(val current: String) : UpdateCheckResult

    /** 检查失败：无网络、超时、限流、数据异常等 */
    data class Failed(val reason: String) : UpdateCheckResult
}

/**
 * 通过 GitHub Releases API 检查更新。
 *
 * 用 `/releases/latest` —— 它只返回「最新的一条正式版本」，自动排除
 * draft 与 prerelease，不需要自己拉全量列表再排序过滤。
 *
 * 未认证调用有 60 次/小时/IP 的限制；自用场景足够。
 */
object UpdateChecker {

    const val REPO_SLUG = "peanutandwine/video2x-android"
    const val REPO_URL = "https://github.com/$REPO_SLUG"

    private const val LATEST_API = "https://api.github.com/repos/$REPO_SLUG/releases/latest"

    /** 当前已安装版本（versionName） */
    fun currentVersion(context: Context): String =
        runCatching {
            context.packageManager
                .getPackageInfo(context.packageName, 0)
                .versionName
        }.getOrNull() ?: "0"

    /**
     * 检查是否有新版本。**必须在后台线程调用**（内部是阻塞网络请求）。
     *
     * 不抛异常 —— 所有失败都收敛为 [UpdateCheckResult.Failed]，
     * 便于 UI 直接展示原因。
     */
    fun check(context: Context): UpdateCheckResult {
        val current = currentVersion(context)

        val body = try {
            httpGet(LATEST_API)
        } catch (t: Throwable) {
            return UpdateCheckResult.Failed(friendlyReason(t))
        }

        val obj = runCatching { JSONObject(body) }.getOrNull()
            ?: return UpdateCheckResult.Failed("返回数据解析失败")

        val tag = obj.optString("tag_name").trim()
        val version = tag.removePrefix("v").removePrefix("V")
        if (version.isBlank()) return UpdateCheckResult.Failed("返回数据缺少版本号")

        return if (compareVersions(version, current) > 0) {
            UpdateCheckResult.Available(
                UpdateInfo(
                    version = version,
                    tag = tag,
                    pageUrl = obj.optString("html_url")
                        .ifBlank { "$REPO_URL/releases" },
                )
            )
        } else {
            UpdateCheckResult.UpToDate(current)
        }
    }

    /** 把底层异常翻译成给用户看的一句话 */
    private fun friendlyReason(t: Throwable): String = when {
        t is java.net.UnknownHostException -> "无法连接网络"
        t is java.net.SocketTimeoutException -> "连接超时"
        t is javax.net.ssl.SSLException -> "安全连接失败"
        else -> t.message?.takeIf { it.isNotBlank() } ?: "网络请求失败"
    }

    /**
     * 版本号比较：只比较数字段，忽略 `-beta` 之类的后缀。
     * 段数不同时缺失段按 0 处理（0.4 == 0.4.0）。
     *
     * @return a > b 返回正数，a < b 返回负数，相等返回 0
     */
    fun compareVersions(a: String, b: String): Int {
        val pa = a.substringBefore('-').split('.').map { it.toIntOrNull() ?: 0 }
        val pb = b.substringBefore('-').split('.').map { it.toIntOrNull() ?: 0 }
        for (i in 0 until maxOf(pa.size, pb.size)) {
            val d = pa.getOrElse(i) { 0 } - pb.getOrElse(i) { 0 }
            if (d != 0) return d
        }
        return 0
    }

    /** 极简 GET；失败抛异常，由 [check] 统一翻译 */
    private fun httpGet(url: String): String {
        var conn: HttpURLConnection? = null
        try {
            conn = (URL(url).openConnection() as HttpURLConnection).apply {
                requestMethod = "GET"
                connectTimeout = 8_000
                readTimeout = 8_000
                setRequestProperty("Accept", "application/vnd.github+json")
                setRequestProperty("User-Agent", "Video2X-Android")
            }
            val code = conn.responseCode
            if (code !in 200..299) error("服务器返回 HTTP $code")
            return conn.inputStream.bufferedReader().use { it.readText() }
        } finally {
            conn?.disconnect()
        }
    }
}
