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
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
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

    var importing by remember { mutableStateOf(false) }
    var message by remember { mutableStateOf("") }

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
            "RIFE 模型全部约 474MB，为避免 APK 过大未随包发布，需要在这里单独导入。" +
                "超分模型（Real-ESRGAN / Real-CUGAN / Anime4K）已内置，无需导入。",
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

        // -------------------------------------------------------------- 关于
        Text("关于", style = MaterialTheme.typography.titleMedium)
        val versionName = remember {
            runCatching {
                context.packageManager.getPackageInfo(context.packageName, 0).versionName
            }.getOrNull() ?: "?"
        }
        Text("版本：$versionName", style = MaterialTheme.typography.bodySmall)
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
