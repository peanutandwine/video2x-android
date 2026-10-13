package com.video2x.android

import android.content.Intent
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.clickable
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * 设置页。
 *
 * 目前只做一件事：管理补帧（RIFE）模型 —— 超分模型仍随 APK 打包，
 * 只有 RIFE 因为体积（全部约 474MB）走按需导入。
 */
@Composable
fun SettingsScreen(
    installed: List<String>,
    onImported: () -> Unit,
    onBack: () -> Unit,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val uriHandler = LocalUriHandler.current

    var importing by remember { mutableStateOf(false) }
    var message by remember { mutableStateOf("") }

    // 自动检查更新开关；每次进入设置页时读取当前值
    var autoCheck by remember { mutableStateOf(AppSettings.autoCheckUpdate(context)) }

    // 手动检查更新的状态
    val currentVersion = remember { UpdateChecker.currentVersion(context) }
    var manualChecking by remember { mutableStateOf(false) }
    var manualMessage by remember { mutableStateOf("") }
    var manualUpdate by remember { mutableStateOf<UpdateInfo?>(null) }

    // 选 zip 同样用 GET_CONTENT + */*，这样第三方文件管理器也能出现在列表里
    val zipPicker = rememberLauncherForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode == android.app.Activity.RESULT_OK) {
            val uri: Uri? = result.data?.data
            if (uri != null) {
                importing = true
                message = "正在导入，请勿退出…（大包可能需要一两分钟）"
                scope.launch {
                    val outcome = withContext(Dispatchers.IO) {
                        runCatching { RifeModels.importFromZip(context, uri) }
                    }
                    importing = false
                    outcome.onSuccess { names ->
                        message = if (names.isEmpty()) {
                            "包里没找到可用模型 —— 需要 rife/<模型名>/flownet.param 这样的结构"
                        } else {
                            "已导入 ${names.size} 个模型：" + names.joinToString("、")
                        }
                        onImported()
                    }.onFailure {
                        message = "导入失败：${it.message ?: it}"
                    }
                }
            }
        }
    }

    fun pickZip() {
        val inner = Intent(Intent.ACTION_GET_CONTENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
        }
        try {
            zipPicker.launch(Intent.createChooser(inner, "选择模型包（zip）"))
        } catch (_: Exception) {
            try {
                zipPicker.launch(
                    Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                        type = "*/*"
                        addCategory(Intent.CATEGORY_OPENABLE)
                    }
                )
            } catch (_: Exception) {
                message = "没有可用的文件选择器"
            }
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp)
            .verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("设置", style = MaterialTheme.typography.headlineSmall, modifier = Modifier.weight(1f))
            OutlinedButton(onClick = onBack) { Text("返回") }
        }

        HorizontalDivider()

        // ---------------------------------------------------------- 补帧模型
        Text("补帧模型（RIFE）", style = MaterialTheme.typography.titleMedium)
        Text(
            "RIFE 补帧模型（v4 系列 5 个）已随 APK 内置，开箱即用。" +
                "此处可额外导入其他版本的 RIFE 模型（上游全部版本合计约 474MB）。" +
                "超分模型（Real-ESRGAN / Real-CUGAN / Anime4K）同样已内置。",
            style = MaterialTheme.typography.bodySmall,
        )

        if (installed.isEmpty()) {
            Text(
                "尚未导入任何模型，插帧功能暂不可用。",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.error,
            )
        } else {
            Text(
                "已安装 ${installed.size} 个模型，占用 ${RifeModels.formatSize(RifeModels.totalSize(context))}",
                style = MaterialTheme.typography.bodySmall,
            )
            installed.forEach { model ->
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        "· $model",
                        style = MaterialTheme.typography.bodySmall,
                        modifier = Modifier.weight(1f),
                    )
                    Text(
                        RifeModels.formatSize(RifeModels.sizeOf(context, model)),
                        style = MaterialTheme.typography.bodySmall,
                    )
                    Spacer(Modifier.width(8.dp))
                    TextButton(
                        onClick = {
                            if (RifeModels.remove(context, model)) {
                                message = "已删除 $model"
                                onImported()
                            } else {
                                message = "删除失败：$model"
                            }
                        },
                        enabled = !importing,
                    ) {
                        Text("删除")
                    }
                }
            }
        }

        Row(verticalAlignment = Alignment.CenterVertically) {
            Button(onClick = { pickZip() }, enabled = !importing) {
                Text(if (importing) "导入中…" else "导入模型包")
            }
        }

        if (importing) {
            LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
        }

        if (message.isNotEmpty()) {
            Text(message, style = MaterialTheme.typography.bodySmall)
        }

        Text(
            "模型包为 zip，内部结构应为 rife/<模型名>/flownet.param 与 flownet.bin；" +
                "也可以直接使用上游仓库 models/ 目录的原始结构（带不带 rife/ 前缀都支持）。",
            style = MaterialTheme.typography.bodySmall,
        )

        HorizontalDivider()

        // ---------------------------------------------------------- 更新
        Text("更新", style = MaterialTheme.typography.titleMedium)
        Text("当前版本：$currentVersion", style = MaterialTheme.typography.bodySmall)

        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(modifier = Modifier.weight(1f)) {
                Text("自动检查更新", style = MaterialTheme.typography.bodyLarge)
                // 仓库地址：点击可直接在浏览器打开
                Text(
                    UpdateChecker.REPO_URL,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.primary,
                    modifier = Modifier.clickable {
                        runCatching { uriHandler.openUri(UpdateChecker.REPO_URL) }
                    },
                )
            }
            Spacer(Modifier.width(8.dp))
            Switch(
                checked = autoCheck,
                onCheckedChange = {
                    autoCheck = it
                    AppSettings.setAutoCheckUpdate(context, it)
                },
            )
        }

        Text(
            "开启后，启动应用时会自动向 GitHub 查询最新版本；发现新版本会弹窗提示，" +
                "点「更新」跳转 Release 页面下载。在更新弹窗中勾选「不再提示」再点" +
                "「跳过」，同样会关闭此开关。",
            style = MaterialTheme.typography.bodySmall,
        )

        // 手动检查：不受自动检查开关影响，随时可点
        Row(verticalAlignment = Alignment.CenterVertically) {
            Button(
                onClick = {
                    manualChecking = true
                    manualMessage = ""
                    scope.launch {
                        val result = withContext(Dispatchers.IO) { UpdateChecker.check(context) }
                        manualChecking = false
                        when (result) {
                            is UpdateCheckResult.Available -> manualUpdate = result.info
                            is UpdateCheckResult.UpToDate ->
                                manualMessage = "已是最新版本（${result.current}）"
                            is UpdateCheckResult.Failed ->
                                manualMessage = "检查失败：${result.reason}"
                        }
                    }
                },
                enabled = !manualChecking,
            ) {
                Text(if (manualChecking) "检查中…" else "检查更新")
            }
            if (manualChecking) {
                Spacer(Modifier.width(12.dp))
                CircularProgressIndicator(
                    modifier = Modifier.size(18.dp),
                    strokeWidth = 2.dp,
                )
            }
        }

        if (manualMessage.isNotEmpty()) {
            Text(manualMessage, style = MaterialTheme.typography.bodySmall)
        }

        // 手动检查发现新版本 —— 与自动检查共用同一个弹窗
        manualUpdate?.let { info ->
            UpdateDialog(
                info = info,
                currentVersion = currentVersion,
                onDismiss = {
                    manualUpdate = null
                    // 「不再提示」可能刚把开关改动过，同步回 UI
                    autoCheck = AppSettings.autoCheckUpdate(context)
                },
            )
        }

        HorizontalDivider()

        // -------------------------------------------------------------- 关于
        Text("关于", style = MaterialTheme.typography.titleMedium)
        Text("版本：$currentVersion", style = MaterialTheme.typography.bodySmall)
        Text(
            "模型目录：${ModelAssets.modelsDir(context).absolutePath}",
            style = MaterialTheme.typography.bodySmall,
        )
        Text(
            "输出目录：${FileBridge.OUTPUT_DIR}",
            style = MaterialTheme.typography.bodySmall,
        )
    }
}
