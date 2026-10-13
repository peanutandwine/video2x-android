package com.video2x.android

import android.content.Intent
import android.net.Uri
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Checkbox
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp

/**
 * 「发现新版本」弹窗 —— 自动检查（主界面）与手动检查（设置页）共用同一份实现。
 *
 * - 「更新」：打开浏览器跳转 Release 页面
 * - 「跳过」：仅关闭；若勾了「不再提示」，同时关闭设置里的「自动检查更新」
 */
@Composable
fun UpdateDialog(
    info: UpdateInfo,
    currentVersion: String,
    onDismiss: () -> Unit,
) {
    val context = LocalContext.current
    var dontRemindAgain by remember { mutableStateOf(false) }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("发现新版本 ${info.version}") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Text("当前版本：$currentVersion")
                Text("最新版本：${info.version}")
                Text(
                    "点击「更新」将打开浏览器，跳转到 GitHub Release 页面下载。",
                    style = MaterialTheme.typography.bodySmall,
                )
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Checkbox(
                        checked = dontRemindAgain,
                        onCheckedChange = { dontRemindAgain = it },
                    )
                    Text("不再提示", style = MaterialTheme.typography.bodySmall)
                }
            }
        },
        confirmButton = {
            TextButton(onClick = {
                onDismiss()
                runCatching {
                    context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(info.pageUrl)))
                }
            }) { Text("更新") }
        },
        dismissButton = {
            TextButton(onClick = {
                if (dontRemindAgain) AppSettings.setAutoCheckUpdate(context, false)
                onDismiss()
            }) { Text("跳过") }
        },
    )
}
