package com.video2x.android

import android.content.Context

/**
 * 应用级偏好设置（SharedPreferences）。
 *
 * 目前只有一项：启动时是否自动检查更新。
 * 统一走 applicationContext，避免意外持有 Activity 引用。
 */
object AppSettings {

    private const val PREF_NAME = "video2x_settings"
    private const val KEY_AUTO_CHECK_UPDATE = "auto_check_update"

    /** 默认开启：首次安装后启动即检查一次 */
    private const val DEFAULT_AUTO_CHECK_UPDATE = true

    private fun prefs(context: Context) =
        context.applicationContext.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE)

    fun autoCheckUpdate(context: Context): Boolean =
        prefs(context).getBoolean(KEY_AUTO_CHECK_UPDATE, DEFAULT_AUTO_CHECK_UPDATE)

    fun setAutoCheckUpdate(context: Context, enabled: Boolean) {
        prefs(context).edit().putBoolean(KEY_AUTO_CHECK_UPDATE, enabled).apply()
    }
}
